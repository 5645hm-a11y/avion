#pragma once
#include "media/session/MediaSession.hpp"
#include <cstddef>
#include <cstdint>

namespace media::platform::windows {
// The session's files on Windows (docs/session.md). Called between files (Preparing or Stopped), never while playing.
// The ledger's usual place, %LOCALAPPDATA%\MediaEngine\resume.bin (the folder is created). False: no such variable, or
// `capacity` too small.
bool DefaultLedgerPath(wchar_t* out, std::size_t capacity) noexcept;
// False (and an empty ledger): no file, a damaged one, or not a ledger.
bool LoadLedger(const wchar_t* path, session::ResumeLedger&) noexcept;
// Written beside the ledger, then moved over it, unflushed: a crash leaves the old ledger, the new one, or one whose
// checksum fails and loads empty; never a wrong one.
bool SaveLedger(const wchar_t* path, const session::ResumeLedger&) noexcept;
// A local file's identity in the ledger: the key of its full path, and its size. False: not a local file.
bool FileIdentity(const wchar_t* path, std::uint64_t& key, std::uint64_t& bytes) noexcept;
// The playable files of the folder `file` is in, sorted (session::Playlist); `folder` receives that folder with its
// trailing separator, `index` the file's place in the list (-1: not playable, or not there). False: the folder cannot
// be listed, or a path is longer than `capacity`.
bool ListFolder(const wchar_t* file, session::Playlist&, wchar_t* folder, std::size_t capacity, int& index) noexcept;
}
