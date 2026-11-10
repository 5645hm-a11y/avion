#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace media::session {
// Playback sessions across files (docs/session.md): where each file was left, and the folder's files in order.
// Portable, fixed storage, nothing allocated; the platform reads and writes the ledger's bytes and lists the folder.

// A file's bookmarks (docs/session.md, "Bookmarks"): up to kMax media times, in order. Fixed storage.
class Bookmarks final {
public:
    static constexpr unsigned kMax = 32;
    static constexpr std::int64_t kNear = 5000000;  // 0.5 s: a bookmark this close to one already set is that one.
    static constexpr std::int64_t kAhead = 2500000; // "Next" skips one just landed on (within 0.25 s after it).
    static constexpr std::int64_t kBack = 10000000; // "Previous" from less than 1 s after a bookmark goes to the one before.
    // The bookmark at `time`: an existing one within kNear, else a new one in order. Its index, or -1 (full, or negative).
    int Add(std::int64_t time) noexcept;
    bool Remove(unsigned index) noexcept;
    void Clear() noexcept { count_ = 0; }
    int Next(std::int64_t position) const noexcept;     // The first one after position + kAhead; -1: none.
    int Previous(std::int64_t position) const noexcept; // The last one before position - kBack; -1: none.
    unsigned Count() const noexcept { return count_; }
    std::int64_t At(unsigned index) const noexcept { return index < count_ ? at_[index] : 0; }
    // To and from a ledger entry (ResumeEntry's fields). Load keeps the valid, ordered prefix of what it is given.
    void Store(std::uint32_t& count, std::int64_t (&at)[kMax]) const noexcept;
    void Load(std::uint32_t count, const std::int64_t (&at)[kMax]) noexcept;
private:
    std::int64_t at_[kMax]{};
    unsigned count_ = 0;
};
// One file as it was left: its position, its duration, the tracks chosen, and its bookmarks (version 2).
struct ResumeEntry {
    std::uint64_t key = 0;        // ResumeLedger::Key of the file's full path (0: a free slot).
    std::uint64_t file_bytes = 0; // The file's size then: a different file under the same name is not resumed.
    std::int64_t position = 0, duration = 0; // 100 ns; duration 0: unknown.
    std::uint32_t audio_track = 0;    // Container track id (0: none chosen).
    std::uint32_t subtitle_track = 0; // Container track id; 0 the sidecar; kNoTrack none shown.
    std::uint32_t stamp = 0;          // Recency: the ledger's clock when recorded.
    std::uint32_t bookmark_count = 0; // Version 1: reserved, always 0.
    std::int64_t bookmarks[Bookmarks::kMax]{}; // Media times (100 ns), in order.
};
static_assert(sizeof(ResumeEntry) == 48 + 8 * Bookmarks::kMax);
// The last kEntries files played, by path. The least recently recorded one gives way to a new file. Saved as kFileBytes
// fixed bytes (little-endian, with a checksum); a damaged or foreign file loads as an empty ledger. A version 1 ledger (the
// entries' first 48 bytes, before bookmarks) loads too, every file without bookmarks.
class ResumeLedger final {
public:
    static constexpr unsigned kEntries = 64;
    static constexpr std::int64_t kSecond = 10000000;
    // Resuming: past the first kMinimumWatched, and more than kMinimumLeft before the end (closer to the end, the file
    // was watched: it starts again from the beginning).
    static constexpr std::int64_t kMinimumWatched = 15 * kSecond, kMinimumLeft = 60 * kSecond;
    static constexpr std::uint32_t kNoTrack = 0xffffffffu;
    static constexpr std::uint32_t kVersion = 2;
    static constexpr std::size_t kHeaderBytes = 16, kFileBytes = kHeaderBytes + kEntries * sizeof(ResumeEntry);
    static constexpr std::size_t kVersion1Entry = 48, kVersion1Bytes = kHeaderBytes + kEntries * kVersion1Entry;
    // FNV-1a over the path's code units, with ASCII letters folded to lower case and '/' read as '\\' (Windows paths name
    // the same file whatever their case). Never 0.
    template <class Char> static std::uint64_t Key(const Char* path, std::size_t length) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (std::size_t i = 0; i < length; ++i) {
            auto c = static_cast<std::uint32_t>(path[i]);
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c == '/') c = '\\';
            for (unsigned b = 0; b < 4; ++b) { h ^= (c >> (8 * b)) & 0xffu; h *= 0x100000001b3ull; }
        }
        return h ? h : 1;
    }
    // The file's entry; nullptr when there is none, or the file at that path is not the one recorded (another size).
    const ResumeEntry* Find(std::uint64_t key, std::uint64_t file_bytes) const noexcept;
    // Where an entry's file resumes: its position, or 0 (from the start) inside the first kMinimumWatched or the last
    // kMinimumLeft. With no duration known, only the first rule applies.
    static std::int64_t ResumeAt(const ResumeEntry&) noexcept;
    // Records (or updates) the entry for entry.key, newest of all; a new key takes a free slot, else the least recent.
    void Record(ResumeEntry entry) noexcept;
    bool Forget(std::uint64_t key) noexcept;
    void Clear() noexcept { *this = ResumeLedger{}; }
    unsigned Count() const noexcept;
    const ResumeEntry& At(unsigned i) const noexcept { return entries_[i < kEntries ? i : 0]; } // Slots, free ones included.
    // kFileBytes into `out` (false: too small); a ledger saved and loaded is the same ledger.
    bool Save(std::span<std::byte> out) const noexcept;
    // False (and an empty ledger): not kFileBytes (kVersion1Bytes for version 1), not a ledger, another version, or a
    // checksum that does not match.
    bool Load(std::span<const std::byte> in) noexcept;
private:
    ResumeEntry entries_[kEntries]{};
    std::uint32_t clock_ = 0;
};

// The playable files of a folder in the order a person reads them: case ignored and digit runs compared as numbers
// ("Episode 2" before "Episode 10"). Names only, no folder; wchar_t as the platform lists them. Fixed storage.
class Playlist final {
public:
    static constexpr unsigned kMaxFiles = 2048;
    static constexpr std::size_t kNameUnits = 1u << 17; // All names together.
    // .mp4 .m4v .m4a .mov .3gp (any case): what the ISO BMFF reader opens (an .m4a plays in music mode).
    static bool Playable(std::wstring_view name) noexcept;
    // Natural order: < 0, 0 or > 0. Names equal but for case (or leading zeros) compare by their code units.
    static int Compare(std::wstring_view a, std::wstring_view b) noexcept;
    void Clear() noexcept { count_ = 0; used_ = 0; skipped_ = 0; }
    // A playable name is kept (false and counted as skipped otherwise, or when the storage is full).
    bool Add(std::wstring_view name) noexcept;
    void Sort() noexcept;
    unsigned Count() const noexcept { return count_; }
    unsigned Skipped() const noexcept { return skipped_; }
    std::wstring_view At(unsigned i) const noexcept;
    int Find(std::wstring_view name) const noexcept; // The index of `name` (ASCII case ignored), -1 when absent.
private:
    struct Name { std::uint32_t offset = 0; std::uint32_t length = 0; };
    wchar_t pool_[kNameUnits]{};
    Name names_[kMaxFiles]{};
    unsigned count_ = 0, skipped_ = 0;
    std::size_t used_ = 0;
};
}
