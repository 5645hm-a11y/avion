#include "media/codec/BitstreamPerception.hpp"
#include "media/codec/Rbsp.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace media::codec {
namespace {
RbspReader Reader(const std::uint8_t* p, std::uint32_t n) noexcept { return RbspReader{std::span<const std::byte>{reinterpret_cast<const std::byte*>(p), n}}; }
double Smooth(double e0, double e1, double x) noexcept { const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3 - 2 * t); }
unsigned CeilLog2(std::uint32_t v) noexcept { unsigned n = 0; while ((std::uint64_t{1} << n) < v) ++n; return n; }
// An H.264 scaling list (7.3.2.1.1.1): only read past.
void SkipScalingList(RbspReader& r, unsigned size) noexcept {
    std::int32_t last = 8, next = 8;
    for (unsigned j = 0; j < size && r.Good(); ++j) {
        if (next) next = (last + r.Se() + 256) % 256;
        last = next ? next : last;
    }
}
// HEVC profile_tier_level (7.3.3): read past.
void SkipProfileTierLevel(RbspReader& r, unsigned max_sub_layers_minus1) noexcept {
    r.Skip(88); r.Skip(8); // general_profile_space .. general_inbld/reserved (88 bits), general_level_idc.
    bool profile[8]{}, level[8]{};
    for (unsigned i = 0; i < max_sub_layers_minus1; ++i) { profile[i] = r.Bits(1) != 0; level[i] = r.Bits(1) != 0; }
    if (max_sub_layers_minus1 > 0) for (unsigned i = max_sub_layers_minus1; i < 8; ++i) r.Skip(2);
    for (unsigned i = 0; i < max_sub_layers_minus1; ++i) { if (profile[i]) r.Skip(88); if (level[i]) r.Skip(8); }
}
// HEVC scaling_list_data (7.3.4): read past.
void SkipScalingListData(RbspReader& r) noexcept {
    for (unsigned size = 0; size < 4; ++size)
        for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
            if (!r.Bits(1)) { (void)r.Ue(); continue; }
            const unsigned coefficients = (std::min)(64u, 1u << (4 + (size << 1)));
            if (size > 1) (void)r.Se();
            for (unsigned i = 0; i < coefficients && r.Good(); ++i) (void)r.Se();
        }
}
// HEVC st_ref_pic_set(index) (7.3.7) into `out`, its deltas derived (7.4.8) when predicted from an earlier set of the SPS
// (fetch(i): its set i); `count`: num_short_term_ref_pic_sets (a slice's own set has index == count). False when it cannot
// be followed.
template <class Fetch>
bool ReadReferenceSet(RbspReader& r, unsigned index, unsigned count, const Fetch& fetch, BitstreamPerception::ReferenceSet& out) noexcept {
    BitstreamPerception::ReferenceSet set;
    if (index != 0 && r.Bits(1)) {
        const unsigned back = index == count ? r.Ue() + 1 : 1;
        if (back > index) return false;
        const BitstreamPerception::ReferenceSet ref = fetch(index - back);
        const int sign = r.Bits(1) ? -1 : 1, delta_rps = sign * static_cast<int>(r.Ue() + 1);
        const unsigned n = ref.negative + ref.positive;
        bool used[33]{}, use[33]{};
        for (unsigned j = 0; j <= n; ++j) { used[j] = r.Bits(1) != 0; use[j] = used[j] || r.Bits(1) != 0; }
        // (7-61): the negatives, nearest first; (7-62): the positives.
        std::int16_t negative[32]{}, positive[32]{}; bool used_negative[32]{}, used_positive[32]{};
        unsigned i = 0;
        for (int j = ref.positive - 1; j >= 0; --j) {
            const int d = ref.delta[ref.negative + j] + delta_rps;
            if (d < 0 && use[ref.negative + j] && i < 16) { negative[i] = static_cast<std::int16_t>(d); used_negative[i++] = used[ref.negative + j]; }
        }
        if (delta_rps < 0 && use[n] && i < 16) { negative[i] = static_cast<std::int16_t>(delta_rps); used_negative[i++] = used[n]; }
        for (unsigned j = 0; j < ref.negative; ++j) {
            const int d = ref.delta[j] + delta_rps;
            if (d < 0 && use[j] && i < 16) { negative[i] = static_cast<std::int16_t>(d); used_negative[i++] = used[j]; }
        }
        set.negative = static_cast<std::uint8_t>(i);
        i = 0;
        for (int j = ref.negative - 1; j >= 0; --j) {
            const int d = ref.delta[j] + delta_rps;
            if (d > 0 && use[j] && i < 16) { positive[i] = static_cast<std::int16_t>(d); used_positive[i++] = used[j]; }
        }
        if (delta_rps > 0 && use[n] && i < 16) { positive[i] = static_cast<std::int16_t>(delta_rps); used_positive[i++] = used[n]; }
        for (unsigned j = 0; j < ref.positive; ++j) {
            const int d = ref.delta[ref.negative + j] + delta_rps;
            if (d > 0 && use[ref.negative + j] && i < 16) { positive[i] = static_cast<std::int16_t>(d); used_positive[i++] = used[ref.negative + j]; }
        }
        set.positive = static_cast<std::uint8_t>(i);
        for (unsigned k = 0; k < set.negative; ++k) { set.delta[k] = negative[k]; if (used_negative[k]) set.used |= 1u << k; }
        for (unsigned k = 0; k < set.positive; ++k) { set.delta[set.negative + k] = positive[k]; if (used_positive[k]) set.used |= 1u << (set.negative + k); }
    } else {
        const auto negatives = r.Ue(), positives = r.Ue();
        if (negatives > 16 || positives > 16) return false;
        set.negative = static_cast<std::uint8_t>(negatives); set.positive = static_cast<std::uint8_t>(positives);
        int poc = 0;
        for (unsigned k = 0; k < negatives; ++k) { poc -= static_cast<int>(r.Ue() + 1); set.delta[k] = static_cast<std::int16_t>(poc); if (r.Bits(1)) set.used |= 1u << k; }
        poc = 0;
        for (unsigned k = 0; k < positives; ++k) { poc += static_cast<int>(r.Ue() + 1); set.delta[negatives + k] = static_cast<std::int16_t>(poc); if (r.Bits(1)) set.used |= 1u << (negatives + k); }
    }
    if (!r.Good()) return false;
    out = set;
    return true;
}
// H.264 ref_pic_list_modification's loop for one list (7.3.3.1).
void SkipListModification(RbspReader& r) noexcept {
    if (!r.Bits(1)) return;
    for (unsigned i = 0; i < 64 && r.Good(); ++i) {
        const auto idc = r.Ue();
        if (idc == 3) return;
        (void)r.Ue(); // abs_diff_pic_num_minus1 or long_term_pic_num.
    }
}
}

bool BitstreamPerception::Configure(std::uint32_t codec, const std::uint8_t* config, std::uint32_t config_bytes, std::uint32_t nal_length_bytes) noexcept {
    codec_ = 0;
    if (codec != kH264 && codec != kHevc) return false;
    for (auto& s : sps264_) s = {};
    for (auto& p : pps264_) p = {};
    for (auto& s : spshevc_) s = {};
    for (auto& p : ppshevc_) p = {};
    width_ = height_ = 0; last_type_ = 'I'; last_qp_ = -1; entry_count_ = delta_count_ = 0;
    ResetStatistics();
    codec_ = codec; // ParameterSet reads by it.
    std::uint32_t length = nal_length_bytes;
    const auto fail = [this] { codec_ = 0; return false; };
    if (config && config_bytes) {
        // avcC (ISO/IEC 14496-15 5.3.3.1) or hvcC (8.3.3.1): the length size and the parameter sets.
        const auto sets = [&](std::uint32_t& at, unsigned count) {
            for (unsigned i = 0; i < count; ++i) {
                if (at + 2 > config_bytes) return false;
                const std::uint32_t n = (std::uint32_t{config[at]} << 8) | config[at + 1]; at += 2;
                if (at + n > config_bytes) return false;
                (void)ParameterSet(config + at, n); at += n;
            }
            return true;
        };
        if (codec == kH264) {
            if (config_bytes < 7 || config[0] != 1) return fail();
            if (!length) length = (config[4] & 3u) + 1;
            std::uint32_t at = 6;
            if (!sets(at, config[5] & 31u)) return fail();
            if (at < config_bytes) { const unsigned pps = config[at++]; if (!sets(at, pps)) return fail(); }
        } else {
            if (config_bytes < 23 || config[0] != 1) return fail();
            if (!length) length = (config[21] & 3u) + 1;
            std::uint32_t at = 23;
            for (unsigned a = 0, arrays = config[22]; a < arrays; ++a) {
                if (at + 3 > config_bytes) return fail();
                const unsigned count = (unsigned{config[at + 1]} << 8) | config[at + 2]; at += 3;
                if (!sets(at, count)) return fail();
            }
        }
    }
    if (length != 1 && length != 2 && length != 4) return fail();
    length_ = length;
    return true;
}
void BitstreamPerception::ResetStatistics() noexcept {
    index_ = last_key_ = scheduled_ = period_ = 0; keyed_ = false; intra_ = 0; after_key_ = 0;
    anchor_distance_ = previous_distance_ = 0; anchor_burst_ = 1; quiet_ = drop_index_ = 0; drop_open_ = false;
    for (auto& m : inter_) m = 0;
    for (auto& p : pending_) p = {};
}
unsigned BitstreamPerception::Class(const BitstreamFrame& frame) noexcept {
    const unsigned place = frame.reference_distance ? (std::min)(CeilLog2(frame.reference_distance), 4u) : 5u + (std::min)(unsigned{frame.layer}, 2u);
    return (frame.type == 'B' ? kClasses / 2 : 0u) + place;
}
void BitstreamPerception::DropSets(unsigned id) noexcept {
    auto& s = spshevc_[id];
    if (s.sets_known && s.sets) {
        // Its entries and their deltas out; everything after them moves down (the SPSs that came later point lower).
        const unsigned e0 = s.set_first, e1 = e0 + s.sets;
        const unsigned d0 = entries_[e0].first, d1 = e1 < entry_count_ ? entries_[e1].first : delta_count_;
        for (unsigned e = e1; e < entry_count_; ++e) { entries_[e - (e1 - e0)] = entries_[e]; entries_[e - (e1 - e0)].first = static_cast<std::uint16_t>(entries_[e].first - (d1 - d0)); }
        for (unsigned d = d1; d < delta_count_; ++d) deltas_[d - (d1 - d0)] = deltas_[d];
        for (auto& other : spshevc_) if (&other != &s && other.sets_known && other.set_first >= e1) other.set_first = static_cast<std::uint16_t>(other.set_first - (e1 - e0));
        entry_count_ = static_cast<std::uint16_t>(entry_count_ - (e1 - e0)); delta_count_ = static_cast<std::uint16_t>(delta_count_ - (d1 - d0));
    }
    s.sets_known = false;
}
bool BitstreamPerception::StoreSet(const ReferenceSet& set) noexcept {
    const unsigned n = set.negative + set.positive;
    if (entry_count_ >= kSetEntries || delta_count_ + n > kDeltas) return false;
    auto& e = entries_[entry_count_++];
    e.first = delta_count_; e.negative = set.negative; e.positive = set.positive; e.used = set.used;
    for (unsigned k = 0; k < n; ++k) deltas_[delta_count_++] = set.delta[k];
    return true;
}
BitstreamPerception::ReferenceSet BitstreamPerception::LoadSet(unsigned entry) const noexcept {
    ReferenceSet set;
    if (entry >= entry_count_) return set;
    const auto& e = entries_[entry];
    set.negative = e.negative; set.positive = e.positive; set.used = e.used;
    for (unsigned k = 0; k < static_cast<unsigned>(e.negative + e.positive); ++k) set.delta[k] = deltas_[e.first + k];
    return set;
}
bool BitstreamPerception::ParameterSet(const std::uint8_t* nal, std::uint32_t bytes) noexcept {
    if (codec_ == kH264) {
        if (bytes < 2) return false;
        const unsigned type = nal[0] & 0x1Fu;
        auto r = Reader(nal + 1, bytes - 1);
        if (type == 7) {
            Sps264 s;
            const unsigned profile = r.Bits(8); r.Skip(16);
            const auto id = r.Ue();
            if (id > 31) return false;
            if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
                profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
                s.chroma = static_cast<std::uint8_t>(r.Ue());
                if (s.chroma == 3) s.separate = r.Bits(1) != 0;
                (void)r.Ue(); (void)r.Ue(); r.Skip(1); // Bit depths, qpprime_y_zero_transform_bypass.
                if (r.Bits(1)) for (unsigned i = 0; i < (s.chroma != 3 ? 8u : 12u); ++i) if (r.Bits(1)) SkipScalingList(r, i < 6 ? 16 : 64);
            }
            s.frame_bits = static_cast<std::uint8_t>(r.Ue() + 4);
            s.poc_type = static_cast<std::uint8_t>(r.Ue());
            if (s.poc_type == 0) s.poc_bits = static_cast<std::uint8_t>(r.Ue() + 4);
            else if (s.poc_type == 1) {
                s.always_zero = r.Bits(1) != 0; (void)r.Se(); (void)r.Se();
                const auto cycle = r.Ue(); if (cycle > 255) return false;
                for (unsigned i = 0; i < cycle; ++i) (void)r.Se();
            }
            (void)r.Ue(); r.Skip(1); // max_num_ref_frames, gaps_in_frame_num_value_allowed_flag.
            s.width_mbs = static_cast<std::uint16_t>(r.Ue() + 1); s.height_units = static_cast<std::uint16_t>(r.Ue() + 1);
            s.frame_mbs_only = r.Bits(1) != 0;
            if (!r.Good() || s.frame_bits > 16 || s.poc_bits > 16 || s.poc_type > 2) return false;
            s.valid = true; sps264_[id] = s;
            return true;
        }
        if (type == 8) {
            Pps264 p;
            const auto id = r.Ue(), sps = r.Ue();
            if (id > 255 || sps > 31) return false;
            p.sps = static_cast<std::uint8_t>(sps);
            p.entropy = r.Bits(1) != 0; p.bottom_poc = r.Bits(1) != 0;
            const auto groups = r.Ue() + 1;
            if (groups > 8) return false;
            if (groups > 1) {
                // Slice groups (FMO, 7.3.2.2): read past.
                const auto map = r.Ue();
                if (map == 0) for (unsigned g = 0; g < groups; ++g) (void)r.Ue();
                else if (map == 2) for (unsigned g = 0; g + 1 < groups; ++g) { (void)r.Ue(); (void)r.Ue(); }
                else if (map >= 3 && map <= 5) { r.Skip(1); (void)r.Ue(); }
                else if (map == 6) { const auto units = r.Ue() + 1; const unsigned bits = CeilLog2(groups); for (std::uint32_t u = 0; u < units && r.Good(); ++u) r.Skip(bits); }
            }
            p.refs0 = static_cast<std::uint8_t>((std::min)(r.Ue() + 1, 32u)); p.refs1 = static_cast<std::uint8_t>((std::min)(r.Ue() + 1, 32u));
            p.weighted = r.Bits(1) != 0; p.bipred = static_cast<std::uint8_t>(r.Bits(2));
            const auto qp = r.Se();
            (void)r.Se(); (void)r.Se(); r.Skip(2); // pic_init_qs, chroma_qp_index_offset, deblocking control, constrained intra.
            p.redundant = r.Bits(1) != 0;
            if (!r.Good() || qp < -26 || qp > 25) return false;
            p.qp = static_cast<std::int8_t>(26 + qp);
            p.valid = true; pps264_[id] = p;
            return true;
        }
        return false;
    }
    if (codec_ != kHevc || bytes < 3) return false;
    const unsigned type = (nal[0] >> 1) & 0x3Fu;
    auto r = Reader(nal + 2, bytes - 2);
    if (type == 33) {
        r.Skip(4);
        const unsigned sub_layers = r.Bits(3); r.Skip(1);
        SkipProfileTierLevel(r, sub_layers);
        const auto id = r.Ue();
        if (id > 15) return false;
        SpsHevc s;
        s.chroma = static_cast<std::uint8_t>(r.Ue());
        if (s.chroma == 3) s.separate = r.Bits(1) != 0;
        if (s.separate) s.chroma = 0; // ChromaArrayType.
        const auto width = r.Ue(), height = r.Ue();
        if (r.Bits(1)) { (void)r.Ue(); (void)r.Ue(); (void)r.Ue(); (void)r.Ue(); } // The conformance window.
        (void)r.Ue(); (void)r.Ue();                                                // Bit depths.
        s.poc_bits = static_cast<std::uint8_t>(r.Ue() + 4);
        const bool ordering = r.Bits(1) != 0;
        for (unsigned i = ordering ? 0 : sub_layers; i <= sub_layers; ++i) { (void)r.Ue(); (void)r.Ue(); (void)r.Ue(); }
        const auto min_cb = r.Ue() + 3, ctb = min_cb + r.Ue();
        if (!r.Good() || !width || !height || width > 16384 || height > 16384 || ctb < 4 || ctb > 6 || s.poc_bits > 16) return false;
        s.width = static_cast<std::uint16_t>(width); s.height = static_cast<std::uint16_t>(height);
        const std::uint32_t size = 1u << ctb, ctbs = ((width + size - 1) >> ctb) * ((height + size - 1) >> ctb);
        s.address_bits = static_cast<std::uint8_t>(CeilLog2(ctbs));
        // Its slices' type is known from here; their QP needs the rest (the reference picture sets). The id's earlier SPS and
        // its sets are replaced.
        DropSets(id);
        s.valid = true; spshevc_[id] = s;
        (void)r.Ue(); (void)r.Ue(); (void)r.Ue(); (void)r.Ue();                    // Transform sizes and depths.
        if (r.Bits(1) && r.Bits(1)) SkipScalingListData(r);                       // scaling_list_enabled, sps_scaling_list_data_present.
        r.Skip(1); s.sao = r.Bits(1) != 0;                                          // amp_enabled, sample_adaptive_offset_enabled.
        if (r.Bits(1)) { r.Skip(8); (void)r.Ue(); (void)r.Ue(); r.Skip(1); }       // PCM.
        const auto sets = r.Ue();
        if (sets > 64) return true;
        s.sets = static_cast<std::uint8_t>(sets); s.set_first = entry_count_;
        // Appended to the pool as they are read (a predicted set reads an earlier one of this SPS); taken back on a failure.
        const std::uint16_t entries = entry_count_, deltas = delta_count_;
        const auto abandon = [&] { entry_count_ = entries; delta_count_ = deltas; return true; };
        const auto fetch = [&](unsigned i) { return LoadSet(entries + i); };
        for (unsigned i = 0; i < sets; ++i) {
            ReferenceSet set;
            if (!ReadReferenceSet(r, i, sets, fetch, set) || !StoreSet(set)) return abandon();
        }
        s.long_term = r.Bits(1) != 0;
        if (s.long_term) {
            const auto count = r.Ue();
            if (count > 32) return abandon();
            s.long_terms = static_cast<std::uint8_t>(count);
            for (unsigned i = 0; i < count; ++i) { r.Skip(s.poc_bits); if (r.Bits(1)) s.long_term_used |= 1u << i; }
        }
        s.temporal_mvp = r.Bits(1) != 0;
        if (!r.Good()) return abandon();
        s.sets_known = true; spshevc_[id] = s;
        return true;
    }
    if (type == 34) {
        const auto id = r.Ue(), sps = r.Ue();
        if (id > 63 || sps > 15) return false;
        PpsHevc p; p.sps = static_cast<std::uint8_t>(sps);
        p.dependent = r.Bits(1) != 0; p.output_flag = r.Bits(1) != 0; p.extra_bits = static_cast<std::uint8_t>(r.Bits(3));
        r.Skip(1); p.cabac_init = r.Bits(1) != 0;                                   // sign_data_hiding, cabac_init_present.
        p.refs0 = static_cast<std::uint8_t>((std::min)(r.Ue() + 1, 16u)); p.refs1 = static_cast<std::uint8_t>((std::min)(r.Ue() + 1, 16u));
        const auto qp = r.Se();
        r.Skip(2);                                                                  // constrained_intra_pred, transform_skip.
        if (r.Bits(1)) (void)r.Ue();                                                // cu_qp_delta: diff_cu_qp_delta_depth.
        (void)r.Se(); (void)r.Se(); r.Skip(1);                                      // Chroma QP offsets, their slice override.
        p.weighted = r.Bits(1) != 0; p.bipred = r.Bits(1) != 0;
        r.Skip(1);                                                                  // transquant_bypass.
        const bool tiles = r.Bits(1) != 0; r.Skip(1);                               // entropy_coding_sync.
        if (tiles) {
            const auto columns = r.Ue() + 1, rows = r.Ue() + 1;
            if (columns > 64 || rows > 64) return false;
            if (!r.Bits(1)) { for (unsigned i = 0; i + 1 < columns; ++i) (void)r.Ue(); for (unsigned i = 0; i + 1 < rows; ++i) (void)r.Ue(); }
            r.Skip(1);                                                              // loop_filter_across_tiles.
        }
        r.Skip(1);                                                                  // loop_filter_across_slices.
        if (r.Bits(1)) { r.Skip(1); if (!r.Bits(1)) { (void)r.Se(); (void)r.Se(); } } // Deblocking: override, disabled, offsets.
        if (r.Bits(1)) SkipScalingListData(r);
        p.lists = r.Bits(1) != 0;
        if (!r.Good() || qp < -26 || qp > 25) return false;
        p.qp = static_cast<std::int8_t>(26 + qp);
        p.valid = true; ppshevc_[id] = p;
        return true;
    }
    return false;
}
bool BitstreamPerception::Slice264(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept {
    const unsigned nal_type = nal[0] & 0x1Fu, ref_idc = (nal[0] >> 5) & 3u;
    const bool idr = nal_type == 5;
    key = idr;
    slice_layer_ = ref_idc ? 0 : 1; slice_distance_ = 0;
    auto r = Reader(nal + 1, bytes - 1);
    (void)r.Ue(); // first_mb_in_slice.
    const auto slice_type = r.Ue() % 5; // 0 P, 1 B, 2 I, 3 SP, 4 SI.
    if (!r.Good() || slice_type > 4) return false;
    type = slice_type == 1 ? 'B' : (slice_type == 0 || slice_type == 3) ? 'P' : 'I';
    const auto pps_id = r.Ue();
    if (pps_id > 255 || !pps264_[pps_id].valid || !sps264_[pps264_[pps_id].sps].valid) return true; // The type is known; the QP not.
    const auto& p = pps264_[pps_id]; const auto& s = sps264_[p.sps];
    width_ = s.width_mbs * 16u; height_ = s.height_units * 16u * (s.frame_mbs_only ? 1u : 2u);
    if (s.separate) r.Skip(2);
    r.Skip(s.frame_bits);
    bool field = false;
    if (!s.frame_mbs_only) { field = r.Bits(1) != 0; if (field) r.Skip(1); }
    if (idr) (void)r.Ue();
    if (s.poc_type == 0) { r.Skip(s.poc_bits); if (p.bottom_poc && !field) (void)r.Se(); }
    if (s.poc_type == 1 && !s.always_zero) { (void)r.Se(); if (p.bottom_poc && !field) (void)r.Se(); }
    if (p.redundant) (void)r.Ue();
    const bool b = slice_type == 1, inter = slice_type == 0 || slice_type == 1 || slice_type == 3;
    if (b) r.Skip(1); // direct_spatial_mv_pred_flag.
    unsigned refs0 = p.refs0, refs1 = p.refs1;
    if (inter && r.Bits(1)) { refs0 = r.Ue() + 1; if (b) refs1 = r.Ue() + 1; }
    if (refs0 > 32 || refs1 > 32) return true;
    if (slice_type != 2 && slice_type != 4) { SkipListModification(r); if (b) SkipListModification(r); }
    if ((p.weighted && (slice_type == 0 || slice_type == 3)) || (p.bipred == 1 && b)) {
        // pred_weight_table (7.3.3.2).
        (void)r.Ue();
        const unsigned chroma = s.separate ? 0u : s.chroma;
        if (chroma) (void)r.Ue();
        for (unsigned list = 0; list < (b ? 2u : 1u); ++list)
            for (unsigned i = 0; i < (list ? refs1 : refs0) && r.Good(); ++i) {
                if (r.Bits(1)) { (void)r.Se(); (void)r.Se(); }
                if (chroma && r.Bits(1)) for (unsigned j = 0; j < 2; ++j) { (void)r.Se(); (void)r.Se(); }
            }
    }
    if (ref_idc) {
        // dec_ref_pic_marking (7.3.3.3).
        if (idr) r.Skip(2);
        else if (r.Bits(1))
            for (unsigned i = 0; i < 66 && r.Good(); ++i) {
                const auto op = r.Ue();
                if (op == 0) break;
                if (op == 1 || op == 3) (void)r.Ue();
                if (op == 2) (void)r.Ue();
                if (op == 3 || op == 6) (void)r.Ue();
                if (op == 4) (void)r.Ue();
            }
    }
    if (p.entropy && slice_type != 2 && slice_type != 4) (void)r.Ue(); // cabac_init_idc.
    const auto delta = r.Se();
    const int value = p.qp + delta;
    if (r.Good() && value >= 0 && value <= 51) qp = value;
    return true;
}
bool BitstreamPerception::SliceHevc(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept {
    const unsigned nal_type = (nal[0] >> 1) & 0x3Fu;
    key = nal_type >= 16 && nal_type <= 23;
    slice_layer_ = (nal[1] & 7u) ? (nal[1] & 7u) - 1 : 0; slice_distance_ = 0;
    auto r = Reader(nal + 2, bytes - 2);
    const bool first = r.Bits(1) != 0;
    if (key) r.Skip(1); // no_output_of_prior_pics_flag.
    const auto pps_id = r.Ue();
    if (!r.Good() || pps_id > 63 || !ppshevc_[pps_id].valid || !spshevc_[ppshevc_[pps_id].sps].valid) { type = key ? 'I' : 'P'; return true; }
    const auto& p = ppshevc_[pps_id]; const auto& s = spshevc_[p.sps];
    width_ = s.width; height_ = s.height;
    bool dependent = false;
    if (!first) { if (p.dependent) dependent = r.Bits(1) != 0; r.Skip(s.address_bits); }
    if (dependent) { type = last_type_; qp = last_qp_; slice_distance_ = last_distance_; return true; }
    r.Skip(p.extra_bits);
    const auto slice_type = r.Ue(); // 0 B, 1 P, 2 I.
    if (!r.Good() || slice_type > 2) { type = key ? 'I' : 'P'; return true; }
    type = slice_type == 0 ? 'B' : slice_type == 1 ? 'P' : 'I';
    last_type_ = type; last_qp_ = -1; last_distance_ = 0;
    // The QP: the rest of the header to slice_qp_delta (7.3.6.1), with its SPS's sets.
    if (!s.sets_known) return true;
    if (p.output_flag) r.Skip(1);
    if (s.separate) r.Skip(2);
    std::uint32_t in_use = 0; // NumPicTotalCurr.
    bool temporal_mvp = false;
    if (nal_type != 19 && nal_type != 20) {
        r.Skip(s.poc_bits);
        ReferenceSet set;
        if (!r.Bits(1)) {
            const auto fetch = [&](unsigned i) { return LoadSet(s.set_first + i); };
            if (!ReadReferenceSet(r, s.sets, s.sets, fetch, set)) return true;
        } else {
            const unsigned index = s.sets > 1 ? r.Bits(CeilLog2(s.sets)) : 0;
            if (!s.sets || index >= s.sets) return true;
            set = LoadSet(s.set_first + index);
        }
        if (!r.Good()) return true;
        for (std::uint32_t u = set.used; u; u &= u - 1) ++in_use;
        for (unsigned k = 0; k < static_cast<unsigned>(set.negative + set.positive); ++k)
            if (set.used >> k & 1u) {
                const unsigned d = static_cast<unsigned>(set.delta[k] < 0 ? -set.delta[k] : set.delta[k]);
                if (!slice_distance_ || d < slice_distance_) slice_distance_ = d;
            }
        last_distance_ = slice_distance_;
        if (s.long_term) {
            const auto from_sps = s.long_terms ? r.Ue() : 0u, own = r.Ue();
            if (from_sps > s.long_terms || own > 32) return true;
            for (unsigned i = 0; i < from_sps + own && r.Good(); ++i) {
                if (i < from_sps) { const unsigned index = s.long_terms > 1 ? r.Bits(CeilLog2(s.long_terms)) : 0; if (s.long_term_used >> index & 1u) ++in_use; }
                else { r.Skip(s.poc_bits); if (r.Bits(1)) ++in_use; }
                if (r.Bits(1)) (void)r.Ue(); // delta_poc_msb_cycle_lt.
            }
        }
        if (s.temporal_mvp) temporal_mvp = r.Bits(1) != 0;
    }
    if (s.sao) { r.Skip(1); if (s.chroma) r.Skip(1); }
    if (slice_type != 2) {
        unsigned refs0 = p.refs0, refs1 = slice_type == 0 ? p.refs1 : 0;
        if (r.Bits(1)) { refs0 = r.Ue() + 1; if (slice_type == 0) refs1 = r.Ue() + 1; }
        if (refs0 > 16 || refs1 > 16) return true;
        if (p.lists && in_use > 1) {
            const unsigned bits = CeilLog2(in_use);
            if (r.Bits(1)) r.Skip(bits * refs0);
            if (slice_type == 0 && r.Bits(1)) r.Skip(bits * refs1);
        }
        if (slice_type == 0) r.Skip(1); // mvd_l1_zero_flag.
        if (p.cabac_init) r.Skip(1);
        if (temporal_mvp) {
            bool from_l0 = true;
            if (slice_type == 0) from_l0 = r.Bits(1) != 0;
            if ((from_l0 && refs0 > 1) || (!from_l0 && refs1 > 1)) (void)r.Ue(); // collocated_ref_idx.
        }
        if ((p.weighted && slice_type == 1) || (p.bipred && slice_type == 0)) {
            // pred_weight_table (7.3.6.3): each list's luma flags, then its chroma flags, then the weights they announce.
            (void)r.Ue(); if (s.chroma) (void)r.Se();
            for (unsigned list = 0; list < (slice_type == 0 ? 2u : 1u); ++list) {
                const unsigned n = list ? refs1 : refs0;
                std::uint32_t luma = 0, chroma = 0;
                for (unsigned i = 0; i < n; ++i) if (r.Bits(1)) luma |= 1u << i;
                if (s.chroma) for (unsigned i = 0; i < n; ++i) if (r.Bits(1)) chroma |= 1u << i;
                for (unsigned i = 0; i < n && r.Good(); ++i) {
                    if (luma >> i & 1u) { (void)r.Se(); (void)r.Se(); }
                    if (chroma >> i & 1u) for (unsigned j = 0; j < 2; ++j) { (void)r.Se(); (void)r.Se(); }
                }
            }
        }
        (void)r.Ue(); // five_minus_max_num_merge_cand.
    }
    const auto delta = r.Se();
    const int value = p.qp + delta;
    if (r.Good() && value >= 0 && value <= 51) qp = last_qp_ = value;
    return true;
}
bool BitstreamPerception::Slice(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept {
    return codec_ == kH264 ? Slice264(nal, bytes, type, qp, key) : SliceHevc(nal, bytes, type, qp, key);
}
bool BitstreamPerception::Inspect(const std::uint8_t* unit, std::uint32_t bytes, BitstreamFrame& out) noexcept {
    out = {};
    out.index = index_;
    if (!codec_ || !unit) return false;
    // The NAL units: parameter sets taken, slices read to their type (and QP).
    double qp_sum = 0; unsigned qp_count = 0;
    bool any_b = false, any_p = false, any_i = false;
    for (std::uint32_t at = 0; at < bytes;) {
        if (bytes - at < length_) return false;
        std::uint32_t n = 0;
        for (unsigned i = 0; i < length_; ++i) n = (n << 8) | unit[at + i];
        at += length_;
        if (!n || n > bytes - at) return false;
        const std::uint8_t* nal = unit + at; at += n;
        const unsigned type = codec_ == kH264 ? nal[0] & 0x1Fu : (nal[0] >> 1) & 0x3Fu;
        const bool vcl = codec_ == kH264 ? type == 1 || type == 5 : type <= 31;
        if (!vcl) {
            const bool sets = codec_ == kH264 ? type == 7 || type == 8 : type == 33 || type == 34;
            if (sets && ParameterSet(nal, n)) out.parameter_sets = true;
            continue;
        }
        if (n < (codec_ == kH264 ? 2u : 3u)) continue;
        // HEVC's reserved VCL types (10-15, 22-31 but the IRAPs) carry no slice this reader knows.
        if (codec_ == kHevc && ((type >= 10 && type <= 15) || type >= 24)) continue;
        char slice = '?'; int qp = -1; bool key = false;
        if (!Slice(nal, n, slice, qp, key)) continue;
        ++out.slices; out.bytes += n; out.key = out.key || key;
        if (out.slices == 1 || slice_layer_ > out.layer) out.layer = static_cast<std::uint8_t>(slice_layer_);
        if (slice_distance_ && (!out.reference_distance || slice_distance_ < out.reference_distance)) out.reference_distance = static_cast<std::uint16_t>(slice_distance_);
        any_b = any_b || slice == 'B'; any_p = any_p || slice == 'P'; any_i = any_i || slice == 'I';
        if (qp >= 0) { qp_sum += qp; ++qp_count; }
    }
    if (!out.slices) return true; // Parameter sets, SEI, delimiters: no picture.
    out.type = any_b ? 'B' : any_p ? 'P' : 'I';
    if (qp_count == out.slices) out.qp = static_cast<int>(std::lround(qp_sum / qp_count));
    const double pixels = width_ && height_ ? static_cast<double>(width_) * height_ : 0;
    double complexity = pixels > 0 ? 8.0 * out.bytes / pixels : 8.0 * out.bytes;
    if (out.qp >= 0) complexity *= std::exp2((out.qp - 26) / 6.0);
    out.complexity = static_cast<float>(complexity);
    const std::uint32_t index = index_++;
    if (out.type == 'I') {
        // An intra picture: early for the GOP's beat (an encoder's scene-cut detection put it there), or unlike the last ones.
        // On the beat: a period after the last scheduled one, or after the last intra picture (encoders that restart their
        // GOP at a cut); later than the beat: a longer GOP, which becomes the beat. One on the beat is the encoder's clock
        // (2026-11-03): its cost against the last intra pictures' says how much the content drifted in a whole GOP (a fade
        // from black: 14x in 4 s), not that it cut, so it stays under kClockKey; a cut that lands exactly on the clock is
        // missed (the bitstream cannot tell it from a fade).
        const auto close = [](std::uint32_t a, std::uint32_t b) { return a + 1 >= b && a <= b + 1; };
        double p = 0;
        if (keyed_ && intra_ > 0) {
            const bool beat = period_ && (close(index, scheduled_ + period_) || close(index, last_key_ + period_));
            const bool early = period_ && !beat && index + 1 < scheduled_ + period_;
            p = early ? 0.9 : (beat ? kClockKey : 0.85) * Smooth(0.5, 1.5, std::fabs(std::log(complexity / intra_)));
            if (!period_ || (!beat && !early)) period_ = index - last_key_;
            if (!early) scheduled_ = index;
        } else scheduled_ = index;
        out.scene_cut = static_cast<float>(p);
        intra_ = intra_ > 0 ? 0.5 * intra_ + 0.5 * complexity : complexity;
        keyed_ = true; last_key_ = index; after_key_ = 0; quiet_ = 0; drop_open_ = false;
        return true;
    }
    const unsigned kind = Class(out);
    auto& pending = pending_[kind];
    if (pending.open) {
        // The candidate cut before this picture of its class, judged by it: a cut is followed by pictures of the new shot as
        // cheap as ever; a burst of new detail (a third of the picture churning) goes on costing. Burst: its cost becomes the
        // class's norm.
        pending.open = false;
        const bool persists = complexity >= kPersist * pending.complexity;
        out.revised = true; out.revised_index = pending.index;
        out.revised_scene_cut = static_cast<float>(persists ? pending.p * 0.3 : (std::max)(pending.p, 0.95));
        if (persists) inter_[kind] = 0.5 * inter_[kind] + 0.5 * pending.complexity;
    }
    const double ratio = intra_ > 0 ? complexity / intra_ : -1, burst = inter_[kind] > 0 ? complexity / inter_[kind] : 1;
    out.activity = ratio >= 0 ? static_cast<float>((std::min)(ratio, 1.0)) : -1.0f;
    out.burst = static_cast<float>(burst);
    // The first inter pictures after an intra one say more about the encoder's prediction around it (leading pictures
    // predicted across it, a longer reference distance) than about the scene: a hardware HEVC encoder spent 3x the bits there
    // with nothing new in view. A cut there would have been the intra picture itself.
    // Against its own class a burst of 1.5-3x is already rare (the mixed mean of every B picture needed 2-5x). In a
    // hierarchical GOP the anchor (its widest reference distance, first in decode order) is predicted across any cut in its
    // mini-GOP: a narrower picture that costs more while its anchor did not is a one-picture event (a flash), not a cut.
    const unsigned distance = out.reference_distance;
    const bool anchor = !distance || !after_key_ || distance > previous_distance_ || distance >= anchor_distance_;
    if (anchor) { anchor_distance_ = distance; anchor_burst_ = burst; }
    previous_distance_ = distance;
    double p = ratio >= 0 && after_key_ >= kAfterKey ? Smooth(0.3, 0.7, ratio) * Smooth(1.5, 3.0, burst) : 0;
    if (!anchor) p *= Smooth(1.2, 1.5, anchor_burst_);
    // A step down (2026-11-03): a cut into a much simpler shot costs less, not more (churning bubbles into still honey: every
    // class 4-8x under its mean). Two inter pictures in a row each kStepDown or more under their class's mean, after a level
    // held kSustain pictures (no intra picture, cut candidate or burst of new detail since) and busy (the class's mean a
    // tenth of an intra picture or more): a cut at the first, confirmed by the second. A burst that ends looks the same,
    // which is why the level must have held: a burst's own pictures keep resetting it.
    // Pictures of a class too cheap to be busy (a hierarchy's leaves) neither confirm nor break a drop; the confirmation comes
    // within kDropSpan pictures, and a dropped picture leaves its class's mean alone until then (the next of its class is
    // judged against the level before the drop, not one already halfway down).
    const bool busy = ratio >= 0 && after_key_ >= kAfterKey && inter_[kind] >= kBusy * intra_;
    const bool drop = busy && quiet_ >= kSustain && complexity * kStepDown <= inter_[kind];
    ++after_key_;
    out.scene_cut = static_cast<float>(p);
    out.motion_burst = (burst >= 2.0 && p < 0.5) || (out.revised && out.revised_scene_cut < 0.5f);
    if (drop_open_ && index > drop_index_ + kDropSpan) drop_open_ = false;
    bool hold = false;
    if (drop && drop_open_) {
        out.step_down = true; drop_open_ = false; quiet_ = 0;
        if (!out.revised) { out.revised = true; out.revised_index = drop_index_; out.revised_scene_cut = 0.95f; }
        else out.scene_cut = (std::max)(out.scene_cut, 0.95f); // The revision slot is taken: the cut is reported here.
    } else if (drop) {
        drop_open_ = true; drop_index_ = index; hold = true; ++quiet_;
    } else {
        if (busy) drop_open_ = false;
        quiet_ = p >= 0.5 || out.motion_burst ? 0 : quiet_ + 1;
    }
    if (hold) return true;
    if (p >= 0.5) pending = {true, index, p, complexity};
    // The mean of its kind, cuts left out (they are not this shot's motion): down fast (a burst's level does not linger), up
    // slowly.
    else if (inter_[kind] <= 0) inter_[kind] = complexity;
    else inter_[kind] += (complexity - inter_[kind]) * (complexity < inter_[kind] ? 0.5 : 0.15);
    return true;
}
}
