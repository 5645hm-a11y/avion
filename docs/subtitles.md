# GPU Subtitles: SRT/WebVTT, DirectWrite Shaping, Paged SDF Atlas

Sidecar subtitles are parsed in `Preparing`. Everything else is prepared off the startup path and off the presentation thread:
- a shaping thread lays out cues with DirectWrite and outlines glyphs on request;
- the GPU turns outlines into signed-distance cells of a **paged atlas**, inside the composite that needs them.

The presentation thread selects cues, keeps the right glyphs resident and draws. It does no font work, no allocation and no CPU pixel work, and it never waits.

**Subtitles are optional.** No subtitle problem stops playback. A missing, unreadable, malformed or unrenderable track sets `SubtitleStatus::Disabled`.

**Every glyph is drawn as itself.** The atlas is a cache of 2048 cells, not a limit on the track: a track with more distinct glyphs pages them in and out. Nothing is replaced by a dot. A cue is drawn only once all its glyphs are resident, so text appears whole or not yet.

```text
Preparing:        file ─► SubtitleTrack (arena copy, tolerant parse, sorted cue table)
                       ─► SdfSubtitlePipeline::Prepare: fixed arena tables, start the shaping thread   (0.3 ms)
Shaping thread:   GPU objects (atlas, upload ring, pipelines), then DirectWrite layout per cue line,
                  from the playback position onwards; glyph outlines on request into 64 staging slots
Presentation:     Update (every 8.3 ms): mark the glyphs of the cues shown now and in the next 10 s,
                  request the missing ones (free cell, else the least recently needed one)
                  Composite: generate outlined glyphs into their cells (compute, at most 64 per composite)
                           ─► draw the cues whose glyphs are all resident ─► glass dock ─► Present
```

## Status and failure handling

`PlaybackCoordinator::SubtitleState()` reports one of these states; `SubtitleError()` holds the native cause for `Disabled`.

| State | When | Effect |
|---|---|---|
| `Off` | No subtitles were requested | — |
| `Ready` | Normal | Every glyph is drawn as itself, whatever the number of distinct glyphs |
| `AtlasExhausted` | A hard limit was hit: more than 32 768 distinct glyphs, more than 262 144 shaped glyphs, a line DirectWrite cannot lay out (sideways runs), or a glyph it cannot outline | Those glyphs are left out and counted in `Statistics::dropped`. Nothing is substituted |
| `Disabled` | The file cannot be opened or read (for example `0x80070002`), is empty or over 8 MiB, contains no valid cue (`ERROR_INVALID_DATA`), or DirectWrite/D3D12 setup fails | No subtitles |

The demo prints the state after every run that requested subtitles:

```text
Subtitles: ready (native 00000000); cells=2048 resident=2048 distinct=4203 generated=4202 evictions=2154 dropped=0 composites=209 cues shown=60 of 60
Startup: subtitle pipeline prepared in 0.31ms; first frame 13.1ms after Run
```

## Input and parsing (`media::subtitle`)

- **Input.** `--subtitles file.srt|file.vtt`: a local UTF-8 file of at most 8 MiB, read in `Preparing` into the playback arena. `--test-subtitles` and `--test-subtitle-overflow` use built-in tracks instead.
- **`CueReader`** is an allocation-free cursor over an immutable buffer.
  - Formats: SRT (numeric identifiers, `,` or `.` milliseconds) and WebVTT (header block, optional identifiers, `MM:SS.mmm` or `HH:MM:SS.mmm` times).
  - Cue settings are accepted, but default placement is used. `NOTE`, `STYLE` and `REGION` blocks are skipped. A leading BOM is ignored.
  - Invalid: bad times, `end <= start`, empty or oversized text, malformed UTF-8 (overlongs, surrogates, truncation, U+0000).
- **Tolerance.** `SubtitleTrack::Prepare` is tolerant, as other players are.
  - After an invalid block, `CueReader::Resync()` skips to the block's blank line, and parsing continues. `Stats().skipped` counts such blocks; `ErrorLine()` is the first bad line.
  - Up to 16 384 cues are kept; later ones are counted in `Stats().dropped`.
  - More than 4 simultaneously active cues are accepted. `Select` then returns the 4 latest-starting ones, and `Stats().max_active` reports the peak.
  - `Prepare` fails only when no valid cue remains, the source is empty or over 8 MiB, or the arena is exhausted. A garbage input (for example a binary file) fails without consuming arena memory.
- **Table.** Cues are copied into the arena and sorted by start time, with a running `prefix_end` (the maximum end time so far).
- **`Select(time)`** returns the active cues in start order. It binary-searches the start times, then walks back while `prefix_end > time`, and never allocates. The walk is O(log n + k) for typical tracks, but a very long cue that overlaps many later cues lengthens it: at most O(n).
- **Intervals** are half-open, `[start, end)`. Back-to-back cues switch on the same 100 ns tick.
- **Channels.** One `SubtitleTrack` holds several sources (`Prepare(arena, span<string_view>)`), each a *channel*: the sidecar file is channel 0, each embedded `tx3g` track of the MP4 the next ([tracks](tracks.md)). Every cue carries its channel; `Show(mask)` (one atomic store, any thread) chooses which channels are drawn, searched and prefetched, so a language change re-reads and re-shapes nothing. `Shown()` is the mask; `ChannelCues(c)` counts a channel's cues.
- **Embedded timed text.** `tx3g` samples of a local file are found by table lookup (`IsobmffParser::SampleAt`) and read in `Preparing` with positioned reads of just their bytes on a second file handle (through the block source, interleaved samples would mean reading the whole file). Each sample's 16-bit length and UTF-8 text (or UTF-16 with its byte order mark) becomes one cue lasting the sample's duration; empty samples are gaps, styling boxes are ignored. The cues are written as WebVTT text into the arena and parsed like any other source. Remote files' embedded tracks are not imported.
- **Search.** The parsed track is also searchable by quote, with a seek to the matching cue: see [subtitle search](subtitle-search.md). UTF-8 decoding lives in `media::text` (`text::Utf8`), shared with that search and with the command grammar.

## Shaping (shaping thread)

`SdfSubtitlePipeline::Prepare` starts one thread that owns DirectWrite: an isolated factory with Segoe UI Medium, centred. It is the only thread that touches fonts. Layout happens once per cue, in a resolution-independent **layout space**: a 48 px em (`kLayoutEm`, the atlas em), with lines wrapped at 34 em (`kWrapEm`).

- **Order.** The presentation thread publishes the cue playback is at. The thread shapes that cue first, then onwards, wrapping around, until every cue is shaped. A seek only changes where it continues.
  - 80 cues of 57 CJK glyphs take about 50 ms in total. A cue is published with one release store once its glyphs are written.
- **Markup.** `<…>` tags are stripped; the entities `&amp; &lt; &gt; &nbsp; &lrm; &rlm; &quot; &apos;` are decoded. A cue containing only markup is kept, but draws nothing and takes no space.
- **Bidi.** Each cue line is laid out as its own bidi paragraph (one `IDWriteTextLayout` per line).
  - The line's base direction is its first strong character (`GetStringTypeW(CT_CTYPE2)`). A line with no strong character takes the cue's direction, and a cue with none is left-to-right.
  - This matches how browsers render WebVTT (`unicode-bidi: plaintext`). It keeps trailing punctuation and embedded numbers on the correct side in bilingual cues such as `Hello` / `שלום!`.
- **Glyphs.** A custom `IDWriteTextRenderer` records every shaped glyph: the left edge of its advance box, its baseline, its run scale and a key for its distinct `(font face, glyph)` pair. It handles fallback fonts, Arabic joining forms and right-to-left pen advances.
  - A 65 536-slot open-addressed table holds up to 32 768 distinct pairs.
  - A glyph with no ink (a space) is recognised from the font's metrics (a black box with no area) and never takes a cell.
- **Outlines, on request.** When the presentation thread asks for a glyph, the thread outlines it with `GetGlyphRunOutline` at the 48 px em into one of 64 staging slots (4096 edges each), fitted into a 96 × 96 cell with an 8 px pad.
  - Curves are flattened by adaptive de Casteljau subdivision: under 0.08 atlas-pixel flatness, at most 10 levels deep. An outline that would not fit its slot is flattened more coarsely instead of failing.
  - Requests are served before shaping continues: they are for cues about to be shown.
- **Two SPSC rings** connect the threads: requests `{glyph, slot}` one way, completions `{glyph, slot, edge count, winding, cell transform}` the other. The thread sleeps on an event when it has nothing to do. It allocates nothing from the process heap; DirectWrite's own allocations are the system's.

## Atlas paging and draw

- **Atlas.** One R32_FLOAT texture, 32 cells wide. Its capacity is what the track's text can need, at most 2048 cells (`kAtlasCells`): a three-cue track gets a few rows, a film gets the full 72 MiB. It is cleared once to "far outside" in the first composite.
- **Residency** (`Update`, presentation thread, every 8.3 ms and in every `Record`).
  - Needed glyphs are those of the cues active now and of the cues starting within 10 s (`kPrefetch`, at most 64 cues).
  - All of them are marked first, then the missing ones are requested: the glyphs shown now first, then in start order. Marking first keeps an update from evicting a glyph it is about to need.
  - A request takes a free cell, else the cell of the **least recently needed** resident glyph that this update does not need. Without a candidate (every cell is needed), the request waits for a later update.
- **Generation.** `Record` copies the outlines that have arrived into its upload region and records one compute dispatch (`SubtitleSdf.hlsl`, `BuildAtlas`) for them: at most 64 cells per composite, the cues shown now first.
  - Each texel holds the exact distance to the glyph's line segments, signed by non-zero or even-odd winding, following the font's fill mode.
  - The dispatch is part of the composite's own command list, between two transitions of the atlas. There is no separate upload, no fence wait and no stall.
- **Upload ring.** Four regions in one persistently mapped upload buffer hold, per composite, the instances drawn, the cells generated and their edges. A region is reused once the fence of the composite that used it has completed.
- **Draw.** For each active cue whose glyphs are all resident, bottom-most first, `Record` writes its quads into the region, sets 12 root constants and calls `DrawInstanced(6, glyphs)`.
  - A cue with a glyph still on its way is not drawn in that composite. `Drawable` reports it as soon as its outlines have arrived; that changes the renderer's stamp and triggers a refresh (at most 8.3 ms later).
  - The pixel shader (`SubtitleComposite.hlsl`) filters the distance itself, from four `Load`s and the texel position inside the glyph's own cell. A glyph therefore renders **bit-identically whichever cell it was paged into**: neighbouring cells never bleed in, and the weights do not depend on the cell's position.
  - It antialiases over `max(fwidth, 0.6)` px. Output is premultiplied, blended `ONE, INV_SRC_ALPHA`.
- **Size and scale.** `Record(…, width, height, time, bottom_inset, dpi_scale)` scales layout space uniformly into the window.
  - **Font size:** `FontPixels = min(max(height / 24, 18 × dpi), 0.92 × width / 34)`. That is 30 px at 720p and 90 px at 4K.
  - The width cap keeps a full 34 em line within 92 % of a narrow window, so portrait windows keep every line inside the frame. The price is smaller text there: about 1.5 % of the height in 9:16, against 4.2 % in landscape.
  - The text block is centred horizontally.
  - The outline (1.4 px at a 30 px font, at least 1 px) and the drop-shadow offset (1.6 px at 30 px) scale with the font.
  - Distances are true Euclidean distances, so glyph edges stay crisp when magnified (90 px at 4K is 1.9× the atlas em). Corners round by about a texel at that magnification.
- **Stacking.** Overlapping cues stack upwards: the latest-starting cue sits at the bottom, with 0.4 font sizes between cues. A cue that would rise into the top 3 % of the window is skipped, together with any older cue above it, rather than drawn off-screen.
- **Device loss.** `Recreate` builds the GPU objects on the new device. Shaped cues and glyph metrics are kept; resident glyphs are paged in again as needed ([recovery](recovery.md)).

## Presentation integration

`D3D12Renderer::Composite` draws in this order: letterboxed video (or the retained history image) → subtitles → glass dock → `Present`.

- **Selection time.** Cues are selected at the PTS of the video frame being composited. A controls-only refresh has no new video frame. It selects at the renderer's media position instead: the PTS of the last presented frame, or while playing, the current epoch's clock estimate.
- **Position.** `D3D12Renderer::CaptionInset` places the lowest caption line.
  - At rest it sits 1.3 font sizes above the bottom edge (40 px at 720p).
  - While the dock is visible it rides 20 scaled px above the dock's top edge, gliding with the dock's reveal and fade.
  - This holds at every aspect ratio and DPI; `media_layout_gpu_tests` measures the gap.
- **Change detection.** A change in the active cue set triggers a controls-only refresh, limited to 120 Hz. While playing, a cue appears or disappears on the clock (within the 8.3 ms refresh throttle), not only on the next video frame; when a picture is due within one display refresh, the change rides on that picture instead. `ShownSubtitles()` reports the cue set composited into the last presented image, and the time it was selected at.

### Seek, scrub, pause and underrun

- **Seek/scrub.** The seek's first target frame is presented as soon as it is in order (see [seeking](seeking.md)). Its composite selects cues at the seek target when the target lies inside that frame's interval, and at the frame's PTS otherwise (see [seeking](seeking.md#protocol)). The landing image therefore carries the target's captions, with no stale cue and no one-frame lag, whenever their glyphs are resident (always for a track that fits the atlas; otherwise the caption follows a refresh or two later, see [Limits](#limits-and-open-items)). `SeekPresentation` records the cue set of every landing image and the time it was selected at, and the playback tests compare it with `SubtitleTrack::Select` at that time.
- **Epoch guard.** Between a seek request and its landing, clock estimates from an older epoch are ignored, so captions and controls stay on the held image.
- **Pause.** The media position is frozen while paused, and a paused seek still lands with its captions.
- **Underrun and re-anchoring.** Video composites select cues by frame PTS, so re-anchoring the PLL after an underrun or resume cannot shift captions relative to the picture.

## Memory

| Resource | Size | Lifetime |
|---|---|---|
| Arena: source copy + cue table | file size + 40 B × cues | session |
| Arena: shaped glyphs 4 MiB, staging outlines 4 MiB, distinct-glyph keys, hash table and residency about 1.7 MiB, cue layouts 16 B × cues | ≈ 10 MiB | session |
| Arena: quote search (folded text and a 32-byte signature per cue) and concept search (4 B per cue, 2 B per concept found) | text size + 36 B × cues + concepts | session |
| GPU atlas (R32F, 3072 px wide) | 1.125 MiB per 32 cells; 72 MiB at the full 2048 cells | session |
| GPU upload ring (instances, edges, cell ranges) | 5 MiB | session |

The playback arena is 256 MiB. Steady-state playback allocates nothing.

## Searching the track

The track loaded for display is also what the viewer searches: `find …`, `מצא …`, or any text typed into the [command bar](command-palette.md) lands on the cue's start, with the cue on the landing picture ([subtitle search](subtitle-search.md)). A quote that is in no cue falls back to the cues that mean it, so "danger" finds "זהירות!" and "סכנה" finds "watch out" ([concept search](subtitle-search.md#concept-search-2026-10-20)); a `Concept match: "…"` cue then shows the line found. Both indexes are built in `Preparing` from the arena and read the stack only while playing. Only the channels shown are searched: a quote is looked for in the language on screen.

## Verification

| Test | Checks |
|---|---|
| `media_subtitle_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | SRT/WebVTT parsing including Arabic/Hebrew/CJK UTF-8; invalid timing/UTF-8 rejected by the reader; sort order; overlap selection; five overlapping cues accepted with the latest four selected; malformed blocks skipped between valid cues, with `skipped` and the error line reported; garbage and all-invalid input rejected without using the arena; allocation-free `Prepare` and `Select` |
| `media_subtitle_gpu_tests` | **Bidi, from shaped placements:** RTL punctuation per line; four distinct Arabic joining forms; LTR digits inside Hebrew; exactly 4 RTL lines.<br>**Pixels:** half-open boundaries at ±1 tick; back-to-back switching; markup-only cues; upward stacking.<br>**Scrub:** 360 random-time draws bit-identical per cue set, 0 allocations.<br>**Paging:** 80 cues, each with 字幕 plus 55 unique ideographs (4403 distinct glyphs against 2048 cells):<br>• `Prepare` under 20 ms (measured 0.4 ms), all cues shaped 51 ms later in the background<br>• all 80 cues drawn whole, 4794–8032 bright pixels each (a dot leaves a handful per glyph)<br>• 4466 cells generated, 2418 evictions, 0 dropped, status `Ready`<br>• the first cue drawn again after its glyphs were evicted: 0 pixels differ<br>• 0 allocations |
| `media_layout_gpu_tests` | Captions and dock at 11 sizes, from 640×360 to 3840×2160 and 2160×3840, including 21:9 and 9:16, at 100–200 % DPI. Caption width and height follow `FontPixels` against the 4K reference (±2 px + 3 %). Captions stay centred inside the frame and at least 20 scaled px above the dock. 0 allocations |
| `media_hud_gpu_tests` | Captions composited with the dock: Arabic, Hebrew, Devanagari, Thai, CJK and Hangul shape with 0 missing glyphs; exact end-time removal; bit-identical restore after a backward jump |
| `media_playback_subtitle_test`, `media_playback_subtitle_scrub_test` | Seek and scrub landings carry exactly `Select(landed PTS)`; 0 drops, 0 allocations |
| `media_playback_subtitle_overflow_test` | Real playback with a built-in track of 60 cues of 50 ms, 70 unique ideographs each (4203 distinct glyphs): all 60 cues shown whole, 2154 evictions, 0 dropped glyphs, status `Ready`; all 72 frames and all audio, 0 frame drops, 0 allocations; pipeline preparation under 20 ms (measured 0.3 ms Release, 0.6 ms Debug) |
| `media_playback_subtitle_missing_test` | `--subtitles` names a file that does not exist: all 72 frames and all audio, 0 drops, status `Disabled` (`0x80070002`), no caption composites |
| `media_playback_resize_test` | Captions composited throughout five window resizes, including fullscreen (see [resizing](resizing.md)) |
| `media_playback_device_loss_test` | After a real device removal the atlas is rebuilt and the landing frame's captions match the track ([recovery](recovery.md)) |

Allocation probes cover the application's C++ allocation entry points and, in MSVC Debug, the CRT. They do not cover driver or OS heaps.

```powershell
ctest --test-dir build -C Release -R "subtitle|layout" --output-on-failure
.\build\Release\media_player_demo.exe movie.mp4 --subtitles movie.he.srt
```

## Limits and open items

- **Startup.** Before this change, preparing a full atlas of CJK glyphs took 0.92 s before the first frame (27 ms layout, 265 ms outlines, 567 ms atlas build). `Prepare` now takes 0.3–0.6 ms; the first frame is presented about 10 ms after `Run`, with or without subtitles.
- **A cue can appear slightly after its picture.** A cue is drawn once all its glyphs are resident. Within the 10 s look-ahead that is always before it starts. After a seek, the glyphs of the text at the seek's target are requested while the seek is landing (since 2026-09-30), so the landing image has its caption when landing takes longer than the outlines (about 0.13 ms per glyph, requested every 8.3 ms). `media_fixture_subtitle_search_test` measures it on a real file: four seeks minutes apart into text that was not resident, each landing image composited with its cue. A landing faster than the outlines (a seek onto a sync sample of a small picture, into a cue with many new glyphs) can still show the picture a refresh before its caption; that case is not covered by a test. A cue with more than 64 glyphs not yet in the atlas also takes more than one composite.
- **Generation cost.** A cell costs about 0.28 ms of GPU time on the UHD 630 (every texel tests every edge). The 64-cell budget therefore allows up to about 18 ms in one composite when a dense cue arrives unannounced. Culling edges per tile would cut it; not done.
- **The shaping thread runs during playback.** It does not use the process heap, but DirectWrite allocates from the system's. The allocation probes do not see that.
- **Portrait windows.** Lines are wrapped once, at 34 em, and never re-wrapped for a new window shape. Portrait windows get smaller text instead.
- **Styling and placement.** WebVTT positioning (`line`, `position`, `align`, `size`, regions), `::cue` CSS and ruby are not supported. `<i>`/`<b>`/`<u>` are stripped rather than styled. A line with vertical (sideways) runs is left out.
- **Formats.** No SSA/ASS. There are no subtitle tracks embedded in the MP4: the demuxer rejects any track other than one video and one AAC track.
- **Fonts.** The font is fixed (Segoe UI plus system fallback). Missing glyphs render as `.notdef` and are counted in `Statistics::missing`.
