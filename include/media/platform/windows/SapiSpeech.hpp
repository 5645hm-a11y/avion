#pragma once
#include "media/ai/CustomWakeWord.hpp"
#include <cstddef>

struct ISpObjectToken;
namespace media::platform::windows {
// The system's voices as a speech source for custom wake words (ai::SpeechSource; docs/voice.md, "Custom wake words"):
// Windows' own speech synthesiser (SAPI 5, desktop and OneCore voices, one voice per speaker), rendering into memory at
// 16 kHz. Setup only (Prepare): COM is initialised on the calling thread when it is not already, and SAPI allocates
// inside its calls. Nothing is played or written anywhere.
class SapiSpeech final {
public:
    static constexpr unsigned kMaxVoices = ai::CustomWakeWord::kMaxSourceVoices;
    SapiSpeech() noexcept = default;
    SapiSpeech(const SapiSpeech&) = delete;
    SapiSpeech& operator=(const SapiSpeech&) = delete;
    ~SapiSpeech() noexcept { Close(); }
    // Finds the voices; `exclude` names speakers to leave out ("Mark": tests hold voices out). False when there is none.
    [[nodiscard]] bool Open(const wchar_t* const* exclude = nullptr, unsigned excluded = 0) noexcept;
    void Close() noexcept;
    [[nodiscard]] unsigned Voices() const noexcept { return count_; }
    [[nodiscard]] const wchar_t* VoiceName(unsigned i) const noexcept { return i < count_ ? names_[i] : L""; }
    [[nodiscard]] ai::SpeechSource Source() noexcept { return {&SapiSpeech::Speak, this, count_}; }
    // `text` (UTF-8, letters, spaces and apostrophes only) said by voice `voice` at `rate` (-1, 0, +1: SAPI rates -3, 0, +3).
    static std::size_t Speak(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept;
private:
    ISpObjectToken* tokens_[kMaxVoices]{};
    wchar_t names_[kMaxVoices][32]{};
    unsigned count_ = 0;
    bool com_ = false; // This object initialised COM on its thread (and uninitialises it at Close).
};
}
