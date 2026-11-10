# Command bar (`ui::CommandPalette`, `render::PalettePipeline`)

A floating text field in the player's window. Type a command in any of the grammar's nine languages, or any words to look for in the subtitles, and press Enter.

| Key | Action |
|---|---|
| `/`, Ctrl+F, Ctrl+K | Open the bar |
| Any text | Typed into it, in any script; input methods compose in place |
| Enter | Execute: a command if the text reads as one, otherwise a subtitle search |
| Escape, or a click outside the bar | Dismiss it |
| Backspace, Ctrl+Backspace | Delete a character, a word |
| Ctrl+V | Paste |
| F11, Alt+Enter | Fullscreen (still available while typing) |

While the bar is open, every other key types: Space does not pause, M does not mute, and Escape closes the bar, not the window.

## What Enter does

The text goes to `PlaybackCoordinator::Submit`, which applies [the command-or-quote rule](intents.md#a-command-or-else-a-quote-interpret-submit):

- `暂停`, `volumen 40`, `10 Sekunden zurück`, `перейди на 1:30`: the command runs and the bar fades away.
- `the quiet between`, `עברית`: no command, so the text is searched in the subtitles and playback lands on the first cue containing it ([subtitle search](subtitle-search.md)).
- `"pause"`: quotation marks force a search for the word itself.
- Nothing found, or a command the media refuses (`forward 2 hours` in a short film): the bar stays open, its rim turns amber and it shakes briefly. The text is kept for correction.

**Before Enter.** As the text changes, the bar shows what Enter would do (`PlaybackCoordinator::HandlePalettePreview`, the same rule without executing):

| Shown | Meaning |
|---|---|
| Chevron on the left, Return arrow on the right, accent rim | The text reads as a command |
| Magnifier, and a number on the right | A search; the number is how many cues contain the text (up to 999) |
| Magnifier, amber `0` | Nothing would be found, or the command would be refused |

The engine does not seek while the text is being typed: a half-typed command is also a valid quote (`pa…`), and seeking on every keystroke would move playback away from where the user is.

## Parts

| Part | Role |
|---|---|
| `ui::CommandPalette` (portable, `media_ui`) | The model: the text as UTF-8, the input method's composition, open/close motion, caret blink, refusal shake, layout. No drawing, no execution |
| `render::PalettePipeline` (Windows) | The GPU pass, and the text thread that rasterises the text |
| `D3D12Renderer::PaletteMessage` | Win32 input: keys, characters, input-method messages |
| `PlaybackCoordinator::Submit` | Execution, through `ai::LocalIntentDispatcher` |

### Text input

- **Characters.** `WM_CHAR` delivers UTF-16 code units, one message each; the model pairs surrogate halves (`Utf16`). `WM_UNICHAR` delivers a whole scalar. Control characters never become text: the keys that produce them (Enter, Backspace, Escape, Ctrl+letter) are handled as `WM_KEYDOWN`.
- **Opening is by character**, not by key code: `/` wherever the keyboard layout has it.
- **Input methods.** The window has no input context while the bar is closed (`ImmAssociateContextEx`), so playback shortcuts work under a Chinese, Japanese or Korean input method too. Opening the bar attaches the default context.
  - `WM_IME_COMPOSITION` with `GCS_COMPSTR`: the composition string is shown after the committed text, underlined, and replaced as it changes.
  - `GCS_RESULTSTR`: the result string is inserted and the composition ends.
  - `WM_IME_SETCONTEXT` clears `ISC_SHOWUICOMPOSITIONWINDOW`: the input method draws no composition window of its own. Its candidate window is placed under the caret (`ImmSetCandidateWindow`).
  - Losing the focus or closing the bar cancels an unfinished composition.
- **Right-to-left.** The line's direction is that of its first strong character, as in any text field: Hebrew and Arabic text is laid out from the right, with the caret at its left end. Mixed text follows the Unicode bidi algorithm (DirectWrite).
- **Buffers.** The text (512 bytes, the command limit) and the composition (128 bytes) live in the engine's arena, handed over once in `PreparePalette`. Input beyond the capacity is dropped on a scalar boundary.

### Drawing

- **The bar** is analytical geometry in `PaletteComposite.hlsl`, like the dock: a rounded glass box with a rim and a shadow, the magnifier or chevron, the match counter in the dock's monoline numerals, the Return arrow and the caret. No texture.
- **The text** cannot be analytical: it may be in any script. DirectWrite lays it out (font fallback, bidi, Arabic joining) and rasterises it on a text thread, at the lane's exact pixel size, as one byte of coverage per pixel.
  - The raster is greyscale at gamma 1 (no ClearType fringes) and is read by the shader one pixel to one pixel, never filtered. At 150 % scaling the text is rasterised at 33 px, not stretched from 22 px.
  - The shader reads the coverage straight from an upload buffer through a root buffer view: no texture, no descriptor heap, no copy on the GPU.
  - Three regions of that buffer rotate, each guarded by the fence of the last composite that read it, so a raster is never written while the GPU may still read it.
  - Text longer than the lane scrolls so that its end, where the caret is, stays inside.
- **Threads.** The text thread and the presentation thread meet in two SPSC rings (request, completion). Neither waits for the other. At most one raster is in flight; keystrokes that arrive meanwhile coalesce into the next one.
- **Motion.** Opening: a critically damped glide, nine tenths of the way in under 0.1 s, settled in about 0.2 s. The bar arrives from 10 design pixels above; the text travels with it by whole pixels. Dismissal is the same in reverse: the whole pass, geometry and text, is scaled by the visibility. With reduced motion (`SPI_GETCLIENTAREAANIMATION` off) it appears and disappears at once and does not shake.
- **Layout.** Centred horizontally, its centre at 38 % of the window's height; up to 640 design pixels wide, narrower in a narrow window; the dock's scale rule (DPI and window size). The bar is drawn above the video, the captions and the dock.

### Device loss

The pass is rebuilt with the renderer (`PalettePipeline::Recreate`) and the text is rasterised again. If the pass cannot be rebuilt, the bar is not drawn; keys and commands still work.

## Memory

| Buffer | Size | Where |
|---|---|---|
| Text and composition | 640 bytes | Arena |
| Raster staging (up to 2048 × 256 px) | 512 KiB | Arena |
| Request text (UTF-16) | 1.3 KiB | Arena |
| Coverage regions read by the shader | 3 × 512 KiB | GPU upload buffer |
| DirectWrite's bitmap target | 2 MiB | System (created once, by the text thread at start) |

Nothing is allocated from the process heap by the engine's code while typing, parsing or drawing. DirectWrite allocates internally when it lays text out; that happens on the text thread, outside the allocation probes, as for the subtitles.

## Verification

| Test | Checks |
|---|---|
| `media_command_palette_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **Typing** in Hebrew, Arabic, Chinese, Japanese, Russian, German, Spanish, French, Korean, Devanagari and Thai, and surrogate pairs: the UTF-8 is exact and well formed. Controls, unpaired surrogates and non-characters are dropped. Backspace over 1- to 4-byte scalars; word deletion; capacity reached on a scalar boundary. **Input methods:** composition shown apart from the text, replaced, committed, cancelled; pasted line breaks and tabs become spaces; long compositions and pastes are cut on a scalar boundary. **Motion:** layout at eight window sizes (bar inside the window, lane inside the bar); opening and closing are monotonic, without overshoot, and settle exactly; caret phases; refusal shake bounded and decaying; reduced motion; backward time. Zero allocations. |
| `media_palette_gpu_tests` | The production pass on an offscreen target, checked on pixels. **Text:** Latin left-aligned with the caret after it; Hebrew and Arabic right-aligned with the caret at the left; Chinese and Japanese from fallback fonts; Cyrillic and accents. Typing and deleting returns a pixel-identical image. A composition is underlined, committed text is not. Long text scrolls (left-to-right and right-to-left) and never draws outside its lane. **Bar:** glass and rim, magnifier, chevron and Return arrow, match counter digits, amber miss, amber rim on refusal, no scissor seam over white, fade between the open and the empty image, nothing left at the end. **150 % scaling:** the text is 1.5 times as wide. A rebuilt pass redraws identically. No D3D12 debug message, zero allocations. Measured: keystroke to finished raster about **0.3 ms** mean, 0.4–0.5 ms slowest (40 keys, Release, two runs). |
| `media_palette_window_tests` | Real messages through the production window procedure. `/`, Ctrl+K and Ctrl+F open the bar (and Ctrl+F does not toggle fullscreen); while open no shortcut fires (M, D, F, N, C, Space, Tab, arrows); characters in five scripts and a surrogate pair through `WM_CHAR`, astral scalars through `WM_UNICHAR`; control characters ignored; held Backspace repeats, held Enter does not; Ctrl+Backspace. Enter submits the exact text; acceptance closes and fades, refusal keeps the bar open and shaking; Escape closes the bar and not the window; a click outside dismisses without reaching the dock. **Input method:** the window has no input context while closed and one while open; composition and result strings written into the real input context and delivered by `WM_IME_COMPOSITION` (composition shown, replaced, committed; a result without a composition flag; a cancelled composition; focus loss). Resize and F11 while open. Zero allocations. |
| `media_playback_palette_test` | Nine texts typed into the window during real H.264/AAC playback, as key and character messages: `音量40` (after a typo and Backspace), `пауза`, `再生`, `עברית` (no command: searched, lands on its cue), `zurückspulen 1 Sekunde`, `xyzzy` (not found: refused, dismissed with Escape, the window stays), `توقف`, `chercher quiet` (lands on its cue while paused), `reproducir`. Each leaves the expected volume and pause state; the three seeks land exactly with the target's cues; the bar was composited with its rasterised text; no dropped frame, zero allocations. Measured: 3–15 µs from Enter to the engine action. |

```powershell
ctest --test-dir build -C Release -R "palette|intent" --output-on-failure
.\build\Release\media_palette_gpu_tests.exe .\review   # optional review images (palette-*.bmp)
.\build\Release\media_player_demo.exe film.mp4 --subtitles film.srt   # then press /
```

## Open

- The caret is always at the end of the text: no arrow keys, selection, or editing in the middle.
- Enter always lands on the first matching cue; there is no next-match key yet.
- Paste (Ctrl+V) is not covered by an automated test: a test would overwrite the user's clipboard.
- Input methods are tested by writing composition strings into the window's real input context and sending the messages an input method sends. No test drives an actual Chinese or Japanese input method's candidate window; its placement under the caret was not inspected on screen.
- The bar itself was reviewed on the offscreen renders (`palette-*.bmp`), not on a live window with a real keyboard.
- Colour emoji are drawn as monochrome glyphs.
- While the bar is open it is recomposited at up to 120 Hz, as the dock is while visible (with the pictures instead, when one is due within a display refresh: 60 fps video on a 60 Hz display).
- DirectWrite runs on the text thread during playback (system allocations, outside the probes). A script whose fallback font was never loaded costs its font load on the first keystroke in that script; five script families are loaded ahead of time (Latin, Hebrew, Arabic, CJK, Cyrillic).
- Lanes wider than 2048 px or taller than 256 px are cropped (the raster then covers the lane's left part and its vertical middle). The lane is 524 × 56 design pixels, so this starts at a layout scale of 3.9, which needs more than 300 % display scaling. Not tested at that scale.
