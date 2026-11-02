// script_suppress_test -- proves the singleplayer script tables in apps/engine/sp_scripts.h are
// internally sound and, above all, SAFE: the list of story scripts the engine terminates to
// silence the campaign voice/text bleed must never contain a session-root script. Terminating
// main_persistent would kill our own script host; terminating main / startup would tear down the
// singleplayer session we run inside. A typo there would crash or freeze the game on every join,
// so it is worth a gate.
//
// This includes the real header the engine uses, so the test checks the shipped data, not a copy.
// joaat()'s correctness against the live game is established elsewhere (the script tick only goes
// live when joaat("main_persistent") matches a running thread); here we check table integrity.
//
// Build target: script_suppress_test (selftest/, never shipped).

#include "../apps/engine/sp_scripts.h"

#include <cstdio>
#include <cstring>

namespace sp = gage::engine::spscripts;

static int g_run = 0, g_fail = 0;
static void Check(const char* name, bool ok) {
    ++g_run;
    if (!ok) ++g_fail;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", name);
}

static bool listContains(const char* const* list, size_t n, const char* s) {
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(list[i], s) == 0) return true;
    return false;
}

int main() {
    // joaat is the lowercased Jenkins hash: case-insensitive and deterministic.
    Check("joaat is case-insensitive", sp::Joaat("Main_Persistent") == sp::Joaat("main_persistent"));
    Check("joaat is deterministic", sp::Joaat("prologue1") == sp::Joaat("prologue1"));
    Check("joaat distinguishes distinct names", sp::Joaat("main") != sp::Joaat("main_persistent"));

    size_t nKnown = 0;
    const char* const* known = sp::KnownNames(nKnown);
    size_t nStory = 0;
    const char* const* story = sp::StoryScriptsToTerminate(nStory);
    size_t nRoot = 0;
    const char* const* roots = sp::SessionRootNames(nRoot);

    Check("known-name table is non-empty", nKnown > 0);
    Check("story terminate list is non-empty", nStory > 0);
    Check("session-root list is non-empty", nRoot > 0);

    // No hash collisions within the diagnostic table: LookupName must be unambiguous, or the log
    // would mislabel one script as another.
    bool noCollision = true;
    for (size_t i = 0; i < nKnown; ++i)
        for (size_t j = i + 1; j < nKnown; ++j)
            if (sp::Joaat(known[i]) == sp::Joaat(known[j])) {
                std::printf("     collision: %s and %s -> 0x%08x\n", known[i], known[j],
                            sp::Joaat(known[i]));
                noCollision = false;
            }
    Check("no hash collisions in the known-name table", noCollision);

    // Every known name round-trips through LookupName (no name is empty / unreachable).
    bool roundTrip = true;
    for (size_t i = 0; i < nKnown; ++i) {
        if (!known[i] || !known[i][0]) { roundTrip = false; continue; }
        const char* back = sp::LookupName(sp::Joaat(known[i]));
        if (!back || std::strcmp(back, known[i]) != 0) {
            std::printf("     no round-trip: %s\n", known[i]);
            roundTrip = false;
        }
    }
    Check("every known name round-trips through LookupName", roundTrip);

    // THE SAFETY INVARIANT: the terminate list must be disjoint from the session roots.
    bool safe = true;
    for (size_t i = 0; i < nStory; ++i)
        if (listContains(roots, nRoot, story[i])) {
            std::printf("     DANGER: terminate list contains session root %s\n", story[i]);
            safe = false;
        }
    Check("terminate list never contains a session-root script", safe);

    // The roots we most care about are actually listed as roots (so the invariant has teeth).
    Check("main_persistent is a session root", listContains(roots, nRoot, "main_persistent"));
    Check("main is a session root", listContains(roots, nRoot, "main"));
    Check("startup is a session root", listContains(roots, nRoot, "startup"));

    // The terminate list has no empty entries and no duplicates.
    bool clean = true;
    for (size_t i = 0; i < nStory; ++i) {
        if (!story[i] || !story[i][0]) { clean = false; continue; }
        for (size_t j = i + 1; j < nStory; ++j)
            if (std::strcmp(story[i], story[j]) == 0) {
                std::printf("     duplicate in terminate list: %s\n", story[i]);
                clean = false;
            }
    }
    Check("terminate list has no empty or duplicate entries", clean);

    // prologue1 -- the prime suspect for a fresh-boot Story Mode voice/text bleed -- is targeted.
    Check("prologue1 is in the terminate list", listContains(story, nStory, "prologue1"));

    // The post-entry hijack list obeys the same invariant: never a session root, no empty/duplicate.
    size_t nHijack = 0;
    const char* const* hijack = sp::PostEntryHijackers(nHijack);
    Check("hijack list is non-empty", nHijack > 0);
    bool hijackSafe = true;
    for (size_t i = 0; i < nHijack; ++i) {
        if (!hijack[i] || !hijack[i][0] || listContains(roots, nRoot, hijack[i])) {
            std::printf("     DANGER: bad hijack entry %s\n", hijack[i] ? hijack[i] : "(null)");
            hijackSafe = false;
        }
        for (size_t j = i + 1; j < nHijack; ++j)
            if (std::strcmp(hijack[i], hijack[j]) == 0) hijackSafe = false;
    }
    Check("hijack list never contains a session root, no empty/duplicate", hijackSafe);
    Check("player_controller is in the hijack list", listContains(hijack, nHijack, "player_controller"));

    std::printf(g_fail ? "\nRESULT: %d of %d FAILED\n" : "\nRESULT: all %d passed\n",
                g_fail ? g_fail : g_run, g_run);
    return g_fail ? 1 : 0;
}
