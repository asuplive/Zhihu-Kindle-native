/* img.c —— JPEG（基线）解码成灰度 + 图片缓存
 *
 * 详见 img.h 的说明。这里刻意不用浮点：armv5te 是软浮点，整页解码里
 * 几千万次浮点乘法会慢到不可接受；所有变换都用 Q12 定点整数做。
 */
#include "img.h"
#include "common.h"
#include "md5.h"
#include "png.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

int img_decode_jpeg(const uint8_t *data, size_t len, int target_w, int max_h,
                    ImgGray *out, char *err, int errsz);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- 位读取（含 0xFF00 填充） ---------------- */

enum { BR_OK = 0, BR_EOF = 1, BR_MARKER = 2, BR_BAD = 3 };

typedef struct {
    const uint8_t *d;
    size_t n, pos;
    uint32_t buf;
    int cnt;
    int err;
} BR;

static int br_bit(BR *b) {
    if (b->cnt == 0) {
        if (b->pos >= b->n) { b->err = BR_EOF; return 0; }
        unsigned c = b->d[b->pos++];
        if (c == 0xFF) {
            if (b->pos >= b->n) { b->err = BR_EOF; return 0; }
            unsigned c2 = b->d[b->pos];
            if (c2 == 0x00) {
                b->pos++;
            } else {
                b->pos--;              /* 退回到 marker 起点，留着重同步用 */
                b->err = BR_MARKER;
                return 0;
            }
        }
        b->buf = c;
        b->cnt = 8;
    }
    b->cnt--;
    return (int)((b->buf >> b->cnt) & 1u);
}

static int br_bits(BR *b, int n) {
    int v = 0;
    while (n-- > 0) {
        int bit = br_bit(b);
        if (b->err) return v;
        v = (v << 1) | bit;
    }
    return v;
}

/* ---------------- 霍夫曼表 ---------------- */

typedef struct {
    uint8_t  vals[256];
    int32_t  mincode[17];
    int32_t  maxcode[17];
    int32_t  valptr[17];
    int      ok;
} Huff;

static void huff_build(Huff *h, const uint8_t *bits, const uint8_t *vals) {
    int code = 0, k = 0;
    memset(h, 0, sizeof(*h));
    for (int l = 1; l <= 16; l++) {
        int n = bits[l];
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += n;
        k += n;
        h->maxcode[l] = n ? code - 1 : -1;
        code <<= 1;
    }
    if (k > 256) k = 256;
    memcpy(h->vals, vals, (size_t)k);
    h->ok = 1;
}

static int huff_dec(BR *b, const Huff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        int bit = br_bit(b);
        if (b->err) return -1;
        code = (code << 1) | bit;
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l]) {
            int idx = h->valptr[l] + code - h->mincode[l];
            if (idx < 0 || idx >= 256) { b->err = BR_BAD; return -1; }
            return h->vals[idx];
        }
    }
    b->err = BR_BAD;
    return -1;
}

static int extend_val(int v, int t) {
    if (t == 0) return 0;
    return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
}

/* ---------------- 定点 IDCT ---------------- */

/* tab[log2(8/N)][u][x] = 4096 * 0.5 * c(u) * cos((2*xs+1)*u*pi/16)
 * 其中 xs = (x+0.5)*(8/N) - 0.5：把 N 点输出采样到原 8 点栅格上，
 * 这样 DCT 缩放后亮度级别与原图一致（DC 仍还原成平均值）。 */
static int16_t g_tab[4][8][8];
static int     g_tab_ready;

static void idct_tables(void) {
    if (g_tab_ready) return;
    for (int s = 0; s < 4; s++) {
        int N = 8 >> s;
        for (int u = 0; u < N; u++) {
            double cu = (u == 0) ? 0.7071067811865476 : 1.0;
            for (int x = 0; x < N; x++) {
                double xs = (x + 0.5) * (8.0 / N) - 0.5;
                double v = 0.5 * cu * cos((2.0 * xs + 1.0) * u * M_PI / 16.0);
                g_tab[s][u][x] = (int16_t)lround(v * 4096.0);
            }
        }
    }
    g_tab_ready = 1;
}

/* coef 为自然顺序（已反量化、已 clamp 到 ±16383），输出 N×N 个样本 */
static void idct_block(const int32_t *coef, int s, uint8_t *dst, int stride) {
    int N = 8 >> s;
    int32_t t[64];
    for (int r = 0; r < N; r++) {
        const int32_t *row = coef + r * 8;
        for (int x = 0; x < N; x++) {
            int32_t sum = 0;
            for (int u = 0; u < N; u++)
                sum += row[u] * (int32_t)g_tab[s][u][x];
            t[r * N + x] = (sum + 2048) >> 12;
        }
    }
    for (int c = 0; c < N; c++) {
        for (int y = 0; y < N; y++) {
            int32_t sum = 0;
            for (int v = 0; v < N; v++)
                sum += t[v * N + c] * (int32_t)g_tab[s][v][y];
            int val = (int)((sum + 2048) >> 12) + 128;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            dst[y * stride + c] = (uint8_t)val;
        }
    }
}

/* ---------------- 组件 ---------------- */

typedef struct {
    int id, h, v, tq, td, ta;
} Comp;

typedef struct {
    int  w, h;            /* 图像尺寸 */
    int  ncomp;
    Comp c[4];
    int  hmax, vmax;
    int  mcus_x, mcus_y;
    int  dri;             /* 重启间隔（MCU） */
} Frame;

/* ---------------- 解码 ---------------- */

typedef struct {
    Frame fr;
    uint16_t quant[4][64];
    int      qok[4];
    Huff     dc[4], ac[4];
    uint8_t *plane;       /* 只存亮度 */
    int      pw, ph;      /* 亮度平面尺寸（含 MCU 补齐） */
    int      bs;          /* 每个块输出 bs×bs 个样本 */
    int      blocks_done;
    int      blocks_expect;
} Dec;

static uint8_t g_zigzag[64];

static void zigzag_init(void) {
    static const uint8_t z[64] = {
         0,  1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63,
    };
    if (!g_zigzag[1]) memcpy(g_zigzag, z, 64);
}

/* 每个分量的 DC 预测值。必须是文件级的：块解码和重启处理都要改它，
 * 放在函数里当 static 会在重启时忘记清零，画面会整片偏色。 */
static int32_t g_dc_pred[4];

/* 解码一个 8x8 块。want=1 时做 IDCT 写进平面，否则只走位流。 */
static int decode_block(BR *br, Dec *d, int ci, int32_t *coef, int want,
                        int px, int py) {
    const Comp *c = &d->fr.c[ci];

    memset(coef, 0, sizeof(int32_t) * 64);
    if (!d->dc[c->td].ok || !d->ac[c->ta].ok) return -1;

    int t = huff_dec(br, &d->dc[c->td]);
    if (t < 0) return -1;
    if (t > 16) return -1;
    int diff = t ? extend_val(br_bits(br, t), t) : 0;
    if (br->err) return -1;
    g_dc_pred[ci] += diff;
    coef[0] = g_dc_pred[ci] * (int32_t)d->quant[c->tq][0];

    int k = 1;
    while (k < 64) {
        int rs = huff_dec(br, &d->ac[c->ta]);
        if (rs < 0) return -1;
        int s = rs & 15, r = rs >> 4;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) break;         /* 越界说明这块的数据已经用完，按结束处理 */
        int v = extend_val(br_bits(br, s), s);
        if (br->err) return -1;
        /* 量化表在文件里就是 zigzag 顺序，所以第 k 个系数配第 k 个量化值 */
        coef[g_zigzag[k]] = v * (int32_t)d->quant[c->tq][k];
        k++;
    }

    if (want) {
        for (int i = 0; i < 64; i++) {
            int32_t v = coef[i];
            if (v > 16383) v = 16383;
            if (v < -16383) v = -16383;
            coef[i] = v;
        }
        idct_block(coef, d->bs == 8 ? 0 : (d->bs == 4 ? 1 : (d->bs == 2 ? 2 : 3)),
                   d->plane + (size_t)py * (size_t)d->pw + (size_t)px, d->pw);
    }
    d->blocks_done++;
    return 0;
}

/* 跳过到下一个 RSTn 标记之后 */
static void br_resync(BR *b) {
    b->cnt = 0;
    b->err = 0;
    while (b->pos + 1 < b->n) {
        if (b->d[b->pos] == 0xFF && (b->d[b->pos + 1] & 0xF8) == 0xD0) {
            b->pos += 2;
            return;
        }
        b->pos++;
    }
    b->err = BR_EOF;
}

static int decode_scan(BR *br, Dec *d) {
    static int32_t coef[64];
    /* 找亮度分量：采样率最大的那个（标准里是第一个） */
    int luma = 0;
    for (int i = 1; i < d->fr.ncomp; i++)
        if (d->fr.c[i].h * d->fr.c[i].v > d->fr.c[luma].h * d->fr.c[luma].v) luma = i;

    memset(g_dc_pred, 0, sizeof(g_dc_pred));
    int total = d->fr.mcus_x * d->fr.mcus_y;
    int m = 0;
    for (int my = 0; my < d->fr.mcus_y; my++) {
        for (int mx = 0; mx < d->fr.mcus_x; mx++, m++) {
            if (d->fr.dri > 0 && m > 0 && (m % d->fr.dri) == 0) {
                br_resync(br);
                memset(g_dc_pred, 0, sizeof(g_dc_pred));
                if (br->err) return -1;
            }
            for (int ci = 0; ci < d->fr.ncomp; ci++) {
                Comp *c = &d->fr.c[ci];
                for (int by = 0; by < c->v; by++) {
                    for (int bx = 0; bx < c->h; bx++) {
                        int want = (ci == luma);
                        int px = (mx * c->h + bx) * d->bs;
                        int py = (my * c->v + by) * d->bs;
                        if (decode_block(br, d, ci, coef, want, px, py) != 0) return -1;
                    }
                }
            }
            if (br->err) return -1;
        }
    }
    (void)total;
    return 0;
}

/* ---------------- 盒式缩小 ---------------- */

/* src 的实际行宽是 stride（= 平面宽，含 MCU 补齐），有效区域只有 sw × sh。
 * 早期版本把 sw 当 stride 用，宽度不是 16 的倍数时每一行都会整体横移，
 * 文字图片上表现为"字形错位、整行变白"——灰度图宽度恰好对齐，反而看不出来。 */
static uint8_t *box_down(const uint8_t *src, int stride, int sw, int sh, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || stride < sw) return NULL;
    if (dw == sw && dh == sh && stride == sw) {
        uint8_t *c = xmalloc((size_t)sw * (size_t)sh);
        memcpy(c, src, (size_t)sw * (size_t)sh);
        return c;
    }
    uint8_t *dst = xmalloc((size_t)dw * (size_t)dh);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh);
        int y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw);
            int x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t sum = 0;
            int cnt = 0;
            for (int j = y0; j < y1; j++) {
                const uint8_t *row = src + (size_t)j * (size_t)stride;
                for (int i = x0; i < x1; i++) { sum += row[i]; cnt++; }
            }
            dst[(size_t)y * (size_t)dw + x] = (uint8_t)(cnt ? sum / (uint32_t)cnt : 255);
        }
    }
    return dst;
}

/* ---------------- 主入口 ---------------- */

/* stb_image（PNG/GIF/BMP）与 libwebp 的实现，只在 img.c 编一份 */
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <webp/decode.h>

/* 超大图防线：超过这个像素数的图在真机（低内存 + 软浮点慢 CPU）上
 * 解码要几分钟、内存要几十 MB，表现就是"点了之后整个界面卡死"。
 * 明确拒绝并提示，比卡死强得多。e-ink 屏最多也就 ~200 万像素。 */
#define IMG_MAX_PIXELS 20000000LL

/* ---- stb / libwebp 路径：解到灰度后与 JPEG 共用 box_down 缩小 ---- */

static int decode_gray_generic(const uint8_t *data, size_t len, int is_webp,
                               int target_w, int max_h,
                               ImgGray *out, char *err, int errsz) {
    int w = 0, h = 0;
    uint8_t *px = NULL;

    if (is_webp) {
        /* WebP：libwebp。公开 API 没有"直接出灰度"，走 WebPDecodeYUV 拿 Y 平面
         *（Y/U/V 在同一块连续内存里，Y 在最前），U/V 直接丢掉。
         * 有损（VP8）和无损（VP8L）都走这一条。 */
        if (WebPGetInfo(data, len, &w, &h) == 0) {
            if (err) snprintf(err, errsz, "WebP 头解析失败");
            return -1;
        }
        if ((long long)w * h > IMG_MAX_PIXELS) {
            if (err) snprintf(err, errsz, "图片过大（%dx%d）", w, h);
            return -1;
        }
        uint8_t *u = NULL, *v = NULL;
        int stride = 0, uv_stride = 0;
        px = WebPDecodeYUV(data, len, &w, &h, &u, &v, &stride, &uv_stride);
        if (!px) {
            if (err) snprintf(err, errsz, "WebP 解码失败");
            return -1;
        }
        if (stride != w) {
            /* 内部分配的 Y stride 一定等于 w（带 alpha 或外部内存时才会不同），
             * 真出现这种就逐行拷贝太麻烦，直接按行抽取 */
            uint8_t *g2 = xmalloc((size_t)w * (size_t)h);
            for (int yy = 0; yy < h; yy++)
                memcpy(g2 + (size_t)yy * w, px + (size_t)yy * stride, (size_t)w);
            WebPFree(px);
            px = g2;
        }
    } else {
        /* PNG/GIF/BMP：stb_image，请求 1 通道直接得灰度（省 3/4 内存）。
         * 先手工摸一下 PNG 的 IHDR 拿尺寸做限制（stb 本身不限制尺寸） */
        if (len >= 24 && data[0] == 0x89 && data[1] == 'P') {
            long long pw = ((long long)data[16] << 24) | ((long long)data[17] << 16) |
                           ((long long)data[18] << 8) | data[19];
            long long ph = ((long long)data[20] << 24) | ((long long)data[21] << 16) |
                           ((long long)data[22] << 8) | data[23];
            if (pw > 0 && ph > 0 && pw * ph > IMG_MAX_PIXELS) {
                if (err) snprintf(err, errsz, "图片过大（%lldx%lld）", pw, ph);
                return -1;
            }
        }
        px = stbi_load_from_memory(data, (int)len, &w, &h, NULL, 1);
        if (!px) {
            if (err) snprintf(err, errsz, "解码失败：%s", stbi_failure_reason());
            return -1;
        }
    }

    if (w <= 0 || h <= 0) {
        free(px);
        if (err) snprintf(err, errsz, "解码结果为空");
        return -1;
    }

    /* 与 JPEG 路径完全一致的缩小逻辑 */
    int dw = target_w > 0 ? target_w : w;
    if (dw > w) dw = w;
    int dh = (int)((long long)h * dw / w);
    if (dh < 1) dh = 1;
    if (max_h > 0 && dh > max_h) {
        dh = max_h;
        dw = (int)((long long)w * dh / h);
        if (dw < 1) dw = 1;
    }
    uint8_t *small = box_down(px, w, w, h, dw, dh);
    free(px);
    if (!small) {
        if (err) snprintf(err, errsz, "内存不足");
        return -1;
    }
    out->px = small;
    out->w = dw;
    out->h = dh;
    return 0;
}

int img_decode_gray(const uint8_t *data, size_t len, int target_w, int max_h,
                    ImgGray *out, char *err, int errsz) {
    if (!out) return -1;
    out->w = out->h = 0;
    out->px = NULL;
    if (!data || len < 4) { if (err) snprintf(err, errsz, "空数据"); return -1; }

    /* 按文件头分流。知乎 CDN 常把图转成 WebP/PNG，1.2.8 只认 JPEG
     * 的结果就是"解码失败，不是 JPEG（可能是 PNG/WebP）"。 */
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return img_decode_jpeg(data, len, target_w, max_h, out, err, errsz);
    }
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return decode_gray_generic(data, len, 1, target_w, max_h, out, err, errsz);
    }
    if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return decode_gray_generic(data, len, 0, target_w, max_h, out, err, errsz);
    }
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) {
        return decode_gray_generic(data, len, 0, target_w, max_h, out, err, errsz);
    }
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        return decode_gray_generic(data, len, 0, target_w, max_h, out, err, errsz);
    }
    if (err) snprintf(err, errsz, "不认识的图片格式（%02x %02x %02x %02x）",
                      data[0], data[1], data[2], data[3]);
    return -1;
}

/* ---------------- JPEG 解码（基线） ---------------- */

int img_decode_jpeg(const uint8_t *data, size_t len, int target_w, int max_h,
                    ImgGray *out, char *err, int errsz) {
    if (!out) return -1;
    out->w = out->h = 0;
    out->px = NULL;
    if (!data || len < 4) { if (err) snprintf(err, errsz, "空数据"); return -1; }
    if (!(data[0] == 0xFF && data[1] == 0xD8)) {
        if (err) snprintf(err, errsz, "不是 JPEG");
        return -1;
    }
    zigzag_init();
    idct_tables();

    Dec *d = xcalloc(1, sizeof(Dec));
    BR br;
    memset(&br, 0, sizeof(br));
    br.d = data;
    br.n = len;
    br.pos = 2;

    int saw_sof = 0, saw_sos = 0;
    int rc = -1;

    while (br.pos + 1 < br.n) {
        if (data[br.pos] != 0xFF) { br.pos++; continue; }
        int mk = data[br.pos + 1];
        br.pos += 2;
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) continue;
        if (mk == 0xD9) break;
        if (br.pos + 1 >= br.n) break;
        int seg = (data[br.pos] << 8) | data[br.pos + 1];
        br.pos += 2;
        if (seg < 2 || br.pos + (size_t)seg - 2 > br.n) break;
        const uint8_t *p = data + br.pos;
        int plen = seg - 2;

        switch (mk) {
            case 0xDB: {          /* DQT */
                int i = 0;
                while (i < plen) {
                    int pq = p[i] >> 4, tq = p[i] & 15;
                    i++;
                    if (tq > 3) { i = plen; break; }
                    for (int k = 0; k < 64 && i < plen; k++) {
                        if (pq) { d->quant[tq][k] = (uint16_t)((p[i] << 8) | (i + 1 < plen ? p[i + 1] : 0)); i += 2; }
                        else    { d->quant[tq][k] = p[i]; i++; }
                    }
                    d->qok[tq] = 1;
                }
                break;
            }
            case 0xC4: {          /* DHT */
                int i = 0;
                while (i + 17 <= plen) {
                    int tc = p[i] >> 4, th = p[i] & 15;
                    i++;
                    if (th > 3) break;
                    uint8_t bits[17];
                    bits[0] = 0;
                    int total = 0;
                    for (int l = 1; l <= 16; l++) { bits[l] = p[i + l - 1]; total += bits[l]; }
                    i += 16;
                    if (total > 256 || i + total > plen) { i = plen; break; }
                    if (tc == 0) huff_build(&d->dc[th], bits, p + i);
                    else         huff_build(&d->ac[th], bits, p + i);
                    i += total;
                }
                break;
            }
            case 0xC0:
            case 0xC1: {          /* 基线 / 扩展顺序 */
                if (plen < 6) break;
                d->fr.h = (p[1] << 8) | p[2];
                d->fr.w = (p[3] << 8) | p[4];
                int nf = p[5];
                if (p[0] != 8) { if (err) snprintf(err, errsz, "只支持 8bit JPEG"); goto done; }
                if (nf < 1 || nf > 4) { if (err) snprintf(err, errsz, "分量数异常"); goto done; }
                if (!d->fr.w || !d->fr.h) { if (err) snprintf(err, errsz, "图像尺寸为 0"); goto done; }
                d->fr.ncomp = nf;
                int i = 6;
                d->fr.hmax = d->fr.vmax = 1;
                for (int k = 0; k < nf; k++) {
                    if (i + 3 > plen) { if (err) snprintf(err, errsz, "SOF 段不完整"); goto done; }
                    d->fr.c[k].id = p[i];
                    d->fr.c[k].h = p[i + 1] >> 4;
                    d->fr.c[k].v = p[i + 1] & 15;
                    d->fr.c[k].tq = p[i + 2];
                    if (d->fr.c[k].h < 1) d->fr.c[k].h = 1;
                    if (d->fr.c[k].v < 1) d->fr.c[k].v = 1;
                    if (d->fr.c[k].h > d->fr.hmax) d->fr.hmax = d->fr.c[k].h;
                    if (d->fr.c[k].v > d->fr.vmax) d->fr.vmax = d->fr.c[k].v;
                    i += 3;
                }
                saw_sof = 1;
                break;
            }
            case 0xC2:
            case 0xC6:
            case 0xCA:
                if (err) snprintf(err, errsz, "渐进式 JPEG 暂不支持");
                goto done;
            case 0xDD:            /* DRI */
                if (plen >= 2) d->fr.dri = (p[0] << 8) | p[1];
                break;
            case 0xDA: {          /* SOS：开始扫描 */
                if (!saw_sof) goto done;
                /* 分量 -> 霍夫曼表选择。这一段以前被整段跳过，于是色度
                 * 一直用 0 号表解码。平滑的图看不出问题（色度块几乎全是
                 * 直流），一旦图里有细节，色度表选错立刻位流错位，
                 * 解到第几十个块就崩。 */
                if (plen >= 1) {
                    int ns = p[0];
                    if (ns > 0 && ns <= d->fr.ncomp) {
                        for (int k = 0; k < ns; k++) {
                            if (1 + k * 2 + 1 >= plen) break;
                            int cs = p[1 + k * 2];
                            int tdta = p[2 + k * 2];
                            for (int c = 0; c < d->fr.ncomp; c++)
                                if (d->fr.c[c].id == cs) {
                                    d->fr.c[c].td = (tdta >> 4) & 3;
                                    d->fr.c[c].ta = tdta & 3;
                                }
                        }
                    }
                }
                if (d->fr.w * (long long)d->fr.h > 40000000LL) {
                    if (err) snprintf(err, errsz, "图片过大");
                    goto done;
                }
                /* 选缩放倍率：解码宽度控制在 [target, 2*target) 之间 */
                int scale = 1;
                while (scale < 8) {
                    int half = (d->fr.w + 2 * scale - 1) / (2 * scale);
                    if (half >= (target_w > 0 ? target_w : 700)) scale *= 2;
                    else break;
                }
                d->bs = 8 / scale;
                int luma = 0;
                for (int k = 1; k < d->fr.ncomp; k++)
                    if (d->fr.c[k].h * d->fr.c[k].v > d->fr.c[luma].h * d->fr.c[luma].v) luma = k;

                d->fr.mcus_x = (d->fr.w + 8 * d->fr.hmax - 1) / (8 * d->fr.hmax);
                d->fr.mcus_y = (d->fr.h + 8 * d->fr.vmax - 1) / (8 * d->fr.vmax);
                d->pw = d->fr.mcus_x * d->fr.c[luma].h * d->bs;
                d->ph = d->fr.mcus_y * d->fr.c[luma].v * d->bs;
                if (d->pw <= 0 || d->ph <= 0 ||
                    (long long)d->pw * d->ph > 12000000LL) {
                    if (err) snprintf(err, errsz, "图片过大");
                    goto done;
                }
                d->plane = xmalloc((size_t)d->pw * (size_t)d->ph);
                memset(d->plane, 255, (size_t)d->pw * (size_t)d->ph);

                br.pos += plen;          /* 跳过 SOS 头，进入熵编码数据 */
                int per_mcu = 0;
                for (int k = 0; k < d->fr.ncomp; k++)
                    per_mcu += d->fr.c[k].h * d->fr.c[k].v;
                d->blocks_expect = d->fr.mcus_x * d->fr.mcus_y * per_mcu;
                decode_scan(&br, d);
                /* 数据截断（Kindle 上网络断在半张图很常见）时，只要解出
                 * 四分之一以上的块就先用着；更少说明文件本来就不是这张图，
                 * 直接判失败，别把半屏灰块贴到正文里。 */
                if (d->blocks_expect > 0 && d->blocks_done * 4 < d->blocks_expect) {
                    if (err) snprintf(err, errsz, "JPEG 数据不完整（%d/%d 块）",
                                      d->blocks_done, d->blocks_expect);
                    goto done;
                }
                saw_sos = 1;

                /* 有效区域：亮度分量真正的样本数 */
                int cw = (d->fr.w * d->fr.c[luma].h + d->fr.hmax - 1) / d->fr.hmax;
                int ch = (d->fr.h * d->fr.c[luma].v + d->fr.vmax - 1) / d->fr.vmax;
                int vw = (cw * d->bs + 7) / 8;
                int vh = (ch * d->bs + 7) / 8;
                if (vw > d->pw) vw = d->pw;
                if (vh > d->ph) vh = d->ph;
                if (vw <= 0 || vh <= 0) { if (err) snprintf(err, errsz, "解码结果为空"); goto done; }

                int dw = target_w > 0 ? target_w : vw;
                if (dw > vw) dw = vw;
                int dh = (int)((long long)vh * dw / vw);
                if (dh < 1) dh = 1;
                if (max_h > 0 && dh > max_h) {
                    dh = max_h;
                    dw = (int)((long long)vw * dh / vh);
                    if (dw < 1) dw = 1;
                }
                uint8_t *small = box_down(d->plane, d->pw, vw, vh, dw, dh);
                if (!small) { if (err) snprintf(err, errsz, "内存不足"); goto done; }
                free(d->plane);
                d->plane = NULL;
                out->px = small;
                out->w = dw;
                out->h = dh;
                rc = 0;
                goto done;
            }
            default:
                break;
        }
        br.pos += (size_t)plen;
    }

done:
    free(d->plane);
    free(d);
    if (rc != 0 && err && !err[0]) snprintf(err, errsz, "JPEG 解析失败");
    (void)saw_sos;
    return rc;
}

void img_free(ImgGray *g) {
    if (!g) return;
    free(g->px);
    g->px = NULL;
    g->w = g->h = 0;
}

/* ---------------- 缓存 ---------------- */

#define IMG_MAGIC 0x3147485Au   /* "ZHG1" 小端 */

/* 1.2.9：缓存按文章分目录。<home>/cache/img/<文章>/<md5>_<宽>.zhg。
 * 文章 key 由调用方给（问题 id / 专栏 id / 标题的 md5 前 12 位都行）；
 * 不给（旧路径/自检）就落回平铺目录。查询时两处都找一遍，
 * 老用户升级后已有缓存不作废。 */
static char g_cache_sub[24];

void img_cache_set_article(const char *key) {
    g_cache_sub[0] = 0;
    if (key && *key) {
        char hex[33];
        md5_hex(key, strlen(key), hex);
        memcpy(g_cache_sub, hex, 12);
        g_cache_sub[12] = 0;
    }
}

static int cache_path(char *out, size_t n, const char *url, int w) {
    char hex[33];
    md5_hex(url, strlen(url), hex);
    if (g_cache_sub[0])
        snprintf(out, n, "%s/cache/img/%s/%s_%d.zhg", zh_home(), g_cache_sub, hex, w);
    else
        snprintf(out, n, "%s/cache/img/%s_%d.zhg", zh_home(), hex, w);
    return 0;
}

/* 平铺位置（1.2.8 及以前），仅查询时兼容用 */
static int cache_path_flat(char *out, size_t n, const char *url, int w) {
    char hex[33];
    md5_hex(url, strlen(url), hex);
    snprintf(out, n, "%s/cache/img/%s_%d.zhg", zh_home(), hex, w);
    return 0;
}

long img_cache_count(void) {
    char dir[320];
    snprintf(dir, sizeof(dir), "%s/cache/img", zh_home());
    long n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* 文章子目录：数里面的 .zhg */
            DIR *d2 = opendir(p);
            if (!d2) continue;
            struct dirent *e2;
            while ((e2 = readdir(d2)) != NULL) {
                if (e2->d_name[0] == '.') continue;
                if (strstr(e2->d_name, ".zhg")) n++;
            }
            closedir(d2);
        } else if (strstr(e->d_name, ".zhg")) {
            n++;
        }
    }
    closedir(d);
    return n;
}

int img_cache_lookup(const char *url, int w, int *ow, int *oh) {
    char path[512];
    cache_path(path, sizeof(path), url, w);
    if (img_cache_dims(path, ow, oh) == 0) return 0;
    /* 旧版平铺兼容 */
    cache_path_flat(path, sizeof(path), url, w);
    return img_cache_dims(path, ow, oh);
}

int img_cache_fill(const char *url, int w, char *path, size_t n, int *ow, int *oh) {
    if (!url || !path || n == 0) return -1;
    cache_path(path, n, url, w);
    if (img_cache_dims(path, ow, oh) == 0) return 0;
    cache_path_flat(path, n, url, w);
    return img_cache_dims(path, ow, oh);
}

int img_cache_dims(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t hdr[3];
    int ok = fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == IMG_MAGIC;
    fclose(f);
    if (!ok) return -1;
    if (w) *w = (int)hdr[1];
    if (h) *h = (int)hdr[2];
    return (hdr[1] > 0 && hdr[2] > 0) ? 0 : -1;
}

/* 缓存文件名里的宽度必须**固定**用 IMG_CACHE_W，不能用解出来的 g->w。
 *
 * 1.2.7 的真机上"图片一张都出不来"就是这行：竖长图会被 max_h 压窄
 * （1200x2297 在 max_h=1300 下解出 679x1300），于是存成 ..._679.zhg，
 * 而查缓存时用的是 1024，永远查不到 —— 下载明明成功，界面一直占位，
 * 最后一律退成"[图片]（未能加载）"。窗口里跑的是正好 1024 宽的测试图，
 * 所以自检一路绿。 */
int img_cache_save(const char *url, const ImgGray *g) {
    if (!url || !g || !g->px || g->w <= 0 || g->h <= 0) return -1;
    char dir[320];
    if (g_cache_sub[0])
        snprintf(dir, sizeof(dir), "%s/cache/img/%s", zh_home(), g_cache_sub);
    else
        snprintf(dir, sizeof(dir), "%s/cache/img", zh_home());
    mkdir_p(dir);
    char path[512];
    cache_path(path, sizeof(path), url, IMG_CACHE_W);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t hdr[3] = { IMG_MAGIC, (uint32_t)g->w, (uint32_t)g->h };
    fwrite(hdr, sizeof(hdr), 1, f);
    size_t n = (size_t)g->w * (size_t)g->h;
    size_t wn = fwrite(g->px, 1, n, f);
    fclose(f);
    if (wn != n) { remove(path); return -1; }
    return 0;
}

int img_cache_load(const char *path, ImgGray *g) {
    if (!g) return -1;
    g->w = g->h = 0;
    g->px = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t hdr[3];
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != IMG_MAGIC ||
        hdr[1] == 0 || hdr[2] == 0 || hdr[1] > 4096 || hdr[2] > 8192) {
        fclose(f);
        return -1;
    }
    size_t n = (size_t)hdr[1] * (size_t)hdr[2];
    uint8_t *px = xmalloc(n);
    size_t got = fread(px, 1, n, f);
    fclose(f);
    if (got != n) { free(px); return -1; }
    g->px = px;
    g->w = (int)hdr[1];
    g->h = (int)hdr[2];
    return 0;
}

/* ---------------- 自检 ---------------- */

#include "img_test_jpeg.h"

/* 真机上没有 gdb，解码器一旦有偏差只能看到"图片发灰/发黑"。
 * 这里用内置的 32x32 四象限图把三件事钉死：
 *   尺寸对、四个象限的亮度对（说明反量化/IDCT/YCbCr 全对）、缓存往返。 */
int img_selftest(void) {
    ImgGray g;
    char err[128] = "";
    if (img_decode_gray(IMG_TEST_JPEG, IMG_TEST_JPEG_LEN, 32, 0, &g, err, sizeof(err)) != 0) {
        logmsg("selfcheck: img 解码失败 %s", err);
        return 0;
    }
    int ok = (g.w == 32 && g.h == 32);
    struct { int x, y, want; } q[] = {
        { 8, 8, 76 }, { 24, 8, 150 }, { 8, 24, 29 }, { 24, 24, 200 },
    };
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        int v = g.px[q[i].y * g.w + q[i].x];
        int d = v - q[i].want;
        if (d < 0) d = -d;
        if (d > 12) {
            logmsg("selfcheck: img 象限%d 得到 %d 期望 %d", (int)i, v, q[i].want);
            ok = 0;
        }
    }
    /* PNG 必须被明确拒绝（返回 -1 并给出原因），不能解出垃圾。
     * （只有 8 字节签名，stb 解不出内容，必须失败） */
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    ImgGray bad;
    char e2[64] = "";
    if (img_decode_gray(png, sizeof(png), 32, 0, &bad, e2, sizeof(e2)) == 0) ok = 0;

    /* 1.2.9：PNG 往返 —— png_write_gray8 写一张，img_decode_gray 要能解回来
     * （1.2.8 真机上"解码失败，不是 JPEG"就是缺这一环） */
    {
        uint8_t row[48];
        for (int i = 0; i < 48; i++) row[i] = (uint8_t)(i * 5);
        char ppath[512];
        snprintf(ppath, sizeof(ppath), "%s/cache/img/selftest.png", zh_home());
        if (png_write_gray8(ppath, 48, 3, row, 48) == 0) {
            size_t pn = 0;
            uint8_t *pdat = (uint8_t *)read_file(ppath, &pn);
            if (pdat) {
                ImgGray pg;
                char pe[96] = "";
                if (img_decode_gray(pdat, pn, 48, 0, &pg, pe, sizeof(pe)) == 0) {
                    if (pg.w != 48 || pg.h != 3) ok = 0;
                    int diffs = 0;
                    for (int i = 0; i < 48; i++)
                        if (abs(pg.px[i] - row[i]) > 24) diffs++;
                    if (diffs > 4) ok = 0;    /* 灰度直存直解，误差应该很小 */
                    img_free(&pg);
                } else {
                    logmsg("selfcheck: png 解码失败 %s", pe);
                    ok = 0;
                }
                free(pdat);
            } else ok = 0;
            remove(ppath);
        } else ok = 0;
    }

    /* 1.2.9：WebP —— 有损(VP8)与无损(VP8L)都要能出灰度 */
#include "img_test_webp.h"
    {
        ImgGray wg;
        char we[96] = "";
        if (img_decode_gray(IMG_TEST_WEBP_LL, IMG_TEST_WEBP_LL_LEN, 16, 0, &wg, we, sizeof(we)) == 0) {
            /* 无损：四象限灰度应基本还原 */
            int v1 = wg.px[4 * wg.w + 4], v2 = wg.px[12 * wg.w + 12];
            if (abs(v1 - 60) > 40 || abs(v2 - 230) > 40) ok = 0;
            img_free(&wg);
        } else {
            logmsg("selfcheck: webp(无损) 解码失败 %s", we);
            ok = 0;
        }
        if (img_decode_gray(IMG_TEST_WEBP_LOSSY, IMG_TEST_WEBP_LOSSY_LEN, 16, 0, &wg, we, sizeof(we)) == 0) {
            img_free(&wg);                 /* 有损：能解出正确尺寸就算过 */
        } else {
            logmsg("selfcheck: webp(有损) 解码失败 %s", we);
            ok = 0;
        }
    }

    /* 1.2.9：文章分目录 —— 设 key 后保存，文件应落在子目录里，且能查到 */
    {
        const char *u2 = "https://pic1.zhimg.com/selftest-sub.jpg";
        img_cache_set_article("article-selftest");
        if (img_cache_save(u2, &g) != 0) ok = 0;
        int cw2 = 0, ch2 = 0;
        if (img_cache_lookup(u2, IMG_CACHE_W, &cw2, &ch2) != 0) ok = 0;
        /* 文件必须真在子目录里（而不是平铺位置） */
        char hex2[33], hex3[33];
        md5_hex(u2, strlen(u2), hex2);
        md5_hex("article-selftest", strlen("article-selftest"), hex3);
        char dp2[320];
        snprintf(dp2, sizeof(dp2), "%s/cache/img/%.12s", zh_home(), hex3);
        char fp[512];
        snprintf(fp, sizeof(fp), "%s/%s_%d.zhg", dp2, hex2, IMG_CACHE_W);
        if (!file_exists(fp)) ok = 0;
        remove(fp);
        rmdir(dp2);
        img_cache_set_article(NULL);
    }

    /* 缓存往返：写进去、按 URL 查尺寸、再整张读回来 */
    const char *url = "https://pic1.zhimg.com/selftest.jpg";
    if (img_cache_save(url, &g) != 0) ok = 0;
    int cw = 0, ch = 0;
    /* 关键：查询用的宽度是 IMG_CACHE_W，而图本身只有 32 宽。
     * 文件名必须按 IMG_CACHE_W 起，否则真机上"竖长图被压窄再存盘"
     * 就会永远查不到（1.2.7 的图片全灭就是这个）。 */
    if (img_cache_lookup(url, IMG_CACHE_W, &cw, &ch) != 0 || cw != g.w || ch != g.h) ok = 0;
    ImgGray back;
    char path[512];
    char hex[33];
    md5_hex(url, strlen(url), hex);
    snprintf(path, sizeof(path), "%s/cache/img/%s_%d.zhg", zh_home(), hex, IMG_CACHE_W);
    if (img_cache_load(path, &back) == 0) {
        if (back.w != g.w || back.h != g.h ||
            memcmp(back.px, g.px, (size_t)g.w * (size_t)g.h) != 0) ok = 0;
        img_free(&back);
    } else {
        ok = 0;
    }
    remove(path);
    img_free(&g);
    return ok;
}
