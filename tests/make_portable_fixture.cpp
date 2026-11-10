// Offline fixture authoring, portable (no platform encoder): the host-driven SDK test's media (docs/sdk.md, "Host-driven
// playback"), written from scratch so that every platform can make it and every byte of it is known.
//   media_make_portable_fixture out.mp4
// - Video (track 1): 9 s of 64x64 H.264 at 24 fps, Baseline, CAVLC. Even pictures are IDRs of I_PCM macroblocks, odd ones
//   P pictures of skipped macroblocks (exact repeats). Three scenes of flat luma with a fixed texture of 0-23 codes, dark 24
//   (0-3 s), bright 196 (3-6 s), mid 110 (6-9 s): hard cuts at 3 s and 6 s. In the third scene a bright square (12x12,
//   luma 235) starts at the centre and moves 16 samples to the right by 9 s (the tracking target).
// - Audio (track 2, English, enabled): `ipcm` 16-bit little-endian, 48 kHz stereo, 1024 frames a sample: 440 Hz at
//   -12 dBFS and 4 kHz at -24.4 dBFS on both channels.
// - Audio (track 3, French, same alternate group): QuickTime `sowt`, 44.1 kHz mono, 1024 frames a sample: 660 Hz at -10.5 dBFS.
// - Nero chapters at 0, 3 and 6 s: "Opening", "Bright", "Moving".
// Allocation happens freely here: this is a tool, not the engine.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr double kPi = 3.14159265358979323846;
struct Writer {
    Bytes b;
    void B(unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); }
    void U16(unsigned v) { B(v >> 8); B(v); }
    void U32(unsigned v) { U16(v >> 16); U16(v); }
    void Zero(unsigned n) { while (n--) B(0); }
    unsigned Box(const char* tag) { const auto at = static_cast<unsigned>(b.size()); U32(0); for (unsigned i = 0; i < 4; ++i) B(static_cast<unsigned char>(tag[i])); return at; }
    void End(unsigned at) { const auto size = static_cast<unsigned>(b.size()) - at; for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(size >> (24 - 8 * i)); }
    void Data(const Bytes& bytes) { b.insert(b.end(), bytes.begin(), bytes.end()); }
};
struct Bits {
    Bytes b; unsigned bits = 0;
    void Bit(unsigned v) { if (!(bits % 8)) b.push_back(0); if (v) b.back() |= static_cast<std::uint8_t>(1u << (7 - bits % 8)); ++bits; }
    void Value(unsigned v, unsigned n) { while (n) Bit((v >> --n) & 1); }
    void Ue(unsigned v) { unsigned n = 0; for (unsigned t = v + 1; t > 1; t >>= 1) ++n; for (unsigned i = 0; i < n; ++i) Bit(0); Value(v + 1, n + 1); }
    void Align() { while (bits % 8) Bit(0); }
    Bytes Nal(unsigned header) { Bit(1); Align(); Bytes out{static_cast<std::uint8_t>(header)}; unsigned zeros = 0; for (auto v : b) { if (zeros == 2 && v <= 3) { out.push_back(3); zeros = 0; } out.push_back(v); zeros = v == 0 ? zeros + 1 : 0; } return out; }
};
Bytes Sps() { // Baseline, level 3.0, 64x64 (4x4 macroblocks), frame_num and POC LSB of 4 bits, one reference frame.
    Bits b; b.Value(66, 8); b.Value(0, 8); b.Value(30, 8); b.Ue(0); b.Ue(0); b.Ue(0); b.Ue(0);
    b.Ue(1); b.Bit(0); b.Ue(3); b.Ue(3); b.Bit(1); b.Bit(1); b.Bit(0); b.Bit(0); return b.Nal(0x67);
}
Bytes Pps() { // CAVLC, one slice group, one reference, deblocking control present.
    Bits b; b.Ue(0); b.Ue(0); b.Bit(0); b.Bit(0); b.Ue(0); b.Ue(0); b.Ue(0);
    b.Bit(0); b.Value(0, 2); b.Ue(0); b.Ue(0); b.Ue(0); b.Bit(1); b.Bit(0); b.Bit(0); return b.Nal(0x68);
}
constexpr unsigned kFrames = 216, kSize = 64;
// The luma of picture `frame` at (x, y): the scene's base and texture, and the moving square in the third scene.
unsigned Luma(unsigned frame, unsigned x, unsigned y) {
    const unsigned scene = frame < 72 ? 0 : frame < 144 ? 1 : 2;
    const unsigned base[3]{24, 196, 110};
    if (scene == 2) {
        const unsigned shown = frame & ~1u; // The square moves on the IDRs; the P pictures repeat them.
        const unsigned left = 26 + (shown - 144) * 16 / 72, top = 26;
        if (x >= left && x < left + 12 && y >= top && y < top + 12) return 235;
    }
    return base[scene] + (x * 13 + y * 7) % 24;
}
Bytes Video(unsigned frame) {
    Bits b; const bool idr = !(frame % 2);
    b.Ue(0); b.Ue(idr ? 7 : 5); b.Ue(0); b.Value(idr ? 0 : 1, 4); if (idr) b.Ue(frame / 2 % 2); b.Value(idr ? 0 : 2, 4);
    if (idr) { b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); b.Bit(0); }
    b.Ue(0); b.Ue(1);
    if (!idr) b.Ue(16);
    else for (unsigned mb = 0; mb < 16; ++mb) {
        b.Ue(25); b.Align();
        const unsigned mx = mb % 4 * 16, my = mb / 4 * 16;
        for (unsigned i = 0; i < 256; ++i) b.Value(Luma(frame, mx + i % 16, my + i / 16), 8);
        for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
    }
    Writer w; const auto nal = b.Nal(idr ? 0x65 : 0x41); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
}
void Put16(Bytes& out, int v, bool little) {
    const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(v));
    if (little) { out.push_back(static_cast<std::uint8_t>(u)); out.push_back(static_cast<std::uint8_t>(u >> 8)); }
    else { out.push_back(static_cast<std::uint8_t>(u >> 8)); out.push_back(static_cast<std::uint8_t>(u)); }
}
// 1024-frame samples of a track `seconds` long (the last one shorter).
std::vector<Bytes> Tone(unsigned rate, unsigned channels, double seconds, std::vector<unsigned>& deltas, double (*value)(double)) {
    const auto total = static_cast<unsigned>(seconds * rate);
    std::vector<Bytes> samples;
    for (unsigned at = 0; at < total; at += 1024) {
        const unsigned n = total - at < 1024 ? total - at : 1024;
        Bytes s;
        for (unsigned i = 0; i < n; ++i) {
            const int v = static_cast<int>(std::lround(value(static_cast<double>(at + i) / rate) * 32767.0));
            for (unsigned c = 0; c < channels; ++c) Put16(s, v, true);
        }
        samples.push_back(s); deltas.push_back(n);
    }
    return samples;
}
double English(double t) { return 0.25 * std::sin(2 * kPi * 440 * t) + 0.06 * std::sin(2 * kPi * 4000 * t); }
double French(double t) { return 0.3 * std::sin(2 * kPi * 660 * t); }
struct Track {
    unsigned id; const char* handler; const char* entry; const char* language; bool enabled; unsigned group, timescale, rate, channels;
    std::vector<unsigned> sizes, offsets, deltas; bool sync_every_other = false;
};
void Matrix(Writer& w) { w.U32(0x10000); w.Zero(12); w.U32(0x10000); w.Zero(12); w.U32(0x40000000); }
void Trak(Writer& w, const Track& s) {
    const auto count = static_cast<unsigned>(s.sizes.size());
    unsigned duration = 0; for (auto d : s.deltas) duration += d;
    const bool video = std::string_view(s.handler) == "vide";
    const auto track = w.Box("trak"); auto box = w.Box("tkhd"); w.U32(s.enabled ? 3 : 2); w.Zero(8); w.U32(s.id); w.U32(0);
    w.U32(static_cast<unsigned>(static_cast<std::uint64_t>(duration) * 1000 / s.timescale)); w.Zero(8);
    w.U16(0); w.U16(s.group); w.U16(video ? 0 : 0x100); w.U16(0); Matrix(w); w.U32(video ? kSize << 16 : 0); w.U32(video ? kSize << 16 : 0); w.End(box);
    const auto mdia = w.Box("mdia"); box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(s.timescale); w.U32(duration);
    w.U16(static_cast<unsigned>(((s.language[0] - 0x60) << 10) | ((s.language[1] - 0x60) << 5) | (s.language[2] - 0x60))); w.U16(0); w.End(box);
    box = w.Box("hdlr"); w.Zero(8); for (char c : std::string_view(s.handler)) w.B(static_cast<unsigned char>(c)); w.Zero(12); w.B(0); w.End(box);
    const auto minf = w.Box("minf");
    const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
    const auto stbl = w.Box("stbl"); box = w.Box("stsd"); w.U32(0); w.U32(1);
    const auto entry = w.Box(s.entry); w.Zero(6); w.U16(1);
    const std::string_view type{s.entry};
    if (type == "avc1") {
        w.Zero(16); w.U16(kSize); w.U16(kSize); w.U32(0x480000); w.U32(0x480000); w.U32(0); w.U16(1); w.Zero(32); w.U16(24); w.U16(0xffff);
        const auto avcc = w.Box("avcC"); const auto sps = Sps(), pps = Pps();
        w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(static_cast<unsigned>(sps.size())); w.Data(sps); w.B(1); w.U16(static_cast<unsigned>(pps.size())); w.Data(pps); w.End(avcc);
    } else {
        // AudioSampleEntry version 0: channels, 16 bits, rate; `ipcm` says its format in `pcmC`, `sowt` in its name.
        w.Zero(8); w.U16(s.channels); w.U16(16); w.Zero(4); w.U32(s.rate << 16);
        if (type == "ipcm") { const auto config = w.Box("pcmC"); w.U32(0); w.B(1); w.B(16); w.End(config); }
    }
    w.End(entry); w.End(box);
    box = w.Box("stts"); w.U32(0); w.U32(count); for (auto d : s.deltas) { w.U32(1); w.U32(d); } w.End(box);
    box = w.Box("stsc"); w.U32(0); w.U32(1); w.U32(1); w.U32(1); w.U32(1); w.End(box);
    box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count); for (const auto size : s.sizes) w.U32(size); w.End(box);
    box = w.Box("stco"); w.U32(0); w.U32(count); for (auto offset : s.offsets) w.U32(offset); w.End(box);
    if (s.sync_every_other) { box = w.Box("stss"); w.U32(0); w.U32((count + 1) / 2); for (unsigned i = 0; i < count; i += 2) w.U32(i + 1); w.End(box); }
    w.End(stbl); w.End(minf); w.End(mdia); w.End(track);
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::puts("Usage: media_make_portable_fixture out.mp4"); return 2; }
    Track v{1, "vide", "avc1", "und", true, 0, 48000, 0, 0, {}, {}, {}, true};
    Track e{2, "soun", "ipcm", "eng", true, 1, 48000, 48000, 2, {}, {}, {}};
    Track f{3, "soun", "sowt", "fra", false, 1, 44100, 44100, 1, {}, {}, {}};
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) { video.push_back(Video(i)); v.deltas.push_back(2000); }
    const auto english = Tone(48000, 2, 9.0, e.deltas, English);
    const auto french = Tone(44100, 1, 9.0, f.deltas, French);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0x200); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    // The samples in time order, as a muxer interleaves them.
    box = w.Box("mdat");
    std::size_t vi = 0, ei = 0, fi = 0;
    const auto time = [](std::size_t index, double per) { return static_cast<double>(index) * per; };
    while (vi < video.size() || ei < english.size() || fi < french.size()) {
        const double tv = vi < video.size() ? time(vi, 1.0 / 24) : 1e9, te = ei < english.size() ? time(ei, 1024.0 / 48000) : 1e9,
                     tf = fi < french.size() ? time(fi, 1024.0 / 44100) : 1e9;
        Track* t; const Bytes* s;
        if (tv <= te && tv <= tf) { t = &v; s = &video[vi++]; } else if (te <= tf) { t = &e; s = &english[ei++]; } else { t = &f; s = &french[fi++]; }
        t->offsets.push_back(static_cast<unsigned>(w.b.size())); t->sizes.push_back(static_cast<unsigned>(s->size())); w.Data(*s);
    }
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(9000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(4); w.End(box);
    Trak(w, v); Trak(w, e); Trak(w, f);
    const char* titles[]{"Opening", "Bright", "Moving"};
    const auto user = w.Box("udta"); box = w.Box("chpl"); w.U32(0x01000000); w.U32(0); w.B(3);
    for (unsigned c = 0; c < 3; ++c) {
        const std::uint64_t start = c * 30000000ull; w.U32(static_cast<unsigned>(start >> 32)); w.U32(static_cast<unsigned>(start));
        const auto n = static_cast<unsigned>(std::strlen(titles[c])); w.B(n); for (unsigned k = 0; k < n; ++k) w.B(static_cast<unsigned char>(titles[c][k]));
    }
    w.End(box); w.End(user);
    w.End(moov);
#if defined(_MSC_VER)
    FILE* out = nullptr; if (fopen_s(&out, argv[1], "wb") != 0) out = nullptr;
#else
    FILE* out = std::fopen(argv[1], "wb");
#endif
    if (!out) { std::printf("Cannot write %s\n", argv[1]); return 1; }
    const bool good = std::fwrite(w.b.data(), 1, w.b.size(), out) == w.b.size();
    std::fclose(out);
    std::printf("Portable fixture: %zu pictures (I_PCM and skipped), %zu English and %zu French PCM samples, chapters at 0/3/6 s, %zu bytes\n",
                video.size(), english.size(), french.size(), w.b.size());
    return good ? 0 : 1;
}
