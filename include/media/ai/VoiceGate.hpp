#pragma once
#include "media/ai/UtteranceBuffer.hpp"
#include "media/audio/MicroVad.hpp"
#include <cstdint>
#include <string_view>

namespace media::ai {
// The recogniser's side of the voice path (docs/voice.md). It is given the speech of one utterance, slice by slice,
// and then the empty SpeechEnd slice, and writes what it has heard into the utterance's text (Push for a new piece,
// Replace for a revised hypothesis) at any of those calls. Silence never reaches it.
using SpeechRecogniser = void (*)(void* context, const audio::SpeechSlice&, UtteranceBuffer& text) noexcept;
// Called once per finished utterance with its text, which is what LocalIntentDispatcher::Dispatch takes. The view
// is valid during the call.
using UtteranceHandler = void (*)(void* context, std::string_view text, const audio::Utterance&) noexcept;
struct VoiceGateStatistics {
    std::uint32_t utterances = 0; // Closed by the detector.
    std::uint32_t handed = 0;     // Given to the handler.
    std::uint32_t too_short = 0;  // Less speech than min_speech_ms: a knock, a cough, a click that got through.
    std::uint32_t empty = 0;      // The recogniser wrote nothing.
    std::uint32_t overflowed = 0; // More text than a command may have.
    std::uint64_t speech_frames = 0; // Frames passed to the recogniser.
};
// Joins the voice activity detector to the text seam: PCM from the capture ring goes through MicroVad, and only the
// slices of an utterance reach the recogniser, which fills one UtteranceBuffer per utterance. SpeechStart clears the
// text; SpeechEnd hands it on. Text is never handed on for an utterance that was too short, that the recogniser left
// empty, that overflowed (a truncated command must not run), or that was cut by the detector's length limit.
// One owner thread (the capture ring's reader); fixed storage; no allocation.
class VoiceGate final {
public:
    void Configure(audio::MicroVad& vad, SpeechRecogniser recogniser, void* recogniser_context, UtteranceHandler handler,
                   void* handler_context, std::uint32_t min_speech_ms = 120) noexcept {
        vad_ = &vad; recogniser_ = recogniser; recogniser_context_ = recogniser_context; handler_ = handler; handler_context_ = handler_context;
        min_speech_ = static_cast<std::uint64_t>(vad.Options().sample_rate) * min_speech_ms / 1000;
        text_.Clear(); stats_ = {};
    }
    // Moves everything `source` holds (anything with `std::uint32_t Read(std::span<float>)`: Win32WasapiCapture)
    // through the detector. Returns the frames taken.
    template <class Source> std::uint64_t Pump(Source& source) noexcept { return vad_ ? vad_->Drain(source, &VoiceGate::Slice, this) : 0; }
    // The same for PCM already in hand.
    audio::VadState Process(std::span<const float> samples) noexcept {
        return vad_ ? vad_->Process(samples, &VoiceGate::Slice, this) : audio::VadState::Silence;
    }
    // A gap in the capture: the open utterance is abandoned, not handed on.
    void Reset() noexcept { if (vad_) vad_->Reset(); text_.Clear(); }
    [[nodiscard]] const UtteranceBuffer& Text() const noexcept { return text_; } // The open utterance's text so far.
    [[nodiscard]] const VoiceGateStatistics& Stats() const noexcept { return stats_; }
private:
    static void Slice(void* context, const audio::SpeechSlice& slice) noexcept {
        auto& self = *static_cast<VoiceGate*>(context);
        if (slice.state == audio::VadState::SpeechStart) self.text_.Clear();
        // The recogniser hears every slice, the empty SpeechEnd one included: a recogniser that decides on the whole
        // utterance writes its text there.
        self.stats_.speech_frames += slice.frames;
        if (self.recogniser_) self.recogniser_(self.recogniser_context_, slice, self.text_);
        if (slice.state != audio::VadState::SpeechEnd) return;
        const auto& utterance = self.vad_->Last();
        ++self.stats_.utterances;
        if (utterance.active < self.min_speech_) ++self.stats_.too_short;
        else if (self.text_.Overflowed() || utterance.truncated) ++self.stats_.overflowed;
        else if (self.text_.Empty()) ++self.stats_.empty;
        else { ++self.stats_.handed; if (self.handler_) self.handler_(self.handler_context_, self.text_.Text(), utterance); }
        self.text_.Clear();
    }
    audio::MicroVad* vad_ = nullptr;
    SpeechRecogniser recogniser_ = nullptr; void* recogniser_context_ = nullptr;
    UtteranceHandler handler_ = nullptr; void* handler_context_ = nullptr;
    std::uint64_t min_speech_ = 0;
    UtteranceBuffer text_;
    VoiceGateStatistics stats_{};
};
}
