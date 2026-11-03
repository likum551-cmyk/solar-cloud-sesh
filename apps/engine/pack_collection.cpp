// pack_collection: implementation of the pure RPF7-shaped TOC builder. See pack_collection.h for the
// confirmed entry format and the contract. No game calls, no game memory.
#include "pack_collection.h"

#include <cstring>

namespace gage::game::content {

static uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ParseRsc7Header(const uint8_t* data, size_t len, Rsc7Header& out) {
    if (!data || len < 16) return false;
    const uint32_t magic = ReadLE32(data);
    if (magic != kRsc7Magic) return false;
    out.magic = magic;
    out.version = ReadLE32(data + 4);
    out.systemFlags = ReadLE32(data + 8);
    out.graphicsFlags = ReadLE32(data + 12);
    return true;
}

bool BuildImage(const std::vector<FileSpec>& files, BuiltImage& out, std::string& err) {
    out = BuiltImage{};
    err.clear();
    if (files.empty()) { err = "no files"; return false; }

    const uint32_t fileCount = static_cast<uint32_t>(files.size());

    // Name heap: root name "" at offset 0, then each leaf name, each NUL-terminated. The root dir entry
    // points at offset 0; every file entry's NameOffset is the byte offset of its name in this heap.
    std::vector<char> heap;
    heap.push_back('\0');  // root name = "" at offset 0
    std::vector<uint32_t> nameOffsets(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        const FileSpec& f = files[i];
        if (f.name.empty()) { err = "empty file name at index " + std::to_string(i); return false; }
        if (!f.data && f.size != 0) { err = "null data for " + f.name; return false; }
        if (f.size > kMaxFileSize24) { err = f.name + ": size exceeds 24-bit on-disk field"; return false; }
        const size_t off = heap.size();
        if (off > 0xffff) { err = "name heap exceeds the 16-bit NameOffset field"; return false; }
        nameOffsets[i] = static_cast<uint32_t>(off);
        heap.insert(heap.end(), f.name.begin(), f.name.end());
        heap.push_back('\0');
    }

    // Data blob: each file's bytes start on a 512-byte sector boundary; the gaps are zero padding. The
    // file entry stores the start sector index (not the byte offset). After the last file the blob is padded
    // to a whole number of sectors, so the blob a collection exposes is `blobSectors * 512` bytes and a
    // sector-granular read of the final file's last (partial) sector never runs past the end.
    std::vector<uint8_t> blob;
    std::vector<uint32_t> startSectors(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        const size_t pad = (kSectorSize - (blob.size() % kSectorSize)) % kSectorSize;
        blob.insert(blob.end(), pad, 0u);
        const uint64_t startSector = blob.size() / kSectorSize;
        if (startSector > kSectorMask) { err = files[i].name + ": sector offset exceeds the 23-bit field"; return false; }
        startSectors[i] = static_cast<uint32_t>(startSector);
        if (files[i].size != 0) blob.insert(blob.end(), files[i].data, files[i].data + files[i].size);
    }
    blob.insert(blob.end(), (kSectorSize - (blob.size() % kSectorSize)) % kSectorSize, 0u);
    if (blob.size() / kSectorSize > kSectorMask) { err = "blob exceeds the 23-bit sector-offset field"; return false; }

    // Entries: [0] = root directory (children 1..N), then one entry per file.
    std::vector<TocEntry> entries;
    entries.reserve(fileCount + 1);
    entries.push_back(MakeDirEntry(/*nameOffset=*/0, /*firstChildIndex=*/1, /*childCount=*/fileCount));
    for (uint32_t i = 0; i < fileCount; ++i) {
        const FileSpec& f = files[i];
        const uint32_t onDisk = static_cast<uint32_t>(f.size);
        Rsc7Header rsc;
        if (ParseRsc7Header(f.data, f.size, rsc)) {
            entries.push_back(MakeResourceEntry(nameOffsets[i], onDisk, startSectors[i],
                                                rsc.systemFlags, rsc.graphicsFlags));
        } else {
            // Stored uncompressed, so the uncompressed size equals the on-disk size.
            entries.push_back(MakeBinaryEntry(nameOffsets[i], onDisk, startSectors[i], onDisk));
        }
    }

    // Placement view: where each file's bytes sit in the blob, derived from the layout above.
    out.placements.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        FilePlacement p;
        p.entryIndex = i + 1;
        p.sector = startSectors[i];
        p.sectorCount = static_cast<uint32_t>(SectorsFor(files[i].size));
        p.byteOffset = static_cast<uint64_t>(startSectors[i]) << kSectorShift;
        p.size = static_cast<uint32_t>(files[i].size);
        out.placements.push_back(p);
    }

    out.entryTable = SerializeEntryTable(entries);
    out.entries = std::move(entries);
    out.nameHeap = std::move(heap);
    out.blobSectors = static_cast<uint32_t>(blob.size() / kSectorSize);
    out.blob = std::move(blob);
    out.fileCount = fileCount;
    return true;
}

static void PutLE64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xffu));
}

static uint64_t ReadLE64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 7; i >= 0; --i) x = (x << 8) | p[i];
    return x;
}

std::vector<uint8_t> SerializeEntryTable(const std::vector<TocEntry>& entries) {
    std::vector<uint8_t> t;
    t.reserve(entries.size() * 16);
    for (const TocEntry& e : entries) { PutLE64(t, e.q0); PutLE64(t, e.q1); }
    return t;
}

TocEntry ReadTocEntry(const uint8_t* p) {
    TocEntry e;
    e.q0 = ReadLE64(p);
    e.q1 = ReadLE64(p + 8);
    return e;
}

// ---- RSC7 page-flag decode (spec: docs/content-streaming/r3-plan.md; see the header for the caveats) ----
namespace {
struct PageField { uint32_t lowBit; uint32_t width; uint32_t sizeShift; };
// Lowest bit first. The last four single-bit fields (bits 24..27 -> shifts 3..0) are the plan's
// "bits24-27 (<<3..<<0)" read as ascending bit / descending shift; that mapping is an interpretation.
constexpr PageField kPageFields[] = {
    {4, 1, 8}, {5, 2, 7}, {7, 4, 6}, {11, 6, 5}, {17, 7, 4}, {24, 1, 3}, {25, 1, 2}, {26, 1, 1}, {27, 1, 0},
};
static_assert(sizeof(kPageFields) / sizeof(kPageFields[0]) == kRsc7SizeClasses, "one field per size class");
}  // namespace

Rsc7Pages DecodeRsc7Pages(uint32_t flags) {
    Rsc7Pages r;
    r.baseShift = flags & 0xfu;
    r.basePageSize = 0x200u << r.baseShift;
    for (const PageField& f : kPageFields) {
        const uint32_t count = (flags >> f.lowBit) & ((1u << f.width) - 1u);
        r.counts[f.sizeShift] = count;
        r.pageCount += count;
        r.totalSize += static_cast<uint64_t>(count) * (static_cast<uint64_t>(r.basePageSize) << f.sizeShift);
    }
    return r;
}

Rsc7PageFlags DecodeRsc7PageFlags(uint32_t sysFlags, uint32_t gfxFlags) {
    Rsc7PageFlags r;
    r.sys = DecodeRsc7Pages(sysFlags);
    r.gfx = DecodeRsc7Pages(gfxFlags);
    return r;
}

}  // namespace gage::game::content
