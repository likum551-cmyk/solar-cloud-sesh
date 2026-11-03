// pack_collection_test: offline proofs for apps/engine/pack_collection.{h,cpp}.
//
// The entry bit-packing is pinned to GROUND TRUTH: the expected q0/q1 below are the exact 64-bit words
// read live out of GTA V 1.0.3889's own mounted packfiles on 2026-11-03 (the [entdiag] dump), including
// real vehicle-fragment entries. The builder must reproduce them from the decoded field inputs; if the
// format drifts, these fail. No game, no GPU, pure host code.
#include "../apps/engine/pack_collection.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gage::game::content;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) { ++g_pass; }                                                                    \
        else { ++g_fail; std::printf("FAIL  %s:%d  %s  -- ", __FILE__, __LINE__, #cond);           \
               std::printf(__VA_ARGS__); std::printf("\n"); }                                      \
    } while (0)

static void AppendLE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x & 0xff)); v.push_back(uint8_t((x >> 8) & 0xff));
    v.push_back(uint8_t((x >> 16) & 0xff)); v.push_back(uint8_t((x >> 24) & 0xff));
}

// ---- 1. golden vectors: encode must match the words observed in the live game ------------------
static void TestGoldenVectors() {
    // image[2], resource (.yft/.ytd) entries:  MakeResourceEntry(nameOff, onDisk, sector, sysFlags, gfxFlags)
    {
        TocEntry e = MakeResourceEntry(0x2a08, 16567, 0x06fa6, 0xa0100000u, 0x20000000u);
        CHECK(e.q0 == 0x806fa60040b72a08ULL, "res248 q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x20000000a0100000ULL, "res248 q1=%016llx", (unsigned long long)e.q1);
    }
    {
        TocEntry e = MakeResourceEntry(0x2a16, 22563, 0x06fc7, 0xa01a0000u, 0x20000000u);
        CHECK(e.q0 == 0x806fc70058232a16ULL, "res249 q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x20000000a01a0000ULL, "res249 q1=%016llx", (unsigned long long)e.q1);
    }
    // image[1], binary (compressed) entries:  MakeBinaryEntry(nameOff, onDisk, sector, uncompressed)
    {
        TocEntry e = MakeBinaryEntry(0x0646, 193, 0x01db4, 0x47b);
        CHECK(e.q0 == 0x001db40000c10646ULL, "bin248 q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x0fefffff0000047bULL, "bin248 q1=%016llx", (unsigned long long)e.q1);
    }
    {
        TocEntry e = MakeBinaryEntry(0x0676, 45721, 0x01db8, 0x1b7fef);
        CHECK(e.q0 == 0x001db800b2990676ULL, "bin24b q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x0fefffff001b7fefULL, "bin24b q1=%016llx", (unsigned long long)e.q1);
    }
    // directory entries:  MakeDirEntry(nameOff, firstChildIndex, childCount)
    {
        TocEntry e = MakeDirEntry(0x0000, 1, 5);  // image[2] root
        CHECK(e.q0 == 0x7fffff0000000000ULL, "dir0 q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x0000000500000001ULL, "dir0 q1=%016llx", (unsigned long long)e.q1);
    }
    {
        TocEntry e = MakeDirEntry(0x2fa9, 0x2c1, 3);  // image[1] ent[2]
        CHECK(e.q0 == 0x7fffff0000002fa9ULL, "dir2 q0=%016llx", (unsigned long long)e.q0);
        CHECK(e.q1 == 0x00000003000002c1ULL, "dir2 q1=%016llx", (unsigned long long)e.q1);
    }
}

// ---- 2. accessors are the exact inverse of the builders, and classify correctly ----------------
static void TestRoundTrip() {
    TocEntry res = MakeResourceEntry(0x2a08, 16567, 0x06fa6, 0xa0100000u, 0x20000000u);
    CHECK(EntryNameOffset(res) == 0x2a08, "res nameOff");
    CHECK(EntryFileSize(res) == 16567, "res size");
    CHECK(EntrySector(res) == 0x06fa6, "res sector");
    CHECK(EntryByteOffset(res) == (uint64_t)0x06fa6 << 9, "res byteOff");
    CHECK(EntryIsResource(res) && !EntryIsDir(res), "res classified resource");
    CHECK(ResSystemFlags(res) == 0xa0100000u, "res sysFlags");
    CHECK(ResGraphicsFlags(res) == 0x20000000u, "res gfxFlags");

    TocEntry bin = MakeBinaryEntry(0x0646, 193, 0x01db4, 0x47b);
    CHECK(EntryNameOffset(bin) == 0x0646, "bin nameOff");
    CHECK(EntryFileSize(bin) == 193, "bin size");
    CHECK(EntrySector(bin) == 0x01db4, "bin sector");
    CHECK(!EntryIsResource(bin) && !EntryIsDir(bin), "bin classified binary");
    CHECK(BinUncompressedSize(bin) == 0x47b, "bin uncompressed");

    TocEntry dir = MakeDirEntry(0x10, 1, 5);
    CHECK(EntryIsDir(dir), "dir classified directory");
    CHECK(DirFirstChild(dir) == 1 && DirChildCount(dir) == 5, "dir child range");
}

// ---- 3. RSC7 header parse ------------------------------------------------------------------------
static void TestRsc7Parse() {
    std::vector<uint8_t> hdr;
    AppendLE32(hdr, kRsc7Magic);     // "RSC7"
    AppendLE32(hdr, 0x00000030u);    // version (arbitrary)
    AppendLE32(hdr, 0xa0100000u);    // system flags
    AppendLE32(hdr, 0x20000000u);    // graphics flags
    hdr.resize(64, 0u);              // a resource is longer; only the first 16 bytes matter here
    Rsc7Header h;
    CHECK(ParseRsc7Header(hdr.data(), hdr.size(), h), "rsc7 parse ok");
    CHECK(h.magic == kRsc7Magic && h.version == 0x30u, "rsc7 magic/version");
    CHECK(h.systemFlags == 0xa0100000u && h.graphicsFlags == 0x20000000u, "rsc7 flags");

    std::vector<uint8_t> notres = {'P','K',0x03,0x04, 1,2,3,4, 5,6,7,8, 9,10,11,12};  // not RSC7
    Rsc7Header h2;
    CHECK(!ParseRsc7Header(notres.data(), notres.size(), h2), "non-rsc7 rejected");
    CHECK(!ParseRsc7Header(hdr.data(), 8, h), "short buffer rejected");
}

// ---- 4. BuildImage: layout invariants -----------------------------------------------------------
static void TestBuildImage() {
    // file 1: a synthetic RSC7 resource of 700 bytes (header + body) -> occupies sectors 0..1.
    std::vector<uint8_t> f1;
    AppendLE32(f1, kRsc7Magic); AppendLE32(f1, 0xA2u /*fragType*/); AppendLE32(f1, 0x11223344u); AppendLE32(f1, 0x55667788u);
    f1.resize(700, 0xAB);
    // file 2: a plain binary blob of 40 bytes -> starts at the next sector boundary (sector 2).
    std::vector<uint8_t> f2(40, 0xCD);

    std::vector<FileSpec> files = {
        {"jesko.yft", f1.data(), f1.size()},
        {"readme.txt", f2.data(), f2.size()},
    };
    BuiltImage img; std::string err;
    CHECK(BuildImage(files, img, err), "BuildImage ok: %s", err.c_str());
    CHECK(img.fileCount == 2, "fileCount");
    CHECK(img.entries.size() == 3, "entries = root + 2");

    // root directory
    CHECK(EntryIsDir(img.entries[0]), "entry0 is dir");
    CHECK(DirFirstChild(img.entries[0]) == 1 && DirChildCount(img.entries[0]) == 2, "root children 1..2");

    // file 1 = resource, flags copied from its RSC7 header, sector 0
    const TocEntry& e1 = img.entries[1];
    CHECK(EntryIsResource(e1), "entry1 resource");
    CHECK(ResSystemFlags(e1) == 0x11223344u && ResGraphicsFlags(e1) == 0x55667788u, "entry1 flags from header");
    CHECK(EntryFileSize(e1) == 700, "entry1 on-disk size");
    CHECK(EntrySector(e1) == 0, "entry1 sector 0");

    // file 2 = binary, starts at sector ceil(700/512) = 2
    const TocEntry& e2 = img.entries[2];
    CHECK(!EntryIsResource(e2) && !EntryIsDir(e2), "entry2 binary");
    CHECK(EntryFileSize(e2) == 40, "entry2 size");
    CHECK(EntrySector(e2) == 2, "entry2 sector 2 (sector-aligned after file1)");
    CHECK(BinUncompressedSize(e2) == 40, "entry2 stored uncompressed");

    // name heap: root name "" at 0, file names resolvable and matching
    CHECK(!img.nameHeap.empty() && img.nameHeap[0] == '\0', "root name empty at 0");
    CHECK(std::string(&img.nameHeap[EntryNameOffset(e1)]) == "jesko.yft", "entry1 name");
    CHECK(std::string(&img.nameHeap[EntryNameOffset(e2)]) == "readme.txt", "entry2 name");

    // blob: each file's bytes sit at sector*512 and match the source bytes
    CHECK(img.blob.size() >= (size_t)EntrySector(e2) * 512 + 40, "blob large enough");
    CHECK(std::memcmp(&img.blob[(size_t)EntrySector(e1) * 512], f1.data(), f1.size()) == 0, "blob holds file1");
    CHECK(std::memcmp(&img.blob[(size_t)EntrySector(e2) * 512], f2.data(), f2.size()) == 0, "blob holds file2");

    // error cases
    BuiltImage bad; std::string e;
    CHECK(!BuildImage({}, bad, e), "empty file list rejected");
    CHECK(!BuildImage({{"", f2.data(), f2.size()}}, bad, e), "empty name rejected");
}

int main() {
    TestGoldenVectors();
    TestRoundTrip();
    TestRsc7Parse();
    TestBuildImage();
    std::printf("pack_collection: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
