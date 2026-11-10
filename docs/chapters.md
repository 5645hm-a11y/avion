# Chapters and scenes

Status: implemented (2026-10-16). A file's chapters, or sections synthesized from its structure when it has none, and its scenes, shown as ticks on the dock's timeline and reached by command (nine languages) or by Page Down / Page Up.

- The model is portable and allocation-free: [`timeline::ChapterTimeline`](../include/media/timeline/ChapterTimeline.hpp) (256 chapters, 1024 scenes, fixed storage), `timeline::Synthesize` and `timeline::CutDetector`.
- The demuxer reads the chapters ([`IsobmffParser`](../include/media/demux/IsobmffParser.hpp)); the coordinator builds the timeline at Prepare and adds cuts seen while playing; the renderer measures each picture and draws the ticks.
- After Prepare the timeline belongs to the UI thread alone: no synchronisation, no allocation.

## Where chapters come from

In this order; the first source that has any wins.

1. **Nero chapters** (`moov/udta/chpl`): a version, flags (4 more bytes in version 1), a one-byte count, then per chapter a 64-bit start in 100 ns units on the presentation timeline, a one-byte title length and the title. Up to 255 are kept (the count is one byte). Titles are UTF-8 in practice; one that is not becomes an untitled chapter, not a refused one.
2. **A QuickTime chapter track**: the video track's `tref/chap` names a text track. Its samples are the chapters: each sample's time is a chapter's start and its text (a 16-bit length, then UTF-8, or UTF-16 with a byte order mark) is the title. QuickTime `text` sample entries are read like `tx3g` ones (the samples have the same form). The track is catalogued as `TrackUse::Chapters`: it is never imported or offered as subtitles. Read at Prepare with positioned reads of the samples' bytes on a second handle, like embedded subtitles; local files only.
3. **Synthesized sections**, below. The dock and the cues call them sections ("Section 3 · 20:00").

**Never a reason to refuse a file.** `udta` and `tref` are read as bounded leaf boxes (1 MiB at most, and within the source's largest range; larger `udta` boxes, which hold cover art, are not read). Inside them the walk stops quietly at anything malformed: a size past the end, a short or zero header (QuickTime ends `udta` lists with a 4-byte terminator), a count larger than the entries present, a title past its box. `media_chapter_scene_tests` builds each of these.

## Synthesis

`timeline::Synthesize` runs at Prepare on the video track's sync samples (their presentation times and sizes, from the compact index: at most 65,536, evenly strided beyond that) and on the cues of one subtitle source (the one shown at start, else the first). It is a pure function of that input; no clock, no allocation (its scratch is on the stack).

- **Cadence.** The median interval between sync samples, from a 10 ms histogram up to 20 s.
- **Structural scenes.** A sync sample that arrives before three quarters of the cadence: encoders insert one at a scene cut instead of waiting for the next regular one. And a sync sample at least 1.8 times as large as the previous one, or smaller than 0.55 of it: a change of content (the "bitrate shift"; sync-sample sizes stand for the bitrate, since reading every sample's size of a long file at Prepare would cost too much).
- **Dialogue scenes.** Dialogue that resumes after more than 4 s of silence (`kDialogueGap`), also at the start: at the sync sample at or before the resuming cue when one lies in the gap, else at the cue itself.
- **Sections**, only when the file has no chapters: one at the start, then candidates by strength (a dialogue scene that is also a cut: 3; a dialogue scene: 2; a cut: 1), earlier first among equals, then plain sync samples (strength 0) to fill what is left. Each is at least `spacing` = clamp(duration / 12, 1 min, 10 min) from every section kept and no closer than `spacing` / 2 to the end. A file shorter than about one and a half spacings gets only the start.
- Scenes closer than 1 s (`kSceneMerge`) are one; the earlier mark stays.

Measured in `media_chapter_scene_tests` on a synthetic two-hour file (a sync sample every 2 s, three encoder cuts, one content change, dialogue every 3 s with two long silences): cadence 2.005 s, the 4 structural and 2 dialogue scenes exactly where they were put, 10 sections 600 s apart or more, the same result on a second run, and a file with chapters keeps them (scenes only).

## Cuts seen while playing

Every converted picture, SDR or HDR, is measured on the GPU and compared with its neighbour in presentation order.

- **Measure.** A compute pass after each conversion (`HdrToneMap.hlsl`, entry `Stats`, compiled as `SceneStatsShader.hpp`) reads one luma sample per 8×8 block of the crop and adds their sum and count to running totals in the colour pipeline's state buffer (one atomic pair per 64 samples). The state from offset 8 to 40 (pictures, the tone mapper's cut count, epoch starts, the luma totals) is copied to a per-slot readback buffer (`ColorPipeline::RecordStats`), read on the CPU when the slot's fence completes. The CPU differences the totals (they wrap; one picture's share never does).
- **Order.** Conversions run in decode order. The renderer harvests completed slots in conversion order (fence order, not slot order), and `CutDetector` puts pictures back in presentation order through a window of 4 before comparing neighbours.
- **Cut.** A change of more than 0.12 in mean luma (0 to 1), or a cut the HDR tone mapper detected on its own (its peak moving by more than 0.12 PQ: [picture](picture.md#hdr-peak-per-epoch-2026-10-16)).
- **Epochs.** The detector restarts with every epoch that is not a queue seek, like the HDR peak: the first picture after a seek has no neighbour. A cut exactly at a seek's landing is therefore not seen; it is seen when playback runs across it.
- **Timeline.** The coordinator drains the renderer's ring of cuts (`D3D12Renderer::NextCut`) on every streaming pass into the timeline's scenes (`SceneOrigin::Live`), where a live cut takes the place of a structural guess within 1 s, and the dock's ticks are redrawn.

The chapter fixtures have hard cuts at 3 s and 6 s; `media_playback_chapters_test` sees both, at 3.000 s and 6.000 s exactly, once playback has run across them.

## Navigation

`PlaybackCoordinator::Navigate(chapter, direction)`, from the position the next command would see (`Bounds()`: a seek still on its way counts as arrived):

- **Next**: the first mark more than 0.25 s ahead (`kLanding`), so a landing picture that starts a little before its mark does not find the same mark again.
- **Previous**: the start of the current chapter or scene when more than 3 s into it (`kRestart`), else the one before; from inside the first one, its start.
- The move is a plain seek (exact landing, audio on the target sample) with a status cue: `Chapter 3 · Finale · 02:15` (the title when there is one), `Section 2 · 10:00`, `Scene 7 · 1:02:03`; times as the dock shows them (MM:SS, H:MM:SS from an hour). Nothing that way: a warning cue, `No next chapter`, `No previous scene`, and `NotFound`. The text is assembled in place (no formatting library: `snprintf` may allocate).
- **Keys.** Page Down: next chapter; Page Up: previous chapter.
- **Commands** (docs/intents.md, "Chapters and scenes"): `NextChapter`, `PreviousChapter`, `NextScene`, `PreviousScene`. The grammar refuses them (`Unavailable`) when the media is not seekable or has no chapters (scenes); the executor finds the mark.

## On the dock

Up to 32 marks in 16 root constants (`ui::TimelineMarks`: 16 bits each, bit 15 a chapter, 15 bits of position; 0 ends the list), a second constant block (b1) of the HUD pass beside the 128-byte uniforms: 48 root constants in all.

- A chapter is a thin capsule across the track (0.65 px radius, ±(thickness + 3.5) px), a scene a 1.15 px dot above it. Both brighten with the track on hover or drag. Analytical, like the rest of the dock: no texture.
- The coordinator packs them (`PackMarks`): every chapter after the start, then scenes evenly picked when more than fit, none within 1 s of a chapter tick. Repacked when a live cut adds or moves a scene.

## Verification

| Test | Checks |
|---|---|
| `media_chapter_scene_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **Parser:** Nero chapters (a Hebrew title, a title cut to fit, the QuickTime terminator); six malformed or oversized `udta` boxes and a short `tref`, all tolerated; a QuickTime chapter track (`tref/chap`, a `text` entry read as `tx3g`); `SyncSample`, `DecodeDistance` (every picture of an I P B B pattern, bounded scans, before the first picture, past the end), `SyncCount`/`SyncAt`. **Timeline:** order, duplicates, titles (cut, control characters, not UTF-8), next/previous with the landing slack and the restart window, scene merging and live correction. **Synthesis** as above. **Cuts:** decode order to presentation order, a reported cut, epochs. **Marks:** packing and limits. **Dock:** a drag emits `Scrub` on each move that changes the target and `Seek` on release; the preview cue glides and snaps. **Grammar:** 25 phrases in nine languages, validation, the command bar, table consistency. Zero allocations. |
| `media_playback_chapters_test`, `media_playback_chapters_qt_test` | The chapter fixtures (Nero, QuickTime track): seven commands in English and Hebrew and Page Up, each with its cue (`Chapter 2 · פרק שני · 00:03`, ..., `No next scene`, `Scene 2 · 00:06`) and an exact landing; both live cuts within one picture of 3 s and 6 s; ticks at 1/3 and 2/3; the chapter track not offered as subtitles; 0 drops, 0 allocations. |
| `media_hud_gpu_tests` | A chapter tick and a scene dot drawn where they belong and nothing else; no marks: the dock as before. |

## Limits

- Chapters are read from classic MP4 only: a fragmented file's `udta` is read, but a chapter track's samples need sample lookups (`Seekable()`), which fragmented files do not have. Remote sources get Nero chapters but not chapter tracks (no positioned reads), and no structural scenes unless the scan has already finished.
- Synthesis looks at sync samples and dialogue only. A film encoded with a fixed GOP and no subtitles has no structural signal; its sections are then evenly spaced sync samples. Picture content is measured only while playing.
- Live cuts compare mean luma: a cut between two shots of similar brightness is missed, and a fast flash may be counted. Fades are not cuts. A cut at a seek's landing is not seen until playback crosses it again.
- Matroska chapters, WebVTT chapter tracks and MP4 `meta`-box chapters are not read. Chapter titles have no language selection (the first track's titles are used).
- Spoken chapter and scene commands exist since 2026-10-16 (English and Hebrew, [voice](voice.md#chapters-and-scenes-2026-10-16)); without the wake word the Hebrew ones are mostly refused (too short for the Hebrew sentence floor).
