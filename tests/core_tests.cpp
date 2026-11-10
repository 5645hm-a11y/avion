#include "media/core/linear_memory_arena.hpp"
#include "media/core/media_descriptors.hpp"
#include "media/core/spsc_ring_buffer.hpp"

#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <limits>

using namespace media::core;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #expression); ++failures; \
} } while (false)

void TestArena() {
    LinearMemoryArena arena;
    CHECK(!arena.Seal());
    CHECK(arena.Allocate(1) == nullptr);
    CHECK(arena.LastError() == ArenaError::NotInitialized);
    CHECK(!arena.Initialize(0));
    CHECK(!arena.Initialize((std::numeric_limits<std::size_t>::max)()));
    CHECK(arena.Initialize(257));
    CHECK(!arena.Initialize(4096));
    CHECK(arena.LastError() == ArenaError::AlreadyInitialized);
    auto* first = static_cast<unsigned char*>(arena.Allocate(1, 1));
    auto* second = static_cast<unsigned char*>(arena.Allocate(65));
    CHECK(first != nullptr && second != nullptr);
    if (first == nullptr || second == nullptr) return;
    CHECK(reinterpret_cast<std::uintptr_t>(first) % 64 == 0);
    CHECK(second - first == 64);
    CHECK(*first == 0 && second[64] == 0);
    *first = 91;
    second[64] = 73;
    CHECK(arena.Allocate(0) == nullptr);
    CHECK(arena.Allocate(1, 0) == nullptr);
    CHECK(arena.Allocate(1, 3) == nullptr);
    CHECK(arena.Allocate((std::numeric_limits<std::size_t>::max)()) == nullptr);
    CHECK(arena.LastError() == ArenaError::OutOfMemory);
    auto* third = static_cast<unsigned char*>(arena.Allocate(65));
    CHECK(third == first + 192);
    CHECK(arena.Allocate(1) == nullptr);
    auto stats = arena.Statistics();
    CHECK(stats.capacity_bytes == 257 && stats.committed_bytes >= 257);
    CHECK(stats.used_bytes == 257 && stats.requested_bytes == 131);
    CHECK(stats.padding_bytes == 126 && stats.high_water_bytes == 257);
    CHECK(stats.allocation_count == 3 && stats.failed_allocation_count == 5);
    CHECK(*first == 91 && second[64] == 73);
    CHECK(arena.Seal() && arena.Seal());
    CHECK(arena.Allocate(1) == nullptr && arena.LastError() == ArenaError::Sealed);
    CHECK(arena.Shutdown() && arena.Shutdown());
    CHECK(arena.Statistics().capacity_bytes == 0 && !arena.IsSealed());

    CHECK(arena.Initialize(1024 * 1024));
    void* start = arena.Allocate(1);
    CHECK(start != nullptr);
    auto* aligned = arena.Allocate(128, 256 * 1024);
    CHECK(aligned != nullptr && reinterpret_cast<std::uintptr_t>(aligned) % (256 * 1024) == 0);
    auto* packet = arena.Create<MediaPacket>();
    CHECK(packet != nullptr && packet->payload_bytes == 0);
    CHECK(arena.Seal());
    CHECK(arena.Create<MediaPacket>() == nullptr);
    // Reset (a warm session's next file): empty and unsealed again, the same memory from its start, the capacity kept.
    const auto committed = arena.Statistics().committed_bytes;
    auto* again = arena.Create<MediaPacket>();
    CHECK(again == nullptr && arena.Reset() && !arena.IsSealed());
    const auto rewound = arena.Statistics();
    CHECK(rewound.used_bytes == 0 && rewound.allocation_count == 0 && rewound.capacity_bytes == 1024 * 1024 && rewound.committed_bytes == committed);
    auto* first_again = arena.Allocate(1);
    CHECK(first_again == start);
    CHECK(arena.Allocate(1024 * 1024 - 64) != nullptr && arena.Allocate(1) == nullptr); // The whole capacity again.
    CHECK(arena.Shutdown() && !arena.Reset() && arena.LastError() == ArenaError::NotInitialized);
}

template <std::size_t Capacity>
void TestQueueContract() {
    LinearMemoryArena arena;
    using Queue = SpscRingBuffer<std::uint64_t, Capacity>;
    CHECK(arena.Initialize(sizeof(Queue)));
    auto* queue = arena.Create<Queue>((std::numeric_limits<std::uint64_t>::max)() - 2);
    CHECK(queue != nullptr);
    if (queue == nullptr) return;
    CHECK(arena.Seal());
    CHECK(queue->GetCapacity() == Capacity);
    CHECK(queue->PeekReadSlot() == nullptr);
    CHECK(!queue->ReleaseReadSlot() && !queue->CommitWriteSlot());
    CHECK(!queue->CancelWriteSlot());
    auto* canceled = queue->AcquireWriteSlot();
    CHECK(canceled != nullptr);
    *canceled = 42;
    CHECK(queue->AcquireWriteSlot() == canceled);
    CHECK(queue->PeekReadSlot() == nullptr); // Checkout never publishes.
    CHECK(queue->CancelWriteSlot());
    CHECK(!queue->CommitWriteSlot());
    CHECK(queue->PeekReadSlot() == nullptr);

    for (std::uint64_t round = 0; round < 100; ++round) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            auto* write = queue->AcquireWriteSlot();
            CHECK(write != nullptr);
            if (write == nullptr) return;
            CHECK(reinterpret_cast<std::uintptr_t>(write) % 64 == 0);
            *write = round * Capacity + i;
            CHECK(queue->CommitWriteSlot());
            CHECK(!queue->CommitWriteSlot());
        }
        CHECK(queue->AcquireWriteSlot() == nullptr);
        const auto* held = queue->PeekReadSlot();
        CHECK(held != nullptr && queue->PeekReadSlot() == held);
        CHECK(queue->AcquireWriteSlot() == nullptr); // Peek does not recycle.
        for (std::size_t i = 0; i < Capacity; ++i) {
            const auto* read = queue->PeekReadSlot();
            CHECK(read != nullptr);
            if (read == nullptr) return;
            CHECK(*read == round * Capacity + i);
            CHECK(queue->ReleaseReadSlot());
            CHECK(!queue->ReleaseReadSlot());
        }
        CHECK(queue->PeekReadSlot() == nullptr);
    }
}

struct alignas(128) OverAligned { std::uint64_t words[16]; };
void TestOverAlignment() {
    LinearMemoryArena arena;
    using Queue = SpscRingBuffer<OverAligned, 2>;
    CHECK(arena.Initialize(sizeof(Queue) + 128));
    auto* queue = arena.Create<Queue>();
    CHECK(queue != nullptr);
    if (queue == nullptr) return;
    for (int i = 0; i < 2; ++i) {
        auto* slot = queue->AcquireWriteSlot();
        CHECK(slot != nullptr && reinterpret_cast<std::uintptr_t>(slot) % 128 == 0);
        CHECK(queue->CommitWriteSlot());
    }
}

void TestDescriptorTransport() {
    using Packets = SpscRingBuffer<MediaPacket, 2>;
    using Frames = SpscRingBuffer<VideoFrameDescriptor, 2>;
    LinearMemoryArena arena;
    CHECK(arena.Initialize(sizeof(Packets) + sizeof(Frames)));
    auto* packets = arena.Create<Packets>();
    auto* frames = arena.Create<Frames>();
    CHECK(packets != nullptr && frames != nullptr);
    if (packets == nullptr || frames == nullptr) return;
    CHECK(arena.Seal());
    auto* packet = packets->AcquireWriteSlot();
    CHECK(packet != nullptr);
    if (packet == nullptr) return;
    *packet = {};
    packet->pts_100ns = -100000;
    packet->dts_100ns = kUnknownTimestamp;
    packet->duration_100ns = 400000;
    packet->payload_offset = std::uint64_t{1} << 33;
    packet->payload_bytes = 4096;
    packet->payload_block_id = 9;
    packet->payload_generation = 23;
    packet->stream_id = 2;
    packet->playback_epoch = 17;
    packet->codec_config_id = 3;
    packet->flags = PacketFlags::KeyFrame;
    CHECK(packets->CommitWriteSlot());
    const auto* read_packet = packets->PeekReadSlot();
    CHECK(read_packet != nullptr);
    if (read_packet == nullptr) return;
    CHECK(read_packet->pts_100ns == -100000 && read_packet->dts_100ns == kUnknownTimestamp);
    CHECK(read_packet->payload_offset == (std::uint64_t{1} << 33));
    CHECK(read_packet->payload_generation == 23 && read_packet->playback_epoch == 17);
    CHECK(read_packet->flags == PacketFlags::KeyFrame && read_packet->reserved == 0);

    auto* frame = frames->AcquireWriteSlot();
    CHECK(frame != nullptr);
    if (frame == nullptr) return;
    *frame = {};
    frame->pts_100ns = read_packet->pts_100ns;
    frame->duration_100ns = read_packet->duration_100ns;
    frame->playback_epoch = read_packet->playback_epoch;
    frame->ready_fence_value = (std::uint64_t{1} << 40) + 7;
    frame->ready_fence_id = 2;
    frame->surface_id = 31;
    frame->surface_generation = 77;
    frame->color_metadata_id = 4;
    frame->coded_width = frame->crop_right = 3840;
    frame->coded_height = frame->crop_bottom = 2160;
    frame->format = VideoSurfaceFormat::P010;
    CHECK(packets->ReleaseReadSlot());
    CHECK(frames->CommitWriteSlot());
    const auto* read_frame = frames->PeekReadSlot();
    CHECK(read_frame != nullptr);
    if (read_frame == nullptr) return;
    CHECK(read_frame->pts_100ns == -100000 && read_frame->playback_epoch == 17);
    CHECK(read_frame->ready_fence_value == (std::uint64_t{1} << 40) + 7);
    CHECK(read_frame->surface_generation == 77 && read_frame->format == VideoSurfaceFormat::P010);
    CHECK(read_frame->crop_right == 3840 && read_frame->crop_bottom == 2160);
    CHECK(frames->ReleaseReadSlot());
    CHECK(arena.Statistics().allocation_count == 2);
}

struct StressValue { std::uint64_t words[8]; };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Isolate test coordination atomics on separate lines.
#endif
template <std::size_t Capacity>
struct StressContext {
    SpscRingBuffer<StressValue, Capacity>* queue;
    std::uint64_t count;
    std::uint64_t deadline_ms;
    alignas(64) std::atomic<bool> start{false};
    alignas(64) std::atomic<bool> abort{false};
    alignas(64) std::atomic<bool> bad{false};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <std::size_t Capacity>
DWORD WINAPI Produce(void* opaque) {
    auto& context = *static_cast<StressContext<Capacity>*>(opaque);
    while (!context.start.load(std::memory_order_acquire)) _mm_pause();
    std::uint32_t polls = 0;
    for (std::uint64_t i = 0; i < context.count; ++i) {
        StressValue* slot = nullptr;
        while ((slot = context.queue->AcquireWriteSlot()) == nullptr) {
            if (context.abort.load(std::memory_order_relaxed)) return 1;
            if ((++polls & 16383u) == 0 && GetTickCount64() > context.deadline_ms) {
                context.abort.store(true, std::memory_order_relaxed);
                return 2;
            }
            _mm_pause();
        }
        for (std::uint64_t lane = 0; lane < 8; ++lane)
            slot->words[lane] = (i * 0x9e3779b97f4a7c15ull) ^ (lane * 0xd6e8feb86659fd93ull);
        if ((i & 1023u) == 0) for (int delay = 0; delay < 32; ++delay) _mm_pause();
        if (!context.queue->CommitWriteSlot()) context.bad.store(true);
    }
    return 0;
}

template <std::size_t Capacity>
DWORD WINAPI Consume(void* opaque) {
    auto& context = *static_cast<StressContext<Capacity>*>(opaque);
    while (!context.start.load(std::memory_order_acquire)) _mm_pause();
    std::uint32_t polls = 0;
    for (std::uint64_t i = 0; i < context.count; ++i) {
        const StressValue* slot = nullptr;
        while ((slot = context.queue->PeekReadSlot()) == nullptr) {
            if (context.abort.load(std::memory_order_relaxed)) return 1;
            if ((++polls & 16383u) == 0 && GetTickCount64() > context.deadline_ms) {
                context.abort.store(true, std::memory_order_relaxed);
                return 2;
            }
            _mm_pause();
        }
        if ((i & 511u) == 0) for (int delay = 0; delay < 32; ++delay) _mm_pause();
        for (std::uint64_t lane = 0; lane < 8; ++lane) {
            const auto expected = (i * 0x9e3779b97f4a7c15ull) ^ (lane * 0xd6e8feb86659fd93ull);
            if (slot->words[lane] != expected) context.bad.store(true);
        }
        if (!context.queue->ReleaseReadSlot()) context.bad.store(true);
    }
    return 0;
}

template <std::size_t Capacity>
void TestConcurrent(const std::uint64_t count) {
    LinearMemoryArena arena;
    using Queue = SpscRingBuffer<StressValue, Capacity>;
    CHECK(arena.Initialize(sizeof(Queue)));
    auto* queue = arena.Create<Queue>((std::numeric_limits<std::uint64_t>::max)() - 1000);
    CHECK(queue != nullptr);
    if (queue == nullptr) return;
    CHECK(arena.Seal());
    StressContext<Capacity> context{queue, count, GetTickCount64() + 20000};
    HANDLE threads[2]{CreateThread(nullptr, 0, Produce<Capacity>, &context, 0, nullptr),
                      CreateThread(nullptr, 0, Consume<Capacity>, &context, 0, nullptr)};
    CHECK(threads[0] != nullptr && threads[1] != nullptr);
    if (threads[0] == nullptr || threads[1] == nullptr) context.abort.store(true);
    context.start.store(true, std::memory_order_release);
    for (HANDLE thread : threads) {
        if (thread == nullptr) continue;
        // Test harness only. Queue methods never call any wait API.
        CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        DWORD result = 1;
        CHECK(GetExitCodeThread(thread, &result) != 0 && result == 0);
        CHECK(CloseHandle(thread) != 0);
    }
    CHECK(!context.abort.load() && !context.bad.load());
    CHECK(queue->PeekReadSlot() == nullptr);
    CHECK(arena.Statistics().allocation_count == 1);
    std::printf("SPSC capacity %zu: %llu transfers verified\n", Capacity,
                static_cast<unsigned long long>(count));
}
} // namespace

int main() {
    TestArena();
    TestQueueContract<1>();
    TestQueueContract<2>();
    TestQueueContract<8>();
    TestOverAlignment();
    TestDescriptorTransport();
    TestConcurrent<1>(200000);
    TestConcurrent<8>(1000000);
    TestConcurrent<1024>(2000000);
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
