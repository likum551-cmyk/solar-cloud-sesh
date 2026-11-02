#pragma once
#include "common.h"

namespace gage::inject {

    // Markers written to InjectDll's *outErr (they are not Win32 error codes;
    // the high bit keeps them clear of every real code).
    //   kErrRemoteTimeout  - the remote LoadLibraryW thread did not finish in time.
    //   kErrTargetNotReady - kernel32 never appeared in the target (see InjectDll).
    constexpr DWORD kErrRemoteTimeout  = 0x80000001;
    constexpr DWORD kErrTargetNotReady = 0x80000002;

    // Resolve an exported symbol inside the *target* process's copy of a
    // module (ASLR-safe: we read the target's module base from a toolhelp
    // snapshot and reuse the offset we observe in our own process).
    // Returns nullptr when the module is not (yet) loaded in the target.
    LPCVOID RemoteExport(HANDLE hProc, const wchar_t* moduleName, const char* exportName);

    // Inject a DLL by path into an already-open target process using the
    // classic remote-LoadLibrary technique:
    //   1. wait until kernel32!LoadLibraryW resolves in the target (a process
    //      created a moment ago has no module list yet; polls up to 10 s and
    //      retries ERROR_BAD_LENGTH / ERROR_PARTIAL_COPY from the snapshot),
    //   2. write the wide path into the target,
    //   3. CreateRemoteThread(target's kernel32!LoadLibraryW, path).
    // Returns true on successful load. *outErr (optional) is always written:
    // the remote exit code (the HMODULE, or an ERROR_ code on failure), a
    // Win32 error from one of the steps, or kErrTargetNotReady / kErrRemoteTimeout.
    bool InjectDll(HANDLE hProc, const std::wstring& dllPath,
                   DWORD timeoutMs = 30000, DWORD* outErr = nullptr);

    // Open an already-running process by image name (case-insensitive), or
    // launch it from an absolute path if it isn't running yet.
    // Returns a handle on success, or nullptr on failure.
    //   *outLaunched (optional) - true when this call started the process.
    //   *outErr      (optional) - Win32 error on failure. A process that is
    //                running but cannot be opened (e.g. elevated) is reported,
    //                never answered by starting a second copy.
    HANDLE GetOrOpenProcess(const std::wstring& imageBaseName,
                            const std::wstring& launchPath /* may be empty */,
                            bool* outLaunched = nullptr, DWORD* outErr = nullptr);

    // Find a live process by image name; returns its PID or 0.
    DWORD FindProcess(const std::wstring& imageBaseName);

    // Gracefully close a running process by image name: post WM_CLOSE, wait up
    // to `graceMs`, then TerminateProcess as a fallback. Returns true if the
    // process is gone (or was never running); false if it is still alive.
    bool CloseProcessByName(const std::wstring& imageBaseName, DWORD graceMs = 3000);

    // Same, for one specific process the caller holds a handle to (needs
    // SYNCHRONIZE | PROCESS_TERMINATE). The handle pins the process object, so
    // a recycled PID can never be hit. Does not close the handle. Returns true
    // if the process is gone.
    bool CloseProcessByHandle(HANDLE hProc, DWORD graceMs = 3000);

}
