/* qr.h —— 把文本编码成二维码并直接画到帧缓冲
 *
 * 二维码登录的关键：Kindle 没有摄像头，但可以在屏幕上「显示」二维码，
 * 用户拿手机知乎 App 扫屏幕即可。因此必须能在设备端本地生成 QR 位图，
 * 不能依赖服务器返回图片（也没有 PNG/JPEG 解码器）。
 *
 * 底层用 Project Nayuki 的 qrcodegen（MIT，纯 C99，无依赖），见 qrcodegen.c。
 */
#ifndef ZH_QR_H
#define ZH_QR_H

#include "fb.h"

/* 在 (x,y) 为左上角的 box_w×box_h 区域内居中画二维码。
 * 返回实际占用的像素边长；<=0 表示文本过长或编码失败。
 * 底色白、码元黑，自动带 4 模块静区。 */
int qr_draw(Fb *fb, const char *text, int x, int y, int box_w, int box_h);

/* 只生成矩阵，不绘制。n 为边长（含静区），bits[y*n+x]=1 表示黑。
 * 返回 0 成功；调用者 free(*bits)。用于自检与离线验证。 */
int qr_matrix(const char *text, uint8_t **bits, int *n);

#endif
