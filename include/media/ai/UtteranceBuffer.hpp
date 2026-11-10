#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace media::ai {
// The seam between a streaming recogniser and the command grammar. A local recogniser emits an utterance as UTF-8
// pieces (words, or sub-word tokens that continue the previous piece); this collects them in fixed storage, and at
// the end of the utterance Text() is what LocalIntentDispatcher::Dispatch parses: the same text a keyboard would
// have typed. No allocation; one owner thread. An utterance longer than the grammar accepts is marked Overflowed and
// keeps its beginning, so the dispatcher refuses it as TooLong would rather than act on a truncated command.
class UtteranceBuffer final {
public:
    static constexpr unsigned kCapacity = 512; // LocalIntentDispatcher::kMaxCommandBytes.
    // `word_start`: the piece begins a new word (a space is inserted after earlier text in scripts that use one; a
    // recogniser for a spaceless script passes false throughout). False: no room, the utterance is now Overflowed.
    bool Push(std::string_view piece, bool word_start = true) noexcept {
        const unsigned space = word_start && bytes_ ? 1u : 0u;
        if (overflowed_ || piece.size() + space > kCapacity - bytes_) { overflowed_ = true; return false; }
        if (space) text_[bytes_++] = ' ';
        std::memcpy(text_ + bytes_, piece.data(), piece.size()); bytes_ += static_cast<unsigned>(piece.size());
        return true;
    }
    // A recogniser revising its hypothesis replaces the whole text.
    bool Replace(std::string_view text) noexcept { Clear(); return Push(text, false); }
    void Clear() noexcept { bytes_ = 0; overflowed_ = false; }
    std::string_view Text() const noexcept { return {text_, bytes_}; }
    bool Overflowed() const noexcept { return overflowed_; }
    bool Empty() const noexcept { return bytes_ == 0; }
private:
    char text_[kCapacity]{};
    unsigned bytes_ = 0;
    bool overflowed_ = false;
};
}
