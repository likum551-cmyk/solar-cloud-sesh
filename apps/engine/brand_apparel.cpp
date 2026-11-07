// brand_apparel.cpp: see brand_apparel.h for the design. Built with -fno-exceptions like the rest of the engine.
#include "brand_apparel.h"

#include "common/logger.h"
#include "common/registry.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gage::game::brand {

namespace {

// ===========================================================================================================
// Data: the generated decals + fingerprints (apps/engine/brand_apparel_data.inc)
// ===========================================================================================================
enum Fmt : int { kFmtBC1 = 0, kFmtBC3 = 1 };

struct Decal {
    int x, y, w, h;          // rect at the base size (multiples of 16)
    const uint8_t* a8;       // w*h alpha
    uint8_t rgb[3];
};
struct Target {
    const char*  name;
    uint64_t     tailHash;
    Fmt          fmt;
    int          baseSize;   // texture width/height the decals are authored at
    int          levels;
    int          comp, drawable, texture;
    const Decal* decals;
    int          decalCount;
};

#include "brand_apparel_data.inc"

// A decal with its box-filtered pyramid (level k = base / 2^k, k <= 4).
constexpr int kMaxPyr = 5;
struct DecalRt {
    int x = 0, y = 0, w = 0, h = 0;
    uint8_t rgb[3] = { 255, 255, 255 };
    std::vector<uint8_t> lv[kMaxPyr];
};
struct TargetRt {
    std::string name;
    uint64_t    hash = 0;
    bool        bc3 = false;
    int         baseSize = 512;
    int         levels = 8;
    int         comp = 0, drawable = 0, texture = 0;
    std::vector<DecalRt> decals;
};

DecalRt BuildDecalRt(int x, int y, int w, int h, const uint8_t* a8, const uint8_t rgb[3]) {
    DecalRt d;
    d.x = x; d.y = y; d.w = w; d.h = h;
    d.rgb[0] = rgb[0]; d.rgb[1] = rgb[1]; d.rgb[2] = rgb[2];
    d.lv[0].assign(a8, a8 + (size_t)w * h);
    for (int k = 1; k < kMaxPyr; ++k) {
        const int pw = w >> (k - 1), ph = h >> (k - 1), nw = w >> k, nh = h >> k;
        if (nw <= 0 || nh <= 0) break;
        d.lv[k].resize((size_t)nw * nh);
        const std::vector<uint8_t>& p = d.lv[k - 1];
        for (int yy = 0; yy < nh; ++yy)
            for (int xx = 0; xx < nw; ++xx) {
                const int s = p[(size_t)(2 * yy) * pw + 2 * xx] + p[(size_t)(2 * yy) * pw + 2 * xx + 1] +
                              p[(size_t)(2 * yy + 1) * pw + 2 * xx] + p[(size_t)(2 * yy + 1) * pw + 2 * xx + 1];
                d.lv[k][(size_t)yy * nw + xx] = (uint8_t)((s + 2) / 4);
            }
        (void)ph;
    }
    return d;
}

std::vector<TargetRt> g_targets;
std::atomic<bool>     g_targetsReady{ false };
std::mutex            g_targetsMu;

void EnsureTargets() {
    if (g_targetsReady.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_targetsMu);
    if (g_targetsReady.load()) return;
    for (const Target& t : kTargets) {
        TargetRt r;
        r.name = t.name; r.hash = t.tailHash; r.bc3 = (t.fmt == kFmtBC3); r.baseSize = t.baseSize; r.levels = t.levels;
        r.comp = t.comp; r.drawable = t.drawable; r.texture = t.texture;
        for (int i = 0; i < t.decalCount; ++i)
            r.decals.push_back(BuildDecalRt(t.decals[i].x, t.decals[i].y, t.decals[i].w, t.decals[i].h, t.decals[i].a8, t.decals[i].rgb));
        g_targets.push_back(std::move(r));
    }
    g_targetsReady.store(true, std::memory_order_release);
}

// ===========================================================================================================
// BC1 / BC3 colour blocks: decode, encode, fingerprint
// ===========================================================================================================
inline float Clamp255(float v) { return v < 0.f ? 0.f : (v > 255.f ? 255.f : v); }

uint16_t Pack565(const float c[3]) {
    const int r = (int)(Clamp255(c[0]) * (31.0f / 255.0f) + 0.5f);
    const int g = (int)(Clamp255(c[1]) * (63.0f / 255.0f) + 0.5f);
    const int b = (int)(Clamp255(c[2]) * (31.0f / 255.0f) + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}
void Unpack565(uint16_t c, float o[3]) {
    const int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    o[0] = (float)((r5 << 3) | (r5 >> 2));
    o[1] = (float)((g6 << 2) | (g6 >> 4));
    o[2] = (float)((b5 << 3) | (b5 >> 2));
}

// Decode the 8 colour bytes of a BC1 block (or the colour half of a BC3 block, bc3 = always 4-colour mode).
void DecodeColour(const uint8_t* in, bool bc3, float px[16][3], bool* threeColour) {
    const uint16_t c0 = (uint16_t)(in[0] | (in[1] << 8)), c1 = (uint16_t)(in[2] | (in[3] << 8));
    float p[4][3];
    Unpack565(c0, p[0]);
    Unpack565(c1, p[1]);
    const bool four = bc3 || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        if (four) { p[2][k] = (2.f * p[0][k] + p[1][k]) / 3.f; p[3][k] = (p[0][k] + 2.f * p[1][k]) / 3.f; }
        else      { p[2][k] = (p[0][k] + p[1][k]) / 2.f;       p[3][k] = 0.f; }
    }
    if (threeColour) *threeColour = !four;
    const uint32_t idx = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
    for (int i = 0; i < 16; ++i) {
        const int c = (int)((idx >> (2 * i)) & 3u);
        px[i][0] = p[c][0]; px[i][1] = p[c][1]; px[i][2] = p[c][2];
    }
}

void MakePal4(uint16_t c0, uint16_t c1, float p[4][3]) {
    Unpack565(c0, p[0]);
    Unpack565(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        p[2][k] = (2.f * p[0][k] + p[1][k]) / 3.f;
        p[3][k] = (p[0][k] + 2.f * p[1][k]) / 3.f;
    }
}

float AssignIndices(const float px[16][3], const float p[4][3], int nColours, uint32_t* idxOut) {
    float err = 0.f;
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        float be = 1e30f;
        for (int c = 0; c < nColours; ++c) {
            const float dr = px[i][0] - p[c][0], dg = px[i][1] - p[c][1], db = px[i][2] - p[c][2];
            const float e = dr * dr + dg * dg + db * db;
            if (e < be) { be = e; best = c; }
        }
        err += be;
        idx |= (uint32_t)best << (2 * i);
    }
    *idxOut = idx;
    return err;
}

// Opaque 4-colour BC1 colour block (c0 > c1) by principal-axis endpoints plus least-squares refinement.
void EncodeColour(const float px[16][3], uint8_t out[8]) {
    float mean[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) mean[k] += px[i][k] * (1.f / 16.f);
    float cov[3][3] = {};
    for (int i = 0; i < 16; ++i) {
        const float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) cov[a][b] += d[a] * d[b];
    }
    const float trace = cov[0][0] + cov[1][1] + cov[2][2];
    auto put = [&](uint16_t c0, uint16_t c1, uint32_t idx) {
        out[0] = (uint8_t)(c0 & 0xff); out[1] = (uint8_t)(c0 >> 8);
        out[2] = (uint8_t)(c1 & 0xff); out[3] = (uint8_t)(c1 >> 8);
        out[4] = (uint8_t)(idx & 0xff); out[5] = (uint8_t)((idx >> 8) & 0xff);
        out[6] = (uint8_t)((idx >> 16) & 0xff); out[7] = (uint8_t)((idx >> 24) & 0xff);
    };
    if (trace < 1e-3f) {            // flat block: one colour, all indices 0 (c0 == c1 is the 3-colour mode, index 0 = c0)
        const uint16_t c = Pack565(mean);
        put(c, c, 0);
        return;
    }
    // principal axis: power iteration from the widest channel
    float axis[3] = { 0, 0, 0 };
    axis[cov[0][0] >= cov[1][1] && cov[0][0] >= cov[2][2] ? 0 : (cov[1][1] >= cov[2][2] ? 1 : 2)] = 1.f;
    for (int it = 0; it < 8; ++it) {
        float v[3];
        for (int a = 0; a < 3; ++a) v[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-9f) break;
        for (int a = 0; a < 3; ++a) axis[a] = v[a] / len;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; ++i) {
        const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) { e0[k] = mean[k] + axis[k] * tmax; e1[k] = mean[k] + axis[k] * tmin; }

    float bestErr = 1e30f;
    uint16_t bestC0 = 0, bestC1 = 0;
    uint32_t bestIdx = 0;
    for (int round = 0; round < 4; ++round) {
        uint16_t c0 = Pack565(e0), c1 = Pack565(e1);
        if (c0 < c1) { const uint16_t t = c0; c0 = c1; c1 = t; }
        float err;
        uint32_t idx;
        if (c0 == c1) {             // quantised to one colour: the 3-colour mode, only index 0 is used
            float p[4][3];
            Unpack565(c0, p[0]);
            err = AssignIndices(px, p, 1, &idx);
        } else {
            float p[4][3];
            MakePal4(c0, c1, p);
            err = AssignIndices(px, p, 4, &idx);
        }
        if (err < bestErr) { bestErr = err; bestC0 = c0; bestC1 = c1; bestIdx = idx; } else if (round > 0) break;
        if (c0 == c1) break;
        // least-squares endpoints for these indices: pixel ~ a*c0 + b*c1 with (a,b) = (1,0) (0,1) (2/3,1/3) (1/3,2/3)
        static const float A[4] = { 1.f, 0.f, 2.f / 3.f, 1.f / 3.f }, B[4] = { 0.f, 1.f, 1.f / 3.f, 2.f / 3.f };
        float a11 = 0, a12 = 0, a22 = 0, b1[3] = { 0, 0, 0 }, b2[3] = { 0, 0, 0 };
        for (int i = 0; i < 16; ++i) {
            const int c = (int)((idx >> (2 * i)) & 3u);
            a11 += A[c] * A[c]; a12 += A[c] * B[c]; a22 += B[c] * B[c];
            for (int k = 0; k < 3; ++k) { b1[k] += A[c] * px[i][k]; b2[k] += B[c] * px[i][k]; }
        }
        const float det = a11 * a22 - a12 * a12;
        if (std::fabs(det) < 1e-6f) break;
        for (int k = 0; k < 3; ++k) {
            e0[k] = (a22 * b1[k] - a12 * b2[k]) / det;
            e1[k] = (a11 * b2[k] - a12 * b1[k]) / det;
        }
    }
    put(bestC0, bestC1, bestIdx);
}

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline int BlocksAcross(int w) { return w < 4 ? 1 : (w + 3) / 4; }
inline int MipWidth(int width, int level) { const int w = width >> level; return w < 1 ? 1 : w; }

uint64_t TailHashImpl(int width, int levels, const std::vector<std::vector<uint8_t>>& mips) {
    uint64_t h = kFnvBasis;
    for (int l = 0; l < levels && l < (int)mips.size(); ++l) {
        if (MipWidth(width, l) > 64) continue;
        for (uint8_t b : mips[(size_t)l]) { h ^= b; h *= kFnvPrime; }
    }
    return h;
}

// ===========================================================================================================
// Compositing the decals into a mip chain (blocks that no decal touches are never modified)
// ===========================================================================================================
int CompositeImpl(int width, int levels, int bpb, int baseSize, const std::vector<DecalRt>& decals,
                  std::vector<std::vector<uint8_t>>& mips) {
    if (width <= 0 || levels <= 0 || (bpb != 8 && bpb != 16) || baseSize <= 0 || (int)mips.size() < levels) return -1;
    if (width > baseSize) return -1;
    int reencoded = 0;
    for (int l = 0; l < levels; ++l) {
        const int w = MipWidth(width, l);
        if (baseSize % w != 0) continue;
        const int s = baseSize / w;
        int k = 0;
        while ((1 << k) < s) ++k;
        if ((1 << k) != s || k >= kMaxPyr) continue;       // not a power of two, or narrower than baseSize/16
        const int bw = BlocksAcross(w), bh = BlocksAcross(w);
        if (mips[(size_t)l].size() < (size_t)bw * bh * bpb) return -1;
        std::vector<uint8_t> touched((size_t)bw * bh, 0);
        for (const DecalRt& d : decals) {
            const int rx = d.x >> k, ry = d.y >> k, rw = d.w >> k, rh = d.h >> k;
            if (rw <= 0 || rh <= 0) continue;
            for (int by = ry / 4; by <= (ry + rh - 1) / 4 && by < bh; ++by)
                for (int bx = rx / 4; bx <= (rx + rw - 1) / 4 && bx < bw; ++bx) touched[(size_t)by * bw + bx] = 1;
        }
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                if (!touched[(size_t)by * bw + bx]) continue;
                uint8_t* blk = mips[(size_t)l].data() + ((size_t)by * bw + bx) * bpb;
                const bool bc3 = bpb == 16;
                float px[16][3];
                DecodeColour(bc3 ? blk + 8 : blk, bc3, px, nullptr);
                bool changed = false;
                for (int ty = 0; ty < 4; ++ty)
                    for (int tx = 0; tx < 4; ++tx) {
                        const int X = bx * 4 + tx, Y = by * 4 + ty;
                        for (const DecalRt& d : decals) {
                            const int rx = d.x >> k, ry = d.y >> k, rw = d.w >> k, rh = d.h >> k;
                            if (X < rx || Y < ry || X >= rx + rw || Y >= ry + rh) continue;
                            const uint8_t a8 = d.lv[k][(size_t)(Y - ry) * rw + (X - rx)];
                            if (!a8) continue;
                            const float a = a8 * (1.0f / 255.0f);
                            for (int c = 0; c < 3; ++c) px[ty * 4 + tx][c] = px[ty * 4 + tx][c] * (1.f - a) + d.rgb[c] * a;
                            changed = true;
                        }
                    }
                if (!changed) continue;
                EncodeColour(px, bc3 ? blk + 8 : blk);     // BC3: the 8 alpha bytes in front stay as they were
                ++reencoded;
            }
    }
    return reencoded;
}

// ===========================================================================================================
// State: slot table, replacements, pending candidates, readback jobs
// ===========================================================================================================
enum State : uint32_t { kUnclassified = 0, kNotTarget = 1, kCandidate = 2, kTarget = 3, kClassifying = 4 };
constexpr uint32_t kStateMask = 0xff;
constexpr uint32_t kRetryMask = 0x00ff0000u;     // how often a not-yet-matching mutable texture was re-queued
constexpr uint32_t kRetryUnit = 0x00010000u;

struct Slot {
    std::atomic<uintptr_t> key{ 0 };
    std::atomic<uint32_t>  info{ 0 };      // state | (replacement index << 8) | (retries << 16)
    std::atomic<uint32_t>  notBefore{ 0 }; // GetTickCount() before which an Unclassified retry is not started
};
constexpr uint32_t kSlotBits = 18;
constexpr uint32_t kSlotCount = 1u << kSlotBits;
Slot g_slots[kSlotCount];                  // 4 MB of zero-initialised BSS, touched lazily

inline uint32_t HashPtr(uintptr_t p) {
    return (uint32_t)(((uint64_t)p >> 4) * 0x9E3779B97F4A7C15ULL >> (64 - kSlotBits));
}
Slot* FindSlot(uintptr_t key, bool insert);

struct Repl {
    std::atomic<ID3D11ShaderResourceView*> srv{ nullptr };   // what the bind hook hands the game
    ID3D11ShaderResourceView* orig = nullptr;                // the stock SRV, kept alive so its address is never reused
    ID3D11Texture2D*          tex = nullptr;
    int                       target = -1;                   // a brand target, or -1 for a swap::Replace client (0.5.5 radar mask)
    // 0.5.5: what the bind hook really did with this entry. subst counts the binds it rewrote; every kVerifyEvery-th one (and the first) is
    // read back from the context right after the call (PSGetShaderResources) and counted as verified or missed.
    std::atomic<uint64_t> subst{ 0 };
    std::atomic<uint64_t> lastTick{ 0 };                     // GetTickCount64() of the last substitution
    std::atomic<uint32_t> verified{ 0 }, verifyMiss{ 0 };
    // seen counts every bind of the stock view, substituted or not; `on` = false leaves the game's view in place (swap::SetActive)
    std::atomic<uint64_t> seen{ 0 };
    std::atomic<uint64_t> lastSeenTick{ 0 };
    std::atomic<bool>     on{ true };
};
constexpr uint64_t kVerifyEvery = 256;
constexpr int kMaxRepl = 64;
Repl g_repl[kMaxRepl];
std::atomic<int> g_replCount{ 0 };

struct Pending {
    ID3D11ShaderResourceView* srv;         // AddRef'd
    ID3D11Texture2D*          tex;         // AddRef'd
    D3D11_TEXTURE2D_DESC      desc;
};
SRWLOCK g_pendLock = SRWLOCK_INIT;
std::vector<Pending> g_pending;
constexpr size_t kMaxPending = 1024;

struct Job {
    ID3D11ShaderResourceView* srv;
    ID3D11Texture2D*          src;
    ID3D11Texture2D*          staging;
    D3D11_TEXTURE2D_DESC      desc;
    ULONGLONG                 startedAt;
};
std::vector<Job> g_jobs;                   // Pump thread only

// 0.5.5 swap clients (radar_mask.cpp): the hooks are wanted even with brand_apparel=0; every teardown bumps the generation (handles die)
std::atomic<bool>     g_swapWanted{ false };
std::atomic<uint32_t> g_generation{ 1 };

// ---------------------------------------------------------------------------------------------------------
// 0.5.5 probe recorder: while armed, every 2D texture view of an armed size that the game binds is remembered with the set of probe windows it
// was bound in (render-thread frames 3+ of a window only: GTA renders the script's frame a frame or two late). radar_mask.cpp draws the texture
// it is looking for in some windows and nothing in others; the view bound in exactly its windows is the game's own texture of that name.
// ---------------------------------------------------------------------------------------------------------
struct ProbeEnt {
    std::atomic<uintptr_t> key{ 0 };
    std::atomic<uint32_t>  verdict{ 0 };     // 0 unseen, 1 not a candidate, 2 candidate, 3 being classified
    std::atomic<uint32_t>  seen{ 0 };        // bit k = bound in window k + 1
    std::atomic<uint64_t>  binds{ 0 };
    ID3D11ShaderResourceView* held = nullptr;   // candidates: AddRef'd until ProbeTake / teardown
    uint32_t w = 0, h = 0;
};
constexpr uint32_t kProbeSlots = 8192, kProbeMaxCands = 64, kProbeMaxSizes = 4, kProbeSkipFrames = 3;
ProbeEnt g_probeTab[kProbeSlots];
std::atomic<bool>     g_probeArmed{ false };
std::atomic<uint32_t> g_probeWinReq{ 0 };     // the script thread's window (0 = none)
uint32_t              g_probeFrameWin = 0;    // render thread: the window this frame belongs to
uint32_t              g_probeFramesIn = 0;    // render thread: frames since it changed
std::atomic<uint32_t> g_probeRecording{ 0 };  // the window the bind hook records into now (0 = none)
uint32_t              g_probeSizes[kProbeMaxSizes][2] = {};
int                   g_probeSizeCount = 0;
std::atomic<uint32_t> g_probeCands{ 0 }, g_probeOverflow{ 0 };

// counters
std::atomic<uint64_t> g_binds{ 0 };
std::atomic<uint32_t> g_srvSeen{ 0 }, g_candidates{ 0 }, g_fingerprinted{ 0 }, g_appliedTotal{ 0 }, g_tableFull{ 0 }, g_dropped{ 0 };
std::atomic<uint32_t> g_appliedLive{ 0 };

Slot* FindSlot(uintptr_t key, bool insert) {
    uint32_t i = HashPtr(key);
    for (int probe = 0; probe < 128; ++probe, i = (i + 1) & (kSlotCount - 1)) {
        const uintptr_t k = g_slots[i].key.load(std::memory_order_acquire);
        if (k == key) return &g_slots[i];
        if (k == 0) {
            if (!insert) return nullptr;
            uintptr_t expect = 0;
            if (g_slots[i].key.compare_exchange_strong(expect, key, std::memory_order_acq_rel)) {
                g_srvSeen.fetch_add(1, std::memory_order_relaxed);
                return &g_slots[i];
            }
            if (expect == key) return &g_slots[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------------------------------------
using PSSetSRVFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using CreateSRVFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                  ID3D11ShaderResourceView**);
// Measured on this machine (tests/brand_apparel_test.cpp, scratch defprobe): the vtable of a D3D11 context/device
// object lives in that object's own heap block (not in a table shared by the class), and a deferred context has
// another one each. So the patch is per object: it hits exactly the immediate context and device the engine holds,
// and it is only safe while that object is alive. We therefore keep our own reference on both until we restore.
PSSetSRVFn  g_origPS = nullptr;
CreateSRVFn g_origCreate = nullptr;
void**      g_ctxVtbl = nullptr;
void**      g_devVtbl = nullptr;
std::atomic<bool> g_active{ false };
std::atomic<bool> g_hooksInstalled{ false };
ID3D11Device*        g_dev = nullptr;      // owned references (AddRef'd in InstallHooks, released in Teardown)
ID3D11DeviceContext* g_ctx = nullptr;

bool IsBcFormat(DXGI_FORMAT f, bool bc3) {
    if (bc3) return f == DXGI_FORMAT_BC3_TYPELESS || f == DXGI_FORMAT_BC3_UNORM || f == DXGI_FORMAT_BC3_UNORM_SRGB;
    return f == DXGI_FORMAT_BC1_TYPELESS || f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC1_UNORM_SRGB;
}

// Right size and format to be worth a fingerprint? (cheap, no D3D calls besides what the caller already did)
bool MatchesAnyTarget(const D3D11_TEXTURE2D_DESC& d) {
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.Width != d.Height) return false;
    if (d.Usage == D3D11_USAGE_STAGING || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    if (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) return false;
    for (const TargetRt& t : g_targets) {
        if (!IsBcFormat(d.Format, t.bc3)) continue;
        const UINT base = (UINT)t.baseSize;
        if (d.Width != base && d.Width != base / 2 && d.Width != base / 4) continue;
        if (d.MipLevels < 5) continue;                         // the fingerprint needs the <= 64 px tail
        return true;
    }
    return false;
}

// First sight of an SRV (render thread or any thread the game binds from).
uint32_t Classify(Slot* sl, ID3D11ShaderResourceView* srv, uint32_t seen) {
    const uint32_t keep = seen & kRetryMask;
    uint32_t expect = seen;
    // claim the slot: only one thread classifies (and queues) a given view
    if (!sl->info.compare_exchange_strong(expect, kClassifying | keep, std::memory_order_acq_rel)) return expect;
    uint32_t state = kNotTarget;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex)) && tex) {
                D3D11_TEXTURE2D_DESC d;
                tex->GetDesc(&d);
                if (MatchesAnyTarget(d)) {
                    AcquireSRWLockExclusive(&g_pendLock);
                    if (g_pending.size() < kMaxPending) {
                        srv->AddRef();
                        g_pending.push_back({ srv, tex, d });   // keeps the QueryInterface reference
                        tex = nullptr;
                        state = kCandidate;
                        g_candidates.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                    ReleaseSRWLockExclusive(&g_pendLock);
                }
                if (tex) tex->Release();
            }
        }
        res->Release();
    }
    const uint32_t fin = state | keep;
    sl->info.store(fin, std::memory_order_release);
    return fin;
}

// 0.5.5 probe recorder, from the bind hook while armed (the game binds from its render thread; the CAS keeps a second thread out anyway).
void ProbeRecord(ID3D11ShaderResourceView* const* pp, UINT num) {
    const uint32_t win = g_probeRecording.load(std::memory_order_relaxed);
    for (UINT i = 0; i < num; ++i) {
        ID3D11ShaderResourceView* s = pp[i];
        if (!s) continue;
        const uintptr_t key = (uintptr_t)s;
        uint32_t idx = (uint32_t)(((uint64_t)key >> 4) * 0x9E3779B97F4A7C15ULL >> (64 - 13));
        ProbeEnt* e = nullptr;
        for (int probe = 0; probe < 64; ++probe, idx = (idx + 1) & (kProbeSlots - 1)) {
            uintptr_t k = g_probeTab[idx].key.load(std::memory_order_acquire);
            if (k == key) { e = &g_probeTab[idx]; break; }
            if (k == 0) {
                if (g_probeTab[idx].key.compare_exchange_strong(k, key, std::memory_order_acq_rel) || k == key) { e = &g_probeTab[idx]; break; }
            }
        }
        if (!e) continue;
        uint32_t v = e->verdict.load(std::memory_order_acquire);
        if (v == 0) {
            uint32_t expect = 0;
            if (!e->verdict.compare_exchange_strong(expect, 3, std::memory_order_acq_rel)) continue;
            uint32_t fin = 1;
            ID3D11Resource* res = nullptr;
            s->GetResource(&res);
            if (res) {
                D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
                res->GetType(&dim);
                ID3D11Texture2D* tex = nullptr;
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC d;
                    tex->GetDesc(&d);
                    tex->Release();
                    for (int z = 0; z < g_probeSizeCount && fin == 1; ++z)
                        if (d.Width == g_probeSizes[z][0] && d.Height == g_probeSizes[z][1] && d.ArraySize == 1 && !(d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
                            if (g_probeCands.load() >= kProbeMaxCands) { g_probeOverflow.fetch_add(1); break; }
                            g_probeCands.fetch_add(1);
                            s->AddRef();
                            e->held = s;
                            e->w = d.Width;
                            e->h = d.Height;
                            fin = 2;
                        }
                }
                res->Release();
            }
            e->verdict.store(fin, std::memory_order_release);
            v = fin;
        }
        if (v != 2) continue;
        e->binds.fetch_add(1, std::memory_order_relaxed);
        if (win >= 1 && win <= 32) e->seen.fetch_or(1u << (win - 1), std::memory_order_relaxed);
    }
}

void STDMETHODCALLTYPE HookPSSetSRV(ID3D11DeviceContext* self, UINT start, UINT num, ID3D11ShaderResourceView* const* pp) {
    g_binds.fetch_add(1, std::memory_order_relaxed);
    const PSSetSRVFn orig = g_origPS;
    if (!pp || num == 0 || num > 128 || !g_active.load(std::memory_order_relaxed)) {
        orig(self, start, num, pp);
        return;
    }
    if (g_probeArmed.load(std::memory_order_relaxed)) ProbeRecord(pp, num);   // the stock views, before any substitution
    ID3D11ShaderResourceView* local[128];
    bool copied = false;
    int verifySlot = -1;          // one substitution per call is read back (every kVerifyEvery-th of its entry)
    UINT verifyAt = 0;
    for (UINT i = 0; i < num; ++i) {
        ID3D11ShaderResourceView* s = pp[i];
        if (!s) continue;
        Slot* sl = FindSlot((uintptr_t)s, true);
        if (!sl) { g_tableFull.fetch_add(1, std::memory_order_relaxed); continue; }
        uint32_t info = sl->info.load(std::memory_order_acquire);
        if ((info & kStateMask) == kUnclassified) {
            if ((info & kRetryMask) && (int32_t)(GetTickCount() - sl->notBefore.load(std::memory_order_relaxed)) < 0) continue;
            info = Classify(sl, s, info);
        }
        if ((info & kStateMask) != kTarget) continue;
        Repl& rp = g_repl[(info >> 8) & 0xff];
        ID3D11ShaderResourceView* r = rp.srv.load(std::memory_order_acquire);
        if (!r) continue;
        const ULONGLONG nowTick = GetTickCount64();
        rp.seen.fetch_add(1, std::memory_order_relaxed);
        rp.lastSeenTick.store(nowTick, std::memory_order_relaxed);
        if (!rp.on.load(std::memory_order_acquire)) continue;
        if (!copied) { for (UINT j = 0; j < num; ++j) local[j] = pp[j]; copied = true; }
        local[i] = r;
        const uint64_t n = rp.subst.fetch_add(1, std::memory_order_relaxed);
        rp.lastTick.store(nowTick, std::memory_order_relaxed);
        if (verifySlot < 0 && n % kVerifyEvery == 0) { verifySlot = (int)((info >> 8) & 0xff); verifyAt = i; }
    }
    orig(self, start, num, copied ? local : pp);
    if (verifySlot >= 0) {
        // read back what the context holds now: proof that the game's draw sees our view, not just that we passed it on
        ID3D11ShaderResourceView* got = nullptr;
        self->PSGetShaderResources(start + verifyAt, 1, &got);
        Repl& rp = g_repl[verifySlot];
        if (got && got == rp.srv.load(std::memory_order_acquire)) rp.verified.fetch_add(1, std::memory_order_relaxed);
        else rp.verifyMiss.fetch_add(1, std::memory_order_relaxed);
        if (got) got->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookCreateSRV(ID3D11Device* self, ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                        ID3D11ShaderResourceView** ppSRV) {
    const HRESULT hr = g_origCreate(self, res, desc, ppSRV);
    if (SUCCEEDED(hr) && ppSRV && *ppSRV && g_active.load(std::memory_order_relaxed)) {
        // a new view at an address we classified before: the old verdict is about a dead object. Candidates and
        // Targets cannot be hit here (we hold a reference on every one of those), so only NotTarget needs resetting.
        if (Slot* sl = FindSlot((uintptr_t)*ppSRV, false)) {
            uint32_t info = sl->info.load(std::memory_order_acquire);
            if ((info & kStateMask) == kNotTarget) sl->info.compare_exchange_strong(info, kUnclassified);
        }
    }
    return hr;
}

bool PatchSlot(void** vtbl, int idx, void* hook, void** origOut) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (vtbl[idx] != hook) *origOut = vtbl[idx];
    InterlockedExchangePointer(&vtbl[idx], hook);
    DWORD tmp = 0;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &tmp);
    return vtbl[idx] == hook;
}
void RestoreSlot(void** vtbl, int idx, void* hook, void* orig) {
    if (!vtbl || !orig || vtbl[idx] != hook) return;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    InterlockedExchangePointer(&vtbl[idx], orig);
    DWORD tmp = 0;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &tmp);
}

constexpr int kVtblPSSetShaderResources = 8;       // ID3D11DeviceContext: IUnknown 0-2, DeviceChild 3-6, VSSetConstantBuffers 7
constexpr int kVtblCreateSRV = 7;                  // ID3D11Device: IUnknown 0-2, CreateBuffer 3, Texture1D 4, Texture2D 5, Texture3D 6

void Teardown(const char* why);

bool InstallHooks(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    EnsureTargets();
    if (!dev || !ctx) return false;
    void** cvt = *reinterpret_cast<void***>(ctx);
    void** dvt = *reinterpret_cast<void***>(dev);
    if (!cvt || !dvt) return false;
    if (g_hooksInstalled.load() && g_dev == dev && g_ctx == ctx && g_ctxVtbl == cvt && g_devVtbl == dvt) return true;
    if (g_hooksInstalled.load()) Teardown("the game's device or context changed");
    // Hold our references first: the vtables we write into live inside these objects.
    dev->AddRef();
    ctx->AddRef();
    if (!PatchSlot(cvt, kVtblPSSetShaderResources, reinterpret_cast<void*>(&HookPSSetSRV), reinterpret_cast<void**>(&g_origPS)) ||
        !PatchSlot(dvt, kVtblCreateSRV, reinterpret_cast<void*>(&HookCreateSRV), reinterpret_cast<void**>(&g_origCreate))) {
        GLOGW("brand apparel: could not patch the D3D11 vtables (VirtualProtect failed); no logo");
        RestoreSlot(cvt, kVtblPSSetShaderResources, reinterpret_cast<void*>(&HookPSSetSRV), reinterpret_cast<void*>(g_origPS));
        RestoreSlot(dvt, kVtblCreateSRV, reinterpret_cast<void*>(&HookCreateSRV), reinterpret_cast<void*>(g_origCreate));
        ctx->Release();
        dev->Release();
        return false;
    }
    g_ctxVtbl = cvt; g_devVtbl = dvt; g_dev = dev; g_ctx = ctx;
    g_hooksInstalled.store(true);
    g_active.store(true);
    GLOGI("brand apparel: hooks installed (device %p, context %p, PSSetShaderResources %p, CreateShaderResourceView %p); "
          "watching for %u stock texture fingerprint(s)",
          (void*)dev, (void*)ctx, (void*)g_origPS, (void*)g_origCreate, (unsigned)g_targets.size());
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// readback jobs (Pump thread)
// ---------------------------------------------------------------------------------------------------------
void ReleaseJob(Job& j) {
    if (j.staging) j.staging->Release();
    if (j.src) j.src->Release();
    if (j.srv) j.srv->Release();
    j = Job{};
}

void SetSlotState(ID3D11ShaderResourceView* srv, uint32_t info) {
    if (Slot* sl = FindSlot((uintptr_t)srv, true)) sl->info.store(info, std::memory_order_release);
}

void FailJob(Job& j, uint32_t state = kNotTarget) {
    // 0.5.5: a swap::Replace client may have claimed this view while its readback ran; its verdict stands
    Slot* sl = FindSlot((uintptr_t)j.srv, false);
    if (!sl || (sl->info.load(std::memory_order_acquire) & kStateMask) != kTarget) SetSlotState(j.srv, state);
    ReleaseJob(j);
}

bool ReadAllMips(ID3D11DeviceContext* ctx, const Job& j, std::vector<std::vector<uint8_t>>& mips, int bpb, bool& stillDrawing) {
    stillDrawing = false;
    const int levels = (int)j.desc.MipLevels;
    mips.assign((size_t)levels, {});
    for (int l = 0; l < levels; ++l) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = ctx->Map(j.staging, (UINT)l, D3D11_MAP_READ, l == 0 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { stillDrawing = true; return false; }
        if (FAILED(hr) || !m.pData) return false;
        const int w = MipWidth((int)j.desc.Width, l), bw = BlocksAcross(w), bh = BlocksAcross(w);
        mips[(size_t)l].resize((size_t)bw * bh * bpb);
        for (int r = 0; r < bh; ++r)
            std::memcpy(mips[(size_t)l].data() + (size_t)r * bw * bpb, (const uint8_t*)m.pData + (size_t)r * m.RowPitch, (size_t)bw * bpb);
        ctx->Unmap(j.staging, (UINT)l);
    }
    return true;
}

uint32_t g_learnLogged = 0;
constexpr uint32_t kLearnLogMax = 40;

// A fingerprint matched: composite, build the replacement texture + SRV, publish it to the bind hook.
bool ApplyTarget(ID3D11Device* dev, const Job& j, int ti, std::vector<std::vector<uint8_t>>& mips, int bpb, uint64_t hash) {
    const TargetRt& t = g_targets[(size_t)ti];
    const ULONGLONG t0 = GetTickCount64();
    const int n = CompositeImpl((int)j.desc.Width, (int)j.desc.MipLevels, bpb, t.baseSize, t.decals, mips);
    if (n < 0) {
        GLOGW("brand apparel: matched %s but the compositor refused the %ux%u chain; no logo", t.name.c_str(), (unsigned)j.desc.Width, (unsigned)j.desc.Height);
        return false;
    }
    const int slot = g_replCount.load();
    if (slot >= kMaxRepl) {
        GLOGW("brand apparel: replacement slots exhausted (%d); the stock texture stays", kMaxRepl);
        return false;
    }
    D3D11_TEXTURE2D_DESC rd = j.desc;
    rd.Usage = D3D11_USAGE_IMMUTABLE;
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rd.CPUAccessFlags = 0;
    rd.MiscFlags = 0;
    std::vector<D3D11_SUBRESOURCE_DATA> sd((size_t)j.desc.MipLevels);
    for (size_t l = 0; l < sd.size(); ++l) {
        const int w = MipWidth((int)j.desc.Width, (int)l);
        sd[l].pSysMem = mips[l].data();
        sd[l].SysMemPitch = (UINT)(BlocksAcross(w) * bpb);
        sd[l].SysMemSlicePitch = 0;
    }
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&rd, sd.data(), &tex);
    if (FAILED(hr) || !tex) {
        GLOGW("brand apparel: CreateTexture2D for the branded tee failed (0x%08lx); no logo", (unsigned long)hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    j.srv->GetDesc(&vd);
    ID3D11ShaderResourceView* nsrv = nullptr;
    hr = dev->CreateShaderResourceView(tex, &vd, &nsrv);
    if (FAILED(hr) || !nsrv) {
        GLOGW("brand apparel: CreateShaderResourceView for the branded tee failed (0x%08lx); no logo", (unsigned long)hr);
        tex->Release();
        return false;
    }
    Repl& r = g_repl[slot];
    r.orig = j.srv;
    j.srv->AddRef();                                      // keep the stock SRV's address unique for the life of the process
    r.tex = tex;
    r.target = ti;
    SetSlotState(nsrv, kNotTarget);                       // if the game ever re-binds what it read back from the context, no re-scan
    r.srv.store(nsrv, std::memory_order_release);
    g_replCount.store(slot + 1);
    SetSlotState(j.srv, kTarget | ((uint32_t)slot << 8));
    g_appliedLive.fetch_add(1);
    g_appliedTotal.fetch_add(1);
    GLOGI("brand apparel: applied: stock texture %s (srv %p, %ux%u, %u mips, tail hash %016llx) replaced by the Solar RP tee "
          "(%d of %d blocks re-encoded, %d decals, %llu ms); the game's draw calls now bind srv %p",
          t.name.c_str(), (void*)j.srv, (unsigned)j.desc.Width, (unsigned)j.desc.Height, (unsigned)j.desc.MipLevels,
          (unsigned long long)hash, n, (int)(BlocksAcross((int)j.desc.Width) * BlocksAcross((int)j.desc.Width)), (int)t.decals.size(),
          (unsigned long long)(GetTickCount64() - t0), (void*)nsrv);
    return true;
}

// 0 = still waiting, 1 = finished (job released), -1 = failed (job released)
int FinishJob(ID3D11DeviceContext* ctx, ID3D11Device* dev, Job& j) {
    // which block size? the formats of every target in the list are BC1 or BC3; read it off the descriptor
    const bool bc3 = IsBcFormat(j.desc.Format, true);
    const int bpb = bc3 ? 16 : 8;
    std::vector<std::vector<uint8_t>> mips;
    bool still = false;
    if (!ReadAllMips(ctx, j, mips, bpb, still)) {
        if (still) {
            if (GetTickCount64() - j.startedAt > 4000) { GLOGW("brand apparel: a readback stayed busy for 4 s; giving up on srv %p", (void*)j.srv); FailJob(j); return -1; }
            return 0;
        }
        GLOGW("brand apparel: mapping the staging copy of srv %p failed; skipping it", (void*)j.srv);
        FailJob(j);
        return -1;
    }
    g_fingerprinted.fetch_add(1);
    const uint64_t hash = TailHashImpl((int)j.desc.Width, (int)j.desc.MipLevels, mips);
    int match = -1;
    for (size_t i = 0; i < g_targets.size(); ++i)
        if (g_targets[i].hash == hash && g_targets[i].bc3 == bc3) { match = (int)i; break; }
    if (match < 0) {
        Slot* const rsl = FindSlot((uintptr_t)j.srv, false);
        const uint32_t tries = rsl ? (rsl->info.load(std::memory_order_acquire) & kRetryMask) >> 16 : 3u;
        const bool willRetry = j.desc.Usage != D3D11_USAGE_IMMUTABLE && tries < 3;
        if (!willRetry && g_learnLogged < kLearnLogMax) {
            ++g_learnLogged;
            GLOGI("brand apparel: candidate %ux%u %s %u mips (usage %d), tail hash %016llx: not a target%s", (unsigned)j.desc.Width, (unsigned)j.desc.Height,
                  bc3 ? "BC3" : "BC1", (unsigned)j.desc.MipLevels, (int)j.desc.Usage, (unsigned long long)hash,
                  g_learnLogged == kLearnLogMax ? " (further candidates are not listed)" : "");
        }
        // A texture the game can still write to (not IMMUTABLE) may have been created empty and filled by streaming:
        // look again a couple of times, 1.5 s, 3 s and 4.5 s later, before giving up on it.
        if (willRetry && rsl) {
            rsl->notBefore.store(GetTickCount() + 1500u * (tries + 1), std::memory_order_relaxed);
            rsl->info.store(kUnclassified | ((tries + 1) << 16), std::memory_order_release);
            ReleaseJob(j);
            return 1;
        }
        FailJob(j);
        return 1;
    }
    if (!ApplyTarget(dev, j, match, mips, bpb, hash)) { FailJob(j); return -1; }
    ReleaseJob(j);     // the slot state was set to Target by ApplyTarget; the job's references are dropped
    return 1;
}

// Start up to `budget` new readbacks from the pending list.
void StartJobs(ID3D11DeviceContext* ctx, ID3D11Device* dev, int budget) {
    for (; budget > 0; --budget) {
        Pending p = {};
        AcquireSRWLockExclusive(&g_pendLock);
        if (!g_pending.empty()) { p = g_pending.back(); g_pending.pop_back(); }
        ReleaseSRWLockExclusive(&g_pendLock);
        if (!p.srv) return;
        D3D11_TEXTURE2D_DESC sd = p.desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
            Job j = { p.srv, p.tex, nullptr, p.desc, GetTickCount64() };
            FailJob(j);
            continue;
        }
        ctx->CopyResource(staging, p.tex);
        g_jobs.push_back({ p.srv, p.tex, staging, p.desc, GetTickCount64() });
    }
}

// ---------------------------------------------------------------------------------------------------------
// the per-frame pump + status logging
// ---------------------------------------------------------------------------------------------------------
std::atomic<ULONGLONG> g_dressedAt{ 0 };
std::atomic<bool>      g_warnedNoMatch{ false };
ULONGLONG              g_lastStatusLog = 0;
int                    g_statusLogs = 0;

void LogStatus(const char* why) {
    GLOGI("brand apparel: status (%s): binds %llu, srv seen %u, candidates %u, fingerprinted %u, applied %u live / %u total, "
          "dropped %u, table-full %u, pending jobs %u",
          why, (unsigned long long)g_binds.load(), g_srvSeen.load(), g_candidates.load(), g_fingerprinted.load(), g_appliedLive.load(),
          g_appliedTotal.load(), g_dropped.load(), g_tableFull.load(), (unsigned)g_jobs.size());
}

void PumpImpl(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    // finish what the GPU has completed
    for (size_t i = 0; i < g_jobs.size();) {
        const int r = FinishJob(ctx, dev, g_jobs[i]);
        if (r == 0) { ++i; continue; }
        g_jobs.erase(g_jobs.begin() + (std::ptrdiff_t)i);
    }
    // start new ones (a handful per frame, bounded in flight)
    if (g_jobs.size() < 32) StartJobs(ctx, dev, 6);
    dev->Release();

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG dressed = g_dressedAt.load();
    if (dressed && g_appliedLive.load() == 0 && !g_warnedNoMatch.load() && now - dressed > 30000) {
        g_warnedNoMatch.store(true);
        LogStatus("30 s after the outfit was applied and no stock tee texture matched");
        GLOGW("brand apparel: NOT applied: no texture with the stock tee's fingerprint (jbib_diff_008_k_uni, 512x512 BC1, 8 mips) was bound "
              "in 30 s. The 'candidate ... not a target' lines above list the hashes that were seen. Possible causes: the game "
              "binds through a context/vtable we do not hook (binds above stayed 0), the texture is created at another size/format, "
              "or the player is not wearing top 11/8/10.");
    }
    if (!g_lastStatusLog) g_lastStatusLog = now;
    if (g_statusLogs < 4 && now - g_lastStatusLog > (g_statusLogs == 0 ? 20000u : 60000u)) {
        g_lastStatusLog = now;
        ++g_statusLogs;
        LogStatus("periodic");
    }
}

// 0.5.5: disarm the probe recorder and drop every candidate it holds (render thread, or teardown).
void ProbeClear() {
    g_probeArmed.store(false);
    g_probeRecording.store(0);
    for (uint32_t i = 0; i < kProbeSlots; ++i) {
        ProbeEnt& e = g_probeTab[i];
        if (!e.key.load(std::memory_order_relaxed)) continue;
        if (e.held) { e.held->Release(); e.held = nullptr; }
        e.verdict.store(0); e.seen.store(0); e.binds.store(0); e.w = e.h = 0;
        e.key.store(0);
    }
    g_probeCands.store(0);
    g_probeOverflow.store(0);
    g_probeFrameWin = 0;
    g_probeFramesIn = 0;
}

// Restore both vtable slots and drop every D3D object we hold (jobs, queue, replacements, our device/context
// references) and every verdict. Runs on the render thread (Poll) or at engine shutdown; the bind hook is told to
// stand down first.
void Teardown(const char* why) {
    g_active.store(false);
    if (g_hooksInstalled.exchange(false)) {
        RestoreSlot(g_ctxVtbl, kVtblPSSetShaderResources, reinterpret_cast<void*>(&HookPSSetSRV), reinterpret_cast<void*>(g_origPS));
        RestoreSlot(g_devVtbl, kVtblCreateSRV, reinterpret_cast<void*>(&HookCreateSRV), reinterpret_cast<void*>(g_origCreate));
        GLOGI("brand apparel: hooks removed (%s)", why ? why : "?");
    }
    for (Job& j : g_jobs) ReleaseJob(j);
    g_jobs.clear();
    AcquireSRWLockExclusive(&g_pendLock);
    for (Pending& p : g_pending) { if (p.srv) p.srv->Release(); if (p.tex) p.tex->Release(); }
    g_pending.clear();
    ReleaseSRWLockExclusive(&g_pendLock);
    const int n = g_replCount.exchange(0);
    for (int i = 0; i < n; ++i) {
        if (ID3D11ShaderResourceView* s = g_repl[i].srv.exchange(nullptr)) s->Release();
        if (g_repl[i].tex) { g_repl[i].tex->Release(); g_repl[i].tex = nullptr; }
        if (g_repl[i].orig) { g_repl[i].orig->Release(); g_repl[i].orig = nullptr; }
        g_repl[i].target = -1;
        g_repl[i].subst.store(0); g_repl[i].lastTick.store(0); g_repl[i].verified.store(0); g_repl[i].verifyMiss.store(0);
        g_repl[i].seen.store(0); g_repl[i].lastSeenTick.store(0); g_repl[i].on.store(true);
    }
    ProbeClear();
    g_generation.fetch_add(1);                         // every swap::Replace handle handed out so far is dead now
    if (g_srvSeen.load() != 0) {                       // nothing was ever classified: skip walking 4 MB of untouched table
        for (uint32_t i = 0; i < kSlotCount; ++i) {
            if (g_slots[i].key.load(std::memory_order_relaxed)) { g_slots[i].key.store(0); g_slots[i].info.store(0); g_slots[i].notBefore.store(0); }
        }
        g_srvSeen.store(0);
    }
    g_appliedLive.store(0);
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_dev = nullptr; g_ctx = nullptr; g_ctxVtbl = nullptr; g_devVtbl = nullptr;
}

}  // namespace

// ===========================================================================================================
// Public API
// ===========================================================================================================
bool Enabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const std::wstring v = reg::Get(L"brand_apparel", L"1");
        enabled = (v == L"0") ? 0 : 1;
        GLOGI("brand apparel: %s (HKCU\\Software\\GAGE-MP brand_apparel=%ls)", enabled ? "enabled" : "disabled", v.c_str());
    }
    return enabled == 1;
}

namespace {
std::atomic<int> g_notD3D11{ 0 };   // 0.5.5: 1 = Poll saw a swap chain without a D3D11 device (swap::NotD3D11)
}

void Poll(void* swapChain) {
    const bool brand = Enabled();
    if (!swapChain || !(brand || g_swapWanted.load())) return;
    static bool loggedNotD3D11 = false;
    static bool gaveUp = false;
    if (gaveUp) return;
    IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swapChain);
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_ID3D11Device, (void**)&dev)) || !dev) {
        if (!loggedNotD3D11) {
            loggedNotD3D11 = true;
            GLOGI("brand apparel: skipped: the swap chain has no ID3D11Device (Direct3D 12 / Enhanced): the texture swap is D3D11 only");
        }
        g_notD3D11.store(1);
        gaveUp = true;
        return;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        if (g_dev != dev || g_ctx != ctx || !g_hooksInstalled.load()) {
            if (g_dev && g_dev != dev) GLOGI("brand apparel: the game's device changed (%p -> %p); re-installing", (void*)g_dev, (void*)dev);
            InstallHooks(dev, ctx);     // tears the old install down itself when the device or context differs
        }
        if (g_hooksInstalled.load()) {
            swap::ProbeFrame();
            if (brand) PumpImpl(ctx);
        }
        ctx->Release();
    }
    dev->Release();
}

void NoteDressed(const char* when) {
    if (!Enabled()) return;
    ULONGLONG zero = 0;
    g_dressedAt.compare_exchange_strong(zero, GetTickCount64());
    static int logged = 0;
    if (logged < 6) {
        ++logged;
        GLOGI("brand apparel: outfit applied (%s) with top %d/%d/%d (the Solar RP tee); texture swap: %s (applied %u live, candidates %u, binds %llu)",
              when ? when : "?", kTopComponent, kTopDrawable, kTopTexture,
              g_appliedLive.load() ? "ACTIVE" : (g_hooksInstalled.load() ? "waiting for the texture to stream in" : "hooks not installed yet"),
              g_appliedLive.load(), g_candidates.load(), (unsigned long long)g_binds.load());
    }
}

void Shutdown() { Teardown("shutdown"); }

Stats GetStats() {
    Stats s;
    s.enabled = Enabled();
    s.installed = g_hooksInstalled.load();
    s.binds = g_binds.load();
    s.srvSeen = g_srvSeen.load();
    s.candidates = g_candidates.load();
    s.fingerprinted = g_fingerprinted.load();
    s.applied = g_appliedLive.load();
    return s;
}

// ===========================================================================================================
// 0.5.5 swap layer: the same two hooks for another client (radar_mask.cpp)
// ===========================================================================================================
namespace swap {

void Want() { g_swapWanted.store(true); }
bool Installed() { return g_hooksInstalled.load(); }
bool NotD3D11() { return g_notD3D11.load() != 0; }
uint32_t Generation() { return g_generation.load(); }
ID3D11Device* Device() { return g_dev; }
ID3D11DeviceContext* Context() { return g_ctx; }

int Replace(ID3D11ShaderResourceView* stock, ID3D11ShaderResourceView* repl, bool active) {
    if (!stock || !repl || !g_hooksInstalled.load()) return -1;
    if (Slot* sl = FindSlot((uintptr_t)stock, false)) {
        const uint32_t info = sl->info.load(std::memory_order_acquire);
        if ((info & kStateMask) == kTarget) return -1;      // already replaced (by brand or an earlier call)
    }
    const int slot = g_replCount.load();
    if (slot >= kMaxRepl) return -1;
    Slot* ss = FindSlot((uintptr_t)stock, true);
    if (!ss) return -1;
    Repl& r = g_repl[slot];
    stock->AddRef();
    repl->AddRef();
    r.orig = stock;
    r.tex = nullptr;
    r.target = -1;
    r.subst.store(0); r.lastTick.store(0); r.verified.store(0); r.verifyMiss.store(0);
    r.seen.store(0); r.lastSeenTick.store(0); r.on.store(active);
    SetSlotState(repl, kNotTarget);                         // our own view is never classified or probed as a stock one
    r.srv.store(repl, std::memory_order_release);
    g_replCount.store(slot + 1);
    ss->info.store(kTarget | ((uint32_t)slot << 8), std::memory_order_release);
    return slot;
}

SwapCounters Counters(int handle) {
    SwapCounters c;
    if (handle < 0 || handle >= g_replCount.load()) return c;
    const Repl& r = g_repl[handle];
    c.subst = r.subst.load();
    c.lastTick = r.lastTick.load();
    c.verified = r.verified.load();
    c.verifyMiss = r.verifyMiss.load();
    c.seen = r.seen.load();
    c.lastSeenTick = r.lastSeenTick.load();
    c.active = r.on.load();
    return c;
}

bool SetActive(int handle, bool on) {
    if (handle < 0 || handle >= g_replCount.load() || g_repl[handle].target != -1) return false;   // swap clients only, never a brand entry
    g_repl[handle].on.store(on, std::memory_order_release);
    return true;
}

bool ProbeArm(const ProbeSize* sizes, int n) {
    if (!g_hooksInstalled.load() || !sizes || n <= 0 || n > (int)kProbeMaxSizes) return false;
    ProbeClear();
    for (int i = 0; i < n; ++i) { g_probeSizes[i][0] = sizes[i].w; g_probeSizes[i][1] = sizes[i].h; }
    g_probeSizeCount = n;
    g_probeWinReq.store(0);
    g_probeArmed.store(true);
    return true;
}

void ProbeSetWindow(uint32_t window) { g_probeWinReq.store(window <= 32 ? window : 0); }

void ProbeFrame() {
    if (!g_probeArmed.load()) return;
    const uint32_t w = g_probeWinReq.load();
    if (w != g_probeFrameWin) { g_probeFrameWin = w; g_probeFramesIn = 0; }
    else ++g_probeFramesIn;
    // the first frames after a change may still show the previous script frame: not recorded
    g_probeRecording.store(g_probeFramesIn >= kProbeSkipFrames ? w : 0);
}

std::vector<ProbeCand> ProbeTake(uint32_t* overflow) {
    std::vector<ProbeCand> out;
    g_probeArmed.store(false);
    g_probeRecording.store(0);
    if (overflow) *overflow = g_probeOverflow.load();
    for (uint32_t i = 0; i < kProbeSlots; ++i) {
        ProbeEnt& e = g_probeTab[i];
        if (e.held) {
            ProbeCand c;
            c.srv = e.held;                                  // the reference goes to the caller
            c.w = e.w; c.h = e.h;
            c.seen = e.seen.load();
            c.binds = e.binds.load();
            out.push_back(c);
            e.held = nullptr;
        }
    }
    ProbeClear();
    return out;
}

}  // namespace swap

// ===========================================================================================================
// Test seam
// ===========================================================================================================
namespace testing {

uint64_t TailHash(int width, int levels, const std::vector<std::vector<uint8_t>>& mips) { return TailHashImpl(width, levels, mips); }

void EncodeBC1Block(const float rgb[16][3], uint8_t out[8]) { EncodeColour(rgb, out); }
void DecodeBC1Block(const uint8_t in[8], float rgb[16][3], bool* threeColour) { DecodeColour(in, false, rgb, threeColour); }

int CompositeChain(int width, int levels, int bytesPerBlock, int baseSize, const std::vector<Decal>& decals,
                   std::vector<std::vector<uint8_t>>& mips) {
    std::vector<DecalRt> rt;
    for (const Decal& d : decals) rt.push_back(BuildDecalRt(d.x, d.y, d.w, d.h, d.a8.data(), d.rgb));
    return CompositeImpl(width, levels, bytesPerBlock, baseSize, rt, mips);
}

void Reset() {
    Shutdown();
    for (uint32_t i = 0; i < kSlotCount; ++i) { g_slots[i].key.store(0); g_slots[i].info.store(0); }
    {
        std::lock_guard<std::mutex> lock(g_targetsMu);
        g_targets.clear();
        g_targetsReady.store(true);          // the built-in targets stay out: tests register their own
    }
    g_binds = 0; g_srvSeen = 0; g_candidates = 0; g_fingerprinted = 0; g_appliedTotal = 0; g_tableFull = 0; g_dropped = 0;
    g_learnLogged = 0;
}

bool AddTarget(const char* name, uint64_t tailHash, bool bc3, int baseSize, int levels, const std::vector<Decal>& decals) {
    std::lock_guard<std::mutex> lock(g_targetsMu);
    TargetRt r;
    r.name = name ? name : "test target"; r.hash = tailHash; r.bc3 = bc3; r.baseSize = baseSize; r.levels = levels;
    for (const Decal& d : decals) r.decals.push_back(BuildDecalRt(d.x, d.y, d.w, d.h, d.a8.data(), d.rgb));
    g_targets.push_back(std::move(r));
    g_targetsReady.store(true);
    return true;
}

bool InstallOn(ID3D11Device* dev, ID3D11DeviceContext* ctx) { return InstallHooks(dev, ctx); }
void Pump(ID3D11DeviceContext* ctx) { PumpImpl(ctx); }

}  // namespace testing

}  // namespace gage::game::brand
