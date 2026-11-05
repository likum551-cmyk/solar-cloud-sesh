// pack_enh_core.h: Round A of the car-pack port to GTA V Enhanced (pack_enh=1): a READ-ONLY diagnostic.
//
// It mounts nothing, writes no game memory and calls no game function. It (1) resolves the Enhanced pack patterns
// (pack_profile.h kPackEnhanced) in the decrypted image and cross-checks their operands against each other, then
// (2) waits for the game to build its ExtraContentManager, image registry and DLC vehicle store and reads them with
// fault-proof reads, checking the layout the offline reads of the friend's dump predicted. Every check logs one
// `[packenh] check ...` line; the run ends with `[packenh] round A: N of M checks ok`.
//
// Pure C++17 with no OS calls: the live engine wires the Env to the game's memory (pack_enh.cpp), tests/
// pack_profile_test.cpp wires it to the offline image dump plus a synthetic heap and runs the same code.
#pragma once

#include "pack_profile.h"
#include "pack_scan.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gage::game::content::packenh {

struct Env {
    const uint8_t* scanBase = nullptr;   // the image bytes as the pattern scanner sees them (live: the module base)
    uint64_t       vaBase = 0;           // the image's virtual address in the game (live: == scanBase)
    size_t         size = 0;             // SizeOfImage
    // Fault-proof copy of n bytes of game memory at an absolute virtual address.
    bool (*read)(void* ctx, uint64_t va, void* out, size_t n) = nullptr;
    // One log line (already formatted, no newline).
    void (*log)(void* ctx, bool warn, const char* line) = nullptr;
    void* ctx = nullptr;
};

enum CheckId : int {
    // patterns: each must be a UNIQUE hit
    kAddDlcPack, kDlcSetup2, kMount, kMgrAnchor, kMgrCtor, kAtStr, kImageReg, kVehStore,
    // cross-checks between the patterns' operands (still offline-resolvable)
    kMgrGlobal,       // the anchor's and the ctor's manager global agree
    kAtStrCall,       // AddDlcPack's own call to atString::Append lands on the pattern's function
    kImageRegOps,     // the registry lookup's operands are sane and agree with each other
    kVehStoreOps,     // the vehicle store init's operands are sane
    // the live game state
    kMgrPtr,          // the manager singleton holds a heap pointer
    kMgrVtable,       // its vtable is the ctor's, inside the image
    kPackCount,       // the pack count is plausible
    kEntries,         // every DlcEntry reads back with a plausible hash / type / state at the profile's stride
    kFirstPath,       // the first entry's path string reads back
    kRegEntry0,       // image registry[0] holds a pointer
    kRegVtable,       // ... whose vtable is inside the image
    kRegSlot,         // ... and whose vtable slot (+0x1e0) points into the image
    kRegCap,          // the registry's bound is plausible
    kStore,           // the DLC vehicle store exists and n <= cap
    kCheckCount
};

inline const char* CheckName(int id) {
    static const char* const k[kCheckCount] = {
        "AddDlcPack", "DlcSetup2", "Mount", "ManagerAnchor", "ManagerCtor", "AtStringAppend", "ImageRegistry", "VehStoreInit",
        "ManagerGlobal", "AtStringCall", "ImageRegOperands", "VehStoreOperands",
        "ManagerPtr", "ManagerVtable", "PackCount", "Entries", "FirstPath",
        "Registry0", "Registry0Vtable", "RegistrySlot", "RegistryCap", "VehicleStore" };
    return (id >= 0 && id < kCheckCount) ? k[id] : "?";
}

class RoundA {
public:
    // How long the live-state phase waits for the game to build what it reads, after the patterns resolved.
    static constexpr uint64_t kRuntimeWaitMs = 180000;
    // The pattern scan is rate-limited: a pass walks the whole image once per undecided pattern.
    static constexpr uint64_t kScanEveryMs = 1500;
    // Entries logged one per line; the rest are counted in the histogram only.
    static constexpr uint32_t kMaxEntryLines = 400;
    static constexpr uint32_t kMaxPacks = 4096;

    RoundA(const PackProfile& prof, const Env& env) : P_(prof), E_(env) {
        defs_[kAddDlcPack] = &P_.addDlcPack; defs_[kDlcSetup2] = &P_.dlcSetup2; defs_[kMount] = &P_.mount;
        defs_[kMgrAnchor] = &P_.mgrAnchor;   defs_[kMgrCtor] = &P_.mgrCtor;     defs_[kAtStr] = &P_.atStr;
        defs_[kImageReg] = &P_.imageReg;     defs_[kVehStore] = &P_.vehStore;
    }

    bool Done() const { return summarised_; }
    bool PatternsDone() const { return patternsDone_; }
    int  Total() const { return kCheckCount; }
    int  Ok() const { int n = 0; for (int i = 0; i < kCheckCount; ++i) n += st_[i] == 1; return n; }
    bool CheckOk(int id) const { return id >= 0 && id < kCheckCount && st_[id] == 1; }
    bool CheckDecided(int id) const { return id >= 0 && id < kCheckCount && st_[id] != 0; }
    // The RVA a pattern resolved to, or ~0 when it did not resolve uniquely.
    uint64_t SigRva(int id) const {
        return (id >= 0 && id < 8 && count_[id] == 1) ? (uint64_t)(at_[id] - E_.scanBase) : ~0ull;
    }
    int SigHits(int id) const { return (id >= 0 && id < 8) ? count_[id] : -2; }
    // Operands derived from the patterns (0 until Derive ran or when they did not resolve): the offline test pins these.
    uint64_t MgrGlobalRva() const { return mgrG_; }
    uint64_t CtorVtableRva() const { return ctorVtable_; }
    uint64_t RegistryRva() const { return regRva_; }
    uint64_t RegistryCapRva() const { return regCapRva_; }
    uint32_t RegistrySlot() const { return regSlot_; }
    uint64_t GameConfigRva() const { return cfgRva_; }
    uint64_t FactoryTableRva() const { return tblRva_; }
    uint32_t StoreLimitOffset() const { return vsLimitOff_; }
    uint32_t StoreFactorySlot() const { return vsSlot_; }

    // The caller builds the (expensive) readable-run list only when this says a scan is due.
    bool NeedsScan(uint64_t now) const { return !patternsDone_ && now >= nextScanAt_; }

    // One poll. `runs` = the image's readable runs when NeedsScan() was true, else null. waitLog = the caller's 10 s
    // progress tick; giveUp = the pattern-wait budget ran out (decrypt never finished). Returns true once the summary is
    // logged (the run is over).
    bool Step(uint64_t now, const std::vector<scan::Run>* runs, bool waitLog, bool giveUp) {
        if (summarised_) return true;
        if (!started_) {
            started_ = true;
            Log(false, "[packenh] round A: read-only diagnostic of %s (image va 0x%llx, size 0x%llx); %d checks",
                P_.label, (unsigned long long)E_.vaBase, (unsigned long long)E_.size, (int)kCheckCount);
        }
        if (!patternsDone_) {
            if (runs) ScanPatterns(*runs, waitLog);
            nextScanAt_ = now + kScanEveryMs;
            bool pending = false;
            for (int i = 0; i < 8; ++i) pending = pending || st_[i] == 0;
            if (pending && giveUp) {
                for (int i = 0; i < 8; ++i)
                    if (st_[i] == 0)
                        Set(i, false, "still missing after the wait budget (the code never decrypted, or the pattern is wrong for this build)");
                pending = false;
            }
            if (pending) return false;
            patternsDone_ = true;
            phase2Start_ = now;
            Derive();
            return false;
        }
        RunState(now, waitLog);
        return summarised_;
    }

private:
    struct Rd {
        const RoundA& r;
        template <class T> bool operator()(uint64_t va, T& v) const { return r.E_.read && r.E_.read(r.E_.ctx, va, &v, sizeof(T)); }
    };

    void Log(bool warn, const char* fmt, ...) const {
        char buf[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (E_.log) E_.log(E_.ctx, warn, buf);
    }

    void Set(int id, bool ok, const char* fmt, ...) {
        if (st_[id] != 0) return;
        st_[id] = ok ? 1 : 2;
        char buf[480];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        Log(!ok, "[packenh] check %-16s %s  %s", CheckName(id), ok ? "ok  " : "FAIL", buf);
    }

    bool InImage(uint64_t va) const { return va >= E_.vaBase && va < E_.vaBase + E_.size; }
    uint64_t Va(uint64_t rva) const { return E_.vaBase + rva; }
    static bool HeapPtr(uint64_t p) { return p >= 0x10000 && p < 0x00007FFFFFFF0000ull && (p & 7) == 0; }

    // rva = (match - scanBase) + op.endAt + int32 at match+op.dispAt; false when it leaves the image.
    bool Rip(const uint8_t* match, const RipOp& op, uint64_t& rva) const {
        int32_t d;
        memcpy(&d, match + op.dispAt, 4);
        const int64_t r = (int64_t)(match - E_.scanBase) + op.endAt + d;
        if (r < 0 || (uint64_t)r >= E_.size) return false;
        rva = (uint64_t)r;
        return true;
    }

    void NoteRva(int id) const {
        const uint64_t exp = defs_[id]->expectRva;
        if (exp && SigRva(id) != exp)
            Log(true, "[packenh]   NOTE %s resolved at +0x%llx, the offline dump read +0x%llx (the build differs from the dump)",
                CheckName(id), (unsigned long long)SigRva(id), (unsigned long long)exp);
    }

    void ScanPatterns(const std::vector<scan::Run>& runs, bool waitLog) {
        for (int i = 0; i < 8; ++i) {
            if (st_[i] != 0) continue;
            const PackSig& d = *defs_[i];
            if (!d.pat) { Set(i, false, "this profile has no pattern for it"); continue; }
            const uint8_t* first = nullptr;
            const int c = scan::CountPat(runs, d.pat, &first);
            count_[i] = c;
            at_[i] = first;
            if (c < 0) {
                Set(i, false, "BAD PATTERN");
            } else if (c == 1) {
                Set(i, true, "UNIQUE @ +0x%llx   (%s)", (unsigned long long)(first - E_.scanBase), d.source ? d.source : "");
                NoteRva(i);
            } else if (c > 1) {
                Set(i, false, "ambiguous (%d hits, first @ +0x%llx); not used", c, (unsigned long long)(first - E_.scanBase));
            } else if (!missLogged_[i] || waitLog) {
                missLogged_[i] = true;
                Log(false, "[packenh] pattern %-16s missing (0): the code may still be encrypted; retrying", CheckName(i));
            }
        }
    }

    // ---- cross-checks between the patterns' operands -------------------------------------------------------
    void Derive() {
        // the manager global, two independent ways
        uint64_t ga = 0, gc = 0, vt = 0;
        const bool a = st_[kMgrAnchor] == 1 && Rip(at_[kMgrAnchor], P_.mgrAnchorG, ga);
        const bool c = st_[kMgrCtor] == 1 && Rip(at_[kMgrCtor], P_.mgrCtorG, gc);
        const bool v = st_[kMgrCtor] == 1 && Rip(at_[kMgrCtor], P_.mgrCtorVtable, vt);
        if (a && c && ga == gc) {
            mgrG_ = ga;
            Set(kMgrGlobal, true, "ExtraContentManager* global +0x%llx from both the anchor and the ctor", (unsigned long long)ga);
        } else {
            if (a) mgrG_ = ga; else if (c) mgrG_ = gc;
            Set(kMgrGlobal, false, "anchor %s +0x%llx, ctor %s +0x%llx (need both and equal)", a ? "gives" : "FAILED", (unsigned long long)ga,
                c ? "gives" : "FAILED", (unsigned long long)gc);
        }
        if (v) {
            ctorVtable_ = vt;
            Log(false, "[packenh]   manager vtable from the ctor: +0x%llx (offline dump read +0x%x)", (unsigned long long)vt, (unsigned)P_.mgrVtableRva);
        }

        // atString::Append: the pattern's function vs the call AddDlcPack makes
        if (st_[kAtStr] == 1 && st_[kAddDlcPack] == 1 && P_.atStrCall.pat) {
            const uint8_t* fn = at_[kAddDlcPack];
            const uint8_t* end = E_.scanBase + E_.size;
            size_t len = P_.atStrCallScanLen;
            if ((size_t)(end - fn) < len) len = (size_t)(end - fn);
            std::vector<scan::Run> body = { { fn, len } };
            const uint8_t* hit = nullptr;
            const int n = scan::CountPat(body, P_.atStrCall.pat, &hit);
            uint64_t tgt = 0;
            if (n >= 1 && hit && Rip(hit, P_.atStrCallTarget, tgt) && tgt == SigRva(kAtStr))
                Set(kAtStrCall, true, "AddDlcPack's %d call site(s) land on +0x%llx, the pattern's function", n, (unsigned long long)tgt);
            else
                Set(kAtStrCall, false, "AddDlcPack call sites: %d, first target +0x%llx, pattern function +0x%llx", n, (unsigned long long)tgt,
                    (unsigned long long)SigRva(kAtStr));
        } else {
            Set(kAtStrCall, false, "skipped: needs AddDlcPack and AtStringAppend");
        }

        // image registry
        if (st_[kImageReg] == 1) {
            uint64_t cap = 0, a1 = 0, a2 = 0;
            const uint8_t* m = at_[kImageReg];
            const bool ok = Rip(m, P_.imageRegCap, cap) && Rip(m, P_.imageRegArray, a1) && Rip(m, P_.imageRegArray2, a2);
            uint32_t slot = 0;
            memcpy(&slot, m + P_.imageRegSlotAt, 4);
            if (ok && a1 == a2 && slot >= 8 && slot <= 0x800 && (slot & 7) == 0) {
                regCapRva_ = cap; regRva_ = a1; regSlot_ = slot; regOk_ = true;
                Set(kImageRegOps, true, "registry +0x%llx (both lea agree), bound +0x%llx, collection vtable slot +0x%x%s",
                    (unsigned long long)a1, (unsigned long long)cap, (unsigned)slot,
                    slot == P_.imageRegSlotExpect ? " (as the dump)" : " (the dump read a different slot)");
            } else {
                Set(kImageRegOps, false, "registry lea +0x%llx / +0x%llx (agree=%d), bound +0x%llx (in image=%d), slot 0x%x", (unsigned long long)a1,
                    (unsigned long long)a2, (int)(a1 == a2), (unsigned long long)cap, (int)ok, (unsigned)slot);
            }
        } else {
            Set(kImageRegOps, false, "skipped: ImageRegistry pattern did not resolve");
        }

        // vehicle store init
        if (st_[kVehStore] == 1) {
            const uint8_t* m = at_[kVehStore];
            uint64_t cfg = 0, tbl = 0;
            const bool ok = Rip(m, P_.vsCfg, cfg) && Rip(m, P_.vsTbl, tbl);
            uint32_t off = 0;
            memcpy(&off, m + P_.vsLimitAt, 4);
            const uint32_t slot = m[P_.vsSlotAt];
            if (ok && off != 0 && off <= 0x1000 && (off & 3) == 0 && slot != 0 && (slot & 7) == 0) {
                cfgRva_ = cfg; tblRva_ = tbl; vsLimitOff_ = off; vsSlot_ = slot; vsOk_ = true;
                Set(kVehStoreOps, true, "gameconfig global +0x%llx, factory table +0x%llx, MaxExtraVehicleModelInfos at +0x%x%s, vehicle factory slot 0x%x%s",
                    (unsigned long long)cfg, (unsigned long long)tbl, (unsigned)off, off == P_.vsLimitOffExpect ? "" : " (NOT the expected value)",
                    (unsigned)slot, slot == P_.vsSlotExpect ? "" : " (NOT the expected value)");
            } else {
                Set(kVehStoreOps, false, "cfg +0x%llx table +0x%llx (in image=%d) limit offset 0x%x slot 0x%x", (unsigned long long)cfg,
                    (unsigned long long)tbl, (int)ok, (unsigned)off, (unsigned)slot);
            }
        } else {
            Set(kVehStoreOps, false, "skipped: VehStoreInit pattern did not resolve");
        }
    }

    // ---- the live state -----------------------------------------------------------------------------------
    void SkipUnless(bool have, int first, int last, const char* why) {
        if (have) return;
        for (int i = first; i <= last; ++i) Set(i, false, "skipped: %s", why);
    }

    void RunState(uint64_t now, bool waitLog) {
        const bool isFinal = now - phase2Start_ > kRuntimeWaitMs;
        SkipUnless(mgrG_ != 0, kMgrPtr, kFirstPath, "no manager global resolved");
        SkipUnless(regOk_, kRegEntry0, kRegCap, "image registry operands unresolved");
        SkipUnless(vsOk_, kStore, kStore, "vehicle store operands unresolved");
        if (mgrG_ != 0) StepManager(isFinal, waitLog);
        if (regOk_) StepRegistry(waitLog);
        if (vsOk_) StepStore(waitLog);
        if (isFinal) {
            for (int i = kMgrPtr; i < kCheckCount; ++i)
                if (st_[i] == 0)
                    Set(i, false, "not available %u s after the patterns resolved (the game has not built it yet)", (unsigned)(kRuntimeWaitMs / 1000));
        }
        bool pending = false;
        for (int i = 0; i < kCheckCount; ++i) pending = pending || st_[i] == 0;
        if (!pending) Summarise();
    }

    void Summarise() {
        if (summarised_) return;
        summarised_ = true;
        const int ok = Ok();
        Log(ok != kCheckCount, "[packenh] round A: %d of %d checks ok", ok, (int)kCheckCount);
        if (ok != kCheckCount) {
            std::string bad;
            for (int i = 0; i < kCheckCount; ++i)
                if (st_[i] != 1) { if (!bad.empty()) bad += ", "; bad += CheckName(i); }
            Log(true, "[packenh] round A failed checks: %s", bad.c_str());
        }
    }

    // A NUL-terminated printable-ASCII string of at most maxLen bytes behind a char*; false when it does not read back.
    bool ReadCString(uint64_t ptr, uint32_t len, std::string& out) const {
        out.clear();
        if (!HeapPtr(ptr) && !InImage(ptr)) return false;
        if (len == 0 || len > 259) return false;
        std::vector<char> b(len + 1);
        if (!E_.read || !E_.read(E_.ctx, ptr, b.data(), len + 1)) return false;
        if (b[len] != 0) return false;
        for (uint32_t i = 0; i < len; ++i)
            if ((unsigned char)b[i] < 0x20 || (unsigned char)b[i] > 0x7e) return false;
        out.assign(b.data(), len);
        return true;
    }

    void StepManager(bool isFinal, bool waitLog) {
        const Rd rd{ *this };
        uint64_t mgr = 0;
        rd(Va(mgrG_), mgr);
        if (mgr == 0) {
            if (waitLog) Log(false, "[packenh] waiting: the ExtraContentManager singleton (+0x%llx) is still null", (unsigned long long)mgrG_);
            return;
        }
        if (st_[kMgrPtr] == 0) {
            Set(kMgrPtr, HeapPtr(mgr) && !InImage(mgr), "ExtraContentManager* = 0x%llx (global +0x%llx)", (unsigned long long)mgr, (unsigned long long)mgrG_);
            if (st_[kMgrPtr] == 2) return;
        }
        if (st_[kEntries] != 0) return;   // walked already
        uint64_t vt = 0;
        if (st_[kMgrVtable] == 0) {
            const bool read = rd(mgr, vt);
            const uint64_t expect = ctorVtable_ ? Va(ctorVtable_) : 0;
            Set(kMgrVtable, read && InImage(vt) && (expect == 0 || vt == expect), "vtable 0x%llx = image+0x%llx; the ctor's is image+0x%llx (the dump read +0x%x)",
                (unsigned long long)vt, (unsigned long long)(vt - E_.vaBase), (unsigned long long)ctorVtable_, (unsigned)P_.mgrVtableRva);
        }
        uint64_t arr = 0;
        uint16_t count = 0, cap = 0;
        uint32_t gen = 0;
        const bool okArr = rd(mgr + P_.mgrArray, arr), okCnt = rd(mgr + P_.mgrCount, count);
        if (P_.mgrCap) rd(mgr + P_.mgrCap, cap);
        rd(mgr + P_.mgrGen, gen);
        if (!okArr || !okCnt) {
            if (isFinal) Set(kPackCount, false, "the manager's array / count did not read back");
            return;
        }
        const bool stable = lastCount_ == count && lastArr_ == arr;
        lastCount_ = count; lastArr_ = arr;
        // Wait until the game has stopped adding packs (the same non-zero count and array on two polls); at the end of
        // the budget walk whatever is there.
        if ((count == 0 || !stable) && !isFinal) {
            if (waitLog) Log(false, "[packenh] waiting: manager has %u pack(s) (array 0x%llx), not settled yet", (unsigned)count, (unsigned long long)arr);
            return;
        }
        Log(false, "[packenh] manager 0x%llx: %u pack(s), array 0x%llx, capacity %u, generation %u (entry stride 0x%x)", (unsigned long long)mgr,
            (unsigned)count, (unsigned long long)arr, (unsigned)cap, (unsigned)gen, (unsigned)P_.entryStride);
        Set(kPackCount, count >= 1 && count <= kMaxPacks && (cap == 0 || count <= cap) && HeapPtr(arr), "%u pack(s), capacity %u", (unsigned)count, (unsigned)cap);
        WalkEntries(arr, count);
        uint64_t arr2 = 0;
        uint16_t count2 = 0;
        if (rd(mgr + P_.mgrArray, arr2) && rd(mgr + P_.mgrCount, count2) && (arr2 != arr || count2 != count))
            Log(true, "[packenh] the manager changed during the walk (array 0x%llx -> 0x%llx, count %u -> %u): the game was still adding packs",
                (unsigned long long)arr, (unsigned long long)arr2, (unsigned)count, (unsigned)count2);
    }

    void WalkEntries(uint64_t arr, uint32_t count) {
        const Rd rd{ *this };
        if (count == 0 || count > kMaxPacks || !HeapPtr(arr)) {
            Set(kEntries, false, "nothing to walk (count %u, array 0x%llx)", (unsigned)count, (unsigned long long)arr);
            Set(kFirstPath, false, "nothing to read");
            return;
        }
        uint32_t badRead = 0, badField = 0, firstBad = ~0u;
        uint32_t types[4] = {}, typeOther = 0, states[7] = {}, stateOther = 0;
        std::string firstBadWhy;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t e = arr + (uint64_t)i * P_.entryStride;
            uint32_t hash = 0, type = 0, state = 0;
            const bool ok = rd(e + P_.entryHash, hash) && rd(e + P_.entryType, type) && rd(e + P_.entryState, state);
            if (!ok) {
                ++badRead;
                if (firstBad == ~0u) { firstBad = i; firstBadWhy = "unreadable"; }
                if (i < kMaxEntryLines) Log(true, "[packenh] entry #%u @0x%llx: UNREADABLE", (unsigned)i, (unsigned long long)e);
                continue;
            }
            const bool plausible = hash != 0 && (type == P_.typePackfile || type == P_.typeRelative) && state <= 6;
            if (!plausible) {
                ++badField;
                if (firstBad == ~0u) {
                    firstBad = i;
                    char b[96];
                    snprintf(b, sizeof(b), "hash 0x%08x type %u state %u", (unsigned)hash, (unsigned)type, (unsigned)state);
                    firstBadWhy = b;
                }
            }
            if (type < 4) ++types[type]; else ++typeOther;
            if (state < 7) ++states[state]; else ++stateOther;
            if (i < kMaxEntryLines)
                Log(!plausible, "[packenh] entry #%u hash=0x%08x type=%u state=%u%s", (unsigned)i, (unsigned)hash, (unsigned)type, (unsigned)state,
                    plausible ? "" : "  <-- implausible");
        }
        if (count > kMaxEntryLines) Log(false, "[packenh] (%u more entries not listed individually)", (unsigned)(count - kMaxEntryLines));
        Log(false, "[packenh] entries by type: 1=%u 2=%u 0=%u 3=%u other=%u; by state: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u other=%u", (unsigned)types[1],
            (unsigned)types[2], (unsigned)types[0], (unsigned)types[3], (unsigned)typeOther, (unsigned)states[0], (unsigned)states[1], (unsigned)states[2],
            (unsigned)states[3], (unsigned)states[4], (unsigned)states[5], (unsigned)states[6], (unsigned)stateOther);
        if (badRead == 0 && badField == 0)
            Set(kEntries, true, "%u entries read back at stride 0x%x, all with a hash, a type of %u/%u and a state <= 6", (unsigned)count, (unsigned)P_.entryStride,
                (unsigned)P_.typeRelative, (unsigned)P_.typePackfile);
        else
            Set(kEntries, false, "%u unreadable, %u implausible of %u; first bad #%u (%s)", (unsigned)badRead, (unsigned)badField, (unsigned)count, (unsigned)firstBad,
                firstBadWhy.c_str());

        // The first entry's path, and the last one's as extra evidence the stride is right.
        for (int which = 0; which < 2; ++which) {
            const uint32_t idx = which == 0 ? 0 : count - 1;
            if (which == 1 && count < 2) break;
            const uint64_t e = arr + (uint64_t)idx * P_.entryStride;
            uint64_t ptr = 0;
            uint16_t len = 0, pcap = 0;
            std::string s;
            const bool ok = rd(e + P_.entryPath, ptr) && rd(e + P_.entryPathLen, len);
            rd(e + P_.entryPathLen + 2, pcap);
            const bool str = ok && ReadCString(ptr, len, s);
            if (which == 0) {
                Set(kFirstPath, str, "entry #0 path ptr 0x%llx len %u cap %u: \"%s\"", (unsigned long long)ptr, (unsigned)len, (unsigned)pcap, s.c_str());
            } else {
                Log(!str, "[packenh] last entry #%u path ptr 0x%llx len %u: %s%s%s", (unsigned)idx, (unsigned long long)ptr, (unsigned)len, str ? "\"" : "(unreadable)",
                    s.c_str(), str ? "\"" : "");
            }
        }
    }

    void StepRegistry(bool waitLog) {
        const Rd rd{ *this };
        uint64_t e0 = 0;
        rd(Va(regRva_), e0);
        if (e0 == 0) {
            if (waitLog) Log(false, "[packenh] waiting: image registry[0] (+0x%llx) is still null", (unsigned long long)regRva_);
            return;
        }
        if (st_[kRegEntry0] == 0)
            Set(kRegEntry0, HeapPtr(e0) || InImage(e0), "registry[0] = 0x%llx (array +0x%llx)", (unsigned long long)e0, (unsigned long long)regRva_);
        uint64_t vt = 0;
        if (st_[kRegVtable] == 0) {
            const bool ok = rd(e0, vt);
            Set(kRegVtable, ok && InImage(vt), "entry 0 vtable 0x%llx = image+0x%llx (%s)", (unsigned long long)vt, (unsigned long long)(vt - E_.vaBase),
                (vt - E_.vaBase) == P_.rawStreamerVtRva ? "the RawStreamer's, as in the dump" : "NOT the dump's RawStreamer vtable");
        }
        if (st_[kRegSlot] == 0 && st_[kRegVtable] != 0) {
            uint64_t fn = 0;
            const bool ok = st_[kRegVtable] == 1 && rd(vt + regSlot_, fn);
            Set(kRegSlot, ok && InImage(fn), "vtable slot +0x%x -> 0x%llx = image+0x%llx", (unsigned)regSlot_, (unsigned long long)fn,
                (unsigned long long)(fn - E_.vaBase));
        }
        if (st_[kRegCap] == 0) {
            uint32_t cap = 0;
            const bool ok = rd(Va(regCapRva_), cap);
            Set(kRegCap, ok && cap >= 1 && cap <= 0xFFFF, "registry bound (+0x%llx) = %u", (unsigned long long)regCapRva_, (unsigned)cap);
        }
    }

    void StepStore(bool waitLog) {
        const Rd rd{ *this };
        uint32_t maxExtra = 0;
        bool haveMax = false;
        uint64_t gc = 0, lim = 0;
        if (rd(Va(cfgRva_), gc) && gc && rd(gc + 0x18, lim) && lim && rd(lim + vsLimitOff_, maxExtra)) haveMax = true;
        uint64_t table = 0, fac = 0, buckets = 0;
        uint16_t nb = 0;
        if (!(rd(Va(tblRva_), table) && table && rd(table + vsSlot_, fac) && fac && rd(fac + 8, buckets) && buckets && rd(fac + 0x10, nb) && nb)) {
            if (waitLog) Log(false, "[packenh] waiting: the vehicle factory is not built yet (table 0x%llx)", (unsigned long long)table);
            return;
        }
        uint64_t node = 0;
        rd(buckets + (uint64_t)(kDlcKey % nb) * 8, node);
        uint64_t hdr = 0;
        for (int hops = 0; node && hops < 4096; ++hops) {
            uint32_t key = 0;
            if (!rd(node, key)) break;
            if (key == kDlcKey) { rd(node + 8, hdr); break; }
            uint64_t next = 0;
            if (!rd(node + 0x10, next)) break;
            node = next;
        }
        if (!hdr) {
            if (waitLog) Log(false, "[packenh] waiting: the DLC vehicle store (key 0x%x) does not exist yet%s", (unsigned)kDlcKey,
                             haveMax ? "" : " (the gameconfig limit is not readable either)");
            return;
        }
        uint64_t base = 0;
        uint16_t n = 0, cap = 0;
        const bool ok = rd(hdr, base) && rd(hdr + 8, n) && rd(hdr + 0xa, cap);
        Set(kStore, ok && cap > 0 && n <= cap, "DLC vehicle store (key 0x%x) header 0x%llx: %u of %u used, base 0x%llx; MaxExtraVehicleModelInfos %s%u", (unsigned)kDlcKey,
            (unsigned long long)hdr, (unsigned)n, (unsigned)cap, (unsigned long long)base, haveMax ? "" : "(unreadable) ", (unsigned)maxExtra);
    }

    static constexpr uint32_t kDlcKey = 0xf000u;   // the key the DLC stores are created under (Legacy 0x64191c; Enhanced 0x1334b36)

    const PackProfile& P_;
    Env E_;
    const PackSig* defs_[8] = {};
    int  count_[8] = { -2, -2, -2, -2, -2, -2, -2, -2 };
    const uint8_t* at_[8] = {};
    bool missLogged_[8] = {};
    int  st_[kCheckCount] = {};          // 0 pending, 1 ok, 2 FAIL
    bool started_ = false, patternsDone_ = false, summarised_ = false;
    uint64_t nextScanAt_ = 0, phase2Start_ = 0;
    // derived operands
    uint64_t mgrG_ = 0, ctorVtable_ = 0;
    uint64_t regRva_ = 0, regCapRva_ = 0;
    uint32_t regSlot_ = 0;
    bool     regOk_ = false;
    uint64_t cfgRva_ = 0, tblRva_ = 0;
    uint32_t vsLimitOff_ = 0, vsSlot_ = 0;
    bool     vsOk_ = false;
    // manager settle detection
    uint16_t lastCount_ = 0xFFFF;
    uint64_t lastArr_ = 0;
};

}  // namespace gage::game::content::packenh
