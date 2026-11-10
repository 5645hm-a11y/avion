#include "media/platform/windows/H264Dxva.hpp"
#include <cstring>
namespace media::codec {
platform::windows::DecodePicture H264Dxva::Prepare(const H264Picture& p, core::MediaPacket packet, platform::BlockView block) noexcept {
    parameters = {}; std::memset(&matrix, 16, sizeof(matrix));
    auto& d = parameters; const auto& sps = *p.sps; const auto& pps = *p.pps;
    d.wFrameWidthInMbsMinus1 = static_cast<USHORT>(sps.width / 16 - 1);
    d.wFrameHeightInMbsMinus1 = static_cast<USHORT>(sps.height / 16 - 1);
    d.num_ref_frames = sps.max_refs; d.RefPicFlag = p.reference ? 1 : 0;
    d.MbsConsecutiveFlag = 1; d.frame_mbs_only_flag = 1; d.chroma_format_idc = 1;
    d.MinLumaBipredSize8x8Flag = sps.level >= 31 ? 1 : 0;
    d.IntraPicFlag = p.intra ? 1 : 0; d.Reserved16Bits = 3;
    d.CurrFieldOrderCnt[0] = p.top_poc; d.CurrFieldOrderCnt[1] = p.bottom_poc;
    d.frame_num = static_cast<USHORT>(p.frame_num);
    d.log2_max_frame_num_minus4 = static_cast<UCHAR>(sps.frame_bits - 4);
    d.pic_order_cnt_type = sps.poc_type;
    d.log2_max_pic_order_cnt_lsb_minus4 = sps.poc_type == 0 ? static_cast<UCHAR>(sps.poc_bits - 4) : 0;
    d.delta_pic_order_always_zero_flag = sps.always_zero ? 1 : 0;
    d.direct_8x8_inference_flag = sps.direct8 ? 1 : 0;
    d.entropy_coding_mode_flag = pps.entropy ? 1 : 0;
    d.pic_order_present_flag = pps.bottom_poc ? 1 : 0;
    d.weighted_pred_flag = pps.weighted ? 1 : 0;
    d.weighted_bipred_idc = pps.weighted_bi;
    d.transform_8x8_mode_flag = pps.transform8 ? 1 : 0;
    d.constrained_intra_pred_flag = pps.constrained ? 1 : 0;
    d.deblocking_filter_control_present_flag = pps.deblock ? 1 : 0;
    d.pic_init_qp_minus26 = pps.qp; d.pic_init_qs_minus26 = pps.qs;
    d.chroma_qp_index_offset = pps.chroma_qp; d.second_chroma_qp_index_offset = pps.second_chroma_qp;
    d.num_ref_idx_l0_active_minus1 = pps.refs_l0; d.num_ref_idx_l1_active_minus1 = pps.refs_l1;
    for (auto& ref : d.RefFrameList) ref.bPicEntry = 255;
    for (unsigned i = 0; i < p.reference_count; ++i) {
        // The DPB index; the decoder maps it to its surface slot and keeps bit 7 (long-term reference).
        references[i] = p.references[i].surface;
        d.RefFrameList[i].bPicEntry = static_cast<UCHAR>(i | (p.references[i].long_term ? 128u : 0u));
        d.FrameNumList[i] = p.references[i].frame_num; // LongTermFrameIdx for a long-term reference.
        d.FieldOrderCntList[i][0] = p.references[i].top; d.FieldOrderCntList[i][1] = p.references[i].bottom;
        d.UsedForReferenceFlags |= 3u << (2 * i);
    }
    for (unsigned i = 0; i < p.slice_count; ++i) slices[i] = {p.slices[i].offset, p.slices[i].bytes};
    platform::windows::DecodePicture out;
    out.packet = packet; out.payload = block; out.slices = {slices, p.slice_count}; out.references = {references, p.reference_count};
    out.h264 = &parameters; out.h264_matrix = &matrix;
    out.crop_right = sps.right; out.crop_bottom = sps.bottom;
    out.crop_left = sps.left; out.crop_top = sps.top;
    out.retain_as_reference = p.reference && sps.max_refs != 0;
    out.color_metadata_id = static_cast<std::uint32_t>(sps.matrix) | (sps.full_range ? 256u : 0u) |
                            (static_cast<std::uint32_t>(sps.transfer) << 16) | (static_cast<std::uint32_t>(sps.primaries) << 24);
    return out;
}
}
