# Window Resizing, DPI and Fullscreen

The player window can be resized from its edges, moved between monitors with different DPI, and switched to borderless fullscreen. Video, the glass dock and subtitles adapt on the next presented image. Nothing is allocated and nothing blocks on the GPU in the process. Video keeps playing during an interactive drag.

## Window

- **Frame.** `WS_POPUP | WS_THICKFRAME`, with `WM_NCCALCSIZE` returning 0. The frame stays invisible (the whole window is client area), but Windows' native sizing remains available.
- **Hit testing.** `WM_NCHITTEST` returns resize edges and corners within 6 DIPs of the border, a drag strip 24 DIPs high along the top, and client area elsewhere, so the dock still gets mouse input. In fullscreen there are no edges or drag strip.
- **Minimum size.** `WM_GETMINMAXINFO` sets 320 × 180 DIPs.
- **DPI.** The window is per-monitor DPI aware (v2): the renderer creates it under `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`, and the demo sets the same for the process.
  - `WM_DPICHANGED` adopts the new scale (`DpiScale()` = DPI / 96) and the rectangle Windows suggests.
- **Fullscreen.** F11, F or Alt+Enter call `ToggleFullscreen()`. The window takes its monitor's rectangle (borderless fullscreen, eligible for DXGI independent flip). The same keys restore the previous rectangle. DXGI's own Alt+Enter handling remains disabled.

## Swap chain

`WM_SIZE` records a pending size (`RequestResize`); the swap chain is resized from `Present`, never from the window procedure.

- **Applying it.** `ApplyResize` waits, without blocking, until the staged present has completed and every composite context's fence has retired. Until then presentation returns `WouldBlock`.
  - It then releases the three back buffers, calls `ResizeBuffers(3, w, h, RGBA8, FRAME_LATENCY_WAITABLE_OBJECT)`, and re-creates the RTVs in the existing heap.
  - It re-lays out the dock (`KineticHud::Resize`) and requests a redraw, so a paused picture is recomposed at the new size at once.
- **Unchanged.** The frame-latency waitable object, command contexts, history and RGB slots are kept.

## Video: letterboxing without reallocation

- **Slot capacity.** At `Prepare`, the coordinator passes the stream's display size. The RGB slots and the history texture are created at the largest size the video can occupy: its aspect fitted into the virtual desktop (`SM_C?VIRTUALSCREEN`), and at least the initial window.
  - On this machine (two 1920 × 1080 monitors) that is 1080 × 1080 for the square test fixture, and 1920 × 1080 for a 16:9 stream.
  - No GPU resource is created or resized after `Prepare`.
- **Conversion.** Each frame is converted, with the usual reconstruction and sharpening, at `Fit(stream aspect, current window)` into the top-left of its slot.
  - A fit within 2 px of the window snaps to it, so rounding never leaves a sliver of black bar.
- **Composition.** The picture is centred. Letterbox or pillarbox bars are cleared to black only when the picture does not cover the window.
  - **Copy.** A picture converted for the current window size is copied (`CopyTextureRegion`).
  - **Scaled blit.** A picture converted for another size is stretched into place by `PictureBlit.hlsl`: one bilinear draw from an SRV of the slot or history texture. Two cases produce such pictures:
    - frames converted just before a resize (the renderer converts up to 16 frames ahead);
    - the held picture when recomposing while paused.
  - `ScaledComposites()` counts these draws. The video therefore takes its new size on the very next image, even while paused.
- **`LastVideoRect()`** reports the letterboxed rectangle of the last composite.

## Controls and captions

- **Dock.** `KineticHud::LayoutScale = dpi × clamp(min(width_dip / 1280, height_dip / 720), 0.8, 1.25)`.
  - The dock is 280 × 52 design px, scaled. It narrows only when the window is too narrow, keeping 16 scaled px on each side.
  - It sits 26 scaled px above the bottom edge.
  - A resize keeps interaction state: a drag in progress keeps its target and commits against the new geometry.
- **Captions.** Shaped once in layout space and scaled per draw (see [subtitles](subtitles.md)).
  - Font size: `min(max(height / 24, 18 × dpi), 0.92 × width / 34 em)`.
  - The lowest caption rides 20 scaled px above the dock, and 1.3 font sizes above the bottom edge when the dock is hidden.

## Interactive drags (modal loop)

While the user drags an edge or the drag strip, Windows runs a modal loop inside `DispatchMessage`, and the coordinator's own loop is suspended. Previously, video froze until the mouse was released, for moves as well as resizes.

Now:
- `WM_ENTERSIZEMOVE` starts a `USER_TIMER_MINIMUM` timer, and `WM_EXITSIZEMOVE` stops it.
- Each `WM_TIMER` calls the pump installed with `ConfigurePump`. `PlaybackCoordinator::Pump` runs up to 8 streaming steps: convert one frame, present.
- The streaming step is the same `Stream()` the main loop uses, guarded against re-entry. Test scripts do not run inside it.

## Verification

| Test | Checks |
|---|---|
| `media_hud_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | Layout at 10 sizes and DPIs: the scale rule, centring, the 26-px bottom margin, the 280:52 capsule or a 16-px side margin, hit regions; a drag surviving a DPI resize and committing on the new geometry; NaN DPI |
| `media_layout_gpu_tests` | Production dock and caption passes at 11 sizes, 640×360 to 3840×2160 and 2160×3840, including 21:9 and 9:16, at 100–200 % DPI, measured from pixels:<br>• dock box within 2 scaled px of the model, capsule proportions within 3 %, bottom margin and centring<br>• caption width and height proportional to the font size<br>• captions centred inside the frame, at least 20 scaled px above the dock<br>• 0 allocations |
| `media_hud_window_tests` | Through the production window procedure:<br>• `WM_SIZE` → swap chain 960×540 and a re-laid-out dock<br>• edge, corner and drag-strip hit tests<br>• F11 to monitor-sized fullscreen with no edges; Alt+Enter back to 960×540<br>• `WM_DPICHANGED` to 144 DPI with a 1440×810 rectangle, then back to 96 DPI<br>• pump ticks only between `WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE`<br>• 0 allocations |
| `media_playback_resize_test` | Real H.264/AAC playback with captions and the dock, through five steps: 960×540, 540×960 (9:16), 1600×686 (21:9), F11 fullscreen (1920×1080), and F11 back to 1600×686. Each step checks the swap-chain size, the centred letterboxed video, the dock's scale and margins, and that frames kept presenting. Over the whole run: 72/72 frames, 0 drops, 0 allocations |

A Debug run of `--test-resize`:

```text
Resize 1: 960x540 video 540x540 at (210,0) dock scale 0.800 left 256.0 bottom margin 20.8 frames 10->19 ok
Resize 2: 540x960 video 540x540 at (0,210) dock scale 0.800 left 46.0 bottom margin 20.8 frames 19->29 ok
Resize 3: 1600x686 video 686x686 at (457,0) dock scale 0.953 left 533.2 bottom margin 24.8 frames 29->38 ok
Resize 4: 1920x1080 fullscreen video 1080x1080 at (420,0) dock scale 1.250 left 610.0 bottom margin 32.5 frames 38->48 ok
Resize 5: 1600x686 video 686x686 at (457,0) dock scale 0.953 left 533.2 bottom margin 24.8 frames 48->72 ok
Swap-chain resizes=5 scaled composites=188 slot capacity 1080x1080
```

The resize steps come every 0.4 s, which is shorter than the 16-frame conversion lookahead. Most frames in this run were therefore converted for an earlier size and went through the scaled blit, and the dock stayed visible (always-visible test mode), so controls-only recomposites were scaled as well.

The allocation probes cover the application's C++ allocation entry points and, in Debug, the CRT. `SetWindowPos`, `ResizeBuffers` and the DWM allocate inside the OS and driver, which the probes do not measure.

## Limits

- **4K and high DPI.** The two monitors here are 1920 × 1080 at 100 %. 4K windows and 150–200 % DPI are verified offscreen (`media_layout_gpu_tests`) and by synthesised `WM_DPICHANGED`, not on real hardware.
- **Interactive dragging.** The modal-loop pump is verified by messages; no real mouse drag was automated.
- **Transient scaling.** For up to about 0.67 s after a resize at 24 fps (the 16 converted-ahead frames), pictures are stretched bilinearly instead of converted at the new size, without edge-directed reconstruction or sharpening.
- **Slot memory** now follows the desktop rather than the initial window: 16 slots plus history at 1920 × 1080 RGBA8 is about 141 MB, against about 79 MB for a 1440 × 810 window before. A 16:9 stream on a 4K desktop would need about 564 MB. Fewer slots, or sizing them for the monitor rather than the whole desktop, are the obvious reductions.
- **Exclusive fullscreen** is not used; borderless fullscreen relies on DWM independent flip.
