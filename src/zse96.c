/* zse96.c —— 见 zse96.h 说明。
 *
 * 流水线：
 *   signSource = "101_3_3.0" + "+" + pathname(url) + "+" + d_c0 [+ "+" + body]
 *   hash       = md5_hex(signSource)
 *   sig        = customBase64( CBC(rBlock, 0xD2 0x00 || hash, PKCS#7) )
 *   header     = "2.0_" || sig
 */
#include "zse96.h"
#include "md5.h"
#include "zse_tables.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ALPHABET[] =
    "6fpLRqJO8M/c3jnYxFkUVC4ZIG12SiH=5v0mXDazWBTsuw7QetbKdoPyAl+hN9rgE";

static const char KEY16[16] = {
    '0', '5', '9', '0', '5', '3', 'f', '7', 'd', '1', '5', 'e', '0', '1', 'd', '7'
};

static uint32_t rol32(uint32_t v, int n) {
    n &= 31;
    if (n == 0) return v;
    return (v << n) | (v >> (32 - n));
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 一个字节查表 + 五次循环异或扩散 */
static uint32_t g_transform(uint32_t t) {
    uint8_t b0 = (uint8_t)(t >> 24), b1 = (uint8_t)(t >> 16);
    uint8_t b2 = (uint8_t)(t >> 8),  b3 = (uint8_t)t;
    uint32_t ti = ((uint32_t)ZB_TABLE[b0] << 24) | ((uint32_t)ZB_TABLE[b1] << 16) |
                  ((uint32_t)ZB_TABLE[b2] << 8)  | (uint32_t)ZB_TABLE[b3];
    return ti ^ rol32(ti, 2) ^ rol32(ti, 10) ^ rol32(ti, 18) ^ rol32(ti, 24);
}

/* 16 字节 -> 16 字节，36 个寄存器迭代 32 轮 */
static void r_block(const uint8_t *in16, uint8_t *out16) {
    uint32_t tr[36];
    tr[0] = rd32be(in16);
    tr[1] = rd32be(in16 + 4);
    tr[2] = rd32be(in16 + 8);
    tr[3] = rd32be(in16 + 12);
    for (int i = 0; i < 32; i++) {
        uint32_t ta = g_transform(tr[i + 1] ^ tr[i + 2] ^ tr[i + 3] ^ ZK_TABLE[i]);
        tr[i + 4] = tr[i] ^ ta;
    }
    wr32be(out16,      tr[35]);
    wr32be(out16 + 4,  tr[34]);
    wr32be(out16 + 8,  tr[33]);
    wr32be(out16 + 12, tr[32]);
}

/* 知乎自定义 base64：从尾往前每 3 字节出 4 字符，首字节异或 0x3A，字母表自定 */
static void custom_encode(const uint8_t *bytes, size_t n, char *out, size_t outsz) {
    size_t padded = ((n + 2) / 3) * 3;
    uint8_t local[64];
    uint8_t *buf = local;
    if (padded > sizeof(local)) buf = (uint8_t *)calloc(padded, 1);
    else memset(buf, 0, sizeof(local));
    memcpy(buf, bytes, n);

    size_t cnt = 0, o = 0;
    for (size_t p = padded; p >= 3; p -= 3) {
        uint32_t v = 0;
        for (int k = 0; k < 3; k++) {
            uint8_t b = buf[p - 1 - (size_t)k];
            uint8_t m = (uint8_t)(58 >> (8 * (cnt % 4)));
            cnt++;
            v |= (uint32_t)(b ^ m) << (8 * k);
        }
        for (int s = 0; s < 4; s++) {
            uint8_t idx = (uint8_t)((v >> (6 * s)) & 63);
            if (o + 1 < outsz) out[o++] = ALPHABET[idx];
        }
    }
    out[o < outsz ? o : outsz - 1] = 0;
    if (buf != local) free(buf);
}

void zse96_encrypt_v4(const char *in, size_t inlen, char *out, size_t outsz) {
    /* 0xD2 0x00 + 输入，PKCS#7 补齐 */
    size_t plain_len = 2 + inlen;
    size_t pad = 16 - (plain_len % 16);
    size_t total = plain_len + pad;

    uint8_t *plain = (uint8_t *)malloc(total);
    if (!plain) { if (outsz) out[0] = 0; return; }
    plain[0] = 0xD2;
    plain[1] = 0x00;
    memcpy(plain + 2, in, inlen);
    memset(plain + plain_len, (int)pad, pad);

    uint8_t first[16], c0[16], prev[16];
    for (int i = 0; i < 16; i++)
        first[i] = (uint8_t)(plain[i] ^ (uint8_t)KEY16[i] ^ 0x2A);
    r_block(first, c0);
    memcpy(prev, c0, 16);

    uint8_t *cipher = (uint8_t *)malloc(total);
    if (!cipher) { free(plain); if (outsz) out[0] = 0; return; }
    memcpy(cipher, c0, 16);

    for (size_t off = 16; off < total; off += 16) {
        uint8_t mixed[16], blk[16];
        for (int j = 0; j < 16; j++) mixed[j] = (uint8_t)(plain[off + (size_t)j] ^ prev[j]);
        r_block(mixed, blk);
        memcpy(cipher + off, blk, 16);
        memcpy(prev, blk, 16);
    }

    custom_encode(cipher, total, out, outsz);
    free(plain);
    free(cipher);
}

void zse96_pathname(const char *url, char *out, size_t outsz) {
    /* 取 "//" 之后第一个 '/' 起的部分；没有则原样 */
    const char *p = url;
    if (!p) { snprintf(out, outsz, "/"); return; }
    const char *scheme = strstr(p, "//");
    if (scheme) {
        const char *slash = strchr(scheme + 2, '/');
        if (slash) { snprintf(out, outsz, "%s", slash); return; }
        snprintf(out, outsz, "/");
        return;
    }
    if (p[0] == '/') snprintf(out, outsz, "%s", p);
    else snprintf(out, outsz, "/%s", p);
}

void zse96_sign(const char *url, const char *dc0, const char *body, char *out, size_t outsz) {
    char path[512];
    zse96_pathname(url, path, sizeof(path));

    char source[1600];
    if (body) snprintf(source, sizeof(source), "%s+%s+%s+%s",
                       ZSE93_VERSION, path, dc0 ? dc0 : "", body);
    else snprintf(source, sizeof(source), "%s+%s+%s",
                  ZSE93_VERSION, path, dc0 ? dc0 : "");

    char hash[33];
    md5_hex(source, strlen(source), hash);

    char sig[128];
    zse96_encrypt_v4(hash, 32, sig, sizeof(sig));
    snprintf(out, outsz, "2.0_%s", sig);
}
