#include "config.h"

#include "anticheat.h"
#include "httpclient.h"
#include "util.h"

// The launcher's JSON reader is header-only with no platform dependencies
// (std headers only), so the server reuses it rather than carrying a second one.
#include "common/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace gage::server {

    namespace {

        constexpr size_t kMaxGamemodeBytes = 32; // the launcher blanks longer labels (apps/master/README.md)
        constexpr size_t kMaxLangBytes = 8;
        constexpr size_t kMaxUrlChars = 256;
        constexpr size_t kMinTokenChars = 16;
        constexpr size_t kMaxTokenChars = 128;
        constexpr size_t kMaxModelListEntries = 128; // vehicleModelDeny / vehicleModelAllow, each

        // The characters the master list refuses in a label (apps/master/src/schema.ts): the
        // line/paragraph separators U+2028/2029, the bidi embeddings and overrides
        // U+202A..202E and the isolates U+2066..2069. `s` is already valid UTF-8, so a
        // byte-pattern search over their encodings (E2 80 A8..AE, E2 81 A6..A9) is exact.
        bool HasMasterUnsafe(const std::string& s)
        {
            for (size_t i = 0; i + 2 < s.size(); i++)
            {
                const unsigned char a = (unsigned char)s[i], b = (unsigned char)s[i + 1], c = (unsigned char)s[i + 2];
                if (a != 0xE2) continue;
                if (b == 0x80 && c >= 0xA8 && c <= 0xAE) return true;
                if (b == 0x81 && c >= 0xA6 && c <= 0xA9) return true;
            }
            return false;
        }

        // /^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,4})?$/ and at most 8 bytes (the master's rule).
        bool ValidLang(const std::string& s)
        {
            if (s.empty() || s.size() > kMaxLangBytes) return false;
            auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
            auto alnum = [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); };
            size_t i = 0;
            while (i < s.size() && alpha(s[i])) i++;
            if (i < 2 || i > 3) return false;
            if (i == s.size()) return true;
            if (s[i] != '-') return false;
            const size_t sub = i + 1;
            if (s.size() - sub < 2 || s.size() - sub > 4) return false;
            for (size_t k = sub; k < s.size(); k++)
                if (!alnum(s[k])) return false;
            return true;
        }

        // Empty, or an http(s) URL without credentials: printable ASCII, a host, at most 256 characters.
        bool ValidInfoUrl(const std::string& u)
        {
            if (u.empty()) return true;
            if (u.size() > kMaxUrlChars) return false;
            for (char ch : u)
            {
                const unsigned char c = (unsigned char)ch;
                if (c <= 0x20 || c >= 0x7F) return false;
            }
            size_t at;
            if (u.compare(0, 7, "http://") == 0) at = 7;
            else if (u.compare(0, 8, "https://") == 0) at = 8;
            else return false;
            const size_t end = u.find_first_of("/?#", at);
            const std::string authority = u.substr(at, end == std::string::npos ? std::string::npos : end - at);
            return !authority.empty() && authority.find('@') == std::string::npos && authority[0] != ':';
        }

        // The replication knobs that are plain whole numbers: server.json name, field, range.
        struct ReplKey
        {
            const char* name;
            uint32_t ReplConfig::*field;
            uint32_t lo, hi;
        };
        const ReplKey kReplKeys[] = {
            {"relevanceEnterRadius", &ReplConfig::enterRadius, 10, 20000},
            {"relevanceLeaveRadius", &ReplConfig::leaveRadius, 10, 20000},
            {"relevanceMaxRadius", &ReplConfig::maxRadius, 10, 20000},
            {"relevanceLookaheadMs", &ReplConfig::lookaheadMs, 0, 10000},
            {"relevanceCellSize", &ReplConfig::cellSize, 64, 2048},
            {"relevanceEveryMs", &ReplConfig::relevanceEveryMs, 0, 5000},
            {"rateNearHz", &ReplConfig::rateNearHz, 1, 120},
            {"rateMidHz", &ReplConfig::rateMidHz, 1, 120},
            {"rateFarHz", &ReplConfig::rateFarHz, 1, 120},
            {"rateIdleHz", &ReplConfig::rateIdleHz, 1, 120},
            {"rateCoarseHz", &ReplConfig::rateCoarseHz, 1, 120},
            {"nearDistance", &ReplConfig::nearDist, 1, 20000},
            {"midDistance", &ReplConfig::midDist, 1, 20000},
            {"idleAfterMs", &ReplConfig::idleAfterMs, 50, 60000},
            {"clientBudgetBytesPerSec", &ReplConfig::budgetBytesPerSec, 1000, 100000000},
            {"worldPacketMaxBytes", &ReplConfig::packetMaxBytes, 200, 1300},
            {"coarseSharePercent", &ReplConfig::coarseSharePercent, 1, 90},
        };

        const ReplKey* FindReplKey(const std::string& name)
        {
            for (const ReplKey& k : kReplKeys)
                if (name == k.name) return &k;
            return nullptr;
        }

        bool ValidToken(const std::string& t)
        {
            if (t.size() < kMinTokenChars || t.size() > kMaxTokenChars) return false;
            for (char ch : t)
            {
                const unsigned char c = (unsigned char)ch;
                if (c <= 0x20 || c >= 0x7F) return false;
            }
            return true;
        }

    }

    bool Validate(const Config& cfg, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (cfg.name.empty() || cfg.name.size() > proto::kMaxServerName)
            return fail("name must be 1.." + std::to_string(proto::kMaxServerName) + " bytes");
        // ValidChat's rules (UTF-8, no control characters); its own 200-byte cap is
        // looser than the 64 enforced above.
        if (!proto::ValidChat(cfg.name))
            return fail("name must be valid UTF-8 without control characters");
        if (cfg.maxPlayers < 1 || cfg.maxPlayers > proto::kMaxPlayers)
            return fail("maxPlayers must be 1.." + std::to_string(proto::kMaxPlayers));
        if (cfg.tickHz < 1 || cfg.tickHz > 120)
            return fail("tickHz must be 1..120");
        if (cfg.helloTimeoutMs == 0)
            return fail("helloTimeoutMs must be > 0");

        // ---- master list ----
        if (cfg.heartbeatSeconds < 5 || cfg.heartbeatSeconds > 3600)
            return fail("heartbeatSeconds must be 5..3600");
        if (!cfg.masterUrl.empty())
        {
            HttpUrl u;
            std::string why;
            if (!ParseHttpUrl(cfg.masterUrl, u, &why)) return fail("masterUrl: " + why);
            // The master rejects an announce it cannot list, forever: catch it here instead.
            if (cfg.gamemode.empty()) return fail("gamemode must not be empty when masterUrl is set");
            if (HasMasterUnsafe(cfg.name))
                return fail("name has characters the master list rejects (line separators, bidi overrides)");
        }
        if (cfg.gamemode.size() > kMaxGamemodeBytes)
            return fail("gamemode must be at most " + std::to_string(kMaxGamemodeBytes) + " bytes");
        if (!cfg.gamemode.empty() && (!proto::ValidChat(cfg.gamemode) || HasMasterUnsafe(cfg.gamemode)))
            return fail("gamemode must be valid UTF-8 without control, line-separator or bidi-override characters");
        if (!ValidLang(cfg.lang))
            return fail("lang must be a language tag such as \"en\" or \"pt-BR\" (at most 8 bytes)");
        if (!ValidInfoUrl(cfg.url))
            return fail("url must be empty or an http(s) URL without credentials, printable ASCII, at most 256 characters");
        if (cfg.announceBackoffBaseMs == 0 || cfg.announceBackoffMaxMs < cfg.announceBackoffBaseMs)
            return fail("announce backoff must satisfy 0 < base <= max");

        // ---- hardening ----
        if (cfg.maxConnectionsPerIp > 1024)
            return fail("maxConnectionsPerIp must be 0..1024 (0 = no cap)");
        if (cfg.chatBurst < 1 || cfg.chatBurst > 100)
            return fail("chatBurst must be 1..100");
        if (cfg.chatWindowMs < 100 || cfg.chatWindowMs > 3600000)
            return fail("chatWindowSeconds must be 1..3600");
        if (cfg.queryRatePerIp < 1 || cfg.queryRatePerIp > 1000000 || cfg.queryRateTotal < 1 || cfg.queryRateTotal > 1000000)
            return fail("query reply rates must be 1..1000000 per second");
        if (cfg.bansFile.size() > 260)
            return fail("bansFile path is too long");

        // ---- world replication ----
        for (const ReplKey& k : kReplKeys)
        {
            const uint32_t v = cfg.repl.*(k.field);
            if (v < k.lo || v > k.hi)
                return fail(std::string(k.name) + " must be " + std::to_string(k.lo) + ".." + std::to_string(k.hi));
        }
        if (cfg.repl.leaveRadius < cfg.repl.enterRadius)
            return fail("relevanceLeaveRadius must be >= relevanceEnterRadius (the gap is the hysteresis)");
        if (cfg.repl.maxRadius < cfg.repl.enterRadius)
            return fail("relevanceMaxRadius must be >= relevanceEnterRadius");
        if (cfg.repl.rateMidHz > cfg.repl.rateNearHz || cfg.repl.rateFarHz > cfg.repl.rateMidHz)
            return fail("send rates must satisfy rateNearHz >= rateMidHz >= rateFarHz");
        if (cfg.repl.midDist < cfg.repl.nearDist)
            return fail("midDistance must be >= nearDistance");

        // ---- vehicles and combat (v4) ----
        if (cfg.maxVehicles < 1 || cfg.maxVehicles > proto::kMaxVehicles)
            return fail("maxVehicles must be 1.." + std::to_string(proto::kMaxVehicles));
        if (cfg.vehiclesPerPlayer < 1 || cfg.vehiclesPerPlayer > 64) return fail("vehiclesPerPlayer must be 1..64");
        if (cfg.vehicleEmptyDespawnMs > 86400000u) return fail("vehicleEmptyDespawnSeconds must be 0..86400");
        if (cfg.vehicleEnterDistance < 1 || cfg.vehicleEnterDistance > 100) return fail("vehicleEnterDistance must be 1..100");
        if (cfg.vehicleMaxSpeed < 10 || cfg.vehicleMaxSpeed > 600) return fail("vehicleMaxSpeed must be 10..600");
        if (cfg.fuelDrainPerMinute > 1000) return fail("fuelDrainPerMinute must be 0..1000");
        if (cfg.contactLagCapMs > 500) return fail("contactLagCapMs must be 0..500");
        if (cfg.playerHealth < 1 || cfg.playerHealth > 255) return fail("playerHealth must be 1..255");
        for (const std::pair<const char*, uint32_t> p : {std::make_pair("firearmDamagePercent", cfg.firearmDamagePercent),
                                                         std::make_pair("meleeDamagePercent", cfg.meleeDamagePercent),
                                                         std::make_pair("explosiveDamagePercent", cfg.explosiveDamagePercent),
                                                         std::make_pair("runOverDamagePercent", cfg.runOverDamagePercent)})
            if (p.second < 1 || p.second > 1000) return fail(std::string(p.first) + " must be 1..1000");
        if (cfg.headshotPercent < 100 || cfg.headshotPercent > 1000) return fail("headshotPercent must be 100..1000");
        if (cfg.respawnArmour > 255) return fail("respawnArmour must be 0..255");
        if (cfg.respawnDelayMs > 300000) return fail("respawnDelaySeconds must be 0..300");
        if (cfg.maxHitDistance < 10 || cfg.maxHitDistance > 5000) return fail("maxHitDistance must be 10..5000");
        if (cfg.hitsPerSecond < 1 || cfg.hitsPerSecond > 1000) return fail("hitsPerSecond must be 1..1000");
        if (cfg.meleeAdjudicate > 1) return fail("meleeAdjudicate must be 0 or 1");
        if (cfg.meleeReachPercent < 25 || cfg.meleeReachPercent > 400) return fail("meleeReachPercent must be 25..400");
        if (cfg.spawnPoints.empty() || cfg.spawnPoints.size() > 16) return fail("spawnPoints must hold 1..16 points");
        for (const SpawnPoint& sp : cfg.spawnPoints)
        {
            proto::Kinematics k;
            k.x = sp.x;
            k.y = sp.y;
            k.z = sp.z;
            k.heading = sp.heading;
            if (!proto::ValidKinematics(k))
                return fail("every spawn point must be [x, y, z, heading] inside the world box (x, y within +-16000, z -2000..3000, heading +-360)");
        }

        // ---- authority rules (anticheat.h) ----
        if (cfg.hitShotWindowMs != 0 && (cfg.hitShotWindowMs < 100 || cfg.hitShotWindowMs > 5000))
            return fail("hitShotWindowMs must be 0 (off) or 100..5000");
        if (cfg.hitDamageScalePercent < 10 || cfg.hitDamageScalePercent > 1000) return fail("hitDamageScalePercent must be 10..1000");
        if (cfg.onFootMaxSpeed < 5 || cfg.onFootMaxSpeed > 600) return fail("onFootMaxSpeed must be 5..600");
        if (cfg.untrustedWindowMs != 0 && (cfg.untrustedWindowMs < 500 || cfg.untrustedWindowMs > 60000))
            return fail("untrustedWindowMs must be 0 (movement check off) or 500..60000");
        if (cfg.untrustedJoinGraceMs > 120000) return fail("untrustedJoinGraceMs must be 0..120000");
        if (cfg.connectsPerIpPerMinute > 10000) return fail("connectsPerIpPerMinute must be 0..10000 (0 = no limit)");
        if (cfg.vehiclesPerIp < 1 || cfg.vehiclesPerIp > 256) return fail("vehiclesPerIp must be 1..256");
        if (cfg.vehicleOrphanMs < 1000 || cfg.vehicleOrphanMs > 3600000u) return fail("vehicleOrphanSeconds must be 1..3600");
        if (cfg.registerCooldownMs > 60000) return fail("registerCooldownMs must be 0..60000");
        if (cfg.registerSeparationMeters > 20) return fail("registerSeparationMeters must be 0..20 (0 = no such rule)");
        for (const std::vector<uint32_t>* list : {&cfg.vehicleModelDeny, &cfg.vehicleModelAllow})
        {
            if (list->size() > kMaxModelListEntries)
                return fail("vehicleModelDeny / vehicleModelAllow hold at most " + std::to_string(kMaxModelListEntries) + " models each");
            for (uint32_t m : *list)
                if (m == 0) return fail("vehicleModelDeny / vehicleModelAllow must not contain model 0");
        }

        // ---- admin channel ----
        if (cfg.adminEnabled() && !ValidToken(cfg.adminToken))
            return fail("adminToken must be " + std::to_string(kMinTokenChars) + ".." + std::to_string(kMaxTokenChars) +
                        " printable ASCII characters without spaces whenever adminPort is set (or set GAGE_ADMIN_TOKEN)");
        return true;
    }

    namespace {

        // A JSON number that is a whole number in [lo, hi].
        bool WholeNumber(const JVal& v, double lo, double hi, uint32_t& out)
        {
            if (v.t != JVal::Num || v.n != std::floor(v.n) || v.n < lo || v.n > hi) return false;
            out = (uint32_t)v.n;
            return true;
        }

        // "vehicleModelDeny" / "vehicleModelAllow": an array of at most 128 models, each a whole number
        // 1..4294967295 (the GTA hash), a "0x" + 1..8 hex digits string (the same), or a model name
        // (hashed like GET_HASH_KEY). Duplicates collapse.
        bool ParseModelList(const JVal& v, std::vector<uint32_t>& out)
        {
            if (v.t != JVal::Arr || v.a.size() > kMaxModelListEntries) return false;
            std::vector<uint32_t> list;
            for (const JVal& e : v.a)
            {
                uint32_t h = 0;
                if (e.t == JVal::Num)
                {
                    if (!WholeNumber(e, 1, 4294967295.0, h)) return false;
                }
                else if (e.t == JVal::Str)
                {
                    if (e.s.empty() || e.s.size() > 64) return false;
                    bool hex = e.s.size() > 2 && e.s.size() <= 10 && e.s[0] == '0' && (e.s[1] == 'x' || e.s[1] == 'X');
                    for (size_t i = 2; hex && i < e.s.size(); i++) hex = std::isxdigit((unsigned char)e.s[i]) != 0;
                    h = hex ? (uint32_t)std::strtoul(e.s.c_str() + 2, nullptr, 16) : ac::Joaat(e.s);
                }
                else
                    return false;
                if (h == 0) return false;
                if (std::find(list.begin(), list.end(), h) == list.end()) list.push_back(h);
            }
            out = list;
            return true;
        }

        // "spawnPoints": [[x, y, z, heading], ...] with 1..16 points of four numbers each.
        bool ParseSpawnPoints(const JVal& v, std::vector<SpawnPoint>& out)
        {
            if (v.t != JVal::Arr || v.a.empty() || v.a.size() > 16) return false;
            std::vector<SpawnPoint> pts;
            for (const JVal& p : v.a)
            {
                if (p.t != JVal::Arr || p.a.size() != 4) return false;
                for (const JVal& n : p.a)
                    if (n.t != JVal::Num || !std::isfinite(n.n)) return false;
                pts.push_back(SpawnPoint{(float)p.a[0].n, (float)p.a[1].n, (float)p.a[2].n, (float)p.a[3].n});
            }
            out = pts;
            return true;
        }

    }

    bool LoadConfigFile(const std::string& path, Config& cfg, std::string* err, bool* missing)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = path + ": " + why;
            return false;
        };
        if (missing) *missing = false;

        std::string text, readErr;
        bool absent = false;
        if (!ReadWholeFile(path, text, &readErr, &absent))
        {
            if (missing) *missing = absent;
            return fail(absent ? "cannot open" : "cannot read (" + readErr + ")");
        }

        JParser p;
        p.maxNodes = 1024; // the two model lists alone may hold 256
        if (!p.parse(text.data(), text.size()))
            return fail("not valid JSON");
        // JParser stops after the first value: reject trailing junk too.
        const char* rest = p.p;
        while (rest < p.end && (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')) rest++;
        if (rest != p.end)
            return fail("trailing data after the JSON object");
        if (p.v.t != JVal::Obj)
            return fail("top level must be an object");

        Config out = cfg;
        bool fileSetsLatestClient = false;
        for (const auto& kv : p.v.o)
        {
            const std::string& key = kv.first;
            const JVal& v = kv.second;
            uint32_t n = 0;
            auto str = [&](std::string& dst) {
                if (v.t != JVal::Str) return fail("\"" + key + "\" must be a string");
                dst = v.s;
                return true;
            };
            if (key == "name")
            {
                if (!str(out.name)) return false;
            }
            else if (key == "port")
            {
                if (!WholeNumber(v, 1, 65535, n)) return fail("\"port\" must be a whole number 1..65535");
                out.port = (uint16_t)n;
            }
            else if (key == "maxPlayers")
            {
                if (!WholeNumber(v, 1, proto::kMaxPlayers, n))
                    return fail("\"maxPlayers\" must be a whole number 1.." + std::to_string(proto::kMaxPlayers));
                out.maxPlayers = (uint16_t)n;
            }
            else if (key == "tickHz")
            {
                if (!WholeNumber(v, 1, 120, n)) return fail("\"tickHz\" must be a whole number 1..120");
                out.tickHz = (uint16_t)n;
            }
            else if (key == "masterUrl")
            {
                if (!str(out.masterUrl)) return false;
            }
            else if (key == "heartbeatSeconds")
            {
                if (!WholeNumber(v, 5, 3600, n)) return fail("\"heartbeatSeconds\" must be a whole number 5..3600");
                out.heartbeatSeconds = n;
            }
            else if (key == "gamemode")
            {
                if (!str(out.gamemode)) return false;
            }
            else if (key == "lang")
            {
                if (!str(out.lang)) return false;
            }
            else if (key == "url")
            {
                if (!str(out.url)) return false;
            }
            else if (key == "maxConnectionsPerIp")
            {
                if (!WholeNumber(v, 0, 1024, n))
                    return fail("\"maxConnectionsPerIp\" must be a whole number 0..1024 (0 = no cap)");
                out.maxConnectionsPerIp = n;
            }
            else if (key == "chatBurst")
            {
                if (!WholeNumber(v, 1, 100, n)) return fail("\"chatBurst\" must be a whole number 1..100");
                out.chatBurst = n;
            }
            else if (key == "chatWindowSeconds")
            {
                if (!WholeNumber(v, 1, 3600, n)) return fail("\"chatWindowSeconds\" must be a whole number 1..3600");
                out.chatWindowMs = n * 1000;
            }
            else if (key == "bansFile")
            {
                if (!str(out.bansFile)) return false;
            }
            else if (key == "adminPort")
            {
                if (!WholeNumber(v, 0, 65535, n)) return fail("\"adminPort\" must be a whole number 0..65535 (0 = off)");
                out.adminPort = (uint16_t)n;
            }
            else if (key == "adminToken")
            {
                if (!str(out.adminToken)) return false;
            }
            else if (key == "latestClientVersion")
            {
                // "x.y.z" (each 0..255) or "" (none). 0.0.0 is the same as none.
                if (v.t != JVal::Str) return fail("\"latestClientVersion\" must be a string \"x.y.z\" (each part 0..255) or \"\"");
                proto::query::ClientVersion cv;
                if (!v.s.empty() && !proto::query::ParseClientVersion(v.s.data(), v.s.size(), cv))
                    return fail("\"latestClientVersion\" must look like \"0.4.4\" (three numbers 0..255) or be \"\"");
                out.latestClient = cv;
                fileSetsLatestClient = true;
            }
            else if (key == "maxVehicles")
            {
                if (!WholeNumber(v, 1, proto::kMaxVehicles, n))
                    return fail("\"maxVehicles\" must be a whole number 1.." + std::to_string(proto::kMaxVehicles));
                out.maxVehicles = (uint16_t)n;
            }
            else if (key == "vehiclesPerPlayer")
            {
                if (!WholeNumber(v, 1, 64, n)) return fail("\"vehiclesPerPlayer\" must be a whole number 1..64");
                out.vehiclesPerPlayer = (uint16_t)n;
            }
            else if (key == "vehicleEmptyDespawnSeconds")
            {
                if (!WholeNumber(v, 0, 86400, n)) return fail("\"vehicleEmptyDespawnSeconds\" must be a whole number 0..86400 (0 = never)");
                out.vehicleEmptyDespawnMs = n * 1000;
            }
            else if (key == "vehicleEnterDistance")
            {
                if (!WholeNumber(v, 1, 100, n)) return fail("\"vehicleEnterDistance\" must be a whole number 1..100");
                out.vehicleEnterDistance = n;
            }
            else if (key == "vehicleMaxSpeed")
            {
                if (!WholeNumber(v, 10, 600, n)) return fail("\"vehicleMaxSpeed\" must be a whole number 10..600");
                out.vehicleMaxSpeed = n;
            }
            else if (key == "contactLagCapMs")
            {
                if (!WholeNumber(v, 0, 500, n)) return fail("\"contactLagCapMs\" must be a whole number 0..500");
                out.contactLagCapMs = n;
            }
            else if (key == "fuelDrainPerMinute")
            {
                if (!WholeNumber(v, 0, 1000, n)) return fail("\"fuelDrainPerMinute\" must be a whole number 0..1000");
                out.fuelDrainPerMinute = n;
            }
            else if (key == "playerHealth")
            {
                if (!WholeNumber(v, 1, 255, n)) return fail("\"playerHealth\" must be a whole number 1..255");
                out.playerHealth = (uint16_t)n;
            }
            else if (key == "firearmDamagePercent" || key == "meleeDamagePercent" || key == "explosiveDamagePercent" ||
                     key == "runOverDamagePercent")
            {
                if (!WholeNumber(v, 1, 1000, n)) return fail("\"" + key + "\" must be a whole number 1..1000");
                (key == "firearmDamagePercent"    ? out.firearmDamagePercent
                 : key == "meleeDamagePercent"    ? out.meleeDamagePercent
                 : key == "explosiveDamagePercent" ? out.explosiveDamagePercent
                                                   : out.runOverDamagePercent) = n;
            }
            else if (key == "headshotPercent")
            {
                if (!WholeNumber(v, 100, 1000, n)) return fail("\"headshotPercent\" must be a whole number 100..1000");
                out.headshotPercent = n;
            }
            else if (key == "respawnArmour")
            {
                if (!WholeNumber(v, 0, 255, n)) return fail("\"respawnArmour\" must be a whole number 0..255");
                out.respawnArmour = (uint16_t)n;
            }
            else if (key == "respawnDelaySeconds")
            {
                if (!WholeNumber(v, 0, 300, n)) return fail("\"respawnDelaySeconds\" must be a whole number 0..300");
                out.respawnDelayMs = n * 1000;
            }
            else if (key == "maxHitDistance")
            {
                if (!WholeNumber(v, 10, 5000, n)) return fail("\"maxHitDistance\" must be a whole number 10..5000");
                out.maxHitDistance = n;
            }
            else if (key == "hitsPerSecond")
            {
                if (!WholeNumber(v, 1, 1000, n)) return fail("\"hitsPerSecond\" must be a whole number 1..1000");
                out.hitsPerSecond = n;
            }
            else if (key == "meleeAdjudicate")
            {
                if (!WholeNumber(v, 0, 1, n)) return fail("\"meleeAdjudicate\" must be 0 or 1");
                out.meleeAdjudicate = n;
            }
            else if (key == "meleeReachPercent")
            {
                if (!WholeNumber(v, 25, 400, n)) return fail("\"meleeReachPercent\" must be a whole number 25..400");
                out.meleeReachPercent = n;
            }
            else if (key == "spawnPoints")
            {
                if (!ParseSpawnPoints(v, out.spawnPoints))
                    return fail("\"spawnPoints\" must be an array of 1..16 [x, y, z, heading] number arrays");
            }
            else if (key == "hitShotWindowMs")
            {
                if (!WholeNumber(v, 0, 5000, n) || (n != 0 && n < 100))
                    return fail("\"hitShotWindowMs\" must be a whole number 100..5000, or 0 (off)");
                out.hitShotWindowMs = n;
            }
            else if (key == "hitDamageScalePercent")
            {
                if (!WholeNumber(v, 10, 1000, n)) return fail("\"hitDamageScalePercent\" must be a whole number 10..1000");
                out.hitDamageScalePercent = n;
            }
            else if (key == "onFootMaxSpeed")
            {
                if (!WholeNumber(v, 5, 600, n)) return fail("\"onFootMaxSpeed\" must be a whole number 5..600");
                out.onFootMaxSpeed = n;
            }
            else if (key == "untrustedWindowMs")
            {
                if (!WholeNumber(v, 0, 60000, n) || (n != 0 && n < 500))
                    return fail("\"untrustedWindowMs\" must be a whole number 500..60000, or 0 (movement check off)");
                out.untrustedWindowMs = n;
            }
            else if (key == "untrustedJoinGraceMs")
            {
                if (!WholeNumber(v, 0, 120000, n)) return fail("\"untrustedJoinGraceMs\" must be a whole number 0..120000");
                out.untrustedJoinGraceMs = n;
            }
            else if (key == "connectsPerIpPerMinute")
            {
                if (!WholeNumber(v, 0, 10000, n))
                    return fail("\"connectsPerIpPerMinute\" must be a whole number 0..10000 (0 = no limit)");
                out.connectsPerIpPerMinute = n;
            }
            else if (key == "vehiclesPerIp")
            {
                if (!WholeNumber(v, 1, 256, n)) return fail("\"vehiclesPerIp\" must be a whole number 1..256");
                out.vehiclesPerIp = (uint16_t)n;
            }
            else if (key == "vehicleOrphanSeconds")
            {
                if (!WholeNumber(v, 1, 3600, n)) return fail("\"vehicleOrphanSeconds\" must be a whole number 1..3600");
                out.vehicleOrphanMs = n * 1000;
            }
            else if (key == "registerCooldownMs")
            {
                if (!WholeNumber(v, 0, 60000, n)) return fail("\"registerCooldownMs\" must be a whole number 0..60000");
                out.registerCooldownMs = n;
            }
            else if (key == "registerSeparationMeters")
            {
                if (!WholeNumber(v, 0, 20, n)) return fail("\"registerSeparationMeters\" must be a whole number 0..20 (0 = no such rule)");
                out.registerSeparationMeters = n;
            }
            else if (key == "vehicleModelDeny" || key == "vehicleModelAllow")
            {
                if (!ParseModelList(v, key == "vehicleModelDeny" ? out.vehicleModelDeny : out.vehicleModelAllow))
                    return fail("\"" + key + "\" must be an array of at most " + std::to_string(kMaxModelListEntries) +
                                " models: GTA hashes (1..4294967295), \"0x...\" hash strings or model names");
            }
            else if (const ReplKey* rk = FindReplKey(key))
            {
                if (!WholeNumber(v, rk->lo, rk->hi, n))
                    return fail("\"" + key + "\" must be a whole number " + std::to_string(rk->lo) + ".." +
                                std::to_string(rk->hi));
                out.repl.*(rk->field) = n;
            }
            else
            {
                std::string known = "name, port, maxPlayers, tickHz, masterUrl, heartbeatSeconds, gamemode, lang, url, "
                                    "maxConnectionsPerIp, chatBurst, chatWindowSeconds, bansFile, adminPort, adminToken, "
                                    "latestClientVersion, maxVehicles, vehiclesPerPlayer, vehicleEmptyDespawnSeconds, vehicleEnterDistance, "
                                    "vehicleMaxSpeed, contactLagCapMs, fuelDrainPerMinute, playerHealth, firearmDamagePercent, "
                                    "meleeDamagePercent, explosiveDamagePercent, runOverDamagePercent, headshotPercent, "
                                    "respawnArmour, respawnDelaySeconds, maxHitDistance, "
                                    "hitsPerSecond, meleeAdjudicate, meleeReachPercent, spawnPoints, hitShotWindowMs, hitDamageScalePercent, onFootMaxSpeed, "
                                    "untrustedWindowMs, untrustedJoinGraceMs, connectsPerIpPerMinute, vehiclesPerIp, "
                                    "vehicleOrphanSeconds, registerCooldownMs, registerSeparationMeters, vehicleModelDeny, "
                                    "vehicleModelAllow";
                for (const ReplKey& k : kReplKeys) known += std::string(", ") + k.name;
                return fail("unknown key \"" + key + "\" (known: " + known + ")");
            }
        }
        // The token may live in the environment instead of the file (keeps the secret out
        // of anything that gets copied around): only used when the file sets none.
        if (out.adminEnabled() && out.adminToken.empty())
            if (const char* env = std::getenv("GAGE_ADMIN_TOKEN")) out.adminToken = env;
        // Same for the newest client release (tools/deploy/release.sh writes it to /etc/gage/server.env, which the
        // service unit loads; an older server binary ignores an environment variable it does not know, but would
        // refuse a new server.json key, so a rollback stays possible).
        if (!fileSetsLatestClient)
            if (const char* env = std::getenv("GAGE_LATEST_CLIENT_VERSION"))
                if (*env)
                {
                    proto::query::ClientVersion cv;
                    if (!proto::query::ParseClientVersion(env, std::strlen(env), cv))
                        return fail("environment variable GAGE_LATEST_CLIENT_VERSION must look like \"0.4.4\" (three numbers 0..255)");
                    out.latestClient = cv;
                }
        std::string why;
        if (!Validate(out, &why)) return fail(why);
        cfg = out;
        return true;
    }

    std::vector<std::string> ConfigWarnings(const Config& cfg)
    {
        std::vector<std::string> w;
        if (cfg.repl.rateNearHz < cfg.tickHz)
            w.push_back("rateNearHz (" + std::to_string(cfg.repl.rateNearHz) + ") is below tickHz (" + std::to_string(cfg.tickHz) +
                        "): players within nearDistance reach each other at " + std::to_string(cfg.repl.rateNearHz) + "/s, not " +
                        std::to_string(cfg.tickHz) + "/s (the world still ticks at " + std::to_string(cfg.tickHz) +
                        " Hz); raise rateNearHz to tickHz or remove it from server.json");
        return w;
    }

    std::string DescribeRepl(const Config& cfg)
    {
        const ReplConfig& r = cfg.repl;
        return "replication (effective): tickHz " + std::to_string(cfg.tickHz) + ", relevance enter/leave/max " + std::to_string(r.enterRadius) + "/" +
               std::to_string(r.leaveRadius) + "/" + std::to_string(r.maxRadius) + " m, lookahead " + std::to_string(r.lookaheadMs) + " ms, cell " +
               std::to_string(r.cellSize) + " m, relevance every " + std::to_string(r.relevanceEveryMs) + " ms, near/mid distance " +
               std::to_string(r.nearDist) + "/" + std::to_string(r.midDist) + " m, rates near/mid/far/idle/coarse " + std::to_string(r.rateNearHz) + "/" +
               std::to_string(r.rateMidHz) + "/" + std::to_string(r.rateFarHz) + "/" + std::to_string(r.rateIdleHz) + "/" + std::to_string(r.rateCoarseHz) +
               " Hz, idle after " + std::to_string(r.idleAfterMs) + " ms, budget " + std::to_string(r.budgetBytesPerSec) + " B/s per client, world packet max " +
               std::to_string(r.packetMaxBytes) + " B, coarse share " + std::to_string(r.coarseSharePercent) + "%";
    }

    std::string DescribeLatestClient(const Config& cfg)
    {
        const proto::query::ClientVersion& v = cfg.latestClient;
        if (!v.Known()) return "update ping: no latest client configured (the UDP query reports 0.0.0 = unknown)";
        return "update ping: the UDP query tells launchers the latest client is " + std::to_string(v.major) + "." +
               std::to_string(v.minor) + "." + std::to_string(v.patch);
    }

    std::string DescribeCombat(const Config& cfg)
    {
        return "combat (effective): playerHealth " + std::to_string(cfg.playerHealth) + ", damage firearm " + std::to_string(cfg.firearmDamagePercent) +
               "% melee " + std::to_string(cfg.meleeDamagePercent) + "% explosive " + std::to_string(cfg.explosiveDamagePercent) + "% run-over " +
               std::to_string(cfg.runOverDamagePercent) + "% headshot x" + std::to_string(cfg.headshotPercent) + "%, hit caps x" +
               std::to_string(cfg.hitDamageScalePercent) + "%, melee per-hit ceiling fists " +
               std::to_string(ac::MeleeNominalMax(ac::Joaat("WEAPON_UNARMED"), cfg.hitDamageScalePercent)) + " / light " +
               std::to_string(ac::MeleeNominalMax(ac::Joaat("WEAPON_KNIFE"), cfg.hitDamageScalePercent)) + " / heavy " +
               std::to_string(ac::MeleeNominalMax(ac::Joaat("WEAPON_BAT"), cfg.hitDamageScalePercent)) + " (before the melee percent), server melee judgement " +
               (cfg.meleeAdjudicate ? "on (reach x" + std::to_string(cfg.meleeReachPercent) + "%)" : std::string("off"));
    }

}
