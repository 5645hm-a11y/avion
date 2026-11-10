// media_probe and media_engine_last_error_text (API 15; docs/sdk-reference.md): what a file holds, whether it plays, and why
// not, in words. Run with the fixtures directory (skips, 77, without it).
#include "media/c_api.h"
#include "../src/sdk/ProbeText.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;
// The real-world fixtures (the reported mpv file, the 4K and 45-minute clips) are not in the public repository: a case whose file
// is absent is skipped and said so, as the other fixture tests do.
bool Present(const std::string& path) {
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
    std::printf("Fixture: %s not present, skipped\n", path.c_str());
    return false;
}
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

media_probe_t Probe(const std::string& path, media_status& status) {
    media_probe_t p{}; p.struct_size = sizeof(p);
    status = media_probe(path.c_str(), &p);
    std::printf("%-60s status %d container %u playable %u %.2f s: %s\n", path.substr(path.size() > 60 ? path.size() - 60 : 0).c_str(), status,
                p.container, p.playable, p.duration_100ns / 1e7, p.reason);
    for (unsigned i = 0; i < p.track_count; ++i)
        std::printf("    #%u kind %u %-5s %-48s %s\n", p.tracks[i].id, p.tracks[i].kind, p.tracks[i].codec, p.tracks[i].description, p.tracks[i].reason);
    return p;
}
bool Has(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned, unsigned long, const char*, int, wchar_t*, int);
extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
#endif
// argv is in the ANSI code page on Windows (the project's path has 'é'); media_probe takes UTF-8, as hosts pass it.
std::string Utf8Argument(const char* argument) {
#if defined(_WIN32)
    wchar_t wide[2048]; char utf8[4096];
    if (MultiByteToWideChar(0 /* CP_ACP */, 0, argument, -1, wide, 2048) <= 0) return argument;
    if (WideCharToMultiByte(65001 /* CP_UTF8 */, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr) <= 0) return argument;
    return utf8;
#else
    return argument;
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: media_probe_tests <fixtures dir>"); return 77; }
    const std::string dir = Utf8Argument(argv[1]);
    media_status status{};
    // Everyday files the engine plays.
    auto p = Probe(dir + "/compat/x264_high_default.mp4", status);
    CHECK(status == MEDIA_OK && p.container == MEDIA_CONTAINER_MP4 && p.playable == 1 && p.track_count == 2);
    CHECK(p.tracks[0].kind == MEDIA_TRACK_VIDEO && p.tracks[0].supported && Has(p.tracks[0].description, "H.264 High, 4:2:0 8-bit, progressive"));
    CHECK(p.tracks[1].kind == MEDIA_TRACK_AUDIO && p.tracks[1].supported && Has(p.tracks[1].description, "AAC"));
    CHECK(p.duration_100ns > 39000000 && p.duration_100ns < 41000000 && p.tracks[0].width == 640 && p.tracks[0].height == 360);
    // The reported file: mpv's encoder wrote a last sample of no duration; the video track was refused ("stage 4").
    // Its synthetic twin (tools/realworld/make_compat_matrix.py, stts_last_zero), tracked in the repository: always run.
    p = Probe(dir + "/compat/stts_last_zero.mp4", status);
    CHECK(status == MEDIA_OK && p.playable == 1 && p.tracks[0].supported && p.tracks[1].sample_rate == 44100);
    if (Present(dir + "/compat/mpv_last_frame_zero.mp4")) {
        p = Probe(dir + "/compat/mpv_last_frame_zero.mp4", status);
        CHECK(status == MEDIA_OK && p.playable == 1 && p.tracks[0].supported && p.tracks[1].sample_rate == 44100);
    }
    if (Present(dir + "/compat/x264_start_offset.mp4")) {
        p = Probe(dir + "/compat/x264_start_offset.mp4", status);
        CHECK(status == MEDIA_OK && p.playable == 1);
    }
    if (Present(dir + "/big_buck_bunny_4k60.mp4")) {
        p = Probe(dir + "/big_buck_bunny_4k60.mp4", status);
        CHECK(status == MEDIA_OK && p.playable == 1 && Has(p.tracks[0].description, "HEVC Main"));
    }
    // AV1 (API 17): played through the platform's software decoder on Windows (ENGINE_MANIFESTO.md 2E); elsewhere refused with the reason.
    if (Present(dir + "/av_sync_clock_45m.mp4")) {
    p = Probe(dir + "/av_sync_clock_45m.mp4", status);
#if defined(_WIN32)
    CHECK(status == MEDIA_OK && p.playable == 1 && p.tracks[0].kind == MEDIA_TRACK_VIDEO && p.tracks[0].supported && Has(p.tracks[0].description, "AV1"));
#else
    CHECK(status == MEDIA_OK && p.playable == 0 && Has(p.reason, "AV1") && p.tracks[0].kind == MEDIA_TRACK_VIDEO && !p.tracks[0].supported);
#endif
    CHECK(p.duration_100ns > 26000000000ll); // 44 min 27 s.
    }
    // Matroska and WebM (API 16 read them; API 17 plays them): tracks, codecs, profiles, duration, and which ones play.
    p = Probe(dir + "/compat/h264_aac_two_audio.mkv", status);
    CHECK(status == MEDIA_OK && p.container == MEDIA_CONTAINER_MATROSKA && p.playable == 1 && p.track_count == 3);
    CHECK(p.tracks[0].kind == MEDIA_TRACK_VIDEO && Has(p.tracks[0].description, "H.264 High, 4:2:0 8-bit, progressive") && p.tracks[0].width == 640 && p.tracks[0].supported);
    CHECK(p.tracks[2].kind == MEDIA_TRACK_AUDIO && std::strcmp(p.tracks[2].language, "heb") == 0 && p.duration_100ns > 39000000 && p.duration_100ns < 41000000);
    CHECK(p.tracks[1].supported && p.tracks[2].supported);
    p = Probe(dir + "/compat/vp9_opus.webm", status);
    CHECK(status == MEDIA_OK && p.container == MEDIA_CONTAINER_MATROSKA && Has(p.reason, "WebM (VP9, Opus): its video codec is not decoded"));
    CHECK(p.tracks[0].kind == MEDIA_TRACK_VIDEO && std::strcmp(p.tracks[0].codec, "vp09") == 0 && Has(p.tracks[0].reason, "VP9 is not decoded yet"));
    // Not MP4: the container named from its first bytes.
    // Written next to the test binary (an ASCII-safe relative path), read back through the UTF-8 entry point.
    const std::string mkv = "media_probe_test_stub.mkv";
    if (std::FILE* f = std::fopen(mkv.c_str(), "wb")) {
        const unsigned char ebml[] = {0x1a, 0x45, 0xdf, 0xa3, 0x9f, 0x42, 0x86, 0x81, 0x01, 0x42, 0xf7, 0x81, 0x01, 0, 0, 0};
        std::fwrite(ebml, 1, sizeof(ebml), f); std::fclose(f);
        p = Probe(mkv, status);
        CHECK(status == MEDIA_OK && p.container == MEDIA_CONTAINER_MATROSKA && p.playable == 0 && Has(p.reason, "Matroska/WebM: malformed EBML"));
        std::remove(mkv.c_str());
    } else CHECK(!"could not write the Matroska stub");
    // A missing file, a wrong struct size, nulls.
    p = Probe(dir + "/no such file.mp4", status);
    CHECK(status == MEDIA_ERROR_OPEN && p.playable == 0 && Has(p.reason, "cannot be opened"));
    media_probe_t small{}; small.struct_size = 8;
    CHECK(media_probe((dir + "/compat/x264_high_default.mp4").c_str(), &small) == MEDIA_ERROR_INVALID_ARGUMENT);
    CHECK(media_probe(nullptr, &small) == MEDIA_ERROR_INVALID_ARGUMENT && media_probe("x", nullptr) == MEDIA_ERROR_INVALID_ARGUMENT);
    // The text helpers: a cut never splits a UTF-8 sequence ("שלום": two bytes a letter).
    char cut[6]; std::memcpy(cut, "\xd7\xa9\xd7\x9c\xd7\x95", 6); media::sdk::CutUtf8(cut, sizeof(cut));
    CHECK(std::strlen(cut) == 4); // The third letter would need bytes 5 and 6; the terminator takes 6.
    CHECK(std::strcmp(media::sdk::StageText(false, 4), "no usable video track (or, without video, no AAC sound)") == 0);
    CHECK(std::strcmp(media::sdk::StageText(true, 6), "no track this engine can play") == 0);
    CHECK(media_engine_last_error_text(nullptr, cut, sizeof(cut)) == MEDIA_ERROR_INVALID_ARGUMENT);
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "probe tests passed", failures);
    return failures ? 1 : 0;
}
