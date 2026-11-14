/* md5.c —— 标准 RFC 1321 实现（小端 x86/ARM 通用，边长为 64 字节分块） */
#include "md5.h"

#include <string.h>

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define STEP(f, a, b, c, d, x, t, s) \
    do { (a) += f((b), (c), (d)) + (x) + (t); (a) = ROTL((a), (s)); (a) += (b); } while (0)

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t st[4], const uint8_t blk[64]) {
    uint32_t X[16];
    for (int i = 0; i < 16; i++) {
        X[i] = (uint32_t)blk[i * 4] | ((uint32_t)blk[i * 4 + 1] << 8) |
               ((uint32_t)blk[i * 4 + 2] << 16) | ((uint32_t)blk[i * 4 + 3] << 24);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = F(b, c, d); g = i; }
        else if (i < 32) { f = G(b, c, d); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = H(b, c, d); g = (3 * i + 5) % 16; }
        else             { f = I(b, c, d); g = (7 * i) % 16; }
        uint32_t tmp = d;
        d = c;
        c = b;
        uint32_t x = a + f + K[i] + X[g];
        b = b + ROTL(x, S[i]);
        a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void md5_raw(const void *data, size_t len, uint8_t out[16]) {
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;

    for (; i + 64 <= len; i += 64) md5_block(st, p + i);

    uint8_t tail[128];
    size_t rem = len - i;
    memset(tail, 0, sizeof(tail));
    if (rem) memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem + 8 < 64) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tail_len - 8 + k] = (uint8_t)(bits >> (8 * k));
    for (size_t off = 0; off < tail_len; off += 64) md5_block(st, tail + off);

    for (int k = 0; k < 4; k++) {
        out[k * 4 + 0] = (uint8_t)(st[k]);
        out[k * 4 + 1] = (uint8_t)(st[k] >> 8);
        out[k * 4 + 2] = (uint8_t)(st[k] >> 16);
        out[k * 4 + 3] = (uint8_t)(st[k] >> 24);
    }
}

void md5_hex(const void *data, size_t len, char out[33]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t d[16];
    md5_raw(data, len, d);
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[32] = 0;
}
