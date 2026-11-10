# AVION 0.1 — Reddit posts

Rendered by `tools/site/build_site.py` from the measured head-to-head and the showcase; rerun it rather than editing figures. Three posts, each written for its own subreddit's question. Upload `avion-0.1-demo.mp4` (16 s, 1080p60, 6 MB) as the post's video where the subreddit allows it, otherwise link {{SITE_URL}}. Before posting, read each subreddit's current self-promotion rule and post at most one a day. Every post says up front that the source is open (AGPLv3, with a commercial licence for closed products) and that playback is Windows-only, because those are the first things each of these communities asks. Answer the first questions in the comments yourself.

---

## r/cpp

**Title**

Zero heap allocations in steady state, counted: design notes from a C++20 media and perception engine behind a C99 header

**Flair:** Discussion (or "Show and tell" if the subreddit offers it)

**Body**

I have spent the last months on a Windows media and perception engine (C++20 inside, one C99 header outside) with one rule: after it is primed, nothing allocates. I would like the design and the way I measure it picked apart. Disclosure: it is mine, and the full source is on GitHub under the AGPLv3 (with a commercial licence for closed-source use): {{REPO_URL}}. The arena and the ring are in include/media/core/, so you can check every claim below against the code.

**The rule, and how it is enforced**
- Memory is committed at start-up from arenas backed by `VirtualAlloc` with eager page touching. Before the engine goes live the arenas are sealed; a later request hard-fails instead of falling back to the heap. No `malloc`, `new`, `std::string`, `std::vector::push_back` or `std::map` in steady state.
- Stages talk through single-producer single-consumer rings, acquire/release only, cursors on their own 64-byte lines. No mutexes or condition variables in the streaming paths. Queues carry 64-byte descriptors, never pixels; decoded video stays in VRAM.
- Three things bit me that may bite you: an SPSC ring that moves raw bytes needs a trivial element type (enforced with `static_assert`), `snprintf` can allocate on some paths, and a GCC 13 internal compiler error on an aggregate holding an array of structs with default member initialisers.

**How it is measured**
One tiny malloc-counting library is preloaded into every process on Linux, so an allocation counts the same in the engine, ffmpeg and OpenCV, on one machine with the same inputs. Per 1,000 frames: the engine 0; ffmpeg {{S['machine']['linux']['ffmpeg'].split('-')[0]}} decode + scdet on 4K HEVC {{n(LG['ffmpeg_allocs_per_1000'], 0)}}; OpenCV {{S['machine']['linux']['opencv']}} DIS flow {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}}. The counter's source, the raw lines and the scripts are in the repository (tools/bench) and the evaluation bundle. Different jobs, so read the numbers as "what does a steady state cost in allocator calls", not as a race.

**Verification**
{{SU['release']['passed']}}/{{SU['release']['total']}} suites on MSVC x64 `/W4 /WX` in Release and {{SU['debug']['passed']}}/{{SU['debug']['total']}} in Debug (, because a quiet machine proves little), {{SU['gcc']['passed']}}/{{SU['gcc']['total']}} on GCC 13 with ASan/UBSan for the portable core. No third-party multimedia code: Win32, COM, Direct3D 11/12, DXGI, WASAPI.

**What I would like to hear:** where an arena sealed after priming is the wrong model (device loss and format changes drain back to a prepare state here), and whether a counter at the allocator is the right bar or I should also count page faults and syscalls.

Source: {{REPO_URL}} · site, method and raw data: {{SITE_URL}}

---

## r/robotics

**Title**

A virtual IMU from pixels: camera shake, horizon and time to contact on the GPU with no gyro (what works, what does not)

**Flair:** Software / Discussion

**Body**

Many small robots and drones have a camera and a poor or no IMU. I built a perception kernel that reads the camera's own motion from the background of the picture and uses it for the things a robot wants from video. Disclosure: it is mine; the source is open (AGPLv3, or a commercial licence for closed products and devices: {{REPO_URL}}), and the GPU path is Windows-only today. I am after feedback from people who would use the idea.

**What it does, against a truth we set**
- Virtual IMU: the shake recovered from pixels alone is {{n(SC['pulse']['imu_rms'], 3)}} px rms off the true shake in the showcase (0.068 px in the test suite). On the reel's clip the camera shakes 5.91 px rms on screen and 0.74 px after stabilisation; on the suite's 480×272 clip, 14.73 to 0.92.
- Auto-horizon: a harbour tilted {{n(SC['perceive']['tilt_deg'], 0)}}° reads {{'%+.2f' % SC['perceive']['horizon_deg']}}°; on synthetic tilts it is within 0.06°, on real footage within 0.38°.
- Time to contact: a buoy closing at 1 m/s is read as {{n(SC['perceive']['ttc'], 2)}} s for a true {{n(SC['perceive']['ttc_truth'], 2)}} s, with track ids kept through crossings; it flags a synthetic approach on 55 of 55 frames.
- An event-camera stream emulated from ordinary frames: a shaken clip's {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M events become {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M once the camera's own motion is removed: what is left is what moves.
- A remote pulse through the shake (a synthetic 72 BPM about one code deep in the colour of the skin): {{n(SC['pulse']['bpm'], 2)}} BPM. That is a signal check, not a medical claim.

**Honest limits**
- It is a front end for your estimator, not a VIO or SLAM replacement. Stabilisation is one translation and one rotation per frame: no rolling-shutter or parallax correction.
- The horizon needs level lines. On *Big Buck Bunny*'s meadow, tilted {{n(SC['perceive']['meadow_tilt'], 0)}}°, it reads {{'%+.2f' % SC['perceive']['meadow_deg']}}°, so the reel uses a generated harbour. The truths are ones I set, not a drone dataset yet.
- Windows x64 and Direct3D today; the portable core and the host-mode C test pass on Linux, but the GPU path does not run there. Nothing allocates once it is running, which is why I think it fits a control loop: 0 heap allocations per 1,000 frames.

**What I would like to hear:** which real sequences you would test it on first (EuRoC, TUM-VI, your own flight logs with a gyro as the truth?), and which of these outputs you would actually feed to a controller.

16-second reel, method and raw data: {{SITE_URL}}

---

## r/computervision

**Title**

Optical flow 8.5× faster than OpenCV DIS on one core, and 0 allocations per 1,000 frames: how I measured it, and what the comparison does not show

**Flair:** Showcase / Discussion

**Body**

I wrote a GPU and CPU perception kernel (Windows x64 for the GPU path; the source, the benchmark's scripts and its raw data are on GitHub under the AGPLv3, with a commercial licence for closed-source use: {{REPO_URL}}). The numbers below look flattering, so here is exactly what each one compares, and I would like you to tell me where the comparison is unfair.

**The numbers**
- Motion at 640×480, one core: {{n(FL['avion_linux_ms'], 2)}} ms against {{n(FL['opencv_dis_ultrafast_1t_ms'], 2)}} ms for OpenCV {{S['machine']['linux']['opencv']}} DIS ultrafast ({{times(FL['avion_linux_ms'], FL['opencv_dis_ultrafast_1t_ms'], 1)}}). **What it does not show:** mine works on 8×8-sample tiles and also produces time to contact, saliency and objects, so it is not a dense per-pixel flow field; for dense flow DIS is the right tool.
- Structure-tensor survey, 640×480: {{n(SV['avion_linux_avx2_ms'], 3)}} ms against OpenCV's {{n(SV['opencv_1t_ms'], 3)}} ms ({{times(SV['avion_linux_avx2_ms'], SV['opencv_1t_ms'], 1)}}), Scharr plus block means in both, mine in AVX2 integer arithmetic.
- Heap allocations per 1,000 frames: 0 against {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}} for DIS (outputs reused) and {{n(SV['opencv_1t_allocs_per_1000'], 0)}} for OpenCV's survey, counted by one preloaded malloc counter in every process.
- Scene cuts from H.264/HEVC slice headers: {{n(LG['avion_linux_us'], 2)}} µs of CPU a 4K frame against {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms for ffmpeg to decode and score it. **What it does not show:** it is not a decoder race; it reads headers and never sees pixels. Against scdet on one 4K HDR reel it finds {{LG['cuts_matched']}} of {{len(LG['scdet_cuts'])}} cuts with {{len(LG['cuts_extra'])}} extra (checked by eye); two files are a sanity check, not a recall figure.

**Things in the showcase you can check yourself**
- Stabilisation by the engine's virtual IMU: a shaken clip fires {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M emulated events and {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M after the camera's motion is removed. The event stream is an emulation from frames, not a DVS sensor.
- Remote pulse from skin colour (rPPG) through shake: {{n(SC['pulse']['bpm'], 2)}} BPM for a synthetic 72, {{n(SC['pulse']['chroma_bpm'], 2)}} from Cr chroma alone. The pulse is one I added to the footage; it proves the signal chain, not clinical accuracy.
- Every picture is a snapshot the engine wrote through its own SDK, on a truth I set (an exposure, a tilt, a speed, a shake, a pulse).

**Where it loses:** for plain playback VLC uses less CPU than I do at 720p ({{n(PL['vlc']['playing_pct'], 2)}} % against {{n(PL['avion_pure']['playing_pct'], 2)}} %); the table is on the site next to the wins.

**What I would like to hear:** whether DIS ultrafast with a tile-sampled readout is the fair baseline or I should compare against something else (Farnebäck on a downscaled picture? a RAFT-small on the GPU?), and which real datasets with ground-truth motion I should run next.

Method, raw lines, 16-second reel: {{SITE_URL}}
