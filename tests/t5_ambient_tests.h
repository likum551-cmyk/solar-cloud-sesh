// T5 ambient (desync overhaul round 2, symptom 4): the tests of apps/engine/ambient.h.
//   t5::RunPure(report)   pure logic: scheduler, exemption list, sweep planner, breaker, tally, switches, the
//                         10 s line, and a source-order guard on engine.cpp. Run by ambient_test.
//   t5::RunWorld(report)  (only with T5_WITH_WORLD, i.e. inside gage_mp_test) the real World wrappers on a
//                         test exe with NO native table: unresolved hashes are counted, hash-0 natives are inert.
// Each check says what it proves; every absence check is paired with a presence check that proves the code
// path ran. The old behaviour (suppression only behind join::IsConnected() + the entry, nothing on the render-
// thread fallback, no counters) is the fixture OldGate / the counters that did not exist.
#pragma once

#include "../apps/engine/ambient.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef T5_WITH_WORLD
#include "../apps/engine/hook.h"
#endif

namespace t5 {

namespace amb = gage::ambient;
using Report = void (*)(const char* name, bool ok, const std::string& got);

constexpr int kPureChecks = 57;
constexpr int kWorldChecks = 10;

inline std::string S(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string S(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

// The pre-T5 rule, exactly: the density multipliers ran only on the script tick, after EnterServer() returned
// true (a session up AND the entry Done) and with a spawn mode other than 0. The render-thread fallback never.
inline bool OldGate(bool inScript, int spawnMode, bool connected, bool entryDone)
{
    const bool mayChange = inScript ? spawnMode != 0 : spawnMode == 1;
    return inScript && mayChange && connected && entryDone;
}

inline amb::Ctx Script(uint64_t nowMs, bool player = true, bool sweep = false)
{
    amb::Ctx c;
    c.inScript = true;
    c.spawnMode = -1;
    c.nativesUp = true;
    c.havePlayer = player;
    c.sweepAvailable = sweep;
    c.nowMs = nowMs;
    return c;
}

inline std::string ReadAll(const std::string& path)
{
    std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline int RunPure(Report check, const std::string& repoDir)
{
    int n = 0;
    auto C = [&](const char* name, bool ok, const std::string& got = "") { ++n; check(name, ok, got); };

    // ---- the gate: the suppression no longer depends on the connection ------------------------------------------
    {
        // A session timeline at 60 ticks/s: offline 10 s, connecting+entry 10 s, connected 20 s, session ended 10 s
        // (also: join retries), connected again 10 s. The engine feeds the scheduler a Ctx that has no connection
        // field at all; the link below is only the diagnostics label (LinkOf), exactly as TickAmbient does.
        amb::Scheduler sch;
        int ticks[4] = {}, frames[4] = {}, oldFrames[4] = {}, total = 0;
        uint64_t t = 0;
        auto phase = [&](bool connected, bool entryDone, bool ever, int seconds) {
            for (int i = 0; i < seconds * 60; ++i, t += 16)
            {
                const amb::Link l = amb::LinkOf(connected, entryDone, ever);
                const amb::Plan p = sch.Next(Script(t));
                ++ticks[(int)l];
                ++total;
                if (p.frame) ++frames[(int)l];
                if (OldGate(true, -1, connected, entryDone)) ++oldFrames[(int)l];
            }
        };
        phase(false, false, false, 10);   // Offline
        phase(true, false, true, 10);     // Entering
        phase(true, true, true, 20);      // Connected
        phase(false, false, true, 10);    // Ended / retrying
        phase(true, true, true, 10);      // Connected again
        C("gate: ticks ran in all four connection states (presence)",
          ticks[0] > 0 && ticks[1] > 0 && ticks[2] > 0 && ticks[3] > 0,
          S("%d/%d/%d/%d", ticks[0], ticks[1], ticks[2], ticks[3]));
        C("gate: the density multipliers run on EVERY tick of every state",
          frames[0] == ticks[0] && frames[1] == ticks[1] && frames[2] == ticks[2] && frames[3] == ticks[3],
          S("%d/%d/%d/%d of %d/%d/%d/%d", frames[0], frames[1], frames[2], frames[3], ticks[0], ticks[1], ticks[2], ticks[3]));
        C("gate: the old rule covered only the connected+entered ticks (the regression this fixes)",
          oldFrames[(int)amb::Link::Offline] == 0 && oldFrames[(int)amb::Link::Entering] == 0 &&
          oldFrames[(int)amb::Link::Ended] == 0 && oldFrames[(int)amb::Link::Connected] == ticks[(int)amb::Link::Connected] &&
          total > ticks[(int)amb::Link::Connected],
          S("old frames %d/%d/%d/%d", oldFrames[0], oldFrames[1], oldFrames[2], oldFrames[3]));
        C("gate: LinkOf maps the four states",
          amb::LinkOf(false, false, false) == amb::Link::Offline && amb::LinkOf(true, false, false) == amb::Link::Entering &&
          amb::LinkOf(true, true, true) == amb::Link::Connected && amb::LinkOf(false, true, true) == amb::Link::Ended);
    }

    // ---- the render-thread fallback and the diagnostic modes ------------------------------------------------------
    {
        amb::Scheduler sch;
        amb::Ctx r = Script(0, false);
        r.inScript = false;
        const amb::Plan p = sch.Next(r);
        C("render fallback: the per-frame multipliers run", p.frame && p.skip == amb::Skip::None);
        C("render fallback: nothing needing the script thread's context runs (periodic / clear / sweep)",
          !p.periodic && !p.clear && !p.sweep);
        C("render fallback: the old rule never ran there", !OldGate(false, -1, true, true));
        amb::Switches off;
        off.renderFallback = false;
        amb::Scheduler sch2(off);
        const amb::Plan q = sch2.Next(r);
        C("render fallback: ambient_render=0 turns it off and says why", !q.frame && q.skip == amb::Skip::RenderOff);
        amb::Ctx s = Script(100);
        const amb::Plan ps = sch2.Next(s);
        C("render fallback off does not affect the script tick", ps.frame);

        amb::Scheduler sch3;
        amb::Ctx z = Script(0);
        z.spawnMode = 0;
        const amb::Plan pz = sch3.Next(z);
        C("spawn=0 (patch nothing) runs nothing", !pz.frame && !pz.periodic && pz.skip == amb::Skip::SpawnZero);
        amb::Ctx nn = Script(0);
        nn.nativesUp = false;
        const amb::Plan pn = sch3.Next(nn);
        C("no native table: nothing runs and the skip reason is NativesDown", !pn.frame && pn.skip == amb::Skip::NativesDown);
        amb::Switches mo;
        mo.master = false;
        amb::Scheduler sch4(mo);
        const amb::Plan pm = sch4.Next(Script(0));
        C("ambient=0: nothing runs", !pm.frame && !pm.periodic && !pm.clear && pm.skip == amb::Skip::MasterOff);
        amb::Ctx r1 = Script(0);
        r1.spawnMode = 1;
        C("spawn=1 on the script tick still suppresses", sch3.Next(r1).frame);
    }

    // ---- cadences ----------------------------------------------------------------------------------------------------
    {
        amb::Scheduler sch;
        int per = 0, clr = 0, swp = 0, frm = 0, ticks = 0;
        std::vector<uint64_t> perAt;
        for (uint64_t t = 1000; t < 11000; t += 16)
        {
            const amb::Plan p = sch.Next(Script(t, true, true));
            ++ticks;
            if (p.frame) ++frm;
            if (p.periodic) { ++per; perAt.push_back(t); }
            if (p.clear) ++clr;
            if (p.sweep) ++swp;
        }
        C("cadence: a frame every tick over 10 s", frm == ticks && ticks > 600, S("%d/%d", frm, ticks));
        C("cadence: periodic switches every 2 s (5 in 10 s), the first one on the first tick", per == 5 && perAt[0] == 1000,
          S("%d first %llu", per, (unsigned long long)perAt[0]));
        C("cadence: area clear every 3 s (4 in 10 s)", clr == 4, S("%d", clr));
        C("cadence: sweep every 1 s (10 in 10 s) when the enumeration exists", swp == 10, S("%d", swp));
        amb::Scheduler sch2;
        int swp2 = 0, per2 = 0;
        for (uint64_t t = 0; t < 5000; t += 16)
        {
            const amb::Plan p = sch2.Next(Script(t, true, false));
            if (p.sweep) ++swp2;
            if (p.periodic) ++per2;
        }
        C("cadence: no sweep while the enumeration hash is missing (and the rest of the plan still runs)", swp2 == 0 && per2 >= 2);
        amb::Scheduler sch3;
        int pp = 0, ff = 0, cc = 0;
        for (uint64_t t = 0; t < 5000; t += 16)
        {
            const amb::Plan p = sch3.Next(Script(t, false, true));
            if (p.periodic || p.clear || p.sweep) ++pp;
            if (p.frame) ++cc;
            ++ff;
        }
        C("cadence: no player yet -> only the per-frame multipliers", pp == 0 && cc == ff && ff > 100, S("%d %d/%d", pp, cc, ff));
        amb::Scheduler sch4;
        sch4.Next(Script(50000));
        const amb::Plan back = sch4.Next(Script(100));
        C("cadence: the clock going backwards makes the next one due (no stall)", back.periodic && back.clear);
        amb::Switches nc;
        nc.areaClear = false;
        amb::Scheduler sch5(nc);
        int c5 = 0, s5 = 0;
        for (uint64_t t = 0; t < 4000; t += 16)
        {
            const amb::Plan p = sch5.Next(Script(t, true, true));
            if (p.clear) ++c5;
            if (p.sweep) ++s5;
        }
        C("cadence: ambient_clear=0 stops only the area clear", c5 == 0 && s5 >= 3, S("clear %d sweep %d", c5, s5));
    }

    // ---- the circuit breaker ---------------------------------------------------------------------------------------------
    {
        amb::Scheduler sch;
        C("breaker: no loss -> not tripped (presence of the check, absence of the trip)",
          !sch.CheckProtected(7, 7) && !sch.BreakerTripped() && sch.ExemptLost() == 0);
        C("breaker: more alive after than before (a spawn between) does not trip", !sch.CheckProtected(7, 8));
        const bool tripped = sch.CheckProtected(7, 5);
        C("breaker: an exempt handle gone after a clean-up trips it and counts the loss",
          tripped && sch.BreakerTripped() && sch.ExemptLost() == 2 && sch.Trips() == 1);
        int clr = 0, swp = 0, per = 0, frm = 0;
        for (uint64_t t = 0; t < 10000; t += 16)
        {
            const amb::Plan p = sch.Next(Script(t, true, true));
            if (p.clear) ++clr;
            if (p.sweep) ++swp;
            if (p.periodic) ++per;
            if (p.frame) ++frm;
        }
        C("breaker: tripped -> no more area clears and no sweeps, but the density multipliers and switches go on",
          clr == 0 && swp == 0 && per >= 4 && frm > 500, S("clear %d sweep %d periodic %d frames %d", clr, swp, per, frm));
        sch.CheckProtected(9, 3);
        C("breaker: sticky (a second loss does not re-arm it)", sch.BreakerTripped() && sch.Trips() == 1 && sch.ExemptLost() == 8);
    }

    // ---- the exemption list and the sweep planner -------------------------------------------------------------------------
    {
        amb::ExemptSet ex;
        ex.Add(0);
        ex.Add(-1);
        ex.Add(100);
        ex.Add(100);
        ex.Add(200);
        C("exempt: 0, -1 and duplicates are not stored", ex.Size() == 2 && ex.Contains(100) && ex.Contains(200) && !ex.Contains(0));
        C("exempt: an INCOMPLETE list lets nothing be deleted (cannot prove anything)", !ex.MayDelete(300) && !ex.Complete());
        ex.SetComplete(true);
        C("exempt: a complete list lets an unknown handle be deleted (presence: the predicate is not just false)", ex.MayDelete(300));
        C("exempt: a listed handle, 0 and -1 are never deletable", !ex.MayDelete(100) && !ex.MayDelete(200) && !ex.MayDelete(0) && !ex.MayDelete(-1));
        ex.Clear();
        C("exempt: Clear empties the list and drops the completeness", ex.Size() == 0 && !ex.Complete() && !ex.MayDelete(300));

        amb::ExemptSet e2;
        for (int32_t h : { 11, 12, 13, 14 }) e2.Add(h);
        e2.SetComplete(true);
        const int32_t found[] = { 11, 50, 12, 51, 50, 0, -1, 13, 52, 53, 54 };
        std::vector<int32_t> plan;
        const amb::SweepStats st = amb::PlanSweep(found, 11, e2, 16, plan);
        C("sweep: plans exactly the non-exempt, valid, unique handles",
          plan == std::vector<int32_t>({ 50, 51, 52, 53, 54 }) && st.planned == 5, S("planned %d", st.planned));
        C("sweep: counts the protected, the invalid (0, -1, duplicate) and the seen",
          st.exempt == 3 && st.invalid == 3 && st.seen == 11, S("exempt %d invalid %d seen %d", st.exempt, st.invalid, st.seen));
        const amb::SweepStats cp = amb::PlanSweep(found, 11, e2, 2, plan);
        C("sweep: the per-sweep cap binds (2 planned, 3 left for the next sweep)", plan.size() == 2 && cp.capped == 3 && cp.planned == 2,
          S("planned %d capped %d", cp.planned, cp.capped));
        amb::ExemptSet e3;
        e3.Add(11);   // not marked complete
        const amb::SweepStats inc = amb::PlanSweep(found, 11, e3, 16, plan);
        C("sweep: an incomplete list plans NOTHING but still protects what it knows (presence: handles were seen)",
          plan.empty() && inc.planned == 0 && inc.seen == 11 && inc.exempt == 1 && inc.unproven == 7, S("unproven %d", inc.unproven));
        C("sweep: no input -> nothing", amb::PlanSweep(nullptr, 0, e2, 16, plan).planned == 0 && plan.empty());

        // Property: over 2000 random worlds no planned handle is ever on the list, and every exempt handle that was
        // enumerated was counted as protected.
        uint32_t rng = 12345;
        auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) & 0xFFFF; };
        bool leak = false, short_ = false;
        long enumeratedExempt = 0, countedExempt = 0;
        for (int round = 0; round < 2000; ++round)
        {
            amb::ExemptSet e;
            std::vector<int32_t> in;
            for (int i = 0; i < 12; ++i) e.Add((int32_t)(rnd() % 40) + 1);
            e.SetComplete(true);
            for (int i = 0; i < 30; ++i) in.push_back((int32_t)(rnd() % 60));
            std::vector<int32_t> out;
            const amb::SweepStats s = amb::PlanSweep(in.data(), (int)in.size(), e, 64, out);
            for (int32_t h : out) if (e.Contains(h) || h == 0) leak = true;
            for (size_t i = 0; i < in.size(); ++i)
            {
                bool dup = false;
                for (size_t j = 0; j < i; ++j) if (in[j] == in[i]) dup = true;
                if (!dup && in[i] != 0 && e.Contains(in[i])) ++enumeratedExempt;
            }
            countedExempt += s.exempt;
            if (s.planned + s.exempt + s.invalid + s.capped + s.unproven != s.seen) short_ = true;
        }
        C("sweep property (2000 random worlds): no exempt or zero handle is ever planned", !leak && enumeratedExempt > 1000,
          S("exempt enumerated %ld", enumeratedExempt));
        C("sweep property: every enumerated exempt handle was counted protected; the buckets add up to what was seen",
          countedExempt == enumeratedExempt && !short_, S("%ld vs %ld", countedExempt, enumeratedExempt));
    }

    // ---- reading the enumeration array --------------------------------------------------------------------------------------
    {
        uint64_t a[8] = { 3, 0x1111, 0x2222, 0xFFFFFFFF00003333ull, 0, 0, 0, 0 };
        int32_t out[7] = {};
        C("nearby: count in slot 0, one handle per 8-byte slot, low 32 bits only",
          amb::ParseNearby(a, 7, out) == 3 && out[0] == 0x1111 && out[1] == 0x2222 && out[2] == 0x3333);
        uint64_t b[4] = { 9, 1, 2, 3 };
        C("nearby: a count above the capacity rejects the whole array (a wrong layout never feeds a delete)",
          amb::ParseNearby(b, 3, out) == -1);
        uint64_t c[4] = { (uint64_t)(uint32_t)-4, 1, 2, 3 };
        C("nearby: a negative count rejects the array", amb::ParseNearby(c, 3, out) == -1);
        uint64_t d[2] = { 0, 77 };
        C("nearby: zero found -> 0, nothing read", amb::ParseNearby(d, 1, out) == 0 && out[0] == 0x1111);
        C("nearby: null / zero capacity are rejected", amb::ParseNearby(nullptr, 3, out) == -1 && amb::ParseNearby(a, 0, out) == -1);
    }

    // ---- counters and switches -----------------------------------------------------------------------------------------------
    {
        amb::Tally t;
        const bool f1 = t.Note(amb::Id::PedDensity, amb::Result::Ok);
        const bool f2 = t.Note(amb::Id::PedDensity, amb::Result::Failed, 3);
        t.Note(amb::Id::PedBudget, amb::Result::Inert);
        C("tally: the first note of a native says so once (the first-use log line), later ones do not", f1 && !f2);
        C("tally: ok / failed / inert are counted apart",
          t.ok[(int)amb::Id::PedDensity] == 1 && t.failed[(int)amb::Id::PedDensity] == 3 && t.inert[(int)amb::Id::PedBudget] == 1 &&
          t.failed[(int)amb::Id::PedBudget] == 0 && t.Total(amb::Id::PedDensity) == 4);
        const amb::Tally prev = t;
        t.Note(amb::Id::PedDensity, amb::Result::Failed, 2);
        const amb::Tally d = amb::Delta(t, prev);
        C("tally: Delta is the window since the previous report", d.failed[(int)amb::Id::PedDensity] == 2 && d.ok[(int)amb::Id::PedDensity] == 0 &&
          d.Total(amb::Id::PedBudget) == 0);
        C("sentinel ids are exactly the natives with no hash (budgets, generators area, trains, the two enumerations)",
          amb::IsSentinel(amb::Id::PedBudget) && amb::IsSentinel(amb::Id::VehBudget) && amb::IsSentinel(amb::Id::GeneratorsArea) &&
          amb::IsSentinel(amb::Id::DeleteTrains) && amb::IsSentinel(amb::Id::NearbyPeds) && amb::IsSentinel(amb::Id::NearbyVehs) &&
          !amb::IsSentinel(amb::Id::PedDensity) && !amb::IsSentinel(amb::Id::ClearPeds) && !amb::IsSentinel(amb::Id::DeleteEntity));

        C("switches: only an exact 0 / 1 decides, anything else is the default",
          !amb::Flag(std::string("0"), true) && amb::Flag(std::string("1"), false) && amb::Flag(std::string(""), true) &&
          !amb::Flag(std::string(""), false) && amb::Flag(std::string("2"), true) && amb::Flag(std::string("00"), true) &&
          !amb::Flag(std::wstring(L"0"), true));
        std::map<std::string, std::string> reg = { { "ambient_clear", "0" }, { "ambient_render", "1" } };
        const amb::Switches sw = amb::LoadSwitches([&](const char* k) { return reg.count(k) ? reg[k] : std::string(); });
        C("switches: LoadSwitches reads the four registry names, missing ones default on",
          sw.master && sw.renderFallback && !sw.areaClear && sw.sweep);
    }

    // ---- the 10 s line --------------------------------------------------------------------------------------------------------------
    {
        amb::Report r;
        r.link = amb::Link::Ended;
        r.ticks[(int)amb::Link::Connected] = 590;
        r.ticks[(int)amb::Link::Ended] = 12;
        r.frameScript = 602;
        r.frameRender = 7;
        r.periodic = 5;
        r.clears = 3;
        r.exemptCount = 9;
        r.protectedSeen = 21;
        r.nearPeds = 4;
        r.calls.Note(amb::Id::PedDensity, amb::Result::Ok, 600);
        r.calls.Note(amb::Id::PedDensity, amb::Result::Failed, 2);
        r.calls.Note(amb::Id::PedBudget, amb::Result::Inert, 5);
        r.breaker = true;
        r.breakerLost = 2;
        char line[1400];
        const int len = amb::FormatLine(line, sizeof line, r);
        const std::string L = line;
        C("line: starts with the engine: ambient: tag and carries the state", L.rfind("engine: ambient: 10s link=ended ", 0) == 0 && len == (int)L.size(), L.substr(0, 60));
        C("line: ticks per connection state, frames, periodic / clears",
          L.find("ticks[connected=590 entering=0 offline=0 ended=12]") != std::string::npos &&
          L.find("frames[script=602 render=7]") != std::string::npos && L.find("ran[periodic=5 clear=3 sweep=0]") != std::string::npos);
        C("line: failed and inert counts per native (ok/failed/inert); the five multipliers always show, others only when used",
          L.find("ped_density=600/2/0") != std::string::npos && L.find("veh_density=0/0/0") != std::string::npos &&
          L.find("scenario_density=0/0/0") != std::string::npos && L.find("ped_budget=0/0/5") != std::string::npos &&
          L.find("trains=") == std::string::npos);
        C("line: breaker, exempt and protected counts, enumeration counts",
          L.find("breaker=TRIPPED lost=2 exempt=9 protected=21") != std::string::npos && L.find("near[peds=4 vehs=-1]") != std::string::npos &&
          L.find("sweep=no-hash") != std::string::npos && L.back() == ']');
        char small[80];
        const int sl = amb::FormatLine(small, sizeof small, r);
        C("line: a short buffer is cut safely and stays NUL-terminated", sl >= 0 && sl < (int)sizeof small && small[sl] == 0 && std::strlen(small) == (size_t)sl);
    }

    // ---- the engine really calls it before the gate (source-order guard) ---------------------------------------------
    {
        const std::string src = ReadAll(repoDir + "/apps/engine/engine.cpp");
        C("source guard: engine.cpp is readable (presence: the checks below looked at real text)", src.size() > 100000, S("%zu bytes", src.size()));
        const size_t gw = src.find("void GameWork(bool inScript, WorkProbe* probe)");
        const size_t gate = gw == std::string::npos ? std::string::npos : src.find("if (!EnterServer())", gw);
        const size_t call = gw == std::string::npos ? std::string::npos : src.find("TickAmbient(inScript, lp, probe);", gw);
        C("source guard: GameWork calls TickAmbient BEFORE the EnterServer gate (the old code sat behind it)",
          gw != std::string::npos && gate != std::string::npos && call != std::string::npos && call < gate,
          S("GameWork %zu, TickAmbient %zu, EnterServer gate %zu", gw, call, gate));
        size_t pos = 0;
        int callsFrame = 0;
        while ((pos = src.find("SuppressAmbientFrame()", pos)) != std::string::npos) { ++callsFrame; pos += 10; }
        C("source guard: the per-frame multipliers are called from exactly one place in engine.cpp (TickAmbient)", callsFrame == 1, S("%d", callsFrame));
    }
    return n;
}

#ifdef T5_WITH_WORLD
// The real World wrappers on a test exe whose native table never resolves: every call that reaches CallNative
// fails, every hash-0 native is inert. Run inside gage_mp_test (which links hook.cpp).
inline int RunWorld(Report check)
{
    int n = 0;
    auto C = [&](const char* name, bool ok, const std::string& got = "") { ++n; check(name, ok, got); };
    namespace g = gage::game;
    const auto I = [](amb::Id id) { return (int)id; };
    {
        g::World w;
        w.SuppressAmbientFrame();
        w.SuppressAmbientFrame();
        const amb::Tally& t = w.ambientTally;
        C("world: an unresolved density native is COUNTED as failed, per native, every frame (it was silent before)",
          t.failed[I(amb::Id::PedDensity)] == 2 && t.failed[I(amb::Id::VehDensity)] == 2 && t.failed[I(amb::Id::RandVehDensity)] == 2 &&
          t.failed[I(amb::Id::ParkedDensity)] == 2 && t.failed[I(amb::Id::ScenarioDensity)] == 2,
          S("%u %u %u %u %u", t.failed[0], t.failed[1], t.failed[2], t.failed[3], t.failed[4]));
        C("world: ... and not as ok or inert", t.ok[I(amb::Id::PedDensity)] == 0 && t.inert[I(amb::Id::PedDensity)] == 0);
        w.SuppressAmbientSystems(10.f, 20.f, 30.f, true);
        C("world: the persistent switches are counted too (events, trains, boats, parked, cops, dispatch x15, wanted, radar)",
          t.failed[I(amb::Id::RandomEvents)] == 1 && t.failed[I(amb::Id::Trains)] == 1 && t.failed[I(amb::Id::Boats)] == 1 &&
          t.failed[I(amb::Id::ParkedCount)] == 1 && t.failed[I(amb::Id::RandomCops)] == 3 && t.failed[I(amb::Id::Dispatch)] == 15 &&
          t.failed[I(amb::Id::WantedMult)] == 1 && t.failed[I(amb::Id::RadarBlips)] == 1,
          S("dispatch %u cops %u", t.failed[I(amb::Id::Dispatch)], t.failed[I(amb::Id::RandomCops)]));
        // The natives with no hash yet: inert (never reach CallNative, so never "failed"); once a hash is filled in
        // they are simply attempted and fail here like the rest. Either way: counted, once per call, nothing thrown.
        const bool budgetInert = gage::game::native::SET_PED_POPULATION_BUDGET == 0;
        C("world: a hash-0 population budget is inert (counted inert, not failed) -- or attempted once a hash exists",
          budgetInert ? (t.inert[I(amb::Id::PedBudget)] == 1 && t.failed[I(amb::Id::PedBudget)] == 0)
                      : (t.failed[I(amb::Id::PedBudget)] == 1 && t.inert[I(amb::Id::PedBudget)] == 0));
        const bool vehBudgetInert = gage::game::native::SET_VEHICLE_POPULATION_BUDGET == 0;
        const bool genInert = gage::game::native::SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA == 0;
        const bool trainsInert = gage::game::native::DELETE_ALL_TRAINS == 0;
        C("world: the vehicle budget, generators-in-area and delete-trains sentinels are inert too (or attempted with a hash)",
          (vehBudgetInert ? t.inert[I(amb::Id::VehBudget)] == 1 : t.failed[I(amb::Id::VehBudget)] == 1) &&
          (genInert ? t.inert[I(amb::Id::GeneratorsArea)] == 1 : t.failed[I(amb::Id::GeneratorsArea)] == 1) &&
          (trainsInert ? t.inert[I(amb::Id::DeleteTrains)] == 1 : t.failed[I(amb::Id::DeleteTrains)] == 1));
        const bool sweepInert = gage::game::native::GET_PED_NEARBY_PEDS == 0 || gage::game::native::GET_PED_NEARBY_VEHICLES == 0;
        C("world: the sweep reports itself unavailable exactly while an enumeration hash is 0",
          w.AmbientSweepAvailable() == !sweepInert);
        int32_t buf[8] = {};
        const int np = w.NearbyPeds(5, buf, 8);
        const int nv = w.NearbyVehicles(5, buf, 8);
        C("world: the enumerations return -1 (inert or uncallable), never a count, and are counted",
          np == -1 && nv == -1 && t.Total(amb::Id::NearbyPeds) == 1 && t.Total(amb::Id::NearbyVehs) == 1 &&
          (gage::game::native::GET_PED_NEARBY_PEDS == 0 ? t.inert[I(amb::Id::NearbyPeds)] == 1 : t.failed[I(amb::Id::NearbyPeds)] == 1));
        C("world: no enumeration without a ped or a buffer (-1, not even counted)",
          w.NearbyPeds(0, buf, 8) == -1 && w.NearbyPeds(5, nullptr, 8) == -1 && w.NearbyPeds(5, buf, 0) == -1 &&
          t.Total(amb::Id::NearbyPeds) == 1);
        const uint32_t grabBefore = t.Total(amb::Id::GrabEntity);
        const bool d0 = w.DeleteAmbientEntity(0);
        const bool dm = w.DeleteAmbientEntity(-1);
        C("world: DeleteAmbientEntity refuses handle 0 and -1 without touching a native", !d0 && !dm && t.Total(amb::Id::GrabEntity) == grabBefore);
        const bool dd = w.DeleteAmbientEntity(42);
        C("world: a real handle is grabbed and deleted through counted calls (here: both fail, there is no native table)",
          !dd && t.failed[I(amb::Id::GrabEntity)] == 1 && t.failed[I(amb::Id::DeleteEntity)] == 1);
    }
    return n;
}
#endif

}  // namespace t5
