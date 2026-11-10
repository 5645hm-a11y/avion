#pragma once
// media_probe and the failure texts (API 15): shared by CApi.cpp (the exports) and Probe.cpp (the reader).
#include "media/c_api.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace media::sdk {
// `text` NUL-terminated within `bytes`, a cut never leaving a partial UTF-8 sequence.
void CutUtf8(char* text, std::size_t bytes) noexcept;
// UTF-8 to wide characters (UTF-16 on Windows, UTF-32 elsewhere), NUL-terminated within `capacity`; false: invalid or too long.
bool Utf8ToWide(const char* utf8, wchar_t* out, std::size_t capacity) noexcept;
// A UTF-8 path for reading (wide on Windows), or null.
std::FILE* OpenRead(const char* utf8_path) noexcept;
media_status Probe(const char* path, media_probe_t& info) noexcept;
// What an open's or a playback's failure stage means, in words (windowed engine or host mode: their stages differ).
const char* StageText(bool host, std::uint32_t stage) noexcept;
}
