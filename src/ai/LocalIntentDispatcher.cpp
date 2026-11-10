#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/text/Utf8.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <span>

namespace media::ai {
namespace {
using Dispatcher = LocalIntentDispatcher;
constexpr unsigned kMaxTokens = 16, kTokenBytes = 48;

// ---- Folding ---------------------------------------------------------------------------------------------------
// One scalar of a command word as the tables spell it. On top of the search folding (letter case, Hebrew points,
// Arabic harakat, invisible controls) a command ignores what keyboards and habits vary:
// - Latin accents and combining marks (zurück = zuruck, más = mas); Cyrillic ё = е and й = и;
// - Arabic teh marbuta = heh, alef maqsura and Farsi yeh = yeh, keheh = kaf;
// - fullwidth ASCII (１０％ = 10%), Arabic-Indic and Eastern Arabic digits, the Arabic percent sign.
// 0: the scalar is dropped. Never a space for a scalar that is not white space.
std::uint32_t Fold(std::uint32_t s) noexcept {
    s = text::FoldScalar(s, true);
    if (s < 0xe0) return s;
    if (s <= 0xff) {
        // à..å ç è..ë ì..ï ñ ò..ö ø ù..ü ý ÿ; æ, ð, ÷ and þ stay.
        static constexpr char base[33] = "aaaaaa\0ceeeeiiii\0nooooo\0ouuuuy\0y";
        const char b = base[s - 0xe0];
        return b ? static_cast<std::uint32_t>(b) : s;
    }
    if (s <= 0x17f) { // Latin Extended-A, already lower case.
        struct Range { std::uint16_t first, last; char base; };
        static constexpr Range ranges[]{{0x100, 0x105, 'a'}, {0x106, 0x10d, 'c'}, {0x10e, 0x111, 'd'}, {0x112, 0x11b, 'e'}, {0x11c, 0x123, 'g'},
            {0x124, 0x127, 'h'}, {0x128, 0x131, 'i'}, {0x134, 0x135, 'j'}, {0x136, 0x137, 'k'}, {0x139, 0x142, 'l'}, {0x143, 0x148, 'n'},
            {0x14c, 0x151, 'o'}, {0x154, 0x159, 'r'}, {0x15a, 0x161, 's'}, {0x162, 0x167, 't'}, {0x168, 0x173, 'u'}, {0x174, 0x175, 'w'},
            {0x176, 0x178, 'y'}, {0x179, 0x17e, 'z'}};
        for (const auto& r : ranges) if (s >= r.first && s <= r.last) return static_cast<std::uint32_t>(r.base);
        return s;
    }
    if (s >= 0x300 && s <= 0x36f) return 0; // Combining marks.
    if (s == 0x451) return 0x435;
    if (s == 0x439) return 0x438;
    if (s >= 0x600 && s <= 0x6ff) {
        if (s == 0x649 || s == 0x6cc) return 0x64a;
        if (s == 0x629) return 0x647;
        if (s == 0x6a9) return 0x643;
        if (s >= 0x660 && s <= 0x669) return '0' + (s - 0x660);
        if (s >= 0x6f0 && s <= 0x6f9) return '0' + (s - 0x6f0);
        if (s == 0x66a) return '%';
        return s;
    }
    if (s >= 0xff01 && s <= 0xff5e) return s - 0xfee0;
    if (s == 0x2018 || s == 0x2019 || s == 0x2bc) return '\''; // Typographic apostrophes: "i’d", "let’s".
    return s;
}
// Punctuation that may wrap a word without being part of it.
bool Opening(std::uint32_t s) noexcept {
    switch (s) {
    case '"': case '\'': case '(': case '[': case 0xa1: case 0xab: case 0xbf: case 0x2018: case 0x201c: case 0x201e: case 0x300c: case 0x300e: return true;
    default: return false;
    }
}
bool Closing(std::uint32_t s) noexcept {
    switch (s) {
    case '.': case ',': case '!': case '?': case ';': case ':': case '"': case '\'': case ')': case ']': case 0xbb: case 0x2019: case 0x201c: case 0x201d:
    case 0x2026: case 0x3001: case 0x3002: case 0x300d: case 0x300f: case 0x60c: case 0x61f: return true;
    default: return false;
    }
}
// Chinese and Japanese are written without spaces: their phrases match at the start or end of a word.
constexpr bool Cjk(std::string_view word) noexcept {
    const auto lead = static_cast<unsigned char>(word.empty() ? 0 : word.front());
    return lead >= 0xe3 && lead <= 0xe9; // U+3000 to U+9FFF.
}

// ---- Tables ----------------------------------------------------------------------------------------------------
// Every table is a list of '|'-separated words per meaning, expanded and hashed at compile time. The words are
// spelled as Fold leaves them (no capitals, no accents, й as и, ة as ه, ى as ي): TablesConsistent checks it.
enum class Argument : std::uint8_t { None, Step, Percent, Span, Time, Start, Quote, Rate, Index };
struct Phrase { std::string_view words; IntentKind kind; Argument argument; int sign; };
struct Group { IntentKind kind; Argument argument; int sign; std::string_view phrases; };
struct Word { std::string_view word; std::int32_t value; };
struct Set { std::int32_t value; std::string_view words; };
using K = IntentKind;
using A = Argument;
// sign: the direction of a Step or Span; 2 is a forward verb that a closing direction word may turn round.
constexpr Group kGroups[]{
    // Hebrew: imperative, future (what "אני רוצה ש..." and "אשמח אם..." take) and infinitive (after "תוכל", "אפשר").
    // A play verb followed by a direction is a relative seek: "תמשיך 10 שניות קדימה", "הרץ אחורה".
    {K::Play, A::None, 0,
     "play|resume|continue|unpause|keep playing"
     "|נגן|תנגן|נגני|תנגני|לנגן|המשך|תמשיך|המשיכי|תמשיכי|להמשיך|הפעל|תפעיל|הפעילי|תפעילי|להפעיל|הרץ|תריץ|להריץ"
     "|شغل|تشغيل|استمر|اكمل|تابع|استئناف"
     "|reproducir|reproduce|reanudar|reanuda|continuar|continua"
     "|lecture|lire|jouer|joue|reprendre|reprends|continuer"
     "|abspielen|wiedergabe|fortsetzen|weiter|starten|spiel ab|spiele ab"
     "|воспроизведение|воспроизвести|воспроизведи|играть|играи|продолжить|продолжи|продолжаи|пуск|старт"
     "|播放|继续|繼續|继续播放|繼續播放|开始播放|開始播放|播放视频|播放視頻"
     "|再生|再生する|再生して|再開|再開して|続ける|続けて"},
    {K::Pause, A::None, 0,
     "pause|stop|hold|freeze"
     "|עצור|תעצור|עצרי|תעצרי|לעצור|השהה|תשהה|להשהות|הפסק|תפסיק|הפסיקי|תפסיקי|להפסיק|פאוז"
     "|توقف|اوقف|ايقاف|وقف|قف|ايقاف مؤقت"
     "|pausar|pausa|detener|deten|parar|para"
     "|arreter|arrete|suspendre|mettre en pause|mets en pause|en pause"
     "|pausieren|anhalten|stopp|stoppen|halt|halt an"
     "|пауза|паузу|стоп|остановить|останови|приостановить|приостанови|на паузу|поставь на паузу|поставить на паузу"
     "|暂停|暫停|停止|暂停播放|暫停播放|停止播放"
     "|一時停止|一時停止する|一時停止して|止める|止めて|ストップ|ポーズ"},
    {K::VolumeUp, A::Step, 1,
     "volume up|louder|turn up|turn it up|turn volume up|turn the volume up|increase volume|increase the volume|raise volume|raise the volume"
     "|turn the sound up|raise it|pump it up|crank it up"
     "|הגבר|תגביר|הגבירי|תגבירי|להגביר|חזק יותר|יותר חזק|תעלה|להעלות|העלה|תעלי"
     "|ارفع الصوت|رفع الصوت|علي الصوت|زد الصوت|ارفع|اعلي"
     "|subir volumen|subir el volumen|sube el volumen|sube volumen|aumentar volumen|aumentar el volumen|aumenta el volumen|mas alto|mas fuerte|subir|sube"
     "|monter le volume|monte le volume|augmenter le volume|augmente le volume|monter le son|monte le son|plus fort"
     "|lauter|lauter machen|mach lauter|lautstarke erhohen|lautstarke hoch"
     "|громче|погромче|сделаи громче|сделать громче|увеличить громкость|увеличь громкость|прибавить громкость|прибавь громкость|прибавь звук"
     "|增大音量|提高音量|调高音量|調高音量|加大音量|调大音量|調大音量|大声点|大聲點|大声一点|大聲一點|音量增大|音量调高|音量調高|音量加"
     "|音量を上げる|音量を上げて|音量上げて|音量上げる|音量アップ|音量を大きく|ボリュームを上げる|ボリュームを上げて|音を大きく"},
    {K::VolumeDown, A::Step, -1,
     "volume down|quieter|softer|turn down|turn it down|turn volume down|turn the volume down|lower volume|lower the volume|decrease volume"
     "|decrease the volume|reduce volume|reduce the volume"
     "|turn the sound down"
     "|הנמך|תנמיך|הנמיכי|תנמיכי|להנמיך|החלש|תחליש|החלישי|להחליש|שקט יותר|יותר שקט|חלש יותר|יותר חלש|תוריד|להוריד|הורד|תורידי"
     "|اخفض الصوت|خفض الصوت|وطي الصوت|نزل الصوت|انقص الصوت|اخفض"
     "|bajar volumen|bajar el volumen|baja el volumen|baja volumen|reducir volumen|reducir el volumen|reduce el volumen|disminuir el volumen"
     "|mas bajo|mas suave|bajar|baja"
     "|baisser le volume|baisse le volume|diminuer le volume|diminue le volume|reduire le volume|reduis le volume|baisser le son|baisse le son|moins fort"
     "|leiser|leiser machen|mach leiser|lautstarke verringern|lautstarke senken|lautstarke runter"
     "|тише|потише|сделаи тише|сделать тише|уменьшить громкость|уменьши громкость|убавить громкость|убавь громкость|убавь звук"
     "|减小音量|減小音量|降低音量|调低音量|調低音量|调小音量|調小音量|小声点|小聲點|小声一点|小聲一點|音量减小|音量減小|音量调低|音量調低|音量减|音量減"
     "|音量を下げる|音量を下げて|音量下げて|音量下げる|音量ダウン|音量を小さく|ボリュームを下げる|ボリュームを下げて|音を小さく"},
    {K::VolumeSet, A::Percent, 0,
     "volume|set volume|set the volume"
     "|עוצמה|ווליום|עוצמת קול|עוצמת הקול|קבע עוצמה"
     "|الصوت|مستوي الصوت|اضبط الصوت|اجعل الصوت"
     "|volumen|poner volumen|poner el volumen|pon el volumen|ajustar volumen|ajustar el volumen"
     "|regler le volume|mettre le volume|mets le volume"
     "|lautstarke|stelle lautstarke|setze lautstarke"
     "|громкость|установить громкость|установи громкость|поставь громкость"
     "|音量|音量调到|音量調到|音量设为|音量設為|音量设置为|音量設置為|把音量调到|把音量調到|设置音量|設置音量"
     "|音量を|ボリューム|ボリュームを"},
    {K::Mute, A::None, 0,
     "mute|silence"
     "|השתק|תשתיק|השתיקי|תשתיקי|להשתיק"
     "|كتم|كتم الصوت|اكتم الصوت|اكتم|صامت"
     "|silenciar|silencia|silencio"
     "|muet|sourdine|couper le son|coupe le son|mettre en sourdine"
     "|stumm|stummschalten|stumm schalten|ton aus"
     "|без звука|выключить звук|выключи звук|отключить звук|отключи звук|заглушить|заглуши"
     "|静音|靜音|关闭声音|關閉聲音"
     "|ミュート|ミュートする|ミュートして|消音|音を消す|音を消して"},
    {K::Unmute, A::None, 0,
     "unmute|sound on"
     "|בטל השתקה|בטלי השתקה|ביטול השתקה|תבטל השתקה|לבטל השתקה|תבטלי השתקה|החזר קול|החזר את הקול|תחזיר את הקול|להחזיר את הקול"
     "|الغاء الكتم|الغ الكتم|الغاء كتم الصوت|شغل الصوت|اعد الصوت"
     "|activar sonido|activar el sonido|activa el sonido|quitar silencio|quitar el silencio|desactivar silencio"
     "|activer le son|active le son|remettre le son|remets le son|retablir le son"
     "|ton an|ton ein|stummschaltung aufheben|laut schalten"
     "|включить звук|включи звук|вернуть звук|верни звук|со звуком"
     "|取消静音|取消靜音|打开声音|打開聲音|恢复声音|恢復聲音"
     "|ミュート解除|ミュートを解除|消音解除|音を出す|音を出して"},
    {K::SeekRelative, A::Span, -1,
     "rewind|back|go back|skip back|jump back|backward|backwards"
     "|חזור|תחזור|חזרי|תחזרי|לחזור|החזר|תחזיר|תחזירי|להחזיר|אחורה|הרץ אחורה|קפוץ אחורה|דלג אחורה|לך אחורה"
     "|ارجع|رجوع|رجع|ترجيع|تراجع|للخلف|للوراء|ارجع للخلف|الي الخلف|ارجع الي الخلف"
     "|retroceder|retrocede|rebobinar|rebobina|atras|ir atras|hacia atras|saltar atras|volver|vuelve"
     "|reculer|recule|rembobiner|rembobine|arriere|en arriere|retour|revenir|reviens|retour en arriere|revenir en arriere"
     "|zuruckspulen|zuruck|ruckwarts|spule zuruck|zuruck spulen|geh zuruck|gehe zuruck"
     "|назад|перемотать назад|перемотаи назад|отмотать|отмотаи|отмотать назад|отмотаи назад|вернуться|вернись|вернуться назад|вернись назад"
     "|快退|后退|後退|倒退|倒回|回退|往回|向后|向後|往后|往後"
     "|巻き戻し|巻き戻す|巻き戻して|巻戻し|戻る|戻って|戻す|戻して"},
    {K::SeekRelative, A::Span, 1,
     "forward|fast forward|skip forward|skip ahead|jump forward|jump ahead|go forward|ahead|advance"
     "|קדימה|דלג קדימה|הרץ קדימה|קפוץ קדימה|התקדם|תתקדם|להתקדם|לך קדימה"
     "|تقدم|قدم|تقديم|للامام|الي الامام|تقدم للامام"
     "|avanzar|avanza|adelantar|adelanta|adelante|hacia adelante|saltar adelante|avance rapido"
     "|avancer|avance|avance rapide|en avant"
     "|vorspulen|vor|vorwarts|spule vor|vor spulen|geh vor|gehe vor"
     "|вперед|перемотать вперед|перемотаи вперед|промотать|промотаи|промотать вперед|промотаи вперед"
     "|快进|快進|前进|前進|向前|往前"
     "|早送り|早送りして|進む|進める|進めて|進んで"},
    {K::SeekRelative, A::Span, 2,
     "skip|דלג|תדלג|לדלג|דלגי|תדלגי|تخطي|saltar|salta|sauter|saute|spule|spulen|springe|spring|uberspringen|перемотать|перемотаи|пропустить|пропусти|跳过|跳過|スキップ"},
    {K::SeekAbsolute, A::Time, 0,
     "go to|goto|seek|seek to|jump to|skip to"
     "|קפוץ|תקפוץ|לקפוץ|עבור|תעבור|לעבור|לך|תלך|ללכת"
     "|اذهب الي|انتقل الي|اقفز الي|اذهب|انتقل|اقفز"
     "|ir a|ir al|ve a|ve al|saltar a|salta a"
     "|aller a|aller au|va a|va au|sauter a|saute a"
     "|gehe zu|geh zu|springe zu|spring zu"
     "|переити к|переиди к|переити на|переиди на|переити|переиди"
     "|跳到|跳转到|跳轉到|转到|轉到|前往|定位到"},
    {K::SeekAbsolute, A::Start, 0,
     "restart|start over|from the beginning|go to the beginning|go to the start"
     "|מההתחלה|להתחלה|חזור להתחלה|קפוץ להתחלה|התחל מחדש"
     "|من البدايه|الي البدايه|للبدايه|ارجع للبدايه|اعد التشغيل|اعاده التشغيل"
     "|reiniciar|reinicia|desde el principio|al principio|ir al principio|volver al principio|empezar de nuevo"
     "|recommencer|recommence|redemarrer|depuis le debut|au debut|aller au debut|revenir au debut"
     "|neustart|neu starten|von vorne|von vorn|zum anfang|zuruck zum anfang|gehe zum anfang"
     "|сначала|заново|с начала|в начало|начать сначала|начать заново|переити в начало|вернуться в начало"
     "|重新开始|重新開始|从头开始|從頭開始|从头播放|從頭播放|回到开头|回到開頭|重播|从头|從頭"
     "|最初から|最初から再生|最初に戻る|最初へ|先頭へ|先頭に戻る|リスタート"},
    // Chapters and scenes (docs/chapters.md). Sections are what the player calls the chapters it synthesizes.
    {K::NextChapter, A::None, 0,
     "next chapter|the next chapter|skip chapter|chapter forward|go to the next chapter|skip to the next chapter|jump to the next chapter|next section"
     "|פרק הבא|הפרק הבא|לפרק הבא|פרק קדימה|עבור לפרק הבא|תעבור לפרק הבא|לעבור לפרק הבא|דלג לפרק הבא|תדלג לפרק הבא|קפוץ לפרק הבא|תקפוץ לפרק הבא"
     "|الفصل التالي|الفصل القادم|انتقل الي الفصل التالي"
     "|siguiente capitulo|capitulo siguiente|proximo capitulo|ir al siguiente capitulo"
     "|chapitre suivant|prochain chapitre|aller au chapitre suivant"
     "|nachstes kapitel|nachster abschnitt|zum nachsten kapitel"
     "|следующая глава|следующую главу|к следующеи главе"
     "|下一章|下一章节|下一章節|下一个章节|下一個章節"
     "|次のチャプター|次の章|次のチャプターへ"},
    {K::PreviousChapter, A::None, 0,
     "previous chapter|the previous chapter|prior chapter|chapter back|back a chapter|go to the previous chapter|go back a chapter|previous section"
     "|פרק קודם|הפרק הקודם|לפרק הקודם|פרק אחורה|עבור לפרק הקודם|תעבור לפרק הקודם|לעבור לפרק הקודם|חזור לפרק הקודם|תחזור לפרק הקודם|לחזור לפרק הקודם"
     "|الفصل السابق|ارجع الي الفصل السابق"
     "|capitulo anterior|ir al capitulo anterior"
     "|chapitre precedent|aller au chapitre precedent"
     "|vorheriges kapitel|voriges kapitel|vorheriger abschnitt|zum vorherigen kapitel"
     "|предыдущая глава|предыдущую главу|к предыдущеи главе"
     "|上一章|上一章节|上一章節|上一个章节|上一個章節"
     "|前のチャプター|前の章|前のチャプターへ"},
    {K::NextScene, A::None, 0,
     "next scene|the next scene|skip scene|scene forward|go to the next scene|skip to the next scene|jump to the next scene"
     "|סצנה הבאה|הסצנה הבאה|לסצנה הבאה|סצנה קדימה|עבור לסצנה הבאה|תעבור לסצנה הבאה|לעבור לסצנה הבאה|דלג לסצנה הבאה|תדלג לסצנה הבאה|קפוץ לסצנה הבאה"
     "|المشهد التالي|المشهد القادم"
     "|siguiente escena|escena siguiente|proxima escena"
     "|scene suivante|prochaine scene"
     "|nachste szene|zur nachsten szene"
     "|следующая сцена|следующую сцену|к следующеи сцене"
     "|下一个场景|下一個場景|下一场景|下一場景|下一幕"
     "|次のシーン|次のシーンへ"},
    {K::PreviousScene, A::None, 0,
     "previous scene|the previous scene|scene back|back a scene|go to the previous scene|go back a scene"
     "|סצנה קודמת|הסצנה הקודמת|לסצנה הקודמת|סצנה אחורה|עבור לסצנה הקודמת|תעבור לסצנה הקודמת|חזור לסצנה הקודמת|תחזור לסצנה הקודמת|לחזור לסצנה הקודמת"
     "|المشهد السابق"
     "|escena anterior"
     "|scene precedente"
     "|vorherige szene|zur vorherigen szene"
     "|предыдущая сцена|предыдущую сцену|к предыдущеи сцене"
     "|上一个场景|上一個場景|上一场景|上一場景|上一幕"
     "|前のシーン|前のシーンへ"},
    // The next or previous file in the folder (docs/intents.md, "Files"): the session plays it, the grammar only checks
    // that there is one that way.
    {K::NextFile, A::None, 0,
     "next video|the next video|next file|the next file|next episode|the next episode|next movie|the next movie|next clip"
     "|play the next video|go to the next video|skip to the next video|play the next episode|go to the next episode|on to the next video"
     "|הסרטון הבא|סרטון הבא|לסרטון הבא|עבור לסרטון הבא|תעבור לסרטון הבא|לעבור לסרטון הבא|נגן את הסרטון הבא|תנגן את הסרטון הבא"
     "|הסרט הבא|סרט הבא|לסרט הבא|הקובץ הבא|קובץ הבא|לקובץ הבא|הוידאו הבא|הווידאו הבא|הפרק הבא בסדרה"
     "|الفيديو التالي|الفيديو القادم|الملف التالي|انتقل الي الفيديو التالي|شغل الفيديو التالي"
     "|siguiente video|el siguiente video|video siguiente|proximo video|el proximo video|siguiente archivo|siguiente episodio"
     "|video suivante|la video suivante|prochaine video|la prochaine video|fichier suivant|episode suivant"
     "|nachstes video|das nachste video|nachste datei|nachste folge|zum nachsten video"
     "|следующее видео|следующии фаил|следующая серия|следующую серию|к следующему видео"
     "|下一个视频|下一個視頻|下个视频|下一集|下一个文件|下一個文件"
     "|次の動画|次の動画へ|次のビデオ|次のファイル|次のエピソード|次の話"},
    {K::PreviousFile, A::None, 0,
     "previous video|the previous video|previous file|the previous file|previous episode|the previous episode|previous movie|the previous movie"
     "|previous clip|play the previous video|go to the previous video|go back to the previous video|back to the previous video|go back a video"
     "|הסרטון הקודם|סרטון קודם|לסרטון הקודם|עבור לסרטון הקודם|תעבור לסרטון הקודם|חזור לסרטון הקודם|תחזור לסרטון הקודם|לחזור לסרטון הקודם"
     "|הסרט הקודם|סרט קודם|לסרט הקודם|הקובץ הקודם|קובץ קודם|לקובץ הקודם|הוידאו הקודם|הווידאו הקודם|הפרק הקודם בסדרה"
     "|الفيديو السابق|الملف السابق|ارجع الي الفيديو السابق|شغل الفيديو السابق"
     "|video anterior|el video anterior|archivo anterior|episodio anterior"
     "|video precedente|la video precedente|fichier precedent|episode precedent"
     "|vorheriges video|das vorherige video|voriges video|vorherige datei|vorherige folge|zum vorherigen video"
     "|предыдущее видео|предыдущии фаил|предыдущая серия|предыдущую серию|к предыдущему видео"
     "|上一个视频|上一個視頻|上个视频|上一集|上一个文件|上一個文件"
     "|前の動画|前の動画へ|前のビデオ|前のファイル|前のエピソード|前の話"},
    // Playback speed (docs/intents.md, "Speed"). A fixed phrase's speed is its sign, in hundredths.
    {K::SpeedUp, A::None, 0,
     "faster|speed up|play faster|go faster|speed it up|increase speed|increase the speed|increase playback speed"
     "|מהר יותר|יותר מהר|תאיץ|האץ|להאיץ|תאיצי|הגבר מהירות|תגביר מהירות|תגביר את המהירות|להגביר את המהירות"
     "|اسرع|اسرع قليلا|زد السرعه"
     "|mas rapido|acelerar|acelera|aumentar velocidad|aumenta la velocidad"
     "|plus vite|accelerer|accelere|augmenter la vitesse"
     "|schneller|beschleunigen|schneller abspielen"
     "|быстрее|ускорить|ускорь|ускорь воспроизведение"
     "|快一点|快一點|快点|快點|加速|再快点|再快點"
     "|速く|速くして|もっと速く|スピードアップ"},
    {K::SpeedDown, A::None, 0,
     "slower|slow down|play slower|go slower|slow it down|decrease speed|decrease the speed|reduce speed"
     "|לאט יותר|יותר לאט|תאט|האט|להאט|תאטי|הורד מהירות|תוריד מהירות|תוריד את המהירות|להוריד את המהירות"
     "|ابطا|ابطئ|ابطا قليلا|خفض السرعه"
     "|mas lento|mas despacio|ralentizar|ralentiza|reducir velocidad"
     "|plus lent|plus lentement|ralentir|ralentis|reduire la vitesse"
     "|langsamer|verlangsamen|langsamer abspielen"
     "|медленнее|замедлить|замедли|помедленнее"
     "|慢一点|慢一點|慢点|慢點|减速|減速|再慢点|再慢點"
     "|遅く|遅くして|もっと遅く|ゆっくり|スピードダウン"},
    {K::SpeedSet, A::Rate, 0,
     "speed|playback speed|set speed|set the speed|set speed to|set the speed to|speed to|play at|change speed to|change the speed to"
     "|מהירות|מהירות ניגון|קבע מהירות|תקבע מהירות|תקבע את המהירות|שנה מהירות|תשנה מהירות|נגן במהירות|תנגן במהירות"
     "|السرعه|سرعه التشغيل|اضبط السرعه"
     "|velocidad|velocidad de reproduccion|poner velocidad"
     "|vitesse|vitesse de lecture|regler la vitesse"
     "|geschwindigkeit|wiedergabegeschwindigkeit|tempo"
     "|скорость|скорость воспроизведения|установи скорость"
     "|速度|播放速度|设置速度|設置速度"
     "|再生速度|スピード"},
    {K::SpeedSet, A::None, 100,
     "normal speed|regular speed|normal playback speed|reset speed|reset the speed"
     "|מהירות רגילה|מהירות נורמלית|מהירות רגילה בבקשה|חזור למהירות רגילה|תחזור למהירות רגילה"
     "|السرعه العاديه|السرعه الطبيعيه"
     "|velocidad normal"
     "|vitesse normale"
     "|normale geschwindigkeit|normales tempo"
     "|обычная скорость|нормальная скорость"
     "|正常速度|原速|正常播放"
     "|通常速度|標準速度|等速"},
    {K::SpeedSet, A::None, 200,
     "double speed|twice as fast|twice the speed|play at double speed"
     "|מהירות כפולה|כפול מהירות|פי שניים מהירות|במהירות כפולה|תנגן במהירות כפולה"
     "|ضعف السرعه|سرعه مضاعفه"
     "|doble velocidad|velocidad doble"
     "|double vitesse|vitesse double"
     "|doppelte geschwindigkeit|doppeltes tempo"
     "|двоиная скорость|двоиную скорость"
     "|两倍速|兩倍速|二倍速|双倍速度|雙倍速度"
     "|倍速|2倍速"},
    {K::SpeedSet, A::None, 50,
     "half speed|at half speed|play at half speed"
     "|חצי מהירות|במחצית המהירות|תנגן בחצי מהירות"
     "|نصف السرعه"
     "|media velocidad|mitad de velocidad"
     "|demi vitesse|moitie de la vitesse"
     "|halbe geschwindigkeit|halbes tempo"
     "|половинная скорость|половинную скорость"
     "|半速|一半速度"
     "|半分の速度"},
    // Zoom into the picture (docs/picture.md, "Zoom").
    {K::ZoomIn, A::None, 0,
     "zoom in|magnify|enlarge|zoom closer|enlarge the picture|zoom into the picture"
     "|זום פנימה|זום|הגדל|תגדיל|הגדל תמונה|הגדל את התמונה|תגדיל את התמונה|זום אין|תעשה זום"
     "|تكبير|كبر الصوره|قرب الصوره"
     "|acercar|ampliar|hacer zoom|ampliar la imagen"
     "|zoomer|agrandir|zoom avant|agrandir l image"
     "|vergrossern|vergroßern|reinzoomen|hineinzoomen|bild vergrossern|bild vergroßern"
     "|увеличить|приблизить|увеличь|приблизь|увеличь изображение"
     "|放大|放大画面|放大畫面"
     "|拡大|ズームイン|拡大して"},
    {K::ZoomOut, A::None, 0,
     "zoom out|zoom back|shrink the picture"
     "|זום החוצה|הקטן|תקטין|הקטן תמונה|הקטן את התמונה|תקטין את התמונה|זום אאוט"
     "|تصغير|صغر الصوره"
     "|alejar|zoom afuera|alejar la imagen"
     "|dezoomer|zoom arriere"
     "|verkleinern|rauszoomen|herauszoomen|bild verkleinern"
     "|уменьшить|отдалить|уменьши|отдали|уменьши изображение"
     "|缩小|縮小|缩小画面|縮小畫面"
     "|ズームアウト|縮小して"},
    {K::ZoomReset, A::None, 0,
     "reset zoom|reset the zoom|zoom off|no zoom|unzoom|whole picture|show the whole picture"
     "|אפס זום|תאפס זום|תאפס את הזום|בטל זום|תבטל זום|ללא זום|איפוס זום|כל התמונה"
     "|الغاء التكبير|الصوره كامله"
     "|restablecer zoom|quitar zoom|quitar el zoom|imagen completa"
     "|reinitialiser le zoom|annuler le zoom|image entiere"
     "|zoom zurucksetzen|zoom aus|ganzes bild"
     "|сбросить масштаб|сбросить увеличение|сброс масштаба|все изображение"
     "|重置缩放|重置縮放|取消放大|全画面|全畫面"
     "|ズームリセット|拡大解除|全体表示"},
    // The A–B loop (docs/intents.md, "Loop"): its start and end at the position, off, or the sentence on screen.
    {K::LoopStart, A::None, 0,
     "set loop start|loop start|start the loop here|start loop here|mark loop start|mark the loop start|set point a|loop from here"
     "|תחילת לופ|התחלת לופ|קבע תחילת לופ|תקבע תחילת לופ|סמן תחילת לופ|תסמן תחילת לופ|תחילת הלופ|לופ מכאן|נקודה א"
     "|بدايه التكرار|بدايه الحلقه"
     "|inicio del bucle|marcar inicio del bucle|punto a"
     "|debut de boucle|debut de la boucle|marquer le debut de la boucle"
     "|schleifenanfang|anfang der schleife|punkt a"
     "|начало петли|начало цикла|точка а"
     "|循环起点|设置循环起点|设为循环起点"
     "|ループ開始|ループの開始|ループ開始点"},
    {K::LoopEnd, A::None, 0,
     "set loop end|loop end|end the loop here|end loop here|mark loop end|mark the loop end|set point b|loop until here|loop to here"
     "|סוף לופ|קבע סוף לופ|תקבע סוף לופ|סמן סוף לופ|תסמן סוף לופ|סוף הלופ|לופ עד כאן|נקודה ב"
     "|نهايه التكرار|نهايه الحلقه"
     "|fin del bucle|marcar fin del bucle|punto b"
     "|fin de boucle|fin de la boucle|marquer la fin de la boucle"
     "|schleifenende|ende der schleife|punkt b"
     "|конец петли|конец цикла|точка б"
     "|循环终点|设置循环终点|设为循环终点"
     "|ループ終了|ループの終わり|ループ終了点"},
    {K::LoopClear, A::None, 0,
     "clear loop|clear the loop|stop loop|stop the loop|stop looping|loop off|cancel loop|cancel the loop|remove loop|remove the loop"
     "|no loop|exit loop|turn off loop|turn off the loop"
     "|בטל לופ|תבטל לופ|בטל את הלופ|תבטל את הלופ|הפסק לופ|תפסיק לופ|תפסיק את הלופ|בלי לופ|כבה לופ|תכבה לופ|תכבה את הלופ|צא מהלופ|תצא מהלופ"
     "|الغاء التكرار|اوقف التكرار"
     "|quitar bucle|quitar el bucle|desactivar bucle|sin bucle"
     "|arreter la boucle|annuler la boucle|sans boucle|desactiver la boucle"
     "|schleife aus|schleife beenden|ohne schleife"
     "|выключи петлю|убери петлю|без петли|отключи цикл"
     "|取消循环|关闭循环|停止循环"
     "|ループ解除|ループを解除|ループ停止|ループをやめる"},
    {K::LoopCue, A::None, 0,
     "loop this sentence|loop the sentence|loop this line|loop the line|loop that line|loop sentence|repeat this line|repeat this sentence"
     "|repeat this line in a loop|repeat this sentence in a loop|keep repeating this line|keep repeating this sentence"
     "|לופ על המשפט|לופ על המשפט הזה|חזור על המשפט בלופ|תחזור על המשפט בלופ|לחזור על המשפט בלופ|המשפט בלופ|המשפט הזה בלופ|תעשה לופ על המשפט|לופ למשפט"
     "|كرر هذه الجمله باستمرار|تكرار الجمله"
     "|repetir esta frase en bucle|bucle en esta frase|bucle de la frase"
     "|boucle sur cette phrase|repete cette phrase en boucle|boucler la phrase"
     "|satz in schleife|diesen satz in schleife"
     "|зацикли фразу|зациклить фразу|повторяи эту фразу"
     "|循环这句|循环这句话|单句循环"
     "|このセリフをループ|セリフをループ|この文をループ"},
    // The telemetry readout (docs/intents.md, "Telemetry"): the panel, and why the picture quality is what it is.
    {K::ShowStats, A::None, 0,
     "show stats|show statistics|stats|statistics|status report|show telemetry|telemetry|diagnostics|show diagnostics|stats for nerds"
     "|show the stats|show the statistics"
     "|הצג נתונים|תציג נתונים|הראה נתונים|תראה נתונים|נתונים|סטטיסטיקה|הצג סטטיסטיקה|תציג סטטיסטיקה|דוח מצב|תן דוח מצב"
     "|اعرض الاحصائيات|الاحصائيات|تقرير الحاله"
     "|mostrar estadisticas|estadisticas|informe de estado"
     "|afficher les statistiques|statistiques|etat du lecteur"
     "|statistiken anzeigen|statistiken|statusbericht"
     "|показать статистику|покажи статистику|статистика|отчет о состоянии"
     "|显示统计|统计信息|状态报告"
     "|統計を表示|統計情報|ステータス"},
    {K::HideStats, A::None, 0,
     "hide stats|hide the stats|hide statistics|stats off|close stats|hide telemetry|hide diagnostics"
     "|הסתר נתונים|תסתיר נתונים|סגור נתונים|תסגור נתונים|בלי נתונים|תסתיר את הנתונים"
     "|اخف الاحصائيات"
     "|ocultar estadisticas"
     "|masquer les statistiques"
     "|statistiken ausblenden"
     "|скрыть статистику|скрои статистику"
     "|隐藏统计|关闭统计"
     "|統計を隠す|統計を閉じる"},
    {K::ExplainQuality, A::None, 0,
     "why did quality change|why did the quality change|why did quality drop|why did the quality drop|why did quality go down"
     "|why did the quality go down|why did quality improve|why did the quality improve|why is the quality low|why is quality low"
     "|why is the picture blurry|explain quality|explain the quality|quality report"
     "|למה האיכות ירדה|למה האיכות השתנתה|למה האיכות עלתה|למה התמונה מטושטשת|למה האיכות נמוכה|הסבר איכות|תסביר את האיכות|מה קרה לאיכות"
     "|لماذا انخفضت الجوده|لماذا تغيرت الجوده"
     "|por que bajo la calidad|por que cambio la calidad"
     "|pourquoi la qualite a baisse|pourquoi la qualite a change"
     "|warum ist die qualitat gesunken|warum hat sich die qualitat geandert"
     "|почему упало качество|почему изменилось качество"
     "|为什么画质下降|为什么画质变了|画质为什么变了"
     "|なぜ画質が下がった|画質が下がった理由|なぜ画質が変わった"},
    // Night / smart loudness mode (docs/intents.md, "Night mode").
    {K::NightOn, A::None, 0,
     "night mode|night mode on|turn on night mode|enable night mode|normalize volume|normalise volume|normalize the volume|normalise the volume"
     "|loudness normalization|smart volume|even out the volume|level the volume"
     "|מצב לילה|הפעל מצב לילה|תפעיל מצב לילה|אזן עוצמת קול|תאזן עוצמת קול|איזון עוצמה|אזן את הווליום|תאזן את הווליום|נרמל עוצמה"
     "|الوضع الليلي|توحيد مستوي الصوت"
     "|modo nocturno|normalizar volumen|normalizar el volumen"
     "|mode nuit|normaliser le volume"
     "|nachtmodus|lautstarke normalisieren|lautstarke angleichen"
     "|ночнои режим|выровнять громкость|выровняи громкость"
     "|夜间模式|音量均衡"
     "|ナイトモード|音量を均一に"},
    {K::NightOff, A::None, 0,
     "night mode off|turn off night mode|disable night mode|stop night mode|no night mode|exit night mode"
     "|בטל מצב לילה|תבטל מצב לילה|כבה מצב לילה|תכבה מצב לילה|בלי מצב לילה|צא ממצב לילה"
     "|الغاء الوضع الليلي"
     "|desactivar modo nocturno|quitar modo nocturno"
     "|desactiver le mode nuit|mode nuit off"
     "|nachtmodus aus|nachtmodus beenden"
     "|выключи ночнои режим|выключить ночнои режим"
     "|关闭夜间模式"
     "|ナイトモード解除|ナイトモードオフ"},
    // Bookmarks and the pinned frame (docs/intents.md, "Bookmarks").
    {K::BookmarkAdd, A::None, 0,
     "add bookmark|add a bookmark|bookmark this|bookmark here|bookmark|set a bookmark|drop a bookmark|save this point|save this spot|mark this spot"
     "|סמן הערה|שמור נקודה|הוסף סימנייה|תוסיף סימנייה|סימנייה|שמור סימנייה|תשמור סימנייה|סמן נקודה|תסמן נקודה|תשמור נקודה"
     "|اضف علامه|علامه مرجعيه"
     "|agregar marcador|anadir marcador|marcador"
     "|ajouter un signet|signet"
     "|lesezeichen setzen|lesezeichen hinzufugen|lesezeichen"
     "|добавить закладку|добавь закладку|закладка"
     "|添加书签|加书签"
     "|ブックマーク追加|ブックマークする"},
    {K::BookmarkGo, A::Index, 0,
     "go to bookmark|jump to bookmark|bookmark number|back to bookmark|return to bookmark"
     "|לך לסימנייה|תלך לסימנייה|עבור לסימנייה|תעבור לסימנייה|קפוץ לסימנייה|תקפוץ לסימנייה|סימנייה מספר|חזור לסימנייה|תחזור לסימנייה"
     "|اذهب الي العلامه"
     "|ir al marcador"
     "|aller au signet"
     "|zum lesezeichen|gehe zu lesezeichen"
     "|к закладке|переити к закладке"
     "|跳到书签|转到书签"
     "|ブックマークへ|ブックマークに移動"},
    {K::BookmarkNext, A::None, 0,
     "next bookmark|the next bookmark|go to the next bookmark"
     "|הסימנייה הבאה|סימנייה הבאה|לסימנייה הבאה|עבור לסימנייה הבאה|תעבור לסימנייה הבאה"
     "|العلامه التاليه"
     "|siguiente marcador"
     "|signet suivant"
     "|nachstes lesezeichen"
     "|следующая закладка|к следующеи закладке"
     "|下一个书签"
     "|次のブックマーク"},
    {K::BookmarkPrevious, A::None, 0,
     "previous bookmark|the previous bookmark|go to the previous bookmark|last bookmark"
     "|הסימנייה הקודמת|סימנייה קודמת|לסימנייה הקודמת|חזור לסימנייה הקודמת|תחזור לסימנייה הקודמת"
     "|العلامه السابقه"
     "|marcador anterior"
     "|signet precedent"
     "|vorheriges lesezeichen"
     "|предыдущая закладка|к предыдущеи закладке"
     "|上一个书签"
     "|前のブックマーク"},
    {K::PinFrame, A::None, 0,
     "pin frame|pin this frame|pin the frame|freeze frame for comparison|keep this frame|keep this frame for comparison|reference frame"
     "|הקפא פריים להשוואה|הקפא פריים|תקפיא פריים|תקפיא את הפריים|נעץ פריים|שמור פריים להשוואה|תשמור פריים להשוואה"
     "|ثبت الاطار"
     "|fijar fotograma|fijar el fotograma"
     "|epingler l image|figer l image pour comparer"
     "|bild anheften|standbild zum vergleich"
     "|закрепить кадр|закрепи кадр"
     "|固定画面|锁定画面"
     "|フレーム固定|この画面を固定"},
    {K::ComparePinned, A::None, 0,
     "compare with the pinned frame|compare pinned|pinned comparison|compare to the pinned frame|compare with reference|compare with the reference frame"
     "|השווה לפריים הקפוא|תשווה לפריים הקפוא|השוואה לפריים הקפוא|השווה לפריים השמור|תשווה לפריים השמור"
     "|قارن بالاطار المثبت"
     "|comparar con el fotograma fijado"
     "|comparer avec l image epinglee"
     "|mit dem angehefteten bild vergleichen"
     "|сравнить с закрепленным кадром"
     "|与固定画面对比"
     "|固定画面と比較"},
    // Audio zoom, micro-motion and the cures (docs/intents.md, "Audio zoom, micro-motion and cures").
    {K::AudioZoomOn, A::None, 0,
     "audio zoom|acoustic zoom|audio zoom on|turn on audio zoom|enable audio zoom|zoom the audio|zoom the sound|sound zoom|audio focus"
     "|focus the sound|focus the audio"
     "|זום אקוסטי|הפעל זום אקוסטי|תפעיל זום אקוסטי|זום שמע|זום קול|מיקוד קול|מקד את הקול|תמקד את הקול"
     "|تكبير الصوت|تركيز الصوت"
     "|zoom de audio|zoom acustico|enfocar el sonido"
     "|zoom audio|zoom acoustique|focaliser le son"
     "|akustischer zoom|tonzoom|ton fokussieren"
     "|звуковои зум|акустическии зум"
     "|音频变焦|声音变焦|声音聚焦"
     "|オーディオズーム|音声ズーム|音のズーム"},
    {K::AudioZoomOff, A::None, 0,
     "audio zoom off|acoustic zoom off|turn off audio zoom|disable audio zoom|stop audio zoom|no audio zoom|turn off acoustic zoom"
     "|בטל זום אקוסטי|תבטל זום אקוסטי|כבה זום אקוסטי|תכבה זום אקוסטי|בלי זום אקוסטי"
     "|الغاء تكبير الصوت"
     "|quitar zoom de audio|desactivar zoom acustico"
     "|desactiver le zoom audio|zoom audio off"
     "|akustischer zoom aus|tonzoom aus"
     "|выключи звуковои зум"
     "|关闭音频变焦"
     "|オーディオズーム解除"},
    {K::MicroMotionOn, A::None, 0,
     "micro motion|micromotion|micro motion on|tactical vision|tactical vision on|motion magnification|magnify motion|magnify small motion"
     "|amplify motion|amplify small motion|show small movements|show micro movements|pulse view|turn on micro motion|turn on tactical vision"
     "|הדגש תנועה זעירה|תדגיש תנועה זעירה|תנועה זעירה|הדגש תנועות קטנות|תדגיש תנועות קטנות|הגדל תנועה|ראייה טקטית|ראיה טקטית|הפעל ראייה טקטית"
     "|تضخيم الحركه|رؤيه تكتيكيه"
     "|micro movimiento|vision tactica|amplificar movimiento"
     "|micro mouvement|vision tactique|amplifier le mouvement"
     "|mikrobewegung|taktische sicht|bewegung verstarken"
     "|микродвижение|тактическое зрение|усилить движение"
     "|微动放大|战术视觉|放大微动"
     "|微小動作強調|タクティカルビジョン|動きを強調"},
    {K::MicroMotionOff, A::None, 0,
     "micro motion off|tactical vision off|turn off micro motion|turn off tactical vision|no micro motion|stop magnifying motion|stop micro motion"
     "|בטל הדגשת תנועה|תבטל הדגשת תנועה|כבה תנועה זעירה|תכבה תנועה זעירה|בלי תנועה זעירה|בטל ראייה טקטית|תבטל ראייה טקטית|כבה ראייה טקטית"
     "|الغاء تضخيم الحركه"
     "|quitar micro movimiento|desactivar vision tactica"
     "|desactiver la vision tactique|micro mouvement off"
     "|mikrobewegung aus|taktische sicht aus"
     "|выключи микродвижение|выключи тактическое зрение"
     "|关闭微动放大|关闭战术视觉"
     "|微小動作強調解除|タクティカルビジョン解除"},
    // Target tracking (docs/intents.md, "Target tracking"): lock onto what is in the middle of the view, and let go.
    {K::TrackTarget, A::None, 0,
     "track target|track the target|track object|track the object|track this|follow object|follow the object|follow target|follow the target"
     "|follow this|lock on|lock on target|lock onto the target|lock on the target|start tracking|tracking on|turn on tracking|target tracking"
     "|\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x90\xd7\x95\xd7\x91\xd7\x99\xd7\x99\xd7\xa7\xd7\x98"
     "|\xd7\xaa\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x90\xd7\x95\xd7\x91\xd7\x99\xd7\x99\xd7\xa7\xd7\x98"
     "|\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94"
     "|\xd7\xaa\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94"
     "|\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x96\xd7\x94|\xd7\xaa\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x96\xd7\x94"
     "|\xd7\xa0\xd7\xa2\xd7\x9c \xd7\x9e\xd7\x98\xd7\xa8\xd7\x94|\xd7\x94\xd7\xa4\xd7\xa2\xd7\x9c \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91|\xd7\xaa\xd7\xa4\xd7\xa2\xd7\x99\xd7\x9c \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91"
     "|تتبع الهدف|اتبع الهدف|تتبع الجسم"
     "|seguir el objetivo|seguir el objeto|rastrear el objetivo"
     "|suivre la cible|suivre l objet|suivi de cible"
     "|ziel verfolgen|objekt verfolgen|zielverfolgung"
     "|следи за целью|следить за объектом|отслеживать объект"
     "|跟踪目标|追踪目标|跟踪物体"
     "|ターゲットを追跡|対象を追跡|追尾開始"},
    {K::StopTracking, A::None, 0,
     "stop tracking|tracking off|turn off tracking|stop following|stop following the object|stop following the target|release the target"
     "|cancel tracking|no tracking|unlock the target"
     "|\xd7\x94\xd7\xa4\xd7\xa1\xd7\xa7 \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91|\xd7\xaa\xd7\xa4\xd7\xa1\xd7\x99\xd7\xa7 \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91"
     "|\xd7\x94\xd7\xa4\xd7\xa1\xd7\xa7 \xd7\x9c\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91|\xd7\xaa\xd7\xa4\xd7\xa1\xd7\x99\xd7\xa7 \xd7\x9c\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91"
     "|\xd7\x91\xd7\x98\xd7\x9c \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91|\xd7\xaa\xd7\x91\xd7\x98\xd7\x9c \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91|\xd7\x9b\xd7\x91\xd7\x94 \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91"
     "|اوقف التتبع|الغاء التتبع"
     "|dejar de seguir|detener el seguimiento"
     "|arreter le suivi|arrete de suivre"
     "|verfolgung beenden|verfolgung aus"
     "|прекрати слежение|останови отслеживание"
     "|停止跟踪|取消跟踪"
     "|追跡停止|追尾解除"},
    // Several objects (docs/intents.md, "Next target", 2026-10-31): tracking moves onto the next object perception keeps track of.
    {K::NextTarget, A::None, 0,
     "next target|the next target|switch target|switch targets|change target|change the target|other target|the other target|next object"
     "|the next object|track the next target|track the next object|follow the next target|follow the next object|cycle targets"
     "|lock on the next target|lock onto the next target|target the next one"
     "|מטרה הבאה|המטרה הבאה|למטרה הבאה|עבור למטרה הבאה|תעבור למטרה הבאה|החלף מטרה|תחליף מטרה|מטרה אחרת|אובייקט הבא|האובייקט הבא"
     "|עקוב אחרי המטרה הבאה|תעקוב אחרי המטרה הבאה"
     "|الهدف التالي|غير الهدف|الهدف الاخر"
     "|siguiente objetivo|objetivo siguiente|el siguiente objetivo|cambiar de objetivo|cambia de objetivo|otro objetivo"
     "|cible suivante|la cible suivante|prochaine cible|changer de cible|change de cible|objet suivant|autre cible"
     "|nachstes ziel|das nachste ziel|ziel wechseln|wechsle das ziel|anderes ziel"
     "|следующая цель|сменить цель|смени цель|другая цель"
     "|下一个目标|切换目标|换个目标"
     "|次のターゲット|ターゲット切り替え|次の目標"},
    // Forensic snapshots (docs/intents.md, "Snapshots", 2026-11-01): the picture as made, to a BMP and its telemetry sidecar.
    {K::Snapshot, A::None, 0,
     "take snapshot|take a snapshot|snapshot|save snapshot|save a snapshot|take a screenshot|take screenshot|screenshot|save frame|save the frame"
     "|save this frame|capture frame|capture the frame|capture this frame|grab frame|grab a frame|grab the frame|save picture|save the picture"
     "|save image|save the image|capture image|capture the image|export frame|export this frame|export the frame"
     "|צילום מסך|צלם מסך|תצלם מסך|צלם את המסך|תצלם את המסך|צלם תמונה|תצלם תמונה|שמור תמונה|תשמור תמונה|שמור את התמונה|תשמור את התמונה"
     "|תמונת מסך|צלם|תצלם"
     "|لقطه شاشه|التقط صوره|احفظ الصوره|التقط الشاشه"
     "|captura de pantalla|tomar captura|tomar una captura|capturar imagen|capturar la imagen|guardar imagen|guardar la imagen|guardar fotograma"
     "|tomar foto|tomar una foto|instantanea"
     "|capturer l image|capturer l'image|capture d ecran|capture d'ecran|prendre une capture|faire une capture|enregistrer l image|enregistrer l'image"
     "|sauvegarder l image|sauvegarder l'image|prendre une photo|instantane"
     "|bildschirmfoto|schnappschuss|bild speichern|standbild speichern|bild aufnehmen|foto machen"
     "|снимок экрана|сделаи снимок|сделать снимок|сохрани кадр|сохранить кадр|скриншот|сделаи скриншот|снимок"
     "|截图|截屏|屏幕截图|保存画面|保存图片|拍照|快照"
     "|スクリーンショット|スナップショット|画面を保存|画像を保存|キャプチャ|写真を撮って"},
    // The audio saccade (docs/intents.md, "Look at the sound"): the view turns toward where a sudden sound came from.
    {K::AudioSaccadeOn, A::None, 0,
     "look at sound|look at the sound|look at the noise|look toward the sound|look towards the sound|look at the sounds|turn to the sound"
     "|turn toward the sound|face the sound|follow the sound|follow sound|follow the sounds|follow the noise|audio saccade|audio saccade on"
     "|sound saccade|look where the sound is|look where the sound comes from|ears steer the eyes"
     "|עקוב אחרי הקול|תעקוב אחרי הקול|עקוב אחרי הצליל|תעקוב אחרי הצליל|עקוב אחרי הרעש|תעקוב אחרי הרעש"
     "|הסתכל על הקול|תסתכל על הקול|הסתכל לכיוון הקול|תסתכל לכיוון הקול|פנה לכיוון הקול|תפנה לכיוון הקול|הבט לכיוון הקול"
     "|انظر الي الصوت|اتبع الصوت|انظر نحو الصوت"
     "|mira el sonido|mira hacia el sonido|sigue el sonido"
     "|regarde le son|regarde vers le son|suis le son"
     "|schau zum gerausch|schau zum ton|folge dem gerausch|folge dem ton"
     "|смотри на звук|следи за звуком|повернись к звуку"
     "|看向声音|跟随声音|看声音"
     "|音の方を見て|音を追って|音の方向を見て"},
    {K::AudioSaccadeOff, A::None, 0,
     "stop looking at sound|stop looking at the sound|stop following the sound|stop following sounds|audio saccade off|sound saccade off"
     "|ignore sounds|ignore the sound|dont look at sounds|dont follow the sound"
     "|הפסק לעקוב אחרי הקול|תפסיק לעקוב אחרי הקול|הפסק להסתכל על הקול|תפסיק להסתכל על הקול|בטל מעקב קול|תבטל מעקב קול|כבה מעקב קול"
     "|توقف عن اتباع الصوت|لا تتبع الصوت"
     "|deja de seguir el sonido|no sigas el sonido"
     "|arrete de suivre le son|ne suis plus le son"
     "|folge dem gerausch nicht|gerausch nicht folgen"
     "|не следи за звуком|перестань следить за звуком"
     "|停止跟随声音|不要跟随声音"
     "|音を追うのをやめて|音を追わないで"},
    // Rotation (docs/intents.md, "Rotation"): quarter turns of the picture, and its leveling a degree at a time.
    {K::RotateVideo, A::None, 0,
     "rotate video|rotate the video|rotate picture|rotate the picture|rotate image|rotate the image|rotate clockwise|rotate right"
     "|rotate video right|rotate the video right|rotate the picture right|turn the picture|turn the video|rotate ninety degrees"
     "|סובב תמונה|תסובב תמונה|סובב את התמונה|תסובב את התמונה|סובב וידאו|תסובב וידאו|סובב את הווידאו|תסובב את הווידאו"
     "|סובב ימינה|תסובב ימינה|סובב תמונה ימינה|תסובב תמונה ימינה"
     "|دور الفيديو|دور الصوره|تدوير الفيديو|تدوير الصوره"
     "|girar el video|gira el video|girar la imagen|gira la imagen|rotar el video|rota el video"
     "|tourner la video|tourne la video|tourner l image|tourne l image|tourner l'image|tourne l'image|pivoter la video|pivote la video|faire pivoter la video"
     "|video drehen|dreh das video|bild drehen|dreh das bild|video rotieren"
     "|поверни видео|повернуть видео|поверни изображение|повернуть изображение"
     "|旋转视频|旋转画面|旋转图像"
     "|動画を回転|画面を回転|映像を回転"},
    {K::RotateVideoBack, A::None, 0,
     "rotate left|rotate video left|rotate the video left|rotate the picture left|rotate counterclockwise|rotate anticlockwise"
     "|rotate back|rotate the video back|rotate the picture back"
     "|סובב שמאלה|תסובב שמאלה|סובב תמונה שמאלה|תסובב תמונה שמאלה|סובב בחזרה|תסובב בחזרה"
     "|دور الفيديو لليسار|دور الصوره لليسار"
     "|girar a la izquierda|gira a la izquierda|girar el video a la izquierda"
     "|tourner a gauche|tourne a gauche|tourner la video a gauche|tourne la video a gauche"
     "|nach links drehen|video nach links drehen|bild nach links drehen"
     "|поверни влево|поверни видео влево"
     "|向左旋转|逆时针旋转"
     "|左に回転|反時計回りに回転"},
    {K::LevelLeft, A::None, 0,
     "level horizon left|level the horizon left|tilt left|tilt the picture left|tilt the video left"
     "|יישר אופק שמאלה|תיישר אופק שמאלה|הטה שמאלה|תטה שמאלה"
     "|امل لليسار"
     "|inclinar a la izquierda|nivelar el horizonte a la izquierda"
     "|incliner a gauche|redresser l horizon a gauche|redresser l'horizon a gauche"
     "|nach links neigen|horizont nach links"
     "|наклони влево|выровняи горизонт влево"
     "|向左倾斜|地平线向左"
     "|左に傾ける|水平を左に"},
    {K::LevelRight, A::None, 0,
     "level horizon right|level the horizon right|tilt right|tilt the picture right|tilt the video right"
     "|יישר אופק ימינה|תיישר אופק ימינה|הטה ימינה|תטה ימינה"
     "|امل لليمين"
     "|inclinar a la derecha|nivelar el horizonte a la derecha"
     "|incliner a droite|redresser l horizon a droite|redresser l'horizon a droite"
     "|nach rechts neigen|horizont nach rechts"
     "|наклони вправо|выровняи горизонт вправо"
     "|向右倾斜|地平线向右"
     "|右に傾ける|水平を右に"},
    {K::LevelHorizon, A::None, 0,
     "straighten|straighten the picture|straighten the video|straighten the horizon|horizon level"
     "|יישר אופק|תיישר אופק|יישר את האופק|תיישר את האופק|יישר תמונה|תיישר תמונה"
     "|عدل الافق|سو الافق"
     "|nivelar el horizonte|endereza la imagen|enderezar la imagen"
     "|redresser l horizon|redresse l horizon|redresser l'horizon|redresse l'horizon|redresser l image|redresse l image|redresser l'image"
     "|horizont ausrichten|bild gerade richten|bild begradigen"
     "|выровняи горизонт|выровнять горизонт|выпрями изображение"
     "|水平校正|校正地平线|摆正画面"
     "|水平を合わせて|水平補正|傾きを直して"},
    // Auto-leveling (docs/intents.md, "Auto-leveling", 2026-10-29): the horizon's tilt found and followed, and stopped.
    {K::AutoLevel, A::None, 0,
     "auto level|auto level horizon|auto level the horizon|auto leveling|auto levelling|level horizon|level the horizon|level horizon automatically"
     "|level the horizon automatically|automatic horizon|keep the horizon level|keep horizon level"
     "|יישר אופק אוטומטית|תיישר אופק אוטומטית|יישר את האופק אוטומטית|תיישר את האופק אוטומטית|יישור אופק אוטומטי|אופק אוטומטי"
     "|عدل الافق تلقائيا|تسويه الافق تلقائيا"
     "|nivelar el horizonte automaticamente|nivelacion automatica|horizonte automatico"
     "|niveler l horizon|niveler l'horizon|nivelle l horizon|nivelle l'horizon|horizon automatique|redresser l horizon automatiquement"
     "|redresser l'horizon automatiquement|mise a niveau automatique de l horizon|mise a niveau automatique de l'horizon"
     "|horizont automatisch ausrichten|automatischer horizont|horizont automatisch"
     "|автоматически выровняи горизонт|выровняи горизонт автоматически|автовыравнивание горизонта"
     "|自动水平|自动校正地平线|自动水平校正"
     "|自動水平補正|水平を自動で合わせて|自動で水平"},
    {K::AutoLevelOff, A::None, 0,
     "auto level off|stop auto level|stop auto leveling|stop auto levelling|turn off auto level|disable auto level|no auto level|auto leveling off"
     "|stop leveling the horizon|stop levelling the horizon"
     "|בטל יישור אופק אוטומטי|כבה יישור אופק אוטומטי|תפסיק ליישר את האופק|הפסק ליישר את האופק"
     "|اوقف تعديل الافق التلقائي"
     "|desactivar nivelacion automatica|desactiva la nivelacion automatica"
     "|arrete de niveler l horizon|arrete de niveler l'horizon|desactive la mise a niveau automatique|horizon automatique desactive"
     "|automatischen horizont aus|horizont automatik aus"
     "|выключи автовыравнивание|отключи автовыравнивание горизонта"
     "|关闭自动水平"
     "|自動水平補正をオフ"},
    // Stabilisation (docs/intents.md, "Stabilisation", 2026-11-04): the view steadied against the camera's shake, and back.
    {K::Stabilize, A::None, 0,
     "stabilize|stabilise|stabilize video|stabilise video|stabilize the video|stabilise the video|stabilize picture|stabilise picture|stabilize the picture"
     "|stabilise the picture|stabilize image|stabilise image|stabilize the image|stabilise the image|stabilize on|stabilise on|stabilization on|stabilisation on"
     "|video stabilization|video stabilisation|image stabilization|image stabilisation|steady the picture|steady the video|remove camera shake|reduce camera shake"
     "|ייצב תמונה|תייצב תמונה|ייצב את התמונה|תייצב את התמונה|ייצב וידאו|תייצב וידאו|ייצב את הוידאו|ייצוב תמונה|ייצוב וידאו|הפעל ייצוב"
     "|ثبت الصوره|ثبت الفيديو|تثبيت الصوره|تثبيت الفيديو"
     "|estabilizar video|estabilizar el video|estabiliza el video|estabilizar imagen|estabilizar la imagen|estabiliza la imagen|estabilizacion activada"
     "|stabiliser la video|stabilise la video|stabiliser video|stabiliser l image|stabiliser l'image|stabilise l image|stabilise l'image|stabilisation activee"
     "|video stabilisieren|bild stabilisieren|stabilisiere das bild|stabilisiere das video|stabilisierung an"
     "|стабилизируи видео|стабилизируи изображение|стабилизировать видео|стабилизация видео|стабилизация изображения"
     "|稳定画面|稳定视频|开启防抖|防抖"
     "|手ぶれ補正|手ブレ補正|映像を安定させて|手ぶれ補正をオン"},
    {K::StabilizeOff, A::None, 0,
     "stabilize off|stabilise off|stabilization off|stabilisation off|stop stabilizing|stop stabilising|turn off stabilization|turn off stabilisation"
     "|disable stabilization|disable stabilisation|no stabilization|no stabilisation"
     "|בטל ייצוב|כבה ייצוב|הפסק ייצוב|תפסיק לייצב|בלי ייצוב"
     "|اوقف التثبيت|الغ التثبيت"
     "|desactivar estabilizacion|desactiva la estabilizacion|estabilizacion desactivada"
     "|arrete de stabiliser|desactive la stabilisation|desactiver la stabilisation|stabilisation desactivee"
     "|stabilisierung aus|stabilisierung ausschalten"
     "|выключи стабилизацию|отключи стабилизацию"
     "|关闭防抖|关闭稳定"
     "|手ぶれ補正をオフ|手ブレ補正をオフ"},
    // Black bars (docs/intents.md, "Black bars", 2026-10-30): the picture inside bars encoded in it fills the view, and back.
    {K::CropBars, A::None, 0,
     "crop black bars|crop the black bars|crop the bars|auto crop|autocrop|crop letterbox|crop the letterbox|remove black bars|remove the black bars"
     "|hide black bars|hide the black bars|get rid of the black bars|cut the black bars"
     "|חתוך שוליים שחורים|תחתוך שוליים שחורים|חתוך את השוליים השחורים|תחתוך את השוליים השחורים|חתוך פסים שחורים|תחתוך פסים שחורים"
     "|הסר פסים שחורים|תסיר פסים שחורים|הסר את הפסים השחורים|תסיר את הפסים השחורים|חיתוך אוטומטי"
     "|قص الحواف السوداء|قص الاشرطه السوداء|ازل الاشرطه السوداء"
     "|recortar las bandas negras|recorta las bandas negras|quitar las bandas negras|quita las bandas negras|recorte automatico"
     "|recadrer les bandes noires|recadre les bandes noires|supprimer les bandes noires|supprime les bandes noires|enlever les bandes noires"
     "|enleve les bandes noires|recadrage automatique"
     "|schwarze balken entfernen|schwarze balken abschneiden|entferne die schwarzen balken|automatisch zuschneiden"
     "|обрежь черные полосы|обрезать черные полосы|убери черные полосы|убрать черные полосы|автокадрирование"
     "|裁剪黑边|去除黑边|去掉黑边|自动裁剪"
     "|黒帯をカット|黒帯を消して|黒帯を除去|自動クロップ"},
    {K::CropBarsOff, A::None, 0,
     "show black bars|show the black bars|keep the black bars|uncrop|stop cropping|crop off|auto crop off|turn off auto crop|no crop"
     "|הצג שוליים שחורים|תציג שוליים שחורים|החזר את השוליים השחורים|תחזיר את השוליים השחורים|בטל חיתוך|תבטל חיתוך|בטל חיתוך אוטומטי"
     "|اظهر الحواف السوداء|الغ القص"
     "|mostrar las bandas negras|muestra las bandas negras|sin recorte"
     "|afficher les bandes noires|affiche les bandes noires|annuler le recadrage|annule le recadrage"
     "|schwarze balken anzeigen|zuschneiden aus"
     "|покажи черные полосы|отмени обрезку"
     "|显示黑边|取消裁剪"
     "|黒帯を表示|クロップを解除"},
    {K::CureDark, A::None, 0,
     "too dark|the picture is too dark|picture is too dark|picture too dark|the image is too dark|it is too dark|its too dark|too dim"
     "|lift the shadows|brighten the shadows|i cant see anything|cant see in the dark"
     "|חשוך|חשוך מדי|חשוך פה|התמונה חשוכה|התמונה חשוכה מדי|כהה מדי|התמונה כהה|לא רואים כלום|תבהיר את הצללים|הבהר צללים"
     "|مظلم جدا|الصوره مظلمه"
     "|muy oscuro|demasiado oscuro|la imagen esta oscura"
     "|trop sombre|l image est trop sombre"
     "|zu dunkel|das bild ist zu dunkel"
     "|слишком темно|темно|темная картинка"
     "|太暗|太暗了|画面太暗"
     "|暗すぎる|画面が暗い"},
    {K::CureNoise, A::None, 0,
     "noisy|too noisy|its noisy|there is a hum|theres a hum|humming|i hear a hum|buzzing|there is a buzz|remove the hum|kill the hum|remove hum"
     "|hum removal|get rid of the hum"
     "|יש זמזום|זמזום|יש רעש|רועש|רועש מדי|יש רחש|הורד זמזום|תוריד את הזמזום|תעיף את הזמזום|תוריד רעש"
     "|يوجد طنين|يوجد ضوضاء"
     "|hay zumbido|hay ruido|quitar el zumbido"
     "|ca bourdonne|bourdonnement|il y a du bruit"
     "|es brummt|brummen|brummen entfernen"
     "|гудит|гул|убери гул"
     "|有嗡嗡声|有噪音|去除嗡嗡声"
     "|ブーンという音|ノイズがある|ハム音を消して"},
    {K::CureMuffled, A::None, 0,
     "muffled|the dialogue is muffled|dialogue is muffled|the voices are muffled|voices are muffled|i cant hear the dialogue|cant hear the dialogue"
     "|cant understand the dialogue|speech is unclear|the speech is unclear|unclear dialogue|clearer dialogue|make the dialogue clearer"
     "|הדיבור נבלע|הדיבור לא ברור|לא שומעים את הדיבור|לא שומע את הדיבור|הקולות נבלעים|הדיבור עמום|דיבור עמום|שפר דיבור|תשפר את הדיבור"
     "|الكلام مكتوم|الكلام غير واضح"
     "|dialogo apagado|no se entiende el dialogo"
     "|dialogue etouffe|voix etouffees|on n entend pas les dialogues"
     "|dialog dumpf|stimmen dumpf|man versteht den dialog nicht"
     "|речь неразборчива|голос глухои|не слышно диалог"
     "|对白听不清|声音发闷"
     "|セリフがこもる|声がこもっている|セリフが聞こえない"},
    // Follow-ups (docs/intents.md, "Follow-ups"): resolved by a ConversationState from what was done before. The match
    // ordinals ("the second one", "את השני") are read as a bag of words (Ordinal), like the track commands.
    {K::NextMatch, A::None, 0,
     "next match|the next match|next result|the next result|next one|the next one|next occurrence|another match"
     "|search again|find it again"
     "|התוצאה הבאה|תוצאה הבאה|ההתאמה הבאה|התאמה הבאה|את הבא|את הבאה|המופע הבא|חפש שוב|תחפש שוב"
     "|النتيجه التاليه|التالي|التاليه|المطابقه التاليه"
     "|siguiente resultado|el siguiente resultado|resultado siguiente|el siguiente|la siguiente|proximo resultado|siguiente coincidencia|otra coincidencia"
     "|resultat suivant|le resultat suivant|le suivant|la suivante|occurrence suivante|correspondance suivante"
     "|nachster treffer|der nachste treffer|nachstes ergebnis|das nachste ergebnis|der nachste|das nachste|nachste fundstelle"
     "|следующии результат|следующее совпадение|следующии|следующее"
     "|下一个结果|下一个|下一条|下一处|下一个匹配"
     "|次の結果|次の一致|次のやつ|次の検索結果"},
    {K::PreviousMatch, A::None, 0,
     "previous match|the previous match|previous result|the previous result|previous one|the previous one|the one before"
     "|התוצאה הקודמת|תוצאה קודמת|ההתאמה הקודמת|התאמה קודמת|את הקודם|את הקודמת|המופע הקודם"
     "|النتيجه السابقه|السابق|السابقه|المطابقه السابقه"
     "|resultado anterior|el resultado anterior|el anterior|la anterior|coincidencia anterior"
     "|resultat precedent|le resultat precedent|le precedent|la precedente|occurrence precedente"
     "|vorheriger treffer|der vorherige treffer|vorheriges ergebnis|das vorherige ergebnis|der vorherige|das vorherige|voriger treffer"
     "|предыдущии результат|предыдущее совпадение|предыдущии|предыдущее"
     "|上一个结果|上一个|上一条|上一处|上一个匹配"
     "|前の結果|前の一致|前のやつ|前の検索結果"},
    {K::Undo, A::None, 0,
     "undo|undo that|undo it|undo seek|undo the seek|undo the jump|go back to where i was|back to where i was|take me back to where i was"
     "|return to where i was|go back where i was|where was i"
     "|בטל|תבטל|לבטל|בטל את זה|תבטל את זה|בטל קפיצה|בטל את הקפיצה|תבטל את הקפיצה|תחזור לאיפה שהייתי|חזור לאיפה שהייתי|לחזור לאיפה שהייתי"
     "|תחזיר אותי לאיפה שהייתי|תחזיר לאיפה שהייתי|תחזור למקום שהייתי|חזור למקום שהייתי|תחזור למקום הקודם|חזור למקום הקודם|איפה הייתי"
     "|الغاء|الغ|تراجع عن ذلك|ارجعني الي حيث كنت|ارجع الي حيث كنت|ارجع الي مكاني"
     "|deshacer|deshaz|deshazlo|deshaz eso|vuelve a donde estaba|volver a donde estaba|vuelve donde estaba|regresa a donde estaba"
     "|annuler|annule|annule ca|annuler ca|reviens ou j'etais|revenir ou j'etais|retourne ou j'etais|retourner ou j'etais"
     "|ruckgangig|ruckgangig machen|mach das ruckgangig|mach es ruckgangig|zuruck wo ich war|zuruck dahin wo ich war"
     "|отмени|отменить|отмена|верни как было|вернись туда где я был|верни туда где я был|вернись где я был"
     "|撤销|撤消|回到刚才的位置|回到原来的位置|返回原来的位置|取消跳转"
     "|元に戻す|元に戻して|取り消し|取り消して|さっきの場所に戻って|さっきの場所に戻る"},
    {K::Repeat, A::None, 0,
     "again|once more|one more time|do it again|do that again|repeat|repeat that|same again"
     "|שוב|עוד פעם|שוב פעם|עוד הפעם|עוד פעם אחת|תעשה את זה שוב|תעשה שוב|תחזור על זה|חזור על זה"
     "|مره اخري|مجددا|كرر|اعد|مره ثانيه|من جديد"
     "|otra vez|de nuevo|una vez mas|repite|repitelo|repetir|hazlo otra vez"
     "|encore|encore une fois|une fois de plus|de nouveau|refais|refais-le|repete"
     "|nochmal|noch mal|noch einmal|nochmals|wiederholen|wiederhole|wiederhol das"
     "|еще раз|снова|повтори|повторить|еще разок"
     "|再来一次|再一次|再来|重复|再放一次"
     "|もう一度|もう一回|繰り返し|繰り返して"},
    {K::ReplayCue, A::None, 0,
     "replay|replay that|replay the line|repeat the line|repeat that line|say that again|what did he say|what did she say|what did they say"
     "|what was that"
     "|מה הוא אמר|מה היא אמרה|מה הם אמרו|מה אמרו|תחזור על המשפט|חזור על המשפט|לחזור על המשפט|עוד פעם את המשפט|שוב את המשפט"
     "|ماذا قال|ماذا قالت|ماذا قالوا|اعد الجمله|كرر الجمله"
     "|que dijo|que ha dicho|que dijeron|repite la frase|repetir la frase"
     "|qu'est-ce qu'il a dit|qu'est-ce qu'elle a dit|qu'a-t-il dit|qu'a-t-elle dit|repete la phrase|repeter la phrase"
     "|was hat er gesagt|was hat sie gesagt|was haben sie gesagt|satz wiederholen|wiederhole den satz"
     "|что он сказал|что она сказала|что они сказали|повтори фразу|повтори реплику"
     "|他说什么|她说什么|他说了什么|她说了什么|重复这句|重复这句话|再说一遍"
     "|何て言った|なんて言った|今なんて言った|今何て言った|何と言った|セリフをもう一度"},
    {K::CompareOn, A::None, 0,
     "compare|compare quality|compare the quality|quality comparison|comparison|comparison on|split view|split screen|show the difference"
     "|before and after|compare original|compare with the original"
     "|השווה איכות|תשווה איכות|להשוות איכות|השוואת איכות|השוואה|השווה|תשווה|מסך מפוצל|תצוגה מפוצלת|לפני ואחרי|הצג השוואה|תציג השוואה"
     "|قارن الجوده|مقارنه الجوده|شاشه مقسومه|عرض مقسوم|مقارنه|قارن"
     "|comparar calidad|compara la calidad|comparar la calidad|comparacion de calidad|pantalla dividida|vista dividida|comparar"
     "|comparer la qualite|compare la qualite|comparaison de qualite|ecran partage|vue partagee|comparer|comparaison"
     "|qualitat vergleichen|vergleiche die qualitat|vergleichen|vergleich|geteilte ansicht|geteilter bildschirm"
     "|сравнить качество|сравни качество|сравнение качества|разделенныи экран|сравнение|сравнить"
     "|对比画质|比较画质|画质对比|分屏对比|分屏|对比"
     "|画質を比較|画質比較|比較表示|分割表示|比較"},
    {K::CompareOff, A::None, 0,
     "stop comparing|end comparison|end the comparison|comparison off|exit split view|close comparison|close the comparison|no comparison"
     "|hide comparison|hide the comparison|single view|split view off"
     "|בטל השוואה|תבטל השוואה|סגור השוואה|תסגור השוואה|הפסק השוואה|תפסיק השוואה|תפסיק להשוות|בלי השוואה|הסתר השוואה|תסתיר השוואה"
     "|اوقف المقارنه|الغاء المقارنه|بدون مقارنه|اغلق المقارنه"
     "|dejar de comparar|deja de comparar|quitar la comparacion|quita la comparacion|sin comparacion|cerrar comparacion"
     "|arreter la comparaison|arrete de comparer|arrete la comparaison|fin de la comparaison|sans comparaison|fermer la comparaison"
     "|vergleich beenden|vergleich aus|ohne vergleich|vergleich schliessen|beende den vergleich"
     "|выключи сравнение|убери сравнение|без сравнения|закрои сравнение|отключи сравнение"
     "|关闭对比|退出对比|取消对比|停止对比"
     "|比較をやめる|比較をやめて|比較終了|比較を終了|比較を閉じる"},
    // Chinese and Japanese ordinals ("第二个", "二番目"): the ordinal is the phrase's sign, 100 + n (199: the last).
    {K::NthMatch, A::None, 101, "第一个|第一条|第一个结果|第一条结果|第1个|一番目|1番目|一つ目|最初のやつ|最初の結果"},
    {K::NthMatch, A::None, 102, "第二个|第二条|第二个结果|第二条结果|第2个|二番目|2番目|二つ目"},
    {K::NthMatch, A::None, 103, "第三个|第三条|第三个结果|第三条结果|第3个|三番目|3番目|三つ目"},
    {K::NthMatch, A::None, 104, "第四个|第四条|第四个结果|第四条结果|第4个|四番目|4番目|四つ目"},
    {K::NthMatch, A::None, 105, "第五个|第五条|第五个结果|第五条结果|第5个|五番目|5番目|五つ目"},
    {K::NthMatch, A::None, 106, "第六个|第六条|第六个结果|第六条结果|第6个|六番目|6番目|六つ目"},
    {K::NthMatch, A::None, 107, "第七个|第七条|第七个结果|第七条结果|第7个|七番目|7番目|七つ目"},
    {K::NthMatch, A::None, 108, "第八个|第八条|第八个结果|第八条结果|第8个|八番目|8番目|八つ目"},
    {K::NthMatch, A::None, 109, "第九个|第九条|第九个结果|第九条结果|第9个|九番目|9番目|九つ目"},
    {K::NthMatch, A::None, 110, "第十个|第十条|第十个结果|第十条结果|第10个|十番目|10番目"},
    {K::NthMatch, A::None, 199, "最后一个|最后一条|最后一个结果|最後|最後の|最後のやつ|最後の結果"},
    {K::FindSubtitle, A::Quote, 0,
     "find|search|search for|look for"
     "|מצא|תמצא|מצאי|למצוא|חפש|תחפש|חפשי|תחפשי|לחפש"
     "|ابحث عن|ابحث|بحث عن|بحث|جد|اعثر علي"
     "|buscar|busca|encontrar|encuentra"
     "|chercher|cherche|rechercher|recherche|trouver|trouve"
     "|suchen|suche|suche nach|suchen nach|finden|finde"
     "|наити|наиди|поиск|искать|ищи|поищи"
     "|查找|搜索|搜寻|搜尋|寻找|尋找|查询|查詢"
     "|検索|探す|探して"},
};
// Japanese puts the verb last, attached to what it acts on: 10秒戻る, 1:30に移動, 言葉を検索.
constexpr Group kSuffixGroups[]{
    {K::SeekRelative, A::Span, -1, "巻き戻し|巻き戻す|巻き戻して|巻戻し|戻る|戻って|戻す|戻して"},
    {K::SeekRelative, A::Span, 1, "早送り|早送りして|進む|進める|進めて|進んで|スキップ"},
    {K::SeekAbsolute, A::Time, 0, "に移動|に移動して|へ移動|まで移動|にジャンプ|へジャンプ|に飛ぶ"},
    {K::FindSubtitle, A::Quote, 0, "を検索|を検索して|を探す|を探して"},
};
// What a word may be besides a verb.
enum : std::int32_t {
    kPolite = 1, kPlayFiller = 2, kSoundFiller = 4, kVolumeFiller = 8, kRelativeFiller = 16, kAbsoluteFiller = 32,
    kAnd = 64, kPercent = 128, kBack = 256, kForward = 512, kModest = 1024, kTrailing = 2048,
};
constexpr Set kWordSets[]{
    // Courtesy, and the hesitations and discourse words of speech ("um", "ok so", "אז", "יאללה"): set aside at either
    // end of a command.
    {kPolite, "please|now|בבקשה|עכשיו|bitte|jetzt|пожалуиста|сеичас|svp|maintenant|ahora|الان|رجاء|请|請|ください|下さい|お願いします|お願い"
              "|pls|plz|thanks|just|kindly|hey|ok|okay|so|well|umm|uh|uhh|er|erm|hmm|then|right"
              "|תודה|רק|אז|טוב|נו|יאללה|אוקיי|אוקי|אמ|אממ|אה|אהה|אולי|רגע"
              "|gracias|bueno|merci|alors|danke|mal|спасибо|ну|даваи|شكرا|يلا"},
    // Set aside after a command ("תעצור שוב"), but a command of its own alone (Repeat). English "again" is not: "play it
    // again" stays a quote to look for.
    {kTrailing, "שוב"},
    // A softener changes nothing: "a bit louder", "go back a little", "תגביר קצת".
    {kModest, "slightly|somewhat|bit|little|tad|קצת|טיפה|poco|poquito|peu|bisschen|wenig|etwas|немного|немножко|чуть|قليلا|شوي|شويه"},
    // Small words a command may carry around its argument without changing its meaning.
    {kPlayFiller | kSoundFiller | kVolumeFiller | kRelativeFiller, "לי|the|it|את|el|la|le|das|den|die"},
    // "rewind the movie 10 seconds", "תחזיר את הסרט דקה אחורה".
    {kPlayFiller | kRelativeFiller, "video|movie|film|playback|הסרט|הסרטון|הוידאו|הווידאו|הניגון|pelicula|reproduccion|wiedergabe|lecture|видео|фильм|الفيديو|الفيلم|التشغيل"},
    {kSoundFiller | kVolumeFiller, "sound|volume|הקול|sonido|son|ton|звук|الصوت|volumen|lautstarke|громкость"},
    {kSoundFiller, "audio|הסאונד"},
    {kVolumeFiller | kRelativeFiller | kAbsoluteFiller, "by|to|at|ב|ל|на|الي"},
    {kRelativeFiller | kAbsoluteFiller, "for|of|אל"},
    {kVolumeFiller | kRelativeFiller, "de|por|en|ب"},
    {kVolumeFiller | kRelativeFiller | kPolite, "um"}, // German "by" ("lauter um 20"), and the English hesitation.
    {kVolumeFiller | kAbsoluteFiller, "a|al|au|auf"},
    {kVolumeFiller, "על|העוצמה|הווליום|sur|до|علي|を|に"},
    {kRelativeFiller, "hacia|nach|مده|لمده"},
    {kAbsoluteFiller, "zu|к"},
    {kAnd, "and|y|et|und|и|و"},
    {kPercent, "%|percent|אחוז|אחוזים|prozent|процент|процента|процентов|بالمئه|بالمائه|パーセント"},
    {kBack, "back|backward|backwards|אחורה|atras|arriere|zuruck|ruckwarts|назад|للخلف|للوراء|戻る|戻って|戻して|戻す|巻き戻し|巻き戻す|巻き戻して"},
    {kForward, "forward|forwards|ahead|קדימה|adelante|avant|vor|vorwarts|вперед|للامام|進む|進める|進めて|進んで|早送り"},
};
// Counts in quarters, so that a half and a quarter are exact.
constexpr Set kNumberSets[]{
    {1, "quarter|רבע|cuarto|quart|viertel|четверть|ربع"},
    {2, "half|חצי|medio|media|demi|demie|halbe|halben|halb|пол|نصف"},
    {4, "a|an|one|אחת|אחד|un|una|uno|une|ein|eine|einen|einer|один|одну|одна|واحد|واحده"},
    {6, "полторы|полтора"},
    {8, "two|שתי|שתיים|שני|שניים|dos|deux|zwei|два|две|اثنين|اثنتين"},
    {12, "three|שלוש|שלושה|tres|trois|drei|три|ثلاث|ثلاثه"},
    {16, "four|ארבע|ארבעה|cuatro|quatre|vier|четыре|اربع|اربعه"},
    {20, "five|חמש|חמישה|cinco|cinq|funf|пять|خمس|خمسه"},
    {24, "six|שש|שישה|seis|sechs|шесть|ست|سته"},
    {28, "seven|שבע|שבעה|siete|sept|sieben|семь|سبع|سبعه"},
    {32, "eight|שמונה|ocho|huit|acht|восемь|ثمان|ثمانيه"},
    {36, "nine|תשע|תשעה|nueve|neuf|neun|девять|تسع|تسعه"},
    {40, "ten|עשר|עשרה|diez|dix|zehn|десять|عشر|عشره"},
    {60, "fifteen|quince|quinze|funfzehn|пятнадцать"},
    {80, "twenty|עשרים|veinte|vingt|zwanzig|двадцать|عشرين|عشرون"},
    {120, "thirty|שלושים|treinta|trente|dreißig|dreissig|тридцать|ثلاثين|ثلاثون"},
    {160, "forty"},
    {240, "sixty"},
};
// Seconds in one unit. The Hebrew and Arabic duals are two units; some words are a fraction of the next unit.
constexpr Set kUnitSets[]{
    {1, "s|sec|secs|second|seconds|שניה|שנייה|שניות|שנ|segundo|segundos|seg|seconde|secondes|sekunde|sekunden|sek"
        "|секунда|секунды|секунду|секунд|сек|с|ثانيه|ثواني|ثوان|秒|秒钟|秒鐘|秒間"},
    {2, "ثانيتين"},
    {30, "полминуты"},
    {60, "m|min|mins|minute|minutes|דקה|דקות|דק|minuto|minutos|minuten|минута|минуты|минуту|минут|мин|دقيقه|دقائق|دقايق|分|分钟|分鐘|分間"},
    {120, "דקותיים|دقيقتين"},
    {1800, "полчаса|demi-heure"},
    {3600, "h|hr|hrs|hour|hours|שעה|שעות|hora|horas|heure|heures|stunde|stunden|std|час|часа|часов|ч|ساعه|ساعات|小时|小時|个小时|個小時|時間|钟头"},
    {7200, "שעתיים|ساعتين"},
};
// ---- Track and language commands -------------------------------------------------------------------------------
// Read as a set of words, in any order: a track noun (audio or subtitles), a language, or off/on/next, and nothing but
// the words below and courtesy besides ("switch subtitles to hebrew", "תעביר לאודיו באנגלית", "english audio please",
// "כתוביות בעברית", "turn off the subtitles", "next audio track"). Hebrew prefixes attached to a word (ב, ל, ה, ו, מ, ש)
// are tried off when the whole word is not listed. Chinese and Japanese track commands are not read.
constexpr std::int32_t kTrackAudio = 1, kTrackSubtitles = 2, kTrackOff = 3, kTrackOn = 4, kTrackNext = 5, kTrackFiller = 6, kTrackLanguage = 16;
struct Language { char code[4]; const char* english; const char* native; };
constexpr Language kLanguages[]{
    {"eng", "English", "English"}, {"heb", "Hebrew", "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa"}, {"fra", "French", "Fran\xc3\xa7" "ais"},
    {"spa", "Spanish", "Espa\xc3\xb1ol"}, {"deu", "German", "Deutsch"}, {"rus", "Russian", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"},
    {"ara", "Arabic", "\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9"}, {"zho", "Chinese", "\xe4\xb8\xad\xe6\x96\x87"},
    {"jpn", "Japanese", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"}, {"ita", "Italian", "Italiano"}, {"por", "Portuguese", "Portugu\xc3\xaas"}};
constexpr Set kTrackSets[]{
    {kTrackAudio, "audio|soundtrack|dub|dubbing|dubbed|language|languages|voice|voices|narration"
                  "|אודיו|שמע|שפה|שפת|פסקול|דיבוב|מדובב|קריינות"
                  "|idioma|doblaje|doblado|langue|doublage|sprache|synchronfassung|tonspur|аудио|озвучка|озвучку|язык|дорожка|дорожку"
                  "|اللغه|لغه|الدبلجه|دبلجه|مدبلج"},
    {kTrackSubtitles, "subtitles|subtitle|subs|captions|caption|cc|כתוביות|כתובית|תרגום"
                      "|subtitulos|subtitulo|sous-titres|sous-titre|soustitres|untertitel|субтитры|субтитров|субтитрами|ترجمه|الترجمه|الترجمات"},
    {kTrackOff, "off|none|no|disable|hide|remove|כבה|תכבה|לכבות|כבי|הסר|תסיר|להסיר|בטל|תבטל|לבטל|בלי|ללא|הסתר|תסתיר|להסתיר"
                "|desactivar|desactiva|quitar|quita|sin|ocultar|desactiver|enlever|enleve|sans|masquer|aus|ausschalten|deaktivieren|ohne"
                "|выключи|выключить|убери|убрать|без|отключи|اخف|اخفاء|الغاء|بدون|بلا|اطفئ"},
    {kTrackOn, "on|enable|show|display|activate|הפעל|תפעיל|להפעיל|הדלק|תדליק|להדליק|הצג|תציג|להציג|תראה"
               "|activar|activa|mostrar|muestra|activer|afficher|affiche|an|ein|einschalten|anzeigen|aktivieren|включи|включить|покажи|показать"
               "|اظهر|اظهار|فعل|تفعيل"},
    {kTrackNext, "next|another|other|different|הבא|הבאה|אחר|אחרת|שונה|siguiente|otro|otra|suivant|suivante|autre|nachste|nachsten|andere|anderen"
                 "|следующая|следующии|следующую|другои|другая|другую|التالي|اخري|اخر"},
    {kTrackFiller, "switch|change|set|use|put|select|choose|pick|give|swap|turn|go|make|want|to|into|in|the|a|track|tracks|me|with|for|of"
                   "|stream|version|mode|over"
                   "|תעביר|להעביר|העבר|תעבור|לעבור|עבור|תחליף|להחליף|החלף|שנה|תשנה|לשנות|שים|תשים|לשים|בחר|תבחר|לבחור|רוצה|את|ל|ב|ה|על|עם"
                   "|רצועת|רצועה|גרסה|גרסת|לי|אותם|אותן"
                   "|cambia|cambiar|pon|poner|usa|usar|elige|al|en|el|la|los|las|de|con|pista"
                   "|changer|mets|mettre|passe|passer|utilise|le|les|au|des|piste"
                   "|wechsle|wechseln|stelle|stell|schalte|nimm|auf|zu|die|den|der|das|mit|spur"
                   "|переключи|переключить|смени|сменить|поставь|на|в|с"
                   "|غير|حول|بدل|اختر|الي|ب|علي|مع|المسار"},
    // Languages: kTrackLanguage + the index in kLanguages.
    {kTrackLanguage + 0, "english|אנגלית|ingles|anglais|englisch|англиискии|англииском|انجليزي|الانجليزيه|انكليزي|الانكليزيه"
                         "|بالانجليزي|بالانجليزيه|بالانكليزي"},
    {kTrackLanguage + 1, "hebrew|עברית|hebreo|hebreu|hebraisch|иврит|ивритом|иврите|العبريه|عبري|بالعبري|بالعبريه"},
    {kTrackLanguage + 2, "french|צרפתית|frances|francais|franzosisch|французскии|французском|الفرنسيه|فرنسي|بالفرنسي|بالفرنسيه"},
    {kTrackLanguage + 3, "spanish|ספרדית|espanol|espagnol|spanisch|испанскии|испанском|الاسبانيه|اسباني|بالاسباني|بالاسبانيه"},
    {kTrackLanguage + 4, "german|גרמנית|aleman|allemand|deutsch|немецкии|немецком|الالمانيه|الماني|بالالماني|بالالمانيه"},
    {kTrackLanguage + 5, "russian|רוסית|ruso|russe|russisch|русскии|русском|الروسيه|روسي|بالروسي|بالروسيه"},
    {kTrackLanguage + 6, "arabic|ערבית|arabe|arabisch|арабскии|арабском|العربيه|عربي|بالعربي|بالعربيه"},
    {kTrackLanguage + 7, "chinese|סינית|chino|chinois|chinesisch|китаискии|китаиском|الصينيه|صيني|بالصيني|بالصينيه"},
    {kTrackLanguage + 8, "japanese|יפנית|japones|japonais|japanisch|японскии|японском|اليابانيه|ياباني|بالياباني|باليابانيه"},
    {kTrackLanguage + 9, "italian|איטלקית|italiano|italien|italienisch|итальянскии|итальянском|الايطاليه|ايطالي|بالايطالي|بالايطاليه"},
    {kTrackLanguage + 10, "portuguese|פורטוגזית|portugues|portugais|portugiesisch|португальскии|португальском|البرتغاليه|برتغالي|بالبرتغالي|بالبرتغاليه"},
};
// Courtesy of more than one word, at either end of a command.
constexpr std::string_view kPoliteRuns[]{"por favor", "s'il vous plait", "s'il te plait", "من فضلك", "لو سمحت",
    "thank you", "for me", "right now", "if you can", "if you could", "if possible", "if you don't mind", "if you would",
    "אם אפשר", "אם תוכל", "תודה רבה", "רגע אחד"};
// Request frames: what people put in front of a command when they speak to someone ("can you", "i'd like you to",
// "אני רוצה ש", "אשמח אם", "תוכל"). Set aside, longest first; what follows must still be a whole command. Hebrew frames
// that end in a subordinator also accept it attached to the verb ("אשמח שתמשיך", "אני רוצה שתעצור"): see Parse.
constexpr std::string_view kRequestRuns[]{
    "can you please", "could you please", "would you please", "will you please", "can you", "could you", "would you", "will you",
    "would you kindly", "would you mind", "can u", "could u", "please could you", "please can you",
    "i want you to", "i'd like you to", "i would like you to", "i need you to", "i'd love you to",
    "i want to", "i'd like to", "i would like to", "i wanna", "i need to", "we need to", "can we", "could we",
    "let's", "lets", "let me", "go ahead and", "do me a favor and", "do me a favour and", "you can", "you could",
    "try to", "time to", "i said", "i want", "i'd like", "i would like", "i need", "give me",
    "אני רוצה ש", "אני רוצה", "אני רוצה שאתה", "הייתי רוצה ש", "הייתי רוצה", "אני צריך ש", "אני צריכה ש", "אני צריך", "אני צריכה",
    "אשמח אם", "אשמח ש", "אשמח", "הייתי שמח אם", "הייתי שמחה אם", "אני מבקש ש", "אני מבקשת ש", "אני מבקש", "אני מבקשת",
    "תוכל בבקשה", "תוכלי בבקשה", "תוכל", "תוכלי", "אתה יכול", "את יכולה", "אתה יכול בבקשה", "את יכולה בבקשה",
    "אפשר בבקשה", "אפשר", "בוא", "בואו", "תעשה לי טובה", "תעשי לי טובה", "תעשה טובה", "תן לי", "תני לי",
    "puedes", "podrias", "podria", "puede", "quiero que", "quisiera que", "me gustaria que", "necesito que",
    "peux-tu", "peux tu", "pourrais-tu", "pourrais tu", "pouvez-vous", "pouvez vous", "tu peux", "je veux que tu", "je voudrais que tu",
    "kannst du", "konntest du", "konnen sie", "konnten sie", "ich mochte", "ich will",
    "можешь", "можете", "можно", "я хочу", "ты можешь",
    "ممكن", "هل يمكنك", "هل يمكن", "اريد", "اريد ان", "ارجو"};
// After a request frame an English verb may come as a gerund ("would you mind pausing", "could you try skipping
// ahead"): it is read as the verb. Alone, "playing" or "stopping" is no command (it may be a quote to look for).
struct Gerund { std::string_view gerund, verb; };
constexpr Gerund kGerunds[]{
    {"pausing", "pause"}, {"stopping", "stop"}, {"holding", "hold"}, {"freezing", "freeze"}, {"playing", "play"}, {"resuming", "resume"},
    {"continuing", "continue"}, {"unpausing", "unpause"}, {"rewinding", "rewind"}, {"muting", "mute"}, {"unmuting", "unmute"},
    {"silencing", "silence"}, {"skipping", "skip"}, {"jumping", "jump"}, {"going", "go"}, {"seeking", "seek"}, {"turning", "turn"},
    {"raising", "raise"}, {"lowering", "lower"}, {"increasing", "increase"}, {"decreasing", "decrease"}, {"reducing", "reduce"},
    {"setting", "set"}, {"restarting", "restart"}, {"starting", "start"}, {"searching", "search"}, {"finding", "find"}, {"looking", "look"},
    {"switching", "switch"}, {"changing", "change"}, {"putting", "put"}, {"showing", "show"}, {"hiding", "hide"}, {"forwarding", "forward"}};
// Softeners of more than one word, set aside like kModest words ("go back a little", "turn it up a bit").
constexpr std::string_view kModestRuns[]{"a bit", "a little", "a little bit", "a tad", "just a bit", "just a little", "a bit more", "a little more",
    "un poco", "un poquito", "un peu", "un petit peu", "ein bisschen", "ein wenig", "чуть-чуть", "עוד קצת", "עוד טיפה"};
// Courtesy attached to a Chinese or Japanese command: 请暂停, 一時停止してください.
constexpr std::string_view kPolitePrefixes[]{"请", "請", "麻烦", "麻煩"};
constexpr std::string_view kPoliteSuffixes[]{"ください", "下さい", "お願いします", "お願い"};
// Two-word percent signs.
constexpr std::string_view kPercentRuns[]{"por ciento", "pour cent", "في المئه"};

template<std::size_t G, class T> constexpr std::size_t Total(const T (&groups)[G], std::string_view T::* list) noexcept {
    std::size_t n = 0;
    for (const auto& g : groups) { ++n; for (const char c : g.*list) if (c == '|') ++n; }
    return n;
}
template<std::size_t N, std::size_t G> constexpr std::array<Phrase, N> Expand(const Group (&groups)[G]) noexcept {
    std::array<Phrase, N> out{}; std::size_t n = 0;
    for (const auto& g : groups) {
        auto rest = g.phrases;
        for (;;) {
            const auto bar = rest.find('|');
            out[n++] = {rest.substr(0, bar), g.kind, g.argument, g.sign};
            if (bar == rest.npos) break;
            rest.remove_prefix(bar + 1);
        }
    }
    return out;
}
template<std::size_t N, std::size_t G> constexpr std::array<Word, N> Expand(const Set (&sets)[G]) noexcept {
    std::array<Word, N> out{}; std::size_t n = 0;
    for (const auto& s : sets) {
        auto rest = s.words;
        for (;;) {
            const auto bar = rest.find('|');
            out[n++] = {rest.substr(0, bar), s.value};
            if (bar == rest.npos) break;
            rest.remove_prefix(bar + 1);
        }
    }
    return out;
}
constexpr auto kPhrases = Expand<Total(kGroups, &Group::phrases)>(kGroups);
constexpr auto kSuffixes = Expand<Total(kSuffixGroups, &Group::phrases)>(kSuffixGroups);
constexpr auto kWords = Expand<Total(kWordSets, &Set::words)>(kWordSets);
constexpr auto kNumbers = Expand<Total(kNumberSets, &Set::words)>(kNumberSets);
constexpr auto kUnits = Expand<Total(kUnitSets, &Set::words)>(kUnitSets);
constexpr auto kTrackWords = Expand<Total(kTrackSets, &Set::words)>(kTrackSets);

// Chained hash index over a table, built at compile time: a lookup touches one bucket instead of the whole table.
constexpr std::uint32_t Hash(std::string_view s) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : s) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }
    return h;
}
constexpr std::uint16_t kNone = 0xffff;
// A spaced phrase is found by its first word; a Chinese or Japanese one by its first character.
constexpr std::string_view Key(const Phrase& p) noexcept { return Cjk(p.words) ? p.words.substr(0, 3) : p.words.substr(0, p.words.find(' ')); }
constexpr std::string_view Key(const Word& w) noexcept { return w.word; }
struct Run { std::string_view words; }; // A multi-word run, found by its first word.
constexpr std::string_view Key(const Run& r) noexcept { return r.words.substr(0, r.words.find(' ')); }
template<std::size_t N> constexpr std::array<Run, N> Runs(const std::string_view (&runs)[N]) noexcept {
    std::array<Run, N> out{}; for (std::size_t i = 0; i < N; ++i) out[i] = {runs[i]}; return out;
}
struct Tail { std::string_view words; unsigned count; }; // The same, found by its last word.
constexpr std::string_view Key(const Tail& t) noexcept { const auto space = t.words.rfind(' '); return space == t.words.npos ? t.words : t.words.substr(space + 1); }
template<std::size_t N> constexpr std::array<Tail, N> Tails(const std::string_view (&runs)[N]) noexcept {
    std::array<Tail, N> out{};
    for (std::size_t i = 0; i < N; ++i) { unsigned n = 1; for (const char c : runs[i]) if (c == ' ') ++n; out[i] = {runs[i], n}; }
    return out;
}
template<std::size_t N, std::size_t B> struct Index { std::array<std::uint16_t, B> head{}; std::array<std::uint16_t, N> next{}; };
template<std::size_t B, class E, std::size_t N> constexpr Index<N, B> MakeIndex(const std::array<E, N>& entries) noexcept {
    Index<N, B> index{};
    for (auto& h : index.head) h = kNone;
    for (std::size_t i = N; i-- > 0;) {
        const auto bucket = Hash(Key(entries[i])) % B;
        index.next[i] = index.head[bucket]; index.head[bucket] = static_cast<std::uint16_t>(i);
    }
    return index;
}
constexpr auto kPhraseIndex = MakeIndex<2048>(kPhrases);
constexpr auto kWordIndex = MakeIndex<512>(kWords);
constexpr auto kNumberIndex = MakeIndex<512>(kNumbers);
constexpr auto kUnitIndex = MakeIndex<512>(kUnits);
constexpr auto kTrackIndex = MakeIndex<1024>(kTrackWords);
constexpr auto kRequests = Runs(kRequestRuns);
constexpr auto kRequestIndex = MakeIndex<256>(kRequests);
constexpr auto kPoliteHeads = Runs(kPoliteRuns);
constexpr auto kPoliteHeadIndex = MakeIndex<64>(kPoliteHeads);
constexpr auto kPoliteTails = Tails(kPoliteRuns);
constexpr auto kPoliteTailIndex = MakeIndex<64>(kPoliteTails);
constexpr auto kModestHeads = Runs(kModestRuns);
constexpr auto kModestHeadIndex = MakeIndex<64>(kModestHeads);
constexpr auto kModestTails = Tails(kModestRuns);
constexpr auto kModestTailIndex = MakeIndex<64>(kModestTails);
template<std::size_t N, std::size_t B> std::int32_t Lookup(const std::array<Word, N>& table, const Index<N, B>& index, std::string_view word) noexcept {
    for (auto i = index.head[Hash(word) % B]; i != kNone; i = index.next[i]) if (table[i].word == word) return table[i].value;
    return 0;
}
std::int32_t Class(std::string_view word) noexcept { return Lookup(kWords, kWordIndex, word); }
std::int64_t Unit(std::string_view word) noexcept { return Lookup(kUnits, kUnitIndex, word); }
int Direction(std::string_view word) noexcept {
    const auto c = Class(word);
    return (c & kBack) ? -1 : (c & kForward) ? 1 : 0;
}

// ---- Tokens ----------------------------------------------------------------------------------------------------
// One white-space separated word: where it is in the command, and its keyword form (folded, edge punctuation
// removed). A word longer than the keyword buffer keeps its first part only (`truncated`): enough for a Chinese
// or Japanese verb in front of a long quote; it is never a whole keyword.
struct Token {
    std::string_view raw;
    char folded[kTokenBytes]{};
    unsigned size = 0, lead = 0; // lead: bytes of `raw` before the first kept scalar.
    bool truncated = false;
    std::string_view View() const noexcept { return {folded, size}; }
};
void Keyword(Token& token) noexcept {
    auto rest = token.raw; bool started = false;
    while (!rest.empty()) {
        const auto at = static_cast<unsigned>(rest.data() - token.raw.data());
        std::uint32_t scalar = 0;
        if (!text::Utf8(rest, scalar)) return;
        scalar = Fold(scalar);
        if (!scalar) continue;
        if (!started) { if (Opening(scalar)) continue; started = true; token.lead = at; }
        if (token.size + 4 > kTokenBytes) { token.truncated = true; return; }
        token.size += text::Encode(scalar, token.folded + token.size);
    }
    while (token.size) {
        unsigned start = token.size - 1;
        while (start && (static_cast<unsigned char>(token.folded[start]) & 0xc0) == 0x80) --start;
        std::string_view last{token.folded + start, token.size - start}; std::uint32_t scalar = 0;
        if (!text::Utf8(last, scalar) || !Closing(scalar)) break;
        token.size = start;
    }
}
// Where in `raw` the keyword's first `folded_bytes` end.
std::size_t RawOffset(const Token& token, unsigned folded_bytes) noexcept {
    auto rest = token.raw.substr(token.lead); unsigned emitted = 0;
    while (emitted < folded_bytes && !rest.empty()) {
        std::uint32_t scalar = 0; char scratch[4];
        if (!text::Utf8(rest, scalar)) break;
        scalar = Fold(scalar);
        if (scalar) emitted += text::Encode(scalar, scratch);
    }
    return token.raw.size() - rest.size();
}
void TrimFront(Token& token, unsigned folded_bytes) noexcept {
    token.raw.remove_prefix(RawOffset(token, folded_bytes));
    std::memmove(token.folded, token.folded + folded_bytes, token.size - folded_bytes);
    token.size -= folded_bytes; token.lead = 0;
}
// False for ill-formed UTF-8. `more`: words beyond kMaxTokens exist (only a quote may be that long).
bool Tokenize(std::string_view command, Token* tokens, unsigned& count, bool& more) noexcept {
    count = 0; more = false;
    const char* begin = nullptr;
    auto rest = command;
    const auto close = [&](const char* end) {
        if (!begin) return;
        if (count < kMaxTokens) { tokens[count].raw = {begin, static_cast<std::size_t>(end - begin)}; Keyword(tokens[count]); ++count; }
        else more = true;
        begin = nullptr;
    };
    while (!rest.empty()) {
        const char* at = rest.data(); std::uint32_t scalar = 0;
        if (!text::Utf8(rest, scalar)) return false;
        const auto folded = text::FoldScalar(scalar, true);
        if (folded == ' ') close(at); else if (!begin && folded) begin = at; // Invisible characters alone are not a word.
    }
    close(command.data() + command.size());
    return true;
}
// Words of `words` (space separated) found at the start of the token list: their number when all match, else 0.
unsigned Words(std::string_view words, const Token* tokens, unsigned count) noexcept {
    for (unsigned i = 0;; ++i) {
        const auto space = words.find(' ');
        if (i >= count || tokens[i].truncated || tokens[i].View() != words.substr(0, space)) return 0;
        if (space == words.npos) return i + 1;
        words.remove_prefix(space + 1);
    }
}
unsigned WordCount(std::string_view words) noexcept {
    unsigned n = 1;
    for (const char c : words) if (c == ' ') ++n;
    return n;
}
// The longest run of a table found at the start of the token list (by its first word), or ending just before `end`
// (by its last word): its number of tokens, else 0.
template<std::size_t N, std::size_t B> unsigned Front(const std::array<Run, N>& runs, const Index<N, B>& index, const Token* tokens, unsigned count) noexcept {
    if (!count || tokens[0].truncated) return 0;
    unsigned best = 0;
    for (auto i = index.head[Hash(tokens[0].View()) % B]; i != kNone; i = index.next[i])
        if (const auto n = Words(runs[i].words, tokens, count); n > best) best = n;
    return best;
}
template<std::size_t N, std::size_t B> unsigned Back(const std::array<Tail, N>& runs, const Index<N, B>& index, const Token* tokens, unsigned begin, unsigned end) noexcept {
    if (end == begin || tokens[end - 1].truncated) return 0;
    unsigned best = 0;
    for (auto i = index.head[Hash(tokens[end - 1].View()) % B]; i != kNone; i = index.next[i]) {
        const auto n = runs[i].count;
        if (n > best && end - begin >= n && Words(runs[i].words, tokens + (end - n), n)) best = n;
    }
    return best;
}
// Tokens of courtesy at the start of the list, or just before `end`.
unsigned PoliteFront(const Token* tokens, unsigned count) noexcept {
    if (count && !tokens[0].truncated && (Class(tokens[0].View()) & kPolite)) return 1;
    return Front(kPoliteHeads, kPoliteHeadIndex, tokens, count);
}
unsigned PoliteBack(const Token* tokens, unsigned begin, unsigned end) noexcept {
    if (end > begin && !tokens[end - 1].truncated && (Class(tokens[end - 1].View()) & (kPolite | kTrailing))) return 1;
    return Back(kPoliteTails, kPoliteTailIndex, tokens, begin, end);
}
bool Modest(const Token& token) noexcept { return !token.truncated && (Class(token.View()) & kModest) != 0; }
unsigned RequestFront(const Token* tokens, unsigned count) noexcept { return Front(kRequests, kRequestIndex, tokens, count); }
// Is `word` the first word of a spaced phrase of the grammar?
bool Head(std::string_view word) noexcept {
    for (auto i = kPhraseIndex.head[Hash(word) % kPhraseIndex.head.size()]; i != kNone; i = kPhraseIndex.next[i])
        if (!Cjk(kPhrases[i].words) && Key(kPhrases[i]) == word) return true;
    return false;
}
// Hebrew attaches its one-letter prefixes (ב in, ל to, ה the, ו and, מ from, ש that) to the next word.
bool HebrewPrefix(std::string_view word) noexcept {
    if (word.size() < 4 || static_cast<unsigned char>(word[0]) != 0xd7) return false;
    const auto letter = static_cast<unsigned char>(word[1]);
    return letter == 0x91 || letter == 0x9c || letter == 0x94 || letter == 0x95 || letter == 0x9e || letter == 0xa9;
}

// ---- Track commands --------------------------------------------------------------------------------------------
std::int32_t TrackClass(std::string_view word, bool* prefixed = nullptr) noexcept {
    for (unsigned strip = 0;; ++strip) {
        if (const auto value = Lookup(kTrackWords, kTrackIndex, word)) { if (prefixed) *prefixed = strip != 0; return value; }
        if (strip == 2 || !HebrewPrefix(word)) return 0;
        word.remove_prefix(2);
    }
}
// The tokens [begin, end) as a track command, or false when they are not one (the rest of the grammar reads them).
bool Track(const Token* tokens, unsigned begin, unsigned end, const MediaBounds& bounds, Intent& intent) noexcept {
    std::int32_t noun = 0, language = -1, choice = 0; unsigned words = 0; bool asked = false;
    for (unsigned i = begin; i < end; ++i) {
        if (tokens[i].truncated) return false;
        const auto word = tokens[i].View();
        bool prefixed = false;
        const auto value = TrackClass(word, &prefixed);
        if (!value) { if (Class(word) & kPolite) continue; return false; }
        ++words; asked = asked || prefixed;
        if (value == kTrackAudio || value == kTrackSubtitles) { if (noun && noun != value) return false; noun = value; }
        else if (value >= kTrackLanguage) { if (language >= 0 && language != value - kTrackLanguage) return false; language = value - kTrackLanguage; }
        else { asked = true; if (value != kTrackFiller) { if (choice && choice != value) return false; choice = value; } }
    }
    if (!words || (!noun && language < 0)) return false; // "turn it off", "next": not about a track.
    // A language's name alone ("עברית", "english") may as well be a word to look for in the subtitles: it asks for a
    // track only with a track named or a request around it ("switch to english", "בעברית"); courtesy is not enough.
    if (!noun && !asked) return false;
    if (noun == kTrackAudio && choice == kTrackOff) return false; // "audio off" is no track choice.
    intent = {};
    intent.kind = noun == kTrackAudio ? IntentKind::SelectAudio : noun == kTrackSubtitles ? IntentKind::SelectSubtitles : IntentKind::SelectLanguage;
    if (choice == kTrackOff) intent.track = TrackChoice::Off; // "turn off the english subtitles": off, whatever the language.
    else if (language >= 0) {
        intent.track = TrackChoice::Language;
        std::memcpy(intent.language, kLanguages[language].code, 4);
    } else if (choice == kTrackNext) intent.track = TrackChoice::Next;
    else if (noun == kTrackSubtitles) intent.track = TrackChoice::On; // "subtitles", "show subtitles".
    else return false; // "audio", "change the audio track": which one?
    const unsigned available = intent.kind == IntentKind::SelectAudio ? bounds.audio_tracks :
                               intent.kind == IntentKind::SelectSubtitles ? bounds.subtitle_tracks : bounds.audio_tracks + bounds.subtitle_tracks;
    // Another audio track needs two to choose from; subtitles cycle through off as well.
    const bool possible = available && (intent.kind != IntentKind::SelectAudio || intent.track != TrackChoice::Next || available >= 2);
    intent.status = possible ? IntentStatus::Ok : IntentStatus::Unavailable;
    return true;
}

// ---- Match ordinals ---------------------------------------------------------------------------------------------
// "the second one", "the 3rd match", "go to the last one", "את השני", "התוצאה השלישית", "el segundo", "la deuxieme", "der
// zweite treffer", "второи результат", "الثاني": one ordinal and words from the filler set, in any order ("one" only after
// the ordinal: "one second" is no ordinal). An ordinal needs another word with it ("first", "segundo" alone may be a quote
// or a unit), except the forms that stand alone (kStandalone): Hebrew and Arabic definite forms, and Russian, which has no
// article. No search verb is among the fillers, nor does a match phrase start with one: "find the second one" and "find
// next week" look for those words; nor an indefinite article ("une seconde", "un segundo": a second of time).
// Chinese and Japanese ordinals are phrases (kGroups).
constexpr std::int32_t kOrdinalLast = 99, kOrdinalFiller = 100, kOrdinalAfter = 101, kStandalone = 1000;
constexpr Set kOrdinalSets[]{
    {1, "first|1st|primero|primera|premier|premiere|erste|ersten|erster|erstes"},
    {2, "second|2nd|segundo|segunda|deuxieme|seconde|zweite|zweiten|zweiter|zweites"},
    {3, "third|3rd|tercero|tercera|tercer|troisieme|dritte|dritten|dritter|drittes"},
    {4, "fourth|4th|cuarta|quatrieme|vierte|vierten|vierter|viertes"},
    {5, "fifth|5th|quinto|quinta|cinquieme|funfte|funften|funfter|funftes"},
    {6, "sixth|6th|sexto|sexta|sixieme|sechste|sechsten|sechster|sechstes"},
    {7, "seventh|7th|septimo|septima|septieme|siebte|siebten|siebter|siebtes"},
    {8, "eighth|8th|octavo|octava|huitieme|achte|achten|achter|achtes"},
    {9, "ninth|9th|noveno|novena|neuvieme|neunte|neunten|neunter|neuntes"},
    {10, "tenth|10th|decimo|decima|dixieme|zehnte|zehnten|zehnter|zehntes"},
    {kOrdinalLast, "last|ultimo|ultima|dernier|derniere|letzte|letzten|letzter|letztes"},
    {kStandalone + 1, "הראשון|הראשונה|الاول|الاولي|первыи|первая|первое|первую"},
    {kStandalone + 2, "השני|השנייה|השניה|الثاني|الثانيه|второи|вторая|второе|вторую"},
    {kStandalone + 3, "השלישי|השלישית|الثالث|الثالثه|третии|третья|третье|третью"},
    {kStandalone + 4, "הרביעי|הרביעית|الرابع|الرابعه|четвертыи|четвертая|четвертое|четвертую"},
    {kStandalone + 5, "החמישי|החמישית|الخامس|الخامسه|пятыи|пятая|пятое|пятую"},
    {kStandalone + 6, "השישי|השישית|السادس|السادسه|шестои|шестая|шестое|шестую"},
    {kStandalone + 7, "השביעי|השביעית|السابع|السابعه|седьмои|седьмая|седьмое|седьмую"},
    {kStandalone + 8, "השמיני|השמינית|الثامن|الثامنه|восьмои|восьмая|восьмое|восьмую"},
    {kStandalone + 9, "התשיעי|התשיעית|التاسع|التاسعه|девятыи|девятая|девятое|девятую"},
    {kStandalone + 10, "העשירי|העשירית|العاشر|العاشره|десятыи|десятая|десятое|десятую"},
    {kStandalone + kOrdinalLast, "האחרון|האחרונה|الاخير|الاخيره|последнии|последняя|последнее|последнюю"},
    {kOrdinalFiller, "the|match|result|occurrence|hit|go|to|jump|show|me|take|את|התוצאה|ההתאמה|תוצאה|התאמה|המופע|עבור|תעבור|לך|תלך"
                     "|קפוץ|תקפוץ|ל|תראה|הראה|לי"
                     "|el|la|resultado|coincidencia|ve|al|muestra|muestrame|ir"
                     "|le|resultat|correspondance|va|au|montre|montre-moi|aller"
                     "|der|die|das|den|ergebnis|treffer|zum|zur|gehe|geh|zeig|zeige|mir"
                     "|результат|совпадение|к|покажи|переиди|на"
                     "|النتيجه|المطابقه|الي|اذهب|اعرض|انتقل"},
    {kOrdinalAfter, "one"},
};
constexpr auto kOrdinalWords = Expand<Total(kOrdinalSets, &Set::words)>(kOrdinalSets);
constexpr auto kOrdinalIndex = MakeIndex<256>(kOrdinalWords);
bool Ordinal(const Token* tokens, unsigned begin, unsigned end, const MediaBounds& bounds, Intent& intent) noexcept {
    std::int32_t ordinal = 0; unsigned words = 0; bool alone = false;
    for (unsigned i = begin; i < end; ++i) {
        if (tokens[i].truncated) return false;
        const auto word = tokens[i].View();
        const auto value = Lookup(kOrdinalWords, kOrdinalIndex, word);
        if (!value) { if (Class(word) & kPolite) continue; return false; }
        ++words;
        if (value == kOrdinalFiller) continue;
        if (value == kOrdinalAfter) { if (!ordinal) return false; continue; } // "the second one"; "one second" is time.
        if (ordinal) return false; // Two ordinals: no command.
        ordinal = value % kStandalone; alone = value >= kStandalone;
    }
    if (!ordinal || (words < 2 && !alone)) return false;
    intent = {}; intent.kind = IntentKind::NthMatch;
    intent.match = ordinal == kOrdinalLast ? Dispatcher::kLastMatch : static_cast<unsigned>(ordinal);
    // With no search to follow it is no command here (Interpret then looks for the words themselves).
    intent.status = !bounds.seekable ? IntentStatus::Unavailable : bounds.matches ? IntentStatus::Ok : IntentStatus::NoContext;
    return true;
}

// ---- Numbers and time spans ------------------------------------------------------------------------------------
bool Digit(char c) noexcept { return c >= '0' && c <= '9'; }
// A playback speed: "1.5", "1.5x", "x1.5", "1,5", "×2", "150%", "2倍", "1.5倍速"; with a times word before or after it
// ("x 1.5", "1.5 x", "2 times", "פי 2", "2 fois", "2 veces", "2 mal", "2 раза").
bool RateWord(std::string_view word, double& rate) noexcept {
    // Hebrew's attached prepositions: "ל-1.5", "ב1.5".
    if (word.size() > 2 && (word.substr(0, 2) == "\xd7\x9c" || word.substr(0, 2) == "\xd7\x91")) { word.remove_prefix(2); if (!word.empty() && word[0] == '-') word.remove_prefix(1); }
    for (const std::string_view mark : {std::string_view{"x"}, std::string_view{"\xc3\x97"}})
        if (word.size() > mark.size() && word.substr(0, mark.size()) == mark) { word.remove_prefix(mark.size()); break; }
    double scale = 1;
    for (const std::string_view mark : {std::string_view{"x"}, std::string_view{"\xc3\x97"}, std::string_view{"\xe5\x80\x8d\xe9\x80\x9f"},
                                         std::string_view{"\xe5\x80\x8d"}, std::string_view{"%"}})
        if (word.size() > mark.size() && word.ends_with(mark)) { word.remove_suffix(mark.size()); if (mark == "%") scale = 0.01; break; }
    std::int64_t whole = 0, fraction = 0; double divisor = 1; bool seen = false, point = false;
    for (const char c : word) {
        if ((c == '.' || c == ',') && !point && seen) { point = true; continue; }
        if (c < '0' || c > '9') return false;
        if (point) { if (divisor >= 1000) return false; fraction = fraction * 10 + (c - '0'); divisor *= 10; }
        else { if (whole > 1000) return false; whole = whole * 10 + (c - '0'); }
        seen = true;
    }
    if (!seen || (point && divisor == 1)) return false;
    rate = (static_cast<double>(whole) + static_cast<double>(fraction) / divisor) * scale;
    return true;
}
bool Number(std::string_view word, std::int64_t& value) noexcept {
    if (word.empty() || word.size() > 6) return false;
    value = 0;
    for (const char c : word) { if (!Digit(c)) return false; value = value * 10 + (c - '0'); }
    return true;
}
// Chinese and Japanese numerals: a digit, ten or a hundred; -1 for anything else.
int Numeral(std::uint32_t scalar) noexcept {
    switch (scalar) {
    case 0x96f6: case 0x3007: return 0;
    case 0x4e00: return 1;
    case 0x4e8c: case 0x4e24: case 0x5169: return 2;
    case 0x4e09: return 3;
    case 0x56db: return 4;
    case 0x4e94: return 5;
    case 0x516d: return 6;
    case 0x4e03: return 7;
    case 0x516b: return 8;
    case 0x4e5d: return 9;
    case 0x5341: return 10;
    case 0x767e: return 100;
    default: return -1;
    }
}
constexpr std::uint32_t kHalf = 0x534a; // 半
// A count at the front of `word`, in quarters: ASCII digits, numerals (三十, 两, 一百二十) or 半. Returns the bytes
// read; 0 when the word does not start with a count.
std::size_t Count(std::string_view word, std::int64_t& quarters) noexcept {
    std::size_t digits = 0;
    while (digits < word.size() && Digit(word[digits])) ++digits;
    if (digits) {
        std::int64_t value = 0;
        if (!Number(word.substr(0, digits), value)) return 0;
        quarters = value * 4; return digits;
    }
    auto rest = word; std::int64_t total = 0, current = -1; unsigned read = 0;
    while (read < 6) {
        auto next = rest; std::uint32_t scalar = 0;
        if (next.empty() || !text::Utf8(next, scalar)) break;
        if (!read && scalar == kHalf) { quarters = 2; return word.size() - next.size(); }
        const int value = Numeral(scalar);
        if (value < 0) break;
        if (value >= 10) { total += (current < 0 ? 1 : current) * value; current = -1; }
        else current = current < 0 ? value : current * 10 + value;
        rest = next; ++read;
    }
    if (!read) return 0;
    quarters = (total + (current < 0 ? 0 : current)) * 4;
    return word.size() - rest.size();
}
bool StartsCount(std::string_view word) noexcept {
    if (word.empty()) return false;
    if (Digit(word.front())) return true;
    std::uint32_t scalar = 0;
    return static_cast<unsigned char>(word.front()) >= 0xe3 && text::Utf8(word, scalar) && (scalar == kHalf || Numeral(scalar) >= 0);
}
std::size_t ScalarBytes(std::string_view word) noexcept {
    const auto lead = static_cast<unsigned char>(word.front());
    const std::size_t bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    return bytes < word.size() ? bytes : word.size();
}
// Hebrew and Arabic attach their prepositions: "ב-10", "ל1:30", "ב־20" (maqaf), "ب20". Returns the word from its
// first digit.
std::string_view Bare(std::string_view word) noexcept {
    for (const std::string_view prefix : {"ב", "ל", "כ", "ب", "ل"}) {
        if (!word.starts_with(prefix)) continue;
        auto rest = word.substr(prefix.size());
        if (rest.starts_with("-")) rest.remove_prefix(1); else if (rest.starts_with("\xd6\xbe")) rest.remove_prefix(2);
        if (!rest.empty() && Digit(rest.front())) return rest;
    }
    return word;
}
// A count in quarters: digits, a number word or numerals. `digits`: a plain number (it may stand alone as seconds).
bool Quantity(std::string_view word, std::int64_t& quarters, bool& digits) noexcept {
    digits = Number(word, quarters);
    if (digits) { quarters *= 4; return true; }
    if (const auto value = Lookup(kNumbers, kNumberIndex, word)) { quarters = value; return true; }
    if (!Cjk(word) || Count(word, quarters) != word.size()) return false;
    digits = quarters >= 4; return true;
}
// "1:30" or "1:02:03".
bool Timecode(std::string_view word, std::int64_t& ticks) noexcept {
    std::int64_t fields[3]{}; unsigned count = 0;
    while (count < 3) {
        const auto colon = word.find(':');
        if (!Number(word.substr(0, colon), fields[count])) return false;
        ++count;
        if (colon == word.npos) break;
        word.remove_prefix(colon + 1);
        if (count == 3) return false;
    }
    if (count < 2) return false;
    for (unsigned i = 1; i < count; ++i) if (fields[i] >= 60) return false;
    const auto seconds = count == 2 ? fields[0] * 60 + fields[1] : fields[0] * 3600 + fields[1] * 60 + fields[2];
    ticks = seconds * Dispatcher::kSecond; return true;
}
// Counts and units written together: "30s", "1m30s", "1h2m3s", "10秒", "1分30秒", "三十秒", "两个小时", "一分半", "10секунд".
bool Compact(std::string_view word, std::int64_t& ticks) noexcept {
    std::int64_t total = 0, last = 0; unsigned parts = 0;
    while (!word.empty()) {
        std::int64_t quarters = 0;
        const auto read = Count(word, quarters);
        if (!read) return false;
        word.remove_prefix(read);
        std::size_t length = 0;
        while (length < word.size() && !StartsCount(word.substr(length))) length += ScalarBytes(word.substr(length));
        if (const auto unit = Unit(word.substr(0, length))) { total += quarters * unit * Dispatcher::kSecond / 4; last = unit; }
        else if (!length && quarters < 4 && last) total += quarters * last * Dispatcher::kSecond / 4; // 一分半.
        else return false;
        word.remove_prefix(length); ++parts;
    }
    if (!parts) return false;
    ticks = total; return true;
}
// A time span in words: "10 seconds", "a minute and a half", "1m30s", "1:30", "דקה וחצי", "10 секунд", "media hora",
// "eine halbe stunde", "دقيقه و 30 ثانيه", "1分30秒". A bare number is seconds. False when any word is not part of a span.
bool Duration(const Token* tokens, unsigned count, std::int64_t& ticks) noexcept {
    if (!count) return false;
    for (unsigned i = 0; i < count; ++i) if (tokens[i].truncated) return false;
    if (count == 1 && Timecode(Bare(tokens[0].View()), ticks)) return true;
    std::int64_t total = 0, last_unit = 0; unsigned parts = 0;
    for (unsigned i = 0; i < count;) {
        auto word = Bare(tokens[i].View());
        if (parts && (Class(word) & kAnd)) { if (++i == count) return false; continue; }
        if (parts) for (const std::string_view conjunction : {"ו", "و"}) { // The attached Hebrew and Arabic "and".
            if (!word.starts_with(conjunction) || word.size() <= 2) continue;
            auto rest = word.substr(2);
            if (rest.starts_with("-")) rest.remove_prefix(1);
            std::int64_t q = 0, t = 0; bool digits = false;
            if (Quantity(rest, q, digits) || Unit(rest) || Compact(rest, t)) word = rest;
            break;
        }
        std::int64_t quarters = 0, part = 0; bool digits = false;
        if (Quantity(word, quarters, digits)) {
            ++i;
            const auto next = [&] { return i < count ? tokens[i].View() : std::string_view{}; };
            if (quarters == 4 && !digits) { // "a half", "una media", "eine halbe".
                std::int64_t fraction = 0; bool plain = false;
                if (Quantity(next(), fraction, plain) && !plain && fraction < 4) { quarters = fraction; ++i; }
            }
            if (quarters < 4 && (next() == "a" || next() == "an")) ++i;                                                  // "half an hour".
            if (const auto unit = Unit(next())) { total += quarters * unit * Dispatcher::kSecond / 4; last_unit = unit; ++i; }
            else if (quarters < 4 && last_unit) total += quarters * last_unit * Dispatcher::kSecond / 4;                // "... and a half".
            else if (digits && !parts && i == count) total += quarters / 4 * Dispatcher::kSecond;                        // Bare seconds.
            else return false;
        } else if (const auto unit = Unit(word)) {
            total += unit * Dispatcher::kSecond; last_unit = unit; ++i;
            if (i < count) for (const std::string_view one : {"אחת", "אחד", "واحده", "واحد"}) if (tokens[i].View() == one) { ++i; break; } // "דקה אחת".
        } else if (Compact(word, part)) { total += part; ++i; }
        else return false;
        ++parts;
    }
    ticks = total; return true;
}
bool Filler(IntentKind kind, const Token& token) noexcept {
    if (token.truncated) return false;
    const auto c = Class(token.View());
    switch (kind) {
    case IntentKind::Play: case IntentKind::Pause: return (c & kPlayFiller) != 0;
    case IntentKind::Mute: case IntentKind::Unmute: return (c & kSoundFiller) != 0;
    case IntentKind::VolumeUp: case IntentKind::VolumeDown: case IntentKind::VolumeSet: return (c & kVolumeFiller) != 0;
    case IntentKind::SeekRelative: return (c & kRelativeFiller) != 0;
    case IntentKind::SeekAbsolute: return (c & kAbsoluteFiller) != 0;
    default: return false;
    }
}
// "percent" after a number: one word, or "por ciento".
bool PercentWords(const Token* tokens, unsigned count) noexcept {
    if (count == 1) return !tokens[0].truncated && (Class(tokens[0].View()) & kPercent) != 0;
    for (const auto run : kPercentRuns) if (WordCount(run) == count && Words(run, tokens, count)) return true;
    return false;
}
std::string_view Trim(std::string_view text) noexcept {
    const auto blank = [](char c) { return c == ' ' || (c >= 9 && c <= 13); };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    return text;
}
// The text without surrounding blanks and without one pair of quotation marks around it.
std::string_view Quote(std::string_view text) noexcept {
    text = Trim(text);
    struct Pair { std::string_view open, close; };
    static constexpr Pair pairs[]{{"\"", "\""}, {"'", "'"}, {"\xe2\x80\x9c", "\xe2\x80\x9d"}, {"\xd7\xb4", "\xd7\xb4"}, {"\xc2\xab", "\xc2\xbb"},
                                  {"\xe3\x80\x8c", "\xe3\x80\x8d"}, {"\xe3\x80\x8e", "\xe3\x80\x8f"}, {"\xe2\x80\x9e", "\xe2\x80\x9c"},
                                  {"\xe2\x80\x98", "\xe2\x80\x99"}};
    for (const auto& pair : pairs)
        if (text.size() >= pair.open.size() + pair.close.size() && text.starts_with(pair.open) && text.ends_with(pair.close)) {
            text.remove_prefix(pair.open.size()); text.remove_suffix(pair.close.size()); break;
        }
    return text;
}
Intent Fail(Intent intent, IntentStatus status) noexcept { intent.status = status; return intent; }
bool Folded(std::string_view word) noexcept {
    if (word.empty() || word.size() + 4 > kTokenBytes) return false;
    while (!word.empty()) {
        std::uint32_t scalar = 0;
        if (!text::Utf8(word, scalar) || scalar == ' ' || Fold(scalar) != scalar) return false;
    }
    return true;
}
bool FoldedWords(std::string_view words) noexcept {
    for (;;) {
        const auto space = words.find(' ');
        if (!Folded(words.substr(0, space))) return false;
        if (space == words.npos) return true;
        words.remove_prefix(space + 1);
    }
}
}

Intent LocalIntentDispatcher::Parse(std::string_view command, const MediaBounds& bounds) noexcept {
    Intent intent;
    if (command.size() > kMaxCommandBytes) return Fail(intent, IntentStatus::TooLong);
    Token tokens[kMaxTokens]; unsigned count = 0; bool more = false;
    if (!Tokenize(command, tokens, count, more)) return Fail(intent, IntentStatus::InvalidText);
    // Framing in front of the command: courtesy and hesitations, request frames ("can you", "אני רוצה ש"), softeners.
    unsigned first = 0; bool framed = false;
    while (first < count) {
        auto skip = PoliteFront(tokens + first, count - first);
        if (!skip && (skip = RequestFront(tokens + first, count - first)) != 0) framed = true;
        if (!skip) skip = Front(kModestHeads, kModestHeadIndex, tokens + first, count - first);
        if (!skip) skip = Modest(tokens[first]) ? 1u : 0u;
        if (!skip) break;
        first += skip;
    }
    if (first == count) return Fail(intent, count ? IntentStatus::Unknown : IntentStatus::Empty);
    if (framed && !tokens[first].truncated)
        for (const auto& g : kGerunds)
            if (tokens[first].View() == g.gerund) { std::memcpy(tokens[first].folded, g.verb.data(), g.verb.size()); tokens[first].size = static_cast<unsigned>(g.verb.size()); break; }
    // A subordinator or conjunction attached to the verb: "שתעצור" after "אני רוצה", "ותמשיך" after "תעשה לי טובה".
    for (unsigned attached = 0; framed && attached < 2; ++attached) {
        auto& head = tokens[first];
        if (head.truncated || Head(head.View()) || !HebrewPrefix(head.View())) break;
        const auto letter = static_cast<unsigned char>(head.folded[1]);
        if ((letter != 0xa9 && letter != 0x95) || !Head(head.View().substr(2))) break;
        TrimFront(head, 2);
    }
    // Track and language commands read their words in any order.
    if (!more) {
        unsigned last = count;
        while (last > first) { const auto polite = PoliteBack(tokens, first, last); if (!polite) break; last -= polite; }
        if (Track(tokens, first, last, bounds, intent)) return intent;
        if (Ordinal(tokens, first, last, bounds, intent)) return intent;
    }
    // Attached courtesy: 请暂停, 一時停止してください.
    for (const auto prefix : kPolitePrefixes)
        if (tokens[first].size > prefix.size() && tokens[first].View().starts_with(prefix)) { TrimFront(tokens[first], static_cast<unsigned>(prefix.size())); break; }
    if (!more && !tokens[count - 1].truncated)
        for (const auto suffix : kPoliteSuffixes)
            if (tokens[count - 1].size > suffix.size() && tokens[count - 1].View().ends_with(suffix)) { tokens[count - 1].size -= static_cast<unsigned>(suffix.size()); break; }

    // The longest phrase wins: whole words ("volume up" over "volume"), or the start of a spaceless word (快退10秒).
    const Phrase* phrase = nullptr; unsigned matched = 0, split = 0; std::size_t bytes = 0;
    const auto head = tokens[first].View();
    if (!tokens[first].truncated)
        for (auto i = kPhraseIndex.head[Hash(head) % kPhraseIndex.head.size()]; i != kNone; i = kPhraseIndex.next[i]) {
            const auto& candidate = kPhrases[i];
            if (Cjk(candidate.words) || candidate.words.size() <= bytes) continue;
            if (const auto words = Words(candidate.words, tokens + first, count - first)) { phrase = &candidate; matched = words; bytes = candidate.words.size(); }
        }
    if (Cjk(head) && head.size() >= 3)
        for (auto i = kPhraseIndex.head[Hash(head.substr(0, 3)) % kPhraseIndex.head.size()]; i != kNone; i = kPhraseIndex.next[i]) {
            const auto& candidate = kPhrases[i];
            if (!Cjk(candidate.words) || candidate.words.size() <= bytes || !head.starts_with(candidate.words)) continue;
            if (candidate.words.size() == head.size() && tokens[first].truncated) continue;
            phrase = &candidate; matched = 1; bytes = candidate.words.size();
            split = candidate.words.size() < head.size() ? static_cast<unsigned>(candidate.words.size()) : 0;
        }
    if (split) { // The rest of the word becomes the first argument.
        const unsigned last = count < kMaxTokens ? count : kMaxTokens - 1;
        if (count == kMaxTokens) more = true; else ++count;
        for (unsigned i = last; i > first + 1; --i) tokens[i] = tokens[i - 1];
        tokens[first + 1] = tokens[first]; TrimFront(tokens[first + 1], split);
        tokens[first].size = split; tokens[first].truncated = false;
    }
    int sign = 0; auto argument = Argument::Span;
    unsigned begin = first + matched, end = count;
    const char* quote_end = command.data() + command.size();
    if (phrase) { intent.kind = phrase->kind; sign = phrase->sign; argument = phrase->argument; }
    else if (!more && count - first >= 2 && !tokens[count - 1].truncated && Direction(tokens[count - 1].View())) {
        // The span first: "10 שניות אחורה", "30 seconds back", "10 Sekunden zurück", "10秒 戻る".
        intent.kind = IntentKind::SeekRelative; sign = Direction(tokens[count - 1].View()); end = count - 1;
    } else {
        // The verb attached last: 10秒戻る, 1:30に移動, 言葉を検索.
        const Phrase* suffix = nullptr; auto& tail = tokens[count - 1];
        if (!more && !tail.truncated)
            for (const auto& candidate : kSuffixes)
                if (tail.size > candidate.words.size() && tail.View().ends_with(candidate.words) && (!suffix || candidate.words.size() > suffix->words.size()))
                    suffix = &candidate;
        if (!suffix) return Fail(intent, IntentStatus::Unknown);
        intent.kind = suffix->kind; sign = suffix->sign; argument = suffix->argument;
        tail.size -= static_cast<unsigned>(suffix->words.size());
        quote_end = tail.raw.data() + RawOffset(tail, tail.size);
    }

    if (argument == Argument::Quote) {
        if (begin == count) return Fail(intent, IntentStatus::Malformed);
        const auto* from = tokens[begin].raw.data();
        intent.query = Quote({from, static_cast<std::size_t>(quote_end - from)});
        if (intent.query.empty()) return Fail(intent, IntentStatus::Malformed);
        if (intent.query.size() > kMaxQueryBytes) return Fail(intent, IntentStatus::TooLong);
        if (!bounds.subtitles || !bounds.seekable) return Fail(intent, IntentStatus::Unavailable);
        intent.status = IntentStatus::Ok; return intent;
    }
    if (more) return Fail(intent, IntentStatus::Malformed);
    while (end > begin) { const auto polite = PoliteBack(tokens, begin, end); if (!polite) break; end -= polite; }
    if (argument != Argument::Percent) {
        // Softeners at either end of the arguments: "turn it up a bit", "go back a little", "תחזיר אחורה קצת".
        for (;;) {
            unsigned skip = 0; // Runs first: "a bit" is more than its last word.
            if (const auto n = Back(kModestTails, kModestTailIndex, tokens, begin, end)) { end -= n; skip = n; }
            else if (end > begin && Modest(tokens[end - 1])) { --end; skip = 1; }
            else if (const auto m = Front(kModestHeads, kModestHeadIndex, tokens + begin, end - begin)) { begin += m; skip = m; }
            else if (end > begin && Modest(tokens[begin])) { ++begin; skip = 1; }
            if (!skip) break;
        }
    }
    const auto closing = [&] { return end > begin && !tokens[end - 1].truncated ? Direction(tokens[end - 1].View()) : 0; };
    if (argument == Argument::Time && closing()) {
        // "קפוץ 30 שניות קדימה", "seek 10 seconds back": a jump verb with a direction is a relative seek.
        intent.kind = IntentKind::SeekRelative; argument = Argument::Span; sign = 2;
    }
    if (intent.kind == IntentKind::Play && phrase) {
        // "תמשיך 10 שניות קדימה", "תריץ קדימה חצי דקה", "הרץ אחורה": playing on in a direction is a relative seek.
        if (closing()) { intent.kind = IntentKind::SeekRelative; argument = Argument::Span; sign = 2; }
        else if (end > begin && !tokens[begin].truncated && Direction(tokens[begin].View())) {
            intent.kind = IntentKind::SeekRelative; argument = Argument::Span; sign = Direction(tokens[begin].View()); ++begin;
        }
    }
    if (intent.kind == IntentKind::SeekRelative && phrase) {
        // "חזור 10 שניות אחורה", "skip 10 seconds back": a closing direction must agree with the verb.
        if (const int direction = closing()) {
            if (sign == 2) sign = direction; else if (direction != sign) return Fail(intent, IntentStatus::Malformed);
            --end;
        }
    }
    if (sign == 2) sign = 1;
    while (begin < end && Filler(intent.kind, tokens[begin])) ++begin;
    while (end > begin && Filler(intent.kind, tokens[end - 1])) --end;
    const unsigned words = end - begin;
    const Token* arguments = tokens + begin;

    switch (argument) {
    case Argument::None:
        if (words) return Fail(intent, IntentStatus::Malformed);
        if (intent.kind == IntentKind::NextChapter || intent.kind == IntentKind::PreviousChapter ||
            intent.kind == IntentKind::NextScene || intent.kind == IntentKind::PreviousScene) {
            const bool chapter = intent.kind == IntentKind::NextChapter || intent.kind == IntentKind::PreviousChapter;
            if (!bounds.seekable || (chapter ? bounds.chapters : bounds.scenes) == 0) return Fail(intent, IntentStatus::Unavailable);
        }
        if ((intent.kind == IntentKind::NextFile && !bounds.files_after) || (intent.kind == IntentKind::PreviousFile && !bounds.files_before))
            return Fail(intent, IntentStatus::Unavailable); // No file that way in the folder.
        if (intent.kind == IntentKind::NthMatch) intent.match = sign >= 199 ? Dispatcher::kLastMatch : static_cast<unsigned>(sign - 100); // 第二个, 二番目.
        if (intent.kind == IntentKind::SpeedSet) intent.speed = static_cast<float>(sign) / 100; // "double speed": 200.
        if (intent.kind == IntentKind::NextMatch || intent.kind == IntentKind::PreviousMatch || intent.kind == IntentKind::NthMatch) {
            if (!bounds.seekable) return Fail(intent, IntentStatus::Unavailable);
            if (!bounds.matches) return Fail(intent, IntentStatus::NoContext); // No search to follow.
        }
        if (intent.kind == IntentKind::Undo && !bounds.cure && (!bounds.seekable || bounds.duration_100ns <= 0)) return Fail(intent, IntentStatus::Unavailable);
        if (intent.kind == IntentKind::ReplayCue && (!bounds.seekable || !bounds.subtitles)) return Fail(intent, IntentStatus::Unavailable);
        if ((intent.kind == IntentKind::LoopStart || intent.kind == IntentKind::LoopEnd) && (!bounds.seekable || bounds.duration_100ns <= 0))
            return Fail(intent, IntentStatus::Unavailable); // A loop turns back by seeking.
        if (intent.kind == IntentKind::LoopCue && (!bounds.seekable || bounds.duration_100ns <= 0 || !bounds.subtitles)) return Fail(intent, IntentStatus::Unavailable);
        if (intent.kind == IntentKind::BookmarkNext || intent.kind == IntentKind::BookmarkPrevious) {
            if (!bounds.seekable) return Fail(intent, IntentStatus::Unavailable);
            if (!bounds.bookmarks) return Fail(intent, IntentStatus::NoContext);
        }
        break;
    case Argument::Step: case Argument::Percent: {
        // "20", "20%", "20 percent", "ב-20", "20 אחוז", "20 por ciento", "50に", "50パーセント".
        std::int64_t percent = 0; bool given = false;
        if (words) {
            if (arguments[0].truncated) return Fail(intent, IntentStatus::Malformed);
            auto word = Bare(arguments[0].View());
            for (const std::string_view particle : {"にして", "にする", "に"})
                if (word.size() > particle.size() && word.ends_with(particle)) { word.remove_suffix(particle.size()); break; }
            bool sign_attached = false;
            for (const std::string_view mark : {"%", "パーセント"})
                if (word.size() > mark.size() && word.ends_with(mark)) { word.remove_suffix(mark.size()); sign_attached = true; break; }
            const bool unit = words >= 2 && !sign_attached && PercentWords(arguments + 1, words - 1);
            if (!Number(word, percent) || (words != 1 && !unit)) return Fail(intent, IntentStatus::Malformed);
            given = true;
        }
        const float current = bounds.volume >= 0 ? (bounds.volume < 2 ? bounds.volume : 2.0f) : 0.0f; // Also maps NaN to 0.
        // A step stays under 100 % unless the volume is boosted already (API 16): "volume up" never starts a boost, and never
        // pulls a boost back down to 100 % either.
        const long top = current > 1.0f + 1e-6f ? 200 : 100;
        if (argument == Argument::Percent) {
            if (!given) return Fail(intent, IntentStatus::Malformed);
            if (percent > 200) return Fail(intent, IntentStatus::OutOfRange); // "volume 150": a boost (API 16).
            intent.volume = static_cast<float>(percent) / 100;
        } else {
            if (given && (percent == 0 || percent > 100)) return Fail(intent, IntentStatus::OutOfRange);
            // Without an amount: the next 5 % grid point, as the keyboard does. With one: that many whole points.
            const long next = given ? std::lround(current * 100) + sign * static_cast<long>(percent) : (std::lround(current * 20) + sign) * 5;
            intent.volume = static_cast<float>(next > top ? top : next > 0 ? next : 0) / 100;
        }
        break;
    }
    case Argument::Span: case Argument::Time: case Argument::Start: {
        std::int64_t span = argument == Argument::Span ? kDefaultSeek : 0;
        if (argument == Argument::Start) { if (words) return Fail(intent, IntentStatus::Malformed); }
        else if (words) { if (!Duration(arguments, words, span)) return Fail(intent, IntentStatus::Malformed); }
        else if (argument == Argument::Time) return Fail(intent, IntentStatus::Malformed);
        if ((argument == Argument::Span && span == 0) || span > kMaxSeek) return Fail(intent, IntentStatus::OutOfRange);
        if (!bounds.seekable || bounds.duration_100ns <= 0) return Fail(intent, IntentStatus::Unavailable);
        if (argument == Argument::Span) {
            const auto position = bounds.position_100ns < 0 ? 0 : bounds.position_100ns < bounds.duration_100ns ? bounds.position_100ns : bounds.duration_100ns;
            intent.offset_100ns = sign * span;
            intent.target_100ns = position + intent.offset_100ns;
            if (intent.target_100ns < 0) intent.target_100ns = 0; // Back past the start is the start.
        } else intent.target_100ns = span;
        // Nothing to land on at or past the end.
        if (intent.target_100ns >= bounds.duration_100ns) return Fail(intent, IntentStatus::OutOfRange);
        break;
    }
    case Argument::Quote: break;
    case Argument::Index: {
        // "go to bookmark 2", "לך לסימנייה 3": a number from 1; none, the next one (0).
        std::int64_t index = 0;
        if (words > 1 || (words == 1 && (arguments[0].truncated || !Number(Bare(arguments[0].View()), index)))) return Fail(intent, IntentStatus::Malformed);
        if (!bounds.seekable) return Fail(intent, IntentStatus::Unavailable);
        if (!bounds.bookmarks) return Fail(intent, IntentStatus::NoContext);
        if (words && (index < 1 || index > static_cast<std::int64_t>(bounds.bookmarks))) return Fail(intent, IntentStatus::OutOfRange);
        intent.match = static_cast<unsigned>(index);
        break;
    }
    case Argument::Rate: {
        double rate = 0; bool read = false;
        const auto times = [](std::string_view w) {
            return w == "x" || w == "\xc3\x97" || w == "times" || w == "\xd7\xa4\xd7\xa2\xd7\x9e\xd7\x99\xd7\x9d" || w == "fois" || w == "veces" ||
                   w == "mal" || w == "\xd1\x80\xd0\xb0\xd0\xb7\xd0\xb0" || w == "\xd1\x80\xd0\xb0\xd0\xb7";
        };
        if (words == 1 && !arguments[0].truncated) read = RateWord(arguments[0].View(), rate);
        else if (words == 2 && !arguments[0].truncated && !arguments[1].truncated) {
            const auto before = arguments[0].View(), after = arguments[1].View();
            if (times(before) || before == "\xd7\xa4\xd7\x99") read = RateWord(after, rate); // "x 1.5", "פי 2".
            else if (times(after)) read = RateWord(before, rate);
        }
        if (!read) return Fail(intent, IntentStatus::Malformed);
        if (rate < 0.25 - 1e-9 || rate > 4.0 + 1e-9) return Fail(intent, IntentStatus::OutOfRange); // To 4x since API 16.
        intent.speed = static_cast<float>(rate);
        break;
    }
    }
    intent.status = IntentStatus::Ok; return intent;
}
Intent LocalIntentDispatcher::Interpret(std::string_view text, const MediaBounds& bounds) noexcept {
    const auto intent = Parse(text, bounds);
    if (intent.status == IntentStatus::Empty || intent.status == IntentStatus::TooLong || intent.status == IntentStatus::InvalidText) return intent;
    const auto quote = Quote(text);
    const bool quoted = quote.size() < Trim(text).size();
    if (!quoted && intent.status != IntentStatus::Unknown && intent.status != IntentStatus::Malformed && intent.status != IntentStatus::NoContext) return intent;
    if (quote.empty()) return intent;
    const bool searchable = bounds.subtitles && bounds.seekable;
    if (!searchable && !quoted) return intent; // Nothing to search: the text stays an unknown command.
    Intent search; search.kind = IntentKind::FindSubtitle; search.query = quote; search.literal = true;
    search.status = quote.size() > kMaxQueryBytes ? IntentStatus::TooLong : searchable ? IntentStatus::Ok : IntentStatus::Unavailable;
    return search;
}
namespace {
bool FollowUp(IntentKind kind) noexcept {
    return kind == IntentKind::NextMatch || kind == IntentKind::PreviousMatch || kind == IntentKind::NthMatch || kind == IntentKind::Undo || kind == IntentKind::Repeat;
}
}
Intent LocalIntentDispatcher::Run(std::string_view text, Intent intent, const MediaBounds& bounds) noexcept {
    ++stats_.commands;
    // A follow-up becomes the action it refers to before anything executes: the executor never sees one.
    if (intent.status == IntentStatus::Ok && FollowUp(intent.kind)) {
        if (conversation_) intent = conversation_->Resolve(intent, bounds);
        else intent.status = IntentStatus::NoContext;
    }
    if (intent.status != IntentStatus::Ok) { ++stats_.refused; return intent; }
    intent.status = executor_ ? executor_(context_, intent) : IntentStatus::Rejected;
    if (intent.status == IntentStatus::Ok) ++stats_.executed; else ++stats_.failed;
    if (conversation_) conversation_->Record(text, intent);
    return intent;
}
Intent LocalIntentDispatcher::Dispatch(std::string_view command, const MediaBounds& bounds) noexcept { return Run(command, Parse(command, bounds), bounds); }
Intent LocalIntentDispatcher::Submit(std::string_view text, const MediaBounds& bounds) noexcept { return Run(text, Interpret(text, bounds), bounds); }
Intent ConversationState::Resolve(const Intent& said, const MediaBounds& bounds) const noexcept {
    if (said.status != IntentStatus::Ok) return said;
    Intent r;
    r.from = said.kind;
    switch (said.kind) {
    case IntentKind::NextMatch: case IntentKind::PreviousMatch: case IntentKind::NthMatch: {
        r.kind = IntentKind::FindSubtitle;
        if (!query_bytes_) return Fail(r, IntentStatus::NoContext);
        const unsigned n = bounds.matches;
        if (!n) return Fail(r, IntentStatus::NotFound); // The quote is in no cue now (another subtitle track).
        if (said.kind == IntentKind::NthMatch && said.match != Dispatcher::kLastMatch && (said.match == 0 || said.match > n)) return Fail(r, IntentStatus::OutOfRange);
        const unsigned shown = match_ % n;
        r.match = said.kind == IntentKind::NextMatch ? (shown + 1) % n : said.kind == IntentKind::PreviousMatch ? (shown + n - 1) % n :
                  said.match == Dispatcher::kLastMatch ? n - 1 : said.match - 1;
        r.query = Query(); r.status = IntentStatus::Ok;
        return r;
    }
    case IntentKind::Undo:
        if (cure_) { r.kind = IntentKind::UndoCure; r.status = IntentStatus::Ok; return r; } // The cure is the latest.
        r.kind = IntentKind::SeekAbsolute;
        if (!jumped_) return Fail(r, IntentStatus::NoContext);
        if (!bounds.seekable || bounds.duration_100ns <= 0) return Fail(r, IntentStatus::Unavailable);
        r.target_100ns = jump_from_ < 0 ? 0 : jump_from_;
        if (r.target_100ns >= bounds.duration_100ns) return Fail(r, IntentStatus::OutOfRange);
        r.status = IntentStatus::Ok;
        return r;
    case IntentKind::Repeat:
        if (repeat_bytes_) { r = Dispatcher::Parse(Repeatable(), bounds); r.from = IntentKind::Repeat; return r; }
        r.kind = IntentKind::ReplayCue; // Nothing to repeat: the line on screen, again.
        r.status = !bounds.subtitles ? IntentStatus::NoContext : !bounds.seekable ? IntentStatus::Unavailable : IntentStatus::Ok;
        return r;
    default: return said;
    }
}
void ConversationState::Record(std::string_view text, const Intent& done) noexcept {
    if (done.status != IntentStatus::Ok) return; // A refusal changes nothing, here as in the engine.
    if (done.kind == IntentKind::CureDark || done.kind == IntentKind::CureNoise || done.kind == IntentKind::CureMuffled) cure_ = true;
    if (done.kind == IntentKind::UndoCure) cure_ = false; // One step: the next "undo" is the last jump's.
    switch (done.kind) {
    case IntentKind::FindSubtitle: {
        // The quote may already be this one (a follow-up's view into it): memmove.
        const auto size = done.query.size() < sizeof(query_) ? done.query.size() : sizeof(query_);
        if (done.query.data() != query_) std::memmove(query_, done.query.data(), size);
        query_bytes_ = static_cast<unsigned>(size); match_ = done.match; repeat_bytes_ = 0;
        return;
    }
    case IntentKind::SeekRelative: case IntentKind::VolumeUp: case IntentKind::VolumeDown:
    case IntentKind::NextChapter: case IntentKind::PreviousChapter: case IntentKind::NextScene: case IntentKind::PreviousScene:
    case IntentKind::SpeedUp: case IntentKind::SpeedDown: case IntentKind::ZoomIn: case IntentKind::ZoomOut:
    case IntentKind::BookmarkNext: case IntentKind::BookmarkPrevious:
        if (done.from == IntentKind::Repeat) return; // "again" again: the same command stays the one to repeat.
        if (text.size() <= sizeof(repeat_)) { std::memmove(repeat_, text.data(), text.size()); repeat_bytes_ = static_cast<unsigned>(text.size()); }
        else repeat_bytes_ = 0;
        return;
    default: repeat_bytes_ = 0; return; // Anything else is the last thing done, and no step to repeat.
    }
}
bool LocalIntentDispatcher::TablesConsistent(TableReport report) noexcept {
    bool good = true;
    const auto check = [&](bool ok, const char* rule, std::string_view word) { if (!ok) { good = false; if (report) report(rule, word); } };
    for (std::size_t i = 0; i < kPhrases.size(); ++i) {
        const auto words = kPhrases[i].words;
        check(FoldedWords(words) && (!Cjk(words) || words.find(' ') == words.npos), "phrase not folded", words);
        for (std::size_t k = 0; k < i; ++k) check(kPhrases[k].words != words, "phrase listed twice", words);
    }
    for (const auto& suffix : kSuffixes) check(Folded(suffix.words) && Cjk(suffix.words), "suffix not folded", suffix.words);
    const auto words = [&](const auto& table, const char* name) {
        for (std::size_t i = 0; i < table.size(); ++i) {
            check(Folded(table[i].word) && table[i].value != 0, name, table[i].word);
            for (std::size_t k = 0; k < i; ++k) check(table[k].word != table[i].word, "word listed twice", table[i].word);
        }
    };
    words(kWords, "word not folded"); words(kNumbers, "number not folded"); words(kUnits, "unit not folded"); words(kTrackWords, "track word not folded");
    words(kOrdinalWords, "ordinal not folded");
    for (const auto run : kPoliteRuns) check(FoldedWords(run), "courtesy not folded", run);
    for (const auto run : kPercentRuns) check(FoldedWords(run), "percent run not folded", run);
    for (const auto run : kRequestRuns) check(FoldedWords(run), "request frame not folded", run);
    for (const auto run : kModestRuns) check(FoldedWords(run), "softener not folded", run);
    // A request frame must neither begin a phrase nor begin with one: stripping it would change or remove a command.
    const auto prefix = [](std::string_view a, std::string_view b) { return a == b || (b.size() > a.size() && b.starts_with(a) && b[a.size()] == ' '); };
    for (const auto run : kRequestRuns)
        for (const auto& phrase : kPhrases) check(Cjk(phrase.words) || (!prefix(run, phrase.words) && !prefix(phrase.words, run)), "request frame overlaps a phrase", run);
    // A gerund stands for a verb that starts a command (or a track command's verb).
    for (const auto& g : kGerunds)
        check(Folded(g.gerund) && !Head(g.gerund) && (Head(g.verb) || TrackClass(g.verb) == kTrackFiller || TrackClass(g.verb) >= kTrackOff), "gerund", g.gerund);
    // Nor may a word set aside as courtesy or softener start one.
    for (const auto& word : kWords) check(!(word.value & (kPolite | kModest)) || !Head(word.word), "courtesy starts a phrase", word.word);
    for (const auto word : kPolitePrefixes) check(Folded(word) && Cjk(word), "courtesy prefix", word);
    for (const auto word : kPoliteSuffixes) check(Folded(word) && Cjk(word), "courtesy suffix", word);
    return good;
}
const char* Name(IntentKind kind) noexcept {
    static constexpr const char* names[]{"None", "Play", "Pause", "VolumeUp", "VolumeDown", "VolumeSet", "Mute", "Unmute", "SeekRelative", "SeekAbsolute", "FindSubtitle",
                                         "SelectAudio", "SelectSubtitles", "SelectLanguage", "NextChapter", "PreviousChapter", "NextScene", "PreviousScene",
                                         "NextMatch", "PreviousMatch", "NthMatch", "Undo", "Repeat", "ReplayCue", "CompareOn", "CompareOff",
                                         "NextFile", "PreviousFile", "SpeedUp", "SpeedDown", "SpeedSet", "ZoomIn", "ZoomOut", "ZoomReset",
                                         "LoopStart", "LoopEnd", "LoopClear", "LoopCue", "ShowStats", "HideStats", "ExplainQuality",
                                         "NightOn", "NightOff", "BookmarkAdd", "BookmarkGo", "BookmarkNext", "BookmarkPrevious", "PinFrame", "ComparePinned",
                                         "AudioZoomOn", "AudioZoomOff", "MicroMotionOn", "MicroMotionOff", "CureDark", "CureNoise", "CureMuffled", "UndoCure",
                                         "TrackTarget", "StopTracking", "AudioSaccadeOn", "AudioSaccadeOff",
                                         "RotateVideo", "RotateVideoBack", "LevelLeft", "LevelRight", "LevelHorizon", "AutoLevel", "AutoLevelOff", "Stabilize", "StabilizeOff",
                                         "CropBars", "CropBarsOff", "NextTarget", "Snapshot"};
    return static_cast<unsigned>(kind) < sizeof(names) / sizeof(names[0]) ? names[static_cast<unsigned>(kind)] : "?";
}
const char* Name(TrackChoice choice) noexcept {
    static constexpr const char* names[]{"Language", "Off", "On", "Next"};
    return static_cast<unsigned>(choice) < sizeof(names) / sizeof(names[0]) ? names[static_cast<unsigned>(choice)] : "?";
}
void CanonicalLanguage(const char* code, char out[4]) noexcept {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!code) return;
    char c[4]{}; unsigned n = 0;
    while (n < 4 && code[n]) {
        const char ch = code[n];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) return;
        c[n++] = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    if (n == 4 || n < 2) return;
    // Two-letter ISO 639-1 codes (sidecar file names), then ISO 639-2/B codes, to ISO 639-2/T.
    struct Map { const char* from; const char* to; };
    static constexpr Map two[]{{"en", "eng"}, {"he", "heb"}, {"iw", "heb"}, {"fr", "fra"}, {"es", "spa"}, {"de", "deu"}, {"ru", "rus"}, {"ar", "ara"},
                               {"zh", "zho"}, {"ja", "jpn"}, {"it", "ita"}, {"pt", "por"}, {"nl", "nld"}, {"pl", "pol"}, {"tr", "tur"}, {"ko", "kor"}};
    static constexpr Map bibliographic[]{{"fre", "fra"}, {"ger", "deu"}, {"chi", "zho"}, {"per", "fas"}, {"dut", "nld"}, {"cze", "ces"}, {"gre", "ell"},
                                         {"arm", "hye"}, {"baq", "eus"}, {"bur", "mya"}, {"geo", "kat"}, {"ice", "isl"}, {"mac", "mkd"}, {"mao", "mri"},
                                         {"may", "msa"}, {"rum", "ron"}, {"slo", "slk"}, {"tib", "bod"}, {"wel", "cym"}, {"alb", "sqi"}};
    const std::string_view given{c, n};
    for (const auto& m : n == 2 ? std::span<const Map>{two} : std::span<const Map>{bibliographic})
        if (given == m.from) { std::memcpy(out, m.to, 3); return; }
    if (n == 2 || given == "und" || given == "mul" || given == "zxx" || given == "mis") return; // Unknown two-letter code, or no language.
    std::memcpy(out, c, 3);
}
const char* LanguageName(const char* code) noexcept {
    char canonical[4]; CanonicalLanguage(code, canonical);
    for (const auto& l : kLanguages) if (std::strcmp(l.code, canonical) == 0) return l.english;
    return nullptr;
}
const char* NativeLanguageName(const char* code) noexcept {
    char canonical[4]; CanonicalLanguage(code, canonical);
    for (const auto& l : kLanguages) if (std::strcmp(l.code, canonical) == 0) return l.native;
    return nullptr;
}
const char* Name(IntentStatus status) noexcept {
    static constexpr const char* names[]{"Ok", "Empty", "TooLong", "InvalidText", "Unknown", "Malformed", "OutOfRange", "Unavailable", "NotFound", "Rejected", "NoContext"};
    return static_cast<unsigned>(status) < sizeof(names) / sizeof(names[0]) ? names[static_cast<unsigned>(status)] : "?";
}
}
