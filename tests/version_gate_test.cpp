// version_gate_test -- the launcher's lockstep policy (apps/launcher/version_gate.h), every row.
//
// The header is pure (no Windows, no state), so this includes the real file the launcher includes:
// the test checks the shipped decisions, not a copy. What it pins:
//   * the protocol gate's decision table (server ahead / behind / equal / unknown, update pending or not)
//   * that EVERY available update is mandatory, whatever the manifest's `mandatory` flag says
//   * the check timing: the 2-minute poll, the 60-second minimum gap between event-driven checks, and a
//     simulated hour of events showing the gap holds and the poll is never throttled by it
//   * what PLAY does about a running / due / not-due check
//   * that the launcher's "server is behind" sentence is the one the server sends a newer client
//   * the update ping: a server whose query reports a latest client newer than the launcher's own refuses PLAY
//     with code 14 and starts the update check (same text, same rate limit), also for an unchanged protocol;
//     unknown (0.0.0, an older server), equal and older never do; the protocol gate keeps precedence
//
// Build target: version_gate_test (selftest/, never shipped). Classified `gate` in
// tools/deploy/wine-selftests.list: it touches nothing wine implements differently from Windows.

#include "../apps/launcher/version_gate.h"
#include "protocol/protocol.h"
#include "protocol/query.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace vg = gage::vgate;
namespace proto = gage::proto;

static int g_run = 0, g_fail = 0;
static void Check(const char* name, bool ok) {
    ++g_run;
    if (!ok) ++g_fail;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", name);
}

static bool Same(const wchar_t* a, const wchar_t* b) { return std::wcscmp(a, b) == 0; }
static std::wstring Widen(const char* s) { std::wstring w; for (; *s; ++s) w.push_back((wchar_t)(unsigned char)*s); return w; }

int main() {
    const int kClient = proto::kVersion;

    // ---- protocol gate: the table -----------------------------------------------------------------------
    for (bool pending : { false, true }) {
        const vg::Gate eq = vg::Decide(kClient, kClient, pending);
        Check(pending ? "equal protocols: allowed (update pending)" : "equal protocols: allowed",
              !eq.refuse && !eq.checkNow && !eq.usePendingText && eq.code == 0);
        const vg::Gate unk = vg::Decide(-1, kClient, pending);
        const vg::Gate zero = vg::Decide(0, kClient, pending);
        Check(pending ? "server protocol unknown (-1 and 0): allowed, the server's own refusal is the backstop (update pending)"
                      : "server protocol unknown (-1 and 0): allowed, the server's own refusal is the backstop",
              !unk.refuse && !unk.checkNow && !zero.refuse && !zero.checkNow);
        const vg::Gate noClient = vg::Decide(kClient, 0, pending);
        Check(pending ? "client protocol unknown (0): allowed (update pending)" : "client protocol unknown (0): allowed",
              !noClient.refuse);
        const vg::Gate behind = vg::Decide(kClient - 1, kClient, pending);
        Check(pending ? "server behind: refused with code 15 and the 'not updated yet' sentence (update pending)"
                      : "server behind: refused with code 15 and the 'not updated yet' sentence",
              behind.refuse && behind.code == 15 && !behind.checkNow && !behind.usePendingText &&
                  Same(behind.text, L"This server has not been updated yet. Try again in a minute."));
    }
    const vg::Gate ahead = vg::Decide(kClient + 1, kClient, false);
    Check("server ahead, no update known: refused with code 14, 'Updating the launcher...', and a check is requested",
          ahead.refuse && ahead.code == 14 && ahead.checkNow && !ahead.usePendingText &&
              Same(ahead.text, L"Solar RP was updated. Updating the launcher..."));
    const vg::Gate aheadPending = vg::Decide(kClient + 1, kClient, true);
    Check("server ahead, update already pending: refused with code 14, the pending-update text is used, no new check",
          aheadPending.refuse && aheadPending.code == 14 && !aheadPending.checkNow && aheadPending.usePendingText);
    Check("server far ahead (+7) is the same as one ahead", vg::Decide(kClient + 7, kClient, false).code == 14 &&
                                                                  vg::Decide(kClient + 7, kClient, false).checkNow);
    Check("server far behind (1) is the same as one behind", vg::Decide(1, kClient, false).code == 15);
    Check("the launcher's own constants: 14 = update, 15 = server behind", vg::kCodeUpdate == 14 && vg::kCodeServerBehind == 15);

    // ---- the sentences ----------------------------------------------------------------------------------
    Check("launcher 'server is behind' sentence == the server's Refuse text for a newer client (proto::kRefuseTextClientNew)",
          Same(vg::kTextServerBehind, Widen(proto::kRefuseTextClientNew).c_str()));
    Check("launcher sentences fit a launchResult line (<= 150) and differ",
          std::wcslen(vg::kTextServerAhead) <= 150 && std::wcslen(vg::kTextServerBehind) <= 150 &&
              !Same(vg::kTextServerAhead, vg::kTextServerBehind));
    Check("the server's two refusal texts differ and each is non-empty",
          std::strlen(proto::kRefuseTextClientOld) > 20 && std::strlen(proto::kRefuseTextClientNew) > 20 &&
              std::strcmp(proto::kRefuseTextClientOld, proto::kRefuseTextClientNew) != 0);

    // ---- every available update is mandatory -----------------------------------------------------------------
    Check("available + manifest mandatory=false: mandatory (the rule that forces updates)", vg::UpdateIsMandatory(true, false));
    Check("available + manifest mandatory=true: mandatory", vg::UpdateIsMandatory(true, true));
    Check("not available + mandatory=false: not mandatory", !vg::UpdateIsMandatory(false, false));
    Check("not available + mandatory=true (a stale flag): not mandatory", !vg::UpdateIsMandatory(false, true));

    // ---- timing constants -------------------------------------------------------------------------------
    Check("poll = 2 minutes", vg::kUpdatePollMs == 120000u);
    Check("minimum gap between event-driven checks = 60 s", vg::kMinCheckGapMs == 60000u);
    Check("the poll is never throttled by the gap (poll >= gap)", vg::kUpdatePollMs >= vg::kMinCheckGapMs);
    Check("PLAY never waits for a check longer than the HTTP fetch's own 15 s", vg::kLaunchCheckWaitMs > 0 && vg::kLaunchCheckWaitMs < 15000u);

    // ---- CheckDue ---------------------------------------------------------------------------------------
    Check("CheckDue: never started -> due", vg::CheckDue(0, 0, false));
    Check("CheckDue: 0 ms after a start -> not due", !vg::CheckDue(5000, 5000, true));
    Check("CheckDue: 59.999 s after a start -> not due", !vg::CheckDue(60000 + 59999, 60000, true));
    Check("CheckDue: exactly 60 s after a start -> due", vg::CheckDue(60000 + 60000, 60000, true));
    Check("CheckDue: an hour after -> due", vg::CheckDue(3600000, 1000, true));
    Check("CheckDue: the clock went backwards -> due (never stuck)", vg::CheckDue(1000, 900000, true));
    Check("CheckDue: a custom gap is honoured", !vg::CheckDue(9999, 0, true, 10000) && vg::CheckDue(10000, 0, true, 10000));
    Check("CheckDue: 64-bit uptime (50 days in ms) does not overflow", vg::CheckDue(50ull * 86400 * 1000 + 60000, 50ull * 86400 * 1000, true));

    // ---- simulated hour: events at odd times + the 2-minute poll -----------------------------------------------
    {
        // Event times in ms: window activations, PLAY presses, game exits, server-ahead sightings, all piled up.
        const uint64_t events[] = { 0, 3000, 11000, 59000, 59999, 60000, 61000, 119000, 130000, 130500, 200000,
                                    239999, 240000, 240001, 600000, 659999, 660000, 3599000 };
        uint64_t last = 0;
        bool started = false;
        int starts = 0;
        uint64_t prevStart = 0;
        bool gapOk = true;
        for (uint64_t t : events) {
            if (vg::CheckDue(t, last, started)) {
                if (started && t - prevStart < vg::kMinCheckGapMs) gapOk = false;
                started = true; last = t; prevStart = t; ++starts;
            }
        }
        Check("simulated events: no two checks start closer than 60 s", gapOk);
        // Checks start at 0, 60000, 130000, 200000, 600000, 660000, 3599000; the other 11 events fall inside a gap
        // (59999 and 659999 by 1 ms, 240000 and 239999 by 20 s).
        Check("simulated events: 18 events collapse to the expected 7 checks", starts == 7);

        // The 2-minute poll with the gap: every tick starts a check, even right after an event-driven one.
        uint64_t l2 = 0; bool s2 = false; int polls = 0;
        for (uint64_t t = 0; t <= 3600000; t += vg::kUpdatePollMs) {
            if (vg::CheckDue(t, l2, s2)) { s2 = true; l2 = t; ++polls; }
        }
        Check("the poll alone starts a check on every tick (31 in an hour)", polls == 31);
        // An event 1 s before each poll tick starts the check (it is due); the tick itself then falls inside the
        // gap. The check cadence stays one per period: the gap never starves the poll.
        uint64_t l3 = 0, prev3 = 0, maxInterval = 0; bool s3 = false; int total = 0;
        for (uint64_t t = 0; t <= 3600000; t += vg::kUpdatePollMs) {
            const uint64_t times[2] = { t >= 1000 ? t - 1000 : t, t };
            for (uint64_t at : times) {
                if (vg::CheckDue(at, l3, s3)) {
                    if (s3 && at - prev3 > maxInterval) maxInterval = at - prev3;
                    s3 = true; l3 = at; prev3 = at; ++total;
                }
            }
        }
        Check("an event 1 s before each poll tick does not starve the poll: 31 checks in an hour, none more than 2 min apart",
              total == 31 && maxInterval <= vg::kUpdatePollMs && maxInterval >= vg::kMinCheckGapMs);
    }

    // ---- PLAY ----------------------------------------------------------------------------------------------
    Check("PLAY: a check is running -> wait for it", vg::BeforePlay(true, false, true) == vg::PlayStep::WaitForRunningCheck &&
                                                      vg::BeforePlay(true, true, false) == vg::PlayStep::WaitForRunningCheck);
    Check("PLAY: idle and a check is due -> check, then go", vg::BeforePlay(false, true, true) == vg::PlayStep::CheckThenGo);
    Check("PLAY: idle but the last check was < 60 s ago -> go at once", vg::BeforePlay(false, true, false) == vg::PlayStep::Go);
    Check("PLAY: a check cannot start (download / install under way) and none is running -> go (the update refusal comes first)",
          vg::BeforePlay(false, false, true) == vg::PlayStep::Go);

    // ---- rule 4: the update ping (query format v2: the server reports the newest client it was released with) ----
    {
        namespace q = gage::proto::query;
        const uint32_t kMe = vg::ParseVersion("0.4.3");          // this launcher
        const uint32_t kNew = vg::ParseVersion("0.4.4");         // the server's latest client
        Check("versions: ParseVersion packs strict x.y.z like the query codec's ClientVersion (0.4.3 = 0x000403) and orders like the version",
              kMe == 0x000403 && kNew == 0x000404 && kMe == (q::ClientVersion{0, 4, 3}).Packed() && kNew == (q::ClientVersion{0, 4, 4}).Packed() &&
                  vg::ParseVersion("1.0.0") > vg::ParseVersion("0.255.255") && vg::ParseVersion("0.10.0") > vg::ParseVersion("0.9.9") &&
                  vg::PackVersion(1, 2, 3) == 0x010203);
        Check("versions: ParseVersion refuses anything but x.y.z with parts 0..255 (unknown = 0, never an update)",
              vg::ParseVersion(nullptr) == 0 && vg::ParseVersion("") == 0 && vg::ParseVersion("0.4") == 0 && vg::ParseVersion("0.4.4.1") == 0 &&
                  vg::ParseVersion("256.0.0") == 0 && vg::ParseVersion("a.b.c") == 0 && vg::ParseVersion(" 0.4.4") == 0 &&
                  vg::ParseVersion("0.4.4-rc1") == 0 && vg::ParseVersion("0..4") == 0 && vg::ParseVersion("0.0.0") == 0 &&
                  vg::PackVersion(256, 0, 0) == 0);

        // The table.
        for (bool pending : { false, true }) {
            const vg::Gate newer = vg::DecideClient(kNew, kMe, pending);
            Check(pending ? "update ping: server latest 0.4.4 > client 0.4.3: refused with code 14, the pending-update text, no new check"
                          : "update ping: server latest 0.4.4 > client 0.4.3: refused with code 14 and 'Updating the launcher...', and a check is requested",
                  newer.refuse && newer.code == 14 && newer.usePendingText == pending && newer.checkNow == !pending &&
                      Same(newer.text, L"Solar RP was updated. Updating the launcher..."));
            const vg::Gate unknown = vg::DecideClient(0, kMe, pending);
            Check(pending ? "update ping: server latest unknown (0.0.0: an old server, or none configured): allowed (update pending)"
                          : "update ping: server latest unknown (0.0.0: an old server, or none configured): allowed",
                  !unknown.refuse && !unknown.checkNow && !unknown.usePendingText && unknown.code == 0);
            const vg::Gate same = vg::DecideClient(kMe, kMe, pending);
            const vg::Gate older = vg::DecideClient(vg::ParseVersion("0.4.2"), kMe, pending);
            const vg::Gate muchOlder = vg::DecideClient(vg::ParseVersion("0.0.1"), kMe, pending);
            Check(pending ? "update ping: server latest == client, or older (0.4.2, 0.0.1): allowed, no check (update pending)"
                          : "update ping: server latest == client, or older (0.4.2, 0.0.1): allowed, no check",
                  !same.refuse && !same.checkNow && !older.refuse && !older.checkNow && !muchOlder.refuse && !muchOlder.checkNow);
        }
        Check("update ping: a launcher that cannot read its own version (0) is never told to update, whatever the server reports",
              !vg::DecideClient(kNew, 0, false).refuse && !vg::DecideClient(0xFFFFFF, 0, false).refuse);
        Check("update ping: the next patch, minor and major release all count (0.4.4, 0.5.0, 1.0.0 vs 0.4.3); 0.4.3 vs 0.4.3 and 0.3.9 do not",
              vg::DecideClient(vg::ParseVersion("0.4.4"), kMe, false).refuse && vg::DecideClient(vg::ParseVersion("0.5.0"), kMe, false).refuse &&
                  vg::DecideClient(vg::ParseVersion("1.0.0"), kMe, false).refuse && !vg::DecideClient(vg::ParseVersion("0.3.9"), kMe, false).refuse &&
                  !vg::DecideClient(kMe, kMe, false).refuse);

        // DecideServer = protocol gate first, then the ping. The protocol rows must be exactly the old Decide.
        const int P = (int)proto::kVersion;
        for (bool pending : { false, true }) {
            const vg::Gate sameProto = vg::DecideServer(P, P, kNew, kMe, pending);
            Check(pending ? "server answer: same protocol but a newer latest client: refused with code 14 (update pending: its text, no new check)"
                          : "server answer: same protocol but a newer latest client (the 0.4.3-style same-protocol release): refused with code 14, check now",
                  sameProto.refuse && sameProto.code == 14 && sameProto.checkNow == !pending && sameProto.usePendingText == pending);
            const vg::Gate old = vg::DecideServer(P, P, 0, kMe, pending);
            Check(pending ? "server answer: same protocol, old server (latest unknown): allowed, exactly as before the update ping (update pending)"
                          : "server answer: same protocol, old server (latest unknown): allowed, exactly as before the update ping",
                  !old.refuse && !old.checkNow && old.code == 0);
            const vg::Gate behind = vg::DecideServer(P - 1, P, kNew, kMe, pending);
            Check(pending ? "server answer: protocol behind keeps code 15 even if the latest client is newer (update pending)"
                          : "server answer: protocol behind keeps code 15 even if the latest client is newer (the protocol gate wins)",
                  behind.refuse && behind.code == 15 && !behind.checkNow && Same(behind.text, vg::kTextServerBehind));
            const vg::Gate ahead = vg::DecideServer(P + 1, P, 0, kMe, pending);
            Check(pending ? "server answer: protocol ahead keeps code 14 and its text, latest unknown (update pending)"
                          : "server answer: protocol ahead keeps code 14 and its text, latest unknown",
                  ahead.refuse && ahead.code == 14 && ahead.checkNow == !pending && ahead.usePendingText == pending);
        }
        Check("server answer: no answer (protocol -1, latest 0): allowed",
              !vg::DecideServer(-1, P, 0, kMe, false).refuse && !vg::DecideServer(0, P, 0, kMe, true).refuse);

        // The rate limit is the existing one: the update ping asks for a check the same way a protocol-ahead server
        // does (Gate::checkNow), and the caller applies CheckDue/kMinCheckGapMs. A server queried every 5 s that keeps
        // reporting a newer client therefore starts at most one check a minute: simulate 10 minutes of 5-second answers.
        {
            uint64_t last = 0;
            bool started = false;
            int checks = 0;
            bool gapOk = true;
            uint64_t prev = 0;
            for (uint64_t t = 0; t <= 10 * 60 * 1000; t += 5000) {
                const vg::Gate g = vg::DecideServer(P, P, kNew, kMe, false);
                if (g.checkNow && vg::CheckDue(t, last, started)) {
                    if (started && t - prev < vg::kMinCheckGapMs) gapOk = false;
                    started = true; last = t; prev = t; ++checks;
                }
            }
            Check("update ping: a server reporting a newer client on every 5 s answer starts one check per minute (11 in 10 minutes), never closer than 60 s",
                  gapOk && checks == 11);
            // ...and once the update is pending no further check is requested at all.
            int pendingChecks = 0;
            for (uint64_t t = 0; t <= 10 * 60 * 1000; t += 5000)
                if (vg::DecideServer(P, P, kNew, kMe, true).checkNow) ++pendingChecks;
            Check("update ping: with an update already pending no check is requested by the ping", pendingChecks == 0);
        }
    }

    std::printf("CHECKS %d FAILED %d\n", g_run, g_fail);
    if (g_run < 60) { std::printf("FAIL  expected at least 60 checks, ran %d\n", g_run); return 1; }
    return g_fail == 0 ? 0 : 1;
}
