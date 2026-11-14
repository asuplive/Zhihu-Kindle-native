/* net.h —— 独立网络栈：DNS(UDP) + TCP + TLS(mbedTLS) + HTTP/1.1
 *
 * 全部在设备端完成。不用系统 libcurl、不用系统 DNS（Kindle 静态 glibc 的
 * NSS 在静态链接下不可靠），因此自带一个最小的 A 记录解析器。
 */
#ifndef ZH_NET_H
#define ZH_NET_H

#include "common.h"

typedef struct {
    int   status;        /* HTTP 状态码，0 表示失败 */
    Buf   body;          /* 解 chunked 之后的正文 */
    Buf   set_cookie;    /* 所有 Set-Cookie 行，'\n' 分隔 */
    char  err[256];
    int   insecure;      /* 1 = 未校验证书（无 CA 时的降级） */
} HttpResp;

void http_resp_init(HttpResp *r);
void http_resp_free(HttpResp *r);

/* 发起请求。extra_headers 形如 "a: b\r\nc: d"，可为 NULL。
 * 返回 0 表示拿到 HTTP 响应（status 有效），-1 表示网络层失败。
 * cancel 非空时，读循环里轮询，置 1 可中断（返回 -1, err="已取消"）。
 */
int http_request(const char *method, const char *url,
                 const char *extra_headers, const char *body,
                 HttpResp *resp, volatile int *cancel);

/* 覆盖 User-Agent。
 * 知乎按 UA 把请求分到不同的后端：带 `desktop=true` 的接口如果配一个
 * Android 手机 UA，很容易被分到"非浏览器"通道上（表现为 404/请升级客户端）。
 * api.c 在调网页端接口前会切成桌面 Chrome UA，其它地方保持默认移动端 UA。 */
void http_set_user_agent(const char *ua);
const char *http_user_agent(void);

/* 供测试/诊断：解析域名，成功返回 0 并写入 ip */
int net_resolve(const char *host, char *ip, size_t ipsz);

#endif
