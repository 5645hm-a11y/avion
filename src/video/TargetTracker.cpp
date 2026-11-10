#include "media/video/TargetTracker.hpp"
#include <algorithm>
#include <cmath>

namespace media::video {
namespace {
constexpr unsigned kMaxGridWidth = 480, kMaxGridHeight = 272;
}
unsigned GridScale(unsigned width, unsigned height) noexcept {
    const unsigned sx = (width + kMaxGridWidth - 1) / kMaxGridWidth, sy = (height + kMaxGridHeight - 1) / kMaxGridHeight;
    return (std::max)({1u, sx, sy});
}
void MakeGrid(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, unsigned scale, std::uint8_t* out, unsigned out_stride) noexcept {
    if (!luma || !out || !scale) return;
    const unsigned gw = (width + scale - 1) / scale, gh = (height + scale - 1) / scale;
    for (unsigned gy = 0; gy < gh; ++gy)
        for (unsigned gx = 0; gx < gw; ++gx) {
            const unsigned x0 = gx * scale, y0 = gy * scale, x1 = (std::min)(width, x0 + scale), y1 = (std::min)(height, y0 + scale);
            unsigned sum = 0;
            for (unsigned y = y0; y < y1; ++y) for (unsigned x = x0; x < x1; ++x) sum += luma[y * stride + x];
            const unsigned count = (x1 - x0) * (y1 - y0);
            out[gy * out_stride + gx] = static_cast<std::uint8_t>((sum + count / 2) / count);
        }
}
// The weighted mean of the patch whose top-left cell is (left, top) (it fits).
float TargetTracker::Mean(const LumaGrid& grid, int left, int top) const noexcept {
    float sum = 0;
    for (unsigned y = 0; y < kPatch; ++y) {
        const auto* row = grid.cells + static_cast<unsigned>(top + static_cast<int>(y)) * grid.stride + static_cast<unsigned>(left);
        for (unsigned x = 0; x < kPatch; ++x) sum += weight_[y * kPatch + x] * row[x];
    }
    return sum / weight_sum_;
}
float TargetTracker::Match(const LumaGrid& grid, int left, int top) const noexcept {
    if (left < 0 || top < 0 || left + static_cast<int>(kPatch) > static_cast<int>(grid.width) || top + static_cast<int>(kPatch) > static_cast<int>(grid.height)) return -2;
    const float mean = Mean(grid, left, top);
    float dot = 0, energy = 0;
    for (unsigned y = 0; y < kPatch; ++y) {
        const auto* row = grid.cells + static_cast<unsigned>(top + static_cast<int>(y)) * grid.stride + static_cast<unsigned>(left);
        for (unsigned x = 0; x < kPatch; ++x) {
            const float w = weight_[y * kPatch + x], p = row[x] - mean;
            dot += w * p * template_[y * kPatch + x]; energy += w * p * p;
        }
    }
    return energy > 1e-3f ? dot / (norm_ * std::sqrt(energy)) : 0.0f;
}
// The template from the patch at (left, top): its weighted zero-mean samples (blended into the old ones by `keep`) and norm.
void TargetTracker::Take(const LumaGrid& grid, int left, int top, float keep) noexcept {
    const float mean = Mean(grid, left, top);
    float energy = 0;
    for (unsigned j = 0; j < kPatch; ++j)
        for (unsigned i = 0; i < kPatch; ++i) {
            auto& t = template_[j * kPatch + i];
            t = keep * t + (1 - keep) * (grid.cells[static_cast<unsigned>(top + static_cast<int>(j)) * grid.stride + static_cast<unsigned>(left) + i] - mean);
            energy += weight_[j * kPatch + i] * t * t;
        }
    norm_ = std::sqrt(energy);
}
bool TargetTracker::Lock(const LumaGrid& grid, float x, float y) noexcept {
    status_ = {};
    if (!grid.cells || grid.width < kPatch || grid.height < kPatch) return false;
    // The patch centred on the point, kept inside the grid.
    const float cx = x * static_cast<float>(grid.width), cy = y * static_cast<float>(grid.height);
    const int left = std::clamp(static_cast<int>(std::lround(cx - kPatch / 2.0f)), 0, static_cast<int>(grid.width - kPatch));
    const int top = std::clamp(static_cast<int>(std::lround(cy - kPatch / 2.0f)), 0, static_cast<int>(grid.height - kPatch));
    // A raised-cosine window over the patch: the target in its middle counts, the background at its edges hardly does.
    weight_sum_ = 0;
    for (unsigned j = 0; j < kPatch; ++j)
        for (unsigned i = 0; i < kPatch; ++i) {
            const auto h = [](unsigned k) { return 0.5f - 0.5f * std::cos(6.2831853f * (static_cast<float>(k) + 0.5f) / static_cast<float>(kPatch)); };
            weight_[j * kPatch + i] = h(i) * h(j); weight_sum_ += weight_[j * kPatch + i];
        }
    for (auto& t : template_) t = 0;
    Take(grid, left, top, 0);
    if (norm_ / std::sqrt(weight_sum_) < kMinTexture) return false; // Nothing to follow.
    cx_ = static_cast<float>(left) + kPatch / 2.0f; cy_ = static_cast<float>(top) + kPatch / 2.0f; vx_ = vy_ = 0;
    grid_width_ = grid.width; grid_height_ = grid.height;
    status_.state = TrackState::Tracking; status_.confidence = 1;
    status_.x = cx_ / static_cast<float>(grid.width); status_.y = cy_ / static_cast<float>(grid.height);
    return true;
}
TrackStatus TargetTracker::Update(const LumaGrid& grid, bool scene_cut) noexcept {
    status_.reacquired = false;
    if (status_.state == TrackState::Off) return status_;
    status_.vx = status_.vy = 0; // Set again by a match.
    if (!grid.cells || grid.width != grid_width_ || grid.height != grid_height_) { // Another picture size: nothing to compare.
        status_.state = TrackState::Lost; ++status_.lost; vx_ = vy_ = 0; return status_;
    }
    if (scene_cut) { status_.state = TrackState::Lost; ++status_.lost; status_.confidence = 0; vx_ = vy_ = 0; return status_; }
    const bool lost = status_.state == TrackState::Lost;
    const float px = cx_ + vx_, py = cy_ + vy_;
    const int radius = static_cast<int>(lost ? 2 * kSearch : kSearch);
    const int base_left = static_cast<int>(std::lround(px - kPatch / 2.0f)), base_top = static_cast<int>(std::lround(py - kPatch / 2.0f));
    // Every offset's score, kept for the refinement around the best one.
    constexpr int kSide = 4 * static_cast<int>(kSearch) + 1;
    float scores[kSide * kSide];
    int best_dx = 0, best_dy = 0; float best = -2;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const float s = Match(grid, base_left + dx, base_top + dy);
            scores[(dy + 2 * static_cast<int>(kSearch)) * kSide + dx + 2 * static_cast<int>(kSearch)] = s;
            if (s > best) { best = s; best_dx = dx; best_dy = dy; }
        }
    status_.confidence = best < -1 ? 0 : best;
    if (best < (lost ? kReacquire : kAccept)) {
        status_.state = TrackState::Lost; ++status_.lost; vx_ = vy_ = 0; // Hold: the estimate stays where the target was.
        return status_;
    }
    // A parabola through the best score and its neighbours, per axis (a neighbour outside the search keeps the whole cell).
    const auto at = [&](int dx, int dy) {
        if (dx < -radius || dx > radius || dy < -radius || dy > radius) return -2.0f;
        return scores[(dy + 2 * static_cast<int>(kSearch)) * kSide + dx + 2 * static_cast<int>(kSearch)];
    };
    const auto vertex = [](float minus, float centre, float plus) {
        if (minus < -1 || plus < -1) return 0.0f;
        const float d = minus - 2 * centre + plus;
        return d < -1e-6f ? std::clamp(0.5f * (minus - plus) / d, -0.5f, 0.5f) : 0.0f;
    };
    const float fx = vertex(at(best_dx - 1, best_dy), best, at(best_dx + 1, best_dy));
    const float fy = vertex(at(best_dx, best_dy - 1), best, at(best_dx, best_dy + 1));
    const float zx = static_cast<float>(base_left + best_dx) + fx + kPatch / 2.0f, zy = static_cast<float>(base_top + best_dy) + fy + kPatch / 2.0f;
    if (lost) { cx_ = zx; cy_ = zy; vx_ = vy_ = 0; status_.reacquired = true; } // Taken back where it is found: no inherited motion.
    else {
        const float rx = zx - px, ry = zy - py;
        cx_ = px + kAlpha * rx; cy_ = py + kAlpha * ry; vx_ += kBeta * rx; vy_ += kBeta * ry;
    }
    // A clear match refreshes the template a little (the patch at the whole-cell best).
    if (best >= kRefresh) {
        Take(grid, base_left + best_dx, base_top + best_dy, 0.9f);
    }
    status_.state = TrackState::Tracking; status_.lost = 0;
    status_.match_x = std::clamp(zx / static_cast<float>(grid.width), 0.0f, 1.0f); status_.match_y = std::clamp(zy / static_cast<float>(grid.height), 0.0f, 1.0f);
    status_.x = std::clamp(cx_ / static_cast<float>(grid.width), 0.0f, 1.0f); status_.y = std::clamp(cy_ / static_cast<float>(grid.height), 0.0f, 1.0f);
    status_.vx = vx_ / static_cast<float>(grid.width); status_.vy = vy_ / static_cast<float>(grid.height);
    return status_;
}
}
