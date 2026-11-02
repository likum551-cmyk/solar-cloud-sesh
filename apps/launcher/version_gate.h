#pragma once
// Client and server move in lockstep (tools/deploy/release.sh): this header is the launcher's whole
// policy for it, as pure functions with no Windows, no state and no I/O, so tests/version_gate_test.cpp
// can pin every row. gage_gui.cpp supplies the inputs (the update state, the server's UDP-query protocol,
// the clock) and acts on the answers; docs/UPDATES.md ("Lockstep releases") is the prose version.
//
// Four rules live here:
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
//  4. The update ping (DecideClient, DecideServer): the server's UDP query (format v2) also reports the newest
//     client it was released with. When that is newer than this launcher's kClientVersion, PLAY is refused with
//     code 14 and the update check starts at once (same rate limit), even though the protocol is the same.
//     Unknown (0.0.0, an older server, no latestClientVersion configured), equal and older never trigger anything.

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

    // ---- rule 4: versions as one number -----------------------------------------------------------------------
    // major<<16 | minor<<8 | patch, each 0..255: orders like the version. 0 = unknown. The same packing as
    // proto::query::ClientVersion::Packed() (libs/protocol/query.h); the test asserts they agree.
    inline constexpr uint32_t kUnknownVersion = 0;
    inline constexpr uint32_t PackVersion(unsigned major, unsigned minor, unsigned patch)
    {
        return (major > 255 || minor > 255 || patch > 255) ? kUnknownVersion : ((major << 16) | (minor << 8) | patch);
    }
    // Strict "x.y.z" (three numbers of 1..3 digits, each 0..255, nothing else) -> packed; anything else, or a null
    // pointer, is kUnknownVersion. The launcher feeds it kClientVersion; a client that cannot read its own version
    // is never told to update.
    inline uint32_t ParseVersion(const char* s)
    {
        if (!s) return kUnknownVersion;
        unsigned part[3] = {0, 0, 0};
        unsigned idx = 0, digits = 0;
        for (; *s; ++s) {
            if (*s >= '0' && *s <= '9') {
                if (++digits > 3) return kUnknownVersion;
                part[idx] = part[idx] * 10 + (unsigned)(*s - '0');
                if (part[idx] > 255) return kUnknownVersion;
            } else if (*s == '.' && digits > 0 && idx < 2) {
                ++idx;
                digits = 0;
            } else {
                return kUnknownVersion;
            }
        }
        if (idx != 2 || digits == 0) return kUnknownVersion;
        return PackVersion(part[0], part[1], part[2]);
    }

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

    // ---- rule 4: the update ping ---------------------------------------------------------------------------
    // serverLatest: the newest client the server reported in its v2 UDP query reply (packed; 0 = unknown: an older
    // server answered v1, or none is configured). clientVersion: this launcher's kClientVersion (packed; 0 = could not
    // be read). updatePending: this launcher already knows a newer version.
    //
    //   server latest              update pending   result
    //   unknown (0), or client unknown   any        allow
    //   <= client                  any              allow (an older or equal "latest" never triggers anything)
    //   >  client                  no               refuse, code 14, kTextServerAhead, checkNow
    //   >  client                  yes              refuse, code 14, usePendingText (the update is the cure)
    inline Gate DecideClient(uint32_t serverLatest, uint32_t clientVersion, bool updatePending)
    {
        Gate g;
        if (serverLatest == kUnknownVersion || clientVersion == kUnknownVersion || serverLatest <= clientVersion) return g;
        g.refuse = true;
        g.code = kCodeUpdate;
        g.text = kTextServerAhead;
        g.checkNow = !updatePending;
        g.usePendingText = updatePending;
        return g;
    }

    // Everything one server answer can say: the protocol gate first (a protocol mismatch is the harder fact: ahead
    // and behind keep their codes and texts), then the update ping. This is what the launcher calls.
    inline Gate DecideServer(int serverProtocol, int clientProtocol, uint32_t serverLatest, uint32_t clientVersion,
                             bool updatePending)
    {
        const Gate byProtocol = Decide(serverProtocol, clientProtocol, updatePending);
        if (byProtocol.refuse) return byProtocol;
        return DecideClient(serverLatest, clientVersion, updatePending);
    }

}
