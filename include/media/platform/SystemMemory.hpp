#pragma once
#include <cstddef>
#include <cstdint>

namespace media::platform {
struct MemoryRegion {
    void* address = nullptr;
    std::size_t bytes = 0;
    std::size_t page_bytes = 0;
};
// Startup/teardown only. Exactly one native implementation is linked.
[[nodiscard]] bool ReserveCommitted(std::size_t bytes, MemoryRegion& region,
                                     std::uint32_t& os_error) noexcept;
[[nodiscard]] bool ReleaseCommitted(MemoryRegion region, std::uint32_t& os_error) noexcept;
}
