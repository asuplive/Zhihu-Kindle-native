/* md5.h —— MD5 摘要（签名流程必需，纯 C 实现，无外部依赖） */
#ifndef ZH_MD5_H
#define ZH_MD5_H

#include <stdint.h>
#include <stddef.h>

/* 输出 32 字节小写十六进制，并补 '\0'；out 至少 33 字节 */
void md5_hex(const void *data, size_t len, char out[33]);
/* 输出 16 字节原始摘要 */
void md5_raw(const void *data, size_t len, uint8_t out[16]);

#endif
