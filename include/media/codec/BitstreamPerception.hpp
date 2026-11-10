#pragma once
#include <cstdint>

namespace media::codec {
// What one compressed picture says about itself before it is decoded (BitstreamPerception::Inspect).
struct BitstreamFrame {
    std::uint32_t index = 0;        // Units inspected before this one.
    std::uint32_t bytes = 0;        // Its slices' NAL units (headers included).
    std::uint32_t slices = 0;       // Slice (segment) NAL units.
    char type = '?';                // 'I', 'P' or 'B': B when any slice is, else P when any is, else I ('?': no slice).
    bool key = false;               // An IDR (H.264) or IRAP (HEVC) picture.
    bool parameter_sets = false;    // The unit carries sequence or picture parameter sets.
    int qp = -1;                    // Its slices' mean QP (rounded); -1 unknown (a header this reader could not follow).
    // Its place in the encoder's prediction structure (2026-11-03): HEVC's temporal sub-layer (nuh_temporal_id; H.264: 0 for a
    // reference picture, 1 for a non-reference one), and the POC distance to its nearest reference in use (HEVC, its slice's
    // short-term set; 0 unknown or intra).
    std::uint8_t layer = 0;
    std::uint16_t reference_distance = 0;
    float complexity = 0;           // Bits per pixel, at QP 26 when the QP is known (x 2^((QP - 26) / 6): what the content cost).
    float activity = -1;            // Inter pictures: complexity over the recent intra pictures' (0 still .. 1 all new); -1 intra.
    float burst = 0;                // Complexity over the recent mean of its kind (P or B) before it (1: as usual).
    float scene_cut = 0;            // 0..1: the probability that it starts a new shot.
    bool motion_burst = false;      // Much more new detail than the recent pictures (burst >= 2), not a cut.
    bool step_down = false;         // It confirmed a cut into a much simpler shot: the picture before it (a revision), or itself.
    // An inter picture's cut is confirmed or withdrawn by the next picture of its kind (P, B), which says so here: the earlier
    // picture's index and its probability now (0.95 or more: confirmed; under 0.3: a burst of new detail that went on).
    bool revised = false;
    std::uint32_t revised_index = 0;
    float revised_scene_cut = 0;
};

// Pre-decode perception from the compressed bitstream (docs/picture.md, "Bitstream perception", 2026-11-02), portable, no
// allocation (its tables are the object: ~9 KB): H.264 and HEVC access units as stored in MP4 (length-prefixed NAL units),
// read without decoding a pixel, in a few microseconds each.
// - The headers: the parameter sets from the sample entry's avcC / hvcC (Configure) and in-band; each slice's type and QP,
//   its header read to slice_qp_delta. H.264: frame_num, POC, reference list modification, weighted prediction tables,
//   reference marking (FMO maps in the PPS). HEVC: the short-term reference picture sets (the SPS's, with inter-set
//   prediction's derivation of each set's deltas, and the slice's own), long-term pictures, SAO, the reference counts, list
//   modification (NumPicTotalCurr from the set's pictures in use), the temporal MVP's collocated picture, the weight tables;
//   the PPS's tiles and scaling lists read past. Every parameter set is kept by its id (H.264: 32 SPS, 256 PPS; HEVC: 16
//   SPS, 64 PPS), and since 2026-11-03 each HEVC SPS's reference picture sets too, in one pool (kSetEntries sets, kDeltas
//   delta POCs: a stream that switches between sequences, or carries several, has every slice's QP; before, only the newest
//   SPS's sets were kept and a slice of another had its type alone). An SPS sent again under its id replaces its sets (the
//   pool compacted); one whose sets do not fit has its slices' types, not their QPs. Without the QP, rate control's swings
//   would be read as the content's: a hardware HEVC encoder measured here spends 3x the bits on the picture after each intra
//   one and next to nothing more on a cut.
// - Complexity: the slices' bits per pixel, normalised to QP 26 (QP + 6 halves the bits for the same content).
// - Activity: an inter picture's complexity over the intra pictures' (an exponential mean of the last few): a still scene
//   codes in a few percent of an intra picture, motion more, everything new (a cut) about as much.
// - Bursts: against the exponential mean of its own class, cuts left out of the mean. A class (2026-11-03) is P or B times the
//   picture's place in the prediction structure: its nearest reference's distance (HEVC's short-term set: 1, 2, 3-4, 5-8, 9+)
//   or, unknown, its layer (H.264: reference or not). A hierarchical GOP's anchor, predicted from 4 pictures back, costs 3-5x
//   its neighbours: against one mean for every B picture it was a "burst" every 4 pictures (252 false cuts on a 4K HEVC reel).
// - Scene cuts: an inter picture costing at least half an intra one and several times its kind's mean (an encoder that coded
//   the cut as a P picture of intra blocks), confirmed by the next picture of its kind falling back below kPersist of its cost
//   (the new shot predicts well) or withdrawn when it does not (a burst of new detail, water or a crowd suddenly churning over
//   a third of the picture, looks the same for one picture: measured with both platform encoders); an intra picture out of the
//   GOP's rhythm (an encoder's scene-cut detection put it there); a scheduled intra picture whose cost is unlike the last ones',
//   at most kClockKey (0.4, not a cut) when it is on the beat: an encoder's clock says nothing of the content, and a fade
//   changes an intra picture's cost 5-14x in a GOP. In a hierarchical GOP a picture narrower than its mini-GOP's anchor is
//   judged only if the anchor (predicted across any cut in it) burst too: one leaf alone is a flash. A step down (a cut into
//   a much simpler shot, every class kStepDown or more under its mean after a level that held kSustain pictures) is
//   reported on the confirming picture as a revision of the first (`step_down`); a burst of new detail that lasted longer
//   than kSustain and stopped at once looks the same.
//   The first kAfterKey inter pictures after an intra one are not judged (an encoder's prediction structure around its key
//   pictures costs there: a cut one or two pictures after a key can be missed). The kind's mean falls fast (half the way
//   a picture) and rises slowly (0.15), so a burst's level does not hide a cut that follows it.
class BitstreamPerception final {
public:
    enum : std::uint32_t { kH264 = 1, kHevc = 2 };
    // `codec`: kH264 or kHevc; `config`: the avcC / hvcC record (null: parameter sets in band only); `nal_length_bytes` 1, 2
    // or 4 (0: the record's). False for a bad codec, length or record.
    [[nodiscard]] bool Configure(std::uint32_t codec, const std::uint8_t* config, std::uint32_t config_bytes, std::uint32_t nal_length_bytes) noexcept;
    // One access unit (decode order). False when it is not a sequence of whole NAL units.
    bool Inspect(const std::uint8_t* unit, std::uint32_t bytes, BitstreamFrame& out) noexcept;
    void ResetStatistics() noexcept;          // The means and the GOP rhythm (a seek); the parameter sets stay.
    [[nodiscard]] std::uint32_t Codec() const noexcept { return codec_; }
    [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }   // The newest slice's sequence (0 before one).
    [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }
private:
    struct Sps264 { bool valid = false, frame_mbs_only = true, always_zero = false, separate = false; std::uint8_t frame_bits = 4, poc_type = 0, poc_bits = 4, chroma = 1; std::uint16_t width_mbs = 0, height_units = 0; };
    struct Pps264 { bool valid = false, entropy = false, bottom_poc = false, weighted = false, redundant = false; std::uint8_t sps = 0, bipred = 0, refs0 = 1, refs1 = 1; std::int8_t qp = 26; };
    struct SpsHevc {
        bool valid = false, separate = false, sao = false, long_term = false, temporal_mvp = false;
        bool sets_known = false;          // Its sets are in the pool from set_first (else its slices' QPs are unknown).
        std::uint8_t address_bits = 0, poc_bits = 4, chroma = 1, sets = 0, long_terms = 0;
        std::uint16_t set_first = 0;
        std::uint32_t long_term_used = 0; // Bit i: lt_ref_pic i of the SPS is used by the current picture.
        std::uint16_t width = 0, height = 0;
    };
    struct PpsHevc {
        bool valid = false, dependent = false, output_flag = false, cabac_init = false, weighted = false, bipred = false, lists = false;
        std::uint8_t sps = 0, extra_bits = 0, refs0 = 1, refs1 = 1; std::int8_t qp = 26;
    };
public:
    // A short-term reference picture set (H.265 7.4.8): its delta POCs, negatives first, and which the picture uses.
    struct ReferenceSet { std::uint8_t negative = 0, positive = 0; std::uint32_t used = 0; std::int16_t delta[32]{}; };
private:
    bool ParameterSet(const std::uint8_t* nal, std::uint32_t bytes) noexcept;
    // One slice NAL: its type ('I', 'P', 'B'), its QP (-1 unknown), its sequence's size.
    bool Slice(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept;
    bool Slice264(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept;
    bool SliceHevc(const std::uint8_t* nal, std::uint32_t bytes, char& type, int& qp, bool& key) noexcept;
    // The HEVC SPSs' reference picture sets (2026-11-03): each SPS's sets contiguous in entries_ (from its set_first), their
    // deltas contiguous in deltas_ (from each entry's first), in the order the SPSs came.
    static constexpr unsigned kSetEntries = 256, kDeltas = 1536;
    struct SetEntry { std::uint16_t first = 0; std::uint8_t negative = 0, positive = 0; std::uint32_t used = 0; };
    void DropSets(unsigned sps) noexcept;                 // Its sets out of the pool (compacted).
    bool StoreSet(const ReferenceSet& set) noexcept;      // Appended; false when the pool is full.
    [[nodiscard]] ReferenceSet LoadSet(unsigned entry) const noexcept;
    std::uint32_t codec_ = 0, length_ = 4, width_ = 0, height_ = 0;
    Sps264 sps264_[32]{}; Pps264 pps264_[256]{};
    SpsHevc spshevc_[16]{}; PpsHevc ppshevc_[64]{};
    SetEntry entries_[kSetEntries]{};
    std::int16_t deltas_[kDeltas]{};
    std::uint16_t entry_count_ = 0, delta_count_ = 0;
    char last_type_ = 'I'; int last_qp_ = -1; // A dependent slice segment's type and QP are its slice's.
    unsigned slice_layer_ = 0, slice_distance_ = 0, last_distance_ = 0; // The slice just read: its layer, its nearest reference.
    // Statistics.
    std::uint32_t index_ = 0, last_key_ = 0, scheduled_ = 0, period_ = 0; // Intra pictures: the last, the last on the beat, the beat.
    bool keyed_ = false;
    // Inter pictures are judged against their own class (2026-11-03): P or B, times their place in the prediction structure
    // (the nearest reference's distance in octaves 1, 2, 3-4, 5-8, 9+; unknown: the layer 0, 1, 2+). A hierarchical GOP's
    // anchor, predicted from 4 or 8 pictures back, costs 3-5x its neighbours with nothing new in view: one mean for every B
    // picture read each anchor as a burst, and the cheap picture after it "confirmed" a cut (252 on a 3,000-picture HEVC reel).
    static constexpr unsigned kClasses = 16;
    [[nodiscard]] static unsigned Class(const BitstreamFrame& frame) noexcept;
    double intra_ = 0, inter_[kClasses]{};
    static constexpr double kPersist = 0.5, kClockKey = 0.4; // kClockKey: the most an intra picture on the GOP's beat scores.
    static constexpr unsigned kAfterKey = 2;  // Inter pictures after an intra one that are not judged for cuts.
    struct Pending { bool open = false; std::uint32_t index = 0; double p = 0, complexity = 0; };
    Pending pending_[kClasses]{};             // A cut to confirm, of each class.
    unsigned after_key_ = 0;                  // Inter pictures since the last intra one.
    unsigned anchor_distance_ = 0, previous_distance_ = 0; // The current mini-GOP's anchor's reference distance; the last picture's.
    double anchor_burst_ = 1;                 // The anchor's burst.
    // Step-down cuts (2026-11-03): the drop's factor, the pictures the level must have held, how busy it must have been.
    static constexpr double kStepDown = 3.5, kBusy = 0.1;
    static constexpr unsigned kSustain = 30, kDropSpan = 4; // kDropSpan: the confirming picture comes within it.
    unsigned quiet_ = 0;                      // Inter pictures since the last intra one, cut candidate or burst.
    std::uint32_t drop_index_ = 0; bool drop_open_ = false; // The last picture, when it dropped.
};
}
