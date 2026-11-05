// pack_win.h: the Windows memory helpers shared by pack_stream.cpp and pack_enh.cpp: the game image's bounds, the
// committed-and-readable runs a pattern scan may walk, and page-checked / fault-proof reads. Moved verbatim from
// pack_stream.cpp (GameImage, ProtReadable, IsReadable, ReadableRuns); SafeRead is new.
#pragma once

#include "pack_scan.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gage::game::content::win {

struct Image { const uint8_t* base = nullptr; size_t size = 0; };

inline Image GameImage() {
    Image im;
    HMODULE m = GetModuleHandleW(nullptr);   // the game exe: this DLL runs inside its process
    if (!m) return im;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(m);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return im;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return im;
    im.base = b;
    im.size = nt->OptionalHeader.SizeOfImage;
    return im;
}

inline bool ProtReadable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

inline bool IsReadable(const void* p, size_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(p);
    const uint8_t* end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !ProtReadable(mbi.Protect))
            return false;
        a = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// The committed, readable stretches of [base, base+size), adjacent ones merged: the Arxan-packed image
// can hold guard/reserved pages, and a scan that walks into one faults the game.
inline std::vector<scan::Run> ReadableRuns(const Image& im) {
    std::vector<scan::Run> runs;
    const uint8_t* a = im.base;
    const uint8_t* end = im.base + im.size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof(mbi))) break;
        const uint8_t* rb = static_cast<const uint8_t*>(mbi.BaseAddress);
        const uint8_t* re = rb + mbi.RegionSize;
        if (re > end) re = end;
        if (mbi.State == MEM_COMMIT && ProtReadable(mbi.Protect) && re > a) {
            if (!runs.empty() && runs.back().p + runs.back().n == a)
                runs.back().n += (size_t)(re - a);
            else
                runs.push_back({ a, (size_t)(re - a) });
        }
        if (re <= a) break;
        a = re;
    }
    return runs;
}

// Copy n bytes of this process's memory, or fail without faulting: ReadProcessMemory on our own handle returns false
// where a plain load would raise an access violation (a page freed or re-protected between a VirtualQuery and the
// read). The codebase has no SEH helper (mingw-g++ has no __try); this is the same call present_hook.cpp's readBytes
// makes. A game structure the game may reallocate under us is read with this, never with a bare pointer.
inline bool SafeRead(const void* p, void* out, size_t n) {
    if (!p || !out || !n) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

// Write n bytes into this process's memory, or fail without faulting (the counterpart of SafeRead; WriteProcessMemory on our
// own handle). Round B writes only plain WORD / DWORD fields of the game's heap structures through it.
inline bool SafeWrite(void* p, const void* in, size_t n) {
    if (!p || !in || !n) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), p, in, n, &put) && put == n;
}

}  // namespace gage::game::content::win
