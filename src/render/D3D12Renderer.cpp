#include "media/render/D3D12Renderer.hpp"
#include "media/render/PictureBlit.hpp"
#include "media/audio/DspProcessor.hpp"
#include <dwmapi.h>
#include "media/render/NeuralWeights.hpp"
#include "BlitVertexShader.hpp"
#include "BlitPixelShader.hpp"
#include <d3dcompiler.h>
#include <windowsx.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace media::render {
namespace {
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return b;
}
}
namespace {
// The window thread's private messages (any mode: the window procedure handles them first).
constexpr UINT kAttach = WM_APP + 0x31, kDestroy = WM_APP + 0x32, kIme = WM_APP + 0x33, kImePlace = WM_APP + 0x34, kStall = WM_APP + 0x35, kCreate = WM_APP + 0x36;
constexpr std::uint32_t kCtrl = 1, kAlt = 2, kShift = 4;
std::uint32_t Modifiers() noexcept {
    return (GetKeyState(VK_CONTROL) < 0 ? kCtrl : 0u) | (GetKeyState(VK_MENU) < 0 ? kAlt : 0u) | (GetKeyState(VK_SHIFT) < 0 ? kShift : 0u);
}
struct CreateRequest { void* renderer; int width, height; HWND window; HANDLE done; HWND parent; }; // parent: embedding (API 12), or null.
// A window of another thread is attached, detached and destroyed by its own thread (SendMessage: done when it returns).
void AttachWindow(HWND window, void* renderer) noexcept {
    if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    else (void)SendMessageW(window, kAttach, 0, reinterpret_cast<LPARAM>(renderer));
}
void DestroyEngineWindow(HWND window) noexcept {
    if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) DestroyWindow(window);
    else (void)SendMessageW(window, kDestroy, 0, 0);
}
}
LRESULT CALLBACK D3D12Renderer::WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
    auto* self = reinterpret_cast<D3D12Renderer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<D3D12Renderer*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (message) {
    case kAttach: { // On the window's thread, between its messages: no procedure of the previous renderer is running.
        SetWindowLongPtrW(window, GWLP_USERDATA, l);
        if (auto* next = reinterpret_cast<D3D12Renderer*>(l)) next->ui_tracking_ = next->ui_captured_ = false;
        return 0;
    }
    case kDestroy: DestroyWindow(window); return 0;
    case kStall: Sleep(static_cast<DWORD>(w)); return 0; // StallWindow: a procedure that blocks (a test).
    case WM_DESTROY:
        // Embedding (API 12): the host destroying its window takes this child with it, and ends Run (Closed). The hook is this
        // thread's to remove.
        if (self && self->parent_) {
            if (self->parent_hook_) { (void)UnhookWinEvent(self->parent_hook_); self->parent_hook_ = nullptr; }
            self->closed_ = true;
            if (self->input_wake_) SetEvent(self->input_wake_);
        }
        break;
    default: break;
    }
    if (self && self->message_thread_) return self->Forward(window, message, w, l);
    if (!self) {
        if (message == WM_NCCALCSIZE && w) return 0;
        if (message == WM_GETMINMAXINFO) { reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {320, 180}; return 0; }
        return message == WM_ERASEBKGND ? 1 : DefWindowProcW(window, message, w, l);
    }
    self->keys_ = Modifiers();
    return self->Process(window, message, w, l, self->InputTime());
}
LRESULT D3D12Renderer::Forward(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
    // The window thread: what must be answered now, what only this thread may do, the rest queued (Push).
    const float dpi = static_cast<float>(ui_dpi_.load(std::memory_order_relaxed)) / USER_DEFAULT_SCREEN_DPI;
    switch (message) {
    case WM_CLOSE: closed_ = true; if (input_wake_) SetEvent(input_wake_); return 0;
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {static_cast<LONG>(320 * dpi), static_cast<LONG>(180 * dpi)}; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: if (isolate_input_.load(std::memory_order_relaxed)) return MA_NOACTIVATEANDEAT; break;
    case WM_NCHITTEST: return parent_ ? HTCLIENT : HitTest(window, l, ui_fullscreen_.load(std::memory_order_relaxed), dpi);
    case WM_IME_SETCONTEXT: return DefWindowProcW(window, message, w, l & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
    case WM_UNICHAR: if (w == UNICODE_NOCHAR) return ui_palette_.load(std::memory_order_relaxed) ? TRUE : FALSE; Push(message, w, l); return 0;
    case WM_MOUSEMOVE:
        if (!ui_tracking_) { TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0}; ui_tracking_ = TrackMouseEvent(&track) != FALSE; }
        Push(message, w, l); return 0;
    case WM_MOUSELEAVE: ui_tracking_ = false; Push(message, w, l); return 0;
    case WM_LBUTTONDOWN:
        if (parent_ && GetFocus() != window) (void)SetFocus(window); // Embedded: the click gives the picture the keyboard (shortcuts).
        Push(message, w, l); SetCapture(window); ui_captured_ = GetCapture() == window; return 0; // Held for a drag either way.
    case WM_LBUTTONUP: Push(message, w, l); if (ui_captured_) { ui_captured_ = false; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: if (ui_captured_) { ui_captured_ = false; Push(message, w, l); } return 0;
    case WM_CANCELMODE: case WM_KILLFOCUS: Push(message, w, l); if (ui_captured_) { ui_captured_ = false; ReleaseCapture(); } break;
    case WM_SIZE: Push(message, w, l); return 0;
    case WM_DPICHANGED: {
        ui_dpi_.store(HIWORD(w), std::memory_order_relaxed);
        const auto* r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Push(message, w, 0); return 0; // The rectangle is this call's: placed here.
    }
    case WM_ENTERSIZEMOVE: return 0;
    case WM_EXITSIZEMOVE: Push(message, w, l); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: Push(message, w, 0); break; // WM_SETTINGCHANGE's string is this call's.
    case WM_SYSKEYDOWN: if ((w == VK_RETURN || w == 'R') && (l & (LPARAM{1} << 29))) { Push(message, w, l); return 0; } break;
    case WM_SYSCHAR: if (w == VK_RETURN || w == 'r' || w == 'R') return 0; break;
    case WM_KEYDOWN: case WM_CHAR: case WM_MOUSEWHEEL: case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_CHAR:
        Push(message, w, l); return 0;
    case WM_IME_COMPOSITION: {
        // The strings are read here (the input context is this thread's) and travel with the event.
        wchar_t result[64], composition[64]; LONG result_bytes = 0, composition_bytes = 0;
        if (const HIMC context = ImmGetContext(window)) {
            if (l & GCS_RESULTSTR) result_bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, result, sizeof(result));
            if (l & GCS_COMPSTR) composition_bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, composition, sizeof(composition));
            (void)ImmReleaseContext(window, context);
        }
        Push(message, w, l, result, result_bytes > 0 ? static_cast<unsigned>(result_bytes) / sizeof(wchar_t) : 0u, composition,
             composition_bytes > 0 ? static_cast<unsigned>(composition_bytes) / sizeof(wchar_t) : 0u);
        return 0;
    }
    case kIme:
        if (w == 2) if (const HIMC context = ImmGetContext(window)) { (void)ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); (void)ImmReleaseContext(window, context); }
        (void)ImmAssociateContextEx(window, nullptr, w == 1 ? IACE_DEFAULT : 0);
        return 0;
    case kImePlace:
        if (const HIMC context = ImmGetContext(window)) {
            COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT; composition.ptCurrentPos = {static_cast<LONG>(w), static_cast<LONG>(LOWORD(l))};
            CANDIDATEFORM candidates{}; candidates.dwStyle = CFS_CANDIDATEPOS; candidates.ptCurrentPos = {static_cast<LONG>(w), static_cast<LONG>(HIWORD(l))};
            (void)ImmSetCompositionWindow(context, &composition); (void)ImmSetCandidateWindow(context, &candidates);
            (void)ImmReleaseContext(window, context);
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, w, l);
}
void D3D12Renderer::Push(UINT message, WPARAM w, LPARAM l, const wchar_t* result, unsigned result_length, const wchar_t* composition,
                         unsigned composition_length) noexcept {
    auto* e = input_.AcquireWriteSlot();
    if (!e) { input_lost_.fetch_add(1, std::memory_order_relaxed); return; } // 64 behind: the presentation thread is not running.
    e->message = message; e->w = w; e->l = l; e->time = InputTime(); e->keys = Modifiers();
    e->lengths[0] = (std::min)(result_length, 64u); e->lengths[1] = (std::min)(composition_length, 64u);
    for (unsigned i = 0; i < e->lengths[0]; ++i) e->text[0][i] = result[i];
    for (unsigned i = 0; i < e->lengths[1]; ++i) e->text[1][i] = composition[i];
    (void)input_.CommitWriteSlot();
    if (input_wake_) SetEvent(input_wake_);
}
bool D3D12Renderer::Down(int key) const noexcept {
    const std::uint32_t bit = key == VK_CONTROL ? kCtrl : key == VK_MENU ? kAlt : key == VK_SHIFT ? kShift : 0u;
    return (keys_ & bit) != 0;
}
LRESULT D3D12Renderer::Deliver(UINT message, WPARAM w, LPARAM l) noexcept {
    if (!message_thread_ || !window_) return window_ ? SendMessageW(window_, message, w, l) : 0;
    keys_ = Modifiers(); // This thread's key state: a script may hold Control (SetKeyboardState); no physical key reaches it.
    return Process(window_, message, w, l, InputTime());
}
bool D3D12Renderer::StallWindow(unsigned milliseconds) noexcept { return window_ && PostMessageW(window_, kStall, milliseconds, 0) != FALSE; }
void D3D12Renderer::Ime(unsigned mode) noexcept {
    if (!window_) return;
    if (message_thread_) { (void)PostMessageW(window_, kIme, mode, 0); return; }
    if (mode == 2) if (const HIMC context = ImmGetContext(window_)) { (void)ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); (void)ImmReleaseContext(window_, context); }
    (void)ImmAssociateContextEx(window_, nullptr, mode == 1 ? IACE_DEFAULT : 0);
}
DWORD WINAPI D3D12Renderer::WindowThread(void* ready) noexcept {
    // Per-monitor DPI awareness (v2) for every window made here; then the queue, and the creator may go on.
    (void)SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MSG m{}; (void)PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!m.hwnd && m.message == kCreate) {
            auto* r = reinterpret_cast<CreateRequest*>(m.lParam);
            if (r->parent) r->window = CreateChild(r->parent, r->width, r->height, r->renderer, static_cast<D3D12Renderer*>(r->renderer)->parent_hook_);
            else r->window = CreateWindowExW(0, L"MediaEngineCanvas", L"Media engine — Escape to close", WS_POPUP | WS_THICKFRAME, 80, 80, r->width, r->height,
                                             nullptr, nullptr, GetModuleHandleW(nullptr), r->renderer);
            SetEvent(r->done);
            continue;
        }
        auto* self = m.hwnd ? reinterpret_cast<D3D12Renderer*>(GetWindowLongPtrW(m.hwnd, GWLP_USERDATA)) : nullptr;
        const int admit = self ? self->Admit(m) : 2;
        if (!admit) continue;
        if (admit == 2) TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
DWORD D3D12Renderer::WindowThreadId() noexcept {
    // One for the process, started by the first renderer that asks (setup), never stopped: it owns every engine window.
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static DWORD id = 0;
    (void)InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return TRUE;
        DWORD made = 0;
        if (const HANDLE thread = CreateThread(nullptr, 0, WindowThread, ready, 0, &made)) {
            (void)SetThreadDescription(thread, L"media window");
            if (WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0) id = made;
            CloseHandle(thread);
        }
        CloseHandle(ready);
        return TRUE;
    }, nullptr, nullptr);
    return id;
}
HWND D3D12Renderer::CreateChild(HWND parent, int width, int height, void* renderer, HWINEVENTHOOK& hook) noexcept {
    // Embedding (API 12): on the thread that will own it. The child takes its parent's DPI awareness (a child of another
    // awareness is refused, or scaled, unless the host asked for mixed hosting), then a hook on the parent's moves and resizes:
    // out of context, so it runs on this thread, between its messages; no polling.
    const auto awareness = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent));
    // WS_EX_NOPARENTNOTIFY: creating (and destroying) a child sends its parent WM_PARENTNOTIFY, synchronously, to the parent's thread;
    // a host whose UI thread waits for open (and pumps nothing meanwhile) would hold this thread, and open, until it pumped.
    const HWND child = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"MediaEngineCanvas", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, width, height,
                                       parent, nullptr, GetModuleHandleW(nullptr), renderer);
    if (awareness) (void)SetThreadDpiAwarenessContext(awareness);
    if (!child) return nullptr;
    DWORD process = 0; const DWORD thread = GetWindowThreadProcessId(parent, &process);
    hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &ParentChanged, process, thread, WINEVENT_OUTOFCONTEXT);
    return child;
}
void CALLBACK D3D12Renderer::ParentChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) noexcept {
    // A window of the parent's thread moved or changed size: when it is a parent of ours, our child fills its client area again
    // (on this, the child's thread: its WM_SIZE resizes the swap chain as a user's drag does).
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !window) return;
    const HWND ours = FindWindowExW(window, nullptr, L"MediaEngineCanvas", nullptr);
    if (!ours || GetWindowThreadProcessId(ours, nullptr) != GetCurrentThreadId()) return;
    RECT parent{}, current{};
    if (!GetClientRect(window, &parent) || !GetClientRect(ours, &current) || parent.right <= 0 || parent.bottom <= 0) return; // Minimised: keep the size.
    if (parent.right != current.right || parent.bottom != current.bottom)
        (void)SetWindowPos(ours, nullptr, 0, 0, parent.right, parent.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}
LRESULT D3D12Renderer::Process(HWND window, UINT message, WPARAM w, LPARAM l, std::int64_t now) noexcept {
    // Thread-bound calls (capture, tracking, timers, placing the window, the default procedure) only on the window's own
    // thread: with a window thread (message_thread_) that thread did them before the message was queued.
    const bool own = !message_thread_;
    // Escape closes the window, unless the command bar is open: then it closes the bar (PaletteMessage).
    if (message == WM_CLOSE || (message == WM_KEYDOWN && w == VK_ESCAPE && !palette_.IsOpen() && !parent_)) { closed_ = true; return 0; } // Embedded: the host's window decides.
    // Borderless but resizable: WS_THICKFRAME keeps native sizing, and the whole window is client area.
    if (message == WM_NCCALCSIZE && w) return 0;
    if (message == WM_GETMINMAXINFO) {
        const float dpi = dpi_;
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {static_cast<LONG>(320 * dpi), static_cast<LONG>(180 * dpi)}; return 0;
    }
    {
        const float x = static_cast<float>(GET_X_LPARAM(l)), y = static_cast<float>(GET_Y_LPARAM(l));
        LRESULT handled = 0;
        if (PaletteMessage(message, w, l, now, handled)) return handled;
        switch (message) {
        case WM_CHAR:
            // By character, so they follow the keyboard layout: ] and [ the speed, . and , one picture (docs/audio-dsp.md).
            if (w == L']' || w == L'[') { HudAction({ui::HudCommand::Speed, w == L']' ? 1 : -1}); return 0; }
            if (w == L'.' || w == L',') { HudAction({ui::HudCommand::Step, w == L'.' ? 1 : -1}); return 0; }
            break;
        case WM_MOUSEACTIVATE:
            // An automated run's window is never activated by a click, and the click itself is discarded.
            if (isolate_input_.load(std::memory_order_relaxed)) return MA_NOACTIVATEANDEAT;
            break;
        case WM_MOUSEMOVE: {
            if (own && !tracking_mouse_) {
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
                tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
            }
            InputHud({ui::PointerEvent::Kind::Move, x, y, now}); return 0;
        }
        case WM_LBUTTONDOWN:
            InputHud({ui::PointerEvent::Kind::Down, x, y, now});
            if (own && hud_.Hit(x, y) != ui::HitRegion::None) { SetCapture(window); captured_ = GetCapture() == window; }
            return 0;
        case WM_LBUTTONUP:
            InputHud({ui::PointerEvent::Kind::Up, x, y, now});
            if (own && captured_) { captured_ = false; ReleaseCapture(); }
            return 0;
        case WM_MOUSEWHEEL: {
            // Volume, one 5 % step per notch. (The coordinates are screen coordinates; the wheel acts anywhere.) With Ctrl:
            // zoom, 1.25x a notch, at the pointer.
            wheel_ += GET_WHEEL_DELTA_WPARAM(w);
            const int notches = wheel_ / WHEEL_DELTA; wheel_ -= notches * WHEEL_DELTA;
            if (notches && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) && !music_) { // Music mode has no picture to zoom.
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(window, &p);
                const auto& v = video_rect_;
                const float fx = v.width ? (std::clamp)((static_cast<float>(p.x) - v.x) / v.width, 0.0f, 1.0f) : 0.5f;
                const float fy = v.height ? (std::clamp)((static_cast<float>(p.y) - v.y) / v.height, 0.0f, 1.0f) : 0.5f;
                ZoomBy(std::pow(1.25f, static_cast<float>(notches)), fx, fy);
                return 0;
            }
            if (notches) InputHud({ui::PointerEvent::Kind::Wheel, 0, static_cast<float>(notches), now});
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false; InputHud({ui::PointerEvent::Kind::Leave, 0, 0, now}); return 0;
        case WM_CAPTURECHANGED:
            // The window thread queues it only when it held the capture (it keeps that state).
            if (captured_ || !own) { captured_ = false; InputHud({ui::PointerEvent::Kind::Cancel, 0, 0, now}); }
            return 0;
        case WM_CANCELMODE: case WM_KILLFOCUS:
            palette_.Compose<wchar_t>(nullptr, 0, now); // An unfinished composition does not survive the focus.
            InputHud({ui::PointerEvent::Kind::Cancel, 0, 0, now});
            if (own && captured_) { captured_ = false; ReleaseCapture(); }
            break;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)) RequestResize(LOWORD(l), HIWORD(l));
            return 0;
        case WM_DPICHANGED: {
            // Per-monitor DPI: adopt the new scale and the size Windows suggests for it (WM_SIZE follows).
            dpi_ = static_cast<float>(HIWORD(w)) / USER_DEFAULT_SCREEN_DPI;
            if (const auto* r = reinterpret_cast<const RECT*>(l); own && r) // The window thread placed it already (and queued no rectangle).
                SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RequestResize(pending_width_ ? pending_width_ : width_, pending_height_ ? pending_height_ : height_);
            return 0;
        }
        // The modal move / size loop: a timer keeps pictures coming while it runs on this thread (not with a window thread).
        case WM_ENTERSIZEMOVE: if (own) SetTimer(window, 1, USER_TIMER_MINIMUM, nullptr); return 0;
        case WM_EXITSIZEMOVE: if (own) KillTimer(window, 1); UpdateRefresh(); return 0; // It may be on another monitor now.
        case WM_DISPLAYCHANGE: UpdateRefresh(); break;
        case WM_TIMER:
            if (w == 1) { if (pump_) pump_(pump_context_); return 0; }
            break;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN && (l & (LPARAM{1} << 29))) { if (!(l & (LPARAM{1} << 30))) ToggleFullscreen(); return 0; }
            if (w == 'R' && (l & (LPARAM{1} << 29))) { if (!(l & (LPARAM{1} << 30))) Rotate(-1); return 0; } // Alt+R: a quarter turn back.
            if (w == 'S' && (l & (LPARAM{1} << 29))) { // Alt+S: the view stabilised, or not (docs/picture.md, "Stabilising the window").
                if (!(l & (LPARAM{1} << 30)) && !SetStabilization(!Stabilization())) ShowStatus("Stabilisation \xc2\xb7 not on this picture", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (w == VK_RETURN || w == 'r' || w == 'R' || w == 's' || w == 'S') return 0; // Alt+Enter, Alt+R, Alt+S handled above; no system beep.
            break;
        case WM_KEYDOWN:
            if (zoom_ > 1.0f && (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) && !Down(VK_CONTROL)) {
                Pan(w == VK_LEFT ? -0.1f : w == VK_RIGHT ? 0.1f : 0.0f, w == VK_UP ? -0.1f : w == VK_DOWN ? 0.1f : 0.0f);
                return 0;
            }
            if (!(l & (LPARAM{1} << 30))) InputKey(static_cast<unsigned>(w));
            if (!(l & (LPARAM{1} << 30))) {
                using Kind = ui::PointerEvent::Kind;
                if (w == VK_F11 || w == 'F') { ToggleFullscreen(); return 0; }
                if (w == 'N') { // Auto -> Neural -> Edge-directed -> Fast -> Auto.
                    SetQualityMode(static_cast<QualityMode>((static_cast<unsigned>(governor_.Mode()) + 1) % 4));
                    QualityCue(true); return 0;
                }
                if (w == 'C') { guided_chroma_ = !guided_chroma_; SettingsChanged(); return 0; }
                if (w == 'R') { // R: a quarter turn clockwise (Alt+R back); Ctrl+R / Ctrl+Shift+R: a degree of level; Shift+R: auto-leveling
                    // on / off (docs/picture.md, "Rotation", "Auto-leveling the horizon").
                    const bool shift = Down(VK_SHIFT);
                    if (Down(VK_CONTROL)) Level(shift ? -1.0f : 1.0f, true); else if (shift) SetAutoLevel(!auto_level_); else Rotate(1);
                    return 0;
                }
                if (w == 'X') { SetAutoCrop(!auto_crop_); return 0; } // X: crop the black bars, and back (docs/picture.md, "Black bars").
                if (w == 'V') { CycleCompare(); return 0; } // Original | Enhanced, Pinned | Live, off.
                if (w == 'L') { HudAction({ui::HudCommand::Loop, 0}); return 0; }  // A, B, off (docs/seeking.md, "A–B loop").
                if (w == 'I') { HudAction({ui::HudCommand::Stats, 0}); return 0; } // The telemetry panel.
                if (w == 'K') { HudAction({ui::HudCommand::Night, 0}); return 0; } // Night mode.
                if (w == 'T') { HudAction({ui::HudCommand::Track, 0}); return 0; } // Target tracking on / off.
                // S: a forensic snapshot of the picture as shown (docs/picture.md, "Forensic snapshots"); Shift+S is the host's (binaural).
                if (w == 'S' && !Down(VK_SHIFT) && !Down(VK_CONTROL)) { HudAction({ui::HudCommand::Snapshot, 0}); return 0; }
                if (w == 'B') { HudAction({ui::HudCommand::Bookmark, Down(VK_CONTROL) ? 1 : 0}); return 0; } // Bookmark; Ctrl: pin the frame.
                if (w == VK_TAB) { // Tab: the next target while tracking (docs/picture.md, "Several objects at once"), else the dock's focus.
                    if (Tracking() && targets_ready_) HudAction({ui::HudCommand::NextTarget, 0}); else InputHud({Kind::FocusNext,0,0,now});
                    return 0;
                }
                // Page Down / Page Up: the next chapter, or the start of this one / the previous one (docs/chapters.md).
                if (w == VK_NEXT || w == VK_PRIOR) { HudAction({ui::HudCommand::Chapter, w == VK_NEXT ? 1 : -1}); return 0; }
                if (w == VK_RETURN) { InputHud({Kind::Activate,0,0,now}); return 0; }
                // Ctrl+Right / Ctrl+Left: the next or previous file in the folder (docs/session.md).
                if ((w == VK_LEFT || w == VK_RIGHT) && Down(VK_CONTROL) && !Down(VK_MENU)) {
                    HudAction({ui::HudCommand::File, w == VK_RIGHT ? 1 : -1}); return 0;
                }
                if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END) {
                    const Kind kind = w == VK_LEFT ? Kind::SeekBack : w == VK_RIGHT ? Kind::SeekForward : w == VK_HOME ? Kind::SeekStart : Kind::SeekEnd;
                    InputHud({kind,0,0,now}); return 0;
                }
            }
            if (w == VK_SPACE) {
                if (!(l & (LPARAM{1} << 30))) InputHud({ui::PointerEvent::Kind::TogglePause, 0, 0, now});
                return 0;
            }
            break;
        case WM_SETTINGCHANGE: {
            BOOL animations = TRUE; (void)SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
            hud_.SetReducedMotion(animations == FALSE); palette_.SetReducedMotion(animations == FALSE);
            status_.SetReducedMotion(animations == FALSE); break;
        }
        case WM_NCHITTEST: return parent_ ? HTCLIENT : HitTest(window, l, fullscreen_, dpi_);
        }
    }
    if (message == WM_ERASEBKGND) return 1;
    return own ? DefWindowProcW(window, message, w, l) : 0;
}
LRESULT D3D12Renderer::HitTest(HWND window, LPARAM l, bool fullscreen, float dpi) noexcept {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(window, &p);
    RECT client{}; GetClientRect(window, &client);
    if (!fullscreen) {
        // Resize edges (6 DIPs), then a narrow top drag strip; the rest stays client for the HUD.
        const LONG edge = static_cast<LONG>(6 * dpi);
        const bool left = p.x < edge, right = p.x >= client.right - edge, top = p.y < edge, bottom = p.y >= client.bottom - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    return !fullscreen && p.y >= 0 && p.y < static_cast<LONG>(24 * dpi) ? HTCAPTION : HTCLIENT;
}
bool D3D12Renderer::PaletteMessage(UINT message, WPARAM w, LPARAM l, std::int64_t now, LRESULT& result) noexcept {
    result = 0;
    if (!window_) return false; // Still inside CreateWindow.
    const bool repeat = (l & (LPARAM{1} << 30)) != 0;
    // Control alone: AltGr (Control+Alt) types characters on many layouts.
    const bool control = message == WM_KEYDOWN && Down(VK_CONTROL) && !Down(VK_MENU);
    // An input method never draws its own composition window here: the bar shows the composition itself.
    if (message == WM_IME_SETCONTEXT && !message_thread_) { result = DefWindowProcW(window_, message, w, l & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW)); return true; }
    if (message == WM_UNICHAR && w == UNICODE_NOCHAR) { result = palette_.Available() ? TRUE : FALSE; return true; }
    if (!palette_.IsOpen()) {
        // Opening is by character, so it follows the keyboard layout: `/` wherever the layout has it.
        if (message == WM_CHAR && w == L'/') return OpenPalette();
        if ((w == 'F' || w == 'K') && control && palette_.Available()) {
            if (!repeat) (void)OpenPalette();
            return true;
        }
        return false;
    }
    switch (message) {
    case WM_KEYDOWN: {
        if (w == VK_ESCAPE) ClosePalette();
        else if (w == VK_RETURN) { if (!repeat) SubmitPalette(); }
        else if (w == VK_BACK) { if (control) (void)palette_.DeleteWord(now); else (void)palette_.Backspace(now); }
        else if (w == 'V' && control) PastePalette();
        else if (w == VK_F11 && !repeat) ToggleFullscreen();
        return true; // Typing: no playback shortcut fires, and the key's character follows as WM_CHAR.
    }
    case WM_CHAR:
        if (w >= 0x20 && w != 0x7f) (void)palette_.Utf16(static_cast<std::uint16_t>(w), now); // Controls came as keys.
        return true;
    case WM_UNICHAR: (void)palette_.Scalar(static_cast<std::uint32_t>(w), now); return true;
    case WM_IME_STARTCOMPOSITION: PlaceIme(); return true;
    case WM_IME_COMPOSITION: if (!message_thread_) ImeComposition(l); return true; // With a window thread: Messages() (its strings).
    case WM_IME_ENDCOMPOSITION: palette_.Compose<wchar_t>(nullptr, 0, now); return true;
    case WM_IME_CHAR: return true; // Already taken from the composition's result string.
    case WM_LBUTTONDOWN:
        if (!palette_.Hit(static_cast<float>(GET_X_LPARAM(l)), static_cast<float>(GET_Y_LPARAM(l)))) ClosePalette();
        return true;
    default: return false;
    }
}
namespace {
// A session's passes keep their text buffers in the session's arena, which outlives every file's.
core::LinearMemoryArena* SessionArena(GpuSession& gpu) noexcept {
    if (!gpu.arena.Statistics().capacity_bytes && !gpu.arena.Initialize(4u * 1024 * 1024)) return nullptr;
    return &gpu.arena;
}
}
bool D3D12Renderer::PreparePalette(core::LinearMemoryArena& arena) noexcept {
    if (!device_ || palette_.Available()) return false;
    auto* storage = static_cast<char*>(arena.Allocate(ui::CommandPalette::kStorageBytes));
    if (!storage) return false;
    if (!palette_pipeline_.Ready()) { // A warm session's next file finds it ready.
        auto* host = session_ ? SessionArena(gpu_) : &arena;
        const HRESULT hr = host ? palette_pipeline_.Prepare(device_, *host) : E_OUTOFMEMORY;
        if (FAILED(hr)) { error_ = hr; return false; }
    }
    palette_.Configure({storage, ui::CommandPalette::kStorageBytes}); palette_.ContinueRevision(gpu_.palette_revision); palette_.Layout(width_, height_, dpi_);
    ui_palette_.store(true, std::memory_order_relaxed); // The window thread's answer to WM_UNICHAR's question.
    return true;
}
bool D3D12Renderer::PrepareStatus(core::LinearMemoryArena& arena) noexcept {
    if (!device_) return false;
    if (!status_pipeline_.Ready()) { // A warm session's next file finds it ready.
        auto* host = session_ ? SessionArena(gpu_) : &arena;
        const HRESULT hr = host ? status_pipeline_.Prepare(device_, *host) : E_OUTOFMEMORY;
        if (FAILED(hr)) { error_ = hr; return false; }
    }
    status_.ContinueRevision(gpu_.status_revision); status_.Layout(width_, height_, dpi_);
    return true;
}
void D3D12Renderer::ShowStatus(std::string_view utf8, ui::StatusIcon icon, ui::StatusTone tone, float hold_seconds) noexcept {
    if (!status_pipeline_.Ready()) return;
    const auto now = InputTime();
    status_.Show(utf8, icon, tone, now, hold_seconds);
    status_pipeline_.Update(status_, now); // The raster is asked for at once.
}
bool D3D12Renderer::OpenPalette() noexcept {
    if (!palette_.Open(InputTime())) return false;
    previewed_ = palette_.Revision() - 1;
    Ime(1); // Input methods compose only while the bar takes text.
    return true;
}
void D3D12Renderer::ClosePalette() noexcept {
    if (!palette_.IsOpen()) return;
    Ime(2);
    palette_.Close(InputTime());
}
void D3D12Renderer::SubmitPalette() noexcept {
    const auto text = palette_.Text();
    if (text.empty()) { ClosePalette(); return; }
    ++palette_submits_;
    if (palette_submit_ && palette_submit_(palette_context_, text)) ClosePalette();
    else { palette_.Rejected(InputTime()); ++palette_rejections_; }
}
void D3D12Renderer::PastePalette() noexcept {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(window_)) return;
    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            (void)palette_.Insert(text, GlobalSize(data) / sizeof(wchar_t), InputTime());
            (void)GlobalUnlock(data);
        }
    }
    (void)CloseClipboard();
}
void D3D12Renderer::ImeComposition(LPARAM flags) noexcept {
    const HIMC context = ImmGetContext(window_);
    if (!context) return;
    const auto now = InputTime();
    wchar_t text[256];
    // The committed result first (it replaces the composition), then what is still being composed.
    if (flags & GCS_RESULTSTR) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, text, sizeof(text));
        if (bytes > 0) (void)palette_.Insert(text, static_cast<std::size_t>(bytes) / sizeof(wchar_t), now);
    }
    if (flags & GCS_COMPSTR) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, text, sizeof(text));
        palette_.Compose(text, bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(wchar_t) : 0, now);
    } else if ((flags & GCS_RESULTSTR) || !flags) palette_.Compose<wchar_t>(nullptr, 0, now); // Committed, or cancelled.
    (void)ImmReleaseContext(window_, context);
    PlaceIme();
}
void D3D12Renderer::PlaceIme() noexcept {
    const auto lane = palette_.TextRect();
    const LONG x = lane.x + static_cast<LONG>(palette_pipeline_.Stats().caret);
    if (message_thread_) { // Placed by the window's thread: the caret, and the candidates under the lane.
        if (window_) (void)PostMessageW(window_, kImePlace, static_cast<WPARAM>(x), MAKELPARAM(lane.y, lane.y + static_cast<LONG>(lane.height)));
        return;
    }
    const HIMC context = ImmGetContext(window_);
    if (!context) return;
    COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT; composition.ptCurrentPos = {x, lane.y};
    CANDIDATEFORM candidates{}; candidates.dwStyle = CFS_CANDIDATEPOS; candidates.ptCurrentPos = {x, lane.y + static_cast<LONG>(lane.height)};
    (void)ImmSetCompositionWindow(context, &composition); (void)ImmSetCandidateWindow(context, &candidates);
    (void)ImmReleaseContext(window_, context);
}
D3D12Renderer::~D3D12Renderer() noexcept { Close(); }
bool D3D12Renderer::Prepare(unsigned width, unsigned height, unsigned surfaces, bool visible, unsigned reorder_depth, bool activate,
                            unsigned video_width, unsigned video_height, video::SampleAspect sample_aspect) noexcept {
    if ((device_ && !session_) || !width || !height || width > 8192 || height > 8192 || !surfaces || surfaces > 32) return false;
    if (session_) {
        // A warm session's window, and the swap chain on it. A window closed in between takes its swap chain with it: the
        // session's objects are made again.
        if (gpu_.window.window && !IsWindow(gpu_.window.window)) gpu_.window = {};
        if (device_ && !gpu_.window.window) { (void)WaitIdle(); ReleaseShared(); Release(device_); }
        if (gpu_.window.window) adopt_ = gpu_.window;
    }
    if (texture_output_) {
        // Texture output (API 13): no swap chain; the (hidden) window and the composites are the textures' size.
        if (output_width_ < 16 || output_height_ < 16 || output_width_ > 8192 || output_height_ > 8192) { error_ = E_INVALIDARG; return false; }
        width = output_width_; height = output_height_;
    }
    if (parent_) {
        // Embedding (API 12): the window is the host's client area, whatever the video's shape (the picture is fitted inside it).
        RECT client{};
        if (!IsWindow(parent_) || !GetClientRect(parent_, &client)) { error_ = E_INVALIDARG; return false; }
        width = (std::clamp)(static_cast<unsigned>((std::max)(client.right, LONG{64})), 64u, 8192u);
        height = (std::clamp)(static_cast<unsigned>((std::max)(client.bottom, LONG{64})), 64u, 8192u);
        adopt_ = {}; // A session's window is never adopted into a host's.
    }
    sample_aspect_ = video::Normalize(sample_aspect);
    targets_ready_ = false; retarget_ = false; target_id_ = 0; targets_epoch_ = UINT32_MAX; // PrepareTargets for this file, from its arena.
    warm_pts_ = perceived_pts_ = INT64_MIN; warm_epoch_ = UINT32_MAX; warm_grids_ = 0;
    if (!order_.Configure(reorder_depth)) { error_ = E_INVALIDARG; return false; }
    width_ = width; height_ = height; surfaces_ = surfaces;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || frequency.QuadPart > INT64_MAX / 10000000) return false;
    input_frequency_ = frequency.QuadPart;
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MediaEngineCanvas";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // Per-monitor DPI awareness (v2) for this window, whatever the process default: physical pixels, crisp scaling
    // and WM_DPICHANGED when moved between monitors.
    const auto awareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT client{};
    if (adopt_.window && IsWindow(adopt_.window) && GetClientRect(adopt_.window, &client) && client.right > 0 && client.bottom > 0) {
        // The previous file's window: its size, place and fullscreen state stay as the viewer left them.
        window_ = adopt_.window; adopted_ = true;
        if (message_thread_ && !input_wake_) input_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        AttachWindow(window_, this);
        width = (std::min)(static_cast<unsigned>(client.right), 8192u); height = (std::min)(static_cast<unsigned>(client.bottom), 8192u);
        width_ = width; height_ = height;
        // A session's swap chain keeps its buffers' size; a window resized in between is caught up like any resize.
        if (session_ && swap_ && (gpu_.width != width || gpu_.height != height)) { width_ = gpu_.width; height_ = gpu_.height; pending_width_ = width; pending_height_ = height; resize_pending_ = true; }
        fullscreen_ = adopt_.fullscreen; restore_ = adopt_.restore;
    } else if (message_thread_) {
        // Made by the window thread (its messages arrive there); this thread waits for it (setup).
        if (!input_wake_) input_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CreateRequest request{this, static_cast<int>(width), static_cast<int>(height), nullptr, CreateEventW(nullptr, TRUE, FALSE, nullptr), parent_};
        const DWORD thread = WindowThreadId();
        if (input_wake_ && thread && request.done && PostThreadMessageW(thread, kCreate, 0, reinterpret_cast<LPARAM>(&request)))
            (void)WaitForSingleObject(request.done, 10000);
        if (request.done) CloseHandle(request.done);
        window_ = request.window;
    } else if (parent_) {
        window_ = CreateChild(parent_, static_cast<int>(width), static_cast<int>(height), this, parent_hook_); // This thread pumps its hook (Messages).
    } else {
        window_ = CreateWindowExW(0, wc.lpszClassName, L"Media engine — Escape to close", WS_POPUP | WS_THICKFRAME,
            80, 80, static_cast<int>(width), static_cast<int>(height), nullptr, nullptr, wc.hInstance, this);
    }
    // Largest picture the video can occupy: its aspect fitted into the whole desktop (it may span monitors).
    capacity_width_ = width; capacity_height_ = height;
    if (video_width && video_height) {
        const unsigned desktop_width = (std::min)((std::max)(static_cast<unsigned>((std::max)(GetSystemMetrics(SM_CXVIRTUALSCREEN), 0)), width), 8192u);
        const unsigned desktop_height = (std::min)((std::max)(static_cast<unsigned>((std::max)(GetSystemMetrics(SM_CYVIRTUALSCREEN), 0)), height), 8192u);
        capacity_width_ = desktop_width; capacity_height_ = desktop_height; // Fit() clamps into the capacity.
        auto shape = video::DisplaySize(video_width, video_height, sample_aspect_); // Square pixels.
        if (orientation_.Across()) std::swap(shape.width, shape.height); // The file is shown a quarter turn round (tkhd).
        // The slots hold the picture either way round (the viewer may turn it a quarter): the larger fit along each axis.
        unsigned across_width = capacity_width_, across_height = capacity_height_;
        Fit(shape.width, shape.height, capacity_width_, capacity_height_); Fit(shape.height, shape.width, across_width, across_height);
        unsigned initial_width = width, initial_height = height; Fit(shape.width, shape.height, initial_width, initial_height);
        capacity_width_ = (std::max)({capacity_width_, initial_width, across_width}); capacity_height_ = (std::max)({capacity_height_, initial_height, across_height});
    }
    // Music mode shows no picture: its slots are placeholders, and a session's kept ones wait for the next video file.
    if (music_) capacity_width_ = capacity_height_ = 64;
    if (awareness) (void)SetThreadDpiAwarenessContext(awareness);
    if (!window_) return false;
    if (!resize_pending_) { width_ = width; height_ = height; }
    seek_requested_ = InputTime();
    const UINT dpi = GetDpiForWindow(window_); dpi_ = dpi ? static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0f;
    ui_dpi_.store(dpi ? dpi : USER_DEFAULT_SCREEN_DPI, std::memory_order_relaxed); ui_fullscreen_.store(fullscreen_, std::memory_order_relaxed);
    hud_.Configure(width, height, 0, false, dpi_);
    UpdateRefresh();
    // Keys are playback shortcuts until the command bar opens: no input method composes them meanwhile.
    Ime(0);
    video_width_ = video_width; video_height_ = video_height;
    governor_.Configure();
    if (!CreateGpu()) return false;
    if (visible) ShowWindow(window_, activate && !parent_ ? SW_SHOW : SW_SHOWNOACTIVATE); // Embedded: the host's window keeps the focus.
    if (visible && fresh_swap_ && !WarmUpFlip()) return false;
    return true;
}
bool D3D12Renderer::WarmUpFlip() noexcept {
    // Cold start (docs/seeking.md, "Pre-roll and the presentation queue"): a new swap chain's first image goes through the
    // compositor here, in Preparing, before playback and its clock exist. Its first Present and first composition (the
    // compositor setting up the window's surface) are the slow ones; a stall there now delays the start, never a picture.
    // One black image, presented, then DwmFlush: back once the compositor has composed what was outstanding.
    fresh_swap_ = false;
    const auto begin = InputTime();
    if (WaitForSingleObject(latency_, 1000) != WAIT_OBJECT_0) { error_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT); return false; } // As every composite does.
    const unsigned index = swap_->GetCurrentBackBufferIndex(); auto& c = composites_[index];
    error_ = c.allocator->Reset(); if (SUCCEEDED(error_)) error_ = c.commands->Reset(c.allocator, nullptr);
    if (FAILED(error_)) return false;
    auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += static_cast<SIZE_T>(index) * rtv_stride_;
    constexpr float black[]{0, 0, 0, 1};
    auto b = Barrier(buffers_[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET); c.commands->ResourceBarrier(1, &b);
    c.commands->ClearRenderTargetView(rtv, black, 0, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter); c.commands->ResourceBarrier(1, &b);
    error_ = c.commands->Close(); if (FAILED(error_)) return false;
    ID3D12CommandList* lists[]{c.commands}; queue_->ExecuteCommandLists(1, lists);
    error_ = swap_->Present(0, 0); if (FAILED(error_)) return false;
    c.fence = ++sequence_; error_ = queue_->Signal(fence_, c.fence); if (FAILED(error_)) return false;
    const auto presented = InputTime();
    warm_up_result_ = DwmFlush();
    warm_up_present_ = presented - begin; warm_up_ = InputTime() - begin;
    return true;
}
bool D3D12Renderer::CreateShared() noexcept {
    // The device, the direct queue and fence, the swap chain on the window, the composition lists and the passes that
    // do not depend on the video: once per session (GpuSession), and again after a device loss.
    IDXGIFactory4* factory = nullptr;
    error_ = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(error_)) error_ = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    // D3D12 hands back the existing device of an adapter while anything still references it, removed or not.
    if (SUCCEEDED(error_)) error_ = device_->GetDeviceRemovedReason();
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(error_)) error_ = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_));
    if (SUCCEEDED(error_)) error_ = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (texture_output_) {
        // Texture output (API 13): two shared fences (the engine's frames, the host's releases) and three shared textures.
        if (SUCCEEDED(error_) && !output_.CreateFences(device_)) error_ = FAILED(output_.LastError()) ? output_.LastError() : E_FAIL;
        Release(factory);
        if (FAILED(error_)) return false;
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_ || !CreateOutputs(width_, height_)) return false;
    } else {
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = width_; desc.Height = height_; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        IDXGISwapChain1* swap = nullptr;
        if (SUCCEEDED(error_)) error_ = factory->CreateSwapChainForHwnd(queue_, window_, &desc, nullptr, nullptr, &swap);
        if (SUCCEEDED(error_)) error_ = swap->QueryInterface(IID_PPV_ARGS(&swap_));
        if (factory) (void)factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
        Release(swap); Release(factory);
        if (FAILED(error_)) return false;
        error_ = swap_->SetMaximumFrameLatency(2); latency_ = swap_->GetFrameLatencyWaitableObject();
        fresh_swap_ = true; // Prepare composes its first image (WarmUpFlip).
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); // Kept across a device recovery.
        if (FAILED(error_) || !latency_ || !event_) return false;
        for (unsigned i = 0; i != 3; ++i) if (FAILED(error_ = swap_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvs{}; rtvs.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvs.NumDescriptors = 3;
    if (FAILED(error_ = device_->CreateDescriptorHeap(&rtvs, IID_PPV_ARGS(&rtvs_)))) return false;
    rtv_stride_ = device_->GetDescriptorHandleIncrementSize(rtvs.Type);
    for (unsigned i = 0; i < 3; ++i) {
        auto target = rtvs_->GetCPUDescriptorHandleForHeapStart(); target.ptr += static_cast<SIZE_T>(i) * rtv_stride_;
        device_->CreateRenderTargetView(buffers_[i], nullptr, target);
        auto& c = composites_[i];
        error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator));
        if (SUCCEEDED(error_)) error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator, nullptr, IID_PPV_ARGS(&c.commands));
        if (SUCCEEDED(error_)) error_ = c.commands->Close();
        if (FAILED(error_)) return false;
    }
    if (FAILED(error_ = hud_pipeline_.Create(device_))) return false;
    if (FAILED(error_ = visual_pipeline_.Create(device_))) return false;
    if (FAILED(error_ = color_.Create(device_))) return false;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, sizeof(BlitConstants) / 4};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *errors = nullptr;
        error_ = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (SUCCEEDED(error_)) error_ = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&blit_root_));
        Release(blob); Release(errors);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = blit_root_;
        p.VS = {g_BlitVertexShader, sizeof(g_BlitVertexShader)}; p.PS = {g_BlitPixelShader, sizeof(g_BlitPixelShader)};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1; p.RTVFormats[0] = SharedOutput::Dxgi(output_format_); p.SampleDesc.Count = 1; // The composites' format.
        if (SUCCEEDED(error_)) error_ = device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&blit_));
        if (FAILED(error_)) return false;
    }
    gpu_.width = width_; gpu_.height = height_;
    return true;
}
bool D3D12Renderer::CreateGpu() noexcept {
    // Everything bound to the device, sized by the window (back buffers), the slot capacity and the video; run by
    // Prepare and, after a device loss, by RecreateDevice on the replacement device. A warm session's next file finds the
    // device-level part made.
    if (!device_ && !CreateShared()) return false;
    // The slots' and the swap chain's format (API 14: texture output's own, so a picture is copied into it as converted).
    const DXGI_FORMAT target = SharedOutput::Dxgi(output_format_);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Plane SRVs (2 per decode surface), slot UAVs, then slot SRVs and the history SRV for scaled composition, then the
    // neural 2x luma UAV and SRV, then the held source's plane SRVs and the history UAV (paused re-conversion).
    heap.NumDescriptors = surfaces_ * 2 + Count * 2 + 7; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // + the reference's SRV.
    if (FAILED(error_ = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors_)))) return false;
    stride_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_DEFAULT; props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = capacity_width_; texture.Height = capacity_height_; texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.Format = target; texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // A warm session's slots and history, when the previous file's pictures were fitted to this same size.
    const bool kept = session_ && !music_ && gpu_.history && gpu_.slot_width == capacity_width_ && gpu_.slot_height == capacity_height_;
    if (session_ && !kept && !music_) gpu_.ReleaseSlots();
    if (kept) { history_ = gpu_.history; gpu_.history = nullptr; gpu_.slot_width = gpu_.slot_height = 0; ++kept_slots_; }
    else if (FAILED(error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&history_)))) return false;
    // Micro-motion's per-pixel reference, as large as a slot (kept by the session's colour pipeline while large enough).
    if (!music_ && FAILED(error_ = color_.PrepareMotion(device_, std::uint64_t{capacity_width_} * capacity_height_))) return false;
    micro_start_ = true;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = texture.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + Count * 2 + 5) * stride_;
        device_->CreateUnorderedAccessView(history_, nullptr, &uav, cpu);
        if (FAILED(error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&still_allocator_)))) return false;
        if (FAILED(error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, still_allocator_, color_.Initial(), IID_PPV_ARGS(&still_commands_)))) return false;
        if (FAILED(error_ = still_commands_->Close())) return false;
        if (FAILED(error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&steady_allocator_)))) return false;
        if (FAILED(error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, steady_allocator_, color_.Initial(), IID_PPV_ARGS(&steady_commands_)))) return false;
        if (FAILED(error_ = steady_commands_->Close())) return false;
        // Forensic snapshots: no readback until one is asked for (MakeSnapshotReadback, 2026-11-02).
        snapshot_state_ = SnapshotState::Idle;
        if (capture_enabled_) { // Test hook: a readback of the held image.
            UINT64 bytes = 0; device_->GetCopyableFootprints(&texture, 0, 1, 0, &capture_layout_, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1;
            buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(error_ = device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&capture_readback_)))) return false;
        }
    }
    for (unsigned i = 0; i != Count; ++i) {
        auto& s = slots_[i];
        if (kept) { auto& k = gpu_.slots[i]; s.rgb = k.rgb; s.allocator = k.allocator; s.commands = k.commands; k = {}; }
        else {
            error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.rgb));
            if (SUCCEEDED(error_)) error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator));
            if (SUCCEEDED(error_)) error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, color_.Initial(), IID_PPV_ARGS(&s.commands));
            if (SUCCEEDED(error_)) error_ = s.commands->Close();
        }
        if (FAILED(error_) || !s.rgb || !s.commands) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = texture.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + i) * stride_;
        device_->CreateUnorderedAccessView(s.rgb, nullptr, &uav, cpu);
    }
    // The reference frame (Pinned | Live) and the pinned loop pictures, at the slots' size; one list copies them.
    {
        D3D12_RESOURCE_DESC copyable = texture; copyable.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &copyable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reference_)))) return false;
        for (auto*& pin : pin_rgb_)
            if (FAILED(error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &copyable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pin)))) return false;
        if (FAILED(error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pin_allocator_)))) return false;
        if (FAILED(error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pin_allocator_, nullptr, IID_PPV_ARGS(&pin_commands_)))) return false;
        if (FAILED(error_ = pin_commands_->Close())) return false;
        D3D12_COMMAND_QUEUE_DESC copy{}; copy.Type = D3D12_COMMAND_LIST_TYPE_COPY; copy.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        if (FAILED(error_ = device_->CreateCommandQueue(&copy, IID_PPV_ARGS(&copy_queue_)))) return false;
        if (FAILED(error_ = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_)))) return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (FAILED(error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&turn_allocators_[i])))) return false;
            if (FAILED(error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, turn_allocators_[i], nullptr, IID_PPV_ARGS(&turn_commands_[i])))) return false;
            if (FAILED(error_ = turn_commands_[i]->Close())) return false;
        }
        copy_sequence_ = 0;
        pin_fence_ = 0; pin_count_ = 0; pin_complete_ = false; reference_valid_ = false;
    }
    // Neural 2x luma: only when the video can be shown larger than its source somewhere on this desktop.
    if (neural_allowed_ && video_width_ && video_height_ && (capacity_width_ > video_width_ || capacity_height_ > video_height_)) {
        neural_width_ = 2 * video_width_; neural_height_ = 2 * video_height_;
        D3D12_RESOURCE_DESC luma = texture; luma.Width = neural_width_; luma.Height = neural_height_; luma.Format = DXGI_FORMAT_R16_FLOAT;
        if (FAILED(error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &luma, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&neural_texture_)))) return false;
        if (FAILED(error_ = neural_pass_.Create(device_, queue_, kNeuralWeights, video_width_, video_height_))) return false;
        neural_pass_.SetReuse(neural_reuse_);
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + Count * 2 + 1) * stride_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = luma.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(neural_texture_, nullptr, &uav, cpu);
        cpu.ptr += stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = luma.Format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(neural_texture_, &view, cpu);
    }
    // GPU time of every conversion (QualityGovernor): two timestamps per slot, resolved into a mapped readback buffer.
    // Optional: without them the tier simply stays where the mode puts it.
    UINT64 ticks = 0;
    const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * Count, 0};
    if (SUCCEEDED(queue_->GetTimestampFrequency(&ticks)) && ticks && SUCCEEDED(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_)))) {
        D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 16 * Count; buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr; const D3D12_RANGE everything{0, 16 * Count};
        if (SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&timestamp_readback_))) && SUCCEEDED(timestamp_readback_->Map(0, &everything, &mapped))) {
            timestamp_data_ = static_cast<const std::uint64_t*>(mapped); timestamp_frequency_ = ticks;
        } else { Release(timestamp_readback_); Release(timestamps_); }
    }
    // Scene statistics of every conversion (live cuts), copied from the peak state per slot, then the horizon's histograms per
    // slot (auto-leveling; kStatsBytes x Count on). Optional like the timestamps.
    {
        constexpr UINT64 kBytes = (ColorPipeline::kStatsBytes + ColorPipeline::kHorizonBytes) * Count;
        D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kBytes; buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr; const D3D12_RANGE everything{0, kBytes};
        if (SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&stats_readback_))) && SUCCEEDED(stats_readback_->Map(0, &everything, &mapped)))
            stats_data_ = static_cast<const std::uint8_t*>(mapped);
        else Release(stats_readback_);
        stats_cuts_ = stats_sum_ = stats_count_ = 0; cut_detector_.Reset(); // The new peak state starts from zero.
    }
    // Target tracking's grid: a UAV and a readback region per slot, made here (with the file) so that tracking allocates nothing.
    {
        D3D12_HEAP_PROPERTIES gpu{}; gpu.Type = D3D12_HEAP_TYPE_DEFAULT; gpu.CreationNodeMask = gpu.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kGridBytes; buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rows = buffer; rows.Width = std::uint64_t{kGridBytes} * Count; rows.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* mapped = nullptr; const D3D12_RANGE everything{0, static_cast<SIZE_T>(rows.Width)};
        if (SUCCEEDED(device_->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&grid_))) &&
            SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rows, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&grid_readback_))) &&
            SUCCEEDED(grid_readback_->Map(0, &everything, &mapped)))
            grid_data_ = static_cast<const std::uint8_t*>(mapped);
        else { Release(grid_); Release(grid_readback_); }
        tracker_.Stop(); track_pending_ = false; grid_mean_ = -1;
        // Vibrometry's samples (2026-11-03): the same, kVibroBytes.
        buffer.Width = kVibroBytes; rows.Width = std::uint64_t{kVibroBytes} * Count; mapped = nullptr;
        const D3D12_RANGE all{0, static_cast<SIZE_T>(rows.Width)};
        if (SUCCEEDED(device_->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&vibro_buffer_))) &&
            SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rows, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vibro_readback_))) &&
            SUCCEEDED(vibro_readback_->Map(0, &all, &mapped)))
            vibro_data_ = static_cast<const std::uint8_t*>(mapped);
        else { Release(vibro_buffer_); Release(vibro_readback_); }
        vibro_on_ = false; vibro_.Reset(); vibro_path_.Reset();
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC picture{}; picture.Format = texture.Format; picture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    picture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; picture.Texture2D.MipLevels = 1;
    for (unsigned i = 0; i != Count + 1; ++i) {
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + Count + i) * stride_;
        device_->CreateShaderResourceView(i < Count ? slots_[i].rgb : history_, &picture, cpu);
    }
    {
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + Count * 2 + 6) * stride_;
        device_->CreateShaderResourceView(reference_, &picture, cpu);
    }
    return true;
}
bool D3D12Renderer::Bind(const platform::windows::D3D12VideoDecoder& decoder) noexcept {
    if (!device_ || bound_) return false;
    for (unsigned i = 0; i < surfaces_; ++i) {
        auto* resource = decoder.SurfaceAt(i); if (!resource) return false;
        const auto format = resource->GetDesc().Format;
        if ((format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010) || (i && (format == DXGI_FORMAT_P010) != ten_bit_)) return false;
        ten_bit_ = format == DXGI_FORMAT_P010;
        for (unsigned plane = 0; plane != 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ten_bit_ ? (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) : (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(i * 2 + plane) * stride_;
            device_->CreateShaderResourceView(resource, &srv, cpu);
        }
    }
    // The held source for paused re-conversion: one more surface of the same description, and its plane views.
    if (!held_) {
        const auto desc = decoder.SurfaceAt(0)->GetDesc();
        D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_DEFAULT; props.CreationNodeMask = props.VisibleNodeMask = 1;
        if (FAILED(error_ = device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&held_))))
            return false;
    }
    for (unsigned plane = 0; plane != 2; ++plane) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = ten_bit_ ? (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) : (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(surfaces_ * 2 + Count * 2 + 3 + plane) * stride_;
        device_->CreateShaderResourceView(held_, &srv, cpu);
    }
    held_valid_ = false;
    bound_ = true; return true;
}
namespace {
// What the desktop's input devices deliver through a thread's queue: keys and characters, mouse (client and frame),
// wheel, hover and leave, raw input, touch, pointer (pen, touch) and gestures, and the input method's messages.
bool InputMessage(UINT m) noexcept {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ||
           (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) || m == WM_NCMOUSEHOVER || m == WM_NCMOUSELEAVE ||
           m == WM_MOUSEHOVER || m == WM_MOUSELEAVE || m == WM_INPUT || m == WM_GESTURE || (m >= 0x0240 && m <= 0x0257) || // WM_TOUCH, WM_POINTER*.
           (m >= WM_IME_STARTCOMPOSITION && m <= WM_IME_KEYLAST) || m == WM_IME_CHAR || m == WM_IME_KEYDOWN || m == WM_IME_KEYUP;
}
}
bool D3D12Renderer::PostTestInput(UINT message, WPARAM w, LPARAM l) noexcept {
    if (!window_) return false;
    for (auto& t : tagged_) {
        if (t.armed.load(std::memory_order_acquire)) continue;
        t.message = message; t.w = w; t.l = l; t.armed.store(true, std::memory_order_release);
        if (PostMessageW(window_, message, w, l)) return true;
        t.armed.store(false, std::memory_order_release); return false;
    }
    return false; // 16 tagged posts already waiting.
}
int D3D12Renderer::Admit(const MSG& m) noexcept {
    if (!isolate_input_.load(std::memory_order_relaxed) || !InputMessage(m.message)) return 2;
    // Admitted only when the harness posted exactly this message (PostTestInput); the tag is used up.
    for (auto& t : tagged_)
        if (t.armed.load(std::memory_order_acquire) && t.message == m.message && t.w == m.wParam && t.l == m.lParam) { t.armed.store(false, std::memory_order_release); return 1; }
    isolated_inputs_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}
bool D3D12Renderer::Messages() noexcept {
    if (message_thread_) {
        // The window thread's input, in order, handled as its procedure would have (Process), with its modifiers and times.
        for (unsigned i = 0; i != 64; ++i) {
            const auto* e = input_.PeekReadSlot();
            if (!e) break;
            const auto now = InputTime(), latency = now - e->time;
            if (e->time >= input_since_) { ++input_stats_.handled; input_stats_.latency_sum += latency; input_stats_.latency_max = (std::max)(input_stats_.latency_max, latency); }
            keys_ = e->keys;
            if (e->message == WM_IME_COMPOSITION) {
                // The strings the window thread read: the committed result first (it replaces the composition), then what is
                // still being composed.
                if (palette_.IsOpen()) {
                    if (e->lengths[0]) (void)palette_.Insert(e->text[0], e->lengths[0], e->time);
                    if (e->l & GCS_COMPSTR) palette_.Compose(e->text[1], e->lengths[1], e->time);
                    else if ((e->l & GCS_RESULTSTR) || !e->l) palette_.Compose<wchar_t>(nullptr, 0, e->time);
                    PlaceIme();
                }
            } else (void)Process(window_, e->message, e->w, e->l, e->time);
            (void)input_.ReleaseReadSlot();
        }
        // This thread's own queue has no window: anything posted to the thread itself.
        MSG message{};
        for (unsigned i = 0; i != 8 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
            if (message.message == WM_QUIT) closed_ = true;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return !closed_;
    }
    MSG message{};
    for (unsigned i = 0; i != 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (message.message == WM_QUIT) closed_ = true;
        const int admit = Admit(message);
        if (!admit) continue;
        if (admit == 1) { DispatchMessageW(&message); continue; } // Not translated: a test posts the characters it means (WM_CHAR) itself.
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return !closed_;
}
bool D3D12Renderer::Lost() const noexcept {
    return device_ && (FAILED(device_->GetDeviceRemovedReason()) || error_ == DXGI_ERROR_DEVICE_REMOVED || error_ == DXGI_ERROR_DEVICE_RESET ||
                       error_ == DXGI_ERROR_DEVICE_HUNG);
}
RenderStatus D3D12Renderer::Convert(const core::VideoFrameDescriptor& frame, const platform::windows::D3D12VideoDecoder& decoder,
                                   platform::windows::VideoSurfaceReturn& returned) noexcept {
    const auto status = ConvertFrame(frame, decoder, returned);
    return status == RenderStatus::Failed && Lost() ? RenderStatus::DeviceLost : status;
}
D3D12Renderer::ConvertBlock D3D12Renderer::ConvertWait(const core::VideoFrameDescriptor& frame, const platform::windows::D3D12VideoDecoder& decoder) const noexcept {
    // ConvertFrame's tests in the order that matters for waiting: with no slot to put it in, the picture waits for a flip,
    // however far its decode is.
    if (!bound_ || frame.playback_epoch != epoch_) return ConvertBlock::Now; // Retired at once, or an adoption to decide.
    const auto completed = fence_->GetCompletedValue(), decoded = decoder.DecodeFence()->GetCompletedValue();
    if (completed == UINT64_MAX || decoded == UINT64_MAX) return ConvertBlock::Now;
    bool free = false, retiring = false;
    for (const auto& s : slots_) { if (!s.busy && completed >= s.fence) free = true; else if (!s.busy) retiring = true; }
    if ((!free && !retiring) || order_.Size() == Count) return ConvertBlock::Presentation;
    if (decoded < frame.ready_fence_value) return ConvertBlock::Decode;
    for (const auto& s : slots_) if (s.busy && completed < s.fence) return ConvertBlock::Conversion;
    if ((steady_stage_ == 1 && completed < steady_fence_) || (steady_stage_ == 2 && steady_job_.load(std::memory_order_acquire) != 2)) return ConvertBlock::Steadying;
    return free ? ConvertBlock::Now : ConvertBlock::Conversion; // A free slot whose last flip the GPU has not finished.
}
bool D3D12Renderer::ArmConvertWake(ConvertBlock block, const core::VideoFrameDescriptor& frame, const platform::windows::D3D12VideoDecoder& decoder, HANDLE event) noexcept {
    if (block == ConvertBlock::Decode) return SUCCEEDED(decoder.DecodeFence()->SetEventOnCompletion(frame.ready_fence_value, event));
    if (block == ConvertBlock::Steadying) {
        if (steady_stage_ == 1) return SUCCEEDED(fence_->SetEventOnCompletion(steady_fence_, event));
        steady_wake_.store(event, std::memory_order_release); // Perception's thread sets it when the measurement is done...
        if (steady_job_.load(std::memory_order_acquire) == 2) (void)SetEvent(event); // ...unless it already is.
        return true;
    }
    if (block != ConvertBlock::Conversion) return false;
    // The earliest fence the slots wait on: the conversion in flight, or a free slot's last flip.
    const auto completed = fence_->GetCompletedValue(); std::uint64_t next = UINT64_MAX;
    for (const auto& s : slots_) if (completed < s.fence) next = (std::min)(next, s.fence);
    return next != UINT64_MAX && SUCCEEDED(fence_->SetEventOnCompletion(next, event));
}
RenderStatus D3D12Renderer::ConvertFrame(const core::VideoFrameDescriptor& frame, const platform::windows::D3D12VideoDecoder& decoder,
                                        platform::windows::VideoSurfaceReturn& returned) noexcept {
    const auto expected = ten_bit_ ? core::VideoSurfaceFormat::P010 : core::VideoSurfaceFormat::Nv12;
    if (!bound_ || frame.surface_id >= surfaces_ || frame.format != expected || frame.playback_epoch > epoch_) return RenderStatus::Failed;
    bool adopted = false;
    if (frame.playback_epoch < epoch_) {
        // A queue seek the worker has not decided yet: the picture may be adopted, so it waits.
        if (adoption_ == Adoption::Pending) return RenderStatus::WouldBlock;
        adopted = adoption_ == Adoption::Confirmed && frame.playback_epoch >= adopt_chain_ &&
                  frame.pts_100ns + frame.duration_100ns > adopt_floor_;
        if (!adopted) { // Decoded before a seek: retired without GPU access.
            ++stale_; returned = {frame.surface_id, frame.surface_generation, frame.playback_epoch, 2, 0};
            return RenderStatus::Ready;
        }
    }
    if (trace_converted_ < kTrace && frame.pts_100ns != trace_seen_pts_) { trace_seen_pts_ = frame.pts_100ns; trace_seen_ = InputTime(); trace_decoded_ = 0; }
    // A picture the decoder has not finished is not converted yet. Its conversion would sit in this queue behind a
    // GPU wait for the decoder, and every flip queued after it would wait too: a decode that takes longer than a frame
    // interval would then make a picture late that was converted long before.
    const auto decoded = decoder.DecodeFence()->GetCompletedValue();
    if (decoded == UINT64_MAX) return RenderStatus::Failed;
    if (decoded < frame.ready_fence_value) return RenderStatus::WouldBlock;
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX) return RenderStatus::Failed;
    const auto now = InputTime();
    if (trace_converted_ < kTrace && !trace_decoded_) trace_decoded_ = now;
    HarvestTimings(now, completed); // Before any slot whose conversion has completed can be reused.
    // One conversion in this queue at a time, for the same reason: filling sixteen slots at start-up or after a seek
    // must not put sixteen conversions in front of the next flip. The slots fill as fast as the GPU converts anyway.
    for (const auto& other : slots_) if (other.busy && completed < other.fence) return RenderStatus::WouldBlock;
    unsigned index = Count;
    const auto copied = copy_fence_ ? copy_fence_->GetCompletedValue() : UINT64_MAX;
    const auto free_slot = [&](unsigned i) { return !slots_[i].busy && completed >= slots_[i].fence && (!slots_[i].copy_fence || copied >= slots_[i].copy_fence); };
    if (steady_stage_ && free_slot(steady_slot_)) index = steady_slot_; // The grid being measured is in this slot's readback.
    for (unsigned i = 0; i != Count && index == Count; ++i) if (free_slot(i)) index = i;
    if (index == Count) return RenderStatus::WouldBlock; // Every RGB slot waits for presentation or GPU retirement (or a copy).
    SteadyView steady{};
    if (steady_on_) { const auto measured = Measure(frame, index, decoder, completed, steady); if (measured != RenderStatus::Ready) return measured; }
    const SteadyResult motion = steady.on ? steady_result_ : SteadyResult{};
    auto& s = slots_[index];
    // Admit into presentation order first: a frame that can no longer be shown in order is retired
    // without any GPU access (consumer fence 0), never presented out of order.
    const auto admitted = order_.Insert(frame.pts_100ns, index);
    if (admitted != video::ReorderInsert::Inserted) {
        if (admitted == video::ReorderInsert::Full) return RenderStatus::WouldBlock;
        ++out_of_order_;
        last_reject_ = {frame.pts_100ns, order_.LastReleased(), frame.playback_epoch, epoch_, static_cast<std::uint8_t>(admitted), false};
        returned = {frame.surface_id, frame.surface_generation, frame.playback_epoch, 2, 0};
        return RenderStatus::Ready;
    }
    error_ = s.allocator->Reset(); if (SUCCEEDED(error_)) error_ = s.commands->Reset(s.allocator, color_.Initial());
    if (FAILED(error_)) return RenderStatus::Failed;
    if (timestamps_) s.commands->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * index);
    auto* source = decoder.SurfaceAt(frame.surface_id);
    D3D12_RESOURCE_BARRIER barriers[]{Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(s.rgb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    s.commands->ResourceBarrier(2, barriers);
    ID3D12DescriptorHeap* heaps[]{descriptors_}; s.commands->SetDescriptorHeaps(1, heaps);
    auto planes = descriptors_->GetGPUDescriptorHandleForHeapStart(); planes.ptr += static_cast<UINT64>(frame.surface_id * 2) * stride_;
    auto target = descriptors_->GetGPUDescriptorHandleForHeapStart(); target.ptr += static_cast<UINT64>(surfaces_ * 2 + index) * stride_;
    const auto source_desc = source->GetDesc();
    // A seek's landing converted while paused: its source is kept, so that a picture setting changed while paused can
    // convert it again at once (SettingsChanged). One copy, only then.
    const bool landing = hud_paused_ && held_ && preview_ && (frame.playback_epoch == epoch_ || adopted) && seek_target_ != INT64_MIN &&
                         frame.pts_100ns <= seek_target_ && seek_target_ < frame.pts_100ns + (frame.duration_100ns > 0 ? frame.duration_100ns : 1);
    if (landing) {
        D3D12_RESOURCE_BARRIER copy[]{Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                      Barrier(held_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        s.commands->ResourceBarrier(2, copy); s.commands->CopyResource(held_, source);
        for (auto& b : copy) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        s.commands->ResourceBarrier(2, copy);
        held_frame_ = frame; held_frame_.playback_epoch = epoch_; held_valid_ = true; held_steady_ = steady;
    }
    const auto setup = Setup(frame, static_cast<unsigned>(source_desc.Width), source_desc.Height, now, true, steady);
    const auto& constants = setup.constants;
    const unsigned picture_width = setup.width, picture_height = setup.height;
    const unsigned crop_width = frame.crop_right - frame.crop_left, crop_height = frame.crop_bottom - frame.crop_top;
    const auto tier = setup.tier;
    const bool neural = setup.neural, fast = tier == QualityTier::Fast;
    auto neural_view = descriptors_->GetGPUDescriptorHandleForHeapStart(); neural_view.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 1) * stride_;
    if (neural) {
        auto b = Barrier(neural_texture_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); s.commands->ResourceBarrier(1, &b);
        const auto r = RegionOf(frame);
        neural_pass_.Record(s.commands, planes, neural_view, r.left, r.top, r.right, r.bottom, ten_bit_);
        b = Barrier(neural_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); s.commands->ResourceBarrier(1, &b);
        neural_view.ptr += stride_; // SRV follows the UAV.
    }
    color_.Record(s.commands, planes, target, constants, neural ? neural_view : planes);
    s.stats = stats_data_ != nullptr; s.stats_epoch = epoch_; s.measured = s.cut = false; s.luma = s.peak = s.motion = -1; s.cut_pts = -1;
    s.horizon = s.stats && (constants.filter_flags & kHorizon) != 0;
    if (s.stats) color_.RecordStats(s.commands, constants, stats_readback_, std::uint64_t{ColorPipeline::kStatsBytes} * index, stats_readback_,
                                    std::uint64_t{ColorPipeline::kStatsBytes} * Count + std::uint64_t{ColorPipeline::kHorizonBytes} * index);
    s.grid = false;
    // While not tracking, a grid every kWarmInterval of media for perception (the newest picture that far from the last, in
    // conversion order; a new epoch starts over), so that T finds the objects already known. Only with room on the GPU: the last
    // conversion measured took at most half the picture's interval and the governor is not at its Fast tier (measured: a 4K60
    // HDR file at the Fast tier, 12 ms of 16.7 a conversion, dropped 3 to 10 pictures with the grid pass every sixth picture).
    const bool room = last_gpu_ > 0 && 2 * last_gpu_ <= frame.duration_100ns && governor_.Tier() != QualityTier::Fast;
    // A new epoch (a seek's landing, the start) begins the cadence: its first grid kWarmInterval later, so the landing picture's
    // conversion carries no grid pass (measured: one picture dropped after each of a film's seeks at 60/s, "waited for its
    // conversion").
    if (frame.playback_epoch != warm_epoch_ || frame.pts_100ns < warm_pts_) { warm_epoch_ = frame.playback_epoch; warm_pts_ = frame.pts_100ns; }
    bool warm = false;
    if (warm_perception_ && grid_data_ && targets_ready_ && !Tracking() && room && frame.pts_100ns - warm_pts_ >= kWarmInterval) { warm = true; warm_pts_ = frame.pts_100ns; }
    ApplyEventRequest();
    if (grid_data_ && (Tracking() || warm || events_on_ || steady.on)) {
        // The whole crop's grid (not the zoomed region): the tracker sees the target move in the picture.
        ColorConstants whole = constants;
        whole.left = frame.crop_left; whole.top = frame.crop_top; whole.right = frame.crop_right; whole.bottom = frame.crop_bottom;
        const unsigned w = frame.crop_right - frame.crop_left, h = frame.crop_bottom - frame.crop_top;
        s.grid_scale = video::GridScale(w, h); s.grid_width = (w + s.grid_scale - 1) / s.grid_scale; s.grid_height = (h + s.grid_scale - 1) / s.grid_scale;
        s.grid_words = (s.grid_width + 3) / 4;
        if (std::uint64_t{s.grid_words} * 4 * s.grid_height <= kGridBytes) {
            // Stabilising: its grid is in this slot's readback already, measured before this conversion.
            if (!steady.on) color_.RecordGrid(s.commands, whole, s.grid_scale, s.grid_words, s.grid_height, grid_, grid_readback_, std::uint64_t{kGridBytes} * index);
            s.grid = !steady.on || Tracking() || warm || events_on_;
        }
    }
    s.steadied = steady.on; s.motion_x = motion.valid ? motion.dx : 0; s.motion_y = motion.valid ? motion.dy : 0; s.motion_turn = motion.valid ? motion.turn : 0;
    steady_stage_ = 0;
    if (steady.on) ++steadied_;
    // Remote vibrometry's samples (2026-11-03): the region of this picture, at full resolution, Y', Cb and Cr.
    s.vibro = false;
    video::AreaRect vibro_region{}; bool vibro_follow = false;
    if (vibro_data_ && VibroRegion(frame, vibro_region, vibro_follow)) {
        color_.RecordVibro(s.commands, constants, static_cast<float>(vibro_region.left + frame.crop_left), static_cast<float>(vibro_region.top + frame.crop_top),
                           static_cast<float>(vibro_region.right + frame.crop_left), static_cast<float>(vibro_region.bottom + frame.crop_top), video::kVibrometryCells,
                           vibro_buffer_, vibro_readback_, std::uint64_t{kVibroBytes} * index);
        s.vibro = true; s.vibro_region = vibro_region; s.vibro_follow = vibro_follow;
    }
    // Tensor export (2026-11-03): a host's request, with this conversion.
    bool tensor = false;
    std::uint32_t asked = 1;
    // While paused, only the picture on screen: pictures converted ahead of it are not what the host sees.
    if (tensor_data_ && (!hud_paused_ || landing) && tensor_state_.load(std::memory_order_acquire) == 1 && tensor_state_.compare_exchange_strong(asked, 4, std::memory_order_acq_rel)) {
        video::AreaRect r{};
        if (!TensorRegion(frame, steady, r)) tensor_state_.store(5, std::memory_order_release);
        else {
            color_.RecordTensor(s.commands, constants, static_cast<float>(r.left + frame.crop_left), static_cast<float>(r.top + frame.crop_top),
                                static_cast<float>(r.right + frame.crop_left), static_cast<float>(r.bottom + frame.crop_top), tensor_ask_.width, tensor_ask_.height,
                                tensor_buffer_, tensor_readback_, true);
            Answer(frame, r, constants.transfer, false);
            tensor = true;
        }
    }
    // Test load (SetTestLoad): the same conversion again, in order (it writes the same pixels), calibrated to the time
    // asked for. HDR conversions are left alone: each one also folds the tone mapper's peak detection.
    unsigned repeats = 0;
    if (test_load_ > 0 && !fast && !IsHdrTransfer(constants.transfer) && !(constants.picture_flags & kMicro)) {
        repeats = load_repeats_;
        D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = s.rgb;
        for (unsigned r = 0; r < repeats; ++r) { s.commands->ResourceBarrier(1, &uav); color_.Record(s.commands, planes, target, constants, neural ? neural_view : planes); }
    }
    if (neural) {
        auto b = Barrier(neural_texture_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); s.commands->ResourceBarrier(1, &b);
        ++neural_frames_;
    }
    if (IsHdrTransfer(constants.transfer)) ++tone_mapped_;
    if (compare_) ++compare_frames_;
    barriers[0] = Barrier(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barriers[1] = Barrier(s.rgb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    s.commands->ResourceBarrier(2, barriers);
    if (timestamps_) {
        s.commands->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * index + 1);
        s.commands->ResolveQueryData(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * index, 2, timestamp_readback_, 16ull * index);
    }
    error_ = s.commands->Close(); if (SUCCEEDED(error_)) error_ = queue_->Wait(decoder.DecodeFence(), frame.ready_fence_value);
    if (FAILED(error_)) { neural_pass_.Invalidate(); return RenderStatus::Failed; } // Its history was recorded, never run.
    ID3D12CommandList* lists[]{s.commands}; queue_->ExecuteCommandLists(1, lists);
    s.fence = ++sequence_; error_ = queue_->Signal(fence_, s.fence); if (FAILED(error_)) return RenderStatus::Failed;
    if (tensor) { tensor_fence_.store(s.fence, std::memory_order_relaxed); tensor_state_.store(2, std::memory_order_release); }
    s.frame = frame; s.width = picture_width; s.height = picture_height; s.staged = false; s.busy = true; s.copy_fence = 0; ++converted_;
    s.crop = {frame.crop_left, frame.crop_top, frame.crop_right, frame.crop_bottom}; s.region = setup.region; s.zoom_generation = zoom_generation_;
    s.view = s.region; s.tracked = tracker_.Active();
    s.frame.playback_epoch = epoch_; // An adopted picture joins the current epoch (its surface returns under its own).
    if (adopted) ++adopted_frames_;
    s.timed = timestamps_ != nullptr; s.tier = tier; s.repeats = repeats;
    s.workload = (std::uint64_t{picture_width} * picture_height) << 32 | (std::uint64_t{crop_width} * crop_height);
    s.budget = frame.duration_100ns > 0 ? frame.duration_100ns : 416667;
    ++tier_frames_[static_cast<unsigned>(tier)];
    s.trace = -1;
    if (trace_converted_ < kTrace) {
        unsigned busy = 0; for (const auto& other : slots_) busy += other.busy && &other != &s;
        trace_conversions_[trace_converted_] = {frame.pts_100ns, trace_seen_, trace_decoded_, now, -1, 0, static_cast<std::uint8_t>(tier), static_cast<std::uint8_t>(busy)};
        s.trace = static_cast<std::int16_t>(trace_converted_++); trace_seen_pts_ = INT64_MIN;
    }
    returned = {frame.surface_id, frame.surface_generation, frame.playback_epoch, 2, s.fence};
    return RenderStatus::Ready;
}
D3D12Renderer::Conversion D3D12Renderer::Setup(const core::VideoFrameDescriptor& frame, unsigned source_width, unsigned source_height, std::int64_t now,
                                                bool fold_peak, const SteadyView& steady) noexcept {
    Conversion c;
    // The picture keeps the stream's aspect inside the current window (letterboxed at composition), within the slot.
    // The aspect is that of the crop in square pixels: the crop's samples times the stream's sample aspect ratio. The
    // conversion scales each axis from the crop to the picture on its own, so anamorphic samples are resampled once.
    c.width = width_; c.height = height_;
    auto shape = video::DisplaySize(frame.crop_right - frame.crop_left, frame.crop_bottom - frame.crop_top, sample_aspect_);
    const bool across = orientation_.Across(); // A quarter turn: the picture's width is shown upright.
    if (across) std::swap(shape.width, shape.height);
    Fit(shape.width, shape.height, c.width, c.height);
    // A zoom converts its region of the crop to the same picture area (the region has the crop's shape).
    const auto region = RegionOf(frame); last_region_ = region; c.region = region;
    const bool turned = !orientation_.Upright() || steady.on; // Anything but as stored.
    // color_metadata_id: matrix bits 0-7, full range bit 8, transfer bits 16-23, primaries bits 24-31 (0 = unspecified).
    // PQ/HLG streams take the tone-mapping pass with a GPU-detected source peak; HDR reference white is SDR white.
    // Neural luma: while upscaling (as the conversion decides), when the crop fits the prepared 2x texture.
    const unsigned crop_width = region.right - region.left, crop_height = region.bottom - region.top;
    const unsigned shown_width = across ? crop_height : crop_width, shown_height = across ? crop_width : crop_height; // In picture samples.
    const bool upscaling = c.width >= shown_width && c.height >= shown_height && (c.width > shown_width || c.height > shown_height);
    // The neural 2x image is of the upright region: a turned picture is reconstructed edge-directed.
    const bool neural_possible = neural_texture_ && upscaling && !turned && 2 * crop_width <= neural_width_ && 2 * crop_height <= neural_height_;
    // The governor's tier (or the mode's): Neural that cannot run here converts as Edge-directed; Fast drops the
    // edge-directed kernel, guided chroma and sharpening.
    governor_.SetNeuralAvailable(neural_possible, now);
    const auto requested = governor_.Tier();
    c.tier = requested == QualityTier::Neural && !neural_possible ? QualityTier::EdgeDirected : requested;
    c.neural = c.tier == QualityTier::Neural;
    const bool fast = c.tier == QualityTier::Fast;
    std::uint32_t peak_flags = 0;
    const float peak_dt = fold_peak && IsHdrTransfer((frame.color_metadata_id >> 16) & 255u) ? peak_timing_.Step(frame.pts_100ns, peak_flags) : 0.0f;
    c.constants = {c.width, c.height, source_width, source_height,
        region.left, region.top, region.right, region.bottom, frame.color_metadata_id & 255u,
        (frame.color_metadata_id >> 8) & 1u, ten_bit_ ? 1u : 0u, frame.color_metadata_id >> 24,
        (frame.color_metadata_id >> 16) & 255u, 0u, 1000.0f, 203.0f, c.neural ? kNeural : fast ? kBilinear : reconstruction_, fast ? 0.0f : sharpness_, 0.02f,
        guided_chroma_ && !fast ? kGuidedChroma : kBilinearChroma, peak_dt, peak_flags,
        (fast ? std::uint32_t{kDither} : std::uint32_t{kAreaPrefilter | kDither}) | (compare_ ? std::uint32_t{kCompare} : 0u)}; // Fast also drops the downscale prefilter.
    if (auto_level_ && fold_peak) c.constants.filter_flags |= kHorizon; // The statistics pass's histograms (a re-conversion has none).
    // API 14: an HDR texture format: the picture encoded as PQ BT.2020 (scRGB written linear), not tone-mapped, no 8-bit dither.
    if (HdrOutput()) c.constants.filter_flags = (c.constants.filter_flags & ~std::uint32_t{kDither}) | kHdrOutput |
                                                 (output_format_ == SharedOutput::Format::Scrgb16 ? std::uint32_t{kLinearOutput} : 0u);
    // The picture's extents for the black bars: every picture until they are known, then every fourth (they hold still).
    if (fold_peak && (!matte_.Locked() || matte_pictures_++ % 4 == 0)) c.constants.filter_flags |= kMatte;
    if (turned) {
        // The conversion's own mapping of the turned region (video::Orientation::Map): no pass of its own.
        double m[4], t[2]; orientation_.Map(c.width, c.height, crop_width, crop_height, m, t);
        if (steady.on) {
            // The view, about its own centre: q = v + R(turn) (M X + t - r), v the steadied centre and r the region's, from the
            // crop's origin (the whole crop is what reads clamp to: the view reaches past the region it has the size of).
            const double rx = crop_width / 2.0, ry = crop_height / 2.0, co = std::cos(static_cast<double>(steady.turn)), si = std::sin(static_cast<double>(steady.turn));
            const double vx = static_cast<double>(steady.x) * (frame.crop_right - frame.crop_left), vy = static_cast<double>(steady.y) * (frame.crop_bottom - frame.crop_top);
            const double a[4]{m[0], m[1], m[2], m[3]}, ux = t[0] - rx, uy = t[1] - ry;
            m[0] = co * a[0] - si * a[2]; m[1] = co * a[1] - si * a[3]; m[2] = si * a[0] + co * a[2]; m[3] = si * a[1] + co * a[3];
            t[0] = vx + co * ux - si * uy; t[1] = vy + si * ux + co * uy;
            c.constants.left = frame.crop_left; c.constants.top = frame.crop_top; c.constants.right = frame.crop_right; c.constants.bottom = frame.crop_bottom;
        }
        for (unsigned i = 0; i < 4; ++i) c.constants.view_m[i] = static_cast<float>(m[i]);
        c.constants.view_t[0] = static_cast<float>(t[0]); c.constants.view_t[1] = static_cast<float>(t[1]);
        c.constants.filter_flags |= kRotate;
    }
    // Picture controls (each neutral one changes no bit) and micro-motion (docs/picture.md, "Picture controls and micro-motion").
    const auto& p = picture_;
    std::uint32_t picture = p.brightness != 0 || p.contrast != 1 || p.saturation != 1 || p.gamma != 1 || p.shadow_lift > 0 ? std::uint32_t{kGrade} : 0u;
    if (p.micro_motion > 1 && color_.MotionPixels() >= std::uint64_t{c.width} * c.height) {
        // The reference is per output pixel: it starts over with a new epoch, zoom region or picture size. A re-conversion
        // (fold_peak false) neither starts nor folds it: it magnifies against the reference as it is.
        const bool start = micro_start_ || micro_epoch_ != epoch_ || micro_width_ != c.width || micro_height_ != c.height || micro_turn_ != turn_generation_ ||
                           micro_region_.left != region.left || micro_region_.top != region.top || micro_region_.right != region.right ||
                           micro_region_.bottom != region.bottom;
        if (!start || fold_peak) {
            // Folded in presentation order: a B-picture converted after its successor is magnified, not folded.
            const bool fold = fold_peak && (start || frame.pts_100ns > micro_pts_);
            picture |= kMicro | (start ? kMicroStart : 0u) | (fold ? kMicroFold : 0u);
            if (fold) micro_pts_ = frame.pts_100ns;
            if (start) { micro_start_ = false; micro_epoch_ = epoch_; micro_width_ = c.width; micro_height_ = c.height; micro_region_ = region; micro_turn_ = turn_generation_; }
            ++micro_pictures_;
        }
    }
    c.constants.picture_flags = picture;
    c.constants.brightness = p.brightness; c.constants.contrast = p.contrast; c.constants.saturation = p.saturation;
    c.constants.gamma = p.gamma; c.constants.shadow_lift = p.shadow_lift; c.constants.micro_gain = p.micro_motion;
    return c;
}
void D3D12Renderer::SetPictureControls(const PictureControls& wanted) noexcept {
    const auto clamp = [](float v, float lo, float hi, float neutral) { return v == v ? (v < lo ? lo : v > hi ? hi : v) : neutral; }; // NaN: neutral.
    PictureControls p{clamp(wanted.brightness, -0.5f, 0.5f, 0), clamp(wanted.contrast, 0, 2, 1), clamp(wanted.saturation, 0, 2, 1),
                      clamp(wanted.gamma, 0.5f, 2.5f, 1), clamp(wanted.shadow_lift, 0, 1, 0), clamp(wanted.micro_motion, 1, 8, 1)};
    if (p.micro_motion > 1 && picture_.micro_motion <= 1) micro_start_ = true;
    picture_ = p;
    SettingsChanged();
}
void D3D12Renderer::SetOrientation(const std::int8_t t[4]) noexcept {
    // Only a quarter turn or mirror of the axes (IsobmffParser keeps anything else as the identity).
    const bool axes = video::Orientation::Axes(t);
    for (unsigned i = 0; i < 4; ++i) orientation_.file[i] = axes ? t[i] : static_cast<std::int8_t>(i == 0 || i == 3);
    orientation_.turns = 0; orientation_.level = 0;
    ++turn_generation_;
    horizon_.Reset(); // A new file: its own horizon (auto-leveling stays as the viewer left it).
    matte_.Reset(0, 0); cropped_changes_ = 0; // And its own bars (sized by its first picture converted whole).
}
void D3D12Renderer::Rotate(int quarter_turns, bool cue) noexcept {
    orientation_.turns = static_cast<unsigned>((static_cast<int>(orientation_.turns) + quarter_turns % 4 + 4) % 4);
    ++turn_generation_; ++rotations_;
    SettingsChanged();
    if (!cue) return;
    static constexpr const char* kCues[]{"Rotation \xc2\xb7 upright", "Rotation \xc2\xb7 90\xc2\xb0", "Rotation \xc2\xb7 180\xc2\xb0", "Rotation \xc2\xb7 270\xc2\xb0"};
    ShowStatus(kCues[QuarterTurns()], ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
void D3D12Renderer::SetAutoLevel(bool on, bool cue) noexcept {
    // Off keeps the turn it had reached (the viewer may go on by hand from there); on starts the horizon's tilts over.
    if (on && !auto_level_) horizon_.Reset();
    auto_level_ = on;
    if (cue) ShowStatus(on ? "Auto level \xc2\xb7 on" : "Auto level \xc2\xb7 off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
void D3D12Renderer::SetAutoCrop(bool on, bool cue) noexcept {
    // Off: the whole picture again (if the view is still the crop's). On: the active area as soon as the bars are known.
    if (!on && auto_crop_ && cropped_changes_) SetZoom(1.0f, 0.5f, 0.5f, false);
    auto_crop_ = on; cropped_changes_ = 0;
    FollowBars();
    if (!cue) return;
    ShowStatus(!on ? "Crop Â· off" : matte_.Bars() ? "Crop Â· black bars" : matte_.Locked() ? "Crop Â· no black bars" : "Crop Â· looking for black bars",
               ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
void D3D12Renderer::FollowBars() noexcept {
    if (!auto_crop_ || !matte_.Locked() || matte_.Changes() == cropped_changes_) return;
    const bool first = !cropped_changes_;
    cropped_changes_ = matte_.Changes();
    if (!matte_.Bars()) { if (!first) SetZoom(1.0f, 0.5f, 0.5f, false); return; }
    // The crop's shape inside the active area (two samples in from each bar: the region's even rounding never shows a line of
    // it), as large as it fits, about the area's centre.
    const auto r = matte_.Active();
    const double w = matte_.Width(), h = matte_.Height();
    const double left = r.left + (r.left ? 2.0 : 0.0), top = r.top + (r.top ? 2.0 : 0.0);
    const double right = r.right - (r.right < w ? 2.0 : 0.0), bottom = r.bottom - (r.bottom < h ? 2.0 : 0.0);
    if (right - left < 16 || bottom - top < 16) return;
    const double zoom = (std::max)(w / (right - left), h / (bottom - top));
    SetZoom(static_cast<float>((std::min)(zoom, static_cast<double>(kMaxZoom))), static_cast<float>((left + right) / 2 / w), static_cast<float>((top + bottom) / 2 / h), false);
}
void D3D12Renderer::FollowHorizon() noexcept {
    if (!auto_level_ || !horizon_.Locked()) return;
    // The stored picture's tilt, clockwise; shown through a mirror it turns the other way. Leveled by the opposite turn, in
    // the tenths of a degree the leveling turn keeps.
    const float shown = orientation_.Mirrored() ? -horizon_.Tilt() : horizon_.Tilt();
    const float level = std::round((std::clamp)(-shown, -kMaxLevel, kMaxLevel) * 10) / 10;
    if (level == orientation_.level) return;
    orientation_.level = level; ++turn_generation_;
    SettingsChanged();
}
void D3D12Renderer::Level(float degrees, bool relative, bool cue) noexcept {
    if (!(degrees == degrees)) return;
    if (auto_level_) SetAutoLevel(false, false); // The viewer's own turn: auto-leveling stops.
    const float level = (std::clamp)(relative ? orientation_.level + degrees : degrees, -kMaxLevel, kMaxLevel);
    const float rounded = std::round(level * 10) / 10; // Tenths of a degree: steps land exactly back on 0.
    if (rounded == orientation_.level) return;
    orientation_.level = rounded; ++turn_generation_; ++rotations_;
    SettingsChanged();
    if (!cue) return;
    // "Level · 2°", "Level · -1.5°", "Level · straight".
    char text[40]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
    put("Level \xc2\xb7 ");
    const float level_now = orientation_.level;
    if (level_now == 0) put("straight");
    else {
        const int tenths = static_cast<int>(std::lround(std::fabs(level_now) * 10));
        if (level_now < 0) put("-");
        char digits[8]; unsigned k = 0; int whole = tenths / 10;
        do { digits[k++] = static_cast<char>('0' + whole % 10); whole /= 10; } while (whole && k < sizeof(digits));
        while (k) { const char one[2]{digits[--k], 0}; put(one); }
        if (tenths % 10) { const char tenth[3]{'.', static_cast<char>('0' + tenths % 10), 0}; put(tenth); }
        put("\xc2\xb0");
    }
    text[n] = 0;
    ShowStatus(text, ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
RenderStatus D3D12Renderer::Reconvert() noexcept {
    const auto completed = fence_->GetCompletedValue(); if (completed == UINT64_MAX) return RenderStatus::Failed;
    if (completed < still_fence_) return RenderStatus::WouldBlock; // The previous re-conversion is still running.
    error_ = still_allocator_->Reset(); if (SUCCEEDED(error_)) error_ = still_commands_->Reset(still_allocator_, color_.Initial());
    if (FAILED(error_)) return RenderStatus::Failed;
    auto* list = still_commands_;
    const auto desc = held_->GetDesc();
    const auto setup = Setup(held_frame_, static_cast<unsigned>(desc.Width), desc.Height, InputTime(), false, steady_on_ ? held_steady_ : SteadyView{});
    D3D12_RESOURCE_BARRIER b[]{Barrier(held_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                               Barrier(history_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, b);
    ID3D12DescriptorHeap* heaps[]{descriptors_}; list->SetDescriptorHeaps(1, heaps);
    const auto base = descriptors_->GetGPUDescriptorHandleForHeapStart();
    auto planes = base; planes.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 3) * stride_;
    auto target = base; target.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 5) * stride_;
    auto neural_view = base; neural_view.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 1) * stride_;
    if (setup.neural) { // Evaluated whole: the 2x texture's tiles belong to the pictures converted ahead.
        auto n = Barrier(neural_texture_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); list->ResourceBarrier(1, &n);
        neural_pass_.Invalidate();
        const auto r = RegionOf(held_frame_);
        neural_pass_.Record(list, planes, neural_view, r.left, r.top, r.right, r.bottom, ten_bit_);
        neural_pass_.Invalidate(); // The next picture's tiles are not this one's either.
        n = Barrier(neural_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &n);
        neural_view.ptr += stride_;
    }
    color_.Record(list, planes, target, setup.constants, setup.neural ? neural_view : planes);
    if (setup.neural) { auto n = Barrier(neural_texture_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); list->ResourceBarrier(1, &n); }
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    list->ResourceBarrier(2, b);
    error_ = list->Close(); if (FAILED(error_)) return RenderStatus::Failed;
    ID3D12CommandList* lists[]{list}; queue_->ExecuteCommandLists(1, lists);
    still_fence_ = ++sequence_; error_ = queue_->Signal(fence_, still_fence_); if (FAILED(error_)) return RenderStatus::Failed;
    history_width_ = setup.width; history_height_ = setup.height; history_valid_ = true;
    history_crop_ = {held_frame_.crop_left, held_frame_.crop_top, held_frame_.crop_right, held_frame_.crop_bottom};
    history_region_ = setup.region; history_generation_ = zoom_generation_;
    pending_update_ = {setup.tier, setup.constants.chroma_mode, (setup.constants.filter_flags & kCompare) != 0, false, 0};
    reconvert_pending_ = false; reconverted_ = true; redraw_ = true;
    return RenderStatus::Ready;
}
RenderStatus D3D12Renderer::HeldTensor() noexcept {
    if (tensor_state_.load(std::memory_order_acquire) != 1 || !tensor_data_ || shown_pts_ == INT64_MIN) return RenderStatus::Ready;
    if (!held_valid_ || held_frame_.playback_epoch != epoch_ || held_frame_.pts_100ns != shown_pts_) {
        // The picture on screen is not held: its landing, which carries the request and holds it from then on.
        if (!preview_ && !refresh_in_flight_) { settings_asked_ = InputTime(); refresh_wanted_ = true; }
        return RenderStatus::Ready;
    }
    const auto completed = fence_->GetCompletedValue(); if (completed == UINT64_MAX) return RenderStatus::Failed;
    if (preview_ || completed < still_fence_) return RenderStatus::Ready; // Its landing, or a re-conversion, is still running.
    std::uint32_t asked = 1;
    if (!tensor_state_.compare_exchange_strong(asked, 4, std::memory_order_acq_rel)) return RenderStatus::Ready;
    const SteadyView view = steady_on_ ? held_steady_ : SteadyView{};
    video::AreaRect r{};
    if (!TensorRegion(held_frame_, view, r)) { tensor_state_.store(5, std::memory_order_release); return RenderStatus::Ready; }
    error_ = still_allocator_->Reset(); if (SUCCEEDED(error_)) error_ = still_commands_->Reset(still_allocator_, color_.Initial());
    if (FAILED(error_)) { tensor_state_.store(5, std::memory_order_release); return RenderStatus::Failed; }
    auto* list = still_commands_;
    const auto desc = held_->GetDesc();
    const auto setup = Setup(held_frame_, static_cast<unsigned>(desc.Width), desc.Height, InputTime(), false, view);
    auto b = Barrier(held_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[]{descriptors_}; list->SetDescriptorHeaps(1, heaps);
    const auto base = descriptors_->GetGPUDescriptorHandleForHeapStart();
    auto planes = base; planes.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 3) * stride_;
    auto target = base; target.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 5) * stride_;
    color_.Bind(list, planes, target, setup.constants);
    color_.RecordTensor(list, setup.constants, static_cast<float>(r.left + held_frame_.crop_left), static_cast<float>(r.top + held_frame_.crop_top),
                        static_cast<float>(r.right + held_frame_.crop_left), static_cast<float>(r.bottom + held_frame_.crop_top), tensor_ask_.width, tensor_ask_.height,
                        tensor_buffer_, tensor_readback_, false);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
    error_ = list->Close();
    if (FAILED(error_)) { tensor_state_.store(5, std::memory_order_release); return RenderStatus::Failed; }
    ID3D12CommandList* lists[]{list}; queue_->ExecuteCommandLists(1, lists);
    still_fence_ = ++sequence_; error_ = queue_->Signal(fence_, still_fence_);
    if (FAILED(error_)) { tensor_state_.store(5, std::memory_order_release); return RenderStatus::Failed; }
    Answer(held_frame_, r, setup.constants.transfer, true);
    tensor_fence_.store(still_fence_, std::memory_order_relaxed); tensor_state_.store(2, std::memory_order_release);
    return RenderStatus::Ready;
}
void D3D12Renderer::Answer(const core::VideoFrameDescriptor& frame, const video::AreaRect& region, std::uint32_t transfer, bool held) noexcept {
    tensor_answer_ = {region, frame.pts_100ns, Tracking() ? target_id_ : 0u, tensor_data_, {transfer, ((frame.color_metadata_id >> 8) & 1u) != 0, ten_bit_, 1000.0f}, held};
    tensor_used_.store(true, std::memory_order_relaxed);
}
RenderStatus D3D12Renderer::CaptureShown() noexcept {
    const auto completed = fence_->GetCompletedValue(); if (completed == UINT64_MAX) return RenderStatus::Failed;
    if (capture_pending_) {
        if (completed < capture_fence_) return RenderStatus::WouldBlock;
        capture_pending_ = false;
        std::uint8_t* data = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(capture_layout_.Footprint.RowPitch) * capture_height_ + capture_layout_.Offset};
        if (FAILED(capture_readback_->Map(0, &range, reinterpret_cast<void**>(&data)))) return RenderStatus::Failed;
        if (capture_spots_ && capture_kind_ == CaptureKind::Screen) {
            // The test's spot: the brighter than 90 % of full scale, every second pixel each way (quick in Debug, sub-pixel for a disc).
            std::uint64_t sum_x = 0, sum_y = 0; unsigned pixels = 0;
            for (unsigned y = 0; y < capture_height_; y += 2) {
                const auto* row = data + capture_layout_.Offset + static_cast<std::size_t>(y) * capture_layout_.Footprint.RowPitch;
                for (unsigned x = 0; x < capture_width_; x += 2) if (row[4 * x + 1] > 230) { sum_x += x; sum_y += y; ++pixels; }
            }
            const bool again = spot_count_ && spots_[spot_count_ - 1].pts == capture_pts_; // A controls-only image of the same picture.
            if (spot_count_ < kMaxSpots && pixels && !again) spots_[spot_count_++] = {capture_pts_, static_cast<float>(sum_x) / static_cast<float>(pixels), static_cast<float>(sum_y) / static_cast<float>(pixels), pixels};
            const D3D12_RANGE nothing{0, 0}; capture_readback_->Unmap(0, &nothing);
            return RenderStatus::Ready;
        }
        auto& c = captures_[capture_count_ < kMaxCaptures ? capture_count_ : kMaxCaptures - 1];
        c = {1469598103934665603ull, 1469598103934665603ull, capture_width_, capture_height_, capture_kind_, {}};
        const unsigned seam = capture_width_ / 2;
        double cells[16]{}; unsigned counts[16]{};
        for (unsigned y = 0; y < capture_height_; ++y) {
            const auto* row = data + capture_layout_.Offset + static_cast<std::size_t>(y) * capture_layout_.Footprint.RowPitch;
            for (unsigned x = 0; x < capture_width_; ++x) {
                const unsigned cell = (y * 4 / capture_height_) * 4 + x * 4 / capture_width_;
                cells[cell] += 0.2126 * row[4 * x] + 0.7152 * row[4 * x + 1] + 0.0722 * row[4 * x + 2]; ++counts[cell];
                if (x + 1 == seam || x == seam) continue; // The seam's two columns belong to neither half.
                auto& h = x < seam ? c.left : c.right;
                for (unsigned k = 0; k < 4; ++k) { h ^= row[4 * x + k]; h *= 1099511628211ull; }
            }
        }
        for (unsigned k = 0; k < 16; ++k) c.grid[k] = counts[k] ? static_cast<float>(cells[k] / counts[k]) : 0.0f;
        if (capture_horizon_) { // The shown picture's own lines (auto-leveling's test): its red channel, the tests' pictures being grey.
            std::uint32_t words[video::HorizonLevel::kWords]{};
            video::HorizonLevel::Accumulate(data + capture_layout_.Offset, capture_layout_.Footprint.RowPitch, capture_width_, capture_height_, words, 4, 0);
            c.horizon = video::HorizonLevel::Measure(words);
            std::uint32_t extents[video::Matte::kWords]{}; // And where the shown image has picture (full-range RGB: black is 0).
            video::Matte::Accumulate(data + capture_layout_.Offset, capture_layout_.Footprint.RowPitch, capture_width_, capture_height_, extents, 0, 4);
            c.extents_found = video::Matte::Extents(extents, capture_width_, capture_height_, c.extents);
        }
        const D3D12_RANGE none{0, 0}; capture_readback_->Unmap(0, &none);
        ++capture_count_;
        return RenderStatus::Ready;
    }
    const bool held = capture_requested_ && history_valid_, reference = !held && reference_capture_requested_ && reference_valid_;
    if ((!held && !reference) || completed < still_fence_ || completed < pin_fence_) return RenderStatus::WouldBlock;
    if (held) capture_requested_ = false; else reference_capture_requested_ = false;
    auto* source = held ? history_ : reference_;
    const unsigned width = held ? history_width_ : reference_width_, height = held ? history_height_ : reference_height_;
    capture_kind_ = held ? CaptureKind::Held : CaptureKind::Reference;
    error_ = still_allocator_->Reset(); if (SUCCEEDED(error_)) error_ = still_commands_->Reset(still_allocator_, nullptr);
    if (FAILED(error_)) return RenderStatus::Failed;
    auto b = Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE); still_commands_->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION to{capture_readback_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = capture_layout_;
    D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    still_commands_->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter); still_commands_->ResourceBarrier(1, &b);
    error_ = still_commands_->Close(); if (FAILED(error_)) return RenderStatus::Failed;
    ID3D12CommandList* lists[]{still_commands_}; queue_->ExecuteCommandLists(1, lists);
    still_fence_ = capture_fence_ = ++sequence_; error_ = queue_->Signal(fence_, capture_fence_); if (FAILED(error_)) return RenderStatus::Failed;
    capture_width_ = width; capture_height_ = height; capture_pending_ = true;
    return RenderStatus::Ready;
}
void D3D12Renderer::HarvestScenes(std::uint64_t completed) noexcept {
    if (!stats_data_ || completed == UINT64_MAX) return;
    for (;;) {
        Slot* next = nullptr;
        for (auto& s : slots_) if (s.stats && completed >= s.fence && (!next || s.fence < next->fence)) next = &s;
        if (!next) return;
        auto& s = *next; s.stats = false;
        const auto slot = static_cast<unsigned>(&s - slots_);
        if (s.horizon) {
            // Auto-leveling: this picture's edge orientations (1024 bytes), of the current timeline only.
            s.horizon = false; ++horizon_reads_;
            std::uint32_t words[video::HorizonLevel::kWords];
            static_assert(sizeof(words) == ColorPipeline::kHorizonBytes);
            std::memcpy(words, stats_data_ + std::size_t{ColorPipeline::kStatsBytes} * Count + std::size_t{ColorPipeline::kHorizonBytes} * slot, sizeof(words));
            if (auto_level_ && s.stats_epoch == epoch_) { horizon_.Push(s.frame.pts_100ns, video::HorizonLevel::Measure(words)); FollowHorizon(); }
        }
        std::uint32_t d[ColorPipeline::kStatsBytes / 4]; std::memcpy(d, stats_data_ + ColorPipeline::kStatsBytes * slot, sizeof(d));
        // Black bars: the extents of a picture converted whole (a zoomed region shows none of its bars), any timeline (they are
        // the file's).
        if (s.region.left == s.crop.left && s.region.top == s.crop.top && s.region.right == s.crop.right && s.region.bottom == s.crop.bottom) {
            const unsigned cw = s.crop.right - s.crop.left, ch = s.crop.bottom - s.crop.top;
            if (matte_.Width() != cw || matte_.Height() != ch) { matte_.Reset(cw, ch); cropped_changes_ = 0; }
            matte_.Push(s.frame.pts_100ns, d + 10); FollowBars();
        }
        // This picture's share of the running totals (unsigned differences: the totals wrap).
        const std::uint32_t sum = d[6] - stats_sum_, count = d[7] - stats_count_, cuts = d[1] - stats_cuts_;
        stats_sum_ = d[6]; stats_count_ = d[7]; stats_cuts_ = d[1];
        if (!count) continue;
        last_luma_ = static_cast<float>(static_cast<double>(sum) / count / 1023.0); ++luma_pictures_;
        // The frame signature: this picture's brightest sample and moving blocks (against the picture converted before it, when
        // that one was of the same epoch and region: otherwise the blocks hold another picture's samples).
        const bool continued = s.stats_epoch == motion_epoch_ && s.region.left == motion_region_.left && s.region.top == motion_region_.top &&
                               s.region.right == motion_region_.right && s.region.bottom == motion_region_.bottom;
        s.luma = last_luma_; s.peak = static_cast<float>(d[8] & 1023u) / 1023.0f; s.motion = continued ? static_cast<float>(d[9]) / static_cast<float>(count) : -1.0f;
        s.measured = true; motion_epoch_ = s.stats_epoch; motion_region_ = s.region;
        // Only the current timeline's pictures: one converted before a seek has no neighbour in it.
        std::int64_t cut = 0;
        if (s.stats_epoch == epoch_ && cut_detector_.Push(s.frame.pts_100ns, last_luma_, cuts != 0, cut)) { cuts_[cut_count_++ % kCutRing] = cut; s.cut = true; s.cut_pts = cut; }
    }
}
bool D3D12Renderer::MakeSnapshotReadback(unsigned width, unsigned height) noexcept {
    // On the coordinator's snapshot thread (the device is free-threaded; the presentation thread does not touch the readback
    // while no snapshot is requested): a readback of the window's size, persistently mapped, kept only while one is made.
    if (snapshot_data_ && width <= snapshot_width_ && height <= snapshot_height_) return true;
    DropSnapshotReadback();
    if (!device_ || !width || !height) return false;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = width; texture.Height = height;
    texture.DepthOrArraySize = texture.MipLevels = 1; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1;
    UINT64 bytes = 0; device_->GetCopyableFootprints(&texture, 0, 1, 0, &snapshot_layout_, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr; const D3D12_RANGE everything{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&snapshot_readback_))) ||
        FAILED(snapshot_readback_->Map(0, &everything, &mapped))) { Release(snapshot_readback_); return false; }
    snapshot_data_ = static_cast<const std::uint8_t*>(mapped);
    snapshot_width_ = width; snapshot_height_ = height;
    snapshot_bytes_.store(bytes, std::memory_order_relaxed);
    if (bytes > snapshot_peak_.load(std::memory_order_relaxed)) snapshot_peak_.store(bytes, std::memory_order_relaxed);
    return true;
}
void D3D12Renderer::DropSnapshotReadback() noexcept {
    if (snapshot_readback_ && snapshot_data_) { const D3D12_RANGE none{0, 0}; snapshot_readback_->Unmap(0, &none); }
    snapshot_data_ = nullptr; Release(snapshot_readback_); snapshot_width_ = snapshot_height_ = 0;
    snapshot_bytes_.store(0, std::memory_order_relaxed);
}
bool D3D12Renderer::RequestSnapshot() noexcept {
    // API 14: an HDR texture format's composites are not 8-bit RGBA: no snapshot of them (the host has the texture).
    if (snapshot_state_ != SnapshotState::Idle || !snapshot_data_ || music_ || !history_valid_ || HdrOutput()) return false;
    snapshot_state_ = SnapshotState::Requested; redraw_ = true; // Paused: the picture held is composed again for it.
    return true;
}
bool D3D12Renderer::SnapshotReady(SnapshotImage& out) noexcept {
    if (snapshot_state_ == SnapshotState::Copying) {
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX || completed < snapshot_fence_) return false;
        snapshot_state_ = SnapshotState::Ready;
    }
    if (snapshot_state_ != SnapshotState::Ready) return false;
    out = snapshot_;
    return true;
}
bool D3D12Renderer::PrepareTargets(core::LinearMemoryArena& arena) noexcept {
    StopPerception();
    targets_ready_ = false;
    if (!video_width_ || !video_height_) return false;
    const unsigned scale = video::GridScale(video_width_, video_height_);
    const unsigned width = (video_width_ + scale - 1) / scale, height = (video_height_ + scale - 1) / scale;
    if (!targets_.Configure(arena, width, height, 0)) return false; // Under Perception::kMinSize cells a side: no thread.
    // The grid handed to perception's thread (at most 480 x 272 bytes), the thread and its wake (the event camera's too).
    perceive_cells_ = static_cast<std::uint8_t*>(arena.Allocate(std::size_t{width} * height, 64));
    if (!perceive_cells_ || !(perceive_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr))) { StopPerception(); return false; }
    perceived_[0] = perceived_[1] = {}; published_.store(0, std::memory_order_relaxed);
    perceive_state_.store(0, std::memory_order_relaxed); perceive_stop_.store(false, std::memory_order_relaxed); reset_pending_ = false;
    perceive_thread_ = CreateThread(nullptr, 0, PerceiveEntry, this, 0, nullptr);
    if (!perceive_thread_) { StopPerception(); return false; }
    (void)SetThreadDescription(perceive_thread_, L"media perception");
    targets_ready_ = true;
    return true;
}
DWORD WINAPI D3D12Renderer::PerceiveEntry(void* context) noexcept { static_cast<D3D12Renderer*>(context)->PerceiveThread(); return 0; }
void D3D12Renderer::PerceiveThread() noexcept {
    for (;;) {
        (void)WaitForSingleObject(perceive_wake_, INFINITE);
        if (perceive_stop_.load(std::memory_order_acquire)) return;
        if (steady_job_.load(std::memory_order_acquire) == 1) Steady(); // First: a picture's conversion waits for it.
        if (perceive_state_.load(std::memory_order_acquire) != 1) continue;
        if (perceive_run_) {
            if (perceive_reset_) targets_.Reset();
            (void)targets_.Push(perceive_cells_, perceive_width_, perceive_pts_, {});
            const unsigned next = published_.load(std::memory_order_relaxed) ^ 1u;
            perceived_[next] = targets_.Result();
            published_.store(next, std::memory_order_release);
        }
        if (sense_run_) {
            if (sense_restart_) { events_.SetOptions(sense_options_); events_.Reset(); grid_imu_.Reset(); }
            Sense(perceive_cells_, perceive_width_, perceive_height_, perceive_pts_, sense_epoch_);
        }
        perceive_state_.store(0, std::memory_order_release);
    }
}
void D3D12Renderer::StopPerception() noexcept {
    if (perceive_thread_) {
        perceive_stop_.store(true, std::memory_order_release); (void)SetEvent(perceive_wake_);
        (void)WaitForSingleObject(perceive_thread_, INFINITE); CloseHandle(perceive_thread_); perceive_thread_ = nullptr;
    }
    steady_job_.store(0, std::memory_order_relaxed); steady_stage_ = 0; // A measurement asked for has no thread to run on any more.
    if (perceive_wake_) { CloseHandle(perceive_wake_); perceive_wake_ = nullptr; }
    perceive_cells_ = nullptr; targets_ready_ = false;
}
void D3D12Renderer::Steady() noexcept {
    // On perception's thread (docs/picture.md, "Stabilising the window"): the grid in place, in its slot's readback.
    const auto a = steady_job_data_;
    if (a.restart) { grid_imu_.Reset(); stabilizer_.Reset(); steady_mean_ = -1; steady_newest_ = INT64_MIN; steady_reversed_ = false; }
    // A cut is no shake: the view starts over at the middle of what it shows, where nobody sees it move.
    std::uint64_t sum = 0;
    for (unsigned y = 0; y < a.height; ++y) { const auto* row = a.cells + std::size_t{y} * a.stride; for (unsigned x = 0; x < a.width; ++x) sum += row[x]; }
    const float mean = static_cast<float>(static_cast<double>(sum) / (std::max)(1u, a.width * a.height) / 255.0);
    if (steady_mean_ >= 0 && std::fabs(mean - steady_mean_) > timeline::CutDetector::kCutLuma) { grid_imu_.Reset(); stabilizer_.Reset(); steady_newest_ = INT64_MIN; steady_reversed_ = false; }
    steady_mean_ = mean;
    // In decode order: to and from a B-picture before the newest one, the motion reverses.
    const bool reversed = a.pts < steady_newest_;
    if (reversed || steady_reversed_) grid_imu_.ForgetMotion();
    steady_reversed_ = reversed; steady_newest_ = (std::max)(steady_newest_, a.pts);
    const auto& seen = targets_.Result(); // This thread's own: the objects are left out of the background.
    const auto& r = grid_imu_.Push(a.cells, a.stride, seen.objects, seen.object_count);
    stabilizer_.PushDecoded(a.pts, grid_imu_.Ego());
    float zoom = a.zoom, x = a.x, y = a.y, turn = 0;
    stabilizer_.Apply(zoom, x, y, turn);
    steady_result_ = {{x, y, turn, true}, r.dx, r.dy, r.turn, r.valid};
    if (r.valid) steady_measured_.fetch_add(1, std::memory_order_relaxed);
    steady_job_.store(2, std::memory_order_release);
    if (const HANDLE wake = steady_wake_.exchange(nullptr, std::memory_order_acq_rel)) (void)SetEvent(wake);
}
RenderStatus D3D12Renderer::Measure(const core::VideoFrameDescriptor& frame, unsigned slot, const platform::windows::D3D12VideoDecoder& decoder, std::uint64_t completed,
                                    SteadyView& view) noexcept {
    if (steady_stage_ && (steady_pts_ != frame.pts_100ns || steady_surface_ != frame.surface_id || steady_slot_ != slot)) {
        // Another picture (a seek took this one), or its slot went to a pinned landing: it starts over, once the thread has let go.
        if (steady_stage_ == 2 && steady_job_.load(std::memory_order_acquire) == 1) return RenderStatus::WouldBlock;
        steady_job_.store(0, std::memory_order_relaxed); steady_stage_ = 0;
    }
    if (!steady_stage_) {
        const unsigned w = frame.crop_right - frame.crop_left, h = frame.crop_bottom - frame.crop_top, scale = video::GridScale(w, h);
        const unsigned width = (w + scale - 1) / scale, height = (h + scale - 1) / scale, words = (width + 3) / 4;
        if (!StabilizationPossible() || std::uint64_t{words} * 4 * height > kGridBytes) return RenderStatus::Ready; // Not measured: the picture as it comes.
        error_ = steady_allocator_->Reset(); if (SUCCEEDED(error_)) error_ = steady_commands_->Reset(steady_allocator_, color_.Initial());
        if (FAILED(error_)) return RenderStatus::Failed;
        auto* source = decoder.SurfaceAt(frame.surface_id);
        auto b = Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); steady_commands_->ResourceBarrier(1, &b);
        ID3D12DescriptorHeap* heaps[]{descriptors_}; steady_commands_->SetDescriptorHeaps(1, heaps);
        const auto base = descriptors_->GetGPUDescriptorHandleForHeapStart();
        auto planes = base; planes.ptr += static_cast<UINT64>(frame.surface_id * 2) * stride_;
        auto target = base; target.ptr += static_cast<UINT64>(surfaces_ * 2 + slot) * stride_;
        ColorConstants whole{}; whole.left = frame.crop_left; whole.top = frame.crop_top; whole.right = frame.crop_right; whole.bottom = frame.crop_bottom; whole.ten_bit = ten_bit_ ? 1u : 0u;
        color_.Bind(steady_commands_, planes, target, whole);
        color_.RecordGrid(steady_commands_, whole, scale, words, height, grid_, grid_readback_, std::uint64_t{kGridBytes} * slot);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); steady_commands_->ResourceBarrier(1, &b);
        error_ = steady_commands_->Close(); if (FAILED(error_)) return RenderStatus::Failed;
        ID3D12CommandList* lists[]{steady_commands_}; queue_->ExecuteCommandLists(1, lists);
        steady_fence_ = ++sequence_; error_ = queue_->Signal(fence_, steady_fence_); if (FAILED(error_)) return RenderStatus::Failed;
        steady_stage_ = 1; steady_slot_ = slot; steady_pts_ = frame.pts_100ns; steady_surface_ = frame.surface_id;
        steady_grid_width_ = width; steady_grid_height_ = height; steady_grid_words_ = words;
        return RenderStatus::WouldBlock;
    }
    if (steady_stage_ == 1) {
        if (completed < steady_fence_) return RenderStatus::WouldBlock;
        const bool restart = steady_restart_ || steady_epoch_ != frame.playback_epoch;
        steady_restart_ = false; steady_epoch_ = frame.playback_epoch;
        steady_job_data_ = {grid_data_ + std::uint64_t{kGridBytes} * slot, steady_grid_words_ * 4, steady_grid_width_, steady_grid_height_, frame.pts_100ns, ViewZoom(), zoom_x_, zoom_y_, restart};
        steady_job_.store(1, std::memory_order_release); (void)SetEvent(perceive_wake_);
        steady_stage_ = 2;
        return RenderStatus::WouldBlock;
    }
    if (steady_stage_ == 2) {
        if (steady_job_.load(std::memory_order_acquire) != 2) return RenderStatus::WouldBlock;
        steady_job_.store(0, std::memory_order_relaxed); steady_stage_ = 3;
    }
    view = steady_result_.view;
    return RenderStatus::Ready;
}
bool D3D12Renderer::SetStabilization(bool on) noexcept {
    if (on && !StabilizationPossible()) return false;
    steady_wanted_.store(on, std::memory_order_relaxed);
    return true;
}
void D3D12Renderer::ApplyStabilization() noexcept {
    const bool wanted = steady_wanted_.load(std::memory_order_relaxed);
    if (wanted == steady_on_) return;
    // The view's crop changes as a zoom does: what is converted already goes through the blit, what comes next is steadied.
    steady_on_ = wanted; steady_restart_ = true; held_steady_ = {};
    ++zoom_generation_; redraw_ = true;
    if (!hud_paused_ && window_) zoom_redraw_ = true;
    SettingsChanged();
    ShowStatus(wanted ? "Stabilisation on" : "Stabilisation off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
bool D3D12Renderer::NextTarget() noexcept {
    if (!targets_ready_ || !grid_data_) return false;
    const auto& r = Objects();
    if (!r.object_count) return false;
    // After the object followed now, in the table's order (the most urgent first); the first when none is.
    int current = -1;
    if (Tracking()) for (unsigned i = 0; i < r.object_count; ++i) if (r.objects[i].track_id == target_id_) current = static_cast<int>(i);
    if (current >= 0 && r.object_count == 1) return false;
    const auto& o = r.objects[current < 0 ? 0u : (static_cast<unsigned>(current) + 1) % r.object_count];
    // The view at once (a live zoom: the next composite shows it), the lock on the next converted picture, at the object.
    if (zoom_ <= 1.0f) { SetZoom(2.0f, o.x, o.y, false); track_zoomed_ = !tracker_.Active(); } else Follow(o.x, o.y);
    retarget_ = true; retarget_x_ = o.x; retarget_y_ = o.y; track_pending_ = true; track_lock_failed_ = false;
    target_id_ = o.track_id; ++retargets_;
    return true;
}
void D3D12Renderer::FollowObject() noexcept {
    // The object holding the tracker's position (its box and a twentieth of the picture around it), the target's own track
    // first; none: the id stays (an object missed for a picture or two).
    const auto& st = tracker_.Status();
    if (st.state != video::TrackState::Tracking || track_pending_) return;
    if (const auto holder = ObjectAt(st.x, st.y)) target_id_ = holder;
}
std::uint32_t D3D12Renderer::ObjectAt(float x, float y) const noexcept {
    if (!targets_ready_) return 0;
    const auto& r = Objects();
    std::uint32_t holder = 0;
    for (unsigned i = 0; i < r.object_count; ++i) {
        const auto& o = r.objects[i];
        if (std::fabs(x - o.x) > 0.5f * o.width + 0.05f || std::fabs(y - o.y) > 0.5f * o.height + 0.05f) continue;
        if (!holder || o.track_id == target_id_) holder = o.track_id;
    }
    return holder;
}
void D3D12Renderer::SetTracking(bool on) noexcept {
    tracker_.Stop(); track_lock_failed_ = false; grid_mean_ = -1; track_zoomed_ = false;
    retarget_ = false; target_id_ = 0; // Perception goes on (2026-11-01: its table is warm; T's target is the object there).
    track_pending_ = on && grid_data_;
    if (track_pending_) target_id_ = ObjectAt(zoom_x_, zoom_y_);
    // The view answers T on the next composite: the zoom goes to 2x now (through the pictures already converted), the lock
    // follows on the next converted picture's grid.
    if (track_pending_ && zoom_ <= 1.0f) { SetZoom(2.0f, zoom_x_, zoom_y_, false); track_zoomed_ = true; }
}
void D3D12Renderer::Follow(float x, float y) noexcept {
    const float half = 0.5f / zoom_;
    x = (std::clamp)(x, half, 1.0f - half); y = (std::clamp)(y, half, 1.0f - half);
    if (x == zoom_x_ && y == zoom_y_) return;
    zoom_x_ = x; zoom_y_ = y; ++zoom_generation_;
}
D3D12Renderer::Region D3D12Renderer::TrackedView(const Slot& s) const noexcept {
    // Where the target is at this picture's time: the tracker's estimate (for the picture it last measured, converted after
    // this one) carried back along its velocity; before the lock, or lost, the view as it stands.
    const auto& st = tracker_.Status();
    if (st.state != video::TrackState::Tracking || estimate_duration_ <= 0) return RegionOf(s.crop);
    const float pictures = (std::clamp)(static_cast<float>(static_cast<double>(s.frame.pts_100ns - estimate_pts_) / static_cast<double>(estimate_duration_)), -64.0f, 64.0f);
    return RegionAt(s.crop, st.x + st.vx * pictures, st.y + st.vy * pictures);
}
bool D3D12Renderer::RequestTensor(const TensorAsk& ask) noexcept {
    if (!device_ || !ask.width || !ask.height || std::uint64_t{ask.width} * ask.height > kMaxTensorCells || ask.kind > kTensorCustom) return false;
    std::uint32_t idle = 0;
    if (!tensor_state_.compare_exchange_strong(idle, 3, std::memory_order_acq_rel)) return false; // Another request is in flight.
    const std::uint64_t cells = std::uint64_t{ask.width} * ask.height;
    if (cells > tensor_cells_) {
        // Made or grown here, on the asking thread (the device is free-threaded): the presentation thread allocates nothing.
        if (tensor_readback_ && tensor_data_) { const D3D12_RANGE none{0, 0}; tensor_readback_->Unmap(0, &none); }
        tensor_data_ = nullptr; Release(tensor_readback_); Release(tensor_buffer_); tensor_cells_ = 0;
        D3D12_HEAP_PROPERTIES gpu{}; gpu.Type = D3D12_HEAP_TYPE_DEFAULT; gpu.CreationNodeMask = gpu.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = cells * 12 + 16; buffer.Height = 1; // The cells, then an HDR picture's peak.
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK; readback.CreationNodeMask = readback.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rows = buffer; rows.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* mapped = nullptr; const D3D12_RANGE all{0, static_cast<SIZE_T>(rows.Width)};
        if (SUCCEEDED(device_->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tensor_buffer_))) &&
            SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rows, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tensor_readback_))) &&
            SUCCEEDED(tensor_readback_->Map(0, &all, &mapped))) { tensor_data_ = static_cast<const float*>(mapped); tensor_cells_ = cells; }
        else { Release(tensor_buffer_); Release(tensor_readback_); tensor_state_.store(0, std::memory_order_release); return false; }
    }
    tensor_ask_ = ask;
    tensor_state_.store(1, std::memory_order_release);
    if (input_wake_) (void)SetEvent(input_wake_); // A paused presentation thread sleeps until its next advance of the controls.
    return true;
}
int D3D12Renderer::WaitTensor(TensorAnswer& answer, std::uint32_t timeout_ms) noexcept {
    // Its conversion recorded, then run (the fence): a host's thread waits, a millisecond at a time.
    for (std::uint32_t waited = 0;; ++waited) {
        const auto state = tensor_state_.load(std::memory_order_acquire);
        if (state == 2) {
            const auto done = fence_->GetCompletedValue();
            if (done == UINT64_MAX) return -1;
            if (done >= tensor_fence_.load(std::memory_order_relaxed)) {
                answer = tensor_answer_;
                if (video::IsHdrTensor(answer.color)) answer.color.peak_nits = static_cast<float>(video::PqToNits(tensor_data_[3 * std::uint64_t{tensor_ask_.width} * tensor_ask_.height]));
                return 1;
            }
        } else if (state != 1 && state != 4) return -1;
        if (waited >= timeout_ms) return 0;
        Sleep(1);
    }
}
void D3D12Renderer::FinishTensor() noexcept {
    // A request the presentation thread has not taken yet is withdrawn; a recorded one is over once its fence has passed.
    std::uint32_t asked = 1;
    if (tensor_state_.compare_exchange_strong(asked, 0, std::memory_order_acq_rel)) return;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto state = tensor_state_.load(std::memory_order_acquire);
        if (state != 4 && (state != 2 || fence_->GetCompletedValue() >= tensor_fence_.load(std::memory_order_relaxed))) break;
        Sleep(1);
    }
    tensor_state_.store(0, std::memory_order_release);
}
bool D3D12Renderer::TensorRegion(const core::VideoFrameDescriptor& frame, const SteadyView& steady, video::AreaRect& out) const noexcept {
    const double w = frame.crop_right - frame.crop_left, h = frame.crop_bottom - frame.crop_top, aspect = static_cast<double>(tensor_ask_.width) / tensor_ask_.height;
    if (!(w > 0) || !(h > 0)) return false;
    const auto& a = tensor_ask_;
    if (a.kind == kTensorFull) { out = {0, 0, w, h}; return true; }
    if (a.kind == kTensorCustom) {
        if (!(a.left >= 0 && a.top >= 0 && a.right <= 1 && a.bottom <= 1 && a.right > a.left && a.bottom > a.top)) return false;
        out = {a.left * w, a.top * h, a.right * w, a.bottom * h};
        return true;
    }
    if (a.kind == kTensorView) {
        const auto r = RegionOf(frame);
        out = {static_cast<double>(r.left - frame.crop_left), static_cast<double>(r.top - frame.crop_top), static_cast<double>(r.right - frame.crop_left),
               static_cast<double>(r.bottom - frame.crop_top)};
        if (steady.on) { // The view shown: the region at the steadied centre (its turn left out).
            const double dx = steady.x * w - 0.5 * (out.left + out.right), dy = steady.y * h - 0.5 * (out.top + out.bottom);
            out.left += dx; out.right += dx; out.top += dy; out.bottom += dy;
        }
        return true;
    }
    // The fovea: the target while tracking (its object's box grown by a fifth, else the fovea's size about the tracker), else
    // half the view at its centre; shaped as the tensor (the smaller side grown), then scaled into the picture.
    const double zoom = ViewZoom() > 1.0f ? ViewZoom() : 1.0;
    double cx = (steady.on ? steady.x : zoom_x_) * w, cy = (steady.on ? steady.y : zoom_y_) * h, rw = 0.5 * w / zoom, rh = 0.5 * h / zoom;
    const auto& st = tracker_.Status();
    if (Tracking() && st.state == video::TrackState::Tracking) {
        cx = st.x * w; cy = st.y * h;
        const auto& objects = Objects();
        for (unsigned i = 0; i < objects.object_count; ++i)
            if (target_id_ && objects.objects[i].track_id == target_id_) {
                cx = objects.objects[i].x * w; cy = objects.objects[i].y * h; rw = 1.2 * objects.objects[i].width * w; rh = 1.2 * objects.objects[i].height * h;
            }
    }
    if (rw / rh < aspect) rw = rh * aspect; else rh = rw / aspect;
    const double fit = (std::min)({1.0, w / rw, h / rh});
    rw *= fit; rh *= fit;
    cx = std::clamp(cx, rw / 2, w - rw / 2); cy = std::clamp(cy, rh / 2, h - rh / 2);
    out = {cx - rw / 2, cy - rh / 2, cx + rw / 2, cy + rh / 2};
    return true;
}
bool D3D12Renderer::PrepareSensing(core::LinearMemoryArena& arena) noexcept {
    if (!video_width_ || !video_height_) return false;
    const unsigned scale = video::GridScale(video_width_, video_height_);
    const unsigned width = (video_width_ + scale - 1) / scale, height = (video_height_ + scale - 1) / scale;
    events_on_ = false; events_applied_ = 0; events_request_.store(0, std::memory_order_relaxed);
    // On perception's thread: none without it (a grid under Perception::kMinSize cells a side).
    if (!perceive_thread_ || !events_.Configure(arena, width, height, kEventRing)) { events_.Release(); return false; }
    if (!grid_imu_.Configure(arena, width, height, 0)) grid_imu_.Release();
    stabilizer_.Configure(width, height, 0); // Of the same pictures, at the same field of view: its fractions are the crop's.
    steady_wanted_.store(false, std::memory_order_relaxed); steady_on_ = false; steady_epoch_ = UINT32_MAX;
    return true;
}
bool D3D12Renderer::SetEventCamera(const video::EventCameraOptions* options) noexcept {
    if (!events_.Configured()) return false;
    // Packed for the presentation thread: the sequence (16 bits), on, stabilised, the events a pixel may fire (5 bits), the
    // threshold's float bits (32).
    const std::uint64_t old = events_request_.load(std::memory_order_relaxed);
    std::uint64_t request = ((old & 0xFFFFu) + 1) & 0xFFFFu;
    if (options) {
        std::uint32_t bits; const float threshold = options->contrast_threshold; std::memcpy(&bits, &threshold, 4);
        request |= 1u << 16 | (options->stabilized ? 1u << 17 : 0u) | (std::min)(options->max_per_pixel, 31u) << 18 | std::uint64_t{bits} << 32;
    }
    events_request_.store(request, std::memory_order_release);
    return true;
}
void D3D12Renderer::ApplyEventRequest() noexcept {
    const std::uint64_t request = events_request_.load(std::memory_order_acquire);
    if ((request & 0xFFFFu) == events_applied_ || !events_.Configured()) return;
    events_applied_ = static_cast<std::uint32_t>(request & 0xFFFFu);
    const bool on = (request >> 16) & 1u;
    if (on) {
        video::EventCameraOptions o;
        const auto bits = static_cast<std::uint32_t>(request >> 32); std::memcpy(&o.contrast_threshold, &bits, 4);
        o.stabilized = (request >> 17) & 1u; o.max_per_pixel = static_cast<unsigned>((request >> 18) & 31u);
        events_options_ = o; events_restart_ = true; // Perception's thread applies them with the next grid it takes.
    }
    events_on_ = on;
}
D3D12Renderer::EventCameraState D3D12Renderer::EventState() const noexcept {
    EventCameraState s;
    s.configured = events_.Configured();
    if (!s.configured) return s;
    s.on = (events_request_.load(std::memory_order_acquire) >> 16) & 1u;
    s.width = events_.Width(); s.height = events_.Height(); s.scale = events_.Scale();
    s.pictures = events_pictures_.load(std::memory_order_acquire); s.rebases = events_rebases_.load(std::memory_order_relaxed);
    s.last = events_last_.load(std::memory_order_relaxed); s.events = events_emitted_.load(std::memory_order_relaxed); s.dropped = events_dropped_.load(std::memory_order_relaxed);
    s.skipped = events_skipped_.load(std::memory_order_relaxed);
    return s;
}
void D3D12Renderer::Sense(const std::uint8_t* cells, unsigned width, unsigned height, std::int64_t pts, std::uint32_t epoch) noexcept {
    // On perception's thread: the event camera on the grid (its picture: the tracking grid's cells, tight), stabilised by the
    // virtual IMU's motion since the previous grid (cells), the objects perception keeps track of left out of the background.
    if (!events_.Configured() || width / events_.Scale() != events_.Width() || height / events_.Scale() != events_.Height()) return;
    if (events_epoch_ != epoch) { events_.Reset(); grid_imu_.Reset(); events_epoch_ = epoch; }
    float dx = 0, dy = 0, roll = 0;
    if (sense_measured_) { dx = sense_given_x_; dy = sense_given_y_; roll = sense_given_turn_; } // The view's stabilisation has measured them.
    else if (grid_imu_.Configured() && events_.Options().stabilized) {
        const auto& seen = targets_.Result(); // This thread's own (the table published from it).
        const auto& r = grid_imu_.Push(cells, width, seen.objects, seen.object_count);
        if (r.valid) { dx = r.dx; dy = r.dy; roll = r.turn; }
    }
    (void)events_.Push(cells, width, pts, dx, dy, roll);
    events_emitted_.store(events_.Emitted(), std::memory_order_relaxed); events_dropped_.store(events_.Dropped(), std::memory_order_relaxed);
    events_rebases_.store(events_.Rebases(), std::memory_order_relaxed); events_last_.store(events_.LastCount(), std::memory_order_relaxed);
    events_pictures_.store(events_.Pictures(), std::memory_order_release);
}
bool D3D12Renderer::VibroRegion(const core::VideoFrameDescriptor& frame, video::AreaRect& region, bool& follow) noexcept {
    // Remote vibrometry's region in a picture converted now (2026-11-03; docs/picture.md, "Vibrometry in the window"): while
    // tracking (T) the fovea's size centred on the target's path at this picture's time (the line through the tracker's last
    // second of positions, measured on pictures converted before this one: the region follows the target and the window
    // goes on); with micro-motion on, the view's fovea (half the view across and down at its centre). Off: the window goes.
    const bool on = Tracking() || picture_.micro_motion > 1.0f;
    if (!on) { if (vibro_on_) { vibro_.Reset(); vibro_on_ = false; } return false; }
    const double w = frame.crop_right - frame.crop_left, h = frame.crop_bottom - frame.crop_top, zoom = zoom_ > 1.0f ? zoom_ : 1.0f;
    const double fw = (std::min)(w, 0.5 * w / zoom), fh = (std::min)(h, 0.5 * h / zoom);
    double cx = zoom_x_ * w, cy = zoom_y_ * h;
    follow = false;
    if (Tracking() && tracker_.Status().state == video::TrackState::Tracking && vibro_path_.At(frame.pts_100ns, cx, cy)) follow = true;
    cx = std::clamp(cx, fw / 2, w - fw / 2); cy = std::clamp(cy, fh / 2, h - fh / 2);
    region = {cx - fw / 2, cy - fh / 2, cx + fw / 2, cy + fh / 2};
    return fw >= 8 && fh >= 8;
}
void D3D12Renderer::HarvestVibro(std::uint64_t completed) noexcept {
    if (!vibro_data_ || completed == UINT64_MAX) return;
    for (;;) { // In conversion order.
        Slot* next = nullptr;
        for (auto& s : slots_) if (s.vibro && completed >= s.fence && (!next || s.fence < next->fence)) next = &s;
        if (!next) return;
        next->vibro = false;
        if (next->frame.playback_epoch == epoch_) Vibrate(*next);
    }
}
void D3D12Renderer::Vibrate(const Slot& s) noexcept {
    // A new epoch, tracking turned on or off, a lock on a target (a cut, the target found again elsewhere): the window over.
    if (!vibro_on_ || vibro_epoch_ != epoch_ || vibro_tracking_ != Tracking() || (Tracking() && vibro_lock_ != locks_)) {
        vibro_.Reset(); vibro_on_ = true; vibro_epoch_ = epoch_; vibro_tracking_ = Tracking(); vibro_lock_ = locks_; readout_pts_ = INT64_MIN;
    }
    // The sample: seven floats a cell (HdrToneMap.hlsl Vibro); the region's means are its cells' (equal areas).
    constexpr unsigned kCells = video::kVibrometryCells * video::kVibrometryCells, kValues = ColorPipeline::kVibroValues;
    const auto* v = reinterpret_cast<const float*>(vibro_data_ + std::uint64_t{kVibroBytes} * static_cast<unsigned>(&s - slots_));
    double luma = 0, cb = 0, cr = 0;
    for (unsigned k = 0; k < kCells; ++k) {
        const float* c = v + k * kValues;
        vibro_sample_.cells[k] = c[0]; vibro_sample_.right[k] = c[1]; vibro_sample_.left[k] = c[2]; vibro_sample_.below[k] = c[3]; vibro_sample_.above[k] = c[4];
        luma += c[0]; cb += c[5]; cr += c[6];
    }
    vibro_sample_.luma = static_cast<float>(luma / kCells); vibro_sample_.cb = static_cast<float>(cb / kCells); vibro_sample_.cr = static_cast<float>(cr / kCells);
    vibro_.Push(vibro_sample_, s.vibro_region, s.frame.pts_100ns, s.vibro_follow);
    // The readout: every kReadoutInterval of media while a component stands above the noise ("Pulse 72 BPM", "Vibration
    // 8.0 Hz", or both), assembled in place (no formatting library: it allocates).
    const auto& r = vibro_.Result();
    const bool vital = r.vital_confidence > 0, vibration = r.confidence > 0 && !(vital && std::fabs(r.vibration_hz * 60 - r.vital_bpm) < 1.0f);
    if ((!vital && !vibration) || (readout_pts_ != INT64_MIN && s.frame.pts_100ns - readout_pts_ < kReadoutInterval && s.frame.pts_100ns >= readout_pts_)) return;
    readout_pts_ = s.frame.pts_100ns;
    char text[64]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
    const auto number = [&](unsigned value) { char d[12]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + value % 10); value /= 10; } while (value && k < 11); while (k && n + 1 < sizeof(text)) text[n++] = d[--k]; };
    if (vibration) {
        const auto tenths = static_cast<unsigned>(std::lround(r.vibration_hz * 10));
        put("Vibration "); number(tenths / 10); put("."); number(tenths % 10); put(" Hz");
    }
    if (vital) { put(vibration ? " \xc2\xb7 pulse " : "Pulse "); number(static_cast<unsigned>(std::lround(r.vital_bpm))); put(" BPM"); }
    text[n] = 0;
    std::memcpy(readout_, text, n + 1); ++readouts_;
    ShowStatus({text, n}, ui::StatusIcon::None, ui::StatusTone::Info);
}
void D3D12Renderer::HarvestGrid(std::uint64_t completed) noexcept {
    if (!grid_data_ || completed == UINT64_MAX) return;
    Slot* next = nullptr; // In conversion order.
    for (auto& s : slots_) if (s.grid && completed >= s.fence && (!next || s.fence < next->fence)) next = &s;
    if (!next) return;
    auto& s = *next; s.grid = false;
    if (s.frame.playback_epoch != epoch_) return;
    const auto* cells = grid_data_ + std::uint64_t{kGridBytes} * static_cast<unsigned>(&s - slots_);
    const video::LumaGrid grid{cells, s.grid_width, s.grid_height, s.grid_words * 4};
    std::uint64_t sum = 0;
    for (unsigned y = 0; y < s.grid_height; ++y) for (unsigned x = 0; x < s.grid_width; ++x) sum += cells[y * grid.stride + x];
    const float mean = static_cast<float>(static_cast<double>(sum) / (std::max)(1u, s.grid_width * s.grid_height) / 255.0);
    // A cut for the tracker at once (the live detector confirms one four pictures later): the mean jumps as CutDetector's does.
    const bool cut = grid_mean_ >= 0 && std::fabs(mean - grid_mean_) > timeline::CutDetector::kCutLuma;
    grid_mean_ = mean;
    // Perception on the same grid (2026-10-31): the objects NextTarget moves between. A new epoch or a cut starts it over. One
    // grid every kWarmInterval, tracking or not (2026-11-01): a slow object's change from picture to picture at 24 or more a
    // second is under saliency's floor where over 100 ms it is not.
    // On perception's thread (2026-11-01): the grid copied (a row at a time, tight) and handed over when the thread is idle; a
    // cut or a new epoch starts it over with the next grid it takes.
    if (cut || targets_epoch_ != epoch_) { reset_pending_ = true; targets_epoch_ = epoch_; perceived_pts_ = INT64_MIN; }
    if (cut && Tracking()) { ++locks_; vibro_path_.Reset(); } // Vibrometry's window starts over at a cut.
    // The event camera (2026-11-03) takes every grid on the same thread; one the thread is still busy for is skipped.
    const bool idle = perceive_cells_ && perceive_state_.load(std::memory_order_acquire) == 0;
    const bool perceive = idle && targets_ready_ && s.grid_width == targets_.Width() && s.grid_height == targets_.Height() &&
                          (perceived_pts_ == INT64_MIN || s.frame.pts_100ns < perceived_pts_ || s.frame.pts_100ns - perceived_pts_ >= kWarmInterval);
    const bool sense = events_on_ && events_.Configured();
    if (sense && !idle) events_skipped_.fetch_add(1, std::memory_order_relaxed);
    if (sense && s.steadied) { sense_x_ += s.motion_x; sense_y_ += s.motion_y; sense_turn_ += s.motion_turn; } // Up to the next grid it takes.
    if (perceive || (sense && idle)) {
        for (unsigned y = 0; y < s.grid_height; ++y) std::memcpy(perceive_cells_ + std::size_t{y} * s.grid_width, cells + std::size_t{y} * grid.stride, s.grid_width);
        perceive_width_ = s.grid_width; perceive_height_ = s.grid_height; perceive_pts_ = s.frame.pts_100ns;
        perceive_run_ = perceive; sense_run_ = sense; sense_epoch_ = epoch_;
        sense_restart_ = sense && events_restart_; sense_options_ = events_options_; if (sense) events_restart_ = false;
        sense_measured_ = s.steadied && events_.Options().stabilized; sense_given_x_ = sense_x_; sense_given_y_ = sense_y_; sense_given_turn_ = sense_turn_;
        if (sense) sense_x_ = sense_y_ = sense_turn_ = 0;
        if (perceive) {
            perceive_reset_ = reset_pending_; reset_pending_ = false;
            perceived_pts_ = s.frame.pts_100ns;
            if (!Tracking()) ++warm_grids_;
        }
        perceive_state_.store(1, std::memory_order_release); (void)SetEvent(perceive_wake_);
    }
    if (!Tracking()) return; // A warm grid (2026-11-01): perception only.
    if (track_pending_) {
        track_pending_ = false; ++track_events_;
        const bool retarget = retarget_; retarget_ = false;
        if (!tracker_.Lock(grid, retarget ? retarget_x_ : zoom_x_, retarget ? retarget_y_ : zoom_y_)) {
            track_lock_failed_ = true; target_id_ = 0;
            if (track_zoomed_) SetZoom(1.0f, 0.5f, 0.5f, false); // Nothing to follow: the zoom T put on goes again.
            track_zoomed_ = false;
            return;
        }
        if (zoom_ <= 1.0f) SetZoom(2.0f, zoom_x_, zoom_y_, false); // Tracking frames the target: closer.
        estimate_pts_ = s.frame.pts_100ns; estimate_duration_ = s.frame.duration_100ns;
        ++locks_; vibro_path_.Reset();
        const auto& locked = tracker_.Status();
        vibro_path_.Push(s.frame.pts_100ns, locked.x * static_cast<double>(s.crop.right - s.crop.left), locked.y * static_cast<double>(s.crop.bottom - s.crop.top));
        return;
    }
    const auto was = tracker_.Status().state;
    const auto st = tracker_.Update(grid, cut);
    tracked_pts_ = estimate_pts_ = s.frame.pts_100ns; estimate_duration_ = s.frame.duration_100ns;
    if (st.state != was || st.reacquired) ++track_events_;
    if (st.reacquired) { ++locks_; vibro_path_.Reset(); }
    if (st.state == video::TrackState::Tracking) {
        Follow(st.x, st.y); FollowObject();
        vibro_path_.Push(s.frame.pts_100ns, st.x * static_cast<double>(s.crop.right - s.crop.left), st.y * static_cast<double>(s.crop.bottom - s.crop.top));
    } else vibro_path_.Reset();
}
void D3D12Renderer::HarvestTimings(std::int64_t now, std::uint64_t completed) noexcept {
    HarvestScenes(completed);
    HarvestGrid(completed);
    HarvestVibro(completed);
    if (!timestamp_data_ || completed == UINT64_MAX) return;
    for (unsigned i = 0; i != Count; ++i) {
        auto& s = slots_[i];
        if (!s.timed || completed < s.fence) continue;
        s.timed = false;
        const auto begin = timestamp_data_[2 * i], end = timestamp_data_[2 * i + 1];
        if (end <= begin || end - begin > timestamp_frequency_ * 10) continue; // Not a measurement (a clock reset).
        const auto gpu = static_cast<std::int64_t>((end - begin) * 10000000 / timestamp_frequency_);
        last_gpu_ = gpu;
        if (s.trace >= 0) { trace_conversions_[s.trace].gpu = gpu; trace_conversions_[s.trace].harvested = now; s.trace = -1; }
        if (s.tier == QualityTier::Neural) { neural_gpu_.sum += gpu; ++neural_gpu_.count; }
        if (test_load_ <= 0) load_repeats_ = 0;
        else if (s.tier != QualityTier::Fast) {
            // Test load: as many repeats as make the extra work about the time asked for (one conversion's time each).
            const auto single = gpu / (s.repeats + 1);
            const auto wanted = static_cast<std::int64_t>(static_cast<double>(test_load_) * 10000.0);
            load_repeats_ = single > 0 ? static_cast<unsigned>((std::min)((wanted + single - 1) / single, std::int64_t{4000})) : load_repeats_;
        }
        // The queue it was measured against (2026-11-03: the governor's thin-queue rule): the pictures converted and in order.
        const auto queued = static_cast<std::int64_t>(order_.Size()) * s.budget;
        if (governor_.Observe(s.tier, gpu, s.workload, s.budget, now, queued)) QualityCue(false);
    }
}
void D3D12Renderer::SetQualityMode(QualityMode mode) noexcept { governor_.SetMode(mode, InputTime()); SettingsChanged(); }
void D3D12Renderer::SettingsChanged() noexcept {
    if (!hud_paused_ || !window_) return; // Playing: the next converted pictures show it.
    settings_asked_ = InputTime();
    if (refresh_in_flight_) { reconvert_after_refresh_ = true; return; } // The landing may have been converted already.
    if (held_valid_ && held_frame_.playback_epoch == epoch_ && held_frame_.pts_100ns == shown_pts_) reconvert_pending_ = true;
    else refresh_wanted_ = true;
}
D3D12Renderer::Region D3D12Renderer::RegionOf(const core::VideoFrameDescriptor& frame) const noexcept {
    return RegionOf(Region{frame.crop_left, frame.crop_top, frame.crop_right, frame.crop_bottom});
}
D3D12Renderer::Region D3D12Renderer::RegionOf(const Region& crop) const noexcept { return RegionAt(crop, zoom_x_, zoom_y_); }
D3D12Renderer::Region D3D12Renderer::RegionAt(const Region& crop, float x_at, float y_at) const noexcept {
    const unsigned w = crop.right - crop.left, h = crop.bottom - crop.top;
    const float zoom = ViewZoom();
    if (zoom <= 1.0f || w < 32 || h < 32) return crop;
    // The crop's shape at 1/zoom, on even samples, at least 16 of them, its centre where asked, inside the crop.
    const auto even = [](double v) { return static_cast<unsigned>(std::lround(v / 2)) * 2u; };
    const unsigned rw = (std::clamp)(even(w / zoom), 16u, w & ~1u), rh = (std::clamp)(even(h / zoom), 16u, h & ~1u);
    const double cx = static_cast<double>(x_at) * w, cy = static_cast<double>(y_at) * h;
    const unsigned x = even((std::clamp)(cx - rw / 2.0, 0.0, static_cast<double>(w - rw))), y = even((std::clamp)(cy - rh / 2.0, 0.0, static_cast<double>(h - rh)));
    const unsigned left = crop.left + (std::min)(x, (w - rw) & ~1u), top = crop.top + (std::min)(y, (h - rh) & ~1u);
    return {left, top, left + rw, top + rh};
}
void D3D12Renderer::SetZoom(float zoom, float x, float y, bool cue) noexcept {
    if (cue && auto_crop_) { auto_crop_ = false; cropped_changes_ = 0; } // The viewer's own zoom: auto-crop stops.
    zoom = (std::clamp)(zoom, 1.0f, kMaxZoom);
    // The centre stays where a region of this size fits.
    const float half = 0.5f / zoom;
    x = (std::clamp)(x, half, 1.0f - half); y = (std::clamp)(y, half, 1.0f - half);
    if (zoom == 1.0f) x = y = 0.5f;
    const bool changed = zoom != zoom_ || x != zoom_x_ || y != zoom_y_;
    zoom_ = zoom; zoom_x_ = x; zoom_y_ = y;
    if (changed) {
        // Live zoom: what is already converted shows the change through the blit from the next composite (redraw_), and
        // what is converted from now on is reconstructed from the new region.
        ++zoom_generation_;
        if (!hud_paused_ && window_) {
            auto& z = live_zooms_[live_zoom_count_ < kMaxLiveZooms ? live_zoom_count_ : kMaxLiveZooms - 1]; ++live_zoom_count_;
            z = {}; z.generation = zoom_generation_; z.requested = InputTime(); z.region = RegionOf(history_crop_);
            zoom_redraw_ = true; // Shown by the next composite, not after the controls' refresh interval.
        }
        SettingsChanged(); redraw_ = true;
    }
    if (cue) ZoomCue();
}
void D3D12Renderer::ZoomBy(float factor, float x, float y, bool cue) noexcept {
    // The point at fraction (x, y) of the view shows the same sample before and after. (x, y) are of the picture as shown:
    // a turned picture's are brought back to the crop's axes (D's inverse, its transpose).
    orientation_.ToStored(x, y);
    if (auto_crop_) { auto_crop_ = false; cropped_changes_ = 0; } // The viewer's own zoom: auto-crop stops.
    const float before = zoom_, after = (std::clamp)(zoom_ * factor, 1.0f, kMaxZoom);
    const float sx = zoom_x_ + (x - 0.5f) / before, sy = zoom_y_ + (y - 0.5f) / before; // The sample under the point (crop fractions).
    SetZoom(after, sx - (x - 0.5f) / after, sy - (y - 0.5f) / after, cue);
}
void D3D12Renderer::Pan(float dx, float dy) noexcept {
    if (zoom_ <= 1.0f) return;
    orientation_.DirectionToStored(dx, dy); // As shown -> the crop's axes.
    SetZoom(zoom_, zoom_x_ + dx / zoom_, zoom_y_ + dy / zoom_, false);
}
void D3D12Renderer::ZoomCue() noexcept {
    // "Zoom · 2.5×", "Zoom off".
    if (zoom_ <= 1.0f) { ShowStatus("Zoom off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return; }
    char text[32]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
    put("Zoom \xc2\xb7 ");
    const auto tenths = static_cast<unsigned>(std::lround(zoom_ * 10));
    text[n++] = static_cast<char>('0' + tenths / 10);
    if (tenths % 10) { text[n++] = '.'; text[n++] = static_cast<char>('0' + tenths % 10); }
    put("\xc3\x97"); text[n] = 0;
    ShowStatus(text, ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
void D3D12Renderer::SetComparison(bool on, bool cue) noexcept { SetCompare(on ? Compare::Enhanced : Compare::Off, cue); }
void D3D12Renderer::SetCompare(Compare mode, bool cue) noexcept {
    if (mode == Compare::Pinned && !reference_valid_) mode = Compare::Off;
    const bool enhanced = mode == Compare::Enhanced;
    compare_mode_ = mode; redraw_ = true;
    if (enhanced != compare_) { compare_ = enhanced; SettingsChanged(); } // Original | Enhanced is made by the conversion.
    if (!cue) return;
    if (mode == Compare::Pinned) {
        // "Comparison · Pinned 0:12.4 | Live".
        char text[64]; unsigned n = 0;
        const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; text[n] = 0; };
        const auto digits = [&](std::int64_t v, unsigned width) { char d[20]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 19);
                                                                    while (k < width) d[k++] = '0'; while (k && n + 1 < sizeof(text)) text[n++] = d[--k]; text[n] = 0; };
        const std::int64_t tenths = reference_pts_ > 0 ? (reference_pts_ + 500000) / 1000000 : 0, seconds = tenths / 10;
        put("Comparison \xc2\xb7 Pinned "); digits(seconds / 60, 1); put(":"); digits(seconds % 60, 2); put("."); digits(tenths % 10, 1); put(" | Live");
        ShowStatus({text, n}, ui::StatusIcon::Device, ui::StatusTone::Info);
    } else ShowStatus(enhanced ? std::string_view{"Comparison \xc2\xb7 Original | Enhanced"} : std::string_view{"Comparison off"}, ui::StatusIcon::Device, ui::StatusTone::Info);
}
void D3D12Renderer::CycleCompare() noexcept {
    SetCompare(compare_mode_ == Compare::Off ? Compare::Enhanced : compare_mode_ == Compare::Enhanced && reference_valid_ ? Compare::Pinned : Compare::Off);
}
bool D3D12Renderer::PinReference() noexcept {
    // The history is the picture on screen; copied after every list already queued (one queue, in order).
    if (!history_valid_ || !reference_ || !pin_commands_) return false;
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX || completed < pin_fence_) return false;
    if (FAILED(error_ = pin_allocator_->Reset()) || FAILED(error_ = pin_commands_->Reset(pin_allocator_, nullptr))) return false;
    D3D12_RESOURCE_BARRIER b[]{Barrier(history_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                               Barrier(reference_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    pin_commands_->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION to{reference_, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{history_, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{0, 0, 0, history_width_, history_height_, 1};
    pin_commands_->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    pin_commands_->ResourceBarrier(2, b);
    if (FAILED(error_ = pin_commands_->Close())) return false;
    ID3D12CommandList* lists[]{pin_commands_}; queue_->ExecuteCommandLists(1, lists);
    pin_fence_ = ++sequence_; if (FAILED(error_ = queue_->Signal(fence_, pin_fence_))) return false;
    reference_width_ = history_width_; reference_height_ = history_height_; reference_pts_ = shown_pts_ != INT64_MIN ? shown_pts_ : hud_media_;
    reference_valid_ = true; redraw_ = true;
    return true;
}
void D3D12Renderer::ArmLoopPins(std::int64_t a, std::int64_t b) noexcept {
    if (pin_armed_ && pin_a_ == a && pin_b_ == b) return; // The same loop: what is pinned stays.
    pin_armed_ = true; pin_a_ = a; pin_b_ = b; pin_count_ = 0; pin_complete_ = false;
    ReleaseStagedLanding();
}
void D3D12Renderer::ClearLoopPins() noexcept { pin_armed_ = pin_complete_ = false; pin_count_ = 0; ReleaseStagedLanding(); }
std::int64_t D3D12Renderer::LoopPinEnd() const noexcept {
    if (!LoopPinsReady()) return INT64_MIN;
    const auto& last = pins_[pin_count_ - 1].frame;
    return last.pts_100ns + (last.duration_100ns > 0 ? last.duration_100ns : 1);
}
void D3D12Renderer::PinPicture(const Slot& s, ID3D12GraphicsCommandList* commands) noexcept {
    // The loop's pins: the picture holding A starts them; each next one must follow the last pinned one exactly (same epoch,
    // its start at the last one's end), else the run starts over at the next A. Copied in this composite's list.
    if (!pin_armed_ || pin_complete_) return;
    const auto& f = s.frame;
    const std::int64_t duration = f.duration_100ns > 0 ? f.duration_100ns : 1, end = f.pts_100ns + duration;
    const bool first = f.pts_100ns <= pin_a_ && pin_a_ < end;
    if (first) { pin_count_ = 0; pin_epoch_ = f.playback_epoch; }
    else if (!pin_count_) return;
    else {
        const auto& last = pins_[pin_count_ - 1].frame;
        const auto last_end = last.pts_100ns + (last.duration_100ns > 0 ? last.duration_100ns : 1);
        if (f.playback_epoch != pin_epoch_ || f.pts_100ns < last_end - 2 || f.pts_100ns > last_end + 2) { pin_count_ = 0; return; }
    }
    if (pin_count_ && (end - pin_a_ > kPinSpan || end > pin_b_ - kPinMargin)) { pin_complete_ = true; return; } // Enough: the rest decodes in time.
    auto& pin = pins_[pin_count_];
    D3D12_RESOURCE_BARRIER b[]{Barrier(s.rgb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                               Barrier(pin_rgb_[pin_count_], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    commands->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION to{pin_rgb_[pin_count_], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{s.rgb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{0, 0, 0, s.width, s.height, 1};
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    commands->ResourceBarrier(2, b);
    pin = {f, s.width, s.height, s.crop, s.region, s.zoom_generation};
    if (++pin_count_ == kPins) pin_complete_ = true;
}
void D3D12Renderer::ReleaseStagedLanding() noexcept {
    // A staged landing no turn took (another seek, the loop changed or cleared): its slot back to the pool (a conversion
    // into it waits for its copy's fence).
    if (landing_slot_ < Count && !(swap_pending_ && staged_slot_ == landing_slot_)) slots_[landing_slot_].busy = false;
    landing_slot_ = Count;
}
bool D3D12Renderer::StageLoopLanding() noexcept {
    // The pinned turn's landing ahead of B (2026-11-02): A's first pinned picture copied on the copy queue into a slot held
    // out of the queue, so that the turn, when it comes, has no copy to wait for. Measured before: a pinned turn waited 5-7 ms
    // for its landing's copy about once a full suite (the copy queued behind the GPU's other work), against 1-4 ms usually.
    if (landing_slot_ < Count) {
        if (landing_epoch_ == epoch_ && pin_complete_ && landing_pts_ == pins_[0].frame.pts_100ns) return true;
        ReleaseStagedLanding(); // Stale: another timeline or other pins.
    }
    if (!LoopPinsReady() || !copy_queue_) return false;
    const auto completed = fence_->GetCompletedValue(), copied = copy_fence_->GetCompletedValue();
    if (completed == UINT64_MAX || copied < turn_fence_[0]) return false; // Its list's last copies still run.
    unsigned index = Count;
    for (unsigned i = 0; i != Count && index == Count; ++i)
        if (!slots_[i].busy && !(swap_pending_ && staged_slot_ == i) && completed >= slots_[i].fence && (!slots_[i].copy_fence || copied >= slots_[i].copy_fence)) index = i;
    if (index == Count) return false;
    auto* allocator = turn_allocators_[0]; auto* list = turn_commands_[0];
    if (FAILED(error_ = allocator->Reset()) || FAILED(error_ = list->Reset(allocator, nullptr))) return false;
    auto& s = slots_[index]; const auto& pin = pins_[0];
    D3D12_RESOURCE_BARRIER b[]{Barrier(pin_rgb_[0], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE), Barrier(s.rgb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION to{s.rgb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{pin_rgb_[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    const D3D12_BOX box{0, 0, 0, pin.width, pin.height, 1};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    list->ResourceBarrier(2, b);
    if (FAILED(error_ = list->Close())) return false;
    ID3D12CommandList* lists[]{list}; copy_queue_->ExecuteCommandLists(1, lists);
    ++copy_sequence_; if (FAILED(error_ = copy_queue_->Signal(copy_fence_, copy_sequence_))) return false;
    turn_fence_[0] = copy_sequence_;
    // Held (busy) out of the presentation queue, of no epoch (a queue seek's adoption leaves it alone).
    s.busy = true; s.staged = false; s.copy_fence = copy_sequence_; s.frame.playback_epoch = 0;
    landing_slot_ = index; landing_epoch_ = epoch_; landing_pts_ = pin.frame.pts_100ns; ++staged_landings_;
    return true;
}
bool D3D12Renderer::BeginPinnedEpoch(std::uint32_t epoch, std::int64_t now, std::int64_t target, std::int64_t& end) noexcept {
    if (!LoopPinsReady() || epoch <= epoch_ || !copy_queue_) return false;
    const auto completed = fence_->GetCompletedValue(), copied = copy_fence_->GetCompletedValue();
    // The landing staged ahead (StageLoopLanding), when it is A's first pin on this timeline: its copy has run already.
    const unsigned staged = landing_slot_ < Count && landing_epoch_ == epoch_ && landing_pts_ == pins_[0].frame.pts_100ns ? landing_slot_ : Count;
    // The lists this turn records: their last copies must have run (the staged landing's list is not recorded again).
    if (completed == UINT64_MAX || copied < turn_fence_[1] || (staged == Count && copied < turn_fence_[0])) return false;
    // Slots the flush frees whose last direct-queue work is done (the copy queue does not wait for the direct one): as many
    // pins as there are such slots, from A on; the staged landing first.
    unsigned free_slots[kPins]{}, found = 0;
    if (staged < Count) free_slots[found++] = staged;
    for (unsigned i = 0; i != Count && found < pin_count_; ++i)
        if (i != staged && !(swap_pending_ && staged_slot_ == i) && completed >= slots_[i].fence && (!slots_[i].copy_fence || copied >= slots_[i].copy_fence))
            free_slots[found++] = i;
    if (!found) return false;
    const unsigned queued = found;
    const std::uint64_t staged_fence = staged < Count ? slots_[staged].copy_fence : 0;
    (void)BeginEpoch(epoch, now, target, false, 0); // Frees every busy slot, the staged one too (taken back below).
    // The landing's copy goes first, on its own (its fence is what the screen waits for); the others follow in a second list.
    std::uint64_t landing_fence = staged_fence;
    for (unsigned part = staged < Count ? 1u : 0u; part < 2; ++part) {
        auto* allocator = turn_allocators_[part]; auto* list = turn_commands_[part];
        const unsigned first = part ? 1 : 0, last = part ? queued : 1;
        if (first >= last) break;
        if (FAILED(error_ = allocator->Reset()) || FAILED(error_ = list->Reset(allocator, nullptr))) return false;
        for (unsigned k = first; k < last; ++k) {
            auto& s = slots_[free_slots[k]]; const auto& pin = pins_[k];
            D3D12_RESOURCE_BARRIER b[]{Barrier(pin_rgb_[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       Barrier(s.rgb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(2, b);
            D3D12_TEXTURE_COPY_LOCATION to{s.rgb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{pin_rgb_[k], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            const D3D12_BOX box{0, 0, 0, pin.width, pin.height, 1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            list->ResourceBarrier(2, b);
        }
        if (FAILED(error_ = list->Close())) return false;
        ID3D12CommandList* lists[]{list}; copy_queue_->ExecuteCommandLists(1, lists);
        ++copy_sequence_; if (FAILED(error_ = copy_queue_->Signal(copy_fence_, copy_sequence_))) return false;
        turn_fence_[part] = copy_sequence_;
        if (!part) landing_fence = copy_sequence_;
    }
    if (staged < Count) ++staged_turns_;
    // Into the new epoch's queue, converted already: they leave by the clock like any picture (the first one as the landing).
    for (unsigned k = 0; k < queued; ++k) {
        auto& s = slots_[free_slots[k]]; const auto& pin = pins_[k];
        s.frame = pin.frame; s.frame.playback_epoch = epoch_; s.width = pin.width; s.height = pin.height;
        s.crop = pin.crop; s.region = s.view = pin.region; s.zoom_generation = pin.zoom_generation; s.tracked = false;
        s.fence = 0; s.copy_fence = k ? copy_sequence_ : landing_fence; s.busy = true; s.staged = false; s.timed = false; s.stats = false;
        const auto admitted = order_.Insert(s.frame.pts_100ns, free_slots[k]);
        if (admitted != video::ReorderInsert::Inserted) {
            s.busy = false; ++out_of_order_;
            last_reject_ = {s.frame.pts_100ns, order_.LastReleased(), s.frame.playback_epoch, epoch_, static_cast<std::uint8_t>(admitted), true};
        }
    }
    const auto& last = pins_[queued - 1].frame;
    end = last.pts_100ns + (last.duration_100ns > 0 ? last.duration_100ns : 1);
    ++pinned_turns_;
    return true;
}
void D3D12Renderer::QualityCue(bool manual) noexcept {
    // "Quality · Fast bilinear (manual)", "Quality · Auto"; the governor's own changes: "Auto quality · Edge-directed ·
    // GPU 38.2 ms" (down: the time that did not fit) or "Auto quality · Neural super-resolution" (up). No formatting
    // library: the text is assembled in place.
    char text[ui::StatusCue::kMaxBytes]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
    if (manual) {
        put("Quality \xc2\xb7 "); put(Name(governor_.Mode()));
        if (governor_.Mode() != QualityMode::Auto) put(" (manual)");
    } else {
        const auto& change = governor_.LastChange();
        put("Auto quality \xc2\xb7 "); put(Name(change.to));
        if (change.reason == QualityGovernor::Reason::Overload) {
            const auto tenths = (change.p95 + 500) / 1000; // 100 ns -> 0.1 ms.
            char digits[24]; unsigned d = 0; auto whole = tenths / 10;
            do { digits[d++] = static_cast<char>('0' + whole % 10); whole /= 10; } while (whole && d < 20);
            put(" \xc2\xb7 GPU ");
            while (d && n + 1 < sizeof(text)) text[n++] = digits[--d];
            const char fraction[]{'.', static_cast<char>('0' + tenths % 10), ' ', 'm', 's', 0}; put(fraction);
        }
    }
    ShowStatus({text, n}, ui::StatusIcon::Device, ui::StatusTone::Info);
}
RenderStatus D3D12Renderer::Retire(Slot& s, bool presented) noexcept {
    // A frame staged before a seek flush is no longer in the window; only pop the entry that names it.
    const auto index = static_cast<std::uint32_t>(&s - slots_);
    if (order_.Front() && order_.Front()->slot == index) (void)order_.Pop();
    s.busy = false; s.staged = false;
    if (presented) ++presented_; else ++dropped_;
    return RenderStatus::Ready;
}
bool D3D12Renderer::BeginEpoch(std::uint32_t epoch, std::int64_t now, std::int64_t target, bool adopt, std::uint32_t chain) noexcept {
    if (epoch <= epoch_) return false;
    ReleaseStagedLanding();
    if (Tracking()) { tracker_.Stop(); track_pending_ = false; ++track_events_; } // Another picture: no target to follow in it.
    bool kept = false;
    if (adopt && target != INT64_MIN) {
        // Queue seek: pictures in presentation order that end at or before the target leave; the rest stay, re-tagged.
        while (const auto* front = order_.Front()) {
            const auto index = front->slot; auto& s = slots_[index];
            if (s.frame.pts_100ns + s.frame.duration_100ns > target) break;
            (void)order_.Pop(); ++stale_;
            if (!(swap_pending_ && staged_slot_ == index)) s.busy = false;
        }
        kept = order_.Size() != 0;
        for (unsigned i = 0; i != Count; ++i)
            if (slots_[i].busy && slots_[i].frame.playback_epoch == epoch_ && !(swap_pending_ && staged_slot_ == i)) { slots_[i].frame.playback_epoch = epoch; ++adopted_frames_; }
        // Earlier queue seeks of the same chain stay adopted: their pictures may still be on the way.
        if (adoption_ != Adoption::Confirmed) adopt_chain_ = chain;
        adopt_floor_ = target; adoption_ = kept ? Adoption::Confirmed : Adoption::Pending;
    } else {
        // Queued RGB slots become reusable at once: Convert still waits for each slot's GPU fence before reuse.
        for (unsigned i = 0; i != Count; ++i) {
            if (!slots_[i].busy || (swap_pending_ && staged_slot_ == i)) continue;
            slots_[i].busy = false; ++stale_;
        }
        order_.Clear(); adoption_ = Adoption::None;
        peak_timing_.Restart(); // The new timeline's first picture takes its own peak.
        cut_detector_.Reset();  // And it has no predecessor to be cut from.
    }
    neural_pass_.Invalidate(); // Temporal tile reuse starts over with the epoch's first picture (docs/neural.md).
    epoch_ = epoch; seek_requested_ = now; seek_target_ = target; preview_ = true;
    return kept;
}
void D3D12Renderer::ResolveAdoption(bool adopted) noexcept {
    if (adoption_ == Adoption::None) return;
    if (adopted) { adoption_ = Adoption::Confirmed; return; }
    // Declined: the worker restarted the timeline, so whatever was kept would come again. It leaves like a seek's flush
    // (a kept landing was never declined in the tests: the worker keeps the chain whenever the landing was converted).
    for (unsigned i = 0; i != Count; ++i) {
        if (!slots_[i].busy || (swap_pending_ && staged_slot_ == i)) continue;
        slots_[i].busy = false; ++stale_;
    }
    order_.Clear(); adoption_ = Adoption::None; peak_timing_.Restart(); cut_detector_.Reset();
}
bool D3D12Renderer::LandingReady() const noexcept {
    // A seek's landing picture (its interval contains the target) is final as soon as it is in the window: every other
    // picture the epoch shows ends after the target, so it starts at or after the landing's end (presentation intervals do
    // not overlap) and can only follow it. It need not wait for `depth` successors (docs/seeking.md, "Landing release").
    if (!preview_ || seek_target_ == INT64_MIN) return false;
    const auto* front = order_.Front();
    if (!front) return false;
    const auto& f = slots_[front->slot].frame;
    return f.playback_epoch == epoch_ && f.pts_100ns <= seek_target_ && seek_target_ - f.pts_100ns < f.duration_100ns;
}
std::int64_t D3D12Renderer::MediaTime(const core::VideoFrameDescriptor& frame) const noexcept {
    const bool landing = preview_ && frame.playback_epoch == epoch_ && seek_target_ > frame.pts_100ns &&
                         seek_target_ - frame.pts_100ns < frame.duration_100ns;
    return landing ? seek_target_ : frame.pts_100ns;
}
RenderStatus D3D12Renderer::Present(const clock::PredictiveMasterClock& clock, std::int64_t now, const PresentState& state) noexcept {
    // Health probe: a device removed while nothing is drawn (paused, idle controls) is still noticed within 250 ms.
    if (now - health_at_ >= 2500000) { health_at_ = now; if (Lost()) return RenderStatus::DeviceLost; }
    const auto status = PresentFrame(clock, now, state);
    return status == RenderStatus::Failed && Lost() ? RenderStatus::DeviceLost : status;
}
RenderStatus D3D12Renderer::PresentFrame(const clock::PredictiveMasterClock& clock, std::int64_t now, const PresentState& state) noexcept {
    const bool pausing = state.paused && !present_paused_; // SetHudPaused has told hud_paused_ already.
    present_paused_ = hud_paused_ = state.paused;
    wake_ = {now, true}; settled_ = false; // Every return before the end: come back at once.
    ApplyStabilization();
    HarvestTimings(now, fence_->GetCompletedValue());
    // A host that exports tensors: the picture on screen is held from the start of a pause, so its next export decodes nothing.
    if (pausing && tensor_used_.load(std::memory_order_relaxed) && shown_pts_ != INT64_MIN && !preview_ && !refresh_in_flight_ &&
        !(held_valid_ && held_frame_.playback_epoch == epoch_ && held_frame_.pts_100ns == shown_pts_)) { settings_asked_ = InputTime(); refresh_wanted_ = true; }
    if (awaiting_update_ < kMaxPausedUpdates && fence_->GetCompletedValue() >= awaiting_fence_) {
        paused_updates_[awaiting_update_].completed = InputTime() - settings_asked_; awaiting_update_ = kMaxPausedUpdates;
    }
    if (ready_epoch_ != epoch_) {
        const auto completed = fence_->GetCompletedValue();
        for (const auto& s : slots_)
            if (s.busy && s.frame.playback_epoch == epoch_ && completed != UINT64_MAX && SlotReady(s, completed)) { ready_epoch_ = epoch_; break; }
    }
    if (!hud_advance_ || now - hud_advance_ >= 83333) {
        clock::ClockEstimate estimate{};
        // A clock still anchored to a timeline that a seek has left does not describe the screen: the controls and
        // subtitles hold the last presented position until the new epoch's first frame arrives.
        if (!state.paused && clock.Estimate(now, estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch_)
            hud_media_ = estimate.media_time_100ns;
        HudAction(hud_.Advance(now, state.paused, hud_media_, presented_)); hud_advance_ = now;
        // Command bar: its motion, what Enter would do with the text as typed so far, and the text's raster.
        palette_.Advance(now);
        if (palette_.IsOpen() && palette_.Revision() != previewed_) {
            previewed_ = palette_.Revision(); unsigned matches = 0;
            const auto hint = palette_preview_ && !palette_.Text().empty() ? palette_preview_(palette_context_, palette_.Text(), matches) : ui::PaletteHint::Empty;
            palette_.SetHint(hint, matches);
        }
        palette_pipeline_.Update(palette_);
        status_pipeline_.Update(status_, now); // A finished raster lets its cue appear.
        // Subtitle glyphs for the cues shown now and in the next seconds are outlined ahead of their composites. While
        // a seek is landing, that is the text at its target: decoding up to the target frame takes longer than the
        // outlines, so the landing image has its caption even when the seek jumped to text that was not resident.
        if (const auto* next = pending_rebind_.load(std::memory_order_acquire); next && subtitle_track_ && subtitles_.Rebind(next)) {
            // Subtitles after open (API 17): from this pass nothing of the old track is drawn or counted.
            subtitle_track_ = next; shown_subtitles_ = {}; staged_subtitles_ = {}; std::memset(cue_seen_, 0, sizeof(cue_seen_));
            pending_rebind_.store(nullptr, std::memory_order_release);
        }
        if (subtitle_track_) subtitles_.Update(preview_ ? seek_target_ : hud_media_);
    }
    if (swap_pending_) return FinishPresent(now); // Retry exactly the staged image, even across a pause.
    if (resize_pending_) { const auto resized = ApplyResize(); if (resized != RenderStatus::Ready) return resized; }
    if (state.paused && reconvert_pending_ && held_valid_ && !preview_) { if (Reconvert() == RenderStatus::Failed) return RenderStatus::Failed; }
    if (state.paused && HeldTensor() == RenderStatus::Failed) return RenderStatus::Failed;
    if (state.paused && capture_readback_ && (capture_requested_ || reference_capture_requested_ || capture_pending_) && !reconvert_pending_ && !reconverted_) {
        if (CaptureShown() == RenderStatus::Failed) return RenderStatus::Failed;
    } else if (!state.paused && capture_pending_ && capture_kind_ == CaptureKind::Screen) {
        // A screen capture taken while playing (RequestScreenCapture) is read once its composite has run.
        if (CaptureShown() == RenderStatus::Failed) return RenderStatus::Failed;
    }
    std::int64_t until_picture = INT64_MAX; // To the next picture's deadline, when the clock gives it one.
    bool untimed = false; // A picture in order that the clock gives no deadline yet (the start, a landing's re-anchor).
    if ((!state.paused || preview_ || recovery_preview_) && (order_.Ready(state.draining) || LandingReady())) {
        const auto slot = order_.Front()->slot; auto& s = slots_[slot];
        // Seek preview: the first frame of the new timeline is shown as soon as it is in order. After a device
        // loss, the first frame decoded on the new device likewise (the screen's picture went with the device).
        if (preview_ || recovery_preview_) { staged_deadline_ = 0; return Composite(slot, now); }
        std::int64_t deadline = 0; std::uint32_t epoch = 0;
        const auto timing = clock.Deadline(s.frame.pts_100ns, now, deadline, epoch);
        if (timing == clock::ClockQuery::OutOfRange) {
            clock::ClockEstimate estimate{};
            if (clock.Estimate(now, estimate) == clock::ClockQuery::Ready && estimate.epoch == s.frame.playback_epoch &&
                s.frame.pts_100ns < estimate.media_time_100ns) {
                Trace(s.frame.pts_100ns, 0, now, true);
                last_drop_ = {s.frame.pts_100ns, estimate.media_time_100ns - s.frame.pts_100ns, true, waited_}; waited_ = 0;
                return Retire(s, false);
            }
        }
        // A clock still anchored to an earlier timeline is not a deadline for this frame: hold it.
        if (timing == clock::ClockQuery::Ready && epoch == s.frame.playback_epoch) {
            const auto interval = s.frame.duration_100ns > 0 ? s.frame.duration_100ns : 0;
            if (now - deadline > interval) { Trace(s.frame.pts_100ns, deadline, now, true); last_drop_ = {s.frame.pts_100ns, now - deadline, false, waited_}; waited_ = 0; return Retire(s, false); }
            if (now >= deadline) { staged_deadline_ = deadline; awaiting_clock_ = false; return Composite(slot, now); } // Due video always has priority over HUD-only refresh.
            until_picture = deadline - now;
        } else untimed = true;
    }
    // Recovered device: until its first video frame (or 1 s), a controls-only image would be black under the HUD.
    if (recovery_preview_ && now - recovery_requested_ < 10000000) return RenderStatus::WouldBlock;
    SubtitleFrame selected{};
    const auto stamp = SelectSubtitles(hud_media_, selected);
    const bool palette = palette_.Animating() || palette_was_drawn_ || palette_pipeline_.Dirty();
    status_.Advance(now);
    const bool status = status_.Animating() || status_was_drawn_ || status_pipeline_.Dirty();
    // A status cue fading in or out (no picture due: paused, or between pictures) is refreshed at the display's rate, and
    // so is the sound in music mode while it plays; a pause dims it once. Music mode paces itself by the refresh period
    // and presents like pictures (sync interval 0): a vsync-paced present is throttled by the compositor when nobody sees
    // the window (a locked session ran it at 6 a second), and uncapped it ran at 548.
    const bool live = music_ && !state.paused;
    if (music_ && state.paused != music_paused_) { music_paused_ = state.paused; redraw_ = true; }
    const std::int64_t interval = live ? refresh_period_ - 5000 : status_.Fading() ? refresh_period_ : 83333;
    // A controls-only image (dock, captions, command bar over the held picture) is not drawn when a picture is due within
    // one refresh of the display: that picture's own image carries the controls, and the display could not show both.
    // Queued anyway, the extra image takes a place in the swap chain's queue, and the picture's Present then waits for a
    // vertical blank: at 60 pictures a second on a 60 Hz display that made pictures up to a whole refresh late.
    // A paused re-conversion is shown at once: the user is looking at that picture.
    const bool controls = live || HudChanged() || redraw_ || stamp != subtitle_stamp_ || palette || status;
    // At the start and at a resume, a picture in order that the clock does not time yet is due within moments: the same rule.
    // A controls-only image drawn then (the start's status cue, the resume's undimmed controls) took the swap chain's place,
    // and the first pictures' Present waited for frame latency: dropped as late (4K60 AV1, 2026-11-05). Only until the first
    // timed picture: applied to a seek's landing too, it held back the drawing that the next conversions follow (a 4K HEVC
    // landing then waited for its conversion). Bounded: a clock that does not come within 250 ms leaves the controls be drawn.
    if (was_paused_ && !state.paused) awaiting_clock_ = true;
    was_paused_ = state.paused;
    if (untimed && !state.paused && awaiting_clock_) {
        if (!untimed_since_) untimed_since_ = now;
        if (now - untimed_since_ < 2500000) until_picture = 0;
    } else untimed_since_ = 0;
    if (controls && (now - hud_present_ >= interval || reconverted_ || zoom_redraw_) && until_picture >= refresh_period_) return Composite(Count, now);
    // Nothing to draw now. Back for the picture's deadline (precise), the controls' next refresh, or their next advance.
    if (untimed) return RenderStatus::WouldBlock;
    wake_ = {hud_advance_ + 83333, false, until_picture != INT64_MAX};
    // The controls' next refresh, unless a picture due within a refresh carries them (they wait for it, so does the thread).
    if (controls && until_picture >= refresh_period_) wake_.at = (std::min)(wake_.at, hud_present_ + interval);
    if (until_picture != INT64_MAX && now + until_picture <= wake_.at) wake_ = {now + until_picture, true, true};
    // Paused with nothing that moves (SetSettledWait): before 2026-11-03 the thread came back every 8.3 ms to advance controls
    // that were still (120 passes a second, 0.4 % of a core).
    if (state.paused && settled_wait_ > 83333 && !controls && until_picture == INT64_MAX && hud_.Settled() && !preview_ && !recovery_preview_ &&
        !reconvert_pending_ && !refresh_in_flight_ && !capture_pending_ && snapshot_state_ == SnapshotState::Idle &&
        tensor_state_.load(std::memory_order_relaxed) == 0 && fence_->GetCompletedValue() >= sequence_) {
        settled_ = true; wake_.at = hud_advance_ + settled_wait_;
    }
    return RenderStatus::WouldBlock;
}
bool D3D12Renderer::HudChanged() const noexcept {
    // The controls need an image when they appear or go, or when what they show moved: any uniform but the clock (which the
    // shader does not read) by more than 1e-4 (a tenth of a pixel on any dock), or a mark. Shown and still (paused, the
    // pointer resting), they are not drawn again: before 2026-10-22 they were, 120 times a second.
    const bool shown = hud_enabled_ && hud_.Animating();
    if (shown != hud_was_drawn_) return true;
    if (!shown) return false;
    float now[32], drawn[32];
    static_assert(sizeof(ui::HudUniforms) == sizeof(now));
    std::memcpy(now, &hud_.Uniforms(), sizeof(now)); std::memcpy(drawn, &drawn_hud_, sizeof(drawn));
    for (unsigned i = 0; i < 31; ++i) if (i != 3 && std::abs(now[i] - drawn[i]) > 1e-4f) return true;
    return hud_.Uniforms().loop != drawn_hud_.loop || std::memcmp(&marks_, &drawn_marks_, sizeof(marks_)) != 0 ||
           std::memcmp(&bookmark_marks_, &drawn_bookmarks_, sizeof(bookmark_marks_)) != 0;
}
void D3D12Renderer::LandAudio(std::int64_t media) noexcept {
    // Music mode's landing: no picture comes, so the epoch lands when its sound plays.
    if (!preview_) return;
    const auto presented = InputTime();
    preview_ = false; ready_epoch_ = epoch_; hud_media_ = media;
    seek_ = {epoch_, media, 0, presented - seek_requested_, presented, shown_subtitles_};
}
VisualUniforms D3D12Renderer::MusicUniforms(std::int64_t now) noexcept {
    const double dt = music_advanced_ ? (std::clamp)(static_cast<double>(now - music_advanced_) / 1e7, 0.0, 0.1) : 0.0;
    music_advanced_ = now;
    const auto& p = audio_picture_;
    const float fall = hud_paused_ ? 0.0f : static_cast<float>(1.6 * dt); // Held while paused.
    const auto follow = [fall](float& shown, float target) { shown = target >= shown ? target : (std::max)(target, shown - fall); };
    // The picture is of the PCM before the output gain: shown as heard, the gain moves the bands and the level down by its
    // decibels (a band code is 78/255 dB, the level 60 dB full scale), the peak and the waveform by its amplitude. Muted,
    // everything falls flat.
    const float gain = audio::OutputGain::Gain(volume_, muted_);
    const float decibels = gain > 0 ? 20.0f * std::log10(gain) : -1000.0f;
    const float band_shift = decibels * (255.0f / 78.0f) / 255.0f, level_shift = decibels / 60.0f;
    for (unsigned b = 0; b < audio::AudioPicture::kBands; ++b) follow(shown_bands_[b], p.bands[b] ? (std::max)(0.0f, p.bands[b] / 255.0f + band_shift) : 0.0f);
    follow(shown_level_, (std::max)(0.0f, p.level + level_shift)); follow(shown_peak_, p.peak * gain);
    VisualUniforms u;
    u.width = static_cast<float>(width_); u.height = static_cast<float>(height_);
    u.time = static_cast<float>(static_cast<double>(now % (3600ll * 10000000)) / 1e7);
    u.level = shown_level_; u.peak = shown_peak_; u.scale = dpi_; u.dim = hud_paused_ ? 1.0f : muted_ ? 0.6f : 0.0f;
    for (unsigned b = 0; b < audio::AudioPicture::kBands; ++b)
        u.bands[b / 4] |= static_cast<std::uint32_t>(std::lround((std::clamp)(shown_bands_[b], 0.0f, 1.0f) * 255.0f)) << (8 * (b % 4));
    for (unsigned c = 0; c < audio::AudioPicture::kColumns; ++c) {
        const auto low = static_cast<std::uint32_t>(std::lround(p.low[c] * gain) + 128), high = static_cast<std::uint32_t>(std::lround(p.high[c] * gain) + 128);
        u.wave[c / 2] |= (low | high << 8) << (16 * (c % 2));
    }
    return u;
}
RenderStatus D3D12Renderer::Composite(unsigned slot, std::int64_t now) noexcept {
    const auto complete = fence_->GetCompletedValue(); if (complete == UINT64_MAX) return RenderStatus::Failed;
    const bool video = slot < Count;
    if (video && !SlotReady(slots_[slot], complete)) { waited_ |= kWaitConversion; return RenderStatus::WouldBlock; }
    unsigned index = 0;
    if (texture_output_) {
        // Texture output (API 13): a texture neither published nor held, whose last composite's work has retired.
        const int claimed = output_.Claim([&](unsigned i) { return complete >= composites_[i].fence; });
        if (claimed < 0) { if (video) waited_ |= kWaitBackBuffer; return RenderStatus::WouldBlock; }
        index = static_cast<unsigned>(claimed);
    } else index = swap_->GetCurrentBackBufferIndex();
    auto& c = composites_[index];
    if (complete < c.fence) { if (texture_output_) output_.Unclaim(index); if (video) waited_ |= kWaitBackBuffer; return RenderStatus::WouldBlock; }
    if (!texture_output_ && WaitForSingleObject(latency_, 0) != WAIT_OBJECT_0) { if (video) waited_ |= kWaitLatency; return RenderStatus::WouldBlock; }
    error_ = c.allocator->Reset(); if (SUCCEEDED(error_)) error_ = c.commands->Reset(c.allocator, nullptr);
    if (FAILED(error_)) return RenderStatus::Failed;
    auto* back = buffers_[index];
    auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += static_cast<SIZE_T>(index) * rtv_stride_;
    constexpr float black[]{0, 0, 0, 1};
    video_rect_ = {}; staged_visual_ = false;
    staged_transformed_ = staged_native_ = false; staged_generation_ = 0; zoom_redraw_ = false;
    Region shown_region{}; // The picture's region composed (a snapshot's record).
    const bool overlays = !HdrOutput(); // API 14: the 8-bit overlays are not drawn into a 10-bit or FP16 texture.
    if (!video && music_ && visual_pipeline_.Ready() && overlays) {
        // Music mode: the sound is the background (opaque, the whole target).
        auto b = Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET); c.commands->ResourceBarrier(1, &b);
        visual_pipeline_.Record(c.commands, rtv, width_, height_, MusicUniforms(now)); staged_visual_ = true;
    } else if (video || history_valid_) {
        auto* source = video ? slots_[slot].rgb : history_;
        const unsigned picture_width = video ? slots_[slot].width : history_width_, picture_height = video ? slots_[slot].height : history_height_;
        // Live zoom: a picture converted from another region than the one asked for now (converted before the zoom or pan
        // changed) is shown through the change, sampled out of what it holds (docs/picture.md, "Live zoom").
        const Region converted = video ? slots_[slot].region : history_region_;
        // Tracking: a picture converted following the target keeps its framing; one converted before (the queue at T) is
        // shown through the tracking view at once.
        const Region wanted = !video ? RegionOf(history_crop_) : !Tracking() ? RegionOf(slots_[slot].crop) :
                              slots_[slot].tracked ? slots_[slot].region : TrackedView(slots_[slot]);
        if (video) slots_[slot].view = wanted;
        shown_region = wanted;
        const bool transformed = converted.right > converted.left && converted.bottom > converted.top && !(converted == wanted);
        staged_transformed_ = transformed; staged_generation_ = zoom_generation_;
        staged_native_ = !transformed && (Tracking() || (video ? slots_[slot].zoom_generation : history_generation_) == zoom_generation_);
        // Letterboxed/pillarboxed, centred. A picture converted for this window size is copied; one converted for an
        // earlier size (queued just before a resize, or the held picture while paused) is scaled into place.
        unsigned width = width_, height = height_; Fit(picture_width, picture_height, width, height);
        const bool resized = width != picture_width || height != picture_height, covers = width == width_ && height == height_;
        const bool scaled = resized || transformed;
        video_rect_ = {(width_ - width) / 2, (height_ - height) / 2, width, height};
        D3D12_TEXTURE_COPY_LOCATION to{}; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX whole{0, 0, 0, picture_width, picture_height, 1};
        D3D12_RESOURCE_BARRIER b[2]{Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE), {}};
        c.commands->ResourceBarrier(1, b);
        if (video) { // Keep the clean picture for controls-only refreshes.
            auto h = Barrier(history_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); c.commands->ResourceBarrier(1, &h);
            to.pResource = history_; c.commands->CopyTextureRegion(&to, 0, 0, 0, &from, &whole);
            history_width_ = picture_width; history_height_ = picture_height;
            history_crop_ = slots_[slot].crop; history_region_ = slots_[slot].region; history_generation_ = slots_[slot].zoom_generation;
            h = Barrier(history_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); c.commands->ResourceBarrier(1, &h);
            // The source is in COPY_SOURCE here: the loop pins copy from it in the same state round trip.
            b[0] = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); c.commands->ResourceBarrier(1, b);
            PinPicture(slots_[slot], c.commands);
            b[0] = Barrier(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE); c.commands->ResourceBarrier(1, b);
        }
        if (scaled || !covers) {
            b[1] = Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET); c.commands->ResourceBarrier(1, &b[1]);
            if (!covers) c.commands->ClearRenderTargetView(rtv, black, 0, nullptr);
        }
        if (scaled) {
            b[0] = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); c.commands->ResourceBarrier(1, b);
            ID3D12DescriptorHeap* heaps[]{descriptors_}; c.commands->SetDescriptorHeaps(1, heaps);
            c.commands->SetGraphicsRootSignature(blit_root_); c.commands->SetPipelineState(blit_);
            // The picture is the slot's top-left corner; sampling stops at its last texel (PictureBlit.hpp). The view is
            // the region asked for now, in the region the picture was converted from.
            auto extent = MakeBlitConstants(picture_width, picture_height, capacity_width_, capacity_height_);
            if (transformed) ViewOf(converted, wanted, extent);
            c.commands->SetGraphicsRoot32BitConstants(0, sizeof(extent) / 4, &extent, 0);
            auto srv = descriptors_->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += static_cast<UINT64>(surfaces_ * 2 + Count + (video ? slot : Count)) * stride_;
            c.commands->SetGraphicsRootDescriptorTable(1, srv);
            const D3D12_VIEWPORT viewport{static_cast<float>(video_rect_.x), static_cast<float>(video_rect_.y), static_cast<float>(width), static_cast<float>(height), 0, 1};
            const D3D12_RECT scissor{static_cast<LONG>(video_rect_.x), static_cast<LONG>(video_rect_.y),
                                     static_cast<LONG>(video_rect_.x + width), static_cast<LONG>(video_rect_.y + height)};
            c.commands->RSSetViewports(1, &viewport); c.commands->RSSetScissorRects(1, &scissor);
            c.commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr); c.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c.commands->DrawInstanced(3, 1, 0, 0);
            if (resized) ++scaled_composites_;
            if (transformed) ++transformed_composites_;
            b[0] = Barrier(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); c.commands->ResourceBarrier(1, b);
        } else {
            b[1] = Barrier(back, covers ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            c.commands->ResourceBarrier(1, &b[1]);
            to.pResource = back; c.commands->CopyTextureRegion(&to, video_rect_.x, video_rect_.y, 0, &from, &whole);
            b[0] = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            b[1] = Barrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET); c.commands->ResourceBarrier(2, b);
        }
    } else {
        auto b = Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET); c.commands->ResourceBarrier(1, &b);
        c.commands->ClearRenderTargetView(rtv, black, 0, nullptr);
    }
    if (video_rect_.width && video_rect_.height) {
        const auto& v = video_rect_;
        // Pinned | Live: the reference over the picture's left half, then a seam (the two columns either side of the middle).
        if (compare_mode_ == Compare::Pinned && reference_valid_) {
            unsigned width = width_, height = height_; Fit(reference_width_, reference_height_, width, height);
            if (width == reference_width_ && height == reference_height_ && width == v.width && height == v.height) {
                D3D12_RESOURCE_BARRIER b[]{Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
                                           Barrier(reference_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
                c.commands->ResourceBarrier(2, b);
                D3D12_TEXTURE_COPY_LOCATION to{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{reference_, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                const D3D12_BOX box{0, 0, 0, v.width / 2, v.height, 1};
                c.commands->CopyTextureRegion(&to, v.x, v.y, 0, &from, &box);
                for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                c.commands->ResourceBarrier(2, b);
            } else { // Another size: scaled into the same rectangle, its left half.
                auto b = Barrier(reference_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); c.commands->ResourceBarrier(1, &b);
                ID3D12DescriptorHeap* heaps[]{descriptors_}; c.commands->SetDescriptorHeaps(1, heaps);
                c.commands->SetGraphicsRootSignature(blit_root_); c.commands->SetPipelineState(blit_);
                const auto constants = MakeBlitConstants(reference_width_, reference_height_, capacity_width_, capacity_height_);
                c.commands->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                auto srv = descriptors_->GetGPUDescriptorHandleForHeapStart(); srv.ptr += static_cast<UINT64>(surfaces_ * 2 + Count * 2 + 6) * stride_;
                c.commands->SetGraphicsRootDescriptorTable(1, srv);
                const D3D12_VIEWPORT viewport{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.width), static_cast<float>(v.height), 0, 1};
                const D3D12_RECT scissor{static_cast<LONG>(v.x), static_cast<LONG>(v.y), static_cast<LONG>(v.x + v.width / 2), static_cast<LONG>(v.y + v.height)};
                c.commands->RSSetViewports(1, &viewport); c.commands->RSSetScissorRects(1, &scissor);
                c.commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr); c.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c.commands->DrawInstanced(3, 1, 0, 0);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter); c.commands->ResourceBarrier(1, &b);
            }
            constexpr float seam[]{0.92f, 0.94f, 0.97f, 1};
            const D3D12_RECT line{static_cast<LONG>(v.x + v.width / 2 - 1), static_cast<LONG>(v.y), static_cast<LONG>(v.x + v.width / 2 + 1), static_cast<LONG>(v.y + v.height)};
            c.commands->ClearRenderTargetView(rtv, seam, 1, &line);
            ++pinned_compare_composites_;
        }
        // Test hook: the picture as composed, before captions and controls, into the capture readback.
        if (screen_capture_requested_ && capture_readback_ && !capture_pending_ && v.width <= capacity_width_ && v.height <= capacity_height_) {
            screen_capture_requested_ = false;
            auto b = Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); c.commands->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION to{capture_readback_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = capture_layout_;
            D3D12_TEXTURE_COPY_LOCATION from{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            const D3D12_BOX box{v.x, v.y, 0, v.x + v.width, v.y + v.height, 1};
            c.commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter); c.commands->ResourceBarrier(1, &b);
            capture_fence_ = sequence_ + 1; capture_width_ = v.width; capture_height_ = v.height; capture_kind_ = CaptureKind::Screen; capture_pending_ = true;
            capture_pts_ = video ? slots_[slot].frame.pts_100ns : shown_pts_;
        }
        // A forensic snapshot (2026-11-01): the same picture, as composed, into the snapshot readback; this composite signals
        // sequence_ + 1.
        if (snapshot_state_ == SnapshotState::Requested && (!snapshot_data_ || v.width > snapshot_width_ || v.height > snapshot_height_))
            snapshot_state_ = SnapshotState::Idle; // The window grew past the readback made for it: the coordinator makes another.
        if (snapshot_state_ == SnapshotState::Requested) {
            auto b = Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); c.commands->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION to{snapshot_readback_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = snapshot_layout_;
            D3D12_TEXTURE_COPY_LOCATION from{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            const D3D12_BOX box{v.x, v.y, 0, v.x + v.width, v.y + v.height, 1};
            c.commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter); c.commands->ResourceBarrier(1, &b);
            snapshot_fence_ = sequence_ + 1; snapshot_state_ = SnapshotState::Copying;
            auto& s = snapshot_;
            s.rgba = snapshot_data_ + snapshot_layout_.Offset; s.stride = snapshot_layout_.Footprint.RowPitch; s.width = v.width; s.height = v.height;
            s.pts_100ns = video ? slots_[slot].frame.pts_100ns : shown_pts_; s.region = shown_region;
            s.zoom = zoom_; s.x = zoom_x_; s.y = zoom_y_; s.level = orientation_.level; s.quarter_turns = orientation_.QuarterTurns(); s.mirrored = orientation_.Mirrored();
            s.auto_level = auto_level_;
            s.compare = compare_mode_ == Compare::Pinned && reference_valid_ ? std::uint8_t{2} : compare_ ? std::uint8_t{1} : std::uint8_t{0};
        }
    }
    staged_hud_ = hud_enabled_ && hud_.Animating() && overlays;
    const auto subtitle_time = video ? MediaTime(slots_[slot].frame) : hud_media_;
    // The cues drawn are those whose glyphs are all in the atlas (generated in this list if just outlined); this
    // composite signals sequence_ + 1.
    const float inset = CaptionInset(hud_.Uniforms(), width_, height_, dpi_);
    staged_subtitles_ = {}; staged_subtitles_.time = subtitle_time;
    if (subtitle_track_ && overlays) staged_subtitles_.count = subtitles_.Record(c.commands, rtv, width_, height_, subtitle_time, inset, dpi_,
                                                                    {complete, sequence_ + 1}, staged_subtitles_.cues);
    staged_subtitle_stamp_ = Stamp(staged_subtitles_);
    if (staged_hud_) hud_pipeline_.Record(c.commands, rtv, width_, height_, hud_.Uniforms(), marks_, bookmark_marks_);
    staged_hud_uniforms_ = hud_.Uniforms(); staged_marks_ = marks_; staged_bookmarks_ = bookmark_marks_;
    // The command bar floats above everything else; a finished text raster is shown by this very composite.
    // The status pill, at the composite's own time (its fades are closed-form), under the command bar.
    status_.Advance(now); status_pipeline_.Update(status_, now);
    staged_status_ = status_.Visible() && status_pipeline_.Ready() && overlays;
    if (staged_status_) status_pipeline_.Record(c.commands, rtv, width_, height_, status_, {complete, sequence_ + 1});
    staged_palette_ = palette_.Visible() && palette_pipeline_.Ready() && overlays;
    if (staged_palette_) palette_pipeline_.Record(c.commands, rtv, width_, height_, palette_, {complete, sequence_ + 1});
    auto b = Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT); c.commands->ResourceBarrier(1, &b);
    error_ = c.commands->Close(); if (FAILED(error_)) return RenderStatus::Failed;
    // Texture output: the host's GPU work on this texture first (its release's value on the host fence), on the GPU.
    if (texture_output_) if (const auto wait = output_.HostWait(index); wait && output_.HostFence()) {
        error_ = queue_->Wait(output_.HostFence(), wait); if (FAILED(error_)) return RenderStatus::Failed;
    }
    ID3D12CommandList* lists[]{c.commands}; queue_->ExecuteCommandLists(1, lists);
    c.fence = ++sequence_; error_ = queue_->Signal(fence_, c.fence); if (FAILED(error_)) return RenderStatus::Failed;
    // Its fence value signalled, then published to the host (with the picture's transfer: an HDR format's host knows it is HDR).
    if (texture_output_) (void)output_.Publish(queue_, index, subtitle_time, video ? (slots_[slot].frame.color_metadata_id >> 16) & 255u : shown_transfer_);
    if (video) { slots_[slot].fence = c.fence; history_valid_ = true; }
    swap_pending_ = true; staged_slot_ = slot; staged_context_ = index; redraw_ = false;
    reconverted_staged_ = !video && reconverted_; if (!video) reconverted_ = false;
    return FinishPresent(now);
}
RenderStatus D3D12Renderer::FinishPresent(std::int64_t now) noexcept {
    error_ = texture_output_ ? S_OK : swap_->Present(0, DXGI_PRESENT_DO_NOT_WAIT); // Texture output: published already (Composite).
    if (error_ == DXGI_ERROR_WAS_STILL_DRAWING) { if (staged_slot_ < Count) waited_ |= kWaitPresent; return RenderStatus::WouldBlock; }
    if (FAILED(error_)) return RenderStatus::Failed;
    // Include Present's queued work before resetting this back buffer's allocator on a later pass.
    auto& c = composites_[staged_context_]; c.fence = ++sequence_;
    error_ = queue_->Signal(fence_, c.fence); if (FAILED(error_)) return RenderStatus::Failed;
    swap_pending_ = false; hud_was_drawn_ = staged_hud_; hud_present_ = now;
    drawn_hud_ = staged_hud_uniforms_; drawn_marks_ = staged_marks_; drawn_bookmarks_ = staged_bookmarks_;
    if (live_zoom_count_ && staged_generation_) {
        // Live zoom: the latest change made while playing, on screen (through the transform, or converted at it).
        auto& z = live_zooms_[live_zoom_count_ <= kMaxLiveZooms ? live_zoom_count_ - 1 : kMaxLiveZooms - 1];
        if (staged_generation_ == z.generation) {
            const auto at = InputTime();
            if (!z.shown) z.shown = at - z.requested;
            if (staged_transformed_ && !z.native) ++z.bridged;
            if (staged_native_ && !z.native) z.native = at - z.requested;
        }
    }
    if (reconverted_staged_) { // A paused re-conversion is on screen: from the setting's change to now.
        reconverted_staged_ = false; pending_update_.latency = InputTime() - settings_asked_;
        awaiting_update_ = paused_update_count_ < kMaxPausedUpdates ? paused_update_count_ : kMaxPausedUpdates - 1; awaiting_fence_ = c.fence;
        paused_updates_[awaiting_update_] = pending_update_; ++paused_update_count_;
    }
    palette_was_drawn_ = staged_palette_; if (staged_palette_) ++palette_presents_;
    if (staged_visual_) ++music_presents_;
    status_was_drawn_ = staged_status_;
    if (staged_status_) {
        ++status_presents_;
        const auto shown = status_.ShownRevision();
        if (shown != status_counted_ && status_pipeline_.Stats().revision == shown) { status_counted_ = shown; ++status_shown_; }
    }
    subtitle_stamp_ = staged_subtitle_stamp_; shown_subtitles_ = staged_subtitles_; if (shown_subtitles_.count) ++subtitle_presents_;
    for (unsigned i = 0; i < shown_subtitles_.count && i < subtitle::SubtitleTrack::kMaxActive; ++i) {
        const auto cue = shown_subtitles_.cues[i]; if (cue >= subtitle::SubtitleTrack::kMaxCues) continue;
        auto& word = cue_seen_[cue / 64]; const auto bit = std::uint64_t{1} << (cue % 64);
        if (!(word & bit)) { word |= bit; ++cues_shown_; }
    }
    if (staged_hud_) {
        ++hud_presents_; if (hud_paused_) ++paused_hud_presents_;
        if (hud_.Uniforms().volume_visibility > 0.5f) ++volume_presents_;
    }
    if (staged_slot_ < Count) {
        auto& s = slots_[staged_slot_]; s.fence = c.fence;
        // The HUD follows what is on screen, so a seek's winding lane collapses on the first target frame.
        shown_pts_ = s.frame.pts_100ns; shown_duration_ = s.frame.duration_100ns; hud_media_ = MediaTime(s.frame);
        shown_transfer_ = (s.frame.color_metadata_id >> 16) & 255u;
        if (s.crop.right > s.crop.left && s.crop.bottom > s.crop.top) { // The centre of its region in the crop (the audio focus follows it).
            const Region& v = s.view.right > s.view.left ? s.view : s.region; // What was composited (a live view or its region).
            shown_x_ = ((v.left + v.right) / 2.0f - s.crop.left) / static_cast<float>(s.crop.right - s.crop.left);
            shown_y_ = ((v.top + v.bottom) / 2.0f - s.crop.top) / static_cast<float>(s.crop.bottom - s.crop.top);
            shown_zoom_ = v.right > v.left ? static_cast<float>(s.crop.right - s.crop.left) / static_cast<float>(v.right - v.left) : 1.0f;
        }
        if (staged_deadline_ && now - staged_deadline_ > worst_flip_.lateness) worst_flip_ = {s.frame.pts_100ns, now - staged_deadline_, waited_};
        if (staged_deadline_) Trace(s.frame.pts_100ns, staged_deadline_, now, false);
        const auto waited = waited_;
        staged_deadline_ = 0; waited_ = 0;
        if (recovery_preview_ && s.frame.playback_epoch == epoch_) {
            recovery_preview_ = false;
            recovery_landing_ = {epoch_, s.frame.pts_100ns, s.frame.duration_100ns, now - recovery_requested_, now, shown_subtitles_};
        }
        if (s.frame.playback_epoch == epoch_) ready_epoch_ = epoch_;
        if (signature_sink_) {
            if (s.stats) HarvestScenes(fence_->GetCompletedValue()); // Its conversion has run by now, almost always.
            const PictureSignature g{s.frame.pts_100ns, s.frame.duration_100ns, InputTime(), s.cut ? s.cut_pts : -1, s.frame.playback_epoch, s.luma, s.peak,
                                     s.motion, s.cut, s.measured};
            signature_sink_(signature_context_, g);
        }
        if (preview_ && s.frame.playback_epoch == epoch_) {
            // Stamped now, after Present returned (`now` is when this presentation pass began: the image was composed since).
            const auto presented = InputTime();
            preview_ = false; seek_ = {epoch_, s.frame.pts_100ns, s.frame.duration_100ns, presented - seek_requested_, presented, shown_subtitles_, waited};
            if (refresh_in_flight_) { // A refresh landing: the picture on screen again, converted with the settings of then.
                refresh_in_flight_ = false;
                awaiting_update_ = paused_update_count_ < kMaxPausedUpdates ? paused_update_count_ : kMaxPausedUpdates - 1; awaiting_fence_ = c.fence;
                paused_updates_[awaiting_update_] = {s.tier, guided_chroma_ ? kGuidedChroma : kBilinearChroma, compare_, true, presented - settings_asked_, 0};
                ++paused_update_count_;
                if (reconvert_after_refresh_) { reconvert_after_refresh_ = false; reconvert_pending_ = held_valid_; }
            }
        }
        return Retire(s, true);
    }
    return RenderStatus::Ready;
}
float D3D12Renderer::CaptionInset(const ui::HudUniforms& hud, unsigned width, unsigned height, float dpi) noexcept {
    const float rest = 1.3f * SdfSubtitlePipeline::FontPixels(width, height, dpi);
    const float dock_top = (static_cast<float>(height) - hud.center_y) + hud.dock_half_height * hud.scale + 20 * hud.scale;
    return rest + hud.visibility * (std::max)(dock_top - rest, 0.0f);
}
void D3D12Renderer::Fit(unsigned source_width, unsigned source_height, unsigned& width, unsigned& height) const noexcept {
    // Aspect-preserving fit of the source into width x height (itself clamped to the slot capacity). Within 2 px of
    // the bound, snap to it: a rounding sliver is not worth a black bar.
    width = (std::min)(width, capacity_width_ ? capacity_width_ : width); height = (std::min)(height, capacity_height_ ? capacity_height_ : height);
    const auto fitted = video::FitInside({source_width, source_height}, {width, height});
    width = fitted.width; height = fitted.height;
}
void D3D12Renderer::RequestResize(unsigned width, unsigned height) noexcept {
    if (!width || !height || width > 16384 || height > 16384) return;
    pending_width_ = width; pending_height_ = height; resize_pending_ = true;
}
RenderStatus D3D12Renderer::ApplyResize() noexcept {
    // ResizeBuffers needs every back-buffer reference released and no queued GPU work using them: wait (without
    // blocking) until the staged present is done and every composite context's fence has retired.
    if (swap_pending_) return RenderStatus::WouldBlock;
    const auto completed = fence_->GetCompletedValue(); if (completed == UINT64_MAX) return RenderStatus::Failed;
    for (const auto& c : composites_) if (completed < c.fence) return RenderStatus::WouldBlock;
    if (pending_width_ != width_ || pending_height_ != height_) {
        if (texture_output_) {
            ReleaseOutputs();
            if (!CreateOutputs(pending_width_, pending_height_)) return RenderStatus::Failed;
        } else {
            for (auto*& buffer : buffers_) Release(buffer);
            error_ = swap_->ResizeBuffers(3, pending_width_, pending_height_, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            if (FAILED(error_)) return RenderStatus::Failed;
        }
        for (unsigned i = 0; i != 3; ++i) {
            if (!texture_output_ && FAILED(error_ = swap_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) return RenderStatus::Failed;
            auto target = rtvs_->GetCPUDescriptorHandleForHeapStart(); target.ptr += static_cast<SIZE_T>(i) * rtv_stride_;
            device_->CreateRenderTargetView(buffers_[i], nullptr, target);
        }
        width_ = pending_width_; height_ = pending_height_; ++resizes_;
    }
    hud_.Resize(width_, height_, dpi_); palette_.Layout(width_, height_, dpi_); status_.Layout(width_, height_, dpi_);
    UpdateRefresh(); // Fullscreen and snapping can move the window to another monitor.
    resize_pending_ = false; redraw_ = true; // Recompose the held picture at the new size, also while paused.
    return RenderStatus::Ready;
}
void D3D12Renderer::UpdateRefresh() noexcept {
    // The refresh period the desktop is composed at; 60 Hz when Windows does not say. Asked of the compositor, which
    // answers from its own timing data: EnumDisplaySettings asks the display driver and stalled playback for 170 ms.
    DWM_TIMING_INFO timing{}; timing.cbSize = sizeof(timing);
    refresh_period_ = 166667;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiNumerator && timing.rateRefresh.uiDenominator) {
        const auto period = 10000000ll * timing.rateRefresh.uiDenominator / timing.rateRefresh.uiNumerator;
        if (period >= 10000 && period <= 416667) refresh_period_ = period; // 24 Hz to 1 kHz.
    }
}
void D3D12Renderer::ToggleFullscreen() noexcept {
    if (!window_) return;
    if (parent_) { fullscreen_requests_.fetch_add(1, std::memory_order_relaxed); return; } // Embedded: asked of the host (MEDIA_EVENT_FULLSCREEN_REQUEST).
    if (!fullscreen_) {
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        if (!GetWindowRect(window_, &restore_) || !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        fullscreen_ = true; const auto& r = monitor.rcMonitor;
        SetWindowPos(window_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_FRAMECHANGED | (message_thread_ ? SWP_ASYNCWINDOWPOS : 0u));
        // With a window thread the move is its to make (asynchronous): the client is the whole window (borderless), so the
        // swap chain's new size is known now, and presentation waits for it as for any resize (its WM_SIZE follows).
        if (message_thread_) RequestResize(static_cast<unsigned>(r.right - r.left), static_cast<unsigned>(r.bottom - r.top));
    } else {
        fullscreen_ = false; const auto& r = restore_;
        SetWindowPos(window_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | (message_thread_ ? SWP_ASYNCWINDOWPOS : 0u));
        if (message_thread_) RequestResize(static_cast<unsigned>(r.right - r.left), static_cast<unsigned>(r.bottom - r.top));
    }
    ui_fullscreen_.store(fullscreen_, std::memory_order_relaxed);
}
std::int64_t D3D12Renderer::InputTime() const noexcept {
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    return input_frequency_ ? counter.QuadPart / input_frequency_ * 10000000 + counter.QuadPart % input_frequency_ * 10000000 / input_frequency_ : 0;
}
void D3D12Renderer::ConfigureHud(std::int64_t duration, bool always_visible, HudHandler handler, void* context) noexcept {
    hud_handler_ = handler; hud_context_ = context; hud_.Configure(width_, height_, duration, always_visible, dpi_);
    BOOL animations = TRUE; (void)SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0); hud_.SetReducedMotion(animations == FALSE);
    palette_.SetReducedMotion(animations == FALSE); status_.SetReducedMotion(animations == FALSE);
    InputHud({ui::PointerEvent::Kind::Move, -100, -100, InputTime()}); // Brief discovery reveal, then idle fade.
}
bool D3D12Renderer::PrepareSubtitles(core::LinearMemoryArena& arena, const subtitle::SubtitleTrack& track, unsigned min_cells) noexcept {
    error_ = subtitles_.Prepare(device_,queue_,arena,track,min_cells);
    if (FAILED(error_)) return false;
    subtitle_track_ = &track; subtitles_prepared_.store(true, std::memory_order_release); return true;
}
bool D3D12Renderer::RequestSubtitleRebind(const subtitle::SubtitleTrack* track) noexcept {
    if (!track || !subtitles_prepared_.load(std::memory_order_acquire)) return false;
    const subtitle::SubtitleTrack* none = nullptr;
    return pending_rebind_.compare_exchange_strong(none, track, std::memory_order_acq_rel);
}
std::uint64_t D3D12Renderer::SelectSubtitles(std::int64_t time, SubtitleFrame& frame) const noexcept {
    // The cues a composite at `time` would draw: a cue whose glyphs finish outlining changes the stamp, so it appears
    // with the next refresh even while nothing else moves.
    frame = {}; frame.time = time;
    if (subtitle_track_) frame.count = subtitles_.Drawable(time, frame.cues);
    return Stamp(frame);
}
std::uint64_t D3D12Renderer::Stamp(const SubtitleFrame& frame) noexcept {
    std::uint64_t stamp = 0;
    for (unsigned i = 0; i < frame.count && i < subtitle::SubtitleTrack::kMaxActive; ++i) stamp = stamp * 4097 + frame.cues[i] + 1;
    return stamp;
}
void D3D12Renderer::HudAction(ui::HudAction action) noexcept {
    if (action.command == ui::HudCommand::None) return;
    const bool accepted = hud_handler_ && hud_handler_(hud_context_, action);
    if (action.command == ui::HudCommand::Volume) return; // The model already shows it; the engine's report corrects it.
    // A scrub seek the engine cannot take now is not an error: the release's seek decides, and shows any rejection. A
    // chapter or file key's outcome is the engine's status cue.
    if (action.command == ui::HudCommand::Scrub || action.command == ui::HudCommand::Chapter || action.command == ui::HudCommand::File ||
        action.command == ui::HudCommand::Speed || action.command == ui::HudCommand::Step || action.command == ui::HudCommand::Loop ||
        action.command == ui::HudCommand::Stats || action.command == ui::HudCommand::Night || action.command == ui::HudCommand::Bookmark ||
        action.command == ui::HudCommand::Snapshot) return;
    if (!accepted) { hud_.SeekRejected(); if (action.command == ui::HudCommand::Seek) ++rejected_seeks_; }
    else if (action.command == ui::HudCommand::Pause || action.command == ui::HudCommand::Resume)
        hud_paused_ = action.command == ui::HudCommand::Pause;
}
void D3D12Renderer::InputHud(const ui::PointerEvent& event) noexcept {
    if (!hud_enabled_) return; // No dock to touch (docs/sdk.md, "Modules").
    HudAction(hud_.Input(event, hud_paused_));
}
bool D3D12Renderer::WaitIdle() noexcept {
    // A live device may still execute queued work that uses these objects: wait for its queue (bounded by device
    // removal, which retires everything). A removed device needs no wait.
    if (!queue_ || !fence_ || !event_ || FAILED(device_->GetDeviceRemovedReason())) return true;
    const auto value = ++sequence_;
    error_ = queue_->Signal(fence_, value);
    if (SUCCEEDED(error_)) error_ = fence_->SetEventOnCompletion(value, event_);
    if (FAILED(error_) && SUCCEEDED(device_->GetDeviceRemovedReason())) return false;
    while (SUCCEEDED(device_->GetDeviceRemovedReason()) && fence_->GetCompletedValue() < value) {
        const auto wait = WaitForSingleObject(event_, 100);
        if (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0) { error_ = HRESULT_FROM_WIN32(GetLastError()); return false; }
    }
    return true;
}
void D3D12Renderer::ReleaseGpu() noexcept { ReleaseFile(); ReleaseShared(); }
void D3D12Renderer::ReleaseShared() noexcept {
    for (auto& c : composites_) { Release(c.commands); Release(c.allocator); }
    palette_pipeline_.ReleaseGpu(); status_pipeline_.ReleaseGpu(); hud_pipeline_.Release(); visual_pipeline_.Release(); Release(blit_); Release(blit_root_); Release(rtvs_);
    for (auto*& buffer : buffers_) Release(buffer);
    color_.Release();
    Release(swap_); Release(queue_); Release(fence_);
    if (latency_) { CloseHandle(latency_); latency_ = nullptr; }
    ReleaseOutputs(); output_.Release();
}
// ---- Texture output (API 13; docs/sdk.md, "Texture output: API 13") ----
bool D3D12Renderer::CreateOutputs(unsigned width, unsigned height) noexcept {
    // The shared textures (render::SharedOutput, a new generation), drawn into as render targets; buffers_ holds a reference each.
    if (!output_.CreateTextures(device_, width, height, output_format_, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        error_ = FAILED(output_.LastError()) ? output_.LastError() : E_FAIL; return false;
    }
    for (unsigned i = 0; i != kOutputs; ++i) { buffers_[i] = output_.Texture(i); buffers_[i]->AddRef(); }
    return true;
}
void D3D12Renderer::ReleaseOutputs() noexcept {
    if (texture_output_) for (auto*& buffer : buffers_) Release(buffer);
    output_.ReleaseTextures();
}
void D3D12Renderer::PollOutputSize() noexcept {
    if (const auto asked = output_request_.exchange(0, std::memory_order_acq_rel))
        RequestResize(static_cast<unsigned>(asked >> 32), static_cast<unsigned>(asked & 0xffffffffu));
}
void D3D12Renderer::ReleaseFile() noexcept {
    for (auto& s : slots_) { Release(s.commands); Release(s.allocator); Release(s.rgb); }
    neural_pass_.Release(); Release(neural_texture_);
    while (perceive_thread_ && steady_job_.load(std::memory_order_acquire) == 1) Sleep(1); // Perception's thread reads the grid's readback.
    steady_job_.store(0, std::memory_order_relaxed); steady_stage_ = 0;
    Release(steady_commands_); Release(steady_allocator_);
    Release(still_commands_); Release(still_allocator_); Release(held_); Release(capture_readback_);
    DropSnapshotReadback(); snapshot_state_ = SnapshotState::Idle;
    held_valid_ = reconvert_pending_ = refresh_wanted_ = refresh_in_flight_ = reconverted_ = capture_pending_ = false; still_fence_ = 0;
    if (timestamp_readback_ && timestamp_data_) { const D3D12_RANGE none{0, 0}; timestamp_readback_->Unmap(0, &none); }
    timestamp_data_ = nullptr; timestamp_frequency_ = 0; Release(timestamp_readback_); Release(timestamps_);
    for (auto& s : slots_) s.timed = s.stats = false;
    if (stats_readback_ && stats_data_) { const D3D12_RANGE none{0, 0}; stats_readback_->Unmap(0, &none); }
    stats_data_ = nullptr; Release(stats_readback_);
    for (auto& s : slots_) s.grid = false;
    if (grid_readback_ && grid_data_) { const D3D12_RANGE none{0, 0}; grid_readback_->Unmap(0, &none); }
    grid_data_ = nullptr; Release(grid_readback_); Release(grid_); tracker_.Stop(); track_pending_ = false;
    if (vibro_readback_ && vibro_data_) { const D3D12_RANGE none{0, 0}; vibro_readback_->Unmap(0, &none); }
    vibro_data_ = nullptr; Release(vibro_readback_); Release(vibro_buffer_);
    if (tensor_readback_ && tensor_data_) { const D3D12_RANGE none{0, 0}; tensor_readback_->Unmap(0, &none); }
    tensor_data_ = nullptr; Release(tensor_readback_); Release(tensor_buffer_); tensor_cells_ = 0; tensor_state_.store(0, std::memory_order_relaxed);
    subtitles_.ReleaseGpu(); Release(history_); Release(descriptors_);
    Release(reference_); for (auto*& pin : pin_rgb_) Release(pin);
    Release(pin_commands_); Release(pin_allocator_);
    for (auto*& c : turn_commands_) Release(c);
    for (auto*& a : turn_allocators_) Release(a);
    Release(copy_fence_); Release(copy_queue_); copy_sequence_ = 0;
    for (auto& s : slots_) s.copy_fence = 0;
    pin_fence_ = 0; pin_count_ = 0; pin_complete_ = false; reference_valid_ = false;
}
bool D3D12Renderer::ReleaseDevice(ULONG& remaining) noexcept {
    remaining = 0;
    if (!device_ || !WaitIdle()) return false;
    ReleaseGpu();
    // Every child object is gone: the count left on the device is what something else still holds (0 unless a
    // reference leaked, which would also make D3D12CreateDevice hand the removed device back).
    remaining = device_->Release(); device_ = nullptr;
    for (auto& s : slots_) { s.busy = s.staged = false; s.fence = 0; }
    landing_slot_ = Count; turn_fence_[0] = turn_fence_[1] = 0;
    for (auto& c : composites_) c.fence = 0;
    order_.Clear(); sequence_ = 0; staged_slot_ = Count; bound_ = false; peak_timing_.Restart();
    swap_pending_ = history_valid_ = hud_was_drawn_ = staged_hud_ = staged_palette_ = palette_was_drawn_ = false;
    return true;
}
bool D3D12Renderer::RecreateDevice() noexcept {
    if (device_ || !window_) return false;
    if (!CreateGpu()) { ULONG remaining = 0; (void)ReleaseDevice(remaining); return false; }
    // Subtitles are optional here as in Prepare: an atlas that cannot be rebuilt disables them, never the playback.
    if (subtitle_track_ && FAILED(subtitles_.Recreate(device_, queue_))) subtitle_track_ = nullptr;
    // Likewise the command bar: if its pass cannot be rebuilt it is not drawn (Ready() is false), and keys still work.
    if (palette_.Available()) (void)palette_pipeline_.Recreate(device_);
    (void)status_pipeline_.Recreate(device_); // Does nothing without PrepareStatus.
    ++recoveries_; recovery_preview_ = true; recovery_requested_ = health_at_ = InputTime();
    return true;
}
bool D3D12Renderer::Close() noexcept {
    StopPerception(); // Its grid is in the file's arena.
    if (session_) {
        // Warm: what this file needed goes; the device, swap chain, window and passes wait for the next file (or the
        // session's Close). Closing again (the destructor) finds nothing more of its own to release.
        if (device_ && !WaitIdle()) return false;
        if (device_ && history_ && !music_) { // The slots and the history wait for a next file of the same picture size.
            gpu_.ReleaseSlots();
            for (unsigned i = 0; i < Count; ++i) { gpu_.slots[i] = {slots_[i].rgb, slots_[i].allocator, slots_[i].commands}; slots_[i].rgb = nullptr; slots_[i].allocator = nullptr; slots_[i].commands = nullptr; }
            gpu_.history = history_; history_ = nullptr; gpu_.slot_width = capacity_width_; gpu_.slot_height = capacity_height_;
        }
        ReleaseFile(); subtitles_.Release(); subtitles_prepared_.store(false); pending_rebind_.store(nullptr);
        if (window_) {
            gpu_.width = width_; gpu_.height = height_;
            gpu_.status_revision = status_.Revision(); gpu_.palette_revision = palette_.Revision();
            AttachWindow(window_, nullptr);
            gpu_.window = {window_, fullscreen_, restore_}; window_ = nullptr;
        }
        return true;
    }
    if (window_ && keep_window_) {
        // Handed on: the window stops reaching this renderer (its messages go to the default procedure until the next
        // renderer adopts it), and keeps its place, size and fullscreen state.
        AttachWindow(window_, nullptr);
        handoff_ = {window_, fullscreen_, restore_}; window_ = nullptr;
    }
    if (window_) { DestroyEngineWindow(window_); window_ = nullptr; }
    if (input_wake_) { CloseHandle(input_wake_); input_wake_ = nullptr; }
    if (device_ && !WaitIdle()) return false;
    ReleaseGpu(); subtitles_.Release(); palette_pipeline_.Release(); status_pipeline_.Release(); Release(device_);
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    return true;
}
void GpuSession::ReleaseSlots() noexcept {
    for (auto& k : slots) { Release(k.commands); Release(k.allocator); Release(k.rgb); }
    Release(history); slot_width = slot_height = 0;
}
void GpuSession::Close() noexcept {
    // The session's end: the queue idle, then every object, the window last (on the thread that made it).
    if (device && queue && fence && SUCCEEDED(device->GetDeviceRemovedReason())) {
        const auto value = ++sequence;
        if (SUCCEEDED(queue->Signal(fence, value)) && fence->GetCompletedValue() < value) {
            const HANDLE wait = event ? event : CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (wait && SUCCEEDED(fence->SetEventOnCompletion(value, wait))) (void)WaitForSingleObject(wait, 5000);
            if (wait && wait != event) CloseHandle(wait);
        }
    }
    palette.Release(); status.Release(); hud.Release(); visual.Release(); color.Release(); ReleaseSlots();
    for (auto& c : composites) { Release(c.commands); Release(c.allocator); c.fence = 0; }
    Release(blit); Release(blit_root); Release(rtvs);
    for (auto*& b : buffers) Release(b);
    Release(swap); Release(queue); Release(fence); Release(device);
    if (latency) { CloseHandle(latency); latency = nullptr; }
    if (event) { CloseHandle(event); event = nullptr; }
    if (window.window && IsWindow(window.window)) DestroyEngineWindow(window.window);
    window = {}; sequence = 0; width = height = rtv_stride = 0; status_revision = palette_revision = 0;
    (void)arena.Shutdown();
}
}
