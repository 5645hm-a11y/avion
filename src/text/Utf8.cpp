#include "media/text/Utf8.hpp"

namespace media::text {
bool Utf8(std::string_view& text, std::uint32_t& scalar) noexcept {
    if (text.empty()) return false;
    const auto lead = static_cast<unsigned char>(text.front());
    unsigned bytes = 0; std::uint32_t minimum = 0;
    if (lead < 0x80) { scalar = lead; bytes = 1; }
    else if (lead >= 0xc2 && lead <= 0xdf) { scalar = lead & 0x1fu; bytes = 2; minimum = 0x80; }
    else if (lead >= 0xe0 && lead <= 0xef) { scalar = lead & 0x0fu; bytes = 3; minimum = 0x800; }
    else if (lead >= 0xf0 && lead <= 0xf4) { scalar = lead & 7u; bytes = 4; minimum = 0x10000; }
    else return false;
    if (text.size() < bytes) return false;
    for (unsigned i = 1; i < bytes; ++i) {
        const auto c = static_cast<unsigned char>(text[i]); if ((c & 0xc0) != 0x80) return false;
        scalar = scalar * 64 + (c & 63u);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) || scalar == 0) return false;
    text.remove_prefix(bytes); return true;
}
unsigned Encode(std::uint32_t s, char* out) noexcept {
    if (s < 0x80) { out[0] = static_cast<char>(s); return 1; }
    if (s < 0x800) { out[0] = static_cast<char>(0xc0 | (s >> 6)); out[1] = static_cast<char>(0x80 | (s & 63)); return 2; }
    if (s < 0x10000) {
        out[0] = static_cast<char>(0xe0 | (s >> 12)); out[1] = static_cast<char>(0x80 | ((s >> 6) & 63)); out[2] = static_cast<char>(0x80 | (s & 63));
        return 3;
    }
    out[0] = static_cast<char>(0xf0 | (s >> 18)); out[1] = static_cast<char>(0x80 | ((s >> 12) & 63));
    out[2] = static_cast<char>(0x80 | ((s >> 6) & 63)); out[3] = static_cast<char>(0x80 | (s & 63));
    return 4;
}
std::uint32_t FoldScalar(std::uint32_t s, bool fold) noexcept {
    if (s < 0x80) {
        if (s == ' ' || (s >= 9 && s <= 13)) return ' ';
        return fold && s >= 'A' && s <= 'Z' ? s + 32 : s;
    }
    // White space and invisible format controls, in every form.
    if (s == 0xa0 || s == 0x1680 || (s >= 0x2000 && s <= 0x200a) || s == 0x2028 || s == 0x2029 || s == 0x202f || s == 0x205f || s == 0x3000) return ' ';
    if (s == 0xad || s == 0x61c || (s >= 0x200b && s <= 0x200f) || (s >= 0x202a && s <= 0x202e) || (s >= 0x2060 && s <= 0x2064) ||
        (s >= 0x2066 && s <= 0x2069) || s == 0xfeff) return 0;
    if (!fold) return s;
    if (s < 0x180) { // Latin-1 and Latin Extended-A.
        if (s >= 0xc0 && s <= 0xde) return s == 0xd7 ? s : s + 32;
        if (s < 0x100) return s;
        if (s == 0x130) return 'i';
        if (s == 0x178) return 0xff;
        if (s == 0x17f) return 's';
        const bool odd_upper = (s >= 0x139 && s <= 0x148) || (s >= 0x179 && s <= 0x17e);
        if (odd_upper) return (s & 1) ? s + 1 : s;
        if (s == 0x131 || s == 0x138 || s == 0x149) return s;
        return (s & 1) ? s : s + 1;
    }
    if (s >= 0x386 && s <= 0x3ab) { // Greek.
        if (s == 0x386) return 0x3ac;
        if (s >= 0x388 && s <= 0x38a) return s + 37;
        if (s == 0x38c) return 0x3cc;
        if (s == 0x38e || s == 0x38f) return s + 63;
        if (s >= 0x391 && s != 0x3a2) return s + 32;
        return s;
    }
    if (s == 0x3c2) return 0x3c3; // Final sigma.
    if (s >= 0x400 && s <= 0x52f) { // Cyrillic.
        if (s <= 0x40f) return s + 80;
        if (s <= 0x42f) return s + 32;
        if (s == 0x4c0) return 0x4cf;
        if (s >= 0x4c1 && s <= 0x4ce) return (s & 1) ? s + 1 : s;
        if ((s >= 0x460 && s <= 0x481) || (s >= 0x48a && s <= 0x4bf) || s >= 0x4d0) return (s & 1) ? s : s + 1;
        return s;
    }
    if (s >= 0x531 && s <= 0x556) return s + 48; // Armenian.
    // Hebrew points and cantillation (the maqaf, paseq and sof pasuq punctuation stay).
    if ((s >= 0x591 && s <= 0x5bd) || s == 0x5bf || s == 0x5c1 || s == 0x5c2 || s == 0x5c4 || s == 0x5c5 || s == 0x5c7) return 0;
    // Arabic marks and tatweel; alef variants.
    if ((s >= 0x610 && s <= 0x61a) || (s >= 0x64b && s <= 0x65f) || s == 0x670 || s == 0x640 || (s >= 0x6d6 && s <= 0x6dc) ||
        (s >= 0x6df && s <= 0x6e4) || s == 0x6e7 || s == 0x6e8 || (s >= 0x6ea && s <= 0x6ed)) return 0;
    if (s == 0x622 || s == 0x623 || s == 0x625) return 0x627;
    if (s >= 0xff21 && s <= 0xff3a) return s + 32; // Fullwidth Latin.
    return s;
}
std::size_t Normalize(std::string_view text, char* out, std::size_t capacity, unsigned flags) noexcept {
    if (capacity < text.size()) return kInvalid;
    struct Entity { std::string_view name; std::uint32_t scalar; };
    static constexpr Entity entities[]{{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", 0xa0}, {"&lrm;", 0x200e}, {"&rlm;", 0x200f},
                                       {"&quot;", '"'}, {"&apos;", '\''}};
    const bool fold = (flags & kFold) != 0, markup = (flags & kMarkup) != 0;
    std::size_t n = 0;
    while (!text.empty()) {
        std::uint32_t scalar = 0;
        if (markup && text.front() == '<') {
            const auto end = text.find('>');
            if (end != text.npos) { text.remove_prefix(end + 1); continue; }
        }
        if (markup && text.front() == '&')
            for (const auto& e : entities) if (text.starts_with(e.name)) { scalar = e.scalar; text.remove_prefix(e.name.size()); break; }
        if (!scalar && !Utf8(text, scalar)) return kInvalid;
        scalar = FoldScalar(scalar, fold);
        if (!scalar || (scalar == ' ' && n && out[n - 1] == ' ')) continue;
        n += Encode(scalar, out + n);
    }
    return n;
}
}
