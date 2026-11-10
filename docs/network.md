# Network streaming: `HttpBlockSource`

`platform::windows::HttpBlockSource` implements `IBlockSource` over HTTP/HTTPS byte ranges with asynchronous WinHTTP. `PlaybackCoordinator::Prepare` accepts an `http://` or `https://` URL wherever it accepts a file path. The demuxer, codecs, seeking, scrubbing and audio pre-roll are unchanged: they see the same lease contract as the IOCP file source.

## Design

**Storage.** At `Open`, the arena provides the only payload storage:
- a fixed number of **chunk** buffers (in the engine, 16 × 8 MiB: the file source's 128 MiB budget and its largest admissible sample);
- a fixed lease table.

`WinHttpReadData` writes each response body directly into its chunk buffer. Nothing is staged and nothing is copied afterwards.

**Leases alias chunks.** `Request(offset, bytes)` returns a lease on the live chunk that covers the range, or starts a new one. The interface already allows this: "Ready views stay immutable until Release, including aliases". The demuxer takes one lease per sample and hands it downstream with the packet, so many consecutive samples share one chunk and one GET.

**Progressive readiness.** Each chunk publishes a received-byte watermark with release stores. Bytes below the watermark are final: a retry resumes above it and never rewrites them. A lease is `Ready` as soon as the watermark covers it, not when its chunk is complete. A sample is therefore usable one round trip plus its own transfer time after it is requested, which is what makes seeking over a network fast.

**Read-ahead.** Each request keeps `prefetch_chunks` (2) chunks queued ahead of the chunk it touched once the reader reads on through the object (it asked inside a read-ahead chunk). Behind a chunk fetched on demand (the open, a seek, the Cues at the end of a Matroska file) it keeps one (2026-11-09): the second follows as soon as the reader enters the first. On `media_http_mkv_seek_test` (five seeks in a 198 MB file) that is 11 GETs and 69 MB instead of 14 GETs and 102 MB, with the same seek latencies.
- Consecutive read-ahead chunks overlap by `overlap_bytes` (1/8 of a chunk), so a sample that crosses a boundary usually lies wholly inside the next chunk.
- A miss that would duplicate a live chunk starting inside its span fetches only up to that chunk, or exactly the missed range if it straddles.
- Read-ahead only reuses idle chunks older than the one being read, so it never evicts what is in use.

**Seeking.** A seek only moves the demuxer's table cursor. The next `Request` names the sync sample's offset, which is either already cached or fetched from exactly there. Nothing before the offset is downloaded; the unit test checks this.

**Integrity.** The probe and every GET demand `206 Partial Content`, with a `Content-Range` that names exactly the requested bytes of an object of the probed length. A `200` (ranges ignored), a different range or a changed length is rejected instead of mixing unrelated bytes into a chunk. A strong `ETag` recorded by the probe is sent as `If-Match`, so an object replaced mid-stream yields `412` and a terminal error.

**Resilience.**

| Condition | Result |
|---|---|
| Transport failure: reset, truncated body, timeout | The fetch resumes from its watermark with a new range, up to `retries` (2) times, including transient `5xx` responses. |
| `412` or a malformed partial response | Terminal: never retried. |
| Terminal failure | `Poll` returns `OsError`; the coordinator fails the stream (stage 18). |
| Slow arrival | Always non-blocking: `Poll` returns `Pending`. |
| Pool exhausted | `Request` returns `WouldBlock` (every lease or chunk in use), `TooLarge` (a range longer than a chunk) or `OutOfRange`. The pools never grow. |

**Open.** `Open` is setup-time and blocking, bounded by the timeout. It parses the URL; the query string, such as a presigned signature, stays part of the request target. It then opens an asynchronous session with timeouts and a per-server connection limit, and issues a one-byte range GET. That probe proves range support and returns the length and validator in one round trip. It rejects missing objects (`404`) and servers without range support.

## Threads and lifetime

- **Owner.** One thread (the media worker) calls `Open`, `Request`, `Poll`, `Release`, `Pump` and `Close`. It never blocks or locks.
- **Callbacks.** WinHTTP completion callbacks run on WinHTTP threads and touch only the chunk their request belongs to: SEND → RECEIVE → headers validated → read loop.
  - They publish the watermark and, at the handle's closing notification, the final state.
  - The first finisher (completion, failure or owner cancellation) closes the request handle exactly once, arbitrated by an atomic exchange.
  - The closing notification is WinHTTP's last for that handle. Only after it can the owner reuse the chunk, because until then WinHTTP may still write into it.
- **Cancellation.** `BeginCancel` closes in-flight handles. The destructor waits for their closing notifications before closing the connection and session.
- **Release.** Releasing a pending lease only drops interest. The chunk keeps fetching into its own storage and stays cached.
- **Allocation.** The engine performs no allocation after `Open`. WinHTTP allocates a native request handle per GET, on the OS heap outside the application heap: the same boundary as the D3D12 and Media Foundation objects.
  - The allocation probes count process-wide `operator new` and, in Debug, the CRT heap hook. They report 0 in every networked run.
  - A Debug-only count in an early run came from the test's own `printf`: the stdio buffer is allocated on first use while stdout is redirected. The print was moved outside the measured window.

## Test server

`tests/loopback_http_server.hpp` is an HTTP/1.1 range server on `127.0.0.1` (Winsock, a single `select` thread, 32 fixed connection slots). It serves from caller memory and allocates nothing after `Start`, so it runs inside allocation-probed processes.
- **Protocol:** `GET` and `HEAD`, single ranges, `416`, keep-alive, strong `ETag` and `If-Match`.
- **Adjustable at run time:** per-response latency, a per-connection bandwidth cap, range support switched off, dropped connections after N body bytes, `503` responses, and object mutation (a new ETag).

`media_player_demo file.mp4 [--test…] --http` serves the file through it and plays the URL. `--http-slow` adds 15 ms per response and a 50 Mbit/s cap. The fixtures are only 0.27–2.7 MB, so `--http` runs use chunks of about half the file: this forces several ranged GETs and read-ahead, while chunks stay larger than a 4K IRAP picture.

## Measured results

Release build, loopback, Intel UHD 630. Seek latency is measured from the request to the target frame on screen, as in [seeking](seeking.md).

| Stream over HTTP | Profile | GETs / bytes for file | Seek forward (playing) | Seek backward (paused) | Landing, audio start |
|---|---|---|---|---|---|
| H.264 (0.27 MB) | loopback | 2 / 307 KB for 274 KB | 1.6 ms | 1.8 ms | exact, exact |
| HEVC 720p (0.85 MB) | loopback | 2 / 918 KB for 852 KB | 20.2 ms | 16.4 ms | exact, exact |
| HEVC 720p | 15 ms, 50 Mbit/s | 2 / 918 KB | 19.1 ms | 15.8 ms | exact, exact |
| HEVC Main10 4K (2.7 MB) | loopback | 3 / 4.19 MB for 2.75 MB | 157 ms | 108 ms | exact, exact |
| HEVC Main10 4K | 15 ms, 50 Mbit/s | 3 / 4.19 MB | 155 ms | 108 ms | exact, exact |

**Scrubbing** over the 15 ms / 50 Mbit/s profile, 93 requests at 120 Hz:

| Stream | Frames shown while dragging | Longest gap | Last request → final frame | Burst of four |
|---|---|---|---|---|
| HEVC 720p | 59 | 26 ms | 7.1 ms | 3 superseded, exact |
| HEVC 4K | 7 | 239 ms | 76 ms | 3 superseded, exact |

Every run had zero dropped or out-of-order frames, zero failures, zero retries and zero allocations.

- **Why seek latency matches local playback.** These fixtures are small enough that read-ahead had usually fetched the seek target already. The cold-seek network cost is characterised by `media_http_source_tests` instead: with 60 ms per response, a far request became ready after 62 ms (one round trip), and the owner's `Poll` returned `Pending` in 0.1–1.3 µs while it waited.
- **Why 4K downloads 1.5× the file.** Its IRAP pictures exceed the 256 KiB test overlap (1/8 of a 2 MiB chunk). An IRAP that straddles a read-ahead boundary is fetched again as its own range. The engine default (8 MiB chunks, 1 MiB overlap) makes such straddles rare; the playback tests bound total download at 2× the file.

## Verification

| Test | Checks |
|---|---|
| `media_http_source_tests` | Against the loopback server, with every streaming phase under the allocation probe (0 allocations). **Correctness:** sequential sample-sized reads over a 3 MB object are byte-exact, served by 15 GETs (13 read-ahead) with 128 of 130 requests hitting cache and 1.15× bytes; aliasing leases share one buffer at their relative offsets; bounds give `TooLarge`, `OutOfRange`, `InvalidArgument`, and `WouldBlock` once the lease pool is exhausted. **Network behaviour:** a far first request downloads one chunk from its offset and nothing before it; `Poll` stays non-blocking under latency; under an 80 KB/s cap, the first bytes are ready while the chunk is still arriving. **Failures:** a mid-body drop resumes with one retry; a 503 recovers; exhausted retries give a terminal `OsError` after exactly 1 + `retries` GETs; a mutated object gives 412 with no retry. **Lifetime:** cancellation aborts a 3 s response in well under 1 s; `Open` rejects 404, a server that ignores ranges (200) and non-HTTP schemes. |
| `media_playback_http_test`, `media_playback_http_seek_test` | H.264 full playback and seeking over loopback HTTP. |
| `media_playback_hevc_http_seek_test`, `media_playback_hevc_http_scrub_test` | HEVC 720p seeking (loopback) and scrubbing (15 ms, 50 Mbit/s). |
| `media_playback_hevc10_4k_http_seek_test` | 4K Main10 seeking over 15 ms, 50 Mbit/s. |

Each playback test also requires: several ranged GETs, no failures, at most 2× the file downloaded, exact landings and audio starts, no drops and zero allocations.

## Limits

- **Platform:** Windows only (WinHTTP). A POSIX implementation of the same contract would need its own HTTP client.
- **One request per range:** WinHTTP has no request pipelining and needs a native handle per GET. Parallelism comes from read-ahead over several keep-alive connections (8 per server).
- **Not exercised:** HTTPS (`WINHTTP_FLAG_SECURE`) and redirects are handled by WinHTTP but not tested here, since the loopback server is plain HTTP. Proxies follow the system WinHTTP proxy configuration.
- **Largest sample:** one chunk (8 MiB in the engine). Larger samples fail at demux preparation with `LimitExceeded`.
- **Cache policy:** least-recently-used over idle chunks, with no bandwidth adaptation. A slow link cannot make read-ahead deeper than `prefetch_chunks`, and read-ahead already issued continues after a seek (bounded by `prefetch_chunks` chunks).
- **Fragmented MP4:** still neither seekable nor supported for remote progressive parsing beyond what the demuxer already does.
