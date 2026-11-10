// Micro-ESPCN 2x luma super-resolution (media::video::NeuralModel, see NeuralUpscale.hpp for the exact definition,
// which NeuralUpscaleReference implements on the CPU). One 16x16 group per 16x16 source tile:
//   1. the 20x20 luma neighbourhood (clamped to the crop rectangle) is staged in groupshared memory;
//   2. conv1 (3x3, 1 -> 16, leaky ReLU) is evaluated on the 18x18 ring-extended tile into groupshared memory, each
//      value computed once and shared by the nine conv2 taps that read it;
//   3. each thread evaluates conv2 (3x3, 16 -> 8, leaky ReLU) and the 1x1 sub-pixel layer (8 -> 4) for its own
//      source sample, adds the residuals to the bilinear sub-pixel base, clamps each to the range of the four samples
//      that base interpolates (deringing: no halos), and writes its 2x2 output quad.
// Flat tiles: when the whole 20x20 neighbourhood spans less than kFlatRange (1.5/255), the group skips both
// convolutions and writes the bilinear base (residual 0), a uniform branch, so the work is really skipped (letterbox
// bars, skies, backgrounds). Part of the model definition: the CPU reference applies the same rule.
// Temporal tile reuse (docs/neural.md): every tile keeps, in `history`, the 20x20 source codes it was last evaluated from
// (its anchor: 8-bit codes for NV12, 16-bit for P010, two per word, 800 bytes a tile). A group whose neighbourhood matches
// its anchor (bit 1 of `temporal`: the caller vouches that the output still holds that evaluation, for the same crop,
// format and weights) returns without evaluating or writing anything, and keeps its anchor. Matches:
// - exact (bit 2 clear): every code equal. The network is a pure function of the neighbourhood, so the output tile is
//   already what it would write, bit for bit;
// - tolerant (bit 2 set): no code more than one 8-bit step from the anchor and the mean difference at most a quarter of
//   one: compression noise on a still background. The output stays the anchor's evaluation, and since a reused tile
//   never moves its anchor, the differences tolerated are always against the picture actually evaluated: they cannot
//   add up from picture to picture.
// An evaluated tile records its neighbourhood as its new anchor (bit 0). Each group adds one to the reused or evaluated
// counter at `statsOffset`. Each group reads and writes only its own anchor: no ordering between groups is needed.
// Weights: 1356 floats in a root constant buffer (uniform across the group: broadcast reads). Groupshared use:
// 400 + 16 x 324 floats = 22.3 KB. Layout [channel][position] keeps a warp's reads on consecutive addresses.
// fp32 throughout: min16float measured 1.8x slower on the reference GPU (Intel UHD 630, fxc SM 5.0).
// Direct convolution on purpose: every constant-buffer offset is a compile-time constant (the reference GPU pays up
// to 18x for a dynamic one, even a uniform one), and conv2 keeps 8 accumulators per thread. Five Winograd F(2x2, 3x3)
// formulations, register blocking and native fp16 were all measured slower on the reference GPU (docs/neural.md).
Texture2D<float> source : register(t0);     // Luma plane (NV12 R8 or P010 R16 view), UNORM.
RWTexture2D<float> upscaled : register(u0); // 2x output (R16F or R32F), origin at the crop's top-left.
RWByteAddressBuffer history : register(u1); // Anchors, tile after tile (row of `tilesPerRow`), then the counters (root UAV).
cbuffer Region : register(b0) {
    uint left, top, right, bottom; // Crop rectangle in the plane (right/bottom exclusive).
    uint codeScale;                // 255 (R8 luma view) or 65535 (R16): the plane's code of a sample is round(v * codeScale).
    uint temporal;                 // Bit 0: anchors kept; bit 1: reuse tiles that match their anchor; bit 2: tolerant match.
    uint tilesPerRow, statsOffset; // Anchors per row of tiles; byte offset of the reused and evaluated tile counters.
};
cbuffer Weights : register(b1) { float4 packed[339]; };
static const uint T = 16, In = T + 4, F = T + 2;
static const uint C1W = 0, C1B = 144, C2W = 160, C2B = 1312, C3W = 1320, C3B = 1352;
groupshared float tile[In * In];
groupshared float features[16 * F * F];
groupshared uint tileLow, tileHigh; // Range of the neighbourhood (asuint order equals float order for values >= 0).
groupshared uint diffMax, diffSum;  // Largest and summed code difference of the neighbourhood from the tile's anchor.
static const uint kAnchorWords = In * In / 2;
static const float kFlatRange = 1.5 / 255.0;
float Weight(uint i) { return packed[i >> 2][i & 3]; }
float LeakyRelu(float x) { return x > 0 ? x : 0.1 * x; }
// A sample's code: UNORM to float is exact to well under half a step, so equal codes are equal samples and back.
uint Code(float v) { return uint(v * float(codeScale) + 0.5); }
uint AnchorCode(uint anchor, uint i) { const uint word = history.Load(anchor + (i >> 1) * 4); return (i & 1) ? word >> 16 : word & 0xffff; }
[numthreads(16, 16, 1)]
void CS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint index = thread.y * T + thread.x;
    const int2 low = int2(left, top), high = int2(right, bottom) - 1;
    const int2 origin = low + int2(group.xy * T) - 2; // Plane position of tile[0].
    const uint anchor = (group.y * tilesPerRow + group.x) * kAnchorWords * 4; // This tile's anchor, in bytes.
    if (index == 0) { tileLow = 0x7f7fffff; tileHigh = 0; diffMax = 0; diffSum = 0; }
    GroupMemoryBarrierWithGroupSync();
    float lo = 1e30, hi = 0; uint most = 0, total = 0;
    for (uint i = index; i < In * In; i += T * T) {
        const float v = source.Load(int3(clamp(origin + int2(i % In, i / In), low, high), 0));
        tile[i] = v; lo = min(lo, v); hi = max(hi, v);
        [branch] if (temporal & 2) { const uint c = Code(v), a = AnchorCode(anchor, i), d = c > a ? c - a : a - c; most = max(most, d); total += d; }
    }
    InterlockedMin(tileLow, asuint(lo)); InterlockedMax(tileHigh, asuint(hi));
    if (most) { InterlockedMax(diffMax, most); InterlockedAdd(diffSum, total); }
    GroupMemoryBarrierWithGroupSync();
    // The neighbourhood matches the anchor: the output tile already holds the anchor's evaluation. Uniform per group; the
    // barrier below stays in uniform flow and the group leaves after it. Tolerant: one 8-bit step at most, a quarter of
    // one on average (the sum over the 400 codes at most 100 steps).
    const uint step = codeScale == 255 ? 1u : 257u;
    const bool reuse = (temporal & 2) != 0 && (diffMax == 0 || ((temporal & 4) != 0 && diffMax <= step && diffSum <= 100u * step));
    // Evaluated: this neighbourhood is the tile's new anchor (two codes a word, the first 200 threads).
    [branch] if ((temporal & 1) && !reuse && index < kAnchorWords) history.Store(anchor + index * 4, Code(tile[2 * index]) | Code(tile[2 * index + 1]) << 16);
    if ((temporal & 1) && index == 0) history.InterlockedAdd(statsOffset + (reuse ? 0u : 4u), 1u);
    const int2 position = int2(group.xy * T + thread.xy); // Relative to the crop.
    const bool flat = asfloat(tileHigh) - asfloat(tileLow) < kFlatRange; // Same value in every thread of the group.
    if (!flat && !reuse) for (uint p = index; p < F * F; p += T * T) {
        const uint fx = p % F, fy = p / F;
        float taps[9];
        [unroll] for (uint k = 0; k < 9; ++k) taps[k] = tile[(fy + k / 3) * In + fx + k % 3];
        [unroll] for (uint c = 0; c < 16; ++c) {
            float sum = Weight(C1B + c);
            [unroll] for (uint k2 = 0; k2 < 9; ++k2) sum += Weight(C1W + c * 9 + k2) * taps[k2];
            features[c * F * F + p] = LeakyRelu(sum);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (reuse || any(position > high - low)) return;
    float f2[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    [branch] if (!flat) {
        [unroll] for (uint o = 0; o < 8; ++o) f2[o] = Weight(C2B + o);
        [unroll] for (uint c = 0; c < 16; ++c) [unroll] for (uint k = 0; k < 9; ++k) {
            const float a = features[c * F * F + (thread.y + k / 3) * F + thread.x + k % 3];
            [unroll] for (uint o2 = 0; o2 < 8; ++o2) f2[o2] += Weight(C2W + (o2 * 16 + c) * 9 + k) * a;
        }
        [unroll] for (uint o3 = 0; o3 < 8; ++o3) f2[o3] = LeakyRelu(f2[o3]);
    }
    // Bilinear base at the four sub-pixels (a quarter pixel towards each neighbour: weights 9/16, 3/16, 3/16, 1/16).
    const int centre = int((thread.y + 2) * In + thread.x + 2);
    const float c0 = tile[centre];
    [unroll] for (uint s = 0; s < 4; ++s) {
        const uint sx = s & 1, sy = s >> 1;
        const int dx = sx ? 1 : -1, dy = sy ? (int)In : -(int)In;
        const float h = tile[centre + dx], v = tile[centre + dy], d = tile[centre + dx + dy];
        const float base = 0.5625 * c0 + 0.1875 * h + 0.1875 * v + 0.0625 * d;
        float residual = flat ? 0.0 : Weight(C3B + s);
        [unroll] for (uint c3 = 0; c3 < 8; ++c3) residual += Weight(C3W + s * 8 + c3) * f2[c3]; // f2 = 0 when flat.
        upscaled[uint2(position) * 2 + uint2(sx, sy)] = clamp(base + residual, min(min(c0, h), min(v, d)), max(max(c0, h), max(v, d)));
    }
}
