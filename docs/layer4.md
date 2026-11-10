# Layer 4: HEVC syntax frontend and verified inter decode

This milestone adds a portable, allocation-free H.265/HEVC syntax parser (`HevcSyntaxParser`), its Windows DXVA adapter (`HevcDxva`), and pixel-level hardware verification. That verification exposed and fixed a pre-existing defect in `D3D12VideoDecoder`: inter-predicted pictures of **both** codecs decoded to blank surfaces while reporting `D3D12_VIDEO_DECODE_STATUS_OK`. The engine coordinator, renderer and demuxer are unchanged. HEVC is verified from parser to decoded GPU surface. [Layer 5](layer5.md) wires it into `media_player_demo` for playback.

## Components

| Component | Location | Responsibility |
|---|---|---|
| Shared RBSP reader | `include/media/codec/Rbsp.hpp`, `src/codec/Rbsp.cpp` | EBSP reading with logical emulation-prevention removal, `Position()` in RBSP bits, `SyntaxStatus`, arena table helper. Moved out of the H.264 parser; H.264 behavior is unchanged. |
| HEVC syntax | `include/media/codec/HevcSyntaxParser.hpp`, `src/codec/HevcSyntaxParser.cpp` | VPS/SPS/PPS, slice-segment headers, POC (8.3.1), RPS and DPB marking (8.3.2), transactional commit |
| HEVC DXVA adapter | `platform/windows/HevcDxva.*` | `DXVA_PicParams_HEVC`, `DXVA_Qmatrix_HEVC`, reference handles and slice ranges |
| Decode backend fixes | `platform/windows/D3D12VideoDecoder.cpp` | Committed decode surfaces, `CurrPic` reference slot, HEVC inverse-quantization argument only with scaling lists |

The parser has no Windows types; `HevcDxva` is the only HEVC code under `platform/windows/`.

## Parser design

One codec owner calls `ParameterSet`, `Parse`, `Picture`, `Commit` and `Cancel`, mirroring `H264SyntaxParser`. `Initialize` allocates every table from the arena before sealing: 16 VPS, 16 SPS (each with 64 derived short-term RPS, 32 long-term SPS entries, resolved scaling lists and VUI colour data), 64 PPS, arena scratch for one SPS/PPS under construction, one pending picture with 256 slice segments, a 16-entry DPB and two slice-state records. This is roughly 0.3 MiB. Nothing is allocated afterwards; large parameter-set structures are never built on the stack.

`Parse(access_unit, length_bytes)` accepts one length-prefixed access unit containing exactly one base-layer picture:

- NAL units with `nuh_layer_id > 0`, SEI, filler, AUD and reserved/unspecified non-VCL types are ignored. Reserved VCL types return `Unsupported`.
- In-band VPS/SPS/PPS must precede the first VCL unit. An identical repeat of the active SPS is accepted; a changed active SPS returns `Unsupported` (a new decoder session is required). PPS content may change between pictures. Parameter-set updates inside a failed access unit are not rolled back.
- The first VCL unit must have `first_slice_segment_in_pic_flag`; another first segment in the same sample is `Unsupported`. Slice-segment addresses must strictly increase. Dependent segments inherit their header. Every independent segment must agree on PPS, POC LSB, the selected short-term RPS, long-term entries, `slice_temporal_mvp_enabled_flag` and `pic_output_flag`.
- Slice headers are parsed through `slice_temporal_mvp_enabled_flag`. D3D12 uses the DXVA short-slice format, so the hardware parses weights, list modification, entry points and slice data itself. Corrupt slice data can therefore pass syntax checks. The hardware verification below rejects wrong pixels, not only wrong status.
- The stream must start at an IRAP picture, as must the first picture after an end-of-sequence or end-of-bitstream NAL unit (`NeedIdr`). RASL pictures that follow an IRAP with `NoRaslOutputFlag = 1` return `Discard`. The caller returns their packet without decoding.

`Commit(surface, evicted, count)` runs only after successful native submission. It applies the pending DPB marking, returns evicted handles for `ReleaseReference`, inserts the current picture when it can be referenced, and updates `prevTid0Pic`. `Cancel` discards a pending picture with no state change.

### Admission

| Feature | Admitted |
|---|---|
| Profiles | Main and Main 10 (`general_profile_idc` 1/2 or the matching compatibility flag), profile space 0 |
| Format | 4:2:0, luma and chroma both 8-bit or both 10-bit, progressive (`field_seq_flag` rejected), up to 8192×8192 |
| Coding tools | All Main/Main 10 tools: tiles (≤ 20×22, the DXVA capacity), WPP, dependent slices, PCM, SAO, AMP, transform skip, sign hiding, weighted prediction, scaling lists, temporal MVP, long-term references, temporal sub-layers |
| Extensions | Range, SCC, 3D and multilayer PPS extensions return `Unsupported`. `sps/pps_extension_4bits` payloads are ignored as the specification requires. |
| Limits | 256 slice segments per picture, 15 references (DXVA), `NumPicTotalCurr ≤ 8`, DPB ≤ 16 |

### Derivations

- **Scaling lists**: stored in coded (up-right diagonal) order, which is the order `DXVA_Qmatrix_HEVC` uses. The defaults are Table 7-5/7-6. A prediction for 32×32 steps `refMatrixId` by 3. DC coefficients are copied with their list. Lists are resolved at parse time: explicit, default (enabled but not present) or flat (disabled). The adapter uses the PPS lists when they are present.
- **Short-term RPS**: explicit sets and inter-RPS prediction follow equations 7-61/7-62, including the slice-header form with `delta_idx_minus1`. Each set records `NumDeltaPocs[RefRpsIdx]` when predicted.
- **POC**: `prevTid0Pic` excludes TemporalId > 0, RASL, RADL and sub-layer non-reference pictures. MSB is reset for an IRAP picture with `NoRaslOutputFlag`.
- **RPS marking**: long-term candidates are searched among all reference pictures (by LSB, or by full POC when `delta_poc_msb_present_flag`). `DeltaPocMsbCycleLt` accumulates separately within the SPS-sourced and slice-sourced entries. Short-term entries match only short-term pictures. A missing entry in a *Curr* list is `Invalid`; a missing *Foll* entry is legal. An ambiguous LSB-only long-term match is `Invalid`. An IRAP picture with `NoRaslOutputFlag` marks the whole DPB unused.
- **Retention**: a sub-layer non-reference picture at the highest sub-layer is never referenced and does not hold a DPB surface.

## DXVA mapping

- `RefPicList` holds every picture kept by the current RPS (Curr and Foll, short- and long-term, `AssociatedFlag` = long-term), as FFmpeg's DXVA path does. `RefPicSetStCurrBefore/After/LtCurr` index `RefPicList`; unused entries are `0xFF`. `D3D12VideoDecoder` remaps `Index7Bits` to stable surface IDs and writes `CurrPic`.
- `wNumBitsForShortTermRPSInSlice` counts **RBSP** bits of `st_ref_pic_set()` in the slice header, excluding emulation-prevention bytes and including `inter_ref_pic_set_prediction_flag`; it is 0 when the SPS set is selected. `ucNumDeltaPocsOfRefRpsIdx` is taken from the RPS actually selected. A hardware mutation adding one bit to the count corrupts decoding, which confirms the semantics.
- `IntraPicFlag` means every slice is I. `NoPicReorderingFlag` means `sps_max_num_reorder_pics == 0`. Tile column/row sizes are passed only for non-uniform spacing.
- `color_metadata_id` keeps the H.264 layout (matrix bits 0–7, full range bit 8) and adds transfer characteristics (16–23) and colour primaries (24–31). The current renderer reads only the first two.

## Native backend corrections

Three changes to `D3D12VideoDecoder`:

1. **Committed decode surfaces.** Decode textures were previously placed together in one explicit `DEFAULT` heap. On the Intel UHD 630 (driver-reported tier 2, no configuration flags), those surfaces were written correctly and read correctly by compute shaders, but motion compensation read them as zeros. Every inter-predicted picture of both codecs therefore produced a blank surface with decode status OK. A handwritten H.264 probe isolated the defect:
   - an I_PCM P picture decoded correctly;
   - a P-skip copy of any reference came out unwritten;
   - changing the reference array, the `CurrPic` slot, synchronization or the compute access had no effect;
   - switching only the surface allocation to committed resources fixed it.
   
   The most plausible mechanism is driver-managed per-resource compression metadata. Committed resources are still created once in `Prepare`, before sealing. Bitstream buffers remain placed. Because the Layer 3 fixture alternates IDR and P-skip pictures, its P pictures were probably blank before this fix; status-only tests could not observe it.
2. **`CurrPic` reference slot.** `ReferenceFrames[CurrPic]` now names the output texture instead of an aliased reference, as DXVA indexing and FFmpeg's D3D12 path do. It had no observable effect on this driver.
3. **HEVC inverse-quantization matrix.** It is passed only when `scaling_list_enabled_flag` is set, as in FFmpeg's DXVA and Mesa's D3D12 frontends.

## Verification

| Suite | Evidence |
|---|---|
| `media_hevc_codec_tests` (portable) | Bit-exact authored VPS/SPS/PPS/slices:<br>• PTL with sub-layers; VUI with a three-sub-layer NAL+VCL HRD; conformance window; PCM<br>• default, explicit, copied and DC scaling lists<br>• hand-derived inter-RPS goldens (both `deltaRps` signs, SPS and slice-header forms)<br>• RPS bit count across an emulation-prevention byte<br>• hierarchical GOP-4 with three sub-layers and POC-LSB wrap, against an independent DPB model (explicit and SPS-indexed RPS)<br>• long-term references with MSB cycles and the 7-52 accumulation boundary<br>• `prevTid0Pic` exclusions; CRA/RASL/RADL/BLA/IDR/EOS<br>• multi-segment structure; in-band parameter-set policy; tiles<br>• truncation at every byte; every single-byte mutation of SPS/PPS/VPS/AUs<br>• zero allocations after sealing |
| `media_hevc_dxva_tests` | CPU-only field-by-field check of `DXVA_PicParams_HEVC` / `DXVA_Qmatrix_HEVC` / `DecodePicture` |
| `media_decode_reference_tests` | Hardware H.264 probe: IDR, I_PCM P picture, reference preservation and a P-skip copy, each checked by a GPU pixel sum |
| `media_hevc_decode_tests` | Main and Main10 streams from the platform's HEVC encoder MFT (Intel on this machine), 320×192, 48 pictures with B-pictures and reordering, two IDRs. For every picture: parser → adapter → `D3D12VideoDecoder`; decode status OK; a compute shader compares decoded luma with the analytic source pattern. Only one error counter per comparison is read back, never pixels. Measured maximum mean absolute luma error: 0.45 (Main) and 0.40 (Main10). A control comparison against the next frame's pattern stays ≥ 23, proving sensitivity. Zero allocations over the whole measured loop. |

Mutation testing confirmed that these suites catch real defects:
- **Parser** (portable suite): POC MSB wrap, inter-RPS sign handling, `DeltaPocMsbCycleLt` accumulation (both boundary variants), long-term exclusion from short-term search, SEI handling, retention, `prevTid0Pic` TemporalId and SLNR exclusions, RASL discard, and CRA-after-EOS each made the suite fail.
- **Adapter** (on hardware): reversed `RefPicSetStCurrBefore`, an off-by-one RPS bit count, or shifted `PicOrderCntValList` values each raised the pixel error to 33–64 while the driver still reported status OK.
- **Backend reference texture** (`media_decode_reference_tests`): pointing references at the wrong texture is detected. On this Intel driver HEVC decoding ignored that particular corruption, so the H.264 probe is the backend's guard.

MSVC 19.51 x64, `/W4 /WX /permissive-`: all 14 CTest entries pass in Debug and Release. GCC 13.3 (WSL Ubuntu 24.04): the portable HEVC and H.264 suites pass with `-Wall -Wextra -Wpedantic -Werror` at `-O2`, and under AddressSanitizer/UndefinedBehaviorSanitizer with `-fno-sanitize-recover=all`:

```sh
g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
  tests/hevc_codec_tests.cpp src/codec/HevcSyntaxParser.cpp src/codec/Rbsp.cpp \
  src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp tests/allocation_probe.cpp \
  -o build/linux/media_hevc_codec_tests && ./build/linux/media_hevc_codec_tests
```

`media_hevc_decode_tests inspect` (not registered with CTest) prints the syntax the parser derives from the encoder's stream; it is a diagnostic for new streams. The hardware suites skip with code 77 when no D3D12 device, HEVC decode profile or platform HEVC encoder exists. Only one Intel UHD 630 machine has run them; other drivers are unverified.

## Limits and next steps

*Update: phases 4 and 5 below are implemented in [Layer 5](layer5.md).* At the time of this layer, HEVC was not yet wired into `PlaybackCoordinator`. Approved as the next phase (4):
- codec selection in the coordinator, including the `Discard` status;
- P010 plane views and BT.2020 in the renderer shader;
- presentation reordering by PTS, since B-pictures are the HEVC norm (the frame queue and renderer are currently FIFO in decode order);
- `pic_output_flag = 0` handling;
- 4K sizing of IOCP blocks, bitstream capacity, arena and surface count;
- explicit rejection of PQ/HLG until tone mapping exists.

Phase 5 is single-entry `elst` admission in the demuxer, needed by most real HEVC MP4 files. Also outside this milestone: resolution or SPS changes within a session, field coding, RExt/SCC/multilayer, error concealment for missing references, and HDR metadata presentation.
