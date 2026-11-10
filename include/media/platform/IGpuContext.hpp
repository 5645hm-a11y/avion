#pragma once
#include <cstdint>
#include <span>

namespace media::platform {
struct SurfaceHandle { std::uint32_t id; std::uint32_t generation; };
struct TimelinePoint { std::uint32_t timeline; std::uint64_t value; };
enum class GpuStatus : std::uint8_t { Submitted, WouldBlock, InvalidArgument, DeviceLost, Failed };
struct ComputeSubmission {
    std::uint32_t prepared_kernel_id;
    std::span<const SurfaceHandle> inputs;
    std::span<const SurfaceHandle> outputs;
    std::span<const TimelinePoint> dependencies;
    std::uint32_t groups_x, groups_y, groups_z;
};
// Handles reference precreated resources/pipelines. Implementations consume span
// metadata before returning; resources stay leased until the returned fence retires.
class IGpuContext {
public:
    virtual ~IGpuContext() = default;
    [[nodiscard]] virtual GpuStatus TrySubmit(const ComputeSubmission&, TimelinePoint& completion) noexcept = 0;
    [[nodiscard]] virtual GpuStatus TryPresent(SurfaceHandle, TimelinePoint ready,
                                              TimelinePoint& retirement) noexcept = 0;
    [[nodiscard]] virtual bool IsComplete(TimelinePoint) const noexcept = 0;
};
}
