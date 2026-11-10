#include "media/codec/Rbsp.hpp"

namespace media::codec {
std::uint32_t RbspReader::Bits(unsigned count) noexcept {
    if (count > 32) { good_ = false; return 0; }
    std::uint32_t result = 0;
    while (count-- && good_) {
        if (!remaining_) {
            if (offset_ == bytes_.size()) { good_ = false; break; }
            unsigned value = std::to_integer<unsigned>(bytes_[offset_++]);
            if (zeros_ == 2) {
                if (value == 3) {
                    if (offset_ == bytes_.size()) { good_ = false; break; }
                    value = std::to_integer<unsigned>(bytes_[offset_++]);
                    if (value > 3) { good_ = false; break; }
                    zeros_ = 0;
                } else if (value < 3) { good_ = false; break; }
            }
            zeros_ = value == 0 ? zeros_ + 1 : 0;
            current_ = value; remaining_ = 8;
        }
        result = (result << 1) | ((current_ >> --remaining_) & 1u);
        ++position_;
    }
    return result;
}
std::uint32_t RbspReader::Ue() noexcept {
    unsigned zeros = 0;
    while (good_ && Bits(1) == 0) if (++zeros > 31) { good_ = false; return 0; }
    if (!good_) return 0;
    return ((std::uint32_t{1} << zeros) - 1) + Bits(zeros);
}
std::int32_t RbspReader::Se() noexcept {
    const auto code = Ue();
    if (code == UINT32_MAX) { good_ = false; return 0; }
    return code & 1 ? static_cast<std::int32_t>((code + 1) / 2) : -static_cast<std::int32_t>(code / 2);
}
bool RbspReader::MoreData() const noexcept { auto copy = *this; return !copy.Finish(); }
bool RbspReader::Finish() noexcept {
    if (!good_ || Bits(1) != 1) return false;
    while (remaining_) if (Bits(1)) return false;
    return good_ && offset_ == bytes_.size();
}
}
