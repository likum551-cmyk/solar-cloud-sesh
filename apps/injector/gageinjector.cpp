// GAGE-MP stage-2 injector.
//
// Injected by gage_v.exe into the game / anti-cheat launcher process. From
// here we locate the actual game process and load gage_loader.dll into it.
// No exports — DllMain is the entry, exactly like the original vinjector.dll.

#include "common/common.h"
#include "common/inject.h"
#include "common/registry.h"
#include "common/logger.h"
#include "common/crashscan.h"

#include <windows.h> // AddVectoredExceptionHandler / RemoveVectoredExceptionHandler (kernel32)
#include <dbghelp.h> // MINIDUMP_EXCEPTION_INFORMATION (types only; dbghelp.dll is loaded on demand)
#include <cstddef>   // offsetof
#include <cstring>   // memcpy

// dbghelp.h declares MINIDUMP_EXCEPTION_INFORMATION under 4-byte packing, so on
// x64 ExceptionPointers sits at offset 4, not 8. A hand-rolled struct with default
// alignment put the pointer at 8; dbghelp then read a torn pointer and every
// in-process dump failed with ERROR_NOACCESS (0x800703E6).
static_assert(offsetof(MINIDUMP_EXCEPTION_INFORMATION, ExceptionPointers) == 4,
              "MINIDUMP_EXCEPTION_INFORMATION must use dbghelp.h's 4-byte packing");

using namespace gage;

namespace {

    // ---- In-game crash capture -------------------------------------------------
    // gageinjector is the first code that runs inside the game and never leaves.
    // A high-priority vectored exception handler therefore sees any fatal fault
    // (the 0xC0000005 we see ~6 s after launch) before SEH does, regardless of
    // which thread takes it. On a fatal code it logs a signature (code, faulting
    // module + RVA, read/write target) to gage.log and writes a minidump via
    // dbghelp, turning a bare "exited, code 0xC0000005" into a readable stack.
    // dbghelp is loaded on demand, so the DLL stays dependency-free until a crash.
    static PVOID g_veh = nullptr;

    static const char* ExcName(DWORD code)
    {
        switch (code) {
            case 0xC0000005: return "access violation";
            case 0xC0000006: return "in-page error";
            case 0xC000001D: return "illegal instruction";
            case 0xC000001E: return "non-continuable exception";
            case 0xC000001F: return "breakpoint";
            case 0xC000008D: return "datatype misalignment";
            case 0xC00000FD: return "stack overflow";
            case 0xC0000374: return "heap corruption";
            case 0xC0000409: return "stack buffer overrun / fast fail";
            case 0xC0000602: return "fail-fast exception";
            case 0xC0150014: return "corrupt activation-context stack";
            case 0x80000003: return "breakpoint (user-mode)";
            case 0xE06D7363: return "C++ exception (unhandled)";
            default:         return "unknown";
        }
    }

    static void WriteMiniDump(const EXCEPTION_POINTERS* ep)
    {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (!dbghelp)
        {
            GLOGW("gageinjector: dbghelp.dll missing (err=%lu); no minidump",
                (unsigned long)GetLastError());
            return;
        }
        using MDWFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);
        auto fn = (MDWFn)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump");
        if (!fn)
        {
            GLOGW("gageinjector: MiniDumpWriteDump not found");
            FreeLibrary(dbghelp);
            return;
        }

        // Install dir first (where the game's files live), then the per-user
        // fallback the logger uses — one of the two is always writable.
        std::vector<std::wstring> cands;
        cands.push_back(ExeDir() + L"game_crash.dmp");
        wchar_t base[260] = {0};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 260);
        if (n > 0 && n < 260)
        {
            std::wstring d(base, n);
            while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
                d.pop_back();
            d += L"\\GAGE-MP";
            CreateDirectoryW(d.c_str(), nullptr); // already-exists is fine
            cands.push_back(d + L"\\game_crash.dmp");
        }

        bool ok = false;
        for (auto& path : cands)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                continue;
            // Small, crash-appropriate dump: indirectly-referenced memory (0x2, so
            // the faulting thread's stack frames are captured) + loaded modules
            // (0x20, for symbol/address resolution).
            // When we fault, also pass the exception record + the faulting thread's
            // context so the dump carries an exception stream + the crash thread.
            const DWORD flags = 0x2 | 0x20;
            if (ep)
            {
                // We are running on the faulting thread inside its VEH, so the
                // current thread id IS the faulting thread.
                MINIDUMP_EXCEPTION_INFORMATION mdie{};
                mdie.ThreadId = (DWORD)GetCurrentThreadId();
                mdie.ExceptionPointers = const_cast<EXCEPTION_POINTERS*>(ep);
                mdie.ClientPointers = FALSE;
                ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, flags, &mdie, nullptr, nullptr);
            }
            else
                ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, flags, nullptr, nullptr, nullptr);
            CloseHandle(f);
            if (ok)
            {
                GLOGW("gageinjector: minidump -> %s", Narrow(path).c_str());
                break;
            }
        }
        if (!ok)
            GLOGW("gageinjector: minidump write failed (err=%lu)",
                (unsigned long)GetLastError());
        FreeLibrary(dbghelp);
    }

    // Only a genuine, fatal fault is ours to report and dump. Benign,
    // continuable codes must pass straight through -- above all
    // DBG_PRINTEXCEPTION_C (0x40010006) and its wide form
    // DBG_PRINTEXCEPTION_WIDE_C (0x4001000A), which OutputDebugString raises when
    // no debugger is attached and which a VEH sees before OutputDebugString's own
    // handler swallows them. The game emits these constantly during startup.
    static bool IsFatalExceptionCode(DWORD code)
    {
        switch (code) {
            case 0xC0000005: case 0xC0000006: case 0xC000001D:
            case 0xC000001F: case 0xC000008D: case 0xC00000FD:
            case 0xC0000374: case 0xC0000409: case 0xC0000602:
            case 0xC0150014: case 0x80000003: return true;
            default: return false;
        }
    }

    static LONG NTAPI CrashFilter(EXCEPTION_POINTERS* ep)
    {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const DWORD code = er->ExceptionCode;
        // Decline every non-fatal exception BEFORE taking the re-entrancy guard.
        // The old order took the guard on every entry and released it only by
        // terminating, so a second benign exception on the same thread (two
        // OutputDebugString calls during startup) looked like a re-entrant dump
        // fault and killed the game with the benign code -- the 0x40010006
        // 1-second exit on the first BattlEye-free boot.
        if (!IsFatalExceptionCode(code))
            return EXCEPTION_CONTINUE_SEARCH;

        // Owner = id of the thread that entered first (0 = free).
        static volatile LONG owner = 0;
        const LONG self = (LONG)GetCurrentThreadId();
        const LONG prev = InterlockedCompareExchange(&owner, self, 0);
        if (prev == self)
        {
            // The dump path itself faulted on this thread: end the process now.
            TerminateProcess(GetCurrentProcess(), code);
            for (;;) Sleep(1000);
        }
        if (prev != 0)
            Sleep(INFINITE); // another thread is reporting; let it finish

        char where[300];
        crash::DescribeAddr((DWORD64)(uintptr_t)er->ExceptionAddress, where, sizeof(where));
        GLOGW("gageinjector: CRASH code=0x%08lX (%s) at %s thread=%lu",
            (unsigned long)code, ExcName(code), where, (unsigned long)self);
        if ((code == 0xC0000005 || code == 0xC0000006) && er->NumberParameters >= 2)
            GLOGW("gageinjector:   %s 0x%llx",
                er->ExceptionInformation[0] == 0 ? "reading"
                : er->ExceptionInformation[0] == 1 ? "writing" : "accessing",
                (unsigned long long)er->ExceptionInformation[1]);

        // Faulting thread's full register state + the faulting instruction bytes,
        // straight from the VEH — so gage.log alone is diagnostic (no dmp needed).
        if (ep->ContextRecord)
        {
            CONTEXT* C = (CONTEXT*)ep->ContextRecord;
            GLOGW("gageinjector:   regs RSP=%p RIP=%p RAX=%p RCX=%p RDX=%p RBX=%p RBP=%p RSI=%p RDI=%p R8=%p R9=%p R10=%p R11=%p R12=%p R13=%p R14=%p R15=%p",
                (void*)C->Rsp, (void*)C->Rip, (void*)C->Rax, (void*)C->Rcx, (void*)C->Rdx,
                (void*)C->Rbx, (void*)C->Rbp, (void*)C->Rsi, (void*)C->Rdi, (void*)C->R8,
                (void*)C->R9, (void*)C->R10, (void*)C->R11, (void*)C->R12, (void*)C->R13,
                (void*)C->R14, (void*)C->R15);
            // The fault address may itself be unmapped (a call through a null or stale pointer: an EXECUTE violation). A plain memcpy would fault
            // again on this thread, and the re-entry guard above would terminate before the stack scan and the dump. Copy only what VirtualQuery
            // says is committed and readable (the same rule as gage_loader.cpp's Readable).
            BYTE insn[16];
            int got = 0;
            {
                const uintptr_t at = (uintptr_t)er->ExceptionAddress;
                MEMORY_BASIC_INFORMATION mbi{};
                if (at >= 0x10000 && VirtualQuery((const void*)at, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                {
                    const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                    got = (int)(end - at < 16 ? end - at : 16);
                    memcpy(insn, (const void*)at, (size_t)got);
                }
            }
            char hex[56] = "unreadable";
            int p = 0;
            for (int i = 0; i < got; i++)
                p += snprintf(hex + p, sizeof(hex) - p, "%02x ", insn[i]);
            if (p > 0) hex[p - 1] = '\0';
            GLOGW("gageinjector:   fault bytes: %s", hex);
            crash::LogStackScan("gageinjector:", C);
        }

        WriteMiniDump(ep);

        if (g_veh)
        {
            RemoveVectoredExceptionHandler(g_veh);
            g_veh = nullptr;
        }
        TerminateProcess(GetCurrentProcess(), code);
        for (;;) Sleep(1000);
    }

    DWORD ReadPidValue(const wchar_t* key)
    {
        if (!reg::Has(key))
            return 0;
        std::wstring v = reg::Get(key);
        DWORD pid = 0;
        for (wchar_t c : v)
            if (c >= L'0' && c <= L'9')
                pid = pid * 10 + (DWORD)(c - L'0');
        return pid;
    }

    void Run()
    {
        GLOGI("gageinjector: stage-2 active (pid %lu)", (unsigned long)GetCurrentProcessId());

        // Crash capture first: it must be in before the gage_loader / gage_mp.dll
        // loads, which is where a bad engine build takes the game down
        // (0xC0000005, during LoadLibraryW of one of them).
        if (!g_veh)
        {
            g_veh = AddVectoredExceptionHandler(1, CrashFilter); // 1 = highest priority
            GLOGI("gageinjector: crash handler %s", g_veh ? "installed" : "FAILED");
        }

        std::wstring dir = reg::Get(L"install_dir");
        if (dir.empty())
            dir = ExeDir();
        std::wstring loaderPath = dir + L"gage_loader.dll";

        // Figure out where to deliver the loader.
        DWORD targetPid = ReadPidValue(L"inject_target");
        if (targetPid == 0)
        {
            // The first running game image. (Not `a || b`: that yields 1, not a pid.)
            // BattlEye images are never targets.
            for (const wchar_t* image : { L"GTA5.exe", L"GTA5_Enhanced.exe" })
                if ((targetPid = inject::FindProcess(image)) != 0)
                    break;
        }

        bool ok = false;
        if (targetPid == 0 || targetPid == GetCurrentProcessId())
        {
            // Self-test / no separate game: load the loader in this process.
            GLOGI("gageinjector: loading loader in-process -> %s",
                Narrow(loaderPath).c_str());
            ok = (LoadLibraryW(loaderPath.c_str()) != nullptr);
        }
        else
        {
            HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, targetPid);
            if (h)
            {
                DWORD err = 0;
                ok = inject::InjectDll(h, loaderPath, 30000, &err);
                CloseHandle(h);
                GLOGI("gageinjector: injected loader into pid %lu ok=%d err=0x%08X",
                    (unsigned long)targetPid, (int)ok, (unsigned)err);
            }
            else
            {
                GLOGE("gageinjector: OpenProcess(%lu) failed: %s",
                    (unsigned long)targetPid, Narrow(FormatError(GetLastError())).c_str());
            }
        }

        GLOGI("gageinjector: done ok=%d", (int)ok);
    }

}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Off the loader lock; the DLL is permanent (no exports, never unloaded).
        HANDLE t = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)Run, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
