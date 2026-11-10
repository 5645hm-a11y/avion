// The command bar through the production window procedure: real WM_KEYDOWN / WM_CHAR / WM_UNICHAR and input-method
// messages, in several scripts, on the window's own UI thread.
#include "media/render/D3D12Renderer.hpp"
#include "allocation_probe.hpp"
#include <imm.h>
#include <immdev.h>
#include <cstdio>
#include <cstring>
#include <string_view>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
using media::ui::PaletteHint;
struct Host {
    unsigned submits = 0, previews = 0, keys = 0, toggles = 0;
    char submitted[600]{}, previewed[600]{};
    std::size_t submitted_size = 0, previewed_size = 0;
    bool accept = true;
    std::string_view Submitted() const { return {submitted, submitted_size}; }
    std::string_view Previewed() const { return {previewed, previewed_size}; }
};
bool Submit(void* context, std::string_view text) noexcept {
    auto& h = *static_cast<Host*>(context); ++h.submits;
    h.submitted_size = text.size() < sizeof(h.submitted) ? text.size() : sizeof(h.submitted); std::memcpy(h.submitted, text.data(), h.submitted_size);
    return h.accept;
}
PaletteHint Preview(void* context, std::string_view text, unsigned& matches) noexcept {
    auto& h = *static_cast<Host*>(context); ++h.previews;
    h.previewed_size = text.size() < sizeof(h.previewed) ? text.size() : sizeof(h.previewed); std::memcpy(h.previewed, text.data(), h.previewed_size);
    matches = static_cast<unsigned>(text.size());
    return text.starts_with("pause") ? PaletteHint::Command : PaletteHint::Search;
}
void Key(void* context, unsigned) noexcept { ++static_cast<Host*>(context)->keys; }
bool Hud(void* context, media::ui::HudAction action) noexcept {
    if (action.command == media::ui::HudCommand::Pause || action.command == media::ui::HudCommand::Resume) ++static_cast<Host*>(context)->toggles;
    return true;
}
std::int64_t Now(std::int64_t frequency) {
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart / frequency * 10000000 + t.QuadPart % frequency * 10000000 / frequency;
}
void Type(HWND window, std::wstring_view keys) { for (const wchar_t key : keys) SendMessageW(window, WM_CHAR, key, 0); }
void Control(bool down) {
    BYTE state[256]{}; (void)GetKeyboardState(state); state[VK_CONTROL] = down ? 0x80 : 0; (void)SetKeyboardState(state);
}
// Writes a composition string and a result string into the window's input context, as an input method does before
// it sends WM_IME_COMPOSITION. False when the window has no input context to write to.
bool Compose(HWND window, std::wstring_view composing, std::wstring_view result) {
    const HIMC context = ImmGetContext(window);
    if (!context) return false;
    bool written = false;
    if (auto* input = ImmLockIMC(context)) {
        const auto bytes = static_cast<DWORD>(sizeof(COMPOSITIONSTRING) + (composing.size() + result.size() + 2) * sizeof(wchar_t));
        const HIMCC block = input->hCompStr ? ImmReSizeIMCC(input->hCompStr, bytes) : ImmCreateIMCC(bytes);
        if (block) {
            input->hCompStr = block;
            if (auto* string = static_cast<COMPOSITIONSTRING*>(ImmLockIMCC(block))) {
                std::memset(string, 0, bytes); string->dwSize = bytes;
                string->dwCompStrLen = static_cast<DWORD>(composing.size()); string->dwCompStrOffset = sizeof(COMPOSITIONSTRING);
                std::memcpy(reinterpret_cast<BYTE*>(string) + string->dwCompStrOffset, composing.data(), composing.size() * sizeof(wchar_t));
                string->dwResultStrLen = static_cast<DWORD>(result.size());
                string->dwResultStrOffset = static_cast<DWORD>(sizeof(COMPOSITIONSTRING) + (composing.size() + 1) * sizeof(wchar_t));
                std::memcpy(reinterpret_cast<BYTE*>(string) + string->dwResultStrOffset, result.data(), result.size() * sizeof(wchar_t));
                (void)ImmUnlockIMCC(block); written = true;
            }
        }
        (void)ImmUnlockIMC(context);
    }
    (void)ImmReleaseContext(window, context);
    return written;
}
}
int main() {
    static media::render::D3D12Renderer renderer; static media::core::LinearMemoryArena arena;
    media::clock::PredictiveMasterClock clock; Host host;
    CHECK(arena.Initialize(4 * 1024 * 1024) && renderer.Prepare(960, 540, 2, false)); if (failures) return 1;
    // Before the buffers exist the bar cannot open: `/` is just a key.
    SendMessageW(renderer.Window(), WM_CHAR, L'/', 0); CHECK(!renderer.Palette().IsOpen() && !renderer.Palette().Available());
    CHECK(renderer.PreparePalette(arena) && arena.Seal() && renderer.Palette().Available());
    renderer.ConfigurePalette(Submit, Preview, &host);
    renderer.ConfigureHud(600000000, true, Hud, &host); renderer.ConfigureKeys(Key, &host);
    const auto window = renderer.Window(); const auto& palette = renderer.Palette();
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
    bool paused = false;
    const auto present = [&](unsigned milliseconds) {
        const auto end = Now(f.QuadPart) + static_cast<std::int64_t>(milliseconds) * 10000;
        while (Now(f.QuadPart) < end) {
            CHECK(renderer.Messages());
            CHECK(renderer.Present(clock, Now(f.QuadPart), {false, paused}) != media::render::RenderStatus::Failed);
            Sleep(1);
        }
    };
    present(50);
    const bool ime_closed = ImmGetContext(window) == nullptr; // Closed: no input method is attached to the window.
    allocation_probe::Begin();
    // Closed: keys are playback shortcuts and typed characters are nothing.
    SendMessageW(window, WM_KEYDOWN, 'M', 0); CHECK(host.keys == 1);
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0); CHECK(host.toggles == 1);
    Type(window, L"pause"); CHECK(!palette.IsOpen() && palette.Text().empty() && ime_closed);
    const unsigned keys = host.keys; // Every key press so far reached the application's key handler.
    // `/` opens the bar and is not part of the text. From then on every key types; no shortcut fires.
    SendMessageW(window, WM_CHAR, L'/', 0); CHECK(palette.IsOpen() && palette.Text().empty());
    for (const WPARAM key : {WPARAM{'M'}, WPARAM{'D'}, WPARAM{'F'}, WPARAM{'N'}, WPARAM{'C'}, WPARAM{VK_SPACE}, WPARAM{VK_TAB}, WPARAM{VK_LEFT}, WPARAM{VK_UP}})
        SendMessageW(window, WM_KEYDOWN, key, 0);
    CHECK(host.keys == keys && host.toggles == 1 && !renderer.Fullscreen() && !renderer.ResizePending() && palette.IsOpen());
    Type(window, L"pause /now"); CHECK(palette.Text() == "pause /now"); // A later `/` is text.
    SendMessageW(window, WM_CHAR, 0x08, 0); SendMessageW(window, WM_CHAR, 0x0d, 0); SendMessageW(window, WM_CHAR, 0x1b, 0); // Control characters follow their keys.
    CHECK(palette.Text() == "pause /now" && palette.IsOpen());
    for (unsigned i = 0; i < 5; ++i) SendMessageW(window, WM_KEYDOWN, VK_BACK, i ? LPARAM{1} << 30 : 0); // Backspace, held: it repeats.
    CHECK(palette.Text() == "pause");
    // The bar is drawn, its text rasterised, and the preview asked about the text as typed.
    present(350);
    CHECK(renderer.PalettePresents() > 5 && renderer.PaletteText().Stats().uploads >= 1 && renderer.PaletteText().Stats().failures == 0);
    CHECK(host.previews >= 1 && host.Previewed() == "pause" && palette.Hint() == PaletteHint::Command && palette.Uniforms().visibility == 1);
    CHECK(renderer.PaletteText().Stats().caret > 20);
    // Enter submits; an accepted text closes the bar, which then fades out.
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, LPARAM{1} << 30); CHECK(host.submits == 0); // A held Enter does not submit again.
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(host.submits == 1 && host.Submitted() == "pause" && !palette.IsOpen() && renderer.PaletteSubmits() == 1 && renderer.PaletteRejections() == 0);
    const unsigned drawn = renderer.PalettePresents();
    present(400); CHECK(!palette.Visible() && renderer.PalettePresents() > drawn && palette.Uniforms().visibility == 0);
    const unsigned faded = renderer.PalettePresents(); present(100); CHECK(renderer.PalettePresents() == faded); // Gone: no longer drawn.
    SendMessageW(window, WM_KEYDOWN, 'M', 0); CHECK(host.keys == keys + 1); // Shortcuts are back.
    // Control+K and Control+F open it too (and F does not toggle fullscreen); their control characters are not text.
    Control(true);
    SendMessageW(window, WM_KEYDOWN, 'K', 0); SendMessageW(window, WM_CHAR, 0x0b, 0);
    CHECK(palette.IsOpen() && palette.Text().empty() && host.keys == keys + 1);
    Control(false); SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0); CHECK(!palette.IsOpen() && renderer.Messages()); // Escape closes the bar, not the window.
    Control(true); SendMessageW(window, WM_KEYDOWN, 'F', 0); SendMessageW(window, WM_CHAR, 0x06, 0); Control(false);
    CHECK(palette.IsOpen() && palette.Text().empty() && !renderer.Fullscreen() && !renderer.ResizePending() && host.keys == keys + 1);
    // Any script, one UTF-16 code unit per WM_CHAR; astral characters as surrogate pairs or as one WM_UNICHAR.
    Type(window, L"עצור 暂停 пауза توقف 一時停止 \xd83d\xde00");
    CHECK(palette.Text() == "עצור 暂停 пауза توقف 一時停止 \xf0\x9f\x98\x80");
    CHECK(SendMessageW(window, WM_UNICHAR, UNICODE_NOCHAR, 0) == TRUE);
    SendMessageW(window, WM_UNICHAR, 0x1d11e, 0); SendMessageW(window, WM_UNICHAR, 0x05d0, 0);
    CHECK(palette.Text() == "עצור 暂停 пауза توقف 一時停止 \xf0\x9f\x98\x80\xf0\x9d\x84\x9e\xd7\x90");
    // Control+Backspace deletes a word.
    Control(true); SendMessageW(window, WM_KEYDOWN, VK_BACK, 0); SendMessageW(window, WM_CHAR, 0x7f, 0); Control(false);
    CHECK(palette.Text() == "עצור 暂停 пауза توقف 一時停止 ");
    present(150);
    CHECK(host.Previewed() == palette.Text() && palette.Hint() == PaletteHint::Search && renderer.PaletteText().Stats().right_to_left); // Hebrew first: a right-to-left line.
    // A refused text keeps the bar open and shakes it; Escape then dismisses it.
    host.accept = false; SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(host.submits == 2 && palette.IsOpen() && palette.Uniforms().denied == 1 && renderer.PaletteRejections() == 1 && host.Submitted() == palette.Text());
    present(100); CHECK(palette.IsOpen() && palette.Uniforms().denied > 0 && palette.Uniforms().denied < 1);
    host.accept = true;
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0); CHECK(!palette.IsOpen() && renderer.Messages());
    // Enter on an empty bar just closes it.
    SendMessageW(window, WM_CHAR, L'/', 0); SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0); CHECK(!palette.IsOpen() && host.submits == 2);
    // A click outside the bar dismisses it; a click inside does not, and neither reaches the dock.
    SendMessageW(window, WM_CHAR, L'/', 0);
    const auto& u = palette.Uniforms();
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<WORD>(u.center_x), static_cast<WORD>(u.center_y))); CHECK(palette.IsOpen());
    const auto dock = renderer.HudState();
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<WORD>(dock.center_x), static_cast<WORD>(dock.center_y - 12 * dock.scale)));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<WORD>(dock.center_x), static_cast<WORD>(dock.center_y - 12 * dock.scale)));
    CHECK(!palette.IsOpen() && host.toggles == 1);
    // Input method: attached only while the bar is open. The composition string is shown in the bar (not committed),
    // replaced as it changes, and its result string becomes text.
    SendMessageW(window, WM_CHAR, L'/', 0); Type(window, L"find ");
    const auto allocations_before_ime = allocation_probe::End();
    const bool ime_open = ImmGetContext(window) != nullptr;
    bool ime = false;
    if (ime_open && Compose(window, L"にほ", L"")) {
        ime = true;
        SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
        allocation_probe::Begin();
        SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_CURSORPOS);
        CHECK(palette.Composition() == "にほ" && palette.Text() == "find ");
        const auto step = allocation_probe::End(); CHECK(step == 0);
        CHECK(Compose(window, L"にほんご", L"")); SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
        CHECK(palette.Composition() == "にほんご" && palette.Text() == "find ");
        SendMessageW(window, WM_KEYDOWN, VK_PROCESSKEY, 0); SendMessageW(window, WM_KEYDOWN, VK_BACK, 0); // Keys during a composition belong to the input method.
        CHECK(palette.Composition() == "にほんご" && palette.Text() == "find ");
        present(150); CHECK(host.Previewed() == "find " && renderer.PaletteText().Stats().failures == 0); // The preview sees committed text only.
        // The user picks the candidate: result string, composition emptied.
        CHECK(Compose(window, L"", L"日本語")); SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
        SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
        CHECK(palette.Text() == "find 日本語" && palette.Composition().empty());
        // A result without a composition flag (some input methods), then a cancelled composition.
        CHECK(Compose(window, L"", L" 中文")); SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        CHECK(palette.Text() == "find 日本語 中文");
        CHECK(Compose(window, L"zhong", L"")); SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_COMPSTR); CHECK(palette.Composition() == "zhong");
        CHECK(Compose(window, L"", L"")); SendMessageW(window, WM_IME_COMPOSITION, 0, 0); CHECK(palette.Composition().empty() && palette.Text() == "find 日本語 中文");
        // Losing the focus ends a composition; closing the bar detaches the input method again.
        CHECK(Compose(window, L"か", L"")); SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_COMPSTR); CHECK(palette.Composition() == "か");
        SendMessageW(window, WM_KILLFOCUS, 0, 0); CHECK(palette.Composition().empty() && palette.IsOpen());
        SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0); CHECK(host.submits == 3 && host.Submitted() == "find 日本語 中文" && !palette.IsOpen());
    } else SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(ImmGetContext(window) == nullptr && !palette.IsOpen());
    // Resizing re-lays the bar out; it stays centred and its text is rasterised again for the new lane.
    allocation_probe::Begin();
    SendMessageW(window, WM_CHAR, L'/', 0); Type(window, L"go to 1:30"); present(150);
    const unsigned rasters = renderer.PaletteText().Stats().rasters;
    SetWindowPos(window, nullptr, 0, 0, 1280, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); present(300);
    CHECK(renderer.Width() == 1280 && palette.Uniforms().center_x == 640 && palette.IsOpen() && palette.Text() == "go to 1:30");
    CHECK(renderer.PaletteText().Stats().rasters > rasters && renderer.PaletteText().Stats().width == palette.TextRect().width);
    SendMessageW(window, WM_KEYDOWN, VK_F11, 0); present(300); CHECK(renderer.Fullscreen() && palette.IsOpen()); // F11 still works while typing.
    SendMessageW(window, WM_KEYDOWN, VK_F11, 0); present(300); CHECK(!renderer.Fullscreen());
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0); present(350); CHECK(!palette.Visible());
    const auto allocations = allocation_probe::End() + allocations_before_ime; CHECK(allocations == 0);
    // With the bar closed, Escape closes the window as before.
    CHECK(renderer.Messages()); SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0); CHECK(!renderer.Messages());
    const auto& stats = renderer.PaletteText().Stats();
    CHECK(stats.failures == 0 && stats.rasters >= 6);
    std::printf("Input method: %s; attached while closed: %s\n", ime ? "composition and result strings through WM_IME_COMPOSITION" :
                ime_open ? "SKIPPED (the input context could not be written)" : "SKIPPED (the window has no input context on this system)", ime_closed ? "no" : "YES");
    std::printf("Command bar Win32 routing: %s (%d failures, %u composites with the bar, %u text rasters, %u submits, %llu allocations)\n", failures ? "FAIL" : "PASS",
                failures, renderer.PalettePresents(), stats.rasters, host.submits, static_cast<unsigned long long>(allocations));
    CHECK(renderer.Close());
    return failures ? 1 : 0;
}
