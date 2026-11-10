#include "media/codec/H264PcmDecoder.hpp"
#include <cstring>

namespace media::codec {
SyntaxStatus H264PcmDecoder::Decode(const H264Picture& picture, std::span<const std::byte> au, const PicturePlanes& out,
                                    const PicturePlanes* reference) noexcept {
    if (!picture.sps || !picture.pps || !out.y || !out.cb || !out.cr) return SyntaxStatus::Invalid;
    const auto& s = *picture.sps; const auto& p = *picture.pps;
    if (p.entropy || p.weighted) return SyntaxStatus::Unsupported;
    const std::uint32_t mbs_wide = s.width / 16u, mbs = mbs_wide * (s.height / 16u);
    for (std::uint32_t k = 0; k < picture.slice_count; ++k) {
        const auto nal = au.subspan(picture.slices[k].offset, picture.slices[k].bytes);
        const auto header = std::to_integer<unsigned>(nal[0]);
        const bool idr = (header & 31) == 5, reference_nal = (header & 96) != 0;
        RbspReader r(nal.subspan(1));
        // The slice header, as far as the slice data (7.3.3).
        std::uint32_t mb = r.Ue();
        const auto type = r.Ue() % 5; (void)r.Ue(); // slice_type, pic_parameter_set_id.
        if (type != 0 && type != 2) return SyntaxStatus::Unsupported; // P and I only.
        (void)r.Bits(s.frame_bits);
        if (idr) (void)r.Ue();
        if (s.poc_type == 0) { (void)r.Bits(s.poc_bits); if (p.bottom_poc) (void)r.Se(); }
        else if (s.poc_type == 1 && !s.always_zero) { (void)r.Se(); if (p.bottom_poc) (void)r.Se(); }
        if (type == 0) {
            if (r.Bits(1)) (void)r.Ue();                         // num_ref_idx_active_override.
            if (r.Bits(1)) return SyntaxStatus::Unsupported;     // ref_pic_list_modification: another RefPicList0[0].
            if (!reference || !reference->y) return SyntaxStatus::Unsupported;
        }
        if (reference_nal) {
            if (idr) (void)r.Bits(2);
            else if (r.Bits(1)) for (unsigned n = 0; n < 66; ++n) { // MMCO: the syntax parser marks; skipped here.
                const auto op = r.Ue();
                if (!r.Good()) return SyntaxStatus::Invalid;
                if (op == 0) break;
                if (op == 1 || op == 3) (void)r.Ue();
                if (op == 2) (void)r.Ue();
                if (op == 3 || op == 6) (void)r.Ue();
                if (op == 4) (void)r.Ue();
            }
        }
        (void)r.Se(); // slice_qp_delta.
        if (p.deblock && r.Ue() != 1) { (void)r.Se(); (void)r.Se(); }
        if (!r.Good()) return SyntaxStatus::Invalid;
        // The slice data (7.3.4), CAVLC.
        const auto copy = [&](std::uint32_t at) {
            const std::uint32_t x = at % mbs_wide * 16, y = at / mbs_wide * 16;
            for (unsigned row = 0; row < 16; ++row) std::memcpy(out.y + (y + row) * out.y_stride + x, reference->y + (y + row) * reference->y_stride + x, 16);
            for (unsigned row = 0; row < 8; ++row) {
                std::memcpy(out.cb + (y / 2 + row) * out.c_stride + x / 2, reference->cb + (y / 2 + row) * reference->c_stride + x / 2, 8);
                std::memcpy(out.cr + (y / 2 + row) * out.c_stride + x / 2, reference->cr + (y / 2 + row) * reference->c_stride + x / 2, 8);
            }
        };
        while (mb < mbs) {
            if (type == 0) {
                const auto skip = r.Ue();
                if (!r.Good() || skip > mbs - mb) return SyntaxStatus::Invalid;
                for (std::uint32_t i = 0; i < skip; ++i) copy(mb++);
                if (mb == mbs || !r.MoreData()) break;
            }
            const auto mb_type = r.Ue();
            if (mb_type != (type == 0 ? 30u : 25u)) return SyntaxStatus::Unsupported; // Anything but I_PCM.
            r.Skip((8u - static_cast<unsigned>(r.Position() % 8)) % 8u);                  // pcm_alignment_zero_bit.
            const std::uint32_t x = mb % mbs_wide * 16, y = mb / mbs_wide * 16;
            for (unsigned i = 0; i < 256; ++i) out.y[(y + i / 16) * out.y_stride + x + i % 16] = static_cast<std::uint8_t>(r.Bits(8));
            for (unsigned i = 0; i < 64; ++i) out.cb[(y / 2 + i / 8) * out.c_stride + x / 2 + i % 8] = static_cast<std::uint8_t>(r.Bits(8));
            for (unsigned i = 0; i < 64; ++i) out.cr[(y / 2 + i / 8) * out.c_stride + x / 2 + i % 8] = static_cast<std::uint8_t>(r.Bits(8));
            if (!r.Good()) return SyntaxStatus::Invalid;
            ++mb;
            if (!r.MoreData()) break;
        }
    }
    return SyntaxStatus::Ready;
}
}
