#include "common.h"

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ================= Buf ================= */

void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

void buf_free(Buf *b) { free(b->p); buf_init(b); }

void buf_reset(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_add(Buf *b, const void *data, size_t n) {
    if (n == 0) return;
    buf_grow(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_adds(Buf *b, const char *s) { if (s) buf_add(b, s, strlen(s)); }

void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }

void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        buf_add(b, tmp, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, (size_t)n);
    free(big);
}

const char *buf_cstr(Buf *b) {
    buf_grow(b, 0);
    if (!b->p) { b->p = xmalloc(1); b->p[0] = 0; b->cap = 1; }
    else b->p[b->len] = 0;
    return b->p;
}

char *buf_steal(Buf *b) {
    buf_cstr(b);
    char *p = b->p;
    if (!p) p = xstrdup("");
    buf_init(b);
    return p;
}

/* ================= 内存 ================= */

static void oom(void) {
    fputs("zhihu: out of memory\n", stderr);
    _exit(70);
}

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) oom(); return q; }

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ================= UTF-8 ================= */

int u8_seqlen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int u8_decode(const char *s, size_t n, uint32_t *cp) {
    uint32_t dummy;
    if (!cp) cp = &dummy;
    const unsigned char *u = (const unsigned char *)s;
    if (n == 0) { *cp = 0; return 0; }
    int len = u8_seqlen(u[0]);
    if (len > (int)n) { *cp = u[0]; return 1; }
    if (len == 1) { *cp = u[0]; return 1; }
    uint32_t v = u[0] & (0xFF >> (len + 1));
    for (int i = 1; i < len; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = u[0]; return 1; }
        v = (v << 6) | (u[i] & 0x3F);
    }
    *cp = v;
    return len;
}

int u8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t u8_strlen(const char *s) {
    size_t n = 0, i = 0, len = strlen(s);
    while (i < len) { i += (size_t)u8_decode(s + i, len - i, NULL); n++; }
    return n;
}

/* ================= 字符串 ================= */

int str_startswith(const char *s, const char *pre) {
    return s && pre && strncmp(s, pre, strlen(pre)) == 0;
}

int str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') n--;
        else break;
    }
    s[n] = 0;
    return s;
}

long str_indexof(const char *s, const char *needle) {
    if (!s || !needle) return -1;
    const char *p = strstr(s, needle);
    return p ? (long)(p - s) : -1;
}

/* ================= 路径 ================= */

int mkdir_p(const char *path) {
    char tmp[512];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

int read_file_into(const char *path, char *buf, size_t cap) {
    if (cap == 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    for (;;) {
        if (off + 1 >= cap) break;
        ssize_t n = read(fd, buf + off, cap - 1 - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = 0;
    return (int)off;
}

long sys_uptime_sec(void) {
    char buf[64];
    if (read_file_into("/proc/uptime", buf, sizeof(buf)) <= 0) return 0;
    return atol(buf);
}

int write_file_atomic(const char *path, const void *data, size_t n) {
    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); remove(tmp); return -1; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* ================= 退出请求 =================
 * 只用一个 sig_atomic_t：信号处理函数里不能 malloc、不能 fprintf，
 * 但可以在主循环里做这些。真正的清理（恢复 cvm / 释放 wakelock）
 * 由 main.c 的 atexit 与 start.sh 的 trap 完成。 */
static volatile sig_atomic_t g_quit_sig = 0;

void app_quit_request(int sig) { g_quit_sig = sig; }
int  app_quit_signal(void) { return (int)g_quit_sig; }

/* ================= 日志 ================= */

static char g_home[256] = "/mnt/us/zhihu";

const char *zh_home(void) { return g_home; }

void zh_set_home(const char *home) {
    if (!home) return;
    snprintf(g_home, sizeof(g_home), "%s", home);
}

void logmsg(const char *fmt, ...) {
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    static FILE *f = NULL;
    static int tried = 0;
    if (!f && !tried) {
        tried = 1;
        char path[320];
        snprintf(path, sizeof(path), "%s/log.txt", g_home);
        f = fopen(path, "a");
        if (f) {
            /* 超过 512KB 就轮转：把上一份留成 log.prev.txt。
             * 以前是直接清空——真出问题时，最关键的"出事前那几行"往往就
             * 这样被自己抹掉了。 */
            if (ftell(f) > 512 * 1024) {
                fclose(f);
                char prev[340];
                snprintf(prev, sizeof(prev), "%s.prev.txt", path);
                remove(prev);
                rename(path, prev);
                f = fopen(path, "w");
            }
        }
    }
    if (f) {
        fprintf(f, "%s\n", line);
        fflush(f);
    }
}

void logmsg_sync(const char *fmt, ...) {
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    char path[320];
    snprintf(path, sizeof(path), "%s/log.txt", g_home);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    size_t n = strlen(line);
    ssize_t w1 = write(fd, line, n);
    ssize_t w2 = write(fd, "\n", 1);
    (void)w1; (void)w2;
    fsync(fd);
    close(fd);
}
