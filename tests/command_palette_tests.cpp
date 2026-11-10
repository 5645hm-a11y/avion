// media::ui::CommandPalette: text typed as UTF-16 code units in any script becomes well-formed UTF-8 in fixed
// storage; input-method compositions stay apart from the committed text; the bar's motion is bounded and settles.
#include "media/ui/CommandPalette.hpp"
#include "media/text/Utf8.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>
using namespace media::ui;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t kTick = 10000000 / 120; // One 120 Hz refresh.
char storage[CommandPalette::kStorageBytes];
bool WellFormed(std::string_view text) {
    while (!text.empty()) { std::uint32_t scalar = 0; if (!media::text::Utf8(text, scalar)) return false; }
    return true;
}
unsigned Type(CommandPalette& palette, std::u16string_view keys, std::int64_t now) {
    unsigned changed = 0;
    for (const char16_t key : keys) changed += palette.Utf16(static_cast<std::uint16_t>(key), now) ? 1u : 0u;
    return changed;
}
void Typing() {
    CommandPalette palette;
    CHECK(!palette.Available() && !palette.Open(1) && !palette.IsOpen()); // No storage: it never opens.
    char small[16]; palette.Configure(small); CHECK(!palette.Available());
    palette.Configure(storage); CHECK(palette.Available());
    CHECK(!palette.Utf16('a', 1) && palette.Text().empty()); // Closed: keys are not text.
    CHECK(palette.Open(1) && palette.IsOpen() && palette.Text().empty());
    const auto revision = palette.Revision();
    CHECK(Type(palette, u"pause", 2) == 5 && palette.Text() == "pause" && palette.Revision() == revision + 5);
    // Every script arrives the same way, one code unit at a time.
    struct Sample { std::u16string_view keys; std::string_view utf8; };
    const Sample samples[]{
        {u"עצור", "עצור"}, {u"חזור 10 שניות", "חזור 10 שניות"}, {u"توقف", "توقف"}, {u"ابحث عن مرحبا", "ابحث عن مرحبا"},
        {u"暂停", "暂停"}, {u"快退10秒", "快退10秒"}, {u"一時停止", "一時停止"}, {u"音量を50に", "音量を50に"}, {u"пауза", "пауза"},
        {u"zurückspulen", "zurückspulen"}, {u"más alto", "más alto"}, {u"arrête", "arrête"}, {u"한국어 हिन्दी ไทย", "한국어 हिन्दी ไทย"},
        {u"\xd83d\xde00 \xd834\xdd1e", "\xf0\x9f\x98\x80 \xf0\x9d\x84\x9e"}, // Surrogate pairs: an emoji and a musical clef.
    };
    for (const auto& sample : samples) {
        CHECK(palette.Open(3));
        (void)Type(palette, sample.keys, 4);
        CHECK(palette.Text() == sample.utf8 && WellFormed(palette.Text()));
    }
    // Controls and broken surrogates never become text.
    CHECK(palette.Open(5));
    for (const auto unit : std::initializer_list<std::uint16_t>{0x08, 0x0d, 0x1b, 0x06, 0x0b, 0x7f, 0x85, 0x9f, 0xfffe, 0xffff, 0x00}) CHECK(!palette.Utf16(unit, 6));
    CHECK(!palette.Utf16(0xde00, 6) && palette.Text().empty());                      // A low half alone.
    CHECK(!palette.Utf16(0xd83d, 6) && palette.Utf16('a', 6) && palette.Text() == "a"); // A high half followed by a letter: the letter only.
    CHECK(!palette.Utf16(0xd83d, 6) && !palette.Utf16(0xd83d, 6) && palette.Utf16(0xde00, 6) && palette.Text() == "a\xf0\x9f\x98\x80");
    CHECK(palette.Scalar(0x1d11e, 6) && !palette.Scalar(0x110000, 6) && !palette.Scalar(0xd800, 6) && !palette.Scalar(0x1fffe, 6) && !palette.Scalar(9, 6));
    CHECK(palette.Text() == "a\xf0\x9f\x98\x80\xf0\x9d\x84\x9e");
    // Backspace removes one scalar, whatever its length; a word at a time with Control.
    CHECK(palette.Backspace(7) && palette.Text() == "a\xf0\x9f\x98\x80" && palette.Backspace(7) && palette.Text() == "a");
    CHECK(palette.Backspace(7) && palette.Text().empty() && !palette.Backspace(7) && !palette.DeleteWord(7));
    (void)Type(palette, u"חזור 10 שניות  ", 8);
    CHECK(palette.DeleteWord(8) && palette.Text() == "חזור 10 " && palette.DeleteWord(8) && palette.Text() == "חזור ");
    CHECK(palette.Backspace(8) && palette.Backspace(8) && palette.Text() == "חזו" && palette.DeleteWord(8) && palette.Text().empty());
    // Capacity: text stops at the limit, on a scalar boundary.
    CHECK(palette.Open(9));
    unsigned accepted = 0;
    for (unsigned i = 0; i < 300; ++i) { (void)palette.Utf16(0xd83d, 9); accepted += palette.Utf16(0xde00, 9) ? 1u : 0u; }
    CHECK(accepted == CommandPalette::kMaxTextBytes / 4 && palette.Text().size() == CommandPalette::kMaxTextBytes && palette.Full() && WellFormed(palette.Text()));
    CHECK(!palette.Utf16('a', 9) && palette.Backspace(9) && palette.Utf16('a', 9) && palette.Full() && !palette.Utf16(0x65e5, 9));
    CHECK(palette.Text().size() == CommandPalette::kMaxTextBytes - 3 && WellFormed(palette.Text())); // Never a cut scalar.
    // Closing keeps the text (it fades out with the bar); opening starts empty.
    palette.Close(10); CHECK(!palette.IsOpen() && palette.Text().size() == CommandPalette::kMaxTextBytes - 3 && !palette.Utf16('a', 10));
    CHECK(palette.Open(11) && palette.Text().empty());
}
// Input methods: the composition is shown after the text and replaced as it changes; the result is committed.
void Composing() {
    CommandPalette palette; palette.Configure(storage);
    CHECK(palette.Open(1));
    (void)Type(palette, u"find ", 1);
    auto revision = palette.Revision();
    palette.Compose(u"にほ", 2, 2);
    CHECK(palette.Composition() == "にほ" && palette.Text() == "find " && palette.Revision() != revision);
    palette.Compose(u"にほんご", 4, 2); CHECK(palette.Composition() == "にほんご");
    palette.Compose(u"日本語", 3, 2); CHECK(palette.Composition() == "日本語" && palette.Text() == "find ");
    CHECK(!palette.Backspace(2) && !palette.DeleteWord(2) && palette.Text() == "find "); // The input method owns the keys meanwhile.
    // Commit: the result string is inserted and the composition ends.
    CHECK(palette.Insert(u"日本語", 3, 3) == 3);
    palette.Compose(static_cast<const char16_t*>(nullptr), 0, 3);
    CHECK(palette.Text() == "find 日本語" && palette.Composition().empty());
    // Pinyin: letters compose, the chosen characters arrive as the result; a second word follows.
    palette.Compose(u"zan'ting", 8, 4); CHECK(palette.Composition() == "zan'ting");
    CHECK(palette.Insert(u" 暂停", 3, 4) == 3); palette.Compose(u"", 0, 4);
    CHECK(palette.Text() == "find 日本語 暂停" && palette.Composition().empty());
    // A cancelled composition leaves the text alone.
    palette.Compose(u"한", 1, 5); palette.Compose(u"", 0, 5); CHECK(palette.Text() == "find 日本語 暂停" && palette.Composition().empty());
    // Hebrew and Arabic results (no composition, committed directly), with a surrogate pair in a result.
    CHECK(palette.Open(6) && palette.Insert(u"שלום عالم \xd83d\xde00", 12, 6) == 12 && palette.Text() == "שלום عالم \xf0\x9f\x98\x80");
    // Pasted text: line breaks and tabs are spaces, a NUL ends it, controls are dropped.
    CHECK(palette.Open(7) && palette.Insert(u"two\r\nlines\tand\x01 more\0hidden", 30, 7) == 20 && palette.Text() == "two  lines and more");
    // A long composition is cut on a scalar boundary; a long paste stops when the text is full.
    char16_t many[300]; for (auto& unit : many) unit = u'語';
    palette.Compose(many, 300, 8);
    CHECK(palette.Composition().size() <= CommandPalette::kMaxCompositionBytes && palette.Composition().size() >= CommandPalette::kMaxCompositionBytes - 3 && WellFormed(palette.Composition()));
    palette.Compose(u"", 0, 8); CHECK(palette.Open(9));
    const auto taken = palette.Insert(many, 300, 9);
    CHECK(taken < 300 && palette.Full() && palette.Text().size() <= CommandPalette::kMaxTextBytes && WellFormed(palette.Text()));
    // Closing ends a composition; composing while closed is ignored.
    CHECK(palette.Open(10)); palette.Compose(u"か", 1, 10); palette.Close(11); CHECK(palette.Composition().empty());
    palette.Compose(u"か", 1, 12); CHECK(palette.Composition().empty());
}
void Motion() {
    CommandPalette palette; palette.Configure(storage);
    palette.Layout(1280, 720, 1);
    auto u = palette.Uniforms();
    CHECK(u.center_x == 640 && u.center_y == 273 && u.scale == 1 && u.half_width == 320 && u.half_height == 28 && u.visibility == 0);
    auto lane = palette.TextRect();
    CHECK(lane.x == 372 && lane.y == 245 && lane.width == 524 && lane.height == 56 && lane.font_pixels == 22);
    CHECK(palette.Hit(640, 273) && palette.Hit(330, 250) && !palette.Hit(640, 310) && !palette.Hit(300, 273));
    // Every window size keeps the bar inside the window, whole pixels for its text.
    const unsigned sizes[][3]{{320, 180, 100}, {640, 360, 100}, {960, 540, 125}, {1920, 1080, 100}, {3840, 2160, 200}, {540, 960, 100}, {2560, 1080, 150}, {400, 2000, 100}};
    for (const auto& size : sizes) {
        palette.Layout(size[0], size[1], static_cast<float>(size[2]) / 100);
        u = palette.Uniforms(); lane = palette.TextRect();
        const float left = u.center_x - u.half_width * u.scale, right = u.center_x + u.half_width * u.scale;
        const float top = u.center_y - u.half_height * u.scale, bottom = u.center_y + u.half_height * u.scale;
        CHECK(left >= 0 && right <= static_cast<float>(size[0]) && top >= 0 && bottom <= static_cast<float>(size[1]));
        CHECK(lane.x > static_cast<int>(left) && lane.x + static_cast<int>(lane.width) < static_cast<int>(right) && lane.width >= 80 && lane.height >= 40);
        CHECK(lane.font_pixels == 22 * u.scale);
    }
    palette.Layout(1280, 720, 1);
    // Opening: visibility rises without overshoot and settles on exactly 1 within a quarter of a second.
    std::int64_t now = 10000000; palette.Advance(now);
    CHECK(!palette.Visible() && !palette.Animating());
    CHECK(palette.Open(now) && palette.Animating());
    float previous = 0; bool monotonic = true; unsigned frames = 0;
    float early = 0;
    for (; frames < 60 && palette.Uniforms().visibility < 1; ++frames) {
        now += kTick; palette.Advance(now);
        monotonic = monotonic && palette.Uniforms().visibility >= previous && palette.Uniforms().visibility <= 1; previous = palette.Uniforms().visibility;
        if (frames == 11) early = previous; // A tenth of a second in.
    }
    CHECK(monotonic && palette.Uniforms().visibility == 1 && frames <= 30 && frames > 3 && early > 0.9f && palette.Visible());
    // The caret is steady while keys arrive and blinks once they rest.
    CHECK(palette.Utf16('a', now)); palette.Advance(now); CHECK(palette.Uniforms().caret == 1);
    palette.Advance(now + 4000000); CHECK(palette.Uniforms().caret == 1);
    palette.Advance(now + 8000000); CHECK(palette.Uniforms().caret == 0);
    palette.Advance(now + 12000000); CHECK(palette.Uniforms().caret == 1);
    now += 12000000; CHECK(palette.Utf16('b', now)); palette.Advance(now); CHECK(palette.Uniforms().caret == 1);
    // Hints reach the uniforms; the counter saturates.
    palette.SetHint(PaletteHint::Search, 7); CHECK(palette.Uniforms().hint == 2 && palette.Uniforms().matches == 7 && palette.Hint() == PaletteHint::Search);
    palette.SetHint(PaletteHint::Search, 123456); CHECK(palette.Uniforms().matches == 999);
    palette.SetHint(PaletteHint::Command, 0); CHECK(palette.Uniforms().hint == 1);
    // A refusal: the bar stays open, tints and shakes within bounds, and comes to rest.
    palette.Rejected(now); CHECK(palette.IsOpen() && palette.Uniforms().denied == 1);
    float largest = 0; bool bounded = true;
    for (frames = 0; frames < 360 && (palette.Uniforms().denied > 0 || frames == 0); ++frames) {
        now += kTick; palette.Advance(now);
        largest = (std::max)(largest, std::fabs(palette.Uniforms().shake)); bounded = bounded && std::fabs(palette.Uniforms().shake) <= 7.0f;
    }
    CHECK(bounded && largest > 2 && palette.Uniforms().denied == 0 && palette.Uniforms().shake == 0 && frames < 360 && palette.Text() == "ab");
    // Closing: falls without undershoot to exactly 0; the text is kept until then.
    palette.Close(now); previous = 1; monotonic = true;
    for (frames = 0; frames < 60 && palette.Uniforms().visibility > 0; ++frames) {
        now += kTick; palette.Advance(now);
        monotonic = monotonic && palette.Uniforms().visibility <= previous && palette.Uniforms().visibility >= 0; previous = palette.Uniforms().visibility;
    }
    CHECK(monotonic && palette.Uniforms().visibility == 0 && frames <= 30 && !palette.Visible() && !palette.Animating() && palette.Uniforms().caret == 0 && palette.Text() == "ab");
    // Reduced motion: no glide, no shake.
    palette.SetReducedMotion(true);
    CHECK(palette.Open(now)); now += kTick; palette.Advance(now); CHECK(palette.Uniforms().visibility == 1);
    palette.Rejected(now); now += kTick; palette.Advance(now); CHECK(palette.Uniforms().shake == 0 && palette.Uniforms().denied == 1);
    palette.Advance(now + 6000000); CHECK(palette.Uniforms().denied == 0);
    palette.Close(now + 6000000); palette.Advance(now + 6000000 + kTick); CHECK(palette.Uniforms().visibility == 0);
    // Time running backwards (a stale timestamp) changes nothing.
    const auto before = palette.Uniforms(); palette.Advance(5); CHECK(std::memcmp(&before, &palette.Uniforms(), sizeof(before)) == 0);
}
}
int main() {
    allocation_probe::Begin();
    Typing(); Composing(); Motion();
    const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
    std::printf("Command palette model: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
