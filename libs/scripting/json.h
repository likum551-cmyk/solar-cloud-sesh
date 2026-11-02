// Strict JSON (RFC 8259) for the scripting runtime: one streaming parser, used both to build a
// DOM (manifests, config, storage files) and to build Lua values directly (json.decode).
// C++17, std only, no exceptions, no recursion beyond Limits::maxDepth.
//
// Strictness: no comments, no trailing commas, no leading zeros, no NaN/Infinity, strings must
// be valid UTF-8 with no raw control characters, lone surrogate escapes are errors, and the
// text must be exactly one value (surrounding whitespace allowed). Numbers without a
// fraction or exponent that fit in int64 are reported as integers, every other number as a
// double.
//
// Thread-safety: no shared state; a Parser object is single-use per thread.
#pragma once

#include "fsutil.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gage::scripting::json {

    // Where and why a parse stopped. `message` is a short fixed text, `offset` the byte index.
    struct ParseError
    {
        char message[96] = {0};
        size_t offset = 0;
    };

    // Hard limits that keep a hostile document from using unbounded stack or memory.
    struct Limits
    {
        int maxDepth = 64;          // nesting of arrays/objects
        size_t maxNodes = 1000000;  // values in the document
    };

    // `d` as the shortest decimal text that reads back as exactly `d`, always containing a '.'
    // or an exponent so it re-parses as a double. `d` must be finite. Writes into `buf`
    // (at least 40 bytes) and returns the length.
    inline size_t FormatDouble(double d, char* buf)
    {
        for (int prec = 15; prec <= 17; prec++)
        {
            std::snprintf(buf, 40, "%.*g", prec, d);
            if (prec == 17 || std::strtod(buf, nullptr) == d) break;
        }
        size_t n = std::strlen(buf);
        bool looksInt = true;
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') looksInt = false;
        if (looksInt && n + 2 < 40)
        {
            buf[n++] = '.';
            buf[n++] = '0';
            buf[n] = 0;
        }
        return n;
    }

    // The streaming parser. H is a handler with these members, each returning false to abort
    // the parse (reported as "aborted by handler"):
    //   bool Null();            bool Bool(bool v);        bool Int(long long v);   bool Num(double v);
    //   bool Str(const char* p, size_t n);                 // p is valid only during the call
    //   bool BeginArr();        bool EndArr();
    //   bool BeginObj();        bool Key(const char* p, size_t n);   // p valid only during the call
    //   bool EndObj();
    // `scratch` is the buffer used to decode escaped strings; passing one owned by the caller
    // keeps this class free of members that need a destructor (it may run in code that a Lua
    // error can longjmp out of).
    template <class H>
    class Parser
    {
    public:
        Parser(H& handler, std::string& scratch, const Limits& limits) : h_(handler), scratch_(scratch), lim_(limits) {}

        // Parses the whole of [text, text+n). On false, `err` says why.
        bool Parse(const char* text, size_t n, ParseError& err)
        {
            begin_ = p_ = text;
            end_ = text + n;
            depth_ = 0;
            nodes_ = 0;
            err_ = &err;
            err.message[0] = 0;
            err.offset = 0;
            if (!ParseValue()) return false;
            SkipWs();
            if (p_ != end_) return Fail("unexpected data after the value");
            return true;
        }

    private:
        H& h_;
        std::string& scratch_;
        Limits lim_;
        const char* begin_ = nullptr;
        const char* p_ = nullptr;
        const char* end_ = nullptr;
        int depth_ = 0;
        size_t nodes_ = 0;
        ParseError* err_ = nullptr;

        bool Fail(const char* msg)
        {
            if (err_ && err_->message[0] == 0)
            {
                std::snprintf(err_->message, sizeof(err_->message), "%s", msg);
                err_->offset = static_cast<size_t>(p_ - begin_);
            }
            return false;
        }

        void SkipWs()
        {
            while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
        }

        bool Call(bool ok) { return ok ? true : Fail("aborted by handler"); }

        bool ParseValue()
        {
            SkipWs();
            if (p_ >= end_) return Fail("unexpected end of input");
            if (++nodes_ > lim_.maxNodes) return Fail("too many values");
            switch (*p_)
            {
            case '{': return ParseObject();
            case '[': return ParseArray();
            case '"': return ParseStringValue();
            case 't': return Literal("true", 4) && Call(h_.Bool(true));
            case 'f': return Literal("false", 5) && Call(h_.Bool(false));
            case 'n': return Literal("null", 4) && Call(h_.Null());
            default:
                if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9')) return ParseNumber();
                return Fail("unexpected character");
            }
        }

        bool Literal(const char* word, size_t len)
        {
            if (static_cast<size_t>(end_ - p_) < len || std::memcmp(p_, word, len) != 0) return Fail("invalid literal");
            p_ += len;
            return true;
        }

        bool ParseNumber()
        {
            const char* s = p_;
            if (*p_ == '-') p_++;
            if (p_ >= end_) return Fail("invalid number");
            if (*p_ == '0')
                p_++;
            else if (*p_ >= '1' && *p_ <= '9')
                while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
            else
                return Fail("invalid number");
            bool isInt = true;
            if (p_ < end_ && *p_ == '.')
            {
                isInt = false;
                p_++;
                if (p_ >= end_ || *p_ < '0' || *p_ > '9') return Fail("invalid number");
                while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
            }
            if (p_ < end_ && (*p_ == 'e' || *p_ == 'E'))
            {
                isInt = false;
                p_++;
                if (p_ < end_ && (*p_ == '+' || *p_ == '-')) p_++;
                if (p_ >= end_ || *p_ < '0' || *p_ > '9') return Fail("invalid number");
                while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
            }
            const size_t len = static_cast<size_t>(p_ - s);
            if (len > 64) return Fail("number is too long");
            char tok[72];
            std::memcpy(tok, s, len);
            tok[len] = 0;
            if (isInt)
            {
                // Accumulate negatively so INT64_MIN is representable; overflow falls back to double.
                const bool neg = tok[0] == '-';
                unsigned long long acc = 0;
                bool overflow = false;
                for (const char* q = tok + (neg ? 1 : 0); *q; q++)
                {
                    const unsigned d = static_cast<unsigned>(*q - '0');
                    if (acc > (0xFFFFFFFFFFFFFFFFull - d) / 10) { overflow = true; break; }
                    acc = acc * 10 + d;
                }
                if (!overflow && (neg ? acc <= 0x8000000000000000ull : acc <= 0x7FFFFFFFFFFFFFFFull))
                {
                    const long long v = neg ? static_cast<long long>(0ull - acc) : static_cast<long long>(acc);
                    return Call(h_.Int(v));
                }
            }
            const double d = std::strtod(tok, nullptr);
            if (!std::isfinite(d)) return Fail("number out of range");
            return Call(h_.Num(d));
        }

        static int Hex(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        bool Hex4(unsigned& out)
        {
            if (end_ - p_ < 4) return false;
            unsigned v = 0;
            for (int i = 0; i < 4; i++)
            {
                const int h = Hex(p_[i]);
                if (h < 0) return false;
                v = (v << 4) | static_cast<unsigned>(h);
            }
            p_ += 4;
            out = v;
            return true;
        }

        static void PutUtf8(std::string& s, unsigned cp)
        {
            if (cp < 0x80)
                s.push_back(static_cast<char>(cp));
            else if (cp < 0x800)
            {
                s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        // Reads the string at p_ ('"' first). On success *sp/*sn view the decoded bytes, valid
        // until the next call (they point into the input or into `scratch_`).
        bool ReadString(const char*& sp, size_t& sn)
        {
            p_++; // opening quote
            const char* s = p_;
            bool plain = true;
            while (p_ < end_)
            {
                const unsigned char c = static_cast<unsigned char>(*p_);
                if (c == '"') break;
                if (c < 0x20) return Fail("raw control character in string");
                if (c == '\\')
                {
                    plain = false;
                    break;
                }
                p_++;
            }
            if (p_ >= end_) return Fail("unterminated string");
            if (plain)
            {
                if (!fsutil::Utf8Valid(s, static_cast<size_t>(p_ - s))) return Fail("invalid UTF-8 in string");
                sp = s;
                sn = static_cast<size_t>(p_ - s);
                p_++; // closing quote
                return true;
            }
            scratch_.assign(s, static_cast<size_t>(p_ - s));
            while (p_ < end_)
            {
                const unsigned char c = static_cast<unsigned char>(*p_);
                if (c == '"')
                {
                    p_++;
                    if (!fsutil::Utf8Valid(scratch_.data(), scratch_.size())) return Fail("invalid UTF-8 in string");
                    sp = scratch_.data();
                    sn = scratch_.size();
                    return true;
                }
                if (c < 0x20) return Fail("raw control character in string");
                if (c != '\\')
                {
                    scratch_.push_back(static_cast<char>(c));
                    p_++;
                    continue;
                }
                p_++;
                if (p_ >= end_) return Fail("unterminated string");
                const char e = *p_++;
                switch (e)
                {
                case '"': scratch_.push_back('"'); break;
                case '\\': scratch_.push_back('\\'); break;
                case '/': scratch_.push_back('/'); break;
                case 'b': scratch_.push_back('\b'); break;
                case 'f': scratch_.push_back('\f'); break;
                case 'n': scratch_.push_back('\n'); break;
                case 'r': scratch_.push_back('\r'); break;
                case 't': scratch_.push_back('\t'); break;
                case 'u':
                {
                    unsigned cp;
                    if (!Hex4(cp)) return Fail("invalid \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        unsigned lo;
                        if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return Fail("lone surrogate in \\u escape");
                        p_ += 2;
                        if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return Fail("lone surrogate in \\u escape");
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        return Fail("lone surrogate in \\u escape");
                    PutUtf8(scratch_, cp);
                    break;
                }
                default:
                    return Fail("invalid escape");
                }
            }
            return Fail("unterminated string");
        }

        bool ParseStringValue()
        {
            const char* sp;
            size_t sn;
            if (!ReadString(sp, sn)) return false;
            return Call(h_.Str(sp, sn));
        }

        bool ParseArray()
        {
            if (depth_ >= lim_.maxDepth) return Fail("nested too deeply");
            depth_++;
            p_++; // '['
            if (!Call(h_.BeginArr())) return false;
            SkipWs();
            if (p_ < end_ && *p_ == ']')
            {
                p_++;
                depth_--;
                return Call(h_.EndArr());
            }
            for (;;)
            {
                if (!ParseValue()) return false;
                SkipWs();
                if (p_ >= end_) return Fail("unterminated array");
                if (*p_ == ',')
                {
                    p_++;
                    continue;
                }
                if (*p_ == ']')
                {
                    p_++;
                    depth_--;
                    return Call(h_.EndArr());
                }
                return Fail("expected ',' or ']' in array");
            }
        }

        bool ParseObject()
        {
            if (depth_ >= lim_.maxDepth) return Fail("nested too deeply");
            depth_++;
            p_++; // '{'
            if (!Call(h_.BeginObj())) return false;
            SkipWs();
            if (p_ < end_ && *p_ == '}')
            {
                p_++;
                depth_--;
                return Call(h_.EndObj());
            }
            for (;;)
            {
                SkipWs();
                if (p_ >= end_ || *p_ != '"') return Fail("expected a string key in object");
                const char* kp;
                size_t kn;
                if (!ReadString(kp, kn)) return false;
                if (!Call(h_.Key(kp, kn))) return false;
                SkipWs();
                if (p_ >= end_ || *p_ != ':') return Fail("expected ':' after object key");
                p_++;
                if (!ParseValue()) return false;
                SkipWs();
                if (p_ >= end_) return Fail("unterminated object");
                if (*p_ == ',')
                {
                    p_++;
                    continue;
                }
                if (*p_ == '}')
                {
                    p_++;
                    depth_--;
                    return Call(h_.EndObj());
                }
                return Fail("expected ',' or '}' in object");
            }
        }
    };

    // ---- DOM -----------------------------------------------------------------------------

    // A parsed JSON value. Objects keep their members in document order; duplicate keys are
    // rejected by ParseDom.
    struct JsonValue
    {
        enum class T { Null, Bool, Int, Num, Str, Arr, Obj };
        T t = T::Null;
        bool b = false;
        long long i = 0;
        double d = 0;
        std::string s;
        std::vector<JsonValue> arr;
        std::vector<std::pair<std::string, JsonValue>> obj;

        // The member named `key` of an object, or nullptr (also for a non-object).
        const JsonValue* Find(const std::string& key) const
        {
            if (t != T::Obj) return nullptr;
            for (const auto& kv : obj)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }
    };

    namespace detail {

        class DomBuilder
        {
        public:
            explicit DomBuilder(JsonValue& root) : root_(root) {}
            bool Null() { return Put(JsonValue::T::Null); }
            bool Bool(bool v)
            {
                JsonValue* n = Slot(JsonValue::T::Bool);
                if (n) n->b = v;
                return n != nullptr;
            }
            bool Int(long long v)
            {
                JsonValue* n = Slot(JsonValue::T::Int);
                if (n) n->i = v;
                return n != nullptr;
            }
            bool Num(double v)
            {
                JsonValue* n = Slot(JsonValue::T::Num);
                if (n) n->d = v;
                return n != nullptr;
            }
            bool Str(const char* p, size_t n)
            {
                JsonValue* v = Slot(JsonValue::T::Str);
                if (v) v->s.assign(p, n);
                return v != nullptr;
            }
            bool BeginArr() { return Open(JsonValue::T::Arr); }
            bool EndArr() { return Close(); }
            bool BeginObj() { return Open(JsonValue::T::Obj); }
            bool Key(const char* p, size_t n)
            {
                pendingKey_.assign(p, n);
                if (!keys_.back().insert(pendingKey_).second)
                {
                    duplicate_ = true;
                    return false;
                }
                return true;
            }
            bool EndObj()
            {
                keys_.pop_back();
                return Close();
            }
            bool duplicate() const { return duplicate_; }

        private:
            JsonValue& root_;
            std::vector<JsonValue*> stack_;
            std::vector<std::set<std::string>> keys_;
            std::string pendingKey_;
            bool haveRoot_ = false;
            bool duplicate_ = false;

            // Where the next value goes: the root, or appended to the open container.
            JsonValue* Slot(JsonValue::T t)
            {
                JsonValue* v;
                if (stack_.empty())
                {
                    if (haveRoot_) return nullptr;
                    haveRoot_ = true;
                    v = &root_;
                }
                else
                {
                    JsonValue* parent = stack_.back();
                    if (parent->t == JsonValue::T::Arr)
                    {
                        parent->arr.emplace_back();
                        v = &parent->arr.back();
                    }
                    else
                    {
                        parent->obj.emplace_back(pendingKey_, JsonValue());
                        v = &parent->obj.back().second;
                    }
                }
                v->t = t;
                return v;
            }
            bool Put(JsonValue::T t) { return Slot(t) != nullptr; }
            bool Open(JsonValue::T t)
            {
                JsonValue* v = Slot(t);
                if (!v) return false;
                stack_.push_back(v);
                if (t == JsonValue::T::Obj) keys_.emplace_back();
                return true;
            }
            bool Close()
            {
                stack_.pop_back();
                return true;
            }
        };

    }

    // Parses `text` as one JSON value into `out`. Returns false with a message like
    // "expected ',' or '}' in object (at byte 17)" or "duplicate object key" in *err.
    inline bool ParseDom(const std::string& text, JsonValue& out, std::string* err, const Limits& limits = Limits())
    {
        out = JsonValue();
        detail::DomBuilder b(out);
        std::string scratch;
        Parser<detail::DomBuilder> parser(b, scratch, limits);
        ParseError e;
        if (parser.Parse(text.data(), text.size(), e)) return true;
        if (err)
        {
            if (b.duplicate())
                *err = "duplicate object key (at byte " + std::to_string(e.offset) + ")";
            else
                *err = std::string(e.message) + " (at byte " + std::to_string(e.offset) + ")";
        }
        return false;
    }

    // `s` as a JSON string literal including the quotes. `s` must be valid UTF-8 (bytes >= 0x80
    // pass through unescaped).
    inline std::string Quote(const std::string& s)
    {
        std::string o;
        o.reserve(s.size() + 2);
        o.push_back('"');
        for (char ch : s)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (c)
            {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                }
                else
                    o.push_back(ch);
            }
        }
        o.push_back('"');
        return o;
    }

    // `v` as compact JSON text.
    inline std::string Serialize(const JsonValue& v)
    {
        switch (v.t)
        {
        case JsonValue::T::Null: return "null";
        case JsonValue::T::Bool: return v.b ? "true" : "false";
        case JsonValue::T::Int: return std::to_string(v.i);
        case JsonValue::T::Num:
        {
            char buf[40];
            FormatDouble(v.d, buf);
            return buf;
        }
        case JsonValue::T::Str: return Quote(v.s);
        case JsonValue::T::Arr:
        {
            std::string o = "[";
            for (size_t i = 0; i < v.arr.size(); i++)
            {
                if (i) o.push_back(',');
                o += Serialize(v.arr[i]);
            }
            o.push_back(']');
            return o;
        }
        case JsonValue::T::Obj:
        {
            std::string o = "{";
            for (size_t i = 0; i < v.obj.size(); i++)
            {
                if (i) o.push_back(',');
                o += Quote(v.obj[i].first);
                o.push_back(':');
                o += Serialize(v.obj[i].second);
            }
            o.push_back('}');
            return o;
        }
        }
        return "null";
    }

}
