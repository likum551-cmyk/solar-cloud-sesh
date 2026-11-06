#pragma once
// v8 accounts (protocol v8: JoinAuth / AccountInfo / AdminCmd / AdminResult; apps/server/platform.h): the real server and the
// real ClientSession on loopback against a fake platform internal listener (FakeMaster with a responder, server_test.cpp).
// Included by tests/server_test.cpp after FakeMaster; every check runs on the passing path (kAccountChecks of them).
//
// The fake platform answers POST /internal/game/verify by the token in the body, the way apps/platform does
// (apps/platform/docs/game-server-contract.md section 2: every verdict is HTTP 200; ok:false carries a reason), and
// POST /internal/game/session-end with 200.

#include "platform.h"

namespace acct {

    constexpr int kAccountChecks = 23;

    constexpr const char* kSecret = "s3cret-platform-0123456789";

    static FakeMaster::Reply PlatformAnswer(const FakeMaster::Req& rq)
    {
        FakeMaster::Reply r;
        if (rq.head.rfind("POST /internal/game/session-end ", 0) == 0)
        {
            r.body = "{\"ok\":true}";
            return r;
        }
        if (rq.head.rfind("POST /internal/game/verify ", 0) != 0)
        {
            r.status = 404;
            r.body = "{\"error\":{\"code\":\"not_found\"}}";
            return r;
        }
        auto has = [&](const char* tok) { return rq.body == std::string("{\"token\":\"") + tok + "\"}"; };
        auto ok = [&](const char* acc, const char* user, const char* roles) {
            r.body = std::string("{\"ok\":true,\"accountId\":\"") + acc + "\",\"username\":\"" + user + "\",\"roles\":" + roles +
                     ",\"emailVerified\":true,\"sessionId\":\"ses-" + acc + "\",\"ban\":null}";
        };
        if (has("tok-player")) ok("acc-player", "Pia_Player", "[\"player\"]");
        else if (has("tok-player2")) ok("acc-player", "Pia_Player", "[\"player\"]");   // the same account, a second join
        else if (has("tok-admin")) ok("acc-admin", "Ada_Admin", "[\"player\",\"admin\"]");
        else if (has("tok-mod")) ok("acc-mod", "Mo_Mod", "[\"player\",\"mod\"]");
        else if (has("tok-badname")) ok("acc-bad", "not a name!", "[\"player\"]");
        else if (has("tok-expired")) r.body = "{\"ok\":false,\"reason\":\"expired\"}";
        else if (has("tok-used")) r.body = "{\"ok\":false,\"reason\":\"used\"}";
        else if (has("tok-banned"))
            r.body = "{\"ok\":false,\"reason\":\"banned\",\"accountId\":\"acc-ban\",\"username\":\"Bo_Banned\",\"ban\":{\"reason\":\"cheating\",\"expiresAt\":null}}";
        else if (has("tok-500"))
        {
            r.status = 500;
            r.body = "{\"error\":{\"code\":\"internal_error\"}}";
        }
        else if (has("tok-401"))
        {
            r.status = 401;
            r.body = "{\"error\":{\"code\":\"invalid_server_credentials\"}}";
        }
        else r.body = "{\"ok\":false,\"reason\":\"invalid_token\"}";
        return r;
    }

    static server::Config PlatformConfig(const FakeMaster& fake, bool requireAccount)
    {
        server::Config c = Rig::BaseConfig();
        c.platformUrl = fake.url();
        c.platformServerId = "sol";
        c.platformServerSecret = kSecret;
        c.requireAccount = requireAccount;
        c.platformTimeoutMs = 1500;
        return c;
    }

    static auto WithToken(const char* tok)
    {
        return [tok](net::SessionOptions& o) { o.joinToken = tok; };
    }

    // Connect with `tok` and wait for the Welcome or the end; then drain.
    static bool JoinWith(Rig& rig, Probe& p, const char* hello, const char* tok)
    {
        const bool ok = p.Join(hello, WithToken(tok));
        if (!ok) p.WaitClosed(3000);
        rig.Pump(100);
        p.Drain();
        return ok;
    }

    static const net::Event* Account(const Probe& p) { return p.First(ET::AccountInfo); }

    // What a probe heard plus how it ended (refuse reason, disconnect reason) and the server's newest log lines about
    // accounts and connections: the detail of a refusal check, so a failure says which side ended the connection.
    static std::string Why(const Rig& rig, const Probe& p, const std::string& name)
    {
        std::string out = p.Describe();
        if (const net::Event* r = p.First(ET::Refused)) out += " refuse=" + std::to_string((int)r->refuse.reason);
        if (const net::Event* d = p.First(ET::Disconnected)) out += " disconnect=" + std::to_string((int)d->disconnect);
        const net::ClientSession::Counters k = p.s.counters();
        out += " wireIn=" + std::to_string(k.wireDatagramsIn) + " wireOut=" + std::to_string(k.wireDatagramsOut) +
               " sockErr=" + std::to_string(k.socketErrors) + "/" + std::to_string(k.lastSocketError);
        std::vector<std::string> tail;
        for (const std::string& l : rig.logs)
            if (l.find(name) != std::string::npos || l.find("] connect ") != std::string::npos || l.find("] disconnect ") != std::string::npos ||
                l.find("] refuse ") != std::string::npos)
                tail.push_back(l);
        const size_t from = tail.size() > 16 ? tail.size() - 16 : 0;
        for (size_t i = from; i < tail.size(); i++) out += "\n      log: " + tail[i];
        return out;
    }

    static uint16_t MyId(const Probe& p)
    {
        const net::Event* w = p.First(ET::Welcome);
        return w ? w->welcome.yourId : 0;
    }

    static const server::PlayerInfo* Info(const std::vector<server::PlayerInfo>& all, uint16_t id)
    {
        for (const server::PlayerInfo& i : all)
            if (i.id == id) return &i;
        return nullptr;
    }

    // One AdminCmd and its AdminResult (code 255 when none came).
    static proto::AdminResult Admin(Rig& rig, Probe& p, proto::AdminOp op, const std::string& arg = "")
    {
        static uint16_t seq = 100;
        proto::AdminCmd c;
        c.seq = ++seq;
        c.op = op;
        c.arg = arg;
        proto::AdminResult none;
        none.code = (proto::AdminCode)255;
        if (!p.s.SendAdminCmd(c)) return none;
        const int before = p.Count(ET::AdminResult);
        rig.Until([&] { p.Drain(); return p.Count(ET::AdminResult) > before; }, 2000);
        for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
            if (it->type == ET::AdminResult && it->adminResult.seq == c.seq) return it->adminResult;
        return none;
    }

    // One vehicle command and its VehicleResult (code 255 when none came).
    static proto::VehicleResult Veh(Rig& rig, Probe& p, proto::VehicleOp op, uint32_t arg)
    {
        proto::VehicleCmd c;
        c.op = op;
        c.arg = arg;
        proto::VehicleResult none;
        none.code = (proto::VehicleResultCode)255;
        const int before = p.Count(ET::VehicleResult);
        if (!p.s.SendVehicleCmd(c)) return none;
        rig.Until([&] { p.Drain(); return p.Count(ET::VehicleResult) > before; }, 2000);
        for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
            if (it->type == ET::VehicleResult) return it->vehicleResult;
        return none;
    }

    // The server holds a position for `p` (a Spawn needs one).
    static bool Placed(Rig& rig, Probe& p, float x, float y)
    {
        const uint16_t id = MyId(p);
        return SendUntil(rig, p, Kin(x, y, 30.0f), [&] {
            const std::vector<server::PlayerInfo> all = rig.srv.Players();
            const server::PlayerInfo* i = Info(all, id);
            return i && i->hasState && std::fabs(i->k.x - x) < 0.05f;
        }, 2000);
    }

    static int Count(const std::vector<FakeMaster::Req>& reqs, const std::string& pathPrefix, const std::string& bodyPart = "")
    {
        int n = 0;
        for (const FakeMaster::Req& r : reqs)
            if (r.head.rfind("POST " + pathPrefix, 0) == 0 && (bodyPart.empty() || r.body.find(bodyPart) != std::string::npos)) n++;
        return n;
    }

    static void UnitTests()
    {
        using server::VerifyResult;
        using server::ParseVerifyAnswer;
        const VerifyResult okR = ParseVerifyAnswer(200, "{\"ok\":true,\"accountId\":\"a1\",\"username\":\"Pia\",\"roles\":[\"player\",\"admin\"],\"sessionId\":\"s1\",\"ban\":null}");
        const VerifyResult exp = ParseVerifyAnswer(200, "{\"ok\":false,\"reason\":\"expired\"}");
        const VerifyResult ban = ParseVerifyAnswer(200, "{\"ok\":false,\"reason\":\"banned\",\"ban\":{\"reason\":\"x\",\"expiresAt\":\"2026-12-01T00:00:00Z\"}}");
        const VerifyResult e500 = ParseVerifyAnswer(500, "{\"error\":{\"code\":\"internal_error\"}}");
        const VerifyResult noRoles = ParseVerifyAnswer(200, "{\"ok\":true,\"accountId\":\"a1\",\"username\":\"Pia\"}");
        const VerifyResult junk = ParseVerifyAnswer(200, "<html>");
        Check("v8 platform answer: ok:true -> Ok with account, username, session and roles; expired -> Rejected; banned -> Banned with reason "
              "and expiry; HTTP 500 -> Unavailable naming the code; ok:true without roles and a non-JSON body -> Unavailable (fail closed)",
              okR.outcome == VerifyResult::Outcome::Ok && okR.accountId == "a1" && okR.username == "Pia" && okR.sessionId == "s1" &&
                  okR.roles.size() == 2 && okR.roles[1] == "admin" && exp.outcome == VerifyResult::Outcome::Rejected && exp.reason == "expired" &&
                  ban.outcome == VerifyResult::Outcome::Banned && ban.banReason == "x" && ban.banExpiresAt == "2026-12-01T00:00:00Z" &&
                  e500.outcome == VerifyResult::Outcome::Unavailable && e500.reason == "HTTP 500 internal_error" &&
                  noRoles.outcome == VerifyResult::Outcome::Unavailable && junk.outcome == VerifyResult::Outcome::Unavailable,
              e500.reason + " | " + noRoles.reason);

        // Config: the keys load, and Validate refuses the unsafe combinations.
        {
            const char* path = "gage_server_test_accounts.json";
            WriteTextFile(path, "{ \"platformUrl\": \"http://127.0.0.1:8788\", \"platformServerId\": \"sol\","
                                " \"platformServerSecret\": \"0123456789abcdefXYZ\", \"requireAccount\": true, \"adminRoles\": [\"admin\", \"owner\", \"admin\"] }");
            server::Config c;
            std::string err;
            const bool ok = server::LoadConfigFile(path, c, &err);
            server::Config d = Rig::BaseConfig(), noUrl = d, badId = d, noSecret = d, pathUrl = d, emptyRoles = d;
            d.platformUrl = "http://127.0.0.1:8788";
            d.platformServerId = "sol";
            d.platformServerSecret = "0123456789abcdefXYZ";
            noUrl.requireAccount = true;
            badId = d; badId.platformServerId = "Sol!";
            noSecret = d; noSecret.platformServerSecret = "";
            pathUrl = d; pathUrl.platformUrl = "http://127.0.0.1:8788/internal";
            emptyRoles.adminRoles.clear();
            std::string e1, e2, e3, e4, e5, e6;
            const bool good = server::Validate(d, &e6);
            Check("v8 config: platformUrl / platformServerId / platformServerSecret / requireAccount / adminRoles load (roles de-duplicated); "
                  "Validate refuses requireAccount without platformUrl, a bad server id, a missing secret, a URL with a path and no admin role",
                  ok && c.platformUrl == "http://127.0.0.1:8788" && c.platformServerId == "sol" && c.platformServerSecret == "0123456789abcdefXYZ" &&
                      c.requireAccount && c.adminRoles.size() == 2 && c.adminRoles[1] == "owner" && good && !server::Validate(noUrl, &e1) &&
                      !server::Validate(badId, &e2) && !server::Validate(noSecret, &e3) && !server::Validate(pathUrl, &e4) &&
                      !server::Validate(emptyRoles, &e5) && server::Config().requireAccount == false,
                  err + " | " + e6);
            std::remove(path);
        }
    }

    static void LiveTests()
    {
        FakeMaster fake;
        if (!fake.Start()) { Check("v8 accounts: fake platform starts", false); return; }
        fake.Respond(PlatformAnswer);

        // ---- requireAccount ON ----
        {
            Rig rig;
            if (!rig.Start(PlatformConfig(fake, true))) { Check("v8 accounts: server with requireAccount starts", false); return; }

            // A verified player: the account's username replaces the Hello name; AccountInfo closes the burst.
            Probe pia(rig);
            const bool joined = JoinWith(rig, pia, "whatever", "tok-player");
            const net::Event* ai = Account(pia);
            const std::vector<server::PlayerInfo> all = rig.srv.Players();
            const server::PlayerInfo* pi = Info(all, MyId(pia));
            const std::vector<FakeMaster::Req> reqs = fake.Requests();
            bool headers = false;
            for (const FakeMaster::Req& r : reqs)
                if (r.head.rfind("POST /internal/game/verify ", 0) == 0)
                    headers = (r.head + "\r\n").find("\r\nX-Gage-Server-Id: sol\r\n") != std::string::npos &&    // head ends before its last CRLF
                              (r.head + "\r\n").find(std::string("\r\nX-Gage-Server-Secret: ") + kSecret + "\r\n") != std::string::npos &&
                              r.body == "{\"token\":\"tok-player\"}";
            bool secretLogged = false, tokenLogged = false;
            for (const std::string& l : rig.logs)
            {
                if (l.find(kSecret) != std::string::npos) secretLogged = true;
                if (l.find("tok-player") != std::string::npos) tokenLogged = true;
            }
            Check("v8 live: a valid token joins under the account's username, the server records account id, roles and verified, the "
                  "verify POST carries X-Gage-Server-Id / X-Gage-Server-Secret and {token}, and neither the token nor the secret is logged",
                  joined && pi && pi->name == "Pia_Player" && pi->verified && pi->accountId == "acc-player" && pi->roles.size() == 1 &&
                      !pi->isAdmin && headers && !secretLogged && !tokenLogged && rig.srv.stats().authVerified == 1,
                  pia.Describe() + (pi ? " name=" + pi->name : std::string(" no player")) + " headers=" + std::to_string(headers) +
                      " secretLogged=" + std::to_string(secretLogged) + " tokenLogged=" + std::to_string(tokenLogged) +
                      " verified=" + std::to_string(rig.srv.stats().authVerified));
            Check("v8 live: AccountInfo closes the Welcome burst: verified, the username, no admin and no admin commands for the role player",
                  ai && (ai->account.flags & proto::kAcctVerified) && !(ai->account.flags & proto::kAcctAdmin) &&
                      !(ai->account.flags & proto::kAcctCommands) && ai->account.username == "Pia_Player" &&
                      pia.s.accountName() == "Pia_Player" && pia.events.size() >= 2 && pia.events[0].type == ET::Welcome,
                  pia.Describe());

            // Admin commands: refused for the role player, also the vehicle Spawn (the Cars page / /car path).
            const bool placed = Placed(rig, pia, 100.0f, 100.0f);
            const proto::AdminResult heal = Admin(rig, pia, proto::AdminOp::Heal);
            const proto::AdminResult car = Admin(rig, pia, proto::AdminOp::Car, "adder");
            const proto::VehicleResult spawn = Veh(rig, pia, proto::VehicleOp::Spawn, 0xB779A091u);
            Check("v8 live: a player without the admin role gets NotAdmin for /heal and /car with the sentence, and VehicleCmd Spawn is "
                  "refused NotAllowed (no vehicle is created)",
                  placed && heal.code == proto::AdminCode::NotAdmin && heal.op == proto::AdminOp::Heal &&
                      heal.text == "Only admins can use /heal on this server." && car.code == proto::AdminCode::NotAdmin &&
                      spawn.code == proto::VehicleResultCode::NotAllowed && rig.srv.Vehicles().size() == 0 && rig.LogHas("REFUSED (not an admin)"),
                  std::string(proto::ToString(heal.code)) + " / " + proto::ToString(spawn.code));

            // An admin: AccountInfo says so, the same commands pass, the heal is done by the server, a Spawn creates a car.
            Probe ada(rig);
            const bool adaIn = JoinWith(rig, ada, "Ada", "tok-admin");
            const net::Event* adaAi = Account(ada);
            const bool adaPlaced = Placed(rig, ada, 200.0f, 100.0f);
            const proto::AdminResult aHeal = Admin(rig, ada, proto::AdminOp::Heal);
            const proto::AdminResult aCar = Admin(rig, ada, proto::AdminOp::Car, "adder");
            const proto::VehicleResult aSpawn = Veh(rig, ada, proto::VehicleOp::Spawn, 0xB779A091u);
            const std::vector<server::PlayerInfo> all2 = rig.srv.Players();
            const server::PlayerInfo* adaInfo = Info(all2, MyId(ada));
            Check("v8 live: the role admin is an admin: AccountInfo carries admin + commands, /heal and /car answer Ok, Spawn creates a vehicle",
                  adaIn && adaAi && (adaAi->account.flags & proto::kAcctAdmin) && (adaAi->account.flags & proto::kAcctCommands) && adaPlaced &&
                      aHeal.code == proto::AdminCode::Ok && aCar.code == proto::AdminCode::Ok && aSpawn.code == proto::VehicleResultCode::Ok &&
                      rig.srv.Vehicles().size() == 1 && adaInfo && adaInfo->isAdmin && rig.srv.stats().adminAllowed >= 2,
                  ada.Describe() + " " + proto::ToString(aHeal.code) + " / " + proto::ToString(aSpawn.code));

            // The role mod is not an admin under the default adminRoles ["admin"].
            Probe mo(rig);
            const bool moIn = JoinWith(rig, mo, "Mo", "tok-mod");
            const proto::AdminResult mHeal = Admin(rig, mo, proto::AdminOp::Noclip);
            Check("v8 live: the role mod is no admin by default (adminRoles [\"admin\"]): /noclip is NotAdmin",
                  moIn && mHeal.code == proto::AdminCode::NotAdmin, proto::ToString(mHeal.code));

            // Refusals: no token, expired, used, invalid, banned, platform 500, a username that is no valid name.
            Probe none(rig), expd(rig), used(rig), inval(rig), banned(rig), down(rig), bad(rig);
            JoinWith(rig, none, "Nobody", "");
            JoinWith(rig, expd, "Exp", "tok-expired");
            JoinWith(rig, used, "Used", "tok-used");
            JoinWith(rig, inval, "Inval", "tok-nonsense");
            JoinWith(rig, banned, "Ban", "tok-banned");
            JoinWith(rig, down, "Down", "tok-500");
            JoinWith(rig, bad, "Bad", "tok-badname");
            const net::Event* exR = expd.First(ET::Refused);
            const net::Event* baR = banned.First(ET::Refused);
            Check("v8 live (requireAccount on): no token -> AccountRequired; nothing reached the platform for it",
                  none.RefusedWith(proto::RefuseReason::AccountRequired) && !none.First(ET::Welcome) &&
                      Count(fake.Requests(), "/internal/game/verify", "\"token\":\"\"") == 0,
                  none.Describe());
            Check("v8 live (requireAccount on): expired, used and unknown tokens -> AuthFailed with a 'press Play again' sentence",
                  expd.RefusedWith(proto::RefuseReason::AuthFailed) && used.RefusedWith(proto::RefuseReason::AuthFailed) &&
                      inval.RefusedWith(proto::RefuseReason::AuthFailed) && exR && exR->refuse.text.find("expired") != std::string::npos &&
                      exR->refuse.text.find("Press Play") != std::string::npos && !expd.First(ET::Welcome),
                  (exR ? exR->refuse.text : std::string()) + " | expd " + Why(rig, expd, "(Exp)") + " | used " + Why(rig, used, "(Used)") +
                      " | inval " + Why(rig, inval, "(Inval)"));
            Check("v8 live: a banned account -> Banned with the platform's reason and 'permanently'",
                  banned.RefusedWith(proto::RefuseReason::Banned) && baR && baR->refuse.text.find("cheating") != std::string::npos &&
                      baR->refuse.text.find("permanently") != std::string::npos,
                  baR ? baR->refuse.text : Why(rig, banned, "(Ban)"));
            Check("v8 live (requireAccount on): the platform answering 500 -> AuthUnavailable (fail closed); an account name that is no "
                  "valid in-game name -> BadName and its session is ended",
                  down.RefusedWith(proto::RefuseReason::AuthUnavailable) && bad.RefusedWith(proto::RefuseReason::BadName) &&
                      rig.Until([&] { return Count(fake.Requests(), "/internal/game/session-end", "ses-acc-bad") >= 1; }, 3000),
                  Why(rig, down, "(Down)") + " / " + Why(rig, bad, "(Bad)"));

            // The same account joins again: the older connection is removed, the new one plays; the old session is ended.
            Probe pia2(rig);
            const bool again = JoinWith(rig, pia2, "Pia2", "tok-player2");
            rig.Until([&] { pia.Drain(); return pia.Closed(); }, 3000);
            const std::vector<server::PlayerInfo> all3 = rig.srv.Players();
            int piaCount = 0;
            for (const server::PlayerInfo& i : all3)
                if (i.accountId == "acc-player") piaCount++;
            Check("v8 live: the same account joining again replaces its older connection (one player per account, the old one is kicked)",
                  again && pia.Closed() && piaCount == 1 && Info(all3, MyId(pia2)) && Info(all3, MyId(pia2))->name == "Pia_Player",
                  Why(rig, pia, "Pia_Player") + " | " + pia2.Describe() + " again=" + std::to_string(again) + " piaCount=" + std::to_string(piaCount));

            // Leaving ends the platform session (reason quit).
            const int endsBefore = Count(fake.Requests(), "/internal/game/session-end", "\"sessionId\":\"ses-acc-admin\"");
            ada.s.Disconnect();
            rig.Until([&] { return Count(fake.Requests(), "/internal/game/session-end", "\"sessionId\":\"ses-acc-admin\",\"reason\":\"quit\"") > endsBefore; }, 4000);
            Check("v8 live: a verified player's leave sends POST /internal/game/session-end {sessionId, reason quit}",
                  Count(fake.Requests(), "/internal/game/session-end", "\"sessionId\":\"ses-acc-admin\",\"reason\":\"quit\"") == 1,
                  std::to_string(Count(fake.Requests(), "/internal/game/session-end")));
        }

        // ---- requireAccount OFF ----
        {
            Rig rig;
            if (!rig.Start(PlatformConfig(fake, false))) { Check("v8 accounts: server without requireAccount starts", false); return; }
            Probe guest(rig), lapsed(rig), down(rig), pia(rig), banned(rig);
            const bool gIn = JoinWith(rig, guest, "Gus", "");
            const bool lIn = JoinWith(rig, lapsed, "Lapsed", "tok-expired");
            const bool dIn = JoinWith(rig, down, "Downy", "tok-500");
            const bool pIn = JoinWith(rig, pia, "Pia", "tok-player");
            JoinWith(rig, banned, "Ban", "tok-banned");
            const net::Event* gAi = Account(guest);
            const std::vector<server::PlayerInfo> all = rig.srv.Players();
            const server::PlayerInfo* gi = Info(all, MyId(guest));
            Check("v8 live (requireAccount off): no token, an expired token and a platform failure join as guests under the Hello name, "
                  "each logged; a banned account is still refused",
                  gIn && lIn && dIn && gi && gi->name == "Gus" && !gi->verified && rig.LogHas("Gus (127.0.0.1) joins as a guest: no join token") &&
                      rig.LogHas("Lapsed (127.0.0.1) joins as a guest: the platform refused its join token (expired)") &&
                      rig.LogHas("joins as a guest: the platform could not verify") && banned.RefusedWith(proto::RefuseReason::Banned) &&
                      rig.srv.stats().authGuests == 3,
                  guest.Describe() + " | " + banned.Describe());
            Check("v8 live (requireAccount off): a guest's AccountInfo is not verified but allows the admin commands (roles unknown, today's "
                  "behaviour); a verified player in the same server is still held to its roles",
                  gAi && !(gAi->account.flags & proto::kAcctVerified) && (gAi->account.flags & proto::kAcctCommands) &&
                      gAi->account.username.empty() && pIn && Account(pia) && !(Account(pia)->account.flags & proto::kAcctCommands),
                  guest.Describe());
            const bool gPlaced = Placed(rig, guest, 50.0f, 50.0f), pPlaced = Placed(rig, pia, 300.0f, 50.0f);
            const proto::AdminResult gFix = Admin(rig, guest, proto::AdminOp::Fix);
            const proto::VehicleResult gSpawn = Veh(rig, guest, proto::VehicleOp::Spawn, 0xB779A091u);
            const proto::AdminResult pFix = Admin(rig, pia, proto::AdminOp::Fix);
            const proto::VehicleResult pSpawn = Veh(rig, pia, proto::VehicleOp::Spawn, 0xB779A091u);
            Check("v8 live (requireAccount off): a guest's /fix and Spawn are allowed and logged as 'roles unknown'; the verified player "
                  "without the role is refused both",
                  gPlaced && pPlaced && gFix.code == proto::AdminCode::Ok && gSpawn.code == proto::VehicleResultCode::Ok &&
                      pFix.code == proto::AdminCode::NotAdmin && pSpawn.code == proto::VehicleResultCode::NotAllowed &&
                      rig.LogHas("allowed: a guest on a server that does not require accounts") && rig.srv.stats().adminUnknownRoles >= 2,
                  std::string(proto::ToString(gFix.code)) + " " + proto::ToString(gSpawn.code) + " " + proto::ToString(pFix.code) + " " +
                      proto::ToString(pSpawn.code));

            // A guest holding the account's name gives it up when the account joins.
            Probe squat(rig), owner(rig);
            const bool sIn = JoinWith(rig, squat, "Ada_Admin", "");
            const bool oIn = JoinWith(rig, owner, "Ada", "tok-admin");
            rig.Until([&] { squat.Drain(); return squat.Closed(); }, 3000);
            const std::vector<server::PlayerInfo> all2 = rig.srv.Players();
            Check("v8 live: a guest playing under an account's username is removed when that account joins; the account gets the name",
                  sIn && oIn && squat.Closed() && Info(all2, MyId(owner)) && Info(all2, MyId(owner))->name == "Ada_Admin",
                  squat.Describe() + " | " + owner.Describe());

            // Shutdown ends every verified player's session (reason shutdown).
            const int before = Count(fake.Requests(), "/internal/game/session-end", "\"reason\":\"shutdown\"");
            rig.srv.Stop();
            const int after = Count(fake.Requests(), "/internal/game/session-end", "\"reason\":\"shutdown\"");
            Check("v8 live: a server shutdown sends session-end reason shutdown for each verified player (Pia_Player and Ada_Admin) before it stops",
                  after - before == 2, std::to_string(before) + " -> " + std::to_string(after));
        }

        // ---- no platformUrl: a token is ignored, everybody is a guest, today's behaviour ----
        {
            Rig rig;
            if (!rig.Start()) { Check("v8 accounts: server without platform starts", false); return; }
            Probe p(rig);
            const bool in = JoinWith(rig, p, "Plain", "tok-admin");
            const net::Event* ai = Account(p);
            const bool placed = Placed(rig, p, 10.0f, 10.0f);
            const proto::AdminResult heal = Admin(rig, p, proto::AdminOp::Heal);
            Check("v8 live (no platformUrl): a client with a token joins as a guest under its Hello name, may use the admin commands, and "
                  "the platform was never asked",
                  in && ai && (ai->account.flags & proto::kAcctCommands) && !(ai->account.flags & proto::kAcctVerified) && placed &&
                      heal.code == proto::AdminCode::Ok && rig.LogHas("a join token, ignored: no platformUrl"),
                  p.Describe());
        }

        // ---- a v7-style client (Hello only, never a JoinAuth) and a slow platform ----
        {
            server::Config c = PlatformConfig(fake, false);
            c.helloTimeoutMs = 800;
            Rig rig;
            if (!rig.Start(c)) { Check("v8 accounts: server for the timing checks starts", false); return; }
            Probe old(rig);
            old.Connect("Oldie", [](net::SessionOptions& o) { o.autoJoinAuth = false; });
            old.WaitClosed(3000);
            old.Drain();
            Check("v8 live: a client that sends Hello but no JoinAuth is refused HelloTimeout after helloTimeoutMs (never joins)",
                  old.RefusedWith(proto::RefuseReason::HelloTimeout) && !old.First(ET::Welcome) && rig.LogHas("no JoinAuth within 800 ms"),
                  old.Describe());
        }
        {
            FakeMaster slow;
            if (!slow.Start()) { Check("v8 accounts: slow platform starts", false); return; }
            FakeMaster::Reply hang;
            hang.mode = FakeMaster::Mode::Hang;
            slow.Script({hang});
            server::Config c = PlatformConfig(slow, true);
            c.platformTimeoutMs = 400;
            Rig rig;
            if (!rig.Start(c)) { Check("v8 accounts: server for the slow platform starts", false); return; }
            Probe p(rig);
            p.Connect("Slow", WithToken("tok-player"));
            p.WaitClosed(6000);
            p.Drain();
            Check("v8 live (requireAccount on): a platform that never answers -> AuthUnavailable within the platform timeout, no Welcome",
                  p.RefusedWith(proto::RefuseReason::AuthUnavailable) && !p.First(ET::Welcome), Why(rig, p, "Slow"));
        }
        {
            // A verdict handled in the server's Tick starts a close with a clock reading LATER than the one the Tick's
            // timers use. The stuck-close timer computed that age unsigned, wrapped it, and reset the peer in the same
            // pass: the Refuse never left and the client saw a handshake timeout (about 1 run in 2 under load). The log
            // sink runs inside the Tick before the verdict, so stalling it 3 ms on the worker's line makes the order certain.
            Rig rig;
            rig.onLog = [](const std::string& line) {
                if (line.find("platform: verify FAILED") != std::string::npos) std::this_thread::sleep_for(std::chrono::milliseconds(3));
            };
            if (!rig.Start(PlatformConfig(fake, true))) { Check("v8 accounts: server for the clock-order check starts", false); return; }
            int refused = 0;
            std::string detail;
            for (int i = 0; i < 3; i++)
            {
                Probe p(rig);
                JoinWith(rig, p, "Late", "tok-500");
                if (p.RefusedWith(proto::RefuseReason::AuthUnavailable)) refused++;
                else detail = Why(rig, p, "(Late)");
                p.s.Disconnect();
                rig.Pump(50);
            }
            Check("v8 live: a refusal decided in the server's Tick (the platform's verdict) reaches the client even when the clock moved "
                  "on between the Tick's timers and the close (3 of 3; the stuck-close timer used to reset the peer at once)",
                  refused == 3, std::to_string(refused) + " of 3 refused" + (detail.empty() ? std::string() : " | " + detail));
        }
    }

    static void Tests()
    {
        UnitTests();
        LiveTests();
        // the shipped example lists the v8 keys at their defaults
        std::string here = __FILE__;
        const size_t slash = here.find_last_of("/\\");
        const std::string example = (slash == std::string::npos ? std::string(".") : here.substr(0, slash)) + "/../apps/server/server.json.example";
        server::Config c, d;
        std::string err;
        const bool ok = server::LoadConfigFile(example, c, &err);
        Check("v8 config: server.json.example lists platformUrl, platformServerId, requireAccount and adminRoles at their defaults",
              ok && c.platformUrl == d.platformUrl && c.platformServerId == d.platformServerId && c.requireAccount == d.requireAccount &&
                  c.adminRoles == d.adminRoles,
              err);
    }

}
