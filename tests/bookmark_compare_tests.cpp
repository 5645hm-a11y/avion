// Bookmarks and the commands around them (docs/session.md, "Bookmarks"): the list's rules (order, half a second apart, 32
// at most, next and previous), kept in the resume ledger (version 2, saved and loaded byte for byte; a version 1 ledger
// loads with every file's other fields and no bookmarks; a damaged list is cut to its valid prefix), and the commands for
// bookmarks, the pinned frame, Pinned | Live and night mode in nine languages. The Pinned | Live picture itself is checked
// on screen by media_playback_bookmark_compare_test. No allocation.
#include "media/session/MediaSession.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
#include <cstring>
using media::session::Bookmarks; using media::session::ResumeLedger; using media::session::ResumeEntry;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t S = 10000000;
std::uint64_t Checksum(const std::byte* bytes, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) { h ^= static_cast<std::uint8_t>(bytes[i]); h *= 0x100000001b3ull; }
    return h;
}
std::byte bytes[ResumeLedger::kFileBytes], old_bytes[ResumeLedger::kVersion1Bytes];
ResumeLedger ledger, loaded;
void List() {
    Bookmarks b;
    CHECK(b.Add(30 * S) == 0 && b.Add(10 * S) == 0 && b.Add(20 * S) == 1 && b.Count() == 3); // In order.
    CHECK(b.At(0) == 10 * S && b.At(1) == 20 * S && b.At(2) == 30 * S);
    CHECK(b.Add(20 * S + 4 * S / 10) == 1 && b.Count() == 3); // Within half a second: that one.
    CHECK(b.Add(-1) == -1 && b.Count() == 3);
    // Next skips the one just landed on (within 0.25 s after it); previous from just after one goes to the one before.
    CHECK(b.Next(5 * S) == 0 && b.Next(10 * S) == 1 && b.Next(10 * S + S / 10) == 1 && b.Next(30 * S) == -1);
    CHECK(b.Previous(31 * S) == 1 && b.Previous(35 * S) == 2 && b.Previous(10 * S) == -1);
    CHECK(b.Remove(1) && b.Count() == 2 && b.At(1) == 30 * S && !b.Remove(5));
    b.Clear();
    for (unsigned i = 0; i < Bookmarks::kMax; ++i) CHECK(b.Add(static_cast<std::int64_t>(i) * S) == static_cast<int>(i));
    CHECK(b.Add(1000 * S) == -1 && b.Count() == Bookmarks::kMax); // 32 at most.
    // Through a ledger entry's fields: the valid, ordered prefix of what is there.
    ResumeEntry e{}; b.Store(e.bookmark_count, e.bookmarks);
    Bookmarks c; c.Load(e.bookmark_count, e.bookmarks); CHECK(c.Count() == Bookmarks::kMax && c.At(31) == 31 * S);
    e.bookmarks[5] = e.bookmarks[4]; c.Load(e.bookmark_count, e.bookmarks); CHECK(c.Count() == 5); // Out of order: cut there.
    e.bookmark_count = 1000; e.bookmarks[5] = 5 * S; c.Load(e.bookmark_count, e.bookmarks); CHECK(c.Count() == Bookmarks::kMax);
}
void Ledger() {
    // Version 2: bookmarks with each file, saved and loaded as they were.
    ResumeEntry e{}; e.key = 42; e.file_bytes = 1000; e.position = 600 * S; e.duration = 3600 * S; e.audio_track = 3; e.subtitle_track = 7;
    Bookmarks b; (void)b.Add(12 * S); (void)b.Add(95 * S); (void)b.Add(61 * S); b.Store(e.bookmark_count, e.bookmarks);
    ledger.Clear(); ledger.Record(e);
    CHECK(ledger.Save(bytes) && loaded.Load(bytes));
    const auto* got = loaded.Find(42, 1000);
    CHECK(got && got->bookmark_count == 3 && got->bookmarks[0] == 12 * S && got->bookmarks[1] == 61 * S && got->bookmarks[2] == 95 * S && got->position == 600 * S);
    CHECK(ResumeLedger::kFileBytes == 16 + 64 * 304 && sizeof(ResumeEntry) == 304);
    // Another version, or a size between: refused.
    std::uint32_t version = ResumeLedger::kVersion + 1; std::memcpy(bytes + 4, &version, 4); CHECK(!loaded.Load(bytes) && loaded.Count() == 0);
    CHECK(!loaded.Load(std::span<const std::byte>{bytes, ResumeLedger::kFileBytes - 8}));
    // Version 1 (48-byte entries, no bookmarks): loaded, every field kept, no bookmarks.
    std::memset(old_bytes, 0, sizeof(old_bytes));
    std::memcpy(old_bytes, "MRL1", 4); version = 1; std::memcpy(old_bytes + 4, &version, 4);
    for (unsigned i = 0; i < 2; ++i) {
        ResumeEntry v{}; v.key = 100 + i; v.file_bytes = 5000 + i; v.position = (300 + i) * S; v.duration = 7200 * S; v.audio_track = 2; v.stamp = 1 + i;
        std::memcpy(old_bytes + ResumeLedger::kHeaderBytes + i * ResumeLedger::kVersion1Entry, &v, ResumeLedger::kVersion1Entry);
    }
    const auto sum = Checksum(old_bytes + ResumeLedger::kHeaderBytes, ResumeLedger::kVersion1Bytes - ResumeLedger::kHeaderBytes);
    std::memcpy(old_bytes + 8, &sum, 8);
    CHECK(loaded.Load(old_bytes) && loaded.Count() == 2);
    const auto* old = loaded.Find(101, 5001);
    CHECK(old && old->position == 301 * S && old->duration == 7200 * S && old->audio_track == 2 && old->bookmark_count == 0);
    old_bytes[ResumeLedger::kHeaderBytes + 3] ^= std::byte{1}; CHECK(!loaded.Load(old_bytes)); // Its checksum still counts.
    std::printf("Ledger: version 2 is %zu bytes; version 1 (%zu bytes) loads without bookmarks\n", ResumeLedger::kFileBytes, ResumeLedger::kVersion1Bytes);
}
void Commands() {
    using media::ai::IntentKind; using media::ai::IntentStatus; using Dispatcher = media::ai::LocalIntentDispatcher;
    media::ai::MediaBounds bounds; bounds.duration_100ns = 3600 * S; bounds.position_100ns = 60 * S; bounds.seekable = true; bounds.bookmarks = 3;
    struct Case { const char* text; IntentKind kind; unsigned match; };
    static constexpr Case cases[]{
        {"add bookmark", IntentKind::BookmarkAdd, 0}, {"Bookmark this.", IntentKind::BookmarkAdd, 0},
        {"\xd7\xa1\xd7\x9e\xd7\x9f \xd7\x94\xd7\xa2\xd7\xa8\xd7\x94", IntentKind::BookmarkAdd, 0},               // סמן הערה
        {"\xd7\xa9\xd7\x9e\xd7\x95\xd7\xa8 \xd7\xa0\xd7\xa7\xd7\x95\xd7\x93\xd7\x94", IntentKind::BookmarkAdd, 0}, // שמור נקודה
        {"lesezeichen setzen", IntentKind::BookmarkAdd, 0},
        {"go to bookmark 2", IntentKind::BookmarkGo, 2}, {"go to bookmark", IntentKind::BookmarkGo, 0},
        {"\xd7\x9c\xd7\x9a \xd7\x9c\xd7\xa1\xd7\x99\xd7\x9e\xd7\xa0\xd7\x99\xd7\x99\xd7\x94", IntentKind::BookmarkGo, 0},     // לך לסימנייה
        {"\xd7\x9c\xd7\x9a \xd7\x9c\xd7\xa1\xd7\x99\xd7\x9e\xd7\xa0\xd7\x99\xd7\x99\xd7\x94 3", IntentKind::BookmarkGo, 3},   // לך לסימנייה 3
        {"next bookmark", IntentKind::BookmarkNext, 0}, {"previous bookmark", IntentKind::BookmarkPrevious, 0},
        {"\xd7\x94\xd7\xa1\xd7\x99\xd7\x9e\xd7\xa0\xd7\x99\xd7\x99\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94", IntentKind::BookmarkNext, 0}, // הסימנייה הבאה
        {"pin frame", IntentKind::PinFrame, 0},
        {"\xd7\x94\xd7\xa7\xd7\xa4\xd7\x90 \xd7\xa4\xd7\xa8\xd7\x99\xd7\x99\xd7\x9d \xd7\x9c\xd7\x94\xd7\xa9\xd7\x95\xd7\x95\xd7\x90\xd7\x94", IntentKind::PinFrame, 0}, // הקפא פריים להשוואה
        {"compare with the pinned frame", IntentKind::ComparePinned, 0},
        {"night mode", IntentKind::NightOn, 0}, {"normalize volume", IntentKind::NightOn, 0},
        {"\xd7\x9e\xd7\xa6\xd7\x91 \xd7\x9c\xd7\x99\xd7\x9c\xd7\x94", IntentKind::NightOn, 0},                     // מצב לילה
        {"\xd7\x90\xd7\x96\xd7\x9f \xd7\xa2\xd7\x95\xd7\xa6\xd7\x9e\xd7\xaa \xd7\xa7\xd7\x95\xd7\x9c", IntentKind::NightOn, 0}, // אזן עוצמת קול
        {"night mode off", IntentKind::NightOff, 0}, {"nachtmodus", IntentKind::NightOn, 0},
        {"\xd7\x91\xd7\x98\xd7\x9c \xd7\x9e\xd7\xa6\xd7\x91 \xd7\x9c\xd7\x99\xd7\x9c\xd7\x94", IntentKind::NightOff, 0}, // בטל מצב לילה
    };
    for (const auto& c : cases) {
        const auto intent = Dispatcher::Parse(c.text, bounds);
        if (intent.kind != c.kind || intent.status != IntentStatus::Ok || intent.match != c.match)
            std::printf("  \"%s\": %s %s %u\n", c.text, media::ai::Name(intent.kind), media::ai::Name(intent.status), intent.match);
        CHECK(intent.kind == c.kind && intent.status == IntentStatus::Ok && intent.match == c.match);
    }
    // Refusals: a bookmark that is not there, none at all, words that are not a number, media that cannot seek.
    CHECK(Dispatcher::Parse("go to bookmark 9", bounds).status == IntentStatus::OutOfRange);
    CHECK(Dispatcher::Parse("go to bookmark 0", bounds).status == IntentStatus::OutOfRange);
    CHECK(Dispatcher::Parse("go to bookmark two three", bounds).status == IntentStatus::Malformed);
    auto none = bounds; none.bookmarks = 0;
    CHECK(Dispatcher::Parse("next bookmark", none).status == IntentStatus::NoContext && Dispatcher::Parse("go to bookmark 1", none).status == IntentStatus::NoContext);
    CHECK(Dispatcher::Parse("add bookmark", none).status == IntentStatus::Ok);
    auto fixed = bounds; fixed.seekable = false;
    CHECK(Dispatcher::Parse("previous bookmark", fixed).status == IntentStatus::Unavailable);
    // Their neighbours keep their meaning.
    CHECK(Dispatcher::Parse("compare quality", bounds).kind == IntentKind::CompareOn);
    CHECK(Dispatcher::Parse("\xd7\x91\xd7\x98\xd7\x9c", bounds).kind == IntentKind::Undo); // בטל
}
}
int main() {
    std::puts("Bookmark and comparison tests"); // The first output allocates stdout's buffer: before any probe.
    allocation_probe::Begin();
    List();
    Ledger();
    Commands();
    const auto allocations = allocation_probe::End();
    std::printf("Bookmarks: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    CHECK(allocations == 0);
    return failures ? 1 : 0;
}
