// HDR (PQ / HLG, BT.2020) to SDR BT.709 display conversion. Dedicated compute pass:
//   Y'CbCr (BT.2020 NCL) -> R'G'B' -> linear display light (PQ EOTF, or HLG inverse OETF + BT.2100 OOTF)
//   -> BT.2390 EETF (Hermite roll-off in the PQ domain) on max(R,G,B), hue preserved by ratio scaling
//   -> BT.2020 to BT.709 primaries with luminance-preserving desaturation -> display encoding (gamma 2.4).
// Source peak: fixed, or dynamic: each picture's own peak is detected on the GPU before it is converted (Detect), and
// folded into the smoothed peak in presentation time (Peak), so a picture is converted with an exposure that includes
// itself; a new epoch (seek, start) and a scene cut take the picture's own peak at once.
// Scaling, reconstruction and sharpening: Reconstruct.hlsli (shared with the SDR pass).
#include "Reconstruct.hlsli"
RWByteAddressBuffer peak : register(u1); // [0] picture max (PQ * 65535), [4] smoothed peak PQ (float), [8] pictures,
                                         // [12] scene cuts, [16] epoch starts; Stats: [32] luma sum, [36] samples, [40]
                                         // brightest, [44] moving blocks, [48, 64) the picture's extents (Matte), [64, 1088)
                                         // the horizon's histograms (Orientation).
// PqToNits, NitsToPq, the BT.2087 matrices: Reconstruct.hlsli (the HDR output's encoding, API 14, needs them in the SDR pass too).
float HlgToScene(float e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
}
// BT.2390 EETF with black level 0: maps [0, sourcePq] onto [0, targetPq] in the PQ domain.
float Eetf(float pq, float sourcePq, float targetPq) {
    float e1 = pq / sourcePq, maxLum = targetPq / sourcePq;
    if (maxLum >= 1.0) return pq; // The whole source fits the target: no compression.
    float ks = 1.5 * maxLum - 0.5;
    if (e1 < ks) return e1 * sourcePq;
    float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
    float e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * maxLum;
    return min(e2, maxLum) * sourcePq;
}
float3 DecodeRgb(float y, float2 c) {
    float code = tenBit ? 64.0 / 65535.0 : 1.0 / 255.0, depth = tenBit ? 4.0 : 1.0;
    c -= 128.0 * depth * code;
    if (fullRange) { float span = (tenBit ? 1023.0 : 255.0) * code; y /= span; c /= span; }
    else { y = (y - 16.0 * depth * code) / (219.0 * depth * code); c /= 224.0 * depth * code; }
    const float kr = 0.2627, kb = 0.0593; // BT.2020 non-constant luminance.
    float red = y + 2 * (1 - kr) * c.y, blue = y + 2 * (1 - kb) * c.x;
    float green = (y - kr * red - kb * blue) / (1 - kr - kb);
    return saturate(float3(red, green, blue));
}
float3 Convert(float y, float2 c) {
    float3 e = DecodeRgb(y, c);
    float3 nits;
    if (transfer == 18) {
        // HLG: scene light, then the BT.2100 OOTF for a 1000 cd/m2 display (system gamma 1.2).
        float3 scene = float3(HlgToScene(e.r), HlgToScene(e.g), HlgToScene(e.b));
        float ys = dot(float3(0.2627, 0.6780, 0.0593), scene);
        nits = 1000.0 * pow(max(ys, 1e-6), 0.2) * scene;
    } else nits = float3(PqToNits(e.r), PqToNits(e.g), PqToNits(e.b));
    float brightest = max(nits.r, max(nits.g, nits.b));
    float brightestPq = NitsToPq(brightest);
    float sourcePq = peakMode == 0 ? asfloat(peak.Load(4)) : NitsToPq(asfloat(fixedPeakBits));
    float target = asfloat(targetBits);
    float mapped = PqToNits(Eetf(brightestPq, max(sourcePq, 1e-4), NitsToPq(target)));
    float3 light = brightest > 0 ? nits * (mapped / brightest) / target : float3(0, 0, 0);
    // BT.2020 -> BT.709 (ITU-R BT.2087); out-of-gamut colours desaturate toward their luminance.
    light = float3(dot(float3( 1.660491, -0.587641, -0.072850), light),
                   dot(float3(-0.124550,  1.132900, -0.008349), light),
                   dot(float3(-0.018151, -0.100579,  1.118730), light));
    float lum = dot(float3(0.2126, 0.7152, 0.0722), light);
    float lowest = min(light.r, min(light.g, light.b));
    if (lowest < 0 && lum > 0) light = lum + (light - lum) * (lum / (lum - lowest));
    // API 14, an HDR texture: kept in BT.2020 as PQ, not tone-mapped (a PQ picture's own codes).
    return HdrOutput() ? NitsToPq3(nits) : pow(saturate(light), 1.0 / 2.4);
}
// The brightest max(R,G,B) of a picture, in PQ, from its source samples: one thread per 4:2:0 chroma sample of the crop,
// with the four luma samples it covers (resolution independent: the picture's peak, not the output's). PQ is monotonic,
// so for PQ the maximum of the three PQ values is the PQ of the brightest channel. One atomic per group.
groupshared uint groupPeak;
[numthreads(16, 16, 1)]
void Detect(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) groupPeak = 0;
    GroupMemoryBarrierWithGroupSync();
    const int2 site = ChromaLow() + int2(id.xy);
    if (all(site <= ChromaHigh())) {
        const float2 c = chroma.Load(int3(site, 0));
        float brightestPq = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            const float3 e = DecodeRgb(LumaAt(site * 2 + int2(k & 1, k >> 1)), c); // LumaAt clamps to the crop.
            float pq;
            if (transfer == 18) {
                const float3 scene = float3(HlgToScene(e.r), HlgToScene(e.g), HlgToScene(e.b));
                const float3 nits = 1000.0 * pow(max(dot(float3(0.2627, 0.6780, 0.0593), scene), 1e-6), 0.2) * scene;
                pq = NitsToPq(max(nits.r, max(nits.g, nits.b)));
            } else pq = max(e.r, max(e.g, e.b));
            brightestPq = max(brightestPq, pq);
        }
        InterlockedMax(groupPeak, (uint)(saturate(brightestPq) * 65535.0 + 0.5));
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) peak.InterlockedMax(0, groupPeak);
}
// Scene statistics of every picture, SDR or HDR (docs/chapters.md, "Live cuts"): one luma code value per 8x8 block of
// the crop, in tenths of a percent of full scale (0 to 1023), added to running totals the CPU differences per picture
// (they wrap; one picture's share never does). One atomic pair per group of 64 samples.
// The same samples give the frame signature (docs/sdk.md, "Frame signatures") at no extra read: the picture's brightest
// sample ([40], a maximum, cleared after the copy by StatsReset) and how many blocks moved ([44]): each block keeps its last
// sample (u3, one word per block, row of ceil(crop width / 8), the first kMaxBlocks), and a block moved when its sample
// changed by more than kMoved codes since the picture before it.
RWByteAddressBuffer blocks : register(u3);
static const uint kMaxBlocks = 262144, kMoved = 12; // A 4096 x 4096 crop; 1.2 % of full scale.
groupshared uint groupSum, groupCount, groupMax, groupMoved;
// Auto-leveling (docs/picture.md, "Auto-leveling the horizon", 2026-10-29; filterFlags bit 5, kHorizon): the same threads
// find blocks' dominant edge orientations and add them to a histogram the CPU reads with the statistics (no pass of its own,
// no picture leaves the GPU). About kHorizonAcross blocks across the crop at any size (8 texels a side up to 640 wide, 16 at
// 1280, 48 at 3840: a multiple of 8), one per thread of the grid's top-left corner (the active threads contiguous: whole
// waves work or none): a 4K picture costs what a 720p one does and is measured as densely across the scene (8-texel blocks
// everywhere cost 0.42 ms more a 4K picture on the UHD 630 for no better tilt; 16-texel blocks at 640 wide had a quarter of
// the blocks and read -6.0 degrees as -5.68). The block's structure tensor from Scharr gradients
// at the 8 x 8 texels at its centre; it counts when its edge is strong (kHorizonEnergy: about 9 codes across it) and
// straight (coherence at least kHorizonCoherence: one direction; texture's blocks have several). Its gradient direction
// modulo 90 degrees is the tilt of its line, clockwise on the picture (y down): a near-horizontal line (the gradient within
// 45 degrees of vertical) or a near-vertical one, each class its own histogram of kHorizonBins bins of 0.5 degree over
// [-16, 16), each bin two words at kHorizonBase: the weight (16 x coherence squared) and the weight times where in the bin
// the angle fell (256ths), for a sub-bin centroid on the CPU (video::HorizonLevel, whose Accumulate is this on the CPU).
// Blocks whose texels leave the crop are not counted (clamped loads would make edges of the border).
// Measured on an antialiased half-plane (2026-10-29), the blocks' median tilt against the truth: central differences +20 %
// and 16 % of the blocks at exactly 0 degrees (a sharp edge between two sample columns reads as level); point or 2x2-mean
// samples two texels apart, worse; Sobel -10 %; Scharr within 0.15 degree from 4.5 to 14.
static const uint kHorizon = 32, kHorizonBase = 64, kHorizonBins = 64;
static const float kHorizonEnergy = 0.005, kHorizonCoherence = 0.85;
static const uint kHorizonAcross = 80;
// Black bars (docs/picture.md, "Black bars", 2026-10-30; video::Matte): a sample brighter than black by more than kMatteLift
// of full scale is picture. Black is the range's: 16 (64 in 10-bit) codes limited, 0 full.
static const float kMatteLift = 0.03, kBarAxis = 1.0;
static const uint kMatte = 64, kMatteLines = 32, kMatteBase = 48; // filterFlags bit 6.
float Dark() {
    const float code = tenBit ? 64.0 / 65535.0 : 1.0 / 255.0, depth = tenBit ? 4.0 : 1.0;
    return (fullRange ? 0.0 : 16.0 * depth * code) + kMatteLift;
}
void Orientation(uint2 id) {
    const uint s = max(8u, ((right - left) / kHorizonAcross + 4) / 8 * 8); // The block's side.
    if (id.x >= (right - left) / s || id.y >= (bottom - top) / s) return;
    // The 8 x 8 texels at the block's centre and the texel around them: ten rows of ten from `origin`, two rows at a time
    // (five Gathers), the Scharr gradients of a row once the row after it is in (rows 1 to 8, in order).
    const int2 origin = int2(left, top) + int2(id * s + s / 2) - 5;
    if (any(origin < LumaLow()) || any(origin + 9 > LumaHigh())) return;
    float v[10][10];
    float jxx = 0, jyy = 0, jxy = 0;
    [unroll] for (int j = 0; j < 5; ++j) {
        [unroll] for (int i = 0; i < 5; ++i) {
            const float4 q = GatheredQuad(origin + int2(2 * i, 2 * j)); // x = (0, 1), y = (1, 1), z = (1, 0), w = (0, 0).
            v[2 * j][2 * i] = q.w; v[2 * j][2 * i + 1] = q.z; v[2 * j + 1][2 * i] = q.x; v[2 * j + 1][2 * i + 1] = q.y;
        }
        [unroll] for (int y = 2 * j - 1; y <= 2 * j; ++y) {
            if (y < 1 || y > 8) continue;
            [unroll] for (int x = 1; x < 9; ++x) {
                const float gx = (3 * (v[y - 1][x + 1] - v[y - 1][x - 1]) + 10 * (v[y][x + 1] - v[y][x - 1]) + 3 * (v[y + 1][x + 1] - v[y + 1][x - 1])) / 32.0;
                const float gy = (3 * (v[y + 1][x - 1] - v[y - 1][x - 1]) + 10 * (v[y + 1][x] - v[y - 1][x]) + 3 * (v[y + 1][x + 1] - v[y - 1][x + 1])) / 32.0;
                jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
            }
        }
    }
    const float energy = jxx + jyy, d = jxx - jyy;
    if (energy < kHorizonEnergy) return;
    const float coherence = sqrt(d * d + 4 * jxy * jxy) / energy;
    if (coherence < kHorizonCoherence) return;
    float angle = degrees(0.5 * atan2(2 * jxy, d)); // The gradient's direction, -90 to 90 degrees.
    uint lines = 1;                                // A near-vertical line: its gradient near horizontal.
    if (abs(angle) > 45) { angle -= angle > 0 ? 90.0 : -90.0; lines = 0; }
    const float at = (angle + 16.0) * 2.0;
    if (at < 0 || at >= (float)kHorizonBins) return;
    // The edge of a bar encoded in the picture (a letterbox's, a pillarbox's) is not the scene's: a line along the picture's
    // axis with a whole end row (column) of the window black is left out (video::HorizonLevel::Accumulate does the same).
    if (abs(angle) < kBarAxis) {
        const float dark = Dark();
        bool first = true, last = true;
        [unroll] for (int i = 0; i < 10; ++i) {
            first = first && (lines ? v[i][0] : v[0][i]) <= dark;
            last = last && (lines ? v[i][9] : v[9][i]) <= dark;
        }
        if (first || last) return;
    }
    const uint bin = min((uint)at, kHorizonBins - 1), weight = (uint)(16.0 * coherence * coherence + 0.5);
    const uint fraction = min((uint)((at - (float)bin) * 256.0), 255u);
    const uint address = kHorizonBase + (lines * kHorizonBins + bin) * 8;
    peak.InterlockedAdd(address, weight); peak.InterlockedAdd(address + 4, weight * fraction);
}
// The picture's extents for video::Matte (docs/picture.md, "Black bars"): the first kMatteLines threads of each row of the
// grid read one of kMatteLines columns spread over the crop at the even rows of their block, and the first kMatteLines threads
// of each column one of kMatteLines rows at the even columns of theirs (fewer lines for a crop under 256 samples: one per
// block); a sample brighter than Dark() is picture. Even samples only: an edge is found at most one sample inside the
// picture, never in the bar (64 lines at every sample cost 0.2 ms a 4K picture on the UHD 630; this, about half of that).
// Only with kMatte (the renderer: every picture until the bars are known, then every fourth). Each group keeps the largest of 65536 - first row, last row + 1, 65536 - first
// column, last column + 1 its threads saw, then one atomic maximum each ([48, 64), cleared by StatsReset: zero is none).
groupshared uint groupMatte[4];
void Matte(uint2 id) {
    const uint w = right - left, h = bottom - top, lines = min(kMatteLines, (w + 7) / 8), rows = min(kMatteLines, (h + 7) / 8);
    const float dark = Dark();
    if (id.x < lines) {
        const uint x = (uint)((2 * id.x + 1) * w / (2 * lines)); // Matte::Sample.
        uint first = 0, last = 0;
        [unroll] for (uint k = 0; k < 8; k += 2) {
            const uint y = id.y * 8 + k;
            if (y < h && luma.Load(int3(left + x, top + y, 0)) > dark) { first = max(first, 65536 - y); last = max(last, y + 1); }
        }
        if (last) { InterlockedMax(groupMatte[0], first); InterlockedMax(groupMatte[1], last); }
    }
    if (id.y < rows) {
        const uint y = (uint)((2 * id.y + 1) * h / (2 * rows));
        uint first = 0, last = 0;
        [unroll] for (uint k = 0; k < 8; k += 2) {
            const uint x = id.x * 8 + k;
            if (x < w && luma.Load(int3(left + x, top + y, 0)) > dark) { first = max(first, 65536 - x); last = max(last, x + 1); }
        }
        if (last) { InterlockedMax(groupMatte[2], first); InterlockedMax(groupMatte[3], last); }
    }
}
[numthreads(8, 8, 1)]
void Stats(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) { groupSum = 0; groupCount = 0; groupMax = 0; groupMoved = 0; groupMatte[0] = 0; groupMatte[1] = 0; groupMatte[2] = 0; groupMatte[3] = 0; }
    GroupMemoryBarrierWithGroupSync();
    if ((filterFlags & kMatte) != 0) Matte(id.xy);
    const int2 site = int2(left, top) + int2(id.xy) * 8 + 4;
    if (all(site < int2(right, bottom))) {
        if ((filterFlags & kHorizon) != 0) Orientation(id.xy);
        const uint code = (uint)(saturate(LumaAt(site)) * 1023.0 + 0.5);
        InterlockedAdd(groupSum, code);
        InterlockedAdd(groupCount, 1);
        InterlockedMax(groupMax, code);
        const uint block = id.y * ((right - left + 7) / 8) + id.x;
        if (block < kMaxBlocks) {
            const uint before = blocks.Load(block * 4);
            if (abs(int(code) - int(before)) > int(kMoved)) InterlockedAdd(groupMoved, 1);
            blocks.Store(block * 4, code);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && groupCount != 0) {
        peak.InterlockedAdd(32, groupSum); peak.InterlockedAdd(36, groupCount);
        peak.InterlockedMax(40, groupMax); peak.InterlockedAdd(44, groupMoved);
    }
    if (index == 0 && groupMatte[1] != 0) { peak.InterlockedMax(kMatteBase, groupMatte[0]); peak.InterlockedMax(kMatteBase + 4, groupMatte[1]); }
    if (index == 0 && groupMatte[3] != 0) { peak.InterlockedMax(kMatteBase + 8, groupMatte[2]); peak.InterlockedMax(kMatteBase + 12, groupMatte[3]); }
}
// After the statistics have been copied for the CPU: the per-picture fields start over for the next picture (the extents and
// the horizon's histograms too: 256 words, four a thread).
[numthreads(64, 1, 1)]
void StatsReset(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0) { peak.Store(40, 0); peak.Store(44, 0); peak.Store4(kMatteBase, uint4(0, 0, 0, 0)); }
    peak.Store4(kHorizonBase + id.x * 16, uint4(0, 0, 0, 0));
}
// Target tracking (docs/picture.md, "Target tracking"): the crop's luma at coarse resolution for video::TargetTracker, one
// cell per `width` x `width` samples (the grid scale; [left, right) x [top, bottom) is the whole crop, not the zoomed
// region), their rounded mean as an 8-bit code, four cells a word, `height` words a row (u3 is the grid here). Edge cells
// average the samples they cover, as video::MakeGrid does on the CPU. Recorded only while tracking.
[numthreads(8, 8, 1)]
void Grid(uint3 id : SV_DispatchThreadID) {
    const uint scale = width, words = height;
    const uint cellsWide = (right - left + scale - 1) / scale, cellsHigh = (bottom - top + scale - 1) / scale;
    if (id.x >= words || id.y >= cellsHigh) return;
    uint word = 0;
    for (uint k = 0; k < 4; ++k) {
        const uint cx = id.x * 4 + k;
        if (cx >= cellsWide) break;
        const uint x0 = left + cx * scale, y0 = top + id.y * scale, x1 = min(right, x0 + scale), y1 = min(bottom, y0 + scale);
        uint sum = 0;
        for (uint y = y0; y < y1; ++y)
            for (uint x = x0; x < x1; ++x) sum += (uint)(saturate(luma.Load(int3(x, y, 0))) * 255.0 + 0.5);
        const uint count = (x1 - x0) * (y1 - y0);
        word |= ((sum + count / 2) / count) << (8 * k);
    }
    blocks.Store((id.y * words + id.x) * 4, word);
}
// Remote vibrometry in the window (docs/picture.md, "Vibrometry in the window", 2026-11-03; video::VibrometrySample): the
// region [viewM0, viewM2) x [viewM1, viewM3) of the decoded picture (luma samples of the coded surface, floats: fractional
// edges) as `width` x `width` cells (video::kVibrometryCells), one group a cell: its Y' mean, the same with the cell moved half a
// sample right, left, down and up, and its Cb and Cr means (the chroma plane over the cell's half-size rectangle), each an
// area mean (a sample counts by how much of it the rectangle covers, as video::AreaMean), in 8-bit codes (10-bit sources: the
// code / 4), seven floats a cell at u3 (blocks) [cell * 7]. Recorded only while vibrometry measures.
static const uint kVibroValues = 7;
groupshared float vibroSums[64 * kVibroValues];
float Covered(float s, float a, float b) { return max(0.0, min(s + 1.0, b) - max(s, a)); }
[numthreads(8, 8, 1)]
void Vibro(uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    const uint cells = width;
    const float l = asfloat(viewM0), t = asfloat(viewM1), r = asfloat(viewM2), b = asfloat(viewM3);
    const float cw = (r - l) / cells, ch = (b - t) / cells;
    const float x0 = l + group.x * cw, x1 = x0 + cw, y0 = t + group.y * ch, y1 = y0 + ch;
    const float scale = tenBit ? 65535.0 / 256.0 : 255.0;
    float sums[kVibroValues] = {0, 0, 0, 0, 0, 0, 0};
    // Luma over the cell and its moves: [x0 - 0.5, x1 + 0.5) x [y0 - 0.5, y1 + 0.5).
    const int sx0 = (int)floor(x0 - 0.5), sx1 = (int)ceil(x1 + 0.5), sy0 = (int)floor(y0 - 0.5), sy1 = (int)ceil(y1 + 0.5);
    for (int y = sy0 + (int)(index / 8); y < sy1; y += 8) {
        const float fy = (float)y, wy = Covered(fy, y0, y1), down = Covered(fy, y0 + 0.5, y1 + 0.5), up = Covered(fy, y0 - 0.5, y1 - 0.5);
        for (int x = sx0 + (int)(index % 8); x < sx1; x += 8) {
            const float fx = (float)x, wx = Covered(fx, x0, x1), across = Covered(fx, x0 + 0.5, x1 + 0.5), back = Covered(fx, x0 - 0.5, x1 - 0.5);
            const float v = luma.Load(int3(clamp(x, 0, (int)codedWidth - 1), clamp(y, 0, (int)codedHeight - 1), 0)) * scale;
            sums[0] += wx * wy * v; sums[1] += across * wy * v; sums[2] += back * wy * v; sums[3] += wx * down * v; sums[4] += wx * up * v;
        }
    }
    // Chroma: the cell's rectangle on the half-resolution plane.
    const float c0 = 0.5 * x0, c1 = 0.5 * x1, d0 = 0.5 * y0, d1 = 0.5 * y1;
    const int cx0 = (int)floor(c0), cx1 = (int)ceil(c1), cy0 = (int)floor(d0), cy1 = (int)ceil(d1);
    for (int v = cy0 + (int)(index / 8); v < cy1; v += 8) {
        const float wv = Covered((float)v, d0, d1);
        for (int u = cx0 + (int)(index % 8); u < cx1; u += 8) {
            const float w = Covered((float)u, c0, c1) * wv;
            const float2 c = chroma.Load(int3(clamp(u, 0, (int)(codedWidth / 2) - 1), clamp(v, 0, (int)(codedHeight / 2) - 1), 0)) * scale;
            sums[5] += w * c.x; sums[6] += w * c.y;
        }
    }
    for (uint k = 0; k < kVibroValues; ++k) vibroSums[index * kVibroValues + k] = sums[k];
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1) {
        if (index < stride) for (uint k = 0; k < kVibroValues; ++k) vibroSums[index * kVibroValues + k] += vibroSums[(index + stride) * kVibroValues + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        const float area = cw * ch, carea = 0.25 * cw * ch;
        const uint at = ((group.y * cells + group.x) * kVibroValues) * 4;
        for (uint k = 0; k < kVibroValues; ++k) blocks.Store(at + k * 4, asuint(vibroSums[k] / (k < 5 ? area : carea)));
    }
}
// Tensor export in the window (docs/picture.md, "Tensor export", 2026-11-03; video::ExportTensorMeans): the region [viewM0,
// viewM2) x [viewM1, viewM3) (luma samples of the coded surface, floats) as `width` x `height` cells, one thread a cell: its
// Y', Cb and Cr area means (fractional edges, as video::AreaMean), 8-bit codes, three floats a cell at u3 (blocks) [cell * 3],
// row-major. The CPU converts and normalises them into the caller's tensor.
[numthreads(8, 8, 1)]
void Tensor(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const float l = asfloat(viewM0), t = asfloat(viewM1), r = asfloat(viewM2), b = asfloat(viewM3);
    const float cw = (r - l) / width, ch = (b - t) / height;
    const float x0 = l + id.x * cw, x1 = x0 + cw, y0 = t + id.y * ch, y1 = y0 + ch;
    const float scale = tenBit ? 65535.0 / 256.0 : 255.0;
    float y = 0, cb = 0, cr = 0, wl = 0, wc = 0;
    for (int sy = (int)floor(y0); sy < (int)ceil(y1); ++sy) {
        const float wy = Covered((float)sy, y0, y1);
        for (int sx = (int)floor(x0); sx < (int)ceil(x1); ++sx) {
            const float w = Covered((float)sx, x0, x1) * wy;
            y += w * luma.Load(int3(clamp(sx, 0, (int)codedWidth - 1), clamp(sy, 0, (int)codedHeight - 1), 0)); wl += w;
        }
    }
    const float c0 = 0.5 * x0, c1 = 0.5 * x1, d0 = 0.5 * y0, d1 = 0.5 * y1;
    for (int v = (int)floor(d0); v < (int)ceil(d1); ++v) {
        const float wv = Covered((float)v, d0, d1);
        for (int u = (int)floor(c0); u < (int)ceil(c1); ++u) {
            const float w = Covered((float)u, c0, c1) * wv;
            const float2 c = chroma.Load(int3(clamp(u, 0, (int)(codedWidth / 2) - 1), clamp(v, 0, (int)(codedHeight / 2) - 1), 0));
            cb += w * c.x; cr += w * c.y; wc += w;
        }
    }
    const uint at = (id.y * width + id.x) * 12;
    blocks.Store(at, asuint(scale * y / max(wl, 1e-6))); blocks.Store(at + 4, asuint(scale * cb / max(wc, 1e-6))); blocks.Store(at + 8, asuint(scale * cr / max(wc, 1e-6)));
}
// One thread, after Detect and before the conversion: fold this picture's peak into the smoothed source peak over the
// presentation time since the last folded picture (peakDtBits: 0 for a picture that presents before one already
// folded, a B-picture after its successor), fast attack (100 ms) and slow release (800 ms), whatever the frame rate.
// A new epoch, the first picture ever, and a scene cut (the peak moving by more than kCutPq at once) take the picture's
// own peak: no inherited exposure. Bounded to [100, 10000] cd/m2. Then the detector is cleared for the next picture.
static const float kCutPq = 0.12; // About a factor of 2.5 in luminance around 100-1000 cd/m2.
[numthreads(1, 1, 1)]
void Peak(uint3 id : SV_DispatchThreadID) {
    const float detected = peak.Load(0) / 65535.0, dt = asfloat(peakDtBits);
    const uint frames = peak.Load(8);
    float smoothed = asfloat(peak.Load(4));
    const float floorPq = 0.5080784; // PQ(100) cd/m2.
    const bool start = frames == 0 || (peakFlags & 1) != 0;
    const bool cut = !start && abs(max(detected, floorPq) - smoothed) > kCutPq; // smoothed is stored clamped.
    if (start || cut) smoothed = detected;
    else smoothed += (detected - smoothed) * (1.0 - exp(-max(dt, 0.0) / (detected > smoothed ? 0.1 : 0.8)));
    peak.Store(4, asuint(clamp(smoothed, floorPq, 1.0)));
    peak.Store(0, 0);
    peak.Store(8, frames + 1);
    if (cut) peak.Store(12, peak.Load(12) + 1);
    if (start) peak.Store(16, peak.Load(16) + 1);
}
