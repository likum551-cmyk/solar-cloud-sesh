// pack_enh_mount_core.h: Round B of the car-pack port to GTA V Enhanced (pack_enh=2): the MOUNT, as pure logic.
//
// Round A (pack_enh_core.h) proved the layout read-only. Round B makes the same calls Legacy's pack_stream.cpp makes, with the
// Enhanced profile (pack_profile.h kPackEnhanced), in the order the game's own pack loop at 0xc28b18 uses:
//
//   1 AddDlcPack(mgr, "<folder>/")                    appends a fresh type-2 DlcEntry (the array grows when it is full)
//   2 folder override (loose packs only)              the entry's path atString is rewritten THROUGH THE GAME'S OWN append
//                                                     (0xaa02e0), then type := 1 (relative device over the folder)
//   3 DlcSetup2(entry, gen)                           parses setup2.xml: name hash, mount point, state 1
//   4 Mount(entry, gen, mountPoint), then state := 2  the loop's inlined "MountGate" (Legacy has it as a function)
//   5 (the caller) register the pack's loose stream files, read its car list, check the vehicle store
//   6 RunPackGroup(mgr, entry, &early_on), then &startup
//
// Everything the mount touches goes through MountAbi, so the live engine wires real game calls and tests/pack_profile_test.cpp
// wires a fake game that keeps the same structures (it grows the array and frees the old one, as the game does). Every step
// logs ONE line BEFORE it runs, so the last `[packenh]` line before a crash names the step. A failed step logs
// `[packenh] pack X FAILED at step N/6 (name): why` and stops that pack; a failed read stops BEFORE the first write.
//
// 0.4.14 lesson: never store our own pointer in a game-owned struct. The only game structure written here are plain
// WORD / DWORD fields (path length, type, state) and the path atString's CONTENT through the game's append.
// Pointers into the manager's array are never kept across a call that can grow it: the entry is re-derived from the
// array pointer after AddDlcPack and again before RunPackGroup.
//
// Pure C++17, no OS calls.
#pragma once

#include "pack_enh_core.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gage::game::content::packenh {

// ---- the patterns Round A does not resolve ---------------------------------------------------------------------------------
enum ExtraId : int {
    kXRunPackGroup, kXRegStream, kXGetDevice, kXPushFolder, kXExists, kXPopFolder,
    kXStreamInfo,   // the streaming-info manager global, read out of RegStreamFile's own call (not a separate scan of the image)
    kExtraCount
};

inline const char* ExtraName(int id) {
    static const char* const k[kExtraCount] = { "RunPackGroup", "RegStreamFile", "GetDevice", "PushFolder", "Exists", "PopFolder", "StreamInfoMgr" };
    return (id >= 0 && id < kExtraCount) ? k[id] : "?";
}

// The mount needs these four; PushFolder / Exists / PopFolder are only the diagnostic probe's calls (a miss is logged, never fatal).
inline bool ExtraCritical(int id) { return id == kXRunPackGroup || id == kXRegStream || id == kXGetDevice || id == kXStreamInfo; }

class Extras {
public:
    Extras(const PackProfile& prof, const Env& env) : P_(prof), E_(env) {
        defs_[kXRunPackGroup] = &P_.runPackGroup; defs_[kXRegStream] = &P_.regStream; defs_[kXGetDevice] = &P_.getDevice;
        defs_[kXPushFolder] = &P_.pushFolder;     defs_[kXExists] = &P_.exists;       defs_[kXPopFolder] = &P_.popFolder;
        defs_[kXStreamInfo] = &P_.regStreamInfo;
        for (int i = 0; i < kExtraCount; ++i) count_[i] = -2;
    }

    bool Done() const { for (int i = 0; i < kExtraCount; ++i) if (st_[i] == 0) return false; return true; }
    bool Ok() const { for (int i = 0; i < kExtraCount; ++i) if (ExtraCritical(i) && st_[i] != 1) return false; return true; }
    bool IdOk(int id) const { return id >= 0 && id < kExtraCount && st_[id] == 1; }
    // The RVA a pattern resolved to (kXStreamInfo: the global's RVA), or ~0.
    uint64_t Rva(int id) const { return IdOk(id) ? rva_[id] : ~0ull; }
    int Hits(int id) const { return (id >= 0 && id < kExtraCount) ? count_[id] : -2; }
    bool NeedsScan(uint64_t now) const { return !Done() && now >= nextScanAt_; }

    // One poll. runs = the image's readable runs when NeedsScan() was true. giveUp = the pattern-wait budget ran out.
    // Returns true once every pattern is decided.
    bool Step(uint64_t now, const std::vector<scan::Run>* runs, bool giveUp) {
        if (Done()) return true;
        if (!started_) {
            started_ = true;
            Log(false, "[packenh] round B: resolving the mount patterns (%d) in the image", (int)kExtraCount);
        }
        if (runs) Scan(*runs);
        nextScanAt_ = now + 1500;
        if (giveUp)
            for (int i = 0; i < kExtraCount; ++i)
                if (st_[i] == 0) Set(i, false, "still missing after the wait budget (the code never decrypted, or the pattern is wrong for this build)");
        return Done();
    }

private:
    void Log(bool warn, const char* fmt, ...) const {
        char b[560];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        if (E_.log) E_.log(E_.ctx, warn, b);
    }
    void Set(int id, bool ok, const char* fmt, ...) {
        if (st_[id] != 0) return;
        st_[id] = ok ? 1 : 2;
        char b[460];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        Log(!ok, "[packenh] pattern %-14s %s  %s%s", ExtraName(id), ok ? "ok  " : "FAIL", b,
            (!ok && !ExtraCritical(id)) ? "  (diagnostic only: the mount does not need it)" : "");
    }

    void Scan(const std::vector<scan::Run>& runs) {
        for (int i = 0; i < kXStreamInfo; ++i) {
            if (st_[i] != 0) continue;
            const PackSig& d = *defs_[i];
            if (!d.pat) { Set(i, false, "this profile has no pattern for it"); continue; }
            const uint8_t* first = nullptr;
            const int c = scan::CountPat(runs, d.pat, &first);
            count_[i] = c;
            at_[i] = first;
            if (c < 0) Set(i, false, "BAD PATTERN");
            else if (c > 1) Set(i, false, "ambiguous (%d hits, first @ +0x%llx); not used", c, (unsigned long long)(first - E_.scanBase));
            else if (c == 1) {
                rva_[i] = (uint64_t)(first - E_.scanBase);
                Set(i, true, "UNIQUE @ +0x%llx   (%s)", (unsigned long long)rva_[i], d.source ? d.source : "");
                if (d.expectRva && d.expectRva != rva_[i])
                    Log(true, "[packenh]   NOTE %s resolved at +0x%llx, the offline dump read +0x%llx (the build differs from the dump)", ExtraName(i),
                        (unsigned long long)rva_[i], (unsigned long long)d.expectRva);
            }
        }
        // The streaming-info manager: the register call inside RegStreamFile (searched in its body only).
        if (st_[kXStreamInfo] == 0) {
            if (st_[kXRegStream] == 2) { Set(kXStreamInfo, false, "skipped: RegStreamFile did not resolve"); }
            else if (st_[kXRegStream] == 1) {
                const PackSig& d = P_.regStreamInfo;
                const uint8_t* fn = at_[kXRegStream];
                const uint8_t* end = E_.scanBase + E_.size;
                size_t len = P_.regStreamInfoScanLen;
                if ((size_t)(end - fn) < len) len = (size_t)(end - fn);
                std::vector<scan::Run> body = { { fn, len } };
                const uint8_t* hit = nullptr;
                const int n = d.pat ? scan::CountPat(body, d.pat, &hit) : -1;
                count_[kXStreamInfo] = n;
                int32_t disp = 0;
                int64_t r = -1;
                if (n == 1 && hit) {
                    memcpy(&disp, hit + P_.regStreamInfoOp.dispAt, 4);
                    r = (int64_t)(hit - E_.scanBase) + P_.regStreamInfoOp.endAt + disp;
                }
                if (n == 1 && r >= 0 && (uint64_t)r < E_.size) {
                    rva_[kXStreamInfo] = (uint64_t)r;
                    Set(kXStreamInfo, true, "strStreamingInfoManager +0x%llx from the register call at RegStreamFile+0x%llx (%s)", (unsigned long long)r,
                        (unsigned long long)(hit - fn), d.source ? d.source : "");
                    if (P_.streamInfoMgrRva && P_.streamInfoMgrRva != (uint32_t)r)
                        Log(true, "[packenh]   NOTE the streaming-info manager is +0x%llx, the offline dump read +0x%x", (unsigned long long)r, (unsigned)P_.streamInfoMgrRva);
                } else {
                    Set(kXStreamInfo, false, "%d register call(s) inside RegStreamFile, operand %s", n, (r >= 0 && (uint64_t)r < E_.size) ? "in the image" : "outside the image");
                }
            }
        }
        for (int i = 0; i < kExtraCount; ++i)
            if (st_[i] == 0 && !missLogged_[i] && i != kXStreamInfo) {
                missLogged_[i] = true;
                Log(false, "[packenh] pattern %-14s missing (0): the code may still be encrypted; retrying", ExtraName(i));
            }
    }

    const PackProfile& P_;
    Env E_;
    const PackSig* defs_[kExtraCount] = {};
    int st_[kExtraCount] = {};
    int count_[kExtraCount] = {};
    const uint8_t* at_[kExtraCount] = {};
    uint64_t rva_[kExtraCount] = {};
    bool missLogged_[kExtraCount] = {};
    bool started_ = false;
    uint64_t nextScanAt_ = 0;
};

// ---- the mount ---------------------------------------------------------------------------------------------------------------
struct MountAbi {
    // Fault-proof access to the game's memory (absolute virtual addresses).
    bool (*read)(void* ctx, uint64_t va, void* out, size_t n) = nullptr;
    bool (*write)(void* ctx, uint64_t va, const void* in, size_t n) = nullptr;
    // The game's functions. Pointers are virtual addresses in the game.
    uint8_t (*addDlcPack)(void* ctx, uint64_t mgr, const char* path) = nullptr;
    uint8_t (*dlcSetup2)(void* ctx, uint64_t entry, uint32_t gen) = nullptr;
    int     (*mount)(void* ctx, uint64_t entry, uint32_t gen, uint64_t mountPointVa /* 0 = the empty string */) = nullptr;
    void    (*atAppend)(void* ctx, uint64_t atString, const char* src, int len) = nullptr;
    void    (*runGroup)(void* ctx, uint64_t mgr, uint64_t entry, uint32_t groupHash) = nullptr;
    void    (*log)(void* ctx, bool warn, const char* line) = nullptr;
    void*   ctx = nullptr;
};

struct MountOut {
    bool ok = false;
    int step = 0;                  // the step that failed (1..6; 0 = the manager check), 0 when ok
    std::string stepName, why;
    uint64_t entry = 0;            // the entry's address NOW (valid until the next AddDlcPack)
    uint32_t index = 0, hash = 0, gen = 0, state = 0;
    uint32_t countBefore = 0, countAfter = 0;
    bool arrayMoved = false;       // the manager's array was reallocated by this AddDlcPack
};

class Mounter {
public:
    static constexpr uint32_t kGroupEarlyOn = 0x6f8bd408u;   // joaat("early_on"), what the game's loop runs first
    static constexpr uint32_t kGroupStartup = 0x5bf6578fu;   // joaat("startup")
    static constexpr int kSteps = 6;

    Mounter(const PackProfile& prof, const MountAbi& abi, uint64_t mgrVa, uint64_t mgrVtableVa) : P_(prof), A_(abi), mgr_(mgrVa), vtable_(mgrVtableVa) {}

    // The manager's array, count, capacity and generation. False when any of them does not read.
    bool ReadMgr(uint64_t& arr, uint32_t& count, uint32_t& cap, uint32_t& gen) const {
        uint16_t c = 0, k = 0;
        uint32_t g = 0;
        if (!Rd(mgr_ + P_.mgrArray, arr) || !Rd(mgr_ + P_.mgrCount, c) || !Rd(mgr_ + P_.mgrGen, g)) return false;
        if (P_.mgrCap) Rd(mgr_ + P_.mgrCap, k);
        count = c; cap = k; gen = g;
        return true;
    }

    // The pack's mount point ("dlc_supra:/"): the entry's atString, "" when unreadable or implausible.
    std::string MountPointOf(uint64_t entry) const {
        uint64_t ptr = 0;
        uint16_t len = 0;
        if (!Rd(entry + P_.entryMountPoint, ptr) || !Rd(entry + P_.entryMountPointLen, len)) return "";
        std::string s;
        if (len == 0 || len > 120 || !CStr(ptr, len, s)) return "";
        if (s.find(':') == std::string::npos) return "";
        if (s.back() != '/') s.push_back('/');
        return s;
    }

    // Steps 1-4. label = the pack's folder name (log lines); device = AddDlcPack's "<folder>/" (forward slashes, trailing '/').
    bool Add(const std::string& label, const std::string& device, bool folder, MountOut& out) {
        out = MountOut();
        const char* L = label.c_str();
        // 0. the manager is what the dump says it is
        uint64_t vt = 0;
        if (!Rd(mgr_, vt) || vt != vtable_)
            return Fail(out, L, 0, "manager check", "the manager's vtable reads 0x%llx, expected 0x%llx", (unsigned long long)vt, (unsigned long long)vtable_);
        uint64_t arr0 = 0;
        uint32_t n0 = 0, cap0 = 0, gen = 0;
        if (!ReadMgr(arr0, n0, cap0, gen)) return Fail(out, L, 0, "manager check", "the manager's array / count / generation do not read back");
        if (n0 >= 60000 || (cap0 && n0 > cap0)) return Fail(out, L, 0, "manager check", "implausible pack count %u (capacity %u)", (unsigned)n0, (unsigned)cap0);
        out.countBefore = n0; out.gen = gen;

        // 1. AddDlcPack
        Log(false, "[packenh] pack %s: step 1/%d AddDlcPack \"%s\" (packs %u, array 0x%llx, capacity %u, generation %u)", L, kSteps, device.c_str(), (unsigned)n0,
            (unsigned long long)arr0, (unsigned)cap0, (unsigned)gen);
        const uint8_t added = A_.addDlcPack(A_.ctx, mgr_, device.c_str());
        uint64_t arr1 = 0;
        uint32_t n1 = 0, cap1 = 0, gen1 = 0;
        if (!ReadMgr(arr1, n1, cap1, gen1)) return Fail(out, L, 1, "AddDlcPack", "the manager no longer reads back after the call");
        out.countAfter = n1;
        out.arrayMoved = arr1 != arr0;
        Log(false, "[packenh] pack %s: AddDlcPack returned %u; pack count %u -> %u; array 0x%llx -> 0x%llx%s; capacity %u -> %u", L, (unsigned)added, (unsigned)n0, (unsigned)n1,
            (unsigned long long)arr0, (unsigned long long)arr1, out.arrayMoved ? " (the game grew it; the entry is re-derived from the new array)" : "", (unsigned)cap0,
            (unsigned)cap1);
        if (n1 != n0 + 1)
            return Fail(out, L, 1, "AddDlcPack", "returned %u but the pack count went %u -> %u (expected %u): the game's device layer did not resolve \"%sdlc.rpf\"",
                        (unsigned)added, (unsigned)n0, (unsigned)n1, (unsigned)(n0 + 1), device.c_str());
        if (!added) Log(true, "[packenh] pack %s: AddDlcPack returned 0 although it appended an entry; going on with the entry it made", L);
        out.index = n1 - 1;
        out.entry = arr1 + (uint64_t)out.index * P_.entryStride;
        uint32_t hash = 0, type = 0, state = 0;
        if (!Rd(out.entry + P_.entryHash, hash) || !Rd(out.entry + P_.entryType, type) || !Rd(out.entry + P_.entryState, state))
            return Fail(out, L, 1, "AddDlcPack", "the new entry #%u at 0x%llx does not read back", (unsigned)out.index, (unsigned long long)out.entry);
        Log(false, "[packenh] pack %s: new entry #%u @0x%llx: type %u state %u nameHash 0x%08x", L, (unsigned)out.index, (unsigned long long)out.entry, (unsigned)type,
            (unsigned)state, (unsigned)hash);
        if (type != P_.typePackfile || state > 1)
            return Fail(out, L, 1, "AddDlcPack", "the new entry is not a fresh packfile entry (type %u state %u; expected type %u, state 0): the layout is not what the dump says; nothing was written",
                        (unsigned)type, (unsigned)state, (unsigned)P_.typePackfile);

        // 2. the folder override
        if (folder) {
            if (!A_.atAppend) return Fail(out, L, 2, "folder override", "the game's atString append was not resolved; nothing was written");
            uint64_t oldPtr = 0;
            uint16_t oldLen = 0, oldCap = 0;
            if (!Rd(out.entry + P_.entryPath, oldPtr) || !Rd(out.entry + P_.entryPathLen, oldLen) || !Rd(out.entry + P_.entryPathLen + 2, oldCap))
                return Fail(out, L, 2, "folder override", "the entry's path atString does not read back; nothing was written");
            Log(false, "[packenh] pack %s: step 2/%d folder override: path ptr 0x%llx length %u capacity %u, type %u; WRITING length 0, then the game's append of \"%s\", then type %u", L,
                kSteps, (unsigned long long)oldPtr, (unsigned)oldLen, (unsigned)oldCap, (unsigned)type, device.c_str(), (unsigned)P_.typeRelative);
            const uint16_t zero = 0;
            if (!Wr(out.entry + P_.entryPathLen, zero)) return Fail(out, L, 2, "folder override", "writing the path length failed");
            A_.atAppend(A_.ctx, out.entry + P_.entryPath, device.c_str(), (int)device.size());
            uint64_t newPtr = 0;
            uint16_t newLen = 0;
            std::string got;
            if (!Rd(out.entry + P_.entryPath, newPtr) || !Rd(out.entry + P_.entryPathLen, newLen) || newLen != device.size() || !CStr(newPtr, newLen, got) || got != device)
                return Fail(out, L, 2, "folder override", "the entry path did not read back as the folder after the game's append (length %u, \"%s\")", (unsigned)newLen, got.c_str());
            const uint32_t rel = P_.typeRelative;
            uint32_t back = 0;
            if (!Wr(out.entry + P_.entryType, rel) || !Rd(out.entry + P_.entryType, back) || back != rel)
                return Fail(out, L, 2, "folder override", "writing the device type failed or did not read back (%u)", (unsigned)back);
            Log(false, "[packenh] pack %s: folder override done: relative device (type %u) over \"%s\" (game-owned string, ptr 0x%llx, length %u)", L, (unsigned)back, got.c_str(),
                (unsigned long long)newPtr, (unsigned)newLen);
        } else {
            Log(false, "[packenh] pack %s: step 2/%d folder override: skipped (the pack is a dlc.rpf; the game opens it itself)", L, kSteps);
        }

        // 3. DlcSetup2
        Log(false, "[packenh] pack %s: step 3/%d DlcSetup2(entry 0x%llx, generation %u)", L, kSteps, (unsigned long long)out.entry, (unsigned)gen);
        const uint8_t ok2 = A_.dlcSetup2(A_.ctx, out.entry, gen);
        if (!Rd(out.entry + P_.entryState, state) || !Rd(out.entry + P_.entryHash, hash))
            return Fail(out, L, 3, "DlcSetup2", "the entry no longer reads back after the call");
        Log(false, "[packenh] pack %s: DlcSetup2 returned %u; state %u, nameHash 0x%08x", L, (unsigned)ok2, (unsigned)state, (unsigned)hash);
        if (!ok2) return Fail(out, L, 3, "DlcSetup2", "returned false (setup2.xml not parsed; entry state %u). The pack needs a valid setup2.xml at its root", (unsigned)state);
        if (hash == 0) return Fail(out, L, 3, "DlcSetup2", "the entry's nameHash is still zero after DlcSetup2; the change set cannot be run");
        out.hash = hash;

        // 4. the mount (the game's loop: state 1 -> Mount -> state 2)
        out.state = state;
        if (state == 2) {
            Log(false, "[packenh] pack %s: step 4/%d Mount: skipped, the entry is already mounted (state 2)", L, kSteps);
        } else if (state == 1) {
            uint64_t mpPtr = 0;
            uint16_t mpLen = 0;
            Rd(out.entry + P_.entryMountPoint, mpPtr);
            Rd(out.entry + P_.entryMountPointLen, mpLen);
            const std::string mp = MountPointOf(out.entry);
            const uint64_t mpVa = mpLen ? mpPtr : 0;
            Log(false, "[packenh] pack %s: step 4/%d Mount(entry, generation %u, mount point \"%s\"%s)", L, kSteps, (unsigned)gen, mp.c_str(), mpLen ? "" : " = the empty string");
            const int rc = A_.mount(A_.ctx, out.entry, gen, mpVa);
            Rd(out.entry + P_.entryState, state);
            Log(false, "[packenh] pack %s: Mount returned %d (0 = ok); state %u", L, rc, (unsigned)state);
            if (rc != 0) return Fail(out, L, 4, "Mount", "returned %d (state %u): the archive or the folder did not mount", rc, (unsigned)state);
            Log(false, "[packenh] pack %s: WRITING state 2 (mounted), as the game's loop does after a successful Mount", L);
            const uint32_t two = 2;
            uint32_t back = 0;
            if (!Wr(out.entry + P_.entryState, two) || !Rd(out.entry + P_.entryState, back) || back != 2)
                return Fail(out, L, 4, "Mount", "writing state 2 failed or did not read back (%u)", (unsigned)back);
            out.state = 2;
        } else {
            return Fail(out, L, 4, "Mount", "unexpected entry state %u after DlcSetup2 (expected 1)", (unsigned)state);
        }
        out.ok = true;
        Log(false, "[packenh] pack %s: mounted (entry #%u, nameHash 0x%08x, state %u, mount point \"%s\")", L, (unsigned)out.index, (unsigned)out.hash, (unsigned)out.state,
            MountPointOf(out.entry).c_str());
        return true;
    }

    // Step 6. The entry is re-derived first (the array may have moved since Add): by index, then by name hash.
    bool RunChangeSets(const std::string& label, MountOut& out) {
        const char* L = label.c_str();
        out.ok = false;
        uint64_t arr = 0;
        uint32_t n = 0, cap = 0, gen = 0;
        if (!ReadMgr(arr, n, cap, gen)) return Fail(out, L, 6, "change sets", "the manager does not read back");
        uint64_t e = 0;
        uint32_t found = ~0u;
        for (uint32_t k = 0; k < n && k < 60000; ++k) {
            const uint32_t i = (out.index + k) % n;
            uint32_t h = 0;
            if (Rd(arr + (uint64_t)i * P_.entryStride + P_.entryHash, h) && h == out.hash) { found = i; e = arr + (uint64_t)i * P_.entryStride; break; }
        }
        if (found == ~0u) return Fail(out, L, 6, "change sets", "no entry with nameHash 0x%08x in the manager's %u pack(s)", (unsigned)out.hash, (unsigned)n);
        if (e != out.entry)
            Log(false, "[packenh] pack %s: the entry moved (0x%llx -> 0x%llx, #%u -> #%u); using the current one", L, (unsigned long long)out.entry, (unsigned long long)e,
                (unsigned)out.index, (unsigned)found);
        out.entry = e;
        out.index = found;
        const uint32_t groups[2] = { kGroupEarlyOn, kGroupStartup };
        const char* const gname[2] = { "early_on", "startup" };
        for (int g = 0; g < 2; ++g) {
            Log(false, "[packenh] pack %s: step 6/%d RunPackGroup(manager, entry 0x%llx, group %s 0x%08x)", L, kSteps, (unsigned long long)e, gname[g], (unsigned)groups[g]);
            A_.runGroup(A_.ctx, mgr_, e, groups[g]);
            Log(false, "[packenh] pack %s: RunPackGroup %s returned", L, gname[g]);
        }
        Log(false, "[packenh] pack %s: change-set groups ran (nameHash 0x%08x)", L, (unsigned)out.hash);
        out.ok = true;
        return true;
    }

private:
    template <class T> bool Rd(uint64_t va, T& v) const { return A_.read && A_.read(A_.ctx, va, &v, sizeof(T)); }
    template <class T> bool Wr(uint64_t va, const T& v) const { return A_.write && A_.write(A_.ctx, va, &v, sizeof(T)); }

    // len bytes of printable ASCII behind a char*, NUL-terminated.
    bool CStr(uint64_t ptr, uint32_t len, std::string& out) const {
        out.clear();
        if (ptr < 0x10000 || len == 0 || len > 4096) return false;
        std::vector<char> b(len + 1);
        if (!A_.read || !A_.read(A_.ctx, ptr, b.data(), len + 1) || b[len] != 0) return false;
        for (uint32_t i = 0; i < len; ++i)
            if ((unsigned char)b[i] < 0x20 || (unsigned char)b[i] > 0x7e) return false;
        out.assign(b.data(), len);
        return true;
    }

    void Log(bool warn, const char* fmt, ...) const {
        char b[900];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        if (A_.log) A_.log(A_.ctx, warn, b);
    }

    bool Fail(MountOut& out, const char* label, int step, const char* name, const char* fmt, ...) const {
        char b[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        out.ok = false;
        out.step = step;
        out.stepName = name;
        out.why = b;
        Log(true, "[packenh] pack %s FAILED at step %d/%d (%s): %s", label, step, kSteps, name, b);
        return false;
    }

    const PackProfile& P_;
    MountAbi A_;
    uint64_t mgr_ = 0, vtable_ = 0;
};

}  // namespace gage::game::content::packenh
