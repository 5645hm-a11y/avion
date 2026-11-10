#pragma once
#include <cstdint>

namespace media::video {
// Baked-in black bars (docs/picture.md, "Black bars", 2026-10-30), portable, no allocation: a film's letterbox (top and bottom),
// a 4:3 programme's pillarbox (left and right) or both (windowbox), encoded in the picture. One picture's evidence comes from
// the statistics pass (HdrToneMap.hlsl Matte, every picture, no pass of its own) or the same arithmetic on the CPU
// (Accumulate: host mode, live sensors, tests): kLines columns across the crop are read at every other row and kLines rows at
// every other column (even ones: an edge is found at most a sample inside the picture, never in the bar), and a sample
// brighter than black by more than kLift is picture. The picture's extents (its first and last rows and
// columns with picture in them) are four words, each the largest of what the threads found: 65536 - the first row, the last
// row + 1, 65536 - the first column, the last column + 1 (all zero: no picture at all, a black frame).
// The bars are what stays black: Push keeps, for each side, the bar's thickness seen in the recent pictures:
// - thinner at once: a picture with picture inside the bar proves the bar is not that thick (the active area grows: content
//   is never cropped away, burned-in subtitles in a bar included);
// - thicker (and the first time) only when the pictures have agreed on one thickness, within kTolerance, for kHold seconds of
//   presentation time: a bar is constant, while a dark scene's edges move from picture to picture.
// A black picture is no evidence. Rows and columns are of the crop (the visible picture), right and bottom exclusive.
class Matte final {
public:
    static constexpr unsigned kLines = 32;       // Sampled columns (for the rows) and rows (for the columns).
    static constexpr float kLift = 0.03f;        // Above black, of full scale: about 8 codes of 8-bit video.
    static constexpr unsigned kTolerance = 2;    // Pixels: one thickness.
    static constexpr float kHold = 1.0f;         // Seconds of agreement before a bar is believed (or grows).
    static constexpr unsigned kMinPictures = 8;  // And at least this many pictures.
    static constexpr unsigned kWords = 4;
    struct Rect { std::uint32_t left = 0, top = 0, right = 0, bottom = 0; };
    // The sampling both sides use: `lines` columns (rows) over `size` samples, the i-th at Sample(i, lines, size).
    [[nodiscard]] static unsigned Lines(unsigned size) noexcept { const unsigned blocks = (size + 7) / 8; return blocks < kLines ? blocks : kLines; }
    [[nodiscard]] static unsigned Sample(unsigned i, unsigned lines, unsigned size) noexcept { return static_cast<unsigned>((2ull * i + 1) * size / (2ull * lines)); }
    // One picture's words on the CPU, as the shader makes them: 8-bit luma, `step` bytes apart in a row; `black` the code of
    // black (16: limited range, 0: full). Added into `words` with max (start from zeros).
    static void Accumulate(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t words[kWords],
                           unsigned black = 16, unsigned step = 1) noexcept;
    // The extents a picture's words describe in a width x height crop; false for a black picture.
    [[nodiscard]] static bool Extents(const std::uint32_t words[kWords], unsigned width, unsigned height, Rect& extents) noexcept;
    void Reset(unsigned width, unsigned height) noexcept;
    void Push(std::int64_t pts_100ns, const std::uint32_t words[kWords]) noexcept;
    [[nodiscard]] bool Locked() const noexcept { return locked_; }              // Every side has been measured.
    [[nodiscard]] Rect Active() const noexcept;                                  // The picture inside the bars.
    [[nodiscard]] bool Bars() const noexcept;                                    // Locked, and a bar of at least 1 % of its side.
    [[nodiscard]] std::uint32_t Changes() const noexcept { return changes_; }  // The active area changed (re-crop).
    [[nodiscard]] std::uint32_t Pictures() const noexcept { return pictures_; } // Pushed with picture in them.
    [[nodiscard]] unsigned Width() const noexcept { return width_; }
    [[nodiscard]] unsigned Height() const noexcept { return height_; }
private:
    struct Side { std::uint32_t bar = 0, candidate = 0, run = 0; std::int64_t since = 0; bool measured = false; };
    Side sides_[4]{}; // Left, top, right, bottom: thicknesses.
    unsigned width_ = 0, height_ = 0;
    std::uint32_t changes_ = 0, pictures_ = 0;
    bool locked_ = false;
};
}
