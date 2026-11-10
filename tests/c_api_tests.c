/* media_c_api_tests: the C99 SDK (include/media/c_api.h, docs/sdk.md), from a pure C translation unit.
 *   media_c_api_tests portable_fixture.mp4 voice_custom_wake.bin [playback_chapters.mp4 (Windows)]
 * Everywhere (MSVC, GCC -std=c99 -pedantic with ASan/UBSan):
 * - defaults, the arena budget (exact: leaving any module out removes exactly its region), module budgets, invalid input;
 * - the wake word: what the built-in recogniser can hear, custom names it can synthesise, names it cannot read;
 * - the lifecycle in host storage (too small, misaligned, a bad configuration refused; create, configure, destroy);
 * - host-driven playback (MEDIA_PLAYBACK_HOST) of the portable fixture (tests/make_portable_fixture.cpp) on a virtual clock,
 *   step and 10 ms of pumped audio at a time: the software bridge's pictures (each scene's luma, the cut at 3 s, the sidecar's
 *   caption), the English PCM track (440 Hz and 4 kHz), "volume down" / "volume up", "audio in french please" (the 44.1 kHz
 *   mono track converted: 660 Hz), the custom wake word "Falcon" made hearable from the host's text-to-speech (David, Zira,
 *   Hortense, Paul) and spoken by Mark (held out): "Avion pause" and room talk set aside, "Falcon pause" pauses (the position
 *   held), "play", "faster" (1.25x, pitch kept), "find quiet between the frames" (an exact landing at 1 s), "noisy" (the notch
 *   killer takes the 660 Hz tone down) and "בטל" (back), "next chapter" (an exact landing at 6 s), "track target" (the view
 *   follows the moving square), the end; no allocation while playing and no thread started;
 * - live sensors (API 3), no file: 640x480 NV12 camera frames and stereo microphones pushed at 30 fps / 10 ms: an obstacle
 *   approaching at 1 m/s from 3 m (looming flagged, the time to contact against Z / v and falling, the hazard on it), a small
 *   object moving over a static textured scene (the saliency mask is its tiles only), "look at sound" and a burst on the right
 *   channel (azimuth near +1, the view turned right at 2x on the next frame), P010 and RGBA frames converted to the expected
 *   luma codes, wrong sizes and timestamps refused; no allocation while ingesting.
 * On Windows, with the chapters fixture (three flat-luma scenes 24 / 196 / 110 + a texture of up to 23 codes, hard cuts at 3 s
 * and 6 s, a 440 Hz tone):
 * - an engine with every module plays it to the end; its frame signatures (callback and poll agree) carry each scene's luma
 *   (mean and peak within the scene's codes), exactly two scene cuts, naming the pictures at 3 s and 6 s (confirmed four
 *   pictures later), tile motion 1 on those pictures and 0 on every picture the stream repeats, the tone's band (500 Hz) as
 *   dominant and no speech;
 * - the custom wake word "Jarvis": "jarvis volume down" runs, "volume down" and "avion volume down" are set aside; a cure and
 *   "בטל" (undo) run, a second "בטל" has nothing to undo;
 * - an engine without the HUD, subtitles, chapters, voice and telemetry modules: its arena capacity and bytes in use are smaller
 *   by exactly those regions, it starts exactly their threads fewer (the command bar's, the status cues', the subtitle shaper),
 *   it plays, and it produces no signature; a push-to-talk engine runs an utterance only while talk is held;
 * - no allocation while running. */
#include "media/c_api.h"
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

/* tests/allocation_probe_c.cpp */
void media_test_allocations_begin(void);
uint64_t media_test_allocations_end(void);

#define STORAGE (48u << 20)
static unsigned char storage_a[STORAGE];
static void* Aligned(unsigned char* base) {
    const size_t a = media_engine_storage_alignment();
    const uintptr_t p = (uintptr_t)base;
    return (void*)(base + ((a - p % a) % a));
}

static void TestPortable(void) {
    media_engine_config_t config;
    media_module_budget_t budget;
    uint64_t sum = 0, all, none;
    unsigned m;
    media_engine_t* engine = NULL;
    void* store = Aligned(storage_a);
    media_engine_config_init(&config);
    CHECK(config.struct_size == sizeof(config) && config.modules == MEDIA_MODULE_ALL && config.wake_policy == MEDIA_WAKE_DEFAULT && config.visible == 1);
    {
        /* Both ways to play: leaving a module out removes exactly its region (in host mode HUD and neural upscaling take none). */
        const media_playback_mode defaults = config.playback, modes[2] = {MEDIA_PLAYBACK_WINDOW, MEDIA_PLAYBACK_HOST};
        unsigned k;
        for (k = 0; k < 2; ++k) {
            config.playback = modes[k]; sum = 0;
            all = media_query_arena_budget(&config);
            for (m = 0; m < 6; ++m) {
                media_engine_config_t without = config;
                const int counted = modes[k] == MEDIA_PLAYBACK_WINDOW || ((1u << m) != MEDIA_MODULE_HUD && (1u << m) != MEDIA_MODULE_NEURAL_UPSCALE);
                CHECK(media_query_module_budget(1u << m, &budget) == MEDIA_OK);
                without.modules = MEDIA_MODULE_ALL & ~(1u << m);
                CHECK(all - media_query_arena_budget(&without) == (counted ? budget.arena_bytes : 0));
                if (counted) sum += budget.arena_bytes;
            }
            config.modules = 0; none = media_query_arena_budget(&config); config.modules = MEDIA_MODULE_ALL;
            printf("Budget (%s): core %.1f MiB, modules %.1f MiB, all %.1f MiB\n", k ? "host" : "window", none / 1048576.0, sum / 1048576.0, all / 1048576.0);
            CHECK(none > 0 && none + sum == all);
        }
        config.playback = defaults;
    }
    CHECK(media_query_module_budget(3u, &budget) == MEDIA_ERROR_INVALID_ARGUMENT && media_query_module_budget(1u << 6, &budget) == MEDIA_ERROR_INVALID_ARGUMENT);
    config.struct_size = 8; CHECK(media_query_arena_budget(&config) == 0); config.struct_size = sizeof(config);
    config.modules = 1u << 7; CHECK(media_query_arena_budget(&config) == 0); config.modules = MEDIA_MODULE_ALL;
    CHECK(media_query_arena_budget(NULL) == 0);
    /* Wake words. */
    CHECK(media_wake_word_hearable("avion") && media_wake_word_hearable("Avion") && media_wake_word_hearable("  avion "));
    /* API 2: names the engine can synthesise are hearable; names it cannot read are not. */
    CHECK(media_wake_word_hearable("jarvis") && media_wake_word_hearable("Falcon") && media_wake_word_hearable("Hey Jarvis") &&
          media_wake_word_hearable("\xd7\x93\xd7\xa0\xd7\x94"));
    CHECK(!media_wake_word_hearable("R2D2") && !media_wake_word_hearable("") && !media_wake_word_hearable("   ") && !media_wake_word_hearable(NULL));
    CHECK(!media_wake_word_hearable("a name that is much longer than thirty-one bytes"));
    /* Lifecycle. */
    printf("Storage: %lu bytes, aligned to %lu\n", (unsigned long)media_engine_storage_bytes(), (unsigned long)media_engine_storage_alignment());
    CHECK(media_engine_storage_bytes() > 0 && media_engine_storage_bytes() + 4096 <= STORAGE);
    CHECK(media_engine_storage_alignment() && !(media_engine_storage_alignment() & (media_engine_storage_alignment() - 1)));
    CHECK(media_engine_create(&config, store, 16, &engine) == MEDIA_ERROR_STORAGE);
    if (media_engine_storage_alignment() > 1) CHECK(media_engine_create(&config, (unsigned char*)store + 1, STORAGE - 4096, &engine) == MEDIA_ERROR_STORAGE);
    config.wake_policy = MEDIA_WAKE_CUSTOM; config.wake_word[0] = 0;
    CHECK(media_engine_create(&config, store, STORAGE - 4096, &engine) == MEDIA_ERROR_INVALID_ARGUMENT);
    strcpy(config.wake_word, "Jarvis");
    CHECK(media_engine_create(&config, store, STORAGE - 4096, &engine) == MEDIA_OK && engine != NULL);
    if (engine) {
        media_engine_stats_t stats;
        media_frame_signature_t signature;
        CHECK(media_engine_set_wake(engine, MEDIA_WAKE_CUSTOM, "") == MEDIA_ERROR_INVALID_ARGUMENT);
        CHECK(media_engine_set_wake(engine, MEDIA_WAKE_DEFAULT, NULL) == MEDIA_OK);
        CHECK(media_engine_run(engine) == MEDIA_ERROR_STATE && media_engine_close(engine) == MEDIA_ERROR_STATE);
        CHECK(media_engine_command(engine, "pause") == MEDIA_ERROR_STATE && media_engine_poll_frame(engine, &signature) != MEDIA_OK);
        CHECK(media_engine_stats(engine, &stats) == MEDIA_OK && stats.modules == MEDIA_MODULE_ALL);
        CHECK(media_engine_step(engine, 0) == MEDIA_ERROR_STATE && media_engine_pump_audio(engine, NULL, 0, 0) == MEDIA_ERROR_INVALID_ARGUMENT);
#if !defined(_WIN32)
        CHECK(config.playback == MEDIA_PLAYBACK_HOST && media_engine_open(engine, "/nonexistent.mp4") == MEDIA_ERROR_OPEN);
#endif
        media_engine_destroy(engine);
    }
#if !defined(_WIN32)
    /* The windowed engine is Windows': elsewhere it can be configured, not opened. */
    config.playback = MEDIA_PLAYBACK_WINDOW;
    CHECK(media_engine_create(&config, store, STORAGE - 4096, &engine) == MEDIA_OK);
    if (engine) { CHECK(media_engine_open(engine, "/nonexistent.mp4") == MEDIA_ERROR_UNSUPPORTED); media_engine_destroy(engine); }
    config.playback = MEDIA_PLAYBACK_HOST;
#endif
    /* Host mode takes its own voice input only. */
    config.playback = MEDIA_PLAYBACK_HOST; config.voice_input = MEDIA_VOICE_MICROPHONE;
    CHECK(media_engine_create(&config, store, STORAGE - 4096, &engine) == MEDIA_ERROR_INVALID_ARGUMENT);
    config.voice_input = MEDIA_VOICE_NONE; config.audio_channels = 3;
    CHECK(media_engine_create(&config, store, STORAGE - 4096, &engine) == MEDIA_ERROR_INVALID_ARGUMENT);
}
/* ---- Host-driven playback (every platform) ---- */
/* The custom wake corpus (tests/data/voice_custom_wake.bin, MVS1): label, meaning, 16 kHz int16 samples. */
#define MAX_ITEMS 256
typedef struct Utterance { char label[96]; char means[48]; uint32_t samples; float* audio; } Utterance;
static Utterance corpus[MAX_ITEMS];
static unsigned corpus_count = 0;
static float corpus_audio[24u << 20 >> 2]; /* 24 MiB of samples. */
static int LoadCorpus(const char* path) {
#if defined(_WIN32)
    /* A UTF-8 path (fopen would read it in the ANSI code page). */
    wchar_t wide[4096];
    FILE* f = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 4096) > 0 ? _wfopen(wide, L"rb") : NULL;
#else
    FILE* f = fopen(path, "rb");
#endif
    char magic[4]; uint32_t count = 0, i; size_t used = 0;
    if (!f) return 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MVS1", 4) != 0 || fread(&count, 4, 1, f) != 1) { fclose(f); return 0; }
    for (i = 0; i < count && corpus_count < MAX_ITEMS; ++i) {
        Utterance* u = &corpus[corpus_count];
        uint32_t label = 0, means = 0, samples = 0, k;
        if (fread(&label, 4, 1, f) != 1 || label >= sizeof(u->label)) break;
        if (fread(u->label, 1, label, f) != label) break;
        u->label[label] = 0;
        if (fread(&means, 4, 1, f) != 1 || means >= sizeof(u->means)) break;
        if (fread(u->means, 1, means, f) != means) break;
        u->means[means] = 0;
        if (fread(&samples, 4, 1, f) != 1 || used + samples > sizeof(corpus_audio) / sizeof(float)) break;
        u->audio = corpus_audio + used; u->samples = samples;
        for (k = 0; k < samples; ++k) { int16_t v; if (fread(&v, 2, 1, f) != 1) break; u->audio[k] = v / 32768.0f; }
        used += samples; ++corpus_count;
    }
    fclose(f);
    return corpus_count == count;
}
static const Utterance* Find(const char* label) {
    unsigned i;
    for (i = 0; i < corpus_count; ++i) if (strcmp(corpus[i].label, label) == 0) return &corpus[i];
    return NULL;
}
/* The host's text-to-speech for the custom name: the corpus's renditions by David, Zira, Hortense and Paul (never Mark or Julie,
 * the voices that speak to the engine below). */
static const char* const source_voices[4] = {"David", "Zira", "Hortense", "Paul"};
static unsigned spoken_calls = 0;
static size_t Speak(void* context, const char* text, uint32_t voice, int32_t rate, float* out, size_t capacity) {
    char label[96]; const Utterance* u; size_t n;
    (void)context; ++spoken_calls;
    if (voice >= 4 || strcmp(text, "Falcon") != 0) return 0;
    sprintf(label, "Falcon | %s rate %d", source_voices[voice], 3 * (int)rate);
    u = Find(label);
    if (!u) return 0;
    n = u->samples < capacity ? u->samples : capacity;
    memcpy(out, u->audio, n * sizeof(float));
    return n;
}
/* What the host saw. */
#define MAX_SHOWN 1024
typedef struct Shown { int64_t pts; uint32_t flags; float view_x, view_zoom; int subtitled; int64_t at; } Shown;
static Shown shown[MAX_SHOWN];
static unsigned shown_count = 0;
static int64_t virtual_now = 0;
static void OnPresent(void* context, const media_picture_t* p) {
    (void)context;
    if (shown_count < MAX_SHOWN) {
        Shown* s = &shown[shown_count++];
        s->pts = p->pts_100ns; s->flags = p->flags; s->view_x = p->view_x; s->view_zoom = p->view_zoom; s->at = virtual_now;
        s->subtitled = p->subtitle && strcmp(p->subtitle, "The quiet between the frames.") == 0;
    }
}
static media_frame_signature_t host_seen[MAX_SHOWN];
static unsigned host_seen_count = 0;
static void OnHostFrame(void* context, const media_frame_signature_t* s) { (void)context; if (host_seen_count < MAX_SHOWN) host_seen[host_seen_count++] = *s; }
/* The pumped audio, left channel, 48 kHz, for the whole run. */
#define RUN_FRAMES (48000u * 40u)
static float heard[RUN_FRAMES];
static uint32_t heard_count = 0;
static double Level(double from_s, double to_s, double hz) {
    /* Goertzel power of `hz` over [from, to) of what was pumped, in dB re a full-scale sine. */
    const uint32_t a = (uint32_t)(from_s * 48000), b = (uint32_t)(to_s * 48000) < heard_count ? (uint32_t)(to_s * 48000) : heard_count;
    const double w = 2 * 3.14159265358979323846 * hz / 48000.0, c = 2 * cos(w);
    double s1 = 0, s2 = 0, power; uint32_t i;
    if (b <= a) return -200;
    for (i = a; i < b; ++i) { const double s = heard[i] + c * s1 - s2; s2 = s1; s1 = s; }
    power = (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)(b - a) * (b - a));
    return 10 * log10(power * 4 + 1e-30);
}
static float pump_buffer[480 * 2];
static unsigned char storage_h[STORAGE];
static void TestHost(const char* fixture, const char* corpus_path) {
    media_engine_config_t config;
    media_engine_t* e = NULL;
    media_engine_stats_t stats;
    media_playback_state_t state;
    FILE* srt;
    uint64_t allocations = 0;
    int status = MEDIA_OK, paused_at_attempt = 0, attempts = 0, ended = 0;
    int64_t paused_position = -1, speed_m0 = 0, speed_m1 = 0, landing_find = -1, landing_chapter = -1;
    unsigned i, steps = 0, voice_ignored_after_avion = 0, chatter_quiet = 0;
    double volume_before = 0, volume_after = 0, volume_back = 0, eng_440 = 0, eng_4k = 0, fre_660 = 0, fre_440 = 0;
    double speed_660 = 0, speed_528 = 0, cure_before = 0, cure_after = 0, cure_undone = 0;
    unsigned scene_ok[3] = {0, 0, 0}, scene_n[3] = {0, 0, 0}, cuts = 0, subtitled = 0, tracked = 0, tracked_close = 0;
    int64_t cut_at = -1;
    float worst_view = 0;
    if (!LoadCorpus(corpus_path)) { printf("FAIL cannot read %s\n", corpus_path); ++failures; return; }
    srt = fopen("c_api_host.srt", "wb");
    if (srt) { fputs("1\r\n00:00:01,000 --> 00:00:02,500\r\nThe quiet between the frames.\r\n\r\n", srt); fclose(srt); }
    media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.voice_input = MEDIA_VOICE_HOST;
    config.wake_policy = MEDIA_WAKE_CUSTOM; strcpy(config.wake_word, "Falcon");
    config.speak = Speak; config.speak_voices = 4;
    config.on_present = OnPresent; config.on_frame = OnHostFrame; config.subtitle_path = "c_api_host.srt";
    config.audio_rate = 48000; config.audio_channels = 2;
    CHECK(media_engine_create(&config, Aligned(storage_h), STORAGE - 4096, &e) == MEDIA_OK);
    if (!e) return;
    if (media_engine_open(e, fixture) != MEDIA_OK) {
        int32_t code = 0; const uint32_t stage = media_engine_last_error(e, &code);
        printf("FAIL host open %s: stage %u\n", fixture, stage); ++failures; media_engine_destroy(e); return;
    }
    CHECK(media_engine_stats(e, &stats) == MEDIA_OK);
    printf("Host mode: arena %.1f MiB (%.1f MiB in use, modules %.1f MiB), %u threads; custom wake word \"Falcon\": %u templates (%u renditions asked of the host)\n",
           stats.arena_capacity_bytes / 1048576.0, stats.arena_used_bytes / 1048576.0, stats.module_arena_bytes / 1048576.0, stats.threads,
           stats.wake_templates, spoken_calls);
    CHECK(stats.arena_capacity_bytes == media_query_arena_budget(&config) && stats.threads == 0 && stats.wake_templates > 36 && media_wake_word_hearable("Falcon"));
    media_test_allocations_begin();
    /* The run, on a virtual clock: a step and 10 ms of audio every 10 ms. Events by virtual time (seconds). */
    for (steps = 0; steps < 4000 && !ended; ++steps) {
        const double v = steps / 100.0;
        uint32_t k;
        virtual_now = (int64_t)steps * 100000;
        status = media_engine_step(e, virtual_now);
        if (status == MEDIA_ERROR_EMPTY) { ended = 1; break; }
        if (status != MEDIA_OK) break;
        (void)media_engine_pump_audio(e, pump_buffer, 480, virtual_now);
        for (k = 0; k < 480 && heard_count < RUN_FRAMES; ++k) heard[heard_count++] = pump_buffer[2 * k];
        if (steps == 160) (void)media_engine_command(e, "volume down");
        if (steps == 280) (void)media_engine_command(e, "volume up");
        if (steps == 360) (void)media_engine_command(e, "audio in french please");
        /* Voice at 4.6 s: "Avion pause" and room talk (set aside), then Mark's "Falcon pause" until it pauses. */
        if (steps == 460 || steps == 480) {
            const Utterance* u = Find(steps == 460 ? "Avion pause | Mark rate 0" : "what time is it | Mark rate 0");
            if (u) { (void)media_engine_feed_voice(e, u->audio, u->samples); }
        }
        if (steps == 470 || steps == 490) {
            static float quiet[16000];
            (void)media_engine_feed_voice(e, quiet, 16000); /* A second of silence ends the utterance. */
            (void)media_engine_state(e, &state);
            if (!state.paused) { if (steps == 470) ++voice_ignored_after_avion; else ++chatter_quiet; }
        }
        if (steps >= 500 && steps < 560 && steps % 20 == 0 && !paused_at_attempt) {
            static const char* const renditions[3] = {"Falcon pause | Mark rate 0", "Falcon pause | Mark rate -3", "Falcon pause | Mark rate 3"};
            static float quiet[16000];
            const Utterance* u = Find(renditions[attempts]);
            ++attempts;
            if (u) { (void)media_engine_feed_voice(e, u->audio, u->samples); (void)media_engine_feed_voice(e, quiet, 16000); }
        }
        if (steps >= 501 && steps < 570 && !paused_at_attempt) {
            (void)media_engine_state(e, &state);
            if (state.paused) { paused_at_attempt = attempts; paused_position = state.position_100ns; }
        }
        if (steps == 600) { (void)media_engine_state(e, &state); if (paused_position >= 0 && state.position_100ns != paused_position) paused_position = -2; (void)media_engine_command(e, "play"); }
        if (steps == 620) (void)media_engine_command(e, "faster");
        if (steps == 640) { (void)media_engine_state(e, &state); speed_m0 = state.position_100ns; }
        if (steps == 740) { (void)media_engine_state(e, &state); speed_m1 = state.position_100ns; (void)media_engine_command(e, "slower"); }
        if (steps == 760) (void)media_engine_command(e, "find quiet between the frames");
        if (steps == 780) (void)media_engine_command(e, "noisy");
        if (steps == 1100) (void)media_engine_command(e, "\xd7\x91\xd7\x98\xd7\x9c"); /* בטל: the cure undone. */
        if (steps == 1200) (void)media_engine_command(e, "next chapter");
        if (steps == 1210) (void)media_engine_command(e, "track target");
        (void)v;
    }
    allocations = media_test_allocations_end();
    (void)media_engine_state(e, &state);
    CHECK(media_engine_stats(e, &stats) == MEDIA_OK);
    /* What the pictures said. */
    for (i = 0; i < shown_count; ++i) {
        const Shown* s = &shown[i];
        if (s->at >= 76000000 && s->at < 77000000 && (s->flags & MEDIA_PICTURE_LANDING)) landing_find = s->pts;
        if (s->at >= 120000000 && s->at < 121000000 && (s->flags & MEDIA_PICTURE_LANDING)) landing_chapter = s->pts;
        if (s->pts >= 10000000 && s->pts < 25000000 && s->subtitled) ++subtitled;
        if ((s->flags & MEDIA_PICTURE_TRACKING) && s->pts >= 62500000) {
            /* The square's centre, as the fixture draws it (it moves on the IDRs). */
            const int64_t frame = (s->pts * 24 + 5000000) / 10000000;
            const int64_t even = frame & ~1;
            const double centre = (26 + (double)((even - 144) * 16 / 72) + 6) / 64.0;
            const double truth = centre < 0.75 ? centre : 0.75; /* At 2x the view stays inside the picture. */
            const float err = (float)fabs(s->view_x - truth);
            ++tracked; if (err < 0.04f) ++tracked_close; if (err > worst_view) worst_view = err;
        }
    }
    for (i = 0; i < host_seen_count; ++i) {
        const media_frame_signature_t* s = &host_seen[i];
        static const unsigned base[3] = {24, 196, 110};
        const unsigned scene = s->pts_100ns < 30000000 ? 0u : s->pts_100ns < 60000000 ? 1u : 2u;
        if (s->flags & MEDIA_FRAME_SCENE_CUT) { ++cuts; if (cut_at < 0) cut_at = s->scene_cut_pts_100ns; }
        if (!(s->flags & MEDIA_FRAME_MEASURED) || scene == 2) continue;
        ++scene_n[scene];
        if (s->luminance_mean >= base[scene] / 255.0f - 0.01f && s->luminance_peak <= (base[scene] + 23) / 255.0f + 0.01f) ++scene_ok[scene];
    }
    volume_before = Level(0.6, 1.5, 440); volume_after = Level(1.8, 2.7, 440); volume_back = Level(3.0, 3.5, 440);
    eng_440 = Level(0.6, 1.5, 440); eng_4k = Level(0.6, 1.5, 4000);
    fre_660 = Level(3.9, 4.5, 660); fre_440 = Level(3.9, 4.5, 440);
    speed_660 = Level(6.5, 7.3, 660); speed_528 = Level(6.5, 7.3, 528);
    cure_before = Level(7.9, 8.6, 660); cure_after = Level(10.4, 10.9, 660); cure_undone = Level(11.5, 11.95, 660);
    printf("Host run: %u steps (%.2f s virtual), %s; %u pictures shown, %u dropped, %u signatures; %llu allocations while playing\n",
           steps, steps / 100.0, ended ? "to the end" : "NOT ENDED", stats.presented, stats.dropped, stats.signatures, (unsigned long long)allocations);
    printf("  pictures: scene luma right %u/%u, %u/%u; cuts %u (first at %.3f s); captions on %u pictures at 1-2.5 s\n",
           scene_ok[0], scene_n[0], scene_ok[1], scene_n[1], cuts, cut_at / 1e7, subtitled);
    printf("  English: 440 Hz %.1f dB, 4 kHz %.1f dB; \"volume down\" %+.1f dB, \"volume up\" back to %+.1f dB\n", eng_440, eng_4k,
           volume_after - volume_before, volume_back - volume_before);
    printf("  \"audio in french please\": 660 Hz %.1f dB, 440 Hz %.1f dB; track \"%.3s\"\n", fre_660, fre_440, state.audio_language);
    printf("  voice: \"Avion pause\" %s, room talk %s, Mark's \"Falcon pause\" paused on rendition %d of %d; held at %.3f s while paused%s\n",
           voice_ignored_after_avion ? "set aside" : "ACTED ON", chatter_quiet ? "set aside" : "ACTED ON", paused_at_attempt, attempts,
           paused_position / 1e7, paused_position == -2 ? " (MOVED)" : "");
    printf("  \"faster\": %.3f media seconds a second; 660 Hz %.1f dB, 528 Hz %.1f dB (pitch kept)\n", (speed_m1 - speed_m0) / 1e7, speed_660, speed_528);
    printf("  \"find quiet between the frames\" landed on %.4f s; \"noisy\" (notch killer): 660 Hz %.1f -> %.1f dB; \"undo\" -> %.1f dB\n",
           landing_find / 1e7, cure_before, cure_after, cure_undone);
    printf("  \"next chapter\" landed on %.4f s; \"track target\": %u pictures tracked after 6.25 s, %u within 0.04 of the square (worst %.3f), zoom %.1f\n",
           landing_chapter / 1e7, tracked, tracked_close, worst_view, state.view_zoom);
    CHECK(ended && allocations == 0 && stats.presented > 200 && stats.threads == 0);
    CHECK(scene_n[0] > 30 && scene_ok[0] == scene_n[0] && scene_n[1] > 5 && scene_ok[1] == scene_n[1]);
    CHECK(cut_at >= 29500000 && cut_at <= 30500000);
    CHECK(subtitled > 20);
    CHECK(eng_440 > -16 && eng_440 < -8 && eng_4k > -30 && eng_4k < -20);
    CHECK(volume_after - volume_before < -2 && volume_after - volume_before > -4.5 && fabs(volume_back - volume_before) < 0.5);
    CHECK(fre_660 > -14 && fre_440 < fre_660 - 30 && strcmp(state.audio_language, "fra") == 0);
    CHECK(voice_ignored_after_avion && chatter_quiet && paused_at_attempt > 0 && paused_position >= 0);
    CHECK(fabs((speed_m1 - speed_m0) / 1e7 - 1.25) < 0.04 && speed_660 > speed_528 + 20);
    CHECK(landing_find == 10000000);
    CHECK(cure_after < cure_before - 20 && fabs(cure_undone - cure_before) < 3);
    CHECK(landing_chapter == 60000000);
    CHECK(tracked > 50 && tracked_close == tracked && state.view_zoom == 2.0f);
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
    remove("c_api_host.srt");
}

/* ---- Live sensors (API 3): a camera and a stereo microphone pair, no file ---- */
#define SW 640u
#define SH 480u
static uint32_t Hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static double Noise2(double u, double v, uint32_t seed) {
    const double fu = floor(u), fv = floor(v), a = u - fu, b = v - fv;
    const int i = (int)fu, j = (int)fv;
#define LATTICE(x, y) ((double)(Hash32((uint32_t)(x) * 7919u + (uint32_t)(y) * 104729u + seed) % 181u))
    return (1 - a) * (1 - b) * LATTICE(i, j) + a * (1 - b) * LATTICE(i + 1, j) + (1 - a) * b * LATTICE(i, j + 1) + a * b * LATTICE(i + 1, j + 1);
#undef LATTICE
}
static uint8_t frame_y[SW * SH], frame_uv[SW * SH / 2];
/* An NV12 frame: a flat or textured static background, an optional square with its own texture centred at (cx, cy). */
static void DrawSensor(int textured, int object, double cx, double cy, double half) {
    unsigned x, y;
    for (y = 0; y < SH; ++y)
        for (x = 0; x < SW; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            double v = textured ? 40 + Noise2(px / 9.0, py / 9.0, 3) : 128;
            if (object && fabs(px - cx) < half && fabs(py - cy) < half) v = 30 + Noise2((px - cx) / half * 6 + 50, (py - cy) / half * 6 + 50, 11);
            frame_y[y * SW + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
        }
    memset(frame_uv, 128, sizeof(frame_uv));
}
typedef struct SensorShown { int64_t pts; float view_x, view_zoom; uint8_t luma0; } SensorShown;
static SensorShown sensor_shown[256];
static unsigned sensor_shown_count = 0;
static void OnSensorPresent(void* context, const media_picture_t* p) {
    (void)context;
    if (sensor_shown_count < 256) {
        SensorShown* s = &sensor_shown[sensor_shown_count++];
        s->pts = p->pts_100ns; s->view_x = p->view_x; s->view_zoom = p->view_zoom; s->luma0 = p->luma ? p->luma[0] : 0;
    }
}
static float mic[480 * 2];
static unsigned char storage_s[STORAGE];
static void TestSensors(void) {
    media_engine_config_t config;
    media_sensor_config_t sensor;
    media_sensor_frame_t frame;
    media_frame_telemetry_t t;
    media_saliency_grid_t grid;
    media_playback_state_t state;
    media_engine_t* e = NULL;
    uint64_t allocations = 0;
    unsigned step, flagged = 0, measured = 0, early_n = 0, late_n = 0, salient_frames = 0, salient_far = 0, salient_max = 0, i, grounded = 0, missed_n = 0;
    double missed[8];
    double early = 0, late = 0, hazard_sum = 0, ttc_error_sum = 0;
    float view_first = 0, view_after = 0, azimuth = 0;
    int saccade_cmd = -1, saccade_frames = 0;
    unsigned saccades = 0, sound_events = 0, ok_frames = 0, rejected = 0, next = 0;
    memset(&t, 0, sizeof(t)); memset(&grid, 0, sizeof(grid));
    media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_rate = 48000; config.audio_channels = 2;
    config.modules = MEDIA_MODULE_TELEMETRY | MEDIA_MODULE_CHAPTERS; config.on_present = OnSensorPresent;
    CHECK(media_engine_create(&config, Aligned(storage_s), STORAGE - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_init(&sensor);
    sensor.width = SW; sensor.height = SH; sensor.audio_rate = 48000; sensor.audio_channels = 2; sensor.microphone_spacing = 0.2f;
    CHECK(media_engine_open(e, "no such file.mp4") == MEDIA_ERROR_OPEN);
    if (media_engine_open_sensor(e, &sensor) != MEDIA_OK) { printf("FAIL sensor open: stage %u\n", media_engine_last_error(e, NULL)); ++failures; media_engine_destroy(e); return; }
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_ERROR_STATE);
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = SW; frame.height = SH;
    frame.planes[0] = frame_y; frame.planes[1] = frame_uv; frame.strides[0] = SW; frame.strides[1] = SW;
    /* 4 s in 10 ms steps: 480 frames of microphone audio each step, a camera frame at 30 fps, 480 frames pumped.
     * 0-2 s: an obstacle 3 m away coming at 1 m/s (its half size 50 Z0 / Z px on a flat background; TTC = Z / v).
     * 2-3 s: a textured static scene, a small object moving left in its top-left corner (saliency).
     * 3 s: "look at sound"; 3.5 s: a burst of noise on the right channel only. */
    for (step = 0; step < 400; ++step) {
        const int64_t now = (int64_t)step * 100000;
        const unsigned picture = (unsigned)((step * 30 + 99) / 100); /* Pictures due by the end of this step. */
        unsigned k;
        for (k = 0; k < 480; ++k) {
            const uint32_t n = step * 480u + k;
            float l = 0.001f * ((float)(Hash32(n * 2u) % 2001u) / 1000.0f - 1.0f), r = 0.001f * ((float)(Hash32(n * 2u + 1u) % 2001u) / 1000.0f - 1.0f);
            if (n >= 168000u && n < 168960u) r += 0.35f * ((float)(Hash32(n * 31u + 99u) % 2001u) / 1000.0f - 1.0f); /* 3.5 s, 20 ms. */
            mic[2 * k] = l; mic[2 * k + 1] = r;
        }
        if (step == 300) saccade_cmd = media_engine_command(e, "look at sound");
        media_test_allocations_begin();
        CHECK(media_engine_ingest_audio_samples(e, mic, MEDIA_SAMPLE_F32, 480, now) == MEDIA_OK);
        (void)media_engine_pump_audio(e, pump_buffer, 480, now);
        allocations += media_test_allocations_end();
        while (next < picture && next < 120) {
            const unsigned f = next++;
            const double tf = f / 30.0;
            frame.timestamp_100ns = (int64_t)f * 10000000 / 30;
            if (tf < 2.0) { const double z = 3.0 - tf; DrawSensor(0, 1, 0.55 * SW, 0.45 * SH, 50 * 3.0 / z); }
            else DrawSensor(1, tf < 3.0, 120 - 120 * (tf - 2.0), 70, 22);
            media_test_allocations_begin();
            if (media_engine_ingest_video_frame(e, &frame) == MEDIA_OK) ++ok_frames;
            (void)media_engine_step(e, now);
            t.struct_size = sizeof(t);
            (void)media_query_frame_telemetry(e, &t);
            grid.struct_size = sizeof(grid);
            (void)media_query_saliency_grid(e, &grid);
            allocations += media_test_allocations_end();
            if (tf >= 0.3 && tf < 2.0) {
                const double truth = 3.0 - tf;
                ++measured;
                if (t.flags & MEDIA_TELEMETRY_GROUND) ++grounded; /* A still camera: no ground travelled over. */
                if (!(t.flags & MEDIA_TELEMETRY_LOOMING) && missed_n < 8) missed[missed_n++] = tf;
                if (t.flags & MEDIA_TELEMETRY_LOOMING) {
                    ++flagged; ttc_error_sum += fabs(t.ttc_seconds - truth) / truth; hazard_sum += hypot(t.hazard_x - 0.55, t.hazard_y - 0.45);
                    if (tf < 0.8) { early += t.ttc_seconds; ++early_n; }
                    if (tf > 1.5) { late += t.ttc_seconds; ++late_n; }
                }
            }
            if (tf >= 2.15 && tf < 3.0) {
                /* The object (centre x 120 - 120 (t - 2), y 70, half 22) spans tiles 0-4 x 1-3 at most: nothing else is salient. */
                unsigned r, c;
                ++salient_frames; if (grid.salient_tiles > salient_max) salient_max = grid.salient_tiles;
                for (r = 0; r < 16; ++r) for (c = 0; c < 16; ++c) if ((grid.mask[r] >> c) & 1u) { if (c > 4 || r > 3) ++salient_far; }
            }
            if (tf > 3.5 && sensor_shown_count >= 1) {
                ++saccade_frames;
                if (saccade_frames == 1) view_first = sensor_shown[sensor_shown_count - 1].view_x;
                if (saccade_frames == 6) view_after = sensor_shown[sensor_shown_count - 1].view_x;
            }
        }
    }
    azimuth = t.sound_azimuth; saccades = t.saccades; sound_events = t.sound_events;
    (void)media_engine_state(e, &state);
    /* Misuse: the wrong size, a timestamp that goes back. */
    frame.width = SW / 2; rejected += media_engine_ingest_video_frame(e, &frame) == MEDIA_ERROR_INVALID_ARGUMENT;
    frame.width = SW; frame.timestamp_100ns = 0; rejected += media_engine_ingest_video_frame(e, &frame) == MEDIA_ERROR_INVALID_ARGUMENT;
    /* The other pixel formats: P010 at code 512 (8-bit 128), RGBA white and grey (BT.709 limited range: 235 and 126). */
    {
        static uint16_t p010_y[SW * SH], p010_uv[SW * SH / 2];
        static uint8_t rgba[SW * SH * 4];
        uint8_t got_p010, got_white, got_grey;
        for (i = 0; i < SW * SH; ++i) p010_y[i] = (uint16_t)(512u << 6);
        for (i = 0; i < SW * SH / 2; ++i) p010_uv[i] = (uint16_t)(512u << 6);
        frame.format = MEDIA_PIXEL_P010; frame.planes[0] = (const uint8_t*)p010_y; frame.planes[1] = (const uint8_t*)p010_uv; frame.strides[0] = frame.strides[1] = SW * 2;
        frame.timestamp_100ns = 50000000; (void)media_engine_ingest_video_frame(e, &frame); got_p010 = sensor_shown[sensor_shown_count - 1].luma0;
        memset(rgba, 255, sizeof(rgba));
        frame.format = MEDIA_PIXEL_RGBA8; frame.planes[0] = rgba; frame.planes[1] = NULL; frame.strides[0] = SW * 4;
        frame.timestamp_100ns = 50333333; (void)media_engine_ingest_video_frame(e, &frame); got_white = sensor_shown[sensor_shown_count - 1].luma0;
        memset(rgba, 128, sizeof(rgba));
        frame.timestamp_100ns = 50666666; (void)media_engine_ingest_video_frame(e, &frame); got_grey = sensor_shown[sensor_shown_count - 1].luma0;
        printf("  formats: P010 code 512 -> %u, RGBA white -> %u, RGBA 128 grey -> %u (BT.709 limited range)\n", got_p010, got_white, got_grey);
        CHECK(got_p010 == 128 && got_white == 235 && got_grey == 126);
    }
    early = early_n ? early / early_n : 0; late = late_n ? late / late_n : 0;
    printf("Live sensors: %u camera frames (640x480 NV12, 30 fps) and 4 s of stereo microphones ingested, no file; %llu allocations\n", ok_frames,
           (unsigned long long)allocations);
    printf("  approaching obstacle: looming on %u of %u frames; TTC error mean %.1f %%; TTC %.2f s early (truth 2.45) -> %.2f s late (truth 1.25); hazard %.3f from it (mean); "
           "a ground on %u (a still camera)", flagged, measured, flagged ? 100 * ttc_error_sum / flagged : 0.0, early, late, flagged ? hazard_sum / flagged : 1.0, grounded);
    for (i = 0; i < missed_n; ++i) printf("%s%.2f", i ? ", " : "; missed at ", missed[i]);
    printf("\n");
    CHECK(grounded == 0);
    printf("  saliency: %u frames with a small moving object: at most %u of 256 tiles salient, %u salient tiles away from it\n", salient_frames, salient_max, salient_far);
    printf("  \"look at sound\": %s; a burst on the right channel: azimuth %+.3f, %u sound(s), %u saccade(s); the view %.3f on the next frame, %.3f five frames later (zoom %.1f); saccade %s\n",
           saccade_cmd == MEDIA_OK ? "on" : "REFUSED", azimuth, sound_events, saccades, view_first, view_after, state.view_zoom, state.saccade ? "on" : "off");
    CHECK(ok_frames == 120 && allocations == 0 && rejected == 2);
    CHECK(measured > 40 && flagged >= measured * 9 / 10 && ttc_error_sum / (flagged ? flagged : 1) < 0.15 && early > late + 1.0 && hazard_sum / (flagged ? flagged : 1) < 0.04);
    CHECK(salient_frames > 20 && salient_max > 0 && salient_max < 26 && salient_far == 0);
    CHECK(saccade_cmd == MEDIA_OK && state.saccade && azimuth > 0.9f && sound_events == 1 && saccades == 1 && view_first > 0.6f && view_after >= 0.74f && state.view_zoom == 2.0f);
    CHECK(media_engine_set_audio_saccade(e, 0) == MEDIA_OK && media_engine_state(e, &state) == MEDIA_OK && !state.saccade);
    /* Stabilisation (API 4): a sudden turn of the camera (the IMU's yaw) moves the view with it; without MEDIA_SENSOR_GPU there is
     * no GPU image and no picture controls. */
    {
        media_frame_telemetry_t t0, t1; media_gpu_image_t image; media_picture_controls_t controls;
        CHECK(media_engine_set_stabilization(e, 1) == MEDIA_OK);
        memset(&t0, 0, sizeof(t0)); t0.struct_size = sizeof(t0); t1 = t0;
        frame.flags = MEDIA_SENSOR_IMU; frame.imu_yaw = 0; frame.imu_pitch = 0; frame.imu_roll = 0;
        frame.timestamp_100ns = 51000000; (void)media_engine_ingest_video_frame(e, &frame); (void)media_query_frame_telemetry(e, &t0);
        frame.imu_yaw = 0.02f; frame.timestamp_100ns = 51333333; (void)media_engine_ingest_video_frame(e, &frame); (void)media_query_frame_telemetry(e, &t1);
        memset(&image, 0, sizeof(image)); image.struct_size = sizeof(image); media_picture_controls_init(&controls);
        printf("  stabilisation: a 0.02 rad turn to the right moves the view from %.4f to %.4f (zoom %.2f, %s)\n", t0.view_x, t1.view_x, t1.view_zoom,
               (t1.flags & MEDIA_TELEMETRY_STABILIZED) ? "flagged" : "NOT FLAGGED");
        CHECK((t1.flags & MEDIA_TELEMETRY_STABILIZED) && t1.view_zoom >= 1.25f && t1.view_x < t0.view_x - 0.005f && t1.view_x > t0.view_x - 0.02f);
        CHECK(media_query_gpu_image(e, &image) == MEDIA_ERROR_UNSUPPORTED && media_engine_set_picture_controls(e, &controls) == MEDIA_ERROR_UNSUPPORTED);
        /* Roll (API 5): a sudden 0.03 rad roll clockwise turns the scene counter-clockwise; the view turns with it (its fast
         * part: 0.03 less a 1/30 s step of the 0.35 s low-pass). */
        {
            media_frame_telemetry_t t2;
            memset(&t2, 0, sizeof(t2)); t2.struct_size = sizeof(t2);
            frame.imu_yaw = 0; frame.imu_roll = 0.03f; frame.timestamp_100ns = 51666666; (void)media_engine_ingest_video_frame(e, &frame);
            (void)media_query_frame_telemetry(e, &t2);
            printf("  roll: a 0.03 rad roll turns the view by %+.4f rad (API %u)\n", t2.view_roll, (unsigned)MEDIA_API_VERSION);
            CHECK(MEDIA_API_VERSION >= 5u && t2.view_roll < -0.02f && t2.view_roll > -0.035f && (t2.flags & MEDIA_TELEMETRY_STABILIZED));
            frame.imu_roll = 0;
        }
    }
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}

/* ---- Rotation, the horizon and black bars (API 6): host mode, a camera ---- */
/* A scene as a camera rolled `tilt` degrees clockwise sees it (4 x 4 samples a pixel): sky over a horizon at the middle, poles
 * standing on it, furrows in the ground; then `bars` rows of black at the top and the bottom (a 16:9 picture letterboxed in 4:3). */
static void DrawTilted(double tilt, unsigned bars) {
    const double r = tilt * 3.14159265358979323846 / 180, co = cos(r), si = sin(r), cx = SW / 2.0, cy = SH / 2.0;
    unsigned x, y;
    int k;
    for (y = 0; y < SH; ++y)
        for (x = 0; x < SW; ++x) {
            double sum = 0;
            for (k = 0; k < 16; ++k) {
                const double px = x + 0.125 + 0.25 * (k & 3) - cx, py = y + 0.125 + 0.25 * (k >> 2) - cy;
                const double u = co * px + si * py + cx, v = -si * px + co * py + cy; /* The upright scene. */
                double value = v < cy ? 190 : (fmod(v - cy, 30.0) < 5 ? 55 : 95);
                if (v < cy && v > cy - 110 && (fmod(u, 120.0) < 14)) value = 40;
                sum += value;
            }
            frame_y[y * SW + x] = (uint8_t)(y < bars || y >= SH - bars ? 16 : sum / 16 + 0.5);
        }
    memset(frame_uv, 128, sizeof(frame_uv));
}
typedef struct OrientedShown { uint32_t quarter_turns; int32_t mirrored; float view_level, level_zoom, view_zoom, view_y; } OrientedShown;
static OrientedShown oriented_last;
static unsigned oriented_count = 0;
static void OnOrientedPresent(void* context, const media_picture_t* p) {
    (void)context; ++oriented_count;
    oriented_last.quarter_turns = p->quarter_turns; oriented_last.mirrored = p->mirrored; oriented_last.view_level = p->view_level;
    oriented_last.level_zoom = p->level_zoom; oriented_last.view_zoom = p->view_zoom; oriented_last.view_y = p->view_y;
}
static unsigned char storage_o[STORAGE];
static void TestOrientation(void) {
    media_engine_config_t config;
    media_sensor_config_t sensor;
    media_sensor_frame_t frame;
    media_frame_telemetry_t t;
    media_playback_state_t state;
    media_engine_t* e = NULL;
    uint64_t allocations = 0;
    unsigned f;
    const double degrees = 180 / 3.14159265358979323846;
    union { media_frame_telemetry_t t; unsigned char bytes[sizeof(media_frame_telemetry_t)]; } api5;
    media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY; config.on_present = OnOrientedPresent;
    CHECK(media_engine_create(&config, Aligned(storage_o), STORAGE - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_init(&sensor);
    sensor.width = SW; sensor.height = SH;
    if (media_engine_open_sensor(e, &sensor) != MEDIA_OK) { printf("FAIL orientation sensor open: stage %u\n", media_engine_last_error(e, NULL)); ++failures; media_engine_destroy(e); return; }
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = SW; frame.height = SH;
    frame.planes[0] = frame_y; frame.planes[1] = frame_uv; frame.strides[0] = SW; frame.strides[1] = SW;
    /* Auto-leveling by the call, auto-crop by the command (French); 60 frames at 30 a second of the camera rolled +4.5 degrees,
     * its picture letterboxed (60 rows each side). */
    CHECK(media_engine_set_auto_level(e, 1) == MEDIA_OK);
    CHECK(media_engine_command(e, "recadrer les bandes noires") == MEDIA_OK);
    DrawTilted(4.5, 60);
    for (f = 0; f < 60; ++f) {
        frame.timestamp_100ns = (int64_t)f * 10000000 / 30;
        media_test_allocations_begin();
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        (void)media_engine_step(e, frame.timestamp_100ns);
        memset(&t, 0, sizeof(t)); t.struct_size = sizeof(t);
        (void)media_query_frame_telemetry(e, &t);
        allocations += media_test_allocations_end();
    }
    CHECK(media_engine_state(e, &state) == MEDIA_OK);
    printf("Orientation (API %u): the horizon %s %+.2f degrees, level %+.1f; bars %s, active (%u, %u)-(%u, %u); the picture shown with %u quarter turns, "
           "level %+.2f degrees (zoom %.3f), view zoom %.3f at %.3f; state %#x %+.1f; %llu allocations\n", (unsigned)MEDIA_API_VERSION,
           (t.flags & MEDIA_TELEMETRY_HORIZON) ? "at" : "NOT FOUND,", t.estimated_horizon_deg, t.level_degrees, (t.flags & MEDIA_TELEMETRY_BARS) ? "found" : "NOT FOUND",
           t.active_left, t.active_top, t.active_right, t.active_bottom, oriented_last.quarter_turns, oriented_last.view_level * degrees, oriented_last.level_zoom,
           oriented_last.view_zoom, oriented_last.view_y, state.orientation, state.level_degrees, (unsigned long long)allocations);
    CHECK(MEDIA_API_VERSION == 17u && (t.flags & MEDIA_TELEMETRY_HORIZON) && fabs(t.estimated_horizon_deg - 4.5) <= 0.35 && fabs(t.level_degrees + 4.5) <= 0.35);
    CHECK((t.flags & MEDIA_TELEMETRY_BARS) && t.active_left <= 2 && t.active_top >= 58 && t.active_top <= 62 && t.active_right >= SW - 2 && t.active_bottom >= SH - 62 &&
          t.active_bottom <= SH - 58);
    CHECK(oriented_last.quarter_turns == 0 && !oriented_last.mirrored && fabs(oriented_last.view_level * degrees + 4.5) <= 0.35 && oriented_last.level_zoom > 1.05f &&
          fabs(oriented_last.view_zoom - (double)SH / (SH - 124)) <= 0.02 && fabs(oriented_last.view_y - 0.5) <= 0.01);
    CHECK(state.orientation == (MEDIA_ORIENTATION_AUTO_LEVEL | MEDIA_ORIENTATION_AUTO_CROP) && fabs(state.level_degrees + 4.5) <= 0.35 && allocations == 0);
    /* API 5's telemetry struct is still accepted: what lies past its 112 bytes is not written. */
    memset(api5.bytes, 0xA5, sizeof(api5.bytes)); api5.t.struct_size = 112;
    CHECK(media_query_frame_telemetry(e, &api5.t) == MEDIA_OK && api5.t.struct_size == 112 && api5.bytes[112] == 0xA5 && api5.bytes[sizeof(api5.bytes) - 1] == 0xA5);
    /* And API 6's (136 bytes, through reserved): API 7's objects past it are not written. */
    memset(api5.bytes, 0xA5, sizeof(api5.bytes)); api5.t.struct_size = 136;
    CHECK(media_query_frame_telemetry(e, &api5.t) == MEDIA_OK && api5.t.struct_size == 136 && api5.bytes[136] == 0xA5 && api5.bytes[sizeof(api5.bytes) - 1] == 0xA5);
    /* And API 7's and 8's (400 bytes, through the objects): API 9's vibrometry past it is not written. */
    memset(api5.bytes, 0xA5, sizeof(api5.bytes)); api5.t.struct_size = 400;
    CHECK(media_query_frame_telemetry(e, &api5.t) == MEDIA_OK && api5.t.struct_size == 400 && api5.bytes[400] == 0xA5 && api5.bytes[sizeof(api5.bytes) - 1] == 0xA5);
    /* And API 9's (448 bytes, through the vibrometry region): API 10's virtual IMU past it is not written. */
    memset(api5.bytes, 0xA5, sizeof(api5.bytes)); api5.t.struct_size = 448;
    CHECK(media_query_frame_telemetry(e, &api5.t) == MEDIA_OK && api5.t.struct_size == 448 && api5.bytes[448] == 0xA5 && api5.bytes[sizeof(api5.bytes) - 1] == 0xA5);
    CHECK(sizeof(media_frame_telemetry_t) == 136 + 8 + 64 * MEDIA_MAX_OBJECTS + 48 + 24 && sizeof(media_detected_object_t) == 64);
    /* API 10 from C99: the virtual IMU's fields and flags (these frames carry no IMU deltas: a still picture, no motion). */
    CHECK(MEDIA_TELEMETRY_VIRTUAL_IMU == 8192u && MEDIA_VIBROMETRY_FOLLOWING == 1u && t.ego_tiles <= 48u &&
          (!(t.flags & MEDIA_TELEMETRY_VIRTUAL_IMU) || (fabs(t.ego_dx) < 0.05 && fabs(t.ego_dy) < 0.05 && fabs(t.ego_turn) < 0.001)));
    /* API 9 from C99: the event camera's, the tensor's and the inspector's structs and calls (suites #202-#204 test them). */
    {
        media_event_camera_config_t ec; media_tensor_request_t tr; media_bitstream_info_t bi;
        static media_bitstream_inspector_t inspector;
        static const uint8_t parameter_sets[] = {0, 0, 0, 2, 0x09, 0xF0}; /* An access unit delimiter alone: no picture. */
        media_event_camera_config_init(&ec); media_tensor_request_init(&tr);
        memset(&bi, 0, sizeof(bi)); bi.struct_size = sizeof(bi);
        CHECK(sizeof(media_event_spike_t) == 16 && ec.contrast_threshold == 0.15f && media_tensor_bytes(&tr) == 3u * 224 * 224 * 4);
        CHECK(media_bitstream_inspector_init(&inspector, MEDIA_CODEC_H264, NULL, 0, 4) == MEDIA_OK &&
              media_inspect_bitstream_packet(&inspector, parameter_sets, sizeof(parameter_sets), &bi) == MEDIA_ERROR_EMPTY && bi.picture_type == MEDIA_PICTURE_TYPE_NONE);
    }
    /* API 7: no object with that id in this still scene. */
    CHECK(media_engine_track_object(e, 9999u) == MEDIA_ERROR_EMPTY && media_engine_track_object(NULL, 0) == MEDIA_ERROR_INVALID_ARGUMENT);
    api5.t.struct_size = 100;
    CHECK(media_query_frame_telemetry(e, &api5.t) == MEDIA_ERROR_INVALID_ARGUMENT);
    /* The commands host mode used to refuse: a quarter turn and back, straight again, the viewer's own turn by the call. */
    CHECK(media_engine_command(e, "rotate video") == MEDIA_OK);
    frame.timestamp_100ns += 333333; (void)media_engine_ingest_video_frame(e, &frame);
    CHECK(oriented_last.quarter_turns == 1);
    CHECK(media_engine_command(e, "\xd7\xa1\xd7\x95\xd7\x91\xd7\x91 \xd7\xa9\xd7\x9e\xd7\x90\xd7\x9c\xd7\x94") == MEDIA_OK); /* "rotate left" in Hebrew. */
    frame.timestamp_100ns += 333333; (void)media_engine_ingest_video_frame(e, &frame);
    CHECK(oriented_last.quarter_turns == 0);
    CHECK(media_engine_set_orientation(e, 2, 3.0f) == MEDIA_OK && media_engine_set_orientation(e, 0, 20.0f) == MEDIA_ERROR_INVALID_ARGUMENT);
    frame.timestamp_100ns += 333333; (void)media_engine_ingest_video_frame(e, &frame);
    CHECK(media_engine_state(e, &state) == MEDIA_OK);
    CHECK(oriented_last.quarter_turns == 2 && fabs(oriented_last.view_level * degrees - 3.0) <= 0.01 && (state.orientation & MEDIA_ORIENTATION_TURNS) == 2 &&
          !(state.orientation & MEDIA_ORIENTATION_AUTO_LEVEL) && state.level_degrees == 3.0f);
    CHECK(media_engine_command(e, "straighten") == MEDIA_OK && media_engine_command(e, "show black bars") == MEDIA_OK);
    frame.timestamp_100ns += 333333; (void)media_engine_ingest_video_frame(e, &frame);
    CHECK(media_engine_state(e, &state) == MEDIA_OK);
    printf("  rotate video, rotate left, set_orientation(2, 3.0), straighten, show black bars: turns %u, level %+.1f, view zoom %.2f\n",
           oriented_last.quarter_turns, oriented_last.view_level * degrees, oriented_last.view_zoom);
    CHECK(oriented_last.view_level == 0 && oriented_last.level_zoom == 1.0f && oriented_last.view_zoom == 1.0f && !(state.orientation & MEDIA_ORIENTATION_AUTO_CROP));
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}


#if defined(_WIN32)
/* ---- Playback (Windows) ---- */
static unsigned char storage_b[STORAGE];
#define MAX_SIGNATURES 2048
static media_frame_signature_t seen[MAX_SIGNATURES];
static unsigned seen_count = 0;
struct Script { media_engine_t* engine; unsigned step; int results[8]; };
static struct Script script;
static void OnFrame(void* context, const media_frame_signature_t* s) {
    struct Script* run = (struct Script*)context;
    if (seen_count < MAX_SIGNATURES) seen[seen_count++] = *s;
    /* At 1.5 s the wake word and a cure, one a picture (the callback may give commands). */
    if (s->pts_100ns < 15000000 || run->step >= 6) return;
    switch (run->step) {
    case 0: run->results[0] = media_engine_hear(run->engine, "volume down"); break;          /* No name: set aside. */
    case 1: run->results[1] = media_engine_hear(run->engine, "avion volume down"); break;    /* Not the name now. */
    case 2: run->results[2] = media_engine_hear(run->engine, "Jarvis, volume down"); break;  /* The name. */
    case 3: run->results[3] = media_engine_command(run->engine, "\xd7\x97\xd7\xa9\xd7\x95\xd7\x9a"); break; /* חשוך */
    case 4: run->results[4] = media_engine_command(run->engine, "\xd7\x91\xd7\x98\xd7\x9c"); break;         /* בטל */
    case 5: run->results[5] = media_engine_command(run->engine, "\xd7\x91\xd7\x98\xd7\x9c"); break;         /* Nothing left. */
    }
    ++run->step;
}
static unsigned Scene(int64_t pts) { return pts < 30000000 ? 0u : pts < 60000000 ? 1u : 2u; }

static void TestPlayback(const char* fixture) {
    static const unsigned scene_luma[3] = {24, 196, 110};
    media_engine_config_t config;
    media_engine_t *a = NULL, *b = NULL;
    media_engine_stats_t sa, sb, after;
    media_frame_signature_t polled;
    unsigned measured = 0, cuts = 0, dominant = 0, speechless = 0, audible = 0, i, in_scene[3] = {0, 0, 0}, luma_ok[3] = {0, 0, 0};
    unsigned repeats[3] = {0, 0, 0}, polls = 0, polls_match = 0;
    int64_t cut_at[4] = {0, 0, 0, 0}, confirmed[4] = {0, 0, 0, 0};
    float cut_motion[4] = {0, 0, 0, 0};
    uint64_t allocations;
    FILE* srt = fopen("c_api_tests.srt", "wb");
    if (srt) { fputs("1\r\n00:00:00,500 --> 00:00:02,000\r\nThe quiet between the frames.\r\n\r\n", srt); fclose(srt); }
    media_engine_config_init(&config);
    config.visible = 0; config.wake_policy = MEDIA_WAKE_CUSTOM; strcpy(config.wake_word, "Jarvis");
    config.on_frame = OnFrame; config.on_frame_context = &script; config.subtitle_path = "c_api_tests.srt";
    CHECK(media_engine_create(&config, Aligned(storage_a), STORAGE - 4096, &a) == MEDIA_OK);
    if (!a) return;
    script.engine = a;
    if (media_engine_open(a, fixture) != MEDIA_OK) {
        int32_t code = 0; const uint32_t stage = media_engine_last_error(a, &code);
        printf("FAIL open %s: stage %u, 0x%08lx\n", fixture, stage, (unsigned long)(uint32_t)code); ++failures; media_engine_destroy(a); return;
    }
    CHECK(media_engine_stats(a, &sa) == MEDIA_OK);
    media_test_allocations_begin();
    CHECK(media_engine_run(a) == MEDIA_OK);
    allocations = media_test_allocations_end();
    CHECK(media_engine_stats(a, &after) == MEDIA_OK);
    while (media_engine_poll_frame(a, &polled) == MEDIA_OK) {
        ++polls;
        if (polled.sequence < seen_count && memcmp(&polled, &seen[polled.sequence], sizeof(polled)) == 0) ++polls_match;
    }
    printf("Engine (all modules): arena %.1f MiB (modules %.1f MiB), %.1f MiB in use, %u threads; %u pictures, %u dropped, %u signatures (%u lost), "
           "%u polled; %llu allocations while running\n", sa.arena_capacity_bytes / 1048576.0, sa.module_arena_bytes / 1048576.0,
           sa.arena_used_bytes / 1048576.0, sa.threads, after.presented, after.dropped, after.signatures, after.signatures_lost, polls,
           (unsigned long long)allocations);
    {
        media_engine_config_t core = config; core.modules = 0;
        CHECK(sa.arena_capacity_bytes == media_query_arena_budget(&config) && sa.arena_capacity_bytes - sa.module_arena_bytes == media_query_arena_budget(&core));
    }
    CHECK(after.presented > 200 && after.signatures == seen_count && polls > 0 && polls == polls_match && allocations == 0);
    /* The signatures against the fixture. */
    for (i = 0; i < seen_count; ++i) {
        const media_frame_signature_t* s = &seen[i];
        const unsigned scene = Scene(s->pts_100ns);
        if (s->flags & MEDIA_FRAME_SCENE_CUT) { if (cuts < 4) { cut_at[cuts] = s->scene_cut_pts_100ns; confirmed[cuts] = s->pts_100ns; } ++cuts; }
        if (!(s->flags & MEDIA_FRAME_MEASURED)) continue;
        ++measured; ++in_scene[scene];
        if (s->luminance_mean >= scene_luma[scene] / 255.0f - 0.01f && s->luminance_mean <= (scene_luma[scene] + 23) / 255.0f + 0.01f &&
            s->luminance_peak >= s->luminance_mean && s->luminance_peak <= (scene_luma[scene] + 23) / 255.0f + 0.01f) ++luma_ok[scene];
        if (s->tile_motion == 0.0f) ++repeats[scene];
        if (s->pts_100ns >= 10000000) {
            ++audible;
            if (s->dominant_band == 4 && s->dominant_band_hz == 500.0f) ++dominant;
            if (s->speech_probability >= 0.0f && s->speech_probability < 0.3f) ++speechless;
        }
    }
    printf("Signatures: %u measured of %u; luma within the scene's codes %u/%u, %u/%u, %u/%u; repeated pictures (motion 0) %u, %u, %u; "
           "%u cuts at %.3f s and %.3f s; 500 Hz dominant %u/%u, no speech %u/%u\n",
           measured, seen_count, luma_ok[0], in_scene[0], luma_ok[1], in_scene[1], luma_ok[2], in_scene[2], repeats[0], repeats[1], repeats[2],
           cuts, cut_at[0] / 1e7, cut_at[1] / 1e7, dominant, audible, speechless, audible);
    CHECK(measured * 10 >= seen_count * 9);
    for (i = 0; i < 3; ++i) CHECK(in_scene[i] > 40 && luma_ok[i] == in_scene[i] && repeats[i] * 10 >= in_scene[i] * 4);
    for (i = 0; i < seen_count; ++i) { /* The cut's own picture: every block moved. */
        if (cuts > 0 && seen[i].pts_100ns == cut_at[0]) cut_motion[0] = seen[i].tile_motion;
        if (cuts > 1 && seen[i].pts_100ns == cut_at[1]) cut_motion[1] = seen[i].tile_motion;
    }
    printf("  cuts confirmed on the pictures at %.3f s and %.3f s; motion of the cut pictures %.2f, %.2f\n", confirmed[0] / 1e7, confirmed[1] / 1e7,
           cut_motion[0], cut_motion[1]);
    CHECK(cuts == 2 && cut_at[0] >= 29500000 && cut_at[0] <= 30500000 && cut_at[1] >= 59500000 && cut_at[1] <= 60500000);
    CHECK(confirmed[0] > cut_at[0] && confirmed[0] - cut_at[0] <= 2500000 && confirmed[1] > cut_at[1] && confirmed[1] - cut_at[1] <= 2500000);
    CHECK(cut_motion[0] > 0.95f && cut_motion[1] > 0.95f);
    CHECK(audible > 0 && dominant * 10 >= audible * 9 && speechless * 10 >= audible * 9);
    /* The wake word and the cure, as the callback gave them. */
    printf("Commands: \"volume down\" %d, \"avion volume down\" %d, \"Jarvis, volume down\" %d, cure %d, undo %d, undo again %d; wake ignored %u\n",
           script.results[0], script.results[1], script.results[2], script.results[3], script.results[4], script.results[5], after.wake_ignored);
    CHECK(script.step == 6 && script.results[0] == MEDIA_ERROR_REFUSED && script.results[1] == MEDIA_ERROR_REFUSED && script.results[2] == MEDIA_OK);
    CHECK(script.results[3] == MEDIA_OK && script.results[4] == MEDIA_OK && script.results[5] == MEDIA_ERROR_REFUSED && after.wake_ignored == 2);
    CHECK(media_engine_close(a) == MEDIA_OK);
    /* Without the HUD, subtitles, chapters, voice and telemetry: exactly their regions and threads fewer. */
    {
        const uint32_t left_out = MEDIA_MODULE_HUD | MEDIA_MODULE_SUBTITLES | MEDIA_MODULE_CHAPTERS | MEDIA_MODULE_VOICE | MEDIA_MODULE_TELEMETRY;
        uint64_t regions = 0; uint32_t threads = 0; unsigned m;
        media_module_budget_t budget;
        for (m = 0; m < 6; ++m) if (left_out & (1u << m)) { (void)media_query_module_budget(1u << m, &budget); regions += budget.arena_bytes; }
        /* The command bar's and the status cues' text threads, the subtitle shaper (voice input is off); telemetry's perception thread is
           not started for this fixture's picture, under the 128 cells a side perception needs. */
        threads = 2 + 1;
        media_engine_config_init(&config);
        config.visible = 0; config.modules = MEDIA_MODULE_ALL & ~left_out; config.maximum_seconds = 2; config.subtitle_path = "c_api_tests.srt";
        config.wake_policy = MEDIA_WAKE_DISABLED;
        CHECK(media_engine_create(&config, Aligned(storage_b), STORAGE - 4096, &b) == MEDIA_OK);
        if (!b) return;
        CHECK(media_engine_open(b, fixture) == MEDIA_OK);
        CHECK(media_engine_stats(b, &sb) == MEDIA_OK);
        printf("Engine (no HUD, subtitles, chapters, voice, telemetry): arena %.1f MiB, %.1f MiB in use, %u threads; less by %.1f MiB, %.1f MiB, %u threads"
               " (their regions: %.1f MiB, threads %u)\n", sb.arena_capacity_bytes / 1048576.0, sb.arena_used_bytes / 1048576.0, sb.threads,
               (sa.arena_capacity_bytes - sb.arena_capacity_bytes) / 1048576.0, (sa.arena_used_bytes - sb.arena_used_bytes) / 1048576.0,
               sa.threads - sb.threads, regions / 1048576.0, threads);
        CHECK(sb.arena_capacity_bytes == media_query_arena_budget(&config));
        CHECK(sa.arena_capacity_bytes - sb.arena_capacity_bytes == regions && sa.arena_used_bytes - sb.arena_used_bytes == regions);
        CHECK(sa.threads - sb.threads == threads);
        /* Push-to-talk: an utterance only while talk is held. */
        media_engine_set_talk(b, 0);
        CHECK(media_engine_hear(b, "volume down") == MEDIA_ERROR_REFUSED);
        media_engine_set_talk(b, 1);
        CHECK(media_engine_hear(b, "volume down") == MEDIA_OK);
        media_engine_set_talk(b, 0);
        CHECK(media_engine_run(b) == MEDIA_OK);
        CHECK(media_engine_stats(b, &sb) == MEDIA_OK);
        printf("  played %u pictures, %u signatures; poll %s\n", sb.presented, sb.signatures, media_engine_poll_frame(b, &polled) == MEDIA_ERROR_EMPTY ? "empty" : "NOT EMPTY");
        CHECK(sb.presented > 30 && sb.signatures == 0 && media_engine_poll_frame(b, &polled) == MEDIA_ERROR_EMPTY);
        CHECK(media_engine_close(b) == MEDIA_OK);
        media_engine_destroy(b);
    }
    media_engine_destroy(a);
    remove("c_api_tests.srt");
}
#endif

int main(int argc, char** argv) {
    static char args[4][4096];
    int i;
    TestPortable();
#if defined(_WIN32)
    {
        /* The SDK takes UTF-8; argv is in the ANSI code page ("Développement" would not survive it): the wide command line. */
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        for (i = 1; i < argc && i < 4 && wide && i < count; ++i) (void)WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], (int)sizeof(args[i]), NULL, NULL);
        if (wide) LocalFree(wide);
    }
#else
    for (i = 1; i < argc && i < 4; ++i) { strncpy(args[i], argv[i], sizeof(args[i]) - 1); }
#endif
    (void)argv;
    TestSensors();
    TestOrientation();
    if (argc > 2) TestHost(args[1], args[2]);
    else { printf("FAIL no portable fixture and corpus given\n"); ++failures; }
#if defined(_WIN32)
    if (argc > 3) TestPlayback(args[3]);
    else { printf("FAIL no chapters fixture given\n"); ++failures; }
#endif
    printf("C SDK: %d failures\n", failures);
    return failures ? 1 : 0;
}
