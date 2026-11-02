// W10 (docs/sync-specs/live-0.4.2-fixes.md section W10): the player name tag above each remote ped's head. Pure C++17,
// header-only, no game calls, single-threaded (the engine's script tick). engine.cpp (TickNameTags) only gathers the
// inputs the natives give (head bone, projection, line of sight, camera distance) and pushes the result to the overlay
// (Overlay::SetNameTags); overlay_ui.cpp only draws it. Every rule here is exercised by gameplay_test (TestW10).
//
//   DistanceAlpha / DistanceScale   full alpha up to 25 m, linear to 0 at 40 m; the text scales 1.0 -> 0.76 between 8 m and 40 m
//   ProjectionUsable                the projected point must be a finite point on (or just off) the screen
//   LosCache                        the line-of-sight result of one peer: asked at 5 Hz, kept for a bounded time, never a guess
//   NameTagTrack                    one peer's tag across frames: the eased alpha (no flash), the last position, the LOS cache
//   FormatTag / TruncateName        "[id] Name", a long or hostile name cut on a code point, control characters gone
//   KeepNearest                     at most Config::maxTags tags, the nearest
//
// Decisions (the engine follows them; the review note docs/NAMETAGS-REVIEW.md lists what the PC reviewer checks):
//   - Dead peer (the puppet plays its death): no tag. Fades out in fadeOutMs.
//   - Peer seated in a vehicle: the tag stays, at the head bone like any other (the head is inside the car; the tag floats
//     above it). Occlusion is the game's answer to the same LOS test; if a car body turns out to hide its own occupants
//     (INFERRED not to: trace 17 is believed to be map + objects) the reviewer sees seated peers never tagged, and
//     nametags_los=<flags> (HKCU\Software\GAGE-MP) changes the trace without a rebuild.
//   - Out of range (beyond maxRangeM), off screen, behind the camera, not driven yet, gone: no tag.
//   - LOS result pending (never asked, or the peer just came into range): hidden until the first answer, which the engine
//     asks for in the same frame, so the wait is zero frames. Result stale (older than losStaleMs, the checks stopped): hidden
//     (fail closed: a broken or throttled native shows no tag, never a tag through a wall).
//   - The tag fades in over fadeInMs and out over fadeOutMs, so a ray that flips at a door frame blinks softly, never flashes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gage::sync {

    struct NameTagConfig
    {
        float maxRangeM = 40.0f;         // beyond this nothing is drawn (alpha 0)
        float fadeStartM = 25.0f;        // full alpha up to here, linear to 0 at maxRangeM
        float scaleNearM = 8.0f;         // full size up to here, linear to scaleFar at maxRangeM
        float scaleFar = 0.76f;          // the smallest size (of the HUD fonts) a tag is drawn at
        float headLiftM = 0.35f;         // the anchor is this far above the head bone
        float marginX = 0.04f, marginY = 0.03f;   // a projected point this far outside 0..1 still counts (the text has width)
        double losIntervalMs = 200.0;    // 5 Hz per peer
        double losStaleMs = 1000.0;      // a LOS result older than this is no result
        double fadeInMs = 160.0, fadeOutMs = 110.0;
        std::size_t maxTags = 12;        // tags drawn at most (the nearest)
        std::size_t maxCandidates = 16;  // peers the natives are asked about at most per frame (the nearest by pose distance)
        std::size_t maxNameCodePoints = 24;   // proto::kMaxName: a valid name is never cut
    };

    // ---- distance ----

    // 1 up to fadeStartM, linear down to 0 at maxRangeM, 0 beyond; NaN and negatives are 0 (no tag from a bad number).
    inline float DistanceAlpha(float distM, const NameTagConfig& c = NameTagConfig())
    {
        if (!(distM >= 0.0f)) return 0.0f;
        if (distM <= c.fadeStartM) return 1.0f;
        if (distM >= c.maxRangeM) return 0.0f;
        return (c.maxRangeM - distM) / (c.maxRangeM - c.fadeStartM);
    }

    // 1 up to scaleNearM, linear down to scaleFar at maxRangeM, scaleFar beyond and for a bad number: never above 1, never
    // below scaleFar, so a tag stays legible at the edge and does not balloon next to the camera.
    inline float DistanceScale(float distM, const NameTagConfig& c = NameTagConfig())
    {
        if (!(distM >= 0.0f)) return c.scaleFar;
        if (distM <= c.scaleNearM) return 1.0f;
        if (distM >= c.maxRangeM) return c.scaleFar;
        const float t = (distM - c.scaleNearM) / (c.maxRangeM - c.scaleNearM);
        return 1.0f + (c.scaleFar - 1.0f) * t;
    }

    // ---- projection ----

    // GET_SCREEN_COORD_FROM_WORLD_COORD answered `nativeOk` (false: behind the camera, off screen or not callable) with a
    // point sx / sy in 0..1. Usable when finite and within the screen plus a small margin (the tag is centred on x and has width).
    inline bool ProjectionUsable(bool nativeOk, float sx, float sy, const NameTagConfig& c = NameTagConfig())
    {
        if (!nativeOk || !std::isfinite(sx) || !std::isfinite(sy)) return false;
        return sx >= -c.marginX && sx <= 1.0f + c.marginX && sy >= -c.marginY && sy <= 1.0f + c.marginY;
    }

    // ---- line of sight ----

    // One peer's HAS_ENTITY_CLEAR_LOS_TO_ENTITY answer. Due() says when to ask again (every losIntervalMs, at once when
    // there is no answer yet or the clock went backwards), Record() stores the answer, Visible() is the answer while it is
    // fresh. Pending (no answer) and stale (older than losStaleMs) both read "not visible".
    class LosCache
    {
    public:
        bool Due(double nowMs, const NameTagConfig& c = NameTagConfig()) const
        {
            return !have_ || nowMs < atMs_ || nowMs - atMs_ >= c.losIntervalMs;
        }
        void Record(double nowMs, bool clear)
        {
            have_ = true;
            atMs_ = nowMs;
            clear_ = clear;
        }
        bool Visible(double nowMs, const NameTagConfig& c = NameTagConfig()) const
        {
            return have_ && nowMs >= atMs_ && nowMs - atMs_ <= c.losStaleMs && clear_;
        }
        bool Have() const { return have_; }
        void Reset() { have_ = false; clear_ = false; atMs_ = 0.0; }

    private:
        bool have_ = false, clear_ = false;
        double atMs_ = 0.0;
    };

    // ---- one peer's tag ----

    // What the engine learned this frame for one peer. Fields the natives could not fill stay at their defaults.
    struct TagFrame
    {
        double nowMs = 0.0;
        double dtMs = 0.0;           // since the last frame this peer was stepped (clamped to 0..100 inside)
        bool placed = false;         // a puppet ped exists and has been driven
        bool dead = false;           // the puppet plays its death
        bool projOk = false;         // GET_SCREEN_COORD_FROM_WORLD_COORD answered true ...
        float sx = 0.0f, sy = 0.0f;  // ... with this point (0..1) for the head anchor
        float distM = 1e9f;          // camera -> head anchor
    };

    struct TagOut
    {
        bool draw = false;
        float x = 0.0f, y = 0.0f;    // screen, 0..1, the tag's bottom centre
        float alpha = 0.0f;          // 0..1: the eased visibility times the distance fade
        float scale = 1.0f;
        float distM = 1e9f;          // camera -> head anchor as of the last frame the tag was wanted (the sort key)
    };

    class NameTagTrack
    {
    public:
        LosCache los;

        // The natives are worth calling this frame: the puppet is driven and alive, and within reach (distM from the pose; the
        // caller adds a margin for the pose-to-head difference).
        static bool Worth(bool placed, bool dead, float approxDistM, const NameTagConfig& c = NameTagConfig())
        {
            return placed && !dead && approxDistM >= 0.0f && approxDistM <= c.maxRangeM;
        }

        // One frame. Call it for every peer every frame (with placed / dead / projOk false when the natives were skipped):
        // the alpha eases toward 1 while the tag is wanted and toward 0 otherwise, and the last position is kept so the
        // fade-out stays where the tag was.
        TagOut Step(const TagFrame& f, const NameTagConfig& c = NameTagConfig())
        {
            const bool inRange = f.distM >= 0.0f && f.distM <= c.maxRangeM;
            const bool wanted = f.placed && !f.dead && inRange && ProjectionUsable(f.projOk, f.sx, f.sy, c) &&
                                los.Visible(f.nowMs, c);
            if (wanted)
            {
                x_ = f.sx;
                y_ = f.sy;
                dist_ = f.distM;
            }
            const double dt = f.dtMs < 0.0 ? 0.0 : (f.dtMs > 100.0 ? 100.0 : f.dtMs);
            const float step = (float)(dt / (wanted ? c.fadeInMs : c.fadeOutMs));
            alpha_ = wanted ? std::min(1.0f, alpha_ + step) : std::max(0.0f, alpha_ - step);
            TagOut o;
            const float fade = DistanceAlpha(dist_, c);
            o.alpha = alpha_ * fade;
            o.draw = o.alpha > 0.01f;
            o.x = x_;
            o.y = y_;
            o.scale = DistanceScale(dist_, c);
            o.distM = dist_;
            return o;
        }

        float Alpha() const { return alpha_; }
        void Reset() { los.Reset(); alpha_ = 0.0f; dist_ = 1e9f; }

    private:
        float alpha_ = 0.0f;
        float x_ = 0.0f, y_ = 0.0f;
        float dist_ = 1e9f;
    };

    // ---- text ----

    // Cuts `name` to at most maxCodePoints code points on a code point boundary (an overlong name ends in an ellipsis,
    // U+2026, within the budget), turns control characters into spaces, drops invalid UTF-8 sequences, collapses runs of
    // spaces and trims the ends. Empty after that: "Player".
    inline std::string TruncateName(const char* name, std::size_t maxCodePoints)
    {
        std::vector<std::string> cps;       // one entry per code point
        const unsigned char* p = (const unsigned char*)(name ? name : "");
        while (*p)
        {
            std::size_t n = 1;
            if (*p < 0x80) n = 1;
            else if (*p >= 0xC2 && *p <= 0xDF) n = 2;
            else if (*p >= 0xE0 && *p <= 0xEF) n = 3;
            else if (*p >= 0xF0 && *p <= 0xF4) n = 4;
            else { ++p; continue; }                          // a stray continuation byte or an invalid lead: dropped
            bool ok = true;
            for (std::size_t i = 1; i < n; ++i)
                if ((p[i] & 0xC0) != 0x80) { ok = false; break; }   // also stops at the NUL terminator
            if (!ok) { ++p; continue; }
            if (n == 1 || (n == 2 && p[0] == 0xC2 && p[1] < 0xA0))     // ASCII, or a C1 control (U+0080..U+009F)
            {
                const char ch = (n == 2 || *p < 0x20 || *p == 0x7F) ? ' ' : (char)*p;
                if (ch == ' ' && (cps.empty() || cps.back() == " ")) { ++p; continue; }   // no leading / doubled spaces
                cps.emplace_back(1, ch);
            }
            else
                cps.emplace_back((const char*)p, n);
            p += n;
        }
        while (!cps.empty() && cps.back() == " ") cps.pop_back();
        if (cps.empty()) return "Player";
        std::string out;
        if (maxCodePoints > 0 && cps.size() > maxCodePoints)
        {
            for (std::size_t i = 0; i + 1 < maxCodePoints; ++i) out += cps[i];
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += "\xE2\x80\xA6";
        }
        else
            for (const std::string& s : cps) out += s;
        return out;
    }

    struct TagText
    {
        std::string id;      // "[12]"
        std::string name;    // the cleaned, cut name
        std::string Full() const { return id + " " + name; }   // "[12] Name"
    };

    inline TagText FormatTag(int id, const char* name, const NameTagConfig& c = NameTagConfig())
    {
        TagText t;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "[%d]", id);
        t.id = buf;
        t.name = TruncateName(name, c.maxNameCodePoints);
        return t;
    }

    // ---- the list ----

    struct TagItem
    {
        int id = 0;
        float distM = 0.0f;
        TagOut out;
        TagText text;
    };

    // The nearest `cap` items, nearest first (ties by id, so the list is stable frame to frame).
    inline void KeepNearest(std::vector<TagItem>& v, std::size_t cap)
    {
        std::sort(v.begin(), v.end(), [](const TagItem& a, const TagItem& b) {
            return a.distM != b.distM ? a.distM < b.distM : a.id < b.id;
        });
        if (v.size() > cap) v.resize(cap);
    }

    // The same cut for the pre-native candidate list (a pose distance and an id).
    struct TagCandidate
    {
        int id = 0;
        float distM = 0.0f;
    };
    inline void KeepNearestCandidates(std::vector<TagCandidate>& v, std::size_t cap)
    {
        std::sort(v.begin(), v.end(), [](const TagCandidate& a, const TagCandidate& b) {
            return a.distM != b.distM ? a.distM < b.distM : a.id < b.id;
        });
        if (v.size() > cap) v.resize(cap);
    }

    // ---- the 10 s log line ----

    struct NameTagStats
    {
        uint32_t frames = 0;         // frames the tags ran
        uint32_t drawnSum = 0;       // tags handed to the overlay, summed over the frames
        uint32_t drawnMax = 0;
        uint32_t candidates = 0;     // peers the natives were asked about (summed)
        uint32_t losChecks = 0;      // HAS_ENTITY_CLEAR_LOS_TO_ENTITY calls
        uint32_t losHits = 0;        // frames a peer reused a fresh cached LOS answer instead of asking
        uint32_t losBlocked = 0;     // answers "blocked"
        uint32_t losFailed = 0;      // calls that could not be made
        uint32_t projFailed = 0;     // peers the projection said off screen / behind the camera
        uint32_t nativeFailed = 0;   // head bone / camera calls that could not be made
        void Reset() { *this = NameTagStats(); }
    };

}  // namespace gage::sync
