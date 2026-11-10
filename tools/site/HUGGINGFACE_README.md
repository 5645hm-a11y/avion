---
library_name: avion-c99-sdk
license: agpl-3.0
tags:
- robotics
- computer-vision
- optical-flow
- neuromorphic
- event-camera
- rppg
- video-stabilization
- time-to-contact
- zero-allocation
- c99
- direct3d12
- edge-ai
---

# AVION 0.1

**A deterministic perception, vision and media kernel behind one C99 header.** It stabilises, levels, tracks, measures time to contact, reads a pulse and emits neuromorphic events from ordinary video, on the GPU, and allocates nothing once it runs. For drones, robots, vehicles and smart cameras, and for the AI pipelines and players behind them.

This is not a model: there are no weights to download here. AVION is the front end a model sits behind. It hands a network a foveated NCHW tensor (float32 or float16) with no copy, and it filters what reaches the network: cuts read from the bitstream before decoding, an event stream with the camera's own motion removed, objects with track ids and time to contact.

[Source on GitHub]({{REPO_URL}}) (AGPLv3 or commercial) · [Site and charts]({{SITE_URL}}) · [Whitepaper]({{SITE_URL}}whitepaper.html) · [Evaluation bundle]({{SITE_URL}}downloads/AVION-0.1-Evaluation.zip) ({{n(M['archive']['bytes'] / 1048576, 2)}} MiB, SHA-256 `{{M['archive']['sha256']}}`)

![The event camera, unstabilised against stabilised by AVION's virtual IMU]({{SITE_URL}}assets/showcase/plate-events.png)

▶ [16-second demo reel, 1080p60, the engine's own output]({{SITE_URL}}assets/avion-0.1-demo.mp4)

## Measured head to head

One machine (Intel Core i7-10700, UHD 630), same inputs, and one malloc-counting library preloaded into every process, so an allocation counts the same in AVION, ffmpeg and OpenCV. Lower is better.

| Measure | AVION 0.1 | Baseline | Baseline tool |
|---|---:|---:|---|
| Heap allocations per 1,000 frames, 4K HEVC scan | **0** | {{n(LG['ffmpeg_allocs_per_1000'], 0)}} | ffmpeg {{S['machine']['linux']['ffmpeg'].split('-')[0]}}, decode + scdet |
| Heap allocations per 1,000 frames, motion | **0** | {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}} | OpenCV {{S['machine']['linux']['opencv']}} DIS ultrafast |
| Heap allocations per 1,000 frames, survey | **0** | {{n(SV['opencv_1t_allocs_per_1000'], 0)}} | OpenCV Scharr + block means |
| CPU per 4K HEVC frame to find cuts | **{{n(LG['avion_linux_us'], 2)}} µs** | {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms | ffmpeg decode + scdet, one thread ({{times(LG['avion_linux_us'], LG['ffmpeg_linux_software_1t_cpu_us'])}}) |
| Motion estimation, 640×480, one core | **{{n(FL['avion_linux_ms'], 2)}} ms** | {{n(FL['opencv_dis_ultrafast_1t_ms'], 2)}} ms | OpenCV DIS ultrafast ({{times(FL['avion_linux_ms'], FL['opencv_dis_ultrafast_1t_ms'], 1)}}) |
| Structure-tensor survey, 640×480 | **{{n(SV['avion_linux_avx2_ms'], 3)}} ms** | {{n(SV['opencv_1t_ms'], 3)}} ms | OpenCV ({{times(SV['avion_linux_avx2_ms'], SV['opencv_1t_ms'], 1)}}) |
| Player CPU while playing, 720p | {{n(PL['avion_pure']['playing_pct'], 2)}} % | {{n(PL['ffplay']['playing_pct'], 2)}} % · **{{n(PL['vlc']['playing_pct'], 2)}} %** | ffplay · VLC: VLC uses less |
| Player CPU while paused | **{{n(PL['avion_pure']['paused_pct'], 2)}} %** | {{n(PL['ffplay']['paused_pct'], 2)}} % · {{n(PL['vlc_start_paused']['paused_pct'], 2)}} % | ffplay · VLC (started paused) |

The scan rows compare reading slice headers with decoding every pixel: that is the point of pre-decode inspection, not a decoder race. AVION's motion step does more than DIS (time to contact, saliency, objects) on 8×8-sample tiles, not per pixel. Cut accuracy on LG's 4K HDR reel, scored against scdet: {{LG['cuts_matched']}} of {{len(LG['scdet_cuts'])}} found, {{len(LG['cuts_extra'])}} extra. Method and raw lines: [benchmarks]({{SITE_URL}}#method).

## What it does, with the figures that check it

**Autonomy, drones and robotics**
- Virtual IMU: the camera's shake from the background alone, 0.068 px rms off the truth (no gyro).
- Second-order stabilisation: 14.73 → 0.92 px rms on screen (the filter predicted 0.94).
- Auto-horizon: synthetic tilts within 0.06°, real footage within 0.38°; black-bar matte +0.014 ms on the GPU.
- Time to contact with ground-plane and surf rejection: 55 of 55 frames, 3.4 % median error; four objects with crossing-safe track ids; an audio beam 10.3 dB down off-axis that follows the view.

**AI front end and neuromorphic perception**
- Bitstream perception: scene cuts and activity from H.264 / HEVC slice headers in {{n(LG['avion_linux_us'], 2)}} µs a 4K frame.
- Tensor bridge: NCHW float32 / float16, foveated on the target, HDR tone-mapped as shown; windowed and host exports identical (0.000000).
- Event camera (DVS emulation), stabilised: a still scene under shake fires 141 events instead of 1,201,527.
- Remote vibrometry and rPPG: 72.00 BPM through shake (138 if the shake is ignored), {{n(SC['pulse']['chroma_bpm'], 2)}} BPM from Cr chroma alone; an 8.000 Hz vibration read from a ±0.3-sample motion.

**Host integration (API 14)**
- Your renderer, our decoder: the GPU decodes H.264 / HEVC into shared Direct3D 11 or Direct3D 12 textures (with fences) that your own renderer draws; PQ10 and scRGB for HDR. 0.7 % of one core for H.264, 6.8 % for 4K HDR10 HEVC (the host read a peak of 5,849 cd/m²), 0 allocations, seeks and resizes included.
- Packages: a Python wheel (`pip install` works offline; zero-copy tensors) and a .NET 8 NuGet package, both exercised from a fresh environment by their own tests.

**Media**
- 4K60 HDR on the GPU; {{n(PL['avion_pure']['paused_pct'], 2)}} % CPU paused, {{n(PL['avion_pure']['playing_pct'], 2)}} % playing 720p; a 5.4 KB (1,356-parameter) neural upscaler with exact temporal tile reuse; shadow lift; hum and drone notches −41.8 to −48.5 dB; pinned A–B loop turns in 0.19–3.76 ms; intent control in nine languages; forensic BMP + JSON snapshots.

## Real engine output

Each picture is a snapshot AVION wrote through its C99 SDK; the overlays come from the snapshot's sidecar. Footage: *Big Buck Bunny* © Blender Foundation, CC BY 3.0, or a generated scene whose truth is known.

![Shadow lift and neural reconstruction]({{SITE_URL}}assets/showcase/plate-lift.png)
![Horizon, two tracked objects and time to contact]({{SITE_URL}}assets/showcase/plate-perceive.png)
![A pulse read through camera shake]({{SITE_URL}}assets/showcase/plate-pulse.png)

| Plate | Truth | AVION read |
|---|---|---|
| Harbour, tilted | {{'%+.2f' % SC['perceive']['tilt_deg']}}° | {{'%+.2f' % SC['perceive']['horizon_deg']}}° ({{'%+.3f' % SC['perceive']['flat_deg']}}° untilted) |
| Buoy closing at 1 m/s | TTC {{n(SC['perceive']['ttc_truth'], 2)}} s | {{n(SC['perceive']['ttc'], 2)}} s; {{n(100 * SC['perceive']['ttc_median_error'], 1)}} % median over {{SC['perceive']['looming_frames']}} frames |
| Pulse under shake | 72.00 BPM | {{n(SC['pulse']['bpm'], 2)}} BPM; Cr only {{n(SC['pulse']['chroma_bpm'], 2)}} BPM |
| Virtual IMU, same clip | the applied shake | {{n(SC['pulse']['imu_rms'], 3)}} px rms off it |
| Event camera, shaken cartoon | — | {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M events → {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M stabilised |

## Quickstart (C99)

A camera in; perception, a tensor and event spikes out. This is the core of `quickstart.c`, which builds with `/W4 /WX` and runs against this release:

```c
{{quickstart(True)}}
```

## Verification

MSVC x64 `/W4 /WX`: Release {{suite('release')}}; Debug {{suite('debug')}}, CTest serial. GCC with ASan + UBSan: {{suite('gcc')}}. 0 third-party libraries: Win32, COM, Direct3D 12, DXGI, WASAPI and C++20.

## Limits

- Windows playback is fully exercised; host mode passes on Linux; Android and Apple toolchains have not been run.
- MP4/MOV and MKV/WebM with H.264 or HEVC on the GPU, AV1 through Windows' own decoder where the GPU has none, AAC, Opus or PCM; VP9 is refused.
- Stabilisation is one translation and one rotation per frame (no rolling-shutter or parallax correction).
- A horizon needs level lines: *Big Buck Bunny*'s meadow, tilted {{n(SC['perceive']['meadow_tilt'], 0)}}°, reads {{'%+.2f' % SC['perceive']['meadow_deg']}}°.
- Cut accuracy on two files is a sanity check, not a recall figure.

## Download

[`AVION-0.1-Evaluation.zip`]({{SITE_URL}}downloads/AVION-0.1-Evaluation.zip): the C99 header and SDK guide, the Release demo and static libraries (Windows x64), the Python wheel and the NuGet package, the head-to-head's raw data and scripts, and the Release suite's log. {{n(M['archive']['bytes'], 0)}} bytes, SHA-256 `{{M['archive']['sha256']}}` ([manifest]({{SITE_URL}}downloads/AVION-0.1-delivery-manifest.json); digests for integrity, not a signature). Dual-licensed ([LICENSING.md]({{SITE_URL}}downloads/LICENSING.md)): the GNU AGPLv3, under which the full source is at {{REPO_URL}}, or a commercial licence from the author for closed-source software, devices and services. The packages on their own: [`avion-0.1.0-py3-none-win_amd64.whl`]({{SITE_URL}}downloads/avion-0.1.0-py3-none-win_amd64.whl) (`pip install`) and [`Avion.Sdk.0.1.0.nupkg`]({{SITE_URL}}downloads/Avion.Sdk.0.1.0.nupkg).
