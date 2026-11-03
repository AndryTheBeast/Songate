/*
 * Songate · md5.hpp
 * MD5 (RFC 1321), for Last.fm's api_sig and nothing else: the only place its protocol asks for it, and never used to protect
 * anything here.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace md5 {

namespace detail {

inline uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

inline void block(uint32_t st[4], const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d); g = (7 * i) % 16; break;
        }
        const uint32_t t = d;
        d = c;
        c = b;
        b += rol(a + f + K[i] + m[g], R[i / 16 * 4 + i % 4]);
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

}  // namespace detail

/** The digest of s, in lower-case hex. */
inline std::string hex(const std::string& s) {
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const size_t n = s.size();
    size_t i = 0;
    for (; n - i >= 64; i += 64) detail::block(st, reinterpret_cast<const uint8_t*>(s.data()) + i);
    uint8_t last[128] = {};
    const size_t left = n - i;
    if (left) memcpy(last, s.data() + i, left);
    last[left] = 0x80;
    const size_t end = left < 56 ? 56 : 120;
    const uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; k++) last[end + size_t(k)] = uint8_t(bits >> (8 * k));
    detail::block(st, last);
    if (end == 120) detail::block(st, last + 64);
    static const char* const digits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int w = 0; w < 4; w++)
        for (int k = 0; k < 4; k++) {
            const uint8_t b = uint8_t(st[w] >> (8 * k));
            out += digits[b >> 4];
            out += digits[b & 15];
        }
    return out;
}

}  // namespace md5
