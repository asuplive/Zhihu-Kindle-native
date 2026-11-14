/* ioctl_spy.c —— 抓 eips 到底怎么调 mxcfb 的（LD_PRELOAD 小工具）
 *
 * 为什么需要它：
 *   KO3 上本程序发的 MXCFB_SEND_UPDATE 被内核一路 EINVAL，但系统自带的 eips
 *   在同一台机器上刷得好好的。说明"内核支持刷新"这件事是真的，只是我们发的
 *   结构体/尺寸/取值和它对不上。与其继续猜，不如把 eips 自己拦下来看：
 *   它的 ioctl 号、结构体大小、每个字段的值，全都会被抓到日志里。
 *
 * 用法（Kindle 上，需要 root）：
 *   LD_PRELOAD=/mnt/us/zhihu/bin/spy.so eips -f
 *   cat /mnt/us/zhihu/eips_spy.log
 *
 * 实现上不链接 libdl、不用 dlsym：ARM 上直接 `svc 0` 发 ioctl，
 * 只依赖 libc 的 write()/open()，这样 Kindle 那套老 glibc 也能加载。
 * 万一加载失败，eips 照常运行，不影响任何东西。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__arm__)
#include <sys/syscall.h>
#endif

/* 统一成"返回 0 成功 / 失败时把 errno 填进 out_err"，
 * 这样 ARM 的裸 syscall 和开发机的 syscall() 两条路行为一致。 */
static long raw_ioctl(long fd, unsigned long req, void *arg, int *out_err) {
#if defined(__arm__)
    register long r0 __asm__("r0") = fd;
    register long r1 __asm__("r1") = (long)req;
    register long r2 __asm__("r2") = (long)arg;
    register long r7 __asm__("r7") = 54;   /* __NR_ioctl (ARM EABI) */
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    if (r0 < 0) { *out_err = (int)-r0; return 0; }
    *out_err = 0;
    return r0;
#else
    long r = syscall(SYS_ioctl, fd, req, arg);
    if (r < 0) { *out_err = errno; return 0; }
    *out_err = 0;
    return r;
#endif
}

/* 手写格式化：不引 snprintf，减少对 glibc 版本的依赖 */
static char *put_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *put_dec(char *p, long v) {
    /* 故意用"逐位减法"而不是 / % 10：ARM 上整数除法会生成 __aeabi_uidivmod
     * 调用，那是 libgcc 的符号，Kindle 的老 glibc 不一定给得到，
     * 一个未解析符号就会让整个 LD_PRELOAD 失败、连 eips 都起不来。 */
    static const unsigned long pow10[] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) *p++ = '-';
    int started = 0;
    for (int i = 0; i < 10; i++) {
        int d = 0;
        while (u >= pow10[i]) { u -= pow10[i]; d++; }
        if (d || started || i == 9) { *p++ = (char)('0' + d); started = 1; }
    }
    return p;
}

static char *put_hex(char *p, unsigned long v, int width) {
    static const char *d = "0123456789abcdef";
    for (int i = width - 1; i >= 0; i--) *p++ = d[(v >> (i * 4)) & 0xF];
    return p;
}

static const char *spy_log_path(void) {
    const char *p = getenv("ZH_SPY_LOG");
    return (p && *p) ? p : "/mnt/us/zhihu/eips_spy.log";
}

/* 记录一次 fb 类 ioctl。type 字节是 'F' 的才记（其余是 tty 之类，记了没用）。 */
static void spy_log(long fd, unsigned long req, const void *arg, long rc) {
    unsigned long type = (req >> 8) & 0xFF;
    unsigned long nr = req & 0xFF;
    unsigned long size = (req >> 16) & 0x3FFF;
    unsigned long dir = (req >> 30) & 0x3;

    char line[256];
    char *p = line;
    p = put_str(p, "ioctl fd=");
    p = put_dec(p, fd);
    p = put_str(p, " req=0x");
    p = put_hex(p, req, 8);
    p = put_str(p, " type=");
    if (type >= 32 && type < 127) { *p++ = (char)type; } else { p = put_str(p, "?"); }
    p = put_str(p, " nr=0x");
    p = put_hex(p, nr, 2);
    p = put_str(p, " size=");
    p = put_dec(p, (long)size);
    p = put_str(p, " dir=");
    p = put_dec(p, (long)dir);
    p = put_str(p, " rc=");
    p = put_dec(p, rc < 0 ? -rc : rc);
    p = put_str(p, rc < 0 ? " (err)" : " (ok)");
    if (arg && size) {
        const unsigned char *b = (const unsigned char *)arg;
        unsigned long n = size < 32 ? size : 32;
        p = put_str(p, " data=");
        for (unsigned long i = 0; i < n; i++) {
            p = put_hex(p, b[i], 2);
            *p++ = ' ';
        }
    }
    *p++ = '\n';

    int f = open(spy_log_path(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f < 0) return;
    ssize_t w = write(f, line, (size_t)(p - line));
    (void)w;
    close(f);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    int err = 0;
    long rc = raw_ioctl(fd, req, arg, &err);

    unsigned long type = (req >> 8) & 0xFF;
    if (type == 'F' && getenv("ZH_SPY_OFF") == NULL)
        spy_log(fd, req, arg, err ? -(long)err : rc);

    if (err) { errno = err; return -1; }
    return (int)rc;
}
