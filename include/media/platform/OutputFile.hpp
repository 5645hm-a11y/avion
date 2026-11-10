#pragma once
#include <cstddef>

namespace media::platform {
// A file written from start to end through the system's own calls (CreateFileW and WriteFile on Windows, open and write
// elsewhere): no C library stream, so nothing is allocated while playing (docs/picture.md, "Forensic snapshots"). Paths are
// UTF-8 on every platform. One thread at a time.
class OutputFile final {
public:
    OutputFile() noexcept = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile() noexcept { (void)Close(); }
    // Creates the file; `replace`: an existing one is replaced, else it is left alone and this fails (a free name is sought).
    [[nodiscard]] bool Create(const char* utf8_path, bool replace) noexcept;
    bool Write(const void* bytes, std::size_t count) noexcept;
    // video::SnapshotSink's function: `context` is the OutputFile.
    static bool WriteTo(void* context, const void* bytes, std::size_t count) noexcept { return static_cast<OutputFile*>(context)->Write(bytes, count); }
    // True when it was created and every write went through.
    bool Close() noexcept;
    static bool Remove(const char* utf8_path) noexcept;
private:
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    bool good_ = false;
};
// The default folder for snapshots (UTF-8, ending with its separator): on Windows "Pictures\MediaEngine\" in the user's profile
// (made when missing), elsewhere "" (the working directory). False when it cannot be made.
bool DefaultSnapshotFolder(char* out, std::size_t capacity) noexcept;
}
