// Filesystem and text helpers of the scripting runtime. C++17, std only, no exceptions
// (every std::filesystem call uses its std::error_code overload).
//
// Thread-safety: every function is re-entrant and touches no shared state; the runtime
// itself only calls them from the server tick thread.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gage::scripting::fsutil {

    // Reads the whole file `path` (binary) into `out`. A leading UTF-8 byte-order mark is
    // NOT removed (callers that parse text strip it with StripBom). Returns true on success.
    // Returns false with a one-line reason in *err (when non-null): "no such file", "is a
    // directory", "cannot open (permission denied?)", "read error" or "file is larger than
    // <maxBytes> bytes". *missing (when non-null) is set true ONLY when the path does not
    // exist, so "missing" can mean "start from nothing" without also covering a file that is
    // there but unreadable. `out` is unspecified after a failure.
    bool ReadFile(const std::string& path, std::string& out, std::string* err, bool* missing = nullptr,
                  size_t maxBytes = static_cast<size_t>(64) * 1024 * 1024);

    // Replaces `path` with `content` atomically: the bytes go to "<path>.tmp", are flushed to
    // disk, and the temp file is renamed over `path` (an overwriting rename on both Windows
    // and POSIX), so a crash leaves the old file or the new one, never half of either. The
    // directory of `path` must exist. Returns true on success. On failure returns false with
    // a one-line reason in *err (when non-null), removes the temp file and leaves `path`
    // untouched.
    bool AtomicWrite(const std::string& path, const std::string& content, std::string* err);

    // Creates `dir` and its parents when missing. True when the directory exists afterwards.
    bool EnsureDir(const std::string& dir, std::string* err);

    // The names of the immediate subdirectories of `dir`, sorted bytewise. False (with *err)
    // when `dir` cannot be listed; a missing `dir` yields an empty list and returns true so
    // "no resources folder yet" is not an error.
    bool ListSubdirs(const std::string& dir, std::vector<std::string>& names, std::string* err);

    // True when `p` exists and is a regular file (not a directory, not a symlink target of
    // either; symlinks are judged by ResolveInside, not here).
    bool IsRegularFile(const std::string& p);

    // Lexical check of a path that must stay inside a folder: non-empty, at most 200 bytes,
    // forward slashes only, every segment a non-empty name that is not "." or "..", no
    // drive letters or colons, no backslashes, no control characters, not absolute.
    // `rel` is UTF-8.
    bool IsSafeRelPath(const std::string& rel);

    // Resolves `rel` (see IsSafeRelPath) below the directory `rootDir` to a path for a regular
    // file that exists. Returns false with a reason in *err for: an unsafe path, a missing
    // file, anything that is not a regular file, a symbolic link or Windows junction anywhere
    // below `rootDir` (resources may not smuggle a link out of their folder), or a resolved
    // location outside `rootDir`. On success *out is the path to open.
    bool ResolveInside(const std::string& rootDir, const std::string& rel, std::string* out, std::string* err);

    // Resource names: 1..64 of [A-Za-z0-9_-], not a Windows device name (con, nul, com1...).
    bool ValidResourceName(const std::string& name);

    // True when [p, p+n) is well-formed UTF-8 (no overlongs, surrogates or code points above
    // U+10FFFF). Control characters are allowed here.
    bool Utf8Valid(const char* p, size_t n);

    // `s` without a leading UTF-8 byte-order mark.
    std::string StripBom(const std::string& s);

    // Cuts `s` to at most `maxBytes` without splitting a UTF-8 sequence.
    std::string Utf8Truncate(const std::string& s, size_t maxBytes);

    // `s` with C0 control characters (other than \n and \t) and DEL replaced by '?', so text a
    // script prints cannot inject terminal escape sequences into the server log.
    std::string SanitizeForLog(const std::string& s);

}
