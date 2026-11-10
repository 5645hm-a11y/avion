# Real-world fixtures

`assets/test_fixtures/` holds media the engine did not generate itself. The `.mp4` files (about 3 GB) are not in version control (`.gitignore`); the `.srt` sidecars are. Every test below skips (exit 77) when its file is absent.

## What the files are (audit of 2026-09-30, 16:05)

Read by `media_fixture_audit` through the production file source and demuxer. Three of the files are HEVC re-encodes made outside the engine that afternoon, replacing AV1 downloads of the same names; the names still promise more than some files hold.

| File | Video | Audio | Plays |
| --- | --- | --- | --- |
| `big_buck_bunny_4k60.mp4` (1367 MiB, 10 min 35 s) | HEVC Main, level 5.1, 3840x2160, 8-bit 4:2:0, 60 fps, BT.709 | AAC-LC, 44.1 kHz, stereo | yes |
| `lg_black_4k60_hdr.mp4` (357 MiB, 2 min 7 s) | HEVC Main 10, level 5.1, 3840x2160, 10-bit 4:2:0, 59.94 fps, BT.2020 PQ, MaxCLL 839, MaxFALL 39, mastering display present | AAC-LC, 44.1 kHz, stereo | yes (it dropped pictures at the start until the fix below) |
| `dolby_surround_51_test.mp4` (18.7 MiB, 1 min 4 s) | HEVC Main, level 4.0, 1920x1080, 8-bit 4:2:0, 24 fps, BT.709 | AAC-LC, 44.1 kHz, **stereo** (not 5.1) | yes |
| `av_sync_clock_45m_converted.mp4` (352 MiB, 11 min 3 s) | HEVC Main, level 5.1, 3840x2160, 8-bit 4:2:0, 60 fps, BT.709 (the first 11 minutes of the file below) | AAC-LC, 44.1 kHz, stereo | yes |
| `costa_rica_4k60_hdr.mp4` (95 MiB, 5 min 14 s) | H.264 Main, level 3.2, **1280x720**, 8-bit 4:2:0, 59.94 fps, **BT.709 SDR** (with a stray `clli`: MaxCLL 1100, MaxFALL 180) | AAC-LC, 44.1 kHz, stereo | yes |
| `av_sync_clock_45m.mp4` (820 MiB, 44 min 27 s) | AV1 Main, level 5.1, 3840x2160, 8-bit 4:2:0, 60 fps, BT.709 | AAC-LC, 44.1 kHz, stereo | no |

- **AV1.** The original downloads were AV1 in four of five cases; one is left. The engine has no AV1 decoder frontend, and the Intel UHD 630 of the reference machine has no D3D12 AV1 decode profile (it reports H.264, HEVC Main, HEVC Main10 and VP9). AV1 is refused at preparation stage 4. A remux does not help; the replacements are re-encodes, new streams rather than the original bits.
- **No Opus, no VP9, no 5.1.** Every audio track is AAC-LC with `channelConfiguration` 2, decoded as stereo (mask 0x003). The re-encode of the "5.1" file kept its stereo track.
- **Containers.** All are classic MP4 with one rate-1 edit per track and an audio `roll` sample group (distance -1). The sample index takes 9 to 16 bytes per sample: 2.97 MiB of the 48 MiB pool for the 45-minute file (274,930 samples).
- **Subtitles.** `big_buck_bunny.srt` has 13 cues in Hebrew with English, Arabic and Chinese lines and two emoji; `big_buck_bunny_en.srt` has 87 English sound-description cues (UTF-8 with a byte-order mark). Both belong to `big_buck_bunny_4k60.mp4`, and the subtitle test now uses that pairing.

## What the files found in the engine

Before 2026-09-30 none of the original five opened.

| Found | Change |
| --- | --- |
| Every file's AAC track carries `sgpd`/`sbgp` roll recovery, which the demuxer rejected | Admitted on sound tracks ([Layer 1](layer1.md)) |
| 44.1 kHz audio failed on a 48 kHz endpoint (stage 9, `AUDCLNT_E_UNSUPPORTED_FORMAT`) | Windows' shared-mode converter is used when the rates differ; the stream and clock stay in programme frames |
| A landing image after a long seek had no caption | Glyphs are requested for the seek target while it lands ([subtitles](subtitles.md)) |
| Seeks took 0.27 to 1.0 s on the 720p file | Non-reference pictures skipped, file blocks shared: 60 to 195 ms ([seeking](seeking.md#landing-on-long-gop-files)) |
| 4K Main10 PQ at 59.94 fps drops pictures in its first 0.3 s, and now and then later | Two causes in the presentation queue, both removed; see below |
| The same file still dropped a picture in ~4 % of cold starts (2026-11-02) | The governor kept the expensive tier while the queue was thin; a thin-queue rule (2026-11-03); see [cold starts](#cold-starts-the-thin-queue-2026-11-03) |

## Start-up of `lg_black_4k60_hdr.mp4`: found and fixed

**Symptom** (first recorded 2026-09-30 afternoon: 6 of 15 three-second runs). One to five of the first 18 pictures were each one frame interval late and dropped. Reproduced the same evening in 5 of 12 and 10 of 12 runs (Release, Intel UHD 630); two of those runs dropped pictures later instead (the last at 1.57 s and at 2.72 s).

**What the start-up trace showed** (a temporary event log in fixed storage, written by both threads; since removed):

| Suspect | Finding |
| --- | --- |
| The clock on a converting endpoint | Anchored 20 ms after the endpoint started; every later observation within ±0.04 ms of the prediction. Not it. |
| Decode or conversion throughput | Every dropped picture had been converted 60 to 190 ms before its deadline and became due on time. Not it, as the earlier hold-the-clock experiments had suggested. |
| Pipeline or PSO warm-up | Nothing was compiled or allocated in the window; the first picture was on screen 40 ms after its decode. Not it. |
| Presentation queue | **Every late picture waited for the swap chain's frame-latency object**, only in the first 280 ms, and only while the D3D12 queue had 5 to 8 submissions outstanding (0 to 2 afterwards). Runs that passed had pictures 11 to 15 ms late in the same window. |

**Cause 1: conversions waiting for the decoder in the queue the flips use.** At start-up the worker decodes as fast as it can and the renderer converts each picture as it arrives, to fill its 16 slots. Each conversion was enqueued behind a GPU-side wait for its picture's decode. Flips enqueued after them waited too, so the compositor received nothing, the frame-latency object stayed unsignalled, and due pictures aged past one interval. Later in the file the same thing happened whenever one decode took longer than a frame interval: for this stream only one decoded picture waits beyond the 16 converted ones, so each conversion is enqueued about one interval after its decode was submitted.
- Fix: a picture is converted only after its decode fence has completed, and one conversion is in flight at a time.

**Cause 2: controls-only images between the pictures.** With cause 1 removed, one start in 20 to 25 still dropped a picture, now between 1.5 and 1.9 s, where this file fades in from black (its pictures are under 1 KiB until 1.2 s) and decoding becomes real work; the wait was frame latency again, or "nothing" with the presentation thread absent for 12.7 ms. Timing the thread's calls showed single `Present` calls of 8 to 15.7 ms (one 60 Hz refresh is 16.7 ms) in 11 of 25 runs. While the dock is visible (the first 2.5 s) the renderer was also presenting controls-only images at 120 Hz, which on a 60 Hz display filled the swap chain's queue.
- Fix: no controls-only image when a picture is due within one display refresh.

**The pre-roll contract.** The clock now starts only after the first picture has been decoded, tone-mapped and handed to the swap chain (it used to start once that picture's conversion had completed). A longer pre-roll, holding the clock until the pipeline was full and idle, was built first: it stopped the start-up drops (0 of 15) but started the clock 100 to 130 ms later at every start and after every seek, and was removed once the two causes were fixed (0 of 20 without it). [Seeking](seeking.md#pre-roll-and-the-presentation-queue-2026-09-30) states the three rules.

**After** (Release unless said, same machine):

| Measure | Before | After |
| --- | --- | --- |
| Cold starts with a dropped picture | 5 of 12, 10 of 12, 6 of 15 | 0 of 29 undisturbed starts; 0 of 10 and 0 of 10 in the CTest cold-start test (Release, Debug) |
| Latest flip in a run, against its deadline | 11 to 15 ms in runs that passed | 0.01 to 0.69 ms in the ten Release starts; median 0.28 ms over 30 |
| Longest `Present` call | 8 to 15.7 ms in 11 of 25 runs | under 2 ms in 26 of 30 runs |
| Clock start | about 125 ms after the first decode was submitted (trace) | 62 to 78 ms after `Run` (first picture at 26 to 37 ms) |
| Pictures shown in 3 s | 173 to 175 when nothing dropped | 176 (60.0 per second of clock) |

Side effects measured the same evening: `media_playback_hevc10_4k_http_seek_test` in Debug, which failed 3 of 20 runs before, passed 20 of 20; the full Debug suite passed at the first attempt (one of five before).

**Not explained, and what remains:**
- One of the ten Debug cold starts flipped a picture 16.5 ms late (not dropped; the limit is 16.68 ms), waiting for frame latency at 2.04 s. Three undisturbed Release starts of 30 still had one `Present` call of 9 to 15 ms; the latest picture in those runs was 3.1 ms late.
- **The presentation thread is the window's thread.** One start in 30 dropped two pictures while a single window-message dispatch took 57.7 ms (0.1 to 0.9 ms in the others); another run was seeked by a real click on the dock. Both happened while someone was using the machine during the tests. A message that takes longer than a frame interval makes a 60 fps picture late; moving presentation off the window's thread is the structural answer and is not done.
- The display's refresh period is the compositor's (`DwmGetCompositionTimingInfo`), not the rate of the monitor the window is on. `EnumDisplaySettingsW` would give that, but called while playing it stalled the presentation thread for 170 ms (it failed `media_playback_resize_test` until it was replaced).
- With 60 fps video on a 60 Hz display the dock, captions and command bar now update with the pictures, not at 120 Hz.
- Only this machine (Intel UHD 630, 60 Hz displays) was measured.

## Cold starts: the thin queue (2026-11-03)

**Symptom.** After the fixes above, `media_fixture_lg_hdr_cold_start_test` still failed in about half of its 20-start invocations in full suites (about 4 % of single starts, 2026-11-02; 8–25 % on 2026-10-21 and 10-27). In this session, on a quiet machine: 2 of 100 direct starts dropped (3 and 10 pictures, the last at 0.22 s and 0.42 s, each "missed its deadline; waited for its conversion", the worst conversion 16 ms on the GPU), then 0 of 300.

**The start-up trace.** `D3D12Renderer` now records the session's first 96 conversions and pictures in fixed arrays: when each picture reached `ConvertFrame`, when its decode was first seen complete, when its conversion was queued and read, its GPU time, its tier and the slots busy then; and each timed picture's deadline, flip or drop and what it waited for. The demo prints the trace after a run that dropped a picture, or with `MEDIA_STARTUP_TRACE=1`. A passing start, read with it:

- every conversion waits for its decode (6 to 20 ms after the picture reaches the renderer), and the decode→convert pipeline runs only ~1.3x real time at the start;
- at the Edge-directed tier, where the quality governor starts every session, the 4K PQ conversions take 6.6–12.8 ms (every fourth, the reference picture's, ~12 ms) and share the GPU's memory bandwidth with the decoder;
- so the queue of converted pictures ahead of the clock grows by about one picture in four: 2 at 36 ms, 6 at 0.25 s, 15 (full) only at 0.73 s. The tightest picture was ready 6 ms before its deadline; at 0.22 s and 0.42 s, where the failing starts dropped, the slack was 48 and 98 ms.

**The cause.** For most of a second the queue is too thin to absorb anything: a stall of 50–200 ms in that window (the failing starts' worst conversions show the GPU slowing) drops pictures "waiting for their conversion". The governor's step-down rules (a p95 over 20 pictures above 85 % of the interval, or four pictures over the whole interval) assume a full queue. Here Edge-directed's p95 stays under its limit (~12 ms of 16.7), so the governor kept the expensive tier through the whole thin window and left it only later in the run.

**What it is not** (measured): a decode stall alone (the worker stopped 60, 90, 120 or 150 ms at 0.2 s: 0 drops in 32 starts, either tier, because queued decodes keep the GPU busy); a moderate GPU load (another player on a 1080p24 HEVC file: 0 of 15 starts dropped either way); a 4K60 load saturates the iGPU outright (95–158 drops every start, either way). The external hiccup that empties the thin queue was not reproduced on demand; its mechanism was.

**The fix: a thin-queue rule in the governor.** While the pictures converted ahead of presentation hold less than 100 ms of media (`GovernorOptions::thin_queue`; the renderer passes the reorder window's pictures x their interval to `Observe`), one picture over half the frame interval steps down at once, as an overload: its cost is remembered and the usual back-off applies. Edge-directed returns once the queue is full, its back-off has passed and Fast has shown the headroom. Content whose conversions are cheap (HD) never trips it. The demo's `Quality:` line counts these steps ("of them N on a thin queue").

| Measured (Release, UHD 630) | Before | After |
|---|---|---|
| the tier during the start | Edge-directed until the governor's p95 rule, late in the run | Fast from the sixth conversion (12.41 ms on a thin queue), Edge-directed again at ~2 s with the queue full (the run ends there, p95 11.56 ms) |
| GPU per conversion while the queue fills | 6.6–12.8 ms | 4.7–6.3 ms |
| the queue full (15 pictures ahead) | at 0.73 s | at 0.34 s |
| direct cold starts that dropped a picture | 2 of 400 | 0 of 200 |
| `media_fixture_lg_hdr_cold_start_test` (20 starts each) | about half its invocations failed in suites | passed in every invocation since: 5 alone (100 starts) and in 5 full suites (Release and Debug) |

## Tests

| Test | What it does |
| --- | --- |
| `media_fixture_audit_test` | `media_fixture_audit assets/test_fixtures`: for each `.mp4`, container and codec description, index size, 60 video packets and 2 s of audio demuxed, the audio decoded by `Win32AacDecoder`, two seeks by table lookup landing on sync samples, no allocation after sealing, every block lease returned. For each `.srt`, parse, index, and every cue found by its first line. An unsupported video codec is reported, not failed. A file another process is writing is skipped. |
| `media_fixture_playback_<name>_test` (four: `costa_rica_4k60_hdr`, `big_buck_bunny_4k60`, `dolby_surround_51_test`, `av_sync_clock_45m_converted`) | `media_player_demo <file> --test-fixture`: three seconds, closed with Escape; at least 60 pictures and 2 s of audio, no drop, no allocation. |
| `media_fixture_lg_hdr_playback_test` | `media_player_demo lg_black_4k60_hdr.mp4 --test-fixture-hdr`: the fixture test for a 4K 10-bit HDR file at 50 fps or more. Every picture tone-mapped, at least 150 pictures, no drop, no allocation, and the last picture shown is exactly as many frame intervals in as pictures were shown (none was skipped, from the first picture on). |
| `media_fixture_lg_hdr_cold_start_test` | The same in twenty new processes in a row (`tests/repeat_command.cmake`); the first run that drops a picture fails the test. Each run also checks that the clock started only after the first picture was presented (2026-10-22): the first Convert + Present pair (a cold driver's first conversion, the swap chain's first flip) is absorbed by the pre-roll, before the strict count begins. |
| `media_fixture_subtitle_search_test` | `--test-fixture-search --subtitles big_buck_bunny.srt` on `big_buck_bunny_4k60.mp4`: four quotes (Hebrew and English as spoken-style commands, Chinese and Arabic typed into the command bar) each landing on its cue's frame with the cue composited. Landings take 160 to 350 ms at 4K. |
| `media_fixture_pacing_24_test`, `media_fixture_pacing_60_test` | `--test-pacing` on `dolby_surround_51_test.mp4` (1080p24 HEVC) and `costa_rica_4k60_hdr.mp4` (720p59.94 H.264): play to 6 s, pause 4 s, play to 14 s; no drop, and the presentation thread and the worker each under 20 % of a core playing and 5 % paused ([low-power pacing](session.md#low-power-pacing-2026-10-22)). |
| `media_real_perception_tests` | `costa_rica_4k60_hdr.mp4` decoded by Media Foundation (this test only): perception on the footage, with a shake added and with an obstacle pasted on, and the sound locator on bursts in a reverberant room mixed into its soundtrack ([picture](picture.md#perception-on-real-footage-2026-10-22), [audio](audio-dsp.md#reverberant-rooms-and-real-ambience-2026-10-22)). Since 2026-10-27 it also requires the beach (the ground the camera travels over) to loom on at most 20 pictures (2; it was 91) with the ground found on at least half, and the obstacle found from 2 to 2.8 s of contact on at least 80 % of the pictures where it is the strongest approach (48 of 48; it was 0 of 15), its time to contact within 15 % ([the fovea and the ground](picture.md#the-fovea-and-the-ground-2026-10-27)). Since 2026-10-29 ([the predicted search, the surf and the low horizon](picture.md#the-predicted-search-the-surf-and-the-low-horizon-2026-10-29)): its windows with the horizon low, high and pitching loom on at most 8 pictures each (0, 0, 0; they were 10, 31, 13) with the ground found on at least 200, half and 150 (292, 274, 188), the footage itself on at most 8 (0), the obstacle found from 2 to 2.8 s on at least 45 of 46 (48 of 48), and window E, 640x480 of the footage panning 120 px/s with the IMU, gives the same results at every SIMD level (600 comparisons, 0 differing) in at most 0.6 ms a picture in optimised builds (0.51 with AVX2). `--verbose` prints each picture's approach (fovea or not, the ground, the windows on it), `--flows N` the footage's tile flow at picture N. |
| `media_fixture_refused_av_sync_clock_45m_test` | The AV1 file is refused at stage 4. To be replaced when an AV1 frontend exists. |

## Getting playable versions

- YouTube serves H.264 (`avc1`) only up to 1080p, and HDR only as VP9 Profile 2 or AV1; it serves no HEVC. A 4K or HDR download from there is never playable by this engine as downloaded.
- For H.264 up to 1080p from the same sources: `yt-dlp -f "bv*[vcodec^=avc1]+ba[acodec^=mp4a]" --merge-output-format mp4 <url>`.
- For 4K, HDR and 5.1: use sources that publish HEVC or H.264 masters, or re-encode offline as was done here. A re-encode is a new stream, not the original bits, and its audio is whatever the download had (stereo here).
- The repository's own generated fixtures (`media_make_playback_fixture`) remain the bit-exact references for 4K HEVC Main10, PQ, HLG and 5.1 AAC.
