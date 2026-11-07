#pragma once
// The servers the launcher's status worker asks by UDP query (bridge `queryServers`, BRIDGE.md): the boundary check of the
// list the SPA sends, pure and header-only so a selftest can pin it (tests/version_gate_test.cpp). gage_gui.cpp reads the
// JSON into Raw entries, keeps what Validate keeps, and logs every reason it gives for the rest.

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace gage::qtargets {

    inline constexpr size_t kMaxTargets = 16;      // a listed server each; the platform lists 3 today (SOL, LUNA, TERRA)
    inline constexpr size_t kMaxHostLen = 253;     // the DNS name limit; an IPv6 literal is at most 45

    struct Target
    {
        std::string host;   // a hostname, an IPv4 literal or a bare IPv6 literal (no brackets), as net::QueryServer takes it
        int port = 0;       // 1..65535

        std::string Key() const { return host + ":" + std::to_string(port); }
    };

    // One `servers[]` element as the bridge read it: whether each field had the right JSON type, and its value.
    struct Raw
    {
        bool hostIsString = false;
        std::string host;
        bool portIsNumber = false;
        double port = 0;
    };

    struct Result
    {
        std::vector<Target> targets;        // what the worker polls, in the order sent, duplicates removed
        std::vector<std::string> dropped;   // one line per refused entry: "#<index>: <reason>" (never the raw host)
    };

    inline bool IsAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    inline bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

    // A DNS name or an IPv4 literal: dot-separated labels of 1..63 [A-Za-z0-9-], none starting or ending with '-',
    // at most kMaxHostLen in all, no empty label (so no leading, trailing or double dot).
    inline bool ValidHostname(const std::string& h)
    {
        if (h.empty() || h.size() > kMaxHostLen) return false;
        size_t start = 0;
        while (start <= h.size())
        {
            size_t end = h.find('.', start);
            if (end == std::string::npos) end = h.size();
            const size_t len = end - start;
            if (len == 0 || len > 63) return false;
            if (h[start] == '-' || h[end - 1] == '-') return false;
            for (size_t i = start; i < end; ++i)
                if (!(IsAsciiAlnum(h[i]) || h[i] == '-')) return false;
            start = end + 1;
        }
        return true;
    }

    // A bare IPv6 literal, plausibly: 2..45 characters of hex digits, ':' and '.' (an embedded IPv4 tail), 2..7 colons,
    // no ":::" and at most one "::". getaddrinfo is the real judge; this keeps anything that is not shaped like one out.
    inline bool ValidIpv6(const std::string& h)
    {
        if (h.size() < 2 || h.size() > 45) return false;
        int colons = 0;
        for (char c : h)
        {
            if (c == ':') ++colons;
            else if (!(IsHex(c) || c == '.')) return false;
        }
        if (colons < 2 || colons > 7) return false;
        if (h.find(":::") != std::string::npos) return false;
        const size_t dbl = h.find("::");
        return dbl == std::string::npos || h.find("::", dbl + 1) == std::string::npos;
    }

    inline bool ValidHost(const std::string& h) { return h.find(':') != std::string::npos ? ValidIpv6(h) : ValidHostname(h); }

    inline bool ValidPort(double p) { return std::isfinite(p) && p >= 1 && p <= 65535 && p == std::floor(p); }

    // The first kMaxTargets entries are looked at; the rest are dropped in ONE line (counted, not judged), so an
    // oversized list cannot flood gage.log. A host is compared case-insensitively for duplicates, so "SOL.example" and
    // "sol.example" on one port are one target.
    inline Result Validate(const std::vector<Raw>& in)
    {
        Result r;
        std::vector<std::string> seen;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const std::string at = "#" + std::to_string(i) + ": ";
            if (i >= kMaxTargets)
            {
                r.dropped.push_back("#" + std::to_string(i) + "-#" + std::to_string(in.size() - 1) + ": " + std::to_string(in.size() - i) +
                                    " over the limit of " + std::to_string(kMaxTargets) + " servers");
                break;
            }
            const Raw& e = in[i];
            if (!e.hostIsString) { r.dropped.push_back(at + "host is not a string"); continue; }
            if (!ValidHost(e.host))
            {
                r.dropped.push_back(at + "host is not a hostname, IPv4 or IPv6 literal of at most " + std::to_string(kMaxHostLen) +
                                    " characters (length " + std::to_string(e.host.size()) + ")");
                continue;
            }
            if (!e.portIsNumber || !ValidPort(e.port)) { r.dropped.push_back(at + "port is not a whole number in 1..65535"); continue; }
            Target t{e.host, (int)e.port};
            std::string folded = t.Key();
            for (char& c : folded)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            bool dup = false;
            for (const std::string& s : seen) dup = dup || s == folded;
            if (dup) { r.dropped.push_back(at + "a duplicate of an earlier server"); continue; }
            seen.push_back(folded);
            r.targets.push_back(t);
        }
        return r;
    }

}
