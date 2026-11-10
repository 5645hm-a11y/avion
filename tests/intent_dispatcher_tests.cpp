// media::ai::LocalIntentDispatcher: every command of the grammar becomes the expected typed action with the expected
// parameters; malformed, out-of-range and unavailable commands are refused with the expected reason; parsing is
// deterministic and allocation-free, also on random input. Follow-ups ("next match", "the second one", "undo", "again",
// "את השני", "בטל", "עוד פעם") resolve through a ConversationState against an engine stub that reports its jumps.
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/UtteranceBuffer.hpp"
#include "allocation_probe.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
using namespace media::ai;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(false)
using Dispatcher = LocalIntentDispatcher;
constexpr std::int64_t S = Dispatcher::kSecond;
// A two-hour film, ten minutes in, at half volume, with subtitles.
constexpr MediaBounds kFilm{7200 * S, 600 * S, 0.5f, false, false, true, true};

struct Case { const char* text; IntentKind kind; std::int64_t target; float volume; const char* query; };
constexpr Case kAccepted[]{
    // Transport.
    {"play", IntentKind::Play, 0, 0, ""}, {"Play", IntentKind::Play, 0, 0, ""}, {"PLAY!", IntentKind::Play, 0, 0, ""}, {"resume", IntentKind::Play, 0, 0, ""},
    {"continue", IntentKind::Play, 0, 0, ""}, {"please play", IntentKind::Play, 0, 0, ""}, {"  play the video  ", IntentKind::Play, 0, 0, ""},
    {"נגן", IntentKind::Play, 0, 0, ""}, {"תנגן", IntentKind::Play, 0, 0, ""}, {"המשך", IntentKind::Play, 0, 0, ""}, {"הפעל את הסרט", IntentKind::Play, 0, 0, ""},
    {"נגן בבקשה", IntentKind::Play, 0, 0, ""}, {"נַגֵּן", IntentKind::Play, 0, 0, ""},
    {"pause", IntentKind::Pause, 0, 0, ""}, {"Pause.", IntentKind::Pause, 0, 0, ""}, {"stop", IntentKind::Pause, 0, 0, ""}, {"hold", IntentKind::Pause, 0, 0, ""},
    {"pause the movie please", IntentKind::Pause, 0, 0, ""}, {"pause now", IntentKind::Pause, 0, 0, ""},
    {"עצור", IntentKind::Pause, 0, 0, ""}, {"תעצור", IntentKind::Pause, 0, 0, ""}, {"השהה", IntentKind::Pause, 0, 0, ""}, {"עצור את הסרט", IntentKind::Pause, 0, 0, ""},
    {"\xe2\x80\x8f" "עצור" "\xe2\x80\x8f", IntentKind::Pause, 0, 0, ""}, // Wrapped in right-to-left marks, as an input field may send it.
    // Volume: one 5 % step, or an amount in points.
    {"volume up", IntentKind::VolumeUp, 0, 0.55f, ""}, {"louder", IntentKind::VolumeUp, 0, 0.55f, ""}, {"turn it up", IntentKind::VolumeUp, 0, 0.55f, ""},
    {"turn the volume up", IntentKind::VolumeUp, 0, 0.55f, ""}, {"increase the volume", IntentKind::VolumeUp, 0, 0.55f, ""},
    {"הגבר", IntentKind::VolumeUp, 0, 0.55f, ""}, {"תגביר", IntentKind::VolumeUp, 0, 0.55f, ""}, {"הגבר את העוצמה", IntentKind::VolumeUp, 0, 0.55f, ""},
    {"יותר חזק", IntentKind::VolumeUp, 0, 0.55f, ""}, {"volume up 20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"volume up by 20%", IntentKind::VolumeUp, 0, 0.70f, ""},
    {"הגבר ב-20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"הגבר ב 20 אחוז", IntentKind::VolumeUp, 0, 0.70f, ""}, {"הגבר ב־20", IntentKind::VolumeUp, 0, 0.70f, ""},
    {"louder 80", IntentKind::VolumeUp, 0, 1.0f, ""},
    {"volume down", IntentKind::VolumeDown, 0, 0.45f, ""}, {"quieter", IntentKind::VolumeDown, 0, 0.45f, ""}, {"turn down the volume", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"lower the volume", IntentKind::VolumeDown, 0, 0.45f, ""}, {"תנמיך", IntentKind::VolumeDown, 0, 0.45f, ""}, {"הנמך", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"החלש", IntentKind::VolumeDown, 0, 0.45f, ""}, {"תנמיך את הקול", IntentKind::VolumeDown, 0, 0.45f, ""}, {"שקט יותר", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"volume down 30", IntentKind::VolumeDown, 0, 0.20f, ""}, {"תנמיך ב-70", IntentKind::VolumeDown, 0, 0.0f, ""},
    {"volume 50", IntentKind::VolumeSet, 0, 0.50f, ""}, {"volume 35%", IntentKind::VolumeSet, 0, 0.35f, ""}, {"set volume to 80", IntentKind::VolumeSet, 0, 0.80f, ""},
    {"set the volume to 5 percent", IntentKind::VolumeSet, 0, 0.05f, ""}, {"volume 0", IntentKind::VolumeSet, 0, 0.0f, ""}, {"volume 100", IntentKind::VolumeSet, 0, 1.0f, ""},
    {"עוצמה 30", IntentKind::VolumeSet, 0, 0.30f, ""}, {"ווליום 75", IntentKind::VolumeSet, 0, 0.75f, ""}, {"עוצמה ל-60", IntentKind::VolumeSet, 0, 0.60f, ""},
    {"עוצמת קול 20 אחוז", IntentKind::VolumeSet, 0, 0.20f, ""},
    {"mute", IntentKind::Mute, 0, 0, ""}, {"Mute!", IntentKind::Mute, 0, 0, ""}, {"mute the sound", IntentKind::Mute, 0, 0, ""}, {"השתק", IntentKind::Mute, 0, 0, ""},
    {"תשתיק", IntentKind::Mute, 0, 0, ""}, {"השתק את הקול", IntentKind::Mute, 0, 0, ""},
    {"unmute", IntentKind::Unmute, 0, 0, ""}, {"sound on", IntentKind::Unmute, 0, 0, ""}, {"בטל השתקה", IntentKind::Unmute, 0, 0, ""}, {"החזר את הקול", IntentKind::Unmute, 0, 0, ""},
    // Relative seeks from 10:00; back past the start is the start.
    {"rewind 30s", IntentKind::SeekRelative, 570 * S, 0, ""}, {"rewind 30 s", IntentKind::SeekRelative, 570 * S, 0, ""}, {"rewind 30 seconds", IntentKind::SeekRelative, 570 * S, 0, ""},
    {"rewind", IntentKind::SeekRelative, 590 * S, 0, ""}, {"back 5", IntentKind::SeekRelative, 595 * S, 0, ""}, {"go back 2 minutes", IntentKind::SeekRelative, 480 * S, 0, ""},
    {"skip back 1m30s", IntentKind::SeekRelative, 510 * S, 0, ""}, {"rewind 1:30", IntentKind::SeekRelative, 510 * S, 0, ""}, {"rewind a minute", IntentKind::SeekRelative, 540 * S, 0, ""},
    {"rewind a minute and a half", IntentKind::SeekRelative, 510 * S, 0, ""}, {"rewind half a minute", IntentKind::SeekRelative, 570 * S, 0, ""},
    {"rewind one minute and 30 seconds", IntentKind::SeekRelative, 510 * S, 0, ""}, {"rewind 20 minutes", IntentKind::SeekRelative, 0, 0, ""},
    {"30 seconds back", IntentKind::SeekRelative, 570 * S, 0, ""}, {"rewind by 15 sec please", IntentKind::SeekRelative, 585 * S, 0, ""},
    {"חזור 10 שניות", IntentKind::SeekRelative, 590 * S, 0, ""}, {"חזור 10 שניות אחורה", IntentKind::SeekRelative, 590 * S, 0, ""}, {"תחזור דקה", IntentKind::SeekRelative, 540 * S, 0, ""},
    {"אחורה חצי דקה", IntentKind::SeekRelative, 570 * S, 0, ""}, {"10 שניות אחורה", IntentKind::SeekRelative, 590 * S, 0, ""}, {"חזור שתי דקות", IntentKind::SeekRelative, 480 * S, 0, ""},
    {"חזור דקה וחצי", IntentKind::SeekRelative, 510 * S, 0, ""}, {"חזור דקה ו-30 שניות", IntentKind::SeekRelative, 510 * S, 0, ""}, {"חזור שנייה", IntentKind::SeekRelative, 599 * S, 0, ""},
    {"חזור דקה אחת", IntentKind::SeekRelative, 540 * S, 0, ""}, {"חזור", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"forward 1 minute", IntentKind::SeekRelative, 660 * S, 0, ""}, {"קדימה דקה", IntentKind::SeekRelative, 660 * S, 0, ""}, {"קדימה", IntentKind::SeekRelative, 610 * S, 0, ""},
    {"skip 45 seconds", IntentKind::SeekRelative, 645 * S, 0, ""}, {"skip 45 seconds back", IntentKind::SeekRelative, 555 * S, 0, ""}, {"fast forward 2 min", IntentKind::SeekRelative, 720 * S, 0, ""},
    {"דלג 20 שניות", IntentKind::SeekRelative, 620 * S, 0, ""}, {"דלג 20 שניות אחורה", IntentKind::SeekRelative, 580 * S, 0, ""}, {"קפוץ 30 שניות קדימה", IntentKind::SeekRelative, 630 * S, 0, ""},
    {"דקה קדימה", IntentKind::SeekRelative, 660 * S, 0, ""}, {"forward 1 hour", IntentKind::SeekRelative, 4200 * S, 0, ""}, {"קדימה שעה", IntentKind::SeekRelative, 4200 * S, 0, ""},
    {"קדימה רבע שעה", IntentKind::SeekRelative, 1500 * S, 0, ""}, {"forward 1h2m3s", IntentKind::SeekRelative, 4323 * S, 0, ""}, {"התקדם 5 דקות", IntentKind::SeekRelative, 900 * S, 0, ""},
    {"forward 109 minutes 59 seconds", IntentKind::SeekRelative, 7199 * S, 0, ""}, {"קדימה שעה וחצי", IntentKind::SeekRelative, 6000 * S, 0, ""},
    // Absolute seeks.
    {"go to 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"go to 1:02:03", IntentKind::SeekAbsolute, 3723 * S, 0, ""}, {"seek to 90", IntentKind::SeekAbsolute, 90 * S, 0, ""},
    {"seek 90s", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"jump to 5 minutes", IntentKind::SeekAbsolute, 300 * S, 0, ""}, {"go to 0", IntentKind::SeekAbsolute, 0, 0, ""},
    {"go to 1:59:59", IntentKind::SeekAbsolute, 7199 * S, 0, ""}, {"קפוץ ל-1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"עבור ל 2:00", IntentKind::SeekAbsolute, 120 * S, 0, ""},
    {"לך ל10 דקות", IntentKind::SeekAbsolute, 600 * S, 0, ""}, {"restart", IntentKind::SeekAbsolute, 0, 0, ""}, {"start over", IntentKind::SeekAbsolute, 0, 0, ""},
    {"go to the beginning", IntentKind::SeekAbsolute, 0, 0, ""}, {"מההתחלה", IntentKind::SeekAbsolute, 0, 0, ""}, {"חזור להתחלה", IntentKind::SeekAbsolute, 0, 0, ""},
    // Quote search: the quote is the rest of the command, as typed.
    {"find hello world", IntentKind::FindSubtitle, 0, 0, "hello world"}, {"find \"To be, or not\"", IntentKind::FindSubtitle, 0, 0, "To be, or not"},
    {"Find   spaced  text ", IntentKind::FindSubtitle, 0, 0, "spaced  text"}, {"search for the quiet", IntentKind::FindSubtitle, 0, 0, "the quiet"},
    {"מצא שלום עולם", IntentKind::FindSubtitle, 0, 0, "שלום עולם"}, {"חפש את האור", IntentKind::FindSubtitle, 0, 0, "את האור"},
    {"מצא ״שלום״", IntentKind::FindSubtitle, 0, 0, "שלום"}, {"find find", IntentKind::FindSubtitle, 0, 0, "find"}, {"find 日本語", IntentKind::FindSubtitle, 0, 0, "日本語"},
    {"find please", IntentKind::FindSubtitle, 0, 0, "please"}, {"please find rewind 10", IntentKind::FindSubtitle, 0, 0, "rewind 10"},
    {"find one two three four five six seven eight nine ten eleven twelve thirteen fourteen", IntentKind::FindSubtitle, 0, 0,
     "one two three four five six seven eight nine ten eleven twelve thirteen fourteen"},
};
// The same actions in the other languages of the grammar, from 10:00 of the two-hour film at half volume.
constexpr Case kArabic[]{
    {"توقف", IntentKind::Pause, 0, 0, ""}, {"إيقاف", IntentKind::Pause, 0, 0, ""}, {"أوقف الفيديو", IntentKind::Pause, 0, 0, ""}, {"توقف من فضلك", IntentKind::Pause, 0, 0, ""},
    {"شغل", IntentKind::Play, 0, 0, ""}, {"شَغِّل", IntentKind::Play, 0, 0, ""}, {"تشغيل", IntentKind::Play, 0, 0, ""}, // The second one carries harakat.
    {"ارجع", IntentKind::SeekRelative, 590 * S, 0, ""}, {"ارجع 10 ثواني", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"ارجع ٣٠ ثانية", IntentKind::SeekRelative, 570 * S, 0, ""}, {"ارجع دقيقة", IntentKind::SeekRelative, 540 * S, 0, ""},
    {"ارجع دقيقتين", IntentKind::SeekRelative, 480 * S, 0, ""}, {"تقدم دقيقة و 30 ثانية", IntentKind::SeekRelative, 690 * S, 0, ""},
    {"تقدم دقيقة و30 ثانية", IntentKind::SeekRelative, 690 * S, 0, ""}, {"10 ثواني للخلف", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"تقدم نصف ساعة", IntentKind::SeekRelative, 2400 * S, 0, ""}, {"اذهب إلى 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""},
    {"من البداية", IntentKind::SeekAbsolute, 0, 0, ""},
    {"ابحث عن مرحبا", IntentKind::FindSubtitle, 0, 0, "مرحبا"}, {"ابحث عن العربية", IntentKind::FindSubtitle, 0, 0, "العربية"},
    {"ارفع الصوت", IntentKind::VolumeUp, 0, 0.55f, ""}, {"ارفع الصوت 20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"اخفض الصوت", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"الصوت 30", IntentKind::VolumeSet, 0, 0.30f, ""}, {"الصوت ٣٠٪", IntentKind::VolumeSet, 0, 0.30f, ""}, {"الصوت 25 بالمئة", IntentKind::VolumeSet, 0, 0.25f, ""},
    {"كتم الصوت", IntentKind::Mute, 0, 0, ""}, {"إلغاء الكتم", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kChinese[]{
    {"暂停", IntentKind::Pause, 0, 0, ""}, {"暫停", IntentKind::Pause, 0, 0, ""}, {"请暂停", IntentKind::Pause, 0, 0, ""}, {"暂停。", IntentKind::Pause, 0, 0, ""},
    {"播放", IntentKind::Play, 0, 0, ""}, {"继续播放", IntentKind::Play, 0, 0, ""}, {"繼續", IntentKind::Play, 0, 0, ""},
    {"快退", IntentKind::SeekRelative, 590 * S, 0, ""}, {"快退10秒", IntentKind::SeekRelative, 590 * S, 0, ""}, {"快退 10 秒", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"快退十秒", IntentKind::SeekRelative, 590 * S, 0, ""}, {"快进三十秒", IntentKind::SeekRelative, 630 * S, 0, ""}, {"快進三十秒", IntentKind::SeekRelative, 630 * S, 0, ""},
    {"后退一分钟", IntentKind::SeekRelative, 540 * S, 0, ""}, {"快进1分30秒", IntentKind::SeekRelative, 690 * S, 0, ""}, {"快进两分钟", IntentKind::SeekRelative, 720 * S, 0, ""},
    {"快退半分钟", IntentKind::SeekRelative, 570 * S, 0, ""}, {"快进一分半", IntentKind::SeekRelative, 690 * S, 0, ""}, {"快进１０秒", IntentKind::SeekRelative, 610 * S, 0, ""},
    {"前进一个小时", IntentKind::SeekRelative, 4200 * S, 0, ""}, {"快进一百二十秒", IntentKind::SeekRelative, 720 * S, 0, ""},
    {"跳到1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"跳到 5分钟", IntentKind::SeekAbsolute, 300 * S, 0, ""}, {"重新开始", IntentKind::SeekAbsolute, 0, 0, ""},
    {"從頭開始", IntentKind::SeekAbsolute, 0, 0, ""},
    {"查找你好", IntentKind::FindSubtitle, 0, 0, "你好"}, {"查找 你好 世界", IntentKind::FindSubtitle, 0, 0, "你好 世界"}, {"搜索「你好」", IntentKind::FindSubtitle, 0, 0, "你好"},
    {"尋找中文", IntentKind::FindSubtitle, 0, 0, "中文"},
    {"增大音量", IntentKind::VolumeUp, 0, 0.55f, ""}, {"調高音量", IntentKind::VolumeUp, 0, 0.55f, ""}, {"增大音量20", IntentKind::VolumeUp, 0, 0.70f, ""},
    {"减小音量10", IntentKind::VolumeDown, 0, 0.40f, ""}, {"音量50", IntentKind::VolumeSet, 0, 0.50f, ""}, {"音量 80%", IntentKind::VolumeSet, 0, 0.80f, ""},
    {"音量调到30", IntentKind::VolumeSet, 0, 0.30f, ""}, {"静音", IntentKind::Mute, 0, 0, ""}, {"取消静音", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kJapanese[]{
    {"一時停止", IntentKind::Pause, 0, 0, ""}, {"一時停止してください", IntentKind::Pause, 0, 0, ""}, {"ポーズ", IntentKind::Pause, 0, 0, ""},
    {"再生", IntentKind::Play, 0, 0, ""}, {"再生して", IntentKind::Play, 0, 0, ""}, {"再生 お願いします", IntentKind::Play, 0, 0, ""},
    {"巻き戻し", IntentKind::SeekRelative, 590 * S, 0, ""}, {"10秒巻き戻し", IntentKind::SeekRelative, 590 * S, 0, ""}, {"10秒戻る", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"30秒進む", IntentKind::SeekRelative, 630 * S, 0, ""}, {"1分30秒戻って", IntentKind::SeekRelative, 510 * S, 0, ""}, {"巻き戻し 30秒", IntentKind::SeekRelative, 570 * S, 0, ""},
    {"早送り", IntentKind::SeekRelative, 610 * S, 0, ""}, {"10秒 戻る", IntentKind::SeekRelative, 590 * S, 0, ""}, {"1時間進む", IntentKind::SeekRelative, 4200 * S, 0, ""},
    {"三十秒戻してください", IntentKind::SeekRelative, 570 * S, 0, ""},
    {"1:30に移動", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"5分に移動", IntentKind::SeekAbsolute, 300 * S, 0, ""}, {"最初から", IntentKind::SeekAbsolute, 0, 0, ""},
    {"検索 こんにちは", IntentKind::FindSubtitle, 0, 0, "こんにちは"}, {"こんにちはを検索", IntentKind::FindSubtitle, 0, 0, "こんにちは"},
    {"日本語を探して", IntentKind::FindSubtitle, 0, 0, "日本語"},
    {"音量を上げる", IntentKind::VolumeUp, 0, 0.55f, ""}, {"音量を下げて", IntentKind::VolumeDown, 0, 0.45f, ""}, {"音量を50に", IntentKind::VolumeSet, 0, 0.50f, ""},
    {"音量 30", IntentKind::VolumeSet, 0, 0.30f, ""}, {"音量25パーセント", IntentKind::VolumeSet, 0, 0.25f, ""}, {"ミュート", IntentKind::Mute, 0, 0, ""},
    {"ミュート解除", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kRussian[]{
    {"пауза", IntentKind::Pause, 0, 0, ""}, {"Пауза", IntentKind::Pause, 0, 0, ""}, {"стоп", IntentKind::Pause, 0, 0, ""}, {"пауза, пожалуйста", IntentKind::Pause, 0, 0, ""},
    {"воспроизведение", IntentKind::Play, 0, 0, ""}, {"продолжай", IntentKind::Play, 0, 0, ""}, {"ВОСПРОИЗВЕСТИ", IntentKind::Play, 0, 0, ""},
    {"назад", IntentKind::SeekRelative, 590 * S, 0, ""}, {"назад 10 секунд", IntentKind::SeekRelative, 590 * S, 0, ""}, {"назад на 30 секунд", IntentKind::SeekRelative, 570 * S, 0, ""},
    {"вперёд на минуту", IntentKind::SeekRelative, 660 * S, 0, ""}, {"вперед на полторы минуты", IntentKind::SeekRelative, 690 * S, 0, ""},
    {"10 секунд назад", IntentKind::SeekRelative, 590 * S, 0, ""}, {"перемотай на 10 секунд назад", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"перемотай вперёд на 2 минуты", IntentKind::SeekRelative, 720 * S, 0, ""}, {"назад 10с", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"вперед полчаса", IntentKind::SeekRelative, 2400 * S, 0, ""}, {"назад минуту и 30 секунд", IntentKind::SeekRelative, 510 * S, 0, ""},
    {"перейди на 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"сначала", IntentKind::SeekAbsolute, 0, 0, ""},
    {"найти привет мир", IntentKind::FindSubtitle, 0, 0, "привет мир"}, {"Найди привет", IntentKind::FindSubtitle, 0, 0, "привет"},
    {"громче", IntentKind::VolumeUp, 0, 0.55f, ""}, {"громче на 20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"тише", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"громкость 40", IntentKind::VolumeSet, 0, 0.40f, ""}, {"громкость 40 процентов", IntentKind::VolumeSet, 0, 0.40f, ""},
    {"выключи звук", IntentKind::Mute, 0, 0, ""}, {"включи звук", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kSpanish[]{
    {"pausar", IntentKind::Pause, 0, 0, ""}, {"Pausa", IntentKind::Pause, 0, 0, ""}, {"pausar el video por favor", IntentKind::Pause, 0, 0, ""}, {"detén", IntentKind::Pause, 0, 0, ""},
    {"reproducir", IntentKind::Play, 0, 0, ""}, {"por favor, reproduce", IntentKind::Play, 0, 0, ""}, {"¡Reproducir!", IntentKind::Play, 0, 0, ""},
    {"retroceder", IntentKind::SeekRelative, 590 * S, 0, ""}, {"retroceder 10 segundos", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"retrocede 30 s", IntentKind::SeekRelative, 570 * S, 0, ""}, {"avanzar 1 minuto", IntentKind::SeekRelative, 660 * S, 0, ""},
    {"avanzar minuto y medio", IntentKind::SeekRelative, 690 * S, 0, ""}, {"10 segundos atrás", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"adelantar media hora", IntentKind::SeekRelative, 2400 * S, 0, ""}, {"salta 20 segundos atrás", IntentKind::SeekRelative, 580 * S, 0, ""},
    {"retroceder un minuto y 30 segundos", IntentKind::SeekRelative, 510 * S, 0, ""}, {"retroceder dos minutos", IntentKind::SeekRelative, 480 * S, 0, ""},
    {"ir a 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"desde el principio", IntentKind::SeekAbsolute, 0, 0, ""}, {"reiniciar", IntentKind::SeekAbsolute, 0, 0, ""},
    {"buscar hola mundo", IntentKind::FindSubtitle, 0, 0, "hola mundo"}, {"Busca ¿dónde estás?", IntentKind::FindSubtitle, 0, 0, "¿dónde estás?"},
    {"subir el volumen", IntentKind::VolumeUp, 0, 0.55f, ""}, {"más alto", IntentKind::VolumeUp, 0, 0.55f, ""}, {"sube el volumen 20", IntentKind::VolumeUp, 0, 0.70f, ""},
    {"bajar volumen", IntentKind::VolumeDown, 0, 0.45f, ""}, {"volumen 40", IntentKind::VolumeSet, 0, 0.40f, ""},
    {"volumen al 35 por ciento", IntentKind::VolumeSet, 0, 0.35f, ""}, {"silenciar", IntentKind::Mute, 0, 0, ""}, {"activar sonido", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kFrench[]{
    {"pause", IntentKind::Pause, 0, 0, ""}, {"Arrête", IntentKind::Pause, 0, 0, ""}, {"mettre en pause", IntentKind::Pause, 0, 0, ""}, {"mets en pause la lecture", IntentKind::Pause, 0, 0, ""},
    {"lecture", IntentKind::Play, 0, 0, ""}, {"lecture s'il vous plaît", IntentKind::Play, 0, 0, ""}, {"Reprendre", IntentKind::Play, 0, 0, ""},
    {"reculer", IntentKind::SeekRelative, 590 * S, 0, ""}, {"reculer de 10 secondes", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"recule de 30 s", IntentKind::SeekRelative, 570 * S, 0, ""}, {"avancer de 2 minutes", IntentKind::SeekRelative, 720 * S, 0, ""},
    {"10 secondes en arrière", IntentKind::SeekRelative, 590 * S, 0, ""}, {"reculer une minute et demie", IntentKind::SeekRelative, 510 * S, 0, ""},
    {"avance rapide 1 minute", IntentKind::SeekRelative, 660 * S, 0, ""}, {"reculer une demi-heure", IntentKind::SeekRelative, 0, 0, ""},
    {"avancer une heure", IntentKind::SeekRelative, 4200 * S, 0, ""},
    {"aller à 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"va à 2 minutes", IntentKind::SeekAbsolute, 120 * S, 0, ""}, {"recommencer", IntentKind::SeekAbsolute, 0, 0, ""},
    {"depuis le début", IntentKind::SeekAbsolute, 0, 0, ""},
    {"chercher bonjour le monde", IntentKind::FindSubtitle, 0, 0, "bonjour le monde"}, {"Cherche «bonjour»", IntentKind::FindSubtitle, 0, 0, "bonjour"},
    {"monter le volume", IntentKind::VolumeUp, 0, 0.55f, ""}, {"plus fort", IntentKind::VolumeUp, 0, 0.55f, ""}, {"monte le volume de 20", IntentKind::VolumeUp, 0, 0.70f, ""},
    {"baisser le volume", IntentKind::VolumeDown, 0, 0.45f, ""}, {"moins fort", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"volume à 60 pour cent", IntentKind::VolumeSet, 0, 0.60f, ""}, {"couper le son", IntentKind::Mute, 0, 0, ""}, {"remettre le son", IntentKind::Unmute, 0, 0, ""},
};
constexpr Case kGerman[]{
    {"Pause", IntentKind::Pause, 0, 0, ""}, {"Anhalten", IntentKind::Pause, 0, 0, ""}, {"pausieren bitte", IntentKind::Pause, 0, 0, ""},
    {"abspielen", IntentKind::Play, 0, 0, ""}, {"bitte abspielen", IntentKind::Play, 0, 0, ""}, {"weiter", IntentKind::Play, 0, 0, ""}, {"spiel ab", IntentKind::Play, 0, 0, ""},
    {"zurückspulen", IntentKind::SeekRelative, 590 * S, 0, ""}, {"zurückspulen 10 Sekunden", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"ZURÜCKSPULEN 30 SEK", IntentKind::SeekRelative, 570 * S, 0, ""}, {"zuru\xcc\x88" "ckspulen", IntentKind::SeekRelative, 590 * S, 0, ""}, // u + combining diaeresis.
    {"zuruckspulen", IntentKind::SeekRelative, 590 * S, 0, ""}, {"10 Sekunden zurück", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"spule 20 sekunden zurück", IntentKind::SeekRelative, 580 * S, 0, ""}, {"vorspulen 1 Minute", IntentKind::SeekRelative, 660 * S, 0, ""},
    {"vorspulen eine halbe Stunde", IntentKind::SeekRelative, 2400 * S, 0, ""}, {"vor 2 min", IntentKind::SeekRelative, 720 * S, 0, ""},
    {"zurück 1 Minute und 30 Sekunden", IntentKind::SeekRelative, 510 * S, 0, ""}, {"vorspulen fünf Minuten", IntentKind::SeekRelative, 900 * S, 0, ""},
    {"gehe zu 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"springe zu 5 Minuten", IntentKind::SeekAbsolute, 300 * S, 0, ""}, {"von vorne", IntentKind::SeekAbsolute, 0, 0, ""},
    {"suche hallo welt", IntentKind::FindSubtitle, 0, 0, "hallo welt"}, {"suche nach hallo", IntentKind::FindSubtitle, 0, 0, "hallo"},
    {"lauter", IntentKind::VolumeUp, 0, 0.55f, ""}, {"lauter um 20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"leiser", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"Lautstärke 40", IntentKind::VolumeSet, 0, 0.40f, ""}, {"Lautstärke auf 40 Prozent", IntentKind::VolumeSet, 0, 0.40f, ""},
    {"stumm", IntentKind::Mute, 0, 0, ""}, {"Ton an", IntentKind::Unmute, 0, 0, ""},
};
// Other spellings of the same text: fullwidth forms, accents, digits of other scripts.
constexpr Case kNormalized[]{
    {"ＰＡＵＳＥ", IntentKind::Pause, 0, 0, ""}, {"PAUSÁR", IntentKind::Pause, 0, 0, ""}, {"volume ５０％", IntentKind::VolumeSet, 0, 0.50f, ""},
    {"rewind ۳۰ seconds", IntentKind::SeekRelative, 570 * S, 0, ""}, {"חזור ١٠ שניות", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"go to 1：30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"„pause“", IntentKind::Pause, 0, 0, ""}, {"Résumé", IntentKind::Play, 0, 0, ""},
    {"10s back", IntentKind::SeekRelative, 590 * S, 0, ""}, {"10 שניות אחורה", IntentKind::SeekRelative, 590 * S, 0, ""}, {"10秒 戻る", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"retroceder 10 segundos", IntentKind::SeekRelative, 590 * S, 0, ""}, {"назад 10 секунд", IntentKind::SeekRelative, 590 * S, 0, ""},
};
// Continuous, conversational speech: the command inside a request frame, courtesy, hesitations and softeners, as people
// say it to someone in the room (from 10:00 of the two-hour film at half volume).
constexpr Case kConversation[]{
    // English.
    {"Can you please pause the movie", IntentKind::Pause, 0, 0, ""}, {"could you pause it for me", IntentKind::Pause, 0, 0, ""},
    {"would you mind pausing the video", IntentKind::Pause, 0, 0, ""}, {"hey, can you rewind 10 seconds?", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"I'd like you to skip forward 30 seconds", IntentKind::SeekRelative, 630 * S, 0, ""},
    {"i\xe2\x80\x99" "d like you to skip forward 30 seconds", IntentKind::SeekRelative, 630 * S, 0, ""}, // Typographic apostrophe.
    {"I want you to go back two minutes please", IntentKind::SeekRelative, 480 * S, 0, ""}, {"Turn up the volume a bit", IntentKind::VolumeUp, 0, 0.55f, ""},
    {"turn up the volume a bit please", IntentKind::VolumeUp, 0, 0.55f, ""}, {"could you turn it down a little", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"would you mind turning up the volume", IntentKind::VolumeUp, 0, 0.55f, ""}, {"please go back a bit", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"i'd like to go back a little", IntentKind::SeekRelative, 590 * S, 0, ""}, {"ok so can you play the movie now", IntentKind::Play, 0, 0, ""},
    {"um, could you mute the sound please", IntentKind::Mute, 0, 0, ""}, {"let's start over", IntentKind::SeekAbsolute, 0, 0, ""},
    {"go ahead and skip ahead 5 minutes", IntentKind::SeekRelative, 900 * S, 0, ""}, {"would you please set the volume to 30 percent", IntentKind::VolumeSet, 0, 0.30f, ""},
    {"can you jump to 1:30", IntentKind::SeekAbsolute, 90 * S, 0, ""}, {"i want to find the quiet", IntentKind::FindSubtitle, 0, 0, "the quiet"},
    {"rewind the movie 10 seconds", IntentKind::SeekRelative, 590 * S, 0, ""}, {"could you rewind it a little bit", IntentKind::SeekRelative, 590 * S, 0, ""},
    {"a little louder please", IntentKind::VolumeUp, 0, 0.55f, ""}, {"just pause it right now thank you", IntentKind::Pause, 0, 0, ""},
    // Hebrew.
    {"אני רוצה שתעצור עכשיו", IntentKind::Pause, 0, 0, ""}, {"אשמח שתמשיך 10 שניות קדימה", IntentKind::SeekRelative, 610 * S, 0, ""},
    {"תוכל להגביר קצת את הווליום", IntentKind::VolumeUp, 0, 0.55f, ""}, {"תחזיר דקה אחורה בבקשה", IntentKind::SeekRelative, 540 * S, 0, ""},
    {"אשמח אם תעצור את הסרט", IntentKind::Pause, 0, 0, ""}, {"אתה יכול להנמיך את הקול בבקשה", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"תוכל לחזור 30 שניות אחורה", IntentKind::SeekRelative, 570 * S, 0, ""}, {"יאללה תמשיך", IntentKind::Play, 0, 0, ""},
    {"תעשה לי טובה ותשתיק", IntentKind::Mute, 0, 0, ""}, {"תקפוץ ל-1:30 בבקשה", IntentKind::SeekAbsolute, 90 * S, 0, ""},
    {"הייתי רוצה שתגביר ב-20", IntentKind::VolumeUp, 0, 0.70f, ""}, {"תריץ קדימה חצי דקה", IntentKind::SeekRelative, 630 * S, 0, ""},
    {"אני רוצה שתחזיר את הסרט דקה אחורה", IntentKind::SeekRelative, 540 * S, 0, ""}, {"אתה יכול לעצור?", IntentKind::Pause, 0, 0, ""},
    {"אפשר להמשיך בבקשה", IntentKind::Play, 0, 0, ""}, {"תגביר קצת", IntentKind::VolumeUp, 0, 0.55f, ""}, {"קצת יותר חזק", IntentKind::VolumeUp, 0, 0.55f, ""},
    {"אממ תעצור רגע", IntentKind::Pause, 0, 0, ""}, {"תוכלי לחפש שלום עולם", IntentKind::FindSubtitle, 0, 0, "שלום עולם"},
    {"תמשיך קצת קדימה", IntentKind::SeekRelative, 610 * S, 0, ""}, {"תוריד את הווליום", IntentKind::VolumeDown, 0, 0.45f, ""},
    {"אני מבקש שתבטל השתקה", IntentKind::Unmute, 0, 0, ""},
    // Other languages of the grammar.
    {"¿puedes pausar el video?", IntentKind::Pause, 0, 0, ""}, {"peux-tu mettre en pause s'il te plaît", IntentKind::Pause, 0, 0, ""},
    {"kannst du bitte lauter machen", IntentKind::VolumeUp, 0, 0.55f, ""}, {"можешь поставить на паузу пожалуйста", IntentKind::Pause, 0, 0, ""},
    {"ممكن توقف من فضلك", IntentKind::Pause, 0, 0, ""}, {"podrías retroceder un poco", IntentKind::SeekRelative, 590 * S, 0, ""},
};
struct Refusal { const char* text; IntentStatus status; };
// Framing never makes a command of what is none, nor completes a broken one.
constexpr Refusal kConversationRefused[]{
    {"can you make coffee", IntentStatus::Unknown}, {"can you", IntentStatus::Unknown}, {"could you please", IntentStatus::Unknown},
    {"אני רוצה פיצה", IntentStatus::Unknown}, {"אשמח", IntentStatus::Unknown}, {"please please please", IntentStatus::Unknown},
    {"pausing", IntentStatus::Unknown}, {"stopping", IntentStatus::Unknown}, {"would you mind", IntentStatus::Unknown}, {"a little", IntentStatus::Unknown},
    {"turn up the volume a lot", IntentStatus::Malformed}, {"can you pause 5", IntentStatus::Malformed}, {"i'd like you to rewind banana", IntentStatus::Malformed},
    {"תוכל לחזור לבננה", IntentStatus::Malformed}, {"אשמח שתעצור 5", IntentStatus::Malformed}, {"שתעצור", IntentStatus::Unknown},
    {"would you mind skipping 2 hours", IntentStatus::OutOfRange}, {"um", IntentStatus::Unknown}, {"תודה", IntentStatus::Unknown},
    {"would it be possible to pause", IntentStatus::Unknown}, {"pause and turn it down", IntentStatus::Malformed}, // Unlisted frame; two commands.
};
constexpr Refusal kRefused[]{
    {"", IntentStatus::Empty}, {"   \n\t", IntentStatus::Empty}, {"\xe2\x80\x8f", IntentStatus::Empty},
    {"please", IntentStatus::Unknown}, {"make coffee", IntentStatus::Unknown}, {"volumes up", IntentStatus::Unknown}, {"xyzzy", IntentStatus::Unknown},
    {"10 seconds", IntentStatus::Unknown}, {"שלום", IntentStatus::Unknown}, {"playing", IntentStatus::Unknown}, {"un mute", IntentStatus::Unknown},
    {"play\xff", IntentStatus::InvalidText}, {"\xc0\xafplay", IntentStatus::InvalidText}, {"rewind \xed\xa0\x80", IntentStatus::InvalidText},
    {"volume", IntentStatus::Malformed}, {"volume loud", IntentStatus::Malformed}, {"volume 50 60", IntentStatus::Malformed}, {"volume up a lot", IntentStatus::Malformed},
    {"volume 50% percent", IntentStatus::Malformed}, {"volume -5", IntentStatus::Malformed}, {"volume 5.5", IntentStatus::Malformed},
    {"rewind banana", IntentStatus::Malformed}, {"rewind 10 bananas", IntentStatus::Malformed}, {"rewind -5", IntentStatus::Malformed},
    {"rewind 10 seconds and", IntentStatus::Malformed}, {"rewind a", IntentStatus::Malformed}, {"rewind 1m30", IntentStatus::Malformed},
    {"rewind 99999999999999999999 seconds", IntentStatus::Malformed}, {"go to", IntentStatus::Malformed}, {"go to nowhere", IntentStatus::Malformed},
    {"go to 1:75", IntentStatus::Malformed}, {"go to 1:2:3:4", IntentStatus::Malformed}, {"find", IntentStatus::Malformed}, {"find   ", IntentStatus::Malformed},
    {"find \"\"", IntentStatus::Malformed}, {"pause 5", IntentStatus::Malformed}, {"mute 5", IntentStatus::Malformed}, {"play play", IntentStatus::Malformed},
    {"forward back", IntentStatus::Malformed}, {"rewind forward 10", IntentStatus::Malformed}, {"חזור 10 שניות קדימה", IntentStatus::Malformed},
    {"restart now again", IntentStatus::Malformed}, {"rewind 1 2 3 4 5 6 7 8 9 10 11 12 13", IntentStatus::Malformed},
    {"volume 201", IntentStatus::OutOfRange}, {"volume 250%", IntentStatus::OutOfRange}, {"volume up 0", IntentStatus::OutOfRange}, {"volume up 500", IntentStatus::OutOfRange},
    {"rewind 0", IntentStatus::OutOfRange}, {"rewind 0 seconds", IntentStatus::OutOfRange}, {"forward 2 hours", IntentStatus::OutOfRange},
    {"forward 110 minutes", IntentStatus::OutOfRange}, {"go to 2:00:00", IntentStatus::OutOfRange}, {"go to 3:00:00", IntentStatus::OutOfRange},
    {"rewind 25 hours", IntentStatus::OutOfRange}, {"rewind 999999 hours", IntentStatus::OutOfRange}, {"קדימה שעתיים", IntentStatus::OutOfRange},
    {"חזור 0 שניות", IntentStatus::OutOfRange},
    // The other languages refuse for the same reasons.
    {"こんにちは", IntentStatus::Unknown}, {"مرحبا", IntentStatus::Unknown}, {"hola", IntentStatus::Unknown}, {"привет", IntentStatus::Unknown},
    {"bonjour tout le monde", IntentStatus::Unknown}, {"10 Sekunden", IntentStatus::Unknown}, {"10秒", IntentStatus::Unknown}, {"你好世界", IntentStatus::Unknown},
    {"快退香蕉", IntentStatus::Malformed}, {"音量", IntentStatus::Malformed}, {"查找", IntentStatus::Malformed}, {"buscar", IntentStatus::Malformed},
    {"retroceder banana", IntentStatus::Malformed}, {"pausar 5", IntentStatus::Malformed}, {"назад вперед", IntentStatus::Malformed},
    {"10 secondes en avant en arrière", IntentStatus::Malformed}, {"播放器", IntentStatus::Malformed}, {"zurückspulen zehn Bananen", IntentStatus::Malformed},
    {"ارجع 10 موز", IntentStatus::Malformed}, {"音量を上げる 5.5", IntentStatus::Malformed}, {"1:75に移動", IntentStatus::Malformed},
    {"音量300", IntentStatus::OutOfRange}, {"volumen 250", IntentStatus::OutOfRange}, {"快进3小时", IntentStatus::OutOfRange},
    {"巻き戻し100時間", IntentStatus::OutOfRange}, {"вперед на 2 часа", IntentStatus::OutOfRange}, {"avancer de 0 secondes", IntentStatus::OutOfRange},
    {"Lautstärke 201", IntentStatus::OutOfRange}, {"الصوت 500", IntentStatus::OutOfRange},
};
bool Same(const Intent& a, const Intent& b) {
    return a.kind == b.kind && a.status == b.status && a.target_100ns == b.target_100ns && a.offset_100ns == b.offset_100ns &&
           std::memcmp(&a.volume, &b.volume, sizeof(float)) == 0 && a.query.data() == b.query.data() && a.query.size() == b.query.size();
}
struct Language { const char* name; unsigned commands; int failures; } report[12];
unsigned languages = 0;
template<std::size_t N> void Accept(const char* language, const Case (&cases)[N]) {
    const int before = failures;
    for (const auto& c : cases) {
        const std::string_view text{c.text};
        const auto intent = Dispatcher::Parse(text, kFilm);
        bool good = intent.status == IntentStatus::Ok && intent.kind == c.kind && intent.query == c.query;
        const bool seek = c.kind == IntentKind::SeekRelative || c.kind == IntentKind::SeekAbsolute;
        const bool volume = c.kind == IntentKind::VolumeUp || c.kind == IntentKind::VolumeDown || c.kind == IntentKind::VolumeSet;
        if (seek) good = good && intent.target_100ns == c.target && intent.target_100ns >= 0 && intent.target_100ns < kFilm.duration_100ns;
        if (volume) good = good && intent.volume == c.volume;
        // A relative seek keeps the amount asked for; its target is that amount from the position, stopped at the start.
        if (c.kind == IntentKind::SeekRelative) good = good && intent.offset_100ns != 0 && (intent.target_100ns == kFilm.position_100ns + intent.offset_100ns || intent.target_100ns == 0);
        // The quote is a view into the command text, never a copy.
        if (c.kind == IntentKind::FindSubtitle) good = good && intent.query.data() >= text.data() && intent.query.data() + intent.query.size() <= text.data() + text.size();
        if (!good) { ++failures; std::printf("FAIL accepted \"%s\": %s %s target=%lld volume=%g query=\"%.*s\"\n", c.text, Name(intent.kind), Name(intent.status),
                                             static_cast<long long>(intent.target_100ns), static_cast<double>(intent.volume), static_cast<int>(intent.query.size()), intent.query.empty() ? "" : intent.query.data()); }
    }
    if (languages < std::size(report)) report[languages++] = {language, static_cast<unsigned>(N), failures - before};
}
void Grammar() {
    CHECK(Dispatcher::TablesConsistent([](const char* rule, std::string_view word) noexcept {
        std::printf("  table: %s: \"%.*s\"\n", rule, static_cast<int>(word.size()), word.data());
    }));
    Accept("English/Hebrew", kAccepted); Accept("Arabic", kArabic); Accept("Chinese", kChinese); Accept("Japanese", kJapanese);
    Accept("Russian", kRussian); Accept("Spanish", kSpanish); Accept("French", kFrench); Accept("German", kGerman); Accept("Spellings", kNormalized);
    for (const auto& r : kRefused) {
        const auto intent = Dispatcher::Parse(r.text, kFilm);
        if (intent.status != r.status) { ++failures; std::printf("FAIL refused \"%s\": %s %s, expected %s\n", r.text, Name(intent.kind), Name(intent.status), Name(r.status)); }
    }
}
// Track and language commands, against a film with three audio tracks and two subtitle tracks.
struct TrackCase { const char* text; IntentKind kind; TrackChoice choice; const char* language; };
constexpr TrackCase kTracks[]{
    {"switch subtitles to hebrew", IntentKind::SelectSubtitles, TrackChoice::Language, "heb"},
    {"תעביר לאודיו באנגלית", IntentKind::SelectAudio, TrackChoice::Language, "eng"},
    {"english audio", IntentKind::SelectAudio, TrackChoice::Language, "eng"}, {"audio in french please", IntentKind::SelectAudio, TrackChoice::Language, "fra"},
    {"change the language to spanish", IntentKind::SelectAudio, TrackChoice::Language, "spa"},
    {"כתוביות בעברית", IntentKind::SelectSubtitles, TrackChoice::Language, "heb"},
    {"תעביר את הכתוביות לאנגלית", IntentKind::SelectSubtitles, TrackChoice::Language, "eng"},
    {"turn off the subtitles", IntentKind::SelectSubtitles, TrackChoice::Off, ""}, {"subtitles off", IntentKind::SelectSubtitles, TrackChoice::Off, ""},
    {"בטל כתוביות", IntentKind::SelectSubtitles, TrackChoice::Off, ""}, {"בלי כתוביות", IntentKind::SelectSubtitles, TrackChoice::Off, ""},
    {"no subtitles", IntentKind::SelectSubtitles, TrackChoice::Off, ""}, {"turn off the english subtitles", IntentKind::SelectSubtitles, TrackChoice::Off, ""},
    {"show subtitles", IntentKind::SelectSubtitles, TrackChoice::On, ""}, {"subtitles", IntentKind::SelectSubtitles, TrackChoice::On, ""},
    {"הצג כתוביות", IntentKind::SelectSubtitles, TrackChoice::On, ""}, {"next audio track", IntentKind::SelectAudio, TrackChoice::Next, ""},
    {"רצועת אודיו הבאה", IntentKind::SelectAudio, TrackChoice::Next, ""}, {"other subtitles", IntentKind::SelectSubtitles, TrackChoice::Next, ""},
    {"switch to french", IntentKind::SelectLanguage, TrackChoice::Language, "fra"}, {"בעברית בבקשה", IntentKind::SelectLanguage, TrackChoice::Language, "heb"},
    {"can you switch the audio to english", IntentKind::SelectAudio, TrackChoice::Language, "eng"},
    {"אני רוצה כתוביות באנגלית", IntentKind::SelectSubtitles, TrackChoice::Language, "eng"},
    {"i want english subtitles please", IntentKind::SelectSubtitles, TrackChoice::Language, "eng"},
    {"would you mind switching to the hebrew dub", IntentKind::SelectAudio, TrackChoice::Language, "heb"},
    {"אשמח שתעביר לדיבוב בעברית", IntentKind::SelectAudio, TrackChoice::Language, "heb"},
    {"sous-titres en français", IntentKind::SelectSubtitles, TrackChoice::Language, "fra"},
    {"Untertitel auf Deutsch", IntentKind::SelectSubtitles, TrackChoice::Language, "deu"},
    {"subtítulos en español", IntentKind::SelectSubtitles, TrackChoice::Language, "spa"},
    {"субтитры на русском", IntentKind::SelectSubtitles, TrackChoice::Language, "rus"},
    {"الترجمة بالعربية", IntentKind::SelectSubtitles, TrackChoice::Language, "ara"},
    {"idioma inglés", IntentKind::SelectAudio, TrackChoice::Language, "eng"},
};
void Tracks() {
    auto film = kFilm; film.audio_tracks = 3; film.subtitle_tracks = 2;
    int before = failures;
    for (const auto& c : kTracks) {
        const auto intent = Dispatcher::Parse(c.text, film);
        const bool good = intent.status == IntentStatus::Ok && intent.kind == c.kind && intent.track == c.choice && std::strcmp(intent.language, c.language) == 0;
        if (!good) { ++failures; std::printf("FAIL track \"%s\": %s %s %s \"%s\"\n", c.text, Name(intent.kind), Name(intent.status), Name(intent.track), intent.language); }
        // A track command is understood as such by the command bar too, never looked for as a quote.
        const auto typed = Dispatcher::Interpret(c.text, film);
        if (typed.kind != c.kind || typed.literal) { ++failures; std::printf("FAIL interpreted \"%s\": %s\n", c.text, Name(typed.kind)); }
    }
    std::printf("Tracks: %zu commands, %d failures\n", std::size(kTracks), failures - before);
    // Not track commands: the rest of the grammar reads them (or refuses them).
    CHECK(Dispatcher::Parse("audio", film).status == IntentStatus::Unknown && Dispatcher::Parse("audio off", film).status == IntentStatus::Unknown);
    CHECK(Dispatcher::Parse("switch to klingon", film).status == IntentStatus::Unknown && Dispatcher::Parse("english hebrew subtitles", film).status == IntentStatus::Unknown);
    CHECK(Dispatcher::Parse("subtitles in english and hebrew", film).status == IntentStatus::Unknown && Dispatcher::Parse("next", film).status == IntentStatus::Unknown);
    // A language's name alone is a word to look for (the command bar searches for it), not a track.
    CHECK(Dispatcher::Parse("עברית", film).status == IntentStatus::Unknown && Dispatcher::Parse("English", film).status == IntentStatus::Unknown);
    CHECK(Dispatcher::Parse("hebrew please", film).status == IntentStatus::Unknown);    const auto typed = Dispatcher::Interpret("עברית", film);
    CHECK(typed.kind == IntentKind::FindSubtitle && typed.literal && typed.query == "עברית");
    const auto quote = Dispatcher::Parse("find english", film);
    CHECK(quote.status == IntentStatus::Ok && quote.kind == IntentKind::FindSubtitle && quote.query == "english");
    CHECK(Dispatcher::Parse("mute the audio", film).kind == IntentKind::Mute && Dispatcher::Parse("הפעל", film).kind == IntentKind::Play);
    // What the media has to choose from.
    auto single = film; single.audio_tracks = 1; single.subtitle_tracks = 0;
    CHECK(Dispatcher::Parse("next audio track", single).status == IntentStatus::Unavailable && Dispatcher::Parse("hebrew subtitles", single).status == IntentStatus::Unavailable);
    CHECK(Dispatcher::Parse("english audio", single).status == IntentStatus::Ok && Dispatcher::Parse("switch to french", single).status == IntentStatus::Ok);
    auto none = film; none.audio_tracks = 0; none.subtitle_tracks = 0;
    CHECK(Dispatcher::Parse("switch to french", none).status == IntentStatus::Unavailable && Dispatcher::Parse("subtitles off", none).status == IntentStatus::Unavailable);
    auto one_subtitle = film; one_subtitle.subtitle_tracks = 1;
    CHECK(Dispatcher::Parse("next subtitles", one_subtitle).status == IntentStatus::Ok); // Cycles through off.
    // Language codes as containers and file names write them.
    char code[4];
    CanonicalLanguage("fre", code); CHECK(std::strcmp(code, "fra") == 0);
    CanonicalLanguage("GER", code); CHECK(std::strcmp(code, "deu") == 0);
    CanonicalLanguage("he", code); CHECK(std::strcmp(code, "heb") == 0);
    CanonicalLanguage("iw", code); CHECK(std::strcmp(code, "heb") == 0);
    CanonicalLanguage("eng", code); CHECK(std::strcmp(code, "eng") == 0);
    CanonicalLanguage("und", code); CHECK(code[0] == 0);
    CanonicalLanguage("e1g", code); CHECK(code[0] == 0);
    CanonicalLanguage("xx", code); CHECK(code[0] == 0);
    CanonicalLanguage("english", code); CHECK(code[0] == 0);
    CanonicalLanguage("kor", code); CHECK(std::strcmp(code, "kor") == 0);
    CHECK(std::strcmp(LanguageName("heb"), "Hebrew") == 0 && std::strcmp(NativeLanguageName("he"), "עברית") == 0 && LanguageName("kor") == nullptr);
    CHECK(std::strcmp(Name(IntentKind::SelectLanguage), "SelectLanguage") == 0 && std::strcmp(Name(TrackChoice::Next), "Next") == 0);
}
// The next and previous file in the folder (docs/intents.md, "Files"), in every language, as said and as typed.
constexpr struct { const char* text; IntentKind kind; } kFiles[]{
    {"next video", IntentKind::NextFile}, {"play the next video", IntentKind::NextFile}, {"skip to the next video", IntentKind::NextFile},
    {"next episode please", IntentKind::NextFile}, {"can you go to the next video", IntentKind::NextFile}, {"Next File", IntentKind::NextFile},
    {"previous video", IntentKind::PreviousFile}, {"go back to the previous video", IntentKind::PreviousFile}, {"the previous episode", IntentKind::PreviousFile},
    {"הסרטון הבא", IntentKind::NextFile}, {"תעבור לסרטון הבא בבקשה", IntentKind::NextFile}, {"אשמח שתעבור לסרטון הבא", IntentKind::NextFile},
    {"הקובץ הבא", IntentKind::NextFile}, {"הפרק הבא בסדרה", IntentKind::NextFile},
    {"הסרטון הקודם", IntentKind::PreviousFile}, {"תחזור לסרטון הקודם", IntentKind::PreviousFile}, {"הסרט הקודם", IntentKind::PreviousFile},
    {"الفيديو التالي", IntentKind::NextFile}, {"الفيديو السابق", IntentKind::PreviousFile},
    {"siguiente vídeo", IntentKind::NextFile}, {"vídeo anterior", IntentKind::PreviousFile},
    {"vidéo suivante", IntentKind::NextFile}, {"vidéo précédente", IntentKind::PreviousFile},
    {"nächstes Video", IntentKind::NextFile}, {"vorheriges Video", IntentKind::PreviousFile},
    {"следующее видео", IntentKind::NextFile}, {"предыдущее видео", IntentKind::PreviousFile},
    {"下一个视频", IntentKind::NextFile}, {"上一集", IntentKind::PreviousFile},
    {"次の動画", IntentKind::NextFile}, {"前の動画", IntentKind::PreviousFile},
};
void Files() {
    auto folder = kFilm; folder.files_before = 2; folder.files_after = 3;
    int before = failures;
    for (const auto& c : kFiles) {
        const auto intent = Dispatcher::Parse(c.text, folder);
        if (intent.status != IntentStatus::Ok || intent.kind != c.kind) { ++failures; std::printf("FAIL file \"%s\": %s %s\n", c.text, Name(intent.kind), Name(intent.status)); }
        const auto typed = Dispatcher::Interpret(c.text, folder);
        if (typed.kind != c.kind || typed.literal) { ++failures; std::printf("FAIL interpreted \"%s\": %s\n", c.text, Name(typed.kind)); }
    }
    std::printf("Files: %zu commands, %d failures\n", std::size(kFiles), failures - before);
    // Nothing that way: refused as unavailable (and never looked for as a quote).
    auto first = folder; first.files_before = 0;
    auto last = folder; last.files_after = 0;
    CHECK(Dispatcher::Parse("previous video", first).status == IntentStatus::Unavailable && Dispatcher::Parse("next video", first).status == IntentStatus::Ok);
    CHECK(Dispatcher::Parse("הסרטון הבא", last).status == IntentStatus::Unavailable && Dispatcher::Parse("הסרטון הקודם", last).status == IntentStatus::Ok);
    CHECK(Dispatcher::Parse("next video", kFilm).status == IntentStatus::Unavailable); // No folder.
    const auto refused = Dispatcher::Interpret("next video", last);
    CHECK(refused.kind == IntentKind::NextFile && refused.status == IntentStatus::Unavailable && !refused.literal);
    // Neighbours keep their meaning: chapters, matches, scenes.
    CHECK(Dispatcher::Parse("הפרק הבא", folder).kind == IntentKind::NextChapter && Dispatcher::Parse("next chapter", folder).kind == IntentKind::NextChapter);
    CHECK(Dispatcher::Parse("next one", folder).kind == IntentKind::NextMatch && Dispatcher::Parse("следующее", folder).kind == IntentKind::NextMatch);
    CHECK(Dispatcher::Parse("next scene", folder).kind == IntentKind::NextScene && Dispatcher::Parse("下一章", folder).kind == IntentKind::NextChapter);
    CHECK(Dispatcher::Parse("find the next video", folder).kind == IntentKind::FindSubtitle); // A quote that says it.
    CHECK(Dispatcher::Parse("next video 2", folder).status == IntentStatus::Malformed);
    CHECK(std::strcmp(Name(IntentKind::NextFile), "NextFile") == 0 && std::strcmp(Name(IntentKind::PreviousFile), "PreviousFile") == 0);
}
// Playback speed (docs/intents.md, "Speed"): steps, set values in every form, fixed phrases, in the nine languages.
constexpr struct { const char* text; IntentKind kind; float speed; } kSpeeds[]{
    {"faster", IntentKind::SpeedUp, 0}, {"speed up", IntentKind::SpeedUp, 0}, {"can you play faster please", IntentKind::SpeedUp, 0},
    {"slower", IntentKind::SpeedDown, 0}, {"slow down a bit", IntentKind::SpeedDown, 0},
    {"speed 1.5x", IntentKind::SpeedSet, 1.5f}, {"speed 1.5", IntentKind::SpeedSet, 1.5f}, {"set the speed to 0.75", IntentKind::SpeedSet, 0.75f},
    {"play at 1.25x", IntentKind::SpeedSet, 1.25f}, {"speed x2", IntentKind::SpeedSet, 2.0f}, {"speed 2 times", IntentKind::SpeedSet, 2.0f},
    {"playback speed 50%", IntentKind::SpeedSet, 0.5f}, {"speed 0,25", IntentKind::SpeedSet, 0.25f}, {"speed 1.5 x", IntentKind::SpeedSet, 1.5f},
    {"normal speed", IntentKind::SpeedSet, 1.0f}, {"double speed", IntentKind::SpeedSet, 2.0f}, {"half speed", IntentKind::SpeedSet, 0.5f},
    {"מהר יותר", IntentKind::SpeedUp, 0}, {"תאיץ בבקשה", IntentKind::SpeedUp, 0}, {"לאט יותר", IntentKind::SpeedDown, 0}, {"תאט קצת", IntentKind::SpeedDown, 0},
    {"מהירות כפולה", IntentKind::SpeedSet, 2.0f}, {"חצי מהירות", IntentKind::SpeedSet, 0.5f}, {"מהירות רגילה", IntentKind::SpeedSet, 1.0f},
    {"מהירות 1.5", IntentKind::SpeedSet, 1.5f}, {"תקבע מהירות ל-1.25", IntentKind::SpeedSet, 1.25f}, {"מהירות פי 2", IntentKind::SpeedSet, 2.0f},
    {"اسرع", IntentKind::SpeedUp, 0}, {"أبطأ", IntentKind::SpeedDown, 0}, {"السرعة 1.5", IntentKind::SpeedSet, 1.5f},
    {"más rápido", IntentKind::SpeedUp, 0}, {"velocidad 1.5", IntentKind::SpeedSet, 1.5f}, {"plus vite", IntentKind::SpeedUp, 0},
    {"vitesse normale", IntentKind::SpeedSet, 1.0f}, {"langsamer", IntentKind::SpeedDown, 0}, {"doppelte Geschwindigkeit", IntentKind::SpeedSet, 2.0f},
    {"быстрее", IntentKind::SpeedUp, 0}, {"скорость 1.5", IntentKind::SpeedSet, 1.5f}, {"快一点", IntentKind::SpeedUp, 0}, {"两倍速", IntentKind::SpeedSet, 2.0f},
    {"慢一点", IntentKind::SpeedDown, 0}, {"速く", IntentKind::SpeedUp, 0}, {"通常速度", IntentKind::SpeedSet, 1.0f}, {"倍速", IntentKind::SpeedSet, 2.0f},
    // Zoom (docs/picture.md, "Zoom").
    {"zoom in", IntentKind::ZoomIn, 0}, {"can you zoom in please", IntentKind::ZoomIn, 0}, {"zoom out", IntentKind::ZoomOut, 0}, {"reset zoom", IntentKind::ZoomReset, 0},
    {"זום פנימה", IntentKind::ZoomIn, 0}, {"תגדיל את התמונה", IntentKind::ZoomIn, 0}, {"זום החוצה", IntentKind::ZoomOut, 0}, {"אפס זום", IntentKind::ZoomReset, 0},
    {"בטל זום", IntentKind::ZoomReset, 0}, {"تكبير", IntentKind::ZoomIn, 0}, {"ampliar", IntentKind::ZoomIn, 0}, {"dézoomer", IntentKind::ZoomOut, 0},
    {"vergrößern", IntentKind::ZoomIn, 0}, {"уменьшить", IntentKind::ZoomOut, 0}, {"放大", IntentKind::ZoomIn, 0}, {"縮小", IntentKind::ZoomOut, 0},
    {"ズームリセット", IntentKind::ZoomReset, 0},
};
void Speeds() {
    int before = failures;
    for (const auto& c : kSpeeds) {
        const auto intent = Dispatcher::Parse(c.text, kFilm);
        const bool good = intent.status == IntentStatus::Ok && intent.kind == c.kind && (c.kind != IntentKind::SpeedSet || std::fabs(intent.speed - c.speed) < 1e-6f);
        if (!good) { ++failures; std::printf("FAIL speed \"%s\": %s %s %.3f\n", c.text, Name(intent.kind), Name(intent.status), static_cast<double>(intent.speed)); }
        const auto typed = Dispatcher::Interpret(c.text, kFilm);
        if (typed.kind != c.kind || typed.literal) { ++failures; std::printf("FAIL interpreted \"%s\": %s\n", c.text, Name(typed.kind)); }
    }
    std::printf("Speeds: %zu commands, %d failures\n", std::size(kSpeeds), failures - before);
    // Out of the range, unreadable, and the neighbours that share words.
    CHECK(Dispatcher::Parse("speed 5x", kFilm).status == IntentStatus::OutOfRange && Dispatcher::Parse("speed 0.1", kFilm).status == IntentStatus::OutOfRange);
    CHECK(Dispatcher::Parse("speed fast", kFilm).status == IntentStatus::Malformed && Dispatcher::Parse("speed", kFilm).status == IntentStatus::Malformed);
    CHECK(Dispatcher::Parse("speed 1.5.2", kFilm).status == IntentStatus::Malformed && Dispatcher::Parse("speed 1..5", kFilm).status == IntentStatus::Malformed &&
          Dispatcher::Parse("speed .5", kFilm).status == IntentStatus::Malformed && Dispatcher::Parse("speed 1.2345", kFilm).status == IntentStatus::Malformed);
    CHECK(Dispatcher::Parse("speed 1.", kFilm).speed == 1.0f); // A sentence's full stop is no decimal point.
    CHECK(Dispatcher::Parse("play", kFilm).kind == IntentKind::Play && Dispatcher::Parse("forward 10", kFilm).kind == IntentKind::SeekRelative);
    CHECK(Dispatcher::Parse("faster 2", kFilm).status == IntentStatus::Malformed);
    CHECK(std::strcmp(Name(IntentKind::SpeedSet), "SpeedSet") == 0 && std::strcmp(Name(IntentKind::SpeedUp), "SpeedUp") == 0);
    // Zoom's neighbours: "בטל" alone is still undo, the volume's longer phrases still the volume.
    CHECK(Dispatcher::Parse("בטל", kFilm).kind == IntentKind::Undo && Dispatcher::Parse("увеличить громкость", kFilm).kind == IntentKind::VolumeUp);
    CHECK(Dispatcher::Parse("reducir el volumen", kFilm).kind == IntentKind::VolumeDown && Dispatcher::Parse("zoom in 2", kFilm).status == IntentStatus::Malformed);
    CHECK(std::strcmp(Name(IntentKind::ZoomReset), "ZoomReset") == 0);
}
void Conversation() {
    Accept("Conversation", kConversation);
    for (const auto& r : kConversationRefused) {
        const auto intent = Dispatcher::Parse(r.text, kFilm);
        if (intent.status != r.status) { ++failures; std::printf("FAIL refused \"%s\": %s %s, expected %s\n", r.text, Name(intent.kind), Name(intent.status), Name(r.status)); }
    }
}
// The same words against other media states.
void Bounds() {
    // Length limits.
    static char long_command[Dispatcher::kMaxCommandBytes + 2], long_quote[Dispatcher::kMaxQueryBytes + 8];
    std::memset(long_command, 'a', sizeof(long_command) - 1);
    CHECK(Dispatcher::Parse(long_command, kFilm).status == IntentStatus::TooLong);
    std::memset(long_quote, 'q', sizeof(long_quote) - 1); std::memcpy(long_quote, "find ", 5);
    CHECK(Dispatcher::Parse(long_quote, kFilm).status == IntentStatus::TooLong);           // A 258-byte quote.
    long_quote[5 + Dispatcher::kMaxQueryBytes] = 0;
    const auto longest = Dispatcher::Parse(long_quote, kFilm);
    CHECK(longest.status == IntentStatus::Ok && longest.query.size() == Dispatcher::kMaxQueryBytes);
    // A stream that cannot seek, a live stream without duration, a file without subtitles.
    auto live = kFilm; live.seekable = false;
    CHECK(Dispatcher::Parse("rewind 10", live).status == IntentStatus::Unavailable && Dispatcher::Parse("go to 1:00", live).status == IntentStatus::Unavailable);
    CHECK(Dispatcher::Parse("find hello", live).status == IntentStatus::Unavailable && Dispatcher::Parse("restart", live).status == IntentStatus::Unavailable);
    CHECK(Dispatcher::Parse("rewind banana", live).status == IntentStatus::Malformed && Dispatcher::Parse("rewind 0", live).status == IntentStatus::OutOfRange);
    CHECK(Dispatcher::Parse("pause", live).status == IntentStatus::Ok && Dispatcher::Parse("volume 30", live).status == IntentStatus::Ok);
    auto endless = kFilm; endless.duration_100ns = 0;
    CHECK(Dispatcher::Parse("go to 10", endless).status == IntentStatus::Unavailable && Dispatcher::Parse("forward 10", endless).status == IntentStatus::Unavailable);
    auto silent_film = kFilm; silent_film.subtitles = false;
    CHECK(Dispatcher::Parse("find hello", silent_film).status == IntentStatus::Unavailable && Dispatcher::Parse("find", silent_film).status == IntentStatus::Malformed);
    CHECK(Dispatcher::Parse("rewind 10", silent_film).status == IntentStatus::Ok);
    // Positions: the amount is measured from where playback is; at the ends only one direction remains.
    auto clip = kFilm; clip.duration_100ns = 3 * S; clip.position_100ns = 19 * S / 10;
    auto intent = Dispatcher::Parse("חזור שנייה", clip);
    CHECK(intent.status == IntentStatus::Ok && intent.target_100ns == 9 * S / 10 && intent.offset_100ns == -S);
    CHECK(Dispatcher::Parse("forward 1 second", clip).target_100ns == 29 * S / 10 && Dispatcher::Parse("forward 2 seconds", clip).status == IntentStatus::OutOfRange);
    CHECK(Dispatcher::Parse("rewind 30s", clip).status == IntentStatus::Ok && Dispatcher::Parse("rewind 30s", clip).target_100ns == 0);
    CHECK(Dispatcher::Parse("go to 2", clip).status == IntentStatus::Ok && Dispatcher::Parse("go to 3", clip).status == IntentStatus::OutOfRange);
    clip.position_100ns = 99 * S; // A reported position past the end counts as the end.
    CHECK(Dispatcher::Parse("rewind 1", clip).target_100ns == 2 * S && Dispatcher::Parse("forward 1", clip).status == IntentStatus::OutOfRange);
    clip.position_100ns = -5 * S;
    CHECK(Dispatcher::Parse("forward 1", clip).target_100ns == S && Dispatcher::Parse("rewind 1", clip).target_100ns == 0);
    // Volume: the ends hold, off-grid levels rejoin the 5 % grid as the keyboard does, garbage is silence.
    auto level = kFilm; level.volume = 1;
    CHECK(Dispatcher::Parse("louder", level).volume == 1 && Dispatcher::Parse("louder", level).status == IntentStatus::Ok && Dispatcher::Parse("quieter", level).volume == 0.95f);
    level.volume = 0;
    CHECK(Dispatcher::Parse("quieter", level).volume == 0 && Dispatcher::Parse("louder", level).volume == 0.05f);
    level.volume = 0.52f;
    CHECK(Dispatcher::Parse("louder", level).volume == 0.55f && Dispatcher::Parse("quieter", level).volume == 0.45f && Dispatcher::Parse("louder 10", level).volume == 0.62f);
    level.volume = std::numeric_limits<float>::quiet_NaN();
    CHECK(Dispatcher::Parse("louder", level).volume == 0.05f);
    level.volume = 40; // Beyond the boost's top: taken as 200 %.
    CHECK(Dispatcher::Parse("quieter", level).volume == 1.95f);
    // API 16: a boost is asked for by number; steps stay under 100 % unless boosted already, and never pull a boost down.
    level.volume = 1.0f;
    CHECK(Dispatcher::Parse("louder", level).volume == 1.0f && Dispatcher::Parse("volume 150", level).volume == 1.5f &&
          Dispatcher::Parse("volume 200%", level).volume == 2.0f && Dispatcher::Parse("volume 201", level).status == IntentStatus::OutOfRange);
    level.volume = 1.5f;
    CHECK(Dispatcher::Parse("louder", level).volume == 1.55f && Dispatcher::Parse("quieter", level).volume == 1.45f && Dispatcher::Parse("louder 80", level).volume == 2.0f);
    // And speed to 4x.
    CHECK(Dispatcher::Parse("speed 3", level).speed == 3.0f && Dispatcher::Parse("speed x4", level).speed == 4.0f &&
          Dispatcher::Parse("speed 4.5", level).status == IntentStatus::OutOfRange);
}
struct Recorder { unsigned calls = 0; Intent last{}; IntentStatus answer = IntentStatus::Ok; };
IntentStatus Record(void* context, const Intent& intent) noexcept {
    auto& r = *static_cast<Recorder*>(context); ++r.calls; r.last = intent; return r.answer;
}
// Dispatch: only an Ok intent reaches the executor, exactly once, and the executor's answer is the outcome.
void Dispatching() {
    Dispatcher dispatcher; Recorder recorder;
    CHECK(dispatcher.Dispatch("pause", kFilm).status == IntentStatus::Rejected && dispatcher.Stats().failed == 1); // No executor: nothing can act.
    dispatcher.Configure(Record, &recorder);
    auto intent = dispatcher.Dispatch("חזור 10 שניות", kFilm);
    CHECK(intent.status == IntentStatus::Ok && recorder.calls == 1 && recorder.last.kind == IntentKind::SeekRelative && recorder.last.target_100ns == 590 * S);
    CHECK(dispatcher.Dispatch("forward 2 hours", kFilm).status == IntentStatus::OutOfRange && recorder.calls == 1);
    CHECK(dispatcher.Dispatch("make coffee", kFilm).status == IntentStatus::Unknown && recorder.calls == 1);
    constexpr std::string_view quote = "מצא שלום";
    recorder.answer = IntentStatus::NotFound;
    intent = dispatcher.Dispatch(quote, kFilm);
    CHECK(intent.status == IntentStatus::NotFound && intent.kind == IntentKind::FindSubtitle && recorder.calls == 2 && recorder.last.query == "שלום");
    recorder.answer = IntentStatus::Rejected;
    CHECK(dispatcher.Dispatch("mute", kFilm).status == IntentStatus::Rejected && recorder.calls == 3 && recorder.last.kind == IntentKind::Mute);
    const auto& stats = dispatcher.Stats();
    CHECK(stats.commands == 6 && stats.executed == 1 && stats.refused == 2 && stats.failed == 3);
    CHECK(std::strcmp(Name(IntentKind::FindSubtitle), "FindSubtitle") == 0 && std::strcmp(Name(IntentStatus::Rejected), "Rejected") == 0);
}
// Follow-ups: the grammar, then a conversation against an engine stub (docs/intents.md, "Follow-ups").
struct Engine {
    MediaBounds bounds = kFilm; ConversationState* conversation = nullptr; unsigned calls = 0; Intent last{};
    static constexpr std::int64_t kCues[3]{100 * S, 200 * S, 300 * S}; // Where the quote "hello" is said.
    MediaBounds Now() const { auto b = bounds; b.matches = conversation && conversation->Query() == "hello" ? 3 : 0; return b; }
};
IntentStatus Act(void* context, const Intent& intent) noexcept {
    auto& e = *static_cast<Engine*>(context); ++e.calls; e.last = intent;
    switch (intent.kind) {
    case IntentKind::SeekRelative: case IntentKind::SeekAbsolute: e.conversation->NoteJump(e.bounds.position_100ns); e.bounds.position_100ns = intent.target_100ns; break;
    case IntentKind::FindSubtitle:
        if (intent.query != "hello" || intent.match >= 3) return IntentStatus::NotFound;
        e.conversation->NoteJump(e.bounds.position_100ns); e.bounds.position_100ns = Engine::kCues[intent.match]; break;
    case IntentKind::VolumeUp: case IntentKind::VolumeDown: e.bounds.volume = intent.volume; break;
    default: break;
    }
    return IntentStatus::Ok;
}
void FollowUps() {
    // ---- Grammar ----
    auto searched = kFilm; searched.matches = 3; searched.audio_tracks = 2; searched.subtitle_tracks = 2;
    struct Said { const char* text; IntentKind kind; unsigned match; };
    constexpr Said said[]{
        {"next match", IntentKind::NextMatch, 0}, {"the next one", IntentKind::NextMatch, 0}, {"search again", IntentKind::NextMatch, 0},
        {"התוצאה הבאה", IntentKind::NextMatch, 0}, {"את הבאה", IntentKind::NextMatch, 0}, {"תחפש שוב", IntentKind::NextMatch, 0},
        {"previous match", IntentKind::PreviousMatch, 0}, {"the previous one", IntentKind::PreviousMatch, 0}, {"התוצאה הקודמת", IntentKind::PreviousMatch, 0},
        {"the second one", IntentKind::NthMatch, 2}, {"second match", IntentKind::NthMatch, 2}, {"the 3rd match", IntentKind::NthMatch, 3},
        {"go to the last one", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"can you show me the second one", IntentKind::NthMatch, 2},
        {"את השני", IntentKind::NthMatch, 2}, {"השנייה", IntentKind::NthMatch, 2}, {"התוצאה השלישית", IntentKind::NthMatch, 3},
        {"האחרונה בבקשה", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"תעבור לראשון", IntentKind::None, 0},
        {"undo", IntentKind::Undo, 0}, {"undo that", IntentKind::Undo, 0}, {"go back to where I was", IntentKind::Undo, 0}, {"where was i", IntentKind::Undo, 0},
        {"בטל", IntentKind::Undo, 0}, {"תבטל את הקפיצה", IntentKind::Undo, 0}, {"תחזור לאיפה שהייתי", IntentKind::Undo, 0}, {"אני רוצה לבטל", IntentKind::Undo, 0},
        {"again", IntentKind::Repeat, 0}, {"do it again", IntentKind::Repeat, 0}, {"one more time please", IntentKind::Repeat, 0},
        {"עוד פעם", IntentKind::Repeat, 0}, {"שוב", IntentKind::Repeat, 0}, {"תעשה את זה שוב", IntentKind::Repeat, 0},
        {"what did he say", IntentKind::ReplayCue, 0}, {"replay the line", IntentKind::ReplayCue, 0}, {"מה הוא אמר", IntentKind::ReplayCue, 0},
        {"תחזור על המשפט", IntentKind::ReplayCue, 0},
        {"compare quality", IntentKind::CompareOn, 0}, {"split view", IntentKind::CompareOn, 0}, {"השווה איכות", IntentKind::CompareOn, 0},
        {"מסך מפוצל", IntentKind::CompareOn, 0}, {"stop comparing", IntentKind::CompareOff, 0}, {"בטל השוואה", IntentKind::CompareOff, 0},
        // What the new words must not take: the commands that share their first words.
        {"go back 10 seconds", IntentKind::SeekRelative, 0}, {"בטל השתקה", IntentKind::Unmute, 0}, {"בטל כתוביות", IntentKind::SelectSubtitles, 0},
        {"תחזור 5 שניות", IntentKind::SeekRelative, 0}, {"תעצור שוב", IntentKind::Pause, 0}, {"find the second one", IntentKind::FindSubtitle, 0},
        {"find next week", IntentKind::FindSubtitle, 0}, {"next chapter", IntentKind::NextChapter, 0},
        // The seven other languages (2026-10-19, second directive): every kind, in their own words.
        {"النتيجة التالية", IntentKind::NextMatch, 0}, {"السابق", IntentKind::PreviousMatch, 0}, {"الثاني", IntentKind::NthMatch, 2},
        {"النتيجة الأخيرة", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"إلغاء", IntentKind::Undo, 0}, {"ارجع إلى حيث كنت", IntentKind::Undo, 0},
        {"مرة أخرى", IntentKind::Repeat, 0}, {"ماذا قال", IntentKind::ReplayCue, 0}, {"قارن الجودة", IntentKind::CompareOn, 0}, {"أوقف المقارنة", IntentKind::CompareOff, 0},
        {"siguiente resultado", IntentKind::NextMatch, 0}, {"el anterior", IntentKind::PreviousMatch, 0}, {"el segundo", IntentKind::NthMatch, 2},
        {"ve al último resultado", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"deshacer", IntentKind::Undo, 0}, {"vuelve a donde estaba", IntentKind::Undo, 0},
        {"otra vez", IntentKind::Repeat, 0}, {"¿qué dijo?", IntentKind::ReplayCue, 0}, {"pantalla dividida", IntentKind::CompareOn, 0}, {"dejar de comparar", IntentKind::CompareOff, 0},
        {"résultat suivant", IntentKind::NextMatch, 0}, {"le précédent", IntentKind::PreviousMatch, 0}, {"la deuxième", IntentKind::NthMatch, 2},
        {"le troisième résultat", IntentKind::NthMatch, 3}, {"annuler", IntentKind::Undo, 0}, {"reviens où j'étais", IntentKind::Undo, 0}, {"encore une fois", IntentKind::Repeat, 0},
        {"qu'est-ce qu'il a dit", IntentKind::ReplayCue, 0}, {"comparer la qualité", IntentKind::CompareOn, 0}, {"arrête de comparer", IntentKind::CompareOff, 0},
        {"nächster Treffer", IntentKind::NextMatch, 0}, {"der vorherige", IntentKind::PreviousMatch, 0}, {"der zweite Treffer", IntentKind::NthMatch, 2},
        {"zeig mir den letzten", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"rückgängig machen", IntentKind::Undo, 0}, {"zurück wo ich war", IntentKind::Undo, 0},
        {"noch einmal", IntentKind::Repeat, 0}, {"was hat er gesagt", IntentKind::ReplayCue, 0}, {"Qualität vergleichen", IntentKind::CompareOn, 0}, {"Vergleich beenden", IntentKind::CompareOff, 0},
        {"следующий результат", IntentKind::NextMatch, 0}, {"предыдущий", IntentKind::PreviousMatch, 0}, {"второй", IntentKind::NthMatch, 2},
        {"покажи третий результат", IntentKind::NthMatch, 3}, {"отмени", IntentKind::Undo, 0}, {"вернись туда где я был", IntentKind::Undo, 0}, {"ещё раз", IntentKind::Repeat, 0},
        {"что он сказал", IntentKind::ReplayCue, 0}, {"сравнить качество", IntentKind::CompareOn, 0}, {"выключи сравнение", IntentKind::CompareOff, 0},
        {"下一个结果", IntentKind::NextMatch, 0}, {"上一条", IntentKind::PreviousMatch, 0}, {"第二个", IntentKind::NthMatch, 2}, {"最后一个", IntentKind::NthMatch, Dispatcher::kLastMatch},
        {"撤销", IntentKind::Undo, 0}, {"再来一次", IntentKind::Repeat, 0}, {"他说了什么", IntentKind::ReplayCue, 0}, {"对比画质", IntentKind::CompareOn, 0}, {"关闭对比", IntentKind::CompareOff, 0},
        {"次の結果", IntentKind::NextMatch, 0}, {"前の結果", IntentKind::PreviousMatch, 0}, {"二番目", IntentKind::NthMatch, 2}, {"3番目", IntentKind::NthMatch, 3},
        {"最後", IntentKind::NthMatch, Dispatcher::kLastMatch}, {"元に戻す", IntentKind::Undo, 0}, {"もう一度", IntentKind::Repeat, 0}, {"何て言った", IntentKind::ReplayCue, 0},
        {"画質を比較", IntentKind::CompareOn, 0}, {"比較をやめる", IntentKind::CompareOff, 0},
        // Their neighbours keep their meaning: chapters, seeks, pause, unmute, restart, volume.
        {"siguiente capítulo", IntentKind::NextChapter, 0}, {"volver", IntentKind::SeekRelative, 0}, {"arrête", IntentKind::Pause, 0}, {"zurück", IntentKind::SeekRelative, 0},
        {"вернись", IntentKind::SeekRelative, 0}, {"أوقف", IntentKind::Pause, 0}, {"إلغاء الكتم", IntentKind::Unmute, 0}, {"تراجع", IntentKind::SeekRelative, 0},
        {"重播", IntentKind::SeekAbsolute, 0}, {"最初から", IntentKind::SeekAbsolute, 0}, {"下一章", IntentKind::NextChapter, 0}, {"次の章", IntentKind::NextChapter, 0},
        {"mach lauter", IntentKind::VolumeUp, 0}, {"atrás un segundo", IntentKind::SeekRelative, 0}};
    unsigned wrong = 0;
    for (const auto& s : said) {
        auto bounds = searched; bounds.chapters = 3;
        const auto intent = Dispatcher::Parse(s.text, bounds);
        const bool ok = s.kind == IntentKind::None ? intent.status != IntentStatus::Ok :
                        intent.status == IntentStatus::Ok && intent.kind == s.kind && (s.kind != IntentKind::NthMatch || intent.match == s.match);
        if (!ok) { ++wrong; std::printf("FAIL follow-up \"%s\": %s %s (match %u)\n", s.text, Name(intent.kind), Name(intent.status), intent.match); }
    }
    CHECK(wrong == 0);
    // No search to follow: a match follow-up is NoContext, and the search field looks for the words themselves.
    CHECK(Dispatcher::Parse("next match", kFilm).status == IntentStatus::NoContext && Dispatcher::Parse("the second one", kFilm).status == IntentStatus::NoContext);
    const auto literal = Dispatcher::Interpret("the first", kFilm);
    CHECK(literal.kind == IntentKind::FindSubtitle && literal.literal && literal.query == "the first" && literal.status == IntentStatus::Ok);
    // An English ordinal alone is no command, nor are two ordinals; English "again" closing a command is no courtesy.
    CHECK(Dispatcher::Parse("second", searched).status == IntentStatus::Unknown && Dispatcher::Parse("the second third", searched).status == IntentStatus::Unknown);
    CHECK(Dispatcher::Parse("play it again", searched).status == IntentStatus::Malformed);
    // A second of time is no ordinal: "one" counts only after the ordinal, and no indefinite article is a filler.
    for (const char* time : {"one second", "une seconde", "un segundo", "eine Sekunde"}) CHECK(Dispatcher::Parse(time, searched).kind != IntentKind::NthMatch);
    // Ordinals that need another word (no article in the language marks them: "segundo" is also a unit, "erste" a quote).
    CHECK(Dispatcher::Parse("segundo", searched).status == IntentStatus::Unknown && Dispatcher::Parse("deuxieme", searched).status == IntentStatus::Unknown);
    CHECK(Dispatcher::Parse("find the second one", searched).query == "the second one" && Dispatcher::Parse("find next week", searched).query == "next week");
    auto silent = searched; silent.subtitles = false;
    auto fixed = searched; fixed.seekable = false;
    CHECK(Dispatcher::Parse("what did he say", silent).status == IntentStatus::Unavailable && Dispatcher::Parse("undo", fixed).status == IntentStatus::Unavailable);
    CHECK(Dispatcher::Parse("next match", fixed).status == IntentStatus::Unavailable && Dispatcher::Parse("the second one", fixed).status == IntentStatus::Unavailable);

    // ---- The conversation ----
    ConversationState conversation; Engine engine; engine.conversation = &conversation;
    Dispatcher dispatcher; dispatcher.Configure(Act, &engine); dispatcher.Converse(&conversation);
    const auto say = [&](std::string_view text) { return dispatcher.Dispatch(text, engine.Now()); };
    // Nothing to follow yet.
    CHECK(say("next match").status == IntentStatus::NoContext && say("undo").status == IntentStatus::NoContext && engine.calls == 0);
    auto intent = say("again"); // Nothing repeatable: the line on screen.
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::ReplayCue && intent.from == IntentKind::Repeat && engine.calls == 1);
    // A search, then its matches in turn (round at both ends), by ordinal, and refused past the last.
    char typed[32]; std::memcpy(typed, "find hello", 11); // The text is gone after the command (the bar's buffer).
    intent = dispatcher.Dispatch(std::string_view{typed, 10}, engine.Now()); std::memset(typed, 'x', sizeof(typed));
    CHECK(intent.status == IntentStatus::Ok && conversation.Query() == "hello" && engine.bounds.position_100ns == 100 * S);
    struct Step { const char* text; unsigned match; };
    for (const Step& step : {Step{"next match", 1}, Step{"התוצאה הבאה", 2}, Step{"the next one", 0}, Step{"previous match", 2}, Step{"את השני", 1},
                             Step{"the last one", 2}, Step{"הראשונה", 0}, Step{"siguiente resultado", 1}, Step{"下一个结果", 2}, Step{"le précédent", 1},
                             Step{"der erste Treffer", 0}, Step{"последний", 2}, Step{"二番目", 1}, Step{"الأول", 0}}) {
        intent = say(step.text);
        const bool ok = intent.status == IntentStatus::Ok && intent.kind == IntentKind::FindSubtitle && intent.query == "hello" && intent.match == step.match &&
                        engine.last.match == step.match && engine.bounds.position_100ns == Engine::kCues[step.match] && conversation.Match() == step.match;
        if (!ok) std::printf("FAIL conversation \"%s\": %s %s match %u\n", step.text, Name(intent.kind), Name(intent.status), intent.match);
        CHECK(ok);
    }
    const unsigned calls = engine.calls;
    CHECK(say("the fifth one").status == IntentStatus::OutOfRange && engine.calls == calls && conversation.Match() == 0);
    // Undo: back to before the last jump (the first match was reached from the second one), and undo again comes back.
    intent = say("undo");
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::SeekAbsolute && intent.from == IntentKind::Undo && intent.target_100ns == 200 * S);
    intent = say("בטל");
    CHECK(intent.status == IntentStatus::Ok && intent.target_100ns == 100 * S && engine.bounds.position_100ns == 100 * S);
    // Again: the last relative seek or volume step, from where things are now; a refusal changes nothing.
    CHECK(say("back 10 seconds").status == IntentStatus::Ok && engine.bounds.position_100ns == 90 * S);
    CHECK(say("again").status == IntentStatus::Ok && engine.bounds.position_100ns == 80 * S && engine.last.from == IntentKind::Repeat);
    CHECK(say("עוד פעם").status == IntentStatus::Ok && engine.bounds.position_100ns == 70 * S && conversation.Repeatable() == "back 10 seconds");
    CHECK(say("forward 5 hours").status == IntentStatus::OutOfRange && say("שוב").status == IntentStatus::Ok && engine.bounds.position_100ns == 60 * S);
    intent = say("תחזור לאיפה שהייתי");
    CHECK(intent.status == IntentStatus::Ok && intent.target_100ns == 70 * S);
    CHECK(say("louder").status == IntentStatus::Ok && say("again").status == IntentStatus::Ok && say("שוב").status == IntentStatus::Ok && engine.bounds.volume == 0.65f);
    CHECK(say("otra vez").status == IntentStatus::Ok && say("noch einmal").status == IntentStatus::Ok && say("再来一次").status == IntentStatus::Ok && engine.bounds.volume == 0.8f);
    const auto before_undo = engine.bounds.position_100ns;
    CHECK(say("back 10 seconds").status == IntentStatus::Ok && say("撤销").status == IntentStatus::Ok && engine.bounds.position_100ns == before_undo);
    // Anything else done ends what "again" repeats: the line on screen again.
    CHECK(say("pause").status == IntentStatus::Ok && say("again").kind == IntentKind::ReplayCue);
    // A new search replaces the quote; the conversation without a search, or without a dispatcher's conversation.
    conversation.Reset();
    CHECK(conversation.Query().empty() && say("undo").status == IntentStatus::NoContext && say("again").kind == IntentKind::ReplayCue);
    Dispatcher alone; alone.Configure(Act, &engine);
    CHECK(alone.Dispatch("undo", engine.Now()).status == IntentStatus::NoContext && alone.Dispatch("next match", searched).status == IntentStatus::NoContext);
    CHECK(std::strcmp(Name(IntentKind::NthMatch), "NthMatch") == 0 && std::strcmp(Name(IntentStatus::NoContext), "NoContext") == 0 &&
          std::strcmp(Name(IntentKind::CompareOff), "CompareOff") == 0);
}
// Interpret, the search field's rule: a command when the grammar reads one, else the text is a quote to look for.
void Interpreting() {
    const auto quote = [](std::string_view text, const MediaBounds& bounds, std::string_view expected) {
        const auto intent = Dispatcher::Interpret(text, bounds);
        return intent.status == IntentStatus::Ok && intent.kind == IntentKind::FindSubtitle && intent.literal && intent.query == expected &&
               intent.query.data() >= text.data() && intent.query.data() + intent.query.size() <= text.data() + text.size();
    };
    // Commands stay commands, in any language, and keep their refusals when the media cannot follow.
    for (const char* text : {"pause", "暂停", "一時停止", "пауза", "pausar", "توقف", "עצור", "Anhalten", "mettre en pause"}) {
        const auto intent = Dispatcher::Interpret(text, kFilm);
        CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::Pause && !intent.literal);
    }
    auto intent = Dispatcher::Interpret("快退10秒", kFilm);
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::SeekRelative && intent.target_100ns == 590 * S);
    intent = Dispatcher::Interpret("find hello", kFilm);
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::FindSubtitle && !intent.literal && intent.query == "hello");
    CHECK(Dispatcher::Interpret("forward 2 hours", kFilm).status == IntentStatus::OutOfRange && Dispatcher::Interpret("volume 250", kFilm).status == IntentStatus::OutOfRange);
    // Anything else is the quote itself: unknown words, and words that only start like a command.
    CHECK(quote("the quiet between", kFilm, "the quiet between") && quote("  שלום עולם ", kFilm, "שלום עולם") && quote("日本語", kFilm, "日本語"));
    CHECK(quote("play it again sam", kFilm, "play it again sam") && quote("播放器", kFilm, "播放器") && quote("stop the car", kFilm, "stop the car"));
    CHECK(quote("مرحبا بالعالم", kFilm, "مرحبا بالعالم") && quote("привет", kFilm, "привет") && quote("find", kFilm, "find"));
    // Quotation marks force a quote, whatever the words say.
    CHECK(quote("\"pause\"", kFilm, "pause") && quote("「一時停止」", kFilm, "一時停止") && quote("«volume 50»", kFilm, "volume 50") && quote(" 'mute' ", kFilm, "mute"));
    CHECK(Dispatcher::Interpret("\"\"", kFilm).status != IntentStatus::Ok);
    // Nothing to search: unknown text stays unknown, a forced quote is unavailable, commands still work.
    auto silent_film = kFilm; silent_film.subtitles = false;
    CHECK(Dispatcher::Interpret("the quiet between", silent_film).status == IntentStatus::Unknown && Dispatcher::Interpret("play it again", silent_film).status == IntentStatus::Malformed);
    CHECK(Dispatcher::Interpret("\"pause\"", silent_film).status == IntentStatus::Unavailable && Dispatcher::Interpret("pause", silent_film).status == IntentStatus::Ok);
    // Limits and bad text.
    static char long_quote[Dispatcher::kMaxQueryBytes + 40];
    std::memset(long_quote, 'z', sizeof(long_quote) - 1);
    CHECK(Dispatcher::Interpret(long_quote, kFilm).status == IntentStatus::TooLong && Dispatcher::Interpret(long_quote, kFilm).kind == IntentKind::FindSubtitle);
    long_quote[Dispatcher::kMaxQueryBytes] = 0;
    CHECK(quote(long_quote, kFilm, long_quote));
    CHECK(Dispatcher::Interpret("  \t", kFilm).status == IntentStatus::Empty && Dispatcher::Interpret("hello\xff", kFilm).status == IntentStatus::InvalidText);
    // Submit executes what Interpret read.
    Dispatcher dispatcher; Recorder recorder; dispatcher.Configure(Record, &recorder);
    intent = dispatcher.Submit("the quiet", kFilm);
    CHECK(intent.status == IntentStatus::Ok && recorder.calls == 1 && recorder.last.kind == IntentKind::FindSubtitle && recorder.last.query == "the quiet" && recorder.last.literal);
    intent = dispatcher.Submit("音量50", kFilm);
    CHECK(intent.status == IntentStatus::Ok && recorder.calls == 2 && recorder.last.kind == IntentKind::VolumeSet && recorder.last.volume == 0.5f);
    CHECK(dispatcher.Submit("forward 2 hours", kFilm).status == IntentStatus::OutOfRange && recorder.calls == 2);
    recorder.answer = IntentStatus::NotFound;
    CHECK(dispatcher.Submit("xyzzy", kFilm).status == IntentStatus::NotFound && recorder.calls == 3);
    const auto& stats = dispatcher.Stats();
    CHECK(stats.commands == 4 && stats.executed == 2 && stats.refused == 1 && stats.failed == 1);
}
// One parse, measured on commands of every language. Batches are timed whole and the fastest batch counts: what the
// code costs, not what else the machine was doing.
void Latency() {
    constexpr const char* commands[]{"pause", "volume up 20", "rewind a minute and a half", "go to 1:02:03", "find hello world", "set the volume to 5 percent",
        "עצור", "חזור 10 שניות אחורה", "הגבר ב-20", "توقف", "ارجع ٣٠ ثانية", "ابحث عن مرحبا", "暂停", "快退10秒", "快进一百二十秒", "查找你好", "音量调到30",
        "一時停止してください", "10秒巻き戻し", "音量を50に", "こんにちはを検索", "пауза", "перемотай на 10 секунд назад", "громкость 40 процентов",
        "pausar el video por favor", "retroceder un minuto y 30 segundos", "volumen al 35 por ciento", "mettre en pause", "reculer une minute et demie",
        "volume à 60 pour cent", "zurückspulen 10 Sekunden", "Lautstärke auf 40 Prozent", "vorspulen eine halbe Stunde", "make coffee", "the quiet between the frames"};
    constexpr unsigned rounds = 400, batches = 25;
    double best = 1e30; unsigned accepted = 0;
    allocation_probe::Begin();
    for (unsigned b = 0; b < batches; ++b) {
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned r = 0; r < rounds; ++r) for (const char* command : commands) accepted += Dispatcher::Interpret(command, kFilm).status == IntentStatus::Ok;
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / (rounds * std::size(commands));
        if (ns < best) best = ns;
    }
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && accepted == batches * rounds * std::size(commands));
    // Optimised builds must stay under a microsecond per command; unoptimised and sanitised builds only report.
#if (defined(NDEBUG) || defined(__OPTIMIZE__)) && !defined(__SANITIZE_ADDRESS__)
    CHECK(best < 1000);
#endif
    std::printf("Latency: %.0f ns per command (fastest of %u batches of %zu commands in nine languages), %llu allocations\n", best, batches,
                rounds * std::size(commands), static_cast<unsigned long long>(allocations));
}
// Random bytes and random sentences of grammar words: never a crash, never an out-of-range action, the same answer
// every time, and no allocation.
void Fuzz() {
    constexpr const char* words[]{"play", "pause", "volume", "up", "down", "mute", "unmute", "rewind", "forward", "back", "skip", "go", "to", "seek", "find", "search",
        "for", "the", "a", "an", "and", "half", "please", "by", "10", "30", "0", "100", "250", "999999", "1:30", "1:02:03", "5m", "30s", "1h2m3s", "50%", "second",
        "seconds", "minute", "minutes", "hour", "hours", "percent", "נגן", "עצור", "הגבר", "תנמיך", "השתק", "חזור", "קדימה", "אחורה", "דלג", "קפוץ", "מצא", "חפש",
        "שניות", "שנייה", "דקה", "דקות", "שעה", "חצי", "וחצי", "רבע", "שתי", "ב-20", "ל-1:30", "ו-30", "אחוז", "בבקשה", "את", "\"", "\xff", "\xe2\x80\x8f",
        "暂停", "播放", "快退", "快进10秒", "快退十秒", "查找", "音量", "音量50", "增大音量", "请", "秒", "分钟", "三十", "半", "一時停止", "再生", "巻き戻し", "10秒戻る",
        "に移動", "を検索", "音量を", "50に", "ください", "пауза", "назад", "вперёд", "на", "секунд", "минуту", "полторы", "найти", "громкость", "и",
        "pausar", "retroceder", "avanzar", "segundos", "atrás", "por", "favor", "ciento", "y", "medio", "buscar", "volumen", "reculer", "avancer",
        "secondes", "en", "arrière", "et", "demie", "chercher", "pour", "cent", "zurückspulen", "vorspulen", "Sekunden", "zurück", "und", "halbe",
        "suche", "Lautstärke", "auf", "Prozent", "توقف", "شغل", "ارجع", "تقدم", "ثواني", "دقيقة", "و30", "للخلف", "ابحث", "عن", "الصوت", "٣٠٪",
        "１０", "％", "。", "「", "」", "999999小时", "九百九十九", "next", "match", "one", "second", "last", "undo", "again", "where", "was", "i",
        "compare", "split", "view", "בטל", "שוב", "עוד", "פעם", "השני", "התוצאה", "הבאה", "השווה", "איכות", "לאיפה", "שהייתי"};
    std::uint32_t state = 0x1234567u;
    const auto next = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
    static char command[1024];
    unsigned accepted = 0, unstable = 0, unsafe = 0; std::uint64_t parsed = 0;
    const auto check = [&](std::string_view text) {
        const auto first = Dispatcher::Parse(text, kFilm), second = Dispatcher::Parse(text, kFilm);
        ++parsed;
        if (!Same(first, second)) ++unstable;
        if (first.status != IntentStatus::Ok) return;
        ++accepted;
        const bool seek = first.kind == IntentKind::SeekRelative || first.kind == IntentKind::SeekAbsolute;
        if (seek && (first.target_100ns < 0 || first.target_100ns >= kFilm.duration_100ns)) ++unsafe;
        if (!(first.volume >= 0 && first.volume <= 1)) ++unsafe;
        if (first.kind == IntentKind::FindSubtitle && (first.query.empty() || first.query.size() > Dispatcher::kMaxQueryBytes ||
            first.query.data() < text.data() || first.query.data() + first.query.size() > text.data() + text.size())) ++unsafe;
        if (first.kind == IntentKind::None) ++unsafe;
    };
    allocation_probe::Begin();
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 60000; ++i) { // Sentences of one to eight grammar words.
        std::size_t n = 0; const unsigned count = 1 + next() % 8;
        for (unsigned w = 0; w < count; ++w) {
            const char* word = words[next() % std::size(words)];
            if (w) command[n++] = next() % 16 ? ' ' : '\t';
            for (; *word; ++word) command[n++] = *word;
        }
        check({command, n});
    }
    for (unsigned i = 0; i < 40000; ++i) { // Random bytes, any length up to beyond the limit now and then.
        const std::size_t n = next() % 64 == 0 ? 500 + next() % 100 : next() % 80;
        for (std::size_t k = 0; k < n; ++k) command[k] = static_cast<char>(next() % 8 == 0 ? next() : 32 + next() % 96);
        check({command, n});
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    const auto allocations = allocation_probe::End();
    CHECK(unstable == 0 && unsafe == 0 && allocations == 0 && accepted > 500);
    std::printf("Fuzz: %llu commands (%u accepted), %u unstable, %u unsafe, %llu allocations; %.2f us per parse\n", static_cast<unsigned long long>(parsed), accepted,
                unstable, unsafe, static_cast<unsigned long long>(allocations), elapsed / (2.0 * static_cast<double>(parsed)));
}
// A streaming recogniser's pieces, collected and parsed by the same grammar as typed text.
void Utterances() {
    UtteranceBuffer spoken;
    CHECK(spoken.Empty() && Dispatcher::Parse(spoken.Text(), kFilm).status == IntentStatus::Empty);
    // Words, then sub-word pieces that continue a word ("rew" + "ind"), in English and Hebrew.
    CHECK(spoken.Push("rew") && spoken.Push("ind", false) && spoken.Push("30") && spoken.Push("seconds"));
    auto intent = Dispatcher::Parse(spoken.Text(), kFilm);
    CHECK(spoken.Text() == "rewind 30 seconds" && intent.status == IntentStatus::Ok && intent.kind == IntentKind::SeekRelative && intent.target_100ns == 570 * S);
    spoken.Clear(); CHECK(spoken.Push("חזור") && spoken.Push("דקה") && spoken.Push("וחצי"));
    intent = Dispatcher::Parse(spoken.Text(), kFilm);
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::SeekRelative && intent.target_100ns == 510 * S);
    // A spaceless script: pieces are joined as they come.
    spoken.Clear(); CHECK(spoken.Push("快退", false) && spoken.Push("10", false) && spoken.Push("秒", false));
    intent = Dispatcher::Parse(spoken.Text(), kFilm);
    CHECK(intent.status == IntentStatus::Ok && intent.kind == IntentKind::SeekRelative && intent.target_100ns == 590 * S);
    // A revised hypothesis replaces the text.
    CHECK(spoken.Replace("volume 35%") && Dispatcher::Parse(spoken.Text(), kFilm).volume == 0.35f);
    // An utterance longer than the grammar accepts keeps its beginning and says so; nothing is cut mid-piece.
    spoken.Clear(); unsigned pushed = 0;
    while (spoken.Push("louder")) ++pushed;
    CHECK(pushed == 73 && spoken.Overflowed() && spoken.Text().size() == 73 * 7 - 1 && !spoken.Push("x") && spoken.Text().size() <= UtteranceBuffer::kCapacity);
    spoken.Clear(); CHECK(!spoken.Overflowed() && spoken.Empty() && spoken.Push("mute") && Dispatcher::Parse(spoken.Text(), kFilm).kind == IntentKind::Mute);
}
}
int main() {
    std::puts("Intent dispatcher tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    allocation_probe::Begin();
    Grammar(); Bounds(); Dispatching(); Interpreting(); Utterances(); Conversation(); Tracks(); Files(); Speeds(); FollowUps();
    const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
    for (unsigned i = 0; i < languages; ++i) std::printf("%-14s %3u commands, %d failures\n", report[i].name, report[i].commands, report[i].failures);
    std::printf("Grammar: %zu accepted commands, %zu refusals\n", std::size(kAccepted) + std::size(kArabic) + std::size(kChinese) + std::size(kJapanese) +
                std::size(kRussian) + std::size(kSpanish) + std::size(kFrench) + std::size(kGerman) + std::size(kNormalized) + std::size(kConversation) +
                std::size(kTracks), std::size(kRefused) + std::size(kConversationRefused));
    {
        // Sentences cost more than bare commands: they are longer and their framing is looked up. The fastest of 25
        // batches, as for the commands below.
        auto film = kFilm; film.audio_tracks = 3; film.subtitle_tracks = 2;
        double best = 1e9; unsigned sink = 0;
        for (unsigned batch = 0; batch < 25; ++batch) {
            const auto begin = std::chrono::steady_clock::now();
            for (unsigned round = 0; round < 100; ++round) {
                for (const auto& c : kConversation) sink += static_cast<unsigned>(Dispatcher::Parse(c.text, film).kind);
                for (const auto& c : kTracks) sink += static_cast<unsigned>(Dispatcher::Parse(c.text, film).kind);
            }
            const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / (100.0 * (std::size(kConversation) + std::size(kTracks)));
            best = ns < best ? ns : best;
        }
        CHECK(sink != 0);
        // Optimised builds stay under 2 µs per sentence; unoptimised and sanitised builds only report.
#if (defined(NDEBUG) || defined(__OPTIMIZE__)) && !defined(__SANITIZE_ADDRESS__)
        CHECK(best < 2000);
#endif
        std::printf("Sentence latency: %.0f ns per conversational or track command (fastest of 25 batches)\n", best);
    }
    Latency(); Fuzz();
    std::printf("Intent dispatcher: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations)); return failures ? 1 : 0;
}
