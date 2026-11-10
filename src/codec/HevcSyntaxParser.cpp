#include "media/codec/HevcSyntaxParser.hpp"
#include "media/video/PictureGeometry.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace media::codec {
namespace {
using S = SyntaxStatus;
enum : unsigned { RadlN = 6, RaslN = 8, RaslR = 9, BlaWLp = 16, BlaNLp = 18, IdrWRadl = 19, IdrNLp = 20, Cra = 21,
                  VpsNut = 32, SpsNut = 33, PpsNut = 34, AudNut = 35, EosNut = 36, EobNut = 37, SeiPrefix = 39, SeiSuffix = 40 };
bool Irap(unsigned type) noexcept { return type >= BlaWLp && type <= 23; }
bool Idr(unsigned type) noexcept { return type == IdrWRadl || type == IdrNLp; }
bool Rasl(unsigned type) noexcept { return type == RaslN || type == RaslR; }
bool Radl(unsigned type) noexcept { return type == RadlN || type == RadlN + 1; }
bool SubLayerNonReference(unsigned type) noexcept { return type <= 14 && !(type & 1); }
unsigned CeilLog2(std::uint32_t n) noexcept { return n <= 1 ? 0u : static_cast<unsigned>(std::bit_width(n - 1)); }
unsigned Count(std::uint32_t bits) noexcept { return static_cast<unsigned>(std::popcount(bits)); }

// ITU-T H.265 Table 7-6, coded (up-right diagonal) order.
constexpr std::uint8_t kIntra[64]{
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
    24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
constexpr std::uint8_t kInter[64]{
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};
unsigned MatrixStep(unsigned size) noexcept { return size == 3 ? 3u : 1u; }
unsigned Coefficients(unsigned size) noexcept { return size ? 64u : 16u; }
void DefaultList(HevcScalingList& s, unsigned size, unsigned matrix) noexcept {
    for (unsigned i = 0; i < Coefficients(size); ++i) s.lists[size][matrix][i] = size ? (matrix < 3 ? kIntra : kInter)[i] : std::uint8_t{16};
    if (size > 1) s.dc[size - 2][matrix] = 16;
}
void FillLists(HevcScalingList& s, bool flat) noexcept {
    for (unsigned size = 0; size < 4; ++size)
        for (unsigned matrix = 0; matrix < 6; matrix += MatrixStep(size)) {
            DefaultList(s, size, matrix);
            if (flat) for (unsigned i = 0; i < Coefficients(size); ++i) s.lists[size][matrix][i] = 16;
        }
}
bool ScalingListData(RbspReader& r, HevcScalingList& s) noexcept {
    for (unsigned size = 0; size < 4; ++size)
        for (unsigned matrix = 0; matrix < 6; matrix += MatrixStep(size)) {
            if (!r.Bits(1)) {
                const auto delta = r.Ue();
                if (!r.Good() || delta > matrix / MatrixStep(size)) return false;
                if (!delta) { DefaultList(s, size, matrix); continue; }
                const auto reference = matrix - delta * MatrixStep(size);
                for (unsigned i = 0; i < Coefficients(size); ++i) s.lists[size][matrix][i] = s.lists[size][reference][i];
                if (size > 1) s.dc[size - 2][matrix] = s.dc[size - 2][reference];
                continue;
            }
            std::int32_t next = 8;
            if (size > 1) {
                const auto dc = r.Se();
                if (dc < -7 || dc > 247) return false;
                next = dc + 8; s.dc[size - 2][matrix] = static_cast<std::uint8_t>(next);
            }
            for (unsigned i = 0; i < Coefficients(size); ++i) {
                const auto delta = r.Se();
                if (delta < -128 || delta > 127) return false;
                next = (next + delta + 256) % 256;
                if (!next) return false;
                s.lists[size][matrix][i] = static_cast<std::uint8_t>(next);
            }
        }
    return r.Good();
}
S ProfileTierLevel(RbspReader& r, unsigned sub_layers, std::uint8_t& profile, std::uint8_t& level, bool& tier) noexcept {
    const auto space = r.Bits(2); tier = r.Bits(1) != 0; profile = static_cast<std::uint8_t>(r.Bits(5));
    const auto compatible = r.Bits(32);
    r.Skip(48); // Source/constraint flags.
    level = static_cast<std::uint8_t>(r.Bits(8));
    bool profile_present[7]{}, level_present[7]{};
    for (unsigned i = 0; i < sub_layers; ++i) { profile_present[i] = r.Bits(1) != 0; level_present[i] = r.Bits(1) != 0; }
    if (sub_layers) r.Skip(2 * (8 - sub_layers));
    for (unsigned i = 0; i < sub_layers; ++i) { if (profile_present[i]) r.Skip(88); if (level_present[i]) r.Skip(8); }
    if (!r.Good()) return S::Invalid;
    // general_profile_compatibility_flag[j] is bit 31-j: Main (1) or Main 10 (2).
    const bool main = profile == 1 || profile == 2 || ((compatible >> 30) & 1u) || ((compatible >> 29) & 1u);
    return space || !main ? S::Unsupported : S::Ready;
}
bool Hrd(RbspReader& r, unsigned sub_layers) noexcept {
    const bool nal = r.Bits(1) != 0, vcl = r.Bits(1) != 0;
    bool sub_picture = false;
    if (nal || vcl) {
        sub_picture = r.Bits(1) != 0;
        if (sub_picture) r.Skip(8 + 5 + 1 + 5);
        r.Skip(8); // bit_rate_scale, cpb_size_scale.
        if (sub_picture) r.Skip(4);
        r.Skip(15);
    }
    for (unsigned i = 0; i <= sub_layers && r.Good(); ++i) {
        const bool fixed = r.Bits(1) != 0 || r.Bits(1) != 0; // General, else within-CVS.
        bool low_delay = false;
        if (fixed) { if (r.Ue() > 2047) return false; }
        else low_delay = r.Bits(1) != 0;
        unsigned count = 1;
        if (!low_delay) { const auto c = r.Ue(); if (c > 31) return false; count = c + 1; }
        const unsigned tables = (nal ? 1u : 0u) + (vcl ? 1u : 0u);
        for (unsigned t = 0; t < tables; ++t)
            for (unsigned j = 0; j < count; ++j) {
                (void)r.Ue(); (void)r.Ue();
                if (sub_picture) { (void)r.Ue(); (void)r.Ue(); }
                (void)r.Bits(1);
            }
    }
    return r.Good();
}
S Vui(RbspReader& r, HevcSps& s) noexcept {
    if (r.Bits(1)) {
        // aspect_ratio_idc; EXTENDED_SAR carries the pair. A zero term means "unspecified" (E.3.1).
        const auto idc = r.Bits(8); unsigned width = 0, height = 0;
        if (idc == 255) { width = r.Bits(16); height = r.Bits(16); }
        const auto sar = video::SampleAspectFromIdc(idc, width, height);
        s.sar_width = static_cast<std::uint16_t>(sar.width); s.sar_height = static_cast<std::uint16_t>(sar.height);
    }
    if (r.Bits(1)) (void)r.Bits(1);
    if (r.Bits(1)) {
        (void)r.Bits(3); s.full_range = r.Bits(1) != 0; s.signal_described = true;
        if (r.Bits(1)) {
            s.colour_primaries = static_cast<std::uint8_t>(r.Bits(8));
            s.transfer = static_cast<std::uint8_t>(r.Bits(8));
            s.matrix = static_cast<std::uint8_t>(r.Bits(8)); s.colour_described = true;
        }
    }
    if (r.Bits(1) && (r.Ue() > 5 || r.Ue() > 5)) return S::Invalid;
    (void)r.Bits(1);
    if (r.Bits(1)) return S::Unsupported; // field_seq_flag: coded field pictures.
    (void)r.Bits(1);
    if (r.Bits(1)) for (unsigned i = 0; i < 4; ++i) (void)r.Ue(); // Default display window.
    if (r.Bits(1)) {
        if (!r.Bits(32) || !r.Bits(32)) return S::Invalid;
        if (r.Bits(1)) (void)r.Ue();
        if (r.Bits(1) && !Hrd(r, s.max_sub_layers_minus1)) return S::Invalid;
    }
    if (r.Bits(1)) {
        r.Skip(3);
        if (r.Ue() > 4095 || r.Ue() > 16 || r.Ue() > 16 || r.Ue() > 15 || r.Ue() > 15) return S::Invalid;
    }
    return r.Good() ? S::Ready : S::Invalid;
}
// st_ref_pic_set(index) and its derivation (7.3.7, 7.4.8). index == count is the slice-header form.
S ShortTermRps(RbspReader& r, const HevcShortTermRps* sets, unsigned index, unsigned count, unsigned max_dec,
               HevcShortTermRps& out) noexcept {
    out = {};
    if (index && r.Bits(1)) {
        unsigned distance = 1;
        if (index == count) { const auto d = r.Ue(); if (!r.Good() || d >= index) return S::Invalid; distance = d + 1; }
        const auto& ref = sets[index - distance];
        const bool sign = r.Bits(1) != 0; const auto magnitude = r.Ue();
        if (!r.Good() || magnitude > 32767) return S::Invalid;
        const std::int32_t delta = (sign ? -1 : 1) * static_cast<std::int32_t>(magnitude + 1);
        const unsigned total = ref.negative + ref.positive;
        bool used[17]{}, use[17]{};
        for (unsigned j = 0; j <= total; ++j) { used[j] = r.Bits(1) != 0; use[j] = used[j] || r.Bits(1) != 0; }
        if (!r.Good()) return S::Invalid;
        unsigned n = 0;
        auto add = [&](std::int32_t* deltas, std::uint16_t& mask, std::int32_t poc, bool current) noexcept {
            if (n == 16) return false;
            deltas[n] = poc; if (current) mask = static_cast<std::uint16_t>(mask | (1u << n));
            ++n; return true;
        };
        for (unsigned j = ref.positive; j-- > 0;) {
            const auto poc = ref.delta_s1[j] + delta; const auto k = ref.negative + j;
            if (poc < 0 && use[k] && !add(out.delta_s0, out.used_s0, poc, used[k])) return S::Invalid;
        }
        if (delta < 0 && use[total] && !add(out.delta_s0, out.used_s0, delta, used[total])) return S::Invalid;
        for (unsigned j = 0; j < ref.negative; ++j) {
            const auto poc = ref.delta_s0[j] + delta;
            if (poc < 0 && use[j] && !add(out.delta_s0, out.used_s0, poc, used[j])) return S::Invalid;
        }
        out.negative = static_cast<std::uint8_t>(n); n = 0;
        for (unsigned j = ref.negative; j-- > 0;) {
            const auto poc = ref.delta_s0[j] + delta;
            if (poc > 0 && use[j] && !add(out.delta_s1, out.used_s1, poc, used[j])) return S::Invalid;
        }
        if (delta > 0 && use[total] && !add(out.delta_s1, out.used_s1, delta, used[total])) return S::Invalid;
        for (unsigned j = 0; j < ref.positive; ++j) {
            const auto poc = ref.delta_s1[j] + delta; const auto k = ref.negative + j;
            if (poc > 0 && use[k] && !add(out.delta_s1, out.used_s1, poc, used[k])) return S::Invalid;
        }
        out.positive = static_cast<std::uint8_t>(n);
        out.reference_delta_pocs = static_cast<std::uint8_t>(total);
    } else {
        const auto negative = r.Ue();
        if (!r.Good() || negative > max_dec) return S::Invalid;
        const auto positive = r.Ue();
        if (!r.Good() || positive > max_dec - negative) return S::Invalid;
        std::int32_t poc = 0;
        for (unsigned i = 0; i < negative; ++i) {
            const auto d = r.Ue(); if (d > 32767) return S::Invalid;
            poc -= static_cast<std::int32_t>(d) + 1; out.delta_s0[i] = poc;
            if (r.Bits(1)) out.used_s0 = static_cast<std::uint16_t>(out.used_s0 | (1u << i));
        }
        poc = 0;
        for (unsigned i = 0; i < positive; ++i) {
            const auto d = r.Ue(); if (d > 32767) return S::Invalid;
            poc += static_cast<std::int32_t>(d) + 1; out.delta_s1[i] = poc;
            if (r.Bits(1)) out.used_s1 = static_cast<std::uint16_t>(out.used_s1 | (1u << i));
        }
        out.negative = static_cast<std::uint8_t>(negative); out.positive = static_cast<std::uint8_t>(positive);
    }
    return r.Good() && out.negative + 0u + out.positive <= max_dec ? S::Ready : S::Invalid;
}
}

bool HevcSyntaxParser::Initialize(core::LinearMemoryArena& arena) noexcept {
    if (sps_ || arena.IsSealed()) return false;
    vps_ = ArenaArray<HevcVps>(arena, 16);
    sps_ = ArenaArray<HevcSps>(arena, 16); sps_scratch_ = ArenaArray<HevcSps>(arena, 1);
    pps_ = ArenaArray<HevcPps>(arena, 64); pps_scratch_ = ArenaArray<HevcPps>(arena, 1);
    picture_ = ArenaArray<HevcPicture>(arena, 1); dpb_ = ArenaArray<HevcReference>(arena, 16);
    first_ = ArenaArray<SliceState>(arena, 1); slice_ = ArenaArray<SliceState>(arena, 1);
    return vps_ && sps_ && sps_scratch_ && pps_ && pps_scratch_ && picture_ && dpb_ && first_ && slice_;
}
const HevcSps* HevcSyntaxParser::FirstSps() const noexcept {
    if (sps_) for (unsigned i = 0; i != 16; ++i) if (sps_[i].valid) return sps_ + i;
    return nullptr;
}
SyntaxStatus HevcSyntaxParser::ParameterSet(std::span<const std::byte> nal) noexcept {
    if (!picture_ || pending_ || nal.size() < 2) return S::Invalid;
    const auto h0 = std::to_integer<unsigned>(nal[0]), h1 = std::to_integer<unsigned>(nal[1]);
    const unsigned type = (h0 >> 1) & 63;
    if ((h0 & 128) || !(h1 & 7)) return S::Invalid;
    if ((h0 & 1) || (h1 >> 3)) return S::Ready; // nuh_layer_id > 0: not part of the base layer.
    if (type >= VpsNut && type <= PpsNut) return Nal(nal);
    return type == SeiPrefix || type == SeiSuffix ? S::Ready : S::Unsupported;
}
SyntaxStatus HevcSyntaxParser::Nal(std::span<const std::byte> nal) noexcept {
    RbspReader r(nal.subspan(2));
    const unsigned type = (std::to_integer<unsigned>(nal[0]) >> 1) & 63;
    return type == VpsNut ? Vps(r) : type == SpsNut ? Sps(r) : Pps(r);
}
SyntaxStatus HevcSyntaxParser::Vps(RbspReader& r) noexcept {
    const auto id = r.Bits(4); r.Skip(8); const auto sub_layers = r.Bits(3); r.Skip(17);
    if (!r.Good() || sub_layers > 6) return S::Invalid;
    std::uint8_t profile = 0, level = 0; bool tier = false;
    if (ProfileTierLevel(r, sub_layers, profile, level, tier) == S::Invalid) return S::Invalid;
    const bool all = r.Bits(1) != 0;
    for (unsigned i = all ? 0 : sub_layers; i <= sub_layers; ++i) {
        const auto dec = r.Ue(), reorder = r.Ue(); (void)r.Ue();
        if (!r.Good() || dec > 15 || reorder > dec) return S::Invalid;
    }
    // Layer sets, timing and HRD parameters do not affect base-layer decoding.
    vps_[id] = {true, static_cast<std::uint8_t>(sub_layers)};
    return S::Ready;
}
SyntaxStatus HevcSyntaxParser::Sps(RbspReader& r) noexcept {
    auto& s = *sps_scratch_; s = {};
    s.vps = static_cast<std::uint8_t>(r.Bits(4)); const auto sub_layers = r.Bits(3); (void)r.Bits(1);
    if (!r.Good() || sub_layers > 6) return S::Invalid;
    s.max_sub_layers_minus1 = static_cast<std::uint8_t>(sub_layers);
    auto status = ProfileTierLevel(r, sub_layers, s.profile, s.level, s.tier);
    if (status != S::Ready) return status;
    const auto id = r.Ue(), chroma = r.Ue();
    if (!r.Good() || id > 15 || chroma > 3) return S::Invalid;
    if (chroma != 1) return S::Unsupported;
    const auto width = r.Ue(), height = r.Ue();
    if (!r.Good() || !width || !height) return S::Invalid;
    if (width > 8192 || height > 8192) return S::Unsupported;
    std::uint64_t left = 0, right = 0, top = 0, bottom = 0;
    if (r.Bits(1)) { left = r.Ue(); right = r.Ue(); top = r.Ue(); bottom = r.Ue(); }
    if (!r.Good() || (left + right) * 2 >= width || (top + bottom) * 2 >= height) return S::Invalid;
    s.width = static_cast<std::uint16_t>(width); s.height = static_cast<std::uint16_t>(height);
    s.left = static_cast<std::uint16_t>(left * 2); s.right = static_cast<std::uint16_t>(width - right * 2);
    s.top = static_cast<std::uint16_t>(top * 2); s.bottom = static_cast<std::uint16_t>(height - bottom * 2);
    const auto luma = r.Ue(), chroma_depth = r.Ue(), poc = r.Ue();
    if (!r.Good() || luma > 8 || chroma_depth > 8 || poc > 12) return S::Invalid;
    if (luma != chroma_depth || (luma != 0 && luma != 2)) return S::Unsupported;
    s.bit_depth_luma_minus8 = static_cast<std::uint8_t>(luma); s.bit_depth_chroma_minus8 = static_cast<std::uint8_t>(chroma_depth);
    s.poc_lsb_bits = static_cast<std::uint8_t>(poc + 4);
    const bool all = r.Bits(1) != 0;
    for (unsigned i = all ? 0 : sub_layers; i <= sub_layers; ++i) {
        const auto dec = r.Ue(), reorder = r.Ue(); (void)r.Ue();
        if (!r.Good() || dec > 15 || reorder > dec) return S::Invalid;
        s.max_dec_pic_buffering_minus1 = static_cast<std::uint8_t>(dec); s.max_num_reorder = static_cast<std::uint8_t>(reorder);
    }
    const auto min_cb = r.Ue(), diff_cb = r.Ue(), min_tb = r.Ue(), diff_tb = r.Ue(), inter = r.Ue(), intra = r.Ue();
    if (!r.Good() || min_cb > 3 || diff_cb > 3 || min_tb > 3 || diff_tb > 3 || inter > 4 || intra > 4) return S::Invalid;
    const unsigned min_cb_log2 = min_cb + 3, ctb_log2 = min_cb_log2 + diff_cb, min_tb_log2 = min_tb + 2;
    if (ctb_log2 < 4 || ctb_log2 > 6 || min_tb_log2 >= min_cb_log2 || min_tb_log2 + diff_tb > (std::min)(ctb_log2, 5u) ||
        inter > ctb_log2 - min_tb_log2 || intra > ctb_log2 - min_tb_log2 ||
        width % (1u << min_cb_log2) || height % (1u << min_cb_log2)) return S::Invalid;
    s.log2_min_cb_minus3 = static_cast<std::uint8_t>(min_cb); s.log2_diff_max_min_cb = static_cast<std::uint8_t>(diff_cb);
    s.log2_min_tb_minus2 = static_cast<std::uint8_t>(min_tb); s.log2_diff_max_min_tb = static_cast<std::uint8_t>(diff_tb);
    s.max_transform_depth_inter = static_cast<std::uint8_t>(inter); s.max_transform_depth_intra = static_cast<std::uint8_t>(intra);
    s.scaling_list_enabled = r.Bits(1) != 0;
    if (s.scaling_list_enabled && r.Bits(1)) { if (!ScalingListData(r, s.scaling)) return S::Invalid; }
    else FillLists(s.scaling, !s.scaling_list_enabled);
    s.amp = r.Bits(1) != 0; s.sao = r.Bits(1) != 0; s.pcm = r.Bits(1) != 0;
    if (s.pcm) {
        const auto pcm_luma = r.Bits(4), pcm_chroma = r.Bits(4); const auto min_pcm = r.Ue(), diff_pcm = r.Ue();
        s.pcm_loop_filter_disabled = r.Bits(1) != 0;
        if (!r.Good() || pcm_luma > 7 + luma || pcm_chroma > 7 + chroma_depth || min_pcm > 2 || diff_pcm > 2) return S::Invalid;
        const unsigned min_pcm_log2 = min_pcm + 3, limit = (std::min)(ctb_log2, 5u);
        if (min_pcm_log2 < (std::min)(min_cb_log2, 5u) || min_pcm_log2 + diff_pcm > limit) return S::Invalid;
        s.pcm_bit_depth_luma_minus1 = static_cast<std::uint8_t>(pcm_luma); s.pcm_bit_depth_chroma_minus1 = static_cast<std::uint8_t>(pcm_chroma);
        s.log2_min_pcm_minus3 = static_cast<std::uint8_t>(min_pcm); s.log2_diff_max_min_pcm = static_cast<std::uint8_t>(diff_pcm);
    }
    const auto sets = r.Ue();
    if (!r.Good() || sets > 64) return S::Invalid;
    s.num_short_term_rps = static_cast<std::uint8_t>(sets);
    for (unsigned i = 0; i < sets; ++i) {
        status = ShortTermRps(r, s.rps, i, sets, s.max_dec_pic_buffering_minus1, s.rps[i]);
        if (status != S::Ready) return status;
    }
    s.long_term_present = r.Bits(1) != 0;
    if (s.long_term_present) {
        const auto count = r.Ue();
        if (!r.Good() || count > 32) return S::Invalid;
        s.num_long_term_sps = static_cast<std::uint8_t>(count);
        for (unsigned i = 0; i < count; ++i) {
            s.lt_poc_lsb[i] = static_cast<std::uint16_t>(r.Bits(s.poc_lsb_bits));
            if (r.Bits(1)) s.lt_used |= 1u << i;
        }
    }
    s.temporal_mvp = r.Bits(1) != 0; s.strong_intra_smoothing = r.Bits(1) != 0;
    s.vui_flag_bit = static_cast<std::uint32_t>(r.Position());
    if (r.Bits(1)) { status = Vui(r, s); if (status != S::Ready) return status; }
    s.vui_end_bit = static_cast<std::uint32_t>(r.Position());
    bool extension_data = false;
    if (r.Bits(1)) {
        const bool range = r.Bits(1) != 0, multilayer = r.Bits(1) != 0, three_d = r.Bits(1) != 0, scc = r.Bits(1) != 0;
        extension_data = r.Bits(4) != 0; // sps_extension_data_flag content is ignored (7.4.3.2.1).
        if (range || three_d || scc) return S::Unsupported;
        if (multilayer) (void)r.Bits(1);
    }
    if (extension_data ? !r.Good() : !r.Finish()) return S::Invalid;
    s.valid = true;
    // Repeated identical in-band SPS is legal; changing the active SPS needs a new decoder session.
    // The sample shape is not decoder configuration: a repeat that differs only there is accepted, and the shape the
    // session started with stands. (A colour change is still a changed set here, as it always was.)
    if (started_ && id == active_sps_) {
        s.sar_width = sps_[id].sar_width; s.sar_height = sps_[id].sar_height;
        return s == sps_[id] ? S::Ready : S::Unsupported;
    }
    sps_[id] = s; return S::Ready;
}
SyntaxStatus HevcSyntaxParser::Pps(RbspReader& r) noexcept {
    auto& p = *pps_scratch_; p = {};
    const auto id = r.Ue(), sps = r.Ue();
    if (!r.Good() || id > 63 || sps > 15) return S::Invalid;
    p.sps = static_cast<std::uint8_t>(sps);
    p.dependent_slices = r.Bits(1) != 0; p.output_flag_present = r.Bits(1) != 0;
    p.extra_slice_header_bits = static_cast<std::uint8_t>(r.Bits(3));
    p.sign_data_hiding = r.Bits(1) != 0; p.cabac_init_present = r.Bits(1) != 0;
    const auto l0 = r.Ue(), l1 = r.Ue(); const auto qp = r.Se();
    if (!r.Good() || l0 > 14 || l1 > 14 || qp < -38 || qp > 25) return S::Invalid;
    p.refs_l0_minus1 = static_cast<std::uint8_t>(l0); p.refs_l1_minus1 = static_cast<std::uint8_t>(l1);
    p.init_qp_minus26 = static_cast<std::int8_t>(qp);
    p.constrained_intra = r.Bits(1) != 0; p.transform_skip = r.Bits(1) != 0; p.cu_qp_delta = r.Bits(1) != 0;
    if (p.cu_qp_delta) {
        const auto depth = r.Ue(); if (!r.Good() || depth > 3) return S::Invalid;
        p.diff_cu_qp_delta_depth = static_cast<std::uint8_t>(depth);
    }
    const auto cb = r.Se(), cr = r.Se();
    if (!r.Good() || cb < -12 || cb > 12 || cr < -12 || cr > 12) return S::Invalid;
    p.cb_qp_offset = static_cast<std::int8_t>(cb); p.cr_qp_offset = static_cast<std::int8_t>(cr);
    p.slice_chroma_qp_offsets = r.Bits(1) != 0; p.weighted_pred = r.Bits(1) != 0; p.weighted_bipred = r.Bits(1) != 0;
    p.transquant_bypass = r.Bits(1) != 0; p.tiles = r.Bits(1) != 0; p.entropy_sync = r.Bits(1) != 0;
    if (p.tiles) {
        const auto columns = r.Ue(), rows = r.Ue();
        if (!r.Good() || (!columns && !rows)) return S::Invalid;
        if (columns > 19 || rows > 21) return S::Unsupported; // DXVA_PicParams_HEVC capacity.
        p.tile_columns_minus1 = static_cast<std::uint8_t>(columns); p.tile_rows_minus1 = static_cast<std::uint8_t>(rows);
        p.uniform_spacing = r.Bits(1) != 0;
        if (!p.uniform_spacing) {
            for (unsigned i = 0; i < columns; ++i) {
                const auto w = r.Ue(); if (!r.Good() || w > 511) return S::Invalid;
                p.column_width_minus1[i] = static_cast<std::uint16_t>(w);
            }
            for (unsigned i = 0; i < rows; ++i) {
                const auto h = r.Ue(); if (!r.Good() || h > 511) return S::Invalid;
                p.row_height_minus1[i] = static_cast<std::uint16_t>(h);
            }
        }
        p.loop_filter_across_tiles = r.Bits(1) != 0;
    }
    p.loop_filter_across_slices = r.Bits(1) != 0;
    if (r.Bits(1)) {
        p.deblocking_override = r.Bits(1) != 0; p.deblocking_disabled = r.Bits(1) != 0;
        if (!p.deblocking_disabled) {
            const auto beta = r.Se(), tc = r.Se();
            if (!r.Good() || beta < -6 || beta > 6 || tc < -6 || tc > 6) return S::Invalid;
            p.beta_offset_div2 = static_cast<std::int8_t>(beta); p.tc_offset_div2 = static_cast<std::int8_t>(tc);
        }
    }
    p.scaling_list_present = r.Bits(1) != 0;
    if (p.scaling_list_present && !ScalingListData(r, p.scaling)) return S::Invalid;
    p.lists_modification = r.Bits(1) != 0;
    const auto merge = r.Ue();
    if (!r.Good() || merge > 4) return S::Invalid;
    p.log2_parallel_merge_minus2 = static_cast<std::uint8_t>(merge);
    p.slice_header_extension = r.Bits(1) != 0;
    bool extension_data = false;
    if (r.Bits(1)) {
        const bool range = r.Bits(1) != 0, multilayer = r.Bits(1) != 0, three_d = r.Bits(1) != 0, scc = r.Bits(1) != 0;
        extension_data = r.Bits(4) != 0;
        if (range || multilayer || three_d || scc) return S::Unsupported;
    }
    if (extension_data ? !r.Good() : !r.Finish()) return S::Invalid;
    p.valid = true; pps_[id] = p; return S::Ready;
}
SyntaxStatus HevcSyntaxParser::Activate(unsigned id) noexcept {
    const auto& pps = pps_[id];
    if (!pps.valid || !sps_[pps.sps].valid) return S::Invalid;
    if (started_ && pps.sps != active_sps_) return S::Unsupported;
    const auto& sps = sps_[pps.sps];
    const unsigned ctb = sps.log2_min_cb_minus3 + 3u + sps.log2_diff_max_min_cb;
    const unsigned columns = (sps.width + (1u << ctb) - 1) >> ctb, rows = (sps.height + (1u << ctb) - 1) >> ctb;
    if (pps.init_qp_minus26 < -(26 + 6 * sps.bit_depth_luma_minus8) || pps.diff_cu_qp_delta_depth > sps.log2_diff_max_min_cb ||
        pps.log2_parallel_merge_minus2 + 2u > ctb || (pps.scaling_list_present && !sps.scaling_list_enabled)) return S::Invalid;
    if (pps.tiles) {
        if (pps.tile_columns_minus1 >= columns || pps.tile_rows_minus1 >= rows) return S::Invalid;
        if (!pps.uniform_spacing) {
            unsigned width = 0, height = 0;
            for (unsigned i = 0; i < pps.tile_columns_minus1; ++i) width += pps.column_width_minus1[i] + 1u;
            for (unsigned i = 0; i < pps.tile_rows_minus1; ++i) height += pps.row_height_minus1[i] + 1u;
            if (width >= columns || height >= rows) return S::Invalid;
        }
    }
    picture_->sps = &sps; picture_->pps = &pps; picture_->pps_id = id;
    return S::Ready;
}
SyntaxStatus HevcSyntaxParser::Slice(std::span<const std::byte> nal, unsigned type, unsigned temporal_id, bool first,
                                     std::uint32_t& address) noexcept {
    RbspReader r(nal.subspan(2));
    const bool first_segment = r.Bits(1) != 0;
    if (!r.Good()) return S::Invalid;
    if (first && !first_segment) return S::Invalid;     // Leading slice segments are missing.
    if (!first && first_segment) return S::Unsupported; // More than one picture in the access unit.
    auto& out = *picture_;
    if (!first && (type != out.nal_type || temporal_id != out.temporal_id)) return S::Invalid;
    const bool irap = Irap(type), idr = Idr(type);
    if (irap) (void)r.Bits(1); // no_output_of_prior_pics_flag: output policy belongs downstream.
    const auto pps_id = r.Ue();
    if (!r.Good() || pps_id > 63) return S::Invalid;
    if (first) { const auto status = Activate(pps_id); if (status != S::Ready) return status; }
    else if (pps_id != out.pps_id) return S::Invalid;
    const auto& pps = *out.pps; const auto& sps = *out.sps;
    const unsigned ctb = sps.log2_min_cb_minus3 + 3u + sps.log2_diff_max_min_cb;
    const std::uint32_t ctbs = ((sps.width + (1u << ctb) - 1) >> ctb) * ((sps.height + (1u << ctb) - 1) >> ctb);
    address = 0;
    bool dependent = false;
    if (!first_segment) {
        if (pps.dependent_slices) dependent = r.Bits(1) != 0;
        address = r.Bits(CeilLog2(ctbs));
        if (!r.Good() || address >= ctbs) return S::Invalid;
    }
    if (dependent) return r.Good() ? S::Ready : S::Invalid; // Header fields are inherited.
    r.Skip(pps.extra_slice_header_bits);
    const auto slice_type = r.Ue();
    if (!r.Good() || slice_type > 2 || (irap && slice_type != 2)) return S::Invalid;
    auto& s = first ? *first_ : *slice_;
    s = {}; s.pps = pps_id;
    if (pps.output_flag_present) s.output = r.Bits(1) != 0;
    if (!idr) {
        s.poc_lsb = r.Bits(sps.poc_lsb_bits);
        if (!r.Bits(1)) {
            const auto begin = r.Position();
            const auto status = ShortTermRps(r, sps.rps, sps.num_short_term_rps, sps.num_short_term_rps,
                                             sps.max_dec_pic_buffering_minus1, s.rps);
            if (status != S::Ready) return status;
            s.rps_bits = static_cast<std::uint16_t>(r.Position() - begin);
        } else {
            if (!sps.num_short_term_rps) return S::Invalid;
            const auto index = r.Bits(CeilLog2(sps.num_short_term_rps));
            if (!r.Good() || index >= sps.num_short_term_rps) return S::Invalid;
            s.rps = sps.rps[index];
        }
        if (sps.long_term_present) {
            const auto from_sps = sps.num_long_term_sps ? r.Ue() : 0u;
            const auto explicit_count = r.Ue();
            if (!r.Good() || from_sps > sps.num_long_term_sps || explicit_count > 15 ||
                from_sps + explicit_count + s.rps.negative + s.rps.positive > sps.max_dec_pic_buffering_minus1) return S::Invalid;
            s.lt_count = static_cast<std::uint8_t>(from_sps + explicit_count);
            for (unsigned i = 0; i < s.lt_count; ++i) {
                bool used = false;
                if (i < from_sps) {
                    const auto index = sps.num_long_term_sps > 1 ? r.Bits(CeilLog2(sps.num_long_term_sps)) : 0u;
                    if (index >= sps.num_long_term_sps) return S::Invalid;
                    s.lt_lsb[i] = sps.lt_poc_lsb[index]; used = ((sps.lt_used >> index) & 1u) != 0;
                } else {
                    s.lt_lsb[i] = static_cast<std::uint16_t>(r.Bits(sps.poc_lsb_bits)); used = r.Bits(1) != 0;
                }
                if (used) s.lt_used = static_cast<std::uint16_t>(s.lt_used | (1u << i));
                std::uint64_t cycle = 0;
                if (r.Bits(1)) { s.lt_msb = static_cast<std::uint16_t>(s.lt_msb | (1u << i)); cycle = r.Ue(); }
                if (i != 0 && i != from_sps) cycle += s.lt_cycle[i - 1]; // DeltaPocMsbCycleLt (7-52).
                if (!r.Good() || cycle > UINT32_MAX) return S::Invalid;
                s.lt_cycle[i] = static_cast<std::uint32_t>(cycle);
            }
        }
        if (sps.temporal_mvp) s.temporal_mvp = r.Bits(1) != 0;
    }
    if (!r.Good()) return S::Invalid;
    const unsigned current = Count(s.rps.used_s0) + Count(s.rps.used_s1) + Count(s.lt_used);
    if (current > 8 || (irap ? current != 0 : (slice_type != 2 && current == 0))) return S::Invalid;
    out.intra = out.intra && slice_type == 2;
    if (!first) return s == *first_ ? S::Ready : S::Invalid;
    out.nal_type = static_cast<std::uint8_t>(type); out.temporal_id = static_cast<std::uint8_t>(temporal_id);
    out.irap = irap; out.idr = idr; out.output = s.output;
    out.short_term_rps_bits = s.rps_bits; out.reference_rps_delta_pocs = s.rps.reference_delta_pocs;
    // A sub-layer non-reference picture at the highest sub-layer can never be referenced.
    out.retain = sps.max_dec_pic_buffering_minus1 != 0 && !(SubLayerNonReference(type) && temporal_id == sps.max_sub_layers_minus1);
    return References(s);
}
SyntaxStatus HevcSyntaxParser::References(const SliceState& s) noexcept {
    auto& out = *picture_; const auto& sps = *out.sps;
    const unsigned type = out.nal_type;
    out.no_rasl_output = out.idr || (type >= BlaWLp && type <= BlaNLp) || (type == Cra && (!started_ || after_eos_));
    // 8.3.1 picture order count.
    const std::int64_t max = std::int64_t{1} << sps.poc_lsb_bits, lsb = s.poc_lsb;
    std::int64_t msb = 0;
    if (!(out.irap && out.no_rasl_output)) {
        const std::int64_t previous = previous_poc_, previous_lsb = previous & (max - 1);
        msb = previous - previous_lsb;
        if (lsb < previous_lsb && previous_lsb - lsb >= max / 2) msb += max;
        else if (lsb > previous_lsb && lsb - previous_lsb > max / 2) msb -= max;
    }
    const auto poc = msb + lsb;
    if (poc < INT32_MIN || poc > INT32_MAX) return S::Unsupported;
    out.poc = static_cast<std::int32_t>(poc);
    for (auto& keep : keep_) keep = 0;
    out.reference_count = 0; out.st_curr_before_count = out.st_curr_after_count = out.lt_curr_count = 0;
    if (out.irap && out.no_rasl_output) return S::Ready; // Every DPB picture becomes unused (8.3.2).
    // 8.3.2: long-term candidates are searched among all reference pictures, then marked, then short-term.
    std::uint8_t before[8]{}, after[8]{}, lt[8]{};
    unsigned before_count = 0, after_count = 0, lt_count = 0;
    for (unsigned i = 0; i < s.lt_count; ++i) {
        const bool msb_present = ((s.lt_msb >> i) & 1u) != 0, used = ((s.lt_used >> i) & 1u) != 0;
        std::int64_t target = s.lt_lsb[i];
        if (msb_present) target += poc - static_cast<std::int64_t>(s.lt_cycle[i]) * max - (poc & (max - 1));
        unsigned match = 16;
        for (unsigned j = 0; j < refs_; ++j) {
            const std::int64_t candidate = msb_present ? dpb_[j].poc : dpb_[j].poc & (max - 1);
            if (candidate != target) continue;
            if (match != 16) return S::Invalid; // Ambiguous LSB-only long-term reference.
            match = j;
        }
        if (match == 16) { if (used) return S::Invalid; continue; } // Missing following entries are legal.
        if (keep_[match]) return S::Invalid;
        keep_[match] = 2;
        if (used) lt[lt_count++] = static_cast<std::uint8_t>(match);
    }
    auto short_term = [&](std::int32_t delta, bool used, std::uint8_t* list, unsigned& count) noexcept {
        const auto target = poc + delta;
        for (unsigned j = 0; j < refs_; ++j) {
            if (dpb_[j].long_term || keep_[j] == 2 || dpb_[j].poc != target) continue;
            if (keep_[j]) return false;
            keep_[j] = 1;
            if (used) list[count++] = static_cast<std::uint8_t>(j);
            return true;
        }
        return !used;
    };
    for (unsigned i = 0; i < s.rps.negative; ++i)
        if (!short_term(s.rps.delta_s0[i], ((s.rps.used_s0 >> i) & 1u) != 0, before, before_count)) return S::Invalid;
    for (unsigned i = 0; i < s.rps.positive; ++i)
        if (!short_term(s.rps.delta_s1[i], ((s.rps.used_s1 >> i) & 1u) != 0, after, after_count)) return S::Invalid;
    std::uint8_t index[16]{};
    unsigned count = 0;
    for (unsigned j = 0; j < refs_; ++j) {
        if (!keep_[j]) continue;
        if (count == 15) return S::Invalid;
        index[j] = static_cast<std::uint8_t>(count);
        out.references[count++] = {dpb_[j].surface, dpb_[j].poc, keep_[j] == 2};
    }
    for (unsigned i = 0; i < before_count; ++i) out.st_curr_before[i] = index[before[i]];
    for (unsigned i = 0; i < after_count; ++i) out.st_curr_after[i] = index[after[i]];
    for (unsigned i = 0; i < lt_count; ++i) out.lt_curr[i] = index[lt[i]];
    out.st_curr_before_count = static_cast<std::uint8_t>(before_count);
    out.st_curr_after_count = static_cast<std::uint8_t>(after_count);
    out.lt_curr_count = static_cast<std::uint8_t>(lt_count);
    out.reference_count = count;
    return S::Ready;
}
SyntaxStatus HevcSyntaxParser::Parse(std::span<const std::byte> au, unsigned length) noexcept {
    if (!picture_ || pending_) return S::Busy;
    if (length != 1 && length != 2 && length != 4) return S::Invalid;
    *picture_ = {};
    std::size_t offset = 0;
    std::uint32_t last_address = 0;
    bool vcl = false, discard = false, eos = false;
    while (offset < au.size()) {
        if (au.size() - offset < length) return S::Invalid;
        std::uint32_t size = 0;
        for (unsigned i = 0; i < length; ++i) size = (size << 8) | std::to_integer<unsigned>(au[offset++]);
        if (size < 2 || size > au.size() - offset || offset > UINT32_MAX) return S::Invalid;
        const auto nal = au.subspan(offset, size);
        const auto h0 = std::to_integer<unsigned>(nal[0]), h1 = std::to_integer<unsigned>(nal[1]);
        const unsigned type = (h0 >> 1) & 63, temporal_plus1 = h1 & 7;
        if ((h0 & 128) || !temporal_plus1) return S::Invalid;
        if ((h0 & 1) || (h1 >> 3)) { offset += size; continue; } // nuh_layer_id > 0 is not base-layer data.
        if (type <= RaslR || (type >= BlaWLp && type <= Cra)) {
            if (eos) return S::Invalid;
            if (!vcl) {
                if (Irap(type) && temporal_plus1 != 1) return S::Invalid;
                if ((!started_ || after_eos_) && !Irap(type)) return S::NeedIdr; // Random-access point required.
                discard = Rasl(type) && discard_rasl_;
            }
            if (!discard) {
                if (picture_->slice_count == 256) return S::Unsupported;
                std::uint32_t address = 0;
                const auto status = Slice(nal, type, temporal_plus1 - 1, !vcl, address);
                if (status != S::Ready) return status;
                if (vcl && address <= last_address) return S::Invalid;
                last_address = address;
                picture_->slices[picture_->slice_count++] = {static_cast<std::uint32_t>(offset), size};
            }
            vcl = true;
        } else if (type <= 31) return S::Unsupported; // Reserved VCL NAL unit types.
        else if (type <= PpsNut) {
            if (vcl) return S::Invalid;
            const auto status = Nal(nal); if (status != S::Ready) return status;
        } else if (type == AudNut) { if (vcl) return S::Invalid; }
        else if (type == EosNut || type == EobNut) { if (!vcl) return S::Invalid; eos = true; }
        // Filler data, SEI, reserved and unspecified non-VCL units are ignored.
        offset += size;
    }
    if (!vcl) return S::Invalid;
    if (discard) { if (eos) after_eos_ = true; return S::Discard; }
    picture_->end_of_sequence = eos;
    pending_ = true; return S::Ready;
}
bool HevcSyntaxParser::Commit(platform::SurfaceHandle surface, std::span<platform::SurfaceHandle> evicted, unsigned& count) noexcept {
    count = 0;
    if (!pending_ || evicted.size() < 16) return false;
    const auto& p = *picture_;
    unsigned kept = 0;
    for (unsigned i = 0; i < refs_; ++i) {
        if (!keep_[i]) { evicted[count++] = dpb_[i].surface; continue; }
        auto entry = dpb_[i]; entry.long_term = keep_[i] == 2; dpb_[kept++] = entry;
    }
    refs_ = kept; // At most 15 by construction of the reference list.
    if (p.retain) dpb_[refs_++] = {surface, p.poc, false};
    if (!p.temporal_id && !Rasl(p.nal_type) && !Radl(p.nal_type) && !SubLayerNonReference(p.nal_type)) previous_poc_ = p.poc;
    if (p.irap) discard_rasl_ = p.no_rasl_output;
    after_eos_ = p.end_of_sequence;
    active_sps_ = p.pps->sps;
    started_ = true; pending_ = false; return true;
}
bool HevcSyntaxParser::Reset(std::span<platform::SurfaceHandle> released, unsigned& count) noexcept {
    count = 0;
    if (released.size() < refs_) return false;
    for (unsigned i = 0; i < refs_; ++i) released[count++] = dpb_[i].surface;
    refs_ = 0; started_ = pending_ = discard_rasl_ = after_eos_ = false; previous_poc_ = 0;
    return true;
}
}
