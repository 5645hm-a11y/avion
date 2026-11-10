# Layer 1: portable contracts, native asynchronous I/O, and bounded MP4 demuxing

Implemented for the updated cross-platform milestone, researched 2026-09-24. The user's cross-platform update supersedes the Windows-only wording in `ENGINE_MANIFESTO.md`; its allocation, ownership, dependency, and ordering constraints remain in force.

## Platform boundary

| Module | Contract / implementation |
| --- | --- |
| `include/media/core/` | Standard C++20 only. Arena bookkeeping, atomics, descriptors, and placement construction contain no Windows types or headers. |
| `include/media/demux/`, `src/demux/` | Portable checked byte parsing, a compact pooled sample index, time conversion, and packet publication. |
| `platform/SystemMemory.hpp` | Startup/teardown free-function boundary. Native mappings are linked from Windows `VirtualAlloc` or POSIX `mmap` implementations. Arena page touching remains in portable C++. |
| `platform/IBlockSource.hpp` | Immutable, known-length range source. Request/Poll/Release transport generation-checked block leases, offsets, and byte counts. |
| `platform/IAudioSink.hpp` | Precreated endpoint write-span checkout/commit and hardware-position/monotonic-clock correlation. No WASAPI/CoreAudio/ALSA handles in the contract. |
| `platform/IGpuContext.hpp` | Prepared kernel/surface handles, compute dependencies, presentation, and timeline completion. No D3D12/Vulkan/Metal types in the contract. |
| `platform/windows/Win32IocpBlockSource` | Implemented IOCP file provider, owning fixed arena blocks and stable OVERLAPPED records. |

Interfaces allocate no storage and do not own hidden worker pools. Virtual dispatch is one indirect call per operation; it is not literally zero CPU instructions. Parsing inner loops are concrete functions over contiguous records. Memory backend calls happen only before sealing or during teardown.

The POSIX memory backend makes the portable code executable on Linux and supports the same native mapping model on Apple/Android targets. It is not an implementation of `io_uring`, kqueue, audio, or GPU backends. Those can implement the existing contracts without changing the demuxer. Lock-free 64-bit atomics remain a compile-time requirement of the queue. Its isolation model is 64-byte cache lines, not a claim about every CPU's cache geometry.

## Buffered I/O, unbuffered IOCP, and DirectStorage

IOCP is completion delivery, not a caching policy. Both buffered and unbuffered files can use OVERLAPPED reads. Buffered reads can be preferable for small ranges or repeated access because the system cache and read-ahead amortize device work. Unbuffered reads give the application responsibility for buffer management and bypass the normal file-data cache; that does not prove lower latency for every workload or eliminate device/metadata caches. Offsets and transfer lengths must meet sector requirements, and buffer addresses should meet physical-sector alignment. [Windows file buffering](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering)

The backend queries `FileStorageInfo`, validates a power-of-two logical sector size, and uses the maximum reported logical/physical/effective sector size for buffer, offset, and transfer alignment. Unbuffered block capacity must be a multiple of that alignment. Unsupported queries fail Open; there is no silent cached fallback. The caller can explicitly request buffered mode. [FILE_STORAGE_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_storage_info)

Windows DirectStorage is a different API with its own runtime/distribution model and optimized request path; it is not equivalent to calling IOCP with NO_BUFFERING. BypassIO also depends on filesystem/filter/storage support. This milestone links neither DirectStorage nor its SDK and does not claim that IOCP automatically enables BypassIO. The native Win32 backend preserves the current dependency constraint. [DirectStorage overview](https://learn.microsoft.com/en-us/windows/win32/dstorage/dstorage-portal), [BypassIO architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/bypassio)

The actual guarantee is that the application gives `ReadFile` its final arena destination and performs no staging-buffer copy after completion. Whether the kernel/device uses direct DMA, a filter, bounce storage, or internal copies is not controlled by this API. OVERLAPPED operations can still spend time in submission, and IOCP uses OS-managed synchronization internally. Zero-timeout polling removes an intentional completion wait from our streaming loop; it does not remove syscalls or scheduler preemption. [ReadFile lifetime and asynchronous behavior](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)

## Source state machine and ownership

`Open` runs before sealing. It opens an immutable file handle with read sharing only, obtains size/alignment, creates one completion port, and allocates all request records and fixed-capacity payload blocks from the arena. Partial setup failure closes native handles; monotonic arena reservations remain charged until arena shutdown. Do not retry indefinitely in a nearly full arena.

```text
Free --Request--> Pending --completion--> Ready --Release--> Free
                      |                       |
                      +--failed completion--> Failed --Release--> Free
                      |
                      +--Release (abandon)--> wait for completion --> Free
```

Each accepted request receives `{id, generation}`. The generation sequence survives Close/Open and refuses further requests at exhaustion instead of wrapping. Old/double-release tokens fail validation. A record and its bytes are never reused before completion has been dequeued. Synchronous `ReadFile` success also waits for its IOCP notification; skip-completion-on-success mode is not enabled.

`Poll(token)` dequeues at most one completion; `Pump(budget)` dequeues at most `budget`. Both use a zero timeout. Other requests can complete first; their records retain the result until their owner polls them. FALSE plus non-null OVERLAPPED is a failed request completion, while null OVERLAPPED plus WAIT_TIMEOUT means no completion. [GetQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)

`BeginCancel` requests cancellation without waiting. Releasing an outstanding token abandons its result but keeps kernel-visible storage alive. `Close` returns WouldBlock while operations or completed leases remain. The destructor cancels and drains outstanding kernel operations; this blocking cleanup belongs to teardown, never Running. Cancellation is not completion, and OVERLAPPED/buffer memory must remain valid until retirement. [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)

One owner invokes source/parser methods. A downstream decoder may read a published block concurrently, but returns its token through a separate SPSC return queue; only the owner calls `ReturnPacket`/`Release`. Destroy in this order: stop consumers and return their leases, destroy the parser, close/destroy the source, then release the arena. Parser destruction abandons only its own outstanding request, never downstream packets.

## Resolving samples that cross arbitrary chunk boundaries

For a sample at absolute offset `s`, length `n`, sector alignment `a`, and fixed block capacity `B`, choose:

`read_start = floor(s/a)*a`

`prefix = s-read_start`

The request is admissible when `prefix+n <= B`. The provider reports a conservative guaranteed maximum `B-(a-1)`. Reads fill fixed blocks with sector-aligned read-ahead; the final read can be short at EOF. The parser verifies the returned view covers the entire requested interval and rejects short/mismatched responses without retrying forever.

This is a **range-window provider**, not a partition of the file into immutable non-overlapping 1 MiB chunks. A sample spanning an arbitrary chunk boundary is read into one final block window. There is no assembly copy. Samples larger than the guaranteed range limit fail with LimitExceeded; supporting arbitrarily large samples would require larger prepared blocks or a scatter/gather transport contract.

The parser has one outstanding range lease and emits one packet lease per sample. Since 2026-09-30 the file source shares its blocks between leases: a request whose range lies inside a block already read (or being read) takes a lease on that block without I/O, and only a range outside every block reads a new one, reusing the least recently used block that nobody holds. Before that change every sample re-read a whole block from its own offset (8 MiB per sample in the engine's configuration, about 2 ms each from the page cache), which is what made seeks on long-GOP files slow; see [seeking](seeking.md). Blocks nobody holds keep their bytes, so the block set is also a read cache (128 MiB in the engine). Zero application payload copies is established. Not done: reading the next block ahead of need (a block boundary waits for one read), and pipelining several reads.

## Parser machine and grammar

`Step(queue)` performs one structural box operation, one container close/finalization, or one sample emission. States are Scan, Validate, Emit, Done, and Failed, with NeedInput and Backpressure as resumable results. A cached header state prevents re-reading a header when a pending request targets its body. There is no recursion or AST. A 16-entry stack stores parent end offset, type, and track index.

Every box validates `header_size <= size <= parent_end-cursor` before forming its end. Standard 32-bit and extended 64-bit sizes are supported. Size zero is allowed only at top level and means the known object's remaining bytes. Unknown boxes are skipped by their validated extent. Boxes that alter supported sample interpretation (compact sizes, encryption auxiliaries, sample groups other than audio roll recovery, and edit lists other than a single rate-1 entry) reject explicitly. Audio roll recovery (`sgpd`/`sbgp` of type `roll` or `prol` on a sound track, written by common muxers for every AAC track) is admitted: it changes no sample, its distance is reported in `TrackInfo::roll_distance`, and seeks pre-roll audio for every target alike. A video sample entry's `colr` (nclx/nclc), `clli` and `mdcv` are read into `TrackInfo::color`. [Layer 5](layer5.md) admits the single-entry edit list as a timestamp shift.

| Box | Implemented treatment |
| --- | --- |
| `ftyp` | Validate size/alignment and uniqueness. Brands are informational; implemented structures determine admission. |
| `moov`, `trak`, `mdia`, `minf`, `stbl` | Check parent grammar and walk with the fixed stack. A track consumes one preallocated record set. |
| `mvhd` | Versions 0/1, nonzero movie timescale, minimum fixed layout. |
| `tkhd`, `mdhd`, `hdlr` | Versions 0/1 where applicable, unique nonzero track IDs, media timescale, and audio/video handler. `tkhd`'s matrix: its linear part as `TrackInfo::transform` when it is a quarter turn or mirror of the axes (2026-10-28, below). |
| `dinf` / `dref` | If present, require one self-contained `url ` reference. External data references reject. Sample entry data-reference index must be 1. |
| `stsd` | One description per track; visual/audio fixed fields and bounded configuration child records. |
| `stts` | Positive run counts/durations, kept as runs with a random-access mark every 64 runs; exact sample coverage. |
| `ctts` | Versions 0/1, unsigned/signed offsets respectively; exact coverage. PTS need not be monotonic. |
| `stsc` | Strictly increasing 1-based first-chunk entries, positive samples-per-chunk, description index 1, exact coverage. |
| `stsz` | Constant size (nothing stored per sample) or explicit sizes; the index pool bounds the count. Zero-sized samples reject. |
| `stco` / `co64` | 32/64-bit absolute chunk offsets, mutually exclusive through duplicate detection. |
| `stss` | Strictly increasing valid 1-based sync IDs; absent means all samples are sync samples. |
| `mdat` | Record a bounded data extent and skip payload during structural scan. Every emitted sample must lie wholly inside an admitted extent. |
| `mvex` / `trex` | Store track-specific duration/size/flags defaults; resolve IDs at movie finalization. Fragment initialization requires empty classic sample tables. |
| `moof` / `mfhd` | Reset reusable per-fragment sample storage and require increasing fragment sequence numbers. |
| `traf` / `tfhd` | One traf per track per moof; resolve defaults, explicit base offsets, default-base-is-moof, or first-traf implicit moof base. |
| `tfdt` | Required version 0/1 decode anchor; reject backwards per-track decode time. |
| `trun` | Versions 0/1, signed data offsets, optional per-sample duration/size/flags/CTO, first-sample flags, and multiple runs with inherited run end. |

The fragmented admission profile follows the useful initialization/media-segment distinction in the W3C byte-stream specification: empty sample tables in initialization metadata, per-fragment timing/locations, and media-data extents. It is a bounded profile, not a claim of complete ISO/IEC 14496-12 or MSE conformance. [W3C ISO BMFF byte stream format](https://www.w3.org/TR/mse-byte-stream-format-isobmff/)

Classic files are structurally scanned through EOF before emission; this admits both fast-start and tail-`moov` files without retaining the entire `mdat`. Fragmented files emit after a completed moof and sufficient following mdat extents cover that fragment. Sample arrays are reused before the next fragment; total movie sample count can therefore exceed per-fragment capacity.

## Sample index (long-form files)

Classic sample tables are kept in the form the file stores them, never expanded to one record per sample. The index is carved from one pool that `Initialize` reserves in the arena (`ParserLimits::index_bytes`, default 48 MiB), so parsing a moov allocates nothing, even after the arena is sealed.

| Table | Stored as | Bytes |
| --- | --- | --- |
| `stts`, `ctts` | runs `{count, value}` plus a mark `{dts, first sample}` every 64 runs | 8 per run + 16 per 64 runs |
| `stsc` | runs `{first chunk, samples per chunk, first sample}` | 12 per run |
| `stco` / `co64` | one offset per chunk | 4 / 8 per chunk |
| `stsz` | one size per sample, or nothing for a fixed size | 4 per sample |
| `stss` | one sample number per sync sample | 4 per sync sample |

Typical files need 5-11 bytes per sample (the former flat records took 40, and capped each track at 131072 samples, about 46 minutes of 48 kHz AAC). The worst case is VFR video with per-sample `ctts` and one sample per chunk: about 24 bytes per video sample. The default pool holds a 4-hour 60 fps film with its audio at that worst case (measured 21.8 MiB, 14.9 B/sample). A file whose tables exceed the pool fails with `LimitExceeded`.

- **Reading tables.** A sample table box is read in windows of at most 1 MiB (and `MaxRangeBytes`) holding whole entries, so a table's size is bounded by the pool, not by the source's range limit. Other leaf boxes still must fit one range.
- **Validation.** After the scan, `Validate` walks every sample once (coverage by an `mdat`, range limit, timestamp conversion), 65536 samples per `Step`, so no step stalls the caller (longest measured: 3.5 ms for 1.5 million samples).
- **Emission.** A per-track cursor holds the sample's position in every table (run, remaining count, chunk, byte offset, next sync entry) and advances in O(1).
- **Random access.** `Seek` binary-searches the sync table; each probe positions a cursor by binary search over the marks, then at most 64 runs, and over the `stsc` runs, then sums the sizes before the sample in its chunk. Measured: 4-27 µs per seek on 1.5 million samples, without I/O or allocation.
- **Fragments.** Fragmented files have empty classic tables. At the end of the moov the pool is split evenly into per-track windows for one fragment's samples (40 bytes each), reused by every `moof`.

`tests/longform_demux_tests.cpp` synthesises 2 h, 3 h 30 min and 4 h files (up to 4.7 GiB, 64-bit offsets, VFR, B-frames) with real moov bytes and virtual payload. It checks every packet of two of them against an independent model, and every seek landing against a brute-force search. `media_playback_longform_test` plays a real 3 h 30 min fixture through the decoders, seeking to 1 h, 2 h, 45 min and 3 h 30 min.

“Single pass” means one forward structural traversal and one decode of admitted metadata records. It does **not** mean each physical byte is read exactly once. A tail-moov file and finite buffers require range access or unbounded retention to emit its earlier payload. The range abstraction makes that requirement explicit.

## Codec parameter extraction

`TrackInfo.configuration` and `parameters[]` contain absolute source ranges, not pointers into an evictable parser view. A decoder can request those ranges through the same source and retain their leases as needed.

- `avc1` / `avc3`: `avcC` record, NAL length field width, SPS/PPS, and supported high-profile SPS extension arrays. In-band parameter updates remain in the access units; no Annex-B rewriting occurs.
- `hvc1` / `hev1`: `hvcC` record, NAL length width, and bounded NAL arrays with their NAL unit types, including VPS/SPS/PPS.
- `av01`: `av1C` record and configuration OBU byte range. OBU/sequence syntax decoding belongs to the codec layer. [AV1 ISOBMFF binding](https://aomediacodec.github.io/av1-isobmff/)
- `mp4a`: bounded ES/DecoderConfig/DecoderSpecific descriptor parsing, MPEG-4 Audio indication, and the AudioSpecificConfig range. ASC semantic/profile validation remains the audio decoder's responsibility.
- `Opus`: `dOps` fields and channel-mapping bounds; retain the configuration record directly and report the 48 kHz decode rate. Pre-skip/output-gain interpretation belongs to the audio decoder. [Opus ISOBMFF encapsulation](https://opus-codec.org/docs/opus_in_isobmff.html)

Container validation is not validation of an entire H.264/HEVC/AV1/AAC/Opus elementary stream. No codec decoder or external codec library is linked.

## Sample positions, timestamps, and zero-copy proof

For chunk `c` with absolute offset `O_c`, map its sample count through the active `stsc` run. If samples `j..k` belong to that chunk, then `offset_j=O_c` and `offset_(i+1)=offset_i+size_i`. Every addition is checked before use; total mapped samples must equal `stsz` count. Chunk offsets need not be increasing across tracks. The familiar table relationships are documented by Apple; this implementation stores only decoded fixed records rather than a container object graph. [Sample atoms](https://developer.apple.com/documentation/quicktime-file-format/sample_atoms), [Sample-to-chunk table](https://developer.apple.com/documentation/quicktime-file-format/sample-to-chunk_atom/sample-to-chunk_table)

For classic samples, `DTS_0=0`, `DTS_(i+1)=DTS_i+duration_i`, and `PTS_i=DTS_i+CTO_i`. Fragments use tfdt as their decode anchor. tfhd/trex defaults apply unless a trun field overrides them; first-sample flags and per-sample flags cannot both be present. A negative version-1 CTO may produce negative preroll PTS. Output selection merges per-track DTS in 100 ns units, with stable track-index tie-breaking. Across independently timestamped fragments, only per-track nondecreasing decode time is guaranteed.

`To100ns` uses integer quotient and remainder:

`abs(ticks)=q*timescale+r`

`abs(time_100ns)=q*10,000,000+floor(r*10,000,000/timescale)`

The sign is then restored, giving truncation toward zero. The remainder product fits uint64 because the timescale is uint32. Check the quotient against the remaining INT64 range before multiplying. INT64_MIN remains reserved for unknown time. Packet duration is `rescale(DTS+duration)-rescale(DTS)`, so fractional rounding does not accumulate across adjacent packets.

For a ready block with immutable base pointer `p` and file origin `b`, sample `(s,n)` is validated with subtraction-first checks. Packet fields are:

`payload_block_id = block.id`

`payload_generation = block.generation`

`payload_offset = s-b`

`payload_bytes = n`

Consequently `p + payload_offset` addresses the same destination bytes supplied to the source operation. No payload `memcpy`, intermediate byte vector, NAL rewriting, or reassembly occurs. Numeric metadata is decoded into flat prepared tables; the 64-byte descriptor is assigned into a checked-out SPSC slot. A release commit publishes it. The parser relinquishes its block lease only after a successful commit; full output retains that lease and retries without rereading or duplicating a packet. The consumer later returns the generation-checked token.

This proves zero **application payload movement**, not zero metadata stores, no device reads, or zero kernel/driver copies. The test provider checks pointer identity against the fixture's original bytes; native integration checks exact offset/length/content against the IOCP-owned arena block.

## Admission and bounded failure

Defaults: 8 tracks, 65,536 samples per track (or per fragment), 16,384 entries per table per track, 64 parameter ranges per description, 16 container stack entries, and 64 mdat extents per classic file/current fragment. Limits are selected and all storage is allocated before sealing. The memory cost is the track count multiplied by its sample array plus five flat row arrays; there are no allocations during parsing or emission.

Recognized leaf box bodies must fit `MaxRangeBytes()` so they can be parsed directly from one immutable window. This explicitly limits large stsd/stsz/stco/stts/etc. leaves independently of row counts. Larger blocks/limits must be prepared beforehand. `Step` work is bounded by admitted table/sample sizes, not necessarily constant-time: movie finalization expands tables, and fragment admission validates its samples against bounded extents.

Unsupported interpretation-changing features fail instead of being ignored: edit lists other than one rate-1 entry with non-negative media time (Layer 5), encrypted entries/auxiliary encryption records, sample groups (except audio roll recovery), compact sample sizes, multiple descriptions per track, non-audio/video handlers, QuickTime audio entry versions 1/2, mixed nonempty classic and fragmented tables, and implicit base inheritance for subsequent trafs. Empty/zero-duration access units are also outside this profile. A real-world file using these legal features may reject; the parser is not advertised as a universal MP4 compatibility replacement.

Every failure records error category, current absolute box offset, and FourCC, drops/abandons the parser-owned lease, and becomes terminal. Previously published packets remain valid until returned. A new seek/session requires draining and constructing a new parser, with a new playback epoch. Done is the explicit end-of-stream signal; this layer does not synthesize an extra EOS MediaPacket.

## Future network providers

Implement the same Request/Poll/Release contract with final fixed arena destinations, immutable object identity (for example an HTTP validator), bounded in-flight requests, and out-of-order completion records. The parser does not know whether its bytes came from a file, a network response, or a synthetic provider. A cloud provider must honor range coverage and retain its final destination until lease return; it must not reuse an HTTP receive buffer behind a published packet.

The current contract requires a known object length and range access. It directly supports immutable cloud objects and finite collections of initialization/media fragments. Unbounded live input or a push-only source needs an explicit growing-extent/segment-session contract; this implementation does not hide that limitation. No Google Drive authentication, HTTP client, or network backend is included in the current milestone.

## The track's orientation: tkhd's matrix (2026-10-28)

Phones, drones and action cameras record in the sensor's orientation and say how to show it in the video track's `tkhd` transformation matrix (ISO/IEC 14496-12: x' = a x + c y + u, y' = b x + d y + v, a, b, c, d in 16.16 fixed point, at bytes 40-75 of a version-0 payload, 52-87 of version 1). The parser keeps its linear part as `TrackInfo::transform` (a, b, c, d) when each term is 0 or exactly +-1 and it is a quarter turn or mirror of the axes: the identity, 90 degrees clockwise (0, 1, -1, 0: what phones write for a portrait recording), 180 (-1, 0, 0, -1), 270 (0, -1, 1, 0), and the mirrors. Anything else (a free angle, a scale) is kept as the identity with `transform_ignored` set; an all-zero matrix, which many muxers leave, is no matrix (the identity, not ignored). The translation terms are not used: a turned picture is placed by its own bounds. The playback engine shows the picture this way round from its first frame and shapes its window by it ([picture](picture.md#rotation-2026-10-28)).

`media_rotation_tests` (portable; MSVC and GCC -O2, ASan/UBSan) parses in-memory files with eight matrices (none, identity, the three turns, a mirror, 30 degrees, a 2x scale): 8 of 8 read as expected, without allocation while parsing.

## Verification

Three Windows suites cover the original Layer 0 contracts, portable demuxing, and real buffered/unbuffered IOCP integration. MSVC Debug and Release use `/W4 /WX /permissive-`. Fixture coverage includes all five codec descriptions, classic stco/co64, signed CTS offsets, sync flags, two stsc runs, tail-moov, extended-size headers, zero-to-EOF mdat, two fragments, trun fields/defaults, backpressure, and leased payload identity. Bytewise deterministic corruption sweeps and truncation sweeps must terminate without invalid memory access or unbounded retries.

Native I/O tests issue several reads before polling, consume them in reverse request order, cross arbitrary block boundaries, validate short EOF reads, reject invalid/oversized ranges, abandon/cancel an operation, and verify stale-token rejection. A short-response test provider verifies parser rejection rather than a busy retry loop.

Allocation probes intercept global C++ allocation operators; MSVC Debug additionally hooks CRT allocation/reallocation. Each guarded parsing/I/O loop starts after sealing, reports zero observed allocations, and leaves arena allocation counts unchanged. This is not an audit of Windows/kernel/driver private heaps, and the Release C++ probe alone does not intercept arbitrary direct malloc/HeapAlloc calls. Production source inspection shows none in the streaming code.

The portable core, POSIX memory implementation, platform contracts, demuxer, and tests also compile/run under GCC on Linux with `-Wall -Wextra -Wpedantic -Werror`. Sanitizer builds exercise the same malformed corpus. From the repository root on Linux, without CMake:

```sh
g++ -std=c++20 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -Itests \
  src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp \
  src/demux/IsobmffParser.cpp tests/demux_tests.cpp tests/allocation_probe.cpp \
  -o media_demux_tests
./media_demux_tests
```

These are synthetic container and storage correctness tests, not full codec playback/conformance tests or throughput benchmarks. macOS, Android, iOS, and Clang execution have not been claimed. The absence of native headers in core/demux contracts is additionally enforced by their actual Linux compilation.
