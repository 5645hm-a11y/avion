#include "media/engine/HostEngine.hpp"
#include <memory>
#include "media/ai/VoiceTemplates.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace media::engine {
namespace {
using demux::Codec;
using demux::ParseStatus;
constexpr std::int64_t kSecond = 10000000;
constexpr float kSpeeds[]{0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f};
// One sample of a PCM track as a float, from its bytes.
float PcmSample(const std::byte* p, unsigned bits, bool little, bool is_float) noexcept {
    const auto b = [&](unsigned i) { return std::to_integer<std::uint32_t>(p[i]); };
    const unsigned n = bits / 8;
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(b(little ? i : n - 1 - i)) << (8 * i);
    if (is_float) {
        if (bits == 32) { std::uint32_t u = static_cast<std::uint32_t>(v); float f; std::memcpy(&f, &u, 4); return f; }
        double d; std::memcpy(&d, &v, 8); return static_cast<float>(d);
    }
    if (bits == 8) return (static_cast<float>(v) - 128.0f) / 128.0f; // 8-bit PCM is unsigned.
    const std::uint64_t sign = 1ull << (bits - 1);
    const auto value = static_cast<std::int64_t>((v ^ sign) - sign);
    return static_cast<float>(static_cast<double>(value) / static_cast<double>(sign));
}
}

HostEngine::HostEngine() noexcept = default;

void HostEngine::Status(const char* utf8, std::int64_t seconds) noexcept {
    std::size_t n = 0; while (utf8[n] && n + 1 < sizeof(status_)) { status_[n] = utf8[n]; ++n; }
    status_[n] = 0; status_until_ = MediaNow(last_now_) + seconds;
}
bool HostEngine::PrepareModules(const HostOptions& o) noexcept {
    // Each enabled module's region, in a fixed order (docs/sdk.md, "Modules"); HUD and neural upscale take none here.
    core::LinearMemoryArena* views[kModuleCount]{&voice_arena_, nullptr, &subtitle_arena_, nullptr, &chapter_arena_, &telemetry_arena_};
    module_bytes_ = 0;
    for (unsigned i = 0; i < kModuleCount; ++i) {
        if (!views[i] || !(o.modules & (1u << i))) continue;
        const auto bytes = kModuleBudgets[i].arena_bytes;
        void* region = arena_.Allocate(bytes, 65536);
        if (!region || !views[i]->InitializeView(region, bytes)) return false;
        module_bytes_ += bytes;
    }
    return true;
}
bool HostEngine::Open(const char* path, const HostOptions& options) noexcept {
    Close();
    options_ = options; error_ = 0; phase_ = Phase::Preparing;
    const bool sound = options.audio_channels != 0;
    if (sound && (options.audio_channels > 2 || options.audio_rate < 8000 || options.audio_rate > 192000)) return Fail(1);
    if (!arena_.Initialize(ArenaBudget(options.modules, options.gpu != nullptr))) return Fail(2);
    if (!PrepareModules(options)) return Fail(2);
    if (!source_.Open(arena_, path, options.gpu ? platform::FileBlockSource::kLargeBlockBytes : platform::FileBlockSource::kBlockBytes)) { Close(); return Fail(3); }
    packets_ = arena_.Create<core::SpscRingBuffer<core::MediaPacket, 64>>();
    demux::ParserLimits limits; limits.tracks = 8; limits.index_bytes = 8ull << 20; limits.playback_epoch = epoch_;
    if (!packets_ || !parser_.Initialize(arena_, source_, limits)) { Close(); return Fail(4); }
    // The metadata, and the tables a seek needs (the reads are synchronous: a bounded number of steps).
    for (unsigned i = 0; i < 1000000 && !(parser_.MetadataReady() && (parser_.Indexed() || parser_.Fragmented())); ++i) {
        const auto status = parser_.Step(*packets_);
        if (status == ParseStatus::Failed) { Close(); return Fail(5); }
        if (status == ParseStatus::Done) { parsed_ = true; break; }
        if (status == ParseStatus::Backpressure) break;
    }
    if (!parser_.MetadataReady()) { Close(); return Fail(5); }
    // Tracks: the first usable video track; every audio track this engine can decode (PCM, or the host's decoder).
    for (std::uint32_t i = 0; i < parser_.TrackCount(); ++i) {
        const auto* t = parser_.Track(i);
        if (!t || t->refusal != demux::ParseError::None) continue;
        if (t->kind == demux::TrackKind::Video && video_index_ < 0 && (t->codec == Codec::H264 || t->codec == Codec::Hevc) &&
            (options.decode_video || options.gpu || t->codec == Codec::H264)) { video_index_ = static_cast<std::int32_t>(i); video_ = *t; video_id_ = t->id; }
        if (sound && t->kind == demux::TrackKind::Audio && track_count_ < kMaxAudioTracks &&
            (t->codec == Codec::Pcm || ((t->codec == Codec::Aac || t->codec == Codec::Opus) && options.decode_audio))) {
            auto& a = tracks_[track_count_++];
            a.index = i; a.id = t->id; a.rate = t->sample_rate; a.channels = t->channels; a.codec = t->codec;
            ai::CanonicalLanguage(t->language, a.language);
        }
        duration_ = (std::max)(duration_, t->duration_100ns);
    }
    // The file's default audio track first (enabled), else the first.
    for (unsigned k = 0; k < track_count_; ++k) if (parser_.Track(tracks_[k].index)->enabled) { track_ = k; break; }
    audio_ = track_count_ > 0;
    for (std::uint32_t i = 0; i < parser_.TrackCount(); ++i) {
        const bool emit = static_cast<std::int32_t>(i) == video_index_ || (audio_ && i == tracks_[track_].index);
        (void)parser_.SetEmission(i, emit);
    }
    if (video_index_ < 0 && !audio_) { Close(); return Fail(6); }
    if (video_index_ >= 0 && !PrepareVideo()) { Close(); return Fail(7); }
    if (audio_ && !PrepareAudio()) { Close(); return Fail(8); }
    if ((options.modules & kModuleSubtitles) && !PrepareSubtitles(options.subtitle_path)) { Close(); return Fail(9); }
    Catalogue();
    landing_ = 0; landing_due_ = true; audio_trim_ = 0; written_end_ = 0;
    return FinishOpen(options);
}
bool HostEngine::FinishOpen(const HostOptions& options) noexcept {
    text_gate_.Configure({16000, 4000, options.voice_wake, options.push_to_talk}, nullptr, nullptr);
    if (!text_gate_.SetWord(options.wake_word ? options.wake_word : "")) { Close(); return Fail(1); }
    if (options.voice && (options.modules & kModuleVoice) && !PrepareVoice(options)) { Close(); return Fail(10); }
    if (options.modules & kModuleTelemetry) {
        signatures_ = telemetry_arena_.Create<core::SpscRingBuffer<FrameSignature, 64>>();
        controls_.analysis.store(true, std::memory_order_relaxed);
    }
    dispatcher_.Configure(&HostEngine::HandleIntent, this); dispatcher_.Converse(&conversation_); conversation_.Reset(); cures_.Reset();
    // Upright as the file's tkhd says (a camera: as it is), its own horizon and bars (auto-leveling and auto-crop stay as the
    // viewer left them).
    orientation_ = {};
    if (!sensor_ && video_index_ >= 0 && video::Orientation::Axes(video_.transform)) for (unsigned i = 0; i < 4; ++i) orientation_.file[i] = video_.transform[i];
    horizon_.Reset(); matte_.Reset(crop_width_, crop_height_); cropped_changes_ = 0; surveyed_ = 0;
    flush_request_.store(epoch_); flush_ack_.store(epoch_); pump_epoch_ = epoch_; ring_origin_.store(0);
    if (!arena_.Seal()) { Close(); return Fail(2); }
    for (auto* view : {&voice_arena_, &subtitle_arena_, &chapter_arena_, &telemetry_arena_})
        if (view->Statistics().capacity_bytes && !view->Seal()) { Close(); return Fail(2); }
    phase_ = Phase::Primed; open_ = true;
    return true;
}
bool HostEngine::OpenSensor(const SensorOptions& s, const HostOptions& options) noexcept {
    Close();
    options_ = options; error_ = 0; phase_ = Phase::Preparing; sensor_ = true; sensor_options_ = s;
    const bool camera = s.width || s.height, microphones = s.audio_channels != 0;
    if (camera && (s.width < 64 || s.height < 64 || s.width > 4096 || s.height > 2304 || (s.width & 1) || (s.height & 1))) return Fail(1);
    if (microphones && (s.audio_channels > 8 || s.audio_rate < 8000 || s.audio_rate > 192000)) return Fail(1);
    if ((!camera && !microphones) || options.audio_channels > 2 || (options.audio_channels && (options.audio_rate < 8000 || options.audio_rate > 192000))) return Fail(1);
    if (!arena_.Initialize(ArenaBudget(options.modules))) return Fail(2);
    if (!PrepareModules(options)) { Close(); return Fail(2); }
    fov_ = s.field_of_view > 0.05f && s.field_of_view < 3.1f ? s.field_of_view : 1.5707963f;
    if (camera) {
        // Two pictures (the one shown and the next being filled), the tracker's grid, telemetry's block codes, perception.
        video_index_ = 0; coded_width_ = crop_width_ = s.width; coded_height_ = crop_height_ = s.height; crop_left_ = crop_top_ = 0;
        const std::size_t luma = std::size_t{s.width} * s.height, chroma = luma / 4;
        for (unsigned i = 0; i < 2; ++i) {
            auto& p = pictures_[i];
            p.y = static_cast<std::uint8_t*>(arena_.Allocate(luma, 64));
            p.cb = static_cast<std::uint8_t*>(arena_.Allocate(chroma, 64)); p.cr = static_cast<std::uint8_t*>(arena_.Allocate(chroma, 64));
            if (!p.y || !p.cb || !p.cr) { Close(); return Fail(7); }
        }
        code_count_ = ((s.width + 7) / 8) * ((s.height + 7) / 8);
        codes_ = static_cast<std::uint16_t*>(arena_.Allocate(sizeof(std::uint16_t) * code_count_, 64));
        grid_scale_ = video::GridScale(s.width, s.height);
        grid_width_ = (s.width + grid_scale_ - 1) / grid_scale_; grid_height_ = (s.height + grid_scale_ - 1) / grid_scale_;
        grid_ = static_cast<std::uint8_t*>(arena_.Allocate(static_cast<std::size_t>(grid_width_) * grid_height_, 64));
        if (!codes_ || !grid_) { Close(); return Fail(7); }
        if (options.modules & kModuleTelemetry) (void)perception_.Configure(arena_, s.width, s.height, fov_);
        stabilizer_.Configure(s.width, s.height, fov_); stabilize_ = false;
        PrepareSensing(s.width, s.height);
    }
    if (microphones) {
        sensor_input_ = static_cast<float*>(arena_.Allocate(sizeof(float) * kChunk * s.audio_channels, 64));
        if (!sensor_input_) { Close(); return Fail(8); }
        if (s.audio_channels >= 2) (void)locator_.Configure(s.audio_rate, s.audio_channels, s.microphone_spacing);
        if (options.audio_channels) {
            // What the host pumps: the microphones through the DSP (converted to the output's rate and channels first).
            if (s.audio_rate != options.audio_rate || s.audio_channels != options.audio_channels) {
                if (!audio::PcmConverter::Supported(s.audio_rate, s.audio_channels, options.audio_rate, options.audio_channels)) { Close(); return Fail(8); }
                sensor_converter_ = arena_.Create<audio::PcmConverter>();
                if (!sensor_converter_ || !sensor_converter_->Configure(arena_, s.audio_rate, s.audio_channels, options.audio_rate, options.audio_channels, 4096)) { Close(); return Fail(8); }
            }
            if (!PrepareOutput()) { Close(); return Fail(8); }
            audio_ = true;
        }
    }
    landing_due_ = false; parsed_ = true;
    return FinishOpen(options);
}
bool HostEngine::IngestVideo(const SensorFrame& f) noexcept {
    if (!open_ || !sensor_ || video_index_ < 0) return false;
    const unsigned w = crop_width_, h = crop_height_;
    if (f.width != w || f.height != h || !f.planes[0] || (sensor_last_ >= 0 && f.pts_100ns <= sensor_last_)) return false;
    const bool rgba = f.format == kPixelRgba8;
    if (!rgba && (f.format != kPixelNv12 && f.format != kPixelP010)) return false;
    const unsigned bytes = f.format == kPixelP010 ? 2u : 1u;
    if (rgba ? f.strides[0] < w * 4 : (!f.planes[1] || f.strides[0] < w * bytes || f.strides[1] < w * bytes)) return false;
    auto& p = pictures_[sensor_slot_]; sensor_slot_ ^= 1u;
    if (f.format == kPixelNv12) {
        for (unsigned y = 0; y < h; ++y) std::memcpy(p.y + static_cast<std::size_t>(y) * w, f.planes[0] + static_cast<std::size_t>(y) * f.strides[0], w);
        for (unsigned y = 0; y < h / 2; ++y) {
            const auto* uv = f.planes[1] + static_cast<std::size_t>(y) * f.strides[1];
            for (unsigned x = 0; x < w / 2; ++x) { p.cb[y * (w / 2) + x] = uv[2 * x]; p.cr[y * (w / 2) + x] = uv[2 * x + 1]; }
        }
    } else if (f.format == kPixelP010) {
        // 10 bits in the high bits of little-endian 16-bit samples: the top 8, rounded.
        const auto take = [](const std::uint8_t* s) { const unsigned v = s[0] | (static_cast<unsigned>(s[1]) << 8); return static_cast<std::uint8_t>((std::min)(255u, (v + 128u) >> 8)); };
        for (unsigned y = 0; y < h; ++y) {
            const auto* row = f.planes[0] + static_cast<std::size_t>(y) * f.strides[0];
            for (unsigned x = 0; x < w; ++x) p.y[static_cast<std::size_t>(y) * w + x] = take(row + 2 * x);
        }
        for (unsigned y = 0; y < h / 2; ++y) {
            const auto* uv = f.planes[1] + static_cast<std::size_t>(y) * f.strides[1];
            for (unsigned x = 0; x < w / 2; ++x) { p.cb[y * (w / 2) + x] = take(uv + 4 * x); p.cr[y * (w / 2) + x] = take(uv + 4 * x + 2); }
        }
    } else {
        // RGBA8 (full-range BT.709 RGB) to limited-range BT.709 Y'CbCr 4:2:0, in integers (the same codes on every platform).
        constexpr int kr = 11966, kg = 40254, kb = 4064;            // 219/255 x (0.2126, 0.7152, 0.0722), 16-bit fixed point.
        for (unsigned y = 0; y < h; ++y) {
            const auto* row = f.planes[0] + static_cast<std::size_t>(y) * f.strides[0];
            for (unsigned x = 0; x < w; ++x)
                p.y[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(16 + ((kr * row[4 * x] + kg * row[4 * x + 1] + kb * row[4 * x + 2] + 32768) >> 16));
        }
        for (unsigned y = 0; y < h / 2; ++y)
            for (unsigned x = 0; x < w / 2; ++x) {
                int r = 0, g = 0, b = 0;
                for (unsigned j = 0; j < 2; ++j) {
                    const auto* q = f.planes[0] + static_cast<std::size_t>(2 * y + j) * f.strides[0] + 8 * x;
                    r += q[0] + q[4]; g += q[1] + q[5]; b += q[2] + q[6];
                }
                // Cb = 128 + 224/255 (B - Y) / 1.8556, Cr = 128 + 224/255 (R - Y) / 1.5748 on the 2x2 means (sums of 4), 16-bit fixed point.
                const int cb = -6596 * r - 22189 * g + 28785 * b, cr = 28785 * r - 26145 * g - 2640 * b;
                const auto code = [](int v) { return static_cast<std::uint8_t>((std::clamp)(128 + ((v + (v >= 0 ? 131072 : -131072)) / 262144), 0, 255)); };
                p.cb[y * (w / 2) + x] = code(cb); p.cr[y * (w / 2) + x] = code(cr);
            }
    }
    const bool first = sensor_last_ < 0;
    p.pts = f.pts_100ns; p.duration = first ? 0 : f.pts_100ns - sensor_last_; p.epoch = epoch_; p.decoded = true; p.queued = false; p.referenced = false;
    sensor_last_ = sensor_now_ = last_now_ = f.pts_100ns;
    phase_ = Phase::Running;
    DrainVoice();
    ego_ = frame_ego_ = f.ego; imu_frame_ = f.ego.valid; // The stabiliser and the steady fovea follow in Sense (Show).
    Show(p, first ? kPictureLanding : 0u, f.pts_100ns);
    return true;
}
bool HostEngine::IngestAudio(const void* samples, std::uint32_t format, std::uint32_t frames, std::int64_t pts) noexcept {
    const unsigned channels = sensor_options_.audio_channels, rate = sensor_options_.audio_rate;
    if (!open_ || !sensor_ || !channels || !samples || (format != kSampleF32 && format != kSampleS16)) return false;
    const unsigned out_channels = options_.audio_channels;
    for (std::uint32_t at = 0; at < frames;) {
        const std::uint32_t n = (std::min)(frames - at, kChunk);
        if (format == kSampleF32) std::memcpy(sensor_input_, static_cast<const float*>(samples) + static_cast<std::size_t>(at) * channels, sizeof(float) * n * channels);
        else {
            const auto* s16 = static_cast<const std::int16_t*>(samples) + static_cast<std::size_t>(at) * channels;
            for (std::uint32_t i = 0; i < n * channels; ++i) sensor_input_[i] = static_cast<float>(s16[i]) / 32768.0f;
        }
        const std::int64_t when = pts >= 0 ? pts + static_cast<std::int64_t>(static_cast<double>(at) * kSecond / rate) : -1;
        if (locator_.Configured()) locator_.Process(sensor_input_, n, when);
        if (audio_) {
            // Into the ring PumpAudio plays (converted to the output first); without room the samples are counted and dropped.
            const auto out = static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) * options_.audio_rate / rate) + 16;
            if (ring_.FreeFrames() < out) audio_overflow_.fetch_add(n, std::memory_order_relaxed);
            else if (!sensor_converter_) (void)WritePcm(sensor_input_, n, out_channels);
            else {
                const float* in = sensor_input_; unsigned left = n;
                while (left) {
                    const unsigned take = (std::min)(left, sensor_converter_->Room());
                    if (!take && !sensor_converter_->Available()) break;
                    (void)sensor_converter_->Push(in, take); in += static_cast<std::size_t>(take) * channels; left -= take;
                    while (const unsigned ready = (std::min)(sensor_converter_->Available(), 4096u)) {
                        const unsigned made = sensor_converter_->Pull(converted_, ready);
                        if (!WritePcm(converted_, made, out_channels)) break;
                    }
                }
            }
        }
        at += n;
    }
    return true;
}
HostTelemetry HostEngine::Telemetry() const noexcept {
    HostTelemetry t;
    t.perception = perception_.Result();
    t.horizon = horizon_.Locked(); t.horizon_tilt = horizon_.Target();
    t.bars = matte_.Bars(); t.active = matte_.Active();
    t.sound_known = locator_.Configured() && locator_.Latest(t.sound);
    t.saccade = saccade_; t.saccades = saccades_;
    t.view_zoom = zoom_; t.view_x = view_x_; t.view_y = view_y_; ShownView(t.view_zoom, t.view_x, t.view_y, t.view_roll);
    t.target_id = target_id_;
    if (vibro_) { t.vibrometry = vibro_->Result(); t.vibrometry_region = vibro_->Region(); t.vibrometry_following = vibro_->Following(); }
    t.virtual_imu = virtual_ego_; if (virtual_imu_.Configured()) t.virtual_motion = virtual_imu_.Result();
    return t;
}
bool HostEngine::SensorPicture(const std::uint8_t*& luma, const std::uint8_t*& cb, const std::uint8_t*& cr, std::uint32_t& luma_stride,
                               std::uint32_t& chroma_stride, std::int64_t& pts) const noexcept {
    if (!sensor_ || video_index_ < 0 || sensor_last_ < 0) return false;
    const auto& p = pictures_[sensor_slot_ ^ 1u]; // IngestVideo filled this one, then moved on.
    luma = p.y; cb = p.cb; cr = p.cr; luma_stride = crop_width_; chroma_stride = crop_width_ / 2; pts = p.pts;
    return true;
}
bool HostEngine::ShownPicture(const std::uint8_t*& luma, const std::uint8_t*& cb, const std::uint8_t*& cr, std::uint32_t& luma_stride,
                              std::uint32_t& chroma_stride, std::uint32_t& width, std::uint32_t& height, std::int64_t& pts) const noexcept {
    if (video_index_ < 0 || shown_ < 0) return false;
    const auto& p = pictures_[shown_];
    if (!p.decoded || !p.y) return false;
    const std::size_t offset = static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, chroma = static_cast<std::size_t>(crop_top_ / 2) * (coded_width_ / 2) + crop_left_ / 2;
    luma = p.y + offset; cb = p.cb + chroma; cr = p.cr + chroma; luma_stride = coded_width_; chroma_stride = coded_width_ / 2;
    width = crop_width_; height = crop_height_; pts = p.pts;
    return true;
}
void HostEngine::ShownView(float& zoom, float& view_x, float& view_y, float& roll) const noexcept {
    zoom = zoom_; view_x = view_x_; view_y = view_y_; roll = 0;
    if (stabilize_) stabilizer_.Apply(zoom, view_x, view_y, roll);
}
void HostEngine::SceneMotion(float& dx, float& dy, float& roll) const noexcept {
    dx = dy = roll = 0;
    const auto& ego = frame_ego_;
    if (!ego.valid) return;
    // Perception's model at the centre: yaw right moves the scene left, pitch up moves it down, roll clockwise turns it by -roll.
    const double focal = stabilizer_.Focal();
    dx = static_cast<float>(-focal * ego.yaw); dy = static_cast<float>(focal * ego.pitch); roll = -ego.roll;
}
ai::IntentStatus HostEngine::SetStabilization(bool on) noexcept {
    // A camera (its IMU, else the virtual IMU), or the bridge's pictures with the virtual IMU (2026-11-03).
    if (video_index_ < 0 || (!sensor_ && !(bridge_ && virtual_imu_.Configured()))) {
        if (on) Status("Stabilisation \xc2\xb7 needs the pictures and telemetry");
        return ai::IntentStatus::Unavailable;
    }
    if (on == stabilize_) return ai::IntentStatus::NoContext;
    stabilize_ = on; Status(on ? "Stabilisation on" : "Stabilisation off");
    return ai::IntentStatus::Ok;
}
ai::IntentStatus HostEngine::SetSaccade(bool on) noexcept {
    if (!on) { const bool was = saccade_; saccade_ = saccade_moving_ = false; if (was) Status("Audio saccade off"); return was ? ai::IntentStatus::Ok : ai::IntentStatus::NoContext; }
    if (!locator_.Configured() || video_index_ < 0) { Status("Audio saccade \xc2\xb7 needs stereo sound and a picture"); return ai::IntentStatus::Unavailable; }
    audio::SoundEvent e; saccade_seen_ = locator_.Latest(e) ? e.sequence : 0; // Only sounds from now on.
    saccade_ = true; Status("Audio saccade \xc2\xb7 looking at sounds");
    return ai::IntentStatus::Ok;
}
void HostEngine::Perceive(const Picture& p) noexcept {
    if (!perception_.Configured()) return;
    if (p.epoch != perception_epoch_) { perception_.Reset(); perception_epoch_ = p.epoch; }
    (void)perception_.Push(p.y + static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, coded_width_, p.pts, ego_);
    ego_ = {};
    FollowObject();
}
void HostEngine::FollowObject() noexcept {
    // The object the tracker is on: the one that holds its position (its box, and a twentieth of the picture around it), the
    // target's own track first. None holding it: the id stays (an object missed for a picture or two).
    const auto& st = tracker_.Status();
    if (!tracker_.Active()) { if (!track_pending_) target_id_ = 0; return; }
    if (st.state != video::TrackState::Tracking || track_pending_) return;
    if (const auto holder = ObjectAt(st.x, st.y)) target_id_ = holder;
}
std::uint32_t HostEngine::ObjectAt(float x, float y) const noexcept {
    const auto& r = perception_.Result();
    std::uint32_t holder = 0;
    for (unsigned i = 0; i < r.object_count; ++i) {
        const auto& o = r.objects[i];
        if (std::fabs(x - o.x) > 0.5f * o.width + 0.05f || std::fabs(y - o.y) > 0.5f * o.height + 0.05f) continue;
        if (!holder || o.track_id == target_id_) holder = o.track_id;
    }
    return holder;
}
ai::IntentStatus HostEngine::TrackObject(std::uint32_t track_id) noexcept {
    if (!grid_) { Status("Tracking \xc2\xb7 no picture to follow here"); return ai::IntentStatus::Unavailable; }
    const auto& r = perception_.Result();
    if (!perception_.Configured() || !r.object_count) { Status("Next target \xc2\xb7 no object in view"); return ai::IntentStatus::NotFound; }
    unsigned next = r.object_count;
    if (track_id) { for (unsigned i = 0; i < r.object_count; ++i) if (r.objects[i].track_id == track_id) next = i; }
    else {
        // After the one followed now, in the table's order (the most urgent first); the first when none is followed.
        int current = -1;
        if (tracker_.Active() || track_pending_) for (unsigned i = 0; i < r.object_count; ++i) if (r.objects[i].track_id == target_id_) current = static_cast<int>(i);
        if (current >= 0 && r.object_count == 1) { Status("Next target \xc2\xb7 the only one in view"); return ai::IntentStatus::NoContext; }
        next = current < 0 ? 0u : (static_cast<unsigned>(current) + 1) % r.object_count;
    }
    if (next >= r.object_count) { Status("Target \xc2\xb7 not in view"); return ai::IntentStatus::NotFound; }
    const auto& o = r.objects[next];
    // The view (and the audio focus's beam) at once; the lock on the next picture, there.
    if (zoom_ <= 1.0f) zoom_ = 2.0f;
    const float half = 0.5f / zoom_;
    view_x_ = std::clamp(o.x, half, 1.0f - half); view_y_ = std::clamp(o.y, half, 1.0f - half);
    slew_pts_ = -1; saccade_moving_ = false;
    SyncFocus();
    retarget_ = true; retarget_x_ = o.x; retarget_y_ = o.y; track_pending_ = true; target_id_ = o.track_id;
    Status(o.approaching ? "Next target \xc2\xb7 approaching" : "Next target");
    return ai::IntentStatus::Ok;
}
void HostEngine::Survey(const Picture& p) noexcept {
    if (!(options_.modules & kModuleTelemetry) && !auto_level_ && !auto_crop_) return;
    // What the windowed engine's statistics pass finds on the GPU, on the CPU: the horizon's edge orientations (1 KB of words on
    // the stack) and the picture's extents, both of the stored crop.
    const std::uint8_t* y = p.y + static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_;
    std::uint32_t words[video::HorizonLevel::kWords]{};
    video::HorizonLevel::Accumulate(y, coded_width_, crop_width_, crop_height_, words);
    horizon_.Push(p.pts, video::HorizonLevel::Measure(words));
    // The extents every picture until the bars are known, then every fourth (as the GPU's kMatte, 2026-10-31): a bar holds still.
    if (!matte_.Locked() || surveyed_ % 4 == 0) {
        std::uint32_t extents[video::Matte::kWords]{};
        video::Matte::Accumulate(y, coded_width_, crop_width_, crop_height_, extents);
        matte_.Push(p.pts, extents);
    }
    ++surveyed_;
    if (auto_level_ && horizon_.Locked()) {
        // The stored picture's tilt, clockwise; shown through a mirror it turns the other way. Leveled by the opposite turn, in
        // tenths of a degree (D3D12Renderer::FollowHorizon).
        const float shown = orientation_.Mirrored() ? -horizon_.Tilt() : horizon_.Tilt();
        orientation_.level = std::round(std::clamp(-shown, -video::HorizonLevel::kMaxTilt, video::HorizonLevel::kMaxTilt) * 10) / 10;
    }
    FollowBars();
}
void HostEngine::FollowBars() noexcept {
    if (!auto_crop_ || !matte_.Locked() || matte_.Changes() == cropped_changes_) return;
    const bool first = !cropped_changes_;
    cropped_changes_ = matte_.Changes();
    if (!matte_.Bars()) { if (!first) { zoom_ = 1; view_x_ = view_y_ = 0.5f; SyncFocus(); } return; }
    // The picture's shape inside the active area, as large as it fits, about the area's centre (D3D12Renderer::FollowBars).
    const auto r = matte_.Active();
    const float w = static_cast<float>(matte_.Width()), h = static_cast<float>(matte_.Height());
    const float left = static_cast<float>(r.left) + (r.left ? 2.0f : 0.0f), top = static_cast<float>(r.top) + (r.top ? 2.0f : 0.0f);
    const float right = static_cast<float>(r.right) - (static_cast<float>(r.right) < w ? 2.0f : 0.0f);
    const float bottom = static_cast<float>(r.bottom) - (static_cast<float>(r.bottom) < h ? 2.0f : 0.0f);
    if (right - left < 16 || bottom - top < 16) return;
    zoom_ = std::clamp((std::max)(w / (right - left), h / (bottom - top)), 1.0f, 8.0f);
    const float half = 0.5f / zoom_;
    view_x_ = std::clamp((left + right) / 2 / w, half, 1 - half); view_y_ = std::clamp((top + bottom) / 2 / h, half, 1 - half);
    SyncFocus();
}
void HostEngine::PrepareSensing(unsigned width, unsigned height) noexcept {
    // Vibrometry's window (~25 KB) and the event camera's sensor (at most ~3.5 MiB), when telemetry is on and the arena has room
    // for them and for what the rest of the open still takes (4 MiB: the audio path): neither is worth failing an open for.
    if (!(options_.modules & kModuleTelemetry)) return;
    const auto room = [this](std::uint64_t bytes) { const auto st = arena_.Statistics(); return st.capacity_bytes - st.used_bytes >= bytes + (4ull << 20); };
    if (room(sizeof(video::Vibrometry))) vibro_ = arena_.Create<video::Vibrometry>();
    if (room(video::EventCamera::Bytes(width, height))) (void)events_.Configure(arena_, width, height);
    // The virtual IMU (2026-11-03; ~0.5 MiB): the camera's motion where no IMU says it.
    if (room(video::VirtualImu::ArenaBytes())) (void)virtual_imu_.Configure(arena_, width, height, sensor_ ? fov_ : 0.0f);
    steady_x_ = steady_y_ = slow_x_ = slow_y_ = 0; steady_pts_ = -1; vibro_path_.Reset(); imu_frame_ = virtual_ego_ = cut_now_ = false;
}
video::AreaRect HostEngine::FoveaArea() const noexcept {
    // kFovea of the view across and down at its centre, moved with the camera's shake (the scene's steadied motion).
    const double w = crop_width_, h = crop_height_, fw = kFovea * w / zoom_, fh = kFovea * h / zoom_;
    const double cx = std::clamp(view_x_ * w + (steady_x_ - slow_x_), fw / 2, w - fw / 2), cy = std::clamp(view_y_ * h + (steady_y_ - slow_y_), fh / 2, h - fh / 2);
    return {cx - fw / 2, cy - fh / 2, cx + fw / 2, cy + fh / 2};
}
void HostEngine::Sense(const Picture& p) noexcept {
    // The camera's motion since the previous picture: the frame's IMU deltas (IngestVideo), else the virtual IMU's
    // (2026-11-03), which perception then takes out of its flow as it would an IMU's.
    const bool imu = imu_frame_;
    imu_frame_ = virtual_ego_ = false;
    if (!imu) {
        frame_ego_ = {};
        if (virtual_imu_.Configured()) {
            if (p.epoch != virtual_epoch_) { virtual_imu_.Reset(); virtual_epoch_ = p.epoch; }
            const auto& seen = perception_.Result(); // The objects of the picture before: moving on their own, left out.
            (void)virtual_imu_.Push(p.y + static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, coded_width_, seen.objects, seen.object_count);
            frame_ego_ = virtual_imu_.Ego(); virtual_ego_ = frame_ego_.valid;
        }
        ego_ = frame_ego_;
    }
    stabilizer_.Push(p.pts, frame_ego_);
    // The scene's motion for vibrometry's fovea: the camera's, less its part slower than kSteadySeconds (the shake stays).
    float dx = 0, dy = 0, roll = 0; SceneMotion(dx, dy, roll);
    const double dt = steady_pts_ >= 0 ? std::clamp(static_cast<double>(p.pts - steady_pts_) / kSecond, 0.0, 1.0) : 0.0;
    const double k = 1 - std::exp(-dt / kSteadySeconds);
    steady_x_ += dx; steady_y_ += dy; slow_x_ += (steady_x_ - slow_x_) * k; slow_y_ += (steady_y_ - slow_y_) * k;
    steady_pts_ = p.pts;
}
void HostEngine::Vibrate(const Picture& p) noexcept {
    if (!vibro_) return;
    const double w = crop_width_, h = crop_height_;
    video::AreaRect r = FoveaArea();
    const auto& st = tracker_.Status();
    if (p.epoch != vibro_epoch_) { vibro_->Reset(); vibro_path_.Reset(); vibro_epoch_ = p.epoch; } // Another timeline: another signal.
    bool follow = false;
    if (tracker_.Active() && st.state == video::TrackState::Tracking) {
        // The target's region, of the fovea's size, centred on its path (2026-11-03; docs/picture.md, "Moving targets"): it
        // follows the target and the window goes on. A lock on another target, a cut or the track found again elsewhere starts
        // it over. The path is a line through the last second of the tracker's positions: the region does not follow the
        // target's own vibration, which is what is measured.
        if (vibro_lock_ != locks_ || cut_now_ || st.reacquired) { vibro_->Reset(); vibro_path_.Reset(); vibro_lock_ = locks_; }
        const double fw = r.right - r.left, fh = r.bottom - r.top;
        double cx = st.x * w, cy = st.y * h;
        vibro_path_.Push(p.pts, cx, cy);
        (void)vibro_path_.At(p.pts, cx, cy);
        cx = std::clamp(cx, fw / 2, w - fw / 2); cy = std::clamp(cy, fh / 2, h - fh / 2);
        r = {cx - fw / 2, cy - fh / 2, cx + fw / 2, cy + fh / 2};
        follow = true;
    } else vibro_path_.Reset();
    const std::size_t luma = static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, chroma = static_cast<std::size_t>(crop_top_ / 2) * (coded_width_ / 2) + crop_left_ / 2;
    const video::Planes420 planes{p.y + luma, p.cb + chroma, p.cr + chroma, coded_width_, coded_width_ / 2, crop_width_, crop_height_};
    vibro_->Push(planes, r, p.pts, follow);
}
void HostEngine::Spike(const Picture& p) noexcept {
    if (!events_on_) return;
    if (p.epoch != events_epoch_) { events_.Reset(); events_epoch_ = p.epoch; }
    float dx = 0, dy = 0, roll = 0; SceneMotion(dx, dy, roll);
    (void)events_.Push(p.y + static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, coded_width_, p.pts, dx, dy, roll);
}
ai::IntentStatus HostEngine::SetEventCamera(const video::EventCameraOptions* options) noexcept {
    if (!options) { const bool was = events_on_; events_on_ = false; return was ? ai::IntentStatus::Ok : ai::IntentStatus::NoContext; }
    if (!events_.Configured()) return ai::IntentStatus::Unavailable;
    events_.SetOptions(*options); events_on_ = true;
    return ai::IntentStatus::Ok;
}
bool HostEngine::TensorArea(TensorRegion kind, video::AreaRect f, double aspect, video::AreaRect& out) const noexcept {
    if (video_index_ < 0 || shown_ < 0 || !pictures_[shown_].decoded || !(aspect > 0)) return false;
    const double w = crop_width_, h = crop_height_;
    if (kind == TensorRegion::Full) { out = {0, 0, w, h}; return true; }
    if (kind == TensorRegion::Custom) {
        if (!(f.left >= 0 && f.top >= 0 && f.right <= 1 && f.bottom <= 1 && f.right > f.left && f.bottom > f.top)) return false;
        out = {f.left * w, f.top * h, f.right * w, f.bottom * h};
        return true;
    }
    double cx, cy, rw, rh;
    if (kind == TensorRegion::View) {
        float zoom = 1, vx = 0.5f, vy = 0.5f; ShownView(zoom, vx, vy);
        rw = w / zoom; rh = h / zoom; cx = vx * w; cy = vy * h;
    } else {
        const auto r = FoveaArea();
        cx = (r.left + r.right) / 2; cy = (r.top + r.bottom) / 2; rw = r.right - r.left; rh = r.bottom - r.top;
        const auto& st = tracker_.Status();
        if (tracker_.Active() && st.state == video::TrackState::Tracking) {
            // The target: its object's box grown by a fifth, else the fovea's size about the tracker.
            cx = st.x * w; cy = st.y * h;
            const auto& o = perception_.Result();
            for (unsigned i = 0; i < o.object_count; ++i)
                if (target_id_ && o.objects[i].track_id == target_id_) {
                    cx = o.objects[i].x * w; cy = o.objects[i].y * h; rw = 1.2 * o.objects[i].width * w; rh = 1.2 * o.objects[i].height * h;
                }
        }
        // The tensor's shape: the smaller side grown, then the whole scaled into the picture when it does not fit.
        if (rw / rh < aspect) rw = rh * aspect; else rh = rw / aspect;
        const double fit = (std::min)({1.0, w / rw, h / rh});
        rw *= fit; rh *= fit;
    }
    rw = (std::min)(rw, w); rh = (std::min)(rh, h);
    cx = std::clamp(cx, rw / 2, w - rw / 2); cy = std::clamp(cy, rh / 2, h - rh / 2);
    out = {cx - rw / 2, cy - rh / 2, cx + rw / 2, cy + rh / 2};
    return true;
}
ai::IntentStatus HostEngine::SetOrientation(unsigned quarter_turns, float level) noexcept {
    if (video_index_ < 0) return ai::IntentStatus::Unavailable;
    if (!(level == level) || level < -video::HorizonLevel::kMaxTilt || level > video::HorizonLevel::kMaxTilt) return ai::IntentStatus::OutOfRange;
    orientation_.turns = quarter_turns % 4; orientation_.level = std::round(level * 10) / 10;
    auto_level_ = false; // The viewer's own turn: auto-leveling stops.
    return ai::IntentStatus::Ok;
}
ai::IntentStatus HostEngine::SetAutoLevel(bool on) noexcept {
    if (video_index_ < 0) return ai::IntentStatus::Unavailable;
    if (on == auto_level_) return ai::IntentStatus::NoContext;
    if (on) horizon_.Reset(); // Off keeps the turn reached.
    auto_level_ = on; Status(on ? "Auto level \xc2\xb7 on" : "Auto level \xc2\xb7 off");
    return ai::IntentStatus::Ok;
}
ai::IntentStatus HostEngine::SetAutoCrop(bool on) noexcept {
    if (video_index_ < 0) return ai::IntentStatus::Unavailable;
    if (on == auto_crop_) return ai::IntentStatus::NoContext;
    if (!on && cropped_changes_ && matte_.Bars()) { zoom_ = 1; view_x_ = view_y_ = 0.5f; SyncFocus(); }
    auto_crop_ = on; cropped_changes_ = 0;
    FollowBars();
    Status(!on ? "Crop \xc2\xb7 off" : matte_.Bars() ? "Crop \xc2\xb7 black bars" : matte_.Locked() ? "Crop \xc2\xb7 no black bars" : "Crop \xc2\xb7 looking for black bars");
    return ai::IntentStatus::Ok;
}
void HostEngine::Saccade(std::int64_t pts) noexcept {
    if (!saccade_) { slew_pts_ = -1; return; }
    audio::SoundEvent e;
    if (locator_.Latest(e) && e.sequence != saccade_seen_) {
        saccade_seen_ = e.sequence;
        if (!tracker_.Active() && !track_pending_) { // Tracking keeps the view on its target.
            if (zoom_ <= 1.0f) zoom_ = 2.0f;         // A view narrower than the picture, so that it can turn.
            const float half = 0.5f / zoom_;
            // The direction's sine to the picture: x = 1/2 + tan(asin a) / (2 tan(fov / 2)).
            const double a = std::clamp(static_cast<double>(e.azimuth), -0.999, 0.999), t = std::tan(std::asin(a)) / std::tan(fov_ * 0.5);
            saccade_x_ = std::clamp(static_cast<float>(0.5 + 0.5 * t), half, 1.0f - half);
            saccade_moving_ = true; ++saccades_;
            Status(e.azimuth > 0.15f ? "Looking at the sound \xc2\xb7 right" : e.azimuth < -0.15f ? "Looking at the sound \xc2\xb7 left" : "Looking at the sound \xc2\xb7 ahead");
        }
    }
    if (saccade_moving_) {
        // A saccade: most of the way within a picture or two (time constant 40 ms), the audio focus with the view.
        const double dt = slew_pts_ >= 0 ? std::clamp(static_cast<double>(pts - slew_pts_) / kSecond, 0.0, 0.2) : 1.0 / 30.0;
        const float k = static_cast<float>(1.0 - std::exp(-dt / 0.04));
        const float half = 0.5f / zoom_;
        view_x_ = std::clamp(view_x_ + (saccade_x_ - view_x_) * k, half, 1.0f - half);
        if (std::fabs(view_x_ - saccade_x_) < 0.002f) { view_x_ = saccade_x_; saccade_moving_ = false; }
        SyncFocus();
    }
    slew_pts_ = pts;
}
bool HostEngine::PrepareVideo() noexcept {
    // The decoder configuration record, and its parameter sets for the syntax parser.
    const auto& range = video_.configuration;
    if (!range.bytes || range.bytes > (1u << 20)) return false;
    video_config_bytes_ = static_cast<std::uint32_t>(range.bytes);
    video_config_ = static_cast<std::byte*>(arena_.Allocate(video_config_bytes_, 16));
    if (!video_config_ || !source_.ReadAt(range.offset, video_config_, video_config_bytes_)) return false;
    const bool h264 = video_.codec == Codec::H264;
    if (h264 ? !h264_.Initialize(arena_) : !hevc_.Initialize(arena_)) return false;
    for (std::uint32_t i = 0; i < video_.parameter_count; ++i) {
        const auto& p = video_.parameters[i].range;
        if (p.offset < range.offset || p.offset + p.bytes > range.offset + range.bytes) return false;
        const std::span<const std::byte> nal{video_config_ + (p.offset - range.offset), static_cast<std::size_t>(p.bytes)};
        if ((h264 ? h264_.ParameterSet(nal) : hevc_.ParameterSet(nal)) != codec::SyntaxStatus::Ready) return false;
    }
    crop_width_ = video_.width; crop_height_ = video_.height;
    bool ten_bit = false;
    if (h264) {
        const auto* sps = h264_.FirstSps();
        if (!sps) return false;
        coded_width_ = sps->width; coded_height_ = sps->height;
        crop_left_ = sps->left; crop_top_ = sps->top; crop_width_ = sps->right - sps->left; crop_height_ = sps->bottom - sps->top;
    }
    // API 14: the GPU first (docs/sdk.md, "Hardware decoding in host mode"); when it cannot decode this stream, the host's decoder
    // or, for H.264, the software bridge, as before (no cost: nothing of the GPU's is made).
    gpu_ = false;
    if (options_.gpu) {
        unsigned width = coded_width_, height = coded_height_, crop_height = crop_height_, reorder = h264 ? h264_.FirstSps()->reorder : 0u;
        const codec::HevcSps* hevc = h264 ? nullptr : hevc_.FirstSps();
        if (hevc) { width = hevc->width; height = hevc->height; crop_height = hevc->bottom - hevc->top; ten_bit = hevc->bit_depth_luma_minus8 == 2; reorder = hevc->max_num_reorder; }
        if ((h264 || hevc) && width && height && options_.gpu->PrepareDecode(arena_, video_, width, height, crop_height, ten_bit, reorder)) {
            gpu_ = true;
            if (hevc) {
                coded_width_ = hevc->width; coded_height_ = hevc->height;
                crop_left_ = hevc->left; crop_top_ = hevc->top; crop_width_ = hevc->right - hevc->left; crop_height_ = hevc->bottom - hevc->top;
            }
        }
    }
    if (!gpu_ && !h264 && !options_.decode_video) return false; // HEVC with neither the GPU nor the host's decoder.
    bridge_ = h264 && !options_.decode_video && !gpu_;
    if (bridge_) {
        if (coded_width_ > 4096 || coded_height_ > 2304) return false; // The pool's planes come from the core region.
        const std::size_t luma = static_cast<std::size_t>(coded_width_) * coded_height_, chroma = luma / 4;
        for (auto& p : pictures_) {
            p.y = static_cast<std::uint8_t*>(arena_.Allocate(luma, 64));
            p.cb = static_cast<std::uint8_t*>(arena_.Allocate(chroma, 64)); p.cr = static_cast<std::uint8_t*>(arena_.Allocate(chroma, 64));
            if (!p.y || !p.cb || !p.cr) return false;
        }
        // Telemetry's block codes and the tracker's grid.
        code_count_ = ((crop_width_ + 7) / 8) * ((crop_height_ + 7) / 8);
        codes_ = static_cast<std::uint16_t*>(arena_.Allocate(sizeof(std::uint16_t) * code_count_, 64));
        grid_scale_ = video::GridScale(crop_width_, crop_height_);
        grid_width_ = (crop_width_ + grid_scale_ - 1) / grid_scale_; grid_height_ = (crop_height_ + grid_scale_ - 1) / grid_scale_;
        grid_ = static_cast<std::uint8_t*>(arena_.Allocate(static_cast<std::size_t>(grid_width_) * grid_height_, 64));
        if (!codes_ || !grid_) return false;
        if (options_.modules & kModuleTelemetry) (void)perception_.Configure(arena_, crop_width_, crop_height_, 0); // Too small: not measured.
        stabilizer_.Configure(crop_width_, crop_height_, 0); stabilize_ = false; // The virtual IMU's shake (2026-11-03).
        PrepareSensing(crop_width_, crop_height_);
    }
    return true;
}
bool HostEngine::PrepareAudio() noexcept {
    const unsigned rate = options_.audio_rate, channels = options_.audio_channels;
    for (unsigned k = 0; k < track_count_; ++k) {
        auto& t = tracks_[k];
        if (!t.rate || !t.channels || t.channels > 8) return false;
        if (t.rate != rate || t.channels != channels) {
            if (!audio::PcmConverter::Supported(t.rate, t.channels, rate, channels)) return false;
            t.converter = arena_.Create<audio::PcmConverter>();
            if (!t.converter || !t.converter->Configure(arena_, t.rate, t.channels, rate, channels, 4096)) return false;
        }
        const auto* info = parser_.Track(t.index);
        if (t.codec != Codec::Pcm && info->configuration.bytes) {
            audio_config_bytes_[k] = static_cast<std::uint32_t>(info->configuration.bytes);
            audio_config_[k] = static_cast<std::byte*>(arena_.Allocate(audio_config_bytes_[k], 16));
            if (!audio_config_[k] || !source_.ReadAt(info->configuration.offset, audio_config_[k], audio_config_bytes_[k])) return false;
        }
    }
    if (!PrepareOutput()) return false;
    // The audio saccade on a film: the programme's pair, as it is played.
    locate_in_pump_ = channels >= 2 && locator_.Configure(rate, channels, 0);
    return true;
}
bool HostEngine::PrepareOutput() noexcept {
    const unsigned rate = options_.audio_rate, channels = options_.audio_channels;
    decoded_capacity_ = 8192;
    decoded_ = static_cast<float*>(arena_.Allocate(sizeof(float) * decoded_capacity_ * 8, 64));
    converted_ = static_cast<float*>(arena_.Allocate(sizeof(float) * 4096 * channels, 64));
    pump_ = static_cast<float*>(arena_.Allocate(sizeof(float) * kChunk * channels, 64));
    analysis_ = arena_.Create<audio::SpectralTracker>();
    if (!decoded_ || !converted_ || !pump_ || !analysis_) return false;
    dsp_.AttachAnalysis(analysis_);
    return ring_.Initialize(arena_, kRingFrames, channels * 4) && dsp_.Configure(rate, channels) && gain_.Configure(rate, channels) &&
           stretch_.Configure(arena_, rate, channels);
}
bool HostEngine::PrepareSubtitles(const char* path) noexcept {
    // The subtitle module's region (API 17): the sidecar's read buffer, the file's own tracks' texts, then one region per subtitle
    // state (the live one, the one an attach builds). Views of the region: nothing is allocated after open.
    const auto region = kModuleBudgets[ModuleIndex(kModuleSubtitles)].arena_bytes;
    const std::size_t read = std::size_t{kMaxAttachBytes} + 64, embedded = 6u << 20, state = static_cast<std::size_t>((region - read - embedded - 4 * 65536) / 2);
    void* regions[4]{subtitle_arena_.Allocate(read, 64), subtitle_arena_.Allocate(embedded, 64), subtitle_arena_.Allocate(state, 64), subtitle_arena_.Allocate(state, 64)};
    if (!regions[0] || !regions[1] || !regions[2] || !regions[3] || !read_arena_.InitializeView(regions[0], read) ||
        !embedded_arena_.InitializeView(regions[1], embedded) || !state_arenas_[0].InitializeView(regions[2], state) || !state_arenas_[1].InitializeView(regions[3], state))
        return false;
    (void)ImportEmbedded(); // A file whose own tracks cannot be read still plays (they are listed, not selectable).
    std::string_view sidecar;
    if (path) {
        std::FILE* f = platform::OpenUtf8(path);
        if (!f) return false;
        std::setvbuf(f, nullptr, _IONBF, 0);
        std::fseek(f, 0, SEEK_END); const long size = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        char* text = size > 0 && size <= static_cast<long>(kMaxAttachBytes) ? static_cast<char*>(read_arena_.Allocate(static_cast<std::size_t>(size), 16)) : nullptr;
        const bool ok = text && std::fread(text, 1, static_cast<std::size_t>(size), f) == static_cast<std::size_t>(size);
        std::fclose(f);
        if (!ok) return false;
        sidecar = {text, static_cast<std::size_t>(size)};
    }
    if (!path && !embedded_count_) return true; // Nothing to show (an attach may bring some).
    if (!BuildSubtitles(0, sidecar)) return !path; // A sidecar without a cue fails the open, as before; the file's own may be empty.
    live_subtitles_ = 0; sidecar_ = path != nullptr;
    PublishSubtitles();
    return true;
}
bool HostEngine::BuildSubtitles(unsigned buffer, std::string_view sidecar) noexcept {
    // Channels: the sidecar first (when there is one), then the file's own tracks in their order.
    std::string_view sources[subtitle::SubtitleTrack::kMaxChannels]; unsigned n = 0;
    if (!sidecar.empty()) sources[n++] = sidecar;
    for (unsigned k = 0; k < embedded_count_ && n < subtitle::SubtitleTrack::kMaxChannels; ++k) sources[n++] = embedded_[k];
    if (!n) return false;
    auto& arena = state_arenas_[buffer];
    if (arena.Statistics().used_bytes && !arena.Reset()) return false;
    std::destroy_at(&subtitle_tracks_[buffer]); std::construct_at(&subtitle_tracks_[buffer]);
    std::destroy_at(&searches_[buffer]); std::construct_at(&searches_[buffer]);
    return subtitle_tracks_[buffer].Prepare(arena, std::span<const std::string_view>{sources, n}) && searches_[buffer].Prepare(arena, subtitle_tracks_[buffer]);
}
bool HostEngine::ImportEmbedded() noexcept {
    // Each sample read from the file at its offset (setup: the source's direct reads), converted into a WebVTT text.
    embedded_count_ = 0;
    if (!parser_.Indexed()) return false;
    static std::uint8_t raw[8192]; // Preparing only, one caller at a time (an engine opens on its host's thread).
    for (std::uint32_t i = 0; i < parser_.TrackCount() && embedded_count_ < subtitle::SubtitleTrack::kMaxChannels - 1; ++i) {
        const auto* t = parser_.Track(i);
        if (!t || t->refusal != demux::ParseError::None || t->kind != demux::TrackKind::Subtitle || !t->sample_count) continue;
        const auto format = t->codec == Codec::Tx3g ? subtitle::EmbeddedFormat::Tx3g : t->codec == Codec::Text ? subtitle::EmbeddedFormat::Text :
                            t->codec == Codec::Ssa ? subtitle::EmbeddedFormat::Ssa : static_cast<subtitle::EmbeddedFormat>(255);
        if (static_cast<unsigned>(format) > 2 || t->sample_count > subtitle::SubtitleTrack::kMaxCues) continue;
        std::uint64_t need = 16; demux::SampleRecord r{};
        for (std::uint32_t k = 0; k < t->sample_count; ++k) if (parser_.SampleAt(i, k, r)) need += subtitle::CueBound((std::min)(r.bytes, std::uint32_t{sizeof(raw)}));
        auto* out = need <= (4u << 20) ? static_cast<char*>(embedded_arena_.Allocate(static_cast<std::size_t>(need), 16)) : nullptr;
        if (!out) continue;
        std::size_t used = subtitle::WebVttHeader(out, static_cast<std::size_t>(need)); unsigned cues = 0;
        for (std::uint32_t k = 0; k < t->sample_count; ++k) {
            if (!parser_.SampleAt(i, k, r) || !r.bytes || r.bytes > sizeof(raw) || !source_.ReadAt(r.offset, raw, r.bytes)) continue;
            const auto wrote = subtitle::AppendCue(out + used, static_cast<std::size_t>(need) - used, format, r.pts_100ns, r.pts_100ns + r.duration_100ns, raw, r.bytes);
            used += wrote; cues += wrote ? 1u : 0u;
        }
        if (!cues) continue;
        embedded_ids_[embedded_count_] = t->id; embedded_[embedded_count_++] = {out, used};
    }
    return embedded_count_ != 0;
}
void HostEngine::Catalogue() noexcept {
    catalogue_count_ = 0;
    const bool subtitles = !Subs().Cues().empty();
    if (sidecar_ && subtitles) {
        auto& c = catalogue_[catalogue_count_++]; c = {};
        c.id = 0; c.kind = demux::TrackKind::Subtitle; c.codec = Codec::Text; std::memcpy(c.language, "und", 4);
        c.selectable = true; c.channel = 0; c.is_default = true;
    }
    for (std::uint32_t i = 0; i < parser_.TrackCount() && catalogue_count_ < kMaxCatalogue; ++i) {
        const auto* t = parser_.Track(i);
        if (!t) continue;
        auto& c = catalogue_[catalogue_count_++]; c = {};
        c.id = t->id; c.kind = t->kind; c.codec = t->codec; ai::CanonicalLanguage(t->language, c.language);
        std::memcpy(c.title, t->name, sizeof(c.title)); c.title[sizeof(c.title) - 1] = 0;
        c.channels = t->channels; c.rate = t->sample_rate; c.width = t->width; c.height = t->height; c.is_default = t->enabled;
        c.video = static_cast<std::int32_t>(i) == video_index_;
        if (c.video) c.selectable = true;
        for (unsigned k = 0; k < track_count_; ++k) if (tracks_[k].index == i) { c.audio_slot = static_cast<std::uint8_t>(k); c.selectable = true; }
        for (unsigned k = 0; k < embedded_count_; ++k)
            if (embedded_ids_[k] == t->id && subtitles) { c.channel = static_cast<std::int8_t>(k + (sidecar_ ? 1 : 0)); c.selectable = Subs().ChannelCues(static_cast<unsigned>(c.channel)) > 0; }
    }
    active_audio_.store(audio_ ? tracks_[track_].id : 0, std::memory_order_release);
}
void HostEngine::PublishSubtitles() noexcept {
    // The track shown: the first channel shown (its catalogue id), or none.
    std::int64_t id = -1;
    const auto shown = Subs().Shown();
    if (subtitles_shown_ && !Subs().Cues().empty() && shown) {
        unsigned channel = 0; while (channel < 32 && !((shown >> channel) & 1u)) ++channel;
        if (sidecar_ && channel == 0) id = 0;
        else { const unsigned k = channel - (sidecar_ ? 1u : 0u); if (k < embedded_count_) id = embedded_ids_[k]; }
    }
    active_subtitles_.store(id, std::memory_order_release);
}
bool HostEngine::AttachSubtitles(const char* path) noexcept {
    if (!path || !(options_.modules & kModuleSubtitles) || !read_arena_.Statistics().capacity_bytes) return false;
    // The read buffer is free: the live state copied its sidecar when it was built. The file is read through the OS directly
    // (no stdio stream: the C runtime allocates one), at most kMaxAttachBytes.
    if (!read_arena_.Reset()) return false;
    char* text = static_cast<char*>(read_arena_.Allocate(kMaxAttachBytes, 16));
    std::size_t size = 0;
    if (!text || !platform::ReadWholeFile(path, text, kMaxAttachBytes, size) || !size) return false;
    const unsigned other = Subs().Cues().empty() ? live_subtitles_ : 1u - live_subtitles_;
    if (!BuildSubtitles(other, {text, size})) return false;
    live_subtitles_ = other; sidecar_ = true; subtitles_shown_ = true;
    Subs().Show(1u); // The attached file is shown.
    Catalogue(); PublishSubtitles();
    return true;
}
bool HostEngine::PrepareVoice(const HostOptions& o) noexcept {
    voice_texts_ = voice_arena_.Create<core::SpscRingBuffer<VoiceText, 8>>();
    if (!voice_texts_ || !vad_.Configure(voice_arena_)) return false;
    // A custom name: its templates (formant renditions, and the host's voices' when it gave a source), as in the windowed engine.
    std::string_view word = o.wake_word ? std::string_view{o.wake_word} : std::string_view{}, rest;
    const bool builtin = word.empty() || (ai::AvionWakeGate::StripWake(word, rest) && rest.empty());
    bool configured = false;
    if (o.voice_wake && !builtin && ai::CustomWakeWord::Readable(word)) {
        auto* custom = voice_arena_.Create<ai::CustomWakeWord>();
        if (custom && custom->Synthesize(voice_arena_, word, ai::voice_templates::kSet.settings.scale, o.wake_speech) &&
            recogniser_.Configure(voice_arena_, ai::voice_templates::kSet, custom->Templates())) {
            wake_templates_ = static_cast<unsigned>(custom->Templates().size()); configured = true;
        }
    }
    if (!configured && !recogniser_.Configure(voice_arena_, ai::voice_templates::kSet)) return false;
    wake_gate_.Configure({vad_.Options().sample_rate, 4000, o.voice_wake, o.push_to_talk}, &HostEngine::HandleWake, this);
    if (!wake_gate_.SetWord(o.wake_word ? o.wake_word : "")) return false;
    voice_gate_.Configure(vad_, &ai::AcousticIntentRecognizer::Hear, &recogniser_, &ai::AvionWakeGate::Handle, &wake_gate_, 100);
    recogniser_.FollowWake(&wake_gate_);
    voice_on_ = true;
    return true;
}
void HostEngine::Close() noexcept {
    if (phase_ == Phase::Closed && !arena_.Statistics().capacity_bytes) return;
    if (gpu_ && options_.gpu) options_.gpu->CloseDecode(); // Its ledger is the arena's: before the arena goes.
    gpu_ = false;
    for (auto& p : pictures_) { p.gpu = {}; p.gpu_display = p.gpu_reference = false; }
    source_.Close();
    for (auto* view : {&read_arena_, &embedded_arena_, &state_arenas_[0], &state_arenas_[1]}) if (view->Statistics().capacity_bytes) (void)view->Shutdown();
    for (auto* view : {&voice_arena_, &subtitle_arena_, &chapter_arena_, &telemetry_arena_}) if (view->Statistics().capacity_bytes) (void)view->Shutdown();
    if (arena_.Statistics().capacity_bytes) (void)arena_.Shutdown();
    // The subtitle states pointed into the module's region (API 17): forgotten, so that the next file starts without cues.
    for (unsigned b = 0; b < 2; ++b) { std::destroy_at(&subtitle_tracks_[b]); std::construct_at(&subtitle_tracks_[b]); std::destroy_at(&searches_[b]); std::construct_at(&searches_[b]); }
    live_subtitles_ = 0; embedded_count_ = 0; sidecar_ = false; catalogue_count_ = 0; subtitles_shown_ = true;
    active_audio_.store(0, std::memory_order_relaxed); active_subtitles_.store(-1, std::memory_order_relaxed);
    open_ = false; phase_ = Phase::Closed; sensor_ = false; shown_ = -1; snapshot_requested_ = false;
    vibro_ = nullptr; events_.Release(); events_on_ = false; virtual_imu_.Release(); // Their storage was the arena's.
}

// ---- Step ----
StepStatus HostEngine::Step(std::int64_t now) noexcept {
    if (!open_) return StepStatus::Failed;
    if (wall_now_ == INT64_MIN) { wall_now_ = now; wall_media_ = 0; }
    last_now_ = now;
    if (phase_ == Phase::Primed || phase_ == Phase::Draining) phase_ = Phase::Running;
    if (phase_ == Phase::Stopped) return StepStatus::Ended;
    DrainVoice();
    if (sensor_) return StepStatus::Playing; // Live sensors: the pictures come with IngestVideo.
    if (!Pull()) return StepStatus::Failed;
    Present(now);
    const auto media = MediaNow(now);
    if (options_.maximum_seconds && media >= static_cast<std::int64_t>(options_.maximum_seconds) * kSecond) { phase_ = Phase::Stopped; return StepStatus::Ended; }
    bool pending = have_held_ || packets_->PeekReadSlot() != nullptr;
    for (const auto& p : pictures_) pending = pending || p.queued;
    // The end: everything read and shown, and the clock past the last sound and picture.
    if (parsed_ && !pending && media >= (std::max)(end_, written_end_) - kSecond / 50) { phase_ = Phase::Stopped; return StepStatus::Ended; }
    return StepStatus::Playing;
}
bool HostEngine::Pull() noexcept {
    for (unsigned i = 0; i < 512; ++i) {
        if (!have_held_) {
            if (const auto* p = packets_->PeekReadSlot()) { held_ = *p; (void)packets_->ReleaseReadSlot(); have_held_ = true; }
            else {
                if (parsed_) return true;
                const auto status = parser_.Step(*packets_);
                if (status == ParseStatus::Failed) return Fail(11);
                if (status == ParseStatus::Done) parsed_ = true;
                continue;
            }
        }
        bool consumed = false;
        if (!TakePacket(held_, consumed)) return false;
        if (!consumed) return true; // The pools are full: the next step.
        have_held_ = false;
    }
    return true;
}
bool HostEngine::TakePacket(const core::MediaPacket& packet, bool& consumed) noexcept {
    consumed = true;
    platform::BlockView view{};
    if (packet.playback_epoch == epoch_ && source_.Poll({packet.payload_block_id, packet.payload_generation}, view) == platform::IoStatus::Ready) {
        const std::span<const std::byte> bytes{view.data + packet.payload_offset, packet.payload_bytes};
        if (packet.stream_id == video_id_ && video_index_ >= 0) { if (!DecodeVideo(packet, bytes, view, consumed)) return false; }
        else if (audio_ && packet.stream_id == tracks_[track_].id) { if (!DecodeAudio(packet, bytes, consumed)) return false; }
    }
    if (consumed && !parser_.ReturnPacket(packet)) return Fail(12);
    return true;
}
HostEngine::Picture* HostEngine::FreeSlot() noexcept {
    for (auto& p : pictures_) if (!p.queued && !p.referenced) return &p;
    return nullptr;
}
void HostEngine::FreePicture(Picture& p) noexcept { p.queued = false; GpuForget(p, true, false); }
void HostEngine::GpuForget(Picture& p, bool display, bool reference) noexcept {
    if (!gpu_) return;
    if (display && p.gpu_display) { options_.gpu->Discard(p.gpu); p.gpu_display = false; }
    if (reference && p.gpu_reference) { options_.gpu->Unreference(p.gpu); p.gpu_reference = false; }
}
bool HostEngine::DecodeVideo(const core::MediaPacket& packet, std::span<const std::byte> bytes, const platform::BlockView& view, bool& consumed) noexcept {
    consumed = false;
    auto* slot = FreeSlot();
    if (!slot) return true;
    if (gpu_ && !options_.gpu->Ready()) return true; // API 14: the GPU's surfaces are all in use (pictures in flight): the next step.
    consumed = true;
    const auto index = static_cast<std::uint32_t>(slot - pictures_);
    slot->generation = ++generation_;
    slot->gpu = {}; slot->gpu_display = slot->gpu_reference = false;
    const platform::SurfaceHandle handle{index, slot->generation};
    platform::SurfaceHandle evicted[16]; unsigned count = 0;
    bool reference = false, decoded = false, key = (static_cast<std::uint32_t>(packet.flags) & static_cast<std::uint32_t>(core::PacketFlags::KeyFrame)) != 0;
    // Before a seek's target: decoded for reference only, never shown.
    bool display = packet.pts_100ns + (packet.duration_100ns > 0 ? packet.duration_100ns : 1) > landing_ || !landing_due_;
    const auto host = [&] {
        if (!options_.decode_video) return false;
        const HostVideoUnit unit{bytes.data(), static_cast<std::uint32_t>(bytes.size()), video_.nal_length_bytes, video_config_, video_config_bytes_,
                                 video_.codec == Codec::H264 ? kCodecH264 : kCodecHevc, packet.pts_100ns, packet.dts_100ns, packet.duration_100ns,
                                 packet.playback_epoch, (key ? kUnitKey : 0u) | (flush_unit_ ? kUnitFlush : 0u), crop_width_, crop_height_};
        flush_unit_ = false;
        return options_.decode_video(options_.decoder_context, unit) == 0;
    };
    // API 14: the GPU decodes it; its references are the slots' GPU pictures. Not submitted: nothing to show, nothing kept.
    bool on_gpu = false;
    const auto gpu = [&](const codec::H264Picture* h264, const codec::HevcPicture* hevc) {
        HostGpuSlot slots[kPictures];
        for (unsigned i = 0; i < kPictures; ++i) slots[i] = {pictures_[i].generation, pictures_[i].gpu, pictures_[i].gpu_reference || pictures_[i].gpu_display};
        platform::SurfaceHandle surface{};
        on_gpu = options_.gpu->Decode(h264, hevc, packet, view, display, slots, kPictures, surface) == HostGpu::Status::Submitted;
        if (on_gpu) { slot->gpu = surface; slot->gpu_display = display; slot->gpu_reference = reference; }
        else ++undecodable_;
    };
    if (video_.codec == Codec::H264) {
        if (h264_.Parse(bytes, video_.nal_length_bytes) != codec::SyntaxStatus::Ready) { ++undecodable_; return true; }
        const auto* picture = h264_.Picture();
        reference = picture->reference;
        if (gpu_) gpu(picture, nullptr);
        else if (bridge_) {
            // RefPicList0[0] of a P picture without list modification: the short-term frame with the largest FrameNumWrap.
            const codec::PicturePlanes out{slot->y, slot->cb, slot->cr, coded_width_, coded_width_ / 2};
            codec::PicturePlanes ref{}; const codec::PicturePlanes* from = nullptr;
            const auto max_frame = std::int64_t{1} << picture->sps->frame_bits;
            std::int64_t best = INT64_MIN;
            for (unsigned i = 0; i < picture->reference_count; ++i) {
                const auto& r = picture->references[i];
                if (r.long_term || r.surface.id >= kPictures || pictures_[r.surface.id].generation != r.surface.generation) continue;
                const std::int64_t wrap = r.frame_num > picture->frame_num ? r.frame_num - max_frame : r.frame_num;
                if (wrap > best) { best = wrap; auto& p = pictures_[r.surface.id]; ref = {p.y, p.cb, p.cr, coded_width_, coded_width_ / 2}; from = &ref; }
            }
            decoded = codec::H264PcmDecoder::Decode(*picture, bytes, out, from) == codec::SyntaxStatus::Ready;
            if (!decoded) ++undecodable_;
        } else if (!host()) ++undecodable_;
        // A picture the GPU did not take is not kept: later pictures predicted from it fail too, until the next IDR.
        if (!h264_.Commit(gpu_ && !on_gpu ? platform::SurfaceHandle{} : handle, evicted, count)) return Fail(13);
    } else {
        const auto status = hevc_.Parse(bytes, video_.nal_length_bytes);
        if (status == codec::SyntaxStatus::Discard) return true;
        if (status != codec::SyntaxStatus::Ready) { ++undecodable_; return true; }
        reference = hevc_.Picture()->retain;
        if (gpu_) { display = display && hevc_.Picture()->output; gpu(nullptr, hevc_.Picture()); } // pic_output_flag 0: a reference only.
        else if (!host()) ++undecodable_;
        if (!hevc_.Commit(gpu_ && !on_gpu ? platform::SurfaceHandle{} : handle, evicted, count)) return Fail(13);
    }
    for (unsigned i = 0; i < count; ++i)
        if (evicted[i].id < kPictures && pictures_[evicted[i].id].generation == evicted[i].generation) {
            auto& e = pictures_[evicted[i].id];
            e.referenced = false; GpuForget(e, false, true);
        }
    slot->referenced = reference && (!gpu_ || on_gpu); slot->decoded = decoded;
    slot->pts = packet.pts_100ns; slot->duration = packet.duration_100ns; slot->epoch = packet.playback_epoch;
    slot->queued = display && (!gpu_ || on_gpu);
    end_ = (std::max)(end_, slot->pts + slot->duration);
    return true;
}
bool HostEngine::WritePcm(const float* frames, std::uint32_t count, unsigned channels) noexcept {
    while (count) {
        platform::AudioWriteSpan span{};
        if (ring_.TryAcquire(count, span) != platform::EndpointStatus::Ready) return false;
        std::memcpy(span.data, frames, static_cast<std::size_t>(span.frames) * channels * sizeof(float));
        if (ring_.Commit(span.lease, span.frames) != platform::EndpointStatus::Ready) return false;
        frames += static_cast<std::size_t>(span.frames) * channels; count -= span.frames;
    }
    return true;
}
bool HostEngine::DecodeAudio(const core::MediaPacket& packet, std::span<const std::byte> bytes, bool& consumed) noexcept {
    consumed = false;
    if (flush_ack_.load(std::memory_order_acquire) != flush_request_.load(std::memory_order_relaxed)) return true; // Until the pump flushed.
    auto& t = tracks_[track_];
    const unsigned out_channels = options_.audio_channels;
    const auto* info = parser_.Track(t.index);
    std::uint32_t frames = 0;
    if (t.codec == Codec::Pcm) {
        const unsigned frame_bytes = t.channels * info->pcm_bits / 8u;
        frames = static_cast<std::uint32_t>(bytes.size() / frame_bytes);
    } else frames = 2048; // A compressed unit's frames are known once decoded; room for the longest (AAC: 1024, Opus: 960, HE: 2048).
    // Room for everything this packet becomes, converted (a margin for the converter's rounding), or try again later.
    // Decoded at most kAheadMs ahead of what the host has pumped: a track switch is heard that soon, not after a full ring.
    const auto out_frames = static_cast<std::uint32_t>(static_cast<std::uint64_t>(frames) * options_.audio_rate / t.rate) + 16;
    const auto queued = kRingFrames - ring_.FreeFrames(), ahead = options_.audio_rate * kAheadMs / 1000;
    if (ring_.FreeFrames() < out_frames || queued > ahead) return true;
    consumed = true;
    std::int64_t pts = packet.pts_100ns;
    const auto convert = [&](const float* in, std::uint32_t n) {
        if (!t.converter) return WritePcm(in, n, out_channels);
        while (n) {
            const unsigned take = (std::min)(n, t.converter->Room());
            if (!take && !t.converter->Available()) return false;
            (void)t.converter->Push(in, take); in += static_cast<std::size_t>(take) * t.channels; n -= take;
            while (const unsigned ready = (std::min)(t.converter->Available(), 4096u)) {
                const unsigned made = t.converter->Pull(converted_, ready);
                if (!WritePcm(converted_, made, out_channels)) return false;
            }
        }
        return true;
    };
    // Frames before the epoch's start (a seek's target, a switch's splice) are dropped.
    const auto trimmed = [&](std::uint32_t n) -> std::uint32_t {
        if (audio_trim_ <= pts) return 0;
        const auto skip = static_cast<std::uint64_t>((audio_trim_ - pts) * t.rate / kSecond);
        return static_cast<std::uint32_t>((std::min)(skip, static_cast<std::uint64_t>(n)));
    };
    if (t.codec == Codec::Pcm) {
        const unsigned bits = info->pcm_bits, frame_bytes = t.channels * bits / 8u;
        const std::uint32_t skip = trimmed(frames);
        for (std::uint32_t at = skip; at < frames;) {
            const std::uint32_t n = (std::min)(frames - at, decoded_capacity_);
            for (std::uint32_t f = 0; f < n; ++f)
                for (unsigned c = 0; c < t.channels; ++c)
                    decoded_[f * t.channels + c] = PcmSample(bytes.data() + static_cast<std::size_t>(at + f) * frame_bytes + c * (bits / 8u), bits, info->pcm_little_endian, info->pcm_float);
            if (!convert(decoded_, n)) return Fail(14);
            at += n;
        }
    } else {
        const HostAudioUnit unit{bytes.data(), static_cast<std::uint32_t>(bytes.size()), audio_config_[track_], audio_config_bytes_[track_],
                                 t.codec == Codec::Aac ? kCodecAac : kCodecOpus, t.rate, t.channels, audio_flush_unit_ ? kUnitFlush : 0u, pts};
        audio_flush_unit_ = false;
        const auto made = options_.decode_audio(options_.decoder_context, unit, decoded_, decoded_capacity_ * 8 / t.channels);
        frames = made > 0 ? static_cast<std::uint32_t>(made) : 0;
        const auto skip = trimmed(frames);
        if (frames > skip && !convert(decoded_ + static_cast<std::size_t>(skip) * t.channels, frames - skip)) return Fail(14);
    }
    written_end_ = (std::max)(written_end_, pts + static_cast<std::int64_t>(frames) * kSecond / t.rate);
    return true;
}
std::int64_t HostEngine::MediaNow(std::int64_t now) const noexcept {
    if (sensor_) return sensor_now_; // The newest picture's sensor time.
    if (paused_.load(std::memory_order_relaxed)) return paused_at_;
    if (landing_due_) return landing_;
    if (audio_) {
        clock::ClockEstimate e{};
        if (clock_.Estimate(now, e) == clock::ClockQuery::Ready && e.epoch == epoch_) return (std::max)(e.media_time_100ns, landing_);
        return landing_; // Not started yet (or starved): where it stands.
    }
    return wall_media_ + static_cast<std::int64_t>(static_cast<double>(now - wall_now_) * speed_.load(std::memory_order_relaxed));
}
void HostEngine::Present(std::int64_t now) noexcept {
    if (video_index_ < 0) return;
    if (landing_due_) {
        // The epoch's first picture at once (the start, a seek): the earliest queued one.
        Picture* first = nullptr;
        for (auto& p : pictures_) if (p.queued && p.epoch == epoch_ && (!first || p.pts < first->pts)) first = &p;
        if (!first) return;
        landing_ = (std::max)(landing_, first->pts);
        landing_due_ = false;
        if (!audio_) { wall_media_ = landing_; wall_now_ = now; }
        Show(*first, kPictureLanding, now);
        return;
    }
    if (paused_.load(std::memory_order_relaxed)) return;
    const auto media = MediaNow(now);
    Picture* due = nullptr;
    for (auto& p : pictures_) if (p.queued && p.epoch == epoch_ && p.pts <= media && (!due || p.pts > due->pts)) due = &p;
    if (!due) return;
    for (auto& p : pictures_) if (p.queued && &p != due && p.pts < due->pts) { FreePicture(p); ++dropped_; }
    Show(*due, 0, now);
}
void HostEngine::Show(Picture& p, std::uint32_t flags, std::int64_t now) noexcept {
    p.queued = false;
    if (p.gpu_display) {
        // API 14: converted on the GPU into a free shared texture and published (the host acquires it); the view as shown.
        float z = zoom_, x = view_x_, y = view_y_; ShownView(z, x, y);
        if (options_.gpu->Show(p.gpu, {p.pts, p.epoch, z, x, y})) flags |= kPictureTexture;
        p.gpu_display = false;
    }
    shown_ = static_cast<int>(&p - pictures_);
    if (p.decoded) { Sense(p); FollowTarget(p); Perceive(p); Survey(p); Vibrate(p); Spike(p); }
    Saccade(p.pts);
    const auto media = MediaNow(now);
    const char* caption = nullptr;
    if (subtitles_shown_ && !Subs().Cues().empty()) {
        unsigned indices[4]; const unsigned n = Subs().Select(p.pts, indices);
        for (unsigned i = 0; i < n && !caption; ++i) {
            const auto text = Subs().Cues()[indices[i]].text;
            const auto size = (std::min)(text.size(), sizeof(caption_) - 1);
            std::memcpy(caption_, text.data(), size); caption_[size] = 0; caption = caption_;
        }
    }
    const std::size_t luma = static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, chroma = static_cast<std::size_t>(crop_top_ / 2) * (coded_width_ / 2) + crop_left_ / 2;
    if (tracker_.Active()) flags |= kPictureTracking;
    float zoom = zoom_, view_x = view_x_, view_y = view_y_, roll = 0; ShownView(zoom, view_x, view_y, roll);
    HostPicture picture{p.pts, p.duration, p.epoch, sequence_++, crop_width_, crop_height_,
                        p.decoded ? p.y + luma : nullptr, p.decoded ? p.cb + chroma : nullptr, p.decoded ? p.cr + chroma : nullptr,
                        coded_width_, coded_width_ / 2, flags | (p.decoded ? kPictureDecoded : !gpu_ && options_.decode_video ? kPictureHost : 0u),
                        zoom, view_x, view_y, caption, status_[0] && media < status_until_ ? status_ : nullptr, roll,
                        orientation_.QuarterTurns(), orientation_.Mirrored(), 0.0f, 1.0f};
    if (orientation_.level != 0) {
        // The leveling turn, and the zoom that keeps its corners inside (video::Orientation::Map's f) for the picture as shown.
        constexpr double kPi = 3.14159265358979323846;
        const double radians = static_cast<double>(orientation_.level) * kPi / 180, w = orientation_.Across() ? crop_height_ : crop_width_,
                     h = orientation_.Across() ? crop_width_ : crop_height_;
        picture.view_level = static_cast<float>(radians);
        picture.level_zoom = static_cast<float>(std::fabs(std::cos(radians)) + std::fabs(std::sin(radians)) * (std::max)(w / h, h / w));
    }
    ++presented_;
    if (options_.present) options_.present(options_.present_context, picture);
    Signature(p, now);
}
void HostEngine::Signature(const Picture& p, std::int64_t now) noexcept {
    if (!signatures_ && !options_.frame_callback) return;
    FrameSignature s{};
    s.pts_100ns = p.pts; s.presented_100ns = now; s.sequence = signature_count_; s.epoch = p.epoch;
    s.luminance_mean = s.luminance_peak = s.tile_motion = -1; s.scene_cut_pts_100ns = -1;
    if (p.decoded && codes_) {
        // One sample at the centre of every 8x8 block of the crop, as the GPU's statistics pass takes it (docs/sdk.md).
        const bool continued = codes_valid_ && codes_epoch_ == p.epoch;
        std::uint64_t sum = 0; std::uint32_t count = 0, peak = 0, moved = 0;
        for (std::uint32_t by = 0; by * 8 + 4 < crop_height_; ++by)
            for (std::uint32_t bx = 0; bx * 8 + 4 < crop_width_; ++bx) {
                const auto y = p.y[static_cast<std::size_t>(crop_top_ + by * 8 + 4) * coded_width_ + crop_left_ + bx * 8 + 4];
                const auto code = static_cast<std::uint32_t>(y * 1023u + 127u) / 255u;
                const auto block = by * ((crop_width_ + 7) / 8) + bx;
                if (continued && (code > codes_[block] ? code - codes_[block] : codes_[block] - code) > 12u) ++moved;
                codes_[block] = static_cast<std::uint16_t>(code);
                sum += code; ++count; peak = (std::max)(peak, code);
            }
        if (count) {
            s.luminance_mean = static_cast<float>(static_cast<double>(sum) / count / 1023.0); s.luminance_peak = static_cast<float>(peak) / 1023.0f;
            s.tile_motion = continued ? static_cast<float>(moved) / static_cast<float>(count) : -1.0f;
            s.flags |= kFrameMeasured;
            codes_valid_ = true; codes_epoch_ = p.epoch;
            std::int64_t cut = 0;
            if (cuts_.Push(p.pts, s.luminance_mean, false, cut)) { s.flags |= kFrameSceneCut; s.scene_cut_pts_100ns = cut; }
        }
    }
    const float speech = speech_now_.load(std::memory_order_relaxed);
    const unsigned band = speech >= 0 ? band_now_.load(std::memory_order_relaxed) : audio::SpectralTracker::kBands;
    s.speech_probability = speech; s.dominant_band = band;
    s.dominant_band_hz = band < audio::SpectralTracker::kBands ? audio::SpectralTracker::kBandCentres[band] : -1.0f;
    s.dominant_band_db = band < audio::SpectralTracker::kBands ? band_db_now_.load(std::memory_order_relaxed) : -144.0f;
    ++signature_count_;
    if (signatures_) {
        if (auto* slot = signatures_->AcquireWriteSlot()) { *slot = s; (void)signatures_->CommitWriteSlot(); } else ++signatures_lost_;
    }
    if (options_.frame_callback) options_.frame_callback(options_.frame_context, s);
}
void HostEngine::FollowTarget(const Picture& p) noexcept {
    cut_now_ = false;
    if (!grid_ || (!track_pending_ && !tracker_.Active())) { grid_mean_ = -1; return; }
    video::MakeGrid(p.y + static_cast<std::size_t>(crop_top_) * coded_width_ + crop_left_, coded_width_, crop_width_, crop_height_, grid_scale_, grid_, grid_width_);
    const video::LumaGrid grid{grid_, grid_width_, grid_height_, grid_width_};
    std::uint64_t sum = 0; for (std::uint32_t i = 0; i < grid_width_ * grid_height_; ++i) sum += grid_[i];
    const float mean = static_cast<float>(static_cast<double>(sum) / (grid_width_ * grid_height_) / 255.0);
    // A cut for the tracker at once (the live detector confirms one four pictures later): the mean jumps as CutDetector's does.
    const bool cut = grid_mean_ >= 0 && std::fabs(mean - grid_mean_) > timeline::CutDetector::kCutLuma;
    grid_mean_ = mean; cut_now_ = cut;
    if (track_pending_) {
        track_pending_ = false;
        const bool retarget = retarget_; retarget_ = false;
        if (!tracker_.Lock(grid, retarget ? retarget_x_ : view_x_, retarget ? retarget_y_ : view_y_)) {
            target_id_ = 0; Status(retarget ? "Next target \xc2\xb7 nothing to follow there" : "No target in the middle \xc2\xb7 nothing to track"); return;
        }
        ++locks_;
        if (zoom_ <= 1.0f) zoom_ = 2.0f; // Tracking frames the target: the view follows it closer.
        if (!retarget) Status("Tracking \xc2\xb7 following the target"); // TrackObject said which.
        SyncFocus();
        return;
    }
    const auto was = tracker_.Status().state;
    const auto st = tracker_.Update(grid, cut);
    if (st.state == video::TrackState::Tracking) {
        const float half = 0.5f / zoom_;
        view_x_ = std::clamp(st.x, half, 1.0f - half); view_y_ = std::clamp(st.y, half, 1.0f - half);
        if (st.reacquired) Status("Tracking \xc2\xb7 target found again");
        SyncFocus();
    } else if (was == video::TrackState::Tracking) Status("Tracking lost \xc2\xb7 Holding view", 30000000); // The view holds.
}
void HostEngine::SyncFocus() noexcept {
    controls_.zoom.store(zoom_, std::memory_order_relaxed); controls_.zoom_x.store(view_x_, std::memory_order_relaxed);
}

// ---- The audio thread ----
std::uint32_t HostEngine::PumpAudio(float* out, std::uint32_t frames, std::int64_t now) noexcept {
    const unsigned channels = options_.audio_channels;
    if (!out || !frames) return 0;
    if (!open_ || !audio_) { std::memset(out, 0, static_cast<std::size_t>(frames) * (channels ? channels : 1) * sizeof(float)); return 0; }
    const auto request = flush_request_.load(std::memory_order_acquire);
    if (request != pump_epoch_) {
        // A seek: what the ring holds belongs to the epoch before. Dropped; the stream starts again at the new origin.
        (void)ring_.Discard(); stretch_.Reset(); dsp_.Reset(); clock_.Invalidate(); anchored_ = false; pump_epoch_ = request;
        flush_ack_.store(request, std::memory_order_release);
    }
    const std::int64_t origin = ring_origin_.load(std::memory_order_relaxed);
    if (paused_.load(std::memory_order_relaxed)) {
        std::memset(out, 0, static_cast<std::size_t>(frames) * channels * sizeof(float));
        if (anchored_) { clock_.Invalidate(); anchored_ = false; }
        pumped_ += frames; return 0;
    }
    const float speed = speed_.load(std::memory_order_relaxed);
    if (stretch_.Speed() != speed) (void)stretch_.SetSpeed(speed);
    dsp_.Apply(controls_); gain_.Apply(controls_);
    const std::uint64_t first = stretch_.Pulled();
    std::uint32_t produced = 0;
    while (produced < frames) {
        if (const unsigned available = stretch_.Available()) {
            produced += stretch_.Pull(out + static_cast<std::size_t>(produced) * channels, (std::min)(available, frames - produced));
            continue;
        }
        const unsigned want = (std::min)(stretch_.Room(), kChunk);
        if (!want) break;
        const auto got = ring_.ReadInto({reinterpret_cast<std::byte*>(pump_), static_cast<std::size_t>(want) * channels * sizeof(float)});
        if (!got) break; // Starved.
        if (locate_in_pump_) locator_.Process(pump_, got, -1); // The programme as heard, before the DSP moves it.
        dsp_.Process(pump_, got);
        (void)stretch_.Push(pump_, got);
    }
    if (produced < frames) {
        std::memset(out + static_cast<std::size_t>(produced) * channels, 0, static_cast<std::size_t>(frames - produced) * channels * sizeof(float));
        if (anchored_) underruns_.fetch_add(frames - produced, std::memory_order_relaxed);
        anchored_ = false; // Silence put in: the clock starts again from the next programme frame.
    }
    gain_.Process(out, frames);
    if (produced) {
        // The media time of this call's first frame: the stretcher's input position for it, from the epoch's origin.
        const auto& change = stretch_.LastChange();
        const double input = change.input + (static_cast<double>(first) - static_cast<double>(change.output)) * change.speed;
        const auto media = origin + static_cast<std::int64_t>(input * static_cast<double>(kSecond) / options_.audio_rate);
        const platform::AudioClockSample sample{pumped_, options_.audio_rate, now, true, generation_clock_};
        const float in_force = stretch_.SpeedInForce();
        if (!anchored_ || in_force != anchored_speed_) {
            anchored_ = clock_.Reanchor(sample, media, pump_epoch_, in_force); anchored_speed_ = in_force;
        } else if (clock_.Observe(sample) == clock::ClockUpdate::NeedsAnchor) anchored_ = clock_.Reanchor(sample, media, pump_epoch_, in_force);
    }
    pumped_ += frames;
    if (analysis_) {
        speech_now_.store(analysis_->Speech(), std::memory_order_relaxed);
        const unsigned band = analysis_->Dominant();
        band_now_.store(band, std::memory_order_relaxed); band_db_now_.store(analysis_->BandDb(band), std::memory_order_relaxed);
    }
    return produced;
}

// ---- Voice ----
bool HostEngine::FeedVoice(const float* samples, std::uint32_t frames) noexcept {
    if (!open_ || !voice_on_ || !samples) return false;
    for (std::uint32_t at = 0; at < frames; at += 160) (void)voice_gate_.Process({samples + at, (std::min)(frames - at, 160u)});
    voice_frames_ += frames;
    return true;
}
void HostEngine::HandleWake(void* context, ai::WakeDecision decision, std::string_view text, const audio::Utterance&) noexcept {
    auto& self = *static_cast<HostEngine*>(context);
    if (decision == ai::WakeDecision::Listening) { self.Status("Listening\xe2\x80\xa6", 40000000); return; }
    auto* slot = self.voice_texts_ ? self.voice_texts_->AcquireWriteSlot() : nullptr;
    if (!slot) return;
    const auto n = static_cast<std::uint32_t>((std::min)(text.size(), sizeof(slot->text)));
    std::memcpy(slot->text, text.data(), n); slot->bytes = n;
    (void)self.voice_texts_->CommitWriteSlot();
}
void HostEngine::DrainVoice() noexcept {
    if (!voice_texts_) return;
    while (const auto* t = voice_texts_->PeekReadSlot()) {
        VoiceText copy = *t; (void)voice_texts_->ReleaseReadSlot();
        (void)Command({copy.text, copy.bytes});
    }
}
bool HostEngine::Hear(std::string_view utf8) noexcept {
    audio::Utterance utterance{};
    utterance.onset_frame = utterance.end_frame = static_cast<std::uint64_t>((std::max)(MediaNow(last_now_), std::int64_t{0}) / 625);
    std::string_view command;
    const auto decision = text_gate_.Decide(utf8, utterance, command);
    if (decision == ai::WakeDecision::Ignored) return false;
    if (decision == ai::WakeDecision::Listening) { Status("Listening\xe2\x80\xa6", 40000000); return true; }
    (void)Command(command);
    return true;
}
bool HostEngine::SetWake(bool required, bool push_to_talk, const char* word) noexcept {
    if (!text_gate_.SetWord(word ? word : "")) return false;
    text_gate_.SetPolicy(required, push_to_talk);
    if (voice_on_) { (void)wake_gate_.SetWord(word ? word : ""); wake_gate_.SetPolicy(required, push_to_talk); }
    return true;
}
bool HostEngine::PollFrame(FrameSignature& out) noexcept {
    if (!signatures_) return false;
    const auto* s = signatures_->PeekReadSlot();
    if (!s) return false;
    out = *s; (void)signatures_->ReleaseReadSlot();
    return true;
}

// ---- Commands ----
ai::MediaBounds HostEngine::Bounds() const noexcept {
    ai::MediaBounds b;
    b.duration_100ns = duration_; b.position_100ns = (std::clamp)(MediaNow(last_now_), std::int64_t{0}, duration_ ? duration_ : INT64_MAX);
    b.volume = controls_.volume.load(std::memory_order_relaxed); b.muted = controls_.muted.load(std::memory_order_relaxed);
    b.paused = paused_.load(std::memory_order_relaxed); b.seekable = parser_.Seekable();
    b.subtitles = !Subs().Cues().empty(); b.audio_tracks = track_count_; b.subtitle_tracks = b.subtitles ? Subs().Channels() : 0;
    b.chapters = (options_.modules & kModuleChapters) ? parser_.ChapterCount() : 0;
    const auto query = conversation_.Query();
    b.matches = query.empty() || !Search().Ready() ? 0 : Search().Count(query);
    b.cure = conversation_.CurePending();
    return b;
}
ai::Intent HostEngine::Command(std::string_view utf8) noexcept { return dispatcher_.Dispatch(utf8, Bounds()); }
ai::IntentStatus HostEngine::Control(HostControl op, double value) noexcept {
    const auto bounds = Bounds();
    if ((op == HostControl::kSeek || op == HostControl::kSeekRelative) && !bounds.seekable) return ai::IntentStatus::Unavailable;
    if (op == HostControl::kAudioTrack) { // API 17: a playable audio track by its id.
        for (unsigned i = 0; i < catalogue_count_; ++i)
            if (catalogue_[i].kind == demux::TrackKind::Audio && catalogue_[i].id == static_cast<std::uint32_t>(value) && catalogue_[i].audio_slot != 0xff) {
                if (!SelectAudio(catalogue_[i].audio_slot)) return ai::IntentStatus::Rejected;
                active_audio_.store(catalogue_[i].id, std::memory_order_release);
                return ai::IntentStatus::Ok;
            }
        return ai::IntentStatus::NotFound;
    }
    if (op == HostControl::kSubtitleTrack) { // API 17: -1 none, 0 the sidecar, else a subtitle track's id.
        if (value < 0) { subtitles_shown_ = false; PublishSubtitles(); return ai::IntentStatus::Ok; }
        for (unsigned i = 0; i < catalogue_count_; ++i)
            if (catalogue_[i].kind == demux::TrackKind::Subtitle && catalogue_[i].id == static_cast<std::uint32_t>(value) && catalogue_[i].channel >= 0 && catalogue_[i].selectable) {
                Subs().Show(1u << catalogue_[i].channel); subtitles_shown_ = true; PublishSubtitles();
                return ai::IntentStatus::Ok;
            }
        return ai::IntentStatus::NotFound;
    }
    ai::Intent intent{};
    if (!ControlIntent(op, value, bounds.position_100ns, bounds.duration_100ns, intent)) return ai::IntentStatus::Unavailable;
    return Execute(intent);
}
ai::IntentStatus HostEngine::HandleIntent(void* context, const ai::Intent& intent) noexcept { return static_cast<HostEngine*>(context)->Execute(intent); }
bool HostEngine::Pause(bool pause) noexcept {
    if (pause == paused_.load(std::memory_order_relaxed)) return true;
    if (pause) { paused_at_ = MediaNow(last_now_); paused_.store(true, std::memory_order_relaxed); }
    else { landing_ = paused_at_; wall_media_ = paused_at_; wall_now_ = last_now_; paused_.store(false, std::memory_order_relaxed); }
    return true;
}
bool HostEngine::Seek(std::int64_t target) noexcept {
    if (!parser_.Seekable()) return false;
    target = (std::clamp)(target, std::int64_t{0}, duration_ > 0 ? duration_ - 1 : std::int64_t{0});
    const auto from = MediaNow(last_now_);
    if (!parser_.Seek(target, epoch_ + 1, 0)) return false;
    phase_ = Phase::Draining;
    ++epoch_;
    if (have_held_) { (void)parser_.ReturnPacket(held_); have_held_ = false; }
    platform::SurfaceHandle released[16]; unsigned count = 0;
    if (video_index_ >= 0) { if (video_.codec == Codec::H264) (void)h264_.Reset(released, count); else (void)hevc_.Reset(released, count); }
    for (auto& p : pictures_) { GpuForget(p, true, true); p.queued = false; p.referenced = false; }
    if (gpu_) (void)options_.gpu->Restart(epoch_); // API 14: the GPU's decoder takes the new epoch's packets.
    flush_unit_ = audio_flush_unit_ = true; parsed_ = false;
    landing_ = target; landing_due_ = true; paused_at_ = target; wall_media_ = target; wall_now_ = last_now_;
    end_ = 0;
    if (audio_) {
        audio_trim_ = target; written_end_ = target;
        if (auto* c = tracks_[track_].converter) c->Reset();
        ring_origin_.store(target, std::memory_order_relaxed);
        flush_request_.store(epoch_, std::memory_order_release);
    }
    cuts_.Reset(); codes_valid_ = false; grid_mean_ = -1;
    if (tracker_.Active()) { tracker_.Stop(); track_pending_ = false; Status("Tracking off \xc2\xb7 the picture changed"); }
    retarget_ = false; target_id_ = 0;
    conversation_.NoteJump(from);
    return true;
}
bool HostEngine::SelectAudio(std::uint32_t slot) noexcept {
    if (slot >= track_count_ || slot == track_) return slot == track_;
    // The new track from where the ring's sound ends: the switch is heard without a gap or a repeat.
    const auto at = written_end_;
    if (!parser_.SwitchTrack(tracks_[track_].index, tracks_[slot].index, at, 0)) return false;
    track_ = slot; audio_trim_ = at; audio_flush_unit_ = true;
    active_audio_.store(tracks_[slot].id, std::memory_order_release);
    if (auto* c = tracks_[slot].converter) c->Reset();
    if (have_held_ && held_.stream_id != video_id_) { (void)parser_.ReturnPacket(held_); have_held_ = false; }
    return true;
}
ai::IntentStatus HostEngine::Cure(ai::IntentKind kind, bool undo) noexcept {
    ui::PictureAudioCures::Settings s{};
    for (unsigned b = 0; b < 10; ++b) s.audio.eq_db[b] = controls_.eq_db[b].load(std::memory_order_relaxed);
    s.audio.clarity = controls_.clarity.load(); s.audio.notch_killer = controls_.notch_killer.load(); s.audio.speech_ducking = controls_.speech_ducking.load();
    s.audio.iso_loudness = controls_.iso_loudness.load(); s.audio.audio_zoom = controls_.audio_zoom.load();
    if (undo ? !cures_.Undo(s) : !cures_.Apply(kind, s)) return undo ? ai::IntentStatus::NoContext : ai::IntentStatus::Rejected;
    for (unsigned b = 0; b < 10; ++b) controls_.eq_db[b].store(s.audio.eq_db[b], std::memory_order_relaxed);
    controls_.clarity.store(s.audio.clarity); controls_.notch_killer.store(s.audio.notch_killer != 0); controls_.speech_ducking.store(s.audio.speech_ducking != 0);
    controls_.iso_loudness.store(s.audio.iso_loudness != 0); controls_.audio_zoom.store(s.audio.audio_zoom != 0);
    Status(undo ? "Settings put back" : ui::PictureAudioCures::Describe(kind));
    return ai::IntentStatus::Ok;
}
ai::IntentStatus HostEngine::Track(bool on) noexcept {
    if (!on) {
        const bool was = tracker_.Active() || track_pending_; tracker_.Stop(); track_pending_ = retarget_ = false; target_id_ = 0;
        if (was) Status("Tracking off");
        return was ? ai::IntentStatus::Ok : ai::IntentStatus::NoContext;
    }
    if (!grid_) { Status("Tracking \xc2\xb7 no picture to follow here"); return ai::IntentStatus::Unavailable; }
    track_pending_ = true; retarget_ = false; // Locked on the next picture shown, at the view's centre.
    // The object there is the target from now (2026-11-01): "next target" right after moves on from it, without waiting for the lock.
    target_id_ = ObjectAt(view_x_, view_y_);
    return ai::IntentStatus::Ok;
}
ai::IntentStatus HostEngine::Execute(const ai::Intent& intent) noexcept {
    using ai::IntentKind; using ai::IntentStatus;
    const auto done = [](bool ok) { return ok ? IntentStatus::Ok : IntentStatus::Rejected; };
    const auto position = MediaNow(last_now_);
    switch (intent.kind) {
    case IntentKind::Play: return done(Pause(false));
    case IntentKind::Pause: return done(Pause(true));
    case IntentKind::VolumeUp: case IntentKind::VolumeSet: controls_.volume.store(intent.volume); controls_.muted.store(false); return IntentStatus::Ok;
    case IntentKind::VolumeDown: controls_.volume.store(intent.volume); return IntentStatus::Ok;
    case IntentKind::Mute: controls_.muted.store(true); return IntentStatus::Ok;
    case IntentKind::Unmute: controls_.muted.store(false); return IntentStatus::Ok;
    case IntentKind::SeekRelative: case IntentKind::SeekAbsolute: return done(Seek(intent.target_100ns));
    case IntentKind::FindSubtitle: {
        subtitle::SearchMatch m{};
        if (!Search().Ready() || !Search().Find(intent.query, intent.match, m)) return IntentStatus::NotFound;
        return done(Seek(m.start));
    }
    case IntentKind::ReplayCue: {
        std::int64_t start = -1;
        for (const auto& c : Subs().Cues()) if (c.start <= position && position < c.end + 3 * kSecond && c.start > start) start = c.start;
        return start < 0 ? IntentStatus::NotFound : done(Seek(start));
    }
    case IntentKind::SelectAudio: case IntentKind::SelectLanguage:
        if (intent.track == ai::TrackChoice::Next) return track_count_ > 1 ? done(SelectAudio((track_ + 1) % track_count_)) : IntentStatus::NotFound;
        for (unsigned k = 0; k < track_count_; ++k)
            if (std::strcmp(tracks_[k].language, intent.language) == 0) {
                if (!SelectAudio(k)) return IntentStatus::Rejected;
                char text[64]; const char* name = ai::LanguageName(intent.language);
                std::size_t n = 0; for (const char* c = "Audio \xc2\xb7 "; *c; ++c) text[n++] = *c;
                for (const char* c = name ? name : intent.language; *c && n + 1 < sizeof(text); ++c) text[n++] = *c;
                text[n] = 0; Status(text);
                return IntentStatus::Ok;
            }
        return IntentStatus::NotFound;
    case IntentKind::SelectSubtitles:
        if (Subs().Cues().empty()) return IntentStatus::NotFound;
        subtitles_shown_ = intent.track == ai::TrackChoice::Off ? false : intent.track == ai::TrackChoice::On ? true : !subtitles_shown_; PublishSubtitles();
        return IntentStatus::Ok;
    case IntentKind::NextChapter: case IntentKind::PreviousChapter: {
        const unsigned n = (options_.modules & kModuleChapters) ? parser_.ChapterCount() : 0;
        std::int64_t target = -1;
        for (unsigned i = 0; i < n; ++i) {
            const auto start = parser_.ChapterAt(i).start_100ns;
            if (intent.kind == IntentKind::NextChapter && start > position + kSecond / 2 && (target < 0 || start < target)) target = start;
            if (intent.kind == IntentKind::PreviousChapter && start < position - 2 * kSecond && start > target) target = start;
        }
        if (intent.kind == IntentKind::PreviousChapter && target < 0 && n) target = 0;
        return target < 0 ? IntentStatus::OutOfRange : done(Seek(target));
    }
    case IntentKind::SpeedUp: case IntentKind::SpeedDown: {
        const float now_speed = speed_.load();
        float next = now_speed;
        if (intent.kind == IntentKind::SpeedUp) { for (float s : kSpeeds) if (s > now_speed + 1e-3f) { next = s; break; } }
        else for (float s : kSpeeds) if (s < now_speed - 1e-3f) next = s;
        if (next == now_speed) return IntentStatus::OutOfRange;
        if (!audio_) { wall_media_ = MediaNow(last_now_); wall_now_ = last_now_; }
        speed_.store(next); return IntentStatus::Ok;
    }
    case IntentKind::SpeedSet:
        if (intent.speed < audio::TimeStretcher::kMinSpeed || intent.speed > audio::TimeStretcher::kMaxSpeed) return IntentStatus::OutOfRange;
        if (!audio_) { wall_media_ = MediaNow(last_now_); wall_now_ = last_now_; }
        speed_.store(intent.speed); return IntentStatus::Ok;
    case IntentKind::ZoomIn: case IntentKind::ZoomOut: case IntentKind::ZoomReset:
        if (video_index_ < 0) return IntentStatus::Unavailable;
        auto_crop_ = false; cropped_changes_ = 0; // The viewer's own zoom: auto-crop stops.
        zoom_ = intent.kind == IntentKind::ZoomReset ? 1.0f : std::clamp(intent.kind == IntentKind::ZoomIn ? zoom_ * 2 : zoom_ / 2, 1.0f, 8.0f);
        if (zoom_ == 1.0f) { view_x_ = view_y_ = 0.5f; }
        else { const float half = 0.5f / zoom_; view_x_ = std::clamp(view_x_, half, 1 - half); view_y_ = std::clamp(view_y_, half, 1 - half); }
        SyncFocus();
        return IntentStatus::Ok;
    case IntentKind::NightOn: case IntentKind::NightOff:
        if (!audio_) return IntentStatus::Unavailable;
        controls_.night.store(intent.kind == IntentKind::NightOn); return IntentStatus::Ok;
    case IntentKind::AudioZoomOn: case IntentKind::AudioZoomOff:
        if (!audio_) return IntentStatus::Unavailable;
        SyncFocus(); controls_.audio_zoom.store(intent.kind == IntentKind::AudioZoomOn); return IntentStatus::Ok;
    case IntentKind::CureNoise: case IntentKind::CureMuffled: return audio_ ? Cure(intent.kind, false) : IntentStatus::Unavailable;
    case IntentKind::UndoCure: return Cure(intent.kind, true);
    case IntentKind::TrackTarget: return Track(true);
    case IntentKind::StopTracking: return Track(false);
    case IntentKind::NextTarget: return TrackObject(0);
    case IntentKind::Snapshot: // The SDK writes it (media_engine_save_snapshot's way) when the command returns.
        if (video_index_ < 0) return IntentStatus::Unavailable;
        snapshot_requested_ = true; Status("Snapshot");
        return IntentStatus::Ok;
    case IntentKind::AudioSaccadeOn: return SetSaccade(true);
    case IntentKind::AudioSaccadeOff: return SetSaccade(false);
    // Rotation (API 6): a quarter turn, a degree of level (auto-leveling stops), straight; auto-leveling; black bars.
    case IntentKind::RotateVideo: case IntentKind::RotateVideoBack: {
        if (video_index_ < 0) return IntentStatus::Unavailable;
        orientation_.turns = (orientation_.turns + (intent.kind == IntentKind::RotateVideo ? 1u : 3u)) % 4;
        static constexpr const char* kCues[]{"Rotation \xc2\xb7 upright", "Rotation \xc2\xb7 90\xc2\xb0", "Rotation \xc2\xb7 180\xc2\xb0", "Rotation \xc2\xb7 270\xc2\xb0"};
        Status(kCues[orientation_.QuarterTurns()]);
        return IntentStatus::Ok;
    }
    case IntentKind::LevelLeft: case IntentKind::LevelRight:
        return SetOrientation(orientation_.turns, std::clamp(orientation_.level + (intent.kind == IntentKind::LevelRight ? 1.0f : -1.0f),
                                                             -video::HorizonLevel::kMaxTilt, video::HorizonLevel::kMaxTilt));
    case IntentKind::LevelHorizon:
        if (video_index_ < 0) return IntentStatus::Unavailable;
        if (orientation_.level == 0 && !auto_level_) return IntentStatus::NoContext;
        return SetOrientation(orientation_.turns, 0);
    case IntentKind::AutoLevel: case IntentKind::AutoLevelOff: return SetAutoLevel(intent.kind == IntentKind::AutoLevel);
    case IntentKind::Stabilize: case IntentKind::StabilizeOff: return SetStabilization(intent.kind == IntentKind::Stabilize);
    case IntentKind::CropBars: case IntentKind::CropBarsOff: return SetAutoCrop(intent.kind == IntentKind::CropBars);
    default: break; // Picture controls, loops, bookmarks, files, statistics: the windowed engine's.
    }
    return IntentStatus::Unavailable;
}
HostState HostEngine::State() const noexcept {
    HostState s;
    s.position_100ns = MediaNow(last_now_); s.duration_100ns = duration_; s.epoch = epoch_;
    s.paused = paused_.load(); s.muted = controls_.muted.load(); s.saccade = saccade_; s.volume = controls_.volume.load(); s.speed = speed_.load();
    s.tracking = tracker_.Status().state == video::TrackState::Tracking ? 1u : tracker_.Status().state == video::TrackState::Lost ? 2u : 0u;
    s.view_zoom = zoom_; s.view_x = view_x_; s.view_y = view_y_;
    if (audio_) { s.audio_track = tracks_[track_].id; std::memcpy(s.audio_language, tracks_[track_].language, 4); }
    s.audio_tracks = track_count_;
    s.presented = presented_; s.dropped = dropped_; s.underruns = underruns_.load();
    s.quarter_turns = orientation_.QuarterTurns(); s.mirrored = orientation_.Mirrored(); s.auto_level = auto_level_; s.auto_crop = auto_crop_;
    s.level_degrees = orientation_.level;
    return s;
}
}
