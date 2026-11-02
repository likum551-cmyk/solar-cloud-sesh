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

#include <cstdarg>
#include <cstdint>
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
constexpr uint32_t kOffEntryState= 0xd0;         // DlcEntry -> DWORD setup state (0 fresh, 1 parsed, 2 mounted)

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

    g_resolveReady = all;
    GLOGI("hook: [packstream] signature resolution %s", all ? "COMPLETE" : "INCOMPLETE (mount will not run)");
    return all;
}

// ---------------------------------------------------------------------------------------------
// Function-pointer views of the resolved addresses (Windows x64 ABI: rcx, rdx, r8).
// ---------------------------------------------------------------------------------------------
using AddDlcPackFn   = uint8_t (*)(void* mgr, const char* path);
using Setup2Fn       = uint8_t (*)(void* entry, uint32_t gen);
using MountGateFn    = uint8_t (*)(void* entry, uint32_t gen);
using RunPackGroupFn = void    (*)(void* mgr, uint32_t packHash, uint32_t groupHash);

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
char      g_fail[192] = "";
ULONGLONG g_mgrSince  = 0;        // when we first started waiting for the manager singleton
ULONGLONG g_mountAt   = 0;        // when the change set ran (start of the model wait)
uint32_t  g_modelHash = 0;
bool      g_attempted = false;    // the mount chain has been run once (success or fail)

void Fail(const char* fmt, ...) GAGE_PRINTF(1, 2);
void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail, sizeof(g_fail), fmt, ap);
    va_end(ap);
    g_status = Status::Failed;
    GLOGW("hook: [packstream] gave up: %s", g_fail);
}

bool Enabled() {
    if (g_enabled < 0) {
        g_enabled = (reg::Get(L"pack_mount") == L"1") ? 1 : 0;
        GLOGI("hook: [packstream] pack mount %s (HKCU\\Software\\GAGE-MP pack_mount=%s)",
              g_enabled ? "ARMED" : "off", g_enabled ? "1" : "unset or not 1");
    }
    return g_enabled == 1;
}

// The pack's device path (handed to AddDlcPack, ends in '/') and the on-disk dlc.rpf path (preflighted).
struct Paths { std::string device; std::wstring file; };

bool ResolvePaths(Paths& out) {
    const std::wstring name = reg::Get(L"pack_name", L"gage_jesko");
    std::wstring file = reg::Get(L"pack_path");
    if (file.empty()) {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        if (!local || !*local) { Fail("LOCALAPPDATA is not set and pack_path is unset"); return false; }
        file = std::wstring(local) + L"\\GAGE-MP\\packs\\" + name + L"\\dlc.rpf";
    }
    out.file = file;

    const std::wstring dev = reg::Get(L"pack_device");
    if (!dev.empty()) {
        // Verbatim device form (e.g. "platform:/dlcpacks/<name>/"), must be narrow ASCII + end in '/'.
        for (wchar_t c : dev)
            if (c > 0x7E) { Fail("pack_device has a non-ASCII character; the game takes a narrow path"); return false; }
        out.device = Narrow(dev);
        if (out.device.empty() || out.device.back() != '/') out.device.push_back('/');
        return true;
    }

    // Default: the folder that holds dlc.rpf, as a forward-slash OS path ending in '/'.
    size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { Fail("pack_path has no folder component: %s", Narrow(file).c_str()); return false; }
    std::wstring folder = file.substr(0, slash + 1);
    for (wchar_t c : folder)
        if (c > 0x7E) { Fail("the pack folder path has a non-ASCII character; the game takes a narrow path"); return false; }
    out.device.clear();
    for (wchar_t c : folder) out.device.push_back(c == L'\\' ? '/' : (char)c);
    if (out.device.size() > 248)
        GLOGW("hook: [packstream] device path is %u chars; AddDlcPack truncates past ~248", (unsigned)out.device.size());
    return true;
}

// Read and sanity-check the pack's RPF7 header before the game ever opens it. A header the retail game
// would reject (OPEN, or not RPF7) must never reach the loader: it ends the game with a fatal error.
bool Preflight(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Fail("pack not found or unreadable: %s (set pack_path, or drop dlc.rpf there)", Narrow(file).c_str());
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

bool ModelInCdimage(World& world, uint32_t modelHash) {
    const uint64_t a[] = { (uint64_t)modelHash };
    uint64_t ret[4] = {};
    if (!world.CallNative(kNativeIsModelInCdimage, a, 1, ret)) return false;
    return (ret[0] & 0xff) != 0;
}

// Run the whole chain once. Game thread. Sets g_status = Failed on any refusal; returns true when the
// chain ran to the point of waiting for the model (g_status set to Pending by the caller).
bool MountSequence(World& world) {
    void* mgr = Manager();
    if (!mgr) { Fail("ExtraContentManager singleton is null after the wait (boot did not finish?)"); return false; }
    if (!VtableOk(mgr)) { Fail("manager vtable mismatch at @0x%llx; refusing to call into it", (unsigned long long)(uintptr_t)mgr); return false; }
    // Confirmed before touching the pack, so a signature-only run (pack_mount=1, no pack) still validates
    // that the singleton dereferences to a live manager with a sane pack count and the expected vtable.
    GLOGI("hook: [packstream] manager @0x%llx live, vtable OK, %u packs currently loaded",
          (unsigned long long)(uintptr_t)mgr, (unsigned)Count(mgr));

    Paths paths;
    if (!ResolvePaths(paths)) return false;              // Fail() already logged
    if (!Preflight(paths.file)) return false;            // Fail() already logged
    GLOGI("hook: [packstream] AddDlcPack path \"%s\"", paths.device.c_str());

    const uint16_t n0 = Count(mgr);
    const uint8_t added = reinterpret_cast<AddDlcPackFn>(g_addr[kAddDlcPack])(mgr, paths.device.c_str());
    const uint16_t n1 = Count(mgr);
    GLOGI("hook: [packstream] AddDlcPack returned %u; pack count %u -> %u", (unsigned)added, (unsigned)n0, (unsigned)n1);
    if (!added && n1 == n0) {
        Fail("AddDlcPack refused \"%s\": the game's device layer could not resolve \"<path>dlc.rpf\". "
             "Try the proven form via pack_device=platform:/dlcpacks/<name>/ with the pack under the game dir",
             paths.device.c_str());
        return false;
    }
    const int idx = (int)n1 - 1;
    char* e = Entry(mgr, idx);
    if (!e) { Fail("the new DLC entry at index %d is not readable", idx); return false; }
    const uint32_t packHash = EntryHash(e);
    const uint32_t gen = Gen(mgr);
    GLOGI("hook: [packstream] entry @0x%llx idx %d: nameHash 0x%08x, state %u, gen %u",
          (unsigned long long)(uintptr_t)e, idx, (unsigned)packHash, (unsigned)EntryState(e), (unsigned)gen);
    if (!packHash) { Fail("the new DLC entry has a zero name hash; AddDlcPack did not populate it"); return false; }

    if (!reinterpret_cast<Setup2Fn>(g_addr[kSetup2])(e, gen)) {
        Fail("DlcSetup2 returned false (setup2.xml not parsed; entry state %u). The pack needs a valid "
             "setup2.xml at its root", (unsigned)EntryState(e));
        return false;
    }
    GLOGI("hook: [packstream] DlcSetup2 ok (state %u)", (unsigned)EntryState(e));

    if (!reinterpret_cast<MountGateFn>(g_addr[kMountGate])(e, gen)) {
        Fail("MountGate returned false (dlc.rpf not mounted; entry state %u). This is where retail would "
             "reject the archive's encryption if it were OPEN", (unsigned)EntryState(e));
        return false;
    }
    GLOGI("hook: [packstream] MountGate ok (state %u) -- dlc.rpf is mounted", (unsigned)EntryState(e));

    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupEarlyOn);
    reinterpret_cast<RunPackGroupFn>(g_addr[kRunPackGroup])(mgr, packHash, kGroupStartup);
    GLOGI("hook: [packstream] ran EARLY_ON + STARTUP change-set groups for pack 0x%08x", (unsigned)packHash);

    g_modelHash = world.HashKey(Narrow(reg::Get(L"pack_model", L"jesko")).c_str());
    g_mountAt = GetTickCount64();
    GLOGI("hook: [packstream] waiting up to %llu ms for model '%ls' (hash 0x%08x) to appear in the cdimage",
          (unsigned long long)kModelWaitMs, reg::Get(L"pack_model", L"jesko").c_str(), (unsigned)g_modelHash);
    return true;
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
        Fail("not every ExtraContentManager signature resolved uniquely on this image (see the [packstream] lines)");
        return g_status;
    }

    if (!g_attempted) {
        // Wait for the manager singleton to exist (it is built during boot; it should already be up by the
        // time the player is in the world, but give it a bounded grace period rather than failing at once).
        if (!Manager()) {
            if (!g_mgrSince) g_mgrSince = GetTickCount64();
            if (GetTickCount64() - g_mgrSince > kMgrWaitMs) {
                Fail("the ExtraContentManager singleton stayed null for %llu ms", (unsigned long long)kMgrWaitMs);
                return g_status;
            }
            return Status::Idle;   // try again next tick
        }
        g_attempted = true;
        if (MountSequence(world))
            g_status = Status::Pending;
        return g_status;
    }

    // Pending: wait for the pack's model to become streamable.
    if (g_status == Status::Pending) {
        if (g_modelHash && ModelInCdimage(world, g_modelHash)) {
            g_status = Status::Ready;
            GLOGI("hook: [packstream] READY: model 0x%08x is in the cdimage; the pack mounted and its "
                  "change set registered the model", (unsigned)g_modelHash);
        } else if (GetTickCount64() - g_mountAt > kModelWaitMs) {
            Fail("the model (hash 0x%08x) did not appear in the cdimage within %llu ms after the change set; "
                 "the pack mounted but its vehicle meta did not register (check content.xml / setup2.xml types)",
                 (unsigned)g_modelHash, (unsigned long long)kModelWaitMs);
        }
    }
    return g_status;
}

const char* FailReason() {
    return g_status == Status::Failed ? g_fail : "";
}

}  // namespace gage::game::content
