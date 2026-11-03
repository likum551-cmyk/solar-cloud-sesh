// pack_collection: the pure data layer for a Solar RP streaming content pack (e.g. the Jesko add-on).
//
// It builds an RPF7-shaped table-of-contents (name heap + 16-byte TOC entries + a sector-aligned data
// blob) for a set of loose resource/binary files, in the EXACT entry bit-packing GTA V 1.0.3889 uses.
// That packing was confirmed on 2026-11-03 against 16 live golden vectors read out of the game's own
// mounted packfiles (real vehicle-fragment entries included); see tests/pack_collection_test.cpp and
// the scratch verifier verify_entry_fmt.py. Getting a single bit wrong makes the game read garbage and
// crash, so every field here is pinned to an observed value, and the selftest reproduces those vectors.
//
// This layer makes NO game calls and touches NO game memory. It is deliberately engine-independent so
// the offline selftest can exercise it on any host. Binding a BuiltImage into the live streaming system
// (registering it as an image, AddSlot, writing the streaming-info mgr) is a separate, game-side step.
//
// Entry format (one 16-byte entry = {q0, q1}), confirmed:
//   q0 = NameOffset[bits 0..15] | FileSize24[bits 16..39] | C[bits 40..63]
//        C   = resourceFlag(bit 23) | sectorOffset(bits 0..22)
//        byteOffset = (C & 0x7fffff) << 9        (512-byte sectors)
//        NameOffset = byte offset into the name heap (a loose file's name string), NOT a data offset.
//        FileSize24 = on-disk (compressed) size in bytes.
//   Directory entry: (C & 0x7fffff) == 0x7fffff marker, FileSize24 = 0,
//        q1 = (childCount << 32) | firstChildIndex.
//   Binary file:   q1 = (0x0fefffff << 32) | uncompressedSize.
//   Resource file: C has the resource bit set; q1 = (graphicsFlags << 32) | systemFlags,
//        both taken verbatim from the file's own RSC7 header (off +8 = sys/virtual, +0xc = gfx/physical).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gage::game::content {

// ---- constants (all observed) -------------------------------------------------------------------
inline constexpr uint32_t kSectorSize   = 512;          // byte offset = sector << 9
inline constexpr uint32_t kSectorShift  = 9;
inline constexpr uint32_t kDirMarker    = 0x7fffffu;    // C & 0x7fffff for a directory entry
inline constexpr uint32_t kResourceBit  = 0x800000u;    // bit 23 of C: set => resource, clear => binary
inline constexpr uint32_t kSectorMask   = 0x7fffffu;    // 23-bit sector-offset field
inline constexpr uint32_t kBinSentinel  = 0x0fefffffu;  // q1 high dword of a binary file entry
inline constexpr uint32_t kRsc7Magic    = 0x37435352u;  // "RSC7" read little-endian from the first 4 bytes
inline constexpr uint32_t kMaxFileSize24 = 0xffffffu;   // 24-bit on-disk size field cap

// ---- the 16-byte TOC entry ----------------------------------------------------------------------
struct TocEntry {
    uint64_t q0 = 0;
    uint64_t q1 = 0;
};

// ---- field accessors (decode) -------------------------------------------------------------------
inline uint32_t EntryNameOffset(const TocEntry& e) { return static_cast<uint32_t>(e.q0 & 0xffffu); }
inline uint32_t EntryFileSize(const TocEntry& e)   { return static_cast<uint32_t>((e.q0 >> 16) & 0xffffffu); }
inline uint32_t EntryC(const TocEntry& e)          { return static_cast<uint32_t>((e.q0 >> 40) & 0xffffffu); }
inline uint32_t EntrySector(const TocEntry& e)     { return EntryC(e) & kSectorMask; }
inline uint64_t EntryByteOffset(const TocEntry& e) { return static_cast<uint64_t>(EntrySector(e)) << kSectorShift; }
inline bool     EntryIsDir(const TocEntry& e)      { return (EntryC(e) & kSectorMask) == kDirMarker && !(EntryC(e) & kResourceBit); }
inline bool     EntryIsResource(const TocEntry& e) { return !EntryIsDir(e) && (EntryC(e) & kResourceBit) != 0; }
inline uint32_t DirFirstChild(const TocEntry& e)   { return static_cast<uint32_t>(e.q1 & 0xffffffffu); }
inline uint32_t DirChildCount(const TocEntry& e)   { return static_cast<uint32_t>(e.q1 >> 32); }
inline uint32_t ResSystemFlags(const TocEntry& e)  { return static_cast<uint32_t>(e.q1 & 0xffffffffu); }
inline uint32_t ResGraphicsFlags(const TocEntry& e){ return static_cast<uint32_t>(e.q1 >> 32); }
inline uint32_t BinUncompressedSize(const TocEntry& e) { return static_cast<uint32_t>(e.q1 & 0xffffffffu); }

// ---- entry builders (encode) --------------------------------------------------------------------
// q0 = nameOffset | (fileSize << 16) | (C << 40). sector/size are assumed pre-validated by the caller
// (BuildImage validates); the accessors above are the inverse of these.
inline TocEntry MakeQ0(uint32_t nameOffset, uint32_t fileSize24, uint32_t c24) {
    TocEntry e;
    e.q0 = (static_cast<uint64_t>(nameOffset & 0xffffu))
         | (static_cast<uint64_t>(fileSize24 & 0xffffffu) << 16)
         | (static_cast<uint64_t>(c24 & 0xffffffu) << 40);
    return e;
}
inline TocEntry MakeDirEntry(uint32_t nameOffset, uint32_t firstChildIndex, uint32_t childCount) {
    TocEntry e = MakeQ0(nameOffset, 0, kDirMarker);
    e.q1 = (static_cast<uint64_t>(childCount) << 32) | firstChildIndex;
    return e;
}
inline TocEntry MakeBinaryEntry(uint32_t nameOffset, uint32_t onDiskSize, uint32_t sector,
                                uint32_t uncompressedSize) {
    TocEntry e = MakeQ0(nameOffset, onDiskSize, sector & kSectorMask);  // resource bit clear
    e.q1 = (static_cast<uint64_t>(kBinSentinel) << 32) | uncompressedSize;
    return e;
}
inline TocEntry MakeResourceEntry(uint32_t nameOffset, uint32_t onDiskSize, uint32_t sector,
                                  uint32_t systemFlags, uint32_t graphicsFlags) {
    TocEntry e = MakeQ0(nameOffset, onDiskSize, (sector & kSectorMask) | kResourceBit);
    e.q1 = (static_cast<uint64_t>(graphicsFlags) << 32) | systemFlags;
    return e;
}

// ---- RSC7 resource header (the first 16 bytes of a .yft / .ytd / .ydr loose resource) ------------
struct Rsc7Header {
    uint32_t magic = 0;         // kRsc7Magic for a resource
    uint32_t version = 0;       // resource type/version (e.g. fragType = 162 for .yft on this build)
    uint32_t systemFlags = 0;   // virtual/system page flags  -> resource entry q1 low dword
    uint32_t graphicsFlags = 0; // physical/graphics page flags -> resource entry q1 high dword
};

// Reads the 16-byte RSC7 header from the front of `data`. Returns false (and leaves out unset) when the
// buffer is shorter than 16 bytes or the magic is not "RSC7" (i.e. the file is not an RSC7 resource).
bool ParseRsc7Header(const uint8_t* data, size_t len, Rsc7Header& out);

// ---- image builder --------------------------------------------------------------------------------
// One loose file to pack. `data`/`size` are the raw on-disk bytes exactly as they sit in the loose
// folder (an RSC7 resource keeps its header; the header's flags are read, the bytes are NOT modified).
struct FileSpec {
    std::string name;            // leaf name as the game will look it up, e.g. "jesko.yft"
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// Whole 512-byte sectors needed to hold `bytes` bytes: ceil(bytes / 512). 0 bytes -> 0 sectors.
inline constexpr uint64_t SectorsFor(uint64_t bytes) { return (bytes + kSectorSize - 1) / kSectorSize; }

// Where one file landed in the image. Everything here is derived from the entry + the layout rules above;
// it is a convenience view for the game-side registrar and for the offline layout tests.
struct FilePlacement {
    uint32_t entryIndex = 0;    // index into BuiltImage::entries (>= 1; entry 0 is the root directory)
    uint32_t sector = 0;        // first sector of the file inside `blob` (== EntrySector of its entry)
    uint32_t sectorCount = 0;   // SectorsFor(size): whole sectors the file occupies (the next file starts after these)
    uint64_t byteOffset = 0;    // sector << 9: where its first byte sits in `blob` (== EntryByteOffset)
    uint32_t size = 0;          // on-disk size in bytes (== EntryFileSize)
};

// The assembled pack: the in-RAM image a custom collection will expose, as three byte arrays plus a
// placement view. `entries` is the TOC (entry[0] = root dir, then one per file in `files` order);
// `entryTable` is the same TOC serialised exactly as it would sit in memory (16 bytes per entry, stride
// 0x10, q0 at +0 then q1 at +8, both little-endian); `nameHeap` is the name string heap that every
// entry's NameOffset indexes; `blob` is the sector-aligned data holding the file bytes. Offsets in the file
// entries are sector indices into `blob`. This is consumed by the (separate) game-side registrar.
//
// Layout invariants (asserted by the offline selftest; they are properties of THIS builder, not yet proven
// against the game's own reader):
//   * placements[k].sector == 0 for k == 0, and placements[k+1].sector - placements[k].sector ==
//     placements[k].sectorCount (== ceil(size/512)): files are back-to-back, each starting on a sector.
//   * blob.size() == blobSectors * 512, and blobSectors == last.sector + last.sectorCount: the blob is a
//     whole number of sectors (the final file's tail is zero-padded to the sector boundary).
//   * the bytes of file k are exactly blob[placements[k].byteOffset .. + size); padding bytes are zero.
struct BuiltImage {
    std::vector<TocEntry>      entries;      // [0] root dir, [1..N] files, in input order
    std::vector<uint8_t>       entryTable;   // SerializeEntryTable(entries): 16 bytes per entry
    std::vector<char>          nameHeap;     // "\0"-terminated names; root name "" first, then each file name
    std::vector<uint8_t>       blob;         // file bytes, each starting on a 512-byte sector boundary
    std::vector<FilePlacement> placements;   // one per file, in input order (entryIndex == k + 1)
    uint32_t                   blobSectors = 0;  // blob.size() / 512
    uint32_t                   fileCount = 0;
};

// Serialises a TOC to its in-memory form: for each entry, q0 then q1, each as 8 little-endian bytes (so entry
// i starts at byte i * 0x10). Total, pure. ReadTocEntry is the inverse for one entry; `p` must have 16 bytes.
std::vector<uint8_t> SerializeEntryTable(const std::vector<TocEntry>& entries);
TocEntry ReadTocEntry(const uint8_t* p);

// Builds a BuiltImage from `files`. Each file becomes a resource entry when it carries a valid RSC7
// header, otherwise a binary entry (q1 uncompressed-size = its own size, since we pack stored/uncompressed).
// Fails (returns false, sets err) on: no files, empty name, a file whose size exceeds the 24-bit field,
// or a blob that would need a sector offset past the 23-bit field. The output is deterministic.
bool BuildImage(const std::vector<FileSpec>& files, BuiltImage& out, std::string& err);

// ---- RSC7 page flags (spec decode of game fn 0x1370140; NOT game-validated) ------------------------------
// A resource's system (virtual) flags and graphics (physical) flags each describe a set of memory pages. This
// is the decode exactly as documented in docs/content-streaming/r3-plan.md ("RSC7 page-flag decoder"):
//   baseShift = flags bits 0..3;  base page size = 0x200 << baseShift.
//   Page-count bit-fields, lowest bit first, each counting pages of (base page size << sizeShift):
//     bit  4      (1 bit )  sizeShift 8
//     bits 5..6   (2 bits)  sizeShift 7
//     bits 7..10  (4 bits)  sizeShift 6
//     bits 11..16 (6 bits)  sizeShift 5
//     bits 17..23 (7 bits)  sizeShift 4
//     bit  24     (1 bit )  sizeShift 3
//     bit  25     (1 bit )  sizeShift 2
//     bit  26     (1 bit )  sizeShift 1
//     bit  27     (1 bit )  sizeShift 0
//   total size = sum over the fields of count * (base page size << sizeShift); page count = sum of counts.
// The plan writes the last four fields as "bits24-27 (<<3..<<0)"; mapping them bit 24 -> <<3 ... bit 27 -> <<0
// (ascending bit, descending shift) follows the same pattern as the earlier fields but is an INTERPRETATION of
// that one line. Bits 28..31 are not mentioned by the plan and are ignored here. Both points, and the meaning
// of the two outputs of 0x1370140, must be confirmed against the game before any total is trusted.
inline constexpr int kRsc7SizeClasses = 9;   // sizeShift 0..8

struct Rsc7Pages {
    uint32_t baseShift = 0;                      // flags bits 0..3
    uint32_t basePageSize = 0;                   // 0x200 << baseShift
    uint32_t counts[kRsc7SizeClasses] = {};      // counts[s] = pages of (basePageSize << s), s = sizeShift 0..8
    uint32_t pageCount = 0;                      // sum of counts
    uint64_t totalSize = 0;                      // sum of counts[s] * (basePageSize << s)
};

struct Rsc7PageFlags {
    Rsc7Pages sys;   // decoded from the system (virtual) flags
    Rsc7Pages gfx;   // decoded from the graphics (physical) flags
};

// Decodes one flags dword / both flags of a resource. Pure, total (every input decodes).
Rsc7Pages DecodeRsc7Pages(uint32_t flags);
Rsc7PageFlags DecodeRsc7PageFlags(uint32_t sysFlags, uint32_t gfxFlags);

}  // namespace gage::game::content
