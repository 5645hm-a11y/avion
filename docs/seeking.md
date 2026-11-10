# Seeking: epoch-based repositioning

Status: implemented for classic (non-fragmented) MP4 with H.264 or HEVC video and optional AAC-LC audio.
- Each seek lands on the frame whose display interval contains the target.
- Audio restarts on the target sample with no restart artifact.
- Rapid scrubbing is coalesced, so superseded targets never start a decode.
- The pipeline is not reallocated, and no allocation is measured after sealing.

## Protocol

A seek is a **playback epoch** transition, not a queue-cursor reset. Every `MediaPacket` and `VideoFrameDescriptor` already carries `playback_epoch`. The UI thread owns the newest requested epoch; the media worker owns the epoch it has repositioned to.

1. **Request (UI thread).** `PlaybackCoordinator::RequestSeek(target, scrub)` has these callers: the dock (`HudCommand::Scrub` while a timeline drag moves, `HudCommand::Seek` on release, a click or a key), commands, chapter navigation and scripted tests.
   - It clamps the target to `[0, video duration − 1]`, so a target at the very end still lands on the last video frame, even when the audio track runs longer.
   - A plain seek (`scrub = false`) is sent at once and replaces any scrub target still waiting. A scrub seek follows the rule in [Coalescing](#coalescing).
   - Sending publishes a 64-byte `ControlCommand{Seek, epoch + 1, target}` on the existing SPSC control queue.
   - It then calls `D3D12Renderer::BeginEpoch(epoch, now, target)` right away. This frees every queued RGB slot and clears the reorder window, so no earlier frame can reach the screen, even before the worker wakes.
2. **Reposition (media worker)**, in order:
   - **Coalescing.** Drained seek commands are only recorded. After the drain, the worker applies the newest one and counts the others as superseded. Pause and resume commands are applied in order as before; they commute with the seek.
   - **Forward continuation** (since 2026-10-15). When the target lies ahead in the group of pictures being decoded, the worker carries on decoding instead of restarting from the sync sample. Only audio and the clock are repositioned. See [Forward continuation](#forward-continuation-2026-10-15). The steps below are those of every other seek.
   - **Input leases.** The held packet (its codec parse cancelled, if any) and every packet already queued return their block leases. Packets still inside the decoder come back later through `packet_returns_`; `IsobmffParser::ReturnPacket` accepts leases from any earlier epoch.
   - **Demux.** `IsobmffParser::Seek(target, epoch, audio_preroll)` binary-searches each track and releases any in-flight read.
     - Tracks with `stss` search the sync-sample table for the last sync sample presenting at or before the target, or the first sync sample if none does.
     - Other tracks search their samples by decode order.
     - Audio tracks then step back `audio_preroll` access units (2 here), bounded by the first sample.
     - There is no I/O and no allocation, and later packets carry the new epoch. A classic file becomes seekable once the scan has admitted every sample.
     - Each probe is a table lookup in the compact index ([Layer 1](layer1.md#sample-index-long-form-files)): a few microseconds even on 1.5 million samples, so a seek to 3 h 30 min costs the same as one to 3 s.
   - **Codec.** `H264SyntaxParser::Reset` / `HevcSyntaxParser::Reset` hand every DPB hold to `D3D12VideoDecoder::ReleaseReference` and require a new IDR/IRAP picture. For HEVC, the CRA's NoRaslOutputFlag then discards its RASL pictures. `D3D12VideoDecoder::SetEpoch` makes later submissions require, and later frames carry, the new epoch.
   - **Audio.** `Win32AacDecoder::Flush` (MFT flush and new stream) drops pending PCM and codec state, including a completed end-of-stream drain. `Win32WasapiAudioSink::Reset` stops the stream if needed and calls `IAudioClient::Reset`, which drops the endpoint's queued frames and restarts device position at 0. It then discards the PCM ring (a consumer-side `PcmRingBuffer::Discard`) and zeroes the silence accounting.
   - **Clock.** `PredictiveMasterClock::Invalidate()`. The worker publishes the completed epoch (release), after clearing end-of-input and the audio end time.
3. **Refill.**
   - **Audio pre-roll.** Decoding restarts two access units before the target's unit.
     - The Microsoft AAC MFT withholds its first output after a flush, so each output belongs to the previous input. PCM is therefore timed by counting output frames from the first decoded unit, exactly as the clock does.
     - The first `(target − first unit's PTS) × rate` output frames only rebuild the decoder's overlap state, and `Win32AacDecoder::Skip` drops them. This includes output that arrives only with the end-of-stream drain.
     - The first kept frame is the target sample. It becomes the new clock origin (exact to the sample) and is entered with a 2.5 ms linear fade-in (`FadeIn`), so starting mid-waveform cannot click.
     - The endpoint then starts again through the ordinary prefill path.
   - The next precise device tuple re-anchors the clock to `origin + position/frequency − silence/rate` under the new epoch.
   - Without audio, or past the last audio packet, the endpoint's silence clocks the target.
   - Video decodes from the sync sample. Pictures that end at or before the target are submitted with `NoDisplay`, so they are decoded as references and retired without GPU reads.
   - **Pictures neither shown nor referenced are not decoded** (since 2026-09-30). A picture that ends before the target and that no other picture predicts from (H.264 `nal_ref_idc` 0; HEVC sub-layer non-reference pictures of the highest sub-layer) is parsed, its syntax state is committed as if it had been decoded, and its packet returns unread by the GPU. Later pictures decode bit for bit as before. See [Landing on long-GOP files](#landing-on-long-gop-files).
   - **Remote sources prime a lead.** Over HTTP the clock starts or restarts once eight further displayable pictures are decoded (or the input ends, or one second has passed), not on the first picture. The next random-access picture is large and precedes the pictures shown before it, and a link not much faster than the programme needs that lead. Local files do not wait. The landing picture is on screen at once in both cases.
4. **Landing (UI thread).**
   - The first frame of the new epoch is presented as soon as presentation order allows (`ReorderWindow::Ready`), without waiting for the clock or for resume. This also works while paused, so scrubbing a paused video shows the new frame.
   - Later frames follow the re-anchored clock. A clock still anchored to an earlier epoch is never used as a deadline for newer frames: they are held.
   - Presenting a frame sets the HUD's media position to that frame's PTS, so the ring's committed winding lane collapses on the first target frame.
   - **The landing frame stands for the target.** A target rarely falls on a frame boundary; the landing frame is the one on screen at that instant (`pts ≤ target < pts + duration`). For that one frame, captions and the dock's position are taken at the target, not at the frame's first instant. A seek to the start of a subtitle cue therefore shows that cue on its landing frame, also while paused ([subtitle search](subtitle-search.md)). `SeekPresentation::subtitles.time` records the time used; later frames use their PTS as before.

## Coalescing

Coalescing works at two points:

- **Worker (queue drain).** Of all seek commands drained in one pass, only the newest is applied. A burst that reaches the worker together therefore costs one repositioning.
- **UI thread (scrub mode).** During a continuous drag, at most one scrub seek is in flight, meaning sent but its first frame not yet on screen.
  - A newer scrub target goes into a single pending slot, replacing any target already waiting there; each replaced target is counted as coalesced.
  - When the in-flight seek's frame is presented, the pending target is sent (`PumpScrub`, called on every UI loop pass and before each request). A seek that never lands stops blocking the pending one after 1 s.

Why not simply send on every event? Each seek would flush its predecessor before the predecessor could show anything. On 4K, a fast drag would then show nothing until it stopped, and the decode of every superseded target would be wasted. With the rule:
- every decode that runs ends in a visible frame;
- the frame rate during a drag is the decoder's seek rate;
- after release, the final target is at most one in-flight seek away.

Both points preserve two accounting identities, which the scrub tests assert:
- requests = sent + coalesced;
- sent = applied + superseded.

## Lifetime and concurrency arguments

- **One producer, one consumer, as before.** No queue gains a producer or consumer and no counter is reset concurrently. The control queue is UI → worker; `frames_` is worker → UI; `surface_returns_` and `packet_returns_` are UI → worker and decoder → worker. Epochs only filter: stale descriptors still travel the queues to their owners.
- **Surfaces.** Stale frames still in `frames_` are returned by `Convert` with fence 0 (no GPU access) and validated by the worker like any other return. Surface generations, not epochs, prove lease identity, so `ReturnSurface` accepts any epoch from the first to the current one.
- **RGB slots.** Slots freed by `BeginEpoch` can be reused only after their GPU fence retires: `Convert` checks the fence before reuse. A frame already staged for presentation finishes; `Retire` pops the reorder window only when the front entry names that slot.
- **End of stream.** End of input counts only when the worker's published epoch equals the UI's, so a seek from the end cannot be mistaken for completion. The exit test also requires the clock estimate to belong to the current epoch.
- **Audio.** The PCM ring, AAC decoder and endpoint all belong to the worker (producer and consumer are the same thread), so the flush and the trimming are sequential. The endpoint is stopped before `IAudioClient::Reset`, as the API requires.
- **Allocation.** The added state is fixed-size: a 16-entry release array on the stack, one pending scrub target, and the per-seek and scrub result records in the coordinator. The allocation probes cover the whole scripted run, including a 93-request scrub.
- **Burst test hook.** `hold_controls_` is an atomic flag the worker checks before draining. The scrub test holds draining while it commits four seeks, so all four are drained together. It only delays consumption; it adds no lock.

## Measured results

Release build on Intel UHD 630 with the platform HEVC decoder, three runs per stream. Latency is measured from the UI request (the same call a ring release makes) to the `Present` that shows the target frame. It includes demux lookup, decode from the sync sample, conversion and flip submission, but not display scan-out.

**Single seeks** (`--test-seek`):

| Stream | Seek | Landing (target → frame PTS) | Request → first target frame | Request → clock re-anchored |
|---|---|---|---|---|
| H.264 64×64, IDR every 2 frames | forward 0.8 → 1.7 s, playing | 1.6667 s, exact | 1.4–1.8 ms | 17–26 ms |
| | backward → 0.6 s, paused | 0.5833 s, exact | 1.4 ms | after resume |
| HEVC 720p, GOP 24, B-frames | forward, playing | exact | 19.7–21.1 ms | 38–40 ms |
| | backward, paused | exact | 16.9–18.2 ms | after resume |
| HEVC Main10 3840×2160, GOP 24, B-frames | forward, playing | exact | 132–185 ms | 146–194 ms |
| | backward, paused | exact | 96–112 ms | after resume |

Audio restarted on the target sample itself: `1.700000 s` and `0.600000 s` for targets 1.7 s and 0.6 s, within one 48 kHz sample.

The HEVC seeks decode 16 reference-only pictures from the sync sample at 1.0 s to the 1.7 s target, and 14 from the sync sample at 0 s to the 0.6 s target. Latency scales with the distance from the preceding sync sample. At 4K, decoding itself dominates: a Main10 reference picture of these fixtures takes about 5.5–6.5 ms from submission to its decode fence on this GPU, measured on 2026-10-16 ([Seek catch-up](#seek-catch-up-2026-10-16)). That section also explains a wrong claim of 2026-10-15 that decoding took only 0.01–0.16 ms. The sub-15 ms goal is met for H.264 here, and for HEVC only when the target is at most about one picture from a sync sample.

**Scrubbing** (`--test-scrub`). UI events at 120 Hz (8.33 ms apart) drag the target from 0.5 s forward to 2.4 s over 0.5 s, then back to 1.1 s over 0.25 s: 93 requests in all. The first event arrives as a backlog of three requests.

| Stream | Seeks sent (of 93 + 3 burst) | Coalesced | Frames shown while dragging | Longest gap between them | Last request → final frame | Final landing |
|---|---|---|---|---|---|---|
| H.264 64×64 | 96 | 1 | 92 | 9–21 ms | 1.3–1.5 ms | 1.0833 s, exact |
| HEVC 720p | 62–63 | 34–35 | 58–59 | 26–27 ms | 7.6–9.3 ms | exact |
| HEVC Main10 4K | 12–14 | 83–85 | 8–10 | 146–188 ms | 42–77 ms | exact |

A frame appears during the drag each time a seek completes, so the gap equals the per-seek decode time:
- H.264 keeps pace with the 120 Hz events.
- HEVC 720p reaches about 40 frames per second.
- 4K is decode-bound at about 150 ms per frame, since each landing decodes up to 23 pictures from the sync sample. Coalescing keeps that cost from compounding: 83–85 of 93 targets never started a decode.

**Burst.** After the drag, four immediate seeks (1.9, 0.4, 2.2 and 1.29 s) are committed while the worker holds its queue. On every stream exactly 3 were superseded and one repositioning ran. It landed on 1.2500 s (target 1.29 s), with audio starting at 1.290000 s. (Until 2026-09-30 the last target was 1.3 s, the instant the fixture's first cue ends; a landing frame now shows the captions of its target, and the caption check needs a cue there.)

Every run had zero dropped or out-of-order frames and zero allocations.

## Verification

| Test | Checks |
|---|---|
| `media_demux_tests` (MSVC, GCC, ASan/UBSan) | **Seek:** target exactly on a sync sample, one tick before it, before the first sample, past the last. Epochs only move forward. A stale published packet can still be returned after a seek; an in-flight read is released. Fragmented files are not seekable. **Pre-roll:** AAC tracks step back 1 or 5 units, bounded by the first sample; zero pre-roll is unchanged; a video track ignores pre-roll. Zero allocations. |
| `media_timing_tests` | `PcmRingBuffer::Discard` frees the capacity and later frames read back intact. |
| `media_aac_preroll_tests` | Runs the production `Win32AacDecoder` (Microsoft AAC MFT) on encoded two-tone audio against a continuous decode. Without pre-roll the restart is audibly wrong: error 0.198 against a 0.186 peak. With 1 or 2 units of pre-roll, trimming by output count reproduces the continuous decode exactly (error 0), including a target inside the final unit reached through the drain. `FadeIn(120)` is an exact linear ramp that leaves later samples untouched. Zero allocations. |
| `media_playback_seek_test`, `media_playback_hevc_seek_test`, `media_playback_hevc10_4k_seek_test` | Forward seek while playing, backward seek while paused, resume. Each landing frame satisfies `pts ≤ target < pts + duration`; audio starts within one sample of the target; the paused landing is presented; the clock re-anchors after each seek; playback completes. No drops, no out-of-order frames, no errors, zero allocations. |
| `media_playback_scrub_test`, `media_playback_hevc_scrub_test`, `media_playback_hevc10_4k_scrub_test` | **Drag and backlog:** at least 3 frames shown while dragging, at least 1 target coalesced, exact final landing, both accounting identities. **Burst of four:** drained together, exactly 3 superseded, exact landing, audio starting on the target sample. No drops, zero allocations. |
| `media_multitrack_demux_tests` (MSVC, GCC, ASan/UBSan) | **Continuation:** `IsobmffParser::Continue` on a B-picture GOP. It refuses: before the scan, an epoch that is not newer, a non-video or unknown track, a target in the next GOP, a target before the end of a published picture (0.4999999 s against a picture shown 0.4–0.5 s). It accepts exactly at 0.5 s: video carries on at the next sample in the new epoch, audio restarts one unit before the target's unit, 4 samples resumed. It refuses at the end of the track, and for a track without `stss`. 0 allocations, no lease left. |
| `media_playback_hevc_seek_forward_test`, `media_playback_hevc10_4k_seek_forward_test` | The same 1.5 → 2.5 s seek on a single-GOP clip, with and without continuation. Both land exactly with audio on the target sample; the first is a continuation and the third seek is not; fewer reference pictures decoded and an earlier landing with continuation; 0 drops, 0 allocations. |
| `media_playback_hud_test` | The scripted ring drag seeks to the end while paused. The seek is accepted (none rejected), its landing frame is presented, and the HUD's winding lane is exactly 0 at the end: collapsed on arrival, not by timeout. |

Test windows open without keyboard activation (`PlaybackOptions::activate = false`). In an earlier run, desktop keystrokes reached a focused test window and toggled pause.

## Limits

- Fragmented MP4 is not seekable. `RequestSeek` returns false and the HUD shows its rejection feedback.
- Accuracy relies on sync samples being true random-access points (IDR for H.264, IRAP for HEVC). If a track's sync-sample PTS order does not follow decode order, the binary search can pick a neighbouring sync sample; decoding still starts at a random-access point. Pictures before the first random-access point are skipped.
- The outgoing audio stops abruptly at the seek, because the endpoint reset discards its queued frames; only the incoming audio fades in. A fade-out would need the old PCM to keep playing for a few milliseconds after the request.
- Pre-roll is two access units, which is right for AAC-LC. HE-AAC (SBR) and Opus would need more, but neither is decoded by this engine.
- At 4K, an exact scrub frame costs the decoder's seek rate: about 6 ms per reference picture from the sync sample. Since 2026-10-16 a drag shows the target's sync sample alone when more than two would be needed ([Scrub preview](#scrub-preview-2026-10-16)); forward drags continue in the GOP (2026-10-15) or take pictures already decoded (queue seeks).
- A scrub seek already sent cannot be withdrawn, so at most one superseded seek per drag is decoded to completion.

## Start-up is epoch 1 (2026-09-28)

Initial playback now follows the same rules as a seek landing:
- **Preview.** The renderer presents the first picture of epoch 1 as soon as it is in order, before the clock has a deadline for it (`preview_` starts true).
- **Primed gate.** The worker starts the endpoint, and therefore the clock, only when PCM is queued and the first picture of the current epoch has been handed to the swap chain, or input has ended. The same gate applies to each epoch after a seek. (Until 2026-09-30 the gate opened one step earlier, when that picture's conversion had completed on the GPU; see the next section.)

**Why.** Previously the first picture waited for the clock. When the first valid clock sample arrived more than one frame interval after start, frame 0 was already past its interval and was dropped. That was the long-standing intermittent `media_playback_hevc10_4k*` failure. It was reproduced deterministically on a clocked-down GPU (for example 4K PQ in Debug, `Last drop: pts=0.0000s late by 41.7ms`), and it disappeared after a GPU warm-up.
- With the fix: three cold runs out of three were 72/72 with 0 drops, and the 4K tests passed 5 consecutive Release repeats.
- `D3D12Renderer::LastDrop()` and the demo's `Last drop:` line now report the PTS and lateness of any dropped frame.

## Pre-roll and the presentation queue (2026-09-30)

Three rules keep a picture that is ready from being shown late. They were found on a 4K 59.94 fps HDR file that dropped pictures at start-up ([fixtures](fixtures.md#start-up-of-lg_black_4k60_hdrmp4-found-and-fixed) has the measurements), and they apply to every start and every seek.

1. **Pre-roll: the clock starts after the first picture is on its way to the screen.** Primed → Running needs PCM queued and the epoch's first picture decoded, converted (tone-mapped when HDR) and handed to the swap chain. End of input opens the gate, and so does one second after the first picture to show was decoded (`kPrerollLimit`), so a picture that cannot be shown never holds the sound back for ever. Remote sources still wait for their lead as well.
2. **A conversion never waits in the presentation queue.** Conversions and flips share one D3D12 queue. `ConvertFrame` used to enqueue a GPU-side wait for the picture's decode fence and then the conversion, so every flip enqueued after it waited for that decode too. Now a picture is converted only once its decode fence has completed (`WouldBlock` until then), and only one conversion is in flight at a time. The 16 RGB slots still fill as fast as the GPU converts; a due flip has at most one conversion in front of it.
3. **No controls-only image when a picture is due within one display refresh.** While the dock, captions or command bar are changing, the renderer composes controls-only images at up to 120 Hz. Between 60 fps pictures on a 60 Hz display those images filled the swap chain's queue, a `Present` then blocked until the next vertical blank (8 to 15.7 ms), and the next picture was handed over that much late. The picture's own image carries the controls, so the extra one is skipped (`until_picture >= refresh_period_`). At 24 fps there is room and the controls still refresh between pictures.

4. **A new swap chain's first image is composed in Preparing (2026-10-21).** `D3D12Renderer::Prepare` presents one black image on a swap chain it has just created, then calls `DwmFlush` until the compositor has composed it (`WarmUpFlip`). A swap chain's first `Present` and first composition are the slow ones: the compositor sets up the window's surface. Measured here, with the display idle, that first composition took 168–207 ms, the size of the lone ~150 ms stalls that failed `media_fixture_lg_hdr_cold_start_test` about once in 12 runs. It now happens before playback and its clock exist, and delays the start instead of a picture. The clock still starts 80–86 ms after `Run`. A warm session's swap chain has composed images already, so the next file does not do it. The demo prints `warm-up flip … (Present …; DwmFlush …)` on its `Pre-roll:` line.
   - First tried: waiting, before the clock started, for `IDXGISwapChain::GetFrameStatistics` to report the first picture displayed. On this machine it never reported it (S_OK, present count 0), although the compositor composed 245 frames meanwhile. Every cold start would have waited out the time limit, so it was removed.

A fifth idea was built, measured and removed: holding the clock until the whole pipeline was full and idle. It stopped the start-up drops by itself (0 of 15 starts), but it delayed the clock by 100 to 130 ms at every start and after every seek, and with rules 2 and 3 in place it changed nothing (0 of 20 without it, worst flip 0.66 ms late).

**Diagnostics** (demo output, all modes):
- `Pre-roll: first picture … clock started … with N pictures decoded; latest flip … after its deadline (pts; waited for …)`: `D3D12Renderer::WorstFlip()` is the picture handed over latest, and what it waited for: its own conversion, the back buffer, the swap chain's frame latency, `Present`, or nothing (the presentation thread was elsewhere).
- `Last drop: … (…; waited for …)`: the same for a dropped picture.
- `Longest calls on the presentation thread: convert … present … window messages …` (`PlaybackCoordinator::LongestCalls()`): the longest single call of each kind while the clock ran. The presentation thread is also the window's thread, so any of the three taking longer than a frame interval makes a picture late.

## Landing on long-GOP files

Measured on 2026-09-30 with a real downloaded file (`assets/test_fixtures/costa_rica_4k60_hdr.mp4`: H.264 Main, 1280x720, 59.94 fps, sync samples 4.6 s apart on average, so a landing decodes up to 277 pictures first). `media_fixture_subtitle_search_test` makes four seeks minutes apart while playing; Release build, Intel UHD 630, file in the page cache. Latency is from the request to the target frame on screen.

| Seek target | Before | Non-reference pictures skipped | and blocks shared |
| --- | --- | --- | --- |
| 26.0 s | 270 ms | 250 ms | 60 ms |
| 250.0 s | 636 ms | 570 ms | 100 to 150 ms |
| 195.0 s | 842 to 1009 ms | 770 ms | 172 to 195 ms |
| 130.0 s | 272 to 304 ms | 290 ms | 95 to 103 ms |

Two causes, found in that order:

1. **Decode work.** Of the 881 pictures between the four sync samples and their targets, 520 are non-reference B-pictures. They are no longer decoded. This alone saved about 10 %, which showed that decoding was not the main cost.
2. **Read amplification.** The demuxer asks the file source for one sample at a time, and the source answered each request with a fresh read of a whole 8 MiB block starting at that sample (about 2 ms from the page cache). Playback hid it (60 reads a second); a landing paid it for every picture on the way. The source now shares a block between every range inside it ([Layer 1](layer1.md)). The column "and blocks shared" has both changes; block sharing was not measured on its own.

Related numbers from the same day:
- 4K HEVC Main10 over the slow loopback link (`media_playback_hevc10_4k_http_seek_test`): the landing went from 126 to 199 ms (24 runs with skipping switched off) to 65 to 148 ms. With the faster landing the clock restarted before the link had delivered the next random-access picture, and one frame was dropped in 8 of 24 runs; the remote lead (above) removed that (0 of 24), and the clock now restarts about 400 ms after the request there instead of about 200 ms.
- What remains on this file is decoding the reference pictures from the sync sample (361 of the 881). Showing the sync picture at once as a preview, then the exact frame, would cut the perceived latency further and is not done.
- Not measured: a cold file (not in the page cache), where a landing now costs one block read instead of one per picture, and unbuffered mode.

## Forward continuation (2026-10-15)

Directive: "INTRA-GOP FORWARD SEEK CONTINUATION (Review Task 18)". A seek a little ahead used to flush the decoder and decode again from the preceding sync sample, including every picture the decoder had already passed.

**Rule.** When the worker applies a seek, it first asks the demuxer whether the target lies ahead in the group of pictures being read (`IsobmffParser::Continue`). It does when all of these hold:
- The sync sample a seek would start from (the last one presenting at or before the target) has already been published, so no sync sample lies between the next video sample and the target.
- No sample published since that sync sample is shown at or after the target (`pts + duration > max(target, 0)`, the 100 ns values its packet carried). One that is would have gone out in the old epoch and been flushed from presentation.
- The video track has a sync-sample table (without one, every sample is a sync sample and a seek starts at the target itself), and its next sample exists.

The coordinator adds two conditions: decoding has started since the last repositioning (so the chain really runs from that sync sample), and continuation is on (`PlaybackOptions::seek_continuation`, `SetSeekContinuation`).

**What a continuation does.**
- **Demux.** Every other track is positioned as by `Seek` (audio two units before the target's unit). The video cursor stays, and later packets carry the new epoch.
- **Packets already read.** Packets in the queue and the one in hand stay. As they are taken, video packets of the old epoch are re-stamped with the new epoch, because they are the next links of the decode chain; anything else returns unread. Only a continuation leaves packets queued across a seek, so the rule cannot touch any other case.
- **Codec and decoder.** No `Reset`: the DPB and the syntax state stay, and `D3D12VideoDecoder::SetEpoch` makes later frames carry the new epoch. A parsed but unsubmitted picture keeps its parse. Pictures that end before the target are decoded as references or skipped (non-reference), exactly as on the way from a sync sample.
- **Audio, clock and presentation.** As for every seek: the AAC decoder is flushed, the endpoint reset, the clock invalidated, and `BeginEpoch` drops earlier frames from presentation.

**Statistics.** `PlaybackCoordinator::WorkFor(epoch)` returns what each applied seek cost the decoder:
- whether it continued, and the pictures from the sync sample that were not read again;
- the reference pictures decoded before the target, and the pictures skipped.

`ContinuedSeeks()` counts the continuations. The demo prints a `decoder:` line under each `Seek` line.

**Where it applies.** The presentation path lets decoding run about a second ahead of the screen: 16 RGB slots, plus decoded pictures waiting for a slot. A target less than that far ahead has usually been decoded already and flushed with its epoch. Re-using those converted pictures would be a different optimisation, not done. Continuation therefore helps forward seeks of a second or more inside long groups of pictures, which is what real files have: `costa_rica_4k60_hdr.mp4` has sync samples 4.6 s apart.
- The generated 1 s-GOP fixtures rarely qualify. The existing `--test-seek` (0.8 → 1.7 s) crosses the sync sample at 1.0 s and is unchanged.
- Two fixtures with a single IDR for the whole 3 s clip were added: `--hevc-longgop` (720p) and `--hevc10-4k-longgop` (4K Main10), both with B-pictures.

**Measured** (`--test-seek-forward`, Release, UHD 630). Playing, the same seek from 1.5 s to 2.5 s twice: with continuation, then, after a seek back to 1.3 s, restarting from the sync sample. Three runs each:

| Stream | | Reference pictures decoded | Skipped | Landing |
| --- | --- | --- | --- | --- |
| HEVC 720p, one GOP | continued (47 pictures not read again) | 3 | 10 | 8.4–10.0 ms |
| | restarted from the sync sample | 20 | 40 | 27.3–30.5 ms |
| HEVC Main10 4K, one GOP | continued (47 pictures not read again) | 3 | 10 | 38.2–40.9 ms |
| | restarted from the sync sample | 20 | 40 | 149.5–162.6 ms |

Every landing was exact, with audio on the target sample (2.500000 s), 0 drops and 0 allocations.

**Where the rest of a landing goes.** This paragraph claimed on 2026-10-15 that a 150 ms landing was mostly waiting, from decode-queue timestamps that put each 4K picture at 0.01–0.16 ms. That was wrong: those timestamps do not bracket the decode. See [Seek catch-up](#seek-catch-up-2026-10-16): the landing is a chain of reference decodes of about 6 ms each.

**Limits.**
- Backward seeks, and forward seeks past the next sync sample, restart from a sync sample as before.
- A target already decoded (inside the read-ahead) used to restart too, since its pictures went out in the old epoch. Since 2026-10-16 such a seek keeps them ([Queue seek](#queue-seek-2026-10-16)); `--test-seek-forward` turns queue seeks off to measure continuation alone.
- A scrub drag forward inside a long GOP continues from wherever the previous landing left the decoder. The scrub tests' fixtures have 1 s GOPs, so they exercise it rarely.

## Seek catch-up (2026-10-16)

Directive: "ELIMINATING THE 22–26 ms 4K SEEK BURST GAPS". The directive was built on a finding of 2026-10-15: that 4K seek landings decode in bursts of about three pictures separated by idle gaps of 22–26 ms. **That finding was wrong, and there are no such gaps.**

**What was measured.** Both threads were traced (fixed arrays, test-only, removed afterwards) through a 4K landing with 20 reference pictures, with the decode lists' GPU timestamps mapped onto the CPU clock (`ID3D12CommandQueue::GetClockCalibration`).
- The landing was one strict chain, repeated 20 times: submit a reference picture; the next one is refused (`ReferenceReady`: its reference has not been returned); the decode fence completes about 5.5–6.5 ms later; the worker validates the picture; the next one is submitted 0.3 ms later.
- The UI thread had already retired each reference-only picture 2 µs after its submission. It was never what the chain waited for.
- Each decode list began 0.4–0.9 ms after its submission, and its last timestamp came 9 µs after its first, while its fence completed about 5.5 ms later. The decode engine works after the list's last command has run, so the timestamps never contained the decode. That is where 0.01–0.16 ms came from. The "bursts and gaps" were begin timestamps read during paced playback.

**So the landing is the decode itself:** about 6 ms per 4K Main10 reference picture of these fixtures on the UHD 630, one after the other, since each predicts the next.

**What was improvable: the round trip per link.** A reference could be used only once its consumer return had arrived (`SurfaceLeasePool::ReferenceReady`). That return travels through the presentation thread and the worker's validation, which waits for the fence. So every link cost about 0.7 ms of CPU and GPU round trip on top of the decode, and the GPU stood idle meanwhile.
- A picture that is not displayed is never read by the presentation side (it is retired with consumer fence 0). It is now submitted with `unread`, and as a reference it is usable at once: only the decode queue touches it, in submission order.
- The slot is still not reused before its return arrives and its decode has completed.
- The landing's chain is now submitted back to back. In the trace, 14 pictures were queued in 7.5 ms, and the decode engine ran without pause.
- `media_timing_tests` checks the rule: an unread reference is ready at once with no consumer fence; its slot is not free before its return; a displayed reference still waits for its return.

**Measured.** Release, UHD 630, three runs each, the same build with and without the rule. The first column of times is the seek before the slash:

| Seek | Before | After |
| --- | --- | --- |
| `--test-seek`, HEVC 720p (forward playing / backward paused) | 11.7–12.8 / 12.7–13.8 ms | 8.6–10.6 / 10.3–11.2 ms (one run 19.2 ms) |
| `--test-seek-forward` restart, HEVC 720p, 20 references | 26.3–29.5 ms | 21.1–21.6 ms (−22 %) |
| the same, its 11-reference seek back | 20.3–22.2 ms | 14.6–16.0 ms |
| `--test-seek`, HEVC Main10 4K | 39.9–50.4 / 47.7–49.1 ms | 39.8–44.6 / 51.5–53.9 ms (no change) |
| `--test-seek-forward` restart, 4K, 20 references | 144–150 ms | 137–147 ms (−4 %) |

- At 720p the round trip was a large share of each link (a picture decodes in about 1 ms), so it shows.
- At 4K the engine's 6 ms per picture is the limit, and there is no idle time left to remove. A 4K landing shortens only by decoding fewer pictures: [Forward continuation](#forward-continuation-2026-10-15), and [Queue seek](#queue-seek-2026-10-16) below.
- The 4K `--test-seek` figures are far below the 132–185 / 96–112 ms of 2026-09-28. That comes from the earlier work (shared blocks, skipped non-reference pictures, continuation: this forward seek now continues in its GOP), not from this change.

**Measurement fix.** A landing's latency was stamped with the time its presentation pass began, not when `Present` returned. So it left out the time spent composing the image, and a landing in the same pass showed 0.00 ms. It is now stamped after `Present`. All figures above use the corrected stamp.

## Queue seek (2026-10-16)

A forward seek to a picture already decoded used to drop it with every other picture of the old epoch and decode again from the sync sample. Now the new epoch **adopts** the old one's pictures from the target on.

**When.** The UI asks for a queue seek when the target lies ahead of both the picture on screen and the newest target (`PlaybackOptions::queue_seek`, `SetQueueSeek`).
- `D3D12Renderer::BeginEpoch(…, adopt, chain)` drops the converted pictures that end at or before the target, and keeps the later ones, re-tagged with the new epoch.
- **Kept:** at least one converted picture ends after the target. The adoption is then certain, and the command tells the worker so.
- **Pending:** nothing was kept. The worker decides: the target must lie before the end of the latest picture it has submitted for display in this decode chain. Meanwhile, old-epoch pictures wait in `Convert` (`WouldBlock`).
- The worker publishes its decision before it publishes the epoch. The presentation thread then resolves it (`ResolveAdoption`): adopted pictures join the epoch, and the others are stale as before.

**What the worker does for an adopted seek.**
- Video: nothing. No repositioning, no codec reset, no flush. `IsobmffParser::SeekOthers` repositions every other track.
- Video packets already read are re-stamped as for a continuation. Audio, the clock and presentation are handled as for any seek.
- The new floor is the target: pictures still to decode that end before it are references only.
- The landing is the first picture in presentation order, shown at once like any seek's.
- Pictures from earlier queue seeks of the same decode chain stay adopted (`chain_epoch_`: the last seek the worker applied that was not adopted).

**Where it applies.** A displayed reference picture must be converted before the next picture predicted from it can be decoded. So the decoder never runs more than a picture or two beyond the 16 converted slots, and "already decoded" in practice means "already converted": up to about 0.67 s ahead at 24 fps, 0.27 s at 60 fps.
- The pending path, with the worker's decision, is taken when nothing converted was kept. In the tests it ended in a refusal (the target was beyond what had been decoded) and the seek fell back to continuation, as it should.
- A pending seek that the worker accepts was not observed: there was never a picture decoded and not yet converted at the right moment.

**Measured** (`--test-seek-micro`, Release, three runs each): playing, 1.0 → 1.4 s and 1.6 → 2.1 s, then back to 0.9 s, then 1.0 → 1.4 s again with queue seeks off:

| Stream | Queue seek 1 | Queue seek 2 | The same seek restarted |
| --- | --- | --- | --- |
| HEVC 720p (1 s GOP) | 0.10–0.11 ms | 3.6–4.5 ms | 8.1–8.6 ms (3 references decoded) |
| HEVC Main10 4K, one GOP | 0.11–0.14 ms | 9.5–11.7 ms | 79.5–81.1 ms (11 references decoded) |

- Both queue seeks decoded nothing again (0 reference pictures, 0 skipped before the target), and every landing was exact with audio on the target sample.
- The first queue seek lands in the same presentation pass. The second waits up to one display refresh for the swap chain: the previous image's flip was still queued.
- `media_playback_hevc_seek_micro_test` and `media_playback_hevc10_4k_seek_micro_test` assert all of that:
  - two adopted seeks, adopted pictures counted (`AdoptedFrames`);
  - the restart decoding references;
  - the queue seek landing sooner;
  - 0 drops and 0 allocations.

**Scrubbing.** A forward drag is a run of queue seeks. Each lands in its presentation pass, so the next pending target is sent at once (`PumpScrub`), and the worker drains several together and keeps the decode chain. `--test-scrub` on 4K Main10 (one run, Release) showed 43 frames while dragging, the longest gap between them 70.4 ms, and 50 targets coalesced; the table above records 8–10 frames and 146–188 ms. The backward half of the drag still restarts from sync samples.

**A bug found on the way.** The first version set the worker's `random_access` flag after an adopted seek, as after a restart. The next queue seek was then refused, even when its landing had been kept, and the worker restarted. The pictures already kept then came again from the decoder, and the reorder window refused them: 25–31 out-of-order pictures in every scrub test. Now:
- an adoption keeps the flag as it is;
- the UI waits for the worker's decision on every queue seek, kept ones too. If one is ever refused, the renderer drops what it kept before anything can arrive twice (`ResolveAdoption(false)`).

## Scrub preview (2026-10-16)

Directive: "HYBRID KEYFRAME SCRUB PREVIEW (Review Task 17)". A backward drag, or a jump out of the group of pictures being decoded, restarted from a sync sample and decoded every reference picture up to the target: about 150 ms per frame at 4K. A drag now shows the sync sample itself in those cases, and the release lands exactly.

**The rule** (worker, for a scrub seek: `RequestSeek(target, true)`, command bit 2, `PlaybackOptions::scrub_preview`, `SetScrubPreview`).
- A queue seek (the target already decoded) lands exactly, as before.
- Otherwise the cost of the exact frame is the number of pictures, in decode order, before the target's own: from the sync sample (`IsobmffParser::SyncSample`, `DecodeDistance`, which reads at most `kPreviewReferences` + 1 samples), or from the next picture this chain would decode when the seek can continue in the GOP (the chain's sync sample plus the video samples taken since, less the one in hand). The smaller one counts. Non-reference pictures are skipped on the way but counted: the bound is an upper one.
- More than `kPreviewReferences` (2): **preview**. The demux, codec and audio are repositioned as for a restart, and the worker decodes from the sync sample until the first picture that can be shown (the sync sample itself in practice), submits it, and reads nothing more in that epoch. Its audio packets return unread and its clock never starts: the epoch is a still picture. The chain is marked as broken (`random_access`), so the next seek neither continues nor adopts from it.
- Only while the drag moves backward (the drag's own direction, from its consecutive requests: the targets sent lag the drag, and the picture on screen may itself be a preview behind it; bit 3 marks a forward drag). A forward drag stays exact: queue seeks and continuations serve it, and a preview would leave the decoder holding the sync sample alone, so every later target of that group of pictures would restart from it. A first version previewed forward too when the sync sample lay ahead of the screen; it cut the forward half from 20–40 to 16–26 frames (5 runs each), and was removed.
- The picture leaves the reorder window at once: the worker publishes the epoch in `preview_held_` once its picture is submitted, and presentation treats it like the end of input (nothing else of the epoch follows).

**Landing release.** A seek's landing picture (its interval contains the target) used to wait in the reorder window for `depth` successors like any picture. Every other picture the epoch shows ends after the target and, since presentation intervals do not overlap, starts at or after the landing's end: the landing is final as soon as it is in the window (`D3D12Renderer::LandingReady`). This applies to every seek. On the `--test-seek` targets it changed nothing measurable; exact drag landings on sync pictures, which waited for the B-pictures after them, took 2.7–12.9 ms with it and 6.8–32.6 ms without (traced drags, two runs each; not a controlled comparison).

**Release and settle.**
- The dock emits `HudCommand::Scrub` on every move that changes the target and `HudCommand::Seek` on release ([dock](kinetic-hud.md)). The release is a plain seek: exact frame, audio pre-roll, clock. When the last scrub seek already landed exactly on the release target (applied, not a preview), nothing is sent.
- A drag held still on a preview for `kScrubSettle` (0.25 s) sends the exact seek too (`SettledScrubs()`), so the resting picture is never an approximation for long, also when the gesture ends without a release (a cancelled capture).

**Honest preview state.** The worker publishes each applied epoch's decision before the epoch itself (`preview_epochs_`, 16 slots by epoch). While the landed epoch is a preview, the dock's knob is a ring and an amber diamond marks where the previewed picture is (`SetHudPreview`, `HudUniforms::preview`), and the first preview of a drag shows a status cue: `Keyframe preview · the exact frame follows on release`. `ShowingPreview()`, `PreviewSeeks()` and `SeekWork::preview` report it.

**Measured** (`--test-scrub`: 120 Hz events, 0.5 → 2.4 s forward over 0.5 s, back to 1.1 s over 0.25 s, then a release; Release build, UHD 630, same build with `--no-scrub-preview`; frames counted in the half of the drag they arrive in): see the table below.

| Stream | Scrub preview | Forward half: frames, longest gap | Backward half: frames, longest gap | Previews | Release → exact frame |
|---|---|---|---|---|---|
| HEVC Main10 4K, 1 s GOP | off | 20–40, 46.7–65.5 ms | 5–6, 67.6–97.4 ms | 0 | 30.9–41.7 ms |
| | on | 34–37, 53.2–90.1 ms | 11–15, 23.2–62.4 ms | 8–12 | 27.7–38.5 ms |
| HEVC Main10 4K, one GOP | off | 13–22, 59.6–143.1 ms | 1–4, 73.3–219.3 ms | 0 | 61.7–137.4 ms |
| | on | 26–37, 58.5–132.2 ms | 9–22, 23.1–158.6 ms | 9–21 | 80.0–86.4 ms |

Five runs of each, on 2026-10-16 with other processes busy on the machine (one to one and a half cores, and a browser on the GPU), so the ranges are wide. The directive's reference, 43 frames and a 70.4 ms longest gap, was one run of the whole drag; the same build without previews gave 26–46 frames, 67.6–97.4 ms that day.

Every run: final landing exact (1.0833 s for 1.1 s) with audio on the target sample after the burst, 0 drops, 0 out-of-order, 0 allocations.

**What the numbers mean.**
- A preview costs one sync-sample decode, conversion and flip: about 15–18 ms at 4K for these fixtures' IDR pictures (10–12 ms on the long-GOP one), against 6 ms for a P-picture's decode alone. The directive's 6–12 ms was not reached at 4K: the sync picture is the largest of its group and decodes slowest.
- Consecutive previews in one GOP show the same picture again (the backward half of the long-GOP clip shows its only sync sample, at 0 s, every time): the frame count measures responsiveness, not new information.
- The longest backward gaps are at the turnaround: a forward scrub seek already sent cannot be withdrawn, and when it is an exact one (a continuation of several references) the first backward preview waits for it.

**Verification.**

| Test | Checks |
|---|---|
| `media_playback_hevc10_4k_scrub_test` (`--expect-previews`) | The scrub test's checks, plus at least one preview and a frame in the backward half; the release exact; no preview left on screen |
| `media_playback_hevc10_4k_scrub_exact_test` (`--no-scrub-preview`) | The same drag with every scrub seek exact: no preview at all |
| `media_playback_hevc10_4k_longgop_scrub_test` (`--expect-previews`) | The single-GOP clip: the backward half previews the one sync sample |
| `media_playback_hud_test` | The scripted dock drag: a scrub seek on the move and the release's exact seek (or none when the scrub seek had already landed exactly); the landing presented, no preview left |
| `media_chapter_scene_tests` | `SyncSample`, `DecodeDistance` on an I P B B pattern (every picture, bounded scans, before the first picture, past the end) |

**Limits.**
- The cost bound counts samples, not reference pictures: a target after several non-reference B-pictures is previewed although its exact decode would be cheap.
- A preview of the sync sample already on screen decodes it again (about 15 ms at 4K): the result is the same picture.
- Forward drags are never previewed. A forward jump far ahead (a new group of pictures) pays its exact landing once, then continues; in a long-GOP 4K file that first landing can take well over 100 ms.
- A forward scrub seek already sent cannot be withdrawn: at the turnaround of a drag the first backward preview waits for it (the 150–160 ms gaps of the long-GOP rows).

## A–B loop (2026-10-21)

Directive: "SURGICAL A–B & SENTENCE LOOPING (AI TASK A37)". A stretch of the media plays again and again: for close inspection, language practice or an instrument.

**Controls.**

| Input | Effect |
|---|---|
| **L** | First press: A at the position. Second: B, and the loop [A, B] plays. Third: off |
| `set loop start`, `תחילת לופ`, `inicio del bucle`, `ループ開始`, … | A at the position (a loop already set waits for a new B) |
| `set loop end`, `סוף לופ`, `punto b`, … | B at the position, and the loop plays (`NoContext` without an A) |
| `stop the loop`, `בטל לופ`, `取消循环`, … | Off |
| `loop this sentence`, `repeat this line`, `לופ על המשפט`, `חזור על המשפט בלופ`, … | The cue on screen, or the one that ended within 3 s, padded 150 ms on both sides; playback goes to its start at once |

Commands in nine languages: [intents](intents.md#loop-and-telemetry-2026-10-21). Each change shows a status cue (`Loop · A at 0:12.4 · L again for B`, `Loop · 0:12.4 – 0:15.8`, `Loop · sentence 0:01.3 – 0:02.4`, `Loop off`). The dock draws the loop on its timeline: a mint bracket at A opening right and one at B opening left, with the stretch between them lit. A alone is its bracket only.

**How it turns.**
- `timeline::LoopRegion` (portable) holds the state: Off, Armed (A only), Looping.
  - B before A is taken the other way round.
  - A loop shorter than 200 ms is refused (`TooShort`): a double press, not a loop.
  - The sentence's padding stays inside the media.
- The ends are the clock's media time at the press (`PrecisePosition`), not the dock's position, which is sampled every 83 ms.
- On every pass of the UI loop, `PumpLoop` reads the clock. Once it reaches B, it sends a seek to A (`SendSeek`), and waits for that landing before it judges B again. Looping, playback never ends: reaching the end of the media turns back too.
- A seek outside the loop leaves it set; reaching B turns back as usual.
- A turn is an ordinary exact seek: audio restarts on A's sample, the landing picture is the one whose interval contains A, the clock re-anchors there.
  - Queue seeks and forward continuation are forward-only, so a turn back to A cannot use them. It restarts decoding from A's sync sample.
  - Its cost is that GOP's reference pictures: a few milliseconds on the short-GOP fixtures; on long-GOP 4K, the [catch-up](#seek-catch-up-2026-10-16) numbers apply.
- The bracket is `HudUniforms::loop` (`LoopRegion::Packed`: A and B as 16-bit positions along the track). Setting it reveals the dock.
- Each set, clear and turn is a flight recorder event (`LoopSet`, `LoopCleared`, `LoopWrap`); the telemetry panel shows the loop and its turns ([session](session.md#flight-recorder-telemetry-readout-and-watchdog-2026-10-21)).

**Measured** (`media_playback_ab_loop_test`, the 3 s H.264 fixture with its built-in captions, Release):

| Turn | Loop | B seen | Landed on | Turn's landing |
|---|---|---|---|---|
| 1–3 (L at 1.0 s, L at 2.0 s) | [1.0000 s, 2.0000 s] | 0.11–0.26 ms past B | 1.0000 s, exact | 2.70–3.25 ms |
| 4–6 (`loop this sentence` on cue 2, 1.4–2.2 s) | [1.2500 s, 2.3500 s] | 0.00 ms past B | 1.2500 s, exact | 3.65–4.08 ms |

Then `בטל לופ`: loop off, bracket gone. 0 pictures dropped, 0 allocations.

`media_ab_loop_tests` covers the rest:
- the model: the L key's three steps, either end, the sentence's padding at both ends of the media, the refusals, when a turn is due, the packing;
- 22 commands in nine languages, and that `repeat the line` and `חזור על המשפט` stay the replay they were;
- the bracket drawn offscreen: mint at A and B, the stretch lit, nothing else changed, no mint without a loop;
- 0 allocations.

**Limits.**
- A turn is a seek. On a long-GOP file the jump back shows that GOP's decode time as a held picture.
- Audio is not crossfaded at the turn.
- The loop is per file and is not remembered by the resume ledger.

## Pinned loop turnaround (2026-10-21)

Directive: "PINNED ZERO-LATENCY A–B LOOP TURNAROUND". A turn back to A used to be an ordinary seek. On a long-GOP file it waited for the decoder to rebuild the reference chain from the sync sample (69.9 ms on the 4K long-GOP fixture, A 24 pictures after the only sync sample). A loop's destination is known and fixed, so its first pictures and sound are kept, and every later turn starts from them.

**Pinned pictures** (`D3D12Renderer::ArmLoopPins`, `BeginPinnedEpoch`):
- **Capture.** While a loop is set, the pictures shown from the one holding A on are copied into pinned textures as they reach the screen. They must be consecutive and of one epoch; at most `kPins` (8), within 450 ms of A, ending at least 100 ms before B. The textures are made in Prepare, at the RGB slots' size: about 8 MB each for a 1080p desktop.
- **A turn.** The new epoch's queue gets copies of them at once, already converted, and the decoder is asked for where they end, not for A.
  - The copies run on a copy queue of their own, so a conversion in flight on the direct queue does not delay them. On one queue the landing waited behind a 4K conversion and took up to 6 ms.
  - The landing picture's copy is submitted alone, ahead of the others.
  - Only slots whose last direct-queue work is done are used; if fewer are free, fewer pins are queued, and decoding starts where they end.
- The first pin is the landing: on screen as soon as its copy is done. The others leave by the clock like any picture, and the decoded pictures follow from the end of the pins.

**Pinned sound** (the media worker):
- **Capture.** Told the loop's A (`ControlCommand::LoopAnchor`), the worker copies the endpoint frames it commits from the first one at A on: 500 ms, after the DSP and before the volume, while playing at 1× on one timeline and without a converter. It publishes the copy once whole.
- **A turn.** The turn's seek (`reserved[1]` bit 0, A in `reserved[2..3]`) repositions everything to the end of the pins as usual. Then:
  - the clock's origin is A;
  - the pinned PCM up to the end of the pins goes to the endpoint before any decoded PCM;
  - the decoded audio joins it there, as a track splice does: trimmed to that frame, without the seek's fade-in.
- **Fallback.** Without the pinned PCM (another track chosen since, another speed), the worker turns to A in the ordinary way.

**The UI** turns through the pins when they are ready, the speed is 1× and the worker's PCM covers them; else with an ordinary seek. The first turn of a loop is always ordinary: it is the pass that pins. Each turn is in the flight recorder (`LoopWrap`, `a` = 1 when pinned) and in the loop record (`LoopTurn::pinned`).

**Measured** (`media_playback_ab_loop_longgop_test`, HEVC Main10 4K at 24 fps, one sync sample; Release, UHD 630):

| Turn | Landing on A | Sound from A | Clock running after |
|---|---|---|---|
| First of the loop (ordinary seek) | 58.8–69.9 ms | ±0.02 ms | 87.5–107.1 ms |
| Later turns, through the pins | 1.1–4.2 ms (25 turns, 5 runs) | ±0.02 ms | 21.4–39.9 ms |

0 pictures dropped in every run. "Clock running" is the first precise anchor of the endpoint's clock, not when the sound starts: the pinned PCM is in the endpoint at once, and the clock waits only for the device's position to move.

The short fixture (`media_playback_ab_loop_test`, 64 × 64 H.264) is pinned the same way: 0.9–2.0 ms against 2.5–3.1 ms. Its first turns were already fast. The test now requires every later turn of a loop to be pinned and under 5 ms, and every turn's sound to start within one frame of A.

**Limits.**
- 8 pinned pictures take 66 MB of video memory on a 1080p desktop, from Prepare, whether a loop is ever set or not.
- The pins cover 450 ms at most. A GOP whose decode from its sync sample to there takes longer would make the first decoded pictures late. The 4K fixture decoded its 32 pictures in time; 60 fps 4K has half that window.
- Settings changed after the pins were made (quality tier, comparison) show on the pinned pictures only at the next pass. Zoom and pan go through the live-zoom transform as usual.

### Staged landings (2026-11-02)

Directive: "Harden `media_playback_ab_loop_test` pinned turnaround". The pinned turn missed its 5 ms budget about once a full Release suite (5.12 ms on 2026-10-21, 6.96 ms on 2026-11-01; each passed alone). Six direct runs on 2026-11-02 found one more, 6.83 ms. Every slow turn "waited for its conversion": for a pinned turn, its landing's copy on the copy queue. The copy is tiny, but the GPU ran it late behind its other work.

- **Staged** (`D3D12Renderer::StageLoopLanding`): as the clock comes within 150 ms of B (`kStageAhead`), the UI copies A's first pin into a free slot, on the copy queue. The slot is held out of the presentation queue, of no epoch, so a queue seek's adoption leaves it alone.
- **The turn** (`BeginPinnedEpoch`) takes that slot as its landing, with no copy to submit or wait for, and copies only the other pins.
- **Given back**: any other new timeline (`BeginEpoch`), the loop changing or being cleared, or a stale stage (another epoch, other pins) returns the slot to the pool.
- **Copy fences**: conversions now wait for a slot's copy fence as well as its direct-queue fence. A slot freed while a copy into it still ran could otherwise have been converted into at the same time; that was possible before for pinned slots flushed by a seek.

| Measured (Release, UHD 630) | Before (6 runs, 2026-11-02) | Staged (4 runs) |
|---|---|---|
| `media_playback_ab_loop_test`, pinned turns | 0.99–6.83 ms, several "waited for its conversion"; 1 run failed | 0.19–3.76 ms, 20 of 20 "waited for nothing", 20 of 20 staged; 4 of 4 passed |
| `media_playback_ab_loop_longgop_test` (4K HEVC Main10), pinned turns | 1.1–4.2 ms (2026-10-21) | 0.28–2.26 ms, 10 of 10 staged |

The test now also requires every pinned turn to have taken a staged landing (`StagedTurns() == PinnedTurns()`).
