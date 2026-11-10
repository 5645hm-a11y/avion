#include "media/audio/MasteringEq.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kAntiDenormal = 1e-20f;
constexpr float kClipThreshold = 0.8912509f; // -1 dBFS, as DspProcessor's soft clipper.
float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float Princarg(float phase) { // Into (-pi, pi].
    const float two_pi = static_cast<float>(2 * kPi);
    phase -= two_pi * std::floor(phase / two_pi);
    return phase > static_cast<float>(kPi) ? phase - two_pi : phase;
}
// RBJ cookbook biquads, normalised by a0, computed in double.
struct Coefficients { float b0, b1, b2, a1, a2; };
Coefficients Peaking(double hz, double db, double q, double rate) {
    const double a = std::pow(10.0, db / 40), w = 2 * kPi * hz / rate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha / a;
    return {static_cast<float>((1 + alpha * a) / a0), static_cast<float>(-2 * c / a0), static_cast<float>((1 - alpha * a) / a0),
            static_cast<float>(-2 * c / a0), static_cast<float>((1 - alpha / a) / a0)};
}
Coefficients NotchAt(double hz, double q, double rate) {
    const double w = 2 * kPi * hz / rate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
    return {static_cast<float>(1 / a0), static_cast<float>(-2 * c / a0), static_cast<float>(1 / a0), static_cast<float>(-2 * c / a0),
            static_cast<float>((1 - alpha) / a0)};
}
Coefficients BandPassAt(double hz, double q, double rate) { // Constant 0 dB peak.
    const double w = 2 * kPi * hz / rate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    return {static_cast<float>(alpha / a0), 0.0f, static_cast<float>(-alpha / a0), static_cast<float>(-2 * std::cos(w) / a0),
            static_cast<float>((1 - alpha) / a0)};
}
// Transposed direct form II, one sample.
inline float Run(float b0, float b1, float b2, float a1, float a2, float* z, float x) {
    const float y = b0 * x + z[0];
    z[0] = b1 * x - a1 * y + z[1];
    z[1] = b2 * x - a2 * y;
    return y;
}
// ISO 226:2003 Table 1: frequency, exponent af, magnitude Lu (dB) and hearing threshold Tf (dB).
constexpr double kIsoHz[29]{20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500,
                            3150, 4000, 5000, 6300, 8000, 10000, 12500};
constexpr double kIsoAf[29]{0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330, 0.315, 0.301, 0.288, 0.276, 0.267,
                            0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242, 0.242, 0.245, 0.254, 0.271, 0.301};
constexpr double kIsoLu[29]{-31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5, -3.1, -2.0, -1.1, -0.4, 0.0, 0.3, 0.5, 0.0,
                            -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1};
constexpr double kIsoTf[29]{78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
                            -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3};
}

// ---- SpectralTracker ----
bool SpectralTracker::Configure(unsigned rate) noexcept {
    if (rate < 8000 || rate > 192000) return false;
    rate_ = rate;
    for (unsigned i = 0; i < kSize; ++i) window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / kSize));
    for (unsigned i = 0; i < kSize / 2; ++i) { cosine_[i] = static_cast<float>(std::cos(2 * kPi * i / kSize)); sine_[i] = static_cast<float>(std::sin(2 * kPi * i / kSize)); }
    unsigned bits = 0; while ((1u << bits) < kSize) ++bits;
    for (unsigned i = 0; i < kSize; ++i) {
        unsigned r = 0; for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
        reversed_[i] = static_cast<std::uint16_t>(r);
    }
    const double hop = static_cast<double>(kHop) / rate;
    stationary_hops_ = static_cast<unsigned>(std::ceil(2.0 / hop));
    drop_hops_ = static_cast<unsigned>(std::ceil(1.0 / hop));
    speech_rate_ = static_cast<float>(1 - std::exp(-hop / 0.15));
    deviation_rate_ = static_cast<float>(1 - std::exp(-hop / 0.5));
    Reset();
    return true;
}
void SpectralTracker::Reset() noexcept {
    std::memset(history_, 0, sizeof(history_)); std::memset(previous_phase_, 0, sizeof(previous_phase_));
    filled_ = write_ = level_count_ = 0; speech_ = 0; dominant_ = kBands; analyses_ = 0; since_reset_ = 0;
    for (auto& b : band_db_) b = -144.0f;
    for (auto& l : speech_levels_) l = -90.0f;
    for (auto& t : tracks_) t = {};
}
unsigned SpectralTracker::Feed(const float* x, unsigned frames, unsigned channels, bool& hop) noexcept {
    hop = false;
    if (!rate_ || !x || !channels) return frames;
    const unsigned n = (std::min)(frames, kHop - filled_);
    const float scale = 1.0f / static_cast<float>(channels);
    for (unsigned f = 0; f < n; ++f, x += channels) {
        float sum = 0;
        for (unsigned c = 0; c < channels; ++c) sum += std::isfinite(x[c]) ? x[c] : 0.0f;
        history_[write_] = sum * scale; write_ = (write_ + 1) & (kSize - 1);
    }
    filled_ += n;
    if (filled_ == kHop) { filled_ = 0; hop = true; }
    return n;
}
void SpectralTracker::Push(const float* x, unsigned frames, unsigned channels) noexcept {
    while (frames && x) {
        bool hop = false;
        const unsigned n = Feed(x, frames, channels, hop);
        if (hop) Analyse();
        x += static_cast<std::size_t>(n) * channels; frames -= n;
    }
}
void SpectralTracker::Analyse() noexcept {
    ++analyses_; ++since_reset_;
    // Oldest sample first (history_[write_]), windowed, in bit-reversed order; then an iterative radix-2 FFT.
    for (unsigned i = 0; i < kSize; ++i) { const unsigned r = reversed_[i]; real_[r] = history_[(write_ + i) & (kSize - 1)] * window_[i]; imaginary_[r] = 0; }
    for (unsigned size = 2; size <= kSize; size *= 2) {
        const unsigned half = size / 2, step = kSize / size;
        for (unsigned start = 0; start < kSize; start += size)
            for (unsigned k = 0; k < half; ++k) {
                const float c = cosine_[k * step], s = sine_[k * step];
                const unsigned i = start + k, j = i + half;
                const float tr = real_[j] * c + imaginary_[j] * s, ti = imaginary_[j] * c - real_[j] * s;
                real_[j] = real_[i] - tr; imaginary_[j] = imaginary_[i] - ti; real_[i] += tr; imaginary_[i] += ti;
            }
    }
    for (unsigned k = 0; k < kBins; ++k) { power_[k] = real_[k] * real_[k] + imaginary_[k] * imaginary_[k]; phase_[k] = std::atan2(imaginary_[k], real_[k]); }
    const double reference = 1.5 * (kSize / 4.0) * (kSize / 4.0); // A full-scale sine's three main-lobe bins.
    const double bin_hz = static_cast<double>(rate_) / kSize, nyquist = 0.5 * rate_;
    const auto bin = [&](double hz) { return static_cast<unsigned>((std::min)(std::max(hz / bin_hz, 0.0), static_cast<double>(kBins - 1))); };
    const auto sum = [&](double lo, double hi) { double s = 0; for (unsigned k = bin(lo) + (bin(lo) * bin_hz < lo ? 1u : 0u); k < kBins && k * bin_hz < hi; ++k) s += power_[k]; return s; };
    // Band levels and the dominant band.
    float loudest = -90.0f; dominant_ = kBands;
    for (unsigned b = 0; b < kBands; ++b) {
        if (!BandEnabled(b)) { band_db_[b] = -144.0f; continue; }
        const double centre = kBandCentres[b];
        band_db_[b] = static_cast<float>(10 * std::log10(sum(centre / 1.41421356, centre * 1.41421356) / reference + 1e-20));
        if (band_db_[b] > loudest) { loudest = band_db_[b]; dominant_ = b; }
    }
    // Speech: the speech band's share above 300 Hz, its syllabic modulation and its level.
    const double speech = sum(300, 3400), upper = sum(300, (std::min)(8000.0, 0.45 * rate_));
    const float speech_db = static_cast<float>((std::max)(10 * std::log10(speech / reference + 1e-20), -90.0));
    speech_levels_[level_count_++ % 24] = speech_db;
    const unsigned held = (std::min)(level_count_, 24u);
    float mean = 0, spread = 0;
    for (unsigned i = 0; i < held; ++i) mean += speech_levels_[i];
    mean /= static_cast<float>(held);
    for (unsigned i = 0; i < held; ++i) spread += (speech_levels_[i] - mean) * (speech_levels_[i] - mean);
    spread = std::sqrt(spread / static_cast<float>(held));
    const float share = upper > 0 ? static_cast<float>(speech / upper) : 0.0f;
    const float wanted = Smoothstep(0.5f, 0.8f, share) * Smoothstep(2.0f, 5.0f, spread) * Smoothstep(-65.0f, -55.0f, mean);
    speech_ += (wanted - speech_) * speech_rate_;
    // Stationary tones. Not on the first hop after a reset: the phase advance needs a previous hop.
    if (since_reset_ >= 2) {
        struct Candidate { float hz, level, prominence; };
        Candidate found[kCandidates]{}; unsigned count = 0;
        const unsigned first = (std::max)(bin(30.0), 3u) + 1, last = (std::min)(bin((std::min)(16000.0, 0.45 * rate_)), kBins - 4);
        const auto db = [&](unsigned k) { return static_cast<float>(10 * std::log10(power_[k] / reference + 1e-20)); };
        for (unsigned k = first; k < last; ++k) {
            if (!(power_[k] > power_[k - 1] && power_[k] >= power_[k + 1])) continue;
            const float level = static_cast<float>(10 * std::log10((power_[k - 1] + power_[k] + power_[k + 1]) / reference + 1e-20));
            if (level < -70.0f) continue;
            float around[48]; unsigned n = 0;
            for (unsigned j = k > 24 ? k - 24 : 0; j <= k + 24 && j < kBins; ++j) if (j + 3 < k || j > k + 3) around[n++] = db(j);
            if (n < 8) continue;
            std::nth_element(around, around + n / 2, around + n);
            const float prominence = db(k) - around[n / 2];
            if (prominence < 15.0f) continue;
            // Phase vocoder: the advance over one hop beyond the bin centre's gives the frequency within the main lobe.
            const float expected = static_cast<float>(2 * kPi * static_cast<double>(k) * kHop / kSize);
            const float delta = Princarg(phase_[k] - previous_phase_[k] - expected);
            const float hz = static_cast<float>((static_cast<double>(k) + delta * kSize / (2 * kPi * kHop)) * bin_hz);
            if (!(hz > 20.0f) || hz >= nyquist) continue;
            // Keep the most prominent kCandidates, in order.
            unsigned at = count < kCandidates ? count++ : kCandidates;
            while (at > 0 && found[at - 1].prominence < prominence) { if (at < kCandidates) found[at] = found[at - 1]; --at; }
            if (at < kCandidates) found[at] = {hz, level, prominence};
        }
        bool seen[kMaxTracks]{};
        for (unsigned c = 0; c < count; ++c) {
            const auto& d = found[c];
            int best = -1; float distance = 3.0f;
            for (unsigned t = 0; t < kMaxTracks; ++t)
                if (tracks_[t].live && !seen[t] && std::fabs(tracks_[t].hz - d.hz) < distance) { distance = std::fabs(tracks_[t].hz - d.hz); best = static_cast<int>(t); }
            if (best >= 0) {
                auto& t = tracks_[best]; seen[best] = true;
                t.deviation_hz += (std::fabs(d.hz - t.hz) - t.deviation_hz) * deviation_rate_;
                t.level_deviation_db += (std::fabs(d.level - t.level_db) - t.level_deviation_db) * deviation_rate_;
                t.hz += (d.hz - t.hz) * 0.25f; t.level_db += (d.level - t.level_db) * 0.25f; t.prominence_db = d.prominence;
                ++t.age; t.missed = 0;
                // Hysteresis: qualifying needs half the deviation that keeps a stationary tone so.
                t.stationary = t.age >= stationary_hops_ && t.deviation_hz < (t.stationary ? 2.0f : 1.0f) && t.level_deviation_db < (t.stationary ? 6.0f : 3.0f);
                continue;
            }
            // A new track: a free slot, else the non-stationary one missed longest (then youngest).
            int slot = -1; long worst = -1;
            for (unsigned t = 0; t < kMaxTracks && slot < 0; ++t) if (!tracks_[t].live) slot = static_cast<int>(t);
            if (slot < 0)
                for (unsigned t = 0; t < kMaxTracks; ++t) {
                    const auto& o = tracks_[t];
                    if (o.stationary || seen[t]) continue;
                    const long score = static_cast<long>(o.missed) * 100000L - static_cast<long>(o.age);
                    if (score > worst) { worst = score; slot = static_cast<int>(t); }
                }
            if (slot < 0) continue;
            auto& t = tracks_[slot]; t = {}; seen[slot] = true;
            t.hz = d.hz; t.level_db = d.level; t.prominence_db = d.prominence; t.deviation_hz = 1.0f; t.level_deviation_db = 1.0f;
            t.age = 1; t.live = true;
        }
        for (unsigned t = 0; t < kMaxTracks; ++t)
            if (tracks_[t].live && !seen[t] && ++tracks_[t].missed > drop_hops_) tracks_[t] = {};
    }
    std::memcpy(previous_phase_, phase_, sizeof(phase_));
}

// ---- MasteringEq ----
double MasteringEq::EqualLoudness(double hz, double phon) noexcept {
    hz = std::clamp(hz, kIsoHz[0], kIsoHz[28]);
    unsigned i = 0; while (i < 27 && kIsoHz[i + 1] < hz) ++i;
    const double t = (std::log(hz) - std::log(kIsoHz[i])) / (std::log(kIsoHz[i + 1]) - std::log(kIsoHz[i]));
    const double af = kIsoAf[i] + t * (kIsoAf[i + 1] - kIsoAf[i]), lu = kIsoLu[i] + t * (kIsoLu[i + 1] - kIsoLu[i]);
    const double tf = kIsoTf[i] + t * (kIsoTf[i + 1] - kIsoTf[i]);
    const double a = 4.47e-3 * (std::pow(10.0, 0.025 * phon) - 1.15) + std::pow(0.4 * std::pow(10.0, (tf + lu) / 10 - 9), af);
    return 10 / af * std::log10(a) - lu + 94;
}
float MasteringEq::IsoGain(unsigned band, float level_db) noexcept {
    if (band >= kBands || !(level_db < 0)) return 0.0f;
    const double phon = std::clamp(80.0 + static_cast<double>(level_db), 20.0, 80.0), hz = SpectralTracker::kBandCentres[band];
    const double gain = (EqualLoudness(hz, phon) - phon) - (EqualLoudness(hz, 80) - 80);
    return static_cast<float>(std::clamp(gain, 0.0, static_cast<double>(kMaxDb)));
}
double MasteringEq::PeakingResponseDb(double hz, double centre, double gain_db, double q, double rate) noexcept {
    const double a = std::pow(10.0, gain_db / 40), w0 = 2 * kPi * centre / rate, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    const double b0 = 1 + alpha * a, b1 = -2 * c, b2 = 1 - alpha * a, a0 = 1 + alpha / a, a1 = -2 * c, a2 = 1 - alpha / a;
    const double w = 2 * kPi * hz / rate, cw = std::cos(w), sw = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = b0 + b1 * cw + b2 * c2, ni = -(b1 * sw + b2 * s2), dr = a0 + a1 * cw + a2 * c2, di = -(a1 * sw + a2 * s2);
    return 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
bool MasteringEq::Configure(unsigned rate, unsigned channels) noexcept {
    if (rate < 8000 || rate > 192000 || !channels || channels > kMaxChannels) return false;
    rate_ = rate; channels_ = channels;
    period_ = static_cast<float>(kControl) / static_cast<float>(rate);
    notch_rate_in_ = period_ / 0.1f; notch_rate_out_ = period_ / 0.3f;
    tail_frames_ = static_cast<unsigned>(0.25 * rate);
    for (unsigned b = 0; b < kBands; ++b) { current_db_[b] = coefficient_db_[b] = smooth_db_[b] = duck_db_[b] = 0; bands_[b] = {}; band_live_[b] = false; }
    for (auto& n : notches_) n = {};
    Reset();
    return true;
}
void MasteringEq::ResetStates() noexcept {
    std::memset(z_, 0, sizeof(z_)); std::memset(notch_z_, 0, sizeof(notch_z_));
    for (unsigned b = 0; b < kBands; ++b) { band_live_[b] = current_db_[b] != 0; tail_[b] = tail_frames_; }
}
void MasteringEq::Reset() noexcept {
    // A seek: the ramps finish at once (no glide from the old position's ducking) and the filters start from silence.
    for (unsigned b = 0; b < kBands; ++b) duck_db_[b] = 0;
    for (auto& n : notches_) if (n.live) { n.depth = n.target; if (n.depth == 0) n = {}; }
    phase_ = 0; ResetStates();
}
void MasteringEq::SetBand(unsigned band, float db) noexcept {
    if (band < kBands) user_db_[band] = std::isfinite(db) ? std::clamp(db, -kMaxDb, kMaxDb) : 0.0f;
}
unsigned MasteringEq::Notches() const noexcept {
    unsigned n = 0; for (const auto& notch : notches_) n += notch.live && notch.depth > 0 ? 1u : 0u; return n;
}
bool MasteringEq::Wanted() const noexcept {
    if (iso_ || ducking_ || notch_killer_) return true;
    for (unsigned b = 0; b < kBands; ++b) if (user_db_[b] != 0 || current_db_[b] != 0 || band_live_[b]) return true;
    for (const auto& n : notches_) if (n.live) return true;
    return false;
}
bool MasteringEq::Active() const noexcept {
    for (unsigned b = 0; b < kBands; ++b) if (current_db_[b] != 0 || band_live_[b]) return true;
    for (const auto& n : notches_) if (n.live) return true;
    return false;
}
void MasteringEq::ControlUpdate(const SpectralTracker* analysis) noexcept {
    const float speech = ducking_ && analysis ? analysis->Speech() : 0.0f;
    double voice = 0;
    if (speech > 0) for (unsigned b = 5; b <= 7; ++b) voice += std::pow(10.0, analysis->BandDb(b) / 10.0);
    const float voice_db = static_cast<float>(10 * std::log10(voice + 1e-20));
    const float smooth = 1.0f - std::exp(-period_ / 0.05f), attack = smooth, release = 1.0f - std::exp(-period_ / 0.3f);
    // Iso-loudness: the contours only change with the volume (or the switch).
    const float level = iso_ ? iso_level_db_ : 0.0f;
    if (level != iso_cached_db_) { iso_cached_db_ = level; for (unsigned b = 0; b < kBands; ++b) iso_db_[b] = level < 0 ? IsoGain(b, level) : 0.0f; }
    for (unsigned b = 0; b < kBands; ++b) {
        if (SpectralTracker::kBandCentres[b] * 2.4f >= static_cast<float>(rate_)) { smooth_db_[b] = duck_db_[b] = 0; }
        else {
            float user = user_db_[b];
            if (speech > 0 && b >= 4 && b <= 7 && user < -3.0f) user += (-3.0f - user) * speech; // Speech protection.
            const float wanted = user + iso_db_[b];
            smooth_db_[b] += (wanted - smooth_db_[b]) * smooth;
            if (std::fabs(wanted - smooth_db_[b]) < 0.005f) smooth_db_[b] = wanted;
            float duck = 0;
            if (speech > 0 && b <= 3) duck = -(std::min)((std::max)(analysis->BandDb(b) - voice_db - 6.0f, 0.0f), 6.0f) * speech;
            duck_db_[b] += (duck - duck_db_[b]) * (duck < duck_db_[b] ? attack : release);
            if (std::fabs(duck - duck_db_[b]) < 0.005f) duck_db_[b] = duck;
        }
        current_db_[b] = std::clamp(smooth_db_[b] + duck_db_[b], -2 * kMaxDb, 2 * kMaxDb);
        if (current_db_[b] != coefficient_db_[b]) {
            const auto c = Peaking(SpectralTracker::kBandCentres[b], current_db_[b], kBandQ, rate_);
            bands_[b] = {c.b0, c.b1, c.b2, c.a1, c.a2}; coefficient_db_[b] = current_db_[b];
        }
        // Back at 0 dB the biquad is an exact identity (b = a) whose state only decays: after a 250 ms tail (seven time
        // constants of the slowest band, what is left is below -130 dBFS) it is dropped, and the band is bit-identical again.
        if (current_db_[b] != 0) { band_live_[b] = true; tail_[b] = tail_frames_; }
        else if (band_live_[b]) {
            if (tail_[b] > kControl) tail_[b] -= kControl;
            else { band_live_[b] = false; std::memset(z_[b], 0, sizeof(z_[b])); }
        }
    }
    // Notches follow the stationary tones, most prominent first.
    bool kept[kMaxNotches]{};
    if (notch_killer_ && analysis) {
        unsigned order[SpectralTracker::kMaxTracks]; unsigned count = 0;
        for (unsigned t = 0; t < SpectralTracker::kMaxTracks; ++t) {
            const auto& tone = analysis->Track(t);
            if (!tone.live || !tone.stationary || tone.hz >= 0.45f * static_cast<float>(rate_)) continue;
            unsigned at = count++;
            while (at > 0 && analysis->Track(order[at - 1]).prominence_db < tone.prominence_db) { order[at] = order[at - 1]; --at; }
            order[at] = t;
        }
        for (unsigned i = 0; i < count; ++i) {
            const auto& tone = analysis->Track(order[i]);
            int slot = -1;
            for (unsigned n = 0; n < kMaxNotches && slot < 0; ++n) if (notches_[n].live && !kept[n] && std::fabs(notches_[n].hz - tone.hz) < 3.0f) slot = static_cast<int>(n);
            for (unsigned n = 0; n < kMaxNotches && slot < 0; ++n) if (!notches_[n].live) slot = static_cast<int>(n);
            if (slot < 0) continue;
            auto& n = notches_[slot]; kept[slot] = true;
            if (!n.live || std::fabs(n.hz - tone.hz) > 0.02f) {
                const auto c = NotchAt(tone.hz, kNotchQ, rate_);
                n.c = {c.b0, c.b1, c.b2, c.a1, c.a2}; n.hz = tone.hz;
            }
            n.live = true; n.target = 1;
        }
    }
    for (unsigned n = 0; n < kMaxNotches; ++n) {
        auto& notch = notches_[n];
        if (!notch.live) continue;
        if (!kept[n]) notch.target = 0;
        notch.from = notch.depth;
        notch.depth = notch.target > notch.depth ? (std::min)(notch.depth + notch_rate_in_, 1.0f) : (std::max)(notch.depth - notch_rate_out_, 0.0f);
    }
}
void MasteringEq::Process(float* x, unsigned frames, const SpectralTracker* analysis) noexcept {
    if (!channels_ || !x) return;
    if (!Wanted()) { if (!idle_) { ResetStates(); idle_ = true; phase_ = 0; } return; } // Exact passthrough.
    idle_ = false;
    const unsigned ch = channels_;
    for (unsigned f = 0; f < frames; ++f, x += ch) {
        if (phase_ == 0) ControlUpdate(analysis);
        const float through = static_cast<float>(phase_ + 1) / static_cast<float>(kControl); // Notch depth, interpolated per frame.
        phase_ = (phase_ + 1) % kControl;
        for (unsigned c = 0; c < ch; ++c) if (!std::isfinite(x[c])) x[c] = 0;
        bool boosting = false;
        for (unsigned b = 0; b < kBands; ++b) {
            if (!band_live_[b]) continue;
            const auto& q = bands_[b];
            for (unsigned c = 0; c < ch; ++c) x[c] = Run(q.b0, q.b1, q.b2, q.a1, q.a2, z_[b][c], x[c] + kAntiDenormal) - kAntiDenormal;
            boosting = boosting || current_db_[b] > 0;
        }
        for (unsigned n = 0; n < kMaxNotches; ++n) {
            auto& notch = notches_[n];
            if (!notch.live) continue;
            const float depth = notch.from + (notch.depth - notch.from) * through;
            for (unsigned c = 0; c < ch; ++c) {
                const float y = Run(notch.c.b0, notch.c.b1, notch.c.b2, notch.c.a1, notch.c.a2, notch_z_[n][c], x[c] + kAntiDenormal) - kAntiDenormal;
                x[c] += depth * (y - x[c]);
            }
            if (notch.depth == 0 && notch.from == 0 && notch.target == 0) { notch = {}; for (unsigned c = 0; c < ch; ++c) notch_z_[n][c][0] = notch_z_[n][c][1] = 0; }
        }
        if (boosting)
            for (unsigned c = 0; c < ch; ++c) {
                const float magnitude = std::fabs(x[c]);
                if (magnitude > kClipThreshold) {
                    const float room = 1 - kClipThreshold;
                    const float bounded = kClipThreshold + room * std::tanh((std::min)(magnitude - kClipThreshold, 1e3f) / room);
                    x[c] = std::copysign((std::min)(bounded, 0.99999994f), x[c]);
                }
            }
    }
}

// ---- AudioFocus ----
bool AudioFocus::Configure(unsigned rate, unsigned channels) noexcept {
    if (rate < 8000 || rate > 192000 || !channels || channels > MasteringEq::kMaxChannels) return false;
    rate_ = rate; channels_ = channels; step_ = 1.0f / (0.05f * static_cast<float>(rate));
    const auto c = BandPassAt(2500.0, 0.7, rate);
    b0_ = c.b0; b1_ = c.b1; b2_ = c.b2; a1_ = c.a1; a2_ = c.a2;
    Reset();
    return true;
}
void AudioFocus::Reset() noexcept { z1_ = z2_ = 0; }
float AudioFocus::Target() const noexcept {
    return enabled_ && channels_ >= 2 && std::isfinite(zoom_) ? Smoothstep(kStart, kFull, zoom_) : 0.0f;
}
float AudioFocus::SourceGain(float x, float target_x, float focus) noexcept {
    const float delta = (std::clamp(x, 0.0f, 1.0f) - std::clamp(target_x, 0.0f, 1.0f)) * static_cast<float>(kPi / 2);
    const float g = std::pow(10.0f, kResidualDb * focus / 20), c = std::cos(delta), s = std::sin(delta);
    return std::sqrt(c * c + g * g * s * s);
}
void AudioFocus::Process(float* x, unsigned frames) noexcept {
    if (!x || channels_ < 2) return;
    const float target = Target();
    const float target_angle = std::clamp(std::isfinite(x_) ? x_ : 0.5f, 0.0f, 1.0f) * static_cast<float>(kPi / 2);
    if (focus_ == 0 && target == 0) { z1_ = z2_ = 0; angle_ = target_angle; return; } // Exact passthrough; a new focus starts on target.
    const unsigned ch = channels_;
    const float angle_step = step_ * static_cast<float>(kPi / 2);
    float cached_angle = -1, c = 1, s = 0, cached_focus = -1, g = 1, boost = 0;
    for (unsigned f = 0; f < frames; ++f, x += ch) {
        focus_ = target > focus_ ? (std::min)(focus_ + step_, target) : (std::max)(focus_ - step_, target);
        angle_ = target_angle > angle_ ? (std::min)(angle_ + angle_step, target_angle) : (std::max)(angle_ - angle_step, target_angle);
        if (focus_ == 0 && target == 0) { z1_ = z2_ = 0; return; } // Ramped out at a frame boundary: the rest stays untouched.
        if (angle_ != cached_angle) { cached_angle = angle_; c = std::cos(angle_); s = std::sin(angle_); }
        if (focus_ != cached_focus) { cached_focus = focus_; g = std::pow(10.0f, kResidualDb * focus_ / 20); boost = std::pow(10.0f, kBoostDb * focus_ / 20) - 1; }
        const float left = std::isfinite(x[0]) ? x[0] : 0.0f, right = std::isfinite(x[1]) ? x[1] : 0.0f;
        float beam = c * left + s * right;
        const float residual = -s * left + c * right;
        const float y = b0_ * beam + z1_;
        z1_ = b1_ * beam - a1_ * y + z2_ + kAntiDenormal; z2_ = b2_ * beam - a2_ * y;
        beam += boost * y;
        x[0] = c * beam - s * g * residual;
        x[1] = s * beam + c * g * residual;
        if (ch >= 6) {
            // Centre: its on-axis share of the angle between it (pi/4) and the target: cos(pi/4 - a) = (c + s) / sqrt2.
            const float dc = (c + s) * 0.70710678f, ds = (c - s) * 0.70710678f;
            x[2] *= std::sqrt(dc * dc + g * g * ds * ds);
            for (unsigned k = 3; k < ch; ++k) x[k] *= g;  // LFE and surrounds: ambience.
        }
    }
}
}
