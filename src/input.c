/* input.c —— 读 Kindle 的触摸屏与实体按键（/dev/input/event*）
 *
 * 触摸这块比看上去麻烦，Kindle 各代驱动不一样：
 *   A. 老机型：ABS_X / ABS_Y + BTN_TOUCH（单点，按下抬起靠 BTN_TOUCH）
 *   B. 新机型：MT 协议 B —— ABS_MT_SLOT / ABS_MT_TRACKING_ID /
 *      ABS_MT_POSITION_X/Y，**很多固件根本不发 BTN_TOUCH**
 *
 * 以前只认 BTN_TOUCH，在 B 类机型上等于完全收不到点击：界面出来了，
 * 但点哪儿都没反应。所以这里同时维护两条状态机，哪条有数据就走哪条。
 */
#include "input.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS 12
#define TOUCH_SLOP 22
#define SWIPE_MIN 70

typedef struct {
    int fd;
    int has_abs;
    int has_mt;      /* 有 ABS_MT_TRACKING_ID：MT 协议 B */
    int btn_touch;   /* 这台设备用 BTN_TOUCH 报按下（A 类） */
    int is_touch;    /* 真的像触摸屏（有 MT 或 BTN_TOUCH），别把加速度计当触摸 */
    int is_keys;     /* 含翻页键（且不是触摸屏）：需要独占，否则系统也会跟着翻页 */
    int has_power;   /* 这个设备上还有电源键：不能抢，抢了关不了机 */
    int grabbed;     /* EVIOCGRAB 成功（独占） */
    int minx, maxx, miny, maxy;
} Dev;

static Dev  g_devs[MAX_DEVS];
static int  g_ndevs;
static int  g_w, g_h;            /* 逻辑画布（已按旋转交换过宽高） */
static int  g_pw, g_ph;          /* 物理屏宽高（framebuffer 原始方向） */
static int  g_rot;               /* 界面旋转 0..3，与 fb.c 的 pix_off 完全一致 */
static int  g_swap, g_ix, g_iy;  /* 兜底：换轴 / 翻转（校准用） */

/* EVIOCGRAB 的结果。
 * Kindle 的 framework（cvm）总是先把触摸屏独占走，而独占是**排他**的：
 * 拿不到独占还能收到事件，但只要 cvm 也被 SIGSTOP 冻住，它的队列就没人读，
 * 于是整块屏"点哪儿都没反应"——1.2.3 的"卡在主页没法操作"就是这么来的。
 * 这里把自己能拿到独占与否记下来，真机上直接能看出是谁占着。 */
static int  g_grab_ok, g_grab_busy;
static int  g_key_grab_ok, g_key_grab_skip;   /* 翻页键设备的独占结果 */
static long g_ev_count;      /* 累计收到的 input_event 条数（判断"触摸死没死"） */
static long g_act_count;     /* 累计判定出的点击/按键数 */

/* 触摸状态机（两条路径共用一套输出状态） */
static int  g_raw_x, g_raw_y;
static int  g_have_xy;
static int  g_down;
static long g_down_ms;
static int  g_down_x, g_down_y;
static int  g_moved;
static int  g_tid = -1;        /* 当前 MT tracking id，-1 表示没手指 */
static int  g_pending = 0;     /* 手指已按下，等 SYN 时记起点（坐标才齐） */

/* 模拟器：从脚本文件读事件，便于在开发机上回放点击流程 */
static Ev   g_sim[512];
static int  g_simn, g_simi;
static long g_sim_pending_wait;

/* 交互模拟（ZH_SIM_LIVE）：事件由外部持续追加到一个队列文件，
 * 本程序每次 poll 增量读取，没有事件就空转等待，永不自行退出。 */
static const char *g_live_path;
static long        g_live_off;

/* 解析一行事件。返回 1 表示产出了一个事件，0 表示忽略（wait/空行/未知）。
 * 除数字按键码外还支持语义别名，方便桌面模拟器直接发 "back"/"next"。 */
static int parse_sim_line(const char *line, Ev *e) {
    char cmd[16];
    int a = 0, b = 0;
    if (sscanf(line, "%15s %d %d", cmd, &a, &b) < 1) return 0;
    memset(e, 0, sizeof(*e));
    if      (strcmp(cmd, "tap") == 0)    { e->type = ZH_TAP;     e->x = a; e->y = b; }
    else if (strcmp(cmd, "swipeL") == 0) { e->type = ZH_SWIPE_L; }
    else if (strcmp(cmd, "swipeR") == 0) { e->type = ZH_SWIPE_R; }
    else if (strcmp(cmd, "key") == 0)    { e->type = ZH_KEY;     e->code = a; }
    else if (strcmp(cmd, "back") == 0)   { e->type = ZH_KEY;     e->code = 158; }
    else if (strcmp(cmd, "home") == 0)   { e->type = ZH_KEY;     e->code = 102; }
    else if (strcmp(cmd, "prev") == 0)   { e->type = ZH_KEY;     e->code = 105; }
    else if (strcmp(cmd, "next") == 0)   { e->type = ZH_KEY;     e->code = 109; }
    else if (strcmp(cmd, "up") == 0)     { e->type = ZH_KEY;     e->code = 103; }
    else if (strcmp(cmd, "down") == 0)   { e->type = ZH_KEY;     e->code = 108; }
    else if (strcmp(cmd, "enter") == 0)  { e->type = ZH_KEY;     e->code = 28;  }
    else if (strcmp(cmd, "quit") == 0)   { e->type = ZH_KEY;     e->code = 999; }
    else if (strcmp(cmd, "demo") == 0)   { e->type = ZH_KEY;     e->code = 997; }
    else if (strcmp(cmd, "qrcode") == 0) { e->type = ZH_KEY;     e->code = 996; }
    else if (strcmp(cmd, "dmlist") == 0) { e->type = ZH_KEY;     e->code = 995; }
    else if (strcmp(cmd, "dmread") == 0) { e->type = ZH_KEY;     e->code = 994; }
    else if (strcmp(cmd, "empty") == 0)  { e->type = ZH_KEY;     e->code = 993; }
    else return 0;
    return 1;
}

static void sim_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f) && g_simn < (int)(sizeof(g_sim) / sizeof(g_sim[0]))) {
        char cmd[16];
        int a = 0;
        if (sscanf(line, "%15s %d", cmd, &a) < 1) continue;
        if (strcmp(cmd, "wait") == 0) { g_sim_pending_wait += a; continue; }
        Ev e;
        if (!parse_sim_line(line, &e)) continue;
        e.ms = g_sim_pending_wait;   /* 等待时间属于"下一个事件" */
        g_sim_pending_wait = 0;
        g_sim[g_simn++] = e;
    }
    fclose(f);
    logmsg("sim: 载入 %d 个事件", g_simn);
}

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void input_set_transform(int swap, int invert_x, int invert_y) {
    g_swap = swap;
    g_ix = invert_x;
    g_iy = invert_y;
}

/* 把界面旋转同步到触摸映射。
 *
 * 1.2.4 的 bug：显示走 fb.c 的 pix_off()，按 ui_rotate 做软件旋转；触摸却只在
 * 启动时写死 input_set_transform(0,0,0)，设置里改旋转只动显示、不动触摸。
 * 结果就是"有一面是正的，其余三个方向全是反的"。
 *
 * 这里不去猜极性，而是把 pix_off 的变换**求逆**：显示是
 *   逻辑 (x,y) -> 物理 (px,py)
 * 触摸收到的是物理坐标，所以反过来 (px,py) -> 逻辑 (x,y)。
 * 只要 rotate=0 是对的（用户反馈正是如此），另外三个方向就一定对。 */
void input_set_rotate(int rotate, int phys_w, int phys_h) {
    g_rot = ((rotate % 4) + 4) % 4;
    if (phys_w > 0) g_pw = phys_w;
    if (phys_h > 0) g_ph = phys_h;
    g_w = (g_rot & 1) ? g_ph : g_pw;
    g_h = (g_rot & 1) ? g_pw : g_ph;
}

int input_rotate(void) { return g_rot; }

/* 纯映射（不碰全局状态），自检和真机都用它：
 * 原始触摸坐标 -> 屏幕逻辑坐标。 */
void input_map_pure(int raw_x, int raw_y,
                    int raw_minx, int raw_maxx, int raw_miny, int raw_maxy,
                    int phys_w, int phys_h, int rot,
                    int swap_hint, int invert_x, int invert_y,
                    int *out_x, int *out_y) {
    int rw = raw_maxx - raw_minx;
    int rh = raw_maxy - raw_miny;
    int px = 0, py = 0;

    /* 原始轴与物理屏是否对调：两边长宽比谁更接近就用谁。
     * 对比的是**物理屏**，不是画布——旋转后画布是转过 90° 的，
     * 拿它比会把"本来就对"的坐标判成需要换轴（1.2.4 就在这里出过错）。 */
    int swap = swap_hint;
    if (!swap && rw > 0 && rh > 0 && phys_w > 0 && phys_h > 0) {
        long long e1 = (long long)rw * phys_h - (long long)rh * phys_w;
        long long e2 = (long long)rh * phys_h - (long long)rw * phys_w;
        if (e1 < 0) e1 = -e1;
        if (e2 < 0) e2 = -e2;
        if (e2 < e1) swap = 1;
    }

    int dx = raw_x - raw_minx, dy = raw_y - raw_miny;
    if (swap) {
        px = rh > 0 ? (int)((long long)dy * phys_w / rh) : 0;
        py = rw > 0 ? (int)((long long)dx * phys_h / rw) : 0;
    } else {
        px = rw > 0 ? (int)((long long)dx * phys_w / rw) : 0;
        py = rh > 0 ? (int)((long long)dy * phys_h / rh) : 0;
    }

    /* pix_off 的逆变换（fb.c）：逻辑 (x,y) -> 物理 (px,py) 是
     *   1: (y, H-1-x)   2: (W-1-x, H-1-y)   3: (W-1-y, x)
     * 分别解出 x,y。 */
    int lx, ly;
    switch (rot & 3) {
        case 1: lx = phys_h - 1 - py; ly = px; break;
        case 2: lx = phys_w - 1 - px; ly = phys_h - 1 - py; break;
        case 3: lx = py;              ly = phys_w - 1 - px; break;
        default: lx = px;             ly = py; break;
    }

    if (invert_x) lx = ((rot & 1) ? phys_h : phys_w) - 1 - lx;
    if (invert_y) ly = ((rot & 1) ? phys_w : phys_h) - 1 - ly;

    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    int lw = (rot & 1) ? phys_h : phys_w;
    int lh = (rot & 1) ? phys_w : phys_h;
    if (lw > 0 && lx > lw - 1) lx = lw - 1;
    if (lh > 0 && ly > lh - 1) ly = lh - 1;
    *out_x = lx;
    *out_y = ly;
}

/* 自检：四个方向都把"物理中心/四角"映射回对应的逻辑位置。
 * 这四条断言一旦红，说明触摸和显示又脱钩了（1.2.4 的毛病）。 */
int input_rotate_selftest(void) {
    const int W = 1264, H = 1680;
    int ok = 1;
    for (int rot = 0; rot < 4; rot++) {
        int lw = (rot & 1) ? H : W;
        int lh = (rot & 1) ? W : H;
        /* 物理中心 -> 逻辑中心 */
        int x, y;
        input_map_pure(W / 2, H / 2, 0, W - 1, 0, H - 1, W, H, rot, 0, 0, 0, &x, &y);
        if (x < lw / 2 - 2 || x > lw / 2 + 2 || y < lh / 2 - 2 || y > lh / 2 + 2) ok = 0;
        /* 物理左上角（0,0） -> 逻辑的某个角，且必须落在画布内 */
        input_map_pure(0, 0, 0, W - 1, 0, H - 1, W, H, rot, 0, 0, 0, &x, &y);
        if (x != 0 && x != lw - 1) ok = 0;
        if (y != 0 && y != lh - 1) ok = 0;
        /* 与 pix_off 的逆关系：逻辑点 (10,20) 经 rot 变换到物理，再映射回来必须是 (10,20) */
        int lx = 10, ly = 20, px, py;
        switch (rot) {
            case 1: px = ly;            py = H - 1 - lx; break;
            case 2: px = W - 1 - lx;    py = H - 1 - ly; break;
            case 3: px = W - 1 - ly;    py = lx; break;
            default: px = lx;           py = ly; break;
        }
        input_map_pure(px, py, 0, W - 1, 0, H - 1, W, H, rot, 0, 0, 0, &x, &y);
        if (x != lx || y != ly) ok = 0;
    }
    return ok;
}

void input_get_raw_range(int *minx, int *maxx, int *miny, int *maxy) {
    *minx = *maxx = *miny = *maxy = 0;
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].has_abs && g_devs[i].is_touch) {
            *minx = g_devs[i].minx;
            *maxx = g_devs[i].maxx;
            *miny = g_devs[i].miny;
            *maxy = g_devs[i].maxy;
            return;
        }
    }
}

/* 按键位图里某一位是否为 1（EVIOCGBIT(EV_KEY) 的结果按字节存） */
static int keybit_get(const unsigned char *kb, size_t n, int code) {
    if (code < 0 || (size_t)code / 8 >= n) return 0;
    return (kb[code / 8] >> (code % 8)) & 1;
}

/* 是否应该独占这个按键设备。
 *
 * 1.2.5 的"翻页键会闪出原生主页"就是因为只抢了触摸屏：翻页键同时被
 * 我们和 framework 收到，于是我们翻页、系统也切到主页。
 *
 * 但抢按键设备有风险：KO3 上电源键和翻页键在不同 gpio 设备上，抢翻页键
 * 那个没问题；万一某机型把它们放在同一个设备里，抢了就等于把电源键
 * 藏起来（长按也关不了机），这个代价不能接受。所以规则是：
 *   含翻页键 且 不含电源键  -> 抢
 *   含电源键                -> 不抢（日志里说明，用户可用 KUAL 的
 *                              「停掉系统界面」模式彻底接管）
 * 抽成纯函数是为了能自检。 */
int input_key_grab_rule(int has_page_keys, int has_power_key) {
    return has_page_keys && !has_power_key;
}

/* grabkeys=0 写进 config.txt 可以彻底关掉按键独占（万一某机型抢了以后
 * 出现"按键没反应"，不用等新版本，改一行配置就行）。 */
static int key_grab_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 1;
    char path[320];
    snprintf(path, sizeof(path), "%s/config.txt", zh_home());
    size_t n = 0;
    char *s = read_file(path, &n);
    if (!s) return cached;
    char *p = s;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *line = str_trim(p);
        if (str_startswith(line, "grabkeys=")) {
            const char *v = line + 9;
            cached = !(strcmp(v, "0") == 0 || str_ieq(v, "off") || str_ieq(v, "no"));
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(s);
    return cached;
}

int input_open(int phys_w, int phys_h) {
    /* 传进来的是**物理**屏尺寸；逻辑画布由 input_set_rotate 按旋转算。
     * 调用顺序：input_open(fb.width, fb.height) -> input_set_rotate(rotate, ...)
     *
     * 传 0 表示"沿用上一次的几何"——input_probe_text() 里会在没有设备时
     * 再调一次 input_open(0,0) 来枚举，如果它把几何清零，之后的触摸映射
     * 就全乱了（1.2.5 之前一直有这颗雷）。 */
    if (phys_w > 0 && phys_h > 0) {
        g_pw = phys_w;
        g_ph = phys_h;
        g_rot = 0;
    }
    if (g_pw <= 0) g_pw = 1264;
    if (g_ph <= 0) g_ph = 1680;
    g_w = (g_rot & 1) ? g_ph : g_pw;
    g_h = (g_rot & 1) ? g_pw : g_ph;
    g_ndevs = 0;
    g_grab_ok = g_grab_busy = 0;
    g_ev_count = g_act_count = 0;
    const char *sim = getenv("ZH_SIM_EVENTS");
    if (sim && getenv("ZH_SIM_LIVE")) {
        /* 交互模拟：造一个"虚拟 KO3 触摸屏"，让硬件诊断也能显示得像真机。
         * 事件从队列文件尾部开始读，启动前预置的内容仍可作为初始化脚本。 */
        g_live_path = sim;
        g_live_off = 0;
        FILE *f = fopen(sim, "r");
        if (f) { fseek(f, 0, SEEK_END); g_live_off = ftell(f); fclose(f); }
        g_ndevs = 1;
        memset(&g_devs[0], 0, sizeof(g_devs[0]));
        g_devs[0].fd = -1;              /* 虚拟设备，没有真实 fd */
        g_devs[0].has_abs = 1;
        g_devs[0].has_mt = 1;           /* KO3 是 MT 协议 B */
        g_devs[0].minx = 0; g_devs[0].maxx = (phys_w > 0 ? phys_w : 1264) - 1;
        g_devs[0].miny = 0; g_devs[0].maxy = (phys_h > 0 ? phys_h : 1680) - 1;
        logmsg("sim-live: 虚拟 KO3 触摸屏 MT-B %dx%d，事件队列 %s",
               g_devs[0].maxx + 1, g_devs[0].maxy + 1, sim);
        return 0;
    }
    if (sim) {
        sim_load(sim);
        return 0;
    }
    for (int i = 0; i < 32 && g_ndevs < MAX_DEVS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        struct input_absinfo ax, ay;
        int has_abs = 0;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 &&
            ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            has_abs = 1;
        } else if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 &&
                   ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 &&
                   ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            has_abs = 1;
        }

        /* 没有任何 EV_KEY 也没有 ABS 的设备没用 */
        unsigned long evbit[4] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
        if (!has_abs && !(evbit[0] & (1UL << EV_KEY))) { close(fd); continue; }

        /* 判断 MT 协议 B：能不能上报 ABS_MT_TRACKING_ID */
        unsigned char absbits[(ABS_MAX / 8) + 2];
        memset(absbits, 0, sizeof(absbits));
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
        int has_mt = (absbits[ABS_MT_TRACKING_ID / 8] >> (ABS_MT_TRACKING_ID % 8)) & 1;

        /* 有没有 BTN_TOUCH：A 类触摸屏靠它报按下。
         * 加速度计也有 ABS_X/ABS_Y，但它既没有 MT 也没有 BTN_TOUCH，
         * 拿它当触摸屏会让点击坐标彻底跑偏，更不能去抢它的独占。 */
        /* 按字节索引：BTN_TOUCH 是 0x14A（330），用 unsigned long[8] 只覆盖到
         * 255，越界读出来的东西纯属随机——这一处差点把"是不是触摸屏"判反。 */
        unsigned char keybit[(KEY_MAX / 8) + 2];
        memset(keybit, 0, sizeof(keybit));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
        int has_btn_touch = (BTN_TOUCH / 8 < (int)sizeof(keybit))
                            ? (keybit[BTN_TOUCH / 8] >> (BTN_TOUCH % 8)) & 1 : 0;
        int is_touch = has_abs && (has_mt || has_btn_touch);

        Dev *d = &g_devs[g_ndevs++];
        memset(d, 0, sizeof(*d));
        d->fd = fd;
        d->has_abs = has_abs;
        d->has_mt = has_mt;
        d->is_touch = is_touch;
        if (has_abs) {
            d->minx = ax.minimum;
            d->maxx = ax.maximum;
            d->miny = ay.minimum;
            d->maxy = ay.maximum;
        }
        d->has_power = keybit_get(keybit, sizeof(keybit), 116);   /* KEY_POWER */
        int has_prev = keybit_get(keybit, sizeof(keybit), 105);   /* KEY_PREVIOUS */
        int has_next = keybit_get(keybit, sizeof(keybit), 106);   /* KEY_NEXT */
        int has_190 = keybit_get(keybit, sizeof(keybit), 190);
        int has_191 = keybit_get(keybit, sizeof(keybit), 191);
        int has_192 = keybit_get(keybit, sizeof(keybit), 192);
        int has_page = has_prev || has_next || has_190 || has_191 || has_192;
        d->is_keys = (!is_touch && has_page);

        logmsg("input: %s abs=%d mt=%d touch=%d btntouch=%d pagekey=%d(p%d/n%d/%d%d%d) power=%d "
               "range=%d..%d / %d..%d",
               path, has_abs, has_mt, is_touch, has_btn_touch,
               has_page, has_prev, has_next, has_190, has_191, has_192, d->has_power,
               d->minx, d->maxx, d->miny, d->maxy);

        /* 只对触摸屏抢独占。
         * 不抢按键设备（event0/event6 这类）：电源键、霍尔传感器也在上面，
         * 独占它们等于把开关机键抢走，出问题就再也关不掉了。 */
        if (d->is_keys) {
            if (!key_grab_enabled()) {
                logmsg("input: %s 含翻页键，但 grabkeys=0，按配置不抢", path);
            } else if (input_key_grab_rule(has_page, d->has_power)) {
                errno = 0;
                if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                    d->grabbed = 1;
                    g_key_grab_ok++;
                    logmsg("input: %s grab=ok（已独占翻页键设备，系统不会再跟着翻页）", path);
                } else {
                    g_key_grab_skip++;
                    logmsg("input: %s 翻页键独占失败 errno=%d(%s)——系统仍会收到翻页键",
                           path, errno, errno ? strerror(errno) : "?");
                }
            } else {
                g_key_grab_skip++;
                /* 同一个设备上还有电源键：宁可不抢。否则长按电源键关不了机，
                 * 用户只能等电池耗尽——这比"翻页闪一下主页"严重得多。 */
                logmsg("input: %s 含翻页键**也**含电源键，不抢独占（保电源键可用）", path);
            }
        }

        if (is_touch) {
            errno = 0;
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                d->grabbed = 1;
                g_grab_ok++;
                logmsg("input: %s grab=ok（已独占触摸屏）", path);
            } else {
                g_grab_busy++;
                /* EBUSY 就是"别人已经独占了"。这是实锤：framework 还攥着屏，
                 * 此时不接管系统界面，点击永远进不来。 */
                logmsg("input: %s grab 失败 errno=%d(%s)——触摸屏被别的进程独占",
                       path, errno, errno ? strerror(errno) : "?");
            }
        }
    }
    if (g_ndevs == 0) {
        logmsg("input: 没有可用的输入设备");
        return -1;
    }
    return 0;
}

void input_close(void) {
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0) continue;
        /* 先显式放开独占再关，别指望 close 的副作用——有的驱动要到
         * 进程退出才释放，中间那几秒系统界面会收不到触摸 */
        if (g_devs[i].grabbed) ioctl(g_devs[i].fd, EVIOCGRAB, 0);
        close(g_devs[i].fd);
    }
    g_ndevs = 0;
    g_grab_ok = g_grab_busy = 0;
}

/* 1=拿到独占  0=没有触摸设备（或模拟器）  -1=被别的进程独占 */
int input_grab_status(void) {
    if (g_grab_ok) return 1;
    if (g_grab_busy) return -1;
    return 0;
}

/* 补抢独占：启动时可能刚好赶上 framework 重新拉起 cvm 的窗口期，
 * 那一下抢不到就一直收不到点击。主循环空闲时每两秒补一次，
 * 一旦抢到就记一行日志（真机上能看出"几秒后才恢复可用"）。 */
int input_retry_grab(void) {
    int got = 0;
    for (int i = 0; i < g_ndevs; i++) {
        Dev *d = &g_devs[i];
        if (d->fd < 0 || d->grabbed) continue;
        /* 触摸屏要抢；翻页键设备同样要抢（1.2.5 只补抢触摸，按键一直漏给系统）。 */
        int want = d->is_touch || (d->is_keys && !d->has_power && key_grab_enabled());
        if (!want) continue;
        if (ioctl(d->fd, EVIOCGRAB, 1) == 0) {
            d->grabbed = 1;
            if (d->is_touch) g_grab_ok++; else g_key_grab_ok++;
            g_grab_busy = g_grab_busy > 0 ? g_grab_busy - 1 : 0;
            got = 1;
            logmsg("input: fd=%d 补抢独占成功（%s）", d->fd,
                   d->is_touch ? "触摸屏，现在能收到点击了" : "翻页键设备，系统不会再抢键");
        }
    }
    return got;
}

long input_event_count(void) { return g_ev_count; }
long input_action_count(void) { return g_act_count; }

const char *input_probe_text(void) {
    static char out[600];
    if (getenv("ZH_SIM_EVENTS") && getenv("ZH_SIM_LIVE"))
        return "模拟器：虚拟 KO3 触摸屏（MT 协议 B）";
    if (getenv("ZH_SIM_EVENTS")) return "模拟器（脚本事件）";
    if (g_ndevs == 0) input_open(0, 0);
    int n = snprintf(out, sizeof(out), "%d 个设备:", g_ndevs);
    for (int i = 0; i < g_ndevs && n < (int)sizeof(out) - 90; i++) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, " [fd%d abs%d mt%d%s %d..%d/%d..%d]",
                      g_devs[i].fd, g_devs[i].has_abs, g_devs[i].has_mt,
                      g_devs[i].is_keys ? (g_devs[i].grabbed ? " key:grab" : " key") : "",
                      g_devs[i].minx, g_devs[i].maxx, g_devs[i].miny, g_devs[i].maxy);
    }
    return out;
}

/* 只有这些键才当"按键"处理。
 *
 * Kindle 的触摸屏在按下时会先发 BTN_TOOL_FINGER，键盘底座、电源键、
 * 霍尔传感器也都在这条链路上。以前"任何 value==1 的 EV_KEY 都当按键"，
 * 结果每摸一下屏幕都会先冒出一个伪按键：列表页被它踢走、阅读页多翻一页。
 */
static int is_action_key(int code) {
    switch (code) {
        case 103: /* KEY_UP */
        case 105: /* KEY_LEFT  / KEY_PREVIOUS（Oasis 侧键） */
        case 106: /* KEY_RIGHT / KEY_NEXT */
        case 108: /* KEY_DOWN */
        case 104: /* KEY_PAGEUP */
        case 109: /* KEY_PAGEDOWN */
        case 102: /* KEY_HOME */
        case 158: /* KEY_BACK */
        case 139: /* KEY_MENU */
        case 28:  /* KEY_ENTER */
        case 96:  /* KEY_KPENTER */
        case 190: /* Kindle 自定义翻页键 */
        case 191:
        case 192:
            return 1;
        default:
            return 0;
    }
}

static void map_point(int dev_x, int dev_y, int *ox, int *oy) {
    const Dev *d = NULL;
    /* 优先用真正的触摸屏。机器上还有别的 ABS 设备（加速度计之类），
     * 拿错一个，点击坐标就整体跑偏，看起来像"点哪儿都不对"。 */
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].has_abs && g_devs[i].is_touch) { d = &g_devs[i]; break; }
    if (!d) for (int i = 0; i < g_ndevs; i++) if (g_devs[i].has_abs) { d = &g_devs[i]; break; }

    if (!d) { *ox = dev_x; *oy = dev_y; return; }

    int x = 0, y = 0;
    input_map_pure(dev_x, dev_y, d->minx, d->maxx, d->miny, d->maxy,
                   g_pw > 0 ? g_pw : g_w, g_ph > 0 ? g_ph : g_h,
                   g_rot, g_swap, g_ix, g_iy, &x, &y);

    /* 画布尺寸以调用方给的为准（模拟器会指定更小的画布） */
    if (g_w > 0 && x > g_w - 1) x = g_w - 1;
    if (g_h > 0 && y > g_h - 1) y = g_h - 1;
    *ox = x;
    *oy = y;
}

/* 手指抬起：判定是点击还是滑动 */
static int finish_touch(Ev *out) {
    int cx, cy;
    g_pending = 0;
    map_point(g_raw_x, g_raw_y, &cx, &cy);
    long dt = now_ms() - g_down_ms;
    int dx = cx - g_down_x;
    out->x = cx;
    out->y = cy;
    out->code = 0;
    out->ms = dt;
    if (dt < 700 && dx <= -SWIPE_MIN) { out->type = ZH_SWIPE_L; return 1; }
    if (dt < 700 && dx >= SWIPE_MIN) { out->type = ZH_SWIPE_R; return 1; }
    if (!g_moved || (dx < TOUCH_SLOP && dx > -TOUCH_SLOP)) { out->type = ZH_TAP; return 1; }
    return 0;
}

static void begin_touch(void) {
    g_down = 1;
    g_moved = 0;
    g_down_ms = now_ms();
    map_point(g_raw_x, g_raw_y, &g_down_x, &g_down_y);
}

/* 单条 input_event 的处理（抽出来是为了能离线测触摸状态机） */
static int handle_event(Dev *d, const struct input_event *e, Ev *out) {
    if (e->type == EV_ABS) {
        if (e->code == ABS_MT_POSITION_X || e->code == ABS_X) {
            g_raw_x = e->value;
            g_have_xy = 1;
        } else if (e->code == ABS_MT_POSITION_Y || e->code == ABS_Y) {
            g_raw_y = e->value;
            g_have_xy = 1;
        } else if (e->code == ABS_MT_TRACKING_ID) {
            if (e->value >= 0) {
                g_tid = e->value;
                g_pending = 1;       /* 手指按下，等 SYN 时坐标齐了再记起点 */
            } else {
                g_tid = -1;
            }
        }
    } else if (e->type == EV_KEY) {
        if (e->code == BTN_TOUCH) {
            d->btn_touch = 1;
            if (e->value == 1) {
                g_pending = 1;       /* 同上：等 SYN 再记起点，坐标才准 */
            } else if (e->value == 0 && g_down) {
                g_down = 0;
                g_tid = -1;
                if (finish_touch(out)) return 1;
            }
        } else if (e->value == 1 && is_action_key(e->code)) {
            out->type = ZH_KEY;
            out->code = e->code;
            out->x = out->y = 0;
            return 1;
        }
    } else if (e->type == EV_SYN) {
        /* 一帧结束：按下记起点、MT 抬手、判断是否在拖动 */
        if (g_pending && !g_down) {
            g_pending = 0;
            begin_touch();
        }
        if (d->has_mt && g_down && !d->btn_touch && g_tid < 0) {
            g_down = 0;
            if (finish_touch(out)) return 1;
        }
        if (g_down) {
            int cx, cy;
            map_point(g_raw_x, g_raw_y, &cx, &cy);
            int dx = cx - g_down_x, dy = cy - g_down_y;
            if (dx > TOUCH_SLOP || dx < -TOUCH_SLOP || dy > TOUCH_SLOP || dy < -TOUCH_SLOP)
                g_moved = 1;
        }
    }
    return 0;
}

int input_poll(Ev *out, int timeout_ms) {
    if (g_live_path) {
        FILE *f = fopen(g_live_path, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (g_live_off > sz) g_live_off = 0;   /* 队列文件被重建了 */
            if (fseek(f, g_live_off, SEEK_SET) == 0) {
                char line[128];
                long start = ftell(f);
                if (fgets(line, sizeof(line), f)) {
                    size_t len = strlen(line);
                    if (len > 0 && line[len - 1] == '\n') {
                        line[len - 1] = 0;
                        g_live_off = ftell(f);
                        fclose(f);
                        if (parse_sim_line(line, out)) {
                            logmsg("sim-live: %s", line);
                            return 1;
                        }
                        return 0;
                    }
                    g_live_off = start;   /* 只写到一半，下轮重读 */
                }
            }
            fclose(f);
        }
        /* 没有新事件：照常睡眠，界面保持存活（绝不自行退出） */
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        return 0;
    }
    if (getenv("ZH_SIM_EVENTS")) {
        if (g_simi < g_simn && g_sim[g_simi].ms > 0) {
            int w = (int)(g_sim[g_simi].ms < timeout_ms ? g_sim[g_simi].ms : timeout_ms);
            struct timespec ts;
            ts.tv_sec = w / 1000;
            ts.tv_nsec = (long)(w % 1000) * 1000000L;
            nanosleep(&ts, NULL);
            g_sim[g_simi].ms -= w;
            return 0;
        }
        if (g_simi >= g_simn) {
            /* 脚本跑完：发一个退出事件，模拟器不会无限循环 */
            memset(out, 0, sizeof(*out));
            out->type = ZH_KEY;
            out->code = 999;
            return 1;
        }
        *out = g_sim[g_simi++];
        return 1;
    }

    fd_set rf;
    FD_ZERO(&rf);
    int maxfd = -1;
    for (int i = 0; i < g_ndevs; i++) {
        FD_SET(g_devs[i].fd, &rf);
        if (g_devs[i].fd > maxfd) maxfd = g_devs[i].fd;
    }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int n = select(maxfd + 1, &rf, NULL, NULL, &tv);
    if (n <= 0) return 0;

    for (int i = 0; i < g_ndevs; i++) {
        if (!FD_ISSET(g_devs[i].fd, &rf)) continue;
        Dev *d = &g_devs[i];
        struct input_event ie[32];
        ssize_t got = read(d->fd, ie, sizeof(ie));
        if (got == 0 || (got < 0 && (errno == ENODEV || errno == EBADF))) {
            /* 设备被拔掉/关闭：不摘掉它的话 select 会一直返回可读，
             * 主循环空转把 CPU 打满，机器就"卡死"了 */
            logmsg("input: 设备 fd=%d 已断开，移除", d->fd);
            close(d->fd);
            *d = g_devs[--g_ndevs];
            i--;
            continue;
        }
        if (got < 0) continue;
        int cnt = (int)(got / (ssize_t)sizeof(struct input_event));
        g_ev_count += cnt;
        for (int k = 0; k < cnt; k++) {
            if (handle_event(d, &ie[k], out)) { g_act_count++; return 1; }
        }
    }
    return 0;
}
