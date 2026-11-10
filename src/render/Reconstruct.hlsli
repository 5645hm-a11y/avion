// Shared spatial stage of the colour-conversion passes: source reconstruction and output sharpening, fused into
// the conversion dispatch. The including shader defines `float3 Convert(float y, float2 c)`, mapping one
// reconstructed Y'CbCr sample (normalized UNORM, as sampled) to display-encoded R'G'B' (BT.1886, gamma 2.4).
//
// Source access: point loads with the bilinear arithmetic in the shader, clamped to the crop rectangle. On the
//   reference integrated GPU (Intel UHD 630) filtered sampling of the planar NV12/P010 views is about 3x slower than
//   point loads, and groupshared staging slower still (scattered shared-memory access is a data-port message per
//   value there), so reconstruction uses neither.
// Crop: only texels inside the crop rectangle are picture; the rest of the coded surface is padding the decoder does
//   not define. Every read is clamped to the crop, including the 2x2 fetches (LumaQuad: a Gather when the quad lies
//   inside the crop, four clamped loads at its border), so no output depends on padding. Positions are computed
//   relative to the crop's origin and the origin is added to the integer texel index, so a crop inside a surface
//   converts bit for bit as the same picture alone in a surface of its own size (the origin's chroma parity, half a
//   chroma texel for an odd origin, is the only part of the offset that enters the fractions).
// Reconstruction (reconstruction == 1, only while upscaling in both axes, strictly in at least one):
//   Luma is rebuilt from a 4x4 neighbourhood. A structure tensor from central-difference gradients at the four
//   samples surrounding the output position (bilinearly weighted) gives the local edge direction (its dominant
//   eigenvector) and coherence. An elliptical Lanczos-2-shaped kernel is kept at unit width across the edge and
//   stretched along it with coherence, which suppresses staircasing without softening the edge. The result is
//   clamped to the range of the four surrounding samples: negative lobes sharpen but cannot ring. Chroma (half
//   resolution) stays bilinear. At 1:1 or when downscaling luma is bilinear: the kernel is not interpolating on
//   texel centres, and downscaling needs a prefilter instead.
// Sharpening (sharpness > 0): contrast-adaptive, on linear-light luminance. Each 16x16 group also reconstructs the
//   one-pixel ring around its tile and keeps the linear luminance of all 18x18 outputs in groupshared memory (one
//   value per pixel, the only shared-memory use). Each pixel adds a gain-scaled high-pass of its 4-neighbour cross;
//   the gain falls with local contrast (already sharp edges are left alone) and fades out below a noise floor
//   (flat, noisy areas are not amplified). The sharpened luminance is clamped to the range of that cross, so it
//   cannot overshoot its neighbours (no halos or ringing), and is applied to the pixel's colour as a ratio, which
//   preserves hue.
// Neural (reconstruction == 2, same upscaling condition): luma comes from the micro-ESPCN pass (NeuralSuperRes.hlsl),
//   a 2x image of the crop at `neural`'s origin, sampled bilinearly (point loads). At exactly 2x the output lands on its
//   texels and reproduces the network's pixels; other factors resample them.
// Guided chroma (chromaMode bit 0, whenever chroma is upsampled: fewer than two source pixels per output pixel in
//   either axis, so also at 1:1): 4:2:0 chroma is interpolated with the reconstructed luma as a guide, so colour
//   edges follow luma edges instead of bleeding over the width of a chroma texel. Over the four chroma samples that
//   bilinear interpolation uses, luma at each chroma site (mean of its 2x2 luma quad, one Gather) and the chroma
//   are fitted by a local linear model c = mean(c) + a (Y - mean(Y)) (guided-filter regression, a per component,
//   regularized by kGuideEpsilon so luma variation below a few codes, i.e. noise, does not drive it). The output adds
//   a times the high-frequency luma residual (reconstructed luma at the output minus the chroma-resolution luma
//   interpolated like the chroma) to bilinear chroma, then clamps to the range of the four samples: no overshoot,
//   and where luma is flat (isoluminant colour edges) a = 0 and the result is exactly bilinear.
// Downscale prefilter (filterFlags bit 0, when the crop is downscaled by more than 2x in either axis): luma is the average of the
//   output pixel's footprint on the source, a box of `span` source texels per axis ([centre - span/2, centre + span/2),
//   with fractional weights for the texels it covers in part: area averaging), bounded to kMaxSpan texels per axis.
//   An axis that is not downscaled keeps the bilinear tent. Chroma does the same on its own plane where chroma itself
//   is downscaled (more than two luma texels per output pixel). Clamped loads only: the footprint lies inside the crop.
// Dither (filterFlags bit 1, 10-bit and HDR sources): before the 8-bit UNORM write, a static interleaved-gradient-noise
//   offset in [-0.5, 0.5) LSB per pixel decorrelates the quantization from the picture, so smooth 10-bit gradients do
//   not band. It tapers to nothing within half an LSB of 0 and 1, so black and white stay exact; no output moves by
//   more than one LSB.
// Turned view (filterFlags bit 3, kRotate; docs/picture.md, "Roll and compensated micro-motion", "Rotation"): the output
//   pixel's centre X maps to its position in the crop through an affine map, q = M X + t (viewM, viewT, luma texels from the
//   crop's origin): the stabiliser's roll and the sub-texel part of a moving view (live ingest), a file's quarter turns and
//   mirror (tkhd) and the viewer's leveling turn (playback). Scale() is then the extent of one output pixel's step along each
//   source axis. Reads clamp to the crop, or with bit 4 (kSurfaceClamp, live ingest: surfaces without padding) to the coded
//   surface, where a turned view's corners reach past the crop; every group takes the clamped loads (TileInterior's test
//   assumes an upright crop). Only in the PICTURE_CONTROLS variants (ColorPipeline chooses them for it); neural
//   reconstruction is not used with it.
// Comparison (filterFlags bit 2, docs/picture.md "Original | Enhanced"): the output's left half is the plain conversion
//   (bilinear luma and chroma, no sharpening: what the picture would be without the reconstruction), the right half the
//   conversion as configured, each bit for bit as it would be alone; a two-pixel seam at the centre column, darkened then
//   lightened, so it shows over any picture. Groups wholly left of the seam take the plain path alone (uniform per group);
//   only the column of groups across the seam evaluates both. The sharpening ring keeps the enhanced values, so the right
//   half's sharpening does not see the left half's plain pixels. It is a separate compiled variant (SPLIT_COMPARE, chosen
//   by ColorPipeline when the flag is set): compiled into the production shader, the unused branch alone cost 0.9 ms per
//   720p -> 1080p conversion on the reference GPU (register allocation), with or without the flag.
// With sharpness 0 each output is converted and written directly, per pixel.
// Picture controls and micro-motion (pictureFlags; docs/picture.md, "Picture controls and micro-motion"), after the
//   reconstruction, the sharpening and the comparison, before the dither, once per output pixel (Finish):
//   - Micro-motion (bit 1): each output pixel keeps a temporal reference of its display-encoded luma Y' (u2, one float per
//     pixel of the slot). The pixel's change d = Y' - reference is amplified to Y' + (gain - 1) w d, added to all three
//     channels (a luma change, hue kept), where w is 1 for kMicroNoise < |d| <= kMicroSmall, falls linearly to 0 at kMicroLarge,
//     and is 0 above it and for |d| below the noise floor's half (quantisation jitter of one code is not magnified). A change
//     larger than kMicroLarge is motion: no amplification, and the reference jumps to the pixel at once, so a moving edge
//     leaves no trail (no ghost). Otherwise a folded picture (bit 3: the next in presentation order) moves the reference by
//     kMicroFollow of the change: a temporal low-pass, so what is amplified is variation around the pixel's recent mean
//     (a pulse, a breath, a sub-pixel vibration), as in Eulerian video magnification. Bit 2 starts over: the reference is the
//     pixel itself (a new epoch, a new zoom region or picture size). The reference is read at microRead and written at
//     microWrite (floats into u2; both 0 for the windowed engine: in place). Bit 4 (kMicroWarp, 2026-10-27: live ingest, whose
//     view moves with stabilisation, the saccade or tracking) reads it where this pixel's content was in the previous picture's
//     output, (m0 X + m1 Y + m2, m3 X + m4 Y + m5) for its centre (X, Y), between pixels (the reference is then double-buffered:
//     read and write differ); a pixel whose content came from outside the previous view starts over, and where the reference's
//     2x2 neighbourhood spans more than kMicroEdge (an edge or texture) the magnification fades out to nothing at twice that,
//     since a residual misregistration of a fraction of a pixel there is a change the size of the gradient, not a pulse.
//   - Grade (bit 0), on display-encoded R'G'B', each step only when it is not neutral (a neutral step changes no bit):
//     shadow lift L (0 to 1) adds 2 L Y' (1 - Y'/0.75)^2 to every channel below Y' = 0.75 (black stays black; the curve is
//     monotonic and meets the identity with slope 1 at 0.75, so highlights are bit-identical and never clip; a channel pushed
//     past 1 is pulled toward the new luma, hue kept); contrast c around mid-grey ((v - 0.5) c + 0.5); brightness b (+b);
//     gamma g (v^(1/g), g > 1 brightens the mid-tones); saturation s around the BT.709 luma (s = 0: monochrome).
static const uint kGrade = 1, kMicro = 2, kMicroStart = 4, kMicroFold = 8, kMicroWarp = 16;
static const float kMicroNoise = 1.0 / 255.0, kMicroSmall = 0.04, kMicroLarge = 0.08, kMicroFollow = 0.125, kMicroEdge = 3.0 / 255.0;
static const uint kRotate = 8, kSurfaceClamp = 16;
// Bars (filterFlags bit 7, kBars, 2026-11-01: live ingest's fit, MEDIA_SENSOR_GPU_FIT): with kRotate, an output pixel whose centre
//   maps outside the crop is black (0, 0, 0), after everything else: the turned picture fitted whole inside the image, black
//   bars around it. The pixels inside are what they are without the flag, bit for bit (their sharpening ring reads the
//   clamped picture, not the bars).
static const uint kBars = 128;
static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
Texture2D<float> neural : register(t2);
RWTexture2D<float4> rgb : register(u0);
SamplerState pointClamp : register(s0); // Static sampler; Gather ignores its filter.
cbuffer Parameters : register(b0) {
    uint width, height, codedWidth, codedHeight;
    uint left, top, right, bottom;
    uint matrixId, fullRange, tenBit, primaries;
    uint transfer, peakMode, fixedPeakBits, targetBits; // HDR pass only; peak and target are float bits.
    uint reconstruction, sharpnessBits, noiseBits, chromaMode; // sharpness in [0, 1]; noise floor in sqrt-luminance units;
                                                              // chromaMode bit 0: luma-guided chroma.
    uint peakDtBits, peakFlags; // HDR dynamic peak: presentation time since the last folded picture (s, float bits);
                                // peakFlags bit 0: start over (a new playback epoch: no history).
    uint filterFlags;           // Bit 0: downscale prefilter (area averaging); bit 1: dither the 8-bit output; bit 2: split
                                // comparison (plain left half, enhanced right half).
    uint pictureFlags;          // kGrade, kMicro, kMicroStart, kMicroFold (above).
    uint brightnessBits, contrastBits, saturationBits, gammaBits, liftBits, microBits; // Floats; microBits: the gain (1 to 8).
    uint viewM0, viewM1, viewM2, viewM3, viewT0, viewT1;    // kRotate: q = M X + t (floats).
    uint microRead, microWrite;                            // Micro-motion's reference: where it is read and written (floats).
    uint microM0, microM1, microM2, microM3, microM4, microM5; // kMicroWarp: output centre -> previous output centre (floats).
};
RWStructuredBuffer<float> motion : register(u2); // Micro-motion: each output pixel's luma reference (row of `width`).
// HDR output (API 14; ColorPipeline kHdrOutput, kLinearOutput): Convert returns PQ BT.2020 code values (an HDR10 signal) instead of
// display-referred BT.709 ones, and Written stores them as they are (a 10-bit texture) or as scRGB (an FP16 texture: linear BT.709,
// 1.0 = 80 cd/m2, negative outside BT.709). Everything in between (sharpening, controls, bars) runs on the PQ values.
static const uint kHdrOutput = 256, kLinearOutput = 512;
bool HdrOutput() { return (filterFlags & kHdrOutput) != 0; } // Uniform per dispatch.
static const float M1 = 0.1593017578125, M2 = 78.84375, C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
float PqToNits(float e) {
    float p = pow(max(e, 0.0), 1.0 / M2);
    return 10000.0 * pow(max((p - C1) / (C2 - C3 * p), 0.0), 1.0 / M1);
}
float NitsToPq(float nits) {
    float p = pow(max(nits, 0.0) / 10000.0, M1);
    return pow(max((C1 + C2 * p) / (1.0 + C3 * p), 0.0), M2);
}
float3 NitsToPq3(float3 nits) { return float3(NitsToPq(nits.r), NitsToPq(nits.g), NitsToPq(nits.b)); }
// BT.709 primaries to BT.2020 (ITU-R BT.2087), and back, in linear light.
float3 Bt709To2020(float3 l) {
    return float3(dot(float3(0.627404, 0.329283, 0.043313), l), dot(float3(0.069097, 0.919540, 0.011362), l), dot(float3(0.016391, 0.088013, 0.895595), l));
}
float3 Bt2020To709(float3 l) {
    return float3(dot(float3(1.660491, -0.587641, -0.072850), l), dot(float3(-0.124550, 1.132900, -0.008349), l), dot(float3(-0.018151, -0.100579, 1.118730), l));
}
float4 Written(float3 color) {
    float4 written = float4(color, 1);
    [branch] if ((filterFlags & kLinearOutput) != 0) {
        const float3 nits = float3(PqToNits(color.r), PqToNits(color.g), PqToNits(color.b));
        written = float4(Bt2020To709(nits) / 80.0, 1);
    }
    return written;
}
float3 Convert(float y, float2 c);

static const uint Tile = 16;       // Output pixels per group side (a larger tile halves the sharpening ring overhead).
static const uint Halo = Tile + 2; // Sharpening needs one output pixel around the tile.
groupshared float haloLight[Halo][Halo];

#ifdef PICTURE_CONTROLS
bool Turned() { return (filterFlags & kRotate) != 0; } // Uniform per dispatch.
bool SurfaceClamp() { return (filterFlags & kSurfaceClamp) != 0; }
#else
bool Turned() { return false; }
bool SurfaceClamp() { return false; }
#endif
// Source pixels per output pixel (each source axis); a turned view's: the extent of one output step along each source axis.
float2 Scale() {
    float2 scale = float2(right - left, bottom - top) / float2(width, height);
#ifdef PICTURE_CONTROLS
    [branch] if (Turned()) scale = float2(abs(asfloat(viewM0)) + abs(asfloat(viewM1)), abs(asfloat(viewM2)) + abs(asfloat(viewM3)));
#endif
    return scale;
}
// Position of an output pixel's centre in luma texels from the crop's origin (texel centres at integer + 0.5).
float2 SourcePosition(int2 output) {
    const float2 x = float2(output) + 0.5;
    float2 q;
#ifdef PICTURE_CONTROLS
    [branch] if (Turned()) q = float2(asfloat(viewM0) * x.x + asfloat(viewM1) * x.y + asfloat(viewT0), asfloat(viewM2) * x.x + asfloat(viewM3) * x.y + asfloat(viewT1));
    else q = x * Scale();
#else
    q = x * Scale();
#endif
    return q;
}
int2 LumaLow() { return int2(left, top); }
int2 LumaHigh() { return int2(right, bottom) - 1; }
// 4:2:0 chroma texels that are picture: those covering at least one luma column and row of the crop.
int2 ChromaLow() { return int2(uint2(left, top) / 2); }
int2 ChromaHigh() { return int2((uint2(right, bottom) + 1) / 2) - 1; }
// What reads clamp to: the crop, or the whole coded surface (kSurfaceClamp).
int2 LumaFloor() { return SurfaceClamp() ? int2(0, 0) : LumaLow(); }
int2 LumaCeiling() { return SurfaceClamp() ? int2(codedWidth, codedHeight) - 1 : LumaHigh(); }
int2 ChromaFloor() { return SurfaceClamp() ? int2(0, 0) : ChromaLow(); }
int2 ChromaCeiling() { return SurfaceClamp() ? int2((uint2(codedWidth, codedHeight) + 1) / 2) - 1 : ChromaHigh(); }
// The same position in chroma texels from ChromaLow(): half the luma position, plus half a texel when the crop starts
// on an odd luma column or row (its first chroma texel then starts one luma texel before the crop).
float2 ChromaPosition(float2 q) { return 0.5 * q + 0.5 * float2(left & 1, top & 1); }
float LumaAt(int2 texel) { return luma.Load(int3(clamp(texel, LumaFloor(), LumaCeiling()), 0)); }
float2 ChromaAt(int2 texel) { return chroma.Load(int3(clamp(texel, ChromaFloor(), ChromaCeiling()), 0)); }
// The 2x2 luma quad whose first (top-left) texel is `first`, in Gather order (x = (0,1), y = (1,1), z = (1,0),
// w = (0,0)), clamped to the crop: one Gather where the quad is known to be picture, four clamped loads where it may
// reach the crop's border (a Gather there would return padding). Which one is decided once per 16x16 group
// (TileInterior) and once per fetch site below, so each pixel takes one branch that is the same for every thread of
// its group: deciding per quad cost 22 % of the edge-directed pass and doubled the cost of guided chroma on the
// reference GPU.
float4 GatheredQuad(int2 first) { return luma.GatherRed(pointClamp, (float2(first) + 1.0) / float2(codedWidth, codedHeight)); }
float4 ClampedQuad(int2 first) { return float4(LumaAt(first + int2(0, 1)), LumaAt(first + int2(1, 1)), LumaAt(first + int2(1, 0)), LumaAt(first)); }
// Whether every luma texel any thread of the group at output `origin` can fetch lies inside the crop: the group's
// outputs and their one-pixel sharpening ring, the 4x4 edge-directed block around each (two texels either side), and
// the luma quads of the chroma sites around them (up to three texels either side).
// Conservative, and in floats only (each thread of the group evaluates it): the ring's first and last pixel centres,
// in luma texels from the crop's origin, must be at least 4.5 texels inside the crop on the low side and 5.5 on the
// high side.
bool TileInterior(int2 origin, float2 scale) {
    const float2 first = (float2(origin) - 0.5) * scale, last = (float2(origin) + (Tile + 0.5)) * scale;
    return !Turned() && all(first >= 4.5) && all(last <= float2(right - left, bottom - top) - 5.5);
}
// `q`: positions from the crop's (or the chroma crop's) origin, in texels of the respective plane.
float BilinearLuma(float2 q) {
    float2 b = floor(q - 0.5), f = q - 0.5 - b; int2 i = int2(b) + LumaLow();
    return lerp(lerp(LumaAt(i), LumaAt(i + int2(1, 0)), f.x), lerp(LumaAt(i + int2(0, 1)), LumaAt(i + int2(1, 1)), f.x), f.y);
}
// Downscale prefilter: per axis, the taps of a resampling kernel at `center` (texels from the plane's crop origin).
static const float kMaxSpan = 4.0; // Widest footprint averaged, in source texels per axis (5 taps at most).
struct Axis { float center, low, high, span; int first; bool tent; };
Axis AxisFor(float center, float span) {
    Axis a; a.center = center; a.tent = span <= 1.0;
    a.span = a.tent ? 1.0 : min(span, kMaxSpan);
    a.low = center - 0.5 * a.span; a.high = center + 0.5 * a.span;
    // The first texel within the tent's reach, or overlapping the box (a rounding error just below an integer edge
    // must not start one texel early: its weight would be ~0 and the last texel would be cut off).
    a.first = a.tent ? int(floor(center - 0.5)) : int(floor(a.low + 1e-4));
    return a;
}
float AxisWeight(Axis a, int texel) { // `texel` from the crop origin; 0 outside the kernel.
    const float t = float(texel);
    if (a.tent) return max(1.0 - abs(a.center - (t + 0.5)), 0.0);
    return max(min(t + 1.0, a.high) - max(t, a.low), 0.0) / a.span;
}
// Taps per axis for a span, the same for every pixel of a dispatch: 2 for the tent; an integer span's footprint starts
// on a texel edge (output centres at (o + 0.5) span); otherwise it covers ceil(span) + 1 texels at most.
int AxisTaps(float span) { return span <= 1.0 ? 2 : min(frac(min(span, kMaxSpan)) == 0.0 ? int(min(span, kMaxSpan)) : int(ceil(min(span, kMaxSpan))) + 1, 5); }
// Fully unrolled per tap count, so the loads of a pixel are issued together (a data-dependent loop serialises them:
// it cost 4.5 ms at 4K to 1080p on the reference GPU, with the same number of loads as bilinear).
#define AREA_KERNELS(K) float AreaLuma##K(Axis ax, Axis ay) {     float sum = 0;     [unroll] for (int j = 0; j < K; ++j) {         float row = 0;         [unroll] for (int i = 0; i < K; ++i) row += AxisWeight(ax, ax.first + i) * LumaAt(LumaLow() + int2(ax.first + i, ay.first + j));         sum += AxisWeight(ay, ay.first + j) * row;     }     return sum; } float2 AreaChroma##K(Axis ax, Axis ay) {     float2 sum = 0;     [unroll] for (int j = 0; j < K; ++j) {         float2 row = 0;         [unroll] for (int i = 0; i < K; ++i) row += AxisWeight(ax, ax.first + i) * ChromaAt(ChromaLow() + int2(ax.first + i, ay.first + j));         sum += AxisWeight(ay, ay.first + j) * row;     }     return sum; }
AREA_KERNELS(2)
AREA_KERNELS(3)
AREA_KERNELS(4)
AREA_KERNELS(5)
float AreaLuma(float2 q, float2 span) {
    const Axis ax = AxisFor(q.x, span.x), ay = AxisFor(q.y, span.y);
    const int taps = max(AxisTaps(span.x), AxisTaps(span.y)); // Uniform per dispatch.
    float y;
    [branch] if (taps <= 2) y = AreaLuma2(ax, ay);
    else [branch] if (taps == 3) y = AreaLuma3(ax, ay);
    else [branch] if (taps == 4) y = AreaLuma4(ax, ay);
    else y = AreaLuma5(ax, ay);
    return y;
}
float2 AreaChroma(float2 q, float2 span) {
    const Axis ax = AxisFor(q.x, span.x), ay = AxisFor(q.y, span.y);
    const int taps = max(AxisTaps(span.x), AxisTaps(span.y));
    float2 c;
    [branch] if (taps <= 2) c = AreaChroma2(ax, ay);
    else [branch] if (taps == 3) c = AreaChroma3(ax, ay);
    else [branch] if (taps == 4) c = AreaChroma4(ax, ay);
    else c = AreaChroma5(ax, ay);
    return c;
}
float NeuralAt(int2 texel) { return neural.Load(int3(clamp(texel, int2(0, 0), int2(2 * (right - left), 2 * (bottom - top)) - 1), 0)); }
float NeuralLuma(float2 q) {
    float2 n = q * 2.0; // Position in the 2x image, whose origin is the crop's (texel centres at integer + 0.5).
    float2 b = floor(n - 0.5), f = n - 0.5 - b; int2 i = int2(b);
    return lerp(lerp(NeuralAt(i), NeuralAt(i + int2(1, 0)), f.x), lerp(NeuralAt(i + int2(0, 1)), NeuralAt(i + int2(1, 1)), f.x), f.y);
}
float2 BilinearChroma(float2 q) {
    float2 b = floor(q - 0.5), f = q - 0.5 - b; int2 i = int2(b) + ChromaLow();
    float2 a = ChromaAt(i), c = ChromaAt(i + int2(1, 0)), d = ChromaAt(i + int2(0, 1)), e = ChromaAt(i + int2(1, 1));
    return lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y);
}
static const float kGuideEpsilon = 1e-3; // In summed squared UNORM luma over the four sites: about a 4-code contrast.
// The guided fit: chroma `c` and guide luma `Y` at the four sites, fractions `f`, reconstructed luma `y`.
float2 GuideFit(float2 c[4], float Y[4], float2 f, float y) {
    const float meanY = 0.25 * (Y[0] + Y[1] + Y[2] + Y[3]);
    const float2 meanC = 0.25 * (c[0] + c[1] + c[2] + c[3]);
    float variance = 0; float2 covariance = 0;
    [unroll] for (int k2 = 0; k2 < 4; ++k2) { const float d = Y[k2] - meanY; variance += d * d; covariance += d * (c[k2] - meanC); }
    const float2 slope = covariance / (variance + kGuideEpsilon);
    const float2 bilinear = lerp(lerp(c[0], c[1], f.x), lerp(c[2], c[3], f.x), f.y);
    const float lowY = lerp(lerp(Y[0], Y[1], f.x), lerp(Y[2], Y[3], f.x), f.y);
    const float2 lo = min(min(c[0], c[1]), min(c[2], c[3])), hi = max(max(c[0], c[1]), max(c[2], c[3]));
    return clamp(bilinear + slope * (y - lowY), lo, hi);
}
// Chroma at chroma-plane position `q`, guided by `y`, the reconstructed luma at the same output position.
float2 GuidedChroma(float2 q, float y, bool interior) {
    float2 b = floor(q - 0.5), f = q - 0.5 - b; int2 i = int2(b) + ChromaLow();
    const int2 low = ChromaFloor(), high = ChromaCeiling();
    const int2 sites[4] = {clamp(i, low, high), clamp(i + int2(1, 0), low, high), clamp(i + int2(0, 1), low, high), clamp(i + int2(1, 1), low, high)};
    float2 c[4]; float Y[4];
    [unroll] for (int k = 0; k < 4; ++k) c[k] = chroma.Load(int3(sites[k], 0));
    // Chroma texel s covers luma texels 2s and 2s + 1 in each axis. At an odd crop edge one of each pair is padding:
    // the clamped quad then repeats the picture's column or row, so the guide is the mean of picture luma only.
    // The fit is inside each branch, so only its result crosses the join (as in EdgeDirectedLuma).
    float2 result;
    [branch] if (interior) {
        [unroll] for (int g = 0; g < 4; ++g) { const float4 quad = GatheredQuad(2 * sites[g]); Y[g] = 0.25 * (quad.x + quad.y + quad.z + quad.w); }
        result = GuideFit(c, Y, f, y);
    } else {
        [unroll] for (int l = 0; l < 4; ++l) { const float4 quad = ClampedQuad(2 * sites[l]); Y[l] = 0.25 * (quad.x + quad.y + quad.z + quad.w); }
        result = GuideFit(c, Y, f, y);
    }
    return result;
}
// Lanczos-2 approximated on the squared radius x = r^2: a quadratic lobe (25/16 (2x/5 - 1)^2 - 9/16) times the
// window (x/4 - 1)^2, which factorises to (x - 1)(x/4 - 1)^3. It is 1 at r = 0, zero at r = 1, has one negative
// lobe, and min(., 0) makes it exactly zero from r = 2 on.
float Lanczos2(float x) { float w = min(0.25 * x - 1.0, 0.0); return (x - 1.0) * w * w * w; }
// The edge-directed kernel on a fetched 4x4 block `L` ([row][column]) with the output at fractions `f` of its centre.
float EdgeKernel(float L[4][4], float2 f) {
    // Structure tensor from the four samples surrounding q: (1,1) (2,1) (1,2) (2,2) in the block.
    float weights[4] = {(1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y};
    float txx = 0, txy = 0, tyy = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        int x = 1 + (k & 1), y = 1 + (k >> 1);
        float gx = 0.5 * (L[y][x + 1] - L[y][x - 1]), gy = 0.5 * (L[y + 1][x] - L[y - 1][x]);
        txx += weights[k] * gx * gx; txy += weights[k] * gx * gy; tyy += weights[k] * gy * gy;
    }
    float difference = txx - tyy, root = sqrt(difference * difference + 4 * txy * txy), energy = txx + tyy;
    // Gradient normal: eigenvector of the larger eigenvalue (both forms, whichever is well conditioned).
    float2 normal = difference >= 0 ? float2(difference + root, 2 * txy) : float2(2 * txy, root - difference);
    normal = dot(normal, normal) > 1e-20 ? normalize(normal) : float2(1, 0);
    float2 tangent = float2(-normal.y, normal.x);
    // Anisotropy: coherent (line-like) structure with a gradient above ~1% of full scale per pixel.
    float edge = (root / (energy + 1e-12)) * smoothstep(1e-4, 1e-3, energy);
    float along = 1.0 + edge; // Up to two source pixels along the edge; unit width across it.
    // Elliptical radius r^2 = (d.t / along)^2 + (d.n)^2 = d^T M d with M = n n^T + t t^T / along^2, expanded into
    // per-column and per-row terms of d = (tx - 1 - f.x, ty - 1 - f.y) so each tap costs one multiply-add.
    const float stretch = 1.0 / (along * along);
    const float m00 = normal.x * normal.x + tangent.x * tangent.x * stretch;
    const float m01 = normal.x * normal.y + tangent.x * tangent.y * stretch;
    const float m11 = normal.y * normal.y + tangent.y * tangent.y * stretch;
    float column[4], cross[4], row[4], dy[4];
    [unroll] for (int i = 0; i < 4; ++i) {
        float dx = i - 1 - f.x; dy[i] = i - 1 - f.y;
        column[i] = m00 * dx * dx; cross[i] = 2.0 * m01 * dx; row[i] = m11 * dy[i] * dy[i];
    }
    float sum = 0, total = 0;
    [unroll] for (int ty = 0; ty < 4; ++ty) [unroll] for (int tx = 0; tx < 4; ++tx) {
        float w = Lanczos2(column[tx] + row[ty] + cross[tx] * dy[ty]);
        sum += w * L[ty][tx]; total += w;
    }
    float lo = min(min(L[1][1], L[1][2]), min(L[2][1], L[2][2]));
    float hi = max(max(L[1][1], L[1][2]), max(L[2][1], L[2][2]));
    return clamp(sum / max(total, 1e-4), lo, hi); // Deringing: never beyond the four surrounding samples.
}
float EdgeDirectedLuma(float2 q, bool interior) {
    float2 b = floor(q - 0.5), f = q - 0.5 - b;
    int2 o = int2(b) - 1 + LumaLow(); // The 4x4 block's first texel.
    float L[4][4];
    // The 4x4 block, clamped to the crop: four Gathers (x = (0,1), y = (1,1), z = (1,0), w = (0,0)) where the group is
    // inside the crop, sixteen clamped loads where it touches the border, so that the block repeats the picture's
    // edge texels exactly as it would at a texture's edge. The kernel is evaluated inside each branch: joining the
    // sixteen values after the branch instead cost 10 % of the pass on the reference GPU.
    float result;
    [branch] if (interior) {
        [unroll] for (int gy = 0; gy < 2; ++gy) [unroll] for (int gx = 0; gx < 2; ++gx) {
            float4 quad = GatheredQuad(o + int2(gx * 2, gy * 2));
            L[gy * 2][gx * 2] = quad.w; L[gy * 2][gx * 2 + 1] = quad.z; L[gy * 2 + 1][gx * 2] = quad.x; L[gy * 2 + 1][gx * 2 + 1] = quad.y;
        }
        result = EdgeKernel(L, f);
    } else {
        [unroll] for (int ly = 0; ly < 4; ++ly) [unroll] for (int lx = 0; lx < 4; ++lx) L[ly][lx] = LumaAt(o + int2(lx, ly));
        result = EdgeKernel(L, f);
    }
    return result;
}
// mode: 0 bilinear, 1 edge-directed, 2 neural, 3 area (downscale prefilter: luma, and chroma unless it is guided).
float3 Reconstruct(int2 output, uint mode, bool guided, bool interior) {
    float2 q = SourcePosition(output);
    float y;
    // Real branches: HLSL's ?: evaluates both operands, which would pay for the edge-directed kernel everywhere.
    [branch] if (mode == 1) y = EdgeDirectedLuma(q, interior);
    else [branch] if (mode == 2) y = NeuralLuma(q);
    else [branch] if (mode == 3) y = AreaLuma(q, Scale());
    else y = BilinearLuma(q);
    // Chroma at the same normalized position on the half-resolution plane.
    float2 c;
    [branch] if (guided) c = GuidedChroma(ChromaPosition(q), y, interior);
    else [branch] if (mode == 3) c = AreaChroma(ChromaPosition(q), 0.5 * Scale());
    else c = BilinearChroma(ChromaPosition(q));
    return Convert(y, c);
}
// The 8-bit output, dithered for 10-bit and HDR sources when asked (interleaved gradient noise: static, no texture).
float3 Dithered(float3 color, int2 p) {
    const bool dither = (filterFlags & 2) != 0 && (tenBit != 0 || transfer == 16 || transfer == 18); // Uniform per dispatch.
    const float noise = frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715))));
    // Full amplitude from half an LSB inside the range on: black and white (and anything within half an LSB of them,
    // such as a white that converts to 0.9999999) are never pushed across a rounding boundary.
    const float3 room = saturate(min(color, 1.0 - color) * 510.0);
    return dither ? color + (noise - 0.5) / 255.0 * room : color;
}
// Split comparison: the pixel as shown. `enhanced` is the configured conversion of `p`; the plain one is reconstructed
// here for the left half (the column of groups across the seam is the only one that pays for both).
static const uint kCompare = 4;
float3 Compared(int2 p, float3 enhanced, bool interior) {
    const int seam = int(width / 2);
    float3 shown = enhanced;
    [branch] if (p.x < seam) shown = Reconstruct(p, 0, false, interior) * (p.x == seam - 1 ? 0.25 : 1.0); // Dark side of the seam.
    else if (p.x == seam) shown = lerp(enhanced, 1.0, 0.75);                                            // Light side.
    return shown;
}
#ifdef PICTURE_CONTROLS
// Micro-motion: the pixel's change against its reference, magnified (see the header); updates the reference.
float MotionAt(int2 q) { return motion[microRead + uint(q.y) * width + uint(q.x)]; }
float3 Magnify(float3 color, int2 p) {
    const uint index = uint(p.y) * width + uint(p.x);
    const float y = dot(color, kLuma709);
    bool fresh = (pictureFlags & kMicroStart) != 0;
    float reference = y, gate = 1.0;
    [branch] if (!fresh && (pictureFlags & kMicroWarp) != 0) {
        // Where this pixel's content was in the previous picture's output (pixel coordinates), between pixels.
        const float2 centre = float2(p) + 0.5, last = float2(width, height) - 1.0;
        const float2 at = float2(asfloat(microM0) * centre.x + asfloat(microM1) * centre.y + asfloat(microM2),
                                 asfloat(microM3) * centre.x + asfloat(microM4) * centre.y + asfloat(microM5)) - 0.5;
        if (any(at < -0.5) || any(at > last + 0.5)) fresh = true; // It came from outside the previous view.
        else {
            const float2 q = clamp(at, 0.0, last), f = q - floor(q);
            const int2 b = int2(floor(q)), b1 = min(b + 1, int2(last));
            const float r00 = MotionAt(b), r10 = MotionAt(int2(b1.x, b.y)), r01 = MotionAt(int2(b.x, b1.y)), r11 = MotionAt(b1);
            reference = lerp(lerp(r00, r10, f.x), lerp(r01, r11, f.x), f.y);
            const float range = max(max(r00, r10), max(r01, r11)) - min(min(r00, r10), min(r01, r11));
            gate = saturate((2.0 * kMicroEdge - range) / kMicroEdge);
        }
    } else if (!fresh) reference = motion[microRead + index];
    if (fresh) reference = y;
    const float d = y - reference, a = abs(d);
    const float w = a > kMicroLarge ? 0.0 : gate * saturate((kMicroLarge - a) / (kMicroLarge - kMicroSmall)) * saturate(2.0 * a / kMicroNoise - 1.0);
    if ((pictureFlags & (kMicroFold | kMicroStart)) != 0) motion[microWrite + index] = a > kMicroLarge ? y : reference + kMicroFollow * d;
    float3 c = color; // Still, or moving (w = 0): the conversion as it was, bit for bit.
    [branch] if (w > 0.0) {
        const float lifted = (asfloat(microBits) - 1.0) * w * d;
        c = color + lifted;
        // Out of range: toward the new luma, hue kept (a dark pixel pulled further down cannot go below black either).
        const float target = saturate(y + lifted), high = max(c.r, max(c.g, c.b)), low = min(c.r, min(c.g, c.b));
        if (high > 1.0) c = target + (c - target) * (1.0 - target) / max(high - target, 1e-6);
        if (low < 0.0) c = target + (c - target) * target / max(target - low, 1e-6);
        c = saturate(c);
    }
    return c;
}
float3 Grade(float3 c) {
    const float lift = asfloat(liftBits), contrast = asfloat(contrastBits), brightness = asfloat(brightnessBits);
    const float gamma = asfloat(gammaBits), saturation = asfloat(saturationBits);
    [branch] if (lift > 0.0) {
        const float y = dot(c, kLuma709);
        if (y < 0.75) {
            const float u = 1.0 - y / 0.75, delta = 2.0 * lift * y * u * u, target = y + delta;
            c += delta;
            const float high = max(c.r, max(c.g, c.b));
            if (high > 1.0) c = target + (c - target) * (1.0 - target) / max(high - target, 1e-6);
        }
    }
    if (contrast != 1.0 || brightness != 0.0) c = (c - 0.5) * contrast + 0.5 + brightness;
    if (gamma != 1.0) c = pow(saturate(c), 1.0 / gamma);
    if (saturation != 1.0) { const float y = dot(c, kLuma709); c = y + (c - y) * saturation; }
    return saturate(c);
}
#endif
// What every output pixel goes through last, before the dither: micro-motion, then the picture controls. Compiled only into
// the PICTURE_CONTROLS variants (ColorPipeline chooses them when picture_flags is not 0): in the production shader their code
// alone cost 0.65-1.4 ms per 1080p conversion on the reference GPU, with the flags off (register allocation), as the split
// comparison's did.
float3 Finish(float3 color, int2 p) {
#ifdef PICTURE_CONTROLS
    [branch] if ((pictureFlags & kMicro) != 0) color = Magnify(color, p);
    [branch] if ((pictureFlags & kGrade) != 0) color = Grade(color);
#endif
    return color;
}
// The pixel as written: black outside the crop with kBars (a turned view only: PICTURE_CONTROLS).
float3 Barred(float3 color, int2 p) {
#ifdef PICTURE_CONTROLS
    [branch] if ((filterFlags & kBars) != 0 && Turned()) {
        const float2 q = SourcePosition(p);
        if (any(q < 0.0) || any(q > float2(right - left, bottom - top))) color = float3(0, 0, 0);
    }
#endif
    return color;
}
float Luminance(float3 encoded) { return pow(saturate(dot(encoded, float3(0.2126, 0.7152, 0.0722))), 2.4); } // Linear light.
float SharpenedLuminance(uint2 p) { // p: halo coordinates of the centre, 1..Tile.
    float c = haloLight[p.y][p.x];
    float n = haloLight[p.y - 1][p.x], s = haloLight[p.y + 1][p.x], w = haloLight[p.y][p.x - 1], e = haloLight[p.y][p.x + 1];
    float lo = min(min(min(c, n), min(s, w)), e), hi = max(max(max(c, n), max(s, w)), e);
    // Local contrast, measured perceptually (square root of linear luminance) so dark and bright areas compare.
    float range = sqrt(hi) - sqrt(lo);
    float noise = asfloat(noiseBits);
    float gain = 1.5 * asfloat(sharpnessBits) * smoothstep(noise, 2.0 * noise, range) * saturate(1.0 - range);
    return clamp(c + gain * (c - 0.25 * (n + s + w + e)), lo, hi);
}
[numthreads(Tile, Tile, 1)]
void CS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint index = thread.y * Tile + thread.x;
    const int2 origin = int2(group.xy * Tile), output = origin + int2(thread.xy), last = int2(width, height) - 1;
    const float2 scale = Scale();
    // Uniform per dispatch, so the barrier below stays in uniform control flow.
    const bool upscaling = scale.x <= 1.0 && scale.y <= 1.0 && (scale.x < 1.0 || scale.y < 1.0);
    // Up to two source texels per output pixel, bilinear's two-texel support already covers the footprint (at exactly 2x
    // it is the 2x2 box itself, bit for bit), so the prefilter starts above that.
    const bool downscaling = scale.x > 2.0 || scale.y > 2.0;
    const uint mode = upscaling && (reconstruction == 1 || reconstruction == 2) ? reconstruction : downscaling && (filterFlags & 1) != 0 ? 3 : 0;
    const bool guided = (chromaMode & 1) != 0 && (scale.x < 2.0 || scale.y < 2.0); // Chroma upsampled in some axis.
    const bool interior = TileInterior(origin, scale); // Uniform per group.
#ifdef SPLIT_COMPARE
    const bool compare = (filterFlags & kCompare) != 0;
#else
    const bool compare = false; // The production variant: the comparison's code is not there at all.
#endif
    // A group wholly in the plain half (left of the seam's two columns): the plain conversion alone, no sharpening.
    [branch] if (compare && origin.x + int(Tile) < int(width / 2)) {
        if (all(output <= last)) rgb[output] = Written(Barred(Dithered(Finish(Reconstruct(output, 0, false, interior), output), output), output));
        return;
    }
    const bool across = compare && origin.x < int(width / 2) + 1; // The column of groups the seam crosses (uniform).
    const float3 color = Reconstruct(min(output, last), mode, guided, interior);
    [branch] if (asfloat(sharpnessBits) <= 0.0) {
        if (all(output <= last)) rgb[output] = Written(Barred(Dithered(Finish(across ? Compared(output, color, interior) : color, output), output), output));
        return;
    }
    // Linear luminance of the tile (each thread's own output) and of its 68-pixel ring (first 68 threads).
    haloLight[thread.y + 1][thread.x + 1] = Luminance(color);
    if (index < 4 * Halo - 4) {
        int2 ring = index < Halo ? int2(index, 0) : index < 2 * Halo ? int2(index - Halo, Halo - 1) :
                    index < 3 * Halo - 2 ? int2(0, index - 2 * Halo + 1) : int2(Halo - 1, index - 3 * Halo + 3);
        haloLight[ring.y][ring.x] = Luminance(Reconstruct(clamp(origin - 1 + ring, int2(0, 0), last), mode, guided, interior));
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(output <= last)) {
        float before = haloLight[thread.y + 1][thread.x + 1], after = SharpenedLuminance(thread.xy + 1);
        // Hue-preserving: the colour is scaled by the luminance ratio, re-encoded for the display.
        float ratio = before > 1e-6 ? pow(max(after / before, 0.0), 1.0 / 2.4) : 1.0;
        const float3 sharpened = saturate(color * ratio);
        rgb[output] = Written(Barred(Dithered(Finish(across ? Compared(output, sharpened, interior) : sharpened, output), output), output));
    }
}
