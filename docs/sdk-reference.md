# AVION SDK reference (API 16)

The reference for hosts that embed AVION: what to call, what comes back, what fails and why. `docs/sdk.md` is the design log
(why each part is the way it is); this page is the contract. Everything here is declared in `include/media/c_api.h`, exported
by `avion.dll`, and covered by `media_embed_tests`, `media_embed_dll_tests`, `media_c_api_tests` and `media_probe_tests`.

## Before opening: `media_probe`

```c
media_probe_t info = { .struct_size = sizeof info };
if (media_probe(path_utf8, &info) == MEDIA_OK && !info.playable) {
    /* hand the file to another player; info.reason says why in English */
}
```

- No engine, window, GPU or audio device. Reads the container's metadata only (7 files including a 45-minute one: 0.10 s in all).
- `playable`: `media_engine_open` (windowed) is expected to succeed. The GPU's decoder can still refuse a size or level.
- Per track: kind, fourcc, a description (`"H.264 High, 4:2:0 8-bit, progressive"`, `"AAC, 2 ch, 48000 Hz"`), `supported`,
  and a `reason` when not.
- Non-MP4 containers are named from their first bytes, so a host can route them: Matroska/WebM, AVI, MPEG-TS/M2TS, MPEG-PS,
  FLV, ASF, MP3, FLAC, Ogg, WAV.
- `MEDIA_ERROR_OPEN`: the file could not be read at all (missing, unreadable).

Reasons you will see:

| Situation | `reason` |
|---|---|
| AV1 | `AV1 is not decoded yet (H.264 and HEVC only)` |
| H.264 10-bit, 4:2:2, 4:4:4 | `H.264 4:2:2 10-bit is not decoded (H.264 4:2:0 8-bit only)` |
| Interlaced H.264 | `interlaced H.264 (fields or MBAFF) is not decoded` |
| HEVC 4:2:2/4:4:4 or 12-bit | `HEVC 4:4:4 10-bit is not decoded (4:2:0, 8- and 10-bit only)` |
| Plays, sound not decodable | `plays without sound: codec 'ac-3' is not supported ('ac-3' track)` |
| Another container | `Matroska/WebM container: not supported yet (MP4 and MOV only)` |
| A table the reader refuses | `refused by the MP4 reader: <what> in '<box>' at byte <offset>` |

## After a failure: `media_engine_last_error_text`

```c
char why[256];
if (media_engine_open(e, path) != MEDIA_OK) {
    media_engine_last_error_text(e, why, sizeof why);
    /* "open failed at stage 4 (no usable video track (or, without video, no AAC sound)): AV1 is not decoded yet (H.264 and HEVC only)" */
}
```

After a failed open of a local file, the text includes `media_probe`'s reason for that file. URLs are not probed, because a
diagnosis must not send another request. `media_engine_last_error` still returns the stage and code. The stages differ by mode:

| Stage | Windowed engine (`MEDIA_PLAYBACK_WINDOW`) | Host mode (`MEDIA_PLAYBACK_HOST`) |
|---|---|---|
| 1 | the file or URL could not be opened | invalid configuration |
| 2 | memory, the container reader or the decoder could not start | memory for the engine's arenas |
| 3 | the container could not be read | the file could not be opened |
| 4 | no usable video track (or, without video, no AAC sound) | the container reader could not start |
| 5 | the video stream's configuration is not decodable | the container could not be read |
| 6 | the renderer could not start (GPU, window) | no track this engine can play |
| 7 | the GPU decoder refused the stream | the video decoder could not be prepared |
| 8 | a worker thread could not start | the audio path could not be prepared |
| 9 | the audio decoder or output could not start | the subtitle file could not be read |
| 10 | | the voice module could not be prepared |
| 11 | a packet could not be returned to the reader | the container failed during playback |
| 12, 13 | | the decode chain failed |
| 14 | audio output failed | audio conversion failed |
| 15, 16 | audio output failed | |
| 17 | the container failed during playback | |
| 18 | a read failed during playback | |
| 19 | AAC decoding failed | |
| 20 | the video bitstream is not decodable | |
| 21 | the picture size changed mid-stream | |
| 22-24 | the decode chain failed | |
| 29 | timestamps out of order or out of range | |
| 32 | the stream reorders more pictures than the renderer holds | |

## Controls: `media_engine_post_control`

For buttons and sliders. No text is built or parsed. Each control runs exactly what the matching grammar command runs, on
the same lock-free ring, and is answered by the same `MEDIA_EVENT_COMMAND` (with your `command_id`).

| Control | Value | Notes |
|---|---|---|
| `MEDIA_CONTROL_SEEK` | seconds, >= 0 | past the end lands on the last picture; frame-exact (measured: 2.000 s, 2.2 ms paused) |
| `MEDIA_CONTROL_SEEK_RELATIVE` | seconds, signed | clamped to the media |
| `MEDIA_CONTROL_PAUSE` | 1 pause, 0 play | |
| `MEDIA_CONTROL_VOLUME` | 0..2 | 0..1 a perceptual 60 dB taper; 1..2 a boost to x2 (+6 dB) through a soft limiter from -1 dBFS that never reaches full scale (API 16); unmutes |
| `MEDIA_CONTROL_MUTE` | 1, 0 | |
| `MEDIA_CONTROL_SPEED` | 0.25..4 | pitch kept (4x since API 16: a 220 Hz tone stays 220 Hz) |
| `MEDIA_CONTROL_STEP_FRAME` | +1, -1 | windowed engine; pauses first |
| `MEDIA_CONTROL_AUDIO_TRACK` | a track id | API 16, windowed engine; spliced into the sound without a gap |
| `MEDIA_CONTROL_SUBTITLE_TRACK` | a track id, 0 the sidecar, -1 none | API 16, windowed engine |

- A value out of range returns `MEDIA_ERROR_INVALID_ARGUMENT` at once and nothing is queued.
- `MEDIA_ERROR_REFUSED` from the post means the ring (32) is full.
- A refusal in the event means the media cannot do it: not seekable, or no picture to step from.
- `MEDIA_EVENT_POSITION` comes 10 times a second while playing (4 before API 15).

## Tracks (API 16; host mode since API 17)

```c
uint32_t n = 0; media_engine_track_count(e, &n);
for (uint32_t i = 0; i < n; ++i) {
    media_track_info_t t = { .struct_size = sizeof t };
    media_engine_track_info(e, i, &t);  /* id, kind, codec, language, title, channels, rate, size, selectable, active, is_default */
}
media_engine_post_control(e, MEDIA_CONTROL_AUDIO_TRACK, 3, id);
```

- The list holds every track of the file (refused ones too, with `selectable` 0) and the subtitle sidecar (id 0).
- It is fixed from open to close, so any thread may read it.
- `active` is what the engine last published: the video that plays, the audio playing, the subtitles shown.
- Both modes since API 17. Host mode lists the audio tracks it plays (PCM, or through the host's `decode_audio`) as selectable,
  and the file's own subtitle tracks (MP4 timed text, Matroska text and ASS) as channels of its subtitle track; the two
  `MEDIA_CONTROL_*_TRACK` controls run on the host's thread at its next step. A live sensor has no tracks (count 0).
- `codec` for a Matroska track is its MP4 name: `avc1`, `hvc1`, `av01`, `mp4a`, `Opus`; `text` and `ssa` for subtitles.

## Subtitles after open (API 17)

```c
media_engine_attach_subtitles(e, "C:\\Users\\me\\Downloads\\film.en.srt");  /* UTF-8; .srt or .vtt, at most 4 MiB */
```

- The file becomes the sidecar (track id 0) and is shown at once; the file's own subtitle tracks stay. A later call replaces it.
- Host mode: parsed on the host's thread during the call, into the half of the subtitle region the engine reserved at open, and
  swapped in. Nothing is allocated, and the text shown before stays valid until the next attach.
- Windowed engine (2026-11-05): parsed on the caller's thread into one of two regions reserved at open, then handed to the
  window's thread, which rebinds the subtitle renderer to it on its next pass. The call returns when the shaping thread has
  taken it (at most 3 s). The renderer is prepared at open even when the file has no subtitles, with an empty track and a
  glyph atlas of at least 512 cells, so the first attach needs nothing new. Nothing is allocated and no picture is dropped:
  `media_player_demo --test-attach` attaches an .srt at 1 s from another thread, refuses a file without a cue at 2 s, and
  replaces the first file with a .vtt at 3 s. It runs as `media_fixture_attach_mp4_test` and `media_fixture_attach_mkv_test`
  (0 dropped, 0 allocations). `tests/embed_tests.c` calls it through `avion.dll`.
- `MEDIA_ERROR_OPEN`: the file cannot be read or holds no cue; what was shown is unchanged. `MEDIA_ERROR_UNSUPPORTED`: no
  subtitles module. `MEDIA_ERROR_STATE` (windowed): the previous attach is still being taken. `MEDIA_ERROR_PLAYBACK`
  (windowed): the window's thread did not take it within 3 s.

## Snapshots into memory (API 17)

```c
static uint8_t pixels[3840 * 2160 * 4];
media_snapshot_buffer_t b = { .struct_size = sizeof b, .format = MEDIA_PIXELS_BGRA8, .pixels = pixels, .capacity_bytes = sizeof pixels };
media_status s = media_engine_snapshot_to_memory(e, &b);   /* b.width, b.height, b.pts_100ns, b.frame_hash */
```

- The image `media_engine_save_snapshot` would write (the window's composed picture; host mode's GPU image or software view),
  as 8-bit BGRA or RGBA with alpha 255, rows `stride` bytes apart (0: width x 4). Its `frame_hash` is the BMP's.
- Synchronous in both modes. The windowed engine hands the request to its window thread and waits up to 3 s. Calling it from
  the window's own thread (an `on_frame` callback) returns `MEDIA_ERROR_STATE`, since it would wait for itself.
- `MEDIA_ERROR_STORAGE`: the buffer is too small; `width` and `height` say what it needs. No file, no sidecar, no allocation.

## Opening with headers, and cancelling an open

```c
media_open_options_t o = { .struct_size = sizeof o, .http_headers = "Authorization: Bearer ya29...." };
media_engine_open_ex(e, "https://www.googleapis.com/drive/v3/files/<id>?alt=media", &o);
```

- `http_headers` go out with every range GET.
- With headers, the engine does not follow redirects, so a token is never sent to a host the server points to. A redirect
  fails the open, and its status is the code (`media_engine_last_error`). The same applies to 401, 403 and 404.
- Up to 4095 characters. `Range`, `If-Match` and `Host` belong to the engine: passing them returns
  `MEDIA_ERROR_INVALID_ARGUMENT`.
- `media_engine_cancel_open(e)` can be called from any thread. It stops the open in progress at its next wait (the network
  probe, the metadata reads). The open then returns `MEDIA_ERROR_OPEN`, stage 1, code `E_ABORT`, and the text says
  `cancelled`. Outside an open it does nothing, so a later open is not affected. When the user switches files, cancel, wait
  for the open call to return, then open the next file.

## What plays (2026-11-05, API 17)

| | Supported | Not yet |
|---|---|---|
| Containers | MP4, M4V, MOV (QuickTime, iPhone), 3GP; classic and fragmented (CMAF); **MKV and WebM** (API 17: SimpleBlock and BlockGroup, Xiph/EBML/fixed lacing, unknown-size clusters, chapters) | AVI, TS/M2TS, FLV, WMV, MPEG-PS; Matroska with compressed or encrypted frames (ContentEncodings) |
| Video | H.264 Baseline/Main/High, progressive 4:2:0 8-bit; HEVC Main and Main 10 (HDR10 tone-mapped), on the GPU; **AV1** Main 4:2:0 8/10-bit through Windows' own software decoder (the AV1 Video Extension; API 17, ENGINE_MANIFESTO.md 2E) | VP9, VP8, MPEG-2, VC-1; H.264 10-bit, 4:2:2, 4:4:4, interlaced; AV1 on the GPU (no D3D12 AV1 decoding here; UHD 630 has none) |
| Audio (windowed) | AAC-LC up to 5.1, **Opus** mono/stereo (Windows' decoders) | AC-3/E-AC-3, DTS, FLAC, MP3, Vorbis, 7.1 AAC, multichannel Opus |
| Audio (host mode) | PCM tracks; anything the host's `decode_audio` decodes (AAC and Opus units are handed to it) | |
| Edits and timing | one edit, leading empty edits (a delayed track), a last sample of no duration, a short last AAC frame; Matroska's CodecDelay | multi-segment edits, rate edits |
| Subtitles | tx3g (MP4 timed text), **Matroska S_TEXT/UTF8, WebVTT, ASS/SSA** (text only, styling dropped), SRT/WebVTT sidecar at open, **a sidecar after open in host mode** | PGS, VobSub; a sidecar after open in the windowed engine |

Stage numbers added in API 17: 40 the platform's AV1 decoder could not be prepared, 41 it failed while playing.

A Matroska file is indexed by one sequential read of its clusters (`docs/matroska.md`): playback starts from the part already
read, and seeking waits until the whole file is indexed. Measured: a 1.4 GB 4K60 HEVC file in 0.46 s from the system's cache.

The matrix behind this table is `tools/realworld/make_compat_matrix.py`. Run `tools/realworld/probe_open.py <avion.dll> <files>`
on your own files to get the same lines this page quotes.
