// pack_profile_test: offline proofs for the per-build car-pack profile (apps/engine/pack_profile.h) and for Round A of the
// Enhanced car-pack port (apps/engine/pack_enh_core.h, the read-only pack_enh=1 diagnostic).
//
//  1. Legacy: the profile still holds, byte for byte, the 11 patterns pack_stream.cpp shipped with. Their FNV-1a-64 were
//     taken from `git show HEAD:apps/engine/pack_stream.cpp` BEFORE the move (tests cannot see a Legacy image: none is
//     checked in), so a transcription slip or a later edit fails here.
//  2. Enhanced: when the friend's decrypted dump exists (`enhanced EXE/dump and bin/GTA5_Enhanced.exe-95703552-959889125.img`,
//     file offset = RVA) every Enhanced pattern must hit EXACTLY ONCE at its expectRva, none of the 11 Legacy patterns may
//     hit at all, and the structure offsets must be visible in the code bytes. A missing dump SKIPS LOUDLY (and fails when
//     GAGE_REQUIRE_IMG=1); it never passes vacuously: the final line says how many checks ran and how many were skipped,
//     and the count itself is asserted.
//  3. The Round A diagnostic itself runs against the dump plus a synthetic game heap (same code the engine runs), 22 of 22,
//     and against mutants (bad pattern, wrong vtable, wrong stride, store never built, ...) that must fail.
//
// Path of the dump: argv[1], else $GAGE_ENH_IMG, else the repo-relative default searched from the working directory upwards.
#include "../apps/engine/pack_enh_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gage::game::content;
using gage::game::content::packenh::RoundA;

static int g_pass = 0, g_fail = 0, g_skip = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) { ++g_pass; }                                                                    \
        else { ++g_fail; std::printf("FAIL  %s:%d  %s  -- ", __FILE__, __LINE__, #cond);           \
               std::printf(__VA_ARGS__); std::printf("\n"); }                                      \
    } while (0)

static uint64_t Fnv1a64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 0x100000001b3ull; }
    return h;
}

static int Tokens(const char* s) {
    std::vector<uint8_t> b, m;
    return scan::ParsePat(s, b, m) ? (int)b.size() : -1;
}

// ---- 1. Legacy -------------------------------------------------------------------------------------------------
struct Golden { const char* name; int tokens; uint64_t fnv; };
static const Golden kLegacyGolden[11] = {
    { "AddDlcPack", 40, 0xb6fc32bfd2a48f2dULL },
    { "DlcSetup2", 24, 0x5587b95321bf97e2ULL },
    { "MountGate", 24, 0x4d4a8667fd7709d2ULL },
    { "RunPackGroup", 40, 0xc2a754d1c64a568bULL },
    { "Mount", 36, 0x2d5ab244e8b840abULL },
    { "PushFolder", 40, 0xc2339470769e208dULL },
    { "Exists", 39, 0xe95cc21a63034cf4ULL },
    { "PopFolder", 18, 0x15d1f92bcbb89b4fULL },
    { "RegStreamFile", 50, 0x1f2f8be62c72de49ULL },
    { "VehStoreInit", 60, 0x485246f8d0f68675ULL },
    { "GetDevice", 41, 0x1ec59506082e7f52ULL },
};

static const PackSig* LegacySigs[11] = {
    &kPackLegacy.addDlcPack, &kPackLegacy.dlcSetup2, &kPackLegacy.mountGate, &kPackLegacy.runPackGroup, &kPackLegacy.mount,
    &kPackLegacy.pushFolder, &kPackLegacy.exists, &kPackLegacy.popFolder, &kPackLegacy.regStream, &kPackLegacy.vehStore,
    &kPackLegacy.getDevice,
};

static void TestLegacy() {
    for (int i = 0; i < 11; ++i) {
        const PackSig& s = *LegacySigs[i];
        CHECK(s.name && !strcmp(s.name, kLegacyGolden[i].name), "legacy sig %d name '%s' != '%s'", i, s.name ? s.name : "(null)", kLegacyGolden[i].name);
        CHECK(s.pat != nullptr, "legacy %s has no pattern", kLegacyGolden[i].name);
        if (!s.pat) continue;
        CHECK(Tokens(s.pat) == kLegacyGolden[i].tokens, "legacy %s: %d tokens, golden %d", kLegacyGolden[i].name, Tokens(s.pat), kLegacyGolden[i].tokens);
        CHECK(Fnv1a64(s.pat) == kLegacyGolden[i].fnv, "legacy %s pattern text changed: fnv %016llx, golden %016llx", kLegacyGolden[i].name,
              (unsigned long long)Fnv1a64(s.pat), (unsigned long long)kLegacyGolden[i].fnv);
        CHECK(s.expectRva != 0, "legacy %s has no expectRva", kLegacyGolden[i].name);
        CHECK(s.source && *s.source, "legacy %s has no source", kLegacyGolden[i].name);
    }
    // the layout pack_stream.cpp shipped with
    const PackProfile& L = kPackLegacy;
    CHECK(L.mountable && !kPackEnhanced.mountable, "only Legacy may run the mount chain");
    CHECK(L.mgrVtableRva == 0x1a9c508u && L.mgrArray == 0x28 && L.mgrCount == 0x30 && L.mgrGen == 0x178, "legacy manager layout");
    CHECK(L.entryStride == 0xf0 && L.entryHash == 0x60 && L.entryPath == 0xb8 && L.entryPathLen == 0xc0 && L.entryType == 0xc8 && L.entryState == 0xd0,
          "legacy DlcEntry layout");
    CHECK(L.typePackfile == 2 && L.typeRelative == 1, "legacy device types");
    CHECK(L.assetGlobalRva == 0x1e2ee90u && L.streamInfoMgrRva == 0x2ea23d0u && L.imageRegistryRva == 0x2c67400u && L.rawStreamerVtRva == 0x1b7d5e0u,
          "legacy single RVAs");
    CHECK(L.vsCfg.dispAt == 3 && L.vsCfg.endAt == 7 && L.vsTbl.dispAt == 19 && L.vsTbl.endAt == 23 && L.vsLimitAt == 52 && L.vsSlotAt == 59 &&
          L.vsReadLen == 60 && L.vsLimitOffExpect == 0x324 && L.vsSlotExpect == 0x28, "legacy vehicle-store operand positions");
    CHECK((int)L.vsReadLen == Tokens(L.vehStore.pat), "legacy vsReadLen == pattern length");
    CHECK(L.mgrAnchor.pat == nullptr && L.mgrCtor.pat == nullptr && L.atStr.pat == nullptr && L.imageReg.pat == nullptr,
          "Legacy has no Enhanced-only patterns (its equivalents are read out of RunPackGroup / AddDlcPack)");

    // profile selection by exe name (BuildProfile's rule)
    CHECK(&SelectPackProfile(L"GTA5.exe") == &kPackLegacy, "GTA5.exe -> Legacy");
    CHECK(&SelectPackProfile(L"gta5.EXE") == &kPackLegacy, "case-insensitive");
    CHECK(&SelectPackProfile(L"GTA5_Enhanced.exe") == &kPackEnhanced, "GTA5_Enhanced.exe -> Enhanced");
    CHECK(&SelectPackProfile(L"gta5_enhanced.exe") == &kPackEnhanced, "lower case -> Enhanced");
    CHECK(&SelectPackProfile(L"gage_mp_test.exe") == &kPackLegacy, "a test harness exe keeps the Legacy default");
    CHECK(&SelectPackProfile(nullptr) == &kPackLegacy, "null name -> Legacy");

    // every Enhanced pattern parses and carries enough fixed bytes to be a real anchor
    const PackSig* E[8] = { &kPackEnhanced.addDlcPack, &kPackEnhanced.dlcSetup2, &kPackEnhanced.mount, &kPackEnhanced.mgrAnchor,
                            &kPackEnhanced.mgrCtor, &kPackEnhanced.atStr, &kPackEnhanced.imageReg, &kPackEnhanced.vehStore };
    for (int i = 0; i < 8; ++i) {
        std::vector<uint8_t> b, m;
        CHECK(E[i]->pat && scan::ParsePat(E[i]->pat, b, m), "enhanced sig %d parses", i);
        if (!E[i]->pat) continue;
        int fixed = 0;
        for (uint8_t x : m) fixed += x;
        CHECK(fixed >= 8, "enhanced %s has only %d fixed bytes", E[i]->name, fixed);   // ManagerAnchor is YimMenu's short anchor; uniqueness is asserted against the dump
        CHECK(E[i]->expectRva != 0, "enhanced %s has no expectRva", E[i]->name);
    }
    CHECK((int)kPackEnhanced.vsReadLen == Tokens(kPackEnhanced.vehStore.pat), "enhanced vsReadLen == pattern length");
}

// ---- image loading -----------------------------------------------------------------------------------------------
static const char* kRelImg = "enhanced EXE/dump and bin/GTA5_Enhanced.exe-95703552-959889125.img";
static const size_t kImgSize = 95703552;   // SizeOfImage 0x5b45200
static const uint64_t kImgVa = 0x7ff628ec0000ull;   // the module base of the process the dump came from (pointers in it use it)

static bool LoadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

static bool FindImage(int argc, char** argv, std::vector<uint8_t>& out, std::string& where) {
    std::vector<std::string> cand;
    if (argc > 1) cand.push_back(argv[1]);
    if (const char* e = getenv("GAGE_ENH_IMG")) cand.push_back(e);
    std::string pre;
    for (int up = 0; up < 6; ++up) { cand.push_back(pre + kRelImg); pre += "../"; }
    for (const std::string& c : cand)
        if (LoadFile(c, out)) { where = c; return true; }
    return false;
}

// ---- 2. Enhanced patterns against the dump ---------------------------------------------------------------------
static bool CodeHas(const std::vector<uint8_t>& img, uint64_t rva, size_t len, const char* pat) {
    std::vector<scan::Run> r = { { img.data() + rva, len } };
    return scan::CountPat(r, pat, nullptr) >= 1;
}

static void TestEnhancedPatterns(const std::vector<uint8_t>& img) {
    CHECK(img.size() == kImgSize, "image is %zu bytes, expected %zu (the dump the profile was derived from)", img.size(), kImgSize);
    const std::vector<scan::Run> runs = { { img.data(), img.size() } };

    // each Enhanced pattern: exactly one hit, at the expected RVA
    const PackSig* E[8] = { &kPackEnhanced.addDlcPack, &kPackEnhanced.dlcSetup2, &kPackEnhanced.mount, &kPackEnhanced.mgrAnchor,
                            &kPackEnhanced.mgrCtor, &kPackEnhanced.atStr, &kPackEnhanced.imageReg, &kPackEnhanced.vehStore };
    for (int i = 0; i < 8; ++i) {
        const uint8_t* first = nullptr;
        const int c = scan::CountPat(runs, E[i]->pat, &first);
        CHECK(c == 1, "enhanced %s: %d hits (need exactly 1)", E[i]->name, c);
        CHECK(c == 1 && (uint64_t)(first - img.data()) == E[i]->expectRva, "enhanced %s at +0x%llx, expected +0x%llx", E[i]->name,
              (unsigned long long)(first ? first - img.data() : 0), (unsigned long long)E[i]->expectRva);
    }

    // negative control: the Legacy patterns do not land in the Enhanced image (different code), with ONE real exception:
    // fiAssetManager::PopFolder is the same machine code in both builds. It sits at +0x102ab0 and DlcSetup2 calls it
    // exactly as Legacy's does (after PushFolder +0x102720 / Exists +0x101e00 on the ASSET global +0x2bc3fe0).
    for (int i = 0; i < 11; ++i) {
        const uint8_t* first = nullptr;
        const int c = scan::CountPat(runs, LegacySigs[i]->pat, &first);
        if (LegacySigs[i] == &kPackLegacy.popFolder) {
            CHECK(c == 1 && first && (uint64_t)(first - img.data()) == 0x102ab0, "legacy PopFolder: %d hit(s), expected exactly one at +0x102ab0", c);
        } else {
            CHECK(c == 0, "legacy %s hits the Enhanced image %d time(s)", LegacySigs[i]->name, c);
        }
    }
    {
        int32_t rel = 0;
        memcpy(&rel, img.data() + 0xc2a925, 4);
        CHECK(img[0xc2a924] == 0xE8 && 0xc2a929 + (int64_t)rel == 0x102ab0, "DlcSetup2 calls the PopFolder analog at +0x102ab0");
        int32_t d = 0;
        memcpy(&d, img.data() + 0xc2a91d + 3, 4);
        CHECK(0xc2a924 + (int64_t)d == 0x2bc3fe0, "...with the ASSET global +0x2bc3fe0 in rcx");
    }

    // the call AddDlcPack makes to atString::Append: both sites land on the pattern's function (0xaa02e0)
    {
        std::vector<scan::Run> body = { { img.data() + 0xc29fc0, 0x240 } };
        const uint8_t* hit = nullptr;
        const int n = scan::CountPat(body, kPackEnhanced.atStrCall.pat, &hit);
        CHECK(n == 2, "AddDlcPack has %d atString::Append call sites, expected 2", n);
        int32_t rel = 0;
        if (hit) memcpy(&rel, hit + kPackEnhanced.atStrCallTarget.dispAt, 4);
        CHECK(hit && (uint64_t)((hit - img.data()) + kPackEnhanced.atStrCallTarget.endAt + rel) == 0xaa02e0, "call target");
    }

    // the structure offsets, visible in the code bytes (entry built at rsp+0x120 in AddDlcPack)
    const uint64_t add = 0xc29fc0, set2 = 0xc2a890, mnt = 0xc2c290;
    CHECK(CodeHas(img, add, 0x240, "48 8D 0C C0 48 C1 E1 05"), "stride: lea rcx,[rax+rax*8]; shl rcx,5 (= 9*32 = 0x120)");
    CHECK(CodeHas(img, add, 0x240, "48 8D B4 24 20 01 00 00"), "entry base = rsp+0x120");
    CHECK(CodeHas(img, add, 0x240, "89 84 24 A0 01 00 00"), "hash at [rsp+0x1a0] = entry+0x80");
    CHECK(CodeHas(img, add, 0x240, "48 8B BC 24 F8 01 00 00"), "path pointer at [rsp+0x1f8] = entry+0xd8");
    CHECK(CodeHas(img, add, 0x240, "66 C7 84 24 00 02 00 00 00 00"), "path length WORD at [rsp+0x200] = entry+0xe0");
    CHECK(CodeHas(img, add, 0x240, "C7 84 24 18 02 00 00 02 00 00 00"), "type = 2 at [rsp+0x218] = entry+0xf8");
    CHECK(CodeHas(img, add, 0x240, "41 0F B7 46 30"), "manager count WORD at +0x30");
    CHECK(CodeHas(img, add, 0x240, "66 45 3B 46 32"), "manager capacity WORD at +0x32");
    CHECK(CodeHas(img, add, 0x240, "49 03 4E 28"), "manager array at +0x28");
    CHECK(CodeHas(img, set2, 0x40, "8B 89 00 01 00 00"), "DlcSetup2 reads the state at entry+0x100");
    CHECK(CodeHas(img, set2, 0x40, "83 FA 02 0F 82"), "states 1..3 are 'already set up'");
    CHECK(CodeHas(img, mnt, 0x40, "8B 81 F8 00 00 00 83 F8 02"), "Mount reads the type at entry+0xf8 and compares with 2");
    CHECK(CodeHas(img, mnt, 0x100, "66 83 BE E0 00 00 00 00"), "Mount tests the path length WORD at entry+0xe0");
    CHECK(CodeHas(img, mnt, 0x100, "48 8B 96 D8 00 00 00"), "Mount loads the path pointer at entry+0xd8");
    // the loop that hands DlcSetup2 the manager's generation: edx = [mgr+0x178], then call DlcSetup2
    {
        uint32_t rel = 0;
        const uint8_t* p = img.data() + 0xc2889f;
        const bool shape = p[0] == 0x8B && p[1] == 0x96 && p[2] == 0x78 && p[3] == 0x01 && p[4] == 0x00 && p[5] == 0x00 && p[9] == 0xE8;
        memcpy(&rel, p + 10, 4);
        CHECK(shape && 0xc2889f + 14 + (int32_t)rel == set2, "mgr+0x178 is the edx passed to DlcSetup2 (call at +0xc288a8)");
    }
    // the ctor stores the 0x9b8-byte manager under the vtable the profile names
    {
        const uint8_t* m = img.data() + 0xc255d8;
        int32_t d = 0;
        memcpy(&d, m + 3, 4);
        CHECK((uint64_t)(0xc255d8 + 7 + d) == kPackEnhanced.mgrVtableRva, "ctor vtable +0x%llx vs profile +0x%x", (unsigned long long)(0xc255d8 + 7 + d),
              (unsigned)kPackEnhanced.mgrVtableRva);
    }
    // the RawStreamer's vtable and its +0x1e0 slot (a code pointer inside the image) exist in the dump
    {
        uint64_t slot = 0;
        memcpy(&slot, img.data() + kPackEnhanced.rawStreamerVtRva + kPackEnhanced.imageRegSlotExpect, 8);
        CHECK(slot > kImgVa && slot < kImgVa + kImgSize, "RawStreamer vtable slot +0x%x = 0x%llx", (unsigned)kPackEnhanced.imageRegSlotExpect,
              (unsigned long long)slot);
        uint32_t cap = 0;
        memcpy(&cap, img.data() + 0x24889b0, 4);
        CHECK(cap == 0x1c58, "image registry bound in the dump = 0x%x", (unsigned)cap);
    }
}

// ---- 3. Round A against a fake process ---------------------------------------------------------------------------
struct FakeProc {
    std::vector<uint8_t>& img;
    uint64_t heapBase = 0x0000027e50000000ull;
    std::vector<uint8_t> heap = std::vector<uint8_t>(0x40000, 0);
    uint64_t bump = 0x100;
    std::vector<std::string> lines;
    int warns = 0;
    uint64_t now = 0;   // the fake clock Drive advances (kept across calls)

    explicit FakeProc(std::vector<uint8_t>& i) : img(i) {}
    uint64_t Alloc(size_t n) { const uint64_t va = heapBase + bump; bump += (n + 15) & ~size_t(15); return va; }
    uint8_t* Ptr(uint64_t va, size_t n) {
        if (va >= kImgVa && va + n <= kImgVa + img.size()) return img.data() + (va - kImgVa);
        if (va >= heapBase && va + n <= heapBase + heap.size()) return heap.data() + (va - heapBase);
        return nullptr;
    }
    template <class T> void Put(uint64_t va, T v) { uint8_t* p = Ptr(va, sizeof(T)); if (p) memcpy(p, &v, sizeof(T)); else std::printf("FakeProc: Put outside memory 0x%llx\n", (unsigned long long)va); }
    void PutImg(uint64_t rva, uint64_t v) { Put<uint64_t>(kImgVa + rva, v); }
    static bool Read(void* ctx, uint64_t va, void* out, size_t n) {
        FakeProc* f = static_cast<FakeProc*>(ctx);
        const uint8_t* p = f->Ptr(va, n);
        if (!p) return false;
        memcpy(out, p, n);
        return true;
    }
    static void Log(void* ctx, bool warn, const char* line) {
        FakeProc* f = static_cast<FakeProc*>(ctx);
        f->lines.push_back(line);
        f->warns += warn;
    }
    bool Has(const char* needle) const {
        for (const std::string& l : lines) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    packenh::Env MakeEnv() {
        packenh::Env e;
        e.scanBase = img.data();
        e.vaBase = kImgVa;
        e.size = img.size();
        e.read = &FakeProc::Read;
        e.log = &FakeProc::Log;
        e.ctx = this;
        return e;
    }
};

struct Scenario {
    uint32_t packs = 5;
    bool manager = true;          // the singleton cell holds a manager
    uint64_t vtable = 0;          // 0 = the ctor's (image+mgrVtableRva)
    uint32_t stride = 0x120;      // the stride the synthetic array is laid out with
    bool registry = true;
    bool store = true;
    uint32_t storeUsed = 120, storeCap = 1000;
};

// Lay the game's objects out the way the Enhanced offsets say they are.
static void Build(FakeProc& f, const Scenario& s) {
    const PackProfile& P = kPackEnhanced;
    if (s.manager) {
        const uint64_t mgr = f.Alloc(0x9b8);
        const uint64_t arr = f.Alloc((size_t)s.packs * s.stride + 16);
        const uint64_t str = f.Alloc(64);
        const char* path = "dlcpacks:/mpbeach/";
        memcpy(f.Ptr(str, 32), path, strlen(path) + 1);
        f.Put<uint64_t>(mgr, s.vtable ? s.vtable : kImgVa + P.mgrVtableRva);
        f.Put<uint64_t>(mgr + P.mgrArray, arr);
        f.Put<uint16_t>(mgr + P.mgrCount, (uint16_t)s.packs);
        f.Put<uint16_t>(mgr + P.mgrCap, (uint16_t)(s.packs + 3));
        f.Put<uint32_t>(mgr + P.mgrGen, 7);
        for (uint32_t i = 0; i < s.packs; ++i) {
            const uint64_t e = arr + (uint64_t)i * s.stride;
            f.Put<uint32_t>(e + P.entryHash, 0x1000u + i * 0x111u);
            f.Put<uint64_t>(e + P.entryPath, str);
            f.Put<uint16_t>(e + P.entryPathLen, (uint16_t)strlen(path));
            f.Put<uint16_t>(e + P.entryPathLen + 2, 64);
            f.Put<uint32_t>(e + P.entryType, i % 2 ? P.typeRelative : P.typePackfile);
            f.Put<uint32_t>(e + P.entryState, 3);
        }
        f.PutImg(0x29cafc8, mgr);
    } else {
        f.PutImg(0x29cafc8, 0);
    }
    if (s.registry) {
        const uint64_t obj = f.Alloc(0x7b8);
        f.Put<uint64_t>(obj, kImgVa + P.rawStreamerVtRva);
        f.PutImg(0x2c075b0, obj);
    } else {
        f.PutImg(0x2c075b0, 0);
    }
    // gameconfig -> limits block; factory table -> factory -> buckets -> node(key 0xf000) -> header
    const uint64_t lim = f.Alloc(0x400), cfg = f.Alloc(0x40);
    f.Put<uint64_t>(cfg + 0x18, lim);
    f.Put<uint32_t>(lim + 0x324, 700);
    f.PutImg(0x2e96fb0, cfg);
    const uint64_t table = f.Alloc(0x80), fac = f.Alloc(0x40), buckets = f.Alloc(8 * 7), node = f.Alloc(0x20), hdr = f.Alloc(0x10);
    f.Put<uint64_t>(table + 0x28, fac);
    f.Put<uint64_t>(fac + 8, buckets);
    f.Put<uint16_t>(fac + 0x10, 7);
    f.Put<uint32_t>(node, 0xf000u);
    f.Put<uint64_t>(node + 8, hdr);
    f.Put<uint64_t>(node + 0x10, 0);
    f.Put<uint64_t>(hdr, f.Alloc(16));
    f.Put<uint16_t>(hdr + 8, (uint16_t)s.storeUsed);
    f.Put<uint16_t>(hdr + 0xa, (uint16_t)s.storeCap);
    if (s.store) f.Put<uint64_t>(buckets + (0xf000u % 7) * 8, node);
    f.PutImg(0x29c87b8, table);
}

// Poll until Done() or maxSteps; steps are stepMs apart. giveUpAfter: pass giveUp=true from that time on.
static bool Drive(RoundA& r, FakeProc& f, int maxSteps, uint64_t stepMs, uint64_t giveUpAfterMs = ~0ull) {
    uint64_t& now = f.now;
    for (int i = 0; i < maxSteps && !r.Done(); ++i) {
        now += stepMs;
        const bool giveUp = now >= giveUpAfterMs;
        std::vector<scan::Run> runs;
        const std::vector<scan::Run>* rp = nullptr;
        if (r.NeedsScan(now) || (giveUp && !r.PatternsDone())) { runs.push_back({ f.img.data(), f.img.size() }); rp = &runs; }
        r.Step(now, rp, i % 10 == 0, giveUp);
    }
    return r.Done();
}

static void TestRoundA(std::vector<uint8_t>& img) {
    // 3a. healthy game: 22 of 22
    {
        FakeProc f(img);
        Build(f, Scenario());
        RoundA r(kPackEnhanced, f.MakeEnv());
        CHECK(Drive(r, f, 40, 1000), "round A finished");
        CHECK(r.Total() == 22, "total checks = %d", r.Total());
        CHECK(r.Ok() == 22, "ok %d of 22", r.Ok());
        for (int i = 0; i < packenh::kCheckCount; ++i) CHECK(r.CheckOk(i), "check %s", packenh::CheckName(i));
        CHECK(f.Has("[packenh] round A: 22 of 22 checks ok"), "summary line");
        if (const char* show = getenv("PACKENH_SHOW"); show && *show == '1')
            for (const std::string& l : f.lines) std::printf("  | %s\n", l.c_str());
        CHECK(f.warns == 0, "%d warning line(s) in a healthy run", f.warns);
        CHECK(f.Has("entry #0 hash=0x00001000 type=2 state=3") && f.Has("entry #4 hash=0x00001444 type=2 state=3"), "per-entry lines");
        CHECK(f.Has("entry #0 path ptr") && f.Has("\"dlcpacks:/mpbeach/\""), "first path string");
        CHECK(f.Has("5 pack(s)") && f.Has("capacity 8"), "pack count line");
        CHECK(f.Has("registry[0] = ") && f.Has("registry bound (+0x24889b0) = 7256"), "registry lines");
        CHECK(f.Has("entry 0 vtable") && f.Has("the RawStreamer's, as in the dump"), "entry 0 vtable line");
        CHECK(f.Has("120 of 1000 used") && f.Has("MaxExtraVehicleModelInfos 700"), "store n of cap line");
        CHECK(f.Has("ManagerGlobal") && f.Has("+0x29cafc8"), "manager global line");
        // the operands derived from the patterns are exactly the ones the dump's code uses
        CHECK(r.MgrGlobalRva() == 0x29cafc8 && r.CtorVtableRva() == 0x262c470, "manager global +0x%llx vtable +0x%llx", (unsigned long long)r.MgrGlobalRva(),
              (unsigned long long)r.CtorVtableRva());
        CHECK(r.RegistryRva() == 0x2c075b0 && r.RegistryCapRva() == 0x24889b0 && r.RegistrySlot() == 0x1e0, "registry +0x%llx bound +0x%llx slot 0x%x",
              (unsigned long long)r.RegistryRva(), (unsigned long long)r.RegistryCapRva(), (unsigned)r.RegistrySlot());
        CHECK(r.GameConfigRva() == 0x2e96fb0 && r.FactoryTableRva() == 0x29c87b8 && r.StoreLimitOffset() == 0x324 && r.StoreFactorySlot() == 0x28,
              "store: cfg +0x%llx table +0x%llx limit 0x%x slot 0x%x", (unsigned long long)r.GameConfigRva(), (unsigned long long)r.FactoryTableRva(),
              (unsigned)r.StoreLimitOffset(), (unsigned)r.StoreFactorySlot());
        // the eight patterns resolved where the dump says
        for (int i = 0; i < 8; ++i) {
            const PackSig* d[8] = { &kPackEnhanced.addDlcPack, &kPackEnhanced.dlcSetup2, &kPackEnhanced.mount, &kPackEnhanced.mgrAnchor,
                                    &kPackEnhanced.mgrCtor, &kPackEnhanced.atStr, &kPackEnhanced.imageReg, &kPackEnhanced.vehStore };
            CHECK(r.SigRva(i) == d[i]->expectRva, "sig %d resolved at +0x%llx", i, (unsigned long long)r.SigRva(i));
        }
    }
    // 3b. the game is slow: the singleton is null for a while, then built; the run waits and then passes
    {
        FakeProc f(img);
        Scenario s; s.manager = false; s.store = false; s.registry = false;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        for (int i = 0; i < 12; ++i) { Drive(r, f, 1, 1000); }
        CHECK(!r.Done(), "still waiting while the game has built nothing");
        CHECK(r.PatternsDone() && r.Ok() == 12, "patterns + cross-checks done, live state pending: ok %d", r.Ok());
        Build(f, Scenario());   // the game finishes booting
        CHECK(Drive(r, f, 40, 1000), "finished once the game built its objects");
        CHECK(r.Ok() == 22, "ok %d of 22 after the late build", r.Ok());
    }
    // 3c. the store is never created (no session): everything else passes, the store fails, the run ends at the budget
    {
        FakeProc f(img);
        Scenario s; s.store = false;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        CHECK(Drive(r, f, 100, 5000), "finished at the budget");
        CHECK(r.Ok() == 21 && !r.CheckOk(packenh::kStore), "21 of 22, the store missing: ok %d", r.Ok());
        CHECK(f.Has("[packenh] round A: 21 of 22 checks ok") && f.Has("failed checks: VehicleStore"), "summary names the failed check");
    }
    // 3d. mutants of the game state: each must fail exactly the check that binds it
    {
        FakeProc f(img);
        Scenario s; s.vtable = kImgVa + 0x2665bb0;   // some other image vtable
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 40, 1000);
        CHECK(r.Done() && !r.CheckOk(packenh::kMgrVtable) && r.Ok() == 21, "wrong manager vtable fails ManagerVtable only (ok %d)", r.Ok());
    }
    {
        FakeProc f(img);
        Scenario s; s.vtable = 0x1234000;   // not in the image at all
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 40, 1000);
        CHECK(r.Done() && !r.CheckOk(packenh::kMgrVtable), "a vtable outside the image fails");
    }
    {
        // the synthetic array is laid out with the Legacy stride; reading it at 0x120 walks into garbage
        FakeProc f(img);
        Scenario s; s.stride = 0xf0;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 40, 1000);
        CHECK(r.Done() && !r.CheckOk(packenh::kEntries), "a wrong stride fails Entries (ok %d)", r.Ok());
        CHECK(f.Has("implausible") || f.Has("UNREADABLE"), "the log says which entry is bad");
    }
    {
        FakeProc f(img);
        Scenario s; s.packs = 0;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 100, 5000);
        CHECK(r.Done() && !r.CheckOk(packenh::kPackCount) && !r.CheckOk(packenh::kEntries), "no packs: PackCount and Entries fail (ok %d)", r.Ok());
    }
    {
        FakeProc f(img);
        Scenario s; s.registry = false;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 100, 5000);
        CHECK(r.Done() && !r.CheckOk(packenh::kRegEntry0) && r.Ok() == 18, "registry[0] never set: its 4 checks fail (ok %d)", r.Ok());
    }
    {
        // a store over its capacity is a failure, not a pass
        FakeProc f(img);
        Scenario s; s.storeUsed = 1001; s.storeCap = 1000;
        Build(f, s);
        RoundA r(kPackEnhanced, f.MakeEnv());
        Drive(r, f, 40, 1000);
        CHECK(r.Done() && !r.CheckOk(packenh::kStore), "n > cap fails VehicleStore");
    }
    // 3e. pattern mutants: a profile copy with one byte broken
    {
        PackProfile bad = kPackEnhanced;
        static char pat[512];
        snprintf(pat, sizeof(pat), "%s", kPackEnhanced.dlcSetup2.pat);
        pat[0] = '5'; pat[1] = '7';   // 56 -> 57
        bad.dlcSetup2.pat = pat;
        FakeProc f(img);
        Build(f, Scenario());
        RoundA r(bad, f.MakeEnv());
        // the code never "decrypts": scan until the pattern budget runs out
        CHECK(Drive(r, f, 200, 1000, 120000), "finished after the pattern budget");
        CHECK(!r.CheckOk(packenh::kDlcSetup2) && r.SigHits(packenh::kDlcSetup2) == 0, "broken DlcSetup2 pattern: %d hits", r.SigHits(packenh::kDlcSetup2));
        CHECK(r.CheckOk(packenh::kAddDlcPack) && r.Ok() == 21, "only DlcSetup2 fails (ok %d)", r.Ok());
        CHECK(f.Has("missing after the wait budget"), "log says why");
    }
    {
        PackProfile bad = kPackEnhanced;
        bad.mgrAnchor.pat = "48 8B";   // ambiguous
        FakeProc f(img);
        Build(f, Scenario());
        RoundA r(bad, f.MakeEnv());
        Drive(r, f, 100, 1000, 60000);
        CHECK(!r.CheckOk(packenh::kMgrAnchor) && r.SigHits(packenh::kMgrAnchor) > 1, "ambiguous pattern is refused (%d hits)", r.SigHits(packenh::kMgrAnchor));
        CHECK(!r.CheckOk(packenh::kMgrGlobal), "and the global no longer has two sources");
    }
    {
        PackProfile bad = kPackEnhanced;
        bad.atStr.pat = "ZZ 11";   // malformed
        FakeProc f(img);
        Build(f, Scenario());
        RoundA r(bad, f.MakeEnv());
        Drive(r, f, 100, 1000, 60000);
        CHECK(!r.CheckOk(packenh::kAtStr) && !r.CheckOk(packenh::kAtStrCall), "a malformed pattern fails (and the call-site check with it)");
    }
    {
        // a wrong operand position in the profile (here the registry's bound) is caught by the live read, not accepted
        PackProfile bad = kPackEnhanced;
        bad.imageRegCap = { 6, 10 };
        FakeProc f(img);
        Build(f, Scenario());
        RoundA r(bad, f.MakeEnv());
        Drive(r, f, 100, 1000, 60000);
        CHECK(r.Done() && r.RegistryCapRva() != 0x24889b0, "a shifted operand position derives a different bound RVA (+0x%llx), which the exact-value check above would catch", (unsigned long long)r.RegistryCapRva());
    }
    // 3f. nothing decrypts, nothing resolves: every check fails, no vacuous pass
    {
        std::vector<uint8_t> blank(img.size() / 8, 0);
        FakeProc f(blank);
        packenh::Env e = f.MakeEnv();
        RoundA r(kPackEnhanced, e);
        CHECK(Drive(r, f, 200, 1000, 120000), "gives up");
        CHECK(r.Ok() == 0, "an image of zeroes passes %d checks", r.Ok());
        CHECK(f.Has("[packenh] round A: 0 of 22 checks ok"), "summary says 0 of 22");
    }
}

#include "pack_enh_mount_tests.h"
#include "../apps/engine/veh_fields.h"

// 0.4.30 veh_steer_field: the Enhanced profile resolves on the dump to the offsets checked by hand (steering 0x9DC, handle->pointer RVA
// 0x1494b0); the Legacy profile finds nothing there (no false positive across editions). 4 checks, image section.
static void TestVehFields(const std::vector<uint8_t>& img) {
    namespace vf = gage::game::vehfields;
    const vf::Result e = vf::Resolve({ img.data(), img.size() }, vf::kEnhanced);
    CHECK(e.ok, "Enhanced steering field did not resolve: %s", e.why);
    CHECK(e.steerOff == 0x9DC, "steering offset 0x%x, expected 0x9DC", (unsigned)e.steerOff);
    CHECK(e.htp && (uint64_t)(e.htp - img.data()) == 0x1494b0ull, "handle->pointer at RVA 0x%llx, expected 0x1494b0",
          (unsigned long long)(e.htp ? e.htp - img.data() : 0));
    const vf::Result l = vf::Resolve({ img.data(), img.size() }, vf::kLegacy);
    CHECK(!l.ok, "the Legacy profile resolved on the Enhanced dump (%s)", l.why);
    std::printf("veh fields: %s | legacy on this dump: %s\n", e.why, l.why);
}

int main(int argc, char** argv) {
    std::printf("pack_profile_test\n");
    TestLegacy();
    const int legacyChecks = g_pass + g_fail;
    TestMountProfile();
    TestLegacyResourceGuard();
    const int profileChecks = g_pass + g_fail - legacyChecks;

    std::vector<uint8_t> img;
    std::string where;
    const bool have = FindImage(argc, argv, img, where);
    int imageChecks = 0;
    if (have) {
        std::printf("Enhanced dump: %s (%zu bytes)\n", where.c_str(), img.size());
        const int before = g_pass + g_fail;
        TestEnhancedPatterns(img);
        TestRoundA(img);
        TestMountPatterns(img);   // round B (pack_enh=2): the mount patterns, the Extras resolver
        TestMounter(img);         // round B: the Mounter against a fake game
        TestVehFields(img);       // 0.4.30 veh_steer_field: the steering field and handle->pointer on Enhanced
        imageChecks = g_pass + g_fail - before;
    } else {
        ++g_skip;
        std::printf("\n"
                    "  ********************************************************************************\n"
                    "  *  SKIPPED: the Enhanced image dump was not found.                              *\n"
                    "  *  The Enhanced pattern checks and the Round A simulation DID NOT RUN.          *\n"
                    "  *  Looked for: argv[1], $GAGE_ENH_IMG, then upwards from the working dir:       *\n"
                    "  *  %s\n"
                    "  ********************************************************************************\n\n", kRelImg);
    }

    // the count itself is asserted: a section that silently stopped running cannot pass
    const int kExpectLegacy = 105, kExpectProfile = 33, kExpectImage = 208;   // profile: 28 + 5 Legacy-asset guard (0.4.27)
    std::printf("legacy-profile checks run: %d (expected %d)\n", legacyChecks, kExpectLegacy);
    if (legacyChecks != kExpectLegacy) { ++g_fail; std::printf("FAIL  legacy check count changed (%d != %d): update the test deliberately\n", legacyChecks, kExpectLegacy); }
    std::printf("round-B profile checks run: %d (expected %d)\n", profileChecks, kExpectProfile);
    if (profileChecks != kExpectProfile) { ++g_fail; std::printf("FAIL  round-B profile check count changed (%d != %d): update the test deliberately\n", profileChecks, kExpectProfile); }
    if (have) {
        std::printf("image checks run: %d (expected %d)\n", imageChecks, kExpectImage);
        if (imageChecks != kExpectImage) { ++g_fail; std::printf("FAIL  image check count changed (%d != %d): update the test deliberately\n", imageChecks, kExpectImage); }
    }

    std::printf("pack_profile_test: %d passed, %d failed, %d section(s) SKIPPED\n", g_pass, g_fail, g_skip);
    if (g_skip) {
        const char* req = getenv("GAGE_REQUIRE_IMG");
        if (req && *req == '1') { std::printf("GAGE_REQUIRE_IMG=1: a skipped image section is a failure\n"); return 1; }
        std::printf("RESULT: INCOMPLETE (exit 0 only because GAGE_REQUIRE_IMG is not set)\n");
    }
    return g_fail ? 1 : 0;
}
