// decode_fuzz_test: hostile input against EVERY protocol Decode (libs/protocol/protocol.h) and the UDP query v1/v2
// datagrams (libs/protocol/query.h), deterministic (a fixed xorshift seed), no sockets, no server. Portable C++17:
// it builds in the Windows tree and in the GAGE_SERVER_ONLY Linux tree (cmake/gage_server.cmake), and the checks are
// meant to be run once under -fsanitize=address,undefined as well (a bad read or a signed overflow aborts).
//
// For each message type a valid sample is encoded and then attacked:
//   truncation   every strict prefix, 0 .. n-1 bytes, is refused;
//   trailing     1 .. 9 extra bytes are refused;
//   bit flips    every single bit of the sample: no crash; an accepted result re-encodes to exactly the flipped
//                bytes (the format has one encoding per value: nothing a decoder accepts is lossy or ambiguous);
//   random       random bytes and 1..4-byte mutations (type byte kept): no crash, the same canonical rule;
//   refusal      a refused packet leaves `out` as it was (Decode's documented contract);
//   caps         a hostile count or length (u16 0xFFFF string, 255 removed ids, count 0xFFFF) is refused without
//                reserving for it (the sanitiser build would show an allocation of that size as a failure of the
//                bounded-memory check below: the decoders must refuse in well under a kilobyte of work).
// Exits non-zero on any failed check or a wrong check count.
#include "protocol/protocol.h"
#include "protocol/query.h"
#include "protocol/wire.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace gage;
using namespace gage::proto;

static int g_run = 0, g_failed = 0;

static void Check(const std::string& name, bool ok, const std::string& got = "")
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), got.empty() ? "" : "  got: ", got.c_str());
}

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }
};

static std::string Hex(const Bytes& b, size_t max = 40)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < b.size() && i < max; i++)
    {
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    if (b.size() > max) s += "...";
    return s;
}

// One message type under test: its valid sample, and a probe that decodes bytes and says (accepted, canonical,
// out-untouched-on-refusal).
struct Probe
{
    bool accepted = false;
    bool canonical = false;     // the accepted value re-encodes to exactly the input
    bool stable = false;        // ... or at least re-encodes to a packet that decodes and re-encodes to itself
    bool untouched = true;      // a refusal left the pre-set `out` equal to the sample
};
struct Case
{
    std::string name;
    Bytes valid;
    std::function<Probe(const Bytes&)> probe;
    // The smallest-three rotation (quant.h) is a quantiser: an arbitrary valid wire pattern decodes to a rotation
    // that re-encodes to a nearby pattern, so these types are held to "stable after one round trip", not "canonical".
    bool lossy = false;
};

template <class M>
static Case Make(const char* name, const M& sample)
{
    Case c;
    c.name = name;
    c.valid = Encode(sample);
    const Bytes sampleBytes = c.valid;
    c.probe = [sample, sampleBytes](const Bytes& in) {
        Probe pr;
        M out = sample;                                   // pre-set to the sample: a refusal must leave it alone
        pr.accepted = Decode(in.data(), in.size(), out);
        const Bytes re = Encode(out);
        if (pr.accepted)
        {
            pr.canonical = re == in;
            M again = sample;
            pr.stable = !re.empty() && Decode(re.data(), re.size(), again) && Encode(again) == re;
        }
        else
            pr.untouched = re == sampleBytes;
        return pr;
    };
    return c;
}

static Kinematics Kin(float x, float y, float z)
{
    Kinematics k;
    k.x = x;
    k.y = y;
    k.z = z;
    k.heading = 90.0f;
    k.vx = 1.5f;
    k.vy = -2.0f;
    k.vz = 0.25f;
    return k;
}

static Anim SampleAnim()
{
    Anim a;
    a.dict = "amb@world_human_hang_out_street@male_c@idle_a";
    a.name = "idle_b";
    a.blendIn = 4.0f;
    a.blendOut = -4.0f;
    a.durationMs = 5000;
    a.flags = 1 | 32;
    a.playbackRate = 100;
    return a;
}

static Look SampleLook()
{
    Look l;
    l.model = 0x705E61F2u;
    for (size_t i = 0; i < kLookComponents; i++)
    {
        l.components[i].drawable = (uint16_t)(i * 7);
        l.components[i].texture = (uint8_t)(i % 5);
        l.components[i].palette = (uint8_t)(i % 4);
    }
    for (size_t i = 0; i < kLookProps; i++)
    {
        l.props[i].drawable = i % 2 ? (int16_t)-1 : (int16_t)(i + 1);
        l.props[i].texture = i % 2 ? 0 : 2;
    }
    l.head.shape1 = 3;
    l.head.shape2 = 9;
    l.head.skin1 = 4;
    l.head.shapeMix = 128;
    l.head.skinMix = 64;
    return l;
}

static std::vector<Case> Corpus()
{
    std::vector<Case> v;
    { Hello m; m.name = "Alice_Smith"; v.push_back(Make("Hello", m)); }
    { Welcome m; m.yourId = 7; m.tickHz = 30; m.serverName = "SOL"; m.playerHealth = 200; v.push_back(Make("Welcome", m)); }
    { Refuse m; m.reason = RefuseReason::VersionMismatch; m.text = kRefuseTextClientOld; v.push_back(Make("Refuse", m)); }
    { PlayerJoined m; m.id = 12; m.name = "Bob"; v.push_back(Make("PlayerJoined", m)); }
    { PlayerLeft m; m.id = 12; m.reason = LeaveReason::Kicked; v.push_back(Make("PlayerLeft", m)); }
    {
        PlayerState m;
        m.seq = 123456;
        m.k = Kin(-1234.5f, 2210.25f, 31.5f);
        m.ack.tick = 900;
        m.ack.bits = 0xF0F0AAu;
        m.ped.move = kMoveRun;
        m.ped.flags = kPfAiming | kPfShooting;
        m.ped.weapon = 0x1B06D571u;
        m.ped.aimX = 3.0f;
        m.ped.aimY = -1.5f;
        m.ped.aimZ = 0.5f;
        m.ped.shotSeq = 5;
        m.sampleMs = 40000;
        v.push_back(Make("PlayerState aiming", m));
        PlayerState e = m;
        e.ped.move = kMoveEnteringVehicle;
        e.ped.flags = kPfCover;
        e.ped.vehId = 9;
        e.ped.aimX = e.ped.aimY = e.ped.aimZ = 0;
        v.push_back(Make("PlayerState entering", e));
    }
    { ChatSend m; m.text = "hello world, this is a chat line"; v.push_back(Make("ChatSend", m)); }
    { ChatMessage m; m.fromId = 3; m.text = "gg"; v.push_back(Make("ChatMessage", m)); }
    {
        CoarseState m;
        m.tick = 77;
        for (uint16_t i = 1; i <= 5; i++) m.players.push_back({(uint16_t)(i * 3), i * 100.0f, -i * 50.0f, 20.0f});
        v.push_back(Make("WorldCoarse", m));
    }
    {
        ResourceManifest m;
        for (uint32_t i = 0; i < 3; i++)
        {
            ResourceInfo r;
            r.name = "logo" + std::to_string(i) + ".png";
            r.size = 1000 + i;
            r.crc32 = 0xDEADBEEFu + i;
            m.resources.push_back(r);
        }
        v.push_back(Make("ResourceManifest", m));
    }
    { ResourceRequest m; m.index = 2; m.startChunk = 17; v.push_back(Make("ResourceRequest", m)); }
    {
        ResourceChunk m;
        m.index = 1;
        m.chunkIndex = 4;
        for (int i = 0; i < 100; i++) m.data.push_back((uint8_t)(i * 3));
        v.push_back(Make("ResourceChunk", m));
    }
    {
        VehicleWorld m;
        m.tick = 4242;
        m.removed = {3, 7};
        m.present = {1, 2, 5, 9};
        VehicleState a;
        a.id = 1;
        a.model = 0x4BFCF28Bu;
        a.k = Kin(10.0f, 20.0f, 30.0f);
        a.k.heading = 0;
        a.rot = Quat{0.0f, 0.0f, 0.7071068f, 0.7071068f};
        a.avz = 0.5f;
        a.steer = -0.25f;
        a.flags = kVehEngine | kVehLights;
        a.bodyHealth = 900;
        a.engineHealth = 850;
        a.fuelPermille = 700;
        a.colour1 = 12;
        a.colour2 = 34;
        a.seats[0] = 4;
        a.seats[2] = 6;
        a.ageMs = 20;
        a.ctrl = 4;
        a.ownerId = 4;
        a.flags2 = kVeh2Headlights | kVeh2Brake;
        a.doors = kDoorHood;
        a.tyres = 0x35;        // v6: slots 0, 2, 4, 5 burst
        a.tyresRim = 0x14;     // ... 2 and 4 off the rim
        a.windows = 0xA1;
        VehicleState b = a;
        b.id = 5;
        b.motionOnly = true;
        m.vehicles = {a, b};
        v.push_back(Make("VehicleWorld", m));
        // R3 T4 (v7): the same frame with a damage block on the full entry (petrol, a broken door and a lamp, three dents) and on a second full one (max)
        m.vehicles[0].dmg.petrol = 99;
        m.vehicles[0].dmg.parts = kDoorHood | kVehDmgLampR;
        m.vehicles[0].dmg.dent[1] = 7;
        m.vehicles[0].dmg.dent[4] = 250;
        m.vehicles[0].dmg.dent[7] = 1;
        v.push_back(Make("VehicleWorld damage", m));
    }
    {
        VehicleCmd m;
        m.vehId = 4;
        m.op = VehicleOp::Enter;
        m.arg = 2;
        v.push_back(Make("VehicleCmd enter", m));
        VehicleCmd r;
        r.vehId = 0;
        r.op = VehicleOp::Register;
        r.arg = 0x4BFCF28Bu;
        r.x = 100.0f;
        r.y = -200.0f;
        r.z = 30.0f;
        r.rot = Quat{0.0f, 0.0f, 0.0f, 1.0f};
        r.colour1 = 1;
        r.colour2 = 2;
        r.seat = 0;
        v.push_back(Make("VehicleCmd register", r));
    }
    { PlayerLook m; m.look = SampleLook(); v.push_back(Make("PlayerLook", m)); }
    { PeerLook m; m.id = 8; m.look = SampleLook(); v.push_back(Make("PeerLook", m)); }
    {
        VehicleUpdate m;
        m.seq = 55;
        m.vehId = 3;
        m.k = Kin(50.0f, 60.0f, 70.0f);
        m.k.heading = 0;
        m.rot = Quat{0.0f, 0.0f, 0.0f, 1.0f};
        m.avy = 1.0f;
        m.steer = 0.5f;
        m.flags = kVehEngine;
        m.bodyHealth = 1000;
        m.engineHealth = 1000;
        m.sampleMs = 12345;
        m.flags2 = kVeh2IndicatorL | kVeh2Handbrake;
        m.doors = kDoorTrunk;
        m.tyres = 0x0E;        // v6
        m.tyresRim = 0x06;
        m.windows = 0x11;
        v.push_back(Make("VehicleUpdate", m));
        // R3 T4 (v7): with the damage block (flags bit 5 on the wire): two dents, a broken door and headlight, a leaking tank
        m.hasDmg = true;
        m.dmg.petrol = 61;
        m.dmg.parts = kDoorFrontLeft | kVehDmgLampL;
        m.dmg.dent[0] = 200;
        m.dmg.dent[5] = 3;
        v.push_back(Make("VehicleUpdate damage", m));
        m.dmg = VehicleDamage();
        v.push_back(Make("VehicleUpdate clean block", m));
    }
    { VehicleResult m; m.op = VehicleOp::Enter; m.code = VehicleResultCode::SeatTaken; m.vehId = 6; v.push_back(Make("VehicleResult", m)); }
    { Hit m; m.kind = HitKind::Player; m.targetId = 5; m.weapon = 0x1B06D571u; m.damage = 40; m.bone = 31086; m.shotSeq = 3; v.push_back(Make("Hit", m)); }
    {
        Damage m;
        m.victimId = 5;
        m.attackerId = 9;
        m.weapon = 0x1B06D571u;
        m.damage = 40;
        m.health = 160;
        m.armor = 20;
        m.killed = false;
        m.bone = -1;
        m.flags = kDmgMelee | kDmgArmour;
        v.push_back(Make("Damage", m));
    }
    { Killed m; m.victimId = 5; m.killerId = 9; m.weapon = 0x1B06D571u; m.flags = kKilledHeadshot; v.push_back(Make("Killed", m)); }
    { v.push_back(Make("Respawn", Respawn())); }
    { RespawnOk m; m.x = 10.0f; m.y = 20.0f; m.z = 30.0f; m.heading = 180.0f; m.health = 200; m.armor = 50; v.push_back(Make("RespawnOk", m)); }
    { PlayAnim m; m.anim = SampleAnim(); v.push_back(Make("PlayAnim", m)); }
    { PlayScenario m; m.name = "WORLD_HUMAN_SMOKING"; v.push_back(Make("PlayScenario", m)); }
    { v.push_back(Make("StopAnim", StopAnim())); }
    { PeerAnim m; m.id = 4; m.anim = SampleAnim(); v.push_back(Make("PeerAnim", m)); }
    { PeerScenario m; m.id = 4; m.name = "WORLD_HUMAN_SMOKING"; v.push_back(Make("PeerScenario", m)); }
    { PeerAnimStop m; m.id = 4; v.push_back(Make("PeerAnimStop", m)); }
    // T1 seats (wire v6): one case per SeatNotice kind (each kind has its own detail rule: Reserved bit 0 only, Cancelled a reason, the
    // others 0), and the two new VehicleCmd ops (their arg is validated by the server, not the codec).
    { SeatNotice m; m.kind = SeatNoticeKind::Reserved; m.player = 7; m.vehId = 12; m.seat = 3; m.detail = kSeatNoticeRun; v.push_back(Make("SeatNotice reserved", m)); }
    { SeatNotice m; m.kind = SeatNoticeKind::Seated; m.player = 7; m.vehId = 12; m.seat = 3; v.push_back(Make("SeatNotice seated", m)); }
    { SeatNotice m; m.kind = SeatNoticeKind::Cancelled; m.player = 7; m.vehId = 12; m.seat = 3; m.detail = (uint8_t)SeatCancelReason::Expired; v.push_back(Make("SeatNotice cancelled", m)); }
    { SeatNotice m; m.kind = SeatNoticeKind::Exited; m.player = 7; m.vehId = 12; m.seat = 3; v.push_back(Make("SeatNotice exited", m)); }
    { VehicleCmd m; m.vehId = 12; m.op = VehicleOp::EnterIntent; m.arg = 3u | kIntentRun; v.push_back(Make("VehicleCmd enter intent", m)); }
    { VehicleCmd m; m.vehId = 12; m.op = VehicleOp::CancelIntent; v.push_back(Make("VehicleCmd cancel intent", m)); }
    {   // T4: the server's resolution of a vehicle contact, for one controller
        VehicleImpact m;
        m.seq = 4242; m.vehId = 3; m.otherId = 17; m.sampleMs = 54321;
        m.dvx = -12.5f; m.dvy = 24.5f; m.dyaw = -2.25f; m.nx = 0.5f; m.ny = -0.75f; m.closing = 38.5f; m.depth = 0.31f;
        v.push_back(Make("VehicleImpact", m));
    }
    for (Case& c : v)
        if (c.name.rfind("VehicleWorld", 0) == 0 || c.name.rfind("VehicleUpdate", 0) == 0 || c.name == "VehicleCmd register") c.lossy = true;
    return v;
}

// Runs the attacks on one case; every failure is reported once per attack kind with the first offender.
static void Attack(const Case& c)
{
    const std::string n = c.name;
    if (c.valid.empty())
    {
        Check(n + ": the valid sample encodes", false);
        return;
    }
    const Probe ok = c.probe(c.valid);
    Check(n + ": the valid sample decodes and re-encodes to the same bytes", ok.accepted && ok.canonical, Hex(c.valid));
    auto good = [&](const Probe& pr) { return c.lossy ? pr.stable : pr.canonical; };

    // 1. Truncation: every strict prefix.
    {
        std::string bad;
        for (size_t len = 0; len < c.valid.size() && bad.empty(); len++)
        {
            Bytes p(c.valid.begin(), c.valid.begin() + (long)len);
            const Probe pr = c.probe(p);
            if (pr.accepted) bad = "prefix of " + std::to_string(len) + " bytes accepted";
            else if (!pr.untouched) bad = "prefix of " + std::to_string(len) + " bytes refused but changed out";
        }
        Check(n + ": every strict prefix is refused and leaves `out` alone", bad.empty(), bad);
    }
    // 2. Trailing bytes.
    {
        std::string bad;
        for (size_t extra = 1; extra <= 9 && bad.empty(); extra++)
        {
            Bytes p = c.valid;
            for (size_t i = 0; i < extra; i++) p.push_back((uint8_t)(0xA5 + i));
            const Probe pr = c.probe(p);
            if (pr.accepted) bad = std::to_string(extra) + " trailing byte(s) accepted";
            else if (!pr.untouched) bad = std::to_string(extra) + " trailing byte(s) refused but changed out";
        }
        Check(n + ": trailing bytes are refused", bad.empty(), bad);
    }
    // 3. Every single-bit flip.
    {
        std::string bad;
        size_t accepted = 0;
        for (size_t bit = 0; bit < c.valid.size() * 8 && bad.empty(); bit++)
        {
            Bytes p = c.valid;
            p[bit / 8] = (uint8_t)(p[bit / 8] ^ (1u << (bit % 8)));
            const Probe pr = c.probe(p);
            if (pr.accepted)
            {
                accepted++;
                if (!good(pr)) bad = "bit " + std::to_string(bit) + " accepted but not " + (c.lossy ? "stable" : "canonical") + ": " + Hex(p);
            }
            else if (!pr.untouched)
                bad = "bit " + std::to_string(bit) + " refused but changed out";
        }
        Check(n + ": every single-bit flip is refused or decodes canonically (" + std::to_string(accepted) + " accepted)", bad.empty(), bad);
    }
    // 4. Random mutations (type byte kept) and random tails.
    {
        Rng rng(0xFEED0000u + std::hash<std::string>()(n));
        std::string bad;
        size_t accepted = 0;
        const int rounds = 6000;
        for (int r = 0; r < rounds && bad.empty(); r++)
        {
            Bytes p = c.valid;
            const uint32_t mode = rng.below(4);
            if (mode == 0)
            {
                const uint32_t k = 1 + rng.below(4);
                for (uint32_t i = 0; i < k; i++)
                    if (p.size() > 1) p[1 + rng.below((uint32_t)p.size() - 1)] = (uint8_t)rng.below(256);
            }
            else if (mode == 1)
            {
                // random body of random length after the right type byte
                const uint32_t len = rng.below(120);
                p.assign(1, c.valid[0]);
                for (uint32_t i = 0; i < len; i++) p.push_back((uint8_t)rng.below(256));
            }
            else if (mode == 2)
            {
                // the valid head, a random tail
                const size_t keep = 1 + rng.below((uint32_t)p.size());
                p.resize(keep);
                const uint32_t more = rng.below(40);
                for (uint32_t i = 0; i < more; i++) p.push_back((uint8_t)rng.below(256));
            }
            else
            {
                // 0xFF / 0x00 saturated bodies: hostile counts and lengths
                const uint8_t fill = rng.below(2) ? 0xFF : 0x00;
                const size_t len = 1 + rng.below(40);
                p.assign(len, fill);
                p[0] = c.valid[0];
            }
            const Probe pr = c.probe(p);
            if (pr.accepted)
            {
                accepted++;
                if (!good(pr)) bad = std::string("random packet accepted but not ") + (c.lossy ? "stable" : "canonical") + ": " + Hex(p, 60);
            }
            else if (!pr.untouched)
                bad = "random packet refused but changed out: " + Hex(p, 60);
        }
        Check(n + ": " + std::to_string(rounds) + " random / mutated packets: no crash, accepted ones canonical, refusals change nothing (" +
                  std::to_string(accepted) + " accepted)",
              bad.empty(), bad);
    }
}

// Hostile counts and lengths, built by hand: each must be refused and cheap.
static void HostileCounts()
{
    {   // ResourceManifest count 255 (cap is kMaxResources) with a short body
        Bytes p{(uint8_t)Msg::ResourceManifest, 255};
        ResourceManifest out;
        Check("hostile: a manifest count of 255 is refused", !Decode(p.data(), p.size(), out));
    }
    {   // VehicleWorld: removed count 255 but nothing after it
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleWorld);
        w.u32(1);
        w.u8(255);
        VehicleWorld out;
        const Bytes p = w.take();
        Check("hostile: VehicleWorld removed count 255 with no ids is refused", !Decode(p.data(), p.size(), out));
    }
    {   // VehicleWorld: present count 0xFFFF
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleWorld);
        w.u32(1);
        w.u8(0);
        w.u16(0xFFFF);
        VehicleWorld out;
        const Bytes p = w.take();
        Check("hostile: VehicleWorld present count 0xFFFF is refused", !Decode(p.data(), p.size(), out));
    }
    {   // VehicleWorld: present count = kMaxVehicles with enough padding for the 2-byte minimum, ids not ascending
        wire::Writer w;
        w.u8((uint8_t)Msg::VehicleWorld);
        w.u32(1);
        w.u8(0);
        w.u16((uint16_t)kMaxVehicles);
        for (int i = 0; i < kMaxVehicles; i++)
        {
            w.u8(7);
            w.u8(0);
        }
        VehicleWorld out;
        const Bytes p = w.take();
        Check("hostile: 256 presence entries with a repeated id are refused", !Decode(p.data(), p.size(), out));
    }
    {   // strings: u16 length 0xFFFF
        wire::Writer w;
        w.u8((uint8_t)Msg::ChatSend);
        w.u16(0xFFFF);
        ChatSend out;
        const Bytes p = w.take();
        Check("hostile: a chat string of 65535 bytes is refused", !Decode(p.data(), p.size(), out));
    }
    {   // ChatSend exactly at and one over the cap
        ChatSend atCap, over;
        atCap.text.assign(kMaxChat, 'a');
        over.text.assign(kMaxChat + 1, 'a');
        const Bytes a = Encode(atCap), o = Encode(over);
        ChatSend out;
        Check("hostile: a chat line of exactly kMaxChat is accepted, one more byte is refused",
              Decode(a.data(), a.size(), out) && !Decode(o.data(), o.size(), out));
    }
    {   // Hello name one over the cap
        Hello atCap, over;
        atCap.name.assign(kMaxName, 'a');
        over.name.assign(kMaxName + 1, 'a');
        const Bytes a = Encode(atCap), o = Encode(over);
        Hello out;
        Check("hostile: a Hello name of kMaxName is accepted, one more byte is refused",
              Decode(a.data(), a.size(), out) && !Decode(o.data(), o.size(), out));
    }
    {   // ResourceChunk: length field above the chunk cap / zero
        wire::Writer w;
        w.u8((uint8_t)Msg::ResourceChunk);
        w.u8(0);
        w.u32(0);
        w.u16(0xFFFF);
        ResourceChunk out;
        const Bytes p = w.take();
        Check("hostile: a ResourceChunk length of 65535 is refused", !Decode(p.data(), p.size(), out));
        wire::Writer z;
        z.u8((uint8_t)Msg::ResourceChunk);
        z.u8(0);
        z.u32(0);
        z.u16(0);
        const Bytes pz = z.take();
        Check("hostile: an empty ResourceChunk is refused", !Decode(pz.data(), pz.size(), out));
    }
    {   // a null pointer with a zero length is refused, never read
        Hello h;
        Check("hostile: a null packet is refused by every decoder entry used first (PeekType, Hello)",
              !Decode(nullptr, 0, h) && [] { Msg m; return !PeekType(nullptr, 0, m); }());
    }
    {   // PeekType: only 1..kMsgLast
        int accepted = 0;
        for (int b = 0; b < 256; b++)
        {
            const uint8_t p[1] = {(uint8_t)b};
            Msg m;
            if (PeekType(p, 1, m)) accepted++;
        }
        Check("PeekType accepts exactly the " + std::to_string((int)kMsgLast) + " defined type bytes", accepted == (int)kMsgLast, std::to_string(accepted));
    }
    {   // T4 VehicleImpact: what the encoder never writes is refused (an i8 normal of -128, an i16 of -32768, a vehicle id at kMaxVehicles); the encoder
        // refuses a non-finite value or such an id and saturates an absurd change at the wire's range
        VehicleImpact m;
        m.vehId = 3; m.otherId = 17; m.dvx = 1.0f;
        const Bytes good = Encode(m);
        Bytes nx = good, id = good, dv = good, oid = good;
        nx[17] = 0x80;                 // nx
        dv[11] = 0x00; dv[12] = 0x80;  // dvx = -32768
        id[5] = 0x00; id[6] = 0x01;    // vehId 256
        oid[7] = 0x00; oid[8] = 0x01;  // otherId 256
        VehicleImpact out, nan = m, far = m, big = m;
        nan.dvy = std::numeric_limits<float>::quiet_NaN();
        far.otherId = (uint16_t)kMaxVehicles;
        big.dvx = 1.0e6f;
        const Bytes bb = Encode(big);
        VehicleImpact back;
        const bool sat = !bb.empty() && Decode(bb.data(), bb.size(), back) && back.dvx > 511.0f && back.dvx < 512.0f;
        Check("hostile: VehicleImpact refuses an i8 of -128, an i16 of -32768 and a vehicle id of 256; Encode refuses NaN and an id of 256 and saturates 1e6 m/s at the wire's +-512",
              good.size() == kVehicleImpactBytes && Decode(good.data(), good.size(), out) && !Decode(nx.data(), nx.size(), out) && !Decode(dv.data(), dv.size(), out) &&
                  !Decode(id.data(), id.size(), out) && !Decode(oid.data(), oid.size(), out) && Encode(nan).empty() && Encode(far).empty() && sat);
    }
}

// The UDP query datagrams (query.h): exactly 24 bytes, every single-bit flip, every other length.
static void QueryFuzz()
{
    namespace q = gage::proto::query;
    q::Request rq;
    for (size_t i = 0; i < rq.nonce.size(); i++) rq.nonce[i] = (uint8_t)(i + 1);
    q::Reply rp;
    rp.protocol = 5;
    rp.nonce = rq.nonce;
    rp.players = 7;
    rp.maxPlayers = 32;
    for (uint8_t ver = 1; ver <= 2; ver++)
    {
        rp.version = ver;
        rp.latestClient = ver == 2 ? q::ClientVersion{0, 4, 5} : q::ClientVersion{};
        const q::Datagram dReply = q::Encode(rp);
        rq.replyVersion = ver;
        const q::Datagram dReq = q::Encode(rq);

        std::string bad;
        size_t accReq = 0, accRep = 0;
        for (size_t bit = 0; bit < 24 * 8 && bad.empty(); bit++)
        {
            q::Datagram a = dReq, b = dReply;
            a[bit / 8] = (uint8_t)(a[bit / 8] ^ (1u << (bit % 8)));
            b[bit / 8] = (uint8_t)(b[bit / 8] ^ (1u << (bit % 8)));
            q::Request r;
            q::Reply y;
            if (q::Decode(a.data(), a.size(), r)) accReq++;
            if (q::Decode(b.data(), b.size(), y))
            {
                accRep++;
                // whatever is accepted must round-trip through Encode to a datagram that decodes to the same fields
                const q::Datagram again = q::Encode(y);
                q::Reply z;
                if (!q::Decode(again.data(), again.size(), z) || z.protocol != y.protocol || z.players != y.players ||
                    z.maxPlayers != y.maxPlayers || z.nonce != y.nonce || z.version != y.version ||
                    z.latestClient.Packed() != y.latestClient.Packed())
                    bad = "reply bit " + std::to_string(bit) + " accepted but does not round-trip";
            }
            // IsRequest agrees with Decode(Request)
            if (q::IsRequest(a.data(), a.size()) != q::Decode(a.data(), a.size(), r)) bad = "IsRequest and Decode disagree at bit " + std::to_string(bit);
        }
        Check("query v" + std::to_string(ver) + ": every single-bit flip of a request / reply is refused or round-trips (" + std::to_string(accReq) + " / " +
                  std::to_string(accRep) + " accepted)",
              bad.empty(), bad);
    }
    {
        std::string bad;
        const q::Datagram d = q::Encode(rq);
        for (size_t len = 0; len <= 64 && bad.empty(); len++)
        {
            if (len == q::kDatagramSize) continue;
            std::vector<uint8_t> buf(len + 1, 0);
            for (size_t i = 0; i < len && i < d.size(); i++) buf[i] = d[i];
            q::Request r;
            q::Reply y;
            if (q::IsRequest(buf.data(), len) || q::Decode(buf.data(), len, r) || q::Decode(buf.data(), len, y)) bad = "length " + std::to_string(len) + " accepted";
        }
        Check("query: any datagram length other than 24 is refused", bad.empty(), bad);
    }
    {
        // Version bytes 0..255 in the latest-client fields round-trip (a launcher reading 255.255.255 must not wrap).
        std::string bad;
        for (int v = 0; v < 256 && bad.empty(); v++)
        {
            q::Reply a;
            a.version = 2;
            a.protocol = 5;
            a.latestClient = q::ClientVersion{(uint8_t)v, (uint8_t)(255 - v), (uint8_t)v};
            const q::Datagram d = q::Encode(a);
            q::Reply b;
            if (!q::Decode(d.data(), d.size(), b) || b.latestClient.major != v || b.latestClient.minor != 255 - v || b.latestClient.patch != v)
                bad = "version byte " + std::to_string(v);
        }
        Check("query v2: every latest-client byte value 0..255 round-trips", bad.empty(), bad);
        Rng rng(99);
        size_t ok = 0;
        for (int r = 0; r < 100000; r++)
        {
            q::Datagram d;
            for (auto& b : d) b = (uint8_t)rng.below(256);
            if (r % 2) { std::memcpy(d.data(), q::kMagic, 4); d[4] = r % 4 == 1 ? q::kKindRequest : q::kKindReply; }
            q::Request x;
            q::Reply y;
            if (q::Decode(d.data(), d.size(), x)) ok++;
            if (q::Decode(d.data(), d.size(), y)) ok++;
        }
        Check("query: 100k random 24-byte datagrams: no crash", true, std::to_string(ok) + " accepted");
    }
}

// The text validators the receivers rely on (ValidChat / ValidName / ValidAnimName / ValidResourceName), against an
// independent strict UTF-8 decoder written here: nothing they accept may hold a control character, an overlong form, a
// surrogate or a code point above U+10FFFF, and names stay inside their charsets.
static bool StrictUtf8(const std::string& s, bool& hasControl)
{
    hasControl = false;
    size_t i = 0;
    while (i < s.size())
    {
        const uint8_t b = (uint8_t)s[i];
        uint32_t cp;
        size_t n;
        if (b < 0x80) { cp = b; n = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; n = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; n = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; n = 4; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++)
        {
            if (((uint8_t)s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        }
        static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (n > 1 && cp < kMin[n]) return false;                 // overlong
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) hasControl = true;
        i += n;
    }
    return true;
}

static void ValidatorFuzz()
{
    Rng rng(4242);
    static const char* kPieces[] = {"a", "Z", " ", "_", ".", "-", "@", "/", "\x01", "\x7f", "\xc2\x80", "\xc2\xa0", "\xc0\x80",
                                    "\xe2\x80\xa8", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f\x98\x80", "\xe1\x83\x92", "\xff", "\x80", "9", ".."};
    std::string bad;
    size_t chatOk = 0, nameOk = 0, animOk = 0, resOk = 0;
    for (int r = 0; r < 200000 && bad.empty(); r++)
    {
        std::string t;
        const uint32_t pieces = rng.below(12);
        for (uint32_t i = 0; i < pieces; i++)
        {
            if (rng.below(5) == 0) t.push_back((char)rng.below(256));
            else t += kPieces[rng.below((uint32_t)(sizeof(kPieces) / sizeof(*kPieces)))];
        }
        if (ValidChat(t))
        {
            chatOk++;
            bool ctl = false;
            if (!StrictUtf8(t, ctl) || ctl || t.empty() || t.size() > kMaxChat) bad = "ValidChat accepted a bad string";
        }
        if (ValidName(t))
        {
            nameOk++;
            if (t.empty() || t.size() > kMaxName || t.front() == ' ' || t.back() == ' ' || t.find("  ") != std::string::npos)
                bad = "ValidName accepted a badly spaced / over-long name";
            for (char c : t)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == ' '))
                    bad = "ValidName accepted a character outside its charset";
            if (NameKey(NameKey(t)) != NameKey(t) || NameKey(t).size() != t.size()) bad = "NameKey is not idempotent / length preserving";
        }
        if (ValidAnimName(t))
        {
            animOk++;
            if (t.empty() || t.size() > kMaxAnimName) bad = "ValidAnimName accepted a bad length";
        }
        if (ValidResourceName(t))
        {
            resOk++;
            if (t.find("..") != std::string::npos || t.front() == '.' || t.back() == '.' || t.find('/') != std::string::npos) bad = "ValidResourceName accepted a traversal";
        }
    }
    Check("validators: 200k random strings: ValidChat only accepts strict UTF-8 without control characters (" + std::to_string(chatOk) +
              " accepted), ValidName / ValidAnimName / ValidResourceName stay inside their charsets (" + std::to_string(nameOk) + " / " +
              std::to_string(animOk) + " / " + std::to_string(resOk) + ")",
          bad.empty(), bad);
}

int main()
{
    const std::vector<Case> corpus = Corpus();
    for (const Case& c : corpus) Attack(c);
    HostileCounts();
    QueryFuzz();
    ValidatorFuzz();
    // per case: valid + prefix + trailing + bit flips + random = 5 checks; 11 hostile-count checks; 5 query checks; 1 validator check.
    const size_t expected = corpus.size() * 5 + 11 + 5 + 1 + 1;
    std::printf("\n%d checks, %d failed (%zu message cases)\n", g_run, g_failed, corpus.size());
    if (g_run != (int)expected)
    {
        std::printf("FAIL: expected %zu checks, ran %d\n", expected, g_run);
        return 1;
    }
    return g_failed ? 1 : 0;
}
