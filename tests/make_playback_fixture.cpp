// Offline original fixture authoring; these allocations occur before playback.
#include "platform_hevc_encoder.hpp"
#include "horizon_scene.hpp"
#include "media/codec/HevcSyntaxParser.hpp"
#include "media/codec/H264SyntaxParser.hpp"
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string_view>
#include <cwchar>
#include <cstring>
#include <algorithm>
using Bytes = std::vector<std::uint8_t>;
namespace {
void Hr(HRESULT hr) { if (FAILED(hr)) { std::printf("Fixture native failure %08lx\n", static_cast<unsigned long>(hr)); ExitProcess(1); } }
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
// --anamorphic: the same 64x64 coded pictures described as a 60x44 crop of non-square samples. The SPS carries the
// crop (4 columns off the right, 20 rows off the bottom), a VUI with sample aspect ratio 4:3 (aspect_ratio_idc 14) and
// a BT.601 colour description (primaries 5, transfer 6, matrix 5, limited range); the sample entry carries a 1:1
// `pasp` (what muxers write by default) and an nclx `colr` saying BT.709. Shown as 80x44, converted as BT.709.
bool g_anamorphic = false;
// --pacing24/30/60: each picture lasts this many 1/48000 s (2000: 24 fps).
unsigned g_video_delta = 2000;
// --rotate90 and the others: the SPS crops this many pairs of rows off the bottom (16: a 64x32 picture).
unsigned g_crop_rows = 0;
// --horizon: the picture is this many macroblocks across and down (4 x 4 otherwise: 64x64).
unsigned g_mbs_wide = 4, g_mbs_high = 4;
Bytes Sps() {
    Bits b; b.Value(66, 8); b.Value(0, 8); b.Value(30, 8); b.Ue(0); b.Ue(0); b.Ue(0); b.Ue(0);
    b.Ue(1); b.Bit(0); b.Ue(g_mbs_wide - 1); b.Ue(g_mbs_high - 1); b.Bit(1); b.Bit(1);
    if (g_crop_rows) { b.Bit(1); b.Ue(0); b.Ue(0); b.Ue(0); b.Ue(g_crop_rows); b.Bit(0); return b.Nal(0x67); } // Cropped, no VUI.
    if (!g_anamorphic) { b.Bit(0); b.Bit(0); return b.Nal(0x67); }
    b.Bit(1); b.Ue(0); b.Ue(2); b.Ue(0); b.Ue(10);                  // Crop offsets in units of two luma samples.
    b.Bit(1);                                                        // VUI.
    b.Bit(1); b.Value(14, 8);                                        // aspect_ratio_idc 14: 4:3.
    b.Bit(0);                                                        // No overscan information.
    b.Bit(1); b.Value(5, 3); b.Bit(0); b.Bit(1); b.Value(5, 8); b.Value(6, 8); b.Value(5, 8); // Signal type: limited range, 5/6/5.
    for (unsigned i = 0; i < 6; ++i) b.Bit(0);                       // Chroma location, timing, NAL HRD, VCL HRD, pic_struct, restrictions.
    return b.Nal(0x67);
}
Bytes Pps() {
    Bits b; b.Ue(0); b.Ue(0); b.Bit(0); b.Bit(0); b.Ue(0); b.Ue(0); b.Ue(0);
    b.Bit(0); b.Value(0, 2); b.Ue(0); b.Ue(0); b.Ue(0); b.Bit(1); b.Bit(0); b.Bit(0); return b.Nal(0x68);
}
// `scene`: 0 for the historic pattern; otherwise the base luma of a scene (flat, with a small texture), so cuts between
// scenes are hard luma jumps (the chapter fixture).
Bytes Video(unsigned frame, unsigned scene = 0) {
    Bits b; const bool idr = !(frame % 2);
    b.Ue(0); b.Ue(idr ? 2 : 0); b.Ue(0); b.Value(idr ? 0 : 1, 4); if (idr) b.Ue(0); b.Value(idr ? 0 : 2, 4);
    if (idr) { b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); b.Bit(0); }
    b.Ue(0); b.Ue(1);
    if (!idr) b.Ue(16);
    else for (unsigned mb = 0; mb < 16; ++mb) {
        b.Ue(25); b.Align();
        for (unsigned i = 0; i < 256; ++i) b.Value(scene ? scene + (mb * 13 + frame * 3 + i % 16) % 24 : 32 + ((mb * 13 + frame * 3 + i % 16) % 190), 8);
        for (unsigned i = 0; i < 64; ++i) b.Value(80 + (frame * 2 % 96), 8);
        for (unsigned i = 0; i < 64; ++i) b.Value(170 - (frame % 80), 8);
    }
    Writer w; const auto nal = b.Nal(idr ? 0x65 : 0x41); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
}
// --audio-only: the stereo tone's frequency from 0, 3 and 6 s (the music fixture's chapters); null: one frequency.
const double* g_steps = nullptr;
// --saccade: a quiet noise bed with a 60 ms noise burst on the right channel only at 2 s and on the left only at 4 s.
bool g_bursts = false;
// Per-speaker test tones in WAVEFORMATEXTENSIBLE order (FL FR FC LFE BL BR SL SR), so routing is checkable.
constexpr double kSpeakerHz[8]{300, 500, 700, 60, 1100, 1300, 1700, 1900};
// Native AAC-LC encode. `channels` 1, 2 or 6; stereo keeps the historic 440 Hz on both channels unless `tones`.
// `first` offsets the per-speaker tone table (to encode one pair of a larger layout).
// `rate`, `hz`: the stereo tone's sample rate and frequency; `marks`: silent for 10 ms from every 250 ms (the
// multi-track fixture's time marks, docs/tracks.md).
std::vector<Bytes> Audio(unsigned count, unsigned channels = 2, bool tones = false, unsigned first = 0, Bytes* asc = nullptr,
                         unsigned rate = 48000, double hz_stereo = 440.0, bool marks = false) {
    IMFTransform* encoder = nullptr; Hr(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)));
    for (unsigned output = 0; output != 2; ++output) {
        IMFMediaType* type = nullptr; Hr(MFCreateMediaType(&type));
        Hr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)); Hr(type->SetGUID(MF_MT_SUBTYPE, output ? MFAudioFormat_AAC : MFAudioFormat_PCM));
        Hr(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate)); Hr(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels)); Hr(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        if (channels == 6) Hr(type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, 0x3f));
        if (output) { Hr(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, channels == 6 ? 24000 : 16000)); Hr(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)); Hr(encoder->SetOutputType(0, type, 0)); }
        else { Hr(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2)); Hr(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, channels * 2 * rate)); Hr(encoder->SetInputType(0, type, 0)); }
        type->Release();
    }
    if (asc) {
        IMFMediaType* current = nullptr; Hr(encoder->GetOutputCurrentType(0, &current)); UINT32 size = 0; Hr(current->GetBlobSize(MF_MT_USER_DATA, &size));
        Bytes user(size); Hr(current->GetBlob(MF_MT_USER_DATA, user.data(), size, nullptr)); current->Release();
        asc->assign(user.begin() + 12, user.end()); // After the 12-byte HEAACWAVEINFO tail.
    }
    Hr(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)); Hr(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    std::vector<Bytes> packets;
    auto output = [&] {
        for (;;) {
            IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
            Hr(MFCreateSample(&sample)); Hr(MFCreateMemoryBuffer(16384, &buffer)); Hr(sample->AddBuffer(buffer));
            MFT_OUTPUT_DATA_BUFFER result{0, sample, 0, nullptr}; DWORD flags = 0; const auto hr = encoder->ProcessOutput(0, 1, &result, &flags);
            if (result.pEvents) result.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { buffer->Release(); sample->Release(); return; }
            Hr(hr); BYTE* data = nullptr; DWORD length = 0; Hr(buffer->Lock(&data, nullptr, &length));
            packets.emplace_back(data, data + length); Hr(buffer->Unlock()); buffer->Release(); sample->Release();
        }
    };
    for (unsigned frame = 0; frame < count; ++frame) {
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
        Hr(MFCreateSample(&sample)); Hr(MFCreateMemoryBuffer(1024 * channels * 2, &buffer)); Hr(sample->AddBuffer(buffer));
        BYTE* bytes = nullptr; Hr(buffer->Lock(&bytes, nullptr, nullptr)); auto* pcm = reinterpret_cast<std::int16_t*>(bytes);
        for (unsigned i = 0; i < 1024; ++i) {
            const unsigned index = frame * 1024 + i;
            const double time = static_cast<double>(index) / rate;
            const bool silent = marks && index % (rate / 4) < rate / 100; // 10 ms from every 250 ms.
            for (unsigned c = 0; c < channels; ++c) {
                const double stepped = g_steps ? g_steps[time < 3 ? 0 : time < 6 ? 1 : 2] : hz_stereo;
                const double hz = tones ? kSpeakerHz[first + c] : stepped, amplitude = tones || marks || g_steps ? 4000.0 : 400.0;
                pcm[i * channels + c] = silent ? std::int16_t{0} : static_cast<std::int16_t>(amplitude * std::sin(time * hz * 6.283185307179586));
                if (g_bursts) {
                    std::uint32_t h = index * 2654435761u + c * 40503u; h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
                    const double noise = static_cast<double>(h % 2001u) / 1000.0 - 1.0;
                    const bool burst = (c == 1 && time >= 2.0 && time < 2.06) || (c == 0 && time >= 4.0 && time < 4.06);
                    pcm[i * channels + c] = static_cast<std::int16_t>(noise * (burst ? 8000.0 : 20.0));
                }
            }
        }
        Hr(buffer->Unlock()); Hr(buffer->SetCurrentLength(1024 * channels * 2));
        Hr(sample->SetSampleTime(static_cast<LONGLONG>(frame) * 1024 * 10000000 / rate)); Hr(sample->SetSampleDuration(1024ll * 10000000 / rate));
        Hr(encoder->ProcessInput(0, sample, 0)); buffer->Release(); sample->Release(); output();
    }
    Hr(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)); Hr(encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)); output(); encoder->Release(); return packets;
}
// 7.1 AAC-LC. The platform encoder refuses 8-channel LC and its frames cannot be split into elements without a
// Huffman-level parser, so the 7.1 fixture is built from silent but valid elements (ONLY_LONG window, max_sfb 0: no
// section, scale-factor or spectral data); the same construction decodes frame for frame on the platform decoder
// in its 5.1 form (media_aac_surround_tests). A PCE describes the layout: front SCE 0 + CPE 0, side CPE 1, back
// CPE 2, LFE 0; each frame carries SCE 0, CPE 0, CPE 1, CPE 2, LFE 0 and ID_END.
Bytes Pce71() {
    Bits b; b.Value(2, 5); b.Value(3, 4); b.Value(0, 4); b.Value(0, 3);
    b.Value(0, 4); b.Value(1, 2); b.Value(3, 4); b.Value(2, 4); b.Value(1, 4); b.Value(1, 4); b.Value(1, 2); b.Value(0, 3); b.Value(0, 4);
    b.Bit(0); b.Bit(0); b.Bit(0);
    b.Bit(0); b.Value(0, 4); b.Bit(1); b.Value(0, 4); // Front: SCE 0 (centre), CPE 0 (left/right).
    b.Bit(1); b.Value(1, 4);                          // Side: CPE 1.
    b.Bit(1); b.Value(2, 4);                          // Back: CPE 2.
    b.Value(0, 4);                                    // LFE 0.
    b.Align(); b.Value(0, 8); return b.b;
}
std::vector<Bytes> Silent71(unsigned count) {
    auto ics = [](Bits& b) { b.Value(100, 8); b.Bit(0); b.Value(0, 2); b.Bit(0); b.Value(0, 6); b.Bit(0); b.Bit(0); b.Bit(0); b.Bit(0); };
    Bits b;
    const unsigned elements[5][2]{{0, 0}, {1, 0}, {1, 1}, {1, 2}, {3, 0}};
    for (const auto& e : elements) { b.Value(e[0], 3); b.Value(e[1], 4); if (e[0] == 1) { b.Bit(0); ics(b); ics(b); } else ics(b); }
    b.Value(7, 3); b.Align();
    return std::vector<Bytes>(count, b.b);
}
void Matrix(Writer& w) { w.U32(0x10000); w.Zero(12); w.U32(0x10000); w.Zero(12); w.U32(0x40000000); }
std::vector<unsigned> Sizes(const std::vector<Bytes>& packets) {
    std::vector<unsigned> sizes; for (const auto& p : packets) sizes.push_back(static_cast<unsigned>(p.size())); return sizes;
}
// `chunks` holds one offset per chunk of `per_chunk` samples (the last chunk may be shorter).
void Track(Writer& w, unsigned id, bool video, const std::vector<unsigned>& sizes, const std::vector<unsigned>& chunks,
           const Bytes& asc = Bytes{0x11, 0x90}, unsigned channels = 2, unsigned per_chunk = 1) {
    const auto count = static_cast<unsigned>(sizes.size());
    const unsigned duration = count * (video ? g_video_delta : 1024);
    const auto track = w.Box("trak"); auto box = w.Box("tkhd"); w.U32(7); w.Zero(8); w.U32(id); w.U32(0); w.U32(duration); w.Zero(8);
    w.U16(0); w.U16(0); w.U16(video ? 0 : 0x100); w.U16(0); Matrix(w);
    w.U32(video ? (g_anamorphic ? 80u : 64u) << 16 : 0); w.U32(video ? (g_anamorphic ? 44u : 64u) << 16 : 0); w.End(box);
    const auto mdia = w.Box("mdia"); box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(48000); w.U32(duration); w.U16(0x55c4); w.U16(0); w.End(box);
    box = w.Box("hdlr"); w.Zero(8); for (char c : video ? std::string_view("vide") : std::string_view("soun")) w.B(static_cast<unsigned char>(c)); w.Zero(13); w.End(box);
    const auto minf = w.Box("minf"); box = w.Box(video ? "vmhd" : "smhd"); w.U32(video ? 1 : 0); w.Zero(video ? 8 : 4); w.End(box);
    const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
    const auto stbl = w.Box("stbl"); box = w.Box("stsd"); w.U32(0); w.U32(1); const auto entry = w.Box(video ? "avc1" : "mp4a"); w.Zero(6); w.U16(1);
    if (video) {
        w.Zero(16); w.U16(g_anamorphic ? 60 : 64); w.U16(g_anamorphic ? 44 : 64); w.U32(0x480000); w.U32(0x480000); w.U32(0); w.U16(1); w.Zero(32); w.U16(24); w.U16(0xffff);
        const auto avcc = w.Box("avcC"); const auto sps = Sps(), pps = Pps();
        w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(static_cast<unsigned>(sps.size())); w.Data(sps); w.B(1); w.U16(static_cast<unsigned>(pps.size())); w.Data(pps); w.End(avcc);
        if (g_anamorphic) {
            auto extra = w.Box("pasp"); w.U32(1); w.U32(1); w.End(extra);
            extra = w.Box("colr"); w.U32(0x6e636c78); w.U16(1); w.U16(1); w.U16(1); w.B(0); w.End(extra);
        }
    } else {
        w.Zero(8); w.U16(channels); w.U16(16); w.Zero(4); w.U32(48000u << 16); const auto esds = w.Box("esds");
        const auto n = static_cast<unsigned>(asc.size());
        w.U32(0); w.B(3); w.B(23 + n); w.U16(id); w.B(0); w.B(4); w.B(15 + n); w.B(0x40); w.B(0x15); w.Zero(3); w.U32(128000); w.U32(128000);
        w.B(5); w.B(n); w.Data(asc); w.B(6); w.B(1); w.B(2); w.End(esds);
    }
    w.End(entry); w.End(box);
    box = w.Box("stts"); w.U32(0); w.U32(1); w.U32(count); w.U32(video ? g_video_delta : 1024); w.End(box);
    const unsigned full = count / per_chunk, rest = count % per_chunk;
    box = w.Box("stsc"); w.U32(0); w.U32(rest ? 2 : 1); w.U32(1); w.U32(per_chunk); w.U32(1);
    if (rest) { w.U32(full + 1); w.U32(rest); w.U32(1); }
    w.End(box);
    box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count); for (const auto size : sizes) w.U32(size); w.End(box);
    box = w.Box("stco"); w.U32(0); w.U32(static_cast<unsigned>(chunks.size())); for (auto offset : chunks) w.U32(offset); w.End(box);
    if (video) { box = w.Box("stss"); w.U32(0); w.U32((count + 1) / 2); for (unsigned i = 0; i < count; i += 2) w.U32(i + 1); w.End(box); }
    w.End(stbl); w.End(minf); w.End(mdia); w.End(track);
}
// ---- HEVC fixtures: platform encoder, B-pictures, ctts, and a single-entry edit list ----
struct HevcVideo {
    std::vector<Bytes> samples; std::vector<unsigned> composition, sync;
    Bytes vps, sps, pps;
    unsigned width = 0, height = 0, delay = 0, profile = 1, level = 0, depth = 0;
    unsigned trim = 0; // Displayed pictures cut by the edit list (they decode as references only).
};
void FillRow(unsigned frame, unsigned y, unsigned width, unsigned height, std::uint8_t* row) {
    const unsigned bx = (7 * frame) % (width - 64), by = (4 * frame) % (height - 64);
    for (unsigned x = 0; x < width; ++x) {
        unsigned v = 40 + ((x + 2 * y + 13 * frame) & 127);
        if (x - bx < 64 && y - by < 64) v += 60;
        row[x] = static_cast<std::uint8_t>(v);
    }
}
Bytes HevcNal(Bits& bits, unsigned type) { auto nal = bits.Nal(type << 1); nal.insert(nal.begin() + 1, 1); return nal; }
// The platform encoder ignores Media Foundation colour attributes, so HDR signalling is spliced into the SPS:
// original RBSP bits up to vui_parameters_present_flag, a new VUI whose colour description is BT.2020 /
// PQ-or-HLG / BT.2020 NCL (limited range), then the original bits after the old VUI. Slice data is untouched.
Bytes Unescape(const std::uint8_t* data, std::size_t size) {
    Bytes rbsp; unsigned zeros = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (zeros == 2 && data[i] == 3) { zeros = 0; continue; }
        rbsp.push_back(data[i]); zeros = data[i] == 0 ? zeros + 1 : 0;
    }
    return rbsp;
}
bool RewriteHdrVui(Bytes& nal, unsigned transfer) {
    media::core::LinearMemoryArena arena; media::codec::HevcSyntaxParser parser;
    if (!arena.Initialize(1024 * 1024) || !parser.Initialize(arena)) return false;
    std::vector<std::byte> view(nal.size()); std::memcpy(view.data(), nal.data(), nal.size());
    if (parser.ParameterSet({view.data(), view.size()}) != media::codec::SyntaxStatus::Ready || !parser.FirstSps()) return false;
    const auto& sps = *parser.FirstSps();
    const auto rbsp = Unescape(nal.data() + 2, nal.size() - 2);
    auto bit = [&](std::size_t i) { return (rbsp[i / 8] >> (7 - i % 8)) & 1u; };
    std::size_t stop = rbsp.size() * 8;
    while (stop && !bit(stop - 1)) --stop;
    if (!stop || sps.vui_end_bit >= stop) return false;
    --stop; // rbsp_stop_one_bit: re-appended by Nal() after the new alignment.
    Bits out;
    for (std::size_t i = 0; i < sps.vui_flag_bit; ++i) out.Bit(bit(i));
    out.Bit(1); // vui_parameters_present_flag
    out.Bit(0); out.Bit(0); // aspect ratio, overscan
    out.Bit(1); out.Value(5, 3); out.Bit(0); out.Bit(1); out.Value(9, 8); out.Value(transfer, 8); out.Value(9, 8);
    for (unsigned i = 0; i < 7; ++i) out.Bit(0); // chroma_loc .. bitstream_restriction flags.
    for (std::size_t i = sps.vui_end_bit; i < stop; ++i) out.Bit(bit(i));
    nal = HevcNal(out, 33);
    return true;
}
// Returns 0 on success, 77 when no platform HEVC encoder accepts the format, 1 on failure.
// `gop`: pictures from one IDR to the next (72: a single IDR, the whole 3 s clip is one group of pictures).
int EncodeHevc(bool ten_bit, unsigned width, unsigned height, HevcVideo& video, unsigned transfer = 0, unsigned gop = 24) {
    encoder::Options options; options.width = width; options.height = height; options.frames = 72; options.fps = 24;
    options.transfer = transfer;
    options.ten_bit = ten_bit; options.gop = gop; options.b_frames = 2; options.bitrate = width >= 3840 ? 20000000 : 6000000;
    std::vector<encoder::Encoded> stream; bool b_frames = false;
    const auto result = encoder::Encode(options, FillRow, stream, b_frames);
    if (result == encoder::Fixture::Unavailable) return 77;
    if (result != encoder::Fixture::Ready || stream.size() != 72) return 1;
    if (transfer) for (auto& e : stream) {
        std::vector<std::byte> rebuilt; bool rewritten = true;
        encoder::ForEachNal(e.au, [&](std::span<const std::byte> nal) {
            Bytes bytes(nal.size()); std::memcpy(bytes.data(), nal.data(), nal.size());
            if (((bytes[0] >> 1) & 63) == 33) rewritten = rewritten && RewriteHdrVui(bytes, transfer);
            for (unsigned i = 4; i-- > 0;) rebuilt.push_back(static_cast<std::byte>(bytes.size() >> (8 * i)));
            for (auto b : bytes) rebuilt.push_back(static_cast<std::byte>(b));
        });
        if (!rewritten) { std::puts("FAIL SPS VUI rewrite"); return 1; }
        e.au = std::move(rebuilt);
    }
    // Presentation order comes from the bitstream (the MFT labels samples in decode order).
    // The encoder emits closed GOPs, so display index = IDR decode index + POC.
    media::core::LinearMemoryArena arena; media::codec::HevcSyntaxParser parser;
    if (!arena.Initialize(4 * 1024 * 1024) || !parser.Initialize(arena)) return 1;
    std::vector<unsigned> display; unsigned base = 0;
    for (unsigned i = 0; i < stream.size(); ++i) {
        const auto& au = stream[i].au;
        if (i == 0) encoder::ForEachNal(au, [&](std::span<const std::byte> nal) {
            const auto type = (std::to_integer<unsigned>(nal[0]) >> 1) & 63;
            auto* target = type == 32 ? &video.vps : type == 33 ? &video.sps : type == 34 ? &video.pps : nullptr;
            if (target && target->empty()) for (auto b : nal) target->push_back(std::to_integer<std::uint8_t>(b));
        });
        if (parser.Parse({au.data(), au.size()}, 4) != media::codec::SyntaxStatus::Ready) return 1;
        const auto* picture = parser.Picture();
        if (picture->idr) base = i;
        if (picture->poc < 0) return 1;
        display.push_back(base + static_cast<unsigned>(picture->poc));
        if (picture->irap) video.sync.push_back(i + 1);
        media::platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
        if (!parser.Commit({i, 1}, evicted, count)) return 1;
        Bytes sample(au.size()); std::memcpy(sample.data(), au.data(), au.size()); video.samples.push_back(std::move(sample));
    }
    std::vector<bool> seen(display.size());
    for (unsigned i = 0; i < display.size(); ++i) {
        if (display[i] >= display.size() || seen[display[i]]) return 1; // Must be a permutation of 0..N-1.
        seen[display[i]] = true;
        if (i > display[i]) video.delay = (std::max)(video.delay, i - display[i]);
    }
    for (unsigned i = 0; i < display.size(); ++i) video.composition.push_back((display[i] + video.delay - i) * 2000);
    const auto* sps = parser.FirstSps();
    if (!sps || video.vps.empty() || video.sps.empty() || video.pps.empty()) return 1;
    video.width = sps->right - sps->left; video.height = sps->bottom - sps->top;
    video.profile = sps->profile; video.level = sps->level; video.depth = sps->bit_depth_luma_minus8;
    std::printf("HEVC fixture VUI: primaries %u, transfer %u, matrix %u, %s range\n", sps->colour_primaries, sps->transfer, sps->matrix,
                sps->full_range ? "full" : "limited");
    if (transfer && (sps->transfer != transfer || sps->colour_primaries != 9 || sps->matrix != 9)) {
        std::puts("FAIL the platform encoder did not signal the requested HDR colour description"); return 1;
    }
    std::printf("HEVC fixture: %ux%u %u-bit, %zu pictures, %zu sync sample(s), reorder delay %u frame(s), B pictures %s\n", video.width, video.height,
                8 + 2 * (video.depth / 2), video.samples.size(), video.sync.size(), video.delay, b_frames ? "requested" : "refused");
    if (video.sync.size() != (72 + gop - 1) / gop) { std::printf("FAIL the encoder made %zu sync samples for a %u-picture GOP\n", video.sync.size(), gop); return 1; }
    return 0;
}
// H.264 with B-frames from the platform encoder (hardware first: a B-pyramid with reference B-pictures, MMCO and
// list modification on Intel; `software`: the Microsoft CAVLC encoder, IBBP). Presentation order is the POC rank inside
// each IDR period (closed GOPs), computed by the engine's own parser.
int EncodeAvc(bool software, HevcVideo& video) {
    encoder::Options options; options.h264 = true; options.software = software; options.width = 1280; options.height = 720;
    options.frames = 72; options.fps = 24; options.gop = 24; options.b_frames = software ? 2 : 3; options.bitrate = 6000000;
    std::vector<encoder::Encoded> stream; bool b_frames = false;
    const auto result = encoder::Encode(options, FillRow, stream, b_frames);
    if (result == encoder::Fixture::Unavailable) return 77;
    if (result != encoder::Fixture::Ready || stream.size() != 72) return 1;
    media::core::LinearMemoryArena arena; media::codec::H264SyntaxParser parser;
    if (!arena.Initialize(4 * 1024 * 1024) || !parser.Initialize(arena)) return 1;
    std::vector<int> poc; std::vector<bool> idr; unsigned b_pictures = 0;
    for (unsigned i = 0; i < stream.size(); ++i) {
        const auto& au = stream[i].au;
        if (i == 0) encoder::ForEachNal(au, [&](std::span<const std::byte> nal) {
            const auto type = std::to_integer<unsigned>(nal[0]) & 31;
            auto* target = type == 7 ? &video.sps : type == 8 ? &video.pps : nullptr;
            if (target && target->empty()) for (auto b : nal) target->push_back(std::to_integer<std::uint8_t>(b));
        });
        if (parser.Parse({au.data(), au.size()}, 4) != media::codec::SyntaxStatus::Ready) return 1;
        const auto* picture = parser.Picture();
        poc.push_back(picture->top_poc); idr.push_back(picture->idr);
        if (picture->idr) video.sync.push_back(i + 1);
        b_pictures += picture->bidirectional ? 1u : 0u;
        media::platform::SurfaceHandle evicted[16]{}; unsigned count = 0;
        if (!parser.Commit({i, 1}, evicted, count)) return 1;
        Bytes sample(au.size()); std::memcpy(sample.data(), au.data(), au.size()); video.samples.push_back(std::move(sample));
    }
    std::vector<unsigned> display(stream.size());
    for (unsigned start = 0; start < stream.size();) {
        unsigned end = start + 1; while (end < stream.size() && !idr[end]) ++end;
        for (unsigned i = start; i < end; ++i) {
            unsigned rank = 0; for (unsigned j = start; j < end; ++j) rank += poc[j] < poc[i] ? 1u : 0u;
            display[i] = start + rank;
        }
        start = end;
    }
    for (unsigned i = 0; i < display.size(); ++i) if (i > display[i]) video.delay = (std::max)(video.delay, i - display[i]);
    for (unsigned i = 0; i < display.size(); ++i) video.composition.push_back((display[i] + video.delay - i) * 2000);
    const auto* sps = parser.FirstSps();
    if (!sps || video.sps.empty() || video.pps.empty() || !b_pictures) return 1;
    video.width = sps->right - sps->left; video.height = sps->bottom - sps->top; video.profile = sps->profile;
    std::printf("H.264 fixture: %ux%u profile %u, %zu pictures (%u B), reorder depth %u, delay %u frame(s), %s encoder\n",
                video.width, video.height, video.profile, video.samples.size(), b_pictures, sps->reorder, video.delay,
                software ? "software" : "platform (hardware first)");
    return 0;
}
void HevcTrack(Writer& w, const HevcVideo& v, const std::vector<unsigned>& offsets, bool avc = false) {
    const auto count = static_cast<unsigned>(v.samples.size()), duration = (count - v.trim) * 2000;
    const auto track = w.Box("trak"); auto box = w.Box("tkhd"); w.U32(7); w.Zero(8); w.U32(1); w.U32(0); w.U32(duration); w.Zero(8);
    w.U16(0); w.U16(0); w.U16(0); w.U16(0); Matrix(w); w.U32(v.width << 16); w.U32(v.height << 16); w.End(box);
    // B-picture reordering delays composition by `delay` frames; the edit list starts presentation at PTS 0.
    const auto edits = w.Box("edts"); box = w.Box("elst"); w.U32(0); w.U32(1); w.U32(duration); w.U32((v.delay + v.trim) * 2000); w.U32(0x10000); w.End(box); w.End(edits);
    const auto mdia = w.Box("mdia"); box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(48000); w.U32(duration); w.U16(0x55c4); w.U16(0); w.End(box);
    box = w.Box("hdlr"); w.Zero(8); for (char c : std::string_view("vide")) w.B(static_cast<unsigned char>(c)); w.Zero(13); w.End(box);
    const auto minf = w.Box("minf"); box = w.Box("vmhd"); w.U32(1); w.Zero(8); w.End(box);
    const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
    const auto stbl = w.Box("stbl"); box = w.Box("stsd"); w.U32(0); w.U32(1);
    const auto entry = w.Box(avc ? "avc1" : "hev1"); w.Zero(6); w.U16(1); // Parameter sets also stay in band.
    w.Zero(16); w.U16(v.width); w.U16(v.height); w.U32(0x480000); w.U32(0x480000); w.U32(0); w.U16(1); w.Zero(32); w.U16(24); w.U16(0xffff);
    if (avc) {
        // avcC: one SPS, one PPS, 4-byte NAL lengths; High profile adds 4:2:0 8-bit and no SPS extensions.
        const auto avcc = w.Box("avcC");
        w.B(1); w.B(v.sps[1]); w.B(v.sps[2]); w.B(v.sps[3]); w.B(0xff); w.B(0xe1);
        w.U16(static_cast<unsigned>(v.sps.size())); w.Data(v.sps); w.B(1); w.U16(static_cast<unsigned>(v.pps.size())); w.Data(v.pps);
        if (v.profile == 100) { w.B(0xfd); w.B(0xf8); w.B(0xf8); w.B(0); }
        w.End(avcc); w.End(entry); w.End(box);
    } else {
    const auto hvcc = w.Box("hvcC");
    w.B(1); w.B(v.profile); w.U32(1u << (31 - v.profile)); w.Zero(6); w.B(v.level);
    w.U16(0xf000); w.B(0xfc); w.B(0xfd); w.B(0xf8 | v.depth); w.B(0xf8 | v.depth); w.U16(0);
    w.B(0x0f); // One temporal layer, nested, 4-byte NAL lengths.
    w.B(3);
    for (const auto* nal : {&v.vps, &v.sps, &v.pps}) { w.B(0x80 | ((*nal)[0] >> 1)); w.U16(1); w.U16(static_cast<unsigned>(nal->size())); w.Data(*nal); }
    w.End(hvcc); w.End(entry); w.End(box);
    }
    box = w.Box("stts"); w.U32(0); w.U32(1); w.U32(count); w.U32(2000); w.End(box);
    box = w.Box("ctts"); w.U32(0); w.U32(count); for (auto offset : v.composition) { w.U32(1); w.U32(offset); } w.End(box);
    box = w.Box("stsc"); w.U32(0); w.U32(1); w.U32(1); w.U32(1); w.U32(1); w.End(box);
    box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count); for (const auto& p : v.samples) w.U32(static_cast<unsigned>(p.size())); w.End(box);
    box = w.Box("stco"); w.U32(0); w.U32(count); for (auto offset : offsets) w.U32(offset); w.End(box);
    box = w.Box("stss"); w.U32(0); w.U32(static_cast<unsigned>(v.sync.size())); for (auto s : v.sync) w.U32(s); w.End(box);
    w.End(stbl); w.End(minf); w.End(mdia); w.End(track);
}
bool Save(const wchar_t* path, const Bytes& bytes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const auto good = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr); CloseHandle(file);
    return good && written == bytes.size();
}
}
// 3 h 30 min 4 s of H.264 and AAC-LC in about 4.5 MB. One 2-second cycle of 48 pictures (an IDR every 2) and one 3-second cycle
// of 141 AAC access units are stored once; every chunk of the sample tables (24 pictures, 47 access units) points into
// a cycle, so the tables hold 302,496 pictures and 590,812 access units, the length of a feature film. The picture
// content repeats every 2 s and the tone restarts every 3 s, which the decoders accept like any stream.
int LongForm(const wchar_t* path) {
    constexpr unsigned kSeconds = 12604, kFrames = kSeconds * 24, kUnits = kSeconds * 48000 / 1024;
    std::vector<Bytes> video; for (unsigned i = 0; i < 48; ++i) video.push_back(Video(i));
    const auto audio = Audio(141);
    if (audio.size() != 141) return 1;
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat"); std::vector<unsigned> video_at, audio_at;
    for (const auto& p : video) { video_at.push_back(static_cast<unsigned>(w.b.size())); w.Data(p); }
    for (const auto& p : audio) { audio_at.push_back(static_cast<unsigned>(w.b.size())); w.Data(p); } w.End(box);
    std::vector<unsigned> video_sizes, audio_sizes, video_chunks, audio_chunks;
    for (unsigned i = 0; i < kFrames; ++i) video_sizes.push_back(static_cast<unsigned>(video[i % 48].size()));
    for (unsigned i = 0; i < kUnits; ++i) audio_sizes.push_back(static_cast<unsigned>(audio[i % 141].size()));
    for (unsigned i = 0; i < kFrames; i += 24) video_chunks.push_back(video_at[i % 48]);
    for (unsigned i = 0; i < kUnits; i += 47) audio_chunks.push_back(audio_at[i % 141]);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(48000); w.U32(kUnits * 1024);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    Track(w, 1, true, video_sizes, video_chunks, Bytes{0x11, 0x90}, 2, 24);
    Track(w, 2, false, audio_sizes, audio_chunks, Bytes{0x11, 0x90}, 2, 47); w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Long-form fixture: %u s, %u pictures, %u AAC AUs, %zu bytes\n", kSeconds, kFrames, kUnits, w.b.size());
    return good ? 0 : 1;
}
// --pacing24, --pacing30, --pacing60 (docs/session.md, "Low-power pacing"): 12 s of the 64x64 H.264 pictures at that rate and
// AAC-LC, stored once as the long-form fixture's cycles are.
int Pacing(const wchar_t* path, unsigned fps) {
    const unsigned seconds = 12, frames = seconds * fps, units = seconds * 48000 / 1024;
    g_video_delta = 48000 / fps;
    std::vector<Bytes> video; for (unsigned i = 0; i < 48; ++i) video.push_back(Video(i));
    const auto audio = Audio(141);
    if (audio.size() != 141) return 1;
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat"); std::vector<unsigned> video_at, audio_at;
    for (const auto& v : video) { video_at.push_back(static_cast<unsigned>(w.b.size())); w.Data(v); }
    for (const auto& a : audio) { audio_at.push_back(static_cast<unsigned>(w.b.size())); w.Data(a); } w.End(box);
    std::vector<unsigned> video_sizes, audio_sizes, video_chunks, audio_chunks;
    for (unsigned i = 0; i < frames; ++i) video_sizes.push_back(static_cast<unsigned>(video[i % 48].size()));
    for (unsigned i = 0; i < units; ++i) audio_sizes.push_back(static_cast<unsigned>(audio[i % 141].size()));
    for (unsigned i = 0; i < frames; i += 24) video_chunks.push_back(video_at[i % 48]);
    for (unsigned i = 0; i < units; i += 47) audio_chunks.push_back(audio_at[i % 141]);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(48000); w.U32(units * 1024);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    Track(w, 1, true, video_sizes, video_chunks, Bytes{0x11, 0x90}, 2, 24);
    Track(w, 2, false, audio_sizes, audio_chunks, Bytes{0x11, 0x90}, 2, 47); w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Pacing fixture: %u s at %u fps, %u pictures, %u AAC AUs, %zu bytes\n", seconds, fps, frames, units, w.b.size());
    return good ? 0 : 1;
}
// --multitrack (docs/tracks.md): 12 s of the 64x64 H.264 pictures with nine tracks, as a release with dubs and
// subtitles has them, plus two the engine cannot use:
//   1 video; 2 AAC English 440 Hz (the default); 3 AAC Hebrew 660 Hz; 4 AAC French 880 Hz at 44.1 kHz (another rate);
//   5 AC-3 Spanish (refused codec); 6 tx3g Hebrew; 7 tx3g English (neither shown by default); 8 a timecode track;
//   9 AAC Arabic 5.1 at 48 kHz, 550 Hz on every speaker (another layout).
// Every AAC track is silent for 10 ms from every 250 ms of media time: marks that let a test see where each spliced
// track's audio lands. Audio tracks share alternate group 1, subtitles group 2.
struct TrackSpec {
    unsigned id = 0; const char* handler = "soun"; const char* entry = "mp4a"; const char* language = "und"; const char* name = "";
    bool enabled = false; unsigned group = 0, timescale = 48000, delta = 1024, rate = 48000;
    std::vector<unsigned> sizes, offsets, deltas; Bytes asc{0x11, 0x90}; bool sync_every_other = false;
    unsigned sync_every = 0; // Every this many samples a sync sample (sync_every_other is 2).
    unsigned chapter = 0; // tref 'chap': the id of this track's QuickTime chapter track.
    const int* matrix = nullptr; // tkhd's a, b, c, d (16.16), when not the identity.
};
void TrackEx(Writer& w, const TrackSpec& s) {
    const auto count = static_cast<unsigned>(s.sizes.size());
    unsigned duration = 0; for (unsigned i = 0; i < count; ++i) duration += s.deltas.empty() ? s.delta : s.deltas[i];
    const bool video = std::string_view(s.handler) == "vide";
    const auto track = w.Box("trak"); auto box = w.Box("tkhd"); w.U32(s.enabled ? 3 : 2); w.Zero(8); w.U32(s.id); w.U32(0); w.U32(duration); w.Zero(8);
    w.U16(0); w.U16(s.group); w.U16(video ? 0 : 0x100); w.U16(0);
    if (s.matrix) { w.U32(static_cast<unsigned>(s.matrix[0])); w.U32(static_cast<unsigned>(s.matrix[1])); w.U32(0); w.U32(static_cast<unsigned>(s.matrix[2]));
                    w.U32(static_cast<unsigned>(s.matrix[3])); w.U32(0); w.U32(0); w.U32(0); w.U32(0x40000000); }
    else Matrix(w);
    w.U32(video ? g_mbs_wide * 16u << 16 : 0); w.U32(video ? g_mbs_high * 16u << 16 : 0); w.End(box);
    if (s.chapter) { const auto references = w.Box("tref"); box = w.Box("chap"); w.U32(s.chapter); w.End(box); w.End(references); }
    const auto mdia = w.Box("mdia"); box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(s.timescale); w.U32(duration);
    w.U16(static_cast<unsigned>(((s.language[0] - 0x60) << 10) | ((s.language[1] - 0x60) << 5) | (s.language[2] - 0x60))); w.U16(0); w.End(box);
    box = w.Box("hdlr"); w.Zero(8); for (char c : std::string_view(s.handler)) w.B(static_cast<unsigned char>(c)); w.Zero(12);
    for (const char* c = s.name; *c; ++c) w.B(static_cast<unsigned char>(*c)); w.B(0); w.End(box);
    const auto minf = w.Box("minf");
    const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
    const auto stbl = w.Box("stbl"); box = w.Box("stsd"); w.U32(0); w.U32(1);
    const auto entry = w.Box(s.entry); w.Zero(6); w.U16(1);
    const std::string_view type{s.entry};
    if (type == "avc1") {
        w.Zero(16); w.U16(g_mbs_wide * 16); w.U16(g_mbs_high * 16); w.U32(0x480000); w.U32(0x480000); w.U32(0); w.U16(1); w.Zero(32); w.U16(24); w.U16(0xffff);
        const auto avcc = w.Box("avcC"); const auto sps = Sps(), pps = Pps();
        w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(static_cast<unsigned>(sps.size())); w.Data(sps); w.B(1); w.U16(static_cast<unsigned>(pps.size())); w.Data(pps); w.End(avcc);
    } else if (type == "mp4a" || type == "ac-3") {
        w.Zero(8); w.U16(2); w.U16(16); w.Zero(4); w.U32(s.rate << 16);
        if (type == "mp4a") {
            const auto esds = w.Box("esds"); const auto n = static_cast<unsigned>(s.asc.size());
            w.U32(0); w.B(3); w.B(23 + n); w.U16(s.id); w.B(0); w.B(4); w.B(15 + n); w.B(0x40); w.B(0x15); w.Zero(3); w.U32(128000); w.U32(128000);
            w.B(5); w.B(n); w.Data(s.asc); w.B(6); w.B(1); w.B(2); w.End(esds);
        } else { const auto dac3 = w.Box("dac3"); w.B(0x10); w.B(0x3d); w.B(0xe0); w.End(dac3); }
    } else if (type == "tx3g") {
        w.U32(0); w.B(1); w.B(0xff); w.U32(0); w.Zero(8); w.U16(0); w.U16(0); w.U16(1); w.B(0); w.B(18); w.U32(0xffffffffu);
        const auto ftab = w.Box("ftab"); w.U16(1); w.U16(1); w.B(4); for (char c : std::string_view("Sans")) w.B(static_cast<unsigned char>(c)); w.End(ftab);
    } else if (type == "tmcd") { w.Zero(4); w.U32(0); w.U32(24); w.U32(1); w.B(24); w.B(0); }
    else if (type == "text") {
        // QuickTime text: display flags, justification, background colour, text box, reserved, font number and face,
        // reserved, foreground colour, and an empty font name.
        w.U32(0); w.U32(0); w.Zero(6); w.Zero(8); w.Zero(8); w.U16(0); w.U16(0); w.B(0); w.U16(0); w.Zero(6); w.B(0);
    }
    w.End(entry); w.End(box);
    box = w.Box("stts"); w.U32(0);
    if (s.deltas.empty()) { w.U32(1); w.U32(count); w.U32(s.delta); } else { w.U32(count); for (auto d : s.deltas) { w.U32(1); w.U32(d); } }
    w.End(box);
    box = w.Box("stsc"); w.U32(0); w.U32(1); w.U32(1); w.U32(1); w.U32(1); w.End(box);
    box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count); for (const auto size : s.sizes) w.U32(size); w.End(box);
    box = w.Box("stco"); w.U32(0); w.U32(count); for (auto offset : s.offsets) w.U32(offset); w.End(box);
    if (s.sync_every_other || s.sync_every > 1) {
        const unsigned every = s.sync_every > 1 ? s.sync_every : 2u;
        box = w.Box("stss"); w.U32(0); w.U32((count + every - 1) / every); for (unsigned i = 0; i < count; i += every) w.U32(i + 1); w.End(box);
    }
    w.End(stbl); w.End(minf); w.End(mdia); w.End(track);
}
// --chapters / --chapters-qt (docs/chapters.md): 9 s of 64x64 H.264 (an IDR every 2 pictures) and AAC in three scenes of
// flat luma (dark 24, bright 196, mid 110: hard cuts at 3 s and 6 s) and three chapters at 0, 3 and 6 s, "Opening",
// "פרק שני" and "Finale": as a Nero list (moov/udta/chpl), or as a QuickTime chapter track (a disabled 'text' track, id 3,
// named by the video's tref 'chap').
int Chapters(const wchar_t* path, bool quicktime) {
    constexpr unsigned kFrames = 216;
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(Video(i, i < 72 ? 24u : i < 144 ? 196u : 110u));
    const auto audio = Audio(9 * 48000 / 1024 + 1);
    const char* titles[]{"Opening", "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99", "Finale"};
    std::vector<Bytes> names;
    for (const char* t : titles) { Bytes b; const auto n = static_cast<unsigned>(std::strlen(t)); b.push_back(static_cast<std::uint8_t>(n >> 8)); b.push_back(static_cast<std::uint8_t>(n)); b.insert(b.end(), t, t + n); names.push_back(b); }
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every_other = true; if (quicktime) v.chapter = 3;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    TrackSpec t{3, "text", "text", "eng", "", false, 0, 1000, 3000};
    place(video, v); place(audio, a); if (quicktime) place(names, t);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(9000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(4); w.End(box);
    TrackEx(w, v); TrackEx(w, a); if (quicktime) TrackEx(w, t);
    if (!quicktime) {
        // Nero chapters: version 1, flags, 4 reserved bytes, a count, then a 64-bit start (100 ns), a length and a title each.
        const auto user = w.Box("udta"); box = w.Box("chpl"); w.U32(0x01000000); w.U32(0); w.B(3);
        for (unsigned c = 0; c < 3; ++c) {
            const std::uint64_t start = c * 30000000ull; w.U32(static_cast<unsigned>(start >> 32)); w.U32(static_cast<unsigned>(start));
            const auto n = static_cast<unsigned>(std::strlen(titles[c])); w.B(n); for (unsigned k = 0; k < n; ++k) w.B(static_cast<unsigned char>(titles[c][k]));
        }
        w.End(box); w.Zero(4); w.End(user); // QuickTime's 4-byte terminator, which readers must tolerate.
    }
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Chapter fixture (%s): %zu pictures, %zu AAC units, scenes at 0/3/6 s, %zu bytes\n", quicktime ? "QuickTime chapter track" : "Nero chpl",
                video.size(), audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --tracking (docs/picture.md, "Target tracking"): 6 s of 64x64 H.264 (IDRs of I_PCM macroblocks every 2 pictures, skipped
// P pictures between) and AAC. A mid-grey textured shot (110 + a fixed texture of 0-23 codes) with a bright 12x12 square
// (235) at the centre that moves 16 samples to the right between 1 s and 4 s and then stays; at 5 s a hard cut to a dark shot
// (24 + texture) with no square. The tracking test locks at 1 s and is lost at the cut.
int Tracking(const wchar_t* path) {
    constexpr unsigned kFrames = 144;
    const auto picture = [](unsigned frame) {
        Bits b; const bool idr = !(frame % 2);
        b.Ue(0); b.Ue(idr ? 7 : 5); b.Ue(0); b.Value(idr ? 0 : 1, 4); if (idr) b.Ue(frame / 2 % 2); b.Value(idr ? 0 : 2, 4);
        if (idr) { b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); b.Bit(0); }
        b.Ue(0); b.Ue(1);
        if (!idr) b.Ue(16);
        else {
            const double t = frame / 24.0;
            const unsigned left = 26 + static_cast<unsigned>(std::lround((std::clamp)((t - 1.0) / 3.0, 0.0, 1.0) * 16.0)), top = 26;
            const bool dark = frame >= 120;
            for (unsigned mb = 0; mb < 16; ++mb) {
                b.Ue(25); b.Align();
                const unsigned mx = mb % 4 * 16, my = mb / 4 * 16;
                for (unsigned i = 0; i < 256; ++i) {
                    const unsigned x = mx + i % 16, y = my + i / 16;
                    const bool square = !dark && x >= left && x < left + 12 && y >= top && y < top + 12;
                    b.Value(square ? 235u : (dark ? 24u : 110u) + (x * 13 + y * 7) % 24, 8);
                }
                for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
            }
        }
        Writer w; const auto nal = b.Nal(idr ? 0x65 : 0x41); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(6 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every_other = true;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(6000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("%s fixture: %zu pictures (a square moving from 1 s to 4 s, a cut at 5 s), %zu AAC units%s, %zu bytes\n", g_bursts ? "Saccade" : "Tracking", video.size(),
                audio.size(), g_bursts ? " (bursts: right at 2 s, left at 4 s)" : "", w.b.size());
    return good ? 0 : 1;
}
// --rotate90, --rotate180, --rotate270, --mirror (docs/picture.md, "Rotation"): 6 s of a still 64x32 H.264 picture (64x64 coded,
// the SPS cropping 32 rows; IDRs of I_PCM macroblocks every 2 pictures, skipped P pictures between) and AAC, its video track's
// tkhd matrix turning it a quarter, half or three quarters clockwise, or mirroring it. The picture tells its orientation: the
// left half dark (60), the right half light (180), a white (235) 12x12 square near the top-left corner (2..13 each way).
int Rotation(const wchar_t* path, const int* matrix, const char* name) {
    constexpr unsigned kFrames = 144;
    const auto picture = [](unsigned frame) {
        Bits b; const bool idr = !(frame % 2);
        b.Ue(0); b.Ue(idr ? 7 : 5); b.Ue(0); b.Value(idr ? 0 : 1, 4); if (idr) b.Ue(frame / 2 % 2); b.Value(idr ? 0 : 2, 4);
        if (idr) { b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); b.Bit(0); }
        b.Ue(0); b.Ue(1);
        if (!idr) b.Ue(16);
        else {
            for (unsigned mb = 0; mb < 16; ++mb) {
                b.Ue(25); b.Align();
                const unsigned mx = mb % 4 * 16, my = mb / 4 * 16;
                for (unsigned i = 0; i < 256; ++i) {
                    const unsigned x = mx + i % 16, y = my + i / 16;
                    const bool marker = x >= 2 && x < 14 && y >= 2 && y < 14;
                    b.Value(marker ? 235u : x < 32 ? 60u : 180u, 8);
                }
                for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
            }
        }
        Writer w; const auto nal = b.Nal(idr ? 0x65 : 0x41); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(6 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every_other = true; v.matrix = matrix;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(6000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Rotation fixture (%s): %zu pictures of 64x32, tkhd (%d, %d, %d, %d), %zu AAC units, %zu bytes\n", name, video.size(), matrix[0] / 65536,
                matrix[1] / 65536, matrix[2] / 65536, matrix[3] / 65536, audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --horizon (docs/picture.md, "Auto-leveling the horizon"): 6 s of a 640x352 H.264 picture (IDRs of I_PCM macroblocks every 8
// pictures, skipped P pictures between, each a reference) and AAC: horizon_scene.hpp's scene tilted +4.5 degrees (clockwise) for 3 s, then
// -6.0 degrees. The auto-leveling test levels it by -4.5, then by +6.0.
// --horizon-letterbox (docs/picture.md, "Black bars"): the same pictures with 40 rows of black (16, an encoder's +-1 code of noise)
// encoded at the top and the bottom.
int Horizon(const wchar_t* path, bool letterbox) {
    constexpr unsigned kFrames = 144, kWidth = 640, kHeight = 352, kEvery = 8;
    g_mbs_wide = kWidth / 16; g_mbs_high = kHeight / 16;
    std::vector<std::uint8_t> scenes[2];
    horizon_scene::Draw(4.5, kWidth, kHeight, scenes[0]); horizon_scene::Draw(-6.0, kWidth, kHeight, scenes[1]);
    if (letterbox) for (unsigned k = 0; k < 2; ++k) horizon_scene::Bars(scenes[k], kWidth, kHeight, 40, 0, k + 1);
    const unsigned mbs = g_mbs_wide * g_mbs_high;
    const auto picture = [&](unsigned frame) {
        Bits b; const unsigned n = frame % kEvery; const bool idr = n == 0;
        b.Ue(0); b.Ue(idr ? 7 : 5); b.Ue(0); b.Value(n, 4); if (idr) b.Ue(frame / kEvery % 2); b.Value(2 * n, 4); // frame_num, POC.
        if (idr) { b.Bit(0); b.Bit(0); } else { b.Bit(0); b.Bit(0); b.Bit(0); }
        b.Ue(0); b.Ue(1);
        if (!idr) b.Ue(mbs);
        else {
            const auto& scene = scenes[frame < kFrames / 2 ? 0 : 1];
            for (unsigned mb = 0; mb < mbs; ++mb) {
                b.Ue(25); b.Align();
                const unsigned mx = mb % g_mbs_wide * 16, my = mb / g_mbs_wide * 16;
                for (unsigned i = 0; i < 256; ++i) b.Value(scene[(my + i / 16) * kWidth + mx + i % 16], 8);
                for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
            }
        }
        Writer w; const auto nal = b.Nal(idr ? 0x65 : 0x41); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(6 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every = kEvery;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(6000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Horizon fixture%s: %zu pictures of %ux%u (+4.5 degrees for 3 s, then -6.0), %zu AAC units, %zu bytes\n", letterbox ? " (letterboxed)" : "", video.size(), kWidth,
                kHeight, audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --pulse (docs/picture.md, "Remote vibrometry", 2026-11-02): 12 s of 160x128 H.264 at 24 pictures a second, every picture an
// IDR of I_PCM macroblocks, and AAC: a still camera's textured background and, at the centre, a textured skin-toned 64x64
// patch whose luma follows a pulse, +1.5 codes x sin(2 pi 1.2 t) (72 beats a minute; the texture's rounding dithers it). The
// vibrometry test locks T on the patch and reads the HUD's pulse, then turns micro-motion on (M) for the warm grids.
// --chroma-pulse (2026-11-03; "Vibrometry in the window"): the same patch with its luma still and its colour pulsing, as a
// face's does with the blood under the skin (remote photoplethysmography): Cr +1.5 codes x sin(2 pi 1.2 t) about 150 (Cb 112),
// a chroma texture of +-3 codes dithering it. A luma grid sees nothing there.
int Pulse(const wchar_t* path, bool chroma) {
    constexpr unsigned kFrames = 288, kWidth = 160, kHeight = 128;
    constexpr double kPi = 3.14159265358979323846;
    g_mbs_wide = kWidth / 16; g_mbs_high = kHeight / 16;
    const unsigned mbs = g_mbs_wide * g_mbs_high;
    std::vector<std::uint8_t> luma(kWidth * kHeight), cb(kWidth * kHeight / 4, 128), cr(kWidth * kHeight / 4, 128);
    const auto picture = [&](unsigned frame) {
        const double t = frame / 24.0, pulse = 1.5 * std::sin(2 * kPi * 1.2 * t);
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                double v = 70 + 25 * std::sin(x / 5.0) * std::cos(y / 7.0) + 10 * std::sin((x + 2.0 * y) / 9.0);
                if (x >= 48 && x < 112 && y >= 32 && y < 96)
                    v = 150 + 18 * std::sin((x - 48) / 4.0) * std::cos((y - 32) / 6.0) + 9 * std::sin((x + y) / 3.0) + (chroma ? 0.0 : pulse);
                luma[y * kWidth + x] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 16l, 235l));
            }
        if (chroma)
            for (unsigned y = 16; y < 48; ++y)
                for (unsigned x = 24; x < 56; ++x) {
                    const double texture = 3 * std::sin(x / 2.3) * std::cos(y / 3.1);
                    cb[y * (kWidth / 2) + x] = static_cast<std::uint8_t>(std::lround(112 + texture));
                    cr[y * (kWidth / 2) + x] = static_cast<std::uint8_t>(std::lround(150 - texture + pulse));
                }
        Bits b;
        b.Ue(0); b.Ue(7); b.Ue(0); b.Value(0, 4); b.Ue(frame % 2); b.Value(0, 4); // An IDR: frame_num 0, POC 0.
        b.Bit(0); b.Bit(0);
        b.Ue(0); b.Ue(1);
        for (unsigned mb = 0; mb < mbs; ++mb) {
            b.Ue(25); b.Align();
            const unsigned mx = mb % g_mbs_wide * 16, my = mb / g_mbs_wide * 16;
            for (unsigned i = 0; i < 256; ++i) b.Value(luma[(my + i / 16) * kWidth + mx + i % 16], 8);
            for (unsigned i = 0; i < 64; ++i) b.Value(cb[(my / 2 + i / 8) * (kWidth / 2) + mx / 2 + i % 8], 8);
            for (unsigned i = 0; i < 64; ++i) b.Value(cr[(my / 2 + i / 8) * (kWidth / 2) + mx / 2 + i % 8], 8);
        }
        Writer w; const auto nal = b.Nal(0x65); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(12 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every = 1;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(12000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Pulse fixture: %zu pictures of %ux%u (a patch pulsing at 1.2 Hz in its %s), %zu AAC units, %zu bytes\n", video.size(), kWidth, kHeight,
                chroma ? "colour" : "luma", audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --shaken (docs/picture.md, "Stabilising the window", 2026-11-04): 8 s of 480x272 H.264 at 24 pictures a second, every picture an
// IDR of I_PCM macroblocks, and AAC: a hand-held camera over a textured wall with a white disc on it, shaking 8.8 and 7.6 px rms
// across and down (three sines each, 2.3 to 6.7 Hz: 11.6 px rms) and turning +-0.4 degree, while it pans 16 px a second. The picture
// is the wall (made at twice the resolution) sampled where the camera looks, between samples: the shake is not whole pixels.
int Shaken(const wchar_t* path) {
    constexpr unsigned kFrames = 192, kWidth = 480, kHeight = 272, kWall = 1600, kWallHigh = 1000;
    constexpr double kPi = 3.14159265358979323846;
    g_mbs_wide = kWidth / 16; g_mbs_high = kHeight / 16;
    const unsigned mbs = g_mbs_wide * g_mbs_high;
    // The wall: value noise at three scales, 40 to 190, the disc (radius 30 of its samples: 15 px) 235 at its centre at the start.
    const auto hash = [](std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
    const auto noise = [&](double u, double v, std::uint32_t seed) {
        const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
        const auto at = [&](int i, int j) { return static_cast<double>(hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 256u) / 255.0; };
        const int i = static_cast<int>(fu), j = static_cast<int>(fv);
        return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
    };
    std::vector<float> wall(std::size_t{kWall} * kWallHigh);
    for (unsigned y = 0; y < kWallHigh; ++y)
        for (unsigned x = 0; x < kWall; ++x) {
            const double v = 40 + 150 * (0.5 * noise(x / 48.0, y / 48.0, 3) + 0.3 * noise(x / 18.0, y / 18.0, 5) + 0.2 * noise(x / 7.0, y / 7.0, 7));
            const double r = std::hypot(x - 640.0, y - 500.0), cover = std::clamp(30.5 - r, 0.0, 1.0);
            wall[std::size_t{y} * kWall + x] = static_cast<float>(v + (235 - v) * cover);
        }
    const auto seen = [&](double x, double y) { // The wall at (x, y), its own samples, bilinear.
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        const double a = x - ix, b = y - iy;
        const float* p = wall.data() + std::size_t(iy) * kWall + ix;
        return (1 - a) * (1 - b) * p[0] + a * (1 - b) * p[1] + (1 - a) * b * p[kWall] + a * b * p[kWall + 1];
    };
    std::vector<std::uint8_t> luma(kWidth * kHeight);
    const auto picture = [&](unsigned frame) {
        const double t = frame / 24.0, two_pi = 2 * kPi;
        const double dx = 9 * std::sin(two_pi * 2.3 * t) + 7 * std::sin(two_pi * 3.7 * t + 1) + 5 * std::sin(two_pi * 5.9 * t + 2) + 16 * t;
        const double dy = 8 * std::sin(two_pi * 2.9 * t) + 6 * std::sin(two_pi * 4.3 * t + 0.5) + 4 * std::sin(two_pi * 6.7 * t + 1);
        const double turn = 0.007 * std::sin(two_pi * 3.1 * t + 0.3), co = std::cos(turn), si = std::sin(turn);
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                const double px = x + 0.5 - kWidth / 2.0, py = y + 0.5 - kHeight / 2.0;
                const double wx = co * px - si * py + dx, wy = si * px + co * py + dy; // Pixels of the wall about the disc.
                luma[y * kWidth + x] = static_cast<std::uint8_t>(std::clamp(std::lround(seen(640 + 2 * wx, 500 + 2 * wy)), 16l, 235l));
            }
        Bits b;
        b.Ue(0); b.Ue(7); b.Ue(0); b.Value(0, 4); b.Ue(frame % 2); b.Value(0, 4); // An IDR: frame_num 0, POC 0.
        b.Bit(0); b.Bit(0);
        b.Ue(0); b.Ue(1);
        for (unsigned mb = 0; mb < mbs; ++mb) {
            b.Ue(25); b.Align();
            const unsigned mx = mb % g_mbs_wide * 16, my = mb / g_mbs_wide * 16;
            for (unsigned i = 0; i < 256; ++i) b.Value(luma[(my + i / 16) * kWidth + mx + i % 16], 8);
            for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
        }
        Writer w; const auto nal = b.Nal(0x65); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(8 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every = 1;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(8000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Shaken fixture: %zu pictures of %ux%u (a camera shaking 11.6 px rms, panning 16 px/s), %zu AAC units, %zu bytes\n", video.size(), kWidth, kHeight, audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --two-targets (docs/picture.md, "Several objects at once", 2026-10-31): 6 s of 320x240 H.264 at 24 pictures a second, every
// picture an IDR of I_PCM macroblocks, and AAC: a still camera's textured static background with two textured squares
// approaching it at once: A centred at (0.27, 0.42), 4 m away at 0.45 m/s, half size 14 px at 4 m; B centred at (0.73, 0.58),
// 5 m away at 0.60 m/s, 18 px at 5 m (TwoTargetsHalf). The next-target test moves the lock between them.
double TwoTargetsHalf(unsigned k, double t) { return k == 0 ? 14 * 4.0 / (4.0 - 0.45 * t) : 18 * 5.0 / (5.0 - 0.60 * t); }
int TwoTargets(const wchar_t* path) {
    constexpr unsigned kFrames = 144, kWidth = 320, kHeight = 240;
    g_mbs_wide = kWidth / 16; g_mbs_high = kHeight / 16;
    const auto hash = [](std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
    const auto noise = [&](double u, double v, std::uint32_t seed) { // Value noise: hashed lattice values, bilinear between them.
        const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
        const auto at = [&](int i, int j) { return static_cast<double>(hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 181u); };
        const int i = static_cast<int>(fu), j = static_cast<int>(fv);
        return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
    };
    const unsigned mbs = g_mbs_wide * g_mbs_high;
    std::vector<std::uint8_t> luma(kWidth * kHeight);
    const auto picture = [&](unsigned frame) {
        const double t = frame / 24.0, cx[2]{0.27 * kWidth, 0.73 * kWidth}, cy[2]{0.42 * kHeight, 0.58 * kHeight};
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) {
                double sum = 0;
                for (int k = 0; k < 4; ++k) {
                    const double px = x + 0.25 + 0.5 * (k & 1), py = y + 0.25 + 0.5 * (k >> 1);
                    double v = 40 + noise(px / 6.0, py / 6.0, 3);
                    for (unsigned o = 0; o < 2; ++o) {
                        const double h = TwoTargetsHalf(o, t);
                        if (std::fabs(px - cx[o]) < h && std::fabs(py - cy[o]) < h) v = 30 + noise((px - cx[o]) / h * 6 + 50 + 40 * o, (py - cy[o]) / h * 6 + 50, 11 + o);
                    }
                    sum += v;
                }
                luma[y * kWidth + x] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 4), 16l, 235l));
            }
        Bits b;
        b.Ue(0); b.Ue(7); b.Ue(0); b.Value(0, 4); b.Ue(frame % 2); b.Value(0, 4); // An IDR: frame_num 0, POC 0.
        b.Bit(0); b.Bit(0);
        b.Ue(0); b.Ue(1);
        for (unsigned mb = 0; mb < mbs; ++mb) {
            b.Ue(25); b.Align();
            const unsigned mx = mb % g_mbs_wide * 16, my = mb / g_mbs_wide * 16;
            for (unsigned i = 0; i < 256; ++i) b.Value(luma[(my + i / 16) * kWidth + mx + i % 16], 8);
            for (unsigned i = 0; i < 128; ++i) b.Value(128, 8);
        }
        Writer w; const auto nal = b.Nal(0x65); w.U32(static_cast<unsigned>(nal.size())); w.Data(nal); return w.b;
    };
    std::vector<Bytes> video;
    for (unsigned i = 0; i < kFrames; ++i) video.push_back(picture(i));
    const auto audio = Audio(6 * 48000 / 1024 + 1);
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every = 1;
    TrackSpec a{2, "soun", "mp4a", "und", "SoundHandler", true};
    place(video, v); place(audio, a);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(6000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    TrackEx(w, v); TrackEx(w, a);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Two-targets fixture: %zu pictures of %ux%u (two squares approaching at once), %zu AAC units, %zu bytes\n", video.size(), kWidth, kHeight, audio.size(), w.b.size());
    return good ? 0 : 1;
}
// --audio-only (docs/audio-dsp.md, "Music mode"): an M4A of 10 s of AAC alone (48 kHz stereo, -18 dBFS) in three Nero
// chapters at 0, 3 and 6 s, "Intro", "Bridge" and "Finale", each a tone of its own: 440 Hz, 1760 Hz and 880 Hz.
int AudioOnly(const wchar_t* path) {
    static constexpr double steps[3]{440.0, 1760.0, 880.0};
    g_steps = steps;
    const auto audio = Audio(10 * 48000 / 1024 + 1);
    g_steps = nullptr;
    const char* titles[]{"Intro", "Bridge", "Finale"};
    Writer w; auto box = w.Box("ftyp"); w.U32(0x4d344120); w.U32(0); w.U32(0x4d344120); w.U32(0x6d703432); w.U32(0x69736f6d); w.End(box); // M4A, mp42, isom.
    box = w.Box("mdat");
    TrackSpec a{1, "soun", "mp4a", "eng", "SoundHandler", true};
    for (const auto& p : audio) { a.offsets.push_back(static_cast<unsigned>(w.b.size())); a.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(10000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(2); w.End(box);
    TrackEx(w, a);
    const auto user = w.Box("udta"); box = w.Box("chpl"); w.U32(0x01000000); w.U32(0); w.B(3);
    for (unsigned c = 0; c < 3; ++c) {
        const std::uint64_t start = c * 30000000ull; w.U32(static_cast<unsigned>(start >> 32)); w.U32(static_cast<unsigned>(start));
        const auto n = static_cast<unsigned>(std::strlen(titles[c])); w.B(n); for (unsigned k = 0; k < n; ++k) w.B(static_cast<unsigned char>(titles[c][k]));
    }
    w.End(box); w.End(user);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Music fixture (M4A): %zu AAC units, chapters at 0/3/6 s (440/1760/880 Hz), %zu bytes\n", audio.size(), w.b.size());
    return good ? 0 : 1;
}
int MultiTrack(const wchar_t* path) {
    constexpr unsigned kSeconds = 12;
    std::vector<Bytes> video; for (unsigned i = 0; i < kSeconds * 24; ++i) video.push_back(Video(i));
    Bytes asc_en, asc_he, asc_fr, asc_ar;
    const auto english = Audio(kSeconds * 48000 / 1024 + 1, 2, false, 0, &asc_en, 48000, 440.0, true);
    const auto hebrew = Audio(kSeconds * 48000 / 1024 + 1, 2, false, 0, &asc_he, 48000, 660.0, true);
    const auto french = Audio(kSeconds * 44100 / 1024 + 1, 2, false, 0, &asc_fr, 44100, 880.0, true);
    const auto arabic = Audio(kSeconds * 48000 / 1024 + 1, 6, false, 0, &asc_ar, 48000, 550.0, true);
    if (english.size() < 500 || hebrew.size() < 500 || french.size() < 500 || arabic.size() < 500) return 1;
    // Subtitles: one cue every 1.5 s; tx3g samples cover the timeline (an empty sample would be a gap).
    const auto cue = [](const char* text) { Bytes b; const auto n = static_cast<unsigned>(std::strlen(text)); b.push_back(static_cast<std::uint8_t>(n >> 8)); b.push_back(static_cast<std::uint8_t>(n)); b.insert(b.end(), text, text + n); return b; };
    std::vector<Bytes> hebrew_text, english_text; char line[64];
    for (unsigned i = 0; i < kSeconds * 2 / 3; ++i) {
        std::snprintf(line, sizeof(line), "\xd7\xa9\xd7\x95\xd7\xa8\xd7\x94 %u \xd7\x91\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa", i + 1); hebrew_text.push_back(cue(line)); // "שורה N בעברית"
        std::snprintf(line, sizeof(line), "Line %u in English", i + 1); english_text.push_back(cue(line));
    }
    const std::vector<Bytes> ac3(kSeconds * 48000 / 1536, Bytes(64, 0x0b)), timecode{Bytes{0, 0, 0, 0}};
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.U32(0x61766331); w.End(box);
    box = w.Box("mdat");
    const auto place = [&](const std::vector<Bytes>& samples, TrackSpec& spec) {
        for (const auto& p : samples) { spec.offsets.push_back(static_cast<unsigned>(w.b.size())); spec.sizes.push_back(static_cast<unsigned>(p.size())); w.Data(p); }
    };
    TrackSpec v{1, "vide", "avc1", "und", "VideoHandler", true, 0, 48000, 2000}; v.sync_every_other = true;
    TrackSpec en{2, "soun", "mp4a", "eng", "SoundHandler", true, 1}; en.asc = asc_en;
    TrackSpec he{3, "soun", "mp4a", "heb", "SoundHandler", false, 1}; he.asc = asc_he;
    TrackSpec fr{4, "soun", "mp4a", "fra", "Commentary", false, 1, 44100, 1024, 44100}; fr.asc = asc_fr;
    TrackSpec es{5, "soun", "ac-3", "spa", "SoundHandler", false, 1, 48000, 1536};
    TrackSpec he_text{6, "sbtl", "tx3g", "heb", "", false, 2, 1000, 1500};
    TrackSpec en_text{7, "sbtl", "tx3g", "eng", "", false, 2, 1000, 1500};
    TrackSpec tc{8, "tmcd", "tmcd", "und", "TimeCodeHandler", false, 0, 1000, kSeconds * 1000};
    TrackSpec ar{9, "soun", "mp4a", "ara", "SoundHandler", false, 1}; ar.asc = asc_ar;
    place(video, v); place(english, en); place(hebrew, he); place(french, fr); place(ac3, es); place(hebrew_text, he_text); place(english_text, en_text); place(timecode, tc); place(arabic, ar);
    w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(kSeconds * 1000);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(9); w.End(box);
    for (const auto* spec : {&v, &en, &he, &fr, &es, &he_text, &en_text, &tc, &ar}) TrackEx(w, *spec);
    w.End(moov);
    const bool good = Save(path, w.b);
    std::printf("Multi-track fixture: %zu pictures, AAC %zu/%zu/%zu/%zu units (en 440 Hz, he 660 Hz, fr 880 Hz at 44.1 kHz, ar 550 Hz 5.1), %zu cues x2, %zu bytes\n",
                video.size(), english.size(), hebrew.size(), french.size(), arabic.size(), hebrew_text.size(), w.b.size());
    return good ? 0 : 1;
}
// Usage: media_make_playback_fixture out.mp4 [--long | --longform | --chapters | --chapters-qt | --h264-bframes | --h264-bframes-sw | --hevc | --hevc-trim | --hevc10-4k | --hevc10-4k-pq | --hevc10-hlg |
//                                           --surround51 | --surround71 | --anamorphic | --audio-only | --pacing24 | --pacing30 | --pacing60]
int wmain(int argc, wchar_t** argv) {
    const bool surround51 = argc == 3 && std::wcscmp(argv[2], L"--surround51") == 0;
    const bool surround71 = argc == 3 && std::wcscmp(argv[2], L"--surround71") == 0;
    const bool extended = argc == 3 && std::wcscmp(argv[2], L"--long") == 0;
    const bool longform = argc == 3 && std::wcscmp(argv[2], L"--longform") == 0;
    const bool trim = argc == 3 && std::wcscmp(argv[2], L"--hevc-trim") == 0;
    const bool hevc = trim || (argc == 3 && std::wcscmp(argv[2], L"--hevc") == 0) ||
                      (argc == 3 && std::wcscmp(argv[2], L"--hevc-longgop") == 0);
    const bool hevc_longgop = argc == 3 && std::wcscmp(argv[2], L"--hevc-longgop") == 0;
    const bool hevc4k_longgop = argc == 3 && std::wcscmp(argv[2], L"--hevc10-4k-longgop") == 0;
    const bool hevc4k = hevc4k_longgop || (argc == 3 && std::wcscmp(argv[2], L"--hevc10-4k") == 0);
    const bool hdr_pq = argc == 3 && std::wcscmp(argv[2], L"--hevc10-4k-pq") == 0;
    const bool hdr_hlg = argc == 3 && std::wcscmp(argv[2], L"--hevc10-hlg") == 0;
    const bool avc_software = argc == 3 && std::wcscmp(argv[2], L"--h264-bframes-sw") == 0;
    const bool avc = avc_software || (argc == 3 && std::wcscmp(argv[2], L"--h264-bframes") == 0);
    g_anamorphic = argc == 3 && std::wcscmp(argv[2], L"--anamorphic") == 0;
    const bool multitrack = argc == 3 && std::wcscmp(argv[2], L"--multitrack") == 0;
    const bool chapters = argc == 3 && std::wcscmp(argv[2], L"--chapters") == 0;
    const bool chapters_qt = argc == 3 && std::wcscmp(argv[2], L"--chapters-qt") == 0;
    const bool audio_only = argc == 3 && std::wcscmp(argv[2], L"--audio-only") == 0;
    const unsigned pacing = argc != 3 ? 0u : std::wcscmp(argv[2], L"--pacing24") == 0 ? 24u : std::wcscmp(argv[2], L"--pacing30") == 0 ? 30u :
                            std::wcscmp(argv[2], L"--pacing60") == 0 ? 60u : 0u;
    g_bursts = argc == 3 && std::wcscmp(argv[2], L"--saccade") == 0;
    const bool tracking = g_bursts || (argc == 3 && std::wcscmp(argv[2], L"--tracking") == 0);
    const bool horizon = argc == 3 && std::wcscmp(argv[2], L"--horizon") == 0, letterbox = argc == 3 && std::wcscmp(argv[2], L"--horizon-letterbox") == 0;
    const bool two_targets = argc == 3 && std::wcscmp(argv[2], L"--two-targets") == 0;
    const bool pulse = argc == 3 && std::wcscmp(argv[2], L"--pulse") == 0, chroma_pulse = argc == 3 && std::wcscmp(argv[2], L"--chroma-pulse") == 0;
    const bool shaken = argc == 3 && std::wcscmp(argv[2], L"--shaken") == 0;
    const unsigned rotation = argc != 3 ? 0u : std::wcscmp(argv[2], L"--rotate90") == 0 ? 90u : std::wcscmp(argv[2], L"--rotate180") == 0 ? 180u :
                              std::wcscmp(argv[2], L"--rotate270") == 0 ? 270u : std::wcscmp(argv[2], L"--mirror") == 0 ? 1u : 0u;
    if (argc < 2 || argc > 3 || (argc == 3 && !extended && !longform && !avc && !hevc && !hevc4k && !hdr_pq && !hdr_hlg && !surround51 && !surround71 && !g_anamorphic && !multitrack && !chapters && !chapters_qt && !audio_only && !tracking && !pacing && !rotation && !horizon && !letterbox && !two_targets && !pulse && !chroma_pulse && !shaken)) return 2;
    const unsigned multiplier = extended ? 10u : 1u;
    Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); Hr(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (longform) { const int result = LongForm(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (pacing) { const int result = Pacing(argv[1], pacing); (void)MFShutdown(); CoUninitialize(); return result; }
    if (multitrack) { const int result = MultiTrack(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (audio_only) { const int result = AudioOnly(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (tracking) { const int result = Tracking(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (horizon || letterbox) { const int result = Horizon(argv[1], letterbox); (void)MFShutdown(); CoUninitialize(); return result; }
    if (two_targets) { const int result = TwoTargets(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (pulse || chroma_pulse) { const int result = Pulse(argv[1], chroma_pulse); (void)MFShutdown(); CoUninitialize(); return result; }
    if (shaken) { const int result = Shaken(argv[1]); (void)MFShutdown(); CoUninitialize(); return result; }
    if (rotation) {
        // tkhd's a, b, c, d: a quarter turn clockwise is (0, 1, -1, 0) (x' = -y, y' = x).
        static constexpr int kTurn90[]{0, 0x10000, -0x10000, 0}, kTurn180[]{-0x10000, 0, 0, -0x10000}, kTurn270[]{0, -0x10000, 0x10000, 0}, kMirror[]{-0x10000, 0, 0, 0x10000};
        const int* matrix = rotation == 90 ? kTurn90 : rotation == 180 ? kTurn180 : rotation == 270 ? kTurn270 : kMirror;
        g_crop_rows = 16;
        const int result = Rotation(argv[1], matrix, rotation == 1 ? "mirror" : rotation == 90 ? "90" : rotation == 180 ? "180" : "270");
        (void)MFShutdown(); CoUninitialize(); return result;
    }
    if (chapters || chapters_qt) { const int result = Chapters(argv[1], chapters_qt); (void)MFShutdown(); CoUninitialize(); return result; }
    HevcVideo hevc_video;
    std::vector<Bytes> video;
    if (hevc || hevc4k || hdr_pq || hdr_hlg) {
        (void)DeleteFileW(argv[1]); // A skipped run must not leave a stale fixture behind.
        const int encoded = hdr_pq ? EncodeHevc(true, 3840, 2160, hevc_video, 16) : hdr_hlg ? EncodeHevc(true, 1280, 720, hevc_video, 18) :
                            hevc4k ? EncodeHevc(true, 3840, 2160, hevc_video, 0, hevc4k_longgop ? 72 : 24) :
                            EncodeHevc(false, 1280, 720, hevc_video, 0, hevc_longgop ? 72 : 24);
        if (encoded) {
            std::puts(encoded == 77 ? "SKIP no platform HEVC encoder for this format" : "FAIL HEVC fixture authoring");
            (void)MFShutdown(); CoUninitialize(); return encoded;
        }
        hevc_video.trim = trim ? 1u : 0u;
        video = hevc_video.samples;
    } else if (avc) {
        (void)DeleteFileW(argv[1]);
        const int encoded = EncodeAvc(avc_software, hevc_video);
        if (encoded) {
            std::puts(encoded == 77 ? "SKIP no platform H.264 encoder" : "FAIL H.264 fixture authoring");
            (void)MFShutdown(); CoUninitialize(); return encoded;
        }
        video = hevc_video.samples;
    } else for (unsigned i = 0; i < 72 * multiplier; ++i) video.push_back(Video(i));
    Bytes asc{0x11, 0x90}; unsigned channels = 2;
    auto audio = surround71 ? Silent71(141) : surround51 ? Audio(141, 6, true, 0, &asc) : Audio(141 * multiplier);
    if (surround51) channels = 6;
    if (surround71) { asc = Pce71(); channels = 8; }
    Writer w; auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d);
    const bool any_hevc = hevc || hevc4k || hdr_pq || hdr_hlg;
    w.U32(any_hevc ? 0x68657631 : 0x61766331); w.End(box);
    box = w.Box("mdat"); std::vector<unsigned> video_offsets, audio_offsets;
    for (const auto& p : video) { video_offsets.push_back(static_cast<unsigned>(w.b.size())); w.Data(p); }
    for (const auto& p : audio) { audio_offsets.push_back(static_cast<unsigned>(w.b.size())); w.Data(p); } w.End(box);
    const auto moov = w.Box("moov"); box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(48000); w.U32(static_cast<unsigned>(audio.size()) * 1024);
    w.U32(0x10000); w.U16(0x100); w.Zero(10); Matrix(w); w.Zero(24); w.U32(3); w.End(box);
    if (any_hevc || avc) HevcTrack(w, hevc_video, video_offsets, avc); else Track(w, 1, true, Sizes(video), video_offsets);
    Track(w, 2, false, Sizes(audio), audio_offsets, asc, channels); w.End(moov);
    const bool good = Save(argv[1], w.b);
    std::printf("Original fixture: %zu video AUs, %zu AAC AUs, %zu bytes\n", video.size(), audio.size(), w.b.size());
    (void)MFShutdown(); CoUninitialize(); return good ? 0 : 1;
}
