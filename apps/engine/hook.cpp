// GAGE-MP in-game engine: game hook primitives — implementation.
//
// Two mechanisms, each resolved independently:
//
//   1) D3D11 Present hook (build-independent): a dummy D3D11 device yields the
//      Present function address in dxgi.dll; an inline E9 detour runs onTick
//      every frame on the game's main/render thread. allocNear() guarantees the
//      detour thunk is within ±2 GB of Present (cross-module).
//
//   2) RAGE native invocation: a single byte-signature locates the 256-bucket
//      XOR-obfuscated native registration table. Every game primitive (ped
//      spawn, position, heading, model loading, entity lifecycle) is called by
//      its 64-bit native hash through scrNativeCallContext. No per-primitive
//      signatures are needed.
//
// Each mechanism degrades independently: if D3D11 Present is not found the tick
// hook is not installed; if the native table is not found the entity API returns
// zero/false. Nothing aborts the engine.
#include "hook.h"
#include "sp_scripts.h"
#include "net_guard.h"
#include "present_hook.h"
#include "nosp_pick.h"
#include "pack_enh.h"

#include <windows.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <intrin.h>

#include "common/logger.h"

using namespace gage::game;

// Defined with the native-table poller below; the poller helpers and World use it first.
extern std::atomic<bool> g_nativeTableReady;

namespace {

// ---------------------------------------------------------------------------
// x86_64 instruction-length disassembler
// ---------------------------------------------------------------------------
// Returns the length of the single instruction at `p` (which has at least n
// bytes available); also reports, via ripDispOff, the byte offset of a
// rip-relative disp32 within the instruction (or -1 when not one). This is the
// classic length-disassembler approach: prefixes, opcode (incl. 0x0F escape),
// ModRM/SIB, then immediate. Good enough for every prologue instruction a
// trampoline copies.

bool insPrefix(uint8_t b) {
    return b == 0xF0 || b == 0xF3 || b == 0xF2 || b == 0x2E || b == 0x36 ||
           b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 || b == 0x66 ||
           b == 0x67;
}

// x86-64 instruction length decoder, enough for function prologues: legacy
// prefixes, REX, one-byte, 0F, 0F 38 and 0F 3A opcodes, ModRM/SIB/displacement
// and immediates. VEX/EVEX and opcodes invalid in 64-bit mode return false, so
// the caller refuses to hook instead of copying a wrong length (the old table
// decoder read the opcode as the ModRM byte: `48 89 5C 24 10` came out as 6
// bytes, `55` failed, and the Present trampoline jumped back mid-instruction).

// One-byte opcode: does it take a ModRM byte, how many immediate bytes (before
// operand-size and REX.W adjustments), and is it valid in 64-bit mode.
struct Op1 { bool modrm; int imm; bool valid; };

Op1 op1Info(uint8_t op) {
    if (op < 0x40) {                          // ALU block: 8 groups of 8
        const uint8_t lo = op & 7;
        if (op == 0x0F) return {false, 0, false};          // escape, handled by caller
        if (lo <= 3) return {true, 0, true};               // r/m forms
        if (lo == 4) return {false, 1, true};              // AL, imm8
        if (lo == 5) return {false, 4, true};              // eAX, imm32
        return {false, 0, false};                          // 06/07/0E/16/17/1E/1F/27/2F/37/3F, prefixes
    }
    if (op < 0x50) return {false, 0, false};               // REX, handled by caller
    if (op < 0x60) return {false, 0, true};                // push/pop r64
    switch (op) {
    case 0x63: return {true, 0, true};                     // movsxd
    case 0x68: return {false, 4, true};                    // push imm32
    case 0x69: return {true, 4, true};                     // imul r, r/m, imm32
    case 0x6A: return {false, 1, true};                    // push imm8
    case 0x6B: return {true, 1, true};                     // imul r, r/m, imm8
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: return {false, 0, true};
    default: break;
    }
    if (op < 0x70) return {false, 0, false};               // 60-62 invalid, 64-67 prefixes
    if (op < 0x80) return {false, 1, true};                // jcc rel8
    switch (op) {
    case 0x80: case 0x83: return {true, 1, true};
    case 0x81: return {true, 4, true};
    case 0x82: return {false, 0, false};
    default: break;
    }
    if (op < 0x90) return {true, 0, true};                 // 84-8F: test/xchg/mov/lea/pop r/m
    if (op == 0x9A) return {false, 0, false};
    if (op < 0xA0) return {false, 0, true};                // nop/xchg/cwde/cdq/fwait/pushf/popf/sahf/lahf
    if (op < 0xA4) return {false, 8, true};                // mov moffs64
    if (op == 0xA8) return {false, 1, true};
    if (op == 0xA9) return {false, 4, true};
    if (op < 0xB0) return {false, 0, true};                // string ops
    if (op < 0xB8) return {false, 1, true};                // mov r8, imm8
    if (op < 0xC0) return {false, 4, true};                // mov r32, imm32 (imm64 with REX.W)
    switch (op) {
    case 0xC0: case 0xC1: case 0xC6: return {true, 1, true};
    case 0xC2: case 0xCA: return {false, 2, true};
    case 0xC3: case 0xC9: case 0xCB: case 0xCC: case 0xCF: return {false, 0, true};
    case 0xC4: case 0xC5: case 0xCE: return {false, 0, false};   // VEX / into
    case 0xC7: return {true, 4, true};
    case 0xC8: return {false, 3, true};                    // enter imm16, imm8
    case 0xCD: return {false, 1, true};
    default: break;
    }
    if (op < 0xD4) return {true, 0, true};                 // shift group 2
    if (op < 0xD7) return {false, 0, false};
    if (op == 0xD7) return {false, 0, true};               // xlat
    if (op < 0xE0) return {true, 0, true};                 // x87
    if (op < 0xE8) return {false, 1, true};                // loop/jrcxz rel8, in/out imm8
    if (op == 0xE8 || op == 0xE9) return {false, 4, true}; // call/jmp rel32
    if (op == 0xEA) return {false, 0, false};
    if (op == 0xEB) return {false, 1, true};               // jmp rel8
    if (op < 0xF0) return {false, 0, true};                // in/out dx
    switch (op) {
    case 0xF1: case 0xF4: case 0xF5: return {false, 0, true};
    case 0xF6: case 0xF7: return {true, 0, true};          // imm depends on ModRM.reg
    case 0xFE: case 0xFF: return {true, 0, true};
    default: break;
    }
    if (op >= 0xF8) return {false, 0, true};               // clc/stc/cli/sti/cld/std
    return {false, 0, false};                              // F0/F2/F3 prefixes
}

// Two-byte (0F xx) opcode: ModRM and imm8. 0F 38 / 0F 3A are handled by the caller.
Op1 op0FInfo(uint8_t op) {
    if (op >= 0x80 && op <= 0x8F) return {false, 4, true};             // jcc rel32
    if (op >= 0xC8 && op <= 0xCF) return {false, 0, true};             // bswap
    switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x37:
    case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return {false, 0, true};
    case 0x0F: case 0x04: case 0x0A: case 0x0C: case 0x36: case 0x39: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0xFF:
        return {false, 0, false};                                      // 3DNow!, undefined
    case 0x70: case 0x71: case 0x72: case 0x73: case 0xA4: case 0xAC: case 0xBA:
    case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        return {true, 1, true};
    default:
        return {true, 0, true};
    }
}

bool decIns(const uint8_t* p, size_t n, size_t& len, int& ripDispOff) {
    len = 0;
    ripDispOff = -1;

    // Legacy prefixes (at most 4), then at most one REX, which must come last.
    bool opsize = false, addrsize = false;
    while (len < n && len < 4 && insPrefix(p[len])) {
        if (p[len] == 0x66) opsize = true;
        if (p[len] == 0x67) addrsize = true;
        ++len;
    }
    bool rexW = false;
    if (len < n && (p[len] & 0xF0) == 0x40) {
        rexW = (p[len] & 0x08) != 0;
        ++len;
    }
    if (len >= n) return false;

    const uint8_t op = p[len++];
    Op1 info;
    if (op == 0x0F) {
        if (len >= n) return false;
        const uint8_t op2 = p[len++];
        if (op2 == 0x38 || op2 == 0x3A) {     // three-byte maps: always ModRM
            if (len >= n) return false;
            ++len;
            info = {true, op2 == 0x3A ? 1 : 0, true};
        } else {
            info = op0FInfo(op2);
        }
    } else {
        info = op1Info(op);
        // Operand-size and REX.W adjustments of the one-byte immediates.
        if (op >= 0xB8 && op <= 0xBF) info.imm = rexW ? 8 : (opsize ? 2 : 4);
        else if (info.imm == 4 && opsize && op != 0xE8 && op != 0xE9) info.imm = 2;
        if (op >= 0xA0 && op <= 0xA3 && addrsize) info.imm = 4;
    }
    if (!info.valid) return false;

    if (info.modrm) {
        if (len >= n) return false;
        const uint8_t modrm = p[len++];
        const uint8_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if ((op == 0xF6 || op == 0xF7) && reg <= 1)          // test r/m, imm
            info.imm = op == 0xF6 ? 1 : (opsize ? 2 : 4);
        if (mod != 3 && rm == 4) {                          // SIB
            if (len >= n) return false;
            const uint8_t sib = p[len++];
            if (mod == 0 && (sib & 7) == 5) len += 4;       // disp32, no base
        }
        if (mod == 0 && rm == 5) {                          // [rip+disp32]
            ripDispOff = (int)len;
            len += 4;
        } else if (mod == 1) {
            len += 1;
        } else if (mod == 2) {
            len += 4;
        }
    }
    len += (size_t)info.imm;
    return len <= n;
}

// True when the `len`-byte instruction at `p` (length from decIns) is a CALL: E8 rel32, FF /2
// (call r/m64) or FF /3 (far call m16:64), behind the same legacy prefixes and REX decIns skips.
// Its return address is p + len, which is what an overwrite of the range must not cut into.
bool insIsCall(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i < len && i < 4 && insPrefix(p[i])) ++i;
    if (i < len && (p[i] & 0xF0) == 0x40) ++i;
    if (i >= len) return false;
    if (p[i] == 0xE8) return true;
    if (p[i] != 0xFF || i + 1 >= len) return false;
    const uint8_t reg = (p[i + 1] >> 3) & 7;
    return reg == 2 || reg == 3;
}

// Rebase the rel32 at `off` of a `len`-byte instruction copied from `src` to
// `dst`, so it still reaches the same target. False when that target is out of
// rel32 reach from `dst`.
bool rebaseRel32(uint8_t* dst, const uint8_t* src, size_t len, size_t off) {
    int32_t disp = 0;
    memcpy(&disp, src + off, 4);
    const int64_t target = (int64_t)(intptr_t)(src + len) + disp;
    const int64_t moved = target - (int64_t)(intptr_t)(dst + len);
    if (moved < INT32_MIN || moved > INT32_MAX) return false;
    const int32_t d32 = (int32_t)moved;
    memcpy(dst + off, &d32, 4);
    return true;
}

// Copy whole instructions at `src` totalling at least `minBytes` bytes into
// `dst` (with room for `cap` bytes), fixing rip-relative displacements and
// jmp/call rel32 targets for the relocation `src -> dst`. Returns the number of
// bytes copied, or 0 when the prologue cannot be relocated safely (a short or
// conditional branch, or a target out of rel32 reach): no hook beats a broken one.
size_t copyPrologue(uint8_t* dst, const uint8_t* src, size_t minBytes, size_t cap) {
    size_t copied = 0;
    size_t insLen = 0;
    while (copied < minBytes && copied + insLen < cap) {
        int rip = -1;
        if (!decIns(src + copied, cap - copied, insLen, rip)) {
            break;                            // can't decode a whole instr
        }
        const uint8_t* s = src + copied;
        uint8_t* d = dst + copied;
        size_t pre = 0;
        while (pre < insLen && (insPrefix(s[pre]) || (s[pre] >= 0x40 && s[pre] <= 0x4F)))
            ++pre;
        const uint8_t op = s[pre];
        // rel8 branches cannot reach back from the trampoline, and 0F 8x (Jcc
        // rel32) is not decoded correctly by decIns: refuse rather than guess.
        if (op == 0xEB || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) ||
            (op == 0x0F && pre + 1 < insLen && (s[pre + 1] & 0xF0) == 0x80)) {
            GLOGW("hook: prologue at %p starts with a branch (opcode %02x) that cannot be relocated", s, op);
            return 0;
        }
        memcpy(d, s, insLen);
        // An E9/E8 here is usually another overlay's hook (Steam's hooks Present
        // when any device is created, our probe's included). Its rel32 is not a
        // ModRM operand, so decIns does not flag it; it must move all the same.
        const bool rel32Branch = (op == 0xE9 || op == 0xE8);
        if (rip >= 0 || rel32Branch) {
            const size_t off = rip >= 0 ? (size_t)rip : pre + 1;
            if (!rebaseRel32(d, s, insLen, off)) {
                GLOGW("hook: prologue at %p: relocated %s target out of rel32 reach",
                      s, rel32Branch ? "branch" : "rip-relative");
                return 0;
            }
        }
        copied += insLen;
    }
    return copied;
}

// ---------------------------------------------------------------------------
// Signature scan
// ---------------------------------------------------------------------------
struct Sig {
    const char* name;
    const uint8_t* bytes;
    const uint8_t* mask;      // 1 = match, 0 = wildcard
    int  len;
};

const uint8_t* findSig(const uint8_t* base, size_t size, const Sig& s) {
    const uint8_t* end = base + size - (size_t)s.len;
    for (const uint8_t* p = base; p <= end; ++p) {
        bool ok = true;
        for (int i = 0; i < s.len; ++i) {
            if (s.mask[i] && p[i] != s.bytes[i]) { ok = false; break; }
        }
        if (ok) return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Inline-hook state (single hooked site at a time)
// ---------------------------------------------------------------------------
struct HookState {
    uint8_t*  target = nullptr;       // original function address (patched)
    uint8_t*  trampoline = nullptr;   // RWX page: copied prologue + jmp back
    uint8_t*  detour = nullptr;       // RWX page: save regs, call onTick, jmp tramp
    uint8_t   savedBytes[64] = {};    // original prologue (pre-patch), for restore
    uint8_t   savedJump[16] = {};     // the original patched region (incl. E9)
    int       patchedLen = 0;
    int       savedJumpLen = 0;
    bool      active = false;
};
HookState g_hook;
void (*g_onTick)() = nullptr;         // called by the inline detour each frame (present_mode=0)
// The IDXGISwapChain* of the latest Present lives in presenthook::SwapChainStorage(): the vtable detour and the
// inline thunk (RCX on entry) both write it.

// Allocate an RWX page within ±1 GB of `nearAddr` for cross-module E9 jmp.
// Falls back to any-address allocation if no nearby region is found.
void* allocNear(size_t bytes, void* nearAddr) {
    if (!nearAddr)
        return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    uintptr_t target = (uintptr_t)nearAddr;
    // Search below, then above, within ±1 GB.
    const uintptr_t range = 0x40000000ULL; // 1 GB
    const size_t granularity = 0x10000;     // 64 KB allocation granularity

    uintptr_t lo = target > range ? (target - range) : 0x10000;
    uintptr_t hi = target + range;

    // Try above first (less likely to conflict with loaded modules).
    for (uintptr_t addr = (target + granularity) & ~(uintptr_t)(granularity - 1);
         addr < hi; addr += granularity)
    {
        void* p = VirtualAlloc((void*)addr, bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    // Try below.
    for (uintptr_t addr = (target - granularity) & ~(uintptr_t)(granularity - 1);
         addr >= lo && addr < target; addr -= granularity)
    {
        void* p = VirtualAlloc((void*)addr, bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    // Fallback: any address, use 14-byte absolute jmp instead of E9.
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

void* allocRWX(size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

bool flushRWX(void* p, size_t bytes) {
    DWORD old = 0;
    if (!VirtualProtect(p, bytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), p, bytes);
    return true;
}

// Build the detour thunk:
//   [movabs rax, <rcxSave>; mov [rax], rcx]   (optional: keep the first argument)
//   push rax; push rcx; push rdx; push rsi; push rdi; push r8..r11;
//   sub rsp,0x20 (the callee's shadow space, so it cannot overwrite the saved r8..r11);
//   movabs rax, <cbSlot>; mov rax,[rax]; test rax,rax; jz skip; call rax;
//   skip: add rsp,0x20; pop r11..; jmp <tramp>;
// Entry rsp is 8 mod 16; nine pushes and the 0x20 leave it 16-aligned at the call.
// Every volatile GPR is restored, so the hooked function sees its own arguments.
bool makeDetour(uint8_t* d, void* tramp, void** rcxSave, void* const* cbSlot) {
    size_t o = 0;

    if (rcxSave) {
        // movabs rax, rcxSave
        d[o++] = 0x48; d[o++] = 0xB8;
        uint64_t scpAddr = (uint64_t)rcxSave;
        memcpy(d + o, &scpAddr, 8); o += 8;
        // mov [rax], rcx
        d[o++] = 0x48; d[o++] = 0x89; d[o++] = 0x08;
    }

    const uint8_t pushes[] = {0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
    memcpy(d + o, pushes, sizeof(pushes)); o += sizeof(pushes);
    // sub rsp, 0x20
    d[o++] = 0x48; d[o++] = 0x83; d[o++] = 0xEC; d[o++] = 0x20;
    // movabs rax, cbSlot
    d[o++] = 0x48; d[o++] = 0xB8;
    uint64_t onTickAddr = (uint64_t)cbSlot;
    memcpy(d + o, &onTickAddr, 8); o += 8;
    // mov rax, [rax]
    d[o++] = 0x48; d[o++] = 0x8B; d[o++] = 0x00;
    // test rax, rax
    d[o++] = 0x48; d[o++] = 0x85; d[o++] = 0xC0;
    // jz +2
    d[o++] = 0x74; d[o++] = 0x02;
    // call rax
    d[o++] = 0xFF; d[o++] = 0xD0;
    // add rsp, 0x20 (the jz above lands here)
    d[o++] = 0x48; d[o++] = 0x83; d[o++] = 0xC4; d[o++] = 0x20;
    // pops (reverse order)
    const uint8_t pops[] = {0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5F,0x5E,0x5A,0x59,0x58};
    memcpy(d + o, pops, sizeof(pops)); o += sizeof(pops);

    // jmp to trampoline: try E9 if within ±2 GB, otherwise 14-byte absolute.
    intptr_t delta = (intptr_t)tramp - (intptr_t)(d + o + 5);
    if (delta >= INT32_MIN && delta <= INT32_MAX) {
        d[o++] = 0xE9;
        int32_t rel = (int32_t)delta;
        memcpy(d + o, &rel, 4); o += 4;
    } else {
        // FF 25 00 00 00 00 [8-byte addr] — jmp qword ptr [rip+0]
        d[o++] = 0xFF; d[o++] = 0x25;
        int32_t zero = 0;
        memcpy(d + o, &zero, 4); o += 4;
        uint64_t abs = (uint64_t)(uintptr_t)tramp;
        memcpy(d + o, &abs, 8); o += 8;
    }
    return flushRWX(d, o);
}

// ---------------------------------------------------------------------------
// RAGE scrNativeCallContext
// ---------------------------------------------------------------------------
// The layout of the context struct passed to every native handler.
// +0x00 returnValue (ptr)   +0x08 argCount (u32)   +0x0C pad
// +0x10 args (ptr)          +0x18 dataCount (i32)   +0x1C pad
// +0x20 vectorSpace[192]
#pragma pack(push, 1)
struct scrNativeCallContext {
    uint64_t* returnValue;  // 0x00
    uint32_t  argCount;     // 0x08
    uint32_t  pad0C;        // 0x0C
    uint64_t* args;         // 0x10
    int32_t   dataCount;    // 0x18
    int32_t   pad1C;        // 0x1C
    uint8_t   vectorSpace[192]; // 0x20
};
#pragma pack(pop)
static_assert(offsetof(scrNativeCallContext, returnValue) == 0x00, "returnValue at 0x00");
static_assert(offsetof(scrNativeCallContext, argCount)    == 0x08, "argCount at 0x08");
static_assert(offsetof(scrNativeCallContext, args)        == 0x10, "args at 0x10");
static_assert(offsetof(scrNativeCallContext, dataCount)   == 0x18, "dataCount at 0x18");
static_assert(offsetof(scrNativeCallContext, vectorSpace) == 0x20, "vectorSpace at 0x20");

using NativeHandler = void(*)(scrNativeCallContext*);

// ---------------------------------------------------------------------------
// NativeRegistration XOR-obfuscated table helpers
// ---------------------------------------------------------------------------
// The 256-bucket table uses XOR encoding on the `next` pointer and the entry
// count. Handlers and hashes are stored PLAIN (not XOR-encoded).
//
// Struct layout (b1604+, 0x88 bytes per node):
//   +0x00: uint64_t nextRegistration_a  }  XOR pair → next = a ^ b
//   +0x08: uint64_t nextRegistration_b  }
//   +0x10: NativeHandler handlers[7]     (7 × 8 = 56 bytes, PLAIN pointers)
//   +0x48: uint32_t numEntries_a         }  XOR pair → count = a ^ b
//   +0x4C: uint32_t numEntries_b         }
//   +0x50: uint64_t hashes[7]            (7 × 8 = 56 bytes, PLAIN hashes)

// All field access is by raw byte offsets — no struct definition needed.
inline uint8_t* regGetNext(uint8_t* reg) {
    uint64_t a, b;
    memcpy(&a, reg + 0x00, 8);
    memcpy(&b, reg + 0x08, 8);
    return (uint8_t*)(uintptr_t)(a ^ b);
}

inline uint32_t regGetCount(uint8_t* reg) {
    uint32_t a, b;
    memcpy(&a, reg + 0x48, 4);
    memcpy(&b, reg + 0x4C, 4);
    return a ^ b;
}

inline NativeHandler regGetHandler(uint8_t* reg, uint32_t index) {
    uint64_t h;
    memcpy(&h, reg + 0x10 + index * 8, 8);
    return (NativeHandler)(uintptr_t)h;
}

inline uint64_t regGetHash(uint8_t* reg, uint32_t index) {
    // Hashes are stored PLAIN (not XOR-encoded) at +0x50, 8 bytes each.
    uint64_t h;
    memcpy(&h, reg + 0x50 + index * 8, 8);
    return h;
}

// Two node layouts: Plain is the one above (as first assumed here). AddrXor is
// NativeRegistrationNew, public since 1.42: handlers plain at +0x10, but next,
// count and every hash XORed with their own address, and hashes in 16-byte
// slots from +0x54 (pastebin.com/6X5QCke2). The poller validates both and keeps
// whichever resolves the probe hashes.
enum class NodeLayout { Plain = 0, AddrXor = 1 };
NodeLayout g_nodeLayout = NodeLayout::Plain;   // set by the poller before g_nativeTableReady

const char* layoutName(NodeLayout l) { return l == NodeLayout::AddrXor ? "addr-xor" : "plain"; }
size_t nodeSize(NodeLayout l) { return l == NodeLayout::AddrXor ? 0x54 + 7 * 16 : 0x88; }

uint8_t* nodeNext(uint8_t* reg, NodeLayout l) {
    if (l == NodeLayout::Plain) return regGetNext(reg);
    uint32_t d0, d1;
    uint64_t n2;
    memcpy(&d0, reg, 4);
    memcpy(&d1, reg + 4, 4);
    memcpy(&n2, reg + 8, 8);
    const uint32_t key = (uint32_t)((uintptr_t)reg ^ n2);
    return (uint8_t*)(uintptr_t)(((uint64_t)(key ^ d1) << 32) | (uint32_t)(key ^ d0));
}

uint32_t nodeCount(uint8_t* reg, NodeLayout l) {
    if (l == NodeLayout::Plain) return regGetCount(reg);
    uint32_t a, b;
    memcpy(&a, reg + 0x48, 4);
    memcpy(&b, reg + 0x4C, 4);
    return (uint32_t)(uintptr_t)(reg + 0x48) ^ a ^ b;
}

uint64_t nodeHash(uint8_t* reg, uint32_t i, NodeLayout l) {
    if (l == NodeLayout::Plain) return regGetHash(reg, i);
    uint8_t* p = reg + 0x54 + 16 * i;
    uint32_t lo, hi, k;
    memcpy(&lo, p, 4);
    memcpy(&hi, p + 4, 4);
    memcpy(&k, p + 8, 4);
    const uint32_t key = (uint32_t)(uintptr_t)p ^ k;
    return ((uint64_t)(key ^ hi) << 32) | (uint32_t)(key ^ lo);
}

// ---------------------------------------------------------------------------
// Native table globals
// ---------------------------------------------------------------------------
uint8_t** g_nativeTable = nullptr;   // the 256-bucket registration table (validated)
std::unordered_map<uint64_t, NativeHandler> g_nativeCache;

// Build 1.0.3889 registers most natives under re-hashed values: a full table dump
// held 6701 entries but only 2 of 10 well-known public NativeDB hashes. Public ->
// 3889 hash for every native the engine calls, from Mr-X-GTA/YimMenu
// (GTA_VERSION_TARGET "1.73-3889.0") invoker/crossmap.hpp indexed through
// natives.hpp; spot-checked against a live 3889 table dump (GET/SET_ENTITY_HEADING,
// DELETE_ENTITY, SET_MODEL_AS_NO_LONGER_NEEDED found there one-for-one).
struct HashMapping { uint64_t pub, b3889; };
const HashMapping kCrossmap3889[] = {
    {native::PLAYER_PED_ID,                       0x4A8C381C258A124D},
    {native::CREATE_PED,                          0xB1DBFEB95C0EFB88},
    {native::SET_PED_DEFAULT_COMPONENT_VARIATION, 0x77EFA99E6A8FFC43},
    {native::IS_PED_DEAD_OR_DYING,                0x89568FA9A6BC0B4A},
    {native::GET_ENTITY_COORDS,                   0xD1A6A821F5AC81DB},
    {native::SET_ENTITY_COORDS,                   0xB2BD5837A8D3CEDA},
    {native::GET_ENTITY_HEADING,                  0xCFC0C995455A6204},
    {native::SET_ENTITY_HEADING,                  0x5C96CEA06531AB03},
    {native::DELETE_ENTITY,                       0x81A7F3CD719DD53B},
    {native::SET_ENTITY_VISIBLE,                  0x4285E11B28063EE0},
    {native::SET_ENTITY_INVINCIBLE,               0x935364B4448CD584},
    {native::FREEZE_ENTITY_POSITION,              0x5D7CD709B34C90F0},
    {native::REQUEST_MODEL,                       0xEC9DAA34BBB4658C},
    {native::HAS_MODEL_LOADED,                    0x6252BC0DD8A320DB},
    {native::SET_MODEL_AS_NO_LONGER_NEEDED,       0x55098D9E9AD58806},
    {native::SET_RANDOM_EVENT_FLAG,               0x7EA65A7D5C6CBDB5},
    {native::SET_RANDOM_TRAINS,                   0xAE03F542B985A69E},
    {native::SET_GARBAGE_TRUCKS,                  0x474491073FE815A8},
    {native::SET_CREATE_RANDOM_COPS,              0xD682DD0578BF5392},
    {native::PLAYER_ID,                           0x259BE71D8A81D4FA},
    {native::SET_PLAYER_MODEL,                    0x52E0301351FCDEC5},
    {native::SET_MAX_WANTED_LEVEL,                0xDAE61414743C8D1D},
    {native::CLEAR_PLAYER_WANTED_LEVEL,           0x3C482AC51A8E85DC},
    // story-mode evasion (0.1.52): dispatch/cops, character wheel, HUD, autosave blocking.
    {native::SET_MISSION_FLAG,                        0x925970A93719CADE},
    {native::CLEAR_CODE_REQUESTED_AUTOSAVE,           0xF7F21AEA9B22B838},
    {native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME,    0xD13237BC328B938E},
    {native::ENABLE_DISPATCH_SERVICE,                 0xCC1C92F7E1A3CE9D},
    {native::SET_CREATE_RANDOM_COPS_NOT_ON_SCENARIOS, 0xD5E2F27BCC913BE9},
    {native::SET_CREATE_RANDOM_COPS_ON_SCENARIOS,     0x931C6A48678F5793},
    {native::SET_DISPATCH_COPS_FOR_PLAYER,            0xE383E18054CA323B},
    {native::SET_POLICE_IGNORE_PLAYER,                0xDAA51A56DBEC0391},
    {native::SET_WANTED_LEVEL_MULTIPLIER,             0x425BBE19F25A57AB},
    {native::SET_PLAYER_CAN_BE_HASSLED_BY_GANGS,      0x9856853A6FCF4F85},
    {native::SET_POLICE_RADAR_BLIPS,                  0x7AD72DFA04253E47},
    {native::DISABLE_CONTROL_ACTION,                  0x66EFB3D6110055C4},
    {native::DISPLAY_RADAR,                           0x37B894853929BF1A},
    {native::DISPLAY_HUD,                             0x747786364137DC63},
    {native::GET_PLAYER_WANTED_LEVEL,                 0xE7B45027762DEFE7},
    {native::DISABLE_PLAYER_VEHICLE_REWARDS,          0x706D6DF51D119A27},
    // self-driven respawn (0.1.53): death fade/restart control, revive, collision streaming.
    {native::PAUSE_DEATH_ARREST_RESTART,              0xD43B9D1692F5C06E},
    {native::IGNORE_NEXT_RESTART,                     0x72B1E2693AC30407},
    {native::SET_FADE_OUT_AFTER_DEATH,                0x31E8D1058586E006},
    {native::SET_FADE_OUT_AFTER_ARREST,               0x332262819D51DF61},
    {native::SET_FADE_IN_AFTER_DEATH_ARREST,          0x40AC02FA167D4D0A},
    {native::FORCE_GAME_STATE_PLAYING,                0xDA91D9D96ED0EB8F},
    {native::RESET_PLAYER_ARREST_STATE,               0x3C2C878E6683CE75},
    {native::RESURRECT_PED,                           0x6ED737C2A74E181D},
    {native::SET_ENTITY_HEALTH,                       0xD25E9BDC14A0B649},
    {native::GET_ENTITY_MAX_HEALTH,                   0xF8A78594664D23A6},
    {native::CLEAR_PED_TASKS_IMMEDIATELY,             0x19626F992DC71FB9},
    {native::CLEAR_PED_BLOOD_DAMAGE,                  0x8EA9C5E0178372E1},
    {native::RESET_PED_VISIBLE_DAMAGE,                0x69AE13B08EFD8497},
    {native::IS_SCREEN_FADED_OUT,                     0x15CCE8886267624F},
    {native::DO_SCREEN_FADE_IN,                       0x10B228D2FDB7AF16},
    {native::DO_SCREEN_FADE_OUT,                      0x8F72AF14CE5AACE4},
    {native::SET_TIME_SCALE,                          0xE6AC149D1121535D},
    {native::ANIMPOSTFX_STOP_ALL,                     0x7919995BD2B8745C},
    {native::SET_CAM_DEATH_FAIL_EFFECT_STATE,         0x735D248F6CA672AE},
    {native::STOP_AUDIO_SCENE,                        0xB43467C43086A6A1},
    {native::REQUEST_COLLISION_AT_COORD,              0xEA2D52183C7EA9CF},
    {native::HAS_COLLISION_LOADED_AROUND_ENTITY,      0x5E22EA3310AF52EA},
    // clean no-Prologue boot reveal: dismiss GTA's loading screen, hand control to the player.
    {native::SHUTDOWN_LOADING_SCREEN,                 0xCD17096A98584C2B},
    {native::SET_PLAYER_CONTROL,                      0x4686BC3BFDBB5348},
    // HUD bars + client-side commands (0.1.54).
    {native::DRAW_RECT,                               0x81645EE95A114FAE},
    {native::GET_ENTITY_HEALTH,                       0x8D91ADE44AC79BC9},
    {native::GET_HASH_KEY,                            0x70E57E9927B6BA58},
    {native::IS_MODEL_IN_CDIMAGE,                     0xE7D342E0F16AAA8F},
    {native::IS_MODEL_A_VEHICLE,                      0xAD1840C2E6AF7D5E},
    {native::CREATE_VEHICLE,                          0x5779387E956077A6},
    {native::SET_PED_INTO_VEHICLE,                    0x73CAFD2038E812B3},
    {native::SET_VEHICLE_ON_GROUND_PROPERLY,          0x1DE99C193C7EC64B},
    {native::SET_VEHICLE_ENGINE_ON,                   0xC229299217554C78},
    {native::GET_VEHICLE_PED_IS_IN,                   0x6EF03BE64E058E2F},
    {native::SET_VEHICLE_FIXED,                       0xF698038C13845696},
    {native::SET_VEHICLE_DEFORMATION_FIXED,           0x1D1124C855316790},
    {native::GIVE_WEAPON_TO_PED,                      0xB41DEC3AAC1AA107},
    // speedometer (0.1.55).
    {native::IS_PED_IN_ANY_VEHICLE,                   0x7F420695E3F776FB},
    {native::GET_ENTITY_SPEED,                        0xDF93B3CFAC96698F},
    // notifications (0.2.0): hide the phone/SMS/notification feed.
    {native::THEFEED_HIDE_THIS_FRAME,                 0x0C9B2F8C2BD128C2},
    {native::THEFEED_FLUSH_QUEUE,                     0xC138265FD0CDEA4E},
    // world clean-up: ambient density, parked cars, boats, area clears.
    {native::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME,            0xF9A2335AB37CF17E},
    {native::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME,   0x0397A00D015A11D4},
    {native::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME,        0xA0265306DFF63938},
    {native::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, 0x23D563236A543309},
    {native::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, 0x40C1C94D5A5157C5},
    {native::SET_NUMBER_OF_PARKED_VEHICLES,                    0xECDFDC2E8AC2D613},
    {native::SET_RANDOM_BOATS,                                 0xBB7BF0D30DB04384},
    {native::CLEAR_AREA_OF_PEDS,                               0x55F7AC4B2B875901},
    {native::CLEAR_AREA_OF_COPS,                               0x8B0110C1F1D9D177},
    {native::REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA,          0xC4BCE90F7242F354},
    // T5 ambient sweep natives (population budgets, generators, trains, nearby peds/vehicles): rows are further down, under 0.4.7.
    // join readiness gate + model verify.
    {native::GET_IS_LOADING_SCREEN_ACTIVE,                     0xDCE42B3C644D1A4E},
    {native::IS_SCREEN_FADED_IN,                               0x9390801B06EE998F},
    {native::IS_PLAYER_SWITCH_IN_PROGRESS,                     0x3555462DB47B7AB1},
    {native::IS_PLAYER_CONTROL_ON,                             0xE916D57851F785AB},
    {native::GET_ENTITY_MODEL,                                 0x4B423FAA24E8ABF0},
    // blips (ClearSpBlips), from the YimMenu crossmap.
    {native::GET_FIRST_BLIP_INFO_ID,                           0xD56419CB9E15983F},
    {native::GET_NEXT_BLIP_INFO_ID,                            0xA3F6143A8F610118},
    {native::GET_BLIP_SPRITE,                                  0x8FACE824AEC046E8},
    {native::REMOVE_BLIP,                                      0xFE54B8568B2ABD12},
    {native::DOES_BLIP_EXIST,                                  0xC450B06E5AAA0985},
    // 0.4.20 map clean-up: peddrive lookup tool, YimMenu b3889 crossmap and YimMenuV2 enhanced AGREE for all 12.
    {native::GET_BLIP_INFO_ID_DISPLAY,                         0xC4BDFF590B6DE7A5},
    {native::SET_BLIP_DISPLAY,                                 0xF55F62DA99DB0C2F},
    {native::GET_MAIN_PLAYER_BLIP_ID,                          0x413A91F497E3428F},
    {native::GET_NORTH_BLID_INDEX,                             0x8999FC36EFA332BC},
    {native::GET_WAYPOINT_BLIP_ENUM_ID,                        0x2A3612A4B836469E},
    {native::GET_NUMBER_OF_ACTIVE_BLIPS,                       0xC802478C8862BDAD},
    {native::SET_THIS_SCRIPT_CAN_REMOVE_BLIPS_CREATED_BY_ANY_SCRIPT, 0x9F83BF77C7204C05},
    {native::IS_WAYPOINT_ACTIVE,                               0x02213DC34A224533},
    {native::SET_WAYPOINT_OFF,                                 0xA4C1E1845880C098},
    {native::SET_MINIMAP_HIDE_FOW,                             0x84DE06FB962FF36D},
    {native::GET_MINIMAP_FOW_DISCOVERY_RATIO,                  0x6FF69BB93DBFAF25},
    {native::GET_MINIMAP_FOW_COORDINATE_IS_REVEALED,           0x9A761B33F5EF75C5},
    // 0.4.22 minimap: peddrive lookup tool (natlookup.mjs), YimMenu b3889 crossmap and YimMenuV2 enhanced AGREE for all 11.
    {native::IS_RADAR_HIDDEN,                                  0xBE259DBA45F2996E},
    {native::IS_MINIMAP_RENDERING,                             0x6DA5456258FF887A},
    {native::IS_RADAR_PREFERENCE_SWITCHED_ON,                  0xC1FF684795F27005},
    {native::IS_HUD_HIDDEN,                                    0xC5D15A335232A15D},
    {native::REQUEST_SCALEFORM_MOVIE,                          0x8DE4F68A9728925E},
    {native::HAS_SCALEFORM_MOVIE_LOADED,                       0xA0C7B98BCF1EEF9E},
    {native::BEGIN_SCALEFORM_MOVIE_METHOD,                     0x88F483FBD433696A},
    {native::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT,             0x330108B080A2132F},
    {native::END_SCALEFORM_MOVIE_METHOD,                       0xE6B753D52F4CA222},
    {native::SET_BIGMAP_ACTIVE,                                0xC2F71CC2AB70CFB1},
    {native::GET_SAFE_ZONE_SIZE,                               0x897B441FF18ECBBB},
    // freemode look (ApplyPedOutfit), same YimMenu b3889 source: natives.hpp enum index 4134 / 4135 /
    // 4140 -> crossmap.hpp[index]. Each also equals YimMenuV2 `enhanced` g_Crossmap[Natives.hpp index
    // 4214 / 4215 / 4220] (two separately indexed lists agreeing by native name). Not live-checked
    // against a 3889 table dump yet.
    {native::IS_PED_COMPONENT_VARIATION_VALID,                 0xE91CD2B3CF8A106C},
    {native::SET_PED_COMPONENT_VARIATION,                      0xD1C578C204015E1F},
    {native::SET_PED_HEAD_BLEND_DATA,                          0x0A5987DCA39E8BE5},
    // remote-player ped driving, sender state readers, appearance read/apply. Each b3889 hash is the
    // YimMenu crossmap.hpp entry at the native's natives.hpp index and AGREES with YimMenuV2
    // `enhanced` g_Crossmap by name (scratchpad peddrive_tool/lookup.mjs). Not live-checked yet:
    // logNewNativeResolution reports them.
    {native::SET_ENTITY_COORDS_NO_OFFSET,                      0x62C438C53BB57AFD},
    {native::SET_ENTITY_VELOCITY,                              0x1AB7223AC0702871},
    {native::GET_ENTITY_VELOCITY,                              0xE5741C6B6539231F},
    {native::SET_ENTITY_COLLISION,                             0x44C48AC14D3C09ED},
    {native::GET_ENTITY_HEIGHT_ABOVE_GROUND,                   0x2399A6E4929050EE},
    {native::GET_GROUND_Z_FOR_3D_COORD,                        0xB1EAADCB692D69CE},
    {native::TASK_GO_STRAIGHT_TO_COORD,                        0x63C8DCBEC1CF8225},
    {native::TASK_STAND_STILL,                                 0x0FD8B5F4BB15CD71},
    {native::CLEAR_PED_TASKS,                                  0x974022927CB47E68},
    {native::SET_PED_DESIRED_MOVE_BLEND_RATIO,                 0xA6897CC743103C98},
    {native::SET_PED_DESIRED_HEADING,                          0xFBF90D96AEB26BCF},
    {native::IS_PED_WALKING,                                   0x0A11073DD28E2CD9},
    {native::IS_PED_RUNNING,                                   0x24433628C02BED5F},
    {native::IS_PED_SPRINTING,                                 0x6F21822F60DC0945},
    {native::IS_PED_STILL,                                     0x6CDEFB59235A1FD9},
    {native::IS_PED_JUMPING,                                   0x2C807E70DCB4BB36},
    {native::TASK_JUMP,                                        0xC3EBEA7530D64F53},
    {native::IS_PED_RAGDOLL,                                   0x8BF5256C439DF778},
    {native::SET_PED_TO_RAGDOLL,                               0xB1C2DC5C115FA50D},
    {native::SET_PED_CAN_RAGDOLL,                              0x9FF00EA9A61211D2},
    {native::SET_PED_CAN_EVASIVE_DIVE,                         0x84D421ACEBF9E529},
    {native::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS,             0xAAA71DD7E9059338},
    {native::SET_PED_CONFIG_FLAG,                              0x0428AFDCAA63B06E},
    {native::GET_PED_CONFIG_FLAG,                              0x49F8918E2DC5FFBC},
    {native::SET_PED_KEEP_TASK,                                0x44FB298D6382876D},
    {native::SET_PED_CAN_PLAY_AMBIENT_ANIMS,                   0xC1BC1B8A5AA67C6B},
    {native::IS_PED_FALLING,                                   0x9857C978BD3CBEDA},
    {native::IS_PED_CLIMBING,                                  0x7CB06BFD42FB0E24},
    {native::IS_PED_IN_PARACHUTE_FREE_FALL,                    0x333554781C7582D7},
    {native::IS_PED_SHOOTING,                                  0x65F146FF416F109F},
    {native::IS_PLAYER_FREE_AIMING,                            0x1C751EF63BF4D501},
    {native::GET_SELECTED_PED_WEAPON,                          0xB0D77D90171EC35F},
    {native::IS_WEAPON_VALID,                                  0x2A9ED010C087BF2B},
    {native::SET_CURRENT_PED_WEAPON,                           0x3C0F448853B71C92},
    {native::TASK_AIM_GUN_AT_COORD,                            0xC86A930D894F8CE2},
    {native::TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD,           0xB8551FB832F73124},
    {native::GET_PED_DRAWABLE_VARIATION,                       0xC0120BBCC298EA2F},
    {native::GET_PED_TEXTURE_VARIATION,                        0xD6AED6BFCC58AF7F},
    {native::GET_PED_PALETTE_VARIATION,                        0xDAF263B0E792EAEC},
    {native::GET_PED_PROP_INDEX,                               0xB204F40D393426B6},
    {native::GET_PED_PROP_TEXTURE_INDEX,                       0x0DC23FA727759F9F},
    {native::SET_PED_PROP_INDEX,                               0x7F08C4791E6D6969},
    {native::CLEAR_PED_PROP,                                   0x09397806857F5DFB},
    {native::GET_PED_HEAD_BLEND_DATA,                          0x5CB7287FD7611BC7},
    // vehicle sync, weapons/shots/damage, camera/aim. Same sourcing as the block above (YimMenu
    // crossmap at the natives.hpp index, AGREES with YimMenuV2 `enhanced` by name); not live-checked
    // yet: logNewNativeResolution reports them.
    {native::GET_ENTITY_QUATERNION,                            0xB66CD98548798F26},
    {native::SET_ENTITY_QUATERNION,                            0xF425E0B05426E088},
    {native::GET_ENTITY_ROTATION,                              0x88124E0D60FB8D11},
    {native::SET_ENTITY_ROTATION,                              0xCF39804E8C88080E},
    {native::GET_ENTITY_ROTATION_VELOCITY,                     0x47507DD57C93B472},
    {native::SET_ENTITY_ANGULAR_VELOCITY,                      0x7B6B10D7260C93CF},
    {native::GET_PED_IN_VEHICLE_SEAT,                          0xFD5C5BBD1FE92BB7},
    {native::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS,             0x2EEC0612337D20CE},
    {native::IS_VEHICLE_SEAT_FREE,                             0xC39AE5D390581AD5},
    {native::GET_VEHICLE_MODEL_NUMBER_OF_SEATS,                0xDC1AA2FE20EEB2E9},   // 0.4.22 doors and seats
    {native::GET_VEHICLE_DOOR_LOCK_STATUS,                     0x38A100E16C95161B},   // 0.4.22
    {native::PLAY_SOUND_FRONTEND,                              0xBF3D28CA44F3BE2D},   // 0.4.22
    {native::GET_SEAT_PED_IS_TRYING_TO_ENTER,                  0x03603B0046EE6ACD},
    {native::GET_VEHICLE_PED_IS_TRYING_TO_ENTER,               0xF9F8E3060F7CAEDB},
    {native::GET_VEHICLE_PED_IS_USING,                         0xCDA725BC2F170795},
    {native::TASK_ENTER_VEHICLE,                               0xEBA229B2E0BB05E0},
    {native::TASK_LEAVE_VEHICLE,                               0x23EB5FC236231892},
    {native::TASK_WARP_PED_INTO_VEHICLE,                       0x6CB88242CC3DA778},
    {native::SET_VEHICLE_STEER_BIAS,                           0xDEABDA7736297FEB},
    {native::GET_VEHICLE_ENGINE_HEALTH,                        0x4C7724D572378B05},
    {native::SET_VEHICLE_ENGINE_HEALTH,                        0x2AEBE39F6BF7D6BC},
    {native::GET_VEHICLE_BODY_HEALTH,                          0x3B5692CB240DBC2F},
    {native::SET_VEHICLE_BODY_HEALTH,                          0x3E7E7AD923FD91A7},
    {native::GET_IS_VEHICLE_ENGINE_RUNNING,                    0x182BD9AD1675B5DE},
    {native::SET_VEHICLE_SIREN,                                0x4539850624F18A9E},
    {native::IS_VEHICLE_SIREN_ON,                              0xE101D58DA98B6070},
    {native::START_VEHICLE_HORN,                               0xAA8884A4BB5B0167},
    {native::GET_VEHICLE_COLOURS,                              0xFF4B16F297D9CB3E},
    {native::SET_VEHICLE_COLOURS,                              0xD133EF7430EDCD09},
    {native::SET_VEHICLE_DOORS_LOCKED,                         0x0B74F181ADFC39BF},
    {native::SET_VEHICLE_LIGHTS,                               0xBA3C1A9AA7FD9616},
    {native::SET_VEHICLE_FORWARD_SPEED,                        0x93C337B66C95C99B},
    {native::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER,             0x983B8E72C35B01F2},
    {native::SET_VEHICLE_IS_STOLEN,                            0xD37CA6D6F8993BB9},
    {native::SET_ENTITY_AS_MISSION_ENTITY,                     0xEE0BCDB1B5E36BCB},
    {native::SET_VEHICLE_DOOR_SHUT,                            0x6515021478088FBC},
    {native::SHOOT_SINGLE_BULLET_BETWEEN_COORDS,               0xA934E5D7EEE03C7E},
    {native::SET_PED_SHOOTS_AT_COORD,                          0xC7AF90C5FC07B06B},
    {native::TASK_SHOOT_AT_COORD,                              0x6C4E9ADFB1521AAC},
    {native::GET_PED_LAST_WEAPON_IMPACT_COORD,                 0x4800F32F989BED65},
    {native::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY,                0x9B3D4335E0EDB0BE},
    {native::HAS_ENTITY_BEEN_DAMAGED_BY_ANY_PED,               0xF76AF9D45D6BDA45},
    {native::CLEAR_ENTITY_LAST_DAMAGE_ENTITY,                  0xE4DC7B3DD712372B},
    {native::GET_PED_LAST_DAMAGE_BONE,                         0xB76C3F86B744BBA5},
    {native::CLEAR_PED_LAST_DAMAGE_BONE,                       0xCBCBAEEB5BBCA58A},
    {native::GET_PED_ARMOUR,                                   0xE5E6F6EFCE07789A},
    {native::SET_PED_ARMOUR,                                   0x10A676E622A468AA},
    {native::GET_WEAPON_DAMAGE,                                0xFE2B29CE6AE93F62},
    {native::GET_PED_SOURCE_OF_DEATH,                          0x836392D80E4F106A},
    {native::IS_ENTITY_DEAD,                                   0x1C2F771CDC87A3A5},
    {native::SET_ENTITY_CAN_BE_DAMAGED,                        0x999C62072AF920FD},
    {native::SET_ENTITY_PROOFS,                                0xD0F1DB0E50B367AD},
    {native::SET_PED_SUFFERS_CRITICAL_HITS,                    0xE915224DCA36212A},
    {native::APPLY_DAMAGE_TO_PED,                              0x39AB1812D20C2C99},
    {native::GET_CURRENT_PED_WEAPON_ENTITY_INDEX,              0x484426882F80CACE},
    {native::GET_ENTITY_BONE_INDEX_BY_NAME,                    0x365DC1E8054AF31A},
    {native::GET_WORLD_POSITION_OF_ENTITY_BONE,                0x75DF72FC74EED046},
    {native::GET_AMMO_IN_PED_WEAPON,                           0x1149D67DB429787A},
    {native::SET_PED_INFINITE_AMMO_CLIP,                       0x53A38286A3AC16C1},
    {native::GET_GAMEPLAY_CAM_COORD,                           0xCF141FCD0940B0A3},
    {native::GET_GAMEPLAY_CAM_ROT,                             0xD84A545408A3099A},
    {native::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE, 0x14C30F326F5883DA},
    {native::GET_SHAPE_TEST_RESULT,                            0x0E7DD1EBCA8D2DE3},
    {native::IS_AIM_CAM_ACTIVE,                                0x196704C916969409},
    {native::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT,              0x66EE98F15844BE4D},
    // network-state baseline (net_probe=1, G0): read-only getters.
    {native::NETWORK_IS_SIGNED_IN,                             0x5F9F81C08516558E},
    {native::NETWORK_IS_SIGNED_ONLINE,                         0x261E3728EE56B3AC},
    {native::NETWORK_HAVE_ONLINE_PRIVILEGES,                   0xD25970CBEE074E07},
    {native::NETWORK_IS_CLOUD_AVAILABLE,                       0x5676319ACE5BEC37},
    {native::NETWORK_IS_SESSION_STARTED,                       0x2312C61FB9082621},
    {native::NETWORK_IS_IN_SESSION,                            0xBF52D447C795492B},
    // protocol v4: emotes, server-owned health, entity checks, vehicle horn.
    {native::REQUEST_ANIM_DICT,                                0x80813AC549A1E8AE},
    {native::HAS_ANIM_DICT_LOADED,                             0xE100DD4F82A51BDE},
    {native::REMOVE_ANIM_DICT,                                 0x268BE77F77533D03},
    {native::DOES_ANIM_DICT_EXIST,                             0xBBE75ED2B58BB1F7},
    {native::TASK_PLAY_ANIM,                                   0x10425721983AE158},
    {native::TASK_START_SCENARIO_IN_PLACE,                     0x21B9EFA4B4AA3A5B},
    {native::IS_ENTITY_PLAYING_ANIM,                           0x13CCB1AD131C1082},
    {native::IS_PED_USING_ANY_SCENARIO,                        0x5110BDFA9CB51F2B},
    {native::SET_ENTITY_MAX_HEALTH,                            0xF0259D91B002AE9F},
    {native::DOES_ENTITY_EXIST,                                0xFC8BFE4B41177C22},
    {native::IS_HORN_ACTIVE,                                   0x1A90E9DC88A85C9D},
    {native::SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER,            0x02DC05627BC628AF},
    // the no-pause pause menu, its proof line's game timer, the weapon group.
    {native::ACTIVATE_FRONTEND_MENU,                           0xBDBC487A0347C2C1},
    {native::IS_PAUSE_MENU_ACTIVE,                             0x4D9174D8796EA622},
    {native::IS_DISABLED_CONTROL_JUST_PRESSED,                 0xDEE3EFEA31A1F555},
    {native::DISABLE_FRONTEND_THIS_FRAME,                      0x6C978B200DAA54DE},
    {native::SET_PAUSE_MENU_ACTIVE,                            0x915FA95E87D33FF5},
    {native::GET_GAME_TIMER,                                   0x1DD05E817C89C737},
    {native::GET_WEAPONTYPE_GROUP,                             0x6CC7A2E68E8A565A},
    // sync plan W0 (docs/SYNC-PLAN.md "Natives"): on-foot reload/stealth/swim/cover/melee/move rate,
    // vehicle lights/doors/roof, drive-by tasks, particle fx. Each b3889 hash is the YimMenu
    // crossmap.hpp entry at the native's natives.hpp index (1.73-3889.0) and AGREES with YimMenuV2
    // `enhanced` g_Crossmap by name. Not live-checked yet: logNewNativeResolution reports them.
    {native::IS_PED_RELOADING,                            0xC722DDBD6C3E86D7},
    {native::TASK_RELOAD_WEAPON,                          0x550B03C7904C63CD},
    {native::GET_PED_STEALTH_MOVEMENT,                    0xC2BF1F6F84E31EB2},
    {native::SET_PED_STEALTH_MOVEMENT,                    0xF9358C41CC69C616},
    {native::IS_PED_SWIMMING,                             0x2CFBD7757B4D922F},
    {native::IS_PED_IN_COVER,                             0xD6179D448A06A77F},
    {native::TASK_PUT_PED_DIRECTLY_INTO_COVER,            0x994B4C04761C0AA0},
    {native::IS_PED_PERFORMING_MELEE_ACTION,              0xB73833BDAAE31047},
    {native::SET_PED_MOVE_RATE_OVERRIDE,                  0xB27B08E34AC92345},
    // 0.4.3 fixes (W8): hashes from the lookup tool, yim3889 and v2enh agreeing; public hashes are in hook.h.
    {native::IS_PED_VAULTING,                             0x75B105C651D87D0E},
    {native::TASK_CLIMB,                                  0x345C12E1D643634F},
    {native::SET_AI_WEAPON_DAMAGE_MODIFIER,               0x36978A351E9CB5D8},
    {native::SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER,         0x767EC66D239AED1D},
    {native::RESET_AI_WEAPON_DAMAGE_MODIFIER,             0xDD84F9C956E69AC8},
    {native::RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER,       0x01B722A2B2026558},
    {native::WAS_PED_KILLED_BY_TAKEDOWN,                  0xD9F6166CE6292D26},
    // Hyper tune (lookup tool, yim3889 and v2enh agreeing; public hashes in hook.h).
    {native::MODIFY_VEHICLE_TOP_SPEED,                    0xAC89BB42FE09CC80},
    {native::SET_VEHICLE_CHEAT_POWER_INCREASE,            0xF1C985BBEC6B6321},
    {native::SET_VEHICLE_MAX_SPEED,                       0x8E9B5F00B41A9F8C},
    {native::GET_VEHICLE_ESTIMATED_MAX_SPEED,             0x66C1D6E6DBF7AC9B},
    // Hyper tune handling (the Dubsta, 2026-11-04): YimMenu b3889 crossmap, each AGREEING with YimMenuV2 enhanced by name; public hashes in hook.h.
    {native::SET_VEHICLE_STRONG,                          0xD05AF216D970F274},
    {native::SET_VEHICLE_HAS_STRONG_AXLES,                0x5FE0938FDE9B0958},
    {native::SET_VEHICLE_TYRES_CAN_BURST,                 0x439C904840715871},
    {native::SET_VEHICLE_WHEELS_CAN_BREAK,                0x72241E9BCAEB0F8B},
    {native::IS_ENTITY_IN_AIR,                            0x3F18810075C77D41},
    // 0.4.14 anim read-backs: YimMenu b3889 crossmap, each AGREEING with YimMenuV2 enhanced by name; public hashes and units in hook.h.
    {native::GET_ENTITY_ANIM_CURRENT_TIME,                0x82E64DE58A6B84A8},
    {native::GET_ENTITY_ANIM_TOTAL_TIME,                  0x58C2315AA20FAF06},
    {native::GET_ANIM_DURATION,                           0x6CEF2D1ADD6A5E7F},
    {native::GET_SCRIPT_TASK_STATUS,                      0x9B5C1660CCDF7189},
    // R5 melee_loop: YimMenu b3889 crossmap, AGREEING with YimMenuV2 enhanced by name; public hash in hook.h.
    {native::STOP_ANIM_TASK,                              0x08D8528BA8E43641},
    // 0.4.19 radio off: YimMenu b3889 crossmap, each AGREEING with YimMenuV2 enhanced (and legacy) by name; public hashes in hook.h.
    {native::SET_USER_RADIO_CONTROL_ENABLED,              0x938C85923AD6778A},
    {native::SET_VEHICLE_RADIO_ENABLED,                   0x4DC72C4B884A6C59},
    {native::SET_VEH_RADIO_STATION,                       0x38911F7BA2D48F10},
    {native::SET_RADIO_TO_STATION_NAME,                   0x166878629B365828},
    {native::SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY,    0x048A57C4A90E9F0D},
    {native::SET_AUDIO_FLAG,                              0xCFBD89D2F1F18961},
    {native::GET_PLAYER_RADIO_STATION_INDEX,              0xE1799BB6E4C1CDFB},
    {native::IS_VEHICLE_RADIO_ON,                         0xC052348A3DF7313F},
    {native::IS_HUD_COMPONENT_ACTIVE,                     0x7B035E8A1D320F1B},
    {native::IS_DISABLED_CONTROL_PRESSED,                 0x4465D55576678706},
    // 0.4.22 noclip: YimMenu b3889 crossmap, each AGREEING with YimMenuV2 enhanced (and legacy) by name; public hashes in hook.h.
    {native::SET_ENTITY_HAS_GRAVITY,                      0xBF861D73D95BF415},
    {native::GET_ENTITY_COLLISION_DISABLED,               0x8473361800D1B85B},
    {native::SET_PLAYER_INVINCIBLE,                       0x2B52F77101390E6F},
    {native::GET_PLAYER_INVINCIBLE,                       0xCF59B5FEF38A9930},
    // 0.4.19 spawn reveal: lookup tool, YimMenu b3889 and YimMenuV2 enhanced agree by name.
    {native::CLEAR_TIMECYCLE_MODIFIER,                    0x80F87A4A3B930613},
    {native::GET_TIMECYCLE_MODIFIER_INDEX,                0x8960847E5FD7B48F},
    {native::DRAW_LIGHT_WITH_RANGE,                       0x748651DF4DA0A890},   // 0.4.27 indicator fallback
    {native::GET_MODEL_DIMENSIONS,                        0xC93BAF616F1C680F},
    {native::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS,      0x0D1381B6E0F3987D},
    // 0.4.4 (W11): lookup tool, public and 3889 agree.
    {native::TASK_GO_TO_ENTITY,                           0xAB3658A740EED98E},
    {native::GET_VEHICLE_LIGHTS_STATE,                    0x9FFEA38DBCE391EC},
    {native::SET_VEHICLE_FULLBEAM,                        0x2F12C305B28C6C59},
    {native::SET_VEHICLE_INTERIORLIGHT,                   0xEC6A9F16D3F15978},
    {native::SET_VEHICLE_INDICATOR_LIGHTS,                0x78004905636369DF},
    {native::GET_VEHICLE_DOOR_ANGLE_RATIO,                0x7BFB76C576628F3D},
    {native::SET_VEHICLE_DOOR_OPEN,                       0xBFE60A5CC0C835D8},
    {native::GET_CONVERTIBLE_ROOF_STATE,                  0x54DA32C15F7A6ABA},
    {native::RAISE_CONVERTIBLE_ROOF,                      0xD6B15DF382A594C7},
    {native::LOWER_CONVERTIBLE_ROOF,                      0x232B023FE4D977E2},
    {native::TASK_DRIVE_BY,                               0xD83588CA24829967},
    {native::SET_DRIVEBY_TASK_TARGET,                     0xF38F1E299109383A},
    {native::TASK_VEHICLE_AIM_AT_COORD,                   0xE61AF8D27706A774},
    {native::CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK,  0x827783BAD5CBE95D},
    {native::GET_IS_TASK_ACTIVE,                          0x12623527E5B8AB7C},
    {native::START_PARTICLE_FX_NON_LOOPED_ON_ENTITY,      0xDD19F75405614D3C},
    {native::USE_PARTICLE_FX_ASSET,                       0xD03F4780B97A39AE},
    {native::REQUEST_NAMED_PTFX_ASSET,                    0xEBEE7DC21AB44EC9},
    {native::HAS_NAMED_PTFX_ASSET_LOADED,                 0x939D49C9FAA8139A},
    // 0.4.4 (W9): lookup tool, public and 3889 agree.
    {native::HAS_ENTITY_COLLIDED_WITH_ANYTHING,           0x14E25ED5E75102C8},
    {native::GET_VEHICLE_CLASS,                           0x5506AFBD6056C05F},
    // 0.4.4 (W10): lookup tool, public and 3889 agree.
    {native::GET_SCREEN_COORD_FROM_WORLD_COORD,           0x2F8A75C70AF54208},
    {native::GET_FINAL_RENDERED_CAM_COORD,                0x02AF3EA0F67D2329},
    {native::HAS_ENTITY_CLEAR_LOS_TO_ENTITY,              0xAD915B5E38F323E5},
    {native::GET_PED_BONE_COORDS,                         0x83FDC027F0BEA202},
    // 0.4.7 (desync round 2): T3 vehicle visual state + T5 ambient sweep. Public hashes from the alloc8or native DB; 3889 hashes from
    // the YimMenu crossmap, each one confirmed by the YimMenuV2 enhanced crossmap (peddrive lookup tool: both AGREE for all 15).
    {native::IS_VEHICLE_TYRE_BURST,                       0x548F6F43A7CB6F45},
    {native::SET_VEHICLE_TYRE_BURST,                      0xE488FDAA43A181AE},
    {native::SET_VEHICLE_TYRE_FIXED,                      0xF516E954BCB89C18},
    {native::IS_VEHICLE_WINDOW_INTACT,                    0x01D37530E5C420F5},
    {native::SMASH_VEHICLE_WINDOW,                        0x62DFD44586348C12},
    {native::FIX_VEHICLE_WINDOW,                          0x3BFC7329EC766FED},
    {native::GET_CONTROL_NORMAL,                          0xB504E1B50AA21FC5},
    {native::SET_VEHICLE_HANDBRAKE,                       0xB2FD24D644A68449},
    {native::SET_VEHICLE_BRAKE_LIGHTS,                    0xE456FB21FF21AE99},
    {native::SET_PED_POPULATION_BUDGET,                   0xAD9B1C8FED6B4D96},
    {native::SET_VEHICLE_POPULATION_BUDGET,               0x283C0970282AA5F3},
    {native::SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA,   0xA63572E348CC4CFB},
    {native::DELETE_ALL_TRAINS,                           0x47931C69C0D75B43},
    {native::GET_PED_NEARBY_PEDS,                         0xAA3F0A9E20917995},
    {native::GET_PED_NEARBY_VEHICLES,                     0xD65B4E942A960E26},
    // Desync round 3: public hashes from the alloc8or native DB; 3889 hashes from the YimMenu crossmap, each confirmed by a second independent crossmap.
    {native::SET_PED_MAX_HEALTH,                                  0x36A20106D0B42723},
    {native::SET_PLAYER_MAX_ARMOUR,                               0x776D5F57819A1E41},
    {native::GET_VEHICLE_PETROL_TANK_HEALTH,                      0x31B58D7972181BFA},
    {native::SET_VEHICLE_PETROL_TANK_HEALTH,                      0xDF9DC0584881B7AF},
    {native::SET_VEHICLE_DAMAGE,                                  0x2BDD547718FF1F1A},
    {native::GET_VEHICLE_DEFORMATION_AT_POS,                      0x05A904F31991BA49},
    {native::IS_VEHICLE_DOOR_DAMAGED,                             0xAD830DCD82C63F31},
    {native::SET_VEHICLE_DOOR_BROKEN,                             0x89E9F387C190061F},
    {native::SET_VEHICLE_DOOR_CONTROL,                            0x3E13A302AA0F06BF},
    {native::IS_VEHICLE_DOOR_FULLY_OPEN,                          0x6B0915C0DDD2954F},
    {native::GET_IS_DOOR_VALID,                                   0xB3E28598CA656061},
    {native::GET_IS_LEFT_VEHICLE_HEADLIGHT_DAMAGED,               0xB4A09397270CB974},
    {native::GET_IS_RIGHT_VEHICLE_HEADLIGHT_DAMAGED,              0x08046513BC06BFBC},
    {native::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED,                  0x6BFFBDE3C98A828E},
    {native::SET_VEHICLE_ENGINE_CAN_DEGRADE,                      0xF9DF73BCBDB93008},
    {native::SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE,              0x8DB4917AFAD347BC},
    {native::IS_PED_GOING_INTO_COVER,                             0x3F055D2CBA242A5D},
    {native::TASK_STAY_IN_COVER,                                  0x115CA3B4F950226D},
    {native::IS_PED_AIMING_FROM_COVER,                            0xF5327658CA9AF463},
    {native::TASK_COMBAT_PED,                                     0x62A5310368A20EFA},
    {native::IS_PED_IN_MELEE_COMBAT,                              0xFFAC548682B3D56E},
    {native::TASK_PUT_PED_DIRECTLY_INTO_MELEE,                    0x849ABF0CBD8EE0D6},
    {native::SET_PED_COMBAT_ATTRIBUTES,                           0xBE8796DB2B90A437},
    // Probes for the table dump report only.
    {0x1090044AD1DA76FA /* TERMINATE_THIS_THREAD */, 0xBBC29EBE6E1A48FA},
    {0x7239B21A38F536BA /* DOES_ENTITY_EXIST */,     0xFC8BFE4B41177C22},
    {0x15C40837039FFAF7 /* GET_FRAME_TIME */,        0x0B852B0BF94A8DC1},
    {0xD24D37CC275948CC /* GET_HASH_KEY */,          0x70E57E9927B6BA58},
};

// ---------------------------------------------------------------------------
// Per-build profile
// ---------------------------------------------------------------------------
// Everything in this file that is specific to a GTA5 build (native crossmap,
// byte-signatures, and the thread/TLS field offsets) lives here so a second
// target is a data change, not scattered edits. The build is chosen in Init()
// by the main module's file name. Legacy 1.0.3889 is fully derived + live-
// proven; Enhanced (GTA5_Enhanced.exe, build ~1158) is Arxan-encrypted on disk
// (.text entropy 8.00), so its values can only come from a decrypted runtime
// dump (see the Enhanced dump mode) and stay `known=false` until then. An
// unknown build keeps every resolver inert (null sig = no scan; empty crossmap
// = no re-hash; null table) instead of scanning with the wrong patterns.
struct BuildProfile {
    const wchar_t*     exeName;              // main-module file name to match (compared lower-cased)
    const char*        label;                // for the log
    bool               known;                // false => sigs/offsets not derived yet; do not scan
    const HashMapping* crossmap;             // public -> build-specific native hash
    size_t             crossmapCount;
    const char*        nativeLookupSig;      // get_native_handler + table (optional; node-walk is the fallback)
    const char*        initNativeTablesSig;  // Enhanced native resolver (scrProgram link); optional
    const char*        scriptThreadsSig;     // run_script_threads (sig matches an interior instruction)
    int                scriptFnOffset;       // signed: match + this = run_script_threads entry (hook target)
    const char*        scriptArraySig;       // separate sig for the ScriptThreads atArray; null => derive
                                             //   from scriptThreadsSig the 3889 way (rip(match-4)-8)
    size_t             tlsScriptThread;      // rage::tlsContext -> current GtaThread*
    size_t             tlsScriptThreadActive;
    size_t             threadId;             // GtaThread::m_context.m_thread_id
    size_t             threadHandler;        // GtaThread -> scriptHandler*
    size_t             threadScriptHash;     // GtaThread::m_script_hash
    const char*        landingPageSig;       // StraightToStoryMode landing-page check (optional)
    // Enhanced-only (0/null on Legacy, which keeps the bucketed path): the
    // InitNativeTables scrProgram-link native resolver and an extra thread field.
    int                initNativeTablesFnOffset;  // signed: initNativeTablesSig match + this = fn entry
    size_t             scrProgNativeCount;        // rage::scrProgram::m_NativeCount
    size_t             scrProgNativeEntrypoints;  // rage::scrProgram::m_NativeEntrypoints
    size_t             threadScriptName;          // scrThread::m_ScriptName (char[64]); diagnostics + name fallback
    // no_sp_scripts (GtaThread::Tick -> no-op). Enhanced: an ENTRY-anchored signature (match + gtaThreadTickFnOffset
    // = the function). Legacy leaves gtaThreadTickSig null: its pattern sits INSIDE the function and stays in
    // noSpInstall (fn = match - 0xB). threadStateOff is where the thread keeps its eThreadState, which is what Tick
    // returns (0 = the Legacy rule threadId + 8).
    const char*        gtaThreadTickSig;
    int                gtaThreadTickFnOffset;
    size_t             threadStateOff;
    // Enhanced: hook GtaThread::Tick only once the stock boot has run (main_persistent is a live thread), not the
    // moment its code decrypts. 0.4.11 hooked it 3 s into the process, before one script had ticked, and the game
    // died with 0xC0000005 a few seconds after its first frame (twice). Legacy keeps the early hook (false).
    bool               noSpAfterBoot;
    // The RVA GtaThread::Tick was PROVEN at in a live run of this build (0 = none). When the signature matches more
    // than once (0.4.20 Legacy: 3 hits for whole sessions), the hit at this RVA is hooked (nosp_pick.h); otherwise
    // an ambiguous signature is still refused.
    uint64_t           gtaThreadTickRva;
};

const BuildProfile kProfile3889 = {
    L"gta5.exe", "Legacy 1.0.3889", /*known=*/true,
    kCrossmap3889, sizeof(kCrossmap3889) / sizeof(kCrossmap3889[0]),
    "48 8D 0D ?? ?? ?? ?? 48 8B 14 FA E8 ?? ?? ?? ?? 48 85 C0 75 0A",  // nativeLookupSig
    nullptr,                                                           // initNativeTablesSig (Legacy: bucketed table)
    "45 33 F6 8B E9 85 C9 B8",                                         // scriptThreadsSig
    -0x1F,                                                             // scriptFnOffset (fn = match - 0x1F)
    nullptr,                                                           // scriptArraySig (derive from scriptThreadsSig)
    0x2A48, 0x2A50, 0x08, 0x118, 0x128,
    "E8 ?? ?? ?? ?? 84 C0 8B 05 ?? ?? ?? ?? 74 ?? 83 C0",              // landingPageSig
    0, 0, 0, 0,                                                        // Enhanced-only fields: unused on Legacy (bucketed path)
    nullptr, 0, 0,                                                     // gtaThreadTickSig / fn offset / state offset: Legacy uses its own pattern in noSpInstall
    false,                                                             // noSpAfterBoot: Legacy hooks Tick as soon as it decrypts (proven live)
    0xa64738,                                                          // gtaThreadTickRva: gage.log 2026-11-05 08:35:22, unique hit, "stock scripts OFF"
};

// Enhanced build 1158.13, DX12. Every value here was derived from the friend's
// decrypted image dump (enhanced EXE/dump and bin/) + YimMenuV2's `enhanced`
// branch, and each CODE signature was verified UNIQUE against that dump
// (scratchpad verify_sigs.py); see enhanced-port-plan. The crossmap is REUSED
// from 3889: Rockstar's native-hash translation is frozen and shared across
// Legacy 3889 and Enhanced, so the build hashes of kCrossmap3889 are present in
// YimMenuV2's gen9 g_Crossmap (verified 103/103 when first written, scratchpad
// crossmap_check.py; after the 3 freemode-look entries were added the whole
// 108-entry table was re-checked, 108/108 present) — the set the Enhanced
// resolver feeds InitNativeTables. Only the hash's presence is verified: the
// freemode-look natives have not been called on Enhanced.
// Native invocation: Enhanced has no classic get_native_handler and no walkable
// bucket table (the scan finds only false positives), so nativeLookupSig is null
// and the resolver is initNativeTablesSig (scrProgram native link, fn=match-0x2A)
// — the primary and only Enhanced native path (tryInitNativeTablesResolve).
// Offsets from YimMenuV2 maintained headers (tlsContext 0x7A0/0x7A8 and the
// GtaThread name-hash 0x1A8 are the heap-only values we cannot check from an
// image dump: findScriptThread dumps threads once and name-matches as a fallback;
// run_script_threads sig is a build-specific immediate load — all re-derive if an
// Enhanced patch moves them).
const BuildProfile kProfile1158 = {
    L"gta5_enhanced.exe", "Enhanced 1158.13 (DX12)", /*known=*/true,
    kCrossmap3889, sizeof(kCrossmap3889) / sizeof(kCrossmap3889[0]),   // SAME crossmap as 3889 (verified identical)
    nullptr,                                                           // nativeLookupSig (no get_native_handler on Enhanced)
    "EB 2A 0F 1F 40 00 48 8B 54 17 10",                               // initNativeTablesSig (entry = match - 0x2A)
    "BE 40 5D C6 00",                                                  // scriptThreadsSig (run_script_threads)
    -0xA,                                                              // scriptFnOffset (fn = match - 0xA)
    "48 8B 05 ?? ?? ?? ?? 48 89 34 F8 48 FF C7 48 39 FB 75 97",        // scriptArraySig (ScriptThreads atArray, &atArray = rip@+3)
    0x7A0, 0x7A8, 0x08, 0x198, 0x1A8,                                  // tls thread/active, thread id, scriptHandler, m_ScriptHash2
    "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 83 C1 ?? 84 C0",                 // landingPageSig (Enhanced)
    -0x2A, 0x2C, 0x40, 0x154,                                          // InitNativeTables fn=match-0x2A; scrProgram m_NativeCount/m_NativeEntrypoints; m_ScriptName
    // GtaThread::Tick (no_sp_scripts), derived 2026-11-04 from the friend's dump, all offline: run_script_threads
    // (RVA 0x920a40) calls `call [rax+0x18]` = vtable slot 3 of every live thread with edx = the op budget and
    // compares the result with 2 (KILLED); the GtaThread vtable (RVA 0x2679270) holds 0x13cfca0 in slot 3 (the base
    // scrThread vtable 0x2610e80 holds 0x9209f0); slot 2 of both is the interpreter wrapper 0x91dc20, the only
    // function in the image that stores the TLS thread slots +0x7A0 / +0x7A8. 0x13cfca0 =
    //   56 57 53 | 48 83 EC 20 | 48 89 CE | 80 B9 CE 01 00 00 00 | 74 08 | 8B 7E 18 | E9 ...
    //   push rsi/rdi/rbx; sub rsp,20; mov rsi,rcx; cmp byte [rcx+0x1ce],0; je +8; mov edi,[rsi+0x18]; jmp -> return edi
    // i.e. the Legacy function's shape with `mov rsi,rcx` between the sub and the cmp (which is why the Legacy
    // pattern found 0 hits), and the early-out returns the state at this+0x18. Unique in the dump (1 hit); the
    // first 17 bytes are whole instructions with no CALL, so netJump takes it. The code is verified offline only;
    // noSpVerifyTick cross-checks it live against the vtable.
    "56 57 53 48 83 EC 20 48 89 CE 80 B9 ?? ?? 00 00 00 74 08 8B 7E 18 E9", 0, 0x18,
    // noSpAfterBoot: the friend's 0.4.11 run hooked Tick 3 s into the process (before any stock script had ticked) and
    // the game died with 0xC0000005 a few seconds after its first frame, twice; 0.4.10 (no hook, engine attached at
    // ~26 s) ran 144 s. The no-op now lands once main_persistent is live, i.e. after the stock boot created the player.
    true,
    0,                                                                 // gtaThreadTickRva: none proven live on Enhanced yet
};

// Selected in Init() by main-module file name; defaults to Legacy so the live
// 3889 path is unchanged if detection ever fails.
const BuildProfile* g_build = &kProfile3889;

// Pick the profile whose exeName matches the main module's file name (compared
// lower-cased). Unmatched -> nullptr (caller falls back to Legacy and logs).
const BuildProfile* selectBuildProfile(const wchar_t* moduleBaseName) {
    auto ieq = [](const wchar_t* a, const wchar_t* b) {
        for (; *a && *b; ++a, ++b) {
            wchar_t ca = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : *a;
            wchar_t cb = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
            if (ca != cb) return false;
        }
        return *a == *b;
    };
    for (const BuildProfile* p : { &kProfile3889, &kProfile1158 })
        if (moduleBaseName && ieq(moduleBaseName, p->exeName)) return p;
    return nullptr;
}

// The build-specific hash for a public one; unmapped hashes (WAIT,
// START_NEW_SCRIPT and anything a gamemode passes that is not listed) and every
// hash on a build with no crossmap yet are returned unchanged.
uint64_t buildHash(uint64_t pub) {
    const BuildProfile* b = g_build;
    for (size_t i = 0; b && b->crossmap && i < b->crossmapCount; ++i)
        if (b->crossmap[i].pub == pub) return b->crossmap[i].b3889;
    return pub;
}

// The game's own lookup (get_native_handler) and the table it reads, found by the
// signature 3889 mod bases use (Mr-X-GTA/YimMenu pointers.cpp "NH", built for
// GTA_VERSION_TARGET "1.73-3889.0"):
//   lea rcx,[table]; mov rdx,[rdx+rdi*8]; call get_native_handler; test rax,rax; jnz
// Calling it sidesteps the node encoding entirely; null until validated.
using GetNativeHandlerFn = NativeHandler (*)(uint8_t** table, uint64_t hash);
GetNativeHandlerFn g_getNativeHandler = nullptr;

// Candidate table references found by the image scan. Each is validated in the
// poller by resolving known native hashes; the one that resolves both wins.
struct NTCandidate {
    uint8_t** addr;   // LEA: the table array itself; MOV: a .data pointer TO the table
    bool      isLea;  // true = use addr directly; false = dereference addr for the table
};
std::vector<NTCandidate> g_candidates;

// For lazy resolution: if Init runs before the game populates its native table
// (common at 0.4 s after game start), we retry a few times from Frame().
const uint8_t* g_imageBase = nullptr;
size_t          g_imageSize = 0;

// Set from World::skipLanding in Init; read by the native poller, which applies
// the landing-page patch once its Arxan-decrypted code appears. Off by default.
static bool g_skipLanding = false;
// Set from World::netSession in Init; when on, a background thread scans the
// decrypted image for the FiveM/YimMenu session-entry signatures and logs which
// resolve uniquely on this build (net_session=1). Recon only -- no hooks. Off by default.
static int g_netSession = 0;
// Set from World::netHost in Init; when on (net_host=1), the FiveM-style local
// session-entry stubs are installed from the poller once the network code is
// decrypted, and undone on Unhook (netUnpatchAll). Off by default. Independent of
// net_session (recon). g_netHostInstalled latches the one-time install (or its 120 s give-up).
static int  g_netHost = 0;
static bool g_netHostInstalled = false;
// Set from World::netProbe in Init (net_probe, default 0 = off): the read-only G0 network baseline --
// hostname/destination logging through net_guard's IAT hooks and the NETWORK_IS_* samples after
// the entry (netProbeTick). Read-only: nothing in the game is patched or called that changes state.
static int  g_netProbe = 0;
// True when the main module's name matched a build profile (Init); an unrecognised exe (a test
// harness) falls back to the 3889 profile for scanning but never gets the import-table probe.
static bool g_buildMatched = false;
static int  g_noSpScripts = 0;           // no_sp_scripts=1: FiveM JustNoScript (GtaThread::Tick no-op)
static bool g_noSpInstalled = false;     // installer finished (hooked, or gave up)
static bool g_noSpKnownRva = true;       // nosp_known_rva (default on): an ambiguous Tick signature may use the proven RVA
static bool g_noSpActive = false;        // GtaThread::Tick is actually hooked
// Set in Init from World::enhDump: write the decrypted image on an unported
// build (see tryDumpDecryptedImage). Defined here so Init (above the dump code)
// can see it.
static bool g_enhDump = false;
int             g_nativeRetries = 0;
const int       kMaxNativeRetries = 600; // retry up to ~600 frames (~10 s at 60 fps)

// Check that `p` points to committed, readable memory (not junk/uninitialized).
// Uses VirtualQuery — no SEH needed. Returns false for null, wild pointers,
// and guard/noaccess pages.
inline bool isReadable(const void* p, size_t len) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || (mbi.Protect & PAGE_GUARD))
        return false;
    // Check that the entire range [p, p+len) is within this committed region.
    uintptr_t start = (uintptr_t)p;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return start + len <= regionEnd;
}

// Walk an arbitrary 256-bucket registration table for `hash`, fully guarded by
// isReadable at every pointer dereference. Returns the handler, or nullptr if
// the hash isn't present or any entry is not yet readable (junk / unpopulated).
// Does NOT touch the global table or cache — safe for validating candidates.
NativeHandler lookupInTable(uint8_t** table, uint64_t hash, NodeLayout layout = g_nodeLayout) {
    if (!table) return nullptr;
    uint32_t bucket = (uint32_t)(hash & 0xFF);

    // The bucket slot itself must be readable (the table may be junk / unmapped).
    if (!isReadable(&table[bucket], sizeof(uint8_t*)))
        return nullptr;
    uint8_t* reg = table[bucket];
    if (!reg) return nullptr;  // empty bucket (null is fine, not junk)

    // Safety cap: max 256 nodes per bucket (should be < 20 in practice).
    for (int safety = 0; reg && safety < 256; ++safety) {
        // Verify this NativeRegistration node is committed/readable before
        // touching any field. At early startup the table holds junk pointers.
        if (!isReadable(reg, nodeSize(layout)))  // the whole node for this layout
            return nullptr;

        uint32_t count = nodeCount(reg, layout);
        if (count > 7) return nullptr;  // junk count → not a real node
        for (uint32_t i = 0; i < count; ++i) {
            if (nodeHash(reg, i, layout) == hash) {
                NativeHandler h = regGetHandler(reg, i);
                if (h && isReadable((void*)h, 1))
                    return h;
            }
        }
        reg = nodeNext(reg, layout);
    }
    return nullptr;
}

// Look up a native handler by its 64-bit hash through the resolved global
// table. Uses the cache for speed.
// The render thread and the game thread both call natives (around the script-tick hand-over),
// and the poller fills and clears the cache: every access goes through this lock (shared for
// reads, which is nearly every call).
SRWLOCK g_nativeCacheLock = SRWLOCK_INIT;

bool nativeCacheFind(uint64_t hash, NativeHandler& out) {
    AcquireSRWLockShared(&g_nativeCacheLock);
    auto it = g_nativeCache.find(hash);
    const bool hit = it != g_nativeCache.end();
    if (hit) out = it->second;
    ReleaseSRWLockShared(&g_nativeCacheLock);
    return hit;
}

void nativeCacheStore(uint64_t hash, NativeHandler h) {
    AcquireSRWLockExclusive(&g_nativeCacheLock);
    g_nativeCache[hash] = h;
    ReleaseSRWLockExclusive(&g_nativeCacheLock);
}

void nativeCacheClear() {
    AcquireSRWLockExclusive(&g_nativeCacheLock);
    g_nativeCache.clear();
    ReleaseSRWLockExclusive(&g_nativeCacheLock);
}

NativeHandler findNativeHandler(uint64_t hash) {
    NativeHandler h = nullptr;
    if (nativeCacheFind(hash, h))
        return h;

    // This build's hash first, then the hash as given (an already-3889 hash, or a
    // build that kept the public one). Cached under the hash the caller passed;
    // a miss is cached too (null), so an unresolvable native costs one lookup, not
    // two per call. Natives register all at once before the table validates, and
    // validation clears the cache, so a cached miss is never stale.
    for (uint64_t want : {buildHash(hash), hash}) {
        h = g_getNativeHandler ? g_getNativeHandler(g_nativeTable, want) : lookupInTable(g_nativeTable, want);
        if (h) break;
    }
    nativeCacheStore(hash, h);
    return h;
}

// ---------------------------------------------------------------------------
// D3D11 Present address (resolved by findPresent below)
// ---------------------------------------------------------------------------
uint8_t* g_presentAddr = nullptr;
// Slot 8 of the swap chain vtable (dxgi's data): the address the vtable hook replaces (present_hook.h).
void** g_presentSlot = nullptr;

// ---------------------------------------------------------------------------
// CE-style pattern scanner (for the native table scan)
// ---------------------------------------------------------------------------
bool parsePat(const char* s, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear(); mask.clear();
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char* tok = s;
        while (*tok && *tok != ' ') tok++;
        int len = (int)(tok - s);
        bool hex = (len == 2) &&
            (strchr("0123456789ABCDEFabcdef", s[0]) != nullptr) &&
            (strchr("0123456789ABCDEFabcdef", s[1]) != nullptr);
        if (hex) {
            bytes.push_back((uint8_t)strtoul(s, nullptr, 16));
            mask.push_back(1);
        } else if (len == 2 && s[0] == '?' && s[1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
        } else {
            return false;
        }
        s = tok;
    }
    return !bytes.empty() && bytes.size() == mask.size();
}

uint8_t* findPat(const uint8_t* base, size_t size, const char* sig) {
    std::vector<uint8_t> bytes, mask;
    if (!parsePat(sig, bytes, mask)) return nullptr;
    size_t L = bytes.size();
    if (L > size) return nullptr;
    const uint8_t* end = base + size - L;
    for (const uint8_t* p = base; p <= end; ++p) {
        bool ok = true;
        for (size_t i = 0; i < L; ++i) if (mask[i] && p[i] != bytes[i]) { ok = false; break; }
        if (ok) return (uint8_t*)p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Network-session recon (net_session=1)  -- FiveM-style local-session entry
// ---------------------------------------------------------------------------
// FiveM's gta-net-five/NetHook.cpp is the recipe to put the LOCAL game into a
// freemode session with no Rockstar matchmaking: stub rlSessionManager::QueryDetail
// to fake success, spoof the session/system keys + host check, disable UPnP/relay,
// then call the game's own hostGame(0,32,0) and poll g_isNetGame -> isSessionStarted().
// Before a single one of those sites is hooked, this pass scans the decrypted image
// for each signature and logs its hit count + address, so we only ever wire a
// signature that resolves to EXACTLY ONE site on THIS build. The patterns are
// FiveM's maintained Legacy sigs (build >=3407, closest to 3889) plus YimMenu v1
// cross-refs; every one must be confirmed unique here on 3889 before it is trusted
// -- a wrong byte-pattern would crash a live session call. Recon only: no hooks are
// installed and nothing in the game is called. The game decrypts its .text in
// stages (network code often not until the menu/online), so a background thread
// re-scans a few times (netSessionReconThread).
struct NetSig { const char* name; const char* sig; };
static const NetSig kNetSessionSigs[] = {
    // FiveM gta-net-five/NetHook.cpp (Legacy >=3407) -- PRIMARY
    { "g_isNetGame",             "48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B D9 74 5F" },
    { "isSessionStarted",        "8B 86 ?? ?? 00 00 C1 E8 05 84 C2" },
    { "hostGame",                "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 41 8A D8 8B FA 8B F1 E8 ?? ?? ?? ?? 45 33 C0" },
    { "joinGame",                "4C 89 74 24 ?? E8 ?? ?? ?? ?? EB ?? 32 C0" },
    { "rlSessionMgr_QueryDetail","E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 49 8B 8E A0 00 00 00" },
    { "sessionKey_site",         "74 15 48 8D 4C 24 78 E8" },
    { "systemKey_site",          "48 83 F8 FF 75 17 48 8D 0D" },
    { "hostSpoof_site",          "33 DB 48 85 C0 74 17 48 8B 48 10 48 85 C9 74 0E" },
    { "isNetworkHost",           "E8 ?? ?? ?? ?? 84 C0 74 ?? 44 38 3B 74" },
    { "netNoUpnp",               "8A D1 76 02 B2 01 48 39 0D" },
    { "netNoPcp",                "8A D1 EB 02 B2 01 48 39 0D" },
    { "relayVerify_off",         "80 3D ?? ?? ?? ?? ?? 49 8B F0 48 8B DA 48 8B F9 74" },
    { "scWaitingForInit",        "E8 ?? ?? ?? ?? 84 C0 75 B6 88 05" },
    // FiveM gta-net-five additional install sites (same source, re-sig for 3889) --
    // SECONDARY: max-slot count patch, "don't treat self as host for world reassign",
    // an obfuscated gate forced true, and the Summer_2025+ "allowed to run net game" gate.
    { "netMaxSlots",             "C7 87 ?? ?? ?? 00 18 00 00 00 C7 87" },
    { "netNotHostReassign",      "EB ?? 41 8A C4 84 C0 0F 84 ?? ?? ?? ?? 45 33 C0" },
    { "netUnkCheck1",            "84 C0 74 0F BA 03 00 00 00 48 8B CB E8 ?? ?? ?? ?? EB 0E" },
    { "netAllowRunGame",         "E8 ?? ?? ?? ?? 84 C0 74 ?? 33 D2 8D 4A ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 85 F6" },
    // TERTIARY: integration sites for the full port (online-address fn, network-mgr
    // ptr, NetworkBail, session migrate, profile settings, rlCreateUUID latch). Some
    // are anchored by a NEGATIVE offset into the real function (e.g. netAddressFunc
    // is match-0x61), so the match address here is the anchor, not the patch target.
    { "netAddressFunc",          "80 78 ?? 02 48 8B C8 0F 85" },
    { "netMgrPtr",               "84 C0 74 2E 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 20" },
    { "netBail",                 "48 83 EC ?? 80 3D ?? ?? ?? ?? ?? C6 05 ?? ?? ?? ?? ?? 0F 84" },
    { "netMigrateCmd",           "48 89 81 08 01 00 00 48 8B 87 00 01 00 00" },
    { "netProfileSettings",      "48 8B 0D ?? ?? ?? ?? 45 33 C0 B2 01 48 8B" },
    { "netRlCreateUUID",         "48 8D 0D ?? ?? ?? ?? 48 8D 14 03" },
    // YimMenu v1 (Legacy ~3407) cross-refs -- alternates if the FiveM sigs miss
    { "yim_m_network",           "48 8B 0D ?? ?? ?? ?? 48 8B D7 E8 ?? ?? ?? ?? 84 C0 75 17 48 8B 0D ?? ?? ?? ?? 48 8B D7" },
    { "yim_is_session_started",  "40 38 35 ?? ?? ?? ?? 75 0E 4C 8B C3 49 8B D7 49 8B CE" },
    { "yim_join_session_info",   "E8 ?? ?? ?? ?? 0F B6 CB 84 C0 41 0F 44 CD" },
    { "yim_host_token",          "48 8B 05 ?? ?? ?? ?? 48 83 F8 FF" },
    { "yim_network_player_mgr",  "48 8B 0D ?? ?? ?? ?? 8A D3 48 8B 01 FF 50 ?? 4C 8B 07 48 8B CF" },
    { "yim_network_object_mgr",  "48 8B 0D ?? ?? ?? ?? 45 33 C0 E8 ?? ?? ?? ?? 33 FF 4C 8B F0" },
};

// Count pattern hits across [base, base+size); records the first match. -1 = the
// pattern string itself was malformed. Mirrors findPat's inner match (same parsePat).
int countPat(const uint8_t* base, size_t size, const char* sig, const uint8_t** firstOut) {
    std::vector<uint8_t> bytes, mask;
    if (firstOut) *firstOut = nullptr;
    if (!parsePat(sig, bytes, mask)) return -1;
    size_t L = bytes.size();
    if (L == 0 || L > size) return 0;
    const uint8_t* end = base + size - L;
    int count = 0;
    for (const uint8_t* p = base; p <= end; ++p) {
        bool ok = true;
        for (size_t i = 0; i < L; ++i) if (mask[i] && p[i] != bytes[i]) { ok = false; break; }
        if (ok) { if (firstOut && !*firstOut) *firstOut = p; if (++count >= 1000) break; }
    }
    return count;
}

// Every hit of a pattern in one pass: keeps the first `cap` in `out` (address order), returns how many there are in
// all (at most 1000, like countPat; -1 = malformed pattern). noSpInstall uses it to choose among several Tick hits.
int findAllPat(const uint8_t* base, size_t size, const char* sig, const uint8_t** out, size_t cap, size_t* kept) {
    std::vector<uint8_t> bytes, mask;
    *kept = 0;
    if (!parsePat(sig, bytes, mask)) return -1;
    size_t L = bytes.size();
    if (L == 0 || L > size) return 0;
    const uint8_t* end = base + size - L;
    int count = 0;
    for (const uint8_t* p = base; p <= end; ++p) {
        bool ok = true;
        for (size_t i = 0; i < L; ++i) if (mask[i] && p[i] != bytes[i]) { ok = false; break; }
        if (!ok) continue;
        if (*kept < cap) out[(*kept)++] = p;
        if (++count >= 1000) break;
    }
    return count;
}

// Recon increment 1.5: for a uniquely-resolved site, log the decrypted bytes so the
// exact 3889 instructions can be disassembled offline (the on-disk GTA5.exe is
// Arxan-packed, and FiveM's patch bytes are build-specific). Read-only: dumps up to
// 96 bytes of the live image at the match, and when the pattern begins with a CALL
// (E8 rel32) also resolves that call's target image-relative -- the real function to
// hook (e.g. rlSessionManager::QueryDetail). Installs nothing.
static void dumpSiteBytes(const char* name, const char* sig, const uint8_t* first) {
    if (!first || !g_imageBase || !g_imageSize) return;
    const size_t kDump = 96;   // >= the far call/patch offsets FiveM uses (hostGame +0x31 etc.)
    size_t avail = (size_t)((g_imageBase + g_imageSize) - first);
    size_t ndump = avail < kDump ? avail : kDump;
    static const char H[] = "0123456789ABCDEF";
    char hex[kDump * 3 + 1];
    size_t p = 0;
    for (size_t i = 0; i < ndump; ++i) {
        hex[p++] = H[first[i] >> 4];
        hex[p++] = H[first[i] & 0xF];
        hex[p++] = ' ';
    }
    hex[p] = '\0';
    GLOGI("hook: [netsession]   %-26s bytes: %s", name, hex);
    if (sig[0] == 'E' && sig[1] == '8' && ndump >= 5) {
        uint32_t u = (uint32_t)first[1] | ((uint32_t)first[2] << 8) |
                     ((uint32_t)first[3] << 16) | ((uint32_t)first[4] << 24);
        const uint8_t* target = first + 5 + (int32_t)u;
        if (target >= g_imageBase && target < g_imageBase + g_imageSize)
            GLOGI("hook: [netsession]   %-26s   call -> +0x%llx", name,
                  (unsigned long long)(uintptr_t)(target - g_imageBase));
    }
}

// Log `len` decrypted bytes starting at anchor+from, 32 per line, each line labelled with its
// offset from the anchor (so "site-50" / "fn+0x57" style offsets can be read straight off the log).
// Read-only. False when the range is outside the image or not readable.
static bool dumpRange(const char* tag, const char* label, const uint8_t* anchor, ptrdiff_t from, size_t len) {
    const uint8_t* start = anchor + from;
    if (!g_imageBase || start < g_imageBase || start + len > g_imageBase + g_imageSize || !isReadable(start, len)) {
        GLOGW("hook: [%s] dump %s: range not readable; skipped", tag, label);
        return false;
    }
    GLOGI("hook: [%s] dump %s: anchor @ +0x%llx, %u bytes from anchor%c0x%x", tag, label,
          (unsigned long long)(uintptr_t)(anchor - g_imageBase), (unsigned)len, from < 0 ? '-' : '+',
          (unsigned)(from < 0 ? -from : from));
    static const char H[] = "0123456789ABCDEF";
    for (size_t row = 0; row < len; row += 32) {
        char hex[32 * 3 + 1];
        size_t p = 0;
        for (size_t i = row; i < row + 32 && i < len; ++i) {
            hex[p++] = H[start[i] >> 4];
            hex[p++] = H[start[i] & 0xF];
            hex[p++] = ' ';
        }
        hex[p] = '\0';
        const ptrdiff_t off = from + (ptrdiff_t)row;
        GLOGI("hook: [%s]   %s %c0x%03x: %s", tag, label, off < 0 ? '-' : '+', (unsigned)(off < 0 ? -off : off), hex);
    }
    return true;
}

// The two wider dumps the G1 port needs confirmed offline: 0x60 bytes BEFORE the scWaitingForInit
// call site (FiveM's _processEntitlements = resolveCall(site-50) lives there) plus the site itself,
// and 0x200 bytes from netAddressFunc's real function start (match-0x61; GetLocalPeerId at +0x57
// etc.). Read-only. True once both were dumped.
static bool dumpNetExtras(const char* tag) {
    bool ok = true;
    const uint8_t* sc = nullptr;
    if (countPat(g_imageBase, g_imageSize, "E8 ?? ?? ?? ?? 84 C0 75 B6 88 05", &sc) == 1)
        ok = dumpRange(tag, "scWaitingForInit", sc, -0x60, 0x70) && ok;
    else ok = false;
    const uint8_t* na = nullptr;
    if (countPat(g_imageBase, g_imageSize, "80 78 ?? 02 48 8B C8 0F 85", &na) == 1)
        ok = dumpRange(tag, "netAddressFn(match-0x61)", na - 0x61, 0, 0x200) && ok;
    else ok = false;
    if (!ok) GLOGW("hook: [%s] extra dumps incomplete (site not unique/decrypted yet); retried next pass", tag);
    return ok;
}

static void reconNetSession(int pass) {
    if (!g_netSession || !g_imageBase || !g_imageSize) return;
    if (g_build != &kProfile3889) {   // the patterns above are Legacy-3889 specific
        GLOGW("hook: [netsession] recon skipped: build is %s, patterns are Legacy 1.0.3889 only", g_build->label);
        return;
    }
    const int n = (int)(sizeof(kNetSessionSigs) / sizeof(kNetSessionSigs[0]));
    int uniq = 0, ambig = 0, miss = 0, bad = 0;
    GLOGI("hook: [netsession] recon pass %d: scanning %d session signatures (image@0x%llx size=%u)",
          pass, n, (unsigned long long)(uintptr_t)g_imageBase, (unsigned)g_imageSize);
    for (const NetSig& ns : kNetSessionSigs) {
        const uint8_t* first = nullptr;
        int c = countPat(g_imageBase, g_imageSize, ns.sig, &first);
        if (c < 0)       { ++bad;   GLOGW("hook: [netsession]   %-26s BAD PATTERN", ns.name); }
        else if (c == 0) { ++miss;  GLOGW("hook: [netsession]   %-26s missing (0)", ns.name); }
        else if (c == 1) { ++uniq;  GLOGI("hook: [netsession]   %-26s UNIQUE @ +0x%llx", ns.name,
                                          (unsigned long long)(uintptr_t)(first - g_imageBase));
                           dumpSiteBytes(ns.name, ns.sig, first); }
        else             { ++ambig; GLOGW("hook: [netsession]   %-26s ambiguous (%d, first @ +0x%llx)", ns.name, c,
                                          (unsigned long long)(uintptr_t)(first - g_imageBase)); }
    }
    static bool extrasDumped = false;
    if (!extrasDumped) extrasDumped = dumpNetExtras("netsession");
    GLOGI("hook: [netsession] recon pass %d done: %d unique, %d ambiguous, %d missing, %d bad "
          "(recon only, no hooks installed)", pass, uniq, ambig, miss, bad);
}

static DWORD WINAPI netSessionReconThread(void*) {
    // The network code decrypts in stages, often not until the menu/online, so scan
    // a few times over ~2 min and log each pass. Read-only: it scans the image and
    // installs nothing.
    static const DWORD kDelays[] = { 15000, 30000, 30000, 45000 };  // ~15 s, 45 s, 75 s, 120 s
    const int kPasses = (int)(sizeof(kDelays) / sizeof(kDelays[0]));
    for (int i = 0; i < kPasses; ++i) {
        Sleep(kDelays[i]);
        reconNetSession(i + 1);
    }
    GLOGI("hook: [netsession] recon thread finished (%d passes); set net_session=0 to silence", kPasses);
    return 0;
}

// ---------------------------------------------------------------------------
// Find the native registration table by scanning the game image.
// ---------------------------------------------------------------------------
// Strategy: scan for the hash-to-bucket-index instruction (two encodings:
// 83 E1 FF = AND ECX,0xFF, or 0F B6 C9 = MOVZX ECX,CL). Near it (±64 bytes)
// there is a LEA or MOV [rip+disp32] referencing the table:
//   48/4C 8D xx  — LEA  (the [rip+disp32] target IS the 256-entry table)
//   48/4C 8B xx  — MOV  (the target is a .data pointer TO the table)
// Many such pairs exist in the image, so we DON'T guess which is right — we
// collect them all and validate each by resolving known native hashes (below).

// Scan the game image and collect EVERY plausible native-table reference into
// g_candidates. We do not try to pick the right one here — the poller validates
// each candidate by resolving known native hashes through it, so a false
// positive simply never validates. This removes all build-specific guessing.
void collectNativeTableCandidates(const uint8_t* base, size_t size) {
    g_candidates.clear();
    if (size < 7) return;

    int patternHits = 0;
    const int kMaxCandidates = 512;  // keep the list bounded

    const uint8_t* scanEnd = base + size - 3;
    for (const uint8_t* p = base; p <= scanEnd; ++p) {
        // Bucket-mask instruction, two known encodings:
        //   83 E1 FF    AND ECX, 0xFF
        //   0F B6 C9    MOVZX ECX, CL
        bool isAndEcx = (p[0] == 0x83 && p[1] == 0xE1 && p[2] == 0xFF);
        bool isMovzx  = (p[0] == 0x0F && p[1] == 0xB6 && p[2] == 0xC9);
        if (!isAndEcx && !isMovzx)
            continue;

        ++patternHits;

        // Search within ±64 bytes for a LEA or MOV [rip+disp32].
        const int searchRange = 64;
        const uint8_t* lo = (p - searchRange > base) ? (p - searchRange) : base;
        const uint8_t* hi = (p + searchRange + 7 < base + size) ? (p + searchRange) : (base + size - 7);

        for (const uint8_t* q = lo; q <= hi; ++q) {
            // REX.W (48) or REX.WR (4C), then LEA (8D) or MOV (8B).
            if ((q[0] != 0x48 && q[0] != 0x4C) || (q[1] != 0x8D && q[1] != 0x8B))
                continue;
            uint8_t modrm = q[2];
            if ((modrm & 0xC7) != 0x05)  // must be [rip+disp32]
                continue;

            int32_t disp = 0;
            memcpy(&disp, q + 3, 4);
            uint8_t** addr = (uint8_t**)((uintptr_t)(q + 7) + (int64_t)disp);

            // The resolved address must be inside the game image.
            if ((uintptr_t)addr < (uintptr_t)base ||
                (uintptr_t)addr + 8 > (uintptr_t)base + size)
                continue;

            bool isLea = (q[1] == 0x8D);
            // Dedup (same addr + kind).
            bool dup = false;
            for (auto& c : g_candidates)
                if (c.addr == addr && c.isLea == isLea) { dup = true; break; }
            if (!dup && (int)g_candidates.size() < kMaxCandidates)
                g_candidates.push_back({ addr, isLea });
        }
    }
    (void)patternHits;  // silent: the poller calls this on every attempt
}

// Resolve a candidate to the table pointer to walk. LEA candidates ARE the
// table; MOV candidates hold a .data pointer TO the table (may be null early).
uint8_t** resolveCandidate(const NTCandidate& c) {
    if (c.isLea) return c.addr;
    if (!isReadable(c.addr, sizeof(uint8_t*))) return nullptr;
    return *(uint8_t***)c.addr;
}

// Try every candidate; the real table resolves BOTH probe hashes (two distinct
// buckets, so a lucky single-hash false match is ruled out). Returns the winning
// table or nullptr if none validate yet (natives not registered this frame).
uint8_t** validateCandidates(NodeLayout& layoutOut) {
    for (NodeLayout l : {NodeLayout::Plain, NodeLayout::AddrXor}) {
        for (auto& c : g_candidates) {
            uint8_t** t = resolveCandidate(c);
            if (!t) continue;
            if (lookupInTable(t, buildHash(native::PLAYER_PED_ID), l) &&
                lookupInTable(t, buildHash(native::GET_ENTITY_COORDS), l)) {
                layoutOut = l;
                return t;
            }
        }
    }
    return nullptr;
}

// What a candidate table looks like when walked with one layout: nodes and
// entries that decode, handlers inside the game image, and hashes that sit in
// their own bucket (hash & 0xFF == bucket: only right when the hash decoding
// is right). Tells "wrong table or layout" from "right table, other hashes".
struct TableStats {
    int nodes = 0, entries = 0, inImage = 0, bucketOk = 0;
    uint64_t sample[3] = {};
};

TableStats tableStats(uint8_t** t, NodeLayout l) {
    TableStats s;
    if (!t || !isReadable(t, 256 * sizeof(uint8_t*))) return s;
    const uintptr_t lo = (uintptr_t)g_imageBase, hi = lo + g_imageSize;
    for (int b = 0; b < 256; ++b) {
        uint8_t* reg = t[b];
        for (int safety = 0; reg && safety < 64; ++safety) {
            if (!isReadable(reg, nodeSize(l))) break;
            const uint32_t count = nodeCount(reg, l);
            if (count > 7) break;
            ++s.nodes;
            for (uint32_t i = 0; i < count; ++i) {
                const uint64_t h = nodeHash(reg, i, l);
                const uintptr_t fn = (uintptr_t)regGetHandler(reg, i);
                if (s.entries < 3) s.sample[s.entries] = h;
                ++s.entries;
                if (fn >= lo && fn < hi) ++s.inImage;
                if ((int)(h & 0xFF) == b) ++s.bucketOk;
            }
            reg = nodeNext(reg, l);
        }
    }
    return s;
}

// Logged once when nothing validated in time: the best candidate per layout.
void logCandidateStats() {
    for (NodeLayout l : {NodeLayout::Plain, NodeLayout::AddrXor}) {
        TableStats best;
        uint8_t** bestT = nullptr;
        for (auto& c : g_candidates) {
            uint8_t** t = resolveCandidate(c);
            const TableStats s = tableStats(t, l);
            if (s.bucketOk > best.bucketOk || (s.bucketOk == best.bucketOk && s.entries > best.entries)) {
                best = s;
                bestT = t;
            }
        }
        GLOGW("hook: [poller] layout %s, best table %p: %d nodes, %d entries, %d handlers in the image, "
              "%d hashes in their own bucket; sample %016llx %016llx %016llx",
              layoutName(l), (void*)bestT, best.nodes, best.entries, best.inImage, best.bucketOk,
              (unsigned long long)best.sample[0], (unsigned long long)best.sample[1],
              (unsigned long long)best.sample[2]);
    }
}

// Locate get_native_handler and its table through kNativeLookupSig. Scans until
// the (Arxan-decrypted) code shows up, then caches. Poller thread only.
uint8_t**          g_sigTable = nullptr;
GetNativeHandlerFn g_sigFn = nullptr;

bool findNativeLookup() {
    if (g_sigFn) return true;
    if (!g_build->nativeLookupSig) return false;  // not derived for this build; node-walk is the fallback
    uint8_t* p = findPat(g_imageBase, g_imageSize, g_build->nativeLookupSig);
    if (!p) return false;
    int32_t dTable = 0, dCall = 0;
    memcpy(&dTable, p + 3, 4);   // lea rcx,[rip+dTable], ends at p+7
    memcpy(&dCall, p + 12, 4);   // call rel32 at p+11, ends at p+16
    uint8_t* t = p + 7 + (int64_t)dTable;
    uint8_t* f = p + 16 + (int64_t)dCall;
    const uintptr_t lo = (uintptr_t)g_imageBase, hi = lo + g_imageSize;
    static bool loggedBad = false;
    if ((uintptr_t)t < lo || (uintptr_t)t + 256 * 8 > hi || (uintptr_t)f < lo || (uintptr_t)f >= hi) {
        if (!loggedBad)
            GLOGW("hook: [poller] native lookup signature at GTA5.exe+0x%llx points outside the image "
                  "(table %p, call %p); ignored", (unsigned long long)(p - g_imageBase), (void*)t, (void*)f);
        loggedBad = true;
        return false;
    }
    g_sigTable = (uint8_t**)t;
    g_sigFn = (GetNativeHandlerFn)f;
    GLOGI("hook: [poller] native lookup signature at GTA5.exe+0x%llx: table GTA5.exe+0x%llx, "
          "get_native_handler GTA5.exe+0x%llx", (unsigned long long)(p - g_imageBase),
          (unsigned long long)(t - g_imageBase), (unsigned long long)(f - g_imageBase));
    return true;
}

// Hex of the first bytes of a bucket's head node, logged once on success so the
// node encoding of this build is on record for the walker.
void logBucketHead(uint8_t** t, uint64_t hash) {
    const uint8_t* n = t[hash & 0xFF];
    char hex[0x60 * 3 + 1] = "";
    if (isReadable(n, 0x60))
        for (int i = 0; i < 0x60; ++i) snprintf(hex + i * 3, 4, "%02x ", n[i]);
    GLOGI("hook: [poller] bucket %02x head %p: %s", (unsigned)(hash & 0xFF), (const void*)n,
          hex[0] ? hex : "(unreadable)");
}

// True when `p` lies in committed executable memory (handlers may sit in an
// Arxan-allocated region outside GTA5.exe, so "in the image" is too strict).
inline bool isExecutable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           !(mbi.Protect & PAGE_GUARD);
}

// Walk table `t` with layout `l`, write every entry to natives-dump.txt next to
// gage_mp.dll ("hash bucket handler" per line, handler as a GTA5.exe offset when
// inside the image) and log how many well-known public NativeDB hashes are in
// it: tells "public hashes, our lookup is wrong" from "this build re-hashed".
void dumpNativeTable(uint8_t** t, NodeLayout l) {
    static const struct { const char* name; uint64_t hash; } kKnown[] = {
        {"WAIT", 0x4EDE34FBADD967A6}, {"START_NEW_SCRIPT", 0xE81651AD79516E48},
        {"TERMINATE_THIS_THREAD", 0x1090044AD1DA76FA}, {"PLAYER_ID", 0x4F8644AF03D0E0D6},
        {"PLAYER_PED_ID", native::PLAYER_PED_ID}, {"GET_ENTITY_COORDS", native::GET_ENTITY_COORDS},
        {"DOES_ENTITY_EXIST", 0x7239B21A38F536BA}, {"GET_GAME_TIMER", 0x9CD27B0045628463},
        {"GET_FRAME_TIME", 0x15C40837039FFAF7}, {"GET_HASH_KEY", 0xD24D37CC275948CC},
    };
    if (!t || !isReadable(t, 256 * sizeof(uint8_t*))) return;

    wchar_t path[MAX_PATH] = L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&dumpNativeTable, &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\') --n;
    path[n] = 0;
    wcsncat(path, L"natives-dump.txt", MAX_PATH - 1 - wcslen(path));
    FILE* f = _wfopen(path, L"w");

    const uintptr_t lo = (uintptr_t)g_imageBase, hi = lo + g_imageSize;
    int nodes = 0, entries = 0, bucketOk = 0, inImage = 0;
    bool known[sizeof kKnown / sizeof kKnown[0]] = {};
    for (int b = 0; b < 256; ++b) {
        uint8_t* reg = t[b];
        for (int safety = 0; reg && safety < 64; ++safety) {
            if (!isReadable(reg, nodeSize(l))) break;
            const uint32_t count = nodeCount(reg, l);
            if (count > 7) break;
            ++nodes;
            for (uint32_t i = 0; i < count; ++i) {
                const uint64_t h = nodeHash(reg, i, l);
                const uintptr_t fn = (uintptr_t)regGetHandler(reg, i);
                ++entries;
                bucketOk += (int)(h & 0xFF) == b;
                const bool in = fn >= lo && fn < hi;
                inImage += in;
                for (size_t k = 0; k < sizeof kKnown / sizeof kKnown[0]; ++k)
                    known[k] |= kKnown[k].hash == h || buildHash(kKnown[k].hash) == h;
                if (f) fprintf(f, "%016llx %02x %s%llx\n", (unsigned long long)h, b, in ? "+" : "@",
                               (unsigned long long)(in ? fn - lo : fn));
            }
            reg = nodeNext(reg, l);
        }
    }
    if (f) fclose(f);

    std::string missing;
    int found = 0;
    for (size_t k = 0; k < sizeof kKnown / sizeof kKnown[0]; ++k) {
        if (known[k]) { ++found; continue; }
        if (!missing.empty()) missing += ' ';
        missing += kKnown[k].name;
    }
    GLOGW("hook: [poller] table walk (%s): %d nodes, %d entries, %d in their own bucket, %d handlers in "
          "GTA5.exe; %d/%d well-known natives present (public or 3889 hash)%s%s", layoutName(l), nodes, entries, bucketOk,
          inImage, found, (int)(sizeof kKnown / sizeof kKnown[0]), missing.empty() ? "" : "; missing: ",
          missing.c_str());
    GLOGI("hook: [poller] natives-dump.txt %s next to gage_mp.dll (%d entries)", f ? "written" : "NOT written",
          entries);
}

// Lookup diagnostics, read by the 60 s report.
int           g_tableFilled = -1;        // non-null slots of the 256 at the last poll
NativeHandler g_lastA = nullptr, g_lastB = nullptr;
bool          g_lookupCalled = false;

// Resolve both probe hashes through the game's own lookup. The table is a static
// array in GTA5.exe, so it is readable from the start. The bucket index is the
// game's business (it need not be hash & 0xFF on this build), so the lookup is
// called once the table is non-empty and has not changed for 2 polls (~150 ms):
// registration has (almost certainly) finished, no node is being linked while we
// walk. A native that registers later is still found: only hits are cached.
bool tryGameLookup(int attempt) {
    if (!findNativeLookup() || !isReadable(g_sigTable, 256 * sizeof(uint8_t*)))
        return false;
    static uint64_t lastPrint = 0;
    static int stable = 0;
    uint64_t print = 0;
    int filled = 0;
    for (int i = 0; i < 256; ++i) {
        print = print * 0x100000001B3ull ^ (uint64_t)(uintptr_t)g_sigTable[i];
        filled += g_sigTable[i] != nullptr;
    }
    if (filled != g_tableFilled)
        GLOGI("hook: [poller] native table %d/256 slots filled (attempt %d)", filled, attempt);
    g_tableFilled = filled;
    if (filled == 0 || print != lastPrint) {
        lastPrint = print;
        stable = 0;
        return false;
    }
    if (++stable < 2)
        return false;

    const NativeHandler a = g_sigFn(g_sigTable, buildHash(native::PLAYER_PED_ID));
    const NativeHandler b = g_sigFn(g_sigTable, buildHash(native::GET_ENTITY_COORDS));
    const uintptr_t lo = (uintptr_t)g_imageBase;
    if (!g_lookupCalled || a != g_lastA || b != g_lastB)
        GLOGI("hook: [poller] get_native_handler: PLAYER_PED_ID -> %p (%s), GET_ENTITY_COORDS -> %p (%s)",
              (void*)a, isExecutable((void*)a) ? "code" : "not code", (void*)b,
              isExecutable((void*)b) ? "code" : "not code");
    g_lookupCalled = true;
    g_lastA = a;
    g_lastB = b;
    if (!isExecutable((void*)a) || !isExecutable((void*)b)) {
        // The game's lookup found nothing: walk the same table ourselves with the
        // address-XOR node layout (3889's bucket heads decode with it).
        const NativeHandler wa = lookupInTable(g_sigTable, buildHash(native::PLAYER_PED_ID), NodeLayout::AddrXor);
        const NativeHandler wb = lookupInTable(g_sigTable, buildHash(native::GET_ENTITY_COORDS), NodeLayout::AddrXor);
        if (!isExecutable((void*)wa) || !isExecutable((void*)wb))
            return false;
        g_nativeTable = g_sigTable;
        g_nodeLayout = NodeLayout::AddrXor;  // before the atomic store that publishes the table
        g_getNativeHandler = nullptr;
        nativeCacheClear();
        g_nativeTableReady = true;
        GLOGI("hook: [poller] native table VALIDATED on attempt %d by walking it (layout addr-xor; the game's "
              "lookup returned nothing): PLAYER_PED_ID -> %p, GET_ENTITY_COORDS -> %p", attempt, (void*)wa,
              (void*)wb);
        return true;
    }
    logBucketHead(g_sigTable, buildHash(native::PLAYER_PED_ID));
    g_nativeTable = g_sigTable;
    g_getNativeHandler = g_sigFn;  // before the atomic store that publishes the table
    nativeCacheClear();
    g_nativeTableReady = true;
    GLOGI("hook: [poller] native table VALIDATED on attempt %d through the game's own lookup "
          "(PLAYER_PED_ID -> %p, GET_ENTITY_COORDS -> %p; GTA5.exe at %p)", attempt, (void*)a, (void*)b,
          (void*)lo);
    return true;
}

// ---------------------------------------------------------------------------
// Enhanced native resolver (scrProgram link via InitNativeTables)
// ---------------------------------------------------------------------------
// Enhanced (1158) has no walkable get_native_handler bucket table — the image
// scan only ever finds false positives (see the .nativetable diagnostic dump),
// so the Legacy path never validates. The game instead resolves a native hash
// to its handler inside InitNativeTables(scrProgram*): it reads each entry of
// program->m_NativeEntrypoints as a hash and overwrites it in place with the
// resolved handler (or the game's own DEFAULT handler when the hash is not in
// its registry). We hand it a zeroed fake scrProgram whose entrypoint array is
// pre-filled with our crossmap's build hashes plus one sentinel, call the game's
// own function, and read the handlers back — YimMenuV2's CacheHandlers trick.
// The default handler is executable too, so a native counts as resolved only
// when its slot differs from the sentinel slot (which took that default). The
// resolved handlers go straight into g_nativeCache keyed by the PUBLIC hash, so
// every existing caller (findNativeHandler -> HUD/spawn/vehicle paths) is
// unchanged. Only the crossmap's natives resolve on Enhanced (no table for the
// rest) — the same curated set the engine already uses. The scrProgram field
// offsets (m_NativeCount 0x2C, m_NativeEntrypoints 0x40) and the "NativeCount==0
// returns" and default-handler-on-miss behaviours were read off the dumped
// disassembly of this function, which is why an early call is crash-safe.
bool tryInitNativeTablesResolve() {
    if (!g_build->initNativeTablesSig || !g_build->crossmap || !g_build->crossmapCount)
        return false;  // Legacy / unported build: bucketed path only
    uint8_t* mt = findPat(g_imageBase, g_imageSize, g_build->initNativeTablesSig);
    if (!mt) return false;  // function not decrypted yet; poller retries
    uint8_t* fn = mt + g_build->initNativeTablesFnOffset;  // entry = match - 0x2A on 1158
    if (!isExecutable(fn)) return false;

    const size_t N = g_build->crossmapCount;
    // Slots: N build hashes + 1 sentinel (a 64-bit value no native uses).
    // InitNativeTables rewrites each in place with the resolved handler, or the
    // DEFAULT handler on a miss — so the sentinel slot reveals that default.
    const uint64_t kSentinel = 0xD1E7C0DE4B1D5EEDull;
    std::vector<uint64_t> slots(N + 1);
    for (size_t i = 0; i < N; ++i) slots[i] = g_build->crossmap[i].b3889;
    slots[N] = kSentinel;

    std::vector<uint8_t> prog(0x80, 0);  // zeroed scrProgram; only two fields set
    *(uint32_t*)(prog.data() + g_build->scrProgNativeCount)        = (uint32_t)(N + 1);
    *(uint64_t**)(prog.data() + g_build->scrProgNativeEntrypoints) = slots.data();

    using InitNativeTablesFn = void (*)(void*);
    ((InitNativeTablesFn)fn)(prog.data());  // game resolves hashes -> handlers in place

    const uint64_t defHandler = slots[N];  // the default handler (from the sentinel)
    auto slotFor = [&](uint64_t pub) -> uint64_t {
        for (size_t i = 0; i < N; ++i)
            if (g_build->crossmap[i].pub == pub) return slots[i];
        return 0;
    };
    const uint64_t vPed = slotFor(native::PLAYER_PED_ID), vCoords = slotFor(native::GET_ENTITY_COORDS);
    const bool ok = vPed && vCoords && vPed != defHandler && vCoords != defHandler &&
                    isExecutable((void*)(uintptr_t)vPed) && isExecutable((void*)(uintptr_t)vCoords);
    static bool loggedWait = false;
    if (!ok) {
        if (!loggedWait) {
            GLOGI("hook: [poller] InitNativeTables at GTA5.exe+0x%llx ran; natives not registered yet "
                  "(default handler %p, PLAYER_PED_ID -> %p); retrying",
                  (unsigned long long)(fn - g_imageBase), (void*)(uintptr_t)defHandler, (void*)(uintptr_t)vPed);
            loggedWait = true;
        }
        return false;
    }

    int resolved = 0;
    for (size_t i = 0; i < N; ++i) {
        const uint64_t h = slots[i];
        if (h && h != defHandler && isExecutable((void*)(uintptr_t)h)) {
            nativeCacheStore(g_build->crossmap[i].pub, (NativeHandler)(uintptr_t)h);
            ++resolved;
        }
    }
    g_nativeTable = nullptr;       // Enhanced resolves through the cache, not a table
    g_getNativeHandler = nullptr;
    g_nativeTableReady = true;
    GLOGI("hook: [poller] natives VALIDATED via InitNativeTables (Enhanced scrProgram link) at "
          "GTA5.exe+0x%llx: %d/%zu resolved, default handler %p (PLAYER_PED_ID -> %p, GET_ENTITY_COORDS -> %p)",
          (unsigned long long)(fn - g_imageBase), resolved, N, (void*)(uintptr_t)defHandler,
          (void*)(uintptr_t)vPed, (void*)(uintptr_t)vCoords);
    return true;
}

// ---------------------------------------------------------------------------
// Find D3D11 Present via a dummy device and swap chain.
// ---------------------------------------------------------------------------
// Minimal inline definitions to avoid MinGW D3D11 header issues.
struct DummyGUID { uint32_t a; uint16_t b, c; uint8_t d[8]; };

uint8_t* findPresent() {
    // Load d3d11.dll dynamically.
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        GLOGW("hook: d3d11.dll not loaded");
        return nullptr;
    }

    using PFN_D3D11CreateDeviceAndSwapChain = long(__stdcall*)(
        void* pAdapter,
        int DriverType,
        HMODULE Software,
        UINT Flags,
        const int* pFeatureLevels,
        UINT FeatureLevels,
        UINT SDKVersion,
        const void* pSwapChainDesc,
        void** ppSwapChain,
        void** ppDevice,
        int* pFeatureLevel,
        void** ppImmediateContext);

    auto createFn = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!createFn) {
        GLOGW("hook: D3D11CreateDeviceAndSwapChain not found");
        return nullptr;
    }

    // Build a minimal DXGI_SWAP_CHAIN_DESC inline. We need a real HWND, and it
    // must be our own: a swap chain makes DXGI associate with its window, and the
    // foreground window this early is usually the game's own window being created
    // (the game then went black and stopped responding; passive mode, which skips
    // this, loaded fine). A hidden window, created and destroyed on this thread.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&findPresent, &self);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = self;
    wc.lpszClassName = L"GageDxgiProbe";
    RegisterClassExW(&wc);  // already registered (a second Init) is fine
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100, 100,
                                nullptr, nullptr, self, nullptr);
    if (!hwnd) {
        GLOGW("hook: dummy window for the Present probe failed (%lu)", GetLastError());
        UnregisterClassW(wc.lpszClassName, self);
        return nullptr;
    }
    auto dropWindow = [&]() {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, self);
    };

    // DXGI_SWAP_CHAIN_DESC as a flat byte buffer to avoid header dependency.
    // Layout (x64, natural alignment):
    //   +0  DXGI_MODE_DESC (28 bytes: Width/Height/RefreshRate/Format/Scan/Scaling)
    //  +28  DXGI_SAMPLE_DESC (8 bytes: Count/Quality)
    //  +36  BufferUsage (4)     +40  BufferCount (4)
    //  +44  (4 pad)             +48  OutputWindow (HWND, 8)
    //  +56  Windowed (4)        +60  SwapEffect (4)     +64  Flags (4)
    uint8_t scdBuf[128];
    memset(scdBuf, 0, sizeof(scdBuf));
    // Width, Height at +0, +4
    *(uint32_t*)(scdBuf + 0) = 100;
    *(uint32_t*)(scdBuf + 4) = 100;
    // RefreshRate Numerator/Denominator at +8, +12
    *(uint32_t*)(scdBuf + 8)  = 60;
    *(uint32_t*)(scdBuf + 12) = 1;
    // Format at +16: DXGI_FORMAT_R8G8B8A8_UNORM = 28
    *(uint32_t*)(scdBuf + 16) = 28;
    // SampleDesc.Count at +28
    *(uint32_t*)(scdBuf + 28) = 1;
    // BufferUsage at +36: DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20
    *(uint32_t*)(scdBuf + 36) = 0x20;
    // BufferCount at +40
    *(uint32_t*)(scdBuf + 40) = 1;
    // OutputWindow at +48 (after padding to 8-byte alignment)
    *(uint64_t*)(scdBuf + 48) = (uint64_t)(uintptr_t)hwnd;
    // Windowed at +56
    *(uint32_t*)(scdBuf + 56) = 1;

    void* pSwapChain = nullptr;
    void* pDevice = nullptr;
    void* pContext = nullptr;
    // D3D_DRIVER_TYPE_HARDWARE = 1, D3D11_SDK_VERSION = 7
    long hr = createFn(nullptr, 1, nullptr, 0, nullptr, 0, 7,
                       scdBuf, &pSwapChain, &pDevice, nullptr, &pContext);
    if (hr < 0 || !pSwapChain) {
        // No usable hardware device (a VM, a GPU-less box, a driver mid-reset): the software rasteriser makes the
        // same dxgi swap chain class, hence the same Present function and vtable. D3D_DRIVER_TYPE_WARP = 5.
        GLOGW("hook: hardware D3D11 probe failed (0x%08lx); retrying on WARP", (unsigned long)hr);
        if (pSwapChain) ((void(__stdcall*)(void*))(*(void***)pSwapChain)[2])(pSwapChain);
        if (pDevice) ((void(__stdcall*)(void*))(*(void***)pDevice)[2])(pDevice);
        if (pContext) ((void(__stdcall*)(void*))(*(void***)pContext)[2])(pContext);
        pSwapChain = pDevice = pContext = nullptr;
        hr = createFn(nullptr, 5, nullptr, 0, nullptr, 0, 7,
                      scdBuf, &pSwapChain, &pDevice, nullptr, &pContext);
    }

    if (hr < 0 || !pSwapChain) {
        GLOGW("hook: D3D11CreateDeviceAndSwapChain failed (0x%08lx)", (unsigned long)hr);
        if (pDevice) ((void(**)(void*))*(void**)pDevice)[2](pDevice);
        if (pContext) ((void(**)(void*))*(void**)pContext)[2](pContext);
        dropWindow();
        return nullptr;
    }

    // Read vtable[8] = Present.
    void** vtable = *(void***)pSwapChain;
    uint8_t* present = (uint8_t*)vtable[8];
    // The class vtable is shared by every swap chain of this class (the game's included) and stays mapped as long
    // as dxgi.dll does: the vtable hook replaces this slot rather than patching the function's code.
    g_presentSlot = &vtable[8];

    GLOGI("hook: D3D11 Present at %p, vtable slot %p (from dummy swap chain)", present, (void*)g_presentSlot);

    // Release all COM objects: Release = vtable[2].
    ((void(__stdcall*)(void*))vtable[2])(pSwapChain);
    void** devVt = *(void***)pDevice;
    ((void(__stdcall*)(void*))devVt[2])(pDevice);
    void** ctxVt = *(void***)pContext;
    ((void(__stdcall*)(void*))ctxVt[2])(pContext);
    dropWindow();  // after the swap chain is gone

    return present;
}

} // namespace

// Defined with the native-table poller below; World uses it first.
DWORD WINAPI nativeTablePollerThread(LPVOID);
// Defined with the poller too; installScriptTickWhenReady keeps it running while it waits.
static void pollCodePatches();
// G0 network baseline (net_probe=1); defined after CallNative, run from scriptTickBody.
static void netProbeTick();

// ---------------------------------------------------------------------------
// World API
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Network-session hosting primitives (net_host=1) -- FiveM-style local session
// ---------------------------------------------------------------------------
// P1: the raw patch toolkit the host stubs are built from. Each write records
// what it overwrote in g_netPatches so netUnpatchAll() restores the game image
// byte-for-byte on Unhook. Inert until netHostInstall() runs (net_host=1); the
// recon pass (net_session) never touches these. decIns / allocNear / flushRWX and
// the image bounds are all defined above. Proven in a local sandbox before any
// live run: correct bytes, instruction-boundary padding, exact restore.
static const size_t kNetPatchMax = 32;
struct NetPatch {
    uint8_t* addr = nullptr;          // where in the game image we wrote
    uint8_t  orig[kNetPatchMax] = {}; // the original bytes, for restore
    size_t   len = 0;                 // how many bytes we overwrote
    void*    thunk = nullptr;         // a far-jmp thunk page to free (netCall), or null
};
std::vector<NetPatch> g_netPatches;

// True when [p, p+len) lies entirely within the game image.
bool netInImage(const void* p, size_t len) {
    if (!g_imageBase || !g_imageSize || !p) return false;
    const uint8_t* b = (const uint8_t*)p;
    return b >= g_imageBase && b + len <= g_imageBase + g_imageSize;
}

// Resolve the E8 rel32 CALL at `site` to its absolute target (in image), or null.
uint8_t* netResolveCall(const uint8_t* site) {
    if (!netInImage(site, 5)) return nullptr;
    int32_t rel = 0;
    memcpy(&rel, site + 1, 4);
    uint8_t* target = (uint8_t*)site + 5 + (intptr_t)rel;
    return netInImage(target, 1) ? target : nullptr;
}

// Resolve the [rip+disp32] operand of the instruction at `ins` to the address it
// references (e.g. a .data global). null if it has no rip-relative operand.
uint8_t* netResolveRip(const uint8_t* ins) {
    if (!netInImage(ins, 1)) return nullptr;
    size_t avail = (size_t)((g_imageBase + g_imageSize) - ins);
    if (avail > 16) avail = 16;
    size_t len = 0; int ripOff = -1;
    if (!decIns(ins, avail, len, ripOff) || ripOff < 0) return nullptr;
    int32_t disp = 0;
    memcpy(&disp, ins + ripOff, 4);
    uint8_t* target = (uint8_t*)ins + len + (intptr_t)disp;
    return netInImage(target, 1) ? target : nullptr;
}

// --- Thread freeze for live-code writes (frozenWrite) ---------------------------------------
// The thread list is read with NtQuerySystemInformation(SystemProcessInformation) into memory
// VirtualAlloc'd up front, never with CreateToolhelp32Snapshot: Toolhelp's internal buffer handling
// is not documented to stay off the process heap, and the list is re-read INSIDE the freeze, where a
// suspended thread may hold the heap or logger lock. Inside the window only syscall stubs run
// (NtQuerySystemInformation, OpenThread, Suspend/ResumeThread, GetThreadContext, VirtualProtect,
// FlushInstructionCache) on that reserved memory: no allocation, no logging, no CRT locks. An open
// handle keeps a thread's id from being reused, so a known id is always the same thread.
// Stable x64 layouts (NT 6.0+); the threads array follows its process entry.
struct SpiProcess {                    // SYSTEM_PROCESS_INFORMATION
    ULONG   nextEntryOffset;           // 0x00; 0 on the last entry
    ULONG   numberOfThreads;           // 0x04
    uint8_t reserved0[0x48];
    HANDLE  uniqueProcessId;           // 0x50
    uint8_t reserved1[0x100 - 0x58];
};
struct SpiThread {                     // SYSTEM_THREAD_INFORMATION
    uint8_t reserved0[0x28];
    HANDLE  uniqueProcess;             // 0x28 ClientId
    HANDLE  uniqueThread;              // 0x30
    uint8_t reserved1[0x50 - 0x38];
};
static_assert(sizeof(SpiProcess) == 0x100 && offsetof(SpiProcess, uniqueProcessId) == 0x50, "SPI x64 layout");
static_assert(sizeof(SpiThread) == 0x50 && offsetof(SpiThread, uniqueThread) == 0x30, "STI x64 layout");
typedef LONG(NTAPI* NtQsiFn)(ULONG, PVOID, ULONG, PULONG);
const ULONG kSystemProcessInformation = 5;
const LONG  kStatusInfoLengthMismatch = (LONG)0xC0000004;

const int kFreezeAttempts   = 200;     // busy/unstable retries per write
const int kFreezeScanRounds = 8;       // re-enumerations per freeze before the set must be stable
const int kFreezeRegrows    = 8;       // buffer regrows (thread list larger than reserved)

// Every thread of this process but the caller: ids, open handles, and which ones WE hold suspended.
// All of it lives in VirtualAlloc'd memory sized before the freeze (freezeReserve, outside only).
struct Freeze {
    NtQsiFn  qsi = nullptr;
    DWORD    pid = 0, self = 0;
    uint8_t* spi = nullptr;  size_t spiSize = 0, spiNeed = 0;   // the system thread list
    HANDLE*  h = nullptr;    DWORD* tid = nullptr;  uint8_t* held = nullptr;
    size_t   n = 0, cap = 0;
};
enum class FreezeStep { Written, Busy, Unstable, GrowSpi, GrowCap, Unwritable, Failed };

// (Re)size the list buffer and the thread arrays, keeping the threads already gathered. Outside the
// freeze only. The list buffer gets half again plus 64 KB of headroom for threads created later.
bool freezeReserve(Freeze& f, size_t spiNeed, size_t capNeed) {
    if (spiNeed > f.spiSize) {
        const size_t sz = (spiNeed + spiNeed / 2 + 0x10000 + 0xFFFF) & ~(size_t)0xFFFF;
        void* p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return false;
        if (f.spi) VirtualFree(f.spi, 0, MEM_RELEASE);
        f.spi = (uint8_t*)p; f.spiSize = sz;
    }
    if (capNeed > f.cap) {
        const size_t cap = capNeed + capNeed / 2 + 64;
        uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, cap * (sizeof(HANDLE) + sizeof(DWORD) + 1),
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return false;
        HANDLE* h = (HANDLE*)p; DWORD* tid = (DWORD*)(h + cap); uint8_t* held = (uint8_t*)(tid + cap);
        if (f.n) {
            memcpy(h, f.h, f.n * sizeof(HANDLE));
            memcpy(tid, f.tid, f.n * sizeof(DWORD));
            memcpy(held, f.held, f.n);
        }
        if (f.h) VirtualFree(f.h, 0, MEM_RELEASE);
        f.h = h; f.tid = tid; f.held = held; f.cap = cap;
    }
    return true;
}

// Suspend thread `i` and wait for it to stop (GetThreadContext only returns once the suspension has
// taken effect, so a "held" thread is really stopped, mid-syscall ones included). True when its RIP
// is inside [addr, addr+len).
bool freezeHoldInRange(Freeze& f, size_t i, const uint8_t* addr, size_t len) {
    f.held[i] = SuspendThread(f.h[i]) != (DWORD)-1;    // -1: the thread is exiting
    if (!f.held[i]) return false;
    CONTEXT c;
    c.ContextFlags = CONTEXT_CONTROL;
    return GetThreadContext(f.h[i], &c) &&
           c.Rip >= (DWORD64)(uintptr_t)addr && c.Rip < (DWORD64)(uintptr_t)(addr + len);
}

// Read the process's threads and add the ones not yet known. `suspend` (inside the freeze): each new
// one is held at once; returns how many were newly held, with `busy` set (and the scan cut short)
// when one is in the range. Outside: only opens handles; returns how many were added. Negative steps
// (GrowSpi / GrowCap / Failed) say why it stopped; anything it already held is in f, to resume.
int freezeScan(Freeze& f, bool suspend, const uint8_t* addr, size_t len, bool& busy, FreezeStep& why) {
    ULONG ret = 0;
    const LONG st = f.qsi(kSystemProcessInformation, f.spi, (ULONG)f.spiSize, &ret);
    if (st == kStatusInfoLengthMismatch) {
        f.spiNeed = ret > f.spiSize ? ret : f.spiSize * 2;
        why = FreezeStep::GrowSpi;
        return -1;
    }
    why = FreezeStep::Failed;
    if (st < 0 || ret > f.spiSize) return -1;
    const SpiProcess* me = nullptr;
    for (size_t off = 0; off + sizeof(SpiProcess) <= ret; ) {
        const SpiProcess* p = (const SpiProcess*)(f.spi + off);
        if ((DWORD)(ULONG_PTR)p->uniqueProcessId == f.pid) {
            if (off + sizeof(SpiProcess) + (size_t)p->numberOfThreads * sizeof(SpiThread) <= ret) me = p;
            break;
        }
        if (!p->nextEntryOffset) break;
        off += p->nextEntryOffset;
    }
    if (!me) return -1;
    const SpiThread* t = (const SpiThread*)(me + 1);
    int added = 0;
    for (ULONG k = 0; k < me->numberOfThreads; ++k) {
        const DWORD id = (DWORD)(ULONG_PTR)t[k].uniqueThread;
        if (id == f.self) continue;
        bool known = false;
        for (size_t i = 0; i < f.n && !known; ++i) known = f.tid[i] == id;
        if (known) continue;
        if (f.n == f.cap) { why = FreezeStep::GrowCap; return -1; }
        // Not recorded when it cannot be opened (it exited): it is retried while still listed.
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
        if (!h) continue;
        const size_t i = f.n++;
        f.h[i] = h; f.tid[i] = id; f.held[i] = 0;
        if (!suspend) { ++added; continue; }
        if (freezeHoldInRange(f, i, addr, len)) { busy = true; return added + 1; }
        if (f.held[i]) ++added;
    }
    return added;
}

// One freeze: hold every known thread, then re-read the list and hold newcomers until a pass finds
// none (a thread spawned by a not-yet-stopped thread, or created mid-freeze, is caught by the next
// pass; once all are held, nothing in the process can start another). Write only when the set is
// stable and no thread is in the range; always resume what was held. Nothing here allocates or logs.
FreezeStep freezeAttempt(Freeze& f, uint8_t* addr, const uint8_t* src, size_t len) {
    for (size_t i = 0; i < f.n; ++i) f.held[i] = 0;
    FreezeStep r = FreezeStep::Busy;
    bool busy = false;
    for (size_t i = 0; i < f.n && !busy; ++i) busy = freezeHoldInRange(f, i, addr, len);
    bool stable = false;
    for (int round = 0; !busy && !stable && round < kFreezeScanRounds; ++round) {
        FreezeStep why = FreezeStep::Failed;
        const int added = freezeScan(f, true, addr, len, busy, why);
        if (added < 0) { r = why; break; }
        stable = added == 0 && !busy;
    }
    if (busy) r = FreezeStep::Busy;
    else if (stable) {
        DWORD old = 0;
        if (VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(addr, src, len);
            VirtualProtect(addr, len, old, &old);
            FlushInstructionCache(GetCurrentProcess(), addr, len);
            r = FreezeStep::Written;
        } else
            r = FreezeStep::Unwritable;
    } else if (r == FreezeStep::Busy)
        r = FreezeStep::Unstable;          // still finding new threads after kFreezeScanRounds
    for (size_t i = 0; i < f.n; ++i)
        if (f.held[i]) { ResumeThread(f.h[i]); f.held[i] = 0; }
    return r;
}

// Back-off after a busy or unstable freeze. A thread inside a 5..32-byte range leaves it within
// microseconds of being resumed, so retry almost at once: spin, then yield, and only fall back to
// Sleep(1) (15.6 ms at the default timer resolution) when the range stays hot. Bounded by
// kFreezeAttempts: 8 spins + 24 yields + 32 Sleep(0) + 136 Sleep(1), about 2.2 s at most.
void freezeBackoff(int attempt) {
    if (attempt < 8) {
        for (int i = 0; i < 256; ++i) YieldProcessor();
    } else if (attempt < 32) SwitchToThread();
    else if (attempt < 64) Sleep(0);
    else                   Sleep(1);
}

// Write `len` bytes of LIVE code with every other thread of the process suspended and none of them
// executing inside [addr, addr+len) -- MinHook's freeze, which FiveM's hooks get for free. A plain
// memcpy over a hot function (GtaThread::Tick runs for every script thread every frame) can let a
// game thread execute a half-written instruction. Threads created while the freeze is being set up
// are caught by re-reading the list inside it (freezeAttempt). False when a thread is still in the
// range after kFreezeAttempts, the thread list cannot be read, or the page cannot be made writable.
// The caller keeps CALLs out of the range (a suspended return address inside it would come back
// into the new bytes): netJump refuses such a prologue; netCall's site returns past its own end.
bool frozenWrite(uint8_t* addr, const uint8_t* src, size_t len) {
    Freeze f;
    f.pid = GetCurrentProcessId();
    f.self = GetCurrentThreadId();
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        f.qsi = (NtQsiFn)(void*)GetProcAddress(nt, "NtQuerySystemInformation");
    // Grow whichever buffer a scan outgrew (outside the freeze); false on Failed or out of memory.
    auto regrow = [&f](FreezeStep why) {
        if (why == FreezeStep::GrowSpi) return freezeReserve(f, f.spiNeed, 0);
        if (why == FreezeStep::GrowCap) return freezeReserve(f, 0, f.cap + 1);
        return false;
    };
    bool written = false;
    if (f.qsi && freezeReserve(f, 0x40000, 256)) {
        // Gather the threads and open their handles before the freeze, growing the buffers as needed,
        // so a freeze normally only re-reads the list and finds nothing new.
        bool primed = false;
        for (int g = 0; g <= kFreezeRegrows && !primed; ++g) {
            bool busy = false;
            FreezeStep why = FreezeStep::Failed;
            if (freezeScan(f, false, addr, len, busy, why) >= 0) primed = true;
            else if (!regrow(why)) break;
        }
        for (int busyTries = 0, grows = 0; primed && !written && busyTries < kFreezeAttempts; ) {
            const FreezeStep r = freezeAttempt(f, addr, src, len);
            if (r == FreezeStep::Written) written = true;
            else if (r == FreezeStep::Busy || r == FreezeStep::Unstable) freezeBackoff(busyTries++);
            else if (++grows > kFreezeRegrows || !regrow(r)) break;   // Unwritable / Failed stop here
        }
    }
    for (size_t i = 0; i < f.n; ++i) CloseHandle(f.h[i]);
    if (f.h) VirtualFree(f.h, 0, MEM_RELEASE);
    if (f.spi) VirtualFree(f.spi, 0, MEM_RELEASE);
    return written;
}

// Save `len` original bytes at `addr`, write `src` (frozen, see frozenWrite), and register for
// teardown (with an optional thunk page to free). The caller owns byte
// correctness. False (freeing `thunk`) when out of image, len out of range, or the write failed.
bool netWrite(uint8_t* addr, const uint8_t* src, size_t len, void* thunk) {
    if (!netInImage(addr, len) || len == 0 || len > kNetPatchMax) {
        if (thunk) VirtualFree(thunk, 0, MEM_RELEASE);
        return false;
    }
    NetPatch p;
    p.addr = addr; p.len = len; p.thunk = thunk;
    memcpy(p.orig, addr, len);
    if (!frozenWrite(addr, src, len)) {
        if (thunk) VirtualFree(thunk, 0, MEM_RELEASE);
        return false;
    }
    g_netPatches.push_back(p);
    return true;
}

// Raw byte patch (no thunk): the simplest primitive (e.g. a data immediate).
bool netPut(uint8_t* addr, const uint8_t* bytes, size_t len) {
    return netWrite(addr, bytes, len, nullptr);
}

// Whole-function replace: overwrite `target`'s entry with a 14-byte absolute jmp
// to `dest` (our DLL code, too far for an E9), padding to the next instruction
// boundary with NOPs so no half-instruction remains. The original is DISCARDED
// (no trampoline); `dest` must do the whole job. `target` must be a clean function
// entry (see the port map's clean-prologue list) -- never a mid-function address.
// False if the prologue cannot be grown to >=14 whole bytes in range, or if a CALL
// inside it returns inside it (logged): a thread parked in that callee would come
// back into the middle of the jmp. A CALL ending exactly at the boundary is fine.
bool netJump(uint8_t* target, void* dest) {
    if (!netInImage(target, 14)) return false;
    size_t covered = 0, callEnd = 0;
    while (covered < 14) {
        size_t avail = (size_t)((g_imageBase + g_imageSize) - (target + covered));
        if (avail > 16) avail = 16;
        size_t len = 0; int ripOff = -1;
        if (!decIns(target + covered, avail, len, ripOff) || len == 0) return false;
        if (insIsCall(target + covered, len)) callEnd = covered + len;
        covered += len;
    }
    if (callEnd && callEnd < covered) {
        GLOGW("hook: [net] netJump refused at GTA5.exe+0x%llx: a CALL returns to +%zu, inside the "
              "%zu-byte overwrite", (unsigned long long)(target - g_imageBase), callEnd, covered);
        return false;
    }
    if (covered > kNetPatchMax) return false;
    uint8_t buf[kNetPatchMax];
    buf[0] = 0xFF; buf[1] = 0x25;                      // jmp qword ptr [rip+0]
    int32_t zero = 0; memcpy(buf + 2, &zero, 4);
    uint64_t abs = (uint64_t)(uintptr_t)dest; memcpy(buf + 6, &abs, 8);
    for (size_t i = 14; i < covered; ++i) buf[i] = 0x90; // NOP pad to boundary
    return netWrite(target, buf, covered, nullptr);
}

// Redirect the E8 rel32 CALL at `site` to our `dest` via a 14-byte absolute-jmp
// thunk allocated within rel32 reach of the site (dest is far). The call's
// original target is returned in `origOut` so `dest` can chain to it. The 5-byte
// call is saved and the thunk recorded for teardown.
bool netCall(uint8_t* site, void* dest, void** origOut) {
    if (!netInImage(site, 5) || site[0] != 0xE8) return false;
    uint8_t* orig = netResolveCall(site);
    if (!orig) return false;
    if (origOut) *origOut = orig;
    uint8_t* thunk = (uint8_t*)allocNear(14, site);
    if (!thunk) return false;
    thunk[0] = 0xFF; thunk[1] = 0x25;
    int32_t zero = 0; memcpy(thunk + 2, &zero, 4);
    uint64_t abs = (uint64_t)(uintptr_t)dest; memcpy(thunk + 6, &abs, 8);
    flushRWX(thunk, 14);
    intptr_t rel = (intptr_t)thunk - (intptr_t)(site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) { VirtualFree(thunk, 0, MEM_RELEASE); return false; }
    uint8_t buf[5];
    buf[0] = 0xE8;
    int32_t r32 = (int32_t)rel; memcpy(buf + 1, &r32, 4);
    return netWrite(site, buf, 5, thunk);
}

// Restore every net-host patch (byte-for-byte, newest first) and free thunks.
void netUnpatchAll() {
    for (auto it = g_netPatches.rbegin(); it != g_netPatches.rend(); ++it) {
        if (it->addr && it->len && !frozenWrite(it->addr, it->orig, it->len))
            GLOGW("hook: [net] could not restore %zu patched bytes at GTA5.exe+0x%llx", it->len,
                  (unsigned long long)(it->addr - g_imageBase));
        if (it->thunk) VirtualFree(it->thunk, 0, MEM_RELEASE);
    }
    g_netPatches.clear();
}

// Arxan wait budget of the one-shot code patches (noSpInstall, netHostInstall, tryPatchLandingPage).
// Each asks on every poll that finds its code still encrypted: WaitLog on the first wait and every
// 10 s after (its progress line), Wait in between, GiveUp once 120 s have passed since the first
// wait. On GiveUp the installer logs why, once, and reports itself done, so codePatchesPending()
// goes false and code that never decrypts stops costing an image scan every second.
enum class CodeWait { Wait, WaitLog, GiveUp };
struct CodeWaitClock { ULONGLONG first = 0, lastLog = 0; bool started = false; };
const ULONGLONG kCodeWaitMs = 120000, kCodeWaitLogMs = 10000;
CodeWaitClock g_noSpWait, g_landingWait, g_netHostWait;
// pack_enh=1 (World::packEnh): the Round A car-pack diagnostic is stepped by pollCodePatches until it finishes. It is
// read-only; g_packEnhDone starts true so a pack_enh=0 run keeps codePatchesPending() exactly as before.
bool g_packEnh = false;
bool g_packEnhDone = true;
CodeWaitClock g_packEnhWait;

CodeWait codeWaitStep(CodeWaitClock& c, ULONGLONG now) {
    if (!c.started) {
        c.started = true;
        c.first = c.lastLog = now;
        return CodeWait::WaitLog;
    }
    if (now - c.first > kCodeWaitMs) return CodeWait::GiveUp;
    if (now - c.lastLog >= kCodeWaitLogMs) {
        c.lastLog = now;
        return CodeWait::WaitLog;
    }
    return CodeWait::Wait;
}

// Resolve a net-session signature to its UNIQUE match in the decrypted image and
// log the address. null (silent) when missing; warns when ambiguous. A non-unique
// site is never returned, so it is never patched.
uint8_t* netFindUnique(const char* name, const char* sig) {
    const uint8_t* first = nullptr;
    int c = countPat(g_imageBase, g_imageSize, sig, &first);
    if (c == 1) {
        GLOGI("hook: [nethost] %-22s @ +0x%llx", name,
              (unsigned long long)(uintptr_t)(first - g_imageBase));
        return (uint8_t*)first;
    }
    if (c > 1)
        GLOGW("hook: [nethost] %-22s ambiguous (%d); not used", name, c);
    return nullptr;
}

// --- Host state machine (net_host=1): sites resolved + stubs installed on the
// poller thread; the hostGame call + poll run on the GAME thread (netHostTick,
// from scriptTickBody), mirroring FiveM's HostState on OnMainGameFrame. Minimal
// self-host recipe (FiveM gta-net-five/NetHook.cpp, confirmed host-path): force the
// "allowed to run net game" + obfuscated online gates true, force the script-
// component IsHost() true (so local scripts run as the MP host and player_controller
// stops forcing the story char), raise slots to 32, then once SC/ROS init is
// complete call hostGame(0,32,0) and poll g_isNetGame. QueryDetail, session/system
// keys and relay/UPnP are JOIN-path only and deliberately skipped.
enum class HostState { Idle, Hosting, HostingNetGame, Hosted, Fatal };
std::atomic<int>   g_hostState{(int)HostState::Idle};
std::atomic<bool>  g_netHostStubsReady{false};   // published by the poller, read on the game thread
void (*g_hostGameFn)(int, int, int) = nullptr;   // the game's host function (+0xd04d04)
volatile uint8_t*  g_isNetGameFlag = nullptr;    // the g_isNetGame bool (poll)
volatile uint32_t* g_scInitStatus  = nullptr;    // SC/ROS init status: 0xB = ready (logged only)
// The game's own isScWaitingForInit, saved by netCall before its call site is pointed
// at netReturnFalse (FiveM: set_call(&_isScWaitingForInit) + call(location, ReturnFalse)).
// netHostTick asks the ORIGINAL whether SC init is still pending, as FiveM's HS_LOADED does.
bool (*g_scWaitOrig)() = nullptr;
const uint8_t*     g_isSessionStartedFn = nullptr; // resolved + logged; not called in this bring-up
ULONGLONG g_hostCallAt = 0;

// netJump target for the forced-true gates. A leaf that returns true (al=1) and
// rets to the ORIGINAL caller -- the 14-byte jmp leaves the caller's return address
// in place -- ignoring any args, so it is ABI-safe for any bool-returning site
// FiveM replaces with ReturnTrue.
bool netReturnTrue() { return true; }

// netCall target for the game's SC-init wait (FiveM's ReturnFalse is int-typed; eax=0
// covers a caller that tests al or eax).
int netReturnFalse() { return 0; }

// Log the function prologue, then replace the whole function with a jmp to
// netReturnTrue. Logs the exact bytes overwritten (so a bad site is visible in the
// log) and whether netJump accepted the prologue.
void netHostJumpTrue(const char* name, uint8_t* fn) {
    if (!fn || !netInImage(fn, 16)) {
        GLOGW("hook: [nethost] %s: function unresolved; not stubbed", name);
        return;
    }
    char hex[16 * 3 + 1] = {};
    for (int i = 0; i < 16; ++i) snprintf(hex + i * 3, 4, "%02x ", fn[i]);
    GLOGI("hook: [nethost] %s fn @ +0x%llx prologue: %s", name,
          (unsigned long long)(uintptr_t)(fn - g_imageBase), hex);
    if (netJump(fn, (void*)&netReturnTrue))
        GLOGI("hook: [nethost]   %s -> ReturnTrue", name);
    else
        GLOGW("hook: [nethost]   %s: netJump refused (prologue undecodable or holds a CALL, or the write failed); "
              "NOT stubbed", name);
}

// Install the host stubs + resolve the drive pointers (poller thread, once the
// network code has decrypted). Returns true once the core site resolves (stop
// retrying); false while the network code is still encrypted (keep polling). All
// stubs are reversible (netUnpatchAll on Unhook); the drive is armed only when
// every required pointer resolved.
//
// SAFETY (S0): nothing here runs unless the ROS guard (net_guard.h, Guard mode) is active, so no
// Rockstar/matchmaking traffic can leave the machine from the game's own network imports. And
// until G1 exists (the identity/SC layer: a local ROS emulator answering the game's sign-in), this
// is a DRY RUN: every site is resolved and logged, nothing is patched, hostGame is never called.
// gage.log showed SC status 0x0 = the SC-init function never ran, so hostGame cannot succeed, and
// forcing the online gates open or skipping the SC wait would only let the game's own online code
// run. Flip kNetHostG1Ready only together with G1.
constexpr bool kNetHostG1Ready = false;

bool netHostInstall() {
    if (g_build != &kProfile3889) {
        GLOGW("hook: [nethost] net_host ignored: build is %s, host port is Legacy 1.0.3889 only",
              g_build->label);
        return true;
    }
    if (!gage::netguard::GuardActive()) {
        GLOGW("nethost: refusing: ROS guard not active (no session stubs installed; hostGame will not be called)");
        return true;
    }
    uint8_t* hostGameSite = netFindUnique("hostGame",
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 41 8A D8 8B FA 8B F1 E8 ?? ?? ?? ?? 45 33 C0");
    if (!hostGameSite) {
        const ULONGLONG now = GetTickCount64();
        switch (codeWaitStep(g_netHostWait, now)) {
        case CodeWait::GiveUp:
            GLOGW("hook: [nethost] hostGame pattern not found after 120 s (network code never decrypted); "
                  "host stubs NOT installed");
            return true;
        case CodeWait::WaitLog:
            GLOGW("hook: [nethost] waiting for network code to decrypt (%llu s)",
                  (unsigned long long)((now - g_netHostWait.first) / 1000));
            break;
        case CodeWait::Wait:
            break;
        }
        return false;
    }
    if (kNetHostG1Ready)
        GLOGI("hook: [nethost] network code decrypted; installing minimal self-host stubs");
    else
        GLOGI("hook: [nethost] network code decrypted; DRY RUN until G1: resolving host sites, patching nothing");
    // A stub that would be installed: installed with G1, otherwise only its target logged.
    auto stubTrue = [](const char* name, uint8_t* fn) {
        if (kNetHostG1Ready) { netHostJumpTrue(name, fn); return; }
        if (fn) GLOGI("hook: [nethost]   %s fn @ +0x%llx (dry run: not stubbed)", name,
                      (unsigned long long)(uintptr_t)(fn - g_imageBase));
        else    GLOGW("hook: [nethost]   %s: function unresolved", name);
    };

    // 1) "allowed to run net game" gate -> true. The sig starts at an E8 call; the
    //    real gate function is that call's target.
    uint8_t* arg = netFindUnique("netAllowRunGame",
        "E8 ?? ?? ?? ?? 84 C0 74 ?? 33 D2 8D 4A ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 85 F6");
    if (arg) stubTrue("netAllowRunGame", netResolveCall(arg));

    // 2) obfuscated online gate -> true. The real function is the call at site+12.
    uint8_t* unk = netFindUnique("netUnkCheck1",
        "84 C0 74 0F BA 03 00 00 00 48 8B CB E8 ?? ?? ?? ?? EB 0E");
    if (unk) stubTrue("netUnkCheck1", netResolveCall(unk + 12));

    // 3) script-component IsHost() -> true (the pattern is the function start). This
    //    lets local scripts run as the MP host so player_controller yields the ped.
    //    FiveM jumps at match-10 (NetHook.cpp: get_pattern(..., -10)): the match is past a 10-byte
    //    prologue, so a jmp AT the match would ret on the prologue's stack. Only patch -10 when it
    //    decodes as whole instructions ending exactly at the match and follows inter-function padding.
    uint8_t* host = netFindUnique("hostSpoof",
        "33 DB 48 85 C0 74 17 48 8B 48 10 48 85 C9 74 0E");
    if (host) {
        uint8_t* fn = host - 10;
        size_t covered = 0;
        bool whole = netInImage(fn - 1, 12);
        while (whole && covered < 10) {
            size_t L = 0; int rip = -1;
            if (!decIns(fn + covered, 16, L, rip) || !L) { whole = false; break; }
            covered += L;
        }
        const bool padded = whole && (fn[-1] == 0xCC || fn[-1] == 0xC3 || fn[-1] == 0x90);
        if (whole && covered == 10 && padded)
            stubTrue("hostSpoof", fn);
        else
            GLOGW("hook: [nethost] hostSpoof: match-10 is not a clean function start (decoded %zu of 10, "
                  "pad byte 0x%02X); NOT stubbed", covered, netInImage(fn - 1, 1) ? fn[-1] : 0);
    }

    // 4) max player slots 24->32 (reversible data patch; imm's low byte at +6).
    uint8_t* slots = netFindUnique("netMaxSlots", "C7 87 ?? ?? ?? 00 18 00 00 00 C7 87");
    if (slots && !kNetHostG1Ready) {
        GLOGI("hook: [nethost]   max slots imm @ +0x%llx = 0x%02X (dry run: not patched)",
              (unsigned long long)(uintptr_t)(slots + 6 - g_imageBase), slots[6]);
    } else if (slots) {
        if (slots[6] == 0x18) {
            const uint8_t v = 0x20;
            if (netPut(slots + 6, &v, 1))
                GLOGI("hook: [nethost]   max slots 24->32 at +0x%llx",
                      (unsigned long long)(uintptr_t)(slots + 6 - g_imageBase));
            else
                GLOGW("hook: [nethost]   max-slots patch refused");
        } else {
            GLOGW("hook: [nethost]   max-slots imm@+6 = 0x%02X not 0x18; not patched", slots[6]);
        }
    }

    // Resolve the drive pointers read on the game thread. The sig matches hostGame's CALLER;
    // hostGame itself is the call at +0x31 (FiveM: get_call(get_pattern(..., 0x31))). Calling the
    // match directly ran the caller, so g_isNetGame never flipped (gage.log 09:22:57).
    if (hostGameSite[0x31] == 0xE8) {
        uint8_t* hg = netResolveCall(hostGameSite + 0x31);
        if (hg && netInImage(hg, 16)) {
            g_hostGameFn = (void (*)(int, int, int))hg;
            GLOGI("hook: [nethost] hostGame fn            @ +0x%llx (call at match+0x31)",
                  (unsigned long long)(uintptr_t)(hg - g_imageBase));
        }
    } else {
        GLOGW("hook: [nethost] hostGame: match+0x31 is 0x%02X, not a call; host drive disabled",
              hostGameSite[0x31]);
    }
    uint8_t* gng = netFindUnique("g_isNetGame",
        "48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B D9 74 5F");
    if (gng) g_isNetGameFlag = (volatile uint8_t*)netResolveRip(gng + 4);   // cmp byte [rip+flag],0
    uint8_t* sc = netFindUnique("scWaitingForInit", "E8 ?? ?? ?? ?? 84 C0 75 B6 88 05");
    if (sc) {
        g_scInitStatus = (volatile uint32_t*)netResolveRip(sc + 15);        // mov dword [rip+status],0xB
        // The game spins on this call before its online init (75 B6 loops back); the
        // status dword above is only written after the loop, so it never reaches 0xB in
        // story mode. Like FiveM, point the call at ReturnFalse and keep the original.
        // G1 only: skipping the SC wait lets the game run its online init (entitlements, ROS).
        void* orig = nullptr;
        if (!kNetHostG1Ready) {
            GLOGI("hook: [nethost]   scWaitingForInit call @ +0x%llx (dry run: not redirected)",
                  (unsigned long long)(uintptr_t)(sc - g_imageBase));
        } else if (netCall(sc, (void*)&netReturnFalse, &orig) && orig) {
            g_scWaitOrig = (bool (*)())orig;
            GLOGI("hook: [nethost]   scWaitingForInit call -> ReturnFalse (orig @ +0x%llx)",
                  (unsigned long long)(uintptr_t)((uint8_t*)orig - g_imageBase));
        } else {
            GLOGW("hook: [nethost]   scWaitingForInit: netCall refused; SC wait not patched");
        }
    }
    uint8_t* iss = netFindUnique("isSessionStarted", "8B 86 ?? ?? 00 00 C1 E8 05 84 C2");
    if (iss) g_isSessionStartedFn = netResolveCall(iss + 17);               // logged; not called yet

    GLOGI("hook: [nethost] drive ptrs: hostGame=%p g_isNetGame=%p scWaitOrig=%p scStatus=%p isSessionStarted=%p",
          (void*)g_hostGameFn, (void*)g_isNetGameFlag, (void*)g_scWaitOrig, (void*)g_scInitStatus,
          (void*)g_isSessionStartedFn);

    if (!g_hostGameFn || !g_isNetGameFlag || !g_scInitStatus || (kNetHostG1Ready && !g_scWaitOrig)) {
        GLOGW("hook: [nethost] a required drive pointer did not resolve; host drive DISABLED (no hostGame call)");
        return true;
    }
    g_netHostStubsReady.store(true);   // publish: the game thread may now drive
    GLOGI("hook: [nethost] drive armed (%s); watching the SC init status on the game thread",
          kNetHostG1Ready ? "stubs installed" : "dry run: hostGame is never called before G1");
    return true;
}

const char* scStatusName(uint32_t v) {
    switch (v) {
    case 0x0: return "SC init never ran";
    case 0x4: return "waiting for SC init";
    case 0xB: return "SC ready";
    default:  return "unknown";
    }
}

// Drive the host machine on the GAME thread (called from scriptTickBody when
// net_host=1). Mirrors FiveM's process(): once SC/ROS init is complete and the game
// is not already in a net game, call hostGame(0,32,0) ONCE, then poll g_isNetGame.
// Calls nothing with an unverified ABI (g_isNetGame is a plain global read);
// isSessionStarted is resolved+logged for the next bring-up, not called.
void netHostTick() {
    if (!g_netHostStubsReady.load()) return;
    const ULONGLONG now = GetTickCount64();
    switch ((HostState)g_hostState.load()) {
    case HostState::Idle: {
        // S0: the guard is re-checked live (a slot the game rewrote turns it off).
        if (!gage::netguard::GuardActive()) {
            GLOGW("nethost: refusing: ROS guard not active (hostGame not called)");
            g_hostState.store((int)HostState::Fatal);
            return;
        }
        // The SC gate reads the status dword only (0x0 = the SC-init function never ran, 0x4 =
        // waiting, 0xB = ready); it no longer waits for 0xB forever. Not 0xB within
        // kScGateMs of the first tick -> give up for this session with the reason.
        constexpr ULONGLONG kScGateMs = 60000;
        static ULONGLONG gateSince = 0;
        static uint32_t lastScv = 0xFFFFFFFFu;
        if (!gateSince) gateSince = now;
        const uint32_t scv = *g_scInitStatus;
        const uint8_t  ng  = g_isNetGameFlag ? *g_isNetGameFlag : 0;
        if (scv != lastScv) {
            lastScv = scv;
            GLOGI("hook: [nethost] SC init status 0x%X (%s) at %llu s", scv, scStatusName(scv),
                  (unsigned long long)((now - gateSince) / 1000));
        }
        if (scv != 0xB) {
            if (now - gateSince < kScGateMs) return;
            GLOGW("nethost: giving up: SC init status 0x%X (%s) after %llu s; hostGame needs the G1 identity "
                  "layer (local ROS) -- not called", scv, scStatusName(scv),
                  (unsigned long long)((now - gateSince) / 1000));
            g_hostState.store((int)HostState::Fatal);
            return;
        }
        if (!kNetHostG1Ready) {
            GLOGW("nethost: giving up: SC reports ready (0xB) but the G1 identity layer is not built; hostGame "
                  "is not called before G1 (by design)");
            g_hostState.store((int)HostState::Fatal);
            return;
        }
        if (g_scWaitOrig && g_scWaitOrig()) return;   // FiveM HS_LOADED: if (_isScWaitingForInit()) return;
        if (ng) {
            GLOGW("hook: [nethost] already in a net game before hosting (g_isNetGame=1); not calling hostGame");
            g_hostState.store((int)HostState::Hosted);
            return;
        }
        GLOGI("hook: [nethost] SC ready (isScWaitingForInit=0, status=0x%X), g_isNetGame=0 -> calling "
              "hostGame(0,32,0) on the game thread", scv);
        g_hostCallAt = now;
        g_hostGameFn(0, 32, 0);
        GLOGI("hook: [nethost] hostGame returned; polling g_isNetGame");
        g_hostState.store((int)HostState::Hosting);
        break;
    }
    case HostState::Hosting:
        if (g_isNetGameFlag && *g_isNetGameFlag) {
            GLOGI("hook: [nethost] g_isNetGame=1 (session coming up)");
            g_hostState.store((int)HostState::HostingNetGame);
        } else if (now - g_hostCallAt > 30000) {
            GLOGW("hook: [nethost] g_isNetGame never set 30 s after hostGame; FATAL (host did not start)");
            g_hostState.store((int)HostState::Fatal);
        }
        break;
    case HostState::HostingNetGame: {
        static ULONGLONG stableSince = 0;
        if (g_isNetGameFlag && *g_isNetGameFlag) {
            if (!stableSince) stableSince = now;
            if (now - stableSince >= 3000) {
                GLOGI("hook: [nethost] HOSTED: g_isNetGame stable 3 s. Watch the ped guard -- if the thrash "
                      "stops, player_controller yielded; if not, the network-mode flag sites are next.");
                g_hostState.store((int)HostState::Hosted);
            }
        } else {
            GLOGW("hook: [nethost] g_isNetGame dropped before stable; FATAL");
            g_hostState.store((int)HostState::Fatal);
        }
        break;
    }
    case HostState::Hosted:
    case HostState::Fatal:
    default:
        break;
    }
}

// --- No stock scripts (no_sp_scripts=1): FiveM rage-scripting-five scrEngine.cpp JustNoScript.
// FiveM does not stop the singleplayer script tree from being CREATED; it stops every stock thread
// from EXECUTING: GtaThread::Tick (pattern below, -0xB; no build branch, FiveM master targets 3889)
// returns the thread's state without running an opcode. startup never runs, so main / main_persistent
// / player_controller never launch; threads that already started freeze where they are. Our engine
// does not need them: it drives the reveal itself and ticks from the run_script_threads detour,
// impersonating main_persistent or, when that never launched, startup (scriptTickBody).
size_t g_threadStateOff = 0x10;   // scrThreadContext::State = ThreadId + 8 (set from the build profile)

// netJump target for GtaThread::Tick(this, a2): eThreadState from the thread's own context. Never
// calls the original, so no trampoline is needed (a whole-function replace, like ReturnTrue).
int noScriptTick(uint8_t* thread, int) { return *(int*)(thread + g_threadStateOff); }

// Poller thread. True once installed or definitively given up; false to keep retrying while the
// script code is still encrypted.
uint8_t* findAnyScriptThread();   // defined with the other thread finders below
bool noSpBootReached();           // likewise (needs findScriptThread / joaat)

// The Tick function we hooked (null until noSpInstall succeeds) and whether noSpVerifyTick has finished.
uint8_t* g_noSpTickFn = nullptr;
bool g_noSpVerified = false;

// How long noSpInstall waits for the stock boot (noSpAfterBoot builds) before leaving the stock scripts running.
// Enhanced reached main_persistent 63 s after process start on the friend's 0.4.10 run; this is counted from the
// poller's first look, with room for a slow disk and the loading screen.
const ULONGLONG kNoSpBootWaitMs = 300000, kNoSpBootLogMs = 10000;

bool noSpInstall() {
    // Legacy 3889: the pattern sits inside the function (fn = match - 0xB). Enhanced: an entry-anchored
    // signature from the build profile. Any other build has neither, so it stays inert.
    const bool profileSig = g_build->gtaThreadTickSig != nullptr;
    if (g_build != &kProfile3889 && !profileSig) {
        GLOGW("hook: [nosp] no_sp_scripts ignored: no GtaThread::Tick pattern for build %s", g_build->label);
        return true;
    }
    const char* sig = profileSig ? g_build->gtaThreadTickSig : "48 83 EC 20 80 B9 ?? 01 00 00 00 8B FA";
    const int fnOffset = profileSig ? g_build->gtaThreadTickFnOffset : -0xB;
    // Enhanced (noSpAfterBoot): let the stock boot run first. The script-thread array only exists once the poller
    // installed the script tick (natives validated), so until then this simply keeps waiting; the trailing poller
    // loop (codePatchesPending) carries on after the native table validates.
    if (g_build->noSpAfterBoot && !noSpBootReached()) {
        static ULONGLONG firstWait = 0, lastLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (!firstWait) firstWait = lastLog = now;
        if (now - firstWait > kNoSpBootWaitMs) {
            GLOGW("hook: [nosp] the stock boot (main_persistent) did not appear in %llu s; stock scripts keep running",
                  (unsigned long long)(kNoSpBootWaitMs / 1000));
            return true;
        }
        if (now - lastLog >= kNoSpBootLogMs) {
            lastLog = now;
            GLOGI("hook: [nosp] waiting for the stock boot (main_persistent live) before hooking GtaThread::Tick "
                  "(%llu s)", (unsigned long long)((now - firstWait) / 1000));
        }
        return false;
    }
    // Every hit, then nosp_pick.h decides: one hit = the old rule; several = only the one at the RVA proven live for
    // this build (0.4.20: 3 hits for whole Legacy sessions, so the unique-only rule never hooked and the stock scripts
    // took the spawn over); anything else is refused and waited on as before.
    const uint8_t* hits[gage::nosp::kMaxTickCandidates] = {};
    size_t kept = 0;
    const int total = findAllPat(g_imageBase, g_imageSize, sig, hits, gage::nosp::kMaxTickCandidates, &kept);
    uint64_t rvas[gage::nosp::kMaxTickCandidates] = {};
    for (size_t i = 0; i < kept; ++i) rvas[i] = (uint64_t)((hits[i] + fnOffset) - g_imageBase);
    const gage::nosp::TickChoice pick = gage::nosp::ChooseTick(rvas, kept, total > 0 ? (size_t)total : 0,
                                                               g_build->gtaThreadTickRva, g_noSpKnownRva);
    if (total > 1) {
        // Once per launch: what every hit looks like, so a log says what the other matches are.
        static bool listed = false;
        if (!listed) {
            listed = true;
            GLOGW("hook: [nosp] GtaThread::Tick signature: %d hits (proven RVA +0x%llx, nosp_known_rva=%d) -> %s",
                  total, (unsigned long long)g_build->gtaThreadTickRva, g_noSpKnownRva ? 1 : 0,
                  gage::nosp::TickPickName(pick.how));
            for (size_t i = 0; i < kept; ++i) {
                const uint8_t* f = hits[i] + fnOffset;
                char hx[24 * 3 + 1] = {};
                for (int b = 0; b < 24; ++b) snprintf(hx + b * 3, 4, "%02x ", f[b]);
                GLOGW("hook: [nosp]   hit %zu: fn +0x%llx%s: %s", i + 1, (unsigned long long)rvas[i],
                      (int)i == pick.index ? " (hooked)" : "", hx);
            }
        }
    }
    if (pick.how != gage::nosp::TickPick::Unique && pick.how != gage::nosp::TickPick::KnownRva) {
        const ULONGLONG now = GetTickCount64();
        switch (codeWaitStep(g_noSpWait, now)) {
        case CodeWait::GiveUp:
            if (total > 1)
                GLOGW("hook: [nosp] GtaThread::Tick signature still has %d hits and none at the proven RVA after 120 s; "
                      "stock scripts keep running (the hits are listed above)", total);
            else
                GLOGW("hook: [nosp] GtaThread::Tick pattern not found after 120 s; stock scripts keep running");
            return true;
        case CodeWait::WaitLog:
            if (total > 1)
                GLOGW("hook: [nosp] GtaThread::Tick signature ambiguous (%d hits, none at the proven RVA) (%llu s)",
                      total, (unsigned long long)((now - g_noSpWait.first) / 1000));
            else
                GLOGW("hook: [nosp] waiting for GtaThread::Tick to decrypt (%llu s)",
                      (unsigned long long)((now - g_noSpWait.first) / 1000));
            break;
        case CodeWait::Wait:
            break;
        }
        return false;
    }
    uint8_t* fn = (uint8_t*)hits[pick.index] + fnOffset;
    GLOGI("hook: [nosp] GtaThread::Tick chosen by %s (%d hit(s)): fn +0x%llx",
          gage::nosp::TickPickName(pick.how), total, (unsigned long long)rvas[pick.index]);
    g_threadStateOff = g_build->threadStateOff ? g_build->threadStateOff : g_build->threadId + 8;
    char hex[16 * 3 + 1] = {};
    for (int i = 0; i < 16; ++i) snprintf(hex + i * 3, 4, "%02x ", fn[i]);
    GLOGI("hook: [nosp] GtaThread::Tick fn @ +0x%llx prologue: %s(state @ thread+0x%zx)",
          (unsigned long long)(uintptr_t)(fn - g_imageBase), hex, g_threadStateOff);
    if (netJump(fn, (void*)&noScriptTick)) {
        g_noSpActive = true;
        g_noSpTickFn = fn;
        GLOGI("hook: [nosp] stock scripts OFF: GtaThread::Tick -> no-op (FiveM JustNoScript)");
    } else
        GLOGW("hook: [nosp] netJump refused the Tick prologue (undecodable or holds a CALL, or the write "
              "failed); stock scripts keep running");
    return true;
}

// Enhanced belt and braces. run_script_threads calls vtable slot 3 of every thread, so the function that really
// ticks a stock script is the live vtable's, not whatever the signature matched. Once the ScriptThreads array is
// known, read slot 3 of a live thread and say whether it is the function noSpInstall hooked; if it is not (a patch
// that kept the signature's shape on another function), hook the vtable's function too, so the log line "stock
// scripts OFF" is never a claim about the wrong function. Runs from the poller until it has looked once or 120 s
// have passed; nothing here touches the game thread.
void noSpVerifyTick() {
    // Legacy (no profile signature) runs it too, LOG ONLY: run_script_threads' slot index is derived on Enhanced, so on
    // Legacy a mismatch is evidence for the next round, never a reason to patch another function.
    if (!g_noSpActive || g_noSpVerified) return;
    const bool mayHookSlot = g_build->gtaThreadTickSig != nullptr;
    static ULONGLONG firstTry = 0;
    const ULONGLONG now = GetTickCount64();
    if (!firstTry) firstTry = now;
    uint8_t* t = findAnyScriptThread();
    if (!t) {
        if (now - firstTry > 120000) {
            g_noSpVerified = true;
            GLOGW("hook: [nosp] vtable cross-check: no live script thread within 120 s; Tick hook unverified");
        }
        return;
    }
    g_noSpVerified = true;
    uint8_t** vt = nullptr;
    if (!isReadable(t, sizeof(void*))) return;
    memcpy(&vt, t, sizeof(vt));
    if (!vt || !isReadable(vt, 4 * sizeof(void*))) {
        GLOGW("hook: [nosp] vtable cross-check: the thread's vtable %p is unreadable; Tick hook unverified", (void*)vt);
        return;
    }
    uint8_t* tick = vt[3];
    const bool inImage = tick >= g_imageBase && tick < g_imageBase + g_imageSize;
    GLOGI("hook: [nosp] vtable cross-check: a live thread's vtable is GTA5.exe+0x%llx, slot 3 (what run_script_threads "
          "calls) = %s+0x%llx, hooked Tick = GTA5.exe+0x%llx: %s",
          (unsigned long long)((uint8_t*)vt - g_imageBase), inImage ? "GTA5.exe" : "(outside the image) ",
          (unsigned long long)(inImage ? (uint64_t)(tick - g_imageBase) : (uint64_t)(uintptr_t)tick),
          (unsigned long long)(g_noSpTickFn - g_imageBase), tick == g_noSpTickFn ? "MATCH" : "MISMATCH");
    if (tick == g_noSpTickFn) return;
    if (!inImage) return;
    if (!mayHookSlot) {
        GLOGW("hook: [nosp] vtable cross-check (Legacy, log only): slot 3 is not the hooked Tick; nothing else patched");
        return;
    }
    if (netJump(tick, (void*)&noScriptTick))
        GLOGW("hook: [nosp] the signature hooked the wrong function; the vtable's slot 3 is hooked as well");
    else
        GLOGW("hook: [nosp] the signature hooked the wrong function and netJump refused the vtable's slot 3; "
              "stock scripts keep running");
}

// Undo an installed inline hook: restore the prologue, free the thunks.
void removeInlineHook(HookState& st) {
    if (!st.active) return;
    uint8_t* t = st.target;
    DWORD old = 0;
    VirtualProtect(t, (size_t)st.savedJumpLen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(t, st.savedBytes, (size_t)st.savedJumpLen);
    VirtualProtect(t, (size_t)st.savedJumpLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (size_t)st.savedJumpLen);
    if (st.trampoline) VirtualFree(st.trampoline, 0, MEM_RELEASE);
    if (st.detour)     VirtualFree(st.detour,     0, MEM_RELEASE);
    st = HookState{};
}

// Inline-hook `target`: its first whole instructions (>= 5 bytes) move to a
// trampoline, an E9 to the detour thunk takes their place. The thunk calls the
// function pointer stored in *cbSlot (if non-null), optionally after saving RCX
// to *rcxSave, then runs the original. `what` names the target in the log.
bool installInlineHook(uint8_t* target, HookState& st, void** rcxSave, void* const* cbSlot, const char* what) {
    // Need >= 5 bytes of whole instructions to place an E9 jmp.
    const int minBytes = 5;
    size_t cap = 40;
    // Detour and trampoline within +-2 GB of the target for E9 (Present lives in
    // dxgi.dll, not in the game image).
    uint8_t* d = (uint8_t*)allocNear(128, target);
    uint8_t* tr = (uint8_t*)allocNear(cap + 14, target); // +14 for absolute jmp fallback
    if (!d || !tr) {
        if (d)  VirtualFree(d, 0, MEM_RELEASE);
        if (tr) VirtualFree(tr, 0, MEM_RELEASE);
        GLOGW("hook: failed to allocate detour/trampoline near %s", what);
        return false;
    }

    // What we are about to overwrite: an E9 here means another overlay hooked first.
    {
        char hex[16 * 3 + 1] = {};
        for (int i = 0; i < 16; ++i)
            snprintf(hex + i * 3, 4, "%02x ", target[i]);
        GLOGI("hook: %s prologue at %p: %s", what, target, hex);
    }
    size_t copied = copyPrologue(tr, target, minBytes, cap);
    if (copied < (size_t)minBytes) {
        GLOGW("hook: %s prologue not relocatable; not hooked", what);
        VirtualFree(d, 0, MEM_RELEASE);
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }

    // The whole instructions the E9 overwrites (copyPrologue may have grown the
    // relocated copy, so measure the original).
    size_t origCopied = 0;
    {
        size_t insLen = 0;
        while (origCopied < (size_t)minBytes) {
            int rip = -1;
            if (!decIns(target + origCopied, cap - origCopied, insLen, rip))
                break;
            origCopied += insLen;
        }
    }

    // Trampoline tail: jmp back to the first instruction after the moved ones.
    // Try E9 first, fall back to 14-byte absolute jmp.
    intptr_t backDelta = (intptr_t)(target + origCopied) - (intptr_t)(tr + copied + 5);
    if (backDelta >= INT32_MIN && backDelta <= INT32_MAX) {
        tr[copied] = 0xE9;
        int32_t back = (int32_t)backDelta;
        memcpy(tr + copied + 1, &back, 4);
        copied += 5;
    } else {
        // FF 25 00 00 00 00 [8-byte absolute address]
        tr[copied]     = 0xFF;
        tr[copied + 1] = 0x25;
        int32_t zero = 0;
        memcpy(tr + copied + 2, &zero, 4);
        uint64_t abs = (uint64_t)(uintptr_t)(target + origCopied);
        memcpy(tr + copied + 6, &abs, 8);
        copied += 14;
    }
    flushRWX(tr, copied);

    makeDetour(d, tr, rcxSave, cbSlot);

    DWORD old = 0;
    VirtualProtect(target, origCopied, PAGE_EXECUTE_READWRITE, &old);
    memcpy(st.savedBytes, target, origCopied);

    // Write the E9 jmp (d was allocated near target, so this should fit).
    intptr_t detourDelta = (intptr_t)d - (intptr_t)(target + 5);
    if (detourDelta >= INT32_MIN && detourDelta <= INT32_MAX) {
        target[0] = 0xE9;
        int32_t rel = (int32_t)detourDelta;
        memcpy(target + 1, &rel, 4);
        for (size_t i = 5; i < origCopied; i++) target[i] = 0x90;
    } else {
        GLOGW("hook: detour too far from %s (%lld bytes); hook not installed", what, (long long)detourDelta);
        VirtualProtect(target, origCopied, old, &old);
        VirtualFree(d, 0, MEM_RELEASE);
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }

    VirtualProtect(target, origCopied, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, origCopied);

    st.target = target;
    st.trampoline = tr;
    st.detour = d;
    st.patchedLen = (int)origCopied;
    st.savedJumpLen = (int)origCopied;
    st.active = true;
    GLOGI("hook: %s detoured at %p (patched %zu bytes)", what, target, origCopied);
    return true;
}

// ---------------------------------------------------------------------------
// Script tick: run our game work inside GTA's script update.
// ---------------------------------------------------------------------------
// run_script_threads(uint32 ops) and the script-thread array it walks, by the
// 3889 "ST" signature (Mr-X-GTA/YimMenu pointers.cpp: function = match - 0x1F,
// threads = rip(match - 4) - 8). The thread being run lives in GTA5.exe's TLS
// block (rage::tlsContext, GTAV-Classes-1 @a8c08f1 for 3889): m_script_thread
// at +0x2A48, m_is_script_thread_active at +0x2A50. Natives that create or move
// entities read them, so we set them to main_persistent's thread around our call
// (YimMenu gta_util::execute_as_script does the same).
// run_script_threads signature + TLS/GtaThread field offsets now live in the
// per-build profile (g_build); the Legacy 3889 values are in kProfile3889 and
// each consumer below reads them into locals of the same names.

struct ScriptThreadArray { uint8_t** data; uint16_t size; uint16_t count; };  // rage::atArray<GtaThread*>

HookState          g_scriptHook;
ScriptThreadArray* g_scriptThreads = nullptr;
void (*g_onScriptTick)() = nullptr;    // the engine's callback (HookScriptTick)
void (*g_scriptTickThunk)() = nullptr; // what the detour calls: scriptTickBody once installed
std::atomic<ULONGLONG> g_scriptTickRanAt{0};  // GetTickCount64 of the last callback run

uint32_t joaat(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        h += (uint8_t)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

// A live GtaThread running the script `hash`, or null.
uint8_t* findScriptThread(uint32_t hash) {
    const size_t kThreadId = g_build->threadId, kThreadHandler = g_build->threadHandler,
                 kThreadScriptHash = g_build->threadScriptHash, kThreadName = g_build->threadScriptName;
    if (!g_scriptThreads || !isReadable(g_scriptThreads, sizeof(ScriptThreadArray))) return nullptr;
    const ScriptThreadArray a = *g_scriptThreads;
    if (!a.data || a.size > 4096 || !isReadable(a.data, (size_t)a.size * sizeof(uint8_t*))) return nullptr;

    // Enhanced: the GtaThread name-hash offset (threadScriptHash) is a maintained-
    // header value we cannot verify from an image-only dump (threads are heap
    // objects). Dump the first few threads once — a wrong offset is then one glance
    // at gage.log, not a silently stuck load. Legacy leaves threadScriptName 0 and
    // skips this block entirely (behaviour byte-identical).
    static bool diagged = false;
    if (kThreadName && !diagged && a.size > 0) {   // the poller's boot wait can look before the first thread exists
        diagged = true;
        for (uint16_t i = 0; i < a.size && i < 16; ++i) {
            uint8_t* t = a.data[i];
            if (!t || !isReadable(t, kThreadName + 64)) continue;
            uint32_t id = 0, h = 0;
            memcpy(&id, t + kThreadId, 4);
            memcpy(&h, t + kThreadScriptHash, 4);
            char nm[65] = {};
            memcpy(nm, t + kThreadName, 64);
            for (char* c = nm; *c; ++c) if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7e) { *c = 0; break; }
            GLOGI("hook: [threads] #%u id=%u hash@0x%zx=0x%08X name@0x%zx='%s' joaat(name)=0x%08X",
                  (unsigned)i, id, kThreadScriptHash, h, kThreadName, nm, joaat(nm));
        }
    }

    // Primary: match by the name hash at threadScriptHash (the long-standing path).
    for (uint16_t i = 0; i < a.size; ++i) {
        uint8_t* t = a.data[i];
        if (!t || !isReadable(t, kThreadScriptHash + 4)) continue;
        uint32_t id = 0, h = 0;
        void* handler = nullptr;
        memcpy(&id, t + kThreadId, 4);
        memcpy(&handler, t + kThreadHandler, 8);
        memcpy(&h, t + kThreadScriptHash, 4);
        if (id && handler && h == hash) return t;
    }

    // Fallback (Enhanced only): if the hash offset above missed every thread, match
    // by the name STRING recomputed through joaat — immune to a wrong hash offset,
    // so a single friend run still gets the tick live and names the bad offset.
    if (kThreadName) {
        for (uint16_t i = 0; i < a.size; ++i) {
            uint8_t* t = a.data[i];
            if (!t || !isReadable(t, kThreadName + 64)) continue;
            uint32_t id = 0;
            void* handler = nullptr;
            memcpy(&id, t + kThreadId, 4);
            memcpy(&handler, t + kThreadHandler, 8);
            if (!id || !handler) continue;
            char nm[65] = {};
            memcpy(nm, t + kThreadName, 64);
            bool printable = (uint8_t)nm[0] >= 0x20 && (uint8_t)nm[0] <= 0x7e;
            for (char* c = nm; *c; ++c) if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7e) { *c = 0; break; }
            if (printable && nm[0] && joaat(nm) == hash) {
                static bool loggedFallback = false;
                if (!loggedFallback) {
                    GLOGW("hook: [threads] matched '%s' by NAME (0x%zx); name-hash offset 0x%zx is wrong "
                          "for this build — update threadScriptHash", nm, kThreadName, kThreadScriptHash);
                    loggedFallback = true;
                }
                return t;
            }
        }
    }
    return nullptr;
}

// Last-resort script host with stock scripts off: the first live thread (id and handler set). While
// GtaThread::Tick is a no-op nothing terminates a thread, so any of them is a stable identity for our
// natives. Covers a Tick hook that landed after startup ended but before main_persistent launched.
uint8_t* findAnyScriptThread() {
    if (!g_scriptThreads || !isReadable(g_scriptThreads, sizeof(ScriptThreadArray))) return nullptr;
    const ScriptThreadArray a = *g_scriptThreads;
    if (!a.data || a.size > 4096 || !isReadable(a.data, (size_t)a.size * sizeof(uint8_t*))) return nullptr;
    const size_t kThreadId = g_build->threadId, kThreadHandler = g_build->threadHandler;
    for (uint16_t i = 0; i < a.size; ++i) {
        uint8_t* t = a.data[i];
        if (!t || !isReadable(t, kThreadHandler + 8)) continue;
        uint32_t id = 0;
        void* handler = nullptr;
        memcpy(&id, t + kThreadId, 4);
        memcpy(&handler, t + kThreadHandler, 8);
        if (id && handler) return t;
    }
    return nullptr;
}

// noSpAfterBoot builds: the stock boot has run far enough to leave the game with a player once main_persistent is a
// live thread (on the friend's 0.4.10 run: the ped appeared at 57 s, main_persistent 6 s later). Needs the script
// thread array, which the poller sets when it installs the script tick, so false until then.
bool noSpBootReached() {
    static const uint32_t kMainPersistent = joaat("main_persistent");
    uint8_t* t = findScriptThread(kMainPersistent);
    if (!t) return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        GLOGI("hook: [nosp] stock boot reached: main_persistent is live (thread %p, id %u); hooking GtaThread::Tick now",
              (void*)t, *(uint32_t*)(t + g_build->threadId));
    }
    return true;
}

// Called by the detour at the top of run_script_threads, on the game thread.
void scriptTickBody() {
    const size_t kThreadId = g_build->threadId, kTlsScriptThread = g_build->tlsScriptThread,
                 kTlsScriptThreadActive = g_build->tlsScriptThreadActive;
    static const uint32_t kMainPersistent = joaat("main_persistent");
    static bool loggedFirst = false;
    // Drive the FiveM-style host machine on the game thread (net_host=1). Runs before
    // the main_persistent gate so hosting proceeds independent of our script host.
    if (g_netHost) netHostTick();
    if (!g_onScriptTick) return;
    // The host thread found last tick is kept while it still carries the same id and script hash
    // (a terminated thread's id goes to 0): one readability check per tick instead of the full
    // scan, which costs a VirtualQuery per thread slot -- and with stock scripts off it would scan
    // for a main_persistent that never exists on every tick.
    static uint8_t* cachedHost = nullptr;
    static uint32_t cachedId = 0, cachedHash = 0;
    const size_t kThreadScriptHash = g_build->threadScriptHash;
    uint8_t* thread = nullptr;
    if (cachedHost && isReadable(cachedHost, kThreadScriptHash + 4) &&
        *(uint32_t*)(cachedHost + kThreadId) == cachedId && *(uint32_t*)(cachedHost + kThreadScriptHash) == cachedHash)
        thread = cachedHost;
    if (!thread)
        thread = findScriptThread(kMainPersistent);
    // With stock scripts off (no_sp_scripts=1) startup never runs, so main_persistent may never
    // launch: borrow the startup thread instead (it exists, never ticks, and is never terminated),
    // or, when the hook landed after startup ended, any live thread.
    if (!thread && g_noSpActive) {
        static const uint32_t kStartup = joaat("startup");
        const char* host = "startup";
        thread = findScriptThread(kStartup);
        if (!thread) { thread = findAnyScriptThread(); host = "the first live script thread"; }
        static bool loggedHost = false;
        if (thread && !loggedHost) {
            loggedHost = true;
            GLOGI("hook: script host: main_persistent never launched (stock scripts off); using %s "
                  "(hash 0x%08X)", host, *(uint32_t*)(thread + g_build->threadScriptHash));
        }
    }
    if (!thread) {
        // Periodic (not one-shot): if main_persistent is NEVER found, the script tick never goes
        // live and the engine never gets a valid local ped. Logging the elapsed time makes a stuck
        // load visible in gage.log instead of one early line that scrolls away.
        static ULONGLONG firstWait = 0, lastWaitLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (!firstWait) firstWait = now;
        if (now - lastWaitLog >= 5000) {
            lastWaitLog = now;
            GLOGW("hook: script tick: main_persistent not running yet; waiting (%llu s)",
                  (unsigned long long)((now - firstWait) / 1000));
        }
        cachedHost = nullptr;
        return;
    }
    if (thread != cachedHost) {
        cachedHost = thread;
        cachedId = *(uint32_t*)(thread + kThreadId);
        cachedHash = *(uint32_t*)(thread + kThreadScriptHash);
    }
    // TLS slot 0 of GTA5.exe (TEB ThreadLocalStoragePointer[0]) -> rage::tlsContext.
    uint8_t* tls = *(uint8_t**)__readgsqword(0x58);
    if (!tls || !isReadable(tls + kTlsScriptThread, 9)) return;
    void* prevThread = nullptr;
    bool prevActive = false;
    memcpy(&prevThread, tls + kTlsScriptThread, 8);
    memcpy(&prevActive, tls + kTlsScriptThreadActive, 1);
    if (!loggedFirst) {
        GLOGI("hook: script tick live (thread %p, id %u, script 0x%08X; previous active thread %p)",
              (void*)thread, *(uint32_t*)(thread + kThreadId), *(uint32_t*)(thread + g_build->threadScriptHash),
              prevThread);
        loggedFirst = true;
    }
    *(uint8_t**)(tls + kTlsScriptThread) = thread;
    *(bool*)(tls + kTlsScriptThreadActive) = true;
    g_scriptTickRanAt.store(GetTickCount64());
    g_onScriptTick();
    // Still inside the borrowed script context: the read-only NETWORK_IS_* samples (net_probe=1).
    if (g_netProbe) netProbeTick();
    memcpy(tls + kTlsScriptThread, &prevThread, 8);
    memcpy(tls + kTlsScriptThreadActive, &prevActive, 1);
}

// Find run_script_threads and hook it. Poller thread, after natives validate (the
// code is decrypted by then). True once installed or definitively impossible.
bool tryInstallScriptTick() {
    if (g_scriptHook.active) return true;
    if (!g_build->scriptThreadsSig) return false;  // not derived for this build
    uint8_t* p = findPat(g_imageBase, g_imageSize, g_build->scriptThreadsSig);
    if (!p) return false;
    uint8_t* fn = p + g_build->scriptFnOffset;  // 3889: match-0x1F; 1158: match-0xA
    // The ScriptThreads atArray: a separate rip-relative sig on 1158 (mov rax,[rip+disp];
    // &atArray = match+7+disp), or derived from the run_script_threads sig on 3889.
    uint8_t* arr = nullptr;
    if (g_build->scriptArraySig) {
        uint8_t* ap = findPat(g_imageBase, g_imageSize, g_build->scriptArraySig);
        if (!ap) {
            GLOGW("hook: script-thread array signature not found; script tick off");
            return true;
        }
        int32_t da = 0;
        memcpy(&da, ap + 3, 4);
        arr = ap + 7 + (int64_t)da;
    } else {
        int32_t d = 0;
        memcpy(&d, p - 4, 4);
        arr = p + (int64_t)d - 8;  // rip(p - 4) = p + disp, then - 8
    }
    const uintptr_t lo = (uintptr_t)g_imageBase, hi = lo + g_imageSize;
    if ((uintptr_t)fn < lo || (uintptr_t)arr < lo || (uintptr_t)arr + sizeof(ScriptThreadArray) > hi) {
        GLOGW("hook: script threads signature at GTA5.exe+0x%llx points outside the image; script tick off",
              (unsigned long long)(p - g_imageBase));
        return true;
    }
    g_scriptThreads = (ScriptThreadArray*)arr;
    GLOGI("hook: script threads signature at GTA5.exe+0x%llx: run_script_threads GTA5.exe+0x%llx, threads "
          "GTA5.exe+0x%llx", (unsigned long long)(p - g_imageBase), (unsigned long long)(fn - g_imageBase),
          (unsigned long long)(arr - g_imageBase));
    g_scriptTickThunk = scriptTickBody;
    if (!installInlineHook(fn, g_scriptHook, nullptr, (void* const*)&g_scriptTickThunk, "run_script_threads")) {
        g_scriptTickThunk = nullptr;
        GLOGW("hook: script tick not installed; game work stays on the render thread (no spawning)");
    }
    return true;
}

// The watchdog's last resort (present_hook.h): the inline hook on Present's code, whose thunk stores the swap chain
// and calls OnInlineEntry, which ticks only when the vtable detour has not already done so for this Present.
void (*g_inlineEntry)() = &gage::presenthook::OnInlineEntry;
bool installPresentInlineFallback(uint8_t* fn) {
    return installInlineHook(fn, g_hook, gage::presenthook::SwapChainStorage(), (void* const*)&g_inlineEntry,
                             "D3D11 Present (fallback)");
}
void removePresentInlineFallback() { removeInlineHook(g_hook); }

} // namespace

void World::Unhook() {
    removeInlineHook(g_scriptHook);
    g_scriptTickThunk = nullptr;
    gage::presenthook::Uninstall();     // the slot back, the watchdog stopped, its inline fallback removed
    removeInlineHook(g_hook);
    g_onTick = nullptr;
    netUnpatchAll();
    gage::netguard::Uninstall();   // import-table slots back to the originals (probe / ROS guard)
}

bool World::HookTick(void (*onTick)()) {
    uint8_t* target = g_presentAddr;
    if (!target) return false;
    if (noDetour) {
        GLOGW("hook: hooks=2: Present resolved at %p but not detoured", target);
        return false;
    }
    gage::presenthook::Uninstall();
    removeInlineHook(g_hook);
    g_onTick = onTick;

    // Default: replace the Present slot in dxgi's vtable (present_hook.h). Overlays (RTSS, Afterburner, Steam,
    // Discord, NVIDIA, OBS) patch Present's CODE and re-assert their patch; an inline E9 of ours on the same bytes
    // lived for one frame and then the engine never ticked again (RTSS, 2026-11-03). The slot is data none of them
    // touch, and their patch stays in the chain beneath us. The inline hook remains as the watchdog's fallback for a
    // swap chain that bypasses the vtable, and as present_mode=0, the pre-0.4.7 behaviour.
    if (presentMode != 0 && g_presentSlot) {
        gage::presenthook::Callbacks cb;
        cb.tick = onTick;
        if (presentMode != 1) {
            cb.installInline = installPresentInlineFallback;
            cb.removeInline = removePresentInlineFallback;
        }
        if (gage::presenthook::Install(g_presentSlot, target, cb))
            return true;
        GLOGW("hook: Present vtable hook not installed%s", presentMode == 1 ? " (present_mode=1: no inline fallback)"
                                                                           : "; using the inline hook");
        if (presentMode == 1) return false;
    } else {
        GLOGI("hook: Present: inline hook (%s)", presentMode == 0 ? "present_mode=0" : "no vtable slot resolved");
    }
    return installInlineHook(target, g_hook, gage::presenthook::SwapChainStorage(), (void* const*)&g_onTick,
                             "D3D11 Present");
}

void World::HookScriptTick(void (*onScriptTick)()) { g_onScriptTick = onScriptTick; }

// Installed and ran in the last second (main_persistent can be missing while the
// game loads; the render-thread path covers that).
bool World::ScriptTickActive() const {
    return g_scriptHook.active && GetTickCount64() - g_scriptTickRanAt.load() < 1000;
}

// Poller thread, once natives validate: hook the script tick, retrying while the
// signature is not decrypted yet (every 0.5 s, for up to 2 minutes).
void installScriptTickWhenReady() {
    for (int i = 0; i < 240; ++i) {
        if (tryInstallScriptTick()) return;
        pollCodePatches();
        Sleep(500);
    }
    GLOGW("hook: script threads signature not found in 2 minutes; script tick off");
}

bool World::Hooked() const { return gage::presenthook::Active() || g_hook.active; }

bool World::StockScriptsOff() const { return g_noSpActive; }

uint32_t World::Resolved() const {
    uint32_t m = 0;
    if (g_presentAddr)      m |= 1u;   // bit0: main_loop (D3D11 Present hook target)
    if (g_nativeTableReady) m |= 2u;   // bit1: native_invocation (table entries validated)
    return m;
}

bool World::HaveNatives() const { return g_nativeTableReady; }

// ---------------------------------------------------------------------------
// Native invocation: CallNative
// ---------------------------------------------------------------------------
static bool callNativeRaw(uint64_t hash, const uint64_t* args, int argCount, uint64_t* result);

bool World::CallNative(uint64_t hash, const uint64_t* args, int argCount,
                       uint64_t* result)
{
    return callNativeRaw(hash, args, argCount, result);
}

// The body of World::CallNative, callable without a World (netProbeTick).
static bool callNativeRaw(uint64_t hash, const uint64_t* args, int argCount, uint64_t* result)
{
    if (!g_nativeTableReady)
        return false;

    NativeHandler handler = findNativeHandler(hash);
    if (!handler)
        return false;

    // Set up the call context on the stack.
    uint64_t retBuf[4] = {};
    uint64_t argBuf[32] = {};
    if (argCount > 32) argCount = 32;
    if (args && argCount > 0)
        memcpy(argBuf, args, (size_t)argCount * 8);

    // 16-aligned: the game writes Vector3* results into 16-byte vectors at +0x40 (below).
    alignas(16) scrNativeCallContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.returnValue = retBuf;
    ctx.argCount    = (uint32_t)argCount;
    ctx.args        = argBuf;
    ctx.dataCount   = 0;
    memset(ctx.vectorSpace, 0, sizeof(ctx.vectorSpace));

    handler(&ctx);

    // Vector3* arguments (GET_SHAPE_TEST_RESULT, GET_PED_LAST_WEAPON_IMPACT_COORD, ...): the native
    // writes each result into an aligned scratch vector in vectorSpace and records the caller's
    // pointer; the caller copies it back (FiveM scrNativeCallContext::SetVectorResults, YimMenu's
    // fix_vectors). vectorSpace: +0x00 scrVector* out[4], +0x20 16-byte {x, y, z, pad} in[4]; the
    // out pointer is a script scrVector (x, y, z each in its own 8-byte slot). A Vector3 RETURN
    // never uses this: it is written straight into returnValue as 3 padded slots (dataCount 0).
    // (This block used to copy the out[] POINTER bytes into retBuf instead; no native then
    // in use took a Vector3*, so nothing depended on it.)
    const int vecCount = ctx.dataCount < 0 ? 0 : (ctx.dataCount > 4 ? 4 : ctx.dataCount);
    for (int i = 0; i < vecCount; ++i) {
        uint8_t* out = nullptr;
        memcpy(&out, ctx.vectorSpace + i * 8, sizeof(out));
        if (!out) continue;
        const uint8_t* in = ctx.vectorSpace + 0x20 + i * 16;
        memcpy(out + 0,  in + 0, 4);
        memcpy(out + 8,  in + 4, 4);
        memcpy(out + 16, in + 8, 4);
    }

    if (result)
        memcpy(result, retBuf, sizeof(retBuf));

    return true;
}

// ---------------------------------------------------------------------------
// G0 network baseline (net_probe=1): read-only
// ---------------------------------------------------------------------------
// After the entry (player ped present and the loading screen gone; or 120 s after the script tick
// went live), sample the NETWORK_IS_* getters every 10 s for 60 s (7 lines), with the g_isNetGame
// flag and the SC init status dword read directly (resolved off the game thread by a one-shot scan,
// which also writes the wider recon dumps when net_session is off). Calls only getters; patches nothing.
static std::atomic<const volatile uint8_t*>  g_probeIsNetGame{nullptr};
static std::atomic<const volatile uint32_t*> g_probeScStatus{nullptr};

static DWORD WINAPI netProbeScanThread(void*) {
    if (g_build != &kProfile3889) {
        GLOGI("netprobe: g_isNetGame / SC status not resolved: build is %s (patterns are Legacy 3889 only)",
              g_build->label);
        return 0;
    }
    const uint8_t* m = nullptr;
    if (countPat(g_imageBase, g_imageSize, "48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B D9 74 5F", &m) == 1)
        g_probeIsNetGame.store(netResolveRip(m + 4));                // cmp byte [rip+flag],0
    m = nullptr;
    if (countPat(g_imageBase, g_imageSize, "E8 ?? ?? ?? ?? 84 C0 75 B6 88 05", &m) == 1)
        g_probeScStatus.store((const volatile uint32_t*)netResolveRip(m + 15));   // mov dword [rip+status],0xB
    const volatile uint8_t* ng = g_probeIsNetGame.load();
    const volatile uint32_t* sc = g_probeScStatus.load();
    GLOGI("netprobe: g_isNetGame %s +0x%llx, SC status dword %s +0x%llx (read-only)",
          ng ? "@" : "UNRESOLVED", ng ? (unsigned long long)((const uint8_t*)ng - g_imageBase) : 0ULL,
          sc ? "@" : "UNRESOLVED", sc ? (unsigned long long)((const uint8_t*)sc - g_imageBase) : 0ULL);
    if (!g_netSession) dumpNetExtras("netprobe");   // net_session=1 writes them in its recon passes
    return 0;
}

// BOOL getter -> 0/1, or -1 when the native did not resolve.
static int probeBool(uint64_t hash) {
    uint64_t r[4] = {};
    if (!callNativeRaw(hash, nullptr, 0, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

static void netProbeTick() {
    constexpr int kSamples = 7;              // t = 0, 10, ..., 60 s
    constexpr ULONGLONG kEveryMs = 10000, kLongstopMs = 120000;
    static ULONGLONG firstTick = 0, startAt = 0, nextAt = 0;
    static int samples = 0;
    if (samples >= kSamples) return;
    const ULONGLONG now = GetTickCount64();
    if (!firstTick) firstTick = now;
    if (!startAt) {
        uint64_t r[4] = {};
        const bool ped = callNativeRaw(native::PLAYER_PED_ID, nullptr, 0, r) && (int32_t)r[0] != 0;
        uint64_t l[4] = {};
        const bool loading = callNativeRaw(native::GET_IS_LOADING_SCREEN_ACTIVE, nullptr, 0, l) && (uint32_t)l[0];
        const bool entered = ped && !loading;
        if (!entered && now - firstTick < kLongstopMs) return;
        startAt = nextAt = now;
        GLOGI("netprobe: %s; sampling NETWORK_IS_* every 10 s for 60 s (read-only)",
              entered ? "entry seen (player ped present, loading screen off)"
                      : "no entry 120 s after the script tick went live (longstop)");
        HANDLE t = CreateThread(nullptr, 0, netProbeScanThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else GLOGW("netprobe: scan thread failed to start (g_isNetGame / SC status not read)");
    }
    if (now < nextAt) return;
    nextAt += kEveryMs;
    const int signedIn  = probeBool(native::NETWORK_IS_SIGNED_IN);
    const int signedOn  = probeBool(native::NETWORK_IS_SIGNED_ONLINE);
    const int privs     = probeBool(native::NETWORK_HAVE_ONLINE_PRIVILEGES);
    const int cloud     = probeBool(native::NETWORK_IS_CLOUD_AVAILABLE);
    const int started   = probeBool(native::NETWORK_IS_SESSION_STARTED);
    const int inSession = probeBool(native::NETWORK_IS_IN_SESSION);
    char ngs[16] = "?", scs[48] = "?";
    const volatile uint8_t* ng = g_probeIsNetGame.load();
    const volatile uint32_t* sc = g_probeScStatus.load();
    if (ng && isReadable((const void*)ng, 1)) snprintf(ngs, sizeof(ngs), "%u", (unsigned)*ng);
    if (sc && isReadable((const void*)sc, 4)) {
        const uint32_t v = *sc;
        snprintf(scs, sizeof(scs), "0x%X (%s)", v, scStatusName(v));
    }
    const gage::netguard::Stats st = gage::netguard::GetStats();
    GLOGI("netprobe: t+%2llus signedIn=%d signedOnline=%d onlinePrivileges=%d cloud=%d sessionStarted=%d "
          "inSession=%d | g_isNetGame=%s scStatus=%s | guard=%s names=%u blocked=%u/%u slots=%u lost=%u",
          (unsigned long long)((now - startAt) / 1000), signedIn, signedOn, privs, cloud, started, inSession,
          ngs, scs, gage::netguard::GuardActive() ? "ACTIVE" : "off", st.names, st.namesBlocked, st.addrsBlocked,
          st.slots, st.slotsLost);
    if (++samples >= kSamples)
        GLOGI("netprobe: baseline done (%d samples; -1 = native unresolved). Hostname/destination lines keep "
              "logging as new ones appear.", samples);
}

// ---------------------------------------------------------------------------
// High-level entity API (all via natives)
// ---------------------------------------------------------------------------

int32_t World::LocalPlayer() {
    uint64_t ret[4] = {};
    if (!CallNative(native::PLAYER_PED_ID, nullptr, 0, ret))
        return 0;
    return (int32_t)ret[0];
}

int32_t World::SpawnPed(int modelHash, float x, float y, float z) {
    static bool loggedModelWait = false, loggedCreateFail = false;
    // Request the model first. If not yet loaded, return 0 (caller retries).
    uint64_t reqArgs[] = { (uint64_t)(uint32_t)modelHash };
    CallNative(native::REQUEST_MODEL, reqArgs, 1, nullptr);

    uint64_t loadedRet[4] = {};
    CallNative(native::HAS_MODEL_LOADED, reqArgs, 1, loadedRet);
    if (!loadedRet[0]) {
        if (!loggedModelWait) {
            loggedModelWait = true;
            GLOGI("hook: SpawnPed waiting for model 0x%08x to stream in", (unsigned)modelHash);
        }
        return 0;  // not yet loaded
    }

    // CREATE_PED(int pedType, Hash modelHash, float x, float y, float z,
    //            float heading, BOOL isNetwork, BOOL bScriptHostPed)
    uint64_t args[] = {
        26,                              // pedType: PED_TYPE_MISSION
        (uint64_t)(uint32_t)modelHash,
        FloatArg(x), FloatArg(y), FloatArg(z),
        FloatArg(0.0f),                  // heading
        0,                               // isNetwork: false
        1                                // bScriptHostPed: true
    };
    uint64_t ret[4] = {};
    if (!CallNative(native::CREATE_PED, args, 8, ret))
        return 0;

    int32_t handle = (int32_t)ret[0];
    if (!handle && !loggedCreateFail) {
        loggedCreateFail = true;
        GLOGW("hook: SpawnPed CREATE_PED returned 0 for model 0x%08x (model was loaded)", (unsigned)modelHash);
    }

    // Set default appearance + make invincible + freeze.
    if (handle) {
        uint64_t pedArgs[] = { (uint64_t)(uint32_t)handle };
        CallNative(native::SET_PED_DEFAULT_COMPONENT_VARIATION, pedArgs, 1, nullptr);

        uint64_t invArgs[] = { (uint64_t)(uint32_t)handle, 1 };
        CallNative(native::SET_ENTITY_INVINCIBLE, invArgs, 2, nullptr);

        uint64_t freezeArgs[] = { (uint64_t)(uint32_t)handle, 1 };
        CallNative(native::FREEZE_ENTITY_POSITION, freezeArgs, 2, nullptr);

        // Release the model reference now that it's spawned.
        uint64_t relArgs[] = { (uint64_t)(uint32_t)modelHash };
        CallNative(native::SET_MODEL_AS_NO_LONGER_NEEDED, relArgs, 1, nullptr);
    }

    return handle;
}

bool World::DeleteEntity(int32_t& handle) {
    if (!handle) return false;
    // DELETE_ENTITY takes a pointer to the handle and zeroes it.
    uint64_t args[] = { (uint64_t)(uintptr_t)&handle };
    return CallNative(native::DELETE_ENTITY, args, 1, nullptr);
}

bool World::GetEntityCoords(int32_t handle, float& x, float& y, float& z) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, 1 /* alive */ };
    uint64_t ret[4] = {};
    if (!CallNative(native::GET_ENTITY_COORDS, args, 2, ret))
        return false;
    // GET_ENTITY_COORDS returns a Vector3 through the vectorSpace mechanism.
    // After our CallNative fixup, ret[0..2] hold the float-encoded components.
    memcpy(&x, &ret[0], 4);
    memcpy(&y, &ret[1], 4);
    memcpy(&z, &ret[2], 4);
    return true;
}

bool World::SetEntityCoords(int32_t handle, float x, float y, float z) {
    if (!handle) return false;
    // SET_ENTITY_COORDS(Entity, x, y, z, xAxis, yAxis, zAxis, clearArea)
    uint64_t args[] = {
        (uint64_t)(uint32_t)handle,
        FloatArg(x), FloatArg(y), FloatArg(z),
        0, 0, 0,  // xAxis, yAxis, zAxis: false
        0          // clearArea: false
    };
    return CallNative(native::SET_ENTITY_COORDS, args, 8, nullptr);
}

float World::GetEntityHeading(int32_t handle) {
    if (!handle) return 0.0f;
    uint64_t args[] = { (uint64_t)(uint32_t)handle };
    uint64_t ret[4] = {};
    if (!CallNative(native::GET_ENTITY_HEADING, args, 1, ret))
        return 0.0f;
    float heading;
    memcpy(&heading, &ret[0], 4);
    return heading;
}

bool World::SetEntityHeading(int32_t handle, float heading) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, FloatArg(heading) };
    return CallNative(native::SET_ENTITY_HEADING, args, 2, nullptr);
}

bool World::FreezeEntityPosition(int32_t handle, bool freeze) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, (uint64_t)freeze };
    return CallNative(native::FREEZE_ENTITY_POSITION, args, 2, nullptr);
}

bool World::SetEntityVisible(int32_t handle, bool visible) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, (uint64_t)visible, 0 };
    return CallNative(native::SET_ENTITY_VISIBLE, args, 3, nullptr);
}

bool World::SetEntityInvincible(int32_t handle, bool invincible) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, (uint64_t)invincible };
    return CallNative(native::SET_ENTITY_INVINCIBLE, args, 2, nullptr);
}

bool World::IsPedDeadOrDying(int32_t handle) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, 1 /* p1 */ };
    uint64_t ret[4] = {};
    if (!CallNative(native::IS_PED_DEAD_OR_DYING, args, 2, ret))
        return false;
    return ret[0] != 0;
}

void World::DumpLiveScripts(const char* when) {
    const size_t kThreadId = g_build->threadId, kThreadHandler = g_build->threadHandler,
                 kThreadScriptHash = g_build->threadScriptHash;
    const char* tag = (when && *when) ? when : "?";
    if (!g_scriptThreads || !isReadable(g_scriptThreads, sizeof(ScriptThreadArray))) {
        GLOGW("hook: dump live scripts (%s): thread array not resolved yet", tag);
        return;
    }
    const ScriptThreadArray a = *g_scriptThreads;
    if (!a.data || a.size > 4096 || !isReadable(a.data, (size_t)a.size * sizeof(uint8_t*))) {
        GLOGW("hook: dump live scripts (%s): thread array not readable", tag);
        return;
    }
    int live = 0;
    for (uint16_t i = 0; i < a.size; ++i) {
        uint8_t* t = a.data[i];
        if (!t || !isReadable(t, kThreadScriptHash + 4)) continue;
        uint32_t id = 0, h = 0;
        void* handler = nullptr;
        memcpy(&id, t + kThreadId, 4);
        memcpy(&handler, t + kThreadHandler, 8);
        memcpy(&h, t + kThreadScriptHash, 4);
        if (!id || !handler) continue;  // a free/parked slot, not a running script
        const char* name = gage::engine::spscripts::LookupName(h);
        if (name)
            GLOGI("hook: live script [%s] id %u  %s (0x%08x)", tag, id, name, h);
        else
            GLOGI("hook: live script [%s] id %u  (unknown) 0x%08x", tag, id, h);
        ++live;
    }
    GLOGI("hook: dump live scripts (%s): %d running", tag, live);
}

void World::SuppressSPSystems() {
    // T5 ambient: the ambient half of this function (random events, trains, trucks, boats, parked cars,
    // random cops, dispatch, police / gang flags, wanted multiplier, radar blips) moved to
    // SuppressAmbientSystems, which the engine's ambient tick runs every 2 s whatever the connection
    // state. This one keeps the SP-session protection (autosave, phone, story scripts) and stays at the
    // entry and the gated 5 s call.
    // Block saving: SET_MISSION_FLAG(true) stops the game writing the multiplayer character into
    // the player's singleplayer savegame; also terminate the autosave controller script.
    uint64_t missionFlag[] = { 1 };
    CallNative(native::SET_MISSION_FLAG, missionFlag, 1, nullptr);
    static const char kAutosaveCtl[] = "autosave_controller";
    uint64_t term[] = { (uint64_t)(uintptr_t)kAutosaveCtl };
    CallNative(native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, term, 1, nullptr);

    // Remove the singleplayer phone (and the texts it drives): terminate the cellphone
    // controller and flush any queued notifications. The feed itself is hidden every frame
    // in SuppressSPFrame.
    static const char kCellphoneCtl[] = "cellphone_controller";
    uint64_t cell[] = { (uint64_t)(uintptr_t)kCellphoneCtl };
    CallNative(native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, cell, 1, nullptr);
    CallNative(native::THEFEED_FLUSH_QUEUE, nullptr, 0, nullptr);

    // Terminate the singleplayer story / activity scripts that drive the campaign voice and the
    // on-screen mission text. These are leaf scripts the game itself terminates on mission end;
    // terminating them is safe (a no-op when the named script is not running) and they do not
    // respawn in free-roam. The session root is deliberately NOT here: main_persistent is our own
    // script host, and main / startup own the singleplayer session itself -- terminating those
    // would tear down the world we run inside. DumpLiveScripts names anything still running so
    // this list can be extended from evidence rather than guesswork. The list is defined in
    // sp_scripts.h and unit-tested (script_suppress_test) to never overlap the session roots.
    size_t storyCount = 0;
    const char* const* story = gage::engine::spscripts::StoryScriptsToTerminate(storyCount);
    for (size_t i = 0; i < storyCount; ++i) {
        uint64_t sa[] = { (uint64_t)(uintptr_t)story[i] };
        CallNative(native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, sa, 1, nullptr);
    }
}

void World::SuppressEntryBoot() {
    // Terminate the SP mission-flow scripts (flow_controller, prologue1, prologuescreen, ...) every
    // tick through the entry window, so a fresh Story Mode boot's Prologue never runs. flow_controller
    // is the processor that (re)launches prologue1, so terminating prologue1 alone is not enough --
    // the flow relaunches it (gage.log: prologue1 reappeared as id 68 and teleported the player to
    // North Yankton). The session roots are never in this list (sp_scripts.h, unit-tested). This does
    // NOT touch the player ped or world position -- the entry still owns the model swap and teleport.
    size_t storyCount = 0;
    const char* const* story = gage::engine::spscripts::StoryScriptsToTerminate(storyCount);
    for (size_t i = 0; i < storyCount; ++i) {
        uint64_t sa[] = { (uint64_t)(uintptr_t)story[i] };
        CallNative(native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, sa, 1, nullptr);
    }
    // Kick the game out of the new-game / intro state so it does not sit waiting on the flow we just
    // killed. Idempotent; the self-driven respawn path uses the same native.
    CallNative(native::FORCE_GAME_STATE_PLAYING, nullptr, 0, nullptr);
}

void World::SuppressPedHijackers() {
    // Terminate the story-character scripts (sp_scripts.h PostEntryHijackers, unit-tested to never
    // overlap the session roots) every tick after the entry: main relaunches player_controller a few
    // seconds in, and it swaps the player to the story character. A no-op while they are not running.
    size_t n = 0;
    const char* const* hijack = gage::engine::spscripts::PostEntryHijackers(n);
    for (size_t i = 0; i < n; ++i) {
        uint64_t sa[] = { (uint64_t)(uintptr_t)hijack[i] };
        CallNative(native::TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, sa, 1, nullptr);
    }
}

void World::SuppressSPFrame() {
    // Hold disabled (pad 0): 19 the character wheel, 27 the phone, 37 the weapon wheel (Tab,
    // reserved for our inventory), 166-169 the MP character-select keys (Michael / Franklin /
    // Trevor / Multiplayer). The pause menu (Esc) is intentionally left alone.
    for (uint64_t control : { (uint64_t)19, (uint64_t)27, (uint64_t)37, (uint64_t)166, (uint64_t)167,
                              (uint64_t)168, (uint64_t)169 }) {
        uint64_t args[] = { 0, control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
        CallNative(native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
    }
    // The radar is DisplayRadar's since 0.4.22 (the engine's TickMinimap: shown after the entry, minimap=0 hides it). Keep the
    // rest of the HUD (reticle) shown.
    uint64_t show[] = { 1 };
    CallNative(native::DISPLAY_HUD, show, 1, nullptr);
    // Hide GTA's notification / SMS / phone toasts (the "feed") every frame.
    CallNative(native::THEFEED_HIDE_THIS_FRAME, nullptr, 0, nullptr);
    CallNative(native::CLEAR_CODE_REQUESTED_AUTOSAVE, nullptr, 0, nullptr);

    uint64_t player[4] = {};
    if (CallNative(native::PLAYER_ID, nullptr, 0, player)) {
        const uint64_t pid = (uint64_t)(uint32_t)player[0];
        uint64_t rew[] = { pid };  // DISABLE_PLAYER_VEHICLE_REWARDS(player)
        CallNative(native::DISABLE_PLAYER_VEHICLE_REWARDS, rew, 1, nullptr);
        uint64_t wArgs[] = { pid };
        uint64_t wl[4] = {};
        if (CallNative(native::GET_PLAYER_WANTED_LEVEL, wArgs, 1, wl) && (uint32_t)wl[0] > 0) {
            uint64_t clr[] = { pid };  // CLEAR_PLAYER_WANTED_LEVEL(player)
            CallNative(native::CLEAR_PLAYER_WANTED_LEVEL, clr, 1, nullptr);
        }
    }
}

bool World::SetPlayerModel(uint32_t modelHash) {
    uint64_t model[] = { modelHash };
    CallNative(native::REQUEST_MODEL, model, 1, nullptr);
    uint64_t loaded[4] = {};
    if (!CallNative(native::HAS_MODEL_LOADED, model, 1, loaded) || !(uint32_t)loaded[0])
        return false;  // still streaming
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player))
        return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], modelHash };
    CallNative(native::SET_PLAYER_MODEL, args, 2, nullptr);
    // The player now has a new ped: give it its default clothes (a freemode ped
    // without components renders as a floating head).
    const int32_t ped = LocalPlayer();
    if (ped) {
        uint64_t pedArgs[] = { (uint64_t)(uint32_t)ped };
        CallNative(native::SET_PED_DEFAULT_COMPONENT_VARIATION, pedArgs, 1, nullptr);
    }
    CallNative(native::SET_MODEL_AS_NO_LONGER_NEEDED, model, 1, nullptr);

    // Report only a swap that verifiably took: the player ped's model must now be the one asked
    // for. A swap that did not take returns false so the caller retries.
    static int loggedNotTaken = 0;
    static bool loggedNoVerify = false;
    if (!ped)
        return false;  // no player ped to check yet
    uint64_t verifyArg[] = { (uint64_t)(uint32_t)ped };
    uint64_t verifyRet[4] = {};
    if (!CallNative(native::GET_ENTITY_MODEL, verifyArg, 1, verifyRet)) {
        // GET_ENTITY_MODEL cannot be called (unresolved): the swap cannot be checked, and
        // retrying would re-run SET_PLAYER_MODEL every frame. Keep the old unchecked result.
        if (!loggedNoVerify) {
            loggedNoVerify = true;
            GLOGW("hook: SetPlayerModel cannot verify the swap (GET_ENTITY_MODEL unresolved); assuming it took");
        }
        return true;
    }
    const uint32_t now = (uint32_t)verifyRet[0];
    if (now != modelHash) {
        if (loggedNotTaken < 3) {
            ++loggedNotTaken;
            GLOGW("hook: SetPlayerModel: player ped model is 0x%08x, wanted 0x%08x; swap did not take, retrying",
                  (unsigned)now, (unsigned)modelHash);
        }
        return false;
    }
    return true;
}

// The component half of ApplyPedOutfit and ApplyPedLook. Asks IS_PED_COMPONENT_VARIATION_VALID
// about every slot BEFORE touching the ped (an index the game does not have is a missing-texture
// checkerboard or another item, never what we meant), then resets the ped to its model defaults
// and sets each valid slot, so an invalid one really falls back to the default for that component.
// palettes[i] per slot, or null for palette 2 everywhere. Returns -1 (ped untouched) when the
// validator cannot be called, else the number of slots set. who/loggedNoValidator/loggedInvalid
// keep each caller's one-shot log lines apart (one bit per component, so each warns once).
static int applyValidComponents(World& w, uint64_t h, const PedComponent* slots, const int* palettes,
                                size_t count, const char* who, bool& loggedNoValidator,
                                uint32_t& loggedInvalid) {
    const size_t kMaxSlots = 12;       // one per ePedVarComp component
    if (count > kMaxSlots)
        count = kMaxSlots;
    // GTA's own scripts and the FiveM/RAGE-MP freemode examples (v-besttorso) pass palette 2.
    const int kPalette = 2;

    bool valid[kMaxSlots] = {};
    for (size_t i = 0; i < count; ++i) {
        uint64_t args[] = { h, (uint64_t)(uint32_t)slots[i].component,
                            (uint64_t)(uint32_t)slots[i].drawable, (uint64_t)(uint32_t)slots[i].texture };
        uint64_t ret[4] = {};
        if (!w.CallNative(native::IS_PED_COMPONENT_VARIATION_VALID, args, 4, ret)) {
            if (!loggedNoValidator) {
                loggedNoValidator = true;
                GLOGW("hook: %s: IS_PED_COMPONENT_VARIATION_VALID cannot be called; look not applied", who);
            }
            return -1;
        }
        valid[i] = (uint32_t)ret[0] != 0;
    }

    uint64_t pedArgs[] = { h };
    w.CallNative(native::SET_PED_DEFAULT_COMPONENT_VARIATION, pedArgs, 1, nullptr);

    int applied = 0;
    for (size_t i = 0; i < count; ++i) {
        const PedComponent& s = slots[i];
        if (!valid[i]) {
            const uint32_t bit = 1u << ((unsigned)s.component & 31u);
            if (!(loggedInvalid & bit)) {
                loggedInvalid |= bit;
                GLOGW("hook: %s: %s (component %d, drawable %d, texture %d) is not valid on this ped; keeping the default",
                      who, s.what ? s.what : "slot", s.component, s.drawable, s.texture);
            }
            continue;
        }
        const int palette = palettes ? palettes[i] : kPalette;
        uint64_t args[] = { h, (uint64_t)(uint32_t)s.component, (uint64_t)(uint32_t)s.drawable,
                            (uint64_t)(uint32_t)s.texture, (uint64_t)(uint32_t)palette };
        if (w.CallNative(native::SET_PED_COMPONENT_VARIATION, args, 5, nullptr))
            ++applied;
    }
    return applied;
}

// Face and skin tone (SET_PED_HEAD_BLEND_DATA, isParent false). Call it after the components so
// the blend is what the ped ends up showing; it has no validity query, the IDs are the caller's.
static bool applyHeadBlend(World& w, uint64_t h, const PedHeadBlend& head) {
    uint64_t blend[] = {
        h,
        (uint64_t)(uint32_t)head.shapeFirst, (uint64_t)(uint32_t)head.shapeSecond, (uint64_t)(uint32_t)head.shapeThird,
        (uint64_t)(uint32_t)head.skinFirst,  (uint64_t)(uint32_t)head.skinSecond,  (uint64_t)(uint32_t)head.skinThird,
        FloatArg(head.shapeMix), FloatArg(head.skinMix), FloatArg(head.thirdMix),
        0                                // isParent: false
    };
    return w.CallNative(native::SET_PED_HEAD_BLEND_DATA, blend, 11, nullptr);
}

bool World::ApplyPedOutfit(int32_t ped, const PedHeadBlend& head,
                           const PedComponent* slots, size_t count) {
    if (!ped || !slots || !count)
        return false;
    if (count > 12)
        count = 12;
    const uint64_t h = (uint64_t)(uint32_t)ped;

    // If the validator cannot be called, set nothing (the ped keeps what SetPlayerModel gave it).
    static bool loggedNoValidator = false;
    static uint32_t loggedInvalid = 0;
    const int applied = applyValidComponents(*this, h, slots, nullptr, count, "ApplyPedOutfit",
                                             loggedNoValidator, loggedInvalid);
    if (applied < 0)
        return false;

    const bool blended = applyHeadBlend(*this, h, head);

    static bool loggedApplied = false;
    if (!loggedApplied) {
        loggedApplied = true;
        GLOGI("hook: ApplyPedOutfit: ped %d dressed, %u of %u slots set, head blend %s",
              (int)ped, (unsigned)applied, (unsigned)count, blended ? "set" : "unavailable");
    }
    return true;
}

void World::DisableWanted() {
    uint64_t zero[] = { 0 };
    CallNative(native::SET_MAX_WANTED_LEVEL, zero, 1, nullptr);
    uint64_t player[4] = {};
    if (CallNative(native::PLAYER_ID, nullptr, 0, player)) {
        uint64_t args[] = { (uint64_t)(uint32_t)player[0] };
        CallNative(native::CLEAR_PLAYER_WANTED_LEVEL, args, 1, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Self-driven respawn (0.1.53): the stock respawn_controller leaves a freemode
// ped on a black screen after death (it re-enables the post-death fade-in only
// for the three story characters). The engine fades and respawns the player
// itself; the state machine lives in TickLife (engine.cpp).
// ---------------------------------------------------------------------------
bool World::IsScreenFadedOut() {
    uint64_t r[4] = {};
    return CallNative(native::IS_SCREEN_FADED_OUT, nullptr, 0, r) && (uint32_t)r[0];
}

void World::DoScreenFadeIn(int ms) {
    uint64_t a[] = { (uint64_t)(uint32_t)ms };
    CallNative(native::DO_SCREEN_FADE_IN, a, 1, nullptr);
}

void World::DoScreenFadeOut(int ms) {
    uint64_t a[] = { (uint64_t)(uint32_t)ms };
    CallNative(native::DO_SCREEN_FADE_OUT, a, 1, nullptr);
}

void World::HoldDeathRestart() {
    // Stop the stock death/arrest machine from running its own restart or fade:
    // it bails for a freemode ped and leaves the screen black. We fade and
    // respawn ourselves instead.
    uint64_t on[]  = { 1 };
    uint64_t off[] = { 0 };
    CallNative(native::PAUSE_DEATH_ARREST_RESTART,     on,  1, nullptr);
    CallNative(native::IGNORE_NEXT_RESTART,            on,  1, nullptr);
    CallNative(native::SET_FADE_OUT_AFTER_DEATH,       off, 1, nullptr);
    CallNative(native::SET_FADE_OUT_AFTER_ARREST,      off, 1, nullptr);
    CallNative(native::SET_FADE_IN_AFTER_DEATH_ARREST, off, 1, nullptr);
}

void World::RestoreDeathDefaults() {
    uint64_t on[]  = { 1 };
    uint64_t off[] = { 0 };
    CallNative(native::PAUSE_DEATH_ARREST_RESTART,     off, 1, nullptr);
    CallNative(native::SET_FADE_OUT_AFTER_DEATH,       on,  1, nullptr);
    CallNative(native::SET_FADE_OUT_AFTER_ARREST,      on,  1, nullptr);
    CallNative(native::SET_FADE_IN_AFTER_DEATH_ARREST, on,  1, nullptr);
}

void World::ClearDeathEffects() {
    CallNative(native::ANIMPOSTFX_STOP_ALL, nullptr, 0, nullptr);
    uint64_t camOff[] = { 0 };  // SET_CAM_DEATH_FAIL_EFFECT_STATE(false)
    CallNative(native::SET_CAM_DEATH_FAIL_EFFECT_STATE, camOff, 1, nullptr);
    static const char kDeathScene[] = "DEATH_SCENE";
    uint64_t scene[] = { (uint64_t)(uintptr_t)kDeathScene };
    CallNative(native::STOP_AUDIO_SCENE, scene, 1, nullptr);
    uint64_t ts[] = { FloatArg(1.0f) };  // SET_TIME_SCALE(1.0) — clear any death slow-mo
    CallNative(native::SET_TIME_SCALE, ts, 1, nullptr);
}

bool World::ReviveLocalPlayer(float x, float y, float z, float heading) {
    const int32_t ped = LocalPlayer();
    if (!ped)
        return false;
    const uint64_t h = (uint64_t)(uint32_t)ped;
    uint64_t pedArg[] = { h };

    // Bring the ped back and drop the death ragdoll task.
    CallNative(native::RESURRECT_PED, pedArg, 1, nullptr);
    CallNative(native::CLEAR_PED_TASKS_IMMEDIATELY, pedArg, 1, nullptr);

    // Reposition at the spawn point (proven helpers, used by the server entry).
    SetEntityCoords(ped, x, y, z);
    SetEntityHeading(ped, heading);

    // Full health. GET_ENTITY_MAX_HEALTH can return 0 if it failed to resolve;
    // fall back to the freemode default rather than setting the ped to 0 (dead).
    uint64_t mh[4] = {};
    uint32_t maxHp = 0;
    if (CallNative(native::GET_ENTITY_MAX_HEALTH, pedArg, 1, mh))
        maxHp = (uint32_t)mh[0];
    if (maxHp == 0 || maxHp > 5000)
        maxHp = 200;
    uint64_t hp[] = { h, (uint64_t)maxHp, 0, 0 };  // SET_ENTITY_HEALTH(ped, max, 0, 0)
    CallNative(native::SET_ENTITY_HEALTH, hp, 4, nullptr);

    // Clear the blood and visible damage the death left on the model.
    CallNative(native::CLEAR_PED_BLOOD_DAMAGE, pedArg, 1, nullptr);
    CallNative(native::RESET_PED_VISIBLE_DAMAGE, pedArg, 1, nullptr);

    // Reset the player's arrest state and force the world back to normal play.
    uint64_t player[4] = {};
    if (CallNative(native::PLAYER_ID, nullptr, 0, player)) {
        uint64_t parg[] = { (uint64_t)(uint32_t)player[0] };
        CallNative(native::RESET_PLAYER_ARREST_STATE, parg, 1, nullptr);
    }
    CallNative(native::FORCE_GAME_STATE_PLAYING, nullptr, 0, nullptr);

    // Freeze in place until the ground collision around the spawn streams in, so
    // the ped does not fall through the map (the Loading phase unfreezes).
    FreezeEntityPosition(ped, true);
    return true;
}

void World::RequestCollisionAt(float x, float y, float z) {
    uint64_t a[] = { FloatArg(x), FloatArg(y), FloatArg(z) };
    CallNative(native::REQUEST_COLLISION_AT_COORD, a, 3, nullptr);
}

bool World::HasCollisionLoadedAround(int32_t handle) {
    if (!handle)
        return false;
    uint64_t a[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    return CallNative(native::HAS_COLLISION_LOADED_AROUND_ENTITY, a, 1, r) && (uint32_t)r[0];
}

void World::ShutdownLoadingScreen() {
    // Tell GTA to drop its own loading screen. On a fresh Story Mode boot whose mission flow we
    // killed, the game would otherwise hold it up for ever (nothing finishes the new-game flow). A
    // no-op when no loading screen is up (e.g. the shared respawn reveal).
    CallNative(native::SHUTDOWN_LOADING_SCREEN, nullptr, 0, nullptr);
}

void World::SetPlayerControl(bool on) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player))
        return;
    // SET_PLAYER_CONTROL(player, bHasControl, flags = 0). Killing the mission flow means the game
    // never hands control over at boot; we grant it at the reveal. A harmless re-assert on respawn.
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], on ? 1ull : 0ull, 0ull };
    CallNative(native::SET_PLAYER_CONTROL, args, 3, nullptr);
}

// ---------------------------------------------------------------------------
// HUD bars (0.1.54)
// ---------------------------------------------------------------------------
void World::DrawRect(float cx, float cy, float w, float h, int r, int g, int b, int a) {
    uint64_t args[] = { FloatArg(cx), FloatArg(cy), FloatArg(w), FloatArg(h),
                        (uint64_t)(uint32_t)r, (uint64_t)(uint32_t)g,
                        (uint64_t)(uint32_t)b, (uint64_t)(uint32_t)a, 0 };
    CallNative(native::DRAW_RECT, args, 9, nullptr);
}

int World::GetEntityHealth(int32_t handle) {
    if (!handle) return 0;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_HEALTH, arg, 1, r)) return 0;
    return (int)(int32_t)(uint32_t)r[0];
}

int World::GetEntityMaxHealth(int32_t handle) {
    if (!handle) return 0;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_MAX_HEALTH, arg, 1, r)) return 0;
    return (int)(int32_t)(uint32_t)r[0];
}

// ---------------------------------------------------------------------------
// Client-side commands (0.1.54): /car /gun /heal /kill /fix
// ---------------------------------------------------------------------------
uint32_t World::HashKey(const char* name) {
    if (!name || !*name) return 0;
    uint64_t arg[] = { (uint64_t)(uintptr_t)name };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_HASH_KEY, arg, 1, r)) return 0;
    return (uint32_t)r[0];
}

bool World::IsVehicleModel(uint32_t modelHash) {
    uint64_t arg[] = { modelHash };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_MODEL_IN_CDIMAGE, arg, 1, r) || !(uint32_t)r[0])
        return false;  // not a real model
    uint64_t r2[4] = {};
    return CallNative(native::IS_MODEL_A_VEHICLE, arg, 1, r2) && (uint32_t)r2[0];
}

void World::RequestModel(uint32_t modelHash) {
    uint64_t arg[] = { modelHash };
    CallNative(native::REQUEST_MODEL, arg, 1, nullptr);
}

bool World::HasModelLoaded(uint32_t modelHash) {
    uint64_t arg[] = { modelHash };
    uint64_t r[4] = {};
    return CallNative(native::HAS_MODEL_LOADED, arg, 1, r) && (uint32_t)r[0];
}

void World::ModelNoLongerNeeded(uint32_t modelHash) {
    uint64_t arg[] = { modelHash };
    CallNative(native::SET_MODEL_AS_NO_LONGER_NEEDED, arg, 1, nullptr);
}

int32_t World::CreateVehicle(uint32_t modelHash, float x, float y, float z, float heading) {
    // CREATE_VEHICLE(model, x, y, z, heading, isNetwork, netMissionEntity, p7)
    uint64_t args[] = { modelHash, FloatArg(x), FloatArg(y), FloatArg(z),
                        FloatArg(heading), 0, 0, 0 };
    uint64_t r[4] = {};
    if (!CallNative(native::CREATE_VEHICLE, args, 8, r)) return 0;
    return (int32_t)(uint32_t)r[0];
}

bool World::SetPedIntoVehicle(int32_t ped, int32_t veh, int seat) {
    if (!ped || !veh) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)veh,
                        (uint64_t)(uint32_t)(int32_t)seat };  // seat -1 = driver
    return CallNative(native::SET_PED_INTO_VEHICLE, args, 3, nullptr);
}

void World::PlaceVehicleOnGround(int32_t veh) {
    if (!veh) return;
    uint64_t args[] = { (uint64_t)(uint32_t)veh, FloatArg(5.0f) };
    CallNative(native::SET_VEHICLE_ON_GROUND_PROPERLY, args, 2, nullptr);
}

bool World::SetVehicleEngineOn(int32_t veh, bool on, bool instantly, bool disableAutoStart) {
    if (!veh) return false;
    // SET_VEHICLE_ENGINE_ON(veh, on, instantly, disableAutoStart)
    uint64_t args[] = { (uint64_t)(uint32_t)veh, on ? 1ull : 0ull, instantly ? 1ull : 0ull, disableAutoStart ? 1ull : 0ull };
    return CallNative(native::SET_VEHICLE_ENGINE_ON, args, 4, nullptr);
}

int32_t World::GetVehiclePedIsIn(int32_t ped) {
    if (!ped) return 0;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, 0 };  // lastVehicle = false
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_PED_IS_IN, args, 2, r)) return 0;
    return (int32_t)(uint32_t)r[0];
}

void World::FixVehicle(int32_t veh) {
    if (!veh) return;
    uint64_t arg[] = { (uint64_t)(uint32_t)veh };
    CallNative(native::SET_VEHICLE_FIXED, arg, 1, nullptr);
    CallNative(native::SET_VEHICLE_DEFORMATION_FIXED, arg, 1, nullptr);
    SetVehicleEngineOn(veh, true);
}

void World::GiveWeapon(int32_t ped, uint32_t weaponHash, int ammo) {
    if (!ped) return;
    // GIVE_WEAPON_TO_PED(ped, weapon, ammo, isHidden, equipNow)
    uint64_t args[] = { (uint64_t)(uint32_t)ped, weaponHash,
                        (uint64_t)(uint32_t)ammo, 0, 1 };
    CallNative(native::GIVE_WEAPON_TO_PED, args, 5, nullptr);
}

bool World::SetEntityHealth(int32_t handle, int health) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, (uint64_t)(uint32_t)health, 0, 0 };
    return CallNative(native::SET_ENTITY_HEALTH, args, 4, nullptr);
}

void World::HealLocal() {
    const int32_t ped = LocalPlayer();
    if (!ped) return;
    int maxHp = GetEntityMaxHealth(ped);
    if (maxHp <= 0 || maxHp > 5000) maxHp = 200;
    SetEntityHealth(ped, maxHp);
    uint64_t pedArg[] = { (uint64_t)(uint32_t)ped };
    CallNative(native::CLEAR_PED_BLOOD_DAMAGE, pedArg, 1, nullptr);
    CallNative(native::RESET_PED_VISIBLE_DAMAGE, pedArg, 1, nullptr);
}

bool World::IsPedInVehicle(int32_t ped) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, 0 };  // atGetIn = false
    uint64_t r[4] = {};
    return CallNative(native::IS_PED_IN_ANY_VEHICLE, args, 2, r) && (uint32_t)r[0];
}

float World::GetEntitySpeed(int32_t handle) {
    if (!handle) return 0.0f;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_SPEED, arg, 1, r)) return 0.0f;
    float f = 0.0f;
    memcpy(&f, &r[0], 4);  // native returns a float in the low 4 bytes
    return f;
}

// ---------------------------------------------------------------------------
// World clean-up (ambient density, area clears, default SP blips), the join
// readiness gate, and model verify.
// ---------------------------------------------------------------------------
// T5 ambient: one native call of the ambient clean-up, counted by native in ambientTally. A hash of 0
// (UNRESOLVED: lookup needed) never reaches the game: it is counted "inert". The first call of each
// native logs once, so the density natives' resolution on this build (Epic Legacy vs Steam Enhanced) is
// in gage.log, and an unresolved hash can no longer hide behind "NPCs still appear".
static bool ambCall(World& w, gage::ambient::Id id, const char* nativeName, uint64_t hash,
                    const uint64_t* args, int argc, uint64_t* ret = nullptr) {
    namespace amb = gage::ambient;
    amb::Result r = amb::Result::Inert;
    bool ok = false;
    if (hash != 0) {
        ok = w.CallNative(hash, args, argc, ret);
        r = ok ? amb::Result::Ok : amb::Result::Failed;
    }
    if (w.ambientTally.Note(id, r)) {
        if (r == amb::Result::Ok)
            GLOGI("ambient: first use of %s: call worked", nativeName);
        else if (r == amb::Result::Failed)
            GLOGW("ambient: first use of %s FAILED: the hash does not resolve on this build, or the call failed "
                  "(what it controls is NOT being held off; see the 'hook: new natives' line)", nativeName);
        else
            GLOGW("ambient: %s has no hash yet (UNRESOLVED: lookup needed): inert, what it would stop stays visible",
                  nativeName);
    }
    return ok;
}
#define GAGE_AMB(id, NAME, args, argc) ambCall(*this, gage::ambient::Id::id, #NAME, native::NAME, args, argc)

void World::SuppressAmbientFrame() {
    // Each multiplier lasts one frame, so this runs every script tick. 0.0 = none.
    uint64_t zero[] = { FloatArg(0.0f) };
    GAGE_AMB(PedDensity,     SET_PED_DENSITY_MULTIPLIER_THIS_FRAME,            zero, 1);
    GAGE_AMB(VehDensity,     SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME,        zero, 1);
    GAGE_AMB(RandVehDensity, SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, zero, 1);
    GAGE_AMB(ParkedDensity,  SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, zero, 1);
    uint64_t scenario[] = { FloatArg(0.0f), FloatArg(0.0f) };  // both parameters 0.0
    GAGE_AMB(ScenarioDensity, SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME,  scenario, 2);
}

void World::SuppressAmbientSystems(float x, float y, float z, bool haveCoords) {
    uint64_t argFalse[] = { 0 };
    GAGE_AMB(RandomEvents,  SET_RANDOM_EVENT_FLAG, argFalse, 1);
    GAGE_AMB(Trains,        SET_RANDOM_TRAINS,     argFalse, 1);
    GAGE_AMB(GarbageTrucks, SET_GARBAGE_TRUCKS,    argFalse, 1);
    GAGE_AMB(Boats,         SET_RANDOM_BOATS,      argFalse, 1);
    uint64_t parked[] = { (uint64_t)(uint32_t)(int32_t)-1 };  // SET_NUMBER_OF_PARKED_VEHICLES(-1)
    GAGE_AMB(ParkedCount,   SET_NUMBER_OF_PARKED_VEHICLES, parked, 1);
    GAGE_AMB(RandomCops,    SET_CREATE_RANDOM_COPS, argFalse, 1);
    GAGE_AMB(RandomCops,    SET_CREATE_RANDOM_COPS_NOT_ON_SCENARIOS, argFalse, 1);
    GAGE_AMB(RandomCops,    SET_CREATE_RANDOM_COPS_ON_SCENARIOS,     argFalse, 1);

    // Every emergency dispatch service off (1..15: police, fire, ambulance, swat, gangs, army,
    // boats, helicopters). These are event-driven, not wanted-driven, so clearing the wanted
    // level alone does not stop them.
    for (uint64_t svc = 1; svc <= 15; ++svc) {
        uint64_t dargs[] = { svc, 0 };
        GAGE_AMB(Dispatch, ENABLE_DISPATCH_SERVICE, dargs, 2);
    }

    // Per-player cop behaviour.
    uint64_t player[4] = {};
    if (CallNative(native::PLAYER_ID, nullptr, 0, player)) {
        const uint64_t pid = (uint64_t)(uint32_t)player[0];
        uint64_t ignore[]   = { pid, 1 };  // SET_POLICE_IGNORE_PLAYER(player, true)
        GAGE_AMB(PlayerFlags, SET_POLICE_IGNORE_PLAYER, ignore, 2);
        uint64_t dispatch[] = { pid, 0 };  // SET_DISPATCH_COPS_FOR_PLAYER(player, false)
        GAGE_AMB(PlayerFlags, SET_DISPATCH_COPS_FOR_PLAYER, dispatch, 2);
        uint64_t hassle[]   = { pid, 0 };  // SET_PLAYER_CAN_BE_HASSLED_BY_GANGS(player, false)
        GAGE_AMB(PlayerFlags, SET_PLAYER_CAN_BE_HASSLED_BY_GANGS, hassle, 2);
    }
    uint64_t mult[] = { FloatArg(0.0f) };  // SET_WANTED_LEVEL_MULTIPLIER(0.0)
    GAGE_AMB(WantedMult, SET_WANTED_LEVEL_MULTIPLIER, mult, 1);
    uint64_t blips[] = { 0 };              // SET_POLICE_RADAR_BLIPS(false)
    GAGE_AMB(RadarBlips, SET_POLICE_RADAR_BLIPS, blips, 1);

    // Sentinel natives (hash 0 until looked up: inert, counted, logged once). Persistent budgets: 0 peds and
    // 0 vehicles; the existing trains; the vehicle generators off in a box around the player.
    uint64_t budget0[] = { 0 };
    GAGE_AMB(PedBudget, SET_PED_POPULATION_BUDGET,     budget0, 1);
    GAGE_AMB(VehBudget, SET_VEHICLE_POPULATION_BUDGET, budget0, 1);
    GAGE_AMB(DeleteTrains, DELETE_ALL_TRAINS, nullptr, 0);
    if (haveCoords) {
        // SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA(x1, y1, z1, x2, y2, z2, toggle = false, p7): a box of +/- 600 m.
        // p7's meaning is unverified (0 here): GI confirms it when the hash is looked up.
        constexpr float r = 600.0f;
        uint64_t box[] = { FloatArg(x - r), FloatArg(y - r), FloatArg(z - r), FloatArg(x + r), FloatArg(y + r), FloatArg(z + r), 0, 0 };
        GAGE_AMB(GeneratorsArea, SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA, box, 8);
    }
}

void World::ClearAmbientAround(float x, float y, float z, float radius) {
    if (!(radius > 0.0f)) return;  // also rejects NaN
    // CLEAR_AREA_OF_PEDS / CLEAR_AREA_OF_COPS(x, y, z, radius, flags = 0)
    uint64_t area[] = { FloatArg(x), FloatArg(y), FloatArg(z), FloatArg(radius), 0 };
    GAGE_AMB(ClearPeds, CLEAR_AREA_OF_PEDS, area, 5);
    GAGE_AMB(ClearCops, CLEAR_AREA_OF_COPS, area, 5);
    // REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA(x1, y1, z1, x2, y2, z2, p6 = 0): a box of +/- radius.
    // No CLEAR_AREA_OF_VEHICLES: it can delete the player's own car.
    uint64_t box[] = { FloatArg(x - radius), FloatArg(y - radius), FloatArg(z - radius),
                       FloatArg(x + radius), FloatArg(y + radius), FloatArg(z + radius), 0 };
    GAGE_AMB(RemoveGenVeh, REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA, box, 7);
}

bool World::AmbientSweepAvailable() const {
    return native::GET_PED_NEARBY_PEDS != 0 && native::GET_PED_NEARBY_VEHICLES != 0;
}

// GET_PED_NEARBY_PEDS(ped, int* sizeAndPeds, ignore) / GET_PED_NEARBY_VEHICLES(ped, int* sizeAndVehs): the
// array is slot 0 = capacity going in / count coming out, then one handle per 8-byte slot (the layout every
// other pointer argument in this file uses). UNVERIFIED live: ParseNearby rejects an implausible count, the
// sweep drops 0 / -1 and checks DOES_ENTITY_EXIST, and the circuit breaker watches the exempt handles.
static int nearbyCall(World& w, gage::ambient::Id id, const char* name, uint64_t hash, int32_t around,
                      int32_t* out, int cap, bool withIgnore) {
    if (!around || !out || cap <= 0) return -1;
    if (cap > gage::ambient::kNearbyCap) cap = gage::ambient::kNearbyCap;
    uint64_t buf[1 + gage::ambient::kNearbyCap] = {};
    buf[0] = (uint64_t)(uint32_t)cap;
    uint64_t args[] = { (uint64_t)(uint32_t)around, (uint64_t)(uintptr_t)buf, (uint64_t)(uint32_t)(int32_t)-1 };
    if (!ambCall(w, id, name, hash, args, withIgnore ? 3 : 2))
        return -1;
    return gage::ambient::ParseNearby(buf, cap, out, &gage::ambient::g_lastNearbyEcho);   // R3 T5: slot 0 may still hold the capacity
}

int World::NearbyPeds(int32_t around, int32_t* out, int cap) {
    return nearbyCall(*this, gage::ambient::Id::NearbyPeds, "GET_PED_NEARBY_PEDS", native::GET_PED_NEARBY_PEDS,
                      around, out, cap, true);
}

int World::NearbyVehicles(int32_t around, int32_t* out, int cap) {
    return nearbyCall(*this, gage::ambient::Id::NearbyVehs, "GET_PED_NEARBY_VEHICLES", native::GET_PED_NEARBY_VEHICLES,
                      around, out, cap, false);
}

bool World::DeleteAmbientEntity(int32_t handle) {
    if (!gage::ambient::ExemptSet::Valid(handle)) return false;
    // SET_ENTITY_AS_MISSION_ENTITY(entity, bScriptHostObject, bGrabFromOtherScript), then DELETE_ENTITY(&entity).
    uint64_t grab[] = { (uint64_t)(uint32_t)handle, 1, 1 };
    GAGE_AMB(GrabEntity, SET_ENTITY_AS_MISSION_ENTITY, grab, 3);
    uint64_t del[] = { (uint64_t)(uintptr_t)&handle };
    return GAGE_AMB(DeleteEntity, DELETE_ENTITY, del, 1);
}
#undef GAGE_AMB

void World::ClearSpBlips() {
    static bool loggedUnresolved = false, loggedRun = false;
    constexpr int kMaxSprite = 1000;    // sprite ids walked: a generous cap on the BLIP_SPRITE range
    constexpr int kWaypointSprite = 8;  // the player's own map waypoint: kept
    constexpr int kMaxBlips = 512;      // blips collected per call (a cap, not an expected count)

    // Collect first, remove after: the first/next cursor then never sees a blip vanish under it.
    // GET_FIRST_BLIP_INFO_ID(sprite) / GET_NEXT_BLIP_INFO_ID(sprite) walk the blips of one sprite.
    int32_t blips[kMaxBlips];
    int count = 0;
    bool unresolved = false;
    for (int sprite = 1; sprite <= kMaxSprite && count < kMaxBlips && !unresolved; ++sprite) {
        uint64_t spriteArg[] = { (uint64_t)(uint32_t)sprite };
        uint64_t r[4] = {};
        if (!CallNative(native::GET_FIRST_BLIP_INFO_ID, spriteArg, 1, r)) { unresolved = true; break; }
        int32_t blip = (int32_t)(uint32_t)r[0];  // 0 when this sprite has no blip
        // The guard bounds a cursor that never ends.
        for (int guard = 0; blip && guard < kMaxBlips && count < kMaxBlips; ++guard) {
            uint64_t blipArg[] = { (uint64_t)(uint32_t)blip };
            uint64_t exists[4] = {};
            if (!CallNative(native::DOES_BLIP_EXIST, blipArg, 1, exists)) { unresolved = true; break; }
            if (!(uint32_t)exists[0]) break;
            // The sprite the blip really has (GET_BLIP_SPRITE); the key we walked if that cannot be read.
            uint64_t sp[4] = {};
            const int blipSprite = CallNative(native::GET_BLIP_SPRITE, blipArg, 1, sp) ? (int)(uint32_t)sp[0] : sprite;
            if (blipSprite != kWaypointSprite)
                blips[count++] = blip;
            if (!CallNative(native::GET_NEXT_BLIP_INFO_ID, spriteArg, 1, r)) { unresolved = true; break; }
            blip = (int32_t)(uint32_t)r[0];
        }
    }
    if (unresolved) {
        if (!loggedUnresolved) {
            loggedUnresolved = true;
            GLOGW("hook: ClearSpBlips: a blip native did not resolve; default SP blips left on the map");
        }
        return;
    }

    int removed = 0;
    for (int i = 0; i < count; ++i) {
        int32_t blip = blips[i];
        // REMOVE_BLIP takes a Blip* (it zeroes the handle): a pointer in the argument slot.
        uint64_t arg[] = { (uint64_t)(uintptr_t)&blip };
        if (CallNative(native::REMOVE_BLIP, arg, 1, nullptr)) ++removed;
    }
    if (!loggedRun) {
        loggedRun = true;
        GLOGI("hook: ClearSpBlips first run: found %d blip(s), removed %d", count, removed);
    }
}

// --- 0.4.20 map clean-up: the native side of sync/mapclear.h (the engine decides which blip to hide) ---
bool World::CollectBlips(int firstSprite, int lastSprite, std::vector<MapBlip>& out) {
    constexpr size_t kMaxBlips = 2048;   // a cap on one walk, not an expected count
    constexpr int kPerSpriteGuard = 512; // bounds a cursor that never ends
    out.clear();
    // Collect first, act after: GET_FIRST / GET_NEXT_BLIP_INFO_ID(sprite) is a cursor over one sprite's blips.
    for (int sprite = firstSprite; sprite <= lastSprite && out.size() < kMaxBlips; ++sprite) {
        uint64_t spriteArg[] = { (uint64_t)(uint32_t)sprite };
        uint64_t r[4] = {};
        if (!CallNative(native::GET_FIRST_BLIP_INFO_ID, spriteArg, 1, r)) return false;
        int32_t blip = (int32_t)(uint32_t)r[0];  // 0 when this sprite has no blip
        for (int guard = 0; blip && guard < kPerSpriteGuard && out.size() < kMaxBlips; ++guard) {
            uint64_t blipArg[] = { (uint64_t)(uint32_t)blip };
            uint64_t exists[4] = {};
            if (!CallNative(native::DOES_BLIP_EXIST, blipArg, 1, exists)) return false;
            if (!(uint32_t)exists[0]) break;
            uint64_t sp[4] = {}, disp[4] = {};
            const int blipSprite = CallNative(native::GET_BLIP_SPRITE, blipArg, 1, sp) ? (int)(uint32_t)sp[0] : sprite;
            const int display = CallNative(native::GET_BLIP_INFO_ID_DISPLAY, blipArg, 1, disp) ? (int)(uint32_t)disp[0] : -1;
            out.push_back({ blip, blipSprite, display });
            if (!CallNative(native::GET_NEXT_BLIP_INFO_ID, spriteArg, 1, r)) return false;
            blip = (int32_t)(uint32_t)r[0];
        }
    }
    return true;
}

int World::HideBlip(int32_t blip) {
    if (!blip) return -1;
    uint64_t set[] = { (uint64_t)(uint32_t)blip, 0 };   // SET_BLIP_DISPLAY(blip, 0): "doesn't show up, ever, anywhere"
    if (!CallNative(native::SET_BLIP_DISPLAY, set, 2, nullptr)) return -1;
    uint64_t arg[] = { (uint64_t)(uint32_t)blip };
    uint64_t r[4] = {};
    return CallNative(native::GET_BLIP_INFO_ID_DISPLAY, arg, 1, r) ? (int)(uint32_t)r[0] : -1;
}

void World::MapKeepHandles(int32_t& mainPlayer, int32_t& north, int& waypointEnum, bool& waypointEnumOk) {
    uint64_t r[4] = {};
    mainPlayer = CallNative(native::GET_MAIN_PLAYER_BLIP_ID, nullptr, 0, r) ? (int32_t)(uint32_t)r[0] : 0;
    r[0] = 0;
    north = CallNative(native::GET_NORTH_BLID_INDEX, nullptr, 0, r) ? (int32_t)(uint32_t)r[0] : 0;
    r[0] = 0;
    waypointEnumOk = CallNative(native::GET_WAYPOINT_BLIP_ENUM_ID, nullptr, 0, r);
    waypointEnum = waypointEnumOk ? (int)(uint32_t)r[0] : 0;
}

int World::ActiveBlipCount() {
    uint64_t r[4] = {};
    return CallNative(native::GET_NUMBER_OF_ACTIVE_BLIPS, nullptr, 0, r) ? (int)(uint32_t)r[0] : -1;
}

bool World::AllowAnyScriptBlips() {
    uint64_t on[] = { 1 };
    return CallNative(native::SET_THIS_SCRIPT_CAN_REMOVE_BLIPS_CREATED_BY_ANY_SCRIPT, on, 1, nullptr);
}

bool World::ClearWaypoint(bool& wasActive, bool& activeAfter) {
    wasActive = activeAfter = false;
    uint64_t r[4] = {};
    if (!CallNative(native::IS_WAYPOINT_ACTIVE, nullptr, 0, r)) return false;
    wasActive = (uint32_t)r[0] != 0;
    if (!wasActive) return true;
    if (!CallNative(native::SET_WAYPOINT_OFF, nullptr, 0, nullptr)) return false;
    r[0] = 0;
    if (!CallNative(native::IS_WAYPOINT_ACTIVE, nullptr, 0, r)) return false;
    activeAfter = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetMapFowHidden(bool hide) {
    uint64_t arg[] = { hide ? 1u : 0u };
    return CallNative(native::SET_MINIMAP_HIDE_FOW, arg, 1, nullptr);
}

bool World::MapFowReadback(float& ratio, bool& farRevealed) {
    ratio = 0.0f;
    farRevealed = false;
    uint64_t r[4] = {};
    if (!CallNative(native::GET_MINIMAP_FOW_DISCOVERY_RATIO, nullptr, 0, r)) return false;
    memcpy(&ratio, &r[0], 4);  // a float in the low 4 bytes
    // Paleto Bay, the far north of the map: unexplored on a fresh story save.
    uint64_t at[] = { FloatArg(-150.0f), FloatArg(6350.0f), FloatArg(31.0f) };
    r[0] = 0;
    if (!CallNative(native::GET_MINIMAP_FOW_COORDINATE_IS_REVEALED, at, 3, r)) return false;
    farRevealed = (uint32_t)r[0] != 0;
    return true;
}

// --- 0.4.22 minimap: the native side of sync/minimap.h (the engine decides when) ---
bool World::DisplayRadar(bool show) {
    uint64_t arg[] = { show ? 1u : 0u };
    return CallNative(native::DISPLAY_RADAR, arg, 1, nullptr);
}

void World::ReadRadar(RadarReadback& out) {
    out = RadarReadback();
    uint64_t r[4] = {};
    out.hiddenOk = CallNative(native::IS_RADAR_HIDDEN, nullptr, 0, r);
    out.hidden = out.hiddenOk && (uint32_t)r[0] != 0;
    r[0] = 0;
    out.renderingOk = CallNative(native::IS_MINIMAP_RENDERING, nullptr, 0, r);
    out.rendering = out.renderingOk && (uint32_t)r[0] != 0;
    r[0] = 0;
    out.prefOk = CallNative(native::IS_RADAR_PREFERENCE_SWITCHED_ON, nullptr, 0, r);
    out.pref = out.prefOk && (uint32_t)r[0] != 0;
    r[0] = 0;
    out.hudOk = CallNative(native::IS_HUD_HIDDEN, nullptr, 0, r);
    out.hudHidden = out.hudOk && (uint32_t)r[0] != 0;
    r[0] = 0;
    out.waypointOk = CallNative(native::IS_WAYPOINT_ACTIVE, nullptr, 0, r);
    out.waypoint = out.waypointOk && (uint32_t)r[0] != 0;
}

int World::RequestScaleform(const char* name) {
    if (!name) return 0;
    uint64_t arg[] = { (uint64_t)(uintptr_t)name };
    uint64_t r[4] = {};
    if (!CallNative(native::REQUEST_SCALEFORM_MOVIE, arg, 1, r)) return 0;
    return (int)(uint32_t)r[0];
}

bool World::ScaleformLoaded(int handle) {
    if (!handle) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    return CallNative(native::HAS_SCALEFORM_MOVIE_LOADED, arg, 1, r) && (uint32_t)r[0] != 0;
}

bool World::ScaleformCallInt(int handle, const char* method, int arg, bool& began) {
    began = false;
    if (!handle || !method) return false;
    uint64_t begin[] = { (uint64_t)(uint32_t)handle, (uint64_t)(uintptr_t)method };
    uint64_t r[4] = {};
    if (!CallNative(native::BEGIN_SCALEFORM_MOVIE_METHOD, begin, 2, r)) return false;
    began = (uint32_t)r[0] != 0;
    if (!began) return true;                 // nothing was pushed: no add / end (the stack protocol)
    uint64_t add[] = { (uint64_t)(uint32_t)arg };
    if (!CallNative(native::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT, add, 1, nullptr)) return false;
    return CallNative(native::END_SCALEFORM_MOVIE_METHOD, nullptr, 0, nullptr);
}

bool World::SetBigmap(bool on) {
    uint64_t arg[] = { on ? 1u : 0u, 0 };    // SET_BIGMAP_ACTIVE(toggleBigMap, showFullMap = false)
    return CallNative(native::SET_BIGMAP_ACTIVE, arg, 2, nullptr);
}

bool World::SafeZoneSize(float& out) {
    out = 1.0f;
    uint64_t r[4] = {};
    if (!CallNative(native::GET_SAFE_ZONE_SIZE, nullptr, 0, r)) return false;
    memcpy(&out, &r[0], 4);                  // a float in the low 4 bytes
    return true;
}

uint32_t World::GetEntityModel(int32_t handle) {
    if (!handle) return 0;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_MODEL, arg, 1, r)) return 0;
    return (uint32_t)r[0];
}

bool World::IsLoadingScreenActive() {
    uint64_t r[4] = {};
    return CallNative(native::GET_IS_LOADING_SCREEN_ACTIVE, nullptr, 0, r) && (uint32_t)r[0];
}

bool World::IsScreenFadedIn() {
    uint64_t r[4] = {};
    return CallNative(native::IS_SCREEN_FADED_IN, nullptr, 0, r) && (uint32_t)r[0];
}

bool World::IsPlayerSwitchInProgress() {
    uint64_t r[4] = {};
    return CallNative(native::IS_PLAYER_SWITCH_IN_PROGRESS, nullptr, 0, r) && (uint32_t)r[0];
}

bool World::IsPlayerControlOn() {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0] };
    uint64_t r[4] = {};
    return CallNative(native::IS_PLAYER_CONTROL_ON, args, 1, r) && (uint32_t)r[0];
}

// ---------------------------------------------------------------------------
// Remote-player peds: spawn, locomotion task + velocity steer + hard snap, the
// sender's local-state readers, and the look read/apply. Script tick only.
// ---------------------------------------------------------------------------
namespace {
// One BOOL-returning native on a single handle argument: false for 0, uncallable, or FALSE.
bool callPedBool(World& w, uint64_t hash, int32_t ped) {
    if (!ped) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)ped };
    uint64_t r[4] = {};
    return w.CallNative(hash, arg, 1, r) && (uint32_t)r[0];
}
// One void native on (handle, BOOL): "could it be called".
bool callPedToggle(World& w, uint64_t hash, int32_t ped, bool on) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, on ? 1ull : 0ull };
    return w.CallNative(hash, args, 2, nullptr);
}
}  // namespace

int32_t World::SpawnRemotePed(int modelHash, float x, float y, float z, float heading, int recipe) {
    static bool loggedModelWait = false, loggedCreateFail = false, loggedSetupFail = false;
    const uint64_t model = (uint64_t)(uint32_t)modelHash;
    uint64_t reqArgs[] = { model };
    CallNative(native::REQUEST_MODEL, reqArgs, 1, nullptr);
    uint64_t loadedRet[4] = {};
    if (!CallNative(native::HAS_MODEL_LOADED, reqArgs, 1, loadedRet) || !(uint32_t)loadedRet[0]) {
        if (!loggedModelWait) {
            loggedModelWait = true;
            GLOGI("hook: SpawnRemotePed waiting for model 0x%08x to stream in", (unsigned)modelHash);
        }
        return 0;  // not yet loaded: call again next frame
    }

    // CREATE_PED(pedType, model, x, y, z, heading, isNetwork, bScriptHostPed)
    // R3 T2 recipe 2 (ALT): PED_TYPE_CIVMALE (4) and no script-host flag; 0 and 1: PED_TYPE_MISSION, script host.
    uint64_t args[] = { recipe >= 2 ? 4ull : 26ull /* PED_TYPE_MISSION */, model, FloatArg(x), FloatArg(y), FloatArg(z),
                        FloatArg(heading), 0 /* isNetwork */, recipe >= 2 ? 0ull : 1ull /* bScriptHostPed */ };
    uint64_t ret[4] = {};
    if (!CallNative(native::CREATE_PED, args, 8, ret))
        return 0;
    const int32_t ped = (int32_t)ret[0];
    if (!ped) {
        if (!loggedCreateFail) {
            loggedCreateFail = true;
            GLOGW("hook: SpawnRemotePed CREATE_PED returned 0 for model 0x%08x (model was loaded)",
                  (unsigned)modelHash);
        }
        return 0;
    }

    const uint64_t h = (uint64_t)(uint32_t)ped;
    uint64_t pedArg[] = { h };
    CallNative(native::SET_PED_DEFAULT_COMPONENT_VARIATION, pedArg, 1, nullptr);
    // Count the setup natives that could not be called: each one missing changes how the ped
    // behaves (it flinches, dives, ragdolls or wanders), so the count is logged once.
    int failed = 0;
    failed += !SetEntityInvincible(ped, true);
    if (recipe <= 0) {
        failed += !FreezeEntityPosition(ped, false);
        failed += !SetEntityCollision(ped, true);
    }
    failed += !SetEntityHeading(ped, heading);
    failed += !callPedToggle(*this, native::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, ped, true);
    if (recipe <= 0) {
        failed += !SetPedCanRagdoll(ped, false);
        failed += !callPedToggle(*this, native::SET_PED_CAN_EVASIVE_DIVE, ped, false);
        failed += !callPedToggle(*this, native::SET_PED_CAN_PLAY_AMBIENT_ANIMS, ped, false);
        failed += !callPedToggle(*this, native::SET_PED_KEEP_TASK, ped, true);
        failed += !SetPedConfigFlag(ped, 188 /* PCF_DisableHurt */, true);
    }
    if (failed && !loggedSetupFail) {
        loggedSetupFail = true;
        GLOGW("hook: SpawnRemotePed: %d of 10 setup natives could not be called (see 'new natives' line)",
              failed);
    }
    CallNative(native::SET_MODEL_AS_NO_LONGER_NEEDED, reqArgs, 1, nullptr);
    return ped;
}

bool World::IsPedStill(int32_t ped) { return callPedBool(*this, native::IS_PED_STILL, ped); }

bool World::SetEntityCoordsNoOffset(int32_t handle, float x, float y, float z) {
    if (!handle) return false;
    // SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, z, xAxis, yAxis, zAxis)
    uint64_t args[] = { (uint64_t)(uint32_t)handle, FloatArg(x), FloatArg(y), FloatArg(z), 0, 0, 0 };
    return CallNative(native::SET_ENTITY_COORDS_NO_OFFSET, args, 7, nullptr);
}

bool World::SetEntityVelocity(int32_t handle, float vx, float vy, float vz) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, FloatArg(vx), FloatArg(vy), FloatArg(vz) };
    return CallNative(native::SET_ENTITY_VELOCITY, args, 4, nullptr);
}

bool World::GetEntityVelocity(int32_t handle, float& vx, float& vy, float& vz) {
    if (!handle) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_VELOCITY, arg, 1, r))
        return false;
    // A Vector3 return, read the same way as GET_ENTITY_COORDS (see GetEntityCoords).
    memcpy(&vx, &r[0], 4);
    memcpy(&vy, &r[1], 4);
    memcpy(&vz, &r[2], 4);
    return true;
}

bool World::SetEntityCollision(int32_t handle, bool on) {
    if (!handle) return false;
    // SET_ENTITY_COLLISION(entity, toggle, keepPhysics)
    uint64_t args[] = { (uint64_t)(uint32_t)handle, on ? 1ull : 0ull, 1 };
    return CallNative(native::SET_ENTITY_COLLISION, args, 3, nullptr);
}

bool World::TaskGoStraightToCoord(int32_t ped, float x, float y, float z, float speed,
                                  int timeoutMs, float targetHeading, float slideDist) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, FloatArg(x), FloatArg(y), FloatArg(z), FloatArg(speed),
                        (uint64_t)(uint32_t)timeoutMs, FloatArg(targetHeading), FloatArg(slideDist) };
    return CallNative(native::TASK_GO_STRAIGHT_TO_COORD, args, 8, nullptr);
}

bool World::TaskStandStill(int32_t ped, int ms) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)ms };
    return CallNative(native::TASK_STAND_STILL, args, 2, nullptr);
}

bool World::ClearPedTasks(int32_t ped) {
    if (!ped) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)ped };
    return CallNative(native::CLEAR_PED_TASKS, arg, 1, nullptr);
}

bool World::SetPedDesiredMoveBlendRatio(int32_t ped, float mbr) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, FloatArg(mbr) };
    return CallNative(native::SET_PED_DESIRED_MOVE_BLEND_RATIO, args, 2, nullptr);
}

bool World::SetPedDesiredHeading(int32_t ped, float heading) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, FloatArg(heading) };
    return CallNative(native::SET_PED_DESIRED_HEADING, args, 2, nullptr);
}

int World::PedGait(int32_t ped) {
    if (callPedBool(*this, native::IS_PED_SPRINTING, ped)) return 3;
    if (callPedBool(*this, native::IS_PED_RUNNING, ped))   return 2;
    if (callPedBool(*this, native::IS_PED_WALKING, ped))   return 1;
    return 0;
}

bool World::TaskJump(int32_t ped) {
    if (!ped) return false;
    // TASK_JUMP(ped, usePlayerLaunchForce, doSuperJump, useFullSuperJumpForce)
    uint64_t args[] = { (uint64_t)(uint32_t)ped, 0, 0, 0 };
    return CallNative(native::TASK_JUMP, args, 4, nullptr);
}

bool World::IsPedJumping(int32_t ped) { return callPedBool(*this, native::IS_PED_JUMPING, ped); }
bool World::IsPedRagdoll(int32_t ped) { return callPedBool(*this, native::IS_PED_RAGDOLL, ped); }

bool World::SetPedToRagdoll(int32_t ped, int time1Ms, int time2Ms) {
    if (!ped) return false;
    // SET_PED_TO_RAGDOLL(ped, time1, time2, ragdollType 0 = CTaskNMRelax, p4, p5, p6); returns BOOL.
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)time1Ms, (uint64_t)(uint32_t)time2Ms,
                        0, 1, 1, 1 };
    uint64_t r[4] = {};
    return CallNative(native::SET_PED_TO_RAGDOLL, args, 7, r) && (uint32_t)r[0];
}

bool World::SetPedCanRagdoll(int32_t ped, bool can) {
    return callPedToggle(*this, native::SET_PED_CAN_RAGDOLL, ped, can);
}

bool World::SetPedConfigFlag(int32_t ped, int flag, bool value) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)flag, value ? 1ull : 0ull };
    return CallNative(native::SET_PED_CONFIG_FLAG, args, 3, nullptr);
}

bool World::GetPedConfigFlag(int32_t ped, int flag, bool& value) {
    if (!ped) return false;
    // GET_PED_CONFIG_FLAG(ped, flagId, p2 = FALSE): the BOOL return is the flag.
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)flag, 0 };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_CONFIG_FLAG, args, 3, r)) return false;
    value = (uint32_t)r[0] != 0;
    return true;
}

bool World::GetGroundZ(float x, float y, float z, float& groundZ) {
    // GET_GROUND_Z_FOR_3D_COORD(x, y, z, float* groundZ, ignoreWater, p5): the game writes a
    // float into a script slot, so hand it a zeroed 8-byte slot and read its low 4 bytes.
    uint64_t slot = 0;
    uint64_t args[] = { FloatArg(x), FloatArg(y), FloatArg(z), (uint64_t)(uintptr_t)&slot,
                        0 /* ignoreWater: false, as R*'s scripts nearly always pass */, 0 };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_GROUND_Z_FOR_3D_COORD, args, 6, r) || !(uint32_t)r[0])
        return false;
    memcpy(&groundZ, &slot, 4);
    return true;
}

float World::GetEntityHeightAboveGround(int32_t handle) {
    if (!handle) return 0.0f;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_HEIGHT_ABOVE_GROUND, arg, 1, r)) return 0.0f;
    float f = 0.0f;
    memcpy(&f, &r[0], 4);
    return f;
}

bool World::IsPedFalling(int32_t ped)  { return callPedBool(*this, native::IS_PED_FALLING, ped); }
bool World::IsPedClimbing(int32_t ped) { return callPedBool(*this, native::IS_PED_CLIMBING, ped); }
bool World::IsPedInParachuteFreeFall(int32_t ped) {
    return callPedBool(*this, native::IS_PED_IN_PARACHUTE_FREE_FALL, ped);
}
bool World::IsPedShooting(int32_t ped) { return callPedBool(*this, native::IS_PED_SHOOTING, ped); }

bool World::IsLocalPlayerFreeAiming() {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0] };
    uint64_t r[4] = {};
    return CallNative(native::IS_PLAYER_FREE_AIMING, args, 1, r) && (uint32_t)r[0];
}

uint32_t World::GetSelectedPedWeapon(int32_t ped) {
    if (!ped) return 0;
    uint64_t arg[] = { (uint64_t)(uint32_t)ped };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_SELECTED_PED_WEAPON, arg, 1, r)) return 0;
    return (uint32_t)r[0];
}

bool World::IsWeaponValid(uint32_t weaponHash) {
    uint64_t arg[] = { weaponHash };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_WEAPON_VALID, arg, 1, r)) return false;
    return (uint32_t)r[0] != 0;
}

bool World::SetCurrentPedWeapon(int32_t ped, uint32_t weaponHash, bool inHand) {
    if (!ped) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, weaponHash, inHand ? 1ull : 0ull };
    return CallNative(native::SET_CURRENT_PED_WEAPON, args, 3, nullptr);
}

bool World::TaskAimGunAtCoord(int32_t ped, float x, float y, float z, int ms) {
    if (!ped) return false;
    // TASK_AIM_GUN_AT_COORD(ped, x, y, z, time, instantBlendToAim, playAnimIntro)
    uint64_t args[] = { (uint64_t)(uint32_t)ped, FloatArg(x), FloatArg(y), FloatArg(z),
                        (uint64_t)(uint32_t)ms, 1, 0 };
    return CallNative(native::TASK_AIM_GUN_AT_COORD, args, 7, nullptr);
}

bool World::TaskGoToCoordWhileAimingAtCoord(int32_t ped, float x, float y, float z,
                                            float ax, float ay, float az, float mbr, bool shoot) {
    if (!ped) return false;
    // (ped, x, y, z, aimX, aimY, aimZ, moveBlendRatio, shoot, targetRadius, slowDistance,
    //  useNavMesh, navFlags, instantBlendToAim, firingPattern)
    const uint64_t kFiringPatternFullAuto = 0xC6EE6B4C;  // FIRING_PATTERN_FULL_AUTO (NativeDB note)
    uint64_t args[] = { (uint64_t)(uint32_t)ped, FloatArg(x), FloatArg(y), FloatArg(z),
                        FloatArg(ax), FloatArg(ay), FloatArg(az), FloatArg(mbr), shoot ? 1ull : 0ull,
                        FloatArg(0.5f), FloatArg(0.0f), 0, 0, 0, kFiringPatternFullAuto };
    return CallNative(native::TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD, args, 15, nullptr);
}

bool World::ReadPedLook(int32_t ped, PedLook& out) {
    if (!ped) return false;
    const uint64_t h = (uint64_t)(uint32_t)ped;
    PedLook look{};
    look.model = GetEntityModel(ped);
    // An int native on (ped, index[, extra]): its value, or `fail` when it cannot be called.
    auto getInt = [&](uint64_t hash, int index, bool extra, int fail) -> int {
        uint64_t args[] = { h, (uint64_t)(uint32_t)index, 1 };
        uint64_t r[4] = {};
        if (!CallNative(hash, args, extra ? 3 : 2, r)) return fail;
        return (int32_t)(uint32_t)r[0];
    };
    const int kNoCall = INT32_MIN;
    for (int c = 0; c < 12; ++c) {
        const int d = getInt(native::GET_PED_DRAWABLE_VARIATION, c, false, kNoCall);
        if (d == kNoCall) {
            static bool loggedNoGetter = false;
            if (!loggedNoGetter) {
                loggedNoGetter = true;
                GLOGW("hook: ReadPedLook: GET_PED_DRAWABLE_VARIATION cannot be called; look not read");
            }
            return false;
        }
        const int t = getInt(native::GET_PED_TEXTURE_VARIATION, c, false, 0);
        const int p = getInt(native::GET_PED_PALETTE_VARIATION, c, false, 0);
        look.drawable[c] = (int16_t)(d < 0 ? 0 : d);
        look.texture[c]  = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
        look.palette[c]  = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
    }
    for (int a = 0; a < 8; ++a) {
        // GET_PED_PROP_INDEX(ped, anchor, p2): p2 is an undocumented later-build addition (Any);
        // 1 is passed, unverified live. -1 = no prop.
        const int d = getInt(native::GET_PED_PROP_INDEX, a, true, -1);
        look.propDrawable[a] = (int16_t)(d < 0 ? -1 : d);
        if (d >= 0) {
            const int t = getInt(native::GET_PED_PROP_TEXTURE_INDEX, a, false, 0);
            look.propTexture[a] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
        }
    }
    // GET_PED_HEAD_BLEND_DATA fills a script struct: 10 fields, each in its own 8-byte slot (value
    // in the low 4 bytes): 6 ints (shape 1-3, skin 1-3), 3 floats (shape/skin/third mix), isParent.
    // FALSE for a ped without a head blend (any non-freemode model). Extra slots as a guard.
    uint64_t blend[16] = {};
    uint64_t args[] = { h, (uint64_t)(uintptr_t)blend };
    uint64_t r[4] = {};
    if (CallNative(native::GET_PED_HEAD_BLEND_DATA, args, 2, r) && (uint32_t)r[0]) {
        auto i32 = [&](int i) { return (int32_t)(uint32_t)blend[i]; };
        auto f32 = [&](int i) { float f; uint32_t u = (uint32_t)blend[i]; memcpy(&f, &u, 4); return f; };
        look.hasHead = true;
        look.head = PedHeadBlend{ i32(0), i32(1), i32(2), i32(3), i32(4), i32(5), f32(6), f32(7), f32(8) };
    }
    out = look;
    return true;
}

bool World::ApplyPedLook(int32_t ped, const PedLook& look) {
    if (!ped) return false;
    const uint64_t h = (uint64_t)(uint32_t)ped;
    // Component indices only mean something on the model they were read from.
    const uint32_t model = GetEntityModel(ped);
    if (look.model && model && model != look.model) {
        static bool loggedModel = false;
        if (!loggedModel) {
            loggedModel = true;
            GLOGW("hook: ApplyPedLook: ped %d is model 0x%08x, look is for 0x%08x; not applied",
                  (int)ped, (unsigned)model, (unsigned)look.model);
        }
        return false;
    }

    PedComponent slots[12];
    int palettes[12];
    for (int c = 0; c < 12; ++c) {
        slots[c] = PedComponent{ c, look.drawable[c], look.texture[c], "look" };
        palettes[c] = look.palette[c];
    }
    static bool loggedNoValidator = false;
    static uint32_t loggedInvalid = 0;
    if (applyValidComponents(*this, h, slots, palettes, 12, "ApplyPedLook", loggedNoValidator,
                             loggedInvalid) < 0)
        return false;

    for (int a = 0; a < 8; ++a) {
        if (look.propDrawable[a] < 0) {
            // CLEAR_PED_PROP(ped, anchor, p2): p2 undocumented (Any); 1 passed, unverified live.
            uint64_t args[] = { h, (uint64_t)(uint32_t)a, 1 };
            CallNative(native::CLEAR_PED_PROP, args, 3, nullptr);
        } else {
            // SET_PED_PROP_INDEX(ped, anchor, drawable, texture, attach, p5)
            uint64_t args[] = { h, (uint64_t)(uint32_t)a, (uint64_t)(uint32_t)look.propDrawable[a],
                                (uint64_t)look.propTexture[a], 1, 0 };
            CallNative(native::SET_PED_PROP_INDEX, args, 6, nullptr);
        }
    }
    if (look.hasHead)
        applyHeadBlend(*this, h, look.head);
    return true;
}

// ---------------------------------------------------------------------------
// Vehicle sync, weapons/shots/damage, camera/aim. Script tick only. Same conventions as the
// remote-ped block: bool = "the native could be called" unless hook.h says otherwise.
// ---------------------------------------------------------------------------
namespace {
// A script Vector3 as a native's Vector3* argument sees it: x, y, z each in its own 8-byte slot
// (FiveM scrVector, ScriptHookV's Vector3). CallNative copies the result into it (see there).
struct ScrVector {
    alignas(8) float x;
    alignas(8) float y;
    alignas(8) float z;
};
static_assert(sizeof(ScrVector) == 24, "scrVector is 3 x 8-byte slots");

inline uint64_t entArg(int32_t h) { return (uint64_t)(uint32_t)h; }
inline uint64_t boolArg(bool b) { return b ? 1ull : 0ull; }
inline uint64_t ptrArg(const void* p) { return (uint64_t)(uintptr_t)p; }
inline float floatOf(uint64_t slot) { float f = 0.0f; memcpy(&f, &slot, 4); return f; }
inline int32_t intOf(uint64_t slot) { return (int32_t)(uint32_t)slot; }
// A Vector3 return (3 padded slots in r[0..2]), as GetEntityCoords reads it.
inline void vec3Of(const uint64_t* r, float& x, float& y, float& z) {
    x = floatOf(r[0]); y = floatOf(r[1]); z = floatOf(r[2]);
}
// One void native on (handle, float).
bool callHandleFloat(World& w, uint64_t hash, int32_t h, float v) {
    if (!h) return false;
    uint64_t args[] = { entArg(h), FloatArg(v) };
    return w.CallNative(hash, args, 2, nullptr);
}
// One void native on (handle, int).
bool callHandleInt(World& w, uint64_t hash, int32_t h, int v) {
    if (!h) return false;
    uint64_t args[] = { entArg(h), (uint64_t)(uint32_t)v };
    return w.CallNative(hash, args, 2, nullptr);
}
// One float-returning native on a handle.
bool callHandleGetFloat(World& w, uint64_t hash, int32_t h, float& out) {
    if (!h) return false;
    uint64_t arg[] = { entArg(h) };
    uint64_t r[4] = {};
    if (!w.CallNative(hash, arg, 1, r)) return false;
    out = floatOf(r[0]);
    return true;
}
// One handle-returning native on a handle: 0 for 0 or uncallable.
int32_t callHandleGetHandle(World& w, uint64_t hash, int32_t h) {
    if (!h) return 0;
    uint64_t arg[] = { entArg(h) };
    uint64_t r[4] = {};
    if (!w.CallNative(hash, arg, 1, r)) return 0;
    return intOf(r[0]);
}
constexpr int      kRotOrder              = 2;           // ZXY, R*'s script default
constexpr uint32_t kFiringPatternFullAuto = 0xC6EE6B4C;  // FIRING_PATTERN_FULL_AUTO
}  // namespace

bool World::GetEntityQuaternion(int32_t handle, float& x, float& y, float& z, float& w) {
    if (!handle) return false;
    // GET_ENTITY_QUATERNION(entity, float* x, float* y, float* z, float* w): plain float pointers
    // (no vector fix-up), each a zeroed 8-byte slot like GetGroundZ's.
    uint64_t s[4] = {};
    uint64_t args[] = { entArg(handle), ptrArg(&s[0]), ptrArg(&s[1]), ptrArg(&s[2]), ptrArg(&s[3]) };
    if (!CallNative(native::GET_ENTITY_QUATERNION, args, 5, nullptr)) return false;
    x = floatOf(s[0]); y = floatOf(s[1]); z = floatOf(s[2]); w = floatOf(s[3]);
    return true;
}

bool World::SetEntityQuaternion(int32_t handle, float x, float y, float z, float w) {
    if (!handle) return false;
    uint64_t args[] = { entArg(handle), FloatArg(x), FloatArg(y), FloatArg(z), FloatArg(w) };
    return CallNative(native::SET_ENTITY_QUATERNION, args, 5, nullptr);
}

bool World::GetEntityRotation(int32_t handle, float& pitch, float& roll, float& yaw) {
    if (!handle) return false;
    uint64_t args[] = { entArg(handle), (uint64_t)kRotOrder };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_ROTATION, args, 2, r)) return false;
    vec3Of(r, pitch, roll, yaw);
    return true;
}

bool World::SetEntityRotation(int32_t handle, float pitch, float roll, float yaw) {
    if (!handle) return false;
    // SET_ENTITY_ROTATION(entity, pitch, roll, yaw, rotationOrder, p5 "usually true")
    uint64_t args[] = { entArg(handle), FloatArg(pitch), FloatArg(roll), FloatArg(yaw),
                        (uint64_t)kRotOrder, 1 };
    return CallNative(native::SET_ENTITY_ROTATION, args, 6, nullptr);
}

bool World::GetEntityRotationVelocity(int32_t handle, float& x, float& y, float& z) {
    if (!handle) return false;
    uint64_t arg[] = { entArg(handle) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_ROTATION_VELOCITY, arg, 1, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

bool World::SetEntityAngularVelocity(int32_t handle, float x, float y, float z) {
    if (!handle) return false;
    uint64_t args[] = { entArg(handle), FloatArg(x), FloatArg(y), FloatArg(z) };
    return CallNative(native::SET_ENTITY_ANGULAR_VELOCITY, args, 4, nullptr);
}

int32_t World::GetPedInVehicleSeat(int32_t veh, int seat) {
    if (!veh) return 0;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)seat, 0 /* p2: default occupant lookup */ };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_IN_VEHICLE_SEAT, args, 3, r)) return 0;
    return intOf(r[0]);
}

int World::GetVehicleMaxPassengers(int32_t veh) {
    if (!veh) return -1;
    uint64_t arg[] = { entArg(veh) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS, arg, 1, r)) return -1;
    return intOf(r[0]);
}

bool World::IsVehicleSeatFree(int32_t veh, int seat, bool countEntering) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)seat, boolArg(countEntering) };
    uint64_t r[4] = {};
    return CallNative(native::IS_VEHICLE_SEAT_FREE, args, 3, r) && (uint32_t)r[0];
}

int World::VehicleSeatFreeState(int32_t veh, int seat, bool countEntering) {
    if (!veh) return -1;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)seat, boolArg(countEntering) };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_VEHICLE_SEAT_FREE, args, 3, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

int World::GetVehicleModelNumberOfSeats(uint32_t model) {
    if (!model) return -1;
    uint64_t arg[] = { (uint64_t)model };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_MODEL_NUMBER_OF_SEATS, arg, 1, r)) return -1;
    return intOf(r[0]);
}

int World::GetVehicleDoorLockStatus(int32_t veh) {
    if (!veh) return -1;
    uint64_t arg[] = { entArg(veh) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_DOOR_LOCK_STATUS, arg, 1, r)) return -1;
    return intOf(r[0]);
}

bool World::PlaySoundFrontend(const char* name, const char* set) {
    if (!name || !set) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)-1, ptrArg(name), ptrArg(set), boolArg(false) };
    return CallNative(native::PLAY_SOUND_FRONTEND, args, 4, nullptr);
}

bool World::GetSeatPedIsTryingToEnter(int32_t ped, int& seat) {
    if (!ped) return false;
    uint64_t arg[] = { entArg(ped) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_SEAT_PED_IS_TRYING_TO_ENTER, arg, 1, r)) return false;
    seat = intOf(r[0]);
    return true;
}

int32_t World::GetVehiclePedIsTryingToEnter(int32_t ped) {
    return callHandleGetHandle(*this, native::GET_VEHICLE_PED_IS_TRYING_TO_ENTER, ped);
}

int32_t World::GetVehiclePedIsUsing(int32_t ped) {
    return callHandleGetHandle(*this, native::GET_VEHICLE_PED_IS_USING, ped);
}

bool World::TaskEnterVehicle(int32_t ped, int32_t veh, int timeoutMs, int seat, float speed, int flags) {
    if (!ped || !veh) return false;
    // TASK_ENTER_VEHICLE(ped, vehicle, timeout, seat, speed, flag, overrideEntryClipsetName, p7):
    // 8 arguments on 3889 (YimMenu natives.hpp; alloc8or lists 7). No clipset override, p7 0.
    uint64_t args[] = { entArg(ped), entArg(veh), (uint64_t)(uint32_t)timeoutMs, (uint64_t)(uint32_t)seat,
                        FloatArg(speed), (uint64_t)(uint32_t)flags, 0, 0 };
    return CallNative(native::TASK_ENTER_VEHICLE, args, 8, nullptr);
}

bool World::TaskLeaveVehicle(int32_t ped, int32_t veh, int flags) {
    if (!ped || !veh) return false;
    uint64_t args[] = { entArg(ped), entArg(veh), (uint64_t)(uint32_t)flags };
    return CallNative(native::TASK_LEAVE_VEHICLE, args, 3, nullptr);
}

bool World::TaskWarpPedIntoVehicle(int32_t ped, int32_t veh, int seat) {
    if (!ped || !veh) return false;
    uint64_t args[] = { entArg(ped), entArg(veh), (uint64_t)(uint32_t)seat };
    return CallNative(native::TASK_WARP_PED_INTO_VEHICLE, args, 3, nullptr);
}

bool World::SetVehicleSteerBias(int32_t veh, float bias) {
    return callHandleFloat(*this, native::SET_VEHICLE_STEER_BIAS, veh, bias);
}

bool World::GetVehicleEngineHealth(int32_t veh, float& health) {
    return callHandleGetFloat(*this, native::GET_VEHICLE_ENGINE_HEALTH, veh, health);
}

bool World::SetVehicleEngineHealth(int32_t veh, float health) {
    return callHandleFloat(*this, native::SET_VEHICLE_ENGINE_HEALTH, veh, health);
}

bool World::GetVehicleBodyHealth(int32_t veh, float& health) {
    return callHandleGetFloat(*this, native::GET_VEHICLE_BODY_HEALTH, veh, health);
}

bool World::SetVehicleBodyHealth(int32_t veh, float health) {
    return callHandleFloat(*this, native::SET_VEHICLE_BODY_HEALTH, veh, health);
}

bool World::IsVehicleEngineRunning(int32_t veh) {
    return callPedBool(*this, native::GET_IS_VEHICLE_ENGINE_RUNNING, veh);
}

bool World::GetVehicleEngineRunning(int32_t veh, bool& running) {
    if (!veh) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)veh };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_IS_VEHICLE_ENGINE_RUNNING, arg, 1, r)) return false;
    running = (uint32_t)r[0] != 0;
    return true;
}

bool World::HasEntityCollidedWithAnything(int32_t entity) {
    return callPedBool(*this, native::HAS_ENTITY_COLLIDED_WITH_ANYTHING, entity);
}

int World::GetVehicleClass(int32_t veh) {
    if (!veh) return -1;
    uint64_t arg[] = { entArg(veh) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_CLASS, arg, 1, r)) return -1;
    return intOf(r[0]);
}

bool World::GetPedBoneCoords(int32_t ped, int bone, float ox, float oy, float oz, float& x, float& y, float& z) {
    if (!ped) return false;
    // GET_PED_BONE_COORDS(ped, boneId, offsetX, offsetY, offsetZ): a Vector3 return, read like GetEntityCoords.
    uint64_t args[] = { entArg(ped), (uint64_t)(uint32_t)bone, FloatArg(ox), FloatArg(oy), FloatArg(oz) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_BONE_COORDS, args, 5, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

bool World::GetScreenCoordFromWorldCoord(float x, float y, float z, float& sx, float& sy) {
    // GET_SCREEN_COORD_FROM_WORLD_COORD(x, y, z, float* screenX, float* screenY): BOOL, the two floats are written through
    // pointers into zeroed 8-byte slots (like GetEntityQuaternion's), read from their low 4 bytes.
    uint64_t s[2] = {};
    uint64_t args[] = { FloatArg(x), FloatArg(y), FloatArg(z), ptrArg(&s[0]), ptrArg(&s[1]) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_SCREEN_COORD_FROM_WORLD_COORD, args, 5, r) || !(uint32_t)r[0]) return false;
    sx = floatOf(s[0]);
    sy = floatOf(s[1]);
    return true;
}

bool World::GetFinalRenderedCamCoord(float& x, float& y, float& z) {
    uint64_t r[4] = {};
    if (!CallNative(native::GET_FINAL_RENDERED_CAM_COORD, nullptr, 0, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

int World::HasEntityClearLosToEntity(int32_t from, int32_t to, int traceType) {
    if (!from || !to) return -1;
    uint64_t args[] = { entArg(from), entArg(to), (uint64_t)(uint32_t)traceType };
    uint64_t r[4] = {};
    if (!CallNative(native::HAS_ENTITY_CLEAR_LOS_TO_ENTITY, args, 3, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::SetVehicleSiren(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_SIREN, veh, on);
}

bool World::IsVehicleSirenOn(int32_t veh) {
    return callPedBool(*this, native::IS_VEHICLE_SIREN_ON, veh);
}

bool World::StartVehicleHorn(int32_t veh, int durationMs) {
    if (!veh) return false;
    // START_VEHICLE_HORN(vehicle, duration, mode, forever): mode 0 = default ("Can be 0").
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)durationMs, 0, 0 };
    return CallNative(native::START_VEHICLE_HORN, args, 4, nullptr);
}

bool World::StartVehicleHorn(int32_t veh, int durationMs, uint32_t modeHash) {
    if (!veh) return false;
    // START_VEHICLE_HORN(Vehicle vehicle, int duration, Hash mode, BOOL forever): forever FALSE, so the duration rules.
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)durationMs, (uint64_t)modeHash, 0 };
    return CallNative(native::START_VEHICLE_HORN, args, 4, nullptr);
}

bool World::GetVehicleColours(int32_t veh, int& primary, int& secondary) {
    if (!veh) return false;
    uint64_t p = 0, s = 0;  // int* outs: zeroed 8-byte slots, low 4 bytes read
    uint64_t args[] = { entArg(veh), ptrArg(&p), ptrArg(&s) };
    if (!CallNative(native::GET_VEHICLE_COLOURS, args, 3, nullptr)) return false;
    primary = intOf(p);
    secondary = intOf(s);
    return true;
}

bool World::SetVehicleColours(int32_t veh, int primary, int secondary) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)primary, (uint64_t)(uint32_t)secondary };
    return CallNative(native::SET_VEHICLE_COLOURS, args, 3, nullptr);
}

bool World::SetVehicleDoorsLocked(int32_t veh, int lockState) {
    return callHandleInt(*this, native::SET_VEHICLE_DOORS_LOCKED, veh, lockState);
}

bool World::SetVehicleLights(int32_t veh, int state) {
    return callHandleInt(*this, native::SET_VEHICLE_LIGHTS, veh, state);
}

bool World::SetVehicleForwardSpeed(int32_t veh, float mps) {
    return callHandleFloat(*this, native::SET_VEHICLE_FORWARD_SPEED, veh, mps);
}

bool World::SetVehicleDoorShut(int32_t veh, int door, bool instantly) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door, boolArg(instantly) };
    return CallNative(native::SET_VEHICLE_DOOR_SHUT, args, 3, nullptr);
}

bool World::MarkVehicleOwned(int32_t veh) {
    if (!veh) return false;
    bool ok = callPedToggle(*this, native::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER, veh, true);
    ok = callPedToggle(*this, native::SET_VEHICLE_IS_STOLEN, veh, false) && ok;
    // SET_ENTITY_AS_MISSION_ENTITY(entity, bScriptHostObject, bGrabFromOtherScript)
    uint64_t args[] = { entArg(veh), 1, 1 };
    ok = CallNative(native::SET_ENTITY_AS_MISSION_ENTITY, args, 3, nullptr) && ok;
    return ok;
}

bool World::ShootBulletBetween(float fx, float fy, float fz, float tx, float ty, float tz,
                               int damage, uint32_t weaponHash, int32_t ownerPed, bool audible,
                               bool invisible, float speed) {
    // SHOOT_SINGLE_BULLET_BETWEEN_COORDS(x1, y1, z1, x2, y2, z2, damage, p7, weaponHash, ownerPed,
    //                                    isAudible, isInvisible, speed); p7 true as R* scripts pass.
    uint64_t args[] = { FloatArg(fx), FloatArg(fy), FloatArg(fz), FloatArg(tx), FloatArg(ty), FloatArg(tz),
                        (uint64_t)(uint32_t)damage, 1, weaponHash, entArg(ownerPed), boolArg(audible),
                        boolArg(invisible), FloatArg(speed) };
    return CallNative(native::SHOOT_SINGLE_BULLET_BETWEEN_COORDS, args, 13, nullptr);
}

bool World::PedShootAtCoord(int32_t ped, float x, float y, float z) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), FloatArg(x), FloatArg(y), FloatArg(z), 1 /* toggle */ };
    return CallNative(native::SET_PED_SHOOTS_AT_COORD, args, 5, nullptr);
}

bool World::TaskShootAtCoord(int32_t ped, float x, float y, float z, int durationMs, uint32_t firingPattern) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), FloatArg(x), FloatArg(y), FloatArg(z), (uint64_t)(uint32_t)durationMs,
                        firingPattern ? firingPattern : kFiringPatternFullAuto };
    return CallNative(native::TASK_SHOOT_AT_COORD, args, 6, nullptr);
}

bool World::LastWeaponImpact(int32_t ped, float& x, float& y, float& z) {
    if (!ped) return false;
    // GET_PED_LAST_WEAPON_IMPACT_COORD(ped, Vector3* coords): a Vector3* out, filled by CallNative's
    // vector fix-up (YimMenu invokes it with fix_vectors = true).
    ScrVector v{};
    uint64_t args[] = { entArg(ped), ptrArg(&v) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_LAST_WEAPON_IMPACT_COORD, args, 2, r) || !(uint32_t)r[0]) return false;
    x = v.x; y = v.y; z = v.z;
    return true;
}

bool World::DamagedBy(int32_t entity, int32_t byEntity) {
    if (!entity || !byEntity) return false;
    uint64_t args[] = { entArg(entity), entArg(byEntity), 1 /* p2: "seems to always be 1" */ };
    uint64_t r[4] = {};
    return CallNative(native::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY, args, 3, r) && (uint32_t)r[0];
}

bool World::DamagedByAnyPed(int32_t entity) {
    return callPedBool(*this, native::HAS_ENTITY_BEEN_DAMAGED_BY_ANY_PED, entity);
}

bool World::ClearLastDamage(int32_t entity) {
    if (!entity) return false;
    uint64_t arg[] = { entArg(entity) };
    return CallNative(native::CLEAR_ENTITY_LAST_DAMAGE_ENTITY, arg, 1, nullptr);
}

bool World::LastDamageBone(int32_t ped, int& bone) {
    if (!ped) return false;
    uint64_t slot = 0;  // int* out
    uint64_t args[] = { entArg(ped), ptrArg(&slot) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_LAST_DAMAGE_BONE, args, 2, r) || !(uint32_t)r[0]) return false;
    bone = intOf(slot);
    return true;
}

bool World::ClearLastDamageBone(int32_t ped) {
    if (!ped) return false;
    uint64_t arg[] = { entArg(ped) };
    return CallNative(native::CLEAR_PED_LAST_DAMAGE_BONE, arg, 1, nullptr);
}

int World::GetPedArmour(int32_t ped) {
    if (!ped) return -1;
    uint64_t arg[] = { entArg(ped) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PED_ARMOUR, arg, 1, r)) return -1;
    return intOf(r[0]);
}

bool World::SetPedArmour(int32_t ped, int amount) {
    return callHandleInt(*this, native::SET_PED_ARMOUR, ped, amount);
}

float World::WeaponDamage(uint32_t weaponHash) {
    uint64_t args[] = { weaponHash, 0 /* componentHash: none */ };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_WEAPON_DAMAGE, args, 2, r)) return -1.0f;
    return floatOf(r[0]);
}

int32_t World::GetPedSourceOfDeath(int32_t ped) {
    return callHandleGetHandle(*this, native::GET_PED_SOURCE_OF_DEATH, ped);
}

bool World::IsEntityDead(int32_t entity) {
    if (!entity) return false;
    uint64_t args[] = { entArg(entity), 0 /* p1 */ };
    uint64_t r[4] = {};
    return CallNative(native::IS_ENTITY_DEAD, args, 2, r) && (uint32_t)r[0];
}

bool World::SetEntityCanBeDamaged(int32_t entity, bool can) {
    return callPedToggle(*this, native::SET_ENTITY_CAN_BE_DAMAGED, entity, can);
}

bool World::SetEntityProofs(int32_t entity, bool bullet, bool fire, bool explosion, bool collision,
                            bool melee, bool steam, bool p7, bool drown) {
    if (!entity) return false;
    uint64_t args[] = { entArg(entity), boolArg(bullet), boolArg(fire), boolArg(explosion), boolArg(collision),
                        boolArg(melee), boolArg(steam), boolArg(p7), boolArg(drown) };
    return CallNative(native::SET_ENTITY_PROOFS, args, 9, nullptr);
}

bool World::SetPedSuffersCriticalHits(int32_t ped, bool on) {
    return callPedToggle(*this, native::SET_PED_SUFFERS_CRITICAL_HITS, ped, on);
}

bool World::ApplyDamageToPed(int32_t ped, int damage, bool armorFirst) {
    if (!ped) return false;
    // APPLY_DAMAGE_TO_PED(ped, damageAmount, p2 (armour first), p3, weaponType): p3 and weaponType 0.
    uint64_t args[] = { entArg(ped), (uint64_t)(uint32_t)damage, boolArg(armorFirst), 0, 0 };
    return CallNative(native::APPLY_DAMAGE_TO_PED, args, 5, nullptr);
}

int32_t World::GetCurrentPedWeaponEntity(int32_t ped) {
    if (!ped) return 0;
    uint64_t args[] = { entArg(ped), 0 /* doDeadCheck: no-op in release builds */ };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_CURRENT_PED_WEAPON_ENTITY_INDEX, args, 2, r)) return 0;
    return intOf(r[0]);
}

int World::GetEntityBoneIndexByName(int32_t entity, const char* bone) {
    if (!entity || !bone) return -1;
    uint64_t args[] = { entArg(entity), ptrArg(bone) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_BONE_INDEX_BY_NAME, args, 2, r)) return -1;
    return intOf(r[0]);
}

bool World::GetWorldPositionOfEntityBone(int32_t entity, int boneIndex, float& x, float& y, float& z) {
    if (!entity || boneIndex < 0) return false;
    uint64_t args[] = { entArg(entity), (uint64_t)(uint32_t)boneIndex };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_WORLD_POSITION_OF_ENTITY_BONE, args, 2, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

bool World::GetModelDimensions(uint32_t model, float mn[3], float mx[3]) {
    if (!model || !mn || !mx) return false;
    // GET_MODEL_DIMENSIONS(Hash, Vector3* min, Vector3* max): two Vector3* outs, ScrVectors filled by CallNative's vector fix-up.
    ScrVector a{}, b{};
    uint64_t args[] = { (uint64_t)model, ptrArg(&a), ptrArg(&b) };
    if (!CallNative(native::GET_MODEL_DIMENSIONS, args, 3, nullptr)) return false;
    mn[0] = a.x; mn[1] = a.y; mn[2] = a.z;
    mx[0] = b.x; mx[1] = b.y; mx[2] = b.z;
    return true;
}

bool World::GetOffsetInWorld(int32_t entity, float ox, float oy, float oz, float& x, float& y, float& z) {
    if (!entity) return false;
    uint64_t args[] = { entArg(entity), FloatArg(ox), FloatArg(oy), FloatArg(oz) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS, args, 4, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

bool World::DrawLightWithRange(float x, float y, float z, int r, int g, int b, float range, float intensity) {
    uint64_t args[] = { FloatArg(x), FloatArg(y), FloatArg(z), (uint64_t)(uint32_t)r, (uint64_t)(uint32_t)g, (uint64_t)(uint32_t)b,
                        FloatArg(range), FloatArg(intensity) };
    return CallNative(native::DRAW_LIGHT_WITH_RANGE, args, 8, nullptr);
}

bool World::GetWeaponMuzzlePos(int32_t ped, float& x, float& y, float& z) {
    const int32_t weapon = GetCurrentPedWeaponEntity(ped);
    if (!weapon) return false;
    static const char kMuzzle[] = "gun_muzzle";
    return GetWorldPositionOfEntityBone(weapon, GetEntityBoneIndexByName(weapon, kMuzzle), x, y, z);
}

int World::GetAmmoInPedWeapon(int32_t ped, uint32_t weaponHash) {
    if (!ped) return -1;
    uint64_t args[] = { entArg(ped), weaponHash };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_AMMO_IN_PED_WEAPON, args, 2, r)) return -1;
    return intOf(r[0]);
}

bool World::SetPedInfiniteAmmoClip(int32_t ped, bool on) {
    return callPedToggle(*this, native::SET_PED_INFINITE_AMMO_CLIP, ped, on);
}

bool World::GetGameplayCamCoord(float& x, float& y, float& z) {
    uint64_t r[4] = {};
    if (!CallNative(native::GET_GAMEPLAY_CAM_COORD, nullptr, 0, r)) return false;
    vec3Of(r, x, y, z);
    return true;
}

bool World::GetGameplayCamRot(float& pitch, float& roll, float& yaw) {
    uint64_t arg[] = { (uint64_t)kRotOrder };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_GAMEPLAY_CAM_ROT, arg, 1, r)) return false;
    vec3Of(r, pitch, roll, yaw);
    return true;
}

bool World::IsAimCamActive() {
    uint64_t r[4] = {};
    return CallNative(native::IS_AIM_CAM_ACTIVE, nullptr, 0, r) && (uint32_t)r[0];
}

bool World::GetEntityLocalPlayerIsFreeAimingAt(int32_t& entity) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t slot = 0;  // Entity* out
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], ptrArg(&slot) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT, args, 2, r) || !(uint32_t)r[0]) return false;
    entity = intOf(slot);
    return entity != 0;
}

bool World::RaycastFromCamera(float maxDist, int32_t ignoreEntity, bool& hit,
                              float& x, float& y, float& z, int32_t& hitEntity) {
    float cx, cy, cz, pitch, roll, yaw;
    if (!GetGameplayCamCoord(cx, cy, cz) || !GetGameplayCamRot(pitch, roll, yaw)) return false;
    // View direction from the camera's pitch (x) and yaw (z), degrees, GTA's +Y-forward frame.
    const float kDeg = 3.14159265358979f / 180.0f;
    const float cp = cosf(pitch * kDeg);
    const float dx = -sinf(yaw * kDeg) * cp, dy = cosf(yaw * kDeg) * cp, dz = sinf(pitch * kDeg);
    const float ex = cx + dx * maxDist, ey = cy + dy * maxDist, ez = cz + dz * maxDist;
    // START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(x1, y1, z1, x2, y2, z2, flags, entity, p8):
    // flags -1 = every collision type; p8 7 = ignore glass | see-through | no-collision.
    uint64_t pargs[] = { FloatArg(cx), FloatArg(cy), FloatArg(cz), FloatArg(ex), FloatArg(ey), FloatArg(ez),
                         (uint64_t)(uint32_t)-1, entArg(ignoreEntity), 7 };
    uint64_t pr[4] = {};
    if (!CallNative(native::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE, pargs, 9, pr)) return false;
    const int32_t probe = intOf(pr[0]);
    if (!probe) return false;
    // GET_SHAPE_TEST_RESULT(handle, BOOL* hit, Vector3* endCoords, Vector3* surfaceNormal,
    // Entity* entityHit) -> 0 invalid, 1 pending, 2 ready. BOOL*/Entity* are zeroed 8-byte slots
    // (low 4 bytes read); the Vector3* are ScrVectors filled by CallNative's vector fix-up.
    uint64_t hitSlot = 0, entSlot = 0;
    ScrVector end{}, normal{};
    uint64_t rargs[] = { (uint64_t)(uint32_t)probe, ptrArg(&hitSlot), ptrArg(&end), ptrArg(&normal),
                         ptrArg(&entSlot) };
    uint64_t rr[4] = {};
    if (!CallNative(native::GET_SHAPE_TEST_RESULT, rargs, 5, rr) || intOf(rr[0]) != 2) return false;
    hit = (uint32_t)hitSlot != 0;
    if (hit) {
        x = end.x; y = end.y; z = end.z;
        hitEntity = intOf(entSlot);
    } else {
        x = ex; y = ey; z = ez;
        hitEntity = 0;
    }
    return true;
}

// ---- protocol v4: emotes, health, entity checks -------------------------------------------
// String arguments are passed as pointers to NUL-terminated text that outlives the call (the
// native copies or hashes it before returning).

bool World::RequestAnimDict(const char* dict) {
    if (!dict || !*dict) return false;
    uint64_t arg[] = { ptrArg(dict) };
    return CallNative(native::REQUEST_ANIM_DICT, arg, 1, nullptr);
}

bool World::HasAnimDictLoaded(const char* dict) {
    if (!dict || !*dict) return false;
    uint64_t arg[] = { ptrArg(dict) };
    uint64_t r[4] = {};
    return CallNative(native::HAS_ANIM_DICT_LOADED, arg, 1, r) && (uint32_t)r[0];
}

bool World::RemoveAnimDict(const char* dict) {
    if (!dict || !*dict) return false;
    uint64_t arg[] = { ptrArg(dict) };
    return CallNative(native::REMOVE_ANIM_DICT, arg, 1, nullptr);
}

int World::AnimDictExists(const char* dict) {
    if (!dict || !*dict) return 0;
    uint64_t arg[] = { ptrArg(dict) };
    uint64_t r[4] = {};
    if (!CallNative(native::DOES_ANIM_DICT_EXIST, arg, 1, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::TaskPlayAnim(int32_t ped, const char* dict, const char* name, float blendIn, float blendOut,
                         int durationMs, int flags, float playbackRate) {
    if (!ped || !dict || !name) return false;
    // TASK_PLAY_ANIM(ped, dict, name, blendIn, blendOut, duration, flag, playbackRate, lockX, lockY, lockZ)
    uint64_t args[] = { entArg(ped), ptrArg(dict), ptrArg(name), FloatArg(blendIn), FloatArg(blendOut),
                        (uint64_t)(uint32_t)durationMs, (uint64_t)(uint32_t)flags, FloatArg(playbackRate),
                        0, 0, 0 };
    return CallNative(native::TASK_PLAY_ANIM, args, 11, nullptr);
}

bool World::StopAnimTask(int32_t entity, const char* dict, const char* name, float blendDelta) {
    if (!entity || !dict || !name) return false;
    // STOP_ANIM_TASK(entity, animDictionary, animationName, blendDelta)
    uint64_t args[] = { entArg(entity), ptrArg(dict), ptrArg(name), FloatArg(blendDelta) };
    return CallNative(native::STOP_ANIM_TASK, args, 4, nullptr);
}

bool World::TaskStartScenarioInPlace(int32_t ped, const char* scenario) {
    if (!ped || !scenario || !*scenario) return false;
    uint64_t args[] = { entArg(ped), ptrArg(scenario), 0 /* unkDelay */, 1 /* playEnterAnim */ };
    return CallNative(native::TASK_START_SCENARIO_IN_PLACE, args, 4, nullptr);
}

int World::IsEntityPlayingAnim(int32_t entity, const char* dict, const char* name) {
    if (!entity || !dict || !name) return 0;
    uint64_t args[] = { entArg(entity), ptrArg(dict), ptrArg(name), 3 /* taskFlag: any */ };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_ENTITY_PLAYING_ANIM, args, 4, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::GetEntityAnimTimes(int32_t entity, const char* dict, const char* name, float& current, float& total, float* phase) {
    current = -1.0f;
    total = -1.0f;
    if (phase) *phase = -1.0f;
    if (!entity || !dict || !name) return false;
    uint64_t args[] = { entArg(entity), ptrArg(dict), ptrArg(name) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_ANIM_TOTAL_TIME, args, 3, r)) return false;
    float totalMs = 0.0f;
    memcpy(&totalMs, &r[0], 4);                          // MILLISECONDS (alloc8or doc)
    if (!(totalMs > 0.0f) || !std::isfinite(totalMs)) return false;   // not on the entity
    uint64_t r2[4] = {};
    if (!CallNative(native::GET_ENTITY_ANIM_CURRENT_TIME, args, 3, r2)) return false;
    float ph = 0.0f;
    memcpy(&ph, &r2[0], 4);                              // PHASE 0..1
    if (!std::isfinite(ph)) return false;
    if (phase) *phase = ph;
    total = totalMs / 1000.0f;
    current = ph * total;
    return true;
}

bool World::GetAnimDuration(const char* dict, const char* name, float& raw) {
    raw = -1.0f;
    if (!dict || !name) return false;
    uint64_t args[] = { ptrArg(dict), ptrArg(name) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ANIM_DURATION, args, 2, r)) return false;
    memcpy(&raw, &r[0], 4);
    return std::isfinite(raw);
}

int World::GetScriptTaskStatus(int32_t ped, uint32_t taskHash) {
    if (!ped) return -1;
    uint64_t args[] = { entArg(ped), (uint64_t)taskHash };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_SCRIPT_TASK_STATUS, args, 2, r)) return -1;
    return (int)(int32_t)(uint32_t)r[0];
}

int World::IsPedUsingAnyScenario(int32_t ped) {
    if (!ped) return 0;
    uint64_t arg[] = { entArg(ped) };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_PED_USING_ANY_SCENARIO, arg, 1, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::SetEntityMaxHealth(int32_t entity, int value) {
    return callHandleInt(*this, native::SET_ENTITY_MAX_HEALTH, entity, value);
}

bool World::SetPedMaxHealth(int32_t ped, int value) {
    return callHandleInt(*this, native::SET_PED_MAX_HEALTH, ped, value);
}

bool World::SetPlayerMaxArmour(int value) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], (uint64_t)(uint32_t)value };
    return CallNative(native::SET_PLAYER_MAX_ARMOUR, args, 2, nullptr);
}

int World::EntityExists(int32_t entity) {
    if (!entity) return 0;
    uint64_t arg[] = { entArg(entity) };
    uint64_t r[4] = {};
    if (!CallNative(native::DOES_ENTITY_EXIST, arg, 1, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::IsHornActive(int32_t veh) {
    return callPedBool(*this, native::IS_HORN_ACTIVE, veh);
}

bool World::IsHornActive(int32_t veh, bool& active) {
    if (!veh) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)veh };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_HORN_ACTIVE, arg, 1, r)) return false;
    active = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetLocalHealthRecharge(float multiplier) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], FloatArg(multiplier) };
    return CallNative(native::SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER, args, 2, nullptr);
}

// ---- the no-pause pause menu ----------------------------------------------------------------

bool World::ActivateFrontendMenu(uint32_t menuHash, bool togglePause, int component) {
    // ACTIVATE_FRONTEND_MENU(Hash menuhash, BOOL togglePause, int component)
    uint64_t args[] = { (uint64_t)menuHash, boolArg(togglePause), (uint64_t)(uint32_t)component };
    return CallNative(native::ACTIVATE_FRONTEND_MENU, args, 3, nullptr);
}

bool World::IsPauseMenuActive() {
    uint64_t r[4] = {};
    return CallNative(native::IS_PAUSE_MENU_ACTIVE, nullptr, 0, r) && (uint32_t)r[0];
}

bool World::IsDisabledControlJustPressed(int pad, int control) {
    uint64_t args[] = { (uint64_t)(uint32_t)pad, (uint64_t)(uint32_t)control };
    uint64_t r[4] = {};
    return CallNative(native::IS_DISABLED_CONTROL_JUST_PRESSED, args, 2, r) && (uint32_t)r[0];
}

bool World::DisableFrontendThisFrame() {
    return CallNative(native::DISABLE_FRONTEND_THIS_FRAME, nullptr, 0, nullptr);
}

bool World::SetPauseMenuActive(bool on) {
    uint64_t arg[] = { boolArg(on) };
    return CallNative(native::SET_PAUSE_MENU_ACTIVE, arg, 1, nullptr);
}

int World::GetGameTimer() {
    uint64_t r[4] = {};
    if (!CallNative(native::GET_GAME_TIMER, nullptr, 0, r)) return -1;
    return intOf(r[0]);
}

uint32_t World::GetWeaponTypeGroup(uint32_t weaponHash) {
    uint64_t arg[] = { (uint64_t)weaponHash };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_WEAPONTYPE_GROUP, arg, 1, r)) return 0;
    return (uint32_t)r[0];
}

// ---- sync plan W0: on-foot, vehicle body, drive-by, particles ---------------------------------
// Argument lists are the ones in docs/SYNC-PLAN.md (alloc8or natives.json). CallNative zero-fills its
// argument buffer, so a 3889 parameter beyond the listed ones reads 0.

bool World::IsPedReloading(int32_t ped) {
    return callPedBool(*this, native::IS_PED_RELOADING, ped);
}

bool World::TaskReloadWeapon(int32_t ped, bool drawWeapon) {
    return callPedToggle(*this, native::TASK_RELOAD_WEAPON, ped, drawWeapon);
}

bool World::GetPedStealthMovement(int32_t ped) {
    return callPedBool(*this, native::GET_PED_STEALTH_MOVEMENT, ped);
}

bool World::SetPedStealthMovement(int32_t ped, bool p1, const char* action) {
    if (!ped) return false;
    // SET_PED_STEALTH_MOVEMENT(ped, p1, action): action may be 0 (alloc8or), so null passes through.
    uint64_t args[] = { entArg(ped), boolArg(p1), ptrArg(action) };
    return CallNative(native::SET_PED_STEALTH_MOVEMENT, args, 3, nullptr);
}

bool World::IsPedSwimming(int32_t ped) {
    return callPedBool(*this, native::IS_PED_SWIMMING, ped);
}

bool World::IsPedInCover(int32_t ped, bool exceptUseWeapon) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), boolArg(exceptUseWeapon) };
    uint64_t r[4] = {};
    return CallNative(native::IS_PED_IN_COVER, args, 2, r) && (uint32_t)r[0];
}

bool World::TaskPutPedDirectlyIntoCover(int32_t ped, float x, float y, float z, int time,
                                        bool allowPeekingAndFiring, float blendInDuration,
                                        bool forceInitialFacingDirection, bool forceFaceLeft,
                                        int identifier, bool doEntry) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), FloatArg(x), FloatArg(y), FloatArg(z), (uint64_t)(uint32_t)time,
                        boolArg(allowPeekingAndFiring), FloatArg(blendInDuration),
                        boolArg(forceInitialFacingDirection), boolArg(forceFaceLeft),
                        (uint64_t)(uint32_t)identifier, boolArg(doEntry) };
    return CallNative(native::TASK_PUT_PED_DIRECTLY_INTO_COVER, args, 11, nullptr);
}

// R3 T3 (cover).
bool World::IsPedGoingIntoCover(int32_t ped) {
    return callPedBool(*this, native::IS_PED_GOING_INTO_COVER, ped);
}

bool World::IsPedAimingFromCover(int32_t ped) {
    return callPedBool(*this, native::IS_PED_AIMING_FROM_COVER, ped);
}

bool World::TaskStayInCover(int32_t ped) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped) };
    return CallNative(native::TASK_STAY_IN_COVER, args, 1, nullptr);
}

bool World::IsPedPerformingMeleeAction(int32_t ped) {
    return callPedBool(*this, native::IS_PED_PERFORMING_MELEE_ACTION, ped);
}

int World::IsPedInMeleeCombat(int32_t ped) {
    if (!ped) return 0;
    uint64_t arg[] = { (uint64_t)(uint32_t)ped };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_PED_IN_MELEE_COMBAT, arg, 1, r)) return -1;
    return (uint32_t)r[0] ? 1 : 0;
}

bool World::TaskPutPedDirectlyIntoMelee(int32_t ped, int32_t target, float blendIn, float timeInMelee, float strafePhaseSync, int aiCombatFlags) {
    if (!ped || !target) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)target, FloatArg(blendIn), FloatArg(timeInMelee),
                        FloatArg(strafePhaseSync), (uint64_t)(uint32_t)aiCombatFlags };
    return CallNative(native::TASK_PUT_PED_DIRECTLY_INTO_MELEE, args, 6, nullptr);
}

bool World::SetPedMoveRateOverride(int32_t ped, float value) {
    return callHandleFloat(*this, native::SET_PED_MOVE_RATE_OVERRIDE, ped, value);
}

bool World::IsPedVaulting(int32_t ped) { return callPedBool(*this, native::IS_PED_VAULTING, ped); }

bool World::TaskClimb(int32_t ped, bool usePlayerLaunchForce) {
    return callPedToggle(*this, native::TASK_CLIMB, ped, usePlayerLaunchForce);   // TASK_CLIMB(ped, BOOL)
}

bool World::TaskGoToEntity(int32_t ped, int32_t target, int durationMs, float distance, float moveBlendRatio,
                           float slowDownDistance, int flags) {
    if (!ped || !target) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)ped, (uint64_t)(uint32_t)target, (uint64_t)(uint32_t)durationMs,
                        FloatArg(distance), FloatArg(moveBlendRatio), FloatArg(slowDownDistance), (uint64_t)(uint32_t)flags };
    return CallNative(native::TASK_GO_TO_ENTITY, args, 7, nullptr);
}

bool World::SetAiWeaponDamageModifier(float value) {
    uint64_t args[] = { FloatArg(value) };
    return CallNative(native::SET_AI_WEAPON_DAMAGE_MODIFIER, args, 1, nullptr);
}

bool World::SetAiMeleeWeaponDamageModifier(float value) {
    uint64_t args[] = { FloatArg(value) };
    return CallNative(native::SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER, args, 1, nullptr);
}

bool World::ResetAiWeaponDamageModifier() {
    return CallNative(native::RESET_AI_WEAPON_DAMAGE_MODIFIER, nullptr, 0, nullptr);
}

bool World::ResetAiMeleeWeaponDamageModifier() {
    return CallNative(native::RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER, nullptr, 0, nullptr);
}

bool World::WasPedKilledByTakedown(int32_t ped) { return callPedBool(*this, native::WAS_PED_KILLED_BY_TAKEDOWN, ped); }

bool World::ModifyVehicleTopSpeed(int32_t veh, float percent) {
    if (!veh) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)veh, FloatArg(percent) };
    return CallNative(native::MODIFY_VEHICLE_TOP_SPEED, args, 2, nullptr);
}

bool World::SetVehicleCheatPowerIncrease(int32_t veh, float value) {
    if (!veh) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)veh, FloatArg(value) };
    return CallNative(native::SET_VEHICLE_CHEAT_POWER_INCREASE, args, 2, nullptr);
}

bool World::SetVehicleMaxSpeed(int32_t veh, float metresPerSecond) {
    if (!veh) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)veh, FloatArg(metresPerSecond) };
    return CallNative(native::SET_VEHICLE_MAX_SPEED, args, 2, nullptr);
}

float World::GetVehicleEstimatedMaxSpeed(int32_t veh) {
    if (!veh) return 0.0f;
    uint64_t args[] = { (uint64_t)(uint32_t)veh };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_ESTIMATED_MAX_SPEED, args, 1, r)) return 0.0f;
    float f = 0.0f;
    memcpy(&f, &r[0], 4);  // native returns a float in the low 4 bytes
    return f;
}

// 0.4.19 radio off (libs/sync/radio_off.h). Station / flag strings are passed by pointer; callers pass literals.
bool World::SetUserRadioControlEnabled(bool on) {
    uint64_t args[] = { on ? 1ull : 0ull };
    return CallNative(native::SET_USER_RADIO_CONTROL_ENABLED, args, 1, nullptr);
}

bool World::SetVehicleRadioEnabled(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_RADIO_ENABLED, veh, on);
}

bool World::SetVehRadioStation(int32_t veh, const char* station) {
    if (!veh || !station) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)veh, ptrArg(station) };
    return CallNative(native::SET_VEH_RADIO_STATION, args, 2, nullptr);
}

bool World::SetRadioToStationName(const char* station) {
    if (!station) return false;
    uint64_t args[] = { ptrArg(station) };
    return CallNative(native::SET_RADIO_TO_STATION_NAME, args, 1, nullptr);
}

bool World::SetMobileRadioEnabledDuringGameplay(bool on) {
    uint64_t args[] = { on ? 1ull : 0ull };
    return CallNative(native::SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY, args, 1, nullptr);
}

bool World::SetAudioFlag(const char* flag, bool on) {
    if (!flag) return false;
    uint64_t args[] = { ptrArg(flag), on ? 1ull : 0ull };
    return CallNative(native::SET_AUDIO_FLAG, args, 2, nullptr);
}

bool World::GetPlayerRadioStationIndex(int& index) {
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PLAYER_RADIO_STATION_INDEX, nullptr, 0, r)) return false;
    index = (int)(int32_t)(uint32_t)r[0];
    return true;
}

bool World::IsVehicleRadioOn(int32_t veh, bool& on) {
    if (!veh) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)veh };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_VEHICLE_RADIO_ON, args, 1, r)) return false;
    on = (uint32_t)r[0] != 0;
    return true;
}

bool World::IsHudComponentActive(int id, bool& active) {
    uint64_t args[] = { (uint64_t)(uint32_t)id };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_HUD_COMPONENT_ACTIVE, args, 1, r)) return false;
    active = (uint32_t)r[0] != 0;
    return true;
}

bool World::IsDisabledControlPressed(int pad, int control, bool& pressed) {
    uint64_t args[] = { (uint64_t)(uint32_t)pad, (uint64_t)(uint32_t)control };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_DISABLED_CONTROL_PRESSED, args, 2, r)) return false;
    pressed = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetEntityHasGravity(int32_t handle, bool on) {
    if (!handle) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)handle, on ? 1ull : 0ull };
    return CallNative(native::SET_ENTITY_HAS_GRAVITY, args, 2, nullptr);
}

bool World::GetEntityCollisionDisabled(int32_t handle, bool& disabled) {
    if (!handle) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_ENTITY_COLLISION_DISABLED, arg, 1, r)) return false;
    disabled = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetPlayerInvincible(bool on) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0], on ? 1ull : 0ull };
    return CallNative(native::SET_PLAYER_INVINCIBLE, args, 2, nullptr);
}

bool World::GetPlayerInvincible(bool& on) {
    uint64_t player[4] = {};
    if (!CallNative(native::PLAYER_ID, nullptr, 0, player)) return false;
    uint64_t args[] = { (uint64_t)(uint32_t)player[0] };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_PLAYER_INVINCIBLE, args, 1, r)) return false;
    on = (uint32_t)r[0] != 0;
    return true;
}

bool World::GetTimecycleModifierIndex(int& index) {
    uint64_t r[4] = {};
    if (!CallNative(native::GET_TIMECYCLE_MODIFIER_INDEX, nullptr, 0, r)) return false;
    index = (int)(int32_t)(uint32_t)r[0];
    return true;
}

bool World::ClearTimecycleModifier() {
    return CallNative(native::CLEAR_TIMECYCLE_MODIFIER, nullptr, 0, nullptr);
}

bool World::StopAllPostFx() {
    return CallNative(native::ANIMPOSTFX_STOP_ALL, nullptr, 0, nullptr);
}

// Hyper tune handling (the Dubsta, libs/sync/hypertune.h): four (vehicle, BOOL) setters and one BOOL getter.
bool World::SetVehicleStrong(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_STRONG, veh, on);
}

bool World::SetVehicleHasStrongAxles(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_HAS_STRONG_AXLES, veh, on);
}

bool World::SetVehicleTyresCanBurst(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_TYRES_CAN_BURST, veh, on);
}

bool World::SetVehicleWheelsCanBreak(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_WHEELS_CAN_BREAK, veh, on);
}

bool World::IsEntityInAir(int32_t handle, bool& inAir) {
    if (!handle) return false;
    uint64_t arg[] = { (uint64_t)(uint32_t)handle };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_ENTITY_IN_AIR, arg, 1, r)) return false;   // false here is "could not call", not "on the ground"
    inAir = (uint32_t)r[0] != 0;
    return true;
}

bool World::GetVehicleLightsState(int32_t veh, bool& lightsOn, bool& highbeamsOn) {
    if (!veh) return false;
    // GET_VEHICLE_LIGHTS_STATE(vehicle, BOOL* lightsOn, BOOL* highbeamsOn): zeroed 8-byte slots, low
    // 4 bytes read. The native's own BOOL return is deliberately not consulted.
    uint64_t on = 0, high = 0;
    uint64_t args[] = { entArg(veh), ptrArg(&on), ptrArg(&high) };
    if (!CallNative(native::GET_VEHICLE_LIGHTS_STATE, args, 3, nullptr)) return false;
    lightsOn = (uint32_t)on != 0;
    highbeamsOn = (uint32_t)high != 0;
    return true;
}

bool World::SetVehicleFullbeam(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_FULLBEAM, veh, on);
}

bool World::SetVehicleInteriorLight(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_INTERIORLIGHT, veh, on);
}

bool World::SetVehicleIndicatorLights(int32_t veh, int turnSignal, bool on) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)turnSignal, boolArg(on) };
    return CallNative(native::SET_VEHICLE_INDICATOR_LIGHTS, args, 3, nullptr);
}

bool World::GetVehicleDoorAngleRatio(int32_t veh, int door, float& ratio) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_DOOR_ANGLE_RATIO, args, 2, r)) return false;
    ratio = floatOf(r[0]);
    return true;
}

bool World::SetVehicleDoorOpen(int32_t veh, int door, bool loose, bool instantly) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door, boolArg(loose), boolArg(instantly) };
    return CallNative(native::SET_VEHICLE_DOOR_OPEN, args, 4, nullptr);
}

// ---- T3 vehicle visual state (protocol v6). A native whose hash constant is still 0 (UNRESOLVED: lookup needed) is never
// called: the wrapper logs once and returns false, which every caller reads as "no information" / "not done".
#define GAGE_T3_NEEDS_HASH(NAME)                                                                                          \
    if (!native::NAME) {                                                                                                  \
        static bool s_logged = false;                                                                                     \
        if (!s_logged) {                                                                                                  \
            s_logged = true;                                                                                              \
            GLOGI("hook: " #NAME " has no hash yet (UNRESOLVED: lookup needed): the vehicle visual state that needs it stays off"); \
        }                                                                                                                 \
        return false;                                                                                                     \
    }

bool World::IsVehicleTyreBurst(int32_t veh, int wheel, bool completely, bool& burst) {
    GAGE_T3_NEEDS_HASH(IS_VEHICLE_TYRE_BURST)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)wheel, boolArg(completely) };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_VEHICLE_TYRE_BURST, args, 3, r)) return false;
    burst = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetVehicleTyreBurst(int32_t veh, int wheel, bool onRim, float damage) {
    GAGE_T3_NEEDS_HASH(SET_VEHICLE_TYRE_BURST)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)wheel, boolArg(onRim), FloatArg(damage) };
    return CallNative(native::SET_VEHICLE_TYRE_BURST, args, 4, nullptr);
}

bool World::SetVehicleTyreFixed(int32_t veh, int wheel) {
    GAGE_T3_NEEDS_HASH(SET_VEHICLE_TYRE_FIXED)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)wheel };
    return CallNative(native::SET_VEHICLE_TYRE_FIXED, args, 2, nullptr);
}

bool World::IsVehicleWindowIntact(int32_t veh, int window, bool& intact) {
    GAGE_T3_NEEDS_HASH(IS_VEHICLE_WINDOW_INTACT)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)window };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_VEHICLE_WINDOW_INTACT, args, 2, r)) return false;
    intact = (uint32_t)r[0] != 0;
    return true;
}

bool World::SmashVehicleWindow(int32_t veh, int window) {
    GAGE_T3_NEEDS_HASH(SMASH_VEHICLE_WINDOW)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)window };
    return CallNative(native::SMASH_VEHICLE_WINDOW, args, 2, nullptr);
}

bool World::FixVehicleWindow(int32_t veh, int window) {
    GAGE_T3_NEEDS_HASH(FIX_VEHICLE_WINDOW)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)window };
    return CallNative(native::FIX_VEHICLE_WINDOW, args, 2, nullptr);
}

bool World::GetControlNormal(int inputGroup, int control, float& value) {
    GAGE_T3_NEEDS_HASH(GET_CONTROL_NORMAL)
    uint64_t args[] = { (uint64_t)(uint32_t)inputGroup, (uint64_t)(uint32_t)control };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_CONTROL_NORMAL, args, 2, r)) return false;
    value = floatOf(r[0]);
    return true;
}

bool World::SetVehicleHandbrake(int32_t veh, bool on) {
    GAGE_T3_NEEDS_HASH(SET_VEHICLE_HANDBRAKE)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), boolArg(on) };
    return CallNative(native::SET_VEHICLE_HANDBRAKE, args, 2, nullptr);
}

bool World::SetVehicleBrakeLights(int32_t veh, bool on) {
    GAGE_T3_NEEDS_HASH(SET_VEHICLE_BRAKE_LIGHTS)
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), boolArg(on) };
    return CallNative(native::SET_VEHICLE_BRAKE_LIGHTS, args, 2, nullptr);
}
#undef GAGE_T3_NEEDS_HASH

// ---- R3 T4 vehicle damage state (protocol v7). All hashes are resolved ones (hook.h, "Desync round 3"); a call that cannot be made
// returns false, which every caller reads as "no information" (a read) or "not done" (a write).
bool World::GetVehiclePetrolTankHealth(int32_t veh, float& health) {
    return callHandleGetFloat(*this, native::GET_VEHICLE_PETROL_TANK_HEALTH, veh, health);
}

bool World::SetVehiclePetrolTankHealth(int32_t veh, float health) {
    return callHandleFloat(*this, native::SET_VEHICLE_PETROL_TANK_HEALTH, veh, health);
}

bool World::SetVehicleDamage(int32_t veh, float x, float y, float z, float damage, float radius, bool focusOnModel) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), FloatArg(x), FloatArg(y), FloatArg(z), FloatArg(damage), FloatArg(radius), boolArg(focusOnModel) };
    return CallNative(native::SET_VEHICLE_DAMAGE, args, 7, nullptr);
}

bool World::GetVehicleDeformationAtPos(int32_t veh, float x, float y, float z, float& dx, float& dy, float& dz) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), FloatArg(x), FloatArg(y), FloatArg(z) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_VEHICLE_DEFORMATION_AT_POS, args, 4, r)) return false;
    memcpy(&dx, &r[0], 4);       // a Vector3 return: three padded slots, as GET_ENTITY_COORDS
    memcpy(&dy, &r[1], 4);
    memcpy(&dz, &r[2], 4);
    return true;
}

bool World::IsVehicleDoorDamaged(int32_t veh, int door, bool& damaged) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door };
    uint64_t r[4] = {};
    if (!CallNative(native::IS_VEHICLE_DOOR_DAMAGED, args, 2, r)) return false;
    damaged = (uint32_t)r[0] != 0;
    return true;
}

bool World::GetIsDoorValid(int32_t veh, int door, bool& valid) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_IS_DOOR_VALID, args, 2, r)) return false;
    valid = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetVehicleDoorBroken(int32_t veh, int door, bool deleteDoor) {
    if (!veh) return false;
    uint64_t args[] = { entArg(veh), (uint64_t)(uint32_t)door, boolArg(deleteDoor) };
    return CallNative(native::SET_VEHICLE_DOOR_BROKEN, args, 3, nullptr);
}

bool World::IsVehicleHeadlightDamaged(int32_t veh, bool left, bool& damaged) {
    if (!veh) return false;
    uint64_t arg[] = { entArg(veh) };
    uint64_t r[4] = {};
    if (!CallNative(left ? native::GET_IS_LEFT_VEHICLE_HEADLIGHT_DAMAGED : native::GET_IS_RIGHT_VEHICLE_HEADLIGHT_DAMAGED, arg, 1, r)) return false;
    damaged = (uint32_t)r[0] != 0;
    return true;
}

bool World::SetVehicleCanBeVisiblyDamaged(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED, veh, on);
}

bool World::SetVehicleEngineCanDegrade(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_VEHICLE_ENGINE_CAN_DEGRADE, veh, on);
}

bool World::SetDisableVehiclePetrolTankDamage(int32_t veh, bool on) {
    return callPedToggle(*this, native::SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE, veh, on);
}

bool World::GetConvertibleRoofState(int32_t veh, int& state) {
    if (!veh) return false;
    uint64_t arg[] = { entArg(veh) };
    uint64_t r[4] = {};
    if (!CallNative(native::GET_CONVERTIBLE_ROOF_STATE, arg, 1, r)) return false;
    state = intOf(r[0]);
    return true;
}

bool World::RaiseConvertibleRoof(int32_t veh, bool instantly) {
    return callPedToggle(*this, native::RAISE_CONVERTIBLE_ROOF, veh, instantly);
}

bool World::LowerConvertibleRoof(int32_t veh, bool instantly) {
    return callPedToggle(*this, native::LOWER_CONVERTIBLE_ROOF, veh, instantly);
}

bool World::TaskDriveBy(int32_t driverPed, int32_t targetPed, int32_t targetVehicle,
                        float x, float y, float z, float distanceToShoot, int pedAccuracy,
                        bool pushUnderneathDrivingTaskIfDriving, uint32_t firingPattern) {
    if (!driverPed) return false;  // targetPed / targetVehicle may be 0 (an unused target)
    uint64_t args[] = { entArg(driverPed), entArg(targetPed), entArg(targetVehicle), FloatArg(x),
                        FloatArg(y), FloatArg(z), FloatArg(distanceToShoot), (uint64_t)(uint32_t)pedAccuracy,
                        boolArg(pushUnderneathDrivingTaskIfDriving),
                        (uint64_t)(firingPattern ? firingPattern : kFiringPatternFullAuto) };  // 0 = full auto, as TaskShootAtCoord
    return CallNative(native::TASK_DRIVE_BY, args, 10, nullptr);
}

bool World::SetDrivebyTaskTarget(int32_t shootingPed, int32_t targetPed, int32_t targetVehicle,
                                 float x, float y, float z) {
    if (!shootingPed) return false;
    uint64_t args[] = { entArg(shootingPed), entArg(targetPed), entArg(targetVehicle), FloatArg(x),
                        FloatArg(y), FloatArg(z) };
    return CallNative(native::SET_DRIVEBY_TASK_TARGET, args, 6, nullptr);
}

bool World::TaskVehicleAimAtCoord(int32_t ped, float x, float y, float z) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), FloatArg(x), FloatArg(y), FloatArg(z) };
    return CallNative(native::TASK_VEHICLE_AIM_AT_COORD, args, 4, nullptr);
}

bool World::ClearDrivebyTaskUnderneathDrivingTask(int32_t ped) {
    if (!ped) return false;
    uint64_t arg[] = { entArg(ped) };
    return CallNative(native::CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK, arg, 1, nullptr);
}

bool World::GetIsTaskActive(int32_t ped, int taskType) {
    if (!ped) return false;
    uint64_t args[] = { entArg(ped), (uint64_t)(uint32_t)taskType };
    uint64_t r[4] = {};
    return CallNative(native::GET_IS_TASK_ACTIVE, args, 2, r) && (uint32_t)r[0];
}

bool World::RequestNamedPtfxAsset(const char* fxName) {
    if (!fxName || !*fxName) return false;
    uint64_t arg[] = { ptrArg(fxName) };
    return CallNative(native::REQUEST_NAMED_PTFX_ASSET, arg, 1, nullptr);
}

bool World::HasNamedPtfxAssetLoaded(const char* fxName) {
    if (!fxName || !*fxName) return false;
    uint64_t arg[] = { ptrArg(fxName) };
    uint64_t r[4] = {};
    return CallNative(native::HAS_NAMED_PTFX_ASSET_LOADED, arg, 1, r) && (uint32_t)r[0];
}

bool World::UseParticleFxAsset(const char* name) {
    if (!name || !*name) return false;
    uint64_t arg[] = { ptrArg(name) };
    return CallNative(native::USE_PARTICLE_FX_ASSET, arg, 1, nullptr);
}

bool World::StartParticleFxNonLoopedOnEntity(const char* effectName, int32_t entity,
                                             float offX, float offY, float offZ,
                                             float rotX, float rotY, float rotZ, float scale,
                                             bool axisX, bool axisY, bool axisZ) {
    if (!effectName || !*effectName || !entity) return false;
    uint64_t args[] = { ptrArg(effectName), entArg(entity), FloatArg(offX), FloatArg(offY), FloatArg(offZ),
                        FloatArg(rotX), FloatArg(rotY), FloatArg(rotZ), FloatArg(scale),
                        boolArg(axisX), boolArg(axisY), boolArg(axisZ) };
    uint64_t r[4] = {};
    return CallNative(native::START_PARTICLE_FX_NON_LOOPED_ON_ENTITY, args, 12, r) && (uint32_t)r[0];
}

// ---------------------------------------------------------------------------
// Init: resolve native table and D3D11 Present
// ---------------------------------------------------------------------------
void gage::game::Init(World& w) {
    w.module = GetModuleHandleW(nullptr);
    if (!w.module) { w.err = "GetModuleHandleW(nullptr)"; return; }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)w.module;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)w.module + dos->e_lfanew);
    w.imageSize = nt->OptionalHeader.SizeOfImage;
    const uint8_t* base = (const uint8_t*)w.module;
    const size_t size = (size_t)w.imageSize;

    // Store for lazy retry from Frame() if the table is not populated yet.
    g_imageBase = base;
    g_imageSize = size;
    g_nativeRetries = 0;

    // ---- Build profile -------------------------------------------------------
    // Pick the per-build signature/offset set by the main module's file name.
    {
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW((HMODULE)w.module, modPath, MAX_PATH);
        const wchar_t* baseName = modPath;
        for (const wchar_t* s = modPath; *s; ++s)
            if (*s == L'\\' || *s == L'/') baseName = s + 1;
        const BuildProfile* prof = selectBuildProfile(baseName);
        g_buildMatched = prof != nullptr;
        if (prof) {
            g_build = prof;
        } else {
            g_build = &kProfile3889;
            GLOGW("hook: unrecognised game module '%ls'; assuming %s", baseName, kProfile3889.label);
        }
        if (g_build->known)
            GLOGI("hook: build = %s", g_build->label);
        else
            GLOGW("hook: build = %s: native/script/overlay signatures not derived yet; launch+join "
                  "work, gameplay+HUD stay inert until a runtime dump is captured", g_build->label);
        // Automatic on an unported build (the engine is inert there anyway);
        // enh_dump=0 disables, enh_dump=1 forces even a known build.
        g_enhDump = (w.enhDump == 1) || (w.enhDump == -1 && !g_build->known);
        if (g_enhDump)
            GLOGW("hook: Enhanced dump mode ON: the decrypted game image is written to "
                  "%%LOCALAPPDATA%%\\GAGE-MP\\dump once the code decrypts (enh_dump)");
        // pack_enh: the read-only car-pack diagnostic (pack_enh.h). Armed here, stepped by pollCodePatches; with
        // pack_enh=0 (default) nothing below runs and g_packEnhDone stays true.
        g_packEnh = gage::game::content::PackEnhArm(w.packEnh, baseName);
        g_packEnhDone = !g_packEnh;
    }

    // ---- Native registration table ------------------------------------------
    // Collect every plausible table reference now; the background poller picks
    // the real one by resolving known native hashes through each candidate once
    // the game has registered its natives. g_nativeTable stays null until then.
    collectNativeTableCandidates(base, size);
    GLOGI("hook: initial native table scan -> %d candidates", (int)g_candidates.size());
    if (g_candidates.empty()) {
        GLOGW("hook: no native table candidates found (native invocation disabled)");
        if (w.err.empty()) w.err = "native_table";
    }

    // ---- D3D11 Present ------------------------------------------------------
    g_presentAddr = w.passive ? nullptr : findPresent();
    if (w.passive) {
        GLOGW("hook: passive mode: no dummy D3D11 device, Present not resolved");
    } else if (g_presentAddr) {
        GLOGI("hook: D3D11 Present resolved at %p", g_presentAddr);
    } else {
        GLOGW("hook: D3D11 Present not found (tick hook disabled)");
        if (w.err.empty()) w.err = "d3d11_present";
    }

    // Opt-in landing-page skip: the poller applies it once the game's
    // Arxan-decrypted code shows up (see tryPatchLandingPage).
    g_skipLanding = w.skipLanding;
    g_netSession = w.netSession;
    g_netHost = w.netHost;
    g_netProbe = w.netProbe;
    g_noSpScripts = w.noSpScripts;
    g_noSpKnownRva = w.noSpKnownRva != 0;

    // Import-table hooks on the game's own ws2_32/wininet imports (net_guard.h), as early as
    // possible. net_host=1 -> GUARD (S0: Rockstar hosts resolve to loopback, unknown non-loopback
    // destinations refused; nethost refuses to run without it, on any build). Otherwise
    // net_probe=1 -> read-only PROBE, on a recognised Legacy 3889 exe only.
    if (g_netHost) {
        gage::netguard::Install(gage::netguard::Mode::Guard);
        if (gage::netguard::GuardActive())
            GLOGI("hook: ROS guard ACTIVE (net_host=1): no Rockstar/matchmaking traffic leaves through the game's imports");
        else
            GLOGW("hook: ROS guard NOT active (net_host=1): the session host will refuse to run");
    } else if (g_netProbe) {
        if (g_buildMatched && g_build == &kProfile3889)
            gage::netguard::Install(gage::netguard::Mode::Probe);
        else
            GLOGI("hook: net_probe: hostname probe skipped (build %s; the import probe is Legacy 3889 only)",
                  g_build->label);
    }

    // Start the background poller that waits for the native table entries to
    // be populated. It runs every 75 ms independently of the render frame rate.
    HANDLE pollerHandle = CreateThread(nullptr, 0, nativeTablePollerThread, nullptr, 0, nullptr);
    if (pollerHandle)
        CloseHandle(pollerHandle);
    else
        GLOGW("hook: failed to start native table poller thread");

    // Network-session recon (net_session=1): a background thread scans the
    // decrypted image for the FiveM/YimMenu session-entry signatures and logs
    // which resolve uniquely on 3889 before any session hook is wired. Recon only.
    if (g_netSession) {
        HANDLE reconHandle = CreateThread(nullptr, 0, netSessionReconThread, nullptr, 0, nullptr);
        if (reconHandle)
            CloseHandle(reconHandle);
        else
            GLOGW("hook: failed to start net-session recon thread");
    }

    w.ready = true;
}

// g_nativeTableReady gates all callers (HaveNatives, CallNative, Resolved).
// Set to true only once PLAYER_PED_ID is successfully looked up.
std::atomic<bool> g_nativeTableReady{false};  // written by the poller thread, read by the game thread

// One-shot, after the native table validates: did the natives added for the world clean-up, the
// join readiness gate, the blip strip, remote-ped driving and the ped look resolve on this build? Their 3889 hashes come from the
// YimMenu crossmap and were not observed live, and an unmapped hash fails silently (CallNative
// returns false), so this line is where a bad one shows up. A native is retried for up to 10 s
// before it counts as unresolved, in case the table was still filling. Poller thread: looks the
// handlers up directly and never touches g_nativeCache (the game thread's).
static void logNewNativeResolution() {
    static bool logged = false;
    if (logged) return;
    logged = true;
#define GAGE_NEW_NATIVE(n) { #n, native::n }
    static const struct { const char* name; uint64_t hash; } kNew[] = {
        GAGE_NEW_NATIVE(SET_PED_DENSITY_MULTIPLIER_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_NUMBER_OF_PARKED_VEHICLES),
        GAGE_NEW_NATIVE(SET_RANDOM_BOATS),
        GAGE_NEW_NATIVE(CLEAR_AREA_OF_PEDS),
        GAGE_NEW_NATIVE(CLEAR_AREA_OF_COPS),
        GAGE_NEW_NATIVE(REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA),
        // T5 ambient sweep (0.4.7): the six that used to be hash-0 sentinels.
        GAGE_NEW_NATIVE(SET_PED_POPULATION_BUDGET),
        GAGE_NEW_NATIVE(SET_VEHICLE_POPULATION_BUDGET),
        GAGE_NEW_NATIVE(SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA),
        GAGE_NEW_NATIVE(DELETE_ALL_TRAINS),
        GAGE_NEW_NATIVE(GET_PED_NEARBY_PEDS),
        GAGE_NEW_NATIVE(GET_PED_NEARBY_VEHICLES),
        // Desync round 3
        GAGE_NEW_NATIVE(SET_PED_MAX_HEALTH),
        GAGE_NEW_NATIVE(SET_PLAYER_MAX_ARMOUR),
        GAGE_NEW_NATIVE(GET_VEHICLE_PETROL_TANK_HEALTH),
        GAGE_NEW_NATIVE(SET_VEHICLE_PETROL_TANK_HEALTH),
        GAGE_NEW_NATIVE(SET_VEHICLE_DAMAGE),
        GAGE_NEW_NATIVE(GET_VEHICLE_DEFORMATION_AT_POS),
        GAGE_NEW_NATIVE(IS_VEHICLE_DOOR_DAMAGED),
        GAGE_NEW_NATIVE(SET_VEHICLE_DOOR_BROKEN),
        GAGE_NEW_NATIVE(SET_VEHICLE_DOOR_CONTROL),
        GAGE_NEW_NATIVE(IS_VEHICLE_DOOR_FULLY_OPEN),
        GAGE_NEW_NATIVE(GET_IS_DOOR_VALID),
        GAGE_NEW_NATIVE(GET_IS_LEFT_VEHICLE_HEADLIGHT_DAMAGED),
        GAGE_NEW_NATIVE(GET_IS_RIGHT_VEHICLE_HEADLIGHT_DAMAGED),
        GAGE_NEW_NATIVE(SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED),
        GAGE_NEW_NATIVE(SET_VEHICLE_ENGINE_CAN_DEGRADE),
        GAGE_NEW_NATIVE(SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE),
        GAGE_NEW_NATIVE(IS_PED_GOING_INTO_COVER),
        GAGE_NEW_NATIVE(TASK_STAY_IN_COVER),
        GAGE_NEW_NATIVE(IS_PED_AIMING_FROM_COVER),
        GAGE_NEW_NATIVE(TASK_COMBAT_PED),
        GAGE_NEW_NATIVE(IS_PED_IN_MELEE_COMBAT),
        GAGE_NEW_NATIVE(TASK_PUT_PED_DIRECTLY_INTO_MELEE),
        GAGE_NEW_NATIVE(SET_PED_COMBAT_ATTRIBUTES),
        GAGE_NEW_NATIVE(GET_IS_LOADING_SCREEN_ACTIVE),
        GAGE_NEW_NATIVE(IS_SCREEN_FADED_IN),
        GAGE_NEW_NATIVE(IS_PLAYER_SWITCH_IN_PROGRESS),
        GAGE_NEW_NATIVE(IS_PLAYER_CONTROL_ON),
        GAGE_NEW_NATIVE(GET_ENTITY_MODEL),
        GAGE_NEW_NATIVE(GET_FIRST_BLIP_INFO_ID),
        GAGE_NEW_NATIVE(GET_NEXT_BLIP_INFO_ID),
        GAGE_NEW_NATIVE(GET_BLIP_SPRITE),
        GAGE_NEW_NATIVE(REMOVE_BLIP),
        GAGE_NEW_NATIVE(DOES_BLIP_EXIST),
        GAGE_NEW_NATIVE(GET_BLIP_INFO_ID_DISPLAY),
        GAGE_NEW_NATIVE(SET_BLIP_DISPLAY),
        GAGE_NEW_NATIVE(GET_MAIN_PLAYER_BLIP_ID),
        GAGE_NEW_NATIVE(GET_NORTH_BLID_INDEX),
        GAGE_NEW_NATIVE(GET_WAYPOINT_BLIP_ENUM_ID),
        GAGE_NEW_NATIVE(GET_NUMBER_OF_ACTIVE_BLIPS),
        GAGE_NEW_NATIVE(SET_THIS_SCRIPT_CAN_REMOVE_BLIPS_CREATED_BY_ANY_SCRIPT),
        GAGE_NEW_NATIVE(IS_WAYPOINT_ACTIVE),
        GAGE_NEW_NATIVE(SET_WAYPOINT_OFF),
        GAGE_NEW_NATIVE(SET_MINIMAP_HIDE_FOW),
        GAGE_NEW_NATIVE(GET_MINIMAP_FOW_DISCOVERY_RATIO),
        GAGE_NEW_NATIVE(GET_MINIMAP_FOW_COORDINATE_IS_REVEALED),
        // 0.4.22 minimap.
        GAGE_NEW_NATIVE(IS_RADAR_HIDDEN),
        GAGE_NEW_NATIVE(IS_MINIMAP_RENDERING),
        GAGE_NEW_NATIVE(IS_RADAR_PREFERENCE_SWITCHED_ON),
        GAGE_NEW_NATIVE(IS_HUD_HIDDEN),
        GAGE_NEW_NATIVE(REQUEST_SCALEFORM_MOVIE),
        GAGE_NEW_NATIVE(HAS_SCALEFORM_MOVIE_LOADED),
        GAGE_NEW_NATIVE(BEGIN_SCALEFORM_MOVIE_METHOD),
        GAGE_NEW_NATIVE(SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT),
        GAGE_NEW_NATIVE(END_SCALEFORM_MOVIE_METHOD),
        GAGE_NEW_NATIVE(SET_BIGMAP_ACTIVE),
        GAGE_NEW_NATIVE(GET_SAFE_ZONE_SIZE),
        // remote-player ped driving, sender state readers, appearance read/apply.
        GAGE_NEW_NATIVE(SET_ENTITY_COORDS_NO_OFFSET),
        GAGE_NEW_NATIVE(SET_ENTITY_VELOCITY),
        GAGE_NEW_NATIVE(GET_ENTITY_VELOCITY),
        GAGE_NEW_NATIVE(SET_ENTITY_COLLISION),
        GAGE_NEW_NATIVE(GET_ENTITY_HEIGHT_ABOVE_GROUND),
        GAGE_NEW_NATIVE(GET_GROUND_Z_FOR_3D_COORD),
        GAGE_NEW_NATIVE(TASK_GO_STRAIGHT_TO_COORD),
        GAGE_NEW_NATIVE(TASK_STAND_STILL),
        GAGE_NEW_NATIVE(CLEAR_PED_TASKS),
        GAGE_NEW_NATIVE(SET_PED_DESIRED_MOVE_BLEND_RATIO),
        GAGE_NEW_NATIVE(SET_PED_DESIRED_HEADING),
        GAGE_NEW_NATIVE(IS_PED_WALKING),
        GAGE_NEW_NATIVE(IS_PED_RUNNING),
        GAGE_NEW_NATIVE(IS_PED_SPRINTING),
        GAGE_NEW_NATIVE(IS_PED_STILL),
        GAGE_NEW_NATIVE(IS_PED_JUMPING),
        GAGE_NEW_NATIVE(TASK_JUMP),
        GAGE_NEW_NATIVE(IS_PED_RAGDOLL),
        GAGE_NEW_NATIVE(SET_PED_TO_RAGDOLL),
        GAGE_NEW_NATIVE(SET_PED_CAN_RAGDOLL),
        GAGE_NEW_NATIVE(SET_PED_CAN_EVASIVE_DIVE),
        GAGE_NEW_NATIVE(SET_BLOCKING_OF_NON_TEMPORARY_EVENTS),
        GAGE_NEW_NATIVE(SET_PED_CONFIG_FLAG),
        GAGE_NEW_NATIVE(GET_PED_CONFIG_FLAG),
        GAGE_NEW_NATIVE(SET_PED_KEEP_TASK),
        GAGE_NEW_NATIVE(SET_PED_CAN_PLAY_AMBIENT_ANIMS),
        GAGE_NEW_NATIVE(IS_PED_FALLING),
        GAGE_NEW_NATIVE(IS_PED_CLIMBING),
        GAGE_NEW_NATIVE(IS_PED_IN_PARACHUTE_FREE_FALL),
        GAGE_NEW_NATIVE(IS_PED_SHOOTING),
        GAGE_NEW_NATIVE(IS_PLAYER_FREE_AIMING),
        GAGE_NEW_NATIVE(GET_SELECTED_PED_WEAPON),
        GAGE_NEW_NATIVE(IS_WEAPON_VALID),
        GAGE_NEW_NATIVE(SET_CURRENT_PED_WEAPON),
        GAGE_NEW_NATIVE(TASK_AIM_GUN_AT_COORD),
        GAGE_NEW_NATIVE(TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD),
        GAGE_NEW_NATIVE(GET_PED_DRAWABLE_VARIATION),
        GAGE_NEW_NATIVE(GET_PED_TEXTURE_VARIATION),
        GAGE_NEW_NATIVE(GET_PED_PALETTE_VARIATION),
        GAGE_NEW_NATIVE(GET_PED_PROP_INDEX),
        GAGE_NEW_NATIVE(GET_PED_PROP_TEXTURE_INDEX),
        GAGE_NEW_NATIVE(SET_PED_PROP_INDEX),
        GAGE_NEW_NATIVE(CLEAR_PED_PROP),
        GAGE_NEW_NATIVE(GET_PED_HEAD_BLEND_DATA),
        // vehicle sync, weapons/shots/damage, camera/aim.
        GAGE_NEW_NATIVE(GET_ENTITY_QUATERNION),
        GAGE_NEW_NATIVE(SET_ENTITY_QUATERNION),
        GAGE_NEW_NATIVE(GET_ENTITY_ROTATION),
        GAGE_NEW_NATIVE(SET_ENTITY_ROTATION),
        GAGE_NEW_NATIVE(GET_ENTITY_ROTATION_VELOCITY),
        GAGE_NEW_NATIVE(SET_ENTITY_ANGULAR_VELOCITY),
        GAGE_NEW_NATIVE(GET_PED_IN_VEHICLE_SEAT),
        GAGE_NEW_NATIVE(GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS),
        GAGE_NEW_NATIVE(IS_VEHICLE_SEAT_FREE),
        GAGE_NEW_NATIVE(GET_VEHICLE_MODEL_NUMBER_OF_SEATS),
        GAGE_NEW_NATIVE(GET_VEHICLE_DOOR_LOCK_STATUS),
        GAGE_NEW_NATIVE(PLAY_SOUND_FRONTEND),
        GAGE_NEW_NATIVE(GET_SEAT_PED_IS_TRYING_TO_ENTER),
        GAGE_NEW_NATIVE(GET_VEHICLE_PED_IS_TRYING_TO_ENTER),
        GAGE_NEW_NATIVE(GET_VEHICLE_PED_IS_USING),
        GAGE_NEW_NATIVE(TASK_ENTER_VEHICLE),
        GAGE_NEW_NATIVE(TASK_LEAVE_VEHICLE),
        GAGE_NEW_NATIVE(TASK_WARP_PED_INTO_VEHICLE),
        GAGE_NEW_NATIVE(SET_VEHICLE_STEER_BIAS),
        GAGE_NEW_NATIVE(GET_VEHICLE_ENGINE_HEALTH),
        GAGE_NEW_NATIVE(SET_VEHICLE_ENGINE_HEALTH),
        GAGE_NEW_NATIVE(GET_VEHICLE_BODY_HEALTH),
        GAGE_NEW_NATIVE(SET_VEHICLE_BODY_HEALTH),
        GAGE_NEW_NATIVE(GET_IS_VEHICLE_ENGINE_RUNNING),
        GAGE_NEW_NATIVE(SET_VEHICLE_SIREN),
        GAGE_NEW_NATIVE(IS_VEHICLE_SIREN_ON),
        GAGE_NEW_NATIVE(START_VEHICLE_HORN),
        GAGE_NEW_NATIVE(GET_VEHICLE_COLOURS),
        GAGE_NEW_NATIVE(SET_VEHICLE_COLOURS),
        GAGE_NEW_NATIVE(SET_VEHICLE_DOORS_LOCKED),
        GAGE_NEW_NATIVE(SET_VEHICLE_LIGHTS),
        GAGE_NEW_NATIVE(SET_VEHICLE_FORWARD_SPEED),
        GAGE_NEW_NATIVE(SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER),
        GAGE_NEW_NATIVE(SET_VEHICLE_IS_STOLEN),
        GAGE_NEW_NATIVE(SET_ENTITY_AS_MISSION_ENTITY),
        GAGE_NEW_NATIVE(SET_VEHICLE_DOOR_SHUT),
        GAGE_NEW_NATIVE(SHOOT_SINGLE_BULLET_BETWEEN_COORDS),
        GAGE_NEW_NATIVE(SET_PED_SHOOTS_AT_COORD),
        GAGE_NEW_NATIVE(TASK_SHOOT_AT_COORD),
        GAGE_NEW_NATIVE(GET_PED_LAST_WEAPON_IMPACT_COORD),
        GAGE_NEW_NATIVE(HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY),
        GAGE_NEW_NATIVE(HAS_ENTITY_BEEN_DAMAGED_BY_ANY_PED),
        GAGE_NEW_NATIVE(CLEAR_ENTITY_LAST_DAMAGE_ENTITY),
        GAGE_NEW_NATIVE(GET_PED_LAST_DAMAGE_BONE),
        GAGE_NEW_NATIVE(CLEAR_PED_LAST_DAMAGE_BONE),
        GAGE_NEW_NATIVE(GET_PED_ARMOUR),
        GAGE_NEW_NATIVE(SET_PED_ARMOUR),
        GAGE_NEW_NATIVE(GET_WEAPON_DAMAGE),
        GAGE_NEW_NATIVE(GET_PED_SOURCE_OF_DEATH),
        GAGE_NEW_NATIVE(IS_ENTITY_DEAD),
        GAGE_NEW_NATIVE(SET_ENTITY_CAN_BE_DAMAGED),
        GAGE_NEW_NATIVE(SET_ENTITY_PROOFS),
        GAGE_NEW_NATIVE(SET_PED_SUFFERS_CRITICAL_HITS),
        GAGE_NEW_NATIVE(APPLY_DAMAGE_TO_PED),
        GAGE_NEW_NATIVE(GET_CURRENT_PED_WEAPON_ENTITY_INDEX),
        GAGE_NEW_NATIVE(GET_ENTITY_BONE_INDEX_BY_NAME),
        GAGE_NEW_NATIVE(GET_WORLD_POSITION_OF_ENTITY_BONE),
        GAGE_NEW_NATIVE(GET_AMMO_IN_PED_WEAPON),
        GAGE_NEW_NATIVE(SET_PED_INFINITE_AMMO_CLIP),
        GAGE_NEW_NATIVE(GET_GAMEPLAY_CAM_COORD),
        GAGE_NEW_NATIVE(GET_GAMEPLAY_CAM_ROT),
        GAGE_NEW_NATIVE(START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE),
        GAGE_NEW_NATIVE(GET_SHAPE_TEST_RESULT),
        GAGE_NEW_NATIVE(IS_AIM_CAM_ACTIVE),
        GAGE_NEW_NATIVE(GET_ENTITY_PLAYER_IS_FREE_AIMING_AT),
        GAGE_NEW_NATIVE(NETWORK_IS_SIGNED_IN),
        GAGE_NEW_NATIVE(NETWORK_IS_SIGNED_ONLINE),
        GAGE_NEW_NATIVE(NETWORK_HAVE_ONLINE_PRIVILEGES),
        GAGE_NEW_NATIVE(NETWORK_IS_CLOUD_AVAILABLE),
        GAGE_NEW_NATIVE(NETWORK_IS_SESSION_STARTED),
        GAGE_NEW_NATIVE(NETWORK_IS_IN_SESSION),
        GAGE_NEW_NATIVE(REQUEST_ANIM_DICT),
        GAGE_NEW_NATIVE(HAS_ANIM_DICT_LOADED),
        GAGE_NEW_NATIVE(REMOVE_ANIM_DICT),
        GAGE_NEW_NATIVE(DOES_ANIM_DICT_EXIST),
        GAGE_NEW_NATIVE(TASK_PLAY_ANIM),
        GAGE_NEW_NATIVE(TASK_START_SCENARIO_IN_PLACE),
        GAGE_NEW_NATIVE(IS_ENTITY_PLAYING_ANIM),
        GAGE_NEW_NATIVE(IS_PED_USING_ANY_SCENARIO),
        GAGE_NEW_NATIVE(SET_ENTITY_MAX_HEALTH),
        GAGE_NEW_NATIVE(DOES_ENTITY_EXIST),
        GAGE_NEW_NATIVE(IS_HORN_ACTIVE),
        GAGE_NEW_NATIVE(SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER),
        GAGE_NEW_NATIVE(ACTIVATE_FRONTEND_MENU),
        GAGE_NEW_NATIVE(IS_PAUSE_MENU_ACTIVE),
        GAGE_NEW_NATIVE(IS_DISABLED_CONTROL_JUST_PRESSED),
        GAGE_NEW_NATIVE(DISABLE_FRONTEND_THIS_FRAME),
        GAGE_NEW_NATIVE(SET_PAUSE_MENU_ACTIVE),
        GAGE_NEW_NATIVE(GET_GAME_TIMER),
        GAGE_NEW_NATIVE(GET_WEAPONTYPE_GROUP),
        GAGE_NEW_NATIVE(IS_PED_RELOADING),
        GAGE_NEW_NATIVE(TASK_RELOAD_WEAPON),
        GAGE_NEW_NATIVE(GET_PED_STEALTH_MOVEMENT),
        GAGE_NEW_NATIVE(SET_PED_STEALTH_MOVEMENT),
        GAGE_NEW_NATIVE(IS_PED_SWIMMING),
        GAGE_NEW_NATIVE(IS_PED_IN_COVER),
        GAGE_NEW_NATIVE(TASK_PUT_PED_DIRECTLY_INTO_COVER),
        GAGE_NEW_NATIVE(IS_PED_PERFORMING_MELEE_ACTION),
        GAGE_NEW_NATIVE(SET_PED_MOVE_RATE_OVERRIDE),
        GAGE_NEW_NATIVE(IS_PED_VAULTING),
        GAGE_NEW_NATIVE(TASK_CLIMB),
        GAGE_NEW_NATIVE(TASK_GO_TO_ENTITY),
        GAGE_NEW_NATIVE(SET_AI_WEAPON_DAMAGE_MODIFIER),
        GAGE_NEW_NATIVE(SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER),
        GAGE_NEW_NATIVE(RESET_AI_WEAPON_DAMAGE_MODIFIER),
        GAGE_NEW_NATIVE(RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER),
        GAGE_NEW_NATIVE(WAS_PED_KILLED_BY_TAKEDOWN),
        GAGE_NEW_NATIVE(MODIFY_VEHICLE_TOP_SPEED),
        GAGE_NEW_NATIVE(SET_VEHICLE_CHEAT_POWER_INCREASE),
        GAGE_NEW_NATIVE(SET_VEHICLE_MAX_SPEED),
        GAGE_NEW_NATIVE(GET_VEHICLE_ESTIMATED_MAX_SPEED),
        GAGE_NEW_NATIVE(SET_VEHICLE_STRONG),
        GAGE_NEW_NATIVE(SET_VEHICLE_HAS_STRONG_AXLES),
        GAGE_NEW_NATIVE(SET_VEHICLE_TYRES_CAN_BURST),
        GAGE_NEW_NATIVE(SET_VEHICLE_WHEELS_CAN_BREAK),
        GAGE_NEW_NATIVE(IS_ENTITY_IN_AIR),
        GAGE_NEW_NATIVE(GET_ENTITY_ANIM_CURRENT_TIME),
        GAGE_NEW_NATIVE(GET_ENTITY_ANIM_TOTAL_TIME),
        GAGE_NEW_NATIVE(GET_ANIM_DURATION),
        GAGE_NEW_NATIVE(GET_SCRIPT_TASK_STATUS),
        GAGE_NEW_NATIVE(STOP_ANIM_TASK),
        GAGE_NEW_NATIVE(SET_USER_RADIO_CONTROL_ENABLED),
        GAGE_NEW_NATIVE(SET_VEHICLE_RADIO_ENABLED),
        GAGE_NEW_NATIVE(SET_VEH_RADIO_STATION),
        GAGE_NEW_NATIVE(SET_RADIO_TO_STATION_NAME),
        GAGE_NEW_NATIVE(SET_MOBILE_RADIO_ENABLED_DURING_GAMEPLAY),
        GAGE_NEW_NATIVE(SET_AUDIO_FLAG),
        GAGE_NEW_NATIVE(GET_PLAYER_RADIO_STATION_INDEX),
        GAGE_NEW_NATIVE(IS_VEHICLE_RADIO_ON),
        GAGE_NEW_NATIVE(IS_HUD_COMPONENT_ACTIVE),
        GAGE_NEW_NATIVE(IS_DISABLED_CONTROL_PRESSED),
        GAGE_NEW_NATIVE(SET_ENTITY_HAS_GRAVITY),
        GAGE_NEW_NATIVE(GET_ENTITY_COLLISION_DISABLED),
        GAGE_NEW_NATIVE(SET_PLAYER_INVINCIBLE),
        GAGE_NEW_NATIVE(GET_PLAYER_INVINCIBLE),
        GAGE_NEW_NATIVE(CLEAR_TIMECYCLE_MODIFIER),
        GAGE_NEW_NATIVE(GET_TIMECYCLE_MODIFIER_INDEX),
        GAGE_NEW_NATIVE(DRAW_LIGHT_WITH_RANGE),
        GAGE_NEW_NATIVE(GET_MODEL_DIMENSIONS),
        GAGE_NEW_NATIVE(GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS),
        GAGE_NEW_NATIVE(GET_VEHICLE_LIGHTS_STATE),
        GAGE_NEW_NATIVE(SET_VEHICLE_FULLBEAM),
        GAGE_NEW_NATIVE(SET_VEHICLE_INTERIORLIGHT),
        GAGE_NEW_NATIVE(SET_VEHICLE_INDICATOR_LIGHTS),
        GAGE_NEW_NATIVE(GET_VEHICLE_DOOR_ANGLE_RATIO),
        GAGE_NEW_NATIVE(SET_VEHICLE_DOOR_OPEN),
        GAGE_NEW_NATIVE(GET_CONVERTIBLE_ROOF_STATE),
        GAGE_NEW_NATIVE(RAISE_CONVERTIBLE_ROOF),
        GAGE_NEW_NATIVE(LOWER_CONVERTIBLE_ROOF),
        GAGE_NEW_NATIVE(TASK_DRIVE_BY),
        GAGE_NEW_NATIVE(SET_DRIVEBY_TASK_TARGET),
        GAGE_NEW_NATIVE(TASK_VEHICLE_AIM_AT_COORD),
        GAGE_NEW_NATIVE(CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK),
        GAGE_NEW_NATIVE(GET_IS_TASK_ACTIVE),
        GAGE_NEW_NATIVE(START_PARTICLE_FX_NON_LOOPED_ON_ENTITY),
        GAGE_NEW_NATIVE(USE_PARTICLE_FX_ASSET),
        GAGE_NEW_NATIVE(REQUEST_NAMED_PTFX_ASSET),
        GAGE_NEW_NATIVE(HAS_NAMED_PTFX_ASSET_LOADED),
        GAGE_NEW_NATIVE(HAS_ENTITY_COLLIDED_WITH_ANYTHING),
        GAGE_NEW_NATIVE(GET_VEHICLE_CLASS),
        GAGE_NEW_NATIVE(GET_SCREEN_COORD_FROM_WORLD_COORD),
        GAGE_NEW_NATIVE(GET_FINAL_RENDERED_CAM_COORD),
        GAGE_NEW_NATIVE(HAS_ENTITY_CLEAR_LOS_TO_ENTITY),
        GAGE_NEW_NATIVE(GET_PED_BONE_COORDS),
        // T3 vehicle visual state (0.4.7). A hash of 0 would be skipped below, with one line each.
        GAGE_NEW_NATIVE(IS_VEHICLE_TYRE_BURST),
        GAGE_NEW_NATIVE(SET_VEHICLE_TYRE_BURST),
        GAGE_NEW_NATIVE(SET_VEHICLE_TYRE_FIXED),
        GAGE_NEW_NATIVE(IS_VEHICLE_WINDOW_INTACT),
        GAGE_NEW_NATIVE(SMASH_VEHICLE_WINDOW),
        GAGE_NEW_NATIVE(FIX_VEHICLE_WINDOW),
        GAGE_NEW_NATIVE(GET_CONTROL_NORMAL),
        GAGE_NEW_NATIVE(SET_VEHICLE_HANDBRAKE),
        GAGE_NEW_NATIVE(SET_VEHICLE_BRAKE_LIGHTS),
    };
#undef GAGE_NEW_NATIVE
    constexpr int total = (int)(sizeof(kNew) / sizeof(kNew[0]));
    bool ok[total] = {};
    int resolved = 0;
    for (int i = 0; i < total; ++i) {
        if (kNew[i].hash) continue;
        ok[i] = true;                                   // not a lookup failure: no hash to look up yet
        ++resolved;
        GLOGI("hook: native %s has no hash yet (UNRESOLVED: lookup needed); the feature that needs it stays off", kNew[i].name);
    }
    for (int pass = 0; pass < 20 && resolved < total; ++pass) {
        if (pass) Sleep(500);
        for (int i = 0; i < total; ++i) {
            if (ok[i]) continue;
            // Enhanced has no bucket table: InitNativeTables filled the native CACHE, keyed by the public hash
            // (what CallNative asks for first). Reading only g_nativeTable made this report 0/233 UNRESOLVED
            // for natives that were in use ("call worked") on the friend's first Enhanced run.
            {
                NativeHandler hc = nullptr;
                if (nativeCacheFind(kNew[i].hash, hc) && hc && isExecutable((const void*)hc)) {
                    ok[i] = true;
                    ++resolved;
                    continue;
                }
            }
            // The 3889 hash first, then the hash as given: the order CallNative uses.
            for (uint64_t want : {buildHash(kNew[i].hash), kNew[i].hash}) {
                const NativeHandler h = g_getNativeHandler ? g_getNativeHandler(g_nativeTable, want)
                                                           : lookupInTable(g_nativeTable, want);
                if (h && isExecutable((const void*)h)) {
                    ok[i] = true;
                    ++resolved;
                    break;
                }
            }
        }
    }
    // [streamdiag] one-shot: log the image RVAs of model/streaming native handlers. The static dump has
    // valid code but no runtime native table (it is populated on the heap at boot), so handler addresses
    // can only be obtained live; with these RVAs their bodies disassemble offline to find the fragment/txd
    // streaming stores for the CStreaming content route. Diagnostic only (not shipped behaviour).
    // Legacy 3889 only: the hashes are 3889 build hashes and the RVAs below are 3889 RVAs. On Enhanced they logged
    // UNRESOLVED / MISMATCH noise (the friend's first run) about natives that work.
    if (g_build == &kProfile3889) {
        struct DiagNative { const char* name; uint64_t hash; };
        static const DiagNative kDiag[] = {
            {"REQUEST_MODEL",                 0xEC9DAA34BBB4658Cull},
            {"HAS_MODEL_LOADED",              0x6252BC0DD8A320DBull},
            {"IS_MODEL_IN_CDIMAGE",           0xE7D342E0F16AAA8Full},
            {"IS_MODEL_A_VEHICLE",            0xAD1840C2E6AF7D5Eull},
            {"CREATE_VEHICLE",                0x5779387E956077A6ull},
            {"GET_HASH_KEY",                  0x70E57E9927B6BA58ull},
            {"SET_MODEL_AS_NO_LONGER_NEEDED", 0x55098D9E9AD58806ull},
        };
        for (const DiagNative& d : kDiag) {
            NativeHandler h = findNativeHandler(d.hash);
            if (h && g_imageBase)
                GLOGI("hook: [streamdiag] %-28s handler @ +0x%llx", d.name,
                      (unsigned long long)((const uint8_t*)(uintptr_t)h - g_imageBase));
            else
                GLOGW("hook: [streamdiag] %-28s UNRESOLVED", d.name);
        }
    }
    // [storewalk] verify the CStreaming store globals found by static RE and dump their LIVE layout. The
    // store objects are runtime-constructed (absent/zero in the image dump), so their slot pool ptr, counts
    // and base stream index can only be read live; these drive the loose-model registration (Track 2).
    // Reads are bounded to the mapped image (RVAs < image size) so a wrong offset logs MISMATCH, never faults.
    if (g_build == &kProfile3889 && g_imageBase && g_imageSize > 0x3000000) {   // 3889 RVAs: noise on Enhanced
        struct StoreRef { const char* name; uintptr_t objRva; uintptr_t vtRva; };
        static const StoreRef kStores[] = {
            {"FragmentStore", 0x2f5d690, 0x1bb4190},
            {"TxdStore",      0x2f5da20, 0x1bb3bd0},
        };
        for (const StoreRef& s : kStores) {
            const uint8_t* obj = g_imageBase + s.objRva;
            const uint64_t vt = *reinterpret_cast<const uint64_t*>(obj);
            const uint64_t wantVt = (uint64_t)(uintptr_t)g_imageBase + s.vtRva;
            GLOGI("hook: [storewalk] %-13s @+0x%llx  vtable=0x%llx want 0x%llx -> %s",
                  s.name, (unsigned long long)s.objRva, (unsigned long long)vt,
                  (unsigned long long)wantVt, vt == wantVt ? "OK" : "MISMATCH");
            const uint64_t* q = reinterpret_cast<const uint64_t*>(obj);
            for (int off = 0; off < 0x200; off += 0x20)
                GLOGI("hook: [storewalk]   +0x%03x: %016llx %016llx %016llx %016llx", off,
                      (unsigned long long)q[off/8+0], (unsigned long long)q[off/8+1],
                      (unsigned long long)q[off/8+2], (unsigned long long)q[off/8+3]);
        }
    }
    // [vehdiag2] read-only: decode the streaming-handle encoding (image/collection index + offset) used
    // by base-game vehicle fragment/txd slots, for the FiveM-style custom collection. v1 proved the slot
    // name hash is NOT at +0x0/+0x8/+0xc, so: (a) raw-dump the first pool entries to see the slot shape;
    // (b) one pass brute-forces the name-hash offset across the whole pool + collects occupancy stats;
    // (c) window-dump the streaming-info manager across each store's global range to expose the handle
    // bit layout even without a name anchor. Bounded, validated reads; NO game-function calls.
    if (g_build == &kProfile3889 && g_imageBase && g_imageSize > 0x3000000) {   // 3889 RVAs: noise on Enhanced
        auto joaat = [](const char* s) -> uint32_t {
            uint32_t h = 0;
            for (; *s; ++s) { char c = *s; if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                h += (uint8_t)c; h += h << 10; h ^= h >> 6; }
            h += h << 3; h ^= h >> 11; h += h << 15; return h;
        };
        const uint8_t* mgr    = g_imageBase + 0x2ea23d0;
        const uint8_t* mgrArr = *reinterpret_cast<const uint8_t* const*>(mgr);
        const uint32_t mgrCnt = *reinterpret_cast<const uint32_t*>(mgr + 8);
        const char* names[3]  = { "adder", "police", "comet2" };
        const uint32_t wants[3] = { joaat(names[0]), joaat(names[1]), joaat(names[2]) };
        GLOGI("hook: [vehdiag2] mgrArr=%p mgrCnt=%u  wants adder=0x%08x police=0x%08x comet2=0x%08x",
              (const void*)mgrArr, mgrCnt, wants[0], wants[1], wants[2]);
        struct Store { const char* tag; uintptr_t objRva; };
        static const Store kStores[] = { {"frag", 0x2f5d690}, {"txd", 0x2f5da20} };
        for (const Store& st : kStores) {
            const uint8_t* s       = g_imageBase + st.objRva;
            const uint8_t* arrBase = *reinterpret_cast<const uint8_t* const*>(s + 0x38);
            const uint32_t count   = *reinterpret_cast<const uint32_t*>(s + 0x48);
            const uint32_t elem    = *reinterpret_cast<const uint32_t*>(s + 0x4c);
            const int32_t  baseIdx = *reinterpret_cast<const int32_t*>(s + 0x8);
            GLOGI("hook: [vehdiag2] %-4s arr=%p count=%u elem=0x%x baseIdx=%d",
                  st.tag, (const void*)arrBase, count, elem, baseIdx);
            if (!arrBase || elem != 0x18 || !count || count >= 0x40000) {
                GLOGW("hook: [vehdiag2] %-4s layout unexpected - skip", st.tag);
                continue;
            }
            // (a) raw dump first 8 entries (3 qwords each)
            for (uint32_t i = 0; i < 8 && i < count; ++i) {
                const uint64_t* q = reinterpret_cast<const uint64_t*>(arrBase + (size_t)i * elem);
                GLOGI("hook: [vehdiag2] %-4s [%u] %016llx %016llx %016llx",
                      st.tag, i, (unsigned long long)q[0], (unsigned long long)q[1],
                      (unsigned long long)q[2]);
            }
            // (b) one pass: brute-force name-hash offset (0x0..0x14) + occupancy stats
            uint32_t nonNullPtr = 0, plaus10 = 0, plaus14 = 0;
            int hitIdx[3] = { -1, -1, -1 };
            int hitOff[3] = { -1, -1, -1 };
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* n = arrBase + (size_t)i * elem;
                if (*reinterpret_cast<const uint64_t*>(n) != 0) ++nonNullPtr;
                if (*reinterpret_cast<const uint32_t*>(n + 0x10) < mgrCnt) ++plaus10;
                if (*reinterpret_cast<const uint32_t*>(n + 0x14) < mgrCnt) ++plaus14;
                for (int k = 0; k < 3; ++k) {
                    if (hitIdx[k] >= 0) continue;
                    for (int off = 0; off <= 0x14; off += 4)
                        if (*reinterpret_cast<const uint32_t*>(n + off) == wants[k]) {
                            hitIdx[k] = (int)i; hitOff[k] = off; break;
                        }
                }
            }
            GLOGI("hook: [vehdiag2] %-4s occupancy nonNullPtr=%u/%u  +0x10 in-range=%u  +0x14 in-range=%u",
                  st.tag, nonNullPtr, count, plaus10, plaus14);
            for (int k = 0; k < 3; ++k) {
                if (hitIdx[k] < 0) { GLOGI("hook: [vehdiag2] %-4s %-8s not in pool", st.tag, names[k]); continue; }
                const uint8_t* n = arrBase + (size_t)hitIdx[k] * elem;
                const uint64_t* q = reinterpret_cast<const uint64_t*>(n);
                const uint32_t f10 = *reinterpret_cast<const uint32_t*>(n + 0x10);
                const uint32_t f14 = *reinterpret_cast<const uint32_t*>(n + 0x14);
                const long long g  = (long long)baseIdx + hitIdx[k];
                GLOGI("hook: [vehdiag2] %-4s %-8s FOUND idx=%d nameOff=0x%x entry %016llx %016llx %016llx",
                      st.tag, names[k], hitIdx[k], hitOff[k],
                      (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2]);
                GLOGI("hook: [vehdiag2] %-4s   +0x10=0x%08x +0x14=0x%08x base+idx=%lld", st.tag, f10, f14, g);
                if (mgrArr && g >= 0 && (uint64_t)g < mgrCnt) {
                    const uint32_t* e = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)g * 8);
                    GLOGI("hook: [vehdiag2] %-4s   mgr[base+idx=%lld] handle=0x%08x flags=0x%08x", st.tag, g, e[0], e[1]);
                }
                if (mgrArr && f10 < mgrCnt) {
                    const uint32_t* e = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)f10 * 8);
                    GLOGI("hook: [vehdiag2] %-4s   mgr[+0x10=%u] handle=0x%08x flags=0x%08x", st.tag, f10, e[0], e[1]);
                }
                if (mgrArr && f14 < mgrCnt) {
                    const uint32_t* e = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)f14 * 8);
                    GLOGI("hook: [vehdiag2] %-4s   mgr[+0x14=%u] handle=0x%08x flags=0x%08x", st.tag, f14, e[0], e[1]);
                }
            }
            // (c) window dump of mgr across this store's global range (handle encoding)
            if (mgrArr && baseIdx >= 0 && (uint64_t)baseIdx < mgrCnt) {
                for (int w = 0; w < 12; ++w) {
                    const long long g = (long long)baseIdx + w;
                    if ((uint64_t)g >= mgrCnt) break;
                    const uint32_t* e = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)g * 8);
                    GLOGI("hook: [vehdiag2] %-4s mgr[%lld] handle=0x%08x flags=0x%08x", st.tag, g, e[0], e[1]);
                }
            }
        }
        // alternate global base (0x2f5d988) in case base+idx is wrong for the fragment store
        const int32_t baseIdxG = *reinterpret_cast<const int32_t*>(g_imageBase + 0x2f5d988);
        if (mgrArr && baseIdxG >= 0 && (uint64_t)baseIdxG < mgrCnt) {
            GLOGI("hook: [vehdiag2] alt baseG=%d window:", baseIdxG);
            for (int w = 0; w < 12; ++w) {
                const long long g = (long long)baseIdxG + w;
                if ((uint64_t)g >= mgrCnt) break;
                const uint32_t* e = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)g * 8);
                GLOGI("hook: [vehdiag2]   mgrG[%lld] handle=0x%08x flags=0x%08x", g, e[0], e[1]);
            }
        }
    }
    // [fidiag] read-only: dump real registered fiCollection pointers from the image registry (0x2c67400)
    // + their vtables (the fiPackfile/fiCollection interface our custom collection must clone) + object
    // layout. Heap coll ptrs are low (<4GB); their vtable ptr is a high image VA -> RVA = vt - g_imageBase.
    // Bounded (first 3 non-null, <=4096 scanned, 56 vtable slots). NO game-function calls -> no crash risk.
    if (g_build == &kProfile3889 && g_imageBase && g_imageSize > 0x3000000) {   // 3889 RVAs: noise on Enhanced
        const uint8_t* const* reg = reinterpret_cast<const uint8_t* const*>(g_imageBase + 0x2c67400);
        const int32_t cap = *reinterpret_cast<const int32_t*>(g_imageBase + 0x1b81588);
        GLOGI("hook: [fidiag] registry=%p cap=%d imageBase=%p imageSize=0x%llx",
              (const void*)reg, cap, (const void*)g_imageBase, (unsigned long long)g_imageSize);
        int shown = 0;
        const int scanMax = (cap > 0 && cap < 4096) ? cap : 4096;
        for (int i = 0; i < scanMax && shown < 3; ++i) {
            const uint8_t* coll = reg[i];
            if (!coll || (uintptr_t)coll < 0x10000) continue;
            const uint8_t* vt = *reinterpret_cast<const uint8_t* const*>(coll);
            const uintptr_t vtrva = (uintptr_t)(vt - g_imageBase);
            if (vtrva >= (uintptr_t)g_imageSize) continue;  // vtable not in image -> not a code collection
            GLOGI("hook: [fidiag] image[%d] coll=%p vtableRVA=0x%llx", i, (const void*)coll,
                  (unsigned long long)vtrva);
            const uint64_t* oq = reinterpret_cast<const uint64_t*>(coll);
            for (int o = 0; o < 0x40; o += 0x20)
                GLOGI("hook: [fidiag]   obj+0x%02x: %016llx %016llx %016llx %016llx",
                      o, (unsigned long long)oq[o/8], (unsigned long long)oq[o/8+1],
                      (unsigned long long)oq[o/8+2], (unsigned long long)oq[o/8+3]);
            const uint64_t* vq = reinterpret_cast<const uint64_t*>(vt);
            for (int s = 0; s < 56; s += 4) {
                auto RV = [&](int k) -> unsigned long long {
                    const uintptr_t r = (uintptr_t)((const uint8_t*)vq[s + k] - g_imageBase);
                    return (r < (uintptr_t)g_imageSize) ? (unsigned long long)r : 0xffffffffULL; };
                GLOGI("hook: [fidiag]   vt[%2d] +0x%03x: %08llx %08llx %08llx %08llx",
                      s, s * 8, RV(0), RV(1), RV(2), RV(3));
            }
            // [entdiag] read-only: for a real fiPackfile (vtable 0x1b82890) dump its object streaming
            // fields + a few concrete 0x10-byte entries so the exact entry bit-packing is observed, not
            // guessed, before we build an in-memory packfile the game will read. idxs 0x248..0x24c are
            // the known vehicle-fragment entries for imageIndex 2 (frag handle 0x00020249). Bounded by
            // the collection's own entry count; heap reads only, no game calls.
            if (vtrva == 0x1b82890) {
                const uint8_t sh = coll[0xb8];
                const uint32_t cnt28 = *reinterpret_cast<const uint32_t*>(coll + 0x28);
                const uint32_t cnt30 = *reinterpret_cast<const uint32_t*>(coll + 0x30);
                const uint64_t dataBase = *reinterpret_cast<const uint64_t*>(coll + 0x10);
                const uint64_t dev = *reinterpret_cast<const uint64_t*>(coll + 0x50);
                const uint32_t baseOff = *reinterpret_cast<const uint32_t*>(coll + 0x58);
                const uint8_t* tbl = *reinterpret_cast<const uint8_t* const*>(coll + 0x20);
                GLOGI("hook: [entdiag] packfile coll=%p shift=%u cnt28=%u cnt30=%u dataBase=%016llx dev=%016llx baseOff=%u tbl=%p",
                      (const void*)coll, sh, cnt28, cnt30,
                      (unsigned long long)dataBase, (unsigned long long)dev, baseOff, (const void*)tbl);
                if (tbl && (uintptr_t)tbl >= 0x10000 && (uintptr_t)tbl < 0x100000000ULL) {
                    static const int idxs[] = { 0, 1, 2, 0x248, 0x249, 0x24a, 0x24b, 0x24c };
                    for (int k = 0; k < 8; ++k) {
                        const uint32_t idx = static_cast<uint32_t>(idxs[k]);
                        if (idx >= cnt28) continue;
                        const uint64_t* e = reinterpret_cast<const uint64_t*>(tbl + static_cast<size_t>(idx) * 0x10);
                        const uint64_t q0 = e[0], q1 = e[1];
                        const uint64_t offBytes = static_cast<uint64_t>(static_cast<uint32_t>(q0 & 0xffff)) << sh;
                        const uint32_t size24 = static_cast<uint32_t>((q0 >> 16) & 0xffffff);
                        const uint32_t flags = static_cast<uint32_t>((q0 >> 40) & 0xffffff);
                        const uint32_t altSize = static_cast<uint32_t>(q1 & 0xffffffff);
                        GLOGI("hook: [entdiag]   ent[0x%x] q0=%016llx q1=%016llx -> off=%llu size24=%u flags=0x%06x alt=%u",
                              idx, (unsigned long long)q0, (unsigned long long)q1,
                              (unsigned long long)offBytes, size24, flags, altSize);
                    }
                }
            }
            ++shown;
        }
    }
    // [cdiag] read-only: settle the fragType resource-construct ABI for Route B (manual load+Set). The
    // place worker (FragmentStore vtable slot 42, RVA 0x16b7a0c) passes a TYPE DESCRIPTOR (RVA 0x1bb5478)
    // + type 0xa2 to the generic constructor 0x1354d0c. We must know whether that construct consumes a
    // datResourceMap of already-decompressed pages (-> hand-build pages + inflate) or a name/req-desc =
    // a full device-backed named load (-> provide a device). Read-only evidence:
    //  (a) descriptor 0x1bb5478: field0 (id/size) + each fn slot resolved to an RVA (so its construct fn
    //      disassembles offline, and the live image is confirmed to match the dump);
    //  (b) a RESIDENT FragmentStore slot (flagByte&0x80==0, obj!=0): the fragType* object head (its vtable
    //      RVA proves a constructed object) + the slot name-hash [+0x08] and streaming handle [+0x10] =>
    //      a live handle into the streaming mgr == device/handle-backed loads;
    //  (c) the resource-manager context head 0x1e2ee90 + the '#'-replacement char [0x1e2d744] + the paging
    //      sentinel [0x1e2ee00].
    // Bounded reads inside the mapped image; NO game-function calls -> no crash risk (same contract as
    // [fidiag]/[vehdiag2]). Remove with the other diagnostics before shipping.
    if (g_build == &kProfile3889 && g_imageBase && g_imageSize > 0x3000000) {   // 3889 RVAs: noise on Enhanced
        auto RVof = [&](const void* p) -> unsigned long long {
            const uintptr_t r = (uintptr_t)((const uint8_t*)p - g_imageBase);
            return (r < (uintptr_t)g_imageSize) ? (unsigned long long)r : 0xffffffffULL;
        };
        // (a) the fragType type descriptor (the arg slot 42 hands to the constructor)
        const uint64_t* dq = reinterpret_cast<const uint64_t*>(g_imageBase + 0x1bb5478);
        GLOGI("hook: [cdiag] descriptor @0x1bb5478 field0=%016llx (id/size), fn slots:",
              (unsigned long long)dq[0]);
        for (int s = 1; s < 8; ++s)
            GLOGI("hook: [cdiag]   desc[%d] +0x%02x: ptr=%016llx rva=0x%llx", s, s * 8,
                  (unsigned long long)dq[s], RVof((const void*)dq[s]));
        // (b) a resident FragmentStore slot: proof of a constructed object + its streaming handle
        {
            const uint8_t* fs      = g_imageBase + 0x2f5d690;
            const uint8_t* pool    = *reinterpret_cast<const uint8_t* const*>(fs + 0x38);
            const uint8_t* flags   = *reinterpret_cast<const uint8_t* const*>(fs + 0x40);
            const uint32_t count   = *reinterpret_cast<const uint32_t*>(fs + 0x48);
            const uint32_t elem    = *reinterpret_cast<const uint32_t*>(fs + 0x4c);
            const int32_t  baseIdx = *reinterpret_cast<const int32_t*>(fs + 0x8);
            const uint8_t* mgr     = g_imageBase + 0x2ea23d0;
            const uint8_t* mgrArr  = *reinterpret_cast<const uint8_t* const*>(mgr);
            const uint32_t mgrCnt  = *reinterpret_cast<const uint32_t*>(mgr + 8);
            GLOGI("hook: [cdiag] FragmentStore pool=%p flags=%p count=%u elem=0x%x baseIdx=%d",
                  (const void*)pool, (const void*)flags, count, elem, baseIdx);
            int shownR = 0;
            if (pool && flags && elem == 0x18 && count && count < 0x40000) {
                for (uint32_t i = 0; i < count && shownR < 3; ++i) {
                    if (flags[i] & 0x80) continue;             // empty / not-loaded slot
                    const uint8_t* e    = pool + (size_t)i * elem;
                    const uint8_t* objp = (const uint8_t*)(*reinterpret_cast<const uint64_t*>(e));
                    if (!objp || (uintptr_t)objp < 0x10000) continue;
                    const uint8_t* objvt = *reinterpret_cast<const uint8_t* const*>(objp);
                    const uint64_t nameHash = *reinterpret_cast<const uint64_t*>(e + 0x08);
                    const uint32_t handle   = *reinterpret_cast<const uint32_t*>(e + 0x10);
                    const uint32_t sflags   = *reinterpret_cast<const uint32_t*>(e + 0x14);
                    GLOGI("hook: [cdiag] slot[%u] flag=0x%02x obj=%p objVtRVA=0x%llx nameHash+flags=%016llx handle=0x%08x sflags=0x%08x",
                          i, flags[i], (const void*)objp, RVof(objvt),
                          (unsigned long long)nameHash, handle, sflags);
                    const uint64_t* oq = reinterpret_cast<const uint64_t*>(objp);
                    for (int o = 0; o < 0x40; o += 0x20)
                        GLOGI("hook: [cdiag]   obj+0x%02x: %016llx %016llx %016llx %016llx", o,
                              (unsigned long long)oq[o/8], (unsigned long long)oq[o/8+1],
                              (unsigned long long)oq[o/8+2], (unsigned long long)oq[o/8+3]);
                    const long long g = (long long)baseIdx + i;
                    if (mgrArr && g >= 0 && (uint64_t)g < mgrCnt) {
                        const uint32_t* m = reinterpret_cast<const uint32_t*>(mgrArr + (size_t)g * 8);
                        GLOGI("hook: [cdiag]   mgr[base+%u=%lld] handle=0x%08x flags=0x%08x", i, g, m[0], m[1]);
                    }
                    ++shownR;
                }
            }
            if (!shownR) GLOGW("hook: [cdiag] no resident FragmentStore slot found");
        }
        // (c) resource-manager context head + the globals the name path reads
        {
            const uint64_t* rq = reinterpret_cast<const uint64_t*>(g_imageBase + 0x1e2ee90);
            const uint8_t hashChar = *reinterpret_cast<const uint8_t*>(g_imageBase + 0x1e2d744);
            const uint64_t pageSentinel = *reinterpret_cast<const uint64_t*>(g_imageBase + 0x1e2ee00);
            GLOGI("hook: [cdiag] resmgr@0x1e2ee90: %016llx %016llx %016llx %016llx  '#'->0x%02x pageSentinel=%016llx",
                  (unsigned long long)rq[0], (unsigned long long)rq[1],
                  (unsigned long long)rq[2], (unsigned long long)rq[3],
                  hashChar, (unsigned long long)pageSentinel);
        }
    }
    if (resolved == total) {
        GLOGI("hook: new natives: %d/%d resolved (world clean-up, join gate, blips, ped drive, look, "
              "vehicles, combat, camera, pause menu, sync-plan on-foot/vehicle/drive-by/particles)",
              resolved, total);
        return;
    }
    std::string missing;
    for (int i = 0; i < total; ++i) {
        if (ok[i]) continue;
        if (!missing.empty()) missing += ", ";
        missing += kNew[i].name;
    }
    GLOGW("hook: new natives: %d/%d resolved; UNRESOLVED (calls to these do nothing): %s", resolved, total,
          missing.c_str());
}

// ---------------------------------------------------------------------------
// Skip the GTA V landing page -> boot straight into Story Mode.
// ---------------------------------------------------------------------------
// GAGE is built on Story Mode: the engine waits for a player ped, then hijacks
// it (freemode swap, spawn teleport, HUD). But GTA Legacy stops at the landing
// page (the Story/Online selection) and never spawns a ped until the player
// clicks, so nothing loads and the loader watchdog (240 s) gives up. This
// patches the landing-page check so it returns 0 ("do not show the landing
// page"), exactly as GTA's own "Landing Page OFF + On Startup = Story Mode"
// setting does.
//
// Technique and signature from Chiheb-Bacha/StraightToStoryMode (GPL; its
// Legacy landing-page patch is in turn from alloc8or). The signature is a fact
// about GTA5.exe's code; the patch below is GAGE's own. It matches:
//   call <landing_page_check>   ; E8 rel32
//   test al, al                 ; 84 C0
//   mov  eax, [rip+disp]        ; 8B 05 ?? ?? ?? ??
//   jz   ...                    ; 74 ??
//   add  eax, ..                ; 83 C0
// We resolve the E8 call's rel32 target (the check function) and overwrite its
// prologue with `xor eax, eax; ret` so it always reports "no landing page".
//
// Retried by the poller because, like the native registration code, this
// function is Arxan-encrypted and only appears mid-startup; it is applied once,
// well before the landing page (~30-40 s) would be reached. Gated behind
// skip_landing=1 (default off): a code patch on build 3889 not verified live.
static bool g_landingPatched = false;   // patcher finished (patched, or gave up after 120 s)

static bool tryPatchLandingPage() {
    const char* kLandingPageSig = g_build->landingPageSig;  // Legacy value in kProfile3889
    if (!kLandingPageSig)
        return false;  // not derived for this build
    // Not patchable yet: retry on the next poll, giving up (true = done) 120 s after the first miss
    // like the other one-shot patches. `why` is logged every 10 s when `logWaits`, and once on giving up.
    auto notYet = [](const char* why, bool logWaits) {
        const CodeWait w = codeWaitStep(g_landingWait, GetTickCount64());
        if (w == CodeWait::GiveUp)
            GLOGW("gage_mp: landing-page: %s after 120 s; giving up, the game will stop at its landing page", why);
        else if (w == CodeWait::WaitLog && logWaits)
            GLOGW("gage_mp: landing-page: %s — not patching yet", why);
        return w == CodeWait::GiveUp;
    };
    uint8_t* p = findPat(g_imageBase, g_imageSize, kLandingPageSig);
    if (!p)
        return notYet("check signature not found", false);  // code not decrypted yet, or not this build

    // Count matches (findPat returns the first) so the log can tell an
    // unambiguous hit from a risky one. Bounded; runs once at patch time.
    int matches = 0;
    for (const uint8_t* q = g_imageBase; q < g_imageBase + g_imageSize; ) {
        uint8_t* m = findPat(q, (size_t)((g_imageBase + g_imageSize) - q), kLandingPageSig);
        if (!m) break;
        ++matches;
        q = m + 1;
        if (matches > 8) break;
    }

    // Resolve the E8 call target: rel32 at p+1, instruction ends at p+5.
    int32_t rel = 0;
    memcpy(&rel, p + 1, 4);
    uint8_t* target = p + 5 + (int64_t)rel;

    const uintptr_t lo = (uintptr_t)g_imageBase, hi = lo + g_imageSize;
    if ((uintptr_t)target < lo || (uintptr_t)target + 3 > hi) {
        char why[160];
        snprintf(why, sizeof(why), "call target %p out of image [%p,%p) (sig@+0x%llx, %d matches)",
                 (void*)target, (void*)g_imageBase, (void*)(uintptr_t)hi,
                 (unsigned long long)(p - g_imageBase), matches);
        return notYet(why, true);
    }

    static const uint8_t kPatch[3] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax ; ret
    if (memcmp(target, kPatch, 3) == 0) {
        GLOGI("gage_mp: landing-page: check at %p already returns 0 (sig@+0x%llx, %d matches)",
              (void*)target, (unsigned long long)(p - g_imageBase), matches);
        return true;
    }

    // Live code the game thread may be running: write it frozen, like the net patches.
    uint8_t before[3];
    memcpy(before, target, 3);
    if (!frozenWrite(target, kPatch, 3)) {
        char why[96];
        snprintf(why, sizeof(why), "check at %p not writable (busy or protected)", (void*)target);
        return notYet(why, true);
    }

    GLOGI("gage_mp: landing-page SKIPPED: patched check at %p (was %02X %02X %02X) via sig@+0x%llx, %d match%s",
          (void*)target, (unsigned)before[0], (unsigned)before[1], (unsigned)before[2],
          (unsigned long long)(p - g_imageBase), matches, matches == 1 ? "" : "es");
    if (matches != 1)
        GLOGW("gage_mp: landing-page: %d signature matches (expected 1); patched the first — verify Story Mode boots",
              matches);
    return true;
}

// ---------------------------------------------------------------------------
// Enhanced diagnostic dump (unported builds)
// ---------------------------------------------------------------------------
// GTA5_Enhanced.exe is Arxan-encrypted on disk (.text entropy 8.00), so the
// build-1158 native/script/landing signatures + field offsets can only be read
// from the DECRYPTED image inside a running process. This writes that image to
// disk page by page, preserving position so A FILE OFFSET EQUALS AN RVA, plus a
// short .txt with the section table. Captured once per session after the code
// has visibly decrypted (the candidate scan yields many hits). The engine is
// inert on an unported build anyway, so this is the whole purpose of such a run.
// Default: automatic on an unknown build; enh_dump=0 disables, enh_dump=1 forces.
// (g_enhDump is declared with the other globals above, so Init can set it.)

static bool dumpPageReadable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD rw = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & rw) != 0;
}

static void tryDumpDecryptedImage() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!g_imageBase || !g_imageSize) return;

    // %LOCALAPPDATA%\GAGE-MP\dump (best-effort create of both levels).
    wchar_t la[MAX_PATH] = {};
    const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    std::wstring gm = ((ln > 0 && ln < MAX_PATH) ? std::wstring(la) : std::wstring(L".")) + L"\\GAGE-MP";
    std::wstring dir = gm + L"\\dump";
    CreateDirectoryW(gm.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(nullptr), exePath, MAX_PATH);
    const wchar_t* exeBase = exePath;
    for (const wchar_t* s = exePath; *s; ++s) if (*s == L'\\' || *s == L'/') exeBase = s + 1;

    const std::wstring stem = dir + L"\\" + exeBase + L"-" +
        std::to_wstring((unsigned long long)g_imageSize) + L"-" +
        std::to_wstring((unsigned long long)GetTickCount64());
    const std::wstring imgPath = stem + L".img";
    const std::wstring txtPath = stem + L".txt";

    HANDLE f = CreateFileW(imgPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        GLOGW("hook: [dump] could not create %ls (%lu)", imgPath.c_str(), GetLastError());
        return;
    }
    static uint8_t zero[0x10000] = {};
    const uint8_t* p = g_imageBase;
    const uint8_t* const end = g_imageBase + g_imageSize;
    size_t written = 0, committed = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        const uint8_t* rend = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        if (rend <= p) break;  // no forward progress: stop rather than spin
        if (dumpPageReadable(mbi)) {
            DWORD w = 0;
            WriteFile(f, p, (DWORD)(rend - p), &w, nullptr);  // one game region is < 4 GB
            committed += (size_t)(rend - p);
        } else {
            for (const uint8_t* q = p; q < rend; ) {
                const size_t remain = (size_t)(rend - q);
                const size_t chunk = remain < sizeof(zero) ? remain : sizeof(zero);
                DWORD w = 0;
                WriteFile(f, zero, (DWORD)chunk, &w, nullptr);
                q += chunk;
            }
        }
        written += (size_t)(rend - p);
        p = rend;
    }
    CloseHandle(f);

    // Short triage report: section table from the in-memory (decrypted) PE headers.
    HANDLE t = CreateFileW(txtPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
        char buf[512];
        auto emit = [&](int n) {
            if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
            if (n > 0) { DWORD w = 0; WriteFile(t, buf, (DWORD)n, &w, nullptr); }
        };
        emit(snprintf(buf, sizeof(buf),
            "Solar RP decrypted image dump\r\nmodule: %ls\r\nbuild: %s\r\nSizeOfImage: 0x%zx (%zu)\r\n"
            "bytes written: %zu  committed: %zu\r\ncandidates: %d\r\n\r\nsections (name vaddr vsize flags):\r\n",
            exeBase, g_build->label, g_imageSize, g_imageSize, written, committed, (int)g_candidates.size()));
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_imageBase;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_imageBase + dos->e_lfanew);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE) {
            const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                char nm[9] = {}; memcpy(nm, sec[i].Name, 8);
                emit(snprintf(buf, sizeof(buf), "  %-8s 0x%08lx 0x%08lx 0x%08lx\r\n", nm,
                              (unsigned long)sec[i].VirtualAddress, (unsigned long)sec[i].Misc.VirtualSize,
                              (unsigned long)sec[i].Characteristics));
            }
        }
        CloseHandle(t);
    }

    GLOGW("hook: [dump] wrote DECRYPTED image %ls (%zu bytes, %zu committed) + %ls. "
          "Send BOTH files back: a file offset in the .img equals an RVA.",
          imgPath.c_str(), written, committed, txtPath.c_str());
}

// ---------------------------------------------------------------------------
// Background native-table readiness poller.
// ---------------------------------------------------------------------------
// Frame() barely runs during the game's loading screen (1 frame in 4+ s),
// so relying on it for retries means the table isn't validated until long
// after it's actually populated. This thread polls every 75 ms on a worker
// thread — VirtualQuery protects every dereference, so it's safe even while
// the game is still initialising.
//
// The thread is started by Init, polls until the table validates (never gives
// up), then installs the script tick and exits.

// One-shot code patches whose target code Arxan decrypts mid-startup. Tried every poll, while the
// script tick installs, and after the native table validates until each one is done: the poller
// used to exit at validation, silently dropping a patch whose code had not decrypted yet. "Done"
// includes giving up 120 s after the first wait (codeWaitStep), so the trailing loop always ends.
static void pollCodePatches() {
    // Stop every stock script thread from executing (no_sp_scripts=1, FiveM JustNoScript) as
    // soon as GtaThread::Tick decrypts -- ideally before startup's first tick, so first.
    if (g_noSpScripts && !g_noSpInstalled && noSpInstall())
        g_noSpInstalled = true;
    noSpVerifyTick();   // Enhanced: once the thread array exists, check the hook against the live vtable

    // Boot straight to Story Mode (opt-in): as soon as the landing-page
    // check's Arxan-decrypted code appears, patch it once — before the
    // landing page would stop the game at its menu. Off unless skip_landing=1.
    if (g_skipLanding && !g_landingPatched && tryPatchLandingPage())
        g_landingPatched = true;

    // Install the local session-entry host stubs once the network code has
    // decrypted (net_host=1). Reversible on Unhook; off unless net_host=1.
    if (g_netHost && !g_netHostInstalled && netHostInstall())
        g_netHostInstalled = true;

    // pack_enh=1: Round A of the car-pack port, READ-ONLY (pack_enh.h). Its patterns sit in code Arxan decrypts late, so it
    // shares this poller and codeWaitStep's 120 s budget; it throttles its own scans (one image pass per 1.5 s).
    if (g_packEnh && !g_packEnhDone) {
        const ULONGLONG now = GetTickCount64();
        const CodeWait w = codeWaitStep(g_packEnhWait, now);
        if (gage::game::content::PackEnhStep(now, w == CodeWait::WaitLog, w == CodeWait::GiveUp))
            g_packEnhDone = true;
    }
}

static bool codePatchesPending() {
    return (g_noSpScripts && !g_noSpInstalled) ||
           (g_noSpActive && !g_noSpVerified) ||
           (g_skipLanding && !g_landingPatched && g_build->landingPageSig) ||
           (g_netHost && !g_netHostInstalled) ||
           (g_packEnh && !g_packEnhDone);
}

DWORD WINAPI nativeTablePollerThread(LPVOID) {
    // Natives register once the game gets that far (story mode can take well over
    // a minute from the landing page), so never give up: every 75 ms for the first
    // minute, then every second (a rescan costs ~30 ms).
    const int fastAttempts = 800;  // 800 × 75 ms = 60 s
    for (int attempt = 1;; ++attempt) {
        if (g_nativeTableReady)
            break;

        if (attempt == fastAttempts + 1)
            GLOGW("hook: [poller] no native table validated after 60 s (lookup signature %s, %d candidates); "
                  "still polling every 1 s", g_sigFn ? "found" : "NOT found", (int)g_candidates.size());
        if (attempt == fastAttempts + 1 || attempt == fastAttempts + 121) {  // 60 s and 3 min
            GLOGW("hook: [poller] game lookup: table %d/256 slots filled, %s; last PLAYER_PED_ID -> %p, "
                  "GET_ENTITY_COORDS -> %p", g_tableFilled, g_lookupCalled ? "called" : "never called",
                  (void*)g_lastA, (void*)g_lastB);
            if (g_sigTable) {
                logBucketHead(g_sigTable, buildHash(native::PLAYER_PED_ID));
                dumpNativeTable(g_sigTable, NodeLayout::AddrXor);
            }
            logCandidateStats();
            // If the bucketed walk never validated and this build has an
            // InitNativeTables resolver (Enhanced), report whether it is present,
            // so a failed run says plainly that the scrProgram-link path is the
            // next thing to wire rather than leaving it a mystery.
            if (g_build->initNativeTablesSig) {
                uint8_t* in = findPat(g_imageBase, g_imageSize, g_build->initNativeTablesSig);
                GLOGW("hook: [poller] natives still unresolved; InitNativeTables resolver %s",
                      in ? "found and attempted each poll, but the game has not registered natives (default "
                           "handler only) — likely still pre-session"
                         : "NOT found by its signature in the decrypted image");
            }
        }
        Sleep(attempt <= fastAttempts ? 75 : 1000);

        pollCodePatches();

        // Preferred: the game's own get_native_handler (no node decoding).
        if (tryGameLookup(attempt))
            break;

        // Enhanced has no walkable bucket table — resolve through the game's own
        // InitNativeTables instead (null initNativeTablesSig => skipped on Legacy).
        if (tryInitNativeTablesResolve())
            break;

        // GTA V decrypts parts of its .text during startup, so the native
        // registration code can be invisible to a scan done at Init time.
        // Re-scan each poll to pick up newly-decrypted code, then validate.
        size_t before = g_candidates.size();
        collectNativeTableCandidates(g_imageBase, g_imageSize);
        if (g_candidates.size() != before)
            GLOGI("hook: [poller] rescan attempt %d -> %d candidates",
                  attempt, (int)g_candidates.size());

        // Unported build: once the code has decrypted (candidates appear, or
        // the 60 s fast window elapsed with any hit), write the decrypted image
        // for offline signature derivation. Once-only; polling then continues.
        if (g_enhDump && (g_candidates.size() >= 64 ||
                          (attempt >= fastAttempts && !g_candidates.empty())))
            tryDumpDecryptedImage();

        if (g_candidates.empty())
            continue;

        NodeLayout layout = NodeLayout::Plain;
        uint8_t** winner = validateCandidates(layout);
        if (winner) {
            g_nativeTable = winner;
            g_nodeLayout = layout;  // before the atomic store that publishes the table
            nativeCacheClear();  // drop any stale negative lookups
            g_nativeTableReady = true;
            GLOGI("hook: [poller] native table VALIDATED on attempt %d at %p, layout %s "
                  "(PLAYER_PED_ID + GET_ENTITY_COORDS both resolved)", attempt, winner, layoutName(layout));
            break;
        }
        // No candidate validates yet — natives not registered this frame. Retry.
    }
    // Natives work: now move the game work into GTA's script tick.
    installScriptTickWhenReady();
    // Code patches whose target had not decrypted when the table validated keep waiting here.
    for (int attempt = 1; codePatchesPending(); ++attempt) {
        Sleep(attempt <= fastAttempts ? 75 : 1000);
        pollCodePatches();
    }
    // After the tick install (time-sensitive); only waits when a new native is missing.
    if (g_nativeTableReady)
        logNewNativeResolution();
    return 0;
}

// Called from engine.cpp Frame() as a fast-path check (no Sleep, no scan).
// Once the poller thread sets g_nativeTableReady, this is a no-op.
void gage::game::retryNativeTableOnce() {
    // Nothing to do — the poller thread handles it.
    // This function exists so engine.cpp doesn't need to change.
}

void* gage::game::GetSwapChain() {
    return gage::presenthook::SwapChain();
}
