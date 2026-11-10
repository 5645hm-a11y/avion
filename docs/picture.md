# Picture: edge-directed reconstruction and contrast-adaptive sharpening

The colour-conversion compute pass (`render::ColorPipeline`: `Nv12ToRgb.hlsl` for SDR, `HdrToneMap.hlsl` for PQ/HLG) now shares a spatial stage, `src/render/Reconstruct.hlsli`, fused into the same dispatch. It decides how each output pixel's source sample is reconstructed and optionally sharpens the result.
- Everything stays in VRAM: decoded planes are read by the conversion shader, and the result is written to the renderer's RGB slot.
- There are no new resources or descriptors, and no CPU work beyond root constants.

## Parameters

`ColorConstants` grew from 16 to 20 root constants (80 bytes). Zero-filled trailing fields reproduce the original conversion, which is how the tone-mapping reference test still runs.

| Field | Meaning | Renderer default |
|---|---|---|
| `reconstruction` | `kBilinear` (0), `kEdgeDirected` (1): edge-directed luma while upscaling, or `kNeural` (2): luma from the neural 2x pass while upscaling (see [neural](neural.md); the renderer selects it when N is on) | `kEdgeDirected` |
| `sharpness` | Contrast-adaptive sharpening strength, 0 (off) to 1 | 0 (opt-in) |
| `noise_floor` | Local contrast (in √luminance) below which sharpening fades out | 0.02 |
| `chroma_mode` | `kBilinearChroma` (0) or `kGuidedChroma` (1): luma-guided 4:2:0 chroma upsampling (the former `reserved` word) | `kGuidedChroma` (**C** toggles) |

`D3D12Renderer::SetPicture(edge_directed, sharpness)`, `SetGuidedChroma(enabled)` and `PlaybackOptions::{edge_directed, sharpness}` expose these. Sharpening is opt-in because of its cost on the reference GPU (see [Performance](#performance)).

## Algorithms

All formulations are my own; no third-party code or shaders are used.

**Source access.** Luma and chroma are read with point loads, and the bilinear arithmetic is done in the shader, clamped to the crop rectangle. The edge-directed path fetches its 4×4 luma block with four `GatherRed`s.
- On the reference GPU (Intel UHD 630), filtered sampling of the planar NV12/P010 views is about 3× slower than point loads.
- Groupshared staging is slower still, because scattered shared-memory access costs a data-port message per value there.
- This change alone makes the existing bilinear conversion 2.3× faster.

**Edge-directed reconstruction** applies only while upscaling (source pixels per output pixel ≤ 1 in both axes, < 1 in at least one).
1. **Structure tensor.** At the output position, central-difference gradients of the four surrounding luma samples, bilinearly weighted, form a structure tensor. Its dominant eigenvector is the gradient normal *n*; the edge tangent *t* is perpendicular to it. Coherence is (λ₁ − λ₂)/(λ₁ + λ₂), gated by gradient energy above about 1 % of full scale per pixel.
2. **Kernel.** An elliptical Lanczos-2-shaped kernel runs over the 4×4 block.
   - It has unit width across the edge and is stretched up to 2× along it, in proportion to coherence. Averaging along the edge removes staircasing; the unit width across it keeps the edge crisp.
   - The kernel is L(x) = (x − 1)·min(x/4 − 1, 0)³ with x = r²: the product of a quadratic lobe and the (x/4 − 1)² window. It is 1 at r = 0, 0 at r = 1, has one negative lobe, and is exactly 0 from r = 2.
   - The rotated and stretched radius is evaluated as a quadratic form, r² = dᵀMd, with per-row and per-column terms precomputed.
3. **Deringing.** The result is clamped to the min/max of the four surrounding samples, so the negative lobes add sharpness but cannot ring.

At 1:1 and when downscaling, luma is bilinear: the kernel does not interpolate on texel centres. Above 2× downscaling the [area prefilter](#downscale-prefilter-and-dither-2026-10-16) replaces it. Chroma is reconstructed separately (next).

**Luma-guided chroma** applies whenever chroma is upsampled: fewer than two source pixels per output pixel in either axis, which includes 1:1. Bilinear 4:2:0 chroma spreads every colour edge over a chroma texel, two luma pixels, even when the luma edge is sharp; that is the colour bleeding visible along saturated edges.
1. **Guide samples.** For each of the four chroma samples bilinear interpolation would use, luma at the chroma site is the mean of the 2×2 luma quad it covers (one `GatherRed` each).
2. **Local linear model.** Over those four sites, chroma is regressed on luma, `c ≈ mean(c) + a·(Y − mean(Y))`, as in a guided filter: `a = Σ ΔY·Δc / (Σ ΔY² + ε)` per chroma component. ε = 10⁻³ (UNORM², summed over the four sites, about a 4-code luma contrast) keeps luma noise from driving it.
3. **Detail transfer.** The output adds `a` times the high-frequency luma residual, the reconstructed luma at the output (bilinear, edge-directed or neural) minus the chroma-resolution luma interpolated like the chroma, to bilinear chroma.
4. **Bounds.** The result is clamped to the range of the four chroma samples: it cannot overshoot, only move the transition to where the luma edge is.

Where luma is flat, as on an isoluminant colour edge, `a = 0` and the result is exactly bilinear. The neural path benefits too, since its sharper 2× luma is the guide.

**Contrast-adaptive sharpening** works on linear-light luminance.
1. Each 16×16 group converts its tile plus a one-pixel ring. It keeps linear luminance, (display luma)^2.4, in groupshared memory: one float per pixel, the only shared-memory use.
2. The high-pass is `c − mean(4-neighbour cross)`.
3. The gain is `1.5 · sharpness · smoothstep(noise, 2·noise, range) · (1 − range)`, where `range` is the √-luminance range of the cross.
   - Where local contrast is already high, the gain falls toward zero, so sharp edges are not over-sharpened.
   - Below the noise floor it fades out, so flat, noisy areas are not amplified.
4. The sharpened luminance is clamped to the cross's range, so it cannot overshoot its neighbours: no halos or ringing by construction.
5. The result is applied to the pixel's colour as a luminance ratio, re-encoded for the display, which preserves hue.

## Measured quality

`media_upscale_tests` converts synthetic 96×96 NV12 fixtures (test input, not decoded video) with the production pipeline at 3× upscale. A test-only shader (`tests/EdgeMetrics.hlsl`) measures the output and returns only scalars.
- **Fixtures:** a dark/bright step at 20° from vertical, point-sampled (aliased) or 16×16 supersampled (antialiased), and a flat field with ±2-code noise.
- **Staircase:** the sub-pixel 50 % crossing in each output row is compared with the true line; the table gives the RMS about the mean.
- **10–90 % width:** the number of transition-band pixels per row.
- **Overshoot:** values beyond the two levels.

Units are source pixels for staircase and width, 8-bit steps for overshoot.

| Fixture | Mode | Staircase RMS | 10–90 % width | Overshoot |
|---|---|---|---|---|
| Aliased edge | bilinear | 0.232 | 0.911 | 0 |
| | edge-directed | **0.142** (−39 %) | 0.696 | 0 |
| | + sharpening 0.5 | 0.143 | 0.615 | 0 |
| Antialiased edge | bilinear | 0.061 | 1.356 | 0 |
| | edge-directed | **0.020** (−67 %) | 0.921 | 0 |
| | + sharpening 1.0 | 0.030 | **0.706** | 0 |

**Flat field, ±2-code noise:** mean absolute deviation is 1.601 steps unsharpened and 1.601 with sharpening at full strength, where the noise floor fully gates it. Without the floor it rises to 1.774.

At 1:1, edge-directed mode is exactly bilinear. The tone-mapping reference test (`media_tonemap_tests`) is unchanged: at most 1 step, mean error 0.001–0.002.

**Chroma.** Three 96×96 NV12 fixtures carry real chroma: a vertical colour edge between source columns 44 and 45, subsampled to 4:2:0 by averaging column pairs, so the edge falls inside a chroma texel as it generally does in video. Each output pixel's RGB is inverted to Cb/Cr (BT.709 is a pure matrix here) and compared with the true 4:4:4 edge over the middle rows.
- **Bleeding width:** pixels per row more than 10 % of the colour step away from the true colour, in source pixels.
- **Error:** the mean chroma distance, in code steps.

| Fixture | Output | Bilinear chroma: width / error | Guided chroma: width / error | Guided overshoot |
|---|---|---|---|---|
| Coincident luma and colour edge (dark red / light blue-grey) | 1:1 | 4.00 px / 1.10 | **0.00 px / 0.19** | 0.29 |
| | 3× (edge-directed luma) | 3.33 px / 1.10 | **0.67 px / 0.36** | 0.29 |
| Isoluminant colour edge | 1:1 and 3× | unchanged: the output is bit-identical to bilinear | | 0.19 |
| Flat colour | 1:1 and 3× | unchanged: bit-identical | | 0.30 |

Overshoot is the chroma beyond the two colours, in code steps; the values are within the RGB 8-bit rounding of the inversion.

## Performance

GPU timestamps around the conversion dispatch alone: 1280×720 NV12 of random content (the worst case for the edge detector), Intel UHD 630, Release, after a warm-up burst, median of 12. The previous sampler-based pass took 4.6 ms at 1080p and 2.05 ms at 720p.

| Output | Mode | Time |
|---|---|---|
| 1920×1080 (1.5× upscale) | bilinear (point loads) | 2.0 ms |
| | edge-directed | 4.9 ms |
| | edge-directed + sharpening | 8.0 ms |
| 1280×720 (1:1) | bilinear | 0.88 ms |
| | bilinear + sharpening | 1.73 ms |

Guided chroma, measured in the same run as its bilinear-chroma counterpart (best of 12):

| Output | Luma mode | Bilinear chroma | Guided chroma |
|---|---|---|---|
| 1920×1080 (1.5×) | bilinear | 2.23 ms | 3.24 ms (+1.0) |
| | edge-directed | 4.98 ms | 6.10 ms (+1.1) |
| 1280×720 (1:1) | bilinear | 0.99 ms | 1.45 ms (+0.46) |

It costs four luma `GatherRed`s and a 2×2 regression per output pixel. It is on by default, because it is the only fix for colour bleeding in 4:2:0 video and fits the 24–30 fps budget; at 60 fps on this GPU it is one of the stages to switch off first (**C**).

**The 1.0 ms target is not met at 1080p output on this GPU, and cannot be met by a full-resolution conversion here.**
- An isolation experiment showed that merely writing a 1920×1080 RGBA8 UAV costs about 0.6 ms.
- The fastest bilinear conversion is about 2 ms.
- The edge-directed kernel's arithmetic, about 200 operations per output pixel, costs about 2.6 ms more on this 24-EU GPU. Fetching its 4×4 block costs nothing extra, as another isolation experiment showed.
- The target is met for plain conversion up to 720p output.

**What I tried, and why the design is what it is:**

| Change | 1080p result | Status |
|---|---|---|
| Shared-memory staging of source texels | slower than the original sampler pass (8–12 ms), because of scattered shared-memory access on Gen9.5 | removed |
| 16 point loads for the 4×4 block | 6.4 ms edge-directed | replaced |
| Four `GatherRed` fetches for the 4×4 block | 4.9 ms edge-directed | kept |
| 12-tap kernel (corners dropped) | saves 0.7 ms, but worsens edge width (0.92 → 0.98) and staircase (0.020 → 0.027) | rejected |
| Kernel algebra (factorisation, quadratic form) | identical quality metrics to the unfactorised kernel, but no measurable speedup: the cost is not simply the tap loop | kept (identical output metrics) |
| 16×16 tiles instead of 8×8 | halves the sharpening ring overhead (68 extra reconstructions per 256 outputs instead of 36 per 64) | kept |

**Defaults.** Edge-directed reconstruction is on, because it costs nothing unless the window is larger than the video. Sharpening is opt-in. At 24–30 fps, a 1080p output with both stages (8 ms) fits the frame budget, but not at 60 fps alongside decode on this GPU.

The test asserts same-run regression guards on the fastest of 12 runs, rather than absolute budgets. GPU clocks vary by machine and power state, and interference from the compositor or other GPU work only adds time: one full-suite run saw the sharpening median jump to 11.6 ms while its fastest run stayed at 8.1 ms. The guards are:
- bilinear at 1080p under 4 ms (well below the old 4.6 ms);
- edge-directed under 3× bilinear;
- edge-directed with sharpening under 5× bilinear;
- 720p sharpening under 3× plain 720p;
- guided chroma under 2.5× bilinear at 1080p and at 1:1 720p, and edge-directed with guided chroma under 1.8× edge-directed.

## Limits

- Chroma follows luma only where they vary together. A colour edge with no luma contrast stays bilinear, which is the correct fallback, since without luma there is no guide.
- Guided chroma assumes centre chroma siting, like the rest of the conversion. MPEG-2/H.264 default (left) siting is not modelled, a quarter-chroma-pixel shift.
- ~~The edge-directed gathers clamp at the texture's edges, not the crop rectangle.~~ Fixed on 2026-09-30, and the effect was larger than this note said: see [Crop, sample aspect ratio and colour description](#crop-sample-aspect-ratio-and-colour-description-2026-09-30).
- ~~Downscaling has no prefilter (bilinear, as before).~~ Above 2× it averages each output pixel's footprint since 2026-10-16: see [Downscale prefilter and dither](#downscale-prefilter-and-dither-2026-10-16).
- Sharpening strength is global; there is no per-content adaptation beyond the local contrast gate.

## Crop, sample aspect ratio and colour description (2026-09-30)

Directive: "METADATA & CROP CLAMPING (P0)". Three things a picture carries besides its samples: which samples are picture (the crop), what shape one sample is (the sample aspect ratio), and how its code values map to colour (the colour description).

### Crop: nothing outside it reaches the screen

A decoded surface is whole macroblocks (H.264, 16 px) or coding blocks (HEVC). The conformance window, the crop, is the part that is picture; the rest is padding whose content the decoder does not define (1080p is 1088 rows coded).

**What was wrong.** Point loads were clamped to the crop, but three paths were not:
- the edge-directed kernel's four `GatherRed` fetches of its 4x4 block;
- luma-guided chroma's `GatherRed` of the 2x2 luma quad under each chroma site;
- the scaled presentation blit (`PictureBlit.hlsl`), used after a resize, which samples an RGB slot with bilinear filtering and read past the picture into what an earlier, larger picture left in the slot.

`media_aspect_ratio_crop_tests` measured it before the fix, converting the same picture with the padding set to three different poisons. Output changed by up to 203 of 255 (edge-directed), 236 (guided chroma, the default), 255 (HDR tone map) and 164 (scaled blit): with default settings, padding reached the screen along crop edges.

**Fix** (`Reconstruct.hlsli`, `PictureBlit.hlsl`):
- Every 2x2 fetch clamps to the crop. A Gather is used where the whole 16x16 output tile's footprint lies inside the crop (`TileInterior`, decided once per group, so the branch is the same for every thread); elsewhere sixteen or four clamped loads replace it. The kernels are evaluated inside each branch: joining their inputs after the branch cost 10 % on the reference GPU.
- Positions are computed from the crop's origin, and the origin is added to integer texel indices. A crop therefore converts bit for bit like the same picture in a surface of its own size: its edges behave exactly as a texture's edges. For 4:2:0 an odd crop origin is half a chroma texel, the only part of the offset that enters the fractions (`ChromaPosition`).
- A chroma texel counts as picture when either luma column (row) it covers does (`video::ChromaBounds`). At an odd crop edge the guide luma under such a texel is the picture's own column, repeated.
- The blit clamps sampling to the centre of the picture's last texel (`render::MakeBlitConstants`, 4 root constants instead of 2).

**Result.** 490 conversions (5 crops with odd and even origins and sizes, 7 conversion modes including neural and HDR, 7 scales from 1/2 to 3x and anamorphic) are bit-identical whatever the padding holds. A crop at (4, 6) in a poisoned 112x80 surface converts bit-identically to the same 100x66 picture alone. The blit test is bit-identical for two different slot contents.

**Cost** (Intel UHD 630, 720p source, best of 12, the unsafe and the safe shader measured alternately in one session):

| Output | Mode | Gathers without clamping | Crop-exact |
| --- | --- | --- | --- |
| 1920x1080 | bilinear | 2.23 ms | 2.34 ms |
| | edge-directed | 4.91 ms | 5.06 ms |
| | edge-directed + sharpening | 8.30 ms | 8.48 ms |
| | bilinear + guided chroma | 3.22 ms | 3.42 ms |
| | edge-directed + guided chroma | 6.06 ms | 6.23 ms |
| 1280x720 | 1:1 | 1.00 ms | 1.03 ms |
| | 1:1 + guided chroma | 1.43 ms | 1.51 ms |

A first version that decided per fetch cost 22 % on edge-directed and doubled guided chroma's cost; the per-tile decision is 3 to 6 %. Quality metrics in `media_upscale_tests` are unchanged, and the tone-mapping reference still agrees within 1 step.

**4:2:0 crops are always even.** H.264 and HEVC signal cropping in units of two luma samples for 4:2:0, so a real stream never has an odd crop. The odd crops in the test exercise the shaders' arithmetic, which accepts any crop.

### Sample aspect ratio: anamorphic video is shown undistorted

**Sources**, in `video::PictureGeometry.hpp`:
- the VUI's `aspect_ratio_idc` in H.264 (Table E-1) and HEVC (Table E.1), or its explicit pair (`EXTENDED_SAR`). A zero term means the stream does not say (E.2.1), which the H.264 parser used to reject as an error;
- the MP4 sample entry's `pasp` box (`TrackInfo::pixel_aspect_width/height`).

**Which one** (`ResolveSampleAspect`): a `pasp` that says something other than 1:1 wins, since it describes the track as authored and remuxers correct streams there. A 1:1 `pasp`, which muxers write when they were told nothing, lets the VUI speak. With neither, samples are square. A ratio beyond 8:1 either way is taken for damage and treated as square.

**Display size** (`DisplaySize`): the crop in square pixels. No sample is thrown away: wide samples widen the picture and tall samples make it taller. So 720x576 at 16:11 becomes 1047x576, 720x480 at 10:11 becomes 720x528, and 1440x1080 at 4:3 becomes 1920x1080. The window's initial shape, the RGB slots' capacity and every picture's viewport (`FitInside`/`Viewport`, which also replace the renderer's own `Fit`) use that shape. The conversion scales each axis on its own, so anamorphic samples are resampled once, in the conversion.

**Only the first description counts.** An in-band SPS repeat that describes the pictures differently (aspect, and for H.264 colour) is accepted without being a new configuration; the description the session started with stands. A mid-stream change of shape is not followed.

### Colour description: all four code points, container over bitstream

**Read.** H.264's VUI used to yield only the matrix and the range flag; primaries and transfer characteristics are now read too. HEVC already read all four. The MP4 `colr` box (`nclx`, or QuickTime's `nclc`, which has no range flag) was read but not used.

**Resolved** (`video::ResolveColor`, used for every picture):
- per code point, the container's value where it gives one, else the bitstream's (ISO/IEC 14496-12: the sample entry's colour information takes precedence); the range flag likewise when the container carries one;
- what is still unspecified is inferred as receivers conventionally do. BT.2020 primaries imply the BT.2020 matrix and the other way round. Pictures taller than 576 lines are BT.709; smaller ones are BT.601, with 625-line primaries at exactly 576 lines and 525-line ones otherwise;
- reserved and out-of-table code points count as unspecified.

**Behaviour change.** An HD stream with no colour description was converted with the BT.601 matrix (the shader's "otherwise" case). It is now BT.709. SD streams are unchanged.

**Exposed.** `PlaybackCoordinator::Picture()` returns the coded size, the crop, the bitstream's and the container's aspect and colour, the resolved ones, and the display size. The demo prints them (`Picture:` and `Colour:` lines). A matrix the shaders do not implement (identity/RGB, YCgCo, constant luminance, ICtCp) still fails preparation at stage 5, now judged on the resolved description.

### Tests

| Test | What it shows |
| --- | --- |
| `media_picture_geometry_tests` (new; MSVC, GCC, ASan/UBSan) | Table E-1 for every idc, explicit pairs, reserved values; normalisation; which description wins; display sizes of 15 formats (PAL/NTSC 4:3 and 16:9, HDV, DVCPRO HD, odd crops, the size limit); viewports with letterbox, pillarbox, the 2-px snap, and a sweep of 750 windows x 4 shapes (inside, touching, centred, shape within the snap); chroma bounds of odd crops; colour resolution (inference by size, container precedence, nclc without range, reserved values, all 23,040 combinations resolve and round-trip); 0 allocations |
| `media_aspect_ratio_crop_tests` (new, GPU) | The viewport arithmetic again; 490 conversions bit-identical under three paddings; crop against the picture alone bit-identical; an anamorphic 101x67 crop at SAR 4:3 shown as 135x67 and in a 480x270 viewport, first and last columns exact and a source edge at the proportional column (66.33 and 237.13, expected 66.33 and 237.12); the scaled blit bit-identical for two slot contents, with exact corner texels |
| `media_codec_tests`, `media_hevc_codec_tests`, `media_demux_tests` (extended; MSVC, GCC, ASan/UBSan) | SAR by idc and explicit pair (zero terms are "unspecified", not errors); the whole H.264 colour description; description changes in an in-band repeat accepted without changing the session; `pasp` and `colr nclc` read, zero-term `pasp` ignored, corruption sweeps |
| `media_make_anamorphic_fixture` + `media_playback_anamorphic_test` (new) | H.264 with a 60x44 crop of 64x64, VUI SAR 4:3 and BT.601, a default 1:1 `pasp` and a BT.709 `colr`: shown as 80x44 in a 640x352 window with no bars, converted as BT.709, 72 pictures, 0 drops, 0 allocations |

### Limits

- The HEVC VUI's default display window and H.264/HEVC chroma sample location are not used.
- The renderer takes one sample aspect ratio for the whole stream (the one at Prepare).
- Primaries are only used to convert BT.2020 to BT.709; BT.470M, SMPTE 240M, P3 and others are converted as if they were BT.709.
- `tkhd` width and height (the track's presentation size, another container hint) are not used.
- Only synthetic surfaces and a handwritten H.264 fixture carry non-square samples here: no real anamorphic file was played.

## Quality governor (2026-10-15)

Directive: "AUTONOMOUS GPU DEADLINE GOVERNOR (Review Task 03 & AI Task A43)". The neural pass costs about 11–20 ms per picture on the UHD 630 depending on resolution. That fits a 24/30 fps interval easily, but not 16.7 ms at 60 fps next to the conversion. The governor picks the most expensive reconstruction that fits, from measured GPU time.

**Tiers** (`render::QualityTier`, most expensive first):

| Tier | Luma while upscaling | Chroma | Sharpening |
| --- | --- | --- | --- |
| Neural | neural 2x ([neural](neural.md)) | guided (the C setting) | the user's |
| EdgeDirected (the earlier default) | edge-directed (`PlaybackOptions::edge_directed`) | guided (the C setting) | the user's |
| Fast | bilinear | bilinear | off |

At 1:1 or when downscaling, luma is bilinear in every tier. Neural then converts as EdgeDirected, and the governor does not offer it.

**Modes** (`render::QualityMode`):
- **Auto** is the default: the governor decides.
- **Neural**, **EdgeDirected** and **Fast** fix the tier. `PlaybackOptions::neural = true` still means fixed Neural.
- The **N** key cycles Auto → Neural → Edge-directed → Fast → Auto, with a status cue such as "Quality · Fast bilinear (manual)" or "Quality · Auto".
- Before 2026-10-15, N toggled Neural on and off.

**Measurement.** Every conversion's command list starts and ends with a D3D12 timestamp query, two per RGB slot, resolved into a mapped readback buffer. The span covers the neural pass and the conversion (and tone mapping), on the presentation queue, where the flips wait behind it.
- When the slot's fence has completed, the span becomes a sample for the tier it was converted at. Samples taken at another tier than the current one are ignored: they were converted before a change took effect.
- The frame interval (the budget) is the picture's duration.
- Without timestamps (`Timing()` false) nothing adapts.

**Rules** (`render::QualityGovernor`; p95 is the nearest-rank 95th percentile of the last 32 samples at the current tier):
- **Down, one tier:** when the p95 exceeds 85 % of the interval, once 20 samples exist. From 20 samples on, the p95 leaves out the single slowest one, so one slow picture after a change, such as a shader warming up, moves nothing. It also steps down at once after 4 consecutive samples over the whole interval.
- **Up, one tier:** only after a full window, at least 1 s at the tier, and a p95 under 55 %. The band between 55 % and 85 % moves nothing either way.
- **No oscillation:**
  - A tier left for overload is not retried for 2 s. The wait doubles with each further failure, up to 64 s, and returns to 2 s once the tier has held for 10 s.
  - A tier measured above 85 % is not retried for 30 s at all.
  - A different picture size or frame interval forgets what was learnt.
- **Cues:** every change the governor makes shows a status cue: "Auto quality · Neural super-resolution" up, "Auto quality · Edge-directed · GPU 37.9 ms" down. Changes and their reasons are kept (`Governor().ChangeAt(i)`).

**Decode is not governed.** The directive asked for decode time in the measured sum. Decoding runs on its own engine, concurrently with the presentation queue, so adding its time to the conversions would describe neither deadline, and no shader tier changes it.
- A decode-queue timing based on timestamps around each `DecodeFrame` was added on 2026-10-15 and removed on 2026-10-16. It was wrong: the decode engine works after the command list's last command has run, so the spans (0.01–0.16 ms) never contained the decode.
- Mapped onto the CPU clock (`GetClockCalibration`), each decode list began 0.4–0.9 ms after submission and ended 9 µs later, while its fence completed about 5.5 ms after that ([seeking](seeking.md#seek-catch-up-2026-10-16)). The fence is the measurement that holds.

**Measured** (Release, UHD 630):

| Run | What happened |
| --- | --- |
| `--test-quality` (64×64 long-form clip shown at 640×640, 24 fps, 41.7 ms) | Auto stepped up to Neural at 1.1 s (p95 1.3 ms). At 4.0 s the test load started: the conversion dispatch repeated until it took 37.5 ms, 90 % of the interval, at the Neural and Edge-directed tiers. Down to Edge-directed at 4.13 s (p95 37.9 ms; two loaded samples sufficed against a full light window), down to Fast at 4.97 s (p95 44.1 ms), then no change while the load lasted. N at 9 s (manual Neural), N three times at 11 s (Auto again, staying at Fast because Edge-directed was measured too slow 6 s before). 311 pictures, 0 drops, 0 allocations, 5 cues shown. |
| `--test` on the 64×64 fixture (upscaled) | Auto stepped up to Neural; p95 2.1 ms. |
| `--test` on 4K HEVC Main10 (24 fps, shown downscaled) | Stayed Edge-directed; p95 5.3 ms of 41.7 ms. |
| `--test-fixture-hdr` on `lg_black_4k60_hdr.mp4` (59.94 fps, tone-mapped, downscaled) | Stayed Edge-directed; p95 7.5 ms of 16.7 ms. |

The test load (`D3D12Renderer::SetTestLoad`) is real GPU work: the same conversion recorded again with a UAV barrier between repeats, calibrated from the measured time of one conversion. The response to overload can therefore be tested on any GPU. It is never applied to HDR conversions, since each one also folds the tone mapper's peak detection.

**Tests.**
- `media_quality_governor_tests` (MSVC, GCC, ASan/UBSan) simulates a 24 fps stream:
  - the p95 against a sorted reference over 200 values;
  - step up after exactly 32 pictures; step down after 20 at 90 %; the 4-picture overload rule;
  - one slow picture in 40 moves nothing, one in 10 steps down;
  - the 55–85 % band never moves;
  - alternating 50/90 % steps down once and stays;
  - back-off retries at exactly 2, 4, 8 and 16 s, and the reset after 10 s;
  - the 30 s memory; manual modes fixed under any load; neural availability; a workload or frame-rate change forgetting the costs;
  - 0 allocations.
- `media_playback_quality_test` runs the script above and checks the six changes, their order, reasons and phases, the final Auto/Fast state, more than 10 pictures in each tier, at least 5 cues, 0 drops.
- `media_playback_neural_test` and `media_playback_hevc_neural_test` drive N through every mode.

**Limits.**
- Only the presentation queue's own work is governed. A slow compositor, or another process on the GPU, shows up in the spans like any other cost, which is the intent. Work the GPU does for the decode engine or other queues meanwhile does not.
- The budget is the frame interval, not the display's refresh. Controls-only composites between pictures are not counted, but they are skipped near a due picture (see [seeking](seeking.md#pre-roll-and-the-presentation-queue-2026-09-30)).
- A sudden, very large overload (one conversion longer than the interval) can still delay flips until the 4-picture rule acts: about 4 conversions.
- The 30 s memory means a tier found too slow under a transient load (another application) is not tried again for 30 s after the load ends.

## HDR peak per epoch (2026-10-16)

Directive: "PTS-ordered & epoch/scene-cut aware HDR peak smoothing (Review Finding 2.F & Task 29)". The dynamic source peak (PQ/HLG tone mapping, `peak_mode` 0) had four faults:
- it was detected *during* each conversion and used from the next one on, so every picture was tone-mapped with the previous picture's exposure, and the first one with a fixed 1000 cd/m2 guess;
- it was smoothed with fixed per-picture factors (0.25 up, 0.05 down), so its speed depended on the frame rate;
- it was smoothed in conversion (decode) order;
- it was never reset, so a seek from a bright scene to a dark one inherited the bright exposure.

**Now** (`HdrToneMap.hlsl`, `ColorPipeline::Record`, `render::PeakTiming`):
1. **Detect** measures the picture's own peak first: the brightest max(R,G,B) in PQ over its source samples, each 2×2 luma block with its chroma sample. It is resolution independent (the picture's peak, not the output's), and one atomic per group.
2. **Peak** folds it into the smoothed peak over the presentation time since the last folded picture, with time constants instead of per-picture factors: 100 ms up, 800 ms down, whatever the frame rate. Bounded to 100–10,000 cd/m2.
3. The conversion then reads the smoothed peak, which now includes the picture itself.
4. **A new epoch starts over.** Every seek, and the start, makes the next picture take its own peak, exactly as when the same picture is decoded cold. The renderer restarts `PeakTiming` in `BeginEpoch` (except for a [queue seek](seeking.md#queue-seek-2026-10-16), which continues the same pictures) and after a device loss.
5. **A scene cut takes its own peak too:** when the detected peak differs from the smoothed one by more than 0.12 in PQ (about a factor of 2.5 around 100–1000 cd/m2).
6. **Presentation order.** Pictures arrive in decode order. `PeakTiming::Step` folds the PTS step since the latest picture folded, and 0 for a picture that presents before it (a B-picture after its successor). The smoothing clock therefore advances only along presentation time, and a GOP adds exactly its presentation span whatever its decode order.

The smoothing still runs in decode order across a reorder window: a B-picture is folded after its successor, with no time of its own. Folding in true presentation order would mean converting in presentation order. That cannot work here: a displayed reference must be converted (returned) before the picture predicted from it can be decoded, and that picture would have to be converted first. The order dependence is bounded to the reorder depth and deterministic, so a seek's landing still equals the cold decode exactly, as both start from the same sync sample in a new epoch.

**Cost** (`lg_black_4k60_hdr.mp4`, 4K PQ at 59.94 fps, Release, UHD 630): conversion p95 4.78 ms, against 7.48 ms with the detector inside the conversion. One atomic per 16×16 group costs less than one per output pixel.

**Tests.**
- `media_hdr_tonemap_epoch_tests` (`media_tonemap_tests epoch`; production `ColorPipeline` on the GPU, scenes of 1000, 600 and 120 cd/m2):

| Check | Result |
| --- | --- |
| Seek bright → dark, landing against the dark scene decoded cold | bit-identical (0 steps) |
| Seek bright → mid (no scene cut between them) | bit-identical |
| Bright → mid within one epoch, no cut | 7 steps off the cold picture, peak still 978 cd/m2: the inherited exposure the reset removes |
| Bright → dark within one epoch | scene cut counted, bit-identical to cold |
| Mid → bright over 0.25 s at 24 and at 60 fps | the same peak (PQ 0.74769 both), equal to the analytic exp(−0.25/0.1) attack |
| Decode order I0 P3 B1 B2 P6 B4 B5 | exactly 6 presentation intervals folded, one epoch start |

- `media_tonemap_tests`: the first picture takes its own peak (no 1000 cd/m2 guess), the detector is cleared once folded, and the reference comparisons are unchanged.
- HDR playback: `media_playback_hevc10_4k_pq_test`, `media_playback_hevc10_hlg_test` and the `lg_black` fixture tests: 0 drops.

## Downscale prefilter and dither (2026-10-16)

Directive: "ANTI-ALIASED 4K->1080p DOWNSCALE PREFILTER & DITHERING (Tasks 25 & 30)". Both are flags of `ColorConstants::filter_flags` (a new trailing word: 0 is the conversion as it was, so the reference tests that zero-fill the constants stay bit-identical), set by the renderer on every conversion.

**Downscale prefilter** (`kAreaPrefilter`).
- **When.** More than two source texels per output pixel in either axis. Up to two, bilinear's two-texel support already covers the output pixel's footprint; at exactly 2× (4K on a 1080p display, this machine's case) bilinear *is* the 2×2 box, bit for bit, so nothing changes there and nothing is paid.
- **What.** Each output pixel's luma is the average of its footprint on the source: a box of `span` source texels per axis around the pixel's centre, with fractional weights for texels it covers in part (area averaging), separable, at most 4 texels wide per axis (`kMaxSpan`; a wider footprint, below a quarter of the source size, is averaged over its central 4 texels and aliases a little again). An axis that is not downscaled keeps the bilinear tent. Chroma does the same on its own plane where chroma itself is downscaled (more than two luma texels per output pixel); guided chroma, which applies while chroma is upsampled, keeps precedence.
- **Crop.** The footprint lies inside the crop by construction (output pixels tile the crop), and every load is clamped to it anyway: point loads only, no Gather.
- **Cost.** The tap count is the same for every pixel of a dispatch (an integer span starts on a texel edge: `span` taps; otherwise at most ceil(span) + 1), so the shader branches once per dispatch to a fully unrolled 2-, 3-, 4- or 5-tap kernel (`AREA_KERNELS`). A first version looped over a per-pixel count: the loads of a pixel were then issued one after the other, and a 4K-to-1080p conversion cost 4.5 ms more with the same loads as bilinear.
- **Fast tier.** The quality governor's Fast tier drops the prefilter (it is there to shed GPU time); the dither stays.

**Dither** (`kDither`, 10-bit and HDR sources only).
- Before the 8-bit UNORM write, each channel gets a static offset of interleaved gradient noise, `frac(52.9829189 · frac(0.06711056 x + 0.00583715 y)) − 0.5` LSB: analytical, no texture, the same for every picture (no flicker), different for neighbouring pixels. The quantization error is then decorrelated from the picture, and a smooth 10-bit gradient shows no bands.
- It tapers to nothing within half an LSB of 0 and 1: black and white stay exact (a white that converts to 0.9999999 is not pushed to 254). No output moves by more than one LSB from the unquantized value.
- 8-bit sources are not dithered (their own quantization already is the source's), and an 8-bit conversion with the flag is bit-identical to one without.

**Measured** (`media_downscale_dither_tests`, synthetic NV12/P010 input read back as RGBA8, Release, UHD 630):

| Check | Without | With |
|---|---|---|
| Aliasing at 3× (a sinusoid of period 2.5 source pixels, 960×540 to 320×180): RMS left in the output | 0.2574 | 0.0539 (÷4.8) |
| Aliasing at 2.5× (to 384×216) | 0.0745 | 0.0529 (÷1.4) |
| Aliasing at 2× (to 480×270) | 0.0795 | 0.0795 (bilinear is the 2×2 box) |
| Passband (period 48), every factor | 0.259 | 0.258–0.259 |
| Banding: a dark 10-bit ramp (64 codes over 1024 pixels), column means against the unquantized conversion | 0.287 LSB | 0.022 LSB |
| Worst pixel against the unquantized conversion | | 0.990 LSB |
| GPU time, 4K Main10 PQ to 1280×720 (detect, peak, tone map, write; median of 15, interleaved) | 3.7–4.1 ms | 4.7–5.9 ms |
| The same to 1920×1080 | 6.1–6.7 ms | 6.1–6.7 ms (no prefilter at 2×; the dither is free) |

- **Crop safety:** a 960×540 picture inside a surface whose padding is poison (white luma, extreme chroma) converts bit for bit as the same picture alone, at six output sizes (2× to 4.5×, non-integer ones included) in three modes: 18 of 18.
- The continuous box's attenuation (for a period of exactly 2.5 pixels at 2.5×, none would be left) is not reachable: the texels are the signal, and their area average is what the prefilter computes.
- On the real 4K60 HDR fixture shown in a test window (`media_fixture_lg_hdr_playback_test`), the governor's p95 stayed at 6.95 ms of 16.7 ms.

**Limits.**
- A box is the simplest antialiasing filter: its sidelobes let some aliasing through (2.5× above). A tent or windowed sinc over the footprint would remove more at four times the taps; on this GPU that would no longer fit beside 4K60 tone mapping.
- The dither pattern is static: it does not move from picture to picture (no temporal averaging), which keeps it invisible but leaves a fixed ±0.5 LSB texture.

## Original | Enhanced: the split comparison (2026-10-19)

Directive: "LIVE A/B SPLIT COMPARISON (Task 69)". The picture is split down the middle so the effect of the reconstruction can be seen live: the **left half** is the plain conversion (bilinear luma, bilinear chroma, no sharpening), the **right half** is the conversion as the quality tier makes it (neural or edge-directed luma, guided chroma, sharpening if on), and a two-pixel seam separates them.

- **Turning it on.** **V** toggles it. The commands `compare quality`, `split view`, `השווה איכות` and `מסך מפוצל` turn it on; `stop comparing` and `בטל השוואה` turn it off ([commands](intents.md#follow-ups-2026-10-19)). A cue says "Comparison · Original | Enhanced" or "Comparison off". It applies from the next converted picture.
- **Exact halves.** Left of the seam, every pixel is bit for bit the plain conversion of the same picture; right of it, bit for bit the enhanced one. Right of the seam, sharpening still reads enhanced neighbours, because the sharpening ring keeps the enhanced values even in the column of groups the seam crosses. The dither of 10-bit and HDR sources applies to both halves (it is output quantisation, not enhancement); the downscale prefilter does not apply to the plain half.
- **The seam** is part of the conversion, at the picture's centre column: column `width/2 − 1` is the plain pixel at a quarter of its value, column `width/2` the enhanced pixel three quarters of the way to white. It is visible on any picture, sits exactly at the boundary at any window size because it is in picture pixels, and stays on screen when the dock fades. The directive asked for a HUD hairline; drawing it in the conversion is what keeps it exactly on the boundary between the halves.
- **Its own shader variant.** The comparison is `ColorConstants::filter_flags` bit 2 (`kCompare`), compiled as a separate variant of both conversion shaders (`SPLIT_COMPARE`: `Nv12CompareShader`, `HdrCompareShader`). `ColorPipeline::Record` picks it when the flag is set. A first version had it in the production shader behind a uniform branch: the unused code alone made every 720p → 1080p conversion 0.9 ms slower (bilinear 2.72 → 3.62 ms, edge-directed 5.56 → 6.47 ms; on Intel GPUs, more code in a kernel can cost register allocation even when it never runs). The production shaders now carry none of it, and their timings are back to that run's baseline.
- **Cost while on:** groups wholly in the plain half take the plain path alone, and only the column of groups across the seam evaluates both. 720p → 1080p with edge-directed luma and guided chroma: 4.56 ms split, 6.72 ms enhanced alone.

**Tests.**
- `media_upscale_tests` converts the hard slanted edge at 3× three ways: split, enhanced alone and plain alone. Two cases: edge-directed + guided chroma + sharpening 0.5, and neural + guided chroma. Left of the seam: 0 values differ from plain. Right of it: 0 differ from enhanced; the enhanced and plain halves differ in 1,209 and 1,779 values, so the check is not vacuous. Both seam columns are within one code of their definition. Recording and running the comparison: 0 allocations. The comparison's cost is guarded too (no more than 1.2× its enhanced half).
- `media_playback_neural_test` presses **V** live (on at the first N, off two steps later): 24 pictures converted split, off at the end.
- `media_playback_followup_test` turns it on and off by command: 10 pictures split, off at the end.

**Limits.** Like **N** and **C**, a toggle during playback shows from the next converted picture: pictures already converted ahead (up to the presentation window) keep the previous look. While paused it shows on the picture on screen at once ([paused re-conversion](#paused-re-conversion-2026-10-19)).

## Paused re-conversion (2026-10-19)

Directive: "LIVE PAUSED RE-CONVERSION ON QUALITY TOGGLES (Task 32)". Inspecting a still picture is what the comparison is for. So while paused, **V**, **N** and **C** (and the commands and setters behind them: `SetComparison`, `SetQualityMode`, `SetGuidedChroma`) change the picture on screen at once. Before, they applied only to pictures converted later.

**Why not pin the decode surfaces.**
- A decode surface returns to the decoder as soon as its conversion has run, and up to sixteen converted pictures wait ahead of the screen.
- Holding the sources of all of them would take up to sixteen surfaces from a pool sized for decoding (the DPB plus eight). The presentation window needs further decoding to release a picture, so that risks the lease deadlock the presentation queue was built to avoid.
- Copying every source as it is converted would cost up to 400 MB of VRAM and about 1.5 GB/s of copies at 4K60 HDR, every frame, for something used while paused.

**What it does instead.**
1. **A held source.** One more surface of the decode surfaces' exact description, created in `Bind` (so it is rebuilt with them after a device loss). The source of a seek's landing converted while paused is copied into it once (`CopyResource`), and only then. While playing, nothing is copied.
2. **A change while paused** (`SettingsChanged`):
   - **The held picture is on screen:** it is converted again with the settings of now (`Setup`, the same rules as `Convert`: tier, neural availability, guided chroma, comparison, prefilter, dither; an HDR picture adds no time to the dynamic peak), straight into `history_`, the clean image the controls-only refresh composites while paused. The refresh is drawn at once, not at the next 83 ms tick. The neural pass evaluates the held picture whole (its tile anchors belong to the pictures converted ahead) and is invalidated again after it.
   - **Nothing is held yet** (the first change after pausing): the renderer asks the engine for a refresh landing (`TakeRefresh`). The engine seeks to the picture on screen, `ShownPts()`. That is an internal jump, not one "undo" goes back to. The landing is decoded and converted with the new settings, bit-identical to the same picture decoded cold ([seeking](seeking.md)), and from then on it is held.
   - A change while that landing is on its way is applied to it when it arrives.

**Measured** (`media_player_demo --test-paused-quality`, Release; "GPU done": from the key to the GPU's completion of the image shown; Neural fixed):

| Stream | First change (refresh landing: decode and convert) | Later changes (re-conversion from the held source) |
|---|---|---|
| 64×64 H.264 test fixture, upscaled to 640×640 | 3.8 ms | 3.0–3.3 ms |
| 720p H.264 (`costa_rica_4k60_hdr.mp4`) | 28.5 ms | 4.7–5.1 ms |
| 4K HEVC Main10 PQ (`lg_black_4k60_hdr.mp4`), tone-mapped | 176.6 ms | 12.3–14.8 ms |

The first change after pausing costs one decode of the picture from its sync sample. That is a long-GOP 4K Main10 stream's limit, not a fixed cost; the later ones cost one conversion.

**Test** (`media_playback_paused_quality_test`). The script pauses at 1 s and captures the picture on screen: the held image, read back and hashed in two halves, left and right of the centre's two columns. Then V, N, C and V, each followed by a capture once the screen has changed. Exact tile reuse, so pictures compare bit for bit. Results:
- **V** (refresh landing): right half identical to before, left half now the plain conversion.
- **N** (Neural → Edge-directed): left half identical, right half changed.
- **C** (guided chroma off): left half identical. The right half was unchanged too, because the fixture has no colour edges and guided chroma is exactly bilinear there; the update records chroma bilinear.
- **V** (off): right half identical, left half the enhanced conversion again.
- Each update's recorded mode is as set; one refresh landing in all; every re-conversion on screen and done within 50 ms of its key; 0 drops, 0 allocations.

**Limits.**
- The first change after pausing re-decodes the picture: up to 0.18 s on 4K Main10 here.
- Pictures converted ahead before the pause keep their settings, so they show the earlier look for about half a second after playback resumes, as when a setting changes during playback.

## Zoom (2026-10-20)

Directive: "SURGICAL ROI ZOOM (TASK A36)". From 1× to 8× into any part of the picture, reconstructed at the zoomed scale from the region's own samples, not magnified after the reconstruction.

**Controls.**

| Input | Effect |
|---|---|
| **Ctrl + wheel** | 1.25× a notch, at the pointer: the sample under the pointer stays under it |
| **Arrow keys**, while zoomed | Pan by a tenth of the region shown (otherwise they seek and change the volume, as before) |
| `zoom in` / `zoom out`, `זום פנימה`, `הקטן`, `拡大`, … | ×2 / ÷2 at the middle of what is shown; at 8× or 1× an `OutOfRange` cue |
| `reset zoom`, `אפס זום`, `whole picture`, … | Back to the whole picture |

`D3D12Renderer::SetZoom(zoom, x, y)`, `ZoomBy(factor, x, y)`, `Pan(dx, dy)`, `Zoom()`, `ZoomX()`, `ZoomY()`. Each change shows `Zoom · 2.5×` or `Zoom off`. Commands in nine languages: [intents](intents.md#speed-and-zoom-2026-10-20).

**How it is drawn.**
- The zoom is a crop. `RegionOf(frame)` gives the region of the picture's crop that is shown: the crop's aspect, 1/zoom of its size, centred where asked and kept inside the crop, with even edges (4:2:0 chroma).
- That region is the conversion's source rectangle: the constants' `left/top/right/bottom` and the neural pass's `Record` both take it. So everything that reconstructs (edge-directed luma, the neural 2× luma, guided chroma, sharpening, the downscale prefilter) runs on the region's own samples at the zoomed scale, and the result fills the letterboxed video rectangle.
- Subtitles, the dock and the status pill are drawn over it, unzoomed.
- While paused, a change re-converts the held picture at once ([paused re-conversion](#paused-re-conversion-2026-10-19)). While playing, it is on screen from the next composite: the pictures already converted are shown through it ([live zoom](#live-zoom-2026-10-21)), and those converted after it are reconstructed from the new region.
- Zoom is per file. Music mode, which has no picture, refuses it.

**Measured: the region reconstructed against the picture enlarged** (`media_upscale_tests`, "Zoom"; the hard slanted edge, 96 × 96, zoomed 4× into its middle 24 × 24 and shown 288 × 288; against the 3× conversion's middle 72 × 72 enlarged 4× bilinearly afterwards; per row, the 50 % crossing against the true edge and the 10–90 % transition width, in output pixels):

| Source, reconstruction | Width: region / enlarged | Position bias: region / enlarged | Staircase (crossing spread): region / enlarged |
|---|---|---|---|
| Aliased edge, edge-directed | **8.2** / 9.9 | **−0.05** / +0.23 | 1.81 / 1.74 |
| Antialiased edge, edge-directed | **10.8** / 11.6 | **−0.03** / +0.22 | 0.31 / 0.23 |
| Antialiased edge, neural | **8.8** / 10.1 | **+0.02** / +0.02 | 0.67 / 0.65 |

The region is sharper (its edge 7–17 % narrower) and at least as well placed. Its staircase is no better: the source's own sampling is what both magnify. The test asserts a narrower edge, a bias no worse, and a staircase within 1.4× the enlarged one's.

**Measured: in playback** (`media_playback_zoom_test`, the 720p HEVC fixture, Release, UHD 630). Pause at 1 s, then each step waits for the screen to change and captures it (hashes of its two halves):

| Step | Zoom | Region (of 1280 × 720) | Re-converted on screen |
|---|---|---|---|
| Paused | 1× | (0,0)–(1280,720) | — |
| `zoom in` | 2× | (320,180)–(960,540): the middle half | 18.1 ms |
| Ctrl + wheel, 3 notches at ¾ across | 3.906× | (554,268)–(882,452): the sample under the pointer, 800.0, stays at 800.0 | 0.9 ms |
| Left | 3.906× | (522,268)–(850,452): a tenth of the region (32 of 32.8 px) to the left | 0.4 ms |
| `אפס זום` | 1× | (0,0)–(1280,720); the image identical, bit for bit, to the one before any zoom | 0.6 ms |

Each step's image differed from the previous one; 0 pictures dropped; 0 allocations.

**Limits.**
- ~~While playing, the pictures already converted ahead keep the previous zoom.~~ Closed 2026-10-21: [live zoom](#live-zoom-2026-10-21). **N** and **V** still apply from the next converted picture.
- At 8× a 720p picture's region is 160 × 90 samples: the reconstruction keeps edges sharp, but it cannot invent detail that is not in the samples.
- There is no zoom gesture for touch, and no minimap of where the region is.

## Live zoom (2026-10-21)

Directive: "ZERO-LATENCY ZOOM ON QUEUED FRAMES (TASK 32)". While playing, up to 16 pictures wait in the presentation queue already converted. Before this, a zoom or pan changed only the pictures converted after it, so the change reached the screen 0.27–0.67 s late.

**How it works.**
- Each RGB slot records the region it was converted from (`Slot::region`, `Slot::crop`, the zoom generation then). The history texture records the same for the picture it holds.
- At composition, the region asked for now (`RegionOf(crop)`) is compared with the one the picture was converted from. When they differ, the picture goes through `PictureBlit.hlsl` instead of the copy. The blit samples the new region out of the old one: `ViewOf(converted, wanted)` gives the view's origin and span as fractions of the picture.
  - Zoom in or pan: a bilinear enlargement of the part of the picture the new region covers.
  - Zoom out: the picture covers only part of the new view; the rest is black until the native pictures arrive. The slot beyond the picture never shows.
- A change made while playing asks for a composite at once (`zoom_redraw_`). It bypasses the controls' refresh interval, but not the rule that no controls-only image is drawn when a picture is due within a refresh: that picture's own image carries the change.
- Pictures converted after the change are reconstructed from the new region's own samples, as before ([zoom](#zoom-2026-10-20)). The hand-off is per picture, with nothing to synchronise: a picture's region decides how it is drawn.
- `PictureBlit.hlsl` takes 8 root constants: `extent`, `limit`, `origin` and `span` (`BlitConstants`).

**Records.** `D3D12Renderer::LiveZoomAt(i)`: for each change made while playing:
- `shown`: from the change to the `Present` of the first image showing it;
- `native`: to the `Present` of the first picture converted at it;
- `bridged`: images shown through the transform in between.

`TransformedComposites()` counts them all.

**Measured: the shader** (`media_aspect_ratio_crop_tests`, "Live zoom view"; a 101 × 67 picture in a poisoned 256 × 256 slot, against a bilinear sample computed on the CPU):

| View | Pixels compared | Largest difference | Beyond the picture |
|---|---|---|---|
| Zoom in 2×, off centre (200 × 136) | 27,200 | 1 of 255 | — |
| Pan (101 × 67) | 6,767 | 1 of 255 | — |
| Zoom out 2× (101 × 67) | 1,568 | 0 | 4,859 of 4,859 black |

**Measured: in playback** (`media_playback_zoom_live_test`, the 64 × 64 long-form fixture at 24 fps, upscaled; Release, UHD 630). Each change waits for the previous one to be on screen natively:

| Change | On screen after | Images through the change | Native after |
|---|---|---|---|
| `zoom in` (2×) | 0.14 ms | 32 | 434 ms |
| Ctrl + wheel, 2 notches | 0.11 ms | 31 | 434 ms |
| Right (pan) | 0.12 ms | 17 | 434 ms |
| `reset zoom` | 0.16 ms | 30 | 434 ms |

110 images were shown through a change, 0 dropped, 0 allocations. "On screen" is until `Present` returned with the image: the display shows it at its next refresh. A first version took 18.1 ms, because the status cue's fade interval held the composite back; a zoom change now bypasses that interval. "Native after" is the queue's depth: about 10 pictures at 24 fps.

**Limits.**
- The pictures in between are enlargements of what was converted: their sharpness is the old region's until native pictures arrive (434 ms at 24 fps with a full queue).
- After a zoom out they are framed in black until then.
- **N**, **C** and **V** still change only pictures converted after them.

## Pinned | Live (2026-10-21)

Directive: "DUAL-TIMESTAMP SPLIT COMPARISON (AI TASK A24)". A frame from minutes ago beside the frame on screen now: before and after, for medical review, surveillance or slides.

**Pinning.** `D3D12Renderer::PinReference` copies the picture on screen (the history texture: the last picture presented, or the paused one) into the reference texture, on the direct queue after everything already queued. These pin it:
- **Ctrl+B**, `pin frame`, `הקפא פריים להשוואה`;
- **B**, which also sets a bookmark ([session](session.md#bookmarks-2026-10-21)).

The cue says `Frame pinned · 0:12.4 · V compares`.

**Comparing.** **V** cycles Original | Enhanced → Pinned | Live (once a frame is pinned) → off. `compare with the pinned frame` goes straight to Pinned | Live. The cue says `Comparison · Pinned 0:12.4 | Live`.

**Composition.** After the live picture is placed, before captions and controls:
- the reference's left half goes over the picture's left half: copied when its size is the picture's, else scaled into the same rectangle;
- a 2-pixel seam is drawn between the halves.

Original | Enhanced is made by the conversion; Pinned | Live only by composition, so it costs nothing while off.

**Measured** (`media_playback_bookmark_compare_test`):
- Paused on bookmark 3 (4.0 s), with the frame at 2.75 s pinned.
- The screen was captured before the controls: its left half's hash equals the pinned frame's left half and its right half's equals the live frame's right half, exactly. The two frames differ.
- 122 composites drew the comparison, 0 pictures dropped.
- Test hooks: `RequestScreenCapture`, `RequestReferenceCapture` (`Capture::kind`).

## Picture controls and micro-motion (2026-10-21)

Directive: "REAL-TIME MICRO-MOTION & PULSE MAGNIFICATION SHADER ... HIGHLIGHT-SAFE TACTICAL SHADOW-LIFT". Both run in the conversion dispatch itself ([`Reconstruct.hlsli`](../src/render/Reconstruct.hlsli) `Finish`), once per output pixel after reconstruction, sharpening and the comparison, before the dither. No new pass.

**Picture controls** (`D3D12Renderer::SetPictureControls`, `ColorConstants::picture_flags` `kGrade`), on display-encoded R'G'B'. Each step runs only when it is not neutral, so neutral controls change no bit:
1. **Shadow lift** L (0 to 1): adds 2 L Y' (1 − Y'/0.75)² to every channel below Y' = 0.75.
   - Black stays black.
   - The curve is monotonic and meets the identity with slope 1 at 0.75, so from Y' 0.75 up the picture is bit-identical and nothing clips.
   - A channel pushed past 1 is pulled toward the new luma (hue kept).
2. **Contrast** c around mid-grey ((v − 0.5) c + 0.5), 0 to 2.
3. **Brightness** b (+b), −0.5 to 0.5.
4. **Gamma** g (v^(1/g)), 0.5 to 2.5: above 1 the mid-tones come up.
5. **Saturation** s around the BT.709 luma, 0 to 2: 0 is monochrome.

**Micro-motion** (`kMicro`; "micro motion", "tactical vision", "הדגש תנועה זעירה"): small temporal changes made visible, as in Eulerian video magnification.
- **The reference.** Each output pixel keeps a temporal reference of its luma: one float per pixel of a slot, in a buffer the session's colour pipeline owns (`ColorPipeline::PrepareMotion`, made with the slots; 8 MiB at 1080p).
- **Magnifying.** The pixel's change against it, d, becomes Y' + (gain − 1) w d, added to all three channels (a luma change, hue kept). w is:
  - 1 for 1/255 < |d| ≤ 0.04;
  - falling linearly to 0 at 0.08, and 0 above it;
  - 0 below half a code (quantisation jitter of one code is not magnified).
- **Motion is not magnified.** A change above 0.08 is motion: it is not amplified, and the reference jumps to the pixel at once. So a moving edge leaves no trail and no ghost.
- **Following.** Otherwise each picture folded in moves the reference an eighth of the way: a temporal low-pass, so what is magnified is variation around the pixel's recent mean (a pulse, a breath, a vibration).
- **Order.** Folding follows presentation order: a B-picture converted after its successor is magnified, not folded.
- **Starting over.** A new epoch (a seek), a new zoom region or a new picture size starts the reference over.
- The coordinator uses gain 6 (`PlaybackCoordinator::kMicroGain`; at most 8). The test load (`SetTestLoad`) is not repeated on a magnified conversion, because a repeat would fold the reference twice.

**Measured** (`media_eq_picture_tests`, the production pipeline on synthetic NV12 converted 1:1 and read back; Intel UHD 630):

| Check | Result |
|---|---|
| Neutral controls with `kGrade` set | bit-identical to no controls |
| Shadow lift 0.6 on a luma ramp | Y' 0.10 rises by 0.094; every column below 0.5 rises, none up to 0.75 falls; from 0.75 up bit-identical; monotonic; nothing clipped |
| Saturation 0 on a coloured picture | R = G = B everywhere |
| A 16×16 patch pulsing ±2 codes on flat grey, gain 6 | 2.50 codes on screen become 14.50 (5.8×); the still surround bit-identical |
| A bar moving 8 px a picture, gain 8 | bit-identical to the conversion without micro-motion: no ghost |

**Cost** (GPU timestamps, 1920×1080 noise converted 1:1, medians of 24, three runs; Chrome was decoding video on the same iGPU at 12–15 % of its 3D engine):

| Conversion | Time |
|---|---|
| Plain | 2.94–3.02 ms |
| + shadow lift and saturation | +0.76–0.82 ms |
| + micro-motion (folding) | +1.24–1.30 ms |
| + both | +1.61–1.68 ms |

Micro-motion is not sub-millisecond on this GPU: each pixel reads and writes its 4-byte reference (16 MB of traffic per 1080p picture on a shared-memory iGPU). Both stages are compiled only into their own shader variants (`PICTURE_CONTROLS`: `Nv12PictureShader`, `HdrPictureShader` and the split-comparison pairs), chosen when `picture_flags` is set, so a conversion without them runs the production shader unchanged. Compiled into the production shader, the stages cost 0.65–1.4 ms per 1080p conversion with the flags off (register allocation), as the split comparison's code once did.

The symptom cures use these controls: "too dark" / "חשוך" sets shadow lift 0.6, gamma 1.15 and brightness +0.03, and "undo" / "בטל" puts back exactly what was there ([intents](intents.md#audio-zoom-micro-motion-and-cures-2026-10-21)).

**Statistics for the frame signatures** ([SDK](sdk.md#frame-signatures)). The scene-statistics pass also keeps each picture's brightest sample and the share of 8×8 blocks that moved. On the ramp, the brightest sample is 899 (expected 898); a repeated picture has 0 of 64 blocks moving, and one changed everywhere 64 of 64.

**Limits.**
- Micro-motion magnifies luma only. Real video's noise above one code is magnified with the signal: it is for steady footage.
- Folding in decode order would be wrong for B-pictures, so they are magnified without being folded.
- The reference is per output pixel: a zoom or pan starts it over.

## Target tracking (2026-10-21)

Directive: "AUTONOMOUS AUDIO-VISUAL TARGET TRACKING ZOOM (AI tasks A11, A35)". **T**, "track target", "follow the object", "עקוב אחרי האובייקט" (nine languages, [intents](intents.md#target-tracking-2026-10-21)) lock onto what is in the middle of the view; the view then follows it, and the audio zoom's beam with it. "stop tracking" / "הפסק מעקב" / T again let go.

- **The grid.** While tracking, each conversion also runs a small pass (`HdrToneMap.hlsl` `Grid`) over the whole crop (not the zoomed region): one cell per s x s samples, their rounded mean as an 8-bit code, s the smallest scale that fits 480 x 272 cells (1 at 64x64, 3 at 720p, 4 at 1080p, 8 at 4K). It is copied to a per-slot readback (130,560 bytes each, made with the file: tracking allocates nothing). The CPU's `video::MakeGrid` is the same arithmetic (host mode uses it on the software bridge's planes).
- **The tracker** ([`video::TargetTracker`](../include/media/video/TargetTracker.hpp)): a 16 x 16-cell template at the lock point, weighted by a raised-cosine window (the background at its edges counts little: without it a target leaving its static surroundings was lost for three pictures); each picture, zero-mean normalised cross-correlation at every whole-cell offset within 10 cells of an alpha-beta prediction (20 while lost), a parabola per axis for the fraction of a cell, the estimate moved by alpha 0.7 / beta 0.3; the template refreshed 10 % toward a match of 0.92 or more. A flat patch is refused ("nothing to follow in the middle").
- **Lost.** A match below 0.6, a picture whose mean jumps by more than the cut detector's 0.12 (at once: the live detector confirms a cut four pictures later), or a seek: the estimate holds, the velocity is forgotten, the cue says **"Tracking lost · Holding view"**, and the search goes on around the last position until a match of 0.75 takes the target back. A seek ends tracking.
- **The view.** One conversion is in flight at a time, so when picture N is converted picture N-1's grid has been read: the tracker moves the view's centre for N's conversion (2x when the zoom was off), kept inside the picture. While tracking each picture is shown with the region it was converted from, its own framing (no live-zoom transform); the pictures already converted when T was pressed (about 0.6 s here) keep the framing they had.
- **The sound.** Each UI pass, the audio focus takes the zoom and the centre of the picture on screen (`ShownX`), so the beam follows what is seen, not the conversions ahead of it.

**Measured.**
- [`media_target_tracking_tests`](../tests/target_tracking_tests.cpp) (portable; MSVC, GCC -O2 and ASan/UBSan): a 24-sample textured target on a textured 960x540 background with noise, crossing the picture at 4.5 cells a picture and then circling: tracked 159 of 159 pictures, match error 0.17 cells RMS (0.34 samples), worst 0.32; the smoothed estimate (the view) RMS 0.39, worst 1.9 cells where the target starts at full speed and turns sharply (the filter's lag). Two tones panned 0.2 and 0.8 under an audio focus following the view: the left one 3.8 dB above the right while the target is left, 4.1 dB below once it is right (constant-power theory: 3.5). A 25 % brightness ramp: on target 20 of 20. A grey card over the target for 12 pictures: lost after 1, the view moved 0.00 samples while lost, found again at once after it. A cut: lost on the cut picture, never "found" in the new shot (0 of 30). 21 of 21 commands; "next audio track" is still a track switch. 0 allocations.
- `media_playback_tracking_test` (Windows, GPU, the tracking fixture: a 12x12 square moving 16 samples right from 1 s to 4 s, a cut to a dark shot at 5 s): 0 dropped; the view first moved on the picture at 1.625 s (before 2026-10-22; now the next picture after T, below); 81 pictures followed, all within 3.5 samples of the square (worst 3.0, of a 64-sample picture: 1-sample cells and the region's even-sample alignment); the tracker's own estimate within 2.3 samples; the audio focus on the shown view for all 81; after the cut 19 pictures, all held at 0.719, with the cue.
- Host mode (`media_c_api_tests`, both platforms): the view within 0.005 of the square on 66 pictures.

**Not done.** The grid pass's GPU cost was not measured (it reads every luma sample once while tracking; the 64x64 fixture says nothing about 4K). No equality test of the GPU grid against `MakeGrid`. One target; no scale or rotation estimate.

**T answers on the next picture (2026-10-22).** Until now the pictures already converted when T was pressed kept their framing, so the view changed only once they had been shown (the test read 1.625 s for a press at 1 s, though part of that was its criterion: the view's centre moves only when the square does). Now T at 1x zooms to 2x at once (a live zoom), and while tracking a picture converted before the lock is shown through the tracking view, sampled out of what it holds (`PictureBlit`, as for a live zoom), centred where the tracker's estimate and velocity put the target at that picture's time (`D3D12Renderer::TrackedView`; `TrackStatus` gained the filter's velocity). Pictures converted while tracking keep their own framing, as before. If the lock finds nothing to follow, the zoom T put on goes again. `ShownX` and the new `ShownZoom` are those of the region actually composited, so the audio focus follows that too. `media_playback_tracking_test` now requires the very next picture shown after the key to be zoomed and the live zoom on screen within two refreshes: measured 0.47 ms from the key to its `Present` (0.03 refreshes), 4 queued pictures shown through the tracking view, 96 pictures followed to 5 s, all within 3.5 samples of the square (worst 3.0, at 1.167 s: the lock picture's patch was centred 3 samples off the square, and the estimate carried back from it keeps that offset), 0 dropped.

## Perception: flow, looming and saliency (2026-10-22)

Directive: "BIONIC RETINA & PHYSICAL WORLD PERCEPTION SDK" (B). [`video::Perception`](../include/media/video/Perception.hpp): what a robot's or a vehicle's camera needs to react before any model runs, from the luma alone, portable, no allocation after `Configure`. It runs on every picture the host engine shows (live sensors, and files whose pictures the software bridge decodes) when the telemetry module is on and the picture is at least 128 x 128 samples; the SDK reads it with `media_query_frame_telemetry` and `media_query_saliency_grid` ([SDK](sdk.md#live-sensors-and-perception-2026-10-22)).

- **Cells and history.** The picture as 128 x 128 cell means (16 times the 8-bit code; area means over integer bounds, so any size works), kept for the last 8 pictures with their times and the camera's accumulated rotation. Tiles are 8 x 8 cells: 16 x 16 tiles.
- **Flow.** Each tile is matched in an earlier picture: the newest one at least 90 ms old (a longer baseline measures slow expansions better), else the oldest held; if the best match lies on the search's edge (too fast for that baseline) it is matched again against the previous picture. Zero-mean SAD (brightness changes cancel) over +-6 cells around where the camera's rotation alone would have moved the tile (an IMU's yaw, pitch and roll through the field of view: a pan faster than the search is still measured), refined to a fraction of a cell by an equiangular fit (SAD rises linearly from its minimum; a parabola was biased toward whole cells: a 90 px/s pan read 94.5, now 91.0). A tile is reliable when it is textured (mean absolute deviation 1.5 codes), its best cost is under half the search's mean (a match, not the least bad of many: content entering at the picture's edge matched garbage at 91-269 px/s before this test), and the cost rises in both axes by at least half a code per cell (no aperture problem). The velocity has the camera's rotation taken out: what remains is the world's own motion.
- **Looming and time to contact.** Over every 5 x 5-tile window with at least 8 reliable tiles, a least-squares affine fit v = v0 + A (p - p0), once more without the tiles whose residual exceeds both 8 px/s and three times the median. An approach is an expansion div = a11 + a22 above 0.2/s (a TTC within 10 s) that is isotropic (both diagonal terms positive, the smaller at least 0.35 of the larger), shear- and rotation-free (|a12| + |a21| at most half the divergence) and explains the flow (the residual at most half the modelled expansion): a translating edge or corner, a pan, or noise is none of these. The strongest such window gives the divergence, **TTC = 2 / div** (a surface approaching at constant speed has flow (x - x0) / TTC around its focus of expansion x0) and the **hazard** at that focus (where A p + v0 = 0, in the picture).
- **Saliency.** Per tile: 0.55 x the temporal change against the previous picture (above the picture's median change, so sensor noise or the camera's own motion is not salient; 6 codes is a full score) + 0.30 x the tile's own speed (a quarter of the picture per second is a full score) + 0.15 x the edge density (24 codes of gradient). The mask is the tiles at 0.25 or more: a static textured scene scores at most 0.15 and leaves it empty.

**Measured** ([`media_perception_tests`](../tests/perception_tests.cpp), 640x480 at 30 fps; MSVC Release, GCC -O2 and ASan/UBSan, the same results):

| Scene | Result |
|---|---|
| An obstacle approaching at 1 m/s from 3 m (textured, flat background) | looming on 53 of 55 pictures from 0.2 s; TTC error median 5.9 %, 90th percentile 17.4 %, worst 42.5 % (the first pictures: a slow expansion over about 15 tiles); 2.96 s early (truth 2.55) to 1.16 s late (truth 1.25); the hazard 0.020 from the obstacle's centre (median, worst 0.084 when it fills half the picture), never off it |
| The same texture translating at (90, 45) px/s over a static textured background | no looming in 42 pictures; every tile inside the object within 11.7 px/s of the truth; the dominant motion 0.001 picture widths per second (the background) |
| A 90 px/s pan with the IMU's yaw | the dominant motion at most 0.002 widths per second (the scene does not move); without the IMU -91.0 px/s |
| A small object moving over a static textured scene; then still | mask at most 2.3 % of the tiles, none away from the object, its own tile always in it; still: an empty mask |

Cost: 0.65 ms per 640x480 picture on the test machine's CPU (Release, the approaching obstacle on a flat background); a textured picture whose every tile is searched costs more. 0 allocations.

**Limits.** Synthetic scenes only: no real camera footage was measured. The flow is one vector per tile (a tile straddling two motions gets one of them, or none); the first pictures of an approach, with little expansion over few tiles, overestimate the TTC by up to 40 %; the TTC assumes constant closing speed. The IMU model is small-angle rotation through a pinhole of the given field of view (no lens distortion, no translation of the camera: a camera moving forward sees the whole scene expand, which is, correctly, an approach). Perception does not run in the windowed engine (its pictures stay on the GPU).

## Stabilisation (2026-10-22)

Electronic image stabilisation for live cameras with an IMU (the SDK's `MEDIA_SENSOR_STABILIZE`, `media_engine_set_stabilization`; [sdk](sdk.md#gpu-live-ingest-and-stabilisation-2026-10-22-second-directive)). [`video::Stabilizer`](../include/media/video/Stabilizer.hpp), portable, no allocation:
- the IMU's yaw and pitch deltas are accumulated; a low-pass of 0.35 s delay, by each frame's timestamp, is what the operator means (a pan, a tilt), the rest is shake (a one-pole until 2026-11-04, then a second-order Butterworth of the same delay: [Stabilising the window](#stabilising-the-window-2026-11-04));
- the view moves with the shake by what it does to the picture's centre: yaw to the right moves the scene left by focal x angle, so the view's centre moves left by focal x (yaw - smoothed yaw) / width; pitch likewise down;
- the view is at least a 1.25x crop (a 10 % margin each side) and stays inside the picture: a shake larger than the margin is held at its edge;
- roll turns the picture about its centre, which a pan-and-zoom view cannot undo: it is left to perception, which takes it out of the flow (since 2026-10-27 the view turns: [Roll](#roll-and-compensated-micro-motion-2026-10-27)).

The host engine applies it to the view it hands the host (`HostPicture`, telemetry) and to the GPU image's region; its own view (zoom, saccade, tracking) is kept unshaken underneath.

**Saliency under shake.** Perception already searched each tile's flow around where the IMU's rotation moved it. Its saliency compared raw cells with the previous picture's, so a shaken camera made every textured tile "changed": 7,865 to 10,288 salient tile-pictures away from the object in 53 shaken pictures. Now:
- each cell is compared with the previous picture where the camera's rotation (yaw, pitch and roll, per cell) put it, between cells (bilinear);
- a cell that came from outside the picture is not compared (two thirds of the remaining false tiles were on the border); a tile with fewer than half its cells compared has no change measured;
- a change counts from 1.5x the tile's floor, the picture's median change scaled by the tile's texture against the median tile's, on a scale of 6 codes plus that floor (a still camera's floor is about 0: unchanged there).

| Shake (+-1.5 / 1.2 / 1.0 degrees yaw / pitch / roll at 7 / 9 / 11 Hz, 640x480, 53 pictures) | with the IMU | without |
|---|---|---|
| Looming | 0 | 0 |
| Dominant motion (widths/s, at most) | 0.005 | 0.222 |
| Salient tile-pictures away from the moving object | 1 (was 2,167 with whole-cell registration, before that thousands) | 7,865 |
| The moving object salient | 53 of 53 | |
| The picture's centre / the stabilised view (RMS) | 7.6 px / 1.0 px | |

## Perception on real footage (2026-10-22)

`media_real_perception_tests` (new, Windows): `costa_rica_4k60_hdr.mp4`'s first 300 pictures (720p59.94 H.264: a low camera on a beach moving slowly forward, water washing toward it, trees, clouds), decoded by Media Foundation for this test only, fed to `video::Perception`:
- **The footage.** The flow covers 59 % of the tiles; the dominant motion agrees with the whole picture's SAD registration at full resolution within 21 % (median; floor 15 px/s). The foreground (wet sand, water coming in) is called looming on 91 of 292 pictures, TTC 1.0-2.1 s, at y 0.66-0.74: the camera does approach it; reported, not judged. 3.4 % of the tiles salient on average.
- **Shake added** (a 960x540 window moving +-12 / 9 px at 7 / 9 Hz, the IMU's deltas from it): the dominant motion differs from the still window's by 3.0 px/s (median; 12.5 at most), looming on 63 pictures against 65, salient 4.8 % against 3.4 %.
- **An obstacle pasted on** (a textured square approaching at 1 m/s from 3 m, half size 100 px at 3 m, at (0.55, 0.45)): where it is the strongest approach and within 2 s of contact, looming on 14 of 14 pictures, TTC error 2.9 % (median), the hazard at most 0.16 half-sizes from its centre. From 2 to 2.8 s (an expansion under 1/s over moving water) it was found on 0 of 15 pictures, where the synthetic scene finds it from 3 m: on real backgrounds the looming range is shorter. (2026-10-27: found on 48 of 48 by the fovea, the beach taken out as ground: [below](#the-fovea-and-the-ground-2026-10-27).)

## Roll and compensated micro-motion (2026-10-27)

Live ingest ([sdk](sdk.md#roll-ten-bit-and-compensated-micro-motion-api-5-2026-10-27)). Two gaps of the stabilised GPU image: roll was not stabilised, and micro-motion restarted with every new region, so a stabilised (moving) view almost never magnified anything.

**Roll.** [`video::Stabilizer`](../include/media/video/Stabilizer.hpp) treats roll like yaw and pitch: accumulated, low-passed (0.35 s), the fast part is shake. A camera rolling clockwise by r turns the scene by -r on the picture (perception's convention), so the view turns by -(r - smoothed r) about its own centre, and its centre turns with the scene about the picture's centre. The turn is at most `kMaxRoll` (0.2 rad) and only as much as keeps the turned view's bounding box inside the picture (bisected), then the centre is clamped to where it fits. A horizon the operator holds level stays level through the jitter; a slow bank passes through, as a pan does.

**The turned view in the conversion** ([`Reconstruct.hlsli`](../src/render/Reconstruct.hlsli), `ColorConstants` `kRotate`; since 2026-10-28 a general affine map, q = M X + t, which the file playback's rotation shares: [Rotation](#rotation-2026-10-28)): each output pixel's position in the crop, q, becomes c + R (q - c) + d (c the crop's centre, d a move of up to a texel each way). The crop itself stays on even texels (4:2:0 chroma), so d is also the sub-texel rest of the view's position: the stabilised view was quantised to 2 texels before. Reads clamp to the coded surface, not the crop (the turned view's corners reach past it): live ingest only, whose surfaces have no padding; every group takes the clamped loads (the interior test assumes an upright crop). The code is compiled into the picture-controls variants only (ColorPipeline picks them for a turned view): unused code in the production shader has cost up to 0.9 ms a conversion on the UHD 630 before. Neural reconstruction is not used on a turned view (its 2x image is of the upright crop).

**Micro-motion that follows the view.** The reference is kept per output pixel. It is now double-buffered (`micro_read`, `micro_write`; the windowed engine keeps 0, 0: in place, as before) and, with `kMicroWarp`, read where the pixel's content was in the previous image, between pixels: one affine map (`micro_map`) composed on the CPU from this view's mapping (crop, scale, turn, sub-texel move), the scene's motion since the previous frame (the IMU's deltas: a shift of focal x angle and a turn about the picture's centre) undone, and the previous view's mapping inverted. Identity (nothing moved): the plain read. It starts over only when the view's size changes (a zoom), when micro-motion is turned on, on `LiveView::restart`, and per pixel when the content came from outside the previous view; a cut changes every pixel by more than micro-motion's motion threshold (0.08), which already makes each of them start over. Where the reference's 2x2 neighbourhood spans more than `kMicroEdge` (3 codes) the magnification fades out, to nothing at 6: an edge or texture under a residual misregistration of a fraction of a pixel changes by its gradient, which is not a pulse.

| 320x240, UHD 630 | stabilised, micro-motion 5x | without micro-motion |
|---|---|---|
| A still flat target pulsing +-2 codes, camera shaken +-3 / 2.5 degrees at 7 / 9 Hz | 27.0 codes peak to peak (5.4x) | 5.0 |
| A still ramp of 1 code per pixel beside it | 2.08 codes | 1.75 |
| The same ramp with the map missing the scene's motion (the first build) | 81 codes | |
| The reference followed the view | 49 of 49 images | |
| Horizon, roll +-5 degrees at 7 Hz: its rows from level (RMS, camera px) | 0.25 (the view turned up to 4.8 degrees) | 4.28 unstabilised |

## Ten-bit live ingest (2026-10-27)

P010 camera frames reached the GPU as the host engine's 8-bit conversion (rounded top 8 bits). With `MEDIA_SENSOR_GPU_10BIT` the texture ring is P010 (`DXGI_FORMAT_P010`, plane views R16 / R16G16): `LiveGpuPipeline::IngestP010` copies the camera's rows as they are, the conversion takes its 10-bit path (`ten_bit`) and dithers to the 8-bit image (the static +-0.5 LSB of [Downscale prefilter and dither](#downscale-prefilter-and-dither-2026-10-16)); 8-bit frames go into the ring as their 10-bit codes (v << 8). The host engine's own pictures (perception, tracking, `on_present`) stay 8-bit.

A P010 ramp of one 10-bit code per column (codes 64 to 223) through shadow lift 1.0, each column averaged over its 240 rows: rising on 159 of 159 columns by at most 0.88 code (the lift's slope near black is 3: 3 x 255 / 876 = 0.87); the 8-bit path rises on 40 columns, by up to 4.0 codes, flat in between: the banding shadow lift makes of 256 levels.

## The fovea and the ground (2026-10-27)

Two findings on `costa_rica_4k60_hdr.mp4` ([above](#perception-on-real-footage-2026-10-22)): a distant obstacle was missed from 2 to 2.8 s of contact, and the camera's own travel made the beach in the foreground "loom" on 91 of 292 pictures. Measured first: in that window the beach's windows expanded faster (1.0-2.0/s) than the obstacle (0.7-1.0/s) and won every picture, and where the beach did not loom the obstacle (2.7 coarse tiles wide, 107 px half size at 2.8 s) was a third of any 5x5-tile window and failed the fit. Changes to [`video::Perception`](../include/media/video/Perception.hpp):

- **The fovea.** The central half of the picture again as 15 x 15 tiles of 8 x 8 cells every 4 cells (twice as dense; 240 x 135 px windows at 720p), matched against the oldest picture held first (seven pictures: 117 ms at 60 fps; then the 90 ms baseline, then the previous picture when the motion is too fast), fitted by the same 5x5-tile windows and tests.
- **Where a tile's flow is.** Each tile's flow is placed at its texture's centroid (its cells weighted by their gradient), which is what its match follows: a tile holding an obstacle's edge over a flat background moves with the edge, not its centre.
- **The baseline's bias.** A displacement over a baseline dt at the tiles' present positions is x (1 - Z / (Z + v dt)): the fit's expansion is 2 / (TTC + dt), so every time to contact read dt late (0.1 s on the 90 ms baseline; the fovea's 233 ms at 30 fps made it 23 % on the synthetic scene). The divergence is now corrected to d / (1 - d dt / 2) with dt the window's mean baseline.
- **The approach reported.** The strongest window says where; the divergence is the median of every accepted window (both levels) whose focus is within a tenth of the picture of it. The strongest of hundreds of noisy windows alone read the time to contact 10-20 % early on the footage.
- **A floor for an expansion.** It must move its tiles a quarter of a cell over the baseline (rms of the expansion field), plus 5 % of what the IMU's rotation moved them (the compensation's errors grow with it): a shaken window's fovea made 1.5-2.4 px expansions over 117 ms out of registration noise.
- **The ground.** Forward travel over a ground plane flows out of a focus of expansion, faster toward the bottom: u = k (x - x0)(y - yh) + r, v = k (y - yh)^2 + c. Fitted to the coarse tiles of the bottom 6 rows (v as a quadratic in y gives k and the horizon, then u gives x0 and r; the worst tiles out once); accepted when k > 0, the horizon is above the first fitted row's centre, the ground moves at least 20 px/s at the bottom and the fit's residual is under 0.75 of its variation. While travel is steady a picture whose rows do not fit keeps the last ground for 0.5 s (an obstacle over them). A window below the horizon is the ground, not an approach, when its residual against the ground is under 0.75 of its expansion or under 1.5 of its own fit's residual. Measured on the footage: the beach's windows at 0.5-0.8 of their expansion, the obstacle's at least 1.05 and 2.4 of its residual. Dropping fit tiles above the horizon instead (to fit windows whose rows reach past the ground) let the fit latch onto an obstacle's lower half: rejected.

| `costa_rica_4k60_hdr.mp4`, 300 pictures | before | now |
|---|---|---|
| The footage's foreground called looming | 91 of 292 | 2 (the ground found on 178) |
| Obstacle 2.0-2.8 s from contact, found | 0 of 15 | 48 of 48 (all by the fovea), TTC 4.5 % off (median) |
| Obstacle within 2 s, found | 14 of 14, TTC 2.9 % | 48 of 48, TTC 1.9 %, hazard within 0.26 half-sizes |
| Shaken window against still: looming | 63 against 65 | 17 against 15 |

| Synthetic (640x480, 30 fps; `media_perception_tests`) | before | now |
|---|---|---|
| Approaching obstacle: flagged | 53 of 55 | 55 of 55 |
| TTC error median / 90th percentile | 5.9 % / 17.4 % | 3.1 % / 11.3 % |
| CPU per picture (Release, mostly flat) | 0.65 ms | 1.8 ms (the fovea's 225 tiles and the centroids) |

Translation, the IMU pan, saliency and the shake test are unchanged (no looming, the same flow and masks).

## Rotation (2026-10-28)

Directive: "UNIFIED ARBITRARY/TKHD VIDEO ROTATION". The turned view was live ingest's only; now file playback turns too.

**The file's orientation.** The parser reads the video track's `tkhd` matrix ([layer 1](layer1.md#the-tracks-orientation-tkhds-matrix-2026-10-28)): a quarter turn or mirror of the axes. The coordinator gives it to the renderer before `Prepare` (`D3D12Renderer::SetOrientation`), which shapes the window by the picture as shown (a portrait phone recording opens portrait) and sizes the conversion slots for either way round (the larger fit along each axis), so the viewer can turn it later without new resources.

**The viewer's turns.** R and Shift+R turn a quarter clockwise and back (since 2026-10-29: Alt+R back, Shift+R [auto-levels](#auto-leveling-the-horizon-2026-10-29)), Ctrl+R and Ctrl+Shift+R level by a degree (at most 15 either way), and the commands "rotate video", "סובב תמונה", "tourner la vidéo", "level horizon" (since 2026-10-29 "straighten"; "level horizon" auto-levels) and the others ([intents](intents.md#rotation-2026-10-28)). While paused the picture is converted again at once (as for any picture setting).

**[`video::Orientation`](../include/media/video/Orientation.hpp)** (portable) composes them: D, the file's matrix then the viewer's turns (display = D x stored), its quarter turns and mirror, and the conversion's map. For an output picture of w x h pixels showing a region of rw x rh samples: an output pixel's centre X about the picture's centre, the leveling turn undone and zoomed in by f = |cos| + |sin| x the longer side over the shorter (the least that keeps the turned picture's corners inside the region), scaled to the region's samples as shown, then to its own axes (D transposed, D's inverse): q = M X + t. The window's picture shape is the region's shape as shown (rows and columns swapped by an odd quarter turn); a zoom's centre and the arrow keys' pan are brought back to the stored picture's axes (`ToStored`, `DirectionToStored`).

**The conversion.** `ColorConstants`' `kRotate` (since this directive a general affine map, `view_m`, `view_t`: 44 root constants) in the picture-controls variants only (ColorPipeline chooses them for it; the production shader is unchanged): `SourcePosition` is M X + t, `Scale()` the extent of an output step along each source axis (the area prefilter and the upscaling decision see the turned picture's true footprint), reads clamped to the crop (decoded surfaces have padding; live ingest adds `kSurfaceClamp` for its surface). No pass of its own, no allocation; neural reconstruction is left out while turned (its 2x image is of the upright region), edge-directed instead. Micro-motion starts over with each change of turn.

**Verified** (`media_playback_rotate90_test`, `_rotate180_test`, `_rotate270_test`, `_mirror_test`, new; their fixtures 64x32 I_PCM H.264 with the tkhd matrix, a dark left half, a light right half and a white square near the top-left corner): each picture's 4 x 4 luma grid (the screen capture while playing, the paused conversion while paused) against the stored picture sampled through `Orientation::Map`:

| | shown | worst cell from the prediction (codes) |
|---|---|---|
| tkhd 90 / 180 / 270 / mirror, at open, playing | 405x810 / 640x320 / 405x810 / 640x320 | 0.8 / 0.8 / 1.0 / 0.8 |
| R while playing (90 file: shown at 180) | 405x203 | 1.6 |
| R while paused (shown at 270) | 405x810 | 1.0 |
| three "level horizon right" (3 degrees, zoomed 1.10x) | 405x810 | 9.7 |
| "level horizon", then "סובב תמונה" (upright) | 405x203 | 2.5 |

0 pictures dropped. `media_rotation_tests` (portable): the map's corners (a 90-degree file's shown top-right is the stored top-left, exactly), every turn leveled up to 15 degrees keeps the corners inside the region, the compositions and the commands.

## Pitch, crops and speed in perception (2026-10-28)

Directive: "PITCH-AWARE HORIZON GROUND MODEL + SIMD PERCEPTION (< 0.5 ms)". Two notes of 2026-10-27: the ground was fitted to the bottom six rows only (a camera pitched up or down, or a window cut from a larger picture, has its horizon elsewhere), and the fovea had made perception cost 1.8 ms a 640x480 picture.

**The ground wherever the horizon is.** [`video::Perception`](../include/media/video/Perception.hpp) fits the ground to the most coarse rows from the bottom (10 down to 3) that one ground explains: rows reaching past the horizon put the quadratic's vertex among them and are refused, so the run stops at the horizon wherever it is. The last ground's horizon, moved by the IMU's pitch (focal x pitch, as Push moves a tile), says which rows to try first, and the held ground moves with it. `u` has its own coefficient ku: a ground spreads sideways half as fast as it falls (ku about k), an obstacle's isotropic expansion fitted the same way gives about 2k: a ground is accepted for ku / k from 0.5 to 1.6. And the ground under a travelling camera reaches the bottom of the picture and spreads across it: its tiles must reach the last three rows and cover 40 % of the width. Without that, a still camera's obstacle over a flat background (its rows the only textured ones) was fitted as a ground on 36 of 51 pictures of the SDK's test, with a ground's shape (ku / k 1.3-1.56) and a horizon just above its top; the fovea's rule below then hid the obstacle on 5 of them. `media_perception_tests` and `media_c_api_tests` now require no ground for a still camera. Tried first and measured worse: a focus of expansion from the flow lines anchoring the rows (it failed on the far, noisy flow: 82 of 292 pictures of the footage, 190 of a window with a low horizon), and fewer rows at most (6: the high-horizon window loomed on 65 pictures instead of 51).

**The fovea over a ground.** It is for small, distant obstacles, which stand near the horizon: with a ground, a fine window below the ground's far third is left to the coarse windows (a near obstacle is large). Over the near beach its small windows did not fit the coarse ground's model (42 of the 60 unexplained windows of the high-horizon window).

| `costa_rica_4k60_hdr.mp4` | 2026-10-27 | now |
|---|---|---|
| A: the beach looming / the ground found | 2 / 178 | 5 / 195 |
| B: shaken against still, looming | 17 / 15 | 10 / 13 |
| C: within 2 s: found, TTC error; 2-2.8 s: found, TTC error | 48 / 48, 1.9 %; 48 / 48, 4.5 % | 47 / 47, 1.7 %; 46 / 46, 4.9 % |
| D: 960x540 window, horizon low (camera up): ground / looming | (bottom-rows model) 0 / 10 | 45 / 10 |
| D: horizon high (camera down) | 200 / 55 | 200 / 31 |
| D: pitching +-80 px at 0.3 Hz with the IMU | 121 / 21 | 135 / 13 |

**Speed.** Measured first, per 640x480 picture of the approach (the synthetic scene): the cells 167 us, coarse matching 117, the fovea's 334, saliency 72, looming fits 99.
- The cells: each cell row's lines added into 16-bit column sums (16 a step with SSE2), then each cell's columns; the division by a cell's sample count is a multiply and shift (Granlund and Montgomery: four reciprocals, exact for every numerator under 2^25). 167 -> 78 us; the same cells.
- Saliency: without the IMU's rotation each cell is compared with the same cell (what the bilinear registration gives, exactly).
- The search's cost: the zero-mean SAD with the mean difference rounded to a whole cell unit, sum |(a - b) - round((sum a - sum b) / 64)|, every term in 16 bits: SSE2 a row of 8 cells a step (subtract, absolute value as max(e, -e), pairs summed by `madd`), AVX2 two rows a step (`abs`) where the processor and the system have it (CPUID and XGETBV, chosen at start), the scalar loop the reference. It is within a cell unit a term of the exact zero-mean SAD (1/64 of its scale): the thresholds moved with it, and every perception figure above was measured with it.
- `Perception::ForceLevel` (0 scalar, 1 SSE2, 2 AVX2) for tests; `MEDIA_PERCEPTION_SCALAR` builds the scalar loop alone (the portable suite builds both).

| 640x480, Release, i7-10700 | approach (an obstacle on grey) | a textured pan under shake |
|---|---|---|
| scalar | 1.29 ms | 4.9-5.4 ms |
| SSE2 | 0.50 ms | 1.86-1.93 ms |
| AVX2 | 0.43-0.45 ms | 1.60-1.63 ms |

Every level gives the same results bit for bit (`media_perception_tests`: 280 picture comparisons, results and tile flows, 0 differing; also under GCC `-O2` and ASan/UBSan, and the forced-scalar build). The textured pan is the worst case (every tile textured, all 481 searched): 0.6 ms is met where most of the picture is not textured, not there.

## Auto-leveling the horizon (2026-10-29)

Directive: "GPU AUTONOMOUS HORIZON AUTO-LEVELING". Leveling was the viewer's: the windowed engine never has a picture on the CPU, so nothing could estimate a tilt. Now the conversion's statistics pass measures it on the GPU and only 1,024 bytes of histograms come back.

**On the GPU** ([`HdrToneMap.hlsl`](../src/render/HdrToneMap.hlsl) `Orientation`, called by `Stats` when `ColorConstants::filter_flags` has `kHorizon`, 32; no pass of its own). The crop is cut into about 80 blocks across (8 texels a side up to 640 wide, 16 at 1280, 48 at 3840: a multiple of 8), one per thread of the statistics grid's top-left corner, so the working threads are contiguous and whole waves work or none. Each block's structure tensor is summed from Scharr gradients at the 8 x 8 texels at its centre (ten rows of ten read two at a time, five Gathers a pair). A block counts when its edge is strong (energy 0.005: about 9 codes across it) and straight (coherence 0.85: texture's blocks have several directions); its gradient direction modulo 90 degrees is its line's tilt, clockwise on the picture. Near-horizontal and near-vertical lines go to two histograms of 64 bins of 0.5 degree over [-16, 16), each bin two words: the weight (16 x coherence squared) and the weight times where in the bin the angle fell (256ths). They live in the peak-state buffer after the statistics ([64, 1088); the buffer grew to 1,088 bytes), are copied with the statistics into the per-slot readback (`ColorPipeline::RecordStats`' new `horizon` arguments) and cleared by `StatsReset` (now 64 threads).

**On the CPU** ([`video::HorizonLevel`](../include/media/video/HorizonLevel.hpp), portable, no allocation). `Measure`: both histograms added (vertical lines at half weight: perspective tilts verticals away from the centre, a horizon is level everywhere), the five bins with the most weight against the histogram's median bin (texture's orientations spread evenly), a picture has a tilt when that peak holds at least 96 over the background and twice its share of it; the tilt is the mean shift of the weight above the background within a degree, from the peak's bin until it settles, each bin at its blocks' mean place in it. `Push`: the last 15 tilts found; their median is the target (a pole or a moving object's lines for a picture or two do not tilt the screen), locked once 5 agree within a degree; the level shown glides to it with a 0.25 s time constant of presentation time. `Accumulate` is the shader's arithmetic on the CPU (tests, and hosts that have CPU pictures).

**In the renderer.** `D3D12Renderer::SetAutoLevel` (Shift+R; "auto level", "level horizon", "יישר אופק אוטומטית", "niveler l'horizon": [intents](intents.md#auto-leveling-2026-10-29)) sets `kHorizon` on every conversion that folds into the statistics (re-conversions of a paused picture have none). `HarvestScenes` reads each picture's histograms when its fence has passed, in conversion order, of the current timeline only, and `FollowHorizon` turns the leveling turn to minus the tilt (a mirrored view the other way), in tenths of a degree, within 15. A manual level (Ctrl+R, "level horizon right", "straighten") turns it off; a new file starts its measurements over. Shift+R, which turned a quarter back, now toggles it; Alt+R turns back. The tilt is measured on the stored picture, whatever the view shows: the loop is open (the turn it causes does not feed back into what it measures).

**Measured first and rejected.** On an antialiased half-plane, the blocks' median tilt against the truth: central differences +20 %, and 16 % of the blocks at exactly 0 degrees (a sharp edge between two sample columns reads as level); point samples, or 2x2 means, two texels apart: the same spike at 0 (a +4.5 degree scene's peak at 0); Sobel -10 %; Scharr within 0.15 degree from 4.5 to 14. Gradients at every other texel brought the spike back. A fixed five-bin centroid read +4.5 as +5.42 (an aliasing peak at 4.24 degrees on the window's edge): hence the mean shift. 8-texel blocks everywhere cost 0.42 ms more a 4K picture on the UHD 630 for no better tilt; 16-texel blocks at 640 wide had a quarter of the blocks and read -6.0 as -5.68. The test scene first drawn with 2 x 2 samples and no blur read 0.5 to 0.6 degree off (its edges were staircases, a camera's are not): it is drawn with 4 x 4 samples and a 0.7 px blur.

**Verified** (`media_auto_horizon_tests`, new; its scene, [`horizon_scene.hpp`](../tests/horizon_scene.hpp): sky over a horizon at 0.45 of the height, three buildings with lit windows on it, a textured ground, turned clockwise by the tilt):

| | |
|---|---|
| Still 1280x720, CPU histograms: +4.5 / -6.0 / 0 / +12 / -14 degrees | +4.48 / -6.00 / +0.03 / +11.90 / -14.02 (worst 0.10) |
| 90 pictures at 30/s (+4.5, then -6.0 from picture 30; noise; a bright pole at +11 degrees every seventh) | locked after 5 pictures; 0.17 degree from +4.5 at picture 30, 0.05 from -6.0 at the end, never past either; 0 allocations |
| GPU histograms, 1280x720 NV12: +4.5 / -6.0 | +4.47 / -6.00 (the CPU reference the same; each class's weight equal: 2,714 / 2,714, 2,049 / 2,049) |
| converted with the leveling turn: the output's own lines | +0.03 / -0.07 |
| the statistics pass, without / with the histograms (UHD 630) | 1280x720 0.057 / 0.138 ms; 3840x2160 0.32 / 0.40-0.41 ms |

`media_playback_auto_horizon_test` (new; `media_make_horizon_fixture --horizon`: 6 s of the scene at 640x352, I_PCM H.264 with an IDR every 8 pictures, +4.5 degrees for 3 s, then -6.0), quality Fast, Shift+R at 0.5 s:

| | level | the horizon's tilt | the shown picture's own lines (screen capture) |
|---|---|---|---|
| 0.5 s, before | 0 | | +4.56 |
| 2.8 s (+4.5) | -4.6 | +4.58, locked | 0.00 |
| 5.8 s (-6.0) | +6.0 | -5.96, locked | +0.03 |

121 of 121 pictures measured from 121 readbacks of 1,024 bytes; 0 dropped, 0 allocations; each capture's 4 x 4 grid within 12 codes of the stored picture sampled through `Orientation::Map`. Quality Fast because the check measures the shown image's lines: through the edge-directed kernel and sharpening at the leveled view's 1.12x zoom they read +0.40 degree where Fast reads 0.00 (the geometry is the same: the grid matched the map either way).

**Limits.** One tilt for the whole picture: lines in perspective (a building's horizontals seen at an angle, a road) are lines too, and a scene of them alone is leveled to their mean; a picture without a strong straight line keeps the last tilt. Letterbox bars encoded in the picture are perfect horizontal lines (a tilted picture inside them reads level). Within 15 degrees. Measured on synthetic scenes and the fixture only: no real tilted footage. (2026-10-30: [the bars' edges are left out, perspective has no tilt, and real footage is measured](#black-bars-perspective-and-real-footage-2026-10-30).)

## The predicted search, the surf and the low horizon (2026-10-29)

Directive: "DIAMOND-SIMD TEXTURED FLOW (< 0.55 ms), & NON-RIGID WATER/TURBULENCE REJECTION". The three open points of [2026-10-28](#pitch-crops-and-speed-in-perception-2026-10-28).

**The search** ([`video::Perception`](../include/media/video/Perception.hpp) `MatchTile`). Every tile searched all 169 positions of its window. Now it is seeded where the IMU puts it (the window's centre) and where its own flow in the previous picture and its left and upper neighbours' in this one predict it (each scaled by the baseline tried), and the small diamond (four neighbours) descends from the cheapest seed to a local minimum. When that costs under 0.35 of the tile's own texture (sum |v - mean| of its cells; an unrelated position costs about that or more) the search ends; else the window on a 4 x 4 lattice (every 4 cells, corners included) gives the mean cost the reliability test uses, and a second descent from its cheapest point catches a motion no seed predicted. Each position's cost is computed once per tile, by the same scalar, SSE2 or AVX2 code as before, a list at a time; every level visits the same positions in the same order, so the results stay equal bit for bit. Positions per 640x480 pan picture: 81,000 -> about 5,800.

Then what the search had hidden, measured per section: saliency's per-cell registration (the shift of a cell is now per row and per column; without roll every cell moves alike, one integer bilinear weight for all, row by row; `std::lround` per cell was a library call), and the windows' fits (no square root per tile, the median only when an error passes 8 px/s). The centroid of a tile's texture is integer sums by row and column (the same values).

| 640x480 per picture, Release, i7-10700 | approach (synthetic) | textured pan under shake (synthetic) | the footage panning (E) |
|---|---|---|---|
| scalar | 1.29 -> 0.40-0.41 ms | 4.9-5.4 -> 0.84 ms | 0.82-0.85 ms |
| SSE2 | 0.50 -> 0.22-0.23 ms | 1.86-1.93 -> 0.49-0.50 ms | 0.50-0.52 ms |
| AVX2 | 0.43-0.45 -> 0.21-0.22 ms | 1.60-1.63 -> 0.48 ms | 0.49-0.51 ms |

280 synthetic and 600 footage picture comparisons, 0 differing. `media_perception_tests` now requires the pan under 0.6 ms too, `media_real_perception_tests` its new window E (a 640x480 window of the footage panning 120 px/s with the IMU) under 0.6 ms and equal at every level (optimised builds).

**The surf.** Measured first: in the high-horizon window, each of the 33 false approaches was one to three accepted windows (water washing in over the wet sand, the sky's reflections on it, foam), while the obstacle pasted on the footage had 42 to 48 a picture, of both levels; and the surf's windows expanded unevenly (a11 / a22 from 0.36 to 2.55; the obstacle's within 0.69-1.24 on 80 % of its windows). An approach is now reported when:
- at least 4 accepted windows lie on its object (within a tenth of the picture of the strongest's focus: a rigid surface expands about one centre, so many overlapping windows see it);
- each window expands both ways, the smaller at least half the larger (0.35 before);
- the object persists: its centre within a tenth of the picture of the last object found, at most 0.15 s before.

The first picture of an approach is therefore not reported (nothing before it). Ablation on the footage (2026-10-29's search): the support alone took every window to 0 false approaches; persistence alone left 17 in the high-horizon window (12 with the isotropy at 0.5) and lost the far obstacle on 3 of 48 pictures; all three keep it on 48 of 48.

**The low horizon.** A picture whose rows do not fit a ground keeps the last one (its horizon moved by the IMU's pitch, as before) while the tiles below that horizon, however few rows, still fit it with its two offsets refitted (residual under 0.75 of its own spread, the fit's own test). Before, with one to three rows of ground visible, the ground was dropped after 0.5 s.

| `costa_rica_4k60_hdr.mp4` | 2026-10-28 | now |
|---|---|---|
| A: the beach looming / the ground found | 5 / 195 | 0 / 181 |
| B: shaken against still, looming | 10 / 13 | 0 / 0 |
| C: within 2 s: found, TTC error | 47 / 47, 1.7 % | 47 / 48, 1.9 % |
| C: 2-2.8 s: found, TTC error | 46 / 46, 4.9 % | 48 / 48, 5.0 % (at least 12 windows on it) |
| D: horizon low: ground / looming | 45 / 10 | 292 / 0 |
| D: horizon high | 200 / 31 | 274 / 0 (at most 2 windows on any surf) |
| D: pitching | 135 / 13 | 188 / 0 |
| synthetic approach: flagged, TTC median / p90 | 55 / 55, 3.8 % / 11.4 % | 55 / 55, 3.4 % / 9.6 % |

(C's pictures are those where the obstacle is the strongest approach: with the footage's own looming gone, there are more of them.) The SDK's approaching obstacle: 51 of 51 frames, TTC error 4.2 %.

**Limits.** A small obstacle covered by fewer than four windows is not reported (two coarse tiles wide over moving water had 12). Persistence costs the first picture of every approach (17 ms at 60 pictures a second). The confirmed ground can hold through many pictures of little evidence: an obstacle over its last rows breaks it after 0.5 s, as before.

## Black bars, perspective and real footage (2026-10-30)

Directive: "AUTOMATIC LETTERBOX/MATTE DETECTION & CROPPING, ... REAL-FOOTAGE HORIZON VERIFICATION". Three limits of [auto-leveling](#auto-leveling-the-horizon-2026-10-29): bars encoded in the picture are perfect horizontal lines, a scene of lines in perspective was leveled to their mean, and no real footage had been measured.

**The bars** ([`video::Matte`](../include/media/video/Matte.hpp), portable, no allocation). One picture's evidence is its extents: its first and last rows and columns with picture in them, where a sample brighter than black by more than 0.03 of full scale (about 8 codes) is picture, black being the range's (16 limited, 0 full). On the GPU the statistics pass finds them (`HdrToneMap.hlsl` `Matte`, `ColorConstants::filter_flags` `kMatte`, 64; no pass of its own): the first 32 threads of each row of its grid read one of 32 columns spread over the crop at the even rows of their block, the first 32 of each column one of 32 rows at its even columns, and each group keeps the largest of 65536 - first row, last row + 1, 65536 - first column, last column + 1 (zero: none), one atomic maximum each into [48, 64) of the peak state, copied with the statistics (`ColorPipeline::kStatsBytes` 40 -> 56) and cleared by `StatsReset`. Even samples only: an edge is found at most one sample inside the picture, never in the bar. `Matte::Accumulate` is the same arithmetic on the CPU (the GPU's words equal the CPU's exactly). `Matte::Push` keeps each side's bar: thinner at once when a picture has picture inside it (the active area grows: nothing shown is ever cropped away, burned-in subtitles in a bar included), thicker (and the first time) only when the pictures have agreed on one thickness within 2 samples for 1 s of presentation time and at least 8 pictures (a bar holds still; a dark scene's edges move from picture to picture). A black picture is no evidence. `Bars()`: a side at least 1 % of its dimension (and more than 2 samples).

**The horizon through them.** A block whose 10 x 10 window has a whole end row (a near-horizontal line) or end column (near-vertical) at most 0.03 over black, and whose line is within 1 degree of the picture's axis, is the edge of a bar, not of the scene: `Orientation` and `HorizonLevel::Accumulate` leave it out (`kBarAxis`). And `Measure` now also finds each class's own peak (near-horizontal and near-vertical lines, each held to the 96 of mass the combined histogram is): when both peak more than 3 degrees apart, the picture is lines in perspective (a camera's roll turns horizontals and verticals alike; vanishing lines do not agree) and has no tilt (`Measurement::perspective`; `classes`, `lines[2]` say what was found).

**Auto-crop** (X; "crop black bars", "auto crop", "חתוך שוליים שחורים", "recadrer les bandes noires": [intents](intents.md#black-bars-2026-10-30)). `D3D12Renderer::SetAutoCrop`: once the bars are known (at once if they are) the view zooms so that the picture inside them fills it: the crop's shape inside the active area (two samples in from each bar: the region's even rounding never shows a line of it), as large as it fits, about the area's centre, through the zoom's own region (`FollowBars`; no pass). A 2.39:1 film in a 16:9 window therefore loses its sides, as zooming by hand would. It follows the active area while on; a zoom by hand (the wheel, "zoom in", "reset zoom") turns it off, and off the view is whole again. The extents are pushed only from pictures converted whole (a zoomed region shows none of its bars): while cropped the bars are not measured. `kMatte` is set on every conversion until the bars are known, then on every fourth (an unflagged picture's words stay zero: no evidence). A new file starts its bars over; auto-crop stays as the viewer left it.

**Measured** (`media_auto_horizon_tests`; the scene of [2026-10-29](#auto-leveling-the-horizon-2026-10-29); `horizon_scene::Bars`: limited-range black with an encoder's +-1 code of noise):

| | |
|---|---|
| tilted +4.5 / -6.0 inside a 2.39:1 letterbox (92 rows each side), CPU | +4.46 / -6.00 |
| the same, the bar test blinded (black taken as code 0) | no tilt, twice (the bars' 160 blocks at exactly 0 against the scene's, then the classes disagree) |
| inside a 4:3 pillarbox (160 columns each side) | +4.47 / -6.06 (blinded the same: vertical lines weigh half, the scene outweighs them) |
| the GPU's histograms, letterbox / pillarbox | +4.46 / -6.00, +4.47 / -6.06; the extents the CPU's words exactly |
| a facade, its horizontal edges receding at +7 degrees, verticals vertical | the classes' peaks +6.97 and +0.02: perspective, no tilt |
| the facade level / rolled +4.5 as a whole | +0.00 / +4.48 |
| `Matte`, 45 pictures at 30/s: letterbox / pillarbox / no bars | locked after 32 pictures: (0, 92)-(1279, 627) / (160, 0)-(1119, 719) against (0, 92)-(1280, 628) / (160, 0)-(1120, 720); no bars found in the scene alone |
| a burned-in subtitle in the bottom bar (rows 650-669) | the bar thinner at once: bottom 669 |
| a dark sky (16-18) whose one bright point moves every picture | no bar; 0 allocations throughout |
| the statistics pass (UHD 630): alone / with the extents / with the histograms too | 1280x720 0.060 / 0.074 / 0.179 ms; 3840x2160 0.331 / 0.406 / 0.556 ms |
| the CPU survey (histograms, extents, measure; what the SDK's host mode does per picture) | 640x480 1.09 ms, 1280x720 0.87 ms (Release, one core; 640 has 8-sample blocks: more of them). Since 2026-10-31 0.10 / 0.08 ms with AVX2: [the CPU survey](#the-cpu-survey-2026-10-31) |

The extents first read 64 lines at every sample: 0.2 ms more a 4K picture. 32 lines at even samples cost 0.075 ms, and only on every fourth picture once the bars are known.

**Real footage** (`media_auto_horizon_tests` with the fixtures directory; `costa_rica_4k60_hdr.mp4`, a beach with the sea's horizon at the left and a tree line, decoded by Media Foundation for the test only): 60 pictures (every other one of 2 s), a 960x540 window of each turned about its centre as a camera's roll would (bilinear, half a sample off the source's grid), with and without a 2.39:1 letterbox (69 rows), against the same window unturned:

| turned | median of the pictures' tilts (against unturned + roll) | the estimator's level, locked (against unturned + roll) |
|---|---|---|
| 0 | -1.46 (letterboxed -1.48) | -1.62 (-1.58) |
| +4.5 | +2.74 against +3.04 (letterboxed +2.76 / +3.02) | +2.53 / +2.88 (+2.54 / +2.92) |
| -6.0 | -7.51 against -7.46 (-7.61 / -7.48) | -7.61 / -7.62 (-7.73 / -7.58) |

Measured on all 60 pictures every time; worst 0.38 degree from the roll; a turned, letterboxed picture through the GPU: +3.61, the CPU +3.61. The footage's own lines are not one horizon: they are a spread of the wet sand's orientations from -5.5 to +2.5 degrees around the shore's (one picture's histogram), so turning the window (which changes what its corners hold) and resampling reweight them: sampling the unturned window on the source's grid instead of half a sample off read it 0.1 to 0.3 degree differently. Weighting blocks by edge strength (tried: up to 16x) moved nothing and measured fewer pictures. The footage is held to 0.5 degree, the synthetic scenes to 0.35.

**Playback** (`media_playback_crop_bars_test`, new; `media_make_horizon_letterbox_fixture --horizon-letterbox`: the auto-horizon fixture with 40 rows of black at the top and the bottom; quality Fast, Shift+R at 0.5 s, "recadrer les bandes noires" after the picture at 2.8 s):

| | level | the horizon's tilt (GPU) | bars, view | the shown image |
|---|---|---|---|---|
| 0.5 s, before | 0 | | not yet known | the bars on screen (picture from row 40); its lines +4.58 (the bars' edges left out) |
| 2.8 s (+4.5) | -4.6 | +4.60, locked | found: (0, 40)-(639, 311); whole | leveled (the bars turn with it) |
| 5.8 s (-6.0) | +5.9 | -5.88, locked | cropped: region (76, 42)-(562, 310) | picture from edge to edge; its lines -0.04 |

0 dropped, 0 allocations; each capture's grid within 10 codes of the stored picture (bars and all) sampled through the map in the view's region. `media_playback_auto_horizon_test` unchanged: +4.58 / -5.96, its shown lines -0.00 / +0.03.

**Limits.** One tilt for the picture still: a scene with a single class of lines in perspective (a road's edges without verticals) is leveled to them. The bars are those of near-black: a coloured or grey matte is picture. A static, dark shot of 1 s whose edge rows stay below the threshold reads as a bar until a brighter picture thins it (at once). While cropped the bars are not measured (an aspect change inside a film is seen when the crop is turned off). The footage's horizon is a spread of lines: within 0.4 degree there, not 0.1.

## The CPU survey (2026-10-31)

Directive: "SIMD CPU TELEMETRY SURVEY (< 0.25 ms)". The SDK's host mode surveys the pictures it holds on the CPU (`HostEngine::Survey`: `HorizonLevel::Accumulate`, `Measure`, `Matte::Accumulate`; [sdk](sdk.md#rotation-the-horizon-and-black-bars-api-6-2026-10-30)) when telemetry, auto-leveling or auto-crop is on: 1.09 ms a 640x480 picture on one core. Now 0.10 ms.

**The histograms in integers.** Each block's structure tensor came from float gradients of code / 255 (the shader's arithmetic). The Scharr gradients of the 8-bit codes themselves, 3 (a - c) + 10 (d - f) + 3 (g - i), are 32 x 255 = 8160 times those and exact: a gradient fits 16 bits (at most 16 x 255 = 4080 each way) and a sum of 64 squares 31 bits (64 x 4080² = 1,065,369,600). So the tensor is three integers whatever computes it, and the block's line (strength, straightness, angle, the bar test, its bin) comes from them in one scalar function, in double: no square root (the weight is coherence², and the straightness test compares d² + 4 jxy² with 0.85² energy²). Separable: per row of a block's 10 x 10 window its horizontal difference c - a and smoothing 3 a + 10 b + 3 c at the 8 centre columns, then per centre row gx = 3 (dx above + dx below) + 10 dx and gy = sx below - sx above.

**Three levels, the same words.** The tensors with SSE2 (a block's row of 8 centre columns in one register, the squares and products summed in pairs with `madd` into 32-bit lanes) and AVX2 (two blocks a register: the first block's row in the low 8 lanes, the second's in the high), chosen as perception's levels are (`HorizonLevel::ForceLevel`: 0 scalar, 1 SSE2, 2 AVX2; `MEDIA_SURVEY_SCALAR` builds the scalar loops only; `src/video/SimdLevel.hpp`). An RGBA channel (`step` 4) is read by the scalar loop. Every level gives the same histogram words, bit for bit.

**The extents, from each end.** `Matte::Accumulate` read every even row and column (each until its first bright sample). Its words are only the first and last even row (column) with a bright sample among the sampled columns (rows), so it now searches from each end and stops at the picture's edge: the same four words; a picture without bars reads a handful of samples. The threshold is the least code that passes the shader's float comparison (the same answer for every code).

**The cadence.** Once the bars are known (`Matte::Locked`), the host engine surveys the extents on every fourth picture, as the windowed engine's statistics pass does (`kMatte`); the histograms every picture.

| Measured (Release, one core, best of 25) | before | scalar | SSE2 | AVX2 |
|---|---|---|---|---|
| 640x480: histograms + extents + measure | 1.087 ms | 0.656 ms | 0.171 ms | **0.104 ms** |
| 1280x720 | 0.865 ms | 0.528 ms | 0.133 ms | 0.078 ms |

`media_auto_horizon_tests` (`TestSurvey`; MSVC Release and Debug, GCC -O2 and ASan/UBSan, and GCC with `MEDIA_SURVEY_SCALAR`): ten pictures (640x480 +4.5 degrees, letterboxed -6.0, 1280x720 +12 pillarboxed, -14 windowboxed, 641x479 for the blocks' tails, 1920x1080, 200x130, noise, black, a subtitle in a bar): the histograms of every level equal bit for bit, the RGBA channel's equal too; against the float histograms (the old code, kept in the test) the same verdicts and the same tilts to 0.000 degree on all eight with a tilt; the extents the old loops' words exactly, limited and full range and RGBA; 0 allocations; at most 0.25 ms at 640x480 (Release, SSE2 or better). Unchanged: the still pictures' +4.48 / -6.00 / +0.03 / +11.90 / -14.02, the bars' +4.46 / -6.00 and +4.47 / -6.06, the GPU's histograms (their weights equal to the CPU's: 2714 / 2049 and 2559 / 2169) and extents, the SDK's +4.40 for +4.5 through its letterbox.

## Several objects at once (2026-10-31)

Directive: "MULTI-OBJECT THREAT & TARGET TRACKING RADAR". Perception reported one approach (the strongest window's object) and a saliency mask. Now it also keeps a table of objects ([`video::PerceivedObject`](../include/media/video/Perception.hpp), `PerceptionResult::objects`, up to four) with track ids that stay with them, target tracking moves from one to the next (Tab while tracking, "next target", "מטרה הבאה", "cible suivante"), and the audio zoom's beam goes with the view.

**What an object is.** Each picture (`Perception::Objects`, after `Approach`):
- *Approaches*: the accepted expansion windows (both levels, as before) taken apart by non-maximum suppression: the strongest window's object (every window whose focus is within a tenth of the picture of it) is an approach when it has at least `kMinSupport` (4) windows; its windows are set aside (with too few, they go too: surf, reflections), then the strongest left, up to four. Each: the median of its windows' expansions and foci.
- *Salient regions*: the saliency mask taken apart into regions of tiles at most `kObjectGap` (2) tiles apart each way. Measured first with 8-neighbours: an approaching square's interior changes little while its edges change, so its salient tiles came in two pieces a tile apart, which became two objects with ids that swapped from picture to picture. A region needs `kMinObjectTiles` (2) tiles and at most `kMaxObjectShare` (0.4) of the grid: a larger one is the scene (a travelling camera's whole field), not an object.
- An approach whose focus is in a region, or next to one, is that region: its extent (the tiles' bounds), its centre (saliency-weighted) and its energy, with the approach's expansion. A region two approaches take (two obstacles grown to a tile of each other: measured, the bridge then fused them, the table's centre for the nearer one moved 0.12 of the picture and the other was left a one-tile box) is split between them, each tile to the nearest focus. Approaches outside the mask stand alone, a tile around their focus at least.

**The tracks.** Before the tracks, the most urgent four (approaches by expansion, then regions by energy). Each continues the track whose prediction (its last centre moved by its velocity over the time since) is nearest, within `kTrackGate` (0.15 of the picture) plus half the object's size, nearest pairs first; otherwise it gets a new id (1, 2, ...: not reused until `Configure`; a cut or a seek (`Reset`) lets every track go but keeps counting). Up to `kTracks` (8) are held; one unseen for `kTrackHold` (0.5 s) is let go. A track's velocity is its centre's motion, the mean of the last two estimates. An approach is confirmed (`approaching`, `ttc_seconds` = 2 / divergence) when its track had one at most `kPersistence` (0.15 s) before, the rule the single approach already followed. The table: confirmed approaches by time to contact, soonest first, then the rest by energy. The single approach (`looming`, `ttc_seconds`, `hazard_*`) is unchanged.

**Next target.** `HostEngine::TrackObject` (the SDK's `media_engine_track_object`, `NextTarget`) and `D3D12Renderer::NextTarget` (Tab while tracking; `PlaybackCoordinator::NextTarget`): the object after the one followed in the table's order (the first when none is; with two, they alternate), or one by id. The view goes to it at once (to 2x from a whole view), so the audio focus's beam turns with it (it follows the view); the tracker locks onto the patch at the object's centre on the next picture (the next converted one in the windowed engine: the pictures already converted keep their framing). Which object the lock is on: each picture, the one holding the tracker's position (its box and a twentieth of the picture around it), the target's own track first (`TargetId`; the SDK's `target_track_id` and `MEDIA_OBJECT_TARGET`). Without tracking, Tab is the dock's focus as before; "next target" starts tracking at the most urgent object.

**In the windowed engine** perception runs on the luma grids target tracking already reads back (the crop at the tracker's cell size, at most 480x272: a 320x240 file's own size, a 4K one's 480x270), in conversion order, only while tracking (since 2026-11-01 one grid every 100 ms, tracking or not, on a thread of its own: [Objects that cross](#objects-that-cross-2026-11-01)), on the presentation thread; its buffers (324.5 KiB at 480 cells) come from the telemetry module's region (448 KiB then, 576 KiB since 2026-11-01; `D3D12Renderer::PrepareTargets`). A grid under 128 cells a side has no objects.

| Measured | |
|---|---|
| perception, two squares approaching a still 640x480 camera (A 3 m at 1.0 m/s, B 4 m at 0.9 m/s, B larger on screen), from 0.4 s | both in the table on 54 of 54 pictures, ids 2 and 1 kept on 54 of 54; both confirmed on 26, A first on 26; TTC median error 3.3 % / 7.1 %; no object off the squares |
| its cost | 435 µs a 640x480 picture (Release; the table a few µs of it) |
| the SDK, audio zoom on (A's tone left, B's right) | "next target" -> A, -> B, "מטרה הבאה" -> A, `track_object(B)` -> B: the target id right on every picture, the view on each (0.257 / 0.711 / 0.275 / 0.721); A's tone over B's +4.1 / -6.4 / +3.7 / -6.6 dB |
| the windowed engine (`media_playback_next_target_test`, 320x240 at 24/s, all I_PCM: A 4 m at 0.45 m/s, B 5 m at 0.60 m/s) | T at 1 s; Tab at 2 s -> B (id 1), Tab at 3.5 s -> A (id 2), "cible suivante" at 5 s -> B (id 1 again): the tracker, the view and the audio focus on each square 0.6 s later; 0 dropped |
| the GPU image (`media_multi_target_tests`), quarter turns 0-3 | the camera's bright square where each turn puts it, within 0.2 px; the region 640x480 / 360x480 |

## Objects that cross (2026-11-01)

Directive: "MILESTONE 200/200: OCCLUSION/CROSSING-SAFE MULTI-OBJECT TRACKING, INSTANT `T -> Tab` TARGET SEEDING, 90° GPU FIT/FILL MODE & FORENSIC FRAME SNAPSHOT". Two limits recorded with [several objects at once](#several-objects-at-once-2026-10-31): two objects crossing could swap ids, and Tab right after T found nothing.

**What went wrong, measured first.** Two textured squares (A bright, 30 px half size; B dark, 22 px) on an X through the same point, B passing behind A, and two squares passing side by side with their edges a tile apart, over a still textured background at 30 pictures a second (`media_multi_target_tests`). Once they come within `kObjectGap` tiles they are one salient region, one object, its centre between them. On the X the table held one object on all 22 pictures of the pass and B came out of it with a new id (3, on 23 of 23 pictures after); side by side A came out with a new id and B with A's (swapped). The track velocities were the centres' differences, which jump by a tile when a region grows, merges or splits: +0.17 to +0.33 widths a second for A's true 0.27.

**The fix** (`Perception::Objects`, `Extrapolate`):
- *A track predicts at constant velocity from its own path*: the least-squares line through its last eight measured centres (at least three over 80 ms; before that its flow). The tiles' flow alone, tried first, reads about 0.7 of an object's speed where its edges cross a still background (a tile half on the square matches somewhere between), so predictions fell behind during the pass; the line through the centres is unbiased: the velocity error is now 18 % (X) and 7 % (side by side) median, as a vector.
- *A region holding two or more tracks' predictions is split between them* (a prediction within a tile of the region's bounds): each tile goes to the track whose prediction is nearest, against its size, plus 0.5 x how far the tile's flow is from the track's velocity (capped). A part is a measurement of its track only when its flow moves as the track does (the same direction within cos 0.8, the speed within 1.8x either way): on the X, the hidden square's part is mostly the other's tiles (its flow read a third of its speed) and corrupted its velocity until this test. Otherwise the track goes on along its line, and a part of fewer than two tiles leaves it *hidden* (`PerceivedObject::occluded`, the SDK's `MEDIA_OBJECT_OCCLUDED`): reported where its line puts it, its size as last seen, while it stays on the other's region, for at most `kTrackHold`.
- *The other objects are given their tracks by the least total cost*, over every assignment of up to four objects to the eight tracks (a table over the tracks used: 4 x 256 x 8 steps): the distance to the prediction against the gate, plus 0.6 x (1 - cos) / 2 of the object's motion (its flow, or where its centre went) against the track's, 0.3 x the log of their tile counts' ratio (over ln 4), 0.15 x the log of their aspect ratios', 0.5 x their tiles' mean codes' difference over 40 codes (each term capped at 1). An object no track takes costs 3, more than any pair inside the gate, so a track is never left for a new one; the costs only choose between pairs. A track's look (tiles, width, height, mean code) is updated only while it is alone.

| Measured (`media_multi_target_tests`) | before | now |
|---|---|---|
| X, both ids in the table while within two tiles | 0 of 22 pictures | 22 of 22 |
| X, after: A's id, B's id | 23 / 23 kept, 0 / 23 (a new id) | 25 / 25, 25 / 25 |
| side by side, both ids while passing | 0 of 20 | 20 of 20 |
| side by side, after | 0 / 25, 0 / 25 (A a new id, B A's) | 26 / 26, 26 / 26 |
| the two approaching squares ([before](#several-objects-at-once-2026-10-31)) | 54 / 54, ids kept, TTC 3.3 % / 7.1 % | unchanged; 440 µs a picture (435) |

**T, then Tab at once.** The windowed engine perceived only while tracking (the grids are read back for the tracker), and perception needs two pictures to compare, so Tab right after T found no object. Now `D3D12Renderer` reads a grid every 100 ms of media while not tracking too (the newest converted picture that far from the last, in conversion order), and perception takes one grid every 100 ms tracking or not: measured, perception on every picture after T at 24/s lost the slowly approaching square (its change from one picture to the next was under saliency's floor; over 100 ms it is not), so the second Tab found it the only object. And T makes the object at the lock point the target at once (the host engine too: `HostEngine::ObjectAt`), so the first Tab moves on from it. Perception runs on a thread of its own (`D3D12Renderer::PerceiveThread`, the telemetry module's thread): on the presentation thread a push costs 0.4 ms in Release but several in Debug, and ten times a second in every playback that dropped pictures (`media_playback_speed_test` and `media_fixture_subtitle_search_test` in Debug, 2 of 3 and 1 of 3 runs alone; 3 of 3 with the warm grids off). The presentation thread copies the grid (at most 127.5 KiB, a row at a time) and hands it over when the thread is idle; the thread publishes the table into the other of two copies, which Tab and the tracker's object read; a grid due while it is busy waits for the next. The cost while not tracking: a grid pass on the GPU and 0.4 ms of perception ten times a second, and only with room on the GPU (the last conversion took at most half the picture's interval, the governor not at its Fast tier): measured first without that, a 4K60 HDR file at the Fast tier (12 ms of 16.7 a conversion) dropped 3 to 10 pictures to the grid pass every sixth picture; there T works as before (perception from the lock on). A new epoch (a seek's landing) starts the cadence 100 ms in: with a grid on the landing picture's own conversion, a film at 60/s dropped the picture after each of its seeks ("waited for its conversion").

| Measured | |
|---|---|
| host mode (`media_multi_target_tests`): the view on the table's first object, "track the target", then "next target" on the very next picture | the target is the first object at once (0 before: "next" chose by the table's order); then the other object at once, the view at x 0.726 (it is at 0.73); the tracker on it 6 pictures later |
| the windowed engine (`media_playback_next_target_test`): T at 1 s after 12 warm grids, Tab on the very next picture | the view moved at once (0.042 s of media after T) onto B, 2 objects in the table; Tab at 2 s -> A, "cible suivante" at 3.5 s -> B; 0 dropped (3 of 3 runs) |

## The GPU image's fit (2026-11-01)

An odd quarter turn of the GPU image converted a region of the output's turned shape, so the turned picture filled the image and the camera's ends were left out (360 of 640 samples across). `MEDIA_SENSOR_GPU_FIT` ([sdk](sdk.md#fit-instead-of-fill-api-8-2026-11-01)) shows the view whole: `LiveGpuPipeline` converts the view's own region with one scale both ways, the larger (the whole region inside the image), about the centres, and the conversion's new `kBars` flag (`Reconstruct.hlsli`, with `kRotate`) writes black where an output pixel's centre maps outside the region, after everything else: the pixels inside are what they are without it, bit for bit (their sharpening ring reads the clamped picture, not the bars). `media_multi_target_tests`: a 640x480 camera with a marker in each corner (24, 16, 32 and 20 samples) into a 640x480 image; at 90 and 270 degrees the picture is 360x480 in the middle (`picture_left` 140, `picture_right` 500, `MEDIA_GPU_FITTED`), all four markers where the turn puts them (0.0 px off, their size the scale's), every byte of the bars 0 and the picture's edge columns lit; at 0 and 180 degrees nothing is barred.

## Forensic snapshots (2026-11-01)

S (and "take snapshot", "save frame", "צילום מסך", "capturer l'image", the nine languages; [intents](intents.md#snapshots-2026-11-01)) writes the picture as the engine made it: an uncompressed 24-bit BMP and a sidecar of its telemetry beside it ([`video::Snapshot`](../include/media/video/Snapshot.hpp), portable; [sdk](sdk.md#snapshots-api-8-2026-11-01)).

- **The image**: BITMAPFILEHEADER and BITMAPINFOHEADER (54 bytes; 24 bits, no compression, a negative height: the rows top to bottom), each row B G R, padded with zeros to 4 bytes. Any reader opens it (Windows, browsers, PIL). The *frame hash*: FNV-1a 64 over the image's R G B bytes, rows top to bottom, no padding: what a reader of the file computes, and what `SnapshotEncoder::HashRgba` gives for the GPU image.
- **The sidecar** (`<name>.json`, "media-engine-snapshot/1"): the image's name, size and hash, the source ("window", "gpu-image", "view"), the media, the picture's time (100 ns units and h:mm:ss.mmm), the view (zoom, centre, roll, the region of the picture shown), the orientation (quarter turns, mirror, leveling, auto-leveling, fit), the horizon's tilt when known, the picture controls, the reconstruction, the comparison, the target and the objects perceived (id, place, size, velocity, approaching and time to contact, hidden, target). Fixed decimals, strings escaped; nothing allocated (`FormatSidecar` writes into a buffer).
- **The windowed engine**: the picture as composed on screen (the window's video rectangle: zoom, tracking, leveling, the turns, Original | Enhanced, Pinned | Live), before captions, the dock and the status cues. `D3D12Renderer::RequestSnapshot` marks the next composite, which copies that rectangle into a readback buffer (since 2026-11-02 made when asked for, of the window's size, by the snapshot thread, and released once the files are written: playing holds none, where it held the slots' capacity, 33 MB for a 4K file; measured on the next-target test: 0 bytes before S, 1,228,800 for its 640x480 window, 0 after; paused, the picture held is composed again for it); once its fence has passed, a UI pass hands the pixels and what they show to the snapshot thread (`PlaybackCoordinator`, started with the file), which writes the files through `platform::OutputFile` (CreateFileW and WriteFile: no C library stream, nothing allocated) and says so; the next UI pass shows "Snapshot saved · <name>". The UI thread neither waits nor writes. One at a time: another S while one is on its way is refused with a cue. Its name: the media's stem and the picture's time (`clip_0h01m02.345s.bmp`, `_2` for a name taken), in the snapshot folder (by default `Pictures\MediaEngine` in the user's profile).
- **Host mode** (the C SDK, on the host's thread, during the call): with `MEDIA_SENSOR_GPU`, the newest GPU image exactly; otherwise the view of the picture the engine holds (its software bridge's, a camera's): the view's region, turned and mirrored as shown, converted to R'G'B' as the GPU converts (BT.709, limited range, 16.16 fixed point), sample for sample; the leveling turn and the stabiliser's roll are the host's to draw (the sidecar has them).
- **Keys**: S takes a snapshot; the demo's binaural rendering moved from S to Shift+S ([audio](audio-dsp.md)).

| Measured (`media_snapshot_tests`, suite #200) | |
|---|---|
| the encoder, a 37x23 RGBA image | the header as written, every pixel back, the padding zero, the hash equal to an FNV-1a computed apart; 0 allocations |
| the sidecar | well-formed JSON (a parser in the test), 27 of 27 fields as given, a path's quotes and backslashes escaped, UTF-8 kept; 0 allocations |
| host mode, a 640x480 camera, 2x zoom and a quarter turn | 240x320: every pixel the stored sample converted apart, within 0 codes (1 allowed); the file's hash the record's and the sidecar's; the sidecar's objects telemetry's; "take snapshot" to a free name in the folder; 0 allocations while saving |
| host mode with the GPU path, shadow lift 0.5, 2x zoom, 3° leveling | every byte of R G B the GPU image's; the same hash |
| the windowed engine (hidden, the two-targets fixture, 2x zoom) | "take snapshot" and `media_engine_save_snapshot`: both 640x480, each file's hash its record's; a second "take snapshot" at once refused; 0 dropped, 0 allocations in the commands |
| S while tracking (`media_playback_next_target_test`) | written at 4.2 s by the snapshot thread; its sidecar names the target followed and marks its object; 0 dropped |

## Remote vibrometry (2026-11-02)

Directive: "PROJECT BIONIC CORTEX", invention 1. Periodic motion and flicker measured from the picture alone: a machine's or a drone's vibration in hertz, a face's pulse in beats a minute (remote photoplethysmography: the skin's colour follows the blood volume; ballistocardiography: the head moves with each beat). [`video::Vibrometry`](../include/media/video/Vibrometry.hpp), portable; its storage is the object (~25 KB), nothing allocated.

- **The region** is held still in the scene. Host mode (the SDK, the software bridge's pictures and live cameras): the view's fovea, half the view across and down at its centre, moved with the camera's shake. The shake is the IMU's motion (`HostEngine::SceneMotion`, accumulated) less its part slower than 3 s, so a pan moves the region away gently while a shake does not move the scene under it. While tracking, the target's region instead: moved only when the target has left a third of it. A region that followed every step of the target would also follow its vibration. Each move starts the window over.
- **Five signals per picture**: the region's Y', Cb and Cr means, through `video::AreaMean`: the plane read as constant over each sample's square, so a region moved by a fraction of a sample changes its mean continuously, never in steps. And the content's own displacement across and down: Lucas-Kanade on a 24 x 24 grid of the region's cell means against a reference grid. The reference's gradients are each cell's mean moved half a sample either way on the reference picture. Differences of neighbouring cells, tried first, alias fine texture: a 0.3-sample vibration read as 0.55. After half a cell the reference is taken again, the series continuing from where it stands.
- **The spectrum**, every fourth picture, over the last 256 (8.5 s at 30 a second):
  - each signal is detrended (a least-squares line) and weighted by a Hann window over the window's span;
  - it is transformed at the pictures' own times: a DFT of uneven samples, so a late picture is not a wrong one;
  - the grid is 1/(2 x span), from 0.7 Hz to min(15 Hz, 0.45 x the picture rate); the strongest bin is refined by golden-section search on the same transform, to a thousandth of a hertz.
- **Confidence** is the peak's power over the band's median power, mapped from 20 (nothing) to 60 (certain). White noise's strongest bin stands ~8x above its median; a tone stands thousands of times above. A signal flatter than its floor (0.01 code, 0.001 sample) has no peak at all, so a still scene and a noise scene both report 0.
- **The vibration** is the channel whose peak stands highest. **The vital rate** is the strongest within 0.75–3.5 Hz (45–210 BPM). Rejected as a pulse: a component where a harmonic (2nd to 6th) of a stronger one folds at the picture rate. Measured: an 8 Hz flicker's 4th harmonic folds to 2 Hz at 30 a second, and read as a pulse of 120 (confidence 0.34) before.
- **SDK (API 9)**: `media_frame_telemetry_t` gains `vibration_hz`, `vibrometry_confidence`, `vibration_amplitude` and `vibrometry_channel`; `vital_bpm`, `vital_confidence` and `vital_channel`; `vibrometry_seconds`; and the region measured. The new flags are `MEDIA_TELEMETRY_VIBRATION` and `MEDIA_TELEMETRY_VITALS` ([sdk](sdk.md#remote-vibrometry-the-event-camera-the-bitstream-and-tensors-api-9-2026-11-02)).
- **The windowed engine**: the tracking grids, luma only (the GPU's 8-bit cell means that the tracker already reads back), feed the same estimator while tracking (T: a grid every picture) or with micro-motion on ("micro motion on": the warm grids, ten a second, which cover the pulse band). The HUD shows "Pulse 72 BPM", "Vibration 8.0 Hz" or both, every 2 s of media while a component stands above the noise (`D3D12Renderer::Vibrate`; the text is assembled in place, no formatting library).

| Measured (`media_vibrometry_tests`, suite #201; synthetic 320x240 at 30/s, ±1 code of noise, 10 s) | Result |
|---|---|
| a pulse, ±1.5 codes at 1.20 Hz on a textured, side-lit skin patch | 72.00 BPM, confidence 1.00 (the same with the camera's shake compensated, and with the times jittered ±3 ms) |
| the same shake, not compensated | 138.00 BPM: the shake's 2.3 Hz, the lighting under the fovea moving 2.4 codes (what the compensation is for) |
| a flicker, ±1.5 codes at 8.0 Hz | 8.000 Hz, confidence 1.00, no vital rate (its folded harmonic rejected); the same under the shake |
| a vibration, the patch's texture moving ±0.3 sample at 8.0 Hz | 8.000 Hz on the shift channel, amplitude 0.301 sample; the same under the shake |
| a still scene; a noise scene (±10 codes a sample a picture) | confidence 0, no frequency (the noise's peak 7.6 x its median) |
| cost | 82–130 µs a picture (Release); 0 allocations |
| the SDK, a 320x240 camera whose IMU reports the shake | 72.00 BPM, `MEDIA_TELEMETRY_VITALS`, the fovea's region; API 8's 400-byte struct still accepted; a still scene says nothing; 0 allocations in the ingests |
| the windowed engine (`media_playback_vibrometry_test`, a 160x128 pulse fixture at 24/s) | tracking: 72.00 BPM over 4.96 s of grids at 24/s; micro-motion: 72.00 BPM over 5.38 s at 8/s; the HUD said "Pulse 72 BPM"; 0 dropped |

**Limits.** The window must span 2.5 s (32 pictures) before anything is reported. The vital rate is a component in the pulse band: the engine does not know that the region is skin. Until 2026-11-03 a tracked target that moved kept restarting the window, file playback without an IMU was not steadied, and the windowed engine measured luma on its grid: see [moving targets](#moving-targets-2026-11-03), [the virtual IMU](#the-virtual-imu-2026-11-03) and [vibrometry in the window](#vibrometry-in-the-window-2026-11-03).

## Event camera (2026-11-02)

Directive invention 2: any camera or file as a neuromorphic event camera (a dynamic vision sensor), stabilised. [`video::EventCamera`](../include/media/video/EventCamera.hpp), portable, nothing allocated after its arena region (at most ~3.5 MiB, made at open with telemetry).

- **The sensor**: the luma box-averaged by an integer scale to at most 640 x 480 (VGA-class: 1080p is 3 x 3 samples a pixel, 4K 6 x 6), kept in sixteenths of a code. Each pixel keeps the log intensity at which it last fired: log of the linearised (gamma 2.2) value + 1/64, in Q10 through a 4,096-entry table, so darks are damped as a sensor's are. It fires an ON (+1) or OFF (−1) event each time its log intensity moves by the contrast threshold C (0.15 by default, ~16 %), at most 4 a picture (beyond that, its reference jumps to the picture).
- **Time order**: a change d fires its k-th crossing at k·C/|d| of the interval between two pictures, in 16 slices. A picture's events are written in slice order (raster order within a slice), so the stream never goes back. Timestamps are the slices' ends in microseconds of the pictures' clock.
- **Stabilised** (the default): the references live in a frame fixed to the scene. Each picture's camera motion moves that frame in the camera's: the IMU's shift at the centre and its roll, the same `SceneMotion` vibrometry uses. Each reference pixel is compared with the picture sampled where it now lies. Three things make a shaking camera fire nothing, each measured on a still scene under a rolling shake (unstabilised: 1.2 million events in 89 pictures):
  - **bicubic** (Catmull-Rom) sampling of the codes: bilinear leaves a code's error at an edge (0.08 % of the pixels fired a picture);
  - **codes before logs**: interpolating logs across an edge makes its geometric mean, ~30 codes too dark between 40 and 200 (0.18 %);
  - a **registration guard**: each change reduced by what a misplacement of 0.1 pixel makes of the reference's local slope. A sharp 160-code edge is ~1.4 log units a pixel, so a tenth of a pixel fires it; a moving edge's change is many times that (0.05 % without the guard, 0 with it).
- **Re-basing**: after a pan of 6 pixels the scene's frame moves toward the camera's by whole pixels. Its references are copied, not resampled: a blurred reference fires at its sharp edges. Pixels that come into view take the picture's value. After 0.03 radian of turn the references start over from the picture (which fires nothing). Coordinates are the scene frame's: steady under shake, re-based after a pan.
- **The ring**: 65,536 events of 16 bytes, single producer (the host thread's ingest), single consumer (`media_poll_event_spikes`, any one thread), cursors on cache lines of their own. A picture's events are published at once. What does not fit is counted dropped, from the latest slices back.
- **SDK (API 9)**: `media_engine_set_event_camera` (the threshold, the per-pixel cap, `MEDIA_EVENTS_UNSTABILIZED`; null: off), `media_poll_event_spikes`, `media_query_event_camera` (the sensor's size and scale, the counts, re-bases). Since 2026-11-03 (API 10) the windowed engine too, on its tracking grids, and pictures without IMU deltas stabilised by [the virtual IMU](#the-virtual-imu-2026-11-03): [the event camera and tensors in the window](#the-event-camera-and-tensors-in-the-window-2026-11-03).

| Measured (`media_neuromorphic_spike_tests`, suite #202; 320x240 pictures integrated over each pixel's area, a 40x40 square with 1.5-sample edges) | Result |
|---|---|
| the square moving right 3 samples a picture, a still camera | 29,060 ON all on the leading edge, 28,985 OFF all on the trailing edge, 0 elsewhere, in time order; ~220 µs a picture; 0 allocations |
| a still scene under a rolling shake (3 + 1.5 px across, 2.5 px down, 0.01 rad) | 141 events stabilised against 1,201,527 unstabilised (0.012 %); every one on the square's sharp border |
| a still scene, the camera panning 135 px and banking 0.11 rad in 3 s | 215 events, 23 re-bases |
| the moving square under the shake | 95.5 % of ON on the leading edge, 98.7 % of OFF on the trailing |
| the ring, unread | 65,536 written, the rest dropped (the latest first), then taken in order; 0 allocations polling |
| the SDK, a 1280x720 camera (a 640x360 sensor), the shake from the IMU | 97 % of events on the square's edges, never going back; unstabilised 24x more; off: nothing; 0 allocations |

## Bitstream perception (2026-11-02)

Directive invention 3, part 1: what a compressed H.264 or HEVC picture says about the scene before a pixel is decoded, so a host can scan hours of recordings, or decode only when something happens. [`codec::BitstreamPerception`](../include/media/codec/BitstreamPerception.hpp), portable, ~9 KB of tables in the object, nothing allocated; `media_bitstream_inspector_t` is the caller's storage (no engine).

- **Headers**: the parameter sets from the avcC / hvcC record and in band. Each slice is read to `slice_qp_delta`:
  - **H.264**: frame_num, POC (types 0 and 1), redundant_pic_cnt, the reference counts' override, list modification, weight tables, adaptive marking; the PPS read past FMO maps, the SPS past scaling matrices.
  - **HEVC**: the short-term reference picture sets (the SPS's, with inter-set prediction's derivation of each set's deltas, and the slice's own), long-term pictures, SAO, the temporal MVP's collocated picture, list modification (NumPicTotalCurr from the set's pictures in use), weight tables; the PPS read past tiles and scaling lists. Since 2026-11-03 every SPS's sets, not the newest's alone: [several sequences in one bitstream](#several-sequences-in-one-bitstream-2026-11-03).
- **Complexity**: the slices' bits per pixel scaled to QP 26 (x 2^((QP − 26)/6)): what the content cost, not what rate control allowed. Without the QP, measured on a hardware HEVC encoder: 3x the bits on the picture after each intra one, and next to nothing more on a cut, with nothing new in view.
- **Activity**: an inter picture's complexity over the recent intra pictures' (0 still, 1 all new). It measures new detail, not speed: a rigid object's translation, however fast, costs a real encoder a motion vector.
- **Bursts**: complexity over its kind's mean (P and B apart). The mean falls fast and rises slowly, so a burst's level does not hide a cut after it.
- **Scene cuts**:
  - an inter picture costing at least half an intra one and several times its kind's mean. It is confirmed by the next picture of its kind falling back below half its cost, or withdrawn when that picture does not (water or a crowd suddenly churning over a third of the picture looks the same for one picture: measured with both platform encoders). The revision comes on the later picture's answer (`MEDIA_BITSTREAM_REVISION`);
  - an intra picture early for the GOP's beat;
  - a scheduled intra picture whose cost is unlike the last ones'.
  
  The first two inter pictures after an intra one are not judged (an encoder's prediction structure around its key pictures costs there). A cut one or two pictures after a key can be missed.

| Measured (`media_bitstream_perception_tests`, suite #203) | Result |
|---|---|
| headers written bit by bit: Baseline P; High CABAC B (scaling matrix, POC type 1, fields allowed, override, list modification, bipred weights, MMCO); IDR; FMO; HEVC B with its dependent segment, P with a predicted set of its own, IDR | QP 23, 23, 35, 22; HEVC 25 (both segments), 35, 20; types and keys right |
| a conditional-replenishment stream (I_PCM where a macroblock changed, skipped elsewhere), 170 pictures | the cut 1.00 as a P picture (confirmed by the next), 0.90 as an early IDR; every other picture 0.00; the burst flagged; activity 0.030 slow, 0.090 burst |
| Microsoft's H.264 MFT (High, CABAC, B pictures), a cut and a burst of new detail | QP known on every picture; the cut 0.95 at its picture and nowhere else (the burst's onset withdrawn); activity 0.032 slow, 0.938 burst; 0.24 µs a picture |
| the Intel hardware HEVC encoder (every inter picture a B slice), the same scene | QP known on every picture; the cut 1.00 at its picture and nowhere else; activity 0.095 slow, 0.616 burst; 0.34 µs a picture |
| cost (the SDK's inspector, Release) | 0.10 µs a picture (~10 million a second; the directive asked < 20 µs); 0 allocations |

### On a real reel: the hierarchical GOP and the encoder's clock (2026-11-03)

The head-to-head benchmark (docs/benchmarks.md) ran the inspector over 3,000 pictures of `lg_black_4k60_hdr.mp4` (4K HEVC Main10) and found 252 confirmed cuts where ffmpeg's `scdet`, decoding every pixel, found 3. Each picture read out (the harness's `scan <file> <pictures> frames`) showed why:

- **The cadence.** Every inter picture is a B slice at QP 26, in mini-GOPs of 4 in decode order: an anchor predicted from 4 pictures back, then one from 2, then two from 1 (738, 738 and 1,499 of them). The anchor costs 3-5x its neighbours with nothing new in view. Against one mean for every B picture, each anchor was a burst, and the cheap picture after it "confirmed" a cut.
- **The clock.** An IDR every 248 pictures whatever the content. The reel fades in and out of black, so one IDR costs 5-14x the one 4 s before it: three of them scored 0.85.

What changed:

- `BitstreamFrame::reference_distance` (the nearest reference in use, from the slice's short-term set) and `layer` (HEVC's temporal sub-layer; H.264: reference or not) say where a picture sits. Inter pictures keep a mean, and a pending cut, per class: P or B times the distance in octaves (1, 2, 3-4, 5-8, 9+), or the layer when the distance is unknown.
- Against its own class a burst of 1.5-3x is already rare: the probability's burst term moved from 2-5x to 1.5-3x.
- In a hierarchical GOP the anchor is predicted across any cut in its mini-GOP. A narrower picture that costs more while its anchor did not (a flash on one leaf: picture 467 of the reel, checked by eye) is no longer judged a cut.
- An intra picture on the GOP's beat (the encoder's clock) scores at most `kClockKey` (0.4). One early for the beat (an encoder's scene detection put it there) still scores 0.9, and an irregular key stream (x264 placing keys at cuts, as in `costa_rica_4k60_hdr.mp4`) is unchanged. The price: a cut that lands exactly on a clock-scheduled IDR is missed, since the bitstream cannot tell it from a fade.

| `lg_black_4k60_hdr.mp4`, 3,000 pictures | Before | After |
|---|---|---|
| cuts | 258 candidates, 252 of them confirmed by a later picture | 3 final cuts (first look, or the revision): 765, 1325, 2125 |
| against `scdet`'s 765, 2128, 2612 (within 4 pictures: decode order) | not scored (that run kept counts only); at least 249 extra | 2 matched |
| missed | | none since the step down (below); before it 2612 (champagne bubbles into slow honey, every class's cost falling 4-8x) |
| extra | | 1325: a still teapot, checked by eye; an anchor 2.7x its class before the shot's motion stops |

**The step down (2026-11-03, later the same day).** The missed cut fell instead of rising, so a second rule reads falls: two inter pictures each `kStepDown` (3.5x) or more under their class's mean, within `kDropSpan` (4) pictures of each other, after a level that held `kSustain` pictures (no intra picture, cut candidate or burst of new detail since) and was busy (the class's mean at least `kBusy`, a tenth, of an intra picture's). The cut is the first, revised on the second (`BitstreamFrame::step_down`). Classes too cheap to be busy (a hierarchy's leaves) neither confirm nor break a drop, and a dropped picture leaves its class's mean alone until the drop is decided (the next of its class is judged against the old level).

- `kSustain` is 30, not the directive's 8: at 8, the platform encoders' 15-picture burst of churning detail (pictures 40-54) read as a cut where it stopped (unit 56, in all three streams). Its own pictures had "held" a level for about 16 pictures. LG's champagne had held for hundreds. The price: a burst of new detail that lasts longer than half a second at 60/s and stops at once is read as a cut.
- LG now: 765, 1325, 2125, 2613 against `scdet`'s 765, 2128, 2612: all 3 found, 1325 still extra. Costa: unchanged (no step down fires).
- `TestHierarchy` adds a 15-picture 4x burst that stops at once (not a cut) and a 5x step down into a simpler shot (found at its picture). With the rule off, the step down is missed; with `kSustain` 0, the platform streams' burst ends are read as cuts.

`costa_rica_4k60_hdr.mp4` (H.264, irregular keys) is unchanged: 8 cuts, 7 of `scdet`'s 8 (410 missed, 1716 extra). 0.21-0.25 µs a picture on both. `media_bitstream_perception_tests` now carries the reel's shape (`TestHierarchy`): an IDR every 64 pictures fading in 5x a GOP, mini-GOPs of 4 whose anchor costs 5x, a flash on one leaf, one cut on an anchor. The cut alone is found (1.00); the flash 0.00; the clock IDRs at most 0.40. With one class per type the same stream gives 47 cuts; without the clock's cap, IDRs at 0.85.

## Tensor export (2026-11-02)

Directive invention 3, part 2: the picture a model takes, in the caller's buffer, ready for PyTorch, ONNX Runtime or TensorRT without a copy. [`video::ExportTensor`](../include/media/video/TensorExport.hpp), portable, nothing allocated.

- **Layout**: NCHW, N = 1: each channel a contiguous height x width plane, rows top to bottom. R G B (B G R with `MEDIA_TENSOR_BGR`), or Y' alone.
- **Values**: each element is its cell of the region (the region divided into width x height cells), area-averaged on each plane (`AreaMean`: a box filter when shrinking, the samples' squares when enlarging). It is converted from BT.709 limited range (Y' (Y − 16)/219, Cb and Cr about 128 over 224) to R'G'B' clamped to [0, 1] (gamma-encoded, as images are given to models), then normalised: [0, 1], ImageNet's (v − mean)/std (0.485 0.456 0.406 / 0.229 0.224 0.225; grey 0.449 / 0.226), or [−1, 1]. The arithmetic is in double, rounded once to float32. Float16 is the float32 element rounded to nearest even (`HalfFromFloat`: subnormals, infinities and NaN kept).
- **Regions** (`media_export_foveated_tensor`, host mode, the picture shown last):
  - the fovea: the tracked object's box grown by a fifth, or the view's fovea, shaped as the tensor (the smaller side grown, kept inside the picture);
  - the view shown (zoom, pan, tracking, stabilisation);
  - the whole picture;
  - a custom region in fractions.

  `media_tensor_info_t` says which region, in samples. Since 2026-11-03 (API 10) the windowed engine too, from the GPU: [the event camera and tensors in the window](#the-event-camera-and-tensors-in-the-window-2026-11-03).

| Measured (`media_tensor_export_tests`, suite #204) | Result |
|---|---|
| binary16 | 63,488 round trips and 63,486 midpoints (ties to even), 0 wrong; overflow, underflow and NaN right |
| a 64x48 picture of random codes at its own size | every element the reference conversion within 3e-8; B G R planes swapped exactly; float16 the float32 rounded, bit for bit; 0 allocations |
| half size; a fractional region to 10x7 | 2x2 means within 3e-8; against an area mean computed pixel by pixel, within 3e-8 |
| the SDK, a 320x240 camera | the whole picture exactly as the class makes it; "zoom in": the view (80, 60)-(240, 180); a square fovea 80 samples at the centre; float16 1x3x224x224 = 301,056 bytes; tracking a moving square, the fovea's centre 218.9 for its 218; 0 allocations |

## Moving targets (2026-11-03)

Directive "BIONIC CORTEX COMPLETENESS", limit 1: a tracked target that moved kept restarting remote vibrometry's 2.5 s window, because the region was re-centred (a new place, a new window) each time the target left a third of it.

- **The region follows the target's path.** [`video::TargetPath`](../include/media/video/Vibrometry.hpp) keeps the tracker's positions and gives the least-squares line through the last second of them at the picture's time. A target moving steadily is followed without lag (a low-pass would trail it), while the tracker's jitter and the target's own vibration (above ~1 Hz, what is measured) are averaged out of the region's place.
- **`Vibrometry::Push(..., follow)`**: the region may move a step at a time without restarting. The window starts over only on a jump of half the region in one picture (the tracker finding the target again elsewhere), a resize by a tenth, a change between following and not, or a `Reset` from the caller: a cut (the tracker's grid-mean test), a lost track, a lock on another target, a new epoch. The cells' reference moves with the region, so the displacement channels measure the target's motion about its path.
- **Illumination**: a target crossing the scene's lighting changes its brightness by more than its pulse, and along no straight line. While following, each signal is detrended by its own moving mean over 1/0.7 s (a centred box; gain ~1.1 at 1.2 Hz, nothing below ~0.35 Hz) instead of a least-squares line, and the luma's residual is scaled by the window's mean over that moving mean: the pulse as a share of the light falling on the target.
- **Where**: host mode (`HostEngine::Vibrate`) and the windowed engine (`D3D12Renderer::VibroRegion`, the path pushed at each tracker update and evaluated at each converted picture's time). The SDK sets `MEDIA_VIBROMETRY_FOLLOWING` in `media_frame_telemetry_t.vibrometry_flags` (API 10).

| Measured (`media_vibrometry_tests`; 320x240 at 30/s, an 80x80 textured skin patch pulsing ±1.5 codes at 1.20 Hz, crossing at 12 samples a second with a slow wave down, under a spotlight (1.0–1.3x) | Result |
|---|---|
| following its path (the tracker's positions with ±0.6 sample of jitter) | 71.96 BPM, confidence 1.00, 0 restarts, the whole 8.50 s window; the region within 1.39 samples of the target (a line through a curving path) |
| the policy before (re-centred after a third) | 2 restarts, never more than 4.37 s of window, nothing reported at the end |
| through the SDK, "track the target" when the patch is at the view's centre | 71.98 BPM over 8.50 s, `MEDIA_VIBROMETRY_FOLLOWING`, the region's centre (267.0, 133.6) for the target at (267.6, 132.4); 0 allocations |

A periodic texture on the target (19 and 23 samples) first let the tracker's 16x16-cell template slip by half a period in both axes (the region 16 samples off, 64.9 BPM); a texture with a unique centre (rings and a fading stripe) is followed within a sample. The tracker, not vibrometry, is what such a scene tests.

## The virtual IMU (2026-11-03)

Limit 3: shake compensation needed an IMU, so file playback and ordinary webcams were not steadied. [`video::VirtualImu`](../include/media/video/VirtualImu.hpp) reads the camera's motion off the background's, picture to picture, and hands it on as an IMU's `EgoMotion`: stabilisation, vibrometry's fovea, the event camera's references and perception's flow then work as they do with a real IMU. Portable, ~0.5 MiB from the arena, nothing allocated after Configure.

- **The working grid**: the picture box-averaged to at most 256 x 192 cells (a 4K picture's blocks read every second sample), kept for the next picture.
- **The global motion**: the half-resolution grids' mean-removed SAD over ±6 half cells around the last motion (a shake of a dozen cells a picture is found).
- **Each tile** (8 x 6) is refined by Lucas-Kanade from there: Gauss-Newton on the bilinear previous grid with the tile's own centred gradients and its mean difference taken out, so a flicker or a pulse is not motion. It counts when it is textured (its gradient tensor's weaker eigenvalue at least 4 codes² a cell), converged and did not run away.
- **The background**: perception's objects (moving or salient, from the picture before) are left out while 8 other tiles remain ("the median non-object tile"). Then the median motion, and the least-squares translation and turn (dx − turn·y, dy + turn·x about the centre) through the tiles within max(0.35 cell, 3 x their median deviation) of it, fitted twice. Too few agreeing tiles: no motion (the camera taken as still).
- **The roll** is the tiles' turn, every picture, in any scene. `HorizonLevel`, which the directive named, needs straight lines in view and settles over a dozen pictures: it levels the picture slowly and keeps doing so; it is not a per-picture gyroscope.
- **Host mode** (`HostEngine::Sense`): every picture the engine holds without IMU deltas (`MEDIA_SENSOR_IMU` unset; the software bridge's files) takes the virtual IMU's motion. `media_engine_set_stabilization` now steadies those too (with telemetry), and the telemetry says so: `MEDIA_TELEMETRY_VIRTUAL_IMU`, `ego_dx`, `ego_dy`, `ego_turn`, `ego_tiles`, `ego_inliers` (API 10). **The windowed engine** runs it on the tracking grids for its event camera (below), and since 2026-11-04 for its view: [Stabilising the window](#stabilising-the-window-2026-11-04).

| Measured (`media_vibrometry_tests`: the pulse scene shaken 3 + 1.5 px across and 2.5 px down at 2.3/5.1/3.7 Hz, recorded without IMU deltas) | Result |
|---|---|
| the virtual IMU's motion against the true shake | on 299 of 299 pictures, 44 of 44 tiles background, 0.068 samples rms |
| the stabilised view against the same engine fed the true IMU | 0.066 samples rms |
| the pulse in the fovea it steadies | 71.99 BPM, confidence 1.00 (138 BPM uncompensated) |
| the event camera stabilised by it | 355,579 events, against 383,330 with the true IMU and 13,242,384 unstabilised (the rest is this scene's point-sampled edges: the IMU leaves them too) |
| a still picture from C (`media_c_api_tests`, frames without deltas) | no motion where it reports one |
| cost (GCC -O2, scalar; a textured picture moving 1.3 samples of a 320-wide picture) | 0.48 ms a picture at 320x180, 1.33 ms at 1920x1080, 2.27 ms at 3840x2160 (the decimation grows with the picture; the grid's work does not) |

## Vibrometry in the window (2026-11-03)

Limit 2: the windowed player measured luma only, at the tracking grid's resolution (8x8-sample cells at 4K), so a face's colour change (remote photoplethysmography) and sub-sample vibration were out of reach.

- **On the GPU**: `HdrToneMap.hlsl Vibro` (`ColorPipeline::RecordVibro`) reads the region of the decoded picture itself, in the conversion's command list, at full resolution. One group of 64 threads per cell of a 24 x 24 grid computes area means with fractional edges (as `video::AreaMean`): the cell's Y', the same cell moved half a sample right, left, down and up (the displacement's gradients), and its Cb and Cr over the cell's half-size rectangle. 10-bit sources are in 8-bit codes (the code / 4). That is 4,032 floats (16,128 bytes) a picture into a per-slot readback.
- **On the CPU**: `video::VibrometrySample` hands those numbers to `Vibrometry::Push(sample, ...)`, which computes what it would from planes. The region's means are the cells' means; the reference's gradients are the GPU's moved cells. Samples are taken in conversion order once each conversion's fence has passed (`HarvestVibro`).
- **The region** is decided at conversion time: the target's path at that picture's time while tracking (the window goes on as it moves), else the view's fovea with micro-motion on. Only while one of them is on; nothing is recorded otherwise.

| Measured (`media_playback_vibrometry_test` and the new `media_playback_vibrometry_chroma_test`; 160x128 at 24/s, Release) | Result |
|---|---|
| the luma pulse fixture, tracking | 71.91 BPM (luma) over 4.79 s; micro-motion's fovea: 72.00 BPM over 5.46 s; "Pulse 72 BPM"; 0 dropped |
| the chroma pulse fixture (`--chroma-pulse`: luma still, Cr ±1.5 codes at 1.2 Hz, a ±3-code chroma texture dithering it), tracking | 71.91 BPM **in Cr**; micro-motion: 72.00 BPM in Cr; 0 dropped. A luma grid sees nothing there. |

## The event camera and tensors in the window (2026-11-03)

Limit 4: the event camera and tensor export needed pictures in host memory, so the windowed engine (whose pictures never leave the GPU) refused them.

- **The event camera** (`D3D12Renderer::PrepareSensing`, `SetEventCamera`, `Sense`): its sensor is the tracking grid, at most 480 x 272 cells (a 4K picture's 8x8 blocks), and at least 128 a side (perception's thread runs it: a smaller picture has none, `MEDIA_ERROR_UNSUPPORTED`). It and a virtual IMU on the grids are made at open from the telemetry module's region, which grew from 576 KiB to 3,392 KiB (2.09 MB for the sensor, 1 MiB of it its ring, and 480 KiB for the virtual IMU; since 2026-11-04 the region is 1,408 KiB: [the telemetry module's memory](#the-telemetry-modules-memory-2026-11-04)). While on, every converted picture's grid is read back and handed to perception's thread, which gives it to the sensor in conversion order, stabilised by the virtual IMU's motion between consecutive grids. A grid that comes while that thread is still busy is skipped for the sensor (the next interval is longer), never waited for: run first on the presentation thread, it made a Debug build drop 48 of 70 pictures (0 in Release); on perception's thread Debug drops none and compares 39 grids in 3 s (Release every one: 78, the decoded pictures ahead of the window's last included). A host's thread turns it on or off with one packed atomic (the presentation thread applies it at its next conversion) and polls the ring as host mode's (`media_poll_event_spikes`, one consumer). `media_query_event_camera` reads counters published after each picture.
- **Tensor export** (`D3D12Renderer::RequestTensor`, `WaitTensor`, `FinishTensor`): the asking thread makes or grows the cells' buffer and readback (the device is free-threaded: the presentation thread allocates nothing), then posts the request. The presentation thread claims it and records `HdrToneMap.hlsl Tensor` with its next conversion: one thread per element, the Y', Cb and Cr area means of its cell. The asking thread waits for that conversion's fence and converts the means with `video::ExportTensorMeans`, the same arithmetic as `ExportTensor`. The regions are host mode's: the fovea (the target while tracking), the region converted, the crop, fractions. At most 1024 x 1024 elements and one request at a time. While paused nothing is converted: the call returned `MEDIA_ERROR_EMPTY` after its second until 2026-11-04 ([tensors while paused](#tensors-while-paused-2026-11-04)); it still does before the first picture.

| Measured (Windows; the two-targets fixture: a still camera, two textured squares approaching; hidden, 3 s, Release) | Result |
|---|---|
| the event camera on (`media_neuromorphic_spike_tests`) | sensor 320x240; 54,131 events polled from the frame callback, 100.00 % within 3 cells of the squares (their known boxes at each event's time), in time order, 0 dropped, 0 allocations in the polls; 0 pictures dropped |
| a 64x48 float32 tensor of the whole picture (`media_tensor_export_tests`) | 32.8 ms; identical to host mode's export of the same picture (its software bridge decoding the same I_PCM pictures): worst difference 0.000000 |
| a 32x32 float16 ImageNet tensor of the fovea | 36.7 ms, the region [80, 40]-[240, 200] as host mode's; worst difference 0.0029 (a float16 step there is ~0.004); 0 allocations in the second export |
| before playing | `MEDIA_ERROR_EMPTY` after the second it waits |

## Several sequences in one bitstream (2026-11-03)

Limit 5: [bitstream perception](#bitstream-perception-2026-11-02) kept the HEVC reference picture sets of the newest SPS only, so a slice of another sequence had its type but not its QP (and its complexity was then read at rate control's swings).

- **A pool of sets**: each HEVC SPS's short-term sets are now kept, contiguous in one pool of 256 sets and 1,536 delta POCs (`BitstreamPerception::DropSets`, `StoreSet`, `LoadSet`), in the object: the inspector grew from 8,840 to 9,344 bytes, inside `media_bitstream_inspector_t`'s 10 KiB.
- An SPS sent again under its id replaces its sets (the pool compacted, the later sequences' places moved down). One whose sets do not fit is taken back whole: its slices have their type, not their QP, and the others are untouched. The decode-path parsers already kept every set by id (`H264SyntaxParser`: 32 SPS, 256 PPS; `HevcSyntaxParser`: 16 and 64); they refuse a set redefined with other content while it is in use, which needs a decoder reconfiguration.

| Measured (`media_bitstream_perception_tests`) | Result |
|---|---|
| handwritten HEVC: SPS 0 (320x192, two sets) and SPS 1 (640x360, three) at once, slices of each | QP 31 (320 wide), 20 (640 wide), 31 again; SPS 0 re-sent with three other sets: QP 27, SPS 1's 26; an SPS with 64 sets of 32 deltas (too many): its slice type P, QP unknown, the others 26 and 27 |
| the Intel HEVC encoder's two encodes (320x192, then 256x160 from the cut), the second's SPS and PPS renumbered 1, all four in the hvcC (SPS 1 newest) | QP known on 110 of 110 pictures; the cut 0.90 at its picture (the second encode's IDR, early for the GOP's beat) and nowhere else; 0.24 µs a picture |

## Stabilising the window (2026-11-04)

Directive "WINDOWED VIRTUAL-IMU VIDEO STABILIZATION", part A. The virtual IMU steadied host mode and the window's event camera, but not the windowed player's view: a shaky file shook on screen. Now **Alt+S**, "stabilize video" ([intents](intents.md#stabilisation-2026-11-04)) or `media_engine_set_stabilization` steady it, with the crop and the pan pass-through of host mode (`video::Stabilizer`).

**Why the motion is measured before the conversion.** Pictures are converted up to 16 ahead of the screen, so two designs were possible. The obvious one converts the whole crop and has the composite blit a shifted 1.25x window out of it: a bilinear enlargement of every picture, softer than what the same picture is without stabilisation, on what is meant to be a clean view. The other measures each picture first, so that the conversion can reconstruct exactly the view it will show, from the picture's own samples at the zoomed scale. That is what is done:

1. **The grid.** `Measure`, in `ConvertFrame`, once a free slot is known and before the picture joins the order: the picture's tracking grid (at most 480 x 272 cells) in a list of its own, into its slot's readback region. It costs one more submission a picture and no copy on the CPU.
2. **The motion.** On perception's thread (`Steady`; the presentation thread never runs it: [a lesson of the event camera](#the-event-camera-and-tensors-in-the-window-2026-11-03)), `video::VirtualImu` on the grid in place, the perceived objects left out, then `video::Stabilizer::PushDecoded`, then `Apply` for the view: its centre and turn. `Measure` returns `WouldBlock` meanwhile; the pacing waits on the fence and then on the thread's event (`ConvertBlock::Steadying`), so nothing spins.
3. **The view.** `Setup` composes the view with the picture's turns and leveling: `q = v + R(turn) (M X + t - r)`, `v` the steadied centre and `r` the region's, and hands it to the conversion as `kRotate`'s affine map (`view_m`, `view_t`), with the whole crop as what reads clamp to. Sub-sample shifts and the roll need no pass of their own. The region keeps its size and its even edges, so the composite's "was it converted for this region" test is unchanged: a picture shown at its own view is copied.

The grid is reused: a slot measured keeps its grid in the readback, so tracking, warm perception and the event camera take that one instead of recording another. The event camera's motion is the one measured (added up over grids it skips), not a second run of the virtual IMU, which could not be pushed the same grid twice.

**Decode order.** Pictures reach `Measure` in decode order: I P B B. Motion between consecutive pictures is still motion, and the sum of it is still where each picture lies, so `yaw_` needs nothing. Two things do:
- The low-pass needs time order. `PushDecoded`: a picture earlier than the newest leaves it alone and takes its value at its own time, between the two kept pictures around it. Host mode's `Push` would have restarted the low-pass there.
- The virtual IMU searches about the last motion, and going to a B-picture reverses it: the search then starts about none (`ForgetMotion`).

**The low-pass.** The first runs left 2.84 px rms on screen, 1.79 source samples: exactly what a one-pole of 0.35 s leaves of this shake (19 % of 2.3 Hz). The measurement was not the limit. The filter is now a second-order Butterworth of the same 0.35 s delay, each picture's interval solved exactly, in `video::Stabilizer`, so host mode has it too. A pan lags as before (0.348 s measured); 7.8 % of a 2.3 Hz shake stays in the view instead of 19.4 %.

**On and off.** A host's thread sets a flag; the presentation thread applies it (with the cue "Stabilisation on"). The view's crop changes as a zoom does: the pictures already converted go through the [live zoom](#live-zoom-2026-10-21)'s blit at once, and those converted next are steadied. A cut starts the view over at the middle (the grid's mean jumps), where nobody sees it move, and so does a seek. A paused picture keeps the view it was converted on; turned on while paused, it takes the crop without a shift. With the telemetry module left out, or under 128 samples a side (no perception thread), Alt+S says "Stabilisation · not on this picture" and the command is `Rejected`.

| Measured (`media_playback_stabilize_test`: the `--shaken` fixture, 480x272 at 24/s, I_PCM, a camera over a textured wall shaking 8.8 and 7.6 samples rms across and down at 2.3 to 6.7 Hz, turning ±0.4 degree, panning 16 samples a second; a white disc's place in each image composed, by a capture of the screen; jitter its distance from its own moving mean over a second; Release, UHD 630) | Result |
|---|---|
| as it comes | **14.73 px rms** (the window shows 1.27 px a sample) |
| stabilised, from 1.5 s after "stabiliser la vidéo" | **0.92 px rms** over 42 pictures; the filter's prediction was 0.94 |
| off again with Alt+S | 15.81 px rms (the 1.25x crop is gone later than the shake is back: its pictures are enlarged by it for a moment) |
| pictures on the steadied view, of them measured | 89, 88; 0 dropped, 0 allocations |
| the presentation thread's longest conversion call | 1.2 to 2.6 ms (before: about the same; the grid is recorded, not waited for) |

| Measured (real footage, the same mode; the disc's numbers mean nothing there) | Dropped |
|---|---|
| `costa_rica_4k60_hdr.mp4` (720p59.94 H.264 with B-pictures), 3.7 s stabilised | 0 of 958 pictures; 221 of 222 measured |
| `lg_black_4k60_hdr.mp4` (4K60 Main10 PQ, HEVC), 3.7 s stabilised | 0 of 956 pictures; 19 of 221 measured: this demo is mostly black, and a picture without texture leaves the camera still |

| Measured (`media_vibrometry_tests`, "Decode order": 320x180, a wall of value noise, the same shake, I P B B) | Result |
|---|---|
| the view against the same pictures in presentation order | 0.50 samples rms apart |
| the shake, what is left of it on the view | 6.53 samples rms, 0.25 |
| the low-pass: of a 2.3 Hz shake kept (a one-pole of that delay: 0.194); a pan's lag | 0.078; 0.348 s |

**Limits.**
- The grid pass is one more on the GPU for every picture. Neural reconstruction is not used on a turned view: stabilised, the picture is edge-directed.
- The motion is one translation and one turn of the whole picture: no rolling-shutter correction and no parallax. A real camera's translation near a subject moves its depth planes apart; the background's fit takes the dominant one.
- A shake larger than the margin (10 % of the picture a side) is held at its edge. Motion of more than about 24 cells a picture is not found; a cell is two to eight samples.
- `lg_black_4k60_hdr.mp4`, near black, gives few measurements: stabilisation does nothing where there is nothing to see.
- A wall that repeats fools the search by a whole period: the first decode-order test, on sines, was 39 samples off, and a noise wall was 0.5.

## Tensors while paused (2026-11-04)

Part B, gap 1: the window's tensors came from the next converted picture, so a paused engine answered `MEDIA_ERROR_EMPTY`.

**Why not every source.** The directive asked to retain the source. [Paused re-conversion](#paused-re-conversion-2026-10-19) measured why no engine can keep all of them: a decode surface returns to the decoder as soon as its conversion has run, up to sixteen converted pictures wait ahead of the screen, and copying each source costs 400 MB of VRAM and 1.5 GB/s at 4K60 HDR. So at the moment of a pause the source of the picture on screen is usually gone. The engine has one surface for this, the held source, and the refresh landing that fills it. Tensors use both:

- **The picture is held** (a seek's landing, a frame step, a settings change, an earlier export): `HeldTensor`, on the presentation thread, records the tensor pass on `held_` in a list of its own. No decode, no conversion, nothing on screen changes. `MEDIA_TENSOR_HELD` says so.
- **It is not:** the request asks for a refresh landing of the picture on screen, which carries the tensor pass with its conversion and holds the source from then on. One decode from the sync sample, once a pause.
- **A host that has exported a tensor** has that landing made when the pause begins (`tensor_used_`), so its next export finds the picture held and decodes nothing. It costs such a host the sixteen pictures queued ahead at each pause, which are converted again on resuming. A host that never exports pays nothing.

While paused, `ConvertFrame` claims a request only for the landing: pictures converted ahead of the screen are not what the host is looking at. The request wakes the presentation thread (`input_wake_`) rather than waiting for the controls' next advance.

| Measured (`media_tensor_export_tests`, the two-targets fixture, hidden, Release) | Result |
|---|---|
| paused at 1 s, the first export, 64x48 float32 of the whole picture | the picture on screen (0.958 s) in 16 ms (a landing); 0.000000 from host mode's export of that picture |
| against a snapshot of the screen, 8 x 6 cells of luma | worst cell 0.0008, a fifth of a code |
| the second export | 3.3 to 15.6 ms over two runs, `MEDIA_TENSOR_HELD`, identical to the first, 0 allocations |
| after resuming and pausing again | `MEDIA_TENSOR_HELD` at once: the pause held it |

**Limits.** The first export of a host's first pause decodes the picture: up to 0.18 s on 4K Main10 ([measured here](#paused-re-conversion-2026-10-19)). A held picture's tone map uses the source peak of the conversions ahead, as a paused re-conversion does.

## HDR tensors (2026-11-04)

Part B, gap 2: the window's tensors took an HDR picture's coded values as BT.709, so PQ code 0.45 (58 cd/m², a dark grey) reached a model as a mid-grey.

A PQ (transfer 16) or HLG (18) picture's elements are now **what the screen shows**: `video::HdrToDisplay`, the arithmetic of `HdrToneMap.hlsl Convert` in double on the CPU, applied to each cell's Y'CbCr means:
- BT.2020 non-constant-luminance Y'CbCr to R'G'B' (limited or full range, 8 or 10 bits);
- display light: the PQ EOTF, or HLG's inverse OETF and the BT.2100 OOTF for 1000 cd/m²;
- the BT.2390 EETF from the source's peak to SDR white (203 cd/m²) on the brightest channel, its ratio kept (hue);
- BT.2020 to BT.709 primaries, out-of-gamut colours desaturated toward their luminance, gamma 2.4.

The source peak is the one the picture on screen was converted with: the smoothed peak of the HDR passes, copied after the means (`RecordTensor`), so a tensor and the screen move together. A Y' tensor is the BT.709 luma of those R'G'B'. `MEDIA_TENSOR_TONE_MAPPED` says so. Means are taken in coded values, then converted, so a cell holding a sharp edge between two brightnesses comes out a little off the mean of what the screen shows for it (0.014 at worst below).

| Measured (`media_tensor_export_tests`) | Result |
|---|---|
| PQ 58 cd/m² (coded 0.454, below the tone map's knee) | 0.5933, which is (58/203)^(1/2.4) |
| black; the source's peak; 4000 cd/m² over a 1000 cd/m² peak | 0.0000; 1.0000; 1.0000 |
| a grey ramp; every colour of the Y'CbCr cube | monotonic; all in [0, 1] |
| HLG 75 % against PQ 203 cd/m² (both are reference white) | 0.9034, 0.9033 |
| the 4K Main10 PQ fixture, paused, 48x32 float32 | `MEDIA_TENSOR_TONE_MAPPED`, elements 0.4547 to 0.8261; 8 x 8 cells of luma within 0.0143 of a snapshot of the screen |

**Limits.** Host mode's pictures are 8-bit and carry no transfer: they are taken as BT.709, as before. `video::ExportTensor` takes a `TensorColor` for a host that knows better.

## The telemetry module's memory (2026-11-04)

Part B, gap 3: the window's event camera took the telemetry module from 576 KiB to 3,392 KiB. The directive offered a smaller sensor or sharing the perception plane; the accounting showed better, and nothing below moves a single event:

| In the region, at the largest grid (480 x 272) | Before | After | How |
|---|---|---|---|
| the sensor's references | 255 KiB | 255 KiB | Kept: each pixel's log intensity at its last event |
| Rebase's target | 255 KiB | 0 | Shares the changes' plane: one is used before the other, never at once |
| the changes | 255 KiB | 255 KiB | |
| the decimated picture | 255 KiB | 0 | At scale 1 the sensor reads the picture Push is given: here the grid handed to perception's thread (the directive's "reusing the perception luma plane") |
| the ring | 1,024 KiB | 256 KiB | 8 bytes an event, 32,768 events: x, y, polarity and contrast in 28 bits, the timestamp's low 36 bits. Poll restores the rest from the newest time, exact while the ring spans under 9.5 hours |
| the virtual IMU | 319 KiB | 159 KiB | 16 bits a cell (sixteenths of a code) instead of floats |
| perception, the grid, the signatures | 456 KiB | 456 KiB | |
| **the module** | **2,819 KiB (the sum above) in a 3,392 KiB region** | **1,372 KiB (1.340 MiB) in a 1,408 KiB region** | |

Measured: `Arena:` in the demo, on a 4K file. A 320x240 picture uses 1,050 KiB.

**What stayed the same.** Spike accuracy: the sensor's grid, threshold and references are untouched. The windowed test polls the same scene's events, 100.00 % on the squares, 0 dropped. A ring of half the events is the one trade: a host that lets more than 32,768 events pile up between two polls loses the rest, counted in `dropped`. That test's 54,088 events are about 750 a picture, polled every picture. Host mode keeps 65,536 events, now in 512 KiB. The virtual IMU's 16-bit grids move nothing that matters: 0.068 samples rms off the shake as before, the event camera's counts the same to the event (355,579 stabilised by it, 383,330 by the true IMU); the view 0.077 samples rms off the IMU's (0.066 with the floats and the one-pole).

**Test.** `media_neuromorphic_spike_tests`, "Packed ring": events across the timestamp's 36-bit wrap, and a seek back with events unread, come back exact; the region stays under 1.5 MiB (asserted from `kModuleBudgets`).
