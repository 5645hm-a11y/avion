#include "media/codec/HevcSyntaxParser.hpp"
#include "allocation_probe.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace media;
using codec::SyntaxStatus;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)

// ---- Bitstream authoring (test-only; independent from the parser under test) ----
struct Bytes {
    std::array<std::byte, 6144> data{}; unsigned size = 0;
    void Add(unsigned v) { data[size++] = static_cast<std::byte>(v & 255u); }
    std::span<const std::byte> View() const { return {data.data(), size}; }
};
unsigned CeilLog2(unsigned n) { unsigned bits = 0; while ((1u << bits) < n) ++bits; return bits; }
struct Bits {
    Bytes bytes; unsigned count = 0;
    void Bit(unsigned v) { if (!(count % 8)) bytes.Add(0); if (v) bytes.data[count / 8] |= static_cast<std::byte>(1u << (7 - count % 8)); ++count; }
    void Value(std::uint64_t v, unsigned n) { while (n) { --n; Bit(static_cast<unsigned>((v >> n) & 1u)); } }
    void Ue(std::uint32_t v) {
        unsigned n = 0; for (std::uint64_t t = std::uint64_t{v} + 1; t > 1; t >>= 1) ++n;
        for (unsigned i = 0; i < n; ++i) Bit(0);
        Value(std::uint64_t{v} + 1, n + 1);
    }
    void Se(int v) { Ue(v <= 0 ? static_cast<unsigned>(-2 * v) : static_cast<unsigned>(2 * v - 1)); }
    Bytes Nal(unsigned type, unsigned tid = 0, unsigned layer = 0) {
        Bit(1); while (count % 8) Bit(0);
        Bytes out; out.Add((type << 1) | (layer >> 5)); out.Add(((layer & 31u) << 3) | (tid + 1));
        unsigned zeros = 0;
        for (auto b : bytes.View()) {
            const auto v = std::to_integer<unsigned>(b);
            if (zeros == 2 && v <= 3) { out.Add(3); zeros = 0; }
            out.Add(v); zeros = v == 0 ? zeros + 1 : 0;
        }
        return out;
    }
};
Bytes Au(std::initializer_list<const Bytes*> nals, unsigned length = 4) {
    Bytes au;
    for (const auto* nal : nals) {
        for (unsigned i = length; i-- > 0;) au.Add(nal->size >> (8 * i));
        for (auto b : nal->View()) au.Add(std::to_integer<unsigned>(b));
    }
    return au;
}
void Ptl(Bits& b, unsigned profile, unsigned sub_layers) {
    auto general = [&](unsigned level) {
        b.Value(0, 2); b.Bit(0); b.Value(profile, 5); b.Value(std::uint64_t{1} << (31 - profile), 32);
        b.Bit(1); b.Bit(0); b.Bit(0); b.Bit(1); b.Value(0, 43); b.Bit(0); b.Value(level, 8);
    };
    general(93);
    for (unsigned i = 0; i < sub_layers; ++i) { b.Bit(1); b.Bit(1); }
    if (sub_layers) for (unsigned i = sub_layers; i < 8; ++i) b.Value(0, 2);
    for (unsigned i = 0; i < sub_layers; ++i) general(90); // 88 profile bits + 8 level bits.
}
struct Ref { int delta; bool used; };
struct Rps {
    bool inter = false;
    Ref negative[8]{}, positive[8]{}; unsigned negative_count = 0, positive_count = 0;
    unsigned delta_idx_minus1 = 0; int delta_rps = 0; unsigned flags = 0; bool used[17]{}, use_delta[17]{};
};
Rps Explicit(std::initializer_list<Ref> negative, std::initializer_list<Ref> positive = {}) {
    Rps r; for (auto x : negative) r.negative[r.negative_count++] = x; for (auto x : positive) r.positive[r.positive_count++] = x; return r;
}
Rps Inter(int delta_rps, std::initializer_list<int> used, std::initializer_list<int> use_delta, unsigned delta_idx_minus1 = 0) {
    Rps r; r.inter = true; r.delta_rps = delta_rps; r.delta_idx_minus1 = delta_idx_minus1;
    for (auto x : used) r.used[r.flags++] = x != 0;
    unsigned i = 0; for (auto x : use_delta) r.use_delta[i++] = x != 0;
    return r;
}
void WriteRps(Bits& b, const Rps& r, unsigned index, unsigned count) {
    if (index) b.Bit(r.inter);
    if (r.inter) {
        if (index == count) b.Ue(r.delta_idx_minus1);
        b.Bit(r.delta_rps < 0); b.Ue(static_cast<unsigned>(std::abs(r.delta_rps) - 1));
        for (unsigned j = 0; j < r.flags; ++j) { b.Bit(r.used[j]); if (!r.used[j]) b.Bit(r.use_delta[j]); }
        return;
    }
    b.Ue(r.negative_count); b.Ue(r.positive_count);
    int previous = 0;
    for (unsigned i = 0; i < r.negative_count; ++i) { b.Ue(static_cast<unsigned>(previous - r.negative[i].delta - 1)); previous = r.negative[i].delta; b.Bit(r.negative[i].used); }
    previous = 0;
    for (unsigned i = 0; i < r.positive_count; ++i) { b.Ue(static_cast<unsigned>(r.positive[i].delta - previous - 1)); previous = r.positive[i].delta; b.Bit(r.positive[i].used); }
}
// Explicit scaling_list_data covering explicit, copy (including 32x32 step 3), default and DC paths.
void ScalingFixture(Bits& b) {
    for (unsigned size = 0; size < 4; ++size)
        for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
            const unsigned n = size ? 64 : 16;
            auto explicit_list = [&](int dc, auto value) {
                b.Bit(1); int next = 8;
                if (size > 1) { b.Se(dc - 8); next = dc; }
                for (unsigned i = 0; i < n; ++i) {
                    const int v = value(i); int d = v - next;
                    if (d > 127) d -= 256;
                    else if (d < -128) d += 256;
                    b.Se(d); next = v;
                }
            };
            if (size == 0 && matrix == 0) explicit_list(0, [](unsigned i) { return static_cast<int>(10 + i); });
            else if (size == 0 && matrix == 1) { b.Bit(0); b.Ue(1); }
            else if (size == 1 && matrix == 0) explicit_list(0, [](unsigned i) { return static_cast<int>(20 + i % 7); });
            else if (size == 2 && matrix == 0) explicit_list(30, [](unsigned) { return 40; });
            else if (size == 2 && matrix == 3) { b.Bit(0); b.Ue(2); }
            else if (size == 3 && matrix == 3) { b.Bit(0); b.Ue(1); }
            else { b.Bit(0); b.Ue(0); }
        }
}
void HrdFixture(Bits& b, unsigned sub_layers) {
    b.Bit(1); b.Bit(1); b.Bit(1); // NAL + VCL HRD, sub-picture parameters.
    b.Value(23, 8); b.Value(4, 5); b.Bit(1); b.Value(4, 5); b.Value(1, 4); b.Value(2, 4); b.Value(3, 4);
    b.Value(23, 5); b.Value(23, 5); b.Value(23, 5);
    for (unsigned i = 0; i <= sub_layers; ++i) {
        unsigned count = 1;
        if (i == 0) { b.Bit(1); b.Ue(0); b.Ue(1); count = 2; }       // Fixed general rate; two CPBs.
        else if (i == 1) { b.Bit(0); b.Bit(0); b.Bit(1); }            // Variable rate, low delay.
        else { b.Bit(0); b.Bit(1); b.Ue(2); b.Ue(0); }                // Fixed within CVS.
        for (unsigned t = 0; t < 2; ++t)
            for (unsigned k = 0; k < count; ++k) { b.Ue(1000); b.Ue(2000); b.Ue(10); b.Ue(20); b.Bit(k & 1); }
    }
}
struct SpsSpec {
    unsigned id = 0, profile = 1, sub_layers = 0, chroma = 1, width = 64, height = 64, depth = 0, poc_bits = 8;
    bool crop = false; unsigned crop_offsets[4]{1, 2, 1, 0};
    unsigned max_dec = 4, reorder = 2, min_cb = 0, diff_cb = 1, min_tb = 0, diff_tb = 2;
    unsigned scaling = 0; // 0 disabled, 1 default lists, 2 explicit fixture.
    bool pcm = false, long_term = false, temporal_mvp = true;
    const Rps* sets = nullptr; unsigned set_count = 0;
    unsigned lt_count = 0, lt_lsb[4]{}; bool lt_used[4]{};
    unsigned vui = 0;       // 0 absent, 1 colour/timing/HRD, 2 field_seq_flag.
    unsigned sar_idc = 255, sar_width = 4, sar_height = 3; // In the VUI: aspect_ratio_idc, and the pair for 255.
    unsigned extension = 0; // 0 absent, 1 range extension, 2 extension_4bits payload.
};
Bytes Sps(const SpsSpec& s) {
    Bits b; b.Value(0, 4); b.Value(s.sub_layers, 3); b.Bit(1); Ptl(b, s.profile, s.sub_layers);
    b.Ue(s.id); b.Ue(s.chroma); if (s.chroma == 3) b.Bit(0);
    b.Ue(s.width); b.Ue(s.height); b.Bit(s.crop); if (s.crop) for (auto v : s.crop_offsets) b.Ue(v);
    b.Ue(s.depth); b.Ue(s.depth); b.Ue(s.poc_bits - 4);
    b.Bit(1); for (unsigned i = 0; i <= s.sub_layers; ++i) { b.Ue(s.max_dec); b.Ue(s.reorder); b.Ue(0); }
    b.Ue(s.min_cb); b.Ue(s.diff_cb); b.Ue(s.min_tb); b.Ue(s.diff_tb); b.Ue(1); b.Ue(1);
    b.Bit(s.scaling != 0); if (s.scaling) { b.Bit(s.scaling == 2); if (s.scaling == 2) ScalingFixture(b); }
    b.Bit(1); b.Bit(1); b.Bit(s.pcm);
    if (s.pcm) { b.Value(7, 4); b.Value(6, 4); b.Ue(0); b.Ue(1); b.Bit(1); }
    b.Ue(s.set_count); for (unsigned i = 0; i < s.set_count; ++i) WriteRps(b, s.sets[i], i, s.set_count);
    b.Bit(s.long_term);
    if (s.long_term) { b.Ue(s.lt_count); for (unsigned i = 0; i < s.lt_count; ++i) { b.Value(s.lt_lsb[i], s.poc_bits); b.Bit(s.lt_used[i]); } }
    b.Bit(s.temporal_mvp); b.Bit(1);
    b.Bit(s.vui != 0);
    if (s.vui) {
        b.Bit(1); b.Value(s.sar_idc, 8); if (s.sar_idc == 255) { b.Value(s.sar_width, 16); b.Value(s.sar_height, 16); } b.Bit(1); b.Bit(1);
        b.Bit(1); b.Value(5, 3); b.Bit(1); b.Bit(1); b.Value(9, 8); b.Value(16, 8); b.Value(9, 8);
        b.Bit(1); b.Ue(1); b.Ue(2); b.Bit(0); b.Bit(s.vui == 2); b.Bit(0);
        b.Bit(1); for (unsigned i = 0; i < 4; ++i) b.Ue(i);
        b.Bit(1); b.Value(1001, 32); b.Value(60000, 32); b.Bit(1); b.Ue(0); b.Bit(1); HrdFixture(b, s.sub_layers);
        b.Bit(1); b.Bit(0); b.Bit(1); b.Bit(0); b.Ue(0); b.Ue(2); b.Ue(1); b.Ue(15); b.Ue(15);
    }
    b.Bit(s.extension != 0);
    if (s.extension == 1) { b.Bit(1); b.Value(0, 3); b.Value(0, 4); b.Value(0, 9); }
    if (s.extension == 2) { b.Value(0, 4); b.Value(1, 4); b.Value(5, 3); }
    return b.Nal(33);
}
Bytes Vps(unsigned sub_layers = 0) {
    Bits b; b.Value(0, 4); b.Bit(1); b.Bit(1); b.Value(0, 6); b.Value(sub_layers, 3); b.Bit(1); b.Value(0xffff, 16);
    Ptl(b, 1, sub_layers); b.Bit(0); b.Ue(4); b.Ue(2); b.Ue(0); b.Value(0, 6); b.Ue(0); b.Bit(0); b.Bit(0);
    return b.Nal(32);
}
struct PpsSpec {
    unsigned id = 0, sps = 0, extra_bits = 0, tiles = 0, cu_depth = 1, merge = 0, extension = 0; int qp = 0;
    bool dependent = false, output_flag = false, scaling = false;
};
Bytes Pps(const PpsSpec& p) {
    Bits b; b.Ue(p.id); b.Ue(p.sps); b.Bit(p.dependent); b.Bit(p.output_flag); b.Value(p.extra_bits, 3); b.Bit(1); b.Bit(1);
    b.Ue(0); b.Ue(0); b.Se(p.qp); b.Bit(0); b.Bit(1); b.Bit(1); b.Ue(p.cu_depth);
    b.Se(-2); b.Se(3); b.Bit(0); b.Bit(1); b.Bit(0); b.Bit(0); b.Bit(p.tiles != 0); b.Bit(1);
    if (p.tiles == 1) { b.Ue(1); b.Ue(1); b.Bit(1); b.Bit(1); }
    if (p.tiles >= 2) { b.Ue(2); b.Ue(0); b.Bit(0); b.Ue(p.tiles == 2 ? 0 : 1); b.Ue(1); b.Bit(0); }
    b.Bit(1); b.Bit(1); b.Bit(1); b.Bit(0); b.Se(-2); b.Se(3);
    b.Bit(p.scaling); if (p.scaling) ScalingFixture(b);
    b.Bit(0); b.Ue(p.merge); b.Bit(0);
    b.Bit(p.extension != 0);
    if (p.extension == 1) { b.Bit(1); b.Value(0, 3); b.Value(0, 4); b.Value(0, 5); }
    if (p.extension == 2) { b.Value(0, 4); b.Value(8, 4); b.Value(3, 2); }
    return b.Nal(34);
}
struct Context {
    unsigned poc_bits = 8, sets = 0, ctbs = 16, lt_sps = 0, extra_bits = 0;
    bool long_term = false, temporal_mvp = true, output_flag = false, dependent = false;
};
struct Lt { unsigned sps_index = 0, lsb = 0; bool used = true, msb = false; unsigned cycle = 0; };
struct SliceSpec {
    unsigned nal = 1, tid = 0, slice_type = 1, poc = 0, pps = 0, address = 0;
    bool first = true, dependent = false, output = true, mvp = true;
    int rps_index = -1; Rps rps{};
    unsigned lt_sps = 0, lt_explicit = 0; Lt lt[4]{};
};
Bytes Slice(const Context& c, const SliceSpec& s, unsigned* rps_bits = nullptr) {
    Bits b; b.Bit(s.first); if (s.nal >= 16 && s.nal <= 23) b.Bit(0); b.Ue(s.pps);
    if (!s.first) { if (c.dependent) b.Bit(s.dependent); b.Value(s.address, CeilLog2(c.ctbs)); }
    if (!s.dependent) {
        b.Value(0, c.extra_bits); b.Ue(s.slice_type); if (c.output_flag) b.Bit(s.output);
        if (s.nal != 19 && s.nal != 20) {
            b.Value(s.poc & ((1u << c.poc_bits) - 1), c.poc_bits);
            b.Bit(s.rps_index >= 0);
            if (s.rps_index < 0) { const auto begin = b.count; WriteRps(b, s.rps, c.sets, c.sets); if (rps_bits) *rps_bits = b.count - begin; }
            else if (c.sets > 1) b.Value(static_cast<unsigned>(s.rps_index), CeilLog2(c.sets));
            if (c.long_term) {
                if (c.lt_sps) b.Ue(s.lt_sps);
                b.Ue(s.lt_explicit);
                for (unsigned i = 0; i < s.lt_sps + s.lt_explicit; ++i) {
                    if (i < s.lt_sps) { if (c.lt_sps > 1) b.Value(s.lt[i].sps_index, CeilLog2(c.lt_sps)); }
                    else { b.Value(s.lt[i].lsb, c.poc_bits); b.Bit(s.lt[i].used); }
                    b.Bit(s.lt[i].msb); if (s.lt[i].msb) b.Ue(s.lt[i].cycle);
                }
            }
            if (c.temporal_mvp) b.Bit(s.mvp);
        }
        b.Value(0x2D, 7); // Stand-in for the remaining header and slice data consumed by hardware.
    }
    return b.Nal(s.nal, s.tid);
}
struct Session {
    core::LinearMemoryArena arena; codec::HevcSyntaxParser parser;
    bool Open() { return arena.Initialize(1024 * 1024) && parser.Initialize(arena) && arena.Seal(); }
    SyntaxStatus Set(const Bytes& nal) { return parser.ParameterSet(nal.View()); }
    SyntaxStatus Parse(const Bytes& au, unsigned length = 4) { return parser.Parse(au.View(), length); }
    bool Commit(unsigned id, unsigned& count, platform::SurfaceHandle* evicted) { return parser.Commit({id, 1}, {evicted, 16}, count); }
};
bool Pocs(const codec::HevcPicture& p, const std::uint8_t* list, unsigned count, std::initializer_list<int> expected) {
    if (count != expected.size()) return false;
    unsigned i = 0;
    for (auto poc : expected) { if (list[i] >= p.reference_count || p.references[list[i]].poc != poc) return false; ++i; }
    return true;
}

// ---- Tests ----
void ParameterSets() {
    Session t; CHECK(t.Open());
    const auto allocations = t.arena.Statistics().allocation_count;
    CHECK(t.Set(Vps()) == SyntaxStatus::Ready);
    CHECK(t.Set(Vps(2)) == SyntaxStatus::Ready);
    SpsSpec s; s.crop = true; s.pcm = true; s.width = 64; s.height = 48;
    CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready);
    const auto* sps = t.parser.FirstSps();
    CHECK(sps && sps->width == 64 && sps->height == 48 && sps->left == 2 && sps->right == 60 && sps->top == 2 && sps->bottom == 48);
    CHECK(sps && sps->poc_lsb_bits == 8 && sps->max_dec_pic_buffering_minus1 == 4 && sps->max_num_reorder == 2);
    CHECK(sps && sps->pcm && sps->pcm_bit_depth_luma_minus1 == 7 && sps->pcm_bit_depth_chroma_minus1 == 6 && sps->log2_diff_max_min_pcm == 1);
    CHECK(sps && !sps->scaling_list_enabled && sps->scaling.lists[1][0][63] == 16 && sps->matrix == 2 && !sps->full_range);
    // Without a VUI nothing is said about colour or sample shape.
    CHECK(sps && !sps->signal_described && !sps->colour_described && sps->colour_primaries == 2 && sps->transfer == 2 && sps->sar_width == 0 && sps->sar_height == 0);
    // Main 10, VUI with colour description and a three-sub-layer HRD.
    SpsSpec ten; ten.id = 1; ten.depth = 2; ten.profile = 2; ten.sub_layers = 2; ten.vui = 1;
    CHECK(t.Set(Sps(ten)) == SyntaxStatus::Ready);
    ten.extension = 2; CHECK(t.Set(Sps(ten)) == SyntaxStatus::Ready); // Extension payload is ignored.
    CHECK(t.parser.FirstSps() == sps);
    Session v; CHECK(v.Open()); SpsSpec main10 = ten; main10.id = 0; main10.extension = 0;
    CHECK(v.Set(Sps(main10)) == SyntaxStatus::Ready);
    const auto* m = v.parser.FirstSps();
    CHECK(m && m->bit_depth_luma_minus8 == 2 && m->max_sub_layers_minus1 == 2 && m->profile == 2 && m->full_range &&
          m->colour_primaries == 9 && m->transfer == 16 && m->matrix == 9);
    // The VUI's explicit sample aspect ratio (EXTENDED_SAR 4:3) and the presence of the signal type.
    CHECK(m && m->sar_width == 4 && m->sar_height == 3 && m->signal_described && m->colour_described);
    // Table E.1 by idc, an explicit pair with a zero term (unspecified, not an error) and a reserved idc.
    struct Aspect { unsigned idc, w, h, expect_w, expect_h; };
    for (const auto a : {Aspect{1, 0, 0, 1, 1}, Aspect{4, 0, 0, 16, 11}, Aspect{14, 0, 0, 4, 3}, Aspect{16, 0, 0, 2, 1}, Aspect{255, 64, 45, 64, 45},
                         Aspect{255, 0, 45, 0, 0}, Aspect{255, 64, 0, 0, 0}, Aspect{0, 0, 0, 0, 0}, Aspect{17, 0, 0, 0, 0}, Aspect{200, 0, 0, 0, 0}}) {
        Session session; CHECK(session.Open());
        SpsSpec spec = main10; spec.sar_idc = a.idc; spec.sar_width = a.w; spec.sar_height = a.h;
        CHECK(session.Set(Sps(spec)) == SyntaxStatus::Ready);
        const auto* parsed = session.parser.FirstSps();
        CHECK(parsed && parsed->sar_width == a.expect_w && parsed->sar_height == a.expect_h && parsed->matrix == 9 && parsed->transfer == 16);
    }
    SpsSpec bad = ten; bad.extension = 1; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = ten; bad.vui = 2; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = s; bad.depth = 1; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = s; bad.chroma = 2; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = s; bad.profile = 4; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = s; bad.width = 8200; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Unsupported);
    bad = s; bad.width = 60; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Invalid); // Not a MinCb multiple.
    bad = s; bad.min_tb = 1; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Invalid);  // MinTb must be below MinCb.
    bad = s; bad.reorder = 5; CHECK(t.Set(Sps(bad)) == SyntaxStatus::Invalid);
    // Scaling lists: Table 7-6 defaults and an explicit/copy/default mix.
    SpsSpec defaults; defaults.id = 2; defaults.scaling = 1;
    CHECK(t.Set(Sps(defaults)) == SyntaxStatus::Ready);
    SpsSpec lists; lists.id = 3; lists.scaling = 2;
    CHECK(t.Set(Sps(lists)) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 2, .sps = 2})) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 3, .sps = 3, .tiles = 1, .scaling = true})) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 4, .sps = 3, .extension = 2})) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 5, .sps = 3, .extension = 1})) == SyntaxStatus::Unsupported);
    CHECK(t.Set(Pps({.id = 6, .sps = 3, .qp = -39})) == SyntaxStatus::Invalid);
    CHECK(t.Set(Pps({.id = 7, .sps = 3, .merge = 5})) == SyntaxStatus::Invalid);
    // Non-parameter units: SEI ignored, enhancement layers ignored, slices rejected.
    Bits sei; sei.Value(5, 8); const auto sei_nal = sei.Nal(39); CHECK(t.Set(sei_nal) == SyntaxStatus::Ready);
    Bits layer; layer.Value(0xAB, 8); const auto layer_nal = layer.Nal(33, 0, 1); CHECK(t.Set(layer_nal) == SyntaxStatus::Ready);
    Bits vcl; vcl.Value(0xAB, 8); const auto vcl_nal = vcl.Nal(1); CHECK(t.Set(vcl_nal) == SyntaxStatus::Unsupported);
    const auto vps = Vps(); CHECK(t.parser.ParameterSet(vps.View().first(1)) == SyntaxStatus::Invalid);
    CHECK(t.parser.ParameterSet(vps.View().first(4)) == SyntaxStatus::Invalid);
    CHECK(t.arena.Statistics().allocation_count == allocations);
}
void ScalingLists() {
    Session t; CHECK(t.Open());
    SpsSpec defaults; defaults.scaling = 1; CHECK(t.Set(Sps(defaults)) == SyntaxStatus::Ready);
    const auto* d = t.parser.FirstSps();
    CHECK(d && d->scaling_list_enabled && d->scaling.lists[0][4][15] == 16 && d->scaling.lists[1][0][63] == 115 &&
          d->scaling.lists[1][3][63] == 91 && d->scaling.lists[2][2][21] == 21 && d->scaling.lists[3][3][63] == 91 &&
          d->scaling.dc[0][5] == 16 && d->scaling.dc[1][3] == 16);
    Session e; CHECK(e.Open());
    SpsSpec lists; lists.scaling = 2; CHECK(e.Set(Sps(lists)) == SyntaxStatus::Ready);
    const auto* x = e.parser.FirstSps();
    bool good = x != nullptr;
    for (unsigned i = 0; good && i < 16; ++i) good = x->scaling.lists[0][0][i] == 10 + i && x->scaling.lists[0][1][i] == 10 + i;
    for (unsigned i = 0; good && i < 64; ++i)
        good = x->scaling.lists[1][0][i] == 20 + i % 7 && x->scaling.lists[2][0][i] == 40 &&
               x->scaling.lists[2][3][i] == x->scaling.lists[2][1][i] && x->scaling.lists[3][3][i] == x->scaling.lists[3][0][i];
    CHECK(good);
    CHECK(x && x->scaling.lists[0][2][0] == 16 && x->scaling.lists[2][1][63] == 115 && x->scaling.lists[3][0][63] == 115);
    CHECK(x && x->scaling.dc[0][0] == 30 && x->scaling.dc[0][3] == 16 && x->scaling.dc[1][3] == 16);
}
void InterRps() {
    // set0 explicit; set1 predicted from set0 (deltaRps -1); set2 predicted from set1 (deltaRps +1).
    // Expected values follow equations 7-61/7-62 computed by hand.
    const Rps sets[]{Explicit({{-1, true}, {-3, true}}, {{2, true}}),
                     Inter(-1, {1, 0, 1, 1}, {1, 1, 1, 1}),
                     Inter(1, {0, 1, 0, 1, 0}, {1, 1, 0, 1, 1})};
    Session t; CHECK(t.Open());
    SpsSpec s; s.sets = sets; s.set_count = 3; CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready);
    const auto* sps = t.parser.FirstSps(); CHECK(sps && sps->num_short_term_rps == 3);
    if (!sps) return;
    const auto& a = sps->rps[1];
    CHECK(a.negative == 3 && a.delta_s0[0] == -1 && a.delta_s0[1] == -2 && a.delta_s0[2] == -4 && a.used_s0 == 0b011);
    CHECK(a.positive == 1 && a.delta_s1[0] == 1 && a.used_s1 == 1 && a.reference_delta_pocs == 3);
    const auto& b = sps->rps[2];
    CHECK(b.negative == 1 && b.delta_s0[0] == -1 && b.used_s0 == 1);
    CHECK(b.positive == 2 && b.delta_s1[0] == 1 && b.delta_s1[1] == 2 && b.used_s1 == 0b10 && b.reference_delta_pocs == 4);
    CHECK(sps->rps[0].reference_delta_pocs == 0);
    // Slice-header inter prediction (delta_idx_minus1 selects set0 of a one-set SPS) and its bit count.
    const Rps one[]{Explicit({{-1, true}, {-2, true}})};
    Session u; CHECK(u.Open());
    SpsSpec p; p.sets = one; p.set_count = 1; CHECK(u.Set(Sps(p)) == SyntaxStatus::Ready); CHECK(u.Set(Pps({})) == SyntaxStatus::Ready);
    Context c; c.sets = 1;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    auto idr = Slice(c, {.nal = 19, .slice_type = 2}); auto au = Au({&idr});
    CHECK(u.Parse(au) == SyntaxStatus::Ready); CHECK(u.Commit(0, count, evicted));
    auto p1 = Slice(c, {.poc = 1, .rps = Explicit({{-1, true}})}); au = Au({&p1});
    CHECK(u.Parse(au) == SyntaxStatus::Ready); CHECK(u.Commit(1, count, evicted));
    auto p2 = Slice(c, {.poc = 2, .rps_index = 0}); au = Au({&p2});
    CHECK(u.Parse(au) == SyntaxStatus::Ready);
    CHECK(u.parser.Picture() && u.parser.Picture()->short_term_rps_bits == 0 && u.parser.Picture()->reference_rps_delta_pocs == 0);
    CHECK(u.Commit(2, count, evicted) && count == 0);
    unsigned bits = 0;
    auto p3 = Slice(c, {.poc = 3, .rps = Inter(-1, {1, 0, 1}, {1, 0, 1}, 0)}, &bits); au = Au({&p3});
    CHECK(u.Parse(au) == SyntaxStatus::Ready);
    const auto* picture = u.parser.Picture();
    CHECK(picture && picture->short_term_rps_bits == bits && bits != 0 && picture->reference_rps_delta_pocs == 2);
    CHECK(picture && Pocs(*picture, picture->st_curr_before, picture->st_curr_before_count, {2, 1}) && picture->reference_count == 2);
    CHECK(u.Commit(3, count, evicted) && count == 1 && evicted[0].id == 0);
    // Emulation-prevention bytes inside st_ref_pic_set do not count as RBSP bits.
    Session w; CHECK(w.Open());
    SpsSpec wide; wide.poc_bits = 16; wide.width = 4096; wide.height = 2304; wide.max_dec = 4;
    CHECK(w.Set(Sps(wide)) == SyntaxStatus::Ready); CHECK(w.Set(Pps({})) == SyntaxStatus::Ready);
    Context cw; cw.poc_bits = 16; cw.ctbs = 256 * 144;
    idr = Slice(cw, {.nal = 19, .slice_type = 2}); au = Au({&idr});
    CHECK(w.Parse(au) == SyntaxStatus::Ready); CHECK(w.Commit(0, count, evicted));
    auto far = Slice(cw, {.poc = 1, .rps = Explicit({{-1, true}, {-32769, false}, {-65537, false}})}, &bits); au = Au({&far});
    bool escaped = false;
    for (unsigned i = 2; i < far.size; ++i)
        if (std::to_integer<unsigned>(far.data[i]) == 3 && far.data[i - 1] == std::byte{0} && far.data[i - 2] == std::byte{0}) escaped = true;
    CHECK(escaped);
    CHECK(w.Parse(au) == SyntaxStatus::Ready);
    CHECK(w.parser.Picture() && w.parser.Picture()->short_term_rps_bits == bits);
}
// Hierarchical GOP-4 in decode order with three temporal sub-layers and POC LSB wrap (MaxLsb = 16).
void Gop(bool sps_sets) {
    const Rps a = Explicit({{-4, true}}), b = Explicit({{-2, true}}, {{2, true}}),
              c = Explicit({{-1, true}}, {{1, true}, {3, false}}), d = Explicit({{-1, true}}, {{1, true}});
    const Rps sets[]{a, b, c, d};
    Session t; CHECK(t.Open());
    SpsSpec s; s.poc_bits = 4; s.sub_layers = 2; s.max_dec = 3; s.reorder = 2;
    if (sps_sets) { s.sets = sets; s.set_count = 4; }
    const auto vps = Vps(2), sps = Sps(s), pps = Pps({});
    Context ctx; ctx.poc_bits = 4; ctx.sets = sps_sets ? 4 : 0;
    int model_poc[16]{}; unsigned model_surface[16]{}, model_count = 0;
    platform::SurfaceHandle evicted[16]{};
    unsigned surface = 0;
    allocation_probe::Begin();
    auto decode = [&](unsigned nal, unsigned tid, unsigned slice_type, int poc, int set, const Rps* rps) {
        SliceSpec spec; spec.nal = nal; spec.tid = tid; spec.slice_type = slice_type; spec.poc = static_cast<unsigned>(poc);
        if (rps) { if (sps_sets) spec.rps_index = set; else spec.rps = *rps; }
        const auto slice = Slice(ctx, spec);
        const auto au = surface == 0 ? Au({&vps, &sps, &pps, &slice}) : Au({&slice});
        CHECK(t.Parse(au) == SyntaxStatus::Ready);
        const auto* p = t.parser.Picture(); if (!p) return;
        CHECK(p->poc == poc);
        // Independent model of 8.3.2 for this explicit RPS.
        int before[8]{}, after[8]{}; unsigned nb = 0, na = 0, kept = 0; bool keep[16]{};
        if (rps) {
            for (unsigned i = 0; i < rps->negative_count + rps->positive_count; ++i) {
                const bool negative = i < rps->negative_count;
                const auto ref = negative ? rps->negative[i] : rps->positive[i - rps->negative_count];
                for (unsigned j = 0; j < model_count; ++j) if (model_poc[j] == poc + ref.delta) {
                    keep[j] = true; ++kept;
                    if (ref.used) (negative ? before[nb++] : after[na++]) = poc + ref.delta;
                }
            }
        }
        CHECK(p->reference_count == kept && p->st_curr_before_count == nb && p->st_curr_after_count == na && !p->lt_curr_count);
        for (unsigned i = 0; i < nb; ++i) CHECK(p->references[p->st_curr_before[i]].poc == before[i]);
        for (unsigned i = 0; i < na; ++i) CHECK(p->references[p->st_curr_after[i]].poc == after[i]);
        const bool retain = !(nal == 0 && tid == 2);
        CHECK(p->retain == retain && p->intra == (slice_type == 2));
        unsigned count = 0; CHECK(t.Commit(surface, count, evicted));
        unsigned expected_evictions = 0, next = 0;
        for (unsigned j = 0; j < model_count; ++j) {
            if (keep[j]) { model_poc[next] = model_poc[j]; model_surface[next++] = model_surface[j]; continue; }
            bool found = false; for (unsigned e = 0; e < count; ++e) found = found || evicted[e].id == model_surface[j];
            CHECK(found); ++expected_evictions;
        }
        CHECK(count == expected_evictions);
        model_count = next;
        if (retain) { model_poc[model_count] = poc; model_surface[model_count++] = surface; }
        ++surface;
    };
    decode(19, 0, 2, 0, -1, nullptr);
    for (int k = 0; k < 10; ++k) {
        const int base = 4 * k;
        decode(1, 0, 1, base + 4, 0, &a);
        decode(1, 1, 0, base + 2, 1, &b);
        decode(0, 2, 0, base + 1, 2, &c);
        decode(0, 2, 0, base + 3, 3, &d);
    }
    CHECK(allocation_probe::End() == 0);
}
void LongTerm() {
    Session t; CHECK(t.Open());
    SpsSpec s; s.poc_bits = 4; s.max_dec = 4; s.long_term = true; s.lt_count = 1; s.lt_lsb[0] = 0; s.lt_used[0] = true;
    CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready); CHECK(t.Set(Pps({})) == SyntaxStatus::Ready);
    Context c; c.poc_bits = 4; c.long_term = true; c.lt_sps = 1;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    auto run = [&](const SliceSpec& spec) { const auto slice = Slice(c, spec); const auto au = Au({&slice}); return t.Parse(au); };
    CHECK(run({.nal = 19, .slice_type = 2}) == SyntaxStatus::Ready); CHECK(t.Commit(0, count, evicted));
    CHECK(run({.poc = 1, .rps = Explicit({{-1, true}})}) == SyntaxStatus::Ready); CHECK(t.Commit(1, count, evicted));
    // POC 2: POC 0 becomes long-term through lt_idx_sps (LSB match).
    CHECK(run({.poc = 2, .rps = Explicit({{-1, true}}), .lt_sps = 1}) == SyntaxStatus::Ready);
    const auto* p = t.parser.Picture();
    CHECK(p && p->lt_curr_count == 1 && p->references[p->lt_curr[0]].poc == 0 && p->references[p->lt_curr[0]].long_term);
    CHECK(p && Pocs(*p, p->st_curr_before, p->st_curr_before_count, {1}));
    CHECK(t.Commit(2, count, evicted) && count == 0);
    for (unsigned poc = 3; poc <= 16; ++poc) {
        SliceSpec spec{.poc = poc, .rps = Explicit({{-1, true}}), .lt_explicit = 1};
        spec.lt[0] = {0, 0, true, true, poc >= 16 ? 1u : 0u};
        CHECK(run(spec) == SyntaxStatus::Ready);
        p = t.parser.Picture();
        CHECK(p && p->lt_curr_count == 1 && p->references[p->lt_curr[0]].poc == 0);
        CHECK(t.Commit(poc, count, evicted) && count == 1 && evicted[0].id == poc - 2);
    }
    // POC 17: an LSB-only match is ambiguous between POC 16 (short-term) and POC 0 (long-term).
    SliceSpec ambiguous{.poc = 17, .rps = Explicit({{-1, true}}), .lt_explicit = 1};
    ambiguous.lt[0] = {0, 0, true, false, 0};
    CHECK(run(ambiguous) == SyntaxStatus::Invalid);
    SliceSpec missing = ambiguous; missing.lt[0] = {0, 5, true, true, 0};
    CHECK(run(missing) == SyntaxStatus::Invalid); // Used long-term reference absent from the DPB.
    SliceSpec cycle = ambiguous; cycle.lt[0] = {0, 0, true, true, 1};
    CHECK(run(cycle) == SyntaxStatus::Ready); CHECK(t.Commit(17, count, evicted) && count == 1 && evicted[0].id == 15);
    // POC 18 lists POC 0 only as a short-term following entry: a long-term picture is never found that way.
    CHECK(run({.poc = 18, .rps = Explicit({{-1, true}, {-18, false}})}) == SyntaxStatus::Ready);
    p = t.parser.Picture(); CHECK(p && p->reference_count == 1);
    CHECK(t.Commit(18, count, evicted) && count == 2 && evicted[0].id == 0 && evicted[1].id == 16);
}
// DeltaPocMsbCycleLt (7-52) accumulates within the SPS-sourced and the slice-sourced long-term
// entries separately; entry num_long_term_sps restarts the sum.
void LongTermCycles() {
    Session t; CHECK(t.Open());
    SpsSpec s; s.poc_bits = 4; s.max_dec = 4; s.long_term = true; s.lt_count = 1; s.lt_lsb[0] = 0; s.lt_used[0] = true;
    CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready); CHECK(t.Set(Pps({})) == SyntaxStatus::Ready);
    Context c; c.poc_bits = 4; c.long_term = true; c.lt_sps = 1;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    auto run = [&](const SliceSpec& spec) { const auto slice = Slice(c, spec); const auto au = Au({&slice}); return t.Parse(au); };
    CHECK(run({.nal = 19, .slice_type = 2}) == SyntaxStatus::Ready); CHECK(t.Commit(0, count, evicted));
    CHECK(run({.poc = 1, .rps = Explicit({{-1, true}})}) == SyntaxStatus::Ready); CHECK(t.Commit(1, count, evicted));
    for (unsigned poc = 2; poc <= 17; ++poc) {
        SliceSpec spec{.poc = poc, .rps = Explicit({{-1, true}}), .lt_explicit = 1};
        spec.lt[0] = {0, 0, true, true, poc >= 16 ? 1u : 0u};
        CHECK(run(spec) == SyntaxStatus::Ready); CHECK(t.Commit(poc, count, evicted));
    }
    // POC 18, DPB {0 (LT), 16, 17}: SPS entry (cycle 1) -> POC 0; slice entry restarts at cycle 0 -> POC 16.
    SliceSpec boundary{.poc = 18, .rps = Explicit({{-1, true}}), .lt_sps = 1, .lt_explicit = 1};
    boundary.lt[0] = {0, 0, true, true, 1}; boundary.lt[1] = {0, 0, true, true, 0};
    CHECK(run(boundary) == SyntaxStatus::Ready);
    const auto* p = t.parser.Picture();
    CHECK(p && p->lt_curr_count == 2 && p->references[p->lt_curr[0]].poc == 0 && p->references[p->lt_curr[1]].poc == 16);
    CHECK(p && p->references[p->lt_curr[1]].long_term);
    CHECK(t.Commit(18, count, evicted) && count == 0);
    // Two slice entries: the second signals a delta that accumulates onto the first. From POC 32 the
    // first cycle is nonzero, so a missing accumulation would resolve both entries to POC 16.
    for (unsigned poc = 19; poc <= 35; ++poc) {
        SliceSpec accumulated{.poc = poc, .rps = Explicit({{-1, true}}), .lt_explicit = 2};
        accumulated.lt[0] = {0, 0, true, true, (poc >> 4) - 1}; accumulated.lt[1] = {0, 0, true, true, 1};
        CHECK(run(accumulated) == SyntaxStatus::Ready);
        p = t.parser.Picture();
        CHECK(p && p->lt_curr_count == 2 && p->references[p->lt_curr[0]].poc == 16 && p->references[p->lt_curr[1]].poc == 0);
        CHECK(t.Commit(poc, count, evicted) && count == 1 && evicted[0].id == poc - 2);
    }
}
// prevTid0Pic (8.3.1) skips TemporalId > 0 and sub-layer non-reference pictures. MaxLsb = 16; each
// skipped picture is placed so that using it as the predecessor would yield a different POC MSB.
void TemporalPoc() {
    Session t; CHECK(t.Open());
    SpsSpec s; s.poc_bits = 4; s.sub_layers = 1; s.max_dec = 4;
    CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready); CHECK(t.Set(Pps({})) == SyntaxStatus::Ready);
    Context c; c.poc_bits = 4;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0, surface = 0;
    auto run = [&](const SliceSpec& spec, int expected) {
        const auto slice = Slice(c, spec); const auto au = Au({&slice});
        CHECK(t.Parse(au) == SyntaxStatus::Ready);
        CHECK(t.parser.Picture() && t.parser.Picture()->poc == expected);
        CHECK(t.Commit(surface++, count, evicted));
    };
    run({.nal = 19, .slice_type = 2}, 0);
    run({.poc = 8, .rps = Explicit({{-8, true}})}, 8);
    run({.tid = 1, .poc = 1, .rps = Explicit({{-1, true}}, {{7, false}})}, 1);  // TemporalId 1.
    run({.poc = 12, .rps = Explicit({{-4, true}})}, 12);                          // From 8, not from 1.
    run({.nal = 0, .slice_type = 0, .poc = 5, .rps = Explicit({}, {{3, true}, {7, false}})}, 5); // TRAIL_N, TemporalId 0.
    run({.poc = 16, .rps = Explicit({{-4, true}})}, 16);                          // From 12, not from 5.
}
void RandomAccess() {
    Session t; CHECK(t.Open());
    SpsSpec s; CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready); CHECK(t.Set(Pps({})) == SyntaxStatus::Ready);
    Context c;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    auto run = [&](const SliceSpec& spec, bool eos = false) {
        const auto slice = Slice(c, spec);
        Bits e; const auto end = e.Nal(36);
        const auto au = eos ? Au({&slice, &end}) : Au({&slice});
        return t.Parse(au);
    };
    CHECK(run({.nal = 1, .poc = 4, .rps = Explicit({{-4, true}})}) == SyntaxStatus::NeedIdr);
    CHECK(run({.nal = 21, .slice_type = 2, .poc = 16}) == SyntaxStatus::Ready);
    const auto* p = t.parser.Picture();
    CHECK(p && p->irap && !p->idr && p->no_rasl_output && p->poc == 16 && p->reference_count == 0);
    CHECK(t.Commit(0, count, evicted));
    CHECK(run({.nal = 8, .slice_type = 0, .poc = 12, .rps = Explicit({{-12, true}}, {{4, true}})}) == SyntaxStatus::Discard);
    CHECK(!t.parser.Picture());
    CHECK(run({.nal = 6, .slice_type = 0, .poc = 14, .rps = Explicit({}, {{2, true}})}) == SyntaxStatus::Ready);
    p = t.parser.Picture(); CHECK(p && !p->retain && Pocs(*p, p->st_curr_after, p->st_curr_after_count, {16}));
    CHECK(t.Commit(1, count, evicted) && count == 0);
    CHECK(run({.poc = 20, .rps = Explicit({{-4, true}})}) == SyntaxStatus::Ready); CHECK(t.Commit(2, count, evicted) && count == 0);
    // Mid-stream CRA: NoRaslOutputFlag = 0; its RPS keeps POC 20 for the following RASL picture.
    CHECK(run({.nal = 21, .slice_type = 2, .poc = 24, .rps = Explicit({{-4, false}})}) == SyntaxStatus::Ready);
    p = t.parser.Picture(); CHECK(p && !p->no_rasl_output && p->reference_count == 1);
    CHECK(t.Commit(3, count, evicted) && count == 1 && evicted[0].id == 0);
    CHECK(run({.nal = 9, .slice_type = 0, .poc = 22, .rps = Explicit({{-2, true}}, {{2, true}})}) == SyntaxStatus::Ready);
    p = t.parser.Picture();
    CHECK(p && Pocs(*p, p->st_curr_before, p->st_curr_before_count, {20}) && Pocs(*p, p->st_curr_after, p->st_curr_after_count, {24}));
    CHECK(t.Commit(4, count, evicted));
    CHECK(run({.poc = 28, .rps = Explicit({{-4, true}})}, true) == SyntaxStatus::Ready);
    CHECK(t.parser.Picture() && t.parser.Picture()->end_of_sequence);
    CHECK(t.Commit(5, count, evicted));
    CHECK(run({.poc = 32, .rps = Explicit({{-4, true}})}) == SyntaxStatus::NeedIdr);
    // CRA after end of sequence: POC MSB resets and every reference is released.
    CHECK(run({.nal = 21, .slice_type = 2, .poc = 3}) == SyntaxStatus::Ready);
    p = t.parser.Picture(); CHECK(p && p->no_rasl_output && p->poc == 3);
    CHECK(t.Commit(6, count, evicted) && count == 2);
    CHECK(run({.nal = 8, .slice_type = 0, .poc = 1, .rps = Explicit({}, {{2, true}})}) == SyntaxStatus::Discard);
    CHECK(run({.nal = 17, .slice_type = 2, .poc = 40}) == SyntaxStatus::Ready);
    p = t.parser.Picture(); CHECK(p && p->poc == 40 && p->no_rasl_output);
    CHECK(t.Commit(7, count, evicted) && count == 1 && evicted[0].id == 6);
    CHECK(run({.nal = 20, .slice_type = 2}) == SyntaxStatus::Ready);
    CHECK(t.parser.Picture() && t.parser.Picture()->poc == 0 && t.parser.Picture()->idr);
    CHECK(t.Commit(8, count, evicted) && count == 1);
}
void Structure() {
    Session t; CHECK(t.Open());
    SpsSpec s; CHECK(t.Set(Sps(s)) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.extra_bits = 2, .dependent = true, .output_flag = true})) == SyntaxStatus::Ready);
    Context c; c.extra_bits = 2; c.dependent = true; c.output_flag = true;
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
    auto idr = Slice(c, {.nal = 19, .slice_type = 2});
    auto second = Slice(c, {.nal = 19, .slice_type = 2, .address = 5, .first = false});
    auto third = Slice(c, {.nal = 19, .address = 9, .first = false, .dependent = true});
    Bits sei_bits; sei_bits.Value(0x55, 8); const auto sei = sei_bits.Nal(39);
    Bits aud_bits; aud_bits.Value(2, 3); const auto aud = aud_bits.Nal(35);
    Bits layer_bits; layer_bits.Value(0, 24); const auto enhancement = layer_bits.Nal(1, 0, 1);
    auto au = Au({&aud, &sei, &idr, &enhancement, &second, &third});
    CHECK(t.Parse(au) == SyntaxStatus::Ready);
    const auto* p = t.parser.Picture();
    CHECK(p && p->slice_count == 3 && p->slices[0].offset == aud.size + sei.size + 12 && p->slices[0].bytes == idr.size);
    CHECK(p && p->slices[2].offset + p->slices[2].bytes == au.size && p->intra);
    CHECK(t.Parse(au) == SyntaxStatus::Busy);
    CHECK(t.Set(Pps({})) == SyntaxStatus::Invalid); // Parameter sets cannot change under a pending picture.
    t.parser.Cancel(); CHECK(!t.parser.Picture());
    auto swapped = Au({&idr, &third, &second}); CHECK(t.Parse(swapped) == SyntaxStatus::Invalid);
    auto missing_first = Au({&second}); CHECK(t.Parse(missing_first) == SyntaxStatus::Invalid);
    auto two_pictures = Au({&idr, &idr}); CHECK(t.Parse(two_pictures) == SyntaxStatus::Unsupported);
    auto p_in_irap = Slice(c, {.nal = 19, .slice_type = 1}); au = Au({&p_in_irap}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    Bits reserved_bits; reserved_bits.Value(0x80, 8); const auto reserved = reserved_bits.Nal(22);
    au = Au({&reserved}); CHECK(t.Parse(au) == SyntaxStatus::Unsupported);
    au = Au({&sei}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    const auto pps = Pps({.extra_bits = 2, .dependent = true, .output_flag = true});
    au = Au({&idr, &pps}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    Bits eos_bits; const auto eos = eos_bits.Nal(36);
    au = Au({&idr, &eos, &second}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    au = Au({&idr}, 2); CHECK(t.Parse(au, 2) == SyntaxStatus::Ready);
    CHECK(t.parser.Commit({0, 1}, {evicted, 15}, count) == false);
    CHECK(t.Commit(0, count, evicted) && !t.parser.Commit({1, 1}, {evicted, 16}, count));
    // pic_output_flag, mismatched segment headers and missing references.
    auto hidden = Slice(c, {.poc = 1, .output = false, .rps = Explicit({{-1, true}})}); au = Au({&hidden});
    CHECK(t.Parse(au) == SyntaxStatus::Ready); CHECK(t.parser.Picture() && !t.parser.Picture()->output && !t.parser.Picture()->intra);
    t.parser.Cancel();
    auto other_poc = Slice(c, {.poc = 2, .address = 3, .first = false, .rps = Explicit({{-2, true}})});
    au = Au({&hidden, &other_poc}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    auto no_refs = Slice(c, {.poc = 1, .rps = Explicit({{-1, false}})}); au = Au({&no_refs}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    auto absent = Slice(c, {.poc = 1, .rps = Explicit({{-3, true}})}); au = Au({&absent}); CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    auto irap_refs = Slice(c, {.nal = 21, .slice_type = 2, .poc = 4, .rps = Explicit({{-4, true}})}); au = Au({&irap_refs});
    CHECK(t.Parse(au) == SyntaxStatus::Invalid);
    // In-band parameter sets: identical repeat accepted, active SPS change rejected, PPS change accepted.
    const auto sps = Sps(s);
    au = Au({&sps, &pps, &hidden}); CHECK(t.Parse(au) == SyntaxStatus::Ready); t.parser.Cancel();
    SpsSpec changed = s; changed.width = 128; CHECK(t.Set(Sps(changed)) == SyntaxStatus::Unsupported);
    changed.id = 1; CHECK(t.Set(Sps(changed)) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 1, .sps = 1})) == SyntaxStatus::Ready);
    auto other_sps = Slice(c, {.poc = 1, .pps = 1, .rps = Explicit({{-1, true}})}); au = Au({&other_sps});
    CHECK(t.Parse(au) == SyntaxStatus::Unsupported);
    // Tiles: 4x4 CTBs; explicit columns must leave room for the last column.
    CHECK(t.Set(Pps({.id = 2, .tiles = 2})) == SyntaxStatus::Ready);
    CHECK(t.Set(Pps({.id = 3, .tiles = 3})) == SyntaxStatus::Ready);
    Context plain;
    auto tiled = Slice(plain, {.poc = 1, .pps = 2, .rps = Explicit({{-1, true}})}); au = Au({&tiled});
    CHECK(t.Parse(au) == SyntaxStatus::Ready);
    CHECK(t.parser.Picture() && t.parser.Picture()->pps->column_width_minus1[1] == 1 && !t.parser.Picture()->pps->uniform_spacing);
    t.parser.Cancel();
    auto overfull = Slice(plain, {.poc = 1, .pps = 3, .rps = Explicit({{-1, true}})}); au = Au({&overfull});
    CHECK(t.Parse(au) == SyntaxStatus::Invalid);
}
void Malformed() {
    const Rps sets[]{Explicit({{-1, true}, {-2, false}}, {{1, true}}), Inter(-1, {1, 0, 1, 1}, {1, 1, 1, 1})};
    SpsSpec rich; rich.sets = sets; rich.set_count = 2; rich.scaling = 2; rich.vui = 1; rich.sub_layers = 2; rich.pcm = true;
    rich.long_term = true; rich.lt_count = 2; rich.lt_lsb[1] = 7; rich.crop = true;
    const auto sps = Sps(rich), pps = Pps({.tiles = 2, .dependent = true, .scaling = true}), vps = Vps(2);
    Session t; CHECK(t.Open());
    for (unsigned n = 0; n < sps.size; ++n) CHECK(t.parser.ParameterSet(sps.View().first(n)) != SyntaxStatus::Ready);
    for (unsigned n = 0; n < pps.size; ++n) CHECK(t.parser.ParameterSet(pps.View().first(n)) != SyntaxStatus::Ready);
    CHECK(t.Set(sps) == SyntaxStatus::Ready); CHECK(t.Set(pps) == SyntaxStatus::Ready); CHECK(t.Set(vps) == SyntaxStatus::Ready);
    Context c; c.sets = 2; c.long_term = true; c.lt_sps = 2; c.dependent = true;
    auto idr = Slice(c, {.nal = 19, .slice_type = 2});
    auto second = Slice(c, {.nal = 19, .slice_type = 2, .address = 7, .first = false});
    const auto idr_au = Au({&idr, &second});
    // Length prefixes make every cut Invalid except the one at the boundary between the two slice NALs.
    for (unsigned n = 0; n < idr_au.size; ++n) {
        const auto state = t.parser.Parse(idr_au.View().first(n), 4); t.parser.Cancel();
        CHECK(state == (n == 4 + idr.size ? SyntaxStatus::Ready : SyntaxStatus::Invalid));
    }
    CHECK(t.Parse(idr_au) == SyntaxStatus::Ready);
    platform::SurfaceHandle evicted[16]{}; unsigned count = 0; CHECK(t.Commit(0, count, evicted));
    SliceSpec trailing{.poc = 1, .rps = Inter(-1, {0, 0, 0, 1}, {0, 0, 0, 1}, 1), .lt_sps = 1, .lt_explicit = 1};
    trailing.lt[0] = {1, 0, false, true, 3}; trailing.lt[1] = {0, 9, false, false, 0};
    auto p = Slice(c, trailing);
    const auto au = Au({&p});
    CHECK(t.Parse(au) == SyntaxStatus::Ready);
    CHECK(t.parser.Picture() && t.parser.Picture()->reference_count == 1 && t.parser.Picture()->short_term_rps_bits != 0);
    t.parser.Cancel();
    // Deterministic single-byte mutations exercise checked lengths, IDs and Exp-Golomb paths under sanitizers.
    allocation_probe::Begin();
    for (unsigned pos = 0; pos < au.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = au; changed.data[pos] = static_cast<std::byte>(v); (void)t.Parse(changed); t.parser.Cancel();
    }
    for (unsigned pos = 0; pos < idr_au.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = idr_au; changed.data[pos] = static_cast<std::byte>(v); (void)t.Parse(changed); t.parser.Cancel();
    }
    for (unsigned pos = 0; pos < pps.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = pps; changed.data[pos] = static_cast<std::byte>(v); (void)t.Set(changed);
    }
    for (unsigned pos = 0; pos < sps.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = sps; changed.data[pos] = static_cast<std::byte>(v); (void)t.Set(changed);
    }
    for (unsigned pos = 0; pos < vps.size; ++pos) for (unsigned v = 0; v < 256; ++v) {
        auto changed = vps; changed.data[pos] = static_cast<std::byte>(v); (void)t.Set(changed);
    }
    CHECK(allocation_probe::End() == 0);
}
}
int main() {
    ParameterSets(); ScalingLists(); InterRps(); Gop(false); Gop(true); LongTerm(); LongTermCycles(); TemporalPoc(); RandomAccess(); Structure(); Malformed();
    std::printf("Portable HEVC: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
