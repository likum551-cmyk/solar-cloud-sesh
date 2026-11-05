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

#include "pack_meta.h"

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

constexpr uint32_t kVtableRva    = 0x1a9c508u;   // *mgr == base + this, from the manager ctor (p2 section 5)
constexpr uint32_t kOffArray     = 0x28;         // mgr -> DlcEntry* array
constexpr uint32_t kOffCount     = 0x30;         // mgr -> WORD entry count
constexpr uint32_t kOffGen       = 0x178;        // mgr -> DWORD generation counter (edx to Setup2/MountGate)
constexpr uint32_t kEntryStride  = 0xf0;
constexpr uint32_t kOffEntryHash = 0x60;         // DlcEntry -> DWORD name hash (the RunPackGroup key)
constexpr uint32_t kOffEntryPath = 0xb8;         // DlcEntry -> const char* path (Mount loads it as a QWORD ptr)
constexpr uint32_t kOffEntryPathLen = 0xc0;      // DlcEntry -> WORD length of the +0xb8 atString (capacity WORD at +0xc2);
                                                 // Mount ignores +0xb8 while it is 0
constexpr uint32_t kOffEntryType = 0xc8;         // DlcEntry -> DWORD device type: 2 = packfile, 1 = relative/folder
constexpr uint32_t kOffEntryState= 0xd0;         // DlcEntry -> DWORD setup state (0 fresh, 1 parsed, 2 mounted)
constexpr uint32_t kTypePackfile = 2;            // Mount 0x94307c: +0xc8==2 -> fiPackfile::Open (the crypto path)
constexpr uint32_t kTypeRelative = 1;            // Mount 0x94307c: +0xc8==1 -> fiDeviceRelative over +0xb8 (loose folder)

// Diagnostic probe (pack_probe=1): the fiAssetManager global and its mount/file-exists calls, used to mount
// the pack under "extra:/" WITHOUT DlcSetup2's teardown and ask the game which pack files are visible.
constexpr uint32_t kAssetGlobalRva = 0x1e2ee90u; // &ASSET; DlcSetup2 lea's it before PushFolder/Exists/PopFolder

// 3889 hash of IS_MODEL_IN_CDIMAGE; World::CallNative maps public hashes and falls back to the hash as given.
constexpr uint64_t kNativeIsModelInCdimage = 0x35B9E0803292B641ULL;

constexpr ULONGLONG kModelWaitMs = 15000;        // after the change set, how long to wait for the model
constexpr ULONGLONG kMgrWaitMs   = 10000;        // how long to wait for the manager singleton to appear

// ---------------------------------------------------------------------------------------------
// Signatures. Each pattern is a function entry (address = the match itself). The singleton global is
// read out of the RunPackGroup body (a "48 8B 0D disp32" at match+13), not hard-coded.
// ---------------------------------------------------------------------------------------------
enum SigId : int { kAddDlcPack, kSetup2, kMountGate, kRunPackGroup, kSigCount };

struct Sig { const char* name; const char* pat; const char* source; };

const Sig kSigs[kSigCount] = {
    { "AddDlcPack",   // RVA 0x916734: bool(mgr /*rcx*/, const char* path /*rdx, narrow, ends '/'*/)
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D A8 E8 FE FF FF 48 81 EC 10 02 00 00 48 8B F9 48 8B DA 48 8D 4D 11",
      "recon-3889 1.2 / p2 section 1" },
    { "DlcSetup2",    // RVA 0x958290: bool(entry /*rcx*/, uint32 gen /*edx, forwarded*/)
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 8B 89 D0 00 00 00",
      "recon-3889 2.2 / p2 section 3" },
    { "MountGate",    // RVA 0x94301c: bool(entry /*rcx*/, uint32 gen /*edx, forwarded to Mount*/)
      "48 89 5C 24 08 57 48 83 EC 20 8B 81 D0 00 00 00 48 8B D9 83 F8 02 75 04",
      "recon-3889 2.4 / p2 section 3" },
    { "RunPackGroup", // RVA 0x92a9ac: void(mgr /*rcx*/, uint32 packHash /*edx*/, uint32 groupHash /*r8d*/)
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 41 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 14 4C 8D 44 24 48 48 8B",
      "recon-3889 2.5 / p2 section 4" },
};

// Diagnostic-only signatures, resolved lazily and independently of the four above (a probe-sig miss must
// never disable the normal mount). Mount(entry, gen, mountPoint) opens+mounts the archive and returns 0 on
// success; PushFolder/Exists/PopFolder are the fiAssetManager calls DlcSetup2 itself uses to test setup2.xml.
enum ProbeSigId : int { kMount, kPushFolder, kExists, kPopFolder, kProbeSigCount };

const Sig kProbeSigs[kProbeSigCount] = {
    { "Mount",       // RVA 0x94307c: int(entry /*rcx*/, uint32 gen /*edx*/, const char* mountPoint /*r8*/); 0=ok
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 45 33 FF 49 8B F0 48 8B D9 4C 39 39",
      "recon-3889 2.3 / p2 section 3" },
    { "PushFolder",  // RVA 0x1367bb8: void(asset /*rcx*/, const char* folder /*rdx*/)
      "48 89 5C 24 18 48 89 54 24 10 57 48 81 EC 20 01 00 00 48 8B D9 48 8D 0D ?? ?? ?? ?? 48 8B FA E8 ?? ?? ?? ?? 80 3F 24 74",
      "fiAssetManager::PushFolder (3889)" },
    { "Exists",      // RVA 0x1362be8: uint8(asset /*rcx*/, const char* name /*rdx*/, uint64 flags /*r8=0*/)
      "48 81 EC 38 01 00 00 4C 8B CA 4C 89 44 24 20 48 8D 54 24 30 41 B8 00 01 00 00 E8 ?? ?? ?? ?? 48 81 C4 38 01 00 00 C3",
      "fiAssetManager::Exists (3889)" },
    { "PopFolder",   // RVA 0x13679b4: void(asset /*rcx*/)
      "FF 89 00 0C 00 00 48 8D 0D ?? ?? ?? ?? E9 ?? ?? ?? ??",
      "fiAssetManager::PopFolder (3889)" },
};

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
const Sig kRegStreamSig = {
    "RegStreamFile",  // RVA 0x169e4d4 (unique in both 3889 dumps)
    "48 89 5C 24 08 48 89 6C 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 50 48 8B EA 4C 8B FA 48 8B D9 "
    "4D 85 C9 B2 01 48 8B CD 45 8A E0 4D 0F 45 F9 E8",
    "3889 RE 2026-11-05: device -> RawStreamer +0x1a8 -> streaming-info register 0x169e5e0" };
using RegStreamFileFn = int* (*)(int* out, const char* path, uint8_t one, const char* name,
                                 uint8_t fatalIfMissing, uint8_t allowOverride);
constexpr uint32_t kStreamInfoMgrRva = 0x2ea23d0u;  // strStreamingInfoManager: [+0] 8-byte (handle, flags) array, [+8] count
constexpr uint32_t kImageRegistryRva = 0x2c67400u;  // fiCollection* per image index
constexpr uint32_t kRawStreamerVtRva = 0x1b7d5e0u;  // image 0's collection (live fidiag 2026-11-02)

// ---------------------------------------------------------------------------------------------
// Image bounds, readable-page runs, pattern scan (self-contained; mirrors dlc_mount.cpp).
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

// The committed, readable stretches of [base, base+size), adjacent ones merged: the Arxan-packed image
// can hold guard/reserved pages, and a scan that walks into one faults the game.
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

bool ParsePat(const char* s, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char* tok = s;
        while (*tok && *tok != ' ') ++tok;
        const int len = (int)(tok - s);
        if (len == 2 && s[0] == '?' && s[1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
        } else if (len == 2 && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) {
            bytes.push_back((uint8_t)(hexv(s[0]) * 16 + hexv(s[1])));
            mask.push_back(1);
        } else {
            return false;
        }
        s = tok;
    }
    return !bytes.empty();
}

// Count matches (capped at 1000); `first` receives the lowest-address one. -1 = malformed pattern.
int CountPat(const std::vector<Run>& runs, const char* sig, const uint8_t** first) {
    std::vector<uint8_t> bytes, mask;
    if (first) *first = nullptr;
    if (!ParsePat(sig, bytes, mask)) return -1;
    const size_t L = bytes.size();
    size_t lead = 0;
    while (lead < L && !mask[lead]) ++lead;
    if (lead == L) return -1;              // all wildcards
    int count = 0;
    for (const Run& r : runs) {
        if (r.n < L) continue;
        const uint8_t* p = r.p + lead;
        const uint8_t* last = r.p + r.n - (L - lead);
        while (p <= last) {
            p = static_cast<const uint8_t*>(memchr(p, bytes[lead], (size_t)(last - p) + 1));
            if (!p) break;
            const uint8_t* s = p - lead;
            bool ok = true;
            for (size_t i = 0; i < L; ++i)
                if (mask[i] && s[i] != bytes[i]) { ok = false; break; }
            if (ok) {
                if (first && !*first) *first = s;
                if (++count >= 1000) return count;
            }
            ++p;
        }
    }
    return count;
}

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
        const int c = CountPat(runs, kSigs[i].pat, &first);
        if (c < 0) {
            GLOGW("hook: [packstream]   %-14s BAD PATTERN", kSigs[i].name);
            all = false;
        } else if (c == 0) {
            GLOGW("hook: [packstream]   %-14s missing (0)   (%s)", kSigs[i].name, kSigs[i].source);
            all = false;
        } else if (c > 1) {
            GLOGW("hook: [packstream]   %-14s ambiguous (%d, first @ +0x%llx); not used", kSigs[i].name, c,
                  (unsigned long long)(uintptr_t)(first - im.base));
            all = false;
        } else {
            g_addr[i] = (uintptr_t)first;
            g_ok[i] = true;
            GLOGI("hook: [packstream]   %-14s UNIQUE @ +0x%llx   (%s)", kSigs[i].name,
                  (unsigned long long)(uintptr_t)(first - im.base), kSigs[i].source);
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
    return *reinterpret_cast<uintptr_t*>(mgr) == (uintptr_t)im.base + kVtableRva;
}
uint16_t Count(void* mgr)   { return IsReadable((char*)mgr + kOffCount, 2) ? *reinterpret_cast<uint16_t*>((char*)mgr + kOffCount) : 0; }
uint32_t Gen(void* mgr)     { return IsReadable((char*)mgr + kOffGen, 4) ? *reinterpret_cast<uint32_t*>((char*)mgr + kOffGen) : 0; }
char*    Entry(void* mgr, int i) {
    if (i < 0 || !IsReadable((char*)mgr + kOffArray, 8)) return nullptr;
    char* arr = *reinterpret_cast<char**>((char*)mgr + kOffArray);
    char* e = arr + (size_t)i * kEntryStride;
    return IsReadable(e, kEntryStride) ? e : nullptr;
}
uint32_t EntryHash(char* e)  { return (e && IsReadable(e + kOffEntryHash, 4)) ? *reinterpret_cast<uint32_t*>(e + kOffEntryHash) : 0; }
uint32_t EntryState(char* e) { return (e && IsReadable(e + kOffEntryState, 4)) ? *reinterpret_cast<uint32_t*>(e + kOffEntryState) : 0xffffffffu; }

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
    std::vector<PackModel> models;
};
std::vector<Pack> g_packs;
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
        const int c = CountPat(runs, kProbeSigs[i].pat, &first);
        if (c == 1) {
            g_probeAddr[i] = (uintptr_t)first;
            GLOGI("hook: [packstream] probe-sig %-11s UNIQUE @ +0x%llx   (%s)", kProbeSigs[i].name,
                  (unsigned long long)(uintptr_t)(first - im.base), kProbeSigs[i].source);
        } else {
            GLOGW("hook: [packstream] probe-sig %-11s %s", kProbeSigs[i].name,
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
    void* asset = reinterpret_cast<void*>((uintptr_t)im.base + kAssetGlobalRva);
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
const Sig kGetDeviceSig = {
    "GetDevice",      // RVA 0x1364150 (the jmp target of 0x1364148, which every device-for-path caller uses)
    "48 89 5C 24 08 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8D 15 ?? ?? ?? ?? 41 B8 07 00 00 00 48 8B F1 E8",
    "3889 RE 2026-11-05: fiDevice::GetDevice(path, readOnly)" };
constexpr size_t kMaxMetaBytes = 4u << 20;   // a vehicles.meta is a few KB; anything this big is not one

bool ReadGameFile(const std::string& path, std::string& out) {
    static uintptr_t s_fn = 0;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        const Image im = GameImage();
        const uint8_t* first = nullptr;
        const int c = (im.base && im.size) ? CountPat(ReadableRuns(im), kGetDeviceSig.pat, &first) : 0;
        if (c == 1) s_fn = (uintptr_t)first;
        GLOGI("hook: [packstream] %s %s", kGetDeviceSig.name, c == 1 ? "UNIQUE" : (c == 0 ? "missing" : "ambiguous"));
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
std::string EntryMountPoint(char* e) {
    if (!e || !IsReadable(e + 0x30, 10)) return "";
    const char* s = *reinterpret_cast<const char* const*>(e + 0x30);
    const uint16_t n = *reinterpret_cast<const uint16_t*>(e + 0x38);
    if (!s || n == 0 || n > 120 || !IsReadable(s, n)) return "";
    std::string m(s, n);
    for (char c : m) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return "";
    if (m.find(':') == std::string::npos) return "";
    if (m.back() != '/') m.push_back('/');
    return m;
}

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
    const int hits = CountPat(ReadableRuns(im), kRegStreamSig.pat, &fn);
    if (hits != 1 || !fn) {
        Fail("stream: %s signature %s (%d hits); not registering %u model files", kRegStreamSig.name,
             hits == 0 ? "missing" : "ambiguous", hits, (unsigned)files.size());
        return false;
    }
    const uintptr_t base = (uintptr_t)im.base;
    void* const* registry = reinterpret_cast<void* const*>(base + kImageRegistryRva);
    void* raw = IsReadable(registry, 8) ? registry[0] : nullptr;
    const uintptr_t rawVt = (raw && IsReadable(raw, 8)) ? *reinterpret_cast<const uintptr_t*>(raw) : 0;
    GLOGI("hook: [packstream] stream: %s UNIQUE @ +0x%llx; image 0 collection @0x%llx vtable +0x%llx (%s)",
          kRegStreamSig.name, (unsigned long long)(fn - im.base), (unsigned long long)(uintptr_t)raw,
          (unsigned long long)(rawVt ? rawVt - base : 0),
          rawVt == base + kRawStreamerVtRva ? "RawStreamer, as expected" : "NOT the RawStreamer");
    if (rawVt != base + kRawStreamerVtRva) {
        Fail("stream: image 0 is not the RawStreamer collection on this image; not registering loose files");
        return false;
    }

    const auto* mgr = reinterpret_cast<const uint8_t*>(base + kStreamInfoMgrRva);
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
        const uint32_t count = IsReadable(mgr + 8, 4) ? *reinterpret_cast<const uint32_t*>(mgr + 8) : 0;
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
        if (!IsReadable(e + kOffEntryType, 4) || !IsReadable(e + kOffEntryPath, 12)) {
            Fail("folder-mode: DLC entry device fields (+0xb8/+0xc0/+0xc8) are not writable"); return false;
        }
        const uint32_t oldType = *reinterpret_cast<uint32_t*>(e + kOffEntryType);
        const uint16_t oldLen = *reinterpret_cast<uint16_t*>(e + kOffEntryPathLen);
        const char* oldPath = *reinterpret_cast<const char* const*>(e + kOffEntryPath);
        GLOGI("hook: [packstream] folder-mode: pre-override type(+0xc8)=%u length(+0xc0)=%u capacity(+0xc2)=%u path(+0xb8)=\"%s\"",
              (unsigned)oldType, (unsigned)oldLen, (unsigned)*reinterpret_cast<uint16_t*>(e + kOffEntryPathLen + 2),
              (oldPath && IsReadable(oldPath, 1)) ? oldPath : "<null>");
        *reinterpret_cast<uint16_t*>(e + kOffEntryPathLen) = 0;
        reinterpret_cast<AtStrAppendFn>(g_strAppend)(e + kOffEntryPath, folder.c_str(), (int)folder.size());
        const char* newPath = *reinterpret_cast<const char* const*>(e + kOffEntryPath);
        const uint16_t newLen = *reinterpret_cast<uint16_t*>(e + kOffEntryPathLen);
        if (newLen != folder.size() || !newPath || !IsReadable(newPath, folder.size() + 1) ||
            memcmp(newPath, folder.c_str(), folder.size() + 1) != 0) {
            Fail("folder-mode: the entry path did not read back as the folder after the game's append (length %u)", (unsigned)newLen);
            return false;
        }
        *reinterpret_cast<uint32_t*>(e + kOffEntryType) = kTypeRelative;
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

    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupEarlyOn);
    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupStartup);
    GLOGI("hook: [packstream] pack %s: ran EARLY_ON + STARTUP change-set groups (0x%08x)", p.name.c_str(), (unsigned)packHash);
    return true;
}

// Publish the menu's copy: every car of every mounted pack, the ones the cdimage has confirmed first-class.
void PublishVehicles() {
    std::vector<PackVehicle> out;
    for (const Pack& p : g_packs)
        for (const PackModel& m : p.models) {
            PackVehicle v;
            v.model = m.v.model; v.make = m.v.make; v.label = m.v.label; v.vclass = m.v.vclass; v.pack = p.name;
            v.ready = m.inCdimage;
            out.push_back(std::move(v));
        }
    std::sort(out.begin(), out.end(), [](const PackVehicle& a, const PackVehicle& b) {
        const int c = _stricmp(a.make.c_str(), b.make.c_str());
        return c != 0 ? c < 0 : _stricmp(a.model.c_str(), b.model.c_str()) < 0;
    });
    std::lock_guard<std::mutex> lk(g_vehMutex);
    g_vehShared = std::move(out);
    ++g_vehGen;
}

}  // namespace

// =============================================================================================
// Public API
// =============================================================================================
Status Tick(World& world) {
    if (g_status == Status::Ready || g_status == Status::Failed)
        return g_status;
    if (!Enabled())
        return Status::Idle;

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
        for (PackModel& m : p.models) m.hash = world.HashKey(m.v.model.c_str());
        g_mountAt = GetTickCount64();
        g_pollAt = 0;
        PublishVehicles();
        return g_status;
    }

    // Every pack ran: poll the cars into the cdimage (2 passes a second), until all are in or the wait runs out.
    const ULONGLONG now = GetTickCount64();
    if (now < g_pollAt) return g_status;
    g_pollAt = now + 500;
    size_t total = 0, in = 0, mounted = 0;
    bool changed = false;
    for (Pack& p : g_packs) {
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
                    GLOGW("hook: [packstream] pack %s: car '%s' (0x%08x) is NOT in the cdimage %llu ms after its change set "
                          "(its vehicles.meta did not register it)", p.name.c_str(), m.v.model.c_str(), (unsigned)m.hash,
                          (unsigned long long)(now - g_mountAt));
        g_status = mounted ? Status::Ready : Status::Failed;
        GLOGI("hook: [packstream] %s: %u of %u pack(s) mounted, %u of %u car(s) in the cdimage", mounted ? "READY" : "FAILED",
              (unsigned)mounted, (unsigned)g_packs.size(), (unsigned)in, (unsigned)total);
    }
    return g_status;
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
