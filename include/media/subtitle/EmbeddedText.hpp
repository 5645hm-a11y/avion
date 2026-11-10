#pragma once
// Embedded subtitle samples as WebVTT cues (2026-11-05, API 17), portable: what a demuxer's subtitle track holds, in the text the
// subtitle track (SubtitleTrack::Prepare) reads. No allocation: the caller's buffers.
// - Tx3g: 3GPP timed text (MP4): a 16-bit length and UTF-8 (or UTF-16 with a byte order mark); style boxes after it unused.
// - Text: the cue's UTF-8 itself (Matroska S_TEXT/UTF8, S_TEXT/WEBVTT).
// - Ssa: an ASS/SSA event without its times ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"): the text after
//   the eighth comma, its {...} override tags dropped, \N and \n line breaks, \h a space.
#include <cstddef>
#include <cstdint>

namespace media::subtitle {
enum class EmbeddedFormat : std::uint8_t { Tx3g, Text, Ssa };
// "WEBVTT\n\n": the text's start. Returns the bytes written (0 when it does not fit).
std::size_t WebVttHeader(char* out, std::size_t capacity) noexcept;
// One cue (`sample`, `bytes` of it; [start, end) in 100 ns) appended as "hh:mm:ss.mmm --> hh:mm:ss.mmm\ntext\n\n". `sample` is
// modified (SSA text is cleaned in place). Returns the bytes written: 0 for an empty cue, or when it does not fit.
std::size_t AppendCue(char* out, std::size_t capacity, EmbeddedFormat format, std::int64_t start, std::int64_t end, std::uint8_t* sample,
                      std::size_t bytes) noexcept;
// What a cue can take at most: its text (UTF-16 becomes up to 3/2 its bytes) and the timing line.
constexpr std::size_t CueBound(std::size_t bytes) noexcept { return bytes * 3 / 2 + 56; }
} // namespace media::subtitle
