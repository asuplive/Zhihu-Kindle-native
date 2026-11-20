#include "fb.h"
#include "common.h"
#include "png.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 只用来量 ioctl 耗时：真机上"卡住"到底是驱动不返回还是我们自己在耗时，
 * 有这一行日志就能分清（1.2.3 只能靠猜）。 */
static long fb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
}

/* ---- i.MX EPDC 刷新（Kindle PW1 及以后） ---- */
struct mxcfb_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct mxcfb_alt_buffer_data {
    uint32_t phys_addr;
    uint32_t width;
    uint32_t height;
    struct mxcfb_rect alt_update_region;
};

/* 5.x 内核的真实布局是 72 字节：flags 之后还有 dither_mode / quant_bit。
 * 老机型（PW1/K4/K5）是 64 字节，没有这两个字段。
 * 下发时只填前 9 个 u32、其余全 0，所以两种布局都安全——
 * 真正的差别只在 ioctl 号的 size 位上，靠开机探测确认。 */
struct mxcfb_update_data {
    struct mxcfb_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    int      temp;
    uint32_t flags;
    int      dither_mode;
    int      quant_bit;
    struct mxcfb_alt_buffer_data alt_buffer_data;
};

/* 注意是 _IOWR 不是 _IOW。1.2.1 这里写成了 _IOW，方向位差一位，
 * ioctl 号就整个对不上，KO3 上必然是 ENOTTY。 */
#define MXCFB_SEND_UPDATE _IOWR('F', 0x2E, struct mxcfb_update_data)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE _IOWR('F', 0x2F, struct mxcfb_update_data)

#define WAVEFORM_MODE_DU   1
#define WAVEFORM_MODE_GC16 2
#define WAVEFORM_MODE_A2   4
#define WAVEFORM_MODE_GL16 5
/* Kindle 内核多出来的"残影补偿波形"（Regal 技术）：
 * PW2/ newer 上翻页不闪屏且驱动主动补偿残影。需要配 UPDATE_MODE_FULL。 */
#define WAVEFORM_MODE_REAGL 0x8

#define UPDATE_MODE_PARTIAL 0
#define UPDATE_MODE_FULL    1

/* mxcfb 的"按环境温度选波形"约定值。0 在某些驱动里是字面 0°C，
 * 波形会选得很慢甚至刷成黑的，所以探测时两档都试。 */
#define TEMP_USE_AMBIENT    0x1000

/* _IOC 编码：dir(2) | size(14) | type(8) | nr(8)
 * 麻烦有两处：
 *   1) 方向位。MXCFB_SEND_UPDATE 是 _IOWR（3），写成 _IOW（1）就永远 ENOTTY；
 *   2) size 参与编码。PW1 是 32 字节，老 5.x 是 64，新 5.x 是 72。
 * 所以两个方向 × 常见尺寸全试一遍，哪个返回 0 就用哪个。 */
#define ZH_IOC_WRITE 1u
#define ZH_IOC_READ  2u
#define ZH_IOC_RW    (ZH_IOC_WRITE | ZH_IOC_READ)

static unsigned int ioc_dir(unsigned int dir, unsigned int type,
                            unsigned int nr, unsigned int size) {
    return (dir << 30) | ((size & 0x3FFFu) << 16) | ((type & 0xFFu) << 8) | (nr & 0xFFu);
}

/* 首帧前后打标记：整机被内核 panic 重启时，用户态信号处理根本跑不到，
 * log.txt 会停在最后一行。这里单独开一个只追加的文件并 fsync，
 * 保证"最后一行"一定落盘，用来区分是「写内存时崩」还是「ioctl 触发 panic」。
 * 只在前几次 present 记，避免翻页时反复 fsync 拖慢。 */
static void fb_trace(Fb *fb, const char *fmt, ...) {
    if (fb->trace_n <= 0) {
        /* 1.2.10：MXCFB 分支不限量——内核 panic 捕不到、不留 *** CRASH，
         * 只能靠 panic 前最后一行定位是哪个刷新参数作的妖。
         * 每页一行小 append 对 flash 的磨损与 KOReader 相当，可接受。 */
        if (fb->eink_kind != 1) return;
        fb->trace_n = 1;
    }
    fb->trace_n--;
    char path[600];
    snprintf(path, sizeof(path), "%s/fb_trace.log", zh_home());
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char line[512];
    int n = snprintf(line, sizeof(line), "[%d] ", (int)fb->flip_count);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(line) - 1) {
        line[n++] = '\n';
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
    }
    fsync(fd);
    close(fd);
}

static void shadow_sync(Fb *fb);

unsigned int fb_send_update_cmd(int size) {
    return ioc_dir(ZH_IOC_RW, 'F', 0x2E, (unsigned)size);
}

/* 探测：把 SEND_UPDATE 的结构体尺寸、方向位、温度档、矩形朝向全试一遍。
 *
 * 1.2.2 在 KO3 上 7 种尺寸 × 2 方向 × 2 温度**全部 EINVAL(22)**，日志里只有一堆
 * 一模一样的 EINVAL，什么也说明不了。这次先量一条基线——发一个必然不存在的
 * 命令（'F' 0x7E），看它回什么：
 *   · 回 ENOTTY  → 说明内核按"命令不存在"处理未知 ioctl，那么真实命令的 EINVAL
 *                  就一定是"命令认得、参数被拒"，问题在结构体内容/尺寸；
 *   · 回 EINVAL  → 说明驱动把未知命令也回成 EINVAL，errno 无法区分两种情况，
 *                  单靠 errno 已经查不下去了，直接换 eips 后路（见下）。
 * 尺寸按 4 字节步进扫 4..160（覆盖 64/72/76/80/88 等所有见过的布局），
 * 同时试"正放"和"横竖对调"两种矩形——有的固件用 line_length 当 xres 校验。
 */
static unsigned int probe_send_update(Fb *fb, int *out_size, int *out_dir, int *out_temp) {
    static const int temps[] = { TEMP_USE_AMBIENT, 0, 25 };
    static const unsigned dirs[] = { ZH_IOC_RW, ZH_IOC_WRITE };
    int fd = fb->fd;
    int aw = fb->width & ~7;
    if (aw <= 0) aw = fb->width;

    {
        uint32_t t[8];
        memset(t, 0, sizeof(t));
        errno = 0;
        ioctl(fd, ioc_dir(ZH_IOC_RW, 'F', 0x7E, 32), t);
        fb->unknown_errno = errno;
    }
    logmsg("fb: 未知命令基线 errno=%d（本机 EINVAL=%d ENOTTY=%d）",
           fb->unknown_errno, EINVAL, ENOTTY);

    struct { int w, h; } reg[2];
    reg[0].w = aw;                  reg[0].h = fb->height;
    reg[1].w = fb->height & ~7;     reg[1].h = fb->width;
    if (reg[1].w <= 0) reg[1].w = fb->height & ~7;

    int tried = 0, other = 0;
    int he[4] = { 0, 0, 0, 0 }, hn[4] = { 0, 0, 0, 0 };

    for (int r = 0; r < 2; r++) {
        for (size_t d = 0; d < ZH_ARRAY_LEN(dirs); d++) {
            for (size_t t = 0; t < ZH_ARRAY_LEN(temps); t++) {
                for (int sz = 4; sz <= 160; sz += 4) {
                    uint32_t b[40];
                    memset(b, 0, sizeof(b));
                    b[0] = 0;                          /* top    */
                    b[1] = 0;                          /* left   */
                    b[2] = (uint32_t)reg[r].w;         /* width  */
                    b[3] = (uint32_t)reg[r].h;         /* height */
                    b[4] = WAVEFORM_MODE_GC16;
                    b[5] = UPDATE_MODE_FULL;           /* 探测用 FULL：比 PARTIAL 宽松 */
                    b[6] = 1;                          /* marker */
                    b[7] = (uint32_t)temps[t];         /* temp   */
                    unsigned int cmd = ioc_dir(dirs[d], 'F', 0x2E, (unsigned)sz);
                    errno = 0;
                    int rc = ioctl(fd, cmd, b);
                    int e = errno;
                    tried++;
                    if (rc == 0) {
                        if (out_size) *out_size = sz;
                        if (out_dir)  *out_dir = (int)dirs[d];
                        if (out_temp) *out_temp = temps[t];
                        logmsg("fb: EPDC 探测成功 size=%d dir=%u temp=%d 矩形=%dx%d ioc=%08x",
                               sz, dirs[d], temps[t], reg[r].w, reg[r].h, cmd);
                        return cmd;
                    }
                    /* errno 直方图：只留前 4 种，日志不能被刷爆 */
                    int slot = -1;
                    for (int i = 0; i < 4; i++) {
                        if (he[i] == e || hn[i] == 0) { slot = i; break; }
                    }
                    if (slot >= 0) { he[slot] = e; hn[slot]++; }
                    if (e != fb->unknown_errno) {
                        other++;
                        if (other <= 6)
                            logmsg("fb: probe 异常见 size=%d dir=%u temp=%d 矩形=%dx%d "
                                   "rc=%d errno=%d(%s)",
                                   sz, dirs[d], temps[t], reg[r].w, reg[r].h,
                                   rc, e, e ? strerror(e) : "ok");
                    }
                }
            }
        }
    }

    logmsg("fb: 探测结束 尝试=%d 成功=0 errno 分布 %d×%d %d×%d %d×%d %d×%d（异常 %d）",
           tried, he[0], hn[0], he[1], hn[1], he[2], hn[2], he[3], hn[3], other);
    if (fb->unknown_errno == EINVAL)
        logmsg("fb: 基线也是 EINVAL —— 内核把\"命令不存在\"和\"参数非法\"回成同一个数，"
               "errno 无法区分，改走 eips 后路");
    return 0;
}

/* ---------------- config.txt ---------------- */

/* refresh=auto|mxcfb|pan|none|eips|blank
 * eips=auto|image|flash|off
 * blankpulse=0|1
 * 只认这几行，其他键值一律不管（ui.c 会负责把它们原样写回）。 */
static void load_opts(Fb *fb) {
    char path[600];
    snprintf(path, sizeof(path), "%s/config.txt", zh_home());
    fb->refresh_mode = 0;
    fb->eips_mode = 0;
    fb->blankpulse = 0;
    fb->guard_align = 3;
    fb->use_du = 0;          /* 默认不用 DU（A2）：只翻像素不擦底，会叠字 */
    fb->eips_clean = 1;      /* 默认 eips 走 "-f -g" 全刷，换屏不留残影 */
    fb->eips_clean_fail = 0;
    fb->regal = 0;           /* 默认不走 REAGL：老驱动不认 0x8，别拿默认值冒险 */
    fb->autorot = 1;         /* 默认跟随系统硬件旋转（KO3 翻转后触摸同步） */
    fb->full_every = 6;      /* 1.2.15：第 4 档默认每 6 次翻页补一次全刷 */

    size_t n = 0;
    char *s = read_file(path, &n);
    if (!s) return;
    char *p = s;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *line = str_trim(p);
        if (str_startswith(line, "refresh=")) {
            const char *v = line + 8;
            if (str_ieq(v, "mxcfb")) fb->refresh_mode = 1;
            else if (str_ieq(v, "pan")) fb->refresh_mode = 2;
            else if (str_ieq(v, "none")) fb->refresh_mode = 3;
            else if (str_ieq(v, "eips")) fb->refresh_mode = 4;
            else if (str_ieq(v, "blank")) fb->refresh_mode = 5;
            else fb->refresh_mode = 0;
        } else if (str_startswith(line, "eips=")) {
            const char *v = line + 5;
            if (str_ieq(v, "image")) fb->eips_mode = 1;
            else if (str_ieq(v, "flash")) fb->eips_mode = 2;
            else if (str_ieq(v, "off")) fb->eips_mode = 3;
            else fb->eips_mode = 0;
        } else if (str_startswith(line, "blankpulse=")) {
            fb->blankpulse = atoi(line + 10) ? 1 : 0;
        } else if (str_startswith(line, "du=")) {
            /* du=1：翻页回到 A2/DU 快刷（快，但会留残影、字发虚） */
            fb->use_du = atoi(line + 3) ? 1 : 0;
        } else if (str_startswith(line, "eipsclean=")) {
            /* eipsclean=0：eips 只做局部更新（1.2.7 的老行为，会有残影） */
            fb->eips_clean = atoi(line + 10) ? 1 : 0;
        } else if (str_startswith(line, "regal=")) {
            /* regal=1：强制翻页走 REAGL（0x8）波形 */
            fb->regal = atoi(line + 6) ? 1 : 0;
        } else if (str_startswith(line, "fullevery=")) {
            /* 1.2.15：第 4 档的周期（每 N 次翻页补一次全刷），2..20 */
            int n = atoi(line + 10);
            if (n < 2 || n > 20) n = 6;
            fb->full_every = n;
        } else if (str_startswith(line, "autorot=")) {
            /* autorot=0：不做"系统旋转了触摸跟着转"的补偿 */
            fb->autorot = atoi(line + 8) ? 1 : 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(s);
}

/* ---------------- eips 后路 ---------------- */

static const char *eips_find(void) {
    static const char *cand[] = {
        "/usr/bin/eips", "/usr/sbin/eips", "/bin/eips", "/sbin/eips",
        "/usr/local/bin/eips",
    };
    for (size_t i = 0; i < ZH_ARRAY_LEN(cand); i++)
        if (access(cand[i], X_OK) == 0) return cand[i];
    return NULL;
}

/* 跑一个外部命令并等它结束；超过 timeout_ms 就杀掉，绝不把主循环卡死。
 * 返回子进程退出码，-1=fork 失败，-2=超时，-3=被信号杀，127=找不到程序。 */
static int run_cmd(char *const argv[], int timeout_ms) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    int waited = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) return -1;
        if (waited >= timeout_ms) {
            /* 杀完也不能无限等：eips 若卡在不可中断的驱动里，
             * 阻塞的 waitpid 会把整个界面锁死，看起来就是"死机"。 */
            kill(pid, SIGKILL);
            for (int i = 0; i < 25; i++) {
                if (waitpid(pid, &st, WNOHANG) == pid) break;
                usleep(20 * 1000);
            }
            return -2;
        }
        usleep(20 * 1000);
        waited += 20;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -3;
}

/* mode 1 = 把整页像素写成 PNG 交给 eips -g（像素级还原）
 * mode 2 = eips -f（让 eips 自己触发一次全屏刷新） */
/* eips 的刷新语义（MobileRead wiki / `eips -h`）：
 *   eips -g file        默认 **partial update**（局部更新），波形 gc16
 *   eips -f -g file     full update（整屏黑闪一次再画），波形 gc16
 *
 * 1.2.7 只用了 `-g`，于是每次画屏都是局部更新：电子墨水只在新旧像素有差异的
 * 地方动作，**不会把上一屏变黑的像素擦回白**。换一次界面就叠一层，连换几次
 * 就成了真机照片里那样——两屏的字互相压着，灰阶一路掉层（"发虚、不清楚"）。
 * 所以整屏换内容必须 `-f -g`；翻页可以走 `-g`，但每 4 次补一次全刷。 */
static int eips_shot(Fb *fb, int mode, int full) {
    if (!fb->eips_path[0]) return -1;
    if (mode == 1) {
        /* 必须放 /tmp（tmpfs）：每翻一页写 2MB 到 /mnt/us 会把闪存写坏 */
        char png[600];
        snprintf(png, sizeof(png), "/tmp/zhihu_frame.png");
        int rc;
        if (fb->bpp == 8) {
            rc = png_write_gray8(png, fb->width, fb->height,
                                 fb->mem + fb->base_off, fb->line_len);
        } else {
            uint8_t *g = xmalloc((size_t)fb->width * (size_t)fb->height);
            for (int y = 0; y < fb->height; y++) {
                for (int x = 0; x < fb->width; x++) {
                    int o = (int)(fb->base_off + (size_t)y * (size_t)fb->line_len +
                                  (size_t)x * (size_t)(fb->bpp / 8));
                    g[(size_t)y * (size_t)fb->width + (size_t)x] =
                        (fb->bpp == 16) ? fb->mem[o + 1] : fb->mem[o];
                }
            }
            rc = png_write_gray8(png, fb->width, fb->height, g, fb->width);
            free(g);
        }
        if (rc != 0) return -1;
        char *argv_full[]  = { fb->eips_path, "-f", "-g", png, NULL };
        char *argv_part[]  = { fb->eips_path, "-g", png, NULL };
        /* Regal（实验）：eips 的 -w 波形参数老固件只认 gc16/gl16/du，
         * 新固件（5.9+）按波形编号传值，REAGL=8。不认的话 eips 会报错，
         * present_eips 里有完整的回退链，不会卡屏。 */
        char *argv_regal[] = { fb->eips_path, "-f", "-g", "-w", "8", png, NULL };
        if (fb->regal && fb->fast_mode == 2) {
            int r8 = run_cmd(argv_regal, 8000);
            if (r8 == 0) {
                if (fb->eips_ok <= 2) logmsg("fb: eips Regal（-w 8）刷新成功");
                return 0;
            }
            if (fb->eips_fail <= 3)
                logmsg("fb: eips -w 8 失败 rc=%d，本机不认 Regal，退回 GC16", r8);
        }
        rc = run_cmd(full ? argv_full : argv_part, 8000);
        unlink(png);
        return rc;
    }
    char *argv[] = { fb->eips_path, "-f", NULL };
    return run_cmd(argv, 6000);
}

int fb_w(Fb *fb) { return (fb->ui_rotate & 1) ? fb->height : fb->width; }
int fb_h(Fb *fb) { return (fb->ui_rotate & 1) ? fb->width : fb->height; }

void fb_set_rotate(Fb *fb, int r) { fb->ui_rotate = ((r % 4) + 4) % 4; }

/* 开发机上的离屏模拟器：把 Kindle 屏幕渲染成 PPM 图片，便于检查排版 */
static void sim_snapshot(Fb *fb) {
    const char *out = getenv("ZH_SIM_OUT");
    if (!out) out = "/tmp/fb.ppm";
    const char *seq = getenv("ZH_SIM_SEQ");
    char path[512];
    static int counter = 0;
    counter++;
    if (seq) {
        char base[400];
        snprintf(base, sizeof(base), "%s", out);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        snprintf(path, sizeof(path), "%s-%03d.ppm", base, counter);
    } else {
        snprintf(path, sizeof(path), "%s", out);
    }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", fb->width, fb->height);
    fwrite(fb->mem, 1, (size_t)fb->width * (size_t)fb->height, f);
    fclose(f);

    /* 交互模拟：再写一个序号文件（序号 + 宽 + 高）。
     * 前端靠它判断"有没有新帧"，避免按 mtime 轮询时漏掉同一秒内的多次刷新。 */
    if (getenv("ZH_SIM_LIVE")) {
        char sp[560];
        snprintf(sp, sizeof(sp), "%s.seq", out);
        FILE *s = fopen(sp, "wb");
        if (s) {
            fprintf(s, "%d %d %d\n", counter, fb->width, fb->height);
            fclose(s);
        }
    }
}

int fb_open(Fb *fb, char *err, int errsz) {
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    if (getenv("ZH_SIM")) {
        int w = 758, h = 1024;
        const char *sw = getenv("ZH_SIM_W"), *sh = getenv("ZH_SIM_H");
        if (sw) w = atoi(sw);
        if (sh) h = atoi(sh);
        fb->width = w;
        fb->height = h;
        fb->bpp = 8;
        fb->line_len = w;
        fb->memlen = (size_t)w * (size_t)h;
        fb->mem = xcalloc(fb->memlen, 1);
        memset(fb->mem, 255, fb->memlen);
        fb->eink_kind = 9;   /* 模拟器 */
        return 0;
    }

    const char *devs[] = { "/dev/fb0", "/dev/graphics/fb0", "/dev/fb/0" };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int fd = open(devs[i], O_RDWR);
        if (fd >= 0) { fb->fd = fd; break; }
    }
    if (fb->fd < 0) {
        snprintf(err, errsz, "打不开 /dev/fb0（%s）。需要 root，且机型需有 fb 设备",
                 strerror(errno));
        return -1;
    }

    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
        ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) != 0) {
        snprintf(err, errsz, "读取屏幕参数失败：%s", strerror(errno));
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    fb->width = (int)var.xres;
    fb->height = (int)var.yres;
    fb->bpp = (int)var.bits_per_pixel;
    fb->line_len = (int)fix.line_length;
    fb->rotate = (int)var.rotate;
    fb->hw_rotate = (int)(var.rotate & 3);
    fb->ui_rotate = 0;

    if (fb->bpp != 8 && fb->bpp != 16 && fb->bpp != 32) {
        snprintf(err, errsz, "不支持的色深：%d bpp", fb->bpp);
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }
    /* 有些机型 line_length 为 0，按 xres 推算 */
    if (fb->line_len <= 0) fb->line_len = fb->width * fb->bpp / 8;

    /* 有的驱动开了双缓冲，smem_len 比一屏大；按 smem_len 映射最保险 */
    fb->memlen = (size_t)fb->line_len * (size_t)fb->height;
    if ((size_t)fix.smem_len > fb->memlen && (size_t)fix.smem_len < 64u * 1024u * 1024u)
        fb->memlen = (size_t)fix.smem_len;

    fb->mem = mmap(NULL, fb->memlen, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        snprintf(err, errsz, "mmap 帧缓冲失败：%s", strerror(errno));
        close(fb->fd);
        fb->fd = -1;
        fb->mem = NULL;
        return -1;
    }

    /* 双缓冲虚拟屏（KO3：yres_virtual = 2*yres）。var.yoffset 指向当前
     * 正在显示的那一页，绘制必须从那一页开始写，否则画进看不见的那半，
     * 屏幕永远不动——表现就是"程序在跑，但屏幕是白的"。 */
    fb->yres_virtual = (int)var.yres_virtual;
    fb->base_off = 0;
    if (var.yres_virtual > var.yres && var.yoffset < var.yres_virtual) {
        size_t off = (size_t)var.yoffset * (size_t)fix.line_length;
        if (off + (size_t)fb->line_len * (size_t)fb->height <= fb->memlen)
            fb->base_off = off;
    }

    /* 探测会做一次真正的全屏刷新，先把可见页填白，免得刷出一屏黑 */
    memset(fb->mem + fb->base_off, 255, (size_t)fb->line_len * (size_t)fb->height);

    /* 上一次的 trace 必须留着：整机重启后程序会自动再启动一次，
     * 如果这里直接 truncate，上次崩在哪就永远看不到了。
     * 所以轮转成 fb_trace.prev.log，重启后先看它。 */
    {
        char tp[600], op[600];
        snprintf(tp, sizeof(tp), "%s/fb_trace.log", zh_home());
        snprintf(op, sizeof(op), "%s/fb_trace.prev.log", zh_home());
        rename(tp, op);
    }
    fb->trace_n = 8;

    load_opts(fb);
    fb->update_size = 0;
    fb->update_dir = 0;
    fb->update_temp = TEMP_USE_AMBIENT;
    fb->send_update_ioctl = 0;
    fb->unknown_errno = 0;
    fb->eips_path[0] = 0;
    fb->eips_ok = 0;
    fb->eips_fail = 0;
    fb->eink_kind = 3;

    /* eips 是系统自带的（/usr/bin/eips），不占空间也不依赖别的库。
     * 只有用户显式写 refresh=none / eips=off 时才不找它。 */
    if (fb->refresh_mode != 3 && fb->eips_mode != 3) {
        const char *ep = eips_find();
        if (ep) snprintf(fb->eips_path, sizeof(fb->eips_path), "%s", ep);
    }

    switch (fb->refresh_mode) {
        case 2:
            /* 用户明确要求走老路径，不探测（探测本身也会刷屏） */
            fb->eink_kind = 2;
            ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
            logmsg("fb: refresh=pan（用户强制），EPDC 机型上可能整机重启");
            break;
        case 3:
            fb->eink_kind = 3;
            logmsg("fb: refresh=none（用户强制），只写内存不刷新");
            break;
        case 4:
            fb->eink_kind = fb->eips_path[0] ? 4 : 3;
            ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
            logmsg("fb: refresh=eips（用户强制）path=%s",
                   fb->eips_path[0] ? fb->eips_path : "(没找到 eips)");
            break;
        case 5:
            fb->eink_kind = 3;
            fb->blankpulse = 1;
            ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
            logmsg("fb: refresh=blank（用户强制），只写内存 + FBIOBLANK 脉冲");
            break;
        case 1:
        default: {
            fb->send_update_ioctl = probe_send_update(fb, &fb->update_size,
                                                      &fb->update_dir, &fb->update_temp);
            if (fb->send_update_ioctl) {
                fb->eink_kind = 1;
                fb->marker = 2;
            } else {
                /* 顺便把屏幕从省电模式唤醒，否则有些机型写进去也不显示 */
                ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
                if (fb->eips_path[0]) {
                    /* KO3 上就是这条路：SEND_UPDATE 全 EINVAL，交给 eips 刷。
                     * 注意顺序——先 eips，再考虑"只写内存"，最后才是 pan：
                     * EPDC 上 FBIOPAN_DISPLAY 会内核 panic（1.2.1 的整机重启）。 */
                    fb->eink_kind = 4;
                    logmsg("fb: 探测失败，改走系统 eips 刷新（%s）", fb->eips_path);
                } else if (fb->yres_virtual >= fb->height * 2) {
                    fb->eink_kind = 3;
                    logmsg("fb: 探测失败、无 eips、且是双缓冲屏 vy=%d，"
                           "EPDC 上不用 FBIOPAN，降级为只写内存", fb->yres_virtual);
                } else {
                    fb->eink_kind = 2;
                }
                if (fb->refresh_mode == 1)
                    logmsg("fb: refresh=mxcfb 但探测失败，已降级 kind=%d", fb->eink_kind);
            }
            break;
        }
    }

    logmsg("fb: %dx%d %dbpp line=%d smem=%u vx=%u vy=%u yoff=%u rot=%d "
           "eink=%d ioc=%08x/%d dir=%d temp=%d base=%u refresh=%d eips=%s mode=%d "
           "blankpulse=%d unk=%d",
           fb->width, fb->height, fb->bpp, fb->line_len, fix.smem_len,
           var.xres_virtual, var.yres_virtual, var.yoffset, fb->rotate,
           fb->eink_kind, fb->send_update_ioctl, fb->update_size, fb->update_dir,
           fb->update_temp, (unsigned)fb->base_off, fb->refresh_mode,
           fb->eips_path[0] ? fb->eips_path : "-", fb->eips_mode,
           fb->blankpulse, fb->unknown_errno);
    return 0;
}

void fb_close(Fb *fb) {
    if (fb->mem && fb->mem != MAP_FAILED) munmap(fb->mem, fb->memlen);
    if (fb->fd >= 0) close(fb->fd);
    free(fb->shadow);
    fb->shadow = NULL;
    fb->shadow_len = 0;
    fb->mem = NULL;
    fb->fd = -1;
}

/* 逻辑坐标 -> 内存偏移。ui_rotate 是给"屏幕装反了/横竖颠倒"的机型用的 */
static inline size_t pix_off(Fb *fb, int x, int y) {
    int px = x, py = y;
    int W = fb->width, H = fb->height;
    switch (fb->ui_rotate) {
        case 1: /* 顺时针 90°：逻辑 (x,y) -> 物理 (y, H-1-x) */
            px = y;
            py = H - 1 - x;
            break;
        case 2:
            px = W - 1 - x;
            py = H - 1 - y;
            break;
        case 3:
            px = W - 1 - y;
            py = x;
            break;
        default:
            break;
    }
    return fb->base_off + (size_t)py * (size_t)fb->line_len +
           (size_t)px * (size_t)(fb->bpp / 8);
}

static inline void put_px(Fb *fb, int x, int y, int gray) {
    if (x < 0 || y < 0 || x >= fb_w(fb) || y >= fb_h(fb)) return;
    size_t o = pix_off(fb, x, y);
    if (fb->bpp == 8) {
        fb->mem[o] = (uint8_t)gray;
    } else if (fb->bpp == 16) {
        uint16_t v = (uint16_t)(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
        memcpy(fb->mem + o, &v, 2);
    } else {
        uint32_t v = ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
        memcpy(fb->mem + o, &v, 4);
    }
}

void fb_clear(Fb *fb, int gray) {
    int W = fb_w(fb), H = fb_h(fb);
    fb_fill(fb, 0, 0, W, H, gray);
}

void fb_fill(Fb *fb, int x, int y, int w, int h, int gray) {
    int W = fb_w(fb), H = fb_h(fb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    if (fb->bpp == 8 && fb->ui_rotate == 0) {
        for (int j = 0; j < h; j++) memset(fb->mem + pix_off(fb, x, y + j), (uint8_t)gray, (size_t)w);
        return;
    }
    if (fb->ui_rotate == 0 && (fb->bpp == 16 || fb->bpp == 32)) {
        /* 16/32bpp 的机型（少数）逐像素画会明显拖慢整页重绘，这里按行压着写 */
        if (fb->bpp == 16) {
            uint16_t v16 = (uint16_t)(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
            uint32_t v32 = (uint32_t)v16 | ((uint32_t)v16 << 16);
            for (int j = 0; j < h; j++) {
                uint8_t *p = fb->mem + pix_off(fb, x, y + j);
                int i = 0;
                for (; i + 1 < w; i += 2) memcpy(p + (size_t)i * 2, &v32, 4);
                if (i < w) memcpy(p + (size_t)i * 2, &v16, 2);
            }
        } else {
            uint32_t v32 = ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
            for (int j = 0; j < h; j++) {
                uint8_t *p = fb->mem + pix_off(fb, x, y + j);
                for (int i = 0; i < w; i++) memcpy(p + (size_t)i * 4, &v32, 4);
            }
        }
        return;
    }
    if (fb->bpp == 8) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) put_px(fb, x + i, y + j, gray);
        return;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put_px(fb, x + i, y + j, gray);
}

void fb_hline(Fb *fb, int x, int y, int w, int gray) { fb_fill(fb, x, y, w, 1, gray); }
void fb_vline(Fb *fb, int x, int y, int h, int gray) { fb_fill(fb, x, y, 1, h, gray); }

void fb_rect(Fb *fb, int x, int y, int w, int h, int gray) {
    fb_hline(fb, x, y, w, gray);
    fb_hline(fb, x, y + h - 1, w, gray);
    fb_vline(fb, x, y, h, gray);
    fb_vline(fb, x + w - 1, y, h, gray);
}

void fb_invert(Fb *fb, int x, int y, int w, int h) {
    /* 只支持 8bpp 的快速反色；其他色深逐像素读改写 */
    int W = fb_w(fb), H = fb_h(fb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            size_t o = pix_off(fb, x + i, y + j);
            if (fb->bpp == 8) fb->mem[o] = (uint8_t)(255 - fb->mem[o]);
            else if (fb->bpp == 16) {
                uint16_t v;
                memcpy(&v, fb->mem + o, 2);
                int g = (int)(((v >> 11) & 0x1F) * 255 / 31);
                int ng = 255 - g;
                v = (uint16_t)(((ng >> 3) << 11) | ((ng >> 2) << 5) | (ng >> 3));
                memcpy(fb->mem + o, &v, 2);
            } else {
                uint32_t v;
                memcpy(&v, fb->mem + o, 4);
                int g = (int)(v & 0xFF);
                int ng = 255 - g;
                v = ((uint32_t)ng << 16) | ((uint32_t)ng << 8) | (uint32_t)ng;
                memcpy(fb->mem + o, &v, 4);
            }
        }
    }
}

void fb_blit_gray(Fb *fb, int x, int y, int w, int h, const uint8_t *alpha, int gray) {
    int W = fb_w(fb), H = fb_h(fb);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= W) continue;
            int a = alpha[j * w + i];
            if (a == 0) continue;
            if (a == 255) { put_px(fb, px, py, gray); continue; }
            size_t o = pix_off(fb, px, py);
            if (fb->bpp == 8) {
                int d = fb->mem[o];
                fb->mem[o] = (uint8_t)((d * (255 - a) + gray * a) / 255);
            } else if (fb->bpp == 16) {
                uint16_t v;
                memcpy(&v, fb->mem + o, 2);
                int d = (int)(((v >> 11) & 0x1F) * 255 / 31);
                int nd = (d * (255 - a) + gray * a) / 255;
                v = (uint16_t)(((nd >> 3) << 11) | ((nd >> 2) << 5) | (nd >> 3));
                memcpy(fb->mem + o, &v, 2);
            } else {
                uint32_t v;
                memcpy(&v, fb->mem + o, 4);
                int d = (int)(v & 0xFF);
                int nd = (d * (255 - a) + gray * a) / 255;
                v = ((uint32_t)nd << 16) | ((uint32_t)nd << 8) | (uint32_t)nd;
                memcpy(fb->mem + o, &v, 4);
            }
        }
    }
}

/* 把一张 8bit 灰度图按最近邻缩放到 (x,y,w,h) 区域。
 * 用来贴正文里的图片：解码时已经把宽度压到正文宽附近，这里只做最后一跳缩放。
 * 每像素走 put_px，所以四个旋转方向、8/16/32bpp 都自动正确。 */
void fb_blit_gray_scaled(Fb *fb, int x, int y, int w, int h,
                         const uint8_t *src, int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    int W = fb_w(fb), H = fb_h(fb);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) continue;
        int sy = (int)((long long)j * sh / h);
        if (sy >= sh) sy = sh - 1;
        const uint8_t *row = src + (size_t)sy * (size_t)sw;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= W) continue;
            int sx = (int)((long long)i * sw / w);
            if (sx >= sw) sx = sw - 1;
            put_px(fb, px, py, row[sx]);
        }
    }
}

/* eips 分支：整页像素交给系统自带的 eips。
 * 先试 eips -g（PNG，像素级还原，能不能用看退出码），失败再试 eips -f。 */
static void present_eips(Fb *fb, int full) {
    int rc;
    /* 该不该全刷：整屏换内容（full）一律全刷；局部更新每 3 次也补一次全刷，
     * 免得残影攒起来。eipsclean=0 可退回 1.2.7 的老行为（全局部，快但叠字）。 */
    int want_full = full;
    if (!want_full && fb->eips_clean && (fb->flip_count % 3 == 0)) want_full = 1;

    if (fb->eips_mode != 2) {
        rc = eips_shot(fb, 1, want_full);
        if (rc != 0 && want_full) {
            /* 有的固件不认 "-f -g" 的组合：退回纯局部更新，至少画面是新的 */
            fb->eips_fail++;
            if (fb->eips_fail <= 3)
                logmsg("fb: eips -f -g 失败 rc=%d，退回局部刷新", rc);
            rc = eips_shot(fb, 1, 0);
        }
        if (rc == 0) {
            fb->eips_ok++;
            if (want_full && fb->eips_ok <= 2)
                logmsg("fb: eips 全刷（-f -g）%s", full ? "换屏" : "定期清残影");
        } else {
            fb->eips_fail++;
            /* 前几次把原因写清楚：rc=127 是没找到程序，-2 是超时（驱动卡住），
             * 其余是 eips 自己报错（比如不认 PNG） */
            if (fb->eips_fail <= 3)
                logmsg("fb: eips -g 失败 rc=%d（第 %d 次），改试 eips -f",
                       rc, fb->eips_fail);
            if (fb->eips_mode == 0 && fb->eips_fail >= 2) fb->eips_mode = 2;
            rc = eips_shot(fb, 2, 1);
            if (rc == 0) fb->eips_ok++;
            else fb->eips_fail++;
        }
    } else {
        rc = eips_shot(fb, 2, 1);
        if (rc == 0) fb->eips_ok++;
        else fb->eips_fail++;
    }

    if (fb->eips_ok == 1)
        logmsg_sync("fb: eips 刷新成功 首帧 mode=%d", fb->eips_mode);
    if (rc != 0 && fb->eips_fail >= 6) {
        /* eips 两条路都不行时，退回"只写内存 + FBIOBLANK 脉冲"：
         * 1.2.2 的日志证明 unblank 确实会把 fb 里的像素重新刷上屏（首帧就是这么出来的），
         * 慢、会闪，但至少屏幕不是死的。 */
        logmsg("fb: eips 连续失败 %d 次，改用 FBIOBLANK 脉冲刷屏（会闪，但比冻屏强）",
               fb->eips_fail);
        fb->eink_kind = 3;
        fb->blankpulse = 1;
        ioctl(fb->fd, FBIOBLANK, FB_BLANK_NORMAL);
        ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
    }
    fb_trace(fb, "eips rc=%d ok=%d fail=%d mode=%d full=%d",
             rc, fb->eips_ok, fb->eips_fail, fb->eips_mode, want_full);
}

/* 1.3.4：波形决策抽成纯函数，fb_present 与自检共用，杜绝"自检过、真机没生效"。
 *
 * 修复的核心：Regal(fast_mode==2 / regal=1) 必须覆盖**所有**刷新，包括
 * 整屏换内容 full=1。1.3.3 及以前的顺序是：
 *     if (full || !fast_mode) wave = GC16;        ← full=1 先命中，黑闪
 *     else if (fast_mode==2 || regal) wave = REAGL; ← 永远轮不到
 * 所以阅读翻页(full=0)能用 Regal 不闪，而主界面换屏/加载/消息框(full=1)
 * 一律 GC16 全刷黑闪——这就是"设了 Regal 主界面还是闪"的根因。
 * 现在把 Regal 提到最前：REAGL + UPDATE_MODE_FULL 在 Kindle 上是标准的
 * "整屏更新但不闪白"波形（KOReader 在 PW2 的用法），既是全刷会做残影补偿、
 * 不会叠屏，又不产生 GC16 那种黑白闪。
 *
 * 参数 full 语义保留：非 Regal 时 full=1 仍是 GC16 全刷、full=0 走局部/第4档。 */
unsigned int fb_wave_for(int fast_mode, int regal, int full, int flip_count,
                         int full_every, int use_du, int *out_mode) {
    /* 1. Regal 最高优先级：翻页、换屏都不闪（REAGL 自带残影补偿） */
    if (fast_mode == 2 || regal) {
        if (out_mode) *out_mode = UPDATE_MODE_FULL;
        return WAVEFORM_MODE_REAGL;
    }
    /* 2. 非 Regal 的整屏更新：清晰档(0)或明确换屏 → GC16 全刷 */
    if (full || !fast_mode) {
        if (out_mode) *out_mode = UPDATE_MODE_FULL;
        return WAVEFORM_MODE_GC16;
    }
    /* 3. 第4档：局部 N 次 + 定期一次 GC16 全刷 */
    if (fast_mode == 3 && full_every > 1 && (flip_count % full_every) == 0) {
        if (out_mode) *out_mode = UPDATE_MODE_FULL;
        return WAVEFORM_MODE_GC16;
    }
    /* 4. 手写的 DU 档：每 8 次补一次 GC16 */
    if (use_du) {
        if (out_mode) *out_mode = UPDATE_MODE_PARTIAL;
        return (flip_count % 8 == 0) ? WAVEFORM_MODE_GC16 : WAVEFORM_MODE_DU;
    }
    /* 5. 默认（局部 GC16，不闪） */
    if (out_mode) *out_mode = UPDATE_MODE_PARTIAL;
    return WAVEFORM_MODE_GC16;
}

/* 自检（无需真机）：把波形规则钉死，防止再出现"设了 Regal 换屏却黑闪"。 */
int fb_refresh_wave_test(void) {
    int m = -1;
    int ok = 1;
    /* Regal + 整屏换内容：必须 REAGL+FULL，绝不能 GC16（黑闪的根） */
    if (fb_wave_for(2, 0, 1, 1, 6, 0, &m) != WAVEFORM_MODE_REAGL) ok = 0;
    if (m != UPDATE_MODE_FULL) ok = 0;
    /* regal=1 强制：即使 fast_mode=1，整屏也走 REAGL */
    if (fb_wave_for(1, 1, 1, 5, 6, 0, &m) != WAVEFORM_MODE_REAGL) ok = 0;
    /* Regal + 翻页(full=0)：同样 REAGL */
    if (fb_wave_for(2, 0, 0, 2, 6, 0, &m) != WAVEFORM_MODE_REAGL) ok = 0;
    /* 非 Regal 清晰档(0) + 换屏：仍 GC16+FULL（用户要"每步闪"就给闪） */
    if (fb_wave_for(0, 0, 1, 1, 6, 0, &m) != WAVEFORM_MODE_GC16) ok = 0;
    if (m != UPDATE_MODE_FULL) ok = 0;
    /* 非 Regal 局部档(1) + 换屏：GC16+FULL（按 1.3.3 两分法，换屏仍全刷） */
    if (fb_wave_for(1, 0, 1, 1, 6, 0, &m) != WAVEFORM_MODE_GC16) ok = 0;
    /* 非 Regal 局部档(1) + 翻页：GC16+PARTIAL（不闪） */
    if (fb_wave_for(1, 0, 0, 1, 6, 0, &m) != WAVEFORM_MODE_GC16) ok = 0;
    if (m != UPDATE_MODE_PARTIAL) ok = 0;
    /* 第4档：非整屏周期点 = PARTIAL；整屏周期点 = FULL */
    if (fb_wave_for(3, 0, 0, 5, 6, 0, &m) != WAVEFORM_MODE_GC16) ok = 0;
    if (m != UPDATE_MODE_PARTIAL) ok = 0;
    if (fb_wave_for(3, 0, 0, 6, 6, 0, &m) != WAVEFORM_MODE_GC16) ok = 0;
    if (m != UPDATE_MODE_FULL) ok = 0;
    return ok;
}

void fb_present(Fb *fb, int full) {
    if (fb->eink_kind == 9) {
        sim_snapshot(fb);
        return;
    }
    msync(fb->mem, fb->memlen, MS_ASYNC);

    fb->flip_count++;
    fb_trace(fb, "present kind=%d full=%d", fb->eink_kind, full);

    if (fb->eink_kind == 1) {
        uint32_t b[32];
        memset(b, 0, sizeof(b));
        /* 驱动要求宽度 8 像素对齐，否则整个 update 被拒（屏幕就不动了） */
        int aw = fb->width & ~7;
        if (aw <= 0) aw = fb->width;
        b[0] = 0;                             /* top */
        b[1] = 0;                             /* left */
        b[2] = (uint32_t)aw;                  /* width */
        b[3] = (uint32_t)fb->height;          /* height */
        int out_mode = UPDATE_MODE_PARTIAL;
        /* 1.3.4：波形统一走纯函数（自检共用）。Regal 现在覆盖 full=1，
         * 主界面换屏/加载/消息框也不再 GC16 黑闪。 */
        unsigned wave = fb_wave_for(fb->fast_mode, fb->regal, full,
                                    fb->flip_count, fb->full_every, fb->use_du,
                                    &out_mode);
        b[5] = (uint32_t)out_mode;
        b[4] = wave;
        /* 第4档定期全刷的日志（频率低，不刷屏） */
        if (fb->fast_mode == 3 && !(fb->regal) && !full && fb->full_every > 1 &&
            (fb->flip_count % fb->full_every == 0) &&
            (fb->flip_count % (fb->full_every * 20) == fb->full_every))
            logmsg("fb: 第4档 定期全刷（每 %d 次翻页）", fb->full_every);
        b[6] = fb->marker++;                  /* update_marker */
        b[7] = (uint32_t)fb->update_temp;     /* temp：探测时确认过的那档 */

        /* 不阻塞等待完成：等的话一旦驱动不返回，整机就卡在这儿，
         * 看起来跟"闪退/死机"一模一样。异步提交由驱动排队即可。
         * 1.2.10：发送前先写 breadcrumb（即将发送的波形/模式/区域）——
         * 内核 panic 不经过用户态信号处理，log.txt 会戛然而止，
         * fb_trace.log 的最后一行就是嫌疑参数。 */
        long t0 = fb_now_ms();
        fb_trace(fb, "即将 SEND_UPDATE wave=%u mode=%u 区域=%ux%d marker=%u",
                 wave, b[5], (unsigned)aw, (unsigned)fb->height, fb->marker);
        errno = 0;
        int r = ioctl(fb->fd, fb->send_update_ioctl, b);
        int e = errno;
        long spent = fb_now_ms() - t0;
        if (spent > 400)
            logmsg("fb: SEND_UPDATE 耗时 %ldms（full=%d wave=%u）——驱动排队了",
                   spent, full, wave);

        /* 1.3.4：Regal 现在也用于 full=1（换屏）。整屏的 REAGL 若被固件拒绝，
         * 同样要补一帧 GC16/FULL 并把 Regal 关掉，否则换屏会一直失败。
         * 条件放宽为：快刷(full=0)失败，或本次用的是 Regal（不管 full）。 */
        int is_regal = (fb->fast_mode == 2 || fb->regal);
        if (r != 0 && fb->fast_mode && (!full || is_regal)) {
            /* 有的固件只认 GC16 + FULL，DU/REAGL/PARTIAL 直接 EINVAL。
             * 先补一帧全刷：能把画面推上去就先推上去。
             * Regal 被拒就永久关掉（fast_mode 退回 1=局部 GC16），并写日志。
             * 1.2.15：第 4 档（局部+定期全刷）不关——它平时用的就是最基本的
             * GC16/PARTIAL，被拒时本次已用全刷兜底，下翻页接着试局部。 */
            int was_regal = is_regal;
            int was_4 = (fb->fast_mode == 3);
            b[4] = WAVEFORM_MODE_GC16;
            b[5] = UPDATE_MODE_FULL;
            errno = 0;
            int r2 = ioctl(fb->fd, fb->send_update_ioctl, b);
            if (r2 == 0) {
                if (was_regal) {
                    logmsg("fb: Regal(REAGL) 被拒 errno=%d，改 GC16/FULL 成功，本次起关闭 Regal", e);
                    fb->fast_mode = 1;
                    fb->regal = 0;   /* config 里 regal=1 强制位也要清，否则下次又开 */
                } else if (was_4) {
                    logmsg("fb: 第4档局部刷新被拒 errno=%d，本次以全刷兜底（模式保留）", e);
                } else {
                    logmsg("fb: 快刷被拒 errno=%d，改 GC16/FULL 成功，本次起关闭 fast_mode", e);
                    fb->fast_mode = 0;
                }
                r = 0;
            } else {
                logmsg("fb: 快刷与全刷都失败 errno=%d/%d", e, errno);
            }
        }

        fb_trace(fb, "mxcfb rc=%d errno=%d ioc=%08x size=%d %ldms",
                 r, e, fb->send_update_ioctl, fb->update_size, spent);
        if (r != 0) {
            logmsg("fb: SEND_UPDATE 失败 %s（ioc=%08x size=%d dir=%d）",
                   strerror(e), fb->send_update_ioctl, fb->update_size, fb->update_dir);
            /* 绝不退 FBIOPAN_DISPLAY：KO3 上那是整机重启的扳机。
             * 有 eips 就交给它，没有才退"只写内存"。 */
            if (fb->eips_path[0] && fb->eips_mode != 3) {
                fb->eink_kind = 4;
                logmsg("fb: 改用 eips 刷新（%s）", fb->eips_path);
            } else {
                fb->eink_kind = 3;
            }
        }
    }

    if (fb->eink_kind == 4) {
        present_eips(fb, full);
        return;
    }

    if (fb->eink_kind == 2) {
        /* 老驱动（K4/K5）：写内存本身就会触发刷新；
         * 再补一次 FBIOPAN_DISPLAY，部分固件靠它才会真正刷屏。
         * ⚠ 编译期警示：这一支绝不允许在 KO3/KO3-类 i.MX6 EPDC 机器上
         * 被走到——FBIOPAN_DISPLAY 在双缓冲屏上会内核 panic 整机重启
         * （1.2.2 事故）。探测层（eink_kind 的取值）是唯一的闸门。 */
#warning "FBIOPAN_DISPLAY branch: KO3-class devices kernel-panic here. eink_kind==2 is K4/K5 only — keep the probe gate intact."
        struct fb_var_screeninfo var;
        if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) == 0) {
            var.activate = FB_ACTIVATE_NOW;
            var.xoffset = 0;
            var.yoffset = 0;
            /* yoffset 必须落在 [0, yres_virtual-yres]，越界会被驱动拒绝 */
            int maxy = (int)var.yres_virtual - (int)var.yres;
            if (maxy < 0) maxy = 0;
            if ((int)var.yoffset > maxy) var.yoffset = (uint32_t)maxy;
            errno = 0;
            int r = ioctl(fb->fd, FBIOPAN_DISPLAY, &var);
            fb_trace(fb, "pan rc=%d errno=%d yoff=%u vy=%u",
                     r, errno, var.yoffset, var.yres_virtual);
        }
    }

    if (fb->eink_kind == 3) {
        /* 最后的兜底：只写内存不刷新。1.2.2 里这里对 full 帧还会打一次
         * FBIOBLANK 脉冲——1.2.2 的日志显示那一下确实能把首帧推上屏，
         * 但它同时会闪一下、也可能干扰驱动状态机，所以改成显式开关：
         * config.txt 写 blankpulse=1 才用。 */
        if (full && fb->blankpulse) {
            ioctl(fb->fd, FBIOBLANK, FB_BLANK_NORMAL);
            ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
        }
        fb_trace(fb, "memory-only（不碰 SEND_UPDATE / FBIOPAN）");
    }

    shadow_sync(fb);   /* 记下"我们想让屏幕长什么样" */
}

/* 每次刷屏后把可见页抄进影子副本。开销约 2MB memcpy（毫秒级），
 * 换来的是"被 cvm 盖掉时能瞬间回灌"，很划算。 */
static void shadow_sync(Fb *fb) {
    if (!fb->shadow || fb->shadow_len == 0) return;
    if (!fb->mem) return;
    const uint8_t *v = fb->mem + fb->base_off;
    size_t n = fb->shadow_len;
    if (fb->base_off + n > fb->memlen) n = fb->memlen > fb->base_off ? fb->memlen - fb->base_off : 0;
    if (n) memcpy(fb->shadow, v, n);
}

int fb_shadow_enable(Fb *fb) {
    if (fb->shadow) return 0;
    if (!fb->mem || fb->line_len <= 0 || fb->height <= 0) return -1;
    size_t n = (size_t)fb->line_len * (size_t)fb->height;
    if (n == 0 || n > 32u * 1024u * 1024u) return -1;
    fb->shadow = (uint8_t *)malloc(n);
    if (!fb->shadow) {
        logmsg("fb: 影子副本分配失败（%zuKB），退回\"被改就重画\"", n / 1024);
        return -1;
    }
    fb->shadow_len = n;
    memset(fb->shadow, 255, n);
    logmsg("fb: 影子副本 %zuKB 已启用（cvm 抢屏时可整页回灌）", n / 1024);
    return 0;
}

int fb_restore(Fb *fb) {
    if (!fb->shadow || !fb->shadow_len || !fb->mem) return -1;
    uint8_t *v = fb->mem + fb->base_off;
    if (fb->base_off + fb->shadow_len > fb->memlen) return -1;
    memcpy(v, fb->shadow, fb->shadow_len);
    fb_present(fb, 1);      /* 整页回灌后必须整屏刷一次，否则屏幕上看不出来 */
    return 0;
}

uint32_t fb_sample_sig(Fb *fb) {
    if (!fb->mem) return 0;
    const uint8_t *p = fb->mem + fb->base_off;
    size_t n = (size_t)fb->line_len * (size_t)fb->height;
    if (fb->base_off + n > fb->memlen) n = fb->memlen > fb->base_off ? fb->memlen - fb->base_off : 0;
    /* 97 是质数，和行长 1264 互质，采样点不会每行落在同一列；
     * 2.1MB 里抽 2 万多个点，足够发现"整屏被人重画过"，又几乎不耗时。 */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i + 4 <= n; i += 97) {
        h ^= (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
             ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

int fb_guard(Fb *fb, uint32_t *last_sig) {
    if (fb->eink_kind == 9 || !fb->mem) return 0;

    /* 双缓冲机型：别人（cvm）可能把可见页翻到另一页，
     * 我们的像素还在原处，但屏幕显示的是它那页 —— 看起来就是"界面跳走了"。
     * 这里顺带把绘制基址重新对齐到当前可见页。 */
    if (fb->guard_align > 0 && fb->yres_virtual > fb->height && fb->fd >= 0) {
        struct fb_var_screeninfo var;
        if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) == 0) {
            size_t want = (size_t)var.yoffset * (size_t)fb->line_len;
            if (want + (size_t)fb->line_len * fb->height <= fb->memlen && want != fb->base_off) {
                logmsg("fb: 可见页被翻走（yoffset=%u），绘制基址 %u -> %u，需要重画",
                       var.yoffset, (unsigned)fb->base_off, (unsigned)want);
                fb->base_off = want;
                if (--fb->guard_align == 0)
                    logmsg("fb: 可见页反复变化（驱动自己翻页？），停止基址对齐检查");
                if (last_sig) *last_sig = fb_sample_sig(fb);
                return 2;
            }
        }
    }

    /* 有影子副本时，直接和"我们应该显示的样子"逐块比。
     * 这比哈希稳：cvm 只改顶栏一小条时，稀疏采样有概率漏掉，
     * 逐块比较（每 32 字节看 4 字节）2MB 只要零点几毫秒。 */
    if (fb->shadow && fb->shadow_len) {
        const uint8_t *v = fb->mem + fb->base_off;
        size_t n = fb->shadow_len;
        if (fb->base_off + n > fb->memlen) n = 0;
        /* 整页 memcmp，不做抽样：抽样会漏掉小改动（顶栏时钟就两三个字），
         * 而漏一次就是"桌面上留下一条永远抹不掉的时钟"。2MB 的 memcmp
         * 在 KO3 上约 0.5ms，120ms 一次只占 0.4% CPU，值。 */
        if (n && memcmp(v, fb->shadow, n) != 0) {
            if (last_sig) *last_sig = 0;
            return 1;
        }
        if (last_sig) *last_sig = 0;
        return 0;
    }

    uint32_t s = fb_sample_sig(fb);
    if (last_sig && s != *last_sig) {
        *last_sig = s;
        return 1;
    }
    if (last_sig) *last_sig = s;
    return 0;
}

void fb_after_resume(Fb *fb) {
    if (fb->fd >= 0) ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);
    /* 挂起期间 eips 可能被杀、驱动状态也可能丢，失败计数清零重来 */
    fb->eips_fail = 0;
    fb_present(fb, 1);
}

int fb_poll_rotate(Fb *fb) {
    if (fb->fd < 0 || fb->eink_kind == 9) return 0;
    if (!fb->autorot) return 0;
    struct fb_var_screeninfo var;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) != 0) return -1;
    int r = (int)(var.rotate & 3);
    if (r == fb->hw_rotate) return 0;
    int old = fb->hw_rotate;
    fb->hw_rotate = r;
    logmsg("fb: 系统旋转 %d -> %d（设备翻转），绘制与触摸需同步补偿", old, r);
    return 1;
}
