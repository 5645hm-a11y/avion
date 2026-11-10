# Local commands (`ai::LocalIntentDispatcher`)

A command as a person would say or type it becomes one typed engine action. Nine languages are understood: English, Hebrew, Arabic, Chinese (simplified and traditional), Japanese, Spanish, French, German and Russian. This is an offline grammar: no model, no network, no microphone. Whatever produces the text (the window's [command bar](command-palette.md), speech recognition, a language model) hands it to the same entry points.

```cpp
ai::Intent PlaybackCoordinator::Command(std::string_view utf8) noexcept;   // parse, validate, execute
ai::Intent PlaybackCoordinator::Submit(std::string_view utf8) noexcept;    // a command, or else a quote to look for
ai::MediaBounds PlaybackCoordinator::Bounds() const noexcept;               // what a command is checked against now
ai::IntentStatus PlaybackCoordinator::Execute(const ai::Intent&) noexcept;  // apply an already typed intent
```

## Separation

- **Parsing** (`LocalIntentDispatcher::Parse`) is a pure function of the text and a `MediaBounds` snapshot (duration, position, volume, mute, pause, seekable, subtitles). It uses fixed stack storage and constant tables, no allocation, no locale and no clock, and it never touches the engine. The same text and bounds always give the same intent.
- **The intent** is final. A seek carries an absolute target already inside the media; a volume command carries the resulting level. The executor interprets nothing.
- **Execution** (`Execute`) maps each kind onto one existing engine call: `RequestPause`, `SetVolume`, `SetMuted`, `RequestSeek`, `SeekToSubtitle`. Only an intent whose status is `Ok` reaches it.

`media_ai` depends on `media_text` and nothing else: no engine, no Windows header. It builds and is tested under GCC.

## How a command is read

1. **Words.** The text is split at white space of any kind. Invisible format characters (bidi marks, zero-width characters) are dropped.
2. **Folding.** Each word is folded to the spelling the tables use:

   | Ignored | Examples |
   |---|---|
   | Letter case | `PAUSE`, `Пауза` |
   | Latin accents and combining marks | `zurück` = `zuruck`, `más` = `mas`, `arrête` = `arrete`, `u` + U+0308 = `ü` |
   | Cyrillic ё/е and й/и | `вперёд` = `вперед`, `найти` = `наити` |
   | Hebrew points, Arabic harakat and tatweel | `נַגֵּן`, `شَغِّل` |
   | Arabic letter variants | alef with hamza or madda = alef; ة = ه; ى and Farsi ی = ي; ک = ك |
   | Fullwidth forms | `ＰＡＵＳＥ`, `１０％`, `1：30` |
   | Digits of other scripts | Arabic-Indic `٣٠`, Eastern Arabic `۳۰`, the Arabic percent sign `٪` |
   | Punctuation around a word | `pause.`, `¡Reproducir!`, `暂停。`, `„pause“` |

3. **Courtesy** at either end is dropped: `please`, `now`, `בבקשה`, `por favor`, `s'il vous plaît`, `bitte`, `пожалуйста`, `من فضلك`, `请…`, `…ください`, `お願いします`, and the hesitations and discourse words of speech (`um`, `ok so`, `well`, `אז`, `נו`, `יאללה`, `רגע`, `bueno`, `alors`, `ну`).
4. **The request frame**, if any, is set aside (below).
5. **The verb.** The longest phrase of the table that starts the command wins (`volume up` over `volume`, `mettre en pause` over nothing at all).
6. **The argument**, if the verb takes one: an amount of volume, a time span, a time, a track, or a quote. Softeners in it change nothing (`a bit`, `a little`, `slightly`, `קצת`, `עוד טיפה`, `un poco`, `un peu`, `ein bisschen`, `чуть-чуть`, `شوي`).

## Conversation: a command inside a sentence

People rarely say a bare imperative. A request frame in front of the verb is dropped, and what follows is read as the command:

| Said | Read as |
|---|---|
| `Can you please pause the movie` | `pause` |
| `I'd like you to skip forward 30 seconds` | `skip forward 30 seconds` |
| `Turn up the volume a bit` | `turn up` (one step) |
| `um, could you mute the sound please` | `mute` |
| `please go back a bit` | `go back` (10 s) |
| `אני רוצה שתעצור עכשיו` | `תעצור` |
| `אשמח שתמשיך 10 שניות קדימה` | `תמשיך 10 שניות קדימה`: a relative seek, not Play |
| `תוכל להגביר קצת את הווליום` | `להגביר` |
| `תחזיר דקה אחורה בבקשה` | a minute back |
| `תעשה לי טובה ותשתיק` | `תשתיק` |
| `¿puedes pausar el video?`, `peux-tu mettre en pause s'il te plaît`, `можешь поставить на паузу пожалуйста`, `ممكن توقف من فضلك` | pause |
| `kannst du bitte lauter machen` | louder |

- **Frames** (a table of runs, indexed by first word): `can you`, `could you please`, `would you mind`, `I want you to`, `I'd like to`, `let's`, `go ahead and`, `do me a favour and`, `give me`; `אני רוצה (ש)`, `הייתי רוצה`, `אשמח (אם/ש)`, `אני מבקש`, `תוכל (בבקשה)`, `אתה יכול`, `אפשר`, `בוא`, `תעשה לי טובה`, `תן לי`; and the like in Spanish, French, German, Russian and Arabic.
- **Hebrew future and infinitive forms**: `תעצור`, `לעצור`, `תמשיך`, `להמשיך`, `להגביר`, `תשתיק`. An attached `ש` (that) or `ו` (and) is removed only after a frame: `אני רוצה שתעצור` pauses, `שתעצור` alone is not a command.
- **English gerunds** follow a frame only (`would you mind pausing the video`, `would you mind turning up the volume`): text that merely starts with `pausing` or `playing` is no command, and the command bar searches for it as before.
- **Filler words** around the argument: `the movie`, `the video`, `it`, `את הסרט`, `לי`.
- **Play with a direction is a seek**: `אשמח שתמשיך 10 שניות קדימה`, `תמשיך קצת קדימה`.

The frames, courtesy, hesitations and softeners are closed tables like the verbs. Framing never makes a command of what is none, nor completes a broken one: `can you make coffee`, `אני רוצה פיצה`, `would you mind` and `a little` stay `Unknown`; `can you pause 5`, `i'd like you to rewind banana` and `אשמח שתעצור 5` are `Malformed`; `would you mind skipping 2 hours` is `OutOfRange`.

## Tracks

Audio and subtitle choices ([tracks](tracks.md)) are read from a bag of words, in any order, each from a table: a track noun (`audio`, `dub`, `language`, `soundtrack`, `אודיו`, `שפה`, `דיבוב`, `idioma`, `langue`, `Sprache`, `озвучка`, `دبلجة`; `subtitles`, `captions`, `subs`, `כתוביות`, `תרגום`, `subtítulos`, `sous-titres`, `Untertitel`, `субтитры`, `ترجمة`), a language (11, each in the nine languages: `english`, `אנגלית`, `inglés`, `anglais`, `Englisch`, `английский`, `الإنجليزية`, …), a choice (`off`, `none`, `no`, `בלי`, `בטל`, `ohne`, `без`; `on`, `show`, `הצג`, `включи`; `next`, `other`, `הבאה`), and verbs and connecting words that change nothing (`switch`, `change`, `use`, `put`, `to`, `in`, `תעביר`, `ל`, `ב`).

| Said | Intent |
|---|---|
| `switch subtitles to hebrew`, `כתוביות בעברית`, `i want english subtitles please`, `sous-titres en français`, `Untertitel auf Deutsch`, `субтитры на русском`, `الترجمة بالعربية` | `SelectSubtitles`, language |
| `תעביר לאודיו באנגלית`, `audio in french please`, `can you switch the audio to english`, `would you mind switching to the hebrew dub`, `אשמח שתעביר לדיבוב בעברית`, `idioma inglés` | `SelectAudio`, language |
| `turn off the subtitles`, `no subtitles`, `בלי כתוביות`, `בטל כתוביות` | `SelectSubtitles`, `Off` |
| `show subtitles`, `subtitles`, `הצג כתוביות` | `SelectSubtitles`, `On` (the last shown) |
| `next audio track`, `רצועת אודיו הבאה`, `other subtitles` | `Next` (subtitles cycle through off) |
| `switch to french`, `בעברית בבקשה` | `SelectLanguage`: that language's audio, else its subtitles |

A bare language name is no track command: `עברית` or `English` alone stays a quote to search for, since a film's subtitles may well contain the word. A track noun, a request frame or an attached Hebrew preposition (`בעברית`, `לאנגלית`) is needed. Languages are returned as ISO 639-2/T codes (`Intent::language`, `CanonicalLanguage`), and `MediaBounds::audio_tracks` and `subtitle_tracks` let `Parse` refuse a track command for a file that has none (`Unavailable`). Which tracks exist, and the cue each choice shows, is the engine's business ([tracks](tracks.md)).

## Chapters and scenes (2026-10-16)

Four commands move between the media's chapters (or synthesized sections) and scenes ([chapters](chapters.md)). They take no argument; the executor finds the mark from the position (`PlaybackCoordinator::Navigate`) and shows a cue: `Chapter 3 · Finale · 02:15`, `Scene 7 · 1:02:03`, or `No next chapter`.

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `NextChapter` | next chapter, skip chapter, go to the next chapter, next section | פרק הבא, הפרק הבא, (ת)עבור לפרק הבא, דלג לפרק הבא | الفصل التالي; siguiente capítulo; chapitre suivant; nächstes Kapitel; следующая глава; 下一章; 次の章 |
| `PreviousChapter` | previous chapter, back a chapter, go to the previous chapter | פרק קודם, הפרק הקודם, (ת)חזור לפרק הקודם | الفصل السابق; capítulo anterior; chapitre précédent; vorheriges Kapitel; предыдущая глава; 上一章; 前の章 |
| `NextScene` | next scene, skip scene, go to the next scene | סצנה הבאה, הסצנה הבאה, עבור לסצנה הבאה | المشهد التالي; siguiente escena; scène suivante; nächste Szene; следующая сцена; 下一个场景; 次のシーン |
| `PreviousScene` | previous scene, back a scene, go to the previous scene | סצנה קודמת, הסצנה הקודמת, חזור לסצנה הקודמת | المشهد السابق; escena anterior; scène précédente; vorherige Szene; предыдущая сцена; 上一个场景; 前のシーン |

- Validation: `Unavailable` when the media is not seekable, or has no chapters (`MediaBounds::chapters`) or no scenes (`scenes`); an extra word is `Malformed` (`next chapter five`). The executor answers `NotFound` when there is nothing that way.
- The longest phrase wins as everywhere, so `go to the next chapter` is not a `go to` time seek and `skip chapter` is not a relative skip. The track grammar reads them first and lets them through: `chapter` and `scene` are no track words.
- Page Down and Page Up do the same for chapters ([dock](kinetic-hud.md)).
- Spoken too since 2026-10-16, in English and Hebrew ([voice](voice.md#chapters-and-scenes-2026-10-16)): "Avion, next chapter", "אביון, סצנה קודמת".

**Languages written without spaces.** Chinese and Japanese attach the verb to its argument, so their phrases also match at the start of a word (`快退10秒`, `查找你好`, `音量50`) and, for Japanese, at its end (`10秒戻る`, `1:30に移動`, `言葉を検索`).

**Lookup cost.** The tables (765 verb phrases and 373 other words: units, number words, connecting words) are expanded from their lists and hashed at compile time. A lookup reads one hash bucket, so the grammar's size does not show in the parse time.

## Files (2026-10-20)

Two commands play the next or previous file of the folder ([sessions](session.md)): they take no argument, and the engine ends the file's `Run` for the session to open the other one (`PlaybackCoordinator::RequestFile`). **Ctrl+Right** and **Ctrl+Left** do the same.

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `NextFile` | next video, next file, next episode, next movie, play/go to/skip to the next video | הסרטון הבא, (ת)עבור לסרטון הבא, נגן את הסרטון הבא, הסרט הבא, הקובץ הבא, הפרק הבא בסדרה | الفيديو التالي; siguiente vídeo; vidéo suivante; nächstes Video; следующее видео; 下一个视频, 下一集; 次の動画 |
| `PreviousFile` | previous video, previous file, previous episode, go back to the previous video | הסרטון הקודם, (ת)חזור לסרטון הקודם, הסרט הקודם, הקובץ הקודם, הפרק הקודם בסדרה | الفيديو السابق; vídeo anterior; vidéo précédente; vorheriges Video; предыдущее видео; 上一个视频, 上一集; 前の動画 |

- Validation: `Unavailable` when the folder has no file that way (`MediaBounds::files_before` / `files_after`, which the session fills; 0 for a file played alone). An extra word is `Malformed`.
- `הפרק הבא` stays the next **chapter**: an episode is `הפרק הבא בסדרה`. `next one` stays the next match, and `find the next video` a quote.
- In conversation as everything else: "תעבור לסרטון הבא בבקשה", "can you go to the next video".

## Speed and zoom (2026-10-20)

Playback speed ([audio](audio-dsp.md#playback-speed-2026-10-20)) and zoom ([picture](picture.md#zoom-2026-10-20)), in the nine languages.

Examples, all among the 61 the tests accept (more phrasings are in the tables):

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `SpeedUp` (the next step of 0.25…2×) | faster, speed up, can you play faster please | מהר יותר, תאיץ בבקשה | اسرع; más rápido; plus vite; быстрее; 快一点; 速く |
| `SpeedDown` | slower, slow down a bit | לאט יותר, תאט קצת | أبطأ; langsamer; 慢一点 |
| `SpeedSet` with a rate | speed 1.5x, speed x2, speed 2 times, play at 1.25x, set the speed to 0.75, playback speed 50%, speed 0,25 | מהירות 1.5, תקבע מהירות ל-1.25, מהירות פי 2 | السرعة 1.5; velocidad 1.5; скорость 1.5 |
| `SpeedSet` fixed | normal speed (1×), double speed (2×), half speed (0.5×) | מהירות רגילה, מהירות כפולה, חצי מהירות | vitesse normale; doppelte Geschwindigkeit; 两倍速; 通常速度; 倍速 |
| `ZoomIn` (×2) | zoom in, can you zoom in please | זום פנימה, תגדיל את התמונה | تكبير; ampliar; vergrößern; 放大 |
| `ZoomOut` (÷2) | zoom out | זום החוצה | dézoomer; уменьшить; 縮小 |
| `ZoomReset` | reset zoom | אפס זום, בטל זום | ズームリセット |

**A rate** (`Argument::Rate`) is a number with up to three decimals, written with `.` or `,`, optionally:
- with `x` or `×` before or after it (`1.5x`, `x1.5`);
- with `%` after it (`150%`);
- as `N times` in the European languages (`2 times`, `2 fois`, `2 veces`, `2 mal`, `2 раза`), or `פי N`;
- with `倍` / `倍速` after it;
- with Hebrew `ל`/`ב` attached (`ל-1.5`).

Outside 0.25–2 it is `OutOfRange` (`speed 3x`). `speed fast`, `speed .5`, `speed 1.2345` and `faster 2` are `Malformed`. A sentence's full stop is not a decimal point (`speed 1.` is 1×).

`SpeedUp`, `SpeedDown`, `ZoomIn` and `ZoomOut` can be repeated with "again". `בטל` alone stays "undo", and the volume's longer phrases stay the volume (`увеличить громкость` is `VolumeUp`, not a zoom).

The keys do the same: `]` and `[` step the speed, Ctrl + wheel zooms, and the arrows pan while zoomed. In music mode the zoom commands are refused (`Rejected`, with a cue).

## Loop and telemetry (2026-10-21)

The A–B loop ([seeking](seeking.md#ab-loop-2026-10-21)) and the telemetry readout ([session](session.md#flight-recorder-telemetry-readout-and-watchdog-2026-10-21)), in the nine languages. Each phrase is whole; anything left over is `Malformed` (`set loop start the movie`).

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `LoopStart` | set loop start, loop start, loop from here, set point a | תחילת לופ, קבע תחילת לופ, לופ מכאן, נקודה א | بداية التكرار; inicio del bucle, punto a; début de boucle; Schleifenanfang, Punkt A; начало петли; 循环起点; ループ開始 |
| `LoopEnd` | set loop end, loop end, loop until here, set point b | סוף לופ, תקבע סוף לופ, לופ עד כאן, נקודה ב | نهاية التكرار; fin del bucle, punto b; fin de boucle; Schleifenende; конец петли; 循环终点; ループ終了 |
| `LoopClear` | stop the loop, clear loop, loop off, cancel the loop | בטל לופ, תבטל את הלופ, תפסיק לופ, בלי לופ | إلغاء التكرار; quitar el bucle; arrêter la boucle; Schleife aus; убери петлю; 取消循环; ループ解除 |
| `LoopCue` | loop this sentence, repeat this line, loop this line, keep repeating this line | לופ על המשפט, חזור על המשפט בלופ, המשפט בלופ | repetir esta frase en bucle; boucle sur cette phrase; diesen Satz in Schleife; зацикли фразу; 循环这句话; このセリフをループ |
| `ShowStats` | show stats, status report, statistics, telemetry, stats for nerds | הצג נתונים, תציג נתונים, דוח מצב, סטטיסטיקה | اعرض الإحصائيات; mostrar estadísticas; afficher les statistiques; Statistiken anzeigen; показать статистику; 显示统计; 統計を表示 |
| `HideStats` | hide stats, close stats, stats off | הסתר נתונים, תסגור נתונים | ocultar estadísticas; masquer les statistiques; скрыть статистику; 隐藏统计 |
| `ExplainQuality` | why did quality change?, why did the quality drop, why is the picture blurry, explain quality | למה האיכות ירדה?, למה האיכות השתנתה, למה התמונה מטושטשת, הסבר איכות | لماذا انخفضت الجودة; ¿por qué bajó la calidad?; pourquoi la qualité a baissé; warum ist die Qualität gesunken; почему упало качество; 为什么画质下降; なぜ画質が下がった |

- `LoopStart` and `LoopEnd` need a seekable media with a duration; `LoopCue` also needs subtitles (`Unavailable` otherwise). The telemetry intents need nothing.
- The loop's outcomes: `NoContext` for `LoopEnd` with no A, `OutOfRange` for a loop under 200 ms, `NotFound` for `LoopCue` with no line on screen.
- The replay keeps its meaning: `repeat the line` and `חזור על המשפט` are still `ReplayCue`, `again` is still `Repeat`. The longer phrase wins, so `חזור על המשפט בלופ` is the loop. `בטל` alone is still "undo".
- The keys: **L** for the loop's three steps, **I** for the panel.


## Follow-ups (2026-10-19)

People refer back: "next match", "the second one", "go back to where I was", "again". These commands mean nothing alone; `ai::ConversationState` turns each into the action it refers to before anything executes. The intent stays final: the executor receives a `FindSubtitle` with its match, a `SeekAbsolute`, or the last command again, and never interprets anything.

| Said | Intent | Resolved by the conversation into |
|---|---|---|
| `next match`, `next result`, `the next one`, `another match`, `search again`, `התוצאה הבאה`, `ההתאמה הבאה`, `את הבא(ה)`, `חפש שוב` | `NextMatch` | `FindSubtitle` of the last quote, the match after the one shown (after the last, the first) |
| `previous match`, `the previous one`, `the one before`, `התוצאה הקודמת`, `את הקודם(ת)` | `PreviousMatch` | the match before (before the first, the last) |
| `the second one`, `second match`, `the 3rd match`, `go to the last one`, `can you show me the second one`, `את השני`, `השנייה`, `התוצאה השלישית`, `האחרונה` | `NthMatch` (first to tenth, last) | that match; past the last, `OutOfRange` |
| `undo`, `undo that`, `undo the seek`, `go back to where I was`, `where was I`, `בטל`, `תבטל את הקפיצה`, `תחזור לאיפה שהייתי`, `חזור למקום הקודם`, `אני רוצה לבטל` | `Undo` | `SeekAbsolute` to where playback was before the last jump; undo again comes back |
| `again`, `once more`, `one more time`, `do it again`, `repeat that`, `עוד פעם`, `שוב`, `שוב פעם`, `תעשה את זה שוב`, `תחזור על זה` | `Repeat` | the last relative seek, volume step, chapter or scene command, read again from the text as said, against the media now (so "back 10 seconds" and "again" is 10 more seconds back); with none, `ReplayCue` |
| `what did he say`, `replay the line`, `say that again`, `מה הוא אמר`, `מה היא אמרה`, `תחזור על המשפט` | `ReplayCue` | (executed) the cue on screen, or one that ended under 3 s ago, from its start, with the cue on the landing picture |
| `compare quality`, `split view`, `split screen`, `before and after`, `השווה איכות`, `מסך מפוצל`, `לפני ואחרי` / `stop comparing`, `comparison off`, `בטל השוואה`, `סגור השוואה` | `CompareOn` / `CompareOff` | (executed) the picture's split comparison ([picture](picture.md#original--enhanced-the-split-comparison-2026-10-19)) |

**What the conversation keeps** (`ConversationState`, fixed storage: 256 + 512 bytes, no allocation):
- **The last quote searched** and the match shown, copied (the command bar's text is gone after Enter). `MediaBounds::matches` is the number of cues it is found in now, counted by the engine in the subtitles on screen. A subtitle track change therefore changes the count, and a match follow-up for a quote no longer found is `NotFound`. A follow-up shows "Match 2 of 5".
- **Where playback was before the last jump**, whoever made it: a command, an arrow key, a chapter key, or the dock's timeline. The engine reports every jump (`NoteJump` in `RequestSeek`); a timeline drag reports where it began, not each scrub step.
- **The last repeatable command's text.** It is replaced by the next relative seek, volume step, chapter or scene command, and cleared by anything else done (a pause, a search, an undo), after which "again" replays the line on screen. A refused command changes nothing.

**Rules.**
- `NoContext` is the new refusal for a follow-up with nothing before it: "undo" before any jump, "next match" before any search, and any follow-up through a dispatcher with no conversation attached (`Converse`).
- A match follow-up with no search to follow is `NoContext` already in `Parse`. `Interpret` then treats it like unknown text: typed into the bar, "the first" or "the next one" before any search looks for those words in the subtitles.
- Ordinals in the spaced languages are read as a bag of words, like the track commands: one ordinal and words of a small filler set (`the`, `match`, `result`, `go to`, `show me`, `את`, `התוצאה`, `el`, `resultado`, `le`, `résultat`, `der`, `Treffer`, `результат`, `покажи`, `النتيجة`…).
  - An ordinal needs another word with it: `second`, `segundo` (also a unit), `deuxième` alone stay unknown.
  - The forms that stand alone are the Hebrew and Arabic definite forms (`השני`, `الثاني`) and Russian (no article: `второй`).
  - `one` counts only after the ordinal (`the second one`). No indefinite article is a filler. So `one second`, `une seconde`, `un segundo` and `eine Sekunde` (a second of time) are no ordinals.
  - Chinese and Japanese ordinals are phrases: `第二个`, `第二条结果`, `最后一个`, `二番目`, `3番目`, `二つ目`, `最後`.
- **No search verb starts a follow-up**: `find next week` and `find the second one` still look for "next week" and "the second one".
- `שוב` was a courtesy word. It is now courtesy only at the end of a command (`תעצור שוב` pauses), and a command of its own alone. English `again` is no courtesy: `play it again` stays a quote, as before.
- Follow-ups exist in all nine languages since 2026-10-19 (second directive):

| | Next / previous match | Undo | Again | The line again | Compare on / off |
|---|---|---|---|---|---|
| Arabic | النتيجة التالية, التالي / السابق | إلغاء, ارجع إلى حيث كنت | مرة أخرى, مجددا, كرر | ماذا قال | قارن الجودة / أوقف المقارنة |
| Spanish | siguiente resultado, el siguiente / el anterior | deshacer, vuelve a donde estaba | otra vez, de nuevo | ¿qué dijo? | pantalla dividida / dejar de comparar |
| French | résultat suivant, le suivant / le précédent | annuler, reviens où j'étais | encore, encore une fois | qu'est-ce qu'il a dit | comparer la qualité / arrête de comparer |
| German | nächster Treffer, der nächste / der vorherige | rückgängig machen, zurück wo ich war | nochmal, noch einmal | was hat er gesagt | Qualität vergleichen / Vergleich beenden |
| Russian | следующий результат, следующий / предыдущий | отмени, вернись туда где я был | ещё раз, снова | что он сказал | сравнить качество / выключи сравнение |
| Chinese | 下一个结果, 下一条 / 上一个结果 | 撤销, 回到刚才的位置 | 再来一次, 重复 | 他说了什么 | 对比画质 / 关闭对比 |
| Japanese | 次の結果 / 前の結果 | 元に戻す, 取り消し | もう一度, 繰り返して | 何て言った | 画質を比較 / 比較をやめる |

  Their neighbours keep their meaning (`siguiente capítulo`, `volver`, `arrête`, `zurück`, `вернись`, `أوقف`, `إلغاء الكتم`, `تراجع`, `重播`, `最初から`, `下一章`, `次の章`): the longest phrase wins as everywhere.

## Grammar

One row per action; each language lists its main forms. Imperative and infinitive forms are both accepted where a language has them.

| Action | English | Hebrew | Arabic | Chinese | Japanese |
|---|---|---|---|---|---|
| Play | play, resume, continue | נגן, המשך, הפעל | شغل, تشغيل, استمر | 播放, 继续 | 再生, 再開, 続ける |
| Pause | pause, stop, hold | עצור, השהה, הפסק | توقف, أوقف, إيقاف | 暂停, 停止 | 一時停止, 止めて, ポーズ |
| Volume up | volume up, louder, turn it up | הגבר, תגביר, יותר חזק | ارفع الصوت | 增大音量, 调高音量, 大声点 | 音量を上げる, 音量アップ |
| Volume down | volume down, quieter | הנמך, תנמיך, החלש | اخفض الصوت | 减小音量, 调低音量, 小声点 | 音量を下げる, 音量ダウン |
| Set volume | volume N, set the volume to N | עוצמה N, ווליום N | الصوت N | 音量N, 音量调到N | 音量 N, 音量をNに |
| Mute / unmute | mute / unmute, sound on | השתק / בטל השתקה | كتم الصوت / إلغاء الكتم | 静音 / 取消静音 | ミュート / ミュート解除 |
| Seek back | rewind, back, go back | חזור, אחורה | ارجع, للخلف | 快退, 后退, 倒退 | 巻き戻し, 戻る, …戻る |
| Seek forward | forward, skip, fast forward | קדימה, דלג, התקדם | تقدم, للأمام | 快进, 前进 | 早送り, 進む, …進む |
| Go to a time | go to, seek to, jump to | קפוץ, עבור, לך ל‑ | اذهب إلى, انتقل إلى | 跳到, 转到 | …に移動, …にジャンプ |
| Go to the start | restart, start over | מההתחלה, התחל מחדש | من البداية | 重新开始, 从头开始 | 最初から, 最初に戻る |
| Find a quote | find, search for, look for | מצא, חפש | ابحث عن | 查找, 搜索, 寻找 | 検索, …を検索, …を探して |

| Action | Spanish | French | German | Russian |
|---|---|---|---|---|
| Play | reproducir, reanudar, continuar | lecture, lire, reprendre | abspielen, wiedergabe, weiter | воспроизведение, продолжить, играть |
| Pause | pausar, pausa, detener | pause, arrêter, mettre en pause | pause, pausieren, anhalten, stopp | пауза, стоп, остановить |
| Volume up | subir el volumen, más alto | monter le volume, plus fort | lauter, lautstärke erhöhen | громче, увеличить громкость |
| Volume down | bajar el volumen, más bajo | baisser le volume, moins fort | leiser, lautstärke verringern | тише, уменьшить громкость |
| Set volume | volumen N | volume N, régler le volume à N | lautstärke N | громкость N |
| Mute / unmute | silenciar / activar sonido | couper le son / remettre le son | stumm / ton an | выключи звук / включи звук |
| Seek back | retroceder, rebobinar, atrás | reculer, rembobiner, en arrière | zurückspulen, zurück | назад, перемотать назад |
| Seek forward | avanzar, adelantar, saltar | avancer, avance rapide, sauter | vorspulen, vor, überspringen | вперёд, перемотать вперёд |
| Go to a time | ir a, saltar a | aller à, sauter à | gehe zu, springe zu | перейти к, перейти на |
| Go to the start | reiniciar, desde el principio | recommencer, depuis le début | von vorne, zum anfang | сначала, в начало |
| Find a quote | buscar, encontrar | chercher, trouver | suchen, finden, suche nach | найти, искать, поиск |

The complete lists are the tables at the top of [LocalIntentDispatcher.cpp](../src/ai/LocalIntentDispatcher.cpp).

Small connecting words are accepted around an argument and change nothing: `the`, `by`, `to`, `את`, `ב`, `el`, `de`, `à`, `auf`, `um`, `на`, `إلى` and the like (`turn down the volume`, `reculer de 10 secondes`, `назад на 30 секунд`, `lautstärke auf 40`).

**Amounts of volume.** `louder` alone is one 5 % step on the keyboard's grid. With a number it is that many points: `volume up 20`, `הגבר ב-20`, `20%`, `20 percent`, `20 אחוז`, `20 por ciento`, `20 pour cent`, `20 Prozent`, `20 процентов`, `٢٠٪`, `20パーセント`.

**Time spans.**

| Form | Examples |
|---|---|
| Number and unit | `30 seconds`, `10 שניות`, `10 ثواني`, `10 segundos`, `10 secondes`, `10 Sekunden`, `10 секунд` |
| Written together | `30s`, `1m30s`, `1h2m3s`, `10秒`, `1分30秒`, `10с` |
| Numerals | `十秒`, `三十秒`, `两分钟`, `一个小时`, `一百二十秒`, `半分钟`, `一分半` |
| Compound | `1 minute 30 seconds`, `un minuto y 30 segundos`, `1 Minute und 30 Sekunden`, `минуту и 30 секунд`, `دقيقة و30 ثانية` |
| Timecode | `1:30`, `1:02:03` |
| A bare number | seconds: `back 5` |
| A unit alone | one of it: `דקה`, `минуту`, `دقيقة`; the duals `דקותיים`, `שעתיים`, `دقيقتين`, `ساعتين` |
| Words and fractions | `a minute and a half`, `half a minute`, `media hora`, `une minute et demie`, `eine halbe Stunde`, `полторы минуты`, `полчаса`, `نصف ساعة`, `דקה וחצי` |

A relative seek without a span is 10 seconds. The span may come first with the direction last (`30 seconds back`, `10 שניות אחורה`, `10 segundos atrás`, `10 Sekunden zurück`, `10 секунд назад`, `10 secondes en arrière`, `10 ثواني للخلف`, `10秒 戻る`), and a jump verb with a direction is relative (`קפוץ 30 שניות קדימה`, `salta 20 segundos atrás`). A closing direction that contradicts the verb is refused (`rewind 10 seconds forward`).

**Quotes.** Everything after the verb is the quote, as typed, with one pair of surrounding quotation marks removed (`"…"`, `«…»`, `「…」`, `„…“`, `״…״`). It is a view into the command text, never a copy. No connecting word is skipped here: in `חפש את האור` the quote is `את האור`.

## A command, or else a quote (`Interpret`, `Submit`)

A search field takes both. `Interpret` applies one rule:

| Text | Result |
|---|---|
| Reads as a command | That command: `暂停`, `volumen 40`, `10 Sekunden zurück` |
| Inside quotation marks | Always a quote, whatever it says: `"pause"` looks for the word pause |
| No command at all (`Unknown`) | A quote, when the media has subtitles: `the quiet between` |
| Starts like a command but does not finish as one (`Malformed`) | A quote: `play it again sam`, `播放器` |
| A command the media refuses (`OutOfRange`, `Unavailable`) | Stays refused: `forward 2 hours` in a two-hour film. It was understood |
| No subtitles to search | Unknown text stays `Unknown` |
| A match follow-up with no search to follow (`NoContext`) | A quote: `the first`, `the next one` |

A quote found this way has `Intent::literal` set. `Submit` executes what `Interpret` read.

## Validation

| Status | Meaning | Examples |
|---|---|---|
| `Ok` | A typed action, ready to execute | |
| `Empty` | Nothing but white space or invisible characters | |
| `TooLong` | Over 512 bytes, or a quote over 256 bytes | |
| `InvalidText` | Not UTF-8 | |
| `Unknown` | No command starts like this | `make coffee`, `10 seconds`, `こんにちは` |
| `Malformed` | A known command with a missing, unreadable, surplus or contradictory argument | `volume`, `rewind banana`, `go to 1:75`, `pause 5`, `find`, `快退香蕉` |
| `OutOfRange` | A readable argument outside the media | `volume 150`, `rewind 0`, `forward 2 hours` in a two-hour film, `音量200`, more than 24 hours |
| `Unavailable` | The media cannot do it | a seek on a fragmented or duration-less stream; a search without subtitles |
| `NotFound` | Executor: the quote is in no cue | |
| `Rejected` | Executor: the engine refused (a full control queue) | |
| `NoContext` | A follow-up with nothing before it to follow | `undo` before any jump, `next match` before any search |

Range rules for seeks:
- The position is where playback is, or the target of a seek still on its way, so two quick `rewind 10` make 20 seconds.
- Back past the start is the start: `rewind 30s` at 0:10 lands on 0:00.
- A target at or past the end is refused, relative or absolute: there is no frame to land on.
- A refusal changes nothing in the engine.

Grammar errors are reported before availability: `rewind banana` is `Malformed` even on a stream that cannot seek.

## Execution notes

- `VolumeUp` and `VolumeSet` end a mute; `VolumeDown` does not. `Mute` and `Unmute` set the state; they do not toggle. `Play` and `Pause` are likewise idempotent.
- Every volume change, whoever made it, shows the dock's volume pill ([glass dock](kinetic-hud.md)).
- A quote search lands on its cue's start with the cue on screen ([subtitle search](subtitle-search.md)). A quote in no cue falls back to the cues that mean it ([concept search](subtitle-search.md#concept-search-2026-10-20)), with a `Concept match: "…"` cue; follow-ups step through those matches the same way.
- `NextFile` and `PreviousFile` end `Run` with `RequestedFile()` set; the session opens the file ([sessions](session.md)).
- `Command()` and `Submit()` belong to the UI thread, like `RequestSeek`.

`PlaybackOptions::commands` runs a list of commands at given times after `Run` starts and records each outcome (`CommandAt`). The demo's `--command "text"` uses it for one command a second into playback, and `--test-intents` for the regression script. `PlaybackOptions::typing` types keys into the window instead (`--test-palette`).

## Verification

| Test | Checks |
|---|---|
| `media_intent_dispatcher_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **452 accepted commands** with their exact kind, target, resulting volume and quote: 140 English and Hebrew, 28 Arabic, 38 Chinese, 29 Japanese, 29 Russian, 30 Spanish, 30 French, 31 German, 13 alternative spellings (fullwidth, accents, other digits), **52 conversational sentences** (24 English, 22 Hebrew, 6 in four other languages) and **32 track commands** (also through `Interpret`, never taken for a quote). **107 refusals**, each with its status, in every language, 21 of them framed sentences that must stay refused. Track bounds: no track to choose is `Unavailable`; a bare language name, `audio`, `next`, an unknown language or two languages stay `Unknown`; ISO 639 codes canonicalised (`fre` → `fra`, `iw` → `heb`). **The tables themselves:** every word is already in its folded spelling and no table lists a word twice. **Other media states:** length limits, a stream that cannot seek, no duration, no subtitles, positions at and beyond both ends, volume at both ends, off the grid and NaN. **Dispatch and Submit:** only `Ok` intents reach the executor, once; its answer is the outcome; statistics. **Interpret:** commands stay commands in nine languages, other text becomes a quote, quotation marks force a quote, refusals stand, no subtitles. **Fuzz:** 60,000 random sentences of grammar words of all nine languages and 40,000 random byte strings, each parsed twice: identical results, every accepted seek inside the media, every volume in 0–1, every quote inside the command text. Zero allocations throughout. |
| The same, latency | 35 commands of the nine languages through `Interpret`, in batches of 14,000; the fastest batch counts. Measured: **596 ns per command** (MSVC Release; 556 ns before the conversation tables); the fuzz mix averages 0.60 µs. Conversational and track sentences: **869 ns** each. Optimised builds assert under 1 µs per command and 2 µs per sentence; sanitised builds only report. |
| `media_playback_tracks_test` | Fourteen spoken-style commands, track changes among them, during real playback of an 8-track file: see [tracks](tracks.md). |
| `media_playback_intent_test` | Twelve commands during real H.264/AAC playback with the three-cue track: `תנמיך`, `volume 40`, `השתק`, `בטל השתקה`, `הגבר`, `forward 1 hour` (refused: out of range), `make coffee` (refused: unknown), `מצא עברית`, `עצור`, `חזור שנייה`, `find 日本語`, `נגן`. Each leaves the expected volume, mute and pause state; the three seeks land exactly, with the target's cues on the landing frame; the median time from text to engine action is under 1 ms (measured: median 5–7 µs); the volume pill was composited; gain changes ramped; no dropped frame, zero allocations. |
| `media_playback_palette_test` | Nine texts typed into the window in seven languages: see the [command bar](command-palette.md). |
| `media_intent_dispatcher_tests` (`FollowUps`) | **134 follow-up phrasings** in the nine languages with their kind and ordinal (51 English and Hebrew, 69 in the seven others, 14 neighbours that must keep their meaning), among them the commands that share their first words and must stay what they were (`go back 10 seconds`, `בטל השתקה`, `בטל כתוביות`, `תעצור שוב`, `find next week`, `next chapter`). Refusals: `NoContext` without a search (and `Interpret` making a quote of it), a lone or double ordinal, `play it again` still malformed, no subtitles or no seeking. **A conversation against an engine stub** that reports its jumps: nothing to follow at first; a search whose command text is overwritten after Enter; next, previous (wrapping both ways), by ordinal, `OutOfRange` past the last without executing; undo and undo again; `back 10 seconds`, `again`, `עוד פעם`, `שוב` from wherever playback is, with a refused command between them changing nothing; `louder` three times; `pause` ending what "again" repeats; `Reset`; a dispatcher without a conversation. In the conversation, matches are also stepped through in Spanish, Chinese, French, German, Russian, Japanese and Arabic, "again" is said in Spanish, German and Chinese, and "undo" in Chinese. A second of time is never an ordinal (`one second`, `une seconde`, `un segundo`, `eine Sekunde`). Fuzz words include the new vocabulary. 0 allocations. Latency with the follow-ups in nine languages: 636 ns a command, 899 ns a sentence (Release). |
| `media_playback_followup_test` | During real playback of the three-cue fixture: `find ·` (in cues 1 and 2), `next match`, `התוצאה הקודמת`, `את השני`, `בטל`, `חזור שנייה`, `עוד פעם`, `what did he say`, `the fifth one` (refused, out of range), `compare quality`, `stop comparing`. Each has the expected kind, outcome and origin (`Intent::from`), every seek lands exactly on its target with the target's captions (the replayed line on its cue's start), 8 seeks, 10 pictures converted split, comparison off at the end. |
| `media_intent_dispatcher_tests` (`Files`) | 31 file commands in the nine languages, through `Parse` and `Interpret` (never a quote), conversational framing included; `Unavailable` at either end of the folder and for a file played alone; `הפרק הבא`, `next chapter`, `next one`, `следующее`, `next scene`, `下一章` and `find the next video` keep their meaning; `next video 2` is malformed. Latency with the file phrases: 665 ns a command, 908 ns a sentence, measured while the playback suite ran on the same machine (636 and 899 ns before, measured idle). |
| `media_playback_resume_test` | `הסרטון הקודם`, `previous video`, `next video` and Ctrl+Right / Ctrl+Left during real playback move through a folder of fixtures: see [sessions](session.md). |
| `media_intent_dispatcher_tests` (`Speeds`) | 61 speed and zoom commands in the nine languages, with their kind and rate, through `Parse` and `Interpret` (never a quote); `speed 3x` and `speed 0.1` out of range; `speed fast`, `speed`, `speed 1.5.2`, `speed 1..5`, `speed .5`, `speed 1.2345`, `faster 2` and `zoom in 2` malformed; a full stop after a rate is no decimal point; `בטל`, `увеличить громкость`, `reducir el volumen`, `play` and `forward 10` keep their meaning. The tables still list no word twice. |
| `media_playback_speed_test`, `media_playback_zoom_test`, `media_playback_music_test` | `speed 2x`, `half speed` and `מהירות רגילה`; `zoom in` and `אפס זום`; `next chapter` and `speed 1.5x` in music mode, during real playback: see [audio](audio-dsp.md#playback-speed-2026-10-20) and [picture](picture.md#zoom-2026-10-20). |

## Open

- The grammar is closed: a phrasing outside the tables is `Unknown`. It is not a language model and does not guess. Conversation is a table of frames, not understanding: `would it be possible to pause` is read only if its frame is listed (it is not), and a sentence with two commands (`pause and turn it down`) is refused as `Malformed`.
- Track commands are not in the Chinese and Japanese tables. Korean, Hindi, Portuguese and other languages are not in the tables; their text is always a quote.
- Inflection is covered by listing forms, not by rules. Russian and Arabic verbs have more forms than the tables hold (feminine and plural imperatives, for instance).
- Chinese and Japanese phrases are matched at the start or end of a word only. A Japanese amount in the middle (`音量を20上げて`) is not read, nor Chinese `一个半小时`.
- A quote that itself starts with a verb of a spaceless language is read as a command (`搜索…` searches for the rest). Quotation marks force the quote.
- Hebrew and Arabic prepositions attached to a number are understood (`ב-20`, `ל-1:30`, `ו-30`, `و30`); other attached forms are not (`לדקה 5`), nor spelled-out time of day.
- Decimal amounts (`1.5 minutes`) are refused; `a minute and a half` is accepted.
- ~~There is no "next match" command yet.~~ Follow-ups exist since 2026-10-19 (above), in the nine languages.
- "Again" repeats a command, not an utterance: after a search it replays the line on screen, not the search.
- Measuring a relative seek from the target of a seek still on its way is implemented in `Bounds()` but no test gives two commands that close together.

## Night mode, bookmarks and the pinned frame (2026-10-21)

Night mode ([audio](audio-dsp.md#loudness-and-night-mode-2026-10-21)), bookmarks ([session](session.md#bookmarks-2026-10-21)) and Pinned | Live ([picture](picture.md#pinned--live-2026-10-21)), in the nine languages.

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `NightOn` | night mode, normalize volume, smart volume, even out the volume | מצב לילה, הפעל מצב לילה, אזן עוצמת קול, איזון עוצמה | الوضع الليلي; modo nocturno; mode nuit; Nachtmodus; ночной режим; 夜间模式; ナイトモード |
| `NightOff` | night mode off, turn off night mode | בטל מצב לילה, כבה מצב לילה | desactivar modo nocturno; Nachtmodus aus; 关闭夜间模式 |
| `BookmarkAdd` | add bookmark, bookmark this, save this point | סמן הערה, שמור נקודה, הוסף סימנייה, סימנייה | marcador; signet; Lesezeichen; закладка; 添加书签; ブックマーク追加 |
| `BookmarkGo` (`match`: from 1; none: the next one) | go to bookmark 2, jump to bookmark | לך לסימנייה 3, עבור לסימנייה, סימנייה מספר 2 | ir al marcador; aller au signet; zum Lesezeichen; к закладке; 跳到书签; ブックマークへ |
| `BookmarkNext` / `BookmarkPrevious` | next bookmark / previous bookmark | הסימנייה הבאה / הסימנייה הקודמת | siguiente marcador; signet précédent; 下一个书签; 前のブックマーク |
| `PinFrame` | pin frame, keep this frame for comparison | הקפא פריים להשוואה, שמור פריים להשוואה | fijar fotograma; закрепить кадр; 固定画面; フレーム固定 |
| `ComparePinned` | compare with the pinned frame, compare pinned | השווה לפריים הקפוא | comparar con el fotograma fijado; 与固定画面对比 |

- `Argument::Index` reads the bookmark number: digits only, at most one word (`go to bookmark two three` is `Malformed`).
- `BookmarkGo`, `BookmarkNext` and `BookmarkPrevious` need a seekable media (`Unavailable`) and a bookmark (`NoContext`; `MediaBounds::bookmarks`).
- "Again" repeats `next bookmark` and `previous bookmark`.
- The keys: **K** night mode, **B** a bookmark (and the frame), **Ctrl+B** the frame, **V** the comparisons.

## Audio zoom, micro-motion and cures (2026-10-21)

Audio zoom ([audio](audio-dsp.md#mastering-eq-and-audio-zoom-2026-10-21)), micro-motion ([picture](picture.md#picture-controls-and-micro-motion-2026-10-21)) and the symptom cures, in the nine languages.

| Intent | English | Hebrew | Other languages |
|---|---|---|---|
| `AudioZoomOn` | audio zoom, acoustic zoom, zoom the sound, audio focus | זום אקוסטי, הפעל זום אקוסטי, זום שמע, מקד את הקול | تكبير الصوت; zoom de audio; zoom audio; akustischer Zoom; звуковой зум; 音频变焦; オーディオズーム |
| `AudioZoomOff` | audio zoom off, turn off audio zoom | בטל זום אקוסטי, כבה זום אקוסטי | quitar zoom de audio; zoom audio off; 关闭音频变焦 |
| `MicroMotionOn` | micro motion, tactical vision, motion magnification, magnify motion | הדגש תנועה זעירה, תנועה זעירה, ראייה טקטית | تضخيم الحركة; visión táctica; vision tactique; taktische Sicht; тактическое зрение; 微动放大; タクティカルビジョン |
| `MicroMotionOff` | micro motion off, tactical vision off | בטל הדגשת תנועה, כבה תנועה זעירה | desactivar visión táctica; Mikrobewegung aus; 关闭微动放大 |
| `CureDark` | too dark, the picture is too dark, lift the shadows | חשוך, חשוך מדי, התמונה חשוכה, כהה מדי | muy oscuro; trop sombre; zu dunkel; слишком темно; 太暗; 暗すぎる |
| `CureNoise` | noisy, there is a hum, humming, buzzing, remove the hum | יש זמזום, זמזום, יש רעש, רועש | hay zumbido; ça bourdonne; es brummt; гудит; 有嗡嗡声; ノイズがある |
| `CureMuffled` | muffled, the dialogue is muffled, can't hear the dialogue | הדיבור נבלע, הדיבור לא ברור, לא שומעים את הדיבור | diálogo apagado; dialogue étouffé; Dialog dumpf; речь неразборчива; 对白听不清; セリフがこもる |

**Cures** (`ui::PictureAudioCures`, portable): a symptom names its remedy.
- `CureDark`: shadow lift at least 0.6, gamma at least 1.15, brightness +0.03 (at most 0.1).
- `CureNoise`: the notch killer on, and 31.5 Hz at −6 dB at most (rumble).
- `CureMuffled`: 2 kHz +4 dB, 4 kHz +3 dB, 1 kHz +2 dB, 250 Hz −2 dB, dialogue clarity at least 2, speech-protected ducking on.

Before applying one, the engine keeps every setting a cure touches (`CurrentSettings`: the picture controls and the DSP controls). A status cue says what changed and that "undo" puts it back.

**Undo.** "undo" / "בטל" already meant "go back to where I was" (the last jump). The conversation now remembers which came last:
- a cure → `ConversationState::Resolve` turns `Undo` into `UndoCure`, which restores exactly the settings from before it (bit for bit);
- a jump (`NoteJump`) after the cure makes "undo" the jump's again.

Undo is one step: once a cure is undone, the next "undo" is the last jump's (or `NoContext`). `MediaBounds::cure` lets "undo" through the grammar on media that cannot seek while a cure is pending.

**Measured** (`media_eq_picture_tests`, portable; `media_c_api_tests` through the SDK):
- the eleven phrases above (Hebrew and English) parse to their intents;
- each cure changes the settings, and "בטל" / "undo" restores them exactly (`memcmp` of the 84-byte settings); a second undo has nothing to undo; with the cure undone, "undo" is a seek again;
- two cures in a row: undo takes back the second only;
- through the C SDK while playing: `חשוך` runs, `בטל` runs, a second `בטל` is refused.

## Target tracking (2026-10-21)

`TrackTarget` and `StopTracking` ([picture](picture.md#target-tracking-2026-10-21)); the key is **T** (on, and off again).

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| on | track target, track the object, follow the object, follow this, lock on target, start tracking | עקוב אחרי האובייקט, תעקוב אחרי המטרה, עקוב אחרי זה, נעל מטרה, הפעל מעקב | تتبع الهدف, seguir el objetivo, suivre la cible, Ziel verfolgen, следи за целью, 跟踪目标, ターゲットを追跡 |
| off | stop tracking, tracking off, stop following the object, release the target | הפסק מעקב, תפסיק לעקוב, בטל מעקב, כבה מעקב | الغاء التتبع, dejar de seguir, arreter le suivi, Verfolgung beenden, прекрати слежение, 停止跟踪, 追跡停止 |

"track" alone or with "audio" or "subtitle" stays a track command ("next audio track"). Nothing to follow (no picture, music mode, a flat patch in the middle) is said, not done.

## Look at the sound (2026-10-22)

`AudioSaccadeOn` and `AudioSaccadeOff` ([audio](audio-dsp.md#sound-localisation-and-the-audio-saccade-2026-10-22)): each sudden sound turns the view toward it. Host mode and the windowed engine; the SDK also has `media_engine_set_audio_saccade`.

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| on | look at sound, look at the sound, look towards the sound, follow the sound, turn to the sound, audio saccade | עקוב אחרי הקול, תעקוב אחרי הצליל, תסתכל לכיוון הקול, פנה לכיוון הקול | اتبع الصوت, انظر الي الصوت; sigue el sonido; regarde le son; folge dem Geräusch; следи за звуком; 跟随声音; 音を追って |
| off | stop following the sound, stop looking at the sound, audio saccade off, ignore sounds | תפסיק לעקוב אחרי הקול, הפסק להסתכל על הקול, בטל מעקב קול | لا تتبع الصوت; deja de seguir el sonido; arrete de suivre le son; folge dem Geräusch nicht; не следи за звуком; 停止跟随声音; 音を追うのをやめて |

Neighbours keep their meaning (`media_perception_tests`, 26 of 26): "follow the object" and "עקוב אחרי האובייקט" are still tracking, "stop tracking" still stops tracking, "volume up" and "הגבר קול" (קול is also "volume") still the volume, "stop" still pauses. Without a stereo pair and a picture the command is said to be unavailable, not done.

## Rotation (2026-10-28)

`RotateVideo` (a quarter turn clockwise), `RotateVideoBack` (counter-clockwise), `LevelLeft` / `LevelRight` (a degree of leveling) and `LevelHorizon` (straight again; nothing to do: no context) turn the picture of the windowed engine ([picture](picture.md#rotation-2026-10-28)). The keys: R and Shift+R a quarter turn, Ctrl+R and Ctrl+Shift+R a degree.

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| rotate | rotate video, rotate the picture, rotate clockwise, rotate right, turn the video | סובב תמונה, תסובב את התמונה, סובב וידאו, סובב ימינה | دور الفيديو, تدوير الصوره; girar el video; tourner la vidéo, faire pivoter la vidéo; Video drehen; поверни видео; 旋转视频; 動画を回転 |
| back | rotate left, rotate counterclockwise, rotate back | סובב שמאלה, תסובב בחזרה | دور الفيديو لليسار; girar a la izquierda; tourner à gauche; nach links drehen; поверни влево; 向左旋转; 左に回転 |
| level | level horizon left / right, tilt left / right | יישר אופק שמאלה / ימינה, הטה שמאלה / ימינה | امل لليسار / لليمين; inclinar a la izquierda; incliner à gauche, redresser l'horizon à droite; nach links neigen; наклони влево; 向左倾斜; 左に傾ける |
| straight | straighten, straighten the picture ("level horizon" until 2026-10-29) | יישר אופק, יישר את האופק, יישר תמונה | عدل الافق; nivelar el horizonte; redresser l'horizon; Horizont ausrichten; выровняй горизонт; 水平校正; 水平補正 |

`media_rotation_tests`: 28 of 28 (the nine languages, and "zoom in", "track the target" and "look at the sound" keep their meaning). Host mode refuses them (its host draws the picture). (2026-10-30: host mode acts on them: [API 6](sdk.md#rotation-the-horizon-and-black-bars-api-6-2026-10-30).)

## Auto-leveling (2026-10-29)

`AutoLevel` finds the horizon's tilt in the picture and keeps the leveling turn on it; `AutoLevelOff` stops it, keeping the turn it had reached ([picture](picture.md#auto-leveling-the-horizon-2026-10-29)). "level horizon" and "level the horizon" are now `AutoLevel` (they straightened before: "straighten" still does, as do "יישר אופק" and "redresser l'horizon"). Already on (or already off): `NoContext`. A manual level ("level horizon right", Ctrl+R) or "straighten" turns it off. The keys: Shift+R on and off (it turned a quarter back: Alt+R does now). Music mode refuses it with a cue; host mode refuses it.

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| on | auto level, auto level horizon, level horizon, level the horizon (automatically), automatic horizon, keep the horizon level | יישר אופק אוטומטית, תיישר את האופק אוטומטית, יישור אופק אוטומטי, אופק אוטומטי | عدل الافق تلقائيا; nivelar el horizonte automáticamente, nivelación automática; niveler l'horizon, nivelle l'horizon, horizon automatique, redresser l'horizon automatiquement; Horizont automatisch ausrichten; автоматически выровняй горизонт, автовыравнивание горизонта; 自动水平, 自动水平校正; 自動水平補正, 水平を自動で合わせて |
| off | auto level off, stop auto level(l)ing, turn off auto level, stop leveling the horizon | בטל יישור אופק אוטומטי, כבה יישור אופק אוטומטי, תפסיק ליישר את האופק | اوقف تعديل الافق التلقائي; desactivar nivelación automática; arrête de niveler l'horizon, désactive la mise à niveau automatique; automatischen Horizont aus; выключи автовыравнивание; 关闭自动水平; 自動水平補正をオフ |

`media_rotation_tests`: 46 of 46 (the rotation commands, these in nine languages, and the neighbours).

## Stabilisation (2026-11-04)

`Stabilize` steadies the view of a shaky picture, `StabilizeOff` stops ([picture](picture.md#stabilising-the-window-2026-11-04)). Already on (or already off): `NoContext`. Under 128 samples a side, or without the telemetry module: `Rejected` with a cue, and music mode refuses it. The key: Alt+S on and off (S alone is a snapshot). In host mode the same two intents reach `media_engine_set_stabilization`'s path.

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| on | stabilize, stabilize video, stabilize the picture, stabilize on, stabilization on, video stabilization, image stabilization, steady the picture, remove camera shake (and the same with "stabilise") | ייצב תמונה, תייצב את התמונה, ייצב וידאו, ייצוב תמונה, הפעל ייצוב | ثبت الصورة, تثبيت الفيديو; estabilizar el video, estabiliza la imagen; stabiliser la vidéo, stabilise l'image, stabilisation activée; Video stabilisieren, Bild stabilisieren; стабилизируй видео, стабилизация видео; 稳定画面, 防抖, 开启防抖; 手ぶれ補正, 映像を安定させて |
| off | stabilize off, stop stabilizing, stabilization off, turn off stabilization, no stabilization | בטל ייצוב, כבה ייצוב, הפסק ייצוב, בלי ייצוב | اوقف التثبيت; desactivar estabilización; arrête de stabiliser, désactive la stabilisation; Stabilisierung aus; выключи стабилизацию, отключи стабилизацию; 关闭防抖; 手ぶれ補正をオフ |

`media_rotation_tests`: these in nine languages with the rotation commands and their neighbours.

## Black bars (2026-10-30)

`CropBars` fills the view with the picture inside black bars encoded in it (letterbox, pillarbox) as soon as they are known, and follows them; `CropBarsOff` shows the whole picture again ([picture](picture.md#black-bars-perspective-and-real-footage-2026-10-30)). Already on (or off): `NoContext`. A zoom ("zoom in", "reset zoom", the wheel) turns it off. The key: X. Music mode refuses it with a cue. Host mode (the C SDK, API 6) acts on these and on the rotation and auto-leveling commands too: its host draws the picture the way `media_picture_t` says ([sdk](sdk.md#rotation-the-horizon-and-black-bars-api-6-2026-10-30)).

| | English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|---|
| on | crop black bars, crop the bars, auto crop, autocrop, crop the letterbox, remove / hide the black bars, get rid of the black bars | חתוך שוליים שחורים, תחתוך את השוליים השחורים, חתוך פסים שחורים, הסר / תסיר את הפסים השחורים, חיתוך אוטומטי | قص الحواف السوداء, ازل الاشرطة السوداء; recortar / quitar las bandas negras, recorte automático; recadrer les bandes noires, supprimer / enlever les bandes noires, recadrage automatique; schwarze Balken entfernen, automatisch zuschneiden; обрежь / убери чёрные полосы, автокадрирование; 裁剪黑边, 去除黑边, 自动裁剪; 黒帯をカット, 黒帯を消して, 自動クロップ |
| off | show (the) black bars, keep the black bars, uncrop, stop cropping, crop off, auto crop off, no crop | הצג שוליים שחורים, תחזיר את השוליים השחורים, בטל חיתוך, בטל חיתוך אוטומטי | اظهر الحواف السوداء, الغ القص; mostrar las bandas negras, sin recorte; afficher les bandes noires, annuler le recadrage; schwarze Balken anzeigen; покажи чёрные полосы, отмени обрезку; 显示黑边, 取消裁剪; 黒帯を表示, クロップを解除 |

"show the whole picture" stays `ZoomReset` and "undo" `Undo`. `media_rotation_tests`: 67 of 67 (the rotation and auto-leveling commands, these in nine languages, and the neighbours).

## Next target (2026-10-31)

`NextTarget` moves target tracking, and with the view the audio zoom's beam, onto the next object perception keeps track of: the object after the one followed in the table's order (confirmed approaches by time to contact, then the rest by how salient they are), the most urgent when none is followed (then it starts tracking) ([picture](picture.md#several-objects-at-once-2026-10-31)). With two objects it alternates. The only object in view, already followed: `NoContext` and a cue; nothing perceived: `NotFound`. The key: Tab while tracking (Tab is otherwise the dock's focus). Host mode (the C SDK, API 7) acts on it too, and `media_engine_track_object` takes an object by its id ([sdk](sdk.md#several-objects-at-once-api-7-2026-10-31)).

| English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|
| next target, the next target, switch target(s), change (the) target, (the) other target, (the) next object, track / follow the next target, follow the next object, cycle targets, lock on(to) the next target, target the next one | מטרה הבאה, המטרה הבאה, (ת)עבור למטרה הבאה, (ת)החלף מטרה, מטרה אחרת, (ה)אובייקט הבא, (ת)עקוב אחרי המטרה הבאה | الهدف التالي, غير الهدف, الهدف الآخر; siguiente objetivo, cambiar de objetivo, otro objetivo; cible suivante, prochaine cible, changer de cible, objet suivant, autre cible; nächstes Ziel, Ziel wechseln, anderes Ziel; следующая цель, смени цель, другая цель; 下一个目标, 切换目标, 换个目标; 次のターゲット, ターゲット切り替え, 次の目標 |

"next one" stays `NextMatch` (a search's follow-up), "track the target" `TrackTarget`, "next chapter" `NextChapter`. `media_multi_target_tests`: 14 of 14 in nine languages and the neighbours.

## Snapshots (2026-11-01)

`Snapshot` writes the picture as the engine made it, an uncompressed BMP with its telemetry beside it (a JSON sidecar: the time, the frame hash, the view, the orientation, the horizon, the picture controls, the target and the objects perceived; [picture](picture.md#forensic-snapshots-2026-11-01)). The windowed engine writes the picture as composed on screen on its snapshot thread ("Snapshot saved · <name>"; another one while one is on its way: `NoContext` and a cue); host mode (the C SDK, API 8) writes the GPU image or the view of the picture it holds before the command returns ([sdk](sdk.md#snapshots-api-8-2026-11-01)). The key: S (the demo's binaural rendering is Shift+S now).

| English | Hebrew | Arabic, Spanish, French, German, Russian, Chinese, Japanese |
|---|---|---|
| take (a) snapshot, snapshot, save (a) snapshot, take (a) screenshot, screenshot, save / capture / grab / export (the, this) frame, save / capture (the) picture or image | צילום מסך, (ת)צלם מסך, (ת)צלם את המסך, (ת)צלם תמונה, (ת)שמור (את ה)תמונה, תמונת מסך, (ת)צלם | لقطة شاشة, التقط صورة, احفظ الصورة, التقط الشاشة; captura de pantalla, tomar (una) captura, capturar (la) imagen, guardar (la) imagen, guardar fotograma, tomar (una) foto, instantánea; capturer l'image, capture d'écran, prendre / faire une capture, enregistrer / sauvegarder l'image, prendre une photo, instantané; Bildschirmfoto, Schnappschuss, Bild speichern, Standbild speichern, Bild aufnehmen, Foto machen; снимок экрана, сделай снимок, сохрани кадр, скриншот, снимок; 截图, 截屏, 屏幕截图, 保存画面, 保存图片, 拍照, 快照; スクリーンショット, スナップショット, 画面を保存, 画像を保存, キャプチャ, 写真を撮って |

"pin frame" stays `PinFrame` (the reference for Pinned | Live, "שמור פריים להשוואה"), "save this point" `BookmarkAdd`. `media_snapshot_tests`: 18 of 18 in nine languages and the neighbours.
