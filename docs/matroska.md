# Matroska, WebM and AV1 (SDK API 17, 2026-11-05)

What changed: the engines read their files through `demux::Demuxer` (`include/media/demux/Demuxer.hpp`), one interface over
`IsobmffParser` (MP4, MOV, fragmented MP4) and the new `MatroskaDemuxer` (MKV, WebM). AV1 plays through Windows' own software
decoder, the one exception ENGINE_MANIFESTO.md now allows (section 2E). Opus plays in the windowed engine through Windows' Opus decoder.

## The demuxer interface

- `Demuxer` holds both demuxers inside it: no allocation, no virtual call. Its first `Step` reads the file's first bytes. An
  EBML header means Matroska; anything else goes to `IsobmffParser`, which refuses what is not ISO BMFF.
- Both demuxers keep the same contract: packets that lease their bytes from the block source (zero copy), playback epochs,
  seeks by table lookup with no I/O, forward continuation, track switches, sample lookups (`SampleAt`, `SyncSample`,
  `DecodeDistance`), and chapters.
- `PlaybackCoordinator` and `HostEngine` changed in two lines each (the member's type). Every call they make is forwarded.

## How `MatroskaDemuxer` reads a file

1. **Metadata.** It reads the EBML header and DocType (`matroska` or `webm`). Then it reads the segment's top-level elements up
   to the first Cluster: Info, Tracks and Chapters, each read whole into one view and parsed by `MatroskaReader`. Every other
   element is skipped by its size. `MetadataReady()` is set at this point.
2. **The Cues (2026-11-05).** The SeekHead names where the Cues element is. When it lies after the first cluster (ffmpeg and
   mkvmerge write it at the end), the demuxer reads it there before scanning: one read of the SeekHead and one of the Cues, not
   the file. On the test file that is 65,678 bytes of 657,424. It keeps each cue point's time and cluster position for the
   video track (up to 65,536 points), and `Seekable()` is true from then on.
3. **The index.** It scans the clusters in order, in windows of up to 1 MiB, and indexes every frame of every usable track in
   file order:
   - SimpleBlock and BlockGroup, with the group's ReferenceBlock (key frames) and BlockDuration (subtitle durations);
   - Xiph, EBML and fixed-size lacing, one entry a frame;
   - clusters of unknown size, which end at the next top-level element (the files a browser's MediaRecorder writes);
   - a cut or damaged tail ends the index, and what came before plays.

   Each entry is 32 bytes from the index pool (`ParserLimits::index_bytes`), plus 4 bytes a sample for the per-track lists once
   the scan is complete. The default 48 MiB pool holds about 1.4 million frames, more than a 3-hour 60 fps film with its sound.
4. **Packets during the scan.** The scan stays at least 64 entries ahead of the next packet. Between packets it reads whenever
   its own read is not in flight, so the index completes while the pictures play. A network source (`ParserLimits::scan_ahead`
   false) reads 64 KiB windows and no further ahead than playback needs. `Indexed()` turns true when the scan is complete.
   `Seekable()` is true with the Cues or once indexed.
5. **A seek before the index is complete** goes to the last cue point at or before the target. The scan starts again at that
   cue's cluster, and the video track waits for its first key frame. On the test file, a seek to 2.5 s right after open lands on
   the key frame at 2.000 s and reads 262 KB of the 657 KB file for its first 20 packets, over a source that reads only on demand. The
   lookups that need every sample (`SampleAt`, `SyncSample`, `DecodeDistance`: the engine's timeline, scene marks and
   intra-GOP continuation) wait for the complete index. A file without Cues (a live WebM: unknown-size clusters) is indexed as
   it is read, and is seekable when its scan ends.

Two faults this fixed (2026-11-05). A track's cursor was resolved only when a packet was due, so playback waited for the
whole scan after open and after a cue seek. And a block's header was read from its payload's offset: when a 64 KiB window
ended inside a block, the next window started after the element header, which the next step reads first, so every window was
read twice (905 KB read for a 657 KB file).

Times:

- Matroska stores presentation times only, in TimecodeScale ticks (usually 1 ms). Packets carry dts = pts.
- **Video** durations are the gap to the next presentation time, found among the 16 frames decoded before and the 16 after
  (B-pictures present out of decode order, and the picture after a B-picture is often decoded before it). DefaultDuration is
  used only for the last picture. Before 2026-11-09 DefaultDuration came first; see "Seeks with B-pictures" below.
- **Audio** times are regenerated so the engine's timing check (stage 29) holds: AAC units are exactly 1024 samples and Opus
  packets last what their TOC byte says. Each unit's time is the run's first block time plus n × frames / rate, exact in
  100 ns, as the MP4 tables give it. A block more than 2 ms from its regenerated time starts a new run.
- Opus times have CodecDelay subtracted, as the specification says.

Refused tracks keep a reason in `TrackInfo::refusal`: VP8/VP9, Vorbis, FLAC, AC-3, PGS, and frames with ContentEncodings
(compression or encryption). Their blocks are never indexed, and the other tracks play.

Not read: Tags, Attachments (fonts), BlockAdditions, and editions after the first. Cues are read only for the video track.

## Measured

`media_matroska_demux_tests` runs on ffmpeg's files, and its counts were checked against ffprobe:

| File | Result |
|---|---|
| `h264_aac_two_audio.mkv` | 120 pictures, 189 AAC units, each one exactly n x 1024 / 48 kHz from the first. Every B-picture's duration is 33-34 ms. A seek to 2 s lands on the key frame at or before it, and the audio on the unit at it. A switch at 1 s to the other AAC track. Every lease returned. |
| `vp9_opus.webm` | VP9 refused (Unsupported), 151 Opus packets of 20 ms. |
| Corruption sweep | Every 7th byte of the first 64 KiB flipped and every 97th cut, about 10,000 files. Each ends in Done or Failed, under ASan/UBSan (GCC) too. |

The windowed engine's pacing run (`--test-pacing`: play to 6 s, pause 4 s, play to 14 s) on `tools/realworld/make_mkv_fixtures.py`'s
files reports 0 dropped and 0 allocations for each, with the sound decoded. These are CTest's `media_fixture_mkv_*`,
`media_fixture_webm_*` and `media_fixture_av1_*` tests.

| File | Pictures | Dropped | Notes |
|---|---:|---:|---|
| H.264 + AAC + SRT + chapters (.mkv) | 180 | 0 | 3 chapters ("Opening", "Middle part", "Finale"); 3 cues imported |
| H.264 + AAC + ASS (.mkv) | 180 | 0 | 2 cues, override tags dropped |
| HEVC Main / Main 10 + AAC (.mkv) | 180 | 0 | |
| H.264 + Opus (.mkv) | 180 | 0 | Opus through Windows' decoder |
| AV1 + Opus (.webm), and a live-style one (unknown-size clusters) | 180 | 0 | |
| AV1 + AAC (.mkv) | 180 | 0 | |
| Big Buck Bunny 4K60 HEVC, stream copy to .mkv (1.43 GB) | 599 | 0 | Index scan 0.46 s from the system cache (3.1 GB/s), before the timeline. First picture 29 ms after Run. |
| `av_sync_clock_45m` 4K60 AV1, .mp4 and stream copy to .mkv | 588-591 | 0 | AV1 on the CPU (below) |

**The cost of a complete index.** A Matroska file is indexed by reading it once. That is 0.46 s for 1.4 GB from the cache. A
cold disk runs at its own speed (an HDD at about 150 MB/s would take about 10 s). The timeline preparation waits at most 5 s.
A seek no longer waits for it when the file has Cues (above). The timeline's scene marks and the intra-GOP continuation still
wait for the complete index.

## Seeks with B-pictures, and over HTTP (2026-11-09)

**The out-of-order picture.** `--test-seek` on `h264_aac_srt_chapters.mkv` reported one picture out of order after the forward
seek to 1.7 s. MP4 seeks were clean. A new diagnostic line (`Last out-of-order:`, from `D3D12Renderer::LastOrderReject`)
named it: the picture at 1.667 s, rejected as late after the landing at 1.700 s had left the reorder window.

The cause was the times, not the decoder or the flush. Matroska stores times in TimecodeScale ticks (1 ms), so the picture at
1.6667 s is stored as 1.667 s. With DefaultDuration (33.333 ms) it "lasts" until 1.7003 s and covers 1.700 s, the next picture's
time. The engine's seek gate shows every picture whose interval reaches the target, so it kept both. The P picture at 1.700 s
is decoded first and lands; the B picture at 1.667 s arrives after it. The backward seek showed the same fault in another way:
it landed on 0.567 s for a target of 0.600 s (its check passed, since 0.567 + 0.0333 > 0.6).

The fix gives each video frame the gap to the next presentation time as its duration. The gaps partition the timeline
exactly, as the MP4 tables do. Looking only at the frames decoded after it was not enough: in I P B B the second B's
successor is the P, decoded before it, so the duration became 67 ms and the fault stayed. Each entry now keeps its gap
(`Entry::gap`). This is filled as the frames are indexed, from the 16 frames before it and the 16 after it in decode order.
Results: out of order 0, both landings on the exact picture (1.700 s and 0.600 s), on H.264 and HEVC, from the file and over
HTTP. CTest: `media_fixture_mkv_bframes_seek_test`, `media_fixture_mkv_bframes_http_seek_test`, `media_fixture_mkv_hevc_bframes_seek_test`.

**End to end over HTTP** (`media_http_mkv_seek_test`). The file is 10 minutes of 720p30 H.264 with B-pictures (`-bf 3`, 2 s GOP),
198 MB, with its Cues at the end as ffmpeg writes them (`make_mkv_fixtures.py`). The loopback server answers 401 to any GET
without `Authorization: Bearer loopback-token`; the engine sends it with an extra header through `PlaybackOptions::http_headers`
(what `media_engine_open_ex` passes). The test uses the engine's default 8 MiB chunks. Script: 400.5 s and back to 120.25 s while
playing, 530.4 s while paused, 121.7 s, then 595.3 s, played to the end.

| Measure | Result |
|---|---|
| Landings | 5 of 5 exact, each with its sound within one 48 kHz sample; 26-30 ms from request to first picture after a cue seek, 50-85 ms to the clock running again |
| Pictures | 0 dropped, 0 out of order, 0 allocations |
| Network | 0 unauthorized, 0 failures; 11 GETs, 69 MB of 198 MB (35 %). The Cues: one 8.5 KB range at the end of the file |
| Timeline | 600.021 s from the start (the segment's Duration), before any index |

Every seek here is a cue seek, because the index covers only what was read. A 2 s GOP means the decoder starts at most 2 s
before the target, at the cue's cluster.

## AV1 on the CPU (ENGINE_MANIFESTO.md 2E)

The machine's GPU (Intel UHD 630, Gen9.5) has no AV1 decoder. The engine then uses the operating system's software decoder,
the AV1 Video Extension's Media Foundation transform (`platform::windows::Win32VideoMftDecoder`). Nothing is bundled.

- Prepared on the worker (where COM and Media Foundation already run) with 16 input samples of the largest access unit. The
  transform's decode threads keep about ten units before the first picture, and they hold the units' buffers, not the samples.
  A slot is reused only when the transform holds neither.
- Its threads are capped at half the logical processors. An uncapped burst (it decodes ahead while surfaces are free) starved
  the presentation thread once in early runs: one picture dropped at 6 s.
- Each picture's NV12 or P010 planes are copied once (one `memcpy` per plane when the pitches match) into one of four upload
  buffers, then onto the GPU on the copy queue, into `D3D12VideoDecoder`'s own surfaces (`VideoCodec::Upload`). The copy
  signals the decode fence, so frames carry the same descriptors, leases and fences as hardware decoding, and the renderer is
  unchanged.
- Pictures never shown (before a seek target) are not copied. Seek continuation, queue seeks and scrub previews keep a hardware
  decoder's chain, so they are off for AV1. Seeks flush the transform.
- `media_query_video_decoder` and the demo's `Stream:` line say it is the platform's software decoder.

Measured on the 45-minute 4K60 clock (i7-10700, UHD 630, Release):

- The transform alone (`tools/av1_mft_probe.cpp`, media_av1_mft_probe) decodes 150 pictures a second, 6.6 ms each.
- Playing: 585-599 of the run's pictures shown, 8 pictures decoded before the clock starts (as for network sources).
- **Drop-free since 2026-11-05: 12 of 12 runs of the .mp4 and 10 of 10 of the .mkv, 0 dropped.** On the morning of 2026-11-05
  the .mp4 dropped one or two pictures in 7 of 10 runs. The decoder was not behind: the pictures were queued 100 ms before their
  deadlines. The start-up trace found three causes:
  1. *Priority.* The transform's 8 threads run flat out at normal priority, and the paced presentation thread woke up to 17 ms
     late behind them ("waited for nothing"). While this path plays, the presentation thread runs at THREAD_PRIORITY_HIGHEST and
     the worker above normal. The caller's priority is restored when `Run` returns.
  2. *The pause.* The worker read the pause command only after up to four 4K uploads (3-4 ms each), so the sound stopped up to
     31 ms after the picture froze (measured 0.01-31 ms), and at the resume the clock was that far ahead of the next picture,
     which was dropped as late. The upload loop now stops when a control command is waiting (pause reaction at most 7 ms).
  3. *Controls-only images before a picture that is not yet timed.* At the start and at a resume, a picture is in order before
     the clock times it. The rule that keeps a controls-only image away from a due picture did not apply. The start's status
     cue and the resume's undimmed controls were drawn just before, and the picture's Present waited for frame latency
     ("missed its deadline; waited for frame latency"). After the start and after a resume, until the first timed picture,
     a picture waiting for the clock now counts as due (for at most 250 ms). Not at a seek's landing: applied there too, it
     held back the drawing that the next conversions follow.
- Process CPU about 400 % (the transform's threads); the worker thread 55-62 % of a core (the transform's calls and the copies).
- Both seeks of `--test-seek` exact (1.700 s and 0.600 s), 260-270 ms each.

On a GPU with AV1 decoding, the engine still uses this path: a D3D12 AV1 hardware path (OBU parsing into DXVA AV1 picture
parameters) is not implemented.
