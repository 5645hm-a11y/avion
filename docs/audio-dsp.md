# Real-time audio DSP

`DspProcessor` is part of `media_timing`. It processes interleaved float PCM in place with fixed inline storage: no heap, locks, extra PCM queue or lookahead buffer. Configure it during preparation. The portable processor accepts 8–192 kHz and mono, stereo, 5.1 or 7.1 in WAVEFORMATEXTENSIBLE order. 5.1 AAC now plays end to end, and a stereo endpoint gets it through the DSP's stereo fold-down: binaural by default, or an ITU downmix. 7.1 AAC cannot be decoded by the platform decoder, so 7.1 processing is covered by synthetic PCM tests. See [surround](surround.md).

## Controls and playback

- **D** cycles dialogue clarity: off, light, medium, strong, off.
- **Shift+S** toggles binaural headphone rendering (**S** until 2026-11-01; **S** now takes a [snapshot](picture.md#forensic-snapshots-2026-11-01)). It has no effect on mono input.
- Both start off, except that surround folded to a stereo endpoint starts with binaural on (S then selects the ITU downmix). Held-key repeats are ignored.
- `PlaybackCoordinator::SetClarityLevel(unsigned)` clamps to 0–3; `SetSpatialAudio(bool)` selects spatial audio. They publish lock-free atomics, sampled by the media worker once per PCM span. Only the worker mutates the processor.

The worker acquires a WASAPI PCM ring write span, reads AAC output directly into it (including the existing post-seek fade-in), applies DSP, then commits. Ring backpressure leaves decoder output pending. Decoder-drain output uses the same path, and end-of-stream silence passes through DSP to let delay/filter tails finish. Seek flushes call `Reset()` after resetting the endpoint; skipped AAC pre-roll never enters DSP. Pause preserves DSP history.

### Volume and mute

- **Up / Down** change the volume by 5 %; **M** toggles mute. Held-key repeats are ignored; the demo keeps the volume on the 5 % grid.
- The **mouse wheel** changes it by the same step per notch, and the dock's **volume pill** can be dragged. Every change, from any source, shows the pill for 1.5 s; a mute shows its cue in the dock for as long as it lasts ([glass dock](kinetic-hud.md)).
- Spoken-style commands set it too: `volume 40`, `louder`, `תנמיך`, `השתק` ([intents](intents.md)).
- `PlaybackCoordinator::SetVolume(float)` clamps to 0–1, `SetMuted(bool)` mutes; `Volume()`, `VolumeDecibels()` and `Muted()` read them back. They are lock-free atomics in `DspControls`, callable from any thread. They survive seeks, pauses and endpoint changes.
- **Taper.** Volume `v` maps to a gain of `60 (v − 1)` dB: 100 % is 0 dB, 95 % is −3 dB, 50 % is −30 dB, and 0 % is silence. Each 5 % step is 3 dB anywhere on the scale.
- **Where it runs.** `audio::OutputGain` scales the PCM as the sink copies it from the ring into the endpoint buffer (`Win32WasapiAudioSink::Fill`), not before the ring. A key press is therefore heard within the endpoint's buffer (10–30 ms) plus the ramp, instead of after the 340 ms of PCM already queued.
- **Ramp.** Every change of target, volume or mute, ramps the gain linearly over 20 ms (960 frames at 48 kHz) from wherever it is. A change during a ramp re-aims it; nothing jumps. The ramp is per frame, the same on every channel, and independent of block size.
- **Exactness.** At 100 % with no ramp in progress the samples are untouched (bit-identical). The gain never exceeds 1, so the stage cannot clip.
- While the null endpoint plays (no audio device, see [recovery](recovery.md)) nothing is scaled; the setting applies again on the next endpoint.

Clarity ramps over 50 ms and spatial over 30 ms. After disabling both and completing those ramps, PCM is bit-identical, including signed zero and arbitrary bit patterns. Bypass deliberately does not clip. `Reset()` clears filters, delay history, envelopes, control phase and ramps; it keeps controls and cumulative statistics. `Configure()` starts fresh statistics. Coordinator statistics may be read only after `Run()` returns or `Close()` joins the worker.

## Signal contracts

Clarity detects the speech band's share of carrier energy through 300 Hz / 3.4 kHz Butterworth filters. Stereo centredness suppresses side content; surround uses the centre channel. A separate in-phase presence filter supplies boost toward −20 dBFS, capped at 4 / 7 / 10 dB. A soft-knee compressor uses a −16 dBFS threshold, 8 dB knee, and ratios 1.5:1 / 2.5:1 / 4:1. Gain targets update every 32 frames, with per-sample interpolation independent of span size.

This is a spectral/centredness heuristic, not a speech classifier or voice separator: centred narrow-band music can trigger it. Listening quality and performance on a speech/noise corpus have not been established by the synthetic tests.

While active, the final soft clipper bounds samples to `[-0.99999994f, +0.99999994f]`. Non-finite decoder samples are replaced before recursive processing so they cannot poison subsequent valid PCM.

Spatial rendering uses Brown–Duda head shadow and Woodworth interaural delays, cubic Lagrange interpolation (linear for delays below one sample), and a rear-source low-pass contribution. Fronts are at ±30°, 5.1 surrounds at ±110°, 7.1 backs at ±150° and sides at ±90°. The two ears replace channels 0 and 1; remaining channels reach exact zero when the ramp finishes. LFE feeds both ears. This preserves channel count/stride; it is not an endpoint format conversion.

The spatial gain contract is a per-ear DC weight of 0.5 for each front, 0.5/√2 for centre/surround, and 0.25 for LFE. Mirrored sources exchange ear energies, and a low-frequency centred stereo phantom retains its level. Arbitrary broadband energy is **not** conserved: head shadow, pinna filtering, interpolation, coherent summation and clipping intentionally change it. Delay tests include the head filter's group delay, rather than mistaking the impulse's peak position for pure propagation delay.

## Stereo fold-down

`Configure(rate, channels, true)` (6 or 8 channels) renders the whole programme into channels 0 and 1 and keeps the others at exact zero, so the engine can keep just two channels for a stereo endpoint.
- **Spatial off: an ITU-R BS.775 downmix.** L = FL + FC/√2 + left surrounds/√2, R likewise, LFE omitted, at the binaural renderer's per-ear scale of 0.5, so a centred voice keeps its level across the toggle.
- **Spatial on: the binaural ears.**
- **Toggle.** The spatial ramp crossfades the two over 30 ms.
- **Never a passthrough.** A fold-down processor always runs, so its soft clipper also bounds a full-scale coherent downmix.

`DownmixWeights(channels, channel, left, right)` exposes the matrix.

## Build and regression

The existing `/W4 /WX` and `-Wall -Wextra -Wpedantic -Werror` policies apply. DSP uses `/fp:precise` on MSVC and `-ffp-contract=off` elsewhere. SSE2 is selected when available, NEON on compilers defining its feature macros, and scalar otherwise; no AVX requirement or x86-only global flags are added. `MEDIA_DSP_FORCE_SCALAR` exercises the fallback in the Linux runner. ARM/NEON has not been executed on this x64 host.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\media_player_demo.exe .\build\playback_fixture.mp4 --test-dsp
```

```bash
bash build/linux/gcc.sh
```

`media_audio_dsp_tests` covers calibrated gain at all clarity levels; silence, centred white noise, side-only and out-of-band rejection; centre-channel isolation; below-knee, mid-knee and above-knee compression; over-range clipping; non-finite recovery; bit-exact bypass; delay, DC gain and mirrored ear energy across five sample rates and every supported layout; block partition invariance including disable; reset equivalence; and zero measured processing/control/reset allocations. For volume and mute it runs a 0.99-peak two-tone stereo signal through four target changes (50 %, mute, unmute half-way through the mute ramp, 100 %): the taper values; bit-exact unity; each target reached exactly 960 frames after it was set; no sample raised and the peak unchanged (no clipping); the slope added by a ramp never above one gain step per frame (0.000975 measured, so no step and no click); block sizes 1 and 4096 bit-identical. `media_playback_volume_test` sends real Down, M and Up key messages during playback and checks the final 90 %, unmuted state, at least four ramps with steps of at most 1/960, all 72 frames and all PCM, and 0 allocations. The fixture's audio is quiet (peak 0.012), so loud-signal behaviour is established by the unit test, not at the endpoint.

`media_playback_dsp_test` runs `--test-dsp` on the H.264/AAC fixture. It sends real D/S window messages, checks repeat suppression, disables/re-enables effects, seeks forward while playing and backward while paused, verifies exact video/audio landing, and checks DSP statistics, two resets and zero allocations.

Allocation probes intercept C++ allocation operators in every build and CRT allocations/reallocations in MSVC Debug. They do not measure native OS/driver heaps. DSP itself contains no allocator calls; the Windows integration retains the existing native decoder/endpoint allocation boundary.

Verified on 2026-09-28: MSVC x64 `/W4 /WX` full CTest passed 47/47 in Debug and 47/47 in Release, with no skips. The final strengthened DSP/key-message tests also passed a targeted Debug rerun. GCC 13.3 on Ubuntu 24.04 (WSL) passed all six portable suites at `-O2` and with ASan+UBSan; the additional forced-scalar DSP variant passed both configurations. DSP processing and the scripted playback reported zero measured allocations.

## Integration fixes

The first regression run exposed a ramp endpoint error: a fully enabled mix alternated between 1 and one increment below 1. Ramps now stop at their target. Disabling clarity also now completes at a deterministic sample boundary instead of waiting for asymptotic envelopes to become exactly zero. Reset clears all ramp state, and active processing rejects non-finite input before it reaches filters. These behaviors have dedicated regression coverage.

## Playback speed (2026-10-20)

Directive: "PITCH-PRESERVING SPEED CONTROL + FRAME-STEP (TASK 45 / A38)". 0.25× to 2×, the pitch kept, the picture following.

**Controls.**
- `]` and `[` step through 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75 and 2×. At either end a warning cue says so (`Speed · 2× is the fastest`).
- Commands: `faster`, `slower`, `speed 1.5x`, `play at 0.75`, `normal speed`, `double speed`, `half speed`, `מהר יותר`, `לאט יותר`, `מהירות כפולה`, `מהירות רגילה`, and their counterparts in seven more languages ([intents](intents.md#speed-and-zoom-2026-10-20)). A rate is any of 0.25–2 (`1.5x`, `x1.5`, `1,5×`, `150%`, `פי 2`, `2倍速`); others are refused (`Rejected`).
- `PlaybackCoordinator::SetSpeed(float)`, `StepSpeed(int)`, `Speed()`. Every change shows `Speed · 1.5×`.

**The stretcher** (`audio::TimeStretcher`, `media_timing`, portable): WSOLA, waveform-similarity overlap-add.
- Output is made in hops of 12 ms. Each hop overlap-adds one periodic-Hann frame of 24 ms; two half-windows sum to exactly 1.
- Frame k is read near its analysis position `a_k = a_(k−1) + speed × 12 ms`, within ±8 ms. The offset chosen is the one whose first half best continues the previous frame's second half: the normalised cross-correlation of the channels' mean, at every fourth sample, then refined to the sample around the best.
- The analysis positions never take the offsets in, so the speed is exact over any span longer than a hop. Frames are copied, never resampled, so the pitch is untouched.
- **At 1× it copies, sample for sample** (`Passing()`). A change away from 1× starts from the input it was passing on, so nothing clicks. A change back to 1× ends at the next frame boundary, where the overlap and the input's natural continuation sum to the input itself.
- A stretch that begins within a hop of its stream's start (a seek or a file started at another speed) searches only from the stream's first frame. Its target lies before it; until 2026-10-20 the search range was then inverted. In practice the start chosen was still frame 0, but the range was undefined behaviour, and MSVC's Debug `std::clamp` assertion stopped `media_time_stretch_tests` there.
- The channels' mean over the search region and the template is computed once per hop, into arena scratch. Results are bit-identical to computing it per comparison; the suite takes 0.38 s in Release instead of 0.65, and 1.3 s in Debug.
- `LastChange()` reports where the speed in force last changed: an output frame and the input position it stands for.
- Storage comes from the arena at `Configure`; then nothing allocates, locks or reads a clock.

**In the worker.**
- At 1× decoded PCM goes to the endpoint as before. At any other speed it goes through the stretcher. Back at 1× the stretcher drains, then the direct path resumes.
- **Timeline.** The worker maps committed endpoint frames to media time in segments of one speed each (`Segment {frame, media, speed}`). The stretcher's changes add segments at the output frame they take effect.
- **Clock.** `PredictiveMasterClock::Reanchor(sample, media, epoch, speed)` accepts 0.25–4×. Its estimate is `anchor + speed × (phase + rate × dt)`, and `Deadline` divides by the speed. The worker anchors in the segment the device is playing, and anchors again whenever the device enters the next one. That is not counted as a discontinuity.
- **Pictures** follow the clock as always: at 2× each picture is due at its own time, which comes twice as fast.
- **Seeks, pauses, track changes.** A seek starts its timeline at the speed in force. A track splice continues the old track's timeline through the stretcher (its target computed from the stretcher's input position). The speed survives both and pauses; it is per file.

**The 92 and 119 ms marks (2026-11-04).** The test failed once in about fifty runs since 2026-10-28, with one mark 92 to 119 ms off at 1×, every clock phase exact, and passing alone. It was the test's probe, not the audio:
- The probe counted a quiet run across a seek. The test ends in frame steps; when the first step cut a mark short (the PCM committed so far ended inside one), its quiet samples and the new timeline's fade-in read as one dip, placed at the new timeline's start. With targets 41.67 ms apart on a 250 ms grid that is anywhere: phase 125 ms less 5 ms for the mark and 1 ms for half a 2 ms tail is 119; −83.3 less 5 and half 7.4 ms is 92.
- It was not the WSOLA tail or the clock's anchor. 30 runs under load (10 busy processes) found no wrong mark (their one failure was a dropped picture); forcing the condition, the first step sent while the committed PCM ends in a mark, failed 8 of 8 runs. The stretcher's change points are exact (`LastChange`), and a mark's silence at 1× is the input's.
- The probe now starts over at each new timeline, and a run that begins in its first 5 ms (a fade-in, a target inside a mark) is not a mark. The test keeps the forced condition, so it covers this for good: 8 of 8 passed, every mark at 0.0 ms at 1×.

**Frame step** (`.` forward, `,` backward; `StepFrame`):
- A step pauses first if playing.
- Each step seeks to the picture after (or before) the one on screen: its pts plus its duration, or its pts less 100 ns. It lands exactly, as any seek does ([seeking](seeking.md)).
- Steps are taken one at a time, each from where the previous one landed (`PumpSteps`), so a held key never skips one.
- At the first or last picture a cue says so. Music mode has none to step through.

**Measured** (`media_playback_speed_test`, on the 8-track fixture: an English 440 Hz track with 10 ms silent marks every 250 ms, Release, UHD 630). Speeds driven by real input: `]` at 1.5 s, `speed 2x` at 3 s, `half speed` at 4.5 s, `מהירות רגילה` at 6.5 s.

| Speed | Clock slope (media / wall) | Marks: worst distance from the grid, in media time |
|---|---|---|
| 1× | 1.0000 | 0 ms |
| 1.25× | 1.2500 | ≤ 8.1 ms |
| 2× | 2.0000 | ≤ 8.1 ms |
| 0.5× | 0.5000 | ≤ 10.2 ms |

The tone heard was 438.6–441.2 Hz at every speed (440 Hz recorded).

Then `.` three times and `,` twice while paused: each step landed exactly one picture from the previous one (±1 tick). 0 pictures dropped, 0 allocations.

The worst mark distance comes from WSOLA's ±8 ms search: a mark is heard up to one search window from where the timeline puts it, and never accumulates.

`media_time_stretch_tests` (MSVC, GCC `-Werror`, ASan/UBSan) checks the stretcher alone:
- 1×: an exact copy.
- 0.25–2×: a tone keeps its frequency (a Hann-windowed Goertzel over 0.1 s windows) and its level; a voiced sound (180 Hz with seven harmonics) keeps its fundamental (autocorrelation).
- The input consumed follows the speed.
- 1 → 2 → 0.5 → 1×: no step larger than the tone's own; each change's input position is where the previous speed would have brought it, within a search window and a hop; after the return to 1× the output is the input again, sample for sample.
- `Reset` starts a stream at the speed asked for; 0 allocations.

**Limits.**
- Nothing has been rated by listeners: the tests measure pitch, timing and continuity only. WSOLA's usual artefacts at the extremes are whole 12 ms pieces dropped (speech sounding clipped towards 2×) or repeated (transients doubled towards 0.25×).
- Pictures are not interpolated: at 0.25× each picture is held four times as long.

## Music mode (2026-10-20)

Directive: "PURE AUDIO/MUSIC MODE". A file with no picture track plays as sound alone, and the window shows the sound. This covers `.m4a`, an audio-only `.mp4`, or any ISO file whose only playable track is AAC-LC.

**What plays.**
- Everything the audio path does: the WASAPI clock, dialogue clarity and binaural rendering, the volume and mute, speed, chapters (Nero `chpl`, or a QuickTime chapter track named by the audio track's `tref 'chap'`), resume and the folder ([session](session.md)), the dock, the command bar, voice.
- A seek lands when its sound starts playing (its clock anchored): `D3D12Renderer::LandAudio` stands for the landing picture, so everything that waits for a landing works unchanged.
- Frame stepping and zoom are refused (a cue says there is no picture).
- A file with neither a picture track nor decodable AAC fails `Prepare` at stage 4, as before.

**What it costs.** No video decoder, no DXVA tables and no decode surfaces are made. A warm session's decoder waits for the next video file.

**The picture of the sound.**
1. **Analysis** (`audio::SpectrumAnalyser`, `media_timing`, portable). The worker feeds it the PCM it hands to the endpoint (after DSP and any fold-down) in blocks of 1024 frames.
   - **Spectrum.** The previous block and this one (2048 frames, centred on this block's first frame), Hann-windowed into a radix-2 FFT, summed into **32 log-spaced bands** from 40 Hz to 16 kHz. Each band is the summed power of its bins, or of the three bins around its centre when it has fewer of its own. It is normalised so that a full-scale sine reads 0 dB in its band, and coded −72 to +6 dB as 0–255 (0 dB is 235).
   - **Waveform and level.** The block's waveform envelope (80 columns of least and greatest sample), its RMS level (−60 to 0 dBFS as 0 to 1) and its peak.
   - **Stamp.** Each `AudioPicture` (256 bytes, trivial) is stamped with the media time of the block's first frame.
2. **Hand-off.** The pictures go to the UI thread through a 32-slot `SpscRingBuffer` (0.68 s of sound at 48 kHz; the endpoint ring holds 0.34 s).
   - Each pass, the UI thread takes the latest picture whose block starts by the time its composite will be on screen: the clock's media time now plus one refresh at the speed in force.
   - Pictures of an epoch a seek has left are dropped. While paused nothing is taken, so the picture holds.
3. **Drawing** (`render::VisualPipeline`, `VisualComposite.hlsl`). One full-target triangle, opaque, under the dock, subtitles, status pill and command bar. Every mark is a signed distance with one pixel of antialiasing, from 56 root constants: no texture, no vertex buffer, no upload.
   - **Bars.** A ring of 64 bars: the 32 bands mirrored left and right, lows at the top.
   - **Orb.** Around a glass orb that breathes with the level, its rim brightening with the peak.
   - **Ribbon.** The waveform envelope as a ribbon across the middle, behind the orb.
   - **Motion.** Bars and level rise at once and fall at most 1.6 of their range a second; held while paused, and dimmed.
4. **Pacing.** One composite per refresh period (from the compositor's timing) while playing; while paused, only when the controls change. They are presented like pictures, with sync interval 0. A vsync-paced present (sync interval 1) was tried and dropped: the compositor throttles it when nobody sees the window, and a locked session ran it at 6 composites a second. Uncapped, it had run at 548.

**Measured** (`media_playback_music_test`, the M4A fixture: 10 s of AAC, chapters at 0, 3 and 6 s holding 440, 1760 and 880 Hz; Release, UHD 630, a 59.9 Hz display):

| | |
|---|---|
| Composites while playing | 60.0 a second |
| Loudest band shown | 12, 20 and 16 in the three chapters: exactly the bands of 440, 1760 (after `next chapter`) and 880 Hz (at 1.5×) |
| Chapter landing | `next chapter` from 2.008 s landed at 3.0047 s, its sound playing 23.0 ms after the command |
| Timing | 371 pictures shown, 0 dropped; from each block's start to its expected time on screen 0–5.9 ms (the hand-off is checked every pass, so a picture goes as soon as it is due) |
| Pause | 0.5 s paused: 0 pictures taken |
| Allocations in `Run` | 0 |

`media_spectrum_tests` (MSVC, GCC `-Werror`, ASan/UBSan):
- At 48 and 44.1 kHz a full-scale sine at the centre of bands 4, 12, 20 and 28 reads 235 (0 dB) in its band. That holds for the low band too, which takes the three bins around its centre. It is the loudest band wherever the band has bins of its own, and every band two octaves away reads under −40 dB.
- Level 0.95 (−3 dBFS RMS), peak 1, envelope ±127.
- Two −12 dB tones read 196 each, with nothing between them; silence reads nothing; `Reset` forgets the previous block.
- Stamps are exact whatever the feeding pattern (1, 7, 480, 1024 and 4096 frames at a time), and a completed picture waits to be taken.
- 0 allocations after `Configure`; `Configure` only before sealing.

**Limits.**
- The picture follows the PCM as handed to the endpoint, before the volume (the visual does not shrink when the sound is turned down).
- The waveform envelope is the block's least and greatest sample per column: at 1–2 kHz a column holds less than a period, and the ribbon shows a beat pattern rather than the wave.
- Below about 300 Hz the bands are narrower than the 23 Hz bins, so neighbouring low bands share bins and rise together.
- No album art: a cover in the file's metadata (`covr`) is not read.

### As heard: the volume and mute (2026-10-21)

Music mode used to draw the PCM as decoded, before the output gain: at 10 % volume, or muted, the bars still danced at full height.

Now `D3D12Renderer::MusicUniforms` applies the engine's own gain (`audio::OutputGain::Gain(volume, muted)`, the 60 dB taper the sink applies) to what it draws:
- the bands and the level move down by its decibels (a band code is 78/255 dB, the level 60 dB full scale);
- the peak and the waveform scale by its amplitude.

The renderer learns the volume and mute through `SetVolume`, as the dock's volume pill does. Muted, the gain is 0: the bars fall flat at their usual 1.6 of their range a second, the waveform is a line, and the picture is dimmed (0.6, against 1.0 while paused). At 100 % nothing changes: the music test's loudest bands are the same.

## Loudness and night mode (2026-10-21)

Directive: "BS.1770 SMART LOUDNESS / NIGHT MODE (TASK 41 / AI A29)". Films and recordings swing from whispered dialogue to effects that wake the house.

**The meter** (`audio::LoudnessMeter`, portable, [`Loudness.hpp`](../include/media/audio/Loudness.hpp)): ITU-R BS.1770-4.
- **K-weighting.** The spec's high shelf (+4 dB above about 1.7 kHz) and RLB high-pass (about 38 Hz), derived for any sample rate from their prototypes by the bilinear transform. At 48 kHz they are the spec's tabulated coefficients (largest difference 8.9e-16). The filters run in double precision: the high-pass's poles sit close to the unit circle.
- **Channel weights.** 1 for left, right and centre, 1.41 for the surrounds, 0 for LFE.
- **Windows.** Over 100 ms sub-blocks: momentary over 400 ms, short-term over 3 s, and integrated with the spec's gating (absolute −70 LUFS, then relative −10 LU), kept in a 0.1 LU histogram of block energies.
- **True peak.** Each channel oversampled 4× by a 48-tap polyphase interpolator (Hann-windowed sinc, 12 taps a phase); the largest magnitude of the samples and the points between them.

**Night mode** (`audio::NightMode`, after every other DSP stage, on what leaves it: a fold-down's stereo pair, else every channel):
- **Level.** The input's short-term loudness against −20 LUFS: up to +12 dB of lift, down to −12 dB. The gain moves at most 3 dB/s up and 10 dB/s down: slow enough not to pump with syllables or beats.
- **Bursts.** Momentary loudness above the target + 4 LU pulls the gain down at up to 30 dB/s.
- **Silence.** While the momentary loudness is below −50 LUFS, or more than 15 LU below the short-term, the gain is frozen. Pauses between sentences and a quiet room's noise floor are never lifted.
- **Limiter.** A look-ahead true-peak limiter at −1 dBTP. Its gain is the window's average of the per-frame minimum required gain over the look-ahead window. Every one of those minima is at most what the frame leaving needs, so the gain is in place when it leaves and got there without a step. It is released at 8 dB/s.
- **Latency.** On, the samples are delayed 69 frames (1.44 ms at 48 kHz: 63 of look-ahead and the interpolator's 6). The clock is not moved for that; 1.4 ms is under any lip-sync tolerance.
- **Switching.** On and off crossfade over 50 ms. Off and settled, the samples pass bit-identical and undelayed.
- **Meters.** The meters run whether night mode is on or off. The media worker publishes the input's momentary and short-term loudness, the output's recent true peak, the gain and the limiter every 100 ms of PCM: `PlaybackCoordinator::LoudnessNow()`, and the telemetry panel's `Loudness` line.

**Controls:**
- **K** toggles night mode;
- `night mode`, `normalize volume`, `מצב לילה`, `אזן עוצמת קול` and their counterparts turn it on; `night mode off`, `בטל מצב לילה` turn it off ([intents](intents.md#night-mode-bookmarks-and-the-pinned-frame-2026-10-21));
- a status cue says so.

**Measured** (`media_loudness_tests`, portable; also GCC `-Werror`, ASan/UBSan):

| Check | Result |
|---|---|
| 997 Hz at 0 dBFS, one channel / two | −3.010 / 0.000 LUFS at 48 kHz; −3.008 / +0.003 at 44.1 kHz |
| Response against 997 Hz | 25 Hz −11.08 dB, 10 kHz +3.35 dB; measured within 0.000 dB of the filters' own |
| Gating: silence, 15 s at −23, 15 s at −43 | silence −144 (none); integrated −23.04, then −23.08 (the quiet stretch gated out) |
| Quarter-rate sine, 45° off its crests | samples −3.01 dBFS, true peak −0.07 dBTP |
| Night mode, a room's noise floor (−69 LUFS) | gain never above 0.00 dB |
| Quiet speech at −45.5 LUFS | lifted +12.0 dB to −33.5 LUFS (the lift's limit) |
| The pause after it | gain held at +12.00 dB: not raised further |
| A blast (noise at 0.6) | gain down to −13 dB; true peak out −0.99 dBTP |
| Inter-sample peaks (−0.92 dBTP in) | −1.10 dBTP out |
| Block sizes 1 to 2039 frames | output bit-identical to whole blocks |
| Off, after a crossfade | bit-identical again |

0 allocations throughout.

**In playback** (`media_playback_telemetry_test`): K, then I. The panel shows `Loudness · −39.0 LUFS · peak −38.1 dBTP · night +0.0 dB`. Two seconds later it shows `night +6.2 dB`: the 3 dB/s rise.

## Mastering EQ and audio zoom (2026-10-21)

Directive: "AUDIO-VISUAL ROI BEAMFORMING ZOOM ... 10-BAND PSYCHOACOUSTIC EQ + AUTONOMOUS DRONE/HUM NOTCH KILLER". Three portable stages ([`MasteringEq.hpp`](../include/media/audio/MasteringEq.hpp)), run first in `DspProcessor`, on the source channels: the audio zoom, then the EQ, a hop of the analysis at a time; then clarity, binaural and fold-down, then night mode, as before.

**The analysis** (`SpectralTracker`): the channels' mean, Hann-windowed into a 4096-point FFT every 1024 frames.
- **Bands.** The level of the ten EQ bands (octaves 31.5 Hz to 16 kHz), a full-scale sine reading 0 dB; the dominant one.
- **Speech.** The speech band's (300–3400 Hz) share of everything from 300 Hz to 8 kHz (bass and drones do not hide a voice), times its syllabic modulation (the standard deviation of its level over 0.5 s: a voice comes and goes 4–10 dB, steady noise and drones do not), gated below −65 dBFS, smoothed over 150 ms.
- **Tones.** Local maxima 15 dB above the median of the 48 bins around them, located by the phase advance of their bin from one hop to the next (a phase vocoder: a fraction of a hertz, where the bins are 11.7 Hz apart). A tone followed for 2 s within 1 Hz and 3 dB is stationary.
- The EQ of a frame sees only the hops completed before it, so the output does not depend on how the stream is split into blocks.
- It is the caller's (`DspProcessor::AttachAnalysis`, 112 KB; the engine owns one). The first version was a member of `DspProcessor`, and `media_audio_dsp_tests`, which keeps several processors on one stack frame, overflowed Windows' 1 MiB stack (`0xC00000FD`). Without an analysis attached, the notch killer and the ducking do nothing; the bands, iso-loudness and the audio zoom still work.

**The EQ** (`MasteringEq`): ten RBJ peaking biquads (Q 1.414), ±12 dB each. A band's gain is the user's + iso-loudness + ducking, ramped in dB at a 32-frame control rate.
- **Iso-loudness.** ISO 226:2003 equal-loudness contours. Full volume is taken as an 80-phon reference, so playing `level_db` lower is listening at 80 + `level_db` phon. Each band gets the contour difference, 0 to +12 dB: at −30 dB, 63 Hz +11.2 dB and 1 kHz +0.0 dB.
- **Speech-protected dynamic EQ.** While the analysis hears speech, the bands from 31.5 to 250 Hz that stand more than 6 dB over the speech bands (1–4 kHz) come down by the excess, at most 6 dB, times the speech probability (50 ms down, 300 ms up). The user's cuts on 500 Hz–4 kHz are held at −3 dB at most meanwhile.
- **Notch killer.** Every stationary tone gets an RBJ notch at Q 12 on its measured frequency, faded in over 100 ms (at most six; the most prominent first). A tone gone for 1 s releases its notch over 300 ms.
- **Bypass.** A band back at 0 dB is an exact identity (b = a) whose state only decays; after a 250 ms tail it is dropped. With every band flat and no notch, the samples are bit-identical.

**The audio zoom** (`AudioFocus`): the picture's zoom steers a directional lobe.
- The zoom's centre across the picture, x, is a direction in the stereo image: the constant-power panning angle φ0 = x·π/2.
- The sound splits into the beam b = (cos φ0, sin φ0)·(L, R), which holds every source panned at φ0 whole, and the residual o = (−sin φ0, cos φ0)·(L, R). The output is the beam (plus a presence band-pass at 2.5 kHz on it alone) and the residual times g: a real 2×2 matrix, phase-coherent, no latency.
- Focus f = smoothstep(1.2, 2.4, zoom): g = −12 f dB, presence +5 f dB. Below 1.2× nothing changes; the matrix glides over 50 ms toward each new target.
- 5.1/7.1: the front pair is the stereo pair, the centre keeps its on-axis share, surrounds and LFE (ambience) take g.
- The coordinator copies the renderer's zoom and centre into `DspControls` every UI pass (`SyncAudioFocus`). "audio zoom", "זום אקוסטי" turn it on ([intents](intents.md#audio-zoom-micro-motion-and-cures-2026-10-21)).

**Measured** (`media_eq_picture_tests`, portable; MSVC Release and Debug, GCC `-O2`, ASan/UBSan, forced scalar):

| Check | Result |
|---|---|
| Random band gains, 16 frequencies 40 Hz–14 kHz | within 0.036 dB (48 kHz) and 0.006 dB (44.1 kHz) of the ten biquads' analytic response |
| 1 kHz band at +6 dB | +6.000 dB at 1 kHz |
| Flat, with the analysis running; a band to +6 dB and back | bit-identical |
| 120 Hz drone (−20 dBFS), 50 Hz hum and its 150 Hz harmonic under speech | notches engaged at 2.25, 2.50, 2.75 s on 120.001, 150.002 and 49.996 Hz; the tones −48.5, −42.0 and −41.8 dB |
| The speech meanwhile | third-octave spectrum 200 Hz–4 kHz within 0.12 dB of the clean voice, level −0.03 dB (the waveform differs by −12.5 dB: the notch turns the phase of the voice's fundamental near 150 Hz) |
| Speech alone, 7 s | no notch |
| A bass line 12 dB over a voice | speech 0.95; 63 and 125 Hz −5.5 dB, 31.5 Hz −3.2, 250 Hz −2.9, the speech bands untouched; a −9 dB cut at 2 kHz held at −3.5 dB; 3 s after the voice, 0.00 dB and −9.0 dB again |
| ISO 226 at 1 kHz | 40 and 80 phon within 0.2 dB |
| Two sources, hard left (600 Hz) and hard right (650 Hz), zoom 3× at x = 0.1 | left +0.86 dB (the model −0.10 dB plus presence), right −10.31 dB (model −10.65) |
| The same at x = 0.9 | left −10.36 dB, right +1.01 dB |
| Panning across the picture | largest sample step 0.0101, under the signal's own 0.0255 |
| Zoom 1×, 1.1×, 1.2× | bit-identical |

0 allocations throughout. In playback the analysis feeds the frame signatures ([SDK](sdk.md#frame-signatures)): on the chapters fixture's 440 Hz tone the 500 Hz band is dominant and speech is below 0.3 in every signature.

**Limits.** A synthetic pad held without vibrato for more than 2 s would be taken for a drone. The speech detector is a heuristic. The audio zoom steers within the stereo image (left–right); the picture's vertical position has no counterpart in stereo. No listening test on real films.

## Sound localisation and the audio saccade (2026-10-22)

Directive: "BIONIC RETINA & PHYSICAL WORLD PERCEPTION SDK" (C1, "ears steer the eyes"). [`audio::SoundLocator`](../include/media/audio/SoundLocator.hpp) says where a sudden sound came from; the **audio saccade** turns the view toward it.

**The locator.** Channels 0 and 1 (left, right), in 10 ms blocks. An onset is a block 12 dB above the noise floor (the quietest recent level, rising 5 dB/s; the onset's own blocks do not move it) and above -60 dB re a full-scale sine, at least 250 ms after the previous one. Its 20 ms from the onset are analysed:
- the **level difference** (ILD, right over left) read through the constant-power pan law, azimuth = 4/pi atan(10^(ILD/20)) - 1: exact for a source panned in a stereo programme, +-1 for a sound on one side only;
- the **time difference** (ITD): the peak of the normalised cross-correlation within the pair's spacing (+-spacing/c, refined by a parabola), azimuth = -ITD c / spacing (the sine of the direction for a spaced pair; c = 343 m/s).

The ITD is used when the channels correlate (0.7 or more) and differ by less than 3 dB; otherwise the ILD. The event (azimuth, level, ILD, ITD, correlation, the onset's timestamp) is published under a seqlock: the locator runs on the audio thread, the view's side reads it from another without a lock. Fixed storage (about 33 KB), no allocation.

**Where it listens.** Live sensors: the microphones as ingested (`media_engine_ingest_audio_samples`, with their timestamps). A file in host mode: the programme as it is pumped, before the DSP. The windowed engine: the programme's first two channels (front left and right) as decoded, before the DSP, in the worker; a seek resets it.

**The saccade.** On ("look at sound", "עקוב אחרי הקול", nine languages, [intents](intents.md#look-at-the-sound-2026-10-22); or `media_engine_set_audio_saccade`), each new sound turns the view: 2x when the view was whole (so that it can turn), its centre at x = 1/2 + tan(asin a) / (2 tan(fov/2)) of the picture (fov: the sensor's field of view, 90 degrees for films), kept inside it. In host mode the view slews with a 40 ms time constant (most of the way on the next frame); in the windowed engine it jumps (a live zoom: shown through the pictures already converted, within a refresh). The audio focus follows the view, so the beam turns with it. While a target is tracked, the tracker keeps the view. Status cues: "Looking at the sound · right / left / ahead".

**Measured.**

| | Result |
|---|---|
| A 20 ms noise burst (-12 dBFS) on the right channel only, over a -62 dB noise bed (`media_perception_tests`) | azimuth +0.996 (ILD +50.9 dB), at 0.500 s |
| The same on the left | -0.996 |
| Panned to +0.5 by the constant-power law | +0.500 (ILD +7.7 dB, correlation 1.00) |
| Equal levels, the right channel 14 samples (0.29 ms) late, 0.2 m pair | -0.500 (ITD 292 us, correlation 0.99; expected -0.500) |
| The noise bed alone | no event |
| Bursts at 0.3, 0.4 and 0.8 s | 2 events (the second within the 250 ms refractory time) |
| Live sensors (`media_c_api_tests`, Windows and Linux) | "look at sound", a right-channel burst at 3.5 s: azimuth +0.996, the view 0.703 on the next frame, 0.750 five frames later |
| Windowed engine (`media_playback_saccade_test`, the saccade fixture: AAC, a 60 ms burst right at 2 s, left at 4 s) | azimuth +1.00 then -1.00; the view's centre at 0.750, then 0.250 (zoom 2); 0 dropped |

**Limits.** Left-right only (one pair); no elevation, no front-back. A sound in a stereo mix is placed where it was panned, not where it is in the picture. Synthetic signals only (no microphones in a room, no reverberation); the onset rule is a level step, so a gradual sound is never localised.

## Reverberant rooms and real ambience (2026-10-22)

`media_real_perception_tests` mixes 30 ms noise bursts into `costa_rica_4k60_hdr.mp4`'s own soundtrack (surf, wind, birds; -35 dBFS RMS after a fade-in from -57) 15 dB above it, each through a room (RT60 0.5 s: the direct sound, six early reflections, a decorrelated diffuse tail 3 dB under the direct sound), at -0.8, -0.4, +0.4 and +0.8, twice: as a panned programme (level differences) and as a spaced pair 0.2 m apart (time differences).

The first run found 1 of 8 bursts: the soundtrack fired the locator every refractory period (48 events in 12 s), because a fluctuating ambience keeps clearing "12 dB over the quietest recent level", and the bursts fell inside the refractory windows. Two changes to [`audio::SoundLocator`](../include/media/audio/SoundLocator.hpp):
- **Attack.** An onset must also be `kAttackDb` (6 dB) above the loudest of the 4 blocks (40 ms) before it: a sudden sound, not a swell of surf, wind or traffic.
- **Time differences in a room.** Reverberation lowered a spaced pair's onset correlation to 0.56-0.64 with the time difference still right: `kMinCorrelation` is 0.5 (was 0.7), and a non-zero lag that says the same side as the levels, further out, is used beyond the 3 dB level gate (reverberation tilted one onset's levels by 3.1 dB). A panned programme has no lag: it still goes by levels.

| 8 bursts, RT60 0.5 s, 15 dB over the film's soundtrack | found | right side | worst direction error | events in all |
|---|---|---|---|---|
| Panned programme (levels) | 8 | 8 | 0.10 | 8 |
| Spaced pair (times) | 8 | 8 | 0.01 | 8 |

The synthetic locator tests (`media_perception_tests`) and the windowed saccade (`media_playback_saccade_test`) are unchanged by both.
