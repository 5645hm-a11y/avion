# AVION 0.1 — LinkedIn launch post

Rendered by `tools/site/build_site.py` from the measured head-to-head and the showcase; rerun it rather than editing figures. Upload `avion-0.1-demo.mp4` (16 s, 1080p60, 6 MB) as native video (it autoplays muted in the feed), or `avion-0.1-x.png`, or the four plates as a carousel. LinkedIn shows the first ~210 characters before "see more"; its limit is 3,000 (checked by `tools/site/render_cards.py`).

---

AVION 0.1 is out, and open source: a perception kernel for machines that have to see, with zero heap allocations once it runs. One C99 header; the GPU does the work.

Drones, robots, vehicles and smart cameras need more from video than pixels on a screen. They need to know how the camera moved, where the horizon is, what is closing in and how soon, and they need it on time, every frame, without a garbage collector or an allocator deciding when. AVION does that work in front of the model, and hands the model a foveated tensor with no copy.

What it does, with the figure that checks it:

▸ Autonomy. A virtual IMU recovers the camera's shake from pixels alone (0.068 px rms off the truth, no gyro); the window's stabiliser takes on-screen shake from 14.73 to 0.92 px rms; the horizon is found within 0.06° on synthetic tilts and 0.38° on real footage; time to contact flags a synthetic approach on 55 of 55 frames.

▸ AI front end. Scene cuts read from H.264/HEVC slice headers before decoding: {{n(LG['avion_linux_us'], 2)}} µs a 4K frame, against {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms for ffmpeg to decode and score it. NCHW tensors in float32 or float16, HDR tone-mapped. A neuromorphic event camera from any camera: a shaken clip's {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M events become {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M once the camera's own motion is removed.

▸ Remote sensing. A 72 BPM pulse about one code deep in a face's colour, read through camera shake: {{n(SC['pulse']['bpm'], 2)}} BPM ({{n(SC['pulse']['chroma_bpm'], 2)}} from Cr chroma alone). An 8.000 Hz vibration from a motion of 0.3 sample.

▸ Engineering. 0 heap allocations per 1,000 frames, against {{n(LG['ffmpeg_allocs_per_1000'], 0)}} in ffmpeg and {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}} in OpenCV's DIS flow, counted by the same preloaded library in every process. Motion at 640×480 {{times(FL['avion_linux_ms'], FL['opencv_dis_ultrafast_1t_ms'], 1)}} faster than DIS on one core; the structure-tensor survey {{times(SV['avion_linux_avx2_ms'], SV['opencv_1t_ms'], 1)}} faster than OpenCV.

▸ Hosts (API 14). Your renderer, our decoder: the GPU decodes into shared Direct3D 11 / 12 textures in 10-bit PQ or scRGB. 0.7 % of one core for H.264, 6.8 % for 4K HDR10 (peak read 5,849 cd/m²), 0 allocations. A Python wheel and a .NET 8 NuGet package, installed offline in their tests.

▸ Media. 4K60 HDR on the GPU, {{n(PL['avion_pure']['paused_pct'], 2)}} % CPU paused. Playing, VLC still uses less CPU than we do ({{n(PL['vlc']['playing_pct'], 2)}} % against {{n(PL['avion_pure']['playing_pct'], 2)}} %); it is on the site next to the wins.

▸ Open source. The whole C++20 engine, its tests and the benchmark scripts are on GitHub under the GNU AGPLv3: {{REPO_URL}}. A commercial licence covers closed-source products, drones, robots and other devices.

Every picture in the showcase is a snapshot the engine wrote through its own SDK, with a truth we set: an exposure, a tilt, a speed, a shake, a pulse.

Verified: {{SU['release']['passed']}}/{{SU['release']['total']}} MSVC suites in Release and {{SU['debug']['passed']}}/{{SU['debug']['total']}} in Debug, {{SU['gcc']['passed']}}/{{SU['gcc']['total']}} GCC with ASan/UBSan, 0 third-party libraries.

Showcase, charts, method, raw data and the evaluation bundle: {{SITE_URL}}

#ComputerVision #Robotics #EdgeAI #Drones #Neuromorphic #CPlusPlus
