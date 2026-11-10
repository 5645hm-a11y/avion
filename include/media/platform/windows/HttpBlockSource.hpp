#pragma once

#include "media/core/linear_memory_arena.hpp"
#include "media/platform/IBlockSource.hpp"
#include <Windows.h>
#include <winhttp.h>
#include <atomic>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line isolated completion state.
#endif
namespace media::platform::windows {
struct HttpSourceOptions {
    std::uint32_t chunk_bytes = 4 * 1024 * 1024; // Largest admissible range (MaxRangeBytes).
    std::uint32_t chunk_count = 24;              // Arena chunk buffers; the only payload storage.
    std::uint32_t lease_count = 64;              // Outstanding views; beyond it Request returns WouldBlock.
    std::uint32_t prefetch_chunks = 2;           // Sequential read-ahead depth behind a requested chunk (1 until the reader enters read-ahead).
    std::uint32_t overlap_bytes = 512 * 1024;    // Read-ahead chunks overlap so boundary samples fit one chunk.
    std::uint32_t retries = 2;                   // Resumed GETs after a transport failure, per chunk fetch.
    std::uint32_t timeout_ms = 10000;            // Resolve/connect/send/receive timeouts; expiry fails the fetch.
    std::uint32_t read_bytes = 256 * 1024;       // WinHttpReadData step: the readiness watermark granularity.
    std::uint32_t connections = 8;               // Connections per server.
    // API 15: the host's request headers ("Name: value", CRLF-separated: Authorization, Cookie, ...), sent with every GET and
    // copied at Open (at most kMaxHeaders - 1 characters). With headers, redirects are not followed: a token never reaches another
    // host (a redirect fails the open with its 3xx status). Range, If-Match and Host are the source's own and are refused.
    const wchar_t* headers = nullptr;
    // Set from any thread: Open stops waiting and returns Cancelled (its probe GET is aborted).
    const std::atomic<bool>* cancel = nullptr;
};
struct HttpSourceStatistics {
    std::uint64_t bytes = 0;          // Payload bytes received into chunks.
    std::uint32_t gets = 0;           // Range GETs issued, including retries and prefetches.
    std::uint32_t prefetches = 0;     // GETs issued ahead of demand.
    std::uint32_t hits = 0;           // Requests served by an already fetched or fetching chunk.
    std::uint32_t retries = 0;        // Resumed GETs after a failure.
    std::uint32_t failures = 0;       // Chunk fetches that failed terminally.
};

// Immutable remote object over HTTP/HTTPS byte ranges (WinHTTP, asynchronous). Payload lands directly in
// arena chunk buffers (WinHttpReadData writes there; nothing is staged or copied). A lease aliases a chunk
// and is Ready as soon as the chunk's received-byte watermark covers it, so a sample is usable one round trip
// after its request, not after its whole chunk. Sequential read-ahead pipelines further chunks.
//
// Threads: one owner calls Open/Request/Poll/Release/Pump/Close; WinHTTP completion callbacks only advance a
// chunk's fetch and publish its watermark and final state with release stores. No lock or wait on the owner
// path. The engine performs no allocation after Open; WinHTTP itself allocates a native request handle per
// GET (outside the application heap).
class HttpBlockSource final : public IBlockSource {
public:
    static constexpr std::size_t kMaxHeaders = 4096; // Characters of the host's headers (an OAuth bearer token is ~200-2000).
    HttpBlockSource() noexcept = default;
    ~HttpBlockSource() noexcept override; // Cancels in-flight fetches and waits for their callbacks to end.
    HttpBlockSource(const HttpBlockSource&) = delete;
    HttpBlockSource& operator=(const HttpBlockSource&) = delete;

    // Setup only (blocking, bounded by timeout_ms): parses the URL, opens the session and probes the object
    // with a one-byte range GET. Requires 206 Partial Content with a complete Content-Range; records the
    // length and a strong ETag, which later GETs send as If-Match so a changed object fails instead of mixing.
    [[nodiscard]] IoStatus Open(const wchar_t* url, core::LinearMemoryArena& arena, HttpSourceOptions options = {}) noexcept;
    [[nodiscard]] std::uint64_t Size() const noexcept override { return size_; }
    [[nodiscard]] std::uint32_t MaxRangeBytes() const noexcept override { return chunk_bytes_; }
    // Pending with a lease, WouldBlock when every lease or every chunk is in use (bounded; never grows).
    [[nodiscard]] IoStatus Request(std::uint64_t offset, std::uint32_t bytes, BlockToken& token) noexcept override;
    // Ready once covered; Pending while bytes are arriving or a retry is due; OsError when the fetch failed.
    [[nodiscard]] IoStatus Poll(BlockToken token, BlockView& view) noexcept override;
    [[nodiscard]] bool Release(BlockToken token) noexcept override;
    // Owner housekeeping: resumes failed fetches that have retries left. Returns GETs reissued.
    std::uint32_t Pump(std::uint32_t budget = 4) noexcept;
    void BeginCancel() noexcept;           // Aborts in-flight GETs; later requests return Cancelled.
    [[nodiscard]] IoStatus Close() noexcept; // Nonblocking; WouldBlock while fetches or leases remain.
    [[nodiscard]] DWORD LastOsError() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t LastHttpStatus() const noexcept { return http_status_; }
    [[nodiscard]] HttpSourceStatistics Statistics() const noexcept;
    [[nodiscard]] std::uint32_t Fetching() const noexcept; // Chunks with a GET in flight.

private:
    enum class State : std::uint8_t { Free, Fetching, Ready, Failed };
    struct Chunk {
        // Owner fields.
        std::byte* data = nullptr;
        std::uint64_t offset = 0, last_use = 0;
        std::uint32_t length = 0, refs = 0, attempts = 0;
        bool prefetched = false, probe = false;
        // Fetch fields: written by the owner before a GET starts, then by that GET's callbacks only. The owner
        // reads them after observing the final state (acquire).
        HttpBlockSource* source = nullptr;
        HINTERNET request = nullptr;
        std::uint32_t start = 0;      // Chunk-relative offset this GET began at (resume point).
        std::uint32_t http_status = 0;
        DWORD error = 0;
        bool complete = false;
        std::uint64_t probe_total = 0;
        wchar_t headers[256]{}; // Range plus If-Match with an ETag of at most 127 characters.
        // Published by callbacks.
        alignas(64) std::atomic<std::uint32_t> received{0}; // Chunk-relative watermark: [0, received) is final.
        std::atomic<State> state{State::Free};
        std::atomic<bool> closing{false};
    };
    struct Lease { std::uint64_t offset = 0; std::uint32_t bytes = 0, chunk = 0, generation = 0; bool active = false; };
    static void CALLBACK Callback(HINTERNET, DWORD_PTR, DWORD, LPVOID, DWORD) noexcept;
    void Advance(Chunk&, DWORD status, LPVOID info, DWORD length) noexcept; // Callback thread.
    void Read(Chunk&) noexcept;                                          // Callback thread.
    void Finish(Chunk&, DWORD error) noexcept;                           // Callback thread.
    [[nodiscard]] bool Start(Chunk&) noexcept;                           // Owner: issue/resume a GET.
    [[nodiscard]] Chunk* Covering(std::uint64_t offset, std::uint32_t bytes) noexcept;
    [[nodiscard]] Chunk* Claim(const Chunk* keep) noexcept;
    void Prefetch(const Chunk& from, std::uint32_t depth) noexcept;
    [[nodiscard]] bool Service(Chunk&) noexcept; // Retry a failed fetch when allowed; false when terminal.
    [[nodiscard]] Lease* Find(BlockToken) noexcept;
    HINTERNET session_ = nullptr, connect_ = nullptr;
    Chunk* chunks_ = nullptr;
    Lease* leases_ = nullptr;
    std::uint64_t size_ = 0, tick_ = 0;
    std::uint32_t chunk_count_ = 0, lease_count_ = 0, chunk_bytes_ = 0, prefetch_ = 0, overlap_ = 0;
    std::uint32_t retries_ = 0, read_bytes_ = 0, next_generation_ = 0, http_status_ = 0;
    DWORD error_ = 0;
    HttpSourceStatistics stats_{};
    std::atomic<std::uint64_t> received_bytes_{0}; // Advanced by callbacks.
    bool secure_ = false, cancelling_ = false, strong_etag_ = false;
    wchar_t host_[256]{}, path_[2048]{}, etag_[128]{};
    wchar_t headers_[kMaxHeaders]{}; // The host's (API 15), "" without.
};
} // namespace media::platform::windows
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
