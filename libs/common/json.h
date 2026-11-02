#pragma once
#ifndef GAGE_COMMON_JSON_H
#define GAGE_COMMON_JSON_H
// Minimal JSON reader shared by the launcher (JS <-> C++ bridge) and the
// server-list fetcher. Header-only, -fno-exceptions, no dependencies.
//
// JVal / JParser / pushUtf8 live at global scope on purpose: they were moved
// verbatim out of apps/launcher/gage_gui.cpp, which includes this header instead.
//
// Hardening over the original copy:
//   * nesting deeper than JParser::kMaxDepth (64) makes parse() fail;
//   * optional JParser::maxNodes budget bounds memory for hostile bodies;
//   * numbers must be consumed completely by strtod and be finite;
//   * lone UTF-16 surrogates in \u escapes decode to U+FFFD (never emit an
//     invalid UTF-8 sequence).
// parse() does not require the buffer to be NUL-terminated.

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj };
    T t;
    bool b;
    double n;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    JVal() : t(Null), b(false), n(0.0) {}
};

inline void pushUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s.push_back((char)cp);
    } else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (cp >> 18)));
        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct JParser {
    enum { kMaxDepth = 64 };  // object/array nesting limit; deeper => parse fails

    const char* p = 0;
    const char* end = 0;
    int depth = 0;
    size_t nodes = 0;
    size_t maxNodes = 0;      // 0 = unlimited; otherwise parse fails past this many values

    bool parse(const char* buf, size_t len) {
        p = buf; end = buf + len;
        depth = 0; nodes = 0;
        v = JVal();
        return Parse(v);
    }
    JVal v;

    bool ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return true; }

    bool Parse(JVal& out) {
        ws();
        if (p >= end) return false;
        if (maxNodes && ++nodes > maxNodes) return false;
        char c = *p;
        if (c == '{' || c == '[') {
            if (depth >= kMaxDepth) return false;
            depth++;
            bool ok = (c == '{') ? ParseObj(out) : ParseArr(out);
            depth--;
            return ok;
        }
        if (c == '"') { out.t = JVal::Str; return ParseStr(out.s); }
        if (c == 't' || c == 'f') return ParseBool(out);
        if (c == 'n') {
            if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') { p += 4; out.t = JVal::Null; return true; }
            return false;
        }
        return ParseNum(out);
    }

    bool ParseObj(JVal& out) {
        out.t = JVal::Obj;
        p++;  // '{'
        ws();
        if (p < end && *p == '}') { p++; return true; }
        for (;;) {
            ws();
            if (p >= end || *p != '"') return false;
            std::string key;
            if (!ParseStr(key)) return false;
            ws();
            if (p >= end || *p != ':') return false;
            p++;
            JVal child;
            if (!Parse(child)) return false;
            out.o.push_back(std::make_pair(std::move(key), std::move(child)));
            ws();
            if (p >= end) return false;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
    }

    bool ParseArr(JVal& out) {
        out.t = JVal::Arr;
        p++;  // '['
        ws();
        if (p < end && *p == ']') { p++; return true; }
        for (;;) {
            JVal child;
            if (!Parse(child)) return false;
            out.a.push_back(std::move(child));
            ws();
            if (p >= end) return false;
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
    }

    bool ParseStr(std::string& s) {
        p++;  // '"'
        s.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { p++; return true; }
            if (c != '\\') { s.push_back((char)c); p++; continue; }
            p++;
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) return false;
                    unsigned cp = hex4(p);
                    if (cp == 0xFFFFu) return false;
                    p += 4;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate: needs an immediately following low one.
                        bool paired = false;
                        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            unsigned lo = hex4(p + 2);
                            if (lo != 0xFFFFu && lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                                p += 6;
                                paired = true;
                            }
                        }
                        if (!paired) cp = 0xFFFDu;
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        cp = 0xFFFDu;  // lone low surrogate
                    }
                    pushUtf8(s, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    unsigned hex4(const char* q) {
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            char c = q[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0xFFFFu;
        }
        return v;
    }

    bool ParseBool(JVal& out) {
        if (end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') { p += 4; out.t = JVal::Bool; out.b = true; return true; }
        if (end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') { p += 5; out.t = JVal::Bool; out.b = false; return true; }
        return false;
    }

    bool ParseNum(JVal& out) {
        const char* s = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) { p++; any = true; }
        if (!any) return false;
        // strtod on a private NUL-terminated copy of exactly the scanned span:
        // the caller's buffer need not be terminated, and a prefix parse
        // ("1-2", "0x10") must not silently succeed.
        std::string tok(s, (size_t)(p - s));
        char* e = 0;
        double d = strtod(tok.c_str(), &e);
        if (e != tok.c_str() + tok.size() || !std::isfinite(d)) return false;
        out.t = JVal::Num;
        out.n = d;
        return true;
    }
};

#endif  // GAGE_COMMON_JSON_H
