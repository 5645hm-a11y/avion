# Layer 3: connected H.264 playback

The Windows reference application now reads a local MP4, derives H.264 DXVA parameters, decodes into GPU NV12 surfaces, converts/scales those surfaces with HLSL, presents a borderless flip-model window and plays AAC-LC through WASAPI. The hardware audio clock disciplines the existing predictive PLL. This is a bounded first-playback implementation; the admission limits below are part of its contract.

## Components and ownership

| Component | Location | Responsibility |
|---|---|---|
| Portable H.264 syntax | `include/media/codec/`, `src/codec/` | EBSP reader, SPS/PPS/slice validation, POC, short-term DPB |
| Native H.264 adapter | `platform/windows/H264Dxva.*` | Translate portable picture metadata into DXVA structures |
| Renderer | `include/media/render/`, `src/render/` | Window, prepared GPU compute resources, NV12 conversion, presentation |
| Native AAC bridge | `platform/windows/Win32AacDecoder.*`, `NativeSampleLease.hpp` | AAC-LC to float PCM using the Microsoft decoder, fixed input leases |
| Coordinator | `include/media/engine/`, `src/engine/` | Preparation, worker/UI ownership, backpressure, clock and shutdown |
| Demo | `apps/media_player_demo.cpp` | File argument, allocation measurement, diagnostics |

The UI thread owns the renderer, window messages, frame queue consumer and surface-return producer. A single media worker owns IOCP, demux, H.264 syntax/DPB, native decode submission, AAC, WASAPI and the clock writer. IO and GPU execution remain asynchronous. Co-locating the media stages avoids extra inter-thread transfers for this milestone; no independent OS thread per stage is necessary. The packet and packet-return queues have one producer/consumer even when both endpoints are cooperatively serviced on the same worker.

All CPU queues use the existing acquire/release SPSC contracts. Decoder lease-ledger mutation stays on its owner. The UI only indexes the immutable prepared GPU resource table while holding a frame lease. The clock's bounded atomic snapshot interface is the only concurrent timing query.

Preparation reads metadata/configuration, allocates the arena (128 MiB here; 256 MiB since Layer 5) and fixed queues, prepares native audio/video objects, starts the worker and waits for readiness. `Seal()` precedes `Run()`. The worker performs no allocation after readiness. On completion/cancellation, the worker stops pumping and acknowledges quiescence. The caller ends allocation measurement, permits native teardown, joins the worker and drains renderer/decoder resources. Reusing a coordinator for a second session is not supported; construct a new session after closing the first.

### IOCP thread handoff

The port uses concurrency one. An initialization thread that calls `GetQueuedCompletionStatus` becomes associated with that port; zero timeout does not undo the association. If it then runs a busy UI loop, the new worker can be denied completions. `DetachPollingThread()` associates the old owner with an empty temporary port and closes it during preparation. Only then does ownership pass to the worker. The regression test keeps the old thread runnable while checking that the new thread receives data. See Microsoft's [IOCP concurrency model](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports) and [thread disassociation explanation](https://devblogs.microsoft.com/oldnewthing/20210120-00/?p=104740).

The demo selects buffered overlapped reads with sixteen blocks (4 MiB here; 8 MiB since Layer 5 for 4K access units). The same backend's direct/unbuffered mode remains available and tested. Buffered IO is not advertised as direct DMA or as zero OS-cache pollution. Metadata, small samples, filesystem and device behavior make these distinct policies; neither mode proves that the kernel performs no copies.

## H.264 syntax and DPB

`RbspReader` reads immutable EBSP directly. It removes emulation-prevention bytes logically, validates escape sequences, bounds bit widths and Exp-Golomb prefixes, and checks parameter-set trailing bits. There is no RBSP allocation or payload rewrite. SPS/PPS arrays (32/256), a pending picture, up to 256 slice ranges and sixteen short-term references are prepared in the arena.

The frontend checks access-unit lengths before subspans, parameter IDs before indexing, macroblock dimensions, crop arithmetic, reference counts and signed POC ranges. An unsupported feature returns a status; it never falls back to an alternative codec library. VCL entropy decoding remains the hardware decoder's job. Corrupt entropy data can therefore pass syntax-header checks, but the coordinator rejects hardware decode statistics with a non-OK status or affected macroblocks.

For POC type 0, with modulus `M = 2^log2_max_pic_order_cnt_lsb`, the previous reference picture supplies `prevLsb` and `prevMsb`. An IDR resets them to zero. If `lsb < prevLsb` and `prevLsb-lsb >= M/2`, add `M` to the previous MSB; if `lsb > prevLsb` and `lsb-prevLsb > M/2`, subtract `M`; otherwise retain it. Top POC is `msb+lsb`, bottom POC is top plus the admitted bottom delta. Only reference pictures update previous POC state. Type 2 uses `2*(FrameNumOffset+frame_num)`, subtracting one for a non-reference picture and resetting on IDR. All results are range checked before conversion to DXVA's signed fields. These rules follow [ITU-T H.264](https://www.itu.int/ITU-T/recommendations/rec.aspx?id=15935), clauses 7 and 8.2.

Parsing is transactional with respect to the DPB: a pending picture survives `WouldBlock`, and `Commit(surface)` happens only after successful native submission. IDR commit evicts all old references; normal reference commit evicts the oldest short-term reference when the configured maximum is reached. Returned handles remove DPB holds through `ReleaseReference`; they do not impersonate consumer returns or fence completion. The native backend remaps logical reference handles to physical DXVA indices and uploads the prepared parameters before publication.

| Feature | Current admission |
|---|---|
| Container | Layer 1 MP4/fMP4 subset; one H.264 track and zero or one AAC track; no encryption (single-entry edit lists since Layer 5) |
| H.264 | Progressive 8-bit 4:2:0 I, P and B pictures; profile IDs 66/77/88/100 within this syntax subset; CABAC or CAVLC. See [H.264 B-pictures](h264-bframes.md) |
| POC / numbering | Types 0 and 2; initial IDR; sequential frame numbers modulo their configured width |
| References | At most sixteen: sliding window, MMCO 1-6, long-term references; lists (with modification and weights) are built by the accelerator from the host DPB |
| Parameters | AVCC length fields 1/2/4 bytes; fixed resolution; flat/default scaling matrices; no runtime SPS/PPS replacement |
| Exclusions | SP/SI pictures, fields/MBAFF, FMO/ASO, redundant pictures, frame_num gaps, custom scaling matrices, auxiliary/extended VCL |
| Capacity | 256 VCL slices/AU, coded dimensions up to 8192 per axis subject to device limits; 4 MiB block/sample bound, native bitstream bound; a 48 MiB compact sample index (4-hour 60 fps files with audio at worst case; see [Layer 1](layer1.md#sample-index-long-form-files)) |
| Presentation | PTS order through the Layer 5 reorder window (VUI `max_num_reorder_frames`, else the Annex E inference); SDR NV12, BT.601/BT.709 or unspecified matrix (601 default), full/limited range; fixed window size |
| Audio | Two-byte AAC-LC AudioSpecificConfig, 1024-frame access units, mono/stereo, rate up to 48 kHz, exact float format supported by WASAPI |

The current renderer assumes square display pixels and centered chroma sampling; sample-aspect-ratio, chroma-location metadata, rotation and HDR transfer functions are not rendered. Streams depending on these features need additional presentation metadata support. Profile ID acceptance does not mean every feature in that profile is supported. Many ordinary MP4 files use B pictures or edit lists and are consequently rejected. HEVC playback, B-picture reordering, P010/BT.2020 and edit lists arrived in [Layer 5](layer5.md). The demo has no Opus decoder, resampler, seeking, pause, device-recovery/reconfiguration or network provider.

**Correction (Layer 4):** the native decoder's placed surfaces made inter-predicted pictures decode blank on the Intel UHD 630 while reporting status OK. This fixture's P-skip pictures were therefore most likely blank before that fix; the counters below verify presentation and decode status, not pixels. Layer 4's `media_decode_reference_tests` now checks inter-decoded pixels.

## GPU timeline and lifecycle proof

Each decoded surface has independent holds for codec reference use and downstream consumption. The source NV12 surface is processed promptly, rather than being held until its PTS. Eight precreated RGB textures hold processed frames until presentation. The renderer uses native D3D12 resources, descriptor tables and compiled HLSL; no pixels are mapped or read back to the CPU.

For decoded surface `S` and processed slot `R`:

1. The copy queue copies packed compressed bytes to a prepared DEFAULT buffer and signals `B`. The decode queue waits on `B` and prior joined consumer uses, decodes into `S`, restores `COMMON`, resolves decode statistics and signals `D`.
2. The UI records compute conversion, queues `Wait(D)`, transitions `S` from `COMMON` to shader read and `R` from `COMMON` to UAV, dispatches the NV12 shader, restores both resources to `COMMON`, then signals consumer fence `C`.
3. Only after enqueueing that signal does the UI publish the reverse lease `(surface,generation,epoch,C)`. The worker validates completed decode statistics and records the return. A later reference decode queues `Wait(C)` before its own resource transitions.
4. Physical reuse of `S` requires the generation to match, no DPB hold, a recorded consumer return, completed decode/reference uses and completed `C`. CPU queue publication alone never proves GPU completion.
5. At the PLL deadline, with conversion complete and swapchain capacity available, the UI copies `R` into the current back buffer and calls `Present(0, DXGI_PRESENT_DO_NOT_WAIT)`. `WAS_STILL_DRAWING` retains the staged slot for retry. A subsequent queue signal protects reuse of the RGB texture and command allocator.

Therefore neither NV12 decode reuse nor RGB/command-allocator reuse can race an earlier GPU access. Both transitions and waits are on GPU timelines; CPU polling uses no positive timeout during `Run()`. The two texture stages also avoid a circular dependency where a P-picture needs a reference whose consumer waits for a future presentation deadline. D3D12 requires flip-model swapchains; see [D3D12 swapchains](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains) and [nonblocking Present flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).

`NV12 -> RGB` applies range normalization and the selected luma/chroma matrix, then bilinear scaling into RGBA8. The shader is compiled at build time by the Windows SDK's `fxc /WX /T cs_5_0` and embedded in the executable. No runtime shader compiler, external shader package or downloaded binary is required. This pass converts decoded GPU data into another GPU texture; zero host readback does not mean zero GPU writes/copies.

Normal teardown drains the renderer before releasing decode resources. If a live device refuses a retirement signal/event and completion cannot be proven, both sides retain native GPU objects until process exit. That exceptional quarantine can leak resources by design; it avoids releasing storage the GPU might still access. Device removal is now recovered in place and validated on hardware with `ID3D12Device5::RemoveDevice` ([recovery](recovery.md)); other fence-failure injection has not been validated.

## Audio and clock coupling

The Microsoft AAC decoder is a native Windows MFT, instantiated directly through COM without a Media Foundation topology or source reader. Eight input samples and one output sample/buffer are prepared before sealing. Its advertised float output format is selected exactly, and end-of-stream is explicitly drained. `NativeSampleLease` owns the COM identity/reference count; a slot can be rewritten only after the transform has released every sample reference back to the owner's single reference. Returning from `ProcessOutput` alone is insufficient: the native API explicitly permits input retention. See [AAC decoder formats](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-decoder) and [MFT input stream flags](https://learn.microsoft.com/en-us/windows/win32/api/mftransform/ne-mftransform-_mft_input_stream_info_flags).

Compressed AAC is copied into prepared native input storage before its file block is returned. Decoded float PCM is copied into the preallocated WASAPI ring. These audio copies and the video compressed-bitstream upload are explicit; the zero-copy promise applies to demux slicing and decoded-video host transport, not to every byte in the system. Native COM/GPU/WASAPI storage is prepared through native allocation APIs; CPU parser tables, transport and file blocks use the sealed arena.

The worker primes PCM before starting WASAPI. The first precise nonzero hardware position anchors media time at `firstAudioPTS + devicePosition/deviceFrequency`. Subsequent hardware/QPC observations discipline the PLL every 10 ms. The renderer queries deadlines in normalized QPC-domain 100 ns. Audio packet PTS/duration must match continuous 1024-frame progression within rounding tolerance; discontinuities fail explicitly. A clock generation/frequency break or starvation requires a new session rather than silent extrapolation across the gap.

Video-only files use an intentional silent audio master. At EOF, AAC is drained and deliberate silence keeps the hardware clock running until the last video deadline. Completion waits for all queued frames, their hardware validation and the audio track endpoint. The hardware clock paces submissions, but neither the PLL nor `Present` guarantees exact photon timing: DWM, scheduling, the driver and refresh cadence remain involved. Swapchain occlusion is a successful DXGI status, so presentation counts are submissions, not proof that every image was visible to a person.

## Verification

MSVC 19.51 x64, Windows SDK 10.0.26100.0, `/W4 /WX /permissive-`, Debug and Release: all ten CTest entries pass, including the real hardware/audio suites. GCC on WSL Ubuntu 24.04 passes the portable codec suite with `-Wall -Wextra -Wpedantic -Werror`, and AddressSanitizer/UndefinedBehaviorSanitizer.

The original fixture generator writes an MP4 without external media tools: 72 handwritten H.264 I_PCM/P-skip access units at 24 fps and 141 AAC-LC packets encoded by the native Microsoft encoder from a quiet 440 Hz stereo signal. `--test` requires exactly 72 presentations, 72 hardware decode validations, 144384 decoded PCM frames (samples per channel), no pipeline error, and zero measured application allocations. Every hardware validation requires status OK and zero affected macroblocks. The longer manual run presented/validated 720 frames and decoded 1443840 PCM frames, also with zero detected application allocations. A desktop capture confirmed the colored animated fixture was rendered in the borderless window. This is not a codec pixel-conformance or acoustic latency test.

New portable tests cover bit/Exp-Golomb reads, emulation-prevention validation, truncation at every parameter/AU byte, bounded byte mutations, POC/frame-number rollover, DPB eviction, IDR reset, cancellation/backpressure, and explicit rejection of B/MMCO syntax. Existing suites continue covering demux, clock skew/jitter, ring bounds, lease fence gates, IOCP, audio and native decode.

Allocation probes cover application global C++ allocation entry points and the application's Debug CRT hook, across the whole `Run()` interval. Arena allocation counts are separately checked by primitive/parser tests. They do not instrument every private Windows, MFT or graphics-driver heap, and do not prove native API internals are allocation-free or lock-free. The engine adds no mutex or blocking wait during streaming; the busy-poll policy consumes CPU and is not a low-power playback policy. Preparation and quiescent teardown may wait and allocate.

Run from the repository root after building:

```powershell
ctest --test-dir build -C Release --output-on-failure
.\build\Release\media_player_demo.exe .\build\playback_fixture.mp4
.\build\Release\media_player_demo.exe 'C:\Media\clip.mp4'
# Optional 30-second original fixture, outside the CTest timing budget:
.\build\Release\media_make_playback_fixture.exe .\build\playback_long.mp4 --long
.\build\Release\media_player_demo.exe .\build\playback_long.mp4
```

Escape closes the window; dragging moves it. `--test` is reserved for the generated three-second fixture and enforces its exact counters plus a 15-second limit. Configuration/audio/native capability failure is reported as a stage plus HRESULT (or parser status), without substituting fake video or audio.

| Stage | Failure domain |
|---|---|
| 1–5 | File, fixed memory, demux metadata, track selection, codec configuration |
| 6–9 | Renderer/decoder setup, IOCP handoff/thread creation, audio/MFT setup |
| 10–18 | Surface/packet return, audio pumping/clock discontinuity, PCM/start, demux/block access |
| 19–24 | AAC processing, H.264 syntax, dimensions, DPB commit/eviction, native submission |
| 25–27 | Conversion, presentation/deadline failure, test timeout |
| 28–30 | GPU decode statistics, unsupported timestamps, unsafe teardown quarantine |
| 31–32 | HDR transfer (PQ/HLG) rejected, reorder depth beyond the presentation window (Layer 5) |

Portable codec verification without a Linux CMake installation:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
  tests/codec_tests.cpp tests/allocation_probe.cpp src/codec/H264SyntaxParser.cpp \
  src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp \
  -o build/linux/media_codec_tests
./build/linux/media_codec_tests
```

For sanitizers replace `-O2` with `-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`. The portable codec headers contain no Win32 includes; the DXVA adapter is isolated under `platform/windows/`. macOS/Android/iOS toolchains and native playback backends have not been run in this milestone.
