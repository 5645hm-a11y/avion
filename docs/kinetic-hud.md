# Glass Dock HUD (`ui::KineticHud` + `HudComposite.hlsl`)

The playback controls are a floating dark-glass dock at the bottom of the window. It holds a round transport button with a Play/Pause glyph morph, elapsed and total time, a fine horizontal timeline, and a volume pill that appears when the volume changes. The earlier central orb/ring presentation is superseded.

The portable `ui::KineticHud` model owns interaction and animation. `HudComposite.hlsl` draws every mark from signed-distance functions, with no texture, sampler, font, image or vertex-buffer input. The Windows SDK compiles `VS`/`PS` as `vs_5_0`/`ps_5_0` with `/WX` into embedded bytecode. Subtitles are a separate pass composited just below the dock (see [subtitles](subtitles.md)).

## Rendering contract

- **Uniforms.** `HudUniforms` is a 128-byte, standard-layout, trivially copyable block of 32 graphics root constants. Its eight rows:
  1. placement and time;
  2. energy/freeze/pulse/visibility;
  3. progress/scrub offset/scrub velocity/hover;
  4. denial and dock geometry;
  5. pointer and timeline;
  6. clocks, focus and reduced motion;
  7. volume fill level, pill visibility, mute cue and pointer emphasis;
  8. pill centre and half-width, the scrub preview cue (`preview`, 0 to 1), and one reserved constant.
- **Timeline marks.** A second block of 16 root constants (b1, `ui::TimelineMarks`) carries up to 32 chapter and scene marks, 16 bits each: 48 root constants in all ([chapters](chapters.md#on-the-dock)).
- **Geometry.** The vertex shader generates a full-screen triangle from `SV_VertexID`. A scissor bounds shading to the dock plus `HudPipeline::kHorizontalMargin` (56) and `kVerticalMargin` (40) design pixels.
  - The margins are chosen so the drop shadow has decayed to the background before the scissor edge. At the previous 36 px lateral margin, the shadow was cut at 1/255 on white. `media_hud_gpu_tests` now requires the scissor's last column and row to equal the background exactly.
- **Layout.** Design pixels are scaled by `KineticHud::LayoutScale = dpi × clamp(min(width_dip / 1280, height_dip / 720), 0.8, 1.25)`.
  - The dock's half-width is `min(280, (width − 32 × scale) / (2 × scale))`, never below 64. It is centred 78 design px above the bottom edge, which leaves 26 scaled px below it.
  - `Resize(width, height, dpi)` re-lays it out on every swap-chain resize or DPI change without resetting interaction state (see [resizing](resizing.md)).
- **Glass.** A 29 px-radius rounded box: 91 % dark optical density with a slight vertical "crown", a hairline rim, a soft offset shadow, and pointer illumination while hovered. Controls stay legible over white and HDR footage.
- **Transport.** A 22 px disc with a focus ring. Its glyph is one continuous SDF morph driven by `freeze` (smoothstep-eased): two convex quads whose eight vertices travel from the two Pause bars to the halves of the Play triangle. Each quad is an exact polygon distance with 1.2 px rounding. Paused geometry is time-invariant.
- **Timeline.** A 1.5 px track that thickens to 3 px on hover, drag or keyboard focus.
  - Progress is filled in the accent colour.
  - A scrub target draws a trail and a glowing knob. Rejected seeks tint the accent amber and fade.
  - While the picture on screen is a keyframe preview ([seeking](seeking.md#scrub-preview-2026-10-16)), the knob is a ring and a small amber diamond marks where the previewed picture really is. The cue glides in and out over about 80 ms (`preview`); an exact landing removes it.
  - Chapters are thin ticks across the track, scenes small dots above it ([chapters](chapters.md#on-the-dock)).
  - The click and drag target is 32 px tall, defined in the model; only the thin track is drawn.
- **Clocks.** Seven-segment monoline numerals, tabular so they never shift while playing. Elapsed time is on the left, total duration on the right. `H:MM:SS` has no leading hour zero; `MM:SS` is used below one hour.
- **Volume pill.** A small capsule in the gap between the transport button and the duration clock, at most 120 px wide, drawn only while `volume_visibility` is above 0.
  - Left, a speaker: one sound wave from 0 %, a second above about half volume. While muted the waves give way to an amber cross.
  - Right, a level track filled in the accent colour up to the volume, with a knob. While muted the level stays but turns grey.
  - The fill level, the fade and the mute cue reach the shader already animated by the model; at rest the pill is time-invariant like the rest of the dock.
  - A dock narrower than 472 design px leaves less than 84 px of gap and has no pill (`volume_half_width` 0); a volume change then still reveals the dock.
- **Blending.** Output is premultiplied RGBA, blended `ONE, INV_SRC_ALPHA` straight into the swapchain back buffer after video conversion or tone mapping. There is no DirectComposition dependency.

## Independent refresh and GPU lifetime

- **Composite contexts.** Presentation has three prepared command allocator/list contexts, one per swapchain buffer, plus one display-sized GPU history texture.
- **Clean history.** New video copies its clean RGB result to both the back buffer and history before the subtitle and dock passes. A controls-only update restores clean history, then overlays captions and the new uniforms. Before the first video frame, it clears to black. This avoids alpha accumulation and any dependence on discarded flip-buffer contents.
- **Refresh.** The renderer advances the model at a bounded 120 Hz cadence. It redraws while `Animating()` is true, or when the active cue set changes. A final clean refresh removes a faded dock. This also works while paused, with an empty reorder window, or with the clock unanchored.
- **Priority.** Due video takes priority over controls-only redraws, and UI presents never increment video-frame counters. A controls-only redraw is also skipped when a picture is due within one display refresh (the compositor's rate, from `DwmGetCompositionTimingInfo`): that picture's image carries the dock, and the extra image would take its place in the swap chain's queue ([seeking](seeking.md#pre-roll-and-the-presentation-queue-2026-09-30)). With 60 fps video on a 60 Hz display the dock therefore moves with the pictures, 60 times a second; with 24 fps video it still refreshes between them.
- **Media position.** The dock follows what is on screen: the PTS of each presented frame, or while playing, the clock estimate of the current epoch. Estimates from a clock still anchored to a timeline a seek has abandoned are ignored.
- **GPU lifetime.** Each composite context is reset only after its own fence retires, and video RGB slots receive the composite fence before returning to the conversion pool. `DXGI_ERROR_WAS_STILL_DRAWING` keeps the staged image and retries exactly that image. All pools, PSOs and root signatures are prepared before the arena is sealed.

## Inputs and engine commands

**Mouse.** `WindowProc` forwards mouse move/down/up/leave, the wheel, capture loss and focus loss to `KineticHud::Input()`.
- The wheel changes the volume anywhere in the window: one 5 % step per notch, on the keyboard's grid. Partial notches from high-resolution wheels accumulate.
- Pressing on the volume pill and dragging sets the level along its track; like the timeline, the drag holds capture outside the window.
- Capture keeps a timeline drag alive outside the client area. Cancellation ends a gesture without seeking or pausing.
- The top 24 client pixels are a native drag strip; the rest returns `HTCLIENT`.

**Keyboard.** Auto-repeat messages are ignored for every key.

| Key | Action |
|---|---|
| Space | Toggles pause |
| Tab | Cycles focus: transport, then timeline |
| Enter | Activates the focused control |
| ← / → | Seek 5 s |
| Page Down / Page Up | Next chapter / this chapter's start or the previous one ([chapters](chapters.md#navigation)) |
| Home / End | Seek to the start / end |
| F11, F, Alt+Enter | Toggle borderless fullscreen |
| ↑ / ↓, M | Volume ±5 %, mute (the demo's key handler; see [audio](audio-dsp.md)) |
| `/`, Ctrl+F, Ctrl+K | Open the [command bar](command-palette.md); while it is open, keys type into it and none of the shortcuts above fires |

**Seeking.** Pressing on the timeline starts a drag. While it moves, each change of the target emits `HudCommand::Scrub`: a scrub seek, coalesced to one in flight, which may show the target's keyframe as a preview ([seeking](seeking.md#scrub-preview-2026-10-16)). Release emits `HudCommand::Seek` at the final target: the exact frame, with its audio. A click (press and release without moving) is one exact seek. A scrub the engine cannot take is not shown as a rejection: the release decides.
- `Committed()` keeps the knob on the absolute target until media arrives within 0.25 s, or a 3 s timeout expires.
- `SeekRejected()` (fragmented input or a full control queue) returns to the live position and flashes the accent amber.
- The coordinator performs an epoch-based seek (see [seeking](seeking.md)). The first target frame, with its captions, is presented at once, even while paused.

**Volume.** The engine owns the volume; the dock only reflects it. The coordinator reports `Volume()` and `Muted()` to `KineticHud::SetVolume` on every streaming pass, so the pill answers every source alike: keys, wheel, a drag, a spoken-style command ([intents](intents.md)).
- The first report is a baseline and shows nothing. A later report that differs reveals the dock and the pill.
- The fill glides to the new level (60 ms time constant); the pill fades in and out over about 100 ms.
- The pill and the dock fade `kVolumeHideSeconds` (1.5 s) after the last change. They stay while the pointer is over the pill or dragging it.
- While muted, the pill stays in the dock with the mute cue for as long as the mute lasts. It does not keep the dock itself awake: an idle dock still fades, and shows the cue again whenever it returns.
- Wheel and drag emit `HudCommand::Volume` with the new level. The coordinator applies it with `SetVolume`; raising the volume ends a mute, lowering it does not.

**Visibility.** The dock reveals briefly at startup. It stays visible while paused, hovered, focused, dragging or committed, and fades 2.5 s after the last input. A hidden dock's first click only reveals it.

**Reduced motion.** When `SPI_GETCLIENTAREAANIMATION` is off (read at start and on `WM_SETTINGCHANGE`), springs snap instead of animating.

**Robustness.** The model rejects non-finite coordinates and backward event time, bounds scrub velocity, clamps seek targets to the media, treats a non-finite volume as silence, and never allocates.

## Test windows (2026-10-16)

An automated run's window takes no input from the desktop (`PlaybackOptions::isolate_input`, which the demo's `--test-*` modes turn on; `D3D12Renderer::SetInputIsolation`). It was introduced after a full-suite run in which a real mouse click hit a test window 0.88 s in and seeked it to 46.16 s.

- The desktop's input always arrives through the thread's message queue. While isolation is on, `Messages()` drops every keyboard, mouse (client and frame), wheel, hover/leave, raw input, touch, pointer, gesture and input-method message it takes from the queue (`IsolatedInputs()` counts them).
- A click does not activate the window either: `WM_MOUSEACTIVATE`, which the system sends before a click, is answered `MA_NOACTIVATEANDEAT`, so the click is discarded.
- Scripted input is unaffected. The tests' keys are `SendMessage` calls from the window's own thread, which call the window procedure directly and never pass through the queue; the dock script calls `InputHud`. A test that must post input through the queue uses `PostTestInput(message, w, l)`: the message is tagged, admitted exactly once, and not translated (a test posts the `WM_CHAR` it means).
- `media_playback_isolation_test` (`--test-isolation`): another thread posts, twice, what a stray desktop user would do (a drag and clicks on the timeline and the transport button; Space, arrows, End, N, F, F11, Up, `/`; the wheel) and sends the `WM_MOUSEACTIVATE` of a click. All 30 messages are dropped, the activation is refused, and nothing changes: no seek, no pause, no fullscreen, no quality change, no command bar, volume as the harness set it (four tagged Down keys: 80 %), the window never in the foreground, the 72 pictures shown with 0 drops. With `--allow-desktop-input` (the negative control) the same input seeks 4 times, changes the quality mode and opens the command bar.

## The A–B loop's bracket (2026-10-21)

`HudUniforms::loop` (it was `reserved1`) carries the [A–B loop](seeking.md#ab-loop-2026-10-21) as two 16-bit positions along the track (`timeline::LoopRegion::Packed`). `HudComposite.hlsl` draws a mint bracket at A opening right and one at B opening left, reaching 5 px beyond the track, with the stretch between them lit (32 %). A alone (B not marked yet) is its bracket only. `KineticHud::SetLoop` reveals the dock when the loop changes, so the bracket is seen. `media_ab_loop_tests` draws it offscreen and checks it: mint at A and B, the stretch lit along its edge row, nothing changed elsewhere, no mint without a loop. `MEDIA_LOOP_DUMP=<folder>` saves the dock as a BMP.

## Status cues (`ui::StatusCue` + `render::StatusPipeline` + `StatusComposite.hlsl`)

A transient glass pill at the top centre of the window names what just changed: `Audio · Hebrew (עברית) · AAC stereo`, `Subtitles · English`, `Subtitles off`, `No German subtitles`, `French audio cannot play here`, `Audio · now on the default output device`, `Audio · 8-channel AAC cannot play here · picture only`, and each voice command heard (`“pause”`, or `“volume 150” · not done` in amber). Engine code calls `D3D12Renderer::ShowStatus(text, icon, tone)` from any of its paths; [tracks](tracks.md) lists the track cues.

- **Geometry.** The glass, rim, drop shadow and the five icons (speaker, captions, microphone, headphones, warning) are signed-distance shapes evaluated per pixel, antialiased over 0.8 px at any DPI. The warning tone adds an amber rim and icon. The listening tone (the wake word heard alone: `Avion · Listening…`, [voice](voice.md)) adds a cool rim and a soft halo outside the pill whose strength breathes on a 1.6 s raised cosine (`StatusUniforms::pulse`; steady under reduced motion). The pill sits `kTop` (26) scaled pixels below the top edge, 38 px high, and as wide as its text plus the icon and padding (112–600 design px). It follows the dock's DPI scale.
- **Text.** A dedicated thread shapes and rasterises the text with DirectWrite (bidirectional, font fallback: Hebrew, Arabic and CJK are drawn in their own fonts) into one byte of coverage per pixel, at the pill's exact pixel size; the shader reads it one to one, never filtered. A cue appears only once its raster exists (`Ready`): it is seen whole or not yet, never as an empty pill. Text is copied (384 bytes at most since 2026-10-21, 160 before; cut at a character boundary; a line feed starts a line, other control characters become spaces, ill-formed UTF-8 a question mark).
- **Motion.** A closed-form function of the time since the raster was ready: 0.22 s in (ease-out cubic, sliding 8 px down), held 2.5 s (or the hold the caller asks for: the listening cue stays exactly as long as the wake word's window), 0.5 s out (ease-in cubic). A new cue while the pill is up keeps it up, restarts the hold, and the width glides to the new text's over 0.15 s; the old text stays until the new raster arrives. Reduced motion: no slide, 0.1 s fades.
- **Panel (2026-10-21).** A cue with line feeds is a glass panel: up to `kMaxLines` (7) lines, `kLineHeight` (20) design px each, as wide as `kMaxPanelHalfWidth` (420 design px) and the window allow. Its top edge stays where the pill's is, its corners keep the pill's radius, and the icon sits on the first line. Empty lines and a last line feed are dropped. The panel takes its new height when its new text's raster is ready, never before (`TextRect` for the text on screen, `PendingRect` for the one being rasterised). The raster area is 2048 × 384 px. The telemetry readout uses it ([session](session.md#telemetry-readout-ai-task-a04)).
- **Refresh.** While a cue fades in or out the renderer refreshes at the display's rate, also while paused; while held it refreshes only with the video. The pass is recorded after subtitles and the dock, under the [command bar](command-palette.md). Without the pipeline (its preparation failed) cues are dropped; playback is unaffected.

## Verification

| Test | Checks |
|---|---|
| `media_hud_status_tests` (model also GCC `-Werror`, ASan/UBSan) | **Model:** layout; panels (lines, the cap at 7, empty lines dropped, the height taken with the text, the top edge kept, the width inside a narrow window); nothing shown before the raster, a stale raster ignored; ease-out in (0.875 at half time, monotonic), the hold, ease-in out, gone after 3.22 s; a new cue restarting the hold and gliding the width (87.5 % at half the glide); empty text taking the pill down as a fade; width limits; text cut and cleaning; reduced motion and DPI; track labels (with a native name, a custom name, an unknown language, a pill too small for some pieces); zero allocations. **Pixels** (production pipeline, offscreen): a Latin cue's ink inside the pill with its right padding, dark glass, no pixel changed outside the pill's shadow box; a hidden cue changes no pixel; a Hebrew cue in its own font; the amber of a warning, absent from an info cue; a cue mid-fade fainter than held; a three-line panel's three bands of ink, its top where the pill's was; zero allocations. `MEDIA_STATUS_DUMP=<folder>` saves the images. |
| `media_chapter_scene_tests` (dock part) | A drag emits `Scrub` on each move that changes the target (none for a move along the same position), `Seek` on release; the preview cue glides in monotonically, settles exactly, glides out, and snaps under reduced motion; `TimelineMarks` packing and limits |
| `media_hud_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | Hit regions, transport tap, cancel, drag-and-release seek, click-to-seek clamping, commit and arrival, rejection, no duplicate intents, commit timeout, Tab/Enter/arrow/Home/End, reduced motion, invalid input, reconfiguration, reveal-only first click, zero allocations. **Volume pill:** baseline report shows nothing; a change reveals dock and pill, the fill glides monotonically and settles exactly, both fade after 1.5 s; the mute cue persists without keeping the dock awake; wheel steps and clamping; drag along the track, release and cancel; hidden pill takes no clicks; reduced motion; non-finite volume; narrow dock |
| `media_hud_gpu_tests` | Production PSOs on an offscreen target:<br>• time-invariance while paused and playing<br>• Play/Pause silhouettes at discriminating pixels, a distinct mid-morph<br>• timeline hover/scrub response<br>• invisible when faded<br>• dark dock over white frames<br>• no scissor seam (last column and row equal the white background, while the shadow exists inside)<br>• captions shaped in six scripts<br>• 640×360 layout<br>• volume pill: nothing drawn when faded, fill up to the level and not beyond, time-invariant, amber mute cross in place of the waves, dimmed level while muted, half-faded state<br>• timeline marks: a chapter tick and a scene dot where they belong and nothing else; without marks the dock is unchanged<br>• scrub preview: the knob a ring (centre 157 against 245 exact), an amber diamond at the previewed picture<br>• no D3D12 debug errors, zero allocations |
| `media_hud_window_tests` | The production Win32 procedure: core click, Space/repeat, cancel, leave, wheel notches (ignored until the volume is known, then one step each, half notches accumulating), the pill composited, controls-only composites while paused, no video-counter increments, zero allocations |
| `media_playback_hud_test` | Real H.264/AAC playback with a scripted tap, a timeline drag released while paused (one accepted seek, landing presented, scrub offset back to exactly 0) and resume; no drops, zero allocations |

With a directory argument, `media_hud_gpu_tests` saves its composites as BMP files (`dock-dark`, `dock-bright-rtl`, `dock-multilingual`, `dock-scrubbing`, `dock-volume`, `dock-muted`, `dock-chapters-preview`, `dock-small`) for visual review. The GPU tests read back only synthetic UI output, never a decoded video surface.

```powershell
ctest --test-dir build -C Release -R "hud|subtitle" --output-on-failure
.\build\Release\media_hud_gpu_tests.exe .\review   # optional review images
```

## Open

- Below roughly 250 px of width, the clocks would overlap the transport button. The window's minimum size is 320 × 180 DIPs.
- The volume pill shares the gap right of the transport button; docks narrower than 472 design px (windows under about 400 px wide) show the dock but no pill.
- The volume is not persisted between runs, and held Up/Down keys do not repeat.
- Status cues are not announced to screen readers (no UI Automation provider yet), and only one cue is shown at a time: a newer one replaces it.
- An interactive on-screen inspection with real input has not been automated. The review images come from the offscreen pass using the production shaders.
