#include "media/demux/Demuxer.hpp"

#include <algorithm>

namespace media::demux {
Demuxer::~Demuxer() noexcept {
    if (requested_ && source_) (void)source_->Release(token_);
}

bool Demuxer::Initialize(core::LinearMemoryArena& arena, platform::IBlockSource& source, ParserLimits limits) noexcept {
    if (source_ != nullptr || arena.IsSealed() || limits.tracks == 0 || limits.tracks > 16) return false;
    arena_ = &arena; source_ = &source; limits_ = limits;
    diagnostic_ = {ParseError::None, 0, 0};
    return true;
}

ParseStatus Demuxer::Sniff() noexcept {
    if (failed_) return ParseStatus::Failed;
    if (!source_ || !arena_) { failed_ = true; diagnostic_ = {ParseError::NotInitialized, 0, 0}; return ParseStatus::Failed; }
    const auto size = source_->Size();
    bool ebml = false;
    if (size >= 4) {
        if (!requested_) {
            const auto status = source_->Request(0, static_cast<std::uint32_t>((std::min)(size, std::uint64_t{16})), token_);
            if (status == platform::IoStatus::WouldBlock) return ParseStatus::Backpressure;
            if (status != platform::IoStatus::Pending) { failed_ = true; diagnostic_ = {ParseError::IoFailure, 0, 0}; return ParseStatus::Failed; }
            requested_ = true;
        }
        platform::BlockView view{};
        const auto status = source_->Poll(token_, view);
        if (status == platform::IoStatus::Pending) return ParseStatus::NeedInput;
        const bool good = status == platform::IoStatus::Ready && view.data && view.file_offset == 0 && view.valid_bytes >= 4;
        if (good) {
            static constexpr unsigned char kEbml[4]{0x1A, 0x45, 0xDF, 0xA3};
            ebml = true;
            for (unsigned i = 0; i < 4; ++i) ebml = ebml && std::to_integer<unsigned char>(view.data[i]) == kEbml[i];
        }
        (void)source_->Release(token_); requested_ = false; token_ = {};
        if (!good) { failed_ = true; diagnostic_ = {ParseError::IoFailure, 0, 0}; return ParseStatus::Failed; }
    }
    // Anything that is not EBML goes to the ISO BMFF parser, which refuses what is not ISO BMFF either.
    const bool ok = ebml ? mkv_.Initialize(*arena_, *source_, limits_) : mp4_.Initialize(*arena_, *source_, limits_);
    if (!ok) { failed_ = true; diagnostic_ = {ParseError::LimitExceeded, 0, 0}; return ParseStatus::Failed; }
    format_ = ebml ? ContainerFormat::Matroska : ContainerFormat::Isobmff;
    return ParseStatus::Progress;
}
} // namespace media::demux
