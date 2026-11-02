// net_guard.h -- hostname probe (G0) and ROS guard (S0) for the game's own network imports.
//
// IAT hooks on the import tables of GTA5.exe and the in-process modules that talk to Rockstar on
// their own (libcurl.dll, socialclub.dll): getaddrinfo, GetAddrInfoW, gethostbyname, connect,
// sendto (ws2_32) and InternetConnectW (wininet). Only import-table POINTERS are written, never
// code bytes; every slot is restored on Uninstall (World::Unhook).
//
//   Probe (net_probe=1, read-only): log each distinct hostname the game resolves and each distinct
//          address it connects/sends to (once each, capped), then call the original unchanged.
//   Guard (net_host=1): additionally, a Rockstar hostname (IsBlockedHost) resolves to 127.0.0.1 /
//          ::1, and connect/sendto to any non-loopback address that did NOT come from a
//          non-Rockstar name resolved through these hooks fails with WSAENETUNREACH (covers
//          addresses the game resolved before the hooks went in). Each block is logged once.
//
// GuardActive() is the S0 gate: net_host may only drive the game's session code while it is true.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gage::netguard {

enum class Mode { Off, Probe, Guard };

// Install the import hooks once (Init). Guard implies everything Probe logs. Returns true when the
// main module's required slots (getaddrinfo, connect, sendto) are hooked. Starts a maintenance
// thread that hooks libcurl.dll / socialclub.dll when they load and notices a slot the game
// rewrote (logged; re-patched in Guard mode, given up in Probe mode).
bool Install(Mode mode);
// Restore every slot that still holds our pointer and stop the maintenance thread. Idempotent.
void Uninstall();
// Guard mode, and every guarded slot found in a loaded module currently holds our hook (re-read
// live), and the main module's getaddrinfo/connect/sendto were found. False in Probe/Off mode.
bool GuardActive();

struct Stats {
    unsigned names;           // distinct hostnames seen
    unsigned namesBlocked;    // of which blocked (Guard)
    unsigned addrsBlocked;    // distinct connect/sendto destinations blocked (Guard)
    unsigned slots;           // import slots currently tracked
    unsigned slotsLost;       // slots the game rewrote and we no longer hold
};
Stats GetStats();

// ---------------------------------------------------------------------------
// Pure matchers (header-only so a scratch test can include them without Windows).
// ---------------------------------------------------------------------------

// A Rockstar Online Services / Social Club / matchmaking host: any name containing
// "rockstargames" (covers *.rockstargames.com, *.rockstargames.com.cdn..., sc*.rockstargames.*,
// rockstargames.cn) or starting with "ros-" / "ros.". Case-insensitive; a trailing dot is ignored.
// Fails closed: a name longer than 253 characters (not a valid DNS name) counts as blocked.
inline bool IsBlockedHost(const char* host) {
    if (!host || !*host) return false;
    char h[256];
    size_t n = 0;
    for (; host[n]; ++n) {
        if (n >= 253) return true;
        char c = host[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h[n] = c;
    }
    while (n > 0 && h[n - 1] == '.') --n;
    h[n] = '\0';
    if (n == 0) return false;
    static const char kNeedle[] = "rockstargames";
    const size_t L = sizeof(kNeedle) - 1;
    for (size_t i = 0; i + L <= n; ++i) {
        size_t k = 0;
        while (k < L && h[i + k] == kNeedle[k]) ++k;
        if (k == L) return true;
    }
    if (n >= 4 && h[0] == 'r' && h[1] == 'o' && h[2] == 's' && (h[3] == '-' || h[3] == '.')) return true;
    return false;
}

// True when `s` is a numeric address literal (IPv4 dotted digits, or anything with ':' = IPv6),
// i.e. not a hostname: its result is never added to the guard's allow list.
inline bool IsAddressLiteral(const char* s) {
    if (!s || !*s) return false;
    bool digitsDots = true;
    for (const char* p = s; *p; ++p) {
        if (*p == ':') return true;
        if (!((*p >= '0' && *p <= '9') || *p == '.')) digitsDots = false;
    }
    return digitsDots;
}

// IPv4 in network byte order as 4 bytes; 127.0.0.0/8 is loopback.
inline bool IsLoopbackV4(const uint8_t a[4]) { return a[0] == 127; }
// IPv6 16 bytes: ::1, or IPv4-mapped ::ffff:127.x.x.x.
inline bool IsLoopbackV6(const uint8_t a[16]) {
    bool zero10 = true;
    for (int i = 0; i < 10; ++i) if (a[i]) { zero10 = false; break; }
    if (zero10 && a[10] == 0 && a[11] == 0 && a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] == 1) return true;
    return zero10 && a[10] == 0xFF && a[11] == 0xFF && a[12] == 127;
}

} // namespace gage::netguard
