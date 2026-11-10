#include "media/platform/SystemMemory.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <limits>

namespace media::platform {
bool ReserveCommitted(const std::size_t bytes, MemoryRegion& region,
                       std::uint32_t& os_error) noexcept {
    region = {};
    const long query = sysconf(_SC_PAGESIZE);
    if (query <= 0) { os_error = static_cast<std::uint32_t>(errno); return false; }
    const auto page = static_cast<std::size_t>(query);
    if (bytes == 0 || bytes > (std::numeric_limits<std::size_t>::max)() - (page - 1)) {
        os_error = EINVAL; return false;
    }
    const auto rounded = ((bytes + page - 1) / page) * page;
    void* address = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) { os_error = static_cast<std::uint32_t>(errno); return false; }
    region = {address, rounded, page};
    os_error = 0;
    return true;
}
bool ReleaseCommitted(const MemoryRegion region, std::uint32_t& os_error) noexcept {
    if (munmap(region.address, region.bytes) != 0) {
        os_error = static_cast<std::uint32_t>(errno); return false;
    }
    os_error = 0;
    return true;
}
}
