#pragma once

// MD5 (RFC 1321), self-contained, for the map identity: a map is known by the
// md5 of its file (entity/map.hpp; the retail mod hashes the loaded level
// file with OpenSSL, utils::md5_from_file).  The server and the headless
// tools hash a custom map file with this (step CMAP: the arena loaded as a
// BML+ custom map), so neither needs a crypto library.  Not for security.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace bmmo::md5 {
    class hasher {
    public:
        void update(const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            size_t index = static_cast<size_t>(count_ & 63);
            count_ += size;
            size_t i = 0;
            if (index) {
                const size_t fill = 64 - index;
                if (size < fill) {
                    std::memcpy(buffer_ + index, bytes, size);
                    return;
                }
                std::memcpy(buffer_ + index, bytes, fill);
                transform(buffer_);
                i = fill;
            }
            for (; i + 64 <= size; i += 64) transform(bytes + i);
            if (i < size) std::memcpy(buffer_, bytes + i, size - i);
        }

        void finish(uint8_t out[16]) {
            const uint64_t bits = count_ * 8;
            static const uint8_t pad[64] = {0x80};
            const size_t index = static_cast<size_t>(count_ & 63);
            update(pad, index < 56 ? 56 - index : 120 - index);
            uint8_t length[8];
            for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (8 * i));
            update(length, 8);
            for (int i = 0; i < 4; ++i)
                for (int b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<uint8_t>(state_[i] >> (8 * b));
        }

    private:
        static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

        void transform(const uint8_t block[64]) {
            static constexpr uint32_t k[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
            static constexpr int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                          5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                          4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                          6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
            uint32_t m[16];
            for (int i = 0; i < 16; ++i)
                m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8)
                     | (static_cast<uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
            uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
            for (int i = 0; i < 64; ++i) {
                uint32_t f;
                int g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
                else { f = c ^ (b | ~d); g = (7 * i) & 15; }
                const uint32_t next = d;
                d = c;
                c = b;
                b = b + rotl(a + f + k[i] + m[g], r[i]);
                a = next;
            }
            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
        }

        uint32_t state_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
        uint64_t count_ = 0;
        uint8_t buffer_[64] = {};
    };

    inline void of_bytes(const void* data, size_t size, uint8_t out[16]) {
        hasher h;
        h.update(data, size);
        h.finish(out);
    }

    // False when the file cannot be read (out is then left as it was).
    inline bool of_file(const std::filesystem::path& path, uint8_t out[16]) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        hasher h;
        char buffer[16 * 1024];
        while (file) {
            file.read(buffer, sizeof(buffer));
            const auto got = file.gcount();
            if (got > 0) h.update(buffer, static_cast<size_t>(got));
        }
        if (file.bad()) return false;
        h.finish(out);
        return true;
    }

    inline std::string to_hex(const uint8_t digest[16]) {
        static const char digits[] = "0123456789abcdef";
        std::string text(32, '0');
        for (int i = 0; i < 16; ++i) {
            text[i * 2] = digits[digest[i] >> 4];
            text[i * 2 + 1] = digits[digest[i] & 15];
        }
        return text;
    }
}
