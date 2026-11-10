# Layer 2: hardware submission, audio endpoint, and predictive clock

## Delivered scope

This layer implements the portable `PredictiveMasterClock`, `PcmRingBuffer`, and `SurfaceLeasePool`, the Windows `Win32WasapiAudioSink`, and the Windows `D3D12VideoDecoder` native submission backend. Preparation creates native objects and allocates bounded application storage. Streaming performs no application heap allocation or blocking CPU wait. Core, audio, clock, and lease headers contain no Windows types.

`D3D12VideoDecoder::Submit` consumes a prepared `DecodePicture`: a leased `MediaPacket`, its immutable block view, VCL NAL ranges, native DXVA picture/quantization parameters, and reference handles. Layer 2 itself does not translate container codec syntax. [Layer 3](layer3.md) now supplies a bounded H.264 I/P frontend, native AAC-LC decoding, orchestration and NV12 presentation. Adaptive MMCO, HEVC RPS/frontend, B-picture reordering, Opus decoding and resampling remain absent. The independent Layer 2 H.264 hardware test supplies constructed parameters for original IDR/P fixtures. [Layer 4](layer4.md) adds the HEVC frontend, pixel-level decode verification, and corrections to this backend (committed decode surfaces, the `CurrPic` reference slot, conditional HEVC quantization matrices). **Before those corrections, inter-predicted pictures decoded to blank surfaces on the test machine while reporting status OK; the status-only evidence below could not detect it.**

D3D12 exposes explicit decode arguments and reference resources; it is not a container or elementary-stream parser. This boundary follows the native API, but it leaves a substantive integration task before general media playback. [Microsoft decode input arguments](https://learn.microsoft.com/en-us/windows/win32/api/d3d12video/ns-d3d12video-d3d12_video_decode_input_stream_arguments)

## Memory, ownership, and execution domains

| Domain | Preparation | Streaming ownership |
|---|---|---|
| CPU arena | Eagerly committed/touched arena, PCM ring, surface ledger, command-context records, SPSC queues | Fixed storage; seal before priming |
| Compressed upload | One persistently mapped native UPLOAD resource per decode context | Decode owner writes only after prior uses retire |
| GPU bitstream | Fixed placed DEFAULT buffers in one prepared GPU heap | COPY queue writes, VIDEO_DECODE queue reads |
| Decoded pixels | Fixed committed NV12/P010 textures created during preparation (placed textures broke reference reads; see Layer 4) | VIDEO_DECODE writes; compute/presentation reads under fences |
| Native decoder | Prepared decoder, decoder heap, allocators, command lists, queues, fences | Native command submission; no per-frame object creation |
| Decode statistics | Prepared query heap and tiny mapped READBACK buffer | Status metadata only; no pixel readback |
| Audio | Prepared COM endpoint, event, native endpoint buffer, arena PCM ring | PCM producer and endpoint owner are distinct SPSC roles |

GPU heaps, mapped upload storage, WASAPI endpoint memory, and driver-private allocations cannot be supplied from `VirtualAlloc` arenas. The guarantee covers application allocations and prepared application capacities, not internal Windows/driver behavior. D3D12 allocator reset and command recording have no public guarantee of never growing private driver storage. No wrapper, decoder package, runtime shader compiler, or third-party library is introduced.

Compressed NAL bytes are packed once from the immutable block into mapped upload memory, including Annex B start codes and native alignment padding; COPY transfers them to a GPU buffer. These are compressed-data transfers. **Decoded pixel resources are never mapped, copied to host RAM, or exposed as CPU pointers.** A claim of zero copies for the entire compressed-input path would be incorrect.

The portable ring uses acquire/release cursors isolated to 64-byte lines. Slots in the existing SPSC transport are also padded to cache lines, including small surface-return records. The PCM ring's occupied interval is `[consumer, producer)`, its capacity is a power of two, and unsigned cursor differences remain bounded by capacity. Producer checkout cannot expose uncommitted samples. Consumer copying ends before its release-store permits producer overwrite. A checkout can publish fewer frames, including zero to cancel. Invalid sizes, stale leases, and double commits are rejected.

Application components do not own worker threads. Thread creation, COM initialization, endpoint selection, resource creation, and arena sealing belong to Preparing. Polling performs zero-timeout native event checks; it may burn CPU if its caller busy-polls indefinitely. This implementation makes no claim that Windows scheduling or opaque native API calls are hard real time.

## Native video state machine

The decoder admits H.264 progressive 8-bit 4:2:0 and HEVC 8/10-bit 4:2:0 configurations. H.264 coded dimensions are macroblock aligned; HEVC dimensions and supplied minimum coding-block sizes must agree. H.264 field pictures, MBAFF, FMO, non-4:2:0 formats, encrypted input, and resolution changes within a session are rejected. Limits are 2–32 prepared surfaces, 256 VCL slices per access unit, 16 H.264 or 15 HEVC input reference handles, and a configured aligned compressed-buffer capacity up to 256 MiB. Admission does not validate every native codec semantic; the prepared DXVA records must come from a trusted, validating codec frontend.

Preparation queries the profile, format, dimensions, decode tier, and allocation flags. This backend requires tier 2 or higher and rejects profiles requiring reference-only allocations. Its textures must support downstream shader access; configurations requiring an additional conversion/reference-output pool are not silently accepted. Hardware-specific height alignment is honored. [Decode support query](https://learn.microsoft.com/en-us/windows/win32/api/d3d12video/ns-d3d12video-d3d12_feature_data_video_decode_support)

Submission follows these transitions:

1. Reserve unpublished slots in both the frame queue and upstream packet-return queue. If either is full, return `WouldBlock` before touching hardware or the input lease.
2. Validate packet generation, block bounds, epoch, PTS, duration, NAL ranges, codec admission, and reference handles. Every reference must still have its DPB hold and must have received the downstream return record.
3. Reserve a reusable context from the lease ledger. Check both native completed fence values; `UINT64_MAX` is device removal, never a usable completion watermark.
4. Pack compressed bytes and short-slice records. Copy and remap supplied DXVA picture records into the context. Caller reference indices address `references[]`; the backend converts these to stable surface-pool indices and writes `CurrPic` itself. DXVA indices must address the corresponding native reference-resource array. [Reference-frame mapping](https://learn.microsoft.com/en-us/windows/win32/api/d3d12video/ns-d3d12video-d3d12_video_decode_reference_frames)
5. Reset only this retired context's allocators. Record COPY transitions `COMMON → COPY_DEST → COMMON` for the bitstream, submit COPY, and signal its fence.
6. Record bitstream/reference transitions to `VIDEO_DECODE_READ`, output to `VIDEO_DECODE_WRITE`, `DecodeFrame`, and status-query resolution. Restore every involved resource to `COMMON`. VIDEO_DECODE waits on the copy fence and the highest returned consumer fence for its references, executes, then signals the decode fence. These are **GPU queue waits**, not CPU waits. Native video queues participate in D3D12 fence synchronization. [D3D12 video execution model](https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-12-video-overview)
7. Mark output/reference last uses, publish the original packet to the upstream return queue, and publish the frame with its decode timeline/value, surface generation, epoch, and PTS. Successful submission is asynchronous; it is not evidence that the bitstream decoded correctly. `Statistics` reports completion and native decode status without reading pixels.

The original packet block can return immediately after successful submission because packing has finished and the GPU reads the prepared upload/default resources. On `WouldBlock` or validation failure neither queue publishes and the caller retains the packet. On native submission failure the context is quarantined and the caller retains the packet for its error/drain path. A return record releases one packet lease, not all packets sharing an input block; Layer 1's outstanding-sample accounting still controls final block reuse.

Output descriptors follow decode submission order. Consumers schedule by PTS and epoch; PTS need not be monotonic in DTS order. There is no automatic presentation-order queue in this component.

## Fence-safety proof and DPB lifetime

For each surface generation maintain:

- `D`: the greatest submitted decode fence that writes the surface **or reads it as a reference**.
- `C`: the joined downstream consumer fence supplied on return, or zero if never used downstream.
- `R`: whether its codec DPB hold is active.
- `L`: whether the downstream consumer has returned the lease.

A submitted generation can be recycled if and only if:

\[
L \land \neg R \land D_{completed} \ge D \land C_{completed} \ge C.
\]

Each term is necessary. Queue-slot release alone does not set `L`. Returning a descriptor does not imply `C` has completed. Dropping the DPB hold does not imply preceding reference reads have completed. Decode completion alone does not end downstream compute access. Generation numbers reject stale/double returns and are never wrapped into reuse; an exhausted slot remains unavailable.

Proof by ownership induction: Reserve selects only an inactive slot or one satisfying the conjunction. Therefore any resource/allocator overwritten by the next generation has no submitted old writer/reader remaining. Submitted establishes a nonzero `D`. Every later reference use extends `D`; a single serial decode queue orders those uses. Return records `C` exactly once. ReleaseReference only clears `R`. None of those transitions bypasses the conjunction. A context reserved but not submitted is also unavailable until explicitly canceled. After actual submission fails, it is not canceled/reused.

There is also a state-transition hazard before reuse: a reference may simultaneously be desired for compute and future decode. The backend requires its consumer return **before** recording a new reference use and queues a GPU wait for `C`. The consumer must enqueue all reads, restore `COMMON`, enqueue its joined fence signal, and only then publish `VideoSurfaceReturn`. Consequently all consumer accesses finish before the decode reference transition. Multiple downstream queues must join into this one fence before returning; a fence from only one reader is insufficient.

Layer 3 should consume the source NV12/P010 texture promptly and retain a separate processed GPU texture for presentation/interpolation history. Holding the native DPB source until a future presentation deadline can unnecessarily backpressure decoding. The pool deliberately returns `WouldBlock` rather than racing resource states.

`SurfaceAt(id)` is an immutable resource-table lookup for a descriptor-holding consumer. `Surface(handle)`, `ReturnSurface`, `ReleaseReference`, `Statistics`, and all ledger mutation belong to the decode owner. Consumers use a reverse `SpscRingBuffer<VideoSurfaceReturn, N>`; `PollReturns` drains a caller-bounded number on the decode owner. One SPSC edge must not have multiple producers. No native `AddRef` or `Release` occurs per frame.

Before teardown, stop submissions, return every published consumer lease, and drain reverse queues. `DrainAndClose` returns `WouldBlock` if a live device still has unreturned consumer leases. Once returned, it may block in **teardown only**, signaling fresh queue markers (including partially submitted failures) and waiting for the joined consumer high watermark. Device removal permits native cleanup. If native signaling/wait setup fails while the device remains live, the method retains resources and returns failure; the owner must retry or handle device removal. Destroying with outstanding leases or ignoring this teardown failure violates the lifecycle contract and can retain native resources rather than release memory still in use. Ordinary successful teardown is covered by hardware tests; arbitrary driver-failure recovery is not proven.

## WASAPI endpoint and continuity

The COM-initialized endpoint owner prepares the exact PCM format, obtains `IAudioClient3`, queries supported engine periods, and chooses the native minimum unless an admissible explicit period is requested. It initializes shared event mode and all native resources before sealing. Unsupported formats fail; there is no implicit resampling or format fallback. [Shared-stream initialization and period constraints](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)

The PCM producer writes through `TryAcquire`/`Commit`. The endpoint owner calls `Start`, `Pump`, `Clock`, and `Stop`. `Pump` polls its event with timeout zero, obtains endpoint padding, and copies at most the free endpoint frames from the fixed cyclic buffer. Underflow writes silence within the endpoint's requested span; overflow gives producer backpressure. There is one necessary PCM copy into native endpoint memory. WASAPI owns that memory and bounds its availability. [Render-buffer contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-getbuffer)

Clock tuples contain device position, native frequency, normalized monotonic timestamp, precision, and continuity generation. **Device position is not assumed to count sample frames.** Divide by `GetFrequency`; the correlated QPC returned by `GetPosition` is already in 100 ns units. `S_FALSE` yields an imprecise tuple without spinning for another sample. Stop preserves the native position; a new/reset stream has a new origin. [Native clock units and restart behavior](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)

Silence insertion changes generation, and recovery changes it again. Clock observations remain imprecise while silence is being inserted and until hardware position passes the last queued silence frame. Merely refilling the software ring does not restore media continuity. Stop also changes generation. Generation/submitted-frame exhaustion fails closed instead of silently wrapping. Seek flushes through `Reset`; format change still requires coordinated drain and a new prepared session. An invalidated endpoint is now replaced in place, with the ring kept and a null endpoint while none can be opened ([recovery](recovery.md)).

The playback controller must map an actual audio position to its media PTS before calling `Reanchor`. At initial playback this is the first PCM PTS plus the native elapsed stream time. After seek or missing audio, it must use the new epoch and the known decoded-PCM position, accounting for inserted silence; the clock cannot infer that correspondence from a hardware counter. Pauses/silence do not silently advance a guessed media origin. Audio decoding and this timeline controller are outside the delivered sink.

## Predictive alpha-beta discipline

Let `q_k` be normalized monotonic time in 100 ns units, `p_k` the device position, `f` its frequency, `p_0` the anchor device position, and `m_0` the corresponding media time. Work in relative coordinates to avoid subtracting large absolute floating-point timestamps:

\[
y_k=10^7(p_k-p_0)/f,\quad d_k=q_k-q_{k-1},\quad
\hat x_k^-=\hat x_{k-1}+\hat v_{k-1}d_k,\quad e_k=y_k-\hat x_k^-.
\]

For configured time constant `tau`, use:

\[
r_k=\exp[-d_k/(10^7\tau)],\quad \alpha_k=1-r_k^2,\quad \beta_k=(1-r_k)^2,
\]

\[
\hat x_k=\hat x_k^-+\alpha_ke_k,\qquad
\hat v_k=\operatorname{clamp}(\hat v_{k-1}+\beta_ke_k/d_k,\,1-s,\,1+s).
\]

`s` is the maximum skew in fractional units (`ppm × 10^-6`). This is a second-order phase/rate estimator. It does not adjust the physical audio oscillator.

For fixed sampling interval and without clamp/rejection, normalized error propagation is

\[
A=\begin{bmatrix}1-\alpha&1-\alpha\\-\beta&1-\beta\end{bmatrix},\quad
\det(\lambda I-A)=\lambda^2-(2-\alpha-\beta)\lambda+(1-\alpha).
\]

The selected gains give `(lambda-r)^2`, with `0 < r < 1`; hence fixed-period noiseless error converges. Equivalently, the Jury conditions are `alpha > 0`, `beta > 0`, `2 alpha + beta < 4`. This derivation is for the linear fixed-interval model; rejection/clamping and variable observation intervals do not inherit an unconditional convergence theorem. They are bounded and exercised by the synthetic tests.

At query time `q`, estimated media time is `m_0 + x_hat + v_hat(q-q_k)`. The predicted monotonic deadline for media PTS `M` is

\[
q_{deadline}=q_k+\frac{M-m_0-\hat x_k}{\hat v_k}.
\]

The result is normalized QPC-domain **100 ns**, not raw `QueryPerformanceCounter` ticks. Normalize raw QPC using its actual frequency before querying. Use the returned epoch to discard old deadlines after reanchor. Rounding and signed overflow are checked; `INT64_MIN` remains the unknown-timestamp sentinel. Floating-point phase uses relative time; sessions should reanchor at real discontinuities rather than maintain an indefinitely growing origin.

Default tuning is a 0.5 s time constant, ±1000 ppm rate clamp, 2–50 ms admitted observation spacing, 20 ms innovation bound, three consecutive soft rejections, 250 ms extrapolation horizon, and a ±2 s deadline horizon. Backward position/time, frequency or generation change, and an observation gap beyond the limit invalidate immediately. Too-frequent observations return `TooSoon`; repeated/imprecise/stuttering/outlier observations do not update the filter and eventually require `Reanchor`. Queries before the latest observation, stale queries, unknown PTS, and out-of-range predictions return explicit statuses.

The endpoint owner is the only writer. Arbitrary readers use six atomic payload words and an atomic sequence, all sequentially consistent; there is no ordinary-memory seqlock data race. Two equal even sequence reads in the single total order admit one consistent publication, assuming fewer than `2^63` publications between the reads. Each query attempts at most four snapshots and returns `Busy` on interference. The atomic type is statically required to be always lock-free. This favors a straightforward portable proof over weaker-order micro-optimization. Publication occurs at clock observation frequency, not per sample.

The synthetic test uses ±500 ppm skew and deterministic ±40 microsecond timestamp jitter over 60 seconds. After five seconds, both media estimates and 33.3333 ms deadline predictions must remain within 100 microseconds of the known synthetic truth; rate error must be below 20 ppm. These are measured test conditions, **not a universal sub-millisecond hardware/display guarantee**. OS scheduling, endpoint reporting error, render latency, and display cadence still matter.

## Verification and remaining validation

On this workspace, MSVC 19.51 x64 with Windows SDK 10.0.26100.0 builds Debug and Release using `/W4 /WX /permissive- /Zc:__cplusplus`, static CRT, and C++20. The tests are registered in CMake:

| Suite | Evidence |
|---|---|
| `media_timing_tests` | Skew/jitter convergence, deadline accuracy, discontinuity rejection, explicit reanchor, overflow/staleness, concurrent snapshot consistency, PCM wrap/full/partial/cancel, 500,000-frame SPSC integrity, all four surface-retirement conditions |
| `media_wasapi_tests` | Real shared endpoint, silent PCM, monotonic hardware/QPC samples, forced starvation and recovery, generation/precision handling |
| `media_d3d12_decode_tests` | Intel UHD 630: original 64×64 H.264 IDR and predicted P-picture, GPU decode status OK, zero affected macroblocks, native DPB reference use, delayed consumer fence, reverse SPSC return, early teardown rejection |
| `media_hevc_prepare_tests` | Real HEVC Main NV12 and Main10 P010 decoder/heap/resource preparation. HEVC bitstream correctness is covered by Layer 4's `media_hevc_decode_tests`. |
| Existing core/demux/IO suites | Regression coverage for Layers 0/1 |

Hardware tests return CTest skip code 77 for explicitly absent capabilities. Unexpected preparation failures fail tests. This machine executed the hardware tests without skips. D3D12 debug validation is enabled when the optional native debug interface is installed; decode status is checked regardless. Pixel readback is deliberately absent, so status OK alone is not a pixel-conformance proof. These small fixtures do not cover arbitrary B-picture streams, all codec features, every driver, or production playback. Device-loss injection (a real `RemoveDevice`) is covered by the playback tests in [recovery](recovery.md).

Allocation probes count global C++ allocation entry points and the application's Debug CRT allocation hook, with thread creation/destruction outside the measured windows. Portable steady-state tests and measured native pumping/submission intervals report zero. This does not audit all private OS/driver heaps. No test claims otherwise.

Portable timing tests also pass GCC on Linux (WSL Ubuntu 24.04) with warnings as errors and with AddressSanitizer/UndefinedBehaviorSanitizer. Reproduce without installing build packages:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
  tests/timing_tests.cpp tests/allocation_probe.cpp \
  src/clock/PredictiveMasterClock.cpp src/audio/PcmRingBuffer.cpp \
  src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp \
  -o build/linux/media_timing_tests
./build/linux/media_timing_tests
```

For sanitizers replace `-O2` with `-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer` and use a distinct output executable. Windows build/test commands remain in the [README](../README.md). Linux/Apple audio and GPU backends, Android/iOS execution, ThreadSanitizer and general codec conformance remain unverified. End-to-end MP4 playback verification is described in [Layer 3](layer3.md).
