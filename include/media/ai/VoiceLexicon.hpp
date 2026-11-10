#pragma once
#include "media/ai/FormantSynthesizer.hpp"
#include <cstddef>
#include <cstring>
#include <string_view>

// What the formant synthesiser says for the voice path (docs/voice.md, "Synthesised speech", "Wake word"): the wake
// word, the Hebrew command vocabulary, the speakers, and the sentences used to train (tools/make_voice_templates.cpp)
// and to test (tests/avion_wake_tests.cpp). No Hebrew voice is installed on the reference machine; these words are
// rendered from phonemes, and also spoken by the French SAPI voices from French spellings of their sounds.
namespace media::ai::voice_lexicon {
enum class Role : std::uint8_t {
    Wake,     // The wake word.
    Command,  // A word of the recogniser's vocabulary (a command alone, or a word a spoken command needs).
    Filler,   // A word people put around a command ("please", "now"): matched and dropped.
    Other     // Said only in test sentences (room chatter): in no template.
};
struct Word {
    const char* text;      // As the grammar reads it (UTF-8).
    const char* phonemes;  // FormantSynthesizer notation; alternatives separated by ';'.
    const char* french;    // A French spelling of its sound for the French SAPI voices (nullptr: none).
    Role role;
};
inline constexpr const char* kWakeText = "avion";
inline constexpr Word kWords[]{
    // The wake word: English /ˈæv.i.ɒn/, French /a.vjɔ̃/, and as an Israeli says the brand /a.vjon/.
    {"avion", "'ae v i O n;a v j 'o~;a v j 'o n", "avion", Role::Wake},
    // Hebrew commands alone (the grammar runs each by itself) and the words of spoken Hebrew commands.
    {"\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7", "t a S t 'i k", "tachtik", Role::Command},                         // תשתיק mute
    {"\xd7\x91\xd7\x98\xd7\x9c", "b a t 'e l", "batel", Role::Command},                                              // בטל cancel
    {"\xd7\x94\xd7\xa9\xd7\xaa\xd7\xa7\xd7\x94", "h a S t a k 'a", "achtaka", Role::Command},                       // השתקה muting
    {"\xd7\xaa\xd7\x92\xd7\x91\xd7\x99\xd7\xa8", "t a g b 'i R", "tagbir", Role::Command},                         // תגביר turn up
    {"\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a", "t a m S 'i X", "tamchir", Role::Command},                         // תמשיך continue
    {"\xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a", "S e t a m S 'i X", "chétamchir", Role::Command},          // שתמשיך that you continue
    {"\xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", "S e t a ? a ts 'o R", "chétaatsor", Role::Command},       // שתעצור that you stop
    {"\xd7\x90\xd7\xa0\xd7\x99", "a n 'i", "ani", Role::Command},                                                    // אני I
    {"\xd7\xa8\xd7\x95\xd7\xa6\xd7\x94", "R o ts 'e", "rotsé", Role::Command},                                       // רוצה want
    {"\xd7\x90\xd7\xa9\xd7\x9e\xd7\x97", "e s m 'a X", "esmar", Role::Command},                                     // אשמח I'd be glad
    {"\xd7\xa2\xd7\xa9\xd7\xa8", "'e s e R", "éssèr", Role::Command},                                               // עשר ten
    {"\xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa", "S n i j 'o t", "chniyot", Role::Command},                         // שניות seconds
    {"\xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94", "k a d 'i m a", "kadima", Role::Command},                          // קדימה forward
    {"\xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94", "a X o R 'a", "arora", Role::Command},                             // אחורה back
    {"\xd7\xa0\xd7\x92\xd7\x9f", "n a g 'e n", "nagu\xc3\xa8ne", Role::Command},                                      // נגן play
    {"\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", "a ts 'o R", "atsor", Role::Command},                                       // עצור stop
    // Chapters and scenes (docs/chapters.md): פרק הבא, פרק קודם, סצנה הבאה, סצנה קודמת.
    {"\xd7\xa4\xd7\xa8\xd7\xa7", "p 'e R e k", "p\xc3\xa9r\xc3\xa8k", Role::Command},                                                  // פרק chapter
    {"\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94", "s ts 'e n a", "sts\xc3\xa9na", Role::Command},                                                 // סצנה scene
    {"\xd7\x94\xd7\x91\xd7\x90", "h a b 'a", "haba", Role::Command},                                                              // הבא next (m.)
    {"\xd7\x94\xd7\x91\xd7\x90\xd7\x94", "h a b a ? 'a", "habaa", Role::Command},                                                    // הבאה next (f.)
    {"\xd7\xa7\xd7\x95\xd7\x93\xd7\x9d", "k 'o d e m", "kod\xc3\xa8m", Role::Command},                                                    // קודם previous (m.)
    {"\xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa", "k o d 'e m e t", "kod\xc3\xa9m\xc3\xa8t", Role::Command},                                     // קודמת previous (f.)
    // Fillers.
    {"\xd7\x90\xd7\xaa", "e t", "ett", Role::Filler},                                                                // את (object marker)
    {"\xd7\x94\xd7\x95\xd7\x95\xd7\x9c\xd7\x99\xd7\x95\xd7\x9d", "h a v 'o l j u m", "avolioum", Role::Filler},      // הווליום the volume
    {"\xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94", "b e v a k a S 'a", "bévakacha", Role::Filler},                     // בבקשה please
    {"\xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95", "a X S 'a v", "arrchav", Role::Filler},                            // עכשיו now
    {"\xd7\xaa\xd7\x95\xd7\x93\xd7\x94", "t o d 'a", "toda", Role::Filler},                                          // תודה thanks
    {"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "S a l 'o m", "chalom", Role::Filler},                                      // שלום hello
    {"\xd7\x9b\xd7\x9f", "k 'e n", "kène", Role::Filler},                                                            // כן yes
    {"\xd7\x9c\xd7\x90", "l 'o", "lo", Role::Filler},                                                                // לא no
    {"\xd7\x9e\xd7\x94", "m 'a", "ma", Role::Filler},                                                                // מה what
    {"\xd7\x98\xd7\x95\xd7\x91", "t 'o v", "tov", Role::Filler},                                                     // טוב good
    {"\xd7\xa1\xd7\xa8\xd7\x98", "s 'e R e t", "sérète", Role::Filler},                                             // סרט film
    {"\xd7\xa7\xd7\xa4\xd7\x94", "k a f 'e", "kafé", Role::Filler},                                                  // קפה coffee
    // Words that begin like the wake word (none of them is in a test sentence): matched, so they are not taken for it.
    {"\xd7\x90\xd7\x95\xd7\x95\xd7\x99\xd7\xa8", "a v 'i R", "aviR", Role::Filler},                                  // אוויר air
    {"\xd7\x90\xd7\x91\xd7\x99\xd7\xa8", "a b 'i R", "abir", Role::Filler},                                        // אביר knight
    {"\xd7\x95\xd7\x99\xd7\x9c\xd7\x95\xd7\x9f", "v i l 'o n", "vilone", Role::Filler},                           // וילון curtain
    // The wake word's nearest Hebrew word, "avion" with a consonant between "avi" and "on", and in the chatter below:
    // as a filler it is the contrast that refuses it (nearer to it than to the name by the margin: no wake). The
    // template speakers say it; the held-out ones only in the test sentences.
    {"\xd7\x90\xd7\x95\xd7\x95\xd7\x99\xd7\xa8\xd7\x95\xd7\x9f", "a v i R 'o n", "aviron", Role::Filler},           // אווירון aeroplane
    // Room chatter only (in no template).
    {"\xd7\x94\xd7\xa9\xd7\xa2\xd7\x94", "h a S 'a a", "hacha'a", Role::Other},                                     // השעה the time
    {"\xd7\x90\xd7\x99\xd7\xa4\xd7\x94", "'e f o", "éfo", Role::Other},                                              // איפה where
    {"\xd7\x94\xd7\x9e\xd7\xa4\xd7\xaa\xd7\x97\xd7\x95\xd7\xaa", "h a m a f t e X 'o t", "hamafteror", Role::Other}, // המפתחות the keys
    {"\xd7\xa9\xd7\x9c\xd7\x99", "S e l 'i", "chéli", Role::Other},                                                  // שלי mine
    {"\xd7\x94\xd7\xa1\xd7\xa8\xd7\x98", "h a s 'e R e t", "hassérète", Role::Other},                               // הסרט the film
    {"\xd7\x94\xd7\x96\xd7\x94", "h a z 'e", "hazé", Role::Other},                                                   // הזה this
    {"\xd7\xa8\xd7\x91\xd7\x94", "R a b 'a", "raba", Role::Other},                                                   // רבה much
    {"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9e\xd7\x9a", "S l o m X 'a", "chlomra", Role::Other},                          // שלומך how are you
    {"\xd7\x90\xd7\x91\xd7\x99\xd7\x91", "a v 'i v", "aviv", Role::Other},                                           // אביב spring
    {"\xd7\x90\xd7\x91\xd7\x90", "'a b a", "aba", Role::Other},                                                      // אבא dad
    {"\xd7\x91\xd7\x90", "b 'a", "ba", Role::Other},                                                                 // בא came
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
// A sentence: words of kWords separated by spaces, and the command the grammar must read from what is recognised
// (nullptr: none).
struct Sentence { const char* text; const char* means; };
// Held out: never said by a template speaker.
inline constexpr Sentence kTestSentences[]{
    {"\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", "\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8"}, // אני רוצה שתעצור
    {"\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94", "\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a 10 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94"}, // תמשיך עשר שניות קדימה
    {"\xd7\xaa\xd7\x92\xd7\x91\xd7\x99\xd7\xa8 \xd7\x90\xd7\xaa \xd7\x94\xd7\x95\xd7\x95\xd7\x9c\xd7\x99\xd7\x95\xd7\x9d", "\xd7\xaa\xd7\x92\xd7\x91\xd7\x99\xd7\xa8"}, // תגביר את הווליום
    {"\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7", "\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7"},                                                                             // תשתיק
    {"\xd7\x91\xd7\x98\xd7\x9c \xd7\x94\xd7\xa9\xd7\xaa\xd7\xa7\xd7\x94", "\xd7\x91\xd7\x98\xd7\x9c \xd7\x94\xd7\xa9\xd7\xaa\xd7\xa7\xd7\x94"},                           // בטל השתקה
    {"\xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94", "\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a 10 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94"}, // אשמח שתמשיך עשר שניות קדימה
    {"\xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94", "\xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94"}, // עשר שניות אחורה
    {"\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a", "\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a"}, // אני רוצה שתמשיך
    {"\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8 \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94", "\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8"},                                                   // עצור בבקשה
    {"\xd7\xa0\xd7\x92\xd7\x9f", "\xd7\xa0\xd7\x92\xd7\x9f"},                                                                                                         // נגן
    {"\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90", "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90"},                                     // פרק הבא
    {"\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa", "\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa"},                       // סצנה קודמת
    {"\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9d", "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9d"},                                     // פרק קודם
    {"\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94", "\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94"},                       // סצנה הבאה
};
// Training (template speakers only), worded unlike the test sentences.
inline constexpr const char* kTrainingSentences[]{
    "\xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8",                                                                                    // אשמח שתעצור
    "\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94", // אני רוצה שתמשיך עשר שניות אחורה
    "\xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94",                                                          // עשר שניות קדימה
    "\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94",                                                                                    // תמשיך קדימה
    "\xd7\xaa\xd7\x92\xd7\x91\xd7\x99\xd7\xa8 \xd7\x90\xd7\xaa \xd7\x94\xd7\x95\xd7\x95\xd7\x9c\xd7\x99\xd7\x95\xd7\x9d \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94",           // תגביר את הווליום בבקשה
    "\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95",                                                                                    // תמשיך עכשיו
    "\xd7\x91\xd7\x98\xd7\x9c \xd7\x94\xd7\xa9\xd7\xaa\xd7\xa7\xd7\x94 \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94",                                                          // בטל השתקה בבקשה
    "\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7 \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94",                                                                                    // תשתיק בבקשה
    "\xd7\xa0\xd7\x92\xd7\x9f \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95",                                                                                                    // נגן עכשיו
    "\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8 \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95",                                                                                            // עצור עכשיו
    "\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8 \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95",                 // אני רוצה שתעצור עכשיו
    "\xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a",                                                                                    // אשמח שתמשיך
    "\xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94 \xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa",                                                          // קדימה עשר שניות
    "\xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94 \xd7\xa2\xd7\xa9\xd7\xa8 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa",                                                          // אחורה עשר שניות
    "avion \xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8",                                                                              // avion אשמח שתעצור
    "avion \xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94",                                                                              // avion תמשיך קדימה
    "avion \xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7 \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94",                                                                              // avion תשתיק בבקשה
    "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90 \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94", // פרק הבא בבקשה
    "\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94 \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95", // סצנה הבאה עכשיו
    "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9d \xd7\x91\xd7\x91\xd7\xa7\xd7\xa9\xd7\x94", // פרק קודם בבקשה
    "\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95", // סצנה קודמת עכשיו
    "avion \xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90", // avion פרק הבא
    "avion \xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa", // avion סצנה קודמת
};
// Room chatter: no wake word, no command (some begin like "avion": אווירון, אביב, אבא).
inline constexpr const char* kChatter[]{
    "\xd7\x9e\xd7\x94 \xd7\x94\xd7\xa9\xd7\xa2\xd7\x94 \xd7\xa2\xd7\x9b\xd7\xa9\xd7\x99\xd7\x95",                                                                          // מה השעה עכשיו
    "\xd7\x90\xd7\x99\xd7\xa4\xd7\x94 \xd7\x94\xd7\x9e\xd7\xa4\xd7\xaa\xd7\x97\xd7\x95\xd7\xaa \xd7\xa9\xd7\x9c\xd7\x99",                                                  // איפה המפתחות שלי
    "\xd7\x94\xd7\xa1\xd7\xa8\xd7\x98 \xd7\x94\xd7\x96\xd7\x94 \xd7\x98\xd7\x95\xd7\x91",                                                                                  // הסרט הזה טוב
    "\xd7\x90\xd7\xa0\xd7\x99 \xd7\xa8\xd7\x95\xd7\xa6\xd7\x94 \xd7\xa7\xd7\xa4\xd7\x94",                                                                                  // אני רוצה קפה
    "\xd7\xaa\xd7\x95\xd7\x93\xd7\x94 \xd7\xa8\xd7\x91\xd7\x94",                                                                                                            // תודה רבה
    "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x9e\xd7\x94 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9e\xd7\x9a",                                                                          // שלום מה שלומך
    "\xd7\x90\xd7\x95\xd7\x95\xd7\x99\xd7\xa8\xd7\x95\xd7\x9f",                                                                                                              // אווירון
    "\xd7\x90\xd7\x91\xd7\x99\xd7\x91",                                                                                                                                      // אביב
    "\xd7\x90\xd7\x91\xd7\x90 \xd7\x91\xd7\x90",                                                                                                                            // אבא בא
    "\xd7\x94\xd7\xa1\xd7\xa8\xd7\x98 \xd7\x94\xd7\x91\xd7\x90 \xd7\x98\xd7\x95\xd7\x91",                                                                          // הסרט הבא טוב: "next" without a chapter or scene
};
// Synthetic speakers. Template speakers' renditions become templates; held-out speakers only test (their pitch, tract
// length, voice quality and rooms are none of the template speakers'). Two template speakers and two held-out ones
// are in reverberant rooms.
struct Speaker { const char* name; SynthVoice voice; };
inline constexpr Speaker kTemplateSpeakers[]{
    {"Synth A", {.f0 = 110, .formant_scale = 1.00f, .breathiness = 0.05f, .open_quotient = 0.60f, .tilt = 0.40f, .reverb_seconds = 0.35f, .seed = 11}},
    {"Synth B", {.f0 = 210, .formant_scale = 1.13f, .breathiness = 0.12f, .open_quotient = 0.65f, .tilt = 0.30f, .seed = 23}},
    {"Synth C", {.f0 = 92, .formant_scale = 0.94f, .breathiness = 0.04f, .open_quotient = 0.55f, .tilt = 0.55f, .reverb_seconds = 0.6f, .reverb_mix = 0.3f, .seed = 37}},
    {"Synth D", {.f0 = 240, .formant_scale = 1.20f, .breathiness = 0.15f, .open_quotient = 0.70f, .tilt = 0.25f, .seed = 41}},
};
inline constexpr Speaker kHeldOutSpeakers[]{
    {"Synth H1", {.f0 = 125, .formant_scale = 1.04f, .breathiness = 0.08f, .open_quotient = 0.58f, .tilt = 0.45f, .reverb_seconds = 0.3f, .seed = 101}},
    {"Synth H2", {.f0 = 190, .formant_scale = 1.09f, .breathiness = 0.10f, .open_quotient = 0.68f, .tilt = 0.35f, .seed = 113}},
    {"Synth H3", {.f0 = 100, .formant_scale = 0.97f, .breathiness = 0.06f, .open_quotient = 0.62f, .tilt = 0.50f, .reverb_seconds = 0.5f, .reverb_mix = 0.3f, .seed = 127}},
    {"Synth H4", {.f0 = 270, .formant_scale = 1.24f, .breathiness = 0.14f, .open_quotient = 0.66f, .tilt = 0.20f, .seed = 131}},
};
inline constexpr float kTemplateRates[]{0.88f, 1.0f, 1.15f};
inline constexpr float kHeldOutRates[]{0.85f, 1.0f, 1.2f};

[[nodiscard]] inline const Word* Find(std::string_view text) noexcept {
    for (const auto& w : kWords) if (text == w.text) return &w;
    return nullptr;
}
// The `alternative`-th pronunciation of a word (0 when it has fewer): its phonemes, written to `out`.
[[nodiscard]] inline std::size_t Pronunciation(const Word& word, unsigned alternative, char* out, std::size_t capacity) noexcept {
    std::string_view all{word.phonemes};
    unsigned count = 1; for (char c : all) count += c == ';' ? 1u : 0u;
    alternative = alternative < count ? alternative : 0;
    std::size_t begin = 0;
    for (unsigned i = 0; i < alternative; ++i) begin = all.find(';', begin) + 1;
    const auto end = all.find(';', begin);
    const auto piece = all.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    if (piece.size() + 1 > capacity) return 0;
    std::memcpy(out, piece.data(), piece.size()); out[piece.size()] = 0;
    return piece.size();
}
[[nodiscard]] inline unsigned Pronunciations(const Word& word) noexcept {
    unsigned count = 1; for (const char* c = word.phonemes; *c; ++c) count += *c == ';' ? 1u : 0u;
    return count;
}
// A sentence's phonemes: each word's (`wake_alternative` picks the wake word's pronunciation), joined by " | ".
// Returns the bytes written, 0 when a word is not in kWords or `out` is too small.
[[nodiscard]] inline std::size_t SentencePhonemes(std::string_view sentence, unsigned wake_alternative, char* out, std::size_t capacity) noexcept {
    std::size_t size = 0;
    while (!sentence.empty()) {
        const auto space = sentence.find(' ');
        const auto token = sentence.substr(0, space);
        sentence = space == std::string_view::npos ? std::string_view{} : sentence.substr(space + 1);
        if (token.empty()) continue;
        const auto* word = Find(token);
        if (!word) return 0;
        if (size) { if (size + 3 >= capacity) return 0; std::memcpy(out + size, " | ", 3); size += 3; }
        const auto n = Pronunciation(*word, word->role == Role::Wake ? wake_alternative : 0, out + size, capacity - size);
        if (!n) return 0;
        size += n;
    }
    return size;
}
// The same sentence in its words' French spellings (for a French SAPI voice); 0 when a word has none.
[[nodiscard]] inline std::size_t SentenceFrench(std::string_view sentence, char* out, std::size_t capacity) noexcept {
    std::size_t size = 0;
    while (!sentence.empty()) {
        const auto space = sentence.find(' ');
        const auto token = sentence.substr(0, space);
        sentence = space == std::string_view::npos ? std::string_view{} : sentence.substr(space + 1);
        if (token.empty()) continue;
        const auto* word = Find(token);
        if (!word || !word->french) return 0;
        const auto n = std::strlen(word->french);
        if (size + n + 2 > capacity) return 0;
        if (size) out[size++] = ' ';
        std::memcpy(out + size, word->french, n); size += n;
    }
    if (size >= capacity) return 0;
    out[size] = 0;
    return size;
}
}
