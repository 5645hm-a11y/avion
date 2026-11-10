# The C99 SDK, modules and frame signatures (2026-10-21)

Directive: "UNIVERSAL C99 SDK & ZERO-COST MODULARITY (TASK 65)". A host written in C, or any language that calls C, embeds the engine through [`include/media/c_api.h`](../include/media/c_api.h): plain structs, enums and one opaque handle, no C++ type, no exception and no allocation across the boundary.

## Lifecycle

```c
media_engine_config_t config;
media_engine_config_init(&config);                       /* Every module, "Avion", visible. */
config.modules = MEDIA_MODULE_ALL & ~MEDIA_MODULE_VOICE;  /* Leave voice out: its region and thread are not made. */
uint64_t arena = media_query_arena_budget(&config);       /* What open will commit, exactly. */

static unsigned char storage[...];                        /* media_engine_storage_bytes(), aligned to ..._alignment(). */
media_engine_t* engine;
media_engine_create(&config, storage, sizeof storage, &engine);
media_engine_open(engine, "film.mp4");                    /* UTF-8 path or http(s):// URL. */
media_engine_run(engine);                                 /* Blocks until the end, maximum_seconds or media_engine_stop. */
media_engine_close(engine);
media_engine_destroy(engine);
```

- **No heap.** The SDK never allocates. The handle is constructed in the host's storage (332,224 bytes on Windows x64, 64-byte aligned); paths are converted into fixed buffers in it. Everything else comes from the engine's arena, committed in `open` and sealed before `run`.
- **One file per open.** Each `open` constructs a fresh coordinator in the same storage; `close` releases the arena, the window and the threads.
- **Threads.** `create`, `open`, `run`, `close`, `destroy`, `command`, `hear` and `set_talk` belong to one host thread (the one that runs). `media_engine_poll_frame` may be called from one other thread, any time between `open` and `close`. `media_engine_stop` may be called from any thread.
- **Commands.** `media_engine_command(engine, "חשוך")` takes any command of the [grammar](intents.md), in the nine languages. `media_engine_hear` takes an utterance from the host's own speech recognition through the wake policy.
- **Errors.** `MEDIA_ERROR_OPEN` and `MEDIA_ERROR_PLAYBACK` leave the engine's stage and HRESULT in `media_engine_last_error`. Reaching `maximum_seconds` is `MEDIA_OK`.
- **Other platforms.** The configuration, budget and wake-word functions are portable. On Linux an engine can be created, configured and destroyed; `open` returns `MEDIA_ERROR_UNSUPPORTED` (there is no playback backend there yet).

## Modules

`media_module_flags` (C) and `EngineModuleFlags` (C++, [`EngineModules.hpp`](../include/media/engine/EngineModules.hpp)) are the same values:

| Module | Region | Threads | Measured use |
|---|---|---|---|
| `MEDIA_MODULE_VOICE` | 12 MiB | 1 (the voice thread, when `voice_input` is set) | 10.26 MiB (VAD, recogniser tables and scratch) + the 256 KiB capture ring |
| `MEDIA_MODULE_HUD` | 1.5 MiB | 2 (the command bar's and the status cues' text threads) | 1.25 MiB |
| `MEDIA_MODULE_SUBTITLES` | 32 MiB | 1 (the shaping thread, when there is text) | 9.6 MiB of glyph pipeline, then the text |
| `MEDIA_MODULE_NEURAL_UPSCALE` | 0 | 0 | GPU resources only: the 2x luma texture and the network |
| `MEDIA_MODULE_CHAPTERS` | 1.5 MiB | 0 | at most 1.25 MiB (65,536 keyframes, 16,384 dialogue spans) |
| `MEDIA_MODULE_TELEMETRY` | 1,408 KiB (64 KiB until 2026-10-31, 448 KiB until 2026-11-01, 576 KiB until 2026-11-03, 3,392 KiB until 2026-11-04) | at most 1 (since 2026-11-01: perception's, for a picture whose tracking grid is 128 cells a side or more) | 4.2 KiB (the 64-signature ring); the DSP's spectral analysis runs for it; perception on the tracker's grids, the objects Tab and "next target" move between (`video::Perception::ArenaBytes` of at most 480 cells: 324.5 KiB), the grid handed to perception's thread (at most 127.5 KiB), and since 2026-11-03 the window's event camera on the grids (2.09 MB) and their virtual IMU (480 KiB) |

The core (file blocks, the demux index pool, codec tables, queues, the audio path) is 192 MiB; it measured 177.0 MiB on every fixture, from 64×64 H.264 to 4K HEVC Main10, 7.1 and the 3.5-hour file. Every module: 239.4 MiB (239.1 before perception joined telemetry's region, 2026-10-31), less than the 256 MiB the engine committed before modules.

**How "zero cost" is made exact.** `Prepare` makes each enabled module's region first, in module order, as one block of the file arena, and puts a nested arena over it (`LinearMemoryArena::InitializeView`). Every allocation of the module comes from its region:
- voice: the recognised-text ring, `MicroVad`, the recogniser, the capture ring;
- HUD: the command bar's text, the command bar's and the status cues' pipelines (a warm session's pipelines live in the session's own arena);
- subtitles: the sidecar, the embedded text (`ImportText`), `SubtitleTrack`, quote and concept search, the SDF glyph pipeline;
- chapters: the keyframe and dialogue scratch of `PrepareTimeline`;
- telemetry: the signature ring.

So the arena's capacity is `ArenaBudget(modules)` exactly, and leaving a module out removes exactly its region from the capacity and from the bytes in use (the regions are multiples of 64 KiB: later allocations move by whole pages, no alignment padding changes). A module that needs more than its region fails alone, as each already could (subtitles off for a file whose text does not fit, voice off): playback goes on.

Left out, a module also starts no thread and does no work: no dock is drawn or touched (`EnableHud`), no 2x texture or network is made (`AllowNeural`; the Neural tier converts as Edge-directed), no timeline is built (chapter commands answer `Unavailable`), no signature ring or spectral analysis runs.

**Measured** (`media_c_api_tests`, Release, the chapters fixture with a subtitle sidecar): with every module the arena is 239.1 MiB, 223.9 MiB in use, 4 threads (the worker, the command bar's and the status cues' text threads, the subtitle shaper); without HUD, subtitles, chapters, voice and telemetry, 192.0 MiB and 176.8 MiB, 1 thread: **47.1 MiB less in capacity and 47.1 MiB less in use, exactly their regions, and 3 threads fewer, exactly theirs.** That engine still plays (48 pictures in its 2 s) and produces no signature.

The first region sizes were guesses; the voice region (8 MiB) turned out too small for the recogniser (10.26 MiB), which made `media_playback_voice_test` fail with voice silently off. The sizes above are the measured use with headroom, and the demo prints each region's use (`Arena:` line) so a module outgrowing its region shows.

## Wake policy

| `wake_policy` | What an utterance needs |
|---|---|
| `MEDIA_WAKE_DEFAULT` | "Avion" (or אביון) first, or within 4 s after it alone ([voice](voice.md)) |
| `MEDIA_WAKE_DISABLED` | push-to-talk: nothing but the host's talk button held (`media_engine_set_talk`) |
| `MEDIA_WAKE_CUSTOM` | the name in `wake_word` (UTF-8, at most 31 bytes, ASCII letters in any case, a whole word) |

The wake gate reads text, not sound (`AvionWakeGate::SetWord`, `SetPolicy`, `SetTalk`). A custom name works for every utterance given to `media_engine_hear` (a host's own speech recognition) and for typed text. Since API 2 (2026-10-21, second directive) it is heard from the microphone too: `open` synthesises its templates (formant synthesiser, and the host's `speak` callback or on Windows the system's voices), so `media_wake_word_hearable("jarvis")` is 1, and 0 only for a name the engine cannot read ("R2D2"). See [voice](voice.md#custom-wake-words-2026-10-21) for how well real voices are heard with and without a speech source; `media_engine_stats_t::wake_templates` says how many templates were made.

## Frame signatures

`media_frame_signature_t` (64 bytes) is what the engine already computed about each picture as it is put on screen, and about the sound under it. Nothing is computed for it alone:
- the video fields come from the conversion's statistics pass (one luma sample per 8×8 block of the crop), which also feeds the live scene-cut detector;
- the audio fields come from the DSP's spectral analysis (`SpectralTracker`: a 4096-point FFT every 1024 frames), which the notch killer and the ducking need anyway.

| Field | Meaning |
|---|---|
| `luminance_mean`, `luminance_peak` | 0..1, of the block samples (code value / full scale, as coded) |
| `tile_motion` | the share of 8×8 blocks whose sample moved by more than 1.2 % since the picture converted before it; −1 for the first of an epoch or a zoom region |
| `MEDIA_FRAME_SCENE_CUT`, `scene_cut_pts_100ns` | the live detector confirmed a cut on this picture; it decides four pictures after the cut (it must see the new shot hold), so the field names the cut's own picture |
| `speech_probability` | 0..1: the speech band's share above 300 Hz, times its syllabic modulation |
| `dominant_band`, `_hz`, `_db` | the loudest of the ten octave bands (31.5 Hz to 16 kHz) and its level |
| `MEDIA_FRAME_MEASURED` | the picture's statistics were read back in time (else the luma fields are −1) |

Two ways to take them:
- **Callback** (`on_frame`): on the engine's thread, right after the presentation pass that showed the picture, so the host may give commands from it.
- **Poll** (`media_engine_poll_frame`): one consumer on any thread, from a 64-entry lock-free ring in the telemetry region. When the ring is full the newest is dropped and counted (`signatures_lost`).

The statistics pass gained the peak and the block motion at no extra read: the same sample is compared with the block's previous one (a 1 MiB buffer of words, 4096×4096 crops), and a one-thread pass clears the per-picture fields after the copy.

**Measured** (`media_c_api_tests` on `playback_chapters.mp4`: three scenes of flat luma 24, 196 and 110 with a texture of up to 23 codes, cuts at 3 s and 6 s, a 440 Hz tone; 24 fps):
- 216 of 216 signatures measured; every one's mean and peak inside its scene's codes (72/72 in each scene);
- the pictures the stream repeats (P_Skip copies) have `tile_motion` exactly 0: 36 per scene, every other picture;
- exactly two cuts, naming 3.000 s and 6.000 s, confirmed on the pictures at 3.167 s and 6.167 s; the cut pictures' `tile_motion` is 1.00;
- the 500 Hz band dominant and speech below 0.3 in 192 of 192 signatures after the first second;
- the callback's and the poll's copies identical; 0 allocations while running.

## Verification

- `media_c_api_tests`: a C99 translation unit (`tests/c_api_tests.c`; MSVC `/W4 /WX`, GCC `-std=c99 -Wpedantic -Werror` and ASan/UBSan). Defaults, exact budgets (each module's removal equals its region), invalid configurations and storage, wake-word hearability, the lifecycle; on Windows the playback checks above, the custom name ("Jarvis, volume down" runs; "volume down" and "avion volume down" are set aside), a cure and its undo through `media_engine_command` (a second undo has nothing to undo), and push-to-talk.

## Host-driven playback on every platform (2026-10-21, second directive)

Directive: "CROSS-PLATFORM LINUX/C99 CALLBACK PLAYBACK". Before it the SDK played on Windows only. Now `config.playback = MEDIA_PLAYBACK_HOST` (the default off Windows) plays on any platform with a C compiler: the host drives, the engine starts no thread.

```c
media_engine_config_init(&config);
config.playback = MEDIA_PLAYBACK_HOST;
config.on_present = present;          /* The picture due now: planes (software bridge) or the pts the host decoded. */
config.decode_video = NULL;           /* NULL: the engine's software bridge; else the host's decoder gets every access unit. */
config.decode_audio = NULL;           /* NULL: PCM tracks only; else the host decodes AAC / Opus. */
config.audio_rate = 48000; config.audio_channels = 2;
config.voice_input = MEDIA_VOICE_HOST; /* The host feeds its microphone (16 kHz mono). */
media_engine_create(&config, storage, sizeof storage, &engine);
media_engine_open(engine, "film.mp4");
while (media_engine_step(engine, now()) == MEDIA_OK) { /* the host's loop */ }
/* The host's audio callback, any thread (one): */ media_engine_pump_audio(engine, out, frames, when_heard);
/* The host's microphone: */                     media_engine_feed_voice(engine, samples, count);
```

**What runs** ([`HostEngine`](../include/media/engine/HostEngine.hpp), portable C++ composed only of portable pieces):

| Stage | Where | What |
|---|---|---|
| File | `step` | [`FileBlockSource`](../include/media/platform/FileBlockSource.hpp): 8 blocks of 1 MiB from the arena, unbuffered stdio (UTF-8 paths, wide on Windows) |
| Demux | `step` | `IsobmffParser` (classic and fragmented MP4; seek, track switch); audio read at most 250 ms ahead, so a switch is heard within that |
| Video | `step` | `H264SyntaxParser` / `HevcSyntaxParser` (DPB, marking, POC) then [`H264PcmDecoder`](../include/media/codec/H264PcmDecoder.hpp) (the software bridge) or the host's `decode_video`; 12 pictures in presentation order |
| Audio | `step` | PCM tracks decoded by the engine (`ipcm`, `fpcm`, `sowt`, `twos`: new in the demuxer), or the host's `decode_audio`; `PcmConverter` to the host's rate and channels; an SPSC `PcmRingBuffer` |
| Sound | `pump_audio` | `DspProcessor` (mastering EQ, notch killer, speech ducking, audio zoom, night mode, clarity), `TimeStretcher` (speed), `OutputGain` (volume, mute) |
| Clock | `pump_audio` / `step` | `PredictiveMasterClock` anchored on what was pumped and when (the host's clock); without audio, the step's clock |
| Pictures | `step` | the due picture to `on_present` (older ones dropped), with the caption on screen, the view (zoom and centre) and the status cue; then its frame signature, measured on the CPU from the decoded planes exactly as the GPU's statistics pass samples them |
| Commands | `step` | `LocalIntentDispatcher` with the conversation state and the cures; `feed_voice`: `MicroVad -> VoiceGate -> AcousticIntentRecognizer -> AvionWakeGate`, its commands run at the next step |

Commands available in host mode: play/pause, volume and mute, relative and absolute seek, quote search and "again", audio track by language or next, subtitles on/off, chapters, speed, zoom (the view handed to the host), night mode, audio zoom, the audio cures and undo, target tracking ([picture](picture.md#target-tracking-2026-10-21)). The rest (picture controls, loops, bookmarks, files, statistics) say `Unavailable`.

**The software bridge** decodes, bit-exactly and without prediction or transform, H.264 pictures made of I_PCM macroblocks and of skipped macroblocks whose motion is zero (RefPicList0[0] copied); deblocking leaves both unchanged. CAVLC, frames. It is what lets every platform decode real pixels from a real MP4 with no codec library; anything else is `Unsupported` and needs the host's decoder (counted: `HostEngine::Undecodable`).

**Threads.** `create`, `open`, `step`, `command`, `hear`, `feed_voice`, `set_talk`, `close`, `destroy` on one host thread; `pump_audio` on one thread (the same, or the audio thread). They share only the ring and atomics (cache-line isolated). The host must pump while a sound track plays: the clock is the sound's.

**Budget.** Host mode's arena is `48 MiB + the regions of voice, subtitles, chapters and telemetry` (HUD and neural upscaling take none): 93.6 MiB with every module. Measured on the portable fixture: 62.6 MiB in use, of which 45.6 MiB are the module regions (the core uses 17 MiB of its 48). The handle is 413,504 bytes on Windows x64 (it holds the windowed coordinator too).

**Not covered.** The bridge decodes only its subset; HEVC, AV1 and general H.264 need the host's decoder, AAC and Opus the host's audio decoder. The windowed engine does not play PCM tracks. A custom wake word set while open is heard as text at once; its templates are made at the next open.

**Verified** (`media_c_api_tests`, pure C99, on Windows MSVC and on Linux GCC 13.3 with `-O2` and with ASan/UBSan; the portable fixture is written by [`tests/make_portable_fixture.cpp`](../tests/make_portable_fixture.cpp) on each platform: 9 s of 64x64 I_PCM H.264 in three scenes with a moving square, an English `ipcm` 48 kHz stereo track at 440 Hz + 4 kHz, a French `sowt` 44.1 kHz mono track at 660 Hz, Nero chapters). One run on a virtual clock (a step and 10 ms of pumped audio every 10 ms), 15.00 s virtual:

- 344 pictures, 0 dropped, 0 allocations while playing, 0 threads; to the end;
- each scene's luma right on every measured picture (120 of 120, 130 of 130), the cut at 3.000 s; the sidecar's caption on 72 pictures;
- English 440 Hz at -12.0 dB and 4 kHz at -24.4 dB (as written); "volume down" -3.0 dB, "volume up" back to 0.0 dB;
- "audio in french please": 660 Hz at -10.5 dB, the English tone gone (-284 dB in the window), the track "fra";
- the custom name "Falcon" (templates from the formant synthesiser and four recorded voices given through `speak`): Mark's "Avion pause" and "what time is it" set aside, his "Falcon pause" paused (first rendition), the position held while paused;
- "faster": 1.250 media seconds a second, the 660 Hz tone kept (528 Hz at -60 dB);
- "find quiet between the frames": landed exactly on 1.0000 s; "noisy": the stationary 660 Hz tone from -10.5 to -69.5 dB by the notch killer; "בטל": back to -10.5 dB;
- "next chapter": landed exactly on 6.0000 s; "track target": the view (2x) within 0.005 of the moving square on all 66 pictures from 6.25 s.

## Live sensors and perception (2026-10-22)

Directive: "BIONIC RETINA & PHYSICAL WORLD PERCEPTION SDK". API 3 (`MEDIA_API_VERSION 3u`). A host that has a camera and a microphone array (a robot, a drone, a vehicle, a smart camera) pushes them into the engine instead of opening a file, and reads back what the engine perceives: motion, approaching obstacles with their time to contact, where to look, where a sound came from.

```c
media_engine_config_init(&config);
config.playback = MEDIA_PLAYBACK_HOST;                 /* Live sensors are host mode (every platform). */
config.modules = MEDIA_MODULE_TELEMETRY;               /* Perception is telemetry: without it, no flow, no saliency. */
config.on_present = present;                           /* Optional: each frame, converted, with the view to draw. */
media_engine_create(&config, storage, sizeof storage, &engine);
media_sensor_config_init(&sensor);
sensor.width = 640; sensor.height = 480;               /* Even, 64..4096 x 64..2304 (perception needs 128 x 128). */
sensor.audio_rate = 48000; sensor.audio_channels = 2;  /* 1..8 channels; 0 and 1 are the pair the locator listens to. */
sensor.field_of_view = 1.57f;                          /* Radians, horizontal (0: 90 degrees). */
sensor.microphone_spacing = 0.2f;                      /* Metres between channels 0 and 1 (0: 0.2). */
media_engine_open_sensor(engine, &sensor);
/* Camera thread: */      media_engine_ingest_video_frame(engine, &frame);   /* NV12, P010 or RGBA8 + timestamp (+ IMU deltas). */
/* Microphone thread: */  media_engine_ingest_audio_samples(engine, pcm, MEDIA_SAMPLE_F32, frames, timestamp);
media_query_frame_telemetry(engine, &telemetry);       /* Motion, TTC, hazard, the newest sound, the view. */
media_query_saliency_grid(engine, &grid);              /* 16 x 16 weights, a mask, per-tile flow. */
media_engine_command(engine, "look at sound");         /* Or media_engine_set_audio_saccade(engine, 1). */
```

**What runs.** [`HostEngine::OpenSensor`](../include/media/engine/HostEngine.hpp) prepares no file, demuxer or decoder: two picture slots of the camera's size (8-bit 4:2:0), the tracker's grid, the signatures' block codes, [`video::Perception`](../include/media/video/Perception.hpp) (about 0.33 MiB), and, for the microphones, a conversion to the pumped output, the ring, the DSP and [`audio::SoundLocator`](../include/media/audio/SoundLocator.hpp). Then the arena is sealed (Primed). Each `ingest_video_frame` copies the frame into a slot (NV12: luma and de-interleaved chroma; P010: the top 8 bits, rounded; RGBA8: full-range BT.709 RGB to limited-range Y'CbCr 4:2:0 in integer arithmetic, the same codes on every platform) and shows it at once through the same `Show` as a file's pictures: target tracking, perception, the audio saccade, the frame signature, `on_present` (planes and the view), `on_frame`. Each `ingest_audio_samples` (float or 16-bit) feeds the locator with the raw pair, and, when the host pumps audio, the ring that `pump_audio` plays through the DSP (EQ, notch killer, the audio zoom's beam following the view, night mode). Without room in the ring the samples are counted and dropped (`HostEngine::AudioOverflow`); ingest never waits.

| Function | Thread | Notes |
|---|---|---|
| `media_engine_open_sensor` | host | `MEDIA_ERROR_STATE` when open or not in host mode; a camera, microphones or both |
| `media_engine_ingest_video_frame` | host (the one that steps and commands) | the configured size; timestamps must increase (else `MEDIA_ERROR_INVALID_ARGUMENT`, nothing changes) |
| `media_engine_ingest_audio_samples` | one thread (the capture's) | `timestamp` of the first frame (-1: unknown; sound events then carry -1) |
| `media_query_frame_telemetry`, `media_query_saliency_grid` | host | the newest picture measured; `MEDIA_ERROR_EMPTY` before it |
| `media_engine_set_audio_saccade` | host | `MEDIA_ERROR_UNSUPPORTED` without a stereo pair and a picture |

`media_frame_telemetry_t`: the dominant motion (median of the reliable tiles, picture widths and heights per second), the share of reliable tiles, the strongest approach's divergence, **`ttc_seconds`** and **`hazard_x`, `hazard_y`** (its focus of expansion), the salient share, the newest sound's azimuth, level and time, the counts of sounds and saccades, and the view. Flags: `FLOW`, `LOOMING`, `EGO` (the IMU's rotation was taken out), `SOUND`, `SACCADE`. `media_saliency_grid_t`: 16 x 16 weights (0..255), a 16-row bit mask of the tiles to look at, and each tile's own motion (`flow_x`, `flow_y`, `flow_valid`). How they are computed: [picture](picture.md#perception-flow-looming-and-saliency-2026-10-22) and [audio](audio-dsp.md#sound-localisation-and-the-audio-saccade-2026-10-22). Files played in host mode get the same perception when the software bridge decodes their pictures (128 x 128 samples or more) and the same locator on the programme as it is pumped; `media_playback_state_t.saccade` (API 3, from a reserved field) says whether the saccade is on.

**What does not run on sensor frames** (until the second 2026-10-22 directive: [GPU live ingest](#gpu-live-ingest-and-stabilisation-2026-10-22-second-directive)). The windowed engine's GPU passes (shadow lift and the other picture controls, micro-motion magnification, neural and edge-directed super-resolution) are D3D12 shaders on decoded GPU surfaces; host mode has no GPU, so ingested frames are handed to the host as 8-bit planes with the view to draw, as a file's pictures are in host mode. Ingesting into the windowed engine's GPU pipeline is not done.

**Verified** (`media_c_api_tests`, pure C99; Windows MSVC Release and Debug, Linux GCC 13.3 `-O2` and ASan/UBSan, the same figures to the digit): 120 camera frames (640x480 NV12, 30 fps) and 4 s of stereo microphones, no file, **0 allocations** while ingesting, stepping and querying.
- An obstacle approaching at 1 m/s from 3 m (a textured square on a flat background, half size 50 Z0 / Z px): looming on 49 of 51 frames from 0.3 s; TTC error 8.6 % on average; TTC 2.78 s early (truth 2.45) falling to 1.19 s (truth 1.25); the hazard 0.028 from the obstacle's centre on average.
- A small object moving over a static textured scene: at most 6 of the 256 tiles salient (98 % of the picture left out), none away from it.
- "look at sound", then a 20 ms burst on the right channel only: azimuth +0.996, one sound, one saccade; the view (2x) at 0.703 on the next frame and 0.750 (the right edge less half a view) five frames later.
- P010 code 512 shown as 128, RGBA white as 235 and mid-grey 128 as 126; a frame of the wrong size and a timestamp going back refused.

## GPU live ingest and stabilisation (2026-10-22, second directive)

Directive: "GPU-ACCELERATED LIVE CAMERA INGEST, HYBRID LOW-POWER WAITABLE PACING, IMU ELECTRONIC STABILIZATION (EIS) & REAL-VIDEO PERCEPTION". API 4 (`MEDIA_API_VERSION 4u`). Live sensors opened with `MEDIA_SENSOR_GPU` send every camera frame through the windowed engine's own GPU passes as well, offscreen, and give the host the result as an RGBA image; `MEDIA_SENSOR_STABILIZE` (or `media_engine_set_stabilization`) steadies the view from the IMU's deltas.

```c
media_sensor_config_init(&sensor);
sensor.width = 1280; sensor.height = 720;
sensor.options = MEDIA_SENSOR_GPU | MEDIA_SENSOR_STABILIZE;   /* API 4 (from two reserved fields: old hosts send 0). */
sensor.output_width = 2560; sensor.output_height = 1440;       /* The GPU image (0 x 0: the camera's size). 2x: super-resolution. */
media_engine_open_sensor(engine, &sensor);                     /* MEDIA_ERROR_UNSUPPORTED without a hardware D3D12 device. */
media_picture_controls_init(&controls);                        /* Neutral, edge-directed. */
controls.shadow_lift = 0.4f; controls.micro_motion = 3; controls.reconstruction = MEDIA_RECONSTRUCT_NEURAL;
media_engine_set_picture_controls(engine, &controls);          /* From the next frame. */
media_engine_ingest_video_frame(engine, &frame);               /* + frame.flags = MEDIA_SENSOR_IMU, imu_yaw/pitch/roll. */
if (media_query_gpu_image(engine, &image) == MEDIA_OK) show(image.rgba, image.width, image.height, image.stride);
```

**What runs.** [`render::LiveGpuPipeline`](../include/media/render/LiveGpuPipeline.hpp) (Windows) is made by `open_sensor`: a device on the first hardware adapter, one direct queue, three slots (an upload buffer, an NV12 texture, an RGBA image and a mapped readback each), the production [`ColorPipeline`](../include/media/render/ColorPipeline.hpp) with micro-motion's reference, the [`NeuralUpscalePass`](../include/media/render/NeuralUpscalePass.hpp) and its 2x luma texture, and timestamps. Each `ingest_video_frame`, after the host engine has taken the frame (converted to 8-bit 4:2:0, perceived, shown), copies it into the next slot's mapped upload buffer, records one list (the upload, the neural pass when asked and upscaling, the conversion of the view's region with the picture controls and micro-motion, the copy to the readback) and submits it. It never waits: when all three slots are still on the GPU the frame's GPU pass is skipped and counted (`media_gpu_image_t.dropped`). `media_query_gpu_image` returns the newest completed image (no wait); its pixels stay valid until two more frames have been ingested.

The region is the host engine's view, so the saccade, target tracking, the zoom commands and stabilisation all steer the GPU image the same way they steer `on_present`'s view. The conversion is the windowed engine's: BT.709 limited range, guided chroma, the downscale prefilter, edge-directed or neural luma while upscaling (bilinear when asked), each picture control only when not neutral, micro-motion against a per-pixel reference that starts over when it is turned on and with every new region (a moving view restarts it: with stabilisation on, micro-motion is mostly restarting). (Since API 5 the reference follows the moving view instead: [below](#roll-ten-bit-and-compensated-micro-motion-api-5-2026-10-27).)

| Function | Notes |
|---|---|
| `media_sensor_config_t.options`, `output_width`, `output_height` | `MEDIA_SENSOR_GPU`, `MEDIA_SENSOR_STABILIZE`; other bits `MEDIA_ERROR_INVALID_ARGUMENT`; the GPU option needs a camera |
| `media_engine_set_stabilization` | host mode with a camera (`MEDIA_ERROR_UNSUPPORTED` otherwise); `MEDIA_TELEMETRY_STABILIZED` while on |
| `media_engine_set_picture_controls` | `MEDIA_SENSOR_GPU` only (`MEDIA_ERROR_UNSUPPORTED` otherwise); clamped as the windowed engine clamps them |
| `media_query_gpu_image` | `MEDIA_ERROR_EMPTY` before the first; flags `MEDIA_GPU_NEURAL`, `_EDGE`, `_GRADED`, `_MICRO_MOTION`, `_STABILIZED`; the region converted; GPU time |

**Stabilisation** ([picture](picture.md#stabilisation-2026-10-22)): [`video::Stabilizer`](../include/media/video/Stabilizer.hpp) accumulates the IMU's yaw and pitch, takes out their slow part (a one-pole low-pass of 0.35 s: pans and tilts pass through) and moves the view with the rest, inside a 1.25x crop. Perception takes the IMU's rotation out of its flow and, since this directive, out of its saliency, stabilised or not. Portable: on Linux the host's view is steadied the same way (the GPU image is Windows only).

**Verified.**
- `media_gpu_live_ingest_tests` (new, Windows, through the C API; UHD 630): shadow lift 0.6 raises all 150 dark columns of a ramp (Y' 0.10 by +0.090) and moves none of the 75 from Y' 0.76 up, monotonic, 0.45 ms on the GPU at 320x240; micro-motion 5x turns a +-2-code pulse into 27.0 codes peak to peak (5.0 without: 5.4x), the surround unchanged; a camera shaken +-3 / 2.5 degrees in yaw / pitch at 7 / 9 Hz: the images' content moves 2.12 camera pixels RMS stabilised against 12.12 without, the IMU's rotation taken out of perception on 42 of 42 pictures, no looming, 14 salient tile-pictures in 42 pictures of a scene where nothing moves (before the saliency change: about 28 a picture); 2x: edge-directed 1.33 ms and neural 2.19 ms, flagged as such, the same mean within 0.1 code; 40 frames through every pass at once with **0 allocations**.
- `media_c_api_tests` (C99, Windows and Linux): stabilisation on, a 0.02 rad turn to the right moves the view from 0.7500 to 0.7409 (2x, flagged); without the GPU option, no GPU image and no picture controls.

## Roll, ten-bit and compensated micro-motion: API 5 (2026-10-27)

Directive: "MOTION-COMPENSATED MICRO-MOTION UNDER SHAKE, NATIVE 10-BIT P010 & ROLL STABILIZATION IN GPU LIVE INGEST, AND MULTI-SCALE GROUND-PLANE LOOMING". `MEDIA_API_VERSION 5u`. Every API 4 call behaves as before; the new fields come from reserved words (`media_frame_telemetry_t`, `media_gpu_image_t`) or are appended (`media_picture_t.view_roll`, which the engine fills and hosts only read).

```c
sensor.options = MEDIA_SENSOR_GPU | MEDIA_SENSOR_GPU_10BIT | MEDIA_SENSOR_STABILIZE; /* API 5: a P010 texture ring. */
frame.format = MEDIA_PIXEL_P010;                     /* Uploaded as it is: all 1,024 levels reach the picture controls. */
frame.flags = MEDIA_SENSOR_IMU; frame.imu_roll = roll_delta; /* Since API 5 the stabiliser turns the view with the roll's shake. */
media_engine_ingest_video_frame(engine, &frame);
media_query_gpu_image(engine, &image);               /* image.roll: the view's turn; MEDIA_GPU_TEN_BIT, MEDIA_GPU_MICRO_FOLLOWED. */
media_query_frame_telemetry(engine, &t);             /* t.view_roll; MEDIA_TELEMETRY_GROUND (t.horizon_y, t.focus_x), _FOVEA. */
```

| Addition | Notes |
|---|---|
| `MEDIA_SENSOR_GPU_10BIT` | with `MEDIA_SENSOR_GPU` only (`MEDIA_ERROR_INVALID_ARGUMENT` alone): P010 textures; P010 frames uploaded as they are, 8-bit frames as their 10-bit codes (v x 4); the conversion's 10-bit path and its dither to the 8-bit image. Without it a P010 frame reaches the GPU as the host engine's 8-bit conversion, as before |
| roll | `MEDIA_SENSOR_STABILIZE` now also turns the view with the fast part of the IMU's roll (picture coordinates, + clockwise on screen): `media_picture_t.view_roll`, `media_frame_telemetry_t.view_roll`, `media_gpu_image_t.roll` (the region given is the upright crop the turned view lies around) |
| `MEDIA_GPU_TEN_BIT`, `MEDIA_GPU_MICRO_FOLLOWED` | the image was converted from 10-bit samples; micro-motion's reference followed the view's and the scene's motion for it instead of starting over |
| `MEDIA_TELEMETRY_GROUND`, `horizon_y`, `focus_x` | perception found the camera travelling over a ground and took its flow out of looming ([picture](picture.md#the-fovea-and-the-ground-2026-10-27)) |
| `MEDIA_TELEMETRY_FOVEA` | the reported approach was found by the fovea's finer tiles (small or distant) |

**What changed underneath** ([picture](picture.md#roll-and-compensated-micro-motion-2026-10-27)):
- `video::Stabilizer` accumulates roll as it does yaw and pitch, low-passes it (0.35 s: a drone's banked turn passes through) and turns the view with the rest, at most 0.2 rad and only as far as the turned view's corners stay in the picture; the view's centre turns with the scene about the picture's centre.
- `render::LiveGpuPipeline`: the crop stays on even texels; the turn and the sub-texel rest of the view's position go to the conversion (`ColorConstants` `kRotate`, compiled into the picture-controls variants only), so the stabilised view is no longer quantised to 2 texels. Neural reconstruction is left out while the view is turned (edge-directed instead).
- Micro-motion's per-pixel reference is double-buffered and, whenever the view or the scene moved, read where each pixel's content was in the previous image: one affine map built from the previous view, the scene's motion from the frame's IMU deltas (`HostEngine::SceneMotion`) and this view. It starts over only on a new view size, when turned on, and per pixel for content from outside the previous view or a change beyond micro-motion's motion threshold (a cut). Where the reference has an edge or texture (its 2x2 neighbourhood spans more than 3 codes) the magnification fades to nothing at 6 codes: there a fraction of a pixel of residual misregistration would be magnified as a pulse.

**Verified** (`media_gpu_live_ingest_tests`, UHD 630, 320x240):
- Shaken camera (+-3 / 2.5 degrees yaw / pitch at 7 / 9 Hz, stabilised) with a still target pulsing +-2 codes: 27.0 codes peak to peak at 5x against 5.0 without (5.4x, as on a tripod); the reference followed the view on 49 of 49 images; a still ramp of 1 code per pixel next to it varies 2.08 codes against 1.75 without micro-motion; the content held to 2.03 camera px RMS. With the scene's motion missing from the map (a bug found by that ramp: perception consumes the host engine's IMU deltas) the ramp swung 81 codes.
- Ten-bit: a P010 ramp of one 10-bit code per column through shadow lift 1.0 rises on 159 of 159 columns by at most 0.88 code (`MEDIA_GPU_TEN_BIT`); through the 8-bit path it rises on 40 columns, by up to 4.0 codes.
- Roll +-5 degrees at 7 Hz with the IMU's deltas: a horizon's rows across the image 0.25 camera px RMS from level stabilised (the view turned up to 4.8 degrees), 4.28 px not.
- 40 P010 frames, rolling, micro-motion following, lift and neural asked for: **0 allocations** (and 40 NV12 frames with every pass, as before). The yaw / pitch stabilisation test: 2.08 camera px RMS (2.12 with the crop on even texels only).
- `media_c_api_tests` (Windows; Linux GCC -O2 and ASan/UBSan): a 0.03 rad roll turns the view by -0.0273 rad on the next frame (the low-pass's 1/30 s step taken out), `MEDIA_API_VERSION` 5; the approaching obstacle's time to contact 4.2 % off on average (8.6 % before the perception changes).

## The window thread, rotation and faster perception (2026-10-28)

Directive: "DEDICATED PRESENTATION THREAD (ELIMINATING WIN32 MESSAGE STALLS), PITCH-AWARE HORIZON GROUND MODEL + SIMD PERCEPTION (< 0.5 ms), & UNIFIED ARBITRARY/TKHD VIDEO ROTATION". No API change: `MEDIA_API_VERSION` stays 5.

- **Windowed mode** (`MEDIA_PLAYBACK_WINDOW`): the engine's window lives on a window thread of its own ([session](session.md#the-window-thread-2026-10-28)): a stalled message dispatch no longer stalls pictures, and input reaches the presentation thread within a fraction of a millisecond. Nothing changes for the host; the `on_frame` signatures and statistics are as before.
- **Rotation** ([picture](picture.md#rotation-2026-10-28)): windowed playback shows a file the way its `tkhd` matrix says (a portrait phone recording opens portrait), and `media_engine_command` takes "rotate video", "rotate left", "level horizon" and their translations ([intents](intents.md#rotation-2026-10-28)). Host mode answers them `MEDIA_ERROR_REFUSED` (the host draws the picture; `media_picture_t.view_roll` is the stabiliser's).
- **Perception** (host mode's live sensors, [picture](picture.md#pitch-crops-and-speed-in-perception-2026-10-28)): `MEDIA_TELEMETRY_GROUND` now finds the ground wherever the horizon is (a camera pitched up or down; the IMU's pitch tracks it) and never for a still camera; a 640x480 picture costs about 0.45-0.5 ms with AVX2 or SSE2 where most of it is untextured (1.6-1.9 ms when every tile is textured), every level bit-identical to the scalar loop.

**Verified** (`media_c_api_tests`, Windows; Linux GCC -O2 and ASan/UBSan): the live approach looms on 51 of 51 frames, TTC 4.2 % off on average, and no frame reports a ground (a still camera; before the coverage test, 36 did, and 5 approach frames were lost).

## Auto-leveling, the predicted search and the surf (2026-10-29)

The C API is unchanged (API 5). What changes for a host:
- **Perception** (host mode's live sensors, [picture](picture.md#the-predicted-search-the-surf-and-the-low-horizon-2026-10-29)): `MEDIA_TELEMETRY_LOOMING` now needs at least four accepted windows on the object, both axes expanding (the smaller at least half the larger), and the object found again within 0.15 s at the same place: the first frame of an approach is not reported, and water washing in, reflections and foam (one to three windows) no longer are (the beach footage: 33 false approaches in its high-horizon window, now 0). `MEDIA_TELEMETRY_GROUND` holds through frames that show only one to three rows of ground while those rows still fit it. The flow's search is seeded by prediction (the camera's rotation, the tile's last flow, its neighbours') with a diamond descent: a fully textured 640x480 frame costs 0.48 ms with AVX2 (1.6 before), every SIMD level giving the same telemetry bit for bit.
- **Auto-leveling** ([picture](picture.md#auto-leveling-the-horizon-2026-10-29)) is the windowed engine's: in windowed mode (`MEDIA_PLAYBACK_WINDOW`) Shift+R and `media_engine_command(engine, "auto level")` (or "level horizon", "יישר אופק אוטומטית", "niveler l'horizon") level the picture from the GPU's own measurement, 1,024 bytes of histograms a picture and no pixel read back. "level horizon" used to straighten: "straighten" does. Host mode refuses the commands like rotation (its host draws the picture); a host that has CPU pictures can call `video::HorizonLevel::Accumulate` and `Measure` itself (portable, not part of the C API).

**Verified** (`media_c_api_tests`, Windows; Linux GCC -O2 and ASan/UBSan): the live approach looms on 51 of 51 frames, TTC 4.2 % off on average, no ground for a still camera.

## Rotation, the horizon and black bars: API 6 (2026-10-30)

Directive: "C99 SDK API v6 (CPU/GPU HORIZON & ROTATION)". `MEDIA_API_VERSION 6u`. Host mode refused rotation and auto-leveling (its host draws the picture). Now the engine says how to draw it, and surveys the pictures it holds (its software bridge, live sensors) the way the windowed engine's statistics pass surveys its own: the horizon's edge-orientation histograms and the black bars' extents, on the CPU (`video::HorizonLevel`, `video::Matte`; [picture](picture.md#black-bars-perspective-and-real-footage-2026-10-30)), when telemetry, auto-leveling or auto-crop is on: about 0.9-1.1 ms a 640x480 to 1280x720 picture on one core. A host decoder's pictures (`MEDIA_PICTURE_HOST`) are not the engine's to survey.

```c
media_engine_set_auto_level(engine, 1);                /* or "level horizon", "יישר אופק אוטומטית" */
media_engine_command(engine, "recadrer les bandes noires"); /* or media_engine_set_auto_crop(engine, 1) */
media_engine_set_orientation(engine, 1, -2.5f);        /* a quarter turn clockwise, 2.5 degrees of level back; auto-leveling stops */
/* on_present: mirror if p->mirrored, turn p->quarter_turns clockwise, then by p->view_level radians zoomed by p->level_zoom. */
media_query_frame_telemetry(engine, &t);               /* MEDIA_TELEMETRY_HORIZON: t.estimated_horizon_deg; MEDIA_TELEMETRY_BARS: t.active_* */
media_engine_state(engine, &s);                        /* s.orientation: MEDIA_ORIENTATION_*, s.level_degrees */
```

| | |
|---|---|
| `media_picture_t` (appended; the engine fills, hosts read) | `quarter_turns`, `mirrored` (the file's `tkhd` and the viewer's turns), `view_level` (the leveling turn, radians, +: the picture turns clockwise), `level_zoom` (>= 1, keeps the turned picture's corners covered). The view (`view_zoom`, `view_x`, `view_y`) is of the stored picture, before the turns; `view_roll` stays the stabiliser's (the view's turn) |
| `media_frame_telemetry_t` (its last reserved word and new ones) | `estimated_horizon_deg` (`MEDIA_TELEMETRY_HORIZON`, 256: the median of the recent pictures' tilts, +: the right end lower), `active_left` .. `active_bottom` (`MEDIA_TELEMETRY_BARS`, 512: the picture inside bars; the whole picture otherwise), `level_degrees`. API 5's 112-byte struct is still accepted (`struct_size` says which; nothing past it is written). The query also answers once a picture has been surveyed without perception |
| `media_playback_state_t` (its two reserved words) | `orientation` (bits 0-1 the quarter turns shown, `MEDIA_ORIENTATION_MIRRORED`, `_AUTO_LEVEL`, `_AUTO_CROP`), `level_degrees` |
| new calls (host mode) | `media_engine_set_orientation` (quarter turns, -15..15 degrees in tenths; out of range `MEDIA_ERROR_INVALID_ARGUMENT`), `media_engine_set_auto_level`, `media_engine_set_auto_crop` |
| commands in host mode | "rotate video", "rotate left", "level horizon right", "straighten", "auto level", "crop black bars", "show black bars" and their translations now act (they answered `MEDIA_ERROR_REFUSED`); "zoom in" / "reset zoom" turn auto-crop off |
| `MEDIA_SENSOR_GPU` | the GPU image is leveled (the view turned by minus the level, zoomed to keep its corners) and cropped like the view; not quarter-turned (the host turns its image: the output size is fixed). Since API 7 it is quarter-turned too: see below |

The windowed engine (`MEDIA_PLAYBACK_WINDOW`) takes the same commands (and X for auto-crop); its telemetry query stays `MEDIA_ERROR_UNSUPPORTED`.

**Verified** (`media_c_api_tests`, Windows and Linux GCC -O2 and ASan/UBSan; a 640x480 camera rolled +4.5 degrees, its picture letterboxed 60 rows each side, auto-leveling by the call, auto-crop by "recadrer les bandes noires", 60 frames at 30/s): the horizon at +4.40 degrees, level -4.4, bars found at (0, 60)-(639, 419), the picture shown with view_level -4.40 degrees (level_zoom 1.099) and view zoom 1.352 at 0.499, state `AUTO_LEVEL | AUTO_CROP`, 0 allocations; API 5's struct accepted and nothing written past its 112 bytes (100 refused); "rotate video" then "סובב שמאלה" (rotate left) 1 then 0 quarter turns; `set_orientation(2, 3.0)` shown as such with auto-leveling off, 20 degrees refused; "straighten" and "show black bars" back to level 0, zoom 1. `media_gpu_live_ingest_tests` (Windows): the same tilted letterboxed horizon on the GPU with auto-crop: the image's horizon +4.49 degrees and bar-free edge rows; with auto-leveling too, -0.12.

## Several objects at once: API 7 (2026-10-31)

Directive: "MULTI-OBJECT THREAT & TARGET TRACKING RADAR (SDK API v7), SIMD CPU TELEMETRY SURVEY, 90° GPU LIVE ORIENTATION". `MEDIA_API_VERSION 7u`. Perception reported one approach: the strongest. A robot, a vehicle or a surgical camera sees more than one thing move at once. Perception now keeps a table of up to four objects with ids that stay with them from picture to picture ([picture](picture.md#several-objects-at-once-2026-10-31)), target tracking moves between them, and the audio zoom's beam goes with it.

```c
media_query_frame_telemetry(engine, &t);   /* MEDIA_TELEMETRY_OBJECTS: t.objects[0 .. t.object_count - 1], the most urgent first */
for (uint32_t i = 0; i < t.object_count; ++i) {
    const media_detected_object_t* o = &t.objects[i];
    /* o->track_id, o->x, o->y, o->width, o->height, o->velocity_x, o->velocity_y; MEDIA_OBJECT_APPROACHING: o->ttc_seconds */
}
media_engine_command(engine, "next target");          /* or "מטרה הבאה", "cible suivante": the next object in the table */
media_engine_track_object(engine, t.objects[1].track_id); /* or one by its id; MEDIA_TARGET_NEXT (0): as "next target" */
/* t.target_track_id: the object followed; its entry has MEDIA_OBJECT_TARGET. */
media_engine_set_orientation(engine, 1, 0.0f);        /* MEDIA_SENSOR_GPU: the GPU image a quarter turn clockwise */
```

| | |
|---|---|
| `media_detected_object_t` (new, 64 bytes) | `track_id` (1, 2, ...: kept while the object is seen; not reused while open), `flags` (`MEDIA_OBJECT_APPROACHING`: an approach confirmed on its track, `_FOVEA`, `_TARGET`), `x`, `y`, `width`, `height` (fractions of the picture), `velocity_x`, `velocity_y` (its track's, picture widths / heights per second), `ttc_seconds` (-1 unless approaching), `divergence`, `saliency` (its tiles' mean, 0..1), `energy` (its tiles' saliency added up), `support` (expansion windows on it), `tiles` (salient tiles), `age` (pictures seen) |
| `media_frame_telemetry_t` (appended) | `object_count`, `target_track_id`, `objects[MEDIA_MAX_OBJECTS]` (4), flag `MEDIA_TELEMETRY_OBJECTS` (1024). The order: confirmed approaches by time to contact (soonest first), then the rest by energy. API 5's 112-byte and API 6's 136-byte structs are still accepted (nothing past them is written) |
| new call (host mode) | `media_engine_track_object(engine, track_id)`: the view goes to the object at once (zoomed to 2x from a whole view), and with it the audio zoom's beam; the tracker locks onto it on the next picture. `MEDIA_TARGET_NEXT` (0): the object after the one followed, in the table's order (the first when none is: it starts tracking). `MEDIA_ERROR_EMPTY`: no such object in view; `MEDIA_ERROR_REFUSED`: "next" while the only object is the one followed |
| commands | `NextTarget` ("next target", "switch target", "מטרה הבאה", "החלף מטרה", "cible suivante", "siguiente objetivo", "nächstes Ziel", "следующая цель", "الهدف التالي", "下一个目标", "次のターゲット", ...; [intents](intents.md#next-target-2026-10-31)); in the windowed engine, Tab while tracking |
| `media_gpu_image_t` (its reserved words) | `quarter_turns`, `mirrored`: how the image was converted. The image is the view turned as the orientation shows it (the viewer's turns, `media_engine_set_orientation` or "rotate video"; mirrored first when the orientation is). Its size stays `output_width` x `output_height`: an odd turn converts a region of the camera of the output's turned shape (`output_height` : `output_width`), as large as the view holds, about the view's centre, so the turned picture fills the image undistorted (a 640x480 camera into a 640x480 image: its central 360x480 turned to 480x360 and scaled to fill). `left` .. `bottom` is that region, upright in the camera |
| `MEDIA_MODULE_TELEMETRY` | its region is 448 KiB (perception for the windowed engine's Tab; the host engine's perception, as before, is in the sensor's own arena) |

The host survey (`MEDIA_TELEMETRY_HORIZON`, `_BARS`, auto-leveling and auto-crop) is about ten times cheaper: 0.10 ms a 640x480 picture with AVX2, 0.17 with SSE2 (1.09 before), the bars' extents surveyed every fourth picture once known, as the GPU's statistics pass does ([picture](picture.md#the-cpu-survey-2026-10-31)).

**Verified** (`media_multi_target_tests`, new: Windows and Linux GCC -O2 and ASan/UBSan). Two textured squares approaching a still 640x480 camera at once over a textured background (A at (0.27, 0.40), 3 m away at 1.0 m/s, 32 px half size at 3 m; B at (0.73, 0.60), 4 m at 0.9 m/s, 45 px at 4 m: larger on screen, later to arrive), and a stereo microphone pair (a 500 Hz tone on A's side, the left channel; 1300 Hz on B's). Perception alone: both in the table on 54 of 54 pictures from 0.4 s, A keeping id 2 and B id 1 on 54 of 54, both approaches confirmed on 26 pictures and A first on all 26, time to contact within 3.3 % (A) and 7.1 % (B) median, no object off the squares, 435 µs a picture, 0 allocations. Through the SDK, the audio zoom on: "next target" -> A (the view at 0.257, the target id A's on 8 of 8 pictures, `MEDIA_OBJECT_TARGET` on its entry; A's tone over B's +4.1 dB), "next target" -> B (0.711; -6.4 dB), "מטרה הבאה" -> A (0.275; +3.7 dB), `media_engine_track_object(B)` -> B (0.721; -6.6 dB): the beam swings by 10 dB; 0 allocations while ingesting, stepping, pumping, querying and commanding. 14 phrasings in nine languages parse to `NextTarget`; "next one" stays `NextMatch`. The GPU image (Windows): a bright square at (220, 120) of a 640x480 camera found at (220.0, 120.0), (480.0, 106.5), (420.0, 360.0) and (160.0, 373.5) for 0 to 3 quarter turns (expected (220, 120), (480, 106.7), (420, 360), (160, 373.3)); the region 640x480 and 360x480; the corners covered. `media_c_api_tests`: API 7's version, API 6's 136-byte struct accepted and nothing written past it, an unknown id `MEDIA_ERROR_EMPTY`.

## Objects that cross, fit and snapshots: API 8 (2026-11-01)

Directive: "MILESTONE 200/200: OCCLUSION/CROSSING-SAFE MULTI-OBJECT TRACKING, INSTANT `T -> Tab` TARGET SEEDING, 90° GPU FIT/FILL MODE & FORENSIC FRAME SNAPSHOT". `MEDIA_API_VERSION 8u`. Everything of API 7 is unchanged; what is new:

- **Objects keep their ids when they cross** ([picture](picture.md#objects-that-cross-2026-11-01)): two objects passing each other, or one behind the other, come out of it with the ids they went in with. An object hidden behind another is still in the table, where its motion puts it: `MEDIA_OBJECT_OCCLUDED` (8) on its entry, `tiles` 0. `velocity_x` / `velocity_y` are now each track's line through its recent centres (within 7 to 18 % of the truth, as a vector, on the test scenes), and a new track's velocity is its flow from its first picture.
- **"track the target", then "next target" at once**: the object at the lock point is the target as soon as tracking is asked for (`target_track_id` right after the command), so "next target" on the very next picture moves on to the other object.

### Fit instead of fill (API 8, 2026-11-01)

```c
sensor.options = MEDIA_SENSOR_GPU | MEDIA_SENSOR_GPU_FIT;   /* the view whole, black bars where it does not reach */
media_engine_set_orientation(engine, 1, 0.0f);             /* 90 degrees: a 640x480 camera into a 640x480 image */
media_query_gpu_image(engine, &image);                     /* MEDIA_GPU_FITTED; the picture in [140, 500) x [0, 480) */
```

| | |
|---|---|
| `MEDIA_SENSOR_GPU_FIT` (8, with `MEDIA_SENSOR_GPU`) | the GPU image shows the view's whole region, turned, scaled to fit (one scale both ways, centred), black around it (every byte 0). Without it an odd turn fills the image with a region of the output's turned shape and leaves the view's ends out. The same at 0 or 180 degrees when the image's shape is not the view's |
| `media_gpu_image_t` (appended) | `picture_left`, `picture_top`, `picture_right`, `picture_bottom`: where the picture lies in the image (inside the bars with `MEDIA_GPU_FITTED` (128), the whole image otherwise). API 7's struct (through `reserved`) is still accepted: nothing is written past it |

### Snapshots (API 8, 2026-11-01)

```c
media_engine_set_snapshot_folder(engine, "/data/evidence");  /* optional: where snapshots without a path go */
media_engine_save_snapshot(engine, "case-17/frame.bmp");     /* or null: a free name from the media and the picture's time */
media_engine_command(engine, "take snapshot");               /* or "צילום מסך", "capturer l'image", ...; S in the windowed engine */
media_snapshot_t s = {sizeof(s)};
if (media_query_snapshot(engine, &s) == MEDIA_OK && s.status == MEDIA_OK) { /* s.path, s.width x s.height, s.pts_100ns, s.frame_hash */ }
```

The picture as the engine made it, as an uncompressed 24-bit BMP (top-down rows, no compression) with a sidecar of its telemetry beside it (the same name with `.json`): the image's size and hash, the source, the media, the picture's time, the view (zoom, centre, roll, the region shown), the orientation (turns, mirror, leveling, auto-leveling, fit), the horizon, the picture controls, the reconstruction, the comparison, the target and the objects perceived ([picture](picture.md#forensic-snapshots-2026-11-01)). `frame_hash` is FNV-1a 64 over the image's R G B bytes, rows top to bottom (no padding, no alpha): a reader of the file, or of `media_gpu_image_t.rgba`, computes the same. No allocation; the frames keep coming.

| Where | What is written | When |
|---|---|---|
| host mode, `MEDIA_SENSOR_GPU` | the newest GPU image exactly (zoom, pan, tracking, stabilisation, turns, fit, leveling, the picture controls, micro-motion, super-resolution); source `MEDIA_SNAPSHOT_GPU_IMAGE` | during the call, on the host's thread |
| host mode otherwise | the view of the picture the engine holds (its software bridge's, a camera's): the view's region turned and mirrored as shown, converted to RGB as the GPU converts (BT.709, limited range), sample for sample; the leveling turn and the roll are the host's to draw (the sidecar says them); `MEDIA_SNAPSHOT_VIEW`. `MEDIA_ERROR_UNSUPPORTED` while the host's decoder holds the pictures | during the call |
| the windowed engine | the picture as composed on screen (zoom, tracking, leveling, the turns, Original \| Enhanced, Pinned \| Live), before captions, the dock and the cues; `MEDIA_SNAPSHOT_WINDOW` | copied by the next composite, written by the engine's snapshot thread: the call returns at once (`MEDIA_OK`: on its way; `MEDIA_ERROR_STATE`: one is on its way already), `media_query_snapshot` says when it is written (`MEDIA_ERROR_EMPTY` until then) |

`path` is the BMP's (UTF-8; its folder must exist; replaced when it exists). Without one, the name is the media's stem and the picture's time (`clip_0h01m02.345s.bmp`, `sensor_...` for a camera; `_2` and on for a name taken: nothing is replaced) in the snapshot folder: `media_engine_set_snapshot_folder`'s, by default `Pictures\MediaEngine` in the user's profile on Windows (made when missing) and the working directory elsewhere. `media_snapshot_t` (`struct_size`, `sequence` (written since open, this one's number), `status`, `source`, `width`, `height`, `pts_100ns`, `frame_hash`, `path[512]`) is the last one written or failed. A command ("take snapshot", ...) in host mode writes it before `media_engine_command` returns (its status), a spoken one at the next `media_engine_step`. The windowed engine's snapshot thread counts in `media_engine_stats_t.threads`, as does its perception thread (the telemetry module's: `MEDIA_MODULE_TELEMETRY` was 576 KiB and 1 thread then, 3,392 KiB since API 10; perception on the presentation thread dropped pictures in Debug builds).

**Verified** (`media_snapshot_tests`, suite #200: Windows and Linux GCC -O2 and ASan/UBSan; `media_multi_target_tests`). The encoder and the sidecar alone (the header, every pixel back, the hash; well-formed JSON with 27 fields as given; 0 allocations). Host mode, a camera, 2x zoom and a quarter turn: a 240x320 image, every pixel the stored sample converted apart (0 codes off); its hash `media_query_snapshot`'s and the sidecar's; the sidecar's objects the telemetry's; "take snapshot" to a free name; 0 allocations while saving. With the GPU path (shadow lift 0.5, 2x zoom, 3° leveling): every byte the GPU image's, the same hash. The windowed engine, hidden, on the two-targets fixture: "take snapshot" and `media_engine_save_snapshot`, both written while playing, a second "take snapshot" at once refused, 0 dropped, 0 allocations in the commands. The fit: all four corners of the camera in the 90° and 270° images (0.0 px off), the bars black, API 7's struct still answered. Crossing: both ids kept through an X (one behind the other) and a side-by-side pass. T then "next target": the other object at once.

## Remote vibrometry, the event camera, the bitstream and tensors: API 9 (2026-11-02)

Directive: "PROJECT BIONIC CORTEX (SUITES 201–204)". `MEDIA_API_VERSION 9u`. API 8 was taken by the snapshots (suite #200), so these additions are API 9. Everything of API 8 is unchanged; what is new:

### Remote vibrometry (API 9)

```c
media_frame_telemetry_t t = {sizeof(t)};
media_query_frame_telemetry(engine, &t);
if (t.flags & MEDIA_TELEMETRY_VITALS)    { /* t.vital_bpm (45..210), t.vital_confidence (0..1) */ }
if (t.flags & MEDIA_TELEMETRY_VIBRATION) { /* t.vibration_hz (0.7..15), t.vibrometry_confidence, t.vibration_amplitude */ }
```

With `MEDIA_MODULE_TELEMETRY`, every picture the engine holds (its software bridge's, a camera's) is measured over a region held still in the scene: the target's while tracking, else the view's fovea, steadied against the camera's shake (the IMU's). The last 256 pictures' brightness, colour and the content's own sub-sample displacement are transformed at their own times ([picture](picture.md#remote-vibrometry-2026-11-02)).

| `media_frame_telemetry_t` (appended, 48 bytes) | |
|---|---|
| `vibration_hz`, `vibrometry_confidence`, `vibration_amplitude`, `vibrometry_channel` | The strongest periodic component between 0.7 Hz and min(15 Hz, 0.45 x the picture rate), its confidence (0: noise or a still scene), its amplitude (codes, or samples of displacement) and where it was found (`MEDIA_VIBROMETRY_LUMA`, `_CB`, `_CR`, `_SHIFT_X`, `_SHIFT_Y`). `-1` Hz: none. |
| `vital_bpm`, `vital_confidence`, `vital_channel` | The strongest component in 0.75–3.5 Hz, x 60: a pulse (remote photoplethysmography, or the head's motion with each beat). A stronger component's harmonics folded at the picture rate are not taken for one. `-1`: none. |
| `vibrometry_seconds`, `vibrometry_left` .. `vibrometry_bottom` | The window's span (nothing is reported before 2.5 s) and the region measured, in samples. |
| flags | `MEDIA_TELEMETRY_VIBRATION` (2048), `MEDIA_TELEMETRY_VITALS` (4096). |

API 7's and 8's struct (400 bytes, through the objects) is still accepted, with nothing written past it, as are API 6's and API 5's. The windowed engine shows the readout on its HUD ("Pulse 72 BPM") while tracking or with micro-motion on.

### The event camera (API 9)

```c
media_event_camera_config_t c; media_event_camera_config_init(&c);      /* C 0.15, 4 events a pixel a picture, stabilised */
media_engine_set_event_camera(engine, &c);                              /* null: off */
media_event_spike_t spikes[4096]; uint32_t n = 0;
while (media_poll_event_spikes(engine, spikes, 4096, &n) == MEDIA_OK) { /* x, y, polarity (+1 ON / -1 OFF), contrast, timestamp_us */ }
```

The pictures the engine holds become a dynamic vision sensor's events. A sensor of at most 640 x 480 (the picture box-averaged; `media_query_event_camera` says its size and scale) fires an ON or OFF event each time a pixel's log intensity moves by the threshold. The crossings between two pictures are spread over the interval in time order. Stabilised by default: the IMU's motion is taken out, so a shaking or turning camera fires nothing, and x, y are the scene's (re-based after a pan of 6 pixels). `MEDIA_EVENTS_UNSTABILIZED` compares pixel for pixel. The ring holds 65,536 events: the host polls from any one thread (single consumer, as `media_engine_poll_frame`). What does not fit is counted in `dropped`. It is made at open with telemetry (~3.5 MiB at most) and fills only while it is on. `MEDIA_ERROR_UNSUPPORTED` without such pictures ([picture](picture.md#event-camera-2026-11-02)); in the windowed engine until API 10.

### Bitstream perception (API 9)

```c
static media_bitstream_inspector_t inspector;                           /* 10 KiB, the caller's */
media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, unit->config, unit->config_bytes, unit->nal_length_bytes);
media_bitstream_info_t info = {sizeof(info)};
if (media_inspect_bitstream_packet(&inspector, unit->data, unit->bytes, &info) == MEDIA_OK) {
    if (info.flags & MEDIA_BITSTREAM_SCENE_CUT) { /* likely a new shot: decode, index, wake the robot */ }
    if (info.flags & MEDIA_BITSTREAM_REVISION)  { /* unit info.revised_index's cut is now info.revised_scene_cut_probability */ }
}
```

No engine needed: a host scanning recordings calls it per access unit (as stored: length-prefixed NAL units, decode order). One that decodes only when something happens calls it from its `decode_video` callback with the `media_video_unit_t` it was given. It returns:
- the picture's type and key flag;
- its QP (H.264 and HEVC);
- its complexity: bits per pixel at QP 26;
- its activity: new detail against the recent intra pictures, 0 still .. 1 all new;
- its burst against its kind's mean;
- a scene-cut probability.

An inter picture's cut is confirmed or withdrawn by the next picture of its kind, which says so (`MEDIA_BITSTREAM_REVISION`, `revised_index`, `revised_scene_cut_probability`). `MEDIA_ERROR_EMPTY`: a unit without a picture (parameter sets, SEI). About 0.1–0.4 µs a picture, nothing allocated ([picture](picture.md#bitstream-perception-2026-11-02)).

### Tensor export (API 9)

```c
media_tensor_request_t r; media_tensor_request_init(&r);                /* 224 x 224, R G B, float32 [0, 1], the fovea */
r.format = MEDIA_TENSOR_F16; r.normalize = MEDIA_TENSOR_IMAGENET;
static uint16_t tensor[3 * 224 * 224];                                   /* media_tensor_bytes(&r) */
media_tensor_info_t info = {sizeof(info)};
media_export_foveated_tensor(engine, &r, tensor, sizeof(tensor), &info); /* NCHW, written during the call */
```

| | |
|---|---|
| `width`, `height`, `channels` | 1..8192 each; 3 (R G B planes, or B G R with `MEDIA_TENSOR_BGR`) or 1 (Y'). |
| `format` | `MEDIA_TENSOR_F32` or `MEDIA_TENSOR_F16` (the float32 value rounded to nearest even). |
| `normalize` | `MEDIA_TENSOR_UNIT` [0, 1]; `MEDIA_TENSOR_IMAGENET` (v − 0.485 0.456 0.406) / 0.229 0.224 0.225 (grey 0.449 / 0.226); `MEDIA_TENSOR_SIGNED` [−1, 1]. |
| `region` | `MEDIA_TENSOR_FOVEA`: the tracked object's box grown by a fifth, else the view's fovea, shaped as the tensor. `MEDIA_TENSOR_VIEW`: the view shown. `MEDIA_TENSOR_FULL`: the picture. `MEDIA_TENSOR_REGION`: `left` .. `bottom` in fractions. |
| `media_tensor_info_t` | The region taken, in samples; the picture's time; the bytes written; the target's id when the fovea was the target. |

Each element is its cell of the region, area-averaged on each plane, converted from BT.709 limited range to R'G'B' in [0, 1], then normalised, in double ([picture](picture.md#tensor-export-2026-11-02)). Errors: `MEDIA_ERROR_STORAGE` when `bytes` is too small; `MEDIA_ERROR_EMPTY` before a picture; `MEDIA_ERROR_UNSUPPORTED` while the host's decoder holds the pictures (and in the windowed engine until API 10).

**Verified** (suites #201–#204: Windows MSVC Release and Debug, Linux GCC -O2 and ASan/UBSan; `media_c_api_tests` compiles the new structs and calls as strict C99): see the tables in [picture](picture.md#remote-vibrometry-2026-11-02).

**Also (2026-11-02).**
- The windowed engine's snapshot readback is made when a snapshot is asked for, of the window's size, by the snapshot thread, and released once the files are written. Playing holds none; it held the slots' capacity before, 33 MB for a 4K file.
- The pinned A–B turn's landing is copied ahead of B ([seeking](seeking.md#staged-landings-2026-11-02)).

## Moving targets, the virtual IMU and the window's sensing: API 10 (2026-11-03)

Directive: "BIONIC CORTEX COMPLETENESS". `MEDIA_API_VERSION 10u`. Everything of API 9 is unchanged (its 448-byte telemetry struct is still accepted); what is new or newly allowed:

| | |
|---|---|
| `media_frame_telemetry_t` (appended, 24 bytes) | `vibrometry_flags` (`MEDIA_VIBROMETRY_FOLLOWING`: the region follows the tracked target's path, so its window goes on while the target moves); `ego_dx`, `ego_dy` (the virtual IMU's measure of the scene's motion since the previous picture, samples) and `ego_turn` (radians, + clockwise); `ego_tiles`, `ego_inliers` (its tiles measured, the background's among them). |
| `MEDIA_TELEMETRY_VIRTUAL_IMU` (8192) | The picture came without IMU deltas and the camera's motion is the virtual IMU's. Perception's flow, stabilisation, vibrometry's fovea and the event camera took it as an IMU's ([picture](picture.md#the-virtual-imu-2026-11-03)). |
| `media_engine_set_stabilization` | Also for frames without `MEDIA_SENSOR_IMU` and for the software bridge's files (with `MEDIA_MODULE_TELEMETRY`): steadied by the virtual IMU. `MEDIA_ERROR_UNSUPPORTED` only when the host's decoder holds the pictures. |
| `media_engine_set_event_camera`, `media_poll_event_spikes`, `media_query_event_camera` | The windowed engine too: its sensor is the tracking grid (at most 480 x 272 cells; `media_query_event_camera` says its size), read back for every picture while it is on, stabilised by the virtual IMU on the grids ([picture](picture.md#the-event-camera-and-tensors-in-the-window-2026-11-03)). |
| `media_export_foveated_tensor` | The windowed engine too: the region of the next picture it converts, read on the GPU (each element's Y', Cb and Cr area means) and converted on the calling thread, at most 1024 x 1024 elements, one call at a time. It waits for that picture (at most a second: `MEDIA_ERROR_EMPTY` while paused). An HDR picture's coded values are taken as BT.709. |
| `MEDIA_MODULE_TELEMETRY` | 3,392 KiB (was 576 KiB): the window's event camera (2.09 MB, its 1 MiB ring included) and its virtual IMU (480 KiB) on the grids. 1,408 KiB since API 11. |

Inside the engines, and with no API of its own: remote vibrometry follows a moving target ([picture](picture.md#moving-targets-2026-11-03)); the windowed engine measures Y', Cb and Cr at full resolution on the GPU ([picture](picture.md#vibrometry-in-the-window-2026-11-03)); and the bitstream inspector keeps every HEVC SPS's reference picture sets, so a stream that switches between sequences has every slice's QP ([picture](picture.md#several-sequences-in-one-bitstream-2026-11-03)).

**Verified**: `media_c_api_tests` (API 10 from strict C99: the struct's size, API 9's 448 bytes accepted with nothing written past them, the new flags); `media_vibrometry_tests` (a moving target tracked through the SDK, a shaken clip without IMU deltas); `media_neuromorphic_spike_tests` and `media_tensor_export_tests` (the windowed engine, Windows); `media_bitstream_perception_tests` (two sequences); `media_playback_vibrometry_chroma_test`.

## The window's stabilisation, tensors while paused and HDR tensors: API 11 (2026-11-04)

Directive: "WINDOWED VIRTUAL-IMU VIDEO STABILIZATION, PAUSED & HDR-CORRECT WINDOWED TENSOR EXPORT, TELEMETRY MEMORY DIET". `MEDIA_API_VERSION 11u`. Structs keep their sizes; what is new or newly allowed:

| | |
|---|---|
| `media_engine_set_stabilization` | The windowed engine too: its view, each picture measured by the virtual IMU before it is converted ([picture](picture.md#stabilising-the-window-2026-11-04)). Any thread after open. `MEDIA_ERROR_UNSUPPORTED` without `MEDIA_MODULE_TELEMETRY` or under 128 samples a side. |
| `media_export_foveated_tensor` | While paused, the picture on screen ([picture](picture.md#tensors-while-paused-2026-11-04)): from its held source, or after one decode of it, which holds it from then on; a host that has exported before has it held from the start of each pause. `MEDIA_ERROR_EMPTY` now only before the first picture. A PQ or HLG picture's elements are what the screen shows, in [0, 1] ([picture](picture.md#hdr-tensors-2026-11-04)). |
| `media_tensor_info_t.flags` | Was `reserved`. `MEDIA_TENSOR_TONE_MAPPED` (1): an HDR picture, tone-mapped. `MEDIA_TENSOR_HELD` (2): from the held source, nothing decoded for this call. |
| `MEDIA_MODULE_TELEMETRY` | 1,408 KiB (was 3,392 KiB; 1,372 KiB used at the largest grid). The window's event ring holds 32,768 events (was 65,536): poll it at least every 32,768 events; `dropped` counts the rest ([picture](picture.md#the-telemetry-modules-memory-2026-11-04)). Host mode's stays 65,536. |

`video::Stabilizer`'s low-pass is now second order at the same delay, so host mode's stabilised view keeps less of the shake (7.8 % of 2.3 Hz, was 19.4 %) with the same lag on a pan.

**Verified**: `media_playback_stabilize_test` (the view, on screen, through a command and Alt+S); `media_tensor_export_tests` (paused against host mode and a snapshot of the screen; the HDR arithmetic; a PQ fixture against the screen; the SDK's stabilisation call); `media_neuromorphic_spike_tests` (the packed ring, the region's size); `media_vibrometry_tests` (decode order, the low-pass); `media_c_api_tests` (API 11 from strict C99).

## Embedding: API 12 (2026-11-03)

Directive: build the engine so that developers put it inside their own product, with their own interface: "how do the other engines do it? The goal is that people connect this engine to their system, with their own design". `MEDIA_API_VERSION 12u`. The established engines all follow the same three rules, and AVION now does too:

| | libmpv | libVLC | GStreamer | AVION 12 |
|---|---|---|---|---|
| Plays inside the host's window | `wid` | `libvlc_media_player_set_hwnd` | `GstVideoOverlay` | `media_engine_config_t.parent_window` |
| Commands from the host's thread, events back | `mpv_command_async`, `mpv_wait_event` | events, any thread | the bus | `media_engine_post_command`, `media_engine_poll_event` |
| A dynamic library with a C ABI | libmpv | libvlc | libgstreamer | `avion.dll` |

**Playing inside a window of the host's** (`parent_window`, an HWND; the windowed engine only):
- The engine's window is a borderless child of it (`WS_CHILD`, `WS_EX_NOPARENTNOTIFY`), filling its client area. It follows the parent's size through a WinEvent hook (`EVENT_OBJECT_LOCATIONCHANGE`, out of context) on the engine's window thread, so nothing is polled: the child's `WM_SIZE` resizes the swap chain as a user's drag does.
- The child takes the parent's DPI awareness, so a DPI-unaware host (WinForms by default) can parent it.
- `WS_EX_NOPARENTNOTIFY` keeps creating the child from waiting for the host's thread. Without it, a host whose UI thread waits for `open` without pumping held `open` for 10 s, then failed (found by the Python sample; the C test now opens with its UI thread asleep).
- The host's design is the host's: leave `MEDIA_MODULE_HUD` out and the engine draws no control. Escape no longer stops playback. F, F11 and Alt+Enter become a `MEDIA_EVENT_FULLSCREEN_REQUEST` (the host fills its own window). A click on the picture gives it the keyboard, so the engine's shortcuts work on it.
- Destroying the host's window takes the child with it and ends `media_engine_run` (`MEDIA_EVENT_ENDED`, `MEDIA_ENDED_CLOSED`).

**Commands and events** (lock-free single-producer, single-consumer rings in the engine's storage; no lock, no allocation):

| Function | Thread | |
|---|---|---|
| `media_engine_post_command(engine, utf8, id)` | one producer (the host's UI thread) | a command of the [grammar](intents.md), at most 247 bytes; the queue holds 32 (`MEDIA_ERROR_REFUSED` when full). The engine's thread is woken (`WakePresenter`), paused and settled too, so a command is applied at once |
| `media_engine_poll_event(engine, &event)` | one consumer | the oldest `media_event_t` (80 bytes); readable after close until the next open, `MEDIA_EVENT_ENDED` the last; the ring holds 256, `lost` counts any dropped |
| `media_engine_state(engine, &state)` | any | the windowed engine too: position, duration, paused, muted, volume, speed, presented, dropped, as of the engine's last pass |
| `media_api_version()` | any | 12: for a binding to check the library against the header it was written for |

| Event | When | Fields |
|---|---|---|
| `MEDIA_EVENT_OPENED` | playback starts | `duration_100ns`, `width` x `height` (the picture as displayed) |
| `MEDIA_EVENT_STATE` | paused, muted, volume or speed changed | all four |
| `MEDIA_EVENT_POSITION` | every 250 ms of playing, and on pausing | `position_100ns` |
| `MEDIA_EVENT_SEEK_LANDED` | a seek's first picture on screen | its pts, `latency_100ns` |
| `MEDIA_EVENT_COMMAND` | a posted command ran | `command_id`, `status` (`MEDIA_OK`, `MEDIA_ERROR_REFUSED`) |
| `MEDIA_EVENT_FULLSCREEN_REQUEST` | F, F11, Alt+Enter on an embedded picture | |
| `MEDIA_EVENT_ENDED` | `media_engine_run` returns | `reason`: `MEDIA_ENDED_MEDIA`, `_STOPPED`, `_CLOSED`, `_ERROR`, `_LIMIT`; `status` |

Host mode: `post_command` is drained by the next `media_engine_step`, each command answered by its `MEDIA_EVENT_COMMAND` (host mode's only event: the host steps, it knows the rest). API 11's `media_engine_config_t` (through `decoder_context`) is still accepted, the new fields taken as zero. A parent window in host mode is `MEDIA_ERROR_INVALID_ARGUMENT`.

**`avion.dll`** (`avion.lib` to link it): every function `c_api.h` declares, exported by a module-definition file that CMake writes from the header at configure time (the export list cannot fall behind it). The C runtime is linked in (`/MT`): the DLL depends on Windows' own libraries only (kernel32, user32, gdi32, imm32, d3d12, dxgi, dwmapi, mfplat, mmdevapi, dwrite, ole32, winhttp), no redistributable to install. No ownership crosses the boundary (the host gives the storage, the engine never returns memory to free), so each module having its own heap is harmless. x64 layouts the bindings rely on, asserted by `media_embed_tests`: `media_engine_config_t` 176 bytes (`parent_window` at 152), `media_event_t` 80, `media_playback_state_t` 88.

**Samples** (`samples/`, shipped in the bundle):
- `samples/csharp/AvionPlayer`: a .NET 8 WinForms player whose controls are all its own (a drawn seek bar and volume, Segoe Fluent icons, drag and drop, fullscreen on request), AVION inside its video panel, through P/Invoke with explicitly laid-out structs (`Avion.cs`, about 170 lines). `--capture shot.png` places it off the screens, plays muted, writes its own window (`PrintWindow`) and exits.
- `samples/python/avion_player.py`: the same in tkinter with ctypes, about 150 lines. `--smoke` plays a file muted off the screens, pauses and resumes through the queue, prints every event and exits 0 at the end.

**Verified** (`media_embed_tests`, strict C99; and `media_embed_dll_tests`, the same program linked with `avion.dll`). A host window and a UI thread of its own, the engine on another thread, Release:
- the engine's window a child of the host's, 640x360 in 640x360; the host resized to 960x540, the child 960x540 within 400 ms;
- position events advancing while playing; 0 allocations in the whole process over 500 ms of embedded playback, the engine's threads included (in the DLL build the count sees only the program's own heap);
- "pause" from the UI thread; "volume down" while paused and settled answered in **0.04-0.09 ms** (the engine's thread woken, not polled); a command the grammar refuses answered `MEDIA_ERROR_REFUSED`; Escape ignored; Alt+Enter a fullscreen request, the child's size unchanged; `media_engine_state` from the UI thread;
- played to the end: `MEDIA_ENDED_MEDIA` at 3.01 s; the host's window destroyed while playing: `MEDIA_ENDED_CLOSED` 2-5 ms later; `media_engine_stop`: `MEDIA_ENDED_STOPPED` 2-3 ms later; opened in 263-286 ms with the UI thread pumping nothing;
- API 11's config size accepted; a parent in host mode refused; host mode's posted commands run by `step` with their events; the x64 layouts.

The C# sample was run on `big_buck_bunny_4k60.mp4` (4K HEVC, the hardware decoder): "go to 1:42" landed, muted from the start, its bar and the engine's state in agreement. The Python sample's smoke run printed OPENED, the commands, STATE, POSITION every 0.25 s, the pause and resume, and ENDED at 3.01 s.

**Not covered.** One engine per parent window. Rendering into a host's own Direct3D texture (for game engines and compositors such as Unity, Unreal, WinUI) came next, in API 13; hardware decoding in host mode is still to come. The window and the DLL are Windows only; host mode, the queue and the events are portable.

## Texture output: API 13 (2026-11-04)

Next step of the embedding work: a game engine, a compositor or a 3D scene does not give the player a window; it wants the pictures as textures it samples itself (a screen in a scene, a video wall, a WinUI `SwapChainPanel` composed with its own visuals). libmpv answers with its render API (`mpv_render_context`, OpenGL), libVLC with its output callbacks (`libvlc_video_set_output_callbacks`, the host's D3D11 device drawing for it). AVION keeps its own Direct3D 12 timeline and hands the host **shared textures and shared fences**: nothing is copied through the CPU, no thread of either side waits for the other's GPU, and the host uses any API that opens NT handles (Direct3D 11 or 12, Vulkan through `VK_KHR_external_memory_win32`). `MEDIA_API_VERSION 13u`; API 12's calls and struct unchanged (the new config fields were its reserved bytes, so it is still 176 bytes).

**Configuring** (`media_engine_config_t`, the windowed engine): `output_flags = MEDIA_OUTPUT_TEXTURE`, `output_width` x `output_height` (16..8192). There is no window on the screen: every picture, with its subtitles (and the engine's controls unless `MEDIA_MODULE_HUD` is left out), is drawn, fitted into that size, into one of **three textures** (`R8G8B8A8_UNORM`, committed on a shared heap, in the common state between the engine's draws). Not with `parent_window`, not in host mode (both `MEDIA_ERROR_INVALID_ARGUMENT`); an unknown flag is refused too.

| Function | | |
|---|---|---|
| `media_engine_query_output_textures(engine, &t)` | after open; again whenever a frame's `generation` differs | the three textures' NT handles, the engine's fence and the host's fence (NT handles), their size, format 28, `generation`, the adapter's LUID (create the host's device on it). `MEDIA_ERROR_EMPTY` while the textures are being made |
| `media_engine_acquire_output_frame(engine, &f)` | the host's render thread, once per host frame | the newest picture not yet taken: `index`, `fence_value`, `pts_100ns`, `generation`, `sequence`. `MEDIA_ERROR_EMPTY` when there is nothing newer (keep drawing the one held) |
| `media_engine_release_output_frame(engine, index, host_value)` | when the host's reads of it are queued | hands it back: the engine's queue waits (on the GPU) for the host's fence to reach `host_value` before drawing into it again |
| `media_engine_resize_output(engine, w, h)` | any time | new textures of that size, a new `generation`; the old ones are gone (a frame of the old generation needs no release) |

**A host frame** (Direct3D 11; with D3D12 the same through `OpenSharedHandle` and `ID3D12CommandQueue::Wait/Signal`):

```c
media_output_frame_t f = { sizeof f };
if (media_engine_acquire_output_frame(engine, &f) == MEDIA_OK) {
    if (f.generation != generation) reopen();               /* media_engine_query_output_textures, OpenSharedResource1 */
    context4->Wait(engine_fence, f.fence_value);             /* GPU wait: the engine's drawing of it is done */
    held = f.index;                                          /* sample textures[held] from here on */
    context4->Signal(host_fence, ++host_value);              /* after the last draw that reads the previous one */
    media_engine_release_output_frame(engine, previous, host_value);
}
```

The textures' states (free, published, held, drawn) are atomics with compare-and-swap; the newest picture is one 64-bit word (`fence value << 2 | index`). Holding one texture leaves the engine two: it draws into one while the other waits as the newest, and a newer picture replaces a published one the host never took (it is free again at once). A frame's fence value is read from its texture's own slot, not from that word: between the host's load of the word and its claim, the engine may have freed, redrawn and republished the same texture. Nothing here takes a lock or allocates, on either side.

**Verified** (`media_texture_output_tests`, Release): a Direct3D 11 device of the test's own on the adapter the LUID names opens the three textures and both fences, and plays the chapter fixture (9 s at 24 fps, scenes of dark, bright and mid luma with a 0-23 pattern, cuts at 3 s and 6 s) into 320 x 180 textures, copying each frame to a staging texture to read it:
- **218 frames** reached the host's texture in 9.0 s, sequences and fence values always rising;
- the mean BT.601 luma of the centre 32 x 32 of each copy: dark **17-34**, bright **209-228**, mid **117-127** (expected 23, 223 and 123 on average; none outside ±20), and the side bars black (the 64 x 64 picture fitted 1:1 into 16:9);
- `media_engine_resize_output(256, 144)` at 4.5 s: generation 1 became 3 (old textures released, new made), the new handles 256 x 144, **107 frames** after it;
- **0 allocations** in the process over a second of the host's acquire, wait, copy, signal and release (the engine's threads included);
- the configuration refusals above, and every call refused before open (`MEDIA_ERROR_STATE`) or with a wrong `struct_size`.

**Not covered.** No Unity or Unreal plug-in yet: this is the interface one is written on (Unity's `IUnityGraphicsD3D11`/`D3D12` device, `Texture2D.CreateExternalTexture`; Unreal's RHI), not the plug-in itself. HDR pictures are tone-mapped into the 8-bit textures (no FP16 output). The host's device must be on the engine's adapter. Hardware decoding in host mode is still to come. *(The last three: API 14, below.)*

## API 14: a Direct3D 12 host, HDR textures, hardware decoding in host mode, packages, the licence (2026-11-04)

`MEDIA_API_VERSION 14u`. API 13's structs are still accepted (`media_output_textures_t` 72 bytes, `media_output_frame_t` 32); the config stays 176 bytes (`output_format` was API 13's reserved field).

### A Direct3D 12 host (verified)

API 13 claimed "Direct3D 11 or 12" and tested 11. `media_texture_output_tests --d3d12` is a Direct3D 12 host of the test's own, on the adapter the LUID names: `ID3D12Device::OpenSharedHandle` for the three textures **and both fences**, its own direct queue `Wait`ing on the GPU for the engine's fence value, a `CopyTextureRegion` into a readback buffer, `Signal` of the host fence, then release. Result on the chapter fixture: **217 frames** in 9.1 s, the three scenes' centre levels **17-34 / 209-228 / 117-127** (expected 23 / 223 / 123), a resize to 256 x 144 (107 frames after), **0 allocations** over a second of host calls: the same as the Direct3D 11 host, frame for frame.

### HDR texture output

`output_format` (with `MEDIA_OUTPUT_TEXTURE`; an unknown value, or a format without texture output, is `MEDIA_ERROR_INVALID_ARGUMENT`):

| `output_format` | DXGI format | Colour space (`color_space`) | What the pixels are |
|---|---|---|---|
| `MEDIA_OUTPUT_FORMAT_RGBA8` (0, the default) | `R8G8B8A8_UNORM` (28) | 0, G2.2 BT.709 | display-referred SDR; an HDR picture tone-mapped (BT.2390 EETF), as API 13 |
| `MEDIA_OUTPUT_FORMAT_PQ10` | `R10G10B10A2_UNORM` (24) | 12, `RGB_FULL_G2084_NONE_P2020` | an HDR10 signal: PQ, BT.2020. A PQ picture keeps its own codes, HLG goes through its 1,000 cd/m² OOTF, SDR has its white at **203 cd/m²** (BT.2408); nothing is tone-mapped |
| `MEDIA_OUTPUT_FORMAT_SCRGB16` | `R16G16B16A16_FLOAT` (10) | 1, `RGB_FULL_G10_NONE_P709` | scRGB: linear BT.709, 1.0 = 80 cd/m², above 1 and below 0 kept (the same light as PQ10) |

`media_output_textures_t` now says `color_space` and `sdr_white_nits` (203 for the HDR formats); each `media_output_frame_t` says the picture's `transfer` (1 BT.709, 16 PQ, 18 HLG; 0 for a composite with no picture yet). How: the conversion passes (`Nv12ToRgb.hlsl`, `HdrToneMap.hlsl`, `Reconstruct.hlsli`) take `kHdrOutput` (PQ BT.2020 code values instead of display-referred BT.709) and `kLinearOutput` (written as scRGB); the rest of the pass (reconstruction, sharpening, bars) runs on the PQ values as it runs on gamma ones, and the 8-bit dither is off. In the windowed engine the slots, the history and the composites take the texture's format (the blit's pipeline too). **In the two HDR formats the windowed engine draws the picture only**: its captions, dock, cues, command bar and music mode's visual are 8-bit render-target pipelines and are not drawn, and snapshots are refused (the host has the texture). The shared-texture state machine itself moved into `render::SharedOutput` (one implementation for the windowed engine and host mode).

**Verified** (the host reads its copy back and decodes PQ or scRGB to display-referred values for the chapter fixture's levels): PQ10 through a **Direct3D 12** host, **218 frames**, levels 17-35 / 209-228 / 117-127, resize, 0 allocations; scRGB through a Direct3D 11 host, 217 frames, 17-34 / 209-228 / 117-127; and the **HDR10 fixture** (HEVC Main 10, 4K, PQ, BT.2020) in PQ10: the brightest light the host read was **5,849 cd/m²** (an 8-bit tone-mapped texture cannot exceed SDR white), every frame's `transfer` 16.

### Hardware decoding in host mode

Host mode (the host steps the engine on its own thread; no engine thread) decoded only through the host's `decode_video` or the software bridge (I_PCM). Now, on Windows, **host mode with `MEDIA_OUTPUT_TEXTURE`** decodes H.264 and HEVC (8- and 10-bit) on the GPU with the engine's own Direct3D 12 video decoder and draws into the same shared textures as the windowed engine (`media_engine_query_output_textures`, `acquire`, `release`, `resize` now answer in host mode):

```
HostEngine (the host's thread: parse, DPB, POC, clock, commands)
  -> HostGpu::Decode: DXVA picture on the decode queue; a picture to show is copied once, on the GPU, into a ring of 8 NV12/P010
     pictures, which returns the decoder's surface at once (decode order is not presentation order: a picture waiting to be shown
     must not hold a surface the decoder needs)
  -> HostGpu::Show (when the clock says): ColorPipeline converts the view (zoom, pan) fitted into a free shared texture,
     in its format; the frames fence signalled; published. on_present gets MEDIA_PICTURE_TEXTURE (no planes).
```

- **Fallback, automatic and free**: when the GPU cannot take the stream (no device, the profile or size refused, more reordering than the ring holds), open goes on with `decode_video` or the software bridge exactly as before (nothing of the GPU's is made) and the texture calls say `MEDIA_ERROR_STATE`. `media_query_video_decoder(engine)` says who decodes: `MEDIA_DECODER_HARDWARE`, `_HOST`, `_SOFTWARE` or `_NONE` (the windowed engine always `_HARDWARE`).
- With texture output, host mode reads the file in **4 MiB blocks** (1 MiB before: a 4K picture's sample can be larger); `media_query_arena_budget` includes the 24 MiB more, exactly.
- The shared textures are written as UAVs; Direct3D 11 opens such a texture only with simultaneous access (`OpenSharedResource1` said `E_INVALIDARG` without it, measured), so host mode's are made with it.
- One direct queue and one fence (the decoder's consumer timeline): the decode queue waits on the GPU for a copy before decoding over its source, the direct queue for a decode before copying it, and for the host's release value before drawing into a texture again. The host's thread never waits for the GPU while playing (a resize or close drains it).

**Verified** (`media_host_gpu_decode_tests`, `media_host_gpu_decode_hdr_tests`; the host steps every millisecond and pumps 10 ms of audio per 10 ms):
- the chapter fixture (H.264) through a Direct3D 11 host, then a Direct3D 12 host: `media_query_video_decoder` **hardware**; **159 frames**, the dark scene from the start, then "go to 0:04" (bright) and "go to 0:07" (mid): levels **17-34 / 214-228 / 116-128**, none wrong; a resize to 256 x 144 while playing (a new generation, 28 frames after); every `on_present` a texture picture; **0 allocations** over a second of steps and host calls; the arena exactly its budget, 0 threads;
- the 4K HDR10 HEVC Main 10 fixture, PQ10, Direct3D 12 host: all **72 frames**, brightest **5,849 cd/m²**;
- **CPU**: the process's CPU time while playing, less the test's own pixel decoding (its eyes; a real host samples on its GPU): **0.04 s over 6.6 s (0.7 % of one core)** for the 1080p-class H.264 fixture and **0.20 s over 3.0 s (6.8 %)** for 4K 10-bit HDR HEVC, the engine and the host's GPU calls together. **0 CPU frame copies**: no picture leaves the GPU.

### Packages

`cmake --build build --config Release --target avion_packages` writes both into `artifacts/packages/`:

- **`avion-0.1.0-py3-none-win_amd64.whl`** (`tools/python/`, built with the standard library by `build_wheel.py`: RECORD hashes, the licence in the dist-info): `avion`, a zero-dependency `ctypes` binding with `avion.dll` and `c_api.h` inside. `AvionEngine` (host mode: `open`, `step`, `play`, `command`, `state`, `video_decoder`), `telemetry()` (rPPG BPM, vibration, TTC, the four-object table, the virtual IMU), `export_foveated_tensor()` (a `Tensor` the engine writes into in place: buffer protocol and `__array_interface__`, so `numpy.asarray` and `torch.from_numpy` are views, no copy), `set_event_camera()` / `poll_event_spikes()` (a NumPy structured view), `BitstreamInspector` / `inspect_bitstream()`, and the shared textures. `pip install` of the file works offline (checked in a fresh virtual environment).
- **`Avion.Sdk.0.1.0.nupkg`** (`tools/nuget/`): `lib/netstandard2.0/Avion.Interop.dll` (`Avion.Native` with explicit x64 layouts, `AvionHost`, `AvionWindow`; its source in `src/` for Unity), `runtimes/win-x64/native/avion.dll`, and for C/C++ `build/native/Avion.Sdk.targets` (include path, `avion.lib`, `avion.dll` copied next to the program).

**Verified**: `media_python_binding_test` builds the wheel, checks every RECORD hash, unpacks it and imports `avion` from there only: the pulse fixture read **72.0 BPM** (confidence 1.00); a 1 x 3 x 224 x 224 float32 tensor in [0.09, 0.74], exported again into the **same buffer** (address unchanged), and float16; **9,473 events** in a second of the two-target fixture (4,804 ON, 4,669 OFF); all **72 pictures** of a Media Foundation H.264 stream inspected through `decode_video` (I, P and B in decode order, QP 26, IDRs at 0, 1 and 2 s, no false cut); PQ10 textures from the **hardware** decoder, a frame acquired and released. `media_nuget_package_test` builds the package, restores it from a local folder only into a fresh .NET 8 console, which finds `avion.dll` copied beside it and plays: **72 frames** in 3 s from the hardware decoder in PQ10, a **72.0 BPM** pulse, a 150,528-float tensor.

### The licence

`LICENSE` (the AGPLv3 text) and `LICENSING.md` (in the repository, the bundle, the wheel and the NuGet package). Since 9 November 2026 AVION is **dual-licensed**: the GNU AGPLv3 (`AGPL-3.0-only`), under which the full source is published at https://github.com/5645hm-a11y/avion, or a commercial licence from the author (Alexandre Heymann) for closed-source software, devices and services. Until then it was the AVION Evaluation & Commercial SDK License 1.1.

**Not covered.** No Unity or Unreal plug-in. The windowed engine's overlays are not drawn into the HDR formats. The host's device must be on the engine's adapter. Host mode's hardware decoding is Windows only (elsewhere open falls back, as documented); its pictures are the GPU's, so host mode's CPU-side perception (telemetry, tensors, snapshots, the event camera) needs the software bridge or live sensors, as with a host decoder.

## API 17: Matroska and WebM, AV1, Opus, host-mode tracks, subtitles after open, snapshots into memory (2026-11-05)

- **Containers.** `.mkv` and `.webm` play in both modes through `demux::Demuxer`, which sniffs the file and holds both
  demuxers. See [matroska.md](matroska.md) for the index, times, lacing and what is refused.
- **AV1.** Windows' software decoder in the windowed engine (ENGINE_MANIFESTO.md 2E), its pictures copied once onto the GPU.
  `media_query_video_decoder` reports it. Host mode does not take AV1 video in this build (the track is listed, not selectable).
- **Opus.** Windows' decoder in the windowed engine, mono or stereo. Host mode hands Opus units to `decode_audio` as before.
- **Tracks in host mode.** `media_engine_track_count/_info` and the two track controls.
- **`media_engine_attach_subtitles`**: an .srt/.vtt loaded while playing, in both modes (windowed since 2026-11-05).
- **`media_engine_snapshot_to_memory`**: the snapshot as BGRA8/RGBA8 in the host's buffer, in both modes.

The reference (calls, errors, limits) is [sdk-reference.md](sdk-reference.md).
