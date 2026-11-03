// gage-bot: a headless GAGE client for exercising a server without the game. It
// joins, sends one PlayerLook (a freemode male in a plain outfit), walks a circle
// sending PlayerState at 20 Hz with a plausible on-foot state (move by speed, unarmed,
// a jump every 10 s), and logs what it hears (each peer's look once).
//
//   gage-bot <host> <port> <name> [--cx X] [--cy Y] [--cz Z] [--radius R]
//            [--speed MPS] [--seconds N] [--say TEXT] [--rate HZ]
//            [--vehicle MODEL] [--shoot ID] [--anim DICT:NAME]
//
// Defaults: a 10 m circle at 3 m/s around Legion Square (195, -934, 30), forever
// (until Ctrl+C or --seconds). --say sends one chat line a second after joining.
// --rate sets how often PlayerState goes out (default 20; 0 = never, like a game
// client still on its loading screen).
// v4: --vehicle MODEL (a GTA model hash, decimal or 0x...) spawns that vehicle a second after
// joining, takes the driver's seat and drives the circle with it (VehicleUpdate at the send rate);
// --shoot ID sends a 10-damage pistol Hit at player ID twice a second (each one preceded by a PlayerState that
// holds the pistol and bumps the shot counter, which is what the server's Hit binding looks for); --anim DICT:NAME plays that
// animation as a loop a second after joining. The bot logs VehicleResult, Damage, Killed, RespawnOk
// and peers' animations, and asks to respawn once a second while it is dead.
// Exit codes: 0 joined and left cleanly, 1 refused or never connected, 2 usage.
#include "netclient/session.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>

namespace {

    volatile std::sig_atomic_t g_stop = 0;

    void OnSignal(int)
    {
        g_stop = 1;
    }

    using Clock = std::chrono::steady_clock;
    const Clock::time_point g_start = Clock::now();

    double Elapsed()
    {
        return std::chrono::duration<double>(Clock::now() - g_start).count();
    }

    void Log(const std::string& msg)
    {
        std::printf("[+%7.3fs] %s\n", Elapsed(), msg.c_str());
        std::fflush(stdout);
    }

    int Usage(const char* why)
    {
        if (why) std::fprintf(stderr, "gage-bot: %s\n", why);
        std::fprintf(stderr,
                     "usage: gage-bot <host> <port> <name> [--cx X] [--cy Y] [--cz Z] [--radius R]\n"
                     "                [--speed MPS] [--seconds N] [--say TEXT] [--rate HZ]\n"
                     "                [--vehicle MODEL] [--shoot ID] [--anim DICT:NAME]\n");
        return 2;
    }

    bool ParseDouble(const char* s, double& out)
    {
        char* end = nullptr;
        out = std::strtod(s, &end);
        return end != s && *end == '\0' && std::isfinite(out);
    }

    const char* DisconnectName(gage::net::DisconnectReason r)
    {
        using R = gage::net::DisconnectReason;
        switch (r)
        {
        case R::Local:            return "local";
        case R::Lost:             return "lost";
        case R::Kicked:           return "kicked";
        case R::Refused:          return "refused";
        case R::Shutdown:         return "server shutdown";
        case R::ConnectFailed:    return "connect failed";
        case R::HandshakeTimeout: return "handshake timeout";
        }
        return "?";
    }

    constexpr uint32_t kFreemodeMale = 0x705E61F2u; // mp_m_freemode_01
    constexpr uint32_t kPistol       = 0x1B06D571u; // WEAPON_PISTOL
    constexpr double   kShootEvery   = 0.5;         // s between --shoot hits
    constexpr uint16_t kShootDamage  = 10;
    constexpr uint32_t kUnarmed      = 0xA2719263u; // WEAPON_UNARMED
    constexpr double   kJumpEvery    = 10.0;        // s between jump starts
    constexpr double   kJumpLasts    = 0.6;         // s the jumping flag stays set

    // A plain freemode male: jeans, trainers, T-shirt, no props, a mixed face. Every value is
    // well inside ValidLook's bounds and exists on the stock model.
    gage::proto::Look BotLook()
    {
        gage::proto::Look l;
        l.model = kFreemodeMale;
        l.components[2] = {4, 0, 0};   // hair
        l.components[3] = {0, 0, 0};   // arms / torso
        l.components[4] = {1, 0, 0};   // legs: jeans
        l.components[6] = {1, 0, 0};   // shoes: trainers
        l.components[8] = {15, 0, 0};  // undershirt: none
        l.components[11] = {0, 0, 0};  // top: T-shirt
        l.head = {4, 21, 0, 4, 21, 0, 128, 128, 0}; // father 4, mother 21, half and half
        return l;
    }

    // The bot's on-foot state at `t` seconds after joining, walking at `speed` m/s.
    gage::proto::PedState BotPed(double t, double speed)
    {
        gage::proto::PedState p;
        p.move = speed <= 0.05 ? gage::proto::kMoveIdle
                 : speed <= 3.0 ? gage::proto::kMoveWalk
                 : speed <= 6.0 ? gage::proto::kMoveRun
                                : gage::proto::kMoveSprint;
        p.weapon = kUnarmed;
        const long jumps = (long)(t / kJumpEvery); // jump starts so far (first at 10 s)
        p.jumpSeq = (uint8_t)(jumps & 3);          // only the low 2 bits travel
        if (jumps > 0 && t - (double)jumps * kJumpEvery < kJumpLasts) p.flags |= gage::proto::kPfJumping;
        return p;
    }

}

int main(int argc, char** argv)
{
    using namespace gage;

    if (argc < 4) return Usage(nullptr);
    net::SessionOptions opts;
    opts.host = argv[1];
    char* end = nullptr;
    const long port = std::strtol(argv[2], &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) return Usage("port must be a whole number 1..65535");
    opts.port = (uint16_t)port;
    opts.name = argv[3];
    if (!proto::ValidName(opts.name))
        return Usage("name must be 1..24 characters of A-Z a-z 0-9 _ . - and single inner spaces");

    double cx = 195, cy = -934, cz = 30, radius = 10, speed = 3, seconds = 0, rate = 20;
    std::string say;
    uint32_t vehicleModel = 0;   // --vehicle: 0 = on foot
    long shootId = -1;           // --shoot: -1 = never
    proto::Anim anim;            // --anim: empty dict = none
    for (int i = 4; i < argc; i++)
    {
        const char* a = argv[i];
        if (i + 1 >= argc) return Usage((std::string("missing value for ") + a).c_str());
        const char* v = argv[++i];
        double* target = nullptr;
        if (!std::strcmp(a, "--cx")) target = &cx;
        else if (!std::strcmp(a, "--cy")) target = &cy;
        else if (!std::strcmp(a, "--cz")) target = &cz;
        else if (!std::strcmp(a, "--radius")) target = &radius;
        else if (!std::strcmp(a, "--speed")) target = &speed;
        else if (!std::strcmp(a, "--seconds")) target = &seconds;
        else if (!std::strcmp(a, "--rate")) target = &rate;
        else if (!std::strcmp(a, "--say")) { say = v; continue; }
        else if (!std::strcmp(a, "--vehicle"))
        {
            char* e2 = nullptr;
            const unsigned long m = std::strtoul(v, &e2, 0);
            if (e2 == v || *e2 != '\0' || m == 0 || m > 0xFFFFFFFFul) return Usage("--vehicle needs a non-zero model hash");
            vehicleModel = (uint32_t)m;
            continue;
        }
        else if (!std::strcmp(a, "--shoot"))
        {
            char* e2 = nullptr;
            shootId = std::strtol(v, &e2, 10);
            if (e2 == v || *e2 != '\0' || shootId < 0 || shootId >= proto::kMaxPlayers) return Usage("--shoot needs a player id");
            continue;
        }
        else if (!std::strcmp(a, "--anim"))
        {
            const char* colon = std::strchr(v, ':');
            if (!colon) return Usage("--anim needs DICT:NAME");
            anim.dict.assign(v, colon);
            anim.name = colon + 1;
            anim.flags = 1; // loop
            if (!proto::ValidAnim(anim)) return Usage("--anim: names are 1..63 characters of A-Z a-z 0-9 _ @ / . -");
            continue;
        }
        else return Usage((std::string("unknown argument: ") + a).c_str());
        if (!ParseDouble(v, *target)) return Usage((std::string("bad number for ") + a).c_str());
    }
    if (radius <= 0) return Usage("--radius must be > 0");
    if (speed < 0) return Usage("--speed must be >= 0");
    if (rate < 0) return Usage("--rate must be >= 0");
    if (seconds < 0) return Usage("--seconds must be >= 0");
    if (!say.empty() && !proto::ValidChat(say)) return Usage("--say text is not a valid chat message");

    // The whole circle must be inside the world box, or the server drops every state.
    for (int s = 0; s < 4; s++)
    {
        proto::Kinematics probe;
        probe.x = (float)(cx + (s == 0 ? radius : s == 2 ? -radius : 0));
        probe.y = (float)(cy + (s == 1 ? radius : s == 3 ? -radius : 0));
        probe.z = (float)cz;
        probe.vx = probe.vy = (float)speed;
        if (!proto::ValidKinematics(probe)) return Usage("the circle leaves the world box or --speed is too high");
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    net::ClientSession session;
    std::string err;
    if (!session.Connect(opts, &err))
    {
        Log("cannot connect: " + err);
        return 1;
    }
    Log("connecting to " + opts.host + ":" + std::to_string(opts.port) + " as " + opts.name);

    constexpr double kPi = 3.14159265358979323846;
    const double sendEvery = rate > 0 ? 1.0 / rate : 0;
    double angle = 0, lastSend = 0, lastFrame = Elapsed(), joinedAt = 0, lastWorldLog = 0;
    bool saidIt = say.empty(), wasJoined = false, refused = false, quitting = false;
    std::set<uint16_t> lookLogged; // peers whose look was logged (cleared on PlayerLeft: ids are reused)
    // v4
    uint16_t myId = proto::kNoPlayer;
    bool vehicleAsked = false, driving = false, dead = false, animSent = anim.dict.empty();
    uint16_t myVehicle = 0;
    double lastShot = 0, lastRespawnAsk = 0, lastVehicleLog = 0;
    uint8_t shotSeq = 0;

    while (session.state() != net::SessionState::Closed)
    {
        session.Poll(5);

        net::Event e;
        while (session.NextEvent(e))
        {
            switch (e.type)
            {
            case net::Event::Type::Welcome:
                myId = e.welcome.yourId;
                Log("welcome: id " + std::to_string(e.welcome.yourId) + ", server \"" + e.welcome.serverName +
                    "\", " + std::to_string(e.welcome.tickHz) + " Hz");
                break;
            case net::Event::Type::Refused:
                refused = true;
                Log(std::string("refused: ") + proto::ToString(e.refuse.reason) + ": " + e.refuse.text);
                break;
            case net::Event::Type::PlayerJoined:
                Log("joined: " + std::to_string(e.joined.id) + " " + e.joined.name);
                break;
            case net::Event::Type::PlayerLeft:
                Log("left: " + std::to_string(e.left.id) + " (" + proto::ToString(e.left.reason) + ")");
                lookLogged.erase(e.left.id);
                break;
            case net::Event::Type::PeerLook:
                // Once per peer: a look that keeps changing would otherwise flood the log.
                if (lookLogged.insert(e.look.id).second)
                {
                    char model[16];
                    std::snprintf(model, sizeof model, "0x%08X", (unsigned)e.look.look.model);
                    Log("look: " + std::to_string(e.look.id) + " model " + model);
                }
                break;
            case net::Event::Type::Chat:
                Log(e.chat.fromId == proto::kServerId
                        ? "chat <server>: " + e.chat.text
                        : "chat <" + std::to_string(e.chat.fromId) + ">: " + e.chat.text);
                break;
            case net::Event::Type::World:
                // One line a second, not 20.
                if (Elapsed() - lastWorldLog >= 1.0)
                {
                    lastWorldLog = Elapsed();
                    Log("world tick " + std::to_string(e.world.tick) + ": " + std::to_string(e.world.players.size()) +
                        " other player(s), rtt " + std::to_string(session.rttMs()) + " ms");
                }
                break;
            case net::Event::Type::Coarse:
                break; // far-player blips: the bot does not use them
            case net::Event::Type::Disconnected:
                Log(std::string("disconnected: ") + DisconnectName(e.disconnect));
                break;
            // ---- v4 ----
            case net::Event::Type::VehicleWorld:
                if (Elapsed() - lastVehicleLog >= 5.0 && !e.vehicles.present.empty())
                {
                    lastVehicleLog = Elapsed();
                    Log("vehicles: " + std::to_string(e.vehicles.present.size()) + " near, " + std::to_string(e.vehicles.vehicles.size()) +
                        " full state(s) this frame");
                }
                break;
            case net::Event::Type::VehicleResult:
                Log(std::string("vehicle ") + proto::ToString(e.vehicleResult.op) + " " + std::to_string(e.vehicleResult.vehId) + ": " +
                    proto::ToString(e.vehicleResult.code));
                if (e.vehicleResult.code == proto::VehicleResultCode::Ok)
                {
                    if (e.vehicleResult.op == proto::VehicleOp::Spawn)
                    {
                        myVehicle = e.vehicleResult.vehId;
                        proto::VehicleCmd enter;
                        enter.op = proto::VehicleOp::Enter;
                        enter.vehId = myVehicle;
                        enter.arg = 0; // the driver's seat
                        session.SendVehicleCmd(enter);
                    }
                    else if (e.vehicleResult.op == proto::VehicleOp::Enter && e.vehicleResult.vehId == myVehicle)
                        driving = true;
                }
                break;
            case net::Event::Type::Damage:
                Log("damage: " + std::to_string(e.damage.victimId) + " hit by " +
                    (e.damage.attackerId == proto::kServerId ? std::string("server") : std::to_string(e.damage.attackerId)) + " for " +
                    std::to_string(e.damage.damage) + " -> health " + std::to_string(e.damage.health) + " armour " +
                    std::to_string(e.damage.armor) + (e.damage.killed ? " KILLED" : ""));
                if (e.damage.victimId == myId && e.damage.killed)
                {
                    dead = true;
                    driving = false; // the server put us out of the vehicle
                }
                break;
            case net::Event::Type::Killed:
                Log("killed: " + std::to_string(e.killed.victimId) + " by " +
                    (e.killed.killerId == proto::kServerId ? std::string("server") : std::to_string(e.killed.killerId)));
                if (e.killed.victimId == myId)
                {
                    dead = true;
                    driving = false;
                }
                break;
            case net::Event::Type::RespawnOk:
                Log("respawn: at " + std::to_string(e.respawn.x) + " " + std::to_string(e.respawn.y) + " " + std::to_string(e.respawn.z) +
                    " with health " + std::to_string(e.respawn.health));
                dead = false;
                animSent = anim.dict.empty(); // the server cleared it: play it again
                vehicleAsked = false;         // and fetch a new car
                break;
            case net::Event::Type::PeerAnim:
                Log("anim: " + std::to_string(e.anim.id) + " " + e.anim.anim.dict + " / " + e.anim.anim.name +
                    (e.anim.anim.durationMs == -1 ? " (loop)" : ""));
                break;
            case net::Event::Type::PeerScenario:
                Log("scenario: " + std::to_string(e.scenario.id) + " " + e.scenario.name);
                break;
            case net::Event::Type::PeerAnimStop:
                Log("anim stop: " + std::to_string(e.animStop.id));
                break;
            case net::Event::Type::SeatNotice:
                Log(std::string("seat notice: ") + proto::ToString(e.seatNotice.kind) + " player " + std::to_string(e.seatNotice.player) + " vehicle " +
                    std::to_string(e.seatNotice.vehId) + " seat " + std::to_string((int)e.seatNotice.seat));
                break;
            }
        }

        const double now = Elapsed();
        if (session.state() == net::SessionState::Joined)
        {
            if (!wasJoined)
            {
                wasJoined = true;
                joinedAt = now;
                if (!session.SendLook(BotLook())) Log("could not send look");
            }
            const double dt = now - lastFrame;
            angle += (speed / radius) * dt;
            // --shoot: a Hit has to name a shot the server SAW (docs/api/server.md "Anti-cheat rules"): the shot counter
            // is bumped in a PlayerState carrying the pistol, and that state goes out before the Hit that names it.
            const bool shootNow = rate > 0 && shootId >= 0 && !dead && now - joinedAt >= 1.0 && now - lastShot >= kShootEvery;
            if (shootNow)
            {
                lastShot = now;
                shotSeq = (uint8_t)((shotSeq + 1) & 15);
            }
            if (rate > 0 && (shootNow || now - lastSend >= sendEvery))
            {
                lastSend = now;
                proto::Kinematics k;
                k.x = (float)(cx + radius * std::cos(angle));
                k.y = (float)(cy + radius * std::sin(angle));
                k.z = (float)cz;
                k.vx = (float)(-speed * std::sin(angle));
                k.vy = (float)(speed * std::cos(angle));
                // GTA heading: 0 faces +Y and grows counter-clockwise (towards -X).
                double h = std::atan2(-(double)k.vx, (double)k.vy) * 180.0 / kPi;
                if (h < 0) h += 360.0;
                k.heading = (float)h;
                proto::PedState ped = BotPed(now - joinedAt, speed);
                ped.shotSeq = shotSeq;
                if (shootId >= 0) ped.weapon = kPistol; // the weapon the Hits claim is the one the states report
                if (driving)
                {
                    ped.move = proto::kMoveInVehicle;
                    ped.flags = proto::kPfDriver;
                    proto::VehicleUpdate u;
                    u.vehId = myVehicle;
                    u.k = k;
                    u.rot = proto::QuatFromHeading(k.heading);
                    u.avz = (float)(speed / radius); // turning left around the circle, rad/s
                    u.steer = 0.2f;
                    u.flags = proto::kVehEngine;
                    session.SendVehicleState(u);
                }
                if (dead) ped.flags |= proto::kPfDead;
                session.SendState(k, ped);
                if (shootNow)
                {
                    proto::Hit hit;
                    hit.kind = proto::HitKind::Player;
                    hit.targetId = (uint16_t)shootId;
                    hit.weapon = kPistol;
                    hit.damage = kShootDamage;
                    hit.shotSeq = shotSeq;
                    session.SendHit(hit);
                }
            }
            // v4 extras, each a second after joining (the server needs a position first).
            if (now - joinedAt >= 1.0)
            {
                if (vehicleModel && !vehicleAsked && !dead)
                {
                    vehicleAsked = true;
                    proto::VehicleCmd spawn;
                    spawn.op = proto::VehicleOp::Spawn;
                    spawn.arg = vehicleModel;
                    session.SendVehicleCmd(spawn);
                }
                if (!animSent && !dead)
                {
                    animSent = true;
                    if (!session.SendAnim(anim)) Log("could not send anim");
                }
                if (dead && now - lastRespawnAsk >= 1.0)
                {
                    lastRespawnAsk = now;
                    session.SendRespawn(); // ignored until the server's respawn delay is over
                }
            }
            if (!saidIt && now - joinedAt >= 1.0)
            {
                saidIt = true;
                if (!session.SendChat(say)) Log("could not send chat");
            }
            if (!quitting && (g_stop || (seconds > 0 && now - joinedAt >= seconds)))
            {
                quitting = true;
                Log("leaving");
                session.Disconnect();
            }
        }
        else if (g_stop && !quitting)
        {
            quitting = true;
            session.Disconnect();
        }
        lastFrame = now;
    }

    return wasJoined && !refused ? 0 : 1;
}
