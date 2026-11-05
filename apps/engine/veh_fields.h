// veh_fields.h (0.4.30, veh_steer_field): where a CVehicle keeps its steering angle, and the game's handle -> pointer function.
// Pure C++17 over byte runs (pack_scan.h), so the engine resolves it on the live image and tests/pack_profile_test.cpp on the
// offline Enhanced dump with the SAME code.
//
// Why: a copy's steering went through SET_VEHICLE_STEER_BIAS, which adds to the PLAYER's steering input; a copy is driven by a
// script ped, so its wheels never turned (0.4.29 live log: "steering applied n=2603 (failed 0) min -1.00 max 1.00", the owner saw
// no wheel move). RAGECOOP, FiveM's SET_VEHICLE_STEERING_ANGLE and SHVDN write the CVehicle steering-angle float instead.
//
// Layout (SHVDN NativeMemory / the Enhanced exit sequence, which also clears +0x10 and +0xC): input = angle - 8, throttle = angle + 8,
// brake = angle + 0xC, handbrake byte = angle + 0x10. Only the angle is written; brake and handbrake are READ for the wheel-spin
// diagnostic (unproven offsets: read only).
//
// Every pattern must match EXACTLY once, the offset must lie in [kMinOff, kMaxOff], and an independent cross-check must agree;
// anything else leaves the feature off with the reason (the engine falls back to the 0.4.29 steer bias).
#pragma once

#include "pack_scan.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gage::game::vehfields {

namespace scan = gage::game::content::scan;

struct Profile
{
    const char* name;
    const char* htpSig;     // the handle -> pointer function (or a call to it)
    int htpCallAt;          // -1: the match IS the function; else an E8 rel32 call at match + htpCallAt
    const char* steerSig;   // an instruction that addresses the steering field
    int steerDispAt;        // where its disp32 sits, relative to the match (may be negative)
    int steerAdd;           // field = disp + steerAdd
    const char* steerPre;   // bytes that must precede the disp (nullptr = none), e.g. "C7 86" (mov dword [rsi+disp32], imm32)
    const char* checkSig;   // an independent instruction addressing the same field
    int checkDispAt;
    bool checkUnique;       // true: the check pattern must match once; false: at least one match must carry the same disp
};

// Legacy 3889: YimMenu b3889 handle_to_ptr; SHVDN / FiveM / RAGECOOP steering input (angle = input + 8); ProperSteeringFix's Legacy
// write of the angle as the cross-check (short pattern: any hit with the same disp). Verifiable only at runtime (the exe is packed).
inline constexpr Profile kLegacy = { "Legacy 3889", "83 F9 FF 74 37 8B D1", -1, "74 0A F3 0F 11 B3 ?? ?? ?? ?? EB 25", 6, 8, nullptr,
                                     "44 89 BB ?? ?? ?? ?? 8B 0D", 3, false };
// Enhanced (the friend's dump): YimMenuV2's handle_to_ptr call site (E8 at +0x21, target RVA 0x1494b0 in the dump); ProperSteeringFix's
// exit locator (mov dword [rsi+0x9DC], 0 ten bytes before the match) and its stationary locator as the cross-check (both 0x9DC).
inline constexpr Profile kEnhanced = { "Enhanced", "0F 1F 84 00 00 00 00 00 89 F8 0F 28 FE 41", 0x21,
                                       "31 C0 80 B9 ?? ?? ?? ?? ?? 0F 94 C0 48 8D 15 ?? ?? ?? ?? F3 0F 10 04 82", -8, 0, "C7 86",
                                       "83 F8 ?? 0F 84 ?? ?? ?? ?? 8B 87 ?? ?? ?? ?? 83 E0 ?? 0F 85", 26, true };

constexpr int32_t kMinOff = 0x800, kMaxOff = 0xC00;
constexpr int32_t kBrakeFromAngle = 0xC, kHandbrakeFromAngle = 0x10;

struct Result
{
    bool ok = false;
    const uint8_t* htp = nullptr;   // the function, inside the scanned run
    int32_t steerOff = 0;           // the steering angle's offset in CVehicle
    char why[200] = {};
};

namespace detail {
inline bool In(const scan::Run& r, const uint8_t* p, size_t n) { return p >= r.p && p + n <= r.p + r.n; }
inline int32_t Rd32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
}  // namespace detail

inline Result Resolve(const scan::Run& img, const Profile& pf)
{
    using detail::In;
    using detail::Rd32;
    Result r;
    const std::vector<scan::Run> runs = { img };
    const uint8_t* m = nullptr;
    int c = scan::CountPat(runs, pf.htpSig, &m);
    if (c != 1) { std::snprintf(r.why, sizeof r.why, "%s: handle->pointer pattern matched %d times (need 1)", pf.name, c); return r; }
    if (pf.htpCallAt >= 0)
    {
        const uint8_t* site = m + pf.htpCallAt;
        if (!In(img, site, 5) || site[0] != 0xE8) { std::snprintf(r.why, sizeof r.why, "%s: no E8 call at match+0x%x", pf.name, pf.htpCallAt); return r; }
        const uint8_t* t = site + 5 + Rd32(site + 1);
        if (!In(img, t, 16)) { std::snprintf(r.why, sizeof r.why, "%s: the handle->pointer call leaves the image", pf.name); return r; }
        r.htp = t;
    }
    else
    {
        // the match is called as a function: it must START one (after int3 / ret / nop padding, or 16-byte aligned), not sit mid-body
        const bool padded = m > img.p && (m[-1] == 0xCC || m[-1] == 0xC3 || m[-1] == 0x90);
        if (!padded && ((size_t)(m - img.p) % 16) != 0)
        {
            std::snprintf(r.why, sizeof r.why, "%s: the handle->pointer match is not a function start (byte before 0x%02x, unaligned)", pf.name, m > img.p ? m[-1] : 0);
            return r;
        }
        r.htp = m;
    }

    c = scan::CountPat(runs, pf.steerSig, &m);
    if (c != 1) { std::snprintf(r.why, sizeof r.why, "%s: steering pattern matched %d times (need 1)", pf.name, c); return r; }
    const uint8_t* d = m + pf.steerDispAt;
    if (!In(img, d - 2, 6)) { std::snprintf(r.why, sizeof r.why, "%s: steering disp outside the image", pf.name); return r; }
    if (pf.steerPre)
    {
        std::vector<uint8_t> pb, pm;
        if (!scan::ParsePat(pf.steerPre, pb, pm) || pb.size() > 8 || !In(img, d - pb.size(), pb.size()) ||
            std::memcmp(d - pb.size(), pb.data(), pb.size()) != 0)
        {
            std::snprintf(r.why, sizeof r.why, "%s: the bytes before the steering disp are not %s", pf.name, pf.steerPre);
            return r;
        }
    }
    const int32_t off = Rd32(d) + pf.steerAdd;
    if (off < kMinOff || off > kMaxOff) { std::snprintf(r.why, sizeof r.why, "%s: steering offset 0x%x outside 0x%x..0x%x", pf.name, (unsigned)off, (unsigned)kMinOff, (unsigned)kMaxOff); return r; }

    // the cross-check: an independent instruction addressing the same field
    const uint8_t* cm = nullptr;
    c = scan::CountPat(runs, pf.checkSig, &cm);
    bool agree = false;
    if (pf.checkUnique)
        agree = c == 1 && In(img, cm + pf.checkDispAt, 4) && Rd32(cm + pf.checkDispAt) == off;
    else if (c > 0)
    {
        // every hit, lowest first: any with the same disp
        std::vector<uint8_t> b, k;
        if (!scan::ParsePat(pf.checkSig, b, k) || !k[0]) { std::snprintf(r.why, sizeof r.why, "%s: the cross-check pattern must start with a fixed byte", pf.name); return r; }
        for (const uint8_t* p = img.p; p + b.size() <= img.p + img.n && !agree; ++p)
        {
            p = static_cast<const uint8_t*>(std::memchr(p, b[0], (size_t)(img.p + img.n - p)));
            if (!p || p + b.size() > img.p + img.n) break;
            bool ok = true;
            for (size_t i = 0; i < b.size() && ok; ++i)
                ok = !k[i] || p[i] == b[i];
            if (ok && In(img, p + pf.checkDispAt, 4) && Rd32(p + pf.checkDispAt) == off) agree = true;
        }
    }
    if (!agree) { std::snprintf(r.why, sizeof r.why, "%s: cross-check (%d hits) does not address 0x%x", pf.name, c, (unsigned)off); return r; }
    r.steerOff = off;
    r.ok = true;
    std::snprintf(r.why, sizeof r.why, "%s: steering angle at CVehicle+0x%x (cross-checked), handle->pointer resolved", pf.name, (unsigned)off);
    return r;
}

// A steering angle we may write: finite, within a wheel's lock (radians) and not subnormal. A field that reads otherwise is not the angle
// (the upper half of a user-mode pointer reads as a subnormal float near 0: never write into it).
inline bool PlausibleAngle(float a) { return std::isfinite(a) && a > -1.6f && a < 1.6f && std::fpclassify(a) != FP_SUBNORMAL; }

}  // namespace gage::game::vehfields
