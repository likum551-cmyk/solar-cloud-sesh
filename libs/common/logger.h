#pragma once
#include <string>
#include "common.h"

// Compile-time printf format checking for the logger entry points (MinGW's
// stdio is the C99-conforming one here, so gnu_printf is the right archetype).
#if defined(__GNUC__) && !defined(__clang__)
#define GAGE_PRINTF(fmtIdx, argIdx) __attribute__((format(gnu_printf, fmtIdx, argIdx)))
#elif defined(__GNUC__)
#define GAGE_PRINTF(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#define GAGE_PRINTF(fmtIdx, argIdx)
#endif

namespace gage::log {

    // Point the log file at a specific path (defaults to <ExeDir>gage.log) and
    // open it now. If the path cannot be opened the log falls back to
    // %LOCALAPPDATA%\GAGE-MP\gage.log. A log file larger than 1 MiB is rotated
    // to "<file>.1" when it is opened.
    void init(const std::wstring& path);

    // Write one formatted line to the log file (and to stdout when the process
    // has a valid stdout handle). Thread-safe. Lines are capped at ~2 KiB; longer
    // messages are truncated, never split or over-read.
    void write(const char* fmt, ...) GAGE_PRINTF(1, 2);

    // One-shot convenience.
    void info(const char* fmt, ...) GAGE_PRINTF(1, 2);
    void warn(const char* fmt, ...) GAGE_PRINTF(1, 2);
    void error(const char* fmt, ...) GAGE_PRINTF(1, 2);

}

#define GLOG(...) ::gage::log::write(__VA_ARGS__)
#define GLOGI(...) ::gage::log::info(__VA_ARGS__)
#define GLOGW(...) ::gage::log::warn(__VA_ARGS__)
#define GLOGE(...) ::gage::log::error(__VA_ARGS__)
