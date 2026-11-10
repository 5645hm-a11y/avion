#pragma once
#include <cstdint>
#include <string_view>

namespace media::ai {
// SelectAudio / SelectSubtitles: a track by language ("english audio", "כתוביות בעברית"), off, on or the next one.
// SelectLanguage: a language with no track named ("switch to french"): the executor looks for it among the audio tracks
// first, then among the subtitle tracks.
// NextChapter ... PreviousScene: the executor finds the mark from the position (docs/chapters.md); the grammar only checks
// that the media is seekable and has chapters (or scenes) at all.
// Follow-ups (docs/intents.md, "Follow-ups"): NextMatch, PreviousMatch, NthMatch ("next match", "the second one", "את
// השני"), Undo ("go back to where I was", "בטל") and Repeat ("again", "עוד פעם") refer to what was done before; a
// ConversationState turns each into the final action it means (a FindSubtitle with its match, a SeekAbsolute, the last
// command again) before anything executes, and the executor never sees them. ReplayCue ("what did he say") is the cue on
// screen from its start; CompareOn / CompareOff the split comparison of the picture (docs/picture.md).
// NextFile / PreviousFile: the next or previous playable file in the folder ("next video", "הסרטון הקודם"); the session
// that plays the folder does it (docs/intents.md, "Files").
// SpeedUp / SpeedDown: one step of the speed list faster or slower ("faster", "לאט יותר"); SpeedSet: a speed, 0.25 to 2
// ("speed 1.5x", "double speed", "מהירות כפולה", "normal speed"; docs/intents.md, "Speed").
// ZoomIn / ZoomOut: twice or half the zoom into the picture's centre region; ZoomReset: the whole picture ("zoom in",
// "זום פנימה", "אפס זום"; docs/picture.md, "Zoom").
// LoopStart / LoopEnd: an end of the A–B loop at the position ("set loop start", "סוף לופ"); LoopClear: the loop off;
// LoopCue: the sentence on screen (or just said) as the loop, padded 150 ms ("loop this sentence", "לופ על המשפט";
// docs/intents.md, "Loop"). ShowStats / HideStats: the telemetry panel ("show stats", "הצג נתונים"); ExplainQuality: why
// the picture quality is what it is ("why did quality change?", "למה האיכות ירדה?"; docs/intents.md, "Telemetry").
// NightOn / NightOff: night / smart loudness mode ("night mode", "normalize volume", "מצב לילה", "אזן עוצמת קול").
// BookmarkAdd: a bookmark at the position ("add bookmark", "סמן הערה", "שמור נקודה"); BookmarkGo: bookmark `match` (from 1;
// 0: the next one) ("go to bookmark 2", "לך לסימנייה"); BookmarkNext / BookmarkPrevious. PinFrame: the picture on screen as
// the reference frame ("pin frame", "הקפא פריים להשוואה"); ComparePinned: Pinned | Live (docs/intents.md, "Bookmarks").
// AudioZoomOn / AudioZoomOff: the picture's zoom steers the sound ("audio zoom", "זום אקוסטי"); MicroMotionOn / MicroMotionOff:
// temporal micro-motion magnification ("micro motion", "tactical vision", "הדגש תנועה זעירה"). Cures name a symptom and get its
// remedy (docs/intents.md, "Cures"): CureDark ("too dark", "חשוך"), CureNoise ("noisy", "יש זמזום"), CureMuffled ("muffled",
// "הדיבור נבלע"). UndoCure is never parsed: "undo" / "בטל" resolves to it while a cure is the last thing done (ConversationState).
// Rotation (docs/intents.md, "Rotation"): RotateVideo a quarter turn clockwise ("rotate video", "סובב תמונה", "tourner la vidéo"),
// RotateVideoBack counter-clockwise ("rotate left"); LevelLeft / LevelRight a degree of leveling ("level horizon left"), and
// LevelHorizon straightens it again ("straighten", "יישר אופק", "redresser l'horizon"). AutoLevel finds the horizon's tilt and
// follows it ("auto level", "level horizon", "יישר אופק אוטומטית", "niveler l'horizon"; 2026-10-29), AutoLevelOff stops it.
// Stabilisation (docs/intents.md, "Stabilisation", 2026-11-04): Stabilize steadies the view of a shaky picture ("stabilize video",
// "stabilize on", "ייצב תמונה", "stabiliser la vidéo"; Alt+S in the windowed engine), StabilizeOff stops ("stabilize off").
// Black bars (docs/intents.md, "Black bars", 2026-10-30): CropBars fills the view with the picture inside bars encoded in it
// ("crop black bars", "auto crop", "חתוך שוליים שחורים", "recadrer les bandes noires"), CropBarsOff shows them again.
// Several objects (docs/intents.md, "Next target", 2026-10-31): NextTarget moves target tracking (and the audio focus) onto the
// next object perception keeps track of, the most urgent first ("next target", "מטרה הבאה", "cible suivante"; Tab while tracking).
// Snapshot (docs/intents.md, "Snapshots", 2026-11-01): the picture as made, written as a BMP with its telemetry beside it ("take
// snapshot", "save frame", "צילום מסך", "capturer l'image"; S in the windowed engine).
enum class IntentKind : std::uint8_t { None, Play, Pause, VolumeUp, VolumeDown, VolumeSet, Mute, Unmute, SeekRelative, SeekAbsolute, FindSubtitle,
                                       SelectAudio, SelectSubtitles, SelectLanguage, NextChapter, PreviousChapter, NextScene, PreviousScene,
                                       NextMatch, PreviousMatch, NthMatch, Undo, Repeat, ReplayCue, CompareOn, CompareOff, NextFile, PreviousFile,
                                       SpeedUp, SpeedDown, SpeedSet, ZoomIn, ZoomOut, ZoomReset,
                                       LoopStart, LoopEnd, LoopClear, LoopCue, ShowStats, HideStats, ExplainQuality,
                                       NightOn, NightOff, BookmarkAdd, BookmarkGo, BookmarkNext, BookmarkPrevious, PinFrame, ComparePinned,
                                       AudioZoomOn, AudioZoomOff, MicroMotionOn, MicroMotionOff, CureDark, CureNoise, CureMuffled, UndoCure,
                                       TrackTarget, StopTracking, AudioSaccadeOn, AudioSaccadeOff,
                                       RotateVideo, RotateVideoBack, LevelLeft, LevelRight, LevelHorizon, AutoLevel, AutoLevelOff, Stabilize, StabilizeOff,
                                       CropBars, CropBarsOff, NextTarget, Snapshot };
enum class TrackChoice : std::uint8_t { Language, Off, On, Next };
enum class IntentStatus : std::uint8_t {
    Ok,
    Empty,       // Nothing but white space.
    TooLong,     // More than kMaxCommandBytes, or a quote longer than kMaxQueryBytes.
    InvalidText, // Not UTF-8.
    Unknown,     // No command of the grammar starts like this.
    Malformed,   // A known command with a missing, unreadable or surplus argument.
    OutOfRange,  // A readable argument outside the media: past the end, over 100 %, zero, more than 24 hours.
    Unavailable, // The media cannot do it: not seekable, no duration, no subtitles.
    NotFound,    // Executor: the quote is in no cue.
    Rejected,    // Executor: the engine refused (a full control queue).
    NoContext,   // A follow-up with nothing before it to follow: "next match" before any search, "undo" before any jump.
};
// What the command is checked against: the engine's state when it was given.
struct MediaBounds {
    std::int64_t duration_100ns = 0, position_100ns = 0; // Duration 0: unknown.
    float volume = 1;
    bool muted = false, paused = false, seekable = false, subtitles = false;
    unsigned audio_tracks = 0, subtitle_tracks = 0; // Tracks a Select* command can choose from.
    unsigned chapters = 0, scenes = 0;              // Marks a chapter or scene command can go to.
    unsigned matches = 0; // Cues the conversation's last quote is found in now (ConversationState::Query); 0: no search.
    unsigned files_before = 0, files_after = 0; // Playable files before and after this one in its folder (0: none, or no folder).
    unsigned bookmarks = 0; // The file's bookmarks.
    bool cure = false;      // A cure is the last thing done: "undo" takes it back (ConversationState::CurePending).
};
// A typed engine action. Every field is final: the executor applies it without interpreting anything.
struct Intent {
    IntentKind kind = IntentKind::None;
    IntentStatus status = IntentStatus::Empty;
    std::int64_t target_100ns = 0; // Seek*: absolute position, inside the media.
    std::int64_t offset_100ns = 0; // SeekRelative: the signed amount asked for (the target may be clamped at the start).
    float volume = 0;              // Volume*: the resulting volume, 0 to 1.
    float speed = 0;               // SpeedSet: the speed asked for, 0.25 to 2.
    std::string_view query;        // FindSubtitle: the quote, a view into the command text.
    bool literal = false;          // FindSubtitle from Interpret: no command was recognised, the text itself is the quote.
    TrackChoice track = TrackChoice::Language; // Select*: what is asked for.
    char language[4]{};            // Select* by language: ISO 639-2/T code ("eng", "heb"), NUL-terminated.
    unsigned match = 0;            // FindSubtitle: which of the quote's cues, 0 the first in time; NthMatch: the ordinal
                                   // said, from 1 (kLastMatch: "the last one").
    IntentKind from = IntentKind::None; // A follow-up the conversation resolved: what was said (NextMatch, Undo, Repeat).
};
class ConversationState;
// Offline, deterministic command grammar in English, Hebrew, Arabic, Chinese, Japanese, Spanish, French, German and
// Russian (docs/intents.md lists it). Parsing is a pure function of the text and the bounds: fixed stack storage,
// constant tables indexed at compile time, no allocation, no locale, no clock. It validates; it never acts.
// Dispatch hands an Ok intent to the executor, whose answer becomes the intent's status.
// Commands are read inside conversational framing: a request frame in front ("can you", "i'd like you to", "אני רוצה
// ש", "אשמח אם", "תוכל ל..."), courtesy at either end, hesitations, and softeners ("a bit", "קצת") are set aside, so
// "אשמח שתמשיך 10 שניות קדימה" is the relative seek it asks for. Anything else left over still refuses the command.
class LocalIntentDispatcher final {
public:
    static constexpr unsigned kMaxCommandBytes = 512, kMaxQueryBytes = 256;
    static constexpr std::int64_t kSecond = 10000000, kDefaultSeek = 10 * kSecond, kMaxSeek = 24 * 3600 * kSecond;
    static constexpr float kVolumeStep = 0.05f; // One "louder"/"quieter": the keyboard's step, on the same grid.
    static constexpr unsigned kLastMatch = 0xffffffffu;
    using Executor = IntentStatus (*)(void* context, const Intent&) noexcept;
    struct Statistics { unsigned commands = 0, executed = 0, refused = 0, failed = 0; }; // Refused by parsing; failed in the executor.
    void Configure(Executor executor, void* context) noexcept { executor_ = executor; context_ = context; }
    // Follow-ups: Dispatch and Submit resolve them through `state` before executing, and tell it what was done after.
    // Without one, a follow-up is refused (NoContext).
    void Converse(ConversationState* state) noexcept { conversation_ = state; }
    static Intent Parse(std::string_view utf8, const MediaBounds&) noexcept;
    // Text typed into a search field: a command when the grammar reads one, else a quote to look for (`literal`).
    // - Text inside quotation marks is always a quote, whatever it says.
    // - Text that is no command (Unknown), or starts like one but does not finish as one (Malformed), is a quote
    //   when the media has subtitles to search; otherwise the refusal stands.
    // - A command the media refuses (OutOfRange, Unavailable) stays refused: it was understood.
    // - A match follow-up with no search to follow ("the first", "next one" before any search) is a quote too.
    static Intent Interpret(std::string_view utf8, const MediaBounds&) noexcept;
    Intent Dispatch(std::string_view utf8, const MediaBounds&) noexcept; // Parse, then execute.
    Intent Submit(std::string_view utf8, const MediaBounds&) noexcept;   // Interpret, then execute.
    const Statistics& Stats() const noexcept { return stats_; }
    // Every word of the grammar's tables is already in the form typed words are folded to, no table lists a word
    // twice, and no framing word or run can hide a command (a test of the tables themselves; not needed at run time).
    // `report` is told the rule and the word of each violation.
    using TableReport = void (*)(const char* rule, std::string_view word) noexcept;
    static bool TablesConsistent(TableReport report = nullptr) noexcept;
private:
    Intent Run(std::string_view text, Intent, const MediaBounds&) noexcept;
    Executor executor_ = nullptr;
    void* context_ = nullptr;
    ConversationState* conversation_ = nullptr;
    Statistics stats_{};
};
// What the conversation so far makes of a follow-up (docs/intents.md, "Follow-ups"): the last quote searched and the match
// shown, where playback was before the last jump, and the last command that "again" repeats. Fixed storage, no
// allocation, one thread (the UI's, as Command). The engine reports every jump, whoever made it (NoteJump); the
// dispatcher reports what it executed (Record).
class ConversationState final {
public:
    // A follow-up made concrete, checked against the media now; any other intent passes unchanged.
    // - NextMatch / PreviousMatch: the quote's next (previous) cue after the one shown, round from the last to the first;
    //   NthMatch: that cue (OutOfRange past the last); NoContext before any search, NotFound when the quote is in no cue now.
    // - Undo: when a cure (CureDark, CureNoise, CureMuffled) is the last thing done, UndoCure (one step: the settings before
    //   it); otherwise a seek back to where playback was before the last jump (so undo twice comes back); NoContext before one.
    // - Repeat: the last relative seek, volume step, chapter or scene command, read again against the media now (a seek
    //   10 s back is 10 s back from here); with none, the cue on screen from its start (ReplayCue).
    Intent Resolve(const Intent& said, const MediaBounds&) const noexcept;
    // After execution: `text` is the command as given (what Repeat reads again), `done` the intent with its outcome.
    void Record(std::string_view text, const Intent& done) noexcept;
    // A jump in the media: playback was at `from`. Undo goes back there.
    void NoteJump(std::int64_t from) noexcept { jump_from_ = from; jumped_ = true; cure_ = false; } // Now the jump is the latest.
    void Reset() noexcept { query_bytes_ = repeat_bytes_ = 0; match_ = 0; jumped_ = false; cure_ = false; } // New media.
    bool CurePending() const noexcept { return cure_; } // A cure is the last thing done: "undo" takes it back.
    std::string_view Query() const noexcept { return {query_, query_bytes_}; } // The last quote searched ("" before any).
    unsigned Match() const noexcept { return match_; }
    bool Jumped() const noexcept { return jumped_; }
    std::int64_t JumpFrom() const noexcept { return jump_from_; }
    std::string_view Repeatable() const noexcept { return {repeat_, repeat_bytes_}; } // What "again" repeats.
private:
    char query_[LocalIntentDispatcher::kMaxQueryBytes]{};
    char repeat_[LocalIntentDispatcher::kMaxCommandBytes]{};
    unsigned query_bytes_ = 0, repeat_bytes_ = 0, match_ = 0;
    std::int64_t jump_from_ = 0;
    bool jumped_ = false, cure_ = false;
};
const char* Name(IntentKind) noexcept;
const char* Name(IntentStatus) noexcept;
const char* Name(TrackChoice) noexcept;
// ISO 639-2 language codes as containers write them: the bibliographic forms ("fre", "ger", "chi", ...) become the
// terminology forms the grammar uses ("fra", "deu", "zho"), lower case; `out` is NUL-terminated ("" for "und" or garbage).
void CanonicalLanguage(const char* code, char out[4]) noexcept;
// The language's English name and its own name ("Hebrew", "עברית"); nullptr for a code the grammar does not know.
const char* LanguageName(const char* code) noexcept;
const char* NativeLanguageName(const char* code) noexcept;
}
