# H.264 B-pictures, reference marking and reordering

`H264SyntaxParser` admits the H.264 streams that common encoders (x264/HandBrake, broadcast, Blu-ray-style, and the Windows encoders) produce with B-pictures:

- I, P and B slices, which may be mixed within a picture;
- non-reference B-pictures and reference B-pictures (hierarchical pyramids);
- reference picture list modification;
- explicit and implicit weighted prediction;
- POC types 0, 1 and 2;
- the full decoded reference picture marking process.

It follows [ITU-T H.264](https://www.itu.int/ITU-T/recommendations/rec.aspx?id=15935) clauses 7.3.3, 7.4.3 and 8.2.

## Division of work with the accelerator

D3D12 video decoding takes H.264 slices in DXVA's short format, so the accelerator parses every slice header and builds, modifies and weights the reference lists itself. The host's job is the state those lists are built from:

| Host responsibility | Where |
| --- | --- |
| The DPB: which frames are references, short- or long-term, with their FrameNum or LongTermFrameIdx | `RefFrameList` (bit 7 = long-term), `FrameNumList`, `UsedForReferenceFlags` |
| Each reference's POCs, and the current picture's | `FieldOrderCntList`, `CurrFieldOrderCnt` |
| The current picture's syntax context | `frame_num`, `RefPicFlag`, `IntraPicFlag`, POC-type fields, PPS fields |

The parser still reads every slice header completely, so that it can check the syntax and reach `dec_ref_pic_marking`. The checks cover:

- per-list active-count overrides of at most 32 entries;
- modification operations: at most one per list entry, operation codes 0–3, and `abs_diff_pic_num` below MaxPicNum;
- weight tables for both lists, each value a signed 8-bit number;
- MMCO operands.

Picture-level syntax must repeat in every slice. That covers frame_num, POC fields, the PPS, the IDR id and the marking. Slice types may differ: a picture is intra only when every slice is I, and bidirectional when any slice is B.

## Frame numbers and POC

- **frame_num.** Without gaps, frame_num equals PrevRefFrameNum or PrevRefFrameNum + 1. Consecutive non-reference B-pictures share the frame_num that the next reference picture then also uses. A gap is `Unsupported`, because `gaps_in_frame_num_allowed_flag` is not admitted.
- **Type 0.** The MSB comes from the previous *reference* picture's LSB and MSB.
- **Types 1 and 2.** FrameNumOffset wraps with the previous picture's frame_num, whether or not that picture is a reference.
  - Type 1 uses the SPS cycle: `offset_for_ref_frame[]`, `offset_for_non_ref_pic` and `offset_for_top_to_bottom_field`, the latter plus `delta_pic_order_cnt[0..1]` when `delta_pic_order_always_zero_flag` is 0.
  - Type 2 uses `2·(FrameNumOffset + frame_num)`, minus 1 for a non-reference picture.
- **After MMCO 5.** The picture counts as frame_num 0 with POCs relative to the smaller of its two. The next picture's `prevPicOrderCnt`, `prevFrameNum` and `prevFrameNumOffset` restart from 0.

## Decoded reference picture marking (8.2.5)

`Commit` runs after the picture's native submission, so the picture's own lists use the DPB from before its marking.

- **IDR.** Every reference leaves the DPB, oldest first. `long_term_reference_flag` makes the IDR long-term index 0 and sets MaxLongTermFrameIdx to 0; otherwise there are no long-term indices.
- **Sliding window.** When the DPB holds `max_num_ref_frames` frames, the short-term frame with the smallest FrameNumWrap leaves. Long-term frames never slide.
- **MMCO, in bitstream order.** PicNum is FrameNumWrap and CurrPicNum is frame_num.

  | Operation | Effect |
  | --- | --- |
  | 1 | Unmark short-term `CurrPicNum − (difference + 1)` |
  | 2 | Unmark long-term by LongTermPicNum |
  | 3 | Turn a short-term frame into long-term index *i*, unmarking any other holder of *i* |
  | 4 | Set MaxLongTermFrameIdx, unmarking every index above it |
  | 5 | Unmark everything |
  | 6 | Mark the current picture long-term index *i* |

  Operations that name an absent picture, or an index above MaxLongTermFrameIdx, are ignored, as tolerant decoders do.
- **Repair.** A non-conforming stream whose adaptive marking overfills the DPB is repaired with the sliding window rather than rejected.

Evicted frames return to `D3D12VideoDecoder::ReleaseReference`. The DPB, like every parser table, lives in the arena, and nothing is allocated after sealing.

## Presentation order

Pictures leave the decoder in decode order and are presented in PTS order through the renderer's `ReorderWindow`, the same one HEVC uses. Its depth is `max_num_reorder_frames`:

- from the VUI's `bitstream_restriction` when present;
- otherwise inferred as in Annex E.2.1: 0 for Baseline and for intra profiles with `constraint_set3_flag`, else MaxDpbFrames for the level and frame size.

The depth is capped at 14, the renderer's 16 slots minus 2. Decode surfaces are sized as `max_num_ref_frames + 1 + 8`, clamped to the range 24–32.

Identical SPS/PPS repeated in band are accepted; Intel's encoder repeats the PPS in every access unit. A different set still requires a new decoder session.

## Not admitted

- fields and MBAFF (`frame_mbs_only_flag` 0);
- SP/SI slices;
- FMO and ASO;
- redundant pictures;
- frame_num gaps;
- custom scaling matrices;
- 4:2:2, 4:4:4 and high bit depths;
- SVC and MVC.

## Verification

| Test | What it proves |
| --- | --- |
| `media_codec_tests` (portable, GCC ASan/UBSan) | IBBP and pyramid syntax, including frame_num repeats, weights for both lists, list modification (and its bounds) and mixed-slice pictures. Sliding window across frame_num wrap (FrameNumWrap). MMCO 1–6 against the expected DPB and eviction order, including the repair window and MMCO 5's frame_num and POC reset. IDR long-term references. POC types 0 (LSB wrap around B-pictures), 1 (cycle, non-reference offset, deltas, FrameNumOffset wrap, reset by MMCO 5) and 2. Reorder inference, and the in-band parameter-set policy. Byte mutations of B slices with modification, weights and MMCO, committed and checked for bounds. |
| `media_h264_lists_tests` (GPU) | Handwritten streams of flat I_PCM references and fully skipped P/B pictures. A P-skip copies `L0[0]`; a spatial-direct B-skip is exactly `(L0[0] + L1[0] + 1) / 2`, so each output luma is known. There are 32 pictures across three streams: default P and B lists, list modification by PicNum and LongTermPicNum, MMCO 1–6, an IDR long-term reference, POC type 1 (a non-reference B-picture placed by `offset_for_non_ref_pic`) and POC type 2 (the L1 swap rule). All must be pixel-exact. Mutation check: dropping the long-term bit fails exactly the two long-term pictures; ignoring MMCO fails the stream. |
| `media_h264_decode_tests` (GPU) | Real streams from the platform encoders at 320×192: Intel hardware B-pyramid (CABAC, reference B-pictures, MMCO 1, P-slice list modification, 6-bit POC LSB wrap), Intel IBBP, and the Microsoft software encoder (High CAVLC and Main). Every picture is compared with its source frame; the POC-derived presentation order is cross-checked against the encoder's timestamps and replayed through `ReorderWindow` at the SPS depth. |
| `media_playback_h264_bframes_*` | 1280×720 MP4s with `ctts` and an edit list: the hardware pyramid (51 of 72 pictures are B, depth 2) and the software IBBP (45 B, depth 1). Each covers playback (72/72 frames, 0 out-of-order, 0 drops, 0 allocations) and exact seeks while playing and paused. The pyramid also covers the scrub test and HTTP seeking. |
