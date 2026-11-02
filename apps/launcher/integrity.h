#pragma once
// The launcher's check of its own injection chain: every DLL it loads into
// the game must hash (SHA-256) to the value baked in at build time
// (gen/chain_hashes.h, tools/integrity.cmake). A DLL changed after the build
// (a swapped-in cheat, a damaged install) is never injected.

#include "common/common.h"

namespace gage::integrity {

    struct ChainFile {
        const wchar_t* name;  // file name in the launcher's folder
        const char* sha256;   // 64 lowercase hex
    };

    // SHA-256 of a file as 64 lowercase hex; "" when it cannot be read.
    std::string Sha256File(const std::wstring& path);

    // Every file in `files` exists in `dir` (ends in a backslash) and hashes
    // to its value. False sets *bad to the first file that does not.
    bool VerifyFiles(const std::wstring& dir, const ChainFile* files, int count, std::wstring* bad);

    // VerifyFiles over the build's own chain (gen/chain_hashes.h).
    bool VerifyChain(const std::wstring& dir, std::wstring* bad);

}
