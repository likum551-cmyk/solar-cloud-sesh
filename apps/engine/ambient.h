// T5 ambient (desync overhaul round 2, symptom 4): the pure decision logic for "no ambient NPCs and no
// ambient traffic for anyone". Header-only, no game and no OS calls, so tests/t5_ambient_tests.h runs it
// natively (ambient_test) and the engine only wires it:
//   * WHEN the suppression runs (Scheduler): every script tick, whatever the connection or entry state,
//     with the render-thread fallback allowed for the per-frame multipliers only; the periodic
//     persistent switches, the area clear and the entity sweep on their own cadences.
//   * WHAT a clean-up must never touch (ExemptSet): a handle safe-list. A sweep deletes a handle only
//     when MayDelete() proves it is not exempt; an incomplete list deletes nothing.
//   * WHEN a clean-up is switched off again (the circuit breaker): if an exempt handle that existed
//     before an area clear / sweep is gone right after it, the clear is switched off for the rest of the
//     process (the semantics of CLEAR_AREA_OF_PEDS on mission peds are unverified live).
//   * HOW MANY native calls worked (Tally), so an unresolved hash no longer looks like "NPCs still
//     appear", and the rate-limited `engine: ambient` line (FormatLine).
// Registry switches (HKCU\Software\GAGE-MP, "0"/"1"): ambient (master, default 1), ambient_render (the
// per-frame multipliers on the render-thread fallback, default 1), ambient_clear (periodic area clear,
// default 1 behind the breaker), ambient_sweep (entity sweep, default 1; inert until the enumeration
// hash exists).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gage::ambient {

// ---------------------------------------------------------------------------
// The natives this module calls, for counting. "Sentinel" ones have no hash yet (hook.h constant 0,
// UNRESOLVED: lookup needed); their wrapper is inert and counted as such.
// ---------------------------------------------------------------------------
enum class Id : int {
    PedDensity, VehDensity, RandVehDensity, ParkedDensity, ScenarioDensity,   // every frame
    RandomEvents, Trains, GarbageTrucks, Boats, ParkedCount, RandomCops,       // periodic, persistent switches
    Dispatch, PlayerFlags, WantedMult, RadarBlips,
    PedBudget, VehBudget, GeneratorsArea, DeleteTrains,                        // periodic, sentinel hashes
    ClearPeds, ClearCops, RemoveGenVeh,                                        // area clear
    NearbyPeds, NearbyVehs,                                                    // sweep, sentinel hashes
    GrabEntity, DeleteEntity,                                                  // sweep
    kCount
};
constexpr int kIdCount = (int)Id::kCount;

inline const char* Name(Id id) {
    static const char* const k[kIdCount] = {
        "ped_density", "veh_density", "rand_veh_density", "parked_density", "scenario_density",
        "random_events", "trains", "garbage_trucks", "boats", "parked_count", "random_cops",
        "dispatch", "player_flags", "wanted_mult", "radar_blips",
        "ped_budget", "veh_budget", "generators_area", "delete_trains",
        "clear_peds", "clear_cops", "remove_gen_veh",
        "nearby_peds", "nearby_vehs",
        "grab", "delete",
    };
    const int i = (int)id;
    return i >= 0 && i < kIdCount ? k[i] : "?";
}
// The natives that need a hash nobody has looked up yet (see NATIVES NEEDED in the T5 report).
inline bool IsSentinel(Id id) {
    return id == Id::PedBudget || id == Id::VehBudget || id == Id::GeneratorsArea || id == Id::DeleteTrains ||
           id == Id::NearbyPeds || id == Id::NearbyVehs;
}

enum class Result { Ok, Failed, Inert };   // Failed: CallNative returned false (hash unresolved on this build, or the call failed)

struct Tally {
    uint32_t ok[kIdCount] = {}, failed[kIdCount] = {}, inert[kIdCount] = {};
    bool seen[kIdCount] = {};              // the first-use line has been logged

    // True the first time this id is noted (the caller logs the first-use resolution line then).
    bool Note(Id id, Result r, uint32_t n = 1) {
        const int i = (int)id;
        if (i < 0 || i >= kIdCount) return false;
        if (r == Result::Ok) ok[i] += n;
        else if (r == Result::Failed) failed[i] += n;
        else inert[i] += n;
        const bool first = !seen[i];
        seen[i] = true;
        return first;
    }
    uint32_t Total(Id id) const { const int i = (int)id; return ok[i] + failed[i] + inert[i]; }
};

// What changed since `prev` (counters only).
inline Tally Delta(const Tally& now, const Tally& prev) {
    Tally d = now;
    for (int i = 0; i < kIdCount; ++i) {
        d.ok[i] = now.ok[i] - prev.ok[i];
        d.failed[i] = now.failed[i] - prev.failed[i];
        d.inert[i] = now.inert[i] - prev.inert[i];
    }
    return d;
}

// ---------------------------------------------------------------------------
// Registry switches
// ---------------------------------------------------------------------------
template <class S> inline bool Flag(const S& v, bool def) {
    if (v.size() == 1 && v[0] == '0') return false;
    if (v.size() == 1 && v[0] == '1') return true;
    return def;                            // missing, empty or anything else: the default
}

struct Switches {
    bool master = true;                    // ambient=0: the whole module off (the pre-T5 behaviour is NOT restored: nothing runs)
    bool renderFallback = true;            // ambient_render=0: the render-thread fallback does not touch the multipliers
    bool areaClear = true;                 // ambient_clear=0: no periodic CLEAR_AREA_OF_PEDS / COPS / generator removal
    bool sweep = true;                     // ambient_sweep=0: no entity sweep (also inert while the enumeration hash is 0)
};
// `get(name)` returns the registry value as something with size() and operator[] (std::string / wstring).
template <class Get> inline Switches LoadSwitches(Get&& get) {
    Switches s;
    s.master = Flag(get("ambient"), true);
    s.renderFallback = Flag(get("ambient_render"), true);
    s.areaClear = Flag(get("ambient_clear"), true);
    s.sweep = Flag(get("ambient_sweep"), true);
    return s;
}

// ---------------------------------------------------------------------------
// The connection state, for the diagnostics line only (suppression does NOT depend on it).
// ---------------------------------------------------------------------------
enum class Link : int { Offline = 0, Entering, Connected, Ended };   // Ended: a session was up and is gone (also: retrying)
inline const char* Name(Link l) {
    switch (l) {
    case Link::Offline: return "offline";
    case Link::Entering: return "entering";
    case Link::Connected: return "connected";
    case Link::Ended: return "ended";
    }
    return "?";
}
inline Link LinkOf(bool connected, bool entryDone, bool everConnected) {
    if (connected) return entryDone ? Link::Connected : Link::Entering;
    return everConnected ? Link::Ended : Link::Offline;
}

// ---------------------------------------------------------------------------
// The scheduler
// ---------------------------------------------------------------------------
constexpr uint64_t kPeriodicMs = 2000;     // the persistent switches (events, trains, boats, parked, cops, dispatch, budgets, generators)
constexpr uint64_t kClearMs = 3000;        // the area clear
constexpr uint64_t kSweepMs = 1000;        // the entity sweep
constexpr float kClearRadiusM = 200.0f;    // the same radius as the entry / respawn clear (engine.cpp kSpawnClearRadius)
constexpr int kNearbyCap = 64;             // handles read per enumeration call
constexpr int kSweepMaxDeletes = 16;       // deleted per sweep: a hitch bound, the next sweep takes the rest

enum class Skip : int { None = 0, MasterOff, SpawnZero, NativesDown, RenderOff, kCount };

struct Ctx {
    bool inScript = false;                 // the script tick (true) or the render-thread fallback (false)
    int spawnMode = -1;                    // World::spawnMode; 0 = the patch-nothing diagnostic mode
    bool nativesUp = false;                // World::HaveNatives()
    bool havePlayer = false;               // the local ped exists (script tick only: the native has no TLS on the render thread)
    bool sweepAvailable = false;           // the enumeration hash is non-zero
    uint64_t nowMs = 0;
};

struct Plan {
    bool frame = false;                    // the per-frame multipliers
    bool periodic = false;                 // the persistent switches
    bool clear = false;                    // the area clear
    bool sweep = false;                    // the entity sweep
    Skip skip = Skip::None;                // why nothing at all runs (frame == false)
};

class Scheduler {
public:
    explicit Scheduler(const Switches& s = Switches()) : sw_(s) {}
    void Configure(const Switches& s) { sw_ = s; }
    const Switches& Sw() const { return sw_; }

    // Decide what runs this tick. Nothing depends on the connection or the entry: the multipliers run on
    // every tick the game thread is live, before, during and after a session. The render-thread fallback
    // gets the per-frame multipliers only (they are plain stores; every other call here needs the script
    // thread's context).
    Plan Next(const Ctx& c) {
        Plan p;
        if (!sw_.master) { p.skip = Skip::MasterOff; return p; }
        if (c.spawnMode == 0) { p.skip = Skip::SpawnZero; return p; }
        if (!c.nativesUp) { p.skip = Skip::NativesDown; return p; }
        if (!c.inScript) {
            if (!sw_.renderFallback) { p.skip = Skip::RenderOff; return p; }
            p.frame = true;
            return p;
        }
        p.frame = true;
        if (!c.havePlayer) return p;
        if (Due(periodic_, c.nowMs, kPeriodicMs)) { p.periodic = true; periodic_ = Stamp(c.nowMs); }
        if (sw_.areaClear && !breaker_ && Due(clear_, c.nowMs, kClearMs)) { p.clear = true; clear_ = Stamp(c.nowMs); }
        if (sw_.sweep && !breaker_ && c.sweepAvailable && Due(sweep_, c.nowMs, kSweepMs)) { p.sweep = true; sweep_ = Stamp(c.nowMs); }
        return p;
    }

    // An area clear or sweep that ran: `before` / `after` = how many exempt handles existed just before
    // and just after it (same tick). One fewer is proof that the clean-up took an exempt entity: the
    // clear and the sweep are switched off for the rest of the process. True when it tripped now.
    bool CheckProtected(int before, int after) {
        if (after >= before) return false;
        if (!breaker_) { breaker_ = true; ++trips_; }
        lost_ += (uint32_t)(before - after);
        return true;
    }
    bool BreakerTripped() const { return breaker_; }
    uint32_t Trips() const { return trips_; }
    uint32_t ExemptLost() const { return lost_; }

private:
    struct Mark { bool have = false; uint64_t at = 0; };
    static Mark Stamp(uint64_t now) { Mark m; m.have = true; m.at = now; return m; }
    static bool Due(const Mark& m, uint64_t now, uint64_t period) { return !m.have || now < m.at || now - m.at >= period; }

    Switches sw_;
    Mark periodic_, clear_, sweep_;
    bool breaker_ = false;
    uint32_t trips_ = 0, lost_ = 0;
};

// ---------------------------------------------------------------------------
// The exemption safe-list
// ---------------------------------------------------------------------------
// Handles, not models: the local ped, every remote player's puppet and its anchor / helper ped, every
// server vehicle and its occupants, the companion, the local player's current / last vehicle, the
// /car vehicles. The engine rebuilds it right before each clean-up and marks it complete only when the
// local ped was known; an incomplete list proves nothing, so nothing may be deleted.
class ExemptSet {
public:
    void Clear() { h_.clear(); complete_ = false; }
    void Add(int32_t h) { if (Valid(h) && !Contains(h)) h_.push_back(h); }
    void SetComplete(bool c) { complete_ = c; }
    bool Complete() const { return complete_; }
    bool Contains(int32_t h) const {
        for (int32_t x : h_) if (x == h) return true;
        return false;
    }
    size_t Size() const { return h_.size(); }
    const std::vector<int32_t>& Handles() const { return h_; }
    // The one predicate every sweep uses: a real handle, on a complete list, not on the list.
    bool MayDelete(int32_t h) const { return complete_ && Valid(h) && !Contains(h); }
    static bool Valid(int32_t h) { return h != 0 && h != -1; }
private:
    std::vector<int32_t> h_;
    bool complete_ = false;
};

// Read the array a GET_PED_NEARBY_* style native fills: slot 0 = count (the capacity going in), then one
// handle per 8-byte slot, low 32 bits. -1 when the count is not plausible (a wrong array layout must
// reject the whole read, not feed garbage handles to a delete).
inline int ParseNearby(const uint64_t* slots, int cap, int32_t* out) {
    if (!slots || !out || cap <= 0) return -1;
    const int32_t n = (int32_t)(uint32_t)slots[0];
    if (n < 0 || n > cap) return -1;
    for (int i = 0; i < n; ++i) out[i] = (int32_t)(uint32_t)slots[1 + i];
    return n;
}

struct SweepStats {
    int seen = 0;                          // handles read (valid or not)
    int invalid = 0;                       // 0 / -1 / duplicates
    int exempt = 0;                        // on the safe-list: protected
    int unproven = 0;                      // not on the list, but the list is incomplete: cannot prove it exempt, left alone
    int planned = 0;                       // to delete
    int capped = 0;                        // over the per-sweep cap: left for the next sweep
};
// Which of the enumerated handles to delete. Exempt, invalid and duplicate handles never are; an
// incomplete ExemptSet plans nothing.
inline SweepStats PlanSweep(const int32_t* in, int n, const ExemptSet& ex, int cap, std::vector<int32_t>& out) {
    SweepStats s;
    out.clear();
    if (!in || n <= 0) return s;
    s.seen = n;
    for (int i = 0; i < n; ++i) {
        const int32_t h = in[i];
        bool dup = false;
        for (int j = 0; j < i; ++j) if (in[j] == h) { dup = true; break; }
        if (dup || !ExemptSet::Valid(h)) { ++s.invalid; continue; }
        if (ex.Contains(h)) { ++s.exempt; continue; }
        if (!ex.MayDelete(h)) { ++s.unproven; continue; }
        if ((int)out.size() >= cap) { ++s.capped; continue; }
        out.push_back(h);
        ++s.planned;
    }
    s.planned = (int)out.size();
    return s;
}

// ---------------------------------------------------------------------------
// The `engine: ambient` line (every 10 s, rate-limited by the caller)
// ---------------------------------------------------------------------------
struct Report {
    double windowSec = 10.0;
    Link link = Link::Offline;
    uint32_t ticks[4] = {};                // script ticks per Link state this window
    uint32_t frameScript = 0, frameRender = 0;
    uint32_t skipped[(int)Skip::kCount] = {};
    uint32_t periodic = 0, clears = 0, sweeps = 0;
    Tally calls;                           // this window's delta
    int exemptCount = 0;                   // handles on the safe-list at the last clean-up (-1: none yet)
    uint32_t protectedSeen = 0;            // exempt handles met in sweeps this window
    uint32_t deleted = 0;                  // entities the sweep deleted this window
    int nearPeds = -1, nearVehs = -1;      // enumeration counts at the last sweep (-1: no enumeration)
    bool breaker = false;
    uint32_t breakerLost = 0;
    Switches sw;
    bool sweepAvailable = false;
};

inline int FormatLine(char* buf, size_t cap, const Report& r) {
    if (!buf || cap == 0) return 0;
    int n = std::snprintf(buf, cap,
        "engine: ambient: %.0fs link=%s ticks[connected=%u entering=%u offline=%u ended=%u] frames[script=%u render=%u] "
        "ran[periodic=%u clear=%u sweep=%u] skipped[master=%u spawn0=%u natives=%u render=%u] "
        "switches[master=%d render=%d clear=%d sweep=%d] sweep=%s breaker=%s lost=%u exempt=%d protected=%u deleted=%u near[peds=%d vehs=%d] calls[",
        r.windowSec, Name(r.link),
        r.ticks[(int)Link::Connected], r.ticks[(int)Link::Entering], r.ticks[(int)Link::Offline], r.ticks[(int)Link::Ended],
        r.frameScript, r.frameRender, r.periodic, r.clears, r.sweeps,
        r.skipped[(int)Skip::MasterOff], r.skipped[(int)Skip::SpawnZero], r.skipped[(int)Skip::NativesDown], r.skipped[(int)Skip::RenderOff],
        (int)r.sw.master, (int)r.sw.renderFallback, (int)r.sw.areaClear, (int)r.sw.sweep,
        r.sweepAvailable ? "on" : "no-hash", r.breaker ? "TRIPPED" : "off", r.breakerLost, r.exemptCount, r.protectedSeen, r.deleted,
        r.nearPeds, r.nearVehs);
    if (n < 0) return 0;
    size_t used = (size_t)n < cap ? (size_t)n : cap - 1;
    bool first = true;
    for (int i = 0; i < kIdCount && used + 48 < cap; ++i) {
        const Id id = (Id)i;
        const bool always = i <= (int)Id::ScenarioDensity;   // the five multipliers always show
        if (!always && r.calls.Total(id) == 0) continue;
        const int m = std::snprintf(buf + used, cap - used, "%s%s=%u/%u/%u", first ? "" : " ", Name(id),
                                    r.calls.ok[i], r.calls.failed[i], r.calls.inert[i]);
        if (m < 0) break;
        used += (size_t)m < cap - used ? (size_t)m : cap - used - 1;
        first = false;
    }
    if (used + 2 < cap) { buf[used++] = ']'; buf[used] = 0; }
    return (int)used;
}

}  // namespace gage::ambient
