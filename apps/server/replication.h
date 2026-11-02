// Replicator: decides, every server tick, WHAT each client is told about the other players and
// builds the packets. Interest management (a uniform spatial grid, a per-receiver relevance set
// with hysteresis, speed-aware radii, an always-relevant override), variable send rates (a
// priority accumulator per receiver/subject pair under a per-client byte budget) and the
// protocol v2 delta frames all live here. It is pure computation over the data it is fed: no
// sockets, no clocks of its own, so tests can run a thousand simulated players through it and
// decode what comes out (tests/interest_test.cpp).
//
// The design in one paragraph. Each tick the Replicator (1) snapshots every player's state into a
// quantised QState (kinematics + on-foot state) and a 32-tick history, (2) for each receiver finds
// newly relevant players through the grid (every relevanceEveryMs) and drops those that moved past
// the leave radius, (3) lets every
// relevant pair accumulate priority at the rate its distance class allows (near/mid/far/idle Hz),
// (4) sends, in priority order, as many due pairs as the receiver's byte budget and one
// non-fragmenting packet allow, each as a delta against the newest state the receiver has
// acknowledged (or in full), and (5) sends a rolling page of coarse positions of everyone who is not
// relevant, about once a second each. See docs/api/server.md and docs/api/protocol.md.
//
// Thread-safety: none; call everything from the thread that drives Server::Poll(). Errors: none
// throw; bad ids make a call return false or do nothing.
#pragma once

#include "metrics.h"
#include "protocol/protocol.h"
#include "protocol/quant.h"
#include "protocol/worldcodec.h"
#include "replconfig.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace gage::server {

    class Replicator
    {
    public:
        // Receives each finished packet: the receiver's player id, whether it is a WorldCoarse (true)
        // or WorldState (false) packet, and its bytes (valid only during the call).
        using Sink = std::function<void(uint16_t receiver, bool coarse, const proto::Bytes& packet)>;

        Replicator();
        ~Replicator();
        Replicator(const Replicator&) = delete;
        Replicator& operator=(const Replicator&) = delete;

        // Applies `cfg` and forgets every player. Values are clamped into their working ranges
        // (rates to 1..tickHz, radii to enter <= leave, maxPlayers to 1..kMaxPlayers); Validate()
        // in config.cpp is what rejects bad server.json values with a message.
        void Configure(const ReplConfig& cfg);
        const ReplConfig& config() const { return cfg_; }

        // A player joined / left. Ids outside 0..maxPlayers-1 are ignored. A removed id is purged
        // from every receiver's relevance set, so a reused id starts clean.
        void AddPlayer(uint16_t id);
        void RemovePlayer(uint16_t id);

        // The player's newest accepted state, received at `nowUs` (the same clock as Tick). The
        // state is quantised here; the player first appears in other clients' worlds after this
        // call and a following Tick. Ignored for an id that is not a player. `ped` is the on-foot
        // state of the same sample (validated by the caller); a change in it alone counts as
        // activity for the idle rate, exactly like a movement.
        //
        // Idle dead-band (sync plan N4): a sample is "the same" for the idle rate while it stays within
        // 2 cm per axis, 2 velocity steps (1/32 m/s) per axis and 36 heading units (about 0.2 degrees) of the
        // reference, with the on-foot state equal except the aim, which may move 3 dm per axis. The
        // reference is the last sample that left the band, and while the sample stays in it the reference
        // (not the newest sample) is what the player's WorldState entries and delta history say, byte for
        // byte. The coarse (out of range) blips are not affected: they are 0.5 m steps of the newest sample.
        // Leaving the band wakes the player (its pairs are due at once) and the sample becomes the new
        // reference, so a slow creep cannot hide in the band and nothing is ever more than the band
        // off the truth. Any change of move, flags, weapon, jump or shot counter, health or armor is outside it.
        void SetState(uint16_t id, const proto::Kinematics& k, const proto::PedState& ped, uint64_t nowUs);
        // Kinematics only: the ped part is the default (idle, no flags, weapon 0).
        void SetState(uint16_t id, const proto::Kinematics& k, uint64_t nowUs) { SetState(id, k, proto::PedState(), nowUs); }

        // The WorldState receipt the player sent in its newest accepted PlayerState.
        void SetAck(uint16_t id, const proto::WorldAck& ack);

        // Gamemode API: makes `subject` always relevant to `receiver` (it is sent whatever the
        // distance, at the far rate, and never leaves), or to every player including those who join
        // later when `receiver` is proto::kServerId. `on == false` undoes it (the pair then follows
        // the normal radii); with kServerId it clears the override of every receiver, also ones set
        // individually. Returns false when `subject` is not a player, when `receiver` is
        // neither a player nor kServerId, or when subject == receiver (nobody is told about itself).
        bool SetAlwaysRelevant(uint16_t subject, uint16_t receiver, bool on);
        // True when the override is on for that pair (directly or through kServerId).
        bool AlwaysRelevant(uint16_t subject, uint16_t receiver) const;

        // One server tick `tick` (numbered from 1) at time `nowUs`: refresh, decide, build, and call
        // `sink` for every packet. `m` receives the replication counters. `tick` follows wall time and may
        // jump forward after a server stall (see tickclock.h): the per-pair history masks move by the jump,
        // so the frame sent just before it is still a usable delta baseline. `nowUs` is the time of the
        // tick's own slot; sample ages (WorldEntry::ageMs) are measured against it.
        void Tick(uint32_t tick, uint64_t nowUs, const Sink& sink, Metrics& m);

        // ---- introspection (tests, stats) ----
        // True when `subject` is in `receiver`'s relevance set right now.
        bool Relevant(uint16_t receiver, uint16_t subject) const;
        size_t RelevantCount(uint16_t receiver) const;
        size_t TotalRelevantPairs() const;
        // The enter radius for two players moving at speeds a and b (m/s): the cap-limited formula
        // of ReplConfig. The leave radius is this plus (leaveRadius - enterRadius).
        float EnterRadius(float speedA, float speedB) const;

    private:
        struct Pair
        {
            uint32_t mask = 0;          // bit k: the subject was sent to this receiver in the frame k ticks ago
            float acc = 0;              // priority accumulator (1.0 = an update is due)
            uint32_t lastSent = 0;      // tick of the last WorldState entry (0 = never): the tie-break among starved pairs
            uint32_t coarseTick = 0;    // tick of the last coarse entry (0 = never)
            proto::QCoarse lastCoarse;  // what that entry said
            uint16_t relIdx = 0;        // index in Receiver::rel while relevant
            uint8_t flags = 0;          // kRelevant | kForced
        };
        struct PendingLeave { uint16_t id; uint8_t remain; };
        struct Receiver
        {
            bool active = false;
            std::vector<Pair> pair;           // by subject id
            std::vector<uint16_t> rel;        // subjects relevant now (unordered)
            std::vector<PendingLeave> leaves; // to tell the client about (repeated a few frames)
            float tokens = 0;                 // byte budget available now
            proto::WorldAck ack;
            uint16_t coarseCursor = 0;
        };
        struct Subject
        {
            bool active = false, hasState = false, forcedAll = false, woke = false;
            proto::Kinematics k;
            // q: the newest sample. refQ: the reference the idle dead-band is measured from and the state that is
            // REPLICATED (delta history and WorldState entries): the last sample that left the band, so while a
            // standing player's sample only flickers inside the band every tick re-sends the same bytes. prevQ: the previous
            // tick's sample, kept only to count the wake-ups a bit-exact idle test would have caused.
            proto::QState q, refQ, prevQ;
            uint32_t idleTicks = 0;  // ticks the sample has stayed inside the band of refQ
            uint32_t exactIdle = 0;  // ticks the sample has been bit-identical to the previous tick's (diagnostics only)
            uint32_t seq = 0;        // samples received so far (SetState calls)
            uint32_t refSeq = 0;     // the sample refQ came from
            uint32_t fwdSeq = 0;     // the refSeq last counted as forwarded
            uint32_t fwdTick = 0;    // the tick that was counted for it
            uint64_t recvUs = 0;
            float speed = 0;
            uint8_t ageMs = 0;
            int32_t cell = -1;
            uint32_t cellPos = 0;
            uint32_t histValid = 0;
            uint32_t histTick[proto::kWorldHistory] = {};
            proto::QState histQ[proto::kWorldHistory];
        };
        struct Cand
        {
            uint16_t id;
            float acc, d2;
            const proto::QState* base;
            uint8_t baseAge;
            proto::EntryPlan plan;
            size_t bytes;
            uint32_t lastSent;
        };

        void ProcessReceiver(uint16_t r, uint32_t tick, const Sink& sink, Metrics& m);
        void FindEnters(uint16_t r);
        void AddPair(uint16_t r, uint16_t s, bool forced);
        void DropPair(uint16_t r, uint16_t s, bool notify);
        void RemoveFromRel(Receiver& R, uint16_t s);
        void PurgeSubject(uint16_t id);
        void GridMove(uint16_t id);
        void GridRemove(uint16_t id);
        int CellCoord(float v) const;
        const proto::QState* FindHist(const Subject& s, uint32_t tick) const;
        size_t BuildCoarse(uint16_t r, uint32_t tick, size_t maxBytes, const Sink& sink, Metrics& m);

        ReplConfig cfg_;
        uint32_t tick_ = 0;
        uint32_t step_ = 1;        // ticks since the previous Tick() call (> 1 after a server stall)
        uint64_t received_ = 0;    // SetState calls not yet folded into Metrics::samplesReceived
        uint64_t nowUs_ = 0;
        std::vector<Subject> subj_;
        std::vector<Receiver> recv_;
        std::vector<uint16_t> active_;
        std::vector<std::vector<uint16_t>> cells_;
        int gridW_ = 1;
        float perTick_ = 0, tokenCap_ = 0;
        uint32_t relEveryTicks_ = 1, idleAfterTicks_ = 1, cycleTicks_ = 1, pageEveryTicks_ = 1, coarseStep_ = 1,
                 coarseRefreshTicks_ = 1;
        float incNear_ = 1, incMid_ = 1, incFar_ = 1, incIdle_ = 1;
        float hysteresis_ = 50;
        uint64_t enters_ = 0, leaves_ = 0;
        proto::WorldPacketBuilder builder_;
        std::vector<Cand> cands_, picked_;
        std::vector<proto::CoarseWire> page_;
    };

}
