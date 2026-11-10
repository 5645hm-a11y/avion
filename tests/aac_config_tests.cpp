// Portable AudioSpecificConfig / program_config_element parsing (MSVC, GCC -Werror, ASan/UBSan).
#include "media/codec/AacConfig.hpp"
#include "allocation_probe.hpp"
#include <bit>
#include <cstdio>
#include <initializer_list>
#include <vector>
using namespace media::codec;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
struct Writer {
    std::vector<std::byte> b; unsigned n = 0;
    void Put(unsigned v, unsigned bits) {
        while (bits) { if (!(n % 8)) b.push_back(std::byte{0}); if ((v >> --bits) & 1) b.back() |= std::byte{static_cast<unsigned char>(0x80 >> (n % 8))}; ++n; }
    }
    void Align() { while (n % 8) Put(0, 1); }
};
std::vector<std::byte> Bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out; for (auto v : values) out.push_back(std::byte{static_cast<unsigned char>(v)}); return out;
}
// AAC-LC 48 kHz with a PCE: front/side/back elements given as is_cpe flags.
std::vector<std::byte> Pce(std::initializer_list<unsigned> front, std::initializer_list<unsigned> side, std::initializer_list<unsigned> back,
                           unsigned lfe, unsigned coupling = 0, unsigned comment = 0) {
    Writer w; w.Put(2, 5); w.Put(3, 4); w.Put(0, 4); w.Put(0, 3);
    w.Put(0, 4); w.Put(1, 2); w.Put(3, 4);
    w.Put(static_cast<unsigned>(front.size()), 4); w.Put(static_cast<unsigned>(side.size()), 4); w.Put(static_cast<unsigned>(back.size()), 4);
    w.Put(lfe, 2); w.Put(1, 3); w.Put(coupling, 4);
    w.Put(1, 1); w.Put(0, 4); w.Put(0, 1); w.Put(1, 1); w.Put(2, 3); // Mono mixdown element, matrix mixdown.
    unsigned tag = 0;
    for (auto group : {front, side, back}) for (auto pair : group) { w.Put(pair, 1); w.Put(tag++ & 15, 4); }
    for (unsigned i = 0; i < lfe; ++i) w.Put(i, 4);
    w.Put(3, 4); // One associated data element.
    for (unsigned i = 0; i < coupling; ++i) { w.Put(0, 1); w.Put(i, 4); }
    w.Align(); w.Put(comment, 8); for (unsigned i = 0; i < comment; ++i) w.Put('a' + i, 8);
    return w.b;
}
void Fixed() {
    AacConfig c{};
    // Native encoder output: stereo, mono, 5.1 (AAC-LC, 48 kHz).
    CHECK(ParseAacConfig(Bytes({0x11, 0x90}), c) == AacConfigStatus::Ready && c.channels == 2 && c.channel_mask == speaker::Stereo && c.sample_rate == 48000);
    CHECK(ParseAacConfig(Bytes({0x11, 0x88}), c) == AacConfigStatus::Ready && c.channels == 1 && c.channel_mask == speaker::Mono);
    CHECK(ParseAacConfig(Bytes({0x11, 0xb0}), c) == AacConfigStatus::Ready && c.channels == 6 && c.channel_mask == speaker::Surround51 && !c.program_config);
    CHECK(ParseAacConfig(Bytes({0x11, 0xa8}), c) == AacConfigStatus::Ready && c.channels == 5 && c.channel_mask == 0x37u); // 5.0, as the decoder reports.
    CHECK(ParseAacConfig(Bytes({0x11, 0xe0}), c) == AacConfigStatus::Ready && c.channels == 8 && c.channel_mask == speaker::Surround71);
    CHECK(ParseAacConfig(Bytes({0x11, 0xb8}), c) == AacConfigStatus::Ready && c.channels == 8 && c.channel_mask == 0xffu); // 7.1 wide.
    CHECK(ParseAacConfig(Bytes({0x11, 0xd8}), c) == AacConfigStatus::Ready && c.channels == 7 && c.channel_mask == 0x70fu); // 6.1.
    CHECK(ParseAacConfig(Bytes({0x12, 0x10}), c) == AacConfigStatus::Ready && c.sample_rate == 44100 && c.channels == 2);
    // The native encoder's 8-channel output is HE-AAC (object type 5): recognised, not admitted.
    CHECK(ParseAacConfig(Bytes({0x2b, 0x39, 0x88, 0x00}), c) == AacConfigStatus::Unsupported && c.object_type == 5);
    // Escaped object type (31 -> 32 + 6 bits): USAC (42).
    Writer usac; usac.Put(31, 5); usac.Put(10, 6); usac.Put(3, 4); usac.Put(2, 4); usac.Align();
    CHECK(ParseAacConfig(usac.b, c) == AacConfigStatus::Unsupported && c.object_type == 42);
    // Explicit sampling frequency (index 15 + 24 bits).
    Writer rate; rate.Put(2, 5); rate.Put(15, 4); rate.Put(44056, 24); rate.Put(2, 4); rate.Put(0, 3); rate.Align();
    CHECK(ParseAacConfig(rate.b, c) == AacConfigStatus::Ready && c.sample_rate == 44056 && c.channels == 2);
    // 960-sample frames, core coder, reserved index, reserved configuration, 22.2.
    CHECK(ParseAacConfig(Bytes({0x11, 0x94}), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Bytes({0x11, 0x92, 0x00, 0x00}), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Bytes({0x16, 0x90}), c) == AacConfigStatus::Invalid);
    CHECK(ParseAacConfig(Bytes({0x11, 0xc0}), c) == AacConfigStatus::Invalid);        // Configuration 8 is reserved.
    CHECK(ParseAacConfig(Bytes({0x11, 0xe8}), c) == AacConfigStatus::Unsupported);    // 13: 22.2.
    CHECK(ParseAacConfig(Bytes({0x11, 0x91}), c) == AacConfigStatus::Invalid);        // extensionFlag on AAC-LC.
    CHECK(ParseAacConfig(Bytes({0x11}), c) == AacConfigStatus::Truncated);
    CHECK(ParseAacConfig({}, c) == AacConfigStatus::Truncated);
}
void Programs() {
    AacConfig c{};
    // The PCEs the native decoder accepted in the platform probe (5.1) and rejected only for its channel count (7.1).
    CHECK(ParseAacConfig(Bytes({0x11, 0x80, 0x04, 0xc8, 0x05, 0x00, 0x01, 0x19, 0x00, 0x00}), c) == AacConfigStatus::Ready);
    CHECK(c.program_config && c.channels == 6 && c.channel_mask == speaker::Surround51 && c.front_elements == 2 && c.back_elements == 1 && c.lfe_elements == 1);
    CHECK(ParseAacConfig(Bytes({0x11, 0x80, 0x04, 0xc8, 0x45, 0x00, 0x01, 0x19, 0x4c, 0x00, 0x00}), c) == AacConfigStatus::Ready);
    CHECK(c.channels == 8 && c.channel_mask == speaker::Surround71 && c.side_elements == 1);
    // Built PCEs, with mixdown fields, an associated data element and a comment.
    CHECK(ParseAacConfig(Pce({0, 1}, {1}, {}, 1), c) == AacConfigStatus::Ready && c.channel_mask == speaker::Surround51Side && c.channels == 6);
    CHECK(ParseAacConfig(Pce({0, 1}, {1}, {1}, 1, 0, 5), c) == AacConfigStatus::Ready && c.channel_mask == speaker::Surround71 && c.channels == 8);
    CHECK(ParseAacConfig(Pce({0, 1, 1}, {}, {1}, 1), c) == AacConfigStatus::Ready && c.channel_mask == 0xffu);           // 7.1 wide.
    CHECK(ParseAacConfig(Pce({0, 1}, {}, {1, 0}, 1), c) == AacConfigStatus::Ready && c.channel_mask == 0x13fu);          // Back pair + centre.
    CHECK(ParseAacConfig(Pce({1}, {}, {}, 0), c) == AacConfigStatus::Ready && c.channel_mask == speaker::Stereo);
    CHECK(ParseAacConfig(Pce({0}, {}, {}, 0), c) == AacConfigStatus::Ready && c.channel_mask == speaker::Mono);
    // Layouts without a speaker assignment, coupling channels, two LFEs, an empty programme.
    CHECK(ParseAacConfig(Pce({0, 0}, {}, {}, 0), c) == AacConfigStatus::Unsupported && c.channels == 2);
    CHECK(ParseAacConfig(Pce({1}, {0}, {}, 0), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Pce({1}, {}, {1, 1}, 0), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Pce({0, 1}, {}, {1}, 2), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Pce({0, 1}, {}, {1}, 1, 1), c) == AacConfigStatus::Unsupported);
    CHECK(ParseAacConfig(Pce({}, {}, {}, 0), c) == AacConfigStatus::Invalid);
    // Every truncation of a valid PCE is reported as truncated, never read past the end.
    const auto full = Pce({0, 1}, {1}, {1}, 1, 0, 3);
    for (std::size_t n = 0; n < full.size(); ++n) CHECK(ParseAacConfig(std::span<const std::byte>(full.data(), n), c) == AacConfigStatus::Truncated);
}
void Mapping() {
    ChannelMap m{};
    // Identity layouts.
    CHECK(MapToOutput(speaker::Stereo, m) && m.identity && m.out_channels == 2 && m.mix[0][0] == 1 && m.mix[1][1] == 1 && m.mix[0][1] == 0);
    CHECK(MapToOutput(speaker::Mono, m) && m.identity && m.out_channels == 1);
    CHECK(MapToOutput(speaker::Surround51, m) && m.identity && m.out_channels == 6 && m.out_mask == speaker::Surround51);
    // 5.0 (FL FR FC BL BR, as the Microsoft decoder reports it): BL/BR move to output 4/5, LFE (output 3) silent.
    CHECK(MapToOutput(0x37u, m) && !m.identity && m.in_channels == 5 && m.out_channels == 6);
    CHECK(m.mix[0][0] == 1 && m.mix[1][1] == 1 && m.mix[2][2] == 1 && m.mix[4][3] == 1 && m.mix[5][4] == 1);
    float lfe = 0; for (unsigned c = 0; c < 6; ++c) lfe += m.mix[3][c]; CHECK(lfe == 0);
    // 5.1 with side surrounds: the sides take the surround pair.
    CHECK(MapToOutput(speaker::Surround51Side, m) && m.out_channels == 6 && m.mix[3][3] == 1 && m.mix[4][4] == 1 && m.mix[5][5] == 1);
    // 4.0 (FL FR FC BC): the back centre feeds both surrounds at -3 dB.
    CHECK(MapToOutput(0x107u, m) && m.in_channels == 4 && m.mix[4][3] == 0.70710678f && m.mix[5][3] == 0.70710678f);
    // 3.0 (FL FR FC).
    CHECK(MapToOutput(0x7u, m) && m.out_channels == 6 && m.mix[2][2] == 1 && m.mix[4][0] == 0);
    // No slot: more than six channels, front-of-centre pairs, top speakers, empty.
    CHECK(!MapToOutput(speaker::Surround71, m) && m.out_channels == 0);
    CHECK(!MapToOutput(speaker::Stereo | speaker::FrontLeftOfCenter | speaker::FrontRightOfCenter, m));
    CHECK(!MapToOutput(speaker::Stereo | 0x800u, m));
    CHECK(!MapToOutput(0, m));
    // Every mask: a mapping that succeeds sums each input to at most sqrt(2) of power and never reads past 6 inputs.
    for (std::uint32_t mask = 0; mask < 0x1000; ++mask) {
        if (!MapToOutput(mask, m)) continue;
        CHECK(m.in_channels <= 6 && (m.out_channels == 6 || m.identity));
        for (unsigned c = 0; c < m.in_channels; ++c) { float power = 0; for (unsigned o = 0; o < 6; ++o) power += m.mix[o][c] * m.mix[o][c]; CHECK(power > 0.99f && power < 1.01f); }
    }
}
void Exhaustive() {
    // Every 1-, 2- and 3-byte input: no crash (ASan/UBSan), a status for each, and consistency of Ready results.
    AacConfig c{}; unsigned ready = 0;
    std::byte buffer[3]{};
    for (unsigned length = 1; length <= 3; ++length)
        for (unsigned v = 0; v < (1u << (8 * length)); ++v) {
            for (unsigned i = 0; i < length; ++i) buffer[i] = std::byte{static_cast<unsigned char>(v >> (8 * (length - 1 - i)))};
            if (ParseAacConfig(std::span<const std::byte>(buffer, length), c) == AacConfigStatus::Ready) {
                ++ready;
                if (c.object_type != 2 || !c.channels || c.channels > 8 || static_cast<unsigned>(std::popcount(c.channel_mask)) != c.channels) {
                    CHECK(false); return;
                }
            }
        }
    CHECK(ready > 0);
}
}
int main() {
    Fixed(); Programs(); Mapping();
    // Parsing itself allocates nothing: inputs are built first, only the parse calls are measured.
    const auto pce = Pce({0, 1}, {1}, {1}, 1, 0, 3); const auto plain = Bytes({0x11, 0xb0});
    AacConfig c{}; unsigned ready = 0;
    allocation_probe::Begin();
    for (unsigned i = 0; i < 1000; ++i) ready += (ParseAacConfig(pce, c) == AacConfigStatus::Ready) + (ParseAacConfig(plain, c) == AacConfigStatus::Ready);
    const auto allocations = allocation_probe::End();
    CHECK(ready == 2000);
    CHECK(allocations == 0);
    Exhaustive();
    std::printf("AAC config: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
