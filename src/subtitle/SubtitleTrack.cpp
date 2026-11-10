#include "media/subtitle/SubtitleTrack.hpp"
#include <algorithm>
#include <cstring>
#include <memory>

namespace media::subtitle {
namespace {
std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
bool Number(std::string_view s, unsigned& value) noexcept {
    value = 0; if (s.empty() || s.size() > 7) return false;
    for (char c : s) { if (c < '0' || c > '9') return false; value = value * 10 + static_cast<unsigned>(c - '0'); }
    return true;
}
bool Timestamp(std::string_view s, bool vtt, std::int64_t& ticks) noexcept {
    const auto first = s.find(':'); if (first == s.npos) return false;
    const auto second = s.find(':', first + 1);
    unsigned hours = 0, minutes = 0, seconds = 0, millis = 0;
    if (second != s.npos) {
        if (first < 2 || second - first != 3 || !Number(s.substr(0, first), hours) || !Number(s.substr(first + 1, 2), minutes)) return false;
        s.remove_prefix(second + 1);
    } else {
        if (!vtt || first != 2 || !Number(s.substr(0, first), minutes)) return false;
        s.remove_prefix(first + 1);
    }
    if (s.size() != 6 || (s[2] != '.' && (vtt || s[2] != ',')) || !Number(s.substr(0, 2), seconds) || !Number(s.substr(3), millis) || minutes >= 60 || seconds >= 60) return false;
    ticks = (static_cast<std::int64_t>(hours) * 3600000 + minutes * 60000 + seconds * 1000 + millis) * 10000;
    return true;
}
bool Timing(std::string_view line, bool vtt, Cue& cue) noexcept {
    const auto arrow = line.find("-->"); if (arrow == line.npos) return false;
    auto end = Trim(line.substr(arrow + 3));
    const auto space = end.find_first_of(" \t");
    if (space != end.npos) { if (!vtt) return false; end = end.substr(0, space); } // VTT settings admitted, default placement.
    return Timestamp(Trim(line.substr(0, arrow)), vtt, cue.start) && Timestamp(end, vtt, cue.end) && cue.end > cue.start;
}
}
CueReader::CueReader(std::string_view source) noexcept : source_(source) {
    if (source_.starts_with("\xef\xbb\xbf")) source_.remove_prefix(3);
    if (source_.starts_with("WEBVTT") && (source_.size() == 6 || source_[6] == '\r' || source_[6] == '\n' || source_[6] == ' ' || source_[6] == '\t')) {
        vtt_ = true; (void)ReadLine();
        // Header metadata ends at the first blank line.
        while (position_ < source_.size() && !ReadLine().empty()) {}
    }
}
std::string_view CueReader::ReadLine() noexcept {
    const auto begin = position_;
    while (position_ < source_.size() && source_[position_] != '\r' && source_[position_] != '\n') ++position_;
    const auto line = source_.substr(begin, position_ - begin);
    if (position_ < source_.size() && source_[position_++] == '\r' && position_ < source_.size() && source_[position_] == '\n') ++position_;
    ++line_; blank_ = Trim(line).empty(); return line;
}
void CueReader::Resync() noexcept {
    while (!blank_ && position_ < source_.size()) (void)ReadLine();
    valid_ = true;
}
ParseStatus CueReader::Next(Cue& cue) noexcept {
    if (!valid_) return ParseStatus::Invalid;
    cue = {};
    while (position_ < source_.size()) {
        auto line = ReadLine(); if (Trim(line).empty()) continue;
        if (vtt_ && (line == "NOTE" || line.starts_with("NOTE ") || line.starts_with("NOTE\t") || line == "STYLE" || line == "REGION")) {
            while (position_ < source_.size() && !ReadLine().empty()) {} continue;
        }
        if (line.find("-->") == line.npos) {
            unsigned id = 0;
            if ((!vtt_ && !Number(Trim(line), id)) || position_ == source_.size()) { valid_ = false; return ParseStatus::Invalid; }
            line = ReadLine();
        }
        if (!Timing(line, vtt_, cue)) { valid_ = false; return ParseStatus::Invalid; }
        const auto begin = position_; auto end = begin;
        while (position_ < source_.size()) {
            const auto text_line = ReadLine(); if (Trim(text_line).empty()) break;
            end = static_cast<std::size_t>(text_line.data() - source_.data()) + text_line.size();
        }
        cue.text = source_.substr(begin, end - begin);
        if (cue.text.empty() || cue.text.size() > SubtitleTrack::kMaxTextBytes) { valid_ = false; return ParseStatus::Invalid; }
        auto text = cue.text; std::uint32_t scalar = 0;
        while (!text.empty()) if (!Utf8(text, scalar)) { valid_ = false; return ParseStatus::Invalid; }
        return ParseStatus::CueReady;
    }
    return ParseStatus::End;
}
bool SubtitleTrack::Prepare(core::LinearMemoryArena& arena, std::string_view source) noexcept {
    return Prepare(arena, std::span<const std::string_view>{&source, 1});
}
bool SubtitleTrack::Prepare(core::LinearMemoryArena& arena, std::span<const std::string_view> sources) noexcept {
    std::size_t total = 0;
    for (const auto source : sources) total += source.size();
    if (cues_ || arena.IsSealed() || sources.empty() || sources.size() > kMaxChannels || !total || total > 8u * 1024 * 1024) return false;
    // Pass 1 counts valid cues (skipping broken blocks); pass 2 fills the exact table from the arena copy.
    const auto scan = [](std::string_view text, std::uint8_t channel, Cue* table, unsigned count, Statistics& stats, unsigned& error_line) noexcept {
        CueReader reader(text); Cue cue;
        for (ParseStatus status; (status = reader.Next(cue)) != ParseStatus::End;) {
            if (status == ParseStatus::Invalid) { if (!error_line) error_line = reader.Line(); ++stats.skipped; reader.Resync(); continue; }
            if (count == kMaxCues) { ++stats.dropped; continue; }
            cue.channel = channel;
            if (table) std::construct_at(table + count, cue);
            ++count;
        }
        return count;
    };
    Statistics stats{}; unsigned error_line = 0, count = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) count = scan(sources[i], static_cast<std::uint8_t>(i), nullptr, count, stats, error_line);
    error_line_ = error_line; stats_ = stats;
    if (!count) return false;
    auto* text = static_cast<char*>(arena.Allocate(total));
    auto* table = static_cast<Cue*>(arena.Allocate(sizeof(Cue) * count, alignof(Cue)));
    if (!text || !table) return false;
    stats = {}; error_line = 0;
    unsigned filled = 0; std::size_t at = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].empty()) std::memcpy(text + at, sources[i].data(), sources[i].size());
        const unsigned before = filled;
        filled = scan({text + at, sources[i].size()}, static_cast<std::uint8_t>(i), table, filled, stats, error_line);
        per_channel_[i] = filled - before; at += sources[i].size();
    }
    if (filled != count) return false;
    channels_ = static_cast<unsigned>(sources.size());
    std::sort(table, table + count, [](const Cue& a, const Cue& b) { return a.start < b.start || (a.start == b.start && a.end < b.end); });
    std::int64_t prefix = 0;
    for (unsigned i = 0; i < count; ++i) {
        prefix = (std::max)(prefix, table[i].end); table[i].prefix_end = prefix;
        unsigned active = 1;
        for (unsigned j = i; j > 0 && table[j - 1].prefix_end > table[i].start; --j) if (table[j - 1].end > table[i].start) ++active;
        stats_.max_active = (std::max)(stats_.max_active, active);
    }
    cues_ = table; count_ = count; return true;
}
unsigned SubtitleTrack::Select(std::int64_t time, std::span<unsigned> indices) const noexcept {
    unsigned low = 0, high = count_;
    while (low < high) { const auto mid = low + (high - low) / 2; if (cues_[mid].start <= time) low = mid + 1; else high = mid; }
    unsigned found = 0;
    const auto shown = Shown();
    while (low && cues_[low - 1].prefix_end > time && found < indices.size()) {
        --low; if (cues_[low].end > time && ((shown >> cues_[low].channel) & 1u)) indices[found++] = low;
    }
    std::reverse(indices.begin(), indices.begin() + found); return found;
}
}
