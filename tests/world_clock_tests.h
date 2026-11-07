// 0.5.7 world clock (libs/sync/world_clock.h + proto::WorldClock 55): the weather table against alloc8or's hashes, the parsers, the clock line,
// the weather plan (fixed, cycle, admin changes), the client's copy (two machines with different round trips show the same second), the two
// holds (which natives a tick calls, the read-back verdicts) and the codec with its refusals.
// Included by tests/gameplay_test.cpp after its Check() and F(); run by worldclock_t::RunPure(), kChecks checks.
#pragma once

#include "sync/world_clock.h"
#include "protocol/protocol.h"

#include <string>
#include <vector>

namespace worldclock_t {

    namespace wc = gage::sync::wclock;
    namespace proto = gage::proto;

    constexpr int kChecks = 14;

    constexpr int64_t kMin = 60000;

    inline void RunPure()
    {
        {
            // alloc8or MISC::GET_CURR_WEATHER_STATE / SET_WEATHER_TYPE_NOW_PERSIST doc: the 12 weather hashes it lists
            struct Pin { const char* name; uint32_t hash; };
            const Pin pins[] = {{"CLEAR", 0x36A83D84u},    {"EXTRASUNNY", 0x97AA0A79u}, {"CLOUDS", 0x30FDAF5Cu}, {"OVERCAST", 0xBB898D2Du},
                                {"RAIN", 0x54A69840u},     {"CLEARING", 0x6DB1A50Du},   {"THUNDER", 0xB677829Fu}, {"SMOG", 0x10DCF4B5u},
                                {"FOGGY", 0xAE737644u},    {"XMAS", 0xAAC9C895u},       {"SNOWLIGHT", 0x23FB812Bu}, {"BLIZZARD", 0x27EA2814u}};
            int good = 0;
            std::string bad;
            for (const Pin& p : pins)
            {
                const int i = wc::FindWeather(p.name);
                if (i >= 0 && wc::WeatherHash((uint8_t)i) == p.hash && wc::WeatherOfHash(p.hash) == i)
                    ++good;
                else
                    bad += std::string(" ") + p.name;
            }
            Check("world clock: the weather table's hashes equal the 12 alloc8or lists (GET_HASH_KEY of the name), each found by name and by hash",
                  good == 12, bad);
        }
        {
            // wire-stable indices (WorldClock carries the index) and the name lookup
            const bool idx = wc::kWeatherCount == 15 && wc::FindWeather("EXTRASUNNY") == 0 && wc::FindWeather("CLEAR") == 1 &&
                             wc::FindWeather("RAIN") == 6 && wc::FindWeather("THUNDER") == 7 && wc::FindWeather("HALLOWEEN") == 14;
            const bool look = wc::FindWeather("rain") == 6 && wc::FindWeather("ExtraSunny") == 0 && wc::FindWeather("RAINY") < 0 &&
                              wc::FindWeather("RAI") < 0 && wc::FindWeather("") < 0 && wc::WeatherOfHash(0x12345678u) < 0 &&
                              std::string(wc::WeatherName(200)) == "?";
            Check("world clock: weather indices are wire-stable (EXTRASUNNY 0, CLEAR 1, RAIN 6, THUNDER 7, HALLOWEEN 14 of 15); names match in any "
                  "case, a prefix or a longer name does not",
                  idx && look);
        }
        {
            // the shipped cycle: the everyday sky only, 90 minutes a lap
            const std::vector<wc::CycleEntry> c = wc::DefaultCycle();
            bool everyday = !c.empty();
            int64_t lap = 0;
            for (const wc::CycleEntry& e : c)
            {
                const std::string n = wc::WeatherName(e.weather);
                if (n == "SNOW" || n == "BLIZZARD" || n == "SNOWLIGHT" || n == "XMAS" || n == "HALLOWEEN" || n == "THUNDER" || n == "?" ||
                    e.minutes == 0)
                    everyday = false;
                lap += e.minutes;
            }
            Check("world clock: the default cycle has no snow, blizzard, xmas, halloween or thunder and laps in 90 real minutes", everyday && lap == 90,
                  F("%d entries, %lld min", (int)c.size(), (long long)lap));
        }
        {
            int64_t a = -1, b = -1, c = -1, d = -1;
            const bool ok = wc::ParseClockTime("18:30", a) && a == (18 * 3600 + 30 * 60) * 1000LL && wc::ParseClockTime("6:05", b) &&
                            b == (6 * 3600 + 5 * 60) * 1000LL && wc::ParseClockTime("23:59:59", c) && c == 86399000LL &&
                            wc::ParseClockTime("00:00", d) && d == 0;
            int64_t x = 0;
            const char* refused[] = {"24:00", "12:60", "12:5", "", "12", "1:02:03:04", "ab:cd", "123:00", "-1:00", "12:00:60", "12:00:5", ":30", "12:"};
            int refusedOk = 0;
            for (const char* s : refused)
                if (!wc::ParseClockTime(s, x)) ++refusedOk;
            uint32_t m1 = 1, m2 = 1, m3 = 1, m4 = 1;
            const bool scale = wc::ParseScale("30", m1) && m1 == 30000 && wc::ParseScale("0", m2) && m2 == 0 && wc::ParseScale("1.5", m3) &&
                               m3 == 1500 && wc::ParseScale("3600", m4) && m4 == 3600000;
            uint32_t y = 0;
            const bool scaleRefused = !wc::ParseScale("3600.1", y) && !wc::ParseScale("-1", y) && !wc::ParseScale("", y) && !wc::ParseScale("1e3", y) &&
                                      !wc::ParseScale("abc", y) && !wc::ParseScale("30x", y);
            Check("world clock: 'time' takes H:MM, HH:MM, HH:MM:SS and refuses 13 malformed or out-of-range forms; 'timescale' takes 0..3600 "
                  "(decimals) and refuses the rest",
                  ok && refusedOk == 13 && scale && scaleRefused, F("refused %d of 13", refusedOk));
        }
        {
            // the line: GTA's rate (1 game minute per 2 real s), frozen, the midnight wrap, a new scale keeps the time shown
            wc::Clock k;
            k.SetTime(1000, 23LL * 3600000 + 59 * kMin);   // 23:59:00 at real 1000 ms
            const bool rate = k.GameMsAt(1000 + 1000) == 23LL * 3600000 + 59 * kMin + 30000 && wc::FormatHms(k.GameMsAt(1000 + 2000)) == "00:00:00" &&
                              wc::FormatHms(k.GameMsAt(1000 + 4000)) == "00:01:00";
            k.SetScale(1000 + 4000, 0);
            const bool frozen = wc::FormatHms(k.GameMsAt(5000)) == "00:01:00" && wc::FormatHms(k.GameMsAt(5000 + 600000)) == "00:01:00";
            k.SetScale(605000, 60000);   // twice GTA's rate from now: the time shown does not move
            const bool rescale = wc::FormatHms(k.GameMsAt(605000)) == "00:01:00" && wc::FormatHms(k.GameMsAt(606000)) == "00:02:00";
            const bool diff = wc::DiffSec(wc::ToHms(86399000), wc::ToHms(1000)) == -2 && wc::DiffSec(wc::ToHms(1000), wc::ToHms(86399000)) == 2 &&
                              wc::DiffSec(wc::ToHms(43200000), wc::ToHms(0)) == -43200;
            Check("world clock: the line runs at GTA's rate (2 real s = 1 game minute), wraps at midnight, stands still at scale 0, and a new scale "
                  "re-anchors without a jump; DiffSec wraps across midnight",
                  rate && frozen && rescale && diff, F("%s", wc::FormatHms(k.GameMsAt(606000)).c_str()));
        }
        {
            // a fixed weather: steady, then an admin's change blends from what is shown over the transition, then steady
            wc::WeatherPlan p;
            p.Configure(wc::kRain, {}, 45000, 0);
            const wc::WeatherView a = p.At(1000);
            p.SetFixed(10000, wc::kClear);
            const wc::WeatherView b = p.At(10000 + 22500), c = p.At(10000 + 45000);
            // a change in the middle of a blend starts from the blend's dominant side (CLEAR -> RAIN at 60 %: RAIN)
            p.SetFixed(100000, wc::kRain);
            p.SetFixed(100000 + 27000, wc::kThunder);
            const wc::WeatherView d = p.At(100000 + 27000);
            Check("world clock: a fixed weather is steady; an admin's change blends from it (RAIN -> CLEAR 50 % at 22.5 of 45 s), steady at the end; "
                  "a change mid-blend starts from the dominant side",
                  a.Steady() && a.to == wc::kRain && !b.Steady() && b.from == wc::kRain && b.to == wc::kClear && b.Progress() == 0.5f &&
                      c.Steady() && c.to == wc::kClear && !d.Steady() && d.from == wc::kRain && d.to == wc::kThunder && d.elapsedMs == 0,
                  F("d %s -> %s", wc::WeatherName(d.from), wc::WeatherName(d.to)));
        }
        {
            // the cycle: entry 0 steady at the start, each next entry blends in over its first transMs, the lap wraps from the last entry
            wc::WeatherPlan p;
            p.Configure(-1, {}, 45000, 0);
            const wc::WeatherView s = p.At(5 * kMin);
            const wc::WeatherView t = p.At(20 * kMin + 10000), u = p.At(20 * kMin + 46000);
            const wc::WeatherView lap = p.At(90 * kMin + 1000);
            const bool phase = s.SamePhase(p.At(19 * kMin)) && !s.SamePhase(t) && !t.SamePhase(u) && t.SamePhase(p.At(20 * kMin + 40000));
            Check("world clock: the cycle starts steady on EXTRASUNNY, turns EXTRASUNNY -> CLEAR over the first 45 s of minute 20, is steady CLEAR "
                  "after, and the next lap blends CLEAR -> EXTRASUNNY; the phase changes only at those edges",
                  p.Cycling() && s.Steady() && s.to == wc::kExtraSunny && !t.Steady() && t.from == wc::kExtraSunny && t.to == wc::kClear &&
                      t.elapsedMs == 10000 && t.totalMs == 45000 && u.Steady() && u.to == wc::kClear && !lap.Steady() && lap.from == wc::kClear &&
                      lap.to == wc::kExtraSunny && phase && p.CycleLapMs() == 90 * kMin);
        }
        {
            // back to the cycle from a fixed weather: from entry 0, blending from the fixed weather
            wc::WeatherPlan p;
            p.Configure(wc::kFoggy, {{wc::kClouds, 10}, {wc::kRain, 5}}, 30000, 0);
            p.SetCycle(60000);
            const wc::WeatherView a = p.At(60000 + 15000), b = p.At(60000 + 31000), c = p.At(60000 + 10 * kMin + 1000);
            Check("world clock: 'weather cycle' after a fixed FOGGY restarts the server's own cycle at its first entry, blending FOGGY -> CLOUDS",
                  p.Cycling() && !a.Steady() && a.from == wc::kFoggy && a.to == wc::kClouds && b.Steady() && b.to == wc::kClouds && !c.Steady() &&
                      c.from == wc::kClouds && c.to == wc::kRain);
        }
        {
            // the client's copy: two machines that receive the same server moment over different round trips show the same game second (the
            // 0.5.6 bug: every machine ran its own clock); without the half-round-trip placement they differ by (130 ms) x 30 = 3.9 game s
            const int64_t serverSendLocalA = 5000, serverSendLocalB = 90000;   // the same real moment on two machines' own clocks
            const uint32_t gameAtSend = 18 * 3600000;
            const wc::Received a = wc::FromWire(gameAtSend, 30000, 1, 1, 0, 0, 7, 0, serverSendLocalA + 20, 40);
            const wc::Received b = wc::FromWire(gameAtSend, 30000, 1, 1, 0, 0, 7, 0, serverSendLocalB + 150, 300);
            const int64_t ga = wc::GameMsAt(a, serverSendLocalA + 10000), gb = wc::GameMsAt(b, serverSendLocalB + 10000);
            const int64_t d = ga > gb ? ga - gb : gb - ga;
            // a joiner in the middle of a blend starts at the server's progress and finishes when everybody does
            const wc::Received j = wc::FromWire(gameAtSend, 30000, wc::kClouds, wc::kRain, 12000, 45000, 9, proto::kClockCycle, 1000, 0);
            const wc::WeatherView j1 = wc::WeatherAt(j, 1000 + 10500), j2 = wc::WeatherAt(j, 1000 + 33000);
            Check("world clock: two clients with 40 ms and 300 ms round trips show the same game time within 1 game s; a joiner mid-blend is at the "
                  "server's progress (22.5 of 45 s after 10.5 s) and steady on RAIN when the blend ends",
                  d <= 1000 && !j1.Steady() && j1.elapsedMs == 22500 && j1.from == wc::kClouds && j2.Steady() && j2.to == wc::kRain,
                  F("diff %lld game ms", (long long)d));
        }
        {
            // ClockHold: the first tick pauses and sets; a new second sets; the read-back every second; more than 2 s off is a mismatch
            wc::ClockHold h;
            wc::Hms t;
            t.h = 18;
            const wc::ClockHold::Plan p0 = h.Next(0, t);
            h.Done(0, p0, true, true);
            const wc::ClockHold::Plan p1 = h.Next(16, t);                          // same second, nothing due
            wc::Hms t2 = t;
            t2.s = 1;
            const wc::ClockHold::Plan p2 = h.Next(33, t2);                         // a new second: set
            h.Done(33, p2, true, true);
            const wc::ClockHold::Plan p3 = h.Next(1000, t2);                       // a read-back is due
            const bool within = !h.OnReadBack(1000, true, t);                      // 1 s off: within tolerance
            wc::Hms off = t2;
            off.m = 1;                                                             // 60 s ahead: the game clock ran (unpaused)
            const bool mis = h.OnReadBack(2000, true, off);
            const bool failedRead = !h.OnReadBack(3000, false, off);               // the natives did not answer: not a mismatch
            const wc::ClockHold::Plan p4 = h.Next(5000, t2);                       // the re-pause is due
            Check("world clock: ClockHold pauses + sets on the first tick, sets only on a new second, reads back every second, counts 60 s off as a "
                  "mismatch (1 s is within tolerance), re-pauses every 5 s",
                  p0.pause && p0.set && !p0.readBack && !p1.set && !p1.pause && !p1.readBack && p2.set && !p2.pause && p3.readBack && !p3.set &&
                      within && mis && failedRead && h.Mismatches() == 1 && p4.pause,
                  F("mismatches %u", h.Mismatches()));
        }
        {
            // WeatherHold: steady = one persist; a blend = mix calls every 500 ms or 1 %; read-back verdicts; Force after a mismatch
            wc::WeatherHold h;
            wc::WeatherView steady;
            steady.from = steady.to = wc::kRain;
            const wc::WeatherHold::Plan a = h.Next(0, steady);
            h.Done(0, a, true);
            const wc::WeatherHold::Plan b = h.Next(100, steady);
            const bool persistOk = h.Matches(wc::WeatherHash(wc::kRain), wc::WeatherHash(wc::kRain), 0.3f) &&
                                   h.Matches(wc::WeatherHash(wc::kClear), wc::WeatherHash(wc::kRain), 1.0f) &&
                                   !h.Matches(wc::WeatherHash(wc::kClear), wc::WeatherHash(wc::kClear), 0.0f);
            wc::WeatherView blend;
            blend.from = wc::kRain;
            blend.to = wc::kClear;
            blend.totalMs = 45000;
            blend.elapsedMs = 9000;   // 20 %
            const wc::WeatherHold::Plan c = h.Next(200, blend);
            h.Done(200, c, true);
            blend.elapsedMs = 9200;   // 20.4 %: under the step, inside 500 ms
            const wc::WeatherHold::Plan d = h.Next(400, blend);
            blend.elapsedMs = 9500;   // 21.1 %: a step
            const wc::WeatherHold::Plan e = h.Next(450, blend);
            blend.elapsedMs = 9300;
            const wc::WeatherHold::Plan f = h.Next(700, blend);   // 500 ms since the last issue
            const bool mixOk = h.Matches(wc::WeatherHash(wc::kRain), wc::WeatherHash(wc::kClear), 0.25f) &&
                               !h.Matches(wc::WeatherHash(wc::kRain), wc::WeatherHash(wc::kClear), 0.5f) &&
                               !h.Matches(wc::WeatherHash(wc::kClouds), wc::WeatherHash(wc::kClear), 0.2f);
            const bool mis = h.OnReadBack(2200, true, wc::WeatherHash(wc::kExtraSunny), wc::WeatherHash(wc::kExtraSunny), 0.0f);
            const wc::WeatherHold::Plan g = h.Force(blend);
            const wc::WeatherHold::Plan r = h.Next(4300, blend);
            Check("world clock: WeatherHold persists a steady weather once, mixes a blend again on a 1 % step or after 500 ms, accepts a persist "
                  "read back as (w, w) or a finished blend into w, flags a foreign weather and forces the target",
                  a.kind == wc::WeatherHold::Persist && a.a == wc::kRain && b.kind == wc::WeatherHold::None && persistOk &&
                      c.kind == wc::WeatherHold::Mix && c.a == wc::kRain && c.b == wc::kClear && d.kind == wc::WeatherHold::None &&
                      e.kind == wc::WeatherHold::Mix && f.kind == wc::WeatherHold::Mix && mixOk && mis && h.Mismatches() == 1 &&
                      g.kind == wc::WeatherHold::Mix && g.a == wc::kRain && r.readBack);
        }
        {
            // the codec: 22 bytes, a round trip
            proto::WorldClock m;
            m.rev = 3;
            m.gameMs = 66615000;
            m.scaleMilli = 30000;
            m.weatherFrom = wc::kClouds;
            m.weatherTo = wc::kRain;
            m.transElapsedMs = 12000;
            m.transTotalMs = 45000;
            m.flags = proto::kClockCycle;
            const proto::Bytes b = proto::Encode(m);
            proto::WorldClock o;
            proto::Msg t{};
            const bool ok = b.size() == proto::kWorldClockBytes && b.size() == 22 && b[0] == 55 && proto::PeekType(b.data(), b.size(), t) &&
                            t == proto::Msg::WorldClock && proto::Decode(b.data(), b.size(), o) && o.rev == 3 && o.gameMs == 66615000 &&
                            o.scaleMilli == 30000 && o.weatherFrom == wc::kClouds && o.weatherTo == wc::kRain && o.transElapsedMs == 12000 &&
                            o.transTotalMs == 45000 && o.flags == proto::kClockCycle && proto::kMsgLast == (uint8_t)proto::Msg::WorldClock;
            Check("world clock: WorldClock (55) is 22 bytes and round-trips every field; it is the protocol's last message type", ok,
                  F("%d bytes", (int)b.size()));
        }
        {
            // refusals: Encode of an invalid message is empty, Decode of a malformed one fails
            auto refused = [](const proto::WorldClock& m) { return proto::Encode(m).empty(); };
            proto::WorldClock base;
            base.gameMs = 1000;
            proto::WorldClock m1 = base, m2 = base, m3 = base, m4 = base, m5 = base, m6 = base;
            m1.gameMs = 86400000;               // a day is out of range
            m2.scaleMilli = 3600001;            // past the fastest
            m3.weatherFrom = 1;                 // a blend without a length
            m3.weatherTo = 2;
            m4.transTotalMs = 1000;             // elapsed past the total
            m4.transElapsedMs = 1001;
            m5.flags = 0x80;                    // a flag nobody defined
            m6.weatherTo = m6.weatherFrom = 32; // past the id range
            const bool enc = refused(m1) && refused(m2) && refused(m3) && refused(m4) && refused(m5) && refused(m6);
            proto::Bytes good = proto::Encode(base);
            proto::WorldClock o;
            proto::Bytes trunc(good.begin(), good.end() - 1), longer = good;
            longer.push_back(0);
            proto::Bytes badDay = good;
            badDay[3] = badDay[4] = badDay[5] = badDay[6] = 0xff;   // gameMs (after the type and the u16 rev) = 0xffffffff
            proto::Bytes badFlag = good;
            badFlag[21] = 0x02;
            const bool dec = !good.empty() && proto::Decode(good.data(), good.size(), o) && !proto::Decode(trunc.data(), trunc.size(), o) &&
                             !proto::Decode(longer.data(), longer.size(), o) && !proto::Decode(badDay.data(), badDay.size(), o) &&
                             !proto::Decode(badFlag.data(), badFlag.size(), o);
            Check("world clock: Encode refuses 6 invalid messages (day overflow, scale, blend without a length, elapsed > total, unknown flag, id "
                  "32); Decode refuses a short, a long, an out-of-range time and an unknown flag",
                  enc && dec);
        }
        {
            // the server's view of a fixed weather + clock as it goes on the wire (Server::WorldClockNow's mapping): steady -> from == to, 0 / 0
            wc::WeatherPlan p;
            p.Configure(wc::kOvercast, {}, 45000, 0);
            const wc::WeatherView v = p.At(100);
            proto::WorldClock m;
            m.gameMs = 1000;
            m.weatherTo = v.to;
            m.weatherFrom = v.Steady() ? v.to : v.from;
            const proto::Bytes b = proto::Encode(m);
            proto::WorldClock o;
            const wc::Received r = proto::Decode(b.data(), b.size(), o)
                                       ? wc::FromWire(o.gameMs, o.scaleMilli, o.weatherFrom, o.weatherTo, o.transElapsedMs, o.transTotalMs, o.rev,
                                                      o.flags, 0, 0)
                                       : wc::Received();
            Check("world clock: a steady weather goes out as from == to with no blend and comes back steady on the client",
                  r.have && wc::WeatherAt(r, 5000).Steady() && wc::WeatherAt(r, 5000).to == wc::kOvercast);
        }
    }

}  // namespace worldclock_t
