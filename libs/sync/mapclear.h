// 0.4.20 map clean-up (GI: "clear the map from all waypoints, and reveal all of it for everyone"): which
// blips the engine hides from the pause map / radar, when it sweeps, and the per-sprite tally the live log
// prints. Pure C++17, single-threaded (the engine's script tick), tested by tests/gameplay_test.cpp
// (tests/map_clear_tests.h). The natives live in apps/engine/hook.cpp (World::CollectBlips / HideBlip /
// ...); this header only decides.
//
// Why a sweep and not the entry one-shot (World::ClearSpBlips): the SP blips are made by stock scripts
// (blip_controller, shop_controller, friends_controller, ...). With stock scripts OFF none exists at the
// entry (gage.log "found 0 blip(s)"), but the GtaThread::Tick no-op is not always installed (gage.log
// 0.4.18: "[nosp] waiting for GtaThread::Tick to decrypt" past 60 s, then blip_controller live after the
// entry), and then the scripts create their blips AFTER the one-shot ran. The old periodic call removed
// with REMOVE_BLIP, which refuses a blip another script created and logged only its first run.
//
// Rules (facts: alloc8or natives.json HUD::SET_BLIP_DISPLAY comment, display ids 0 / 1 / 7 "doesn't show
// up, ever, anywhere"; GET_MAIN_PLAYER_BLIP_ID, GET_NORTH_BLID_INDEX, GET_WAYPOINT_BLIP_ENUM_ID):
//   * kept: a blip the engine created itself (OwnedBlips; none today), the main player blip (the arrow),
//     the north blip, and any blip with the waypoint sprite (a NEW waypoint the player sets stays; the
//     stale one is cleared once per entry with SET_WAYPOINT_OFF, not by the sweep);
//   * every other blip is hidden with SET_BLIP_DISPLAY(blip, 0) (never REMOVE_BLIP: a hidden blip keeps
//     its owner script's handle valid, so a running script does not re-create it every sweep, and it
//     needs no ownership); a blip whose display already reads hidden is left alone;
//   * the sweep runs at the entry and then every sweepEveryMs (a low rate: ~1000 natives a walk), the
//     first censusSweeps sweeps each log a per-sprite census, and a 10 s counter line follows.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gage::sync {

    constexpr int kBlipSpriteFirst = 1;          // sprite ids walked with GET_FIRST / GET_NEXT_BLIP_INFO_ID
    constexpr int kBlipSpriteLast = 1000;        // a generous cap on the BLIP_SPRITE range (same as ClearSpBlips)
    constexpr int kWaypointSpriteDefault = 8;    // the waypoint's sprite when GET_WAYPOINT_BLIP_ENUM_ID cannot be read
    constexpr int kBlipDisplayHidden = 0;        // SET_BLIP_DISPLAY id: "doesn't show up, ever, anywhere"

    // What the sweep does with one blip.
    enum class BlipVerdict
    {
        Invalid,       // handle 0: nothing to do
        KeepOwned,     // the engine created it
        KeepPlayer,    // GET_MAIN_PLAYER_BLIP_ID: the local player arrow
        KeepNorth,     // GET_NORTH_BLID_INDEX
        KeepWaypoint,  // the player's own waypoint (a new one must survive the sweep)
        Hide,          // an SP / stock-script / leftover blip
    };

    inline const char* BlipVerdictName(BlipVerdict v)
    {
        switch (v)
        {
        case BlipVerdict::Invalid: return "invalid";
        case BlipVerdict::KeepOwned: return "owned";
        case BlipVerdict::KeepPlayer: return "player";
        case BlipVerdict::KeepNorth: return "north";
        case BlipVerdict::KeepWaypoint: return "waypoint";
        case BlipVerdict::Hide: return "hide";
        }
        return "?";
    }

    // The blips the engine made itself. Every ADD_BLIP_FOR_* site in the engine must Add its handle here and
    // Remove it when it removes the blip, or the sweep hides it like an SP blip.
    class OwnedBlips
    {
    public:
        void Add(int32_t h)
        {
            if (h && !Has(h))
                h_.push_back(h);
        }
        void Remove(int32_t h) { h_.erase(std::remove(h_.begin(), h_.end(), h), h_.end()); }
        bool Has(int32_t h) const { return h && std::find(h_.begin(), h_.end(), h) != h_.end(); }
        size_t Size() const { return h_.size(); }
        void Clear() { h_.clear(); }

    private:
        std::vector<int32_t> h_;
    };

    // Read once per sweep from the game (0 = unknown / none).
    struct BlipKeepSet
    {
        int32_t mainPlayer = 0;                     // GET_MAIN_PLAYER_BLIP_ID
        int32_t north = 0;                          // GET_NORTH_BLID_INDEX
        int waypointSprite = kWaypointSpriteDefault;  // GET_WAYPOINT_BLIP_ENUM_ID (> 0), else the default
    };

    inline BlipVerdict ClassifyBlip(int32_t handle, int sprite, const BlipKeepSet& keep, const OwnedBlips& owned)
    {
        if (!handle)
            return BlipVerdict::Invalid;
        if (owned.Has(handle))
            return BlipVerdict::KeepOwned;
        if (keep.mainPlayer && handle == keep.mainPlayer)
            return BlipVerdict::KeepPlayer;
        if (keep.north && handle == keep.north)
            return BlipVerdict::KeepNorth;
        if (sprite == keep.waypointSprite)
            return BlipVerdict::KeepWaypoint;
        return BlipVerdict::Hide;
    }

    // GET_BLIP_INFO_ID_DISPLAY: 0, 1 and 7 never show (alloc8or); anything else (or an unreadable -1) does.
    inline bool BlipDisplayShows(int display) { return !(display == 0 || display == 1 || display == 7); }

    // The waypoint sprite to keep: the game's enum when it reads a sane value, else the default.
    inline int WaypointSpriteFrom(int enumId, bool readOk)
    {
        return (readOk && enumId >= kBlipSpriteFirst && enumId <= kBlipSpriteLast) ? enumId : kWaypointSpriteDefault;
    }

    struct MapClearConfig
    {
        double sweepEveryMs = 3000.0;   // the periodic sweep (2-5 s: SP scripts re-add their blips at their own pace)
        double logEveryMs = 10000.0;    // the counter line
        int censusSweeps = 3;           // sweeps that print a per-sprite census line
    };

    // When to sweep, clear the stale waypoint and log. Times are the engine's ms clock.
    class MapClearSchedule
    {
    public:
        explicit MapClearSchedule(const MapClearConfig& cfg = MapClearConfig()) : cfg_(cfg) {}

        const MapClearConfig& Config() const { return cfg_; }

        // The entry finished: sweep now, clear the stale waypoint once, census again.
        void OnEntry(double nowMs)
        {
            entered_ = true;
            nextSweepMs_ = nowMs;
            nextLogMs_ = nowMs + cfg_.logEveryMs;
            waypointPending_ = true;
            census_ = 0;
        }
        void Reset()
        {
            entered_ = false;
            waypointPending_ = false;
            census_ = 0;
        }
        bool Entered() const { return entered_; }

        // True at most once per sweepEveryMs, never before the entry. A late tick does not bunch sweeps.
        bool SweepDue(double nowMs)
        {
            if (!entered_ || nowMs < nextSweepMs_)
                return false;
            nextSweepMs_ = nowMs + cfg_.sweepEveryMs;
            return true;
        }
        // The stale-waypoint clear: once per entry, on the first sweep.
        bool TakeWaypointClear()
        {
            const bool due = waypointPending_;
            waypointPending_ = false;
            return due;
        }
        // Whether this sweep prints its per-sprite census (the first censusSweeps after the entry).
        bool TakeCensus()
        {
            if (census_ >= cfg_.censusSweeps)
                return false;
            ++census_;
            return true;
        }
        bool LogDue(double nowMs)
        {
            if (!entered_ || nowMs < nextLogMs_)
                return false;
            nextLogMs_ = nowMs + cfg_.logEveryMs;
            return true;
        }

    private:
        MapClearConfig cfg_;
        bool entered_ = false;
        bool waypointPending_ = false;
        int census_ = 0;
        double nextSweepMs_ = 0;
        double nextLogMs_ = 0;
    };

    // Per-sprite counts: seen (walked), hidden (SET_BLIP_DISPLAY issued and read back hidden), refused (issued,
    // still shows), already (read hidden before we touched it), kept.
    class BlipTally
    {
    public:
        struct Row
        {
            int sprite = 0;
            uint32_t seen = 0, hidden = 0, refused = 0, already = 0, kept = 0;
        };

        void Seen(int sprite) { ++At(sprite).seen; ++tot_.seen; }
        void Kept(int sprite) { ++At(sprite).kept; ++tot_.kept; }
        void Already(int sprite) { ++At(sprite).already; ++tot_.already; }
        void Hidden(int sprite) { ++At(sprite).hidden; ++tot_.hidden; }
        void Refused(int sprite) { ++At(sprite).refused; ++tot_.refused; }

        const Row& Totals() const { return tot_; }
        size_t Sprites() const { return rows_.size(); }
        void Merge(const BlipTally& o)
        {
            for (const Row& r : o.rows_)
            {
                Row& m = At(r.sprite);
                m.seen += r.seen;
                m.hidden += r.hidden;
                m.refused += r.refused;
                m.already += r.already;
                m.kept += r.kept;
            }
            tot_.seen += o.tot_.seen;
            tot_.hidden += o.tot_.hidden;
            tot_.refused += o.tot_.refused;
            tot_.already += o.tot_.already;
            tot_.kept += o.tot_.kept;
        }
        bool Empty() const { return rows_.empty(); }
        void Clear()
        {
            rows_.clear();
            tot_ = Row();
        }

        // "s52 seen 3 hid 3, s1 seen 2 kept 1 ..." for the maxRows sprites seen most (ties: lower id first),
        // then "+N more" when cut.
        std::string Format(size_t maxRows) const
        {
            std::vector<Row> v = rows_;
            std::sort(v.begin(), v.end(), [](const Row& a, const Row& b) {
                return a.seen != b.seen ? a.seen > b.seen : a.sprite < b.sprite;
            });
            std::string s;
            char buf[128];
            const size_t n = (std::min)(maxRows, v.size());   // parenthesised: windows.h may define min
            for (size_t i = 0; i < n; ++i)
            {
                const Row& r = v[i];
                std::snprintf(buf, sizeof(buf), "%ss%d seen %u", s.empty() ? "" : ", ", r.sprite, r.seen);
                s += buf;
                if (r.hidden) { std::snprintf(buf, sizeof(buf), " hid %u", r.hidden); s += buf; }
                if (r.refused) { std::snprintf(buf, sizeof(buf), " REFUSED %u", r.refused); s += buf; }
                if (r.already) { std::snprintf(buf, sizeof(buf), " already %u", r.already); s += buf; }
                if (r.kept) { std::snprintf(buf, sizeof(buf), " kept %u", r.kept); s += buf; }
            }
            if (v.size() > n)
            {
                std::snprintf(buf, sizeof(buf), "%s+%zu more", s.empty() ? "" : ", ", v.size() - n);
                s += buf;
            }
            return s.empty() ? std::string("none") : s;
        }

    private:
        Row& At(int sprite)
        {
            for (Row& r : rows_)
                if (r.sprite == sprite)
                    return r;
            rows_.push_back(Row());
            rows_.back().sprite = sprite;
            return rows_.back();
        }
        std::vector<Row> rows_;
        Row tot_;
    };

    // One sweep's decisions over the walked blips (Blip: any type with .handle, .sprite, .display, such as
    // game::World::MapBlip). `hide` lists the blips to SET_BLIP_DISPLAY(blip, 0); the engine records each
    // read-back as tally.Hidden / tally.Refused. Seen, kept and already-hidden are tallied here.
    struct MapSweepPlan
    {
        struct Target
        {
            int32_t handle = 0;
            int sprite = 0;
        };
        std::vector<Target> hide;
        uint32_t keptPlayer = 0, keptNorth = 0, keptWaypoint = 0, keptOwned = 0, already = 0;
    };

    template <class Blip>
    MapSweepPlan PlanMapSweep(const std::vector<Blip>& blips, const BlipKeepSet& keep, const OwnedBlips& owned, BlipTally& tally)
    {
        MapSweepPlan plan;
        for (const Blip& b : blips)
        {
            const BlipVerdict v = ClassifyBlip(b.handle, b.sprite, keep, owned);
            if (v == BlipVerdict::Invalid)
                continue;
            tally.Seen(b.sprite);
            if (v != BlipVerdict::Hide)
            {
                ++(v == BlipVerdict::KeepPlayer ? plan.keptPlayer : v == BlipVerdict::KeepNorth ? plan.keptNorth
                   : v == BlipVerdict::KeepWaypoint ? plan.keptWaypoint : plan.keptOwned);
                tally.Kept(b.sprite);
            }
            else if (!BlipDisplayShows(b.display))
            {
                ++plan.already;   // hidden already (by an earlier sweep, or its script): no native this time
                tally.Already(b.sprite);
            }
            else
            {
                MapSweepPlan::Target t;
                t.handle = b.handle;
                t.sprite = b.sprite;
                plan.hide.push_back(t);
            }
        }
        return plan;
    }

} // namespace gage::sync
