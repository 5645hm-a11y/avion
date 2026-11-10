# Layer 5: HEVC playback — reordering, 10-bit colour, 4K envelope and edit lists

This milestone connects the [Layer 4](layer4.md) HEVC frontend to the engine. `media_player_demo` now plays HEVC Main and Main10 (SDR) MP4 files with B-pictures, including 3840×2160 10-bit, alongside the existing H.264 I/P path. The demuxer admits a single-entry edit list. Steady-state playback still performs zero application heap allocations.

## Edit lists (demuxer)

`edts` is a `trak`-level container. `elst` (version 0 or 1, once per track) is admitted only in its common, interpretation-preserving form: exactly one normal edit, `media_time ≥ 0`, `media_rate` 1.0. Any other form returns `Unsupported`:
- more than one entry;
- empty edits (`media_time = -1`);
- dwell or rate edits.

A duplicate `elst` returns `InvalidTable`.

The edit is a presentation-timeline shift, in media-timescale ticks:

```text
DTS' = DTS - media_time        PTS' = DTS + CTS - media_time
```

The shift is applied when a packet is emitted, before the checked 100 ns rescale. It applies to classic sample tables and to `tfdt`-anchored fragments alike, and cross-track interleaving uses the shifted DTS. Payload bytes are never touched or copied. `TrackInfo` reports `edit_media_time`, and `max_sample_bytes` (the largest classic-table sample, 0 when unknown for fragmented files) so decode buffers can be sized.

Samples that land before the edit start keep negative PTS. They are decoded, because later pictures reference them, but they are never presented (see [hidden pictures](#decode-only-and-discarded-pictures)). `segment_duration` is not used to trim the end of the timeline.

## Presentation reordering

Decode order differs from presentation order whenever B-pictures are present. `media::video::ReorderWindow<N>` (portable, header-only) reorders at the presentation stage, not the decode stage:

- **Decoupling.** The renderer converts each decoded NV12/P010 surface into one of 16 display-sized RGB slots as soon as it arrives, then returns the decode surface with its consumer fence. Decode surfaces therefore never wait for a presentation deadline. That matters because `D3D12VideoDecoder` will not reuse a reference until its consumer has returned it: holding references for presentation could stall decoding.
- **Bounded sorted ring.** The window stores `{pts, slot}` records in a fixed power-of-two ring kept sorted by PTS. Insertion shifts at most N entries and nothing is allocated.
- **Bumping (H.265 C.5.2).** The front (smallest PTS) may leave only when more than `depth = sps_max_num_reorder_pics` frames are waiting, or while draining at end of input. By definition of that SPS value, no later-decoded picture can precede a picture once `depth` successors have been decoded, so the front is final. The portable test proves the bound is tight: on hierarchical GOPs of size 2–16, one less than the exact depth always releases a picture too early.
- **Violations are reported, never reordered silently.** A frame whose PTS is at or below an already-released PTS (a stream exceeding its declared depth), or a duplicate PTS, is retired without GPU access and counted as out-of-order.
- **Deadlines.** The front is presented at its PLL deadline. A frame whose whole display interval has passed, or which is far behind the audio clock, is dropped and counted instead of shown late. Presentation lateness no longer fails playback.

H.264 uses the VUI `max_num_reorder_frames`, else the Annex E inference (0 for Baseline), capped at 14 (see [H.264 B-pictures](h264-bframes.md)). Depths up to 14 are admitted (the window needs depth + 2 slots of the 16); deeper streams fail preparation at stage 32.

## Decode-only and discarded pictures

| Picture | Handling |
|---|---|
| RASL after a random-access start (`SyntaxStatus::Discard`) | Never decoded; the worker returns its packet lease at once. |
| `pic_output_flag = 0`, or negative PTS after an edit list | Decoded and kept as a reference. Published with `VideoFrameFlags::NoDisplay`: the UI thread retires the surface immediately (consumer fence 0, no GPU access), and it is still validated by decode statistics. It never reaches the window or the swap chain. |

Playback ends when input is done, the frame queue and window are empty, every retired surface has been validated, and the audio clock has passed the audio end.

## 10-bit colour

The conversion shader (`Nv12ToRgb.hlsl`) handles NV12 and P010 through one code-value unit:

- **Code values.** 8-bit samples read as `v/255`. P010 stores the 10-bit value in the high bits, so one code is `64/65535`. Limited-range levels scale by `2^(bitDepth−8)` (black 16·s, luma span 219·s, chroma centre 128·s, chroma span 224·s); full range divides by `2^bitDepth − 1` codes. The 8-bit maths is unchanged, so H.264 output is identical.
- **Matrices.** BT.709 (`matrix_coeffs` 1), BT.2020 non-constant luminance (9), otherwise BT.601. Admitted matrices are 1, 2 (unspecified, shown as BT.601), 5, 6 and 9.
- **BT.2020 primaries.** When `colour_primaries = 9`, R'G'B' is linearised with the BT.1886 display gamma (2.4). It is then converted to BT.709 primaries with the ITU-R BT.2087 matrix, clipped to gamut, and re-encoded. Out-of-gamut colours are clipped, not compressed.
- **HDR is rejected.** Streams whose SPS VUI declares PQ (`transfer_characteristics` 16) or HLG (18) fail preparation at **stage 31**, with the transfer value as the native code. No tone mapping exists, and washed-out presentation is not allowed.

`color_metadata_id` carries matrix (bits 0–7), full range (8), transfer (16–23) and primaries (24–31) from `HevcDxva`. H.264 leaves primaries 0, which is treated as BT.709.

## 4K envelope

All figures are fixed during preparation; nothing grows after sealing.

| Resource | Sizing |
|---|---|
| CPU arena | 256 MiB, eagerly committed: sixteen 8 MiB IOCP blocks (the largest admissible access unit, about 8 MiB minus sector alignment), a 48 MiB demux sample-index pool, codec tables and queues |
| Decode surfaces | HEVC: DPB (`sps_max_dec_pic_buffering`) + 1 + 8 in flight, at most 32. H.264: 24. A 3840×2160 P010 surface is about 25 MB. |
| Bitstream buffers | Per surface context: largest track sample + 3 bytes per slice start code + alignment (at least 64 KiB), instead of a blanket maximum. Fragmented files fall back to the block limit. |
| RGB slots | 16 display-sized RGBA8 textures |
| Window | Source aspect, at least 640 wide, at most three quarters of the primary display. 4K is downscaled with a single bilinear tap, which aliases at large ratios. |

Audio may start below zero (an AAC priming edit list); the worker no longer rejects a negative audio origin.

## Coordinator stages added

| Stage | Meaning |
|---|---|
| 31 | HDR transfer (PQ or HLG) rejected; native code = `transfer_characteristics` |
| 32 | `sps_max_num_reorder_pics` exceeds the presentation window (more than 14) |

## Verification

New CTest entries (22 in total):

| Test | Evidence |
|---|---|
| `media_timing_tests` (extended) | `ReorderWindow`: exact-depth property on hierarchical GOPs 1–16, tightness (depth − 1 always fails), duplicates, capacity, late arrivals, draining, zero allocations |
| `media_demux_tests` (extended) | `elst` versions 0/1 on classic and fragmented tracks with hand-computed DTS/PTS/durations; rejection of multi-entry, empty, rate ≠ 1, fractional-rate and zero-entry lists; duplicate `elst`; `edts` in the wrong parent; a byte-flip sweep over a file with an edit list |
| `media_make_hevc_fixture` → `media_playback_hevc_test` | Platform-encoded 1280×720 Main `hev1` MP4: B-pictures (reorder delay 1), `ctts`, single `elst`, AAC-LC. 72 presented in order, 72 validated, 144384 PCM frames, 0 dropped, 0 out-of-order, reorder depth ≥ 1 reached the window, 0 allocations. |
| `media_make_hevc10_4k_fixture` → `media_playback_hevc10_4k_test` | Same checks for 3840×2160 Main10 (P010, BT.2020 path) in real time on the Intel UHD 630 |
| `media_make_hevc_trim_fixture` → `media_playback_hevc_trim_test` | Edit list one picture later: 71 presented + 1 decode-only picture, 72 validated |
| `media_make_hevc_pq_fixture` → `media_hdr_rejection_test` | Handwritten Main10 parameter sets with VUI BT.2020/PQ: preparation fails at stage 31 (no encoder needed) |

The playback fixtures use the platform HEVC encoder MFT. The generator parses the encoded stream with `HevcSyntaxParser` to recover presentation order (IDR base + POC, closed GOPs). From it, it writes `ctts` offsets of `(display − decode + delay)` frames and an `elst` with `media_time = delay`, so the first displayed picture lands on PTS 0, as real muxers do. Fixture and playback tests return 77 (skip) when no encoder exists.

**Mutation checks** (each makes a suite fail):
- not applying the edit shift (trim test);
- presenting `NoDisplay` pictures (trim test: the pre-edit picture reaches the window and is dropped);
- ignoring the SPS reorder depth (playback tests).

An honest gap: on this machine, decoding runs far enough ahead of presentation that even depth 0 happened to present in order. The end-to-end tests check that the depth reaches the window, and the exact-depth property itself is proven by the portable test.

**Results.**
- MSVC `/W4 /WX`: 22/22 in Debug and Release.
- GCC 13.3 `-Werror`, at `-O2` and under ASan/UBSan: the HEVC, H.264, demux and timing suites pass.
- **Observed intermittent failure:** in the first full Release run, `media_playback_hevc10_4k_test` failed once after 1.9 s. Its stage was not captured. It has not reproduced in 30 further 4K runs: back-to-back with 720p playback, repeated fixture regeneration, and four full-suite runs. Candidates are a transient allocation failure right after the encoder released GPU memory, or an audio underrun (stage 13, fatal by existing policy). It remains open.

## Limits

- **Untested paths.** Late-frame dropping, stage 32 and the RASL `Discard` path are not exercised end to end. The platform encoder emits IDR, not CRA, so `Discard` is covered only by parser tests.
- **Colour.** No HDR tone mapping and no mastering-display/content-light metadata. Transfer characteristics other than PQ/HLG are shown with display gamma.
- **Edit lists.** No end trimming from `segment_duration`, and no multi-entry lists.
- **Session scope.** One video track and at most one AAC track, no seeking or pause, a fixed window, and no mid-stream resolution or SPS change.
