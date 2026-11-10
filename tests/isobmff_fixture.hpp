#pragma once
#include "media/demux/IsobmffParser.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace fixture {
// Edit list authoring: count entries of (media_time, rate); twice = two elst boxes (duplicate).
// `delay`: an empty edit (media_time -1) of that many movie ticks (1000/s) written first, then the `count` edits.
struct Edit { unsigned version = 0, count = 1; std::int64_t media_time = 0; std::uint32_t rate = 0x10000; bool twice = false; std::uint32_t delay = 0; };
struct File {
    std::array<std::byte, 65536> data{};
    std::uint32_t size = 0;
    std::uint32_t chunk_table = 0, timing_table = 0, config = 0, sample_entry = 0;
    std::uint64_t samples[16]{};
    std::uint32_t sample_count = 0;
    // Optional boxes, set before building: a BT.2020 PQ colour description with HDR metadata in a video sample entry,
    // and a sample group (sgpd + sbgp) of type `group` at the end of stbl.
    bool color = false;
    // QuickTime's layout (iPhones, ffmpeg's .mov): a data handler (hdlr 'dhlr' 'alis') inside minf and an 'alis' dref.
    bool quicktime = false;
    // The classic stts: two samples of `first_delta` ticks, then one of `last_delta` (0: a muxer that did not know the last length).
    std::uint32_t first_delta = 1001, last_delta = 2002;
    // More optional sample-entry boxes: `pasp` (both terms non-zero or `pasp_written`), and a QuickTime `nclc` colour
    // description (BT.709, no range flag) instead of the nclx one.
    std::uint32_t pasp_width = 0, pasp_height = 0; bool pasp_written = false, nclc = false;
    std::int32_t matrix[4]{}; // tkhd's a, b, c, d (16.16); all zero: the matrix left zero, as many muxers do.
    struct Group { std::uint32_t type = 0, version = 1, entry_bytes = 2; int distance = -1; } group;
    void B(std::uint32_t x) { assert(size < data.size()); data[size++] = static_cast<std::byte>(x & 255); }
    void U16(std::uint32_t x) { B(x >> 8); B(x); }
    void U32(std::uint32_t x) { B(x >> 24); B(x >> 16); B(x >> 8); B(x); }
    void U64(std::uint64_t x) { U32(static_cast<std::uint32_t>(x >> 32)); U32(static_cast<std::uint32_t>(x)); }
    void Zero(std::uint32_t n) { for (std::uint32_t i = 0; i < n; ++i) B(0); }
    void Patch32(std::uint32_t at, std::uint32_t x) {
        for (unsigned i = 0; i < 4; ++i) data[at + i] = static_cast<std::byte>((x >> ((3 - i) * 8)) & 255);
    }
    std::uint32_t Box(const char* tag) {
        const auto start = size; U32(0); for (unsigned i = 0; i < 4; ++i) B(static_cast<unsigned char>(tag[i])); return start;
    }
    void End(std::uint32_t start) { Patch32(start, size - start); }
    void Codec(media::demux::Codec codec) {
        using media::demux::Codec;
        const bool audio = codec == Codec::Aac || codec == Codec::Opus;
        const auto entry = Box(codec == Codec::H264 ? "avc1" : codec == Codec::Hevc ? "hvc1" :
            codec == Codec::Av1 ? "av01" : codec == Codec::Aac ? "mp4a" : "Opus");
        sample_entry = entry;
        Zero(6); U16(1);
        if (!audio) { Zero(16); U16(1920); U16(1080); Zero(50); }
        else { Zero(8); U16(2); U16(16); Zero(4); U32(48000u << 16); }
        const auto box = Box(codec == Codec::H264 ? "avcC" : codec == Codec::Hevc ? "hvcC" :
            codec == Codec::Av1 ? "av1C" : codec == Codec::Aac ? "esds" : "dOps");
        config = size;
        if (codec == Codec::H264) {
            B(1); B(66); B(0); B(30); B(255); B(225); U16(2); B(0x67); B(0x42); B(1); U16(2); B(0x68); B(0);
        } else if (codec == Codec::Hevc) {
            B(1); Zero(20); B(3); B(3);
            for (unsigned type = 32; type <= 34; ++type) { B(type | 128); U16(1); U16(2); B(type << 1); B(1); }
        } else if (codec == Codec::Av1) { B(0x81); B(0); B(0x0c); B(0); B(0x0a); B(0); }
        else if (codec == Codec::Aac) {
            U32(0); B(3); B(25); U16(1); B(0);
            B(4); B(17); B(0x40); B(0x15); Zero(11); B(5); B(2); B(0x11); B(0x90);
            B(6); B(1); B(2);
        } else { B(0); B(2); U16(312); U32(48000); U16(0); B(0); }
        End(box);
        if ((pasp_width || pasp_height || pasp_written) && !audio) { const auto extra = Box("pasp"); U32(pasp_width); U32(pasp_height); End(extra); }
        if (nclc && !audio) { const auto extra = Box("colr"); U32(0x6e636c63); U16(1); U16(1); U16(1); End(extra); }
        if (color && !audio) {
            auto extra = Box("colr"); U32(0x6e636c78); U16(9); U16(16); U16(9); B(0x80); End(extra);
            extra = Box("clli"); U16(839); U16(39); End(extra);
            extra = Box("mdcv"); Zero(24); End(extra);
        }
        End(entry);
    }
    void EditBox(const Edit& edit) {
        const auto edits = Box("edts");
        for (unsigned copy = 0; copy < (edit.twice ? 2u : 1u); ++copy) {
            const auto list = Box("elst"); U32(edit.version << 24); U32(edit.count + (edit.delay ? 1u : 0u));
            if (edit.delay) {
                if (edit.version) { U64(edit.delay); U64(~std::uint64_t{0}); } else { U32(edit.delay); U32(0xffffffffu); }
                U32(0x10000);
            }
            for (unsigned i = 0; i < edit.count; ++i) {
                if (edit.version) { U64(5000); U64(static_cast<std::uint64_t>(edit.media_time)); }
                else { U32(5000); U32(static_cast<std::uint32_t>(edit.media_time)); }
                U32(edit.rate);
            }
            End(list);
        }
        End(edits);
    }
    void Movie(media::demux::Codec codec, bool fragmented, bool wide_offsets = false, const Edit* edit = nullptr) {
        const auto movie = Box("moov");
        auto box = Box("mvhd"); U32(0); Zero(8); U32(1000); Zero(84); End(box);
        const auto track = Box("trak");
        box = Box("tkhd"); U32(7); Zero(8); U32(1);
        if (matrix[0] | matrix[1] | matrix[2] | matrix[3]) {
            Zero(24); U32(static_cast<std::uint32_t>(matrix[0])); U32(static_cast<std::uint32_t>(matrix[1])); U32(0);
            U32(static_cast<std::uint32_t>(matrix[2])); U32(static_cast<std::uint32_t>(matrix[3])); U32(0); U32(0); U32(0); U32(0x40000000); Zero(8);
        } else Zero(68);
        End(box);
        if (edit) EditBox(*edit);
        const auto media = Box("mdia");
        box = Box("mdhd"); U32(0); Zero(8); U32(fragmented ? 1000u : 30000u); Zero(8); End(box);
        box = Box("hdlr"); U32(0); U32(0);
        const char* handler = codec == media::demux::Codec::Aac || codec == media::demux::Codec::Opus ? "soun" : "vide";
        for (unsigned i = 0; i < 4; ++i) B(static_cast<unsigned char>(handler[i]));
        Zero(12); End(box);
        const auto information = Box("minf");
        if (quicktime) { box = Box("hdlr"); U32(0); B('d'); B('h'); B('l'); B('r'); B('a'); B('l'); B('i'); B('s'); Zero(12); End(box); }
        const auto dinf = Box("dinf"); box = Box("dref"); U32(0); U32(1);
        const auto url = Box(quicktime ? "alis" : "url "); U32(1); End(url); End(box); End(dinf);
        const auto table = Box("stbl");
        box = Box("stsd"); U32(0); U32(1); Codec(codec); End(box);
        box = Box("stts"); timing_table = size; U32(0); U32(fragmented ? 0u : 2u);
        if (!fragmented) { U32(2); U32(first_delta); U32(1); U32(last_delta); } End(box);
        box = Box("stsc"); U32(0); U32(fragmented ? 0u : 2u);
        if (!fragmented) { U32(1); U32(2); U32(1); U32(2); U32(1); U32(1); } End(box);
        box = Box("stsz"); U32(0); U32(0); U32(fragmented ? 0u : 3u);
        if (!fragmented) { U32(4); U32(6); U32(5); } End(box);
        box = Box(wide_offsets ? "co64" : "stco"); U32(0); U32(fragmented ? 0u : 2u); chunk_table = size;
        if (!fragmented) Zero(wide_offsets ? 16 : 8);
        End(box);
        if (!fragmented) {
            box = Box("ctts"); U32(0x1000000); U32(3); U32(1); U32(static_cast<std::uint32_t>(-500));
            U32(1); U32(2000); U32(1); U32(static_cast<std::uint32_t>(-1000)); End(box);
            box = Box("stss"); U32(0); U32(2); U32(1); U32(3); End(box);
        }
        if (group.type) {
            box = Box("sgpd"); U32(group.version << 24); U32(group.type);
            if (group.version == 1) U32(group.entry_bytes);
            U32(1); U16(static_cast<std::uint32_t>(group.distance)); Zero(group.entry_bytes > 2 ? group.entry_bytes - 2 : 0); End(box);
            box = Box("sbgp"); U32(0); U32(group.type); U32(1); U32(3); U32(1); End(box);
        }
        End(table); End(information); End(media); End(track);
        if (fragmented) {
            const auto mvex = Box("mvex"); box = Box("trex"); U32(0); U32(1); U32(1);
            U32(1000); U32(4); U32(0); End(box); End(mvex);
        }
        End(movie);
    }
    void Ftyp() { const auto box = Box("ftyp"); U32(0x69736f6d); U32(0); U32(0x69736f36); End(box); }
    void Payload(bool wide_offsets) {
        const auto box = Box("mdat");
        samples[0] = size; for (unsigned i = 0; i < 4; ++i) B(0xa0 + i);
        samples[1] = size; for (unsigned i = 0; i < 6; ++i) B(0xb0 + i);
        Zero(2); samples[2] = size; for (unsigned i = 0; i < 5; ++i) B(0xc0 + i);
        End(box); sample_count = 3;
        if (chunk_table) {
            Patch32(chunk_table + (wide_offsets ? 4 : 0), static_cast<std::uint32_t>(samples[0]));
            Patch32(chunk_table + (wide_offsets ? 12 : 4), static_cast<std::uint32_t>(samples[2]));
        }
    }
    void Classic(media::demux::Codec codec, bool wide = false, bool tail_movie = false, const Edit* edit = nullptr) {
        Ftyp();
        if (tail_movie) {
            Payload(wide); Movie(codec, false, wide, edit);
            Patch32(chunk_table + (wide ? 4 : 0), static_cast<std::uint32_t>(samples[0]));
            Patch32(chunk_table + (wide ? 12 : 4), static_cast<std::uint32_t>(samples[2]));
        } else { Movie(codec, false, wide, edit); Payload(wide); }
    }
    void Fragment(std::uint32_t sequence, bool defaults) {
        const auto fragment = Box("moof");
        auto box = Box("mfhd"); U32(0); U32(sequence); End(box);
        const auto traffic = Box("traf");
        box = Box("tfhd"); U32(0x20000); U32(1); End(box);
        box = Box("tfdt"); U32(0x1000000); U64(defaults ? 3000 : 0); End(box);
        box = Box("trun"); U32(defaults ? 1u : 0x1000f01u); U32(defaults ? 1u : 2u);
        const auto data_offset = size; U32(0);
        if (!defaults) {
            U32(1000); U32(4); U32(0); U32(2000);
            U32(2000); U32(4); U32(0x10000); U32(static_cast<std::uint32_t>(-500));
        }
        End(box); End(traffic); End(fragment);
        Patch32(data_offset, size + 8 - fragment);
        box = Box("mdat");
        for (unsigned i = 0; i < (defaults ? 1u : 2u); ++i) {
            samples[sample_count++] = size; for (unsigned j = 0; j < 4; ++j) B(sequence * 16 + i * 4 + j);
        }
        End(box);
    }
    void Fragmented(media::demux::Codec codec, const Edit* edit = nullptr) { Ftyp(); Movie(codec, true, false, edit); Fragment(1, false); Fragment(2, true); }
};
}
