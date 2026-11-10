#pragma once
#include "media/codec/Rbsp.hpp"
#include "media/platform/IGpuContext.hpp"
#include <span>

namespace media::codec {
// Scaling factors in coded (up-right diagonal) order, the order DXVA_Qmatrix_HEVC uses.
// [sizeId][matrixId][i]: 4x4 uses 16 coefficients; 32x32 uses matrixId 0 and 3 only.
struct HevcScalingList {
    std::uint8_t lists[4][6][64]{};
    std::uint8_t dc[2][6]{}; // sizeId 2 and 3.
    bool operator==(const HevcScalingList&) const noexcept = default;
};
// Derived short_term_ref_pic_set (ITU-T H.265 7.4.8). Bit i of used_* is UsedByCurrPicS*[i].
struct HevcShortTermRps {
    std::int32_t delta_s0[16]{}, delta_s1[16]{};
    std::uint16_t used_s0 = 0, used_s1 = 0;
    std::uint8_t negative = 0, positive = 0;
    std::uint8_t reference_delta_pocs = 0; // NumDeltaPocs[RefRpsIdx] when inter-predicted, else 0.
    bool operator==(const HevcShortTermRps&) const noexcept = default;
};
struct HevcVps { bool valid = false; std::uint8_t max_sub_layers_minus1 = 0; };
struct HevcSps {
    bool valid = false;
    std::uint8_t vps = 0, max_sub_layers_minus1 = 0, profile = 0, level = 0;
    std::uint8_t bit_depth_luma_minus8 = 0, bit_depth_chroma_minus8 = 0, poc_lsb_bits = 0;
    std::uint8_t max_dec_pic_buffering_minus1 = 0, max_num_reorder = 0; // Highest sub-layer.
    std::uint8_t log2_min_cb_minus3 = 0, log2_diff_max_min_cb = 0, log2_min_tb_minus2 = 0, log2_diff_max_min_tb = 0;
    std::uint8_t max_transform_depth_inter = 0, max_transform_depth_intra = 0;
    std::uint8_t pcm_bit_depth_luma_minus1 = 0, pcm_bit_depth_chroma_minus1 = 0;
    std::uint8_t log2_min_pcm_minus3 = 0, log2_diff_max_min_pcm = 0;
    std::uint8_t num_short_term_rps = 0, num_long_term_sps = 0;
    std::uint8_t colour_primaries = 2, transfer = 2, matrix = 2; // 2 = unspecified.
    bool signal_described = false, colour_described = false; // video_signal_type present; its three code points present.
    std::uint16_t sar_width = 0, sar_height = 0; // VUI sample aspect ratio (Table E.1 or the explicit pair); 0:0 = not said.
    bool tier = false, full_range = false, scaling_list_enabled = false, amp = false, sao = false;
    bool pcm = false, pcm_loop_filter_disabled = false, long_term_present = false;
    bool temporal_mvp = false, strong_intra_smoothing = false;
    std::uint16_t width = 0, height = 0; // pic_width/height_in_luma_samples.
    std::uint16_t left = 0, top = 0, right = 0, bottom = 0; // Conformance window, exclusive right/bottom.
    std::uint16_t lt_poc_lsb[32]{};
    std::uint32_t lt_used = 0;
    // RBSP bit offsets (after the 2-byte NAL header) of vui_parameters_present_flag and of the first bit after
    // the VUI; they let bitstream tooling rewrite the colour description without re-encoding.
    std::uint32_t vui_flag_bit = 0, vui_end_bit = 0;
    HevcScalingList scaling{}; // Explicit, default (Table 7-5/7-6) or flat, resolved at parse time.
    HevcShortTermRps rps[64]{};
    bool operator==(const HevcSps&) const noexcept = default;
};
struct HevcPps {
    bool valid = false;
    std::uint8_t sps = 0, extra_slice_header_bits = 0, refs_l0_minus1 = 0, refs_l1_minus1 = 0;
    std::uint8_t diff_cu_qp_delta_depth = 0, log2_parallel_merge_minus2 = 0;
    std::uint8_t tile_columns_minus1 = 0, tile_rows_minus1 = 0;
    std::int8_t init_qp_minus26 = 0, cb_qp_offset = 0, cr_qp_offset = 0, beta_offset_div2 = 0, tc_offset_div2 = 0;
    bool dependent_slices = false, output_flag_present = false, sign_data_hiding = false, cabac_init_present = false;
    bool constrained_intra = false, transform_skip = false, cu_qp_delta = false, slice_chroma_qp_offsets = false;
    bool weighted_pred = false, weighted_bipred = false, transquant_bypass = false, tiles = false;
    bool entropy_sync = false, uniform_spacing = true, loop_filter_across_tiles = true;
    bool loop_filter_across_slices = false, deblocking_override = false, deblocking_disabled = false;
    bool scaling_list_present = false, lists_modification = false, slice_header_extension = false;
    std::uint16_t column_width_minus1[19]{}, row_height_minus1[21]{}; // Signalled entries only.
    HevcScalingList scaling{};
};
struct HevcReference { platform::SurfaceHandle surface{}; std::int32_t poc = 0; bool long_term = false; };
struct HevcSlice { std::uint32_t offset = 0, bytes = 0; }; // Slice-segment NAL, excluding its length prefix.
struct HevcPicture {
    const HevcSps* sps = nullptr;
    const HevcPps* pps = nullptr;
    // DXVA RefPicList: every picture retained by this picture's RPS (current and following).
    HevcReference references[15]{};
    std::uint8_t st_curr_before[8]{}, st_curr_after[8]{}, lt_curr[8]{}; // Indices into references.
    std::uint8_t st_curr_before_count = 0, st_curr_after_count = 0, lt_curr_count = 0;
    HevcSlice slices[256]{};
    std::uint32_t slice_count = 0, reference_count = 0, pps_id = 0;
    std::int32_t poc = 0;
    std::uint16_t short_term_rps_bits = 0; // RBSP bits of st_ref_pic_set() in the slice header, 0 if from the SPS.
    std::uint8_t reference_rps_delta_pocs = 0, nal_type = 0, temporal_id = 0;
    bool irap = false, idr = false, intra = true, output = true, retain = true;
    bool no_rasl_output = false, end_of_sequence = false;
};
// One codec owner. Parse is transactional; Commit only after successful native submission.
// Main/Main10 4:2:0 progressive base layer; POC (8.3.1) and RPS/DPB marking (8.3.2).
// Slice headers are parsed through slice_temporal_mvp_enabled_flag; the DXVA short-slice
// contract leaves the remaining slice syntax to the hardware decoder.
class HevcSyntaxParser final {
public:
    bool Initialize(core::LinearMemoryArena&) noexcept;
    // VPS/SPS/PPS; SEI and nuh_layer_id > 0 units are ignored (Ready).
    SyntaxStatus ParameterSet(std::span<const std::byte> nal) noexcept;
    // One length-prefixed access unit containing exactly one base-layer picture.
    // Discard: RASL picture following a random-access start; return its packet undecoded.
    SyntaxStatus Parse(std::span<const std::byte> access_unit, unsigned length_bytes) noexcept;
    const HevcPicture* Picture() const noexcept { return pending_ ? picture_ : nullptr; }
    const HevcSps* FirstSps() const noexcept;
    // Returns evicted DPB holds for native ReleaseReference (not consumer leases).
    bool Commit(platform::SurfaceHandle surface, std::span<platform::SurfaceHandle> evicted, unsigned& count) noexcept;
    void Cancel() noexcept { pending_ = false; }
    // Seek/flush: returns every DPB hold for native ReleaseReference and requires a new IRAP picture, whose
    // NoRaslOutputFlag then discards its RASL pictures. Parameter sets stay valid; nothing is allocated.
    bool Reset(std::span<platform::SurfaceHandle> released, unsigned& count) noexcept;
private:
    struct SliceState {
        HevcShortTermRps rps{};
        std::uint32_t lt_cycle[16]{};
        std::uint16_t lt_lsb[16]{};
        std::uint32_t pps = 0, poc_lsb = 0;
        std::uint16_t rps_bits = 0, lt_used = 0, lt_msb = 0;
        std::uint8_t lt_count = 0;
        bool temporal_mvp = false, output = true;
        bool operator==(const SliceState&) const noexcept = default;
    };
    SyntaxStatus Nal(std::span<const std::byte> nal) noexcept;
    SyntaxStatus Vps(RbspReader&) noexcept;
    SyntaxStatus Sps(RbspReader&) noexcept;
    SyntaxStatus Pps(RbspReader&) noexcept;
    SyntaxStatus Slice(std::span<const std::byte> nal, unsigned type, unsigned temporal_id, bool first, std::uint32_t& address) noexcept;
    SyntaxStatus Activate(unsigned pps_id) noexcept;
    SyntaxStatus References(const SliceState&) noexcept;
    HevcVps* vps_ = nullptr;
    HevcSps *sps_ = nullptr, *sps_scratch_ = nullptr;
    HevcPps *pps_ = nullptr, *pps_scratch_ = nullptr;
    HevcPicture* picture_ = nullptr;
    HevcReference* dpb_ = nullptr;
    SliceState *first_ = nullptr, *slice_ = nullptr;
    std::uint8_t keep_[16]{}; // Pending marking per DPB entry: 0 evict, 1 short-term, 2 long-term.
    unsigned refs_ = 0, active_sps_ = 0;
    std::int32_t previous_poc_ = 0; // prevTid0Pic (8.3.1).
    bool started_ = false, pending_ = false, discard_rasl_ = false, after_eos_ = false;
};
}
