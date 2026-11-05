// pack_stream.cpp: run-time mount of one add-on DLC pack through the game's own ExtraContentManager.
// See pack_stream.h for the flow, the registry switches and the two things the first live run decides.
//
// Every game address is resolved here by a byte pattern taken from the clean-room 3889 analysis
// (docs/content-streaming/recon-3889.md, p2-signatures.md). The scanner, image bounds and pattern
// syntax are kept self-contained (the same style as dlc_mount.cpp), so only hook.h (World) and the
// logger are shared with the rest of the engine.

#include "pack_stream.h"

#include "common/common.h"
#include "common/logger.h"
#include "common/registry.h"

#include <windows.h>

#include "handling_tune.h"
#include "pack_enh.h"
#include "pack_enh_mount_core.h"
#include "pack_meta.h"
#include "pack_profile.h"
#include "pack_scan.h"
#include "pack_win.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gage::game::content {

namespace {

// ---------------------------------------------------------------------------------------------
// Constants from the 3889 analysis (all RVAs / offsets confirmed in p2-signatures.md).
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kRpf7Magic  = 0x52504637u;   // 'RPF7'
constexpr uint32_t kEncNone    = 0x00000000u;
constexpr uint32_t kEncOpen    = 0x4E45504Fu;   // 'OPEN' (keyless; retail REJECTS, no patch here)
constexpr uint32_t kEncAes      = 0x0FFFFFF9u;   // AES-256-ECB TOC (operator-sealed; game reads natively)
constexpr uint32_t kEncNg       = 0x0FEFFFFFu;   // NG (real DRM; game reads natively)

constexpr uint32_t kGroupEarlyOn = 0x6f8bd408u;  // joaat("early_on")  -- what the boot loop runs first
constexpr uint32_t kGroupStartup = 0x5bf6578fu;  // joaat("startup")

// Everything build-specific (patterns, the manager / DlcEntry layout, single RVAs) is in pack_profile.h, selected
// by the main module's file name (BuildProfile's rule). This file's mount chain only runs on the Legacy profile
// (Enabled() refuses any other exe), whose values are the ones this file used to hold inline:
//   mgr vtable RVA 0x1a9c508 (from the manager ctor, p2 section 5); mgr -> array +0x28, WORD count +0x30, DWORD
//   generation +0x178 (edx to Setup2/MountGate); DlcEntry stride 0xf0, DWORD name hash +0x60 (the RunPackGroup key),
//   path atString +0xb8 (Mount loads it as a QWORD ptr; length WORD +0xc0, capacity WORD +0xc2; Mount ignores +0xb8
//   while the length is 0), DWORD device type +0xc8 (2 = packfile, Mount 0x94307c -> fiPackfile::Open, the crypto
//   path; 1 = relative/folder, fiDeviceRelative over +0xb8), DWORD setup state +0xd0 (0 fresh, 1 parsed, 2 mounted).
// Diagnostic probe (pack_probe=1): &ASSET (RVA 0x1e2ee90) and its mount/file-exists calls, used to mount the pack
// under "extra:/" WITHOUT DlcSetup2's teardown and ask the game which pack files are visible.

// 3889 hash of IS_MODEL_IN_CDIMAGE; World::CallNative maps public hashes and falls back to the hash as given.
constexpr uint64_t kNativeIsModelInCdimage = 0x35B9E0803292B641ULL;

constexpr ULONGLONG kModelWaitMs = 15000;        // after the change set, how long to wait for the model
constexpr ULONGLONG kMgrWaitMs   = 10000;        // how long to wait for the manager singleton to appear

// ---------------------------------------------------------------------------------------------
// Signatures (pack_profile.h). Each pattern is a function entry (address = the match itself). The singleton
// global is read out of the RunPackGroup body (a "48 8B 0D disp32" at match+13), not hard-coded.
// ---------------------------------------------------------------------------------------------
using scan::Run;
using scan::ParsePat;
using scan::CountPat;
using win::Image;
using win::GameImage;
using win::ProtReadable;
using win::IsReadable;
using win::ReadableRuns;

// The profile of the running exe, picked once by the main module's file name (the BuildProfile rule). The mount
// chain below only ever runs on the Legacy profile: Enabled() refuses every other exe.
const PackProfile& Prof() {
    static const PackProfile* p = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* base = path;
        if (n && n < MAX_PATH)
            for (const wchar_t* s = path; *s; ++s)
                if (*s == L'\\' || *s == L'/') base = s + 1;
        return &SelectPackProfile(base);
    }();
    return *p;
}

enum SigId : int { kAddDlcPack, kSetup2, kMountGate, kRunPackGroup, kSigCount };

const PackSig& SigOf(int i) {
    const PackProfile& p = Prof();
    return i == kAddDlcPack ? p.addDlcPack : i == kSetup2 ? p.dlcSetup2 : i == kMountGate ? p.mountGate : p.runPackGroup;
}

// Diagnostic-only signatures, resolved lazily and independently of the four above (a probe-sig miss must
// never disable the normal mount). Mount(entry, gen, mountPoint) opens+mounts the archive and returns 0 on
// success; PushFolder/Exists/PopFolder are the fiAssetManager calls DlcSetup2 itself uses to test setup2.xml.
enum ProbeSigId : int { kMount, kPushFolder, kExists, kPopFolder, kProbeSigCount };

const PackSig& ProbeSigOf(int i) {
    const PackProfile& p = Prof();
    return i == kMount ? p.mount : i == kPushFolder ? p.pushFolder : i == kExists ? p.exists : p.popFolder;
}

uintptr_t g_probeAddr[kProbeSigCount] = {};
bool      g_probeResolvedOnce = false;
bool      g_probeReady        = false;

// Loose model files (folder mode, <pack>/stream/*.yft|ytd|ydr|ydd). GTA's own "register a loose file with
// the streaming system" routine (3889 static RE 2026-11-05, clean code): it resolves the file's device; a
// plain OS path lands on the local device, which is not a collection (vt+0x148 = false) and reports image 0
// (vt+0x168 = 0), so the file goes to image 0 = the game's RawStreamer collection (vtable RVA 0x1b7d5e0),
// whose +0x1a8 appends the path to its own file list and returns the file index. The handle is
// (0 << 16) | index, and 0x169e5e0 picks the streaming module from the extension (yft -> FragmentStore,
// ytd -> TxdStore), adds a slot named after the file's base name and writes the handle into the
// streaming-info array. The RawStreamer reads size and RSC7 page flags from the file itself on first use,
// so no RPF, no packfile open and no encryption gate is involved.
//   int* RegisterStreamFile(int* outGlobalIndex /*rcx*/, const char* path /*rdx*/, bool /*r8b, both
//        game callers pass 1*/, const char* name /*r9: "jesko.yft"; null = path*/,
//        bool fatalIfMissing /*[rsp+0x28]: 1 = the game's fatal error, so always 0 here*/,
//        bool allowOverride /*[rsp+0x30]: re-point an object that already has a handle*/)
// The pattern is Prof().regStream (RVA 0x169e4d4, unique in both 3889 dumps).
using RegStreamFileFn = int* (*)(int* out, const char* path, uint8_t one, const char* name,
                                 uint8_t fatalIfMissing, uint8_t allowOverride);
// Prof().streamInfoMgrRva: strStreamingInfoManager, [+0] 8-byte (handle, flags) array, [+8] count (0x2ea23d0).
// Prof().imageRegistryRva: fiCollection* per image index (0x2c67400).
// Prof().rawStreamerVtRva: image 0's collection (0x1b7d5e0, live fidiag 2026-11-02).

// ---------------------------------------------------------------------------------------------
// Image bounds, readable-page runs and the pattern scan live in pack_win.h / pack_scan.h (shared with pack_enh.cpp
// and tests/pack_profile_test.cpp); the old inline copies were moved there unchanged.
// ---------------------------------------------------------------------------------------------
void DumpHex(const char* label, const uint8_t* p, size_t n) {
    static const char H[] = "0123456789ABCDEF";
    for (size_t off = 0; off < n; off += 32) {
        const size_t take = (n - off < 32) ? (n - off) : 32;
        char hex[32 * 3 + 1];
        size_t k = 0;
        for (size_t i = 0; i < take; ++i) {
            hex[k++] = H[p[off + i] >> 4];
            hex[k++] = H[p[off + i] & 0xF];
            hex[k++] = ' ';
        }
        hex[k] = '\0';
        GLOGI("hook: [packstream]   %-14s +0x%llx: %s", label,
              (unsigned long long)(uintptr_t)(p + off), hex);
    }
}

// ---------------------------------------------------------------------------------------------
// Resolved addresses + the manager singleton (read out of the RunPackGroup body).
// ---------------------------------------------------------------------------------------------
std::mutex        g_mu;
uintptr_t         g_addr[kSigCount] = {};
bool              g_ok[kSigCount]   = {};
uintptr_t         g_singleton       = 0;   // &(ExtraContentManager*) : base + 0x1ec1eb0
uintptr_t         g_strAppend       = 0;   // atString::Append(atString*, const char*, int) : base + 0x137aed8 on 3889
bool              g_resolvedOnce    = false;
bool              g_resolveReady    = false;  // every signature + the singleton pointer resolved

// Resolve all four function signatures and the singleton global address. Logs each one. Returns whether
// every required piece resolved uniquely.
bool ResolveAll() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_resolvedOnce) return g_resolveReady;
    g_resolvedOnce = true;

    const Image im = GameImage();
    if (!im.base || !im.size) {
        GLOGW("hook: [packstream] cannot read the game image bounds");
        return false;
    }
    const std::vector<Run> runs = ReadableRuns(im);
    GLOGI("hook: [packstream] resolving %d signatures over %u readable run(s) (@0x%llx size=%u)",
          (int)kSigCount, (unsigned)runs.size(), (unsigned long long)(uintptr_t)im.base, (unsigned)im.size);

    bool all = true;
    for (int i = 0; i < kSigCount; ++i) {
        const uint8_t* first = nullptr;
        if (!SigOf(i).pat) {   // a profile without this pattern (never the Legacy one): nothing to scan with
            GLOGW("hook: [packstream]   %-14s no pattern for this build", SigOf(i).name ? SigOf(i).name : "?");
            all = false;
            continue;
        }
        const int c = CountPat(runs, SigOf(i).pat, &first);
        if (c < 0) {
            GLOGW("hook: [packstream]   %-14s BAD PATTERN", SigOf(i).name);
            all = false;
        } else if (c == 0) {
            GLOGW("hook: [packstream]   %-14s missing (0)   (%s)", SigOf(i).name, SigOf(i).source);
            all = false;
        } else if (c > 1) {
            GLOGW("hook: [packstream]   %-14s ambiguous (%d, first @ +0x%llx); not used", SigOf(i).name, c,
                  (unsigned long long)(uintptr_t)(first - im.base));
            all = false;
        } else {
            g_addr[i] = (uintptr_t)first;
            g_ok[i] = true;
            GLOGI("hook: [packstream]   %-14s UNIQUE @ +0x%llx   (%s)", SigOf(i).name,
                  (unsigned long long)(uintptr_t)(first - im.base), SigOf(i).source);
            if (IsReadable(first, 16)) DumpHex("entry", first, 16);
        }
    }

    // The manager singleton: "48 8B 0D disp32" at RunPackGroup+13 => G = (match+20) + int32(disp @ +16).
    if (g_ok[kRunPackGroup]) {
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(g_addr[kRunPackGroup]);
        if (IsReadable(fn + 13, 7) && fn[13] == 0x48 && fn[14] == 0x8B && fn[15] == 0x0D) {
            int32_t disp = 0;
            memcpy(&disp, fn + 16, 4);
            const uintptr_t g = (uintptr_t)(fn + 20) + (intptr_t)disp;
            if (g >= (uintptr_t)im.base && g < (uintptr_t)im.base + im.size) {
                g_singleton = g;
                GLOGI("hook: [packstream]   ExtraContentManager* global @ +0x%llx (expect 0x1ec1eb0)",
                      (unsigned long long)(g - (uintptr_t)im.base));
            } else {
                GLOGW("hook: [packstream]   manager-global displacement points outside the image; not used");
                all = false;
            }
        } else {
            GLOGW("hook: [packstream]   RunPackGroup prologue is not the expected mov rcx,[rip+x]; manager global not read");
            all = false;
        }
    } else {
        all = false;
    }

    // The game's atString append (0x137aed8 on 3889), read from AddDlcPack's own call to it at +0xa3:
    // "48 8D 4D D8" (lea rcx,[rbp-0x28]) then "E8 rel32"; its body starts "45 85 C0 0F 84" (test r8d,r8d; je).
    // Folder mode rewrites the entry's path with it, so the string stays in game-owned memory (see MountSequence).
    if (g_ok[kAddDlcPack]) {
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(g_addr[kAddDlcPack]);
        const uint8_t* site = fn + 0xa3;
        if (IsReadable(site, 9) && site[0] == 0x48 && site[1] == 0x8D && site[2] == 0x4D && site[3] == 0xD8 && site[4] == 0xE8) {
            int32_t rel = 0;
            memcpy(&rel, site + 5, 4);
            const uint8_t* t = site + 9 + rel;
            if ((uintptr_t)t >= (uintptr_t)im.base && (uintptr_t)t < (uintptr_t)im.base + im.size && IsReadable(t, 5) &&
                t[0] == 0x45 && t[1] == 0x85 && t[2] == 0xC0 && t[3] == 0x0F && t[4] == 0x84) {
                g_strAppend = (uintptr_t)t;
                GLOGI("hook: [packstream]   atString append @ +0x%llx (from AddDlcPack+0xa3; expect 0x137aed8)",
                      (unsigned long long)(g_strAppend - (uintptr_t)im.base));
            }
        }
        if (!g_strAppend) GLOGW("hook: [packstream]   atString append not found at AddDlcPack+0xa3; folder-mode packs are refused");
    }

    g_resolveReady = all;
    GLOGI("hook: [packstream] signature resolution %s", all ? "COMPLETE" : "INCOMPLETE (mount will not run)");
    return all;
}

// ---------------------------------------------------------------------------------------------
// The DLC vehicle model-info store (0.4.16; 3889 static RE 2026-11-05, clean code).
//
// Every vehicles.meta item that is not yet a model takes the next CVehicleModelInfo (0x5a0 bytes) from a
// FIXED array: 0x605794 looks the array up by key (0xf000 for DLC content) in the vehicle factory's map and
// does `slot = base + count++ * 0x5a0` with no bounds check, then calls the slot's vtable. The array is
// created once, at session init (0x632610), with capacity = gameconfig MaxExtraVehicleModelInfos (CGameConfig
// +0x324), the only reader of that field. Rockstar's own DLC cars almost fill it, so the 4th served pack
// (veh_diablosv after jesko, rs6, bmwm5) wrote past the end: live 0.4.15, access violation at +0x6057e1
// `call [rax+0x38]` on a page-aligned slot just past the array. Single-player add-on cars need a modded
// gameconfig.xml for the same reason; we never write to the game install, so PrepareVehicleStore raises the
// in-memory limit before the game creates the array (the engine attaches before the Story session it boots
// with skip_landing), and MountSequence refuses a pack that would not fit instead of letting it crash.
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kDlcStoreKey = 0xf000u;       // the DLC key 0x64191c passes to the vehicles.meta loader
constexpr long     kVehStoreExtraDefault = 256;  // extra slots (x 0x5a0 bytes = 360 KB); veh_store_extra overrides

// The pattern is Prof().vehStore (RVA 0x632610, inside the model-info init 0x631f24 case 8: the three key-0xf000
// stores); its operand positions are Prof().vsCfg / vsTbl / vsLimitAt / vsSlotAt.

// The array header the factory map points at: 0x64ffac writes base and capacity, 0x605794 bumps the count.
struct StoreHdr { char* base; uint16_t count; uint16_t cap; };

std::mutex g_vehStoreMu;
uintptr_t  g_cfgGlobal   = 0;      // &(CGameConfig*), base + 0x2e9e508; the model-info limits are in *(+0x18)
uintptr_t  g_miGlobal    = 0;      // &(model-info factory table), base + 0x1ec11f0
uint32_t   g_vehCapOff   = 0;      // limits block -> DWORD MaxExtraVehicleModelInfos (0x324)
uint32_t   g_vehSlot     = 0;      // factory table -> the vehicle factory (0x28)
int        g_vehStoreSig = 0;      // 0 = not found yet (retried), 1 = resolved, -1 = refused for good
int        g_vehSigLogged = -2;    // the last hit count logged, so a retry loop logs each outcome once
bool       g_vehStoreDone = false; // PrepareVehicleStore made its decision (raised, too late, off or refused)
bool       g_vehStoreSeen = false; // Tick logged the created store once

bool IsWritable(const void* p, size_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(p);
    const uint8_t* end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        a = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// Resolve kVehStoreSig and read its operands. A miss is retried on the next call (the code may still be packed
// when the engine attaches); a malformed or ambiguous match is final.
bool ResolveVehStore() {
    if (g_vehStoreSig) return g_vehStoreSig > 0;
    const Image im = GameImage();
    if (!im.base || !im.size) return false;
    const uint8_t* m = nullptr;
    const int c = CountPat(ReadableRuns(im), Prof().vehStore.pat, &m);
    if (c != g_vehSigLogged) {
        g_vehSigLogged = c;
        if (c != 1)
            GLOGW("hook: [packstream] %s %s (%d hits)%s", Prof().vehStore.name, c == 0 ? "missing" : "ambiguous", c,
                  c == 0 ? "; retried" : "; the DLC vehicle store is not guarded");
    }
    if (c == 0) return false;
    const PackProfile& pp = Prof();
    if (c != 1 || !m || !IsReadable(m, pp.vsReadLen)) { g_vehStoreSig = -1; return false; }
    int32_t d1 = 0, d2 = 0;
    memcpy(&d1, m + pp.vsCfg.dispAt, 4);
    memcpy(&d2, m + pp.vsTbl.dispAt, 4);
    const uintptr_t cfg = (uintptr_t)(m + pp.vsCfg.endAt) + (intptr_t)d1;
    const uintptr_t tbl = (uintptr_t)(m + pp.vsTbl.endAt) + (intptr_t)d2;
    uint32_t off = 0;
    memcpy(&off, m + pp.vsLimitAt, 4);
    const uint32_t slot = m[pp.vsSlotAt];
    const uintptr_t lo = (uintptr_t)im.base, hi = lo + im.size;
    if (cfg < lo || cfg >= hi || tbl < lo || tbl >= hi || off == 0 || off > 0x1000 || (off & 3) || slot == 0 || (slot & 7)) {
        GLOGW("hook: [packstream] %s operands implausible (cfg +0x%llx, table +0x%llx, limit +0x%x, slot 0x%x); not used",
              Prof().vehStore.name, (unsigned long long)(cfg - lo), (unsigned long long)(tbl - lo), (unsigned)off, (unsigned)slot);
        g_vehStoreSig = -1;
        return false;
    }
    g_cfgGlobal = cfg;
    g_miGlobal = tbl;
    g_vehCapOff = off;
    g_vehSlot = slot;
    g_vehStoreSig = 1;
    GLOGI("hook: [packstream] %s UNIQUE @ +0x%llx: config global +0x%llx (expect 0x2e9e508), factory table +0x%llx "
          "(expect 0x1ec11f0), MaxExtraVehicleModelInfos at +0x%x (expect 0x324), vehicle factory slot 0x%x (expect 0x28)",
          Prof().vehStore.name, (unsigned long long)(m - im.base), (unsigned long long)(cfg - lo), (unsigned long long)(tbl - lo),
          (unsigned)off, (unsigned)slot);
    return true;
}

// The key-0xf000 vehicle array, or null when the factory or the array does not exist (yet) or is unreadable.
// The factory's map: buckets at +8, WORD bucket count at +0x10; node = {DWORD key, StoreHdr* +8, next +0x10}
// (the lookup 0x62e830: bucket = key % count).
StoreHdr* DlcVehStore() {
    if (g_vehStoreSig <= 0) return nullptr;
    char* const* tbl = reinterpret_cast<char* const*>(g_miGlobal);
    if (!IsReadable(tbl, 8) || !*tbl || !IsReadable(*tbl + g_vehSlot, 8)) return nullptr;
    char* fac = *reinterpret_cast<char* const*>(*tbl + g_vehSlot);
    if (!fac || !IsReadable(fac, 0x18)) return nullptr;
    char* const* buckets = *reinterpret_cast<char* const* const*>(fac + 8);
    const uint16_t nb = *reinterpret_cast<const uint16_t*>(fac + 0x10);
    if (!nb || !buckets || !IsReadable(buckets, (size_t)nb * 8)) return nullptr;
    const char* node = buckets[kDlcStoreKey % nb];
    for (int hops = 0; node && hops < 4096; ++hops) {
        if (!IsReadable(node, 0x18)) return nullptr;
        if (*reinterpret_cast<const uint32_t*>(node) == kDlcStoreKey) {
            StoreHdr* h = *reinterpret_cast<StoreHdr* const*>(node + 8);
            return (h && IsReadable(h, sizeof(StoreHdr))) ? h : nullptr;
        }
        node = *reinterpret_cast<const char* const*>(node + 0x10);
    }
    return nullptr;
}

// The DWORD MaxExtraVehicleModelInfos inside the game's parsed gameconfig, or null while it is not loaded.
uint32_t* VehStoreLimit() {
    if (g_vehStoreSig <= 0 || !IsReadable((void*)g_cfgGlobal, 8)) return nullptr;
    char* gc = *reinterpret_cast<char* const*>(g_cfgGlobal);
    if (!gc || !IsReadable(gc + 0x18, 8)) return nullptr;
    char* lim = *reinterpret_cast<char* const*>(gc + 0x18);
    if (!lim || !IsReadable(lim + g_vehCapOff, 4)) return nullptr;
    return reinterpret_cast<uint32_t*>(lim + g_vehCapOff);
}

// ---------------------------------------------------------------------------------------------
// Function-pointer views of the resolved addresses (Windows x64 ABI: rcx, rdx, r8).
// ---------------------------------------------------------------------------------------------
using AddDlcPackFn   = uint8_t (*)(void* mgr, const char* path);
using AtStrAppendFn  = void (*)(void* atString, const char* src, int len);  // 0x137aed8: append len bytes, keep NUL
using Setup2Fn       = uint8_t (*)(void* entry, uint32_t gen);
using MountGateFn    = uint8_t (*)(void* entry, uint32_t gen);
using RunPackGroupFn = void    (*)(void* mgr, uint32_t packHash, uint32_t groupHash);
using MountFn        = int     (*)(void* entry, uint32_t gen, const char* mountPoint);
using PushFolderFn   = void    (*)(void* asset, const char* folder);
using ExistsFn       = uint8_t (*)(void* asset, const char* name, uint64_t flags);
using PopFolderFn    = void    (*)(void* asset);

void*    Manager()          { return IsReadable((void*)g_singleton, 8) ? *reinterpret_cast<void**>(g_singleton) : nullptr; }
bool     VtableOk(void* mgr) {
    const Image im = GameImage();
    if (!IsReadable(mgr, 8)) return false;
    return *reinterpret_cast<uintptr_t*>(mgr) == (uintptr_t)im.base + Prof().mgrVtableRva;
}
uint16_t Count(void* mgr)   { return IsReadable((char*)mgr + Prof().mgrCount, 2) ? *reinterpret_cast<uint16_t*>((char*)mgr + Prof().mgrCount) : 0; }
uint32_t Gen(void* mgr)     { return IsReadable((char*)mgr + Prof().mgrGen, 4) ? *reinterpret_cast<uint32_t*>((char*)mgr + Prof().mgrGen) : 0; }
char*    Entry(void* mgr, int i) {
    if (i < 0 || !IsReadable((char*)mgr + Prof().mgrArray, 8)) return nullptr;
    char* arr = *reinterpret_cast<char**>((char*)mgr + Prof().mgrArray);
    char* e = arr + (size_t)i * Prof().entryStride;
    return IsReadable(e, Prof().entryStride) ? e : nullptr;
}
uint32_t EntryHash(char* e)  { return (e && IsReadable(e + Prof().entryHash, 4)) ? *reinterpret_cast<uint32_t*>(e + Prof().entryHash) : 0; }
uint32_t EntryState(char* e) { return (e && IsReadable(e + Prof().entryState, 4)) ? *reinterpret_cast<uint32_t*>(e + Prof().entryState) : 0xffffffffu; }

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
Status    g_status   = Status::Idle;
int       g_enabled  = -1;        // -1 = not read yet
char      g_fail[192] = "";       // the last pack's refusal (FailReason when every pack failed)
ULONGLONG g_mgrSince  = 0;        // when we first started waiting for the manager singleton
ULONGLONG g_mountAt   = 0;        // when the last change set ran (start of the model wait)
ULONGLONG g_pollAt    = 0;        // the next IS_MODEL_IN_CDIMAGE pass
std::string g_curPack;            // the pack being mounted, for the log lines

// The packs under %LOCALAPPDATA%\GAGE-MP\packs\, one folder each, mounted one per tick in name order.
struct Paths { std::string device; std::wstring file; };   // AddDlcPack's "<folder>/" + the folder's dlc.rpf
struct PackModel { MetaVehicle v; uint32_t hash = 0; bool inCdimage = false; };
struct Pack {
    std::string name;             // the folder name, lower case (what the tune's pack rows and the menu use)
    Paths paths;
    bool folder = false;          // setup2.xml at the root: loose folder (models in stream/); else the dlc.rpf itself
    bool mounted = false;         // the change set ran
    bool tune = false;            // the NG handling-tune override pack (%LOCALAPPDATA%\GAGE-MP\tune\gage_tune), mounted last
    std::vector<PackModel> models;
};
std::vector<Pack> g_packs;

// NG handling tune (0.4.18, handling_tune.h): the CHandlingData items gage_tune.json changed in the NG packs mounted so far.
int g_ngTuneOn = -1;                      // pack_ng_tune: -1 = not read yet
bool g_ngTuneSeen = false;                // some NG pack shipped a gage_tune.json
bool g_tuneQueued = false;                // the override pack was considered (once, after the last pack)
std::vector<TunedHandlingItem> g_ngTuned;
std::string g_ngTunePacks;                // "vehicle_regera, vehicle_supra": the packs the items came from
size_t g_next = 0;                // the next pack to mount
bool   g_listed = false;          // g_packs filled

// The menu's copy (overlay thread) of what the packs carry; written on the game thread.
std::mutex g_vehMutex;
std::vector<PackVehicle> g_vehShared;
uint32_t g_vehGen = 0;            // bumped (under g_vehMutex) every time g_vehShared is replaced

void Fail(const char* fmt, ...) GAGE_PRINTF(1, 2);
void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail, sizeof(g_fail), fmt, ap);
    va_end(ap);
    GLOGW("hook: [packstream] pack '%s' gave up: %s", g_curPack.c_str(), g_fail);
}

// The main module is Legacy's GTA5.exe. Every signature and RVA in this file comes from the Legacy 1.0.3889 image;
// Enhanced (GTA5_Enhanced.exe, a different rebuilt binary) must never be scanned with them.
bool IsLegacyGameExe() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    return _wcsicmp(base ? base + 1 : path, L"GTA5.exe") == 0;
}

// Any pack folder (one holding a dlc.rpf) under %LOCALAPPDATA%\GAGE-MP\packs\.
bool AnyPackFolder() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local || !*local) return false;
    const std::wstring root = std::wstring(local) + L"\\GAGE-MP\\packs\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' &&
            GetFileAttributesW((root + fd.cFileName + L"\\dlc.rpf").c_str()) != INVALID_FILE_ATTRIBUTES)
            any = true;
    } while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

// pack_mount (HKCU\Software\GAGE-MP): "1" arms the mount, "0" keeps it off, unset = on when packs are present
// (0.4.14: a tester unzips the packs folder and nothing else). Legacy GTA5.exe only, whatever the value says.
bool Enabled() {
    if (g_enabled < 0) {
        const std::wstring v = reg::Get(L"pack_mount");
        const bool legacy = IsLegacyGameExe();
        const bool present = AnyPackFolder();
        // GTA V Enhanced (0.4.26): armed only by pack_enh=2 (Round B, pack_enh.h), with packs present; pack_mount=0 still turns it off.
        const int enhMode = ParsePackEnh(reg::Get(L"pack_enh").c_str());
        const bool enh = !legacy && &Prof() == &kPackEnhanced && enhMode >= 2;
        if (enh) {
            g_enabled = (present && v != L"0") ? 1 : 0;
            GLOGI("[packenh] pack mount %s (pack_enh=%d, pack_mount=%s, pack folders %s)", g_enabled ? "ARMED: Round B mounts the packs once Round A passes" : "off",
                  enhMode, v.empty() ? "unset" : Narrow(v).c_str(), present ? "present" : "none");
            return g_enabled == 1;
        }
        const bool want = v == L"1" || (v.empty() && present);
        g_enabled = (want && legacy) ? 1 : 0;
        GLOGI("hook: [packstream] pack mount %s (pack_mount=%s, pack folders %s, game %s)", g_enabled ? "ARMED" : "off",
              v.empty() ? "unset" : Narrow(v).c_str(), present ? "present" : "none",
              legacy ? "Legacy GTA5.exe" : "not Legacy GTA5.exe: packs are Legacy 1.0.3889 only");
    }
    return g_enabled == 1;
}

// Every pack folder under %LOCALAPPDATA%\GAGE-MP\packs\ that holds a dlc.rpf (tools/packs/stage_packs.mjs lays them
// out), in name order; pack_name=<folder> limits the run to that one. Folders whose path is not plain ASCII are skipped
// (the game takes narrow paths), as are "*.old" / "*.staging" leftovers of a restage.
void EnumeratePacks() {
    g_packs.clear();
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local || !*local) { GLOGW("hook: [packstream] LOCALAPPDATA is not set; no packs"); return; }
    const std::wstring root = std::wstring(local) + L"\\GAGE-MP\\packs\\";
    const std::wstring only = reg::Get(L"pack_name");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { GLOGW("hook: [packstream] no packs folder at %s", Narrow(root).c_str()); return; }
    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring n = fd.cFileName;
        const auto endsWith = [&n](const wchar_t* sfx) { const size_t k = wcslen(sfx); return n.size() > k && n.compare(n.size() - k, k, sfx) == 0; };
        if (endsWith(L".old") || endsWith(L".staging")) continue;
        if (!only.empty() && _wcsicmp(only.c_str(), n.c_str()) != 0) continue;
        names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const std::wstring& n : names) {
        const std::wstring folder = root + n + L"\\";
        bool ascii = true;
        for (wchar_t c : folder) if (c > 0x7E || c < 0x20) ascii = false;
        if (!ascii) { GLOGW("hook: [packstream] pack folder %ls skipped: the path is not plain ASCII", n.c_str()); continue; }
        if (GetFileAttributesW((folder + L"dlc.rpf").c_str()) == INVALID_FILE_ATTRIBUTES) {
            GLOGW("hook: [packstream] pack folder %ls skipped: no dlc.rpf", n.c_str());
            continue;
        }
        Pack p;
        for (wchar_t c : n) p.name.push_back((char)std::tolower((unsigned char)c));
        p.paths.file = folder + L"dlc.rpf";
        for (wchar_t c : folder) p.paths.device.push_back(c == L'\\' ? '/' : (char)c);
        p.folder = GetFileAttributesW((folder + L"setup2.xml").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (p.paths.device.size() > 248)
            GLOGW("hook: [packstream] pack %s: device path is %u chars; AddDlcPack truncates past ~248", p.name.c_str(),
                  (unsigned)p.paths.device.size());
        g_packs.push_back(std::move(p));
    }
    GLOGI("hook: [packstream] %u pack(s) under %s%s", (unsigned)g_packs.size(), Narrow(root).c_str(),
          only.empty() ? "" : "  (pack_name limits the run to one)");
    for (const Pack& p : g_packs)
        GLOGI("hook: [packstream]   %-24s %s", p.name.c_str(), p.folder ? "loose folder (setup2.xml + stream/)" : "dlc.rpf (the game's own loader)");
}

// Read and sanity-check the pack's RPF7 header before the game ever opens it. A header the retail game
// would reject (OPEN, or not RPF7) must never reach the loader: it ends the game with a fatal error.
bool Preflight(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Fail("pack not found or unreadable: %s", Narrow(file).c_str());
        return false;
    }
    uint32_t hdr[4] = {};
    DWORD got = 0;
    LARGE_INTEGER sz = {};
    const bool ok = ReadFile(h, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (!ok) { Fail("pack header unreadable: %s", Narrow(file).c_str()); return false; }
    if (hdr[0] != kRpf7Magic) { Fail("pack is not an RPF7 archive (magic 0x%08x)", (unsigned)hdr[0]); return false; }
    if (hdr[1] == 0 || hdr[1] > 1000000u) { Fail("pack entry count %u is implausible", (unsigned)hdr[1]); return false; }
    if (sz.QuadPart > (256LL << 20)) { Fail("pack is larger than 256 MiB (%lld bytes); not mounting", (long long)sz.QuadPart); return false; }
    const uint32_t enc = hdr[3];
    const char* encName = enc == kEncOpen ? "OPEN" : enc == kEncAes ? "AES" : enc == kEncNg ? "NG"
                        : enc == kEncNone ? "NONE" : "unknown";
    if (enc == kEncOpen) {
        Fail("pack is OPEN (unencrypted); retail 3889 raises a fatal error on it and the engine has no patch. "
             "Seal it with AES (pack_dlc_rpf.mjs --enc=aes) before mounting");
        return false;
    }
    if (enc != kEncAes && enc != kEncNg) {
        Fail("pack encryption 0x%08x (%s) is neither AES nor NG; not mounting", (unsigned)enc, encName);
        return false;
    }
    GLOGI("hook: [packstream] pack %s: RPF7, %u entries, encryption 0x%08x (%s), %lld bytes",
          Narrow(file).c_str(), (unsigned)hdr[1], (unsigned)enc, encName, (long long)sz.QuadPart);
    return true;
}

// Folder-mode (strategy B) preflight: the pack is a LOOSE folder. dlc.rpf is only the file AddDlcPack's
// Exists gate needs (we override the entry to a relative/folder device, so it is never opened or decrypted);
// the real content is setup2.xml + content.xml + data/*.meta + nested x64/*.rpf read off disk through the
// device. Confirm the gate file and setup2.xml are both present; encryption is irrelevant here.
bool FolderPreflight(const Paths& paths) {
    const DWORD gate = GetFileAttributesW(paths.file.c_str());
    if (gate == INVALID_FILE_ATTRIBUTES || (gate & FILE_ATTRIBUTE_DIRECTORY)) {
        Fail("folder-mode: AddDlcPack's gate file is missing: %s (drop a dlc.rpf there; it is not mounted)",
             Narrow(paths.file).c_str());
        return false;
    }
    // setup2.xml must sit at the folder root (what the relative device exposes as extra:/setup2.xml).
    std::wstring folderW = paths.file;
    const size_t slash = folderW.find_last_of(L"\\/");
    if (slash != std::wstring::npos) folderW.resize(slash + 1);
    const std::wstring setup = folderW + L"setup2.xml";
    if (GetFileAttributesW(setup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail("folder-mode: setup2.xml not found at the pack folder root (%s); the loose layout is incomplete",
             Narrow(setup).c_str());
        return false;
    }
    GLOGI("hook: [packstream] folder-mode preflight ok: gate dlc.rpf + setup2.xml present at \"%s\"",
          paths.device.c_str());
    return true;
}

bool ModelInCdimage(World& world, uint32_t modelHash) {
    const uint64_t a[] = { (uint64_t)modelHash };
    uint64_t ret[4] = {};
    if (!world.CallNative(kNativeIsModelInCdimage, a, 1, ret)) return false;
    return (ret[0] & 0xff) != 0;
}

// Resolve the diagnostic probe signatures once, over the same readable runs ResolveAll uses. Kept separate
// so a probe-sig miss cannot fail the normal mount. Returns whether all four resolved uniquely.
bool ResolveProbe() {
    if (g_probeResolvedOnce) return g_probeReady;
    g_probeResolvedOnce = true;
    const Image im = GameImage();
    if (!im.base || !im.size) return false;
    const std::vector<Run> runs = ReadableRuns(im);
    bool all = true;
    for (int i = 0; i < kProbeSigCount; ++i) {
        const uint8_t* first = nullptr;
        const int c = CountPat(runs, ProbeSigOf(i).pat, &first);
        if (c == 1) {
            g_probeAddr[i] = (uintptr_t)first;
            GLOGI("hook: [packstream] probe-sig %-11s UNIQUE @ +0x%llx   (%s)", ProbeSigOf(i).name,
                  (unsigned long long)(uintptr_t)(first - im.base), ProbeSigOf(i).source);
        } else {
            GLOGW("hook: [packstream] probe-sig %-11s %s", ProbeSigOf(i).name,
                  c < 0 ? "BAD PATTERN" : (c == 0 ? "missing" : "ambiguous"));
            all = false;
        }
    }
    g_probeReady = all;
    return all;
}

// Mount the freshly-added pack under "extra:/" via Mount() directly (NOT DlcSetup2, which tears "extra:/"
// back down on failure) and ask the game, through the same fiAssetManager calls DlcSetup2 uses, which pack
// files are visible. This decides the state-5 blocker: if setup2.xml AND content.xml AND a data meta all
// read absent, the mounted AES TOC is invisible in-process (an open/decrypt-route problem, not our seal,
// which rage-cli and our own --check both read fine); if setup2.xml alone is absent, it is a pack
// naming/layout issue. The entry must be fresh (device ptr 0) so Mount opens rather than short-circuits.
void Probe(void* mgr, char* entry) {
    if (!ResolveProbe()) { GLOGW("hook: [packstream] probe: not all probe signatures resolved; skipping"); return; }
    const Image im = GameImage();
    void* asset = reinterpret_cast<void*>((uintptr_t)im.base + Prof().assetGlobalRva);
    const uint32_t gen = Gen(mgr);
    void* dev0 = IsReadable(entry, 8) ? *reinterpret_cast<void**>(entry) : nullptr;
    GLOGI("hook: [packstream] probe: ASSET @0x%llx; Mount(entry @0x%llx, gen %u, \"extra:/\"); entry.device was 0x%llx",
          (unsigned long long)(uintptr_t)asset, (unsigned long long)(uintptr_t)entry, (unsigned)gen,
          (unsigned long long)(uintptr_t)dev0);
    const int mrc = reinterpret_cast<MountFn>(g_probeAddr[kMount])(entry, gen, "extra:/");
    void* dev1 = IsReadable(entry, 8) ? *reinterpret_cast<void**>(entry) : nullptr;
    GLOGI("hook: [packstream] probe: Mount returned %d (0=ok); entry.device now 0x%llx", mrc,
          (unsigned long long)(uintptr_t)dev1);
    if (mrc != 0) { GLOGW("hook: [packstream] probe: Mount did not open the archive; no file check possible"); return; }

    static const char* const kProbeFiles[] = { "setup2.xml", "content.xml", "data/vehicles.meta", "data/handling.meta" };
    reinterpret_cast<PushFolderFn>(g_probeAddr[kPushFolder])(asset, "extra:/");
    int anyVisible = 0, setup2Visible = 0;
    for (const char* f : kProbeFiles) {
        const uint8_t ex = reinterpret_cast<ExistsFn>(g_probeAddr[kExists])(asset, f, 0);
        if (ex) ++anyVisible;
        if (ex && std::strcmp(f, "setup2.xml") == 0) setup2Visible = 1;
        GLOGI("hook: [packstream] probe:   Exists(extra:/%s) = %u", f, (unsigned)ex);
    }
    reinterpret_cast<PopFolderFn>(g_probeAddr[kPopFolder])(asset);
    GLOGI("hook: [packstream] probe: VERDICT -> %s",
          anyVisible == 0 ? "TOC INVISIBLE in-process (AES TOC not decrypted/parsed via this open route)"
          : (setup2Visible ? "setup2.xml IS visible -- the full chain should work; re-run with pack_probe=0"
                           : "some files visible but setup2.xml absent -- pack naming/layout issue"));
}

// ---- the pack's model list ------------------------------------------------------------------------------------
// A loose pack's content.xml and vehicles.meta are plain files on disk. A dlc.rpf pack (NG) cannot be read outside the
// game, so its files are read through the game's own device layer once MountGate has mounted it:
//   fiDevice* GetDevice(const char* path /*rcx*/, bool readOnly /*dl*/)     RVA 0x1364150 (the pack's mount for "dlc_x:/...")
//   device vtable: +0x08 Open(this, name, readOnly) -> handle (-1 = no), +0x30 Read(this, handle, buf, int len) -> n,
//                  +0x60 Close(this, handle), +0xb0 GetFileSize(this, name) -> bytes. (The bulk calls at +0x10 / +0x38 /
//                  +0x68 are what the RSC7 header reader 0x1364d20 uses; the plain ones sit one slot before each, and
//                  fiPackfile's +0xb0 0x1364a4c returns an entry's uncompressed size, so Read hands back the real text.)
// (The GetDevice pattern is Prof().getDevice: RVA 0x1364150, the jmp target of 0x1364148, which every device-for-path
// caller uses.)
constexpr size_t kMaxMetaBytes = 4u << 20;   // a vehicles.meta is a few KB; anything this big is not one

bool ReadGameFile(const std::string& path, std::string& out) {
    static uintptr_t s_fn = 0;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        const Image im = GameImage();
        const uint8_t* first = nullptr;
        const int c = (im.base && im.size) ? CountPat(ReadableRuns(im), Prof().getDevice.pat, &first) : 0;
        if (c == 1) s_fn = (uintptr_t)first;
        GLOGI("hook: [packstream] %s %s", Prof().getDevice.name, c == 1 ? "UNIQUE" : (c == 0 ? "missing" : "ambiguous"));
    }
    if (!s_fn) return false;
    using GetDeviceFn = void* (*)(const char*, uint8_t);
    using OpenFn = uint64_t (*)(void*, const char*, uint8_t);
    using ReadFn = int (*)(void*, uint64_t, void*, int);
    using CloseFn = int (*)(void*, uint64_t);
    using SizeFn = uint64_t (*)(void*, const char*);
    void* dev = reinterpret_cast<GetDeviceFn>(s_fn)(path.c_str(), 1);
    if (!dev || !IsReadable(dev, 8)) return false;
    void* const* vt = *reinterpret_cast<void* const* const*>(dev);
    if (!vt || !IsReadable(vt, 0xb8)) return false;
    const uint64_t size = reinterpret_cast<SizeFn>(vt[0xb0 / 8])(dev, path.c_str());
    if (size == 0 || size > kMaxMetaBytes) return false;
    const uint64_t fh = reinterpret_cast<OpenFn>(vt[0x08 / 8])(dev, path.c_str(), 1);
    if (fh == ~0ull) return false;
    out.assign((size_t)size, '\0');
    size_t got = 0;
    while (got < out.size()) {
        const int n = reinterpret_cast<ReadFn>(vt[0x30 / 8])(dev, fh, &out[got], (int)(out.size() - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    reinterpret_cast<CloseFn>(vt[0x60 / 8])(dev, fh);
    out.resize(got);
    return got > 0;
}

bool ReadDiskFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart <= kMaxMetaBytes;
    if (ok) {
        out.assign((size_t)sz.QuadPart, '\0');
        DWORD got = 0;
        ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

// The vehicles a pack defines: content.xml -> every VEHICLE_METADATA_FILE -> its <modelName> items. `mount` is the
// pack's device ("dlc_supra:/") for a dlc.rpf pack; a loose pack reads the same files off disk.
void ReadPackModels(Pack& p, const std::string& mount) {
    std::string content;
    std::wstring folderW = p.paths.file;
    folderW.resize(folderW.size() - wcslen(L"dlc.rpf"));
    const bool haveContent = p.folder ? ReadDiskFile(folderW + L"content.xml", content)
                                      : (!mount.empty() && ReadGameFile(mount + "content.xml", content));
    if (!haveContent) {
        GLOGW("hook: [packstream] pack %s: content.xml not readable (%s); its cars will not be listed", p.name.c_str(),
              p.folder ? "on disk" : (mount.empty() ? "no mount point" : mount.c_str()));
        return;
    }
    const std::vector<std::string> metas = MetaFilesOfType(content, "VEHICLE_METADATA_FILE");
    for (const std::string& m : metas) {
        std::string text;
        bool ok = false;
        if (p.folder) {
            std::wstring rel;
            for (char c : PathInPack(m)) rel.push_back(c == '/' ? L'\\' : (wchar_t)(unsigned char)c);
            ok = ReadDiskFile(folderW + rel, text);
        } else {
            ok = ReadGameFile(m, text);
        }
        if (!ok) { GLOGW("hook: [packstream] pack %s: %s not readable", p.name.c_str(), m.c_str()); continue; }
        for (MetaVehicle& v : ParseVehiclesMeta(text)) {
            bool dup = false;
            for (const PackModel& o : p.models) if (_stricmp(o.v.model.c_str(), v.model.c_str()) == 0) dup = true;
            if (dup) continue;
            PackModel pm;
            pm.v = std::move(v);
            p.models.push_back(std::move(pm));
        }
    }
    std::string list;
    for (const PackModel& pm : p.models) { if (!list.empty()) list += ", "; list += pm.v.model; }
    GLOGI("hook: [packstream] pack %s: %u vehicle meta file(s), %u car(s): %s", p.name.c_str(), (unsigned)metas.size(),
          (unsigned)p.models.size(), list.empty() ? "-" : list.c_str());
}

// The pack's mount point ("dlc_supra:/"): DlcEntry +0x30 is an atString (char* +0x30, WORD length +0x38), which
// MountGate 0x94301c hands to Mount when the length is nonzero. "" when unreadable or implausible.
// (The offsets are Prof().entryMountPoint / entryMountPointLen: 0x30 / 0x38 on Legacy.)
std::string EntryMountPoint(char* e) {
    if (!e || !IsReadable(e + Prof().entryMountPoint, Prof().entryMountPointLen - Prof().entryMountPoint + 2)) return "";
    const char* s = *reinterpret_cast<const char* const*>(e + Prof().entryMountPoint);
    const uint16_t n = *reinterpret_cast<const uint16_t*>(e + Prof().entryMountPointLen);
    if (!s || n == 0 || n > 120 || !IsReadable(s, n)) return "";
    std::string m(s, n);
    for (char c : m) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return "";
    if (m.find(':') == std::string::npos) return "";
    if (m.back() != '/') m.push_back('/');
    return m;
}

// Enhanced (pack_enh=2): the image registry and the streaming-info manager are the ones the patterns resolved in THIS process
// (pack_enh_mount_core.h Extras, RegStreamFile's own register call); 0 = use the profile's offline constants (Legacy).
struct StreamAddrs { uint32_t registryRva = 0, streamInfoRva = 0; };
StreamAddrs g_enhStream;

// Register every model file in <pack>/stream/ with the game's streaming system (see kRegStreamSig). Runs
// before the change set, so CVehicleModelInfo::Init (vehicles.meta) finds the "jesko" fragment and txd
// slots by name, already bound to the files. Returns false (after Fail) only when files are present but
// none registered; a pack without a stream/ folder is logged and left to its content.xml.
bool RegisterLooseStream(const Paths& paths) {
    std::wstring folderW = paths.file;
    const size_t slash = folderW.find_last_of(L"\\/");
    if (slash != std::wstring::npos) folderW.resize(slash + 1);
    const std::wstring streamW = folderW + L"stream\\";

    // .ytd first: a vehicle fragment's textures should already have their slot when the fragment's does.
    std::vector<std::wstring> files;
    for (const wchar_t* ext : { L"ytd", L"yft", L"ydr", L"ydd" }) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((streamW + L"*." + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (files.empty()) {
        GLOGI("hook: [packstream] stream: no model files in %s (models come from content.xml only)",
              Narrow(streamW).c_str());
        return true;
    }

    const Image im = GameImage();
    if (!im.base || !im.size) { Fail("stream: game image bounds unavailable"); return false; }
    const uint8_t* fn = nullptr;
    const int hits = CountPat(ReadableRuns(im), Prof().regStream.pat, &fn);
    if (hits != 1 || !fn) {
        Fail("stream: %s signature %s (%d hits); not registering %u model files", Prof().regStream.name,
             hits == 0 ? "missing" : "ambiguous", hits, (unsigned)files.size());
        return false;
    }
    const uintptr_t base = (uintptr_t)im.base;
    void* const* registry = reinterpret_cast<void* const*>(base + (g_enhStream.registryRva ? g_enhStream.registryRva : Prof().imageRegistryRva));
    void* raw = IsReadable(registry, 8) ? registry[0] : nullptr;
    const uintptr_t rawVt = (raw && IsReadable(raw, 8)) ? *reinterpret_cast<const uintptr_t*>(raw) : 0;
    GLOGI("hook: [packstream] stream: %s UNIQUE @ +0x%llx; image 0 collection @0x%llx vtable +0x%llx (%s)",
          Prof().regStream.name, (unsigned long long)(fn - im.base), (unsigned long long)(uintptr_t)raw,
          (unsigned long long)(rawVt ? rawVt - base : 0),
          rawVt == base + Prof().rawStreamerVtRva ? "RawStreamer, as expected" : "NOT the RawStreamer");
    if (rawVt != base + Prof().rawStreamerVtRva) {
        Fail("stream: image 0 is not the RawStreamer collection on this image; not registering loose files");
        return false;
    }

    const auto* mgr = reinterpret_cast<const uint8_t*>(base + (g_enhStream.streamInfoRva ? g_enhStream.streamInfoRva : Prof().streamInfoMgrRva));
    const uint32_t countAt = Prof().regStreamInfoCountAt;   // Legacy 8; Enhanced 0 = the count's offset was not read: only the array is bounds-checked
    int ok = 0;
    for (const std::wstring& f : files) {
        bool ascii = true;
        for (wchar_t c : f) if (c > 0x7E) ascii = false;
        if (!ascii) { GLOGW("hook: [packstream] stream: skipped %ls (non-ASCII name)", f.c_str()); continue; }
        const std::string name = Narrow(f);
        const std::string path = paths.device + "stream/" + name;   // forward-slash OS path
        int out = -1;
        reinterpret_cast<RegStreamFileFn>(fn)(&out, path.c_str(), 1, name.c_str(), 0, 0);
        uint32_t handle = 0, flags = 0;
        const uint8_t* arr = IsReadable(mgr, 16) ? *reinterpret_cast<uint8_t* const*>(mgr) : nullptr;
        const uint32_t count = countAt ? (IsReadable(mgr + countAt, 4) ? *reinterpret_cast<const uint32_t*>(mgr + countAt) : 0) : 0x100000u;
        if (out >= 0 && (uint32_t)out < count && arr && IsReadable(arr + (size_t)out * 8, 8)) {
            handle = *reinterpret_cast<const uint32_t*>(arr + (size_t)out * 8);
            flags  = *reinterpret_cast<const uint32_t*>(arr + (size_t)out * 8 + 4);
        }
        if (out >= 0) ++ok;
        GLOGI("hook: [packstream] stream: %-16s -> streaming index %d, handle 0x%08x (image %u, file %u), flags 0x%08x%s",
              name.c_str(), out, (unsigned)handle, (unsigned)(handle >> 16), (unsigned)(handle & 0xffff),
              (unsigned)flags, out < 0 ? "  REFUSED (file not found by the device, or no module for the extension)" : "");
    }
    GLOGI("hook: [packstream] stream: registered %d of %u model files from %s", ok, (unsigned)files.size(),
          Narrow(streamW).c_str());
    if (ok == 0) { Fail("stream: none of the %u model files registered", (unsigned)files.size()); return false; }
    return true;
}

// ---- NG handling tune (0.4.18) ---------------------------------------------------------------------------------
// An NG pack's handling.meta lives in its encrypted dlc.rpf, so stage_packs cannot tune it; it ships the rules next to
// the archive as gage_tune.json instead. While the pack is mounted (before its change set runs) its HANDLING_FILEs are
// read through the game's device, tuned (handling_tune.h) and kept; after the last pack, the tuned items go into one
// loose override pack that is mounted last. LIVE-UNVERIFIED: that GTA's HANDLING_FILE loader replaces an existing
// handlingName in place (Rockstar's patchday packs re-define base cars' handling that way). If it appends instead, the
// cars keep their stock handling and nothing else changes. pack_ng_tune=0 (HKCU\Software\GAGE-MP) switches it off.
bool NgTuneEnabled() {
    if (g_ngTuneOn < 0) {
        const std::wstring v = reg::Get(L"pack_ng_tune");
        g_ngTuneOn = v == L"0" ? 0 : 1;
        GLOGI("engine: NG handling tune (0.4.18): pack_ng_tune=%s -> %s", v.empty() ? "unset" : Narrow(v).c_str(),
              g_ngTuneOn ? "on (an NG pack's gage_tune.json is applied through an override pack)" : "OFF (NG packs keep their own handling)");
    }
    return g_ngTuneOn == 1;
}

void CollectNgTune(const Pack& p, const std::string& mount, std::vector<TunedHandlingItem>& out) {
    std::wstring folderW = p.paths.file;
    folderW.resize(folderW.size() - wcslen(L"dlc.rpf"));
    std::string json;
    if (!ReadDiskFile(folderW + L"gage_tune.json", json)) return;   // no rules shipped with this pack
    g_ngTuneSeen = true;
    if (!NgTuneEnabled()) return;
    const HandlingTune t = ParseHandlingTune(json);
    if (!t.ok) {
        GLOGW("hook: [packstream] pack %s: gage_tune.json not parsed (%s); its handling stays as the pack has it", p.name.c_str(),
              t.error.c_str());
        return;
    }
    std::string content;
    if (mount.empty() || !ReadGameFile(mount + "content.xml", content)) {
        GLOGW("hook: [packstream] pack %s: handling tune: content.xml not readable through the mount (%s)", p.name.c_str(),
              mount.empty() ? "no mount point" : mount.c_str());
        return;
    }
    const std::vector<std::string> files = MetaFilesOfType(content, "HANDLING_FILE");
    unsigned tuned = 0, skipped = 0, seen = 0;
    for (const std::string& m : files) {
        std::string text;
        if (!ReadGameFile(m, text)) { GLOGW("hook: [packstream] pack %s: handling tune: %s not readable", p.name.c_str(), m.c_str()); continue; }
        const HandlingTuneResult r = TuneHandlingMeta(text, t);
        seen += (unsigned)r.seen;
        for (const std::string& s : r.skipped) {
            ++skipped;
            GLOGW("hook: [packstream] handling tune: %s: SKIPPED %s", p.name.c_str(), s.c_str());
        }
        for (const TunedHandlingItem& it : r.items) {
            ++tuned;
            GLOGI("hook: [packstream] handling tune: %s: %s", it.name.c_str(), it.changes.c_str());
            out.push_back(it);
        }
    }
    GLOGI("hook: [packstream] pack %s: handling tune: %u HANDLING_FILE(s), %u item(s), %u tuned, %u skipped", p.name.c_str(),
          (unsigned)files.size(), seen, tuned, skipped);
}

// Keep a pack's tuned items once its change set has run (a pack refused before that never loaded the handling the
// items would redefine). Two packs with one handlingName: the later pack's version wins, as in the game.
void KeepNgTune(const Pack& p, const std::vector<TunedHandlingItem>& items) {
    if (items.empty()) return;
    for (const TunedHandlingItem& it : items) {
        bool replaced = false;
        for (TunedHandlingItem& o : g_ngTuned)
            if (tunedetail::IEq(o.name, it.name)) { o = it; replaced = true; }
        if (!replaced) g_ngTuned.push_back(it);
    }
    g_ngTunePacks += (g_ngTunePacks.empty() ? "" : ", ") + p.name;
}

bool WriteDiskFile(const std::wstring& path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, data, (DWORD)n, &put, nullptr) && put == n;
    return CloseHandle(h) && ok;
}
bool WriteDiskText(const std::wstring& path, const std::string& s) { return WriteDiskFile(path, s.data(), s.size()); }

constexpr const wchar_t* kTuneOwnerFile = L"gage_tune.owner";   // marks a folder this code wrote (and may delete)
constexpr const wchar_t* kTuneFiles[] = { L"setup2.xml", L"content.xml", L"data\\handling.meta", L"dlc.rpf", kTuneOwnerFile };

// Delete a tune folder this code wrote: only its known files, and only when the owner marker is there; a folder with
// anything else in it survives (RemoveDirectory refuses a non-empty one). True when the folder is gone.
bool RemoveOwnTuneFolder(const std::wstring& dir) {
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (GetFileAttributesW((dir + kTuneOwnerFile).c_str()) == INVALID_FILE_ATTRIBUTES) return false;   // not ours
    for (const wchar_t* f : kTuneFiles)
        if (_wcsicmp(f, kTuneOwnerFile) != 0) DeleteFileW((dir + f).c_str());
    DeleteFileW((dir + kTuneOwnerFile).c_str());
    RemoveDirectoryW((dir + L"data").c_str());
    return RemoveDirectoryW(dir.c_str()) != 0;
}

// Write %LOCALAPPDATA%\GAGE-MP\tune\gage_tune\ (via gage_tune.staging\, then a rename) and describe it as a folder-mode
// pack. Not under packs\: the launcher's pack sync owns that folder. False (logged) when anything cannot be written.
bool BuildTunePack(Pack& out) {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local || !*local) { GLOGW("hook: [packstream] handling tune: LOCALAPPDATA is not set"); return false; }
    const std::wstring root = std::wstring(local) + L"\\GAGE-MP\\tune\\";
    const std::wstring dir = root + L"gage_tune\\", staging = root + L"gage_tune.staging\\";
    for (wchar_t c : dir)
        if (c > 0x7E || c < 0x20) { GLOGW("hook: [packstream] handling tune: %s is not a plain ASCII path", Narrow(dir).c_str()); return false; }
    CreateDirectoryW(root.c_str(), nullptr);
    if (!RemoveOwnTuneFolder(staging)) { GLOGW("hook: [packstream] handling tune: %s holds files this code did not write; left alone", Narrow(staging).c_str()); return false; }
    if (!CreateDirectoryW(staging.c_str(), nullptr) || !CreateDirectoryW((staging + L"data").c_str(), nullptr)) {
        GLOGW("hook: [packstream] handling tune: cannot create %s (error %lu)", Narrow(staging).c_str(), (unsigned long)GetLastError());
        return false;
    }
    // AddDlcPack's gate file (folder mode never opens it): a well-formed empty RPF7, one empty root directory, OPEN like
    // the gates of the loose packs stage_packs writes.
    uint8_t gate[512] = {};
    const uint32_t words[] = { 0x52504637u /* RPF7 */, 1u /* entries */, 16u /* names */, 0x4E45504Fu /* OPEN */,
                               0u /* root: name offset */, 0x7FFFFF00u /* directory */, 1u /* first child */, 0u /* children */ };
    memcpy(gate, words, sizeof(words));
    const bool wrote = WriteDiskText(staging + kTuneOwnerFile, "written by gage_mp.dll (pack_stream.cpp NG handling tune); deleted and rewritten every launch\n") &&
                       WriteDiskText(staging + L"setup2.xml", BuildTuneSetup2()) &&
                       WriteDiskText(staging + L"content.xml", BuildTuneContent()) &&
                       WriteDiskText(staging + L"data\\handling.meta", BuildTuneMeta(g_ngTuned)) &&
                       WriteDiskFile(staging + L"dlc.rpf", gate, sizeof(gate));
    if (!wrote) { GLOGW("hook: [packstream] handling tune: writing %s failed (error %lu)", Narrow(staging).c_str(), (unsigned long)GetLastError()); return false; }
    if (!RemoveOwnTuneFolder(dir)) { GLOGW("hook: [packstream] handling tune: %s holds files this code did not write; left alone", Narrow(dir).c_str()); return false; }
    std::wstring from = staging, to = dir;
    from.pop_back(); to.pop_back();   // MoveFileEx takes the folder names without the trailing separator
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        GLOGW("hook: [packstream] handling tune: rename %s -> %s failed (error %lu)", Narrow(from).c_str(), Narrow(to).c_str(),
              (unsigned long)GetLastError());
        return false;
    }
    out = Pack();
    out.name = "gage_tune";
    out.paths.file = dir + L"dlc.rpf";
    for (wchar_t c : dir) out.paths.device.push_back(c == L'\\' ? '/' : (char)c);
    out.folder = true;
    out.tune = true;
    return true;
}

// Run the whole chain once for one pack. Game thread. Logs (Fail) and returns false on any refusal; true when the
// pack's change set ran (its models are then polled in the cdimage by Tick).
bool MountSequence(World& world, Pack& p) {
    (void)world;
    g_curPack = p.name;
    void* mgr = Manager();
    if (!mgr) { Fail("ExtraContentManager singleton is null after the wait (boot did not finish?)"); return false; }
    if (!VtableOk(mgr)) { Fail("manager vtable mismatch at @0x%llx; refusing to call into it", (unsigned long long)(uintptr_t)mgr); return false; }
    // Confirmed before touching the pack, so a signature-only run (pack_mount=1, no pack) still validates
    // that the singleton dereferences to a live manager with a sane pack count and the expected vtable.
    GLOGI("hook: [packstream] pack %s: manager @0x%llx live, vtable OK, %u packs currently loaded", p.name.c_str(),
          (unsigned long long)(uintptr_t)mgr, (unsigned)Count(mgr));

    const Paths& paths = p.paths;
    const bool folderMode = p.folder;
    if (folderMode) {
        if (!FolderPreflight(paths)) return false;       // Fail() already logged
    } else if (!Preflight(paths.file)) {
        return false;                                     // Fail() already logged
    }
    GLOGI("hook: [packstream] AddDlcPack path \"%s\"%s", paths.device.c_str(),
          folderMode ? "  (folder-mode: entry will be overridden to a relative device)" : "");

    const uint16_t n0 = Count(mgr);
    const uint8_t added = reinterpret_cast<AddDlcPackFn>(g_addr[kAddDlcPack])(mgr, paths.device.c_str());
    const uint16_t n1 = Count(mgr);
    GLOGI("hook: [packstream] AddDlcPack returned %u; pack count %u -> %u", (unsigned)added, (unsigned)n0, (unsigned)n1);
    if (!added && n1 == n0) {
        Fail("AddDlcPack refused \"%s\": the game's device layer could not resolve \"<path>dlc.rpf\"",
             paths.device.c_str());
        return false;
    }
    const int idx = (int)n1 - 1;
    char* e = Entry(mgr, idx);
    if (!e) { Fail("the new DLC entry at index %d is not readable", idx); return false; }
    uint32_t packHash = EntryHash(e);
    const uint32_t gen = Gen(mgr);
    GLOGI("hook: [packstream] entry @0x%llx idx %d: nameHash 0x%08x, state %u, gen %u",
          (unsigned long long)(uintptr_t)e, idx, (unsigned)packHash, (unsigned)EntryState(e), (unsigned)gen);

    if (folderMode) {
        // Override the packfile entry AddDlcPack built into a relative/folder device: Mount 0x94307c takes
        // its folder branch (0x943144) only when +0xc8 == 1 (==2 is packfile = the crypto path), it loads
        // +0xb8 as a const char* (the loose base folder) and ignores it unless the +0xc0 WORD is nonzero.
        //
        // +0xb8 is a GAME-OWNED atString {char* +0xb8, WORD length +0xc0, WORD capacity +0xc2}: the entry's
        // destructor 0x911d6c frees the pointer with the game's allocator, which happens whenever the pack array
        // grows (the next AddDlcPack moves the entries) and at shutdown. 0.4.13 pointed it at our own std::string:
        // harmless with one pack, but the second pack's AddDlcPack freed our pointer and the game's allocator raised
        // its fatal error (live 2026-11-05, INT3 at +0x136af5b, rs6 after jesko). So the path is rewritten IN the
        // game's buffer through the game's own append: length := 0, then Append(folder) (it reallocates with the
        // game's allocator when the folder path is longer than the capacity).
        const std::string& folder = paths.device;          // "<folder>/" forward-slash, trailing '/'
        if (!g_strAppend) { Fail("folder-mode: the game's atString append was not resolved; not overriding the entry"); return false; }
        if (!IsReadable(e + Prof().entryType, 4) || !IsReadable(e + Prof().entryPath, 12)) {
            Fail("folder-mode: DLC entry device fields (+0xb8/+0xc0/+0xc8) are not writable"); return false;
        }
        const uint32_t oldType = *reinterpret_cast<uint32_t*>(e + Prof().entryType);
        const uint16_t oldLen = *reinterpret_cast<uint16_t*>(e + Prof().entryPathLen);
        const char* oldPath = *reinterpret_cast<const char* const*>(e + Prof().entryPath);
        GLOGI("hook: [packstream] folder-mode: pre-override type(+0xc8)=%u length(+0xc0)=%u capacity(+0xc2)=%u path(+0xb8)=\"%s\"",
              (unsigned)oldType, (unsigned)oldLen, (unsigned)*reinterpret_cast<uint16_t*>(e + Prof().entryPathLen + 2),
              (oldPath && IsReadable(oldPath, 1)) ? oldPath : "<null>");
        *reinterpret_cast<uint16_t*>(e + Prof().entryPathLen) = 0;
        reinterpret_cast<AtStrAppendFn>(g_strAppend)(e + Prof().entryPath, folder.c_str(), (int)folder.size());
        const char* newPath = *reinterpret_cast<const char* const*>(e + Prof().entryPath);
        const uint16_t newLen = *reinterpret_cast<uint16_t*>(e + Prof().entryPathLen);
        if (newLen != folder.size() || !newPath || !IsReadable(newPath, folder.size() + 1) ||
            memcmp(newPath, folder.c_str(), folder.size() + 1) != 0) {
            Fail("folder-mode: the entry path did not read back as the folder after the game's append (length %u)", (unsigned)newLen);
            return false;
        }
        *reinterpret_cast<uint32_t*>(e + Prof().entryType) = Prof().typeRelative;
        GLOGI("hook: [packstream] folder-mode: overrode entry to relative device (type=1) over \"%s\" (game-owned string, "
              "length %u; nameHash set from setup2.xml by DlcSetup2)", newPath, (unsigned)newLen);
    } else if (!packHash) {
        Fail("the new DLC entry has a zero name hash; AddDlcPack did not populate it"); return false;
    }

    // Diagnostic branch (default off): mount the pack ourselves and ask the game which files it can see,
    // instead of running DlcSetup2 (which, on a setup2.xml miss, writes state 5 and tears "extra:/" down).
    if (reg::Get(L"pack_probe") == L"1") {
        Probe(mgr, e);
        Fail("pack_probe=1: diagnostic probe complete (see the [packstream] probe lines above); not completing the mount");
        return false;
    }

    if (!reinterpret_cast<Setup2Fn>(g_addr[kSetup2])(e, gen)) {
        Fail("DlcSetup2 returned false (setup2.xml not parsed; entry state %u). The pack needs a valid "
             "setup2.xml at its root", (unsigned)EntryState(e));
        return false;
    }
    GLOGI("hook: [packstream] DlcSetup2 ok (state %u)", (unsigned)EntryState(e));
    {
        // DlcSetup2 parses setup2.xml and sets the entry's nameHash from its <nameHash> (joaat); that is the
        // key RunPackGroup runs the change-set under. Re-read it so folder-mode (AddDlcPack left it 0/wrong)
        // and the packfile route both key on the pack's real identity.
        const uint32_t h2 = EntryHash(e);
        if (h2 && h2 != packHash)
            GLOGI("hook: [packstream] nameHash set by DlcSetup2: 0x%08x -> 0x%08x", (unsigned)packHash, (unsigned)h2);
        if (h2) packHash = h2;
    }
    if (!packHash) { Fail("entry nameHash is still zero after DlcSetup2; cannot run the change-set"); return false; }

    if (!reinterpret_cast<MountGateFn>(g_addr[kMountGate])(e, gen)) {
        Fail("MountGate returned false (dlc.rpf not mounted; entry state %u). This is where retail would "
             "reject the archive's encryption if it were OPEN", (unsigned)EntryState(e));
        return false;
    }
    GLOGI("hook: [packstream] MountGate ok (state %u) -- dlc.rpf is mounted", (unsigned)EntryState(e));

    // Loose models first, so the change set's vehicles.meta finds them by name.
    if (folderMode && !RegisterLooseStream(paths)) return false;   // Fail() already logged

    // The car list, while the pack's device is mounted (a dlc.rpf pack can only be read through it).
    const std::string mount = folderMode ? std::string() : EntryMountPoint(e);
    if (!folderMode) GLOGI("hook: [packstream] pack %s: mounted at \"%s\"", p.name.c_str(), mount.empty() ? "<unknown>" : mount.c_str());
    ReadPackModels(p, mount);
    std::vector<TunedHandlingItem> ngTuned;      // its handling, read while mounted; applied by the override pack at the end
    if (!folderMode) CollectNgTune(p, mount, ngTuned);

    // The change set takes one DLC vehicle slot per new car and the game never checks the array's end (see
    // kVehStoreSig): refuse a pack that does not fit rather than let it write past the array (0.4.15's crash).
    {
        std::lock_guard<std::mutex> lk(g_vehStoreMu);
        StoreHdr* h = ResolveVehStore() ? DlcVehStore() : nullptr;
        if (h) {
            const unsigned used = h->count, cap = h->cap, need = (unsigned)p.models.size();
            const unsigned left = cap > used ? cap - used : 0;
            GLOGI("hook: [packstream] pack %s: DLC vehicle store %u of %u used, %u free; the pack defines %u car(s)",
                  p.name.c_str(), used, cap, left, need);
            if (need > left || (need == 0 && left == 0)) {
                Fail("the DLC vehicle store is full (%u of %u used) and the pack defines %u car(s); its change set "
                     "would write past the end of the array. Raise veh_store_extra (HKCU\\Software\\GAGE-MP)", used, cap, need);
                return false;
            }
        } else {
            GLOGW("hook: [packstream] pack %s: the DLC vehicle store is not readable; running the change set without the "
                  "capacity check", p.name.c_str());
        }
    }

    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupEarlyOn);
    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupStartup);
    GLOGI("hook: [packstream] pack %s: ran EARLY_ON + STARTUP change-set groups (0x%08x)", p.name.c_str(), (unsigned)packHash);
    KeepNgTune(p, ngTuned);
    return true;
}

// Publish the menu's copy: every car of every mounted pack, the ones the cdimage has confirmed first-class.
void PublishVehicles() {
    // The cars in mount order; BuildCarList names, de-duplicates and sorts them (pack_meta.h).
    std::vector<CarIn> in;
    std::vector<const Pack*> packOf;
    std::vector<const PackModel*> modelOf;
    for (const Pack& p : g_packs)
        for (const PackModel& m : p.models) {
            in.push_back(CarIn{ m.v.model, m.v.make, m.v.label });
            packOf.push_back(&p);
            modelOf.push_back(&m);
        }
    std::vector<size_t> dropped;
    const std::vector<CarOut> list = BuildCarList(in, &dropped);
    static std::vector<std::string> s_warned;   // one line per repeated model, not one per publish
    for (const size_t d : dropped) {
        std::string key = in[d].model + "@" + packOf[d]->name;
        for (char& c : key) c = (char)tolower((unsigned char)c);
        if (std::find(s_warned.begin(), s_warned.end(), key) != s_warned.end()) continue;
        s_warned.push_back(key);
        const char* first = "?";
        for (size_t k = 0; k < in.size(); ++k)
            if (_stricmp(in[k].model.c_str(), in[d].model.c_str()) == 0) { first = packOf[k]->name.c_str(); break; }
        GLOGW("hook: [packstream] car '%s' of pack %s has the same model name as a car of pack %s: the list shows it once "
              "(both spawn the same model)", in[d].model.c_str(), packOf[d]->name.c_str(), first);
    }
    std::vector<PackVehicle> out;
    out.reserve(list.size());
    for (const CarOut& c : list) {
        const PackModel& m = *modelOf[c.src];
        PackVehicle v;
        v.model = m.v.model; v.name = c.name; v.make = m.v.make; v.label = m.v.label; v.vclass = m.v.vclass;
        v.pack = packOf[c.src]->name;
        v.ready = m.inCdimage;
        out.push_back(std::move(v));
    }
    std::lock_guard<std::mutex> lk(g_vehMutex);
    g_vehShared = std::move(out);
    ++g_vehGen;
}

// The last phase of both builds: poll the cars into the cdimage (2 passes a second), until all are in or the wait runs out.
// enh only changes the log prefix and the READY line (`[packenh] READY P of P ...`); Legacy's text is unchanged.
Status PollCdimage(World& world, bool enh) {
    const ULONGLONG now = GetTickCount64();
    if (now < g_pollAt) return g_status;
    g_pollAt = now + 500;
    size_t total = 0, in = 0, mounted = 0, packs = 0;
    bool changed = false;
    for (Pack& p : g_packs) {
        if (p.tune) continue;   // the handling override is not a content pack (logged on its own line)
        ++packs;
        if (!p.mounted) continue;
        ++mounted;
        for (PackModel& m : p.models) {
            ++total;
            if (!m.inCdimage && m.hash && ModelInCdimage(world, m.hash)) { m.inCdimage = true; changed = true; }
            if (m.inCdimage) ++in;
        }
    }
    if (changed) PublishVehicles();
    if (in == total || now - g_mountAt > kModelWaitMs) {
        for (const Pack& p : g_packs)
            for (const PackModel& m : p.models)
                if (p.mounted && !m.inCdimage)
                    GLOGW("%s pack %s: car '%s' (0x%08x) is NOT in the cdimage %llu ms after its change set "
                          "(its vehicles.meta did not register it)", enh ? "[packenh]" : "hook: [packstream]", p.name.c_str(), m.v.model.c_str(),
                          (unsigned)m.hash, (unsigned long long)(now - g_mountAt));
        g_status = mounted ? Status::Ready : Status::Failed;
        if (enh)
            GLOGI("[packenh] %s %u of %u pack(s) mounted, %u of %u car(s) in the cdimage", mounted ? "READY" : "FAILED", (unsigned)mounted, (unsigned)packs,
                  (unsigned)in, (unsigned)total);
        else
            GLOGI("hook: [packstream] %s: %u of %u pack(s) mounted, %u of %u car(s) in the cdimage", mounted ? "READY" : "FAILED",
                  (unsigned)mounted, (unsigned)packs, (unsigned)in, (unsigned)total);
    }
    return g_status;
}

// =============================================================================================
// GTA V Enhanced (pack_enh=2, Round B). The same flow as MountSequence with the Enhanced profile. The sequencing of the game
// calls is packenh::Mounter (pack_enh_mount_core.h, tested against a fake game in tests/pack_profile_test.cpp); this section
// wires it to the live game and does the parts around it (preflight, the loose stream files, the car list, the vehicle store
// check, the cdimage poll, the streaming probe). Game thread only, like Tick.
// Not ported: the NG handling tune (pack_ng_tune is ignored on Enhanced).
// =============================================================================================
using EnhRunGroupFn = void (*)(void* mgr, void* entry, uint32_t* groupHash);

struct EnhLive {
    bool inited = false;
    PackEnhMountInfo info;
    uintptr_t base = 0;
    uintptr_t addDlc = 0, setup2 = 0, mount = 0, atStr = 0, runGroup = 0;
    uint64_t vtableVa = 0;
};
EnhLive g_enh;

bool EnhRead(void*, uint64_t va, void* out, size_t n) { return win::SafeRead(reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), out, n); }
bool EnhWrite(void*, uint64_t va, const void* in, size_t n) { return win::SafeWrite(reinterpret_cast<void*>(static_cast<uintptr_t>(va)), in, n); }
uint8_t EnhAdd(void*, uint64_t mgr, const char* path) {
    return reinterpret_cast<AddDlcPackFn>(g_enh.addDlc)(reinterpret_cast<void*>(static_cast<uintptr_t>(mgr)), path);
}
uint8_t EnhSetup2(void*, uint64_t entry, uint32_t gen) {
    return reinterpret_cast<Setup2Fn>(g_enh.setup2)(reinterpret_cast<void*>(static_cast<uintptr_t>(entry)), gen);
}
int EnhMount(void*, uint64_t entry, uint32_t gen, uint64_t mountPointVa) {
    const char* mp = mountPointVa ? reinterpret_cast<const char*>(static_cast<uintptr_t>(mountPointVa)) : "";
    return reinterpret_cast<MountFn>(g_enh.mount)(reinterpret_cast<void*>(static_cast<uintptr_t>(entry)), gen, mp);
}
void EnhAppend(void*, uint64_t atString, const char* src, int len) {
    reinterpret_cast<AtStrAppendFn>(g_enh.atStr)(reinterpret_cast<void*>(static_cast<uintptr_t>(atString)), src, len);
}
void EnhGroup(void*, uint64_t mgr, uint64_t entry, uint32_t group) {
    uint32_t g = group;   // the game wants a pointer to the group hash
    reinterpret_cast<EnhRunGroupFn>(g_enh.runGroup)(reinterpret_cast<void*>(static_cast<uintptr_t>(mgr)), reinterpret_cast<void*>(static_cast<uintptr_t>(entry)), &g);
}
void EnhLog(void*, bool warn, const char* line) {
    if (warn) GLOGW("%s", line);
    else GLOGI("%s", line);
}

packenh::MountAbi EnhAbi() {
    packenh::MountAbi a;
    a.read = EnhRead;
    a.write = EnhWrite;
    a.addDlcPack = EnhAdd;
    a.dlcSetup2 = EnhSetup2;
    a.mount = EnhMount;
    a.atAppend = EnhAppend;
    a.runGroup = EnhGroup;
    a.log = EnhLog;
    return a;
}

uint64_t EnhManager() {
    uint64_t m = 0;
    return win::SafeRead(reinterpret_cast<const void*>(g_enh.base + g_enh.info.mgrGlobal), &m, 8) ? m : 0;
}

void EnhInit(const PackEnhMountInfo& info) {
    const Image im = GameImage();
    g_enh.info = info;
    g_enh.base = (uintptr_t)im.base;
    g_enh.addDlc = g_enh.base + info.addDlcPack;
    g_enh.setup2 = g_enh.base + info.dlcSetup2;
    g_enh.mount = g_enh.base + info.mount;
    g_enh.atStr = g_enh.base + info.atStr;
    g_enh.runGroup = g_enh.base + info.runPackGroup;
    g_enh.vtableVa = (uint64_t)g_enh.base + info.mgrVtable;
    g_enhStream.registryRva = (uint32_t)info.registry;
    g_enhStream.streamInfoRva = (uint32_t)info.streamInfo;
    g_enh.inited = true;
}

// The DLC vehicle store must have room for the pack's cars: its change set takes one slot per car and the game never checks the
// array's end (see PrepareVehicleStore). False (after Fail) when it would not fit.
bool EnhStoreCheck(const Pack& p) {
    std::lock_guard<std::mutex> lk(g_vehStoreMu);
    StoreHdr* h = ResolveVehStore() ? DlcVehStore() : nullptr;
    if (!h) {
        GLOGW("[packenh] pack %s: the DLC vehicle store is not readable; running the change set without the capacity check", p.name.c_str());
        return true;
    }
    const unsigned used = h->count, cap = h->cap, need = (unsigned)p.models.size();
    const unsigned left = cap > used ? cap - used : 0;
    GLOGI("[packenh] pack %s: DLC vehicle store %u of %u used, %u free; the pack defines %u car(s)", p.name.c_str(), used, cap, left, need);
    if (need > left || (need == 0 && left == 0)) {
        Fail("the DLC vehicle store is full (%u of %u used) and the pack defines %u car(s); its change set would write past the end of the "
             "array. Raise veh_store_extra (HKCU\\Software\\GAGE-MP)", used, cap, need);
        return false;
    }
    return true;
}

// One pack, start to finish. Every step logs before it runs ([packenh] lines); a failure is logged by the Mounter or by Fail.
bool EnhMountPack(Pack& p, size_t index, size_t total) {
    g_curPack = p.name;
    const uint64_t mgr = EnhManager();
    if (!mgr) { Fail("the ExtraContentManager singleton is null"); return false; }
    GLOGI("[packenh] pack %s (%u of %u): %s", p.name.c_str(), (unsigned)(index + 1), (unsigned)total,
          p.folder ? "loose folder (setup2.xml + stream/)" : "dlc.rpf, opened by the game's own loader");
    if (p.folder) {
        if (!FolderPreflight(p.paths)) return false;       // Fail() already logged
    } else if (!Preflight(p.paths.file)) {
        return false;
    }
    packenh::Mounter m(Prof(), EnhAbi(), mgr, g_enh.vtableVa);
    packenh::MountOut mo;
    if (!m.Add(p.name, p.paths.device, p.folder, mo)) {
        Fail("step %d/%d (%s): %s", mo.step, packenh::Mounter::kSteps, mo.stepName.c_str(), mo.why.c_str());
        return false;
    }
    // 5. the loose stream files, the car list, the vehicle store
    if (p.folder) {
        GLOGI("[packenh] pack %s: step 5/%d register the loose model files in %sstream/ with the game's streaming system", p.name.c_str(), packenh::Mounter::kSteps,
              p.paths.device.c_str());
        if (!RegisterLooseStream(p.paths)) return false;   // Fail() already logged
    } else {
        GLOGI("[packenh] pack %s: step 5/%d stream files: skipped (a dlc.rpf pack streams from its own archive)", p.name.c_str(), packenh::Mounter::kSteps);
    }
    const std::string mount = p.folder ? std::string() : m.MountPointOf(mo.entry);
    ReadPackModels(p, mount);
    if (!EnhStoreCheck(p)) return false;
    if (!m.RunChangeSets(p.name, mo)) {
        Fail("step %d/%d (%s): %s", mo.step, packenh::Mounter::kSteps, mo.stepName.c_str(), mo.why.c_str());
        return false;
    }
    std::string cars;
    for (const PackModel& pm : p.models) { if (!cars.empty()) cars += ", "; cars += pm.v.model; }
    GLOGI("[packenh] pack %s: MOUNTED, %u car(s) to find in the cdimage: %s", p.name.c_str(), (unsigned)p.models.size(), cars.empty() ? "-" : cars.c_str());
    return true;
}

// ---- the streaming probe ------------------------------------------------------------------------------------------------------
// After READY, every car model of every mounted pack is requested once, ONE AT A TIME, and released again, to answer the
// question this round exists for: do Legacy-built (gen8) vehicle assets stream on gen9 Enhanced. Each request is logged before
// it is made (a crash names the model). pack_enh_stream=0 switches the probe off.
struct ProbeItem { std::string pack, model; uint32_t hash = 0; };
struct StreamProbe {
    bool inited = false, on = false, done = false, requested = false;
    std::vector<ProbeItem> items;
    size_t next = 0;
    ULONGLONG startAt = 0, t0 = 0;
    unsigned loaded = 0, timeouts = 0;
};
StreamProbe g_probe;
constexpr ULONGLONG kProbeSettleMs = 4000;   // after READY: let the world reveal finish first
constexpr ULONGLONG kProbeMaxMs = 8000;      // per model

void EnhProbeTick(World& world) {
    StreamProbe& pr = g_probe;
    if (pr.done || g_status != Status::Ready) return;
    const ULONGLONG now = GetTickCount64();
    if (!pr.inited) {
        pr.inited = true;
        const std::wstring v = reg::Get(L"pack_enh_stream");
        pr.on = v != L"0";
        unsigned skipped = 0;
        for (const Pack& p : g_packs) {
            if (p.tune || !p.mounted) continue;
            for (const PackModel& m : p.models) {
                if (m.inCdimage && m.hash) pr.items.push_back({ p.name, m.v.model, m.hash });
                else ++skipped;
            }
        }
        pr.startAt = now + kProbeSettleMs;
        if (pr.on)
            GLOGI("[packenh] stream probe: %u car model(s) will be requested one at a time (%u not in the cdimage are skipped), %llu ms each at most, starting in %llu ms. "
                  "This answers whether Legacy-built vehicle assets stream on Enhanced; the last [packenh] stream line before a crash names the model. "
                  "pack_enh_stream=0 skips the probe.",
                  (unsigned)pr.items.size(), skipped, (unsigned long long)kProbeMaxMs, (unsigned long long)kProbeSettleMs);
        else
            GLOGI("[packenh] stream probe: off (pack_enh_stream=0)");
    }
    if (!pr.on) { pr.done = true; return; }
    if (now < pr.startAt) return;
    if (pr.next >= pr.items.size()) {
        pr.done = true;
        GLOGI("[packenh] stream probe finished: %u of %u car model(s) loaded, %u timed out", pr.loaded, (unsigned)pr.items.size(), pr.timeouts);
        return;
    }
    const ProbeItem& it = pr.items[pr.next];
    if (!pr.requested) {
        GLOGI("[packenh] stream: requesting '%s' (0x%08x) of pack %s (%u of %u)", it.model.c_str(), (unsigned)it.hash, it.pack.c_str(), (unsigned)(pr.next + 1),
              (unsigned)pr.items.size());
        world.RequestModel(it.hash);
        pr.requested = true;
        pr.t0 = now;
        return;
    }
    const bool ok = world.HasModelLoaded(it.hash);
    if (!ok && now - pr.t0 <= kProbeMaxMs) return;
    if (ok) {
        ++pr.loaded;
        GLOGI("[packenh] stream: '%s' LOADED after %llu ms", it.model.c_str(), (unsigned long long)(now - pr.t0));
    } else {
        ++pr.timeouts;
        GLOGW("[packenh] stream: '%s' NOT loaded after %llu ms (timeout); the model is released again", it.model.c_str(), (unsigned long long)(now - pr.t0));
    }
    world.ModelNoLongerNeeded(it.hash);
    pr.requested = false;
    ++pr.next;
}

// The Enhanced Tick: wait for the patterns phase, then one pack per tick, then the cdimage poll.
Status EnhTick(World& world) {
    if (!g_enh.inited) {
        PackEnhMountInfo info;
        const PackEnhGate gate = PackEnhMountGate(&info);
        if (gate == PackEnhGate::Pending) {
            g_status = Status::Pending;   // the world reveal waits (bounded) for the packs
            return g_status;
        }
        if (gate != PackEnhGate::Ready) {
            g_curPack = "*";
            if (gate == PackEnhGate::Refused) Fail("round B refused: %s", info.why);
            else Fail("pack_enh=2 but Round B was never armed (see the earlier [packenh] lines)");
            GLOGW("[packenh] FAILED: no pack was mounted and nothing was written: %s", g_fail);
            g_status = Status::Failed;
            return g_status;
        }
        EnhInit(info);
    }
    if (!g_listed) {
        g_listed = true;
        EnumeratePacks();
        if (g_packs.empty()) {
            g_curPack = "*";
            Fail("no pack folders to mount");
            GLOGW("[packenh] FAILED: no pack folders under %%LOCALAPPDATA%%\\GAGE-MP\\packs");
            g_status = Status::Failed;
            return g_status;
        }
        GLOGI("[packenh] mounting %u pack(s), one per script tick; NG handling tune is not ported to Enhanced (pack_ng_tune is ignored)", (unsigned)g_packs.size());
        g_status = Status::Pending;
        return g_status;
    }
    if (g_next < g_packs.size()) {
        const size_t idx = g_next++;
        Pack& p = g_packs[idx];
        p.mounted = EnhMountPack(p, idx, g_packs.size());
        if (!p.mounted) GLOGW("[packenh] pack %s FAILED (%s); the next pack is tried", p.name.c_str(), g_fail);
        for (PackModel& m : p.models) m.hash = world.HashKey(m.v.model.c_str());
        g_mountAt = GetTickCount64();
        g_pollAt = 0;
        PublishVehicles();
        return g_status;
    }
    return PollCdimage(world, true);
}

}  // namespace

// =============================================================================================
// Public API
// =============================================================================================
namespace {
void PrepareVehicleStoreLocked() {
    if (g_vehStoreDone) return;
    if (!Enabled()) { g_vehStoreDone = true; return; }   // Enabled() logged why

    long extra = kVehStoreExtraDefault;
    const std::wstring v = reg::Get(L"veh_store_extra");
    if (!v.empty()) {
        wchar_t* stop = nullptr;
        const long n = wcstol(v.c_str(), &stop, 10);
        if (stop && *stop == L'\0' && n >= 0 && n <= 8192) extra = n;
        else GLOGW("hook: [packstream] veh_store_extra=\"%s\" is not a number 0..8192; using %ld", Narrow(v).c_str(), extra);
    }
    if (extra == 0) {
        GLOGW("hook: [packstream] DLC vehicle store: not raised (veh_store_extra=0); packs that do not fit are refused");
        g_vehStoreDone = true;
        return;
    }
    if (v.empty() && &Prof() == &kPackEnhanced) {
        // Enhanced (Round B): the live limit is already 640 (round A, 0.4.25) with 0 used; a raise is one more unproven write into a
        // game structure for room the packs do not need. veh_store_extra=N raises it anyway; a pack that does not fit is refused.
        GLOGI("[packenh] DLC vehicle store: MaxExtraVehicleModelInfos is not raised by default on Enhanced (set veh_store_extra=N to raise it); "
              "a pack that does not fit is refused before its change set runs");
        g_vehStoreDone = true;
        return;
    }
    if (!ResolveVehStore()) {
        if (g_vehStoreSig < 0) g_vehStoreDone = true;   // final: logged by ResolveVehStore
        return;                                         // not found yet: the next call retries
    }
    if (StoreHdr* h = DlcVehStore()) {
        GLOGW("hook: [packstream] DLC vehicle store already created (%u of %u used) before the engine could raise it; "
              "packs that do not fit are refused", (unsigned)h->count, (unsigned)h->cap);
        g_vehStoreDone = true;
        return;
    }
    uint32_t* lim = VehStoreLimit();
    if (!lim) return;                                   // gameconfig not parsed yet: the next call retries
    const uint32_t cur = *lim;
    if (cur == 0 || cur > 0xFFFF) {
        GLOGW("hook: [packstream] DLC vehicle store: MaxExtraVehicleModelInfos reads %u (implausible); not raised", (unsigned)cur);
        g_vehStoreDone = true;
        return;
    }
    if (!IsWritable(lim, 4)) {
        GLOGW("hook: [packstream] DLC vehicle store: the gameconfig limit is not writable; not raised");
        g_vehStoreDone = true;
        return;
    }
    const uint32_t want = (cur + (uint32_t)extra > 0xFFFFu) ? 0xFFFFu : cur + (uint32_t)extra;   // the capacity is a WORD
    *lim = want;
    GLOGI("hook: [packstream] DLC vehicle store: MaxExtraVehicleModelInfos %u -> %u (+%ld, veh_store_extra) before the "
          "game creates the array", (unsigned)cur, (unsigned)*lim, extra);
    g_vehStoreDone = true;
}
}  // namespace

bool PrepareVehicleStore() {
    std::lock_guard<std::mutex> lk(g_vehStoreMu);
    PrepareVehicleStoreLocked();
    return g_vehStoreDone;
}

Status Tick(World& world) {
    if (g_status == Status::Ready || g_status == Status::Failed) {
        if (g_enh.inited) EnhProbeTick(world);   // Enhanced only: the streaming probe, after the packs settled
        return g_status;
    }
    if (!Enabled())
        return Status::Idle;
    if (!g_vehStoreSeen) {
        std::lock_guard<std::mutex> lk(g_vehStoreMu);
        if (!g_vehStoreDone)
            GLOGW("hook: [packstream] DLC vehicle store: still undecided when the player is in the world (gameconfig "
                  "or signature never became readable); packs that do not fit are refused");
        g_vehStoreDone = true;   // too late to raise now; stop the attach-time retry
        g_vehStoreSeen = true;
        if (StoreHdr* h = ResolveVehStore() ? DlcVehStore() : nullptr)
            GLOGI("hook: [packstream] DLC vehicle store in the world: %u of %u used before our packs", (unsigned)h->count,
                  (unsigned)h->cap);
        else
            GLOGW("hook: [packstream] DLC vehicle store not found in the world (signature %d)", g_vehStoreSig);
    }

    // GTA V Enhanced (pack_enh=2): its own flow with the addresses Round A and the extra patterns resolved (EnhTick).
    if (&Prof() == &kPackEnhanced)
        return EnhTick(world);

    // One-time: resolve and log the signatures + the manager global. Even with no pack present this is
    // a complete live validation of the 3889 recon.
    if (!ResolveAll()) {
        g_curPack = "*";
        Fail("not every ExtraContentManager signature resolved uniquely on this image (see the [packstream] lines)");
        g_status = Status::Failed;
        return g_status;
    }

    if (!g_listed) {
        // Wait for the manager singleton to exist (it is built during boot; it should already be up by the
        // time the player is in the world, but give it a bounded grace period rather than failing at once).
        if (!Manager()) {
            if (!g_mgrSince) g_mgrSince = GetTickCount64();
            if (GetTickCount64() - g_mgrSince > kMgrWaitMs) {
                g_curPack = "*";
                Fail("the ExtraContentManager singleton stayed null for %llu ms", (unsigned long long)kMgrWaitMs);
                g_status = Status::Failed;
                return g_status;
            }
            return Status::Idle;   // try again next tick
        }
        g_listed = true;
        EnumeratePacks();
        if (g_packs.empty()) {
            g_curPack = "*";
            Fail("no pack folders to mount");
            g_status = Status::Failed;
            return g_status;
        }
        g_status = Status::Pending;
        return g_status;
    }

    // One pack per tick: a change set parses its metas synchronously, so a long list must not stall one frame.
    if (g_next < g_packs.size()) {
        Pack& p = g_packs[g_next++];
        p.mounted = MountSequence(world, p);
        if (p.tune)
            GLOGI("engine: NG handling tune (0.4.18): %u item(s) from %s, override pack %s (live-unverified: the game's "
                  "HANDLING_FILE loader is expected to replace a handlingName in place)", (unsigned)g_ngTuned.size(),
                  g_ngTunePacks.c_str(), p.mounted ? "mounted" : "failed");
        for (PackModel& m : p.models) m.hash = world.HashKey(m.v.model.c_str());
        g_mountAt = GetTickCount64();
        g_pollAt = 0;
        PublishVehicles();
        return g_status;
    }

    // After the last pack: the NG packs' tuned handling, as one override pack mounted after all of them (next tick).
    if (!g_tuneQueued) {
        g_tuneQueued = true;
        if (!g_ngTuned.empty()) {
            Pack tp;
            if (BuildTunePack(tp)) {
                g_curPack = tp.name;
                g_packs.push_back(std::move(tp));
                return g_status;
            }
            GLOGW("engine: NG handling tune (0.4.18): %u item(s) from %s, override pack failed (it could not be written)",
                  (unsigned)g_ngTuned.size(), g_ngTunePacks.c_str());
        } else if (g_ngTuneSeen && g_ngTuneOn == 1) {
            GLOGI("engine: NG handling tune (0.4.18): 0 item(s): no rule changed a mounted NG pack's handling; no override pack");
        }
    }

    // Every pack ran: poll the cars into the cdimage (2 passes a second), until all are in or the wait runs out.
    return PollCdimage(world, false);
}

const char* FailReason() {
    return g_status == Status::Failed ? g_fail : "";
}

std::vector<PackVehicle> Vehicles() {
    std::lock_guard<std::mutex> lk(g_vehMutex);
    return g_vehShared;
}

uint32_t VehiclesGeneration() {
    std::lock_guard<std::mutex> lk(g_vehMutex);
    return g_vehGen;
}

const char* PackOfModel(uint32_t modelHash) {
    if (!modelHash) return nullptr;
    for (const Pack& p : g_packs)
        for (const PackModel& m : p.models)
            if (m.hash == modelHash) return p.name.c_str();
    return nullptr;
}

}  // namespace gage::game::content
