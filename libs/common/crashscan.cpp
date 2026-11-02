#include "crashscan.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

namespace gage::crash {

    namespace {

        bool Query(const void* p, MEMORY_BASIC_INFORMATION* mbi)
        {
            if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, mbi, sizeof(*mbi)))
                return false;
            return mbi->State == MEM_COMMIT && !(mbi->Protect & (PAGE_GUARD | PAGE_NOACCESS));
        }

        bool IsReadable(const void* p)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!Query(p, &mbi))
                return false;
            return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                 | PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        // Committed, executable memory at `p` (image or private: a packer's
        // unpacked code often lives in VirtualAlloc'd pages).
        bool IsExecutable(const void* p, bool* isImage)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!Query(p, &mbi))
                return false;
            const DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                          | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & x))
                return false;
            *isImage = (mbi.Type == MEM_IMAGE);
            return true;
        }

        // True when the bytes before `ra` end in an x64 CALL, i.e. `ra` is a real
        // return address rather than a stray code pointer on the stack.
        bool FollowsCall(const BYTE* ra)
        {
            if (!IsReadable(ra - 7) || !IsReadable(ra - 1))
                return false;
            if (ra[-5] == 0xE8) return true;                                     // call rel32
            if (ra[-6] == 0xFF && (ra[-5] == 0x15 || (ra[-5] >= 0x90 && ra[-5] <= 0x97)))
                return true;                                                     // call [rip+d32] / [reg+d32]
            if (ra[-7] == 0xFF && ra[-6] == 0x94) return true;                   // call [sib+d32]
            if (ra[-2] == 0xFF && ra[-1] >= 0xD0 && ra[-1] <= 0xD7) return true; // call reg
            if (ra[-3] == 0xFF && ra[-2] >= 0x50 && ra[-2] <= 0x57) return true; // call [reg+d8]
            if (ra[-4] == 0xFF && ra[-3] == 0x54) return true;                   // call [sib+d8]
            if (ra[-3] == 0xFF && ra[-2] == 0x14) return true;                   // call [sib]
            if (ra[-2] == 0xFF && ra[-1] >= 0x10 && ra[-1] <= 0x17) return true; // call [reg]
            return false;
        }

    }

    void DescribeAddr(DWORD64 addr, char* out, size_t cap)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)addr, &m) && m)
        {
            char full[260] = "";
            GetModuleFileNameA(m, full, sizeof(full));
            const char* b = strrchr(full, '\\');
            const char* base = b ? b + 1 : full;
            snprintf(out, cap, "%s+0x%llx", base[0] ? base : "?",
                     (unsigned long long)(addr - (DWORD64)(uintptr_t)m));
        }
        else
            snprintf(out, cap, "<unmapped>0x%llx", (unsigned long long)addr);
    }

    void LogStackScan(const char* tag, const CONTEXT* C)
    {
        const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
        const uintptr_t top = (uintptr_t)tib->StackBase;
        const uintptr_t rsp = (uintptr_t)C->Rsp;
        if (rsp < (uintptr_t)tib->StackLimit || rsp + 8 > top)
        {
            GLOGW("%s   stack scan skipped: RSP %p outside this thread's stack [%p, %p)",
                  tag, (void*)rsp, tib->StackLimit, tib->StackBase);
            return;
        }
        char where[300];
        const uintptr_t ret0 = *(const uintptr_t*)rsp;
        DescribeAddr((DWORD64)ret0, where, sizeof(where));
        GLOGW("%s   [RSP] = %p (%s)%s", tag, (void*)ret0, where,
              FollowsCall((const BYTE*)ret0) ? " after a CALL: the faulting function's caller" : "");

        const int kMaxQwords = 2048; // 16 KB of stack
        const int kMaxHits   = 24;
        int hits = 0;
        for (int i = 1; i < kMaxQwords && hits < kMaxHits; i++)
        {
            const uintptr_t slot = rsp + (uintptr_t)i * 8;
            if (slot + 8 > top)
                break;
            const uintptr_t v = *(const uintptr_t*)slot;
            bool image = false;
            if (!IsExecutable((const void*)v, &image))
                continue;
            const bool call = FollowsCall((const BYTE*)v);
            DescribeAddr((DWORD64)v, where, sizeof(where));
            GLOGW("%s   stack[RSP+0x%03x] %p %s%s%s", tag, i * 8, (void*)v, where,
                  image ? "" : " (private exec memory)", call ? " <- ret after CALL" : "");
            hits++;
        }
        GLOGW("%s   stack scan: %d code pointer(s) in the first %d bytes above RSP",
              tag, hits, kMaxQwords * 8);
    }

}
