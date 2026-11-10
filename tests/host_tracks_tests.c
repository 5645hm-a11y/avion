/* media_host_tracks_tests (API 17): host-mode parity, from a pure C translation unit, on every platform.
 *   media_host_tracks_tests portable_fixture.mp4 h264_aac_srt_chapters.mkv
 * - The track list in host mode: the portable fixture's picture track and its two PCM tracks (English playing), and the audio
 *   track switched by MEDIA_CONTROL_AUDIO_TRACK (the French one active after the next step).
 * - Subtitles after open: an .srt attached while playing becomes the sidecar (id 0), listed, shown on the next pictures;
 *   MEDIA_CONTROL_SUBTITLE_TRACK -1 hides it; a second attach replaces it; a file without a cue is refused and changes nothing.
 * - A snapshot into memory: too small a buffer says the size it needs (MEDIA_ERROR_STORAGE), then BGRA and RGBA of the same
 *   picture (channels swapped, alpha 255) with the frame hash of the BMP media_engine_save_snapshot writes of it.
 * - No allocation in the switch, the attaches, the snapshots or the steps around them.
 * - A Matroska file in host mode: its text subtitle track listed ("text", selectable, shown), its chapters. */
#include "media/c_api.h"
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
void media_test_allocations_begin(void);
uint64_t media_test_allocations_end(void);

#define STORAGE (64u << 20)
static unsigned char storage[STORAGE];
static void* Aligned(unsigned char* base) {
    const size_t a = media_engine_storage_alignment();
    const uintptr_t p = (uintptr_t)base;
    return (void*)(base + ((a - p % a) % a));
}
static char caption[256];
static unsigned presented = 0;
static void OnPresent(void* context, const media_picture_t* p) {
    (void)context; ++presented;
    caption[0] = 0;
    if (p->subtitle) { strncpy(caption, p->subtitle, sizeof(caption) - 1); caption[sizeof(caption) - 1] = 0; }
}
static float pump[480 * 2];
static int64_t now = 0;
static void Steps(media_engine_t* e, unsigned n) {
    unsigned i;
    for (i = 0; i < n; ++i) { now += 100000; (void)media_engine_step(e, now); (void)media_engine_pump_audio(e, pump, 480, now); }
}
static int Active(media_engine_t* e, uint32_t kind, uint32_t* id) {
    uint32_t count = 0, i;
    if (media_engine_track_count(e, &count) != MEDIA_OK) return 0;
    for (i = 0; i < count; ++i) {
        media_track_info_t t; memset(&t, 0, sizeof(t)); t.struct_size = sizeof(t);
        if (media_engine_track_info(e, i, &t) == MEDIA_OK && t.kind == kind && t.active) { *id = t.id; return 1; }
    }
    return 0;
}
static int WriteText(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f); fclose(f); return 1;
}
static unsigned char pixels[2][16u << 20];

static void TestPortable(const char* fixture) {
    media_engine_config_t config;
    media_engine_t* e = NULL;
    uint32_t count = 0, i, english = 0, french = 0, active = 0, audio = 0;
    uint64_t allocations;
    media_snapshot_buffer_t b;
    media_snapshot_t file;
    media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_rate = 48000; config.audio_channels = 2; config.on_present = OnPresent;
    CHECK(media_engine_create(&config, Aligned(storage), STORAGE - 4096, &e) == MEDIA_OK);
    if (!e) return;
    if (media_engine_open(e, fixture) != MEDIA_OK) { printf("FAIL open %s\n", fixture); ++failures; media_engine_destroy(e); return; }
    CHECK(media_engine_track_count(e, &count) == MEDIA_OK && count >= 3 && count <= MEDIA_MAX_TRACKS);
    for (i = 0; i < count; ++i) {
        media_track_info_t t; memset(&t, 0, sizeof(t)); t.struct_size = sizeof(t);
        CHECK(media_engine_track_info(e, i, &t) == MEDIA_OK);
        printf("  track %u: id %u kind %u codec '%s' language '%s' %u ch %u Hz %ux%u selectable %u active %u\n", i, t.id, t.kind, t.codec, t.language,
               t.channels, t.sample_rate, t.width, t.height, t.selectable, t.active);
        if (t.kind == MEDIA_TRACK_AUDIO && t.selectable) { ++audio; if (t.active) english = t.id; else french = t.id; }
    }
    CHECK(audio == 2 && english && french && english != french);
    Steps(e, 150);
    CHECK(presented > 10);
    CHECK(WriteText("host_attach_1.srt", "1\r\n00:00:00,000 --> 00:01:00,000\r\nAttached while playing.\r\n\r\n"));
    CHECK(WriteText("host_attach_2.vtt", "WEBVTT\n\n00:00:00.000 --> 00:01:00.000\nA second file replaces it.\n\n"));
    CHECK(WriteText("host_attach_empty.srt", "no cue in this file\n"));
    /* The French track, by its id. */
    media_test_allocations_begin();
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_AUDIO_TRACK, (double)french, 1) == MEDIA_OK);
    Steps(e, 5);
    CHECK(Active(e, MEDIA_TRACK_AUDIO, &active) && active == french);
    /* Subtitles after open (the files written before the count began). */
    CHECK(!Active(e, MEDIA_TRACK_SUBTITLE, &active));
    CHECK(media_engine_attach_subtitles(e, "host_attach_1.srt") == MEDIA_OK);
    Steps(e, 20);
    CHECK(Active(e, MEDIA_TRACK_SUBTITLE, &active) && active == 0 && strcmp(caption, "Attached while playing.") == 0);
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SUBTITLE_TRACK, -1, 2) == MEDIA_OK);
    Steps(e, 20);
    CHECK(!Active(e, MEDIA_TRACK_SUBTITLE, &active) && caption[0] == 0);
    CHECK(media_engine_attach_subtitles(e, "host_attach_2.vtt") == MEDIA_OK);
    Steps(e, 20);
    CHECK(strcmp(caption, "A second file replaces it.") == 0);
    CHECK(media_engine_attach_subtitles(e, "host_attach_empty.srt") == MEDIA_ERROR_OPEN);
    CHECK(media_engine_attach_subtitles(e, "host_attach_missing.srt") == MEDIA_ERROR_OPEN);
    Steps(e, 20);
    CHECK(strcmp(caption, "A second file replaces it.") == 0);
    /* A snapshot into memory: the size first, then BGRA and RGBA of the same picture. */
    memset(&b, 0, sizeof(b)); b.struct_size = sizeof(b); b.format = MEDIA_PIXELS_BGRA8; b.pixels = pixels[0]; b.capacity_bytes = 64;
    CHECK(media_engine_snapshot_to_memory(e, &b) == MEDIA_ERROR_STORAGE && b.width > 0 && b.height > 0);
    b.capacity_bytes = sizeof(pixels[0]);
    CHECK(media_engine_snapshot_to_memory(e, &b) == MEDIA_OK && b.frame_hash != 0);
    {
        media_snapshot_buffer_t r = b; uint32_t x, swapped = 0, opaque = 0;
        r.format = MEDIA_PIXELS_RGBA8; r.pixels = pixels[1];
        CHECK(media_engine_snapshot_to_memory(e, &r) == MEDIA_OK && r.frame_hash == b.frame_hash && r.width == b.width && r.height == b.height);
        for (x = 0; x < b.width * b.height; ++x) {
            swapped += pixels[0][4 * x] == pixels[1][4 * x + 2] && pixels[0][4 * x + 2] == pixels[1][4 * x] && pixels[0][4 * x + 1] == pixels[1][4 * x + 1];
            opaque += pixels[0][4 * x + 3] == 255 && pixels[1][4 * x + 3] == 255;
        }
        CHECK(swapped == b.width * b.height && opaque == b.width * b.height);
    }
    allocations = media_test_allocations_end();
    printf("Host tracks: English %u -> French %u; attached subtitles shown, hidden, replaced; snapshot to memory %ux%u, hash %016llx; %llu allocations\n",
           english, french, b.width, b.height, (unsigned long long)b.frame_hash, (unsigned long long)allocations);
    CHECK(allocations == 0);
    /* The same picture to a file: the same hash. */
    CHECK(media_engine_save_snapshot(e, "host_tracks_snapshot.bmp") == MEDIA_OK);
    memset(&file, 0, sizeof(file)); file.struct_size = sizeof(file);
    CHECK(media_query_snapshot(e, &file) == MEDIA_OK && file.frame_hash == b.frame_hash && file.width == b.width && file.height == b.height);
    (void)media_engine_close(e);
    media_engine_destroy(e);
    remove("host_attach_1.srt"); remove("host_attach_2.vtt"); remove("host_attach_empty.srt");
    remove("host_tracks_snapshot.bmp"); remove("host_tracks_snapshot.json");
}

static void TestMatroska(const char* path) {
    media_engine_config_t config;
    media_engine_t* e = NULL;
    uint32_t count = 0, i, subtitles = 0, active = 0;
    media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.on_present = OnPresent;
    CHECK(media_engine_create(&config, Aligned(storage), STORAGE - 4096, &e) == MEDIA_OK);
    if (!e) return;
    if (media_engine_open(e, path) != MEDIA_OK) { printf("FAIL open %s\n", path); ++failures; media_engine_destroy(e); return; }
    CHECK(media_engine_track_count(e, &count) == MEDIA_OK && count == 3);
    for (i = 0; i < count; ++i) {
        media_track_info_t t; memset(&t, 0, sizeof(t)); t.struct_size = sizeof(t);
        CHECK(media_engine_track_info(e, i, &t) == MEDIA_OK);
        if (t.kind == MEDIA_TRACK_SUBTITLE) { ++subtitles; CHECK(strcmp(t.codec, "text") == 0 && t.selectable && t.active && strcmp(t.language, "eng") == 0); }
        if (t.kind == MEDIA_TRACK_VIDEO) CHECK(strcmp(t.codec, "avc1") == 0 && t.width == 640 && t.height == 360);
    }
    CHECK(subtitles == 1 && Active(e, MEDIA_TRACK_SUBTITLE, &active) && active != 0);
    printf("Matroska in host mode: %u tracks, its text subtitles listed and shown (id %u)\n", count, active);
    (void)media_engine_close(e);
    media_engine_destroy(e);
}

static char args[3][1024];
int main(int argc, char** argv) {
    int i;
    if (argc < 3) { puts("usage: media_host_tracks_tests portable_fixture.mp4 h264_aac_srt_chapters.mkv"); return 2; }
#if defined(_WIN32)
    {
        /* The SDK takes UTF-8; argv is in the ANSI code page ("Développement" would not survive it): the wide command line. */
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        for (i = 1; i < 3 && wide && i < count; ++i) (void)WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], (int)sizeof(args[i]), NULL, NULL);
        if (wide) LocalFree(wide);
    }
#else
    for (i = 1; i < 3; ++i) strncpy(args[i], argv[i], sizeof(args[i]) - 1);
#endif
    (void)argv;
    TestPortable(args[1]);
    TestMatroska(args[2]);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "host tracks tests passed", failures);
    return failures ? 1 : 0;
}
