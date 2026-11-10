#include "media/codec/H264SyntaxParser.hpp"
#include "media/video/PictureGeometry.hpp"
#include <algorithm>
#include <limits>

namespace media::codec {
namespace {
bool Hrd(RbspReader& r) {
    const auto n = r.Ue(); if (n > 31) return false;
    (void)r.Bits(8);
    for (unsigned i = 0; i <= n; ++i) { (void)r.Ue(); (void)r.Ue(); (void)r.Bits(1); }
    (void)r.Bits(20); return r.Good();
}
bool Vui(RbspReader& r, H264Sps& s) {
    if (r.Bits(1)) {
        // aspect_ratio_idc; EXTENDED_SAR carries the pair. A zero term means "unspecified" (E.2.1), not an error.
        const auto idc = r.Bits(8); unsigned width = 0, height = 0;
        if (idc == 255) { width = r.Bits(16); height = r.Bits(16); }
        const auto sar = video::SampleAspectFromIdc(idc, width, height);
        s.sar_width = static_cast<std::uint16_t>(sar.width); s.sar_height = static_cast<std::uint16_t>(sar.height);
    }
    if (r.Bits(1)) (void)r.Bits(1);
    if (r.Bits(1)) {
        (void)r.Bits(3); s.full_range = r.Bits(1) != 0; s.signal_described = true;
        if (r.Bits(1)) {
            s.primaries = static_cast<std::uint8_t>(r.Bits(8)); s.transfer = static_cast<std::uint8_t>(r.Bits(8));
            s.matrix = static_cast<std::uint8_t>(r.Bits(8)); s.colour_described = true;
        }
    }
    if (r.Bits(1)) { if (r.Ue() > 5 || r.Ue() > 5) return false; }
    if (r.Bits(1)) { if (!r.Bits(32) || !r.Bits(32)) return false; (void)r.Bits(1); }
    const bool nal_hrd = r.Bits(1) != 0; if (nal_hrd && !Hrd(r)) return false;
    const bool vcl_hrd = r.Bits(1) != 0; if (vcl_hrd && !Hrd(r)) return false;
    if (nal_hrd || vcl_hrd) (void)r.Bits(1);
    (void)r.Bits(1);
    if (r.Bits(1)) {
        (void)r.Bits(1);
        for (unsigned i = 0; i < 4; ++i) if (r.Ue() > 16) return false;
        const auto reorder = r.Ue(), buffering = r.Ue();
        if (reorder > 16 || buffering > 16 || reorder > buffering) return false;
        s.reorder = static_cast<std::uint8_t>(reorder); s.reorder_signalled = true;
    }
    return r.Good();
}
// Table A-1 MaxDpbMbs; 0 for an unknown level.
std::uint32_t MaxDpbMbs(const unsigned level) noexcept {
    switch (level) {
    case 9: case 10: return 396;
    case 11: return 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    case 60: case 61: case 62: return 696320;
    default: return 0;
    }
}
// Weight/offset pairs: each value is a signed 8-bit quantity.
bool Weights(RbspReader& r, const unsigned pairs) noexcept {
    for (unsigned i = 0; i < pairs; ++i) {
        const auto weight = r.Se(), offset = r.Se();
        if (weight < -128 || weight > 127 || offset < -128 || offset > 127) return false;
    }
    return true;
}
}
bool H264SyntaxParser::Initialize(core::LinearMemoryArena& arena) noexcept {
    if (sps_ || arena.IsSealed()) return false;
    sps_ = ArenaArray<H264Sps>(arena, 32); pps_ = ArenaArray<H264Pps>(arena, 256);
    picture_ = ArenaArray<H264Picture>(arena, 1); dpb_ = ArenaArray<H264Reference>(arena, 16);
    return sps_ && pps_ && picture_ && dpb_;
}
const H264Sps* H264SyntaxParser::FirstSps() const noexcept {
    if (sps_) for (unsigned i = 0; i != 32; ++i) if (sps_[i].valid) return sps_ + i;
    return nullptr;
}
SyntaxStatus H264SyntaxParser::ParameterSet(std::span<const std::byte> nal) noexcept {
    if (!picture_ || pending_ || nal.size() < 2) return SyntaxStatus::Invalid;
    const auto h = std::to_integer<unsigned>(nal[0]);
    if ((h & 128) || !(h & 96)) return SyntaxStatus::Invalid;
    RbspReader r(nal.subspan(1));
    if ((h & 31) == 7) return Sps(r);
    if ((h & 31) == 8) return Pps(r);
    return SyntaxStatus::Unsupported;
}
SyntaxStatus H264SyntaxParser::Sps(RbspReader& r) noexcept {
    H264Sps s;
    s.profile = static_cast<std::uint8_t>(r.Bits(8)); s.constraints = static_cast<std::uint8_t>(r.Bits(8));
    s.level = static_cast<std::uint8_t>(r.Bits(8)); const auto id = r.Ue();
    if (!r.Good() || id >= 32 || (s.constraints & 3)) return SyntaxStatus::Invalid;
    if (s.profile == 100) {
        if (r.Ue() != 1 || r.Ue() != 0 || r.Ue() != 0 || r.Bits(1) || r.Bits(1)) return SyntaxStatus::Unsupported;
    } else if (s.profile != 66 && s.profile != 77 && s.profile != 88) return SyntaxStatus::Unsupported;
    const auto frame = r.Ue(), poc = r.Ue();
    if (frame > 12 || poc > 2) return SyntaxStatus::Unsupported;
    s.frame_bits = static_cast<std::uint8_t>(frame + 4); s.poc_type = static_cast<std::uint8_t>(poc);
    if (poc == 0) { const auto bits = r.Ue(); if (bits > 12) return SyntaxStatus::Invalid; s.poc_bits = static_cast<std::uint8_t>(bits + 4); }
    else if (poc == 1) {
        s.always_zero = r.Bits(1) != 0; s.offset_non_ref = r.Se(); s.offset_top_bottom = r.Se();
        const auto cycle = r.Ue(); if (cycle > 255) return SyntaxStatus::Invalid;
        s.cycle = static_cast<std::uint8_t>(cycle);
        for (unsigned i = 0; i < cycle; ++i) { s.cycle_offsets[i] = r.Se(); s.cycle_delta += s.cycle_offsets[i]; }
    }
    const auto refs = r.Ue(); if (refs > 16 || r.Bits(1)) return SyntaxStatus::Unsupported; // gaps_in_frame_num_allowed
    s.max_refs = static_cast<std::uint8_t>(refs);
    const auto w = r.Ue(), h = r.Ue();
    if (w >= 512 || h >= 512 || !r.Bits(1)) return SyntaxStatus::Unsupported;
    s.width = static_cast<std::uint16_t>((w + 1) * 16); s.height = static_cast<std::uint16_t>((h + 1) * 16);
    s.direct8 = r.Bits(1) != 0;
    std::uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (r.Bits(1)) { left = r.Ue(); right = r.Ue(); top = r.Ue(); bottom = r.Ue(); }
    if (left > s.width / 2u || right > s.width / 2u || top > s.height / 2u || bottom > s.height / 2u ||
        (left + right) * 2 >= s.width || (top + bottom) * 2 >= s.height) return SyntaxStatus::Invalid;
    s.left = static_cast<std::uint16_t>(left * 2); s.top = static_cast<std::uint16_t>(top * 2);
    s.right = static_cast<std::uint16_t>(s.width - right * 2); s.bottom = static_cast<std::uint16_t>(s.height - bottom * 2);
    if (r.Bits(1) && !Vui(r, s)) return SyntaxStatus::Invalid;
    if (!r.Finish()) return SyntaxStatus::Invalid;
    if (!s.reorder_signalled) {
        // E.2.1: without bitstream_restriction, max_num_reorder_frames is 0 for the intra profiles' constraint_set3
        // and MaxDpbFrames otherwise. Baseline has no B slices and its streams output in decode order: 0.
        const bool intra_only = (s.profile == 100 || s.profile == 110 || s.profile == 122 || s.profile == 244 ||
                                 s.profile == 44 || s.profile == 86) && (s.constraints & 16);
        const auto frame_mbs = static_cast<std::uint32_t>(s.width / 16) * (s.height / 16);
        const auto dpb = MaxDpbMbs(s.level) / frame_mbs;
        s.reorder = intra_only || s.profile == 66 ? 0 : static_cast<std::uint8_t>((std::min)(dpb ? dpb : 16u, 16u));
    }
    s.valid = true;
    // Configuration changes require a new prepared decoder session; encoders may repeat identical sets in band.
    // How the pictures are described (sample shape, colour) is not decoder configuration: a repeat that differs only
    // there is accepted, and the description the session started with stands.
    if (started_) {
        const auto& active = sps_[id];
        s.sar_width = active.sar_width; s.sar_height = active.sar_height; s.primaries = active.primaries; s.transfer = active.transfer;
        s.matrix = active.matrix; s.full_range = active.full_range; s.signal_described = active.signal_described; s.colour_described = active.colour_described;
        return s == active ? SyntaxStatus::Ready : SyntaxStatus::Unsupported;
    }
    sps_[id] = s; return SyntaxStatus::Ready;
}
SyntaxStatus H264SyntaxParser::Pps(RbspReader& r) noexcept {
    H264Pps p; const auto id = r.Ue(), sid = r.Ue();
    if (id >= 256 || sid >= 32 || !sps_[sid].valid) return SyntaxStatus::Invalid;
    p.sps = static_cast<std::uint8_t>(sid); p.entropy = r.Bits(1) != 0; p.bottom_poc = r.Bits(1) != 0;
    if (r.Ue() != 0) return SyntaxStatus::Unsupported;
    const auto l0 = r.Ue(), l1 = r.Ue(); if (l0 > 31 || l1 > 31) return SyntaxStatus::Invalid;
    p.refs_l0 = static_cast<std::uint8_t>(l0); p.refs_l1 = static_cast<std::uint8_t>(l1);
    p.weighted = r.Bits(1) != 0; p.weighted_bi = static_cast<std::uint8_t>(r.Bits(2));
    const auto qp = r.Se(), qs = r.Se(), chroma = r.Se();
    if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || chroma < -12 || chroma > 12 || p.weighted_bi > 2) return SyntaxStatus::Invalid;
    p.qp = static_cast<std::int8_t>(qp); p.qs = static_cast<std::int8_t>(qs); p.chroma_qp = static_cast<std::int8_t>(chroma);
    p.second_chroma_qp = p.chroma_qp;
    p.deblock = r.Bits(1) != 0; p.constrained = r.Bits(1) != 0;
    if (r.Bits(1)) return SyntaxStatus::Unsupported; // redundant_pic_cnt
    if (r.MoreData()) {
        p.transform8 = r.Bits(1) != 0;
        if (r.Bits(1)) return SyntaxStatus::Unsupported; // Custom scaling lists need a larger frontend.
        const auto second = r.Se(); if (second < -12 || second > 12) return SyntaxStatus::Invalid;
        p.second_chroma_qp = static_cast<std::int8_t>(second);
    }
    if (!r.Finish()) return SyntaxStatus::Invalid;
    p.valid = true;
    if (started_) return p == pps_[id] ? SyntaxStatus::Ready : SyntaxStatus::Unsupported;
    pps_[id] = p; return SyntaxStatus::Ready;
}
SyntaxStatus H264SyntaxParser::Slice(std::span<const std::byte> nal, bool first, std::uint32_t& first_mb) noexcept {
    auto& out = *picture_; const auto header = std::to_integer<unsigned>(nal[0]);
    const bool idr = (header & 31) == 5, reference = (header & 96) != 0;
    RbspReader r(nal.subspan(1)); first_mb = r.Ue();
    const auto raw_type = r.Ue(), pid = r.Ue();
    if (raw_type > 9 || pid >= 256 || !pps_[pid].valid) return SyntaxStatus::Invalid;
    const auto type = raw_type % 5; // 0 P, 1 B, 2 I, 3 SP, 4 SI.
    if (type > 2) return SyntaxStatus::Unsupported; // SP/SI: Extended profile switching slices.
    const bool intra = type == 2, bidirectional = type == 1;
    if (idr && (!intra || !reference)) return SyntaxStatus::Invalid;
    const auto& p = pps_[pid]; const auto& s = sps_[p.sps];
    if (!s.valid) return SyntaxStatus::Invalid;
    if (started_ && p.sps != active_sps_) return SyntaxStatus::Unsupported;
    if (first_mb >= static_cast<std::uint32_t>(s.width / 16) * (s.height / 16)) return SyntaxStatus::Invalid;
    const auto frame = r.Bits(s.frame_bits);
    const auto idr_id = idr ? r.Ue() : 0;
    if (idr_id > 65535) return SyntaxStatus::Invalid;
    std::uint32_t lsb = 0; std::int32_t delta = 0, delta0 = 0, delta1 = 0;
    if (s.poc_type == 0) { lsb = r.Bits(s.poc_bits); if (p.bottom_poc) delta = r.Se(); }
    else if (s.poc_type == 1 && !s.always_zero) { delta0 = r.Se(); if (p.bottom_poc) delta1 = r.Se(); }
    std::uint32_t active[2]{p.refs_l0, p.refs_l1};
    if (bidirectional) (void)r.Bits(1); // direct_spatial_mv_pred_flag: slice data belongs to the accelerator.
    if (!intra && r.Bits(1)) { active[0] = r.Ue(); if (bidirectional) active[1] = r.Ue(); }
    if (active[0] > 31 || active[1] > 31) return SyntaxStatus::Invalid;
    // ref_pic_list_modification: validated only. The accelerator builds and modifies the lists from the DPB.
    const unsigned lists = intra ? 0u : bidirectional ? 2u : 1u;
    for (unsigned list = 0; list < lists; ++list) {
        if (!r.Bits(1)) continue;
        for (unsigned n = 0;; ++n) {
            const auto operation = r.Ue();
            if (operation == 3) break;
            // abs_diff_pic_num_minus1 < MaxPicNum; long_term_pic_num < 16 for frames; at most one per list entry.
            const auto value = operation <= 2 ? r.Ue() : 0;
            if (!r.Good() || operation > 2 || n > active[list] || (operation < 2 ? value >= (1u << s.frame_bits) : value >= 16))
                return SyntaxStatus::Invalid;
        }
    }
    if ((p.weighted && type == 0) || (p.weighted_bi == 1 && bidirectional)) {
        if (r.Ue() > 7 || r.Ue() > 7) return SyntaxStatus::Invalid; // luma/chroma_log2_weight_denom
        for (unsigned list = 0; list < lists; ++list)
            for (unsigned i = 0; i <= active[list]; ++i) {
                if (r.Bits(1) && !Weights(r, 1)) return SyntaxStatus::Invalid;
                if (r.Bits(1) && !Weights(r, 2)) return SyntaxStatus::Invalid;
            }
    }
    bool adaptive = false, long_term = false;
    H264Mmco operations[32]{}; unsigned operation_count = 0;
    if (reference) {
        if (idr) { (void)r.Bits(1); long_term = r.Bits(1) != 0; } // no_output_of_prior_pics, long_term_reference_flag
        else if ((adaptive = r.Bits(1) != 0)) {
            bool reset = false;
            for (;;) {
                const auto operation = r.Ue();
                if (!r.Good() || operation > 6) return SyntaxStatus::Invalid;
                if (operation == 0) break;
                if (operation_count == 32) return SyntaxStatus::Unsupported;
                auto& m = operations[operation_count++];
                m.operation = static_cast<std::uint8_t>(operation);
                if (operation == 1 || operation == 3) m.value = r.Ue();
                if (operation == 2) m.value = r.Ue();
                if (operation == 3 || operation == 6) m.index = r.Ue();
                if (operation == 4) m.value = r.Ue();
                if (!r.Good() || ((operation == 1 || operation == 3) && m.value >= (1u << s.frame_bits)) ||
                    (operation == 2 && m.value >= 16) || m.index >= 16 || (operation == 4 && m.value > 16) ||
                    (operation == 5 && reset)) return SyntaxStatus::Invalid;
                reset = reset || operation == 5;
            }
        }
    }
    if (p.entropy && !intra && r.Ue() > 2) return SyntaxStatus::Invalid;
    const auto qp = static_cast<std::int64_t>(r.Se()) + 26 + p.qp;
    if (qp < 0 || qp > 51) return SyntaxStatus::Invalid;
    if (p.deblock) {
        const auto disable = r.Ue(); if (disable > 2) return SyntaxStatus::Invalid;
        if (disable != 1) { const auto a = r.Se(), b = r.Se(); if (a < -6 || a > 6 || b < -6 || b > 6) return SyntaxStatus::Invalid; }
    }
    if (!r.Good()) return SyntaxStatus::Invalid;
    if (!started_ && !idr) return SyntaxStatus::NeedIdr;
    if (idr && frame) return SyntaxStatus::Invalid;
    if (!intra && !idr && !refs_) return SyntaxStatus::Invalid; // Inter prediction needs a reference.
    if (!first) {
        // Picture-level syntax repeats in every slice; slice types may differ.
        if (out.frame_num != frame || out.idr != idr || out.reference != reference || out.poc_lsb != lsb ||
            out.pps_id != pid || out.idr_picture_id != idr_id || out.delta_poc != delta || out.delta_poc0 != delta0 ||
            out.delta_poc1 != delta1 || out.adaptive != adaptive || out.mmco_count != operation_count || out.long_term != long_term)
            return SyntaxStatus::Unsupported;
        out.intra = out.intra && intra; out.bidirectional = out.bidirectional || bidirectional;
        return SyntaxStatus::Ready;
    }
    if (first_mb != 0) return SyntaxStatus::Unsupported;
    const auto modulus = 1u << s.frame_bits;
    // Without gaps, frame_num repeats PrevRefFrameNum (after non-reference pictures) or follows it by one.
    if (started_ && !idr && frame != previous_reference_frame_ && frame != (previous_reference_frame_ + 1) % modulus)
        return SyntaxStatus::Unsupported;
    // 8.2.1: FrameNumOffset for types 1 and 2 wraps with prevFrameNum (the previous picture's frame_num).
    const auto frame_offset = idr ? 0 : frame_offset_ + (previous_frame_ > frame ? modulus : 0);
    std::int64_t msb = 0, top = 0, bottom = 0;
    if (s.poc_type == 0) {
        const auto max = 1u << s.poc_bits; const auto prev = idr ? 0 : previous_lsb_;
        msb = idr ? 0 : previous_msb_;
        if (lsb < prev && prev - lsb >= max / 2) msb += max;
        else if (lsb > prev && lsb - prev > max / 2) msb -= max;
        top = msb + lsb; bottom = top + delta;
    } else if (s.poc_type == 1) {
        std::int64_t expected = 0;
        std::int64_t absolute = s.cycle ? frame_offset + frame : 0;
        if (!reference && absolute > 0) --absolute;
        if (absolute > 0) {
            const auto cycles = (absolute - 1) / s.cycle, in_cycle = (absolute - 1) % s.cycle;
            if (s.cycle_delta && cycles > INT64_MAX / 2 / (s.cycle_delta < 0 ? -s.cycle_delta : s.cycle_delta)) return SyntaxStatus::Unsupported;
            expected = cycles * s.cycle_delta;
            for (std::int64_t i = 0; i <= in_cycle; ++i) expected += s.cycle_offsets[i];
        }
        if (!reference) expected += s.offset_non_ref;
        top = expected + delta0; bottom = top + s.offset_top_bottom + delta1;
    } else {
        top = bottom = idr ? 0 : 2 * (frame_offset + frame) - (reference ? 0 : 1);
    }
    if (top < INT32_MIN || top > INT32_MAX || bottom < INT32_MIN || bottom > INT32_MAX || msb < INT32_MIN || msb > INT32_MAX)
        return SyntaxStatus::Unsupported;
    out.sps = &s; out.pps = &p; out.idr = idr; out.reference = reference; out.intra = intra; out.bidirectional = bidirectional;
    out.pps_id = pid; out.idr_picture_id = idr_id; out.delta_poc = delta; out.delta_poc0 = delta0; out.delta_poc1 = delta1;
    out.frame_num = frame; out.poc_lsb = lsb; out.poc_msb = static_cast<std::int32_t>(msb); out.frame_offset = frame_offset;
    out.top_poc = static_cast<std::int32_t>(top); out.bottom_poc = static_cast<std::int32_t>(bottom);
    out.adaptive = adaptive; out.long_term = long_term; out.mmco_count = operation_count;
    for (unsigned i = 0; i < operation_count; ++i) out.mmco[i] = operations[i];
    out.reference_count = idr ? 0 : refs_;
    for (unsigned i = 0; i < out.reference_count; ++i) out.references[i] = dpb_[i];
    return SyntaxStatus::Ready;
}
SyntaxStatus H264SyntaxParser::Parse(std::span<const std::byte> au, unsigned length) noexcept {
    if (!picture_ || pending_) return SyntaxStatus::Busy;
    if (length != 1 && length != 2 && length != 4) return SyntaxStatus::Invalid;
    *picture_ = {}; std::size_t offset = 0; std::uint32_t last_mb = 0;
    while (offset < au.size()) {
        if (au.size() - offset < length) return SyntaxStatus::Invalid;
        std::uint32_t size = 0; for (unsigned i = 0; i < length; ++i) size = (size << 8) | std::to_integer<unsigned>(au[offset++]);
        if (!size || size > au.size() - offset || offset > UINT32_MAX) return SyntaxStatus::Invalid;
        const auto header = std::to_integer<unsigned>(au[offset]), type = header & 31;
        if (header & 128) return SyntaxStatus::Invalid;
        if (type == 1 || type == 5) {
            if (picture_->slice_count == 256) return SyntaxStatus::Unsupported;
            std::uint32_t first_mb = 0; const auto status = Slice(au.subspan(offset, size), !picture_->slice_count, first_mb);
            if (status != SyntaxStatus::Ready) return status;
            if (picture_->slice_count && first_mb <= last_mb) return SyntaxStatus::Unsupported;
            last_mb = first_mb;
            picture_->slices[picture_->slice_count++] = {static_cast<std::uint32_t>(offset), size};
        } else if (type == 7 || type == 8) {
            const auto status = ParameterSet(au.subspan(offset, size)); if (status != SyntaxStatus::Ready) return status;
        } else if (type != 6 && type != 9 && type != 10 && type != 11 && type != 12) return SyntaxStatus::Unsupported;
        offset += size;
    }
    if (!picture_->slice_count) return SyntaxStatus::Invalid;
    pending_ = true; return SyntaxStatus::Ready;
}
bool H264SyntaxParser::Commit(platform::SurfaceHandle surface, std::span<platform::SurfaceHandle> evicted, unsigned& count) noexcept {
    count = 0;
    if (!pending_ || evicted.size() < 16) return false;
    const auto& p = *picture_; const auto& s = *p.sps;
    auto evict = [&](unsigned i) noexcept {
        evicted[count++] = dpb_[i].surface;
        for (unsigned j = i + 1; j < refs_; ++j) dpb_[j - 1] = dpb_[j];
        --refs_;
    };
    // 8.2.4.1: PicNum of a short-term frame is its FrameNumWrap; CurrPicNum is frame_num. LongTermPicNum is the index.
    const auto max_frame = static_cast<std::int64_t>(1) << s.frame_bits;
    auto short_term = [&](std::int64_t pic_num) noexcept -> int {
        for (unsigned i = 0; i < refs_; ++i)
            if (!dpb_[i].long_term && (dpb_[i].frame_num > p.frame_num ? dpb_[i].frame_num - max_frame : dpb_[i].frame_num) == pic_num)
                return static_cast<int>(i);
        return -1;
    };
    auto long_term = [&](std::uint32_t index) noexcept -> int {
        for (unsigned i = 0; i < refs_; ++i) if (dpb_[i].long_term && dpb_[i].frame_num == index) return static_cast<int>(i);
        return -1;
    };
    // Sliding window (8.2.5.3): the short-term frame with the smallest FrameNumWrap leaves first.
    auto slide = [&]() noexcept {
        int oldest = -1; std::int64_t smallest = INT64_MAX;
        for (unsigned i = 0; i < refs_; ++i) {
            if (dpb_[i].long_term) continue;
            const std::int64_t wrap = dpb_[i].frame_num > p.frame_num ? dpb_[i].frame_num - max_frame : dpb_[i].frame_num;
            if (wrap < smallest) { smallest = wrap; oldest = static_cast<int>(i); }
        }
        if (oldest < 0) return false;
        evict(static_cast<unsigned>(oldest)); return true;
    };
    bool reset = false, current_long = false; std::uint16_t current_index = 0;
    std::int32_t top = p.top_poc, bottom = p.bottom_poc;
    if (p.reference) {
        if (p.idr) {
            while (refs_) evict(0); // Oldest first.
            max_long_term_ = p.long_term ? 0 : -1; current_long = p.long_term;
        } else if (p.adaptive) {
            // 8.2.5.4, in bitstream order. Operations naming absent pictures are ignored, as by tolerant decoders.
            for (unsigned k = 0; k < p.mmco_count; ++k) {
                const auto& m = p.mmco[k];
                const std::int64_t pic_num = static_cast<std::int64_t>(p.frame_num) - (static_cast<std::int64_t>(m.value) + 1);
                if (m.operation == 1) { const auto i = short_term(pic_num); if (i >= 0) evict(static_cast<unsigned>(i)); }
                else if (m.operation == 2) { const auto i = long_term(m.value); if (i >= 0) evict(static_cast<unsigned>(i)); }
                else if (m.operation == 3) {
                    if (static_cast<std::int32_t>(m.index) > max_long_term_ || short_term(pic_num) < 0) continue;
                    const auto held = long_term(m.index); if (held >= 0) evict(static_cast<unsigned>(held));
                    auto& frame = dpb_[short_term(pic_num)];
                    frame.long_term = true; frame.frame_num = static_cast<std::uint16_t>(m.index);
                } else if (m.operation == 4) {
                    max_long_term_ = static_cast<std::int32_t>(m.value) - 1;
                    for (unsigned i = refs_; i-- > 0;)
                        if (dpb_[i].long_term && static_cast<std::int32_t>(dpb_[i].frame_num) > max_long_term_) evict(i);
                } else if (m.operation == 5) {
                    while (refs_) evict(0); // Oldest first.
                    max_long_term_ = -1; reset = true;
                } else if (m.operation == 6) {
                    if (static_cast<std::int32_t>(m.index) > max_long_term_) continue;
                    const auto held = long_term(m.index); if (held >= 0) evict(static_cast<unsigned>(held));
                    current_long = true; current_index = static_cast<std::uint16_t>(m.index);
                }
            }
        } else if (s.max_refs) {
            while (refs_ >= s.max_refs && slide()) {}
        }
        if (reset) {
            // 8.2.1: after MMCO 5 the picture counts as frame_num 0 with POCs relative to the smaller of the two.
            const auto temp = (std::min)(top, bottom); top -= temp; bottom -= temp;
        }
        if (s.max_refs) {
            // A stream whose adaptive marking overfills the DPB is repaired with the sliding window.
            while (refs_ >= s.max_refs && slide()) {}
            if (refs_ == 16) return false;
            dpb_[refs_++] = {surface, current_long ? current_index : static_cast<std::uint16_t>(reset ? 0 : p.frame_num), current_long, top, bottom};
        }
        if (reset) { previous_msb_ = 0; previous_lsb_ = static_cast<std::uint32_t>(top); previous_reference_frame_ = 0; }
        else { previous_msb_ = p.poc_msb; previous_lsb_ = p.poc_lsb; previous_reference_frame_ = p.frame_num; }
    }
    previous_frame_ = reset ? 0 : p.frame_num; frame_offset_ = reset ? 0 : p.frame_offset;
    active_sps_ = p.pps->sps;
    started_ = true; pending_ = false; return true;
}
bool H264SyntaxParser::Reset(std::span<platform::SurfaceHandle> released, unsigned& count) noexcept {
    count = 0;
    if (released.size() < refs_) return false;
    for (unsigned i = 0; i < refs_; ++i) released[count++] = dpb_[i].surface;
    refs_ = 0; started_ = pending_ = false; max_long_term_ = -1;
    previous_frame_ = previous_reference_frame_ = previous_lsb_ = 0; previous_msb_ = 0; frame_offset_ = 0;
    return true;
}
}
