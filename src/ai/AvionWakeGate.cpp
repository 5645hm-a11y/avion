#include "media/ai/AvionWakeGate.hpp"

namespace media::ai {
const char* Name(WakeDecision decision) noexcept {
    switch (decision) {
    case WakeDecision::Command: return "Command";
    case WakeDecision::Listening: return "Listening";
    case WakeDecision::WindowCommand: return "WindowCommand";
    case WakeDecision::Ignored: return "Ignored";
    }
    return "?";
}
void AvionWakeGate::Configure(const WakeOptions& options, WakeHandler handler, void* context) noexcept {
    options_ = options; handler_ = handler; context_ = context;
    window_frames_ = static_cast<std::uint64_t>(options.sample_rate) * options.window_ms / 1000;
    open_ = false; window_end_ = 0; stats_ = {};
}
bool AvionWakeGate::StripWake(std::string_view text, std::string_view& rest) noexcept {
    const auto separator = [](unsigned char c) { return c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' || c == '?' || c == ':' || c == ';' || c == '-'; };
    std::size_t at = 0;
    while (at < text.size() && separator(static_cast<unsigned char>(text[at]))) ++at;
    std::size_t end = 0;
    static constexpr char kLatin[] = "avion";
    if (text.size() - at >= 5) {
        bool same = true;
        for (std::size_t i = 0; i < 5 && same; ++i) {
            const auto c = static_cast<unsigned char>(text[at + i]);
            same = (c >= 'A' && c <= 'Z' ? c + 32 : c) == static_cast<unsigned char>(kLatin[i]);
        }
        if (same) end = at + 5;
    }
    static constexpr std::string_view kHebrew = "\xd7\x90\xd7\x91\xd7\x99\xd7\x95\xd7\x9f"; // אביון
    if (!end && text.substr(at, kHebrew.size()) == kHebrew) end = at + kHebrew.size();
    if (!end) { rest = text; return false; }
    // A word of its own: "avionics" or "avions" is not the name.
    if (end < text.size() && !separator(static_cast<unsigned char>(text[end]))) { rest = text; return false; }
    while (end < text.size() && separator(static_cast<unsigned char>(text[end]))) ++end;
    rest = text.substr(end);
    return true;
}
bool AvionWakeGate::SetWord(std::string_view word) noexcept {
    std::size_t first = 0, last = word.size();
    while (first < last && (word[first] == ' ' || word[first] == '\t')) ++first;
    while (last > first && (word[last - 1] == ' ' || word[last - 1] == '\t')) --last;
    if (last - first > kMaxWord) return false;
    if (first == last && !word.empty()) return false; // Blank.
    word_bytes_ = last - first;
    for (std::size_t i = 0; i < word_bytes_; ++i) {
        const auto c = static_cast<unsigned char>(word[first + i]);
        word_[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    word_[word_bytes_] = 0;
    return true;
}
bool AvionWakeGate::StripName(std::string_view text, std::string_view& rest) const noexcept {
    if (!word_bytes_) return StripWake(text, rest);
    const auto separator = [](unsigned char c) { return c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' || c == '?' || c == ':' || c == ';' || c == '-'; };
    std::size_t at = 0;
    while (at < text.size() && separator(static_cast<unsigned char>(text[at]))) ++at;
    bool same = text.size() - at >= word_bytes_;
    for (std::size_t i = 0; i < word_bytes_ && same; ++i) {
        const auto c = static_cast<unsigned char>(text[at + i]);
        same = (c >= 'A' && c <= 'Z' ? c + 32 : c) == static_cast<unsigned char>(word_[i]);
    }
    std::size_t end = at + word_bytes_;
    if (!same || (end < text.size() && !separator(static_cast<unsigned char>(text[end])))) { rest = text; return false; } // A word of its own.
    while (end < text.size() && separator(static_cast<unsigned char>(text[end]))) ++end;
    rest = text.substr(end);
    return true;
}
WakeDecision AvionWakeGate::Decide(std::string_view text, const audio::Utterance& utterance, std::string_view& command) noexcept {
    ++stats_.utterances;
    command = {};
    std::string_view rest;
    const bool named = StripName(text, rest);
    if (!options_.required) {
        if (options_.push_to_talk && !Talking()) { ++stats_.ignored; return WakeDecision::Ignored; } // Talk not held.
        command = rest; ++stats_.commands; return WakeDecision::Command;
    }
    // A window that has run out by the time this utterance began.
    if (open_ && utterance.onset_frame > window_end_) { open_ = false; ++stats_.expired; }
    if (named && rest.empty()) {
        // The name alone: listen for the next utterance, from the end of this one.
        open_ = true; window_end_ = utterance.end_frame + window_frames_; ++stats_.wakes;
        return WakeDecision::Listening;
    }
    if (named) { open_ = false; command = rest; ++stats_.commands; return WakeDecision::Command; }
    if (open_) { open_ = false; command = text; ++stats_.window_commands; return WakeDecision::WindowCommand; }
    ++stats_.ignored;
    return WakeDecision::Ignored;
}
void AvionWakeGate::Handle(void* context, std::string_view text, const audio::Utterance& utterance) noexcept {
    auto& self = *static_cast<AvionWakeGate*>(context);
    std::string_view command;
    const auto decision = self.Decide(text, utterance, command);
    if (decision != WakeDecision::Ignored && self.handler_) self.handler_(self.context_, decision, command, utterance);
}
}
