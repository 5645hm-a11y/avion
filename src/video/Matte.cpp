#include "media/video/Matte.hpp"
#include <algorithm>

namespace media::video {
void Matte::Accumulate(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t words[kWords], unsigned black,
                       unsigned step) noexcept {
    if (!luma || !words || !width || !height || width > 65535 || height > 65535 || !step) return;
    // The shader's comparison, code / 255 > black / 255 + kLift in float, as the least code that passes it (the same answer for
    // every code).
    const float threshold = static_cast<float>(black) / 255.0f + kLift;
    unsigned least = 256;
    for (unsigned c = 256; c-- > 0;) if (static_cast<float>(c) / 255.0f > threshold) least = c;
    // The words are the first and last even row (column) with a bright sample among the sampled columns (rows): searched from
    // each end, they stop at the picture's edge (2026-10-31; every even row and column before: the same words, a few samples
    // read instead of a thousand lines).
    unsigned xs[kLines], ys[kLines];
    const unsigned columns = Lines(width), rows = Lines(height);
    for (unsigned i = 0; i < columns; ++i) xs[i] = Sample(i, columns, width) * step;
    for (unsigned j = 0; j < rows; ++j) ys[j] = Sample(j, rows, height);
    const auto row = [&](unsigned y) {
        const std::uint8_t* line = luma + static_cast<std::size_t>(y) * stride;
        for (unsigned i = 0; i < columns; ++i) if (line[xs[i]] >= least) return true;
        return false;
    };
    const auto column = [&](unsigned x) {
        const std::uint8_t* at = luma + static_cast<std::size_t>(x) * step;
        for (unsigned j = 0; j < rows; ++j) if (at[static_cast<std::size_t>(ys[j]) * stride] >= least) return true;
        return false;
    };
    const unsigned last_y = (height - 1) & ~1u, last_x = (width - 1) & ~1u;
    for (unsigned y = 0; y <= last_y; y += 2)
        if (row(y)) {
            words[0] = (std::max)(words[0], 65536u - y);
            for (unsigned z = last_y;; z -= 2) if (row(z)) { words[1] = (std::max)(words[1], z + 1); break; } // Found at y at the latest.
            break;
        }
    for (unsigned x = 0; x <= last_x; x += 2)
        if (column(x)) {
            words[2] = (std::max)(words[2], 65536u - x);
            for (unsigned z = last_x;; z -= 2) if (column(z)) { words[3] = (std::max)(words[3], z + 1); break; }
            break;
        }
}
bool Matte::Extents(const std::uint32_t words[kWords], unsigned width, unsigned height, Rect& e) noexcept {
    e = {};
    if (!words || !words[0] || !words[1] || !words[2] || !words[3]) return false;
    e.top = 65536u - (std::min)(words[0], 65536u); e.bottom = (std::min)(words[1], height);
    e.left = 65536u - (std::min)(words[2], 65536u); e.right = (std::min)(words[3], width);
    return e.top < e.bottom && e.left < e.right;
}
void Matte::Reset(unsigned width, unsigned height) noexcept {
    for (auto& s : sides_) s = {};
    width_ = width; height_ = height; changes_ = pictures_ = 0; locked_ = false;
}
void Matte::Push(std::int64_t pts, const std::uint32_t words[kWords]) noexcept {
    Rect e;
    if (!width_ || !height_ || !Extents(words, width_, height_, e)) return; // A black picture says nothing about bars.
    ++pictures_;
    const std::uint32_t thickness[4]{e.left, e.top, width_ - e.right, height_ - e.bottom};
    bool changed = false, all = true;
    for (unsigned k = 0; k < 4; ++k) {
        auto& s = sides_[k];
        const std::uint32_t t = thickness[k];
        // The run of pictures agreeing on one thickness (within kTolerance of where it started).
        if (s.run && (t > s.candidate ? t - s.candidate : s.candidate - t) <= kTolerance) { ++s.run; if (pts < s.since) s.since = pts; } // A seek back.
        else { s.candidate = t; s.run = 1; s.since = pts; }
        if (s.measured && t + kTolerance < s.bar) { s.bar = t; changed = true; } // Picture inside the bar: thinner at once.
        else if (s.run >= kMinPictures && pts - s.since >= static_cast<std::int64_t>(kHold * 1e7f) && (!s.measured || s.candidate > s.bar + kTolerance)) {
            const std::uint32_t bar = (std::min)(s.candidate, t); // The thinner of the run's ends.
            if (!s.measured || bar != s.bar) { s.bar = bar; changed = true; }
            s.measured = true;
        }
        all = all && s.measured;
    }
    if (changed && all) ++changes_;
    if (all && !locked_) { locked_ = true; ++changes_; }
}
Matte::Rect Matte::Active() const noexcept {
    if (!locked_) return {0, 0, width_, height_};
    const std::uint32_t left = (std::min)(sides_[0].bar, width_ / 2), top = (std::min)(sides_[1].bar, height_ / 2);
    const std::uint32_t right = width_ - (std::min)(sides_[2].bar, width_ - left - 1), bottom = height_ - (std::min)(sides_[3].bar, height_ - top - 1);
    return {left, top, right, bottom};
}
bool Matte::Bars() const noexcept {
    if (!locked_) return false;
    const auto r = Active();
    const auto bar = [](std::uint32_t thickness, unsigned size) { return thickness > kTolerance && thickness * 100 >= size; };
    return bar(r.left, width_) || bar(width_ - r.right, width_) || bar(r.top, height_) || bar(height_ - r.bottom, height_);
}
}
