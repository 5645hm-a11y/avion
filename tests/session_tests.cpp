// Sessions across files (session::ResumeLedger, session::Playlist; docs/session.md): the resume rules at their edges,
// recency and eviction of the 64 entries, a ledger saved and loaded (and refused when damaged), the clock renumbering,
// path keys, and the folder's natural order.
#include "media/session/MediaSession.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
#include <cstring>
#include <string>
using namespace media::session;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t S = ResumeLedger::kSecond;
std::uint64_t Fnv(const std::byte* bytes, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) { h ^= static_cast<std::uint8_t>(bytes[i]); h *= 0x100000001b3ull; }
    return h;
}

void Rules() {
    ResumeEntry e; e.key = 1; e.duration = 600 * S;
    e.position = 15 * S; CHECK(ResumeLedger::ResumeAt(e) == 0);                 // Not past the first 15 s.
    e.position = 15 * S + 1; CHECK(ResumeLedger::ResumeAt(e) == 15 * S + 1);
    e.position = 540 * S - 1; CHECK(ResumeLedger::ResumeAt(e) == 540 * S - 1);  // More than 60 s before the end.
    e.position = 540 * S; CHECK(ResumeLedger::ResumeAt(e) == 0);                // Watched: from the start.
    e.position = 600 * S; CHECK(ResumeLedger::ResumeAt(e) == 0);
    e.duration = 0; e.position = 7200 * S; CHECK(ResumeLedger::ResumeAt(e) == 7200 * S); // Duration unknown: the first rule only.
    e.duration = 70 * S; e.position = 20 * S; CHECK(ResumeLedger::ResumeAt(e) == 0);     // A short file: never both.
}

void Ledger() {
    static ResumeLedger ledger;
    CHECK(ledger.Count() == 0 && !ledger.Find(1, 0) && !ledger.Find(0, 0));
    allocation_probe::Begin();
    ResumeEntry e; e.key = 42; e.file_bytes = 1000; e.position = 90 * S; e.duration = 3600 * S; e.audio_track = 3; e.subtitle_track = ResumeLedger::kNoTrack;
    ledger.Record(e);
    const auto* found = ledger.Find(42, 1000);
    CHECK(found && found->position == 90 * S && found->audio_track == 3 && found->subtitle_track == ResumeLedger::kNoTrack && found->stamp == 1);
    CHECK(!ledger.Find(42, 1001)); // Another file under that name now.
    e.position = 120 * S; ledger.Record(e);
    CHECK(ledger.Count() == 1 && ledger.Find(42, 1000)->position == 120 * S && ledger.Find(42, 1000)->stamp == 2);
    // 64 entries: the 65th file takes the least recent one's place, after a re-recorded one became recent again.
    for (std::uint64_t k = 100; k < 163; ++k) { ResumeEntry f; f.key = k; f.position = static_cast<std::int64_t>(k) * S; ledger.Record(f); }
    CHECK(ledger.Count() == ResumeLedger::kEntries);
    e.position = 130 * S; ledger.Record(e); // 42 is the newest again; 100 is the oldest.
    ResumeEntry late; late.key = 999; late.position = 20 * S; ledger.Record(late);
    CHECK(ledger.Count() == ResumeLedger::kEntries && !ledger.Find(100, 0) && ledger.Find(101, 0) && ledger.Find(999, 0) && ledger.Find(42, 1000));
    CHECK(ledger.Forget(999) && !ledger.Forget(999) && ledger.Count() == ResumeLedger::kEntries - 1);
    ResumeEntry zero; ledger.Record(zero); CHECK(ledger.Count() == ResumeLedger::kEntries - 1); // Key 0 is no file.
    // Saved and loaded: the same ledger, and the recency goes on from where it was.
    static std::byte bytes[ResumeLedger::kFileBytes];
    CHECK(ledger.Save(bytes) && !ledger.Save(std::span<std::byte>{bytes, ResumeLedger::kFileBytes - 1}));
    static ResumeLedger loaded;
    CHECK(loaded.Load(bytes) && loaded.Count() == ledger.Count());
    for (unsigned i = 0; i < ResumeLedger::kEntries; ++i) CHECK(std::memcmp(&loaded.At(i), &ledger.At(i), sizeof(ResumeEntry)) == 0);
    ResumeEntry next; next.key = 7; loaded.Record(next);
    CHECK(loaded.Find(7, 0)->stamp > loaded.Find(42, 1000)->stamp);
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0);
    // Damaged, foreign, short: an empty ledger.
    bytes[ResumeLedger::kHeaderBytes + 10] ^= std::byte{1};
    CHECK(!loaded.Load(bytes) && loaded.Count() == 0);
    bytes[ResumeLedger::kHeaderBytes + 10] ^= std::byte{1};
    CHECK(loaded.Load(bytes));
    bytes[0] = std::byte{'X'}; CHECK(!loaded.Load(bytes) && loaded.Count() == 0); bytes[0] = std::byte{'M'};
    CHECK(!loaded.Load(std::span<const std::byte>{bytes, 100}));
    std::uint32_t version = ResumeLedger::kVersion + 1; std::memcpy(bytes + 4, &version, 4); CHECK(!loaded.Load(bytes)); version = ResumeLedger::kVersion; std::memcpy(bytes + 4, &version, 4);
    CHECK(loaded.Load(bytes));
    // The clock at its end: the entries are renumbered 1, 2, ... in their order, and the new one is the newest.
    static ResumeLedger worn;
    ResumeEntry a; a.key = 1; worn.Record(a);
    ResumeEntry b; b.key = 2; worn.Record(b);
    static std::byte raw[ResumeLedger::kFileBytes];
    CHECK(worn.Save(raw));
    for (unsigned i = 0; i < 2; ++i) { // Slot 0 holds key 1, slot 1 key 2: stamps far apart, the last one at the clock's end.
        const std::uint32_t stamp = i ? 0xffffffffu : 0x7fffffffu;
        std::memcpy(raw + ResumeLedger::kHeaderBytes + i * sizeof(ResumeEntry) + offsetof(ResumeEntry, stamp), &stamp, 4);
    }
    const auto sum = Fnv(raw + ResumeLedger::kHeaderBytes, ResumeLedger::kEntries * sizeof(ResumeEntry));
    std::memcpy(raw + 8, &sum, 8);
    CHECK(worn.Load(raw));
    ResumeEntry c; c.key = 3; worn.Record(c);
    CHECK(worn.Find(1, 0)->stamp == 1 && worn.Find(2, 0)->stamp == 2 && worn.Find(3, 0)->stamp == 3);
}

void Keys() {
    const wchar_t* a = L"C:\\Films\\Big Buck Bunny.MP4";
    const wchar_t* b = L"c:/films/big buck bunny.mp4";
    const wchar_t* c = L"C:\\Films\\Big Buck Bunny 2.mp4";
    const auto ka = ResumeLedger::Key(a, std::wcslen(a)), kb = ResumeLedger::Key(b, std::wcslen(b)), kc = ResumeLedger::Key(c, std::wcslen(c));
    CHECK(ka == kb && ka != kc && ka != 0);
    const char* hebrew = "/media/סרטים/הסרט.mp4"; // UTF-8 code units: no folding beyond ASCII.
    CHECK(ResumeLedger::Key(hebrew, std::strlen(hebrew)) != 0 && ResumeLedger::Key(hebrew, 0) == ResumeLedger::Key(b, 0));
}

void Folder() {
    CHECK(Playlist::Playable(L"a.mp4") && Playlist::Playable(L"A.MOV") && Playlist::Playable(L"clip.m4v") && Playlist::Playable(L"x.3GP") &&
          Playlist::Playable(L"song.M4A"));
    CHECK(!Playlist::Playable(L"notes.txt") && !Playlist::Playable(L".mp4") && !Playlist::Playable(L"mp4") && !Playlist::Playable(L"film.mkv") &&
          !Playlist::Playable(L"film.mp4.part"));
    CHECK(Playlist::Compare(L"Episode 2.mp4", L"Episode 10.mp4") < 0 && Playlist::Compare(L"Episode 10.mp4", L"Episode 9.mp4") > 0);
    CHECK(Playlist::Compare(L"episode 02.mp4", L"Episode 2.mp4") != 0 && Playlist::Compare(L"a.mp4", L"a.mp4") == 0);
    CHECK(Playlist::Compare(L"S01E09.mp4", L"S01E10.mp4") < 0 && Playlist::Compare(L"s1e2.mp4", L"S01E10.mp4") < 0);
    CHECK(Playlist::Compare(L"Zebra.mp4", L"apple.mp4") > 0 && Playlist::Compare(L"Film.mp4", L"Film 2.mp4") > 0); // '.' after ' '.
    CHECK(Playlist::Compare(L"x99999999999999999999.mp4", L"x100000000000000000000.mp4") < 0); // Numbers longer than any integer.
    static Playlist list;
    allocation_probe::Begin();
    for (const wchar_t* name : {L"Episode 10.mp4", L"notes.txt", L"Episode 2.mp4", L"episode 1.MP4", L"Episode 3.mp4", L"cover.jpg", L"Bonus.mov"})
        (void)list.Add(name);
    list.Sort();
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0);
    CHECK(list.Count() == 5 && list.Skipped() == 2);
    CHECK(list.At(0) == L"Bonus.mov" && list.At(1) == L"episode 1.MP4" && list.At(2) == L"Episode 2.mp4" && list.At(3) == L"Episode 3.mp4" &&
          list.At(4) == L"Episode 10.mp4" && list.At(5).empty());
    CHECK(list.Find(L"EPISODE 2.MP4") == 2 && list.Find(L"Episode 4.mp4") == -1 && list.Find(L"notes.txt") == -1);
    list.Clear(); CHECK(list.Count() == 0 && list.Skipped() == 0);
    // Full: the names beyond the storage are counted as skipped.
    std::wstring name;
    for (unsigned i = 0; i < Playlist::kMaxFiles + 3; ++i) { name = L"f" + std::to_wstring(i) + L".mp4"; (void)list.Add(name); }
    CHECK(list.Count() == Playlist::kMaxFiles && list.Skipped() == 3);
    list.Sort();
    CHECK(list.At(0) == L"f0.mp4" && list.At(1) == L"f1.mp4" && list.At(Playlist::kMaxFiles - 1) == L"f2047.mp4");
}
}
int main() {
    std::puts("Session tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    Rules(); Ledger(); Keys(); Folder();
    std::printf("Sessions: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
