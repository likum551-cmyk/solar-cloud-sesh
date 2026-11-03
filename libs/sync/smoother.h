// Remote-player smoothing: turns the irregular WorldState snapshots of one
// remote player into a smooth pose the render loop can ask for every frame.
//
//   RemoteSync sync(SmootherConfig::ForTickHz(welcome.tickHz));
//   ...net poll:     sync.OnWorldState(ev.world, nowMs);        // may be lossy / late / reordered
//                    for (id : ev.world.left) sync.Remove(id);  // no longer relevant: ped goes
//   ...render frame: sync.Prune(nowMs, &gone);                  // despawn peds in `gone`
//                    for (id : known) if (sync.Sample(id, nowMs, pose)) MovePed(id, pose);
//
// Pure C++17: no OS headers, no exceptions, no allocation per frame (a player's
// state is two fixed arrays; the only allocation is the map node when a player
// first appears). Single-threaded, one clock: every nowMs is the same monotonic
// local clock (any epoch, milliseconds). Links against nothing (the protocol
// header is used only for Kinematics and SeqNewer, both inline).
//
// HOW IT WORKS (constants are SmootherConfig defaults for the 20 Hz server)
//
//  0. What the server sends (protocol v2, apps/server/replication.cpp). Not every
//     player every tick: per receiver a player is sent at 20 Hz within 120 m,
//     10 Hz within 250 m, 5 Hz beyond, and at 1 Hz once its quantised state has
//     been unchanged for 500 ms (defaults, server.json can change them). Each
//     WorldEntry carries ageMs: how long before the packet was built the server
//     received that player's sample (0..255, saturating). A client sending at
//     ~21 Hz into the 20 Hz tick therefore arrives with irregular ages, and an idle
//     sender's last sample is re-sent with a growing age.
//
//  1. Ordering and placement. A snapshot is accepted only if its tick is newer
//     than the newest accepted one under u32 serial arithmetic (proto::SeqNewer):
//     duplicates and reordered-late packets are dropped (PushResult::Stale), and
//     ticks wrap safely. Ticks are unwrapped to a monotone int64. The packet's
//     time is tick * tickMs (when the server built it); the SAMPLE it carries is
//     placed at tick * tickMs - ageMs on that server timeline, immune to arrival
//     jitter and to the server's choice of which tick carries it.
//
//  2. Repeats. A newer tick whose state (kinematics AND PedState) is bit-identical to the newest stored
//     snapshot (a standing player), or whose placed time is not after the newest
//     stored one (the same sample re-sent with a larger age: an idle player at
//     1 Hz), is not new information: it is not stored (PushResult::Repeat; storing
//     it would freeze the interpolation between two identical points while the
//     velocity says "moving"). It still counts as proof of life and feeds the
//     clock estimate. The pose simply extrapolates (velocity zero for a standing
//     player: it holds).
//
//  3. Clock. Each accepted-or-repeated packet gives an arrival offset
//     a = recvMs - tick*tickMs, measured on the packet tick (the packet's real
//     send time). The baseline is the MINIMUM of the last kClockWindow (100)
//     offsets: the least-delayed packet. A stored snapshot's lateness is its
//     offset plus its ageMs (when its data became visible here, relative to the
//     time it shows); a repeat's lateness is its offset alone (it brings no data
//     to wait for). The 95th percentile of the lateness above the baseline is
//     the jitter (JitterMs) that drives the delay.
//
//  4. Adaptive delay. The pose shown is where the player was `delay` ms before
//     the baseline "now": delay = gap + p95(lateness) + 25 ms, clamped to
//     [75, 300] ms (1.5 .. 6 ticks), starting at 100 ms (2 ticks) until 16
//     packets are measured. `gap` is the median spacing of the placed times of
//     the last 16 STORED snapshots, at least one tick (so a client that sends at
//     10 Hz, or a player the server sends at its 10 Hz mid-range rate, gets a
//     100 ms gap; losses do not raise it: a lost packet is bridged
//     by extrapolation and the continuity rule below, not by more latency). The
//     delay rises at once and falls 2% of the gap-to-target per packet (about
//     2.5 s time constant), so a jitter burst is covered immediately and calm is
//     trusted only slowly. Why gap + p95: to interpolate you need the snapshot
//     AFTER the render time to have arrived; the worst case is one snapshot
//     spacing plus the lateness of that packet.
//
//  5. Render clock. The server-timeline render time `renderT` is a clock of its
//     own: it advances with the local clock and is steered toward
//     (now - baseline - delay) at no more than +-10% speed (proportional, 500 ms
//     time constant), so estimate changes never jump or reverse the pose; only a
//     500 ms-plus error resyncs hard (counted in Stats().resyncs).
//
//  6. Interpolation. Between the two snapshots around renderT: cubic Hermite on
//     position using the reported velocities as tangents (tangent length clamped
//     to 3x the chord, so inconsistent velocity can't make the ped swing out), or
//     linear when SmootherConfig::hermite is false. Heading is interpolated on
//     the shortest arc and always reported in (-180, 180]. Snapshots further
//     apart than maxBridgeMs (500) are not bridged: the older data is dropped.
//
//  7. Extrapolation. When renderT passes the newest snapshot (buffer dry): linear
//     position along the newest velocity for at most extrapolateMaxMs (250), then
//     hold at the position reached at the cap with velocity 0. Heading is held
//     (the protocol carries no turn rate).
//
//  8. Continuity. Whenever a new snapshot changes what the curve says at the
//     current render time (extrapolation prediction vs reality, a bridged gap),
//     the jump is not shown: it becomes a residual offset that decays with a
//     100 ms time constant (about 70 ms half-life). The same applies to a resync.
//
//  9. Teleport. A snapshot more than snapDistance (8 m, sized for on-foot players)
//     from where the newest one predicted it (linear along its velocity, time
//     capped at 250 ms) drops all history and shows at once (Pose::teleported on
//     the next Sample, nothing slides across the map). While the render time is
//     still before the new snapshot the pose holds there (PoseMode::Buffering,
//     about one delay).
//
// 10. Forgetting. A player with no packet at all for forgetAfterMs (5000) is
//     expired: Sample returns false and Prune removes it. The server's reliable
//     PlayerLeft and every id in WorldState::left should call Remove() first;
//     this is the safety net (an idle player still arrives at 1 Hz, so 5 s of
//     silence means it is really gone).
//
// 11. Ped state (protocol v3 WorldEntry::ped). Discrete, never interpolated: each
//     stored snapshot keeps its PedState and Pose::ped is that of the newest stored
//     snapshot at or before the render time (the newest while Extrapolating or
//     Holding; while Buffering, the newest one history dropped: a standing player's
//     long-idle snapshot cut by a bridge reset, or the oldest stored when none was
//     ever dropped), so a gait, flag or weapon change shows
//     when the position it came with does. jumpSeq is a counter, so a jump whose
//     snapshot the render time skipped (a slow frame, a resync) still shows as a
//     changed jumpSeq on the next one. A snapshot whose kinematics equal the newest
//     stored one but whose PedState differs is new information, not a Repeat (a
//     standing player who draws a weapon or starts aiming).
//
// 12. The shared clock (sync plan W7, libs/sync/netclock.h). Items 3 to 5 describe the clock a
//     STANDALONE PlayerSmoother keeps for itself (still the behaviour of a PlayerSmoother nobody
//     attached a clock to, and of RemoteSync with SmootherConfig::sharedClock false). RemoteSync's
//     default is one NetClock for the whole connection, fed by every WorldState (and, in the
//     engine, every VehicleWorld): the baseline is the minimum offset of 5 s of wall time, the delay
//     is tick + p95(age) + p95(excess) + half a tick, rises at once and falls with wall time, and
//     the render clock is slewed once for everybody. A player then draws
//         renderT = netClock.RenderT(now) - extra
//     where extra = (typical gap of ITS stored snapshots in whole ticks - 1) * tick (ClassExtraMs:
//     a mid-range player the server sends every 2nd tick is one tick behind a near one), itself
//     slewed at 10% (ClassDelay) so a player crossing a rate tier does not pop. This is the
//     only per-player adaptivity left. A remote car reads the same RenderT(now), so a passenger
//     and its car agree on the instant they show to a few ms. A hard resync of the shared clock is
//     absorbed per player like item 5's.
//
// 13. Position denoise (sync pipeline review, 2026-11-03). The wire carries a position in 1 cm steps (round to
//     nearest: +-0.5 cm per axis) and a velocity in 1/32 m/s steps (+-1.6 cm/s), so the velocity is 30 times
//     more precise than the position over one 33 ms tick (+-1 cm / 33 ms = +-0.3 m/s of chord noise against
//     +-0.05 m/s of velocity). The Hermite curve goes through the noisy positions and its derivative inherits the
//     chord noise: a ped walking at 1.8 m/s near the receiver showed a pose speed that wobbled by 6% (sd), p95 off
//     by 0.24 m/s, which the locomotion controller reads as gait and animation speed. A stored snapshot's position
//     is therefore blended toward where the newest stored snapshot says it should be by now (its position plus the
//     mean of the two reported velocities times the placed time between them: the trapezoid rule, exact for a
//     constant acceleration): position = predicted + denoiseGain * (measured - predicted), per axis, but only while
//     |measured - predicted| is inside the noise band (denoiseBandM + denoiseBandPerSpeedS * speed: the wire's
//     rounding of both samples plus the placement noise at that speed); beyond it the measurement is taken as it
//     is (a start, a stop, a turn, a hit: real motion is never smoothed). Repeat detection compares the RAW
//     samples (the filter changes what is stored, not what the player sent). Off when denoiseGain is 0 (the
//     struct default; ForTickHz, which the engine uses, sets 0.3: the chord noise of a still-moving ped drops by
//     about 4x for a 4 cm band at walking speed).
//
// ASSUMPTION: the server's tick pace equals the tickHz it announced. A server that
// cannot hold its rate shows up as jitter and pushes the delay toward the max.
// LIMITS of the placement: ageMs counts from the server's RECEIVE of the sample, so
// the client->server uplink latency is not in it (a constant offset of the whole
// timeline, harmless); it is whole milliseconds (placement error < 1 ms); and it
// saturates at 255, so a sample older than that is placed too late (idle re-sends,
// which are repeats anyway, or a link starved by the server's byte budget).
#pragma once

#include "protocol/protocol.h"
#include "sync/netclock.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gage::sync {

    constexpr size_t kSnapshotRing = 64;  // snapshots kept per player (3.2 s at 20 Hz)
    constexpr size_t kClockWindow  = 100; // arrival offsets in the clock/jitter window
    constexpr size_t kGapWindow    = 16;  // stored-snapshot spacings behind the typical gap

    struct SmootherConfig
    {
        double tickHz = 20.0;            // Welcome::tickHz

        double initialDelayMs = 100.0;   // 2 ticks
        double minDelayMs     = 75.0;    // 1.5 ticks
        double maxDelayMs     = 300.0;   // 6 ticks (at least)
        double delaySafetyMs  = 25.0;    // added to tick + p95 jitter (half a tick)

        double extrapolateMaxMs = 250.0; // extrapolate this long, then hold
        // W8 item 8: a newest snapshot that says the player stands (PedState move 0 and none of the flags under which a
        // zero gait still moves: see StandingStill) is extrapolated by at most this many metres (horizontal), whatever
        // its velocity says. A default / stale PedState carrying the last running velocity (the alt-tab fallback of the
        // sender used to send exactly that) used to carry the puppet 1.5 m past where it stopped, then snap back.
        // 0 (the struct default, and every standalone smoother of the older tests) turns the rule off;
        // ForTickHz, which the engine uses, sets it to 0.3 m.
        float  stillExtrapolateMaxM = 0.0f;
        // Item 13: velocity-aided position denoise. denoiseGain is the share of the (measured - predicted) position
        // difference that is believed (1 = the measurement as it is, 0 = off: the struct default); the correction
        // only applies inside the noise band denoiseBandM + denoiseBandPerSpeedS * |v| (metres, per axis), and
        // only across gaps up to denoiseMaxGapMs between stored samples. ForTickHz: 0.3, 0.015 m, 0.003 s, 200 ms.
        float  denoiseGain = 0.0f;
        float  denoiseBandM = 0.015f;
        float  denoiseBandPerSpeedS = 0.003f;
        double denoiseMaxGapMs = 200.0;
        double maxBridgeMs      = 500.0; // do not interpolate across a longer gap
        float  snapDistance     = 8.0f;  // metres from the prediction: teleport (on-foot players)
        double forgetAfterMs    = 5000.0;// silence that forgets a player

        bool   hermite = true;           // false: linear position interpolation

        double residualTauMs = 100.0;    // decay of an absorbed jump
        double slewTauMs     = 500.0;    // render clock steering time constant
        double maxSlew       = 0.10;     // +-10% render clock speed
        double resyncMs      = 500.0;    // render clock error that resyncs hard

        // RemoteSync only (item 12): true (the default, what the engine runs) = one NetClock for the connection,
        // read by every player (and by the vehicles); false = each player keeps its own clock (items 3 to 5).
        bool   sharedClock = true;

        // The tick-derived values for a server rate: initial 2 ticks, min 1.5,
        // max max(300 ms, 6 ticks), safety half a tick. hz outside [1, 1000]
        // (including 0 and NaN) falls back to 20.
        static SmootherConfig ForTickHz(double hz);
    };

    enum class PoseMode : uint8_t
    {
        Buffering,     // render time is before the oldest snapshot: held there
        Interpolating, // between two snapshots
        Extrapolating, // past the newest snapshot, within extrapolateMaxMs
        Holding,       // past the extrapolation cap: standing at the capped position
    };

    struct Pose
    {
        proto::Kinematics k;      // heading in (-180, 180]; velocity 0 while Buffering/Holding
        PoseMode mode = PoseMode::Buffering;
        bool teleported = false;  // true on the first Sample after a snap
        // The newest stored snapshot's PedState at or before the render time (see 11):
        // discrete, never interpolated. Defaults (idle, no flags) for v2-style pushes.
        proto::PedState ped;
        // The server tick this pose shows, fractional (tick + fraction of a tick),
        // not wrapped. now - (renderTick * tickMs on the local clock) is the
        // visual latency; a tool for diagnostics and for lag-compensated hit tests.
        double renderTick = 0;
    };

    enum class PushResult : uint8_t
    {
        Accepted,   // stored
        Teleported, // stored; history dropped (position jumped > snapDistance)
        Repeat,     // newer tick, but bit-identical state (kinematics and ped) or a sample placed no later
                    // than the newest stored one: liveness only, not stored
        Stale,      // duplicate or older tick: dropped
        Invalid,    // non-finite floats or time: dropped
    };

    struct SmootherStats
    {
        uint32_t accepted = 0, repeats = 0, stale = 0, invalid = 0;
        uint32_t teleports = 0;    // snapshots that dropped the history
        uint32_t bridgeResets = 0; // gaps > maxBridgeMs that dropped older data
        uint32_t resyncs = 0;      // hard render clock resyncs
        uint32_t extraChanges = 0; // class-extra changes after the first (ClassHold hysteresis; shared clock only)
        uint32_t denoised = 0;     // stored snapshots whose position the denoise pulled toward the prediction (item 13)
        // Split of `repeats` (pipeline diagnostics): the state was bit-identical to the newest stored one (a standing
        // player, an idle refresh, a sample forwarded twice) versus a sample placed no later than the newest stored one
        // although its state differs (the sender's timeline went backwards: the server's sample clock re-anchored).
        uint32_t repeatsIdentical = 0, repeatsEarlier = 0;
        uint32_t extrapolated = 0, held = 0; // Sample() calls that were Extrapolating / Holding (all frames since the start)
    };

    // The PedState of a player who is standing: move 0 and none of the flags under which a zero gait still moves
    // (the sender reports gait 0 while swimming, climbing, on a ladder, in cover, crouched, aiming or strafing,
    // ragdolling, falling, jumping, on a parachute, in a melee action or driving). Pure; used by the smoother's
    // extrapolation cap and by tests.
    inline bool StandingStill(const proto::PedState& p)
    {
        constexpr uint16_t kMoving = proto::kPfJumping | proto::kPfRagdoll | proto::kPfAiming | proto::kPfFalling |
                                     proto::kPfParachute | proto::kPfClimbing | proto::kPfLadder | proto::kPfCover |
                                     proto::kPfStealth | proto::kPfDriver | proto::kPfSwimming | proto::kPfMelee |
                                     proto::kPfVaulting;
        return p.move == proto::kMoveIdle && (p.flags & kMoving) == 0;
    }

    // One remote player.
    class PlayerSmoother
    {
    public:
        explicit PlayerSmoother(const SmootherConfig& cfg = SmootherConfig());

        // One snapshot of this player: the packet's tick, the state, the state's
        // age at that tick (WorldEntry::ageMs; the sample is placed at
        // tick*tickMs - ageMs), received at nowMs. A non-finite or negative ageMs
        // is Invalid. `ped` (WorldEntry::ped) is stored with the snapshot and shown
        // as Pose::ped.
        PushResult Push(uint32_t tick, const proto::Kinematics& k, double nowMs, double ageMs = 0.0,
                        const proto::PedState& ped = proto::PedState());

        // The pose to draw at nowMs, once per render frame. False: nothing to show
        // (no snapshot yet, or expired). Not const: it steers the render clock.
        bool Sample(double nowMs, Pose& out);

        // Reads the render time from a shared clock (item 12) instead of keeping one: Sample then needs the clock
        // to be Ready(), Push places snapshots on the clock's timeline (feed the clock the snapshot's frame first)
        // and the player's own delay estimate is just its class extra. nullptr (the default) = the standalone
        // smoother. The pointer must outlive the smoother (RemoteSync owns both and rebinds it when copied).
        void SetClock(NetClock* clock) { clock_ = clock; }
        bool SharedClock() const { return clock_ != nullptr; }

        bool HasData() const { return count_ > 0; }
        bool Expired(double nowMs) const { return nowMs - lastRecvMs_ > cfg_.forgetAfterMs; }
        // Current interpolation delay (shared clock: the clock's delay plus this player's class extra, as of the
        // last Sample).
        double DelayMs() const { return delayMs_; }
        // p95 data lateness (arrival + sample age) over the baseline (shared clock: the clock's p95 excess + p95 age).
        double JitterMs() const { return jitterMs_; }
        // The class extra in use (shared clock only; 0 for a standalone smoother).
        double ExtraMs() const { return extra_.Value(); }
        const SmootherStats& Stats() const { return stats_; }
        // The class the extra is held at (1 = one snapshot per tick, 2 = every 2nd tick ...; shared clock only).
        int ClassHeld() const { return extra_.HeldClass(); }
        // Pipeline diagnostics: the gaps between this player's STORED snapshots, as they arrived (local time), since
        // the last take (the entry rhythm the tier, the loss and the jitter make); the window restarts.
        ArrivalWindow TakeArrivals() { ArrivalWindow w = arrivals_; arrivals_.Reset(); return w; }
        // Longest run of consecutive Sample() calls that extrapolated, in ms of local time, since the last take.
        double TakeLongestExtrapolationMs() { const double m = longestExtrapMs_; longestExtrapMs_ = 0.0; return m; }

    private:
        struct Snapshot
        {
            double t = 0;          // placed time, ms on the server timeline (unwrapped tick * tickMs - ageMs)
            double recvMs = 0;     // local receive time
            uint32_t tick = 0;
            proto::Kinematics k;   // heading already normalised
            proto::PedState ped;
        };
        struct Val
        {
            double x = 0, y = 0, z = 0, h = 0, vx = 0, vy = 0, vz = 0;
            proto::PedState ped;   // the snapshot at or before t (not interpolated)
        };
        struct Residual { double x = 0, y = 0, z = 0, h = 0; };

        const Snapshot& At(size_t i) const { return ring_[(head_ + i) % kSnapshotRing]; }
        void Append(const Snapshot& s);
        bool Denoise(const Snapshot& prev, Snapshot& s) const; // item 13
        void NoteStored(double nowMs);
        void UpdateClock(double arrivalOffsetMs, double latenessMs);
        double TypicalGapMs() const;
        Val Evaluate(double t, PoseMode* mode) const;
        void Absorb(const Val& before, const Val& after);

        SmootherConfig cfg_;
        double tickMs_;

        Snapshot ring_[kSnapshotRing];
        size_t head_ = 0, count_ = 0;
        proto::Kinematics lastRaw_;   // the newest stored sample as the wire delivered it (the stored one may be denoised)
        ArrivalWindow arrivals_;      // gaps between stored snapshots' arrivals (diagnostics)
        double lastStoredRecvMs_ = 0, extrapRunStartMs_ = 0, longestExtrapMs_ = 0;
        bool haveStoredRecv_ = false, inExtrapRun_ = false;

        bool started_ = false;
        uint32_t originTick_ = 0; // the first tick seen: the origin of the server timeline
        uint32_t newestTick_ = 0;
        int64_t newestUtick_ = 0;

        double window_[kClockWindow]; // arrival offsets (recvMs - tick * tickMs)
        double late_[kClockWindow];   // the same packets' lateness (offset + age of new data)
        size_t wHead_ = 0, wCount_ = 0;
        double gaps_[kGapWindow];  // ms between the last stored snapshots
        size_t gHead_ = 0, gCount_ = 0;
        double baseMs_ = 0, jitterMs_ = 0, delayMs_;
        double lastRecvMs_ = 0;

        bool haveClock_ = false;
        double renderT_ = 0, lastSampleMs_ = 0;
        NetClock* clock_ = nullptr;   // the shared clock (item 12), or null
        ClassDelay extra_;            // shared clock: this player's class extra, slewed
        uint32_t seenResyncs_ = 0;    // shared clock: NetClock::Resyncs() as of the last Sample
        Residual res_;
        bool teleportPending_ = false;
        // The PedState of the newest snapshot that history dropped (ring eviction, a
        // bridge reset, a teleport): what is "at or before" a render time still before
        // the oldest stored snapshot (Buffering). Without one, Buffering shows the oldest.
        proto::PedState pedBefore_;
        bool havePedBefore_ = false;
        SmootherStats stats_;
    };

    // All remote players, keyed by server player id.
    class RemoteSync
    {
    public:
        explicit RemoteSync(const SmootherConfig& cfg = SmootherConfig());
        // The players hold a pointer to the clock this object owns: copies and moves rebind it.
        RemoteSync(const RemoteSync& o);
        RemoteSync(RemoteSync&& o);
        RemoteSync& operator=(const RemoteSync& o);
        RemoteSync& operator=(RemoteSync&& o);

        // Our own id (Welcome::yourId): its entries are skipped. The server already
        // leaves the receiver out of its WorldState; this is belt and braces.
        void SetLocalId(uint16_t id) { localId_ = id; hasLocalId_ = true; }

        // Feeds every entry of a WorldState (with its ageMs and ped), all stamped nowMs.
        // Entries with an id >= proto::kMaxPlayers are ignored. Returns the number
        // of entries stored. WorldState::left is NOT applied here: the caller
        // despawns those peds and calls Remove(id) for each.
        size_t OnWorldState(const proto::WorldState& ws, double nowMs);
        PushResult OnPlayerState(uint16_t id, uint32_t tick, const proto::Kinematics& k, double nowMs,
                                 double ageMs = 0.0, const proto::PedState& ped = proto::PedState());

        // The shared clock (SmootherConfig::sharedClock; item 12). OnWorldState / OnPlayerState feed it (the frame
        // once, the age of every STORED sample); a caller that filters entries itself (the engine) also calls
        // OnFrame for the packet, so a frame without a stored entry still measures the link. Vehicles read their
        // render time from the same object: Clock().RenderT(now) - class extra. A new session is a new RemoteSync
        // (or Clear(), which also resets the clock).
        NetClock& Clock() { return clock_; }
        const NetClock& Clock() const { return clock_; }
        bool OnFrame(uint32_t tick, double nowMs) { return clock_.OnFrame(tick, nowMs); }

        // Pipeline diagnostics of one player (see PlayerSmoother::TakeArrivals / TakeLongestExtrapolationMs): false for an
        // unknown player. The windows restart.
        bool TakeDiagnostics(uint16_t id, ArrivalWindow& arrivals, double& longestExtrapMs)
        {
            auto it = players_.find(id);
            if (it == players_.end()) return false;
            arrivals = it->second.TakeArrivals();
            longestExtrapMs = it->second.TakeLongestExtrapolationMs();
            return true;
        }

        // False for an unknown or expired player.
        bool Sample(uint16_t id, double nowMs, Pose& out);

        // Drops expired players; their ids go to `forgotten` if given. Returns the count.
        size_t Prune(double nowMs, std::vector<uint16_t>* forgotten = nullptr);
        void Remove(uint16_t id) { players_.erase(id); } // forget now (PlayerLeft, WorldState::left)
        void Clear() { players_.clear(); clock_.Reset(); }

        size_t Count() const { return players_.size(); }
        const PlayerSmoother* Find(uint16_t id) const;

    private:
        void Rebind();

        SmootherConfig cfg_;
        NetClock clock_;
        std::unordered_map<uint16_t, PlayerSmoother> players_;
        uint16_t localId_ = 0;
        bool hasLocalId_ = false;
    };

}
