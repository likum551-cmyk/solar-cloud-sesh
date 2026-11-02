#pragma once
// GAGE-MP settings file.
//
// A small INI-style file (gage_mp.cfg) written next to the executable so the
// last-used game path and server are visible, editable, and move with the
// install folder. This is the user-facing "app cfg / settings file". The
// registry (registry.h) remains the in-process handoff for the injected chain
// (gage_mp.dll reads server_host/port after injection).
//
// Thread safety: every call takes one process-wide lock for its whole
// read-modify-write, and the file is replaced atomically (temp file + rename),
// so concurrent Set/SetList/SetMany calls never lose each other's keys and a
// reader never sees a half-written file. Two *processes* writing at once are
// not serialised (last writer wins), but still never corrupt the file.
//
// File format: "[gage]" then one "key=value" per line, UTF-8. Lines starting
// with ';' or '#' are comments; whitespace around keys and values is ignored
// (so a value cannot carry leading/trailing spaces). There is no escaping:
// values have CR, LF and NUL replaced by a
// space on write, and keys that are empty, contain '=', CR, LF or NUL, start
// with '[', ';', '#' or whitespace, or end in whitespace are rejected.

#include <string>
#include <utility>
#include <vector>

namespace gage::cfg {

    // <exedir>gage_mp.cfg
    std::wstring Path();

    // Read one value (`def` when absent or the file is unreadable). Values live
    // under [gage].
    std::wstring Get(const std::wstring& key, const std::wstring& def = L"");

    // Set one value and rewrite the file. Other keys are preserved. False (and a
    // logged error) when the key is invalid or the file could not be written.
    bool Set(const std::wstring& key, const std::wstring& value);

    // Set many values with a single read-modify-write (one temp file, one
    // rename). All-or-nothing: an invalid key rejects the whole batch.
    bool SetMany(const std::vector<std::pair<std::wstring, std::wstring>>& kv);

    // Ordered list storage (favorites, recents, ...). Backed by
    // "<key>.count" = N and "<key>_0" .. "<key>_<N-1>". Stale entries beyond
    // the new length are removed so the file stays self-consistent. Lists are
    // limited to 4096 entries (longer input is truncated, a larger stored count
    // is clamped).
    std::vector<std::wstring> GetList(const std::wstring& key);
    bool SetList(const std::wstring& key, const std::vector<std::wstring>& values);

}
