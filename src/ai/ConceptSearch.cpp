#include "media/ai/ConceptSearch.hpp"
#include <algorithm>
#include <cstring>

namespace media::ai {
namespace {
// The concepts: one line each, "name: members". Members are separated by spaces; `_` joins a two-word phrase; `|3`,
// `|2` and `|1` set the weight of the members that follow (3 at the start of each line). Every member is written as
// folded text reads it (lower case, no points). The "stop" line lists stop words (no concept, weight 0).
// English first, then Hebrew, then Arabic where present. A word may belong to several concepts.
constexpr char kSource[] =
// Stop words. Some only keep a Hebrew word from being read as a shorter one after its "prefix" (שים is not ש + ים "sea",
// שבת not ש + בת "daughter", בקרב "among" not ב + קרב "battle").
"stop: the a an and or but of to in on at for with from by as is are was were be been being am i you he she it we they me him her us them "
"my your his its our their this that these those there here what who which when where why how not no yes do does did have has had will "
"would can could should just very so too also all any some more most only than then now up down out over again if into about off oh hey "
"get got let lets go going gonna want wanna know think like one two really well okay ok yeah kind "
"של את על עם זה זאת זו הוא היא הם הן אני אתה אתם אתן אנחנו לא כן גם רק כל מה מי איפה למה איך כי אם או אבל עוד כבר יש אין היה הייתה "
"יהיה אל מן מאוד שם פה כאן עכשיו אז הנה לי לך לו לה לנו להם אותו אותה אותי שלי שלך שלו שלה שלנו טוב בסדר שים שבת בקרב משקל "
"في من على الى عن مع هذا هذه هو هي انا انت نحن لا نعم ما ماذا كل قد\n"
"danger: danger dangerous dangers peril perilous risk risky hazard hazardous threat threats threaten unsafe careful caution cautious "
"beware warning warn watch_out look_out heads_up deadly lethal |2 trap traps ambush emergency alarm alert "
"|3 סכנה סכנות מסוכן מסוכנת מסוכנים מסוכנות זהירות זהיר זהירה זהירים היזהר היזהרי תיזהר תיזהרי תיזהרו אזהרה אזהרות שים_לב שימי_לב "
"שימו_לב איום איומים מאיים קטלני קטלנית |2 מלכודת מלכודות מארב אזעקה חירום |3 خطر خطير انتبه احذر حذار |2 فخ مصيدة\n"
"vehicle: car cars automobile automobiles auto vehicle vehicles truck trucks lorry van bus jeep taxi cab motorcycle motorbike |2 drive "
"driving drove driven driver drivers highway traffic garage parking engine fuel |1 road roads wheel |3 מכונית מכוניות רכב רכבים אוטו "
"אוטובוס משאית משאיות טנדר מונית ג'יפ אופנוע |2 נהיגה נוהג נוהגת נהג נהגת לנהוג נסיעה נוסע נוסעת חניה חניון דלק מנוע הגה |1 כביש כבישים "
"|3 سيارة سيارات شاحنة حافلة\n"
"accident: accident accidents crash crashed collision wreck |3 תאונה תאונות התנגשות התרסקות התרסק |3 حادث\n"
"police: police cop cops officer sheriff detective |2 arrest arrested handcuffs |3 משטרה שוטר שוטרת שוטרים בלש בלשית |2 מעצר אזיקים עצור "
"|3 شرطة شرطي\n"
"justice: justice law lawyer court judge trial guilty innocent verdict |2 jail prison crime criminal served |3 צדק חוק חוקים עורך_דין "
"עורכת_דין בית_משפט בית_המשפט שופט שופטת משפט אשם אשמה |2 כלא אסיר פשע פושע |3 عدالة قانون محكمة\n"
"revenge: revenge avenge vengeance payback retaliate |3 נקמה לנקום נוקם נוקמת נקמתי |3 انتقام ثار\n"
"fight: fight fights fighting fought punch attack attacked struggle brawl |2 hit beat |3 קרב קרבות מכות להכות היכה תקיפה מתקפה התקפה "
"מאבק להילחם נלחם נלחמת נלחמים |3 قتال معركة\n"
"war: war wars army soldier soldiers military troops enemy enemies |2 battle |3 מלחמה מלחמות צבא חייל חיילים חיילת אויב אויבים |2 קרב "
"|3 حرب جيش جندي\n"
"weapon: gun guns pistol rifle weapon weapons knife sword bomb bullet bullets |2 shoot shot shooting |3 אקדח רובה נשק סכין חרב פצצה "
"|2 ירי יריות לירות ירה |3 سلاح مسدس سكين\n"
"death: death dead die died dying kill killed killer murder murdered |2 funeral grave corpse |3 מוות מת מתה מתים למות הרג הרגו רצח רוצח "
"נרצח |2 הלוויה קבר גופה |3 موت ميت قتل\n"
"fear: fear afraid scared frightened terrified scary horror panic |2 nightmare |3 פחד פוחד פוחדת מפחד מפחדת מפחיד מפוחד אימה בהלה |2 "
"סיוט |3 خوف خائف\n"
"anger: angry anger mad furious rage |2 hate |3 כועס כועסת כעס זועם זועמת עצבני עצבנית |2 שנאה שונא |3 غضب غاضب\n"
"joy: happy happiness joy glad cheerful delighted |2 smile smiling |3 שמח שמחה שמחים אושר מאושר מאושרת |2 חיוך מחייך מחייכת |3 سعيد فرح\n"
"sadness: sad sadness unhappy sorrow grief depressed lonely |2 cry crying cried tears |3 עצוב עצובה עצב בודד בודדה |2 בוכה בכי בכה "
"דמעות |3 حزين حزن\n"
"laughter: laugh laughs laughing laughed funny joke jokes hilarious |2 mock mocking giggle |3 צחוק צוחק צוחקת צחקו מצחיק בדיחה בדיחות "
"|2 לעג בלעג |3 ضحك\n"
"love: love loves loved lover kiss kissed romance romantic |2 darling sweetheart heart |3 אהבה אוהב אוהבת אהבתי נשיקה נשיקות רומנטי "
"|2 מתוק מותק יקירי יקירתי לב |3 حب حبيبي\n"
"wedding: wedding marry married marriage bride groom |2 husband wife |3 חתונה להתחתן נשוי נשואה כלה חתן |2 בעל אישה |3 زواج عرس\n"
"family: family relatives |1 mother mom mum father dad parents brother sister son daughter grandma grandpa grandmother grandfather uncle "
"aunt |3 משפחה קרובים |1 אמא אבא הורים אח אחות בן בת סבא סבתא דוד דודה |3 عائلة\n"
"mother: mother mom mum mommy |2 parents |3 אמא |2 הורים |3 ام\n"
"father: father dad daddy |2 parents |3 אבא |2 הורים |3 اب\n"
"sibling: brother brothers sister sisters |3 אח אחים אחות אחיות |3 اخ اخت\n"
"grandparent: grandma grandpa grandmother grandfather |3 סבא סבתא |3 جد جدة\n"
"child: baby babies child children kid kids boy girl |2 son daughter |3 תינוק תינוקת ילד ילדה ילדים ילדות |2 בן בת |3 طفل اطفال\n"
"friend: friend friends buddy pal friendship |3 חבר חברה חברים חברות ידיד ידידה |3 صديق اصدقاء\n"
"food: food eat eating ate hungry dinner lunch breakfast meal |2 bread pizza cook cooking kitchen |3 אוכל לאכול אוכלים אכלתי רעב רעבה "
"ארוחה ארוחת |2 לחם פיצה מבשל לבשל מטבח |3 طعام اكل جائع\n"
"fruit: fruit fruits |2 apple apples pear banana |3 פרי פירות |2 תפוח תפוחים אגס בננה |3 فاكهة |2 تفاح\n"
"apple: apple apples |3 תפוח תפוחים |3 تفاح\n"
"drink: drink drinking drank thirsty |2 coffee tea beer wine |3 לשתות שותה שתה שתו שתיתי שתיה צמא צמאה |2 קפה תה בירה יין |3 شرب\n"
"money: money cash dollar dollars pay paid rich poor bank |2 price expensive cheap buy bought sell |3 כסף כספים שקל שקלים דולר דולרים "
"לשלם שילם עשיר עני בנק |2 מחיר יקר זול לקנות קנה למכור |3 مال فلوس\n"
"theft: steal stole stolen thief thieves robbery rob robbed |3 גנב גנבה גנבו לגנוב שוד שודד |3 سرقة لص\n"
"secret: secret secrets hide hidden |2 lie lies lying liar |3 סוד סודות להסתיר מוסתר |2 שקר שקרים משקר שקרן |3 سر\n"
"truth: truth true honest honesty |3 אמת אמיתי כנות |3 حقيقة\n"
"help: help helping helped rescue save saved protect protecting protected |3 עזרה לעזור עוזר עוזרת תעזור הצילו הציל להציל הגנה להגן "
"הגנו |3 مساعدة ساعد النجدة\n"
"escape: escape escaped flee fled chase chasing |2 run running ran hurry |3 לברוח בורח בורחת ברח בריחה מרדף רודף |2 רץ רצה לרוץ "
"מהר |3 هرب\n"
"sleep: sleep sleeping asleep slept nap snore snoring |2 tired bed dream |3 לישון ישן ישנה ישנים שינה נוחר נחירה |2 עייף עייפה "
"מיטה חלום חלמתי |3 نوم نائم\n"
"wake: wake wakes waking awake woke |2 morning sunrise dawn |3 להתעורר מתעורר מתעוררת התעוררתי |2 בוקר זריחה |3 صباح\n"
"night: night dark darkness midnight |2 evening |3 לילה חושך חשוך חצות |2 ערב |3 ليل ظلام\n"
"nature: nature forest forests woods jungle |2 tree trees flower flowers garden grass |3 טבע יער יערות ג'ונגל |2 עץ עצים פרח פרחים "
"גינה דשא |3 غابة |2 شجرة\n"
"tree: tree trees |3 עץ עצים |3 شجرة\n"
"flower: flower flowers |3 פרח פרחים |3 زهرة\n"
"animal: animal animals creature creatures wildlife |1 rabbit bunny squirrel bird dog cat horse butterfly bee fly |3 חיה חיות "
"בעל_חיים |1 ארנב סנאי ציפור כלב חתול סוס פרפר זבוב דבורה |3 حيوان |1 ارنب سنجاب\n"
"rabbit: rabbit rabbits bunny bunnies hare |3 ארנב ארנבת ארנבים ארנבון |3 ارنب\n"
"squirrel: squirrel squirrels |3 סנאי סנאים |3 سنجاب سناجب\n"
"bird: bird birds |3 ציפור ציפורים |3 طائر عصفور\n"
"dog: dog dogs puppy |3 כלב כלבים כלבה כלבלב |3 كلب\n"
"cat: cat cats kitten |3 חתול חתולים חתולה |3 قطة\n"
"horse: horse horses |3 סוס סוסים |3 حصان\n"
"butterfly: butterfly butterflies |3 פרפר פרפרים |3 فراشة\n"
"insect: insect insects bee bees |2 fly flies |3 חרק חרקים זבוב זבובים דבורה דבורים |3 حشرة\n"
"sound: noise sound noisy loud |2 chirp chirping buzz buzzing thud bang knock |3 רעש קול רועש |2 מצייץ מצייצות ציוץ מזמזם זמזום "
"חבטה דפיקה |3 صوت ضجيج\n"
"calm: calm quiet peaceful peace relax relaxed silent silence |3 שקט שקטה רגוע רגועה שלווה |3 هدوء هادئ\n"
"water: water sea ocean river lake beach |2 swim swimming wave |3 מים ים אוקיינוס נהר אגם חוף |2 לשחות שוחה גל |3 ماء بحر\n"
"weather: rain raining storm snow wind weather |2 cold hot sunny |3 גשם סערה שלג רוח מזג_אוויר |2 קר חם שמש |3 مطر\n"
"fire: fire burn burning flames smoke explosion explode exploded |3 אש שריפה בוער בוערת שורף להבות עשן פיצוץ התפוצץ |3 نار حريق\n"
"medical: doctor hospital sick ill nurse medicine ambulance |2 hurt pain injured wound |3 רופא רופאה בית_חולים בית_החולים חולה חולים תרופה "
"אמבולנס |2 כאב פצוע פצועה פצע |3 طبيب مستشفى\n"
"home: home house apartment |2 room door |3 בית הביתה דירה |2 חדר דלת |3 بيت منزل\n"
"city: city town street streets downtown |3 עיר רחוב רחובות |3 مدينة شارع\n"
"travel: travel trip journey vacation flight airport |3 טיול חופשה טיסה שדה_תעופה מסע |2 נסיעה |3 سفر رحلة\n"
"aircraft: plane airplane aircraft helicopter pilot |3 מטוס מסוק טייס טייסת |3 طائرة\n"
"train: train trains railway |2 station |3 רכבת רכבות |2 תחנה |3 قطار\n"
"ship: ship boat sail sailing captain |3 ספינה סירה אונייה רב_חובל קפטן |3 سفينة قارب\n"
"phone: phone call called calling message |3 טלפון להתקשר התקשר שיחה הודעה |3 هاتف\n"
"music: music song songs sing singing dance dancing |3 מוזיקה שיר שירים לשיר רוקד רוקדת לרקוד ריקוד |3 موسيقى اغنية\n"
"party: party celebrate celebration birthday gift congratulations |3 מסיבה חגיגה לחגוג יום_הולדת יום_ההולדת מתנה מתנות מזל_טוב |3 حفلة\n"
"time: late hour hours minute minutes wait waiting clock |3 מאוחר מאחר זמן שעה שעות דקה דקות לחכות מחכה חכה |3 وقت\n"
"school: school teacher student class lesson study homework exam |3 בית_ספר בית_הספר מורה תלמיד תלמידה כיתה שיעור ללמוד לומד מבחן |3 مدرسة\n"
"work: work job boss office business |3 עבודה לעבוד עובד עובדת בוס משרד עסק |3 عمل\n"
"greeting: hello hi goodbye bye welcome |3 שלום להתראות ביי ברוך_הבא |3 مرحبا\n"
"apology: sorry apologize forgive |2 thanks thank |3 סליחה מצטער מצטערת לסלוח |2 תודה |3 اسف |2 شكرا\n"
"royal: king queen prince princess castle kingdom throne crown |3 מלך מלכה נסיך נסיכה ארמון טירה ממלכה כתר |3 ملك\n"
"religion: god pray prayer church heaven hell angel |3 אלוהים להתפלל תפילה גן_עדן גיהנום מלאך |3 الله\n"
"magic: magic wizard witch spell dragon monster ghost |3 קסם קוסם מכשפה כישוף דרקון מפלצת רוח_רפאים |3 سحر\n"
"computer: computer internet email password hack hacker robot |3 מחשב אינטרנט סיסמה האקר רובוט |3 حاسوب\n"
"game: game games ball football soccer team |2 win won lose lost victory |3 משחק כדור כדורגל קבוצה |2 ניצחון ניצחנו להפסיד הפסד |3 مباراة\n"
"insult: stupid idiot fool ugly |2 fat |3 טיפש טיפשה אידיוט מכוער מכוערת |2 שמן שמנה |3 غبي\n"
"size: big giant huge large enormous |2 tiny little |3 גדול גדולה ענק ענקי ענקית |2 קטן קטנה זעיר |3 كبير\n"
"beauty: beautiful pretty gorgeous lovely |3 יפה יפהפה יפים מקסים מקסימה |3 جميل\n"
"kindness: nice gentle sweet |3 נחמד נחמדה עדין עדינה |3 لطيف\n"
"color: color colour colors red blue green yellow black white orange purple pink |3 צבע צבעים אדום אדומה כחול כחולה ירוק ירוקה צהוב "
"צהובה שחור שחורה לבן לבנה כתום סגול ורוד |3 لون احمر ازرق اخضر اسود ابيض\n"
"breaking: break breaks broke broken snap snaps snapped smash smashed destroy destroyed ruin |3 שבר נשבר נשברה לשבור הרס להרוס מנופץ "
"|3 كسر\n"
"falling: fall falls fell falling drop dropped |3 נופל נופלת נפל נפלה ליפול |3 سقط\n"
"annoyance: annoy annoying bully bullies tease teasing harass |3 מציק מציקה מציקים להציק מעצבן הצקה |3 مزعج مشاغب مشاغبة\n"
"capture: catch caught capture captured grab |2 trapped |3 לתפוס תפס תפסו נתפס נלכד נלכדה ללכוד לכד |3 قبض\n"
"plan: plan plans planning prepare prepares preparing ready |3 תוכנית תכנית מתכנן מתכננת מכין מכינה להכין מוכן מוכנה |3 خطة\n"
"cleverness: smart clever genius wise intelligent |3 חכם חכמה גאון נבון |3 ذكي\n"
"flight: fly flying flew wings |3 עף עפה מעופף לעוף כנפיים |3 طار\n"
"smell: smell smells sniff scent |3 מריח מריחה ריח |3 رائحة\n"
"ending: end ending finish finished |3 סוף נגמר נגמרה |3 نهاية\n"
"beginning: beginning start starts begin begins |3 התחלה מתחיל מתחילה מתחילים להתחיל |3 بداية\n"
"arrival: arrive arrives arriving coming |3 מגיע מגיעה מגיעים הגיע להגיע |3 قادمة قادم\n"
"cinema: movie movies film cinema actor actress scene |3 סרט סרטים קולנוע שחקן שחקנית סצנה |3 فيلم\n"
"light: light bright shine shining |3 אור בהיר זורח |3 ضوء\n";

constexpr unsigned kMaxEntries = 2048, kMaxKeyBytes = 63;
struct Entry { std::uint16_t offset; std::uint8_t length, id, weight; };
struct Concept { std::uint16_t offset; std::uint8_t length; };
struct Table {
    Entry entries[kMaxEntries]{};
    Concept concepts[ConceptLexicon::kMaxConcepts]{}; // Concept 0: the stop words.
    unsigned count = 0, concept_count = 0;
    bool overflow = false, malformed = false;
};
// Byte i of a member as compared: `_` is the phrase's space, a Hebrew final letter its ordinary form (in UTF-8 each final
// letter is its ordinary form's code less one, both after the lead byte D7).
unsigned char KeyByte(const char* s, std::size_t i) noexcept {
    auto b = static_cast<unsigned char>(s[i]);
    if (b == '_') return ' ';
    if (i && static_cast<unsigned char>(s[i - 1]) == 0xd7 && (b == 0x9a || b == 0x9d || b == 0x9f || b == 0xa3 || b == 0xa5)) ++b;
    return b;
}
int Compare(const char* a, std::size_t an, const char* b, std::size_t bn) noexcept {
    const auto n = (std::min)(an, bn);
    for (std::size_t i = 0; i < n; ++i) { const auto x = KeyByte(a, i), y = KeyByte(b, i); if (x != y) return x < y ? -1 : 1; }
    return an == bn ? 0 : an < bn ? -1 : 1;
}
Table Build() noexcept {
    Table t;
    std::size_t i = 0; const std::size_t size = sizeof(kSource) - 1;
    unsigned current = 0;
    while (i < size) {
        // "name:"
        const auto name = i;
        while (i < size && kSource[i] != ':' && kSource[i] != '\n') ++i;
        if (i >= size || kSource[i] != ':') { t.malformed = true; break; }
        const bool stop = i - name == 4 && std::memcmp(kSource + name, "stop", 4) == 0;
        if (!stop) {
            if (++current >= ConceptLexicon::kMaxConcepts) { t.overflow = true; break; }
            t.concepts[current] = {static_cast<std::uint16_t>(name), static_cast<std::uint8_t>(i - name)};
            t.concept_count = current;
        }
        ++i;
        std::uint8_t weight = stop ? 0 : 3;
        while (i < size && kSource[i] != '\n') {
            while (i < size && kSource[i] == ' ') ++i;
            const auto begin = i;
            while (i < size && kSource[i] != ' ' && kSource[i] != '\n') ++i;
            if (i == begin) continue;
            if (kSource[begin] == '|') {
                if (stop || i - begin != 2 || kSource[begin + 1] < '1' || kSource[begin + 1] > '3') t.malformed = true;
                else weight = static_cast<std::uint8_t>(kSource[begin + 1] - '0');
                continue;
            }
            if (i - begin > kMaxKeyBytes) { t.malformed = true; continue; }
            if (t.count == kMaxEntries) { t.overflow = true; continue; }
            t.entries[t.count++] = {static_cast<std::uint16_t>(begin), static_cast<std::uint8_t>(i - begin), static_cast<std::uint8_t>(stop ? 0 : current), weight};
        }
        if (i < size) ++i;
    }
    std::sort(t.entries, t.entries + t.count, [](const Entry& a, const Entry& b) {
        const int c = Compare(kSource + a.offset, a.length, kSource + b.offset, b.length);
        return c ? c < 0 : a.id < b.id;
    });
    return t;
}
const Table& Lexicon() noexcept { static const Table table = Build(); return table; }
static_assert(sizeof(kSource) < 65536, "Entry offsets are 16-bit");

// ---- Words in folded text ----
bool Hebrew(std::uint32_t c) noexcept { return c >= 0x5d0 && c <= 0x5f2; }
bool Arabic(std::uint32_t c) noexcept { return (c >= 0x620 && c <= 0x64a) || (c >= 0x660 && c <= 0x669) || (c >= 0x671 && c <= 0x6d3); }
bool Letter(std::uint32_t c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) ||
           (c >= 0x370 && c <= 0x52f) || Hebrew(c) || Arabic(c);
}
bool Joiner(std::uint32_t c) noexcept { return c == '\'' || c == 0x2019 || c == 0x5f3 || c == 0x5f4 || c == '"'; } // Inside a word only.
// The next word of `text` from `at`: its bytes (a view into text) and its script. Joiners between two letters stay in it.
enum class Script : std::uint8_t { Latin, Hebrew, Arabic };
struct Token { std::string_view text; Script script = Script::Latin; };
bool NextToken(std::string_view text, std::size_t& at, Token& token) noexcept {
    std::uint32_t c = 0;
    while (at < text.size()) { // Skip to a letter.
        auto rest = text.substr(at); const auto before = rest.size();
        if (!text::Utf8(rest, c)) { ++at; continue; }
        if (Letter(c)) break;
        at += before - rest.size();
    }
    if (at >= text.size()) return false;
    const auto begin = at; token.script = Script::Latin; bool scripted = false;
    while (at < text.size()) {
        auto rest = text.substr(at); const auto before = rest.size();
        if (!text::Utf8(rest, c)) break;
        const auto step = before - rest.size();
        if (Letter(c)) {
            if (!scripted && (Hebrew(c) || Arabic(c))) { token.script = Hebrew(c) ? Script::Hebrew : Script::Arabic; scripted = true; }
            if (!scripted && c > '9') scripted = true;
            at += step; continue;
        }
        if (Joiner(c) && at > begin) { // A joiner followed by a letter.
            auto next = rest; std::uint32_t d = 0;
            if (text::Utf8(next, d) && Letter(d)) { at += step; continue; }
        }
        break;
    }
    token.text = text.substr(begin, at - begin);
    return true;
}
// The entries whose key is `key` (already in compared form: no final letters, phrases with a space).
std::span<const Entry> Lookup(std::string_view key) noexcept {
    const auto& t = Lexicon();
    const auto less = [](const Entry& e, std::string_view k) { return Compare(kSource + e.offset, e.length, k.data(), k.size()) < 0; };
    const auto* first = std::lower_bound(t.entries, t.entries + t.count, key, less);
    const auto* last = first;
    while (last != t.entries + t.count && Compare(kSource + last->offset, last->length, key.data(), key.size()) == 0) ++last;
    return {first, static_cast<std::size_t>(last - first)};
}
// A word in compared form, in `out` (finals made ordinary); empty when too long.
std::string_view Compared(std::string_view word, char* out, std::size_t capacity) noexcept {
    if (word.size() > capacity) return {};
    for (std::size_t i = 0; i < word.size(); ++i) out[i] = static_cast<char>(KeyByte(word.data(), i));
    return {out, word.size()};
}
constexpr std::size_t kWordBytes = 2 * kMaxKeyBytes + 1;
// The lexicon's reading of one word (or phrase) in compared form: the entries of the first form found, unprefixed first.
std::span<const Entry> Read(std::string_view word, Script script) noexcept {
    if (auto found = Lookup(word); !found.empty()) return found;
    if (script == Script::Hebrew) {
        // Up to four prefix letters (each two bytes: "וכשה"), leaving at least two letters.
        for (std::size_t p = 1; p <= 4 && word.size() >= 2 * p + 4; ++p) {
            const auto letter = word.substr(2 * (p - 1), 2);
            static constexpr std::string_view kPrefixes[]{"ו", "ה", "ב", "ל", "מ", "ש", "כ"};
            if (std::find(std::begin(kPrefixes), std::end(kPrefixes), letter) == std::end(kPrefixes)) break;
            if (auto found = Lookup(word.substr(2 * p)); !found.empty()) return found;
        }
        return {};
    }
    if (script == Script::Arabic) {
        // A conjunction (و ف), a preposition (ب ل ك), the article (ال); "لل" is ل with the article.
        std::string_view rest = word;
        for (const std::string_view c : {std::string_view{"و"}, std::string_view{"ف"}})
            if (rest.size() > c.size() + 4 && rest.substr(0, c.size()) == c) { rest.remove_prefix(c.size()); if (auto f = Lookup(rest); !f.empty()) return f; break; }
        if (rest.size() > 6 && rest.substr(0, 4) == "لل") { if (auto f = Lookup(rest.substr(4)); !f.empty()) return f; }
        for (const std::string_view p : {std::string_view{"ب"}, std::string_view{"ل"}, std::string_view{"ك"}})
            if (rest.size() > p.size() + 4 && rest.substr(0, p.size()) == p) { rest.remove_prefix(p.size()); if (auto f = Lookup(rest); !f.empty()) return f; break; }
        if (rest.size() > 8 && rest.substr(0, 4) == "ال") { if (auto f = Lookup(rest.substr(4)); !f.empty()) return f; }
        return {};
    }
    // English endings: the plural, the possessive, -ies, -ing, -ed, -ly; a doubled consonant before -ing/-ed is single.
    char form[kWordBytes];
    const auto try_form = [&](std::string_view stem, std::string_view tail) -> std::span<const Entry> {
        if (stem.size() < 2 || stem.size() + tail.size() > sizeof(form)) return {};
        std::memcpy(form, stem.data(), stem.size()); std::memcpy(form + stem.size(), tail.data(), tail.size());
        return Lookup({form, stem.size() + tail.size()});
    };
    const auto ends = [&](std::string_view s) { return word.size() > s.size() && word.substr(word.size() - s.size()) == s; };
    const auto cut = [&](std::size_t n) { return word.substr(0, word.size() - n); };
    std::span<const Entry> f;
    if (ends("'s") && !(f = try_form(cut(2), "")).empty()) return f;
    if (ends("ies") && !(f = try_form(cut(3), "y")).empty()) return f;
    if (ends("es") && !(f = try_form(cut(2), "")).empty()) return f;
    if (ends("s") && !ends("ss") && !(f = try_form(cut(1), "")).empty()) return f;
    if (ends("ly") && !(f = try_form(cut(2), "")).empty()) return f;
    for (const std::string_view suffix : {std::string_view{"ing"}, std::string_view{"ed"}}) {
        if (!ends(suffix)) continue;
        const auto stem = cut(suffix.size());
        if (!(f = try_form(stem, "")).empty() || !(f = try_form(stem, "e")).empty()) return f;
        if (stem.size() >= 3 && stem[stem.size() - 1] == stem[stem.size() - 2] && !(f = try_form(stem.substr(0, stem.size() - 1), "")).empty()) return f;
    }
    return {};
}
void Add(std::span<const Entry> entries, std::span<ConceptHit> out, unsigned& count) noexcept {
    for (const auto& e : entries) {
        if (!e.id) continue;
        unsigned k = 0;
        while (k < count && out[k].id != e.id) ++k;
        if (k < count) { out[k].weight = (std::max)(out[k].weight, e.weight); continue; }
        if (count < out.size()) out[count++] = {e.id, e.weight};
    }
}
// Reads `text` word by word: a two-word phrase first, else the word. `visit(entries, token)` for each reading (entries
// empty: a word the lexicon does not know).
template <class Visit> void Words(std::string_view text, Visit&& visit) noexcept {
    std::size_t at = 0; Token token, next;
    bool have = NextToken(text, at, token);
    while (have) {
        std::size_t after = at;
        const bool more = NextToken(text, after, next);
        char buffer[kWordBytes];
        if (more && token.script == next.script && token.text.size() + 1 + next.text.size() <= kMaxKeyBytes) {
            std::memcpy(buffer, token.text.data(), token.text.size()); buffer[token.text.size()] = '_';
            std::memcpy(buffer + token.text.size() + 1, next.text.data(), next.text.size());
            char compared[kWordBytes];
            const auto phrase = Compared({buffer, token.text.size() + 1 + next.text.size()}, compared, sizeof(compared));
            if (const auto entries = Read(phrase, token.script); !entries.empty()) {
                visit(entries, token);
                at = after; have = NextToken(text, at, token); continue;
            }
        }
        char compared[kWordBytes];
        const auto word = Compared(token.text, compared, sizeof(compared));
        visit(word.empty() ? std::span<const Entry>{} : Read(word, token.script), token);
        at = after; token = next; have = more;
    }
}
// `word` is `literal`, or `literal` with prefixes (Hebrew, Arabic) or an English ending: the test for a query's words the
// lexicon does not know. Both are folded; Hebrew finals compare as ordinary letters.
// English: a word and the forms it may be the ending of ("bridges": "bridges", "bridge", "bridg"), at most kStems.
constexpr unsigned kStems = 8;
unsigned Stems(std::string_view word, std::string_view (&out)[kStems], char (&spare)[2][kWordBytes]) noexcept {
    unsigned n = 0; out[n++] = word;
    const auto ends = [&](std::string_view s) { return word.size() >= s.size() + 3 && word.substr(word.size() - s.size()) == s; };
    const auto cut = [&](std::size_t k) { return word.substr(0, word.size() - k); };
    const auto with = [&](std::string_view stem, char tail, unsigned slot) {
        if (stem.size() + 1 > kWordBytes) return std::string_view{};
        std::memcpy(spare[slot], stem.data(), stem.size()); spare[slot][stem.size()] = tail; return std::string_view{spare[slot], stem.size() + 1};
    };
    if (ends("'s")) out[n++] = cut(2);
    if (ends("ies")) out[n++] = with(cut(3), 'y', 0);
    else if (ends("es")) { out[n++] = cut(2); out[n++] = cut(1); }
    else if (ends("s") && !ends("ss")) out[n++] = cut(1);
    if (ends("ing")) { out[n++] = cut(3); out[n++] = with(cut(3), 'e', 1); }
    else if (ends("ed")) { out[n++] = cut(2); out[n++] = cut(1); }
    else if (ends("ly")) out[n++] = cut(2);
    return n;
}
bool SameWord(std::string_view word, std::string_view literal, Script script) noexcept {
    if (Compare(word.data(), word.size(), literal.data(), literal.size()) == 0) return true;
    if (script == Script::Latin) { // Any form of one is a form of the other ("bridges" finds "bridge", "driving" "drive").
        std::string_view a[kStems], b[kStems]; char sa[2][kWordBytes], sb[2][kWordBytes];
        const auto na = Stems(word, a, sa), nb = Stems(literal, b, sb);
        for (unsigned i = 0; i < na; ++i) for (unsigned k = 0; k < nb; ++k) if (!a[i].empty() && a[i] == b[k]) return true;
        return false;
    }
    if (word.size() <= literal.size()) return false;
    const auto extra = word.size() - literal.size();
    if (Compare(word.data() + extra, literal.size(), literal.data(), literal.size()) != 0) return false;
    const auto prefix = word.substr(0, extra);
    if (script == Script::Hebrew) {
        if (extra > 8) return false;
        for (std::size_t i = 0; i < extra; i += 2) {
            const auto letter = prefix.substr(i, 2);
            if (letter != "ו" && letter != "ה" && letter != "ב" && letter != "ל" && letter != "מ" && letter != "ש" && letter != "כ") return false;
        }
        return true;
    }
    return extra <= 8 && (prefix == "ال" || prefix == "و" || prefix == "ب" || prefix == "ل" || prefix == "ف" || prefix == "ك" || prefix == "وال" ||
                          prefix == "بال" || prefix == "فال" || prefix == "لل" || prefix == "كال");
}
}

unsigned ConceptLexicon::Analyse(std::string_view folded, std::span<ConceptHit> out) noexcept {
    unsigned count = 0;
    Words(folded, [&](std::span<const Entry> entries, const Token&) { Add(entries, out, count); });
    return count;
}
unsigned ConceptLexicon::Word(std::string_view folded, std::span<ConceptHit> out, bool* stop) noexcept {
    if (stop) *stop = false;
    std::size_t at = 0; Token token;
    if (!NextToken(folded, at, token)) return 0;
    char buffer[kWordBytes];
    const auto word = Compared(folded.substr(static_cast<std::size_t>(token.text.data() - folded.data())), buffer, sizeof(buffer));
    if (word.empty()) return 0;
    const auto entries = Read(word, token.script);
    unsigned count = 0;
    Add(entries, out, count);
    if (stop && !entries.empty() && !count) *stop = true;
    return count;
}
unsigned ConceptLexicon::Concepts() noexcept { return Lexicon().concept_count; }
unsigned ConceptLexicon::Entries() noexcept { return Lexicon().count; }
std::size_t ConceptLexicon::Bytes() noexcept { return sizeof(kSource) + sizeof(Table); }
std::string_view ConceptLexicon::Name(unsigned id) noexcept {
    const auto& t = Lexicon();
    if (!id || id > t.concept_count) return {};
    return {kSource + t.concepts[id].offset, t.concepts[id].length};
}
bool ConceptLexicon::TablesConsistent(TableReport report) noexcept {
    const auto& t = Lexicon();
    bool good = true;
    const auto fail = [&](const char* rule, std::string_view word) { good = false; if (report) report(rule, word); };
    if (t.malformed) fail("malformed source", {});
    if (t.overflow) fail("index overflow", {});
    for (unsigned i = 0; i < t.count; ++i) {
        const auto& e = t.entries[i];
        const std::string_view word{kSource + e.offset, e.length};
        char folded[kMaxKeyBytes];
        const auto n = text::Normalize(word, folded, sizeof(folded), text::kFold);
        if (n != word.size() || std::memcmp(folded, word.data(), n) != 0) fail("not folded", word);
        std::size_t at = 0; Token token; unsigned words = 0;
        std::string_view rest = word; std::size_t phrase_space = word.find('_');
        if (phrase_space != std::string_view::npos && word.find('_', phrase_space + 1) != std::string_view::npos) fail("more than two words", word);
        char spaced[kMaxKeyBytes]; std::memcpy(spaced, word.data(), word.size());
        for (std::size_t k = 0; k < word.size(); ++k) if (spaced[k] == '_') spaced[k] = ' ';
        rest = {spaced, word.size()};
        std::size_t covered = 0;
        while (NextToken(rest, at, token)) { ++words; covered += token.text.size(); }
        if (!words || covered + words - 1 != word.size()) fail("not words", word);
        if (i && Compare(kSource + t.entries[i - 1].offset, t.entries[i - 1].length, word.data(), word.size()) == 0 && t.entries[i - 1].id == e.id)
            fail("listed twice", word);
    }
    return good;
}

// ---- The index ----
// One term per word of the query: the concepts the word names (a cue scores the best of them), or the word itself.
struct ConceptIndex::Terms {
    struct Term { ConceptHit concepts[4]{}; unsigned concept_count = 0; std::string_view literal; Script script = Script::Latin; };
    Term terms[kMaxTerms]{}; unsigned count = 0;
    char literal_bytes[subtitle::SearchIndex::kMaxQueryBytes]{};
    unsigned Count() const noexcept { return count; }
};
namespace {
// A term counts in a cue from this score: a word of weight 1 alone (1 x 1) is too loose a tie to find anything by.
constexpr unsigned kMinimumTermScore = 2;
}
bool ConceptIndex::Prepare(core::LinearMemoryArena& arena, const subtitle::SearchIndex& search) noexcept {
    if (offsets_ || !search.Ready() || arena.IsSealed()) return false;
    const auto* track = search.Track();
    const auto count = static_cast<unsigned>(track->Cues().size());
    ConceptHit scratch[kMaxCueConcepts];
    std::size_t total = 0;
    for (unsigned i = 0; i < count; ++i) total += ConceptLexicon::Analyse(search.Folded(i), scratch);
    auto* offsets = static_cast<std::uint32_t*>(arena.Allocate(sizeof(std::uint32_t) * (count + 1), alignof(std::uint32_t)));
    auto* hits = static_cast<ConceptHit*>(arena.Allocate(sizeof(ConceptHit) * (total ? total : 1), alignof(ConceptHit)));
    if (!offsets || !hits) return false;
    std::size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
        offsets[i] = static_cast<std::uint32_t>(used);
        const auto n = ConceptLexicon::Analyse(search.Folded(i), scratch);
        for (unsigned k = 0; k < n && used < total; ++k) hits[used++] = scratch[k];
    }
    offsets[count] = static_cast<std::uint32_t>(used);
    search_ = &search; hits_ = hits; cues_ = count; offsets_ = offsets;
    bytes_ = sizeof(std::uint32_t) * (count + 1) + sizeof(ConceptHit) * total;
    return true;
}
std::span<const ConceptHit> ConceptIndex::CueConcepts(unsigned cue) const noexcept {
    if (!offsets_ || cue >= cues_) return {};
    return {hits_ + offsets_[cue], offsets_[cue + 1] - offsets_[cue]};
}
bool ConceptIndex::Read(std::string_view query, Terms& terms) const noexcept {
    if (!offsets_ || query.empty() || query.size() > subtitle::SearchIndex::kMaxQueryBytes) return false;
    char folded[subtitle::SearchIndex::kMaxQueryBytes];
    const auto n = text::Normalize(query, folded, sizeof(folded), text::kFold);
    if (n == text::kInvalid || !n) return false;
    std::size_t used = 0;
    Words({folded, n}, [&](std::span<const Entry> entries, const Token& token) {
        if (terms.count == kMaxTerms) return;
        auto& term = terms.terms[terms.count];
        term = {};
        if (!entries.empty()) { // Concepts, or a stop word (none).
            Add(entries, term.concepts, term.concept_count);
            if (!term.concept_count) return;
            for (unsigned k = 0; k < terms.count; ++k) { // A word said twice is one term.
                const auto& other = terms.terms[k];
                if (other.concept_count == term.concept_count && std::equal(other.concepts, other.concepts + other.concept_count, term.concepts,
                    [](const ConceptHit& a, const ConceptHit& b) { return a.id == b.id && a.weight == b.weight; })) return;
            }
            ++terms.count; return;
        }
        // A word the table does not know: a term of its own (three Latin letters or two others at least; once).
        const auto letters = token.script == Script::Latin ? token.text.size() : token.text.size() / 2;
        if (letters < (token.script == Script::Latin ? 3u : 2u) || used + token.text.size() > sizeof(terms.literal_bytes)) return;
        const auto compared = Compared(token.text, terms.literal_bytes + used, sizeof(terms.literal_bytes) - used);
        for (unsigned k = 0; k < terms.count; ++k) if (!terms.terms[k].concept_count && terms.terms[k].literal == compared) return;
        term.literal = compared; term.script = token.script; used += compared.size(); ++terms.count;
    });
    return terms.Count() != 0;
}
std::uint64_t ConceptIndex::Rank(const Terms& terms, unsigned cue, ConceptMatch* match) const noexcept {
    unsigned coverage = 0, score = 0; bool by_concept = false, literals = false;
    const auto hits = CueConcepts(cue);
    bool found[kMaxTerms]{};
    for (unsigned q = 0; q < terms.count; ++q) {
        const auto& term = terms.terms[q];
        if (!term.concept_count) { literals = true; continue; }
        unsigned best = 0;
        for (unsigned c = 0; c < term.concept_count; ++c)
            for (const auto& h : hits) if (h.id == term.concepts[c].id) best = (std::max)(best, unsigned{h.weight} * term.concepts[c].weight);
        if (best >= kMinimumTermScore) { ++coverage; score += best; by_concept = true; }
    }
    if (literals) {
        const auto folded = search_->Folded(cue);
        std::size_t at = 0; Token token;
        while (NextToken(folded, at, token))
            for (unsigned k = 0; k < terms.count; ++k) {
                const auto& term = terms.terms[k];
                if (!term.concept_count && !found[k] && token.script == term.script && SameWord(token.text, term.literal, token.script)) found[k] = true;
            }
        for (unsigned k = 0; k < terms.count; ++k) if (found[k]) { ++coverage; score += 9; }
    }
    if (!coverage) return 0;
    if (match) {
        const auto& c = search_->Track()->Cues()[cue];
        *match = {cue, c.start, c.end, coverage, terms.Count(), score, static_cast<float>(score) / static_cast<float>(9 * terms.Count()), by_concept};
    }
    // Coverage, then score, then the earlier cue: one sortable key, never 0 for a cue with a term.
    return std::uint64_t{coverage} << 48 | std::uint64_t{score} << 24 | (0xffffffu - cue);
}
unsigned ConceptIndex::Scan(const Terms& terms, unsigned match_index, ConceptMatch* match) const noexcept {
    const auto* track = search_->Track();
    if (!match) {
        unsigned found = 0;
        for (unsigned i = 0; i < cues_; ++i) if (track->Shown(i) && Rank(terms, i, nullptr)) ++found;
        return found;
    }
    // The match_index-th key in descending order, by successive maxima below the previous one: no scratch to sort in.
    std::uint64_t bound = UINT64_MAX;
    for (unsigned k = 0; k <= match_index; ++k) {
        std::uint64_t best = 0; unsigned at = 0;
        for (unsigned i = 0; i < cues_; ++i) {
            if (!track->Shown(i)) continue;
            const auto key = Rank(terms, i, nullptr);
            if (key && key < bound && key > best) { best = key; at = i; }
        }
        if (!best) return 0;
        bound = best;
        if (k == match_index) { (void)Rank(terms, at, match); return 1; }
    }
    return 0;
}
bool ConceptIndex::Find(std::string_view query, unsigned match_index, ConceptMatch& match) const noexcept {
    Terms terms;
    return Read(query, terms) && Scan(terms, match_index, &match) != 0;
}
unsigned ConceptIndex::Count(std::string_view query) const noexcept {
    Terms terms;
    return Read(query, terms) ? Scan(terms, 0, nullptr) : 0;
}
}
