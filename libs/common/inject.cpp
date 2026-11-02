#include "inject.h"
#include "logger.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <string>

#pragma comment(lib, "psapi")

namespace gage::inject {

    // How long InjectDll waits for a freshly created target to expose kernel32.
    static const DWORD kTargetReadyMs = 10000;

    // Resolve `exportName` of `moduleName` inside the target. Returns 0 and
    // fills *out on success, else the Win32 error that explains why not:
    //   ERROR_PROC_NOT_FOUND  our own copy of the module/export is missing (permanent)
    //   ERROR_BAD_LENGTH / ERROR_PARTIAL_COPY  the target's module list could not be
    //                         read yet (process still initialising) - retry
    //   ERROR_MOD_NOT_FOUND   the module is not in the target's list yet - retry
    //   anything else         the snapshot was refused (e.g. ERROR_ACCESS_DENIED)
    static DWORD ResolveRemoteExport(HANDLE hProc, const wchar_t* moduleName,
                                     const char* exportName, LPCVOID* out)
    {
        *out = nullptr;
        DWORD targetPid = GetProcessId(hProc);
        if (targetPid == 0)
            return ERROR_INVALID_HANDLE;

        // Offset of the export within the module (identical in both spaces
        // because it's the same module image).
        HMODULE localBase = GetModuleHandleW(moduleName);
        FARPROC localExp  = localBase ? GetProcAddress(localBase, exportName) : nullptr;
        if (!localBase || !localExp)
            return ERROR_PROC_NOT_FOUND;
        SIZE_T offset = (SIZE_T)localExp - (SIZE_T)localBase;

        // Target's base address for the module. CreateToolhelp32Snapshot can
        // fail with ERROR_BAD_LENGTH while the module list changes under it;
        // the documented remedy is to retry straight away.
        HANDLE hSnap = INVALID_HANDLE_VALUE;
        DWORD snapErr = 0;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, targetPid);
            if (hSnap != INVALID_HANDLE_VALUE)
                break;
            snapErr = GetLastError();
            if (snapErr != ERROR_BAD_LENGTH)
                break;
        }
        if (hSnap == INVALID_HANDLE_VALUE)
            return snapErr ? snapErr : ERROR_BAD_LENGTH;

        HMODULE targetBase = nullptr;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, moduleName) == 0)
                {
                    targetBase = (HMODULE)(SIZE_T)me.modBaseAddr;
                    break;
                }
            } while (Module32NextW(hSnap, &me));
        }
        CloseHandle(hSnap);
        if (!targetBase)
            return ERROR_MOD_NOT_FOUND;

        *out = (LPCVOID)((SIZE_T)targetBase + offset);
        return 0;
    }

    LPCVOID RemoteExport(HANDLE hProc, const wchar_t* moduleName, const char* exportName)
    {
        LPCVOID p = nullptr;
        if (ResolveRemoteExport(hProc, moduleName, exportName, &p) != 0)
            return nullptr;
        return p;
    }

    // Poll until the export resolves in the target. A process that CreateProcessW
    // returned a moment ago has no (readable) module list yet: the snapshot fails
    // with ERROR_BAD_LENGTH / ERROR_PARTIAL_COPY or kernel32 is simply missing.
    // Returns 0 on success, kErrTargetNotReady on timeout, else the first
    // non-retryable Win32 error (target exited, access denied, ...).
    static DWORD WaitForRemoteExport(HANDLE hProc, const wchar_t* moduleName,
                                     const char* exportName, DWORD timeoutMs, LPCVOID* out)
    {
        DWORD start = GetTickCount();
        for (;;)
        {
            DWORD err = ResolveRemoteExport(hProc, moduleName, exportName, out);
            if (err == 0)
            {
                DWORD waited = GetTickCount() - start;
                if (waited >= 50)
                    GLOGI("inject: %s!%s resolved in pid %lu after %lu ms", Narrow(moduleName).c_str(), exportName,
                          (unsigned long)GetProcessId(hProc), (unsigned long)waited);
                return 0;
            }
            bool retryable = err == ERROR_MOD_NOT_FOUND || err == ERROR_BAD_LENGTH
                          || err == ERROR_PARTIAL_COPY;
            if (!retryable)
                return err;
            if (WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0)
                return ERROR_PROCESS_ABORTED; // the target died while we waited
            if (GetTickCount() - start >= timeoutMs)
                return kErrTargetNotReady;
            Sleep(50);
        }
    }

    bool InjectDll(HANDLE hProc, const std::wstring& dllPath,
                   DWORD timeoutMs, DWORD* outErr)
    {
        if (outErr)
            *outErr = 0;
        auto fail = [&](DWORD e) { if (outErr) *outErr = e; return false; };

        LPCVOID remoteLoadLibrary = nullptr;
        DWORD ready = WaitForRemoteExport(hProc, L"kernel32.dll", "LoadLibraryW",
                                          kTargetReadyMs, &remoteLoadLibrary);
        if (ready != 0)
        {
            GLOGE("inject: kernel32!LoadLibraryW not resolvable in pid %lu (err=0x%08lX)",
                  (unsigned long)GetProcessId(hProc), (unsigned long)ready);
            return fail(ready);
        }

        SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remotePath = VirtualAllocEx(hProc, nullptr, bytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
            return fail(GetLastError());

        SIZE_T written = 0;
        if (!WriteProcessMemory(hProc, remotePath, dllPath.c_str(), bytes, &written)
            || written != bytes)
        {
            DWORD e = GetLastError();
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
            return fail(e ? e : ERROR_WRITE_FAULT);
        }

        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
            (LPTHREAD_START_ROUTINE)(uintptr_t)remoteLoadLibrary,
            remotePath, 0, nullptr);
        if (!hThread)
        {
            DWORD e = GetLastError();
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
            return fail(e);
        }

        DWORD wait = WaitForSingleObject(hThread, timeoutMs);
        DWORD code = (DWORD)-1;
        if (wait == WAIT_OBJECT_0)
            GetExitCodeThread(hThread, &code);
        else if (wait == WAIT_TIMEOUT)
            code = kErrRemoteTimeout; // our own "timed out" marker

        CloseHandle(hThread);
        // After a timeout the remote thread may still be reading the path; freeing
        // it under the thread would turn a slow load into a crash in the target.
        if (wait == WAIT_OBJECT_0)
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);

        if (outErr)
            *outErr = code;

        return wait == WAIT_OBJECT_0
            && code != 0
            && code != ERROR_MOD_NOT_FOUND
            && code != kErrRemoteTimeout;
    }

    DWORD FindProcess(const std::wstring& imageBaseName)
    {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE)
            return 0;
        DWORD found = 0;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, imageBaseName.c_str()) == 0)
                {
                    found = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return found;
    }

    static BOOL CALLBACK EnumCloseProc(HWND hwnd, LPARAM lp)
    {
        DWORD want = (DWORD)lp;
        DWORD p = 0;
        GetWindowThreadProcessId(hwnd, &p);
        if (p == want && IsWindowVisible(hwnd))
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }

    bool CloseProcessByName(const std::wstring& imageBaseName, DWORD graceMs)
    {
        DWORD pid = FindProcess(imageBaseName);
        if (pid == 0)
            return true; // nothing to close

        // Ask it to close politely (the game gets a chance to save/confirm).
        EnumWindows(EnumCloseProc, (LPARAM)pid);

        // Wait for it to exit on its own.
        DWORD start = GetTickCount();
        while (GetTickCount() - start < graceMs)
        {
            if (FindProcess(imageBaseName) == 0)
                return true;
            Sleep(100);
        }

        // Still there -> force.
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h)
        {
            TerminateProcess(h, 0);
            CloseHandle(h);
            Sleep(150);
        }
        return FindProcess(imageBaseName) == 0;
    }

    bool CloseProcessByHandle(HANDLE hProc, DWORD graceMs)
    {
        if (!hProc)
            return true;
        if (WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0)
            return true; // already gone

        DWORD pid = GetProcessId(hProc);
        if (pid == 0)
            return false;

        // Ask it to close politely (the game gets a chance to save/confirm).
        EnumWindows(EnumCloseProc, (LPARAM)pid);
        if (WaitForSingleObject(hProc, graceMs) == WAIT_OBJECT_0)
            return true;

        // Still there -> force. The handle pins this exact process object.
        TerminateProcess(hProc, 0);
        return WaitForSingleObject(hProc, 2000) == WAIT_OBJECT_0;
    }

    HANDLE GetOrOpenProcess(const std::wstring& imageBaseName, const std::wstring& launchPath,
                            bool* outLaunched, DWORD* outErr)
    {
        if (outLaunched)
            *outLaunched = false;
        if (outErr)
            *outErr = 0;

        // 1) already running?
        DWORD pid = FindProcess(imageBaseName);
        if (pid != 0)
        {
            HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
            if (h)
                return h;
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED)
            {
                // It is there but we may not touch it (elevated game, unelevated
                // launcher). Starting a second copy would be the wrong answer.
                if (outErr)
                    *outErr = e;
                SetLastError(e);
                return nullptr;
            }
            // Any other failure: the process most likely exited between the
            // snapshot and OpenProcess; fall through and launch.
        }

        // 2) not running and we know where it is -> launch it.
        if (!launchPath.empty())
        {
            STARTUPINFOW si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(launchPath.c_str(), nullptr, nullptr, nullptr,
                FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                if (outLaunched)
                    *outLaunched = true;
                return pi.hProcess;
            }
            if (outErr)
                *outErr = GetLastError();
        }
        return nullptr;
    }

}
