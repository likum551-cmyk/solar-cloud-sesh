// Vehicle signals on the OTHER clients' copies (0.4.19; GI: "make everyone hear the horns from the car, see headlights turn on/off"):
// the horn and the headlights / high beam of a car somebody else controls. Pure C++17, header-only, no game calls: the engine
// passes the server's bits and its own native reads in, calls the natives this module names and says whether they could be called.
// Nothing on the wire changes: the horn is kVehHorn in VehicleState.flags (the driver's, in every full AND motion-only entry, so it
// arrives at the car's send rate), the headlights / high beam are kVeh2Headlights / kVeh2HighBeam in flags2 (full entries; a change
// of them is a full entry for 3 frames). Tested offline in tests/vehsignals_tests.h (pure modules, the store and the real server).
//
// HORN (HornSync, per copy, every script tick):
//   0.4.17 called START_VEHICLE_HORN(car, 400 ms, mode 0) for every state that arrived with the horn bit: 30 calls a second for a
//   near car, a fixed-length tail after the release, and nothing between two states of a slow-rate car. Now the horn follows the
//   STATE: a rise starts it at once, a held horn is renewed every renewMs with a hold of holdMs (so it never runs out between two
//   renewals and never outlives the release by more than holdMs), the release issues one call with a 1 ms hold (stopMs: the
//   game's only way to cut a horn short, INFERRED; veh_horn_stop=0 leaves the tail to run out), and a car whose states stop
//   arriving (staleMs) is let go. IS_HORN_ACTIVE on the copy is the read-back: probeAfterMs after a rise / a release and every
//   probeHeldMs while held, never in the frame of a call (the call's own frame says nothing).
//
// HEADLIGHTS (LightsKeeper, per near copy):
//   ApplyVehicleFields / ApplyVehicleBody set SET_VEHICLE_LIGHTS (2 forced on / 1 forced off) and SET_VEHICLE_FULLBEAM when the
//   server's bits CHANGE, and record them as shown whether or not the game kept them. The keeper reads GET_VEHICLE_LIGHTS_STATE
//   on the copy every readMs and re-issues the part that differs once confirmReads reads in a row disagree; a difference that
//   survives failLimit re-issues is "stuck" (the native does not do what we think on a copy) and the car is left alone for
//   backoffMs, so a wrong assumption costs a few calls, never a stream.
#pragma once

#include "sync/vehicle_body.h"

#include <cstdint>

namespace gage::sync {

    // ---- horn ---------------------------------------------------------------------------------------------------------

    struct HornConfig
    {
        int    holdMs = 300;          // START_VEHICLE_HORN duration of every call while the controller's horn sounds
        double renewMs = 120.0;       // a sounding horn is renewed this often (well under holdMs: never a gap, never per frame or per state)
        double staleMs = 750.0;       // no state of the car for this long: the horn is let go (a lost release never honks for ever)
        int    stopMs = 1;            // the release call's duration (cuts the hold short); 0 = no release call, the hold runs out
        double probeAfterMs = 100.0;  // IS_HORN_ACTIVE this long after a rise / a release...
        double probeHeldMs = 250.0;   // ... and this often while it is held
        int    failLimit = 3;         // calls in a row that could not be made before the car is left alone...
        double failBackoffMs = 5000.0;// ... for this long
    };

    enum class HornCall : uint8_t { None = 0, Start = 1, Stop = 2 };

    struct HornOut
    {
        HornCall call = HornCall::None;
        int durationMs = 0;           // the START_VEHICLE_HORN duration for Start and Stop
        bool probe = false;           // read IS_HORN_ACTIVE this frame and pass it to NoteProbe
    };

    struct HornStats
    {
        uint32_t rises = 0, releases = 0, staleReleases = 0;   // edges of the horn this copy shows (stale = no state for staleMs)
        uint32_t startsIssued = 0, stopsIssued = 0, callFails = 0, suspends = 0;
        uint32_t probes = 0, probeFails = 0;                    // read-backs asked for / that could not be read
        uint32_t riseHeard = 0, riseSilent = 0;                 // read-back probeAfterMs after a rise
        uint32_t heldHeard = 0, heldSilent = 0;                 // ... while held (silent = a gap the players hear)
        uint32_t releaseQuiet = 0, releaseSounding = 0;         // ... probeAfterMs after a release (sounding = the release call did not cut it)
        HornStats& operator+=(const HornStats& o)
        {
            rises += o.rises; releases += o.releases; staleReleases += o.staleReleases;
            startsIssued += o.startsIssued; stopsIssued += o.stopsIssued; callFails += o.callFails; suspends += o.suspends;
            probes += o.probes; probeFails += o.probeFails;
            riseHeard += o.riseHeard; riseSilent += o.riseSilent; heldHeard += o.heldHeard; heldSilent += o.heldSilent;
            releaseQuiet += o.releaseQuiet; releaseSounding += o.releaseSounding;
            return *this;
        }
    };

    class HornSync
    {
    public:
        explicit HornSync(const HornConfig& cfg = HornConfig()) : cfg_(cfg) {}
        void Configure(const HornConfig& cfg) { cfg_ = cfg; }   // the switches (veh_horn_stop) are re-read at every Welcome

        // A new entity, a hand-over, a new session.
        void Reset()
        {
            on_ = false;
            lastStartMs_ = -1e9;
            probeKind_ = kProbeNone;
            probeAtMs_ = 0.0;
            failStreak_ = 0;
            suspendUntil_ = 0.0;
        }

        // One frame. `want` = the newest state's kVehHorn, `stateMs` = when that state was applied here.
        HornOut Update(bool want, double stateMs, double nowMs)
        {
            HornOut o;
            if (want && nowMs - stateMs > cfg_.staleMs)
            {
                if (on_) ++stats_.staleReleases;
                want = false;
            }
            const bool suspended = nowMs < suspendUntil_;
            if (want && !on_)
            {
                on_ = true;
                ++stats_.rises;
                SetProbe(kProbeRise, nowMs + cfg_.probeAfterMs);
                if (!suspended)
                {
                    o.call = HornCall::Start;
                    o.durationMs = cfg_.holdMs;
                }
            }
            else if (want && on_)
            {
                if (!suspended && nowMs - lastStartMs_ >= cfg_.renewMs)
                {
                    o.call = HornCall::Start;
                    o.durationMs = cfg_.holdMs;
                }
            }
            else if (!want && on_)
            {
                on_ = false;
                ++stats_.releases;
                SetProbe(kProbeRelease, nowMs + cfg_.probeAfterMs);
                if (!suspended && cfg_.stopMs > 0)
                {
                    o.call = HornCall::Stop;
                    o.durationMs = cfg_.stopMs;
                }
            }
            // The read-back: due, and never in the frame of a call (it would read the call's own frame).
            if (o.call == HornCall::None && probeKind_ != kProbeNone && nowMs >= probeAtMs_)
            {
                o.probe = true;
                ++stats_.probes;
            }
            if (o.call == HornCall::Start) lastStartMs_ = nowMs;
            return o;
        }

        // After the call Update named: `ok` = it could be made (the World wrapper's return).
        void Commit(const HornOut& o, bool ok, double nowMs)
        {
            if (o.call == HornCall::None) return;
            if (ok)
            {
                failStreak_ = 0;
                if (o.call == HornCall::Start) ++stats_.startsIssued;
                else ++stats_.stopsIssued;
                return;
            }
            ++stats_.callFails;
            if (++failStreak_ >= cfg_.failLimit)
            {
                failStreak_ = 0;
                suspendUntil_ = nowMs + cfg_.failBackoffMs;
                ++stats_.suspends;
            }
        }

        // The IS_HORN_ACTIVE answer for the probe Update asked for. `readOk` false = the native could not be called.
        void NoteProbe(bool readOk, bool active, double nowMs)
        {
            const int kind = probeKind_;
            if (kind == kProbeNone) return;
            if (!readOk) ++stats_.probeFails;
            else if (kind == kProbeRise) ++(active ? stats_.riseHeard : stats_.riseSilent);
            else if (kind == kProbeHeld) ++(active ? stats_.heldHeard : stats_.heldSilent);
            else ++(active ? stats_.releaseSounding : stats_.releaseQuiet);
            // While it is held the next look comes probeHeldMs later; after a release nothing more is asked.
            if (on_) SetProbe(kProbeHeld, nowMs + cfg_.probeHeldMs);
            else probeKind_ = kProbeNone;
        }

        bool On() const { return on_; }
        const HornStats& Stats() const { return stats_; }
        HornStats TakeStats()                                    // the counters since the last take (the engine sums them per 10 s window)
        {
            const HornStats s = stats_;
            stats_ = HornStats();
            return s;
        }
        const HornConfig& Config() const { return cfg_; }

    private:
        static constexpr int kProbeNone = 0, kProbeRise = 1, kProbeHeld = 2, kProbeRelease = 3;
        void SetProbe(int kind, double atMs)
        {
            probeKind_ = kind;
            probeAtMs_ = atMs;
        }

        HornConfig cfg_;
        bool on_ = false;
        double lastStartMs_ = -1e9;
        int probeKind_ = kProbeNone;
        double probeAtMs_ = 0.0;
        int failStreak_ = 0;
        double suspendUntil_ = 0.0;
        HornStats stats_;
    };

    // One car, one frame, the engine's sequence (a template over the game, so a test drives the SAME code). `Game`:
    // bool StartHorn(int durationMs) (START_VEHICLE_HORN; false = could not be called), bool HornActive(bool& active)
    // (IS_HORN_ACTIVE; false = could not be called).
    template <class Game>
    inline HornOut StepHorn(HornSync& hs, Game& g, bool want, double stateMs, double nowMs)
    {
        const HornOut o = hs.Update(want, stateMs, nowMs);
        if (o.call != HornCall::None)
            hs.Commit(o, g.StartHorn(o.durationMs), nowMs);
        if (o.probe)
        {
            bool active = false;
            const bool ok = g.HornActive(active);
            hs.NoteProbe(ok, active, nowMs);
        }
        return o;
    }

    // 0.4.30: the horn bit a controller sends. With the siren on, GTA turns a held INPUT_VEH_HORN (E) into the siren's second tone and
    // IS_HORN_ACTIVE stays false (0.4.29 live log: a police car driven 2.5 min, ~2640 updates, 0 with the horn), so there the driver's held
    // key is the signal; siren off, the read-back rules as before. sirenHornOn false (veh_siren_horn=0) = 0.4.29: the read-back alone.
    inline bool HornBitToSend(bool hornActive, bool sirenOn, bool driver, bool axisOk, float hornAxis, bool sirenHornOn)
    {
        if (hornActive)
            return true;
        return sirenHornOn && sirenOn && driver && axisOk && hornAxis >= 0.5f;
    }

    // The held-key half of HornBitToSend must be HELD: a tap of E with the siren on is GTA's siren-mode switch, not the second tone,
    // so the key counts only after kSirenHornHoldMs of continuous hold (per vehicle). A sounding horn (IS_HORN_ACTIVE) is never delayed.
    constexpr double kSirenHornHoldMs = 150.0;
    struct SirenHornHold
    {
        double since = -1.0;    // when the current hold began (-1 = not held)
        bool Step(bool hornActive, bool keyBit, double nowMs)
        {
            if (hornActive)
                return true;
            if (!keyBit) { since = -1.0; return false; }
            if (since < 0.0) since = nowMs;
            return nowMs - since >= kSirenHornHoldMs;
        }
    };

    // ---- headlights / high beam --------------------------------------------------------------------------------------

    struct LightsWant
    {
        bool on = false, high = false;      // HeadlightsShown(server override, kVeh2Headlights) and kVeh2HighBeam
        bool operator==(const LightsWant& o) const { return on == o.on && high == o.high; }
        bool operator!=(const LightsWant& o) const { return !(*this == o); }
    };

    // SET_VEHICLE_LIGHTS(car, mode) with mode 2 (forced on) / 1 (forced off), SET_VEHICLE_FULLBEAM(car, high).
    struct LightsCmd
    {
        bool setMode = false;
        int  mode = 0;
        bool setHigh = false;
        bool high = false;
        bool Any() const { return setMode || setHigh; }
    };

    struct LightsConfig
    {
        double readMs = 500.0;              // a near copy is read this often
        double afterChangeMs = 150.0;       // the first read after a change of the wanted state (the change's own commands were just issued)
        int    confirmReads = 2;            // reads in a row that must disagree before anything is re-issued (one read can land mid-transition)
        int    failLimit = 3;               // re-issues that did not take before the car is "stuck"...
        double backoffMs = 10000.0;         // ... and left alone for this long
        bool   highIsLit = true;            // 0.4.30 veh_lights_beam_lit: a read of lightsOn 0 / highbeamsOn 1 is lit (LightsLit)
    };

    struct LightsStats
    {
        uint32_t reads = 0, readFails = 0;  // read-backs judged / that could not be read
        uint32_t match = 0, mismatch = 0;   // judged reads that showed / did not show the wanted state
        uint32_t reissued = 0;              // re-issue rounds (SET_VEHICLE_LIGHTS and / or SET_VEHICLE_FULLBEAM)
        uint32_t restored = 0;              // ... after which the next read matched (the re-issue worked)
        uint32_t stuck = 0;                 // a difference that outlived failLimit re-issues (then backoffMs of silence)
        LightsStats& operator+=(const LightsStats& o)
        {
            reads += o.reads; readFails += o.readFails; match += o.match; mismatch += o.mismatch;
            reissued += o.reissued; restored += o.restored; stuck += o.stuck;
            return *this;
        }
    };

    class LightsKeeper
    {
    public:
        explicit LightsKeeper(const LightsConfig& cfg = LightsConfig()) : cfg_(cfg) {}
        void Configure(const LightsConfig& c) { cfg_ = c; }

        void Reset()
        {
            seen_ = false;
            want_ = LightsWant();
            nextReadMs_ = 0.0;
            mismatchRun_ = 0;
            reissueRun_ = 0;
            suspendUntil_ = 0.0;
            reissuedLast_ = false;
        }

        // Whether the engine should read the copy and call Update this frame.
        bool Due(double nowMs, const LightsWant& want, bool isNear) const
        {
            if (!isNear || nowMs < suspendUntil_) return false;
            if (!seen_ || want != want_) return true;
            return nowMs >= nextReadMs_;
        }

        // `readOk`, `on`, `high`: GET_VEHICLE_LIGHTS_STATE on the copy.
        LightsCmd Update(const LightsWant& want, bool readOk, bool on, bool high, double nowMs)
        {
            LightsCmd c;
            if (!seen_ || want != want_)
            {
                // A change (or the first sight): ApplyVehicleFields issued its commands this tick; judge a little later.
                seen_ = true;
                want_ = want;
                mismatchRun_ = 0;
                reissueRun_ = 0;
                reissuedLast_ = false;
                nextReadMs_ = nowMs + cfg_.afterChangeMs;
                return c;
            }
            nextReadMs_ = nowMs + cfg_.readMs;
            if (!readOk)
            {
                ++stats_.readFails;
                return c;
            }
            ++stats_.reads;
            const bool onOk = LightsLit(on, high, cfg_.highIsLit) == want.on;
            const bool highOk = !want.on || high == want.high;     // a high beam with the lights off is not a state of its own
            if (onOk && highOk)
            {
                ++stats_.match;
                if (reissuedLast_) ++stats_.restored;
                mismatchRun_ = 0;
                reissueRun_ = 0;
                reissuedLast_ = false;
                return c;
            }
            ++stats_.mismatch;
            reissuedLast_ = false;
            if (++mismatchRun_ < cfg_.confirmReads)
            {
                nextReadMs_ = nowMs + cfg_.afterChangeMs;          // confirm soon
                return c;
            }
            if (reissueRun_ >= cfg_.failLimit)
            {
                ++stats_.stuck;
                suspendUntil_ = nowMs + cfg_.backoffMs;
                reissueRun_ = 0;
                mismatchRun_ = 0;
                return c;
            }
            ++reissueRun_;
            mismatchRun_ = 0;
            if (!onOk)
            {
                c.setMode = true;
                c.mode = want.on ? 2 : 1;
            }
            if (want.on && (!onOk || !highOk))
            {
                c.setHigh = true;                                  // forced on again: the high beam is said again with it
                c.high = want.high;
            }
            else if (!want.on && !onOk && high)
            {
                c.setHigh = true;                                  // 0.4.30: "lit" may be the full-beam flag alone (LightsLit): clear it too,
                c.high = false;                                    // or a copy that should be dark is re-issued SET_VEHICLE_LIGHTS(1) forever
            }
            ++stats_.reissued;
            reissuedLast_ = true;
            nextReadMs_ = nowMs + cfg_.afterChangeMs;
            return c;
        }

        const LightsStats& Stats() const { return stats_; }
        LightsStats TakeStats()
        {
            const LightsStats s = stats_;
            stats_ = LightsStats();
            return s;
        }

    private:
        LightsConfig cfg_;
        bool seen_ = false;
        LightsWant want_;
        double nextReadMs_ = 0.0;
        int mismatchRun_ = 0, reissueRun_ = 0;
        double suspendUntil_ = 0.0;
        bool reissuedLast_ = false;
        LightsStats stats_;
    };

    // One car, one frame (engine sequence). `Game`: bool ReadLights(bool& on, bool& high) (GET_VEHICLE_LIGHTS_STATE),
    // bool SetLights(int mode), bool SetFullbeam(bool high). Returns the commands issued (empty when not due).
    template <class Game>
    inline LightsCmd StepLights(LightsKeeper& lk, Game& g, const LightsWant& want, bool isNear, double nowMs)
    {
        if (!lk.Due(nowMs, want, isNear)) return LightsCmd();
        bool on = false, high = false;
        const bool ok = g.ReadLights(on, high);
        const LightsCmd c = lk.Update(want, ok, on, high, nowMs);
        if (c.setMode) g.SetLights(c.mode);
        if (c.setHigh) g.SetFullbeam(c.high);
        return c;
    }

}
