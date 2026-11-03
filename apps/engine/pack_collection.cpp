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
    // file entry stores the start sector index (not the byte offset).
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

    out.entries = std::move(entries);
    out.nameHeap = std::move(heap);
    out.blob = std::move(blob);
    out.fileCount = fileCount;
    return true;
}

}  // namespace gage::game::content
