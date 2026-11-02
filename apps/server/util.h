// Small portable helpers shared by the server's modules (UTC timestamps, atomic
// file writes, JSON string escaping, IPv4 parsing). C++17, std only, no exceptions.
//
// Thread-safety: every function here is re-entrant and touches no shared state, so
// any of them may be called from any thread (the announcer's thread uses JsonEscape;
// the Poll thread uses the rest). The two file functions race only with other
// writers of the same path, as any file access does.
#pragma once

#include <cstdint>
#include <string>

namespace gage::server {

    // The current time as "2026-10-29 08:34:56.789Z" (UTC, millisecond precision): the
    // log line prefix. Hand-rolled civil-from-days so it needs neither localtime nor
    // gmtime (not thread-safe, platform variants). Total (never fails).
    std::string LogTimestamp();
    // The current time as "2026-10-29T08:34:56Z" (UTC, ISO-8601), for files (the "at"
    // field of bans.json). Total.
    std::string IsoTimestamp();

    // Replaces `path` with `content` atomically: the bytes go to "<path>.tmp", are
    // flushed to disk, and the temp file is renamed over `path` (an overwriting
    // rename on both Windows and POSIX), so a crash leaves the old file or the new
    // one, never half of either. `path` must be non-empty and its directory must
    // exist. Returns true on success. Returns false on any failure (empty path, the
    // temp file cannot be created or written, the rename fails) with a one-line reason
    // in *err when err is non-null; the temp file is removed and `path` is untouched.
    bool AtomicWriteFile(const std::string& path, const std::string& content, std::string* err);

    // Reads the whole file `path` (binary, no newline translation) into `out`.
    // Returns true on success. Returns false when it cannot, with a reason (no path in
    // it: "no such file", "is a directory", "cannot open (permission denied?)", "read
    // error") in *err when err is non-null. *missing (when non-null) is set true ONLY
    // when the path does not exist, and false for every other failure, so callers can
    // treat "missing" as "start from nothing" without also doing so for a file that
    // is there but unreadable. `out` is unspecified after a failure.
    bool ReadWholeFile(const std::string& path, std::string& out, std::string* err, bool* missing = nullptr);

    // `s` as the inside of a JSON string literal (no surrounding quotes): escapes
    // " \ and control characters; bytes >= 0x80 pass through, so the input must
    // already be valid UTF-8. Total.
    std::string JsonEscape(const std::string& s);

    // `s` without leading and trailing space, tab, CR and LF. Total.
    std::string Trim(const std::string& s);

    // Strict dotted-quad IPv4: four decimal fields 0..255, no leading zeros, nothing
    // else (so the accepted text is already the canonical form). Returns true when `s`
    // is one; on success *host (when non-null) is the address as a host-order integer
    // (a.b.c.d = a<<24|b<<16|c<<8|d). Returns false (leaving *host alone) for anything
    // else: names, IPv6, short forms, whitespace, "01.2.3.4", "256.0.0.1".
    bool ParseIpv4(const std::string& s, uint32_t* host = nullptr);

    // CPU time (user + kernel) consumed so far by the CALLING thread, in seconds, or 0
    // when the platform cannot say. GetThreadTimes on Windows (100 ns units, but the
    // kernel only charges it per scheduler quantum, so read it over a window of at least a
    // second), clock_gettime(CLOCK_THREAD_CPUTIME_ID) elsewhere. Total.
    double ThreadCpuSeconds();

}
