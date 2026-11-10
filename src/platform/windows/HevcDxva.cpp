#include "media/platform/windows/HevcDxva.hpp"
#include <cstring>
namespace media::codec {
platform::windows::DecodePicture HevcDxva::Prepare(const HevcPicture& p, core::MediaPacket packet, platform::BlockView block) noexcept {
    parameters = {};
    const auto& s = *p.sps; const auto& pps = *p.pps;
    auto& d = parameters;
    const unsigned min_cb = s.log2_min_cb_minus3 + 3u;
    d.PicWidthInMinCbsY = static_cast<USHORT>(s.width >> min_cb);
    d.PicHeightInMinCbsY = static_cast<USHORT>(s.height >> min_cb);
    d.chroma_format_idc = 1;
    d.bit_depth_luma_minus8 = s.bit_depth_luma_minus8; d.bit_depth_chroma_minus8 = s.bit_depth_chroma_minus8;
    d.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(s.poc_lsb_bits - 4u);
    d.NoPicReorderingFlag = s.max_num_reorder ? 0 : 1;
    d.sps_max_dec_pic_buffering_minus1 = s.max_dec_pic_buffering_minus1;
    d.log2_min_luma_coding_block_size_minus3 = s.log2_min_cb_minus3;
    d.log2_diff_max_min_luma_coding_block_size = s.log2_diff_max_min_cb;
    d.log2_min_transform_block_size_minus2 = s.log2_min_tb_minus2;
    d.log2_diff_max_min_transform_block_size = s.log2_diff_max_min_tb;
    d.max_transform_hierarchy_depth_inter = s.max_transform_depth_inter;
    d.max_transform_hierarchy_depth_intra = s.max_transform_depth_intra;
    d.num_short_term_ref_pic_sets = s.num_short_term_rps;
    d.num_long_term_ref_pics_sps = s.num_long_term_sps;
    d.num_ref_idx_l0_default_active_minus1 = pps.refs_l0_minus1;
    d.num_ref_idx_l1_default_active_minus1 = pps.refs_l1_minus1;
    d.init_qp_minus26 = pps.init_qp_minus26;
    // Values of the RPS the slice actually selected (SPS or slice header), as consumed by the accelerator.
    d.ucNumDeltaPocsOfRefRpsIdx = p.reference_rps_delta_pocs;
    d.wNumBitsForShortTermRPSInSlice = p.short_term_rps_bits;
    d.scaling_list_enabled_flag = s.scaling_list_enabled; d.amp_enabled_flag = s.amp;
    d.sample_adaptive_offset_enabled_flag = s.sao; d.pcm_enabled_flag = s.pcm;
    if (s.pcm) {
        d.pcm_sample_bit_depth_luma_minus1 = s.pcm_bit_depth_luma_minus1;
        d.pcm_sample_bit_depth_chroma_minus1 = s.pcm_bit_depth_chroma_minus1;
        d.log2_min_pcm_luma_coding_block_size_minus3 = s.log2_min_pcm_minus3;
        d.log2_diff_max_min_pcm_luma_coding_block_size = s.log2_diff_max_min_pcm;
        d.pcm_loop_filter_disabled_flag = s.pcm_loop_filter_disabled;
    }
    d.long_term_ref_pics_present_flag = s.long_term_present; d.sps_temporal_mvp_enabled_flag = s.temporal_mvp;
    d.strong_intra_smoothing_enabled_flag = s.strong_intra_smoothing;
    d.dependent_slice_segments_enabled_flag = pps.dependent_slices; d.output_flag_present_flag = pps.output_flag_present;
    d.num_extra_slice_header_bits = pps.extra_slice_header_bits;
    d.sign_data_hiding_enabled_flag = pps.sign_data_hiding; d.cabac_init_present_flag = pps.cabac_init_present;
    d.constrained_intra_pred_flag = pps.constrained_intra; d.transform_skip_enabled_flag = pps.transform_skip;
    d.cu_qp_delta_enabled_flag = pps.cu_qp_delta; d.pps_slice_chroma_qp_offsets_present_flag = pps.slice_chroma_qp_offsets;
    d.weighted_pred_flag = pps.weighted_pred; d.weighted_bipred_flag = pps.weighted_bipred;
    d.transquant_bypass_enabled_flag = pps.transquant_bypass; d.tiles_enabled_flag = pps.tiles;
    d.entropy_coding_sync_enabled_flag = pps.entropy_sync; d.uniform_spacing_flag = pps.uniform_spacing;
    d.loop_filter_across_tiles_enabled_flag = pps.loop_filter_across_tiles;
    d.pps_loop_filter_across_slices_enabled_flag = pps.loop_filter_across_slices;
    d.deblocking_filter_override_enabled_flag = pps.deblocking_override;
    d.pps_deblocking_filter_disabled_flag = pps.deblocking_disabled;
    d.lists_modification_present_flag = pps.lists_modification;
    d.slice_segment_header_extension_present_flag = pps.slice_header_extension;
    d.IrapPicFlag = p.irap; d.IdrPicFlag = p.idr; d.IntraPicFlag = p.intra;
    d.pps_cb_qp_offset = pps.cb_qp_offset; d.pps_cr_qp_offset = pps.cr_qp_offset;
    if (pps.tiles) {
        d.num_tile_columns_minus1 = pps.tile_columns_minus1; d.num_tile_rows_minus1 = pps.tile_rows_minus1;
        if (!pps.uniform_spacing) {
            for (unsigned i = 0; i < pps.tile_columns_minus1; ++i) d.column_width_minus1[i] = pps.column_width_minus1[i];
            for (unsigned i = 0; i < pps.tile_rows_minus1; ++i) d.row_height_minus1[i] = pps.row_height_minus1[i];
        }
    }
    d.diff_cu_qp_delta_depth = pps.diff_cu_qp_delta_depth;
    d.pps_beta_offset_div2 = pps.beta_offset_div2; d.pps_tc_offset_div2 = pps.tc_offset_div2;
    d.log2_parallel_merge_level_minus2 = pps.log2_parallel_merge_minus2;
    d.CurrPicOrderCntVal = p.poc;
    for (auto& ref : d.RefPicList) ref.bPicEntry = 0xFF;
    std::memset(d.RefPicSetStCurrBefore, 0xFF, sizeof(d.RefPicSetStCurrBefore));
    std::memset(d.RefPicSetStCurrAfter, 0xFF, sizeof(d.RefPicSetStCurrAfter));
    std::memset(d.RefPicSetLtCurr, 0xFF, sizeof(d.RefPicSetLtCurr));
    for (unsigned i = 0; i < p.reference_count; ++i) {
        references[i] = p.references[i].surface;
        d.RefPicList[i].bPicEntry = static_cast<UCHAR>(i | (p.references[i].long_term ? 0x80u : 0u));
        d.PicOrderCntValList[i] = p.references[i].poc;
    }
    for (unsigned i = 0; i < p.st_curr_before_count; ++i) d.RefPicSetStCurrBefore[i] = p.st_curr_before[i];
    for (unsigned i = 0; i < p.st_curr_after_count; ++i) d.RefPicSetStCurrAfter[i] = p.st_curr_after[i];
    for (unsigned i = 0; i < p.lt_curr_count; ++i) d.RefPicSetLtCurr[i] = p.lt_curr[i];
    // Scaling factors stay in coded (diagonal) order. The resolved PPS lists override the SPS lists.
    const auto& lists = pps.scaling_list_present ? pps.scaling : s.scaling;
    for (unsigned m = 0; m < 6; ++m) {
        std::memcpy(matrix.ucScalingLists0[m], lists.lists[0][m], 16);
        std::memcpy(matrix.ucScalingLists1[m], lists.lists[1][m], 64);
        std::memcpy(matrix.ucScalingLists2[m], lists.lists[2][m], 64);
        matrix.ucScalingListDCCoefSizeID2[m] = lists.dc[0][m];
    }
    for (unsigned m = 0; m < 2; ++m) {
        std::memcpy(matrix.ucScalingLists3[m], lists.lists[3][m * 3], 64);
        matrix.ucScalingListDCCoefSizeID3[m] = lists.dc[1][m * 3];
    }
    for (unsigned i = 0; i < p.slice_count; ++i) slices[i] = {p.slices[i].offset, p.slices[i].bytes};
    platform::windows::DecodePicture out;
    out.packet = packet; out.payload = block;
    out.slices = {slices, p.slice_count}; out.references = {references, p.reference_count};
    out.hevc = &parameters; out.hevc_matrix = &matrix;
    out.crop_left = s.left; out.crop_top = s.top; out.crop_right = s.right; out.crop_bottom = s.bottom;
    out.retain_as_reference = p.retain;
    out.color_metadata_id = static_cast<std::uint32_t>(s.matrix) | (s.full_range ? 256u : 0u) |
                            (static_cast<std::uint32_t>(s.transfer) << 16) | (static_cast<std::uint32_t>(s.colour_primaries) << 24);
    return out;
}
}
