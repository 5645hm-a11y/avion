#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line isolation pads the cursors on purpose.
#endif
namespace media::video {
// One event of the emulated event camera: the C SDK's media_event_spike_t, 16 bytes.
struct EventSpike {
    std::uint16_t x, y;        // Where, in the event sensor's pixels (EventCamera::Width() x Height()).
    std::int8_t polarity;      // +1: brighter (ON), -1: darker (OFF).
    std::uint8_t contrast;     // The change that fired it: |log intensity change| in 1/64 units (255: 4 or more).
    std::uint16_t reserved;
    std::int64_t timestamp_us; // The sensor's time (the picture's clock, microseconds), between the previous picture and this one.
};
static_assert(sizeof(EventSpike) == 16);

struct EventCameraOptions {
    float contrast_threshold = 0.15f; // C: a pixel fires when its log intensity moved by C since it last fired (natural log).
    unsigned max_per_pixel = 4;       // Events one pixel may fire for one picture (crossings beyond: its reference jumps).
    bool stabilized = true;           // Take the camera's own motion (the IMU's) out before comparing.
};

// A neuromorphic event camera emulated from ordinary pictures (docs/picture.md, "Event camera", 2026-11-02), portable, no
// allocation after Configure. Like a dynamic vision sensor, each pixel keeps the log intensity at which it last fired and
// fires an ON or OFF event each time its log intensity moves by the contrast threshold C from it; between two pictures the
// crossings are spread over the interval (the k-th of a change d at k C / |d| of it, in kSlices slices: the stream is in
// time order, the timestamps those of the slices' ends).
// - The sensor: the luma, box-averaged by an integer `Scale()` to at most kMaxWidth x kMaxHeight (VGA-class: 4K is 6x6
//   samples a pixel, 1080p 3x3), kept in sixteenths of a code; its log intensity through a table of
//   log((Y' - 16) / 219 linearised (gamma 2.2) + 1/64) per sixteenth (darks are damped as a sensor's are).
// - Stabilised: the references live in a frame fixed to the scene. Each picture's camera motion (the IMU's: a shift at the
//   centre and a roll, Push) moves the scene's frame in the camera's; each reference pixel is compared with the picture
//   sampled where it now lies (bicubic on the codes, then the log: interpolating logs across an edge would make its
//   geometric mean, ~30 codes too dark between 40 and 200, and fire), so the camera shaking or turning fires nothing, and only what moves in the scene
//   (or changes its brightness) does. Each change
//   is first reduced by what a misplacement of kRegistration pixels makes of its reference's local slope (a guard: a sharp
//   edge of 160 codes is ~1.4 log units a pixel, and a tenth of a pixel of error would fire it; a moving edge's change is
//   many times that). A pixel whose place left the picture neither fires nor changes. When the scene's frame
//   has moved by kRebase pixels it is moved toward the camera's by whole pixels (the references copied, not resampled: a
//   blurred reference fires at its sharp edges; pixels come into view take the picture's value); turned by kRebaseRoll
//   radians, the references start over from the picture (which fires nothing).
// - The events go into a single-producer single-consumer ring (Poll from one other thread): a picture's events are written
//   in time order and published at once; what the ring has no room for is counted (Dropped) from the latest slices back.
// Memory (2026-11-04; docs/picture.md, "The telemetry module's memory"): the references and one scratch plane (Rebase's target
// and each picture's changes, never needed at once) in 16 bits a pixel; the decimated picture only when Scale() is above 1 (at
// 1 the picture passed to Push is read in place); the ring in 8 bytes an event (x, y, polarity and contrast in 28 bits, the
// timestamp's low 36 bits: Poll restores the rest from the newest time, exact while the ring spans under 9.5 hours of it).
class EventCamera final {
public:
    static constexpr unsigned kMaxWidth = 640, kMaxHeight = 480;
    static constexpr unsigned kRing = 1u << 16;    // Events (512 KiB), a power of two; Configure may take another.
    static constexpr unsigned kSlices = 16;        // Of the interval between two pictures.
    static constexpr float kRebase = 6.0f, kRebaseRoll = 0.03f;
    static constexpr double kRegistration = 0.1; // Pixels: how far the IMU's compensation may misplace a scene point (its guard).
    // The arena bytes Configure takes for a camera of this size and ring.
    [[nodiscard]] static std::uint64_t Bytes(unsigned width, unsigned height, unsigned ring = kRing) noexcept; // At most ~1.9 MiB.
    // `ring`: events, a power of two, at least 256 (the windowed engine's is smaller: Poll empties it every picture).
    [[nodiscard]] bool Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, unsigned ring = kRing) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return ref_ != nullptr; }
    void Release() noexcept { ref_ = scratch_ = nullptr; now_ = nullptr; ring_ = nullptr; armed_ = false; } // The arena is going away.
    void SetOptions(const EventCameraOptions& options) noexcept;
    [[nodiscard]] const EventCameraOptions& Options() const noexcept { return options_; }
    // Forget the references (the next picture only arms them). The ring is the consumer's: not touched.
    void Reset() noexcept { armed_ = false; }
    // One picture's luma (the camera's `width` x `height`), its time, and the camera's motion since the previous picture: the
    // scene moved by (dx, dy) pixels at the centre and turned by `roll` radians about it (picture coordinates: +x right, +y
    // down, + clockwise). Returns the events written.
    std::uint32_t Push(const std::uint8_t* luma, unsigned stride, std::int64_t pts_100ns, float dx, float dy, float roll) noexcept;
    // The consumer: up to `capacity` of the oldest events; returns how many.
    std::uint32_t Poll(EventSpike* out, std::uint32_t capacity) noexcept;
    [[nodiscard]] unsigned Width() const noexcept { return sw_; }
    [[nodiscard]] unsigned Height() const noexcept { return sh_; }
    [[nodiscard]] unsigned Scale() const noexcept { return scale_; }
    [[nodiscard]] std::uint64_t Emitted() const noexcept { return emitted_; }   // Written since Configure.
    [[nodiscard]] std::uint64_t Dropped() const noexcept { return dropped_; }   // No room in the ring.
    [[nodiscard]] std::uint32_t Rebases() const noexcept { return rebases_; }
    [[nodiscard]] std::uint32_t Pictures() const noexcept { return pictures_; }
    [[nodiscard]] std::uint32_t LastCount() const noexcept { return last_; }    // The last picture's events.
private:
    void Decimate(const std::uint8_t* luma, unsigned stride) noexcept;          // The picture into now_ (codes, Q4).
    void Rebase() noexcept;
    EventCameraOptions options_{};
    unsigned width_ = 0, height_ = 0, scale_ = 1, sw_ = 0, sh_ = 0;
    [[nodiscard]] unsigned Code(unsigned x, unsigned y) const noexcept { return now_ ? now_[static_cast<std::size_t>(y) * sw_ + x] : 16u * luma_[static_cast<std::size_t>(y) * luma_stride_ + x]; }
    [[nodiscard]] std::int32_t CodeAt(double x, double y) const noexcept; // Between pixels (bicubic).
    std::int16_t log_[4096]{};                      // log intensity of a code in sixteenths, Q10 (1024 = 1 natural log unit).
    std::int16_t* ref_ = nullptr;                   // The references, in the scene's frame (Q10).
    std::int16_t* scratch_ = nullptr;               // Rebase's target, then each reference pixel's change this picture (Q10; 0 off the picture).
    std::uint16_t* now_ = nullptr;                  // The picture, decimated (codes, Q4); none at Scale() 1: luma_ below.
    const std::uint8_t* luma_ = nullptr;            // Scale() 1: the picture Push was given.
    std::size_t luma_stride_ = 0;
    std::uint64_t* ring_ = nullptr;                 // Events, packed (the class's comment).
    unsigned ring_size_ = kRing;
    bool armed_ = false;
    double tx_ = 0, ty_ = 0, theta_ = 0;            // The scene's frame in the camera's: p = R(theta)(q - c) + c + t.
    std::int64_t last_pts_ = 0;
    std::uint64_t emitted_ = 0, dropped_ = 0;
    std::uint32_t rebases_ = 0, pictures_ = 0, last_ = 0;
    alignas(core::kCacheLineBytes) std::atomic<std::uint64_t> write_{0}; // Published by the producer.
    std::atomic<std::int64_t> newest_us_{0};                             // The newest timestamp (Poll's reference), published with it.
    alignas(core::kCacheLineBytes) std::atomic<std::uint64_t> read_{0};  // Published by the consumer.
    alignas(core::kCacheLineBytes) std::uint64_t produced_ = 0;          // The producer's own count (write_'s next value).
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
