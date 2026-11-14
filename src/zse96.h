/* zse96.h —— 知乎 x-zse-96 v2 请求签名
 *
 * 算法移植自 zhihu-plus-plus（AGPL-3.0）的 ZseSigner.kt / ZhihuFetchSignature.kt。
 * 因为是网页端 API，绝大多数接口都要校验这个头，否则一律 403。
 * 算法全部是 32 位整数运算，Kindle 上单次签名 < 1ms。
 */
#ifndef ZH_ZSE96_H
#define ZH_ZSE96_H

#include <stddef.h>

#define ZSE93_VERSION "101_3_3.0"

/* 生成 x-zse-96 头值（含 "2.0_" 前缀）。
 * url   : 完整 URL 或 path?query（签名只看 path+query）
 * dc0   : 设备 cookie d_c0 的值，可为 NULL
 * body  : POST body，GET 传 NULL
 */
void zse96_sign(const char *url, const char *dc0, const char *body, char *out, size_t outsz);

/* 底层：把任意字符串（通常是 32 字节 md5 hex）加密成自定义 base64 */
void zse96_encrypt_v4(const char *in, size_t inlen, char *out, size_t outsz);

/* 从 URL 里取 path?query 部分 */
void zse96_pathname(const char *url, char *out, size_t outsz);

#endif
