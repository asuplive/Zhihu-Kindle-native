/* png.h —— 极简 8bit 灰度 PNG 写出器（不依赖 zlib）
 *
 * 用途：KO3 上 mxcfb 的 SEND_UPDATE 全部被内核拒（EINVAL），但系统自带的
 * eips 能正常刷屏。eips 支持 `eips -g <图片>`，把整页像素交给它，
 * 是探测失败时唯一"确实能把画面推上屏"的后路。
 *
 * 静态链接的程序里没有 zlib，所以这里只用 deflate 的 stored（不压缩）块：
 * 输出的 PNG 体积约等于一页像素（2MB 左右），Kindle 的存储完全吃得下，
 * 换来的是零外部依赖和可离线验证。
 */
#ifndef ZH_PNG_H
#define ZH_PNG_H

#include <stddef.h>
#include <stdint.h>

/* 把 data 当成 h 行、每行 stride 字节的 8bit 灰度图写成 PNG。
 * 返回 0 成功，-1 失败（打不开文件 / 参数非法）。 */
int png_write_gray8(const char *path, int w, int h, const uint8_t *data, int stride);

/* 结构自检：逐块校验签名、IHDR、每个 chunk 的 CRC、IDAT 长度与 IEND。
 * 返回 1 合法，0 非法（err 里写原因）。w/h 可为 NULL。 */
int png_verify(const char *path, int *w, int *h, char *err, int errsz);

#endif /* ZH_PNG_H */
