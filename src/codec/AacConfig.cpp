#include "media/codec/AacConfig.hpp"
#include <bit>

namespace media::codec {
namespace {
class Bits {
public:
    explicit Bits(std::span<const std::byte> data) noexcept : data_(data) {}
    bool Read(unsigned count, unsigned& value) noexcept {
        value = 0;
        if (count > 32 || position_ + count > data_.size() * 8) { position_ = data_.size() * 8 + 1; return false; }
        for (unsigned i = 0; i < count; ++i, ++position_)
            value = (value << 1) | ((std::to_integer<unsigned>(data_[position_ / 8]) >> (7 - position_ % 8)) & 1u);
        return true;
    }
    void Align() noexcept { position_ = (position_ + 7) / 8 * 8; }
    std::size_t Position() const noexcept { return position_; }
private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};
constexpr unsigned kRates[13]{96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
// Speakers of the fixed channel configurations; 0 = reserved or not representable.
constexpr std::uint32_t kConfigurationMask[15]{
    0, speaker::Mono, speaker::Stereo, speaker::Stereo | speaker::FrontCenter,
    speaker::Stereo | speaker::FrontCenter | speaker::BackCenter,
    speaker::Stereo | speaker::FrontCenter | speaker::BackLeft | speaker::BackRight,
    speaker::Surround51,
    // 7: centre, an inner front pair, the outer front pair, surrounds and LFE: KSAUDIO_SPEAKER_7POINT1 (wide).
    speaker::Surround51 | speaker::FrontLeftOfCenter | speaker::FrontRightOfCenter,
    0, 0, 0,
    speaker::Stereo | speaker::FrontCenter | speaker::LowFrequency | speaker::BackCenter | speaker::SideLeft | speaker::SideRight, // 11: 6.1
    speaker::Surround71, // 12: 7.1 with side and rear surround pairs.
    0, 0};               // 13 (22.2) and 14 (7.1 with top speakers) have no WAVEFORMATEXTENSIBLE layout here.
// Places one PCE element (is_cpe) of a group on the next free speakers of that group.
bool Place(unsigned group, bool pair, std::uint32_t& mask) noexcept {
    const auto take = [&](std::uint32_t bits) { if (mask & bits) return false; mask |= bits; return true; };
    switch (group) {
    case 0: // Front: a single element is the centre; pairs fill the outer pair, then the inner pair.
        if (!pair) return take(speaker::FrontCenter);
        return !(mask & speaker::FrontLeft) ? take(speaker::Stereo) : take(speaker::FrontLeftOfCenter | speaker::FrontRightOfCenter);
    case 1: return pair && take(speaker::SideLeft | speaker::SideRight);          // Side: one pair.
    case 2: return pair ? take(speaker::BackLeft | speaker::BackRight) : take(speaker::BackCenter); // Back: a pair and/or centre.
    default: return false;
    }
}
}
bool MapToOutput(std::uint32_t mask, ChannelMap& map) noexcept {
    map = {};
    map.in_channels = static_cast<unsigned>(std::popcount(mask));
    if (!map.in_channels || map.in_channels > 6) return false;
    if (mask == speaker::Mono || mask == speaker::Stereo || mask == speaker::Surround51) {
        map.out_channels = map.in_channels; map.out_mask = mask;
        for (unsigned c = 0; c < map.in_channels; ++c) map.mix[c][c] = 1;
        return true;
    }
    map.out_channels = 6; map.out_mask = speaker::Surround51; map.identity = false;
    unsigned source = 0;
    for (std::uint32_t bit = 1; bit <= speaker::SideRight; bit <<= 1) {
        if (!(mask & bit)) continue;
        switch (bit) {
        case speaker::FrontLeft: map.mix[0][source] = 1; break;
        case speaker::FrontRight: map.mix[1][source] = 1; break;
        case speaker::FrontCenter: map.mix[2][source] = 1; break;
        case speaker::LowFrequency: map.mix[3][source] = 1; break;
        case speaker::BackLeft: case speaker::SideLeft: map.mix[4][source] = 1; break;
        case speaker::BackRight: case speaker::SideRight: map.mix[5][source] = 1; break;
        case speaker::BackCenter: map.mix[4][source] = map.mix[5][source] = 0.70710678f; break;
        default: map = {}; return false; // Front-of-centre pair: no 5.1 slot.
        }
        ++source;
    }
    // Bits above SideRight (top speakers) have no slot either.
    if (source != map.in_channels) { map = {}; return false; }
    return true;
}
AacConfigStatus ParseAacConfig(std::span<const std::byte> asc, AacConfig& config) noexcept {
    config = {};
    Bits bits(asc);
    unsigned value = 0, index = 0;
    if (!bits.Read(5, config.object_type)) return AacConfigStatus::Truncated;
    if (config.object_type == 31) { if (!bits.Read(6, value)) return AacConfigStatus::Truncated; config.object_type = 32 + value; }
    if (!bits.Read(4, index)) return AacConfigStatus::Truncated;
    if (index == 15) { if (!bits.Read(24, config.sample_rate)) return AacConfigStatus::Truncated; }
    else if (index < 13) config.sample_rate = kRates[index];
    else return AacConfigStatus::Invalid;
    if (!bits.Read(4, config.channel_configuration)) return AacConfigStatus::Truncated;
    if (!config.sample_rate) return AacConfigStatus::Invalid;
    if (config.object_type != 2) return AacConfigStatus::Unsupported; // HE-AAC (5, 29), Main, LTP, ER, USAC, ...
    // GASpecificConfig.
    unsigned frame_length = 0, core = 0, extension = 0;
    if (!bits.Read(1, frame_length) || !bits.Read(1, core)) return AacConfigStatus::Truncated;
    if (core && !bits.Read(14, value)) return AacConfigStatus::Truncated;
    if (!bits.Read(1, extension)) return AacConfigStatus::Truncated;
    if (frame_length || core) return AacConfigStatus::Unsupported; // 960-sample frames; scalable core coders.
    if (extension) return AacConfigStatus::Invalid;                // Only error-resilient object types extend.
    if (config.channel_configuration) {
        if (config.channel_configuration >= 15) return AacConfigStatus::Invalid;
        config.channel_mask = kConfigurationMask[config.channel_configuration];
        if (!config.channel_mask) return config.channel_configuration == 13 || config.channel_configuration == 14 ?
            AacConfigStatus::Unsupported : AacConfigStatus::Invalid;
        config.channels = static_cast<unsigned>(std::popcount(config.channel_mask));
        return AacConfigStatus::Ready;
    }
    // program_config_element().
    config.program_config = true;
    unsigned tag = 0, profile = 0, pce_index = 0, front = 0, side = 0, back = 0, lfe = 0, assoc = 0, coupling = 0, flag = 0;
    if (!bits.Read(4, tag) || !bits.Read(2, profile) || !bits.Read(4, pce_index) || !bits.Read(4, front) || !bits.Read(4, side) ||
        !bits.Read(4, back) || !bits.Read(2, lfe) || !bits.Read(3, assoc) || !bits.Read(4, coupling)) return AacConfigStatus::Truncated;
    for (unsigned mixdown = 0; mixdown < 2; ++mixdown) { // mono_mixdown, stereo_mixdown: element numbers only.
        if (!bits.Read(1, flag)) return AacConfigStatus::Truncated;
        if (flag && !bits.Read(4, value)) return AacConfigStatus::Truncated;
    }
    if (!bits.Read(1, flag)) return AacConfigStatus::Truncated; // matrix_mixdown_idx_present
    if (flag && !bits.Read(3, value)) return AacConfigStatus::Truncated;
    config.front_elements = front; config.side_elements = side; config.back_elements = back; config.lfe_elements = lfe;
    bool representable = coupling == 0 && lfe <= 1;
    std::uint32_t mask = 0; unsigned channels = 0;
    const unsigned groups[3]{front, side, back};
    for (unsigned group = 0; group < 3; ++group)
        for (unsigned i = 0; i < groups[group]; ++i) {
            unsigned pair = 0;
            if (!bits.Read(1, pair) || !bits.Read(4, tag)) return AacConfigStatus::Truncated;
            channels += pair ? 2 : 1;
            if (!Place(group, pair != 0, mask)) representable = false;
        }
    for (unsigned i = 0; i < lfe; ++i) { if (!bits.Read(4, tag)) return AacConfigStatus::Truncated; mask |= speaker::LowFrequency; ++channels; }
    for (unsigned i = 0; i < assoc; ++i) if (!bits.Read(4, tag)) return AacConfigStatus::Truncated;
    for (unsigned i = 0; i < coupling; ++i) if (!bits.Read(5, tag)) return AacConfigStatus::Truncated; // cc_element_is_ind_sw + tag.
    bits.Align(); // Relative to the start of the AudioSpecificConfig.
    unsigned comment = 0;
    if (!bits.Read(8, comment)) return AacConfigStatus::Truncated;
    for (unsigned i = 0; i < comment; ++i) if (!bits.Read(8, value)) return AacConfigStatus::Truncated;
    if (!channels) return AacConfigStatus::Invalid;
    config.channels = channels;
    if (!representable) return AacConfigStatus::Unsupported;
    config.channel_mask = mask;
    return AacConfigStatus::Ready;
}
}
