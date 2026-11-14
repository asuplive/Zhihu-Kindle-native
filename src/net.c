/* net.c —— 见 net.h */
#include "net.h"
#include "ca_bundle.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

/* ------------------------------------------------ 时间 */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------ DNS */

/* 默认 UA：登录/二维码这些走移动端更稳。api.c 调网页端接口时会临时换成桌面 UA。 */
#define ZH_UA_DEFAULT "Mozilla/5.0 (Linux; Android 9; P1) AppleWebKit/537.36 " \
                       "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36"
static char g_ua[256] = ZH_UA_DEFAULT;

void http_set_user_agent(const char *ua) {
    if (!ua || !*ua) { snprintf(g_ua, sizeof(g_ua), "%s", ZH_UA_DEFAULT); return; }
    snprintf(g_ua, sizeof(g_ua), "%s", ua);
}
const char *http_user_agent(void) { return g_ua; }

static int parse_resolv(char *out, size_t outsz) {
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10) == 0) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            char *e = p;
            while (*e && *e != '\n' && *e != ' ' && *e != '\t' && *e != '#') e++;
            *e = 0;
            if (*p) { snprintf(out, outsz, "%s", p); found = 0; break; }
        }
    }
    fclose(f);
    if (found != 0) snprintf(out, outsz, "8.8.8.8");
    return found;
}

static int udp_query(const char *server, const char *host, char *ip, size_t ipsz, int timeout_ms) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    if (inet_pton(AF_INET, server, &sa.sin_addr) != 1) { close(s); return -1; }

    uint8_t q[512];
    memset(q, 0, sizeof(q));
    q[0] = (uint8_t)(rand() & 0xFF);
    q[1] = (uint8_t)(rand() & 0xFF);
    q[2] = 0x01; /* recursion desired */
    q[5] = 1;    /* qdcount = 1 */
    size_t n = 12;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lab = dot ? (size_t)(dot - p) : strlen(p);
        if (lab == 0 || lab > 63 || n + lab + 2 >= sizeof(q)) { close(s); return -1; }
        q[n++] = (uint8_t)lab;
        memcpy(q + n, p, lab);
        n += lab;
        if (!dot) break;
        p = dot + 1;
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1; /* QTYPE A */
    q[n++] = 0;
    q[n++] = 1; /* QCLASS IN */

    if (sendto(s, q, n, 0, (struct sockaddr *)&sa, sizeof(sa)) != (ssize_t)n) {
        close(s);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t r[1500];
    ssize_t got = recv(s, r, sizeof(r), 0);
    close(s);
    if (got < 12) return -1;
    if ((r[0] != q[0]) || (r[1] != q[1])) return -1;
    int ancount = (r[6] << 8) | r[7];
    if (ancount <= 0) return -1;

    /* 跳过 question */
    size_t off = 12;
    while (off < (size_t)got && r[off] != 0) {
        if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
        off += (size_t)r[off] + 1;
    }
    if (off < (size_t)got && r[off] == 0) off++;
    off += 4;

    for (int i = 0; i < ancount && off + 12 <= (size_t)got; i++) {
        /* name */
        while (off < (size_t)got) {
            if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
            if (r[off] == 0) { off++; break; }
            off += (size_t)r[off] + 1;
        }
        if (off + 10 > (size_t)got) break;
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (off + (size_t)rdlen > (size_t)got) break;
        if (type == 1 && rdlen == 4) {
            snprintf(ip, ipsz, "%u.%u.%u.%u", r[off], r[off + 1], r[off + 2], r[off + 3]);
            return 0;
        }
        off += (size_t)rdlen;
    }
    return -1;
}

int net_resolve(const char *host, char *ip, size_t ipsz) {
    struct in_addr a4;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        snprintf(ip, ipsz, "%s", host);
        return 0;
    }
    /* /etc/hosts 优先（有些越狱环境在这里写死了映射） */
    FILE *hf = fopen("/etc/hosts", "r");
    if (hf) {
        char line[512];
        while (fgets(line, sizeof(line), hf)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            char ipbuf[64] = {0}, name[256] = {0};
            if (sscanf(line, "%63s %255s", ipbuf, name) == 2 && str_ieq(name, host)) {
                fclose(hf);
                snprintf(ip, ipsz, "%s", ipbuf);
                return 0;
            }
        }
        fclose(hf);
    }

    char ns[3][64];
    int nns = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && nns < 3) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "nameserver", 10) == 0) {
                p += 10;
                while (*p == ' ' || *p == '\t') p++;
                char *e = p;
                while (*e && *e != '\n' && *e != ' ' && *e != '\t') e++;
                *e = 0;
                if (*p) snprintf(ns[nns++], sizeof(ns[0]), "%s", p);
            }
        }
        fclose(f);
    }
    if (nns == 0) {
        snprintf(ns[0], sizeof(ns[0]), "223.5.5.5");
        snprintf(ns[1], sizeof(ns[1]), "8.8.8.8");
        nns = 2;
    }
    for (int i = 0; i < nns; i++)
        if (udp_query(ns[i], host, ip, ipsz, 4000) == 0) return 0;
    return -1;
}

/* ------------------------------------------------ TCP */

static int tcp_connect(const char *ip, int port, int timeout_ms, char *err, size_t errsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { snprintf(err, errsz, "socket: %s", strerror(errno)); return -1; }

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &sa.sin_addr);

    int r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (r != 0 && errno != EINPROGRESS) {
        snprintf(err, errsz, "连接 %s:%d 失败: %s", ip, port, strerror(errno));
        close(s);
        return -1;
    }
    if (r != 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(s + 1, NULL, &wf, NULL, &tv);
        if (sel <= 0) {
            snprintf(err, errsz, "连接 %s:%d 超时", ip, port);
            close(s);
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            snprintf(err, errsz, "连接被拒绝: %s", strerror(soerr));
            close(s);
            return -1;
        }
    }
    fcntl(s, F_SETFL, flags);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

/* ------------------------------------------------ TLS 上下文 */

typedef struct {
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_x509_crt         cacert;
    int                      had_ca;
    int                      insecure;
    int                      sock;
    long                     deadline;
    volatile int            *cancel;
    int                      timed_out;
} Tls;

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    Tls *t = (Tls *)ctx;
    ssize_t n = send(t->sock, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    Tls *t = (Tls *)ctx;
    if (t->cancel && *t->cancel) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (now_ms() > t->deadline) { t->timed_out = 1; return MBEDTLS_ERR_NET_RECV_FAILED; }
    ssize_t n = recv(t->sock, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    t->deadline = now_ms() + 30000; /* 每收到数据就续期 */
    return (int)n;
}

static int load_ca(mbedtls_x509_crt *crt) {
    /* 用户自备 CA 优先 */
    static const char *paths[] = {
        "/mnt/us/zhihu/ca.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/usr/local/share/ca-certificates/ca-bundle.crt",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (!file_exists(paths[i])) continue;
        size_t n = 0;
        char *pem = read_file(paths[i], &n);
        if (!pem) continue;
        int r = mbedtls_x509_crt_parse(crt, (const unsigned char *)pem, n + 1);
        free(pem);
        if (r == 0) return 0;
    }
    /* 内置 CA 兜底 */
    int r = mbedtls_x509_crt_parse(crt, (const unsigned char *)ZH_CA_BUNDLE,
                                   strlen(ZH_CA_BUNDLE) + 1);
    return r == 0 ? 0 : -1;
}

/* ------------------------------------------------ URL 解析 */

typedef struct {
    char scheme[8];
    char host[128];
    int  port;
    char path[1024];
} Url;

static int parse_url(const char *url, Url *u) {
    memset(u, 0, sizeof(*u));
    const char *p = url;
    const char *sep = strstr(p, "://");
    if (sep) {
        size_t n = (size_t)(sep - p);
        if (n >= sizeof(u->scheme)) return -1;
        memcpy(u->scheme, p, n);
        u->scheme[n] = 0;
        p = sep + 3;
    } else {
        snprintf(u->scheme, sizeof(u->scheme), "https");
    }
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *q = p; q < hostend; q++) if (*q == ':') colon = q;
    size_t hl = (size_t)((colon ? colon : hostend) - p);
    if (hl >= sizeof(u->host)) return -1;
    memcpy(u->host, p, hl);
    u->host[hl] = 0;
    u->port = str_ieq(u->scheme, "http") ? 80 : 443;
    if (colon) u->port = atoi(colon + 1);
    if (slash) snprintf(u->path, sizeof(u->path), "%s", slash);
    else snprintf(u->path, sizeof(u->path), "/");
    return 0;
}

/* ------------------------------------------------ 响应解析 */

static void header_value(const char *headers, const char *name, char *out, size_t outsz) {
    out[0] = 0;
    size_t nl = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        size_t linelen = e ? (size_t)(e - p) : strlen(p);
        if (linelen > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            size_t vl = linelen - (size_t)(v - p);
            if (vl >= outsz) vl = outsz - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            /* 去掉尾部空白 */
            while (vl > 0 && (out[vl - 1] == ' ' || out[vl - 1] == '\r')) out[--vl] = 0;
            return;
        }
        p = e ? e + 2 : NULL;
    }
}

static void collect_cookies(const char *headers, Buf *out) {
    const char *p = headers;
    size_t nl = 10; /* set-cookie */
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        size_t linelen = e ? (size_t)(e - p) : strlen(p);
        if (linelen > nl && strncasecmp(p, "set-cookie", nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            size_t vl = linelen - (size_t)(v - p);
            /* 只取 name=value 部分 */
            size_t eq = 0;
            while (eq < vl && v[eq] != ';' && v[eq] != '=') eq++;
            if (eq < vl && v[eq] == '=') {
                size_t end = eq + 1;
                while (end < vl && v[end] != ';') end++;
                buf_add(out, v, end);
                buf_addc(out, '\n');
            }
        }
        p = e ? e + 2 : NULL;
    }
}

/* 从 buf 中截取 chunked 正文 */
static int dechunk(const char *in, size_t n, Buf *out) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && in[j] != '\r') j++;
        if (j >= n) break;
        char sz[16];
        size_t sl = j - i;
        if (sl >= sizeof(sz)) return -1;
        memcpy(sz, in + i, sl);
        sz[sl] = 0;
        long len = strtol(sz, NULL, 16);
        if (len <= 0) return 0; /* 结束块 */
        i = j + 2;
        if (i + (size_t)len > n) len = (long)(n - i);
        buf_add(out, in + i, (size_t)len);
        i += (size_t)len;
        if (i + 1 < n && in[i] == '\r' && in[i + 1] == '\n') i += 2;
    }
    return 0;
}

/* ------------------------------------------------ 主流程 */

void http_resp_init(HttpResp *r) {
    memset(r, 0, sizeof(*r));
    buf_init(&r->body);
    buf_init(&r->set_cookie);
}

void http_resp_free(HttpResp *r) {
    buf_free(&r->body);
    buf_free(&r->set_cookie);
}

static int do_request(Url *u, const char *method, const char *extra_headers,
                      const char *body, HttpResp *resp, volatile int *cancel, int depth);

int http_request(const char *method, const char *url,
                 const char *extra_headers, const char *body,
                 HttpResp *resp, volatile int *cancel) {
    Url u;
    if (parse_url(url, &u) != 0) {
        snprintf(resp->err, sizeof(resp->err), "URL 无法解析");
        return -1;
    }
    return do_request(&u, method, extra_headers, body, resp, cancel, 0);
}

static int do_request(Url *u, const char *method, const char *extra_headers,
                      const char *body, HttpResp *resp, volatile int *cancel, int depth) {
    char ip[64];
    if (net_resolve(u->host, ip, sizeof(ip)) != 0) {
        snprintf(resp->err, sizeof(resp->err), "域名解析失败：%s", u->host);
        return -1;
    }

    int sock = tcp_connect(ip, u->port, 12000, resp->err, sizeof(resp->err));
    if (sock < 0) return -1;

    int use_tls = str_ieq(u->scheme, "https");
    Tls tls;
    memset(&tls, 0, sizeof(tls));
    tls.sock = sock;
    tls.cancel = cancel;
    tls.deadline = now_ms() + 30000;

    int ret = 0;
    if (use_tls) {
        mbedtls_ssl_init(&tls.ssl);
        mbedtls_ssl_config_init(&tls.conf);
        mbedtls_ctr_drbg_init(&tls.drbg);
        mbedtls_entropy_init(&tls.entropy);
        mbedtls_x509_crt_init(&tls.cacert);

        const char *pers = "zhihu-kindle";
        if (mbedtls_ctr_drbg_seed(&tls.drbg, mbedtls_entropy_func, &tls.entropy,
                                  (const unsigned char *)pers, strlen(pers)) != 0) {
            snprintf(resp->err, sizeof(resp->err), "随机数初始化失败");
            ret = -1;
        }
        if (ret == 0) {
            tls.had_ca = (load_ca(&tls.cacert) == 0);
            if (mbedtls_ssl_config_defaults(&tls.conf, MBEDTLS_SSL_IS_CLIENT,
                                            MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
                snprintf(resp->err, sizeof(resp->err), "TLS 配置失败");
                ret = -1;
            }
        }
        if (ret == 0) {
            mbedtls_ssl_conf_rng(&tls.conf, mbedtls_ctr_drbg_random, &tls.drbg);
            if (tls.had_ca) {
                mbedtls_ssl_conf_authmode(&tls.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_ca_chain(&tls.conf, &tls.cacert, NULL);
            } else {
                mbedtls_ssl_conf_authmode(&tls.conf, MBEDTLS_SSL_VERIFY_NONE);
                tls.insecure = 1;
                resp->insecure = 1;
            }
            if (mbedtls_ssl_setup(&tls.ssl, &tls.conf) != 0) {
                snprintf(resp->err, sizeof(resp->err), "TLS 初始化失败");
                ret = -1;
            } else {
                mbedtls_ssl_set_hostname(&tls.ssl, u->host);
                mbedtls_ssl_set_bio(&tls.ssl, &tls, bio_send, bio_recv, NULL);
                int hs;
                while ((hs = mbedtls_ssl_handshake(&tls.ssl)) != 0) {
                    if (hs == MBEDTLS_ERR_SSL_WANT_READ || hs == MBEDTLS_ERR_SSL_WANT_WRITE)
                        continue;
                    char ebuf[160] = {0};
                    mbedtls_strerror(hs, ebuf, sizeof(ebuf));
                    if (tls.timed_out) snprintf(resp->err, sizeof(resp->err), "TLS 握手超时");
                    else if (cancel && *cancel) snprintf(resp->err, sizeof(resp->err), "已取消");
                    else snprintf(resp->err, sizeof(resp->err), "TLS 握手失败：%s", ebuf);
                    ret = -1;
                    break;
                }
            }
        }
    }

    if (ret == 0) {
        /* 组装请求 */
        Buf req;
        buf_init(&req);
        buf_printf(&req, "%s %s HTTP/1.1\r\n", method, u->path);
        buf_printf(&req, "Host: %s\r\n", u->host);
        buf_printf(&req, "User-Agent: %s\r\n", g_ua);
        buf_adds(&req, "Accept: */*\r\n");
        buf_adds(&req, "Accept-Language: zh-CN,zh;q=0.9\r\n");
        buf_adds(&req, "Accept-Encoding: identity\r\n");
        buf_adds(&req, "Connection: close\r\n");
        if (extra_headers) { buf_adds(&req, extra_headers); buf_adds(&req, "\r\n"); }
        if (body) {
            buf_printf(&req, "Content-Length: %d\r\n", (int)strlen(body));
        }
        buf_adds(&req, "\r\n");
        if (body) buf_adds(&req, body);

        /* 发送 */
        size_t sent = 0;
        while (sent < req.len) {
            int n;
            if (use_tls) {
                n = mbedtls_ssl_write(&tls.ssl, (const unsigned char *)req.p + sent, req.len - sent);
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            } else {
                n = (int)send(sock, req.p + sent, req.len - sent, 0);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            }
            if (n <= 0) {
                snprintf(resp->err, sizeof(resp->err), "发送请求失败");
                ret = -1;
                break;
            }
            sent += (size_t)n;
        }
        buf_free(&req);
    }

    if (ret == 0) {
        Buf raw;
        buf_init(&raw);
        char tmp[8192];
        for (;;) {
            if (cancel && *cancel) {
                snprintf(resp->err, sizeof(resp->err), "已取消");
                ret = -1;
                break;
            }
            int n;
            if (use_tls) {
                n = mbedtls_ssl_read(&tls.ssl, (unsigned char *)tmp, sizeof(tmp));
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) n = 0;
            } else {
                n = (int)recv(sock, tmp, sizeof(tmp), 0);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            }
            if (n < 0) {
                if (raw.len == 0) {
                    if (tls.timed_out) snprintf(resp->err, sizeof(resp->err), "读取响应超时");
                    else snprintf(resp->err, sizeof(resp->err), "读取响应失败");
                    ret = -1;
                }
                break;
            }
            if (n == 0) break;
            buf_add(&raw, tmp, (size_t)n);
            if (raw.len > 8 * 1024 * 1024) break; /* 防御：8MB 上限 */
        }

        if (ret == 0 && raw.len > 0) {
            char *hdr_end = strstr(raw.p, "\r\n\r\n");
            if (!hdr_end) {
                snprintf(resp->err, sizeof(resp->err), "响应格式异常");
                ret = -1;
            } else {
                size_t hdr_len = (size_t)(hdr_end - raw.p);
                char *hdrs = xstrndup(raw.p, hdr_len);
                resp->status = 0;
                if (strncmp(hdrs, "HTTP/", 5) == 0) {
                    char *sp = strchr(hdrs, ' ');
                    if (sp) resp->status = atoi(sp + 1);
                }
                collect_cookies(hdrs, &resp->set_cookie);

                const char *bodyp = raw.p + hdr_len + 4;
                size_t bodylen = raw.len - hdr_len - 4;

                char te[64], cl[64];
                header_value(hdrs, "Transfer-Encoding", te, sizeof(te));
                header_value(hdrs, "Content-Length", cl, sizeof(cl));
                if (strstr(te, "chunked")) {
                    dechunk(bodyp, bodylen, &resp->body);
                } else if (cl[0]) {
                    long want = atol(cl);
                    if (want >= 0 && (size_t)want < bodylen) bodylen = (size_t)want;
                    buf_add(&resp->body, bodyp, bodylen);
                } else {
                    buf_add(&resp->body, bodyp, bodylen);
                }
                buf_cstr(&resp->body);
                free(hdrs);

                /* 跟随一次重定向 */
                if ((resp->status == 301 || resp->status == 302 || resp->status == 303 ||
                     resp->status == 307) && depth < 2) {
                    char loc[1024];
                    header_value(raw.p, "Location", loc, sizeof(loc));
                    if (loc[0]) {
                        Url nu;
                        if (loc[0] == '/') {
                            nu = *u;
                            snprintf(nu.path, sizeof(nu.path), "%s", loc);
                        } else if (parse_url(loc, &nu) != 0) {
                            nu = *u;
                        }
                        buf_reset(&resp->body);
                        buf_reset(&resp->set_cookie);
                        ret = do_request(&nu, "GET", extra_headers, NULL, resp, cancel, depth + 1);
                    }
                }
            }
        }
        buf_free(&raw);
    }

    if (use_tls) {
        mbedtls_ssl_close_notify(&tls.ssl);
        mbedtls_ssl_free(&tls.ssl);
        mbedtls_ssl_config_free(&tls.conf);
        mbedtls_ctr_drbg_free(&tls.drbg);
        mbedtls_entropy_free(&tls.entropy);
        mbedtls_x509_crt_free(&tls.cacert);
    }
    close(sock);
    return ret;
}
