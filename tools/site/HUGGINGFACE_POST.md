# AVION 0.1 — Hugging Face community post

Rendered by `tools/site/build_site.py` from the measured head-to-head and the showcase; rerun it rather than editing figures. Attach `plate-events.png` (or link the card), and publish the model card from `HUGGINGFACE_README.md` first.

---

AVION 0.1: a perception front end for robot brains, in one C99 header

Most vision stacks feed a model with frames that were decoded, copied and allocated for. AVION does the work in front of the model on the GPU, with zero heap allocations once it runs, and hands the network a foveated NCHW tensor (fp32/fp16) with no copy.

What it gives a model, or a robot, from an ordinary camera:
• a virtual IMU: the camera's shake from pixels alone, {{n(SC['pulse']['imu_rms'], 3)}} px rms off the truth in our showcase (0.068 px in the suite)
• a stabilised event-camera (DVS) stream: {{n((SC['events']['totals']['events_raw']['written'] + SC['events']['totals']['events_raw']['dropped']) / 1e6, 1)}} M events from a shaken clip become {{n((SC['events']['totals']['events_stab']['written'] + SC['events']['totals']['events_stab']['dropped']) / 1e6, 2)}} M, only what moves
• time to contact for what is approaching: {{n(SC['perceive']['ttc'], 2)}} s read for a true {{n(SC['perceive']['ttc_truth'], 2)}} s
• a pulse no eye can see: {{n(SC['pulse']['bpm'], 2)}} BPM through camera shake, {{n(SC['pulse']['chroma_bpm'], 2)}} BPM from Cr chroma alone
• scene cuts from H.264/HEVC slice headers before decoding: {{n(LG['avion_linux_us'], 2)}} µs a 4K frame, against {{n(LG['ffmpeg_linux_software_1t_cpu_us'] / 1000, 1)}} ms for ffmpeg to decode and score it
• host-driven GPU decode into shared D3D11/D3D12 textures (API 14): 0.7 % of one core for H.264, 6.8 % for 4K HDR10 HEVC in PQ10 (peak read 5,849 cd/m²); a pip wheel and a .NET 8 NuGet package

Measured head to head on one machine with one allocation counter: 0 allocations per 1,000 frames against {{n(LG['ffmpeg_allocs_per_1000'], 0)}} (ffmpeg) and {{n(FL['opencv_dis_ultrafast_1t_allocs_per_1000'], 0)}} (OpenCV DIS); motion {{times(FL['avion_linux_ms'], FL['opencv_dis_ultrafast_1t_ms'], 1)}} faster than DIS at 640×480. Where it loses, we say so: VLC plays 720p with less CPU ({{n(PL['vlc']['playing_pct'], 2)}} % against {{n(PL['avion_pure']['playing_pct'], 2)}} %).

The whole source is open under the GNU AGPLv3 ({{REPO_URL}}), with a commercial licence for closed products. Every showcase picture is a snapshot the engine wrote itself, with a truth we set. Model card, charts, method and the evaluation bundle: {{SITE_URL}}
