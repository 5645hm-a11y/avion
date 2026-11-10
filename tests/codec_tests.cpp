#include "media/codec/H264SyntaxParser.hpp"
#include "allocation_probe.hpp"
#include <array>
#include <cstdio>
#include <cstring>
using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
struct Bytes {
    std::array<std::byte, 1024> data{}; unsigned size = 0;
    void Add(unsigned v) { data[size++] = static_cast<std::byte>(v); }
    std::span<const std::byte> View() const { return {data.data(), size}; }
};
struct Bits {
    Bytes bytes; unsigned count = 0;
    void Bit(unsigned v) {
        if (!(count % 8)) bytes.Add(0);
        if (v) bytes.data[count / 8] |= static_cast<std::byte>(1u << (7 - count % 8));
        ++count;
    }
    void Value(unsigned v, unsigned n) { while (n) Bit((v >> --n) & 1); }
    void Ue(unsigned v) { unsigned n = 0; for (auto t = v + 1; t > 1; t >>= 1) ++n; for (unsigned i = 0; i < n; ++i) Bit(0); Value(v + 1, n + 1); }
    void Se(int v) { Ue(static_cast<unsigned>(v <= 0 ? -2 * v : 2 * v - 1)); }
    Bytes Nal(unsigned header) {
        Bit(1); while (count % 8) Bit(0);
        Bytes out; out.Add(header); unsigned zeros = 0;
        for (auto b : bytes.View()) { const auto v = std::to_integer<unsigned>(b); if (zeros == 2 && v <= 3) { out.Add(3); zeros = 0; } out.Add(v); zeros = v == 0 ? zeros + 1 : 0; }
        return out;
    }
};
// Stream parameters shared by the SPS, PPS and slice writers.
struct Config {
    unsigned profile = 66, level = 30, frame_bits = 4, poc = 0, poc_bits = 4, refs = 2;
    bool crop = false, bottom_poc = false, weighted = false, cabac = false; unsigned weighted_bi = 0;
    int reorder = -1; // VUI max_num_reorder_frames; -1: no VUI.
    // VUI sample aspect ratio and colour description (each -1: absent). A VUI is written when any of these is set.
    int sar_idc = -1; unsigned sar_width = 0, sar_height = 0;
    int full_range = -1; int primaries = -1; unsigned transfer = 2, matrix = 2;
    // Type 1.
    bool always_zero = true; int non_ref = 0, top_bottom = 0; unsigned cycle = 0; int offsets[4]{};
};
Bytes Sps(const Config& c) {
    Bits b; b.Value(c.profile, 8); b.Value(0, 8); b.Value(c.level, 8); b.Ue(0);
    if (c.profile == 100) { b.Ue(1); b.Ue(0); b.Ue(0); b.Bit(0); b.Bit(0); }
    b.Ue(c.frame_bits - 4); b.Ue(c.poc);
    if (c.poc == 0) b.Ue(c.poc_bits - 4);
    if (c.poc == 1) { b.Bit(c.always_zero); b.Se(c.non_ref); b.Se(c.top_bottom); b.Ue(c.cycle); for (unsigned i = 0; i < c.cycle; ++i) b.Se(c.offsets[i]); }
    b.Ue(c.refs); b.Bit(0); b.Ue(3); b.Ue(3); b.Bit(1); b.Bit(1); b.Bit(c.crop);
    if (c.crop) { b.Ue(1); b.Ue(2); b.Ue(3); b.Ue(4); }
    const bool vui = c.reorder >= 0 || c.sar_idc >= 0 || c.full_range >= 0;
    b.Bit(vui);
    if (vui) {
        b.Bit(c.sar_idc >= 0);
        if (c.sar_idc >= 0) { b.Value(static_cast<unsigned>(c.sar_idc), 8); if (c.sar_idc == 255) { b.Value(c.sar_width, 16); b.Value(c.sar_height, 16); } }
        b.Bit(0); // Overscan.
        b.Bit(c.full_range >= 0);
        if (c.full_range >= 0) {
            b.Value(5, 3); b.Bit(c.full_range != 0); b.Bit(c.primaries >= 0);
            if (c.primaries >= 0) { b.Value(static_cast<unsigned>(c.primaries), 8); b.Value(c.transfer, 8); b.Value(c.matrix, 8); }
        }
        for (unsigned i = 0; i < 5; ++i) b.Bit(0); // Chroma location, timing, NAL HRD, VCL HRD, pic_struct.
        b.Bit(c.reorder >= 0);
        if (c.reorder >= 0) { b.Bit(1); b.Ue(2); b.Ue(1); b.Ue(16); b.Ue(16); b.Ue(static_cast<unsigned>(c.reorder)); b.Ue(c.refs + 1); }
    }
    return b.Nal(0x67);
}
Bytes Pps(const Config& c) {
    Bits b; b.Ue(0); b.Ue(0); b.Bit(c.cabac); b.Bit(c.bottom_poc); b.Ue(0); b.Ue(0); b.Ue(0); b.Bit(c.weighted); b.Value(c.weighted_bi, 2);
    b.Se(0); b.Se(0); b.Se(0); b.Bit(1); b.Bit(0); b.Bit(0); return b.Nal(0x68);
}
struct Op { unsigned op, value, index; };
struct SliceSpec {
    unsigned type = 0; // 0 P, 1 B, 2 I.
    bool idr = false, reference = true; unsigned frame = 0, lsb = 0; int bottom = 0, delta0 = 0, delta1 = 0;
    int l0 = -1, l1 = -1;                     // num_ref_idx_active override (-1: none).
    Op modification[2][4]{}; unsigned modifications[2]{};
    bool long_term = false;                   // IDR long_term_reference_flag.
    bool adaptive = false; Op mmco[8]{}; unsigned mmco_count = 0;
    unsigned first_mb = 0;
};
Bytes SliceNal(const Config& c, const SliceSpec& s) {
    Bits b; b.Ue(s.first_mb); b.Ue(s.type); b.Ue(0); b.Value(s.frame, c.frame_bits); if (s.idr) b.Ue(0);
    if (c.poc == 0) { b.Value(s.lsb, c.poc_bits); if (c.bottom_poc) b.Se(s.bottom); }
    if (c.poc == 1 && !c.always_zero) { b.Se(s.delta0); if (c.bottom_poc) b.Se(s.delta1); }
    const unsigned lists = s.type == 2 ? 0 : s.type == 1 ? 2 : 1;
    if (s.type == 1) b.Bit(1); // direct_spatial_mv_pred_flag
    if (lists) { b.Bit(s.l0 >= 0); if (s.l0 >= 0) { b.Ue(static_cast<unsigned>(s.l0)); if (s.type == 1) b.Ue(static_cast<unsigned>(s.l1 < 0 ? 0 : s.l1)); } }
    for (unsigned list = 0; list < lists; ++list) {
        b.Bit(s.modifications[list] != 0);
        if (!s.modifications[list]) continue;
        for (unsigned i = 0; i < s.modifications[list]; ++i) { b.Ue(s.modification[list][i].op); b.Ue(s.modification[list][i].value); }
        b.Ue(3);
    }
    if ((c.weighted && s.type == 0) || (c.weighted_bi == 1 && s.type == 1)) {
        b.Ue(5); b.Ue(3);
        const unsigned active[2]{s.l0 >= 0 ? static_cast<unsigned>(s.l0) : 0u, s.l1 >= 0 ? static_cast<unsigned>(s.l1) : 0u};
        for (unsigned list = 0; list < lists; ++list) for (unsigned i = 0; i <= active[list]; ++i) {
            b.Bit(1); b.Se(-7); b.Se(3); b.Bit(i & 1); if (i & 1) { b.Se(1); b.Se(-1); b.Se(2); b.Se(-2); }
        }
    }
    if (s.reference) {
        if (s.idr) { b.Bit(0); b.Bit(s.long_term); }
        else {
            b.Bit(s.adaptive);
            if (s.adaptive) {
                for (unsigned i = 0; i < s.mmco_count; ++i) {
                    const auto& m = s.mmco[i]; b.Ue(m.op);
                    if (m.op == 1 || m.op == 2 || m.op == 3 || m.op == 4) b.Ue(m.value);
                    if (m.op == 3 || m.op == 6) b.Ue(m.index);
                }
                b.Ue(0);
            }
        }
    }
    if (c.cabac && s.type != 2) b.Ue(1);
    b.Se(0); b.Ue(1); // Header-only fixture: entropy data belongs to the hardware decoder.
    return b.Nal((s.idr ? 5u : 1u) | (s.reference ? 0x60u : 0u));
}
void Append(Bytes& au, const Bytes& nal) {
    au.Add(nal.size >> 24); au.Add(nal.size >> 16); au.Add(nal.size >> 8); au.Add(nal.size);
    for (auto v : nal.View()) au.Add(std::to_integer<unsigned>(v));
}
Bytes Unit(const Config& c, const SliceSpec& s) { Bytes au; Append(au, SliceNal(c, s)); return au; }
// Legacy IPPP helper: P slices (or `type`), POC lsb = 2 * frame, optional (empty) adaptive marking.
Bytes Slice(unsigned frame, unsigned poc_type = 0, unsigned type = 0) {
    Config c; c.poc = poc_type; SliceSpec s; s.idr = frame == 0; s.type = s.idr ? 2 : type; s.frame = frame % 16; s.lsb = (frame * 2) % 16;
    return Unit(c, s);
}
struct Harness {
    core::LinearMemoryArena arena; codec::H264SyntaxParser parser; std::uint64_t allocations = 0;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0, next = 0;
    explicit Harness(const Config& c) {
        CHECK(arena.Initialize(1 << 20)); CHECK(parser.Initialize(arena)); CHECK(arena.Seal());
        allocations = arena.Statistics().allocation_count;
        CHECK(parser.ParameterSet(Sps(c).View()) == codec::SyntaxStatus::Ready);
        CHECK(parser.ParameterSet(Pps(c).View()) == codec::SyntaxStatus::Ready);
    }
    // Parses and commits one picture on surface `next++`; returns the parsed picture's copy.
    codec::H264Picture Decode(const Config& c, const SliceSpec& s, codec::SyntaxStatus expected = codec::SyntaxStatus::Ready) {
        const auto status = parser.Parse(Unit(c, s).View(), 4);
        CHECK(status == expected);
        codec::H264Picture copy{};
        if (status != codec::SyntaxStatus::Ready) return copy;
        copy = *parser.Picture();
        CHECK(parser.Commit({next++, 1}, evicted, count));
        return copy;
    }
    // DPB as "id" (short-term) or "id:Lidx" (long-term), in DPB order.
    bool Dpb(std::initializer_list<int> ids) const {
        const auto refs = parser.References();
        if (refs.size() != ids.size()) return false;
        unsigned i = 0;
        for (const int id : ids) {
            const bool long_term = id >= 100;
            const auto surface = static_cast<unsigned>(long_term ? id / 100 - 1 : id);
            if (refs[i].surface.id != surface || refs[i].long_term != long_term || (long_term && refs[i].frame_num != static_cast<unsigned>(id % 100))) return false;
            ++i;
        }
        return true;
    }
    bool Evicted(std::initializer_list<unsigned> ids) const {
        if (count != ids.size()) return false;
        unsigned i = 0; for (const auto id : ids) if (evicted[i++].id != id) return false;
        return true;
    }
};
int Lt(unsigned surface, unsigned index) { return static_cast<int>((surface + 1) * 100 + index); } // Long-term DPB entry.

void Reader() {
    const std::byte escaped[]{std::byte{0}, std::byte{0}, std::byte{3}, std::byte{1}, std::byte{0x80}};
    codec::RbspReader r(escaped); CHECK(r.Bits(24) == 1); CHECK(r.Position() == 24); CHECK(r.Finish()); CHECK(r.Position() == 32);
    for (unsigned n = 2; n <= 3; ++n) { codec::RbspReader cut({escaped, n}); (void)cut.Bits(24); CHECK(!cut.Good()); }
    const std::byte bad[]{std::byte{0}, std::byte{0}, std::byte{3}, std::byte{4}};
    codec::RbspReader invalid(bad); (void)invalid.Bits(24); CHECK(!invalid.Good());
    Bits b; b.Ue(0); b.Ue(31); b.Ue(65535); b.Se(-12); b.Se(25); auto nal = b.Nal(0x67);
    codec::RbspReader values(nal.View().subspan(1)); CHECK(values.Ue() == 0); CHECK(values.Ue() == 31); CHECK(values.Ue() == 65535);
    CHECK(values.Se() == -12); CHECK(values.Se() == 25); CHECK(!values.MoreData()); CHECK(values.Finish());
    codec::RbspReader large(escaped); (void)large.Bits(33); CHECK(!large.Good());
    const std::byte zeros[16]{}; codec::RbspReader overflow(zeros); (void)overflow.Ue(); CHECK(!overflow.Good());
}
// IPPP with the sliding window (POC types 0 and 2), a cancelled parse and an IDR flush.
void Sequence(unsigned poc) {
    Config c; c.poc = poc; c.crop = true;
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20)); codec::H264SyntaxParser parser;
    CHECK(parser.Initialize(arena)); CHECK(arena.Seal()); const auto allocations = arena.Statistics().allocation_count;
    auto sps = Sps(c), pps = Pps(c);
    allocation_probe::Begin();
    CHECK(parser.ParameterSet(sps.View()) == codec::SyntaxStatus::Ready); CHECK(parser.ParameterSet(pps.View()) == codec::SyntaxStatus::Ready);
    const auto* s = parser.FirstSps(); CHECK(s && s->width == 64 && s->left == 2 && s->right == 60 && s->top == 6 && s->bottom == 56);
    CHECK(s && s->reorder == 0 && !s->reorder_signalled); // Baseline without VUI: decode order.
    auto missing = Slice(1, poc); CHECK(parser.Parse(missing.View(), 4) == codec::SyntaxStatus::NeedIdr);
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    for (unsigned i = 0; i < 40; ++i) {
        auto au = Slice(i, poc); CHECK(parser.Parse(au.View(), 4) == codec::SyntaxStatus::Ready);
        const auto* p = parser.Picture(); CHECK(p != nullptr);
        if (p) { CHECK(p->top_poc == static_cast<int>(i * 2)); CHECK(p->frame_num == i % 16); CHECK(p->reference_count == (i < 2 ? i : 2)); CHECK(p->slices[0].offset == 4); }
        CHECK(parser.Parse(au.View(), 4) == codec::SyntaxStatus::Busy);
        CHECK(parser.Commit({i, 1}, evicted, count)); CHECK(count == (i < 2 ? 0u : 1u));
        if (count) CHECK(evicted[0].id == i - 2);
    }
    // A frame_num gap is not admitted (gaps_in_frame_num_allowed_flag is unsupported).
    auto gap = Slice(42, poc); CHECK(parser.Parse(gap.View(), 4) == codec::SyntaxStatus::Unsupported);
    auto idr = Slice(0, poc); CHECK(parser.Parse(idr.View(), 4) == codec::SyntaxStatus::Ready); parser.Cancel(); CHECK(!parser.Picture());
    CHECK(parser.Parse(idr.View(), 4) == codec::SyntaxStatus::Ready); CHECK(parser.Commit({99, 1}, evicted, count)); CHECK(count == 2);
    CHECK(evicted[0].id == 38 && evicted[1].id == 39);
    // In-band repeats of the active sets are accepted; a changed set needs a new decoder session.
    CHECK(parser.ParameterSet(sps.View()) == codec::SyntaxStatus::Ready && parser.ParameterSet(pps.View()) == codec::SyntaxStatus::Ready);
    Config changed = c; changed.refs = 3;
    CHECK(parser.ParameterSet(Sps(changed).View()) == codec::SyntaxStatus::Unsupported);
    changed = c; changed.weighted = true;
    CHECK(parser.ParameterSet(Pps(changed).View()) == codec::SyntaxStatus::Unsupported);
    Bytes repeated; Append(repeated, sps); Append(repeated, pps); Append(repeated, SliceNal(c, SliceSpec{2, false, true, 1, 2}));
    CHECK(parser.Parse(repeated.View(), 4) == codec::SyntaxStatus::Ready); parser.Cancel();
    CHECK(allocation_probe::End() == 0); CHECK(arena.Statistics().allocation_count == allocations);
}
// VUI: sample aspect ratio (Table E-1 and the explicit pair) and the whole colour description.
void Description() {
    const auto parse = [](const Config& c, codec::H264Sps& out) {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20)); codec::H264SyntaxParser parser;
        CHECK(parser.Initialize(arena)); CHECK(arena.Seal());
        const auto status = parser.ParameterSet(Sps(c).View());
        if (status == codec::SyntaxStatus::Ready && parser.FirstSps()) out = *parser.FirstSps();
        return status;
    };
    codec::H264Sps s;
    Config plain; CHECK(parse(plain, s) == codec::SyntaxStatus::Ready);
    CHECK(s.sar_width == 0 && s.sar_height == 0 && !s.signal_described && !s.colour_described && s.primaries == 2 && s.transfer == 2 && s.matrix == 2 && !s.full_range);
    struct Aspect { int idc; unsigned w, h, expect_w, expect_h; };
    for (const auto a : {Aspect{1, 0, 0, 1, 1}, Aspect{2, 0, 0, 12, 11}, Aspect{3, 0, 0, 10, 11}, Aspect{4, 0, 0, 16, 11}, Aspect{5, 0, 0, 40, 33},
                         Aspect{13, 0, 0, 160, 99}, Aspect{14, 0, 0, 4, 3}, Aspect{15, 0, 0, 3, 2}, Aspect{16, 0, 0, 2, 1}, Aspect{255, 4, 3, 4, 3},
                         Aspect{255, 65535, 65534, 65535, 65534}, Aspect{255, 0, 3, 0, 0}, Aspect{255, 4, 0, 0, 0}, Aspect{0, 0, 0, 0, 0}, Aspect{17, 0, 0, 0, 0}}) {
        Config c; c.sar_idc = a.idc; c.sar_width = a.w; c.sar_height = a.h; c.reorder = 0;
        CHECK(parse(c, s) == codec::SyntaxStatus::Ready);
        CHECK(s.sar_width == a.expect_w && s.sar_height == a.expect_h && s.reorder_signalled); // What follows the aspect still parses.
    }
    // Signal type without code points: the range flag alone.
    Config range; range.full_range = 1; CHECK(parse(range, s) == codec::SyntaxStatus::Ready);
    CHECK(s.signal_described && !s.colour_described && s.full_range && s.primaries == 2 && s.transfer == 2 && s.matrix == 2);
    // All three code points, which used to be read as the matrix only.
    Config hd; hd.full_range = 0; hd.primaries = 1; hd.transfer = 13; hd.matrix = 1; hd.sar_idc = 14;
    CHECK(parse(hd, s) == codec::SyntaxStatus::Ready);
    CHECK(s.signal_described && s.colour_described && !s.full_range && s.primaries == 1 && s.transfer == 13 && s.matrix == 1 && s.sar_width == 4 && s.sar_height == 3);
    Config wide; wide.full_range = 1; wide.primaries = 9; wide.transfer = 16; wide.matrix = 9; wide.reorder = 1;
    CHECK(parse(wide, s) == codec::SyntaxStatus::Ready);
    CHECK(s.full_range && s.primaries == 9 && s.transfer == 16 && s.matrix == 9 && s.reorder == 1);
    // Once decoding has started, an in-band repeat that describes the pictures differently is not a changed decoder
    // configuration: it is accepted, and the description the session started with stands. A real change still is one.
    Config base; base.sar_idc = 14; base.full_range = 0; base.primaries = 1; base.transfer = 1; base.matrix = 1;
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20)); codec::H264SyntaxParser parser;
    CHECK(parser.Initialize(arena)); CHECK(arena.Seal());
    CHECK(parser.ParameterSet(Sps(base).View()) == codec::SyntaxStatus::Ready && parser.ParameterSet(Pps(base).View()) == codec::SyntaxStatus::Ready);
    Bytes idr; Append(idr, SliceNal(base, SliceSpec{2, true}));
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    CHECK(parser.Parse(idr.View(), 4) == codec::SyntaxStatus::Ready && parser.Commit({1, 1}, evicted, count));
    Config other = base; other.sar_idc = 15; CHECK(parser.ParameterSet(Sps(other).View()) == codec::SyntaxStatus::Ready);
    other = base; other.transfer = 16; other.primaries = 9; other.matrix = 9; other.full_range = 1;
    CHECK(parser.ParameterSet(Sps(other).View()) == codec::SyntaxStatus::Ready);
    other = base; other.sar_idc = -1; other.full_range = -1; CHECK(parser.ParameterSet(Sps(other).View()) == codec::SyntaxStatus::Ready);
    const auto* active = parser.FirstSps();
    CHECK(active && active->sar_width == 4 && active->sar_height == 3 && active->transfer == 1 && active->matrix == 1 && !active->full_range);
    other = base; other.refs = 3; CHECK(parser.ParameterSet(Sps(other).View()) == codec::SyntaxStatus::Unsupported);
}
// IBBP (non-reference B) and a B-pyramid (reference B): frame_num repeats, POC order, list syntax, weights.
void BFrames() {
    Config c; c.profile = 100; c.level = 30; c.poc_bits = 8; c.refs = 3; c.cabac = true; c.weighted = true; c.weighted_bi = 1; c.reorder = 2;
    Harness h(c);
    const auto* sps = h.parser.FirstSps(); CHECK(sps && sps->reorder == 2 && sps->reorder_signalled);
    allocation_probe::Begin();
    SliceSpec s; s.type = 2; s.idr = true;
    auto p = h.Decode(c, s);                                     // I0   POC 0
    CHECK(p.idr && p.intra && !p.bidirectional && p.top_poc == 0);
    s = {}; s.type = 0; s.frame = 1; s.lsb = 6; s.l0 = 0;
    p = h.Decode(c, s);                                          // P1   POC 6 (weighted P table)
    CHECK(!p.intra && p.reference_count == 1 && p.top_poc == 6);
    s = {}; s.type = 1; s.frame = 2; s.lsb = 2; s.reference = false; s.l0 = 1; s.l1 = 0;
    s.modification[1][0] = {1, 0, 0}; s.modifications[1] = 1; // L1: abs_diff_pic_num_minus1 0, added.
    p = h.Decode(c, s);                                          // b    POC 2, non-reference
    CHECK(p.bidirectional && !p.reference && p.top_poc == 2 && p.reference_count == 2);
    CHECK(h.Dpb({0, 1}) && h.count == 0);                        // Non-reference pictures never enter the DPB.
    s.lsb = 4;
    p = h.Decode(c, s);                                          // b    POC 4, same frame_num (PrevRefFrameNum + 1)
    CHECK(p.top_poc == 4 && p.frame_num == 2);
    s = {}; s.type = 0; s.frame = 2; s.lsb = 12;
    s.modification[0][0] = {0, 1, 0}; s.modification[0][1] = {1, 0, 0}; s.modifications[0] = 2; s.l0 = 1;
    p = h.Decode(c, s);                                          // P2   POC 12, reference, frame_num 2 again
    CHECK(p.reference && p.frame_num == 2 && p.top_poc == 12 && h.Dpb({0, 1, 4}));
    // Pyramid: a reference B (frame_num 3) between P1 and P2, then its non-reference children.
    s = {}; s.type = 1; s.frame = 3; s.lsb = 9; s.l0 = 1; s.l1 = 1;
    p = h.Decode(c, s);                                          // B    POC 9, reference: sliding window evicts I0
    CHECK(p.bidirectional && p.reference && p.reference_count == 3 && h.Evicted({0}) && h.Dpb({1, 4, 5}));
    s = {}; s.type = 1; s.frame = 4; s.lsb = 8; s.reference = false;
    p = h.Decode(c, s); CHECK(p.top_poc == 8 && p.reference_count == 3);
    s.lsb = 10; p = h.Decode(c, s); CHECK(p.top_poc == 10);
    // A picture of an I slice and a B slice: not intra, bidirectional; picture-level syntax must repeat.
    Bytes mixed; SliceSpec first; first.type = 2; first.frame = 4; first.lsb = 14; first.reference = false;
    SliceSpec second = first; second.type = 1; second.first_mb = 8;
    Append(mixed, SliceNal(c, first)); Append(mixed, SliceNal(c, second));
    CHECK(h.parser.Parse(mixed.View(), 4) == codec::SyntaxStatus::Ready);
    if (const auto* m = h.parser.Picture()) CHECK(!m->intra && m->bidirectional && m->slice_count == 2);
    h.parser.Cancel();
    Bytes inconsistent; second.lsb = 15; Append(inconsistent, SliceNal(c, first)); Append(inconsistent, SliceNal(c, second));
    CHECK(h.parser.Parse(inconsistent.View(), 4) == codec::SyntaxStatus::Unsupported); h.parser.Cancel();
    // Invalid list syntax: more modifications than active entries, and an unknown operation.
    SliceSpec bad; bad.type = 0; bad.frame = 4; bad.lsb = 16; bad.l0 = 0;
    bad.modification[0][0] = {0, 0, 0}; bad.modification[0][1] = {0, 0, 0}; bad.modifications[0] = 2;
    CHECK(h.parser.Parse(Unit(c, bad).View(), 4) == codec::SyntaxStatus::Invalid);
    bad.modifications[0] = 1; bad.modification[0][0] = {4, 0, 0};
    CHECK(h.parser.Parse(Unit(c, bad).View(), 4) == codec::SyntaxStatus::Invalid);
    CHECK(allocation_probe::End() == 0 && h.arena.Statistics().allocation_count == h.allocations);
    // Reorder inference without a VUI: Main at level 3.0 for 64x64 is capped at 16 frames; constraint-free High too.
    Config main; main.profile = 77; Harness m(main);
    CHECK(m.parser.FirstSps() && m.parser.FirstSps()->reorder == 16 && !m.parser.FirstSps()->reorder_signalled);
}
// Decoded reference picture marking: sliding window across frame_num wrap, MMCO 1-6, long-term references.
void Marking() {
    Config c; c.refs = 4; c.poc_bits = 8; Harness h(c);
    allocation_probe::Begin();
    auto picture = [&](unsigned frame, std::initializer_list<Op> ops = {}, bool reference = true) {
        SliceSpec s; s.type = frame == 0 && !ops.size() && reference ? 2 : 0; s.idr = false; s.frame = frame % 16;
        s.lsb = (2 * h.next) % 256; s.reference = reference;
        s.adaptive = ops.size() != 0; for (const auto& op : ops) s.mmco[s.mmco_count++] = op;
        return h.Decode(c, s);
    };
    SliceSpec idr; idr.type = 2; idr.idr = true; h.Decode(c, idr);   // 0
    picture(1); picture(2); picture(3);                              // 1 2 3
    CHECK(h.Dpb({0, 1, 2, 3}));
    picture(4); CHECK(h.Evicted({0}) && h.Dpb({1, 2, 3, 4}));       // Sliding window.
    picture(5, {{1, 2, 0}}); CHECK(h.Evicted({2}) && h.Dpb({1, 3, 4, 5}));   // MMCO 1: PicNum 5 - 3 = 2.
    // MMCO 4 (MaxLongTermFrameIdx 1), MMCO 3 (PicNum 3 -> long-term 1), MMCO 1 (PicNum 1).
    picture(6, {{4, 2, 0}, {3, 2, 1}, {1, 4, 0}}); CHECK(h.Evicted({1}) && h.Dpb({Lt(3, 1), 4, 5, 6}));
    // MMCO 6 (current -> long-term 0) and MMCO 1 (PicNum 4): no sliding window for a long-term current picture.
    const auto seven = picture(7, {{6, 0, 0}, {1, 2, 0}});
    CHECK(seven.reference_count == 4 && seven.references[0].long_term && seven.references[0].frame_num == 1);
    CHECK(h.Evicted({4}) && h.Dpb({Lt(3, 1), 5, 6, Lt(7, 0)}));
    picture(8, {{2, 1, 0}}); CHECK(h.Evicted({3}) && h.Dpb({5, 6, Lt(7, 0), 8}));       // MMCO 2: LongTermPicNum 1.
    picture(9, {{1, 0, 0}, {6, 0, 0}}); CHECK(h.Evicted({8, 7}) && h.Dpb({5, 6, Lt(9, 0)})); // MMCO 1 (PicNum 8); MMCO 6 replaces index 0.
    picture(10); CHECK(h.count == 0 && h.Dpb({5, 6, Lt(9, 0), 10}));
    picture(11); CHECK(h.Evicted({5}) && h.Dpb({6, Lt(9, 0), 10, 11}));  // Long-term frames never slide out.
    for (unsigned f = 12; f < 16; ++f) picture(f);                       // 12..15
    CHECK(h.Dpb({Lt(9, 0), 13, 14, 15}));
    // frame_num wraps: FrameNumWrap of 13..15 is negative, so they still slide out before 0 and 1.
    picture(16); CHECK(h.Evicted({13}) && h.Dpb({Lt(9, 0), 14, 15, 16}));
    picture(17); CHECK(h.Evicted({14}) && h.Dpb({Lt(9, 0), 15, 16, 17}));
    // MMCO 1 across the wrap: current frame_num 2, PicNum -1 is frame_num 15 (surface 15).
    picture(18, {{1, 2, 0}}); CHECK(h.Evicted({15}) && h.Dpb({Lt(9, 0), 16, 17, 18}));
    picture(19, {{4, 0, 0}}); CHECK(h.Evicted({9}) && h.Dpb({16, 17, 18, 19}));  // MMCO 4: no long-term indices.
    // MMCO 6 beyond MaxLongTermFrameIdx is ignored, so the picture is short-term and the repair window makes room.
    picture(20, {{6, 0, 0}}); CHECK(h.Evicted({16}) && h.Dpb({17, 18, 19, 20}));
    // MMCO 5: everything leaves, the picture becomes frame_num 0 with POC 0; the next one follows as frame_num 1.
    SliceSpec reset; reset.type = 0; reset.frame = 5; reset.lsb = 60; reset.adaptive = true; reset.mmco[0] = {5, 0, 0}; reset.mmco_count = 1;
    const auto r = h.Decode(c, reset); CHECK(r.top_poc > 0 && h.count == 4);
    CHECK(h.parser.References().size() == 1 && h.parser.References()[0].frame_num == 0 && h.parser.References()[0].top == 0);
    SliceSpec after; after.type = 0; after.frame = 6; after.lsb = 4;
    CHECK(h.parser.Parse(Unit(c, after).View(), 4) == codec::SyntaxStatus::Unsupported); // PrevRefFrameNum is 0 now.
    after.frame = 1; const auto next = h.Decode(c, after); CHECK(next.top_poc == 4 && next.reference_count == 1);
    // IDR with long_term_reference_flag: long-term index 0, MaxLongTermFrameIdx 0.
    SliceSpec lt; lt.type = 2; lt.idr = true; lt.long_term = true; h.Decode(c, lt);
    CHECK(h.count == 2 && h.parser.References().size() == 1 && h.parser.References()[0].long_term);
    picture(1, {{6, 0, 1}}); CHECK(h.parser.References().size() == 2 && !h.parser.References()[1].long_term); // Index 1 > max 0.
    platform::SurfaceHandle released[16]{}; unsigned count = 0;
    CHECK(h.parser.Reset(released, count) && count == 2 && h.parser.References().empty());
    CHECK(allocation_probe::End() == 0 && h.arena.Statistics().allocation_count == h.allocations);
}
// POC types 1 and 2 (8.2.1.2-3), including non-reference pictures, frame_num wrap and MMCO 5.
void PocTypes() {
    {   // Type 1: cycle {4, 2}, non-reference offset -3, deltas sent (always_zero off).
        Config c; c.poc = 1; c.always_zero = false; c.cycle = 2; c.offsets[0] = 4; c.offsets[1] = 2; c.non_ref = -3; c.bottom_poc = true; c.top_bottom = 1;
        Harness h(c);
        SliceSpec s; s.type = 2; s.idr = true;
        auto p = h.Decode(c, s); CHECK(p.top_poc == 0 && p.bottom_poc == 1);
        // Reference frame_num n: absFrameNum n, expected = sum of offsets over n entries of the cycle {4, 2, 4, 2, ...}.
        const int expected_ref[]{0, 4, 6, 10, 12, 16, 18, 22, 24, 28, 30, 34, 36, 40, 42, 46};
        for (unsigned f = 1; f < 16; ++f) {
            s = {}; s.type = 0; s.frame = f; s.delta0 = f % 3 == 0 ? 1 : 0; s.delta1 = -1;
            p = h.Decode(c, s); CHECK(p.top_poc == expected_ref[f] + s.delta0 && p.bottom_poc == p.top_poc + 1 - 1);
        }
        // Non-reference B after frame 15, frame_num wraps to 0: FrameNumOffset 16, absFrameNum 16 - 1 = 15.
        s = {}; s.type = 1; s.frame = 0; s.reference = false;
        p = h.Decode(c, s); CHECK(p.frame_offset == 16 && p.top_poc == 46 + 4 * 0 + (46 - 46) - 3 + 0);
        s = {}; s.type = 0; s.frame = 0; p = h.Decode(c, s); CHECK(p.top_poc == 48); // absFrameNum 16: 8 cycles of 6.
        // MMCO 5 resets FrameNumOffset: the next reference frame_num 1 is absFrameNum 1 again.
        s = {}; s.type = 0; s.frame = 1; s.adaptive = true; s.mmco[0] = {5, 0, 0}; s.mmco_count = 1; h.Decode(c, s);
        s = {}; s.type = 0; s.frame = 1; p = h.Decode(c, s); CHECK(p.frame_offset == 0 && p.top_poc == 4);
    }
    {   // Type 1 with always_zero and an empty cycle: every POC is the non-reference offset or 0.
        Config c; c.poc = 1; c.cycle = 0; c.non_ref = -1; Harness h(c);
        SliceSpec s; s.type = 2; s.idr = true; h.Decode(c, s);
        s = {}; s.type = 1; s.frame = 1; s.reference = false; CHECK(h.Decode(c, s).top_poc == -1);
    }
    {   // Type 2: 2 * (FrameNumOffset + frame_num), minus 1 for non-reference pictures; wraps with frame_num.
        Config c; c.poc = 2; Harness h(c);
        SliceSpec s; s.type = 2; s.idr = true; h.Decode(c, s);
        for (unsigned f = 1; f < 20; ++f) {
            s = {}; s.type = 0; s.frame = f % 16; s.reference = f % 4 != 3;
            const auto p = h.Decode(c, s);
            // A non-reference picture repeats PrevRefFrameNum + 1, which the next reference picture then also uses.
            CHECK(p.top_poc == static_cast<int>(2 * f) - (s.reference ? 0 : 1));
            if (!s.reference) { s.reference = true; const auto q = h.Decode(c, s); CHECK(q.top_poc == static_cast<int>(2 * f)); }
        }
    }
    {   // Type 0 MSB across lsb wrap (4-bit lsb) with B-pictures before and after the wrap.
        Config c; c.poc_bits = 4; Harness h(c);
        SliceSpec s; s.type = 2; s.idr = true; h.Decode(c, s);
        const unsigned lsbs[]{6, 2, 4, 12, 8, 10, 2, 14, 0, 8, 4, 6};
        const int pocs[]{6, 2, 4, 12, 8, 10, 18, 14, 16, 24, 20, 22};
        const bool refs[]{true, false, false, true, false, false, true, false, false, true, false, false};
        unsigned frame = 0;
        for (unsigned i = 0; i < 12; ++i) {
            if (refs[i]) ++frame;
            s = {}; s.type = refs[i] ? 0 : 1; s.reference = refs[i]; s.frame = refs[i] ? frame : frame + 1; s.lsb = lsbs[i];
            const auto p = h.Decode(c, s); CHECK(p.top_poc == pocs[i]);
        }
    }
}
void Malformed() {
    Config c; core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20)); codec::H264SyntaxParser parser; CHECK(parser.Initialize(arena)); CHECK(arena.Seal());
    const auto sps = Sps(c), pps = Pps(c), au = Slice(0);
    for (unsigned n = 0; n < sps.size; ++n) CHECK(parser.ParameterSet(sps.View().first(n)) != codec::SyntaxStatus::Ready);
    CHECK(parser.ParameterSet(sps.View()) == codec::SyntaxStatus::Ready);
    for (unsigned n = 0; n < pps.size; ++n) CHECK(parser.ParameterSet(pps.View().first(n)) != codec::SyntaxStatus::Ready);
    CHECK(parser.ParameterSet(pps.View()) == codec::SyntaxStatus::Ready);
    for (unsigned n = 0; n < au.size; ++n) CHECK(parser.Parse(au.View().first(n), 4) != codec::SyntaxStatus::Ready);
    CHECK(parser.Parse(au.View(), 3) == codec::SyntaxStatus::Invalid);
    // Deterministic byte mutations exercise checked lengths, IDs and Exp-Golomb paths under sanitizers.
    allocation_probe::Begin();
    for (unsigned pos = 0; pos < au.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = au; changed.data[pos] = static_cast<std::byte>(v); (void)parser.Parse(changed.View(), 4); parser.Cancel();
    }
    for (unsigned pos = 0; pos < sps.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = sps; changed.data[pos] = static_cast<std::byte>(v); (void)parser.ParameterSet(changed.View());
    }
    CHECK(allocation_probe::End() == 0);
    // B slices with list modification, weights and MMCO, mutated and committed: marking must stay in bounds.
    Config b; b.profile = 100; b.poc = 1; b.always_zero = false; b.cycle = 2; b.offsets[0] = 2; b.offsets[1] = 2; b.refs = 4;
    b.weighted = true; b.weighted_bi = 1; b.bottom_poc = true;
    core::LinearMemoryArena arena2; CHECK(arena2.Initialize(1 << 20)); codec::H264SyntaxParser marking; CHECK(marking.Initialize(arena2)); CHECK(arena2.Seal());
    CHECK(marking.ParameterSet(Sps(b).View()) == codec::SyntaxStatus::Ready); CHECK(marking.ParameterSet(Pps(b).View()) == codec::SyntaxStatus::Ready);
    const auto sps_b = Sps(b);
    SliceSpec s; s.type = 1; s.frame = 1; s.l0 = 1; s.l1 = 1; s.delta0 = 3; s.delta1 = -2;
    s.modification[0][0] = {0, 0, 0}; s.modification[0][1] = {2, 0, 0}; s.modifications[0] = 2;
    s.modification[1][0] = {1, 3, 0}; s.modifications[1] = 1;
    s.adaptive = true; s.mmco[0] = {1, 0, 0}; s.mmco[1] = {4, 2, 0}; s.mmco[2] = {3, 1, 1}; s.mmco[3] = {6, 0, 0}; s.mmco[4] = {2, 1, 0}; s.mmco[5] = {5, 0, 0}; s.mmco_count = 6;
    const auto unit = Unit(b, s);
    SliceSpec idr; idr.type = 2; idr.idr = true; const auto start = Unit(b, idr);
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0, surface = 0;
    allocation_probe::Begin();
    for (unsigned pos = 4; pos < unit.size; ++pos) for (unsigned v = 0; v < 256; v += 3) {
        if (marking.Parse(start.View(), 4) == codec::SyntaxStatus::Ready) (void)marking.Commit({surface++ % 32, 1}, evicted, count);
        for (unsigned k = 0; k < 3; ++k) {
            auto changed = unit; changed.data[pos] = static_cast<std::byte>(v);
            if (marking.Parse(changed.View(), 4) == codec::SyntaxStatus::Ready) { CHECK(marking.Commit({surface++ % 32, 1}, evicted, count)); CHECK(count <= 16); }
            else marking.Cancel();
            CHECK(marking.References().size() <= 4);
        }
    }
    for (unsigned pos = 0; pos < sps_b.size; ++pos) for (unsigned v = 0; v < 256; v += 5) {
        auto changed = sps_b; changed.data[pos] = static_cast<std::byte>(v); (void)marking.ParameterSet(changed.View());
    }
    CHECK(allocation_probe::End() == 0);
}
}
int main() {
    Reader(); Sequence(0); Sequence(2); Description(); BFrames(); Marking(); PocTypes(); Malformed();
    std::printf("Portable H264: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures); return failures ? 1 : 0;
}
