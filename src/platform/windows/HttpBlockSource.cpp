#include "media/platform/windows/HttpBlockSource.hpp"

#include <algorithm>
#include <climits>
#include <iterator>
#include <memory>

namespace media::platform::windows {
namespace {
// Formatting and parsing without allocation or locale.
wchar_t* Decimal(wchar_t* out, std::uint64_t value) noexcept {
    wchar_t digits[20]; unsigned count = 0;
    do { digits[count++] = static_cast<wchar_t>(L'0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    return out;
}
wchar_t* Text(wchar_t* out, const wchar_t* text) noexcept { while (*text) *out++ = *text++; return out; }
bool Number(const wchar_t*& p, std::uint64_t& value) noexcept {
    if (*p < L'0' || *p > L'9') return false;
    value = 0;
    for (; *p >= L'0' && *p <= L'9'; ++p) {
        if (value > (UINT64_MAX - 9) / 10) return false;
        value = value * 10 + static_cast<std::uint64_t>(*p - L'0');
    }
    return true;
}
// "bytes first-last/total" (RFC 9110 14.4); an unknown total ("*") is rejected: the object must have a length.
bool ContentRange(const wchar_t* p, std::uint64_t& first, std::uint64_t& last, std::uint64_t& total) noexcept {
    while (*p == L' ') ++p;
    for (const wchar_t* unit = L"bytes"; *unit; ++unit, ++p) if ((*p | 0x20) != *unit) return false;
    if (*p != L' ') return false;
    while (*p == L' ') ++p;
    if (!Number(p, first) || *p++ != L'-' || !Number(p, last) || *p++ != L'/' || !Number(p, total)) return false;
    while (*p == L' ') ++p;
    return *p == 0 && first <= last && last < total;
}
}

HttpBlockSource::~HttpBlockSource() noexcept {
    // Teardown only: in-flight GETs are aborted, and their chunk buffers stay untouched by WinHTTP only after
    // each handle's closing notification, so wait for those (bounded: a stuck stack is left to process exit).
    BeginCancel();
    const auto deadline = GetTickCount64() + 30000;
    while (Fetching() && GetTickCount64() < deadline) Sleep(1);
    if (!Fetching()) {
        if (session_) (void)WinHttpSetStatusCallback(session_, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }
}

void CALLBACK HttpBlockSource::Callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) noexcept {
    // Session and connection handles carry no context; request handles carry their chunk.
    if (context) { auto& chunk = *reinterpret_cast<Chunk*>(context); chunk.source->Advance(chunk, status, info, length); }
}

void HttpBlockSource::Read(Chunk& c) noexcept {
    const auto received = c.received.load(std::memory_order_relaxed);
    const auto want = (std::min)(read_bytes_, c.length - received);
    // Direct to the arena: WinHTTP writes the payload into the chunk buffer; nothing is staged.
    if (!WinHttpReadData(c.request, c.data + received, want, nullptr)) Finish(c, GetLastError());
}

void HttpBlockSource::Finish(Chunk& c, DWORD error) noexcept {
    // The first finisher (completion, failure or owner cancellation) closes the handle exactly once; the
    // closing notification that follows publishes the final state.
    if (c.closing.exchange(true, std::memory_order_acq_rel)) return;
    c.error = error; c.complete = error == 0;
    WinHttpCloseHandle(c.request);
}

void HttpBlockSource::Advance(Chunk& c, DWORD status, LPVOID info, DWORD length) noexcept {
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        if (!WinHttpReceiveResponse(c.request, nullptr)) Finish(c, GetLastError());
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
        DWORD code = 0, size = sizeof(code);
        if (!WinHttpQueryHeaders(c.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &code, &size, WINHTTP_NO_HEADER_INDEX)) { Finish(c, GetLastError()); break; }
        c.http_status = code;
        // Only a partial response for exactly the requested bytes of the same-length object is admitted: a 200
        // (ranges ignored), a different range or a changed length would mix unrelated bytes into the chunk.
        wchar_t range[96]{}; size = sizeof(range);
        std::uint64_t first = 0, last = 0, total = 0;
        if (code >= 500) { Finish(c, ERROR_WINHTTP_CONNECTION_ERROR); break; } // Transient server failure: retryable.
        if (code != 206 || !WinHttpQueryHeaders(c.request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range, &size,
                                                 WINHTTP_NO_HEADER_INDEX) || !ContentRange(range, first, last, total) ||
            first != c.offset + c.start || last != c.offset + c.length - 1 || (!c.probe && total != size_)) {
            Finish(c, ERROR_WINHTTP_INVALID_SERVER_RESPONSE); break;
        }
        if (c.probe) {
            // Open waits for this fetch's final state (acquire) before reading these owner fields.
            c.probe_total = total; size = sizeof(etag_);
            if (!WinHttpQueryHeaders(c.request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, etag_, &size, WINHTTP_NO_HEADER_INDEX)) etag_[0] = 0;
            // If-Match uses strong comparison: a weak validator could never match, so it is not sent.
            strong_etag_ = etag_[0] == L'"';
        }
        Read(c);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
        if (!length) { Finish(c, ERROR_WINHTTP_CONNECTION_ERROR); break; } // Body ended before the range did.
        const auto received = c.received.load(std::memory_order_relaxed) + length;
        received_bytes_.fetch_add(length, std::memory_order_relaxed);
        c.received.store(received, std::memory_order_release); // [0, received) is final: leases below may be read.
        if (received >= c.length) Finish(c, 0); else Read(c);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        Finish(c, static_cast<const WINHTTP_ASYNC_RESULT*>(info)->dwError);
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        // Last notification for this request: WinHTTP no longer touches the chunk.
        c.state.store(c.complete ? State::Ready : State::Failed, std::memory_order_release);
        break;
    default: break;
    }
}

IoStatus HttpBlockSource::Open(const wchar_t* url, core::LinearMemoryArena& arena, const HttpSourceOptions options) noexcept {
    if (session_ || !url || arena.IsSealed() || !options.chunk_count || options.chunk_count > 1024 || options.chunk_bytes < 4096 ||
        !options.lease_count || options.lease_count > 65536 || options.overlap_bytes >= options.chunk_bytes || !options.read_bytes ||
        !options.timeout_ms || options.timeout_ms > INT_MAX / 2)
        return IoStatus::InvalidArgument;
    error_ = 0; http_status_ = 0;
    headers_[0] = 0;
    if (options.headers && options.headers[0]) {
        const std::size_t n = wcslen(options.headers);
        if (n >= kMaxHeaders) return IoStatus::TooLarge;
        // The source's own headers stay its own (a host's Range would mix chunks); names compared without case at a line start.
        for (std::size_t i = 0; i < n; ++i) {
            if (i && options.headers[i - 1] != L'\n') continue;
            if (_wcsnicmp(options.headers + i, L"Range:", 6) == 0 || _wcsnicmp(options.headers + i, L"If-Match:", 9) == 0 ||
                _wcsnicmp(options.headers + i, L"Host:", 5) == 0) return IoStatus::InvalidArgument;
        }
        std::copy_n(options.headers, n + 1, headers_);
    }
    wchar_t extra[2048]{};
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host_; parts.dwHostNameLength = static_cast<DWORD>(std::size(host_));
    parts.lpszUrlPath = path_; parts.dwUrlPathLength = static_cast<DWORD>(std::size(path_));
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) { error_ = GetLastError(); return IoStatus::InvalidArgument; }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return IoStatus::InvalidArgument;
    // The query string (for example a presigned URL's signature) is part of the request target.
    if (parts.dwUrlPathLength + parts.dwExtraInfoLength >= std::size(path_)) return IoStatus::TooLarge;
    std::copy_n(extra, parts.dwExtraInfoLength + 1, path_ + parts.dwUrlPathLength);
    if (!path_[0]) { path_[0] = L'/'; path_[1] = 0; }
    secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;
    session_ = WinHttpOpen(L"media-engine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session_) { error_ = GetLastError(); return IoStatus::OsError; }
    const int timeout = static_cast<int>(options.timeout_ms);
    DWORD connections = options.connections ? options.connections : 1;
    if (!WinHttpSetTimeouts(session_, timeout, timeout, timeout, timeout) ||
        !WinHttpSetOption(session_, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &connections, sizeof(connections)) ||
        WinHttpSetStatusCallback(session_, Callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK ||
        !(connect_ = WinHttpConnect(session_, host_, parts.nPort, 0))) {
        error_ = GetLastError(); (void)Close(); return IoStatus::OsError;
    }
    chunks_ = static_cast<Chunk*>(arena.Allocate(sizeof(Chunk) * options.chunk_count, alignof(Chunk)));
    leases_ = static_cast<Lease*>(arena.Allocate(sizeof(Lease) * options.lease_count, alignof(Lease)));
    if (!chunks_ || !leases_) { (void)Close(); return IoStatus::TooLarge; }
    for (std::uint32_t i = 0; i < options.chunk_count; ++i) std::construct_at(chunks_ + i);
    for (std::uint32_t i = 0; i < options.lease_count; ++i) std::construct_at(leases_ + i);
    chunk_count_ = options.chunk_count; lease_count_ = options.lease_count;
    for (std::uint32_t i = 0; i < chunk_count_; ++i) {
        chunks_[i].source = this;
        chunks_[i].data = static_cast<std::byte*>(arena.Allocate(options.chunk_bytes, 64));
        if (!chunks_[i].data) { (void)Close(); return IoStatus::TooLarge; }
    }
    chunk_bytes_ = options.chunk_bytes; prefetch_ = options.prefetch_chunks; overlap_ = options.overlap_bytes;
    retries_ = options.retries; read_bytes_ = options.read_bytes; cancelling_ = false;
    // Probe: a one-byte range proves range support and reports the length (and the validator) in one round trip.
    auto& probe = chunks_[0];
    probe.probe = true; probe.offset = 0; probe.length = 1;
    const bool started = Start(probe);
    const auto deadline = GetTickCount64() + 2ull * options.timeout_ms;
    const auto cancelled = [&] { return options.cancel && options.cancel->load(std::memory_order_acquire); };
    while (started && probe.state.load(std::memory_order_acquire) == State::Fetching && GetTickCount64() < deadline && !cancelled()) Sleep(1);
    const auto state = probe.state.load(std::memory_order_acquire);
    if (state == State::Fetching) { BeginCancel(); while (Fetching()) Sleep(1); cancelling_ = false; }
    if (cancelled()) { (void)Close(); return IoStatus::Cancelled; }
    http_status_ = probe.http_status;
    if (!started || state != State::Ready || !probe.probe_total) {
        if (started && probe.error) error_ = probe.error;
        (void)Close(); return IoStatus::OsError;
    }
    size_ = probe.probe_total;
    probe.probe = false; probe.state.store(State::Free, std::memory_order_relaxed); probe.received.store(0, std::memory_order_relaxed);
    stats_ = {}; received_bytes_.store(0, std::memory_order_relaxed);
    return IoStatus::Ready;
}

bool HttpBlockSource::Start(Chunk& c) noexcept {
    // Owner only, with no GET in flight for this chunk: resume where the watermark stopped.
    const auto from = c.received.load(std::memory_order_relaxed);
    c.start = from; c.complete = false; c.error = 0; c.http_status = 0;
    c.closing.store(false, std::memory_order_relaxed);
    wchar_t* out = Text(c.headers, L"Range: bytes=");
    out = Decimal(out, c.offset + from); *out++ = L'-'; out = Decimal(out, c.offset + c.length - 1);
    if (strong_etag_ && !c.probe) { out = Text(out, L"\r\nIf-Match: "); out = Text(out, etag_); }
    *out = 0;
    c.request = WinHttpOpenRequest(connect_, L"GET", path_, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (!c.request) { error_ = GetLastError(); c.state.store(State::Failed, std::memory_order_relaxed); return false; }
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&c);
    if (!WinHttpSetOption(c.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        error_ = GetLastError(); WinHttpCloseHandle(c.request); c.state.store(State::Failed, std::memory_order_relaxed); return false;
    }
    if (headers_[0]) {
        // The host's headers (API 15), and no redirect: WinHTTP would send them to wherever the server points.
        DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpAddRequestHeaders(c.request, headers_, static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
            !WinHttpSetOption(c.request, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects))) {
            error_ = GetLastError(); WinHttpCloseHandle(c.request); c.state.store(State::Failed, std::memory_order_relaxed); return false;
        }
    }
    ++c.attempts; ++stats_.gets;
    c.state.store(State::Fetching, std::memory_order_release); // Callbacks may run before SendRequest returns.
    if (!WinHttpSendRequest(c.request, c.headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, context)) {
        // No completion follows a synchronous failure; closing still notifies, which publishes Failed.
        error_ = GetLastError(); Finish(c, error_);
    }
    return true;
}

HttpBlockSource::Chunk* HttpBlockSource::Covering(std::uint64_t offset, std::uint32_t bytes) noexcept {
    Chunk* best = nullptr;
    for (std::uint32_t i = 0; i < chunk_count_; ++i) {
        auto& c = chunks_[i];
        const auto state = c.state.load(std::memory_order_acquire);
        if (state == State::Free || (state == State::Failed && c.attempts > retries_)) continue;
        if (c.offset > offset || offset + bytes > c.offset + c.length) continue;
        if (!best || state == State::Ready) best = &c;
    }
    return best;
}

HttpBlockSource::Chunk* HttpBlockSource::Claim(const Chunk* keep) noexcept {
    // A free chunk, else the least recently used idle one (no lease, no GET in flight). When `keep` is given
    // (read-ahead), only chunks older than it qualify, so read-ahead never evicts what is being read.
    Chunk* victim = nullptr;
    for (std::uint32_t i = 0; i < chunk_count_; ++i) {
        auto& c = chunks_[i];
        const auto state = c.state.load(std::memory_order_acquire);
        if (state == State::Free) { victim = &c; break; }
        if (c.refs || state == State::Fetching || (keep && c.last_use >= keep->last_use)) continue;
        if (!victim || c.last_use < victim->last_use) victim = &c;
    }
    if (victim) {
        victim->state.store(State::Free, std::memory_order_relaxed);
        victim->received.store(0, std::memory_order_relaxed);
        victim->attempts = 0; victim->refs = 0; victim->prefetched = false;
    }
    return victim;
}

void HttpBlockSource::Prefetch(const Chunk& from, const std::uint32_t depth_limit) noexcept {
    const Chunk* base = &from;
    for (std::uint32_t depth = 0; depth < depth_limit; ++depth) {
        if (base->length < chunk_bytes_) return; // Reached the end of the object.
        const auto next = base->offset + base->length - overlap_;
        if (next >= size_) return;
        const auto length = static_cast<std::uint32_t>((std::min)(static_cast<std::uint64_t>(chunk_bytes_), size_ - next));
        const Chunk* existing = nullptr;
        for (std::uint32_t i = 0; i < chunk_count_ && !existing; ++i) {
            const auto& c = chunks_[i];
            const auto state = c.state.load(std::memory_order_acquire);
            if (state != State::Free && !(state == State::Failed && c.attempts > retries_) &&
                c.offset <= next && next + length <= c.offset + c.length) existing = &c;
        }
        if (!existing) {
            auto* c = Claim(&from);
            if (!c) return;
            c->offset = next; c->length = length; c->prefetched = true; c->last_use = ++tick_;
            if (!Start(*c)) return;
            ++stats_.prefetches; existing = c;
        }
        base = existing;
    }
}

bool HttpBlockSource::Service(Chunk& c) noexcept {
    if (c.state.load(std::memory_order_acquire) != State::Failed) return true;
    // 412 (changed object) or a malformed partial response will not improve on retry.
    if (!cancelling_ && c.attempts <= retries_ && c.http_status != 412 && c.error != ERROR_WINHTTP_INVALID_SERVER_RESPONSE) {
        ++stats_.retries;
        return Start(c);
    }
    if (c.attempts <= retries_ + 1) { ++stats_.failures; c.attempts = retries_ + 2; } // Counted once.
    return false;
}

HttpBlockSource::Lease* HttpBlockSource::Find(const BlockToken token) noexcept {
    if (token.id >= lease_count_ || !leases_) return nullptr;
    auto& lease = leases_[token.id];
    return lease.active && lease.generation == token.generation ? &lease : nullptr;
}

IoStatus HttpBlockSource::Request(const std::uint64_t offset, const std::uint32_t bytes, BlockToken& token) noexcept {
    token = {};
    if (!session_) return IoStatus::Closed;
    if (cancelling_) return IoStatus::Cancelled;
    if (!bytes) return IoStatus::InvalidArgument;
    if (offset > size_ || bytes > size_ - offset) return IoStatus::OutOfRange;
    if (bytes > chunk_bytes_) return IoStatus::TooLarge;
    Lease* lease = nullptr;
    std::uint32_t id = 0;
    for (; id < lease_count_ && !lease; ++id) if (!leases_[id].active) lease = &leases_[id];
    if (!lease || next_generation_ == UINT32_MAX) return IoStatus::WouldBlock;
    auto* c = Covering(offset, bytes);
    if (c) {
        ++stats_.hits;
        if (!Service(*c)) { error_ = c->error; http_status_ = c->http_status; return IoStatus::OsError; }
    } else {
        // A full chunk from here, unless a live chunk already starts inside that span: then only up to it (at
        // least this range), so a sample straddling a read-ahead boundary costs its own bytes, not a duplicate
        // of the next chunk.
        auto length = (std::min)(static_cast<std::uint64_t>(chunk_bytes_), size_ - offset);
        for (std::uint32_t i = 0; i < chunk_count_; ++i) {
            const auto& other = chunks_[i];
            const auto state = other.state.load(std::memory_order_acquire);
            if (state == State::Free || (state == State::Failed && other.attempts > retries_)) continue;
            if (other.offset > offset && other.offset < offset + length) length = (std::max)(other.offset - offset, std::uint64_t{bytes});
        }
        c = Claim(nullptr);
        if (!c) return IoStatus::WouldBlock; // Every chunk is leased or in flight: bounded, retry later.
        c->offset = offset;
        c->length = static_cast<std::uint32_t>(length);
        if (!Start(*c)) return IoStatus::OsError;
    }
    *lease = {offset, bytes, static_cast<std::uint32_t>(c - chunks_), ++next_generation_, true};
    ++c->refs; c->last_use = ++tick_;
    token = {id - 1, lease->generation};
    // Read-ahead: one chunk behind a chunk fetched on demand (the open, a seek, the Cues at the end of a Matroska file), the full
    // depth once the reader asks inside a read-ahead chunk, i.e. reads on through the object. Playback after a seek still has a
    // chunk in flight ahead of it, and a seek no longer fetches two chunks nobody may read (2026-11-09).
    Prefetch(*c, c->prefetched ? prefetch_ : (std::min)(prefetch_, std::uint32_t{1}));
    return IoStatus::Pending;
}

IoStatus HttpBlockSource::Poll(const BlockToken token, BlockView& view) noexcept {
    view = {};
    auto* lease = Find(token);
    if (!lease) return IoStatus::InvalidToken;
    auto& c = chunks_[lease->chunk];
    const auto need = static_cast<std::uint32_t>(lease->offset + lease->bytes - c.offset);
    if (c.received.load(std::memory_order_acquire) >= need) {
        view = {c.data + (lease->offset - c.offset), lease->offset, lease->bytes, token};
        return IoStatus::Ready;
    }
    const auto state = c.state.load(std::memory_order_acquire);
    if (state == State::Failed && !Service(c)) { error_ = c.error; http_status_ = c.http_status; return IoStatus::OsError; }
    if (state == State::Ready) { error_ = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; return IoStatus::OsError; } // Unreachable.
    return cancelling_ ? IoStatus::Cancelled : IoStatus::Pending;
}

bool HttpBlockSource::Release(const BlockToken token) noexcept {
    auto* lease = Find(token);
    if (!lease) return false;
    // A pending lease only drops interest: the chunk keeps fetching into its own storage and stays cached.
    --chunks_[lease->chunk].refs; lease->active = false;
    return true;
}

std::uint32_t HttpBlockSource::Pump(const std::uint32_t budget) noexcept {
    std::uint32_t reissued = 0;
    for (std::uint32_t i = 0; i < chunk_count_ && reissued < budget; ++i) {
        auto& c = chunks_[i];
        if (c.refs && c.state.load(std::memory_order_acquire) == State::Failed && c.attempts <= retries_) {
            const auto before = stats_.retries;
            (void)Service(c);
            reissued += stats_.retries - before;
        }
    }
    return reissued;
}

std::uint32_t HttpBlockSource::Fetching() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < chunk_count_; ++i) count += chunks_[i].state.load(std::memory_order_acquire) == State::Fetching ? 1u : 0u;
    return count;
}

HttpSourceStatistics HttpBlockSource::Statistics() const noexcept {
    auto statistics = stats_; statistics.bytes = received_bytes_.load(std::memory_order_relaxed); return statistics;
}

void HttpBlockSource::BeginCancel() noexcept {
    cancelling_ = true;
    for (std::uint32_t i = 0; i < chunk_count_; ++i) {
        auto& c = chunks_[i];
        // Closing the handle aborts the GET; its callbacks end with the closing notification (Failed).
        if (c.state.load(std::memory_order_acquire) == State::Fetching && !c.closing.exchange(true, std::memory_order_acq_rel))
            WinHttpCloseHandle(c.request);
    }
}

IoStatus HttpBlockSource::Close() noexcept {
    if (Fetching()) return IoStatus::WouldBlock;
    for (std::uint32_t i = 0; i < lease_count_; ++i) if (leases_[i].active) return IoStatus::WouldBlock;
    if (session_) (void)WinHttpSetStatusCallback(session_, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
    connect_ = session_ = nullptr; chunks_ = nullptr; leases_ = nullptr;
    chunk_count_ = lease_count_ = 0; size_ = 0; chunk_bytes_ = 0;
    return IoStatus::Ready;
}
} // namespace media::platform::windows
