#include "media/platform/SystemMemory.hpp"
#include <Windows.h>
#include <limits>

namespace media::platform {
bool ReserveCommitted(const std::size_t bytes, MemoryRegion& region,
                       std::uint32_t& os_error) noexcept {
    region = {};
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::size_t>(info.dwPageSize);
    if (bytes == 0 || bytes > (std::numeric_limits<std::size_t>::max)() - (page - 1)) {
        os_error = ERROR_INVALID_PARAMETER; return false;
    }
    const auto rounded = ((bytes + page - 1) / page) * page;
    void* address = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (address == nullptr) { os_error = GetLastError(); return false; }
    region = {address, rounded, page};
    os_error = 0;
    return true;
}
bool ReleaseCommitted(const MemoryRegion region, std::uint32_t& os_error) noexcept {
    if (!VirtualFree(region.address, 0, MEM_RELEASE)) { os_error = GetLastError(); return false; }
    os_error = 0;
    return true;
}
}
