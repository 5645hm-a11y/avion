// HttpBlockSource against the in-process loopback server. Every streaming phase runs under the allocation probe
// (the server itself is allocation-free after Start). Small chunks exercise read-ahead, aliasing and eviction.
#include "media/platform/windows/HttpBlockSource.hpp"
#include "loopback_http_server.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cwchar>
#include <vector>
using namespace media;
using platform::IoStatus;
using platform::windows::HttpBlockSource;
using platform::windows::HttpSourceOptions;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::uint32_t kSize = 3 * 1024 * 1024 + 12345;
std::vector<std::byte> data;
loopback::Server server;
wchar_t url[128], missing[128];

HttpSourceOptions Small() {
    HttpSourceOptions o; o.chunk_bytes = 256 * 1024; o.chunk_count = 8; o.lease_count = 16; o.prefetch_chunks = 2;
    o.overlap_bytes = 32 * 1024; o.read_bytes = 16 * 1024; o.timeout_ms = 3000; o.retries = 2; return o;
}
// Polls (pumping) until the lease settles; returns the final status.
IoStatus Wait(HttpBlockSource& source, platform::BlockToken token, platform::BlockView& view, unsigned timeout_ms = 5000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        (void)source.Pump();
        const auto status = source.Poll(token, view);
        if (status != IoStatus::Pending || GetTickCount64() > deadline) return status;
        Sleep(0);
    }
}
bool Same(const platform::BlockView& view, std::uint64_t offset, std::uint32_t bytes) {
    return view.data && view.file_offset == offset && view.valid_bytes == bytes && std::memcmp(view.data, data.data() + offset, bytes) == 0;
}
void Streaming() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    HttpBlockSource source;
    CHECK(source.Open(url, arena, Small()) == IoStatus::Ready && source.Size() == kSize && source.MaxRangeBytes() == 256 * 1024);
    CHECK(arena.Seal());
    allocation_probe::Begin();
    // Sequential sample-sized reads, as the demuxer issues them: correct bytes, served mostly by read-ahead.
    std::uint64_t offset = 0; unsigned pieces = 0; bool all = true;
    while (offset < kSize) {
        const auto bytes = static_cast<std::uint32_t>((std::min)(std::uint64_t{4096 + (pieces * 7919) % 40000}, kSize - offset));
        platform::BlockToken token{}; platform::BlockView view{};
        IoStatus status = IoStatus::WouldBlock;
        while ((status = source.Request(offset, bytes, token)) == IoStatus::WouldBlock) Sleep(0);
        CHECK(status == IoStatus::Pending);
        all = Wait(source, token, view) == IoStatus::Ready && Same(view, offset, bytes) && all;
        CHECK(source.Release(token) && !source.Release(token));
        offset += bytes; ++pieces;
    }
    CHECK(all);
    const auto stats = source.Statistics();
    CHECK(stats.gets * 8 < pieces && stats.prefetches > 0 && stats.hits > pieces / 2 && stats.bytes < kSize + kSize / 4);
    // Aliasing: two leases in one chunk view the same buffer at their relative offsets.
    platform::BlockToken a{}, b{}; platform::BlockView va{}, vb{};
    CHECK(source.Request(100000, 1000, a) == IoStatus::Pending && source.Request(101000, 2000, b) == IoStatus::Pending);
    CHECK(Wait(source, a, va) == IoStatus::Ready && Wait(source, b, vb) == IoStatus::Ready);
    CHECK(vb.data == va.data + 1000 && Same(va, 100000, 1000) && Same(vb, 101000, 2000));
    // Bounds are explicit statuses; nothing grows.
    platform::BlockToken t{};
    CHECK(source.Request(0, 256 * 1024 + 1, t) == IoStatus::TooLarge && source.Request(kSize - 10, 11, t) == IoStatus::OutOfRange);
    CHECK(source.Request(0, 0, t) == IoStatus::InvalidArgument);
    platform::BlockToken held[16]{}; unsigned granted = 2; // a and b are held.
    for (auto& h : held) if (source.Request(200000 + granted * 100, 100, h) == IoStatus::Pending) ++granted;
    CHECK(granted == 16 && source.Request(0, 1, t) == IoStatus::WouldBlock); // Lease pool exhausted.
    for (auto& h : held) (void)source.Release(h);
    CHECK(source.Release(a) && source.Release(b));
    CHECK(source.Poll(a, va) == IoStatus::InvalidToken);
    CHECK(allocation_probe::End() == 0);
    std::printf("Sequential: %u pieces, %u GETs (%u read-ahead), %u hits, %llu bytes for %u\n", pieces, stats.gets, stats.prefetches, stats.hits,
                static_cast<unsigned long long>(stats.bytes), kSize);
    const auto deadline = GetTickCount64() + 5000;
    while (source.Close() == IoStatus::WouldBlock && GetTickCount64() < deadline) Sleep(1);
}
void Network() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    HttpBlockSource source;
    auto options = Small(); options.prefetch_chunks = 0; // Deterministic GET accounting below.
    CHECK(source.Open(url, arena, options) == IoStatus::Ready);
    CHECK(arena.Seal());
    allocation_probe::Begin();
    platform::BlockToken token{}; platform::BlockView view{};
    // Latency: Poll never blocks; the lease becomes Ready once the response arrives.
    server.latency_ms = 60;
    CHECK(source.Request(1000000, 5000, token) == IoStatus::Pending);
    LARGE_INTEGER f{}, t0{}, t1{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const auto first = source.Poll(token, view);
    QueryPerformanceCounter(&t1);
    const double poll_us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
    const auto begin = GetTickCount64();
    CHECK(first == IoStatus::Pending && Wait(source, token, view) == IoStatus::Ready && Same(view, 1000000, 5000));
    const auto waited = GetTickCount64() - begin;
    CHECK(waited >= 40 && source.Release(token));
    // Random access: the first request of this source, at 1 MB, downloaded one chunk from that offset only.
    while (source.Fetching()) Sleep(1);
    CHECK(source.Statistics().gets == 1 && source.Statistics().bytes == 256 * 1024);
    server.latency_ms = 0;
    // Progressive readiness: under an 80 KB/s throttle, the chunk's first bytes are usable long before the chunk.
    while (source.Fetching()) Sleep(1); // The previous chunk finishes unthrottled.
    server.bytes_per_ms = 80;
    CHECK(source.Request(2000000, 4096, token) == IoStatus::Pending);
    CHECK(Wait(source, token, view) == IoStatus::Ready && Same(view, 2000000, 4096) && source.Fetching() == 1);
    CHECK(source.Release(token));
    server.bytes_per_ms = 0;
    // Transport failure mid-body: the fetch resumes from its watermark with a new range and completes.
    auto before = source.Statistics();
    server.drop_after = 20000; server.drop_gets = 1;
    CHECK(source.Request(2400000, 200000, token) == IoStatus::Pending);
    CHECK(Wait(source, token, view) == IoStatus::Ready && Same(view, 2400000, 200000) && source.Release(token));
    CHECK(source.Statistics().retries == before.retries + 1);
    // Transient 503, then success.
    before = source.Statistics(); server.fail_gets = 1;
    CHECK(source.Request(1500000, 1000, token) == IoStatus::Pending);
    CHECK(Wait(source, token, view) == IoStatus::Ready && Same(view, 1500000, 1000) && source.Release(token));
    CHECK(source.Statistics().retries == before.retries + 1);
    // Retries exhausted: a terminal error, not a hang.
    before = source.Statistics(); server.drop_after = 1000; server.drop_gets = 10;
    CHECK(source.Request(500000, 100000, token) == IoStatus::Pending);
    CHECK(Wait(source, token, view) == IoStatus::OsError && source.Statistics().failures == before.failures + 1);
    CHECK(source.Statistics().gets == before.gets + 1 + options.retries && source.Release(token));
    server.drop_gets = 0;
    // The object changed (new ETag): If-Match fails with 412, which is terminal (no retry).
    server.Mutate();
    before = source.Statistics();
    CHECK(source.Request(3000000, 1000, token) == IoStatus::Pending);
    CHECK(Wait(source, token, view) == IoStatus::OsError && source.LastHttpStatus() == 412 && source.Statistics().retries == before.retries);
    CHECK(source.Release(token));
    CHECK(allocation_probe::End() == 0);
    std::printf("Network: poll %.1f us while a 60 ms response was pending (ready after %llu ms); 412 precondition failures %u\n",
                poll_us, static_cast<unsigned long long>(waited), server.preconditions.load());
    const auto deadline = GetTickCount64() + 5000;
    while (source.Close() == IoStatus::WouldBlock && GetTickCount64() < deadline) Sleep(1);
}
void Lifetime() {
    // Abandoning a pending lease keeps the fetch safe; destroying the source cancels in-flight GETs promptly.
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    {
        HttpBlockSource source; CHECK(source.Open(url, arena, Small()) == IoStatus::Ready);
        server.latency_ms = 3000;
        platform::BlockToken token{};
        CHECK(source.Request(0, 1000, token) == IoStatus::Pending && source.Release(token) && source.Fetching() >= 1);
        const auto begin = GetTickCount64();
        source.BeginCancel();
        platform::BlockToken other{};
        CHECK(source.Request(0, 10, other) == IoStatus::Cancelled);
        while (source.Fetching() && GetTickCount64() - begin < 5000) Sleep(1);
        CHECK(source.Fetching() == 0 && GetTickCount64() - begin < 1000); // Cancelled, not waited out.
        CHECK(source.Close() == IoStatus::Ready);
        server.latency_ms = 0;
    }
    // Open rejects a missing object and a server that ignores ranges (200 instead of 206).
    core::LinearMemoryArena a2; CHECK(a2.Initialize(8 * 1024 * 1024));
    HttpBlockSource absent; CHECK(absent.Open(missing, a2, Small()) == IoStatus::OsError && absent.LastHttpStatus() == 404);
    server.ranges = false;
    core::LinearMemoryArena a3; CHECK(a3.Initialize(8 * 1024 * 1024));
    HttpBlockSource plain; CHECK(plain.Open(url, a3, Small()) == IoStatus::OsError && plain.LastHttpStatus() == 200);
    server.ranges = true;
    core::LinearMemoryArena a4; CHECK(a4.Initialize(1024));
    HttpBlockSource bad; CHECK(bad.Open(L"ftp://127.0.0.1/x", a4, Small()) == IoStatus::InvalidArgument);
}
// API 15: the host's headers on every GET, no redirect followed with them, and a cancelled open.
void Headers() {
    server.require_auth = true;
    {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
        HttpBlockSource none; CHECK(none.Open(url, arena, Small()) == IoStatus::OsError && none.LastHttpStatus() == 401);
    }
    {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
        auto options = Small(); options.headers = L"X-Trace: 1\r\nAuthorization: Bearer loopback-token";
        HttpBlockSource source; CHECK(source.Open(url, arena, options) == IoStatus::Ready && source.Size() == kSize);
        const auto before = server.unauthorized.load();
        platform::BlockToken token{}; platform::BlockView view{};
        CHECK(source.Request(1000000, 5000, token) == IoStatus::Pending && Wait(source, token, view) == IoStatus::Ready && Same(view, 1000000, 5000));
        CHECK(source.Release(token) && server.unauthorized.load() == before); // Every GET carried the token.
        const auto deadline = GetTickCount64() + 5000;
        while (source.Close() == IoStatus::WouldBlock && GetTickCount64() < deadline) Sleep(1);
    }
    server.require_auth = false;
    {
        // A redirect is not followed when headers are set (the token would go wherever it points): the 302 fails the open.
        wchar_t redirect[128]; std::swprintf(redirect, std::size(redirect), L"http://127.0.0.1:%u/redirect", server.Port());
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
        auto options = Small(); options.headers = L"Authorization: Bearer loopback-token";
        HttpBlockSource source; CHECK(source.Open(redirect, arena, options) == IoStatus::OsError && source.LastHttpStatus() == 302);
        core::LinearMemoryArena plain_arena; CHECK(plain_arena.Initialize(8 * 1024 * 1024));
        HttpBlockSource followed; CHECK(followed.Open(redirect, plain_arena, Small()) == IoStatus::Ready && followed.Size() == kSize); // Without: followed.
        const auto deadline = GetTickCount64() + 5000;
        while (followed.Close() == IoStatus::WouldBlock && GetTickCount64() < deadline) Sleep(1);
    }
    {
        // The source's own headers are refused; so is a header longer than the buffer.
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
        auto options = Small(); options.headers = L"X-A: 1\r\nRange: bytes=0-1";
        HttpBlockSource source; CHECK(source.Open(url, arena, options) == IoStatus::InvalidArgument);
        static wchar_t huge[HttpBlockSource::kMaxHeaders + 8]; std::wmemset(huge, L'a', std::size(huge) - 1); huge[std::size(huge) - 1] = 0;
        options.headers = huge; HttpBlockSource too_long; CHECK(too_long.Open(url, arena, options) == IoStatus::TooLarge);
    }
    {
        // Cancelled from another thread while the probe waits on a slow server: back at once.
        server.latency_ms = 3000;
        std::atomic<bool> cancel{false};
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
        auto options = Small(); options.cancel = &cancel;
        HANDLE canceller = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { Sleep(150); static_cast<std::atomic<bool>*>(p)->store(true); return 0; }, &cancel, 0, nullptr);
        const auto begin = GetTickCount64();
        HttpBlockSource source; const auto status = source.Open(url, arena, options);
        const auto took = GetTickCount64() - begin;
        WaitForSingleObject(canceller, 5000); CloseHandle(canceller);
        CHECK(status == IoStatus::Cancelled && took < 1000);
        server.latency_ms = 0;
        std::printf("Headers: token on every GET, 401 without it, redirect refused with it (302), cancelled open back in %llu ms\n", static_cast<unsigned long long>(took));
    }
}
}
int main() {
    data.resize(kSize);
    std::uint32_t x = 2463534242u;
    for (auto& b : data) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<std::byte>(x); }
    if (!server.Start(data.data(), data.size())) { std::puts("loopback server failed"); return 1; }
    std::swprintf(url, std::size(url), L"http://127.0.0.1:%u/fixture.bin?sig=abc", server.Port());
    std::swprintf(missing, std::size(missing), L"http://127.0.0.1:%u/missing", server.Port());
    Streaming();
    Network();
    Lifetime();
    Headers();
    server.Stop();
    std::printf("HTTP block source: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
