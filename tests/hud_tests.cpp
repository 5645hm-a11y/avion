#include "media/ui/KineticHud.hpp"
#include "allocation_probe.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace media::ui;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(false)
void Model() {
    KineticHud hud; hud.Configure(1280,720,1200000000,true);
    const auto h = hud.Uniforms(); const float cx = h.center_x, cy = h.center_y-12*h.scale, y = h.center_y+h.timeline_y*h.scale;
    const float half = (h.dock_half_width-28)*h.scale;
    CHECK(sizeof(HudUniforms) == 128); CHECK(hud.Hit(cx,cy) == HitRegion::Core); CHECK(hud.Hit(cx,y) == HitRegion::Timeline);
    CHECK(hud.Hit(10,10) == HitRegion::None); CHECK(h.center_y+h.dock_half_height*h.scale < 720);
    (void)hud.Advance(1,false,300000000,1); CHECK(hud.Uniforms().progress == 0.25f);
    (void)hud.Input({PointerEvent::Kind::Down,cx,cy,2},false);
    CHECK(hud.Input({PointerEvent::Kind::Up,cx,cy,3},false).command == HudCommand::Pause);
    (void)hud.Input({PointerEvent::Kind::Down,cx,cy,4},false); (void)hud.Input({PointerEvent::Kind::Cancel,0,0,5},false);
    CHECK(hud.Input({PointerEvent::Kind::Up,cx,cy,6},false).command == HudCommand::None);
    (void)hud.Input({PointerEvent::Kind::Down,cx,y,7},false); CHECK(hud.Dragging());
    (void)hud.Input({PointerEvent::Kind::Move,cx+half*0.5f,y,1000007},false);
    const auto seek = hud.Input({PointerEvent::Kind::Up,cx+half*0.5f,y,1000008},false);
    CHECK(seek.command == HudCommand::Seek && seek.seek_target == 900000000); CHECK(hud.Committed() && !hud.Dragging());
    (void)hud.Advance(2000000,false,300000000,1); CHECK(hud.Uniforms().scrub_offset == 0.5f);
    (void)hud.Advance(3000000,false,900000000,2); CHECK(!hud.Committed() && hud.Uniforms().scrub_offset == 0);
    // Click-to-seek, out-of-bounds drag clamping and rejection.
    (void)hud.Input({PointerEvent::Kind::Down,cx-half,y,3000001},false);
    CHECK(hud.Input({PointerEvent::Kind::Up,cx-half-200,y,3000002},false).seek_target == 0);
    hud.SeekRejected(); CHECK(!hud.Committed() && hud.Uniforms().denied == 1);
    (void)hud.Input({PointerEvent::Kind::Down,cx,y,3000003},false);
    CHECK(hud.Input({PointerEvent::Kind::Up,cx+half+200,y,3000004},false).seek_target == 1200000000);
    // No duplicate seeks during animation; a seek without a landing times out.
    std::int64_t t = 3000004;
    for (unsigned i = 0; i < 500; ++i) CHECK(hud.Advance(t += 83333,false,900000000,2).command == HudCommand::None);
    CHECK(!hud.Committed());
    (void)hud.Input({PointerEvent::Kind::FocusNext,0,0,++t},false); CHECK(hud.Uniforms().focus == 1);
    CHECK(hud.Input({PointerEvent::Kind::Activate,0,0,++t},true).command == HudCommand::Resume);
    (void)hud.Input({PointerEvent::Kind::FocusNext,0,0,++t},false); CHECK(hud.Uniforms().focus == 2);
    CHECK(hud.Input({PointerEvent::Kind::SeekBack,0,0,++t},false).seek_target == 850000000);
    CHECK(hud.Input({PointerEvent::Kind::SeekForward,0,0,++t},false).seek_target == 900000000);
    CHECK(hud.Input({PointerEvent::Kind::SeekStart,0,0,++t},false).seek_target == 0);
    CHECK(hud.Input({PointerEvent::Kind::SeekEnd,0,0,++t},false).seek_target == 1200000000);
    hud.SetReducedMotion(true); (void)hud.Advance(++t,true,900000000,2); CHECK(hud.Uniforms().freeze == 1 && hud.Uniforms().visibility == 1);
    (void)hud.Input({PointerEvent::Kind::Move,std::numeric_limits<float>::quiet_NaN(),0,++t},false);
    CHECK(std::isfinite(hud.Uniforms().pointer_x));
    hud.Configure(640,360,0,false); CHECK(!hud.Dragging() && !hud.Committed());
    CHECK(hud.Input({PointerEvent::Kind::SeekEnd,0,0,1},false).command == HudCommand::None);
    (void)hud.Advance(2,false,INT64_MAX,1); CHECK(hud.Uniforms().progress == 0);
    hud.Configure(640,360,600000000,false); const auto small = hud.Uniforms();
    CHECK(small.center_x-small.dock_half_width*small.scale >= 15);
    // A hidden dock's first click reveals without unexpectedly pausing.
    (void)hud.Input({PointerEvent::Kind::Down,small.center_x,small.center_y,1},false);
    CHECK(hud.Input({PointerEvent::Kind::Up,small.center_x,small.center_y,2},false).command == HudCommand::None);
    for (int i = 1; i <= 600; ++i) (void)hud.Advance(i*83333ll,false,INT64_MIN,0);
    CHECK(!hud.Animating() && hud.Uniforms().progress == 0);
    (void)hud.Advance(600*83333ll+1,false,INT64_MAX,0); CHECK(hud.Uniforms().progress == 1);
}
// Resize/DPI: the dock keeps its proportions and bottom margin at every size, and interaction state survives.
void Layouts() {
    struct Size { unsigned w, h; float dpi; } sizes[]{{1280,720,1},{1920,1080,1},{3840,2160,1},{3840,2160,1.5f},{2560,1080,1},{3440,1440,1.25f},
                                                      {1080,1920,1},{720,1280,2},{640,360,1},{960,540,1.5f}};
    for (const auto& z : sizes) {
        KineticHud hud; hud.Configure(z.w,z.h,600000000,true,z.dpi); const auto u = hud.Uniforms();
        const float dip_fit = std::fmin(z.w / (1280 * z.dpi), z.h / (720 * z.dpi));
        CHECK(std::fabs(u.scale - z.dpi * std::fmin(std::fmax(dip_fit,0.8f),1.25f)) < 1e-5f);
        CHECK(u.center_x == z.w * 0.5f && std::fabs(z.h - (u.center_y + u.dock_half_height * u.scale) - 26 * u.scale) < 1e-3f);
        // Full-width docks keep the 280:52 capsule; narrow windows only narrow it, keeping 16 scaled px of margin.
        const float left = u.center_x - u.dock_half_width * u.scale;
        CHECK(u.dock_half_width == 280 || std::fabs(left - 16 * u.scale) < 1e-2f);
        CHECK(left >= 16 * u.scale - 1e-2f && u.dock_half_height == 52);
        CHECK(hud.Hit(u.center_x, u.center_y - 12 * u.scale) == HitRegion::Core);
        CHECK(hud.Hit(u.center_x, u.center_y + u.timeline_y * u.scale) == HitRegion::Timeline);
    }
    CHECK(KineticHud::LayoutScale(3840,2160,1.5f) == 1.875f && KineticHud::LayoutScale(1280,720,1) == 1);
    CHECK(KineticHud::LayoutScale(1280,720,std::numeric_limits<float>::quiet_NaN()) == 1);
    // A drag in progress survives a resize (e.g. a DPI change) and commits against the new geometry.
    KineticHud hud; hud.Configure(1280,720,1200000000,true); auto u = hud.Uniforms();
    (void)hud.Input({PointerEvent::Kind::Down,u.center_x,u.center_y+u.timeline_y*u.scale,1},false); CHECK(hud.Dragging());
    hud.Resize(1920,1080,1.5f); u = hud.Uniforms(); CHECK(hud.Dragging() && u.scale == 1.5f && u.pointer_x == -1000);
    const float half = (u.dock_half_width - 28) * u.scale, y = u.center_y + u.timeline_y * u.scale;
    const auto seek = hud.Input({PointerEvent::Kind::Up,u.center_x+half,y,2},false);
    CHECK(seek.command == HudCommand::Seek && seek.seek_target == 1200000000);
    hud.Resize(640,360,1); (void)hud.Advance(3,false,1200000000,1); CHECK(!hud.Committed() && hud.Uniforms().visibility == 1);
}
// Volume pill: the engine's first report is a baseline; a change reveals the dock and the pill, the fill glides to the
// new level, and both fade 1.5 s later. A mute stays in the pill. The wheel and a drag on the pill emit volume actions.
void Volume() {
    KineticHud hud; hud.Configure(1280,720,1200000000,false);
    std::int64_t t = 0;
    const auto run = [&](unsigned frames) { for (unsigned i = 0; i < frames; ++i) (void)hud.Advance(t += 83333,false,0,0); };
    const auto& u = hud.Uniforms();
    CHECK(u.volume_half_width == 60 && u.volume_x == 104);
    CHECK(hud.Input({PointerEvent::Kind::Wheel,0,1,t},false).command == HudCommand::None); // Volume not known yet.
    hud.SetVolume(1,false,t); run(600);
    CHECK(!hud.Animating() && u.volume_visibility == 0 && u.volume == 1 && u.muted == 0);
    hud.SetVolume(1,false,t); run(12); CHECK(!hud.Animating()); // An unchanged report shows nothing.
    hud.SetVolume(0.5f,false,t); // A key press somewhere else.
    run(1); CHECK(u.volume < 1 && u.volume > 0.5f && u.volume_visibility > 0 && u.volume_visibility < 1); // Animated, not snapped.
    float previous = u.volume; bool monotonic = true;
    for (unsigned i = 0; i < 59; ++i) { run(1); monotonic = monotonic && u.volume <= previous && u.volume >= 0.5f; previous = u.volume; }
    CHECK(monotonic && u.volume == 0.5f && u.volume_visibility > 0.99f && u.visibility > 0.99f); // 0.5 s on: settled, dock awake.
    run(110); CHECK(u.volume_visibility == 1 && u.visibility > 0.99f);  // 1.42 s after the change: still shown.
    run(20); CHECK(u.volume_visibility < 1);                            // 1.58 s: fading.
    run(240); CHECK(u.volume_visibility == 0 && !hud.Animating());
    const float y = u.center_y + KineticHud::kVolumeY * u.scale;
    const float from = u.center_x + (u.volume_x - u.volume_half_width + KineticHud::kVolumeTrackLeft) * u.scale;
    const float to = u.center_x + (u.volume_x + u.volume_half_width - KineticHud::kVolumeTrackRight) * u.scale;
    CHECK(hud.Hit((from + to) / 2,y) == HitRegion::None); // A hidden pill takes no clicks.
    // Mute: the cue stays in the pill for as long as it lasts, without keeping the dock awake.
    hud.SetVolume(0.5f,true,t); run(120); CHECK(u.muted == 1 && u.volume_visibility == 1 && hud.Animating());
    run(600); CHECK(u.muted == 1 && u.volume_visibility == 1 && !hud.Animating());
    hud.SetVolume(0.5f,false,t); run(120); CHECK(u.muted == 0 && u.volume_visibility == 1);
    // Wheel: 5 % per notch on the keyboard's grid, clamped at both ends.
    auto action = hud.Input({PointerEvent::Kind::Wheel,0,1,t},false); CHECK(action.command == HudCommand::Volume && action.volume == 0.55f);
    action = hud.Input({PointerEvent::Kind::Wheel,0,-3,t},false); CHECK(action.command == HudCommand::Volume && action.volume == 0.40f);
    hud.SetVolume(0.40f,false,t); // The engine confirms.
    CHECK(hud.Input({PointerEvent::Kind::Wheel,0,30,t},false).volume == 1); CHECK(hud.Input({PointerEvent::Kind::Wheel,0,-30,t},false).volume == 0);
    CHECK(hud.Input({PointerEvent::Kind::Wheel,0,0.2f,t},false).command == HudCommand::None);
    // Drag on the pill: the level follows the pointer along the track and stops at release.
    run(24); CHECK(u.volume_visibility == 1 && u.volume < 0.05f && hud.Hit((from + to) / 2,y) == HitRegion::Volume);
    action = hud.Input({PointerEvent::Kind::Down,(from + to) / 2,y,++t},false);
    CHECK(action.command == HudCommand::Volume && std::fabs(action.volume - 0.5f) < 1e-4f);
    CHECK(hud.Input({PointerEvent::Kind::Move,to + 500,y,++t},false).volume == 1);
    action = hud.Input({PointerEvent::Kind::Move,from - 500,y,++t},false); CHECK(action.command == HudCommand::Volume && action.volume == 0);
    CHECK(hud.Input({PointerEvent::Kind::Up,from,y,++t},false).command == HudCommand::None);
    CHECK(hud.Input({PointerEvent::Kind::Move,to,y,++t},false).command == HudCommand::None);
    (void)hud.Input({PointerEvent::Kind::Down,(from + to) / 2,y,++t},false); (void)hud.Input({PointerEvent::Kind::Cancel,0,0,++t},false);
    CHECK(hud.Input({PointerEvent::Kind::Move,to,y,++t},false).command == HudCommand::None);
    // Reduced motion snaps; garbage volumes are silence; a dock too narrow for the pill still wakes on a change.
    hud.SetReducedMotion(true); hud.SetVolume(0.75f,true,t); run(1); CHECK(u.volume == 0.75f && u.muted == 1 && u.volume_visibility == 1);
    hud.SetVolume(std::numeric_limits<float>::quiet_NaN(),false,t); run(1); CHECK(u.volume == 0 && u.muted == 0);
    hud.SetVolume(7,false,t); run(1); CHECK(u.volume == 1);
    KineticHud narrow; narrow.Configure(400,225,600000000,false); const auto& n = narrow.Uniforms();
    CHECK(n.volume_half_width == 0);
    narrow.SetVolume(1,false,0); narrow.SetVolume(0.5f,false,1);
    for (int i = 1; i <= 60; ++i) (void)narrow.Advance(i * 83333ll,false,0,0);
    CHECK(n.volume_visibility == 0 && n.visibility > 0.99f);
}
}
int main() { allocation_probe::Begin(); Model(); Layouts(); Volume(); const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
    std::printf("Glass dock: %d failures, %llu allocations\n",failures,static_cast<unsigned long long>(allocations)); return failures ? 1 : 0; }
