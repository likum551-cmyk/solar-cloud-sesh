// net_guard.cpp -- see net_guard.h. IAT hooks only: an import-table pointer is swapped, no code byte
// of the game or of ws2_32/wininet is touched, and Uninstall puts every pointer back.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net_guard.h"
#include "common/logger.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define NG_RETADDR() _ReturnAddress()
#else
#define NG_RETADDR() __builtin_return_address(0)
#endif

namespace gage::netguard {
namespace {

enum Fn { kGetAddrInfo, kGetAddrInfoW, kGetHostByName, kConnect, kSendTo, kInternetConnectW, kFnCount };
enum Dll { kWs2 = 0, kWinInet = 1 };

using GetAddrInfoA_t     = INT (WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using GetAddrInfoW_t     = INT (WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
using GetHostByName_t    = hostent* (WSAAPI*)(const char*);
using Connect_t          = int (WSAAPI*)(SOCKET, const sockaddr*, int);
using SendTo_t           = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using InternetConnectW_t = void* (WINAPI*)(void*, LPCWSTR, WORD, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);

std::atomic<int>   g_mode{(int)Mode::Off};
std::atomic<void*> g_orig[kFnCount];      // the pointer each slot held before us (first patch wins)
std::atomic<void*> g_export[kFnCount];    // the real export (GetProcAddress), for matching + fallback
std::atomic<int>   g_inFlight{0};
std::atomic<bool>  g_stop{false};
std::atomic<bool>  g_installed{false};
std::atomic<unsigned> g_namesBlocked{0}, g_addrsBlocked{0}, g_skippedForeign{0};

struct InFlight {
    InFlight() { g_inFlight.fetch_add(1); }
    ~InFlight() { g_inFlight.fetch_sub(1); }
};

bool guarding() { return g_mode.load() == (int)Mode::Guard; }

void* origOf(Fn f) {
    void* p = g_orig[f].load();
    return p ? p : g_export[f].load();
}

// ---------------------------------------------------------------------------
// Logging helpers: each distinct name / destination once, capped.
// ---------------------------------------------------------------------------
constexpr size_t kNameCap = 512, kAddrCap = 256, kAllowCap = 1024;
SRWLOCK g_seenLock = SRWLOCK_INIT;
std::vector<std::string> g_names, g_addrs;

// 1 = new (log it), 2 = new but the cap was just hit (log the cap once), 0 = seen or over cap.
int noteOnce(std::vector<std::string>& seen, const std::string& key, size_t cap) {
    AcquireSRWLockExclusive(&g_seenLock);
    int r = 0;
    bool found = false;
    for (const std::string& s : seen) if (s == key) { found = true; break; }
    if (!found) {
        if (seen.size() < cap) { seen.push_back(key); r = 1; }
        else if (seen.size() == cap) { seen.push_back(std::string()); r = 2; }   // sentinel: cap logged
    }
    ReleaseSRWLockExclusive(&g_seenLock);
    return r;
}

void narrow(const wchar_t* w, char* out, size_t n, bool& truncated) {
    size_t i = 0;
    truncated = false;
    for (; w && w[i]; ++i) {
        if (i + 1 >= n) { truncated = true; break; }
        out[i] = (w[i] > 0 && w[i] < 0x80) ? (char)w[i] : '?';
    }
    out[i] = '\0';
}

void callerName(void* ret, char* out, size_t n) {
    HMODULE m = nullptr;
    if (ret && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)ret, &m) && m) {
        wchar_t path[MAX_PATH] = {};
        const DWORD L = GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* b = path;
        for (DWORD i = 0; i < L; ++i) if (path[i] == L'\\' || path[i] == L'/') b = path + i + 1;
        bool t = false;
        narrow(b, out, n, t);
        return;
    }
    snprintf(out, n, "?");
}

void lower(std::string& s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
}

void noteName(const char* kind, const char* name, bool blocked, void* ret) {
    std::string key(name);
    lower(key);
    const int r = noteOnce(g_names, key, kNameCap);
    if (r == 1) {
        char caller[96];
        callerName(ret, caller, sizeof(caller));
        if (blocked) {
            g_namesBlocked.fetch_add(1);
            GLOGW("netguard: BLOCK %s \"%s\" (caller %s) -> loopback", kind, name, caller);
        } else {
            GLOGI("netprobe: dns %s \"%s\" (caller %s)%s", kind, name, caller,
                  IsBlockedHost(name) ? " [Rockstar host]" : "");
        }
    } else if (r == 2) {
        GLOGW("netprobe: hostname log cap (%u) reached; further new names are not logged", (unsigned)kNameCap);
    }
}

// sockaddr -> 16-byte key (IPv4 as ::ffff:a.b.c.d) + port. False for a non-IP family / short len.
bool sockKey(const sockaddr* sa, int len, uint8_t key[16], unsigned& port) {
    if (!sa) return false;
    if (sa->sa_family == AF_INET && len >= (int)sizeof(sockaddr_in)) {
        const sockaddr_in* s = (const sockaddr_in*)sa;
        memset(key, 0, 10);
        key[10] = key[11] = 0xFF;
        memcpy(key + 12, &s->sin_addr, 4);
        const uint8_t* p = (const uint8_t*)&s->sin_port;
        port = ((unsigned)p[0] << 8) | p[1];
        return true;
    }
    if (sa->sa_family == AF_INET6 && len >= (int)sizeof(sockaddr_in6)) {
        const sockaddr_in6* s = (const sockaddr_in6*)sa;
        memcpy(key, &s->sin6_addr, 16);
        const uint8_t* p = (const uint8_t*)&s->sin6_port;
        port = ((unsigned)p[0] << 8) | p[1];
        return true;
    }
    return false;
}

bool isMappedV4(const uint8_t k[16]) {
    for (int i = 0; i < 10; ++i) if (k[i]) return false;
    return k[10] == 0xFF && k[11] == 0xFF;
}

void fmtKey(const uint8_t k[16], unsigned port, char* out, size_t n) {
    if (isMappedV4(k)) {
        snprintf(out, n, "%u.%u.%u.%u:%u", k[12], k[13], k[14], k[15], port);
        return;
    }
    snprintf(out, n, "[%x:%x:%x:%x:%x:%x:%x:%x]:%u",
             (k[0] << 8) | k[1], (k[2] << 8) | k[3], (k[4] << 8) | k[5], (k[6] << 8) | k[7],
             (k[8] << 8) | k[9], (k[10] << 8) | k[11], (k[12] << 8) | k[13], (k[14] << 8) | k[15], port);
}

bool keyLoopback(const uint8_t k[16]) {
    return isMappedV4(k) ? IsLoopbackV4(k + 12) : IsLoopbackV6(k);
}

// Allow list (Guard): addresses returned for NON-Rockstar hostnames resolved through these hooks.
SRWLOCK g_allowLock = SRWLOCK_INIT;
std::vector<std::array<uint8_t, 16>> g_allow;
bool g_allowCapLogged = false;

void allowKey(const uint8_t k[16]) {
    AcquireSRWLockExclusive(&g_allowLock);
    bool have = false;
    for (const auto& a : g_allow) if (memcmp(a.data(), k, 16) == 0) { have = true; break; }
    bool capHit = false;
    if (!have) {
        if (g_allow.size() < kAllowCap) {
            std::array<uint8_t, 16> a;
            memcpy(a.data(), k, 16);
            g_allow.push_back(a);
        } else if (!g_allowCapLogged) {
            g_allowCapLogged = capHit = true;
        }
    }
    ReleaseSRWLockExclusive(&g_allowLock);
    if (capHit)
        GLOGW("netguard: allow list full (%u); later addresses stay blocked (fail closed)", (unsigned)kAllowCap);
}

void allowSockaddr(const sockaddr* sa, int len) {
    uint8_t k[16];
    unsigned port = 0;
    if (sockKey(sa, len, k, port)) allowKey(k);
}

bool allowed(const uint8_t k[16]) {
    AcquireSRWLockShared(&g_allowLock);
    bool have = false;
    for (const auto& a : g_allow) if (memcmp(a.data(), k, 16) == 0) { have = true; break; }
    ReleaseSRWLockShared(&g_allowLock);
    return have;
}

// Guard decision for connect/sendto. Non-IP families and loopback always pass.
bool destAllowed(const sockaddr* sa, int len) {
    uint8_t k[16];
    unsigned port = 0;
    if (!sockKey(sa, len, k, port)) return true;
    return keyLoopback(k) || allowed(k);
}

// Probe: log each distinct non-loopback destination once. Guard: log each blocked one once.
void noteDest(const char* kind, const sockaddr* sa, int len, bool blocked, void* ret) {
    uint8_t k[16];
    unsigned port = 0;
    if (!sockKey(sa, len, k, port)) return;
    if (!blocked && keyLoopback(k)) return;
    char addr[80];
    fmtKey(k, port, addr, sizeof(addr));
    std::string key = std::string(blocked ? "B" : "A") + kind + " " + addr;
    const int r = noteOnce(g_addrs, key, kAddrCap);
    if (r == 1) {
        char caller[96];
        callerName(ret, caller, sizeof(caller));
        if (blocked) {
            g_addrsBlocked.fetch_add(1);
            GLOGW("netguard: BLOCK %s -> %s (caller %s): not loopback and not resolved from a non-Rockstar "
                  "name through the guard", kind, addr, caller);
        } else {
            GLOGI("netprobe: %s -> %s (caller %s)", kind, addr, caller);
        }
    } else if (r == 2) {
        GLOGW("netprobe: destination log cap (%u) reached; further new destinations are not logged",
              (unsigned)kAddrCap);
    }
}

// ---------------------------------------------------------------------------
// The hooks. Each calls the original it replaced; in Guard mode a Rockstar name is swapped for
// loopback and an unknown non-loopback destination is refused.
// ---------------------------------------------------------------------------
INT WSAAPI hkGetAddrInfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* res) {
    InFlight f;
    auto orig = (GetAddrInfoA_t)origOf(kGetAddrInfo);
    if (!orig) return WSANO_RECOVERY;
    const bool guard = guarding();
    if (node && *node) {
        const bool block = guard && IsBlockedHost(node);
        noteName("getaddrinfo", node, block, NG_RETADDR());
        if (block) {
            const bool v6 = hints && hints->ai_family == AF_INET6;
            return orig(v6 ? "::1" : "127.0.0.1", service, hints, res);
        }
    }
    const INT rc = orig(node, service, hints, res);
    if (guard && rc == 0 && res && *res && node && !IsAddressLiteral(node))
        for (const ADDRINFOA* p = *res; p; p = p->ai_next) allowSockaddr(p->ai_addr, (int)p->ai_addrlen);
    return rc;
}

INT WSAAPI hkGetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* res) {
    InFlight f;
    auto orig = (GetAddrInfoW_t)origOf(kGetAddrInfoW);
    if (!orig) return WSANO_RECOVERY;
    const bool guard = guarding();
    char n[300] = {};
    bool truncated = false;
    if (node && *node) {
        narrow(node, n, sizeof(n), truncated);
        const bool block = guard && (truncated || IsBlockedHost(n));
        noteName("GetAddrInfoW", n, block, NG_RETADDR());
        if (block) {
            const bool v6 = hints && hints->ai_family == AF_INET6;
            return orig(v6 ? L"::1" : L"127.0.0.1", service, hints, res);
        }
    }
    const INT rc = orig(node, service, hints, res);
    if (guard && rc == 0 && res && *res && node && *node && !IsAddressLiteral(n))
        for (const ADDRINFOW* p = *res; p; p = p->ai_next) allowSockaddr(p->ai_addr, (int)p->ai_addrlen);
    return rc;
}

hostent* WSAAPI hkGetHostByName(const char* name) {
    InFlight f;
    auto orig = (GetHostByName_t)origOf(kGetHostByName);
    if (!orig) { SetLastError(WSANO_RECOVERY); return nullptr; }
    const bool guard = guarding();
    if (name && *name) {
        const bool block = guard && IsBlockedHost(name);
        noteName("gethostbyname", name, block, NG_RETADDR());
        if (block) return orig("127.0.0.1");
    }
    hostent* h = orig(name);
    if (guard && h && name && *name && !IsAddressLiteral(name) && h->h_addrtype == AF_INET && h->h_length == 4) {
        for (char** a = h->h_addr_list; a && *a; ++a) {
            uint8_t k[16] = {};
            k[10] = k[11] = 0xFF;
            memcpy(k + 12, *a, 4);
            allowKey(k);
        }
    }
    return h;
}

int WSAAPI hkConnect(SOCKET s, const sockaddr* sa, int len) {
    InFlight f;
    auto orig = (Connect_t)origOf(kConnect);
    if (!orig) { SetLastError(WSAENETDOWN); return SOCKET_ERROR; }
    if (sa) {
        if (guarding() && !destAllowed(sa, len)) {
            noteDest("connect", sa, len, true, NG_RETADDR());
            SetLastError(WSAENETUNREACH);   // WSASetLastError is SetLastError
            return SOCKET_ERROR;
        }
        noteDest("connect", sa, len, false, NG_RETADDR());
    }
    return orig(s, sa, len);
}

int WSAAPI hkSendTo(SOCKET s, const char* buf, int blen, int flags, const sockaddr* to, int tolen) {
    InFlight f;
    auto orig = (SendTo_t)origOf(kSendTo);
    if (!orig) { SetLastError(WSAENETDOWN); return SOCKET_ERROR; }
    if (to) {
        if (guarding() && !destAllowed(to, tolen)) {
            noteDest("sendto", to, tolen, true, NG_RETADDR());
            SetLastError(WSAENETUNREACH);
            return SOCKET_ERROR;
        }
        noteDest("sendto", to, tolen, false, NG_RETADDR());
    }
    return orig(s, buf, blen, flags, to, tolen);
}

void* WINAPI hkInternetConnectW(void* hi, LPCWSTR server, WORD port, LPCWSTR user, LPCWSTR pass,
                                DWORD svc, DWORD flags, DWORD_PTR ctx) {
    InFlight f;
    auto orig = (InternetConnectW_t)origOf(kInternetConnectW);
    if (!orig) { SetLastError(ERROR_INVALID_FUNCTION); return nullptr; }
    if (server && *server) {
        char n[300] = {};
        bool truncated = false;
        narrow(server, n, sizeof(n), truncated);
        const bool block = guarding() && (truncated || IsBlockedHost(n));
        noteName("InternetConnectW", n, block, NG_RETADDR());
        if (block) return orig(hi, L"127.0.0.1", port, user, pass, svc, flags, ctx);
    }
    return orig(hi, server, port, user, pass, svc, flags, ctx);
}

struct FnDef { int dll; const char* name; WORD ordinal; void* hook; bool requiredInMain; };
const FnDef kFns[kFnCount] = {
    { kWs2,     "getaddrinfo",      0,  (void*)&hkGetAddrInfo,      true  },
    { kWs2,     "GetAddrInfoW",     0,  (void*)&hkGetAddrInfoW,     false },
    { kWs2,     "gethostbyname",    52, (void*)&hkGetHostByName,    false },
    { kWs2,     "connect",          4,  (void*)&hkConnect,          true  },
    { kWs2,     "sendto",           20, (void*)&hkSendTo,           true  },
    { kWinInet, "InternetConnectW", 0,  (void*)&hkInternetConnectW, false },
};

// ---------------------------------------------------------------------------
// Import-table patching
// ---------------------------------------------------------------------------
struct Slot { void** slot; int fn; int mod; bool lost; int repatches; };
struct ModTrack { const wchar_t* name; HMODULE base; bool scanned; char label[64]; };
SRWLOCK g_slotLock = SRWLOCK_INIT;
std::vector<Slot> g_slots;
ModTrack g_mods[] = {
    { nullptr,            nullptr, false, "main" },          // GTA5.exe (label filled from the path)
    { L"libcurl.dll",     nullptr, false, "libcurl.dll" },   // the game's HTTP client (ROS calls)
    { L"socialclub.dll",  nullptr, false, "socialclub.dll" },
};
constexpr int kModCount = (int)(sizeof(g_mods) / sizeof(g_mods[0]));
constexpr int kMaxRepatch = 5;

bool readable(const void* p, size_t len) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// Swap one pointer-sized import slot. Keeps an executable page executable (packed images can
// merge sections), writes atomically so a racing caller sees the old or the new pointer, and
// restores the page protection.
bool writeSlot(void** slot, void* v) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(slot, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD want = (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), want, &old)) return false;
    InterlockedExchangePointer((PVOID volatile*)slot, v);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

int dllKind(const char* name) {
    char b[32];
    size_t i = 0;
    for (; name[i] && i < sizeof(b) - 1; ++i) {
        char c = name[i];
        b[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    b[i] = '\0';
    if (strcmp(b, "ws2_32.dll") == 0) return kWs2;
    if (strcmp(b, "wininet.dll") == 0) return kWinInet;
    return -1;
}

void resolveExports() {
    HMODULE dlls[2] = { GetModuleHandleW(L"ws2_32.dll"), GetModuleHandleW(L"wininet.dll") };
    for (int f = 0; f < kFnCount; ++f) {
        if (g_export[f].load()) continue;
        HMODULE d = dlls[kFns[f].dll];
        if (d) g_export[f].store((void*)GetProcAddress(d, kFns[f].name));
    }
}

// Which hooked function an import slot is: by its current pointer (the export), else by the
// import name / ordinal in the lookup table. -1 = not one of ours.
int classify(int dll, void* cur, const uint8_t* base, const IMAGE_THUNK_DATA64* lookup) {
    for (int f = 0; f < kFnCount; ++f)
        if (kFns[f].dll == dll && cur && cur == g_export[f].load()) return f;
    if (!lookup || !readable(lookup, sizeof(*lookup)) || !lookup->u1.AddressOfData) return -1;
    if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
        const WORD ord = (WORD)IMAGE_ORDINAL64(lookup->u1.Ordinal);
        for (int f = 0; f < kFnCount; ++f)
            if (kFns[f].dll == dll && kFns[f].ordinal && kFns[f].ordinal == ord) return f;
        return -1;
    }
    const IMAGE_IMPORT_BY_NAME* ibn = (const IMAGE_IMPORT_BY_NAME*)(base + lookup->u1.AddressOfData);
    if (!readable(ibn, sizeof(*ibn) + 32)) return -1;
    for (int f = 0; f < kFnCount; ++f)
        if (kFns[f].dll == dll && strcmp((const char*)ibn->Name, kFns[f].name) == 0) return f;
    return -1;
}

// Hook every ws2_32 / wininet import of ours in module `mi`. Caller holds g_slotLock exclusively.
void scanModule(int mi) {
    ModTrack& m = g_mods[mi];
    const uint8_t* base = (const uint8_t*)m.base;
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        GLOGW("netguard: %s: PE headers unreadable; not hooked", m.label);
        return;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        GLOGW("netguard: %s: no import directory; not hooked", m.label);
        return;
    }
    std::string hooked;
    int count = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         readable(d, sizeof(*d)) && d->Name; ++d) {
        const char* dllName = (const char*)(base + d->Name);
        if (!readable(dllName, 16)) continue;
        const int dll = dllKind(dllName);
        if (dll < 0 || !d->FirstThunk) continue;
        IMAGE_THUNK_DATA64* iat = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
        const IMAGE_THUNK_DATA64* look =
            d->OriginalFirstThunk ? (const IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk) : nullptr;
        for (size_t i = 0; i < 4096 && readable(&iat[i], sizeof(iat[i])) && iat[i].u1.Function; ++i) {
            void** slot = (void**)&iat[i].u1.Function;
            void* cur = *slot;
            const int f = classify(dll, cur, base, look ? look + i : nullptr);
            if (f < 0) continue;
            if (cur == kFns[f].hook) continue;   // already ours
            void* expected = g_orig[f].load();
            if (expected && cur != expected && cur != g_export[f].load()) {
                g_skippedForeign.fetch_add(1);
                GLOGW("netguard: %s: %s slot holds a foreign pointer %p (not the export, not the first "
                      "original); not hooked -- the guard counts as NOT active", m.label, kFns[f].name, cur);
                continue;
            }
            if (!expected) g_orig[f].store(cur);
            if (!writeSlot(slot, kFns[f].hook)) {
                g_skippedForeign.fetch_add(1);
                GLOGW("netguard: %s: %s slot write failed (err %lu); the guard counts as NOT active",
                      m.label, kFns[f].name, GetLastError());
                continue;
            }
            g_slots.push_back(Slot{ slot, f, mi, false, 0 });
            if (!hooked.empty()) hooked += ", ";
            hooked += kFns[f].name;
            ++count;
        }
    }
    GLOGI("netguard: %s @ %p: hooked %d import slot(s)%s%s", m.label, (void*)base, count,
          count ? ": " : "", hooked.c_str());
}

// One maintenance pass (Install, then the thread): (re)scan modules that appeared or moved, and
// check every slot still holds our hook.
void maintain() {
    AcquireSRWLockExclusive(&g_slotLock);
    if (g_stop.load()) { ReleaseSRWLockExclusive(&g_slotLock); return; }
    resolveExports();
    for (int mi = 0; mi < kModCount; ++mi) {
        ModTrack& m = g_mods[mi];
        HMODULE h = m.name ? GetModuleHandleW(m.name) : GetModuleHandleW(nullptr);
        if (h != m.base) {
            if (m.base) {
                size_t dropped = 0;
                for (size_t i = 0; i < g_slots.size();) {
                    if (g_slots[i].mod == mi) { g_slots.erase(g_slots.begin() + (ptrdiff_t)i); ++dropped; }
                    else ++i;
                }
                GLOGI("netguard: %s unloaded or moved; dropped %u slot(s)", m.label, (unsigned)dropped);
            }
            m.base = h;
            m.scanned = false;
        }
        if (h && !m.scanned) {
            if (!m.name) {
                wchar_t path[MAX_PATH] = {};
                const DWORD L = GetModuleFileNameW(nullptr, path, MAX_PATH);
                const wchar_t* b = path;
                for (DWORD i = 0; i < L; ++i) if (path[i] == L'\\' || path[i] == L'/') b = path + i + 1;
                bool t = false;
                narrow(b, m.label, sizeof(m.label), t);
            }
            scanModule(mi);
            m.scanned = true;
        }
    }
    const bool guard = guarding();
    for (Slot& s : g_slots) {
        if (s.lost) continue;
        if (!readable(s.slot, sizeof(void*))) { s.lost = true; continue; }
        void* cur = *s.slot;
        if (cur == kFns[s.fn].hook) continue;
        if (guard && s.repatches < kMaxRepatch && writeSlot(s.slot, kFns[s.fn].hook)) {
            ++s.repatches;
            GLOGW("netguard: %s %s slot was rewritten (now %p); re-patched (%d/%d)", g_mods[s.mod].label,
                  kFns[s.fn].name, cur, s.repatches, kMaxRepatch);
        } else {
            s.lost = true;
            GLOGW("netguard: %s %s slot was rewritten (now %p); %s", g_mods[s.mod].label, kFns[s.fn].name, cur,
                  guard ? "re-patch budget spent -- the guard is NOT active"
                        : "probe hook given up (read-only probe: not re-patched)");
        }
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

DWORD WINAPI maintThread(void*) {
    const ULONGLONG start = GetTickCount64();
    while (!g_stop.load()) {
        maintain();
        Sleep(GetTickCount64() - start < 600000 ? 1000 : 5000);
    }
    return 0;
}

bool mainRequiredHooked() {   // caller holds g_slotLock (shared or exclusive)
    bool has[kFnCount] = {};
    for (const Slot& s : g_slots)
        if (s.mod == 0 && !s.lost && readable(s.slot, sizeof(void*)) && *s.slot == kFns[s.fn].hook) has[s.fn] = true;
    for (int f = 0; f < kFnCount; ++f)
        if (kFns[f].requiredInMain && !has[f]) return false;
    return true;
}

} // namespace

bool Install(Mode mode) {
    if (mode == Mode::Off) return false;
    if (g_installed.load()) return mode == Mode::Guard ? GuardActive() : true;
    g_mode.store((int)mode);
    g_stop.store(false);
    maintain();
    g_installed.store(true);
    AcquireSRWLockShared(&g_slotLock);
    const bool ok = mainRequiredHooked();
    const size_t n = g_slots.size();
    ReleaseSRWLockShared(&g_slotLock);
    GLOGI("netguard: installed in %s mode: %u import slot(s) hooked; main module getaddrinfo+connect+sendto %s",
          mode == Mode::Guard ? "GUARD (net_host=1: Rockstar hosts -> loopback, unknown destinations refused)"
                              : "PROBE (read-only: log names and destinations)",
          (unsigned)n, ok ? "hooked" : "NOT all hooked");
    HANDLE t = CreateThread(nullptr, 0, maintThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else GLOGW("netguard: maintenance thread failed to start (late modules not hooked, rewrites not noticed)");
    return ok;
}

void Uninstall() {
    if (!g_installed.load()) return;
    g_stop.store(true);
    unsigned restored = 0, left = 0;
    AcquireSRWLockExclusive(&g_slotLock);
    for (const Slot& s : g_slots) {
        const ModTrack& m = g_mods[s.mod];
        const HMODULE now = m.name ? GetModuleHandleW(m.name) : GetModuleHandleW(nullptr);
        void* orig = g_orig[s.fn].load();
        if (now == m.base && readable(s.slot, sizeof(void*)) && *s.slot == kFns[s.fn].hook && orig &&
            writeSlot(s.slot, orig))
            ++restored;
        else
            ++left;
    }
    g_slots.clear();
    for (ModTrack& m : g_mods) { m.base = nullptr; m.scanned = false; }
    ReleaseSRWLockExclusive(&g_slotLock);
    // Let a call already inside a hook return (a DNS lookup can block longer; it still returns
    // through our code, which stays mapped while the engine DLL is loaded).
    for (int i = 0; i < 25 && g_inFlight.load() > 0; ++i) Sleep(10);
    g_mode.store((int)Mode::Off);
    g_installed.store(false);
    GLOGI("netguard: uninstalled: %u slot(s) restored, %u left (module gone or slot no longer ours)",
          restored, left);
}

bool GuardActive() {
    if (g_mode.load() != (int)Mode::Guard || !g_installed.load() || g_stop.load()) return false;
    if (g_skippedForeign.load()) return false;
    AcquireSRWLockShared(&g_slotLock);
    bool ok = mainRequiredHooked();
    for (const Slot& s : g_slots) {
        if (!ok) break;
        if (s.lost || !readable(s.slot, sizeof(void*)) || *s.slot != kFns[s.fn].hook) ok = false;
    }
    ReleaseSRWLockShared(&g_slotLock);
    return ok;
}

Stats GetStats() {
    Stats st{};
    AcquireSRWLockShared(&g_seenLock);
    st.names = (unsigned)g_names.size();
    if (st.names > kNameCap) st.names = (unsigned)kNameCap;
    ReleaseSRWLockShared(&g_seenLock);
    st.namesBlocked = g_namesBlocked.load();
    st.addrsBlocked = g_addrsBlocked.load();
    AcquireSRWLockShared(&g_slotLock);
    st.slots = (unsigned)g_slots.size();
    for (const Slot& s : g_slots) if (s.lost) ++st.slotsLost;
    ReleaseSRWLockShared(&g_slotLock);
    return st;
}

} // namespace gage::netguard
