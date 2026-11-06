// GAGE protocol wire format: little-endian scalars and length-prefixed UTF-8
// strings. Shared by the client engine, the server and the tools; portable
// C++17 (no Windows headers) because the server also builds on Linux.
//
// Reader never throws (-fno-exceptions): any read past the end, or a string
// longer than its cap, latches ok() to false and every later read returns a
// zero value. Callers check ok() / done() once at the end.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gage::wire {

    class Writer
    {
    public:
        void u8(uint8_t v) { buf_.push_back(v); }
        void u16(uint16_t v)
        {
            buf_.push_back((uint8_t)(v & 0xFF));
            buf_.push_back((uint8_t)(v >> 8));
        }
        void u32(uint32_t v)
        {
            for (int i = 0; i < 4; i++)
                buf_.push_back((uint8_t)(v >> (8 * i)));
        }
        void f32(float v)
        {
            uint32_t bits;
            static_assert(sizeof(bits) == sizeof(v), "IEEE-754 binary32 expected");
            std::memcpy(&bits, &v, sizeof(bits));
            u32(bits);
        }
        void i16(int16_t v) { u16((uint16_t)v); }
        void u64(uint64_t v)
        {
            for (int i = 0; i < 8; i++)
                buf_.push_back((uint8_t)(v >> (8 * i)));
        }
        // LEB128: 7 bits per byte, low group first, the high bit says "more follows"
        // (1..5 bytes for a u32).
        void varu32(uint32_t v)
        {
            while (v >= 0x80)
            {
                buf_.push_back((uint8_t)((v & 0x7F) | 0x80));
                v >>= 7;
            }
            buf_.push_back((uint8_t)v);
        }
        // Zigzag (0, -1, 1, -2, 2 ... -> 0, 1, 2, 3, 4 ...) then LEB128: small magnitudes
        // of either sign stay short.
        void vari32(int32_t v) { varu32(((uint32_t)v << 1) ^ (uint32_t)(v >> 31)); }
        void bytes(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
        // u16 byte length + bytes. The caller validates the length against the
        // protocol cap; this only refuses what cannot be framed at all.
        bool str(const std::string& s)
        {
            if (s.size() > 0xFFFF)
                return false;
            u16((uint16_t)s.size());
            buf_.insert(buf_.end(), s.begin(), s.end());
            return true;
        }

        const std::vector<uint8_t>& data() const { return buf_; }
        std::vector<uint8_t> take() { return std::move(buf_); }
        size_t size() const { return buf_.size(); }
        void clear() { buf_.clear(); }
        void reserve(size_t n) { buf_.reserve(n); }   // the hot per-receiver encoders size the buffer once

    private:
        std::vector<uint8_t> buf_;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

        uint8_t u8()
        {
            if (!need(1)) return 0;
            return p_[off_++];
        }
        uint16_t u16()
        {
            if (!need(2)) return 0;
            const uint16_t v = (uint16_t)(p_[off_] | (p_[off_ + 1] << 8));
            off_ += 2;
            return v;
        }
        uint32_t u32()
        {
            if (!need(4)) return 0;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++)
                v |= (uint32_t)p_[off_ + i] << (8 * i);
            off_ += 4;
            return v;
        }
        float f32()
        {
            const uint32_t bits = u32();
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        int16_t i16() { return (int16_t)u16(); }
        uint64_t u64()
        {
            if (!need(8)) return 0;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++)
                v |= (uint64_t)p_[off_ + i] << (8 * i);
            off_ += 8;
            return v;
        }
        // The inverse of Writer::varu32, strictly: at most 5 bytes, no bits above 2^32, and
        // only the shortest form (a trailing 0x00 group after a continuation byte is
        // refused), so every value has exactly one encoding.
        uint32_t varu32()
        {
            uint32_t v = 0;
            for (int i = 0; i < 5; i++)
            {
                if (!need(1)) return 0;
                const uint8_t b = p_[off_++];
                if (i == 4 && (b & 0xF0) != 0) { ok_ = false; return 0; } // would overflow 32 bits
                v |= (uint32_t)(b & 0x7F) << (7 * i);
                if (!(b & 0x80))
                {
                    if (i > 0 && b == 0) { ok_ = false; return 0; } // non-canonical (overlong)
                    return v;
                }
            }
            ok_ = false; // five continuation bytes
            return 0;
        }
        int32_t vari32()
        {
            const uint32_t u = varu32();
            return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
        }
        // Reads `n` raw bytes into `out` (fails the reader when fewer remain).
        bool bytes(uint8_t* out, size_t n)
        {
            if (!need(n)) return false;
            std::memcpy(out, p_ + off_, n);
            off_ += n;
            return true;
        }
        // Reads a u16-prefixed string; longer than `maxBytes` fails the reader.
        std::string str(size_t maxBytes)
        {
            const uint16_t len = u16();
            if (!ok_ || len > maxBytes || !need(len))
            {
                ok_ = false;
                return std::string();
            }
            std::string s((const char*)p_ + off_, len);
            off_ += len;
            return s;
        }

        bool ok() const { return ok_; }
        // Every byte consumed and no read failed: a well-formed message.
        bool done() const { return ok_ && off_ == n_; }
        size_t remaining() const { return ok_ ? n_ - off_ : 0; }

    private:
        bool need(size_t k)
        {
            if (!ok_ || n_ - off_ < k)
            {
                ok_ = false;
                return false;
            }
            return true;
        }

        const uint8_t* p_;
        size_t n_;
        size_t off_ = 0;
        bool ok_ = true;
    };

}
