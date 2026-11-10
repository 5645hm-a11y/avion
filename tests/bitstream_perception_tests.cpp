// Pre-decode bitstream perception (docs/picture.md, "Bitstream perception"; docs/sdk.md, API 9), 2026-11-02. Suite #203.
// Portable (MSVC; GCC -O2 and ASan/UBSan):
// - headers written here bit by bit, read to their QP: a Baseline P slice; a High-profile B slice under CABAC with a scaling
//   matrix, field coding allowed, POC type 1, redundant_pic_cnt, an override of the reference counts, a list modification,
//   the bipred weight tables for both lists and adaptive reference marking (QP 30 - 7 = 23); an IDR I slice under the same
//   PPS (no cabac_init_idc: QP 35); slice groups (FMO type 6) in the PPS; HEVC with a sub-layer's profile and level, a
//   scaling list, SAO, two short-term sets (one predicted from the other), long-term pictures, tiles, list modification and
//   weights: a B slice (QP 25) and its dependent segment (its slice's type and QP), a P slice with a predicted set of its own
//   (QP 35), an IDR (QP 20);
// - a stream from an encoder written here (no platform encoder): 320 x 240 H.264 at 30/s, 170 pictures, an IDR of I_PCM
//   macroblocks every 60, P pictures whose changed macroblocks are I_PCM and the rest skipped (conditional replenishment: the
//   bytes are the motion): a textured still background, a 32 x 32 object moving 2 samples a picture, faster (10, with a second
//   40 x 40 object crossing at 18) for pictures 70-84, a cut at 100 (a new background). Inspected: the cut picture's probability 0.9 or more, every
//   other picture's under 0.3; the burst's first pictures flagged; the still and slow pictures' activity under 0.1 and the
//   burst's 2x and more theirs; the same encoder putting an IDR at the cut (early for the GOP's beat): 0.9 there, and the next
//   scheduled IDR (or one a GOP after the cut, for an encoder that restarts its GOP) not a cut;
// - the C SDK's inspector on that stream: the same answers, MEDIA_ERROR_EMPTY for a unit of parameter sets alone,
//   MEDIA_ERROR_INVALID_ARGUMENT for a cut-short unit; no allocation; under 0.02 ms a picture (optimised builds).
// Windows (platform encoders, tests/platform_hevc_encoder.hpp): Microsoft's H.264 MFT (High, CABAC, B pictures) and the
// hardware HEVC encoder on a 320 x 192 scene: a textured object drifting 1 sample a picture, 9 during pictures 40-54 while a
// churning region (detail new every picture) fills a third of the picture, and a cut (70): every picture's QP known (HEVC's
// too: the reference sets followed), B pictures among them; the cut found within the B pictures' reordering of 70 and
// nowhere else (as finally judged: the burst's onset looks like a cut for one picture, and the next picture of its kind
// withdraws it); the burst's activity twice the slow motion's or more; the inspection's time per picture. (A rigid
// translation, however fast, costs a real encoder a motion vector: activity is new detail, not speed.)
// MEDIA_BITSTREAM_DUMP=1 prints every unit's answer.
#include "media/c_api.h"
#include "media/codec/BitstreamPerception.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>
#if defined(_WIN32)
#include "platform_hevc_encoder.hpp"
#endif

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
using Bytes = std::vector<std::uint8_t>;
struct Bits {
    Bytes b; unsigned bits = 0;
    void Bit(unsigned v) { if (!(bits % 8)) b.push_back(0); if (v) b.back() |= static_cast<std::uint8_t>(1u << (7 - bits % 8)); ++bits; }
    void Value(unsigned v, unsigned n) { while (n) Bit((v >> --n) & 1); }
    void Ue(unsigned v) { unsigned n = 0; for (unsigned t = v + 1; t > 1; t >>= 1) ++n; for (unsigned i = 0; i < n; ++i) Bit(0); Value(v + 1, n + 1); }
    void Se(int v) { Ue(v > 0 ? 2 * static_cast<unsigned>(v) - 1 : 2 * static_cast<unsigned>(-v)); }
    void Align() { while (bits % 8) Bit(0); }
    void Byte(unsigned v) { Value(v, 8); }
    // The NAL unit: header byte(s), the RBSP with its stop bit, emulation prevention.
    Bytes Nal(std::initializer_list<unsigned> header) {
        Bit(1); Align();
        Bytes out; for (auto h : header) out.push_back(static_cast<std::uint8_t>(h));
        unsigned zeros = 0;
        for (auto v : b) { if (zeros == 2 && v <= 3) { out.push_back(3); zeros = 0; } out.push_back(v); zeros = v == 0 ? zeros + 1 : 0; }
        return out;
    }
};
// Length-prefixed (4 bytes) NAL units: one access unit.
Bytes Unit(std::initializer_list<const Bytes*> nals) {
    Bytes out;
    for (const auto* n : nals) { const auto size = static_cast<std::uint32_t>(n->size()); for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i))); out.insert(out.end(), n->begin(), n->end()); }
    return out;
}
codec::BitstreamFrame Inspect(codec::BitstreamPerception& p, const Bytes& unit) {
    codec::BitstreamFrame f; CHECK(p.Inspect(unit.data(), static_cast<std::uint32_t>(unit.size()), f)); return f;
}

// ---- Headers ----
Bytes BaselineSps(unsigned width_mbs, unsigned height_mbs) {
    Bits b; b.Byte(66); b.Byte(0); b.Byte(30); b.Ue(0); b.Ue(0); b.Ue(0); b.Ue(0); // sps 0, frame_num 4 bits, POC type 0 with 4 bits.
    b.Ue(1); b.Bit(0); b.Ue(width_mbs - 1); b.Ue(height_mbs - 1); b.Bit(1); b.Bit(1); b.Bit(0); b.Bit(0);
    return b.Nal({0x67});
}
Bytes BaselinePps() { // CAVLC, one slice group, one reference, QP 26, deblocking control present.
    Bits b; b.Ue(0); b.Ue(0); b.Bit(0); b.Bit(0); b.Ue(0); b.Ue(0); b.Ue(0); b.Bit(0); b.Value(0, 2); b.Se(0); b.Se(0); b.Se(0); b.Bit(1); b.Bit(0); b.Bit(0);
    return b.Nal({0x68});
}
void TestHeaders() {
    codec::BitstreamPerception p;
    CHECK(p.Configure(codec::BitstreamPerception::kH264, nullptr, 0, 4));
    const auto sps = BaselineSps(20, 15), pps = BaselinePps();
    {
        Bits b; b.Ue(0); b.Ue(5); b.Ue(0); b.Value(1, 4); b.Value(2, 4); b.Bit(0); b.Bit(0); b.Bit(0); b.Se(-3); b.Ue(1);
        const auto slice = b.Nal({0x21});
        const auto f = Inspect(p, Unit({&sps, &pps, &slice}));
        std::printf("Baseline P slice: type %c, QP %d, %ux%u, parameter sets %d\n", f.type, f.qp, p.Width(), p.Height(), f.parameter_sets);
        CHECK(f.type == 'P' && f.qp == 23 && f.parameter_sets && f.slices == 1 && p.Width() == 320 && p.Height() == 240 && !f.key);
    }
    {
        // High profile: a scaling matrix (one 4x4 list), 6 bits of frame_num, POC type 1 (a cycle of two), fields allowed.
        Bits s; s.Byte(100); s.Byte(0); s.Byte(40); s.Ue(0);
        s.Ue(1); s.Ue(0); s.Ue(0); s.Bit(0); s.Bit(1);
        for (unsigned i = 0; i < 8; ++i) { s.Bit(i == 0); if (i == 0) for (unsigned j = 0; j < 16; ++j) s.Se(j % 3 ? 1 : -2); }
        s.Ue(2); s.Ue(1); s.Bit(0); s.Se(-1); s.Se(0); s.Ue(2); s.Se(2); s.Se(2);
        s.Ue(2); s.Bit(0); s.Ue(19); s.Ue(7); s.Bit(0); s.Bit(0); s.Bit(1); s.Bit(0); s.Bit(0);
        const auto high_sps = s.Nal({0x67});
        // PPS 3: CABAC, bottom-field POC, two default references in list 0, weighted P and bipred 1, QP 30, redundant_pic_cnt.
        Bits q; q.Ue(3); q.Ue(0); q.Bit(1); q.Bit(1); q.Ue(0); q.Ue(1); q.Ue(0); q.Bit(1); q.Value(1, 2); q.Se(4); q.Se(0); q.Se(0); q.Bit(0); q.Bit(0); q.Bit(1);
        q.Bit(0); // transform_8x8_mode_flag (High's extension: not read).
        const auto high_pps = q.Nal({0x68});
        Bits b; b.Ue(0); b.Ue(6); b.Ue(3); b.Value(5, 6); b.Bit(0); // B, PPS 3, frame_num, a frame (not a field).
        b.Se(3); b.Se(-1); b.Ue(0);                           // delta_pic_order_cnt[0..1], redundant_pic_cnt.
        b.Bit(1); b.Bit(1); b.Ue(1); b.Ue(1);                 // direct_spatial; override: 2 and 2 references.
        b.Bit(1); b.Ue(0); b.Ue(3); b.Ue(3); b.Bit(0);        // List 0 modified (abs_diff 4), list 1 not.
        b.Ue(5); b.Ue(4);                                     // Weights: luma and chroma denominators.
        b.Bit(1); b.Se(33); b.Se(-2); b.Bit(0);               // List 0, ref 0: luma only.
        b.Bit(0); b.Bit(1); for (int i = 0; i < 4; ++i) b.Se(i - 2);
        b.Bit(0); b.Bit(0);                                   // List 1, ref 0: none.
        b.Bit(1); b.Se(1); b.Se(1); b.Bit(1); for (int i = 0; i < 4; ++i) b.Se(-i);
        b.Bit(1); b.Ue(1); b.Ue(2); b.Ue(3); b.Ue(0); b.Ue(1); b.Ue(0); // Adaptive marking: MMCO 1 (2), MMCO 3 (0, 1), end.
        b.Ue(2); b.Se(-7);                                    // cabac_init_idc, slice_qp_delta.
        const auto b_slice = b.Nal({0x41});
        const auto f = Inspect(p, Unit({&high_sps, &high_pps, &b_slice}));
        std::printf("High CABAC B slice: type %c, QP %d, %ux%u\n", f.type, f.qp, p.Width(), p.Height());
        CHECK(f.type == 'B' && f.qp == 23 && p.Width() == 320 && p.Height() == 256);
        Bits i; i.Ue(0); i.Ue(7); i.Ue(3); i.Value(0, 6); i.Bit(0); i.Ue(4); i.Se(0); i.Se(0); i.Ue(0); i.Bit(0); i.Bit(0); i.Se(5);
        const auto idr = i.Nal({0x65});
        const auto g = Inspect(p, Unit({&idr}));
        std::printf("High IDR I slice: type %c, QP %d, key %d\n", g.type, g.qp, g.key);
        CHECK(g.type == 'I' && g.qp == 35 && g.key);
    }
    {
        // Slice groups: FMO map type 6 (a group id for each of 300 map units, 1 bit each).
        Bits q; q.Ue(1); q.Ue(0); q.Bit(0); q.Bit(0); q.Ue(1); q.Ue(6); q.Ue(299); for (unsigned u = 0; u < 300; ++u) q.Bit(u % 7 == 0);
        q.Ue(0); q.Ue(0); q.Bit(0); q.Value(0, 2); q.Se(-6); q.Se(0); q.Se(0); q.Bit(1); q.Bit(0); q.Bit(0);
        const auto fmo = q.Nal({0x68});
        Bits b; b.Ue(0); b.Ue(5); b.Ue(1); b.Value(3, 4); b.Value(6, 4); b.Bit(0); b.Bit(0); b.Bit(0); b.Se(2); b.Ue(1);
        const auto slice = b.Nal({0x21});
        const auto f = Inspect(p, Unit({&sps, &fmo, &slice}));
        std::printf("FMO PPS P slice: type %c, QP %d\n", f.type, f.qp);
        CHECK(f.type == 'P' && f.qp == 22);
    }
    {
        // HEVC: one sub-layer more (its profile and level present), 320 x 192 in 64-sample CTBs (15: 4 bits of address); a
        // scaling list, SAO, two short-term sets (the second predicted from the first: deltas -1 -2 -4, two in use), two
        // long-term pictures in the SPS (the first in use), temporal MVP.
        codec::BitstreamPerception h;
        CHECK(h.Configure(codec::BitstreamPerception::kHevc, nullptr, 0, 4));
        Bits s; s.Value(0, 4); s.Value(1, 3); s.Bit(1);
        for (unsigned i = 0; i < 88; ++i) { s.Bit(i % 5 == 1); }
        s.Byte(93);
        s.Bit(1); s.Bit(1); for (unsigned i = 1; i < 8; ++i) s.Value(0, 2);
        for (unsigned i = 0; i < 88; ++i) { s.Bit(i % 3 == 0); }
        s.Byte(90);
        s.Ue(0); s.Ue(1); s.Ue(320); s.Ue(192); s.Bit(0); s.Ue(0); s.Ue(0); s.Ue(4); s.Bit(1);
        for (unsigned i = 0; i < 2; ++i) { s.Ue(4); s.Ue(2); s.Ue(0); }
        s.Ue(0); s.Ue(3); s.Ue(0); s.Ue(3); s.Ue(1); s.Ue(1);
        s.Bit(1); s.Bit(1); // scaling_list_enabled, its data present:
        for (unsigned size = 0; size < 4; ++size)
            for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
                const bool coded = (size == 0 && matrix == 1) || (size == 2 && matrix == 4);
                s.Bit(coded);
                if (!coded) { s.Ue(0); continue; }
                if (size > 1) s.Se(-3);
                for (unsigned i = 0; i < (size ? 64u : 16u); ++i) s.Se(i % 2 ? 1 : -1);
            }
        s.Bit(1); s.Bit(1); s.Bit(0);                          // AMP, SAO, no PCM.
        s.Ue(2);                                               // Two short-term sets:
        s.Ue(2); s.Ue(1); s.Ue(0); s.Bit(1); s.Ue(1); s.Bit(0); s.Ue(0); s.Bit(1); // -1 (used) -3; +1 (used).
        s.Bit(1); s.Bit(1); s.Ue(0);                           // Predicted from it, delta -1:
        s.Bit(1); s.Bit(0); s.Bit(1); s.Bit(1); s.Bit(1);      //   j0 used, j1 kept, j2 used, the delta itself used.
        s.Bit(1); s.Ue(2); s.Value(5, 8); s.Bit(1); s.Value(9, 8); s.Bit(0); // Long-term in the SPS: 5 (used), 9.
        s.Bit(1); s.Bit(0); s.Bit(0); s.Bit(0);                // Temporal MVP; strong intra smoothing, VUI, extension off.
        const auto hsps = s.Nal({0x42, 0x01});
        // PPS: dependent slices, pic_output_flag, 2 extra bits, cabac_init_present, 2 and 1 references, QP 29, cu_qp_delta,
        // weighted P and B, 2 x 2 tiles (not uniform), deblocking offsets, list modification.
        Bits q; q.Ue(0); q.Ue(0); q.Bit(1); q.Bit(1); q.Value(2, 3); q.Bit(0); q.Bit(1); q.Ue(1); q.Ue(0); q.Se(3);
        q.Bit(0); q.Bit(0); q.Bit(1); q.Ue(1); q.Se(0); q.Se(0); q.Bit(0); q.Bit(1); q.Bit(1); q.Bit(0);
        q.Bit(1); q.Bit(0); q.Ue(1); q.Ue(1); q.Bit(0); q.Ue(2); q.Ue(1); q.Bit(1);
        q.Bit(1); q.Bit(1); q.Bit(1); q.Bit(0); q.Se(-1); q.Se(2); q.Bit(0); q.Bit(1); q.Ue(0); q.Bit(0);
        const auto hpps = q.Nal({0x44, 0x01});
        // A B slice through the SPS's second set (2 in use) with one long-term from the SPS and one of its own (4 in all).
        Bits a; a.Bit(1); a.Ue(0); a.Value(1, 2); a.Ue(0); a.Bit(1); a.Value(7, 8);
        a.Bit(1); a.Value(1, 1);                               // The SPS's set 1.
        a.Ue(1); a.Ue(1); a.Value(0, 1); a.Bit(0); a.Value(3, 8); a.Bit(1); a.Bit(1); a.Ue(2); // Long-term: SPS 0; own (used, msb).
        a.Bit(1); a.Bit(1); a.Bit(0);                          // Temporal MVP; SAO luma, not chroma.
        a.Bit(1); a.Ue(2); a.Ue(1);                            // Override: 3 and 2 references.
        a.Bit(1); a.Value(1, 2); a.Value(3, 2); a.Value(0, 2); a.Bit(0); // List 0 modified (2 bits an entry), list 1 not.
        a.Bit(0); a.Bit(1); a.Bit(0); a.Ue(1);                 // mvd_l1_zero, cabac_init, collocated from list 1, its index.
        a.Ue(6); a.Se(-1);                                     // Weights: denominators.
        a.Bit(1); a.Bit(0); a.Bit(1); a.Bit(0); a.Bit(1); a.Bit(0); // List 0: luma 1 0 1, chroma 0 1 0.
        a.Se(2); a.Se(-3); for (int i = 0; i < 4; ++i) a.Se(i); a.Se(5); a.Se(1);
        a.Bit(0); a.Bit(1); a.Bit(1); a.Bit(1);                // List 1: luma 0 1, chroma 1 1.
        for (int i = 0; i < 4; ++i) a.Se(-i);
        a.Se(1); a.Se(1); for (int i = 0; i < 4; ++i) a.Se(2);
        a.Ue(2); a.Se(-4);                                     // five_minus_max_num_merge_cand; QP 29 - 4.
        const auto b_slice = a.Nal({1 << 1, 1});
        Bits d; d.Bit(0); d.Ue(0); d.Bit(1); d.Value(5, 4);  // Its dependent segment.
        const auto dependent = d.Nal({1 << 1, 1});
        // A P slice with a set of its own, predicted from the SPS's first (delta_idx 2, +2: deltas +1 +3, both in use).
        Bits c; c.Bit(1); c.Ue(0); c.Value(0, 2); c.Ue(1); c.Bit(1); c.Value(9, 8);
        c.Bit(0); c.Bit(1); c.Ue(1); c.Bit(0); c.Ue(1);        // Its own set: predicted, two back, +2.
        c.Bit(1); c.Bit(0); c.Bit(0); c.Bit(1); c.Bit(0); c.Bit(0); // j0 used, j1 dropped, j2 used, the delta dropped.
        c.Ue(0); c.Ue(0); c.Bit(0);                            // No long-term; temporal MVP off.
        c.Bit(0); c.Bit(0);                                    // SAO off.
        c.Bit(0);                                              // No override: 2 references.
        c.Bit(1); c.Bit(1); c.Bit(0);                          // List 0 modified (1 bit an entry).
        c.Bit(0);                                              // cabac_init_flag.
        c.Ue(3); c.Se(0); c.Bit(0); c.Bit(0); c.Bit(0); c.Bit(0); // Weights: denominators, no weight.
        c.Ue(0); c.Se(6);                                      // QP 35.
        const auto p_slice = c.Nal({0 << 1, 1});
        Bits i; i.Bit(1); i.Bit(0); i.Ue(0); i.Value(0, 2); i.Ue(2); i.Bit(1); i.Bit(1); i.Bit(1); i.Se(-9); // IDR: QP 20.
        const auto idr = i.Nal({19 << 1, 1});
        const auto fb = Inspect(h, Unit({&hsps, &hpps, &b_slice, &dependent})), fp = Inspect(h, Unit({&p_slice})), fi = Inspect(h, Unit({&idr}));
        std::printf("HEVC: %c QP %d (%u segments), %c QP %d, %c QP %d key %d, %ux%u\n", fb.type, fb.qp, fb.slices, fp.type, fp.qp, fi.type, fi.qp, fi.key, h.Width(), h.Height());
        CHECK(fb.type == 'B' && fb.qp == 25 && fb.slices == 2 && fp.type == 'P' && fp.qp == 35 && !fp.key && fi.type == 'I' && fi.qp == 20 && fi.key);
        CHECK(h.Width() == 320 && h.Height() == 192);
    }
}

// ---- HEVC with several sequences at once (2026-11-03) ----
struct HevcSet { std::vector<int> deltas; std::uint32_t used = 0; }; // Negatives (descending) then positives (ascending).
Bytes HevcSps(unsigned id, unsigned width, unsigned height, const std::vector<HevcSet>& sets) {
    Bits s; s.Value(0, 4); s.Value(0, 3); s.Bit(1);                     // VPS 0, one sub-layer.
    for (unsigned i = 0; i < 88; ++i) s.Bit(i % 7 == 2);
    s.Byte(93);
    s.Ue(id); s.Ue(1); s.Ue(width); s.Ue(height); s.Bit(0); s.Ue(0); s.Ue(0); s.Ue(4); // 8 bits of POC.
    s.Bit(1); s.Ue(4); s.Ue(2); s.Ue(0);
    s.Ue(0); s.Ue(3); s.Ue(0); s.Ue(3); s.Ue(1); s.Ue(1);              // 8 to 64-sample CTBs; transform sizes.
    s.Bit(0); s.Bit(1); s.Bit(0); s.Bit(0);                            // No scaling lists, AMP, no SAO, no PCM.
    s.Ue(static_cast<unsigned>(sets.size()));
    for (std::size_t k = 0; k < sets.size(); ++k) {
        if (k) s.Bit(0);                                               // Not predicted.
        unsigned negative = 0; for (int d : sets[k].deltas) negative += d < 0;
        s.Ue(negative); s.Ue(static_cast<unsigned>(sets[k].deltas.size()) - negative);
        int poc = 0;
        for (unsigned i = 0; i < sets[k].deltas.size(); ++i) {
            const int d = sets[k].deltas[i];
            if (i == negative) poc = 0;
            s.Ue(static_cast<unsigned>(std::abs(d - poc)) - 1); poc = d;
            s.Bit((sets[k].used >> i) & 1u);
        }
    }
    s.Bit(0); s.Bit(0); s.Bit(0); s.Bit(0); s.Bit(0);                  // No long-term, no temporal MVP, VUI or extension.
    return s.Nal({0x42, 0x01});
}
Bytes HevcPps(unsigned id, unsigned sps, int qp) {
    Bits q; q.Ue(id); q.Ue(sps); q.Bit(0); q.Bit(0); q.Value(0, 3); q.Bit(0); q.Bit(0); q.Ue(0); q.Ue(0); q.Se(qp - 26);
    q.Bit(0); q.Bit(0); q.Bit(0); q.Se(0); q.Se(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0); q.Bit(0);
    return q.Nal({0x44, 0x01});
}
// A P slice (TRAIL_R) through `pps`, its SPS's set `set` of `sets`.
Bytes HevcPSlice(unsigned pps, unsigned set, unsigned sets, int qp_delta, unsigned poc) {
    Bits c; c.Bit(1); c.Ue(pps); c.Ue(1); c.Value(poc & 255u, 8);
    c.Bit(1); unsigned bits = 0; while ((1u << bits) < sets) ++bits; c.Value(set, bits);
    c.Bit(0); c.Ue(0); c.Se(qp_delta);                                 // No override; five_minus_max_num_merge_cand.
    return c.Nal({1 << 1, 1});
}
// An IDR (IDR_W_RADL) I slice through `pps`.
Bytes HevcIdr(unsigned pps, int qp_delta) {
    Bits c; c.Bit(1); c.Bit(0); c.Ue(pps); c.Ue(2); c.Se(qp_delta);
    return c.Nal({19 << 1, 1});
}
// `nal` grown to `bytes` with slice data the reader does not look at.
Bytes Padded(Bytes nal, std::size_t bytes) { while (nal.size() < bytes) nal.push_back(0xA5); return nal; }

// ---- A hierarchical GOP on an encoder's clock (2026-11-03) ----
// The shape of a 4K HEVC reel that scored 252 false cuts: an IDR every 64 pictures whatever the content (fading in, 5x a GOP),
// mini-GOPs of 4 in decode order (an anchor predicted from 4 pictures back, then 2, 1, 1) whose anchor costs 5x the others;
// one picture of the leaves 3x its kind (a flash); 15 pictures of new detail, every class 4x, that stop at once (a burst);
// a cut into a shot 5x simpler (every class 5x cheaper from there on, the way churning bubbles cut to still honey); one cut
// the anchor across it carries (4x its kind, as much as an intra picture). Found: the two cuts alone (the step down within
// 4 pictures of it, confirmed by the picture after; the other by the next anchor), the reference distances, no clock IDR a cut.
void TestHierarchy() {
    constexpr unsigned kGop = 64, kAll = 5 * kGop, kFlash = 151, kBurstA = 160, kBurstB = 175, kDrop = 230, kHierCut = 285;
    const auto sps = HevcSps(0, 320, 192, {{{-4}, 1}, {{-2}, 1}, {{-1}, 1}}), pps = HevcPps(0, 0, 26);
    std::vector<Bytes> units;
    const std::size_t idr_bytes[5] = {320, 320, 1600, 8000, 8000}; // The clock known from the first GOP on.
    for (unsigned f = 0; f < kAll; ++f) {
        if (f % kGop == 0) {
            const auto idr = Padded(HevcIdr(0, 0), idr_bytes[f / kGop]);
            units.push_back(f == 0 ? Unit({&sps, &pps, &idr}) : Unit({&idr}));
            continue;
        }
        const unsigned place = (f - 1) % 4, set = place == 0 ? 0 : place == 1 ? 1 : 2; // Decode order: the anchor, then 2, 1, 1.
        std::size_t bytes = place == 0 ? 2000 : place == 1 ? 700 : 400;
        bytes += (f * 37u) % (bytes / 20);                                               // A few percent of rate control.
        if (f == kFlash) bytes *= 3;
        if (f >= kBurstA && f < kBurstB) bytes *= 4;
        if (f >= kDrop) bytes /= 5;
        if (f == kHierCut) bytes = 8000;
        const auto slice = Padded(HevcPSlice(0, set, 3, 0, f), bytes);
        units.push_back(Unit({&slice}));
    }
    codec::BitstreamPerception p;
    CHECK(p.Configure(codec::BitstreamPerception::kHevc, nullptr, 0, 4));
    std::vector<codec::BitstreamFrame> frames(units.size());
    allocation_probe::Begin();
    for (std::size_t i = 0; i < units.size(); ++i) CHECK(p.Inspect(units[i].data(), static_cast<std::uint32_t>(units[i].size()), frames[i]));
    CHECK(allocation_probe::End() == 0);
    std::vector<float> final(kAll);
    for (unsigned f = 0; f < kAll; ++f) final[f] = frames[f].scene_cut;
    for (unsigned f = 0; f < kAll; ++f) if (frames[f].revised) final[frames[f].revised_index] = frames[f].revised_scene_cut;
    unsigned cuts = 0, distances_wrong = 0, drop_at = 0; float worst_key = 0;
    for (unsigned f = 0; f < kAll; ++f) {
        if (f % kGop == 0) { worst_key = (std::max)(worst_key, final[f]); if (frames[f].reference_distance) ++distances_wrong; continue; }
        const unsigned place = (f - 1) % 4;
        if (frames[f].reference_distance != (place == 0 ? 4u : place == 1 ? 2u : 1u) || frames[f].layer) ++distances_wrong;
        if (final[f] >= 0.5f && f >= kDrop && f <= kDrop + 4 && !drop_at) { drop_at = f; ++cuts; continue; }
        if (final[f] >= 0.5f) { ++cuts; if (f != kHierCut) std::printf("  picture %u: cut %.2f (burst %.2f)\n", f, final[f], frames[f].burst); }
    }
    std::printf("Hierarchical GOP on a clock: cut %.2f (burst %.1f), step down found at %u (cut %u), %u cut(s) in all, flash %.2f (burst %.1f), clock IDRs at most %.2f, %u distance(s) wrong\n",
                final[kHierCut], frames[kHierCut].burst, drop_at, kDrop, cuts, final[kFlash], frames[kFlash].burst, worst_key, distances_wrong);
    CHECK(final[kHierCut] >= 0.95f && drop_at && cuts == 2 && final[kFlash] < 0.3f && worst_key < 0.5f && distances_wrong == 0);
}

void TestSequences() {
    codec::BitstreamPerception h;
    CHECK(h.Configure(codec::BitstreamPerception::kHevc, nullptr, 0, 4));
    // Two sequences at once: SPS 0 (320 x 192, two sets) and SPS 1 (640 x 360, three), each with its PPS, SPS 1 the newest.
    const auto sps0 = HevcSps(0, 320, 192, {{{-1}, 1}, {{-1, -2}, 3}}), sps1 = HevcSps(1, 640, 360, {{{-2}, 1}, {{-1, -3, 1}, 5}, {{-4}, 1}});
    const auto pps0 = HevcPps(0, 0, 28), pps1 = HevcPps(1, 1, 22);
    const auto a = HevcPSlice(0, 1, 2, 3, 1), b = HevcPSlice(1, 2, 3, -2, 2);
    const auto fa = Inspect(h, Unit({&sps0, &sps1, &pps0, &pps1, &a}));
    const unsigned wa = h.Width();
    const auto fb = Inspect(h, Unit({&b}));
    const unsigned wb = h.Width();
    const auto fa2 = Inspect(h, Unit({&a}));
    std::printf("HEVC two sequences: SPS 0's slice %c QP %d (%u wide), SPS 1's %c QP %d (%u wide), SPS 0's again QP %d\n", fa.type, fa.qp, wa, fb.type, fb.qp, wb, fa2.qp);
    CHECK(fa.type == 'P' && fa.qp == 31 && wa == 320 && fb.type == 'P' && fb.qp == 20 && wb == 640 && fa2.qp == 31);
    // SPS 0 sent again with other sets (its old ones leave the pool from under SPS 1's), then a sequence whose 64 sets of 32
    // deltas cannot fit: its slices have their type, not their QP, and the others are untouched.
    const auto sps0b = HevcSps(0, 320, 192, {{{-1}, 1}, {{-1, -2, 1, 2}, 15}, {{-3}, 1}});
    std::vector<HevcSet> huge(64);
    for (auto& set : huge) { for (int d = 1; d <= 16; ++d) set.deltas.push_back(-d); for (int d = 1; d <= 16; ++d) set.deltas.push_back(d); set.used = 0xFFFFFFFFu; }
    const auto sps2 = HevcSps(2, 160, 96, huge), pps2 = HevcPps(2, 2, 30);
    const auto a3 = HevcPSlice(0, 2, 3, -1, 3), b3 = HevcPSlice(1, 1, 3, 4, 4), c3 = HevcPSlice(2, 63, 64, 0, 5);
    const auto f0 = Inspect(h, Unit({&sps0b, &a3})), f1 = Inspect(h, Unit({&b3})), f2 = Inspect(h, Unit({&sps2, &pps2, &c3})), f3 = Inspect(h, Unit({&b3})), f4 = Inspect(h, Unit({&a3}));
    std::printf("HEVC SPS 0 replaced: QP %d; SPS 1: QP %d; SPS 2 (too many sets): %c QP %d; then SPS 1 QP %d, SPS 0 QP %d\n", f0.qp, f1.qp, f2.type, f2.qp, f3.qp, f4.qp);
    CHECK(f0.qp == 27 && f1.qp == 26 && f2.type == 'P' && f2.qp == -1 && f3.qp == 26 && f4.qp == 27);
}

// ---- A conditional-replenishment encoder: I_PCM where a macroblock changed, skipped where it did not ----
constexpr unsigned kW = 320, kH = 240, kMbW = kW / 16, kMbH = kH / 16, kPictures = 170, kCut = 100, kBurst0 = 70, kBurst1 = 85;
double Smooth(double x, double y, double phase) {
    return 110 + 35 * std::sin(x / 9.0 + phase) * std::cos(y / 7.0 - phase) + 20 * std::sin((x + 2 * y) / 13.0 + 2 * phase);
}
// The luma of picture f at (x, y).
std::uint8_t Luma(unsigned f, unsigned x, unsigned y) {
    const double phase = f < kCut ? 0.0 : 1.7; // A new background from the cut on.
    double v = Smooth(x, y, phase);
    // The object: 2 samples a picture, 10 during the burst (then 2 again from where it is).
    const auto position = [](unsigned g) {
        const unsigned slow0 = (std::min)(g, kBurst0), fast = g > kBurst0 ? (std::min)(g, kBurst1) - kBurst0 : 0, slow1 = g > kBurst1 ? g - kBurst1 : 0;
        return static_cast<unsigned>((20 + 2 * slow0 + 10 * fast + 2 * slow1) % 288);
    };
    const unsigned ox = position(f), oy = 100;
    if (x >= ox && x < ox + 32 && y >= oy && y < oy + 32) v = 230 - ((x - ox) * 3 + (y - oy) * 5) % 40;
    if (f >= kBurst0 && f < kBurst1) { // A second object only during the burst, crossing fast.
        const unsigned bx = (f - kBurst0) * 18, by = 30;
        if (x >= bx && x < bx + 40 && y >= by && y < by + 40) v = 20 + (x * 7 + y) % 30;
    }
    return static_cast<std::uint8_t>(std::lround(v));
}
struct Encoder {
    std::vector<std::uint8_t> previous = std::vector<std::uint8_t>(kW * kH);
    unsigned frame_num = 0, idr_id = 0;
    // One access unit: an IDR (all I_PCM) or a P picture (changed macroblocks I_PCM), QP 26 + qp_delta.
    Bytes Picture(unsigned f, bool idr, int qp_delta, const Bytes* sps, const Bytes* pps) {
        std::vector<std::uint8_t> now(kW * kH);
        for (unsigned y = 0; y < kH; ++y) for (unsigned x = 0; x < kW; ++x) now[y * kW + x] = Luma(f, x, y);
        if (idr) frame_num = 0;
        Bits b; b.Ue(0); b.Ue(idr ? 7 : 5); b.Ue(0); b.Value(frame_num % 16, 4);
        if (idr) b.Ue(idr_id++ % 2);
        b.Value((2 * frame_num) % 16, 4);
        if (!idr) { b.Bit(0); b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); }
        b.Se(qp_delta); b.Ue(1);
        const auto pcm = [&](unsigned mb) {
            b.Align();
            const unsigned mx = mb % kMbW * 16, my = mb / kMbW * 16;
            for (unsigned i = 0; i < 256; ++i) b.Byte(now[(my + i / 16) * kW + mx + i % 16]);
            for (unsigned i = 0; i < 128; ++i) b.Byte(128);
        };
        unsigned run = 0;
        for (unsigned mb = 0; mb < kMbW * kMbH; ++mb) {
            if (idr) { b.Ue(25); pcm(mb); continue; }
            bool changed = false;
            const unsigned mx = mb % kMbW * 16, my = mb / kMbW * 16;
            for (unsigned i = 0; i < 256 && !changed; ++i) changed = now[(my + i / 16) * kW + mx + i % 16] != previous[(my + i / 16) * kW + mx + i % 16];
            if (!changed) { ++run; continue; }
            b.Ue(run); run = 0; b.Ue(30); pcm(mb);
        }
        if (run) b.Ue(run);
        ++frame_num; previous = now;
        const auto slice = b.Nal({idr ? 0x65u : 0x21u});
        if (sps && pps) return Unit({sps, pps, &slice});
        return Unit({&slice});
    }
};
// 0: IDRs every 60 only; 1: also an IDR at the cut (the GOP's beat kept: 120 next); 2: an IDR at the cut restarting the GOP (160).
std::vector<Bytes> Stream(unsigned variant) {
    Encoder e; const auto sps = BaselineSps(kMbW, kMbH), pps = BaselinePps();
    std::vector<Bytes> units;
    for (unsigned f = 0; f < kPictures; ++f) {
        const bool idr = variant == 2 ? (f < kCut ? f % 60 == 0 : (f - kCut) % 60 == 0) : (f % 60 == 0 || (variant == 1 && f == kCut));
        units.push_back(e.Picture(f, idr, 0, f == 0 ? &sps : nullptr, f == 0 ? &pps : nullptr));
    }
    return units;
}
void TestReplenishment() {
    for (unsigned variant = 0; variant < 3; ++variant) {
        const auto units = Stream(variant);
        codec::BitstreamPerception p;
        CHECK(p.Configure(codec::BitstreamPerception::kH264, nullptr, 0, 4));
        std::vector<codec::BitstreamFrame> frames(units.size());
        allocation_probe::Begin();
        for (std::size_t i = 0; i < units.size(); ++i) CHECK(p.Inspect(units[i].data(), static_cast<std::uint32_t>(units[i].size()), frames[i]));
        const auto allocations = allocation_probe::End();
        CHECK(allocations == 0);
        unsigned false_cuts = 0, burst_flags = 0; double slow = 0, fast = 0; unsigned slow_n = 0, fast_n = 0; float worst = 0;
        // Each picture's probability as finally judged: the first look, or the revision a later picture made of it.
        std::vector<float> final(kPictures);
        for (unsigned f = 0; f < kPictures; ++f) final[f] = frames[f].scene_cut;
        for (unsigned f = 0; f < kPictures; ++f) if (frames[f].revised) { CHECK(frames[f].revised_index < f); final[frames[f].revised_index] = frames[f].revised_scene_cut; }
        for (unsigned f = 0; f < kPictures; ++f) {
            const auto& r = frames[f];
            if (f == kCut) continue;
            if (final[f] >= 0.3f) { ++false_cuts; std::printf("  picture %u: %c scene cut %.2f (complexity %.3f, activity %.3f, burst %.2f)\n", f, r.type, final[f], r.complexity, r.activity, r.burst); }
            worst = (std::max)(worst, final[f]);
            if (r.motion_burst && f >= kBurst0 && f < kBurst0 + 3) ++burst_flags;
            if (r.type == 'P' && f > 5 && f < kBurst0) { slow += r.activity; ++slow_n; }
            if (r.type == 'P' && f >= kBurst0 + 1 && f < kBurst1) { fast += r.activity; ++fast_n; }
        }
        slow /= slow_n ? slow_n : 1; fast /= fast_n ? fast_n : 1;
        const auto& cut = frames[kCut];
        std::printf("Replenishment stream %u: cut %c %.2f (activity %.2f, burst %.1f); other pictures at most %.2f; burst flagged on %u of its first 3; activity %.3f slow, %.3f burst\n",
                    variant, cut.type, cut.scene_cut, cut.activity, cut.burst, worst, burst_flags, slow, fast);
        CHECK(final[kCut] >= 0.9f && false_cuts == 0);
        CHECK(slow < 0.1 && fast >= 2 * slow && burst_flags >= 1);
        if (variant == 1) CHECK(cut.type == 'I' && cut.key && frames[120].type == 'I' && frames[120].scene_cut < 0.3f);
        if (variant == 2) CHECK(cut.type == 'I' && frames[160].type == 'I' && frames[160].scene_cut < 0.3f);
        if (variant == 0) CHECK(cut.type == 'P' && cut.activity > 0.9f && frames[kCut + 1].revised && frames[kCut + 1].revised_index == kCut && final[kCut] >= 0.95f);
    }
}
void TestSdk() {
    const auto units = Stream(0);
    static media_bitstream_inspector_t inspector; // 10 KB: not on the stack.
    CHECK(media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, nullptr, 0, 4) == MEDIA_OK);
    CHECK(media_bitstream_inspector_init(&inspector, 99, nullptr, 0, 4) == MEDIA_ERROR_INVALID_ARGUMENT);
    // An avcC record: the parameter sets from it, not in band.
    const auto sps = BaselineSps(kMbW, kMbH), pps = BaselinePps();
    Bytes avcc{1, 66, 0, 30, 0xFF, 0xE1};
    avcc.push_back(static_cast<std::uint8_t>(sps.size() >> 8)); avcc.push_back(static_cast<std::uint8_t>(sps.size())); avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1); avcc.push_back(static_cast<std::uint8_t>(pps.size() >> 8)); avcc.push_back(static_cast<std::uint8_t>(pps.size())); avcc.insert(avcc.end(), pps.begin(), pps.end());
    CHECK(media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, avcc.data(), static_cast<uint32_t>(avcc.size()), 0) == MEDIA_OK);
    media_bitstream_info_t info{}; info.struct_size = sizeof(info);
    const Bytes sets = Unit({&sps, &pps});
    CHECK(media_inspect_bitstream_packet(&inspector, sets.data(), static_cast<uint32_t>(sets.size()), &info) == MEDIA_ERROR_EMPTY && (info.flags & MEDIA_BITSTREAM_PARAMETER_SETS));
    CHECK(media_inspect_bitstream_packet(&inspector, units[1].data(), 7, &info) == MEDIA_ERROR_INVALID_ARGUMENT);
    CHECK(media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, avcc.data(), static_cast<uint32_t>(avcc.size()), 0) == MEDIA_OK);
    unsigned cuts = 0, cut_at = 0, qp_known = 0;
    const unsigned rounds = 40;
    double seconds = 0; std::uint64_t allocations = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        if (round) CHECK(media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, avcc.data(), static_cast<uint32_t>(avcc.size()), 0) == MEDIA_OK);
        const auto start = std::chrono::steady_clock::now();
        allocation_probe::Begin();
        for (unsigned f = 0; f < kPictures; ++f) {
            const auto status = media_inspect_bitstream_packet(&inspector, units[f].data(), static_cast<uint32_t>(units[f].size()), &info);
            if (round) continue;
            CHECK(status == MEDIA_OK && info.index == f && info.width == kW && info.height == kH && info.picture_type == static_cast<uint32_t>(f % 60 ? MEDIA_PICTURE_TYPE_P : MEDIA_PICTURE_TYPE_I));
            if (info.flags & MEDIA_BITSTREAM_SCENE_CUT) { ++cuts; cut_at = f; }
            if (f == kCut + 1) CHECK((info.flags & MEDIA_BITSTREAM_REVISION) && info.revised_index == kCut && info.revised_scene_cut_probability >= 0.95f);
            if ((info.flags & MEDIA_BITSTREAM_QP) && info.qp == 26) ++qp_known;
        }
        allocations += allocation_probe::End();
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    const double ms = seconds * 1000 / (rounds * kPictures);
    std::printf("SDK inspector: %u cut (at %u), QP known on %u of %u, %.5f ms a picture (%.0f pictures a second), %llu allocations\n", cuts, cut_at, qp_known, kPictures, ms,
                1 / (ms / 1000), static_cast<unsigned long long>(allocations));
    CHECK(cuts == 1 && cut_at == kCut && qp_known == kPictures && allocations == 0);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 0.02);
#else
    CHECK(ms < 0.2); // Unoptimised or instrumented.
#endif
}

#if defined(_WIN32)
// ---- Platform encoders ----
constexpr unsigned kEncW = 320, kEncH = 192, kEncPictures = 110, kEncCut = 70, kEncBurst0 = 40, kEncBurst1 = 55;
void EncodedRow(unsigned f, unsigned y, unsigned width, unsigned, std::uint8_t* row) {
    const double phase = f < kEncCut ? 0.0 : 2.1;
    const unsigned slow0 = (std::min)(f, kEncBurst0), fast = f > kEncBurst0 ? (std::min)(f, kEncBurst1) - kEncBurst0 : 0, slow1 = f > kEncBurst1 ? f - kEncBurst1 : 0;
    const unsigned ox = (10 + slow0 + 9 * fast + slow1) % 260;
    const bool burst = f >= kEncBurst0 && f < kEncBurst1;
    for (unsigned x = 0; x < width; ++x) {
        double v = 120 + 45 * std::sin(x / 6.0 + phase) * std::cos(y / 5.0 - phase) + 25 * std::sin((x + 3.0 * y) / 11.0 + 2 * phase);
        if (x >= ox && x < ox + 40 && y >= 70 && y < 110) v = 225 - ((x - ox) * 5 + y * 3) % 50;
        // The burst: a churning region (water, a crowd, leaves) whose detail is new every picture: what costs an encoder bits,
        // where a rigid object's translation, however fast, is a motion vector.
        if (burst && x >= 180 && x < 300 && y >= 20 && y < 170) {
            std::uint32_t h = (x / 2) * 73856093u ^ (y / 2) * 19349663u ^ f * 83492791u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            v = 60 + (h & 127u); // Sparkle: 2 x 2 blocks of detail new every picture (no motion predicts it).
        }
        row[x] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 16.0, 235.0)));
    }
}
// The stream's answers, judged: every picture's QP known, the cut found at kEncCut (within the B pictures' reordering) and
// nowhere else, the burst's activity twice the slow motion's or more. `config`: its hvcC (null: parameter sets in band);
// `width0`, `width1`: the pictures' widths before and from the cut (0: not checked).
void Judge(const char* name, bool h264, bool b_requested, const std::vector<encoder::Encoded>& stream, const Bytes* config, unsigned width0 = 0, unsigned width1 = 0) {
    static media_bitstream_inspector_t inspector;
    CHECK(media_bitstream_inspector_init(&inspector, h264 ? MEDIA_CODEC_H264 : MEDIA_CODEC_HEVC, config ? config->data() : nullptr,
                                         config ? static_cast<uint32_t>(config->size()) : 0u, 4) == MEDIA_OK);
    unsigned widths_wrong = 0;
    media_bitstream_info_t info{}; info.struct_size = sizeof(info);
    std::vector<media_bitstream_info_t> infos; std::vector<float> final;
    double seconds = 0;
    for (const auto& unit : stream) {
        const auto start = std::chrono::steady_clock::now();
        const auto status = media_inspect_bitstream_packet(&inspector, reinterpret_cast<const uint8_t*>(unit.au.data()), static_cast<uint32_t>(unit.au.size()), &info);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (status == MEDIA_OK) {
            if (width0 && info.width != (infos.size() < kEncCut ? width0 : width1)) ++widths_wrong;
            infos.push_back(info); final.push_back(info.scene_cut_probability);
            if ((info.flags & MEDIA_BITSTREAM_REVISION) && info.revised_index < final.size()) final[info.revised_index] = info.revised_scene_cut_probability;
        }
    }
    unsigned b = 0, qp = 0, keys = 0, cuts_near = 0, cuts_far = 0; double best = 0; unsigned best_at = 0, slow_n = 0, fast_n = 0; double slow = 0, fast = 0;
    const bool dump = GetEnvironmentVariableA("MEDIA_BITSTREAM_DUMP", nullptr, 0) != 0; // Every unit's answer, for a look at an encoder.
    for (unsigned i = 0; i < infos.size(); ++i) {
        const auto& r = infos[i];
        if (dump) std::printf("  %3u %c%s %6u bytes qp %2d complexity %.3f activity %6.3f burst %5.2f cut %.2f\n", i, " IPB"[r.picture_type], r.flags & MEDIA_BITSTREAM_KEY ? "k" : " ",
                              r.bytes, r.qp, r.complexity, r.activity, r.burst, final[i]);
        b += r.picture_type == MEDIA_PICTURE_TYPE_B; qp += (r.flags & MEDIA_BITSTREAM_QP) != 0; keys += (r.flags & MEDIA_BITSTREAM_KEY) != 0;
        const bool around_cut = i + 3 >= kEncCut && i <= kEncCut + 3;
        if (final[i] >= 0.5f) { if (around_cut) ++cuts_near; else { ++cuts_far; std::printf("  unit %u: type %u cut %.2f, first look %.2f (complexity %.3f, activity %.3f, burst %.2f, qp %d)\n", i, r.picture_type, final[i], r.scene_cut_probability, r.complexity, r.activity, r.burst, r.qp); } }
        if (final[i] > best) { best = final[i]; best_at = i; }
        if (r.picture_type != MEDIA_PICTURE_TYPE_I && r.activity >= 0) {
            if (i > 5 && i + 3 < kEncBurst0) { slow += r.activity; ++slow_n; }
            if (i >= kEncBurst0 + 4 && i < kEncBurst1) { fast += r.activity; ++fast_n; }
        }
    }
    slow /= slow_n ? slow_n : 1; fast /= fast_n ? fast_n : 1;
    std::printf("%s (%s): %zu pictures, %u B, %u keys, QP known on %u; strongest cut %.2f at unit %u (%u near %u, %u elsewhere); activity %.3f slow, %.3f burst; %.5f ms a picture\n",
                name, b_requested ? "B pictures asked" : "no B", infos.size(), b, keys, qp, best, best_at, cuts_near, kEncCut, cuts_far, slow, fast,
                seconds * 1000 / (infos.size() ? infos.size() : 1));
    CHECK(infos.size() == kEncPictures && keys >= 1);
    CHECK(cuts_near >= 1 && cuts_far == 0 && best_at + 3 >= kEncCut && best_at <= kEncCut + 3);
    CHECK(fast >= 2 * slow && qp == infos.size() && widths_wrong == 0);
}
void TestPlatform(bool h264) {
    encoder::Options o; o.width = kEncW; o.height = kEncH; o.frames = kEncPictures; o.fps = 30; o.gop = 30; o.b_frames = 2; o.bitrate = 1500000;
    o.h264 = h264; o.software = h264;
    std::vector<encoder::Encoded> stream; bool b_requested = false;
    const auto fixture = encoder::Encode(o, &EncodedRow, stream, b_requested);
    if (fixture != encoder::Fixture::Ready) { std::printf("%s encoder: unavailable here (skipped)\n", h264 ? "H.264" : "HEVC"); CHECK(fixture != encoder::Fixture::Failed || !h264); return; }
    Judge(h264 ? "H.264" : "HEVC", h264, b_requested, stream, nullptr);
}
// ---- Two sequences at once (2026-11-03): the HEVC encoder's two encodes, the second's ids renumbered ----
Bytes Unescape(std::span<const std::byte> payload) {
    Bytes out; unsigned zeros = 0;
    for (auto byte : payload) {
        const auto v = std::to_integer<std::uint8_t>(byte);
        if (zeros >= 2 && v == 3) { zeros = 0; continue; }
        out.push_back(v); zeros = v == 0 ? zeros + 1 : 0;
    }
    return out;
}
struct BitIn {
    const Bytes& b; std::size_t pos = 0;
    unsigned Bit() { if (pos >= b.size() * 8) return 0; const unsigned v = (b[pos / 8] >> (7 - pos % 8)) & 1u; ++pos; return v; }
    unsigned Value(unsigned n) { unsigned v = 0; while (n--) v = (v << 1) | Bit(); return v; }
    unsigned Ue() { unsigned n = 0; while (!Bit() && n < 31) ++n; return (1u << n) - 1 + Value(n); }
    bool More() const { return pos < b.size() * 8; }
};
// An HEVC SPS, PPS or slice with its ids made `to` (the SPS's id; the PPS's own and its SPS's; the slice's PPS), every other
// bit as it was (a slice's data after its header is no longer aligned: only its header is read here).
Bytes Renumber(std::span<const std::byte> nal, unsigned to) {
    const unsigned type = (std::to_integer<unsigned>(nal[0]) >> 1) & 0x3Fu;
    const auto rbsp = Unescape(nal.subspan(2));
    BitIn in{rbsp}; Bits out;
    const auto copy = [&](unsigned n) { for (unsigned i = 0; i < n; ++i) out.Bit(in.Bit()); };
    if (type == 33) {
        copy(4); const unsigned layers = in.Value(3); out.Value(layers, 3); copy(1); copy(96);
        bool profile[8]{}, level[8]{};
        for (unsigned i = 0; i < layers; ++i) { profile[i] = in.Bit() != 0; level[i] = in.Bit() != 0; out.Bit(profile[i]); out.Bit(level[i]); }
        if (layers) copy(2 * (8 - layers));
        for (unsigned i = 0; i < layers; ++i) { if (profile[i]) copy(88); if (level[i]) copy(8); }
        (void)in.Ue(); out.Ue(to);
    } else if (type == 34) { (void)in.Ue(); out.Ue(to); (void)in.Ue(); out.Ue(to); }
    else { copy(1); if (type >= 16 && type <= 23) copy(1); (void)in.Ue(); out.Ue(to); }
    while (in.More()) out.Bit(in.Bit());
    Bytes result{std::to_integer<std::uint8_t>(nal[0]), std::to_integer<std::uint8_t>(nal[1])};
    unsigned zeros = 0;
    for (auto v : out.b) { if (zeros == 2 && v <= 3) { result.push_back(3); zeros = 0; } result.push_back(v); zeros = v == 0 ? zeros + 1 : 0; }
    return result;
}
void TestPlatformSequences() {
    // Pictures 0-69 at 320 x 192 (SPS 0, PPS 0), 70-109 at 256 x 160 (a second encode: the cut is its first picture), its SPS
    // and PPS renumbered 1. All four parameter sets in the hvcC record, SPS 1 last (the newest: before 2026-11-03 only its
    // sets were kept, and SPS 0's pictures had no QP); none in band.
    encoder::Options o; o.width = kEncW; o.height = kEncH; o.frames = kEncCut; o.fps = 30; o.gop = 30; o.b_frames = 2; o.bitrate = 1500000;
    std::vector<encoder::Encoded> first, second; bool b_requested = false, b_second = false;
    if (encoder::Encode(o, &EncodedRow, first, b_requested) != encoder::Fixture::Ready) { std::puts("HEVC two sequences: encoder unavailable here (skipped)"); return; }
    o.width = 256; o.height = 160; o.frames = kEncPictures - kEncCut;
    const auto later = [](unsigned f, unsigned y, unsigned width, unsigned height, std::uint8_t* row) { EncodedRow(f + kEncCut, y, width, height, row); };
    if (encoder::Encode(o, later, second, b_second) != encoder::Fixture::Ready) { std::puts("FAIL second encode"); ++failures; return; }
    Bytes sps[2], pps[2];
    std::vector<encoder::Encoded> stream;
    for (unsigned part = 0; part < 2; ++part)
        for (const auto& unit : part ? second : first) {
            encoder::Encoded out; out.pts = unit.pts;
            encoder::ForEachNal(unit.au, [&](std::span<const std::byte> nal) {
                const unsigned type = (std::to_integer<unsigned>(nal[0]) >> 1) & 0x3Fu;
                if (type >= 35 || type == 32) return; // VPS, SEI, delimiters.
                const Bytes body = part ? Renumber(nal, 1) : Bytes(reinterpret_cast<const std::uint8_t*>(nal.data()), reinterpret_cast<const std::uint8_t*>(nal.data()) + nal.size());
                if (type == 33 || type == 34) { auto& kept = (type == 33 ? sps : pps)[part]; if (kept.empty()) kept = body; return; }
                const auto n = static_cast<std::uint32_t>(body.size());
                for (int i = 3; i >= 0; --i) out.au.push_back(static_cast<std::byte>(n >> (8 * i)));
                for (auto v : body) out.au.push_back(static_cast<std::byte>(v));
            });
            stream.push_back(std::move(out));
        }
    CHECK(!sps[0].empty() && !sps[1].empty() && !pps[0].empty() && !pps[1].empty());
    Bytes hvcc(23, 0); hvcc[0] = 1; hvcc[21] = 0xFF; hvcc[22] = 2;
    for (unsigned type : {33u, 34u}) {
        hvcc.push_back(static_cast<std::uint8_t>(type)); hvcc.push_back(0); hvcc.push_back(2);
        for (unsigned part = 0; part < 2; ++part) {
            const auto& set = (type == 33 ? sps : pps)[part];
            hvcc.push_back(static_cast<std::uint8_t>(set.size() >> 8)); hvcc.push_back(static_cast<std::uint8_t>(set.size())); hvcc.insert(hvcc.end(), set.begin(), set.end());
        }
    }
    Judge("HEVC two sequences", false, b_requested, stream, &hvcc, kEncW, 256);
}
#endif
}

int main() {
    std::puts("Bitstream perception tests"); // stdout's buffer before the first probe (MSVC Debug counts the CRT's).
    TestHeaders();
    TestSequences();
    TestHierarchy();
    TestReplenishment();
    TestSdk();
#if defined(_WIN32)
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
        TestPlatform(true);
        TestPlatform(false);
        TestPlatformSequences();
        MFShutdown();
    } else { std::puts("FAIL Media Foundation"); ++failures; }
    CoUninitialize();
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "bitstream perception tests passed", failures);
    return failures ? 1 : 0;
}
