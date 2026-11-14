/* qr.c —— 二维码生成 + 帧缓冲绘制 */
#include "qr.h"
#include "qrcodegen.h"

#include <stdlib.h>
#include <string.h>

#define QR_MAX_VER   12      /* 登录 link 约 80 字节，version 6~8 足够；留余量 */
#define QR_QUIET     4       /* 每侧静区模块数 */

int qr_matrix(const char *text, uint8_t **bits, int *n) {
    if (!text || !*text || !bits || !n) return -1;
    size_t len = strlen(text);
    if (len > 900) return -1;   /* 防御：超长文本不让编码 */

    /* 缓冲区按最大版本申请，避免栈上大数组；Kindle 内存紧张，用堆 */
    size_t buflen = (size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VER);
    uint8_t *tmp = (uint8_t *)malloc(buflen);
    uint8_t *qr  = (uint8_t *)malloc(buflen);
    if (!tmp || !qr) { free(tmp); free(qr); return -1; }

    /* ECC_LOW 让码元尽量稀疏，在 300ppi 灰阶屏上更好扫；
     * boostEcl=true 允许库自动提升纠错等级，兼容性更好。 */
    bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, QR_MAX_VER,
                                   qrcodegen_Mask_AUTO, true);
    free(tmp);
    if (!ok) { free(qr); return -1; }

    int size = qrcodegen_getSize(qr);
    int side = size + QR_QUIET * 2;
    uint8_t *m = (uint8_t *)calloc((size_t)side * (size_t)side, 1);
    if (!m) { free(qr); return -1; }
    for (int yy = 0; yy < size; yy++) {
        for (int xx = 0; xx < size; xx++) {
            if (qrcodegen_getModule(qr, xx, yy))
                m[(yy + QR_QUIET) * side + (xx + QR_QUIET)] = 1;
        }
    }
    free(qr);
    *bits = m;
    *n = side;
    return 0;
}

int qr_draw(Fb *fb, const char *text, int x, int y, int box_w, int box_h) {
    uint8_t *bits = NULL;
    int n = 0;
    if (qr_matrix(text, &bits, &n) != 0) return -1;

    int avail = box_w < box_h ? box_w : box_h;
    int scale = avail / n;
    if (scale < 1) { free(bits); return -1; }
    /* 每模块至少 3 像素，否则手机扫不出来 */
    if (scale < 3) { free(bits); return -2; }

    int side_px = scale * n;
    int ox = x + (box_w - side_px) / 2;
    int oy = y + (box_h - side_px) / 2;

    /* 先铺白底（含静区），再逐模块画黑块。
     * 用整块 fb_fill 而不是逐像素，老机型也快。 */
    fb_fill(fb, ox, oy, side_px, side_px, 255);
    for (int yy = 0; yy < n; yy++) {
        int run = -1;
        for (int xx = 0; xx <= n; xx++) {
            int black = (xx < n) && bits[yy * n + xx];
            if (black && run < 0) run = xx;
            if (!black && run >= 0) {
                fb_fill(fb, ox + run * scale, oy + yy * scale,
                        (xx - run) * scale, scale, 0);
                run = -1;
            }
        }
    }
    free(bits);
    return side_px;
}
