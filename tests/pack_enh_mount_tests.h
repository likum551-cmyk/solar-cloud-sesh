// pack_enh_mount_tests.h: offline proofs for Round B of the Enhanced car-pack port (pack_enh=2): the mount patterns against the
// decrypted dump, the Extras resolver, and the Mounter (apps/engine/pack_enh_mount_core.h) against a FAKE GAME that keeps the
// structures the offline reads describe (it grows the manager's array when it is full and poisons the old one, as a freed block
// would be). Included by tests/pack_profile_test.cpp after FakeProc / CHECK / kImgVa are defined; the counts are asserted there.
#pragma once

#include "../apps/engine/pack_enh_mount_core.h"

using gage::game::content::packenh::Extras;
using gage::game::content::packenh::MountAbi;
using gage::game::content::packenh::MountOut;
using gage::game::content::packenh::Mounter;

// ---- 1. the profile (no image needed) ------------------------------------------------------------------------------------
static void TestMountProfile() {
    const PackProfile& L = kPackLegacy;
    const PackProfile& E = kPackEnhanced;
    // Legacy keeps its ABI: the new fields only name what the shipped code hard-coded
    CHECK(!L.runGroupByEntry && L.entryMountPoint == 0x30 && L.entryMountPointLen == 0x38 && L.regStreamInfoCountAt == 8 && L.regStreamInfo.pat == nullptr,
          "legacy: RunPackGroup by hash, mount point atString +0x30/+0x38, streaming-info count at +8");
    CHECK(L.streamInfoMgrRva == 0x2ea23d0u && L.assetGlobalRva == 0x1e2ee90u, "legacy single RVAs untouched");
    CHECK(E.runGroupByEntry && E.entryMountPoint == 0x40 && E.entryMountPointLen == 0x48 && E.regStreamInfoCountAt == 0 && E.regStreamInfoScanLen == 0x100,
          "enhanced: RunPackGroup by entry pointer, mount point atString +0x40/+0x48, streaming-info count not read");
    CHECK(E.streamInfoMgrRva == 0x42f9e48u && E.assetGlobalRva == 0x2bc3fe0u, "enhanced offline RVAs");
    const PackSig* S[7] = { &E.runPackGroup, &E.getDevice, &E.pushFolder, &E.exists, &E.popFolder, &E.regStream, &E.regStreamInfo };
    for (int i = 0; i < 7; ++i) {
        std::vector<uint8_t> b, m;
        CHECK(S[i]->pat && scan::ParsePat(S[i]->pat, b, m), "enhanced round-B sig %d parses", i);
        if (!S[i]->pat) continue;
        int fixed = 0;
        for (uint8_t x : m) fixed += x;
        CHECK(fixed >= 8, "enhanced %s has only %d fixed bytes", S[i]->name, fixed);
        CHECK(S[i]->expectRva != 0 && S[i]->source && *S[i]->source, "enhanced %s has an expectRva and a source", S[i]->name);
    }
    CHECK(E.mountGate.pat == nullptr, "enhanced has no MountGate pattern (the game's loop inlines it; the Mounter does the same)");
    CHECK(!strcmp(E.popFolder.pat, L.popFolder.pat), "PopFolder is the same machine code in both builds");
    CHECK(E.entryStride == 0x120 && E.entryHash == 0x80 && E.entryPath == 0xd8 && E.entryPathLen == 0xe0 && E.entryType == 0xf8 && E.entryState == 0x100,
          "enhanced DlcEntry layout (unchanged from round A)");
}

// ---- 2. the mount patterns against the dump ------------------------------------------------------------------------------
static void TestMountPatterns(std::vector<uint8_t>& img) {
    const PackProfile& E = kPackEnhanced;
    const std::vector<scan::Run> runs = { { img.data(), img.size() } };
    const PackSig* S[7] = { &E.runPackGroup, &E.getDevice, &E.pushFolder, &E.exists, &E.popFolder, &E.regStream, &E.regStreamInfo };
    for (int i = 0; i < 7; ++i) {
        const uint8_t* first = nullptr;
        const int c = scan::CountPat(runs, S[i]->pat, &first);
        CHECK(c == 1, "enhanced %s: %d hits in the whole image (need exactly 1)", S[i]->name, c);
        CHECK(c == 1 && (uint64_t)(first - img.data()) == S[i]->expectRva, "enhanced %s at +0x%llx, expected +0x%llx", S[i]->name,
              (unsigned long long)(first ? first - img.data() : 0), (unsigned long long)S[i]->expectRva);
    }
    // none of the Legacy-only mount patterns lands here (PopFolder is the one shared routine, asserted in TestEnhancedPatterns)
    CHECK(scan::CountPat(runs, kPackLegacy.runPackGroup.pat, nullptr) == 0 && scan::CountPat(runs, kPackLegacy.regStream.pat, nullptr) == 0 &&
          scan::CountPat(runs, kPackLegacy.getDevice.pat, nullptr) == 0, "the Legacy RunPackGroup / RegStreamFile / GetDevice patterns do not hit the Enhanced image");

    // The game's pack loop (0xc28b18): the evidence for the order Add -> Setup2 -> Mount (state 1 -> 2) -> RunPackGroup(early_on, startup)
    const uint8_t* p = img.data();
    CHECK(!memcmp(p + 0xc28c18, "\x83\xBC\x1F\x00\x01\x00\x00\x01\x75\x29", 10), "loop: `cmp [entry+0x100],1; jne` guards the Mount (state 1 only)");
    CHECK(!memcmp(p + 0xc28c22, "\x66\x83\x7C\x1F\x48\x00", 6) && !memcmp(p + 0xc28c2b, "\x74\x05\x4C\x8B\x44\x1F\x40", 7),
          "loop: Mount's r8 = the mount point atString: length WORD at entry+0x48 tested, pointer at entry+0x40 loaded (else the empty string)");
    CHECK(p[0xc28c32] == 0xE8 && 0xc28c37 + *(const int32_t*)(p + 0xc28c33) == 0xc2c290, "loop: the call after it is Mount (+0xc2c290)");
    CHECK(!memcmp(p + 0xc28c37, "\x85\xC0\x75\x10", 4) && !memcmp(p + 0xc28c45, "\xC7\x00\x02\x00\x00\x00", 6),
          "loop: Mount's return 0 -> `mov dword [entry+0x100], 2` (the state write the Mounter repeats)");
    CHECK(!memcmp(p + 0xc28d1d, "\xC7\x44\x24\x38\x08\xD4\x8B\x6F", 8), "loop: group early_on = 0x6f8bd408 stored at [rsp+0x38]");
    CHECK(!memcmp(p + 0xc28ae5, "\xC7\x44\x24\x38\x8F\x57\xF6\x5B", 8), "loop: group startup = 0x5bf6578f stored at [rsp+0x38]");
    CHECK(!memcmp(p + 0xc28aed, "\x48\x89\xF1\x4C\x8D\x44\x24\x38\xE8", 9) && 0xc28afa + *(const int32_t*)(p + 0xc28af6) == 0xc26f00,
          "loop: RunPackGroup(rcx = manager, rdx = the entry, r8 = &group) at +0xc28af5");
    CHECK(!memcmp(p + 0xc26f10, "\x48\x85\xD2\x0F\x84", 5) && !memcmp(p + 0xc26f19, "\x4D\x89\xC5\x48\x89\xD7\x49\x89\xCC", 9) && !memcmp(p + 0xc26f33, "\x41\x8B\x10", 3),
          "RunPackGroup: rdx = entry (null-checked), r8 = pointer to the group hash (edx = [r8])");

    // GetDevice: the device it returns (the packfile device, vtable +0x2488e90) has the four slots ReadGameFile calls
    auto slot = [&](uint64_t vt, uint64_t off) { uint64_t v; memcpy(&v, p + vt + off, 8); return v - kImgVa; };
    CHECK(slot(0x2488e90, 0x08) == 0x110580 && slot(0x2488e90, 0x30) == 0x110d50 && slot(0x2488e90, 0x60) == 0x110ce0 && slot(0x2488e90, 0xb0) == 0x111250,
          "device vtable: +0x08 Open, +0x30 Read, +0x60 Close, +0xb0 GetFileSize");
    CHECK(p[0x101810] == 0xE9 && 0x101815 + *(const int32_t*)(p + 0x101811) == 0x10caf0, "the device-for-path thunk +0x101810 jumps to the GetDevice pattern's function");

    // RegStreamFile: device -> (+0x170 image index, +0x150 isCollection) -> RawStreamer (accessor 0x1279990) +0x208 -> the register call
    auto has = [&](uint64_t at, size_t len, const char* pat) { return CodeHas(img, at, len, pat); };
    CHECK(has(0xce46d0, 0x100, "FF 90 70 01 00 00") && has(0xce46d0, 0x100, "FF 90 50 01 00 00") && has(0xce46d0, 0x100, "FF 90 08 02 00 00") && has(0xce46d0, 0x100, "FF 93 08 02 00 00"),
          "RegStreamFile calls the device's +0x170 / +0x150 and a +0x208 on the device and on the RawStreamer");
    CHECK(slot(0x2665bb0, 0x208) == 0x127a110, "the RawStreamer vtable's +0x208 is 0x127a110 (appends the path to its file list)");
    CHECK(has(0xce46d0, 0x100, "E8 ?? ?? ?? ?? 48 89 C3 48 85 C0") && has(0xce4793, 0x20, "48 8D 0D ?? ?? ?? ?? 4C 89 F2 49 89 F8 41 89 C1 E8"),
          "RegStreamFile: GetDevice thunk, then `lea rcx,[streamInfo]; mov rdx,r14 (out); mov r8,rdi (name); mov r9d,eax (handle); call register`");
    {
        const int32_t rel = *(const int32_t*)(p + 0xce47a4);
        CHECK(p[0xce47a3] == 0xE8 && 0xce47a8 + rel == 0xce3320, "...the register routine is +0xce3320");
        CHECK(has(0xce3320, 0x1f0, "49 8B 75 00 41 89 DA 42 8B 2C D6"), "the register routine reads the handle array through [this+0], 8-byte entries");
    }
    CHECK(has(0x1279990, 0x10, "48 8B 35 ?? ?? ?? ??") && 0x127999c + *(const int32_t*)(p + 0x1279998) == 0x4681bc0, "the RawStreamer singleton accessor (0x1279990) reads the global +0x4681bc0");
    {   // the Extras resolver, run as the engine runs it
        FakeProc f(img);
        Extras x(kPackEnhanced, f.MakeEnv());
        std::vector<scan::Run> r = { { img.data(), img.size() } };
        CHECK(x.Step(1000, &r, false), "extras resolve in one pass");
        CHECK(x.Done() && x.Ok(), "extras: all decided, all critical ones ok");
        const uint64_t want[packenh::kExtraCount] = { 0xc26f00, 0xce46d0, 0x10caf0, 0x102720, 0x101e00, 0x102ab0, 0x42f9e48 };
        bool all = true;
        for (int i = 0; i < packenh::kExtraCount; ++i) all = all && x.Rva(i) == want[i];
        CHECK(all, "extras resolved at the dump's RVAs (RunPackGroup, RegStreamFile, GetDevice, PushFolder, Exists, PopFolder, StreamInfoMgr)");
        CHECK(f.Has("pattern StreamInfoMgr") && f.Has("+0x42f9e48 from the register call at RegStreamFile+0xc3") && f.warns == 0, "log names the streaming-info manager and warns about nothing");
    }
    {   // a broken critical pattern fails and takes the dependent one with it; a broken diagnostic pattern does not block the mount
        PackProfile bad = kPackEnhanced;
        static char pat[512];
        snprintf(pat, sizeof(pat), "%s", kPackEnhanced.regStream.pat);
        pat[0] = '5'; pat[1] = '7';
        bad.regStream.pat = pat;
        FakeProc f(img);
        Extras x(bad, f.MakeEnv());
        std::vector<scan::Run> r = { { img.data(), img.size() } };
        x.Step(1000, &r, true);
        CHECK(x.Done() && !x.Ok() && !x.IdOk(packenh::kXRegStream) && !x.IdOk(packenh::kXStreamInfo) && x.IdOk(packenh::kXRunPackGroup) && x.IdOk(packenh::kXGetDevice),
              "a broken RegStreamFile pattern fails it and the streaming-info manager, nothing else");
        PackProfile bad2 = kPackEnhanced;
        bad2.pushFolder.pat = "48 8B";   // ambiguous
        FakeProc f2(img);
        Extras x2(bad2, f2.MakeEnv());
        x2.Step(1000, &r, true);
        CHECK(x2.Done() && x2.Ok() && !x2.IdOk(packenh::kXPushFolder) && x2.Hits(packenh::kXPushFolder) > 1 && f2.Has("diagnostic only"),
              "an ambiguous PushFolder pattern is refused but is diagnostic only");
        PackProfile bad3 = kPackEnhanced;
        bad3.regStreamInfoOp = { 4, 8 };   // a shifted operand lands on another address, which the exact RVA check above would catch
        FakeProc f3(img);
        Extras x3(bad3, f3.MakeEnv());
        x3.Step(1000, &r, true);
        CHECK(x3.Rva(packenh::kXStreamInfo) != 0x42f9e48, "a shifted streaming-info operand does not derive the right global (+0x%llx)", (unsigned long long)x3.Rva(packenh::kXStreamInfo));
    }
    {   // which patterns are critical, and what an ambiguous critical pattern does: each case breaks ONE pattern and leaves the others intact
        CHECK(packenh::ExtraCritical(packenh::kXRunPackGroup) && packenh::ExtraCritical(packenh::kXRegStream) && packenh::ExtraCritical(packenh::kXGetDevice) &&
                  packenh::ExtraCritical(packenh::kXStreamInfo) && !packenh::ExtraCritical(packenh::kXPushFolder) && !packenh::ExtraCritical(packenh::kXExists) &&
                  !packenh::ExtraCritical(packenh::kXPopFolder),
              "RunPackGroup, RegStreamFile, GetDevice and the streaming-info manager are critical; PushFolder / Exists / PopFolder are diagnostic only");
        // RunPackGroup twice in the image (a second copy of its first 128 bytes, longer than the pattern, in a separate run): two hits, ambiguous, refused, the mount blocked
        std::vector<uint8_t> copy(img.begin() + 0xc26f00, img.begin() + 0xc26f00 + 128);
        std::vector<scan::Run> dupRuns = { { img.data(), img.size() }, { copy.data(), copy.size() } };
        FakeProc f(img);
        Extras x(kPackEnhanced, f.MakeEnv());
        x.Step(1000, &dupRuns, true);
        CHECK(x.Done() && !x.Ok() && !x.IdOk(packenh::kXRunPackGroup) && x.Hits(packenh::kXRunPackGroup) == 2 && x.IdOk(packenh::kXRegStream) &&
                  x.IdOk(packenh::kXGetDevice) && x.IdOk(packenh::kXStreamInfo) && f.Has("ambiguous (2 hits"),
              "a RunPackGroup pattern with 2 hits is refused as ambiguous and blocks the mount (hits %d)", x.Hits(packenh::kXRunPackGroup));
        // only the streaming-info call pattern broken: RegStreamFile itself still resolves, yet the mount is blocked
        PackProfile noInfo = kPackEnhanced;
        static char infoPat[256];
        snprintf(infoPat, sizeof(infoPat), "%s", kPackEnhanced.regStreamInfo.pat);
        infoPat[0] = '5'; infoPat[1] = '7';
        noInfo.regStreamInfo.pat = infoPat;
        FakeProc f2(img);
        Extras x2(noInfo, f2.MakeEnv());
        std::vector<scan::Run> one = { { img.data(), img.size() } };
        x2.Step(1000, &one, true);
        CHECK(x2.Done() && !x2.Ok() && x2.IdOk(packenh::kXRegStream) && !x2.IdOk(packenh::kXStreamInfo) && x2.IdOk(packenh::kXRunPackGroup) && x2.IdOk(packenh::kXGetDevice),
              "a missing streaming-info register call blocks the mount although RegStreamFile resolved");
        // only the diagnostic patterns broken: the mount is not blocked
        PackProfile noDiag = kPackEnhanced;
        noDiag.pushFolder.pat = "57 57 57 57 57 57 57 57 57 57";
        noDiag.exists.pat = "56 56 56 56 56 56 56 56 56 56";
        FakeProc f3(img);
        Extras x3b(noDiag, f3.MakeEnv());
        x3b.Step(1000, &one, true);
        CHECK(x3b.Done() && x3b.Ok() && !x3b.IdOk(packenh::kXPushFolder) && !x3b.IdOk(packenh::kXExists) && x3b.IdOk(packenh::kXPopFolder),
              "broken PushFolder / Exists patterns do not block the mount");
    }
    {   // an image of zeroes: nothing resolves, no vacuous pass
        std::vector<uint8_t> blank(img.size() / 8, 0);
        FakeProc f(blank);
        Extras x(kPackEnhanced, f.MakeEnv());
        std::vector<scan::Run> r = { { blank.data(), blank.size() } };
        x.Step(1000, &r, false);
        CHECK(!x.Done(), "pending while the code may still be encrypted");
        x.Step(300000, &r, true);
        bool any = false;
        for (int i = 0; i < packenh::kExtraCount; ++i) any = any || x.IdOk(i);
        CHECK(x.Done() && !x.Ok() && !any, "an image of zeroes resolves nothing");
    }
}

// ---- 3. the Mounter against a fake game ----------------------------------------------------------------------------------
struct FakeGame {
    FakeProc& f;
    const PackProfile& P = kPackEnhanced;
    uint64_t mgr = 0;
    // behaviour switches
    bool addFails = false, setup2Fails = false, mountFails = false, brokenAppend = false, poisonFreed = true, setup2ZeroHash = false;
    uint16_t pathCap = 200;       // capacity of the path buffer AddDlcPack gives a new entry (small = the game's append reallocates)
    uint32_t growBy = 16;
    // what happened
    struct Call { std::string what; uint64_t a = 0; bool loggedFirst = false; };
    std::vector<Call> calls;
    struct Group { uint64_t entry; uint32_t group; uint32_t hashRead; uint32_t stateRead; };
    std::vector<Group> groups;
    int writes = 0;
    std::vector<std::string> mountPoints;   // what Mount was given

    explicit FakeGame(FakeProc& fp) : f(fp) {}
    uint64_t Arr() const { uint64_t a = 0; FakeProc::Read(&f, mgr + P.mgrArray, &a, 8); return a; }
    uint16_t Cnt() const { uint16_t c = 0; FakeProc::Read(&f, mgr + P.mgrCount, &c, 2); return c; }
    uint16_t Cap() const { uint16_t c = 0; FakeProc::Read(&f, mgr + P.mgrCap, &c, 2); return c; }
    uint64_t EntryAt(uint32_t i) const { return Arr() + (uint64_t)i * P.entryStride; }
    template <class T> T Get(uint64_t va) const { T v{}; FakeProc::Read(&f, va, &v, sizeof(T)); return v; }

    // The manager as Round A saw the live one: `n` packs filling an array of exactly `n` (capacity n), entry 0 a relative device.
    void Build(uint32_t n, uint64_t vtableVa) {
        mgr = f.Alloc(0x9b8);
        const uint64_t arr = f.Alloc((size_t)n * P.entryStride + 16);
        f.Put<uint64_t>(mgr, vtableVa);
        f.Put<uint64_t>(mgr + P.mgrArray, arr);
        f.Put<uint16_t>(mgr + P.mgrCount, (uint16_t)n);
        f.Put<uint16_t>(mgr + P.mgrCap, (uint16_t)n);
        f.Put<uint32_t>(mgr + P.mgrGen, 0);
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t e = arr + (uint64_t)i * P.entryStride;
            f.Put<uint32_t>(e + P.entryHash, 0x7000u + i);
            f.Put<uint32_t>(e + P.entryType, i == 0 ? P.typeRelative : P.typePackfile);
            f.Put<uint32_t>(e + P.entryState, i % 2 ? 1 : 2);
        }
    }

    MountAbi Abi() {
        MountAbi a;
        a.read = &ReadThunk;
        a.write = &WriteThunk;
        a.addDlcPack = &AddThunk;
        a.dlcSetup2 = &Setup2Thunk;
        a.mount = &MountThunk;
        a.atAppend = &AppendThunk;
        a.runGroup = &GroupThunk;
        a.log = &LogThunk;
        a.ctx = this;
        return a;
    }
    // loggedFirst: the LAST log line at the moment of the call is the step line that announces it
    void Note(const char* what, uint64_t a, const char* needle) {
        Call c; c.what = what; c.a = a;
        c.loggedFirst = needle && !f.lines.empty() && f.lines.back().find(needle) != std::string::npos;
        calls.push_back(c);
    }
    int serial = 0;
    static bool ReadThunk(void* ctx, uint64_t va, void* out, size_t n) { return FakeProc::Read(&static_cast<FakeGame*>(ctx)->f, va, out, n); }
    static void LogThunk(void* ctx, bool warn, const char* line) { FakeProc::Log(&static_cast<FakeGame*>(ctx)->f, warn, line); }
    int CallsOf(const char* what) const { int n = 0; for (const Call& c : calls) n += c.what == what; return n; }
    bool AllLoggedFirst() const { for (const Call& c : calls) if (!c.loggedFirst) return false; return true; }

    static bool WriteThunk(void* ctx, uint64_t va, const void* in, size_t n) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        uint8_t* p = g->f.Ptr(va, n);
        if (!p || (va >= kImgVa && va < kImgVa + g->f.img.size())) return false;
        // the first write of a pack must have been announced by its step line
        g->Note("write", va, "WRITING");
        ++g->writes;
        memcpy(p, in, n);
        return true;
    }
    uint64_t NewString(const char* s, uint16_t cap) {
        const uint64_t va = f.Alloc(cap);
        memcpy(f.Ptr(va, strlen(s) + 1), s, strlen(s) + 1);
        return va;
    }
    static uint8_t AddThunk(void* ctx, uint64_t mgr, const char* path) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        g->Note("AddDlcPack", mgr, "step 1/6 AddDlcPack");
        if (g->addFails) return 0;
        const PackProfile& P = g->P;
        uint64_t arr = g->Arr();
        const uint16_t n = g->Cnt(), cap = g->Cap();
        if (n >= cap) {   // full: grow, copy, and "free" the old block
            const uint16_t ncap = (uint16_t)(cap + g->growBy);
            const uint64_t na = g->f.Alloc((size_t)ncap * P.entryStride);
            memcpy(g->f.Ptr(na, (size_t)n * P.entryStride), g->f.Ptr(arr, (size_t)n * P.entryStride), (size_t)n * P.entryStride);
            if (g->poisonFreed) memset(g->f.Ptr(arr, (size_t)n * P.entryStride), 0xDD, (size_t)n * P.entryStride);
            g->f.Put<uint64_t>(mgr + P.mgrArray, na);
            g->f.Put<uint16_t>(mgr + P.mgrCap, ncap);
            arr = na;
        }
        const uint64_t e = arr + (uint64_t)n * P.entryStride;
        memset(g->f.Ptr(e, P.entryStride), 0, P.entryStride);
        g->f.Put<uint32_t>(e + P.entryHash, 0x5000u + n);
        const uint64_t str = g->NewString(path, (uint16_t)(strlen(path) + 1 > g->pathCap ? strlen(path) + 1 : g->pathCap));   // the buffer always holds the path; the entry's capacity field says pathCap
        g->f.Put<uint64_t>(e + P.entryPath, str);
        g->f.Put<uint16_t>(e + P.entryPathLen, (uint16_t)strlen(path));
        g->f.Put<uint16_t>(e + P.entryPathLen + 2, g->pathCap);
        g->f.Put<uint32_t>(e + P.entryType, P.typePackfile);
        g->f.Put<uint32_t>(e + P.entryState, 0);
        g->f.Put<uint16_t>(mgr + P.mgrCount, (uint16_t)(n + 1));
        return 1;
    }
    static uint8_t Setup2Thunk(void* ctx, uint64_t entry, uint32_t gen) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        g->Note("DlcSetup2", entry, "step 3/6 DlcSetup2");
        (void)gen;
        if (g->setup2Fails) { g->f.Put<uint32_t>(entry + g->P.entryState, 5); return 0; }
        const uint32_t st = g->Get<uint32_t>(entry + g->P.entryState);
        if (st == 0) {
            char mp[40];
            snprintf(mp, sizeof(mp), "dlc_fake%d:/", ++g->serial);
            g->f.Put<uint32_t>(entry + g->P.entryHash, g->setup2ZeroHash ? 0u : 0xC0DE0000u + (uint32_t)g->serial);
            g->f.Put<uint64_t>(entry + g->P.entryMountPoint, g->NewString(mp, 40));
            g->f.Put<uint16_t>(entry + g->P.entryMountPointLen, (uint16_t)strlen(mp));
            g->f.Put<uint32_t>(entry + g->P.entryState, 1);
        }
        return 1;
    }
    static int MountThunk(void* ctx, uint64_t entry, uint32_t, uint64_t mpVa) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        g->Note("Mount", entry, "step 4/6 Mount");
        std::string mp;
        if (mpVa) { char b[64] = {}; FakeProc::Read(&g->f, mpVa, b, 63); mp = b; }
        g->mountPoints.push_back(mp);
        if (g->mountFails) return 2;
        if (g->Get<uint32_t>(entry + g->P.entryType) == g->P.typeRelative && g->Get<uint16_t>(entry + g->P.entryPathLen) == 0) return 1;   // a relative device over nothing
        g->f.Put<uint64_t>(entry, g->f.Alloc(0x118));
        return 0;
    }
    static void AppendThunk(void* ctx, uint64_t at, const char* src, int len) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        g->Note("append", at, "step 2/6 folder override");
        if (g->brokenAppend) return;
        uint64_t ptr = g->Get<uint64_t>(at);
        uint16_t cur = g->Get<uint16_t>(at + 8), cap = g->Get<uint16_t>(at + 10);
        if ((uint32_t)cur + (uint32_t)len + 1 > cap) {   // the game's allocator: a new buffer, the old content copied
            const uint16_t ncap = (uint16_t)(cur + len + 1);
            const uint64_t nb = g->f.Alloc(ncap);
            memcpy(g->f.Ptr(nb, cur), g->f.Ptr(ptr, cur), cur);
            ptr = nb; cap = ncap;
            g->f.Put<uint64_t>(at, ptr);
            g->f.Put<uint16_t>(at + 10, cap);
        }
        memcpy(g->f.Ptr(ptr + cur, (size_t)len), src, (size_t)len);
        *g->f.Ptr(ptr + cur + len, 1) = 0;
        g->f.Put<uint16_t>(at + 8, (uint16_t)(cur + len));
    }
    static void GroupThunk(void* ctx, uint64_t mgr, uint64_t entry, uint32_t group) {
        FakeGame* g = static_cast<FakeGame*>(ctx);
        g->Note("RunPackGroup", entry, "step 6/6 RunPackGroup");
        (void)mgr;
        Group r;
        r.entry = entry; r.group = group;
        r.hashRead = g->Get<uint32_t>(entry + g->P.entryHash);
        r.stateRead = g->Get<uint32_t>(entry + g->P.entryState);
        g->groups.push_back(r);
    }
};

static void TestMounter(std::vector<uint8_t>& img) {
    const PackProfile& P = kPackEnhanced;
    const uint64_t vt = kImgVa + P.mgrVtableRva;
    const char* kFolderA = "C:/Users/x/AppData/Local/GAGE-MP/packs/jesko/";
    const char* kFolderB = "C:/Users/x/AppData/Local/GAGE-MP/packs/rs6/";
    const char* kRpf = "C:/Users/x/AppData/Local/GAGE-MP/packs/supra/";

    // 3a. the live shape: 103 packs in an array of 103 (full). Three packs: two loose folders, one dlc.rpf.
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(103, vt);
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a, b, c;
        const uint64_t arr0 = g.Arr();
        CHECK(m.Add("jesko", kFolderA, true, a) && a.ok, "pack 1 (folder) mounted: %s", a.why.c_str());
        CHECK(a.countBefore == 103 && a.countAfter == 104 && a.arrayMoved && g.Arr() != arr0 && g.Cap() == 119, "the full array grew: 103 -> 104, array moved, capacity 103 -> 119 (cap %u)", (unsigned)g.Cap());
        CHECK(a.entry == g.EntryAt(103) && a.index == 103, "the entry is the NEW array's last slot, not the stale one");
        CHECK(g.Get<uint32_t>(a.entry + P.entryType) == P.typeRelative && g.Get<uint32_t>(a.entry + P.entryState) == 2, "folder pack: relative device, state 2");
        const uint64_t pp = g.Get<uint64_t>(a.entry + P.entryPath);
        char got[128] = {};
        FakeProc::Read(&f, pp, got, 100);
        CHECK(!strcmp(got, kFolderA) && g.Get<uint16_t>(a.entry + P.entryPathLen) == strlen(kFolderA), "the entry's path reads back as the folder (\"%s\")", got);
        CHECK(a.hash == 0xC0DE0001u && g.mountPoints.size() == 1 && g.mountPoints[0] == "dlc_fake1:/", "Mount got the pack's own mount point \"%s\"", g.mountPoints.empty() ? "" : g.mountPoints[0].c_str());
        CHECK(m.MountPointOf(a.entry) == "dlc_fake1:/", "MountPointOf reads the atString at +0x40/+0x48");
        CHECK(m.RunChangeSets("jesko", a) && a.ok, "change sets ran");
        CHECK(g.groups.size() == 2 && g.groups[0].group == 0x6f8bd408u && g.groups[1].group == 0x5bf6578fu, "RunPackGroup: early_on then startup");
        CHECK(g.groups.size() == 2 && g.groups[0].entry == g.EntryAt(103) && g.groups[0].hashRead == a.hash && g.groups[0].stateRead == 2, "RunPackGroup got the live entry (hash 0x%08x, state %u)",
              g.groups.empty() ? 0u : g.groups[0].hashRead, g.groups.empty() ? 0u : g.groups[0].stateRead);
        // second pack: no growth this time; its entry is slot 104
        CHECK(m.Add("rs6", kFolderB, true, b) && b.ok && !b.arrayMoved && b.index == 104 && b.countBefore == 104 && b.countAfter == 105, "pack 2 (folder): 104 -> 105, no growth");
        CHECK(m.RunChangeSets("rs6", b) && g.groups.size() == 4 && g.groups[2].entry == g.EntryAt(104) && g.groups[2].hashRead == b.hash, "pack 2's change sets ran on its own entry");
        // third: a dlc.rpf pack. The path and the type are left alone
        CHECK(m.Add("supra", kRpf, false, c) && c.ok && g.Get<uint32_t>(c.entry + P.entryType) == P.typePackfile, "pack 3 (dlc.rpf): mounted, still a packfile entry");
        CHECK(g.CallsOf("append") == 2, "the game's append ran for the two folder packs only (%d)", g.CallsOf("append"));
        CHECK(g.CallsOf("AddDlcPack") == 3 && g.CallsOf("DlcSetup2") == 3 && g.CallsOf("Mount") == 3 && g.CallsOf("RunPackGroup") == 4, "call counts: Add 3, Setup2 3, Mount 3, groups 4");
        CHECK(g.AllLoggedFirst(), "every game call and every write was preceded by its step line in the log");
        CHECK(f.Has("[packenh] pack jesko: AddDlcPack returned 1; pack count 103 -> 104") && f.Has("(the game grew it; the entry is re-derived from the new array)"), "log: AddDlcPack result, count N -> N+1, growth");
        CHECK(f.Has("pack jesko: DlcSetup2 returned 1; state 1, nameHash 0xc0de0001") && f.Has("pack jesko: Mount returned 0 (0 = ok); state 1") && f.Has("pack jesko: mounted (entry #103"),
              "log: DlcSetup2 / Mount state lines");
        CHECK(f.Has("pack supra: step 2/6 folder override: skipped") && f.warns == 0, "log: the dlc.rpf pack skips the override; a healthy run warns about nothing (%d)", f.warns);
        if (const char* show = getenv("PACKENH_SHOW"); show && *show == '2') for (const std::string& l : f.lines) std::printf("  | %s\n", l.c_str());
    }
    // 3b. the array grows BETWEEN Add and RunChangeSets (a second pack was added in between): the entry is re-derived
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(103, vt);
        g.growBy = 1;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a, other;
        CHECK(m.Add("jesko", kFolderA, true, a) && a.ok, "pack mounted");
        const uint64_t stale = a.entry;
        CHECK(m.Add("rs6", kFolderB, true, other) && other.arrayMoved && g.EntryAt(103) != stale, "a later AddDlcPack moved the array (cap grow by 1)");
        CHECK(m.RunChangeSets("jesko", a) && g.groups.size() == 2 && g.groups[0].entry == g.EntryAt(103) && g.groups[0].entry != stale && g.groups[0].hashRead == a.hash,
              "RunPackGroup used the re-derived entry (0x%llx), not the stale one (0x%llx)", (unsigned long long)(g.groups.empty() ? 0 : g.groups[0].entry), (unsigned long long)stale);
        CHECK(f.Has("the entry moved (0x"), "the log says the entry moved");
    }
    // 3c. a path buffer too small for the folder: the game's append reallocates, our content lands in the game's new buffer
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(5, vt);
        g.pathCap = 8;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        const bool ok = m.Add("jesko", kFolderA, true, a);
        CHECK(ok, "mounted with a path buffer smaller than the folder: %s", a.why.c_str());
        const uint64_t pp = g.Get<uint64_t>(a.entry + P.entryPath);
        char got[128] = {};
        FakeProc::Read(&f, pp, got, 100);
        CHECK(!strcmp(got, kFolderA) && g.Get<uint16_t>(a.entry + P.entryPathLen + 2) >= strlen(kFolderA) + 1, "the entry's path is the folder, in a buffer the game's append made (cap %u)",
              (unsigned)g.Get<uint16_t>(a.entry + P.entryPathLen + 2));
    }
    // 3d. failures stop at their step: nothing after it is called, nothing is written before a failed read
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, kImgVa + 0x2665bb0);   // some other vtable
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 0 && g.calls.empty() && g.writes == 0, "a wrong manager vtable stops at the manager check: no call, no write");
        CHECK(f.Has("pack jesko FAILED at step 0/6 (manager check)"), "log names the failing step");
    }
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        g.addFails = true;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 1 && g.CallsOf("DlcSetup2") == 0 && g.writes == 0 && g.Cnt() == 10, "AddDlcPack refusing stops at step 1, nothing else runs");
        CHECK(f.Has("FAILED at step 1/6 (AddDlcPack)") && f.Has("did not resolve"), "log: step 1 and why");
    }
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        g.setup2Fails = true;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 3 && g.CallsOf("Mount") == 0 && g.CallsOf("RunPackGroup") == 0, "DlcSetup2 failing stops at step 3: no Mount, no groups");
        CHECK(g.Get<uint32_t>(a.entry + P.entryState) == 5 && f.Has("FAILED at step 3/6 (DlcSetup2)"), "the state stays what the game left (5); the log names step 3");
    }
    {   // DlcSetup2 says ok but leaves the name hash zero: the change set could not be keyed, so the pack stops at step 3 before Mount
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        g.setup2ZeroHash = true;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 3 && g.CallsOf("Mount") == 0 && g.CallsOf("RunPackGroup") == 0 && f.Has("nameHash is still zero"),
              "a zero name hash after DlcSetup2 stops at step 3: no Mount, no groups, the log says why");
        FakeProc f2(img);
        f2.heap.assign(0x400000, 0);
        FakeGame g2(f2);
        g2.Build(10, vt);
        Mounter m2(P, g2.Abi(), g2.mgr, vt);
        MountOut b;
        CHECK(m2.Add("jesko", kFolderA, true, b) && b.ok && b.hash != 0, "the same pack with a real name hash mounts (the zero-hash case above is not vacuous)");
    }
    {
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        g.mountFails = true;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        const int before = g.writes;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 4 && g.Get<uint32_t>(a.entry + P.entryState) == 1, "Mount failing stops at step 4 and does NOT write state 2");
        const int w = g.writes - before;
        CHECK(w == 2 && g.CallsOf("RunPackGroup") == 0, "only the folder override's two writes happened (%d), no groups ran", w);
    }
    {   // the layout is not what the dump says (an entry stride of 0xf0 reads garbage): refuse BEFORE the first write
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        PackProfile wrong = kPackEnhanced;
        wrong.entryStride = 0xf0;
        Mounter m(wrong, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 1 && g.writes == 0 && g.CallsOf("DlcSetup2") == 0, "a wrong entry stride is refused at step 1 with no write (writes %d)", g.writes);
        CHECK(f.Has("not a fresh packfile entry"), "log says the layout is wrong");
    }
    {   // a broken append: nothing is read back, the type is NOT switched to relative
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        g.brokenAppend = true;
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(!m.Add("jesko", kFolderA, true, a) && a.step == 2 && g.Get<uint32_t>(a.entry + P.entryType) == P.typePackfile && g.CallsOf("DlcSetup2") == 0,
              "an append that does not read back stops at step 2 and leaves the device type alone");
    }
    {   // the pack whose hash is not found any more (the entry vanished) cannot run its change sets
        FakeProc f(img);
        f.heap.assign(0x400000, 0);
        FakeGame g(f);
        g.Build(10, vt);
        Mounter m(P, g.Abi(), g.mgr, vt);
        MountOut a;
        CHECK(m.Add("jesko", kFolderA, true, a), "mounted");
        a.hash ^= 0xffff;   // someone else's hash
        CHECK(!m.RunChangeSets("jesko", a) && a.step == 6 && g.groups.empty() && f.Has("no entry with nameHash"), "a hash that is no longer in the manager runs no group");
    }
}
