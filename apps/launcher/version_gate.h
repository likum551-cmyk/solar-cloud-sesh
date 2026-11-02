#pragma once
// Client and server move in lockstep (tools/deploy/release.sh): this header is the launcher's whole
// policy for it, as pure functions with no Windows, no state and no I/O, so tests/version_gate_test.cpp
// can pin every row. gage_gui.cpp supplies the inputs (the update state, the server's UDP-query protocol,
// the clock) and acts on the answers; docs/UPDATES.md ("Lockstep releases") is the prose version.
//
// Three rules live here:
//
//  1. EVERY available update is mandatory (UpdateIsMandatory): PLAY is refused until the launcher runs the
//     newest published version, whatever the manifest's `mandatory` flag or the "install automatically"
//     setting say, and the download starts at once.
//  2. The protocol gate (Decide): a reachable server reports the protocol it enforces in its UDP query.
//     When that differs from the launcher's compiled gage::proto::kVersion, PLAY is refused with a sentence
//     the player can act on, before the game is started.
//  3. When the launcher looks for an update (kUpdatePollMs, CheckDue, BeforePlay): every 2 minutes while it
//     is open, and on the events that matter (window brought forward, PLAY pressed, the game exited, a
//     server that is ahead of us seen), never more often than once per kMinCheckGapMs.

#include <cstdint>

namespace gage::vgate {

    // ---- timing -------------------------------------------------------------------------------------
    // How often an open launcher (tray included) looks for a new release.
    inline constexpr unsigned kUpdatePollMs = 2 * 60 * 1000;
    // The event-driven checks (window activated, PLAY, game exit, server protocol ahead of us) never start a
    // check closer than this to the previous one. The periodic poll is >= this, so it is never throttled.
    inline constexpr unsigned kMinCheckGapMs = 60 * 1000;
    // PLAY waits at most this long for a check it started (an unreachable update host must not stall PLAY
    // for the 15 s the HTTP fetch may take); the check carries on and the usual update path applies if it
    // finds something.
    inline constexpr unsigned kLaunchCheckWaitMs = 8000;

    // True when a check may start: none ever has, or the last one started at least minGapMs ago. nowMs and
    // lastStartMs are the same monotonic clock (GetTickCount64). A clock that went backwards counts as due.
    inline bool CheckDue(uint64_t nowMs, uint64_t lastStartMs, bool everStarted, uint64_t minGapMs = kMinCheckGapMs)
    {
        if (!everStarted) return true;
        if (nowMs < lastStartMs) return true;
        return nowMs - lastStartMs >= minGapMs;
    }

    // ---- rule 1: every available update is mandatory -------------------------------------------------
    // A verified manifest names a newer version (`available`): it is mandatory, whatever the signed
    // `mandatory` flag says. The flag stays in the manifest format (older launchers read it); this
    // launcher no longer lets it decide anything. Nothing available: nothing to be mandatory.
    inline bool UpdateIsMandatory(bool available, bool /*manifestMandatory*/) { return available; }

    // ---- what PLAY does about updates before it starts the game ---------------------------------------
    // Go                   start the launch now
    // WaitForRunningCheck  a check is already in flight (startup, the poll): wait for it, then decide
    // CheckThenGo          none is running and one is due: start it, wait for it (kLaunchCheckWaitMs at most), decide
    enum class PlayStep { Go, WaitForRunningCheck, CheckThenGo };
    inline PlayStep BeforePlay(bool checkRunning, bool canStartCheck, bool due)
    {
        if (checkRunning) return PlayStep::WaitForRunningCheck;
        if (canStartCheck && due) return PlayStep::CheckThenGo;
        return PlayStep::Go;
    }

    // ---- rule 2: the protocol gate -------------------------------------------------------------------
    // launchResult codes (apps/launcher/BRIDGE.md): 14 = an update stands in the way, 15 = the server is
    // behind this launcher.
    inline constexpr long kCodeUpdate = 14;
    inline constexpr long kCodeServerBehind = 15;

    // Player-facing sentences. kTextServerBehind is the same sentence the server sends a newer client in its
    // Refuse(VersionMismatch) (proto::kRefuseTextClientNew; the test compares them).
    inline constexpr const wchar_t* kTextServerAhead = L"Solar RP was updated. Updating the launcher...";
    inline constexpr const wchar_t* kTextServerBehind = L"This server has not been updated yet. Try again in a minute.";

    struct Gate {
        bool refuse = false;           // PLAY must not start the game
        bool checkNow = false;         // look for a launcher update now (the caller applies kMinCheckGapMs)
        bool usePendingText = false;   // an update is already on its way: show ITS text, not `text`
        long code = 0;                 // launchResult code when refusing
        const wchar_t* text = L"";     // what the player reads when refusing
    };

    // serverProtocol: the protocol the server reported in its UDP query (<= 0: unknown, the server did not
    // answer or predates the field). clientProtocol: gage::proto::kVersion compiled into this launcher.
    // updatePending: this launcher already knows a newer version (downloading, waiting for the game to close).
    //
    //   server protocol            update pending   result
    //   unknown (<= 0)             any              allow (the server's own VersionMismatch is the backstop)
    //   == client                  any              allow
    //   >  client (server ahead)   no               refuse, code 14, kTextServerAhead, checkNow
    //   >  client (server ahead)   yes              refuse, code 14, usePendingText (the update is the cure)
    //   <  client (server behind)  any              refuse, code 15, kTextServerBehind
    inline Gate Decide(int serverProtocol, int clientProtocol, bool updatePending)
    {
        Gate g;
        if (serverProtocol <= 0 || clientProtocol <= 0 || serverProtocol == clientProtocol) return g;
        g.refuse = true;
        if (serverProtocol > clientProtocol) {
            g.code = kCodeUpdate;
            g.text = kTextServerAhead;
            g.checkNow = !updatePending;
            g.usePendingText = updatePending;
        } else {
            g.code = kCodeServerBehind;
            g.text = kTextServerBehind;
        }
        return g;
    }

}
