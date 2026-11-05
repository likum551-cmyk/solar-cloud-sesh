// pack_collection_test: offline proofs for apps/engine/pack_collection.{h,cpp}.
//
// The entry bit-packing is pinned to GROUND TRUTH: the expected q0/q1 below are the exact 64-bit words
// read live out of GTA V 1.0.3889's own mounted packfiles on 2026-11-03 (the [entdiag] dump), including
// real vehicle-fragment entries. The builder must reproduce them from the decoded field inputs; if the
// format drifts, these fail. No game, no GPU, pure host code.
#include "../apps/engine/pack_collection.h"
#include "../apps/engine/pack_meta.h"
#include "../apps/engine/handling_tune.h"

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

// ---- 5. in-RAM image layout: sector offsets land exactly on the bytes ---------------------------------
// These are real assertions about the layout rules this builder implements (entry sector field -> byte
// offset in the blob, file extents, padding, entry-table serialisation). They are proven offline against the
// entry format pinned above; they do NOT prove the game's own reader accepts the image (that needs a live run).
static std::vector<uint8_t> PatternFile(size_t n, uint8_t salt) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t((i * 131u + salt * 17u + 1u) & 0xffu);  // never all-zero, differs per file
    return v;
}
static size_t CeilSectors(size_t bytes) { return (bytes + 511) / 512; }  // independent of SectorsFor on purpose

static void TestImageLayout() {
    // Sizes straddle every sector boundary case: 1 byte, just under / exactly / just over a sector, several
    // sectors, an RSC7 resource (header kept) that is not a sector multiple, and a larger multi-sector file.
    const size_t sizes[] = {1, 511, 512, 513, 1024, 700, 40, 5000, 2049};
    std::vector<std::vector<uint8_t>> src;
    std::vector<FileSpec> files;
    std::vector<std::string> names;
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        src.push_back(PatternFile(sizes[k], uint8_t(k + 1)));
        names.push_back("file" + std::to_string(k) + (k == 5 ? ".yft" : ".bin"));
    }
    // file 5 (700 B) becomes an RSC7 resource: overwrite its first 16 bytes with a real header.
    {
        std::vector<uint8_t> hdr;
        AppendLE32(hdr, kRsc7Magic); AppendLE32(hdr, 0xA2u); AppendLE32(hdr, 0xa3022403u); AppendLE32(hdr, 0x20000000u);
        std::memcpy(src[5].data(), hdr.data(), 16);
    }
    for (size_t k = 0; k < src.size(); ++k) files.push_back({names[k], src[k].data(), src[k].size()});

    BuiltImage img; std::string err;
    CHECK(BuildImage(files, img, err), "layout: BuildImage ok: %s", err.c_str());
    const size_t N = files.size();
    CHECK(img.entries.size() == N + 1 && img.placements.size() == N, "layout: root + N entries, N placements");

    bool allSectorsLand = true, allExtentsOk = true, allCountsOk = true, allPadZero = true, allNames = true;
    for (size_t k = 0; k < N; ++k) {
        const TocEntry& e = img.entries[k + 1];
        const FilePlacement& p = img.placements[k];
        // byteOffset recomputed straight from the documented formula, NOT via EntryByteOffset.
        const uint64_t byteOffset = (uint64_t)(EntryC(e) & 0x7fffffu) << 9;
        if (p.entryIndex != k + 1 || p.byteOffset != byteOffset || p.sector != (EntryC(e) & 0x7fffffu)) allSectorsLand = false;
        if (byteOffset % 512 != 0) allSectorsLand = false;
        // the file's bytes sit exactly there in the blob
        if (byteOffset + src[k].size() > img.blob.size() ||
            std::memcmp(img.blob.data() + byteOffset, src[k].data(), src[k].size()) != 0) allExtentsOk = false;
        if (EntryFileSize(e) != src[k].size() || p.size != src[k].size()) allExtentsOk = false;
        // sectorCount is ceil(size/512), and the next entry starts exactly that many sectors later
        if (p.sectorCount != CeilSectors(src[k].size())) allCountsOk = false;
        if (k + 1 < N) {
            const uint32_t thisSec = EntrySector(img.entries[k + 1]), nextSec = EntrySector(img.entries[k + 2]);
            if (nextSec - thisSec != CeilSectors(src[k].size())) allCountsOk = false;
            // the padding between the end of this file and the start of the next is all zero
            const size_t padStart = (size_t)byteOffset + src[k].size(), padEnd = (size_t)nextSec * 512;
            for (size_t b = padStart; b < padEnd; ++b) if (img.blob[b] != 0) allPadZero = false;
        }
        if (std::string(&img.nameHeap[EntryNameOffset(e)]) != names[k]) allNames = false;
    }
    CHECK(allSectorsLand, "layout: every entry's (C & 0x7fffff) << 9 is its placement byte offset, sector-aligned");
    CHECK(allExtentsOk, "layout: every file's bytes are at blob[byteOffset .. +size) and the entry sizes match");
    CHECK(allCountsOk, "layout: next.sector - this.sector == ceil(size/512) for every consecutive pair");
    CHECK(allPadZero, "layout: the padding between files is zero");
    CHECK(allNames, "layout: every NameOffset resolves to the file's name in the heap");

    // first file at sector 0; the blob is a whole number of sectors ending exactly after the last file
    CHECK(EntrySector(img.entries[1]) == 0, "layout: first file at sector 0");
    const FilePlacement& last = img.placements[N - 1];
    CHECK(img.blobSectors == last.sector + CeilSectors(src[N - 1].size()), "layout: blobSectors == last.sector + ceil(lastSize/512)");
    CHECK(img.blob.size() == (size_t)img.blobSectors * 512, "layout: blob is blobSectors * 512 bytes (final tail padded)");
    bool tailZero = true;
    for (size_t b = (size_t)last.byteOffset + last.size; b < img.blob.size(); ++b) if (img.blob[b] != 0) tailZero = false;
    CHECK(tailZero, "layout: the final file's tail padding is zero");
    // concrete expectation for this size set, worked out by hand: sectors 0,1,2,4,5,7,9,10,..
    // sizes 1,511,512,513,1024,700,40,5000,2049 -> counts 1,1,1,2,2,2,1,10,5 -> starts 0,1,2,3,5,7,9,10,20 -> total 25
    const uint32_t wantStart[] = {0, 1, 2, 3, 5, 7, 9, 10, 20};
    bool startsOk = true;
    for (size_t k = 0; k < N; ++k) if (EntrySector(img.entries[k + 1]) != wantStart[k]) startsOk = false;
    CHECK(startsOk, "layout: hand-computed start sectors 0,1,2,3,5,7,9,10,20");
    CHECK(img.blobSectors == 25, "layout: hand-computed total 25 sectors (got %u)", img.blobSectors);
    CHECK(EntryIsResource(img.entries[6]) && ResSystemFlags(img.entries[6]) == 0xa3022403u &&
          ResGraphicsFlags(img.entries[6]) == 0x20000000u, "layout: the RSC7 file is a resource entry with its header flags");
    CHECK(!EntryIsResource(img.entries[1]) && !EntryIsDir(img.entries[1]), "layout: a plain file is a binary entry");

    // entry table: 16 bytes per entry, stride 0x10, q0 at +0 and q1 at +8, little-endian
    CHECK(img.entryTable.size() == img.entries.size() * 0x10, "table: 16 bytes per entry");
    bool tableRoundTrips = true;
    for (size_t i = 0; i < img.entries.size(); ++i) {
        const TocEntry r = ReadTocEntry(img.entryTable.data() + i * 0x10);
        if (r.q0 != img.entries[i].q0 || r.q1 != img.entries[i].q1) tableRoundTrips = false;
    }
    CHECK(tableRoundTrips, "table: entry i at byte i*0x10 reads back as entries[i]");
    {
        // root dir q0 = 0x7fffff0000000000 -> LE bytes 00 00 00 00 00 ff ff 7f ; q1 = (N<<32)|1
        const uint8_t* t = img.entryTable.data();
        const uint8_t wantQ0[8] = {0, 0, 0, 0, 0, 0xff, 0xff, 0x7f};
        CHECK(std::memcmp(t, wantQ0, 8) == 0, "table: root dir q0 bytes are little-endian");
        CHECK(t[8] == 1 && t[9] == 0 && t[12] == uint8_t(N) && t[13] == 0, "table: root dir q1 = (childCount << 32) | firstChild");
    }
    {
        // a live golden word serialises to the byte order a little-endian qword read would see
        TocEntry g = MakeResourceEntry(0x2a08, 16567, 0x06fa6, 0xa0100000u, 0x20000000u);  // q0 = 0x806fa60040b72a08
        std::vector<uint8_t> t = SerializeEntryTable({g});
        const uint8_t want[16] = {0x08, 0x2a, 0xb7, 0x40, 0x00, 0xa6, 0x6f, 0x80, 0x00, 0x00, 0x10, 0xa0, 0x00, 0x00, 0x00, 0x20};
        CHECK(t.size() == 16 && std::memcmp(t.data(), want, 16) == 0, "table: golden res248 serialises to the expected 16 bytes");
        CHECK(SerializeEntryTable({}).empty(), "table: an empty TOC serialises to nothing");
    }

    // deterministic: building the same files twice gives the same bytes
    BuiltImage again; std::string e2;
    CHECK(BuildImage(files, again, e2) && again.blob == img.blob && again.entryTable == img.entryTable &&
          again.nameHeap == img.nameHeap && again.blobSectors == img.blobSectors, "layout: deterministic output");
}

static void TestImageLayoutEdges() {
    // A zero-size file occupies zero sectors, so the next file starts at the same sector (next - this == ceil(0/512) == 0).
    std::vector<uint8_t> a = PatternFile(513, 7), b = PatternFile(1, 9);
    std::vector<FileSpec> files = {{"a.bin", a.data(), a.size()}, {"z.bin", nullptr, 0}, {"b.bin", b.data(), b.size()}};
    BuiltImage img; std::string err;
    CHECK(BuildImage(files, img, err), "edge: zero-size file accepted: %s", err.c_str());
    CHECK(EntrySector(img.entries[1]) == 0 && EntrySector(img.entries[2]) == 2 && EntrySector(img.entries[3]) == 2,
          "edge: a(513 B) at 0, zero-size z at 2, b at 2");
    CHECK(img.placements[1].sectorCount == 0 && img.placements[2].sectorCount == 1, "edge: sectorCount 0 for the empty file");
    CHECK(img.blobSectors == 3 && img.blob.size() == 3 * 512, "edge: 3 sectors total");

    // a single exact-sector file: blob is exactly one sector, no extra padding sector
    std::vector<uint8_t> one = PatternFile(512, 3);
    std::vector<FileSpec> f1 = {{"one.bin", one.data(), one.size()}};
    BuiltImage i1;
    CHECK(BuildImage(f1, i1, err) && i1.blobSectors == 1 && i1.blob.size() == 512 &&
          std::memcmp(i1.blob.data(), one.data(), 512) == 0, "edge: a 512-byte file fills exactly one sector");

    // a 24-bit size overflow is rejected before the data is read
    uint8_t dummy = 0;
    BuiltImage bad;
    CHECK(!BuildImage({{"big.bin", &dummy, (size_t)kMaxFileSize24 + 1}}, bad, err), "edge: size over the 24-bit field rejected");
    CHECK(SectorsFor(0) == 0 && SectorsFor(1) == 1 && SectorsFor(512) == 1 && SectorsFor(513) == 2, "SectorsFor is ceil(n/512)");
}

// ---- 6. RSC7 page-flag decode: SPEC-CONSISTENCY tests, NOT game-validated --------------------------------
// IMPORTANT: DecodeRsc7PageFlags implements the formula as WRITTEN in docs/content-streaming/r3-plan.md
// (game fn 0x1370140). The tests below prove only that the code matches that document: the expected
// decompositions were derived by hand / by an independent script from the same text. They do NOT show that the
// game computes the same totals. The real sizes must be confirmed by a live run on the owner's machine, and two
// readings are unconfirmed until then: (1) bits 24..27 -> size shifts 3..0 (ascending bit, descending shift),
// (2) bits 28..31 are ignored. If either is wrong the jesko totals below change.
static void CheckPages(const char* what, const Rsc7Pages& got, uint32_t baseShift, uint32_t baseSize,
                       const uint32_t (&counts)[kRsc7SizeClasses], uint32_t pageCount, uint64_t total) {
    bool ok = got.baseShift == baseShift && got.basePageSize == baseSize && got.pageCount == pageCount && got.totalSize == total;
    for (int s = 0; s < kRsc7SizeClasses; ++s) if (got.counts[s] != counts[s]) ok = false;
    CHECK(ok, "%s: baseShift %u base 0x%x pages %u total 0x%llx", what, got.baseShift, got.basePageSize, got.pageCount,
          (unsigned long long)got.totalSize);
    // the decomposition is self-consistent with the documented formula
    uint64_t sum = 0; uint32_t n = 0;
    for (int s = 0; s < kRsc7SizeClasses; ++s) { n += got.counts[s]; sum += (uint64_t)got.counts[s] * ((uint64_t)got.basePageSize << s); }
    CHECK(n == got.pageCount && sum == got.totalSize && got.basePageSize == (0x200u << got.baseShift),
          "%s: totals equal the sum of count * (base << shift)", what);
}

static void TestRsc7PageFlags() {
    // counts[] is indexed by size shift 0..8 (pages of base << shift).
    {   // jesko.yft: sys 0xa3022403 -> shift 3 (base 0x1000); bits 7..10 = 8 (<<6), bits 11..16 = 4 (<<5), bit 24 (<<3) and bit 25 (<<2) set
        const uint32_t c[kRsc7SizeClasses] = {0, 0, 1, 1, 1, 4, 8, 0, 0};
        Rsc7PageFlags d = DecodeRsc7PageFlags(0xa3022403u, 0x20000000u);
        CheckPages("jesko.yft sys", d.sys, 3, 0x1000, c, 15, 0x29c000);
        const uint32_t z[kRsc7SizeClasses] = {};
        CheckPages("jesko.yft gfx", d.gfx, 0, 0x200, z, 0, 0);   // 0x20000000 sets only bit 29, which the plan does not map
    }
    {   // jesko.ytd: sys 0x00060000 -> shift 0 (base 0x200), bits 17..23 = 3 (<<4) ; gfx 0xdb100008 -> shift 8 (base 0x20000)
        const uint32_t cs[kRsc7SizeClasses] = {0, 0, 0, 0, 3, 0, 0, 0, 0};
        const uint32_t cg[kRsc7SizeClasses] = {1, 0, 1, 1, 8, 0, 0, 0, 0};
        Rsc7PageFlags d = DecodeRsc7PageFlags(0x00060000u, 0xdb100008u);
        CheckPages("jesko.ytd sys", d.sys, 0, 0x200, cs, 3, 0x6000);
        CheckPages("jesko.ytd gfx", d.gfx, 8, 0x20000, cg, 11, 0x11a0000);
    }
    {   // jesko_hi.yft: sys 0xa101f004 -> shift 4 (base 0x2000); bits 11..16 = 62 (<<5), bit 24 (<<3) set
        const uint32_t cs[kRsc7SizeClasses] = {0, 0, 0, 1, 0, 62, 0, 0, 0};
        Rsc7PageFlags d = DecodeRsc7PageFlags(0xa101f004u, 0x20000000u);
        CheckPages("jesko_hi.yft sys", d.sys, 4, 0x2000, cs, 63, 0xf90000);
        const uint32_t z[kRsc7SizeClasses] = {};
        CheckPages("jesko_hi.yft gfx", d.gfx, 0, 0x200, z, 0, 0);
    }
    // each field alone, so a shifted or mis-widthed field cannot hide behind the jesko values
    {
        const uint32_t b4[kRsc7SizeClasses] = {0, 0, 0, 0, 0, 0, 0, 0, 1};
        CheckPages("bit4 only", DecodeRsc7Pages(0x10u), 0, 0x200, b4, 1, 0x20000);
        const uint32_t b27[kRsc7SizeClasses] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
        CheckPages("bit27 only", DecodeRsc7Pages(1u << 27), 0, 0x200, b27, 1, 0x200);
        const uint32_t b24[kRsc7SizeClasses] = {0, 0, 0, 1, 0, 0, 0, 0, 0};
        CheckPages("bit24, shift 2", DecodeRsc7Pages((1u << 24) | 2u), 2, 0x800, b24, 1, 0x4000);
        const uint32_t z[kRsc7SizeClasses] = {};
        CheckPages("flags 0", DecodeRsc7Pages(0), 0, 0x200, z, 0, 0);
        // widths: a field holds its maximum and does not leak into its neighbour
        const uint32_t mx[kRsc7SizeClasses] = {1, 1, 1, 1, 127, 63, 15, 3, 1};
        CheckPages("all count bits set, shift 15", DecodeRsc7Pages(0x0fffffffu), 15, 0x1000000, mx, 213, 0x161f000000ULL);
        const uint32_t w5[kRsc7SizeClasses] = {0, 0, 0, 0, 0, 0, 0, 3, 0};
        CheckPages("bits 5..6 = 3", DecodeRsc7Pages(0x60u), 0, 0x200, w5, 3, 0x30000);   // 3 * (0x200 << 7)
    }
    // the plan is silent on bits 28..31; this pins what the code does with them today (ignored), nothing more.
    CHECK(DecodeRsc7Pages(0xf0000000u).pageCount == 0 && DecodeRsc7Pages(0xf0000000u).totalSize == 0,
          "bits 28..31 ignored (spec silent: unconfirmed)");
    // the total exceeds 32 bits for the largest classes: it must not wrap
    CHECK(DecodeRsc7Pages(0x0fffffffu).totalSize > 0xffffffffULL, "total is 64-bit (no wrap above 4 GiB)");
}

// ---- 8. pack_meta: content.xml / vehicles.meta scans (the content-pack mount's model list) -------------
static void TestPackMeta() {
    // the shape of the rs6 pack's content.xml (2026-11-05), plus a commented-out item and an odd-case type
    const std::string content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CDataFileMgr__ContentsOfDataFileXml>\n  <dataFiles>\n"
        "    <Item>\n      <filename>dlc_rs6:/common/data/vehiclelayouts.meta</filename>\n"
        "      <fileType>VEHICLE_LAYOUTS_FILE</fileType>\n    </Item>\n"
        "    <Item>\n      <filename>dlc_rs6:/common/data/vehicles.meta</filename>\n"
        "      <fileType>VEHICLE_METADATA_FILE</fileType>\n      <overlay value=\"false\" />\n    </Item>\n"
        "    <!-- <Item><filename>dlc_rs6:/old/vehicles.meta</filename><fileType>VEHICLE_METADATA_FILE</fileType></Item> -->\n"
        "    <Item>\n      <filename> dlc_rs6:/common/data/extra.meta </filename>\n      <fileType>vehicle_metadata_file</fileType>\n    </Item>\n"
        "    <Item>\n      <filename>dlc_rs6:/%PLATFORM%/levels/gta5/vehicles/vehicles.rpf</filename>\n"
        "      <fileType>RPF_FILE</fileType>\n    </Item>\n  </dataFiles>\n"
        "  <contentChangeSets><Item><filesToEnable><Item>dlc_rs6:/common/data/vehicles.meta</Item></filesToEnable></Item>"
        "</contentChangeSets>\n</CDataFileMgr__ContentsOfDataFileXml>\n";
    const auto metas = MetaFilesOfType(content, "VEHICLE_METADATA_FILE");
    CHECK(metas.size() == 2, "metas %u", (unsigned)metas.size());
    CHECK(metas.size() == 2 && metas[0] == "dlc_rs6:/common/data/vehicles.meta" && metas[1] == "dlc_rs6:/common/data/extra.meta",
          "meta names (comment skipped, trimmed, type case-insensitive)");
    CHECK(MetaFilesOfType(content, "RPF_FILE").size() == 1, "one rpf");
    CHECK(MetaFilesOfType(content, "AUDIO_GAMEDATA").empty(), "absent type -> empty");
    CHECK(MetaFilesOfType("<no data files/>", "VEHICLE_METADATA_FILE").empty(), "no dataFiles -> empty");
    CHECK(PathInPack("dlc_rs6:/common/data/vehicles.meta") == "common/data/vehicles.meta", "path in pack");
    CHECK(PathInPack("dlc_rs6:/%PLATFORM%/levels/a.rpf") == "x64/levels/a.rpf", "%%PLATFORM%% -> x64");
    CHECK(PathInPack("data/handling.meta") == "data/handling.meta", "no prefix kept");

    // two vehicles (the diabloSV pack's field order), a commented one, a duplicate and an invalid name
    const std::string veh =
        "<CVehicleModelInfo__InitDataList>\n  <InitDatas>\n"
        "    <Item>\n      <modelName>diabloSV</modelName>\n      <txdName>diabloSV</txdName>\n"
        "      <handlingId>diabloSV</handlingId>\n      <gameName>Diablo SV</gameName>\n"
        "      <vehicleMakeName>Lamborghini</vehicleMakeName>\n      <vehicleClass>VC_SUPER</vehicleClass>\n    </Item>\n"
        "    <!-- <Item><modelName>oldcar</modelName></Item> -->\n"
        "    <Item>\n      <modelName> rs6 </modelName>\n      <gameName>RS6</gameName>\n      <vehicleMakeName>AUDI</vehicleMakeName>\n"
        "    </Item>\n"
        "    <Item>\n      <modelName>RS6</modelName>\n    </Item>\n"
        "    <Item>\n      <modelName>bad name!</modelName>\n    </Item>\n"
        "  </InitDatas>\n  <txdRelationships><Item><parent>vehicles_race_generic</parent><child>diabloSV</child></Item>"
        "</txdRelationships>\n</CVehicleModelInfo__InitDataList>\n";
    const auto v = ParseVehiclesMeta(veh);
    CHECK(v.size() == 2, "vehicles %u", (unsigned)v.size());
    if (v.size() == 2) {
        CHECK(v[0].model == "diabloSV" && v[0].make == "Lamborghini" && v[0].label == "Diablo SV" && v[0].vclass == "VC_SUPER",
              "diabloSV fields: %s / %s / %s / %s", v[0].model.c_str(), v[0].make.c_str(), v[0].label.c_str(), v[0].vclass.c_str());
        CHECK(v[1].model == "rs6" && v[1].make == "AUDI" && v[1].label == "RS6" && v[1].vclass.empty(),
              "rs6 fields (trimmed; class absent -> empty, not the next car's)");
    }
    CHECK(ParseVehiclesMeta("").empty() && ParseVehiclesMeta("<modelName></modelName>").empty(), "empty -> none");
}

// The overlay car browser's text rules (pack_meta.h), on the names the staged packs really carry (2026-11-05).
static void TestCarBrowserText() {
    using namespace gage::game::content;
    struct Row { const char *make, *label, *model, *want; };
    const Row rows[] = {
        {"jesko", "jesko", "jesko", "Jesko"},                          // same once squashed, all lower case -> capital
        {"BMW M5", "BMWM5", "bmwm5", "BMW M5"},                        // same once squashed -> the spaced spelling
        {"AUDI", "RS6", "rs6", "AUDI RS6"},                            // different -> make + label
        {"Lamborghini", "Diablo SV", "diabloSV", "Lamborghini Diablo SV"},
        {"BMW", "BMW M3 GTR", "m3gtr", "BMW M3 GTR"},                  // label starts with the make -> the label
        {"", "", "supra", "Supra"},                                    // nothing -> the model
        {"NULL", "CARNOTFOUND", "z350", "Z350"},                       // the game's "no text" labels count as missing
        {"Nissan", "", "z350", "Nissan Z350"},                         // a lone make is followed by the model
        {"", "Skyline GT-R", "r34", "Skyline GT-R"},
        {"  Audi  ", " RS6 ", "rs6", "Audi RS6"},                      // trimmed
    };
    for (const Row& r : rows) {
        const std::string got = CarDisplayName(r.make, r.label, r.model);
        CHECK(got == r.want, "CarDisplayName('%s', '%s', '%s') = '%s', want '%s'", r.make, r.label, r.model, got.c_str(), r.want);
    }
    CHECK(CarClassName("VC_SUPER") == "Super", "class super: %s", CarClassName("VC_SUPER").c_str());
    CHECK(CarClassName("VC_SPORT_CLASSIC") == "Sport classic", "class sport classic: %s", CarClassName("VC_SPORT_CLASSIC").c_str());
    CHECK(CarClassName("vc_sedan") == "Sedan" && CarClassName("").empty() && CarClassName("SUPER") == "Super",
          "class edge cases: '%s' '%s' '%s'", CarClassName("vc_sedan").c_str(), CarClassName("").c_str(), CarClassName("SUPER").c_str());

    const std::string hay = "lamborghini diablo sv diablosv super vehicle_lambo";
    CHECK(MatchesAllTerms("", hay) && MatchesAllTerms("   ", hay) && MatchesAllTerms(nullptr, hay), "a blank query matches");
    CHECK(MatchesAllTerms("LAMBO", hay) && MatchesAllTerms("diablo  super", hay) && MatchesAllTerms(" sv ", hay),
          "any case, any term order, extra spaces");
    CHECK(!MatchesAllTerms("diablo sedan", hay), "every term must match (AND), not any");
    CHECK(!MatchesAllTerms("jesko", hay) && !MatchesAllTerms("lambox", hay), "a missing term fails");
}

// The runtime handling tune for NG packs (handling_tune.h): the same rules file stage_packs.mjs applies to loose packs.
static void TestHandlingTune() {
    const int before = g_pass + g_fail;
    const std::string json = R"({
      "_comment": ["ignored", "array"],
      "*": { "scale": { "fTractionCurveMax": 1.15, "fTractionCurveMin": 1.15 } },
      "REGERA": { "_why": "named set after the * scale", "set": { "fTractionCurveMin": 3.5, "fSuspensionRaise": 0.03 },
                  "scale": { "fInitialDriveForce": 0.5 } },
      "broken": { "scale": { "fMissingField": 2 } },
      "dupe":   { "set": { "fMass": 1000 } }
    })";
    const HandlingTune t = ParseHandlingTune(json);
    CHECK(t.ok && t.haveStar && t.named.size() == 3, "rules parse: ok %d star %d named %u err '%s'", (int)t.ok, (int)t.haveStar,
          (unsigned)t.named.size(), t.error.c_str());
    CHECK(!ParseHandlingTune("{ \"*\": { \"scale\": { \"fX\": } } }").ok && !ParseHandlingTune("{} trailing").ok &&
          ParseHandlingTune("{}").ok, "malformed rules are refused, an empty object is fine");

    const std::string sub =
        "<SubHandlingData>\n        <Item type=\"CCarHandlingData\">\n          <fTractionCurveMax value=\"9.000000\" />\n"
        "          <fBackEndPopUpCarImpulseMult value=\"0.100000\" />\n        </Item>\n        <Item type=\"NULL\" />\n"
        "        <Item type=\"NULL\" />\n      </SubHandlingData>";
    auto item = [&sub](const char* name, const char* extra) {
        return std::string("<Item type=\"CHandlingData\">\n      <handlingName>") + name + "</handlingName>\n"
               "      <fMass value=\"1800.000000\" />\n      <fInitialDriveForce value=\"0.300000\" />\n"
               "      <fTractionCurveMax value=\"2.950000\" />\n      <fTractionCurveMaxRatio value=\"1.000000\" />\n"
               "      <fTractionCurveMin value=\"3.050000\" />\n      <fSuspensionRaise value=\"-0.005000\" />\n" + extra +
               "      " + sub + "\n    </Item>";
    };
    const std::string xml = "<?xml version=\"1.0\"?>\n<CHandlingDataMgr>\n  <HandlingData>\n    " + item("regera", "") +
                            "\n    " + item("broken", "") + "\n    " + item("dupe", "      <fMass value=\"1.0\" />\n") +
                            "\n  </HandlingData>\n</CHandlingDataMgr>\n";
    const HandlingTuneResult r = TuneHandlingMeta(xml, t);
    CHECK(r.seen == 3, "three CHandlingData items seen (nested SubHandlingData items do not end an item early): %d", r.seen);
    CHECK(r.items.size() == 1 && r.items[0].name == "regera", "only the tunable item is emitted: %u", (unsigned)r.items.size());
    CHECK(r.skipped.size() == 2, "the missing-field and duplicate-field items are skipped: %u", (unsigned)r.skipped.size());
    if (r.skipped.size() == 2) {
        CHECK(r.skipped[0].find("broken: fMissingField not found") == 0, "missing field reason: %s", r.skipped[0].c_str());
        CHECK(r.skipped[1].find("dupe: fMass found 2 times") == 0, "duplicate field reason: %s", r.skipped[1].c_str());
    }
    if (!r.items.empty()) {
        const std::string& o = r.items[0].text;
        // * scales max 2.95 -> 3.3925; min 3.05 -> 3.5075, then REGERA's set (case-insensitive name) makes it 3.5.
        CHECK(o.find("<fTractionCurveMax value=\"3.3925\" />") != std::string::npos, "* scale on max");
        CHECK(o.find("<fTractionCurveMin value=\"3.5\" />") != std::string::npos, "named set overrides the * scale");
        CHECK(o.find("<fInitialDriveForce value=\"0.15\" />") != std::string::npos, "named scale");
        CHECK(o.find("<fSuspensionRaise value=\"0.03\" />") != std::string::npos, "named set of a negative value");
        CHECK(o.find("<fTractionCurveMaxRatio value=\"1.000000\" />") != std::string::npos, "a longer field name is not a hit");
        CHECK(o.find(sub) != std::string::npos, "SubHandlingData kept verbatim (its fTractionCurveMax untouched)");
        CHECK(o.find("<fMass value=\"1800.000000\" />") != std::string::npos, "unlisted fields keep their text");
        CHECK(r.items[0].changes == "fTractionCurveMax 2.950000->3.3925, fTractionCurveMin 3.050000->3.5075, "
                                    "fInitialDriveForce 0.300000->0.15, fTractionCurveMin 3.5075->3.5, fSuspensionRaise -0.005000->0.03",
              "change log: %s", r.items[0].changes.c_str());
    }
    // The broken item is left out entirely, not half-applied (no * scale written into the output for it).
    {
        bool half = false;
        for (const TunedHandlingItem& it : r.items) half |= it.name == "broken";
        CHECK(!half, "a skipped item is never emitted");
    }
    // No "*" and no matching name: nothing emitted, nothing skipped.
    {
        const HandlingTune t2 = ParseHandlingTune(R"({ "other": { "scale": { "fMass": 2 } } })");
        const HandlingTuneResult r2 = TuneHandlingMeta(xml, t2);
        CHECK(t2.ok && r2.seen == 3 && r2.items.empty() && r2.skipped.empty(), "no matching rule: items %u skipped %u",
              (unsigned)r2.items.size(), (unsigned)r2.skipped.size());
    }
    // Number formatting: 6 decimals, shortest form, no "-0".
    {
        using tunedetail::FormatValue;
        CHECK(FormatValue(4.6000000001) == "4.6" && FormatValue(300.0) == "300" && FormatValue(-0.0000001) == "0" &&
              FormatValue(-0.005) == "-0.005" && FormatValue(2.95 * 1.15) == "3.3925" && FormatValue(0.5505 * 0.67) == "0.368835",
              "format: %s %s %s %s %s %s", FormatValue(4.6000000001).c_str(), FormatValue(300.0).c_str(), FormatValue(-0.0000001).c_str(),
              FormatValue(-0.005).c_str(), FormatValue(2.95 * 1.15).c_str(), FormatValue(0.5505 * 0.67).c_str());
    }
    // The override pack's files name one HANDLING_FILE that the change set enables, under the setup2 device.
    {
        const std::string c = BuildTuneContent(), s = BuildTuneSetup2(), m = BuildTuneMeta(r.items);
        const auto hf = MetaFilesOfType(c, "HANDLING_FILE");
        CHECK(hf.size() == 1 && hf[0] == "dlc_gagetune:/data/handling.meta" && PathInPack(hf[0]) == "data/handling.meta",
              "content.xml lists the one handling file");
        CHECK(c.find("<Item>dlc_gagetune:/data/handling.meta</Item>") != std::string::npos &&
              c.find("<changeSetName>gagetune_AUTOGEN</changeSetName>") != std::string::npos &&
              s.find("<Item>gagetune_AUTOGEN</Item>") != std::string::npos && s.find("<deviceName>dlc_gagetune</deviceName>") != std::string::npos,
              "setup2.xml's change set is the one content.xml defines");
        CHECK(TuneHandlingMeta(m, ParseHandlingTune("{}")).seen == 1, "the override meta holds the tuned item");
    }
    const int ran = g_pass + g_fail - before;
    CHECK(ran == 21, "handling tune: %d checks ran before this one (want 21)", ran);
}

int main() {
    TestGoldenVectors();
    TestPackMeta();
    TestCarBrowserText();
    TestHandlingTune();
    TestRoundTrip();
    TestRsc7Parse();
    TestBuildImage();
    TestImageLayout();
    TestImageLayoutEdges();
    TestRsc7PageFlags();
    std::printf("pack_collection: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
