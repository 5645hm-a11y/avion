# Sessions across files: resume and the folder (`session::ResumeLedger`, `session::Playlist`)

Two things a player does between files:

- It remembers where each file was left, and the tracks chosen. A file opened again starts there.
- It plays the folder: **Ctrl+Right** / **Ctrl+Left**, or "next video", "הסרטון הקודם" and their counterparts in seven more languages, open the next or previous playable file of the folder the file is in.

Both live between files, never while one plays. The engine's state machine is unchanged: a file ends its `Run` (Running → Draining → Stopped), and the next one is prepared (Preparing → Primed → Running) by a coordinator of its own.

## The resume ledger

`session::ResumeLedger` (`include/media/session/MediaSession.hpp`, portable) holds the last **64 files** played, as fixed 48-byte entries:

| Field | What it is |
|---|---|
| `key` | FNV-1a of the file's full path, with ASCII case folded and `/` read as `\` (a Windows path names the same file in any case) |
| `file_bytes` | The file's size when recorded: another file saved under the same name is not resumed |
| `position`, `duration` | 100 ns |
| `audio_track`, `subtitle_track` | Container track ids; the subtitle sidecar is 0, no subtitles `kNoTrack` |
| `stamp` | Recency. A new file takes a free slot, else the least recently recorded one's |

**Resume rule** (`ResumeAt`): the position is used when it is past the first **15 s** and more than **60 s** before the end. Closer to the end, the file counts as watched and starts from the beginning. With no duration known, only the first rule applies.

**On disk:** `%LOCALAPPDATA%\MediaEngine\resume.bin`, 3,088 bytes: a 16-byte header (`MRL1`, version, an FNV-1a checksum of the entries) and the 64 entries, little-endian as in memory. `SaveLedger` writes `resume.bin.new`, flushes it, and moves it over the old one (`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`), so a crash leaves the old ledger or the new one, never half of one. A damaged, foreign or other-version file loads as an empty ledger.

**In the engine** (`PlaybackOptions::resume`, the caller's ledger):

1. `Prepare` looks the file up (`platform::windows::FileIdentity`: full path and size; local files only, not HTTP).
2. A known file starts with its tracks as they were left:
   - the audio track chosen last time is preferred over the language asked for and over the file's default;
   - the subtitle track too, or none if subtitles were off.
3. When `ResumeAt` gives a position, `Run` queues a seek to it **before the worker starts**. The first picture decoded is then the landing, and no picture from 0:00 is shown first. The seek lands as any seek does: on the picture whose interval holds the position, with its audio.
4. A status cue says so: `Resuming at 1:02:04 · Home: from the start`, or `2 of 4 · Episode 2.mp4 · from 1:02:04` in a folder. **Home** seeks to the start, as always.
5. When `Run` ends (for any reason), the position (`Position()`: where playback is, or a seek's target still on its way) and the tracks chosen are recorded. The caller saves the ledger.

The ledger is only touched outside `Run`'s measured window: in `Prepare`, and once `Run` has stopped the worker. `Run` itself allocates nothing, as before. The start cue is assembled by hand, not with `snprintf`.

## The folder

`session::Playlist` lists the playable files of a folder (`.mp4`, `.m4v`, `.m4a`, `.mov`, `.3gp`, any case; an `.m4a` plays in [music mode](audio-dsp.md#music-mode-2026-10-20)) in **natural order**:

- case is ignored;
- digit runs compare as numbers, whatever their length (`Episode 2` before `Episode 10`, `S01E09` before `S01E10`);
- names equal to a reader (`ep 02`, `Ep 2`) are ordered by their code units, so the order is total.

Its storage is fixed: 2,048 names and 128 Ki code units. `platform::windows::ListFolder` fills it with `FindFirstFileExW` and finds the opened file's place. Directories and other extensions are left out.

**Next and previous** (`PlaybackCoordinator::RequestFile`):

- Ctrl+Right / Ctrl+Left (`HudCommand::File`), and the `NextFile` / `PreviousFile` intents ([intents](intents.md#files-2026-10-20)) typed, spoken or scripted, end `Run` with `RequestedFile()` set.
- With no file that way (`PlaybackOptions::files_before` / `files_after`), the grammar refuses the command as `Unavailable`. The keys get a warning cue: `No next video in this folder`.
- The caller (the demo's `RunSession`) closes the coordinator, saves the ledger and opens the next file with a new coordinator.
- A file that cannot be prepared (not a film, AV1, a damaged file) is **passed over** in the direction of travel.
- At the ends of the folder there is no wrap-around.

**One window.** Before `Close`, `KeepWindow()` keeps the window open. It stays where it is, as large, fullscreen or not, and its messages go to the default window procedure meanwhile. The next file adopts it (`PlaybackOptions::window`, `D3D12Renderer::Adopt`) at its client size instead of creating one, with a new swap chain on it. A window that was closed in between (Alt+F4) is replaced by a new one.

## Measured

`media_playback_resume_test`, Release, the development machine (Intel UHD 630), Chrome running:

| | |
|---|---|
| Resume landing | Target 3724.778 s, landed on the picture at 3724.750 s (41.7 ms interval, so it holds the target), 7.3 ms after `Run` began. The second resume: target 3726.250 s, landed at 3726.250 s. |
| Between files | **12.4–19.3 ms** from one file's `Run` returning to the next file's first picture, in a warm session (below); 401–581 ms with `--cold`, the old path. |
| Each file | 0 allocations in `Run`, 0 dropped pictures. |

## Warm switch (2026-10-20)

Directive: "PERSISTENT GPU WARM-SWITCH BETWEEN FILES". Between files, a session (`engine::WarmSession`, owned by the caller: the demo's `RunSession`) now keeps everything a file does not change:

| Kept | Where | Made again when |
|---|---|---|
| The window, its position, size and fullscreen state | `render::GpuSession::window` | It was closed in between (Alt+F4) |
| The D3D12 device, direct queue and fence, the swap chain and its buffers, the composition lists, the colour, HUD, blit, status, command-bar and visualiser pipelines, and the 4 MiB text arena | `render::GpuSession` | After a device loss (as before); the swap chain is resized like any resize when the window's size changed |
| The RGB slots and the history | `GpuSession::slots`, `history` | The next file's pictures are fitted to another size |
| The 256 MiB file arena, committed | `WarmSession::arena` | Never (`LinearMemoryArena::Reset`: rewound and unsealed, pages kept) |
| The D3D12 video decoder, its DPB and its heaps | `WarmSession::decoder`, in `decoder_arena` | The next stream does not fit it (`D3D12VideoDecoder::Compatible`: codec, bit depth, size, surfaces, bitstream capacity); then it is closed and prepared again in its own arena |
| The WASAPI endpoint and its ring | `WarmSession::sink`, in `sink_arena` | The next file asks for another format (rate, channels, layout, conversion) |
| The MTA (`CoIncrementMTAUsage`) | `WarmSession` | Never |

**Per file**, the coordinator still makes its own: the demuxer and its tables, the codec syntax state, the subtitles, the timeline, the AAC decoders, the DSP, the clock, the queues. A kept decoder is restarted (`Restart(epoch)`: waits for its fences, forgets every lease and DPB hold, starts the epoch again; the surfaces stay). A kept endpoint is reset (stopped, its queued PCM dropped, its clock statistics restarted).

**Between files the window holds the last picture.** The renderer's `Close` in a session waits for the queue, moves the slots and the history to the session, and parks the window (its messages go to the default procedure). The swap chain still shows the last image until the next file's first composite.

**The state machine is unchanged.** Each file still goes Preparing → Primed → Running → Draining → Stopped in a coordinator of its own; the session only lends it storage. Nothing is allocated in `Run` (0 per file, as before). `--cold` runs the old path (each file makes and releases its own) for A/B.

**Measured** (`media_playback_resume_test`, Release, UHD 630, "between files" = the previous file's `Run` returning to this file's first picture on screen):

| | Warm | Cold (`--cold`) |
|---|---|---|
| Ordinary files (the 3 s and 8-track fixtures) | **12.4–19.3 ms**: closing 1.7–1.8, ledger 0.7–0.8, `Prepare` 6.7–10.3, the first picture's decode and composite the rest | 401–454 ms: closing 60–69, `Prepare` 313–360 |
| The 3.5-hour file (resumed) | 60–65 ms: its `Prepare` takes 55 ms, of which 46 ms validate its 3.5 hours of sample tables | 581 ms |
| Reused over the 8 files prepared | arena 7, device and window 5, slots 5, decoder 5, endpoint 5; made: decoder 1, endpoint 1 | — |

The test asserts the warm switch **under 50 ms** for the three ordinary files (the second includes passing over the file that is not a film).

**Music mode in a session** ([music mode](audio-dsp.md#music-mode-2026-10-20)). An `.m4a` between two films uses the session's device, window and endpoint, and leaves the video decoder and the RGB slots alone: it makes only 64 × 64 placeholders of its own. The next film takes back both the decoder and the slots. `media_playback_music_session_test` plays the 3 s fixture, the M4A and the 3 s fixture again (Ctrl+Right, `next video`, Escape). It asserts one window; one device, decoder, set of slots and endpoint for the three files; and both switches under 50 ms. Measured: **14.1–14.4 ms** into music mode and **15.2–15.4 ms** out of it, three runs.

## Verification

| Test | Checks |
|---|---|
| `media_session_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | The resume rule at each edge (15 s, 60 s before the end, unknown duration, a 70 s file). Record, update and recency; the 65th file evicts the least recent one after a re-recorded file became recent again; `Forget`; key 0 ignored. Saved and loaded byte for byte; a flipped bit, a foreign magic, a short file and another version all load as an empty ledger. The recency clock at its end: entries renumbered 1, 2, … in their order. Path keys: case and separators folded, other paths distinct. Natural order: numbers of any length, case, leading zeros, extensions; 2,051 names into 2,048 places (3 skipped, sorted `f0` … `f2047`). 0 allocations. |
| `media_playback_resume_test` | A folder beside the fixtures: `Episode 1` (the 8-track fixture), `Episode 2` (the 3.5-hour long-form fixture), `Episode 3` (not a film), `Episode 10` (the 3 s fixture), `notes.txt`; a fresh ledger. Six files: 2 → (Ctrl+Right, 3 passed over) 10 → (`הסרטון הקודם`, 3 passed over) 2 → (`previous video`) 1 → (`hebrew audio`, `english subtitles`, `next video`) 2 → (Ctrl+Left) 1 → Escape. Checked: the file each step opens, the first window new and every later one adopted, 0 allocations, errors and drops per file; Episode 2 new to the ledger, then **resumed twice**, each time on the picture holding the position recorded when it was left, with the cue naming its place and time; Episode 10 left within its first seconds; Episode 1 opened again **with Hebrew audio (3) and English subtitles (7)** and from the start (under 15 s in); Episode 3 passed over twice; the ledger saved after every file and, reloaded from disk, holding the three files as they were left. |
| `media_intent_dispatcher_tests` (`Files`) | 31 file commands in nine languages, through `Parse` and `Interpret`; refusals at either end and without a folder; the neighbours keep their meaning (`הפרק הבא` is still the next chapter, `next one` the next match, `find the next video` a quote). |

```powershell
.\build\Release\media_player_demo.exe 'C:\Films\Episode 2.mp4'   # Plays the folder; resumes where it was left.
ctest --test-dir build -C Release -R "session|resume" --output-on-failure
```

## Open

- ~~The pause between files is a whole new engine.~~ Since 2026-10-20 a warm session keeps the device, window, arena, decoder and endpoint ([warm switch](#warm-switch-2026-10-20)): 12–19 ms between ordinary files. The 3.5-hour file still validates its sample tables (46 ms) on every opening; they could be kept per file in the session.
- The window holds the last image between files (the swap chain is not released). What it looks like when the next file has another shape has not been checked.
- Only local files are resumed and played as a folder: an HTTP source has no folder and is not recorded.
- Resume is automatic. There is no "resume or start over?" prompt; Home starts over.
- The ledger has one writer: two players open at once would each save their own view, and the last save wins.

## Flight recorder, telemetry readout and watchdog (2026-10-21)

Directive: "FLIGHT RECORDER & SELF-EXPLAINING DIAGNOSTIC HUD (P0 TASK 08 / AI A04)", and the hardening of the two intermittent suite findings.

### Flight recorder

`core::FlightRecorder<256>` (portable, `include/media/core/flight_recorder.hpp`): the newest 256 records of one writer thread, readable at any time from any thread.
- **Storage.** Fixed, 40 bytes a slot plus a cache line for the write cursor; no allocation, no lock, no wait.
- **Seqlock slots.** The writer marks a slot odd while it writes it and stamps it with the record's number when done. A reader drops a record overwritten while it was being read; it never returns a mixture of two.
- **Two recorders.** The engine has one per writer: `UiEvents()` (presentation thread) and `WorkerEvents()` (media worker). `core::MergeNewest` reads both in time order.

| Event | Recorded by | Fields |
|---|---|---|
| `SeekRequested`, `SeekLanded` | UI: `SendSeek`; a landing seen by `Stream` | epoch, target or PTS, request-to-screen latency |
| `SeekApplied` | worker | epoch, target; continued, queue seek, preview |
| `TierChange` | UI, from the quality governor | from, to, reason, p95, frame budget |
| `Drop` | UI, from the renderer | PTS, lateness, what it waited for, behind the clock |
| `Pause`, `Resume`, `Speed`, `Zoom` | UI | position, speed, zoom and centre |
| `TrackRequested` / `TrackSwitched` | UI / worker | track; splice time and request-to-splice delay |
| `AudioMoved`, `Underrun` | worker | endpoint changes, clock re-anchors so far |
| `DeviceLost`, `DeviceRecovered` | the thread that saw it / UI | detector and reason; loss-to-rebuilt time |
| `LoopSet`, `LoopCleared`, `LoopWrap` | UI | A, B, turns |
| `Stats` | UI | the readout was asked for (and whether it was the explanation) |

### Telemetry readout (AI task A04)

| Input | Shows |
|---|---|
| **I** | The panel; again: hidden. It is refreshed once a second while up; another status cue over it ends that |
| `show stats`, `status report`, `הצג נתונים`, `statistiken`, … | The panel |
| `hide stats`, `הסתר נתונים`, … | Hides it |
| `why did quality change?`, `למה האיכות ירדה?`, `why is the picture blurry`, … | The explanation, for 10 s |

Commands in nine languages: [intents](intents.md#loop-and-telemetry-2026-10-21).

Everything shown is the engine's own state at that moment (`PlaybackCoordinator::Telemetry()`, a `ui::TelemetrySnapshot`): the quality governor's tier, mode, window, p95, budget and last change; the picture's codec, depth, transfer and sizes; the audio routing; the renderer's counters; the loop; the newest flight recorder events. `ui::ComposeStats` and `ui::ExplainQuality` (portable) only format it. A number not measured is not printed: no GPU line without timestamps, no "Now" line before the new tier has a sample.

The panel is the status cue grown to several lines. A line feed in its text starts a line (`StatusCue::kMaxLines` = 7), the pill becomes a glass panel one `kLineHeight` taller per line, up to `kMaxPanelHalfWidth`, and its top edge stays put. It takes its new height when its new text's raster is ready, never before. Measured here (the 64 × 64 long-form fixture; `media_playback_telemetry_test`):

```
Quality · Edge-directed (auto) · GPU 1.6 ms p95 of 41.7 ms
Why · Neural needed 36.4 ms, over 85% of the 41.7 ms frame · 1 s ago
Picture · H.264 8-bit 64×64 → 640×640
Audio · 48 kHz stereo
Frames · 76 shown · 0 dropped · tile reuse 46%
Last · Neural super-resolution → Edge-directed (GPU 36.4 ms) · 1 s ago
```

and, asked why:

```
Quality · Edge-directed since 2 s ago
Neural super-resolution needed 36.4 ms per picture, over 85% of the 41.7 ms frame (35.4 ms)
Now 1.7 ms per picture (p95)
```

The test asserts the following:
- each readout starts with the tier the governor had;
- each panel states the pictures shown so far;
- the explanation names the tier left and the p95 the governor measured, to 0.1 ms;
- the flight recorder holds the change with that same p95;
- the panel is hidden at the end;
- 0 allocations, 0 drops.

`media_telemetry_explain_tests` covers the rest:
- the recorder's bounds and order;
- a writer filling a ring 3,000,000 times while a reader took 765,782 snapshots: 0 torn records, 0 out of order;
- the merge;
- the exact wording of every reason (overload, over the whole frame, headroom, by hand, unavailable, unchanged), the panel's sections and line count, cut text always well-formed UTF-8, every event kind described;
- 0 allocations.

### Watchdog: a hang becomes a quick, explained failure

`media_playback_tracks_test` once ran 1,218 s in Debug, under a CTest timeout of 60 s. The waits were audited first:
- the worker's are bounded: the track switch's fade waits 50 ms at most, the seekable scan 5 s, and the others end with `stop_`;
- `Run` waited without bound for the worker to stop. It now waits 10 s, then fails with stage 36;
- `Close` still joins the threads without a timeout: a thread that never returns cannot be released safely.

The engine now publishes each thread's progress: `PlaybackCoordinator::Alive()` gives a pass count and the phase it is in. The presentation thread's phases are window messages, scripts, stream, converting a picture, presenting, stopping and closing; the worker's are commands, track switch, seek, media, stopped and teardown.

In every automated run, the demo's watchdog thread reads it every 250 ms and ends the process with exit code 3 when:
- either thread has made no progress for 20 s (a worker waiting for its teardown is not stalled), or
- the run outlives its `maximum_seconds` by 15 s.

Before exiting, it prints which thread was where. The watchdog thread is started and running before the allocation probe begins: its start-up allocates.

**It caught the cause.** In the same unattended night, `media_playback_intent_test` "timed out" after 1,217 s (CTest timeout 30 s) and printed: `WATCHDOG: the presentation thread made no progress for 20 s; presentation thread: stream …, media worker: worker: media …`.
- Its `TerminateProcess` still took 20 minutes to complete. A thread blocked in kernel mode, in a GPU or presentation call, cannot be ended before it returns.
- The System log explains why. The machine went to standby (Kernel-Power 42) at 02:59:55 and 03:40:32, and left its low-power state at 03:20:08 and 04:00:46: 20 minutes later each time. The run started at 03:40, and the earlier 1,218 s has the same length.
- The four single-drop failures that followed (35–140 ms convert or present calls) ran in the first minute after the wake.

The hangs were the machine sleeping under a playing test, with the picture's GPU work suspended until it woke. The engine did nothing to prevent that; now it does.

### Staying awake

While playing, the engine holds `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)` on its UI thread, as any video player does: the display does not dim or sleep and the machine does not enter standby under a film. Music mode holds only the system. It is released on pause and when `Run` ends, so a paused player lets the machine sleep as usual.

The idle timers count on while the request is held. After hours of unattended playing, a plain release let the machine sleep the moment nothing held it: a test sleeping while it prepared, between two playing tests. So the engine resets them once when it releases the request, and once when a file is opened. The usual countdown then starts from the end of playback.

Measured:
- The final Release suite (11 minutes, most of it playback) ran across the time the next standby was due, and the machine stayed awake.
- The Debug suite that followed was put to sleep at 05:12 during a CPU-only unit test, which holds nothing. CTest itself does not keep the machine awake: run long unattended suites with the machine's sleep disabled. `PlaybackCoordinator::AwakeHeld()` is the state Windows reported when the request was released; `media_playback_telemetry_test` checks it held system and display (`80000003`).

## Bookmarks (2026-10-21)

Directive: "TACTICAL BOOKMARKS (AI TASK A23)".

**Setting and moving.**

| Input | Effect |
|---|---|
| **B**, `add bookmark`, `סמן הערה`, `שמור נקודה`, … | A bookmark at the position (the clock's media time); also pins the frame on screen ([picture](picture.md#pinned--live-2026-10-21)) |
| **Ctrl+B**, `pin frame`, `הקפא פריים להשוואה` | The frame alone |
| `go to bookmark 2`, `לך לסימנייה 3` | That bookmark (`OutOfRange` past the last) |
| `go to bookmark`, `next bookmark`, `הסימנייה הבאה` | The first one more than 0.25 s after the position |
| `previous bookmark` | The last one more than 1 s before the position (from just after one, the one before) |

- The list (`session::Bookmarks`, portable) keeps up to 32 per file, in order. A bookmark within half a second of an existing one is that one.
- The dock draws each bookmark as a small amber flag above its timeline (`ui::BookmarkMarks`, 16 more root constants: the HUD pass now holds the 64 a root signature allows).
- Cues: `Bookmarked 3 · 0:12.4`, `Bookmark 2 of 5 · 0:12.4`, refusals in amber.
- The flight recorder notes each (`Bookmark`: added, gone to, frame pinned).

**Kept with the file.** The resume ledger is now version 2: each entry carries its file's bookmarks (304 bytes an entry, 19,472 bytes in all).
- A version 1 ledger (3,088 bytes) still loads, every field kept, without bookmarks. A damaged list is cut to its valid, ordered prefix.
- The coordinator loads a file's bookmarks in Prepare and writes them back in `Remember`.

**Measured** (`media_playback_bookmark_compare_test`, the long-form fixture; `media_bookmark_compare_tests`, portable):
- B at 1.0 s, `add bookmark` at 2.5 s and `סמן הערה` at 4.0 s set bookmarks at 1.0002, 2.5001 and 4.0001 s.
- `go to bookmark 1`, `next bookmark`, `לך לסימנייה 3`, `previous bookmark` and `go to bookmark 3` (paused) each landed exactly on its bookmark's picture, in 3.5–6.3 ms.
- The ledger was saved and loaded, and the file opened again with it had the same three bookmarks.
- The unit tests cover:
  - the list's rules;
  - the ledger, version 2 and version 1;
  - 22 commands in nine languages and their refusals (no bookmarks: `NoContext`; bookmark 9 of 3: `OutOfRange`; `two three`: `Malformed`);
  - `compare quality` and `בטל` keeping their meanings;
  - 0 allocations.

### Unattended suites stay awake

Every test process that links the allocation probe holds `ES_SYSTEM_REQUIRED` for its lifetime (`tests/allocation_probe.cpp`), and resets the idle timer when it ends. So an unattended CTest run is no longer put to sleep inside a long CPU-only test, as the Debug suite was at 05:12 inside `media_acoustic_intent_tests`. The display may still sleep: the tests do not need it.

## Low-power pacing (2026-10-22)

Directive: "HYBRID LOW-POWER WAITABLE PACING (REVIEW TASK 05)". Until now the presentation thread (the window's thread) and the media worker each spun a whole core, playing or paused: `YieldProcessor()` at the end of every pass (28 million passes in 18 s). `PlaybackOptions::low_power_pacing` (on by default; the demo's `--busy-spin` turns it off for comparisons) makes both wait when there is nothing to do.

**The presentation thread** (`PlaybackCoordinator::Pace`, at the end of each pass):
- `D3D12Renderer::NextWake` says when `Present` must run again, as the last call left it: the next picture's deadline (*precise*), the controls' next refresh, or their next advance (8.3 ms, *coarse*), or at once (a due picture waiting on the GPU, a staged image to retry, a resize, a landing, a picture the clock gives no deadline yet). Controls held back because a picture is due within one refresh wake for the picture, not for the controls.
- `D3D12Renderer::ConvertWait` says why the next decoded picture is not converted: no free slot (a flip frees one: wait), its decode or the conversion in flight (`ArmConvertWake` sets an event on that GPU fence), or nothing (spin).
- It spins as before while something is in motion: the start before the clock runs, a seek until its picture is on screen, frame steps, a device recovery, a resize, a scrub, a queue seek's adoption.
- Otherwise it waits on a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) until a picture's deadline less `kPaceSpin` (1.2 ms, then spins the rest; a picture due within `kPaceFloor`, 2 ms, is spun for at once) or until the controls' time, and on the worker's word that a picture is decoded (`WakePresenter`, a seq_cst handshake on a flag, `SetEvent` only while the thread waits) and the armed fence. The A–B loop's B is a deadline too.
- **Messages.** While a picture has a deadline the wait is `WaitForMultipleObjects` on those handles alone; only without one (paused, the controls alone) is it `MsgWaitForMultipleObjectsEx(QS_ALLINPUT, MWMO_INPUTAVAILABLE)`. Measured: the same high-resolution timer returned over 1 ms late on 1 % of message waits, 4-38 ms now and then (a 60 fps picture dropped behind one), against at most 1.8 ms without the message queue; a bare timer in another process stayed under 1.1 ms meanwhile. Raising the thread's priority, MMCSS "Playback" and opting out of power throttling did not change it. Every wait lasts at most one advance of the controls, so input waits at most 8.3 ms while playing.
- **The controls repaint only when they change.** The dock was redrawn 120 times a second whenever it was on screen (`hud_was_drawn_`), also paused and still: 455 controls-only images in 4 s of pause. `D3D12Renderer::HudChanged` compares the uniforms (all but the clock, which the shader does not read, to 1e-4) and the marks with what the last image drew: now about 51 in the same pause (the dimming and the dock settling).

**The worker** waits at the end of a pass that changed nothing (no command, no returned picture or packet, no PCM written, no packet read, decoded or submitted, no state change) on: the endpoint's WASAPI period event (`Win32WasapiAudioSink::Wake`; `Pump(signaled)` then fills it at once), a wake event the UI thread sets after each command and returned picture (`WakeWorker`, `CommitControl`), and a high-resolution timer: 2 ms while playing, bounded by the next clock observation (every 10 ms), 10 ms paused. It spins as before until the clock starts (and through a seek's pre-roll), during a device recovery, and while a read is in flight.

**Measured** (`media_playback_pacing{24,30,60}_test` on 64x64 H.264 at 24, 30 and 60 fps; `media_fixture_pacing_24_test` on 1080p24 HEVC, `media_fixture_pacing_60_test` on 720p59.94 H.264; each plays to 6 s, pauses 4 s, plays to 14 s; Release, UHD 630, i7-10700, 2026-10-27, two runs each). CPU time per thread from `GetThreadTimes` as a share of one core (`PlaybackCoordinator::PacingStats`, the demo's `Pacing:` line):

| | Busy spin (before): presentation / worker / process | Low-power (after): presentation / worker / process | Dropped |
|---|---|---|---|
| 24 fps playing (synthetic; film) | 97.9-99.3 / 98.8-99.8 / 196.6-199.1 % | 0.6-11.7 / 5.0-5.9 / 5.9-16.7 % | 0 |
| 30 fps playing (synthetic) | 97.1-98.4 / 99.3-99.6 / 196.7-197.7 % | 4.9-6.4 / 4.2-5.2 / 9.1-11.6 % | 0 |
| 60 fps playing (synthetic; film) | 98.3-99.2 / 98.6-99.7 / 197.0-198.8 % | 7.1-10.2 / 4.1-7.5 / 11.1-17.2 % | 0 |
| Paused (every file) | 94.9-99.6 / 96.5-100.0 / 191.4-199.6 % | 0.0-1.2 / 0.0-0.8 / 0.4-1.6 % | |

Before a picture the timer woke 0.30-0.37 ms late on average, 1.83 ms at most; 0 to 2 of about 1,900 waits a run over 1 ms. At 60 fps most of the presentation thread's share is the 1.2 ms spin before each picture (7 % of 16.7 ms). The tests require at most 20 % per thread playing and 5 % paused, at least one wait of each thread, and no drop. In Debug: 2.3 / 11.2 % (24 fps) and 7.8 / 11.6 % (60 fps) playing, 0.8-1.6 % paused, no drop.

**Found on the way: a resume could drop the picture due at the pause.** With the scripted pause landing within about 2 ms of a 60 fps picture's deadline (pacing moved the moment a few milliseconds), `media_fixture_pacing_60_test` dropped that picture after the resume in 4 of 8 runs, always "late by 18.1 ms". At the pause the clock and the device agreed to 0.1 ms and the worker stopped the endpoint within 0.4-1.1 ms; but after `Start` the clock waited for the device's first precise position, 10-20 ms of playing later, and a picture whose deadline had passed meanwhile was late by more than its interval. Now the worker keeps the stopped position and its media time at the pause (one timeline, one speed), and anchors the clock there at the instant it restarts the endpoint; the next precise observation disciplines it as usual (the PLL accepted it in every run: one re-anchor per pause, as before). 8 of 8 then, no drop, latest flip 0.4-2.8 ms after its deadline.

### The settled pause (2026-11-03)

The head-to-head benchmark (docs/benchmarks.md) measured the demo at 1.17 % of a core paused, VLC at 0.00 %. Per thread (`tools/bench/thread_cpu.py`; the engine names its threads since this change: `media worker`, `media window`, `media perception`, `media voice`, `media snapshot`), the presentation thread and the worker each took about 0.4 %:

- **The presentation thread** came back every 8.3 ms to advance controls that were still: 120 passes a second.
- **The worker** waited 10 ms paused, although the endpoint is stopped and every command and returned picture already sets its wake event: 100 passes a second.

Now, paused and *settled*, the presentation thread sleeps up to 250 ms between advances (`D3D12Renderer::SetSettledWait`). Settled means:
- no picture due;
- the controls drawn as they are, and nothing in them that ends with time (a drag, a seek's commit, the volume pill, a press: `KineticHud::Settled`);
- no preview, re-conversion, capture, snapshot or tensor export pending, and every GPU submission complete.

The worker's paused wait is 250 ms too. What wakes them earlier:
- input (the window thread's event);
- a decoded picture (`WakePresenter`);
- a command (`WakeWorker`);
- a host's `RequestStop`;
- a voice command (the voice thread calls `WakePresenter`);
- the next scripted command, typing or pause, at its time.

`PlaybackOptions::settled_sleep` (on) turns it off for the demo's test modes whose scripts act on a paused player by polling. The pacing and fixture runs keep it, and `--test-pacing` now requires at least 3 settled waits in its 4 s pause (15 measured). Measured on `costa_rica_4k60_hdr.mp4` (Release, 20 s paused after 2 s): 0.00 % for every thread, against 0.42 + 0.42 % before. In a 4 s pause the share is 0.4-0.8 %: the dock's reveal and freeze, about 48 composites over the first 0.4 s, are real drawing.

**Playing.** Per thread at 60 fps, costa: presentation 8-10 %, worker 5-7 %, perception 0.3-0.8 %. The warm perception grids (one every 100 ms of media, so that T and Tab start from known objects; `PlaybackOptions::warm_perception`, the demo's `--no-perception`) cost about 3 points in all: 15.5-17.2 % with, 12.0-14.5 % without. Most of the presentation thread's share is still the 1.2 ms spin before each picture's deadline. Measured A/B, two runs each of costa, the synthetic 60 fps fixture and the LG 4K reel, all with 0 drops:

| Spin before the deadline / spun at once within | Presentation thread | Timed wakes after the deadline itself (of about 600) |
|---|---|---|
| 1.2 / 2.0 ms (until the evening of 2026-11-03) | 7.4-9.0 % | 0-3 |
| 0.8 / 1.2 ms | 5.9-8.4 % | 1-9 |
| 0.5 / 0.8 ms | 3.6-5.2 % | 34-156 |

**The pre-deadline spin (2026-11-03, evening).** The timer was already the high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, both threads, since 2026-10-22); its lateness is what the spin covers. The spin is now `PlaybackOptions::pace_spin_us`, 450 by default (the floor 1.5x it; the demo's `--pace-spin-us` for A/B), measured on a quiet machine, two runs of each case:

| Spin | Presentation thread | Presents past the deadline (of ~600; LG ~170) | by, mean / most | Drops |
|---|---|---|---|---|
| 1,200 µs | 8.3-10.6 % | 0-1 | 0.02 / 4.96 ms (one LG start-up conversion) | 0 |
| 450 µs (default) | 2.5-6.9 % | 113-184 (LG 45-50) | 0.07-0.13 / 1.25 ms | 0 |
| 400 µs | 2.8-7.4 % | 207-231 (LG 17-47) | 0.10-0.15 / 1.00 ms | 0 |

"Past the deadline" is the Present call, not the display: a picture is shown at the first vertical blank after it is presented, so a Present 0.1 ms late changes what is seen only when a blank falls inside that 0.1 ms (at 60/s on 60 Hz: 0.1 / 16.7 ms of the late quarter, about 0.15 % of pictures), and then as one refresh's judder, not a drop. A host that wants every Present inside its deadline sets 1,200.

## Staying awake, the whole suite (2026-10-22)

Every test process (the demo and each test linking the probe, `media_real_perception_tests` too) now holds `ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED | ES_CONTINUOUS` from before `main` (`tests/allocation_probe.cpp`). And `media_stay_awake` (a CTest fixture every test requires, so any subset starts it) starts a detached holder that keeps a power request ("PLAYER test suite running", visible to an administrator in `powercfg /requests`: system and display required) and resets the idle timers every 30 s until the CTest process ends (12 h at most).

Why both: the Debug suite of 2026-10-22 slept four times (System log: Kernel-Power 42 "System Idle" at 16:39, 17:19, 18:00 and 18:27; each wake an "ACPI Wake Alarm" 20 minutes later, the hibernate timer), each time 20 minutes after the previous wake, the AC sleep timeout, while test processes holding `ES_SYSTEM_REQUIRED` ran. The cause is not established: the processes run with a medium-integrity interactive token, and the power settings allow system-required requests (`SYSTEMREQUIRED` 1, no request overrides). Two contributions were fixed on the way:
- `KeepAwake(false)` (pause, end of `Run`) called `SetThreadExecutionState(ES_CONTINUOUS)` on the UI thread, which clears that thread's earlier request too: a demo process's own keep-awake ended at its first pause. It now restores the thread's earlier state.
- Between test processes nothing held the system until `media_stay_awake`.

Not verified yet: an unattended suite longer than the 20-minute sleep timeout since these changes (the suites since ran with the machine attended).

## The window thread (2026-10-28)

Directive: "DEDICATED PRESENTATION THREAD (ELIMINATING WIN32 MESSAGE STALLS)". The presentation thread was the window's thread: every window message was dispatched there, between pictures. When Windows held a dispatch (the desktop, the compositor, a slow procedure), pictures waited: suites recorded 50-150 ms "window messages" calls dropping 60 fps pictures (73.6 ms in `media_playback_h264_bframes_test_scrub`, 2026-10-27).

**Now** (`PlaybackOptions::message_thread`, on by default; the demo's `--window-thread-off` for comparisons; `D3D12Renderer::SetMessageThread`):
- One window thread for the process (started by the first renderer that asks, never stopped) creates every engine window and pumps its messages, so a window handed from file to file (warm sessions, the folder playlist) keeps its thread. Windows are attached, detached and destroyed by it (`SendMessage` of private messages: done when they return).
- Its window procedure (`D3D12Renderer::Forward`) answers what must be answered at once (`WM_NCHITTEST`, `WM_GETMINMAXINFO`, `WM_MOUSEACTIVATE` under input isolation, `WM_IME_SETCONTEXT`, `WM_UNICHAR`'s question) from the renderer's published state, and does what only the window's thread may: mouse capture and `TrackMouseEvent`, placing the window for a DPI change, reading the input method's strings (`ImmGetCompositionString`), associating and placing the input method's windows (on the presentation thread's request, posted). Everything else (keys, characters, mouse, wheel, size, DPI, focus, settings) goes into a single-producer single-consumer ring (`core::SpscRingBuffer<InputEvent, 64>`, in the renderer: no allocation), with the modifier keys as they were and its time, and an auto-reset event wakes the presentation thread.
- The presentation thread handles the ring in `Messages()` with the very code the window procedure ran (`Process`): the HUD, the command bar, zoom, the keys. Input isolation's test posts are admitted by the window thread (tags in atomic slots).
- `Pace` waits on kernel objects only, the ring's event among them: input wakes the thread at once. Before, while a picture was due the wait ignored messages (they return late: 2026-10-22) and input waited up to 8.3 ms.
- Fullscreen moves the window asynchronously (`SWP_ASYNCWINDOWPOS`) and asks for the swap chain's new size at once (the borderless window's client is all of it).
- Scripted tests deliver their messages to `Process` directly (`D3D12Renderer::Deliver`, with this thread's key state: a script may hold Control); the renderer's own unit tests (`hud_window_tests`, `palette_window_tests`) keep the window on their thread and drive the procedure as before.

**Measured** (`media_playback_window_stall_test`, new: the 60 fps pacing fixture, the window's procedure blocked for 120 ms at 1, 2 and 3 s of playing, a test mouse move every 250 ms; Release, UHD 630):

| | window thread | the window on the presentation thread (`--window-thread-off`) |
|---|---|---|
| Pictures dropped | 0 (239 shown; three runs) | 21-22 |
| Longest "window messages" call on the presentation thread | 0.06-0.12 ms | 128-134 ms |
| Input, queued to handled | 0.006-0.08 ms mean, 0.5 ms at most | - |

Found while moving: the session scripts hold Control through the thread's keyboard state (`SetKeyboardState`), which a message from the ring did not carry (Ctrl+Right seeked instead of opening the next file); `Deliver` now reads the calling thread's state. The resize test checked the size before the asynchronous fullscreen move had asked for one.

## The audio lead (2026-10-30)

Directive: "AUDIO OUTPUT UNDERRUN CUSHION". The 193-test Release suite of 2026-10-29 had five playback failures with the endpoint underrunning while another application played sound; each passed alone.

**Measured first.** The endpoint (shared mode, IAudioClient3 at the minimum period) has a 1,056-frame buffer at 480-frame periods: 22 ms. Underrun silence is written by the sink only when its PCM ring is empty at a fill, so every recorded underrun was the producer's, not a late fill. And the producer could not keep the ring full: the demuxer hands packets in decode-time order, one at a time, so while the video packet in hand waits for a decoder surface (the presentation behind it, a busy GPU) the audio packets behind it cannot be read, and the PCM ahead of the endpoint is never more than the video decoded ahead of the screen. A neural-reuse run alone (GPU-bound: 15-17 ms a conversion at 60 pictures a second) recorded 52 clock re-anchors and 235,689 frames of underrun silence.

**Now:**
- **Reading ahead for the audio.** When the PCM queued for the endpoint falls under 125 ms while the video packet in hand waits for a surface, the worker reads on: video packets wait in a 32-packet backlog, in order (the one in hand first, parsed or not), and one audio packet is decoded; then the video in hand is tried again (a surface freed meanwhile waits at most one AAC decode), and so on until 250 ms is queued (at most three quarters of the ring) or the backlog is full. Reading on until the PCM was full, the first version held the video back at a 4K HDR cold start (`lg_black_4k60_hdr.mp4`): 5 of 40 starts dropped 3-27 pictures waiting for their conversion, against 2 of 40 without reading ahead (whose audio nearly ran dry at every start: 13-43 ms of PCM at the least, two to six gaps bridged); interleaved, 0 of 80. A seek, a device loss or recovery returns the backlog with the packet in hand (a continuation keeps its video, relabelled as it is taken), the end of input waits for it, a scrub preview never reads ahead. Normal playback stays above 125 ms (170-220 ms at the least): it reads ahead once or twice at the start.
- **Less silence.** Short of PCM, the sink writes silence only as far as keeps half its buffer queued, and none while that much still is: the producer keeps the rest of a period to catch up, and a hiccup costs the silence it needs (before, the whole free space).
- **The bridge.** A gap of at most 20 ms (or two periods) is played with the clock running on (no `NeedsAnchor`, so no hold on the pictures); once its silence has been played the clock is anchored again where the sound is (behind the run-on clock by at most the silence: a picture shown a little longer, none dropped). A longer gap, an endpoint change, or a bridge not done in 150 ms takes the hard re-anchor as before. `Win32WasapiAudioSink::Gaps`, `InGap`, `GapFrames`; `PlaybackCoordinator::BridgedGaps`, `ReadAheads`, `LeastAudioLead`; the demo prints them ("Audio lead:").

**Measured** (Release, UHD 630):

| | before | now |
|---|---|---|
| neural-reuse run (`--test-neural-reuse`), alone | 52 re-anchors, 235,689 frames of underrun silence | 4 re-anchors (one after each of its four seeks), no silence; the least PCM queued 115 ms |
| 4K HDR cold start (`lg_black_4k60_hdr.mp4 --test-fixture-hdr`), 80 starts | the audio nearly dry at every start (13-43 ms) | 0 failed; the least PCM queued 177 ms |
| two gaps of 10 and 15 ms injected (`media_playback_hiccup_test`, new; `InjectGap`) | 2 re-anchors, 1-2 pictures dropped (2 runs with the bridge off) | 2 bridged, 0 re-anchors, 0 dropped (2 of 2 runs) |
| `--test-underrun` (600 ms of PCM withheld) | 1 re-anchor, 0 dropped | the same (too long to bridge) |
| fixture playback, 4K60 films | 0 re-anchors | 0 re-anchors; the least PCM queued 177-189 ms |

The neural-reuse run drops more pictures (253 in all against 242): the clock no longer stops while the GPU is behind, so a conversion that does not fit the frame interval is late, as it should be; its reuse phases still drop fewer than Off (the test's criterion).

**Limits.** The bridge is for short gaps; a producer stalled for longer than the ring still re-anchors. Reading ahead holds up to 32 video packets' blocks (their leases) in the file source's budget.
