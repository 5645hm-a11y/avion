#pragma once
#include "media/codec/Rbsp.hpp"
#include "media/platform/IGpuContext.hpp"
#include <span>

namespace media::codec {
struct H264Sps {
    bool valid = false;
    std::uint8_t profile = 0, constraints = 0, level = 0, frame_bits = 0, poc_type = 0, poc_bits = 0, max_refs = 0;
    bool direct8 = false, full_range = false;
    // VUI colour description (H.273 code points, 2 = unspecified). `signal_described`: video_signal_type was present
    // (it carries the range flag); `colour_described`: so were the three code points.
    std::uint8_t primaries = 2, transfer = 2, matrix = 2;
    bool signal_described = false, colour_described = false;
    // VUI sample aspect ratio (Table E-1 or the explicit pair); 0:0 when the stream does not say.
    std::uint16_t sar_width = 0, sar_height = 0;
    // max_num_reorder_frames: the VUI's value, else the Annex E inference (0 for Baseline and intra profiles with
    // constraint_set3, else MaxDpbFrames for the level and picture size). `reorder_signalled`: taken from the VUI.
    std::uint8_t reorder = 0;
    bool reorder_signalled = false;
    std::uint16_t width = 0, height = 0, left = 0, top = 0, right = 0, bottom = 0;
    // pic_order_cnt_type 1 (8.2.1.2).
    bool always_zero = false;
    std::uint8_t cycle = 0;
    std::int32_t offset_non_ref = 0, offset_top_bottom = 0;
    std::int64_t cycle_delta = 0; // ExpectedDeltaPerPicOrderCntCycle.
    std::int32_t cycle_offsets[255]{};
    bool operator==(const H264Sps&) const = default;
};
struct H264Pps {
    bool valid = false, entropy = false, bottom_poc = false, weighted = false;
    bool deblock = false, constrained = false, transform8 = false;
    std::uint8_t sps = 0, refs_l0 = 0, refs_l1 = 0, weighted_bi = 0;
    std::int8_t qp = 0, qs = 0, chroma_qp = 0, second_chroma_qp = 0;
    bool operator==(const H264Pps&) const = default;
};
// A frame in the decoded picture buffer. `frame_num` is FrameNum for a short-term reference and LongTermFrameIdx
// for a long-term one, which is what DXVA's FrameNumList carries.
struct H264Reference {
    platform::SurfaceHandle surface{};
    std::uint16_t frame_num = 0;
    bool long_term = false;
    std::int32_t top = 0, bottom = 0;
};
struct H264Slice { std::uint32_t offset = 0, bytes = 0; };
// memory_management_control_operation 1-6 with its operands: difference_of_pic_nums_minus1 (1, 3),
// long_term_pic_num (2), long_term_frame_idx (3, 6) or max_long_term_frame_idx_plus1 (4).
struct H264Mmco { std::uint8_t operation = 0; std::uint32_t value = 0, index = 0; };
struct H264Picture {
    const H264Sps* sps = nullptr; const H264Pps* pps = nullptr;
    H264Reference references[16]{}; // The DPB before this picture's marking: what the accelerator builds lists from.
    H264Slice slices[256]{};
    H264Mmco mmco[32]{};             // Adaptive marking of the first slice (every slice repeats it).
    std::uint32_t slice_count = 0, reference_count = 0, frame_num = 0, poc_lsb = 0, mmco_count = 0;
    std::uint32_t pps_id = 0, idr_picture_id = 0;
    std::int32_t delta_poc = 0;           // delta_pic_order_cnt_bottom (type 0).
    std::int32_t delta_poc0 = 0, delta_poc1 = 0; // delta_pic_order_cnt[0..1] (type 1).
    std::int32_t top_poc = 0, bottom_poc = 0, poc_msb = 0;
    std::int64_t frame_offset = 0;
    bool idr = false, reference = false, intra = false, bidirectional = false;
    bool adaptive = false, long_term = false; // Adaptive marking; IDR long_term_reference_flag.
};
// One codec owner. Parse is transactional; commit only after successful native submission.
// Progressive 4:2:0 8-bit Baseline/Main/Extended-without-SP/High: I, P and B slices, reference list modification,
// weighted prediction, POC types 0/1/2, and the full decoded reference picture marking process (sliding window,
// MMCO 1-6, long-term references). Reference lists are built by the accelerator from the DPB this parser keeps.
class H264SyntaxParser final {
public:
    bool Initialize(core::LinearMemoryArena&) noexcept;
    SyntaxStatus ParameterSet(std::span<const std::byte> nal) noexcept;
    SyntaxStatus Parse(std::span<const std::byte> access_unit, unsigned length_bytes) noexcept;
    const H264Picture* Picture() const noexcept { return pending_ ? picture_ : nullptr; }
    const H264Sps* FirstSps() const noexcept;
    // Returns evicted DPB holds for native ReleaseReference (not consumer leases).
    bool Commit(platform::SurfaceHandle surface, std::span<platform::SurfaceHandle> evicted, unsigned& count) noexcept;
    void Cancel() noexcept { pending_ = false; }
    // Seek/flush: returns every DPB hold for native ReleaseReference and requires a new IDR picture.
    bool Reset(std::span<platform::SurfaceHandle> released, unsigned& count) noexcept;
    // Current DPB (after the last commit), for diagnostics and tests.
    std::span<const H264Reference> References() const noexcept { return {dpb_, refs_}; }
private:
    SyntaxStatus Sps(RbspReader&) noexcept;
    SyntaxStatus Pps(RbspReader&) noexcept;
    SyntaxStatus Slice(std::span<const std::byte>, bool first, std::uint32_t& first_mb) noexcept;
    H264Sps* sps_ = nullptr; H264Pps* pps_ = nullptr; H264Picture* picture_ = nullptr;
    H264Reference* dpb_ = nullptr;
    unsigned refs_ = 0;
    std::int32_t max_long_term_ = -1; // MaxLongTermFrameIdx; -1: no long-term frame indices.
    std::uint32_t previous_frame_ = 0, previous_reference_frame_ = 0, previous_lsb_ = 0;
    unsigned active_sps_ = 0;
    std::int64_t frame_offset_ = 0;
    std::int32_t previous_msb_ = 0;
    bool started_ = false, pending_ = false;
};
}
