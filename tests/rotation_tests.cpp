// Rotation (docs/picture.md, "Rotation"), portable; MSVC and GCC:
// - IsobmffParser reads the video track's tkhd matrix: the identity, quarter turns (90, 180, 270 degrees clockwise) and a
//   mirror are kept as written; an all-zero matrix (what many muxers leave) is the identity, not ignored; a free angle or a
//   scale is kept as the identity and flagged;
// - video::Orientation composes the file's matrix with the viewer's quarter turns (a 90-degree file turned back three
//   quarters is upright), names the turn and the mirror, maps a turned picture's output pixels onto the stored region
//   (corners land on the corners they should), keeps a leveled picture's corners inside the region, and brings a point and
//   a direction of the picture as shown back to the stored picture;
// - "rotate video", "סובב תמונה", "tourner la vidéo" and the other rotation and leveling commands in nine languages, and the
//   neighbours they must not take.
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/demux/IsobmffParser.hpp"
#include "media/video/Orientation.hpp"
#include "allocation_probe.hpp"
#include "isobmff_fixture.hpp"
#include <cmath>
#include <cstdio>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

// The fixture's bytes, every range ready at once.
class MemorySource final : public platform::IBlockSource {
public:
    explicit MemorySource(const fixture::File& file) noexcept : file_(file) {}
    std::uint64_t Size() const noexcept override { return file_.size; }
    std::uint32_t MaxRangeBytes() const noexcept override { return 4096; }
    platform::IoStatus Request(std::uint64_t offset, std::uint32_t bytes, platform::BlockToken& token) noexcept override {
        token = {};
        if (!bytes || offset > Size() || bytes > Size() - offset) return platform::IoStatus::OutOfRange;
        if (bytes > MaxRangeBytes()) return platform::IoStatus::TooLarge;
        for (std::uint32_t i = 0; i < 4; ++i) {
            auto& slot = slots_[i];
            if (slot.active) continue;
            slot.active = true; ++slot.generation; token = {i, slot.generation};
            slot.view = {file_.data.data() + offset, offset, bytes, token};
            return platform::IoStatus::Pending;
        }
        return platform::IoStatus::WouldBlock;
    }
    platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return platform::IoStatus::InvalidToken;
        view = slots_[token.id].view; return platform::IoStatus::Ready;
    }
    bool Release(platform::BlockToken token) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return false;
        slots_[token.id].active = false; return true;
    }
private:
    struct Slot { platform::BlockView view{}; std::uint32_t generation = 0; bool active = false; };
    const fixture::File& file_;
    Slot slots_[4]{};
};
bool Parse(const fixture::File& file, demux::TrackInfo& info) {
    core::LinearMemoryArena arena; if (!arena.Initialize(1024 * 1024)) return false;
    MemorySource source(file);
    demux::IsobmffParser parser;
    if (!parser.Initialize(arena, source, {2, 65536, 1}) || !arena.Seal()) return false;
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    auto status = demux::ParseStatus::Progress;
    for (unsigned step = 0; step < 5000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed; ++step) {
        status = parser.Step(queue);
        while (const auto* packet = queue.PeekReadSlot()) { (void)parser.ReturnPacket(*packet); (void)queue.ReleaseReadSlot(); }
    }
    if (!parser.TrackCount()) return false;
    info = *parser.Track(0);
    return status == demux::ParseStatus::Done;
}
void TestParser() {
    struct Case { const char* name; std::int32_t matrix[4]; std::int8_t want[4]; bool ignored; };
    constexpr std::int32_t one = 0x10000;
    const Case cases[]{
        {"none (all zero)", {0, 0, 0, 0}, {1, 0, 0, 1}, false},
        {"identity", {one, 0, 0, one}, {1, 0, 0, 1}, false},
        {"90 clockwise", {0, one, -one, 0}, {0, 1, -1, 0}, false},
        {"180", {-one, 0, 0, -one}, {-1, 0, 0, -1}, false},
        {"270 clockwise", {0, -one, one, 0}, {0, -1, 1, 0}, false},
        {"mirror", {-one, 0, 0, one}, {-1, 0, 0, 1}, false},
        {"30 degrees", {56756, 32768, -32768, 56756}, {1, 0, 0, 1}, true},
        {"scale 2", {2 * one, 0, 0, 2 * one}, {1, 0, 0, 1}, true},
    };
    unsigned right = 0;
    for (const auto& c : cases) {
        fixture::File file; for (unsigned i = 0; i < 4; ++i) file.matrix[i] = c.matrix[i];
        file.Classic(demux::Codec::H264);
        demux::TrackInfo info{};
        allocation_probe::Begin();
        const bool parsed = Parse(file, info);
        (void)allocation_probe::End();
        bool ok = parsed && info.transform_ignored == c.ignored;
        for (unsigned i = 0; i < 4; ++i) ok = ok && info.transform[i] == c.want[i];
        right += ok ? 1u : 0u;
        std::printf("  tkhd %-16s -> (%d, %d, %d, %d)%s %s\n", c.name, info.transform[0], info.transform[1], info.transform[2], info.transform[3],
                    info.transform_ignored ? " ignored" : "", ok ? "ok" : "WRONG");
    }
    std::printf("Parser: %u of %zu tkhd matrices read as expected\n", right, sizeof(cases) / sizeof(cases[0]));
    CHECK(right == sizeof(cases) / sizeof(cases[0]));
}
void TestOrientation() {
    video::Orientation o;
    const std::int8_t turn90[4]{0, 1, -1, 0}, mirror[4]{-1, 0, 0, 1}, angled[4]{1, 1, 0, 1};
    CHECK(o.Upright() && o.QuarterTurns() == 0 && !o.Across() && !o.Mirrored());
    for (unsigned i = 0; i < 4; ++i) o.file[i] = turn90[i];
    CHECK(o.QuarterTurns() == 1 && o.Across() && !o.Mirrored() && !o.Upright());
    o.turns = 3; CHECK(o.QuarterTurns() == 0 && o.Upright()); // Turned back three quarters: as stored.
    o.turns = 1; CHECK(o.QuarterTurns() == 2 && !o.Across());
    for (unsigned i = 0; i < 4; ++i) o.file[i] = mirror[i];
    o.turns = 0; CHECK(o.Mirrored() && o.QuarterTurns() == 0 && !o.Across());
    CHECK(video::Orientation::Axes(turn90) && video::Orientation::Axes(mirror) && !video::Orientation::Axes(angled));
    for (unsigned i = 0; i < 4; ++i) o.file[i] = angled[i];
    CHECK(o.Upright()); // Not of the axes: shown as stored.

    // A 64x32 stored picture shown a quarter turn clockwise as 32x64: the shown top-right pixel is the stored top-left, the
    // shown bottom-left the stored bottom-right (output pixel centres to stored sample positions, scale 1).
    for (unsigned i = 0; i < 4; ++i) o.file[i] = turn90[i];
    double m[4], t[2]; o.Map(32, 64, 64, 32, m, t);
    const auto at = [&](double x, double y, double& qx, double& qy) { qx = m[0] * x + m[1] * y + t[0]; qy = m[2] * x + m[3] * y + t[1]; };
    double qx = 0, qy = 0, rx = 0, ry = 0;
    at(31.5, 0.5, qx, qy); at(0.5, 63.5, rx, ry);
    std::printf("Orientation: 90 degrees, shown top-right -> stored (%.2f, %.2f), shown bottom-left -> stored (%.2f, %.2f)\n", qx, qy, rx, ry);
    CHECK(std::fabs(qx - 0.5) < 1e-9 && std::fabs(qy - 0.5) < 1e-9 && std::fabs(rx - 63.5) < 1e-9 && std::fabs(ry - 31.5) < 1e-9);
    float fx = 1, fy = 0; o.ToStored(fx, fy); // The shown top-right corner is the stored top-left.
    float dx = 1, dy = 0; o.DirectionToStored(dx, dy); // Right as shown is up in the stored picture.
    CHECK(std::fabs(fx) < 1e-6f && std::fabs(fy) < 1e-6f && dx == 0 && dy == -1);
    // Leveled 10 degrees (and every level to 15): the turned picture's corners stay inside the stored region.
    double worst = -1;
    for (const float level : {-15.0f, -10.0f, -3.0f, 1.0f, 10.0f, 15.0f})
        for (unsigned turns = 0; turns < 4; ++turns) {
            video::Orientation v; v.turns = turns; v.level = level;
            const double w = turns % 2 ? 360 : 640, h = turns % 2 ? 640 : 360;
            v.Map(w, h, 640, 360, m, t);
            for (const double x : {0.0, w}) for (const double y : {0.0, h}) {
                at(x, y, qx, qy);
                worst = (std::max)(worst, (std::max)((std::max)(-qx, qx - 640), (std::max)(-qy, qy - 360)));
            }
        }
    std::printf("Orientation: leveled up to 15 degrees, every turn: the corners at most %.3f samples outside the region (none)\n", worst);
    CHECK(worst <= 1e-6);
}
void TestCommands() {
    ai::MediaBounds film; film.duration_100ns = 600000000; film.position_100ns = 100000000; film.seekable = true;
    using K = ai::IntentKind;
    struct Said { const char* text; K kind; };
    const Said said[]{
        {"rotate video", K::RotateVideo}, {"Rotate the picture, please", K::RotateVideo}, {"סובב תמונה", K::RotateVideo}, {"תסובב את התמונה", K::RotateVideo},
        {"tourner la vidéo", K::RotateVideo}, {"girar el video", K::RotateVideo}, {"Video drehen", K::RotateVideo}, {"поверни видео", K::RotateVideo},
        {"旋转视频", K::RotateVideo}, {"動画を回転", K::RotateVideo}, {"دور الفيديو", K::RotateVideo},
        {"rotate left", K::RotateVideoBack}, {"rotate counterclockwise", K::RotateVideoBack}, {"סובב שמאלה", K::RotateVideoBack}, {"tourner à gauche", K::RotateVideoBack},
        {"level horizon right", K::LevelRight}, {"tilt left", K::LevelLeft}, {"הטה ימינה", K::LevelRight}, {"incliner à gauche", K::LevelLeft},
        {"straighten", K::LevelHorizon}, {"יישר אופק", K::LevelHorizon}, {"redresser l'horizon", K::LevelHorizon}, {"выровняй горизонт", K::LevelHorizon},
        {"水平校正", K::LevelHorizon}, {"Horizont ausrichten", K::LevelHorizon},
        // Auto-leveling (2026-10-29): "level horizon" now finds the horizon itself.
        {"auto level", K::AutoLevel}, {"level horizon", K::AutoLevel}, {"Level the horizon, please", K::AutoLevel}, {"יישר אופק אוטומטית", K::AutoLevel},
        {"niveler l'horizon", K::AutoLevel}, {"nivelle l'horizon", K::AutoLevel}, {"horizon automatique", K::AutoLevel}, {"nivelar el horizonte automáticamente", K::AutoLevel},
        {"Horizont automatisch ausrichten", K::AutoLevel}, {"автоматически выровняй горизонт", K::AutoLevel}, {"自动水平", K::AutoLevel}, {"自動水平補正", K::AutoLevel},
        {"عدل الافق تلقائيا", K::AutoLevel},
        {"stabilize video", K::Stabilize}, {"stabilize on", K::Stabilize}, {"Stabilise the picture, please", K::Stabilize}, {"ייצב תמונה", K::Stabilize}, {"תייצב את התמונה", K::Stabilize},
        {"stabiliser la vidéo", K::Stabilize}, {"stabilise l'image", K::Stabilize}, {"estabilizar el video", K::Stabilize}, {"Video stabilisieren", K::Stabilize},
        {"стабилизируй видео", K::Stabilize}, {"稳定画面", K::Stabilize}, {"手ぶれ補正", K::Stabilize}, {"ثبت الصورة", K::Stabilize},
        {"stabilize off", K::StabilizeOff}, {"stop stabilizing", K::StabilizeOff}, {"בטל ייצוב", K::StabilizeOff}, {"désactive la stabilisation", K::StabilizeOff},
        {"Stabilisierung aus", K::StabilizeOff}, {"отключи стабилизацию", K::StabilizeOff}, {"关闭防抖", K::StabilizeOff}, {"手ぶれ補正をオフ", K::StabilizeOff},
        {"auto level off", K::AutoLevelOff}, {"stop auto level", K::AutoLevelOff}, {"בטל יישור אופק אוטומטי", K::AutoLevelOff},
        {"arrête de niveler l'horizon", K::AutoLevelOff}, {"关闭自动水平", K::AutoLevelOff},
        // Black bars (2026-10-30): the picture inside them fills the view, and back.
        {"crop black bars", K::CropBars}, {"Crop the black bars, please", K::CropBars}, {"auto crop", K::CropBars}, {"חתוך שוליים שחורים", K::CropBars},
        {"תסיר את הפסים השחורים", K::CropBars}, {"recadrer les bandes noires", K::CropBars}, {"recorta las bandas negras", K::CropBars},
        {"schwarze Balken entfernen", K::CropBars}, {"убери чёрные полосы", K::CropBars}, {"去除黑边", K::CropBars}, {"黒帯をカット", K::CropBars},
        {"قص الحواف السوداء", K::CropBars},
        {"show black bars", K::CropBarsOff}, {"auto crop off", K::CropBarsOff}, {"uncrop", K::CropBarsOff}, {"בטל חיתוך", K::CropBarsOff},
        {"afficher les bandes noires", K::CropBarsOff}, {"取消裁剪", K::CropBarsOff},
        // Neighbours keep their meaning.
        {"zoom in", K::ZoomIn}, {"track the target", K::TrackTarget}, {"look at the sound", K::AudioSaccadeOn},
        {"reset zoom", K::ZoomReset}, {"show the whole picture", K::ZoomReset}, {"undo", K::Undo},
    };
    unsigned right = 0;
    for (const auto& s : said) {
        const auto i = ai::LocalIntentDispatcher::Parse(s.text, film);
        if (i.status == ai::IntentStatus::Ok && i.kind == s.kind) ++right; else std::printf("  \"%s\" -> %s %s\n", s.text, ai::Name(i.kind), ai::Name(i.status));
    }
    std::printf("Commands: %u of %zu as expected\n", right, sizeof(said) / sizeof(said[0]));
    CHECK(right == sizeof(said) / sizeof(said[0]));
}
}

int main() {
    std::puts("Rotation tests");
    TestParser();
    TestOrientation();
    TestCommands();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "rotation tests passed", failures);
    return failures ? 1 : 0;
}
