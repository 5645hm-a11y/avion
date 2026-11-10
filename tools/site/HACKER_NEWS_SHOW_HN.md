# AVION 0.1 — Hacker News "Show HN"

Rendered by `tools/site/build_site.py` from the measured head-to-head and the showcase; rerun it rather than editing figures. Submit the title and the URL, then post the first comment yourself within a minute (Hacker News shows no text for a link post, so the comment is the pitch). Plain text only: Hacker News has no Markdown, so paragraphs are separated by blank lines and the figures are inline. Be there for the first two hours.

---

**Title** (80 characters at most)

Show HN: AVION – zero-allocation perception and media kernel, C++20 (AGPLv3)

**URL**

{{REPO_URL}}

**First comment**

Hi HN, I'm the author. AVION is a perception, vision and media kernel behind one C99 header. You hand it video (a file, a camera, frames from your own decoder, or a texture) and it gives you what a machine needs from it: the camera's shake read from pixels with no gyro, an auto-horizon, objects with track ids and time to contact, an emulated event-camera stream, a remote pulse, scene cuts read from H.264/HEVC slice headers, and NCHW tensors. It runs on the GPU and allocates nothing once it is playing.

The 16-second reel at the top of the page is the engine's own output on the showcase clips (Big Buck Bunny, CC BY 3.0, plus a generated harbour where the truth is known), not a mock-up.

The whole source is in the repo: the C++20 engine, the HLSL shaders, the C99 SDK, about 240 CTest suites and the benchmark scripts. It is dual-licensed: GNU AGPLv3, or a commercial licence for closed-source products, devices and services. It builds with CMake and MSVC (Windows x64, /W4 /WX); the portable core and host mode also build with GCC on Linux. Playback is Windows x64 (Direct3D 11/12, DXGI, WASAPI), with no third-party media code: no ffmpeg, no libav, no SDL, no Qt. Good places to start reading: docs/architecture.md, the arena (include/media/core/linear_memory_arena.hpp) and the SPSC ring (include/media/core/spsc_ring_buffer.hpp). It is not a model: there are no weights, it is the front end a model sits behind. The site with the charts and the reel: {{SITE_URL}}

The claim I would most like checked is the allocation count. One tiny malloc-counting library is preloaded into every process, so an allocation counts the same in AVION, ffmpeg and OpenCV, on one machine with the same files. Heap allocations per 1,000 frames: AVION 0, ffmpeg {{S['machine']['linux']['ffmpeg'].split('-')[0]}} decode + scdet on 4K HEVC {{n(LG['ffmpeg_allocs_per_1000'], 0)}}, OpenCV {{S['machine']['linux']['opencv']}} DIS flow {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}}. In the engine this is a design rule, not a measurement: memory is committed up front in arenas that are sealed before playback, a later request hard-fails, and the stages talk through single-producer single-consumer rings with no locks.

Some numbers, all on one i7-10700 with a UHD 630:
- Cuts in 4K HEVC from slice headers: {{n(LG['avion_linux_us'], 2)}} µs of CPU per frame, against {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms for ffmpeg to decode and score it ({{times(LG['avion_linux_us'], LG['ffmpeg_linux_software_1t_cpu_us'])}}). Different jobs by design: it reads headers, it does not decode pixels. Against scdet on LG's 4K HDR reel it finds {{LG['cuts_matched']}} of {{len(LG['scdet_cuts'])}} cuts, with {{len(LG['cuts_extra'])}} extra I checked by eye.
- Motion at 640×480 on one core: {{n(FL['avion_linux_ms'], 2)}} ms against {{n(FL['opencv_dis_ultrafast_1t_ms'], 2)}} ms for OpenCV DIS ultrafast ({{times(FL['avion_linux_ms'], FL['opencv_dis_ultrafast_1t_ms'], 1)}}). Mine works on 8×8 tiles and also yields time to contact and objects, so it is not the same output as per-pixel DIS.
- On the reel's clip a shaking camera reads 5.91 px rms on screen and 0.74 px after the stabiliser (14.73 → 0.92 on the suite's 480×272 clip). A shaken clip fires {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M emulated events and {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M once its own motion is removed.
- Host-driven hardware decode (API 14): your thread steps the engine, the GPU decodes into shared D3D11 or D3D12 textures you render yourself, PQ10 or scRGB for HDR. 0.7 % of one core for H.264, 6.8 % for 4K HDR10 HEVC (the host read a peak of 5,849 cd/m²), 0 allocations.

Where it loses or is weak, and the site says so next to the wins: VLC plays 720p with {{n(PL['vlc']['playing_pct'], 2)}} % of a core and AVION with {{n(PL['avion_pure']['playing_pct'], 2)}} %. Paused, AVION is at {{n(PL['avion_pure']['paused_pct'], 2)}} %. The benchmark found two of my bugs (false cuts on a hierarchical GOP, CPU while paused) and both are fixed in the engine. Stabilisation is one translation and one rotation per frame (no rolling shutter or parallax). The horizon needs level lines: on Big Buck Bunny's meadow, tilted {{n(SC['perceive']['meadow_tilt'], 0)}}°, it reads {{'%+.2f' % SC['perceive']['meadow_deg']}}°, which is why the harbour in the reel is generated. The pulse is a synthetic 72 BPM about one code deep in the skin's colour; it is a signal-processing check, not a medical claim. MP4/MOV and MKV/WebM with H.264 or HEVC on the GPU, AV1 through Windows' own decoder where the GPU has none, AAC, Opus or PCM; VP9 is refused.

Verified: {{SU['release']['passed']}}/{{SU['release']['total']}} MSVC suites in Release and {{SU['debug']['passed']}}/{{SU['debug']['total']}} in Debug (/W4 /WX), {{SU['gcc']['passed']}}/{{SU['gcc']['total']}} GCC with ASan/UBSan. The Python wheel and the .NET 8 NuGet package are installed offline from the bundle in their own tests.

Happy to go into the arena and ring design, the counting method, the horizon and time-to-contact estimators, or what I would test next on real footage.
