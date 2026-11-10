#include "media/render/D3D12Renderer.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
struct Commands { bool paused = false; unsigned toggles = 0, seeks = 0, volumes = 0; float volume = 0; };
bool Handle(void* context, media::ui::HudAction action) noexcept {
    auto& c = *static_cast<Commands*>(context);
    if (action.command == media::ui::HudCommand::Seek) { ++c.seeks; return false; }
    if (action.command == media::ui::HudCommand::Volume) { ++c.volumes; c.volume = action.volume; return true; }
    c.paused = action.command == media::ui::HudCommand::Pause; ++c.toggles; return true;
}
std::int64_t Now(std::int64_t frequency) {
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart / frequency * 10000000 + t.QuadPart % frequency * 10000000 / frequency;
}
}
int main() {
    media::render::D3D12Renderer renderer; media::clock::PredictiveMasterClock clock; Commands commands;
    CHECK(renderer.Prepare(640, 480, 2, false)); if (failures) return 1;
    renderer.ConfigureHud(600000000, true, Handle, &commands);
    const auto window = renderer.Window(); const auto h = renderer.HudState();
    const auto core = MAKELPARAM(static_cast<WORD>(h.center_x), static_cast<WORD>(h.center_y));
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
    allocation_probe::Begin();
    // These messages go through the production window procedure on its owning UI thread.
    SendMessageW(window, WM_MOUSEMOVE, 0, core);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, core); SendMessageW(window, WM_LBUTTONUP, 0, core);
    CHECK(commands.paused && commands.toggles == 1);
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 1 | (LPARAM{1} << 30)); CHECK(commands.toggles == 1); // Auto-repeat suppressed.
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 1); CHECK(!commands.paused && commands.toggles == 2);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, core);
    SendMessageW(window, WM_CANCELMODE, 0, 0); SendMessageW(window, WM_LBUTTONUP, 0, core); CHECK(commands.toggles == 2);
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    // Mouse wheel: one volume step per notch once the engine's volume is known; half notches accumulate.
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0); CHECK(commands.volumes == 0);
    renderer.SetVolume(0.5f, false);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0); CHECK(commands.volumes == 1 && commands.volume == 0.55f);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), 0); CHECK(commands.volumes == 2 && commands.volume == 0.45f);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), 0); CHECK(commands.volumes == 2);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), 0); CHECK(commands.volumes == 3 && commands.volume == 0.5f);
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 1); CHECK(commands.paused);
    const auto end = Now(f.QuadPart) + 3000000;
    while (Now(f.QuadPart) < end) {
        CHECK(renderer.Messages());
        CHECK(renderer.Present(clock, Now(f.QuadPart), {false, commands.paused}) != media::render::RenderStatus::Failed);
        YieldProcessor();
    }
    CHECK(renderer.PausedHudPresents() > 1); CHECK(renderer.Presented() == 0); // UI-only presents do not count as video.
    CHECK(renderer.VolumePresents() > 1 && renderer.HudState().volume == 0.5f); // The pill was drawn and settled on the level.
    // Resize through the real WM_SIZE path: applied to the swap chain on the next Present, the dock re-laid out.
    const auto present = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            CHECK(renderer.Messages());
            CHECK(renderer.Present(clock, Now(f.QuadPart), {false, commands.paused}) != media::render::RenderStatus::Failed); Sleep(1);
        }
    };
    SetWindowPos(window, nullptr, 0, 0, 960, 540, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); CHECK(renderer.ResizePending());
    present(40);
    CHECK(!renderer.ResizePending() && renderer.Width() == 960 && renderer.Height() == 540 && renderer.Resizes() == 1);
    CHECK(renderer.HudState().center_x == 480 && renderer.HudState().scale == media::ui::KineticHud::LayoutScale(960, 540, renderer.DpiScale()));
    // Borderless resize edges and the drag strip; the rest is client area for the controls.
    RECT frame{}; GetWindowRect(window, &frame);
    const auto hit = [&](LONG x, LONG y) { return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(frame.left + x, frame.top + y)); };
    CHECK(hit(1, 270) == HTLEFT && hit(958, 270) == HTRIGHT && hit(480, 538) == HTBOTTOM && hit(1, 1) == HTTOPLEFT && hit(480, 12) == HTCAPTION);
    CHECK(hit(480, 270) == HTCLIENT);
    // F11 enters borderless fullscreen on this monitor; Alt+Enter (a system key) leaves it and restores 960x540.
    SendMessageW(window, WM_KEYDOWN, VK_F11, 1); present(40);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    CHECK(renderer.Fullscreen() && renderer.Width() == static_cast<unsigned>(monitor.rcMonitor.right - monitor.rcMonitor.left) &&
          renderer.Height() == static_cast<unsigned>(monitor.rcMonitor.bottom - monitor.rcMonitor.top));
    CHECK(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(monitor.rcMonitor.left + 1, monitor.rcMonitor.top + 1)) == HTCLIENT); // No edges.
    SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, 1 | (LPARAM{1} << 29)); present(40);
    CHECK(!renderer.Fullscreen() && renderer.Width() == 960 && renderer.Height() == 540 && renderer.Resizes() == 3);
    // Per-monitor DPI change (as when dragged to a 150 % display): Windows suggests a scaled rectangle; the dock follows.
    GetWindowRect(window, &frame); RECT suggested{frame.left, frame.top, frame.left + 1440, frame.top + 810};
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested)); present(40);
    CHECK(renderer.DpiScale() == 1.5f && renderer.Width() == 1440 && renderer.Height() == 810);
    CHECK(renderer.HudState().scale == media::ui::KineticHud::LayoutScale(1440, 810, 1.5f));
    GetWindowRect(window, &frame); suggested = {frame.left, frame.top, frame.left + 960, frame.top + 540};
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&suggested)); present(40);
    CHECK(renderer.DpiScale() == 1.0f && renderer.Width() == 960 && renderer.HudState().scale == media::ui::KineticHud::LayoutScale(960, 540, 1));
    // Modal move/size loop: WM_TIMER drives the streaming pump between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE only.
    unsigned pumped = 0;
    renderer.ConfigurePump([](void* context) noexcept { ++*static_cast<unsigned*>(context); }, &pumped);
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0); present(60); const unsigned during = pumped;
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0); present(5); const unsigned settled = pumped; present(60);
    CHECK(during > 0 && pumped == settled);
    const auto allocations = allocation_probe::End(); CHECK(allocations == 0); CHECK(renderer.Close());
    std::printf("Resize/fullscreen: %u swap-chain resizes; modal pump ticks %u\n", renderer.Resizes(), during);
    std::printf("HUD Win32 routing: %s (%d failures, %u paused composites, %llu allocations)\n", failures ? "FAIL" : "PASS", failures,
        renderer.PausedHudPresents(), static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
