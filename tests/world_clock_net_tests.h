#pragma once
// 0.5.7 world clock over the wire (proto::WorldClock 55; apps/server Server::SendWorldClock / BroadcastWorldClock / WorldClockCommand /
// ChatCommand): the real server and the real ClientSession on loopback. Included by tests/server_test.cpp after account_tests.h (it reuses
// v8acct's fake platform for a verified admin). Every check runs on the passing path (kChecks of them). The pure rules (the clock line, the
// weather plan, the client's holds, the codec) are tests/world_clock_tests.h in gameplay_test.

#include "sync/world_clock.h"

namespace wclocknet {

    constexpr int kChecks = 7;
    namespace wc = gage::sync::wclock;

    static int Clocks(Probe& p)
    {
        p.Drain();
        return p.Count(ET::WorldClock);
    }
    static const proto::WorldClock* LastClock(const Probe& p)
    {
        for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
            if (it->type == ET::WorldClock) return &it->worldClock;
        return nullptr;
    }
    static bool WaitClocks(Rig& rig, Probe& p, int atLeast, int ms = 1500)
    {
        return rig.Until([&] { return Clocks(p) >= atLeast; }, ms);
    }
    // The newest server notice (a Chat from the server) that contains `part`.
    static bool HeardNotice(Rig& rig, Probe& p, const std::string& part, int ms = 1500)
    {
        return rig.Until([&] {
            p.Drain();
            for (const net::Event& e : p.events)
                if (e.type == ET::Chat && e.chat.fromId == proto::kServerId && e.chat.text.find(part) != std::string::npos) return true;
            return false;
        }, ms);
    }
    // The game time of a WorldClock is within `slackMs` game ms after `expect` (it ran on for the time the test took).
    static bool Near(const proto::WorldClock* m, int64_t expect, int64_t slackMs)
    {
        if (!m) return false;
        const int64_t d = wc::WrapDay((int64_t)m->gameMs - expect);
        return d <= slackMs;
    }
    static std::string Show(const proto::WorldClock* m)
    {
        if (!m) return "no WorldClock";
        return "rev " + std::to_string(m->rev) + " " + wc::FormatHms(m->gameMs) + " x" + wc::FormatScale(m->scaleMilli) + " " +
               wc::WeatherName(m->weatherFrom) + "->" + wc::WeatherName(m->weatherTo) + " " + std::to_string(m->transElapsedMs) + "/" +
               std::to_string(m->transTotalMs) + " flags " + std::to_string(m->flags);
    }

    // ---- join, console changes ----
    static void JoinAndConsole()
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.clockStartMs = 18LL * 3600000;
        cfg.weatherFixed = wc::kRain;
        if (!rig.Start(cfg)) { Check("world clock live: server starts", false); return; }
        Probe ana(rig), ben(rig);
        const bool in = ana.Join("Ana") && ben.Join("Ben");
        const bool got = WaitClocks(rig, ana, 1) && WaitClocks(rig, ben, 1);
        const proto::WorldClock* a0 = LastClock(ana);
        const proto::WorldClock a0copy = a0 ? *a0 : proto::WorldClock();
        Check("world clock live: a joiner receives the server's clock (18:00 at GTA's rate, steady RAIN, fixed) right after joining",
              in && got && a0 && Near(a0, 18LL * 3600000, 5 * 60000) && a0->scaleMilli == wc::kGtaScaleMilli && a0->weatherFrom == wc::kRain &&
                  a0->weatherTo == wc::kRain && a0->transTotalMs == 0 && a0->flags == 0 && rig.LogHas("world clock: on"),
              Show(a0) + " | " + ana.Describe());

        // console: a new time reaches everybody with a new rev
        const int aBefore = Clocks(ana), bBefore = Clocks(ben);
        std::string out;
        const server::CommandResult r1 = rig.srv.Execute("time 06:30", out);
        const bool both = WaitClocks(rig, ana, aBefore + 1) && WaitClocks(rig, ben, bBefore + 1);
        const proto::WorldClock* a1 = LastClock(ana);
        const proto::WorldClock* b1 = LastClock(ben);
        Check("world clock live: console 'time 06:30' answers Ok and both players get 06:30 with a higher rev; the log names who set it",
              in && r1 == server::CommandResult::Ok && both && a1 && b1 && Near(a1, 6LL * 3600000 + 30 * 60000, 60000) &&
                  Near(b1, 6LL * 3600000 + 30 * 60000, 60000) && a1->rev > a0copy.rev && a1->rev == b1->rev &&
                  rig.LogHas("world clock: time set to 06:30:00 by the console"),
              out + " | " + Show(a1) + " | " + Show(b1));

        // console: weather and scale; a bad argument is an Error and sends nothing
        const int aBefore2 = Clocks(ana);
        std::string o2, o3, o4, o5, o6;
        const server::CommandResult r2 = rig.srv.Execute("weather thunder", o2);
        const bool wGot = WaitClocks(rig, ana, aBefore2 + 1);
        const proto::WorldClock* a2 = LastClock(ana);
        const proto::WorldClock a2copy = a2 ? *a2 : proto::WorldClock();
        const server::CommandResult r3 = rig.srv.Execute("timescale 0", o3);
        const bool sGot = WaitClocks(rig, ana, aBefore2 + 2);
        const proto::WorldClock* a3 = LastClock(ana);
        const proto::WorldClock a3copy = a3 ? *a3 : proto::WorldClock();
        const int before = Clocks(ana);
        const server::CommandResult r4 = rig.srv.Execute("time 25:00", o4);
        const server::CommandResult r5 = rig.srv.Execute("weather snowy", o5);
        const server::CommandResult r6 = rig.srv.Execute("time", o6);
        rig.Pump(300);
        const bool quiet = Clocks(ana) == before;
        Check("world clock live: 'weather thunder' blends RAIN -> THUNDER over 45 s, 'timescale 0' freezes the clock, both reach the player; "
              "'time 25:00' and 'weather snowy' are errors that send nothing; bare 'time' prints the status",
              r2 == server::CommandResult::Ok && wGot && a2copy.weatherFrom == wc::kRain && a2copy.weatherTo == wc::kThunder &&
                  a2copy.transTotalMs == 45000 && a2copy.transElapsedMs < 2000 && r3 == server::CommandResult::Ok && sGot && a3copy.scaleMilli == 0 &&
                  a3copy.rev > a2copy.rev && r4 == server::CommandResult::Error && r5 == server::CommandResult::Error && quiet &&
                  r6 == server::CommandResult::Ok && o6.find("06:3") != std::string::npos && o6.find("x0.000") != std::string::npos,
              o2 + " | " + o3 + " | " + o4 + " | " + o5 + " | " + o6 + " | " + Show(&a2copy) + " | " + Show(&a3copy));

        // a guest's /time is refused: a notice, a REFUSED log line, no broadcast, the clock unchanged
        const int bBefore3 = Clocks(ben);
        const bool sent = ben.s.SendChat("/time 12:00");
        const bool told = HeardNotice(rig, ben, "Only admins can change the time or weather");
        rig.Pump(200);
        const proto::WorldClock now = rig.srv.WorldClockNow();
        bool chatLeaked = false;
        ana.Drain();
        for (const net::Event& e : ana.events)
            if (e.type == ET::Chat && e.chat.text.find("/time") != std::string::npos) chatLeaked = true;
        Check("world clock live: a guest's /time 12:00 is refused with a notice and a REFUSED log line; nothing is broadcast, the clock stays "
              "06:30 and the command is not relayed as chat",
              sent && told && Clocks(ben) == bBefore3 && Near(&now, 6LL * 3600000 + 30 * 60000, 180000) && !chatLeaked &&
                  rig.LogHas("REFUSED (not a verified admin)"),
              ben.Describe() + " | " + Show(&now));
    }

    // ---- a verified admin's chat commands (fake platform) ----
    static void AdminChat()
    {
        FakeMaster fake;
        if (!fake.Start()) { Check("world clock live: fake platform starts", false); return; }
        fake.Respond(v8acct::PlatformAnswer);
        Rig rig;
        if (!rig.Start(v8acct::PlatformConfig(fake, false))) { Check("world clock live: server with a platform starts", false); return; }
        Probe ada(rig), pia(rig);
        const bool in = v8acct::JoinWith(rig, ada, "Ada", "tok-admin") && v8acct::JoinWith(rig, pia, "Pia", "tok-player");
        WaitClocks(rig, pia, 1);
        const int before = Clocks(pia);
        const bool sent = ada.s.SendChat("/weather clear");
        const bool told = HeardNotice(rig, ada, "weather set to CLEAR");
        const bool got = WaitClocks(rig, pia, before + 1);
        const proto::WorldClock* p1 = LastClock(pia);
        const int before2 = Clocks(pia);
        const bool sent2 = pia.s.SendChat("/timescale 60");
        const bool refused = HeardNotice(rig, pia, "Only admins");
        rig.Pump(200);
        Check("world clock live: a verified admin's /weather clear answers with a notice and reaches the other player as a blend to CLEAR; a "
              "verified non-admin's /timescale is refused",
              in && sent && told && got && p1 && p1->weatherTo == wc::kClear && sent2 && refused && Clocks(pia) == before2 &&
                  rig.LogHas("weather set to CLEAR by id="),
              Show(p1) + " | " + ada.Describe() + " | " + pia.Describe());
    }

    // ---- worldClock 0, and the server.json keys ----
    static void OffAndConfig()
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.worldClock = 0;
        if (!rig.Start(cfg)) { Check("world clock live: server starts (off)", false); return; }
        Probe ana(rig);
        const bool in = ana.Join("Ana");
        rig.Pump(500);
        std::string out;
        const server::CommandResult r = rig.srv.Execute("time 12:00", out);
        rig.Pump(200);
        const bool none = Clocks(ana) == 0 && rig.srv.WorldClocksSent() == 0;

        const char* path = "gage_server_test_wclock.json";
        WriteTextFile(path, "{ \"worldClock\": 1, \"clockStart\": \"07:15\", \"clockScale\": 0, \"weather\": \"cycle\", "
                            "\"weatherCycle\": [[\"RAIN\", 5], [\"clear\", 10]], \"weatherTransitionSeconds\": 20 }");
        server::Config c;
        std::string err;
        const bool loaded = server::LoadConfigFile(path, c, &err);
        WriteTextFile(path, "{ \"weather\": \"FOGGY\", \"clockScale\": 1.5 }");
        server::Config c2;
        std::string e2;
        const bool loaded2 = server::LoadConfigFile(path, c2, &e2);
        int refusedN = 0;
        std::string reasons;
        const char* bad[] = {"{ \"weather\": \"SNOWY\" }", "{ \"clockStart\": \"25:00\" }", "{ \"weatherCycle\": [[\"RAIN\", 0]] }",
                             "{ \"weatherCycle\": [] }", "{ \"clockScale\": 3601 }", "{ \"weatherTransitionSeconds\": 601 }", "{ \"worldClock\": 2 }"};
        for (const char* b : bad)
        {
            WriteTextFile(path, b);
            server::Config cb;
            std::string eb;
            if (!server::LoadConfigFile(path, cb, &eb)) ++refusedN;
            reasons += " [" + eb + "]";
        }
        std::remove(path);
        const server::Config d;
        Check("world clock live: worldClock 0 sends no WorldClock and the console refuses 'time'; the six keys load (a cycle in any case, "
              "decimal scales), 7 bad values are refused; the defaults are on, 12:00, GTA's rate, the cycle, 45 s",
              in && r == server::CommandResult::Error && none && loaded && c.worldClock == 1 && c.clockStartMs == (7LL * 3600 + 15 * 60) * 1000 &&
                  c.clockScaleMilli == 0 && c.weatherFixed == -1 && c.weatherCycle.size() == 2 && c.weatherCycle[1].weather == wc::kClear &&
                  c.weatherCycle[1].minutes == 10 && c.weatherTransitionSeconds == 20 && loaded2 && c2.weatherFixed == wc::kFoggy &&
                  c2.clockScaleMilli == 1500 && refusedN == 7 && d.worldClock == 1 && d.clockStartMs == 12LL * 3600000 &&
                  d.clockScaleMilli == wc::kGtaScaleMilli && d.weatherFixed == -1 && d.weatherCycle.size() == wc::DefaultCycle().size() &&
                  d.weatherTransitionSeconds == 45,
              out + " | " + err + " | " + e2 + " |" + reasons);

        // the shipped example lists the keys at their defaults
        std::string here = __FILE__;
        const size_t slash = here.find_last_of("/\\");
        const std::string example = (slash == std::string::npos ? std::string(".") : here.substr(0, slash)) + "/../apps/server/server.json.example";
        server::Config ex;
        std::string ee;
        const bool exOk = server::LoadConfigFile(example, ex, &ee);
        bool sameCycle = ex.weatherCycle.size() == d.weatherCycle.size();
        for (size_t i = 0; sameCycle && i < d.weatherCycle.size(); i++)
            sameCycle = ex.weatherCycle[i].weather == d.weatherCycle[i].weather && ex.weatherCycle[i].minutes == d.weatherCycle[i].minutes;
        Check("world clock config: server.json.example lists worldClock, clockStart, clockScale, weather, weatherCycle and weatherTransitionSeconds "
              "at their defaults",
              exOk && ex.worldClock == d.worldClock && ex.clockStartMs == d.clockStartMs && ex.clockScaleMilli == d.clockScaleMilli &&
                  ex.weatherFixed == d.weatherFixed && sameCycle && ex.weatherTransitionSeconds == d.weatherTransitionSeconds,
              ee);
    }

    static void Tests()
    {
        JoinAndConsole();   // 4
        AdminChat();        // 1
        OffAndConfig();     // 2
    }

}  // namespace wclocknet
