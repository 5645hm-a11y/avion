#include "media/subtitle/SubtitleSearch.hpp"
#include <cstring>
#include <memory>

namespace media::subtitle {
namespace {
constexpr unsigned kCue = text::kFold | text::kMarkup;
std::uint32_t Mix(std::uint32_t x) noexcept { x *= 0x9e3779b1u; x ^= x >> 15; x *= 0x85ebca77u; return x ^ (x >> 13); }
}
bool SearchIndex::Sign(std::string_view folded, Signature& s) noexcept {
    s = {};
    std::uint32_t previous = 0, scalar = 0;
    while (!folded.empty()) {
        if (!Utf8(folded, scalar)) return false;
        s.letters |= std::uint64_t{1} << (Mix(scalar) & 63);
        if (previous) { const auto bit = Mix(previous * 0x10001u + scalar) % 192; s.pairs[bit / 64] |= std::uint64_t{1} << (bit % 64); }
        previous = scalar;
    }
    return true;
}
bool SearchIndex::Prepare(core::LinearMemoryArena& arena, const SubtitleTrack& track) noexcept {
    const auto cues = track.Cues();
    if (signatures_ || cues.empty() || arena.IsSealed()) return false;
    // The parser admitted only UTF-8 cues of at most kMaxTextBytes; a cue that does not normalise is left empty and
    // never matches. Pass 1 sizes the folded text, pass 2 writes it.
    char scratch[SubtitleTrack::kMaxTextBytes];
    std::size_t total = 0;
    for (const auto& cue : cues) {
        const auto n = text::Normalize(cue.text, scratch, sizeof(scratch), kCue);
        if (n != text::kInvalid) total += n;
    }
    if (total > UINT32_MAX) return false;
    auto* signatures = static_cast<Signature*>(arena.Allocate(sizeof(Signature) * cues.size(), alignof(Signature)));
    auto* offsets = static_cast<std::uint32_t*>(arena.Allocate(sizeof(std::uint32_t) * (cues.size() + 1), alignof(std::uint32_t)));
    auto* folded = static_cast<char*>(arena.Allocate(total ? total : 1));
    if (!signatures || !offsets || !folded) return false;
    std::size_t used = 0;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        offsets[i] = static_cast<std::uint32_t>(used);
        std::construct_at(signatures + i);
        const auto n = text::Normalize(cues[i].text, scratch, sizeof(scratch), kCue);
        if (n == text::kInvalid || n > total - used) continue;
        std::memcpy(folded + used, scratch, n);
        (void)Sign({folded + used, n}, signatures[i]); used += n;
    }
    offsets[cues.size()] = static_cast<std::uint32_t>(used);
    track_ = &track; signatures_ = signatures; offsets_ = offsets; folded_ = folded;
    bytes_ = sizeof(Signature) * cues.size() + sizeof(std::uint32_t) * (cues.size() + 1) + total;
    return true;
}
unsigned SearchIndex::Scan(std::string_view query, unsigned match_index, SearchMatch* match, MatchMode mode) const noexcept {
    if (!signatures_ || query.empty() || query.size() > kMaxQueryBytes) return 0;
    char folded_query[kMaxQueryBytes], exact_query[kMaxQueryBytes];
    const auto folded_bytes = text::Normalize(query, folded_query, sizeof(folded_query), text::kFold);
    if (folded_bytes == text::kInvalid || !folded_bytes) return 0;
    const std::string_view needle{folded_query, folded_bytes};
    if (needle == " ") return 0; // Blank.
    Signature wanted; if (!Sign(needle, wanted)) return 0;
    // An exact match is a folded match whose unfolded text matches too: the index narrows it, the cue confirms it.
    std::string_view exact;
    if (mode == MatchMode::Exact) {
        const auto n = text::Normalize(query, exact_query, sizeof(exact_query), 0);
        if (n == text::kInvalid || !n) return 0;
        exact = {exact_query, n};
    }
    const auto cues = track_->Cues();
    char plain[SubtitleTrack::kMaxTextBytes];
    unsigned found = 0;
    const auto shown = track_->Shown();
    for (std::size_t i = 0; i < cues.size(); ++i) {
        if (!((shown >> cues[i].channel) & 1u)) continue; // Another language's cue: not on screen, not searched.
        const auto& s = signatures_[i];
        if ((s.letters & wanted.letters) != wanted.letters || (s.pairs[0] & wanted.pairs[0]) != wanted.pairs[0] ||
            (s.pairs[1] & wanted.pairs[1]) != wanted.pairs[1] || (s.pairs[2] & wanted.pairs[2]) != wanted.pairs[2]) continue;
        if (std::string_view{folded_ + offsets_[i], offsets_[i + 1] - offsets_[i]}.find(needle) == std::string_view::npos) continue;
        if (mode == MatchMode::Exact) {
            const auto n = text::Normalize(cues[i].text, plain, sizeof(plain), text::kMarkup);
            if (n == text::kInvalid || std::string_view{plain, n}.find(exact) == std::string_view::npos) continue;
        }
        if (match && found == match_index) { *match = {static_cast<unsigned>(i), cues[i].start, cues[i].end}; return found + 1; }
        ++found;
    }
    return match ? 0 : found;
}
bool SearchIndex::Find(std::string_view query, unsigned match_index, SearchMatch& match, MatchMode mode) const noexcept {
    return Scan(query, match_index, &match, mode) != 0;
}
unsigned SearchIndex::Count(std::string_view query, MatchMode mode) const noexcept { return Scan(query, 0, nullptr, mode); }
std::string_view SearchIndex::Folded(unsigned cue) const noexcept {
    if (!signatures_ || cue >= track_->Cues().size()) return {};
    return {folded_ + offsets_[cue], offsets_[cue + 1] - offsets_[cue]};
}
}
