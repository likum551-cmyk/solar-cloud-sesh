// crash_diag_test — the in-game crash diagnostics, driven on this exe's own
// process. Build target: crash_diag_test (selftest/, never shipped).
//
//   1. Stack scan: reproduces a packed-DllMain fault class seen in the game
//      (GetSystemTimeAsFileTime handed a non-canonical out-pointer, the exact
//      value from gage.log) and
//      checks crash::LogStackScan names the calling function via [RSP].
//   2. Minidump: MiniDumpWriteDump from inside the VEH succeeds with dbghelp.h's
//      MINIDUMP_EXCEPTION_INFORMATION; the old hand-rolled default-aligned struct
//      tears the pointer dbghelp reads (the ERROR_NOACCESS gageinjector logged).
//   3. MSVC throw decoding: gage_loader's DescribeThrownType on a fabricated
//      0xE06D7363 record (RVA-based ThrowInfo) logs the type, its base and what().
//   4. Main thread: FindMainThreadTid picks this process's first thread even
//      after later threads exist, called from a worker thread like the loader.

#include "../apps/injector/gage_loader.cpp" // the TU itself, so its anonymous-namespace helpers are reachable

#include <dbghelp.h>
#include <cstdio>
#include <string>

using namespace gage;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

static std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir, n) + L"gage_crash_diag_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
}

static std::string ReadAll(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// ---- 1 + 2: the fault, caught by a VEH --------------------------------------

static const uintptr_t kBadPtr = 0xC950FD16CAC0412Full; // RCX from the 0.1.29/0.1.30 logs
static FILETIME g_ft;
static volatile LONG g_armed = 0;
static uintptr_t g_ret0 = 0;
static BOOL g_dumpNew = FALSE;
static DWORD g_dumpNewErr = 0;
static bool g_oldTorn = false;
static size_t g_oldPtrAt = 0;
static std::wstring g_dumpPath;

using MDWFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);

static BOOL DumpWith(MDWFn fn, void* mdie, DWORD* err) {
    HANDLE f = CreateFileW(g_dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return FALSE; }
    const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x2 | 0x20, mdie, nullptr, nullptr);
    *err = ok ? 0 : GetLastError();
    CloseHandle(f);
    return ok;
}

static LONG NTAPI TestVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005 || !InterlockedExchange(&g_armed, 0))
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* C = ep->ContextRecord;
    g_ret0 = *(const uintptr_t*)C->Rsp;
    crash::LogStackScan("crashtest:", C);

    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto fn = dbghelp ? (MDWFn)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
    if (fn) {
        MINIDUMP_EXCEPTION_INFORMATION good{};
        good.ThreadId = GetCurrentThreadId();
        good.ExceptionPointers = ep;
        good.ClientPointers = FALSE;
        g_dumpNew = DumpWith(fn, &good, &g_dumpNewErr);
    }

    // The pre-0.1.31 gageinjector struct: default alignment, never zeroed. Not
    // handed to dbghelp here: the torn pointer it yields makes this Windows
    // build's dbgcore fault outright (0x005fdb50cccccccc under gdb), where the
    // game's returned ERROR_NOACCESS. Check the tear itself: dbghelp reads the
    // pointer at offset 4.
    struct OldMdie { DWORD ThreadId; const EXCEPTION_POINTERS* Exception; BOOL ClientPointers; } old;
    memset(&old, 0xCC, sizeof(old)); // stand-in for the uninitialised padding
    old.ThreadId = GetCurrentThreadId();
    old.Exception = ep;
    old.ClientPointers = FALSE;
    uint64_t readAt4 = 0;
    memcpy(&readAt4, (const BYTE*)&old + 4, 8);
    g_oldTorn = (readAt4 != (uint64_t)(uintptr_t)ep);
    g_oldPtrAt = offsetof(OldMdie, Exception);

    C->Rcx = (DWORD64)(uintptr_t)&g_ft; // retry `mov [rcx],eax` against a real FILETIME
    return EXCEPTION_CONTINUE_EXECUTION;
}

// noinline + work after the call: the call must not become a tail jump, so
// [RSP] at the fault is a return address inside this function.
__attribute__((noinline)) static DWORD CallWithBadPointer() {
    GetSystemTimeAsFileTime((FILETIME*)kBadPtr);
    return ((volatile FILETIME*)&g_ft)->dwHighDateTime;
}
__attribute__((noinline)) static void EndOfCallWithBadPointer() { std::printf("%s", ""); }

// ---- 4: main thread from a worker -------------------------------------------

static DWORD g_foundTid = 0;
static DWORD WINAPI FindFromWorker(LPVOID) { g_foundTid = FindMainThreadTid(); return 0; }
static DWORD WINAPI Idle(LPVOID ev) { WaitForSingleObject((HANDLE)ev, INFINITE); return 0; }

int main() {
    const std::wstring logPath = TempFile(L"log.txt");
    g_dumpPath = TempFile(L"test.dmp");
    log::init(logPath);

    // 1 + 2
    PVOID veh = AddVectoredExceptionHandler(1, TestVeh);
    g_armed = 1;
    const DWORD high = CallWithBadPointer();
    RemoveVectoredExceptionHandler(veh);
    const uintptr_t fnLo = (uintptr_t)&CallWithBadPointer;
    char buf[160];
    snprintf(buf, sizeof(buf), "ret0=%p fn=%p", (void*)g_ret0, (void*)fnLo);
    Check("VEH caught the fault and the retry completed", g_armed == 0 && high != 0, buf);
    Check("[RSP] is a return address inside the caller", g_ret0 > fnLo && g_ret0 < fnLo + 64, buf);
    snprintf(buf, sizeof(buf), "ok=%d err=0x%08lx", (int)g_dumpNew, (unsigned long)g_dumpNewErr);
    Check("minidump with MINIDUMP_EXCEPTION_INFORMATION succeeds", g_dumpNew == TRUE, buf);
    snprintf(buf, sizeof(buf), "old pointer at %zu, dbghelp reads at %zu",
             g_oldPtrAt, offsetof(MINIDUMP_EXCEPTION_INFORMATION, ExceptionPointers));
    Check("old default-aligned struct tears the pointer dbghelp reads (the logged bug)",
          g_oldTorn && g_oldPtrAt == 8 &&
          offsetof(MINIDUMP_EXCEPTION_INFORMATION, ExceptionPointers) == 4, buf);
    DeleteFileW(g_dumpPath.c_str());

    // 3
    alignas(8) static BYTE blob[0x200] = {};
    auto put32 = [](BYTE* p, int v) { memcpy(p, &v, 4); };
    put32(blob + 12, 0x40);            // ThrowInfo.pCatchableTypeArray (RVA)
    put32(blob + 0x40, 2);             // CatchableTypeArray.nCatchableTypes
    put32(blob + 0x44, 0x60);          //   [0] thrown type
    put32(blob + 0x48, 0x80);          //   [1] base
    put32(blob + 0x60 + 4, 0x100);     // CatchableType.pType -> TypeDescriptor
    put32(blob + 0x80 + 4, 0x180);
    strcpy((char*)blob + 0x100 + 16, ".?AVruntime_error@std@@");
    strcpy((char*)blob + 0x180 + 16, ".?AVexception@std@@");
    static struct { void* vft; const char* what; bool doFree; } obj = { nullptr, "gage test message", false };
    EXCEPTION_RECORD er{};
    er.ExceptionCode = 0xE06D7363;
    er.ExceptionAddress = (void*)&CallWithBadPointer;
    er.NumberParameters = 4;
    er.ExceptionInformation[0] = 0x19930520;
    er.ExceptionInformation[1] = (ULONG_PTR)&obj;
    er.ExceptionInformation[2] = (ULONG_PTR)blob;
    er.ExceptionInformation[3] = (ULONG_PTR)blob;
    DescribeThrownType(&er);
    EXCEPTION_RECORD bad = er;
    bad.ExceptionInformation[2] = 0x1234; // unreadable ThrowInfo must not fault
    DescribeThrownType(&bad);

    // 4: later threads exist when the lookup runs (as in the game)
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE idle[3];
    for (HANDLE& h : idle) h = CreateThread(nullptr, 0, Idle, ev, 0, nullptr);
    HANDLE w = CreateThread(nullptr, 0, FindFromWorker, nullptr, 0, nullptr);
    WaitForSingleObject(w, 10000);
    CloseHandle(w);
    SetEvent(ev);
    WaitForMultipleObjects(3, idle, TRUE, 10000);
    for (HANDLE h : idle) CloseHandle(h);
    CloseHandle(ev);
    snprintf(buf, sizeof(buf), "found=%lu main=%lu", (unsigned long)g_foundTid,
             (unsigned long)GetCurrentThreadId());
    Check("FindMainThreadTid picks the first-created thread", g_foundTid == GetCurrentThreadId(), buf);

    // Log content (1 + 3)
    const std::string logText = ReadAll(logPath);
    const bool rspLine = logText.find("crashtest:   [RSP] = ") != std::string::npos;
    const bool rspNamed = logText.find("(crash_diag_test.exe+0x") != std::string::npos;
    const bool rspCall = logText.find("after a CALL: the faulting function's caller") != std::string::npos;
    Check("stack scan logs [RSP] named crash_diag_test.exe+RVA after a CALL", rspLine && rspNamed && rspCall);
    Check("stack scan summary line", logText.find("crashtest:   stack scan: ") != std::string::npos);
    Check("throw type logged", logText.find("threw TYPE: .?AVruntime_error@std@@") != std::string::npos);
    Check("throw base logged", logText.find("threw TYPE (base): .?AVexception@std@@") != std::string::npos);
    Check("what() logged", logText.find("what(): gage test message") != std::string::npos);
    Check("unreadable ThrowInfo reported, not faulted",
          logText.find("threw TYPE: <throwinfo unreadable>") != std::string::npos);
    EndOfCallWithBadPointer();

    const int expected = 11;
    std::printf("%d/%d passed  (log: %s)\n", g_run - g_failed, g_run, Narrow(logPath).c_str());
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    if (!g_failed) DeleteFileW(logPath.c_str());
    return g_failed ? 1 : 0;
}
