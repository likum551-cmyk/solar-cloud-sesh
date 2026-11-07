// minimap_size.cpp: the native half of minimap_scale. See minimap_size.h; the pure rules, the pattern and the layout evidence are in
// libs/sync/minimap_size.h (tests/minimap_tests.h pins them, including the pattern against the decrypted 3889 dump).
//
// Self-contained like dlc_mount.cpp / pack_stream.cpp: its own image bounds and page-safe scan (hook.cpp keeps its scanner in an anonymous
// namespace). No SEH here: the engine is built with GCC, which has no __try; every read goes through VirtualQuery and every write through
// WriteGuarded (VirtualQuery, and VirtualProtect only if the page is not writable), then is read back.

#include "minimap_size.h"

#include "common/common.h"
#include "common/logger.h"
#include "sync/minimap_size.h"
#include "sync/ring_round.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gage::game::minimapsize {

using namespace gage::sync;

namespace {

// ---------------------------------------------------------------------------------------------
// Image bounds, readable runs, the main module's name
// ---------------------------------------------------------------------------------------------
struct Image { const uint8_t* base = nullptr; size_t size = 0; };

Image GameImage() {
    Image im;
    HMODULE m = GetModuleHandleW(nullptr);   // the game exe: this DLL runs inside its process
    if (!m) return im;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(m);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return im;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return im;
    im.base = b;
    im.size = nt->OptionalHeader.SizeOfImage;
    return im;
}

bool ProtReadable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool IsReadable(const void* p, size_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(p);
    const uint8_t* end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !ProtReadable(mbi.Protect))
            return false;
        a = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

struct Run { const uint8_t* p; size_t n; };

// The committed, readable stretches of [base, base+size), adjacent ones merged: the Arxan-packed image can hold guard or reserved pages.
std::vector<Run> ReadableRuns(const Image& im) {
    std::vector<Run> runs;
    const uint8_t* a = im.base;
    const uint8_t* end = im.base + im.size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi))) break;
        const uint8_t* rb = static_cast<const uint8_t*>(mbi.BaseAddress);
        const uint8_t* re = rb + mbi.RegionSize;
        if (re > end) re = end;
        if (mbi.State == MEM_COMMIT && ProtReadable(mbi.Protect) && re > a) {
            if (!runs.empty() && runs.back().p + runs.back().n == a)
                runs.back().n += (size_t)(re - a);
            else
                runs.push_back({ a, (size_t)(re - a) });
        }
        if (re <= a) break;
        a = re;
    }
    return runs;
}

// The layout for the main module: Legacy's GTA5.exe (FiveM's pattern, 3889) or GTA5_Enhanced.exe (its own pattern, minimap_size.h); null for anything else.
const MinimapLayout* LayoutForExe() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    const wchar_t* base = wcsrchr(path, L'\\');
    const wchar_t* name = base ? base + 1 : path;
    if (_wcsicmp(name, L"GTA5.exe") == 0) return &kMinimapLayoutLegacy;
    if (_wcsicmp(name, L"GTA5_Enhanced.exe") == 0) return &kMinimapLayoutEnhanced;
    return nullptr;
}

// Every position (up to 4) where the layout's pattern occurs in the readable runs (the matching itself is sync/minimap_size.h FindLayoutHits).
std::vector<const uint8_t*> FindHits(const MinimapLayout& lay, const std::vector<Run>& runs) {
    const uint8_t* hits[4] = {};
    int have = 0;
    for (const Run& r : runs) {
        have = FindLayoutHits(lay, r.p, r.n, hits, 4, have);
        if (have >= 4) break;
    }
    return std::vector<const uint8_t*>(hits, hits + have);
}

// A write into the game's data: VirtualQuery first (committed, not guarded, inside one region), VirtualProtect only when the page is not writable.
bool WriteGuarded(uint8_t* dst, const uint8_t* src, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(dst, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (dst + n > static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) return false;   // across two regions: not worth a partial write
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD old = 0;
    bool reprotected = false;
    if (!(mbi.Protect & writable)) {
        if (!VirtualProtect(dst, n, PAGE_READWRITE, &old)) return false;
        reprotected = true;
    }
    memcpy(dst, src, n);
    if (reprotected) {
        DWORD ignored = 0;
        VirtualProtect(dst, n, old, &ignored);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// State (script tick only)
// ---------------------------------------------------------------------------------------------
enum class Res { Idle, Searching, Ready, Refused };

Res          g_res = Res::Idle;
std::string  g_refusedWhy;
const MinimapLayout* g_layout = nullptr; // which build family's pattern found the table
uint8_t*     g_table = nullptr;          // element 0 of the game's table
size_t       g_tableRva = 0;
MinimapSizeGate g_gate;
MinimapTrio  g_lastWritten;              // what the last verified write put in the table (to tell what the game changed since)
float        g_scale = kMinimapScaleDefault;
std::string  g_src = "unset, default";
bool         g_configured = false;
bool         g_verified = false;         // the last write was read back identical
bool         g_written = false;          // TakeWritten()'s latch
bool         g_tableLogged = false;
int          g_attempts = 0;
double       g_nextAttemptMs = 0.0;
double       g_nextReadMs = 0.0;
double       g_waitLogMs = -1e18;
int          g_waitLogs = 0, g_rewriteLogs = 0;
// 0.5.4 ring_map_big: the big radar's slots (bigmap / bigmap_mask / bigmap_blur) mirror the minimap placement (sync::MirroredTrio).
bool            g_bigWanted = false;
MinimapSizeGate g_bigGate;
MinimapTrio     g_bigLastWritten;
bool            g_bigVerified = false;
int             g_bigLogs = 0;
constexpr int   kMaxBigLogs = 12;

constexpr int    kMaxAttempts = 6;       // the pattern not found yet: the code may not be decrypted; look again 5 s later, six times
constexpr double kRetryMs = 5000.0, kReadEveryMs = 250.0, kWaitLogEveryMs = 10000.0;
constexpr int    kMaxWaitLogs = 6, kMaxRewriteLogs = 12;

bool ReadTable(MinimapComp* out) {
    if (!g_table || !IsReadable(g_table, (size_t)kMinimapEntries * (size_t)kMinimapStride)) return false;
    for (int i = 0; i < kMinimapEntries; ++i)
        out[i] = DecodeMinimapComp(g_table + (size_t)i * (size_t)kMinimapStride);
    return true;
}

std::string Geo(const MinimapComp& c) {
    char b[120];
    std::snprintf(b, sizeof(b), "pos (%.4f, %.4f) size (%.6f, %.6f)", (double)c.posX, (double)c.posY, (double)c.sizeX, (double)c.sizeY);
    return b;
}

// Find the table once. Searching again later only when the pattern was not there at all (code not decrypted yet); every other failure is final.
void Resolve(double nowMs) {
    const Image im = GameImage();
    if (!im.base || !im.size) { g_res = Res::Refused; g_refusedWhy = "the game image could not be read"; return; }
    const MinimapLayout* layout = LayoutForExe();
    if (!layout) {
        g_res = Res::Refused;
        g_refusedWhy = "the game exe is neither GTA5.exe nor GTA5_Enhanced.exe: no minimap table layout for it (the radar stays as the game draws it)";
        return;
    }
    g_layout = layout;
    const ULONGLONG t0 = GetTickCount64();
    const std::vector<const uint8_t*> hits = FindHits(*layout, ReadableRuns(im));
    const unsigned long ms = (unsigned long)(GetTickCount64() - t0);
    if (hits.empty()) {
        ++g_attempts;
        if (g_attempts >= kMaxAttempts) {
            g_res = Res::Refused;
            g_refusedWhy = "the pattern was not found in the image in " + std::to_string(g_attempts) + " scans (a different build?)";
        } else {
            g_res = Res::Searching;
            g_nextAttemptMs = nowMs + kRetryMs;
            GLOGI("engine: minimap size (0.4.25): table pattern not found yet (scan %d of %d took %lu ms), looking again in %.0f s", g_attempts, kMaxAttempts, ms,
                  kRetryMs / 1000.0);
        }
        return;
    }
    if (hits.size() > 1) {
        g_res = Res::Refused;
        g_refusedWhy = "the pattern matched " + std::to_string(hits.size()) + " places; not guessing which is the minimap table";
        return;
    }
    const uint8_t* hit = hits[0];
    if (hit < im.base + layout->spanBefore || !IsReadable(hit - layout->spanBefore, (size_t)(layout->spanBefore + layout->spanAfter))) {
        g_res = Res::Refused;
        g_refusedWhy = "the code around the pattern hit is not readable";
        return;
    }
    const TableResolve r = ResolveMinimapTableFor(*layout, hit, im.base, im.size);
    if (!r.ok) { g_res = Res::Refused; g_refusedWhy = r.why; return; }
    uint8_t* table = const_cast<uint8_t*>(im.base) + r.tableOff;
    if (!IsReadable(table, (size_t)kMinimapEntries * (size_t)kMinimapStride)) {
        g_res = Res::Refused;
        g_refusedWhy = "the table address is not readable memory";
        return;
    }
    g_table = table;
    g_tableRva = r.tableOff;
    g_res = Res::Ready;
    const unsigned long hitRva = (unsigned long)(hit - im.base);
    if (layout == &kMinimapLayoutLegacy)
        GLOGI("engine: minimap size (0.4.25): table found by FiveM's pattern, unique: pattern at GTA5.exe+0x%lx (3889 dumps: 0x%lx), table at +0x%lx (3889 dumps: 0x%lx), "
              "the game's frontend.xml reader checked (%d instructions: stride 0x78, posX +100, posY +104, sizeX +108, sizeY +112, alignX +116, alignY +117); scan %lu ms",
              hitRva, (unsigned long)kMinimapHit3889, (unsigned long)r.tableOff, (unsigned long)kMinimapTable3889, (int)(sizeof(kLoaderChecks) / sizeof(kLoaderChecks[0])) + 1, ms);
    else
        GLOGI("engine: minimap size (0.4.26): table found by the %s loader pattern, unique: pattern at GTA5_Enhanced.exe+0x%lx (dump: 0x%lx), table at +0x%lx (dump: 0x%lx), "
              "the game's frontend.xml reader checked (%d instructions: stride 0x78, posX +100, posY +104, sizeX +108, sizeY +112, alignX +116, alignY +117); scan %lu ms",
              layout->label, hitRva, (unsigned long)layout->expectHit, (unsigned long)r.tableOff, (unsigned long)layout->expectTable,
              layout->nChecks + (layout->readerLea ? 1 : 0), ms);
}

// The whole table, once, the first time it holds names: the next iteration (other components, the real stock values) starts from this line.
void LogTableOnce(const MinimapComp* all) {
    if (g_tableLogged) return;
    int named = 0;
    for (int i = 0; i < kMinimapEntries; ++i)
        if (!all[i].name.empty()) ++named;
    if (!named) return;
    g_tableLogged = true;
    GLOGI("engine: minimap table (0.4.25): %d of %d slots named, as the game parsed common:/data/ui/frontend.xml:", named, kMinimapEntries);
    for (int i = 0; i < kMinimapEntries; ++i)
        if (!all[i].name.empty())
            GLOGI("engine: minimap table (0.4.25):   slot %d %s", i, FormatComp(all[i]).c_str());
}

// Write the target's four floats into each of the three entries, read them back, and put the old bytes back on any mismatch.
bool WriteTarget(const MinimapTrio& target, std::string& err, const char* const* names = kMinimapRoleNames) {
    uint8_t snap[3][16];
    uint8_t* at[3];
    for (int r = 0; r < 3; ++r) {
        at[r] = g_table + (size_t)target.index[r] * (size_t)kMinimapStride + kOffPosX;
        if (!IsReadable(at[r], 16)) { err = "an entry is not readable"; return false; }
        memcpy(snap[r], at[r], 16);
    }
    bool ok = true;
    for (int r = 0; r < 3 && ok; ++r) {
        uint8_t tmp[kMinimapStride] = {};
        EncodeMinimapGeometry(tmp, target.c[r]);
        ok = WriteGuarded(at[r], tmp + kOffPosX, 16);
        if (!ok) err = std::string("the write to ") + RoleName(r, names) + " was refused";
    }
    if (ok) {
        for (int r = 0; r < 3 && ok; ++r) {
            const MinimapComp got = DecodeMinimapComp(g_table + (size_t)target.index[r] * (size_t)kMinimapStride);
            ok = SameComp(got, target.c[r]);
            if (!ok) err = std::string("the read-back of ") + RoleName(r, names) + " is " + Geo(got) + ", written " + Geo(target.c[r]);
        }
    }
    if (!ok)
        for (int r = 0; r < 3; ++r) WriteGuarded(at[r], snap[r], 16);   // best effort; the gate retries or gives up
    return ok;
}

void LogApplied(const MinimapTrio& before, const MinimapTrio& after) {
    std::string line;
    for (int r = 0; r < 3; ++r) {
        line += std::string(r ? "; " : "") + RoleName(r) + " " + before.c[r].alignX + "/" + before.c[r].alignY + " " + Geo(before.c[r]) + " -> " + Geo(after.c[r]);
    }
    if (g_gate.Placed()) {
        const MinimapPlacement& p = g_gate.Placement();
        GLOGI("engine: minimap size (0.5.2 ring_map): placement (%s: per-axis scale %.4f x %.4f about (%.4f, %.4f), moved to (%.4f, %.4f), blur = the visible "
              "square pos (%.4f, %.4f) size (%.4f, %.4f)) APPLIED to the game's table (slots %d, %d, %d), read back identical: %s; the radar movie is flipped "
              "(held) so the minimap component is re-laid out",
              g_src.c_str(), (double)p.sx, (double)p.sy, (double)p.ax, (double)p.ay, (double)p.tx, (double)p.ty, (double)p.vx, (double)p.vy, (double)p.vw,
              (double)p.vh, after.index[0], after.index[1], after.index[2], line.c_str());
        return;
    }
    GLOGI("engine: minimap size (0.4.25): scale %.2f (%s) APPLIED to the game's table (slots %d, %d, %d), read back identical: %s; the vitals strip follows this "
          "scale; the radar movie is flipped once so the minimap component is re-read",
          (double)g_gate.Scale(), g_src.c_str(), after.index[0], after.index[1], after.index[2], line.c_str());
}

void StepMinimap(const MinimapComp* all, bool readable, double nowMs);
void StepBig(const MinimapComp* all, bool readable, double nowMs);

} // namespace

void SetBigMirror(bool on) {
    g_bigWanted = on;
}

void ConfigurePlacement(const MinimapPlacement& p, const char* sourceName) {
    const bool first = !g_configured || !g_gate.Placed();
    const bool same = g_gate.Placed() && g_gate.Placement() == p;
    g_src = sourceName ? sourceName : "";
    g_configured = true;
    if (same) return;
    g_gate.ConfigurePlacement(p);
    if (first)
        GLOGI("engine: minimap size (0.5.2 ring_map): the radar's visible map goes into the vitals ring (%s) once the game's table is populated", g_src.c_str());
}

bool PlacementApplied() {
    const bool mini = g_res == Res::Ready && g_verified && g_gate.Placed() && g_gate.PhaseNow() == MinimapSizeGate::Phase::Applied;
    // 0.5.4 ring_map_big: the big radar's slots too, unless they could not be written at all (then the minimap's alone, as 0.5.3)
    const bool big = !g_bigWanted || g_bigGate.PhaseNow() == MinimapSizeGate::Phase::GaveUp ||
                     (g_bigVerified && g_bigGate.Mirrored() && g_bigGate.PhaseNow() == MinimapSizeGate::Phase::Applied);
    return mini && big;
}

void Configure(float scale, const char* sourceName) {
    g_scale = ClampRadarScale(scale);
    g_src = sourceName ? sourceName : "";
    const bool first = !g_configured;
    g_configured = true;
    g_gate.Configure(g_scale);
    if (first || g_scale < 1.0f || g_gate.HasOriginal())
        GLOGI("engine: minimap size (0.4.25): minimap_scale=%.2f (%s)%s", (double)g_scale, g_src.c_str(),
              g_scale >= 1.0f ? (g_gate.HasOriginal() ? ": the game's own sizes are written back" : ": untouched, the game's own size")
                              : ": the radar's three components are scaled about the bottom-left corner once the game's table is populated");
}

void Tick(double nowMs) {
    if (!g_configured || g_res == Res::Refused) return;
    if (g_gate.PhaseNow() == MinimapSizeGate::Phase::Off) return;   // untouched (or restored): no scan, no read
    if (g_gate.PhaseNow() == MinimapSizeGate::Phase::GaveUp) return;
    if (g_res == Res::Idle || (g_res == Res::Searching && nowMs >= g_nextAttemptMs)) {
        Resolve(nowMs);
        if (g_res == Res::Refused)
            GLOGW("engine: minimap size (0.4.25): NOT applied, the radar stays as the game draws it: %s", g_refusedWhy.c_str());
    }
    if (g_res != Res::Ready) return;
    if (nowMs < g_nextReadMs) return;
    g_nextReadMs = nowMs + kReadEveryMs;

    MinimapComp all[kMinimapEntries];
    const bool readable = ReadTable(all);
    if (readable) LogTableOnce(all);
    StepMinimap(all, readable, nowMs);
    StepBig(all, readable, nowMs);   // after the minimap step: a placement written this tick is mirrored in the same tick (one flip for both)
}

namespace {

void StepMinimap(const MinimapComp* all, bool readable, double nowMs) {
    const MinimapTrio cur = readable ? PickTrio(all, kMinimapEntries) : MinimapTrio();
    const MinimapTrio prevOrig = g_gate.Original();   // before Next(): it may adopt a new stock value
    const MinimapSizeGate::Act act = g_gate.Next(cur, readable, nowMs);

    if (act == MinimapSizeGate::Act::None) {
        const auto ph = g_gate.PhaseNow();
        if (ph == MinimapSizeGate::Phase::Waiting && g_waitLogs < kMaxWaitLogs && nowMs - g_waitLogMs >= kWaitLogEveryMs) {
            g_waitLogMs = nowMs;
            ++g_waitLogs;
            GLOGI("engine: minimap size (0.4.25): waiting for the game's table to fill: %s", g_gate.Why().c_str());
        } else if (ph == MinimapSizeGate::Phase::GaveUp) {
            g_verified = false;
            GLOGW("engine: minimap size (0.4.25): GAVE UP, the table is left as it is: %s", g_gate.Why().c_str());
        }
        return;
    }

    std::string err;
    const MinimapTrio target = g_gate.Target();
    if (!WriteTarget(target, err)) {
        g_verified = false;
        g_gate.WriteFailed();
        GLOGW("engine: minimap size (0.4.25): the write failed and the old bytes were put back: %s (failures %u)", err.c_str(), g_gate.Counters().writeFailures);
        if (g_gate.PhaseNow() == MinimapSizeGate::Phase::GaveUp)
            GLOGW("engine: minimap size (0.4.25): GAVE UP, the table is left as it is: %s", g_gate.Why().c_str());
        return;
    }
    g_verified = true;
    g_written = true;
    if (act == MinimapSizeGate::Act::Apply) {
        LogApplied(g_gate.Original(), target);
    } else if (g_rewriteLogs < kMaxRewriteLogs) {
        ++g_rewriteLogs;
        // What the game did since our last write: compare the table with what we wrote then (not with the gate's new target).
        std::string what;
        for (int r = 0; g_lastWritten.AllFound() && r < 3; ++r)
            if (!SameComp(cur.c[r], g_lastWritten.c[r]))
                what += std::string(what.empty() ? "" : ", ") + RoleName(r) + (SameComp(cur.c[r], prevOrig.c[r]) ? " (back to the stock size)" : " (a new value, adopted as the stock size)");
        GLOGI("engine: minimap size (0.4.25): re-asserted scale %.2f (%s) (re-assert %u of at most %u, adopted %u); now %s | %s | %s", (double)g_gate.Scale(),
              what.empty() ? "the sizes were asked for again: a new factor, or a retry after a failed write" : ("the game changed the table: " + what).c_str(),
              g_gate.Counters().reasserts, (unsigned)MinimapSizeGate::kMaxReasserts, g_gate.Counters().adopted, Geo(target.c[0]).c_str(), Geo(target.c[1]).c_str(),
              Geo(target.c[2]).c_str());
    }
    g_lastWritten = target;
}

// 0.5.4 ring_map_big: once the minimap placement is in the table, the big radar's three slots get the same geometry (MirroredTrio), through their
// own gate (wait for the slots, apply, re-assert what the game rewrites, give up after the same bounds). Its write sets the same TakeWritten latch.
void StepBig(const MinimapComp* all, bool readable, double nowMs) {
    if (g_bigGate.PhaseNow() == MinimapSizeGate::Phase::GaveUp) return;
    if (g_bigWanted && g_gate.Placed()) {
        if (!g_verified || g_gate.PhaseNow() != MinimapSizeGate::Phase::Applied) return;   // the minimap placement is not in the table (yet)
        g_bigGate.ConfigureMirror(g_gate.Target());   // the same geometry is not written again
    } else if (g_bigGate.Mirrored()) {
        g_bigGate.Configure(1.0f);   // ring_map or ring_map_big switched off: the big radar's stock values go back once, from the saved originals
    } else if (!g_bigGate.HasOriginal()) {
        return;                      // never mirrored: nothing of ours in the big slots
    }
    const MinimapTrio cur = readable ? PickTrio(all, kMinimapEntries, kBigmapRoleNames) : MinimapTrio();
    const MinimapSizeGate::Act act = g_bigGate.Next(cur, readable, nowMs);
    if (act == MinimapSizeGate::Act::None) {
        if (g_bigGate.PhaseNow() == MinimapSizeGate::Phase::GaveUp) {
            g_bigVerified = false;
            GLOGW("engine: ring map round 2 (0.5.4): the big radar's slots GAVE UP, left as they are (the ring map waits for the minimap slots only): %s",
                  g_bigGate.Why().c_str());
        }
        return;
    }
    std::string err;
    const MinimapTrio target = g_bigGate.Target();
    if (!WriteTarget(target, err, kBigmapRoleNames)) {
        g_bigVerified = false;
        g_bigGate.WriteFailed();
        GLOGW("engine: ring map round 2 (0.5.4): the big radar's write failed and the old bytes were put back: %s (failures %u)", err.c_str(),
              g_bigGate.Counters().writeFailures);
        return;
    }
    g_bigVerified = true;
    g_written = true;
    if (g_bigLogs < kMaxBigLogs) {
        ++g_bigLogs;
        std::string line;
        for (int r = 0; r < 3; ++r)
            line += std::string(r ? "; " : "") + RoleName(r, kBigmapRoleNames) + " (slot " + std::to_string(target.index[r]) + ") " +
                    Geo(g_bigGate.Original().c[r]) + " -> " + Geo(target.c[r]);
        GLOGI("engine: ring map round 2 (0.5.4): the ring placement %s into the big radar's slots, read back identical: %s",
              !g_bigGate.Mirrored() ? "taken back (stock values)" : act == MinimapSizeGate::Act::Apply ? "MIRRORED" : "re-asserted", line.c_str());
    }
    g_bigLastWritten = target;
}

} // namespace

float AppliedScale() {
    if (g_res != Res::Ready || !g_verified) return 1.0f;
    return g_gate.PhaseNow() == MinimapSizeGate::Phase::Applied ? g_gate.Scale() : 1.0f;
}

bool TakeWritten() {
    const bool w = g_written;
    g_written = false;
    return w;
}

std::string Status() {
    char b[260];
    if (!g_configured) return "size: not configured";
    if (g_res == Res::Refused) return "size: NOT applied (" + g_refusedWhy + ")";
    const MinimapSizeGate::Stats& s = g_gate.Counters();
    switch (g_gate.PhaseNow()) {
    case MinimapSizeGate::Phase::Off:
        return g_scale >= 1.0f ? "size: untouched (minimap_scale=1)" : "size: off";
    case MinimapSizeGate::Phase::Waiting:
        return g_res == Res::Ready ? "size: waiting for the table to fill (" + g_gate.Why() + ")" : "size: looking for the table";
    case MinimapSizeGate::Phase::Applied: {
        if (g_gate.Placed())
            std::snprintf(b, sizeof(b), "size: ring placement (0.5.2) %s, checks %u, re-asserts %u, adopted %u, write failures %u",
                          g_verified ? "in the table (read back)" : "NOT verified", s.checks, s.reasserts, s.adopted, s.writeFailures);
        else
            std::snprintf(b, sizeof(b), "size: scale %.2f %s, checks %u, re-asserts %u, adopted %u, write failures %u", (double)g_gate.Scale(),
                          g_verified ? "in the table (read back)" : "NOT verified", s.checks, s.reasserts, s.adopted, s.writeFailures);
        // The live table, read now: the minimap frame and its mask are what the radar is drawn from.
        MinimapComp all[kMinimapEntries];
        std::string live;
        if (ReadTable(all)) {
            const MinimapTrio t = PickTrio(all, kMinimapEntries);
            if (t.AllFound()) live = std::string("; table now: ") + RoleName(0) + " " + Geo(t.c[0]) + ", " + RoleName(1) + " " + Geo(t.c[1]);
            if (g_bigWanted) {
                const MinimapTrio bt = PickTrio(all, kMinimapEntries, kBigmapRoleNames);
                const char* ph = g_bigGate.PhaseNow() == MinimapSizeGate::Phase::Applied ? (g_bigVerified ? "mirrored (read back)" : "NOT verified")
                               : g_bigGate.PhaseNow() == MinimapSizeGate::Phase::GaveUp ? "GAVE UP"
                               : g_bigGate.PhaseNow() == MinimapSizeGate::Phase::Waiting ? "waiting" : "not yet";
                live += std::string("; big radar (0.5.4 ring_map_big) ") + ph + ", re-asserts " + std::to_string(g_bigGate.Counters().reasserts);
                if (bt.AllFound()) live += std::string(", bigmap ") + Geo(bt.c[0]);
            }
        }
        return std::string(b) + live;
    }
    case MinimapSizeGate::Phase::GaveUp:
        return "size: GAVE UP (" + g_gate.Why() + ")";
    }
    return "size: ?";
}

// ---------------------------------------------------------------------------------------------
// 0.5.5 ring_map_blip_clip: FiveM's minimapIsRect (sync::ringround::kClipLayoutLegacy)
// ---------------------------------------------------------------------------------------------
namespace {
enum class ClipRes { Unresolved, Ready, Refused };
ClipRes     g_clipRes = ClipRes::Unresolved;
std::string g_clipWhy;
uint8_t*    g_clipFlag = nullptr;
size_t      g_clipRva = 0;
uint32_t    g_clipWrites = 0, g_clipWriteFails = 0;
int         g_clipWanted = -1;     // the value last asked for (-1 = never)

void ResolveClip() {
    const Image im = GameImage();
    if (!im.base || !im.size) { g_clipRes = ClipRes::Refused; g_clipWhy = "the game image could not be read"; return; }
    if (LayoutForExe() != &kMinimapLayoutLegacy) {
        g_clipRes = ClipRes::Refused;
        g_clipWhy = "not Legacy GTA5.exe: the flag's pattern is pinned for 3889 only";
        return;
    }
    const sync::MinimapLayout& L = sync::ringround::kClipLayoutLegacy;
    const ULONGLONG t0 = GetTickCount64();
    const uint8_t* hits[4] = {};
    int have = 0;
    for (const Run& r : ReadableRuns(im)) {
        have = FindLayoutHits(L, r.p, r.n, hits, 4, have);
        if (have >= 4) break;
    }
    const unsigned long ms = (unsigned long)(GetTickCount64() - t0);
    if (have != 1) {
        g_clipRes = ClipRes::Refused;
        g_clipWhy = "the pattern matched " + std::to_string(have) + " places (expected exactly 1)";
        return;
    }
    const uint8_t* hit = hits[0];
    if (hit < im.base + L.spanBefore || !IsReadable(hit - L.spanBefore, (size_t)(L.spanBefore + L.spanAfter))) {
        g_clipRes = ClipRes::Refused;
        g_clipWhy = "the code around the hit is not readable";
        return;
    }
    const sync::ringround::ClipResolve r = sync::ringround::ResolveClipFlag(hit, im.base, im.size);
    if (!r.ok) { g_clipRes = ClipRes::Refused; g_clipWhy = r.why; return; }
    uint8_t* flag = const_cast<uint8_t*>(im.base) + r.flagOff;
    if (!IsReadable(flag, 1) || (*flag != 0 && *flag != 1)) {
        g_clipRes = ClipRes::Refused;
        g_clipWhy = "the flag is not a readable bool";
        return;
    }
    g_clipFlag = flag;
    g_clipRva = r.flagOff;
    g_clipRes = ClipRes::Ready;
    GLOGI("engine: ring map round mask (0.5.5): blip clip flag found by FiveM's pattern, unique: pattern at GTA5.exe+0x%lx (3889 dumps: 0x%lx), flag at +0x%lx "
          "(3889 dumps: 0x%lx), holds %u (1 = rectangle, the game's default); scan %lu ms",
          (unsigned long)(hit - im.base), (unsigned long)sync::ringround::kClipHit3889, (unsigned long)r.flagOff,
          (unsigned long)sync::ringround::kClipFlag3889, (unsigned)*flag, ms);
}
} // namespace

bool SetBlipClipRound(bool round) {
    if (g_clipRes == ClipRes::Unresolved) {
        if (!round) return false;                  // nothing to put back before we ever asked for round
        ResolveClip();
        if (g_clipRes == ClipRes::Refused) GLOGW("engine: ring map round mask (0.5.5): blip clip flag NOT used: %s", g_clipWhy.c_str());
    }
    if (g_clipRes != ClipRes::Ready) return false;
    const uint8_t want = round ? 0 : 1;
    g_clipWanted = want;
    if (!IsReadable(g_clipFlag, 1)) return false;
    if (*g_clipFlag == want) return true;
    if (g_clipWriteFails >= 3) return false;
    if (!WriteGuarded(g_clipFlag, &want, 1) || *g_clipFlag != want) {
        ++g_clipWriteFails;
        GLOGW("engine: ring map round mask (0.5.5): blip clip flag write of %u not read back (%u failures)", (unsigned)want, g_clipWriteFails);
        return false;
    }
    ++g_clipWrites;
    return true;
}

std::string BlipClipStatus() {
    char b[200];
    if (g_clipRes == ClipRes::Unresolved) return "blip clip: not looked up yet (only once the map is round)";
    if (g_clipRes == ClipRes::Refused) return "blip clip: refused (" + g_clipWhy + ")";
    const int now = IsReadable(g_clipFlag, 1) ? (int)*g_clipFlag : -1;
    std::snprintf(b, sizeof(b), "blip clip: flag +0x%lx holds %d (%s), %u write(s), %u failure(s)", (unsigned long)g_clipRva, now,
                  now == 0 ? "ROUND: off-radar blips clamp to the circle" : now == 1 ? "rectangle, the game's default" : "unreadable", g_clipWrites,
                  g_clipWriteFails);
    return b;
}

} // namespace gage::game::minimapsize
