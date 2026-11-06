// dlc_mount.cpp: run-time mount of the GAGE-MP logo-tee clothing pack. See dlc_mount.h for the flow.
//
// Nothing here is confirmed on the decrypted 3889 image. Every signature below is copied from the FiveM
// source (citizenfx/fivem, master) and is only used when it resolves to exactly one site in the live image;
// Recon() reports that per signature. The calls made through the resolved addresses mirror FiveM's own
// wrappers. What FiveM does not tell us is marked UNKNOWN in the comments.
//
// Self-contained on purpose: hook.cpp keeps countPat / netFindUnique / g_imageBase in anonymous namespaces,
// so this file has its own image bounds (PE header), its own page-safe scanner and the same "CE style"
// pattern syntax ("48 8D ?? 05"). Only hook.h (World, PedComponent) and the logger are shared.

#include "dlc_mount.h"

#include "common/common.h"
#include "common/logger.h"
#include "common/registry.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace gage::game::dlc {

namespace {

// ---------------------------------------------------------------------------------------------
// The pack, as content/dlc/gage_apparel lays it out. These strings must match the files in dlc.rpf.
// ---------------------------------------------------------------------------------------------
const char* const kMountPoint = "dlc_gage_apparel:/";
const char* const kInnerRpf   = "dlc_gage_apparel:/x64/models/cdimages/gage_apparel_male.rpf";
const char* const kShopMeta   = "dlc_gage_apparel:/common/data/mp_m_freemode_01_gage_apparel_shop.meta";
const char* const kRpfType    = "RPF_FILE";
const char* const kShopType   = "SHOP_PED_APPAREL_META_FILE";

const uint32_t kFreemodeMale  = 0x705E61F2;   // mp_m_freemode_01 (same constant as engine.cpp)

// 3889 build hashes of the two count natives, from Mr-X-GTA/YimMenu (GTA_VERSION_TARGET "1.73-3889.0"):
// natives.hpp enum index -> invoker/crossmap.hpp[index], the same lookup hook.cpp's kCrossmap3889 uses
// (index 4134 / 4135 there reproduce IS_PED_COMPONENT_VARIATION_VALID / SET_PED_COMPONENT_VARIATION).
//   GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS  public 0x27561561732A7842  index 4125 -> 0x1A4EFE92822E3123
//   GET_NUMBER_OF_PED_TEXTURE_VARIATIONS   public 0x8F7156A3142A6BAD  index 4127 -> 0x8401C77F508D70FD
// World::CallNative maps public hashes through kCrossmap3889 and falls back to the hash as given, so the
// 3889 value works without a crossmap row. Not live-checked against a 3889 table dump.
const uint64_t kNativeGetNumDrawables = 0x1A4EFE92822E3123ULL;
const uint64_t kNativeGetNumTextures  = 0x8401C77F508D70FDULL;

const ULONGLONG kPendingMaxMs = 15000;

// ---------------------------------------------------------------------------------------------
// Signatures. kind says how the match turns into the address we use.
// ---------------------------------------------------------------------------------------------
enum class Kind {
    Func,     // address = match + offset (a function entry, or a plain site)
    Call,     // E8/E9 rel32 at match + offset: address = the call's target (FiveM hook::get_call)
    Rip32,    // rel32 at match + offset: address = (match + offset + 4) + rel32 (FiveM hook::get_address)
    AbsRva,   // disp32 at match + offset is an RVA from the image base (FiveM CDataFileMount::sm_Interfaces)
    Self,     // address = match itself (a table the pattern's bytes start)
};

enum Id : int {
    kCtor, kVtable, kOpen, kMount, kGetDevice, kMountGlobal,
    kAddPack, kInitManifest, kLoadManifest, kClearManifest, kManifestChunk,
    kInterfaces, kTypeTable,
    kErrInvalid,
    kSigCount
};

struct Sig {
    const char* name;
    const char* pat;
    int         off;
    Kind        kind;
    bool        required;   // needed by the mount path
    const char* source;
};

// Sources, all github.com/citizenfx/fivem blob/master/code/components/:
//   DC = rage-device-five/src/fiDeviceClasses.cpp   DV = rage-device-five/src/fiDevice.cpp
//   LS = gta-streaming-five/src/LoadStreamingFile.cpp   HM = rage-device-five/src/HookInitialMount.cpp
const Sig kSigs[kSigCount] = {
    { "fiPackfile::fiPackfile",     "44 89 41 28 4C 89 41 38 4C 89 41 50 48 8D",                      -0x1E, Kind::Func,  true,  "DC fiPackfile__ctor" },
    { "fiPackfile vtable",          "44 89 41 28 4C 89 41 38 4C 89 41 50 48 8D 05",                    15,   Kind::Rip32, true,  "DC g_vTable_fiPackfile" },
    { "fiPackfile::OpenPackfile",   "48 8D 68 98 48 81 EC 40 01 00 00 41 8B F9",                      -0x18, Kind::Func,  true,  "DC fiPackfile__openArchive" },
    { "fiPackfile::Mount",          "84 C0 74 1D 48 85 DB 74 0F 48",                                  -0x1E, Kind::Func,  true,  "DC fiPackfile__mount" },
    { "fiDevice::GetDevice",        "41 B8 07 00 00 00 48 8B F1 E8",                                  -0x1F, Kind::Func,  true,  "DV fiDevice__GetDevice" },
    { "fiDevice::MountGlobal",      "41 8A F0 48 8B F9 E8 ?? ?? ?? ?? 33 DB 85 C0",                   -0x28, Kind::Func,  false, "DV fiDevice__MountGlobal (recon only)" },
    { "addPackfile",                "EB 15 48 8B 0B 40 38 7B 0C 74 07 E8",                             11,   Kind::Call,  true,  "LS _addPackfile" },
    { "initManifestChunk",          "48 8D 4F 10 B2 01 48 89 2F",                                     -0x2E, Kind::Func,  true,  "LS _initManifestChunk" },
    { "loadManifestChunk",          "45 38 AE C0 00 00 00 0F 95 C3 E8",                               -5,    Kind::Call,  true,  "LS _loadManifestChunk" },
    { "clearManifestChunk",         "33 FF 48 8D 4B 10 B2 01",                                        -0x15, Kind::Func,  true,  "LS _clearManifestChunk" },
    { "manifestChunk global",       "C7 80 ?? 01 00 00 02 00 00 00 E8 ?? ?? ?? ?? 8B 06",             -4,    Kind::Rip32, true,  "LS manifestChunkPtr" },
    { "CDataFileMount::sm_Interfaces", "48 63 82 90 00 00 00 49 8B 8C C0 ?? ?? ?? ?? 48",             11,    Kind::AbsRva, true, "LS CDataFileMount::sm_Interfaces" },
    { "g_dataFileTypes",            "61 44 DF 04 00 00 00 00",                                         0,    Kind::Self,  true,  "LS g_dataFileTypes (first entry: joaat(\"RPF_FILE\"), index 0)" },
    // The call that raises "Invalid rage::fiPackfile encryption type": ecx = 0xBE0E0AEA (err_gen_invalid), call.
    // Recon only: it points at the encryption switch an OPEN archive falls into on the retail exe.
    { "err_gen_invalid site",       "B9 EA 0A 0E BE E8",                                                0,    Kind::Func,  false, "HM PackfileEncryptionError hook site" },
};

// ---------------------------------------------------------------------------------------------
// Image bounds, readable-page runs, pattern scan
// ---------------------------------------------------------------------------------------------
struct Image {
    const uint8_t* base = nullptr;
    size_t         size = 0;
};

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

// The committed, readable stretches of [base, base+size), adjacent ones merged: the Arxan-packed image can
// hold guard or reserved pages, and a scan that walks into one faults the game.
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

// Same syntax as hook.cpp parsePat: hex byte tokens and "??" wildcards separated by spaces.
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
int CountPat(const std::vector<Run>& runs, const char* sig, const uint8_t** first, const uint8_t** second = nullptr) {
    std::vector<uint8_t> bytes, mask;
    if (first) *first = nullptr;
    if (second) *second = nullptr;
    if (!ParsePat(sig, bytes, mask)) return -1;
    const size_t L = bytes.size();
    size_t lead = 0;                       // first fixed byte, used to skip with memchr
    while (lead < L && !mask[lead]) ++lead;
    if (lead == L) return -1;              // all wildcards
    int count = 0;
    for (const Run& r : runs) {
        if (r.n < L) continue;
        const uint8_t* p = r.p + lead;
        const uint8_t* last = r.p + r.n - (L - lead);   // last position of the lead byte that still fits
        while (p <= last) {
            p = static_cast<const uint8_t*>(memchr(p, bytes[lead], (size_t)(last - p) + 1));
            if (!p) break;
            const uint8_t* s = p - lead;
            bool ok = true;
            for (size_t i = 0; i < L; ++i)
                if (mask[i] && s[i] != bytes[i]) { ok = false; break; }
            if (ok) {
                if (first && !*first) *first = s;
                else if (second && !*second) *second = s;
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
        GLOGI("hook: [dlcmount]   %-22s +0x%llx: %s", label,
              (unsigned long long)(uintptr_t)(p + off), hex);
    }
}

// ---------------------------------------------------------------------------------------------
// Resolved addresses
// ---------------------------------------------------------------------------------------------
std::mutex          g_resolveMu;
uintptr_t           g_addr[kSigCount] = {};
bool                g_ok[kSigCount]   = {};
std::atomic<bool>   g_resolvedOnce{ false };
std::atomic<bool>   g_mountPathReady{ false };

// Turn a unique match into the address the mount path uses; false when the bytes do not look right.
bool Compute(const Sig& s, const uint8_t* match, const Image& im, uintptr_t& out) {
    const uint8_t* p = match + s.off;
    auto inImage = [&](uintptr_t a) { return a >= (uintptr_t)im.base && a < (uintptr_t)im.base + im.size; };
    switch (s.kind) {
    case Kind::Func:
    case Kind::Self:
        out = (uintptr_t)p;
        break;
    case Kind::Call: {
        if (!IsReadable(p, 5) || (p[0] != 0xE8 && p[0] != 0xE9)) return false;
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        out = (uintptr_t)(p + 5 + rel);
        break;
    }
    case Kind::Rip32: {
        if (!IsReadable(p, 4)) return false;
        int32_t rel;
        memcpy(&rel, p, 4);
        out = (uintptr_t)(p + 4 + rel);
        break;
    }
    case Kind::AbsRva: {
        if (!IsReadable(p, 4)) return false;
        uint32_t rva;
        memcpy(&rva, p, 4);
        out = (uintptr_t)im.base + rva;
        break;
    }
    }
    return inImage(out);
}

// Logs the bytes at a resolved site, to be disassembled offline (the on-disk exe is Arxan-packed).
void LogSite(int id, const Sig& s, uintptr_t addr, const uint8_t* match) {
    GLOGI("hook: [dlcmount]   %-30s UNIQUE match @ +0x%llx -> target +0x%llx   (%s)", s.name,
          (unsigned long long)(uintptr_t)(match - GameImage().base),
          (unsigned long long)(addr - (uintptr_t)GameImage().base), s.source);
    if (s.kind == Kind::Func || s.kind == Kind::Call || s.kind == Kind::Self) {
        const uint8_t* t = reinterpret_cast<const uint8_t*>(addr);
        if (IsReadable(t, 32) && id != kTypeTable) DumpHex("bytes", t, 32);
    }
}

// g_dataFileTypes: {hash, index} pairs ended by {0, 0xFFFFFFFF} (FiveM LoadStreamingFile.cpp EnumEntry).
bool FindDataFileType(uintptr_t table, uint32_t hash, uint32_t& indexOut) {
    const uint32_t* e = reinterpret_cast<const uint32_t*>(table);
    for (int i = 0; i < 8192; ++i) {
        if (!IsReadable(e + i * 2, 8)) return false;
        const uint32_t h = e[i * 2], idx = e[i * 2 + 1];
        if (h == 0 && idx == 0xFFFFFFFFu) return false;   // terminator: not in the table
        if (h == hash) { indexOut = idx; return true; }
    }
    return false;
}

// Jenkins one-at-a-time over the bytes as given. FiveM hashes the UPPER-CASE type name with HashRageString,
// and joaat("RPF_FILE") computed this way is 0x04DF4461, the first entry of g_dataFileTypes (the bytes of its
// pattern "61 44 DF 04 00 00 00 00"): the hash and the table agree.
uint32_t Joaat(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        h += (uint8_t)*s;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

ReconSummary ResolveAll(bool verbose, int pass) {
    std::lock_guard<std::mutex> lock(g_resolveMu);
    ReconSummary sum;
    const Image im = GameImage();
    if (!im.base || !im.size) {
        GLOGW("hook: [dlcmount] recon pass %d: the game image bounds cannot be read", pass);
        return sum;
    }
    const std::vector<Run> runs = ReadableRuns(im);
    if (verbose)
        GLOGI("hook: [dlcmount] recon pass %d: %d signatures over %u readable run(s) of the image (@0x%llx size=%u)",
              pass, (int)kSigCount, (unsigned)runs.size(), (unsigned long long)(uintptr_t)im.base, (unsigned)im.size);

    bool allRequired = true;
    for (int i = 0; i < kSigCount; ++i) {
        const Sig& s = kSigs[i];
        g_ok[i] = false;
        const uint8_t* first = nullptr;
        const int c = CountPat(runs, s.pat, &first);
        if (c < 0) {
            ++sum.bad;
            if (verbose) GLOGW("hook: [dlcmount]   %-30s BAD PATTERN", s.name);
        } else if (c == 0) {
            ++sum.missing;
            if (verbose) GLOGW("hook: [dlcmount]   %-30s missing (0)   (%s)", s.name, s.source);
        } else if (c > 1) {
            ++sum.ambiguous;
            if (verbose) GLOGW("hook: [dlcmount]   %-30s ambiguous (%d, first @ +0x%llx); not used", s.name, c,
                               (unsigned long long)(uintptr_t)(first - im.base));
        } else {
            uintptr_t addr = 0;
            if (!Compute(s, first, im, addr)) {
                ++sum.bad;
                if (verbose) GLOGW("hook: [dlcmount]   %-30s unique but the bytes at the site are not what the "
                                   "signature's kind expects (call/rel32 outside the image); not used", s.name);
            } else {
                g_addr[i] = addr;
                g_ok[i] = true;
                ++sum.unique;
                if (verbose) LogSite(i, s, addr, first);
            }
        }
        if (s.required && !g_ok[i]) allRequired = false;
    }

    // Structure checks that a bare signature cannot give.
    uint32_t rpfIdx = 0xFFFFFFFFu, shopIdx = 0xFFFFFFFFu;
    if (g_ok[kTypeTable]) {
        const uint32_t* first = reinterpret_cast<const uint32_t*>(g_addr[kTypeTable]);
        const bool firstOk = IsReadable(first, 8) && first[0] == Joaat(kRpfType) && first[1] == 0;
        const bool haveRpf  = firstOk && FindDataFileType(g_addr[kTypeTable], Joaat(kRpfType), rpfIdx);
        const bool haveShop = firstOk && FindDataFileType(g_addr[kTypeTable], Joaat(kShopType), shopIdx);
        if (verbose)
            GLOGI("hook: [dlcmount]   g_dataFileTypes: first entry %s, %s=%s, %s=%s", firstOk ? "is RPF_FILE/0" : "is NOT RPF_FILE/0",
                  kRpfType, haveRpf ? "found" : "NOT found", kShopType,
                  haveShop ? "found" : "NOT found");
        if (haveShop && verbose)
            GLOGI("hook: [dlcmount]   %s data-file type index = %u (hash 0x%08x)", kShopType, (unsigned)shopIdx,
                  (unsigned)Joaat(kShopType));
        if (!(firstOk && haveRpf && haveShop)) {
            g_ok[kTypeTable] = false;
            allRequired = false;
        }
    }
    if (g_ok[kInterfaces] && g_ok[kTypeTable] && !IsReadable(reinterpret_cast<const void*>(g_addr[kInterfaces]), 8)) {
        g_ok[kInterfaces] = false;
        allRequired = false;
        if (verbose) GLOGW("hook: [dlcmount]   sm_Interfaces table address is not readable; not used");
    }

    g_mountPathReady = allRequired;
    g_resolvedOnce = true;
    sum.mountPathReady = allRequired;
    if (verbose)
        GLOGI("hook: [dlcmount] recon pass %d done: %d unique, %d ambiguous, %d missing, %d bad; mount path %s",
              pass, sum.unique, sum.ambiguous, sum.missing, sum.bad, allRequired ? "READY" : "NOT ready");
    return sum;
}

// ---------------------------------------------------------------------------------------------
// Encryption leads: where the retail exe decides an archive's encryption type is invalid.
// Read-only; logs bytes for offline disassembly, patches nothing.
// ---------------------------------------------------------------------------------------------
void LogEncryptionLeads(const Image& im, const std::vector<Run>& runs) {
    if (g_ok[kErrInvalid]) {
        const uint8_t* site = reinterpret_cast<const uint8_t*>(g_addr[kErrInvalid]);
        const size_t before = 192, after = 48;
        if (IsReadable(site - before, before + after)) {
            GLOGI("hook: [dlcmount] encryption lead: err_gen_invalid call @ +0x%llx; the %u bytes before it hold the "
                  "encryption switch (OPEN archives fall into this error on the retail exe):",
                  (unsigned long long)(uintptr_t)(site - im.base), (unsigned)before);
            DumpHex("pre-site", site - before, before + after);
        }
    } else {
        GLOGW("hook: [dlcmount] encryption lead: the err_gen_invalid site did not resolve uniquely");
    }
    struct Imm { const char* what; const char* pat; };
    static const Imm imms[] = {
        { "imm32 0x0FEFFFFF (NG)",   "FF FF EF 0F" },
        { "imm32 0x0FFFFFF9 (AES)",  "F9 FF FF 0F" },
        { "imm32 0x4E45504F (OPEN)", "4F 50 45 4E" },
    };
    for (const Imm& m : imms) {
        const uint8_t* first = nullptr;
        const int c = CountPat(runs, m.pat, &first);
        GLOGI("hook: [dlcmount] encryption lead: %s occurs %d time(s) in the image%s", m.what, c,
              c > 0 ? "" : " (the constant is not in this build's image)");
        if (c > 0 && c <= 12 && first)
            GLOGI("hook: [dlcmount]   first @ +0x%llx", (unsigned long long)(uintptr_t)(first - im.base));
    }
}

DWORD WINAPI ReconThread(void*) {
    static const DWORD kDelays[] = { 15000, 30000, 30000, 45000 };   // 15 s, 45 s, 75 s, 120 s
    const int passes = (int)(sizeof(kDelays) / sizeof(kDelays[0]));
    for (int i = 0; i < passes; ++i) {
        Sleep(kDelays[i]);
        ResolveAll(true, i + 1);
        if (i == passes - 1) {
            const Image im = GameImage();
            if (im.base) LogEncryptionLeads(im, ReadableRuns(im));
        }
    }
    GLOGI("hook: [dlcmount] recon thread finished (%d passes)", passes);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
Options g_opts;
Status  g_status = Status::Idle;
int     g_baseline = -1;          // freemode top count before the pack
int     g_drawable = -1;          // the logo top's index once Ready
ULONGLONG g_pendingSince = 0;
bool    g_readyEdge = false;
char    g_fail[160] = "";
int     g_enabled = -1;           // -1 not read yet

void Fail(const char* fmt, ...) GAGE_PRINTF(1, 2);
void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail, sizeof(g_fail), fmt, ap);
    va_end(ap);
    g_status = Status::Failed;
    GLOGW("hook: [dlcmount] gave up: %s (the base tee stays)", g_fail);
}

bool CallInt(World& w, uint64_t hash, const uint64_t* args, int n, int& out) {
    uint64_t ret[4] = {};
    if (!w.CallNative(hash, args, n, ret)) return false;
    out = (int)(int32_t)ret[0];
    return true;
}

bool CountDrawables(World& w, int32_t ped, int& out) {
    const uint64_t a[] = { (uint64_t)(uint32_t)ped, (uint64_t)kTopComponent };
    return CallInt(w, kNativeGetNumDrawables, a, 2, out);
}

// The pack file's header, read before the game ever sees the file. A header the game would reject
// (OPEN on the retail exe) must never reach OpenPackfile: it ends the game with a fatal error.
bool Preflight(const std::wstring& path, std::string& narrowOut) {
    for (wchar_t c : path)
        if (c > 0x7E) {
            Fail("pack path has a non-ASCII character; the game takes a narrow path");
            return false;
        }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Fail("pack not found or not readable: %s", Narrow(path).c_str());
        return false;
    }
    uint32_t hdr[4] = {};
    DWORD got = 0;
    LARGE_INTEGER sz = {};
    const bool ok = ReadFile(h, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (!ok) { Fail("pack header unreadable"); return false; }
    if (hdr[0] != 0x52504637u) { Fail("pack is not an RPF7 archive (magic 0x%08x)", (unsigned)hdr[0]); return false; }
    if (hdr[1] == 0 || hdr[1] > 100000u) { Fail("pack entry count %u is implausible", (unsigned)hdr[1]); return false; }
    if (sz.QuadPart > (64LL << 20)) { Fail("pack is larger than 64 MiB; not mounting"); return false; }
    const uint32_t enc = hdr[3];
    if (enc == 0x4E45504Fu && !g_opts.openArchivesPatched) {
        Fail("pack is an OPEN (unencrypted) archive and the engine has no verified patch for the retail encryption check");
        return false;
    }
    if (enc != 0x4E45504Fu && enc != 0x0FEFFFFFu && enc != 0x0FFFFFF9u) {
        Fail("pack encryption type 0x%08x is not NG, AES or OPEN", (unsigned)enc);
        return false;
    }
    GLOGI("hook: [dlcmount] pack %s: RPF7, %u entries, encryption 0x%08x (%s), %lld bytes",
          Narrow(path).c_str(), (unsigned)hdr[1], (unsigned)enc,
          enc == 0x4E45504Fu ? "OPEN" : enc == 0x0FEFFFFFu ? "NG" : "AES", (long long)sz.QuadPart);
    narrowOut.clear();
    for (wchar_t c : path) narrowOut.push_back(c == L'\\' ? '/' : (char)c);
    return true;
}

// CDataFileMgr::DataFile (FiveM LoadStreamingFile.cpp): name[128], pad[16], int32 type @0x90 (the signature of
// sm_Interfaces reads [rdx+0x90]), int32 index, then the flag bytes. Over-allocated and zeroed.
struct DataFileBuf {
    alignas(16) uint8_t b[0x100];
    DataFileBuf(const char* name, uint32_t type) {
        memset(b, 0, sizeof(b));
        strncpy(reinterpret_cast<char*>(b), name, 127);
        memcpy(b + 0x90, &type, 4);
    }
    void* get() { return b; }
    void SetDisabled() { b[0x9B] = 1; }   // FiveM CfxPackfileMounter: entry->disabled = true
};

using CtorFn      = void (*)(void*);
using OpenFn      = bool (*)(void*, const char*, bool, int, intptr_t);
using MountFn     = void (*)(void*, const char*);
using GetDevFn    = void* (*)(const char*, bool);
using VoidPtrFn   = void (*)(void*);
using LoadDataFn  = bool (*)(void*, void*);

// Mount + register. Game thread. Sets g_status = Failed on any refusal. true = every call returned.
bool MountPack() {
    if (!g_resolvedOnce) ResolveAll(true, 0);
    if (!g_mountPathReady) {
        Fail("not every required signature resolved uniquely (see the [dlcmount] recon lines)");
        return false;
    }
    std::wstring path = g_opts.packPath.empty() ? gage::ExeDir() + L"dlc\\gage_apparel\\dlc.rpf" : g_opts.packPath;
    std::string narrow;
    if (!Preflight(path, narrow)) return false;

    // Type indices come from the game's own table.
    const uint32_t rpfHash = Joaat(kRpfType), shopHash = Joaat(kShopType);
    uint32_t rpfIdx = 0, shopIdx = 0;
    if (!FindDataFileType(g_addr[kTypeTable], rpfHash, rpfIdx) || !FindDataFileType(g_addr[kTypeTable], shopHash, shopIdx)) {
        Fail("data-file type indices vanished from the type table");
        return false;
    }
    void** ifaces = reinterpret_cast<void**>(g_addr[kInterfaces]);
    void* shopMounter = IsReadable(ifaces + shopIdx, 8) ? ifaces[shopIdx] : nullptr;
    void** shopVtbl = (shopMounter && IsReadable(shopMounter, 8)) ? *reinterpret_cast<void***>(shopMounter) : nullptr;
    if (!shopVtbl || !IsReadable(shopVtbl, 16) || !shopVtbl[1] || !IsReadable(shopVtbl[1], 1)) {
        Fail("no mounter object for %s in sm_Interfaces[%u]", kShopType, (unsigned)shopIdx);
        return false;
    }

    // 1. A fiPackfile on a page of our own (about 0x650 bytes in the game; never freed, never deleted by the
    //    game because it is never unmounted). Vtable first, then the constructor, as FiveM's fiPackfile::fiPackfile.
    void* pack = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pack) { Fail("VirtualAlloc for the packfile object failed"); return false; }
    *reinterpret_cast<uintptr_t*>(pack) = g_addr[kVtable];
    reinterpret_cast<CtorFn>(g_addr[kCtor])(pack);
    GLOGI("hook: [dlcmount] opening %s (object @0x%llx)", narrow.c_str(), (unsigned long long)(uintptr_t)pack);
    // (path, true, 0, 0): the argument values the third-party RPF runbook gives for FiveM's call; the meaning of
    // `type` is UNKNOWN (FiveM's header names them bTrue / type / veryFalse).
    if (!reinterpret_cast<OpenFn>(g_addr[kOpen])(pack, narrow.c_str(), true, 0, 0)) {
        Fail("OpenPackfile returned false");
        return false;
    }
    reinterpret_cast<MountFn>(g_addr[kMount])(pack, kMountPoint);
    if (!reinterpret_cast<GetDevFn>(g_addr[kGetDevice])(kShopMeta, true)) {
        Fail("after Mount(\"%s\") GetDevice finds no device for the shop meta path", kMountPoint);
        return false;
    }
    GLOGI("hook: [dlcmount] mounted %s; the shop meta path resolves to a device", kMountPoint);

    // 2. The inner ped archive, like FiveM's CfxPackfileMounter::LoadDataFile (manifest chunk around addPackfile).
    DataFileBuf rpf(kInnerRpf, rpfIdx);
    rpf.SetDisabled();
    void* chunk = reinterpret_cast<void*>(g_addr[kManifestChunk]);
    reinterpret_cast<VoidPtrFn>(g_addr[kInitManifest])(chunk);
    reinterpret_cast<VoidPtrFn>(g_addr[kAddPack])(rpf.get());
    reinterpret_cast<VoidPtrFn>(g_addr[kLoadManifest])(chunk);
    reinterpret_cast<VoidPtrFn>(g_addr[kClearManifest])(chunk);
    GLOGI("hook: [dlcmount] registered %s (type %u)", kInnerRpf, (unsigned)rpfIdx);

    // 3. The shop meta, through the game's own mounter for its type (FiveM: sm_Interfaces[type]->LoadDataFile;
    //    slot 1 after the destructor, the order of FiveM's CDataFileMountInterface).
    DataFileBuf meta(kShopMeta, shopIdx);
    const bool loaded = reinterpret_cast<LoadDataFn>(shopVtbl[1])(shopMounter, meta.get());
    GLOGI("hook: [dlcmount] registered %s (type %u): LoadDataFile returned %s", kShopMeta, (unsigned)shopIdx,
          loaded ? "true" : "false");
    if (!loaded) {
        Fail("the shop-meta mounter refused the pack's meta");
        return false;
    }
    return true;
}

bool Enabled() {
    if (g_enabled < 0) {
        g_enabled = (reg::Get(L"dlc_mount") == L"1") ? 1 : 0;
        GLOGI("hook: [dlcmount] logo-tee mount %s (HKCU\\Software\\GAGE-MP dlc_mount=%s)",
              g_enabled ? "ON" : "off", g_enabled ? "1" : "unset or not 1");
    }
    return g_enabled == 1;
}

}  // namespace

// =============================================================================================
// Public API
// =============================================================================================
ReconSummary Recon(int pass) {
    ReconSummary s = ResolveAll(true, pass);
    const Image im = GameImage();
    if (im.base) LogEncryptionLeads(im, ReadableRuns(im));
    return s;
}

void StartReconThread() {
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return;
    HANDLE t = CreateThread(nullptr, 0, ReconThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else GLOGW("hook: [dlcmount] could not start the recon thread");
}

void Configure(const Options& options) {
    g_opts = options;
}

Status Tick(World& world, int32_t ped, PedComponent* outTop) {
    if (g_status == Status::Ready) {
        if (outTop) *outTop = { kTopComponent, g_drawable, 0, "top: Solar RP logo tee" };
        return g_status;
    }
    if (g_status == Status::Failed || !ped || !Enabled())
        return g_status;
    // Counts are per model: only a ped that is verifiably the freemode male counts.
    if (world.GetEntityModel(ped) != kFreemodeMale)
        return g_status;

    if (g_status == Status::Idle) {
        int n0 = 0;
        if (!CountDrawables(world, ped, n0) || n0 <= 0) {
            Fail("GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS cannot be called or returned %d", n0);
            return g_status;
        }
        g_baseline = n0;
        GLOGI("hook: [dlcmount] baseline: the freemode male has %d tops before the pack", n0);
        if (!MountPack())
            return g_status;   // Fail() already logged the reason
        g_status = Status::Pending;
        g_pendingSince = GetTickCount64();
        return g_status;
    }

    // Pending: wait for the game to publish exactly the pack's drawables.
    int n = 0;
    if (!CountDrawables(world, ped, n)) {
        Fail("GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS stopped working while waiting");
        return g_status;
    }
    if (n == g_baseline) {
        if (GetTickCount64() - g_pendingSince > kPendingMaxMs)
            Fail("the top count stayed at %d for %llu ms after the mount (the pack was not picked up)", n,
                 (unsigned long long)kPendingMaxMs);
        return g_status;
    }
    if (n != g_baseline + kPackTopDrawables) {
        Fail("the top count went from %d to %d, expected +%d", g_baseline, n, kPackTopDrawables);
        return g_status;
    }
    // The count grew by exactly the pack's size, so the new drawable is the first index past the old range.
    const int drawable = g_baseline;
    int texCount = 0;
    const uint64_t ta[] = { (uint64_t)(uint32_t)ped, (uint64_t)kTopComponent, (uint64_t)(uint32_t)drawable };
    if (!CallInt(world, kNativeGetNumTextures, ta, 3, texCount) || texCount != kPackTopTextures) {
        Fail("drawable %d has %d texture(s), expected %d", drawable, texCount, kPackTopTextures);
        return g_status;
    }
    const uint64_t va[] = { (uint64_t)(uint32_t)ped, (uint64_t)kTopComponent, (uint64_t)(uint32_t)drawable, 0 };
    int valid = 0;
    if (!CallInt(world, native::IS_PED_COMPONENT_VARIATION_VALID, va, 4, valid) || !valid) {
        Fail("IS_PED_COMPONENT_VARIATION_VALID(11, %d, 0) is false", drawable);
        return g_status;
    }
    g_drawable = drawable;
    g_status = Status::Ready;
    g_readyEdge = true;
    GLOGI("hook: [dlcmount] logo tee ready: component %d drawable %d texture 0 (tops %d -> %d)", kTopComponent,
          drawable, g_baseline, n);
    if (outTop) *outTop = { kTopComponent, g_drawable, 0, "top: Solar RP logo tee" };
    return g_status;
}

bool ConsumeReadyEdge() {
    const bool e = g_readyEdge;
    g_readyEdge = false;
    return e;
}

const char* FailReason() {
    return g_status == Status::Failed ? g_fail : "";
}

}  // namespace gage::game::dlc
