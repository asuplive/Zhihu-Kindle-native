#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- CRC32 / Adler32 ---------------- */

static uint32_t g_crc_tab[256];
static int g_crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[n] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32_upd(uint32_t crc, const void *buf, size_t n) {
    if (!g_crc_ready) crc_init();
    const uint8_t *p = (const uint8_t *)buf;
    crc ^= 0xFFFFFFFFu;
    while (n--) crc = g_crc_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t adler_upd(uint32_t a, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t s1 = a & 0xFFFFu, s2 = (a >> 16) & 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        s1 += p[i];
        if (s1 >= 65521u) s1 -= 65521u;
        s2 += s1;
        if (s2 >= 65521u) s2 -= 65521u;
    }
    return (s2 << 16) | s1;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---------------- 写出 ---------------- */

int png_write_gray8(const char *path, int w, int h, const uint8_t *data, int stride) {
    if (w <= 0 || h <= 0 || !data || stride < w) return -1;

    uint8_t *stage = NULL;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (fwrite(sig, 1, sizeof(sig), f) != sizeof(sig)) goto fail;

    /* IHDR：8bit、灰度（colortype 0）、无隔行 */
    {
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;   /* bit depth */
        ihdr[9] = 0;   /* color type: grayscale */
        ihdr[10] = 0;  /* compression */
        ihdr[11] = 0;  /* filter */
        ihdr[12] = 0;  /* interlace */
        uint8_t head[8];
        be32(head, 13);
        memcpy(head + 4, "IHDR", 4);
        uint32_t c = crc32_upd(0, "IHDR", 4);
        c = crc32_upd(c, ihdr, 13);
        uint8_t tail[4];
        be32(tail, c);
        if (fwrite(head, 1, 8, f) != 8) goto fail;
        if (fwrite(ihdr, 1, 13, f) != 13) goto fail;
        if (fwrite(tail, 1, 4, f) != 4) goto fail;
    }

    /* 原始数据长度：每行前面加一个 filter 字节（0 = None） */
    size_t row = (size_t)w + 1;
    size_t raws = row * (size_t)h;

    /* 先数一遍要切成几个 stored 块，IDAT 长度必须提前写死 */
    size_t nblk = 0, sf = 0;
    for (int y = 0; y < h; y++) {
        if (sf + row > 65535) { nblk++; sf = 0; }
        sf += row;
    }
    if (sf) nblk++;
    if (nblk == 0) nblk = 1;

    uint32_t idat_len = (uint32_t)(2 + raws + 5 * nblk + 4);
    {
        uint8_t head[8];
        be32(head, idat_len);
        memcpy(head + 4, "IDAT", 4);
        if (fwrite(head, 1, 8, f) != 8) goto fail;
    }

    uint32_t crc = crc32_upd(0, "IDAT", 4);
    uint32_t adler = 1;

#define EMIT_C(p_, n_)                                                    \
    do {                                                                  \
        size_t _n = (n_);                                                 \
        if (fwrite((p_), 1, _n, f) != _n) goto fail;                       \
        crc = crc32_upd(crc, (p_), _n);                                    \
    } while (0)
#define EMIT_RAW(p_, n_)                                                  \
    do {                                                                  \
        size_t _n = (n_);                                                 \
        if (fwrite((p_), 1, _n, f) != _n) goto fail;                       \
        crc = crc32_upd(crc, (p_), _n);                                    \
        adler = adler_upd(adler, (p_), _n);                                \
    } while (0)

    /* zlib 头：CMF=0x78（deflate, 32K window），FLG=0x01（最快、无字典、校验通过） */
    {
        uint8_t zh[2] = { 0x78, 0x01 };
        EMIT_C(zh, 2);
    }

    stage = (uint8_t *)malloc(65535);
    if (!stage) goto fail;
    size_t fill = 0;
    for (int y = 0; y < h; y++) {
        if (fill + row > 65535) {
            uint8_t bh[5];
            bh[0] = 0;               /* BFINAL=0, BTYPE=00 (stored) */
            bh[1] = (uint8_t)(fill & 0xFF);
            bh[2] = (uint8_t)((fill >> 8) & 0xFF);
            bh[3] = (uint8_t)(~fill & 0xFF);
            bh[4] = (uint8_t)((~fill >> 8) & 0xFF);
            EMIT_C(bh, 5);
            EMIT_RAW(stage, fill);
            fill = 0;
        }
        stage[fill++] = 0;   /* filter: None */
        memcpy(stage + fill, data + (size_t)y * (size_t)stride, (size_t)w);
        fill += (size_t)w;
    }
    if (fill) {
        uint8_t bh[5];
        bh[0] = 1;                   /* BFINAL=1 */
        bh[1] = (uint8_t)(fill & 0xFF);
        bh[2] = (uint8_t)((fill >> 8) & 0xFF);
        bh[3] = (uint8_t)(~fill & 0xFF);
        bh[4] = (uint8_t)((~fill >> 8) & 0xFF);
        EMIT_C(bh, 5);
        EMIT_RAW(stage, fill);
    }
    free(stage);
    stage = NULL;

    /* zlib 尾：Adler-32（大端） */
    {
        uint8_t ad[4];
        be32(ad, adler);
        EMIT_C(ad, 4);
    }

#undef EMIT_C
#undef EMIT_RAW
    {
        uint8_t tail[4];
        be32(tail, crc);
        if (fwrite(tail, 1, 4, f) != 4) goto fail;
    }

    /* IEND */
    {
        uint8_t head[8];
        be32(head, 0);
        memcpy(head + 4, "IEND", 4);
        uint32_t c = crc32_upd(0, "IEND", 4);
        uint8_t tail[4];
        be32(tail, c);
        if (fwrite(head, 1, 8, f) != 8) goto fail;
        if (fwrite(tail, 1, 4, f) != 4) goto fail;
    }

    if (fclose(f) != 0) return -1;
    return 0;

fail:
    if (stage) free(stage);
    fclose(f);
    remove(path);
    return -1;
}

/* ---------------- 自检 ---------------- */

int png_verify(const char *path, int *w, int *h, char *err, int errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, (size_t)errsz, "打不开文件");
        return 0;
    }
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        snprintf(err, (size_t)errsz, "签名不对");
        fclose(f);
        return 0;
    }
    int got_iend = 0, got_ihdr = 0, got_idat = 0;
    long idat_bytes = 0;
    uint8_t *data = NULL;
    for (;;) {
        uint8_t head[8];
        if (fread(head, 1, 8, f) != 8) break;
        uint32_t len = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) |
                       ((uint32_t)head[2] << 8) | (uint32_t)head[3];
        char type[5];
        memcpy(type, head + 4, 4);
        type[4] = 0;
        if (len > 32u * 1024u * 1024u) {
            snprintf(err, (size_t)errsz, "chunk %s 长度异常 %u", type, len);
            fclose(f);
            return 0;
        }
        data = (uint8_t *)realloc(data, len ? len : 1);
        if (!data) { fclose(f); return 0; }
        if (len && fread(data, 1, len, f) != len) {
            snprintf(err, (size_t)errsz, "chunk %s 数据不全", type);
            free(data);
            fclose(f);
            return 0;
        }
        uint8_t tail[4];
        if (fread(tail, 1, 4, f) != 4) {
            snprintf(err, (size_t)errsz, "chunk %s 缺 CRC", type);
            free(data);
            fclose(f);
            return 0;
        }
        uint32_t want = ((uint32_t)tail[0] << 24) | ((uint32_t)tail[1] << 16) |
                        ((uint32_t)tail[2] << 8) | (uint32_t)tail[3];
        uint32_t c = crc32_upd(0, type, 4);
        if (len) c = crc32_upd(c, data, len);
        if (c != want) {
            snprintf(err, (size_t)errsz, "chunk %s CRC 错", type);
            free(data);
            fclose(f);
            return 0;
        }
        if (strcmp(type, "IHDR") == 0) {
            if (len != 13 || data[8] != 8 || data[9] != 0) {
                snprintf(err, (size_t)errsz, "IHDR 不是 8bit 灰度");
                free(data);
                fclose(f);
                return 0;
            }
            if (w) *w = (int)(((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                              ((uint32_t)data[2] << 8) | data[3]);
            if (h) *h = (int)(((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                              ((uint32_t)data[6] << 8) | data[7]);
            got_ihdr = 1;
        } else if (strcmp(type, "IDAT") == 0) {
            got_idat = 1;
            idat_bytes += (long)len;
        } else if (strcmp(type, "IEND") == 0) {
            got_iend = 1;
            break;
        }
    }
    free(data);
    fclose(f);
    if (!got_ihdr || !got_idat || !got_iend) {
        snprintf(err, (size_t)errsz, "缺 chunk（ihdr=%d idat=%d iend=%d）",
                 got_ihdr, got_idat, got_iend);
        return 0;
    }
    if (idat_bytes < 8) {
        snprintf(err, (size_t)errsz, "IDAT 太短 %ld", idat_bytes);
        return 0;
    }
    return 1;
}
