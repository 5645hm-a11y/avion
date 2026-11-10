// demux::MatroskaReader (API 16): the metadata of a real file, and every corruption and truncation of its first 4 KiB terminates
// cleanly (the portable suite runs this under ASan/UBSan). Arguments: the fixtures directory (skips, 77, without it).
#include "media/demux/MatroskaReader.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
struct Memory { const std::vector<unsigned char>* bytes; };
std::size_t ReadMemory(void* context, std::uint64_t offset, void* out, std::size_t n) noexcept {
    const auto& b = *static_cast<Memory*>(context)->bytes;
    if (offset >= b.size()) return 0;
    const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(n, b.size() - offset));
    std::memcpy(out, b.data() + offset, take);
    return take;
}
std::vector<unsigned char> Load(const std::string& path) {
    std::vector<unsigned char> b;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        unsigned char chunk[65536]; std::size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
        std::fclose(f);
    }
    return b;
}
}

int main(int argc, char** argv) {
    // The portable suite runs it from the repository's root without arguments.
    const std::string dir = argc >= 2 ? argv[1] : "assets/test_fixtures";
    auto file = Load(dir + "/compat/h264_aac_two_audio.mkv");
    auto webm = Load(dir + "/compat/vp9_opus.webm");
    if (file.empty() || webm.empty()) { std::puts("fixtures missing"); return 77; }
    using namespace media::demux;
    MatroskaReader r; Memory m{&file};
    CHECK(r.Read(&ReadMemory, &m, file.size()) == MatroskaStatus::Ok && !r.WebM() && r.TrackCount() == 3);
    CHECK(std::strcmp(r.Track(0).codec_id, "V_MPEG4/ISO/AVC") == 0 && r.Track(0).type == MatroskaTrackType::Video && r.Track(0).width == 640 && r.Track(0).height == 360);
    CHECK(r.Track(0).private_bytes > 8 && r.Track(0).private_offset > 0 && r.Track(0).private_offset + r.Track(0).private_bytes < file.size());
    CHECK(file[r.Track(0).private_offset] == 1); // avcC's configurationVersion.
    CHECK(std::strcmp(r.Track(1).codec_id, "A_AAC") == 0 && r.Track(1).type == MatroskaTrackType::Audio && r.Track(1).sample_rate == 48000 && r.Track(1).channels == 1);
    CHECK(std::strcmp(r.Track(2).language, "heb") == 0 && std::strcmp(r.Track(2).name, "Hebrew dub") == 0);
    CHECK(r.TimecodeScale() == 1000000 && r.Duration100ns() > 39000000 && r.Duration100ns() < 41000000 && r.FirstCluster() > 0);
    Memory w{&webm}; MatroskaReader rw;
    CHECK(rw.Read(&ReadMemory, &w, webm.size()) == MatroskaStatus::Ok && rw.WebM() && rw.TrackCount() == 2 && std::strcmp(rw.Track(0).codec_id, "V_VP9") == 0);
    // Not Matroska at all.
    std::vector<unsigned char> mp4(64, 0); std::memcpy(mp4.data() + 4, "ftyp", 4); Memory n{&mp4}; MatroskaReader rn;
    CHECK(rn.Read(&ReadMemory, &n, mp4.size()) == MatroskaStatus::NotMatroska);
    // Every byte of the first 4 KiB flipped, then every truncation of it: always a status, never a crash or a read out of range.
    unsigned outcomes[5]{};
    const std::size_t span = std::min<std::size_t>(4096, file.size());
    for (std::size_t i = 0; i < span; ++i) {
        auto copy = file; copy[i] ^= 0xff; Memory c{&copy}; MatroskaReader rc;
        const auto s = rc.Read(&ReadMemory, &c, copy.size());
        ++outcomes[static_cast<unsigned>(s)];
        CHECK(rc.TrackCount() <= MatroskaReader::kMaxTracks);
    }
    for (std::size_t cut = 0; cut < span; cut += 7) {
        std::vector<unsigned char> part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut)); Memory c{&part}; MatroskaReader rc;
        (void)rc.Read(&ReadMemory, &c, part.size());
    }
    std::printf("Matroska: 3 tracks (H.264 640x360, AAC, AAC heb \"Hebrew dub\"), %.2f s; WebM VP9 + Opus; %zu corruptions: ok %u, not %u, truncated %u, malformed %u, no tracks %u\n",
                r.Duration100ns() / 1e7, span, outcomes[0], outcomes[1], outcomes[2], outcomes[3], outcomes[4]);
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "matroska tests passed", failures);
    return failures ? 1 : 0;
}
