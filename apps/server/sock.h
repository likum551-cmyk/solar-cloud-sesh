// A deliberately tiny TCP socket shim: Winsock on Windows, BSD/POSIX sockets
// elsewhere, so the HTTP announce client and the admin channel are written once.
// It is only the handful of calls those two need (connect with a deadline, a
// loopback listener, non-blocking send/recv, wait), no platform header leaks out.
// C++17, no exceptions. The game traffic itself stays on ENet's own UDP socket.
//
// Thread-safety: Init()/Cleanup() are reference counted and may be called from any
// thread. Every other function works on the handle it is given and has no shared
// state, so different handles may be used from different threads at once; one handle
// must not be used from two threads at the same time. Errors: no exceptions; failures
// come back as a return value (false, kInvalid or Io::Error), with a human-readable
// reason in *err where the function has that parameter.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gage::server::sock {

    // A socket: a SOCKET on Windows, a file descriptor elsewhere, widened to one integer
    // type so callers need no platform header.
    using Handle = uintptr_t;
    // The "no socket" value (what the creating functions return on failure).
    constexpr Handle kInvalid = ~static_cast<Handle>(0);

    // Reference-counted WSAStartup on Windows (a no-op elsewhere). Every successful
    // Init() must be paired with one Cleanup(). Init() returns false when the network
    // stack cannot be started.
    bool Init();
    void Cleanup();

    // Closes `h`; a kInvalid handle is ignored.
    void Close(Handle h);
    // Sends FIN after what is queued (the peer reads EOF) while still receiving, so a
    // final reply is not lost to the reset that closing over unread input causes.
    void ShutdownSend(Handle h);
    // Switches `h` between non-blocking (true) and blocking mode. Returns false when
    // the OS refuses.
    bool SetNonBlocking(Handle h, bool on);

    // The outcome of one Recv/Send.
    //   Ok         some bytes moved (*got / *sent says how many, always > 0)
    //   WouldBlock nothing could move right now (non-blocking socket): try again later
    //   Closed     the peer shut the connection down in an orderly way (Recv only)
    //   Error      the connection failed (reset, broken pipe, bad handle)
    enum class Io { Ok, WouldBlock, Closed, Error };
    // Reads up to `cap` bytes into `buf`. Ok with *got > 0; Closed on an orderly
    // shutdown by the peer.
    Io Recv(Handle h, void* buf, size_t cap, size_t* got);
    // Writes up to `n` bytes from `buf` (possibly fewer: loop on *sent). Never raises
    // SIGPIPE (MSG_NOSIGNAL where the platform has it).
    Io Send(Handle h, const void* buf, size_t n, size_t* sent);

    // Waits until `h` is readable (or writable, when forWrite) for at most timeoutMs:
    // 1 ready, 0 timeout, -1 error. A peer that closed or reset the connection makes the
    // socket "readable" (the next Recv then reports Closed or Error). Blocks the calling
    // thread for up to timeoutMs.
    int Wait(Handle h, bool forWrite, int timeoutMs);

    // Resolves `host` (name, IPv4 or IPv6 literal without brackets) and connects with
    // a non-blocking connect bounded by timeoutMs overall; `cancel`, when set to true
    // meanwhile, aborts within about 100 ms. The returned socket is non-blocking.
    // Name resolution itself (getaddrinfo) is blocking and cannot be interrupted.
    // Returns kInvalid with *err ("cannot resolve <host>", "connect(): <os reason>",
    // "connect timed out", "cancelled") on failure; with several resolved addresses the
    // reason is the last one tried. Blocks the calling thread (use a worker).
    Handle ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, const std::atomic<bool>* cancel,
                      std::string* err);

    // A listening socket bound to 127.0.0.1 ONLY (port 0 = ephemeral; *bound gets the
    // real port), non-blocking. On POSIX it sets SO_REUSEADDR so a restarted server can
    // rebind at once; on Windows it deliberately does not (there it would allow port
    // hijacking). Returns kInvalid with *err ("bind 127.0.0.1:<port>: <os reason>",
    // "listen(): ...", "socket(): ...") on failure.
    Handle ListenLoopback(uint16_t port, uint16_t* bound, std::string* err);
    // The address a socket is bound to, dotted IPv4, and its port (for tests and
    // diagnostics). Returns false when the OS cannot say or it is not an IPv4 socket.
    bool LocalAddress(Handle h, std::string* ip, uint16_t* port);
    // One pending connection from a listener, or kInvalid when none is waiting. The
    // accepted socket is non-blocking; *peerHost is the peer's IPv4 address, host order
    // (0 when it is not IPv4). Never blocks.
    Handle Accept(Handle listener, uint32_t* peerHost);

    // This machine's outbound IPv4 interface address, found by "connecting" a UDP
    // socket to a documentation address (no packet is sent). False when there is no
    // route. Used by tests to try the admin port on a non-loopback address.
    bool OutboundIpv4(std::string* ip);

}
