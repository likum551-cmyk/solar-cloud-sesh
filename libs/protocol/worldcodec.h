// The WorldState / WorldCoarse wire codec (v2 scheme, v3 entries that also carry the on-foot
// state): delta entries against an acknowledged baseline (server side: WorldPacketBuilder; client
// side: WorldDecoder) and the coarse channel. docs/api/protocol.md has the byte layouts; this
// header is the API.
//
// How the delta scheme stays correct under loss (the whole point of the design):
//   * Both sides keep, per player id, the last kWorldHistory (32) ticks of the QState they sent /
//     decoded for that id (the "history"). A history entry for tick T exists only if the frame of
//     tick T carried that player.
//   * Every PlayerState tells the server which WorldState ticks the client fully decoded
//     (WorldAck). The server may use "player P as sent in frame T-k" as a baseline for P only if
//     it sent P in that frame AND the ack lists that frame. The entry names the baseline by its age k
//     in ticks (1..31), 0 meaning "no baseline: the whole state follows".
//   * So a baseline always exists, bit for bit, on both sides; a lost frame is simply never
//     acknowledged and never used. A client that cannot resolve a baseline (a bug, a hostile
//     server) rejects the whole frame and does not acknowledge it.
// Server ticks start at 1; WorldAck.tick == 0 means "nothing received".
#pragma once

#include "protocol/protocol.h"
#include "protocol/quant.h"
#include "protocol/wire.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gage::proto {

    // ---- planning and sizes (the server budgets with these before it encodes) ----

    // Change bits of a delta entry (v3). The header byte holds the kinematics ones (bits 5..7 of
    // the byte: kChgPos << 5 ...); the extension byte that follows the age byte in every delta
    // entry holds the on-foot ones.
    constexpr uint8_t kChgPos = 1, kChgHead = 2, kChgVel = 4;                  // header byte, >> 5
    constexpr uint8_t kExtMove = 1, kExtFlags = 2, kExtWeapon = 4, kExtAim = 8; // extension byte (v3)
    constexpr uint8_t kExtShot = 16, kExtVitals = 32;                            // extension byte (v4)
    // v5: the vehicle id of a ped that is entering or exiting one (move 5 / 6): a u8 right after the move byte,
    // present only when the id differs from the baseline's (the baseline's id is 0 when its move named no vehicle).
    // Legal only when the RESULTING move is 5 or 6. Bit 7 is 0.
    constexpr uint8_t kExtVeh = 64;

    // How one entry will be written.
    struct EntryPlan
    {
        bool delta = false;       // false: absolute (16-byte kinematics + 8..15-byte ped state + 2-byte vitals), baseAge written as 0
        uint8_t changeBits = 0;   // delta only: kChgPos / kChgHead / kChgVel
        uint8_t extBits = 0;      // delta only: kExtMove / kExtFlags / kExtWeapon / kExtShot / kExtAim / kExtVitals / kExtVeh
        size_t payloadBytes = 26; // bytes after the id gap, header byte and age byte (a delta's
                                  // extension byte included); 26 = an absolute entry, not aiming
    };

    // Picks the cheaper of "delta against `base`" and "absolute". `base` null means absolute.
    // A delta that would cost as many bytes as the absolute form (26, 27 entering / exiting a vehicle, 6 more
    // while aiming) or more is written absolute instead.
    EntryPlan PlanEntry(const QState& cur, const QState* base);
    // Kinematics-only convenience (tools, tests): the ped part is the default (idle, no flags,
    // weapon 0, full health), so the absolute form is 26 bytes.
    EntryPlan PlanEntry(const QKin& cur, const QKin* base);

    // Bytes of the LEB128 form of `v` (1..5).
    size_t VarintBytes(uint32_t v);

    // Exact encoded size of one entry: the id-gap varint, flags, age and payload.
    size_t EntryBytes(uint32_t idGap, const EntryPlan& plan);

    // Exact size of the WorldState header for `nLeft` leave ids and `nEntries` entries.
    size_t WorldHeaderBytes(size_t nLeft, size_t nEntries);

    // The baseline ages (bit k set = tick - k, k in 1..31) a server may use for a frame at `tick`
    // given what the client acknowledged. 0 when the ack is unusable: nothing acknowledged yet,
    // an ack from the future (a buggy or hostile client), or one older than the 32-frame window.
    uint32_t AckedAges(const WorldAck& ack, uint32_t tick);

    // ---- server side ----

    // Builds one WorldState packet. Usage: Begin(tick), AddLeave(...) for ids that left the
    // receiver's relevance range, AddEntry(...) in STRICTLY ASCENDING id order, then Finish().
    // Reusable: Begin() clears it. Not thread-safe.
    class WorldPacketBuilder
    {
    public:
        // Starts a packet for server tick `tick` (>= 1).
        void Begin(uint32_t tick);
        // Tells the receiver `id` is no longer relevant (at most kMaxLeavePerPacket; extras are ignored).
        void AddLeave(uint16_t id);
        // Adds the state `cur` of player `id`. With `base` (the state sent baseAge ticks ago,
        // 1..kMaxBaseAge, and acknowledged) the entry is a delta when that is smaller; otherwise it
        // is absolute. `ageMs` is the sample age (saturated to 255 by the caller). Returns false and
        // adds nothing when `id` does not exceed the previous id or is >= kMaxPlayers. If
        // `wasDelta` is non-null it receives whether a delta was written.
        bool AddEntry(uint16_t id, uint8_t ageMs, const QState& cur, const QState* base, uint8_t baseAge,
                      bool* wasDelta = nullptr);
        // Kinematics-only convenience: the ped part of `cur` and `base` is the default.
        bool AddEntry(uint16_t id, uint8_t ageMs, const QKin& cur, const QKin* base, uint8_t baseAge,
                      bool* wasDelta = nullptr);
        // Entries added so far.
        size_t entryCount() const { return count_; }
        // Bytes Finish() would return now.
        size_t size() const;
        // The finished packet (type byte first). Valid until the next Begin().
        Bytes Finish();

    private:
        uint32_t tick_ = 0;
        std::vector<uint16_t> left_;
        wire::Writer entries_;
        size_t count_ = 0;
        int32_t prevId_ = -1;
    };

    // ---- client side ----

    // Decodes WorldState packets and keeps the per-player history that delta entries refer to,
    // plus the receipt (WorldAck) for the server. One per connection. Not thread-safe.
    class WorldDecoder
    {
    public:
        WorldDecoder();
        ~WorldDecoder();
        WorldDecoder(const WorldDecoder&) = delete;
        WorldDecoder& operator=(const WorldDecoder&) = delete;

        // Forgets everything (a new connection).
        void Reset();

        // Decodes one WorldState packet (type byte included). True: `out` is the frame (ascending
        // ids, dequantised, dropped-range ids in `left`), the history now holds its entries and the
        // ack covers its tick. False: malformed, a value outside the quantiser box, an entry whose
        // baseline this decoder does not have, a tick that is not newer than the newest accepted
        // (stale or duplicate), or a count/id over its cap; nothing was changed. `out` is untouched
        // on failure.
        bool Decode(const uint8_t* p, size_t n, WorldState& out);

        // The receipt to put in the next PlayerState.
        WorldAck ack() const { return WorldAck{ackTick_, ackBits_}; }

        uint64_t framesDecoded() const { return frames_; }
        uint64_t framesRejected() const { return rejected_; }    // malformed or unresolved baseline
        uint64_t framesStale() const { return stale_; }          // not newer than the newest accepted
        uint64_t deltaEntries() const { return deltaEntries_; }
        uint64_t absoluteEntries() const { return absoluteEntries_; }

    private:
        struct Hist;
        std::vector<std::unique_ptr<Hist>> hist_; // by player id, allocated on first sight
        uint32_t ackTick_ = 0, ackBits_ = 0;
        bool haveAck_ = false;
        uint64_t frames_ = 0, rejected_ = 0, stale_ = 0, deltaEntries_ = 0, absoluteEntries_ = 0;
    };

    // ---- the coarse channel ----

    // One entry of a WorldCoarse packet as the server holds it.
    struct CoarseWire { uint16_t id = 0; QCoarse q; };

    // Bytes of one coarse entry whose id is `idGap` above the previous id (gap varint + 6).
    size_t CoarseEntryBytes(uint32_t idGap);
    // Bytes of a WorldCoarse packet header (type, tick, entry-count varint).
    size_t CoarseHeaderBytes(size_t nEntries);

    // Encodes a WorldCoarse packet from entries in STRICTLY ASCENDING id order; empty result
    // when they are not or an id is >= kMaxPlayers.
    Bytes EncodeCoarse(uint32_t tick, const std::vector<CoarseWire>& entries);

}
