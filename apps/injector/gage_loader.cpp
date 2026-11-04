// GAGE-MP loader.
//
// Loaded (in-process) into the game process by gageinjector.dll. Sets the DLL
// search directory to the GAGE-MP install dir and loads the engine
// (gage_mp.dll).
//
// The engine is loaded on the game's MAIN thread: a DllMain that touches
// thread-local state or the game's own initialisation can fault when it runs
// on a worker thread, and the main thread is where the game itself loads its
// modules. The loader keeps a crash-catch around that load (see below) and
// logs what faulted, so a bad engine build costs one log line instead of the
// game process.

#include "common/common.h"
#include "common/registry.h"
#include "common/logger.h"
#include "common/crashscan.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring> // strrchr (DescribeFaultAddr)

using namespace gage;

namespace {

    using QWORD = uint64_t;

    // Build tag logged on the very first loader line so a captured log always
    // identifies which gage_loader.dll produced it (independent of the GUI exe
    // version). Bump alongside every loader change.
    static const char* kLoaderBuild = "0.1.32-gage-only";

    // ---- DllMain crash-catch (main thread) ----------------------------------
    // A DllMain that faults (an access violation, say) on the main thread is
    // fatal when left unhandled: the game process dies with no log of why. So
    // for the duration of a main-thread load we run a highest-priority VEH. If
    // the main thread (g_mainTid) takes a fatal fault while the trampoline is
    // still running (g_loadActive), we redirect its RIP to the trampoline's
    // RESTORE path (g_restorePath) — the `mov rsp,[origRsp]; pop 15;
    // jmp [origRip]` sequence — so it unwinds cleanly back to the game with every
    // GPR restored. We flag the load as crashed (statusSlot sentinel) so Run()
    // reports it instead of re-running the crashing DllMain.
    //
    // Ordering vs gageinjector's VEH: both are priority 1; within a priority the
    // most-recently-added handler runs first, and gage_loader is loaded after
    // gageinjector, so this handler pre-empts the injector's log+minidump+terminate.
    static volatile LONG g_loadActive = 0;
    static volatile LONG g_handled    = 0;
    static DWORD  g_mainTid     = 0;
    static DWORD* g_doneSlot    = nullptr;
    static DWORD* g_statusSlot  = nullptr;
    static BYTE*  g_restorePath = nullptr;
    static PVOID  g_loadVeh     = nullptr;
    // Set when the main thread cannot take another load: LdrLoadDll refused it
    // (STATUS_INVALID_THREAD) or a hijack blob is still pending on it.
    static bool   g_mainThreadUnusable = false;

    static bool IsFatalCode(DWORD c)
    {
        switch (c) {
            case 0xC0000005: // access violation
            case 0xC0000006: // in-page error
            case 0xC000001D: // illegal instruction
            case 0xC000001F: // breakpoint (assert / __debugbreak)
            case 0xC000008D: // datatype misalignment
            case 0xC00000FD: // stack overflow
            case 0xC0000374: // heap corruption
            case 0xC0000409: // stack buffer overrun / fast fail
            case 0xC0000602: // fail-fast (ud2 __fastfail)
            case 0xC0150014: // STATUS_SXS_CORRUPT_ACTIVATION_STACK
            case 0x80000003: // breakpoint (user-mode __debugbreak)
                return true;
            default:
                return false;
        }
    }

    // "module+0xRVA" for a faulting address, without allocating — so a gage.log
    // alone names the DLL whose DllMain/code faulted (cross-ref with the dmp).
    static void DescribeFaultAddr(DWORD64 addr, char* out, size_t cap)
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

    // MSVC C++ exception code ('msc' | 0xE0000000), raised by _CxxThrowException.
    static const DWORD kMsvcCppException = 0xE06D7363;
    // Cap on logged C++ throws: a packed init may throw-and-catch internally many
    // times; the first few name the culprit, the rest are log spam.
    static const LONG kMaxLoggedThrows = 8;
    static volatile LONG g_throwsLogged = 0;

    // True if [p, p+n) is committed, readable memory. Uses VirtualQuery, never a
    // probing read: a fault taken inside this VEH would re-enter it as "fatal".
    static bool Readable(const void* p, size_t n)
    {
        if ((uintptr_t)p < 0x10000) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        return (uintptr_t)p + n <= end;
    }

    // Copy a NUL-terminated narrow string at `s` into `out` (bounded, each chunk
    // checked readable). Returns false if the first byte is unreadable.
    static bool CopyCStr(const char* s, char* out, size_t cap)
    {
        size_t n = 0;
        while (n + 1 < cap)
        {
            if (!Readable(s + n, 1)) break;
            const char c = s[n];
            if (!c) break;
            out[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        out[n] = 0;
        return n > 0;
    }

    // Decode an MSVC x64 C++ throw record (code 0xE06D7363, 4 params):
    //   [0] magic 0x19930520 (or ...21/...22/0x01994000)
    //   [1] thrown object*          [2] _ThrowInfo*        [3] throw image base
    // x64 _ThrowInfo is { DWORD attributes; int pmfnUnwind; int pForwardCompat;
    // int pCatchableTypeArray; } — the int fields are RVAs from [3].
    // _CatchableTypeArray is { int n; int rvaCatchableType[n]; } listing the thrown
    // type first, then each base. _CatchableType is { DWORD props; int pType; ... }
    // and pType is the RVA of a TypeDescriptor { void* vft; void* spare; char name[] }
    // whose name is decorated (".?AVfilesystem_error@filesystem@std@@").
    // When std::exception is among the bases, the object is laid out
    // { vftable; const char* _What; bool _DoFree; }, so _What at +8 is what().
    static void DescribeThrownType(const EXCEPTION_RECORD* er)
    {
        if (!er || er->NumberParameters < 4)
        {
            GLOGW("gage_loader: [main]   threw TYPE: <record has %lu params, expected 4>",
                  er ? (unsigned long)er->NumberParameters : 0UL);
            return;
        }
        const ULONG_PTR magic = er->ExceptionInformation[0];
        const BYTE* obj  = (const BYTE*)er->ExceptionInformation[1];
        const BYTE* ti   = (const BYTE*)er->ExceptionInformation[2];
        const BYTE* base = (const BYTE*)er->ExceptionInformation[3];
        char where[300] = "";
        DescribeFaultAddr((DWORD64)(uintptr_t)er->ExceptionAddress, where, sizeof(where));
        GLOGW("gage_loader: [main]   throw raised at %s (magic=0x%llx obj=%p throwinfo=%p imagebase=%p)",
              where, (unsigned long long)magic, (const void*)obj, (const void*)ti,
              (const void*)base);
        if (!Readable(ti, 16) || !base)
        {
            GLOGW("gage_loader: [main]   threw TYPE: <throwinfo unreadable>");
            return;
        }
        const int rvaCta = *(const int*)(ti + 12);
        const BYTE* cta = base + rvaCta;
        if (!Readable(cta, 4))
        {
            GLOGW("gage_loader: [main]   threw TYPE: <catchable type array unreadable rva=0x%x>",
                  rvaCta);
            return;
        }
        int count = *(const int*)cta;
        if (count < 0 || count > 32) count = 0;
        bool isStdException = false;
        for (int i = 0; i < count; i++)
        {
            const BYTE* slot = cta + 4 + 4 * i;
            if (!Readable(slot, 4)) break;
            const BYTE* ct = base + *(const int*)slot;
            if (!Readable(ct, 8)) break;
            const BYTE* td = base + *(const int*)(ct + 4);
            char name[200] = "";
            if (!Readable(td, 16) || !CopyCStr((const char*)(td + 16), name, sizeof(name)))
                snprintf(name, sizeof(name), "<unreadable>");
            if (strcmp(name, ".?AVexception@std@@") == 0)
                isStdException = true;
            GLOGW("gage_loader: [main]   threw TYPE%s: %s", i == 0 ? "" : " (base)", name);
        }
        if (isStdException && Readable(obj, 16))
        {
            const char* what = *(const char* const*)(obj + 8);
            char msg[400] = "";
            if (CopyCStr(what, msg, sizeof(msg)))
                GLOGW("gage_loader: [main]   what(): %s", msg);
            else
                GLOGW("gage_loader: [main]   what(): <unreadable %p>", (const void*)what);
        }
    }

    static LONG NTAPI LoadCrashFilter(EXCEPTION_POINTERS* ep)
    {
        if (!g_loadActive)
            return EXCEPTION_CONTINUE_SEARCH;
        if ((DWORD)GetCurrentThreadId() != g_mainTid)
            return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const DWORD code = er->ExceptionCode;

        // The C++ throw (0xE06D7363) is raised FIRST, inside the DllMain, before it
        // escapes to the C boundary. Capture the exception TYPE here (the single
        // most useful line in the log: it names exactly what the DllMain threw —
        // filesystem_error, invalid_argument, an environment-check exception, ...).
        // Then CONTINUE_SEARCH: the throw escapes the C boundary (DllMain), the CRT
        // calls CrashForExceptionInNonABICompliantCodeRange, which faults (an AV
        // reading 0xFFFFFFFFFFFFFFFF or a 0x80000003 breakpoint) — that fault is
        // caught by the fatal-fault branch below and redirected to the restore path.
        // A throw that the DllMain catches itself also lands here; only the last one
        // logged before the "DllMain fatal" line is the one that escaped.
        if (code == kMsvcCppException && !g_handled)
        {
            const LONG nth = InterlockedIncrement(&g_throwsLogged);
            if (nth <= kMaxLoggedThrows)
            {
                GLOGW("gage_loader: [main] C++ exception 0xE06D7363 raised in DllMain (#%ld)",
                      nth);
                DescribeThrownType(er);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (g_handled)
            return EXCEPTION_CONTINUE_SEARCH;
        if (!IsFatalCode(code))
            return EXCEPTION_CONTINUE_SEARCH;
        InterlockedExchange(&g_handled, 1);
        GLOGW("gage_loader: [main] DllMain fatal 0x%08lX on main thread; redirecting to restore path",
              (unsigned long)code);
        // Where it faulted, so gage.log alone is diagnostic: module+RVA names the DLL
        // whose DllMain/code faulted; cross-ref against the code blob / dmp if needed.
        char where[300] = "";
        DescribeFaultAddr((DWORD64)(uintptr_t)er->ExceptionAddress, where, sizeof(where));
        GLOGW("gage_loader: [main]   fault at %s info[0]=0x%llx info[1]=0x%llx (params=%lu)",
              where,
              (unsigned long long)(er->NumberParameters >= 1 ? er->ExceptionInformation[0] : 0),
              (unsigned long long)(er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0),
              (unsigned long)er->NumberParameters);
        if (ep->ContextRecord) {
            CONTEXT* C = (CONTEXT*)ep->ContextRecord;
            GLOGW("gage_loader: [main]   regs RSP=%p RIP=%p RAX=%p RCX=%p RDX=%p",
                  (void*)C->Rsp, (void*)C->Rip, (void*)C->Rax, (void*)C->Rcx, (void*)C->Rdx);
            // Names the caller (packed code has no unwind info): this VEH runs before
            // gageinjector's, which never sees a fault we redirect.
            crash::LogStackScan("gage_loader: [main]", C);
        }
        if (g_statusSlot) *g_statusSlot = 0x8BADF00D; // sentinel: "DllMain crashed"
        if (g_doneSlot)   *g_doneSlot   = 1;           // stop the Run() polling loop
        CONTEXT* C = (CONTEXT*)ep->ContextRecord;
        if (C && g_restorePath)
            C->Rip = (DWORD64)(uintptr_t)g_restorePath;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // FileExists for the engine DLL before we commit to a load, so a missing
    // file is logged as such instead of as a loader error.
    bool DllPresent(const std::wstring& path)
    {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    // ntdll loader entry point. Calling LdrLoadDll directly skips the
    // kernelbase/kernel32 LoadLibrary* wrappers that the Steam in-game overlay
    // (gameoverlayrenderer64.dll) hooks — the reason protected modules faulted
    // when the hooked loader sat in the chain.
    struct UnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        wchar_t* Buffer;
    };
    using LdrLoadDllFn = LONG(WINAPI*)(void*, unsigned long*, void*, HMODULE*);

    // FindMainThreadTid (below) identifies the process's main thread as the one
    // with the EARLIEST creation time, via a CreateToolhelp32Snapshot thread walk
    // plus GetThreadTimes, and returns that TID (0 if not found). Run() calls it
    // ONCE and reuses the TID for every main-thread load — see the loader-lock
    // note in Run().

    DWORD FindMainThreadTid()
    {
        DWORD pid = GetCurrentProcessId();
        DWORD self = GetCurrentThreadId();

        // The main thread is the process's first-created thread, so it has the
        // earliest creation time. NOT the lowest thread ID: TIDs come from a
        // recycled kernel handle table, and 0.1.30 picked the lowest TID (1248),
        // which was one of ntdll's parallel-loader worker threads (origRip inside
        // ntdll) — LdrLoadDll refused it with STATUS_INVALID_THREAD. A toolhelp
        // walk is used rather than NtQuerySystemInformation, which kept answering
        // STATUS_INFO_LENGTH_MISMATCH ("need 64") regardless of buffer size here.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            GLOGW("gage_loader: [main] Thread32 snapshot failed (err=%lu)",
                  (unsigned long)GetLastError());
            return 0;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        DWORD best = 0, lowest = 0, total = 0, inProc = 0, noTimes = 0;
        ULONGLONG bestCreated = ~0ull;
        if (Thread32First(snap, &te))
        {
            do
            {
                total++;
                if (te.th32OwnerProcessID != pid)
                    continue;
                inProc++;
                const DWORD tid = te.th32ThreadID;
                if (tid == self)
                    continue;
                if (lowest == 0 || tid < lowest)
                    lowest = tid;
                HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                FILETIME created{}, exited{}, kernel{}, user{};
                const bool got = t && GetThreadTimes(t, &created, &exited, &kernel, &user);
                if (t)
                    CloseHandle(t);
                if (!got)
                {
                    noTimes++; // exited between the snapshot and OpenThread, or denied
                    continue;
                }
                const ULONGLONG c = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
                if (c < bestCreated)
                {
                    bestCreated = c;
                    best = tid;
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        GLOGI("gage_loader: [main] toolhelp total=%lu inProc=%lu mainTid=%lu (earliest-created; "
              "lowest tid=%lu; %lu without times)",
              (unsigned long)total, (unsigned long)inProc, (unsigned long)best,
              (unsigned long)lowest, (unsigned long)noTimes);
        g_mainTid = best; // expose for the DllMain crash-catch VEH (thread filter)
        return best;      // 0 if not found
    }

    // Direct ntdll load — skips the kernel32/kernelbase LoadLibrary* hooks the
    // Steam in-game overlay (gameoverlayrenderer64.dll) installs. Runs on the
    // caller's thread; we use it when the main-thread trampoline is unavailable.
    HMODULE LdrLoadDirect(const std::wstring& path)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto Ldr = ntdll ? (LdrLoadDllFn)GetProcAddress(ntdll, "LdrLoadDll") : nullptr;
        if (!Ldr)
        {
            GLOGW("gage_loader: LdrLoadDll not found (ntdll=%p); LoadLibraryW last resort",
                  (void*)ntdll);
            HMODULE fb = LoadLibraryW(path.c_str());
            GLOGI("gage_loader: [fallback] LoadLibraryW -> %p (err=%lu)",
                  (void*)fb, fb ? 0UL : (unsigned long)GetLastError());
            return fb;
        }
        const wchar_t* pw = path.c_str();
        UnicodeString us;
        us.Length = (USHORT)(wcslen(pw) * 2);
        us.MaximumLength = us.Length + 2;
        us.Buffer = const_cast<wchar_t*>(pw);
        HMODULE hmod = nullptr;
        LONG st = Ldr(nullptr, nullptr, &us, &hmod);
        GLOGI("gage_loader: [worker] LdrLoadDll status=0x%08lx hmod=%p",
              (unsigned long)st, (void*)hmod);
        // NT_SUCCESS (st >= 0: informational statuses such as
        // STATUS_IMAGE_NOT_AT_BASE still mean "loaded"); a failure never hands
        // back whatever the call left in `hmod`.
        return st >= 0 ? hmod : nullptr;
    }

    // Load `path` on the game's MAIN thread.
    //
    // Suspend the main thread, point its RIP at a tiny code blob, resume, and
    // let the blob call ntdll!LdrLoadDll(path). The blob saves every
    // caller-saved register before the call and restores them before jumping
    // back to the original RIP, so the main thread resumes exactly where it was
    // with its register state intact. We then read the HMODULE the call wrote.
    //
    // The DllMain then runs on the same thread the game's own module loads use,
    // not on this worker thread.
    //
    // Enhanced note (2026-11-04): on GTA5_Enhanced.exe this poll times out after 15 s and the caller falls back to
    // a worker-thread load, so the engine attaches ~26 s after the game starts. 0.4.11 skipped the poll (attach at
    // 2.5 s) and the friend's game died with 0xC0000005 a few seconds after its first frame, twice. The ~26 s
    // attach is the timing proven live on Enhanced (0.4.10, 144 s in the world), so the wait stays as it was.
    HMODULE LoadOnMainThread(const std::wstring& path, DWORD mainTid)
    {
        if (g_mainThreadUnusable)
        {
            GLOGE("gage_loader: [main] skipping %s: the main thread is unusable for loads "
                  "(see the earlier failure)", Narrow(path).c_str());
            return nullptr;
        }
        // Open the main-thread handle for THIS load. `mainTid` is the single TID
        // Run() found once, so every main-thread load runs on the SAME thread —
        // required because the ntdll loader lock a crashed DllMain leaks is only
        // re-entrant for that one owner thread (see the note in Run()). A
        // 0/absent TID degrades to the worker-thread LdrLoadDirect path below.
        HANDLE hMain = mainTid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                            THREAD_SET_CONTEXT, FALSE, mainTid) : nullptr;
        GLOGI("gage_loader: [main] OpenThread(mainTid=%lu)=%p pid=%lu",
              (unsigned long)mainTid, (void*)hMain, (unsigned long)GetCurrentProcessId());
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto LdrLoadDll = ntdll ? (LdrLoadDllFn)GetProcAddress(ntdll, "LdrLoadDll") : nullptr;
        GLOGI("gage_loader: [main] LdrLoadDll=%p (ntdll=%p)", (void*)LdrLoadDll, (void*)ntdll);
        if (hMain && LdrLoadDll)
        {
            // Data block: [us][hmod][done][origRip][status][origRsp]. Heap, so it
            // survives the packed DllMain even if the DllMain clobbers every
            // general-purpose register (we no longer trust r15 to carry the
            // original SP across LdrLoadDll).
            BYTE* data = (BYTE*)VirtualAlloc(nullptr, 128, MEM_COMMIT, PAGE_READWRITE);
            if (data)
            {
                UnicodeString* us = (UnicodeString*)data;
                const wchar_t* pw = path.c_str();         // alive on Run()'s stack
                us->Length = (USHORT)(wcslen(pw) * 2);
                us->MaximumLength = us->Length + 2;
                us->Buffer = const_cast<wchar_t*>(pw);

                QWORD* hmodSlot    = (QWORD*)(data + 16);
                DWORD* doneSlot    = (DWORD*)(data + 24);
                QWORD* origRipSlot = (QWORD*)(data + 32);
                DWORD* statusSlot  = (DWORD*)(data + 40);
                QWORD* origRspSlot = (QWORD*)(data + 48);
                *hmodSlot = 0;
                *doneSlot = 0;
                *origRipSlot = 0;
                *statusSlot = 0x7FFFFFFF;
                *origRspSlot = 0;

                BYTE* code = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE);
                if (code)
                {
                    int o = 0;
                    auto u8   = [&](BYTE v)  { code[o++] = v; };
                    auto imm6 = [&](QWORD v) { memcpy(code + o, &v, 8); o += 8; };
                    auto patch4 = [&](int off, DWORD val) { memcpy(code + off, &val, 4); };
                    // Emit a 4-byte relative operand at the current position, as
                    // (target - nextInstructionAddress). `bytesAfter` is any
                    // operand bytes that follow the displacement (e.g. the imm32
                    // in C7 05 disp32 imm32) which we have not emitted yet.
                    auto relOp = [&](QWORD target, int bytesAfter) {
                        int op = o;
                        u8(0); u8(0); u8(0); u8(0);       // 4 placeholder bytes; o = op+4
                        int nextInstr = o + bytesAfter;
                        patch4(op, (DWORD)((QWORD)target - (QWORD)(code + nextInstr)));
                    };

                    // Save ALL 15 GPRs (not just the 8 caller-saved): the DllMain we
                    // are about to run is packed and may not honour the callee-saved
                    // contract, so we restore every general register before jumping
                    // back to the original RIP.
                    u8(0x50);                               // push rax
                    u8(0x51);                               // push rcx
                    u8(0x52);                               // push rdx
                    u8(0x53);                               // push rbx
                    u8(0x55);                               // push rbp
                    u8(0x56);                               // push rsi
                    u8(0x57);                               // push rdi
                    u8(0x41); u8(0x50);                     // push r8
                    u8(0x41); u8(0x51);                     // push r9
                    u8(0x41); u8(0x52);                     // push r10
                    u8(0x41); u8(0x53);                     // push r11
                    u8(0x41); u8(0x54);                     // push r12
                    u8(0x41); u8(0x55);                     // push r13
                    u8(0x41); u8(0x56);                     // push r14
                    u8(0x41); u8(0x57);                     // push r15
                    // The main thread was suspended at an arbitrary SP. Stash the exact
                    // post-push SP in the (heap) data block — NOT in a register — so the
                    // restore does not depend on any GPR surviving the packed DllMain.
                    // Then align to 16 and reserve the 32-byte home/shadow space. All four
                    // LdrLoadDll args go in registers, so no stack argument is needed.
                    u8(0x48); u8(0x89); u8(0x25); relOp((QWORD)origRspSlot, 0); // mov [origRsp], rsp
                    u8(0x48); u8(0x83); u8(0xE4); u8(0xF0); // and rsp, -16       (align)
                    u8(0x48); u8(0x83); u8(0xEC); u8(0x20); // sub rsp, 32        (32-byte home space)
                    // LdrLoadDll(Process, DllPathSize, DllName, DllHandle) — 4 args,
                    // matching the worker path above (Ldr(nullptr, nullptr, &us, &hmod))
                    // and the LdrLoadDllFn typedef (4 params):
                    //   rcx=0 (Process), rdx=0 (DllPathSize), r8=&us (DllName),
                    //   r9=&hmodSlot (DllHandle).
                    // Earlier builds treated it as 5-arg (Flags in r9, Handle* on the
                    // stack at [rsp+24]); with the real 4-arg signature r9=0 was read as
                    // DllHandle, so LdrLoadDll stored the HMODULE at address [0] — the
                    // "writing 0x0" access violation.  mov r8, imm64 = 49 B8;
                    // mov r9, imm64 = 49 B9 (verified against GNU as).
                    u8(0x48); u8(0x31); u8(0xC9);           // xor ecx, ecx        (Process = NULL)
                    u8(0x48); u8(0x31); u8(0xD2);           // xor edx, edx        (DllPathSize = NULL)
                    u8(0x49); u8(0xB8); imm6((QWORD)us);    // mov r8, &us         (DllName)
                    u8(0x49); u8(0xB9); imm6((QWORD)hmodSlot); // mov r9, &hmodSlot (DllHandle)
                    // call LdrLoadDll — absolute, via R11. A relative `E8 rel32`
                    // only reaches +/-2 GB, but this blob is ~2.4 TB and ntdll is
                    // ~140 TB, so the truncated disp32 lands on garbage (0xC000001D).
                    // R11 is caller-saved and was pushed above, so it is free here and
                    // restored by the final `pop r11`.  (Byte encodings verified against
                    // GNU as.)   mov r11, imm64 = 49 BB <imm64>;  call r11 = 41 FF D3.
                    u8(0x49); u8(0xBB); imm6((QWORD)LdrLoadDll);  // mov r11, LdrLoadDll
                    u8(0x41); u8(0xFF); u8(0xD3);                 // call r11
                    u8(0x48); u8(0x89); u8(0x05); relOp((QWORD)statusSlot, 0); // mov [status],rax
                    u8(0xC7); u8(0x05); relOp((QWORD)doneSlot, 4); u8(1); u8(0); u8(0); u8(0); // mov [done],1
                    // Restore: reload the exact post-push SP from the (heap) data block
                    // — not from a register, so a packed DllMain that clobbers GPRs
                    // can't corrupt the restore. Then POP all 15 GPRs (opcodes:
                    // rax/rcx/rdx/rbx/rbp/rsi/rdi = 58-5F; r8-r15 = 41 58-5F); the
                    // pushes above stored them in rax..r15 order, so the pops bring
                    // every register and rsp back exactly to the suspended state.
                    //
                    // This is the RESTORE path: if the DllMain faults (caught by our
                    // VEH), its RIP is pointed here so the main thread unwinds exactly
                    // as if LdrLoadDll had returned.
                    int restoreOff = o;
                    u8(0x48); u8(0x8B); u8(0x25); relOp((QWORD)origRspSlot, 0); // mov rsp, [origRsp]
                    u8(0x41); u8(0x5F);                     // pop r15
                    u8(0x41); u8(0x5E);                     // pop r14
                    u8(0x41); u8(0x5D);                     // pop r13
                    u8(0x41); u8(0x5C);                     // pop r12
                    u8(0x41); u8(0x5B);                     // pop r11
                    u8(0x41); u8(0x5A);                     // pop r10
                    u8(0x41); u8(0x59);                     // pop r9
                    u8(0x41); u8(0x58);                     // pop r8
                    u8(0x5F);                               // pop rdi
                    u8(0x5E);                               // pop rsi
                    u8(0x5D);                               // pop rbp
                    u8(0x5B);                               // pop rbx
                    u8(0x5A);                               // pop rdx
                    u8(0x59);                               // pop rcx
                    u8(0x58);                               // pop rax
                    u8(0xFF); u8(0x25); relOp((QWORD)origRipSlot, 0); // jmp qword [origRip]

                    // Hex-dump the emitted blob (exactly `o` bytes — the VirtualAlloc
                    // buffer is not zeroed, so don't read past it) so a log is
                    // self-verifying: it must show the 4-arg setup `49 b8 <us>` and
                    // `49 b9 <hmodSlot>` (mov r8,&us; mov r9,&hmodSlot) immediately
                    // before `49 bb <LdrLoadDll>` `41 ff d3` (mov r11; call r11).
                    {
                        char hex[512] = {0};
                        int p = 0;
                        for (int i = 0; i < o; i++)
                            p += snprintf(hex + p, sizeof(hex) - p, "%02x ", code[i]);
                        hex[p > 0 ? p - 1 : 0] = '\0';
                        GLOGI("gage_loader: [main] code bytes (%d total): %s", o, hex);
                    }

                    // Arm the DllMain crash-catch: point the VEH at this blob's
                    // restore path + data slots, then install it at highest priority
                    // so it pre-empts gageinjector's log+minidump+terminate VEH.
                    g_restorePath = code + restoreOff;
                    g_doneSlot    = doneSlot;
                    g_statusSlot  = statusSlot;
                    g_handled     = 0;
                    if (!g_loadVeh)
                        g_loadVeh = AddVectoredExceptionHandler(1, LoadCrashFilter);
                    g_loadActive  = 1;
                    GLOGI("gage_loader: [main] crash-catch armed (veh=%p restore=%p tid=%lu)",
                          (void*)g_loadVeh, (void*)g_restorePath, (unsigned long)g_mainTid);

                    SuspendThread(hMain);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    bool gotCtx = GetThreadContext(hMain, &ctx);
                    if (gotCtx)
                    {
                        *origRipSlot = ctx.Rip;
                        ctx.Rip = (DWORD64)(uintptr_t)code;
                        SetThreadContext(hMain, &ctx);
                    }
                    ResumeThread(hMain);
                    CloseHandle(hMain);
                    char ripAt[300] = "";
                    if (gotCtx)
                        DescribeFaultAddr((DWORD64)*origRipSlot, ripAt, sizeof(ripAt));
                    GLOGI("gage_loader: [main] resumed main thread (gotCtx=%d, origRip=%p %s)",
                          (int)gotCtx, (void*)*origRipSlot, ripAt);
                    GLOGI("gage_loader: [main] code blob=%p data block=%p (cross-ref any crash RIP against these)",
                          (void*)code, (void*)data);

                    for (int i = 0; i < 15000 && *doneSlot == 0; i++)
                        Sleep(1);

                    HMODULE res = (HMODULE)*hmodSlot;
                    const DWORD st = *statusSlot;
                    const bool done = (*doneSlot != 0);
                    const bool crashed = (st == 0x8BADF00D);
                    GLOGI("gage_loader: [main] trampoline done=%d hmod=%p status=0x%08lx crashed=%d",
                          (int)done, (void*)res, (unsigned long)st, (int)crashed);
                    g_loadActive = 0;
                    if (g_loadVeh) { RemoveVectoredExceptionHandler(g_loadVeh); g_loadVeh = nullptr; }
                    g_doneSlot = g_statusSlot = nullptr;
                    g_restorePath = nullptr;
                    if (!done) {
                        // The blob has not run (or not finished) within the poll window: the
                        // main thread may still enter it later. Keep code+data allocated (it
                        // would jump into freed memory otherwise), and stop using this thread
                        // — a second hijack would chain onto the pending one.
                        g_mainThreadUnusable = true;
                        GLOGE("gage_loader: [main] main-thread load did not complete in 15 s "
                              "(status=0x%08lx); blob left in place, no further main-thread loads",
                              (unsigned long)st);
                        // The hijack never executed: on some builds the earliest-created thread
                        // sits in a long kernel wait and never returns into the blob (seen on
                        // GTA V Enhanced, origRip deep in ntdll). Because the blob never called
                        // LdrLoadDll, the engine's DllMain never ran -- nothing crashed and no
                        // loader lock is held -- so loading it on THIS worker thread via ntdll
                        // (skipping the Steam-overlay kernel32 hooks) is safe and at least gets
                        // the engine in. Legacy never reaches here (its main-thread load
                        // completes), so the Legacy path is unchanged.
                        GLOGW("gage_loader: [main] main-thread hijack never ran; falling back to a "
                              "worker-thread load");
                        return LdrLoadDirect(path);
                    }
                    VirtualFree(code, 0, MEM_RELEASE);
                    VirtualFree(data, 0, MEM_RELEASE);
                    if (res)
                        return res;
                    if (crashed) {
                        // The DllMain faulted; we already unwound the main thread. Return
                        // null so Run() reports "no engine loaded" — do NOT re-run the
                        // crashing DllMain via the worker-thread LdrLoadDirect path.
                        GLOGW("gage_loader: [main] DllMain crashed (caught); -> null (no retry)");
                        return nullptr;
                    }
                    // LdrLoadDll ran on the main thread and returned an error. Never retry
                    // on this worker thread: its DllMain would run with the crash-catch
                    // disarmed (0.1.30: STATUS_INVALID_THREAD from a wrong "main" thread,
                    // then the worker-thread retry faulted and killed the game).
                    if (st == 0xC000071C)
                        g_mainThreadUnusable = true; // STATUS_INVALID_THREAD: same answer next time
                    GLOGE("gage_loader: [main] LdrLoadDll on the main thread failed, status=0x%08lx%s; "
                          "no worker-thread retry",
                          (unsigned long)st,
                          st == 0xC000071C ? " (STATUS_INVALID_THREAD: not a thread that may load DLLs)" : "");
                    return nullptr;
                }
                else
                    VirtualFree(data, 0, MEM_RELEASE);
            }
        }
        else if (hMain)
        {
            CloseHandle(hMain);
            GLOGW("gage_loader: [main] main thread not found (pid=%lu); LoadLibraryW fallback",
                  (unsigned long)GetCurrentProcessId());
        }
        // No main thread (or trampoline failed): load directly via ntdll on this
        // worker thread, skipping the Steam-overlay LoadLibrary hooks (the hooked
        // kernel32/kernelbase path is where protected modules were seen to fault).
        return LdrLoadDirect(path);
    }

    void Run()
    {
        GLOGI("gage_loader: build=%s", kLoaderBuild);
        std::wstring dir = reg::Get(L"install_dir");
        if (dir.empty())
            dir = ExeDir();

        // DLL-search dir = install dir, where gage_mp.dll lives.
        GLOGI("gage_loader: install dir -> %s (DLL search dir)", Narrow(dir).c_str());
        SetDllDirectoryW(dir.c_str());

        // Identify the main thread ONCE and reuse that TID for every main-thread
        // load. The ntdll loader lock is owner-tracked and only re-entrant for
        // the SAME thread: when a DllMain faults (caught by our VEH) the lock is
        // left held by the thread that ran that load, and a later load from a
        // DIFFERENT thread blocks on it forever (done=0, status=0x7fffffff after
        // the poll timeout). Caching one main thread keeps any later load on the
        // thread that can re-acquire the lock.
        DWORD mainTid = FindMainThreadTid();
        GLOGI("gage_loader: main thread for all main-thread loads = %lu",
              (unsigned long)mainTid);

        const std::wstring engineDll = dir + L"gage_mp.dll";
        if (!DllPresent(engineDll))
        {
            GLOGW("gage_loader: %s not found; no engine loaded", Narrow(engineDll).c_str());
            return;
        }
        HMODULE mp = LoadOnMainThread(engineDll, mainTid);
        GLOGI("gage_loader: gage_mp.dll -> %p (err=%lu)",
              (void*)mp, mp ? 0UL : (unsigned long)GetLastError());
        if (!mp)
            GLOGW("gage_loader: no engine loaded");
    }

}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HANDLE t = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)Run, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
