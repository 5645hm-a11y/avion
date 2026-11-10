/* Embedding (API 12; docs/sdk.md, "Embedding: API 12"), through the C99 SDK only, as a host application uses it: its UI thread
 * owns a window and posts commands; the engine plays on a thread of its own inside that window.
 *   media_embed_tests <playback_fixture.mp4> <portable_fixture.mp4>
 * 1. Embedded (the 3 s fixture): the engine's window is a child of the host's, filling its client area; the host's resize is
 *    followed (a WinEvent hook, no polling); OPENED, then POSITION events advancing while playing; "pause" applied with a STATE
 *    event; while paused and settled, "volume down" answered within 50 ms (the engine's thread is woken, not polled); a command
 *    the grammar refuses answered MEDIA_ERROR_REFUSED; Escape no longer stops playback; Alt+Enter asks the host for fullscreen
 *    (MEDIA_EVENT_FULLSCREEN_REQUEST) and leaves the child's size alone; media_engine_state from the UI thread; ENDED
 *    (MEDIA_ENDED_MEDIA) after the end; 0 allocations in the process while playing embedded (the engine's threads included).
 * 2. The host destroys its window while playing: ENDED (MEDIA_ENDED_CLOSED) within a second, run returns.
 * 3. media_engine_stop from the UI thread: ENDED (MEDIA_ENDED_STOPPED); this run's UI thread pumps nothing while open runs (the
 *    child's creation must not wait for it).
 * 4. The API: API 11's config size accepted, a parent window refused in host mode, a command too long or before open refused,
 *    media_api_version 12; host mode's posted commands run by media_engine_step, each with its MEDIA_EVENT_COMMAND.
 * Built twice: linked with media_sdk (static) and with avion.dll (media_embed_dll_tests: the DLL's exports; its allocation count
 * sees only this program's own heap, the DLL having its own runtime). Prints the measured latencies. Exit 0 when every check passed. */
#include "media/c_api.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void media_test_allocations_begin(void);
uint64_t media_test_allocations_end(void);

static int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static int64_t Now100ns(void) {
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return c.QuadPart / f.QuadPart * 10000000 + c.QuadPart % f.QuadPart * 10000000 / f.QuadPart;
}

/* The host's window and its message pump. */
static LRESULT CALLBACK HostProc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(w, m, wp, lp); }
static HWND MakeHost(int width, int height) {
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = HostProc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"EmbedTestHost";
    (void)RegisterClassW(&wc);
    RECT r = {0, 0, width, height};
    (void)AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    /* Off the screens: a real, visible window that no pointer reaches during the run. */
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"EmbedTestHost", L"Embedding test host", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             -4000, -4000, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (h) ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}
static void ResizeHost(HWND h, int width, int height) {
    RECT r = {0, 0, width, height};
    (void)AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    (void)SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void Client(HWND h, int* width, int* height) { RECT r = {0, 0, 0, 0}; (void)GetClientRect(h, &r); *width = r.right; *height = r.bottom; }

/* The engine's thread: create, open, run, close (and destroy once the UI thread has read the last events). */
typedef struct {
    const char* path; HWND parent; int visible;
    unsigned char* storage;
    media_engine_t* volatile engine;
    volatile LONG opened, done, release;
    media_status open_status, run_status;
} EngineRun;
static DWORD WINAPI EngineThread(void* p) {
    EngineRun* r = (EngineRun*)p;
    media_engine_config_t config; media_engine_config_init(&config);
    config.modules = MEDIA_MODULE_ALL & ~(uint32_t)(MEDIA_MODULE_VOICE | MEDIA_MODULE_HUD); /* The host's design: none of the engine's controls. */
    config.visible = r->visible; config.parent_window = r->parent;
    media_engine_t* e = NULL;
    if (media_engine_create(&config, r->storage, media_engine_storage_bytes(), &e) != MEDIA_OK) { InterlockedExchange(&r->done, 1); return 0; }
    r->open_status = media_engine_open(e, r->path);
    r->engine = e; InterlockedExchange(&r->opened, 1);
    if (r->open_status == MEDIA_OK) { r->run_status = media_engine_run(e); (void)media_engine_close(e); }
    InterlockedExchange(&r->done, 1);
    while (!InterlockedCompareExchange(&r->release, 0, 0)) Sleep(1);
    media_engine_destroy(e);
    return 0;
}

/* The UI thread's log of events, polled while it pumps its messages. */
#define kLog 1024
static media_event_t g_log[kLog];
static int g_count = 0;
static void Poll(media_engine_t* e) {
    media_event_t ev;
    for (;;) {
        memset(&ev, 0, sizeof ev); ev.struct_size = sizeof ev;
        if (media_engine_poll_event(e, &ev) != MEDIA_OK) break;
        if (g_count < kLog) g_log[g_count++] = ev;
    }
}
static void Pump(media_engine_t* e, int milliseconds) {
    const int64_t until = Now100ns() + (int64_t)milliseconds * 10000;
    MSG m;
    while (Now100ns() < until) {
        (void)MsgWaitForMultipleObjects(0, NULL, FALSE, 2, QS_ALLINPUT);
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (e) Poll(e);
    }
}
/* The first event of `kind` from index `from` (and command `id` for MEDIA_EVENT_COMMAND), waiting up to `milliseconds`. */
static const media_event_t* WaitFor(media_engine_t* e, int from, uint32_t kind, uint32_t id, int milliseconds) {
    const int64_t until = Now100ns() + (int64_t)milliseconds * 10000;
    for (;;) {
        for (int i = from; i < g_count; ++i)
            if (g_log[i].kind == kind && (kind != MEDIA_EVENT_COMMAND || g_log[i].command_id == id)) return &g_log[i];
        if (Now100ns() >= until) return NULL;
        Pump(e, 1);
    }
}

static void Embedded(const char* path, unsigned char* storage) {
    HWND host = MakeHost(640, 360);
    CHECK(host != NULL);
    EngineRun r; memset(&r, 0, sizeof r); r.path = path; r.parent = host; r.visible = 1; r.storage = storage;
    g_count = 0;
    HANDLE thread = CreateThread(NULL, 0, EngineThread, &r, 0, NULL);
    while (!InterlockedCompareExchange(&r.opened, 0, 0) && !InterlockedCompareExchange(&r.done, 0, 0)) Pump(NULL, 1);
    media_engine_t* e = r.engine;
    CHECK(e != NULL && r.open_status == MEDIA_OK);
    if (!e || r.open_status != MEDIA_OK) { InterlockedExchange(&r.release, 1); WaitForSingleObject(thread, 10000); CloseHandle(thread); DestroyWindow(host); return; }

    const media_event_t* opened = WaitFor(e, 0, MEDIA_EVENT_OPENED, 0, 10000);
    CHECK(opened && opened->duration_100ns > 25000000 && opened->width > 0 && opened->height > 0);
    HWND child = FindWindowExW(host, NULL, L"MediaEngineCanvas", NULL);
    int pw = 0, ph = 0, cw = 0, ch = 0;
    Client(host, &pw, &ph); if (child) Client(child, &cw, &ch);
    CHECK(child && GetParent(child) == host && (GetWindowLongW(child, GWL_STYLE) & WS_CHILD));
    CHECK(cw == pw && ch == ph && pw == 640 && ph == 360);
    printf("embedded: child of the host, %dx%d in %dx%d; opened %.2f s, picture %ux%u\n", cw, ch, pw, ph,
           opened ? opened->duration_100ns / 1e7 : 0.0, opened ? opened->width : 0, opened ? opened->height : 0);

    /* Playing: positions advance; no allocation anywhere in the process meanwhile (the UI thread's polls included). */
    Pump(e, 300);
    media_test_allocations_begin();
    Pump(e, 500);
    const uint64_t allocations = media_test_allocations_end();
    int positions = 0; int64_t last = -1, first_position = -1;
    for (int i = 0; i < g_count; ++i) if (g_log[i].kind == MEDIA_EVENT_POSITION) { ++positions; if (first_position < 0) first_position = g_log[i].position_100ns; last = g_log[i].position_100ns; }
    CHECK(positions >= 2 && last > first_position);
    CHECK(allocations == 0);

    /* Pause, through the queue. */
    int mark = g_count;
    int64_t asked = Now100ns();
    CHECK(media_engine_post_command(e, "pause", 1) == MEDIA_OK);
    const media_event_t* done = WaitFor(e, mark, MEDIA_EVENT_COMMAND, 1, 2000);
    CHECK(done && done->status == MEDIA_OK);
    const media_event_t* state = WaitFor(e, mark, MEDIA_EVENT_STATE, 0, 2000);
    CHECK(state && state->paused == 1);
    const double pause_ms = state ? (state->time_100ns - asked) / 1e4 : -1;
    media_playback_state_t s; memset(&s, 0, sizeof s);
    CHECK(media_engine_state(e, &s) == MEDIA_OK && s.paused == 1 && s.duration_100ns > 25000000 && s.presented > 0);
    const float volume = state ? state->volume : 1;

    /* Paused and settled (the engine's thread asleep): a command is answered at once. */
    Pump(e, 800);
    mark = g_count; asked = Now100ns();
    CHECK(media_engine_post_command(e, "volume down", 2) == MEDIA_OK);
    const media_event_t* quieter = WaitFor(e, mark, MEDIA_EVENT_STATE, 0, 2000);
    CHECK(quieter && quieter->volume < volume && quieter->paused == 1);
    const double settled_ms = quieter ? (quieter->time_100ns - asked) / 1e4 : -1;
    CHECK(settled_ms >= 0 && settled_ms < 50);

    /* API 15: structured controls. Out-of-range values are refused at once; the rest run as their commands would. */
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_VOLUME, 2.5, 20) == MEDIA_ERROR_INVALID_ARGUMENT); /* To 2 (a boost) since API 16. */
    CHECK(media_engine_post_control(e, 99, 1, 20) == MEDIA_ERROR_INVALID_ARGUMENT && media_engine_post_control(e, MEDIA_CONTROL_SPEED, 4.5, 20) == MEDIA_ERROR_INVALID_ARGUMENT);
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SEEK, -1, 20) == MEDIA_ERROR_INVALID_ARGUMENT && media_engine_post_control(e, MEDIA_CONTROL_PAUSE, 0.5, 20) == MEDIA_ERROR_INVALID_ARGUMENT);
    mark = g_count;
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_VOLUME, 0.4, 21) == MEDIA_OK);
    const media_event_t* set = WaitFor(e, mark, MEDIA_EVENT_COMMAND, 21, 2000);
    const media_event_t* level = WaitFor(e, mark, MEDIA_EVENT_STATE, 0, 2000);
    CHECK(set && set->status == MEDIA_OK && level && level->volume > 0.399f && level->volume < 0.401f);
    mark = g_count; asked = Now100ns();
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SEEK, 2.0, 22) == MEDIA_OK);
    const media_event_t* sought = WaitFor(e, mark, MEDIA_EVENT_COMMAND, 22, 2000);
    const media_event_t* landed = WaitFor(e, mark, MEDIA_EVENT_SEEK_LANDED, 0, 3000);
    CHECK(sought && sought->status == MEDIA_OK && landed && landed->position_100ns > 19000000 && landed->position_100ns <= 20000000);
    const double seek_ms = landed ? (landed->time_100ns - asked) / 1e4 : -1;
    mark = g_count;
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SEEK_RELATIVE, -1.0, 23) == MEDIA_OK);
    const media_event_t* back = WaitFor(e, mark, MEDIA_EVENT_SEEK_LANDED, 0, 3000);
    CHECK(back && back->position_100ns > 9000000 && back->position_100ns <= 10000000);
    mark = g_count;
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SPEED, 1.5, 24) == MEDIA_OK);
    const media_event_t* faster = WaitFor(e, mark, MEDIA_EVENT_STATE, 0, 2000);
    CHECK(faster && faster->speed == 1.5f);
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_SPEED, 1.0, 25) == MEDIA_OK && media_engine_post_control(e, MEDIA_CONTROL_MUTE, 1, 26) == MEDIA_OK);
    const media_event_t* muted = WaitFor(e, mark, MEDIA_EVENT_COMMAND, 26, 2000);
    CHECK(muted && muted->status == MEDIA_OK);
    Pump(e, 100);
    CHECK(media_engine_state(e, &s) == MEDIA_OK && s.muted == 1 && s.speed == 1.0f && s.paused == 1);
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_MUTE, 0, 27) == MEDIA_OK);
    /* API 16: the tracks, listed from the host's thread while the engine runs. */
    uint32_t tracks = 0; int videos = 0, active_video = 0, audios = 0, active_audio = 0;
    CHECK(media_engine_track_count(e, &tracks) == MEDIA_OK && tracks >= 2 && tracks <= MEDIA_MAX_TRACKS);
    for (uint32_t i = 0; i < tracks; ++i) {
        media_track_info_t t; memset(&t, 0, sizeof t); t.struct_size = sizeof t;
        CHECK(media_engine_track_info(e, i, &t) == MEDIA_OK);
        if (t.kind == MEDIA_TRACK_VIDEO) { ++videos; active_video += (int)t.active; CHECK(t.width > 0 && t.height > 0 && t.codec[0]); }
        if (t.kind == MEDIA_TRACK_AUDIO) { ++audios; active_audio += (int)t.active; CHECK(t.sample_rate > 0 && t.channels > 0); }
    }
    CHECK(videos == 1 && active_video == 1 && audios >= 1 && active_audio == 1);
    { media_track_info_t t; memset(&t, 0, sizeof t); t.struct_size = 8; CHECK(media_engine_track_info(e, 0, &t) == MEDIA_ERROR_INVALID_ARGUMENT); }
    { media_track_info_t t; memset(&t, 0, sizeof t); t.struct_size = sizeof t; CHECK(media_engine_track_info(e, tracks, &t) == MEDIA_ERROR_INVALID_ARGUMENT); }
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_AUDIO_TRACK, 0.5, 28) == MEDIA_ERROR_INVALID_ARGUMENT);
    /* A boost through the control: the state says 1.5. */
    mark = g_count;
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_VOLUME, 1.5, 29) == MEDIA_OK);
    /* The state that carries it: a state event of the unmute above may still arrive first (seen through the DLL, 2026-11-05). */
    const media_event_t* boosted = NULL;
    for (int from = mark; !boosted || boosted->volume != 1.5f;) {
        boosted = WaitFor(e, from, MEDIA_EVENT_STATE, 0, 2000);
        if (!boosted) break;
        from = (int)(boosted - g_log) + 1;
    }
    CHECK(boosted && boosted->volume == 1.5f);
    /* API 17: subtitles after open in the window, through the host's call (not the window's thread). */
    {
        FILE* f = fopen("embed_attach.srt", "wb");
        if (f) { fputs("1\r\n00:00:00,000 --> 00:01:00,000\r\nAttached in the host's window.\r\n\r\n", f); fclose(f); }
        CHECK(media_engine_attach_subtitles(e, "embed_attach.srt") == MEDIA_OK);
        CHECK(media_engine_attach_subtitles(e, "embed_attach_missing.srt") == MEDIA_ERROR_OPEN);
        remove("embed_attach.srt");
    }
    printf("tracks: %u (%d video, %d audio), active video %d, audio %d; boost to 150 %%\n", tracks, videos, audios, active_video, active_audio);
    printf("controls: volume 0.4, seek to 2 s landed at %.3f s in %.1f ms, back 1 s to %.3f s, speed 1.5 then 1, mute\n",
           landed ? landed->position_100ns / 1e7 : -1.0, seek_ms, back ? back->position_100ns / 1e7 : -1.0);

    /* The host resizes its window: the child follows. */
    ResizeHost(host, 960, 540);
    Pump(e, 400);
    Client(host, &pw, &ph); Client(child, &cw, &ch);
    CHECK(pw == 960 && ph == 540 && cw == pw && ch == ph);
    printf("resized: host %dx%d, child %dx%d\n", pw, ph, cw, ch);

    /* A command the grammar refuses; Escape (no longer the engine's to close); Alt+Enter: a request to the host. */
    mark = g_count;
    CHECK(media_engine_post_command(e, "flibbertigibbet wobble", 3) == MEDIA_OK);
    const media_event_t* refused = WaitFor(e, mark, MEDIA_EVENT_COMMAND, 3, 2000);
    CHECK(refused && refused->status == MEDIA_ERROR_REFUSED);
    (void)PostMessageW(child, WM_KEYDOWN, VK_ESCAPE, 1);
    (void)PostMessageW(child, WM_SYSKEYDOWN, VK_RETURN, (LPARAM)1 | ((LPARAM)1 << 29));
    const media_event_t* fullscreen = WaitFor(e, mark, MEDIA_EVENT_FULLSCREEN_REQUEST, 0, 2000);
    CHECK(fullscreen != NULL);
    Pump(e, 300);
    CHECK(!InterlockedCompareExchange(&r.done, 0, 0)); /* Escape did not end it. */
    Client(child, &cw, &ch);
    CHECK(cw == 960 && ch == 540); /* Fullscreen is the host's: the child kept its size. */

    /* Play to the end. */
    mark = g_count;
    CHECK(media_engine_post_command(e, "play", 4) == MEDIA_OK);
    const media_event_t* playing = WaitFor(e, mark, MEDIA_EVENT_STATE, 0, 2000);
    CHECK(playing && playing->paused == 0);
    const media_event_t* ended = WaitFor(e, mark, MEDIA_EVENT_ENDED, 0, 15000);
    CHECK(ended && ended->reason == MEDIA_ENDED_MEDIA && ended->status == MEDIA_OK);
    while (!InterlockedCompareExchange(&r.done, 0, 0)) Pump(e, 5);
    CHECK(r.run_status == MEDIA_OK);
    int lost = 0; for (int i = 0; i < g_count; ++i) lost += (int)g_log[i].lost;
    CHECK(lost == 0);
    printf("commands: pause shown in %.1f ms (playing); volume down while paused and settled in %.2f ms; refused %d; fullscreen asked %d; "
           "%d position events; ended (media) at %.2f s; %llu allocations while playing\n",
           pause_ms, settled_ms, refused ? (int)refused->status : -1, fullscreen != NULL, positions, ended ? ended->position_100ns / 1e7 : -1.0,
           (unsigned long long)allocations);
    InterlockedExchange(&r.release, 1); WaitForSingleObject(thread, 10000); CloseHandle(thread);
    DestroyWindow(host); Pump(NULL, 50);
}

static void HostGone(const char* path, unsigned char* storage, int destroy) {
    HWND host = MakeHost(480, 270);
    EngineRun r; memset(&r, 0, sizeof r); r.path = path; r.parent = host; r.visible = 1; r.storage = storage;
    g_count = 0;
    HANDLE thread = CreateThread(NULL, 0, EngineThread, &r, 0, NULL);
    /* The stop run's UI thread waits for open without pumping its messages (as a host blocked in a wait may): creating the
     * child must not need the parent's thread (WS_EX_NOPARENTNOTIFY: no WM_PARENTNOTIFY sent to it). */
    const int64_t asked_open = Now100ns();
    while (!InterlockedCompareExchange(&r.opened, 0, 0) && !InterlockedCompareExchange(&r.done, 0, 0)) { if (destroy) Pump(NULL, 1); else Sleep(1); }
    const double open_ms = (Now100ns() - asked_open) / 1e4;
    media_engine_t* e = r.engine;
    CHECK(e != NULL && r.open_status == MEDIA_OK);
    if (!destroy) { CHECK(open_ms < 5000); printf("opened with the host's thread not pumping: %.0f ms\n", open_ms); }
    if (e && r.open_status == MEDIA_OK) {
        CHECK(WaitFor(e, 0, MEDIA_EVENT_OPENED, 0, 10000) != NULL);
        Pump(e, 700);
        const int64_t asked = Now100ns();
        if (destroy) { DestroyWindow(host); host = NULL; } else media_engine_stop(e);
        const media_event_t* ended = WaitFor(e, 0, MEDIA_EVENT_ENDED, 0, 5000);
        const double ms = ended ? (ended->time_100ns - asked) / 1e4 : -1;
        CHECK(ended && ended->reason == (uint32_t)(destroy ? MEDIA_ENDED_CLOSED : MEDIA_ENDED_STOPPED) && ms >= 0 && ms < 1000);
        while (!InterlockedCompareExchange(&r.done, 0, 0)) Pump(e, 5);
        CHECK(r.run_status == MEDIA_OK);
        printf("%s: ended (%s) %.1f ms after, run %d\n", destroy ? "host window destroyed" : "media_engine_stop", ended ? (ended->reason == MEDIA_ENDED_CLOSED ? "closed" : ended->reason == MEDIA_ENDED_STOPPED ? "stopped" : "other") : "none",
               ms, (int)r.run_status);
    }
    InterlockedExchange(&r.release, 1); WaitForSingleObject(thread, 10000); CloseHandle(thread);
    if (host) DestroyWindow(host);
    Pump(NULL, 50);
}

static void Api(const char* portable, unsigned char* storage) {
    CHECK(media_api_version() == 17u && MEDIA_API_VERSION == 17u); /* API 17 adds Matroska, AV1, host-mode tracks, subtitles after open, snapshots into memory; API 12's calls unchanged. */
#if defined(_WIN64)
    /* The x64 layouts the bindings rely on (samples/csharp, samples/python). */
    CHECK(sizeof(media_engine_config_t) == 176 && offsetof(media_engine_config_t, modules) == 4 && offsetof(media_engine_config_t, visible) == 48 &&
          offsetof(media_engine_config_t, playback) == 100 && offsetof(media_engine_config_t, parent_window) == 152);
    CHECK(sizeof(media_event_t) == 80 && offsetof(media_event_t, position_100ns) == 16 && offsetof(media_event_t, command_id) == 40 &&
          offsetof(media_event_t, volume) == 56 && offsetof(media_event_t, reason) == 72);
    CHECK(sizeof(media_playback_state_t) == 88 && offsetof(media_playback_state_t, paused) == 20 && offsetof(media_playback_state_t, volume) == 32 &&
          offsetof(media_playback_state_t, presented) == 64);
#endif
    media_engine_config_t config; media_engine_config_init(&config);
    CHECK(config.struct_size == sizeof(media_engine_config_t) && config.parent_window == NULL);
    media_engine_t* e = NULL;
    /* API 11's struct (through decoder_context). */
    config.struct_size = (uint32_t)offsetof(media_engine_config_t, parent_window);
    CHECK(media_engine_create(&config, storage, media_engine_storage_bytes(), &e) == MEDIA_OK && e);
    CHECK(media_engine_post_command(e, "pause", 1) == MEDIA_ERROR_STATE); /* Not open. */
    media_engine_destroy(e); e = NULL;
    /* A parent window in host mode. */
    media_engine_config_init(&config); config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.parent_window = GetDesktopWindow();
    CHECK(media_engine_create(&config, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    /* Host mode: posted commands run by the next step, each answered. */
    media_engine_config_init(&config); config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    CHECK(media_engine_create(&config, storage, media_engine_storage_bytes(), &e) == MEDIA_OK && e);
    if (!e) return;
    CHECK(media_engine_open(e, portable) == MEDIA_OK);
    char longest[300]; memset(longest, 'a', sizeof longest - 1); longest[sizeof longest - 1] = 0;
    CHECK(media_engine_post_command(e, longest, 9) == MEDIA_ERROR_INVALID_ARGUMENT);
    CHECK(media_engine_post_command(e, "pause", 11) == MEDIA_OK && media_engine_post_command(e, "wobble flibbertigibbet", 12) == MEDIA_OK);
    media_event_t ev; memset(&ev, 0, sizeof ev); ev.struct_size = sizeof ev;
    CHECK(media_engine_poll_event(e, &ev) == MEDIA_ERROR_EMPTY); /* Nothing until a step runs them. */
    (void)media_engine_step(e, 10000000); /* Runs the two commands first. */
    int ok = 0, refused = 0;
    while (memset(&ev, 0, sizeof ev), ev.struct_size = sizeof ev, media_engine_poll_event(e, &ev) == MEDIA_OK) {
        if (ev.kind == MEDIA_EVENT_COMMAND && ev.command_id == 11 && ev.status == MEDIA_OK) ++ok;
        if (ev.kind == MEDIA_EVENT_COMMAND && ev.command_id == 12 && ev.status == MEDIA_ERROR_REFUSED) ++refused;
    }
    media_playback_state_t s; memset(&s, 0, sizeof s);
    CHECK(ok == 1 && refused == 1 && media_engine_state(e, &s) == MEDIA_OK && s.paused == 1);
    /* API 15, host mode: controls run by the next step; a frame step is the windowed engine's only. */
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_VOLUME, 0.25, 13) == MEDIA_OK && media_engine_post_control(e, MEDIA_CONTROL_STEP_FRAME, 1, 14) == MEDIA_OK);
    CHECK(media_engine_post_control(e, MEDIA_CONTROL_PAUSE, 0, 15) == MEDIA_OK);
    (void)media_engine_step(e, 10010000);
    int controls_ok = 0, controls_refused = 0;
    while (memset(&ev, 0, sizeof ev), ev.struct_size = sizeof ev, media_engine_poll_event(e, &ev) == MEDIA_OK) {
        if (ev.kind == MEDIA_EVENT_COMMAND && (ev.command_id == 13 || ev.command_id == 15) && ev.status == MEDIA_OK) ++controls_ok;
        if (ev.kind == MEDIA_EVENT_COMMAND && ev.command_id == 14 && ev.status == MEDIA_ERROR_REFUSED) ++controls_refused;
    }
    CHECK(controls_ok == 2 && controls_refused == 1 && media_engine_state(e, &s) == MEDIA_OK && s.paused == 0 && s.volume == 0.25f);
    printf("api: version %u, API 11 config accepted, host mode: posted pause %s, refused one %s\n", media_api_version(), ok ? "ran" : "lost", refused ? "answered" : "lost");
    (void)media_engine_close(e); media_engine_destroy(e);
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: media_embed_tests playback_fixture.mp4 portable_fixture.mp4\n"); return 2; }
    unsigned char* block = (unsigned char*)malloc(media_engine_storage_bytes() + media_engine_storage_alignment());
    if (!block) return 1;
    unsigned char* storage = block + (media_engine_storage_alignment() - (size_t)block % media_engine_storage_alignment()) % media_engine_storage_alignment();
    Api(argv[2], storage);
    Embedded(argv[1], storage);
    HostGone(argv[1], storage, 1);
    HostGone(argv[1], storage, 0);
    free(block);
    printf("%s: %d failure(s)\n", failures ? "embedding tests FAILED" : "embedding tests passed", failures);
    return failures ? 1 : 0;
}
