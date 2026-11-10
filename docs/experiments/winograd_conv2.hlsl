// EXPERIMENT, NOT BUILT: Winograd F(2x2, 3x3) conv2 for NeuralSuperRes.hlsl (variant W3 in docs/neural.md).
// Verified against NeuralUpscaleReference (<= 3e-7, R32F) but 3.5x slower than the direct kernel on the Intel UHD 630
// (72.8 ms against 20.9 ms at 1080p -> 4K). Kept for GPUs where the balance may differ (larger register files).
// Weight packing it expects (2252 floats, 563 float4): conv1 weights at 0, conv1 biases 144, U = G g G^T laid out
// [channel][xi][map] at 160 (index 160 + (c * 16 + xi) * 8 + o), conv2 biases 2208, conv3 weights 2216, conv3
// biases 2248; G = {{1,0,0},{.5,.5,.5},{.5,-.5,.5},{0,0,1}}, g the 3x3 correlation kernel (row = dy).

// Micro-ESPCN 2x luma super-resolution (media::video::NeuralModel, see NeuralUpscale.hpp for the exact definition,
// which NeuralUpscaleReference implements on the CPU). One group of 128 threads per 16x16 source tile:
//   1. the 20x20 luma neighbourhood (clamped to the crop rectangle) is staged in groupshared memory with its range;
//   2. conv1 (3x3, 1 -> 16, leaky ReLU) is evaluated on the 18x18 ring-extended tile into groupshared memory;
//   3. conv2 (3x3, 16 -> 8) with Winograd F(2x2, 3x3): thread = (2x2 output block, quad of four output maps). Per input
//      channel it transforms its 4x4 feature window, V = B^T d B (additions only), and accumulates M[xi] += U[xi] V[xi]
//      for its four maps, U = G g G^T transformed once on the CPU (NeuralUpscalePass::Create): 16 multiply-adds per
//      map instead of 36. The quad is selected by a wave-uniform branch, so every weight is read at a compile-time
//      constant-buffer offset (a dynamic offset, even a uniform one, costs up to 18x on the reference GPU).
//   4. output transform Y = A^T M A in registers, bias and leaky ReLU, then this quad's share of the 1x1 sub-pixel
//      layer (8 -> 4) for the block's four samples; the two quads' shares meet in groupshared memory;
//   5. bilinear base, deringing clamp (range of the four samples each sub-pixel interpolates), 2x2 output quads.
// Flat tiles: when the whole 20x20 neighbourhood spans less than kFlatRange (1.5/255), the group skips the network and
// writes the bilinear base (residual 0). Part of the model definition: the CPU reference applies the same rule.
// Groupshared use: 400 + 16 x 324 + 2 x 256 x 4 floats = 30.3 KB.
Texture2D<float> source : register(t0);     // Luma plane (NV12 R8 or P010 R16 view), UNORM.
RWTexture2D<float> upscaled : register(u0); // 2x output (R16F or R32F), origin at the crop's top-left.
cbuffer Region : register(b0) { uint left, top, right, bottom; }; // Crop rectangle in the plane (right/bottom exclusive).
cbuffer Weights : register(b1) { float4 packed[563]; };
static const uint T = 16, In = T + 4, F = T + 2, Threads = 128;
// Offsets in floats; must match NeuralUpscalePass.cpp. U2: [channel][xi][map] (two float4 per channel and xi).
static const uint C1W = 0, C1B = 144, U2 = 160, C2B = 2208, C3W = 2216, C3B = 2248;
groupshared float tile[In * In];
groupshared float features[16 * F * F];
groupshared float4 partials[2 * T * T]; // [quad][sample]: this quad's share of the four sub-pixel residuals.
groupshared uint tileLow, tileHigh; // Range of the neighbourhood (asuint order equals float order for values >= 0).
static const float kFlatRange = 1.5 / 255.0;
float Weight(uint i) { return packed[i >> 2][i & 3]; }
float LeakyRelu(float x) { return x > 0 ? x : 0.1 * x; }
float4 LeakyRelu4(float4 x) { return max(x, 0.1 * x); }
// Winograd conv2 for maps quad * 4 .. quad * 4 + 3 of the block whose feature window starts at `window`; returns the
// block's partial residuals (sample (r, s) in partial[r * 2 + s], one sub-pixel per component). `quad` must be a
// literal so that every constant-buffer offset below folds to a constant.
void Block(const uint quad, uint window, uint first) {
    float4 m[16];
    [unroll] for (uint i = 0; i < 16; ++i) m[i] = 0;
    [unroll] for (uint c = 0; c < 16; ++c) {
        float d[4][4];
        [unroll] for (uint y = 0; y < 4; ++y) [unroll] for (uint x = 0; x < 4; ++x) d[y][x] = features[c * F * F + window + y * F + x];
        float t[4][4]; // B^T d: rows d0 - d2, d1 + d2, d2 - d1, d1 - d3.
        [unroll] for (uint x2 = 0; x2 < 4; ++x2) {
            t[0][x2] = d[0][x2] - d[2][x2]; t[1][x2] = d[1][x2] + d[2][x2]; t[2][x2] = d[2][x2] - d[1][x2]; t[3][x2] = d[1][x2] - d[3][x2];
        }
        [unroll] for (uint r = 0; r < 4; ++r) {
            const float v[4] = {t[r][0] - t[r][2], t[r][1] + t[r][2], t[r][2] - t[r][1], t[r][1] - t[r][3]}; // (B^T d) B
            [unroll] for (uint j = 0; j < 4; ++j) m[r * 4 + j] += packed[(U2 + (c * 16 + r * 4 + j) * 8 + quad * 4) >> 2] * v[j];
        }
    }
    // Y = A^T M A, A^T rows (1, 1, 1, 0) and (0, 1, -1, -1); four maps per float4.
    float4 row[2][4];
    [unroll] for (uint j2 = 0; j2 < 4; ++j2) { row[0][j2] = m[j2] + m[4 + j2] + m[8 + j2]; row[1][j2] = m[4 + j2] - m[8 + j2] - m[12 + j2]; }
    const float4 bias = packed[(C2B + quad * 4) >> 2];
    float4 w3[4]; // Sub-pixel s: conv3 weights of this quad's maps.
    [unroll] for (uint s = 0; s < 4; ++s) w3[s] = packed[(C3W + s * 8 + quad * 4) >> 2];
    [unroll] for (uint r2 = 0; r2 < 2; ++r2) {
        const float4 y0 = LeakyRelu4(row[r2][0] + row[r2][1] + row[r2][2] + bias), y1 = LeakyRelu4(row[r2][1] - row[r2][2] - row[r2][3] + bias);
        partials[quad * T * T + first + r2 * T] = float4(dot(w3[0], y0), dot(w3[1], y0), dot(w3[2], y0), dot(w3[3], y0));
        partials[quad * T * T + first + r2 * T + 1] = float4(dot(w3[0], y1), dot(w3[1], y1), dot(w3[2], y1), dot(w3[3], y1));
    }
}
[numthreads(Threads, 1, 1)]
void CS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint index = thread.x;
    const int2 low = int2(left, top), high = int2(right, bottom) - 1;
    const int2 origin = low + int2(group.xy * T) - 2; // Plane position of tile[0].
    if (index == 0) { tileLow = 0x7f7fffff; tileHigh = 0; }
    GroupMemoryBarrierWithGroupSync();
    float lo = 1e30, hi = 0;
    for (uint i = index; i < In * In; i += Threads) {
        const float v = source.Load(int3(clamp(origin + int2(i % In, i / In), low, high), 0));
        tile[i] = v; lo = min(lo, v); hi = max(hi, v);
    }
    InterlockedMin(tileLow, asuint(lo)); InterlockedMax(tileHigh, asuint(hi));
    GroupMemoryBarrierWithGroupSync();
    const bool flat = asfloat(tileHigh) - asfloat(tileLow) < kFlatRange; // Same value in every thread of the group.
    if (!flat) for (uint p = index; p < F * F; p += Threads) {
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
    const uint block = index & 63, quad = index >> 6, bx = block & 7, by = block >> 3;
    [branch] if (!flat) {
        const uint window = by * 2 * F + bx * 2, first = by * 2 * T + bx * 2; // Feature window, block's first sample.
        [branch] if (quad == 0) Block(0, window, first); else Block(1, window, first); // Uniform per 64 threads.
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint half = 0; half < 2; ++half) {
        const uint sample = half * Threads + index, lx = sample & 15, ly = sample >> 4;
        const int2 position = int2(group.xy * T + uint2(lx, ly)); // Relative to the crop.
        if (any(position > high - low)) continue;
        // Bilinear base at the four sub-pixels (a quarter pixel towards each neighbour: weights 9/16, 3/16, 3/16, 1/16).
        const int centre = int((ly + 2) * In + lx + 2);
        const float c0 = tile[centre];
        [unroll] for (uint s = 0; s < 4; ++s) {
            const uint sx = s & 1, sy = s >> 1;
            const int dx = sx ? 1 : -1, dy = sy ? (int)In : -(int)In;
            const float h = tile[centre + dx], v = tile[centre + dy], d = tile[centre + dx + dy];
            const float base = 0.5625 * c0 + 0.1875 * h + 0.1875 * v + 0.0625 * d;
            // Summation order: conv3 bias, then maps 0-3, then maps 4-7 (as the CPU reference within rounding).
            const float residual = flat ? 0.0 : Weight(C3B + s) + partials[sample][s] + partials[T * T + sample][s];
            upscaled[uint2(position) * 2 + uint2(sx, sy)] = clamp(base + residual, min(min(c0, h), min(v, d)), max(max(c0, h), max(v, d)));
        }
    }
}
