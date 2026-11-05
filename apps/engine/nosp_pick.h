#pragma once
// Which GtaThread::Tick signature hit to hook for no_sp_scripts (FiveM JustNoScript), as a pure rule so it can be
// tested without the game. 0.4.20 live (GI, 2026-11-05): on some launches the Legacy pattern matched 3 places in the
// decrypted image for the whole session (`[nethost] gtaThreadTick ambiguous (3)` every poll), the old unique-only rule
// kept "waiting for GtaThread::Tick to decrypt", the hook never installed, the stock scripts ran, startup_positioning
// moved the player to Michael's house and a player switch held the screen (the green, locked spawn). On the launches
// that worked the pattern matched once, at fn RVA 0xa64738. The game binary of a build does not change, so a hit AT
// the RVA proven live is the function; any other ambiguity keeps the old refusal (never hook a guess).
#include <cstddef>
#include <cstdint>

namespace gage::nosp {

constexpr size_t kMaxTickCandidates = 8;

enum class TickPick {
    None,       // no hit (still encrypted, or not this build's code)
    Unique,     // exactly one hit: the old rule
    KnownRva,   // several hits, exactly one at the RVA this build's Tick was proven at
    Ambiguous,  // several hits and no proven RVA among them (or the rule is switched off): refuse
};

struct TickChoice {
    TickPick how = TickPick::None;
    int index = -1;   // into the rvas array; -1 unless Unique / KnownRva
};

// rvas: the function RVA of every hit kept (match + fn offset - image base), n of them; total: how many hits the scan
// counted (it keeps at most kMaxTickCandidates, so total > n means some were not even looked at -> refuse).
// knownRva: 0 = none proven for this build. allowKnown false = nosp_known_rva=0 (the old unique-only rule).
inline TickChoice ChooseTick(const uint64_t* rvas, size_t n, size_t total, uint64_t knownRva, bool allowKnown) {
    TickChoice c;
    if (n == 0) return c;
    if (n == 1 && total == 1) {
        c.how = TickPick::Unique;
        c.index = 0;
        return c;
    }
    c.how = TickPick::Ambiguous;
    if (!allowKnown || knownRva == 0 || total > n) return c;
    int hit = -1;
    for (size_t i = 0; i < n; ++i) {
        if (rvas[i] != knownRva) continue;
        if (hit >= 0) return c;   // the same address twice cannot happen; refuse rather than assume
        hit = (int)i;
    }
    if (hit >= 0) {
        c.how = TickPick::KnownRva;
        c.index = hit;
    }
    return c;
}

inline const char* TickPickName(TickPick p) {
    switch (p) {
    case TickPick::None: return "none";
    case TickPick::Unique: return "unique";
    case TickPick::KnownRva: return "the proven RVA";
    case TickPick::Ambiguous: return "ambiguous";
    }
    return "?";
}

}  // namespace gage::nosp
