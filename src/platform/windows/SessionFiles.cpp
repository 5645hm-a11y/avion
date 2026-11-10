#include "media/platform/windows/SessionFiles.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>

namespace media::platform::windows {
namespace {
constexpr DWORD kPathUnits = 4096;
}
bool DefaultLedgerPath(wchar_t* out, std::size_t capacity) noexcept {
    if (!out || capacity < 64) return false;
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, static_cast<DWORD>(capacity));
    constexpr wchar_t kFolder[] = L"\\MediaEngine", kFile[] = L"\\resume.bin";
    if (!n || n + std::size(kFolder) + std::size(kFile) > capacity) return false;
    std::wmemcpy(out + n, kFolder, std::size(kFolder));
    if (!CreateDirectoryW(out, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::wmemcpy(out + n + std::size(kFolder) - 1, kFile, std::size(kFile));
    return true;
}
bool LoadLedger(const wchar_t* path, session::ResumeLedger& ledger) noexcept {
    ledger.Clear();
    std::byte bytes[session::ResumeLedger::kFileBytes];
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; DWORD read = 0;
    // This version's size, or version 1's (no bookmarks yet): Load tells them apart.
    const bool sized = GetFileSizeEx(file, &size) && (size.QuadPart == static_cast<LONGLONG>(sizeof(bytes)) ||
                                                      size.QuadPart == static_cast<LONGLONG>(session::ResumeLedger::kVersion1Bytes));
    const auto expected = sized ? static_cast<DWORD>(size.QuadPart) : 0u;
    const bool whole = sized && ReadFile(file, bytes, expected, &read, nullptr) && read == expected;
    CloseHandle(file);
    return whole && ledger.Load({bytes, expected});
}
bool SaveLedger(const wchar_t* path, const session::ResumeLedger& ledger) noexcept {
    std::byte bytes[session::ResumeLedger::kFileBytes];
    wchar_t temporary[kPathUnits];
    const auto length = std::wcslen(path);
    if (!ledger.Save(bytes) || length + 5 > kPathUnits) return false;
    std::wmemcpy(temporary, path, length); std::wmemcpy(temporary + length, L".new", 5);
    const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    // Not flushed: a save between two files must not cost the flush's 15 to 55 ms. After a power cut the ledger is the
    // old one, the new one, or bytes whose checksum fails (an empty ledger): never a wrong one.
    const bool whole = WriteFile(file, bytes, static_cast<DWORD>(sizeof(bytes)), &written, nullptr) && written == sizeof(bytes);
    CloseHandle(file);
    if (!whole) { DeleteFileW(temporary); return false; }
    if (!MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(temporary); return false; }
    return true;
}
bool FileIdentity(const wchar_t* path, std::uint64_t& key, std::uint64_t& bytes) noexcept {
    key = bytes = 0;
    if (!path || _wcsnicmp(path, L"http://", 7) == 0 || _wcsnicmp(path, L"https://", 8) == 0) return false;
    wchar_t full[kPathUnits];
    const DWORD n = GetFullPathNameW(path, kPathUnits, full, nullptr);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!n || n >= kPathUnits || !GetFileAttributesExW(full, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    key = session::ResumeLedger::Key(full, n);
    bytes = (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
    return true;
}
bool ListFolder(const wchar_t* file, session::Playlist& list, wchar_t* folder, std::size_t capacity, int& index) noexcept {
    list.Clear(); index = -1;
    wchar_t* name = nullptr;
    const DWORD n = GetFullPathNameW(file, static_cast<DWORD>(capacity), folder, &name);
    if (!n || n >= capacity || !name || name == folder) return false;
    wchar_t own[kPathUnits]; const auto own_length = std::wcslen(name);
    if (own_length + 1 > kPathUnits) return false;
    std::wmemcpy(own, name, own_length + 1);
    *name = 0; // `folder` ends with its separator now.
    const auto length = static_cast<std::size_t>(name - folder);
    if (length + 2 > capacity) return false;
    folder[length] = L'*'; folder[length + 1] = 0;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileExW(folder, FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    folder[length] = 0;
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        (void)list.Add(found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    list.Sort();
    index = list.Find({own, own_length});
    return true;
}
}
