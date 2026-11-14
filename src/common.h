/* common.h —— 公共基础：动态缓冲、UTF-8、日志、路径
 *
 * 这个程序是「独立原生 Kindle 应用」，不依赖 KOReader、不依赖 Python、
 * 不依赖任何运行时脚本，最终产物是一个静态链接的 ARM ELF，双击/启动即运行。
 */
#ifndef ZH_COMMON_H
#define ZH_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 动态字节/字符串缓冲 ---------- */
typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_reset(Buf *b);
void  buf_add(Buf *b, const void *data, size_t n);
void  buf_adds(Buf *b, const char *s);
void  buf_addc(Buf *b, char c);
void  buf_printf(Buf *b, const char *fmt, ...);
/* 保证结尾有 '\0'（不改变 len），返回 C 字符串 */
const char *buf_cstr(Buf *b);
/* 交出所有权，返回堆上的 NUL 结尾字符串 */
char *buf_steal(Buf *b);

/* ---------- 内存 ---------- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---------- UTF-8 ---------- */
int      u8_seqlen(unsigned char c);            /* 首字节 -> 序列长度，非法返回 1 */
int      u8_decode(const char *s, size_t n, uint32_t *cp);  /* 返回消耗字节数 */
int      u8_encode(uint32_t cp, char *out);     /* 返回写入字节数 */
size_t   u8_strlen(const char *s);

/* ---------- 字符串小工具 ---------- */
int   str_startswith(const char *s, const char *pre);
int   str_ieq(const char *a, const char *b);
char *str_trim(char *s);                        /* 原地去首尾空白，返回新起点 */
/* 在 s 里找 needle，返回偏移或 -1 */
long  str_indexof(const char *s, const char *needle);

/* ---------- 路径 ---------- */
int  mkdir_p(const char *path);
int  file_exists(const char *path);
/* 读取整个文件，返回堆字符串（调用者 free），失败返回 NULL */
char *read_file(const char *path, size_t *out_len);
/* 定长读取：/proc 下的伪文件 stat 出来大小是 0，read_file 的 fseek(SEEK_END)
 * 会得到 0 字节，于是心跳里的 /proc/self/statm 一直显示"内存=0KB"。
 * 这类文件必须老老实实读到 EOF。返回读到的字节数，失败返回 -1。 */
int  read_file_into(const char *path, char *buf, size_t cap);
int  write_file_atomic(const char *path, const void *data, size_t n);

/* /proc/uptime 的秒数（CLOCK_BOOTTIME 口径，**包含**挂起时间）。
 * 和 monotonic 对不上就说明设备睡过一次——用来判断"整机重启是不是挂起唤醒失败"。*/
long sys_uptime_sec(void);

/* ---------- 退出请求 ----------
 * 1.2.3 的 SIGTERM 处理是 `_exit(128+sig)`：进程被砍在绘图中间，
 * start.sh 的 trap 还没跑，cvm 就一直是冻着的状态，framework 起来后
 * 认为自家 UI 异常，弹出「应用程序错误」。
 * 1.2.4 改成：信号只置位，主循环看到后走正常退出路径，把系统界面还原。
 * 信号处理函数里可以用，所以只有 sig_atomic_t 的读写。 */
void app_quit_request(int sig);
int  app_quit_signal(void);     /* 0=没收到 */

/* ---------- 日志 ---------- */
void logmsg(const char *fmt, ...);

/* 与 logmsg 相同，但每次 write 后 fsync 再返回。
 * 用于"下一步可能整机 panic"的关键标记：普通 logmsg 只 fflush 到内核页缓存，
 * 内核挂掉时最后几行会丢，容易把崩点误判到前一行。 */
void logmsg_sync(const char *fmt, ...);
/* 运行根目录，例：/mnt/us/zhihu ；由 main 在启动时确定 */
const char *zh_home(void);
void        zh_set_home(const char *home);

/* ---------- 简易健壮性 ---------- */
#define ZH_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif /* ZH_COMMON_H */
