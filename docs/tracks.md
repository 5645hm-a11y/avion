# Several tracks: catalogue, choice, switching

A film often carries more than one picture, sound or text track: dubbed audio, a commentary, subtitles in several languages, a timecode track. This document covers how the engine lists them, which it plays, and how the audio and subtitle tracks are changed while playing.

Code: `demux::IsobmffParser` (catalogue, emission, `SwitchTrack`, `SampleAt`), `engine::PlaybackCoordinator` (choice, splice, subtitle import, commands), `subtitle::SubtitleTrack` (channels), `ui::StatusCue` (the cue that names each change, [kinetic HUD](kinetic-hud.md)).

## The catalogue

Every `trak` of the file is read, up to 16 (`ParserLimits::tracks`). For each the parser records (`demux::TrackInfo`):

| Field | From |
|---|---|
| `kind` (`Video`, `Audio`, `Subtitle`, `Other`) and `handler` | `hdlr` handler type (`vide`, `soun`, `text`/`sbtl`/`subt`, anything else) |
| `language` | `mdhd`: ISO 639-2 packed letters, or a QuickTime Macintosh language code mapped to its ISO code; `und` when none |
| `name` | `hdlr` name, UTF-8 cut at a character boundary |
| `alternate_group`, `enabled` | `tkhd`: tracks of one group are alternatives; `enabled` is the file's default among them |
| `sample_entry`, `codec` | first `stsd` entry (`avc1`, `hvc1`, `mp4a`, `tx3g`, `ac-3`, ...) |
| `refusal` | why the track cannot be used, or `None` |

**One bad track never stops a file.** A problem confined to one track (an unknown codec or handler, an edit list that changes its timeline, a broken sample table, a timed-text entry the engine does not read) *refuses* that track: its samples are never published, the other tracks play as if it were not there, and the reason stays in `refusal` with its offset. Parsing fails only when no track can be used, with the first refusal's error. A refused track's boxes are skipped except those the catalogue reads (`tkhd`, `mdhd`, `hdlr`, `stsd`), so a corrupt table in a commentary track costs nothing. A classic file with more than 16 tracks has the rest skipped (`SkippedTracks()`); a fragmented one fails with `LimitExceeded`, because its fragments would interleave samples of tracks it never declared.

The engine adds what it decides (`engine::MediaTrack`, `Tracks()`, `TrackAt(i)`, `FindTrack(id, kind)`):

| `TrackUse` | Meaning |
|---|---|
| `Selectable` | Can be chosen now |
| `Refused` | The parser refused it (above) |
| `Undecodable` | The codec is known but not playable here: AAC 7.1 or another layout the platform decoder refuses ([platform AAC limits](surround.md)), AV1 video |
| `Incompatible` | Audio whose decoder cannot be prepared, or whose format the converter does not take (rates more than 4:1 apart, a channel count other than 1, 2, 6 or 8); or a second decodable picture track. It can only be chosen at start |

The language is canonical ISO 639-2/T (`heb`, `eng`, `fra`; `ai::CanonicalLanguage` also maps `he`, `iw`, `en`, `fre` and the bibliographic codes). A track's name is kept only when a person wrote it: muxers put their own label in `hdlr` ("SoundHandler", "Core Media Audio", "GPAC ISO Video Handler"), which is dropped.

## Which tracks play at start

- **Video:** the first decodable picture track (H.264 or HEVC), the file's default (`enabled`) first. Other picture tracks are not published.
- **Audio:** among the AAC tracks whose layout decodes here, the language asked for (`PlaybackOptions::audio_language`), else the file's default, else the first. Every candidate gets its own decoder and its own converter, prepared before playback (`Win32AacDecoder` and `audio::PcmConverter` per slot, at most 8), so a change never creates one. The endpoint, the DSP and the clock are opened for the chosen track; a candidate at another rate or layout is converted to that format when it is chosen (below). Without any decodable audio the file still plays, clocked by the endpoint's silence, and a status cue says so (`Audio · 8-channel AAC cannot play here · picture only`, `UnsupportedLayout`).
- **Subtitles:** shown at start when a sidecar file is given, or the file marks a subtitle track as its default, or a language asked for (`subtitle_language`) has one. The sidecar's language is `sidecar_language`, else a suffix of its name (`film.he.srt`).

## Subtitle tracks

All subtitle text lives in one `SubtitleTrack`, one *channel* per source: the sidecar is channel 0, each `tx3g` track of the file the next. Embedded `tx3g` samples are read before playback (`SampleAt`, table lookups, then reads of each sample's few bytes; local files only) into the arena; their styling boxes are ignored, the text is kept. `SubtitleImportTime()` reports what it cost.

Choosing a subtitle track sets the track's shown mask (one atomic store): the next composite draws only the cues of the shown channel, search (`SeekToSubtitle`, quote commands) looks only in it, and the glyph prefetch skips the others. Nothing is re-read or re-shaped. `kNoTrack` hides subtitles; "subtitles on" brings back the last track shown.

## Changing the audio track while playing

`SelectAudioTrack(id)` (UI thread) queues one control record; the media worker applies it between packets. The new track must be `Selectable`: any rate and layout the converter takes (below).

**The splice.** The new track continues the old one sample-exactly in the PCM stream, without re-anchoring the clock or touching video and subtitles:

1. The old track fades out over 5 ms of its own decoded PCM. If none is decoded yet, or the endpoint has no room, the change waits for both, 50 ms at most.
2. Its packet in hand goes back to the parser unread; what was already committed to the endpoint plays out.
3. The splice point is the media time of the next PCM frame to be written: `W = origin + written / rate`. The parser stops publishing the old track and publishes the new one from its last sample presenting at or before `W`, with the AAC pre-roll (`IsobmffParser::SwitchTrack`; no I/O, no allocation, other tracks continue where they are).
4. The new track's decoder is flushed; its output is trimmed to start exactly at `W` and fades in over 2.5 ms.

So the old language's last syllable and the new one's first are adjacent PCM frames on one timeline: there is no gap, no repeat, and the picture never notices. What the listener still hears of the old track is what was already queued at the endpoint (`TrackSwitch::queued`: 0.26–0.39 s measured).

**Cold changes.** When nothing of the current epoch was written yet (at start, or while a seek lands) there is nothing to splice into: the new track simply starts at the epoch's target, as a seek would position it. A change during a seek follows the seek.

### Another rate or layout: `audio::PcmConverter`

A dub at 44.1 kHz in a 48 kHz film, a stereo commentary beside a 5.1 mix: the new track is converted to the format the output path was opened with, on its way to the endpoint. The endpoint is never reopened, the DSP and the clock keep their configuration, and nothing is allocated (each candidate's converter, and its filter table, are made in Preparing).

- **Channels**, remixed as the PCM arrives: equal counts are copied; mono goes to both sides, or to the centre of 5.1; stereo to mono is the mean; stereo to 5.1 is the front pair; 5.1 (or 7.1) to stereo is the engine's own fold-down (`DspProcessor::DownmixWeights`: ITU-R BS.775 shape at scale 0.5, LFE left out), so a 5.1 track sounds the same whether it started the film on a stereo path or was switched to. Other combinations are refused (`Incompatible`).
- **Rate**: band-limited interpolation with an exact rational step. Output frame *n* lies at input position *p0 + n · in/out*, kept as an integer and a remainder in units of 1/(out/gcd): no drift, ever, and no interpolation of the kernel either. Each of those positions (160 for 44.1 → 48 kHz, at most 1,024) has its own row of a Kaiser-windowed sinc (β 8, 48 zero crossings at the lower rate's Nyquist frequency, cut at 0.945 of it so the stop band begins at Nyquist), each row normalised to unity gain at DC. The filter is centred on each output frame's position: there is no delay. An output frame needs 48 input frames past its position (53 for 48 → 44.1 kHz), which the converter holds back until more arrives; at the end of the track it completes them with silence and plays them.
- **The splice** works as for same-format tracks, with one refinement: the new track is trimmed to the frame at or before `W`, and its converter starts the fraction of a frame between that frame and `W` later, so its first output frame lies exactly at `W` (to 1/160 of a 44.1 kHz frame).
- A seek, or switching back to a track of the output path's format, takes the converter out of the path (`TrackSwitch::converted`, `rate`, `channels` record which).

Measured (`media_pcm_converter_tests`, MSVC Release): a tone keeps its level within 0.01 dB and its phase within 6·10⁻⁸ rad (16 ns at 1 kHz: no delay); residual after fitting the tone put in −93.5 dB (7 kHz at 16 → 48 kHz) to −133.7 dB (96 → 48 kHz), −105.6 dB for 1 kHz at 44.1 → 48 kHz; a 23 kHz tone at 48 → 44.1 kHz is left at −85.5 dB (no fold-back), the image of an 18 kHz tone at 44.1 → 48 kHz at −94 dB. A minute of positions gives exactly ⌈N · 48000 / 44100⌉ frames. About 96 multiply-adds per output sample (44.1 → 48 kHz).

Each change is recorded (`TrackSwitches()`, `SwitchAt(i)`: from, to, requested and applied times, splice media time and PCM frame, whether it was spliced, the new track's rate and channels, and whether it is converted). A switch on a stream that cannot seek by table (fragmented, remote before its index) is a cold change of emission only.

## Commands and cues

The text and voice grammar ([intents](intents.md)) reads track commands in Hebrew, English and the other table languages: `תעביר לאודיו באנגלית`, `switch subtitles to hebrew`, `english subtitles please`, `כתוביות בעברית`, `audio in french`, `turn off the subtitles`, `next audio track`. They become `SelectAudio`, `SelectSubtitles` or `SelectLanguage` (a language with no track named: its audio, else its subtitles).

| Situation | Result | Cue |
|---|---|---|
| The track exists and can be chosen | `Ok` | `Audio · Hebrew (עברית) · AAC stereo`, `Subtitles · English` |
| A track in that language exists but cannot play here | `Unavailable` | `French audio cannot play here` |
| No track in that language | `NotFound` | `No German subtitles` |
| Subtitles off | `Ok` | `Subtitles off` |
| `next audio track` with only one | `NotFound` | `No other audio track` |

A cue is also shown when the audio endpoint changes (`Audio · now on the default output device`), when the audio cannot play at all, and for every voice command heard. Cues are the glass status pill of the [kinetic HUD](kinetic-hud.md).

## Verification

| Test | Checks |
|---|---|
| `media_multitrack_demux_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **Cataloguing:** a 9-track file (H.264; AAC English, the default, and Hebrew in one group; `tx3g` Hebrew and English; timecode; AC-3 Spanish; an AAC commentary with a two-segment edit list; WebVTT in MP4) gives every track its kind, codec, language, name, group, default and sample entry; the last four are refused as `Unsupported` and never published, the rest play; subtitle samples are found by lookup (`SampleAt`); zero allocations. **Switching:** English published alone; `SwitchTrack` at 0.3 s continues with Hebrew from 0.2773 s (its sample before 0.3 s plus one unit of pre-roll), the video continuing untouched with no picture repeated; switching back after the end, and a seek positioning unpublished tracks too. **Refusals:** a broken table or two sample descriptions refuse that track only; a file of unusable tracks fails with the first reason; 19 tracks catalogue 16 and skip 3. **Corruption sweep:** every other byte of the file inverted in turn: each run ends admitted or refused (2,899 and 368), never reading outside the file or allocating. |
| `media_subtitle_tests`, `media_subtitle_search_tests` | Channels: several sources in one track, cue selection and search limited to the shown channel, the mask changed mid-playback |
| `media_pcm_converter_tests` (new; MSVC, GCC `-O2`, ASan/UBSan) | Supported formats (every AAC rate pair within 4:1), identity passthrough bit for bit, remix weights (5.1 → stereo equal to the fold-down's), tone level, phase and residual at eight rate pairs, alias and image rejection, output counts exact over a minute, any split of pushes and pulls bit-identical, a sealed arena refused, zero allocations |
| `media_playback_tracks_test` | Real playback of the multi-track fixture (`media_make_multitrack_fixture`: a 440 Hz English tone, a 660 Hz Hebrew tone, an 880 Hz 44.1 kHz commentary, a 550 Hz Arabic 5.1 track whose sample entry says 2 channels, each with a 10 ms notch every 250 ms of media time) with fourteen spoken-style commands: `תעביר לאודיו בעברית`, `english subtitles please`, `switch subtitles to hebrew`, `can you please switch the audio to english`, `audio in french` (converted 44.1 → 48 kHz), `spanish audio` (Unavailable: AC-3), `turn off the subtitles`, `next audio track` (Arabic 5.1, folded to stereo), `go back 3 seconds`, `english audio` 30 ms after the seek, `כתוביות בעברית`, `German subtitles` (NotFound), `אשמח שתעצור עכשיו`, `תמשיך בבקשה`. Checks the catalogue (the Arabic track listed with its ASC's 6 channels), each command's kind, status and resulting tracks, five applied changes and which were converted, the committed PCM after each splice at the new track's frequency, every notch away from a splice within 1 ms of its 250 ms mark, no silent gap at any splice, the captions on screen in the chosen language, at least eight status cues composited, no dropped picture and zero allocations. Measured (Release, 2026-10-14): changes applied 2–50 ms after the request, heard 374–382 ms later (what the endpoint had queued); 62 time marks, worst **0.083 ms** off the grid (0.000 ms before converted tracks: the filter softens a notch's edges by a few samples); the quietest millisecond at each splice 0.15–0.40 of the tone, no silence; 11 cues. A mutation shifting the splice by 96 frames (2 ms) fails it. |
| `media_playback_tracks_surround_test` (new) | The same fixture started on the Arabic 5.1 track (folded binaurally to the stereo endpoint), then `english audio` (stereo → 5.1 front pair), `audio in french` (and 44.1 → 48 kHz), `arabic audio` (no conversion), `hebrew audio`: four spliced changes, the right three converted, each new track's tone after its splice, 47 time marks worst 0.062 ms off the grid, no silence, no dropped picture, zero allocations. |
| `media_hud_status_tests` | The cue's motion and its pixels ([kinetic HUD](kinetic-hud.md)) |

## Open

- **A change is heard about 0.38 s after it is asked for**: the old track's PCM already queued at the shared-mode endpoint plays out first (`TrackSwitch::queued`). Cutting that would mean discarding queued audio, which the sink does not do; the splice itself is sample-exact.
- Only AAC audio tracks can be selected; AC-3, E-AC-3, Opus-in-MP4 and others are catalogued and refused.
- A converted track keeps the output path the starting track opened. When that path itself runs at a rate the endpoint's mix does not (44.1 kHz on a 48 kHz endpoint), a 48 kHz track switched to is resampled twice (by the converter to 44.1 kHz, then by Windows to 48 kHz). Opening the endpoint at its mix rate from the start would avoid that; it would change every file's playback, and has not been done.
- Stereo put on a 5.1 path plays from the front pair only (no upmix into the centre or surrounds); on a binaural path that is the two front virtual speakers.
- Embedded subtitles of a remote file are not imported (reading every sample ahead of playback would download them first); a remote file's sidecar works.
- `tx3g` styling (colour, bold, karaoke, positions) is ignored; WebVTT-in-MP4 (`wvtt`) and bitmap subtitles (PGS, VobSub) are refused.
- Only one picture track plays; angles and alternate video tracks are listed, not switchable.
- Track commands in Chinese and Japanese are not in the grammar yet.
