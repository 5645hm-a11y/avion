#include "media/subtitle/EmbeddedText.hpp"
#include "media/text/Utf8.hpp"
#include <algorithm>
#include <string_view>

namespace media::subtitle {
namespace {
std::size_t Stamp(char* out, std::int64_t t) noexcept {
    const auto ms = static_cast<std::uint64_t>(t < 0 ? 0 : t) / 10000;
    const std::uint64_t parts[4]{ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000};
    std::size_t n = 0;
    const auto digits = [&](std::uint64_t v, unsigned count) {
        char d[20]; unsigned k = 0;
        do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 20);
        while (k < count) d[k++] = '0';
        while (k) out[n++] = d[--k];
    };
    digits(parts[0], 2); out[n++] = ':'; digits(parts[1], 2); out[n++] = ':'; digits(parts[2], 2); out[n++] = '.'; digits(parts[3], 3);
    return n;
}
}

std::size_t WebVttHeader(char* out, std::size_t capacity) noexcept {
    constexpr std::string_view header = "WEBVTT\n\n";
    if (capacity < header.size()) return 0;
    for (std::size_t i = 0; i < header.size(); ++i) out[i] = header[i];
    return header.size();
}

std::size_t AppendCue(char* out, std::size_t capacity, EmbeddedFormat format, std::int64_t start, std::int64_t end, std::uint8_t* sample,
                      std::size_t bytes) noexcept {
    if (!sample || end <= start || end <= 0) return 0;
    const std::uint8_t* text = sample; std::size_t length = bytes;
    if (format == EmbeddedFormat::Tx3g) {
        if (bytes < 3) return 0;
        length = (std::min)(static_cast<std::size_t>(sample[0] << 8 | sample[1]), bytes - 2); text = sample + 2;
    } else if (format == EmbeddedFormat::Ssa) {
        std::size_t k = 0; unsigned commas = 0;
        while (k < bytes && commas < 8) if (sample[k++] == ',') ++commas;
        if (commas < 8) return 0;
        std::size_t kept = 0; bool tag = false;
        for (; k < bytes; ++k) {
            const auto c = sample[k];
            if (tag) { tag = c != '}'; continue; }
            if (c == '{') { tag = true; continue; }
            if (c == '\\' && k + 1 < bytes && (sample[k + 1] == 'N' || sample[k + 1] == 'n')) { sample[kept++] = '\n'; ++k; continue; }
            if (c == '\\' && k + 1 < bytes && sample[k + 1] == 'h') { sample[kept++] = ' '; ++k; continue; }
            sample[kept++] = c;
        }
        length = kept;
    }
    if (!length || capacity < CueBound(length)) return 0;
    // The text first (one line per line: carriage returns dropped, no empty line, no edge blanks), after room for the timing.
    char* body = out + 40; std::size_t t = 0;
    const auto emit = [&](std::uint32_t c) {
        if (c == '\r' || c == 0) return;
        if (c == '\n' && (t == 0 || body[t - 1] == '\n')) return;
        t += text::Encode(c, body + t);
    };
    if (format == EmbeddedFormat::Tx3g && length >= 2 && text[0] == 0xfe && text[1] == 0xff) { // UTF-16BE.
        for (std::size_t k = 2; k + 1 < length; k += 2) {
            std::uint32_t c = static_cast<std::uint32_t>(text[k] << 8 | text[k + 1]);
            if (c >= 0xd800 && c < 0xdc00 && k + 3 < length) {
                const std::uint32_t low = static_cast<std::uint32_t>(text[k + 2] << 8 | text[k + 3]);
                if (low >= 0xdc00 && low < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); k += 2; } else c = 0xfffd;
            } else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
            emit(c);
        }
    } else {
        std::string_view rest{reinterpret_cast<const char*>(text), length}; std::uint32_t c = 0;
        while (!rest.empty()) { if (!text::Utf8(rest, c)) { rest.remove_prefix(1); c = '?'; } emit(c); }
    }
    while (t && (body[t - 1] == '\n' || body[t - 1] == ' ')) --t;
    if (!t) return 0;
    // The timing line, then the text moved after it.
    char timing[32]; std::size_t n = Stamp(timing, start);
    timing[n++] = ' '; timing[n++] = '-'; timing[n++] = '-'; timing[n++] = '>'; timing[n++] = ' ';
    n += Stamp(timing + n, end); timing[n++] = '\n';
    for (std::size_t i = 0; i < t; ++i) out[n + i] = body[i]; // n < 40: forward copy never overwrites unread text.
    for (std::size_t i = 0; i < n; ++i) out[i] = timing[i];
    out[n + t] = '\n'; out[n + t + 1] = '\n';
    return n + t + 2;
}
} // namespace media::subtitle
