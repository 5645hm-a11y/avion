# AVION 0.1 — X launch thread

Rendered by `tools/site/build_site.py` from the measured head-to-head and the showcase; rerun it rather than editing figures. Attach `avion-0.1-demo.mp4` (16 s, 1080p60, 6 MB: autoplays muted) to post 1 (or `avion-0.1-x.png` / `avion-0.1-x-dark.png` for a still), and the plates (`docs/site/assets/showcase/plate-*.png`) to posts 3–5. Each post is under 280 characters with a URL counted as 23 (checked by `tools/site/render_cards.py`).

The five reply templates at the end are for threads where the topic is already on the table (robotics without an IMU, allocation-free real time, video pre-processing for models, event cameras, benchmark scepticism). Say it is yours, answer the question first, link second; never paste one into an unrelated thread.

---

**1/9**
AVION 0.1 in 16 seconds. Real engine output, no mock-ups: shake read from pixels with no gyro, horizon and time to contact, shadow lift 30 → 69 luma, a pulse through the shake.

Heap allocations per 1,000 frames: 0. ffmpeg: {{n(LG['ffmpeg_allocs_per_1000'], 0)}}. Open source, AGPLv3.

---

**2/9**
How we counted: one tiny malloc-counting library preloaded into every process. AVION, ffmpeg {{S['machine']['linux']['ffmpeg'].split('-')[0]}} and OpenCV {{S['machine']['linux']['opencv']}} counted by the same rule, same machine, same files.

OpenCV DIS flow, outputs reused: {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}} per 1,000 frames. AVION: 0.

---

**3/9**
An event camera from any camera. Shaken by hand, a clip fires {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M events in 3 s.

Stabilised by AVION's virtual IMU, with no gyro: {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M. What is left is what moves.

Real engine output.

---

**4/9**
See it coming. A buoy closes at 1 m/s on a harbour tilted {{n(SC['perceive']['tilt_deg'], 0)}}°.

AVION reads the tilt as {{'%+.2f' % SC['perceive']['horizon_deg']}}°, keeps the boat and the buoy on their track ids, and gives the buoy's time to contact: {{n(SC['perceive']['ttc'], 2)}} s for a true {{n(SC['perceive']['ttc_truth'], 2)}} s.

---

**5/9**
A pulse no eye can see: 72 BPM, about one code deep in the skin's colour, under a shaking camera.

AVION: {{n(SC['pulse']['bpm'], 2)}} BPM. From Cr chroma alone: {{n(SC['pulse']['chroma_bpm'], 2)}}. Ignore the shake and you read 138.

Stabilised, rms on screen: 14.73 → 0.92 px (suite), 5.91 → 0.74 px (reel).

---

**6/9**
Cuts in 4K HEVC without decoding: {{n(LG['avion_linux_us'], 2)}} µs of CPU a frame from slice headers. ffmpeg decoding to score them: {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms.

Motion at 640×480, one core: {{n(FL['avion_linux_ms'], 2)}} ms against OpenCV DIS {{n(FL['opencv_dis_ultrafast_1t_ms'], 2)}} ms.

Different jobs by design.

---

**7/9**
API 14: your renderer, our decoder. The GPU decodes into shared D3D11/D3D12 textures you draw yourself, PQ10 or scRGB: 0.7 % of one core for H.264, 6.8 % for 4K HDR10 (peak read 5,849 cd/m²), 0 allocations.

pip and NuGet packages, installed offline.

---

**8/9**
Where we lose, we say so: VLC plays 720p with {{n(PL['vlc']['playing_pct'], 2)}} % of a core, AVION {{n(PL['avion_pure']['playing_pct'], 2)}} %. Paused: {{n(PL['avion_pure']['paused_pct'], 2)}} %.

The benchmark found two of our bugs (cuts on a hierarchical GOP, paused CPU). Both fixed in the engine before release.

---

**9/9**
{{SU['release']['passed']}}/{{SU['release']['total']}} MSVC Release, {{SU['debug']['passed']}}/{{SU['debug']['total']}} Debug, {{SU['gcc']['passed']}}/{{SU['gcc']['total']}} GCC with ASan/UBSan. 0 dependencies. SDK API 17.

Full source, AGPLv3 or a commercial licence: {{REPO_URL}}
Showcase and raw data: {{SITE_URL}}

---

## Reply templates

---

**1/5**
If your camera has no IMU, or a bad one: AVION recovers the shake from pixels alone. {{n(SC['pulse']['imu_rms'], 3)}} px rms off the truth in our showcase, 0.068 in the suite. A front end for your estimator, not a VIO replacement. 16 s demo: {{SITE_URL}}

---

**2/5**
Steady state, zero heap allocations. We counted per 1,000 frames with one preloaded malloc counter, the same rule in every process: AVION 0, ffmpeg decode + scdet {{n(LG['ffmpeg_allocs_per_1000'], 0)}}, OpenCV DIS {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}}. Raw lines: {{SITE_URL}}

---

**3/5**
A cheap gate before the model: scene cuts read from H.264/HEVC slice headers, {{n(LG['avion_linux_us'], 2)}} µs of CPU a 4K frame, against {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms for ffmpeg to decode and score it. Different jobs; the method is public: {{SITE_URL}}

---

**4/5**
Event-camera data without the sensor: a shaken clip fires {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M emulated events, {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M once its own motion is removed. An emulation from frames, not a DVS. Real output: {{SITE_URL}}

---

**5/5**
"Benchmarks are cherry-picked." So we published where we lose: VLC plays 720p with {{n(PL['vlc']['playing_pct'], 2)}} % of a core, AVION {{n(PL['avion_pure']['playing_pct'], 2)}} %. Two of our bugs came from the benchmark. Raw data and scripts: {{SITE_URL}}
