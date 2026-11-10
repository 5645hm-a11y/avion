#include "media/subtitle/SubtitleTrack.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
using namespace media::subtitle;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(false)
void Parse() {
    CueReader srt("\xef\xbb\xbf" "1\r\n00:00:01,250 --> 00:00:02,500\r\nOne\r\nTwo\r\n\r\n2\r\n01:02:03,004 --> 01:02:05,000\r\nالعربية עברית 日本語");
    Cue cue; CHECK(srt.Next(cue) == ParseStatus::CueReady); CHECK(cue.start == 12500000 && cue.end == 25000000); CHECK(cue.text == "One\r\nTwo");
    CHECK(srt.Next(cue) == ParseStatus::CueReady); CHECK(cue.start == 37230040000ll); CHECK(srt.Next(cue) == ParseStatus::End);
    CueReader vtt("WEBVTT test\nLanguage: en\n\nNOTE ignore\nnot a cue\n\nSTYLE\n::cue { color: white; }\n\nname\n00:01.000 --> 00:02.000 align:center\n<i>Hi &amp; bye</i>\n\n");
    CHECK(vtt.Next(cue) == ParseStatus::CueReady && cue.text == "<i>Hi &amp; bye</i>"); CHECK(vtt.Next(cue) == ParseStatus::End);
    for (auto input : {"1\n00:00:61,000 --> 00:01:02,000\nx", "1\n00:00:01,000 --> 00:00:00,000\nx", "1\n00:00:01,000 --> 00:00:01,000\nx",
                       "1\n00:00:01,000 --> 00:00:02,000\n", "x\n00:00:01,000 --> 00:00:02,000\nx", "1\n00:00:01,000 --> 00:00:02,000\n\xc0\xaf"}) {
        CueReader invalid(input); CHECK(invalid.Next(cue) == ParseStatus::Invalid); CHECK(invalid.Next(cue) == ParseStatus::Invalid);
    }
    for (auto input : {"\xf0\x9f", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xc0\x80"}) {
        std::string_view text(input); std::uint32_t scalar = 0; CHECK(!Utf8(text,scalar));
    }
    std::string_view emoji("\xf0\x9f\x8c\x8d"); std::uint32_t scalar = 0; CHECK(Utf8(emoji,scalar) && scalar == 0x1f30d && emoji.empty());
}
void Track() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(2*1024*1024));
    SubtitleTrack track;
    constexpr auto text = "3\n00:00:03,000 --> 00:00:04,000\nThree\n\n1\n00:00:01,000 --> 00:00:05,000\nLong\n\n2\n00:00:02,000 --> 00:00:03,000\nTwo\n";
    allocation_probe::Begin(); CHECK(track.Prepare(arena,text)); const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
    CHECK(arena.Seal()); CHECK(track.Cues().size() == 3);
    unsigned active[4]{};
    allocation_probe::Begin();
    CHECK(track.Select(9999999,active) == 0); CHECK(track.Select(10000000,active) == 1 && active[0] == 0);
    CHECK(track.Select(20000000,active) == 2 && active[1] == 1); CHECK(track.Select(30000000,active) == 2 && active[1] == 2);
    CHECK(track.Select(40000000,active) == 1 && active[0] == 0); CHECK(track.Select(50000000,active) == 0);
    for (int i = 0; i < 10000; ++i) { CHECK(track.Select(i % 2 ? 35000000 : 15000000,active) == (i % 2 ? 2u : 1u)); }
    CHECK(!track.Prepare(arena,text));
    CHECK(allocation_probe::End() == 0);
    // Five simultaneous cues are accepted; the four latest-starting are selected.
    media::core::LinearMemoryArena limit; CHECK(limit.Initialize(65536)); SubtitleTrack overlap;
    CHECK(overlap.Prepare(limit,"1\n00:00:00,000 --> 00:00:02,000\na\n\n2\n00:00:00,100 --> 00:00:02,000\nb\n\n3\n00:00:00,200 --> 00:00:02,000\nc\n\n4\n00:00:00,300 --> 00:00:02,000\nd\n\n5\n00:00:00,400 --> 00:00:02,000\ne"));
    CHECK(overlap.Stats().max_active == 5 && overlap.Stats().skipped == 0);
    CHECK(overlap.Select(10000000,active) == 4 && active[0] == 1 && active[3] == 4);
}
// Malformed blocks are skipped individually, as other players do; the valid cues around them survive.
void Resilience() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(65536));
    SubtitleTrack track;
    constexpr auto text = "1\n00:00:01,000 --> 00:00:02,000\nkept one\n\n"
                          "2\n00:00:61,000 --> 00:00:62,000\nbad seconds\nsecond line\n\n"     // Invalid timing.
                          "3\n00:00:03,000 --> 00:00:04,000\nkept two\n\n"
                          "4\n00:00:05,000 --> 00:00:06,000\n\xc0\xaf\n\n"                     // Overlong UTF-8.
                          "junk line\nmore junk\n\n"                                             // Not a cue at all.
                          "6\n00:00:07,000 --> 00:00:08,000\nkept three\n";
    CHECK(track.Prepare(arena,text)); CHECK(track.Cues().size() == 3);
    CHECK(track.Stats().skipped == 3 && track.ErrorLine() == 6);
    CHECK(track.Cues()[1].text == "kept two" && track.Cues()[2].text == "kept three");
    SubtitleTrack empty; CHECK(!empty.Prepare(arena,"junk\n\nmore junk\n")); CHECK(empty.Stats().skipped == 2 && empty.ErrorLine() == 1);
    // Garbage input (e.g. a binary file passed as subtitles) fails cleanly without touching the arena.
    const auto used = arena.Statistics().used_bytes;
    constexpr char binary[]{'\x00','\x01','\xff','\xfe','-','-','>','\n','\x89','P','N','G'};
    SubtitleTrack garbage; CHECK(!garbage.Prepare(arena,{binary,sizeof(binary)})); CHECK(arena.Statistics().used_bytes == used);
}
// Two subtitle tracks (languages) as channels of one track: interleaved in time, selected by what is shown.
void Channels() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(65536));
    SubtitleTrack track;
    const std::string_view sources[]{
        "1\n00:00:01,000 --> 00:00:03,000\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\n\n2\n00:00:04,000 --> 00:00:05,000\n\xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d\n", // שלום, עולם
        "", // A track without cues is an empty channel.
        "WEBVTT\n\n00:01.500 --> 00:02.500\nHello\n\n00:04.000 --> 00:06.000\nWorld\n\n00:07.000 --> 00:08.000\nAgain\n"};
    allocation_probe::Begin(); CHECK(track.Prepare(arena, sources)); CHECK(allocation_probe::End() == 0);
    CHECK(track.Channels() == 3 && track.Cues().size() == 5 && track.ChannelCues(0) == 2 && track.ChannelCues(1) == 0 && track.ChannelCues(2) == 3);
    CHECK(track.Cues()[0].channel == 0 && track.Cues()[1].channel == 2 && track.Cues()[0].start < track.Cues()[1].start); // Sorted across channels.
    unsigned active[4]{};
    allocation_probe::Begin();
    CHECK(track.Shown() == ~0u && track.Select(20000000, active) == 2); // Both, by default.
    track.Show(1u << 0);
    CHECK(track.Select(20000000, active) == 1 && track.Cues()[active[0]].channel == 0 && track.Select(70000000, active) == 0);
    CHECK(track.Shown(0) && !track.Shown(1));
    track.Show(1u << 2);
    CHECK(track.Select(20000000, active) == 1 && track.Cues()[active[0]].text == "Hello" && track.Select(45000000, active) == 1);
    CHECK(track.Select(70000000, active) == 1 && track.Cues()[active[0]].text == "Again");
    track.Show(0);
    CHECK(track.Select(20000000, active) == 0 && track.Select(45000000, active) == 0);
    CHECK(allocation_probe::End() == 0);
    SubtitleTrack nothing; const std::string_view empty[]{"", "junk\n\n"};
    CHECK(!nothing.Prepare(arena, empty));
    SubtitleTrack many; std::string_view seventeen[SubtitleTrack::kMaxChannels + 1]; for (auto& s : seventeen) s = sources[0];
    CHECK(!many.Prepare(arena, seventeen));
}
}
int main() { Parse(); Track(); Resilience(); Channels(); std::printf("Subtitles: %d failures\n",failures); return failures ? 1 : 0; }
