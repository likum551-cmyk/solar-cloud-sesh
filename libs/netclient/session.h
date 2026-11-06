// ClientSession: one connection from a GAGE client (the in-game engine, the
// headless bot, the tests) to a GAGE server. It owns its ENet host, speaks
// protocol v5 and turns what arrives into an event queue the owner drains.
//
// World traffic (protocol v2 scheme; v3 entries also carry each player's PedState): the server
// sends only the players relevant to us, as deltas
// against frames we acknowledged (worldcodec.h). The session decodes them with a WorldDecoder,
// acknowledges what it decoded in every SendState(), and hands the owner fully decoded
// frames: each Event::World holds the entries the server chose to send in that frame (not
// necessarily everybody: a player absent from a frame is NOT gone, only a PlayerLeft event or
// an id in `world.left` ends one), and Event::Coarse the low-rate blips of the far players.
// A queue overflow merges frames instead of dropping them, so no entry is ever lost.
//
// v4: vehicles (Event::VehicleWorld per receiver, SendVehicleState for the driver, SendVehicleCmd
// answered by Event::VehicleResult), combat (SendHit, SendRespawn; Event::Damage / Killed /
// RespawnOk; WorldEntry::ped carries the server's health and armor) and emotes (SendAnim,
// SendScenario, SendStopAnim; Event::PeerAnim / PeerScenario / PeerAnimStop).
//
// Built to be linked into the engine DLL later, so: no global state (each
// session holds its own ENet scope), no stdout (state is reported through events
// and return values), and nothing blocks. Connect() and Poll() never wait longer
// than the timeout you pass, and a dead server ends in a Disconnected event
// after SessionOptions::connectTimeoutMs rather than hanging. The one exception
// is a host NAME in SessionOptions::host: resolving it is a blocking OS lookup,
// so pass an IPv4 literal or connect from a worker thread.
//
// Single-threaded: call everything from the thread that calls Poll().
#pragma once

#include "protocol/protocol.h"
#include "protocol/worldcodec.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

struct _ENetHost;
struct _ENetPeer;

namespace gage::net {

    class EnetScope;

    enum class SessionState
    {
        Idle,       // never connected
        Connecting, // ENet handshake in flight
        Joining,    // connected, Hello sent, waiting for Welcome or Refuse
        Joined,     // Welcome received; state, chat and world traffic flow
        Closing,    // Disconnect() asked, waiting for the server's ack (bounded)
        Closed,     // over; a Disconnected event was queued
    };

    enum class DisconnectReason
    {
        Local,            // we called Disconnect()
        Lost,             // the server went silent (or the link died)
        Kicked,           // the server kicked us
        Refused,          // the server refused our Hello (see the Refused event)
        Shutdown,         // the server is stopping
        ConnectFailed,    // no ENet connection to host:port
        HandshakeTimeout, // connected, but no Welcome/Refuse within connectTimeoutMs
    };

    struct SessionOptions
    {
        std::string host = "127.0.0.1"; // IPv4 literal or a host name (blocking lookup)
        uint16_t port = proto::kDefaultPort;
        std::string name;
        // v8: the platform join token sent in JoinAuth right after Hello ("" = none: the server then admits a guest or, with
        // server.json requireAccount, refuses AccountRequired). Single use on the platform side: a reconnect that sends the
        // same token again is refused AuthFailed by a server that requires accounts.
        std::string joinToken;

        // Budget from Connect() to Welcome/Refuse; past it the session closes with
        // ConnectFailed or HandshakeTimeout.
        uint32_t connectTimeoutMs = 10000;
        // Bound on a graceful Disconnect() waiting for the server's ack.
        uint32_t closeTimeoutMs = 1500;
        // ENet peer timeout (limit, minimum ms, maximum ms); 0 keeps ENet's
        // 32 / 5000 / 30000. Lower values detect a dead server sooner.
        uint32_t peerTimeoutLimit = 0;
        uint32_t peerTimeoutMinMs = 0;
        uint32_t peerTimeoutMaxMs = 0;

        // ---- test knobs (leave at defaults in the game) ----
        uint16_t protocolVersion = proto::kVersion; // version sent in Hello
        bool autoHello = true;                      // false: the caller sends Hello (and JoinAuth) via SendRaw
        bool autoJoinAuth = true;                   // false: autoHello sends Hello only (a v7-style client that never sends JoinAuth)
        uint32_t dropWorldPercent = 0;              // drop this % of incoming WorldState (before decoding: a dropped frame is never acknowledged)
        uint32_t dropSeed = 1;                      // seed for the drop pattern
    };

    struct Event
    {
        // New types are only ever appended (code may index tables by the value).
        enum class Type
        {
            Welcome, Refused, PlayerJoined, PlayerLeft, World, Chat, Disconnected, Coarse, PeerLook,
            VehicleWorld, VehicleResult, Damage, Killed, RespawnOk, PeerAnim, PeerScenario, PeerAnimStop, // v4
            SeatNotice, // T1 seats (wire v6)
            VehicleImpact,   // T4 (v6)
            LinkInfo,        // R3 T5: the server's round trip to each vehicle controller (about 1 Hz, unreliable)
            AccountInfo,     // v8: the account the server verified (closes the Welcome burst)
            AdminResult,     // v8: the answer to one SendAdminCmd
            InvState,        // v8 (0.6): our whole inventory + the last InvOp seq the server applied
            InvUsed,         // v8 (0.6): the server consumed one item for us (satiety for the client to add)
        };
        Type type = Type::Disconnected;

        // Exactly the member named by `type` is meaningful.
        proto::Welcome welcome;
        proto::Refuse refuse;
        proto::PlayerJoined joined;
        proto::PlayerLeft left;
        proto::WorldState world;    // entries ascending by id, dequantised (k + ped); `left`: ids out of range now
        proto::CoarseState coarse;  // far players, ~1 Hz, metres at 0.5 m precision
        proto::ChatMessage chat;
        // Player `look.id`'s appearance (passes ValidLook). Reliable and ordered: it always follows
        // that id's PlayerJoined. Sent on every change and, for a newcomer, once per player that
        // has a look, right after the PlayerJoined burst. The newest one per id is the look.
        proto::PeerLook look;
        DisconnectReason disconnect = DisconnectReason::Local;

        // ---- v4 ----
        // The vehicles relevant to us (proto::VehicleWorld: `present` is the complete set, `vehicles`
        // the full states sent this tick, `removed` the destroyed ones). Up to 20 a second; unread ones
        // are merged past 4 like World (newest full state per id, the newest present set, removed the
        // union minus re-appearances).
        proto::VehicleWorld vehicles;
        proto::VehicleResult vehicleResult; // the answer to one of our VehicleCmds
        proto::Damage damage;               // a health changed: ours (victimId == myId) or our victim's
        proto::Killed killed;               // someone died (kill feed)
        proto::RespawnOk respawn;           // we are alive again (position, heading, health, armor)
        proto::PeerAnim anim;               // player anim.id plays an animation
        proto::PeerScenario scenario;       // player scenario.id starts a scenario
        proto::PeerAnimStop animStop;       // player animStop.id stopped its animation / scenario
        proto::SeatNotice seatNotice;       // T1 seats (v6): a player's enter / exit intent on a vehicle seat (reliable, in order)
        proto::VehicleImpact impact;        // T4: the server's resolution of a contact of a vehicle WE control (reliable, once per contact)
        proto::LinkInfo linkInfo;           // R3 T5: the round trip (ms) the server measures to each vehicle controller
        proto::AccountInfo account;         // v8: kAcct* flags and the verified username ("" for a guest)
        proto::AdminResult adminResult;     // v8: the server's answer to one of our AdminCmds
        proto::InvState invState;           // v8 (0.6): blob = sync::inv::Encode(Character), ackSeq = the last InvOp applied or refused
        proto::InvUsed invUsed;             // v8 (0.6): the item used and the satiety it gives
    };

    class ClientSession
    {
    public:
        ClientSession();
        ~ClientSession(); // best-effort graceful quit, never blocks
        ClientSession(const ClientSession&) = delete;
        ClientSession& operator=(const ClientSession&) = delete;

        // Starts a non-blocking connect; progress arrives through Poll()/events.
        // False (with *err) when the host cannot be created or resolved.
        bool Connect(const SessionOptions& opts, std::string* err = nullptr);

        // Services the network for at most `timeoutMs` (0 = just drain what is
        // ready), then runs the deadlines. Call every frame / loop.
        void Poll(uint32_t timeoutMs = 0);

        // Pops the oldest event; false when the queue is empty. Unread World events are
        // capped (4): past that the two oldest are MERGED (newer entries win, the leave
        // list is the union minus re-entries), because frames are partial and dropping one
        // would lose the only sighting of an idle player. Unread Coarse events are merged
        // the same way (newest entry per id).
        bool NextEvent(Event& out);

        // Our movement and on-foot state, unreliable on the state channel, carrying the receipt
        // for the world frames decoded so far. False when not Joined or when `k` fails
        // ValidKinematics or `ped` fails ValidPedState (the server would drop it anyway).
        // `ped` defaults to "standing, nothing happening, weapon 0". v5: `sampleMs` is the millisecond (modulo
        // 65536, any monotonic clock of the caller's) at which `k` and `ped` were SAMPLED; the server places it on
        // its own clock (libs/sync/sampleclock.h) so a WorldState age is the sample's, not the packet's. -1 (default)
        // stamps the moment of this call, which is right for a caller that samples and sends in the same breath.
        bool SendState(const proto::Kinematics& k, const proto::PedState& ped = proto::PedState(), int32_t sampleMs = -1);
        // Our appearance, reliable. The server stores it, tells everyone else (PeerLook) and every
        // later joiner; it passes on at most 2 changes a second per player (the newest wins).
        // False when not Joined or when `look` fails ValidLook. A look equal to the last one sent
        // on this connection is not sent again (returns true).
        bool SendLook(const proto::Look& look);
        // False when not Joined or when `text` fails ValidChat (the server would
        // reject it, and an over-cap message would get us disconnected).
        bool SendChat(const std::string& text);

        // ---- v4 ----
        // The vehicle we drive, unreliable on the state channel (call it in the same send tick as
        // SendState). The session numbers it (u.seq is ignored) and stamps u.sampleMs (v5: `sampleMs` as in
        // SendState, -1 = the moment of this call; u.sampleMs is ignored). The server takes it only from the
        // vehicle's controller (v4: the player in seat 0 of u.vehId; v5: also a nearby player the server picked
        // for a driverless vehicle, VehicleState::ctrl). False when not Joined or when `u` fails ValidVehicleUpdate.
        bool SendVehicleState(const proto::VehicleUpdate& u, int32_t sampleMs = -1);
        // A vehicle request (spawn, enter, exit, lock, lights, colours, register), reliable. Every one
        // is answered with an Event::VehicleResult. False when not Joined or the op is unknown.
        bool SendVehicleCmd(const proto::VehicleCmd& c);
        // One hit our game registered (reliable). False when not Joined or damage is 0.
        bool SendHit(const proto::Hit& h);
        // Ask to come back after dying (reliable). Answered by Event::RespawnOk, or ignored before the
        // server's respawn delay: send it again (about once a second is plenty).
        bool SendRespawn();
        // Our emote (reliable). False when not Joined or the anim fails ValidAnim / the name
        // ValidAnimName. The server relays at most 4 a second.
        bool SendAnim(const proto::Anim& a);
        bool SendScenario(const std::string& name);
        bool SendStopAnim();
        // v8: ask the server whether we may run an admin command (reliable). Every one is answered with an
        // Event::AdminResult carrying the same seq and op. False when not Joined or the arg is over kMaxAdminArg.
        bool SendAdminCmd(const proto::AdminCmd& c);
        // v8 (0.6): one inventory operation (reliable; `op` = sync::inv::EncodeOp). Every one is answered with an
        // Event::InvState whose ackSeq is o.seq. False when not Joined, seq is 0 or op is not kInvOpBytes long.
        bool SendInvOp(const proto::InvOp& o);
        // Sends bytes as they are, bypassing every check. For tools and tests that
        // build deliberately bad packets. False when there is no live connection.
        bool SendRaw(uint8_t channel, const proto::Bytes& bytes, bool reliable);

        // Graceful quit: tells the server (PlayerLeft::Quit), then Closed with a
        // Disconnected(Local) event. Bounded by closeTimeoutMs.
        void Disconnect();
        // Drops the connection without telling the server, like a crash or a
        // pulled cable: the server sees a timeout. No event is queued.
        void Abandon();

        SessionState state() const { return state_; }
        uint16_t myId() const { return myId_; }
        uint16_t tickHz() const { return tickHz_; }
        const std::string& serverName() const { return serverName_; }
        // v5: the server's maximum health (Welcome.playerHealth); proto::kDefaultHealth until Welcome arrives.
        uint8_t playerHealth() const { return playerHealth_; }
        // v8: the newest AccountInfo of this connection (flags 0 and no name until it arrives, and for a guest).
        uint8_t accountFlags() const { return account_.flags; }
        const std::string& accountName() const { return account_.username; }
        uint32_t rttMs() const;
        // R3 T5: the round trip the near-car lead uses: ENet's smoothed value, or the quietest of its last interval when lower (the SampleClock's
        // constant is the fastest path, not the mean). 0 without a peer.
        uint32_t lowRttMs() const;
        uint32_t lastSentSeq() const { return seq_; }

        // ENet's own view of the link (diagnostics, sync pipeline review 2026-11-03). ENet silently drops a share of
        // UNRELIABLE packets (our state channel) when `throttle` is below `throttleScale`: it lowers the throttle each
        // time a round trip is much slower than the last 5 s's quietest, and a deceleration of 0 (what the server
        // configures for every peer since that review) holds it at the scale. lossPercent is ENet's smoothed loss of
        // RELIABLE packets (a retransmission), rttVarMs its smoothed round-trip deviation.
        struct LinkInfo
        {
            uint32_t rttMs = 0, rttVarMs = 0;
            uint32_t throttle = 0, throttleScale = 32, throttleDeceleration = 0;
            double lossPercent = 0;
        };
        LinkInfo link() const;

        // Counters for diagnostics and tests.
        // WorldState packets that arrived, including those the dropWorldPercent knob discarded.
        uint64_t worldReceived() const { return worldReceived_; }
        uint64_t worldDropped() const { return worldDropped_; }   // discarded by the knob
        // Packets that were not valid messages for this state (a world frame the decoder
        // rejected counts here too; a stale or duplicate one does not).
        uint64_t malformed() const { return malformed_; }
        // The receipt the next SendState() will carry.
        proto::WorldAck worldAck() const { return decoder_.ack(); }

        // Traffic counters, cumulative since Connect(): the receive side of the load test.
        struct Counters
        {
            uint64_t wireBytesIn = 0;      // bytes in the UDP datagrams received (ENet headers included, IP/UDP not)
            uint64_t wireDatagramsIn = 0;  // UDP datagrams received
            uint64_t wireBytesOut = 0;     // bytes in the UDP datagrams sent
            uint64_t wireDatagramsOut = 0;
            uint64_t worldPackets = 0;     // WorldState packets decoded
            uint64_t worldBytes = 0;       // their payload bytes
            uint64_t worldEntries = 0;     // player entries in them
            uint64_t worldDeltaEntries = 0;    // of which delta-coded
            uint64_t worldAbsoluteEntries = 0; // of which sent in full
            uint64_t worldRejected = 0;    // frames the decoder refused (malformed, unresolved baseline)
            uint64_t worldStale = 0;       // frames older than the newest accepted
            uint64_t coarsePackets = 0;    // WorldCoarse packets decoded
            uint64_t coarseBytes = 0;
            uint64_t coarseEntries = 0;
            uint64_t socketErrors = 0;     // enet_host_service calls that failed (socket send/receive error)
            int lastSocketError = 0;       // WSAGetLastError()/errno right after the latest such failure
        };
        Counters counters() const;

    private:
        struct Impl; // DropSim lives here so this header stays free of ENet
        void HandleReceive(uint8_t channel, const uint8_t* data, size_t len);
        void HandleDisconnect(uint32_t data);
        void Finish(DisconnectReason why, bool queueEvent);
        void Push(Event&& e);
        void SampleWire(); // folds ENet's 32-bit traffic counters into counters_
        bool SendBytes(uint8_t channel, const proto::Bytes& bytes, bool reliable);

        std::unique_ptr<EnetScope> enet_;
        std::unique_ptr<Impl> impl_;
        proto::WorldDecoder decoder_;
        _ENetHost* host_ = nullptr;
        _ENetPeer* peer_ = nullptr;
        SessionOptions opts_;
        SessionState state_ = SessionState::Idle;
        bool refused_ = false;
        uint64_t deadlineMs_ = 0;
        uint16_t myId_ = 0;
        uint16_t tickHz_ = 0;
        uint8_t playerHealth_ = proto::kDefaultHealth;
        std::string serverName_;
        proto::AccountInfo account_;
        uint32_t seq_ = 0;
        uint32_t vehSeq_ = 0; // VehicleUpdate numbering (its own sequence)
        bool lookSent_ = false;
        proto::Look lastLook_;
        std::deque<Event> events_;
        uint64_t worldReceived_ = 0, worldDropped_ = 0, malformed_ = 0;
        Counters counters_;
        uint32_t lastWireIn_ = 0, lastWireInPackets_ = 0, lastWireOut_ = 0, lastWireOutPackets_ = 0;
    };

}
