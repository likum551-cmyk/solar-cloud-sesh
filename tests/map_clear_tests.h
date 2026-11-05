// 0.4.20 map clean-up (libs/sync/mapclear.h): which blips the sweep hides, when it sweeps, the per-sprite tally.
// Included by tests/gameplay_test.cpp after its Check(); run by mapclear::RunPure(), kChecks checks.
#pragma once

#include "sync/mapclear.h"

#include <string>
#include <vector>

namespace mapclear {

    constexpr int kChecks = 19;

    struct FakeBlip
    {
        int32_t handle;
        int sprite;
        int display;
    };

    inline void RunPure()
    {
        using namespace gage::sync;
        std::printf("-- 0.4.20 map clean-up: blip verdicts, sweep plan, schedule, waypoint, census, tally\n");

        // The keep set as a live 3889 sweep would read it: arrow 0x1001, north 0x1002, waypoint sprite 8.
        BlipKeepSet keep;
        keep.mainPlayer = 0x1001;
        keep.north = 0x1002;
        keep.waypointSprite = 8;
        OwnedBlips owned;

        // 1. an SP shop blip (sprite 52, handle unknown to us) is hidden.
        Check("map: an SP blip is hidden", ClassifyBlip(0x2001, 52, keep, owned) == BlipVerdict::Hide);
        // 2-3. the player arrow and the north blip are kept by HANDLE whatever their sprite.
        Check("map: the main player blip is kept", ClassifyBlip(0x1001, 6, keep, owned) == BlipVerdict::KeepPlayer);
        Check("map: the north blip is kept", ClassifyBlip(0x1002, 52, keep, owned) == BlipVerdict::KeepNorth);
        // 4. a waypoint the player sets (the enum's sprite) survives the sweep; the sprite comes from the keep set,
        //    not a constant: with the enum reading 9, a sprite-8 blip is an ordinary blip.
        {
            BlipKeepSet k9 = keep;
            k9.waypointSprite = 9;
            Check("map: the waypoint sprite (from the game's enum) is kept, sprite 8 is not special when the enum says 9",
                  ClassifyBlip(0x3001, 8, keep, owned) == BlipVerdict::KeepWaypoint && ClassifyBlip(0x3001, 9, k9, owned) == BlipVerdict::KeepWaypoint
                      && ClassifyBlip(0x3001, 8, k9, owned) == BlipVerdict::Hide);
        }
        // 5. our own blip is kept while owned and hidden once the engine gave it up.
        {
            OwnedBlips mine;
            mine.Add(0x4001);
            const bool keptWhileOwned = ClassifyBlip(0x4001, 1, keep, mine) == BlipVerdict::KeepOwned;
            mine.Remove(0x4001);
            Check("map: an engine-owned blip is kept, and hidden after Remove", keptWhileOwned && ClassifyBlip(0x4001, 1, keep, mine) == BlipVerdict::Hide);
        }
        // 6. OwnedBlips ignores 0 and duplicates.
        {
            OwnedBlips mine;
            mine.Add(0);
            mine.Add(7);
            mine.Add(7);
            Check("map: OwnedBlips ignores handle 0 and duplicates", mine.Size() == 1 && !mine.Has(0) && mine.Has(7));
        }
        // 7. display ids 0 / 1 / 7 never show (alloc8or); 2..6, 8, 10 and an unreadable -1 do.
        Check("map: display 0 / 1 / 7 read as hidden, 2-6 / 8 / 10 / -1 as shown",
              !BlipDisplayShows(0) && !BlipDisplayShows(1) && !BlipDisplayShows(7) && BlipDisplayShows(2) && BlipDisplayShows(3)
                  && BlipDisplayShows(4) && BlipDisplayShows(5) && BlipDisplayShows(6) && BlipDisplayShows(8) && BlipDisplayShows(10)
                  && BlipDisplayShows(-1));
        // 8. the waypoint sprite: the enum when it reads sane, else 8.
        Check("map: waypoint sprite = the enum when readable and in range, else 8",
              WaypointSpriteFrom(9, true) == 9 && WaypointSpriteFrom(9, false) == 8 && WaypointSpriteFrom(0, true) == 8
                  && WaypointSpriteFrom(5000, true) == 8);

        // 9-11. a sweep plan over one walk: arrow, north, waypoint, 3 SP blips (one already hidden), our own blip, a 0 handle.
        {
            OwnedBlips mine;
            mine.Add(0x4001);
            const std::vector<FakeBlip> walk = {
                { 0x1001, 6, 2 }, { 0x1002, 7, 2 }, { 0x3001, 8, 2 },     // arrow, north, waypoint
                { 0x2001, 52, 2 }, { 0x2002, 52, 4 }, { 0x2003, 110, 0 },  // SP: two showing, one already hidden
                { 0x4001, 1, 2 }, { 0, 52, 2 },                            // ours; a 0 handle (ignored)
            };
            BlipTally t;
            const MapSweepPlan plan = PlanMapSweep(walk, keep, mine, t);
            Check("map: the plan hides exactly the two showing SP blips",
                  plan.hide.size() == 2 && plan.hide[0].handle == 0x2001 && plan.hide[1].handle == 0x2002 && plan.hide[0].sprite == 52,
                  std::to_string(plan.hide.size()));
            Check("map: the plan keeps arrow / north / waypoint / own once each and skips the already-hidden one",
                  plan.keptPlayer == 1 && plan.keptNorth == 1 && plan.keptWaypoint == 1 && plan.keptOwned == 1 && plan.already == 1);
            const BlipTally::Row& tot = t.Totals();
            Check("map: the plan's tally: 7 seen (the 0 handle not), 4 kept, 1 already, nothing hidden before the read-back",
                  tot.seen == 7 && tot.kept == 4 && tot.already == 1 && tot.hidden == 0 && tot.refused == 0, std::to_string(tot.seen));
        }

        // 12-17. the schedule.
        {
            MapClearConfig cfg;   // 3000 ms sweep, 10000 ms log, 3 census sweeps
            MapClearSchedule s(cfg);
            const bool beforeEntry = s.SweepDue(0) || s.SweepDue(50000) || s.LogDue(50000);
            s.OnEntry(100000);
            const bool atEntry = s.SweepDue(100000);
            const bool tooSoon = s.SweepDue(102999);
            const bool onTime = s.SweepDue(103000);
            Check("map: no sweep before the entry, one at the entry, the next 3 s later (not before)",
                  !beforeEntry && atEntry && !tooSoon && onTime);
            // A late tick (the game stalled 6 s) sweeps once, not twice: no catch-up burst.
            const bool late = s.SweepDue(109000);
            const bool again = s.SweepDue(109001);
            Check("map: a late tick sweeps once, the next is 3 s after it", late && !again && !s.SweepDue(111999) && s.SweepDue(112000));
            Check("map: the 10 s line is due 10 s after the entry, then every 10 s",
                  !s.LogDue(109999) && s.LogDue(110000) && !s.LogDue(119999) && s.LogDue(120000));
            // The stale waypoint: once per entry.
            const bool wp1 = s.TakeWaypointClear(), wp2 = s.TakeWaypointClear();
            s.OnEntry(200000);
            const bool wp3 = s.TakeWaypointClear();
            Check("map: the stale waypoint is cleared once per entry", wp1 && !wp2 && wp3);
            // Census: the first 3 sweeps of every entry.
            int census = 0;
            for (int i = 0; i < 5; ++i)
                census += s.TakeCensus() ? 1 : 0;
            s.OnEntry(300000);
            const bool reArmed = s.TakeCensus();
            Check("map: 3 census sweeps per entry", census == 3 && reArmed, std::to_string(census));
            s.Reset();
            Check("map: no sweep, waypoint clear or log after Reset", !s.SweepDue(400000) && !s.TakeWaypointClear() && !s.LogDue(400000));
        }

        // 18-19. the tally's text and merge.
        {
            BlipTally a;
            a.Seen(52); a.Hidden(52);
            a.Seen(52); a.Refused(52);
            a.Seen(110); a.Already(110);
            a.Seen(3); a.Kept(3);
            const std::string full = a.Format(8);
            const std::string cut = a.Format(1);
            Check("map: tally text: most-seen first, ties by sprite id, '+N more' when cut, 'none' when empty",
                  full == "s52 seen 2 hid 1 REFUSED 1, s3 seen 1 kept 1, s110 seen 1 already 1" && cut == "s52 seen 2 hid 1 REFUSED 1, +2 more"
                      && BlipTally().Format(4) == "none",
                  full + " | " + cut);
            BlipTally w;
            w.Merge(a);
            w.Merge(a);
            const BlipTally::Row& tot = w.Totals();
            Check("map: Merge adds rows and totals", tot.seen == 8 && tot.hidden == 2 && tot.refused == 2 && tot.already == 2 && tot.kept == 2
                                                      && w.Sprites() == 3 && w.Format(1) == "s52 seen 4 hid 2 REFUSED 2, +2 more",
                  w.Format(3));
        }
    }

} // namespace mapclear
