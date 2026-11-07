#pragma once
#include "common/common.h"
#include "protocol/protocol.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gage::join {

    // The engine's connection to a GAGE server: a thin adapter over
    // gage::net::ClientSession (libs/netclient/session.h, protocol v1, ENet over
    // UDP; docs/ENGINE-PLAN.md has the message table).
    //
    // ClientSession is single-threaded and non-blocking. This adapter gives it one
    // worker thread that owns it (connect, poll, send, receive), so the game's
    // main thread and the mp.* bridge only enqueue outgoing messages and read
    // atomics. Sinks run on that worker thread.

    struct Config
    {
        std::wstring host;            // "1.2.3.4" or a hostname
        int          port = proto::kDefaultPort; // the game (UDP) port
        std::wstring player = L"player";
        // v8: the single-use join token the launcher got from the platform (POST /v1/game/join) and handed over in
        // HKCU\Software\GAGE-MP join_token; sent in JoinAuth right after Hello. Empty = join as a guest (the server
        // admits one only while its requireAccount is off). Never logged.
        std::string  joinToken;
    };

    // Whether a Refuse is the end of the engine's connecting (the player has to act: update the game, change the name, ask an
    // admin, report a protocol bug) or something the server lifts by itself, so the engine keeps trying. NameTaken is the
    // ghost of our own dropped connection (the server holds it for its peer timeout, 5..30 s, and a reconnect inside that
    // window is refused as a duplicate name), ServerFull and TooManyConnections clear as slots free and the per-address
    // connect bucket refills, HelloTimeout is a lost packet. Giving up on those left a player whose Wi-Fi blinked
    // staring at a dead loading screen until the game was restarted.
    constexpr bool RefusalIsFinal(proto::RefuseReason r)
    {
        return !(r == proto::RefuseReason::NameTaken || r == proto::RefuseReason::ServerFull ||
                 r == proto::RefuseReason::HelloTimeout || r == proto::RefuseReason::TooManyConnections);
    }
    // How long the engine waits before the next attempt after a refusal it does not treat as final (one attempt costs the
    // server a connection and counts against its per-address connect rate, whose default is well above one per 5 s).
    constexpr unsigned kRefusedRetryMs = 5000;

    // The result of one Connect().
    struct Session
    {
        bool           connected = false;
        bool           refused = false;   // the server refused us (version, name, full); RefusalIsFinal(refuseReason) says whether retrying can help
        uint16_t       myId = 0;
        uint16_t       tickHz = 0;
        std::string    serverName;
        uint8_t        playerHealth = proto::kDefaultHealth; // v5: the server's maximum health (Welcome.playerHealth)
        std::wstring   error;             // short reason when !connected
        // Set only when refused: the server's Refuse, verbatim. refuseText is the server's player-facing sentence
        // (for VersionMismatch: "Solar RP was updated. Close the game; Solar Launcher installs the update." or
        // "This server has not been updated yet. Try again in a minute."), shown to the player by the engine.
        proto::RefuseReason refuseReason = proto::RefuseReason::Protocol;
        std::string    refuseText;
    };

    // The VehicleUpdates waiting to go out, latest-wins PER VEHICLE (sync plan W6: a client controls every car
    // the server assigned it, not only the one it sits in). Put replaces the slot of u.vehId; Take hands over
    // everything pending, ascending by vehicle id, and empties the mailbox. Not thread-safe: join.cpp holds its
    // own lock around each call. Public so a test can drive it.
    class VehicleMailbox
    {
    public:
        struct Item
        {
            proto::VehicleUpdate u;
            int32_t sampleMs = -1;   // when the game thread read it (SendVehicleState's sampleMs); -1 = at the send
        };
        bool Put(const proto::VehicleUpdate& u, int32_t sampleMs);   // false: u.vehId >= proto::kMaxVehicles
        void Take(std::vector<Item>& out);
        void Clear() { items_.clear(); }
        size_t Pending() const { return items_.size(); }
    private:
        std::map<uint16_t, Item> items_;
    };

    // Event sink for the mp.* bridge (see gage_mp.cpp GageMP_Dispatch):
    //   "connected"    payload: the server name
    //   "disconnected" payload: why ("lost", "kicked", "shutdown", "local", ...)
    //   "playerJoined" payload: {"id":N,"name":"..."}   (names are a safe charset)
    //   "playerLeft"   payload: {"id":N}
    using EventSink = std::function<void(const char*, const char*)>;
    void SetEventSink(EventSink sink);

    // What the engine wants to know about the other players and the chat. Any
    // member may be empty. Set before Connect(); called on the worker thread.
    struct PeerSinks
    {
        // The server's Welcome: our id, its tick rate (Welcome::tickHz, 0 if it
        // sent none) and, v5, its maximum player health (Welcome::playerHealth). Called
        // before any other sink of that session, so a WorldState never arrives ahead of
        // the rate it is timed by.
        std::function<void(uint16_t myId, uint16_t tickHz, uint8_t playerHealth)> onWelcome;
        std::function<void(uint16_t id, const std::string& name)> onJoined;
        std::function<void(uint16_t id)> onLeft;
        // Every other player's position (the server leaves the receiver's own entry out).
        std::function<void(const proto::WorldState&)> onWorld;
        // Low-rate (~1 Hz) positions of the players beyond the WorldState's reach,
        // for map blips (protocol v2 WorldCoarse).
        std::function<void(const proto::CoarseState&)> onCoarse;
        // Another player's appearance (protocol v3 PeerLook, passes ValidLook): the newest
        // per id is its look until its PlayerLeft. Always after that id's onJoined.
        std::function<void(const proto::PeerLook&)> onLook;
        std::function<void(uint16_t fromId, const std::string& text)> onChat;
        // A session that had joined is over (lost, kicked, shut down, local):
        // every remote player is gone with it.
        std::function<void()> onEnded;

        // ---- protocol v4 (net::Event docs in libs/netclient/session.h) ----
        // The vehicles relevant to us: removals, the complete present set, the full states due.
        std::function<void(const proto::VehicleWorld&)> onVehicles;
        // The answer to one of our SendVehicleCmd calls (in the order they were sent).
        std::function<void(const proto::VehicleResult&)> onVehicleResult;
        // A health changed: ours (victimId == MyId()) or that of a player we hit.
        std::function<void(const proto::Damage&)> onDamage;
        // Somebody died (kill feed); ours when victimId == MyId().
        std::function<void(const proto::Killed&)> onKilled;
        // We are alive again (the answer to SendRespawn).
        std::function<void(const proto::RespawnOk&)> onRespawnOk;
        // Another player's emote: an animation, a scenario, or the end of either.
        std::function<void(const proto::PeerAnim&)> onPeerAnim;
        std::function<void(const proto::PeerScenario&)> onPeerScenario;
        std::function<void(const proto::PeerAnimStop&)> onPeerAnimStop;
        // T1 seats (wire v6): somebody's enter / exit intent on a vehicle seat (reliable, in order with the server's other traffic).
        std::function<void(const proto::SeatNotice&)> onSeatNotice;
        // T4: the server's resolution of a contact of a vehicle we control (a copy elsewhere, a real car here); once per contact, reliable.
        std::function<void(const proto::VehicleImpact&)> onVehicleImpact;
        // R3 T5: the round trip (ms) the server measures to each vehicle controller (about 1 Hz, unreliable).
        std::function<void(const proto::LinkInfo&)> onLinkInfo;
        // v8: the account the server verified for us (kAcct* flags, the username; flags 0 for a guest on a server that
        // requires none of the admin commands). Arrives once, at the end of the Welcome burst.
        std::function<void(const proto::AccountInfo&)> onAccount;
        // v8: the server's answer to one of our SendAdminCmd calls (same seq and op).
        std::function<void(const proto::AdminResult&)> onAdminResult;
        // v8 (0.6): our inventory (after AccountInfo, after each of our InvOps: ackSeq = its seq, and after a console give / clear),
        // and an item the server used for us (the satiety to add).
        std::function<void(const proto::InvState&)> onInvState;
        std::function<void(const proto::InvUsed&)> onInvUsed;
        // v8 (0.5.4 inventory world): our inventory with weapons (instead of onInvState once we sent an InvReport), and the
        // world containers in reach (the first one, an empty list, is the server's offer of the inventory world).
        std::function<void(const proto::InvSnap&)> onInvSnap;
        std::function<void(const proto::InvNearby&)> onInvNearby;
        // v8 (0.5.7 world clock): the server's clock and weather (on join, on every admin change, every 30 s); rttMs = the
        // session's smoothed round trip when it arrived (the receiver places the server's send moment half of it ago).
        std::function<void(const proto::WorldClock&, uint32_t rttMs)> onWorldClock;

        // Optional. A short text the worker appends to its 10 s heartbeat log line (the engine reports how long its
        // peer lock made the worker and the game thread wait there). Called on the worker thread once per beat;
        // it may start the counters it reports over.
        std::function<std::string()> beatNote;
    };
    void SetPeerSinks(PeerSinks sinks);

    // ---- protocol v4 sends. Each returns false when there is no live session or the message
    // would be refused (the checks the session itself makes); nothing blocks. ----
    // A vehicle we control (v5: the server assigns one controlling client per vehicle, `ctrl`): unreliable,
    // latest-wins PER VEHICLE (VehicleMailbox), sent in the same worker pass as the state. The server takes it
    // only from the vehicle's controller. `sampleMs` as in SendState (-1 = the moment of this call).
    bool SendVehicleState(const proto::VehicleUpdate& u, int32_t sampleMs = -1);
    // A vehicle request (reliable, in order; at most 32 waiting). Answered by onVehicleResult.
    bool SendVehicleCmd(const proto::VehicleCmd& c);
    // One hit our game registered (reliable, in order; at most 64 waiting). damage 0 is refused.
    bool SendHit(const proto::Hit& h);
    // Ask to come back after dying (reliable). Answered by onRespawnOk, or ignored before the server's
    // respawn delay: call it again about once a second.
    bool SendRespawn();
    // Our emote (reliable; anims, scenarios and stops keep their order; at most 16 waiting).
    bool SendAnim(const proto::Anim& a);
    bool SendScenario(const std::string& name);
    bool SendStopAnim();
    // v8: ask the server whether we may run an admin command (/heal, /fix, /spawn, /tp, /noclip, /gun, /car); reliable,
    // in order, at most 16 waiting. Answered by onAdminResult. False when not live or the command fails the wire checks.
    bool SendAdminCmd(const proto::AdminCmd& c);
    // v8 (0.6): one inventory operation (reliable, in order, at most 64 waiting). Answered by onInvState (ackSeq = o.seq).
    bool SendInvOp(const proto::InvOp& o);
    // v8 (0.5.4): what our ped holds (reliable; latest-wins: a newer call before the worker sends replaces the pending one).
    bool SendInvReport(const proto::InvReport& r);

    // Our own movement and on-foot state (protocol v3 PedState), unreliable and
    // latest-wins: a newer call before the worker sends replaces the pending one.
    // False when there is no live session or the values would be refused by the
    // server (non-finite, outside the world box, a PedState failing ValidPedState).
    // v5: `sampleMs` is the millisecond (any monotonic clock, only the low 16 bits travel) at which the game
    // thread read this state: the server places the sample on its clock by it. -1 (default) stamps the moment of
    // this call on the calling thread, which is the frame's own time for a caller that reads and posts in the
    // same breath; the worker never re-stamps it at send time. The keep-alive repeat reuses the last stamp.
    bool SendState(const proto::Kinematics& k, const proto::PedState& ped = proto::PedState(), int32_t sampleMs = -1);

    // Our appearance (protocol v3 PlayerLook), reliable, latest-wins like SendState.
    // The session skips a look equal to the last one it sent on this connection, so
    // calling this periodically with an unchanged look costs nothing on the wire.
    // False when there is no live session or the look fails ValidLook.
    bool SendLook(const proto::Look& look);

    // A chat line (reliable, in order). False when there is no live session or the
    // text would be refused (empty, over 200 bytes, control characters, bad UTF-8).
    bool SendChat(const std::string& text);

    // A player name the server will accept, made from `wanted`: characters outside
    // [A-Za-z0-9_.-] and inner spaces become '_', at most 24 of them, "player" if
    // nothing is left. Pure; Connect() applies it.
    std::string SanitizeName(const std::wstring& wanted);

    // `s` escaped for the inside of a JSON string literal (quote, backslash and
    // control characters; UTF-8 bytes pass through). For text a remote player or
    // the server chose, before it goes into a bridge payload.
    std::string JsonEscape(const std::string& s);

    // Start the session on the worker thread and wait up to `timeoutMs` for the
    // server's Welcome (or Refuse). Returns true on Welcome; the worker keeps the
    // session alive afterwards. On failure no thread is left behind and
    // `session.error` says why. A second Connect while one is live replaces the
    // old session. Blocks the caller: call it from a background thread.
    bool Connect(const Config& cfg, int timeoutMs, Session& session);

    // Tell the server goodbye (bounded to about two seconds), stop the worker.
    // Idempotent.
    void Disconnect();

    // True from Welcome until the session ends.
    bool IsConnected();

    // Our player id from the last Welcome (0 before the first).
    uint16_t MyId();

    // R3 T5: our round trip to the server in ms (the quietest of ENet's last interval, or its smoothed value when lower), 0 when not connected.
    uint32_t RttMs();

}
