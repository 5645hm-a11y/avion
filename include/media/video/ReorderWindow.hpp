#pragma once
#include <cstdint>

namespace media::video {
enum class ReorderInsert : std::uint8_t { Inserted, Full, Duplicate, Late };

// Presentation reorder window: frames enter in decode order and leave in presentation (PTS) order.
// Release follows the H.265 C.5.2 "bumping" rule: the smallest PTS may leave once more than
// `depth` (sps_max_num_reorder_pics) frames wait, or at any time while draining. With that many
// successors decoded, no later frame can precede it in output order, so the front is final.
// Storage is a fixed sorted ring of {pts, slot} records; insertion shifts at most Capacity entries.
// Single owner (the presentation thread). No allocation, no locks.
template <std::uint32_t Capacity>
class ReorderWindow final {
    static_assert(Capacity >= 2 && Capacity <= 64 && (Capacity & (Capacity - 1)) == 0);
public:
    struct Entry { std::int64_t pts; std::uint32_t slot; };
    // depth + 1 entries must fit so that a frame can always be released without draining.
    [[nodiscard]] bool Configure(std::uint32_t depth) noexcept {
        if (count_ || depth + 1 >= Capacity) return false;
        depth_ = depth; released_ = false; return true;
    }
    [[nodiscard]] ReorderInsert Insert(std::int64_t pts, std::uint32_t slot) noexcept {
        if (released_ && pts <= last_released_) return ReorderInsert::Late; // Stream exceeded its declared depth.
        if (count_ == Capacity) return ReorderInsert::Full;
        std::uint32_t position = count_;
        while (position && At(position - 1).pts >= pts) {
            if (At(position - 1).pts == pts) return ReorderInsert::Duplicate;
            --position;
        }
        for (std::uint32_t i = count_; i > position; --i) At(i) = At(i - 1);
        At(position) = {pts, slot}; ++count_;
        return ReorderInsert::Inserted;
    }
    [[nodiscard]] bool Ready(bool draining) const noexcept { return count_ > depth_ || (draining && count_ != 0); }
    [[nodiscard]] const Entry* Front() const noexcept { return count_ ? &entries_[head_] : nullptr; }
    // Removes the front entry after it was presented or deliberately dropped.
    [[nodiscard]] bool Pop() noexcept {
        if (!count_) return false;
        last_released_ = entries_[head_].pts; released_ = true;
        head_ = (head_ + 1) & (Capacity - 1); --count_; return true;
    }
    // Seek flush: forgets every entry and the release history, so an earlier timeline may enter again.
    void Clear() noexcept { head_ = count_ = 0; released_ = false; }
    [[nodiscard]] std::uint32_t Size() const noexcept { return count_; }
    [[nodiscard]] std::uint32_t Depth() const noexcept { return depth_; }
    // The last PTS that left the window (INT64_MIN when none has since the last Clear/Configure).
    [[nodiscard]] std::int64_t LastReleased() const noexcept { return released_ ? last_released_ : INT64_MIN; }
private:
    Entry& At(std::uint32_t index) noexcept { return entries_[(head_ + index) & (Capacity - 1)]; }
    Entry entries_[Capacity]{};
    std::int64_t last_released_ = 0;
    std::uint32_t head_ = 0, count_ = 0, depth_ = 0;
    bool released_ = false;
};
}
