# Hardware fault recovery: GPU device loss and audio endpoint loss

Status: implemented and tested on hardware (Intel UHD Graphics 630, the default Windows 11 render endpoint).
- A removed D3D12 device is rebuilt while playback continues. Nothing leaks, the playback epoch is kept, and audio is not interrupted.
- An invalidated WASAPI stream moves to a replacement endpoint, or to a silent null endpoint while none can be opened. The media clock stays continuous.
- Neither path uses the (sealed) arena or the C++ heap. Native objects are recreated by Windows and the driver, as in `Prepare`.

The GPU fault is raised in the tests as a real event. The audio fault is raised in-process since 2026-10-15: the sink's first failing result and the refused reopens are injected, and everything after them is the production path. The real exclusive-mode steal is still available, but only on request, because it takes the audio device from every application on the machine. See [Verification](#verification).

## GPU device loss

A TDR, a driver update or restart, adapter removal or `ID3D12Device5::RemoveDevice` removes the device. Afterwards every fence reads `UINT64_MAX`, and every call on the device or its children fails with `DXGI_ERROR_DEVICE_REMOVED` (or `_RESET`, `_HUNG`). All objects on it are dead, including the decode surfaces that the codec's DPB references.

### Detection

Either thread may notice first:
- **Presentation thread.** `D3D12Renderer::Convert` and `Present` return `RenderStatus::DeviceLost`, where they used to return `Failed`, when the device reports removal or the failing HRESULT is a removal code. `Present` also runs a health probe (`GetDeviceRemovedReason`) every 250 ms, so a removal is noticed while nothing is drawn (paused, idle controls).
- **Media worker.** `D3D12VideoDecoder::Submit` and `Statistics` already returned `DecodeStatus::DeviceLost` (fence at `UINT64_MAX`, or the device reporting removal).

Neither result is an error any more. Both call `PlaybackCoordinator::LoseGpu`.

### Handshake

The handshake is lock-free and runs on one atomic phase. Each store is a release that hands the decoder and the queues to the other thread:

| Phase | Set by | Meaning |
| --- | --- | --- |
| `Running` → `Lost` | either thread (compare-exchange; later losers do nothing) | Stop using the device. |
| `Lost` → `Parked` | worker | It has cancelled the parse of a held video packet, returned that packet and reset the H.264/HEVC DPB (the surfaces went with the device, so nothing is released to the decoder). From now on it does not touch the decoder, validate surface returns or apply seek commands. It returns video packets unread, so the demuxer keeps delivering **audio**, which keeps playing throughout. |
| `Parked` → `Resume` | presentation thread | Device, renderer and decoder have been rebuilt. |
| `Resume` → `Running` | worker | Demux has been repositioned. |

No thread waits on the other. The presentation thread keeps pumping window messages between phases. Seeks requested meanwhile stay queued and apply once the phase is `Running` again. Pause and resume apply at once (they only touch audio). A 10 s deadline bounds the whole recovery; exceeding it fails playback with stage 34.

### Rebuild (presentation thread, `Parked`)

1. **Discard.** Frame descriptors still queued from the decoder are dropped: their surfaces no longer exist. Surface returns still queued for the worker are dropped by the worker at `Resume`. The retired/validated counters are equalised, so end-of-stream detection stays exact.
2. **Release.** `D3D12VideoDecoder::Suspend` releases every native object and its device and fence references without waiting (a removed device retires nothing more). `D3D12Renderer::ReleaseDevice` releases swap chain, queues, RGB slots, pipelines, HUD, subtitle atlas, neural pass and the device itself. It returns the reference count left on the device, which must be 0.
   - This matters: while any reference remains, `D3D12CreateDevice` hands the *removed* device back. `CreateGpu` checks `GetDeviceRemovedReason` on the device it receives for exactly that reason.
   - A device that is not actually removed would first be drained (`WaitIdle`, bounded by removal).
3. **Recreate.** `D3D12Renderer::RecreateDevice` runs the same `CreateGpu` as `Prepare`: new DXGI factory and device, swap chain on the **same window** at its current size, RGB slots at the same capacity, pipelines, HUD, neural pass and weights.
   - `SdfSubtitlePipeline::Recreate` creates the atlas, upload ring and pipelines again. Shaped cues and glyph metrics are kept in the arena; the resident glyphs went with the old atlas and are paged in again as needed (see [subtitles](subtitles.md)).
   - Then `D3D12VideoDecoder::Resume(device, consumer fence, epoch)` rebuilds decoder, heaps, surfaces, queues and query heap, and `Bind` re-creates the plane views.
   - The lease ledger (`SurfaceLeasePool::Reset`: every lease void, generations kept, so no stale handle validates) and the per-surface contexts are reused in place. GPU memory is the same set of sizes as at `Prepare`.
4. **Retry.** If the driver refuses a device (a restart in progress), whatever was built is released and the rebuild is retried every 500 ms until the deadline.
5. **Target.** Where decoding resumes:
   - paused: the frame on screen;
   - playing: the clock's media time, with catch-up (step 7);
   - a seek's first frame, or the stream's, still due: that epoch's own target.

   The target is clamped to the last frame, as for a seek. It is published with the phase `Resume`.

### Resume (worker)

6. **Reposition.** Held and queued packets are returned. `IsobmffParser::Reposition(start, preroll)` is the seek lookup **in the same epoch**, since everything read since the loss was returned. `start` is the target, or the next undecoded audio packet if that comes earlier, so audio stays contiguous.
   - Audio packets that come round again but were decoded before the loss are skipped. The AAC decoder, the PCM ring, the endpoint and the clock never notice the recovery.
   - Pictures before the target are decoded as references only (`NoDisplay`), and pictures before the next random-access point are skipped.
7. **Catch-up** (playing only). While decoding catches up, the display floor follows the clock plus 150 ms (covering 4K decode and conversion). Pictures already late when decoded stay references, never converted, so video rejoins the audio without a late frame. The first displayed frame ends catch-up.
8. **Landing.** The renderer shows the first frame of the current epoch as soon as it is in order, even while paused, like a seek's first frame, and records it as `RecoveryLanding()`. Until then (at most 1 s) it does not compose a controls-only image, which would show black under the HUD: the old picture's history texture went with the device.

The epoch, the clock, the reorder depth, the HUD state, window size and fullscreen, picture settings (reconstruction, sharpness, neural, guided chroma), subtitle track and cue indices, and DSP settings (clarity, spatial) are all untouched. The engine has no volume or mute control; the DSP settings are its only user audio state.

`PlaybackCoordinator::LastDeviceRecovery()` records the removal reason, the detecting thread, the attempts, the references left on the lost device, the target, and the time of each stage.

## Audio endpoint loss

`Win32WasapiAudioSink` absorbs endpoint faults inside `Pump`, `Clock`, `Start`, `Stop` and `Reset`. These faults are:
- headphones unplugged;
- a Bluetooth device dropping out;
- another application taking the endpoint in exclusive mode;
- a driver or audio service restart.

`AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_RESOURCES_INVALIDATED` and `AUDCLNT_E_SERVICE_NOT_RUNNING` are no longer errors. Only other failures still fail playback.

- **Detach.** The client, render and clock services are released. The PCM ring stays (producer and format unchanged), so the next frame played is the ring's next frame.
  - Frames already handed to the dead endpoint (at most its buffer) are skipped, never replayed.
  - `EndpointOffsetFrames()` adds the frames handed to replaced endpoints. The coordinator anchors media time as `offset + position / frequency − silence`, so the mapping from device position to media stays exact across any number of endpoints.
  - Each endpoint change starts a new clock generation, so the clock re-anchors.
- **Replacement.** The stream reopens on the default render endpoint (or the requested one) in its own format. When the new endpoint's shared mix differs, Windows converts (`AUTOCONVERTPCM`), and a replacement always allows this.
  - It reopens at once only if the default endpoint changed (unplugged headphones, speakers now default).
  - For the *same* endpoint it waits for the retry interval. An exclusive-mode application may have taken it, and reopening at once would fight that application for the device; or its driver is restarting.
- **Null endpoint.** While no endpoint can be opened (`AUDCLNT_E_DEVICE_IN_USE`, `E_NOTFOUND`, ...), the sink plays nothing but consumes PCM at the stream rate on the QPC clock (`PcmRingBuffer::Skip`). A shortfall is accounted as silence, like an underrun.
  - Its clock samples are exact by construction, so video keeps playing on time, silently.
  - Reopening is retried every 200 ms. The null endpoint keeps playing until the replacement client is ready, including the time `Open` takes (170 ms observed for an endpoint just released from exclusive mode), so the media clock carries on at 1x instead of stepping back.
- **Following the default endpoint.** An `IMMNotificationClient`, embedded in the sink and registered when no endpoint ID was requested, raises a flag on default-device and device-state changes. At its next `Pump` the sink moves a healthy stream to a new default: it stops the old endpoint, and whatever that endpoint still held is skipped. A stream on the null endpoint tries the new default at once.

`EndpointRecovery` (losses, migrations, detachments, followed default changes, last loss and last open results, and how many losses were injected by a test) is published as `PlaybackCoordinator::AudioRecovery()`. The coordinator also measures clock continuity at every re-anchor that follows an endpoint change (`Continuity()`). It compares the new anchor with the old endpoint's last prediction run on at 1x:
- **skip:** media ahead of the prediction, i.e. PCM lost with a dead endpoint's buffer;
- **hold:** media behind the prediction but after what had been reached, i.e. the timeline waited for a starting endpoint;
- **repeat:** media before what had already been reached; it must stay 0.

The "last prediction" is the one made from the last sample the old clock accepted. Until 2026-10-15 it was also taken from a sample of the *new* endpoint when the worker read the clock in the same loop pass as a reattach. That sample was extrapolated with the old clock past the instant the old endpoint stopped, so a sub-millisecond "repeat" could appear without any media being repeated. It showed up in 2 of 27 runs (0.02 and 0.09 ms, both with a hold of exactly one 10 ms observation interval). Such a sample is no longer kept as the old endpoint's prediction; 30 runs after the fix gave repeat 0.

## Verification

Fault injection runs in `media_player_demo`, on a thread of its own. The thread is started (and its CRT start-up complete) before allocation measurement begins.
- **GPU:** `ID3D12Device5::RemoveDevice` on the renderer's device, asynchronously, as a TDR would.
- **Audio, in-process (the CTest default, since 2026-10-15):** `PlaybackCoordinator::InjectAudioFault(AUDCLNT_E_DEVICE_INVALIDATED, 500)`. The media worker hands it to `Win32WasapiAudioSink::InjectInvalidation`.
  - At the next `Pump`, the result of `GetCurrentPadding` is replaced by the loss. Every endpoint open for the next 500 ms returns `AUDCLNT_E_DEVICE_IN_USE`, as when an exclusive-mode application holds the device.
  - Only those two results are replaced. Everything after them is the production path: the client is released, the null endpoint plays, the open is retried at 200 and 400 ms (refused) and 600 ms (opened), and the clock is re-anchored on the new stream.
  - The machine's audio device is never touched: other applications keep playing.
- **Audio, real exclusive steal (manual only):** `media_player_demo <file> --test-exclusive-steal`. A second stream opens the default endpoint in **exclusive mode** and holds it for 500 ms. Windows then invalidates the player's shared stream (`AUDCLNT_E_DEVICE_INVALIDATED`) and refuses new shared streams (`AUDCLNT_E_DEVICE_IN_USE`) until the exclusive stream is released.
  - This takes the device from every application on the machine. A media player playing meanwhile fails with `0xC00D4E85` ("the audio playback device is in use"). That is why it was moved out of the default suite.
  - CTest registers it only when the build is configured with `-DMEDIA_TEST_EXCLUSIVE_STEAL=ON` (`media_playback_audio_exclusive_steal_test`, `RUN_SERIAL`).
  - It checks the same things as the in-process test, plus that nothing was injected.

| Test | Fixture | Fault | Checks |
| --- | --- | --- | --- |
| `media_playback_device_loss_test` | H.264 + AAC, WebVTT test cues | removal at 1.5 s, playing | reason `DXGI_ERROR_DEVICE_REMOVED`, 1 attempt, 0 references left, replacement healthy, landing not before the target, captions in the landing image match the track, subtitle composites from the new atlas, no silence, no re-anchor, 0 drops/out-of-order, all PCM, DSP settings kept, 0 allocations |
| `media_playback_hevc_device_loss_paused_test` | HEVC + AAC | removal at 1.7 s, 0.7 s into a pause | the same, and the landing is **the held frame itself**, shown while still paused; afterwards every frame is presented, 0 drops |
| `media_playback_hevc10_4k_device_loss_test` | 4K HEVC Main10 + AAC, test cues | removal at 1.5 s, playing | as the first |
| `media_playback_audio_loss_test` | H.264 + AAC | in-process invalidation at 0.8 s, opens refused for 0.5 s | loss `AUDCLNT_E_DEVICE_INVALIDATED` (1 injected), a detachment (null endpoint), a migration back (last open `S_OK`), attached at the end, skip ≤ 50 ms, repeat 0, every picture presented or at most one dropped per loss, all PCM, DSP settings kept, 0 allocations |
| `media_wasapi_tests` (sink alone) | silence | `InjectInvalidation` with opens refused for 450 ms | detached meanwhile, the clock answering throughout, 1 loss (1 injected), 1 detachment, 1 migration, attached at the end, a new clock generation, 0 allocations |
| `media_playback_audio_exclusive_steal_test` (opt-in, `RUN_SERIAL`) | H.264 + AAC | exclusive hold at 0.8 s for 0.5 s | as the in-process test, nothing injected |

Measured on the UHD 630 (Release):

| Case | Rebuild | Stages | Landing |
| --- | --- | --- | --- |
| H.264 small, playing | 133–177 ms | lost device released 45–85 ms, renderer ≈48 ms, decoder ≈42 ms | 49–62 ms after the rebuild; audio: 0 silence, 0 re-anchors |
| HEVC 720p, paused | 134–178 ms | — | the held frame (0.9583 s), 94–152 ms after the rebuild |
| 4K HEVC Main10, playing | 206–265 ms (20 runs) | released 73–90 ms, renderer 48–61 ms, decoder 70–125 ms | 230–310 ms after the rebuild (IRAP decode and catch-up), 0 drops |
| 4K, 2 runs of about 40 | 2268 and 2346 ms | **releasing the removed device took 2137 ms in the driver** (timed once; the other run predates the stage timer) | the clock had reached the end: landed exactly on the last frame |

- Both detectors occurred naturally: the presentation thread and the decode worker, depending on which touched the device first.
- The worker had always parked before the presentation thread looked, so the parked stage measured 0 ms.

Audio (exclusive preemption, about 25 runs):
- The stream was invalidated within one clock observation.
- The null endpoint ran while the device was held; the sink reattached 143–384 ms after the release (the 200 ms retry granularity plus about 150 ms for the endpoint to start).
- Skip was 21–41 ms (the lost endpoint's queued audio), repeat 0, hold about 11 ms (the new endpoint's start).
- 72/72 pictures, with one dropped to stay in sync in 2 runs.

Audio (in-process invalidation, 2026-10-15, 30 runs after the continuity fix):
- 1 loss (injected), 1 detachment, 1 migration, attached at the end, in every run.
- Skip was 24–35 ms (the endpoint's queued audio, dropped with the released client), hold 10–19 ms, repeat 0.
- 72/72 pictures, 0 drops, 0 allocations.
- The reopen succeeds on the first retry after the hold, because the device is free: there is no 150 ms start-up of an endpoint just released from exclusive mode.

The exclusive-steal test was built but not run that day: it would have interrupted the audio of the user's machine.

The full suite: MSVC x64 `/W4 /WX` Release **85/85** and Debug **85/85**. An earlier full Debug run had one failure, `media_playback_underrun_test`: one frame late by exactly one interval, under full-suite load. The test passed 20/20 on repeat and in the rerun, and its code path only gained the zero endpoint offset. GCC 13.3 `-Werror` ran every portable suite at `-O2` and under ASan/UBSan, including new unit tests for `IsobmffParser::Reposition`, `PcmRingBuffer::Skip` and `SurfaceLeasePool::Reset`.

## Limits and open items

- **The presentation thread blocks during the rebuild**: 130–265 ms typically, and once 2.1 s spent in the driver freeing a removed device that had 4K decode in flight. The window pumps no messages meanwhile. Moving release and recreation to a helper thread would keep the window responsive, but swap-chain creation then depends on the window thread pumping messages. Not done.
- **Endpoint activation runs on the media worker** (tens of ms, 170 ms after an exclusive release). Video continues from queued frames meanwhile, and the null endpoint accounts the time. A helper thread would remove the stall.
- Following a **default-device change** is implemented but has no automated test: changing the system default would modify user settings.
- The endpoint **format and surround decision** are fixed at `Prepare`. A 5.1 passthrough stream that moves to stereo headphones gets Windows' downmix, not the binaural fold-down; re-running the routing decision would need DSP reconfiguration.
- **Fragmented MP4** cannot reposition. After a device loss its video resumes at the next sync sample. Not tested.
- Losses during `Prepare` still fail preparation, including starting with no audio endpoint at all.
- Frames due during the outage are skipped (decoded as references), so a playing recovery presents fewer frames than the file holds.
- Allocation probes cover C++ operators and, in MSVC Debug, CRT allocations. Driver and OS allocations during recovery, as at `Prepare`, are outside them.
