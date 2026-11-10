#pragma once
#include <cstdint>

// Colour description of a video track: the four code points of ISO/IEC 23091-2 / ITU-T H.273, as carried by the
// H.264 and HEVC VUI (video_signal_type) and by the MP4 `colr` box (nclx / nclc). Portable, no storage.
namespace media::video {
struct ColorDescription {
    std::uint8_t primaries = 2; // ColourPrimaries: 1 BT.709, 5 BT.470BG (625), 6 SMPTE 170M (525), 9 BT.2020, 2 unspecified.
    std::uint8_t transfer = 2;  // TransferCharacteristics: 1 BT.709, 6 SMPTE 170M, 13 sRGB, 16 PQ, 18 HLG, 2 unspecified.
    std::uint8_t matrix = 2;    // MatrixCoefficients: 1 BT.709, 5/6 BT.601, 9 BT.2020 NCL, 0 identity (RGB), 2 unspecified.
    bool full_range = false;    // VideoFullRangeFlag: false = limited ("video") range.
    bool described = false;     // The source carried a colour description at all (the code points may still be 2).
    bool range_described = false; // The source carried the range flag (QuickTime `nclc` does not).
    constexpr bool operator==(const ColorDescription&) const noexcept = default;
};
inline constexpr std::uint8_t kUnspecified = 2;
// Reserved code points (0 and 3 for primaries and transfer, 3 for the matrix) and values above the tables say
// nothing usable: they count as unspecified.
[[nodiscard]] constexpr std::uint8_t KnownPrimaries(unsigned v) noexcept { return v == 1 || (v >= 4 && v <= 12) || v == 22 ? static_cast<std::uint8_t>(v) : kUnspecified; }
[[nodiscard]] constexpr std::uint8_t KnownTransfer(unsigned v) noexcept { return v == 1 || (v >= 4 && v <= 18) ? static_cast<std::uint8_t>(v) : kUnspecified; }
[[nodiscard]] constexpr std::uint8_t KnownMatrix(unsigned v) noexcept { return v <= 14 && v != 2 && v != 3 ? static_cast<std::uint8_t>(v) : kUnspecified; }

// The description to convert with.
// 1. Per code point, the container's value when it gives one, else the bitstream's (ISO/IEC 14496-12: colour
//    information in the sample entry takes precedence over the video stream's). The range flag likewise, when the
//    container carries one.
// 2. What is still unspecified is inferred, as receivers conventionally do: BT.2020 primaries imply the BT.2020
//    matrix; otherwise pictures taller than 576 lines are BT.709 and smaller ones BT.601 (625-line primaries at 576
//    lines, 525-line otherwise). The transfer characteristic defaults to BT.709's, which BT.601 shares.
// The result has no unspecified code point.
[[nodiscard]] constexpr ColorDescription ResolveColor(const ColorDescription& container, const ColorDescription& bitstream,
                                                      std::uint32_t picture_height) noexcept {
    const auto pick = [](std::uint8_t c, std::uint8_t b) noexcept { return c != kUnspecified ? c : b; };
    ColorDescription out;
    out.primaries = pick(container.described ? KnownPrimaries(container.primaries) : kUnspecified, KnownPrimaries(bitstream.primaries));
    out.transfer = pick(container.described ? KnownTransfer(container.transfer) : kUnspecified, KnownTransfer(bitstream.transfer));
    out.matrix = pick(container.described ? KnownMatrix(container.matrix) : kUnspecified, KnownMatrix(bitstream.matrix));
    out.full_range = container.described && container.range_described ? container.full_range : bitstream.full_range;
    out.described = container.described || bitstream.described;
    out.range_described = (container.described && container.range_described) || bitstream.range_described;
    const bool high_definition = picture_height > 576;
    if (out.matrix == kUnspecified) out.matrix = out.primaries == 9 ? 9 : high_definition ? 1 : picture_height == 576 ? 5 : 6;
    if (out.primaries == kUnspecified) out.primaries = out.matrix == 9 ? 9 : high_definition ? 1 : picture_height == 576 ? 5 : 6;
    if (out.transfer == kUnspecified) out.transfer = out.primaries == 9 ? 14 : high_definition ? 1 : 6; // 14: BT.2020 10-bit, same curve.
    return out;
}
// VideoFrameDescriptor::color_metadata_id: matrix bits 0-7, full range bit 8, transfer bits 16-23, primaries 24-31.
[[nodiscard]] constexpr std::uint32_t PackColor(const ColorDescription& c) noexcept {
    return static_cast<std::uint32_t>(c.matrix) | (c.full_range ? 256u : 0u) | (static_cast<std::uint32_t>(c.transfer) << 16) |
           (static_cast<std::uint32_t>(c.primaries) << 24);
}
[[nodiscard]] constexpr ColorDescription UnpackColor(std::uint32_t id) noexcept {
    ColorDescription c;
    c.matrix = static_cast<std::uint8_t>(id & 255u); c.full_range = (id & 256u) != 0;
    c.transfer = static_cast<std::uint8_t>((id >> 16) & 255u); c.primaries = static_cast<std::uint8_t>(id >> 24);
    return c;
}
// Matrices the conversion shaders implement (Nv12ToRgb.hlsl): BT.709, BT.601 (5, 6) and BT.2020 non-constant luminance.
[[nodiscard]] constexpr bool ConvertibleMatrix(std::uint8_t matrix) noexcept { return matrix == 1 || matrix == 5 || matrix == 6 || matrix == 9; }

[[nodiscard]] constexpr const char* PrimariesName(std::uint8_t v) noexcept {
    switch (v) {
    case 1: return "BT.709"; case 4: return "BT.470M"; case 5: return "BT.470BG (625)"; case 6: return "SMPTE 170M (525)";
    case 7: return "SMPTE 240M"; case 8: return "film"; case 9: return "BT.2020"; case 10: return "XYZ"; case 11: return "DCI-P3";
    case 12: return "Display P3"; case 22: return "EBU 3213"; default: return "unspecified";
    }
}
[[nodiscard]] constexpr const char* TransferName(std::uint8_t v) noexcept {
    switch (v) {
    case 1: return "BT.709"; case 4: return "gamma 2.2"; case 5: return "gamma 2.8"; case 6: return "SMPTE 170M"; case 7: return "SMPTE 240M";
    case 8: return "linear"; case 9: case 10: return "logarithmic"; case 11: return "xvYCC"; case 12: return "BT.1361"; case 13: return "sRGB";
    case 14: return "BT.2020 10-bit"; case 15: return "BT.2020 12-bit"; case 16: return "PQ"; case 17: return "SMPTE 428"; case 18: return "HLG";
    default: return "unspecified";
    }
}
[[nodiscard]] constexpr const char* MatrixName(std::uint8_t v) noexcept {
    switch (v) {
    case 0: return "identity (RGB)"; case 1: return "BT.709"; case 4: return "FCC"; case 5: return "BT.601 (625)"; case 6: return "BT.601 (525)";
    case 7: return "SMPTE 240M"; case 8: return "YCgCo"; case 9: return "BT.2020 NCL"; case 10: return "BT.2020 CL"; case 11: return "SMPTE 2085";
    case 12: return "chromaticity NCL"; case 13: return "chromaticity CL"; case 14: return "ICtCp"; default: return "unspecified";
    }
}
}
