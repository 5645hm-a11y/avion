#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace media::codec {
// WAVEFORMATEXTENSIBLE speaker bits (ksmedia.h values; portable here).
namespace speaker {
constexpr std::uint32_t FrontLeft = 0x1, FrontRight = 0x2, FrontCenter = 0x4, LowFrequency = 0x8, BackLeft = 0x10, BackRight = 0x20,
                        FrontLeftOfCenter = 0x40, FrontRightOfCenter = 0x80, BackCenter = 0x100, SideLeft = 0x200, SideRight = 0x400;
constexpr std::uint32_t Mono = FrontCenter, Stereo = FrontLeft | FrontRight;
constexpr std::uint32_t Surround51 = Stereo | FrontCenter | LowFrequency | BackLeft | BackRight;        // KSAUDIO_SPEAKER_5POINT1
constexpr std::uint32_t Surround51Side = Stereo | FrontCenter | LowFrequency | SideLeft | SideRight;    // KSAUDIO_SPEAKER_5POINT1_SURROUND
constexpr std::uint32_t Surround71 = Surround51 | SideLeft | SideRight;                                // KSAUDIO_SPEAKER_7POINT1_SURROUND
}
enum class AacConfigStatus : std::uint8_t { Ready, Truncated, Invalid, Unsupported };
// Decoded AudioSpecificConfig (ISO/IEC 14496-3 1.6.2.1) with GASpecificConfig and, for channelConfiguration 0,
// the program_config_element (4.4.1.1). `channel_mask` lists the speakers in WAVEFORMATEXTENSIBLE bit order, which
// is also the interleaved order of decoded PCM.
struct AacConfig {
    unsigned object_type = 0;           // 2 = AAC-LC (after the 31 escape).
    unsigned sample_rate = 0;           // From the index table or the explicit 24-bit value.
    unsigned channel_configuration = 0; // 0 = described by the PCE.
    unsigned channels = 0;              // Including LFE.
    std::uint32_t channel_mask = 0;
    bool program_config = false;
    unsigned front_elements = 0, side_elements = 0, back_elements = 0, lfe_elements = 0; // PCE element counts.
};
// Parses without allocating. Ready: AAC-LC with 1024-sample frames and a speaker layout representable as a
// WAVEFORMATEXTENSIBLE mask (channel configurations 1-7, 11, 12, or a PCE without coupling channels). Unsupported:
// well-formed but outside that (other object types, 960-sample frames, core coders, 22.2, coupling, layouts
// with no speaker bit). Invalid: reserved values or inconsistent fields. Truncated: the bytes end early. Fields
// parsed before the failure are left filled in (object_type, sample_rate, channel_configuration).
AacConfigStatus ParseAacConfig(std::span<const std::byte> asc, AacConfig& config) noexcept;
// Placement of decoded PCM (channels in `mask` bit order) on the engine's output layouts: mono and stereo pass
// through; 3- to 6-channel programmes land on 5.1 (FL FR FC LFE BL BR). Side surrounds take the surround pair, a
// back centre is split to both surrounds at -3 dB, missing speakers stay silent. `mix[out][in]` holds the gains.
// False: more than 6 channels, or a speaker with no 5.1 slot (front-of-centre pairs).
struct ChannelMap {
    unsigned in_channels = 0, out_channels = 0;
    std::uint32_t out_mask = 0;
    bool identity = true;
    float mix[6][6]{};
};
bool MapToOutput(std::uint32_t mask, ChannelMap& map) noexcept;
}
