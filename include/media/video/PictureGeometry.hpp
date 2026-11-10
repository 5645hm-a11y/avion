#pragma once
#include <cstdint>

// Where a decoded picture's samples are and what shape they are shown in. Portable, constexpr, no storage.
// - The *crop* is the conformance window: the samples that are picture. Everything else in the coded surface is
//   padding to a macroblock or coding-block multiple, whose content the decoder does not define.
// - The *sample aspect ratio* (SAR) is the shape of one sample (width : height). Anamorphic video stores fewer
//   samples across than it shows: 720x576 with SAR 16:11 is a 16:9 picture.
// - The *display size* is the crop in square pixels: the shape the viewport must have.
namespace media::video {
struct SampleAspect {
    std::uint32_t width = 1, height = 1;
    constexpr bool operator==(const SampleAspect&) const noexcept = default;
    [[nodiscard]] constexpr bool Square() const noexcept { return width == height; }
};
struct PictureSize {
    std::uint32_t width = 0, height = 0;
    constexpr bool operator==(const PictureSize&) const noexcept = default;
};
struct PictureRect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    constexpr bool operator==(const PictureRect&) const noexcept = default;
};
// Largest picture side handled; also bounds every product below well inside 64 bits.
inline constexpr std::uint32_t kMaxPictureSide = 16384;
// A SAR further from square than this is taken for damage, not for a format (the widest in use is 2:1).
inline constexpr std::uint32_t kMaxSampleAspect = 8;

[[nodiscard]] constexpr std::uint32_t Gcd(std::uint32_t a, std::uint32_t b) noexcept {
    while (b) { const auto t = a % b; a = b; b = t; }
    return a;
}
// Lowest terms; {1, 1} for a zero term or a ratio beyond kMaxSampleAspect either way.
[[nodiscard]] constexpr SampleAspect Normalize(SampleAspect sar) noexcept {
    if (!sar.width || !sar.height) return {};
    const auto g = Gcd(sar.width, sar.height);
    sar.width /= g; sar.height /= g;
    const std::uint64_t w = sar.width, h = sar.height;
    if (w > h * kMaxSampleAspect || h > w * kMaxSampleAspect) return {};
    return sar;
}
[[nodiscard]] constexpr bool Specified(SampleAspect sar) noexcept { return sar.width != 0 && sar.height != 0; }
// aspect_ratio_idc of the H.264 (Table E-1) and HEVC (Table E.1) VUI. 255 (EXTENDED_SAR) takes the explicit pair.
// {0, 0} for 0 (unspecified), a reserved value, or an explicit pair with a zero term: the stream does not say.
[[nodiscard]] constexpr SampleAspect SampleAspectFromIdc(unsigned idc, unsigned sar_width = 0, unsigned sar_height = 0) noexcept {
    constexpr SampleAspect table[17]{{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
                                     {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}};
    if (idc == 255) return sar_width && sar_height ? SampleAspect{sar_width, sar_height} : SampleAspect{0, 0};
    return idc < 17 ? table[idc] : SampleAspect{0, 0};
}
// Which description to believe. A container that says something other than square wins (ISO/IEC 14496-12 `pasp`
// describes the track as authored, and remuxers correct wrong streams there); a `pasp` of 1:1 is what muxers write
// when they were told nothing, so the bitstream's VUI is heard then; with neither, samples are square.
enum class AspectSource : std::uint8_t { Square, Bitstream, Container };
[[nodiscard]] constexpr SampleAspect ResolveSampleAspect(SampleAspect container, SampleAspect bitstream, AspectSource* source = nullptr) noexcept {
    const auto c = Specified(container) ? Normalize(container) : SampleAspect{};
    const auto b = Specified(bitstream) ? Normalize(bitstream) : SampleAspect{};
    const auto chosen = !c.Square() ? AspectSource::Container : !b.Square() ? AspectSource::Bitstream : AspectSource::Square;
    if (source) *source = chosen;
    return chosen == AspectSource::Container ? c : chosen == AspectSource::Bitstream ? b : SampleAspect{};
}
// The crop in square pixels. No source sample is thrown away: a wide sample widens the picture, a tall one makes it
// taller, and the other side keeps its sample count. Rounded to nearest, at least 1, at most kMaxPictureSide.
[[nodiscard]] constexpr PictureSize DisplaySize(std::uint32_t crop_width, std::uint32_t crop_height, SampleAspect sar) noexcept {
    sar = Normalize(sar);
    const auto scale = [](std::uint32_t side, std::uint32_t numerator, std::uint32_t denominator) noexcept {
        const auto scaled = (static_cast<std::uint64_t>(side) * numerator + denominator / 2) / denominator;
        return static_cast<std::uint32_t>(scaled < 1 ? 1 : scaled > kMaxPictureSide ? kMaxPictureSide : scaled);
    };
    if (sar.width > sar.height) return {scale(crop_width, sar.width, sar.height), crop_height};
    if (sar.height > sar.width) return {crop_width, scale(crop_height, sar.height, sar.width)};
    return {crop_width, crop_height};
}
// The largest size of the shape `source` inside `bounds`. Within 2 px of a bound it snaps to the bound: a rounding
// sliver is not worth a black bar. A zero side anywhere returns the bounds.
[[nodiscard]] constexpr PictureSize FitInside(PictureSize source, PictureSize bounds) noexcept {
    if (!source.width || !source.height || !bounds.width || !bounds.height) return bounds;
    const std::uint64_t sw = source.width, sh = source.height;
    if (sw * bounds.height >= sh * bounds.width) {
        const auto h = static_cast<std::uint32_t>((static_cast<std::uint64_t>(bounds.width) * sh + sw / 2) / sw);
        if (h + 2 < bounds.height) bounds.height = h < 1 ? 1 : h;
    } else {
        const auto w = static_cast<std::uint32_t>((static_cast<std::uint64_t>(bounds.height) * sw + sh / 2) / sh);
        if (w + 2 < bounds.width) bounds.width = w < 1 ? 1 : w;
    }
    return bounds;
}
// The viewport: `source`'s shape fitted into the window and centred (letterbox or pillarbox bars around it).
[[nodiscard]] constexpr PictureRect Viewport(PictureSize source, PictureSize window) noexcept {
    const auto fitted = FitInside(source, window);
    return {(window.width - fitted.width) / 2, (window.height - fitted.height) / 2, fitted.width, fitted.height};
}
// 4:2:0 planes of a crop [left, right) x [top, bottom) of the luma plane: the inclusive ranges of texels that are
// picture. A chroma texel belongs to the picture when either luma column (row) it covers does.
struct PlaneBounds { std::int32_t low_x, low_y, high_x, high_y; };
[[nodiscard]] constexpr PlaneBounds LumaBounds(std::uint32_t left, std::uint32_t top, std::uint32_t right, std::uint32_t bottom) noexcept {
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top), static_cast<std::int32_t>(right) - 1, static_cast<std::int32_t>(bottom) - 1};
}
[[nodiscard]] constexpr PlaneBounds ChromaBounds(std::uint32_t left, std::uint32_t top, std::uint32_t right, std::uint32_t bottom) noexcept {
    return {static_cast<std::int32_t>(left / 2), static_cast<std::int32_t>(top / 2), static_cast<std::int32_t>((right + 1) / 2) - 1,
            static_cast<std::int32_t>((bottom + 1) / 2) - 1};
}
}
