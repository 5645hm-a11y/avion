#pragma once
#include "media/codec/H264SyntaxParser.hpp"
#include <cstdint>
#include <span>

namespace media::codec {
// One picture's planes in host memory: 8-bit 4:2:0, the coded size (whole macroblocks).
struct PicturePlanes {
    std::uint8_t* y = nullptr; std::uint8_t* cb = nullptr; std::uint8_t* cr = nullptr;
    std::uint32_t y_stride = 0, c_stride = 0;
};
// The engine's software decode bridge for H.264 (docs/sdk.md, "Host-driven playback"): the macroblock types a picture can
// be made of without any prediction or transform, decoded bit-exactly on the CPU, everywhere.
// - I_PCM macroblocks (mb_type 25 in an I slice, 30 in a P slice): the samples as coded.
// - P_Skip macroblocks whose motion vector prediction is zero (every macroblock of the slice skipped, or skipped after
//   an I_PCM neighbour, whose motion counts as zero): the co-located samples of RefPicList0[0], the reference frame with
//   the largest FrameNumWrap (no list modification).
// Deblocking leaves these pictures unchanged (an I_PCM macroblock has QP 0, whose alpha is 0; skipped neighbours with the
// same reference and motion have boundary strength 0), so none is run. CAVLC only (entropy_coding_mode_flag 0), frames only.
// Anything else (residuals, other macroblock types, CABAC, weighted prediction, list modification) is Unsupported: a host
// decoder (the SDK's media_decode_video_callback) handles such streams. No allocation, no state between pictures.
class H264PcmDecoder final {
public:
    // `picture`: what H264SyntaxParser::Parse accepted for `access_unit` (`length`-byte NAL sizes); `reference`: the planes
    // of RefPicList0[0] for P slices (null for an IDR). Writes every macroblock of `out` or returns Unsupported/Invalid.
    [[nodiscard]] static SyntaxStatus Decode(const H264Picture& picture, std::span<const std::byte> access_unit, const PicturePlanes& out,
                                             const PicturePlanes* reference) noexcept;
};
}
