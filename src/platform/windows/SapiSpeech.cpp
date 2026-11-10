#include "media/platform/windows/SapiSpeech.hpp"
#include <Windows.h>
#include <sapi.h>
#include <cwchar>

namespace media::platform::windows {
namespace {
// "Microsoft David Desktop" -> "David": the speaker, the same in a voice's desktop and OneCore versions.
void SpeakerOf(const wchar_t* name, wchar_t (&out)[32]) noexcept {
    const wchar_t* first = std::wcschr(name, L' ');
    const wchar_t* begin = first ? first + 1 : name;
    std::size_t n = 0;
    while (begin[n] && begin[n] != L' ' && n + 1 < 32) { out[n] = begin[n]; ++n; }
    out[n] = 0;
}
}
bool SapiSpeech::Open(const wchar_t* const* exclude, unsigned excluded) noexcept {
    Close();
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ = SUCCEEDED(init); // RPC_E_CHANGED_MODE: the thread is already in an apartment, which SAPI works in too.
    const wchar_t* categories[]{L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices"};
    for (const wchar_t* id : categories) {
        ISpObjectTokenCategory* category = nullptr; IEnumSpObjectTokens* tokens = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category)))) continue;
        if (SUCCEEDED(category->SetId(id, FALSE)) && SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens))) {
            ISpObjectToken* token = nullptr;
            while (tokens->Next(1, &token, nullptr) == S_OK) {
                wchar_t speaker[32]{};
                ISpDataKey* attributes = nullptr;
                if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes))) {
                    wchar_t* text = nullptr;
                    if (SUCCEEDED(attributes->GetStringValue(L"Name", &text)) && text) { SpeakerOf(text, speaker); CoTaskMemFree(text); }
                    attributes->Release();
                }
                bool keep = speaker[0] != 0 && count_ < kMaxVoices;
                for (unsigned i = 0; keep && i < count_; ++i) keep = std::wcscmp(names_[i], speaker) != 0;
                for (unsigned i = 0; keep && i < excluded; ++i) keep = std::wcscmp(exclude[i], speaker) != 0;
                if (keep) { tokens_[count_] = token; wcscpy_s(names_[count_], speaker); ++count_; }
                else token->Release();
            }
            tokens->Release();
        }
        category->Release();
    }
    return count_ != 0;
}
void SapiSpeech::Close() noexcept {
    for (unsigned i = 0; i < count_; ++i) if (tokens_[i]) tokens_[i]->Release();
    count_ = 0;
    if (com_) { CoUninitialize(); com_ = false; }
}
std::size_t SapiSpeech::Speak(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept {
    auto& self = *static_cast<SapiSpeech*>(context);
    if (!text || voice >= self.count_ || !out) return 0;
    // The text into a small SAPI XML document: letters, spaces and apostrophes only (nothing to escape).
    wchar_t spoken[64]{};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, spoken, 64) <= 0) return 0;
    for (const wchar_t* c = spoken; *c; ++c) if (*c == L'<' || *c == L'>' || *c == L'&' || *c == L'"') return 0;
    wchar_t xml[128];
    std::swprintf(xml, 128, L"<rate absspeed=\"%d\"/>%ls", rate < 0 ? -3 : rate > 0 ? 3 : 0, spoken);
    WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = 16000; format.wBitsPerSample = 16;
    format.nBlockAlign = 2; format.nAvgBytesPerSec = 32000;
    ISpVoice* engine = nullptr; ISpStream* stream = nullptr; IStream* base = nullptr;
    std::size_t written = 0;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&engine));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &base);
    if (SUCCEEDED(hr)) hr = stream->SetBaseStream(base, SPDFID_WaveFormatEx, &format);
    if (SUCCEEDED(hr)) hr = engine->SetVoice(self.tokens_[voice]);
    if (SUCCEEDED(hr)) hr = engine->SetOutput(stream, TRUE);
    if (SUCCEEDED(hr)) hr = engine->Speak(xml, SPF_IS_XML, nullptr);
    if (SUCCEEDED(hr)) {
        STATSTG stat{}; HGLOBAL global = nullptr;
        if (SUCCEEDED(base->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(base, &global))) {
            if (const auto* pcm = static_cast<const std::int16_t*>(GlobalLock(global))) {
                const auto count = static_cast<std::size_t>(stat.cbSize.QuadPart / 2);
                written = count < capacity ? count : capacity;
                for (std::size_t i = 0; i < written; ++i) out[i] = static_cast<float>(pcm[i]) / 32768.0f;
                GlobalUnlock(global);
            }
        }
    }
    if (stream) stream->Release();
    if (base) base->Release();
    if (engine) engine->Release();
    return written;
}
}
