# Real-world checks against recorded sensors

Until 2026-11-04 every number for the virtual IMU came from footage whose shake the tests applied themselves (0.068 px rms off
the applied shake). That proves the arithmetic, not the behaviour on a camera nobody tuned the code for. This page holds the
checks against datasets that carry a real gyroscope and motion-capture ground truth. Nothing here is tuned to them: the tool
runs the engine's `VirtualImu` as shipped.

## Data

| Dataset | Licence | What | Where |
|---|---|---|---|
| [TUM VI](https://cvg.cit.tum.de/data/datasets/visual-inertial-dataset) `room1`, `room3` (and `room5` when it finishes downloading): 512x512, 16-bit PNG, 20 Hz; gyro 200 Hz; mocap on the whole run | CC BY 4.0 | a handheld stereo-inertial rig with a ~190 degree fisheye, in a motion-capture room | `assets/real_world/tumvi/` (not redistributed: 1.7 GB each) |

Download from `https://cdn2.vision.in.tum.de/tumvi/exported/euroc/512_16/dataset-roomN_512_16.tar` (`cdn3`, named on the dataset
page, failed its TLS handshake on this machine; `cdn2` serves the same files).

## Method

1. `tools/realworld/prep_tumvi.py <tar> <dir>`: the pictures to 8-bit luma (ffmpeg, a development tool only), and H.264 copies at
   CRF 18, 28 and 35 decoded back to luma.
2. `media_eval_vimu` (`tools/eval_vimu.cpp`): `VirtualImu::Push` over the pictures, one line each.
3. `tools/realworld/eval_tumvi.py`: the gyro integrated between the two exposures (bias from the start of the run).
   - **Calibrated, no free parameter.** The gyro is rotated into the camera by the dataset's own `T_cam_imu` and scaled by its
     focal length (191 px/rad, equidistant). Only the three axis signs are picked, by the sign of a correlation, never a size.
   - **Fitted.** A robust 3x3 map, fitted on one sequence and judged on another or on a held-out half. It was the first method
     used here and it is *confounded* (see below): kept for comparison, not for claims.

## Results, calibrated (the figures to quote)

| Sequence | Pairs | Valid | Motion (rms) | Residual (rms) | Explained | Corr x / y / roll |
|---|---|---|---|---|---|---|
| room1 | 2,820 | 93 % | 15.4 px | 2.64 px | 97.1 % | 0.992 / 0.983 / 0.866 |
| room3 | 2,820 | 90 % | 12.8 px | 4.42 px | 88.0 % | 0.970 / 0.978 / 0.891 |

Per picture pair (50 ms), 8-bit luma of the lossless frames. H.264 at CRF 18, 28 and 35 changes the residual by under 0.02 px
and the explained share by under 0.002 (room1): compression is not the limit.

### What the residual is (each of these was measured, and two guesses of mine were wrong)

- **Parallax, mostly.** The rig moves about 1 m/s in a small room. Adding the camera's translation (mocap, free linear map) to the
  model brings the gyro's gain on yaw from 1.10 / 1.35 (room1 / room3) to **0.97 / 0.96**, and halves the residual on x
  (1.80 -> 0.95 px, 3.11 -> 1.63 px). The virtual IMU measures image motion, which is rotation plus parallax; that is also what
  stabilisation has to remove. Against a gyro alone the parallax counts as error.
- **Not the fisheye (my first guess).** Cropping to the central 384, 256 and 192 pixels leaves the residual at 2.35, 2.45 and
  2.40 px (was 2.28) and lowers the valid share to 89, 81 and 54 %.
- **Not roll under-reading (my second guess).** Regressing the measurement on the gyro gives a roll gain of 0.75 / 0.81, which
  looked like a 20 % bug. The inverse regression gives 1.00 / 1.04 and the symmetric one 0.87 / 0.91: with a correlation of
  0.87-0.89 and noise on both sides, the least-squares slope is diluted. Roll is within about 10 % of the gyro, no bug shown.
- **Not brightness.** Valid pictures are 92-94 % at every mean luma from under 30 to over 90 (the footage is dark: mean 47).
- **More matched tiles, a smaller error:** 2.7 px in the quarter of pictures with the fewest tiles, 1.8 px in the quarter with the most.
- **The fitted map is confounded.** Fitted on room3 instead of room1 it reports 262 instead of 211 px/rad for yaw (the calibration
  says 191): when the rig is turned its body also translates, and a free map attributes the parallax to the rotation. This is why
  the calibrated numbers above are the ones to quote, and why the fitted focal length is not evidence of anything.
- 7 to 10 % of the pictures give no motion (89 scattered runs, median one picture, longest 13).

### What this changes in what we claim

- "0.068 px rms off the true shake" is a statement about **synthetic shake on a clip with a rigid background**. On real handheld
  fisheye footage against a real gyroscope, per 50 ms: **97 % and 88 % of the motion explained with no fitting, 2.6 and 4.4 px
  rms residual of which about half is parallax, and 7-10 % of the pictures with no motion**. Both belong next to the first
  figure wherever it is quoted.
- Not shown here: other rigs (EuRoC's global-shutter pinhole), rolling-shutter phones, fast flight (UZH-FPV), a sequence with
  the rig truly still (TUM's "still start" is not still: its gyro spreads 0.08-0.15 rad/s, so the bias estimate is rough).

## Reproduce

```
python -I tools/realworld/prep_tumvi.py assets/real_world/tumvi/raw/dataset-room1_512_16.tar assets/real_world/tumvi/room1
python -I tools/realworld/eval_tumvi.py build\Release\media_eval_vimu.exe assets\real_world\tumvi\room1 [more sequence dirs] [--variant raw|crf18|crf28|crf35]
```

## Everyday files: the compatibility matrix (2026-11-04)

A user report: an ordinary x264 + AAC MP4 at 640x360 was refused at the open (stage 4), and their app fell back to mpv. The SDK's
samples and 4K HEVC played. `tools/realworld/make_compat_matrix.py` writes 31 everyday variants (ffmpeg defaults, faststart,
fragmented, CMAF, avc3, profiles, chroma formats, interlaced, VFR, odd sizes, AAC layouts, extra tracks, rotation, timecode,
.mov, .m4v, stream offsets) and `tools/realworld/probe_open.py` opens each through avion.dll as an app does.

Found and fixed:

| Variant | Before | Cause | Now |
|---|---|---|---|
| Any ffmpeg x264 + AAC file (the default) | played, then **failed at the end** (stage 29), its last ~0.4 s of pictures unshown | the AAC track's last access unit is short (188 x 1024 + 512 frames for 4 s): the continuity check refused its duration | plays every picture (120 of 120); the short unit is the last one, at most 21 ms of encoder padding is heard |
| Pictures starting after the sound (`-itsoffset`, editors, screen recorders, trimmed clips) | **refused at the open (stage 4)** | an elst of an empty edit then a normal one: only single-edit lists were admitted | admitted: the empty edits delay the track (`edit_delay_100ns`) |
| The same file, once admitted | **dead lock** at the pre-roll (0 pictures, 0.36 s of PCM) | the clock waited for a picture, the PCM queue filled before the parser reached the first one | the clock starts on the sound when no picture has come and the AAC cannot go on (`sound_first`); 105 of 105 shown |
| QuickTime .mov (iPhones, ffmpeg's .mov) | **refused (stage 3, InvalidTable)** | the data handler (a second `hdlr`, inside `minf`) was taken for a duplicate; Apple's `alis` dref was not admitted | skipped and admitted |

Still refused, by design or not done yet:

- H.264 High 4:4:4 (stage 4), High 10 and 4:2:2 (stage 5): this machine's decoder (UHD 630, DXVA) has no such profiles; the engine
  has no software path. A clearer reason than a stage number belongs in the error.
- Interlaced H.264 (stage 5): field and MBAFF pictures are not decoded.
- HE-AAC: the test file could not be written (ffmpeg's native encoder has no HE profile), not judged.
- Sound starting after the pictures: plays, but the clock starts at the sound, so the pictures before it are dropped (19 at
  0.7 s). The fix is leading silence in the PCM pump (through the stretcher and the fold paths); not done.

Regression tests: `media_demux_tests` (empty edits, both versions, a delay alone, rejected combinations; QuickTime's
layout), `media_fixture_x264_aac_test` and `media_fixture_x264_delayed_video_test` (`assets/test_fixtures/compat/`).
