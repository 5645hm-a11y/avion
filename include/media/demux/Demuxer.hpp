#pragma once
// The engines' demuxer (2026-11-05, SDK API 17; docs/matroska.md): one interface over ISO BMFF (IsobmffParser: .mp4, .m4a, .mov,
// fragmented MP4) and Matroska (MatroskaDemuxer: .mkv, .webm), chosen by the file's first bytes (an EBML header is Matroska,
// anything else is handed to IsobmffParser, which refuses what is not ISO BMFF). Both live inside it: no allocation, no virtual
// call; each call forwards to the one the file is. Until the first Step has read those bytes the file has no metadata.
#include "media/demux/IsobmffParser.hpp"
#include "media/demux/MatroskaDemuxer.hpp"

namespace media::demux {
enum class ContainerFormat : std::uint8_t { Unknown, Isobmff, Matroska };

class Demuxer final {
public:
    Demuxer() noexcept = default;
    ~Demuxer() noexcept;
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;
    // Keeps the arena, the source and the limits; the chosen demuxer takes its tables from the arena at the first Step (while
    // the engine is still preparing: the arena must not be sealed before MetadataReady()).
    [[nodiscard]] bool Initialize(core::LinearMemoryArena& arena, platform::IBlockSource& source, ParserLimits limits = {}) noexcept;
    template <std::size_t Capacity>
    [[nodiscard]] ParseStatus Step(core::SpscRingBuffer<core::MediaPacket, Capacity>& output) noexcept {
        if (format_ == ContainerFormat::Unknown) { const auto status = Sniff(); if (format_ == ContainerFormat::Unknown) return status; }
        if (format_ == ContainerFormat::Matroska)
            return mkv_.StepImpl(&output, [](void* context, const core::MediaPacket& packet) noexcept {
                auto& queue = *static_cast<core::SpscRingBuffer<core::MediaPacket, Capacity>*>(context);
                auto* slot = queue.AcquireWriteSlot();
                if (slot == nullptr) return false;
                *slot = packet;
                return queue.CommitWriteSlot();
            });
        return mp4_.Step(output);
    }
    [[nodiscard]] ContainerFormat Format() const noexcept { return format_; }
    [[nodiscard]] const TrackInfo* Track(std::uint32_t index) const noexcept { return Mkv() ? mkv_.Track(index) : Mp4() ? mp4_.Track(index) : nullptr; }
    [[nodiscard]] std::uint32_t TrackCount() const noexcept { return Mkv() ? mkv_.TrackCount() : Mp4() ? mp4_.TrackCount() : 0; }
    [[nodiscard]] bool MetadataReady() const noexcept { return Mkv() ? mkv_.MetadataReady() : Mp4() && mp4_.MetadataReady(); }
    [[nodiscard]] ParserDiagnostic Diagnostic() const noexcept { return Mkv() ? mkv_.Diagnostic() : Mp4() ? mp4_.Diagnostic() : diagnostic_; }
    [[nodiscard]] bool ReturnPacket(const core::MediaPacket& p) noexcept { return Mkv() ? mkv_.ReturnPacket(p) : Mp4() && mp4_.ReturnPacket(p); }
    [[nodiscard]] bool Seekable() const noexcept { return Mkv() ? mkv_.Seekable() : Mp4() && mp4_.Seekable(); }
    // Sample lookups (SampleAt, SyncSample, ...) need the complete index: an MP4's tables; a Matroska file's whole scan (a seek on its
    // Cues before the scan finished leaves it incomplete).
    [[nodiscard]] bool Indexed() const noexcept { return Mkv() ? mkv_.Indexed() : Mp4() && mp4_.Seekable(); }
    [[nodiscard]] bool Seek(std::int64_t target, std::uint32_t epoch, std::uint32_t preroll = 0) noexcept { return Mkv() ? mkv_.Seek(target, epoch, preroll) : Mp4() && mp4_.Seek(target, epoch, preroll); }
    [[nodiscard]] bool Reposition(std::int64_t target, std::uint32_t preroll = 0) noexcept { return Mkv() ? mkv_.Reposition(target, preroll) : Mp4() && mp4_.Reposition(target, preroll); }
    [[nodiscard]] bool Continue(std::uint32_t video, std::int64_t target, std::uint32_t epoch, std::uint32_t preroll, std::uint32_t& resumed) noexcept {
        resumed = 0;
        return Mkv() ? mkv_.Continue(video, target, epoch, preroll, resumed) : Mp4() && mp4_.Continue(video, target, epoch, preroll, resumed);
    }
    [[nodiscard]] bool SeekOthers(std::uint32_t keep, std::int64_t target, std::uint32_t epoch, std::uint32_t preroll) noexcept {
        return Mkv() ? mkv_.SeekOthers(keep, target, epoch, preroll) : Mp4() && mp4_.SeekOthers(keep, target, epoch, preroll);
    }
    [[nodiscard]] std::uint32_t SyncSample(std::uint32_t index, std::int64_t target) const noexcept { return Mkv() ? mkv_.SyncSample(index, target) : Mp4() ? mp4_.SyncSample(index, target) : UINT32_MAX; }
    [[nodiscard]] std::uint32_t DecodeDistance(std::uint32_t index, std::uint32_t from, std::int64_t target, std::uint32_t limit) noexcept {
        return Mkv() ? mkv_.DecodeDistance(index, from, target, limit) : Mp4() ? mp4_.DecodeDistance(index, from, target, limit) : (std::min)(limit, UINT32_MAX - 1) + 1;
    }
    [[nodiscard]] unsigned ChapterCount() const noexcept { return Mkv() ? mkv_.ChapterCount() : Mp4() ? mp4_.ChapterCount() : 0; }
    [[nodiscard]] const ChapterEntry& ChapterAt(unsigned i) const noexcept { return Mkv() ? mkv_.ChapterAt(i) : mp4_.ChapterAt(i); }
    [[nodiscard]] std::uint32_t SyncCount(std::uint32_t index) const noexcept { return Mkv() ? mkv_.SyncCount(index) : Mp4() ? mp4_.SyncCount(index) : UINT32_MAX; }
    [[nodiscard]] std::uint32_t SyncAt(std::uint32_t index, std::uint32_t i) const noexcept { return Mkv() ? mkv_.SyncAt(index, i) : Mp4() ? mp4_.SyncAt(index, i) : UINT32_MAX; }
    [[nodiscard]] std::uint32_t Epoch() const noexcept { return Mkv() ? mkv_.Epoch() : Mp4() ? mp4_.Epoch() : limits_.playback_epoch; }
    // ISO BMFF fragments (moof) only: Matroska is complete once its scan is (Seekable()).
    [[nodiscard]] bool Fragmented() const noexcept { return Mp4() && mp4_.Fragmented(); }
    [[nodiscard]] std::uint64_t IndexBytes() const noexcept { return Mkv() ? mkv_.IndexBytes() : Mp4() ? mp4_.IndexBytes() : 0; }
    [[nodiscard]] std::uint32_t SkippedTracks() const noexcept { return Mkv() ? mkv_.SkippedTracks() : Mp4() ? mp4_.SkippedTracks() : 0; }
    [[nodiscard]] bool SetEmission(std::uint32_t index, bool emit) noexcept { return Mkv() ? mkv_.SetEmission(index, emit) : Mp4() && mp4_.SetEmission(index, emit); }
    [[nodiscard]] bool Emitting(std::uint32_t index) const noexcept { return Mkv() ? mkv_.Emitting(index) : Mp4() && mp4_.Emitting(index); }
    [[nodiscard]] bool SwitchTrack(std::uint32_t from, std::uint32_t to, std::int64_t at, std::uint32_t preroll = 0) noexcept {
        return Mkv() ? mkv_.SwitchTrack(from, to, at, preroll) : Mp4() && mp4_.SwitchTrack(from, to, at, preroll);
    }
    [[nodiscard]] bool SampleAt(std::uint32_t index, std::uint32_t sample, SampleRecord& out) const noexcept {
        return Mkv() ? mkv_.SampleAt(index, sample, out) : Mp4() && mp4_.SampleAt(index, sample, out);
    }
    [[nodiscard]] const MatroskaDemuxer& Matroska() const noexcept { return mkv_; }
    [[nodiscard]] static const char* Name(ParseError error) noexcept { return IsobmffParser::Name(error); }

private:
    [[nodiscard]] bool Mkv() const noexcept { return format_ == ContainerFormat::Matroska; }
    [[nodiscard]] bool Mp4() const noexcept { return format_ == ContainerFormat::Isobmff; }
    [[nodiscard]] ParseStatus Sniff() noexcept; // Reads the first bytes, then initialises the demuxer they call for.
    core::LinearMemoryArena* arena_ = nullptr;
    platform::IBlockSource* source_ = nullptr;
    ParserLimits limits_{};
    ContainerFormat format_ = ContainerFormat::Unknown;
    platform::BlockToken token_{};
    bool requested_ = false, failed_ = false;
    ParserDiagnostic diagnostic_{ParseError::NotInitialized, 0, 0};
    IsobmffParser mp4_;
    MatroskaDemuxer mkv_;
};
} // namespace media::demux
