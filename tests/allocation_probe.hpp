#pragma once
#include <cstdint>
namespace allocation_probe {
void Begin() noexcept;
[[nodiscard]] std::uint64_t End() noexcept;
}
