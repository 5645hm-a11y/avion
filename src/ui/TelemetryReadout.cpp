#include "media/ui/TelemetryReadout.hpp"
#include <cstring>
namespace media::ui {
namespace {
// Text assembled in place (no formatting library: snprintf allocates in some C runtimes). A part goes in whole or not at
// all, so a cut never splits a character.
struct Text {
    char* out; unsigned capacity, n = 0;
    Text(char* o, unsigned c) noexcept : out(o), capacity(c) { if (out && capacity) out[0] = 0; }
    bool Put(const char* part) noexcept {
        if (!out || !part) return false;
        const auto size = static_cast<unsigned>(std::strlen(part));
        if (n + size + 1 > capacity) return false;
        std::memcpy(out + n, part, size); n += size; out[n] = 0; return true;
    }
    // `value` / 10^decimals, rounded half away from zero, with that many decimals.
    void Fixed(std::int64_t value, unsigned decimals) noexcept {
        char digits[32]; unsigned d = 0;
        const bool negative = value < 0; std::uint64_t v = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        do { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && d < 24);
        while (d <= decimals) digits[d++] = '0';
        char text[40]; unsigned t = 0;
        if (negative) text[t++] = '-';
        for (unsigned i = d; i-- > 0;) { text[t++] = digits[i]; if (i == decimals && decimals) text[t++] = '.'; }
        text[t] = 0; Put(text);
    }
    void Whole(std::uint64_t value) noexcept { Fixed(static_cast<std::int64_t>(value), 0); }
    // 100 ns as milliseconds, one decimal ("18.2 ms").
    void Ms(std::int64_t ticks) noexcept { Fixed((ticks + (ticks >= 0 ? 500 : -500)) / 1000, 1); Put(" ms"); }
    // Media time as m:ss.t, or h:mm:ss.t.
    void Clock(std::int64_t ticks) noexcept {
        if (ticks < 0) ticks = 0;
        const std::int64_t tenths = (ticks + 500000) / 1000000, seconds = tenths / 10, h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
        if (h) { Whole(static_cast<std::uint64_t>(h)); Put(":"); if (m < 10) Put("0"); }
        Whole(static_cast<std::uint64_t>(m)); Put(":"); if (s < 10) Put("0");
        Whole(static_cast<std::uint64_t>(s)); Put("."); Whole(static_cast<std::uint64_t>(tenths % 10));
    }
    // How long ago: "now", "4 s ago", "3 min ago".
    void Ago(std::int64_t ticks) noexcept {
        const std::int64_t seconds = ticks / 10000000;
        if (seconds < 1) { Put("just now"); return; }
        if (seconds < 120) { Whole(static_cast<std::uint64_t>(seconds)); Put(" s ago"); return; }
        Whole(static_cast<std::uint64_t>(seconds / 60)); Put(" min ago");
    }
    void Line() noexcept { if (n && out[n - 1] != '\n') Put("\n"); }
};
constexpr const char* kDot = " \xc2\xb7 ";
constexpr const char* kTimes = "\xc3\x97";
constexpr const char* kArrow = " \xe2\x86\x92 ";
const char* Channels(unsigned channels) noexcept {
    switch (channels) {
    case 1: return "mono"; case 2: return "stereo"; case 6: return "5.1"; case 8: return "7.1";
    default: return nullptr;
    }
}
void PutChannels(Text& t, unsigned channels) noexcept {
    if (const char* name = Channels(channels)) t.Put(name); else { t.Whole(channels); t.Put(" ch"); }
}
// What a dropped picture waited for (D3D12Renderer::Wait).
const char* Waited(std::uint8_t mask) noexcept {
    if (mask & 1) return "its GPU conversion";
    if (mask & 4) return "the display queue";
    if (mask & 2) return "a back buffer";
    if (mask & 8) return "Present";
    return "nothing (the presentation thread was late)";
}
const char* Short(render::QualityTier tier) noexcept {
    return tier == render::QualityTier::Neural ? "Neural" : tier == render::QualityTier::Fast ? "Fast" : "Edge-directed";
}
// The reason for a tier change, with the measurement behind it.
void Why(Text& t, const TelemetrySnapshot& s, const render::QualityGovernor::Change& c, bool long_form) noexcept {
    using Reason = render::QualityGovernor::Reason;
    switch (c.reason) {
    case Reason::Overload:
        t.Put(long_form ? render::Name(c.from) : Short(c.from)); t.Put(" needed "); t.Ms(c.p95);
        if (long_form) t.Put(" per picture");
        if (c.budget > 0 && c.p95 > c.budget) { t.Put(", over the whole "); t.Ms(c.budget); t.Put(" frame"); }
        else if (c.budget > 0) {
            t.Put(", over "); t.Whole(static_cast<std::uint64_t>(s.step_down * 100 + 0.5f)); t.Put("% of the "); t.Ms(c.budget); t.Put(" frame");
            if (long_form) { t.Put(" ("); t.Ms(static_cast<std::int64_t>(static_cast<double>(c.budget) * s.step_down)); t.Put(")"); }
        }
        break;
    case Reason::Headroom:
        t.Put(long_form ? render::Name(c.from) : Short(c.from)); t.Put(" took "); t.Ms(c.p95);
        if (c.budget > 0) {
            t.Put(", under "); t.Whole(static_cast<std::uint64_t>(s.step_up * 100 + 0.5f)); t.Put("% of the "); t.Ms(c.budget); t.Put(" frame");
        }
        if (long_form) t.Put(": room for the better tier");
        break;
    case Reason::Manual: t.Put(long_form ? "Chosen by hand (the N key or a command)" : "chosen by hand"); break;
    case Reason::Unavailable:
        t.Put(long_form ? "Neural super-resolution cannot run: the picture is not being enlarged" : "Neural cannot run: the picture is not enlarged");
        break;
    }
}
// The newest event worth naming on the readout's last line.
bool Notable(core::FlightEvent kind) noexcept {
    using E = core::FlightEvent;
    switch (kind) {
    case E::SeekLanded: case E::TierChange: case E::Drop: case E::TrackSwitched: case E::AudioMoved: case E::Underrun:
    case E::DeviceRecovered: case E::LoopSet: case E::LoopCleared: case E::LoopWrap: case E::Speed: case E::Zoom:
    case E::NightMode: case E::Bookmark: return true;
    default: return false;
    }
}
}
unsigned DescribeEvent(const core::FlightRecord& r, std::int64_t now, char* out, unsigned capacity) noexcept {
    using E = core::FlightEvent;
    Text t(out, capacity);
    switch (r.kind) {
    case E::SeekRequested: t.Put(r.a == 1 ? "Scrub to " : "Seek to "); t.Clock(r.c); break;
    case E::SeekLanded: t.Put("Seek landed at "); t.Clock(r.c); t.Put(" in "); t.Ms(r.d); break;
    case E::SeekApplied: t.Put((r.a & 2) ? "Queue seek to " : (r.a & 1) ? "Seek (continued) to " : "Seek to "); t.Clock(r.c); break;
    case E::TierChange:
        t.Put(render::Name(static_cast<render::QualityTier>(r.a >> 8))); t.Put(kArrow); t.Put(render::Name(static_cast<render::QualityTier>(r.a & 0xff)));
        if (r.b == static_cast<std::uint32_t>(render::QualityGovernor::Reason::Overload)) { t.Put(" (GPU "); t.Ms(r.c); t.Put(")"); }
        break;
    case E::Drop: t.Put("Dropped the picture at "); t.Clock(r.c); t.Put(" (waited for "); t.Put(Waited(static_cast<std::uint8_t>(r.a))); t.Put(")"); break;
    case E::Pause: t.Put("Paused at "); t.Clock(r.c); break;
    case E::Resume: t.Put("Resumed at "); t.Clock(r.c); break;
    case E::Speed: t.Put("Speed "); t.Fixed(r.b / 10, 2); t.Put(kTimes); break;
    case E::Zoom: if (r.b <= 100) t.Put("Zoom off"); else { t.Put("Zoom "); t.Fixed(r.b / 10, 1); t.Put(kTimes); } break;
    case E::TrackRequested: t.Put(r.a == 1 ? "Audio track " : "Subtitle track "); t.Whole(r.b); t.Put(" asked for"); break;
    case E::TrackSwitched: t.Put("Audio track "); t.Whole(r.b); t.Put(r.a ? " spliced in at " : " started at "); t.Clock(r.c); break;
    case E::AudioMoved: t.Put("Audio moved to the default output device"); break;
    case E::Underrun: t.Put("Audio ran dry; the clock re-anchored"); break;
    case E::DeviceLost: t.Put("GPU device lost"); break;
    case E::DeviceRecovered: t.Put("GPU device rebuilt in "); t.Ms(r.c); break;
    case E::LoopSet: t.Put(r.a ? "Sentence loop " : "Loop "); t.Clock(r.c); t.Put(" \xe2\x80\x93 "); t.Clock(r.d); break;
    case E::LoopCleared: t.Put("Loop cleared"); break;
    case E::LoopWrap: t.Put("Loop turned back to "); t.Clock(r.d); t.Put(" ("); t.Whole(r.b); t.Put(r.b == 1 ? " turn)" : " turns)"); break;
    case E::Stats: t.Put("Telemetry asked for"); break;
    case E::NightMode: t.Put(r.a ? "Night mode on" : "Night mode off"); break;
    case E::Bookmark:
        if (r.a == 3) { t.Put("Frame pinned at "); t.Clock(r.c); break; }
        t.Put(r.a == 1 ? "Bookmark " : "Went to bookmark "); t.Whole(r.b); t.Put(" at "); t.Clock(r.c); break;
    default: t.Put(core::Name(r.kind)); break;
    }
    if (now > 0 && r.time > 0) { t.Put(kDot); t.Ago(now - r.time); }
    return t.n;
}
unsigned ComposeStats(const TelemetrySnapshot& s, char* out, unsigned capacity) noexcept {
    Text t(out, capacity);
    // Quality: the tier, its mode, and what it costs against the frame.
    t.Put("Quality"); t.Put(kDot); t.Put(render::Name(s.tier));
    t.Put(s.mode == render::QualityMode::Auto ? " (auto)" : " (fixed)");
    if (!s.picture) { t.Line(); }
    else if (!s.timing) { t.Put(kDot); t.Put("GPU time not measured"); t.Line(); }
    else if (s.budget > 0 && s.samples) { t.Put(kDot); t.Put("GPU "); t.Ms(s.p95); t.Put(" p95 of "); t.Ms(s.budget); t.Line(); }
    else t.Line();
    if (s.changes) {
        const auto& c = s.last_change;
        t.Put("Why"); t.Put(kDot); Why(t, s, c, false); t.Put(kDot); t.Ago(s.now - c.at); t.Line();
    }
    // Picture.
    t.Put("Picture"); t.Put(kDot);
    if (!s.picture) t.Put("none (music mode)");
    else {
        t.Put(s.codec); t.Put(" "); t.Whole(s.bits); t.Put("-bit");
        if (s.transfer && *s.transfer) { t.Put(" "); t.Put(s.transfer); }
        t.Put(" "); t.Whole(s.source_width); t.Put(kTimes); t.Whole(s.source_height);
        if (s.shown_width) { t.Put(kArrow); t.Whole(s.shown_width); t.Put(kTimes); t.Whole(s.shown_height); }
        if (s.zoom > 1.0f) { t.Put(kDot); t.Put("zoom "); t.Fixed(static_cast<std::int64_t>(s.zoom * 10 + 0.5f), 1); t.Put(kTimes); }
    }
    t.Line();
    // Sound.
    t.Put("Audio"); t.Put(kDot);
    if (s.audio_unsupported) { t.Put("this track cannot play here ("); PutChannels(t, s.source_channels); t.Put(")"); }
    else if (!s.audio) t.Put("none");
    else {
        if (s.sample_rate % 1000) t.Fixed(s.sample_rate / 100, 1); else t.Whole(s.sample_rate / 1000); // 44.1, 48.
        t.Put(" kHz "); PutChannels(t, s.source_channels);
        if (s.fold_down) t.Put(s.binaural ? " \xe2\x86\x92 binaural stereo" : " \xe2\x86\x92 stereo downmix");
        else if (s.output_channels && s.output_channels != s.source_channels) { t.Put(kArrow); PutChannels(t, s.output_channels); }
        if (s.muted) { t.Put(kDot); t.Put("muted"); }
        else if (s.volume < 0.995f) { t.Put(kDot); t.Whole(static_cast<std::uint64_t>(s.volume * 100 + 0.5f)); t.Put("%"); }
        if (s.speed != 1.0f) { t.Put(kDot); t.Fixed(static_cast<std::int64_t>(s.speed * 100 + 0.5f), 2); t.Put(kTimes); }
    }
    t.Line();
    // Loudness: the programme's short-term loudness and the recent true peak heard; night mode's gain.
    if (s.loudness && (s.lufs_short > -100 || s.night)) {
        const auto tenths = [](float v) { return static_cast<std::int64_t>(v * 10 + (v < 0 ? -0.5f : 0.5f)); };
        t.Put("Loudness"); t.Put(kDot);
        if (s.lufs_short > -100) { t.Fixed(tenths(s.lufs_short), 1); t.Put(" LUFS"); } else t.Put("silence");
        if (s.true_peak > -100) { t.Put(kDot); t.Put("peak "); t.Fixed(tenths(s.true_peak), 1); t.Put(" dBTP"); }
        if (s.night) { t.Put(kDot); t.Put("night "); if (s.night_gain >= 0) t.Put("+"); t.Fixed(tenths(s.night_gain), 1); t.Put(" dB"); }
        t.Line();
    }
    // Frames.
    if (s.picture) {
        t.Put("Frames"); t.Put(kDot); t.Whole(s.presented); t.Put(" shown"); t.Put(kDot); t.Whole(s.dropped); t.Put(" dropped");
        if (s.dropped) { t.Put(" (last waited for "); t.Put(Waited(s.drop_waited)); t.Put(")"); }
        if (s.tiles_evaluated + s.tiles_reused) {
            const auto total = s.tiles_evaluated + s.tiles_reused;
            t.Put(kDot); t.Put("tile reuse "); t.Whole((s.tiles_reused * 100 + total / 2) / total); t.Put("%");
        }
        t.Line();
    }
    if (s.loop) {
        t.Put("Loop"); t.Put(kDot); t.Clock(s.loop_a); t.Put(" \xe2\x80\x93 "); t.Clock(s.loop_b);
        t.Put(kDot); t.Whole(s.loop_wraps); t.Put(s.loop_wraps == 1 ? " turn" : " turns"); t.Line();
    }
    // The newest notable event.
    for (unsigned i = s.event_count; i-- > 0;) {
        if (!s.events || !Notable(s.events[i].kind)) continue;
        char event[160];
        if (DescribeEvent(s.events[i], s.now, event, sizeof(event))) { t.Put("Last"); t.Put(kDot); t.Put(event); }
        break;
    }
    // No trailing line break.
    if (t.n && t.out[t.n - 1] == '\n') t.out[--t.n] = 0;
    return t.n;
}
unsigned ExplainQuality(const TelemetrySnapshot& s, char* out, unsigned capacity) noexcept {
    Text t(out, capacity);
    t.Put("Quality"); t.Put(kDot); t.Put(render::Name(s.tier));
    if (!s.picture) { t.Put(kDot); t.Put("no picture to convert (music mode)"); return t.n; }
    if (!s.changes) {
        t.Put(s.mode == render::QualityMode::Auto ? " has not changed" : " (fixed by hand) has not changed");
        if (s.timing && s.budget > 0 && s.samples) { t.Line(); t.Put("GPU "); t.Ms(s.p95); t.Put(" per picture (p95) of a "); t.Ms(s.budget); t.Put(" frame"); }
        return t.n;
    }
    const auto& c = s.last_change;
    t.Put(" since "); t.Ago(s.now - c.at); t.Line();
    Why(t, s, c, true);
    if (s.mode == render::QualityMode::Auto && s.timing && s.budget > 0 && s.samples && c.reason != render::QualityGovernor::Reason::Manual) {
        t.Line(); t.Put("Now "); t.Ms(s.p95); t.Put(" per picture (p95)");
    }
    return t.n;
}
}
