#pragma once
#include "media/video/AreaSample.hpp"
#include <cstdint>

namespace media::video {
// What remote visual vibrometry measures now (Vibrometry::Result).
struct VibrometryResult {
    enum Channel : std::uint32_t { kLuma = 0, kCb = 1, kCr = 2, kShiftX = 3, kShiftY = 4, kChannels = 5 };
    bool known = false;              // The window spans kMinSeconds (the rest is meaningful).
    float vibration_hz = -1;         // The strongest periodic component, 0.7 Hz .. min(15 Hz, 0.45 x the picture rate); -1: none.
    float confidence = 0;            // 0..1: how far it stands above the spectrum's noise floor (0: noise, a still scene).
    float amplitude = 0;             // Its amplitude: codes (kLuma, kCb, kCr) or samples of displacement (kShiftX, kShiftY).
    std::uint32_t channel = kLuma;   // Where it was found.
    float vital_bpm = -1;            // The strongest component in 0.75 .. 3.5 Hz (a pulse: 45 .. 210 BPM), x 60; -1: none.
    float vital_confidence = 0;      // 0..1, as `confidence`.
    std::uint32_t vital_channel = kLuma;
    float seconds = 0;               // The window's span.
    float rate = 0;                  // Pictures per second in it.
    std::uint32_t samples = 0;       // Pictures in it.
    float snr = 0, vital_snr = 0;    // Peak power over the band's median power (diagnostics; the confidences come from them).
};

// Remote visual vibrometry and vitals (docs/picture.md, "Remote vibrometry", 2026-11-02), portable, no allocation (its
// storage is the object: ~23 KB). A region of the picture, held still in the scene (the caller places it: the target's, or
// the view's fovea moved by the camera's shake), is sampled every picture:
// - five signals: its Y', Cb and Cr means (AreaMean: fractional edges, so a region moved by a fraction of a sample does not
//   step), and its content's own displacement x / y (Lucas-Kanade on a kCells x kCells grid of the region's cell means
//   against a reference grid, whose gradients are each cell's mean moved half a sample either way on the reference picture:
//   exact for a box, where differences of neighbouring cells alias fine texture; re-referenced, continuing the series,
//   after half a cell), kept for the last kWindow pictures with their times;
// - every kEvery pictures, each signal detrended (least-squares line), weighted by a Hann window over the window's span and
//   transformed at the pictures' own times (a DFT of uneven samples: a late picture is not a wrong one), on a grid of
//   1/(2 x span) from 0.7 Hz to min(15 Hz, 0.45 x the picture rate); its strongest bin is refined by golden-section search on
//   the same transform (to a thousandth of a hertz);
// - a peak counts by its power over the band's median power (white noise's strongest bin stands ~8x above its median, a tone
//   thousands): confidence = (snr - kSnrFloor) / kSnrSpan, clamped; a signal flatter than its floor (a still scene) has none.
// The vibration is the channel whose peak stands highest; the vital rate the same within 0.75 .. 3.5 Hz (remote
// photoplethysmography: the skin's colour with the pulse; ballistocardiography: the head's motion with it), unless it lies
// where a harmonic (2nd to 6th) of a stronger component folds to at the picture rate (an 8 Hz flicker's 4th, at 30/s, is
// at 2 Hz: not a pulse of 120).
// The samples may come measured (2026-11-03; docs/picture.md, "Vibrometry in the window"): the windowed engine's GPU pass
// (HdrToneMap.hlsl Vibro) reads the region of the decoded picture at full resolution, Y', Cb and Cr, and hands the same
// numbers this class takes from planes (VibrometrySample).
inline constexpr unsigned kVibrometryCells = 24;
// One picture's region as measured (8-bit codes): its means, its kVibrometryCells^2 cells' luma means (row-major), and
// each cell's mean with the cell moved half a sample right, left, down and up (the displacement's gradients).
struct VibrometrySample {
    float luma = 0, cb = 0, cr = 0;
    float cells[kVibrometryCells * kVibrometryCells]{};
    float right[kVibrometryCells * kVibrometryCells]{}, left[kVibrometryCells * kVibrometryCells]{};
    float below[kVibrometryCells * kVibrometryCells]{}, above[kVibrometryCells * kVibrometryCells]{};
};
class Vibrometry final {
public:
    static constexpr unsigned kWindow = 256;     // Pictures (8.5 s at 30 per second).
    static constexpr unsigned kCells = kVibrometryCells; // The displacement's grid across the region.
    static constexpr unsigned kEvery = 4;        // The spectrum every this many pictures.
    static constexpr unsigned kMaxBins = 320;
    static constexpr float kMinSeconds = 2.5f;   // Before that nothing is reported.
    static constexpr float kLow = 0.7f, kHigh = 15.0f, kVitalLow = 0.75f, kVitalHigh = 3.5f;
    static constexpr float kSnrFloor = 20.0f, kSnrSpan = 40.0f;
    static constexpr float kTrendSeconds = 1.0f / kLow; // Follow: the moving mean taken out (a box this long, centred).
    void Reset() noexcept;
    // One picture: its planes (null chroma: luma alone, a grid's), the region (luma samples, fractional), its time. A region moved by more than a quarter of its
    // size, or resized by more than a tenth, starts the window over (another place: another signal).
    // `follow` (2026-11-03; docs/picture.md, "Moving targets"): the region moves with a tracked target (its path, TargetPath),
    // so its moving is not another place: the window goes on (a jump of half its size in one picture, a resize by a tenth,
    // or a change between following and not still start it over; the caller resets on a cut or a lost track). The cells'
    // reference moves with the region (the displacement is the target's own about its path), and each signal is taken out
    // of its own moving mean over kTrendSeconds instead of a straight line: a target crossing the scene's lighting changes
    // its brightness by more than its pulse, along no straight line. The luma's residual is also scaled by the window's mean
    // over that moving mean (illumination normalised: the pulse in its share of the light falling on the target, in codes
    // at the window's mean).
    void Push(const Planes420& picture, AreaRect region, std::int64_t pts_100ns, bool follow = false) noexcept;
    // The same from a measured sample of `region`.
    void Push(const VibrometrySample& sample, AreaRect region, std::int64_t pts_100ns, bool follow = false) noexcept;
    [[nodiscard]] bool Following() const noexcept { return follow_; }
    [[nodiscard]] const VibrometryResult& Result() const noexcept { return result_; }
    [[nodiscard]] AreaRect Region() const noexcept { return region_; }
    [[nodiscard]] std::uint32_t Restarts() const noexcept { return restarts_; } // Windows started over since Reset.
private:
    void Analyse() noexcept;
    bool Begin(AreaRect region, std::int64_t pts, bool follow) noexcept; // The window's restarts; false: not a region.
    // The cells (cells_) against the reference: the displacement, re-referencing (`reference`) after half a cell; then the
    // picture's five signals recorded.
    template <class Refer> void Record(float luma, float cb, float cr, AreaRect region, std::int64_t pts, const Refer& reference) noexcept;
    struct Peak { double hz = -1, power = 0, snr = 0; };
    // The weighted transform's power at `hz` (the detrended, windowed signal of `channel`).
    [[nodiscard]] double Power(unsigned channel, double hz) const noexcept;
    Peak Refine(unsigned channel, double hz, double step) const noexcept;
    float values_[VibrometryResult::kChannels][kWindow]{};
    std::int64_t times_[kWindow]{};
    unsigned head_ = 0, count_ = 0, since_ = 0;
    AreaRect region_{};
    float reference_[kCells * kCells]{}, cells_[kCells * kCells]{};
    float gx_[kCells * kCells]{}, gy_[kCells * kCells]{}; // The reference cells' means' change per sample of shift (from the picture).
    bool referenced_ = false;
    void Reference(const Planes420& picture, AreaRect region) noexcept;
    double shift_x_ = 0, shift_y_ = 0;           // The displacement series' offset (re-references keep it continuous).
    // Analyse's working set: the window in order, detrended and weighted.
    double t_[kWindow]{}, weighted_[VibrometryResult::kChannels][kWindow]{};
    double weight_sum_ = 0;
    float spectrum_[VibrometryResult::kChannels][kMaxBins]{};
    std::uint32_t restarts_ = 0;
    bool follow_ = false;
    double trend_[kWindow]{}; // Analyse: a signal's moving mean (follow).
    VibrometryResult result_{};
};

// A tracked target's path for vibrometry's region (2026-11-03; docs/picture.md, "Moving targets"), portable, no allocation:
// the least-squares line through its positions of the last kSeconds, at the time asked. A target moving steadily is
// followed without lag (a low-pass would trail it), the tracker's jitter and the target's own vibration (above ~1 Hz) are
// averaged out of the region's place: vibrometry measures them, the region must not follow them.
class TargetPath final {
public:
    static constexpr unsigned kPoints = 128;
    static constexpr double kSeconds = 1.0;
    void Reset() noexcept { count_ = head_ = 0; }
    void Push(std::int64_t pts_100ns, double x, double y) noexcept;
    // Where the line puts it at `pts` (the last position while there is one point or no time between them); false: none.
    bool At(std::int64_t pts_100ns, double& x, double& y) const noexcept;
    [[nodiscard]] unsigned Count() const noexcept { return count_; }
private:
    std::int64_t t_[kPoints]{};
    double x_[kPoints]{}, y_[kPoints]{};
    unsigned count_ = 0, head_ = 0;
};
}
