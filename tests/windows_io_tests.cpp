#include "media/platform/windows/Win32IocpBlockSource.hpp"
#include "media/demux/IsobmffParser.hpp"
#include "allocation_probe.hpp"
#include "isobmff_fixture.hpp"
#include <Windows.h>
#include <intrin.h>
#include <cstdio>
#include <atomic>

using namespace media;
using platform::IoStatus;
using platform::windows::Win32IocpBlockSource;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s (Win32=%lu)\n", __LINE__, #x, GetLastError()); ++failures; } } while (false)

struct TemporaryFile {
    wchar_t path[MAX_PATH]{};
    TemporaryFile(const void* data, DWORD bytes) {
        wchar_t directory[MAX_PATH]{};
        CHECK(GetTempPathW(MAX_PATH, directory) != 0);
        CHECK(GetTempFileNameW(directory, L"mio", 0, path) != 0);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            CHECK(WriteFile(file, data, bytes, &written, nullptr) && written == bytes);
            CHECK(CloseHandle(file));
        }
    }
    ~TemporaryFile() { CHECK(DeleteFileW(path)); }
};

IoStatus Await(Win32IocpBlockSource& source, platform::BlockToken token, platform::BlockView& view) {
    const auto deadline = GetTickCount64() + 10000;
    IoStatus status;
    do {
        status = source.Poll(token, view);
        if (status != IoStatus::Pending) return status;
        _mm_pause();
    } while (GetTickCount64() < deadline);
    return status;
}

void SourceTest(bool direct) {
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(2 * 1024 * 1024));
    auto* pattern = static_cast<std::byte*>(arena.Allocate(200123));
    CHECK(pattern != nullptr);
    if (!pattern) return;
    for (std::uint32_t i = 0; i < 200123; ++i) pattern[i] = static_cast<std::byte>((i * 17 + (i >> 8)) & 255);
    TemporaryFile file(pattern, 200123);
    Win32IocpBlockSource source;
    const auto open = source.Open(file.path, arena, {65536, 4, direct});
    if (open != IoStatus::Ready) std::printf("Open error %u / %lu\n", static_cast<unsigned>(open), source.LastOsError());
    CHECK(open == IoStatus::Ready);
    if (open != IoStatus::Ready) return;
    CHECK(source.Size() == 200123);
    CHECK(arena.Seal());
    const auto allocations = arena.Statistics().allocation_count;
    allocation_probe::Begin();
    const std::uint64_t offsets[]{0, 65530, 130000, 200000};
    const std::uint32_t lengths[]{128, 40, 8000, 123};
    platform::BlockToken tokens[4]{};
    for (unsigned i = 0; i < 4; ++i) CHECK(source.Request(offsets[i], lengths[i], tokens[i]) == IoStatus::Pending);
    platform::BlockToken ignored{};
    CHECK(source.Stats().reads == 4 && source.Leases() == 4);
    // Every block is held: a range none of them contains must wait (196000 lies between the third and fourth reads).
    CHECK(source.Request(196000, 10, ignored) == IoStatus::WouldBlock);
    CHECK(source.Request(0, 0, ignored) == IoStatus::InvalidArgument);
    CHECK(source.Request(UINT64_MAX, 4, ignored) == IoStatus::OutOfRange);
    CHECK(source.Request(200122, 2, ignored) == IoStatus::OutOfRange);
    CHECK(source.Request(0, 65537, ignored) == IoStatus::TooLarge);
    CHECK(source.Close() == IoStatus::WouldBlock);
    for (int i = 3; i >= 0; --i) { // Deliberately consume out of issue order.
        platform::BlockView view{};
        CHECK(Await(source, tokens[i], view) == IoStatus::Ready);
        CHECK(reinterpret_cast<std::uintptr_t>(view.data) % source.Alignment() == 0);
        CHECK(offsets[i] >= view.file_offset && offsets[i] - view.file_offset + lengths[i] <= view.valid_bytes);
        if (view.data) {
            for (std::uint32_t n = 0; n < lengths[i]; ++n)
                CHECK(view.data[offsets[i] - view.file_offset + n] == pattern[offsets[i] + n]);
        }
        CHECK(source.Release(tokens[i]));
        CHECK(!source.Release(tokens[i]));
        CHECK(source.Poll(tokens[i], view) == IoStatus::InvalidToken);
    }
    CHECK(source.Outstanding() == 0 && source.Leases() == 0);
    // Ranges inside a block already read share it: no read, the same memory, and each lease pins the block on its own.
    {
        const auto reads = source.Stats().reads;
        const std::uint64_t inside[]{5, 1000, 60000};
        platform::BlockToken shared[3]{}; platform::BlockView views[3]{};
        for (unsigned i = 0; i < 3; ++i) CHECK(source.Request(inside[i], 100, shared[i]) == IoStatus::Pending);
        CHECK(shared[0].id != shared[1].id && shared[1].id != shared[2].id && shared[0].generation != shared[1].generation);
        CHECK(source.Stats().reads == reads && source.Outstanding() == 0 && source.Leases() == 3);
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(source.Poll(shared[i], views[i]) == IoStatus::Ready && views[i].data == views[0].data && views[i].file_offset == 0);
            if (views[i].data) for (std::uint32_t n = 0; n < 100; ++n) CHECK(views[i].data[inside[i] + n] == pattern[inside[i] + n]);
        }
        CHECK(source.Release(shared[0]) && !source.Release(shared[0]));
        // Three other ranges take a lease on each of the other three blocks; the shared one stays pinned by its two remaining leases, so
        // a fourth range has nowhere to go until a block is let go.
        platform::BlockToken others[3]{}; platform::BlockView view{};
        const std::uint64_t elsewhere[]{70000, 140000, 200000};
        for (unsigned i = 0; i < 3; ++i) { CHECK(source.Request(elsewhere[i], 64, others[i]) == IoStatus::Pending); CHECK(Await(source, others[i], view) == IoStatus::Ready); }
        CHECK(source.Request(196000, 10, ignored) == IoStatus::WouldBlock);
        CHECK(source.Poll(shared[1], view) == IoStatus::Ready && view.data == views[0].data && view.data[1000] == pattern[1000]);
        CHECK(source.Release(others[1]));
        // The freed block is read again for the new range (the least recently used one that nobody holds).
        const auto before = source.Stats().reads;
        CHECK(source.Request(196000, 10, ignored) == IoStatus::Pending && source.Stats().reads == before + 1);
        CHECK(Await(source, ignored, view) == IoStatus::Ready && view.data[196000 - view.file_offset] == pattern[196000]);
        CHECK(source.Release(ignored) && source.Release(shared[1]) && source.Release(shared[2]) && source.Release(others[0]) && source.Release(others[2]));
        // Released blocks keep their bytes: asking again costs no read.
        const auto cached = source.Stats().reads;
        CHECK(source.Request(60000, 100, ignored) == IoStatus::Pending && source.Stats().reads == cached);
        CHECK(source.Poll(ignored, view) == IoStatus::Ready && view.data[60000 - view.file_offset] == pattern[60000] && source.Release(ignored));
        CHECK(source.Outstanding() == 0 && source.Leases() == 0);
    }
    platform::BlockToken abandoned{};
    CHECK(source.Request(100, 10, abandoned) == IoStatus::Pending);
    CHECK(source.Release(abandoned));
    source.BeginCancel();
    CHECK(source.Request(0, 1, ignored) == IoStatus::Cancelled);
    const auto deadline = GetTickCount64() + 10000;
    while (source.Outstanding() && GetTickCount64() < deadline) { (void)source.Pump(); _mm_pause(); }
    CHECK(source.Outstanding() == 0);
    CHECK(allocation_probe::End() == 0);
    CHECK(arena.Statistics().allocation_count == allocations);
    CHECK(source.Close() == IoStatus::Ready);
    CHECK(source.Request(0, 1, ignored) == IoStatus::Closed);
}

void DemuxIntegration(bool fragmented) {
    fixture::File fixture;
    if (fragmented) fixture.Fragmented(demux::Codec::H264); else fixture.Classic(demux::Codec::H264, true, true);
    TemporaryFile file(fixture.data.data(), fixture.size);
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(2 * 1024 * 1024));
    Win32IocpBlockSource source;
    CHECK(source.Open(file.path, arena, {65536, 4, true}) == IoStatus::Ready);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, 65536, 9}));
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    CHECK(arena.Seal());
    const auto allocations = arena.Statistics().allocation_count;
    unsigned samples = 0;
    auto status = demux::ParseStatus::Progress;
    const auto deadline = GetTickCount64() + 10000;
    allocation_probe::Begin();
    while (status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed && GetTickCount64() < deadline) {
        status = parser.Step(queue);
        if (const auto* packet = queue.PeekReadSlot()) {
            platform::BlockView view{};
            CHECK(source.Poll({packet->payload_block_id, packet->payload_generation}, view) == IoStatus::Ready);
            CHECK(samples < fixture.sample_count);
            if (samples < fixture.sample_count) {
                CHECK(view.file_offset + packet->payload_offset == fixture.samples[samples]);
                for (std::uint32_t i = 0; i < packet->payload_bytes; ++i)
                    CHECK(view.data[packet->payload_offset + i] == fixture.data[fixture.samples[samples] + i]);
            }
            CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot()); ++samples;
        }
        _mm_pause();
    }
    CHECK(allocation_probe::End() == 0);
    CHECK(status == demux::ParseStatus::Done && samples == 3);
    CHECK(arena.Statistics().allocation_count == allocations);
    CHECK(source.Close() == IoStatus::Ready);
}

struct Handoff {
    Win32IocpBlockSource* source;
    std::atomic<bool> done{false};
    bool good = false;
};
DWORD WINAPI PollOnWorker(void* argument) {
    auto& handoff = *static_cast<Handoff*>(argument);
    platform::BlockToken token{}; platform::BlockView view{};
    handoff.good = handoff.source->Request(0, 16, token) == IoStatus::Pending;
    if (handoff.good) {
        handoff.good = Await(*handoff.source, token, view) == IoStatus::Ready;
        if (handoff.good) handoff.good = view.data[0] == std::byte{0x5a};
        handoff.good = handoff.source->Release(token) && handoff.good;
    }
    handoff.done.store(true, std::memory_order_release); return 0;
}
void OwnerHandoff() {
    const std::byte bytes[16]{std::byte{0x5a}}; TemporaryFile file(bytes, 16);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    Win32IocpBlockSource source; CHECK(source.Open(file.path, arena, {65536, 2, false}) == IoStatus::Ready);
    platform::BlockToken token{}; platform::BlockView view{};
    CHECK(source.Request(0, 16, token) == IoStatus::Pending); CHECK(Await(source, token, view) == IoStatus::Ready); CHECK(source.Release(token));
    CHECK(source.DetachPollingThread()); CHECK(arena.Seal());
    Handoff handoff{&source}; HANDLE worker = CreateThread(nullptr, 0, PollOnWorker, &handoff, 0, nullptr);
    CHECK(worker != nullptr); if (!worker) return;
    // Deliberately remain runnable: sleeping here masks the concurrency=1 IOCP association bug.
    const auto deadline = GetTickCount64() + 12000;
    while (!handoff.done.load(std::memory_order_acquire) && GetTickCount64() < deadline) _mm_pause();
    const bool completed_while_owner_runnable = handoff.done.load(std::memory_order_acquire);
    (void)WaitForSingleObject(worker, INFINITE); CHECK(CloseHandle(worker));
    CHECK(completed_while_owner_runnable && handoff.good); CHECK(source.Close() == IoStatus::Ready);
}
}
int main() {
    OwnerHandoff();
    SourceTest(false);
    SourceTest(true);
    DemuxIntegration(false);
    DemuxIntegration(true);
    std::printf("Windows IOCP: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
