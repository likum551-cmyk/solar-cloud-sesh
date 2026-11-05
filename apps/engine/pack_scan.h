// pack_scan.h: the byte-pattern scanner shared by pack_stream.cpp (Legacy mount), pack_enh.cpp (Enhanced Round A
// diagnostic) and tests/pack_profile_test.cpp (which runs it over the offline Enhanced image dump).
// Pure C++17 (no OS calls): a "run" is any readable byte range, so the same code scans the live game image and a file.
//
// Pattern syntax: hex bytes separated by single spaces, "??" = any byte (rel32 / disp32 / imm operands).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gage::game::content::scan {

struct Run { const uint8_t* p; size_t n; };

inline bool ParsePat(const char* s, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char* tok = s;
        while (*tok && *tok != ' ') ++tok;
        const int len = (int)(tok - s);
        if (len == 2 && s[0] == '?' && s[1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
        } else if (len == 2 && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) {
            bytes.push_back((uint8_t)(hexv(s[0]) * 16 + hexv(s[1])));
            mask.push_back(1);
        } else {
            return false;
        }
        s = tok;
    }
    return !bytes.empty();
}

// Count matches (capped at 1000); `first` receives the lowest-address one. -1 = malformed pattern.
inline int CountPat(const std::vector<Run>& runs, const char* sig, const uint8_t** first) {
    std::vector<uint8_t> bytes, mask;
    if (first) *first = nullptr;
    if (!ParsePat(sig, bytes, mask)) return -1;
    const size_t L = bytes.size();
    size_t lead = 0;
    while (lead < L && !mask[lead]) ++lead;
    if (lead == L) return -1;              // all wildcards
    int count = 0;
    for (const Run& r : runs) {
        if (r.n < L) continue;
        const uint8_t* p = r.p + lead;
        const uint8_t* last = r.p + r.n - (L - lead);
        while (p <= last) {
            p = static_cast<const uint8_t*>(memchr(p, bytes[lead], (size_t)(last - p) + 1));
            if (!p) break;
            const uint8_t* s = p - lead;
            bool ok = true;
            for (size_t i = 0; i < L; ++i)
                if (mask[i] && s[i] != bytes[i]) { ok = false; break; }
            if (ok) {
                if (first && !*first) *first = s;
                if (++count >= 1000) return count;
            }
            ++p;
        }
    }
    return count;
}

}  // namespace gage::game::content::scan
