#pragma once
#include <windows.h>
#include <cstddef>

// Crash diagnostics shared by the in-game DLLs (gageinjector's crash VEH and
// gage_loader's DllMain crash-catch). Allocation-free and read-only: every
// memory access is checked with VirtualQuery first, because a fault taken inside
// a vectored exception handler re-enters that handler.
namespace gage::crash {

    // "module+0xRVA" for an address, or "<unmapped>0x..." outside any module.
    void DescribeAddr(DWORD64 addr, char* out, size_t cap);

    // Log [RSP] (the exact caller when the faulting function is a leaf), then
    // scan the current thread's stack above RSP for pointers into executable
    // memory, tagging the ones that follow a CALL. Packed code has no unwind
    // info, so this is what names the caller there. Call it on the faulting
    // thread (from its VEH): the stack bounds come from the current TEB.
    // `tag` prefixes every line ("gageinjector", "gage_loader: [main]").
    void LogStackScan(const char* tag, const CONTEXT* ctx);

}
