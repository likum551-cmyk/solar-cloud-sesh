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

// The assembled pack: a name heap, one TOC entry per node (entry[0] = root dir, then one per file in
// `files` order), and a sector-aligned data blob holding the file bytes back-to-back. Offsets in the
// file entries are sector indices into `blob`. This is consumed by the (separate) game-side registrar.
struct BuiltImage {
    std::vector<TocEntry> entries;      // [0] root dir, [1..N] files, in input order
    std::vector<char>     nameHeap;     // "\0"-terminated names; root name "" first, then each file name
    std::vector<uint8_t>  blob;         // file bytes, each starting on a 512-byte sector boundary
    uint32_t              fileCount = 0;
};

// Builds a BuiltImage from `files`. Each file becomes a resource entry when it carries a valid RSC7
// header, otherwise a binary entry (q1 uncompressed-size = its own size, since we pack stored/uncompressed).
// Fails (returns false, sets err) on: no files, empty name, a file whose size exceeds the 24-bit field,
// or a blob that would need a sector offset past the 23-bit field. The output is deterministic.
bool BuildImage(const std::vector<FileSpec>& files, BuiltImage& out, std::string& err);

}  // namespace gage::game::content
