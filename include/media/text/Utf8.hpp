#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace media::text {
// Decode one strict UTF-8 scalar, advancing the view. Reject overlongs, surrogates and truncation.
bool Utf8(std::string_view& text, std::uint32_t& scalar) noexcept;
// Encode one scalar (at most 4 bytes); returns the byte count.
unsigned Encode(std::uint32_t scalar, char* out) noexcept;

// Text as a person would type it to find it again. Every form, per scalar:
// - any white space (line breaks, tabs, no-break and ideographic spaces) is one space, and a run is one space;
// - invisible format controls are dropped (soft hyphen, zero-width characters, bidi marks, embeddings and isolates).
// kFold additionally ignores what a typed query usually lacks:
// - letter case: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, Armenian and fullwidth Latin;
// - Hebrew points and cantillation, Arabic harakat and tatweel; alef with madda or hamza is a bare alef.
// kMarkup reads the input as subtitle cue text: <tags> are dropped and the WebVTT entities decoded.
// The output is never longer than the input, and the forms nest: if a query occurs in a text, the query's folded
// form occurs in the text's folded form.
enum : unsigned { kFold = 1, kMarkup = 2 };
constexpr std::size_t kInvalid = ~std::size_t{0};
// Returns the bytes written, or kInvalid for ill-formed UTF-8 or a capacity below the input size.
std::size_t Normalize(std::string_view text, char* out, std::size_t capacity, unsigned flags) noexcept;
// One scalar of the folded form: ' ' for white space, 0 when dropped, else the case-folded scalar.
std::uint32_t FoldScalar(std::uint32_t scalar, bool fold) noexcept;
}
