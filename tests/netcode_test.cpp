// netcode_test — GAGE protocol v4 and the server, over real ENet on 127.0.0.1.
// Build target: netcode_test (selftest/, never shipped).
//
//   A. Codec: round trips, the exact wire layout (written out byte by byte), wrong type /
//      truncation at every length / trailing byte / non-finite float / over-cap string /
//      unknown enum / hostile WorldState count, v1 packets refused, and the Valid* helpers.
//   B. Server config: Validate and the server.json loader, replication keys included.
//   C. Network: an in-process gage::server::Server on an ephemeral loopback port and
//      real ClientSessions, all stepped from this one thread. Every wait is a
//      bounded pump on a condition, never a bare sleep. Covers the handshake,
//      version / name / full / hello-timeout refusals, protocol violations,
//      join/leave (Quit, Timeout, Kicked), state sync (receiver exclusion, stale
//      seq, invalid and malformed state, wrap-around, loss), interest management over
//      the wire (relevance, the coarse channel, always-relevant), delta frames under loss,
//      chat (exactly once, in order, rejection), the console, server shutdown and a dead server,
//      and v3: the on-foot state end to end (incl. flags-only changes of an idle player and
//      the refusals), PlayerLook / PeerLook (broadcast, dedupe, late-joiner replay, refusals)
//      and the look rate limit; v4: the vehicle / combat / emote codecs byte for byte, vehicle
//      occupancy, driver-only updates, caps, despawn, Register (adopt), server-owned health (Hit
//      validation, armour then health, the clamp, Killed, respawn timing, vehicle hits, the console)
//      and animation / scenario relay, replay, refusals and rate.
//   The replication logic itself (hysteresis, budgets, rates, pop-in) is in interest_test,
//   the quantisation bounds in quant_test.
//
// Everything is deterministic except wall-clock pacing. The whole run stays well
// under 30 s; it exits non-zero on any failed check or a wrong check count.
#include "netrig.h"
#include "melee_sim.h"        // 0.4.5 melee audit: the engine's melee pipeline as a scripted client ...
#include "sync/combat.cpp"     // ... built from the real libs/sync code (netcode_test links no sync library, so the TU takes it)

#include "metrics.h"
#include "protocol/crc32.h"
#include "sync/outfit.h"      // 0.5.4 outfit_items: the worn items -> look table (header only)

#include <cstdarg>
#include <cstdlib>
#include <memory>


// ===========================================================================
// A. Codec
// ===========================================================================

template <class T>
static bool DecodeAs(const uint8_t* p, size_t n)
{
    T t;
    return proto::Decode(p, n, t);
}

struct Sample
{
    const char* name;
    proto::Bytes bytes;
    bool (*dec)(const uint8_t*, size_t);
};

static std::vector<Sample> Samples()
{
    std::vector<Sample> s;
    proto::Hello hello;
    hello.name = "Alice";
    s.push_back({"Hello", proto::Encode(hello), &DecodeAs<proto::Hello>});

    proto::Welcome welcome;
    welcome.yourId = 7;
    welcome.tickHz = 20;
    welcome.serverName = "GAGE Test";
    welcome.playerHealth = 200; // v5: the effective maximum health the server runs
    s.push_back({"Welcome", proto::Encode(welcome), &DecodeAs<proto::Welcome>});

    proto::Refuse refuse;
    refuse.reason = proto::RefuseReason::NameTaken;
    refuse.text = "name is already in use";
    s.push_back({"Refuse", proto::Encode(refuse), &DecodeAs<proto::Refuse>});

    proto::PlayerJoined joined;
    joined.id = 3;
    joined.name = "Bob";
    s.push_back({"PlayerJoined", proto::Encode(joined), &DecodeAs<proto::PlayerJoined>});

    proto::PlayerLeft left;
    left.id = 3;
    left.reason = proto::LeaveReason::Timeout;
    s.push_back({"PlayerLeft", proto::Encode(left), &DecodeAs<proto::PlayerLeft>});

    proto::PlayerState state;
    state.seq = 42;
    state.sampleMs = 0xBEEF; // v5: the sender's clock when the sample was read
    state.k = Kin(195.5f, -934.25f, 30.0f, 90.0f, 1.5f, -2.5f, 0.25f);
    state.ped.move = proto::kMoveRun; // v3: the sample carries an on-foot state (aiming, so the aim travels)
    state.ped.jumpSeq = 2;
    state.ped.flags = proto::kPfAiming | proto::kPfJumping;
    state.ped.weapon = 0x1B06D571u;
    state.ped.aimX = 12.5f;
    state.ped.aimY = -3.0f;
    state.ped.aimZ = 0.5f;
    s.push_back({"PlayerState", proto::Encode(state), &DecodeAs<proto::PlayerState>});

    proto::WorldState world;
    world.tick = 9;
    for (uint16_t i = 0; i < 3; i++)
    {
        proto::WorldEntry e;
        e.id = i;
        e.k = Kin(10.0f * i, -5.0f * i, 30.0f + i, 45.0f * i, 0.1f * i, 0.2f * i, 0.3f * i);
        e.ped.move = (uint8_t)i;
        e.ped.weapon = 1000u * i;
        if (i == 2)
        {
            e.ped.flags = proto::kPfAiming;
            e.ped.aimX = -100.0f;
        }
        world.players.push_back(e);
    }
    s.push_back({"WorldState", proto::Encode(world), &DecodeAs<proto::WorldState>});

    proto::ChatSend chat;
    chat.text = Raw({'h', 0xC3, 0xA9, 'l', 'l', 'o'}); // "hello" with an e-acute
    s.push_back({"ChatSend", proto::Encode(chat), &DecodeAs<proto::ChatSend>});

    proto::ChatMessage msg;
    msg.fromId = 5;
    msg.text = "hi";
    s.push_back({"ChatMessage", proto::Encode(msg), &DecodeAs<proto::ChatMessage>});

    proto::CoarseState coarse;
    coarse.tick = 9;
    for (uint16_t id : {1, 4, 300})
    {
        proto::CoarseEntry e;
        e.id = id;
        e.x = 100.0f + (float)id;
        e.y = -200.5f;
        e.z = 30.0f;
        coarse.players.push_back(e);
    }
    s.push_back({"WorldCoarse", proto::Encode(coarse), &DecodeAs<proto::CoarseState>});
    return s;
}

// A v4 WorldState written by hand, so the announced entry count can lie about the body:
// no leave ids, `entriesWritten` absolute entries of the all-zero state (16 bytes of kinematics,
// 8 of ped, 2 of vitals: health 100, armour 0) with consecutive ids.
static proto::Bytes RawWorld(uint32_t claimedCount, uint32_t entriesWritten)
{
    wire::Writer w;
    w.u8((uint8_t)proto::Msg::WorldState);
    w.u32(1);
    w.u8(0);
    w.varu32(claimedCount);
    for (uint32_t i = 0; i < entriesWritten; i++)
    {
        w.varu32(0); // id gap
        w.u8(0);     // no baseline
        w.u8(0);     // sample age
        w.u64(0);
        w.u16(0);
        w.i16(0);
        w.i16(0);
        w.i16(0);
        w.u8(0);  // ped: move byte
        w.u16(0); // flags
        w.u32(0); // weapon
        w.u8(0);  // v4: shot byte
        w.u8(100); // v4: health
        w.u8(0);   // v4: armour
    }
    return w.take();
}

// A v4 ped block written by hand (move byte, flags, weapon, the shot byte, then the given i16 aim
// values) so tests can put on the wire what the client library would never encode.
static proto::Bytes RawPed(uint8_t moveByte, uint16_t flags, uint32_t weapon, std::initializer_list<int16_t> aim = {}, uint8_t shot = 0)
{
    wire::Writer w;
    w.u8(moveByte);
    w.u16(flags);
    w.u32(weapon);
    w.u8(shot);
    for (int16_t a : aim) w.i16(a);
    return w.take();
}

// A PlayerState written by hand (no ack): the three position fields in centimetres, to put
// values on the wire that the client library would never encode. `ped` is the v3 ped block as
// written (default: idle, no flags, weapon 0).
static proto::Bytes RawState(uint32_t seq, int32_t xcm, int32_t ycm, int32_t zcm, const proto::Bytes& ped = RawPed(0, 0, 0))
{
    wire::Writer w;
    w.u8((uint8_t)proto::Msg::PlayerState);
    w.u32(seq);
    w.u32(0);
    w.u32(0);
    w.u16(0); // v5: sampleMs
    w.u64((uint64_t)((uint32_t)xcm & 0x3FFFFFu) | ((uint64_t)((uint32_t)ycm & 0x3FFFFFu) << 22) |
          ((uint64_t)((uint32_t)zcm & 0xFFFFFu) << 44));
    w.u16(0);
    w.i16(0);
    w.i16(0);
    w.i16(0);
    w.bytes(ped.data(), ped.size());
    return w.take();
}

static void CodecTests()
{
    const std::vector<Sample> samples = Samples();
    Check("codec: ten message samples", samples.size() == 10, std::to_string(samples.size()));

    // Round trip: decode then re-encode gives the same bytes, for all ten.
    {
        proto::Hello a;        proto::Welcome b;     proto::Refuse c;
        proto::PlayerJoined d; proto::PlayerLeft e;  proto::PlayerState f;
        proto::WorldState g;   proto::ChatSend h;    proto::ChatMessage i;
        proto::CoarseState j;
        auto rt = [&](int k, auto& m) {
            return proto::Decode(samples[k].bytes.data(), samples[k].bytes.size(), m) &&
                   proto::Encode(m) == samples[k].bytes;
        };
        const bool ok[10] = {rt(0, a), rt(1, b), rt(2, c), rt(3, d), rt(4, e), rt(5, f), rt(6, g), rt(7, h), rt(8, i), rt(9, j)};
        std::string bad;
        for (int k = 0; k < 10; k++)
            if (!ok[k]) bad += std::string(samples[k].name) + " ";
        Check("codec: every message round-trips byte for byte", bad.empty(), bad);
        Check("codec: decoded fields match what was encoded",
              a.name == "Alice" && a.version == proto::kVersion && b.yourId == 7 && b.tickHz == 20 &&
                  b.serverName == "GAGE Test" && b.playerHealth == 200 && f.sampleMs == 0xBEEF && c.reason == proto::RefuseReason::NameTaken && d.id == 3 &&
                  d.name == "Bob" && e.id == 3 && e.reason == proto::LeaveReason::Timeout && f.seq == 42 &&
                  f.k.x == 195.5f && f.k.y == -934.25f && f.k.heading == 90.0f && f.k.vy == -2.5f && g.tick == 9 &&
                  f.ped.move == proto::kMoveRun && f.ped.jumpSeq == 2 && f.ped.flags == (proto::kPfAiming | proto::kPfJumping) &&
                  f.ped.weapon == 0x1B06D571u && f.ped.aimX == 12.5f && f.ped.aimY == -3.0f && f.ped.aimZ == 0.5f &&
                  g.players.size() == 3 && g.players[1].ped.move == 1 && g.players[1].ped.weapon == 1000u && g.players[1].ped.aimX == 0.0f &&
                  g.players[2].ped.flags == proto::kPfAiming && g.players[2].ped.aimX == -100.0f &&
                  g.players.size() == 3 && g.players[2].id == 2 && g.players[2].k.z == 32.0f && h.text.size() == 6 &&
                  i.fromId == 5 && i.text == "hi" && j.tick == 9 && j.players.size() == 3 && j.players[2].id == 300 &&
                  std::fabs(j.players[2].x - 400.0f) <= 0.25f && std::fabs(j.players[0].y + 200.5f) <= 0.25f &&
                  std::fabs(j.players[1].z - 30.0f) <= 0.04f);
    }

    // The wire layout, written out byte by byte: a codec that is symmetrically wrong would
    // still round-trip. Little-endian everywhere. The test state is x = 1.00 m, y = -1.00 m,
    // z = 2.50 m, heading 90 deg, v = (1, -1, 0.5) m/s, i.e. x = 100 cm, y = -100 cm, z = 250 cm
    // packed as x | y << 22 | z << 44 into a u64 (0x000FAFFFE7000064), heading 0x4000, velocity
    // 32, -32, 16 in 1/32 m/s.
    {
        const proto::Kinematics k = Kin(1.0f, -1.0f, 2.5f, 90.0f, 1.0f, -1.0f, 0.5f);
        const std::vector<uint8_t> state16 = {0x64, 0x00, 0x00, 0xE7, 0xFF, 0xAF, 0x0F, 0x00, // position
                                              0x00, 0x40,                                     // heading
                                              0x20, 0x00, 0xE0, 0xFF, 0x10, 0x00};            // velocity
        proto::Hello h;
        h.name = "Al";
        // v8 (deliberate): the frozen Hello layout now carries version 8 (the join token rides in JoinAuth).
        const proto::Bytes wantHello = {1, 8, 0, 2, 0, 'A', 'l'}; // type, u16 version 8, u16 length, bytes
        Check("codec: Hello is type, u16 version (8), u16-length string, little-endian",
              proto::kVersion == 8 && proto::Encode(h) == wantHello, Hex(proto::Encode(h)));

        // v3 (deliberate): PlayerState is the v2 29 bytes plus the ped block; v4 (deliberate): the ped block
        // gains the shot byte (8 bytes): 37, 43 while aiming; v5 (deliberate): + u16 sampleMs after the ack: 39, 45.
        // Idle ped: move byte 00, flags 00 00, weapon 00 00 00 00, shot 00.
        proto::PlayerState ps;
        ps.seq = 0x01020304;
        ps.ack.tick = 0x0A0B0C0D;
        ps.ack.bits = 0x80000001;
        ps.sampleMs = 0x1234;
        ps.k = k;
        proto::Bytes wantPs = {6, 0x04, 0x03, 0x02, 0x01, 0x0D, 0x0C, 0x0B, 0x0A, 0x01, 0x00, 0x00, 0x80, 0x34, 0x12};
        wantPs.insert(wantPs.end(), state16.begin(), state16.end());
        const proto::Bytes idlePed = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        proto::Bytes wantPsIdle = wantPs;
        wantPsIdle.insert(wantPsIdle.end(), idlePed.begin(), idlePed.end());
        // Running (2), jumpSeq 1 -> move byte 2 | 1 << 3 = 0x0A; flags aiming | jumping = 0x0005; weapon 0xA2719263;
        // shotSeq 11 -> 0B; aim (1.0, -0.5, 2.0) m -> (10, -5, 20) dm -> 0A 00, FB FF, 14 00.
        proto::PlayerState psAim = ps;
        psAim.ped.move = proto::kMoveRun;
        psAim.ped.jumpSeq = 1;
        psAim.ped.flags = proto::kPfAiming | proto::kPfJumping;
        psAim.ped.weapon = 0xA2719263u;
        psAim.ped.shotSeq = 11;
        psAim.ped.aimX = 1.0f;
        psAim.ped.aimY = -0.5f;
        psAim.ped.aimZ = 2.0f;
        proto::Bytes wantPsAim = wantPs;
        wantPsAim.insert(wantPsAim.end(), {0x0A, 0x05, 0x00, 0x63, 0x92, 0x71, 0xA2, 0x0B, 0x0A, 0x00, 0xFB, 0xFF, 0x14, 0x00});
        // The same aim WITHOUT the aiming flag is not sent at all.
        proto::PlayerState psNoFlag = psAim;
        psNoFlag.ped.flags = proto::kPfJumping;
        proto::Bytes wantPsNoFlag = wantPs;
        wantPsNoFlag.insert(wantPsNoFlag.end(), {0x0A, 0x01, 0x00, 0x63, 0x92, 0x71, 0xA2, 0x0B});
        Check("codec: PlayerState is 39 bytes (type, u32 seq, u32 ack tick, u32 ack bits, u16 sampleMs, 16-byte state, 8-byte ped: move|jumpSeq, u16 flags, u32 weapon, shotSeq), 45 with the aim (3 x i16 dm) only while aiming (v4: 37 / 43)",
              wantPsIdle.size() == 39 && proto::Encode(ps) == wantPsIdle && wantPsAim.size() == 45 && proto::Encode(psAim) == wantPsAim &&
                  wantPsNoFlag.size() == 39 && proto::Encode(psNoFlag) == wantPsNoFlag,
              Hex(proto::Encode(ps)) + "| " + Hex(proto::Encode(psAim)) + "| " + Hex(proto::Encode(psNoFlag)));

        proto::WorldState ws;
        ws.tick = 0x01020304;
        ws.left = {7};
        proto::WorldEntry we;
        we.id = 5;
        we.ageMs = 12;
        we.k = k;
        ws.players.push_back(we);
        proto::Bytes wantWs = {7, 0x04, 0x03, 0x02, 0x01, 1, 7, 0, 1, 5, 0, 12}; // type, tick, 1 leave id 7, 1 entry: gap 5, flags 0, age 12
        wantWs.insert(wantWs.end(), state16.begin(), state16.end());
        wantWs.insert(wantWs.end(), idlePed.begin(), idlePed.end());
        wantWs.insert(wantWs.end(), {100, 0}); // v4 vitals: health 100 (the default), armour 0
        // v3 (deliberate): an absolute entry is the v2 one plus the ped block; v4 (deliberate): 8-byte ped
        // block plus 2 vitals bytes (38-byte packet; v3: 35, v2: 28).
        Check("codec: an absolute WorldState entry is id gap, flags, age, the same 16-byte state, the 8-byte ped block and health, armour",
              wantWs.size() == 38 && proto::Encode(ws) == wantWs, Hex(proto::Encode(ws)));

        proto::CoarseState cs;
        cs.tick = 7;
        proto::CoarseEntry ce;
        ce.id = 3;
        ce.x = 100.0f;
        ce.y = 0.0f;
        ce.z = 30.0f;
        cs.players.push_back(ce);
        // x = (100 + 16384) / 0.5 = 32968 = 0x80C8, y = 32768 = 0x8000, z = (30 + 2048) / 0.078125 = 26598 = 0x67E6
        const proto::Bytes wantCs = {10, 7, 0, 0, 0, 1, 3, 0xC8, 0x80, 0x00, 0x80, 0xE6, 0x67};
        Check("codec: a WorldCoarse entry is id gap and three u16 (0.5 m, 0.5 m, 5/64 m steps from the box corner)",
              proto::Encode(cs) == wantCs, Hex(proto::Encode(cs)));

        // Hello and Refuse kept their v1 layout, so an old client can still read why it is refused.
        proto::Refuse rf;
        rf.reason = proto::RefuseReason::VersionMismatch;
        rf.text = "x";
        proto::Hello oldHello;
        oldHello.version = 1;
        oldHello.name = "Al";
        Check("codec: Hello and Refuse keep their v1 byte layout (a v1 client is told why it is refused)",
              proto::Encode(oldHello) == proto::Bytes({1, 1, 0, 2, 0, 'A', 'l'}) &&
                  proto::Encode(rf) == proto::Bytes({3, 1, 1, 0, 'x'}),
              Hex(proto::Encode(rf)));

        // v1 packets are not v2 packets: a 33-byte v1 PlayerState and a v1 WorldState are malformed.
        proto::Bytes v1State = {6, 1, 0, 0, 0};
        for (int f = 0; f < 7; f++) v1State.insert(v1State.end(), {0, 0, 0, 0});
        proto::Bytes v1World = {7, 1, 0, 0, 0, 1, 0, 0, 0}; // type, u32 tick, u16 count 1, u16 id 0 ...
        v1World.insert(v1World.end(), 28, 0);                  // ... seven f32
        Check("codec: a v1 PlayerState (33 bytes of floats) and a v1 WorldState are rejected",
              !DecodeAs<proto::PlayerState>(v1State.data(), v1State.size()) &&
                  !DecodeAs<proto::WorldState>(v1World.data(), v1World.size()));
    }

    // Wrong type byte: every sample against every other decoder.
    {
        int cases = 0, accepted = 0;
        for (size_t i = 0; i < samples.size(); i++)
            for (size_t j = 0; j < samples.size(); j++)
            {
                if (i == j) continue;
                cases++;
                if (samples[j].dec(samples[i].bytes.data(), samples[i].bytes.size())) accepted++;
            }
        Check("codec: a wrong type byte is rejected by every decoder (90 cases)", cases == 90 && accepted == 0,
              std::to_string(cases) + " cases, " + std::to_string(accepted) + " accepted");
    }

    // Truncation at every length, trailing byte, for every message.
    {
        int cases = 0, accepted = 0, trailingCases = 0, trailingAccepted = 0;
        for (const Sample& s : samples)
        {
            for (size_t len = 0; len < s.bytes.size(); len++)
            {
                cases++;
                if (s.dec(s.bytes.data(), len)) accepted++;
            }
            proto::Bytes longer = s.bytes;
            longer.push_back(0);
            trailingCases++;
            if (s.dec(longer.data(), longer.size())) trailingAccepted++;
        }
        Check("codec: truncation is rejected at every length", cases > 150 && accepted == 0,
              std::to_string(cases) + " cases, " + std::to_string(accepted) + " accepted");
        Check("codec: one trailing byte is rejected for every message", trailingCases == 10 && trailingAccepted == 0,
              std::to_string(trailingAccepted) + " accepted");
    }

    // PeekType.
    {
        proto::Msg m = proto::Msg::Hello;
        // v3: 16 PlayerLook and 17 PeerLook; v4: 18 VehicleUpdate .. 30 PeerAnimStop; T1 (v6): 31 SeatNotice; T4 (v6): 40 VehicleImpact; R3 T5: 44 LinkInfo;
        // v8: 45 JoinAuth .. 48 AdminResult, 0.6 inventory 49 InvState .. 51 InvUsed (32..39 and 41..43 are reserved for other
        // messages; PeekType only range-checks them), 0.5.4 inventory world 52 InvSnap .. 54 InvReport, 0.5.7 55 WorldClock. 56 is the first
        // value past the last message (a 0.5.6 client's PeekType stops at 54: it counts 55 as malformed and goes on, ReservedTypeTest's path).
        const uint8_t zero = 0, firstUnknown = 56, ff = 255, impact = 40, link = 44;
        bool allKnown = true;
        for (uint8_t t = 1; t <= 31; t++)
            allKnown = allKnown && proto::PeekType(&t, 1, m) && (uint8_t)m == t;
        for (uint8_t t = 45; t <= 55; t++)
            allKnown = allKnown && proto::PeekType(&t, 1, m) && (uint8_t)m == t;
        Check("codec: PeekType accepts 1..31, 40, 44 and 45..55 and rejects empty, 0, 56, 255",
              allKnown && proto::kMsgLast == 55 &&proto::PeekType(&impact, 1, m) && m == proto::Msg::VehicleImpact && proto::PeekType(&link, 1, m) && m == proto::Msg::LinkInfo && !proto::PeekType(&zero, 0, m) &&
                  !proto::PeekType(nullptr, 0, m) && !proto::PeekType(&zero, 1, m) && !proto::PeekType(&firstUnknown, 1, m) && !proto::PeekType(&ff, 1, m));
    }

    // Non-finite floats, in each of the seven fields, in PlayerState and a WorldState entry.
    {
        float proto::Kinematics::*const fields[7] = {
            &proto::Kinematics::x,  &proto::Kinematics::y,  &proto::Kinematics::z,  &proto::Kinematics::heading,
            &proto::Kinematics::vx, &proto::Kinematics::vy, &proto::Kinematics::vz};
        const float bads[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
        int cases = 0, accepted = 0, controlOk = 0;
        for (auto field : fields)
            for (float bad : bads)
            {
                proto::PlayerState ps;
                ps.k.*field = bad;
                proto::WorldState ws;
                proto::WorldEntry e;
                e.k.*field = bad;
                ws.players.push_back(e);
                cases += 2;
                if (DecodeAs<proto::PlayerState>(proto::Encode(ps).data(), proto::Encode(ps).size())) accepted++;
                if (DecodeAs<proto::WorldState>(proto::Encode(ws).data(), proto::Encode(ws).size())) accepted++;
            }
        // Control: the same packets with a finite value decode.
        proto::PlayerState okPs;
        proto::WorldState okWs;
        okWs.tick = 1; // tick 0 means "nothing received" and is refused by Decode
        okWs.players.push_back(proto::WorldEntry());
        controlOk = DecodeAs<proto::PlayerState>(proto::Encode(okPs).data(), proto::Encode(okPs).size()) &&
                    DecodeAs<proto::WorldState>(proto::Encode(okWs).data(), proto::Encode(okWs).size());
        Check("codec: NaN and +-Inf are rejected in every kinematics field (42 cases)",
              cases == 42 && accepted == 0 && controlOk,
              std::to_string(cases) + " cases, " + std::to_string(accepted) + " accepted");
    }

    // Over-cap strings: the cap itself decodes, one byte more does not.
    {
        auto at = [](size_t n) { return std::string(n, 'a'); };
        struct Pair { bool atCap, overCap; };
        std::vector<Pair> r;
        auto hello = [&](size_t n) { proto::Hello m; m.name = at(n); auto b = proto::Encode(m); return DecodeAs<proto::Hello>(b.data(), b.size()); };
        auto welcome = [&](size_t n) { proto::Welcome m; m.serverName = at(n); auto b = proto::Encode(m); return DecodeAs<proto::Welcome>(b.data(), b.size()); };
        auto refuse = [&](size_t n) { proto::Refuse m; m.reason = proto::RefuseReason::BadName; m.text = at(n); auto b = proto::Encode(m); return DecodeAs<proto::Refuse>(b.data(), b.size()); };
        auto joined = [&](size_t n) { proto::PlayerJoined m; m.name = at(n); auto b = proto::Encode(m); return DecodeAs<proto::PlayerJoined>(b.data(), b.size()); };
        auto chat = [&](size_t n) { proto::ChatSend m; m.text = at(n); auto b = proto::Encode(m); return DecodeAs<proto::ChatSend>(b.data(), b.size()); };
        auto msg = [&](size_t n) { proto::ChatMessage m; m.text = at(n); auto b = proto::Encode(m); return DecodeAs<proto::ChatMessage>(b.data(), b.size()); };
        r.push_back({hello(proto::kMaxName), hello(proto::kMaxName + 1)});
        r.push_back({welcome(proto::kMaxServerName), welcome(proto::kMaxServerName + 1)});
        r.push_back({refuse(proto::kMaxRefuseText), refuse(proto::kMaxRefuseText + 1)});
        r.push_back({joined(proto::kMaxName), joined(proto::kMaxName + 1)});
        r.push_back({chat(proto::kMaxChat), chat(proto::kMaxChat + 1)});
        r.push_back({msg(proto::kMaxChat), msg(proto::kMaxChat + 1)});
        std::string bad;
        for (size_t i = 0; i < r.size(); i++)
            if (!r[i].atCap || r[i].overCap) bad += std::to_string(i) + " ";
        Check("codec: every string decodes at its cap and is rejected one byte over", r.size() == 6 && bad.empty(),
              "bad: " + bad);
    }

    // Unknown enum values.
    {
        int accepted = 0;
        for (int v : {0, (int)proto::RefuseReason::AuthUnavailable + 1, 255})
        {
            proto::Refuse r;
            r.reason = (proto::RefuseReason)v;
            if (DecodeAs<proto::Refuse>(proto::Encode(r).data(), proto::Encode(r).size())) accepted++;
        }
        for (int v : {0, 4, 255})
        {
            proto::PlayerLeft l;
            l.reason = (proto::LeaveReason)v;
            if (DecodeAs<proto::PlayerLeft>(proto::Encode(l).data(), proto::Encode(l).size())) accepted++;
        }
        proto::Refuse okR;
        okR.reason = proto::RefuseReason::AuthUnavailable; // the last known value (v8)
        proto::PlayerLeft okL;
        okL.reason = proto::LeaveReason::Kicked;
        Check("codec: unknown RefuseReason / LeaveReason values are rejected, the last known ones accepted",
              accepted == 0 && DecodeAs<proto::Refuse>(proto::Encode(okR).data(), proto::Encode(okR).size()) &&
                  DecodeAs<proto::PlayerLeft>(proto::Encode(okL).data(), proto::Encode(okL).size()),
              std::to_string(accepted) + " accepted");
    }

    // WorldState count limits and lies.
    {
        const proto::Bytes max = RawWorld(proto::kMaxPlayers, proto::kMaxPlayers);
        const proto::Bytes over = RawWorld(proto::kMaxPlayers + 1, proto::kMaxPlayers + 1);
        const proto::Bytes empty = RawWorld(0, 0);
        const proto::Bytes huge = RawWorld(0xFFFF, 0);
        const proto::Bytes tooFew = RawWorld(3, 2);
        const proto::Bytes tooMany = RawWorld(2, 3);
        Check("codec: WorldState with n = kMaxPlayers decodes, n = 0 decodes",
              DecodeAs<proto::WorldState>(max.data(), max.size()) &&
                  DecodeAs<proto::WorldState>(empty.data(), empty.size()));
        Check("codec: WorldState n > kMaxPlayers is rejected even when the body matches",
              !DecodeAs<proto::WorldState>(over.data(), over.size()) && !DecodeAs<proto::WorldState>(huge.data(), huge.size()));
        Check("codec: WorldState whose count disagrees with its body is rejected",
              !DecodeAs<proto::WorldState>(tooFew.data(), tooFew.size()) &&
                  !DecodeAs<proto::WorldState>(tooMany.data(), tooMany.size()));

        // Strictness of the v2 body: every one of these is a well-framed packet that must still be refused.
        auto world = [](const std::function<void(wire::Writer&)>& body) {
            wire::Writer w;
            w.u8((uint8_t)proto::Msg::WorldState);
            w.u32(5);
            body(w);
            return w.take();
        };
        auto zeroState = [](wire::Writer& w) { w.u64(0); w.u16(0); w.i16(0); w.i16(0); w.i16(0); w.u8(0); w.u16(0); w.u32(0); w.u8(0); w.u8(100); w.u8(0); }; // + v4 ped block and vitals
        const proto::Bytes okOne = world([&](wire::Writer& w) { w.u8(0); w.varu32(1); w.varu32(0); w.u8(0); w.u8(0); zeroState(w); });
        const proto::Bytes needsBase = world([&](wire::Writer& w) { w.u8(0); w.varu32(1); w.varu32(0); w.u8(1); w.u8(0); }); // delta, no history
        const proto::Bytes idPastMax = world([&](wire::Writer& w) { w.u8(0); w.varu32(1); w.varu32(proto::kMaxPlayers); w.u8(0); w.u8(0); zeroState(w); });
        const proto::Bytes leaveTooBig = world([&](wire::Writer& w) { w.u8(1); w.u16(proto::kMaxPlayers); w.varu32(0); });
        const proto::Bytes flagsOnAbs = world([&](wire::Writer& w) { w.u8(0); w.varu32(1); w.varu32(0); w.u8(0x20); w.u8(0); zeroState(w); });
        const proto::Bytes nonCanonical = world([&](wire::Writer& w) { w.u8(0); w.u8(0x81); w.u8(0x00); }); // count 1 as 81 00
        const proto::Bytes outOfBox = world([&](wire::Writer& w) {
            w.u8(0); w.varu32(1); w.varu32(0); w.u8(0); w.u8(0);
            w.u64((uint64_t)(2000000u & 0x3FFFFFu)); // x = 20 km: inside the 22-bit field, outside the +-16.4 km box
            w.u16(0); w.i16(0); w.i16(0); w.i16(0);
            w.u8(0); w.u16(0); w.u32(0); w.u8(0); w.u8(100); w.u8(0); // v4 ped block and vitals: complete, so the box is the only fault
        });
        const proto::Bytes leaveCountLies = world([&](wire::Writer& w) { w.u8(3); w.u16(1); w.varu32(0); });
        Check("codec: WorldState entries that need a baseline, ids past the cap, odd flags, long varints, out-of-box values and lying leave counts are rejected, a plain one decodes",
              DecodeAs<proto::WorldState>(okOne.data(), okOne.size()) && !DecodeAs<proto::WorldState>(needsBase.data(), needsBase.size()) &&
                  !DecodeAs<proto::WorldState>(idPastMax.data(), idPastMax.size()) &&
                  !DecodeAs<proto::WorldState>(leaveTooBig.data(), leaveTooBig.size()) &&
                  !DecodeAs<proto::WorldState>(flagsOnAbs.data(), flagsOnAbs.size()) &&
                  !DecodeAs<proto::WorldState>(nonCanonical.data(), nonCanonical.size()) &&
                  !DecodeAs<proto::WorldState>(outOfBox.data(), outOfBox.size()) &&
                  !DecodeAs<proto::WorldState>(leaveCountLies.data(), leaveCountLies.size()));
    }

    // Decode leaves the output alone on failure; Encode refuses what cannot be framed.
    {
        proto::Hello keep;
        keep.name = "keep";
        proto::Bytes bad = Samples()[0].bytes;
        bad.push_back(9);
        const bool rejected = !proto::Decode(bad.data(), bad.size(), keep);
        proto::Hello huge;
        huge.name = std::string(0x10000, 'a');
        Check("codec: a failed Decode leaves the output untouched", rejected && keep.name == "keep");
        Check("codec: Encode returns empty for a string that cannot be framed", proto::Encode(huge).empty());
    }

    // ValidName.
    {
        const std::vector<std::string> good = {"Alice", "a", "A_b-c.d", "two words", "Z9", std::string(24, 'x'), "a b c"};
        const std::vector<std::string> bad = {"", " a", "a ", "a  b", std::string(25, 'x'), "bad<name>", Raw({'n', 0xC3, 0xAF, 'v', 'e'}),
                                              Raw({'t', 'a', 'b', '\t', 'x'}), "semi;colon", "slash/", "new\nline", "quote\""};
        std::string wrong;
        for (const auto& s : good) if (!proto::ValidName(s)) wrong += "[good:" + s + "]";
        for (const auto& s : bad) if (proto::ValidName(s)) wrong += "[bad:" + s + "]";
        Check("codec: ValidName accepts safe names and rejects empty, padded, doubled-space, long, unsafe",
              wrong.empty(), wrong);
    }

    // ValidKinematics: edges in, just past the edges out.
    {
        bool in = proto::ValidKinematics(Kin(0, 0, 30)) &&
                  proto::ValidKinematics(Kin(16000, -16000, 3000, 360, 600, -600, 600)) &&
                  proto::ValidKinematics(Kin(-16000, 16000, -2000, -360, -600, 600, -600));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        std::string wrong;
        auto out = [&](const char* what, const proto::Kinematics& k) { if (proto::ValidKinematics(k)) wrong += what; };
        out("[x+]", Kin(16000.5f, 0, 0));
        out("[y-]", Kin(0, -16000.5f, 0));
        out("[z+]", Kin(0, 0, 3000.5f));
        out("[z-]", Kin(0, 0, -2000.5f));
        out("[heading]", Kin(0, 0, 0, 360.5f));
        out("[vx]", Kin(0, 0, 0, 0, 600.5f));
        out("[vz]", Kin(0, 0, 0, 0, 0, 0, -600.5f));
        out("[nan]", Kin(nan, 0, 0));
        out("[inf]", Kin(0, 0, inf));
        out("[nan-heading]", Kin(0, 0, 0, nan));
        Check("codec: ValidKinematics accepts the world-box edges and rejects just past them, NaN and Inf",
              in && wrong.empty(), wrong);
    }

    // ValidChat.
    {
        const std::vector<std::string> good = {"hello", Raw({'h', 0xC3, 0xA9, 'l', 'l', 'o'}), Raw({0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC}),
                                               Raw({0xF0, 0x9F, 0x98, 0x80}), std::string(200, 'a'), "with  spaces and punctuation!?"};
        const std::vector<std::string> bad = {
            "", std::string(201, 'a'), "a\nb", "a\tb", "a\rb", Raw({'a', 0x01}), Raw({'a', 0x7F}),
            Raw({0xC2, 0x85}),                   // U+0085 (C1 control)
            Raw({0x80}),                         // lone continuation byte
            Raw({0xC0, 0x80}),                   // overlong NUL
            Raw({0xE0, 0x80, 0x80}),             // overlong
            Raw({0xE2, 0x82}),                   // truncated sequence
            Raw({0xED, 0xA0, 0x80}),             // UTF-16 surrogate
            Raw({0xF4, 0x90, 0x80, 0x80}),       // above U+10FFFF
            Raw({0xFF})};
        std::string wrong;
        for (size_t i = 0; i < good.size(); i++) if (!proto::ValidChat(good[i])) wrong += "[good" + std::to_string(i) + "]";
        for (size_t i = 0; i < bad.size(); i++) if (proto::ValidChat(bad[i])) wrong += "[bad" + std::to_string(i) + "]";
        Check("codec: ValidChat accepts UTF-8 text and rejects empty, over-cap, control chars and malformed UTF-8",
              wrong.empty(), wrong);
    }

    Check("codec: NameKey lower-cases ASCII, SeqNewer follows u32 wrap-around",
          proto::NameKey("AlIcE_9 X") == "alice_9 x" && proto::SeqNewer(2, 1) && !proto::SeqNewer(1, 2) &&
              !proto::SeqNewer(5, 5) && proto::SeqNewer(0, 0xFFFFFFF0u) && !proto::SeqNewer(0xFFFFFFF0u, 0) &&
              proto::SeqNewer(0x7FFFFFFFu, 0) && !proto::SeqNewer(0x80000000u, 0));
}

// The v2 resource-transfer messages (reliable channel): round trips, the exact wire layout, the
// generic wrong-type / truncation / trailing checks, the decode caps, the two validators and the
// CRC-32. Kept out of the ten-sample machinery above so those byte-exact checks stay untouched.
static void ResourceCodecTests()
{
    proto::ResourceManifest man;
    man.resources.push_back({"gage_logo.png", 2048, 0xCBF43926u});
    man.resources.push_back({"welcome.txt", 10, 0x414FA339u});
    const proto::Bytes manB = proto::Encode(man);

    proto::ResourceRequest req;
    req.index = 1;
    req.startChunk = 3;
    const proto::Bytes reqB = proto::Encode(req);

    proto::ResourceChunk chunk;
    chunk.index = 1;
    chunk.chunkIndex = 2;
    chunk.data = {0xDE, 0xAD, 0xBE, 0xEF};
    const proto::Bytes chunkB = proto::Encode(chunk);

    // Round trip: decode then re-encode is identical.
    {
        proto::ResourceManifest a;
        proto::ResourceRequest b;
        proto::ResourceChunk c;
        const bool rt = proto::Decode(manB.data(), manB.size(), a) && proto::Encode(a) == manB &&
                        proto::Decode(reqB.data(), reqB.size(), b) && proto::Encode(b) == reqB &&
                        proto::Decode(chunkB.data(), chunkB.size(), c) && proto::Encode(c) == chunkB;
        Check("codec: resource messages round-trip byte for byte", rt);
        Check("codec: resource decoded fields match what was encoded",
              a.resources.size() == 2 && a.resources[0].name == "gage_logo.png" && a.resources[0].size == 2048 &&
                  a.resources[0].crc32 == 0xCBF43926u && a.resources[1].name == "welcome.txt" &&
                  a.resources[1].crc32 == 0x414FA339u && b.index == 1 && b.startChunk == 3 && c.index == 1 &&
                  c.chunkIndex == 2 && c.data.size() == 4 && c.data[0] == 0xDE && c.data[3] == 0xEF);
    }

    // The wire layout, byte by byte (little-endian; u16-length name and data).
    {
        proto::ResourceManifest m1;
        m1.resources.push_back({"a", 0x01020304u, 0x0A0B0C0Du});
        const proto::Bytes wantMan = {11, 1, 1, 0, 'a', 0x04, 0x03, 0x02, 0x01, 0x0D, 0x0C, 0x0B, 0x0A};
        proto::ResourceRequest r1;
        r1.index = 2;
        r1.startChunk = 0x01020304u;
        const proto::Bytes wantReq = {12, 2, 0x04, 0x03, 0x02, 0x01};
        proto::ResourceChunk c1;
        c1.index = 3;
        c1.chunkIndex = 0x01020304u;
        c1.data = {0xAA, 0xBB};
        const proto::Bytes wantChunk = {13, 3, 0x04, 0x03, 0x02, 0x01, 2, 0, 0xAA, 0xBB};
        Check("codec: resource wire layout is type, fields, u16-length name/data, little-endian",
              proto::Encode(m1) == wantMan && proto::Encode(r1) == wantReq && proto::Encode(c1) == wantChunk,
              Hex(proto::Encode(m1)) + " | " + Hex(proto::Encode(r1)) + " | " + Hex(proto::Encode(c1)));
    }

    // A small table for the generic loops, plus the ten core samples to prove no decoder crosses
    // the resource boundary either way.
    struct RSample { proto::Bytes bytes; bool (*dec)(const uint8_t*, size_t); };
    const std::vector<RSample> rs = {
        {manB, &DecodeAs<proto::ResourceManifest>},
        {reqB, &DecodeAs<proto::ResourceRequest>},
        {chunkB, &DecodeAs<proto::ResourceChunk>},
    };
    const std::vector<Sample> core = Samples();

    // Wrong type byte: resource bytes rejected by every core decoder, resource decoders reject every
    // core sample and each other.
    {
        int accepted = 0;
        for (const RSample& a : rs)
            for (const Sample& b : core)
                if (b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        for (const RSample& a : rs)
            for (const RSample& b : rs)
                if (&a != &b && b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        for (const Sample& a : core)
            for (const RSample& b : rs)
                if (b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        Check("codec: a wrong type byte is rejected across the resource and core decoders", accepted == 0,
              std::to_string(accepted) + " accepted");
    }

    // Truncation at every length, and one trailing byte, for each resource message.
    {
        int cases = 0, accepted = 0, trailing = 0, trailingAccepted = 0;
        for (const RSample& s : rs)
        {
            for (size_t len = 0; len < s.bytes.size(); len++)
            {
                cases++;
                if (s.dec(s.bytes.data(), len)) accepted++;
            }
            proto::Bytes longer = s.bytes;
            longer.push_back(0);
            trailing++;
            if (s.dec(longer.data(), longer.size())) trailingAccepted++;
        }
        Check("codec: resource truncation is rejected at every length", cases > 20 && accepted == 0,
              std::to_string(cases) + " cases, " + std::to_string(accepted) + " accepted");
        Check("codec: one trailing byte is rejected for every resource message",
              trailing == 3 && trailingAccepted == 0, std::to_string(trailingAccepted) + " accepted");
    }

    // ResourceManifest count cap: kMaxResources decodes, one more is rejected.
    {
        auto manOf = [](size_t count) {
            proto::ResourceManifest m;
            for (size_t i = 0; i < count; i++) m.resources.push_back({"r", 1, 0});
            return proto::Encode(m);
        };
        const proto::Bytes atCap = manOf(proto::kMaxResources);
        const proto::Bytes overCap = manOf(proto::kMaxResources + 1);
        Check("codec: ResourceManifest decodes at kMaxResources and is rejected one over",
              DecodeAs<proto::ResourceManifest>(atCap.data(), atCap.size()) &&
                  !DecodeAs<proto::ResourceManifest>(overCap.data(), overCap.size()));
    }

    // ResourceManifest per-resource size cap.
    {
        proto::ResourceManifest at;
        at.resources.push_back({"r", proto::kMaxResourceBytes, 0});
        proto::ResourceManifest over;
        over.resources.push_back({"r", proto::kMaxResourceBytes + 1, 0});
        Check("codec: ResourceManifest accepts a size at kMaxResourceBytes and rejects one over",
              DecodeAs<proto::ResourceManifest>(proto::Encode(at).data(), proto::Encode(at).size()) &&
                  !DecodeAs<proto::ResourceManifest>(proto::Encode(over).data(), proto::Encode(over).size()));
    }

    // ResourceChunk data cap: kResourceChunk decodes, one over is rejected, an empty chunk is rejected.
    {
        proto::ResourceChunk at;
        at.data.assign(proto::kResourceChunk, 0xAB);
        proto::ResourceChunk over;
        over.data.assign((size_t)proto::kResourceChunk + 1, 0xAB);
        proto::ResourceChunk empty; // data empty
        Check("codec: ResourceChunk accepts kResourceChunk bytes, rejects one over and an empty chunk",
              DecodeAs<proto::ResourceChunk>(proto::Encode(at).data(), proto::Encode(at).size()) &&
                  !DecodeAs<proto::ResourceChunk>(proto::Encode(over).data(), proto::Encode(over).size()) &&
                  !DecodeAs<proto::ResourceChunk>(proto::Encode(empty).data(), proto::Encode(empty).size()));
    }

    // ValidResourceName.
    {
        const std::vector<std::string> good = {"a", "gage_logo.png", "A-b_c.1", "file.name.ext", std::string(64, 'x')};
        const std::vector<std::string> bad = {"",      ".",     "..",         ".hidden",         "trailing.",
                                              "a..b",  "a/b",   "a\\b",       "has space",       std::string(65, 'x'),
                                              "semi;colon", "a..", "../x"};
        std::string wrong;
        for (const auto& s : good) if (!proto::ValidResourceName(s)) wrong += "[good:" + s + "]";
        for (const auto& s : bad) if (proto::ValidResourceName(s)) wrong += "[bad:" + s + "]";
        Check("codec: ValidResourceName accepts safe leaf names and rejects empty, dot, traversal, separators, long",
              wrong.empty(), wrong);
    }

    // ValidManifest.
    {
        proto::ResourceManifest ok;
        ok.resources = {{"a.png", 10, 1}, {"b.txt", 20, 2}};
        proto::ResourceManifest dup;
        dup.resources = {{"a.png", 10, 1}, {"A.PNG", 20, 2}}; // case-insensitive duplicate
        proto::ResourceManifest badName;
        badName.resources = {{"../x", 10, 1}};
        proto::ResourceManifest tooMany;
        for (int i = 0; i < 17; i++) tooMany.resources.push_back({"r", 1, 0}); // 17 > kMaxResources
        proto::ResourceManifest tooBig;
        for (char c = 'a'; c <= 'e'; c++) // 5 * 8 MiB = 40 MiB > kMaxResourceTotal
            tooBig.resources.push_back({std::string(1, c), proto::kMaxResourceBytes, 0});
        Check("codec: ValidManifest accepts a clean manifest and rejects dup names, bad names, over-count and over-total",
              proto::ValidManifest(ok) && !proto::ValidManifest(dup) && !proto::ValidManifest(badName) &&
                  !proto::ValidManifest(tooMany) && !proto::ValidManifest(tooBig));
    }

    // CRC-32 against the IEEE check vectors, whole and summed in two parts.
    {
        const uint8_t empty1 = 0;
        uint32_t part = proto::Crc32((const uint8_t*)"12345", 5);
        part = proto::Crc32((const uint8_t*)"6789", 4, part);
        Check("codec: Crc32 matches the IEEE check vectors (whole, empty, incremental)",
              proto::Crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u && proto::Crc32(&empty1, 0) == 0u &&
                  proto::Crc32((const uint8_t*)"a", 1) == 0xE8B7BE43u && part == 0xCBF43926u);
    }
}

// ===========================================================================
// B. Server config
// ===========================================================================

static void WriteFile(const char* path, const std::string& text)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

static void ConfigTests()
{
    server::Config def;
    std::string err;
    Check("config: the defaults are valid", server::Validate(def, &err), err);

    auto invalid = [](const std::function<void(server::Config&)>& mutate) {
        server::Config c;
        mutate(c);
        std::string e;
        return !server::Validate(c, &e);
    };
    Check("config: maxPlayers 0 or over kMaxPlayers, tickHz 0 or 121, empty / long / control-char name are invalid",
          invalid([](server::Config& c) { c.maxPlayers = 0; }) &&
              invalid([](server::Config& c) { c.maxPlayers = proto::kMaxPlayers + 1; }) &&
              invalid([](server::Config& c) { c.tickHz = 0; }) && invalid([](server::Config& c) { c.tickHz = 121; }) &&
              invalid([](server::Config& c) { c.name = ""; }) &&
              invalid([](server::Config& c) { c.name = std::string(proto::kMaxServerName + 1, 'x'); }) &&
              invalid([](server::Config& c) { c.name = "bad\nname"; }));

    const char* path = "gage_netcode_test_server.json";
    WriteFile(path, "{ \"name\": \"Test Server\", \"port\": 25000, \"maxPlayers\": 10, \"tickHz\": 30 }\n");
    server::Config loaded;
    bool missing = true;
    const bool ok = server::LoadConfigFile(path, loaded, &err, &missing);
    Check("config: server.json loads name, port, maxPlayers and tickHz",
          ok && !missing && loaded.name == "Test Server" && loaded.port == 25000 && loaded.maxPlayers == 10 &&
              loaded.tickHz == 30, err);

    // Each of these must fail and leave the config alone: a typo cannot silently keep a default.
    const std::vector<std::string> badFiles = {
        "{ \"name\": \"x\", \"maxPlayer\": 3 }",     // unknown key (typo)
        "{ \"port\": \"24800\" }",                    // wrong type
        "{ \"tickHz\": 500 }",                        // out of range
        "{ \"port\": 24800.5 }",                      // not whole
        "{ \"name\": ",                               // not JSON
        "{} trailing",                                // junk after the object
        "[1, 2]",                                     // not an object
    };
    int rejected = 0, untouched = 0;
    for (const std::string& text : badFiles)
    {
        WriteFile(path, text);
        server::Config c;
        c.name = "sentinel";
        c.port = 31337;
        bool miss = true;
        if (!server::LoadConfigFile(path, c, &err, &miss) && !miss) rejected++;
        if (c.name == "sentinel" && c.port == 31337) untouched++;
    }
    Check("config: unknown key, wrong type, out of range, not JSON, trailing junk, non-object are all rejected untouched",
          rejected == 7 && untouched == 7, std::to_string(rejected) + " rejected, " + std::to_string(untouched) + " untouched");

    // The replication knobs: the defaults are the documented ones, server.json can change them, and
    // nonsense (leave radius under the enter radius, rates out of order, ranges) is refused untouched.
    {
        server::Config d;
        // Sync plan W7: 30 Hz tick, 30/15/10 Hz rates, 36000 B/s (they were 20, 20/10/5 and 24000).
        Check("config: the replication defaults are 30 Hz tick, 400 m enter, 450 m leave, 30/15/10 Hz, 36000 B/s per client, 1200 B packets",
              d.tickHz == 30 && d.repl.enterRadius == 400 && d.repl.leaveRadius == 450 && d.repl.rateNearHz == 30 &&
                  d.repl.rateMidHz == 15 && d.repl.rateFarHz == 10 && d.repl.budgetBytesPerSec == 36000 && d.repl.packetMaxBytes == 1200);
        WriteFile(path, "{ \"relevanceEnterRadius\": 300, \"relevanceLeaveRadius\": 380, \"relevanceMaxRadius\": 500, "
                        "\"rateNearHz\": 20, \"rateMidHz\": 8, \"rateFarHz\": 4, \"clientBudgetBytesPerSec\": 30000, "
                        "\"worldPacketMaxBytes\": 1000, \"relevanceCellSize\": 100 }");
        server::Config r;
        err.clear();
        const bool loadedRepl = server::LoadConfigFile(path, r, &err, &missing);
        Check("config: server.json sets the replication knobs",
              loadedRepl && r.repl.enterRadius == 300 && r.repl.leaveRadius == 380 && r.repl.maxRadius == 500 &&
                  r.repl.rateMidHz == 8 && r.repl.rateFarHz == 4 && r.repl.budgetBytesPerSec == 30000 &&
                  r.repl.packetMaxBytes == 1000 && r.repl.cellSize == 100,
              err);
        auto badRepl = [&](const char* text) {
            WriteFile(path, text);
            server::Config c;
            c.repl.enterRadius = 123;
            std::string e;
            bool miss2 = true;
            return !server::LoadConfigFile(path, c, &e, &miss2) && !miss2 && c.repl.enterRadius == 123;
        };
        const bool badOnes = badRepl("{ \"relevanceEnterRadius\": 500, \"relevanceLeaveRadius\": 450 }") &&
                             badRepl("{ \"relevanceEnterRadius\": 900, \"relevanceMaxRadius\": 800, \"relevanceLeaveRadius\": 950 }") &&
                             badRepl("{ \"rateNearHz\": 5, \"rateMidHz\": 10 }") && badRepl("{ \"rateFarHz\": 0 }") &&
                             badRepl("{ \"rateMidHz\": 121 }") && badRepl("{ \"worldPacketMaxBytes\": 1301 }") &&
                             badRepl("{ \"clientBudgetBytesPerSec\": 999 }") && badRepl("{ \"nearDistance\": 300, \"midDistance\": 200 }") &&
                             badRepl("{ \"coarseSharePercent\": 0 }") && badRepl("{ \"relevanceCellSize\": 10 }") &&
                             badRepl("{ \"relevanceEnterRadius\": 400.5 }") && badRepl("{ \"relevanceEnterRadius\": \"400\" }");
        Check("config: a leave radius under the enter radius, rates out of order, and out-of-range replication values are refused untouched (12 cases)",
              badOnes);
        Check("config: Validate refuses the same nonsense when a Config is built in code",
              invalid([](server::Config& c) { c.repl.leaveRadius = 399; }) && invalid([](server::Config& c) { c.repl.rateMidHz = 31; }) /* above the 30 Hz near rate */ &&
                  invalid([](server::Config& c) { c.repl.packetMaxBytes = 199; }) &&
                  !invalid([](server::Config& c) { c.repl.leaveRadius = 400; }) /* equal radii are allowed: no hysteresis */);
    }
    std::remove(path);

    server::Config c;
    bool miss = false;
    const bool loadedMissing = server::LoadConfigFile("gage_netcode_test_no_such_file.json", c, &err, &miss);
    Check("config: a missing file reports missing (so the exe can fall back to defaults)", !loadedMissing && miss);
}

// ===========================================================================
// C. Network
// ===========================================================================


// --- handshake ---------------------------------------------------------------

static void HandshakeTests()
{
    Rig rig;
    if (!rig.Start()) { Check("handshake: server starts", false); return; }
    Probe a(rig);
    const bool joined = a.Join("Alice");
    const net::Event* w = a.First(ET::Welcome);
    Check("handshake: the server bound an ephemeral loopback port", rig.srv.port() != 0 && rig.srv.port() != proto::kDefaultPort,
          std::to_string(rig.srv.port()));
    Check("handshake: Welcome carries our id, the tick rate and the server name",
          joined && w && w->welcome.yourId < 8 && w->welcome.tickHz == 20 && w->welcome.serverName == "GAGE Test" &&
              a.s.myId() == w->welcome.yourId && a.s.tickHz() == 20 && a.s.serverName() == "GAGE Test",
          a.Describe());
    const auto players = rig.srv.Players();
    Check("handshake: the server lists the player under the name it sent",
          players.size() == 1 && players[0].name == "Alice" && players[0].id == a.s.myId() && players[0].address == "127.0.0.1");
    a.s.Disconnect();
    const bool closed = a.WaitClosed();
    Check("handshake: a graceful Disconnect ends in Disconnected(Local) and an empty roster",
          closed && a.DisconnectedWith(net::DisconnectReason::Local) && rig.Until([&] { return rig.srv.Players().empty(); }, 1000),
          a.Describe());
}

// --- refusals ----------------------------------------------------------------

static void RefusalTests()
{
    Rig rig;
    if (!rig.Start()) { Check("refuse: server starts", false); return; }

    {
        // A v1 client (protocol version 1) and a client from the future (kVersion + 1): both are refused cleanly.
        Probe v(rig), future(rig);
        v.Connect("Victor", [](net::SessionOptions& o) { o.protocolVersion = 1; });
        future.Connect("Fiona", [](net::SessionOptions& o) { o.protocolVersion = proto::kVersion + 1; });
        const bool closed = v.WaitClosed() && future.WaitClosed();
        const net::Event* r = v.First(ET::Refused);
        const net::Event* rf = future.First(ET::Refused);
        Check("refuse: a v1 client gets Refuse VersionMismatch, then the connection closes (so does a future one)",
              closed && v.RefusedWith(proto::RefuseReason::VersionMismatch) && v.DisconnectedWith(net::DisconnectReason::Refused) &&
                  !v.First(ET::Welcome) && future.RefusedWith(proto::RefuseReason::VersionMismatch) && !future.First(ET::Welcome),
              v.Describe());
        // The text is player-facing (the engine shows it on the loading screen): an older client is told to let the
        // launcher update, a newer one that the server is behind. Not the protocol numbers (those are in the server log).
        Check("refuse: an older client's version refusal tells the player the launcher installs the update",
              r && r->refuse.text == proto::kRefuseTextClientOld &&
                  r->refuse.text == "Solar RP was updated. Close the game; Solar Launcher installs the update.",
              r ? r->refuse.text : "no Refuse");
        Check("refuse: a newer client's version refusal says the server has not been updated yet",
              rf && rf->refuse.text == proto::kRefuseTextClientNew &&
                  rf->refuse.text == "This server has not been updated yet. Try again in a minute.",
              rf ? rf->refuse.text : "no Refuse");
        Check("refuse: the two version texts differ and fit the loader's 150-byte status line",
              std::string(proto::kRefuseTextClientOld) != proto::kRefuseTextClientNew &&
                  std::string(proto::kRefuseTextClientOld).size() <= 150 && std::string(proto::kRefuseTextClientNew).size() <= 150);
        Check("refuse: the server never sent the refused client a Welcome, a roster or any world data",
              v.worlds == 0 && v.coarseEvents == 0 && v.Count(ET::PlayerJoined) == 0 && rig.srv.Players().empty(),
              v.Describe());
    }

    Probe alice(rig);
    const bool aliceIn = alice.Join("Alice");

    // Bad names, each on its own connection.
    {
        const std::vector<std::string> names = {"", "bad<name>", " lead", "trail ", "dou  ble"};
        int refused = 0;
        for (const std::string& n : names)
        {
            Probe p(rig);
            p.Connect(n.c_str());
            p.WaitClosed();
            if (p.RefusedWith(proto::RefuseReason::BadName) && !p.First(ET::Welcome)) refused++;
        }
        Check("refuse: empty, unsafe-charset, padded and double-spaced names get Refuse BadName (5 cases)", refused == 5,
              std::to_string(refused) + " refused");
    }
    {
        Probe p(rig);
        p.Connect(std::string(proto::kMaxName + 1, 'x').c_str());
        p.WaitClosed();
        Check("refuse: a name over the cap is malformed on the wire, so Refuse Protocol", p.RefusedWith(proto::RefuseReason::Protocol),
              p.Describe());
    }

    // Duplicate, case-insensitively; the original is untouched; the name frees up on leave.
    {
        Probe dup(rig);
        dup.Connect("aLICE");
        dup.WaitClosed();
        Check("refuse: a name that differs only in case gets Refuse NameTaken, the first player stays",
              dup.RefusedWith(proto::RefuseReason::NameTaken) && aliceIn && alice.Joined() && rig.srv.Players().size() == 1,
              dup.Describe());
    }
    alice.s.Disconnect();
    alice.WaitClosed();
    rig.Until([&] { return rig.srv.Players().empty(); }, 1000);
    {
        Probe again(rig);
        const bool in = again.Join("ALICE");
        Check("refuse: the name is free again once its owner has left", in && rig.srv.Players().size() == 1 &&
                                                                          rig.srv.Players()[0].name == "ALICE");
    }
}

static void ServerFullTest()
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.maxPlayers = 2;
    if (!rig.Start(cfg)) { Check("full: server starts", false); return; }
    Probe a(rig), b(rig), c(rig);
    const bool two = a.Join("Ann") && b.Join("Ben");
    c.Connect("Cat");
    c.WaitClosed();
    rig.Pump(100);
    Check("full: the third player gets Refuse ServerFull; the two inside are untouched and never hear of them",
          two && c.RefusedWith(proto::RefuseReason::ServerFull) && a.Joined() && b.Joined() && !a.SawJoined(2, "Cat") &&
              a.Count(ET::PlayerJoined) == 1 && rig.srv.Players().size() == 2,
          c.Describe());
}

static void HelloTimeoutTest()
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.helloTimeoutMs = 300;
    if (!rig.Start(cfg)) { Check("hello timeout: server starts", false); return; }
    Probe p(rig);
    p.Connect("Quiet", [](net::SessionOptions& o) { o.autoHello = false; });
    const auto t0 = Clock::now();
    const bool closed = p.WaitClosed(3000);
    const int64_t ms = MsSince(t0);
    // The timer is the binding rule: not before it is due, and well before the test budget.
    Check("hello timeout: a connection that never says Hello is refused HelloTimeout after about helloTimeoutMs",
          closed && p.RefusedWith(proto::RefuseReason::HelloTimeout) && ms >= 250 && ms < 2000,
          std::to_string(ms) + " ms, " + p.Describe());
}

// --- protocol violations -------------------------------------------------------

static void ViolationTests()
{
    Rig rig;
    if (!rig.Start()) { Check("violations: server starts", false); return; }
    Probe bystander(rig);
    const bool inside = bystander.Join("Witness");

    auto before = [&](const char* label, const std::function<void(Probe&)>& send) {
        Probe p(rig);
        p.Connect("Early", [](net::SessionOptions& o) { o.autoHello = false; });
        rig.Until([&] { return p.s.state() == net::SessionState::Joining; }, 2000);
        send(p);
        p.WaitClosed();
        return p.RefusedWith(proto::RefuseReason::Protocol) && !p.First(ET::Welcome) && p.Closed() ? std::string() : std::string(label);
    };
    Check("violations: ChatSend before Hello is refused Protocol",
          before("chat", [](Probe& p) { p.SendReliable(ChatBytes("hi")); }).empty());
    Check("violations: an unknown message type before Hello is refused Protocol",
          before("junk", [](Probe& p) { p.SendReliable(proto::Bytes{0xEE, 1, 2, 3}); }).empty());
    Check("violations: a truncated Hello is refused Protocol",
          before("trunc", [](Probe& p) { p.SendReliable(proto::Bytes{(uint8_t)proto::Msg::Hello, 1}); }).empty());
    Check("violations: a Hello on the state channel is refused Protocol",
          before("hellochan", [](Probe& p) {
              proto::Hello h;
              h.name = "Early";
              p.s.SendRaw(proto::kChanState, proto::Encode(h), false);
          }).empty());
    rig.Pump(100);
    Check("violations: none of those connections ever showed up to the player who was inside",
          inside && bystander.Count(ET::PlayerJoined) == 0 && bystander.Count(ET::PlayerLeft) == 0 && rig.srv.Players().size() == 1,
          bystander.Describe());

    // Joined players that break the rules are cut off and announced as Kicked.
    auto joinedViolation = [&](const char* name, const std::function<void(Probe&)>& send, bool& chatLeaked) {
        Probe p(rig);
        p.Join(name);
        const uint16_t id = p.s.myId();
        send(p);
        p.WaitClosed();
        rig.Until([&] { return bystander.SawLeft(id, proto::LeaveReason::Kicked); }, 1000);
        chatLeaked = bystander.Count(ET::Chat) != 0;
        return p.RefusedWith(proto::RefuseReason::Protocol) && bystander.SawLeft(id, proto::LeaveReason::Kicked);
    };
    bool leaked = false;
    Check("violations: a second Hello from a joined player cuts it off (Refuse Protocol, PlayerLeft Kicked)",
          joinedViolation("Twice", [](Probe& p) {
              proto::Hello h;
              h.name = "Twice";
              p.SendReliable(proto::Encode(h));
          }, leaked));
    Check("violations: ChatSend on the state channel cuts the sender off",
          joinedViolation("WrongChan", [](Probe& p) { p.s.SendRaw(proto::kChanState, ChatBytes("hi"), false); }, leaked));
    Check("violations: an over-cap chat cuts the sender off and is never delivered",
          joinedViolation("Shouter", [](Probe& p) { p.SendReliable(ChatBytes(std::string(proto::kMaxChat + 1, 'a'))); }, leaked) &&
              !leaked);
    Check("violations: PlayerLeft Kicked frees the name and the slot",
          rig.Until([&] { return rig.srv.Players().size() == 1; }, 1000) && rig.srv.Players()[0].name == "Witness");
}

// --- join and leave --------------------------------------------------------------

static void JoinLeaveTests()
{
    Rig rig;
    if (!rig.Start()) { Check("join/leave: server starts", false); return; }
    Probe a(rig), b(rig), c(rig), d(rig);
    const bool ia = a.Join("Alice");
    const bool ib = b.Join("Bob");
    rig.Pump(100);
    Check("join: the newcomer is announced to the player already in", ia && ib && a.SawJoined(b.s.myId(), "Bob"), a.Describe());
    {
        // B hears Welcome first, then the existing player.
        int welcomeAt = -1, joinedAt = -1;
        for (size_t i = 0; i < b.events.size(); i++)
        {
            if (b.events[i].type == ET::Welcome && welcomeAt < 0) welcomeAt = (int)i;
            if (b.events[i].type == ET::PlayerJoined && b.events[i].joined.id == a.s.myId() && joinedAt < 0) joinedAt = (int)i;
        }
        Check("join: the newcomer hears Welcome, then PlayerJoined for the player who was in",
              welcomeAt == 0 && joinedAt == 1 && b.SawJoined(a.s.myId(), "Alice") && !b.SawJoined(b.s.myId(), "Bob"), b.Describe());
    }
    const bool ic = c.Join("Carol");
    rig.Pump(100);
    Check("join: a third player hears about both who were in, and both of them hear about her",
          ic && c.SawJoined(a.s.myId(), "Alice") && c.SawJoined(b.s.myId(), "Bob") && a.SawJoined(c.s.myId(), "Carol") &&
              b.SawJoined(c.s.myId(), "Carol") && c.Count(ET::PlayerJoined) == 2,
          c.Describe());

    // Quit: B says goodbye.
    const uint16_t bId = b.s.myId(), cId = c.s.myId();
    b.s.Disconnect();
    rig.Until([&] { return a.SawLeft(bId, proto::LeaveReason::Quit) && c.SawLeft(bId, proto::LeaveReason::Quit); }, 2000);
    Check("leave: a deliberate quit is PlayerLeft Quit to everyone else",
          a.SawLeft(bId, proto::LeaveReason::Quit) && c.SawLeft(bId, proto::LeaveReason::Quit) && !a.SawLeft(bId, proto::LeaveReason::Timeout),
          a.Describe());

    // Timeout: C vanishes without a word (like a crash or a pulled cable).
    c.s.Abandon();
    const auto t0 = Clock::now();
    const bool timedOut = rig.Until([&] { return a.SawLeft(cId, proto::LeaveReason::Timeout); }, 6000);
    Check("leave: a client that goes silent is PlayerLeft Timeout (ENet timeouts set low for the test)",
          timedOut && !a.SawLeft(cId, proto::LeaveReason::Quit), std::to_string(MsSince(t0)) + " ms, " + a.Describe());

    // Kick through the console.
    const bool id = d.Join("Dave");
    const uint16_t dId = d.s.myId();
    rig.Pump(100);
    std::string out;
    const server::CommandResult listRes = rig.srv.Execute("list", out);
    const bool listOk = listRes == server::CommandResult::Ok && out.find("players 2/8") != std::string::npos &&
                        out.find("Alice") != std::string::npos && out.find("Dave") != std::string::npos;
    Check("console: list shows the roster", listOk, out);
    const server::CommandResult noKick = rig.srv.Execute("kick nobody", out);
    const bool noKickText = out.find("nobody") != std::string::npos;
    const server::CommandResult kickRes = rig.srv.Execute("kick dAVE", out);
    d.WaitClosed();
    rig.Until([&] { return a.SawLeft(dId, proto::LeaveReason::Kicked); }, 1000);
    Check("console: kick <name> (case-insensitive) cuts the player off and announces PlayerLeft Kicked",
          id && kickRes == server::CommandResult::Ok && d.DisconnectedWith(net::DisconnectReason::Kicked) &&
              a.SawLeft(dId, proto::LeaveReason::Kicked) && rig.srv.Players().size() == 1,
          d.Describe());
    Check("console: kick of an unknown player or id is an error that names it",
          noKick == server::CommandResult::Error && noKickText &&
              rig.srv.Execute("kick 999", out) == server::CommandResult::Error && rig.srv.Execute("kick", out) == server::CommandResult::Error);
    const server::CommandResult q = rig.srv.Execute("quit", out);
    Check("console: quit returns Quit, an unknown command is an error",
          q == server::CommandResult::Quit && rig.srv.Execute("frobnicate", out) == server::CommandResult::Error);
}

// --- state sync ----------------------------------------------------------------------

// Sends a state for `p` every 50 ms for `steps` steps, moving x by `dx` each step (so the player
// is always "moving": changed state, non-zero velocity), pumping the rig. Returns the final x.
static float Walk(Rig& rig, Probe& p, float x, float dx, int steps, float y = 0.0f)
{
    for (int i = 0; i < steps; i++)
    {
        x += dx;
        p.s.SendState(Kin(x, y, 30.0f, 0.0f, dx * 20.0f));
        rig.Pump(50);
    }
    return x;
}

// Sends `a` at ax and `b` at bx every 50 ms until `pred` holds (or the budget ends).
static bool Hold(Rig& rig, Probe& a, float ax, Probe& b, float bx, const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = Clock::now();
    auto next = t0;
    while (MsSince(t0) < timeoutMs)
    {
        if (Clock::now() >= next)
        {
            a.s.SendState(Kin(ax, 0.0f, 30.0f));
            b.s.SendState(Kin(bx, 0.0f, 30.0f));
            next += std::chrono::milliseconds(50);
        }
        rig.Step();
        if (pred()) return true;
    }
    return pred();
}

static void StateTests()
{
    Rig rig;
    if (!rig.Start()) { Check("state: server starts", false); return; }
    Probe a(rig), b(rig), c(rig);
    a.Join("Alice");
    b.Join("Bob");
    c.Join("Carol"); // never sends state
    b.traceId = a.s.myId();
    // The server needs a receiver's own position to judge relevance, so Bob stands 10 m from where Alice will be.
    b.s.SendState(Kin(110.0f, 200.0f, 30.0f));

    const proto::Kinematics where = Kin(100.0f, 200.0f, 30.0f, 90.0f, 1.0f, 2.0f, 0.5f);
    const auto t0 = Clock::now();
    const bool seen = SendUntil(rig, a, where, [&] { return b.Seen(a.s.myId()) != nullptr; }, 3000);
    const int64_t ms = MsSince(t0);
    const proto::WorldEntry* e = b.Seen(a.s.myId());
    Check("state: B sees A's position, heading and velocity in WorldState within a second",
          seen && ms < 1000 && e && e->k.x == 100.0f && e->k.y == 200.0f && e->k.z == 30.0f && e->k.heading == 90.0f &&
              e->k.vx == 1.0f && e->k.vy == 2.0f && e->k.vz == 0.5f,
          std::to_string(ms) + " ms");

    // Alice is moving now, so the next stretch exercises the near rate; Bob keeps sending so A is seen both ways.
    const uint64_t w0 = b.worlds;
    float x = Walk(rig, a, 100.0f, 0.1f, 20, 200.0f);
    const uint64_t perSecond = b.worlds - w0;
    Check("state: a relevant, moving player is sent at the near rate, about tickHz (20) frames per second",
          perSecond >= 15 && perSecond <= 25, std::to_string(perSecond) + "/s");
    Check("state: a WorldState never contains the receiver's own entry (presence control: B does see A)",
          b.seenIds.count(a.s.myId()) == 1 && !b.seenIds.count(b.s.myId()) && !a.seenIds.count(a.s.myId()) &&
              !b.coarse.count(b.s.myId()) && !c.seenIds.count(c.s.myId()),
          "b saw " + std::to_string(b.seenIds.size()) + " ids");
    Check("state: a player that has sent no state is not in the world (no phantom at the origin)",
          !b.seenIds.count(c.s.myId()) && !a.seenIds.count(c.s.myId()) && !b.coarse.count(c.s.myId()) &&
              !a.coarse.count(c.s.myId()));

    // Idle: once Alice stands still for half a second she is sent at the idle rate (1 Hz), not every tick.
    rig.Pump(700);
    const uint64_t i0 = b.worlds;
    rig.Pump(2000);
    const uint64_t idleFrames = b.worlds - i0;
    Check("state: a player who stands still is sent at the idle rate (about 1 Hz), not every tick",
          idleFrames >= 1 && idleFrames <= 4, std::to_string(idleFrames) + " frames in 2 s");

    // A receiver that has sent no state yet has no position to measure relevance from: no WorldState
    // entries, but it still gets the coarse channel, so a spectator or a loading client sees the map.
    const bool blip = rig.Until([&] { return c.Blip(a.s.myId()) != nullptr; }, 3000);
    const proto::CoarseEntry* cb = c.Blip(a.s.myId());
    Check("state: a client that has sent no state gets no WorldState entries but does get the coarse blips (within 0.25 m)",
          blip && c.seenIds.empty() && cb && std::fabs(cb->x - x) <= 0.26f && std::fabs(cb->y - 200.0f) <= 0.26f,
          cb ? std::to_string(cb->x) + " vs " + std::to_string(x) : "no blip");

    // Sync plan W7: the shipped defaults (a Config as it comes, tickHz 30 and the 30/15/10 Hz rates; this file's
    // BaseConfig() pins the 20 Hz tick the older checks were written for). A moving near player is sent every tick,
    // a mid-range one (200 m) on every 2nd, and Welcome announces 30.
    {
        server::Config cfg = Rig::BaseConfig();
        cfg.tickHz = server::Config().tickHz;
        cfg.repl = server::Config().repl;
        Rig rig30;
        // The outer rig (a, b, c, ...) must keep being serviced for the ~3 s this block takes: left alone it drops them
        // (ENet peer timeout 400..1000 ms) and the invalid-packet checks further down then find 3 players, not 6.
        rig30.companions.push_back(&rig);
        if (!rig30.Start(cfg)) { Check("state: the 30 Hz server starts", false); return; }
        Probe m(rig30), n(rig30), f(rig30);
        m.Join("Mover");
        n.Join("Near");
        f.Join("Mid");
        n.s.SendState(Kin(110.0f, 0.0f, 30.0f));
        f.s.SendState(Kin(300.0f, 0.0f, 30.0f)); // 200 m from the mover
        m.s.SendState(Kin(100.0f, 0.0f, 30.0f));
        rig30.Pump(300);
        const uint64_t nearWorlds0 = n.worlds;
        const uint64_t midEntries0 = f.worlds;
        float mx = 100.0f;
        const auto r0 = Clock::now();
        for (int i = 0; i < 40; i++) // about 2 s, the mover walking at about 20 states/s
        {
            mx += 0.1f;
            m.s.SendState(Kin(mx, 0.0f, 30.0f, 0.0f, 2.0f));
            n.s.SendState(Kin(110.0f, 0.0f, 30.0f));
            f.s.SendState(Kin(300.0f, 0.0f, 30.0f));
            rig30.Pump(50);
        }
        // Per second of the wall clock that really passed (Pump(50) takes a little over 50 ms, so the loop is more than 2 s).
        const double secs = (double)MsSince(r0) / 1000.0;
        const uint64_t nearPerSec = (uint64_t)std::llround((double)(n.worlds - nearWorlds0) / secs),
                       midPerSec = (uint64_t)std::llround((double)(f.worlds - midEntries0) / secs);
        Check("state: at the default 30 Hz a moving near player arrives at about 30 frames per second (24..36), one at 200 m at about 15 (11..19), "
              "and Welcome says tickHz 30",
              m.s.tickHz() == 30 && nearPerSec >= 24 && nearPerSec <= 36 && midPerSec >= 11 && midPerSec <= 19,
              std::to_string(nearPerSec) + "/s near, " + std::to_string(midPerSec) + "/s mid, tickHz " + std::to_string(m.s.tickHz()));
    }

    // Stale sequence numbers. Raw sends control the seq exactly.
    Probe sa(rig), sb(rig);
    sa.Join("Sender");
    sb.Join("Watcher");
    sb.traceId = sa.s.myId();
    sb.s.SendState(Kin(0.0f, 0.0f, 30.0f)); // the watcher stands at the origin, the sender will be 10 m away
    rig.Pump(100);
    const uint32_t staleBefore = (uint32_t)rig.srv.stats().statesStale;
    sa.SendKin(100, Kin(10, 0, 30));
    rig.Until([&] { return sb.Seen(sa.s.myId()) && sb.Seen(sa.s.myId())->k.x == 10.0f; }, 2000);
    sa.SendKin(99, Kin(500, 0, 30));  // older
    sa.SendKin(100, Kin(600, 0, 30)); // equal: not newer
    const bool counted = rig.Until([&] { return rig.srv.stats().statesStale == staleBefore + 2; }, 1500);
    rig.Pump(1300); // long enough for at least one idle-rate frame to carry the sender's (unchanged) position
    bool neverRegressed = !sb.trace.empty();
    for (float tx : sb.trace)
        if (tx != 10.0f) neverRegressed = false;
    Check("stale: a lower or equal seq is dropped and counted, and the watcher never sees its position",
          counted && neverRegressed && sb.Seen(sa.s.myId()) && sb.Seen(sa.s.myId())->k.x == 10.0f,
          "trace size " + std::to_string(sb.trace.size()));
    Check("stale: the drop reason is in the server log", rig.LogHas("stale PlayerState seq 99"));
    sa.SendKin(101, Kin(20, 0, 30));
    Check("stale: the next higher seq is accepted again (and a sender that starts moving is not held back by the idle rate)",
          rig.Until([&] { return sb.Seen(sa.s.myId()) && sb.Seen(sa.s.myId())->k.x == 20.0f; }, 600));

    // Wrap-around: 5 is newer than 0xFFFFFFF0, and 0xFFFFFFF0 is then older.
    Probe wrap(rig);
    wrap.Join("Wrapper");
    rig.Pump(100);
    const uint16_t wid = wrap.s.myId();
    auto seenAt = [&](float xx) { return rig.Until([&] { return sb.Seen(wid) && sb.Seen(wid)->k.x == xx; }, 2000); };
    wrap.SendKin(0xFFFFFFF0u, Kin(1, 0, 30));
    const bool first = seenAt(1.0f);
    wrap.SendKin(5, Kin(2, 0, 30));
    const bool wrapped = seenAt(2.0f);
    const uint64_t staleNow = rig.srv.stats().statesStale;
    wrap.SendKin(0xFFFFFFF0u, Kin(3, 0, 30));
    const bool dropped = rig.Until([&] { return rig.srv.stats().statesStale == staleNow + 1; }, 1500);
    rig.Pump(1300);
    Check("stale: sequence numbers wrap around (5 is newer than 0xFFFFFFF0, the old value is then stale)",
          first && wrapped && dropped && sb.Seen(wid) && sb.Seen(wid)->k.x == 2.0f);

    // Invalid kinematics and malformed state packets are dropped, never applied, never fatal.
    Check("invalid: ClientSession refuses to send an out-of-box position itself, and a NaN cannot even be encoded",
          !a.s.SendState(Kin(1e6f, 0, 0)) && !a.s.SendState(Kin(std::nanf(""), 0, 0)) &&
              proto::Encode([] { proto::PlayerState ps; ps.k.x = std::nanf(""); return ps; }()).empty());
    const uint64_t invBefore = rig.srv.stats().statesInvalid, malBefore = rig.srv.stats().statesMalformed;
    const uint32_t nextSeq = a.s.lastSentSeq() + 1000; // above anything A has sent
    a.SendKin(nextSeq, Kin(1e6f, 0, 30)); // saturates at the quantiser box (16384 m): decodes, then fails the world box
    const bool inv = rig.Until([&] { return rig.srv.stats().statesInvalid == invBefore + 1; }, 1500);
    // Reliable on purpose (as in the ped refusals below): back-to-back unreliable sends may legally lose one
    // (194/196 in 2 of 3 local runs, 2026-11-02); this check is about the server's refusal, not about delivery.
    a.s.SendRaw(proto::kChanState, RawState(nextSeq + 1, 2000000, 0, 3000), true); // x = 20 km: outside the quantiser box
    a.s.SendRaw(proto::kChanState, proto::Bytes{(uint8_t)proto::Msg::PlayerState, 1, 2, 3}, true); // truncated
    const bool mal = rig.Until([&] { return rig.srv.stats().statesMalformed == malBefore + 2; }, 1500);
    rig.Pump(200);
    bool neverFar = true;
    for (const auto& kv : b.known)
        if (kv.second.k.x > 1000.0f) neverFar = false;
    Check("invalid: an out-of-box position is dropped and logged; a value outside the quantiser box and a truncated packet count as malformed",
          inv && mal && neverFar && rig.LogHas("outside the world box") && rig.LogHas("malformed PlayerState"),
          std::string("inv ") + (inv ? "1" : "0") + ", mal " + (mal ? "1" : "0") + ", neverFar " + (neverFar ? "1" : "0"));
    Check("invalid: bad state packets never disconnect the sender", a.Joined() && rig.srv.Players().size() == 6,
          std::string("joined ") + (a.Joined() ? "1" : "0") + ", players " + std::to_string(rig.srv.Players().size()));
}

// --- interest management over the wire ----------------------------------------------------------

static void InterestNetworkTests()
{
    Rig rig;
    if (!rig.Start()) { Check("interest: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    const uint16_t ia = a.s.myId(), ib = b.s.myId();
    b.traceId = ia;

    // Far apart: Bob at the origin, Alice 3 km away.
    const bool blip = Hold(rig, a, 3000.0f, b, 0.0f, [&] { return b.Blip(ia) != nullptr; }, 4000);
    rig.Pump(300);
    const proto::CoarseEntry* cb = b.Blip(ia);
    Check("interest: a player 3 km away is not replicated at full detail, but reaches the client as a coarse blip (within 0.25 m)",
          blip && cb && std::fabs(cb->x - 3000.0f) <= 0.26f && !b.seenIds.count(ia) && !rig.srv.IsRelevant(ib, ia) &&
              rig.srv.RelevantCount(ib) == 0,
          cb ? std::to_string(cb->x) : "no blip");

    // Into range: relevant, replicated, and the coarse channel stops for her.
    const bool near = Hold(rig, a, 300.0f, b, 0.0f, [&] { return b.Seen(ia) != nullptr; }, 3000);
    const proto::WorldEntry* en = b.Seen(ia);
    Check("interest: at 300 m she is relevant and replicated at full detail",
          near && en && en->k.x == 300.0f && rig.srv.IsRelevant(ib, ia) && rig.srv.RelevantCount(ib) == 1);
    Hold(rig, a, 300.0f, b, 0.0f, [] { return false; }, 500); // let a blip that was already on its way land
    const uint64_t blipsBefore = b.coarseCount[ia];
    Hold(rig, a, 300.0f, b, 0.0f, [] { return false; }, 2200);
    Check("interest: once relevant she is dropped from the coarse channel (no blip of her arrives in 2.2 s, i.e. two full cycles)",
          blipsBefore > 0 && b.coarseCount[ia] == blipsBefore, std::to_string(b.coarseCount[ia] - blipsBefore) + " new blips");

    // Hysteresis: between the enter (400 m) and leave (450 m) radius she stays; past the leave radius she goes.
    Hold(rig, a, 430.0f, b, 0.0f, [] { return false; }, 1500);
    const bool stillIn = rig.srv.IsRelevant(ib, ia) && b.Seen(ia) && b.leftIds.empty();
    const bool gone = Hold(rig, a, 470.0f, b, 0.0f, [&] { return b.leftIds.count(ia) == 1; }, 3000);
    Check("interest: she stays relevant at 430 m (hysteresis), and is told to leave past 450 m: client gets the leave id, the server drops the pair",
          stillIn && gone && !rig.srv.IsRelevant(ib, ia) && b.Seen(ia) == nullptr);
    const bool blipAgain = Hold(rig, a, 470.0f, b, 0.0f, [&] { return b.Blip(ia) && std::fabs(b.Blip(ia)->x - 470.0f) <= 0.26f; }, 3000);
    Check("interest: after leaving, she is back on the coarse channel at her new position", blipAgain);

    // Always-relevant.
    std::string err;
    Hold(rig, a, 3000.0f, b, 0.0f, [] { return false; }, 1000);
    const bool apiOk = rig.srv.SetAlwaysRelevant(ia, ib, true, &err);
    const bool forced = Hold(rig, a, 3000.0f, b, 0.0f, [&] { return b.Seen(ia) && b.Seen(ia)->k.x == 3000.0f; }, 3000);
    const bool relevantNow = rig.srv.IsRelevant(ib, ia) && rig.srv.AlwaysRelevant(ia, ib);
    const uint64_t leftBefore = b.leftIds.count(ia);
    Hold(rig, a, 3000.0f, b, 0.0f, [] { return false; }, 1500);
    Check("interest: SetAlwaysRelevant makes a player 3 km away replicated at full detail, and the leave radius never drops her",
          apiOk && forced && relevantNow && rig.srv.IsRelevant(ib, ia) && b.leftIds.count(ia) == leftBefore, err);
    rig.srv.SetAlwaysRelevant(ia, ib, false);
    const bool released = Hold(rig, a, 3000.0f, b, 0.0f, [&] { return !rig.srv.IsRelevant(ib, ia) && b.Seen(ia) == nullptr; }, 3000);
    Check("interest: turning the override off returns the pair to the normal radii (she leaves, the coarse channel takes over)",
          released && !rig.srv.AlwaysRelevant(ia, ib));
    std::string e1, e2, e3;
    Check("interest: SetAlwaysRelevant refuses unknown players and subject == receiver, and kServerId means everybody (also late joiners)",
          !rig.srv.SetAlwaysRelevant(ia, ia, true, &e1) && !e1.empty() && !rig.srv.SetAlwaysRelevant(99, ib, true, &e2) &&
              !rig.srv.SetAlwaysRelevant(ia, 99, true, &e3) && rig.srv.SetAlwaysRelevant(ia, proto::kServerId, true) &&
              rig.srv.AlwaysRelevant(ia, proto::kServerId) && rig.srv.IsRelevant(ib, ia));
    Probe late(rig);
    late.Join("Latecomer");
    late.s.SendState(Kin(-5000.0f, 0.0f, 30.0f));
    const bool lateSees = rig.Until([&] { return late.Seen(ia) != nullptr; }, 3000);
    Check("interest: an override for everybody reaches a player who joins afterwards", lateSees);
    rig.srv.SetAlwaysRelevant(ia, proto::kServerId, false);

    // Delta frames over a real socket: both walking next to each other for two seconds.
    Probe d1(rig), d2(rig);
    d1.Join("Dee");
    d2.Join("Dum");
    float x1 = 0.0f, x2 = 20.0f;
    for (int i = 0; i < 40; i++)
    {
        x1 += 0.5f;
        x2 += 0.5f;
        d1.s.SendState(Kin(x1, 100.0f, 30.0f, 10.0f, 10.0f));
        d2.s.SendState(Kin(x2, 100.0f, 30.0f, 10.0f, 10.0f));
        rig.Pump(50);
    }
    const auto c1 = d1.s.counters();
    const auto c2 = d2.s.counters();
    Check("interest: over a real socket, delta frames are used (most entries) and no client rejected a frame",
          c1.worldDeltaEntries > c1.worldAbsoluteEntries && c2.worldDeltaEntries > c2.worldAbsoluteEntries &&
              c1.worldRejected == 0 && c2.worldRejected == 0 && d1.s.malformed() == 0 && d2.s.malformed() == 0 &&
              d1.s.worldAck().tick != 0,
          "d1 delta " + std::to_string(c1.worldDeltaEntries) + " abs " + std::to_string(c1.worldAbsoluteEntries) + " rejected " +
              std::to_string(c1.worldRejected));
}

// --- loss ---------------------------------------------------------------------------------

static void LossTest()
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.dropStatePercent = 50; // the server throws away half of the incoming PlayerState (acks included)
    cfg.dropSeed = 7;
    if (!rig.Start(cfg)) { Check("loss: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Walker");
    b.Join("Watcher", [](net::SessionOptions& o) {
        o.dropWorldPercent = 50; // and B loses half of the WorldState it is sent
        o.dropSeed = 11;
    });
    b.traceId = a.s.myId();
    rig.Pump(100);

    // A walks x = 1..60 at 20 Hz, then holds at 60 until B sees it. B stands at the origin and keeps
    // sending (its own state, and with it the acknowledgements the delta frames depend on).
    for (int step = 1; step <= 60; step++)
    {
        a.s.SendState(Kin((float)step, 0, 30, 0, 20.0f));
        b.s.SendState(Kin(0, 0, 30));
        rig.Pump(50);
    }
    const auto t0 = Clock::now();
    bool converged = false;
    for (int i = 0; i < 120 && !converged; i++)
    {
        a.s.SendState(Kin(60, 0, 30));
        b.s.SendState(Kin(0, 0, 30));
        rig.Pump(50);
        converged = b.Seen(a.s.myId()) && b.Seen(a.s.myId())->k.x == 60.0f;
    }
    const int64_t ms = MsSince(t0);

    const server::Stats st = rig.srv.stats();
    const uint64_t total = st.statesSimDropped + st.statesAccepted;
    const double ratio = total ? (double)st.statesSimDropped / (double)total : 0;
    Check("loss: the server really dropped about half of the PlayerState it received (the knob is binding)",
          total >= 60 && ratio > 0.3 && ratio < 0.7, std::to_string(st.statesSimDropped) + " dropped of " + std::to_string(total));
    Check("loss: the client really dropped about half of the WorldState it was sent",
          b.s.worldReceived() >= 40 && b.s.worldDropped() > b.s.worldReceived() / 4 && b.s.worldDropped() < b.s.worldReceived() * 3 / 4,
          std::to_string(b.s.worldDropped()) + " dropped of " + std::to_string(b.s.worldReceived()));
    Check("loss: B still converges on A's final position", converged, std::to_string(ms) + " ms");
    bool monotonic = true;
    std::set<float> distinct(b.trace.begin(), b.trace.end());
    for (size_t i = 1; i < b.trace.size(); i++)
        if (b.trace[i] < b.trace[i - 1]) monotonic = false;
    Check("loss: B saw A's walk with gaps (fewer positions than steps) but never going backwards",
          monotonic && distinct.size() > 5 && distinct.size() < 60, std::to_string(distinct.size()) + " distinct positions");
    const auto bc = b.s.counters();
    Check("loss: with half the frames and half the acknowledgements lost, every frame B decoded resolved its baseline (0 rejected) and deltas were in use",
          bc.worldRejected == 0 && b.s.malformed() == 0 && bc.worldDeltaEntries > 10,
          "rejected " + std::to_string(bc.worldRejected) + ", delta entries " + std::to_string(bc.worldDeltaEntries) + ", absolute " +
              std::to_string(bc.worldAbsoluteEntries));
}

// --- chat -----------------------------------------------------------------------------------

static void ChatTests()
{
    Rig rig;
    if (!rig.Start()) { Check("chat: server starts", false); return; }
    Probe a(rig), b(rig), c(rig);
    a.Join("Alice");
    b.Join("Bob");
    c.Join("Carol");
    rig.Pump(100);
    const uint16_t aId = a.s.myId(), bId = b.s.myId();

    Check("chat: ClientSession refuses empty, over-cap and control-character text itself",
          !a.s.SendChat("") && !a.s.SendChat(std::string(proto::kMaxChat + 1, 'a')) && !a.s.SendChat("a\nb") && !a.s.SendChat(Raw({'a', 0x01})));

    // A's three messages go out back to back (ordering within a channel), then the
    // server's notice, then B's reply.
    a.s.SendChat("one");
    a.s.SendChat("two");
    a.s.SendChat("three");
    const auto all3 = [&](size_t n) { return a.ChatLog().size() >= n && b.ChatLog().size() >= n && c.ChatLog().size() >= n; };
    rig.Until([&] { return all3(3); }, 2000);
    std::string out;
    rig.srv.Execute("say notice from the server", out);
    rig.Until([&] { return all3(4); }, 2000);
    b.s.SendChat("reply");
    rig.Until([&] { return all3(5); }, 2000);
    rig.Pump(300); // anything delivered twice would land by now

    const std::vector<std::pair<uint16_t, std::string>> want = {
        {aId, "one"}, {aId, "two"}, {aId, "three"}, {proto::kServerId, "notice from the server"}, {bId, "reply"}};
    Check("chat: every client (the sender too) gets each message exactly once, in order",
          a.ChatLog() == want && b.ChatLog() == want && c.ChatLog() == want,
          std::to_string(a.ChatLog().size()) + "/" + std::to_string(b.ChatLog().size()) + "/" + std::to_string(c.ChatLog().size()) +
              " of " + std::to_string(want.size()));
    Check("chat: the server counted one delivery per message", rig.srv.stats().chatDelivered == 5,
          std::to_string(rig.srv.stats().chatDelivered));

    // A control-character message that bypasses the client check: dropped, sender told, others not.
    const size_t bBefore = b.ChatLog().size();
    a.SendReliable(ChatBytes(Raw({'b', 'a', 'd', 0x01, 't', 'e', 'x', 't'})));
    const bool counted = rig.Until([&] { return rig.srv.stats().chatRejected == 1; }, 1500);
    rig.Until([&] { return a.ChatLog().size() == want.size() + 1; }, 1500);
    rig.Pump(200);
    const auto aLog = a.ChatLog();
    Check("chat: a control-character message is rejected, only the sender gets a server notice, nobody else hears it",
          counted && aLog.size() == want.size() + 1 && aLog.back().first == proto::kServerId &&
              aLog.back().second.find("not sent") != std::string::npos && b.ChatLog().size() == bBefore && a.Joined() &&
              rig.LogHas("chat rejected"),
          std::to_string(aLog.size()));
}

// --- shutdown and a dead server ------------------------------------------------------------------

static void ShutdownTests()
{
    {
        Rig rig;
        if (!rig.Start()) { Check("shutdown: server starts", false); return; }
        Probe a(rig);
        a.Join("Alice");
        // Stop() waits for the clients to acknowledge, so it needs the client side to
        // keep running: stop the server on its own thread, pump only the client here.
        std::thread stopper([&] { rig.srv.Stop(); });
        const auto t0 = Clock::now();
        while (!a.Closed() && MsSince(t0) < 4000)
        {
            a.s.Poll(0);
            a.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stopper.join();
        bool notice = false;
        for (const auto& m : a.ChatLog())
            if (m.first == proto::kServerId && m.second.find("shutting down") != std::string::npos) notice = true;
        Check("shutdown: Stop() tells clients (notice, then Disconnected Shutdown) and closes the socket",
              a.Closed() && a.DisconnectedWith(net::DisconnectReason::Shutdown) && notice && !rig.srv.running(), a.Describe());
    }
    {
        // A port nothing listens on any more: the client must give up inside its budget.
        Rig rig;
        if (!rig.Start()) { Check("dead server: server starts", false); return; }
        const uint16_t port = rig.srv.port();
        rig.srv.Stop();
        Probe d(rig);
        d.Connect("Lonely", [port](net::SessionOptions& o) {
            o.port = port;
            o.connectTimeoutMs = 800;
        });
        const auto t0 = Clock::now();
        const bool closed = d.WaitClosed(4000);
        const int64_t ms = MsSince(t0);
        Check("dead server: connecting to nothing ends in Disconnected(ConnectFailed) within the budget, never hangs",
              closed && d.DisconnectedWith(net::DisconnectReason::ConnectFailed) && ms < 2500, std::to_string(ms) + " ms, " + d.Describe());
    }
}

// --- metrics (`stats`) and the per-client byte budget over the real server --------------------

static double JsonNum(const std::string& json, const char* key, bool* found = nullptr)
{
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (found) *found = at != std::string::npos;
    return at == std::string::npos ? 0.0 : std::strtod(json.c_str() + at + needle.size(), nullptr);
}

// 30 players inside 100 m of each other, all moving, so every client is owed 29 updates per tick.
// Returns the WorldState bytes player 0 received in `seconds` seconds (after a 1 s warm-up).
static uint64_t CrowdBytes(uint32_t budget, int seconds, double* deferredPerSec)
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.maxPlayers = 32;
    cfg.repl.budgetBytesPerSec = budget;
    if (!rig.Start(cfg)) return 0;
    std::vector<std::unique_ptr<Probe>> ps;
    for (int i = 0; i < 30; i++)
    {
        ps.push_back(std::make_unique<Probe>(rig));
        if (!ps.back()->Join(("P" + std::to_string(i)).c_str())) return 0;
    }
    auto round = [&](int step) {
        for (int i = 0; i < 30; i++)
            ps[i]->s.SendState(Kin(3.0f * (float)i + 0.5f * (float)step, 10.0f * std::sin(0.3f * (float)step + (float)i), 30.0f,
                                   10.0f * (float)(step % 30), 10.0f, 0, 0));
        rig.Pump(50);
    };
    int step = 0;
    for (; step < 20; step++) round(step);
    const uint64_t b0 = ps[0]->s.counters().worldBytes;
    const uint64_t d0 = rig.srv.metrics().budgetDeferred;
    const auto t0 = Clock::now();
    for (const int end = step + seconds * 20; step < end; step++) round(step);
    const double secs = (double)MsSince(t0) / 1000.0;
    if (deferredPerSec) *deferredPerSec = (double)(rig.srv.metrics().budgetDeferred - d0) / secs;
    return ps[0]->s.counters().worldBytes - b0;
}

static void MetricsTests()
{
    {
        server::LogHistogram h;
        for (uint64_t v = 1; v <= 1000; v++) h.Add(v);
        const server::LogHistogram snap = h;
        for (int i = 0; i < 100; i++) h.Add(5000);
        const double p50 = h.Quantile(0.5), p99 = h.Quantile(0.99), winP50 = h.Quantile(0.5, &snap);
        const server::LogHistogram empty;
        const uint64_t peak = h.max();
        h.ResetMax();
        Check("metrics: LogHistogram keeps exact count, sum and max, a mean of 909.5, quantiles within 10%, windows by snapshot difference, 0 when empty",
              h.count() == 1100 && h.sum() == 1000500 && peak == 5000 && h.max() == 0 && std::fabs(h.Mean() - 909.545) < 0.01 &&
                  std::fabs(p50 - 550.0) < 55.0 && std::fabs(p99 - 5000.0) < 500.0 && h.CountSince(&snap) == 100 && h.Mean(&snap) == 5000.0 &&
                  std::fabs(winP50 - 5000.0) < 500.0 && empty.Mean() == 0.0 && empty.Quantile(0.5) == 0.0,
              "p50 " + std::to_string(p50) + " p99 " + std::to_string(p99));
    }
    {
        Rig rig;
        if (!rig.Start()) { Check("metrics: server starts", false); return; }
        Probe a(rig), b(rig);
        a.Join("Ann");
        b.Join("Ben");
        for (int i = 0; i < 20; i++)
        {
            a.SendKin(i + 1, Kin(10.0f + (float)i, 0, 30, 0, 5.0f));
            b.SendKin(i + 1, Kin(20.0f + (float)i, 0, 30, 0, 5.0f));
            rig.Pump(50);
        }
        std::string json;
        const server::CommandResult r1 = rig.srv.Execute("stats json", json);
        bool f1 = false, f2 = false, f3 = false, f4 = false, f5 = false;
        const double players = JsonNum(json, "players", &f1), ticks = JsonNum(json, "ticks", &f2),
                     wpk = JsonNum(json, "world_packets_per_s", &f3), win = JsonNum(json, "window_s", &f4),
                     pairs = JsonNum(json, "relevant_pairs", &f5);
        Check("stats: `stats json` is one JSON object: 2 players, 10+ ticks, world packets flowing, a window over 0.5 s, 2 relevant pairs",
              r1 == server::CommandResult::Ok && json.size() > 2 && json.front() == '{' && json.back() == '}' && f1 && f2 && f3 && f4 && f5 &&
                  players == 2 && ticks >= 10 && wpk > 5 && win > 0.5 && pairs == 2,
              json.substr(0, 200));
        {
            // Sync plan N6 counters: the report carries the per pair-tick outcomes (2 relevant pairs x 20 ticks/s, so they add
            // up to about 40 a second), the wake-ups, the sample accounting and the tick lateness p95.
            const char* keys[] = {"pairs_sent_per_s", "pairs_idle_suppressed_per_s", "pairs_rate_not_due_per_s",
                                  "pairs_budget_deferred_per_s", "pairs_packet_cap_deferred_per_s", "pairs_no_state_per_s"};
            bool all = true;
            double outcomes = 0;
            for (const char* k : keys)
            {
                bool f = false;
                outcomes += JsonNum(json, k, &f);
                all = all && f;
            }
            bool fw = false, fr = false, fn = false, ft = false, fk = false, fx = false;
            const double recv = JsonNum(json, "samples_received_per_s", &fr), fwd = JsonNum(json, "samples_forwarded_per_s", &fw);
            JsonNum(json, "samples_never_forwarded_per_s", &fn);
            JsonNum(json, "samples_forwarded_twice_per_s", &ft);
            JsonNum(json, "wakes_per_s", &fk);
            JsonNum(json, "tick_late_us_p95", &fx);
            bool fz = false;
            JsonNum(json, "noise_wakes_held_per_s", &fz);
            Check("stats: the sync counters are in the report: six pair-tick outcomes that add up to about 40 a second (2 pairs x 20 ticks), samples received and forwarded, never / twice, wakes, noise wakes, tick_late_us_p95",
                  all && fr && fw && fn && ft && fk && fz && fx && outcomes > 15 && outcomes < 55 && recv > 5 && fwd > 5,
                  "outcomes " + std::to_string(outcomes) + "/s, received " + std::to_string(recv) + "/s, forwarded " + std::to_string(fwd) + "/s");
        }
        rig.Pump(500);
        std::string second, text, bad;
        rig.srv.Execute("stats json", second);
        const double ticks2 = JsonNum(second, "ticks");
        const server::CommandResult r3 = rig.srv.Execute("stats", text);
        const server::CommandResult r4 = rig.srv.Execute("stats bogus", bad);
        Check("stats: each report covers the time since the previous one (about 10 ticks in 0.5 s); the text form lists the same keys; a bad argument is an error with usage",
              ticks2 >= 8 && ticks2 <= 12 && r3 == server::CommandResult::Ok && text.rfind("stats\n  window_s ", 0) == 0 &&
                  text.find("\n  tick_work_us_p99 ") != std::string::npos && text.find("\n  relevant_pairs ") != std::string::npos &&
                  r4 == server::CommandResult::Error && bad.find("usage") != std::string::npos,
              std::to_string(ticks2) + " ticks in the second window");
    }
    {
        double deferredLow = 0, deferredHigh = 0;
        const uint64_t low = CrowdBytes(1000, 4, &deferredLow);
        const uint64_t high = CrowdBytes(24000, 4, &deferredHigh);
        Check("budget: over a real socket a 1 kB/s clientBudgetBytesPerSec holds one client to about 1 kB/s (<= 6 kB in 4 s: 4 kB plus the up to 1.2 kB burst the bucket may hold) while the default gives it 4x or more; due updates were deferred",
              low > 0 && low <= 6000 && high >= 4 * low && deferredLow > 100,
              std::to_string(low) + " B at 1 kB/s, " + std::to_string(high) + " B at 24 kB/s, deferred " + std::to_string((long long)deferredLow) + "/s");
    }
}

// The v3 vehicle messages (VehicleWorld on the state channel, VehicleCmd on the reliable channel):
// round trips, the exact wire layout, the generic wrong-type / truncation / trailing checks, the
// decode caps, the ascending-id and field-range strictness, and the VehicleCmd op range. Kept out
// of the ten-sample machinery above so those byte-exact checks stay untouched (as the resource
// messages are).
// v4 vehicle, combat and emote messages: one sample of each (fields chosen to survive quantisation
// exactly), the byte layouts, and the refusals.
static proto::VehicleState SampleVehicle()
{
    proto::VehicleState v;
    v.id = 5;
    v.model = 0x0A0B0C0Du;
    v.k = Kin(1.0f, 2.0f, 3.0f, 0.0f, 5.0f, 6.0f, 7.0f); // 100/200/300 cm, 160/192/224 x 1/32 m/s
    v.rot = proto::Quat();                                // identity: exact on the wire
    v.avx = 0.5f;                                         // 8 x 1/16 rad/s
    v.avy = -1.0f;                                        // -16
    v.steer = -1.0f;                                      // -127
    v.flags = proto::kVehLocked | proto::kVehLights;
    v.bodyHealth = 900;
    v.engineHealth = 800;
    v.fuelPermille = 700;
    v.colour1 = 12;
    v.colour2 = 34;
    v.seats[0] = 3;
    v.seats[2] = 5;
    v.ageMs = 12;                                            // v5
    v.ctrl = 3;
    v.ownerId = 5;
    v.flags2 = proto::kVeh2Headlights | proto::kVeh2Interior | proto::kVeh2IndicatorR; // 0x15
    v.doors = proto::kDoorFrontLeft | proto::kDoorTrunk;                              // 0x21
    v.tyres = 0x11;                                                                    // v6: wheel slots 0 and 4 burst
    v.tyresRim = 0x10;                                                                 // ... slot 4 off its rim
    v.windows = 0x30;                                                                  // ... windows 4 and 5 smashed
    return v;
}

// A v5 motion-only entry: the motion of SampleVehicle() under id 11 with its own age and flags.
static proto::VehicleState SampleMotionOnly()
{
    proto::VehicleState m;
    m.id = 11;
    m.motionOnly = true;
    m.k = Kin(1.0f, 2.0f, 3.0f, 0.0f, 5.0f, 6.0f, 7.0f);
    m.avx = 0.5f;
    m.avy = -1.0f;
    m.steer = -1.0f;
    m.flags = proto::kVehEngine;
    m.ageMs = 30;
    return m;
}

struct VSample { const char* name; proto::Bytes bytes; bool (*dec)(const uint8_t*, size_t); };

static std::vector<VSample> V4Samples()
{
    std::vector<VSample> s;
    proto::VehicleWorld world;
    world.tick = 0x01020304u;
    world.removed = {7};
    world.present = {5, 9, 11};
    world.vehicles = {SampleVehicle(), SampleMotionOnly()};
    s.push_back({"VehicleWorld", proto::Encode(world), &DecodeAs<proto::VehicleWorld>});

    proto::VehicleCmd lock;
    lock.vehId = 5;
    lock.op = proto::VehicleOp::SetLocked;
    lock.arg = 1;
    s.push_back({"VehicleCmd", proto::Encode(lock), &DecodeAs<proto::VehicleCmd>});

    proto::VehicleCmd reg;
    reg.op = proto::VehicleOp::Register;
    reg.arg = 0xB779A091u;
    reg.x = 1.0f;
    reg.y = 2.0f;
    reg.z = 3.0f;
    reg.colour1 = 4;
    reg.colour2 = 5;
    reg.seat = 1;
    s.push_back({"VehicleCmd/Register", proto::Encode(reg), &DecodeAs<proto::VehicleCmd>});

    proto::VehicleUpdate up;
    up.seq = 0x11223344u;
    up.sampleMs = 0x5566;                                    // v5
    up.flags2 = proto::kVeh2Headlights | proto::kVeh2IndicatorL; // 0x09
    up.doors = proto::kDoorFrontRight | proto::kDoorHood;        // 0x12
    up.tyres = 0x06;                                             // v6: slots 1 and 2 burst
    up.tyresRim = 0x02;                                          // ... slot 1 off its rim
    up.windows = 0x01;                                           // ... window 0 smashed
    up.vehId = 5;
    up.k = Kin(1.0f, 2.0f, 3.0f, 0.0f, 5.0f, 6.0f, 7.0f);
    up.avz = 0.25f;
    up.steer = 1.0f;
    up.flags = proto::kVehEngine | proto::kVehHorn;
    up.bodyHealth = 999;
    up.engineHealth = 1000;
    s.push_back({"VehicleUpdate", proto::Encode(up), &DecodeAs<proto::VehicleUpdate>});

    proto::VehicleResult res;
    res.op = proto::VehicleOp::Enter;
    res.code = proto::VehicleResultCode::SeatTaken;
    res.vehId = 5;
    s.push_back({"VehicleResult", proto::Encode(res), &DecodeAs<proto::VehicleResult>});

    proto::Hit hit;
    hit.kind = proto::HitKind::Player;
    hit.targetId = 2;
    hit.weapon = 0x1B06D571u;
    hit.damage = 34;
    hit.bone = 31086;
    hit.shotSeq = 9;
    s.push_back({"Hit", proto::Encode(hit), &DecodeAs<proto::Hit>});

    proto::Damage dmg;
    dmg.victimId = 2;
    dmg.attackerId = 1;
    dmg.weapon = 0x1B06D571u;
    dmg.damage = 34;
    dmg.health = 66;
    dmg.armor = 0;
    dmg.killed = false;
    dmg.bone = 31086;                                         // v5: SKEL_Head
    dmg.flags = proto::kDmgHeadshot | proto::kDmgArmour;      // 0x09
    s.push_back({"Damage", proto::Encode(dmg), &DecodeAs<proto::Damage>});

    proto::Killed kill;
    kill.victimId = 2;
    kill.killerId = proto::kServerId;
    kill.weapon = 0;
    kill.flags = proto::kKilledHeadshot | proto::kKilledMelee; // v5: 0x03
    s.push_back({"Killed", proto::Encode(kill), &DecodeAs<proto::Killed>});

    s.push_back({"Respawn", proto::Encode(proto::Respawn()), &DecodeAs<proto::Respawn>});

    proto::RespawnOk rok;
    rok.x = 195.25f;
    rok.y = -933.75f;
    rok.z = 30.5f;
    rok.heading = 140.0f;
    rok.health = 100;
    rok.armor = 0;
    s.push_back({"RespawnOk", proto::Encode(rok), &DecodeAs<proto::RespawnOk>});

    proto::PlayAnim pa;
    pa.anim.dict = "amb@world_human_smoking@male@male_a@base";
    pa.anim.name = "base";
    pa.anim.flags = 49;
    s.push_back({"PlayAnim", proto::Encode(pa), &DecodeAs<proto::PlayAnim>});

    proto::PlayScenario ps;
    ps.name = "WORLD_HUMAN_SMOKING";
    s.push_back({"PlayScenario", proto::Encode(ps), &DecodeAs<proto::PlayScenario>});

    s.push_back({"StopAnim", proto::Encode(proto::StopAnim()), &DecodeAs<proto::StopAnim>});

    proto::PeerAnim peer;
    peer.id = 4;
    peer.anim = pa.anim;
    peer.anim.durationMs = 2500;
    peer.anim.playbackRate = 150;
    s.push_back({"PeerAnim", proto::Encode(peer), &DecodeAs<proto::PeerAnim>});

    proto::PeerScenario psc;
    psc.id = 4;
    psc.name = "WORLD_HUMAN_SMOKING";
    s.push_back({"PeerScenario", proto::Encode(psc), &DecodeAs<proto::PeerScenario>});

    proto::PeerAnimStop stop;
    stop.id = 4;
    s.push_back({"PeerAnimStop", proto::Encode(stop), &DecodeAs<proto::PeerAnimStop>});
    return s;
}

template <class T>
static bool RoundTrips(const proto::Bytes& b, T& out)
{
    return proto::Decode(b.data(), b.size(), out) && proto::Encode(out) == b;
}

static void VehicleCodecTests()
{
    const std::vector<VSample> vs = V4Samples();

    // Round trip: every v4 message decodes and re-encodes to the same bytes; the fields survive.
    {
        std::string bad;
        for (const VSample& s : vs)
            if (s.bytes.empty() || !s.dec(s.bytes.data(), s.bytes.size())) bad += std::string("[") + s.name + "]";
        proto::VehicleWorld w;
        proto::VehicleCmd lock, reg;
        proto::VehicleUpdate up;
        proto::Hit hit;
        proto::Damage dmg;
        proto::RespawnOk rok;
        proto::PlayAnim pa;
        proto::PeerAnim peer;
        const bool rt = RoundTrips(vs[0].bytes, w) && RoundTrips(vs[1].bytes, lock) && RoundTrips(vs[2].bytes, reg) &&
                        RoundTrips(vs[3].bytes, up) && RoundTrips(vs[5].bytes, hit) && RoundTrips(vs[6].bytes, dmg) &&
                        RoundTrips(vs[9].bytes, rok) && RoundTrips(vs[10].bytes, pa) && RoundTrips(vs[13].bytes, peer);
        const proto::VehicleState* v = w.vehicles.empty() ? nullptr : &w.vehicles[0];
        const bool fields =
            v && w.tick == 0x01020304u && w.removed == std::vector<uint16_t>{7} && w.present == std::vector<uint16_t>{5, 9, 11} &&
            w.vehicles.size() == 2 && !v->motionOnly && v->ageMs == 12 && v->ctrl == 3 && v->ownerId == 5 &&
            v->flags2 == (proto::kVeh2Headlights | proto::kVeh2Interior | proto::kVeh2IndicatorR) &&
            v->doors == (proto::kDoorFrontLeft | proto::kDoorTrunk) && v->tyres == 0x11 && v->tyresRim == 0x10 && v->windows == 0x30 &&
            w.vehicles[1].tyres == 0 && w.vehicles[1].windows == 0 && up.tyres == 0x06 && up.tyresRim == 0x02 && up.windows == 0x01 &&
            w.vehicles[1].motionOnly && w.vehicles[1].id == 11 &&
            w.vehicles[1].ageMs == 30 && w.vehicles[1].flags == proto::kVehEngine && w.vehicles[1].k.vz == 7.0f &&
            w.vehicles[1].steer == -1.0f && w.vehicles[1].model == 0 && w.vehicles[1].ctrl == proto::kNoPlayer &&
            up.sampleMs == 0x5566 && up.flags2 == (proto::kVeh2Headlights | proto::kVeh2IndicatorL) &&
            up.doors == (proto::kDoorFrontRight | proto::kDoorHood) && dmg.bone == 31086 &&
            dmg.flags == (proto::kDmgHeadshot | proto::kDmgArmour) &&
            v->id == 5 && v->model == 0x0A0B0C0Du && v->k.x == 1.0f && v->k.y == 2.0f && v->k.z == 3.0f &&
            v->k.vx == 5.0f && v->k.vz == 7.0f && v->k.heading == 0.0f && v->rot.w == 1.0f && v->rot.z == 0.0f && v->avx == 0.5f &&
            v->avy == -1.0f && v->steer == -1.0f && v->flags == (proto::kVehLocked | proto::kVehLights) && v->bodyHealth == 900 &&
            v->engineHealth == 800 && v->fuelPermille == 700 && v->colour1 == 12 && v->colour2 == 34 && v->seats[0] == 3 &&
            v->seats[1] == proto::kNoPlayer && v->seats[2] == 5 && reg.op == proto::VehicleOp::Register && reg.arg == 0xB779A091u &&
            reg.x == 1.0f && reg.z == 3.0f && reg.seat == 1 && reg.colour2 == 5 && up.seq == 0x11223344u && up.vehId == 5 &&
            up.avz == 0.25f && up.steer == 1.0f && up.flags == (proto::kVehEngine | proto::kVehHorn) && up.bodyHealth == 999 &&
            hit.bone == 31086 && hit.shotSeq == 9 && hit.damage == 34 && dmg.health == 66 && !dmg.killed && rok.heading == 140.0f &&
            pa.anim.dict == "amb@world_human_smoking@male@male_a@base" && pa.anim.durationMs == -1 && pa.anim.blendOut == -8.0f &&
            peer.id == 4 && peer.anim.durationMs == 2500 && peer.anim.playbackRate == 150;
        Check("codec v5: all 16 vehicle, combat and emote samples decode and re-encode byte for byte; every field survives",
              vs.size() == 16 && bad.empty() && rt && fields, bad);
    }

    // The wire layouts, byte by byte (little-endian).
    {
        const proto::Bytes wantWorld = {
            0x0E,                                           // type 14
            0x04, 0x03, 0x02, 0x01,                         // tick
            0x01, 0x07,                                     // removed: u8 count 1, u8 id 7
            0x03, 0x00,                                     // present: u16 count 3
            0x05, 0x01,                                     // id 5, header 1 = full state follows
            0x0D, 0x0C, 0x0B, 0x0A,                         // model
            0x64, 0x00, 0x00, 0x32, 0x00, 0xC0, 0x12, 0x00, // u64 position: 100 | 200 << 22 | 300 << 44 (cm)
            0xA0, 0x00, 0xC0, 0x00, 0xE0, 0x00,             // velocity 160, 192, 224 (1/32 m/s)
            0xFF, 0xFD, 0xF7, 0xDF,                         // rotation: identity = w dropped (3 << 30), x, y, z = 511
            0x08, 0xF0, 0x00,                               // angular velocity 8, -16, 0 (1/16 rad/s)
            0x81,                                           // steering -127
            0x03,                                           // flags locked | lights
            0x84, 0x03, 0x20, 0x03, 0xBC, 0x02,             // body 900, engine 800, fuel 700
            0x0C, 0x22,                                     // colours 12, 34
            0x0C,                                           // v5: ageMs 12
            0x03, 0x00, 0x05, 0x00,                         // v5: ctrl player 3, ownerId player 5
            0x15, 0x21,                                     // v5: flags2 headlights | interior | indicator R, doors FL | trunk
            0x11, 0x10, 0x30,                               // v6: tyres slots 0 + 4 burst, rim slot 4, windows 4 + 5 smashed
            0xFA, 0x00, 0x00,                               // v7 (R3 T4): damage block: petrol 250 (full tank), no broken parts, no dents
            0x05, 0x03, 0x00, 0x05, 0x00,                   // seat mask 0b101: seat 0 = player 3, seat 2 = player 5
            0x09, 0x00,                                     // id 9, header 0 = present, no full state
            0x0B, 0x02,                                     // v5: id 11, header 2 = motion-only entry follows
            0x1E,                                           // ageMs 30
            0x64, 0x00, 0x00, 0x32, 0x00, 0xC0, 0x12, 0x00, // position
            0xA0, 0x00, 0xC0, 0x00, 0xE0, 0x00,             // velocity
            0xFF, 0xFD, 0xF7, 0xDF, 0x08, 0xF0, 0x00, 0x81, // rotation, angular velocity, steering -127
            0x04};                                          // flags engine
        const proto::Bytes wantLock = {0x0F, 0x05, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00}; // type, vehId 5, op 5, arg 1
        const proto::Bytes wantReg = {0x0F, 0x00, 0x00, 0x08, 0x91, 0xA0, 0x79, 0xB7,     // type, vehId 0, op 8, model
                                      0x64, 0x00, 0x00, 0x32, 0x00, 0xC0, 0x12, 0x00,     // position
                                      0xFF, 0xFD, 0xF7, 0xDF, 0x04, 0x05, 0x01};           // rotation, colours, seat
        const proto::Bytes wantUp = {0x12, 0x44, 0x33, 0x22, 0x11, 0x05,                   // type 18, seq, vehId u8
                                     0x64, 0x00, 0x00, 0x32, 0x00, 0xC0, 0x12, 0x00, 0xA0, 0x00, 0xC0, 0x00, 0xE0, 0x00,
                                     0xFF, 0xFD, 0xF7, 0xDF, 0x00, 0x00, 0x04, 0x7F,       // rotation, av 0 0 4, steer 127
                                     0x14, 0xE7, 0x03, 0xE8, 0x03,                          // flags engine | horn, body 999, engine 1000
                                     0x66, 0x55,                                            // v5: sampleMs 0x5566
                                     0x09, 0x12,                                            // v5: flags2 headlights | indicator L, doors FR | hood
                                     0x06, 0x02, 0x01};                                     // v6: tyres slots 1 + 2, rim slot 1, window 0
        const proto::Bytes wantHit = {0x14, 0x01, 0x02, 0x00, 0x71, 0xD5, 0x06, 0x1B, 0x22, 0x00, 0x6E, 0x79, 0x09};
        const proto::Bytes wantDmg = {0x15, 0x02, 0x00, 0x01, 0x00, 0x71, 0xD5, 0x06, 0x1B, 0x22, 0x00, 0x42, 0x00, 0x00,
                                      0x6E, 0x79,   // v5: bone 31086
                                      0x09};        // v5: flags headshot | armour absorbed
        const proto::Bytes wantKilled = {0x16, 0x02, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                         0x03};     // v5: flags headshot | melee
        const proto::Bytes wantStop = {0x1E, 0x04, 0x00};
        Check("codec v7: VehicleWorld (u8 ids, presence list, 49 + 2-per-occupant full entry (the clean damage block included), 24-byte motion-only entry), VehicleCmd (Register tail), "
              "VehicleUpdate (40 bytes), Hit, Damage (17), Killed (10) and PeerAnimStop are laid out byte for byte",
              vs[0].bytes == wantWorld && vs[1].bytes == wantLock && vs[2].bytes == wantReg && vs[3].bytes == wantUp &&
                  vs[5].bytes == wantHit && vs[6].bytes == wantDmg && vs[7].bytes == wantKilled && wantUp.size() == 40 &&
                  wantDmg.size() == 17 && wantKilled.size() == 10 && vs[15].bytes == wantStop && proto::Encode(proto::Respawn()).size() == 1,
              Hex(vs[0].bytes) + "| " + Hex(vs[2].bytes) + "| " + Hex(vs[3].bytes) + "| " + Hex(vs[5].bytes) + "| " + Hex(vs[6].bytes) + "| " +
                  Hex(vs[7].bytes));
    }

    // Wrong type byte: no v4 decoder takes a core sample or another v4 message, no core decoder a v4 one.
    {
        const std::vector<Sample> core = Samples();
        int accepted = 0;
        for (const VSample& a : vs)
        {
            for (const Sample& b : core)
                if (b.dec(a.bytes.data(), a.bytes.size()) || a.dec(b.bytes.data(), b.bytes.size())) accepted++;
            for (const VSample& b : vs)
                if (a.dec != b.dec && b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        }
        Check("codec v4: a wrong type byte is rejected across the v4 and core decoders", accepted == 0, std::to_string(accepted) + " accepted");
    }

    // Truncation at every length and one trailing byte, for each v4 message.
    {
        int cases = 0, accepted = 0, trailingAccepted = 0;
        for (const VSample& s : vs)
        {
            for (size_t len = 0; len < s.bytes.size(); len++)
            {
                cases++;
                if (s.dec(s.bytes.data(), len)) accepted++;
            }
            proto::Bytes longer = s.bytes;
            longer.push_back(0);
            if (s.dec(longer.data(), longer.size())) trailingAccepted++;
        }
        Check("codec v4: truncation at every length and one trailing byte are rejected for all 16 samples",
              cases > 300 && accepted == 0 && trailingAccepted == 0,
              std::to_string(cases) + " cases, " + std::to_string(accepted) + " truncated accepted, " + std::to_string(trailingAccepted) +
                  " trailing accepted");
    }

    // Well-framed VehicleWorld / VehicleUpdate packets that must still be refused.
    {
        // Patches one byte of the sample world (offset into wantWorld above) and decodes.
        auto worldWith = [&](size_t off, uint8_t v) {
            proto::Bytes b = vs[0].bytes;
            b[off] = v;
            return DecodeAs<proto::VehicleWorld>(b.data(), b.size());
        };
        // Offsets: 9 id 5, 10 its header, 15..22 position, 29..32 rotation, 33..35 av, 36 steer, 37 flags,
        // 38/39 body, 42/43 fuel, 46 ageMs, 47/48 ctrl, 49/50 ownerId, 51 flags2, 52 doors, 53 tyres, 54 tyresRim (v6),
        // 55 windows, 56..58 the v7 damage block (petrol, parts, dent mask: 3 bytes, no dents), 59 seat mask, 60/61 the first occupant,
        // 64 the second entry's id, 65 its header, 66 the third entry's id, 67 its header (2: motion-only), 68 its ageMs,
        // 69..90 its motion (90 steering), 91 its flags. (R3 T4 inserted the 3 damage bytes: everything from the seat mask on moved by 3.)
        std::string bad;
        if (worldWith(10, 0x03)) bad += "[reserved header bit]";
        if (worldWith(10, 0x04)) bad += "[header 4]";
        if (worldWith(65, 0x04)) bad += "[reserved header bit, no state]";
        if (worldWith(65, 0x03)) bad += "[header 3 on a presence-only entry]";
        if (worldWith(67, 0x01)) bad += "[motion-only entry read as a full one]";
        if (worldWith(67, 0x00)) bad += "[motion-only entry read as presence-only]";
        if (!worldWith(51, 0xFF)) bad += "[flags2 with bits 6 and 7 (brake lights, handbrake) is legal in v6]";
        if (worldWith(53, 0x00)) bad += "[rim bit without its tyre bit]";
        if (worldWith(52, 0x61)) bad += "[doors bit 6]";
        if (worldWith(48, 0x04)) bad += "[ctrl over kMaxPlayers]";
        if (worldWith(50, 0x04)) bad += "[ownerId over kMaxPlayers]";
        if (worldWith(90, 0x80)) bad += "[motion-only steering -128]";
        if (worldWith(91, 0x24)) bad += "[motion-only unknown flag 32]";
        if (worldWith(64, 0x05)) bad += "[present not ascending]";
        if (worldWith(9, 0x07)) bad += "[present and removed]";
        if (worldWith(6, 0x09)) bad += "[removed and present]";
        if (worldWith(32, 0xFF)) bad += "[rotation code 1023]"; // 0xFFF7FDFF: bits 20..29 = 1023
        if (worldWith(33, 0x80)) bad += "[angular velocity -128]";
        if (worldWith(36, 0x80)) bad += "[steering -128]";
        if (worldWith(37, 0x23)) bad += "[unknown flag 32]";
        if (worldWith(39, 0x04)) bad += "[body over 1000]";
        if (worldWith(43, 0x04)) bad += "[fuel over 1000]";
        if (worldWith(61, 0x04)) bad += "[occupant id over kMaxPlayers]";
        if (worldWith(59, 0x07)) bad += "[seat mask lies]";
        if (worldWith(56, 0xFB)) bad += "[petrol 251 (R3 T4)]";
        if (worldWith(58, 0x01)) bad += "[dent mask names a dent that is not there (R3 T4)]";
        if (worldWith(8, 0x01) || worldWith(7, 0x04)) bad += "[present count lies]"; // 259 present; 4 present with 3 listed
        // Out of the box: z = -2^19 cm (-5243 m, the 20-bit field's minimum) fits the field but not the box.
        {
            proto::Bytes b = vs[0].bytes;
            b[20] = (uint8_t)(b[20] & 0x0F); // keep y's top bits, clear z's low nibble
            b[21] = 0x00;
            b[22] = 0x80;
            if (DecodeAs<proto::VehicleWorld>(b.data(), b.size())) bad += "[out of the box]";
        }
        // Up to kMaxVehicles present decode; the encoder refuses one more (u8 ids) and a hand-made count of 257 is refused.
        proto::VehicleWorld big;
        for (uint16_t i = 0; i < proto::kMaxVehicles; i++) big.present.push_back(i);
        const proto::Bytes bigB = proto::Encode(big);
        if (bigB.empty() || !DecodeAs<proto::VehicleWorld>(bigB.data(), bigB.size())) bad += "[256 present]";
        proto::VehicleWorld over = big;
        over.present.push_back(256);
        if (!proto::Encode(over).empty()) bad += "[encoder took id 256]";
        {
            wire::Writer w;
            w.u8((uint8_t)proto::Msg::VehicleWorld);
            w.u32(1);
            w.u8(0);
            w.u16(257);
            for (int i = 0; i < 257; i++) { w.u8((uint8_t)i); w.u8(0); }
            const proto::Bytes b = w.take();
            if (DecodeAs<proto::VehicleWorld>(b.data(), b.size())) bad += "[count 257]";
        }
        // VehicleUpdate: unknown flag, body over 1000, steering -128.
        auto upWith = [&](size_t off, uint8_t v) {
            proto::Bytes b = vs[3].bytes;
            b[off] = v;
            return DecodeAs<proto::VehicleUpdate>(b.data(), b.size());
        };
        // Offsets: 27 steering, 28 flags, 29/30 body, 33/34 sampleMs, 35 flags2, 36 doors, 37 tyres, 38 tyresRim, 39 windows.
        if (upWith(28, 0x34)) bad += "[update flag 32]";
        if (upWith(30, 0x04)) bad += "[update body over 1000]";
        if (upWith(27, 0x80)) bad += "[update steering -128]";
        if (!upWith(35, 0xFF)) bad += "[update flags2 bits 6 and 7 are legal in v6]";
        if (upWith(37, 0x00)) bad += "[update rim bit without its tyre bit]";
        if (upWith(38, 0x07)) bad += "[update rim bits beyond the tyre bits]";
        if (!upWith(37, 0xFF) || !upWith(39, 0xFF) || !upWith(38, 0x00)) bad += "[update: any tyre / window mask is legal when rim is a subset]";
        if (upWith(36, 0x52)) bad += "[update doors bit 6]";
        if (upWith(36, 0x92)) bad += "[update doors bit 7]";
        if (!upWith(33, 0xFF) || !upWith(35, 0x3F) || !upWith(36, 0x3F)) bad += "[update: every sampleMs, all 6 flags2 and door bits are legal]";
        Check("codec v6: VehicleWorld refuses reserved header bits, unordered or doubly listed ids, rotation code 1023, "
              "-128 in an i8, unknown flags, health or fuel over 1000, a bad occupant id, lying counts and out-of-box positions; "
              "256 present ids decode, 257 do not; VehicleUpdate refuses the same field errors",
              bad.empty(), bad);
    }

    // VehicleCmd ops and the Register tail.
    {
        int known = 0, accepted = 0;
        for (int op = 1; op <= 11; op++)
        {
            proto::VehicleCmd c;
            c.vehId = 1;
            c.op = (proto::VehicleOp)op;
            const proto::Bytes b = proto::Encode(c);
            if (DecodeAs<proto::VehicleCmd>(b.data(), b.size())) known++;
        }
        for (int op : {0, 12, 255})
        {
            wire::Writer w;
            w.u8((uint8_t)proto::Msg::VehicleCmd);
            w.u16(1);
            w.u8((uint8_t)op);
            w.u32(0);
            const proto::Bytes b = w.take();
            if (DecodeAs<proto::VehicleCmd>(b.data(), b.size())) accepted++;
        }
        const proto::Bytes regNoTail(vs[2].bytes.begin(), vs[2].bytes.begin() + 8);
        proto::Bytes lockWithTail = vs[1].bytes;
        lockWithTail.insert(lockWithTail.end(), vs[2].bytes.begin() + 8, vs[2].bytes.end());
        proto::Bytes regBadRot = vs[2].bytes;
        regBadRot[19] = 0xFF; // rotation code 1023 in bits 20..29
        const bool tails = !DecodeAs<proto::VehicleCmd>(regNoTail.data(), regNoTail.size()) &&
                           !DecodeAs<proto::VehicleCmd>(lockWithTail.data(), lockWithTail.size()) &&
                           !DecodeAs<proto::VehicleCmd>(regBadRot.data(), regBadRot.size());
        Check("codec v5/v6: VehicleCmd accepts ops 1..11 (Release v5, EnterIntent and CancelIntent v6), rejects 0, 12 and 255, a Register without its tail, "
              "a tail on another op and an invalid rotation",
              known == 11 && accepted == 0 && tails, std::to_string(known) + " known, " + std::to_string(accepted) + " bad accepted");
    }

    // Combat and emote refusals, and the Valid* helpers.
    {
        std::string bad;
        auto patched = [&](size_t sample, size_t off, uint8_t v) {
            proto::Bytes b = vs[sample].bytes;
            b[off] = v;
            return vs[sample].dec(b.data(), b.size());
        };
        if (patched(5, 1, 0) || patched(5, 1, 3)) bad += "[hit kind]";
        if (patched(6, 13, 2)) bad += "[killed 2]";
        if (patched(6, 2, 0x04)) bad += "[victim over kMaxPlayers]";
        if (patched(6, 16, 0x10) || patched(6, 16, 0x80)) bad += "[damage flags reserved bit]";
        if (!patched(6, 16, 0x0F) || !patched(6, 16, 0x00)) bad += "[damage flags 0..15 are legal]";
        if (patched(7, 9, 0x04) || patched(7, 9, 0x80)) bad += "[killed flags reserved bit]";
        if (!patched(7, 9, 0x00) || !patched(7, 9, 0x01)) bad += "[killed flags 0..3 are legal]";
        if (patched(4, 2, 13)) bad += "[result code 13]";
        if (!patched(4, 2, 12)) bad += "[result code 12 (NotOwner) refused]";
        if (patched(4, 1, 12)) bad += "[result op 12]";
        if (!patched(4, 1, 9) || !patched(4, 1, 10) || !patched(4, 1, 11)) bad += "[result op 9 (Release) / 10 (EnterIntent) / 11 (CancelIntent) refused]";
        {
            proto::RespawnOk r;
            r.x = std::numeric_limits<float>::quiet_NaN();
            if (!proto::Encode(r).empty()) bad += "[respawn NaN encoded]";
        }
        proto::Anim a;
        a.dict = "anim@mp_player_intcelebrationmale@face_palm";
        a.name = "face_palm";
        const bool good = proto::ValidAnim(a) && proto::ValidAnimName("WORLD_HUMAN_AA_COFFEE") && proto::ValidAnimName("a.b-c/d_e@f");
        if (!good) bad += "[valid names refused]";
        for (const char* n : {"", "has space", "semi;colon", "quote\"", "tab\t", "\xC3\xA9"})
            if (proto::ValidAnimName(n)) bad += std::string("[accepted name '") + n + "']";
        if (proto::ValidAnimName(std::string(64, 'a')) || !proto::ValidAnimName(std::string(63, 'a'))) bad += "[name length]";
        proto::Anim blend = a;
        blend.blendIn = 1001.0f;
        proto::Anim dur = a;
        dur.durationMs = -2;
        proto::Anim nan = a;
        nan.blendOut = std::numeric_limits<float>::infinity();
        if (proto::ValidAnim(blend) || proto::ValidAnim(dur) || proto::ValidAnim(nan)) bad += "[bad anim valid]";
        {
            proto::PlayAnim pa;
            pa.anim = a;
            pa.anim.dict = std::string(64, 'a');
            const proto::Bytes b = proto::Encode(pa);
            if (DecodeAs<proto::PlayAnim>(b.data(), b.size())) bad += "[64-byte dict decoded]";
            proto::PlayScenario ps;
            ps.name = std::string(64, 'B');
            const proto::Bytes c = proto::Encode(ps);
            if (DecodeAs<proto::PlayScenario>(c.data(), c.size())) bad += "[64-byte scenario decoded]";
        }
        proto::VehicleUpdate u;
        u.vehId = 3;
        proto::VehicleUpdate uBad = u;
        uBad.k.vx = std::numeric_limits<float>::quiet_NaN();
        proto::VehicleUpdate uRot = u;
        uRot.rot.w = 0.0f; // zero length
        if (!proto::ValidVehicleUpdate(u) || proto::ValidVehicleUpdate(uBad) || !proto::Encode(uBad).empty() ||
            proto::ValidVehicleUpdate(uRot))
            bad += "[ValidVehicleUpdate]";
        Check("codec v5: Hit kind, Damage killed / victim / flags, Killed flags, VehicleResult op (Release 9, EnterIntent 10 and CancelIntent 11 legal, 12 not) / code (NotOwner 12 legal, 13 not) and non-finite RespawnOk are refused; anim names "
              "are 1..63 of [A-Za-z0-9_@/.-], blends finite within +-1000, duration -1 or more; a 64-byte name never decodes; "
              "ValidVehicleUpdate refuses NaN and a zero rotation",
              bad.empty(), bad);
    }
}

// v5: Welcome.playerHealth, the entering / exiting vehicle id in the ped block, the two new ped flags.
static void CodecV5Tests()
{
    // Welcome: the server's maximum health rides after the name.
    {
        proto::Welcome w;
        w.yourId = 0x0102;
        w.tickHz = 0x0304;
        w.serverName = "S";
        w.playerHealth = 200;
        const proto::Bytes want = {0x02, 0x02, 0x01, 0x04, 0x03, 0x01, 0x00, 'S', 200}; // type, id, tickHz, str, v5: playerHealth
        proto::Bytes zero = want;
        zero.back() = 0;
        proto::Bytes cut(want.begin(), want.end() - 1);
        proto::Bytes longer = want;
        longer.push_back(0);
        proto::Welcome back;
        const bool rt = proto::Decode(want.data(), want.size(), back) && back.playerHealth == 200 && back.serverName == "S" &&
                        back.yourId == 0x0102 && proto::Encode(back) == want;
        Check("codec v5: Welcome is type, u16 yourId, u16 tickHz, u16-length name, u8 playerHealth; a playerHealth of 0, a missing byte and a trailing byte are refused",
              proto::Encode(w) == want && rt && !DecodeAs<proto::Welcome>(zero.data(), zero.size()) &&
                  !DecodeAs<proto::Welcome>(cut.data(), cut.size()) && !DecodeAs<proto::Welcome>(longer.data(), longer.size()),
              Hex(proto::Encode(w)));
    }

    // Entering / exiting a vehicle name the vehicle: one byte after the shot byte, only for move 5 and 6.
    {
        proto::PlayerState ps;
        ps.seq = 1;
        ps.k = Kin(1.0f, -1.0f, 2.5f, 90.0f, 0.0f, 0.0f, 0.0f);
        ps.ped.move = proto::kMoveEnteringVehicle;
        ps.ped.vehId = 200;
        const proto::Bytes enter = proto::Encode(ps);
        proto::PlayerState exiting = ps;
        exiting.ped.move = proto::kMoveExitingVehicle;
        exiting.ped.vehId = 7;
        exiting.ped.flags = proto::kPfAiming;
        exiting.ped.aimX = 1.0f;
        const proto::Bytes exit = proto::Encode(exiting);
        proto::PlayerState driving = ps;
        driving.ped.move = proto::kMoveInVehicle;
        driving.ped.flags = proto::kPfDriver;
        driving.ped.vehId = 9; // meaningless while driving: not sent, decodes as 0
        const proto::Bytes drive = proto::Encode(driving);
        proto::PlayerState e2, x2, d2;
        const bool ok = proto::Decode(enter.data(), enter.size(), e2) && proto::Decode(exit.data(), exit.size(), x2) &&
                        proto::Decode(drive.data(), drive.size(), d2);
        // The ped block starts at offset 31 (type 1, seq 4, ack 8, sampleMs 2, state 16): move byte, flags, weapon, shot, vehId.
        Check("codec v5: move 5 (entering) and 6 (exiting) carry one extra u8 vehId after the shot byte (PlayerState 40 bytes, 46 while aiming); "
              "other moves send nothing and decode vehId 0",
              ok && enter.size() == 40 && enter[31] == proto::kMoveEnteringVehicle && enter[39] == 200 && e2.ped.vehId == 200 &&
                  e2.ped.move == proto::kMoveEnteringVehicle && exit.size() == 46 && x2.ped.vehId == 7 && x2.ped.aimX == 1.0f &&
                  drive.size() == 39 && d2.ped.vehId == 0 && proto::Encode(e2) == enter && proto::Encode(x2) == exit,
              std::to_string(enter.size()) + "/" + std::to_string(exit.size()) + "/" + std::to_string(drive.size()) + " bytes");
    }

    // Flags: swimming (13), melee (14) and, since the 0.4.3 fixes (W8, no version bump), vaulting (15) are known: every bit of the u16 is
    // in use. The move-5/6 byte is mandatory.
    {
        std::string bad;
        auto state = [&](const proto::Bytes& ped) {
            const proto::Bytes b = RawState(1, 10000, 20000, 3000, ped);
            return DecodeAs<proto::PlayerState>(b.data(), b.size());
        };
        if (!state(RawPed(0, proto::kPfSwimming, 0))) bad += "[swimming refused]";
        if (!state(RawPed(0, proto::kPfMelee, 0))) bad += "[melee refused]";
        // Every known flag the wire framing allows together: the aiming flag needs its aim bytes (own test below) and the driver flag a vehicle move.
        if (!state(RawPed(0, (uint16_t)(proto::kPfKnown & ~(proto::kPfAiming | proto::kPfDriver)), 0))) bad += "[all known flags refused]";
        if (!state(RawPed(0, proto::kPfVaulting, 0))) bad += "[vaulting refused]";
        if (proto::kPfSwimming != (1u << 13) || proto::kPfMelee != (1u << 14) || proto::kPfVaulting != (1u << 15) || proto::kPfKnown != 0xFFFFu) bad += "[constants]";
        if (state(RawPed(proto::kMoveEnteringVehicle, 0, 0))) bad += "[move 5 without its vehId byte accepted]";
        if (state(RawPed(proto::kMoveExitingVehicle, 0, 0))) bad += "[move 6 without its vehId byte accepted]";
        proto::Bytes withVeh = RawPed(proto::kMoveEnteringVehicle, 0, 0);
        withVeh.push_back(3);
        if (!state(withVeh)) bad += "[move 5 with its vehId byte refused]";
        proto::Bytes extra = RawPed(proto::kMoveRun, 0, 0);
        extra.push_back(3);
        if (state(extra)) bad += "[a vehId byte after move 2 accepted]";
        if (state(RawPed(7, 0, 0))) bad += "[move 7 accepted]";
        proto::PedState ped;
        ped.move = proto::kMoveEnteringVehicle;
        ped.vehId = 3;
        proto::PedState on;
        on.flags = proto::kPfSwimming | proto::kPfMelee;
        proto::PedState vaulting;
        vaulting.flags = proto::kPfVaulting;
        if (!proto::ValidPedState(ped) || !proto::ValidPedState(on) || !proto::ValidPedState(vaulting)) bad += "[ValidPedState]";
        Check("codec v5: PlayerState accepts kPfSwimming (1<<13), kPfMelee (1<<14) and kPfVaulting (1<<15, no version bump), a move-5/6 ped without its vehId byte and a vehId byte after any other move are refused",
              bad.empty(), bad);
    }
}

// ===========================================================================
// v3: on-foot state and appearance
// ===========================================================================

// A valid look that differs with `seed` in the model, every component, prop and head blend value.
static proto::Look MakeLook(uint32_t seed)
{
    proto::Look l;
    l.model = 0x705E61F2u + seed; // mp_m_freemode_01, shifted per seed
    for (size_t i = 0; i < proto::kLookComponents; i++)
    {
        l.components[i].drawable = (uint16_t)((i * 7 + seed) % (proto::kMaxLookDrawable + 1));
        l.components[i].texture = (uint8_t)((i + seed) % (proto::kMaxLookTexture + 1));
        l.components[i].palette = (uint8_t)((i + seed) % (proto::kMaxLookPalette + 1));
    }
    for (size_t i = 0; i < proto::kLookProps; i++)
    {
        const bool none = (i + seed) % 3 == 0;
        l.props[i].drawable = none ? (int16_t)-1 : (int16_t)((i * 11 + seed) % (proto::kMaxLookDrawable + 1));
        l.props[i].texture = none ? (uint8_t)0 : (uint8_t)((i * 2 + seed) % (proto::kMaxLookTexture + 1));
    }
    proto::HeadBlend& h = l.head;
    h.shape1 = (uint8_t)(seed % 46);
    h.shape2 = (uint8_t)((seed + 5) % 46);
    h.shape3 = 0;
    h.skin1 = (uint8_t)((seed + 9) % 46);
    h.skin2 = 3;
    h.skin3 = 0;
    h.shapeMix = (uint8_t)(seed * 37);
    h.skinMix = (uint8_t)(255 - seed);
    h.thirdMix = 0;
    return l;
}

static void LookCodecTests()
{
    const proto::Look look = MakeLook(4);
    proto::PlayerLook pl;
    pl.look = look;
    proto::PeerLook peer;
    peer.id = 1023;
    peer.look = look;
    const proto::Bytes plB = proto::Encode(pl), peerB = proto::Encode(peer);

    {
        proto::PlayerLook a;
        proto::PeerLook b;
        const bool rt = proto::Decode(plB.data(), plB.size(), a) && proto::Encode(a) == plB &&
                        proto::Decode(peerB.data(), peerB.size(), b) && proto::Encode(b) == peerB;
        Check("codec: PlayerLook (86 bytes) and PeerLook (88 bytes) round-trip byte for byte and decode every field",
              rt && plB.size() == 86 && peerB.size() == 88 && a.look == look && b.look == look && b.id == 1023 &&
                  b.look.props[0].drawable == look.props[0].drawable && b.look.head.skinMix == look.head.skinMix,
              std::to_string(plB.size()) + " / " + std::to_string(peerB.size()) + " bytes");
    }

    // The wire layout restated independently of the codec: type, u32 model, 12 x (u16 drawable, u8 texture,
    // u8 palette), 8 x (i16 drawable, u8 texture), 9 x u8 head blend (shape1..3, skin1..3, shapeMix, skinMix,
    // thirdMix), little-endian; PeerLook puts a u16 id after the type.
    {
        proto::Look l;
        l.model = 0x705E61F2u;
        for (size_t i = 0; i < proto::kLookComponents; i++) l.components[i] = {(uint16_t)(0x100 + i), (uint8_t)i, (uint8_t)(i % 4)};
        for (size_t i = 0; i < proto::kLookProps; i++) l.props[i] = {i == 0 ? (int16_t)-1 : (int16_t)(200 + i), i == 0 ? (uint8_t)0 : (uint8_t)i};
        l.head = {21, 0, 45, 3, 4, 5, 128, 255, 0};
        proto::Bytes want = {0x11, 0x05, 0x00, 0xF2, 0x61, 0x5E, 0x70};
        for (size_t i = 0; i < proto::kLookComponents; i++)
            want.insert(want.end(), {(uint8_t)(i & 0xFF), 0x01, (uint8_t)i, (uint8_t)(i % 4)});
        want.insert(want.end(), {0xFF, 0xFF, 0x00});
        for (size_t i = 1; i < proto::kLookProps; i++) want.insert(want.end(), {(uint8_t)(200 + i), 0x00, (uint8_t)i});
        want.insert(want.end(), {21, 0, 45, 3, 4, 5, 128, 255, 0});
        proto::PeerLook m;
        m.id = 5;
        m.look = l;
        Check("codec: PeerLook wire layout: type 0x11, u16 id, u32 model, 12 x (u16, u8, u8), 8 x (i16, u8), 9 x u8 head blend",
              want.size() == 88 && proto::Encode(m) == want, Hex(proto::Encode(m)));
    }

    struct LSample { proto::Bytes bytes; bool (*dec)(const uint8_t*, size_t); };
    const std::vector<LSample> ls = {{plB, &DecodeAs<proto::PlayerLook>}, {peerB, &DecodeAs<proto::PeerLook>}};
    const std::vector<Sample> core = Samples();
    {
        int accepted = 0;
        for (const LSample& a : ls)
            for (const Sample& b : core)
                if (b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        for (const LSample& a : ls)
            for (const LSample& b : ls)
                if (&a != &b && b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        for (const Sample& a : core)
            for (const LSample& b : ls)
                if (b.dec(a.bytes.data(), a.bytes.size())) accepted++;
        int cases = 0, truncAccepted = 0, trailingAccepted = 0;
        for (const LSample& s : ls)
        {
            for (size_t len = 0; len < s.bytes.size(); len++)
            {
                cases++;
                if (s.dec(s.bytes.data(), len)) truncAccepted++;
            }
            proto::Bytes longer = s.bytes;
            longer.push_back(0);
            if (s.dec(longer.data(), longer.size())) trailingAccepted++;
        }
        proto::PeerLook badId = peer;
        badId.id = proto::kMaxPlayers;
        const proto::Bytes badIdB = proto::Encode(badId);
        Check("codec: look messages: a wrong type byte, truncation at every length, a trailing byte and a PeerLook id >= kMaxPlayers are refused",
              accepted == 0 && cases == 174 && truncAccepted == 0 && trailingAccepted == 0 && !DecodeAs<proto::PeerLook>(badIdB.data(), badIdB.size()),
              std::to_string(accepted) + " cross, " + std::to_string(truncAccepted) + "/" + std::to_string(cases) + " truncated, " +
                  std::to_string(trailingAccepted) + " trailing accepted");
    }

    // ValidLook: the edges in, one past them out.
    {
        proto::Look edge = MakeLook(1);
        edge.components[3] = {proto::kMaxLookDrawable, proto::kMaxLookTexture, proto::kMaxLookPalette};
        edge.props[2] = {(int16_t)proto::kMaxLookDrawable, proto::kMaxLookTexture};
        edge.props[3] = {-1, 0};
        edge.head = {proto::kMaxHeadBlendId, 0, 0, proto::kMaxHeadBlendId, 0, 0, 255, 255, 255};
        std::vector<std::pair<const char*, std::function<void(proto::Look&)>>> bads = {
            {"model 0", [](proto::Look& l) { l.model = 0; }},
            {"component drawable 1024", [](proto::Look& l) { l.components[11].drawable = proto::kMaxLookDrawable + 1; }},
            {"component texture 64", [](proto::Look& l) { l.components[0].texture = proto::kMaxLookTexture + 1; }},
            {"palette 4", [](proto::Look& l) { l.components[5].palette = proto::kMaxLookPalette + 1; }},
            {"prop drawable -2", [](proto::Look& l) { l.props[0].drawable = -2; }},
            {"prop drawable 1024", [](proto::Look& l) { l.props[7].drawable = (int16_t)(proto::kMaxLookDrawable + 1); }},
            {"prop texture 64", [](proto::Look& l) { l.props[1].drawable = 3; l.props[1].texture = proto::kMaxLookTexture + 1; }},
            {"no prop with a texture", [](proto::Look& l) { l.props[4].drawable = -1; l.props[4].texture = 1; }},
            {"head shape 64", [](proto::Look& l) { l.head.shape3 = proto::kMaxHeadBlendId + 1; }},
            {"head skin 64", [](proto::Look& l) { l.head.skin2 = proto::kMaxHeadBlendId + 1; }},
        };
        std::string wrong;
        if (!proto::ValidLook(edge)) wrong += "[edge]";
        for (auto& b : bads)
        {
            proto::Look l = edge;
            b.second(l);
            if (proto::ValidLook(l)) wrong += std::string("[") + b.first + "]";
        }
        Check("codec: ValidLook accepts every edge (drawable 1023, texture 63, palette 3, prop -1, head id 63, any mix) and refuses one past each, model 0, and a texture on 'no prop'",
              wrong.empty() && bads.size() == 10, wrong);
    }

    // ValidPedState and what Encode refuses to frame.
    {
        auto ped = [](uint8_t move, uint16_t flags) {
            proto::PedState s;
            s.move = move;
            s.flags = flags;
            return s;
        };
        proto::PedState nanAim = ped(proto::kMoveIdle, proto::kPfAiming);
        nanAim.aimZ = std::numeric_limits<float>::infinity();
        proto::PedState nanIgnored = ped(proto::kMoveIdle, 0);
        nanIgnored.aimZ = std::numeric_limits<float>::quiet_NaN();
        proto::PedState bigJump = ped(proto::kMoveWalk, proto::kPfJumping);
        bigJump.jumpSeq = 200; // only the low 2 bits travel
        const bool good = proto::ValidPedState(proto::PedState()) && proto::ValidPedState(ped(proto::kMoveExitingVehicle, 0)) &&
                          proto::ValidPedState(ped(proto::kMoveInVehicle, proto::kPfDriver)) &&
                          proto::ValidPedState(ped(proto::kMoveExitingVehicle, proto::kPfDriver)) &&
                          proto::ValidPedState(ped(proto::kMoveSprint, proto::kPfKnown & ~proto::kPfDriver)) &&
                          proto::ValidPedState(nanIgnored) && proto::ValidPedState(bigJump);
        // v5: bits 13 (swimming), 14 (melee) and 15 (vaulting, 0.4.3) are known: no reserved bit is left.
        const bool bad = proto::ValidPedState(ped(7, 0)) || !proto::ValidPedState(ped(0, proto::kPfVaulting)) ||
                         proto::ValidPedState(ped(proto::kMoveSprint, proto::kPfDriver)) || proto::ValidPedState(nanAim);
        proto::PlayerState ps8, psNan, psIgnored;
        ps8.ped.move = 8;
        psNan.ped = nanAim;
        psIgnored.ped = nanIgnored;
        proto::WorldState ws8;
        ws8.tick = 1;
        ws8.players.push_back(proto::WorldEntry());
        ws8.players[0].ped.move = 8;
        Check("codec: ValidPedState: move 0..6, known flags, driver only with move >= in-vehicle, finite aim while aiming; Encode refuses a move over 7 bits and a non-finite aim while aiming (and ignores it otherwise)",
              good && !bad && proto::Encode(ps8).empty() && proto::Encode(psNan).empty() && proto::Encode(psIgnored).size() == 39 /* v5: 37 + the u16 sampleMs */ &&
                  proto::Encode(ws8).empty());
    }
}

// The on-foot state over the wire: what B sees of A, how fast a standing player's flags-only change
// arrives, and what the server refuses.
static void PedNetworkTests()
{
    Rig rig;
    if (!rig.Start()) { Check("ped: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    b.s.SendState(Kin(110.0f, 200.0f, 30.0f));
    const uint16_t aid = a.s.myId();
    const proto::Kinematics where = Kin(100.0f, 200.0f, 30.0f, 45.0f);

    // Sends (k, ped) from A every 50 ms (as a game does every frame) until `pred` or the budget.
    auto sendUntil = [&](const proto::PedState& ped, const std::function<bool()>& pred, int timeoutMs) {
        const auto t0 = Clock::now();
        auto next = t0;
        while (MsSince(t0) < timeoutMs)
        {
            if (Clock::now() >= next)
            {
                a.s.SendState(where, ped);
                next += std::chrono::milliseconds(50);
            }
            rig.Step();
            if (pred()) return true;
        }
        return pred();
    };

    proto::PedState ped;
    ped.move = proto::kMoveRun;
    ped.jumpSeq = 3;
    ped.flags = proto::kPfAiming | proto::kPfJumping | proto::kPfShooting;
    ped.weapon = 0x1B06D571u;
    ped.aimX = 25.0f;
    ped.aimY = -12.3f;
    ped.aimZ = 1.5f;
    auto matches = [&](const proto::PedState& want) {
        const proto::WorldEntry* e = b.Seen(aid);
        return e && e->ped.move == want.move && e->ped.jumpSeq == (want.jumpSeq & 3) && e->ped.flags == want.flags &&
               e->ped.weapon == want.weapon && std::fabs(e->ped.aimX - want.aimX) <= 0.051f &&
               std::fabs(e->ped.aimY - want.aimY) <= 0.051f && std::fabs(e->ped.aimZ - want.aimZ) <= 0.051f;
    };
    const bool seen = sendUntil(ped, [&] { return matches(ped); }, 3000);
    const proto::WorldEntry* e = b.Seen(aid);
    Check("ped: B sees A's move, jumpSeq, flags, weapon and aim point (within 5 cm) in WorldState",
          seen && e && e->k.x == 100.0f && rig.srv.Players().size() == 2,
          e ? "move " + std::to_string(e->ped.move) + " flags " + std::to_string(e->ped.flags) : "not seen");

    // A standing player whose only change is its on-foot state is not held back by the 1 Hz idle rate:
    // three times, after 1.2 s of standing still (idle), a flags-only change must reach B within 250 ms.
    int fast = 0;
    std::string times;
    for (int k = 0; k < 3; k++)
    {
        sendUntil(ped, [] { return false; }, 1200); // idle: same k, same ped for > idleAfterMs
        ped.flags ^= proto::kPfReloading;
        const auto t0 = Clock::now();
        const bool got = sendUntil(ped, [&] { return matches(ped); }, 1500);
        const int64_t ms = MsSince(t0);
        times += std::to_string(ms) + " ";
        if (got && ms <= 250) fast++;
    }
    Check("ped: a flags-only change of a standing (idle-rate) player reaches the watcher within 250 ms, 3 of 3 (the idle slot is 1 s)",
          fast == 3, times + "ms");

    // Refusals: hand-written packets the client library never sends. Decode refuses an unknown move and aim bytes
    // without the aiming flag (malformed; every flag bit is known since kPfVaulting took bit 15); the server policy
    // refuses a driver on foot (invalid).
    const uint64_t mal0 = rig.srv.stats().statesMalformed, inv0 = rig.srv.stats().statesInvalid;
    uint32_t seq = a.s.lastSentSeq() + 1000;
    // Reliable on purpose: four back-to-back unreliable sends may legally lose one (seen under wine on the VPS:
    // "malformed +2"), and this check is about the server's refusal, not about delivery.
    a.s.SendRaw(proto::kChanState, RawState(seq++, 10000, 20000, 3000, RawPed(7, 0, 0)), true);
    a.s.SendRaw(proto::kChanState, RawState(seq++, 10000, 20000, 3000, RawPed(0, 0, 0, {1, 2, 3})), true);
    a.s.SendRaw(proto::kChanState, RawState(seq++, 10000, 20000, 3000, RawPed(proto::kMoveSprint, proto::kPfDriver, 0)), true);
    const bool counted = rig.Until([&] {
        return rig.srv.stats().statesMalformed == mal0 + 2 && rig.srv.stats().statesInvalid == inv0 + 1;
    }, 1500);
    rig.Pump(200);
    proto::PedState driverOnFoot;
    driverOnFoot.move = proto::kMoveSprint;
    driverOnFoot.flags = proto::kPfDriver;
    proto::PedState badMove;
    badMove.move = 7;
    const std::vector<server::PlayerInfo> ps = rig.srv.Players();
    bool applied = false;
    for (const server::PlayerInfo& p : ps)
        if (p.id == aid && (p.ped.flags & proto::kPfDriver)) applied = true;
    Check("ped: the server drops move 7 and aim bytes without the aiming flag (malformed) and a driver on foot (invalid), applies none, keeps the sender; SendState refuses such a ped itself",
          counted && !applied && a.Joined() && rig.LogHas("inconsistent on-foot state") && !a.s.SendState(where, driverOnFoot) &&
              !a.s.SendState(where, badMove),
          "malformed +" + std::to_string(rig.srv.stats().statesMalformed - mal0) + ", invalid +" +
              std::to_string(rig.srv.stats().statesInvalid - inv0));
}

// The PeerLooks player `rx` received about `id`, in order.
static std::vector<proto::Look> LooksOf(const Probe& rx, uint16_t id)
{
    std::vector<proto::Look> out;
    for (const net::Event& e : rx.events)
        if (e.type == ET::PeerLook && e.look.id == id) out.push_back(e.look.look);
    return out;
}

static void LookTests()
{
    Rig rig;
    if (!rig.Start()) { Check("look: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    const uint16_t aid = a.s.myId(), bid = b.s.myId();
    const proto::Look la = MakeLook(1), lb = MakeLook(2);

    // A look reaches everyone else as PeerLook, never its sender.
    const bool sent = a.s.SendLook(la) && b.s.SendLook(lb);
    const bool arrived = rig.Until([&] { return LooksOf(b, aid).size() == 1 && LooksOf(a, bid).size() == 1; }, 2000);
    rig.Pump(200);
    Check("look: a PlayerLook reaches every other player as PeerLook with the same look, and not its sender",
          sent && arrived && LooksOf(b, aid)[0] == la && LooksOf(a, bid)[0] == lb && LooksOf(a, aid).empty() && LooksOf(b, bid).empty() &&
              rig.srv.stats().looksBroadcast == 2);

    // The same look again: SendLook skips it client-side; a raw resend is ignored by the server. A change is broadcast.
    const uint64_t unchanged0 = rig.srv.stats().looksUnchanged;
    const bool skipped = a.s.SendLook(la);
    rig.Pump(200);
    const bool notSent = rig.srv.stats().looksUnchanged == unchanged0;
    proto::PlayerLook again;
    again.look = la;
    a.SendReliable(proto::Encode(again));
    const bool ignored = rig.Until([&] { return rig.srv.stats().looksUnchanged == unchanged0 + 1; }, 1500);
    const proto::Look la2 = MakeLook(11);
    a.s.SendLook(la2);
    const bool changed = rig.Until([&] { return LooksOf(b, aid).size() == 2; }, 2000);
    rig.Pump(200);
    Check("look: an unchanged look is not re-sent by SendLook nor re-broadcast by the server; a changed one is broadcast",
          skipped && notSent && ignored && changed && LooksOf(b, aid).size() == 2 && LooksOf(b, aid)[1] == la2);

    // A late joiner gets every stored look right after its PlayerJoined burst, once each, the newest version.
    Probe c(rig);
    c.Join("Carol");
    rig.Until([&] { return LooksOf(c, aid).size() == 1 && LooksOf(c, bid).size() == 1; }, 2000);
    rig.Pump(200);
    int lastJoined = -1, firstLook = -1, i = 0;
    for (const net::Event& e : c.events)
    {
        if (e.type == ET::PlayerJoined) lastJoined = i;
        if (e.type == ET::PeerLook && firstLook < 0) firstLook = i;
        i++;
    }
    Check("look: a late joiner gets PeerLook for every player with a look (the newest one), once each, right after its PlayerJoined burst",
          LooksOf(c, aid).size() == 1 && LooksOf(c, aid)[0] == la2 && LooksOf(c, bid).size() == 1 && LooksOf(c, bid)[0] == lb &&
              lastJoined >= 0 && firstLook == lastJoined + 1 && c.Count(ET::PeerLook) == 2 && rig.srv.stats().looksReplayed == 2,
          c.Describe());

    // Invalid looks are dropped and logged, the sender stays; SendLook refuses them itself; a malformed one is refused.
    const uint64_t invalid0 = rig.srv.stats().looksInvalid;
    proto::PlayerLook bad;
    bad.look = MakeLook(3);
    bad.look.model = 0;
    a.SendReliable(proto::Encode(bad));
    bad.look = MakeLook(3);
    bad.look.components[2].drawable = proto::kMaxLookDrawable + 1;
    a.SendReliable(proto::Encode(bad));
    const bool dropped = rig.Until([&] { return rig.srv.stats().looksInvalid == invalid0 + 2; }, 1500);
    rig.Pump(200);
    const bool clientRefuses = !a.s.SendLook(bad.look);
    Probe d(rig);
    d.Join("Dave");
    proto::Bytes truncated = proto::Encode(again);
    truncated.pop_back();
    d.SendReliable(truncated);
    const bool cut = d.WaitClosed(3000);
    Check("look: a look with model 0 or a drawable over the cap is dropped and logged (sender stays, nobody told); SendLook refuses it; a truncated PlayerLook is refused (Protocol)",
          dropped && a.Joined() && LooksOf(b, aid).size() == 2 && rig.LogHas("PlayerLook rejected") && clientRefuses && cut &&
              d.RefusedWith(proto::RefuseReason::Protocol));
}

// 0.5.4 outfit_items: the worn items' clothes ride the look sync unchanged. The spawn outfit, the shirt / pants / shoes taken off in the
// inventory (sync::inv::Apply, the server's own rules) and put through sync::outfit::Desired onto the look the engine would read back, reach
// the other player as PeerLook with exactly the bare drawables (ValidLook takes legs 61 / feet 34); putting them back on restores the spawn
// look byte for byte.
static proto::Look WithOutfit(proto::Look l, const gage::sync::inv::Character& ch)
{
    const gage::sync::outfit::Want w = gage::sync::outfit::Desired(ch, gage::sync::outfit::Sex::Male);
    for (int c = 0; c < gage::sync::outfit::kComponents; c++)
        if (w.want[c])
        {
            l.components[(size_t)c].drawable = (uint16_t)w.drawable[c];
            l.components[(size_t)c].texture = w.texture[c];
        }
    return l;
}
static void OutfitLookTest()
{
    namespace inv = gage::sync::inv;
    Rig rig;
    if (!rig.Start()) { Check("outfit look: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    const uint16_t aid = a.s.myId();
    // the engine's spawn outfit (engine.cpp kPlayerOutfit): head 0/0, mask 0/0, hair 1/0, arms 8/0, legs 10/0, bag 0, shoes 8/0, neck 0,
    // undershirt 15/0, armour 0, decal 0, top 8/10; palette 2
    proto::Look spawn;
    spawn.model = gage::sync::outfit::kFreemodeMale;
    const int kD[12] = {0, 0, 1, 8, 10, 0, 8, 0, 15, 0, 0, 8}, kT[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10};
    for (size_t i = 0; i < proto::kLookComponents; i++)
    {
        spawn.components[i].drawable = (uint16_t)kD[i];
        spawn.components[i].texture = (uint8_t)kT[i];
        spawn.components[i].palette = 2;
    }
    inv::Character kit = inv::StarterKit();
    const proto::Look worn = WithOutfit(spawn, kit);
    inv::Character bare = kit;
    for (inv::Equip e : {inv::Equip::Shirt, inv::Equip::Pants, inv::Equip::Shoes})
    {
        inv::Op op;
        op.kind = inv::OpKind::Use;   // USE on a worn item takes it off into the pockets
        op.src = inv::Where::Equip;
        op.si = (int16_t)inv::EquipIndex(e);
        inv::Apply(bare, op);
    }
    const proto::Look off = WithOutfit(spawn, bare);
    const bool sent = a.s.SendLook(off);
    const bool arrived = rig.Until([&] { return LooksOf(b, aid).size() == 1; }, 2000);
    rig.Pump(100);
    const bool offOk = arrived && LooksOf(b, aid)[0] == off && off.components[11].drawable == 15 && off.components[3].drawable == 15 &&
                       off.components[8].drawable == 15 && off.components[4].drawable == 61 && off.components[6].drawable == 34 &&
                       off.components[2].drawable == 1 && proto::ValidLook(off);
    a.s.SendLook(worn);
    const bool back = rig.Until([&] { return LooksOf(b, aid).size() == 2; }, 2000);
    rig.Pump(100);
    Check("outfit look (0.5.4): shirt / pants / shoes taken off in the inventory reach the other player as PeerLook (top 15, arms 15, undershirt 15, "
          "legs 61, feet 34, hair untouched); worn again, the spawn look comes back byte for byte",
          sent && offOk && back && LooksOf(b, aid)[1] == spawn && worn == spawn,
          "arrived " + std::to_string(LooksOf(b, aid).size()));
}

// The look rate limit: 2 at once, then 2 a second; over it the newest waits and wins.
static void LookRateTest()
{
    Rig rig;
    if (!rig.Start()) { Check("look rate: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    const uint16_t aid = a.s.myId();
    std::vector<proto::Look> burst;
    for (uint32_t k = 0; k < 5; k++) burst.push_back(MakeLook(20 + k));
    bool sent = true;
    for (const proto::Look& l : burst) sent = sent && a.s.SendLook(l); // one burst: no Poll in between
    const auto t0 = Clock::now();
    const bool two = rig.Until([&] { return LooksOf(b, aid).size() >= 2; }, 1500);
    const int64_t tTwo = MsSince(t0);
    const bool three = rig.Until([&] { return LooksOf(b, aid).size() >= 3; }, 2000);
    const int64_t tThree = MsSince(t0);
    rig.Pump(1200); // nothing else may follow
    const std::vector<proto::Look> got = LooksOf(b, aid);
    const server::Stats& st = rig.srv.stats();
    Check("look rate: 5 looks in a burst: the first 2 go out at once, the newest (5th) ~0.5 s later, the 3rd and 4th never (deferred 3, coalesced 2)",
          sent && two && three && got.size() == 3 && got[0] == burst[0] && got[1] == burst[1] && got[2] == burst[4] && tTwo < 300 &&
              tThree >= 350 && tThree <= 1200 && st.looksBroadcast == 3 && st.looksDeferred == 3 && st.looksCoalesced == 2,
          std::to_string(got.size()) + " looks, 2nd at " + std::to_string(tTwo) + " ms, 3rd at " + std::to_string(tThree) + " ms");
}

// ===========================================================================
// v4: vehicles, combat and emotes over the wire
// ===========================================================================

static uint16_t IdOf(const Probe& p)
{
    const net::Event* w = p.First(ET::Welcome);
    return w ? w->welcome.yourId : proto::kNoPlayer;
}

static const server::PlayerInfo* InfoOf(const std::vector<server::PlayerInfo>& all, uint16_t id)
{
    for (const server::PlayerInfo& i : all)
        if (i.id == id) return &i;
    return nullptr;
}

// Sends `k` until the server holds it as `p`'s position.
static bool Place(Rig& rig, Probe& p, const proto::Kinematics& k)
{
    const uint16_t id = IdOf(p);
    return SendUntil(rig, p, k, [&] {
        const std::vector<server::PlayerInfo> all = rig.srv.Players();
        const server::PlayerInfo* i = InfoOf(all, id);
        return i && i->hasState && std::fabs(i->k.x - k.x) < 0.05f && std::fabs(i->k.y - k.y) < 0.05f;
    }, 2000);
}

// One VehicleCmd and its VehicleResult (code 255 when none came).
static proto::VehicleResult VCmd(Rig& rig, Probe& p, proto::VehicleOp op, uint16_t veh, uint32_t arg,
                                 const std::function<void(proto::VehicleCmd&)>& tweak = nullptr)
{
    proto::VehicleCmd c;
    c.op = op;
    c.vehId = veh;
    c.arg = arg;
    if (tweak) tweak(c);
    const size_t before = p.Results().size();
    proto::VehicleResult none;
    none.code = (proto::VehicleResultCode)255;
    if (!p.s.SendVehicleCmd(c)) return none;
    rig.Until([&] { return p.Results().size() > before; }, 2000);
    const std::vector<proto::VehicleResult> r = p.Results();
    return r.size() > before ? r.back() : none;
}

static std::string Code(const proto::VehicleResult& r) { return proto::ToString(r.code); }

static proto::VehicleUpdate Drive(uint16_t veh, float x, float y, float vy, uint16_t body = 1000)
{
    proto::VehicleUpdate u;
    u.vehId = veh;
    u.k = Kin(x, y, 30.0f, 0.0f, 0.0f, vy, 0.0f);
    u.rot = proto::QuatFromHeading(0.0f);
    u.flags = proto::kVehEngine;
    u.bodyHealth = body;
    return u;
}

static const uint32_t kAdder = 0xB779A091u; // a GTA model hash; the server never interprets it

static void VehicleNetworkTests()
{
    Rig rig;
    if (!rig.Start()) { Check("vehicles: server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("vehicles: three players join", false); return; }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob);

    // ---- occupancy ----
    const proto::VehicleResult early = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
    const bool placed = Place(rig, alice, Kin(100, 100, 30)) && Place(rig, bob, Kin(103, 100, 30)) && Place(rig, carol, Kin(160, 100, 30));
    const proto::VehicleResult spawn = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
    const uint16_t vid = spawn.vehId;
    const server::VehicleStore::Vehicle* sv = rig.srv.FindVehicle(vid);
    const bool ahead = sv && std::fabs(sv->s.k.x - 100.0f) < 0.05f && std::fabs(sv->s.k.y - 104.0f) < 0.05f && sv->spawner == aId && !sv->adopted;
    rig.Until([&] { return bob.Vehicle(vid) != nullptr; }, 2000);
    const proto::VehicleResult aIn = VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0);
    const proto::VehicleResult bTaken = VCmd(rig, bob, proto::VehicleOp::Enter, vid, 0);
    const proto::VehicleResult bIn = VCmd(rig, bob, proto::VehicleOp::Enter, vid, 1);
    const proto::VehicleResult cFar = VCmd(rig, carol, proto::VehicleOp::Enter, vid, 2);
    const proto::VehicleResult lock = VCmd(rig, alice, proto::VehicleOp::SetLocked, vid, 1);
    const proto::VehicleResult bLock = VCmd(rig, bob, proto::VehicleOp::SetLocked, vid, 0);
    Place(rig, carol, Kin(101, 103, 30));
    const proto::VehicleResult cLocked = VCmd(rig, carol, proto::VehicleOp::Enter, vid, 2);
    const proto::VehicleResult cNotIn = VCmd(rig, carol, proto::VehicleOp::Exit, vid, 0);
    const proto::VehicleResult aOut = VCmd(rig, alice, proto::VehicleOp::Exit, vid, 0);
    const proto::VehicleResult aBack = VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0); // locked, but hers
    const proto::VehicleResult badSeat = VCmd(rig, carol, proto::VehicleOp::Enter, vid, 8);
    const proto::VehicleResult noVeh = VCmd(rig, carol, proto::VehicleOp::Enter, 200, 0);
    const bool seen = rig.Until([&] {
        const proto::VehicleState* v = carol.Vehicle(vid);
        return v && v->seats[0] == aId && v->seats[1] == bId && v->seats[2] == proto::kNoPlayer && (v->flags & proto::kVehLocked);
    }, 2000);
    using C = proto::VehicleResultCode;
    Check("vehicles: occupancy is the server's: NoState before a position, Spawn 4 m ahead, NotOwner for another player's driver seat "
          "(v5; v4 said SeatTaken), TooFar, Locked (the spawner still enters), NotAllowed, NotInVehicle, BadArg, NoVehicle; every "
          "player sees the seats and the lock",
          early.code == C::NoState && placed && spawn.code == C::Ok && ahead && aIn.code == C::Ok && bTaken.code == C::NotOwner &&
              bIn.code == C::Ok && cFar.code == C::TooFar && lock.code == C::Ok && bLock.code == C::NotAllowed &&
              cLocked.code == C::Locked && cNotIn.code == C::NotInVehicle && aOut.code == C::Ok && aBack.code == C::Ok &&
              badSeat.code == C::BadArg && noVeh.code == C::NoVehicle && seen,
          Code(early) + " " + Code(spawn) + " " + Code(aIn) + " " + Code(bTaken) + " " + Code(bIn) + " " + Code(cFar) + " " + Code(lock) +
              " " + Code(bLock) + " " + Code(cLocked) + " " + Code(cNotIn) + " " + Code(aOut) + " " + Code(aBack) + " " + Code(badSeat) +
              " " + Code(noVeh) + (ahead ? "" : " [not 4 m ahead]") + (seen ? "" : " [seats not seen]"));

    // ---- only the driver moves it ----
    {
        const server::Stats s0 = rig.srv.stats();
        alice.s.SendVehicleState(Drive(vid, 100, 120, 10, 600));
        const bool moved = rig.Until([&] { const proto::VehicleState* v = bob.Vehicle(vid); return v && std::fabs(v->k.y - 120.0f) < 0.02f; }, 2000);
        bob.s.SendVehicleState(Drive(vid, 100, 300, 0));                  // a passenger
        alice.s.SendVehicleState(Drive(vid, 100, 130, 400));              // 400 m/s (T4: under twice the cap is clamped, not dropped)
        rig.Pump(200);
        alice.s.SendVehicleState(Drive(vid, 100, 121, 10, 900));          // health up: ignored
        rig.Until([&] { return rig.srv.stats().vehicleUpdatesAccepted >= s0.vehicleUpdatesAccepted + 2; }, 2000);
        rig.Pump(150);
        const server::Stats s1 = rig.srv.stats();
        const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(vid);
        const bool state = v && std::fabs(v->s.k.y - 121.0f) < 0.02f && v->s.bodyHealth == 600 && (v->s.flags & proto::kVehLocked) &&
                           (v->s.flags & proto::kVehEngine) && std::fabs(v->s.k.vy - 10.0f) < 0.05f;
        Check("vehicles: only the server's driver moves a vehicle (a passenger's update is dropped), over twice the 150 m/s cap is dropped, "
              "health only goes down, the lock stays the server's while the engine flag is the driver's; the others see it",
              moved && state && s1.vehicleUpdatesRejected == s0.vehicleUpdatesRejected + 1 &&
                  s1.vehicleUpdatesInvalid == s0.vehicleUpdatesInvalid + 1 && s1.vehicleUpdatesAccepted == s0.vehicleUpdatesAccepted + 2,
              std::to_string(s1.vehicleUpdatesAccepted - s0.vehicleUpdatesAccepted) + " accepted, " +
                  std::to_string(s1.vehicleUpdatesRejected - s0.vehicleUpdatesRejected) + " rejected, " +
                  std::to_string(s1.vehicleUpdatesInvalid - s0.vehicleUpdatesInvalid) + " invalid" + (moved ? "" : " [not seen]"));
    }

    // ---- the driver leaves the server ----
    {
        alice.s.Disconnect();
        const bool freed = rig.Until([&] {
            const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(vid);
            return v && v->s.seats[0] == proto::kNoPlayer;
        }, 3000);
        const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(vid);
        const bool parked = v && v->s.k.vx == 0 && v->s.k.vy == 0 && v->s.k.vz == 0 && v->s.seats[1] == bId && v->spawner == proto::kNoPlayer;
        const proto::VehicleResult take = VCmd(rig, bob, proto::VehicleOp::Enter, vid, 0); // locked; a shuffle inside skips the doors
        const bool bobDrives = rig.Until([&] { const proto::VehicleState* cv = carol.Vehicle(vid); return cv && cv->seats[0] == bId && cv->seats[1] == proto::kNoPlayer; }, 2000);
        Check("vehicles: a driver's disconnect frees the seat, parks the vehicle (velocity 0) and forgets the spawner; the passenger takes the wheel",
              freed && parked && take.code == C::Ok && bobDrives, Code(take));
    }

    // ---- Register: adopt a local car ----
    {
        Probe dave(rig);
        if (!dave.Join("Dave") || !Place(rig, dave, Kin(-500, 200, 30))) { Check("vehicles: Register", false); return; }
        const uint16_t dId = IdOf(dave);
        auto reg = [&](float x, float y, float heading, uint8_t seat) {
            return VCmd(rig, dave, proto::VehicleOp::Register, 0, kAdder, [&](proto::VehicleCmd& c) {
                c.x = x;
                c.y = y;
                c.z = 30.0f;
                c.rot = proto::QuatFromHeading(heading);
                c.colour1 = 27;
                c.colour2 = 1;
                c.seat = seat;
            });
        };
        const proto::VehicleResult far = reg(-450, 200, 0, 0);
        const proto::VehicleResult r1 = reg(-498, 200, 90, 0);
        const server::VehicleStore::Vehicle* v1 = rig.srv.FindVehicle(r1.vehId);
        const bool adopted = r1.code == C::Ok && v1 && v1->adopted && v1->spawner == dId && v1->s.seats[0] == dId &&
                             std::fabs(v1->s.k.heading - 90.0f) < 0.3f && v1->s.colour1 == 27 && std::fabs(v1->s.k.x + 498.0f) < 0.02f;
        const proto::VehicleResult r2 = reg(-497, 201, 0, 0);
        const proto::VehicleResult r3 = reg(-496, 202, 0, 1);
        rig.Pump(1100); // the Spawn / Register bucket: 3 at once, then 1 a second
        const proto::VehicleResult r4 = reg(-495, 203, 0, 0);
        const bool gaveUpOldest = r4.code == C::Ok && rig.srv.FindVehicle(r1.vehId) == nullptr && rig.srv.FindVehicle(r2.vehId) &&
                                  rig.srv.FindVehicle(r3.vehId);
        const bool removedSeen = rig.Until([&] { return dave.vehiclesRemoved.count(r1.vehId) > 0; }, 2000);
        Check("vehicles: Register adopts a local car within 10 m (adopted, spawner, the requester seated, heading 90 from the quaternion); "
              "farther is TooFar; the 4th adoption gives up the oldest empty one, and its removal reaches the client",
              far.code == C::TooFar && adopted && r2.code == C::Ok && r3.code == C::Ok && gaveUpOldest && removedSeen,
              Code(far) + " " + Code(r1) + " " + Code(r2) + " " + Code(r3) + " " + Code(r4) + (adopted ? "" : " [adopt fields]"));
    }
}

static void VehicleLifetimeTests()
{
    // Caps: 3 spawned per player, 4 in all; despawn by the spawner only.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.maxVehicles = 4;
        if (!rig.Start(cfg)) { Check("vehicles caps: server starts", false); return; }
        Probe dave(rig), eve(rig);
        if (!dave.Join("Dave") || !eve.Join("Eve") || !Place(rig, dave, Kin(0, 0, 30)) || !Place(rig, eve, Kin(10, 0, 30)))
        {
            Check("vehicles caps: players join", false);
            return;
        }
        using C = proto::VehicleResultCode;
        const proto::VehicleResult d1 = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult d2 = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult d3 = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult dRate = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder); // the 1/s bucket is empty
        rig.Pump(1100);
        const proto::VehicleResult d4 = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);   // per-player cap
        const proto::VehicleResult e1 = VCmd(rig, eve, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult e2 = VCmd(rig, eve, proto::VehicleOp::Spawn, 0, kAdder);    // global cap
        const proto::VehicleResult zero = VCmd(rig, eve, proto::VehicleOp::Spawn, 0, 0);       // model 0
        const proto::VehicleResult eDel = VCmd(rig, eve, proto::VehicleOp::Despawn, d1.vehId, 0);
        const proto::VehicleResult dDel = VCmd(rig, dave, proto::VehicleOp::Despawn, d1.vehId, 0);
        const bool gone = rig.Until([&] { return eve.vehiclesRemoved.count(d1.vehId) > 0; }, 2000) && rig.srv.Vehicles().size() == 3;
        std::string out;
        rig.srv.Execute("vehicles", out);
        Check("vehicles: 3 spawned per player and maxVehicles in all (Limit), Spawn at most 3 at once then 1 a second (RateLimited), "
              "model 0 is BadArg; only the spawner despawns, and the removal reaches the others; the console lists them",
              d1.code == C::Ok && d2.code == C::Ok && d3.code == C::Ok && dRate.code == C::RateLimited && d4.code == C::Limit &&
                  e1.code == C::Ok && e2.code == C::Limit && zero.code == C::BadArg && eDel.code == C::NotAllowed && dDel.code == C::Ok &&
                  gone && out.find("vehicles 3/4") != std::string::npos,
              Code(d1) + " " + Code(d2) + " " + Code(d3) + " " + Code(dRate) + " " + Code(d4) + " " + Code(e1) + " " + Code(e2) + " " +
                  Code(zero) + " " + Code(eDel) + " " + Code(dDel) + " | " + out);
    }
    // An empty vehicle is despawned after vehicleEmptyDespawnMs; an occupied one stays.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.vehicleEmptyDespawnMs = 700;
        if (!rig.Start(cfg)) { Check("vehicles despawn: server starts", false); return; }
        Probe dave(rig);
        if (!dave.Join("Dave") || !Place(rig, dave, Kin(0, 0, 30))) { Check("vehicles despawn: player joins", false); return; }
        const proto::VehicleResult a = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult b = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult in = VCmd(rig, dave, proto::VehicleOp::Enter, b.vehId, 0);
        const auto t0 = Clock::now();
        const bool emptyGone = rig.Until([&] { return rig.srv.FindVehicle(a.vehId) == nullptr; }, 3000);
        const int64_t ms = MsSince(t0);
        rig.Pump(900);
        const bool occupiedStays = rig.srv.FindVehicle(b.vehId) != nullptr;
        Check("vehicles: an empty vehicle is despawned after vehicleEmptyDespawnMs (700 ms here), an occupied one stays",
              a.code == proto::VehicleResultCode::Ok && in.code == proto::VehicleResultCode::Ok && emptyGone && ms >= 400 && occupiedStays &&
                  rig.LogHas("vehicle " + std::to_string(a.vehId) + " removed"),
              std::to_string(ms) + " ms");
    }
}

static const uint32_t kPistol = 0x1B06D571u;  // WEAPON_PISTOL
static const uint32_t kUnarmed = 0xA2719263u; // WEAPON_UNARMED (fists)

static proto::Hit HitOn(uint16_t target, uint16_t damage, proto::HitKind kind = proto::HitKind::Player, uint32_t weapon = kPistol,
                        uint8_t shotSeq = 0)
{
    proto::Hit h;
    h.kind = kind;
    h.targetId = target;
    h.weapon = weapon;
    h.damage = damage;
    h.shotSeq = shotSeq;
    return h;
}

// A Hit that names `shooter`'s latest shot (Fire() advanced its counter).
static proto::Hit ShotHit(const Probe& shooter, uint16_t target, uint16_t damage, uint32_t weapon = kPistol,
                          proto::HitKind kind = proto::HitKind::Player)
{
    return HitOn(target, damage, kind, weapon, shooter.shotSeq);
}

static void CombatNetworkTests()
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.respawnDelayMs = 800;
    cfg.maxHitDistance = 10;
    if (!rig.Start(cfg)) { Check("combat: server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("combat: three players join", false); return; }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob), cId = IdOf(carol);
    const bool placed = Place(rig, alice, Kin(0, 0, 30)) && Place(rig, bob, Kin(5, 0, 30)) && Place(rig, carol, Kin(20, 0, 30));
    const bool relevant = rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(aId, cId) && rig.srv.IsRelevant(cId, bId); }, 3000);
    auto lastDamage = [](const Probe& p) { const auto d = p.Damages(); return d.empty() ? proto::Damage() : d.back(); };
    auto damageCount = [](const Probe& p) { return p.Damages().size(); };
    std::string out;

    // Damage reaches the victim, the attacker and (v5) everybody for whom either is relevant: Carol at 20 m sees both. (Every
    // Hit names a shot Alice's states reported: Fire() advances the counter; the Hit binding itself is tested in
    // AuthorityNetworkTests; the onlooker rule's boundary - a player out of range of both - in CombatV5Tests.)
    const bool fired1 = Fire(rig, alice, Kin(0, 0, 30));
    alice.s.SendHit(ShotHit(alice, bId, 30));
    const bool both = rig.Until([&] { return damageCount(bob) == 1 && damageCount(alice) == 1 && damageCount(carol) == 1; }, 2000);
    rig.Pump(100);
    const proto::Damage db = lastDamage(bob), da = lastDamage(alice);
    Check("combat: a validated Hit sends Damage (victim, attacker, weapon, applied damage, new health / armour) to the victim, the "
          "attacker and (v5) Carol, who sees them both (v4: not to her); health is the server's 100 - 30",
          placed && relevant && fired1 && both && damageCount(carol) == 1 && db.victimId == bId && db.attackerId == aId && db.damage == 30 &&
              db.health == 70 && db.armor == 0 && !db.killed && db.weapon == 0x1B06D571u && da.victimId == bId && da.health == 70,
          std::to_string(damageCount(bob)) + "/" + std::to_string(damageCount(alice)) + "/" + std::to_string(damageCount(carol)) +
              " damages, health " + std::to_string(db.health));

    // Armour first, then health; the others see the vitals in their WorldEntry.
    rig.srv.Execute("armor Bob 50", out);
    const bool armored = rig.Until([&] { return damageCount(bob) == 2; }, 2000) && lastDamage(bob).armor == 50 &&
                         lastDamage(bob).attackerId == proto::kServerId;
    Fire(rig, alice, Kin(0, 0, 30));
    alice.s.SendHit(ShotHit(alice, bId, 80));
    rig.Until([&] { return damageCount(bob) == 3; }, 2000);
    const proto::Damage d80 = lastDamage(bob);
    const bool worldShows = SendUntil(rig, bob, Kin(5, 0, 30), [&] {
        const proto::WorldEntry* e = carol.Seen(bId);
        return e && e->ped.health == 40 && e->ped.armor == 0;
    }, 2000);
    Check("combat: armour is taken first, then health (armour 50, hit 80 -> armour 0, health 70 - 30 = 40, applied 80); "
          "the console's `armor` tells the player; a third player sees health 40 / armour 0 in the WorldEntry",
          armored && d80.damage == 80 && d80.armor == 0 && d80.health == 40 && worldShows,
          "applied " + std::to_string(d80.damage) + " armour " + std::to_string(d80.armor) + " health " + std::to_string(d80.health));

    // The clamp (a pistol blow is at most 160, the class cap, below the protocol's 250), then a kill: Killed to everybody.
    rig.srv.Execute("heal Bob", out);
    rig.srv.Execute("armor Bob 255", out);
    rig.Until([&] { return damageCount(bob) == 5; }, 2000);
    Fire(rig, alice, Kin(0, 0, 30));
    alice.s.SendHit(ShotHit(alice, bId, 1000));
    rig.Until([&] { return damageCount(bob) == 6; }, 2000);
    const proto::Damage dClamp = lastDamage(bob);
    Fire(rig, alice, Kin(0, 0, 30));
    alice.s.SendHit(ShotHit(alice, bId, 200)); // armour 95 -> 0, health 100 -> 35
    rig.Until([&] { return damageCount(bob) == 7; }, 2000);
    const proto::Damage dMid = lastDamage(bob);
    Fire(rig, alice, Kin(0, 0, 30));
    alice.s.SendHit(ShotHit(alice, bId, 200));
    const bool killedAll = rig.Until([&] { return alice.Count(ET::Killed) == 1 && bob.Count(ET::Killed) == 1 && carol.Count(ET::Killed) == 1; }, 2000);
    const proto::Damage dKill = lastDamage(bob);
    const net::Event* k = carol.First(ET::Killed);
    const auto tDeath = Clock::now();
    Check("combat: damage is clamped to the weapon class's cap (a pistol Hit of 1000 -> 160 on armour 255 -> 95, health 100 kept), "
          "the next two (200 -> 160 each) take the armour and the health (35), the last kills (applied 35), Damage says "
          "killed, and Killed (victim, killer, weapon) reaches every player",
          dClamp.damage == 160 && dClamp.armor == 95 && dClamp.health == 100 && dMid.damage == 160 && dMid.armor == 0 &&
              dMid.health == 35 && killedAll && dKill.killed && dKill.health == 0 && dKill.damage == 35 && k &&
              k->killed.victimId == bId && k->killed.killerId == aId && k->killed.weapon == 0x1B06D571u,
          "clamp " + std::to_string(dClamp.damage) + ", mid " + std::to_string(dMid.damage) + ", kill " + std::to_string(dKill.damage) +
              (killedAll ? "" : " [Killed missing]"));

    // Refusals: dead target, dead shooter, out of range, zero damage (session refuses it locally), rate (a self-hit: environmental).
    {
        const server::Stats s0 = rig.srv.stats();
        const size_t carolBefore = damageCount(carol); // the onlooker Damages of the hits above (v5)
        alice.s.SendHit(HitOn(bId, 10));               // Bob is dead
        bob.s.SendHit(HitOn(aId, 10));                 // the shooter is dead
        alice.s.SendHit(HitOn(cId, 10));               // 20 m > maxHitDistance 10
        alice.s.SendHit(HitOn(77, 10));                // nobody
        const bool zeroRefused = !alice.s.SendHit(HitOn(bId, 0));
        rig.Until([&] { return rig.srv.stats().hitsRejected >= s0.hitsRejected + 4; }, 2000);
        rig.Pump(100);
        const server::Stats s1 = rig.srv.stats();
        Check("combat: Hit refused for a dead target, a dead shooter, a target beyond maxHitDistance and an unknown id (dropped, "
              "counted, nobody told); a zero-damage Hit never leaves the session",
              s1.hitsRejected == s0.hitsRejected + 4 && s1.hitsAccepted == s0.hitsAccepted && zeroRefused && damageCount(alice) == 5 &&
                  damageCount(carol) == carolBefore,
              std::to_string(s1.hitsRejected - s0.hitsRejected) + " rejected, alice " + std::to_string(damageCount(alice)) + " damages");
        for (int i = 0; i < 40; i++) carol.s.SendHit(HitOn(cId, 1)); // self-hit (environment): no range checks, still rate-limited
        rig.Until([&] { return rig.srv.stats().hitsAccepted + rig.srv.stats().hitsRejected >= s1.hitsAccepted + s1.hitsRejected + 40; }, 3000);
        rig.Pump(100);
        const server::Stats s2 = rig.srv.stats();
        const uint64_t taken = s2.hitsAccepted - s1.hitsAccepted;
        const proto::Damage dc = lastDamage(carol);
        Check("combat: hitsPerSecond (30) bounds a burst of 40 self-hits (environmental damage, armour then health): about 30 "
              "applied, the rest dropped; the victim's Damage arrives once per hit",
              taken >= 30 && taken <= 32 && s2.hitsRejected - s1.hitsRejected == 40 - taken && dc.health == 100 - taken &&
                  dc.attackerId == cId && damageCount(carol) == carolBefore + taken,
              std::to_string(taken) + " applied, health " + std::to_string(dc.health));
    }

    // Respawn: refused while alive and before respawnDelayMs, honoured after it at the configured spawn point.
    {
        const server::Stats s0 = rig.srv.stats();
        alice.s.SendRespawn(); // alive
        bob.s.SendRespawn();   // dead, but too early
        rig.Until([&] { return rig.srv.stats().respawnsRefused >= s0.respawnsRefused + 2; }, 2000);
        const bool earlyRefused = bob.Count(ET::RespawnOk) == 0 && rig.srv.stats().respawnsRefused == s0.respawnsRefused + 2;
        rig.Until([&] { return MsSince(tDeath) >= 850; }, 2000);
        bob.s.SendRespawn();
        const bool ok = rig.Until([&] { return bob.Count(ET::RespawnOk) == 1; }, 2000);
        const net::Event* r = bob.First(ET::RespawnOk);
        const std::vector<server::PlayerInfo> all = rig.srv.Players();
        const server::PlayerInfo* bi = InfoOf(all, bId);
        Check("combat: Respawn is ignored while alive and before respawnDelayMs (800 ms) after the death, then answered with "
              "RespawnOk at the spawn point (Legion Square) with full health; the server shows the player alive",
              earlyRefused && ok && r && std::fabs(r->respawn.x - 195.2f) < 0.01f && std::fabs(r->respawn.y + 933.8f) < 0.01f &&
                  r->respawn.heading == 140.0f && r->respawn.health == 100 && r->respawn.armor == 0 && bi && !bi->dead && bi->health == 100 &&
                  (bi->ped.flags & proto::kPfDead) == 0,
              std::string(earlyRefused ? "" : "[early not refused] ") + (ok ? "" : "[no RespawnOk]"));
    }

    // A vehicle hit: body health down by the damage, engine by half; the others see it.
    {
        Place(rig, alice, Kin(0, 0, 30));
        const proto::VehicleResult sp = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const bool known = rig.Until([&] { return alice.Vehicle(sp.vehId) && carol.Vehicle(sp.vehId); }, 2000);
        Fire(rig, alice, Kin(0, 0, 30));
        alice.s.SendHit(ShotHit(alice, sp.vehId, 100, kPistol, proto::HitKind::Vehicle));
        const bool seen = rig.Until([&] {
            const proto::VehicleState* v = carol.Vehicle(sp.vehId);
            return v && v->bodyHealth == 900 && v->engineHealth == 950;
        }, 2000);
        alice.s.SendHit(HitOn(201, 100, proto::HitKind::Vehicle)); // no such vehicle
        const uint64_t rej = rig.srv.stats().hitsRejected;
        rig.Until([&] { return rig.srv.stats().hitsRejected > rej; }, 1000);
        Check("combat: a Hit on a vehicle takes the damage off its body health and half off the engine (1000 -> 900 / 950), "
              "seen by the others in VehicleWorld; a Hit on a missing vehicle is refused",
              sp.code == proto::VehicleResultCode::Ok && known && seen && rig.srv.stats().hitsRejected == rej + 1);
    }

    // The console: kill (Killed by kServerId; the others see kPfDead and health 0), heal refuses the dead, unknown names.
    {
        const int killedBefore = alice.Count(ET::Killed);
        const server::CommandResult kr = rig.srv.Execute("kill Carol", out);
        const bool feed = rig.Until([&] { return alice.Count(ET::Killed) == killedBefore + 1; }, 2000);
        const net::Event* last = nullptr;
        for (const net::Event& e : alice.events)
            if (e.type == ET::Killed) last = &e;
        const bool deadSeen = SendUntil(rig, carol, Kin(20, 0, 30), [&] {
            const proto::WorldEntry* e = alice.Seen(cId);
            return e && (e->ped.flags & proto::kPfDead) && e->ped.health == 0;
        }, 2000);
        std::string o2, o3, o4;
        const server::CommandResult healDead = rig.srv.Execute("heal Carol", o2);
        const server::CommandResult killNobody = rig.srv.Execute("kill Nobody", o3);
        const server::CommandResult armorBad = rig.srv.Execute("armor Alice 300", o4);
        std::string help;
        rig.srv.Execute("help", help);
        Check("combat: console `kill` (Killed by kServerId, the others see kPfDead and health 0 in the WorldEntry); `heal` refuses a "
              "dead player, `kill` an unknown one, `armor` a value over 255; help lists heal, kill, armor and vehicles",
              kr == server::CommandResult::Ok && feed && last && last->killed.victimId == cId &&
                  last->killed.killerId == proto::kServerId && deadSeen && healDead == server::CommandResult::Error &&
                  killNobody == server::CommandResult::Error && armorBad == server::CommandResult::Error &&
                  help.find("heal <id|name>") != std::string::npos && help.find("kill <id|name>") != std::string::npos &&
                  help.find("armor <id|name>") != std::string::npos && help.find("vehicles") != std::string::npos,
              o2 + " | " + o3 + " | " + o4);
    }
}

static proto::Anim Smoke()
{
    proto::Anim a;
    a.dict = "amb@world_human_smoking@male@male_a@base";
    a.name = "base";
    a.blendIn = 4.0f;
    a.blendOut = -4.0f;
    a.durationMs = -1;
    a.flags = 49;
    a.playbackRate = 100;
    return a;
}

static bool SameAnim(const proto::Anim& a, const proto::Anim& b)
{
    return a.dict == b.dict && a.name == b.name && a.blendIn == b.blendIn && a.blendOut == b.blendOut && a.durationMs == b.durationMs &&
           a.flags == b.flags && a.playbackRate == b.playbackRate;
}

static void AnimNetworkTests()
{
    Rig rig;
    if (!rig.Start()) { Check("anims: server starts", false); return; }
    Probe alice(rig), bob(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("anims: two players join", false); return; }
    const uint16_t aId = IdOf(alice);

    // A loop is relayed and replayed to a late joiner (after its PlayerJoined); the sender hears nothing back.
    alice.s.SendAnim(Smoke());
    const bool relayed = rig.Until([&] { return bob.Count(ET::PeerAnim) == 1; }, 2000);
    const net::Event* pa = bob.First(ET::PeerAnim);
    Probe carol(rig);
    const bool cj = carol.Join("Carol");
    const bool replay = rig.Until([&] { return carol.Count(ET::PeerAnim) == 1; }, 2000);
    const net::Event* pc = carol.First(ET::PeerAnim);
    size_t joinedAt = 0, animAt = 0;
    for (size_t i = 0; i < carol.events.size(); i++)
    {
        if (carol.events[i].type == ET::PlayerJoined && carol.events[i].joined.id == aId) joinedAt = i + 1;
        if (carol.events[i].type == ET::PeerAnim) animAt = i + 1;
    }
    Check("anims: a looping PlayAnim reaches the others as PeerAnim with every field, is replayed to a late joiner after the "
          "sender's PlayerJoined, and is not echoed to the sender",
          relayed && pa && pa->anim.id == aId && SameAnim(pa->anim.anim, Smoke()) && cj && replay && pc && pc->anim.id == aId &&
              SameAnim(pc->anim.anim, Smoke()) && joinedAt > 0 && animAt > joinedAt && alice.Count(ET::PeerAnim) == 0,
          bob.Describe() + " | carol " + carol.Describe() + " | relayed " + std::to_string(relayed) + " cj " + std::to_string(cj) +
              " replay " + std::to_string(replay) + " joinedAt " + std::to_string(joinedAt) + " animAt " + std::to_string(animAt) +
              " aliceAnims " + std::to_string(alice.Count(ET::PeerAnim)));

    // StopAnim clears it; a scenario is relayed and replayed; a one-shot is relayed but replaces (and is never replayed).
    {
        alice.s.SendStopAnim();
        const bool stopped = rig.Until([&] { return bob.Count(ET::PeerAnimStop) == 1 && carol.Count(ET::PeerAnimStop) == 1; }, 2000);
        Probe dave(rig);
        const bool dj = dave.Join("Dave");
        rig.Pump(200);
        const bool noReplay = dave.Count(ET::PeerAnim) == 0 && dave.Count(ET::PeerScenario) == 0;
        alice.s.SendScenario("WORLD_HUMAN_SMOKING");
        const bool sc = rig.Until([&] { return bob.Count(ET::PeerScenario) == 1 && dave.Count(ET::PeerScenario) == 1; }, 2000);
        Probe erin(rig);
        const bool ej = erin.Join("Erin");
        const bool scReplay = rig.Until([&] { return erin.Count(ET::PeerScenario) == 1; }, 2000);
        const net::Event* es = erin.First(ET::PeerScenario);
        proto::Anim once = Smoke();
        once.durationMs = 2000;
        alice.s.SendAnim(once);
        const bool oneShot = rig.Until([&] { return erin.Count(ET::PeerAnim) == 1; }, 2000);
        Probe finn(rig);
        const bool fj = finn.Join("Finn");
        rig.Pump(250);
        const bool notReplayed = finn.Count(ET::PeerAnim) == 0 && finn.Count(ET::PeerScenario) == 0;
        Check("anims: StopAnim reaches the others as PeerAnimStop and nothing is replayed after it; a scenario is relayed and "
              "replayed to a late joiner; a one-shot animation is relayed, replaces the scenario and is never replayed",
              stopped && dj && noReplay && sc && ej && scReplay && es && es->scenario.id == aId && es->scenario.name == "WORLD_HUMAN_SMOKING" &&
                  oneShot && fj && notReplayed);
    }

    // Refusals: a bad character is dropped (counted, connection kept); a 64-byte name is a protocol violation.
    {
        rig.Pump(1100); // Alice's emote bucket is full again (the rate is checked before the name)
        const server::Stats s0 = rig.srv.stats();
        proto::Anim bad = Smoke();
        bad.name = "base; drop";
        proto::PlayAnim m;
        m.anim = bad;
        const bool localRefused = !alice.s.SendAnim(bad);
        alice.SendReliable(proto::Encode(m));
        proto::PlayScenario ps;
        ps.name = "WORLD HUMAN";
        alice.SendReliable(proto::Encode(ps));
        rig.Until([&] { return rig.srv.stats().animsInvalid >= s0.animsInvalid + 2; }, 2000);
        const int bobAnims = bob.Count(ET::PeerAnim) + bob.Count(ET::PeerScenario);
        rig.Pump(100);
        Probe mallory(rig);
        const bool mj = mallory.Join("Mallory");
        proto::PlayAnim longName;
        longName.anim = Smoke();
        longName.anim.name = std::string(64, 'a');
        mallory.SendReliable(proto::Encode(longName));
        const bool kicked = mallory.WaitClosed(3000) && mallory.RefusedWith(proto::RefuseReason::Protocol);
        Check("anims: a name outside [A-Za-z0-9_@/.-] is refused by the session and, sent raw, dropped by the server (counted, "
              "nobody told, still connected); a name over 63 bytes is a protocol violation",
              localRefused && rig.srv.stats().animsInvalid == s0.animsInvalid + 2 && bob.Count(ET::PeerAnim) + bob.Count(ET::PeerScenario) == bobAnims &&
                  alice.Joined() && mj && kicked,
              std::to_string(rig.srv.stats().animsInvalid - s0.animsInvalid) + " invalid");
    }

    // Rate: 4 at once, then 4 a second.
    {
        const int before = alice.Count(ET::PeerAnim);
        const server::Stats s0 = rig.srv.stats();
        for (int i = 0; i < 10; i++) bob.s.SendAnim(Smoke());
        rig.Until([&] { return rig.srv.stats().animsRateLimited + rig.srv.stats().animsRelayed >= s0.animsRateLimited + s0.animsRelayed + 10; }, 2000);
        rig.Pump(150);
        const int got = alice.Count(ET::PeerAnim) - before;
        Check("anims: a burst of 10 PlayAnims from one player is cut to about 4 (4 at once, 4 a second); the rest are dropped and counted",
              got >= 4 && got <= 5 && rig.srv.stats().animsRateLimited - s0.animsRateLimited == (uint64_t)(10 - got),
              std::to_string(got) + " relayed");
    }
}

// ===========================================================================
// v4 authority rules against modded clients (apps/server/anticheat.h, docs/api/server.md "Anti-cheat rules")
// ===========================================================================

static const uint32_t kSultan = server::ac::Joaat("sultan");
static const uint32_t kRifle = server::ac::Joaat("WEAPON_CARBINERIFLE");
static const uint32_t kShotgun = server::ac::Joaat("WEAPON_PUMPSHOTGUN");
static const uint32_t kSniper = server::ac::Joaat("WEAPON_SNIPERRIFLE");
static const uint32_t kGrenade = server::ac::Joaat("WEAPON_GRENADE");
static const uint32_t kFall = server::ac::Joaat("WEAPON_FALL");
static const uint32_t kRunOver = 0xA36D413Eu; // WEAPON_RUN_OVER_BY_CAR, the cause the engine reports for a car

static int HealthOf(Rig& rig, uint16_t id)
{
    for (const server::PlayerInfo& i : rig.srv.Players())
        if (i.id == id) return i.health;
    return -1;
}
static bool UntrustedOf(Rig& rig, uint16_t id)
{
    for (const server::PlayerInfo& i : rig.srv.Players())
        if (i.id == id) return i.untrusted;
    return false;
}
// Waits until `n` more Hits were judged (accepted or refused) since `s0`, then lets the replies land.
static void HitsJudged(Rig& rig, const server::Stats& s0, size_t n)
{
    rig.Until([&] {
        const server::Stats& s = rig.srv.stats();
        return s.hitsAccepted + s.hitsRejected >= s0.hitsAccepted + s0.hitsRejected + n;
    }, 2500);
    rig.Pump(60);
}

static void HitBindingTests()
{
    Rig rig;
    server::Config cfg = Rig::BaseConfig();
    cfg.maxHitDistance = 100;
    if (!rig.Start(cfg)) { Check("hit binding: server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("hit binding: three players join", false); return; }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob), cId = IdOf(carol);
    const bool placed = Place(rig, alice, Kin(0, 0, 30)) && Place(rig, bob, Kin(5, 0, 30)) && Place(rig, carol, Kin(60, 0, 30));
    const bool relevant = rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(aId, cId) && rig.srv.IsRelevant(cId, bId); }, 3000);
    std::string out;
    auto reset = [&](int armor = 0) {
        rig.srv.Execute("heal Bob", out);
        rig.srv.Execute("armor Bob " + std::to_string(armor), out);
        rig.Pump(60);
    };
    auto lastDamage = [](const Probe& p) { const auto d = p.Damages(); return d.empty() ? proto::Damage() : d.back(); };
    const Probe* none = nullptr;
    (void)none;

    // A Hit with no shot behind it: held for the state it might be waiting for, then refused.
    {
        const server::Stats s0 = rig.srv.stats();
        const auto t0 = Clock::now();
        alice.s.SendHit(HitOn(bId, 90, proto::HitKind::Player, kPistol, 5)); // Alice never fired, holds nothing
        HitsJudged(rig, s0, 1);
        const int64_t ms = MsSince(t0);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a forged Hit (no shot of the shooter's, no weapon in hand) is held 300 ms for the state it may be waiting for, "
              "then refused and counted; the victim is untouched",
              placed && relevant && s1.hitsRejected == s0.hitsRejected + 1 && s1.hitsAccepted == s0.hitsAccepted &&
                  s1.hitsDeferred == s0.hitsDeferred + 1 && s1.hitsUnbound == s0.hitsUnbound + 1 && HealthOf(rig, bId) == 100 &&
                  bob.Damages().empty() && ms >= 250,
              std::to_string(ms) + " ms, " + std::to_string(s1.hitsRejected - s0.hitsRejected) + " rejected");
    }

    // The same Hit with a real shot (the state carrying the shotSeq change reached the server) and the weapon in hand.
    {
        const server::Stats s0 = rig.srv.stats();
        const bool fired = Fire(rig, alice, Kin(0, 0, 30));
        alice.s.SendHit(ShotHit(alice, bId, 30));
        HitsJudged(rig, s0, 1);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a Hit that names a shot the server saw (shotSeq changed in Alice's states, pistol in hand) is applied",
              fired && s1.hitsAccepted == s0.hitsAccepted + 1 && HealthOf(rig, bId) == 70 && lastDamage(bob).damage == 30 &&
                  lastDamage(bob).attackerId == aId && s1.hitsDeferred == s0.hitsDeferred,
              std::to_string(HealthOf(rig, bId)) + " hp");
    }

    // One pistol shot pays for two Hits (a 250-point split, two pellets) and no more; a weapon the shooter never held is refused.
    {
        reset();
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30));
        for (int i = 0; i < 3; i++) alice.s.SendHit(ShotHit(alice, bId, 5));
        HitsJudged(rig, s0, 3);
        const server::Stats s1 = rig.srv.stats();
        const int hp1 = HealthOf(rig, bId);
        Fire(rig, alice, Kin(0, 0, 30)); // a fresh shot of the PISTOL, then a Hit that claims the rifle
        alice.s.SendHit(ShotHit(alice, bId, 5, kRifle));
        HitsJudged(rig, s1, 1);
        const server::Stats s2 = rig.srv.stats();
        Check("hit binding: one pistol shot is credited with at most 2 Hits (the 3rd is refused), and a Hit claiming a weapon the shooter "
              "never reported (a rifle) is refused even with a fresh shot",
              s1.hitsAccepted == s0.hitsAccepted + 2 && s1.hitsRejected == s0.hitsRejected + 1 && hp1 == 90 &&
                  s2.hitsRejected == s1.hitsRejected + 1 && s2.hitsAccepted == s1.hitsAccepted && HealthOf(rig, bId) == 90,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted of 3, hp " + std::to_string(hp1));
    }

    // A shotgun blast: up to 8 pellets on one shot.
    {
        reset();
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kShotgun);
        for (int i = 0; i < 9; i++) alice.s.SendHit(ShotHit(alice, bId, 3, kShotgun));
        HitsJudged(rig, s0, 9);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a shotgun shot is credited with up to 8 pellets (the 9th Hit is refused)",
              s1.hitsAccepted == s0.hitsAccepted + 8 && s1.hitsRejected == s0.hitsRejected + 1 && HealthOf(rig, bId) == 76,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted, hp " + std::to_string(HealthOf(rig, bId)));
    }

    // Damage caps by weapon class: a sniper rifle is held to the protocol's 250, and the engine's own batching still works.
    {
        reset(255);
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kSniper);
        alice.s.SendHit(ShotHit(alice, bId, 1000, kSniper));
        HitsJudged(rig, s0, 1);
        const proto::Damage d = lastDamage(bob);
        Check("hit binding: a sniper Hit of 1000 is clamped to 250 (the class cap equals the protocol clamp), armour first (255 -> 5)",
              d.damage == 250 && d.armor == 5 && d.health == 100 && d.weapon == kSniper, std::to_string(d.damage));
    }

    // The reliable Hit can overtake the unreliable state that carries its shot: it waits for it.
    {
        reset();
        const server::Stats s0 = rig.srv.stats();
        const uint8_t next = (uint8_t)((alice.shotSeq + 1) & 15);
        alice.s.SendHit(HitOn(bId, 20, proto::HitKind::Player, kPistol, next));
        rig.Pump(80); // the Hit is in; its shot is not yet
        const bool early = rig.srv.stats().hitsAccepted == s0.hitsAccepted && HealthOf(rig, bId) == 100;
        Fire(rig, alice, Kin(0, 0, 30)); // the state with that shot arrives
        const bool done = rig.Until([&] { return HealthOf(rig, bId) == 80; }, 2000);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a Hit that arrives before the state carrying its shot is held and applied when the state lands "
              "(deferred 1, accepted 1, refused 0)",
              early && done && s1.hitsDeferred == s0.hitsDeferred + 1 && s1.hitsAccepted == s0.hitsAccepted + 1 &&
                  s1.hitsRejected == s0.hitsRejected,
              std::string(early ? "" : "[applied too early] ") + std::to_string(HealthOf(rig, bId)) + " hp");
    }

    // Melee needs a swing (v5: a step of the shot counter in the shooter's states; v4 needed none), reach (4 m) and has an own
    // per-weapon ceiling (fists 40, W8; the class cap is 100); a firearm used as a club needs reach only (ceiling 40). Fire() with the default count is a swing, with 0 the weapon is out only.
    {
        reset(255);
        Place(rig, bob, Kin(2, 0, 30));
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kUnarmed);
        alice.s.SendHit(ShotHit(alice, bId, 1000, kUnarmed)); // W8: the client says 1000 (a running punch / takedown on its 1000-point puppet)
        HitsJudged(rig, s0, 1);
        const proto::Damage near = lastDamage(bob);
        Place(rig, bob, Kin(5, 0, 30));
        Fire(rig, alice, Kin(0, 0, 30), kUnarmed);
        alice.s.SendHit(ShotHit(alice, bId, 20, kUnarmed));
        HitsJudged(rig, s0, 2);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a melee Hit (fists) needs a swing and the target within 4 m, and is capped at the fist ceiling 40 (claimed 1000 -> 40; was 100); at 5 m, swing or not, it is refused",
              near.damage == 40 && near.weapon == kUnarmed && s1.hitsAccepted == s0.hitsAccepted + 1 &&
                  s1.hitsRejected == s0.hitsRejected + 1,
              std::to_string(near.damage));
        rig.Pump(600); // every earlier shot is stale now
        Place(rig, bob, Kin(2, 0, 30));
        const server::Stats s2 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kPistol, 0); // holds a pistol, fires nothing
        alice.s.SendHit(HitOn(bId, 250, proto::HitKind::Player, kPistol, (uint8_t)((alice.shotSeq + 7) & 15)));
        HitsJudged(rig, s2, 1);
        const proto::Damage whip = lastDamage(bob);
        Place(rig, bob, Kin(5, 0, 30));
        Fire(rig, alice, Kin(0, 0, 30), kPistol, 0);
        alice.s.SendHit(HitOn(bId, 20, proto::HitKind::Player, kPistol, (uint8_t)((alice.shotSeq + 7) & 15)));
        HitsJudged(rig, s2, 2);
        const server::Stats s3 = rig.srv.stats();
        Check("hit binding: a pistol Hit with no shot at 2 m is a pistol-whip (melee ceiling 40; was 100); at 5 m, with no shot, it is refused",
              whip.damage == 40 && whip.weapon == kPistol && s3.hitsAccepted == s2.hitsAccepted + 1 && s3.hitsRejected == s2.hitsRejected + 1,
              std::to_string(whip.damage));
        Place(rig, bob, Kin(5, 0, 30));
    }

    // Run-over: the shooter must drive a vehicle that moves, within 12 m of the target.
    {
        reset();
        Place(rig, alice, Kin(0, 0, 30));
        const proto::VehicleResult sp = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult in = VCmd(rig, alice, proto::VehicleOp::Enter, sp.vehId, 0);
        alice.s.SendVehicleState(Drive(sp.vehId, 0, 4, 0));
        rig.Pump(120);
        alice.s.SendVehicleState(Drive(sp.vehId, 0, 8, 30)); // 4 m in ~0.15 s of server time: moving
        rig.Pump(60);
        const server::Stats s0 = rig.srv.stats();
        alice.s.SendHit(HitOn(bId, 50, proto::HitKind::Player, kRunOver, 0)); // Bob at 9.4 m from the vehicle
        alice.s.SendHit(HitOn(cId, 50, proto::HitKind::Player, kRunOver, 0)); // Carol at 60 m
        bob.s.SendHit(HitOn(aId, 50, proto::HitKind::Player, kRunOver, 0));   // Bob drives nothing
        HitsJudged(rig, s0, 3);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a run-over Hit is applied for the driver of a moving vehicle within 12 m (50 on Bob); refused at 60 m, "
              "and from a player who drives nothing",
              sp.code == proto::VehicleResultCode::Ok && in.code == proto::VehicleResultCode::Ok && s1.hitsAccepted == s0.hitsAccepted + 1 &&
                  s1.hitsRejected == s0.hitsRejected + 2 && HealthOf(rig, bId) == 50 && HealthOf(rig, cId) == 100 && HealthOf(rig, aId) == 100,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted, bob " + std::to_string(HealthOf(rig, bId)) + " hp");
        VCmd(rig, alice, proto::VehicleOp::Exit, sp.vehId, 0);
    }

    // Environmental damage is for oneself only; explosives need a launch.
    {
        reset();
        const server::Stats s0 = rig.srv.stats();
        carol.s.SendHit(HitOn(cId, 30, proto::HitKind::Player, kFall, 0)); // a fall, no shot needed
        alice.s.SendHit(HitOn(bId, 30, proto::HitKind::Player, kFall, 0)); // someone else "falling"
        HitsJudged(rig, s0, 2);
        const server::Stats s1 = rig.srv.stats();
        Check("hit binding: a self-hit (a fall) needs no shot and is applied; a fall claimed against another player is refused",
              s1.hitsAccepted == s0.hitsAccepted + 1 && s1.hitsRejected == s0.hitsRejected + 1 && HealthOf(rig, cId) == 70 &&
                  HealthOf(rig, bId) == 100 && s1.hitsUnbound == s0.hitsUnbound + 1,
              std::to_string(HealthOf(rig, cId)) + "/" + std::to_string(HealthOf(rig, bId)));
        const server::Stats s2 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kGrenade);
        alice.s.SendHit(ShotHit(alice, bId, 40, kGrenade)); // a launch of Alice's
        carol.s.SendHit(HitOn(bId, 40, proto::HitKind::Player, kGrenade, 0)); // Carol never threw anything
        HitsJudged(rig, s2, 2);
        const server::Stats s3 = rig.srv.stats();
        Check("hit binding: an explosive Hit needs a launch of the shooter's within the last 15 s (Alice threw a grenade: applied; "
              "Carol did not: refused)",
              s3.hitsAccepted == s2.hitsAccepted + 1 && s3.hitsRejected == s2.hitsRejected + 1 && HealthOf(rig, bId) == 60,
              std::to_string(HealthOf(rig, bId)) + " hp");
    }

    // The operator's knobs: hitDamageScalePercent scales every cap, hitShotWindowMs 0 turns the shot requirement off (the caps stay).
    {
        Rig r2;
        server::Config c2 = Rig::BaseConfig();
        c2.hitDamageScalePercent = 10;
        c2.hitShotWindowMs = 0;
        if (!r2.Start(c2)) { Check("hit caps: server starts", false); return; }
        Probe a2(r2), b2(r2);
        if (!a2.Join("Ann") || !b2.Join("Ben") || !Place(r2, a2, Kin(0, 0, 30))) { Check("hit caps: players join", false); return; }
        const proto::VehicleResult sp = VCmd(r2, a2, proto::VehicleOp::Spawn, 0, kAdder);
        const bool known = r2.Until([&] { return a2.Vehicle(sp.vehId) != nullptr && r2.srv.FindVehicle(sp.vehId) != nullptr; }, 2000);
        const server::Stats s0 = r2.srv.stats();
        for (int i = 0; i < 12; i++) a2.s.SendHit(HitOn(sp.vehId, 160, proto::HitKind::Vehicle, kPistol, 0)); // no shot at all
        HitsJudged(r2, s0, 12);
        const server::Stats s1 = r2.srv.stats();
        const server::VehicleStore::Vehicle* v = r2.srv.FindVehicle(sp.vehId);
        const int taken = v ? 1000 - (int)v->s.bodyHealth : -1;
        Check("hit caps: hitDamageScalePercent 10 makes a pistol Hit worth 16 and the class budget 100 (50 a second): 12 Hits of 160 without any shot "
              "(binding off) do ~100 damage, not 1920, and the rest are refused as over budget",
              known && sp.code == proto::VehicleResultCode::Ok && s1.hitsAccepted - s0.hitsAccepted >= 6 && s1.hitsClamped > s0.hitsClamped &&
                  s1.hitsOverBudget > s0.hitsOverBudget && taken >= 90 && taken <= 130,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted, " + std::to_string(s1.hitsOverBudget - s0.hitsOverBudget) +
                  " over budget, " + std::to_string(taken) + " damage");
    }
}

// ===========================================================================
// v5 combat (W5): time-to-kill at the shipped defaults, melee swings, Damage for onlookers
// ===========================================================================

// BaseConfig() pins the v4 numbers (health 100, every percent 100) so the exact-number tests above keep passing; the
// tests below run on what the server ships with.
static server::Config ShippedCombatConfig()
{
    server::Config c = Rig::BaseConfig();
    const server::Config d; // the defaults in config.h
    c.playerHealth = d.playerHealth;
    c.firearmDamagePercent = d.firearmDamagePercent;
    c.meleeDamagePercent = d.meleeDamagePercent;
    c.explosiveDamagePercent = d.explosiveDamagePercent;
    c.runOverDamagePercent = d.runOverDamagePercent;
    c.headshotPercent = d.headshotPercent;
    c.respawnArmour = d.respawnArmour;
    c.maxHitDistance = 100;
    c.respawnDelayMs = 300;
    c.meleeAdjudicate = 0;   // R3 T6: these scenarios pin the client-claim path of 0.4.5; tests/r3_t6_tests.h (AdjConfig) turns the server's own judgement on
    return c;
}

static void CombatV5Tests()
{
    Check("combat v5: Config{} ships playerHealth 200",
          server::Config{}.playerHealth == 200, std::to_string(server::Config{}.playerHealth));

    Rig rig;
    if (!rig.Start(ShippedCombatConfig())) { Check("combat v5: server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig), erin(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol") || !erin.Join("Erin"))
    {
        Check("combat v5: four players join", false);
        return;
    }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob), cId = IdOf(carol), eId = IdOf(erin);
    const bool placed = Place(rig, alice, Kin(0, 0, 30)) && Place(rig, bob, Kin(2, 0, 30)) && Place(rig, carol, Kin(20, 0, 30)) &&
                        Place(rig, erin, Kin(1500, 0, 30));
    const bool relevant = rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(aId, cId) && rig.srv.IsRelevant(cId, bId); }, 3000);
    const bool far = !rig.srv.IsRelevant(aId, eId) && !rig.srv.IsRelevant(bId, eId);
    std::string out;
    auto lastDamage = [](const Probe& p) { const auto d = p.Damages(); return d.empty() ? proto::Damage() : d.back(); };
    auto damageCount = [](const Probe& p) { return p.Damages().size(); };
    auto reset = [&] {
        rig.srv.Execute("heal Bob", out);
        rig.srv.Execute("armor Bob 0", out);
        rig.Pump(80);
    };
    // Bob comes back after a kill (Respawn after the delay puts him at the spawn point: back to his place).
    auto revive = [&] {
        const int n = bob.Count(ET::RespawnOk);
        rig.Pump(400);
        bob.s.SendRespawn();
        const bool ok = rig.Until([&] { return bob.Count(ET::RespawnOk) > n; }, 2000);
        // The spawn point is far away: Bob left everybody's view and has to come back into it before a Hit on him is in view.
        return ok && Place(rig, bob, Kin(2, 0, 30)) &&
               rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(cId, bId); }, 3000);
    };
    // Hits until a Damage says killed (at most `cap`); returns how many Damages the victim got; `first` is the first one.
    auto killWith = [&](uint32_t weapon, uint16_t claimed, int cap, proto::Damage& first) {
        int n = 0;
        while (n < cap)
        {
            const size_t before = damageCount(bob);
            Fire(rig, alice, Kin(0, 0, 30), weapon);
            alice.s.SendHit(ShotHit(alice, bId, claimed, weapon));
            if (!rig.Until([&] { return damageCount(bob) > before; }, 2000)) break;
            if (n == 0) first = lastDamage(bob);
            n++;
            if (lastDamage(bob).killed) break;
        }
        return n;
    };

    // Time to kill: a carbine Hit of 32 (what the engine reports for one bullet) is worth 23 at 72 percent; 200 health take 9 hits.
    {
        proto::Damage first;
        const int hits = killWith(kRifle, 32, 20, first);
        Check("combat v5: at the shipped defaults (200 health, firearms at 72 percent) a carbine Hit of 32 is applied as 23 and 200 health take "
              "exactly 9 hits (v4: 4)",
              placed && relevant && lastDamage(bob).killed && hits == 9 && first.damage == 23,
              std::to_string(hits) + " hits, first applied " + std::to_string(first.damage));
    }

    // Fists: the swing (a shot-counter step) is what a melee Hit rides on; a claimed 30 is worth 15 at 50 percent: 14 punches.
    {
        const bool back = revive();
        proto::Damage first;
        const int punches = killWith(kUnarmed, 30, 30, first);
        Check("combat v5: at the shipped defaults a fist Hit with a swing behind it (claimed 30, melee 50 percent -> 15) takes 14 punches to kill "
              "200 health (v4: 4)",
              back && lastDamage(bob).killed && punches == 14 && first.damage == 15,
              std::to_string(punches) + " punches, first applied " + std::to_string(first.damage));
        if (!revive()) Check("combat v5: Bob comes back", false);
        rig.Pump(600); // every earlier swing is stale now
    }

    // W8 (live test 0.4.2, "a punch while running one-shots and kills"): whatever the client claims, a melee Hit is worth its weapon's
    // ceiling at the shipped 50 percent: fists 1000 -> 40 -> 20 (10 punches to kill 200 health; the class cap of 100 gave 50 per Hit and
    // 100 per swing = 2 swings), a bat 1000 -> 80 -> 40 (5 hits).
    {
        const bool back = true; // the Fists block above ended with Bob revived
        proto::Damage first;
        const int punches = killWith(kUnarmed, 1000, 30, first);
        const bool fistsOk = back && lastDamage(bob).killed && punches == 10 && first.damage == 20;
        const std::string fistGot = std::to_string(punches) + " punches, first applied " + std::to_string(first.damage);
        rig.Pump(600);
        const bool back2 = revive();
        proto::Damage firstBat;
        const int swings = killWith(server::ac::Joaat("WEAPON_BAT"), 1000, 30, firstBat);
        Check("combat W8: at the shipped defaults a fist Hit claiming 1000 is applied as 20 (ceiling 40 x 50 percent) and takes 10 punches to kill 200 health, a bat Hit "
              "claiming 1000 as 40 and 5 hits (before: 50 per Hit, 2 swings)",
              fistsOk && back2 && lastDamage(bob).killed && swings == 5 && firstBat.damage == 40,
              fistGot + "; bat " + std::to_string(swings) + " hits, first applied " + std::to_string(firstBat.damage));
        if (!revive()) Check("combat W8: Bob comes back", false);
        rig.Pump(600);
    }

    // A melee Hit with no swing behind it is refused (v4: anyone within 4 m could claim 300 damage a second without swinging).
    {
        reset();
        rig.Pump(1600); // 0.4.5: a melee Hit may name a swing up to 1500 ms old (ac::kMeleeShotWindowMs): every earlier swing must be stale first
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kUnarmed, 0); // fists out, no swing
        alice.s.SendHit(HitOn(bId, 30, proto::HitKind::Player, kUnarmed, alice.shotSeq));
        HitsJudged(rig, s0, 1);
        const server::Stats s1 = rig.srv.stats();
        Check("combat v5: a fist Hit with no swing (no shotSeq step in the shooter's states) is refused: nothing applied, Bob untouched (v4: accepted)",
              s1.hitsRejected == s0.hitsRejected + 1 && s1.hitsAccepted == s0.hitsAccepted && HealthOf(rig, bId) == 200,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted, " + std::to_string(HealthOf(rig, bId)) + " hp");
    }

    // One swing pays for two Hits (hitsPerShot 2: both hands of a combo, two targets), no more.
    {
        reset();
        const server::Stats s0 = rig.srv.stats();
        Fire(rig, alice, Kin(0, 0, 30), kUnarmed);
        for (int i = 0; i < 3; i++) alice.s.SendHit(ShotHit(alice, bId, 20, kUnarmed));
        HitsJudged(rig, s0, 3);
        const server::Stats s1 = rig.srv.stats();
        Check("combat v5: one melee swing is credited with 2 Hits; the third on the same swing is refused (v4: all three applied)",
              s1.hitsAccepted == s0.hitsAccepted + 2 && s1.hitsRejected == s0.hitsRejected + 1,
              std::to_string(s1.hitsAccepted - s0.hitsAccepted) + " accepted of 3");
    }

    // Damage reaches every player for whom the victim or the attacker is relevant, and nobody else.
    {
        reset();
        const size_t c0 = damageCount(carol), e0 = damageCount(erin);
        Fire(rig, alice, Kin(0, 0, 30));
        alice.s.SendHit(ShotHit(alice, bId, 30));
        const bool onlooker = rig.Until([&] { return damageCount(carol) > c0; }, 2000);
        rig.Pump(150);
        const proto::Damage dc = lastDamage(carol);
        Check("combat v5: Damage is also sent to a third player for whom the victim or the attacker is relevant (Carol at 20 m), not to one "
              "out of range of both (Erin at 1500 m)",
              far && onlooker && damageCount(carol) == c0 + 1 && damageCount(erin) == e0 && dc.victimId == bId && dc.attackerId == aId,
              std::to_string(damageCount(carol) - c0) + " for Carol, " + std::to_string(damageCount(erin) - e0) + " for Erin");
    }
}

static void MovementTests()
{
    // The movement check: displacement against server time, the untrusted window and what it costs.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.untrustedWindowMs = 3000;
        cfg.untrustedJoinGraceMs = 0;
        cfg.respawnDelayMs = 600;
        cfg.maxHitDistance = 100;
        if (!rig.Start(cfg)) { Check("movement: server starts", false); return; }
        Probe alice(rig), bob(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("movement: players join", false); return; }
        const uint16_t aId = IdOf(alice), bId = IdOf(bob);
        const bool placed = Place(rig, alice, Kin(0, 0, 30)) && Place(rig, bob, Kin(33, 0, 30));
        const bool relevant = rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(bId, aId); }, 3000);
        for (int i = 1; i <= 8; i++) // a walk at 5 m/s
        {
            alice.s.SendState(Kin(0.5f * (float)i, 0, 30));
            rig.Pump(100);
        }
        Place(rig, alice, Kin(4, 0, 30));
        const server::Stats w = rig.srv.stats();
        Check("movement: walking at 5 m/s for a second is not a jump (no untrusted window)",
              placed && relevant && w.movesRejected == 0 && !UntrustedOf(rig, aId), std::to_string(w.movesRejected) + " jumps");

        Place(rig, alice, Kin(30, 0, 30)); // a 26 m step in ~100 ms
        const bool open = UntrustedOf(rig, aId);
        const server::Stats s0 = rig.srv.stats();
        const proto::VehicleResult spawnRefused = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult bobSpawn = VCmd(rig, bob, proto::VehicleOp::Spawn, 0, kAdder); // 4 m ahead of Bob: (33, 4)
        rig.Until([&] { return rig.srv.FindVehicle(bobSpawn.vehId) != nullptr; }, 1000);
        rig.Pump(1050); // the RateLimited answer is sent at most once a second: wait out the first, still inside the window
        const proto::VehicleResult enterRefused = VCmd(rig, alice, proto::VehicleOp::Enter, bobSpawn.vehId, 1); // a passenger seat: v5 keeps another player's driver seat NotOwner
        Fire(rig, alice, Kin(30, 0, 30));
        alice.s.SendHit(ShotHit(alice, bId, 20));
        Fire(rig, bob, Kin(33, 0, 30));
        bob.s.SendHit(ShotHit(bob, aId, 20));
        HitsJudged(rig, s0, 2);
        const server::Stats s1 = rig.srv.stats();
        using C = proto::VehicleResultCode;
        Check("movement: a 26 m step in 0.1 s opens the untrusted window (counted, logged once): Alice's Spawn, Enter and Hit on Bob are refused "
              "(RateLimited / dropped) while Bob's Hit ON Alice still applies",
              open && s1.movesRejected == 1 && rig.LogHas("untrusted for") && spawnRefused.code == C::RateLimited &&
                  enterRefused.code == C::RateLimited && bobSpawn.code == C::Ok && s1.untrustedRefusals == s0.untrustedRefusals + 2 &&
                  s1.hitsUntrusted == s0.hitsUntrusted + 1 && HealthOf(rig, bId) == 100 && HealthOf(rig, aId) == 80,
              Code(spawnRefused) + " " + Code(enterRefused) + " alice " + std::to_string(HealthOf(rig, aId)) + " hp, " +
                  std::to_string(s1.untrustedRefusals) + " refusals");

        const bool closed = rig.Until([&] { return !UntrustedOf(rig, aId); }, 3500);
        const proto::VehicleResult spawnOk = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult enterOk = VCmd(rig, alice, proto::VehicleOp::Enter, bobSpawn.vehId, 1);
        VCmd(rig, alice, proto::VehicleOp::Exit, bobSpawn.vehId, 0);
        Check("movement: the window closes by itself (3 s here) and Alice may Spawn and Enter again",
              closed && spawnOk.code == C::Ok && enterOk.code == C::Ok, Code(spawnOk) + " " + Code(enterOk));

        // A driver's VehicleUpdate is judged the same way.
        const proto::VehicleResult enterOwn = VCmd(rig, alice, proto::VehicleOp::Enter, spawnOk.vehId, 0);
        alice.s.SendVehicleState(Drive(spawnOk.vehId, 30, 4, 0));
        rig.Pump(100);
        alice.s.SendVehicleState(Drive(spawnOk.vehId, 30, 304, 20)); // 300 m in ~0.1 s
        rig.Until([&] { return UntrustedOf(rig, aId); }, 1500);
        const server::Stats s2 = rig.srv.stats();
        Check("movement: a driver's VehicleUpdate 300 m from the last one in 0.1 s is taken but opens the window too",
              enterOwn.code == C::Ok && UntrustedOf(rig, aId) && s2.movesRejected == 2, std::to_string(s2.movesRejected) + " jumps");

        // The server's own teleport (RespawnOk) is not a jump, not even for the states still in flight from the old place.
        std::string killOut;
        rig.srv.Execute("kill Bob", killOut);
        rig.Pump(700);
        bob.s.SendRespawn();
        const bool ok = rig.Until([&] { return bob.Count(ET::RespawnOk) == 1; }, 2000);
        Place(rig, bob, Kin(33, 0, 30));                 // a state from before the respawn, arriving late
        Place(rig, bob, Kin(195.2f, -933.8f, 30.7f));    // the client arrived at the spawn point
        const server::Stats s3 = rig.srv.stats();
        Check("movement: a RespawnOk teleport (and states still in flight from the old place) is not a jump",
              ok && s3.movesRejected == 2 && !UntrustedOf(rig, bId), std::to_string(s3.movesRejected) + " jumps");
    }

    // Fuel drains with the DISPLACEMENT of the driven vehicle, not with the velocity the driver claims.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.untrustedWindowMs = 2000;
        cfg.untrustedJoinGraceMs = 0;
        cfg.fuelDrainPerMinute = 600; // 10 permille a second
        if (!rig.Start(cfg)) { Check("fuel: server starts", false); return; }
        Probe alice(rig);
        if (!alice.Join("Alice") || !Place(rig, alice, Kin(0, 0, 30))) { Check("fuel: player joins", false); return; }
        const proto::VehicleResult sp = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult in = VCmd(rig, alice, proto::VehicleOp::Enter, sp.vehId, 0);
        auto driveFor = [&](int ms, float step, float claimed) {
            const auto t0 = Clock::now();
            auto next = t0;
            float y = rig.srv.FindVehicle(sp.vehId) ? rig.srv.FindVehicle(sp.vehId)->s.k.y : 4.0f;
            while (MsSince(t0) < ms)
            {
                if (Clock::now() >= next)
                {
                    y += step;
                    alice.s.SendVehicleState(Drive(sp.vehId, 0, y, claimed));
                    next += std::chrono::milliseconds(50);
                }
                rig.Step();
            }
        };
        driveFor(2000, 0.0f, 100.0f); // claims 100 m/s, never moves
        const server::VehicleStore::Vehicle* v0 = rig.srv.FindVehicle(sp.vehId);
        const int fuelStill = v0 ? v0->s.fuelPermille : -1;
        driveFor(2000, 1.0f, 20.0f); // really moves 20 m/s
        const server::VehicleStore::Vehicle* v1 = rig.srv.FindVehicle(sp.vehId);
        const int fuelMoved = v1 ? v1->s.fuelPermille : -1;
        Check("movement: a driver who claims 100 m/s while not moving burns no fuel (1000 stays), one who really drives 20 m/s for 2 s burns ~20 permille",
              sp.code == proto::VehicleResultCode::Ok && in.code == proto::VehicleResultCode::Ok && fuelStill == 1000 && fuelMoved <= 990 &&
                  fuelMoved >= 950 && rig.srv.stats().movesRejected == 0,
              "still " + std::to_string(fuelStill) + ", moved " + std::to_string(fuelMoved));
    }
}

static void ReconnectTests()
{
    // Leaving takes the player's empty vehicles with it, orphans the occupied ones (still counted against the address),
    // and a reconnect does not start over with fresh caps.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.vehiclesPerPlayer = 3;
        cfg.vehiclesPerIp = 3;
        cfg.vehicleOrphanMs = 1000;
        if (!rig.Start(cfg)) { Check("reconnect: server starts", false); return; }
        Probe alice(rig), bob(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob") || !Place(rig, alice, Kin(0, 0, 30)) || !Place(rig, bob, Kin(2, 0, 30)))
        {
            Check("reconnect: players join", false);
            return;
        }
        using C = proto::VehicleResultCode;
        const proto::VehicleResult a1 = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult a2 = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult a3 = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult bIn = VCmd(rig, bob, proto::VehicleOp::Enter, a1.vehId, 1);
        alice.s.Disconnect();
        alice.WaitClosed();
        const bool emptiesGone = rig.Until([&] { return rig.srv.FindVehicle(a2.vehId) == nullptr && rig.srv.FindVehicle(a3.vehId) == nullptr; }, 3000);
        const bool removedSeen = rig.Until([&] { return bob.vehiclesRemoved.count(a2.vehId) && bob.vehiclesRemoved.count(a3.vehId); }, 2000);
        const server::VehicleStore::Vehicle* orphan = rig.srv.FindVehicle(a1.vehId);
        Check("reconnect: a leaver's EMPTY vehicles are destroyed with it (the others are told); the one Bob sits in stays as an orphan "
              "(no spawner)",
              a1.code == C::Ok && a2.code == C::Ok && a3.code == C::Ok && bIn.code == C::Ok && emptiesGone && removedSeen && orphan &&
                  orphan->orphan && orphan->spawner == proto::kNoPlayer && rig.srv.stats().vehiclesOrphaned == 1,
              Code(a1) + " " + Code(bIn));

        Probe alice2(rig);
        if (!alice2.Join("Alice2") || !Place(rig, alice2, Kin(0, 0, 30))) { Check("reconnect: second join", false); return; }
        const proto::VehicleResult n1 = VCmd(rig, alice2, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult n2 = VCmd(rig, alice2, proto::VehicleOp::Spawn, 0, kAdder);
        const proto::VehicleResult n3 = VCmd(rig, alice2, proto::VehicleOp::Spawn, 0, kAdder);
        Check("reconnect: the orphan still counts against the address (per-address cap 3): the reconnected player gets 2 vehicles, the 3rd is Limit",
              n1.code == C::Ok && n2.code == C::Ok && n3.code == C::Limit, Code(n1) + " " + Code(n2) + " " + Code(n3));

        VCmd(rig, bob, proto::VehicleOp::Exit, a1.vehId, 0);
        const auto t0 = Clock::now();
        const bool orphanGone = rig.Until([&] { return rig.srv.FindVehicle(a1.vehId) == nullptr; }, 4000);
        const int64_t ms = MsSince(t0);
        rig.Pump(1100); // the spawn bucket
        const proto::VehicleResult n4 = VCmd(rig, alice2, proto::VehicleOp::Spawn, 0, kAdder);
        Check("reconnect: an orphan goes vehicleOrphanSeconds (1 s here) after it empties, and the address gets its place back",
              orphanGone && ms >= 700 && ms <= 3500 && n4.code == C::Ok, std::to_string(ms) + " ms, " + Code(n4));
    }

    // The connect RATE per address, checked before any Hello work.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.connectsPerIpPerMinute = 3;
        if (!rig.Start(cfg)) { Check("connect rate: server starts", false); return; }
        Probe p1(rig), p2(rig), p3(rig), p4(rig);
        const bool three = p1.Join("P1") && p2.Join("P2") && p3.Join("P3");
        p4.Connect("P4");
        p4.WaitClosed();
        rig.Pump(100);
        const net::Event* ref = p4.First(ET::Refused);
        Check("connect rate: with 3 connects per minute allowed per address the 4th is refused TooManyConnections before Hello (the text names "
              "the limit; counted apart from the live-connection cap); the first three stay in",
              three && p4.RefusedWith(proto::RefuseReason::TooManyConnections) && ref && ref->refuse.text.find("per minute") != std::string::npos &&
                  !p4.First(ET::Welcome) && p1.Joined() && p2.Joined() && p3.Joined() && rig.srv.stats().refusedConnectRate == 1 &&
                  rig.srv.stats().refusedIpCap == 0,
              p4.Describe());
    }
}

static void VehicleAbuseTests()
{
    // RateLimited answers are throttled to one a second; a sustained flood is cut off.
    {
        Rig rig;
        if (!rig.Start()) { Check("vehicle flood: server starts", false); return; }
        Probe alice(rig), bob(rig);
        if (!alice.Join("Alice") || !bob.Join("Bob") || !Place(rig, alice, Kin(0, 0, 30))) { Check("vehicle flood: players join", false); return; }
        const server::Stats s0 = rig.srv.stats();
        proto::VehicleCmd c;
        c.op = proto::VehicleOp::Exit;
        c.vehId = 0;
        for (int i = 0; i < 40; i++) alice.s.SendVehicleCmd(c);
        rig.Until([&] { return rig.srv.stats().vehicleCmds >= s0.vehicleCmds + 40; }, 3000);
        rig.Pump(200);
        int limited = 0;
        for (const proto::VehicleResult& r : alice.Results())
            if (r.code == proto::VehicleResultCode::RateLimited) limited++;
        const server::Stats s1 = rig.srv.stats();
        Check("vehicle flood: of 40 VehicleCmds in a burst (10 a second allowed) the over-limit ones are answered RateLimited at most once a "
              "second (<= 2 here), the rest are not answered at all (counted), and nobody is disconnected",
              limited >= 1 && limited <= 2 && s1.vehicleRepliesSuppressed >= s0.vehicleRepliesSuppressed + 15 && alice.Joined() &&
                  s1.vehicleFloodKicks == 0,
              std::to_string(limited) + " RateLimited replies, " + std::to_string(s1.vehicleRepliesSuppressed - s0.vehicleRepliesSuppressed) + " suppressed");
        for (int i = 0; i < 600; i++)
        {
            if (!alice.s.SendVehicleCmd(c)) break;
            if (i % 40 == 39) rig.Step();
        }
        const bool gone = alice.WaitClosed(4000);
        Check("vehicle flood: 600 more commands far over the limit get the sender cut off (Protocol) and counted; others are unaffected",
              gone && rig.srv.stats().vehicleFloodKicks == 1 && bob.Joined() && rig.LogHas("VehicleCmd flood"), alice.Describe());
    }

    // Register: spacing from networked vehicles and a cooldown; Spawn / Register honour the model lists.
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.registerSeparationMeters = 4;
        cfg.registerCooldownMs = 1200;
        cfg.vehicleModelDeny = {kAdder};
        if (!rig.Start(cfg)) { Check("register rules: server starts", false); return; }
        Probe dave(rig);
        if (!dave.Join("Dave") || !Place(rig, dave, Kin(-500, 200, 30))) { Check("register rules: player joins", false); return; }
        using C = proto::VehicleResultCode;
        auto reg = [&](uint32_t model, float x, float y) {
            return VCmd(rig, dave, proto::VehicleOp::Register, 0, model, [&](proto::VehicleCmd& c) {
                c.x = x;
                c.y = y;
                c.z = 30.0f;
                c.rot = proto::QuatFromHeading(0.0f);
                c.seat = 0;
            });
        };
        const proto::VehicleResult denied = reg(kAdder, -498, 200);
        const proto::VehicleResult deniedSpawn = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        const server::Stats s0 = rig.srv.stats();
        const proto::VehicleResult r1 = reg(kSultan, -498, 200);
        const proto::VehicleResult tooClose = reg(kSultan, -496, 200); // 2 m from r1
        const proto::VehicleResult tooSoon = reg(kSultan, -490, 200);  // 8 m away, but right after the last
        rig.Pump(1300);
        const proto::VehicleResult r2 = reg(kSultan, -490, 200);
        const server::Stats s1 = rig.srv.stats();
        Check("register rules: Register is refused within 4 m of a networked vehicle (BadArg) and within the cooldown (RateLimited, 1.2 s here), "
              "allowed otherwise; a denied model is NotAllowed for Spawn and Register",
              denied.code == C::NotAllowed && deniedSpawn.code == C::NotAllowed && r1.code == C::Ok && tooClose.code == C::BadArg &&
                  tooSoon.code == C::RateLimited && r2.code == C::Ok && s1.registersRefused == s0.registersRefused + 2 && s1.modelsDenied == 2,
              Code(denied) + " " + Code(deniedSpawn) + " " + Code(r1) + " " + Code(tooClose) + " " + Code(tooSoon) + " " + Code(r2));
    }
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.vehicleModelAllow = {kSultan};
        cfg.vehicleModelDeny = {kSultan}; // the allow list wins
        if (!rig.Start(cfg)) { Check("model lists: server starts", false); return; }
        Probe dave(rig);
        if (!dave.Join("Dave") || !Place(rig, dave, Kin(0, 0, 30))) { Check("model lists: player joins", false); return; }
        const proto::VehicleResult ok = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kSultan);
        const proto::VehicleResult no = VCmd(rig, dave, proto::VehicleOp::Spawn, 0, kAdder);
        Check("model lists: with an allow list only its models may be spawned, and it wins over the deny list",
              ok.code == proto::VehicleResultCode::Ok && no.code == proto::VehicleResultCode::NotAllowed, Code(ok) + " " + Code(no));
    }
}

static void AnimAuthorityTests()
{
    Rig rig;
    if (!rig.Start()) { Check("anim rules: server starts", false); return; }
    Probe alice(rig), bob(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob")) { Check("anim rules: players join", false); return; }
    proto::Anim a = Smoke();
    a.flags = 0xFFFFFFFFu; // every bit, the dangerous ones included
    a.playbackRate = 255;
    alice.s.SendAnim(a);
    const bool got = rig.Until([&] { return bob.Count(ET::PeerAnim) == 1; }, 2000);
    const net::Event* pa = bob.First(ET::PeerAnim);
    Probe carol(rig);
    carol.Join("Carol");
    const bool replay = rig.Until([&] { return carol.Count(ET::PeerAnim) == 1; }, 2000);
    const net::Event* pc = carol.First(ET::PeerAnim);
    Check("anim rules: PlayAnim flags are reduced to the allow-list (bits 0..8) and the playback rate to 0.10x..2.00x, for the relay and for "
          "the replay to a newcomer; the rest of the animation is untouched",
          got && pa && pa->anim.anim.flags == 0x1FFu && pa->anim.anim.playbackRate == 200 && pa->anim.anim.dict == a.dict && replay && pc &&
              pc->anim.anim.flags == 0x1FFu && pc->anim.anim.playbackRate == 200 && rig.srv.stats().animsSanitized == 1,
          got && pa ? std::to_string(pa->anim.anim.flags) + "/" + std::to_string(pa->anim.anim.playbackRate) : std::string("not relayed"));
    rig.Pump(300);

    const server::Stats s0 = rig.srv.stats();
    std::string out;
    rig.srv.Execute("kill Alice", out);
    rig.Pump(200);
    const int before = bob.Count(ET::PeerAnim) + bob.Count(ET::PeerScenario);
    alice.s.SendAnim(Smoke());
    alice.s.SendScenario("WORLD_HUMAN_SMOKING");
    rig.Until([&] { return rig.srv.stats().animsDeadDropped >= s0.animsDeadDropped + 2; }, 2000);
    rig.Pump(100);
    Check("anim rules: a dead player's PlayAnim and PlayScenario are dropped (counted), nothing reaches the others",
          rig.srv.stats().animsDeadDropped == s0.animsDeadDropped + 2 && bob.Count(ET::PeerAnim) + bob.Count(ET::PeerScenario) == before &&
              alice.Joined());
}


// ===========================================================================
// 0.4.5 melee audit: scripted fights, the client's real melee pipeline (tests/melee_sim.h) into the real server
// ===========================================================================

// Drives one attacker: the sim client's frames every 16 ms (the script's presses and blows on the wall clock), its PedState
// (weapon, shotSeq, kPfMelee) every 22 ms, and its Hits straight into the session as the engine's flush does.
static std::string F2(const char* fmt, ...)
{
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

struct FightRun
{
    Rig& rig;
    Probe& atk;
    uint16_t victim;
    meleesim::Client c;
    FightRun(Rig& r, Probe& a, uint16_t v, uint32_t weapon, bool melee = true) : rig(r), atk(a), victim(v)
    {
        c.target = v;
        c.weapon = weapon;
        c.held = melee;
        c.shotSeq = a.shotSeq;
        c.sink = [this](const sync::BatchedHit& h) {
            proto::Hit x;
            x.kind = (proto::HitKind)h.kind;
            x.targetId = h.target;
            x.weapon = h.weapon;
            x.damage = h.damage;
            x.bone = h.bone;
            x.shotSeq = h.shotSeq;
            atk.s.SendHit(x);
        };
    }
    struct Blow { double t; int pool; bool event; };
    std::vector<double> presses;        // ms from the start
    std::vector<Blow> blows;
    std::vector<double> shots;          // gun shots (each also drops the puppet's pool by 30 at the same time)
    double actionMs = 350.0;
    bool dead = false;                  // the attacker keeps sending states
    double nextFrame = 0.0, nextState = 0.0;
    size_t pi = 0, bi = 0, si = 0;
    // One slice of the script at `now` ms from the start: the state every 22 ms, the client's frame every 16 ms.
    void Tick(double now)
    {
        if (now >= nextState)
        {
            proto::PedState ped;
            ped.weapon = c.weapon;
            ped.shotSeq = (uint8_t)(c.shotSeq & 15);
            if (c.actionNow) ped.flags |= proto::kPfMelee;
            atk.s.SendState(Kin(0, 0, 30), ped);
            nextState = now + 22.0;
        }
        if (now >= nextFrame)
        {
            bool press = false, action = false;
            while (pi < presses.size() && presses[pi] <= now) { press = true; ++pi; }
            for (double p : presses)
                if (actionMs > 0.0 && now >= p && now < p + actionMs) action = true;
            bool shot = false;
            while (si < shots.size() && shots[si] <= now) { shot = true; ++si; }
            if (shot) c.Shot();
            while (bi < blows.size() && blows[bi].t <= now) { c.pup.Blow(blows[bi].pool, blows[bi].event); ++bi; }
            if (shot) c.pup.Blow(30, true);
            c.Frame(now, press, action);
            nextFrame = now + 16.0;
        }
    }
    // Runs `endMs` of wall time (alone).
    void Run(double endMs);
};

// Runs several attackers on one clock for `endMs`.
static void RunFights(Rig& rig, std::initializer_list<FightRun*> fights, double endMs)
{
    const auto t0 = Clock::now();
    for (double now = 0.0; now < endMs; now = std::chrono::duration<double, std::milli>(Clock::now() - t0).count())
    {
        for (FightRun* f : fights) f->Tick(now);
        rig.Step();
    }
    for (FightRun* f : fights) f->atk.shotSeq = (uint8_t)(f->c.shotSeq & 15);
}
void FightRun::Run(double endMs) { RunFights(rig, { this }, endMs); }

static void MeleeFightSimTests()
{
    Rig rig;
    if (!rig.Start(ShippedCombatConfig())) { Check("0.4.5 melee sim: server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig), dave(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol") || !dave.Join("Dave"))
    {
        Check("0.4.5 melee sim: four players join", false);
        return;
    }
    const uint16_t aId = IdOf(alice), bId = IdOf(bob), cId = IdOf(carol), dId = IdOf(dave);
    const bool placed = Place(rig, alice, Kin(0, 0, 30)) && Place(rig, bob, Kin(2, 0, 30)) && Place(rig, carol, Kin(20, 0, 30)) &&
                        Place(rig, dave, Kin(1, 1, 30));
    const bool relevant = rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(cId, bId) && rig.srv.IsRelevant(dId, bId); }, 3000);
    if (!placed || !relevant) { Check("0.4.5 melee sim: players placed and relevant", false); return; }
    std::string out;
    auto reset = [&] {
        rig.srv.Execute("heal Bob", out);
        rig.srv.Execute("armor Bob 0", out);
        rig.Pump(80);
    };
    const uint32_t kBatW = server::ac::Joaat("WEAPON_BAT");
    // Damages Bob took (from `from`), their sum, and the attackers
    auto dmgs = [&](size_t from) { std::vector<proto::Damage> d = bob.Damages(); d.erase(d.begin(), d.begin() + (long)std::min(from, d.size())); return d; };
    auto sum = [](const std::vector<proto::Damage>& v) { int n = 0; for (const proto::Damage& d : v) n += d.damage; return n; };
    auto refusals = [&](server::HitReason r) { return rig.srv.stats().hitRefusals[(size_t)r]; };

    // 1. one punch: claimed 40 -> 20 after the shipped 50 percent; Bob, the attacker and the onlooker (Carol at 20 m) each hear it once
    {
        reset();
        const size_t b0 = bob.Damages().size(), a0 = alice.Damages().size(), c0 = carol.Damages().size();
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 150 };
        f.blows = { { 450, 0, true } };
        f.Run(1300);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: ONE punch (the client's real pipeline) is exactly one Damage of 20 on Bob (claimed 40 x 50%), Bob at 180, the attacker and the onlooker told once each",
              d.size() == 1 && d[0].damage == 20 && d[0].attackerId == aId && (d[0].flags & proto::kDmgMelee) && HealthOf(rig, bId) == 180 &&
                  alice.Damages().size() == a0 + 1 && carol.Damages().size() == c0 + 1,
              F2("%zu damages, %d, bob %d, alice +%zu carol +%zu", d.size(), sum(d), HealthOf(rig, bId), alice.Damages().size() - a0, carol.Damages().size() - c0));
    }
    // 2. a combo of three punches 0.5 s apart: three Damages of 20, none lost, none doubled
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 100, 600, 1100 };
        f.blows = { { 400, 0, true }, { 900, 0, true }, { 1400, 0, true } };
        f.Run(2300);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: a combo of 3 punches 0.5 s apart is three Damages of 20 (60 off Bob: 140), none lost and none doubled",
              d.size() == 3 && sum(d) == 60 && HealthOf(rig, bId) == 140, F2("%zu damages, %d, bob %d", d.size(), sum(d), HealthOf(rig, bId)));
    }
    // 3. a swing that misses, then one that hits: one Damage
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 100, 800 };
        f.blows = { { 1100, 0, true } };
        f.Run(1900);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: a swing that misses and a swing that hits is one Damage of 20", d.size() == 1 && d[0].damage == 20, F2("%zu damages", d.size()));
    }
    // 4. a late blow: 700 ms after the press (inside the client's 900 ms window): the 500 ms shot window refused it as "no shot with that shotSeq"
    {
        reset();
        const size_t b0 = bob.Damages().size();
        const server::Stats s0 = rig.srv.stats();
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 100 };
        f.blows = { { 800, 0, true } };
        f.Run(1700);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        const server::Stats s1 = rig.srv.stats();
        Check("0.4.5 melee sim: a blow 700 ms after the press (heavy swing, bat wind-up, a laggy link) is one Damage of 20 through the melee swing window; it counts as a late melee hit "
              "and is not refused as 'no shot' (before: refused, nobody told)",
              d.size() == 1 && d[0].damage == 20 && s1.hitsMeleeLate == s0.hitsMeleeLate + 1 && refusals(server::HitReason::NoShot) == 0,
              F2("%zu damages, late %llu, no_shot %llu", d.size(), (unsigned long long)(s1.hitsMeleeLate - s0.hitsMeleeLate), (unsigned long long)refusals(server::HitReason::NoShot)));
    }
    // 5. the whole pipeline's stale case: a blow outside the window leaves an event; the next swing is a miss: Bob is not hurt
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 100, 2300 };
        f.blows = { { 1300, 0, true } };
        f.Run(3200);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: a swing that missed after a stale damage event hurts nobody (no ghost Hit): Bob untouched", d.empty() && HealthOf(rig, bId) == 200, F2("%zu damages", d.size()));
    }
    // 6. a bat: the client's event claim is the nominal 40 whatever the weapon, so the bat's 80 ceiling never shows: 20 per hit (design question, reported)
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f(rig, alice, bId, kBatW);
        f.presses = { 100 };
        f.blows = { { 500, 0, true } };
        f.Run(1300);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: a bat swing is one Damage inside the expected band 20..40 (today exactly 20: the event claims the fist nominal 40, the bat's server ceiling of 80 is never reached)",
              d.size() == 1 && d[0].damage >= 20 && d[0].damage <= 40 && d[0].weapon == kBatW, F2("%zu damages, first %d", d.size(), d.empty() ? 0 : d[0].damage));
    }
    // 7. two attackers punching Bob at once: two Damages, one per attacker, 20 each
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f1(rig, alice, bId, kUnarmed), f2(rig, dave, bId, kUnarmed);
        f1.presses = { 100 };
        f1.blows = { { 420, 0, true } };
        f2.presses = { 100 };
        f2.blows = { { 440, 0, true } };
        RunFights(rig, { &f1, &f2 }, 1000);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        bool both = d.size() == 2 && d[0].attackerId != d[1].attackerId && sum(d) == 40;
        Check("0.4.5 melee sim: two attackers each land one punch on Bob: two Damages of 20 from two different attackers (40 off, Bob at 160)",
              both && HealthOf(rig, bId) == 160, F2("%zu damages, %d, bob %d", d.size(), sum(d), HealthOf(rig, bId)));
    }
    // 8. a punch at a dead target is refused as 'target_dead' and counted; after the respawn the next punch lands
    {
        reset();
        rig.srv.Execute("kill Bob", out);
        rig.Pump(100);
        const size_t b0 = bob.Damages().size();
        const uint64_t dead0 = refusals(server::HitReason::TargetDead);
        FightRun f(rig, alice, bId, kUnarmed);
        f.presses = { 100 };
        f.blows = { { 400, 0, true } };
        f.Run(1000);
        rig.Pump(200);
        const bool refused = refusals(server::HitReason::TargetDead) == dead0 + 1 && dmgs(b0).empty();
        const int n = bob.Count(ET::RespawnOk);
        rig.Pump(400);
        bob.s.SendRespawn();
        rig.Until([&] { return bob.Count(ET::RespawnOk) > n; }, 2000);
        Place(rig, bob, Kin(2, 0, 30));
        rig.Until([&] { return rig.srv.IsRelevant(aId, bId) && rig.srv.IsRelevant(cId, bId) && rig.srv.IsRelevant(dId, bId); }, 3000);
        const size_t b1 = bob.Damages().size();
        FightRun g(rig, alice, bId, kUnarmed);
        g.presses = { 100 };
        g.blows = { { 400, 0, true } };
        g.Run(1000);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b1);
        Check("0.4.5 melee sim: a punch at a dead target is refused (target_dead counted, no Damage); after the respawn the next punch is one Damage of 20 on the full 200",
              refused && d.size() == 1 && d[0].damage == 20 && HealthOf(rig, bId) == 180, F2("refused %d, %zu damages, bob %d", (int)refused, d.size(), HealthOf(rig, bId)));
    }
    // 9. guns are not broken: a carbine, 5 shots 120 ms apart, each shot takes 30 off the puppet's pool: five Damages of 22 (30 x 72%), 110 in all
    {
        reset();
        const size_t b0 = bob.Damages().size();
        FightRun f(rig, alice, bId, kRifle, false);
        f.shots = { 100, 220, 340, 460, 580 };
        f.Run(1500);
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: a carbine burst (5 shots 120 ms apart, 30 off the pool each) is five Damages of 22 (30 x 72%), 110 off Bob: guns are exactly once per shot",
              d.size() == 5 && sum(d) == 110 && HealthOf(rig, bId) == 90, F2("%zu damages, %d, bob %d", d.size(), sum(d), HealthOf(rig, bId)));
    }
    // 10. the reliable Hit overtakes the state that carries its swing (reorder): held, then applied exactly once
    {
        reset();
        const size_t b0 = bob.Damages().size();
        const server::Stats s0 = rig.srv.stats();
        alice.shotSeq = (uint8_t)((alice.shotSeq + 1) & 15);
        proto::Hit h = HitOn(bId, 40, proto::HitKind::Player, kUnarmed, alice.shotSeq);
        alice.s.SendHit(h);
        rig.Pump(100);
        proto::PedState ped;
        ped.weapon = kUnarmed;
        ped.shotSeq = alice.shotSeq;
        for (int i = 0; i < 4; ++i) { alice.s.SendState(Kin(0, 0, 30), ped); rig.Pump(30); }
        rig.Pump(200);
        const std::vector<proto::Damage> d = dmgs(b0);
        const server::Stats s1 = rig.srv.stats();
        Check("0.4.5 melee sim: a punch's Hit that overtakes the state carrying its swing is held and applied exactly once (20)",
              d.size() == 1 && d[0].damage == 20 && s1.hitsDeferred == s0.hitsDeferred + 1, F2("%zu damages, deferred %llu", d.size(), (unsigned long long)(s1.hitsDeferred - s0.hitsDeferred)));
    }
    // 11. the same Hit delivered twice (a client bug or a replay): the slot credits one swing twice (hitsPerShot 2) and refuses the third; the per-swing worst case is 2 x 20
    {
        reset();
        const size_t b0 = bob.Damages().size();
        const uint64_t used0 = refusals(server::HitReason::ShotUsed);
        Fire(rig, alice, Kin(0, 0, 30), kUnarmed);
        for (int i = 0; i < 3; ++i) alice.s.SendHit(ShotHit(alice, bId, 40, kUnarmed));
        rig.Pump(300);
        const std::vector<proto::Damage> d = dmgs(b0);
        Check("0.4.5 melee sim: one swing delivered three identical Hits is two Damages (40 at most per punch, the server cannot tell a repeat from a second hand) and the third is refused as shot_used",
              d.size() == 2 && sum(d) == 40 && refusals(server::HitReason::ShotUsed) == used0 + 1, F2("%zu damages, %d", d.size(), sum(d)));
    }
    // 12. every refusal is counted by reason, in the 10 s `combat:` server line
    {
        rig.Pump(300);
        const server::Stats& s = rig.srv.stats();
        uint64_t byReason = 0;
        for (size_t i = 0; i < server::kHitReasons; ++i) byReason += s.hitRefusals[i];
        std::string combatLines; // the refusal part of each 10 s line the server logged, for the failure message
        for (const std::string& l : rig.logs)
            if (l.find("combat: last 10 s") != std::string::npos) combatLines += l.substr(l.find("refused")) + " | ";
        Check("0.4.5 melee sim: every refused Hit is counted in exactly one reason slot (the per-reason sum equals hitsRejected), and the server's 10 s `combat:` line carries the "
              "accepted / melee numbers and the reasons (target_dead=1 from the dead-target fight)",
              byReason == s.hitsRejected && rig.LogHas("combat: last 10 s: hits accepted") && rig.LogHas("target_dead=1") &&
                  s.hitsMelee >= 8 && s.hitsDamageApplied > 0,
              F2("%llu by reason, %llu rejected, %llu melee accepted; %s", (unsigned long long)byReason, (unsigned long long)s.hitsRejected, (unsigned long long)s.hitsMelee,
                 combatLines.c_str()));
    }
}

#define T6_HEALTH_NETWORK
#include "t6_health_tests.h"   // T6 health: the real server and session into the client's tracker and HUD (uses the helpers above)
#include "t1_seats_net_tests.h"   // T1 seats (wire v6): the three-client scenario against the real server
#include "t4_netcode_tests.h"
#define R3T6_NET
#include "r3_t6_tests.h"   // R3 T6 melee: the real server judging swings by itself (about 12 s)
#define R4T3_NET
#include "r4_t3_tests.h"   // R4 T3 punches: a swing at the spot where the friend is not (about 3 s)

int main()
{
    // Unbuffered, so the lines before a crash are not lost when stdout is a pipe.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = Clock::now();
    std::printf("-- codec\n");
    CodecTests();
    ResourceCodecTests();
    VehicleCodecTests();
    CodecV5Tests();
    LookCodecTests();
    std::printf("-- server config\n");
    ConfigTests();
    std::printf("-- network\n");
    HandshakeTests();
    RefusalTests();
    ServerFullTest();
    HelloTimeoutTest();
    ViolationTests();
    JoinLeaveTests();
    StateTests();
    InterestNetworkTests();
    LossTest();
    ChatTests();
    std::printf("-- on-foot state and appearance (v3)\n");
    PedNetworkTests();
    LookTests();
    LookRateTest();
    OutfitLookTest();   // 0.5.4 outfit_items
    std::printf("-- vehicles, combat and emotes (v4)\n");
    VehicleNetworkTests();
    VehicleLifetimeTests();
    CombatNetworkTests();
    AnimNetworkTests();
    std::printf("-- authority rules against modded clients (v4)\n");
    HitBindingTests();
    CombatV5Tests();
    std::printf("-- 0.4.5 melee audit: scripted fights, the client's melee pipeline into the real server\n");
    MeleeFightSimTests();
    MovementTests();
    ReconnectTests();
    VehicleAbuseTests();
    AnimAuthorityTests();
    t6health::NetworkTests();   // T6 health: Welcome.playerHealth / Hit / Damage / heal / kill / Respawn into the tracker and the HUD
    std::printf("-- T1 seats: enter / exit intents, three clients, the real server (wire v6)\n");
    t1net::SeatNetworkTests();
    std::printf("-- T4: vehicle collisions (VehicleImpact over the real server)\n");
    t4net::T4NetworkTests();
    std::printf("-- R3 T6: the server judges melee swings itself\n");
    r3t6::NetTests();
    r4t3::NetTests();
    std::printf("-- metrics and budget\n");
    MetricsTests();
    ShutdownTests();

    // A skipped section (an early return, a crash-free no-op) must fail, not pass vacuously.
    // v4: the 9 v3 vehicle-codec checks became 7 v4 codec checks; + 4 vehicle, 2 vehicle-lifetime, 8 combat and 4 emote
    // network checks; + 28 authority-rule checks (12 hit binding and caps, 6 movement and fuel, 4 reconnect and connect
    // rate, 4 flood / Register / model lists, 2 emote rules).
    const int expected = 195 + 1 + 12 + 1 /* 0.5.4 outfit look */ + t6health::kNetworkChecks + t1net::kNetChecks + t4net::kT4NetChecks + r3t6::kNetChecks + r4t3::kNetChecks; // 0.4.5 melee audit +12; W8 +1 melee ceiling; T6 health network checks; T1 seats (reworded checks, not added, + the three-client scenario and the wire rules); T4 collisions (+3: VehicleImpact wire, head-on arbitration, the speed clamp)
    std::printf("%d/%d passed  (%lld ms)\n", g_run - g_failed, g_run, (long long)MsSince(t0));
    if (g_run != expected)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
