// server_test - the server's master-list, hardening and admin features, in-process.
// Build target: server_test (selftest/, never shipped). Same style as netcode_test: Check(),
// bounded pumps instead of bare sleeps, an expected-check-count assert, non-zero exit.
//
//   A. UDP query codec: the byte vectors of apps/master/PROTOCOL.md, rejection cases.
//   B. Config: the new server.json keys, the shipped example, URL parsing, announce JSON.
//   C. BanList: matching, removal, atomic persistence, damaged files.
//   D. UDP query against a live in-process server: counts, nonce echo, malformed
//      datagrams ignored, per-source rate limit.
//   E. Master announce against an in-process fake HTTP master: the POST and its JSON, the
//      heartbeat, backoff after 500/422/503 (Retry-After honoured), the master's own
//      heartbeatSeconds, a hung master never stalls the tick loop, Stop() stops it, plus the
//      HTTP client's response framing (Content-Length, chunked, to-EOF, truncated).
//   F. Chat flood limit.  G. Per-address connection cap.
//   H. Bans through the console: name and address, at connect and at Hello, persisted
//      across a server restart, damaged bans.json refuses to start.
//   I. The loopback admin channel: bad/missing token, good token, kick/ban/unban/say
//      through it, brute-force lockout, line limits, loopback-only binding.
#include "netrig.h"

#include "common/json.h"
#include "admin.h"
#include "anticheat.h"
#include "announce.h"
#include "banlist.h"
#include "httpclient.h"
#include "netcommon/netcommon.h"
#include "protocol/query.h"
#include "sock.h"
#include "util.h"
#include "damage.h"
#include "vehicles.h"

#include <enet/enet.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace query = proto::query;
namespace sock = server::sock;

// ===========================================================================
// helpers
// ===========================================================================

static std::string HexPlain(const uint8_t* p, size_t n)
{
    std::string out;
    char buf[4];
    for (size_t i = 0; i < n; i++)
    {
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        out += buf;
    }
    return out;
}

static const JVal* Member(const JVal& obj, const char* key)
{
    if (obj.t != JVal::Obj) return nullptr;
    for (const auto& kv : obj.o)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
static std::string StrField(const JVal& obj, const char* key)
{
    const JVal* v = Member(obj, key);
    return v && v->t == JVal::Str ? v->s : std::string("<missing>");
}
static double NumField(const JVal& obj, const char* key)
{
    const JVal* v = Member(obj, key);
    return v && v->t == JVal::Num ? v->n : -1.0;
}

static bool FileExists(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

static std::string ReadFileText(const std::string& path)
{
    std::string text, err;
    server::ReadWholeFile(path, text, &err);
    return text;
}

static void RemoveBanFiles(const std::string& path)
{
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

static void WriteTextFile(const std::string& path, const std::string& text)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

static void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static server::Config QuietConfig()
{
    server::Config c = Rig::BaseConfig();
    return c;
}

// ===========================================================================
// A. UDP query codec
// ===========================================================================

// `n` bytes of a datagram (0..24) as a plain vector, for building malformed variants.
static std::vector<uint8_t> Dg(const query::Datagram& d, size_t n = query::kDatagramSize)
{
    std::vector<uint8_t> v(n, 0);
    for (size_t i = 0; i < n && i < d.size(); i++) v[i] = d[i];
    return v;
}

static void QueryCodecTests()
{
    query::Request rq;
    for (size_t i = 0; i < query::kNonceSize; i++) rq.nonce[i] = (uint8_t)(i + 1);
    query::Reply rp;
    rp.protocol = 1;
    rp.nonce = rq.nonce;
    rp.players = 3;
    rp.maxPlayers = 32;
    const query::Datagram dq = query::Encode(rq), dr = query::Encode(rp);
    const std::string wantReq = "474147450101000001020304050607080000000000000000";
    const std::string wantRep = "474147458101010001020304050607080300200000000000";
    Check("query: request and reply encode to the byte vectors of apps/master/PROTOCOL.md, both 24 bytes",
          dq.size() == 24 && dr.size() == 24 && HexPlain(dq.data(), 24) == wantReq && HexPlain(dr.data(), 24) == wantRep,
          HexPlain(dq.data(), 24) + " / " + HexPlain(dr.data(), 24));

    query::Request rq2;
    query::Reply rp2;
    const bool a = query::Decode(dq.data(), dq.size(), rq2);
    const bool b = query::Decode(dr.data(), dr.size(), rp2);
    Check("query: both vectors decode back to their fields (nonce, protocol, players, maxPlayers)",
          a && b && rq2.nonce == rq.nonce && rp2.protocol == 1 && rp2.nonce == rq.nonce && rp2.players == 3 &&
              rp2.maxPlayers == 32);

    // A request is exactly 24 bytes with the prefix GAGE 01 01; everything else is not ours.
    std::vector<std::vector<uint8_t>> bad;
    auto variant = [&](size_t index, uint8_t value) {
        std::vector<uint8_t> v = Dg(dq);
        v[index] = value;
        bad.push_back(std::move(v));
    };
    bad.push_back(Dg(dq, 23)); // 23 bytes
    {
        std::vector<uint8_t> v = Dg(dq);
        v.push_back(0); // 25 bytes
        bad.push_back(std::move(v));
    }
    bad.push_back(Dg(dq, 0)); // empty
    for (size_t i = 0; i < 4; i++) variant(i, (uint8_t)(dq[i] ^ 0x20)); // each magic byte
    variant(4, 0x81); // a reply sent as a request
    variant(4, 0x00);
    variant(4, 0x02);
    variant(5, 0);    // query version 0
    variant(5, 2);    // query version 2
    bad.push_back(std::vector<uint8_t>(24, 0)); // zeros
    bad.push_back(Dg(dr));                      // a real reply
    int accepted = 0;
    for (const auto& v : bad)
    {
        query::Request r;
        if (query::IsRequest(v.data(), v.size()) || query::Decode(v.data(), v.size(), r)) accepted++;
    }
    // Bytes the spec calls zero are ignored on receipt, not required.
    std::vector<uint8_t> loose = Dg(dq);
    for (size_t i : {6, 7, 16, 17, 18, 19, 20, 21, 22, 23}) loose[i] = 0xFF;
    query::Request lr;
    Check("query: IsRequest/Decode accept only exactly-24-byte GAGE 01 01 datagrams (14 reject cases, reserved bytes ignored)",
          bad.size() == 14 && accepted == 0 && query::IsRequest(dq.data(), dq.size()) &&
              query::Decode(loose.data(), loose.size(), lr) && lr.nonce == rq.nonce && !query::IsRequest(nullptr, 24),
          std::to_string(bad.size()) + " cases, " + std::to_string(accepted) + " accepted");

    std::vector<std::vector<uint8_t>> badReply;
    badReply.push_back(Dg(dr, 23));
    {
        std::vector<uint8_t> v = Dg(dr);
        v.push_back(0);
        badReply.push_back(std::move(v));
    }
    badReply.push_back(Dg(dq)); // a request is not a reply
    {
        std::vector<uint8_t> v = Dg(dr);
        v[5] = 3; // 2 is the v2 reply since the update ping; the next format is not known yet
        badReply.push_back(std::move(v));
    }
    {
        std::vector<uint8_t> v = Dg(dr);
        v[0] = 'X';
        badReply.push_back(std::move(v));
    }
    badReply.push_back(std::vector<uint8_t>(24, 0));
    int replyAccepted = 0;
    query::Reply sentinel;
    sentinel.protocol = 777;
    for (const auto& v : badReply)
        if (query::Decode(v.data(), v.size(), sentinel)) replyAccepted++;
    Check("query: Decode(Reply) rejects wrong length, kind, version, magic and zeros (6 cases) and leaves the output untouched",
          badReply.size() == 6 && replyAccepted == 0 && sentinel.protocol == 777,
          std::to_string(replyAccepted) + " accepted");

    // ---- query format v2 ("update ping", 2026-11-02): the reply also carries the latest client x.y.z ----
    {
        query::Request r2 = rq;
        r2.replyVersion = 2;
        const query::Datagram d2q = query::Encode(r2);
        query::Reply v2 = rp;
        v2.version = 2;
        v2.latestClient = {0, 4, 4};
        const query::Datagram d2r = query::Encode(v2);
        // The v2 request differs from the v1 vector in ONE byte (the u16 at 6 = 2); the v2 reply from the v1 vector in
        // byte 5 and bytes 20..22. Both are still exactly 24 bytes.
        const std::string wantReq2 = "474147450101020001020304050607080000000000000000";
        const std::string wantRep2 = "474147458102010001020304050607080300200000040400";
        // (a v1 reply that carries a latestClient it was given still encodes as v1: the field is v2-only)
        query::Reply v1WithLatest = rp;
        v1WithLatest.latestClient = {9, 9, 9};
        query::Request rq3;
        query::Reply rp3, rp4;
        const bool a2 = query::Decode(d2q.data(), d2q.size(), rq3);
        const bool b2 = query::Decode(d2r.data(), d2r.size(), rp3);
        const bool c2 = query::Decode(dr.data(), dr.size(), rp4);
        Check("query v2: the request asking for v2 and the v2 reply encode to fixed 24-byte vectors (v1 vectors unchanged, a v1 reply "
              "ignores latestClient) and decode back (version, protocol, nonce, players, maxPlayers, latest client 0.4.4; a v1 reply decodes as version 1, 0.0.0)",
              d2q.size() == 24 && d2r.size() == 24 && HexPlain(d2q.data(), 24) == wantReq2 && HexPlain(d2r.data(), 24) == wantRep2 &&
                  HexPlain(query::Encode(rq).data(), 24) == wantReq && HexPlain(dr.data(), 24) == wantRep &&
                  HexPlain(query::Encode(v1WithLatest).data(), 24) == wantRep && a2 && rq3.replyVersion == 2 && rq3.nonce == rq.nonce && b2 &&
                  rp3.version == 2 && rp3.protocol == 1 && rp3.nonce == rq.nonce && rp3.players == 3 && rp3.maxPlayers == 32 &&
                  rp3.latestClient == query::ClientVersion{0, 4, 4} && c2 && rp4.version == 1 && !rp4.latestClient.Known() &&
                  query::Decode(dq.data(), dq.size(), rq2) && rq2.replyVersion == 1,
              HexPlain(d2q.data(), 24) + " / " + HexPlain(d2r.data(), 24));

        // A request asks for v2 only with a u16 at 6 of exactly 2. Everything else, including garbage in what v1 called
        // "zero, ignored on receipt", is a v1 asker and must get the v1 reply it can decode.
        struct Case { uint8_t b6, b7; int want; };
        const Case cases[] = {{0, 0, 1}, {1, 0, 1}, {2, 0, 2}, {3, 0, 1}, {0xFF, 0xFF, 1}, {2, 1, 1}, {0, 2, 1}, {0x02, 0x80, 1}, {4, 0, 1}};
        int wrong = 0;
        for (const Case& c : cases)
        {
            std::vector<uint8_t> v = Dg(dq);
            v[6] = c.b6;
            v[7] = c.b7;
            query::Request r;
            if (!query::Decode(v.data(), v.size(), r) || r.replyVersion != c.want || !query::IsRequest(v.data(), v.size())) wrong++;
        }
        // v2 never relaxes the framing: 23 and 25 bytes, and query version 2 in byte 5 of a request, are still not queries.
        std::vector<uint8_t> v23 = Dg(d2q, 23), v25 = Dg(d2q);
        v25.push_back(0);
        std::vector<uint8_t> req2b5 = Dg(d2q);
        req2b5[5] = 2;
        query::Request junk;
        Check("query v2: only a u16 at 6 of exactly 2 asks for v2 (0, 1, 3, 4, 0xFFFF, 2 with a nonzero byte 7, 0x8002 and 0x0200 all get v1), "
              "and a v2 request of 23 or 25 bytes, or with query version 2 in byte 5, is not a query",
              wrong == 0 && sizeof(cases) / sizeof(cases[0]) == 9 && !query::IsRequest(v23.data(), v23.size()) &&
                  !query::IsRequest(v25.data(), v25.size()) && !query::Decode(v25.data(), v25.size(), junk) &&
                  !query::IsRequest(req2b5.data(), req2b5.size()) && !query::Decode(req2b5.data(), req2b5.size(), junk),
              std::to_string(wrong) + " wrong");

        // Compatibility with the 0.4.2 / 0.4.3 launcher, whose Decode(Reply) is frozen here as it shipped: exactly
        // 24 bytes, magic, kind 0x81, query version 1. And the old server's IsRequest (the same test the master uses).
        auto legacyDecodeReply = [](const uint8_t* p, size_t n) {
            return n == 24 && std::memcmp(p, "GAGE", 4) == 0 && p[4] == 0x81 && p[5] == 1;
        };
        auto legacyIsRequest = [](const uint8_t* p, size_t n) {
            return n == 24 && std::memcmp(p, "GAGE", 4) == 0 && p[4] == 0x01 && p[5] == 1;
        };
        const query::Datagram serverV1 = query::Encode([&] { query::Reply r = rp; r.version = 1; r.latestClient = {0, 4, 4}; return r; }());
        Check("query v2 compat: the v1 reply (what a v1 request gets, whatever the server's latest client) passes the old launcher's frozen "
              "Decode; a v2 reply does not (so it must never be sent unasked); the v2-asking request passes the old server's IsRequest "
              "and has the same size as before",
              legacyDecodeReply(serverV1.data(), serverV1.size()) && HexPlain(serverV1.data(), 24) == wantRep &&
                  !legacyDecodeReply(d2r.data(), d2r.size()) && legacyIsRequest(d2q.data(), d2q.size()) && d2q.size() == query::kDatagramSize);
    }
    {
        struct V { const char* s; bool ok; uint8_t a, b, c; };
        const V vs[] = {{"0.4.4", true, 0, 4, 4}, {"0.4.3", true, 0, 4, 3}, {"255.255.255", true, 255, 255, 255}, {"0.0.0", true, 0, 0, 0},
                        {"10.20.30", true, 10, 20, 30}, {"1.2.3", true, 1, 2, 3}, {"007.0.1", true, 7, 0, 1},
                        {"", false, 0, 0, 0}, {"0.4", false, 0, 0, 0}, {"0.4.4.1", false, 0, 0, 0}, {"256.0.0", false, 0, 0, 0},
                        {"0.4.256", false, 0, 0, 0}, {"-1.0.0", false, 0, 0, 0}, {"a.b.c", false, 0, 0, 0}, {" 0.4.4", false, 0, 0, 0},
                        {"0.4.4 ", false, 0, 0, 0}, {"0..4", false, 0, 0, 0}, {".4.4", false, 0, 0, 0}, {"0.4.", false, 0, 0, 0},
                        {"0.4.4-rc1", false, 0, 0, 0}, {"1000.0.0", false, 0, 0, 0}, {"+1.0.0", false, 0, 0, 0}, {"0x1.0.0", false, 0, 0, 0}};
        int bad2 = 0;
        for (const V& v : vs)
        {
            query::ClientVersion cv{9, 9, 9};
            const bool ok = query::ParseClientVersion(v.s, std::strlen(v.s), cv);
            if (ok != v.ok || (ok && !(cv.major == v.a && cv.minor == v.b && cv.patch == v.c)) || (!ok && !(cv.major == 9 && cv.minor == 9 && cv.patch == 9)))
                bad2++;
        }
        query::ClientVersion nul{9, 9, 9};
        const char embedded[] = {'0', '.', '4', '\0', '.', '4'};
        Check("query v2: ParseClientVersion takes only strict x.y.z with parts 0..255 (23 cases incl. 0.4, 0.4.4.1, 256, signs, spaces, suffixes), leaves the output "
              "untouched on failure, and orders like the version (Packed 0.4.4 > 0.4.3 > 0.0.0 = unknown)",
              bad2 == 0 && !query::ParseClientVersion(nullptr, 0, nul) && !query::ParseClientVersion(embedded, sizeof(embedded), nul) &&
                  nul.major == 9 && (query::ClientVersion{0, 4, 4}).Packed() > (query::ClientVersion{0, 4, 3}).Packed() &&
                  (query::ClientVersion{1, 0, 0}).Packed() > (query::ClientVersion{0, 255, 255}).Packed() && !(query::ClientVersion{}).Known() &&
                  (query::ClientVersion{0, 0, 1}).Known(),
              std::to_string(bad2) + " wrong");
    }
}

// ===========================================================================
// B. Config, URLs, announce JSON
// ===========================================================================

static void ConfigTests()
{
    // The shipped example must be exactly the built-in defaults: a copy-paste changes nothing.
    {
        std::string here = __FILE__;
        const size_t slash = here.find_last_of("/\\");
        const std::string example = (slash == std::string::npos ? std::string(".") : here.substr(0, slash)) +
                                    "/../apps/server/server.json.example";
        server::Config c, d;
        std::string err;
        const bool ok = server::LoadConfigFile(example, c, &err);
        const server::ReplConfig &rc = c.repl, &rd = d.repl;
        const bool replSame =
            rc.enterRadius == rd.enterRadius && rc.leaveRadius == rd.leaveRadius && rc.maxRadius == rd.maxRadius &&
            rc.lookaheadMs == rd.lookaheadMs && rc.cellSize == rd.cellSize && rc.relevanceEveryMs == rd.relevanceEveryMs &&
            rc.nearDist == rd.nearDist && rc.midDist == rd.midDist && rc.rateNearHz == rd.rateNearHz && rc.rateMidHz == rd.rateMidHz &&
            rc.rateFarHz == rd.rateFarHz && rc.rateIdleHz == rd.rateIdleHz && rc.rateCoarseHz == rd.rateCoarseHz &&
            rc.idleAfterMs == rd.idleAfterMs && rc.budgetBytesPerSec == rd.budgetBytesPerSec && rc.packetMaxBytes == rd.packetMaxBytes &&
            rc.coarseSharePercent == rd.coarseSharePercent;
        // The file must LIST every replication key (equal-to-defaults alone would pass if it listed none).
        std::string text;
        if (FILE* f = std::fopen(example.c_str(), "rb"))
        {
            char buf[1024];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        const char* replKeys[] = {"relevanceEnterRadius", "relevanceLeaveRadius", "relevanceMaxRadius", "relevanceLookaheadMs",
                                  "relevanceCellSize", "relevanceEveryMs", "rateNearHz", "rateMidHz", "rateFarHz", "rateIdleHz",
                                  "rateCoarseHz", "nearDistance", "midDistance", "idleAfterMs", "clientBudgetBytesPerSec",
                                  "worldPacketMaxBytes", "coarseSharePercent"};
        size_t listed = 0;
        for (const char* k : replKeys)
            if (text.find(std::string("\"") + k + "\"") != std::string::npos) listed++;
        // v4: the vehicle and combat keys are listed too, at their defaults.
        const char* v4Keys[] = {"maxVehicles", "vehiclesPerPlayer", "vehicleEmptyDespawnSeconds", "vehicleEnterDistance",
                                "vehicleMaxSpeed", "fuelDrainPerMinute", "playerHealth", "respawnDelaySeconds",
                                "maxHitDistance", "hitsPerSecond", "spawnPoints"};
        size_t listedV4 = 0;
        for (const char* k : v4Keys)
            if (text.find(std::string("\"") + k + "\"") != std::string::npos) listedV4++;
        bool spawnSame = c.spawnPoints.size() == d.spawnPoints.size();
        for (size_t i = 0; spawnSame && i < c.spawnPoints.size(); i++)
            spawnSame = c.spawnPoints[i].x == d.spawnPoints[i].x && c.spawnPoints[i].y == d.spawnPoints[i].y &&
                        c.spawnPoints[i].z == d.spawnPoints[i].z && c.spawnPoints[i].heading == d.spawnPoints[i].heading;
        const bool v4Same = c.maxVehicles == d.maxVehicles && c.vehiclesPerPlayer == d.vehiclesPerPlayer &&
                            c.vehicleEmptyDespawnMs == d.vehicleEmptyDespawnMs && c.vehicleEnterDistance == d.vehicleEnterDistance &&
                            c.vehicleMaxSpeed == d.vehicleMaxSpeed && c.fuelDrainPerMinute == d.fuelDrainPerMinute &&
                            c.playerHealth == d.playerHealth && c.respawnDelayMs == d.respawnDelayMs &&
                            c.maxHitDistance == d.maxHitDistance && c.hitsPerSecond == d.hitsPerSecond && spawnSame;
        // The anti-cheat keys (docs/api/server.md "Anti-cheat rules") are listed too, at their defaults.
        const char* acKeys[] = {"hitShotWindowMs", "hitDamageScalePercent", "onFootMaxSpeed", "untrustedWindowMs",
                                "untrustedJoinGraceMs", "connectsPerIpPerMinute", "vehiclesPerIp", "vehicleOrphanSeconds",
                                "registerCooldownMs", "registerSeparationMeters", "vehicleModelDeny", "vehicleModelAllow"};
        size_t listedAc = 0;
        for (const char* k : acKeys)
            if (text.find(std::string("\"") + k + "\"") != std::string::npos) listedAc++;
        const bool acSame = c.hitShotWindowMs == d.hitShotWindowMs && c.hitDamageScalePercent == d.hitDamageScalePercent &&
                            c.onFootMaxSpeed == d.onFootMaxSpeed && c.untrustedWindowMs == d.untrustedWindowMs &&
                            c.untrustedJoinGraceMs == d.untrustedJoinGraceMs && c.connectsPerIpPerMinute == d.connectsPerIpPerMinute &&
                            c.vehiclesPerIp == d.vehiclesPerIp && c.vehicleOrphanMs == d.vehicleOrphanMs &&
                            c.registerCooldownMs == d.registerCooldownMs && c.registerSeparationMeters == d.registerSeparationMeters &&
                            c.vehicleModelDeny == d.vehicleModelDeny && c.vehicleModelAllow == d.vehicleModelAllow;
        Check("config: apps/server/server.json.example loads and equals the built-in defaults (all 17 relevance, rate and budget keys, the 11 v4 vehicle / combat keys and the 12 anti-cheat keys listed)",
              ok && replSame && listed == 17 && listedV4 == 11 && listedAc == 12 && v4Same && acSame && c.name == d.name && c.port == d.port && c.maxPlayers == d.maxPlayers && c.tickHz == d.tickHz &&
                  c.masterUrl == d.masterUrl && c.heartbeatSeconds == d.heartbeatSeconds && c.gamemode == d.gamemode &&
                  c.lang == d.lang && c.url == d.url && c.maxConnectionsPerIp == d.maxConnectionsPerIp &&
                  c.chatBurst == d.chatBurst && c.chatWindowMs == d.chatWindowMs && c.bansFile == d.bansFile &&
                  c.adminPort == d.adminPort && c.adminToken == d.adminToken,
              err);
    }

    const char* path = "gage_server_test_config.json";
    SetEnv("GAGE_ADMIN_TOKEN", "");
    {
        WriteTextFile(path,
                      "{ \"name\": \"N\", \"port\": 25001, \"maxPlayers\": 12, \"tickHz\": 30,"
                      " \"masterUrl\": \"http://master.example.org:8080/base\", \"heartbeatSeconds\": 45,"
                      " \"gamemode\": \"race\", \"lang\": \"pt-BR\", \"url\": \"https://example.org/x\","
                      " \"maxConnectionsPerIp\": 7, \"chatBurst\": 9, \"chatWindowSeconds\": 12,"
                      " \"bansFile\": \"my-bans.json\", \"adminPort\": 24900,"
                      " \"adminToken\": \"0123456789abcdefXYZ\" }");
        server::Config c;
        std::string err;
        const bool ok = server::LoadConfigFile(path, c, &err);
        Check("config: every new server.json key loads (masterUrl, heartbeatSeconds, gamemode, lang, url, "
              "maxConnectionsPerIp, chatBurst, chatWindowSeconds, bansFile, adminPort, adminToken)",
              ok && c.masterUrl == "http://master.example.org:8080/base" && c.heartbeatSeconds == 45 &&
                  c.gamemode == "race" && c.lang == "pt-BR" && c.url == "https://example.org/x" &&
                  c.maxConnectionsPerIp == 7 && c.chatBurst == 9 && c.chatWindowMs == 12000 &&
                  c.bansFile == "my-bans.json" && c.adminPort == 24900 && c.adminToken == "0123456789abcdefXYZ",
              err);
    }
    {
        const std::vector<std::string> badFiles = {
            "{ \"masterUrl\": \"https://master.example.org\" }",                // no TLS
            "{ \"masterUrl\": \"ftp://master.example.org\" }",                  // wrong scheme
            "{ \"masterUrl\": \"http://user:pw@master.example.org\" }",         // credentials
            "{ \"masterUrl\": \"master.example.org\" }",                        // no scheme
            "{ \"masterUrl\": \"http://m.example.org:99999\" }",                // bad port
            "{ \"masterUrl\": 5 }",                                             // wrong type
            "{ \"masterUrl\": \"http://m.example.org\", \"gamemode\": \"\" }",  // master needs a gamemode
            "{ \"heartbeatSeconds\": 4 }",                                      // below 5
            "{ \"gamemode\": \"123456789012345678901234567890123\" }",          // 33 bytes
            "{ \"lang\": \"english-US1\" }",                                    // not a tag, > 8 bytes
            "{ \"lang\": \"e\" }",                                              // too short
            "{ \"url\": \"ftp://example.org\" }",                               // not http(s)
            "{ \"url\": \"http://user:pw@example.org\" }",                      // credentials
            "{ \"maxConnectionsPerIp\": 2000 }",                                // over 1024
            "{ \"chatBurst\": 0 }",                                             // below 1
            "{ \"chatWindowSeconds\": 0 }",                                     // below 1
            "{ \"adminPort\": 24900 }",                                         // channel on, no token
            "{ \"adminPort\": 24900, \"adminToken\": \"short\" }",              // token under 16 characters
            "{ \"adminPort\": 70000, \"adminToken\": \"0123456789abcdefXYZ\" }", // bad port
            "{ \"bansFiles\": \"x\" }",                                         // unknown key (typo)
        };
        int rejected = 0, untouched = 0;
        std::string firstErr;
        for (const std::string& text : badFiles)
        {
            WriteTextFile(path, text);
            server::Config c;
            c.name = "sentinel";
            c.chatBurst = 77;
            std::string err;
            bool miss = true;
            if (!server::LoadConfigFile(path, c, &err, &miss) && !miss) rejected++;
            else firstErr += "[accepted: " + text + "]";
            if (c.name == "sentinel" && c.chatBurst == 77) untouched++;
        }
        Check("config: bad masterUrl (https, scheme, credentials, port), gamemode, lang, url, heartbeat, caps, burst, admin "
              "token and unknown keys are all rejected untouched (20 cases)",
              rejected == 20 && untouched == 20, std::to_string(rejected) + " rejected, " + firstErr);
    }
    // v4: the vehicle and combat keys load, and every out-of-range value is refused untouched.
    {
        WriteTextFile(path, "{ \"maxVehicles\": 200, \"vehiclesPerPlayer\": 5, \"vehicleEmptyDespawnSeconds\": 0,"
                            " \"vehicleEnterDistance\": 6, \"vehicleMaxSpeed\": 90, \"fuelDrainPerMinute\": 0,"
                            " \"playerHealth\": 200, \"respawnDelaySeconds\": 2, \"maxHitDistance\": 300, \"hitsPerSecond\": 12,"
                            " \"spawnPoints\": [[1, 2, 3, 90], [-100.5, 200.25, 40, -45]] }");
        server::Config c;
        std::string err;
        const bool ok = server::LoadConfigFile(path, c, &err);
        const bool loaded = ok && c.maxVehicles == 200 && c.vehiclesPerPlayer == 5 && c.vehicleEmptyDespawnMs == 0 &&
                            c.vehicleEnterDistance == 6 && c.vehicleMaxSpeed == 90 && c.fuelDrainPerMinute == 0 &&
                            c.playerHealth == 200 && c.respawnDelayMs == 2000 && c.maxHitDistance == 300 && c.hitsPerSecond == 12 &&
                            c.spawnPoints.size() == 2 && c.spawnPoints[1].x == -100.5f && c.spawnPoints[1].y == 200.25f &&
                            c.spawnPoints[1].heading == -45.0f;
        const std::vector<std::string> bad = {
            "{ \"maxVehicles\": 0 }",          "{ \"maxVehicles\": 257 }",          "{ \"vehiclesPerPlayer\": 65 }",
            "{ \"vehicleMaxSpeed\": 5 }",      "{ \"playerHealth\": 256 }",         "{ \"playerHealth\": 0 }",
            "{ \"respawnDelaySeconds\": 301 }", "{ \"hitsPerSecond\": 0 }",          "{ \"maxHitDistance\": 9 }",
            "{ \"spawnPoints\": [] }",          "{ \"spawnPoints\": [[1, 2, 3]] }",  "{ \"spawnPoints\": [[1, 2, 3, \"n\"]] }",
            "{ \"spawnPoints\": [[20000, 0, 0, 0]] }",
        };
        int rejected = 0;
        std::string firstErr;
        for (const std::string& text : bad)
        {
            WriteTextFile(path, text);
            server::Config cc;
            cc.maxVehicles = 77;
            std::string e;
            if (!server::LoadConfigFile(path, cc, &e) && cc.maxVehicles == 77) rejected++;
            else firstErr += "[accepted: " + text + "]";
        }
        Check("config v4: vehicle and combat keys load (spawnPoints as [x, y, z, heading] arrays); caps, health, delays, rates, "
              "distances and malformed or out-of-box spawn points are rejected untouched (13 cases)",
              loaded && rejected == 13, err + firstErr);
    }
    // v5: the damage-scaling keys (docs/combat spec 1): the defaults, the shipped example, a custom load and the range checks.
    {
        const server::Config d;
        const bool defaults = d.playerHealth == 200 && d.firearmDamagePercent == 72 && d.meleeDamagePercent == 50 &&
                              d.explosiveDamagePercent == 100 && d.runOverDamagePercent == 100 && d.headshotPercent == 300 &&
                              d.respawnArmour == 0;
        std::string here = __FILE__;
        const size_t slash = here.find_last_of("/\\");
        const std::string example = (slash == std::string::npos ? std::string(".") : here.substr(0, slash)) +
                                    "/../apps/server/server.json.example";
        std::string text;
        if (FILE* f = std::fopen(example.c_str(), "rb"))
        {
            char buf[1024];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        size_t listed = 0;
        for (const char* k : {"firearmDamagePercent", "meleeDamagePercent", "explosiveDamagePercent", "runOverDamagePercent",
                              "headshotPercent", "respawnArmour"})
            if (text.find(std::string("\"") + k + "\"") != std::string::npos) listed++;
        server::Config ex;
        std::string exErr;
        const bool exOk = server::LoadConfigFile(example, ex, &exErr);
        const bool exSame = exOk && ex.playerHealth == d.playerHealth && ex.firearmDamagePercent == d.firearmDamagePercent &&
                            ex.meleeDamagePercent == d.meleeDamagePercent && ex.explosiveDamagePercent == d.explosiveDamagePercent &&
                            ex.runOverDamagePercent == d.runOverDamagePercent && ex.headshotPercent == d.headshotPercent &&
                            ex.respawnArmour == d.respawnArmour;
        WriteTextFile(path, "{ \"playerHealth\": 150, \"firearmDamagePercent\": 100, \"meleeDamagePercent\": 25,"
                            " \"explosiveDamagePercent\": 150, \"runOverDamagePercent\": 10, \"headshotPercent\": 100, \"respawnArmour\": 50 }");
        server::Config c;
        std::string err;
        const bool ok = server::LoadConfigFile(path, c, &err);
        const bool loaded = ok && c.playerHealth == 150 && c.firearmDamagePercent == 100 && c.meleeDamagePercent == 25 &&
                            c.explosiveDamagePercent == 150 && c.runOverDamagePercent == 10 && c.headshotPercent == 100 &&
                            c.respawnArmour == 50;
        const std::vector<std::string> bad = {
            "{ \"firearmDamagePercent\": 0 }",     "{ \"firearmDamagePercent\": 1001 }", "{ \"meleeDamagePercent\": 0 }",
            "{ \"explosiveDamagePercent\": 1001 }", "{ \"runOverDamagePercent\": 0 }",    "{ \"headshotPercent\": 99 }",
            "{ \"headshotPercent\": 1001 }",        "{ \"respawnArmour\": 256 }",         "{ \"respawnArmour\": -1 }",
            "{ \"firearmDamagePercent\": 72.5 }",
        };
        int rejected = 0;
        std::string firstErr;
        for (const std::string& t : bad)
        {
            WriteTextFile(path, t);
            server::Config cc;
            cc.firearmDamagePercent = 77;
            std::string e;
            if (!server::LoadConfigFile(path, cc, &e) && cc.firearmDamagePercent == 77) rejected++;
            else firstErr += "[accepted: " + t + "]";
        }
        Check("config v5: playerHealth 200, firearms 72, melee 50, explosives 100, run-over 100, headshots 300 and respawn armour 0 are the defaults, "
              "the shipped server.json.example lists the six new keys at those values, they load, and out-of-range or fractional values are rejected untouched (10 cases)",
              defaults && listed == 6 && exSame && loaded && rejected == 10, err + exErr + firstErr);
    }
    // The anti-cheat keys: load (model lists as numbers, "0x" strings and names), and every out-of-range value is refused untouched.
    {
        const std::string adder = std::to_string(server::ac::Joaat("adder"));
        WriteTextFile(path, "{ \"hitShotWindowMs\": 0, \"hitDamageScalePercent\": 50, \"onFootMaxSpeed\": 9,"
                            " \"untrustedWindowMs\": 1000, \"untrustedJoinGraceMs\": 0, \"connectsPerIpPerMinute\": 2,"
                            " \"vehiclesPerIp\": 2, \"vehicleOrphanSeconds\": 30, \"registerCooldownMs\": 0,"
                            " \"registerSeparationMeters\": 0,"
                            " \"vehicleModelDeny\": [" + adder + ", \"0xB779A091\", \"Sultan\"],"
                            " \"vehicleModelAllow\": [\"zentorno\"] }");
        server::Config c;
        std::string err;
        const bool ok = server::LoadConfigFile(path, c, &err);
        const bool loaded = ok && c.hitShotWindowMs == 0 && c.hitDamageScalePercent == 50 && c.onFootMaxSpeed == 9 &&
                            c.untrustedWindowMs == 1000 && c.untrustedJoinGraceMs == 0 && c.connectsPerIpPerMinute == 2 &&
                            c.vehiclesPerIp == 2 && c.vehicleOrphanMs == 30000 && c.registerCooldownMs == 0 &&
                            c.registerSeparationMeters == 0 && c.vehicleModelDeny.size() == 2 &&
                            c.vehicleModelDeny[0] == server::ac::Joaat("adder") && c.vehicleModelDeny[1] == server::ac::Joaat("sultan") &&
                            c.vehicleModelAllow.size() == 1 && c.vehicleModelAllow[0] == server::ac::Joaat("zentorno");
        std::string manyModels = "{ \"vehicleModelDeny\": [";
        for (int i = 1; i <= 129; i++) manyModels += (i > 1 ? "," : "") + std::to_string(i);
        manyModels += "] }";
        const std::vector<std::string> bad = {
            "{ \"hitShotWindowMs\": 99 }",        "{ \"hitShotWindowMs\": 5001 }",      "{ \"hitDamageScalePercent\": 9 }",
            "{ \"hitDamageScalePercent\": 1001 }", "{ \"onFootMaxSpeed\": 4 }",         "{ \"onFootMaxSpeed\": 601 }",
            "{ \"untrustedWindowMs\": 499 }",     "{ \"untrustedWindowMs\": 60001 }",   "{ \"untrustedJoinGraceMs\": 120001 }",
            "{ \"connectsPerIpPerMinute\": 10001 }", "{ \"vehiclesPerIp\": 0 }",        "{ \"vehiclesPerIp\": 257 }",
            "{ \"vehicleOrphanSeconds\": 0 }",    "{ \"vehicleOrphanSeconds\": 3601 }", "{ \"registerCooldownMs\": 60001 }",
            "{ \"registerSeparationMeters\": 21 }", "{ \"vehicleModelDeny\": \"adder\" }", "{ \"vehicleModelDeny\": [0] }",
            "{ \"vehicleModelAllow\": [1.5] }",   "{ \"vehicleModelAllow\": [\"\"] }",  "{ \"vehicleModelAllow\": [true] }",
            manyModels,
        };
        int rejected = 0;
        std::string firstErr;
        for (const std::string& text : bad)
        {
            WriteTextFile(path, text);
            server::Config cc;
            cc.maxVehicles = 77;
            std::string e;
            if (!server::LoadConfigFile(path, cc, &e) && cc.maxVehicles == 77) rejected++;
            else firstErr += "[accepted: " + text.substr(0, 60) + "]";
        }
        server::Config prog;
        std::string verr;
        prog.vehicleModelAllow = {1, 2};
        const bool validOk = server::Validate(prog, &verr);
        prog.vehicleModelAllow = {0};
        const bool zeroRefused = !server::Validate(prog, &verr);
        prog.vehicleModelAllow.assign(129, 5);
        const bool manyRefused = !server::Validate(prog, &verr);
        Check("config anti-cheat: the 12 keys load (vehicleOrphanSeconds in seconds; model lists as numbers, \"0x\" hex strings and joaat names, "
              "duplicates collapse); out-of-range values, a model 0, a non-array list and 129 models are rejected untouched (22 cases); "
              "Validate holds the same bounds for a programmatic Config",
              loaded && rejected == (int)bad.size() && bad.size() == 22 && validOk && zeroRefused && manyRefused, err + firstErr);
    }
    {
        // The token may come from the environment when the file sets none.
        WriteTextFile(path, "{ \"adminPort\": 24900 }");
        server::Config none;
        std::string err;
        const bool without = server::LoadConfigFile(path, none, &err);
        SetEnv("GAGE_ADMIN_TOKEN", "env-token-0123456789");
        server::Config withEnv;
        const bool with = server::LoadConfigFile(path, withEnv, &err);
        WriteTextFile(path, "{ \"adminPort\": 24900, \"adminToken\": \"file-token-0123456789\" }");
        server::Config fileWins;
        const bool both = server::LoadConfigFile(path, fileWins, &err);
        SetEnv("GAGE_ADMIN_TOKEN", "");
        Check("config: the admin token falls back to GAGE_ADMIN_TOKEN only when the file has none",
              !without && with && withEnv.adminToken == "env-token-0123456789" && both &&
                  fileWins.adminToken == "file-token-0123456789");
    }
    {
        // The update ping: latestClientVersion in the file, else GAGE_LATEST_CLIENT_VERSION (what release.sh sets), else unknown.
        SetEnv("GAGE_LATEST_CLIENT_VERSION", "");
        std::string err;
        WriteTextFile(path, "{ \"tickHz\": 30 }");
        server::Config none;
        const bool okNone = server::LoadConfigFile(path, none, &err);
        WriteTextFile(path, "{ \"latestClientVersion\": \"0.4.4\" }");
        server::Config fromFile;
        const bool okFile = server::LoadConfigFile(path, fromFile, &err);
        SetEnv("GAGE_LATEST_CLIENT_VERSION", "0.5.1");
        server::Config fileWins;
        const bool okBoth = server::LoadConfigFile(path, fileWins, &err);
        WriteTextFile(path, "{ \"tickHz\": 30 }");
        server::Config fromEnv;
        const bool okEnv = server::LoadConfigFile(path, fromEnv, &err);
        WriteTextFile(path, "{ \"latestClientVersion\": \"\" }");
        server::Config fileEmpty;
        const bool okEmpty = server::LoadConfigFile(path, fileEmpty, &err); // an explicit "" in the file means none, env or not
        SetEnv("GAGE_LATEST_CLIENT_VERSION", "0.4");
        WriteTextFile(path, "{ \"tickHz\": 30 }");
        server::Config badEnv;
        badEnv.latestClient = {1, 1, 1};
        std::string badEnvErr;
        const bool okBadEnv = server::LoadConfigFile(path, badEnv, &badEnvErr);
        SetEnv("GAGE_LATEST_CLIENT_VERSION", "");
        const std::vector<std::string> badFiles = {"{ \"latestClientVersion\": \"0.4\" }", "{ \"latestClientVersion\": \"0.4.4.1\" }",
                                                   "{ \"latestClientVersion\": \"256.0.0\" }", "{ \"latestClientVersion\": \"v0.4.4\" }",
                                                   "{ \"latestClientVersion\": 4 }", "{ \"latestClientVersion\": null }",
                                                   "{ \"latestClientversion\": \"0.4.4\" }"};
        int rejected = 0, untouched = 0;
        for (const std::string& text : badFiles)
        {
            WriteTextFile(path, text);
            server::Config c;
            c.latestClient = {7, 7, 7};
            if (!server::LoadConfigFile(path, c, &err)) rejected++;
            if (c.latestClient == proto::query::ClientVersion{7, 7, 7}) untouched++;
        }
        Check("config: latestClientVersion (server.json, else GAGE_LATEST_CLIENT_VERSION): absent = 0.0.0 unknown; the file key wins over the environment; "
              "an explicit \"\" means none; the environment alone works; 7 malformed file values and a malformed environment value are refused with the config untouched",
              okNone && !none.latestClient.Known() && okFile && fromFile.latestClient == proto::query::ClientVersion{0, 4, 4} && okBoth &&
                  fileWins.latestClient == proto::query::ClientVersion{0, 4, 4} && okEnv && fromEnv.latestClient == proto::query::ClientVersion{0, 5, 1} &&
                  okEmpty && !fileEmpty.latestClient.Known() && !okBadEnv && badEnv.latestClient == proto::query::ClientVersion{1, 1, 1} &&
                  badEnvErr.find("GAGE_LATEST_CLIENT_VERSION") != std::string::npos && rejected == 7 && untouched == 7,
              err + badEnvErr);
    }
    std::remove(path);

    {
        auto invalid = [](const std::function<void(server::Config&)>& mutate) {
            server::Config c;
            mutate(c);
            std::string e;
            return !server::Validate(c, &e);
        };
        const std::string rlo = Raw({0xE2, 0x80, 0xAE}); // U+202E right-to-left override
        const std::string ls = Raw({0xE2, 0x80, 0xA8});  // U+2028 line separator
        server::Config ok;
        ok.masterUrl = "http://127.0.0.1:8080";
        std::string e;
        Check("config: Validate rejects what the master would reject forever (bidi/line-separator in name or gamemode, "
              "enabled admin without a token, zero backoff) and accepts the same fields when valid",
              server::Validate(ok, &e) && invalid([&](server::Config& c) { c.masterUrl = "http://h"; c.name = "a" + rlo + "b"; }) &&
                  invalid([&](server::Config& c) { c.gamemode = "x" + ls; }) &&
                  invalid([](server::Config& c) { c.adminEphemeral = true; }) &&
                  invalid([](server::Config& c) { c.announceBackoffBaseMs = 0; }) &&
                  invalid([](server::Config& c) { c.announceBackoffMaxMs = 1; c.announceBackoffBaseMs = 2; }) &&
                  invalid([](server::Config& c) { c.queryRatePerIp = 0; }) &&
                  invalid([](server::Config& c) { c.chatWindowMs = 99; }),
              e);
    }

    {
        server::HttpUrl u;
        std::string err;
        const bool a = server::ParseHttpUrl("http://master.example.org:8080/base/", u, &err) && u.host == "master.example.org" &&
                       u.port == 8080 && u.path == "/base/" && u.hostHeader == "master.example.org:8080";
        server::HttpUrl v6;
        const bool b = server::ParseHttpUrl("http://[::1]:9/", v6, &err) && v6.host == "::1" && v6.port == 9 &&
                       v6.hostHeader == "[::1]:9";
        server::HttpUrl plain;
        const bool c = server::ParseHttpUrl("HTTP://Example.org", plain, &err) && plain.port == 80 && plain.path == "/" &&
                       plain.hostHeader == "Example.org";
        const std::vector<std::string> rejects = {"", "https://x.org", "ftp://x.org", "x.org", "http://", "http://:80",
                                                  "http://u@x.org", "http://x.org?q=1", "http://x.org/#f", "http://x.org:0",
                                                  "http://x.org:65536", "http://x org", "http://x.org:", "http://[::1",
                                                  std::string("http://") + std::string(300, 'a')};
        int bad = 0;
        std::string httpsErr;
        for (const std::string& r : rejects)
        {
            server::HttpUrl x;
            std::string e;
            if (!server::ParseHttpUrl(r, x, &e)) bad++;
            if (r == "https://x.org") httpsErr = e;
        }
        auto path = [](const char* url) {
            server::HttpUrl x;
            std::string e;
            server::ParseHttpUrl(url, x, &e);
            return server::AnnouncePath(x);
        };
        Check("http: ParseHttpUrl reads host/port/path (IPv6, default port, case) and rejects https, credentials, queries, "
              "bad ports and junk (15 cases); AnnouncePath appends /announce once",
              a && b && c && bad == (int)rejects.size() && rejects.size() == 15 &&
                  httpsErr.find("not supported") != std::string::npos && path("http://m") == "/announce" &&
                  path("http://m/") == "/announce" && path("http://m/base") == "/base/announce" &&
                  path("http://m/base/") == "/base/announce" && path("http://m/base/announce") == "/base/announce",
              httpsErr);
    }

    {
        server::AnnounceSettings s;
        std::string err;
        server::ParseHttpUrl("http://127.0.0.1:1", s.master, &err);
        s.gamePort = 24800;
        s.name = "Say \"hi\" \\ there";
        s.gamemode = "freeroam";
        s.lang = "en";
        s.protocol = 1;
        s.maxPlayers = 32;
        const std::string withoutUrl = server::BuildAnnounceJson(s, 3);
        s.url = "https://example.org/a?b=c";
        const std::string withUrl = server::BuildAnnounceJson(s, 0);
        JParser p1, p2;
        const bool ok1 = p1.parse(withoutUrl.data(), withoutUrl.size());
        const bool ok2 = p2.parse(withUrl.data(), withUrl.size());
        Check("announce: the JSON body escapes the name and carries port, gamemode, lang, players, maxPlayers, protocol; "
              "url only when set",
              ok1 && ok2 && NumField(p1.v, "port") == 24800 && StrField(p1.v, "name") == "Say \"hi\" \\ there" &&
                  StrField(p1.v, "gamemode") == "freeroam" && StrField(p1.v, "lang") == "en" && NumField(p1.v, "players") == 3 &&
                  NumField(p1.v, "maxPlayers") == 32 && NumField(p1.v, "protocol") == 1 && !Member(p1.v, "url") &&
                  StrField(p2.v, "url") == "https://example.org/a?b=c" && NumField(p2.v, "players") == 0,
              withoutUrl);
    }
}

// ===========================================================================
// C. BanList
// ===========================================================================

static void BanListTests()
{
    const std::string path = "gage_server_test_banlist.json";
    RemoveBanFiles(path);
    std::string err, saveErr;
    {
        server::BanList bl;
        const bool missingOk = bl.Load(path, &err) && bl.entries().empty(); // no file yet: empty
        server::BanEntry n;
        n.name = "Mallory";
        n.reason = "cheating \"badly\"";
        server::BanEntry ip;
        ip.ip = "203.0.113.5";
        server::BanEntry both;
        both.name = "Eve";
        both.ip = "198.51.100.7";
        const bool added = bl.Add(n, &err, &saveErr) && bl.Add(ip, &err, &saveErr) && bl.Add(both, &err, &saveErr);
        bool again = false;
        std::string againErr;
        const bool dupRefused = !bl.Add(n, &againErr, &saveErr, &again) && again;
        Check("banlist: a missing file is an empty list; names and addresses are added, a duplicate is refused as already banned",
              missingOk && added && saveErr.empty() && dupRefused && bl.entries().size() == 3, againErr);
        Check("banlist: match by name is case-insensitive, by address exact, either one of a name+address entry bans; "
              "an unknown name or address does not",
              bl.FindName("mallory") && bl.FindName("MALLORY")->reason == "cheating \"badly\"" && bl.FindIp("203.0.113.5") &&
                  bl.Match("eve", "") && bl.Match("", "198.51.100.7") && bl.Match("Zed", "203.0.113.5") &&
                  !bl.Match("Zed", "203.0.113.6") && !bl.FindName("Malloryy") && !bl.FindIp("203.0.113.50"));
        // Remove by name takes the whole name+address entry; by address only the entries with it.
        const size_t byName = bl.Remove("EVE", &saveErr);
        const bool eveGone = !bl.FindName("Eve") && !bl.FindIp("198.51.100.7");
        const size_t byIp = bl.Remove("203.0.113.5", &saveErr);
        const size_t none = bl.Remove("nobody", &saveErr);
        Check("banlist: Remove lifts every entry matching a name (case-insensitive) or an address, and reports zero for neither",
              byName == 1 && eveGone && byIp == 1 && none == 0 && bl.entries().size() == 1 && bl.FindName("Mallory"));
    }
    {
        // Persistence: what was saved reloads identically; the temp file never lingers.
        server::BanList reload;
        const bool loaded = reload.Load(path, &err);
        const server::BanEntry* m = reload.FindName("Mallory");
        const std::string text = ReadFileText(path);
        JParser p;
        const bool parses = p.parse(text.data(), text.size());
        const size_t reloadedCount = reload.entries().size();
        // `m` points into the list: read it before Add() can reallocate the entries.
        const bool entryKept = m && m->reason == "cheating \"badly\"" && !m->at.empty();
        server::BanEntry again;
        again.name = "Trent";
        again.reason = "second write replaces the first";
        const bool second = reload.Add(again, &err, &saveErr); // rename over an existing file
        server::BanList third;
        const bool loaded3 = third.Load(path, &err);
        Check("banlist: saved atomically (no .tmp left), valid JSON, reloads identically, and a second save replaces the file",
              loaded && entryKept && reloadedCount == 1 && parses &&
                  !FileExists(path + ".tmp") && second && saveErr.empty() && loaded3 && third.entries().size() == 2 &&
                  third.FindName("trent") && third.FindName("mallory"),
              text);
    }
    {
        const std::vector<std::string> badFiles = {
            "{ not json", "[]", "{\"bans\": 5}", "{\"bans\": [5]}", "{\"bans\": [{}]}", "{\"bans\": [{\"ip\": \"999.1.1.1\"}]}",
            "{\"bans\": [{\"ip\": \"1.2.3\"}]}", "{\"bans\": [{\"name\": \"bad<name>\"}]}",
            "{\"bans\": [{\"name\": \"A\", \"extra\": \"x\"}]}", "{\"ban\": []}", "{\"bans\": []} trailing",
            "{\"bans\": [{\"name\": 5}]}"};
        int rejected = 0, untouched = 0;
        server::BanList keep;
        server::BanEntry seed;
        seed.name = "Seed";
        keep.Load("", &err);
        keep.Add(seed, &err, &saveErr);
        for (const std::string& text : badFiles)
        {
            const std::string bad = "gage_server_test_banlist_bad.json";
            WriteTextFile(bad, text);
            if (!keep.Load(bad, &err)) rejected++;
            if (keep.entries().size() == 1 && keep.FindName("Seed")) untouched++;
            std::remove(bad.c_str());
        }
        server::BanList empty;
        std::string e2;
        const bool addBad = !empty.Add(server::BanEntry(), &e2, &saveErr) ;
        server::BanEntry badIp;
        badIp.ip = "01.2.3.4";
        const bool addBadIp = !empty.Add(badIp, &e2, &saveErr);
        Check("banlist: damaged or invalid files (12 shapes) are errors that leave the list untouched; empty and malformed "
              "entries cannot be added",
              rejected == 12 && untouched == 12 && addBad && addBadIp, std::to_string(rejected) + " rejected");
    }
    {
        // A path that exists but cannot be read is an error, never "missing, so start empty": the
        // working directory itself stands in for an unreadable ban list / server.json.
        server::BanList keep;
        std::string banErr, cfgErr, readErr, absentErr;
        const bool banRejected = !keep.Load(".", &banErr) && banErr.find("cannot read the ban list") != std::string::npos;
        server::Config cfg;
        bool cfgMissing = true;
        const bool cfgRejected = !server::LoadConfigFile(".", cfg, &cfgErr, &cfgMissing) && !cfgMissing;
        std::string text;
        bool dirMissing = true, absent = false;
        const bool dirRead = !server::ReadWholeFile(".", text, &readErr, &dirMissing) && !dirMissing;
        const bool absentRead = !server::ReadWholeFile("gage_server_test_no_such_file.json", text, &absentErr, &absent) && absent;
        Check("files: a path that exists but cannot be read is an error and NOT 'missing' (ban list, server.json, ReadWholeFile); "
              "only a path that does not exist is missing",
              banRejected && cfgRejected && dirRead && absentRead && readErr == "is a directory",
              banErr + " | " + cfgErr + " | " + readErr);
    }
    RemoveBanFiles(path);
}

// ===========================================================================
// D. UDP query against a live server
// ===========================================================================

struct RawUdp
{
    ENetSocket s;
    RawUdp()
    {
        s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    }
    ~RawUdp() { enet_socket_destroy(s); }
    void SendTo(uint16_t port, const void* data, size_t n)
    {
        ENetAddress a;
        enet_address_set_host_ip(&a, "127.0.0.1");
        a.port = port;
        ENetBuffer b;
        b.data = const_cast<void*>(data);
        b.dataLength = n;
        enet_socket_send(s, &a, &b, 1);
    }
    // One datagram if one is waiting, else 0.
    int Recv(uint8_t* buf, size_t cap)
    {
        ENetAddress from;
        ENetBuffer b;
        b.data = buf;
        b.dataLength = cap;
        const int n = enet_socket_receive(s, &from, &b, 1);
        return n > 0 ? n : 0;
    }
};

static query::Nonce MakeNonce(uint8_t seed)
{
    query::Nonce n;
    for (size_t i = 0; i < n.size(); i++) n[i] = (uint8_t)(seed + 17 * i);
    return n;
}

// Sends one request and pumps the rig until the reply (or the budget) arrives.
static bool AskQuery(Rig& rig, RawUdp& udp, const query::Nonce& nonce, query::Reply& out, int* sizeOut = nullptr, int ms = 1500)
{
    query::Request r;
    r.nonce = nonce;
    const query::Datagram d = query::Encode(r);
    udp.SendTo(rig.srv.port(), d.data(), d.size());
    uint8_t buf[128];
    int got = 0;
    rig.Until([&] { return (got = udp.Recv(buf, sizeof(buf))) > 0; }, ms);
    if (sizeOut) *sizeOut = got;
    return got == (int)query::kDatagramSize && query::Decode(buf, (size_t)got, out);
}

static void QueryLiveTests()
{
    net::EnetScope enetScope;
    {
        Rig rig;
        if (!rig.Start()) { Check("query live: server starts", false); return; }
        Probe a(rig), b(rig);
        a.Join("Alice");
        b.Join("Bob");
        RawUdp udp;
        query::Reply rep;
        int size = 0;
        const query::Nonce n1 = MakeNonce(0xA1);
        const bool got = AskQuery(rig, udp, n1, rep, &size);
        Check("query live: a request is answered with exactly 24 bytes carrying protocol 5 (kVersion), players 2, maxPlayers 8 and the nonce",
              got && size == 24 && rep.protocol == proto::kVersion && rep.protocol == 5 && rep.players == 2 && rep.maxPlayers == 8 && rep.nonce == n1,
              got ? "players " + std::to_string(rep.players) : "no reply");
        query::Reply rep2;
        const query::Nonce n2 = MakeNonce(0x5C);
        const bool got2 = AskQuery(rig, udp, n2, rep2);
        Check("query live: every reply echoes its own request's nonce (two different nonces)",
              got2 && rep2.nonce == n2 && rep2.nonce != n1);

        Probe c(rig);
        c.Join("Carol");
        query::Reply three;
        const bool got3 = AskQuery(rig, udp, MakeNonce(1), three);
        b.s.Disconnect();
        b.WaitClosed();
        rig.Until([&] { return rig.srv.Players().size() == 2; }, 2000);
        query::Reply backToTwo;
        const bool got4 = AskQuery(rig, udp, MakeNonce(2), backToTwo);
        Check("query live: the counts are live (3 after a join, 2 after a leave)",
              got3 && three.players == 3 && got4 && backToTwo.players == 2,
              std::to_string(three.players) + " then " + std::to_string(backToTwo.players));

        // Malformed datagrams are not answered and create nothing.
        const uint64_t answeredBefore = rig.srv.stats().queryAnswered;
        const size_t playersBefore = rig.srv.Players().size();
        const query::Datagram good = query::Encode([&] { query::Request r; r.nonce = MakeNonce(9); return r; }());
        std::vector<std::vector<uint8_t>> variants;
        variants.push_back(std::vector<uint8_t>(good.begin(), good.end() - 1));
        {
            std::vector<uint8_t> v(good.begin(), good.end());
            v.push_back(0);
            variants.push_back(v);
        }
        for (size_t idx : {0, 4, 5})
        {
            std::vector<uint8_t> v(good.begin(), good.end());
            v[idx] = (uint8_t)(v[idx] ^ 0x40);
            variants.push_back(v);
        }
        variants.push_back(std::vector<uint8_t>(24, 0));
        for (const auto& v : variants) udp.SendTo(rig.srv.port(), v.data(), v.size());
        uint8_t buf[128];
        int stray = 0;
        rig.Until([&] { stray += udp.Recv(buf, sizeof(buf)) > 0 ? 1 : 0; return false; }, 400);
        query::Reply control;
        const bool controlOk = AskQuery(rig, udp, MakeNonce(77), control);
        Check("query live: six malformed datagrams get no reply, are not counted as queries and create no peer; a valid one right after is answered",
              variants.size() == 6 && stray == 0 && rig.srv.stats().queryAnswered == answeredBefore + 1 && controlOk &&
                  rig.srv.Players().size() == playersBefore && a.Joined() && c.Joined());
    }
    {
        // The update ping on the wire: a server released with client 0.4.4.
        server::Config cfg = Rig::BaseConfig();
        cfg.latestClient = {0, 4, 4};
        Rig rig;
        if (!rig.Start(cfg)) { Check("query v2 live: server starts", false); return; }
        Probe a(rig);
        a.Join("Alice");
        RawUdp udp;
        // One raw round trip: the datagram out, the first datagram back (its bytes), pumping the rig meanwhile.
        auto ask = [&](const std::vector<uint8_t>& dg, std::vector<uint8_t>& got, int ms = 800) {
            udp.SendTo(rig.srv.port(), dg.data(), dg.size());
            uint8_t buf[128];
            int n = 0;
            rig.Until([&] { return (n = udp.Recv(buf, sizeof(buf))) > 0; }, ms);
            got.assign(buf, buf + (n > 0 ? n : 0));
            return n > 0;
        };
        const query::Nonce n1 = MakeNonce(0x31);
        // v1 request (byte for byte what 0.4.2 / 0.4.3 launchers and the master send).
        query::Request r1;
        r1.nonce = n1;
        const query::Datagram d1 = query::Encode(r1);
        std::vector<uint8_t> v1(d1.begin(), d1.end()), g1;
        const bool got1 = ask(v1, g1);
        // The exact v1 reply the same server would have sent before the update ping existed.
        query::Reply expect1;
        expect1.protocol = proto::kVersion;
        expect1.nonce = n1;
        expect1.players = 1;
        expect1.maxPlayers = 8;
        const query::Datagram e1 = query::Encode(expect1);
        auto legacyDecode = [](const std::vector<uint8_t>& g) { return g.size() == 24 && std::memcmp(g.data(), "GAGE", 4) == 0 && g[4] == 0x81 && g[5] == 1; };
        Check("query v2 live: a v1 request gets the byte-exact v1 reply (24 bytes, query version 1, bytes 20..23 zero) even though the server has a latest client "
              "configured, and the frozen 0.4.3 Decode accepts it",
              got1 && g1.size() == 24 && std::memcmp(g1.data(), e1.data(), 24) == 0 && legacyDecode(g1), got1 ? HexPlain(g1.data(), g1.size()) : "no reply");

        // v2 request: the same 24 bytes with the u16 at 6 = 2; the reply is v2 and carries 0.4.4.
        query::Request r2;
        r2.nonce = MakeNonce(0x32);
        r2.replyVersion = 2;
        const query::Datagram d2 = query::Encode(r2);
        std::vector<uint8_t> g2;
        const bool got2 = ask(std::vector<uint8_t>(d2.begin(), d2.end()), g2);
        query::Reply rep2;
        const bool dec2 = got2 && query::Decode(g2.data(), g2.size(), rep2);
        Check("query v2 live: a v2 request gets a 24-byte v2 reply: query version 2, protocol 5, players 1, maxPlayers 8, its own nonce, latest client 0.4.4 "
              "in bytes 20..22, byte 23 zero",
              got2 && g2.size() == 24 && dec2 && rep2.version == 2 && rep2.protocol == proto::kVersion && rep2.players == 1 && rep2.maxPlayers == 8 &&
                  rep2.nonce == r2.nonce && rep2.latestClient == query::ClientVersion{0, 4, 4} && g2[20] == 0 && g2[21] == 4 && g2[22] == 4 && g2[23] == 0 &&
                  !legacyDecode(g2),
              got2 ? HexPlain(g2.data(), g2.size()) : "no reply");

        // A v1 asker that left junk in the bytes v1 calls "zero" (or a future v3 asker) still gets the v1 reply; so does 2 with a stray byte 7.
        int junkWrong = 0;
        for (const std::pair<uint8_t, uint8_t>& b67 : std::vector<std::pair<uint8_t, uint8_t>>{{0, 0}, {1, 0}, {3, 0}, {0xFF, 0xFF}, {2, 1}, {0, 2}})
        {
            std::vector<uint8_t> v(d1.begin(), d1.end()), g;
            v[6] = b67.first;
            v[7] = b67.second;
            if (!ask(v, g) || !legacyDecode(g) || std::memcmp(g.data() + 8, d1.data() + 8, 8) != 0) junkWrong++;
        }
        // Not queries at all (no reply, no peer, not counted): a v2 request with a trailing byte or one byte short, and query version 2 in byte 5.
        const uint64_t answeredBefore = rig.srv.stats().queryAnswered;
        std::vector<uint8_t> t25(d2.begin(), d2.end()), t23(d2.begin(), d2.end() - 1), b5(d2.begin(), d2.end());
        t25.push_back(0);
        b5[5] = 2;
        int stray = 0;
        for (const std::vector<uint8_t>* m : {&t25, &t23, &b5})
        {
            std::vector<uint8_t> g;
            if (ask(*m, g, 250)) stray++;
        }
        std::vector<uint8_t> gc;
        const bool controlOk = ask(std::vector<uint8_t>(d2.begin(), d2.end()), gc);
        Check("query v2 live: junk in the reserved bytes (u16 at 6 = 0, 1, 3, 0xFFFF, 0x0102, 0x0200) gets the v1 reply; a v2 request with a trailing byte, "
              "one byte short, or query version 2 in byte 5 gets no reply and is not counted; a valid v2 request right after is answered, the player stays joined",
              junkWrong == 0 && stray == 0 && controlOk && rig.srv.stats().queryAnswered == answeredBefore + 1 && a.Joined() && rig.srv.Players().size() == 1,
              std::to_string(junkWrong) + " junk wrong, " + std::to_string(stray) + " stray");
    }
    {
        // A server with no latest client configured (the default): a v2 request still gets a v2 reply, 0.0.0 = unknown.
        Rig rig;
        if (!rig.Start()) { Check("query v2 live (no latest client): server starts", false); return; }
        RawUdp udp;
        query::Request r2;
        r2.nonce = MakeNonce(0x44);
        r2.replyVersion = 2;
        const query::Datagram d2 = query::Encode(r2);
        udp.SendTo(rig.srv.port(), d2.data(), d2.size());
        uint8_t buf[128];
        int n = 0;
        rig.Until([&] { return (n = udp.Recv(buf, sizeof(buf))) > 0; }, 800);
        query::Reply rep;
        const bool dec = n == 24 && query::Decode(buf, (size_t)n, rep);
        Check("query v2 live: a server with no latestClientVersion answers a v2 request with a v2 reply carrying 0.0.0 (unknown: a launcher never acts on it)",
              dec && rep.version == 2 && !rep.latestClient.Known() && rep.nonce == r2.nonce && buf[20] == 0 && buf[21] == 0 && buf[22] == 0);
    }
    {
        // Per-source rate limit: 5 replies per second per address here.
        server::Config cfg = Rig::BaseConfig();
        cfg.queryRatePerIp = 5;
        Rig rig;
        if (!rig.Start(cfg)) { Check("query rate: server starts", false); return; }
        Probe a(rig);
        a.Join("Alice");
        RawUdp udp;
        const query::Datagram d = query::Encode([&] { query::Request r; r.nonce = MakeNonce(3); return r; }());
        for (int i = 0; i < 40; i++) udp.SendTo(rig.srv.port(), d.data(), d.size());
        int replies = 0;
        uint8_t buf[128];
        rig.Until([&] {
            while (udp.Recv(buf, sizeof(buf)) > 0) replies++;
            return false;
        }, 500);
        const server::Stats st = rig.srv.stats();
        // The burst is one second of tokens (5); refill during the burst adds at most one.
        Check("query rate: 40 requests in a burst from one address get 5..7 replies, the rest are dropped silently and counted",
              replies >= 5 && replies <= 7 && st.queryAnswered + st.queryRateLimited == 40 && st.queryRateLimited >= 33,
              std::to_string(replies) + " replies, answered " + std::to_string(st.queryAnswered) + ", limited " +
                  std::to_string(st.queryRateLimited));
        rig.Pump(1300); // the bucket refills
        query::Reply again;
        const bool refilled = AskQuery(rig, udp, MakeNonce(4), again);
        Check("query rate: after the bucket refills the address is answered again, and the joined player was never disturbed",
              refilled && a.Joined() && rig.srv.Players().size() == 1);
    }
}

// ===========================================================================
// E. Master announce against a fake HTTP master
// ===========================================================================

class FakeMaster
{
public:
    enum class Mode { Normal, Chunked, NoLength, Truncated, Hang };
    struct Reply
    {
        int status = 200;
        std::string body;
        std::string headers; // extra header lines, each ending in \r\n
        Mode mode = Mode::Normal;
    };
    struct Req
    {
        std::string head, body;
        uint64_t atMs = 0;
    };

    ~FakeMaster() { Stop(); }

    bool Start()
    {
        if (!sock::Init()) return false;
        std::string err;
        listener_ = sock::ListenLoopback(0, &port_, &err);
        if (listener_ == sock::kInvalid)
        {
            std::printf("      fake master cannot listen: %s\n", err.c_str());
            return false;
        }
        th_ = std::thread([this] { Run(); });
        return true;
    }
    void Stop()
    {
        if (!th_.joinable()) return;
        stop_ = true;
        th_.join();
        sock::Close(listener_);
        listener_ = sock::kInvalid;
        sock::Cleanup();
    }
    uint16_t port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    // Reply number i (0-based, by request order) is script[i]; the last one repeats.
    void Script(std::vector<Reply> script)
    {
        std::lock_guard<std::mutex> lock(m_);
        script_ = std::move(script);
    }
    std::vector<Req> Requests() const
    {
        std::lock_guard<std::mutex> lock(m_);
        return reqs_;
    }

private:
    void Run()
    {
        while (!stop_)
        {
            if (sock::Wait(listener_, false, 50) != 1) continue;
            uint32_t peer = 0;
            const sock::Handle c = sock::Accept(listener_, &peer);
            if (c == sock::kInvalid) continue;
            Serve(c);
            sock::Close(c);
        }
    }

    static std::string Lower(std::string s)
    {
        for (char& ch : s)
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        return s;
    }

    bool ReadRequest(sock::Handle c, Req& rq)
    {
        std::string raw;
        const uint64_t deadline = net::NowMs() + 3000;
        size_t headEnd = std::string::npos, need = 0;
        while (net::NowMs() < deadline && !stop_)
        {
            if (headEnd == std::string::npos)
            {
                headEnd = raw.find("\r\n\r\n");
                if (headEnd != std::string::npos)
                {
                    const std::string head = Lower(raw.substr(0, headEnd));
                    size_t at = head.find("content-length:");
                    size_t len = 0;
                    if (at != std::string::npos) len = (size_t)std::strtoul(head.c_str() + at + 15, nullptr, 10);
                    need = headEnd + 4 + len;
                }
            }
            if (headEnd != std::string::npos && raw.size() >= need) break;
            if (sock::Wait(c, false, 50) != 1) continue;
            char buf[2048];
            size_t n = 0;
            const sock::Io r = sock::Recv(c, buf, sizeof(buf), &n);
            if (r == sock::Io::Ok) raw.append(buf, n);
            else if (r != sock::Io::WouldBlock) break;
        }
        if (headEnd == std::string::npos || raw.size() < need) return false;
        rq.head = raw.substr(0, headEnd);
        rq.body = raw.substr(headEnd + 4, need - headEnd - 4);
        rq.atMs = net::NowMs();
        return true;
    }

    void SendAll(sock::Handle c, const std::string& s)
    {
        size_t sent = 0;
        while (sent < s.size() && !stop_)
        {
            size_t n = 0;
            const sock::Io r = sock::Send(c, s.data() + sent, s.size() - sent, &n);
            if (r == sock::Io::Ok) sent += n;
            else if (r == sock::Io::WouldBlock) sock::Wait(c, true, 50);
            else break;
        }
    }

    void Serve(sock::Handle c)
    {
        Req rq;
        if (!ReadRequest(c, rq)) return;
        Reply rp;
        {
            std::lock_guard<std::mutex> lock(m_);
            reqs_.push_back(rq);
            const size_t idx = reqs_.size() - 1;
            if (!script_.empty()) rp = script_[idx < script_.size() ? idx : script_.size() - 1];
        }
        const std::string status = std::to_string(rp.status) + " X";
        switch (rp.mode)
        {
        case Mode::Normal:
            SendAll(c, "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(rp.body.size()) + "\r\nConnection: close\r\n" + rp.headers + "\r\n" + rp.body);
            break;
        case Mode::Chunked:
        {
            const size_t half = rp.body.size() / 2;
            char h1[16], h2[16];
            std::snprintf(h1, sizeof(h1), "%zx", half);
            std::snprintf(h2, sizeof(h2), "%zx", rp.body.size() - half);
            SendAll(c, "HTTP/1.1 " + status + "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n" + rp.headers + "\r\n" +
                           h1 + "\r\n" + rp.body.substr(0, half) + "\r\n" + h2 + "\r\n" + rp.body.substr(half) + "\r\n0\r\n\r\n");
            break;
        }
        case Mode::NoLength:
            SendAll(c, "HTTP/1.1 " + status + "\r\nConnection: close\r\n" + rp.headers + "\r\n" + rp.body);
            break;
        case Mode::Truncated:
            SendAll(c, "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(rp.body.size() + 50) +
                           "\r\nConnection: close\r\n\r\n" + rp.body);
            break;
        case Mode::Hang:
        {
            const uint64_t until = net::NowMs() + 12000;
            while (!stop_ && net::NowMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            break;
        }
        }
    }

    sock::Handle listener_ = sock::kInvalid;
    uint16_t port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    std::vector<Req> reqs_;
    std::vector<Reply> script_;
};

static const char* kOkBody = "{\"status\":\"ok\",\"key\":\"127.0.0.1:24800\",\"created\":true,\"ttlSeconds\":90,\"heartbeatSeconds\":30}";

static void AnnounceTests()
{
    {
        // The POST, its JSON, and the live player count in later heartbeats.
        FakeMaster fake;
        if (!fake.Start()) { Check("announce: fake master starts", false); return; }
        FakeMaster::Reply okReply;
        okReply.body = kOkBody;
        fake.Script({okReply});
        server::Config cfg = Rig::BaseConfig();
        cfg.name = "GAGE \"Test\" \\ Server";
        cfg.masterUrl = fake.url();
        cfg.gamemode = "race";
        cfg.lang = "pt-BR";
        cfg.url = "https://example.org/x";
        cfg.announceIntervalMs = 150; // the master says 30 s; the smaller of the two wins
        Rig rig;
        if (!rig.Start(cfg)) { Check("announce: server starts", false); return; }
        const bool first = rig.Until([&] { return fake.Requests().size() >= 1; }, 5000);
        FakeMaster::Req r0;
        if (first) r0 = fake.Requests()[0];
        JParser p;
        const bool parsed = first && p.parse(r0.body.data(), r0.body.size());
        const std::string wantHost = "Host: 127.0.0.1:" + std::to_string(fake.port());
        Check("announce: the first POST goes out at once to /announce with the right headers and a body that carries every field",
              first && r0.head.compare(0, 24, "POST /announce HTTP/1.1\r") == 0 && r0.head.find(wantHost) != std::string::npos &&
                  r0.head.find("Content-Type: application/json") != std::string::npos &&
                  r0.head.find("Content-Length: " + std::to_string(r0.body.size())) != std::string::npos && parsed &&
                  NumField(p.v, "port") == rig.srv.port() && StrField(p.v, "name") == cfg.name &&
                  StrField(p.v, "gamemode") == "race" && StrField(p.v, "lang") == "pt-BR" &&
                  StrField(p.v, "url") == "https://example.org/x" && NumField(p.v, "players") == 0 &&
                  NumField(p.v, "maxPlayers") == 8 && NumField(p.v, "protocol") == proto::kVersion && proto::kVersion == 5, // v5: the master list shows protocol 5
              first ? r0.head + "\n" + r0.body : "no request");

        Probe a(rig);
        a.Join("Alice");
        const bool sawOne = rig.Until([&] {
            const auto rs = fake.Requests();
            JParser q;
            return !rs.empty() && q.parse(rs.back().body.data(), rs.back().body.size()) && NumField(q.v, "players") == 1;
        }, 5000);
        rig.Pump(700);
        const auto rs = fake.Requests();
        std::vector<int64_t> gaps;
        for (size_t i = 1; i < rs.size(); i++) gaps.push_back((int64_t)(rs[i].atMs - rs[i - 1].atMs));
        std::sort(gaps.begin(), gaps.end());
        const int64_t median = gaps.empty() ? -1 : gaps[gaps.size() / 2];
        Check("announce: heartbeats follow at the configured interval (150 ms beats the master's 30 s) and report the live player count",
              sawOne && rs.size() >= 5 && median >= 100 && median <= 1200, std::to_string(rs.size()) + " requests, median gap " +
                                                                                std::to_string(median) + " ms");
        Check("announce: the first 200 is logged once (key, ttl, interval), not on every heartbeat",
              rig.LogHas("master: listed as 127.0.0.1:24800") && rig.LogHas("ttl 90 s") && [&] {
                  int n = 0;
                  for (const std::string& l : rig.logs)
                      if (l.find("master: listed") != std::string::npos) n++;
                  return n == 1;
              }());
        // Stop() ends the announcing for good.
        rig.srv.Stop();
        const size_t atStop = fake.Requests().size();
        std::this_thread::sleep_for(std::chrono::milliseconds(600)); // four intervals: any straggler would show
        Check("announce: after Stop() no further announce arrives", fake.Requests().size() == atStop && atStop >= 5,
              std::to_string(atStop) + " -> " + std::to_string(fake.Requests().size()));
    }
    {
        // Failures back off exponentially; Retry-After binds; the master's heartbeatSeconds is honoured.
        FakeMaster fake;
        if (!fake.Start()) { Check("backoff: fake master starts", false); return; }
        FakeMaster::Reply r500, r422, r503, rok;
        r500.status = 500;
        r422.status = 422;
        r422.body = "{\"error\":\"unreachable\",\"message\":\"no reply to the UDP query\"}";
        r503.status = 503;
        r503.body = "{\"error\":\"probe_busy\",\"message\":\"too many probes\"}";
        r503.headers = "Retry-After: 1\r\n";
        rok.body = "{\"status\":\"ok\",\"key\":\"k:1\",\"created\":false,\"ttlSeconds\":90,\"heartbeatSeconds\":1}";
        fake.Script({r500, r422, r503, rok});
        server::Config cfg = Rig::BaseConfig();
        cfg.masterUrl = fake.url();
        cfg.announceIntervalMs = 100000; // would be 100 s; the master's heartbeatSeconds:1 must win
        cfg.announceBackoffBaseMs = 200;
        cfg.announceBackoffMaxMs = 2000;
        Rig rig;
        if (!rig.Start(cfg)) { Check("backoff: server starts", false); return; }
        const bool five = rig.Until([&] { return fake.Requests().size() >= 5; }, 9000);
        const auto rs = fake.Requests();
        auto gap = [&](size_t i) { return i < rs.size() ? (int64_t)(rs[i].atMs - rs[i - 1].atMs) : (int64_t)-1; };
        const int64_t g1 = gap(1), g2 = gap(2), g3 = gap(3), g4 = gap(4);
        Check("backoff: after a 500 the retry waits ~base (200 ms), after a 422 ~double (400 ms)",
              five && g1 >= 150 && g1 < 700 && g2 >= 330 && g2 < 1300 && g2 > g1 * 13 / 10,
              std::to_string(g1) + " ms, " + std::to_string(g2) + " ms");
        Check("backoff: a 503 with Retry-After: 1 waits the full second although the backoff alone would be 800 ms",
              five && g3 >= 900 && g3 < 2500, std::to_string(g3) + " ms");
        Check("backoff: after the 200 the interval is the master's heartbeatSeconds (1 s), not the configured 100 s",
              five && g4 >= 800 && g4 < 2500, std::to_string(g4) + " ms");
        const auto c = rig.srv.announceCounters();
        Check("backoff: statuses are logged with the master's error code, message and hint (500, 422 unreachable, 503 probe_busy), "
              "then the recovery; counters say 3 failed",
              rig.LogHas("HTTP 500") && rig.LogHas("HTTP 422 unreachable: no reply to the UDP query") &&
                  rig.LogHas("could not reach UDP port") && rig.LogHas("HTTP 503 probe_busy") && rig.LogHas("master: listed") &&
                  rig.LogHas("(recovered)") && c.failed == 3 && c.ok >= 1,
              std::to_string(c.failed) + " failed, " + std::to_string(c.ok) + " ok");
    }
    {
        // A master that accepts and never answers must not stall the tick loop, and Stop() must not wait for it.
        FakeMaster fake;
        if (!fake.Start()) { Check("hang: fake master starts", false); return; }
        FakeMaster::Reply hang;
        hang.mode = FakeMaster::Mode::Hang;
        fake.Script({hang});
        server::Config cfg = Rig::BaseConfig();
        cfg.masterUrl = fake.url();
        Rig rig;
        if (!rig.Start(cfg)) { Check("hang: server starts", false); return; }
        Probe a(rig);
        a.Join("Alice");
        const bool stuck = rig.Until([&] { return fake.Requests().size() >= 1; }, 5000); // announcer is mid-request now
        const uint64_t t0 = rig.srv.stats().ticks;
        const auto w0 = Clock::now();
        rig.Pump(1000);
        const uint64_t ticks = rig.srv.stats().ticks - t0;
        Check("hang: with the master hung mid-request the server keeps ticking at about tickHz (20) and the player stays in",
              stuck && ticks >= 14 && ticks <= 26 && a.Joined(), std::to_string(ticks) + " ticks in " +
                                                                  std::to_string(MsSince(w0)) + " ms");
        a.s.Abandon();
        const auto s0 = Clock::now();
        rig.srv.Stop();
        const int64_t ms = MsSince(s0);
        Check("hang: Stop() cancels the in-flight announce and returns promptly (not after the 10 s request timeout)",
              !rig.srv.running() && ms < 1800, std::to_string(ms) + " ms");
    }
    {
        // Nothing listening at all: the failure is logged, the server carries on.
        uint16_t closedPort = 0;
        {
            std::string err;
            sock::Init();
            const sock::Handle h = sock::ListenLoopback(0, &closedPort, &err);
            sock::Close(h);
            sock::Cleanup();
        }
        server::Config cfg = Rig::BaseConfig();
        cfg.masterUrl = "http://127.0.0.1:" + std::to_string(closedPort);
        cfg.announceBackoffBaseMs = 200;
        Rig rig;
        if (!rig.Start(cfg)) { Check("dead master: server starts", false); return; }
        Probe a(rig);
        const bool joined = a.Join("Alice");
        const bool logged = rig.Until([&] { return rig.LogHas("master: announce to 127.0.0.1:") && rig.LogHas("failed"); }, 5000);
        Check("dead master: a refused connection is logged as a failed announce with the retry delay; play is unaffected",
              joined && logged && rig.LogHas("retry in") && a.Joined() && rig.srv.announceCounters().failed >= 1);
    }
    {
        // The HTTP client's response framing, directly.
        FakeMaster fake;
        if (!fake.Start()) { Check("http: fake master starts", false); return; }
        FakeMaster::Reply normal, chunked, nolen, trunc;
        normal.status = 429;
        normal.body = "{\"error\":\"rate_limited\"}";
        normal.headers = "Retry-After: 7\r\n";
        chunked.mode = FakeMaster::Mode::Chunked;
        chunked.body = "hello chunked world";
        nolen.mode = FakeMaster::Mode::NoLength;
        nolen.body = "until the connection closes";
        trunc.mode = FakeMaster::Mode::Truncated;
        trunc.body = "short";
        fake.Script({normal, chunked, nolen, trunc});
        server::HttpUrl u;
        std::string err;
        server::ParseHttpUrl(fake.url() + "/x", u, &err);
        server::HttpOptions opt;
        opt.totalTimeoutMs = 3000;
        server::HttpResponse r1, r2, r3, r4;
        std::string e1, e2, e3, e4;
        const bool ok1 = server::HttpPost(u, "application/json", "{\"a\":1}", opt, r1, &e1);
        const bool ok2 = server::HttpPost(u, "application/json", "{}", opt, r2, &e2);
        const bool ok3 = server::HttpPost(u, "application/json", "{}", opt, r3, &e3);
        const bool ok4 = server::HttpPost(u, "application/json", "{}", opt, r4, &e4);
        const auto reqs = fake.Requests();
        Check("http: Content-Length bodies, status and Retry-After are read; the request path and body arrive intact",
              ok1 && r1.status == 429 && r1.body == "{\"error\":\"rate_limited\"}" && r1.retryAfterSeconds == 7 && reqs.size() == 4 &&
                  reqs[0].head.compare(0, 15, "POST /x HTTP/1.") == 0 && reqs[0].body == "{\"a\":1}",
              e1);
        Check("http: a chunked response is de-chunked", ok2 && r2.status == 200 && r2.body == "hello chunked world", e2);
        Check("http: a response without a length runs to the end of the connection", ok3 && r3.body == "until the connection closes", e3);
        Check("http: a body cut short of its Content-Length is a failure, not a success", !ok4 && e4.find("truncated") != std::string::npos, e4);
    }
}

// ===========================================================================
// F. Chat flood limit
// ===========================================================================

static void ChatFloodTests()
{
    server::Config cfg = Rig::BaseConfig();
    cfg.chatBurst = 5;
    cfg.chatWindowMs = 1000; // one token per 200 ms
    Rig rig;
    if (!rig.Start(cfg)) { Check("flood: server starts", false); return; }
    Probe a(rig), b(rig);
    a.Join("Alice");
    b.Join("Bob");
    rig.Pump(100);
    const uint16_t aId = a.s.myId();

    for (int i = 0; i < 12; i++) a.s.SendChat("m" + std::to_string(i));
    rig.Until([&] { return rig.srv.stats().chatDelivered + rig.srv.stats().chatFlooded >= 12; }, 3000);
    rig.Pump(300);
    const server::Stats st = rig.srv.stats();
    std::vector<std::pair<uint16_t, std::string>> want;
    for (int i = 0; i < (int)st.chatDelivered; i++) want.push_back({aId, "m" + std::to_string(i)});
    int notices = 0;
    for (const auto& m : a.ChatLog())
        if (m.first == proto::kServerId && m.second.find("too fast") != std::string::npos) notices++;
    Check("flood: of 12 messages in a burst only the bucket's 5 (6 if a token refilled mid-burst) are delivered, the first ones, in order, to everyone else",
          st.chatDelivered >= 5 && st.chatDelivered <= 6 && st.chatFlooded == 12 - st.chatDelivered && b.ChatLog() == want,
          std::to_string(st.chatDelivered) + " delivered, " + std::to_string(st.chatFlooded) + " flooded, bob heard " +
              std::to_string(b.ChatLog().size()));
    Check("flood: the dropped messages cost the sender one rate-limited server notice (not one per message), no disconnect, and a log line",
          notices >= 1 && notices <= 2 && a.Joined() && rig.LogHas("chat flood"), std::to_string(notices) + " notices");

    // The bucket is per player: Bob is unaffected by Alice's flood.
    const uint64_t before = rig.srv.stats().chatDelivered;
    b.s.SendChat("bob speaks");
    const bool bobOk = rig.Until([&] { return rig.srv.stats().chatDelivered == before + 1; }, 2000);
    Check("flood: another player's bucket is independent (Bob can still talk while Alice is limited)", bobOk);

    // It is a token bucket, not a lifetime cap: tokens come back.
    rig.Pump(650); // about three tokens
    const uint64_t b2 = rig.srv.stats().chatDelivered;
    a.s.SendChat("back again");
    const bool refilled = rig.Until([&] { return rig.srv.stats().chatDelivered == b2 + 1; }, 2000);
    Check("flood: after waiting the bucket has refilled and Alice's next message goes through", refilled);
}

// ===========================================================================
// G. Per-address connection cap
// ===========================================================================

static void ConnectionCapTests()
{
    {
        server::Config cfg = Rig::BaseConfig();
        cfg.maxConnectionsPerIp = 4;
        Rig rig;
        if (!rig.Start(cfg)) { Check("cap: server starts", false); return; }
        Probe p1(rig), p2(rig), p3(rig), p4(rig), p5(rig), p6(rig);
        const bool four = p1.Join("P1") && p2.Join("P2") && p3.Join("P3") && p4.Join("P4");
        p5.Connect("P5");
        p5.WaitClosed();
        rig.Pump(100);
        const net::Event* ref = p5.First(ET::Refused);
        Check("cap: the 5th connection from one address is refused TooManyConnections (text names the limit); the first four stay in",
              four && p5.RefusedWith(proto::RefuseReason::TooManyConnections) && ref && ref->refuse.text.find("limit 4") != std::string::npos &&
                  !p5.First(ET::Welcome) && p1.Joined() && p2.Joined() && p3.Joined() && p4.Joined() &&
                  rig.srv.Players().size() == 4 && rig.srv.stats().refusedIpCap == 1,
              p5.Describe());
        p4.s.Disconnect();
        p4.WaitClosed();
        rig.Until([&] { return rig.srv.Players().size() == 3; }, 2000);
        const bool again = p6.Join("P6");
        Check("cap: a slot frees when a connection leaves, and the next one gets in", again && rig.srv.Players().size() == 4);
    }
    {
        // Connections that have not said Hello yet hold a slot too.
        server::Config cfg = Rig::BaseConfig();
        cfg.maxConnectionsPerIp = 4;
        cfg.helloTimeoutMs = 6000;
        Rig rig;
        if (!rig.Start(cfg)) { Check("cap pending: server starts", false); return; }
        Probe q1(rig), q2(rig), q3(rig), q4(rig), q5(rig);
        for (Probe* q : {&q1, &q2, &q3, &q4})
            q->Connect("Silent", [](net::SessionOptions& o) { o.autoHello = false; });
        rig.Until([&] {
            return q1.s.state() == net::SessionState::Joining && q2.s.state() == net::SessionState::Joining &&
                   q3.s.state() == net::SessionState::Joining && q4.s.state() == net::SessionState::Joining;
        }, 3000);
        q5.Connect("Late");
        q5.WaitClosed();
        Check("cap: connections still waiting for their Hello count against the cap (the 5th is refused)",
              q5.RefusedWith(proto::RefuseReason::TooManyConnections) && rig.srv.stats().refusedIpCap == 1, q5.Describe());
    }
}

// ===========================================================================
// H. Bans through the console
// ===========================================================================

static void BanTests()
{
    const std::string path = "gage_server_test_bans.json";
    RemoveBanFiles(path);
    std::string out;
    {
        server::Config cfg = Rig::BaseConfig();
        cfg.bansFile = path;
        Rig rig;
        if (!rig.Start(cfg)) { Check("bans: server starts", false); return; }

        // A name nobody is using: banned by name only, refused at Hello (case-insensitive).
        const server::CommandResult r1 = rig.srv.Execute("ban Mallory cheating at cards", out);
        const std::string banOut = out;
        Probe m(rig);
        m.Connect("mallory");
        m.WaitClosed();
        const net::Event* ref = m.First(ET::Refused);
        Check("bans: `ban <name> <reason>` bans a name that is not online; that name (any case) is refused Banned at Hello with the reason",
              r1 == server::CommandResult::Ok && banOut.find("name Mallory") != std::string::npos && m.RefusedWith(proto::RefuseReason::Banned) &&
                  ref && ref->refuse.text.find("cheating at cards") != std::string::npos && !m.First(ET::Welcome) &&
                  rig.srv.stats().refusedBanned == 1 && rig.LogHas("refuse Banned"),
              m.Describe());
        server::BanList onDisk;
        std::string lerr;
        const bool saved = onDisk.Load(path, &lerr) && onDisk.entries().size() == 1 && onDisk.FindName("Mallory") &&
                           onDisk.FindName("Mallory")->reason == "cheating at cards" && !FileExists(path + ".tmp");
        Check("bans: the ban was written to bans.json at once (valid, reason kept, no temp file left)", saved, lerr);

        // An innocent player can still join.
        Probe alice(rig);
        const bool aliceIn = alice.Join("Alice");

        // banip: cuts off everyone connected from the address and refuses it at connect, before Hello.
        const server::CommandResult r2 = rig.srv.Execute("banip 127.0.0.1 spam", out);
        alice.WaitClosed();
        Probe quiet(rig);
        quiet.Connect("Quiet", [](net::SessionOptions& o) { o.autoHello = false; });
        quiet.WaitClosed();
        Check("bans: `banip` cuts off a player on that address (Refused Banned) and refuses a new connection at connect, before it says Hello",
              aliceIn && r2 == server::CommandResult::Ok && alice.RefusedWith(proto::RefuseReason::Banned) && rig.srv.Players().empty() &&
                  quiet.RefusedWith(proto::RefuseReason::Banned) && !quiet.First(ET::Welcome),
              alice.Describe() + " | " + quiet.Describe());
        server::BanList onDisk2;
        onDisk2.Load(path, &lerr);
        Check("bans: both bans are in the file (an address entry and a name entry)",
              onDisk2.entries().size() == 2 && onDisk2.FindIp("127.0.0.1") && onDisk2.FindName("mallory"));

        // A connection that is waiting for its Hello when the address ban lands is cut off too.
        rig.srv.Execute("unban 127.0.0.1", out);
        Probe waiting(rig);
        waiting.Connect("Waiting", [](net::SessionOptions& o) { o.autoHello = false; });
        rig.Until([&] { return waiting.s.state() == net::SessionState::Joining; }, 2000);
        rig.srv.Execute("banip 127.0.0.1 again", out);
        waiting.WaitClosed();
        Check("bans: a connection already waiting for its Hello is refused Banned when its address gets banned",
              waiting.RefusedWith(proto::RefuseReason::Banned), waiting.Describe());
        rig.srv.Execute("unban 127.0.0.1", out); // leave only the name ban for the restart below... re-add the address ban:
        rig.srv.Execute("banip 127.0.0.1 persisted", out);
    }
    {
        // A new server process on the same file: both bans are still there.
        server::Config cfg = Rig::BaseConfig();
        cfg.bansFile = path;
        Rig rig;
        if (!rig.Start(cfg)) { Check("bans reload: server starts", false); return; }
        const bool loaded = rig.srv.bans().entries().size() == 2 && rig.LogHas("bans: 2 entries");
        Probe viaIp(rig);
        viaIp.Connect("Anyone", [](net::SessionOptions& o) { o.autoHello = false; });
        viaIp.WaitClosed();
        const bool ipStill = viaIp.RefusedWith(proto::RefuseReason::Banned);
        std::string o2;
        const server::CommandResult lift = rig.srv.Execute("unban 127.0.0.1", o2);
        Probe viaName(rig);
        viaName.Connect("MALLORY");
        viaName.WaitClosed();
        const bool nameStill = viaName.RefusedWith(proto::RefuseReason::Banned) && !viaName.First(ET::Welcome);
        Probe alice(rig);
        const bool aliceIn = alice.Join("Alice");
        Check("bans: after a restart the address ban and the name ban from the file are both still enforced (persisted across a reload)",
              loaded && ipStill && lift == server::CommandResult::Ok && nameStill && aliceIn,
              viaIp.Describe() + " | " + viaName.Describe());
        const server::CommandResult lift2 = rig.srv.Execute("unban MALLORY", o2);
        Probe mallory(rig);
        const bool malloryIn = mallory.Join("Mallory");
        server::BanList empty;
        std::string lerr;
        const bool fileEmpty = empty.Load(path, &lerr) && empty.entries().empty();
        const std::string text = ReadFileText(path);
        Check("bans: `unban <name>` lifts the ban at once, in memory and in bans.json (empty list written)",
              lift2 == server::CommandResult::Ok && malloryIn && fileEmpty && text.find("\"bans\": []") != std::string::npos, text);
    }
    RemoveBanFiles(path);
    {
        // ban <online player>: the name and the address, the player is refused with the reason.
        server::Config cfg = Rig::BaseConfig();
        cfg.bansFile = path;
        Rig rig;
        if (!rig.Start(cfg)) { Check("bans online: server starts", false); return; }
        Probe alice(rig), bob(rig);
        alice.Join("Alice");
        bob.Join("Bob");
        const uint16_t aliceId = alice.s.myId();
        const server::CommandResult r = rig.srv.Execute("ban " + std::to_string(aliceId) + " griefing", out);
        const std::string result = out;
        alice.WaitClosed();
        bob.WaitClosed();
        server::BanList onDisk;
        std::string lerr;
        onDisk.Load(path, &lerr);
        const server::BanEntry* e = onDisk.FindName("alice");
        Check("bans: `ban <id>` on an online player bans its name and address, refuses it Banned with the reason, and (same address) anyone else on it",
              r == server::CommandResult::Ok && result.find("name Alice") != std::string::npos && result.find("address 127.0.0.1") != std::string::npos &&
                  alice.RefusedWith(proto::RefuseReason::Banned) && alice.First(ET::Refused)->refuse.text.find("griefing") != std::string::npos &&
                  bob.RefusedWith(proto::RefuseReason::Banned) && rig.srv.Players().empty() && e && e->ip == "127.0.0.1" &&
                  e->reason == "griefing",
              alice.Describe() + " | " + bob.Describe());
        const server::CommandResult un = rig.srv.Execute("unban ALICE", out);
        Probe again(rig), bob2(rig);
        const bool both = again.Join("Alice") && bob2.Join("Bob");
        Check("bans: `unban <name>` removes the name+address entry together, so both can join again",
              un == server::CommandResult::Ok && out.find("1 entry") != std::string::npos && both && rig.srv.bans().entries().empty());
    }
    RemoveBanFiles(path);
    {
        // Console errors and the listing.
        Rig rig;
        if (!rig.Start()) { Check("bans console: server starts", false); return; }
        std::string o;
        auto cmd = [&](const std::string& line) { return rig.srv.Execute(line, o); };
        const bool e1 = cmd("ban") == server::CommandResult::Error && o.find("usage") != std::string::npos;
        const bool e2 = cmd("ban 999") == server::CommandResult::Error && o.find("no player with id 999") != std::string::npos;
        const bool e3 = cmd("ban bad<name>") == server::CommandResult::Error && o.find("not a player name") != std::string::npos;
        const bool e4 = cmd("banip 999.1.1.1") == server::CommandResult::Error && cmd("banip") == server::CommandResult::Error;
        const bool e5 = cmd("unban nobody") == server::CommandResult::Error && o.find("no ban matches") != std::string::npos &&
                        cmd("unban") == server::CommandResult::Error;
        const bool e6 = cmd(std::string("ban Foo bad") + char(1) + "reason") == server::CommandResult::Error;
        const bool ok1 = cmd("ban Zed worst reason ever") == server::CommandResult::Ok;
        const bool dup = cmd("ban zed") == server::CommandResult::Error && o.find("already banned") != std::string::npos;
        const bool ok2 = cmd("banip 192.0.2.44 scanner") == server::CommandResult::Ok;
        cmd("bans");
        const bool list = o.find("bans 2") != std::string::npos && o.find("name Zed") != std::string::npos &&
                          o.find("reason: worst reason ever") != std::string::npos && o.find("ip 192.0.2.44") != std::string::npos;
        cmd("help");
        const bool help = o.find("ban <id|name>") != std::string::npos && o.find("banip") != std::string::npos &&
                          o.find("unban <name|ip>") != std::string::npos && o.find("bans") != std::string::npos;
        Check("bans console: usage and errors (no argument, unknown id, bad name, bad address, nothing to unban, control character in a reason, duplicate), "
              "the listing and help",
              e1 && e2 && e3 && e4 && e5 && e6 && ok1 && dup && ok2 && list && help,
              std::string(e1 ? "" : "e1 ") + (e2 ? "" : "e2 ") + (e3 ? "" : "e3 ") + (e4 ? "" : "e4 ") + (e5 ? "" : "e5 ") +
                  (e6 ? "" : "e6 ") + (ok1 ? "" : "ok1 ") + (dup ? "" : "dup ") + (ok2 ? "" : "ok2 ") + (list ? "" : "list ") +
                  (help ? "" : "help ") + o);
    }
    {
        // A damaged ban list stops the server instead of silently unbanning everybody.
        WriteTextFile(path, "{ \"bans\": [ { \"name\": ");
        server::Config cfg = Rig::BaseConfig();
        cfg.bansFile = path;
        cfg.log = [](const std::string&) {};
        server::Server srv;
        std::string err;
        const bool started = srv.Start(cfg, &err);
        const std::string after = ReadFileText(path);
        Check("bans: a bans.json that exists but cannot be parsed makes Start() fail (naming the file), and the file is left alone",
              !started && err.find("ban list") != std::string::npos && err.find(path) != std::string::npos &&
                  after == "{ \"bans\": [ { \"name\": ",
              err);
    }
    RemoveBanFiles(path);
}

// ===========================================================================
// I. The loopback admin channel
// ===========================================================================

static const char* kAdminToken = "test-admin-token-0123456789abcdef";

struct AdminClient
{
    Rig& rig;
    sock::Handle h = sock::kInvalid;
    std::string buf;
    bool eof = false;

    explicit AdminClient(Rig& r) : rig(r) {}
    ~AdminClient() { sock::Close(h); }

    bool Connect(uint16_t port, const char* host = "127.0.0.1")
    {
        std::string err;
        h = sock::ConnectTcp(host, port, 1500, nullptr, &err);
        return h != sock::kInvalid;
    }
    void Pull()
    {
        char tmp[2048];
        for (;;)
        {
            size_t n = 0;
            const sock::Io r = sock::Recv(h, tmp, sizeof(tmp), &n);
            if (r == sock::Io::Ok)
                buf.append(tmp, n);
            else
            {
                if (r == sock::Io::Closed || r == sock::Io::Error) eof = true;
                break;
            }
        }
    }
    void Send(const std::string& text)
    {
        size_t sent = 0;
        while (sent < text.size())
        {
            size_t n = 0;
            const sock::Io r = sock::Send(h, text.data() + sent, text.size() - sent, &n);
            if (r == sock::Io::Ok) sent += n;
            else if (r == sock::Io::WouldBlock) rig.Step();
            else break;
        }
    }
    bool Line(std::string& out, int ms)
    {
        bool got = false;
        rig.Until([&] {
            Pull();
            const size_t nl = buf.find('\n');
            if (nl != std::string::npos)
            {
                out = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                got = true;
                return true;
            }
            return eof;
        }, ms);
        return got;
    }
    bool WaitEof(int ms)
    {
        rig.Until([&] {
            Pull();
            return eof;
        }, ms);
        return eof;
    }

    struct Result
    {
        bool got = false, ok = false;
        std::string text;
    };
    static Result Parse(const std::string& line)
    {
        Result r;
        JParser p;
        if (!p.parse(line.data(), line.size()) || p.v.t != JVal::Obj) return r;
        r.got = true;
        const JVal* ok = Member(p.v, "ok");
        r.ok = ok && ok->t == JVal::Bool && ok->b;
        const JVal* t = Member(p.v, "output");
        if (!t) t = Member(p.v, "error");
        if (t && t->t == JVal::Str) r.text = t->s;
        return r;
    }
    Result Call(const std::string& line, int ms = 2000)
    {
        Send(line + "\n");
        std::string reply;
        if (!Line(reply, ms)) return Result();
        return Parse(reply);
    }
};

static server::Config AdminConfig()
{
    server::Config cfg = Rig::BaseConfig();
    cfg.adminEphemeral = true;
    cfg.adminToken = kAdminToken;
    return cfg;
}

// An admin target is an id (decimal) or a name. An id wider than the wire's u16 must NOT wrap onto a real player
// (`kick 65536` used to kick player 0, `ban 65537` banned player 1). AllDigits caps an id at 5 digits, so 65536..99999
// are the wrapping range; a longer digit string is a name.
static void AdminIdTests()
{
    Rig rig;
    if (!rig.Start()) { Check("admin ids: server starts", false); return; }
    Probe alice(rig), bob(rig);
    const bool joined = alice.Join("Alice") && bob.Join("Bob");
    const uint16_t aId = alice.s.myId(), bId = bob.s.myId();
    std::string o;
    bool wrapped = true;
    std::string what;
    for (const unsigned long long off : {65536ull, 70000ull, 99000ull}) // AllDigits caps an id at 5 digits; longer digit strings are names
    {
        const std::string aWide = std::to_string(aId + off), bWide = std::to_string(bId + off);
        for (const char* verb : {"kick ", "heal ", "kill ", "ban "})
        {
            if (rig.srv.Execute(std::string(verb) + aWide, o) != server::CommandResult::Error) { wrapped = false; what += std::string(verb) + aWide + " "; }
            if (rig.srv.Execute(std::string(verb) + bWide, o) != server::CommandResult::Error) { wrapped = false; what += std::string(verb) + bWide + " "; }
        }
        if (rig.srv.Execute("armor " + aWide + " 5", o) != server::CommandResult::Error) { wrapped = false; what += "armor " + aWide + " "; }
    }
    rig.Pump(100);
    Check("admin ids: a decimal id wider than 16 bits (65536 + id ... 99999) never reaches player `id`: kick / heal / kill / ban / armor refuse it, "
          "nobody is kicked, killed or banned",
          joined && wrapped && rig.srv.Players().size() == 2 && rig.srv.bans().entries().empty(), what + " players=" + std::to_string(rig.srv.Players().size()));
}

static void AdminTests()
{
    {
        server::Config cfg = AdminConfig();
        Rig rig;
        if (!rig.Start(cfg)) { Check("admin: server starts", false); return; }
        const uint16_t port = rig.srv.adminPort();
        Check("admin: the channel is on an ephemeral loopback port when enabled, and off (port 0) by default",
              port != 0 && [] {
                  Rig plain;
                  return plain.Start() && plain.srv.adminPort() == 0;
              }(), std::to_string(port));

        {
            AdminClient c(rig);
            const bool conn = c.Connect(port);
            c.Send("auth wrong-token-0123456789abcdef\n");
            std::string line;
            const bool got = c.Line(line, 2000);
            const auto r = AdminClient::Parse(line);
            c.Send("list\n");
            const bool closed = c.WaitEof(2500);
            Check("admin: a wrong token gets {ok:false, bad token} and the connection is closed; nothing runs afterwards",
                  conn && got && r.got && !r.ok && r.text.find("bad token") != std::string::npos && closed && c.buf.empty(), line);
        }
        {
            AdminClient c(rig);
            c.Connect(port);
            c.Send("list\n");
            std::string line;
            const bool got = c.Line(line, 2000);
            const auto r = AdminClient::Parse(line);
            const bool closed = c.WaitEof(2500);
            Check("admin: a command before `auth` is refused (authentication required) and the connection closed",
                  got && !r.ok && r.text.find("authentication required") != std::string::npos && closed, line);
        }
        {
            AdminClient c(rig);
            c.Connect(port);
            c.Send("auth ");
            c.Send(std::string(2000, 'x'));
            std::string line;
            const bool got = c.Line(line, 2000);
            const auto r = AdminClient::Parse(line);
            Check("admin: a line over 1024 bytes without a newline is refused (line too long) and the connection closed",
                  got && !r.ok && r.text.find("too long") != std::string::npos && c.WaitEof(2500), line);
        }

        // The good path: auth, then the console commands.
        Probe alice(rig);
        alice.Join("Alice");
        rig.Pump(100);
        AdminClient c(rig);
        c.Connect(port);
        const auto auth = c.Call(std::string("auth ") + kAdminToken);
        const auto list = c.Call("list");
        const auto say = c.Call("say hello from the panel");
        rig.Until([&] { return alice.ChatLog().size() >= 1; }, 2000);
        const auto bad = c.Call("frobnicate");
        const auto help = c.Call("help");
        Check("admin: the right token is accepted ({ok:true, authenticated}); `list` shows the roster; `say` reaches the players; "
              "an unknown command is {ok:false}; `help` works",
              auth.got && auth.ok && auth.text == "authenticated" && list.ok && list.text.find("players 1/8") != std::string::npos &&
                  list.text.find("Alice") != std::string::npos && say.ok && alice.ChatLog().size() == 1 &&
                  alice.ChatLog()[0].second == "hello from the panel" && bad.got && !bad.ok &&
                  bad.text.find("unknown command") != std::string::npos && help.ok && help.text.find("unban") != std::string::npos,
              list.text);

        const auto kick = c.Call("kick alice");
        alice.WaitClosed();
        Check("admin: `kick <name>` over the channel executes: the player is cut off (Disconnected Kicked) and the roster empties",
              kick.ok && alice.DisconnectedWith(net::DisconnectReason::Kicked) && rig.Until([&] { return rig.srv.Players().empty(); }, 1000),
              kick.text + " | " + alice.Describe());

        const auto banip = c.Call("banip 203.0.113.9 from the panel");
        const auto bans = c.Call("bans");
        const auto unban = c.Call("unban 203.0.113.9");
        Check("admin: `banip`, `bans` and `unban` work through the channel exactly like on the console",
              banip.ok && bans.ok && bans.text.find("ip 203.0.113.9") != std::string::npos && unban.ok && rig.srv.bans().entries().empty(),
              bans.text);
        c.Send("quit\n");
        std::string quitLine;
        c.Line(quitLine, 2000);
        Check("admin: `quit` over the channel asks the server to stop (quitRequested), as on the console",
              rig.srv.quitRequested() && AdminClient::Parse(quitLine).got);
    }
    {
        // Loopback only: the listener must not answer on any other address of this machine.
        server::Config cfg = AdminConfig();
        Rig rig;
        if (!rig.Start(cfg)) { Check("admin loopback: server starts", false); return; }
        std::string lan;
        const bool haveLan = sock::OutboundIpv4(&lan) && lan != "127.0.0.1" && lan.compare(0, 4, "127.") != 0;
        std::string how = "no non-loopback IPv4 address on this host: NOT EXERCISED";
        bool reachable = false, connected = false;
        if (haveLan)
        {
            AdminClient c(rig);
            connected = c.Connect(rig.srv.adminPort(), lan.c_str());
            if (connected)
            {
                // The listener itself must not be there; this only records what a leak would have allowed.
                const auto r = c.Call(std::string("auth ") + kAdminToken, 1000);
                reachable = r.got && r.ok;
                how = "connected to " + lan + " but " + (reachable ? "AUTHENTICATED" : "got no service");
            }
            else
                how = "connection to " + lan + ":" + std::to_string(rig.srv.adminPort()) + " refused";
        }
        AdminClient lo(rig);
        const bool loOk = lo.Connect(rig.srv.adminPort()) && lo.Call(std::string("auth ") + kAdminToken).ok;
        // Bound to 127.0.0.1 only means the connection to any other local address is refused outright.
        Check(haveLan ? "admin: the channel is loopback-only (the listener refuses connections on the machine's own non-loopback address)"
                      : "admin: the channel is loopback-only (SKIPPED: this host has no non-loopback IPv4 address)",
              loOk && !connected && !reachable, how);
    }
    {
        // Brute force: ten failures, then even the right token is refused until the allowance refills.
        server::Config cfg = AdminConfig();
        Rig rig;
        if (!rig.Start(cfg)) { Check("admin lockout: server starts", false); return; }
        const uint16_t port = rig.srv.adminPort();
        int rejected = 0;
        for (int i = 0; i < 10; i++)
        {
            AdminClient c(rig);
            c.Connect(port);
            const auto r = c.Call("auth guess-number-" + std::to_string(i) + "-0123456789");
            if (r.got && !r.ok && r.text.find("bad token") != std::string::npos) rejected++;
        }
        AdminClient c(rig);
        c.Connect(port);
        const auto locked = c.Call(std::string("auth ") + kAdminToken);
        Check("admin: after 10 failed logins the right token is refused too (too many failed logins), so the token cannot be brute-forced",
              rejected == 10 && locked.got && !locked.ok && locked.text.find("too many failed logins") != std::string::npos,
              std::to_string(rejected) + " bad-token replies, then: " + locked.text);
    }
    {
        // The channel needs a token to start at all.
        server::Config cfg = Rig::BaseConfig();
        cfg.adminEphemeral = true;
        cfg.adminToken = "short";
        cfg.log = [](const std::string&) {};
        server::Server srv;
        std::string err;
        const bool started = srv.Start(cfg, &err);
        Check("admin: enabling the channel without a proper token (16+ characters) makes Start() fail", !started && err.find("adminToken") != std::string::npos,
              err);
    }
    {
        // ConstantTimeEquals is only used for the token; its answers must be plain equality.
        Check("admin: ConstantTimeEquals agrees with ==, including prefixes, empty strings and different lengths",
              server::ConstantTimeEquals("abc", "abc") && !server::ConstantTimeEquals("abc", "abd") &&
                  !server::ConstantTimeEquals("abc", "abcd") && !server::ConstantTimeEquals("abcd", "abc") &&
                  server::ConstantTimeEquals("", "") && !server::ConstantTimeEquals("", "a"));
    }
}

// ===========================================================================
// J. The authority rules' building blocks (apps/server/anticheat.h, vehicles.h), without a socket
// ===========================================================================

static proto::VehicleUpdate UnitDrive(uint16_t veh, float x, float y, float vy)
{
    proto::VehicleUpdate u;
    u.vehId = veh;
    u.k.x = x;
    u.k.y = y;
    u.k.z = 30.0f;
    u.k.vy = vy;
    u.rot = proto::QuatFromHeading(0.0f);
    u.flags = proto::kVehEngine;
    return u;
}

static void AuthorityUnitTests()
{
    namespace ac = server::ac;
    const uint32_t kPistol = 0x1B06D571u, kUnarmed = 0xA2719263u, kRunOver = 0xA36D413Eu;
    const uint32_t kGrenade = ac::Joaat("WEAPON_GRENADE");

    // ---- weapon table ----
    {
        const ac::WeaponRule pistol = ac::RuleFor(kPistol), half = ac::RuleFor(kPistol, 50), tiny = ac::RuleFor(kPistol, 10);
        Check("rules: joaat is case-insensitive and matches the known hashes; weapons map to their class (pistol handgun, fists melee, "
              "the engine's run-over cause run-over, a stranger unknown); the class caps scale with hitDamageScalePercent and never reach 0",
              ac::Joaat("Adder") == 0xB779A091u && ac::Joaat("WEAPON_PISTOL") == kPistol && ac::KindOf(kPistol) == ac::WeaponKind::Handgun &&
                  ac::KindOf(kUnarmed) == ac::WeaponKind::Melee && ac::KindOf(kRunOver) == ac::WeaponKind::RunOver &&
                  ac::KindOf(kGrenade) == ac::WeaponKind::Explosive && ac::KindOf(0x12345678u) == ac::WeaponKind::Unknown &&
                  ac::KindOf(ac::Joaat("WEAPON_FALL")) == ac::WeaponKind::Environment && pistol.maxHit == 160 && pistol.hitsPerShot == 2 &&
                  half.maxHit == 80 && tiny.maxHit == 16 && ac::RuleForKind(ac::WeaponKind::Melee, 10).maxHit == 10 &&
                  ac::NeedsShot(ac::WeaponKind::Melee) /* v5: a melee Hit needs a swing */ && ac::RuleOf(ac::WeaponKind::Melee).hitsPerShot == 2 &&
                  ac::NeedsShot(ac::WeaponKind::Unknown) && ac::NeedsShot(ac::WeaponKind::Handgun) &&
                  !ac::NeedsShot(ac::WeaponKind::Explosive) && !ac::NeedsShot(ac::WeaponKind::RunOver));
    }

    // ---- shot ring ----
    {
        ac::ShotLog log;
        log.OnState(5, kPistol, 1000); // the first state is only the baseline: a joiner whose counter is 5 has not fired
        const bool baselineOnly = log.Find(5, 1000, 500) == nullptr;
        log.OnState(5, kPistol, 1050);
        const bool unchanged = log.Find(5, 1050, 500) == nullptr && log.Find(6, 1050, 500) == nullptr;
        log.OnState(6, kPistol, 1100);
        ac::ShotLog::Slot* s6 = log.Find(6, 1300, 500);
        const bool found = s6 && s6->atMs == 1100 && s6->weapon == kPistol;
        const bool expires = log.Find(6, 1601, 500) == nullptr && log.Find(6, 1600, 500) != nullptr;
        // A wrap and a burst: 14 -> 2 passes 15, 0, 1, 2 (four shots between two states).
        ac::ShotLog w;
        w.OnState(14, kPistol, 0);
        w.OnState(2, kPistol, 100);
        const bool wrapped = w.Find(15, 100, 500) && w.Find(0, 100, 500) && w.Find(1, 100, 500) && w.Find(2, 100, 500) &&
                             !w.Find(14, 100, 500) && !w.Find(3, 100, 500);
        // A reordered Hit names a shot one or two ahead of the newest the server knows.
        ac::ShotLog a;
        a.OnState(3, kPistol, 0);
        const bool ahead = a.Ahead(4) && a.Ahead(11) && !a.Ahead(3) && !a.Ahead(12) && !ac::ShotLog().Ahead(1);
        // Credits: one shot, N hits.
        a.OnState(4, kPistol, 10);
        a.Find(4, 10, 500)->hits = 2;
        a.OnState(5, kGrenade, 20);
        ac::ShotLog::Slot* launch = a.FindLaunch(20 + 14000, 15000);
        const bool launchFound = launch && launch->weapon == kGrenade && a.FindLaunch(20 + 16000, 15000) == nullptr;
        launch->hits = 16;
        const bool launchSpent = a.FindLaunch(100, 15000) == nullptr;
        ac::WeaponHistory h;
        h.Note(kPistol, 100);
        h.Note(kUnarmed, 200);
        h.Note(kPistol, 300);
        const bool held = h.Held(kPistol, 700, 500) && !h.Held(kPistol, 801, 500) && h.Held(kUnarmed, 600, 500) && !h.Held(kGrenade, 300, 500);
        Check("shots: the ring stamps every counter value a state passes through (wraps 14 -> 2 stamp 15, 0, 1, 2), ignores the first state "
              "and repeats, expires after the window, spots a Hit one to eight shots ahead, finds an unspent explosive launch for 15 s, "
              "and the weapon history answers 'held in the last 500 ms'",
              baselineOnly && unchanged && found && expires && wrapped && ahead && launchFound && launchSpent && held);
    }

    // ---- movement budget ----
    {
        proto::PedState walk, car, ragdoll, para;
        car.move = proto::kMoveInVehicle;
        ragdoll.flags = proto::kPfRagdoll;
        para.flags = proto::kPfParachute;
        const float a = ac::AllowedMove(15.0f, 0.1), b = ac::AllowedMove(15.0f, 1.0), c = ac::AllowedMove(150.0f, 0.1);
        Check("movement: the displacement budget is speed * (server time + 0.25 s of jitter) + 2 m (15 m/s, 0.1 s -> 7.25 m; 1 s -> 20.75 m; "
              "150 m/s, 0.1 s -> 54.5 m) and a negative time still gets the jitter; vehicles, ragdolls, falls and parachutes use the "
              "vehicle limit, walking does not",
              std::fabs(a - 7.25f) < 0.01f && std::fabs(b - 20.75f) < 0.01f && std::fabs(c - 54.5f) < 0.01f &&
                  std::fabs(ac::AllowedMove(15.0f, -5.0) - 5.75f) < 0.01f && !ac::FastPed(walk) && ac::FastPed(car) && ac::FastPed(ragdoll) &&
                  ac::FastPed(para) && std::fabs(ac::Dist(Kin(0, 0, 0), Kin(3, 4, 12)) - 13.0f) < 0.001f);
    }

    // ---- connect rate ----
    {
        ac::ConnectLimiter lim;
        lim.Configure(3);
        const bool three = lim.Allow(1, 0) && lim.Allow(1, 100) && lim.Allow(1, 200);
        const bool fourth = lim.Allow(1, 300);
        const bool other = lim.Allow(2, 300); // another address is its own bucket
        const bool refill = lim.Allow(1, 20400); // 3 a minute = a token per 20 s
        const bool againEmpty = !lim.Allow(1, 20500);
        ac::ConnectLimiter off;
        off.Configure(0);
        bool offAll = true;
        for (int i = 0; i < 100; i++) offAll = offAll && off.Allow(7, 0);
        // A full table never locks a NEW address out (fail-open), and never grows past its bound.
        ac::ConnectLimiter full;
        full.Configure(5);
        for (uint32_t i = 0; i < ac::ConnectLimiter::kMaxSources; i++) full.Allow(1000 + i, 0);
        const bool failOpen = full.Allow(999999, 0) && full.tracked() <= ac::ConnectLimiter::kMaxSources;
        // Idle sources are swept when the table is full and room is needed.
        const bool swept = full.Allow(888888, 600000) && full.tracked() < ac::ConnectLimiter::kMaxSources;
        Check("connect rate: 3 a minute per address = 3 pass, the 4th is refused, other addresses are independent, a token returns every 20 s; "
              "0 = off; a table full of busy addresses lets a new one through without growing, and idle entries are swept",
              three && !fourth && other && refill && againEmpty && offAll && failOpen && swept,
              std::to_string(full.tracked()) + " tracked");
    }

    // ---- animation sanitising ----
    {
        proto::Anim an;
        an.flags = 0xFFFFFFFFu;
        an.playbackRate = 255;
        const bool c1 = ac::SanitizeAnim(an) && an.flags == 0x1FFu && an.playbackRate == 200;
        proto::Anim slow;
        slow.flags = 1;
        slow.playbackRate = 0;
        const bool c2 = ac::SanitizeAnim(slow) && slow.flags == 1 && slow.playbackRate == 10;
        proto::Anim fine;
        fine.flags = 0x1FFu;
        fine.playbackRate = 100;
        const bool c3 = !ac::SanitizeAnim(fine) && fine.flags == 0x1FFu && fine.playbackRate == 100;
        proto::Anim high;
        high.flags = 0x200u; // collision off: the lowest forbidden bit
        high.playbackRate = 100;
        const bool c4 = ac::SanitizeAnim(high) && high.flags == 0;
        Check("anim: SanitizeAnim keeps bits 0..8, strips every higher bit (collision off, physics override, ...), clamps the rate to 10..200 "
              "and reports whether it changed anything",
              c1 && c2 && c3 && c4);
    }

    // ---- vehicle store ----
    server::VehicleStore::Rules rules;
    rules.maxVehicles = 256;
    rules.perPlayer = 1;
    rules.perIp = 6;
    rules.emptyDespawnMs = 600000;
    rules.orphanMs = 1000;
    rules.fuelDrainPerMinute = 600;
    const proto::Quat rot = proto::QuatFromHeading(0.0f);
    proto::Kinematics origin;
    origin.z = 30.0f;
    using Code = proto::VehicleResultCode;
    {
        // Create checks everything that can refuse BEFORE it gives up the oldest empty adopted car.
        server::VehicleStore vs;
        vs.Configure(rules, 8);
        std::vector<uint16_t> ids;
        uint16_t id = 0;
        bool filled = true;
        for (int i = 0; i < 255; i++)
        {
            filled = filled && vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &id) == Code::Ok;
            ids.push_back(id);
        }
        uint16_t mine = 0;
        filled = filled && vs.Create(1, origin, rot, 0, 0, 0, 7, true, 10, 1, &mine) == Code::Ok && vs.Count() == 256;
        std::vector<uint16_t> despawned;
        uint16_t again = 0;
        const Code code = vs.Create(1, origin, rot, 0, 0, 0, 7, true, 20, 2, &again, &despawned);
        Check("vehicles: with the per-player cap reached and every id taken, Create is Limit and the car it would have evicted is still there "
              "(no eviction before a free id is known)",
              filled && code == Code::Limit && despawned.empty() && vs.Get(mine) != nullptr && vs.Count() == 256, proto::ToString(code));
    }
    {
        // 256 destroyed at once: at most 255 ids in one frame, the rest carried, every id told.
        server::VehicleStore vs;
        vs.Configure(rules, 8);
        vs.AddReceiver(0);
        bool created = true;
        for (int i = 0; i < 256; i++)
        {
            uint16_t id = 0;
            created = created && vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &id) == Code::Ok;
        }
        for (uint16_t id = 0; id < 256; id++) vs.Destroy(id, 0, 5, nullptr);
        std::set<uint16_t> told;
        size_t biggest = 0;
        int frames = 0;
        for (uint32_t tick = 6; tick < 14; tick++)
        {
            proto::VehicleWorld w;
            vs.BuildFrame(0, true, origin, tick, 0, w);
            biggest = std::max(biggest, w.removed.size());
            if (!w.removed.empty()) frames++;
            for (uint16_t r : w.removed) told.insert(r);
        }
        proto::VehicleWorld last;
        vs.BuildFrame(0, true, origin, 14, 0, last);
        Check("vehicles: 256 vehicles destroyed at once are announced at most 255 to a frame (the wire's u8), the surplus carries over, every id is "
              "told, and the notices stop after their 3 repeats",
              created && biggest == 255 && told.size() == 256 && frames >= 4 && frames <= 5 && last.removed.empty(),
              std::to_string(biggest) + " max, " + std::to_string(told.size()) + " told, " + std::to_string(frames) + " frames");
    }
    {
        // A leaver's empty cars go with it, the occupied ones become orphans that count against the address and time out once empty.
        server::VehicleStore::Rules r2 = rules;
        r2.maxVehicles = 64;
        r2.perPlayer = 3;
        r2.perIp = 3;
        server::VehicleStore vs;
        vs.Configure(r2, 8);
        vs.AddReceiver(0);
        vs.AddReceiver(1);
        uint16_t a = 0, b = 0, c = 0;
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &a);
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &b);
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &c);
        const Code in = vs.Enter(1, b, 1, origin, 100, 2); // v5: a passenger seat (the driver's seat of player 0's Spawn is NotOwner for player 1)
        std::vector<uint16_t> destroyed;
        size_t orphaned = 0;
        vs.PlayerGone(0, 200, 3, &destroyed, &orphaned);
        const server::VehicleStore::Vehicle* orphan = vs.Get(b);
        const bool shape = in == Code::Ok && destroyed.size() == 2 && orphaned == 1 && vs.Get(a) == nullptr && vs.Get(c) == nullptr && orphan &&
                           orphan->orphan && orphan->spawner == proto::kNoPlayer && vs.OwnedBy(7, false) == 1;
        uint16_t extra = 0;
        const bool addressCounts = vs.Create(1, origin, rot, 0, 0, 2, 7, false, 300, 4, &extra) == Code::Ok &&
                                   vs.Create(1, origin, rot, 0, 0, 2, 7, false, 300, 4, &extra) == Code::Ok &&
                                   vs.Create(1, origin, rot, 0, 0, 2, 7, false, 300, 4, &extra) == Code::Limit;
        std::vector<uint16_t> gone;
        vs.Tick(100000, 5, &gone); // occupied: no timer
        const bool occupiedStays = vs.Get(b) != nullptr && gone.empty();
        vs.Exit(1, b, 100000, 6);
        vs.Tick(100500, 7, &gone);
        const bool notYet = vs.Get(b) != nullptr;
        vs.Tick(101000, 8, &gone);
        Check("vehicles: PlayerGone destroys the leaver's EMPTY vehicles and orphans the occupied one (no spawner, still counted against the address); "
              "an orphan goes orphanMs after it empties, not before, and never while occupied",
              shape && addressCounts && occupiedStays && notYet && vs.Get(b) == nullptr && gone.size() == 1 && gone[0] == b);
    }
    {
        // Displacement: speed and fuel come from where the car went, not from what the driver claims.
        server::VehicleStore vs;
        vs.Configure(rules, 8);
        vs.AddReceiver(0);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 1000, 2);
        bool jumped = true;
        const bool claimed = vs.Update(0, UnitDrive(v, 0, 0, 100.0f), 1100, &jumped) == Code::Ok && !jumped; // claims 100 m/s, stays put
        const bool notMoving = !vs.MovedRecently(v, 1100, 2000);
        vs.Tick(1100, 3, nullptr);
        const bool noFuelUsed = vs.Get(v)->s.fuelPermille == 1000;
        bool allSteady = true;
        float y = 0;
        for (int i = 1; i <= 10; i++) // 20 m/s for a second
        {
            y += 2.0f;
            allSteady = allSteady && vs.Update(0, UnitDrive(v, 0, y, 0.0f), 1100 + 100 * i, &jumped) == Code::Ok && !jumped;
        }
        const bool moving = vs.MovedRecently(v, 2100, 500) && !vs.MovedRecently(v, 2700, 500);
        vs.Tick(2100, 4, nullptr);
        const int fuel = vs.Get(v)->s.fuelPermille; // 600 permille a minute = 10 a second
        bool j2 = false;
        const bool jump = vs.Update(0, UnitDrive(v, 0, y + 400.0f, 0.0f), 2200, &j2) == Code::Ok && j2; // 400 m in 0.1 s: taken, but flagged
        const bool taken = std::fabs(vs.Get(v)->s.k.y - (y + 400.0f)) < 0.01f;
        Check("vehicles: a driver claiming 100 m/s without moving is not 'moving' and burns no fuel; 20 m/s of real displacement for a second "
              "is moving and burns about 10 permille; a 400 m jump in 0.1 s is taken but reported as a jump",
              claimed && notMoving && noFuelUsed && allSteady && moving && fuel >= 988 && fuel <= 991 && jump && taken, std::to_string(fuel) + " permille");
    }
    {
        server::VehicleStore vs;
        vs.Configure(rules, 8);
        uint16_t v = 0;
        proto::Kinematics at = origin;
        at.x = 10.0f;
        vs.Create(1, at, rot, 0, 0, 0, 7, false, 0, 1, &v);
        proto::Kinematics closeBy = origin, distant = origin;
        closeBy.x = 12.0f;
        distant.x = 100.0f;
        Check("vehicles: NearestVehicleDistance measures to the closest networked vehicle (a huge number when there is none)",
              std::fabs(vs.NearestVehicleDistance(closeBy) - 2.0f) < 0.001f && vs.NearestVehicleDistance(distant) > 89.0f &&
                  server::VehicleStore().NearestVehicleDistance(closeBy) > 1e20f);
    }
}

// ===========================================================================
// v5 vehicles (W5): ownership, one controlling client, sample ages, motion-only entries
// ===========================================================================

static server::VehicleStore::Rules V5Rules()
{
    server::VehicleStore::Rules r;
    r.maxVehicles = 64;
    r.perPlayer = 3;
    r.perIp = 6;
    r.emptyDespawnMs = 600000;
    r.orphanMs = 60000;
    return r;
}

static proto::Kinematics At(float x, float y)
{
    proto::Kinematics k;
    k.x = x;
    k.y = y;
    k.z = 30.0f;
    return k;
}

static server::VehicleStore::ControlCandidate Cand(uint16_t id, float x, float y, bool trusted = true, bool hasState = true)
{
    server::VehicleStore::ControlCandidate c;
    c.id = id;
    c.pos = At(x, y);
    c.hasState = hasState;
    c.trusted = trusted;
    return c;
}

static void VehicleV5UnitTests()
{
    using Code = proto::VehicleResultCode;
    const proto::Quat rot = proto::QuatFromHeading(0.0f);
    const proto::Kinematics origin = At(0, 0);

    // Ownership (vehicles spec A7): a Spawn is owned by its spawner; nobody else may take its driver seat.
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &v); // p0 spawns
        const server::VehicleStore::Vehicle* V = vs.Get(v);
        const bool owned = V && V->owned && V->s.ownerId == 0;
        const Code seat1 = vs.Enter(1, v, 1, origin, 100, 2);         // a passenger seat is open to anybody
        const Code shuffle = vs.Enter(1, v, 0, origin, 110, 3);       // GTA's auto-shuffle to the driver seat: refused (v4: Ok)
        vs.Exit(1, v, 120, 4);
        const Code fromFoot = vs.Enter(1, v, 0, origin, 130, 5);      // the driver seat from the street: refused (v4: Ok)
        const Code owner = vs.Enter(0, v, 0, origin, 140, 6);         // the owner drives
        const Code notSeatTaken = vs.Enter(2, v, 0, origin, 150, 7);  // NotOwner is checked before SeatTaken
        const Code releaseByOther = vs.Release(1, v, 8);              // owner only
        vs.Exit(0, v, 160, 9);
        const Code release = vs.Release(0, v, 10);
        const bool unowned = !V->owned && V->s.ownerId == proto::kNoPlayer;
        const Code releaseAgain = vs.Release(0, v, 11);               // nothing left to release
        const Code afterRelease = vs.Enter(1, v, 0, origin, 170, 12); // now anybody may drive
        const Code releaseMissing = vs.Release(0, 200, 13);
        Check("vehicles v5: a Spawn is owned by its spawner (ownerId on the wire); a passenger seat is open to anybody, the driver seat is NotOwner for "
              "everybody else - from the shuffle, from the street and before SeatTaken - and Release (owner only) frees it",
              owned && seat1 == Code::Ok && shuffle == Code::NotOwner && fromFoot == Code::NotOwner && owner == Code::Ok &&
                  notSeatTaken == Code::NotOwner && releaseByOther == Code::NotAllowed && release == Code::Ok && unowned &&
                  releaseAgain == Code::NotAllowed && afterRelease == Code::Ok && releaseMissing == Code::NoVehicle,
              std::string(proto::ToString(shuffle)) + "/" + proto::ToString(fromFoot) + "/" + proto::ToString(notSeatTaken));
    }
    {
        // Register (adopted), server-spawned and orphaned vehicles are not owned; an owner who leaves orphans an occupied car unowned.
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
        uint16_t adopted = 0, byServer = 0, mine = 0;
        vs.Create(1, origin, rot, 0, 0, 0, 7, true, 0, 1, &adopted);                        // Register by p0
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, false, 0, 1, &byServer);       // console spawn
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &mine);                          // Spawn by p0
        const Code a = vs.Enter(1, adopted, 0, origin, 100, 2);
        const Code s = vs.Enter(2, byServer, 0, origin, 100, 2);
        const Code p = vs.Enter(3, mine, 1, origin, 100, 2); // p3 rides along in p0's car
        const bool flags = !vs.Get(adopted)->owned && !vs.Get(byServer)->owned && vs.Get(mine)->owned &&
                           vs.Get(adopted)->s.ownerId == proto::kNoPlayer && vs.Get(byServer)->s.ownerId == proto::kNoPlayer;
        size_t orphaned = 0;
        vs.PlayerGone(0, 200, 3, nullptr, &orphaned);
        const server::VehicleStore::Vehicle* o = vs.Get(mine);
        const bool orphan = orphaned == 2 && o && o->orphan && !o->owned && o->s.ownerId == proto::kNoPlayer;
        vs.Exit(3, mine, 300, 4);
        uint16_t fresh = 0;
        vs.Create(1, origin, rot, 0, 0, 1, 8, false, 400, 5, &fresh);
        const Code takeOrphan = vs.Enter(2, fresh, 0, origin, 500, 6); // p1's new Spawn still belongs to p1
        Check("vehicles v5: only a player's Spawn is owned (Register, a console spawn and an orphan are not, ownerId none); the owner leaving orphans an "
              "occupied car unowned",
              a == Code::Ok && s == Code::Ok && p == Code::Ok && flags && orphan && takeOrphan == Code::NotOwner,
              std::string(proto::ToString(a)) + "/" + proto::ToString(s) + "/" + proto::ToString(takeOrphan));
    }

    // One controlling client (vehicles spec C): the seat-0 driver; else a sticky nearest trusted player; the update comes from `ctrl`.
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 6; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 1000, 2);
        const bool driverControls = vs.Get(v)->s.ctrl == 0;
        vs.Update(0, UnitDrive(v, 0, 10, 5), 1100);
        vs.Exit(0, v, 1200, 3);
        // The driver got out: it stays the controller (initially the last driver) and the car keeps its motion.
        const bool lastDriverKeeps = vs.Get(v)->s.ctrl == 0 && vs.Get(v)->s.k.vy == 5.0f;
        // p0 walked 300 m away; p1 (50 m) and p2 (120 m) are in view. Receivers' relevance comes from BuildFrame.
        proto::VehicleWorld w;
        vs.BuildFrame(1, true, At(50, 0), 4, 1300, w);
        vs.BuildFrame(2, true, At(120, 0), 4, 1300, w);
        std::vector<server::VehicleStore::ControlCandidate> ps = {Cand(0, 300, 0), Cand(1, 50, 0), Cand(2, 120, 0)};
        vs.AssignControllers(1300, 4, ps);
        const bool nearest = vs.Get(v)->s.ctrl == 1;
        bool jumped = false;
        const Code fromCtrl = vs.Update(1, UnitDrive(v, 0, 10, 0), 1350, &jumped);
        const Code fromOld = vs.Update(0, UnitDrive(v, 0, 10, 0), 1360);
        const Code fromOther = vs.Update(2, UnitDrive(v, 0, 10, 0), 1370);
        // The controller disconnects: the next nearest takes over.
        vs.PlayerGone(1, 1400, 5);
        const bool cleared = vs.Get(v)->s.ctrl == proto::kNoPlayer;
        ps = {Cand(0, 300, 0), Cand(2, 120, 0)};
        vs.AssignControllers(1450, 6, ps);
        const bool next = vs.Get(v)->s.ctrl == 2 && vs.Update(2, UnitDrive(v, 0, 10, 0), 1460) == Code::Ok;
        Check("vehicles v5: the seat-0 driver controls; when it gets out it stays the controller, until it is 300 m away: then the nearest trusted player in "
              "view takes over - its Update is Ok, the old controller's and a bystander's are NotAllowed (v4: the driver only) - and a controller that "
              "disconnects is replaced by the next nearest",
              driverControls && lastDriverKeeps && nearest && fromCtrl == Code::Ok && fromOld == Code::NotAllowed &&
                  fromOther == Code::NotAllowed && cleared && next,
              "ctrl " + std::to_string(vs.Get(v)->s.ctrl));
    }
    {
        // Sticky within 250 m, new controllers within 200 m, trusted and in view only; with nobody the car stops.
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 8; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        proto::VehicleWorld w;
        auto view = [&](uint16_t p, float x, uint32_t tick) { vs.BuildFrame(p, true, At(x, 0), tick, 5000, w); };
        // p4 stands 230 m away: in view but outside the 200 m a NEW controller may be; p5 is untrusted; p6 has no state yet; p7 is not in view.
        view(4, 230, 2);
        view(5, 20, 2);
        view(6, 25, 2);
        std::vector<server::VehicleStore::ControlCandidate> ps = {Cand(4, 230, 0), Cand(5, 20, 0, false), Cand(6, 25, 0, true, false),
                                                                  Cand(7, 30, 0)};
        vs.AssignControllers(5000, 2, ps);
        const bool nobody = vs.Get(v)->s.ctrl == proto::kNoPlayer;
        // Somebody walks up (p3, 150 m) and takes it; a closer player (p2, 40 m) arriving later does not take it away.
        view(3, 150, 3);
        view(2, 40, 3);
        ps = {Cand(3, 150, 0), Cand(4, 230, 0)};
        vs.AssignControllers(5100, 3, ps);
        const bool first = vs.Get(v)->s.ctrl == 3;
        ps = {Cand(3, 150, 0), Cand(2, 40, 0)};
        vs.AssignControllers(5200, 4, ps);
        const bool sticky = vs.Get(v)->s.ctrl == 3;
        ps = {Cand(3, 249, 0), Cand(2, 40, 0)};
        vs.AssignControllers(5300, 5, ps);
        const bool hysteresis = vs.Get(v)->s.ctrl == 3; // 249 m: still inside the 250 m the holder keeps
        vs.Update(3, UnitDrive(v, 0, 0, 0), 5350); // keep the watchdog quiet
        ps = {Cand(3, 251, 0), Cand(2, 40, 0)};
        vs.AssignControllers(5400, 6, ps);
        const bool handover = vs.Get(v)->s.ctrl == 2;
        Check("vehicles v5: with nobody within 200 m, in view, trusted and with a position there is no controller; a holder is kept out to 250 m "
              "and a closer arrival does not displace it; beyond 250 m the nearest eligible player takes over",
              nobody && first && sticky && hysteresis && handover, "ctrl " + std::to_string(vs.Get(v)->s.ctrl));
    }
    {
        // Watchdog and parking: a driverless controller that sends nothing for 1500 ms is skipped for 10 s; no controller zeroes the motion.
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 4; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 1000, 2);
        vs.Update(0, UnitDrive(v, 0, 5, 20), 1100);
        vs.Exit(0, v, 1200, 3);
        proto::VehicleWorld w;
        vs.BuildFrame(1, true, At(30, 0), 3, 1200, w);
        vs.BuildFrame(2, true, At(60, 0), 3, 1200, w);
        std::vector<server::VehicleStore::ControlCandidate> ps = {Cand(0, 500, 0), Cand(1, 30, 0), Cand(2, 60, 0)};
        vs.AssignControllers(1300, 4, ps); // p0 is gone from reach: p1 takes over; it has 1500 ms to send something
        const bool p1 = vs.Get(v)->s.ctrl == 1;
        vs.AssignControllers(2799, 5, ps);
        const bool stillP1 = vs.Get(v)->s.ctrl == 1;
        vs.AssignControllers(2800, 6, ps); // 1500 ms of silence
        const bool skippedP1 = vs.Get(v)->s.ctrl == 2;
        proto::VehicleUpdate rolling = UnitDrive(v, 0, 5, 12); // p2 answers in time, with the car still rolling and its horn on
        rolling.flags |= proto::kVehHorn;
        rolling.steer = 0.5f;
        rolling.avx = 0.25f;
        vs.Update(2, rolling, 3000);
        vs.AssignControllers(4400, 7, ps);
        const bool p2Stays = vs.Get(v)->s.ctrl == 2;
        // p2 leaves; p1 is still skipped (until 12800), so nobody is left, and the car's motion is zeroed.
        ps = {Cand(0, 500, 0), Cand(1, 30, 0)};
        vs.PlayerGone(2, 5000, 8);
        vs.AssignControllers(5100, 10, ps);
        const server::VehicleStore::Vehicle* V = vs.Get(v);
        const bool nobody = V->s.ctrl == proto::kNoPlayer && V->s.k.vx == 0 && V->s.k.vy == 0 && V->s.k.vz == 0 && V->s.avx == 0 &&
                            V->s.steer == 0 && (V->s.flags & proto::kVehHorn) == 0;
        vs.AssignControllers(12801, 11, ps); // the skip is over
        const bool back = vs.Get(v)->s.ctrl == 1;
        Check("vehicles v5: a driverless controller silent for 1500 ms is skipped for 10 s and the next nearest is assigned; with nobody left the car's "
              "velocities are zeroed; the skip ends after 10 s",
              p1 && stillP1 && skippedP1 && p2Stays && nobody && back, "ctrl " + std::to_string(vs.Get(v)->s.ctrl));
    }

    // Ages and the controller-owned flags (vehicles spec B and E, network N1).
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 3; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 900, 2);
        vs.Update(0, UnitDrive(v, 0, 1, 10), 1000); // sample time = receipt time
        proto::VehicleWorld w;
        vs.BuildFrame(1, true, At(20, 0), 3, 1030, w);
        const int age30 = w.vehicles.size() == 1 ? w.vehicles[0].ageMs : -1;
        vs.Update(0, UnitDrive(v, 0, 2, 10), 1100, nullptr, 1080); // received at 1100, sampled at 1080 on the server's clock
        vs.BuildFrame(1, true, At(20, 0), 4, 1130, w);
        const int age50 = w.vehicles.size() == 1 ? w.vehicles[0].ageMs : -1;
        vs.BuildFrame(1, true, At(20, 0), 5, 2000, w);
        const int ageCap = w.vehicles.size() == 1 ? w.vehicles[0].ageMs : -1;
        vs.BuildFrame(1, true, At(20, 0), 6, 1050, w); // a tick slot earlier than the sample: age 0, never negative
        const int ageFloor = w.vehicles.size() == 1 ? w.vehicles[0].ageMs : -1;
        Check("vehicles v5: ageMs = tick time - sample time, capped at 255 and never negative (Update at 1000, frame at 1030 -> 30; sampled at 1080, frame at 1130 -> 50)",
              age30 == 30 && age50 == 50 && ageCap == 255 && ageFloor == 0,
              std::to_string(age30) + "/" + std::to_string(age50) + "/" + std::to_string(ageCap) + "/" + std::to_string(ageFloor));
    }
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 3; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 900, 2);
        proto::VehicleUpdate u = UnitDrive(v, 0, 1, 10);
        u.flags2 = proto::kVeh2Headlights | proto::kVeh2Interior | proto::kVeh2IndicatorR;
        u.doors = proto::kDoorFrontLeft | proto::kDoorTrunk;
        const Code fromCtrl = vs.Update(0, u, 1000);
        const bool stored = vs.Get(v)->s.flags2 == u.flags2 && vs.Get(v)->s.doors == u.doors;
        proto::VehicleUpdate other = UnitDrive(v, 0, 1, 10);
        other.flags2 = proto::kVeh2RoofDown;
        other.doors = proto::kDoorHood;
        const Code fromOther = vs.Update(1, other, 1010);
        const bool unchanged = vs.Get(v)->s.flags2 == u.flags2 && vs.Get(v)->s.doors == u.doors;
        proto::VehicleUpdate reserved = UnitDrive(v, 0, 1, 10);
        reserved.tyresRim = 0x04;                        // v6: flags2 has no reserved bit left; a rim bit without its tyre bit is the refusal
        const Code badFlags = vs.Update(0, reserved, 1020);
        reserved.tyresRim = 0;
        reserved.doors = 0x80;
        const Code badDoors = vs.Update(0, reserved, 1030);
        proto::VehicleWorld w;
        vs.BuildFrame(1, true, At(20, 0), 3, 1040, w);
        const bool wire = w.vehicles.size() == 1 && w.vehicles[0].flags2 == u.flags2 && w.vehicles[0].doors == u.doors &&
                          w.vehicles[0].ctrl == 0 && w.vehicles[0].ownerId == proto::kNoPlayer;
        Check("vehicles v5: flags2 (lights, indicators, roof) and doors are the controller's alone - taken from its Update, ignored from anybody else (NotAllowed), "
              "BadArg with a reserved doors bit or a rim bit without its tyre bit - and ride the VehicleWorld entry with ctrl and ownerId",
              fromCtrl == Code::Ok && stored && fromOther == Code::NotAllowed && unchanged && badFlags == Code::BadArg && badDoors == Code::BadArg && wire);
    }

    // Motion-only entries (network N1): a driven car's statics go out on change, on entering and about once a second; the frames between carry
    // 26 bytes instead of 40+.
    {
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 3; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 900, 2);
        int fulls = 0, motion = 0, driverFulls = 0, frames = 0;
        size_t motionBytes = 0, fullBytes = 0;
        std::vector<int> fullTicks;
        bool ageSet = true;
        for (uint32_t tick = 10; tick < 50; tick++)
        {
            const uint64_t now = (uint64_t)tick * 50;
            vs.Update(0, UnitDrive(v, 0, 0.5f * (float)tick, 10), now - 20);
            proto::VehicleWorld w;
            if (vs.BuildFrame(1, true, At(20, 0), tick, now, w) && w.vehicles.size() == 1)
            {
                frames++;
                const proto::VehicleState& s = w.vehicles[0];
                if (s.motionOnly)
                {
                    motion++;
                    motionBytes = proto::Encode(w).size();
                }
                else
                {
                    fulls++;
                    fullBytes = proto::Encode(w).size();
                    fullTicks.push_back((int)tick);
                }
                ageSet = ageSet && s.ageMs == 20;
            }
            proto::VehicleWorld mine;
            if (vs.BuildFrame(0, true, At(0, 0), tick, now, mine) && !mine.vehicles.empty()) driverFulls++;
        }
        // A frame is 8 header bytes, then id + header byte + the entry: 26 motion-only; 46 + 2 per occupant when full (v6: tyres, rim, windows; one occupant: 50).
        Check("vehicles v5: for a receiver of a driven car the entries are full for the first 3 frames and then about once a second (20 ticks) and motion-only "
              "(a 34-byte frame, entry 26 bytes) in between, every one with its age; the driver itself gets full states only at the parked rate",
              frames == 40 && ageSet && fulls >= 4 && fulls <= 5 && motion == frames - fulls && motionBytes == 8 + 26 && fullBytes == 8 + 2 + 46 + 2 &&
                  driverFulls <= 5,
              std::to_string(fulls) + " full, " + std::to_string(motion) + " motion-only, " + std::to_string(motionBytes) + "/" + std::to_string(fullBytes) +
                  " bytes, driver " + std::to_string(driverFulls));
    }
    {
        // Anything the motion-only entry leaves out and that changes is sent in full for 3 frames: flags2, doors, health, occupancy, ownership.
        server::VehicleStore vs;
        vs.Configure(V5Rules(), 8);
        for (uint16_t p = 0; p < 3; p++) vs.AddReceiver(p);
        uint16_t v = 0;
        vs.Create(1, origin, rot, 0, 0, 0, 7, false, 0, 1, &v);
        vs.Enter(0, v, 0, origin, 900, 2);
        uint32_t tick = 10;
        auto drive = [&](const proto::VehicleUpdate& u) {
            const uint64_t now = (uint64_t)tick * 50;
            vs.Update(0, u, now - 10);
            proto::VehicleWorld w;
            const bool got = vs.BuildFrame(1, true, At(20, 0), tick, now, w) && w.vehicles.size() == 1;
            tick++;
            return got ? (w.vehicles[0].motionOnly ? 1 : 2) : 0; // 1 motion-only, 2 full, 0 nothing
        };
        for (int i = 0; i < 6; i++) drive(UnitDrive(v, 0, (float)i, 10)); // settle: the fresh frames pass
        const int quiet = drive(UnitDrive(v, 0, 6, 10));
        proto::VehicleUpdate lights = UnitDrive(v, 0, 7, 10);
        lights.flags2 = proto::kVeh2Headlights;
        const int a1 = drive(lights), a2 = drive(lights), a3 = drive(lights), a4 = drive(lights);
        proto::VehicleUpdate doors = lights;
        doors.doors = proto::kDoorFrontLeft;
        const int b1 = drive(doors);
        proto::VehicleUpdate crash = doors;
        crash.bodyHealth = 700;
        for (int i = 0; i < 3; i++) drive(crash);
        const int c1 = drive(crash); // the health change was told in full for 3 frames and is quiet again (no refresh due yet)
        Check("vehicles v5: a change of flags2, doors or health puts the car's full entry in 3 frames, then the motion-only entries resume",
              quiet == 1 && a1 == 2 && a2 == 2 && a3 == 2 && a4 == 1 && b1 == 2 && c1 == 1,
              std::to_string(quiet) + " " + std::to_string(a1) + std::to_string(a2) + std::to_string(a3) + std::to_string(a4) + " " + std::to_string(b1) +
                  " " + std::to_string(c1));
    }
}

// ===========================================================================
// v5 combat (W5): ScaleHit, the server's damage scaling (apps/server/damage.h)
// ===========================================================================

static void DamageScaleUnitTests()
{
    namespace ac = server::ac;
    const server::Config d; // the shipped defaults: firearms 72, melee 50, explosives 100, run-over 100, headshots 300
    const int16_t head = server::kBoneHead, chest = 24818;
    const uint16_t carbine = 32, fist = 30;
    // Body shots: round to nearest, never to 0.
    const server::HitScale body = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, chest, 10.0f);
    const server::HitScale noBone = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, -1, 10.0f);
    const server::HitScale pistol = server::ScaleHit(d, ac::WeaponKind::Handgun, 26, -1, 10.0f);
    const server::HitScale punch = server::ScaleHit(d, ac::WeaponKind::Melee, fist, -1, 1.0f);
    const server::HitScale tiny = server::ScaleHit(d, ac::WeaponKind::Rifle, 1, -1, 10.0f);
    const server::HitScale blast = server::ScaleHit(d, ac::WeaponKind::Explosive, 100, -1, 10.0f);
    const server::HitScale car = server::ScaleHit(d, ac::WeaponKind::RunOver, 50, -1, 0.0f);
    const server::HitScale fall = server::ScaleHit(d, ac::WeaponKind::Environment, 30, -1, 0.0f);
    const bool bodyOk = body.damage == 23 && !body.headshot && noBone.damage == 23 && !noBone.headshot && pistol.damage == 19 && punch.damage == 15 &&
                        !punch.headshot && tiny.damage == 1 && blast.damage == 100 && car.damage == 50 && fall.damage == 30;
    // Headshots: 300 percent up to 5 m, falling linearly to 100 percent at 40 m (a sniper rifle: 300 m); the multiplier is the same in
    // front of a firearm class and never applies to fists.
    const server::HitScale h3 = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, head, 3.0f);
    const server::HitScale h5 = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, head, 5.0f);
    const server::HitScale h225 = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, head, 22.5f); // 200 percent
    const server::HitScale h40 = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, head, 40.0f);
    const server::HitScale h90 = server::ScaleHit(d, ac::WeaponKind::Rifle, carbine, head, 90.0f);
    const server::HitScale s5 = server::ScaleHit(d, ac::WeaponKind::Sniper, 100, head, 5.0f);
    const server::HitScale s1525 = server::ScaleHit(d, ac::WeaponKind::Sniper, 100, head, 152.5f); // 200 percent
    const server::HitScale s300 = server::ScaleHit(d, ac::WeaponKind::Sniper, 100, head, 300.0f);
    const server::HitScale fistHead = server::ScaleHit(d, ac::WeaponKind::Melee, fist, head, 1.0f);
    const server::HitScale blastHead = server::ScaleHit(d, ac::WeaponKind::Explosive, 100, head, 1.0f);
    const bool headOk = h3.damage == 69 && h3.headshot && h5.damage == 69 && h225.damage == 46 && h225.headshot && h40.damage == 23 && h40.headshot &&
                        h90.damage == 23 && s5.damage == 216 && s1525.damage == 144 && s300.damage == 72 && s300.headshot && fistHead.damage == 15 &&
                        !fistHead.headshot && blastHead.damage == 100 && !blastHead.headshot;
    // The v4 numbers: every percent at 100 is the identity (what tests/netrig.h BaseConfig() pins).
    server::Config legacy = d;
    legacy.firearmDamagePercent = legacy.meleeDamagePercent = legacy.explosiveDamagePercent = legacy.runOverDamagePercent = 100;
    legacy.headshotPercent = 100;
    bool identity = true;
    for (ac::WeaponKind k : {ac::WeaponKind::Unknown, ac::WeaponKind::Melee, ac::WeaponKind::Handgun, ac::WeaponKind::Smg, ac::WeaponKind::Rifle,
                             ac::WeaponKind::MachineGun, ac::WeaponKind::Shotgun, ac::WeaponKind::Sniper, ac::WeaponKind::Explosive,
                             ac::WeaponKind::RunOver, ac::WeaponKind::Environment})
        for (uint16_t claimed : {1, 7, 100, 250})
            identity = identity && server::ScaleHit(legacy, k, claimed, head, 3.0f).damage == claimed;
    // Operator knobs: firearms at 100 percent and a headshot multiplier of 500 (the cap of the key is 1000).
    server::Config custom = d;
    custom.firearmDamagePercent = 100;
    custom.headshotPercent = 500;
    const server::HitScale c3 = server::ScaleHit(custom, ac::WeaponKind::Rifle, carbine, head, 3.0f);
    // Damage flags: the wire bits of the Damage message.
    const uint8_t bits = server::DamageFlags(true, ac::WeaponKind::Melee, true);
    const uint8_t blastBits = server::DamageFlags(false, ac::WeaponKind::Explosive, false);
    Check("damage: ScaleHit rounds firearms 72 percent / melee 50 percent / explosives and run-over 100 percent to the nearest point (carbine 32 -> 23, pistol 26 -> 19, "
          "fist 30 -> 15, never 0), the head bone pays 300 percent up to 5 m falling to 100 percent at 40 m (a sniper rifle 300 m) for firearms only "
          "(carbine at 3 m 69), all percents at 100 are the identity, and the Damage flag bits are headshot 1, melee 2, explosion 4, armour 8",
          bodyOk && headOk && identity && c3.damage == 160 && bits == (proto::kDmgHeadshot | proto::kDmgMelee | proto::kDmgArmour) &&
              blastBits == proto::kDmgExplosion,
          std::to_string(body.damage) + "/" + std::to_string(h3.damage) + "/" + std::to_string(h225.damage) + "/" + std::to_string(s1525.damage));

    // Hits to kill at 200 health (the spec's table): carbine 9, pistol 11, fists 14, carbine headshot at 3 m 3.
    auto hitsToKill = [&](ac::WeaponKind k, uint16_t claimed, int16_t bone, float dist) {
        int hp = 200, n = 0;
        while (hp > 0 && n < 100)
        {
            hp -= server::ScaleHit(d, k, claimed, bone, dist).damage;
            n++;
        }
        return n;
    };
    Check("damage: at 200 health and no armour a carbine (32) takes 9 hits, a pistol (26) 11, fists (30) 14, a carbine headshot at 3 m 3",
          hitsToKill(ac::WeaponKind::Rifle, 32, chest, 10) == 9 && hitsToKill(ac::WeaponKind::Handgun, 26, chest, 10) == 11 &&
              hitsToKill(ac::WeaponKind::Melee, 30, -1, 1) == 14 && hitsToKill(ac::WeaponKind::Rifle, 32, head, 3) == 3,
          std::to_string(hitsToKill(ac::WeaponKind::Rifle, 32, chest, 10)) + "/" + std::to_string(hitsToKill(ac::WeaponKind::Handgun, 26, chest, 10)) + "/" +
              std::to_string(hitsToKill(ac::WeaponKind::Melee, 30, -1, 1)) + "/" + std::to_string(hitsToKill(ac::WeaponKind::Rifle, 32, head, 3)));
}

// ===========================================================================
// N6c: the tick clock follows the wall clock, sample ages are against the nominal tick time
// ===========================================================================


// W8 item 1 (docs/sync-specs/live-0.4.2-fixes.md): no silent pins. A valid server.json that pins rateNearHz under tickHz runs, but
// the operator is told in words, at Server::Start, and the effective replication and combat numbers are in the log.
static void W8ConfigAndLogTests()
{
    const char* path = "gage_server_test_w8.json";
    SetEnv("GAGE_ADMIN_TOKEN", "");
    {
        WriteTextFile(path, "{ \"tickHz\": 30, \"rateNearHz\": 20 }");
        server::Config pinned;
        std::string err;
        const bool ok = server::LoadConfigFile(path, pinned, &err);
        const std::vector<std::string> w = server::ConfigWarnings(pinned);
        WriteTextFile(path, "{ \"tickHz\": 20, \"rateNearHz\": 20 }");
        server::Config same;
        const bool ok2 = server::LoadConfigFile(path, same, &err);
        WriteTextFile(path, "{ \"tickHz\": 30, \"rateNearHz\": 60 }");
        server::Config above;
        const bool ok3 = server::LoadConfigFile(path, above, &err);
        std::remove(path);
        const bool text = w.size() == 1 && w[0].find("rateNearHz (20)") != std::string::npos && w[0].find("tickHz (30)") != std::string::npos &&
                          w[0].find("reach each other at 20/s, not 30/s") != std::string::npos;
        Check("config warning: {tickHz 30, rateNearHz 20} loads (a warning never fails Validate) and warns once, naming both values and the effect "
              "(\"reach each other at 20/s, not 30/s\"); the shipped defaults, 20/20 and 30/60 warn nothing",
              ok && ok2 && ok3 && text && server::ConfigWarnings(server::Config{}).empty() && server::ConfigWarnings(same).empty() &&
                  server::ConfigWarnings(above).empty(),
              w.empty() ? err : w[0]);
    }
    {
        net::EnetScope enetScope;
        auto hasLine = [](const std::vector<std::string>& logs, const char* needle) {
            for (const std::string& l : logs)
                if (l.find(needle) != std::string::npos) return true;
            return false;
        };
        server::Config pinned = Rig::BaseConfig();
        pinned.tickHz = 30;
        pinned.repl.rateNearHz = 20;
        pinned.playerHealth = 150;
        pinned.meleeDamagePercent = 35;
        pinned.latestClient = {0, 4, 4};
        bool pinnedOk, plainOk;
        std::vector<std::string> pinnedLogs, plainLogs;
        {
            Rig rig;
            pinnedOk = rig.Start(pinned);
            pinnedLogs = rig.logs;
        }
        {
            Rig rig;
            plainOk = rig.Start(Rig::BaseConfig());
            plainLogs = rig.logs;
        }
        Check("server start log: the effective replication numbers, playerHealth and every damage percent are logged, and a pinned rateNearHz 20 under tickHz 30 "
              "logs a WARNING line (none for the matching default); the update ping line names the latest client, or says none is configured",
              pinnedOk && plainOk && hasLine(pinnedLogs, "replication (effective): tickHz 30") && hasLine(pinnedLogs, "rates near/mid/far/idle/coarse 20/10/5/") &&
                  hasLine(pinnedLogs, "combat (effective): playerHealth 150, damage firearm 100% melee 35% explosive 100% run-over 100%") &&
                  hasLine(pinnedLogs, "WARNING: rateNearHz (20) is below tickHz (30)") && hasLine(plainLogs, "combat (effective): playerHealth 100") &&
                  !hasLine(plainLogs, "WARNING") &&
                  hasLine(pinnedLogs, "update ping: the UDP query tells launchers the latest client is 0.4.4") &&
                  hasLine(plainLogs, "update ping: no latest client configured"),
              std::to_string(pinnedLogs.size()) + " lines");
    }
}

// W8 (live test 0.4.2, "a punch while running one-shots and kills"): the per-Hit ceiling of a melee Hit by weapon, before ScaleHit.
static void W8MeleeCeilingTests()
{
    namespace ac = server::ac;
    const server::Config d; // shipped: melee 50 percent
    const uint32_t unarmed = ac::Joaat("WEAPON_UNARMED"), knuckle = ac::Joaat("WEAPON_KNUCKLE"), knife = ac::Joaat("WEAPON_KNIFE"),
                   bat = ac::Joaat("WEAPON_BAT"), pistol = ac::Joaat("WEAPON_PISTOL");
    const auto after = [&](uint32_t weapon) { return server::ScaleHit(d, ac::WeaponKind::Melee, std::min<uint16_t>(1000, ac::MeleeNominalMax(weapon)), -1, 1.0f).damage; };
    Check("melee ceiling: fists and knuckles 40, a knife 60, a bat 80, a firearm used as a club and an unknown weapon 40 (the class cap was 100 for all); a Hit claiming 1000 is "
          "worth 20 / 30 / 40 / 20 after the 50 percent melee scale; the ceilings follow hitDamageScalePercent (50 -> 20, 10 -> 4) and never reach 0",
          ac::MeleeNominalMax(unarmed) == 40 && ac::MeleeNominalMax(knuckle) == 40 && ac::MeleeNominalMax(knife) == 60 && ac::MeleeNominalMax(bat) == 80 &&
              ac::MeleeNominalMax(pistol) == 40 && ac::MeleeNominalMax(0x12345678u) == 40 && after(unarmed) == 20 && after(knife) == 30 && after(bat) == 40 &&
              after(pistol) == 20 && ac::MeleeNominalMax(unarmed, 50) == 20 && ac::MeleeNominalMax(unarmed, 10) == 4 && ac::MeleeNominalMax(unarmed, 1) >= 1 &&
              ac::RuleOf(ac::WeaponKind::Melee).maxHit == 100,
          std::to_string(ac::MeleeNominalMax(unarmed)) + "/" + std::to_string(ac::MeleeNominalMax(knife)) + "/" + std::to_string(ac::MeleeNominalMax(bat)));
}

static void SpinUntil(Clock::time_point t)
{
    while (Clock::now() < t) {}
}

static void StallTests()
{
    {
        // Bob moves (so Alice is sent his entry on every tick), Alice stands; both keep sending. Then nobody polls anything
        // for 500 ms (a hitch of the server's host, a long GC pause, a debugger stop). The WorldState tick numbers Alice
        // sees must jump by the 10 periods that passed: the client places every sample at tick * tickMs, so a counter that
        // advances by one after a stall puts the whole stall into the arrival offset of the next frame.
        Rig rig;
        if (!rig.Start()) { Check("stall: server starts", false); return; }
        Probe a(rig), b(rig);
        const bool joined = a.Join("Alice") && b.Join("Bob");
        float x = 10;
        auto play = [&](int ms) {
            const auto t0 = Clock::now();
            auto next = t0;
            while (MsSince(t0) < ms)
            {
                if (Clock::now() >= next)
                {
                    a.s.SendState(Kin(0, 0, 30));
                    b.s.SendState(Kin(x, 0, 30, 0, 5.0f));
                    x += 0.25f;
                    next += std::chrono::milliseconds(50);
                }
                rig.Step();
            }
        };
        play(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        play(1000);
        uint32_t maxGap = 0, gaps = 0;
        for (size_t i = 1; i < a.worldTicks.size(); i++)
        {
            const uint32_t gap = a.worldTicks[i] - a.worldTicks[i - 1];
            maxGap = std::max(maxGap, gap);
            if (gap >= 9) gaps++;
        }
        Check("stall: after the server is not polled for 500 ms the next WorldState's tick is 9..14 above the previous one (the wall clock's 10 periods; it was 1), once, and the frames after it are consecutive again",
              joined && a.worldTicks.size() > 30 && maxGap >= 9 && maxGap <= 14 && gaps == 1 && a.worldTicks.back() > a.worldTicks[a.worldTicks.size() / 2],
              "max gap " + std::to_string(maxGap) + ", " + std::to_string(gaps) + " big gaps in " + std::to_string(a.worldTicks.size()) + " frames");
    }
    {
        // Ages are measured against the tick's NOMINAL time (its slot), not against whenever the loop got round to it. The
        // measurement is spun, not slept (a 15 ms timer step would swamp it): align to a tick (time 0), deliver Bob's
        // sample at +8 ms, and let the next tick, whose slot is +50 ms, run at +90 ms (40 ms late). Against the slot the
        // sample is ~42 ms old; against the clock of the late tick it would be ~82 ms.
        Rig rig;
        if (!rig.Start()) { Check("ages: server starts", false); return; }
        Probe a(rig), b(rig);
        const bool joined = a.Join("Alice") && b.Join("Bob");
        float x = 10;
        auto play = [&](int ms) {
            const auto t0 = Clock::now();
            auto next = t0;
            while (MsSince(t0) < ms)
            {
                if (Clock::now() >= next)
                {
                    a.s.SendState(Kin(0, 0, 30));
                    b.s.SendState(Kin(x, 0, 30, 0, 5.0f));
                    x += 0.25f;
                    next += std::chrono::milliseconds(50);
                }
                rig.Step();
            }
        };
        play(600);
        std::vector<int> ages;
        for (int round = 0; round < 5 && joined; round++)
        {
            const uint64_t t0 = rig.srv.stats().ticks;
            while (rig.srv.stats().ticks == t0) rig.srv.Poll(0); // the server has just run a tick
            const auto tick = Clock::now();
            SpinUntil(tick + std::chrono::milliseconds(4));
            b.s.SendState(Kin(x, 0, 30, 0, 5.0f));
            x += 0.25f;
            b.s.Poll(0); // out of the client's queue and onto the socket
            SpinUntil(tick + std::chrono::milliseconds(8));
            rig.srv.Poll(0); // the sample is received here; the next slot is ~42 ms away, so no tick yet
            SpinUntil(tick + std::chrono::milliseconds(90));
            const size_t n0 = a.worldTicks.size();
            rig.srv.Poll(0); // the tick that was due at +50 ms runs at +90 ms
            const auto t1 = Clock::now();
            while (a.worldTicks.size() == n0 && MsSince(t1) < 500)
            {
                a.s.Poll(0);
                a.Drain();
            }
            if (a.worldTicks.size() > n0)
                if (const proto::WorldEntry* e = a.Seen(b.s.myId())) ages.push_back(e->ageMs);
            play(200); // settle: acks, the next slot
        }
        std::sort(ages.begin(), ages.end());
        const int median = ages.empty() ? -1 : ages[ages.size() / 2];
        std::string all;
        for (int v : ages) all += std::to_string(v) + " ";
        Check("ages: a sample received 8 ms after a tick is aged against the next tick's slot (~42 ms), not the clock of a tick that ran 40 ms late (~82 ms): median of 5 in 30..60 ms",
              joined && ages.size() == 5 && median >= 30 && median <= 60, "ages " + all);
    }
}


// ---- sync pipeline review (2026-11-03): ENet throttle, per-tier counters, the sync log line ---------------------------------------------

// ENet's own bookkeeping, as protocol.c keeps it on every acknowledgement (enet_protocol_handle_acknowledge), around the real
// enet_peer_throttle(): a model of how the packet throttle reacts to a round-trip trace, so the effect of the deceleration the server now
// configures is measured on ENet's own function. `calmMs` of 40 ms round trips (a ping every 500 ms), then `stormMs` at 140 ms, then calm again.
// Returns the lowest throttle (of 32) and the share of unreliable packets ENet would have dropped over the 10 s from the start of the storm.
struct ThrottleRun { enet_uint32 lowest = 32; double droppedShare = 0; };
static ThrottleRun RunThrottle(enet_uint32 deceleration)
{
    ENetPeer p;
    std::memset(&p, 0, sizeof(p));
    p.packetThrottle = ENET_PEER_DEFAULT_PACKET_THROTTLE;
    p.packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    p.packetThrottleAcceleration = ENET_PEER_PACKET_THROTTLE_ACCELERATION;
    p.packetThrottleDeceleration = deceleration;
    p.packetThrottleInterval = ENET_PEER_PACKET_THROTTLE_INTERVAL;
    p.roundTripTime = 40;
    p.roundTripTimeVariance = 4;
    p.lowestRoundTripTime = 40;
    p.highestRoundTripTimeVariance = 4;
    p.lastRoundTripTime = 40;
    p.lastRoundTripTimeVariance = 4;
    ThrottleRun r;
    double dropped = 0, counted = 0;
    uint32_t nextEpoch = ENET_PEER_PACKET_THROTTLE_INTERVAL;
    for (uint32_t t = 0; t < 40000; t += 500)
    {
        const bool storm = t >= 20000 && t < 26000;
        const enet_uint32 rtt = storm ? 140 : 40 + (t / 500) % 4; // 40..43 ms calm
        enet_peer_throttle(&p, rtt);                                // the real function, as the ack handler calls it
        p.roundTripTimeVariance -= p.roundTripTimeVariance / 4;
        if (rtt >= p.roundTripTime) { const enet_uint32 d = rtt - p.roundTripTime; p.roundTripTimeVariance += d / 4; p.roundTripTime += d / 8; }
        else { const enet_uint32 d = p.roundTripTime - rtt; p.roundTripTimeVariance += d / 4; p.roundTripTime -= d / 8; }
        if (p.roundTripTime < p.lowestRoundTripTime) p.lowestRoundTripTime = p.roundTripTime;
        if (p.roundTripTimeVariance > p.highestRoundTripTimeVariance) p.highestRoundTripTimeVariance = p.roundTripTimeVariance;
        if (t >= nextEpoch)
        {
            p.lastRoundTripTime = p.lowestRoundTripTime;
            p.lastRoundTripTimeVariance = std::max<enet_uint32>(p.highestRoundTripTimeVariance, 1);
            p.lowestRoundTripTime = p.roundTripTime;
            p.highestRoundTripTimeVariance = p.roundTripTimeVariance;
            nextEpoch = t + ENET_PEER_PACKET_THROTTLE_INTERVAL;
        }
        r.lowest = std::min(r.lowest, p.packetThrottle);
        if (t >= 20000 && t < 30000)
        {
            dropped += double(ENET_PEER_PACKET_THROTTLE_SCALE - p.packetThrottle) / double(ENET_PEER_PACKET_THROTTLE_SCALE);
            counted += 1;
        }
    }
    r.droppedShare = counted > 0 ? dropped / counted : 0;
    return r;
}

static std::string Str(double v)
{
    char b[48];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

static void PipelineHealthTests()
{
    // 1. The mechanism: a 6 s storm of slow round trips costs the state channel a large share of its packets for the 10 s that follow under
    // ENet's default deceleration (2) and nothing with 0, which is what the server configures for every peer.
    const ThrottleRun withDecel = RunThrottle(ENET_PEER_PACKET_THROTTLE_DECELERATION), without = RunThrottle(0);
    std::printf("      [ENet throttle model] default deceleration: lowest %u/32, %.0f%% of unreliable packets dropped in the 10 s from the storm's start; deceleration 0: lowest %u/32, %.0f%%\n",
                (unsigned)withDecel.lowest, withDecel.droppedShare * 100, (unsigned)without.lowest, without.droppedShare * 100);
    Check("enet throttle: with ENet's default deceleration a 6 s storm of slow round trips (40 -> 140 ms) drops the throttle to <= 12/32 (up to 2 in 3 unreliable state packets deleted at the worst moment) and ENet deletes > 15% of them over the 10 s from its start; "
          "with deceleration 0 (the server's setting) the throttle never leaves 32/32",
          withDecel.lowest <= 12 && withDecel.droppedShare > 0.15 && without.lowest == 32 && without.droppedShare == 0.0,
          "default " + std::to_string(withDecel.lowest) + "/32, " + Str(withDecel.droppedShare * 100) + "% dropped; with 0: " + std::to_string(without.lowest) + "/32");

    // 2. The live server: every joined peer is configured (the client's peer takes the command too, so an older client is covered) and the
    // gauges and per-tier counters show up in `stats`.
    {
        Rig rig;
        if (!rig.Start()) { Check("pipeline health: server starts", false); return; }
        Probe a(rig), b(rig), c(rig), d(rig);
        const bool joined = a.Join("Alice") && b.Join("Bob") && c.Join("Carol") && d.Join("Dave");
        // Alice stands at the origin; Bob walks 50 m away (near tier), Carol 180 m (mid), Dave 320 m (far); they all move, so every pair is replicated
        // at its distance rate (the idle class would otherwise take them to 1 Hz).
        float t = 0;
        const auto t0 = Clock::now();
        auto next = t0;
        while (MsSince(t0) < 3400)
        {
            if (Clock::now() >= next)
            {
                a.s.SendState(Kin(0, 0, 30));
                b.s.SendState(Kin(50 + t, 0, 30, 0, 2.0f));
                c.s.SendState(Kin(180 + t, 0, 30, 0, 2.0f));
                d.s.SendState(Kin(320 + t, 0, 30, 0, 2.0f));
                t += 0.1f;
                next += std::chrono::milliseconds(50);
            }
            rig.Step();
        }
        const server::Metrics& m = rig.srv.metrics();
        std::string out;
        rig.srv.Execute("stats", out);
        const net::ClientSession::LinkInfo link = a.s.link();
        Check("enet throttle: every joined peer gets deceleration 0 from the server (the client's own peer reports it), the server's gauges read 4 peers at 32/32, and `stats` lists them",
              joined && link.throttleDeceleration == 0 && link.throttle == link.throttleScale && m.enetPeers == 4 && m.enetThrottleMin == 32 && out.find("enet_throttle_min 32") != std::string::npos &&
                  out.find("enet_peers 4") != std::string::npos,
              "client deceleration " + std::to_string(link.throttleDeceleration) + ", throttle " + std::to_string(link.throttle) + "/" + std::to_string(link.throttleScale) + ", server peers " +
                  std::to_string(m.enetPeers) + " min " + std::to_string(m.enetThrottleMin));
        // The per-tier counts. The (receiver, moving subject) pairs: Alice <- Bob 50 m is the only near one; Alice <- Carol 180 m, Bob <- Carol 130 m, Carol <- Bob 130 m,
        // Carol <- Dave 140 m and Dave <- Carol 140 m are mid (5); Alice <- Dave 320 m, Bob <- Dave 270 m and Dave <- Bob 270 m are far (3); everything about the standing
        // Alice is the idle class. At this rig's legacy 20 / 10 / 5 Hz that is 20, 50 and 15 entries a second.
        const double secs = (double)m.ticks / 20.0;
        const double nearPerS = (double)m.pairSentTier[0] / secs, midPerS = (double)m.pairSentTier[1] / secs, farPerS = (double)m.pairSentTier[2] / secs;   // not near / far: windef.h macros
        Check("sync counters: the entries are counted by the rate that applied (near, mid, far, idle): the rig's pairs give about 20 / 50 / 15 entries a second near / mid / far (within 25%), the standing "
              "player's pairs are the idle class, and `stats` lists the four rates",
              joined && secs > 2.5 && nearPerS > 15 && nearPerS < 25 && midPerS > 37.5 && midPerS < 62.5 && farPerS > 11.2 && farPerS < 18.8 && m.pairSentTier[3] > 0 && out.find("pairs_sent_near_per_s") != std::string::npos &&
                  out.find("pairs_sent_far_per_s") != std::string::npos && out.find("pairs_sent_idle_per_s") != std::string::npos,
              "near " + Str(nearPerS) + " mid " + Str(midPerS) + " far " + Str(farPerS) + " /s over " + Str(secs) + " s, idle " + Str((double)m.pairSentTier[3]));
    }

    // 3. The 10 s log line: pure formatting of two metric snapshots.
    {
        server::Metrics a, b;
        b.ticks = 300; b.ticksMissed = 2; b.worldPackets = 600; b.worldEntries = 1500; b.pairSentTier[0] = 900; b.pairSentTier[1] = 300; b.pairSentTier[2] = 200; b.pairSentTier[3] = 100;
        b.samplesReceived = 1400; b.samplesForwarded = 900; b.samplesForwardedTwice = 120; b.pairBudgetDeferred = 5;
        b.enetPeers = 3; b.enetThrottleMin = 24; b.enetLossPermilleMax = 15; b.enetRttMsAvg = 41; b.enetRttMsMax = 77; b.enetRttVarMsMax = 9;
        for (int i = 0; i < 300; i++) b.tickLateUs.Add(500);
        const std::string line = server::FormatSyncLine(b, a, 10.0, 30, 3);
        Check("sync log line: 10 s of counters read as 30.0 ticks/s, near 90.0 / mid 30.0 / far 20.0 / idle 10.0 entries/s, samples in 140.0 forwarded 90.0 twice 12.0 never 50.0 /s, and ENet's "
              "throttle 24/32, loss 1.5%, rtt 41 / 77 ms",
              line.find("sync 10 s: 3 players, 30.0 ticks/s of 30") != std::string::npos && line.find("near 90.0 mid 30.0 far 20.0 idle 10.0") != std::string::npos &&
                  line.find("samples in 140.0 /s, forwarded 90.0 twice 12.0 never 50.0 /s") != std::string::npos && line.find("throttle min 24/32 loss max 1.5% rtt avg 41 max 77 ms var max 9 ms") != std::string::npos,
              line);
    }
}

#include "t4_server_tests.h"

int main()
{
    // Unbuffered, so the lines before a crash are not lost when stdout is a pipe.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = Clock::now();
    std::printf("-- query codec\n");
    QueryCodecTests();
    std::printf("-- config, urls, announce json\n");
    ConfigTests();
    std::printf("-- ban list\n");
    BanListTests();
    std::printf("-- udp query (live)\n");
    QueryLiveTests();
    std::printf("-- master announce\n");
    AnnounceTests();
    std::printf("-- chat flood\n");
    ChatFloodTests();
    std::printf("-- connection cap\n");
    ConnectionCapTests();
    std::printf("-- bans\n");
    BanTests();
    std::printf("-- admin channel\n");
    AdminIdTests();
    AdminTests();
    std::printf("-- authority rules (anticheat.h, vehicles.h)\n");
    AuthorityUnitTests();
    std::printf("-- vehicles v5: ownership, controller, ages, motion-only entries\n");
    VehicleV5UnitTests();
    std::printf("-- T4: vehicle collisions (claim of a driverless vehicle, speed cap clamp)\n");
    t4srv::T4ServerTests();
    std::printf("-- damage scaling (damage.h)\n");
    DamageScaleUnitTests();
    std::printf("-- tick clock and sample ages (N6c)\n");
    StallTests();
    std::printf("-- W8: config warning, start log, melee ceiling\n");
    W8ConfigAndLogTests();
    W8MeleeCeilingTests();
    std::printf("-- sync pipeline review: ENet throttle, per-tier counters, sync log line\n");
    PipelineHealthTests();

    // A skipped section (an early return, a crash-free no-op) must fail, not pass vacuously.
    // 69 before the authority rules; + 1 anti-cheat config check + 10 unit checks of section J.
    const int expected = 111 + t4srv::kT4ServerChecks; // T4: +2 (the claim, the speed cap clamp); // G: +1 admin id truncation; W8: +2 config warning / start log, +1 melee ceiling; the Check() calls on the passing path (v4: +1 vehicle / combat config; W4: +2 tick clock / sample age; W5: +1 damage config, +9 vehicle v5, +2 damage scaling); a skipped section fails the run Sync C: +4 (enet throttle model, live throttle config, per-tier counters, sync log line).
    std::printf("%d/%d passed  (%lld ms)\n", g_run - g_failed, g_run, (long long)MsSince(t0));
    if (g_run != expected)
    {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
