#pragma once
#include <cstdint>

namespace media::video {
// Auto-leveling the horizon (docs/picture.md, "Auto-leveling the horizon", 2026-10-29), portable, no allocation. The pictures
// never leave the GPU: the statistics pass (HdrToneMap.hlsl Orientation, filter flag kHorizon) finds the dominant edge of each
// block (about 80 across the picture, 8 to 48 texels a side; Scharr gradients over its centre's 8 x 8), and where it is strong
// and straight adds its tilt modulo 90 degrees to one of two histograms (lines near horizontal, lines near vertical), kBins
// bins of kBinDegrees over [kLowest, -kLowest), each bin two words: the blocks' weight and their
// weight times where in the bin they fell (256ths). The renderer reads those 1024 bytes with the statistics once the picture's
// fence has passed and gives them here.
// - Measure (one picture): both histograms added (the vertical lines at kVerticalWeight: perspective tilts verticals that are
//   off the picture's centre, while a horizon is level everywhere), the peak's five bins against the histogram's median bin
//   (texture's orientations spread evenly over all of them): a picture has a tilt when the peak holds at least kMinMass over
//   that background and twice its own share of it; the tilt is the mean shift of the weight above the background, within a
//   degree, from the peak's bin until it settles, each bin at its blocks' mean place in it (sub-bin: the bins are half a
//   degree).
// - Push (each picture's measurement, in decode order or not): the last kWindow tilts found; their median is the target
//   (moving objects' lines, a picture or two, do not tilt the screen), locked once kLock of them agree within a degree; the
//   level shown moves to it with time constant kGlide seconds of presentation time (smoothly, not in jumps). A picture
//   without lines keeps the last tilt.
// - Black bars and perspective (docs/picture.md, "Black bars", 2026-10-30): a block whose window has a whole end row (a
//   near-horizontal line) or end column (near-vertical) at most kLift over black, and whose line is within kBarAxis of the
//   picture's axis, is the edge of a bar encoded in the picture (video::Matte), not of the scene: it is not counted. And a
//   picture whose two classes each have a peak (at least kMinMass of their own) more than kDisagree apart is lines in
//   perspective (a camera's roll turns horizontal and vertical lines alike): it has no tilt (`perspective`).
// Tilts are degrees clockwise on the stored picture (x right, y down): a horizon whose right end is lower has a positive tilt,
// and a leveling turn of minus that makes it level.
class HorizonLevel final {
public:
    static constexpr unsigned kBins = 64;
    static constexpr float kBinDegrees = 0.5f, kLowest = -16.0f;
    static constexpr unsigned kWords = 2 * kBins * 2; // ColorPipeline::kHorizonBytes / 4: [lines][bin]{weight, weight x fraction}.
    static constexpr float kMaxTilt = 15.0f;          // The leveling turn's range (D3D12Renderer::kMaxLevel).
    static constexpr float kVerticalWeight = 0.5f;
    static constexpr float kMinMass = 96.0f;          // About six to sixteen straight edge blocks over the background.
    static constexpr unsigned kWindow = 15, kLock = 5;
    static constexpr float kGlide = 0.25f;
    static constexpr float kBarAxis = 1.0f;           // Degrees: a bar's edge is the picture's axis.
    static constexpr float kLift = 0.03f;             // video::Matte::kLift.
    static constexpr float kDisagree = 3.0f;          // Degrees between the classes' peaks: perspective.
    struct Measurement {
        bool found = false; float tilt = 0, mass = 0;
        bool perspective = false;                     // Both classes peaked, more than kDisagree apart: no tilt.
        unsigned classes = 0;                         // 1: the near-horizontal lines peaked, 2: the near-vertical ones.
        float lines[2]{};                             // Their peaks' tilts.
    };
    [[nodiscard]] static Measurement Measure(const std::uint32_t* words) noexcept;
    // The GPU's histograms computed on the CPU (tests, and hosts with CPU pictures): `luma` of width x height 8-bit samples,
    // `step` bytes apart in a row (4: one channel of RGBA), as the shader reads them (code / 255), added into `words` (kWords;
    // not cleared); `black` the code of black (16: limited range, 0: full). The block's structure tensor in integers (the codes'
    // own Scharr gradients: exact), its line from them in double (2026-10-31; float before, as the shader): a block on a bin's
    // boundary may land in the GPU's neighbour.
    // The CPU survey (2026-10-31, docs/picture.md "The CPU survey"): the tensors with SSE2 (a block's 8 centre columns a
    // register) and AVX2 (two blocks a register) where available, unless built with MEDIA_SURVEY_SCALAR; every level gives the
    // same words, bit for bit (ForceLevel: 0 scalar, 1 SSE2, 2 AVX2, clamped to what the processor has; process-wide, set
    // before use; `step` other than 1 is scalar).
    static void Accumulate(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t* words, unsigned step = 1,
                           unsigned black = 16) noexcept;
    static unsigned ForceLevel(unsigned level) noexcept;
    [[nodiscard]] static unsigned Level() noexcept;
    void Reset() noexcept;
    void Push(std::int64_t pts_100ns, const Measurement& m) noexcept;
    [[nodiscard]] bool Locked() const noexcept { return locked_; }
    [[nodiscard]] float Tilt() const noexcept { return tilt_; }     // Smoothed; 0 until locked.
    [[nodiscard]] float Target() const noexcept { return target_; } // The recent tilts' median.
    [[nodiscard]] unsigned Measured() const noexcept { return measured_; } // Pictures pushed with a tilt.
    [[nodiscard]] unsigned Pushed() const noexcept { return pushed_; }
private:
    float tilts_[kWindow]{};
    unsigned count_ = 0, next_ = 0, measured_ = 0, pushed_ = 0;
    std::int64_t last_pts_ = INT64_MIN;
    bool locked_ = false;
    float tilt_ = 0, target_ = 0;
};
}
