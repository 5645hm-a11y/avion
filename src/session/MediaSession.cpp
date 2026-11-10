#include "media/session/MediaSession.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>

namespace media::session {
static_assert(std::endian::native == std::endian::little, "The ledger's bytes are little-endian: written as they are in memory");
namespace {
constexpr char kMagic[4]{'M', 'R', 'L', '1'};
std::uint64_t Checksum(const std::byte* bytes, std::size_t size) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) { h ^= static_cast<std::uint8_t>(bytes[i]); h *= 0x100000001b3ull; }
    return h;
}
}
const ResumeEntry* ResumeLedger::Find(std::uint64_t key, std::uint64_t file_bytes) const noexcept {
    if (!key) return nullptr;
    for (const auto& e : entries_) if (e.key == key) return e.file_bytes == file_bytes ? &e : nullptr;
    return nullptr;
}
std::int64_t ResumeLedger::ResumeAt(const ResumeEntry& e) noexcept {
    if (e.position <= kMinimumWatched) return 0;
    if (e.duration > 0 && e.position >= e.duration - kMinimumLeft) return 0;
    return e.position;
}
void ResumeLedger::Record(ResumeEntry entry) noexcept {
    if (!entry.key) return;
    ResumeEntry* slot = nullptr;
    for (auto& e : entries_) if (e.key == entry.key) { slot = &e; break; }
    for (auto& e : entries_) if (!slot && !e.key) { slot = &e; break; }
    if (!slot) { slot = entries_; for (auto& e : entries_) if (e.stamp < slot->stamp) slot = &e; } // The least recent.
    if (clock_ == 0xffffffffu) { // The clock would wrap: renumber the entries 1, 2, ... in their order (stamps are unique).
        ResumeEntry* order[kEntries]; unsigned n = 0;
        for (auto& e : entries_) {
            if (!e.key) continue;
            unsigned k = n++;
            while (k && order[k - 1]->stamp > e.stamp) { order[k] = order[k - 1]; --k; }
            order[k] = &e;
        }
        for (unsigned k = 0; k < n; ++k) order[k]->stamp = k + 1;
        clock_ = n;
    }
    entry.stamp = ++clock_;
    Bookmarks marks; marks.Load(entry.bookmark_count, entry.bookmarks); marks.Store(entry.bookmark_count, entry.bookmarks); // Kept valid.
    *slot = entry;
}
bool ResumeLedger::Forget(std::uint64_t key) noexcept {
    for (auto& e : entries_) if (key && e.key == key) { e = {}; return true; }
    return false;
}
unsigned ResumeLedger::Count() const noexcept {
    unsigned n = 0;
    for (const auto& e : entries_) n += e.key != 0;
    return n;
}
bool ResumeLedger::Save(std::span<std::byte> out) const noexcept {
    if (out.size() < kFileBytes) return false;
    std::memcpy(out.data(), kMagic, 4);
    std::memcpy(out.data() + 4, &kVersion, 4);
    std::memcpy(out.data() + kHeaderBytes, entries_, sizeof(entries_));
    const auto sum = Checksum(out.data() + kHeaderBytes, sizeof(entries_));
    std::memcpy(out.data() + 8, &sum, 8);
    return true;
}
bool ResumeLedger::Load(std::span<const std::byte> in) noexcept {
    Clear();
    std::uint32_t version = 0; std::uint64_t sum = 0;
    if (in.size() < kHeaderBytes || std::memcmp(in.data(), kMagic, 4) != 0) return false;
    std::memcpy(&version, in.data() + 4, 4); std::memcpy(&sum, in.data() + 8, 8);
    if (version == kVersion && in.size() == kFileBytes && sum == Checksum(in.data() + kHeaderBytes, sizeof(entries_))) {
        std::memcpy(entries_, in.data() + kHeaderBytes, sizeof(entries_));
    } else if (version == 1 && in.size() == kVersion1Bytes && sum == Checksum(in.data() + kHeaderBytes, kEntries * kVersion1Entry)) {
        // Version 1: the same fields, before the bookmarks (its reserved word, 0, is now the bookmark count).
        // Over the entry's first bytes only: its bookmarks stay as Clear() left them, none.
        static_assert(std::is_trivially_copyable_v<ResumeEntry>);
        for (unsigned i = 0; i < kEntries; ++i)
            std::memcpy(static_cast<void*>(&entries_[i]), in.data() + kHeaderBytes + i * kVersion1Entry, kVersion1Entry);
    } else return false;
    for (auto& e : entries_) {
        clock_ = (std::max)(clock_, e.stamp);
        Bookmarks marks; marks.Load(e.bookmark_count, e.bookmarks); marks.Store(e.bookmark_count, e.bookmarks); // A damaged list is cut.
    }
    return true;
}
int Bookmarks::Add(std::int64_t time) noexcept {
    if (time < 0) return -1;
    for (unsigned i = 0; i < count_; ++i) if (at_[i] - time <= kNear && time - at_[i] <= kNear) return static_cast<int>(i);
    if (count_ == kMax) return -1;
    unsigned i = count_;
    while (i && at_[i - 1] > time) { at_[i] = at_[i - 1]; --i; }
    at_[i] = time; ++count_;
    return static_cast<int>(i);
}
bool Bookmarks::Remove(unsigned index) noexcept {
    if (index >= count_) return false;
    for (unsigned i = index + 1; i < count_; ++i) at_[i - 1] = at_[i];
    --count_; return true;
}
int Bookmarks::Next(std::int64_t position) const noexcept {
    for (unsigned i = 0; i < count_; ++i) if (at_[i] > position + kAhead) return static_cast<int>(i);
    return -1;
}
int Bookmarks::Previous(std::int64_t position) const noexcept {
    for (unsigned i = count_; i-- > 0;) if (at_[i] < position - kBack) return static_cast<int>(i);
    return -1;
}
void Bookmarks::Store(std::uint32_t& count, std::int64_t (&at)[kMax]) const noexcept {
    count = count_;
    for (unsigned i = 0; i < kMax; ++i) at[i] = i < count_ ? at_[i] : 0;
}
void Bookmarks::Load(std::uint32_t count, const std::int64_t (&at)[kMax]) noexcept {
    count_ = 0;
    for (unsigned i = 0; i < count && i < kMax; ++i) {
        if (at[i] < 0 || (count_ && at[i] <= at_[count_ - 1])) break; // Only a valid, ordered list.
        at_[count_++] = at[i];
    }
}

namespace {
std::uint32_t Fold(wchar_t c) noexcept {
    auto u = static_cast<std::uint32_t>(c);
    if ((u >= 'A' && u <= 'Z') || (u >= 0xc0 && u <= 0xde && u != 0xd7)) u += 0x20; // ASCII and Latin-1 capitals.
    return u;
}
bool Digit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }
}
bool Playlist::Playable(std::wstring_view name) noexcept {
    const auto dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return false;
    const auto ext = name.substr(dot + 1);
    for (const std::wstring_view known : {std::wstring_view{L"mp4"}, std::wstring_view{L"m4v"}, std::wstring_view{L"m4a"}, std::wstring_view{L"mov"}, std::wstring_view{L"3gp"}}) {
        if (ext.size() != known.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < ext.size() && same; ++i) same = Fold(ext[i]) == static_cast<std::uint32_t>(known[i]);
        if (same) return true;
    }
    return false;
}
int Playlist::Compare(std::wstring_view a, std::wstring_view b) noexcept {
    std::size_t i = 0, k = 0;
    while (i < a.size() && k < b.size()) {
        if (Digit(a[i]) && Digit(b[k])) {
            // Two numbers: by value (length without leading zeros, then digits).
            std::size_t ai = i, bk = k;
            while (ai < a.size() && a[ai] == L'0') ++ai;
            while (bk < b.size() && b[bk] == L'0') ++bk;
            std::size_t ae = ai, be = bk;
            while (ae < a.size() && Digit(a[ae])) ++ae;
            while (be < b.size() && Digit(b[be])) ++be;
            if (ae - ai != be - bk) return ae - ai < be - bk ? -1 : 1;
            for (std::size_t d = 0; d < ae - ai; ++d) if (a[ai + d] != b[bk + d]) return a[ai + d] < b[bk + d] ? -1 : 1;
            i = ae; k = be; continue;
        }
        const auto x = Fold(a[i]), y = Fold(b[k]);
        if (x != y) return x < y ? -1 : 1;
        ++i; ++k;
    }
    if (i < a.size() || k < b.size()) return i < a.size() ? 1 : -1;
    // The same to a reader: decide by the code units, so the order is total.
    return a == b ? 0 : a < b ? -1 : 1;
}
bool Playlist::Add(std::wstring_view name) noexcept {
    if (!Playable(name) || count_ == kMaxFiles || name.size() > kNameUnits - used_) { ++skipped_; return false; }
    std::memcpy(pool_ + used_, name.data(), name.size() * sizeof(wchar_t));
    names_[count_++] = {static_cast<std::uint32_t>(used_), static_cast<std::uint32_t>(name.size())};
    used_ += name.size();
    return true;
}
std::wstring_view Playlist::At(unsigned i) const noexcept {
    if (i >= count_) return {};
    return {pool_ + names_[i].offset, names_[i].length};
}
void Playlist::Sort() noexcept {
    std::sort(names_, names_ + count_, [this](const Name& x, const Name& y) {
        return Compare({pool_ + x.offset, x.length}, {pool_ + y.offset, y.length}) < 0;
    });
}
int Playlist::Find(std::wstring_view name) const noexcept {
    for (unsigned i = 0; i < count_; ++i) {
        const auto n = At(i);
        if (n.size() != name.size()) continue;
        bool same = true;
        for (std::size_t c = 0; c < n.size() && same; ++c) same = Fold(n[c]) == Fold(name[c]);
        if (same) return static_cast<int>(i);
    }
    return -1;
}
}
