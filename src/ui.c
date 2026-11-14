/* ui.c —— 「知乎阅读」全部界面与交互
 *
 * 设计原则：
 *   - 整页翻页：正文一次排版成页，翻页只重画缓存行，不重排、不求网络；
 *   - 不卡：联网在 fork 出来的子进程里做，主进程继续响应点击，可随时取消；
 *   - 全机型：所有尺寸按屏幕宽高换算，不硬编码任何像素或按键码。
 */
#include "ui.h"

#include "api.h"
#include "account.h"
#include "common.h"
#include "content.h"
#include "fb.h"
#include "font.h"
#include "img.h"
#include "input.h"
#include "json.h"
#include "layout.h"
#include "net.h"
#include "qr.h"
#include "social.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------- 配置 ---------------- */

typedef struct {
    int size;       /* 0 小 1 中 2 大 */
    int fast;       /* 1 快速刷新 0 清晰 2 Regal（1.2.9 实验） */
    int rotate;     /* 0..3 */
    int images;     /* 1 图片位置显示 [图片] */
    int margin_pct; /* 左右留白百分比 */
    /* refresh=mxcfb|pan|none：设置界面里没有这一项，由用户手写进 config.txt。
     * 必须原样透传——config_save 会整份重写文件，不带上它的话，
     * 用户改一次字号就把这行冲掉了，下次启动又退回到会重启的刷新路径。 */
    char refresh[8];
    /* 1.2.10：showbar 与 framework 彻底解耦。顶栏（时间/网络/电量）是
     * cvm 画的，1.2.9 曾用"杀 cvm"来隐藏它——cvm 一死 framework 看门狗
     * 就在宽限期后整机重启（真机实测两次）。现在 showbar=0 只表示
     * "共存 + 屏幕守护压顶栏"，不再碰 framework。默认 1（显示，推荐）。 */
    int showbar;
    /* 推荐列表条数：6/12/18（6 的倍数，最多 18） */
    int rec;
} Config;

static Config g_cfg = { 1, 1, 0, 0, 7, "", 0, 6 };

static void config_load(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/config.txt", zh_home());
    size_t n = 0;
    char *s = read_file(path, &n);
    if (!s) return;
    char *line = strtok(s, "\n");
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = str_trim(line);
            char *v = str_trim(eq + 1);
            int iv = atoi(v);
            if (strcmp(k, "size") == 0) g_cfg.size = iv;
            else if (strcmp(k, "fast") == 0) g_cfg.fast = iv;
            else if (strcmp(k, "rotate") == 0) g_cfg.rotate = iv;
            else if (strcmp(k, "images") == 0) g_cfg.images = iv;
            else if (strcmp(k, "margin") == 0) g_cfg.margin_pct = iv;
            else if (strcmp(k, "showbar") == 0) g_cfg.showbar = iv;
            else if (strcmp(k, "rec") == 0) g_cfg.rec = iv;
            else if (strcmp(k, "refresh") == 0)
                snprintf(g_cfg.refresh, sizeof(g_cfg.refresh), "%s", v);
        }
        line = strtok(NULL, "\n");
    }
    free(s);
    if (g_cfg.size < 0 || g_cfg.size > 2) g_cfg.size = 1;
    if (g_cfg.rotate < 0 || g_cfg.rotate > 3) g_cfg.rotate = 0;
    if (g_cfg.margin_pct < 2 || g_cfg.margin_pct > 15) g_cfg.margin_pct = 7;
    /* 1.2.8：images 从"开关"变成三档（0 关 / 1 点按 / 2 自动）。
     * 老配置里的 images=1 是"显示"，正好对应新的"点按"，不用迁移。 */
    if (g_cfg.images < 0 || g_cfg.images > 2) g_cfg.images = 0;
    /* 1.2.9：fast 三态（1 局部 / 0 整屏闪 / 2 Regal），非法值回到 1 */
    if (g_cfg.fast < 0 || g_cfg.fast > 2) g_cfg.fast = 1;
    /* 推荐条数只认 6/12/18 */
    if (g_cfg.rec != 6 && g_cfg.rec != 12 && g_cfg.rec != 18) g_cfg.rec = 6;
    g_cfg.showbar = g_cfg.showbar ? 1 : 0;
}

static void config_save(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/config.txt", zh_home());

    /* 设置页改一次就把 config.txt 整份重写。手写的 refresh= / eips= / blankpulse=
     * 不是设置项，必须原样搬回去——1.2.1 就是没搬，用户设的 refresh=none
     * 被一次改字号冲掉，下次启动又掉回会重启的刷新路径（"修好了又重启"）。 */
    Buf keep;
    buf_init(&keep);
    {
        size_t on = 0;
        char *old = read_file(path, &on);
        if (old) {
            char *p = old;
            while (p && *p) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = 0;
                char *line = str_trim(p);
                if (*line) {
                    char key[32];
                    char *eq = strchr(line, '=');
                    size_t kl = eq ? (size_t)(eq - line) : 0;
                    if (kl >= sizeof(key)) kl = sizeof(key) - 1;
                    memcpy(key, line, kl);
                    key[kl] = 0;
                    char *kt = str_trim(key);
                    int managed = !eq ||
                                  strcmp(kt, "size") == 0 || strcmp(kt, "fast") == 0 ||
                                  strcmp(kt, "rotate") == 0 || strcmp(kt, "images") == 0 ||
                                  strcmp(kt, "margin") == 0 || strcmp(kt, "showbar") == 0 ||
                                  strcmp(kt, "rec") == 0;
                    if (!managed) buf_printf(&keep, "%s\n", line);
                }
                if (!nl) break;
                p = nl + 1;
            }
            free(old);
        }
    }

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "size=%d\nfast=%d\nrotate=%d\nimages=%d\nmargin=%d\nshowbar=%d\nrec=%d\n",
                     g_cfg.size, g_cfg.fast, g_cfg.rotate, g_cfg.images, g_cfg.margin_pct,
                     g_cfg.showbar, g_cfg.rec);
    if (n > 0 && (size_t)n < sizeof(buf) && keep.len)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s", buf_cstr(&keep));
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    write_file_atomic(path, buf, (size_t)n);
    buf_free(&keep);
}

static int doc_images_prepare(Doc *d, ImgRefs *refs, int text_w, int ph_h, int tap);
static const char *img_placeholder_label(int tap);

/* 自检用：三档图片模式在排版阶段就该分得清。
 * 自动档（2）：未下到的图写成 "\x01下标\t宽\t高"，并计入待下张数；
 * 点按档（1）：写成 "\x02下标\t宽\t高"，但**不计入**待下张数（等用户点），
 *             也不许后台自己开下。
 * 这两个前缀如果写反，界面表现就是"点了没反应"或者"没点就偷偷跑流量"。 */
int ui_img_mode_test(void) {
    if (strcmp(img_placeholder_label(1), "点按加载此图") != 0) return 0;
    if (strcmp(img_placeholder_label(0), "点按加载此图") == 0) return 0;
    const char *html = "<p>前</p><img data-actualsrc=\"https://pic1.zhimg.com/v2-selftest-none.jpg\">";
    int ok = 1;

    for (int tap = 0; tap <= 1; tap++) {
        Doc d;
        doc_init(&d);
        ImgRefs r;
        imgrefs_init(&r);
        html_to_doc_img(html, &d, IMG_REAL, &r);
        if (r.n != 1) ok = 0;
        int pending = doc_images_prepare(&d, &r, 800, 600, tap);

        int imgblk = -1;
        for (int i = 0; i < d.n; i++) if (d.v[i].kind == BLK_IMAGE) imgblk = i;
        if (imgblk < 0 || !d.v[imgblk].text) {
            ok = 0;
        } else {
            char want = tap ? '\x02' : '\x01';
            if (d.v[imgblk].text[0] != want) ok = 0;
            if (tap && pending != 0) ok = 0;      /* 点按档不许自动排队 */
            if (!tap && pending != 1) ok = 0;
        }
        doc_free(&d);
        imgrefs_free(&r);
    }
    return ok;
}

/* 自检用：先写一份带 refresh= 的 config，读回来，再存一次，确认这行还在。
 * 这条链路的坑很小但很致命：设置页改一次字号就会整份重写 config.txt，
 * 若没把 refresh 透传，用户设的 refresh=none 会静默消失，
 * 下次启动又退回到 FBIOPAN_DISPLAY，于是"修好了又重启"。 */
int ui_config_roundtrip_test(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/config.txt", zh_home());
    /* 三个手写键一起验：refresh=（刷新路径）、eips=（后路开关）、blankpulse=
     * 都是排障用的，任何一个被冲掉，用户下次开机会掉回会重启/冻屏的状态。 */
    const char *seed = "size=2\nfast=0\nrotate=1\nimages=2\nmargin=9\n"
                       "refresh=pan\neips=off\nblankpulse=1\n";
    if (write_file_atomic(path, seed, strlen(seed)) != 0) return 0;

    g_cfg.refresh[0] = 0;
    config_load();
    int loaded = (strcmp(g_cfg.refresh, "pan") == 0) && g_cfg.size == 2 &&
                 g_cfg.images == 2;   /* 1.2.8：images 是三档，2 不能被夹成 1 */

    config_save();
    size_t n = 0;
    char *s = read_file(path, &n);
    int kept = s && strstr(s, "refresh=pan") != NULL &&
               strstr(s, "eips=off") != NULL && strstr(s, "blankpulse=1") != NULL;
    if (s) free(s);
    unlink(path);

    /* 复位成默认值，别把测试数据带进后面的流程 */
    g_cfg.size = 1;
    g_cfg.fast = 1;
    g_cfg.rotate = 0;
    g_cfg.images = 0;
    g_cfg.margin_pct = 7;
    g_cfg.refresh[0] = 0;
    g_cfg.showbar = 1;   /* 1.2.10：默认显示（共存）。1.2.9 默认 0 曾触发看门狗重启 */
    g_cfg.rec = 6;
    return loaded && kept;
}

/* ---------------- 应用状态 ---------------- */

enum {
    SCR_HOME = 0,
    SCR_LIST,
    SCR_READER,
    SCR_SETTINGS,
    SCR_ABOUT,
    SCR_LOADING,
    SCR_MESSAGE,
    SCR_INPUT,
    SCR_HISTORY,
    SCR_ACCOUNT,   /* 账号 / 登录 */
    SCR_QRCODE,    /* 二维码扫码登录 */
    SCR_DMLIST,    /* 私信会话列表 */
    SCR_DMREAD,    /* 会话内消息（整页翻页） */
};

typedef struct {
    Fb  fb;
    int W, H;
    int base;       /* 正文字号 px */
    int margin;
    int header_h;
    int footer_h;

    int scr;
    int scr_prev;
    int need_redraw;
    int quit;

    /* 列表 */
    ApiList list;
    char list_title[200];
    char list_kind[16];    /* hot/rec/search/answers/collection/history */
    char list_qid[64];
    int  list_sel;
    int  list_top;
    int  list_page;
    /* 列表为空时的说明。空列表不能让用户看到一片白：
     * 必须写清「为什么空」和「下一步做什么」。 */
    char list_msg[512];

    /* 阅读 */
    Doc    doc;
    Layout lay;
    int    page;
    int    reading;
    char   doc_title[600];
    char   doc_author[128];
    char   reader_err[256];

    /* 正文图片：1.2.7 起"先出字、后补图"。
     * doc_refs 是正文里出现的所有图片地址（按出现顺序），后台子进程
     * 从第一张开始逐张下载，每下完一张就写一行进度到 img_stat；
     * 主循环据此把占位替换成真图并只刷一次屏。 */
    ImgRefs doc_refs;
    pid_t   img_pid;
    int     img_total;
    int     img_done;
    int     img_dirty;      /* 有图到货，下次画阅读页时先重排 */
    char    img_stat[320];
    char    img_referer[512];

    /* 历史 */
    ApiList hist;

    /* 输入 */
    char input[256];
    int  input_len;
    char input_title[96];
    int  input_kind;
    int  kb_shift;

    /* 消息 */
    char msg[512];

    /* 加载 */
    char  load_text[256];
    long  load_start;
    int   load_cancellable;

    /* 账号 / 二维码登录 */
    char  acc_user[128];       /* 已登录用户名，空=游客 */
    char  qr_link[512];        /* 二维码内容 */
    char  qr_state_text[256];  /* 扫码状态提示 */
    int   qr_state;            /* 1 等扫码 2 待确认 3 成功 -1 失败 */
    int   qr_sel;              /* 账号页选中行 */

    /* 私信 */
    ThreadList threads;
    MsgList    msgs;
    char       dm_peer[128];
    char       dm_id[64];
    int        dm_sel;
    int        dm_top;

    /* 二维码登录：轮询放在 fork 子进程里，父进程不切屏、不阻塞 */
    pid_t      qr_pid;
    long       qr_last_ms;
    char       qr_path[320];
    int        qr_demo;       /* 模拟器离线二维码：不触发网络轮询 */

    /* 屏幕归属守护（1.2.4）：默认与 framework 共存，cvm 可能随时把
     * 我们画好的这页盖掉。screen_sig 是"我们最后一帧的采样哈希"。 */
    uint32_t   screen_sig;
    long       guard_ms;
    long       guard_first;    /* 第一次被抢屏的时刻（判断"持续抢"） */
    int        stop_req;       /* （1.2.10 已废弃）曾经请求过停止系统界面 */
    long       grab_ms;
    long       msg_until;     /* 自动关闭的提示（触摸可能不通时不能弹模态） */       /* 补抢触摸独占的节流 */
    int        guard_hits;    /* 屏幕被抢过几次 */
    long       idle_since;    /* 最近一次有事件的时间，用来判断触摸是不是死了 */

    /* 1.2.8：只让"翻一页"走局部刷新。换屏（首页↔列表↔阅读↔设置…）
     * 一律全刷，否则上一屏的字会留在纸上（真机照片里的"重叠"）。 */
    int        fast_next;
    /* 三档图片模式的当前档位快照（0 无图 / 1 点按加载 / 2 自动异步） */
    int        img_mode;
} App;

static App g;

/* 二维码登录状态在父子进程间通过文件传递。
 * 同架构、同一份二进制，直接 fwrite/fread 结构体，无需序列化；
 * session 里是完整 cookie，保证每次轮询都带着累积的 _xsrf/_zap/d_c0。 */
typedef struct {
    AuthQr q;
    char   session[4096];
} QrStateFile;

/* 登录类任务（Cookie 导入 / 登录态校验）的结果 */
typedef struct {
    char user[128];
    char err[256];
    int  rc;
} LoginResult;

/* 前向声明 */
static long now_ms_ui(void);
static int  fetch_job(const char *path, const char *title, char *out_path, size_t outsz,
                      char *err, size_t errsz);
static void draw_list(void);
static void draw_home(void);
static void draw_reader(void);
static void draw_dm_read(void);
static void draw_dm_list(void);
static int  reader_top_n(void);
static void draw_account(void);
static void draw_qrcode(void);
static void draw_settings(void);
static void show_loading(const char *text, int cancellable);
static void show_message(const char *fmt, ...);
static void open_history(void);
static void act_account(void);
static void act_dm(void);
static void qr_stop(void);
static void qr_tick(void);
static int  is_cjk_simple(uint32_t c);
static int  draw_wrapped(int x, int y, int maxw, int px, const char *s, int gray, int maxy);
static void sim_seed_image(const char *url);

static int is_cjk_simple(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) ||
           (c >= 0xFF00 && c <= 0xFF60);
}

/* ---------------- 基础绘制 ---------------- */

static void text_at(int x, int baseline, int px, const char *s, int gray, int bold) {
    font_draw(&g.fb, x, baseline, px, s, (int)strlen(s), gray, bold);
}

/* 超宽则截断并加省略号 */
static void text_clip(int x, int baseline, int px, const char *s, int maxw, int gray, int bold) {
    int len = (int)strlen(s);
    if (font_width(px, s, len) <= maxw) {
        font_draw(&g.fb, x, baseline, px, s, len, gray, bold);
        return;
    }
    const char *ell = "…";
    int ellw = font_width(px, ell, (int)strlen(ell));
    int i = 0, w = 0;
    while (i < len) {
        uint32_t cp;
        int n = u8_decode(s + i, (size_t)(len - i), &cp);
        int cw = font_width(px, s + i, n);
        if (w + cw + ellw > maxw) break;
        w += cw;
        i += n;
    }
    font_draw(&g.fb, x, baseline, px, s, i, gray, bold);
    font_draw(&g.fb, x + w, baseline, px, ell, (int)strlen(ell), gray, bold);
}

static void draw_header(const char *title, const char *right) {
    int h = g.header_h;
    fb_fill(&g.fb, 0, 0, g.W, h + 1, 255);
    int px = g.base * 3 / 4;
    int baseline = (h + px) / 2 - px / 6;
    int rw = 0;
    if (right && *right) rw = font_width(px, right, (int)strlen(right)) + g.margin;
    text_clip(g.margin, baseline, px, title, g.W - 2 * g.margin - rw, 0, 0);
    if (right && *right) {
        int w = font_width(px, right, (int)strlen(right));
        text_at(g.W - g.margin - w, baseline, px, right, 110, 0);
    }
    fb_hline(&g.fb, 0, h, g.W, 60);
}

/* ---------------- 页脚按钮（1.2.7 统一交互） ----------------
 * 1.2.6 之前每个页面各自约定「点哪里返回」：列表点页眉，阅读页点页眉或按返回键，
 * 二维码点任意处……真机上根本没有统一的肌肉记忆。1.2.7 起所有次级页面
 * （列表 / 阅读 / 设置 / 账号 / 二维码 / 私信 / 关于）右下角都有同一个
 * 「返回」按钮，位置、大小、边框完全一致；阅读页额外把「点右侧翻页」
 * 限制在正文区，避免想翻页却退回列表。 */
typedef struct { int x, y, w, h; } Rect;

enum { BTN_NONE = 0, BTN_BACK, BTN_PREV, BTN_NEXT, BTN_REFRESH };

static Rect g_fb_btn[4];
static int  g_fb_id[4];
static int  g_fb_n;

static void footer_begin(void) {
    fb_hline(&g.fb, 0, g.H - g.footer_h, g.W, 200);
    g_fb_n = 0;
}

/* 从右往左排：第一个加进去的就在最右边 */
static Rect footer_btn(int id, const char *label) {
    Rect r = { 0, 0, 0, 0 };
    if (g_fb_n >= 4 || !label || !*label) return r;
    int px = g.base * 3 / 5;
    int tw = font_width(px, label, (int)strlen(label));
    int w = tw + g.base * 3 / 2;
    int h = g.footer_h - 8;
    if (h < g.base) h = g.base;
    int y = g.H - g.footer_h + (g.footer_h - h) / 2;
    int x = g.W - g.margin - w;
    for (int i = 0; i < g_fb_n; i++) x -= g_fb_btn[i].w + g.base / 3;
    if (x < g.margin / 2) x = g.margin / 2;
    r.x = x; r.y = y; r.w = w; r.h = h;

    fb_fill(&g.fb, x, y, w, h, 255);
    fb_hline(&g.fb, x, y, w, 60);
    fb_hline(&g.fb, x, y + h - 1, w, 60);
    fb_vline(&g.fb, x, y, h, 60);
    fb_vline(&g.fb, x + w - 1, y, h, 60);
    text_at(x + (w - tw) / 2, y + (h + px) / 2 - px / 6, px, label, 0, 0);

    g_fb_btn[g_fb_n] = r;
    g_fb_id[g_fb_n] = id;
    g_fb_n++;
    return r;
}

/* 页脚左半边文字：自动躲开按钮，绝不压在按钮上 */
static void footer_end(const char *left) {
    int px = g.base * 3 / 5;
    int baseline = g.H - g.footer_h + (g.footer_h + px) / 2 - px / 6;
    int limit = g.W - g.margin;
    for (int i = 0; i < g_fb_n; i++)
        if (g_fb_btn[i].x - g.base / 2 < limit) limit = g_fb_btn[i].x - g.base / 2;
    if (limit < g.margin + g.base) limit = g.margin + g.base;
    if (left && *left) text_clip(g.margin, baseline, px, left, limit - g.margin, 120, 0);
}

/* 页脚右端小字（版本号之类，不是按钮） */
static void footer_right_text(const char *s) {
    if (!s || !*s) return;
    /* 有按钮时不画：版本号会压在「返回」上（账号页出过这个） */
    if (g_fb_n > 0) return;
    int px = g.base * 3 / 5;
    int w = font_width(px, s, (int)strlen(s));
    int baseline = g.H - g.footer_h + (g.footer_h + px) / 2 - px / 6;
    text_at(g.W - g.margin - w, baseline, px, s, 120, 0);
}

/* 命中哪个按钮；返回 BTN_NONE 表示没点到按钮 */
static int footer_hit(int x, int y) {
    for (int i = 0; i < g_fb_n; i++) {
        Rect *r = &g_fb_btn[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return g_fb_id[i];
    }
    return BTN_NONE;
}

/* 三种提交方式：
 *   present_full —— 整屏换内容（首帧、切屏、消息框）：GC16 全刷，画面最干净；
 *   present_ui   —— 同一屏内的变化（选中条移动、列表翻页、二维码状态刷新）：
 *                   fast=1 时走 DU，不闪、响应快，每 6 帧自动补一次全刷；
 *   present_fast —— 阅读页翻页专用（同上，但不受 fast 开关影响前先判一次）。
 * 1.2.3 的毛病是全都用 present_full：点一下菜单就整屏黑闪一次，一秒钟才回来，
 * 用户感受就是"卡"。电子墨水本来就慢，能省的那一次闪一定要省。 */
static void present_full(void) {
    g.fast_next = 0;
    fb_present(&g.fb, 1);
    g.screen_sig = fb_sample_sig(&g.fb);
}
static void present_fast(void) {
    fb_present(&g.fb, 0);
    g.screen_sig = fb_sample_sig(&g.fb);
}
/* 换屏专用：整屏换内容一律走全刷（GC16 / eips -f -g），否则上一屏的字会
 * 留在纸上——这就是真机照片里"每个界面都重叠"的根。只有明确标记了
 * "这一屏只是翻一页"（g.fast_next）才允许走局部更新。 */
static void present_screen(void) {
    if (g.fast_next && g_cfg.fast) {
        g.fast_next = 0;
        present_fast();
        return;
    }
    g.fast_next = 0;
    present_full();
}

/* ---------------- 屏幕：首页 ---------------- */

static const char *HOME_ITEMS[] = {
    "热榜", "推荐", "私信", "账号 / 登录", "搜索",
    "我的收藏", "阅读历史", "设置", "关于", "退出",
};
static const char *HOME_DESC[] = {
    "实时热榜 · 50 条", "个性化推荐流（需登录）", "私信会话（需登录）", "扫码登录 / 导入 Cookie",
    "输入关键词搜索", "收藏夹（需登录）", "最近读过的内容", "字号 / 刷新 / 旋转",
    "版本与设备信息", "返回 Kindle 系统",
};
#define HOME_COUNT ((int)(sizeof(HOME_ITEMS) / sizeof(HOME_ITEMS[0])))

/* 首页第一行的 y 起点（draw_home 与 handle_tap 共用，避免两处算不一样） */
static int home_top(void) {
    return g.header_h + g.base + g.base * 9 / 5 + g.base * 3 / 5 * 3 + g.base / 2;
}
/* 行数从 8 增到 10，行高按可用高度自适应，任何机型都不溢出 */
static int home_row_h(void) {
    int avail = g.H - g.footer_h - home_top();
    int rh = avail / HOME_COUNT;
    int natural = g.base * 7 / 2;
    if (rh <= 0) rh = natural;
    if (rh > natural) rh = natural;   /* 别把菜单拉得过于稀疏 */
    return rh;
}

static void draw_home(void) {
    fb_clear(&g.fb, 255);

    int top = g.header_h + g.base;
    int px = g.base * 9 / 5;
    text_at(g.margin, top + px, px, "知乎阅读", 0, 1);
    int sub_px = g.base * 3 / 5;
    /* 登录态放在副标题里，不额外占一行：首页 10 项菜单的高度是按
     * 现有行数算的，再加一行会把每项压矮、也会打乱模拟器的点击坐标。 */
    int logged = account_has_login();
    char sub[128];
    if (g.fb.eink_kind == 3)
        /* 刷新探测失败被降级：屏幕几乎不会更新，必须让人一眼看到原因，
         * 否则会被当成"程序卡死" */
        snprintf(sub, sizeof(sub), "刷新不可用（EPDC 探测失败）· 见 log.txt");
    else if (logged) snprintf(sub, sizeof(sub), "Kindle 原生客户端 · 已登录");
    else       snprintf(sub, sizeof(sub), "Kindle 原生客户端 · 游客模式（热榜可能受限）");
    text_at(g.margin, top + px + sub_px * 2, sub_px, sub, logged ? 110 : 60, 0);

    int y = top + px + sub_px * 3 + g.base / 2;
    int rh = home_row_h();
    for (int i = 0; i < HOME_COUNT; i++) {
        if (i == g.list_sel) {
            fb_fill(&g.fb, g.margin / 2, y, g.W - g.margin, rh, 235);
            fb_vline(&g.fb, g.margin / 2, y, rh, 0);
        }
        int bpx = g.base;
        int baseline = y + rh / 2 - bpx / 6;
        text_at(g.margin + g.base / 2, baseline, bpx, HOME_ITEMS[i], 0, i == g.list_sel);
        int dpx = g.base * 3 / 5;
        text_at(g.margin + g.base / 2 + font_width(bpx, HOME_ITEMS[i], (int)strlen(HOME_ITEMS[i])) +
                    g.base,
                baseline - 2, dpx, HOME_DESC[i], 130, 0);
        y += rh;
    }

    /* 页脚左侧常驻登录态：游客点热榜大概率拿到空列表，
     * 与其让人对着空白猜，不如在首页就把原因和出口摆出来。 */
    char foot[192];
    if (account_has_login())
        snprintf(foot, sizeof(foot), "已登录：%s", g.acc_user[0] ? g.acc_user : "是");
    else
        snprintf(foot, sizeof(foot), "游客 · 空列表请到「账号 / 登录」扫码或导入 Cookie");
    footer_begin();
    footer_end(foot);
    footer_right_text("v" ZH_VERSION);
    present_screen();
}

/* ---------------- 屏幕：列表 ---------------- */

static int list_row_h(void) { return g.base * 13 / 5; }
static int list_rows_per_page(void) {
    int avail = g.H - g.header_h - g.footer_h;
    int n = avail / list_row_h();
    return n > 0 ? n : 1;
}

static void draw_list(void) {
    fb_clear(&g.fb, 255);

    int per = list_rows_per_page();
    int total_pages = (g.list.n + per - 1) / per;
    if (total_pages < 1) total_pages = 1;
    int page = g.list_sel / per;
    if (page >= total_pages) page = total_pages - 1;

    /* 保证选中项在可视页内 */
    if (g.list_sel < g.list_top) g.list_top = g.list_sel;
    if (g.list_sel >= g.list_top + per) g.list_top = g.list_sel - per + 1;
    if (g.list_top < 0) g.list_top = 0;

    char right[64];
    snprintf(right, sizeof(right), "%d/%d", page + 1, total_pages);
    draw_header(g.list_title, right);

    int y = g.header_h + 2;
    int rh = list_row_h();
    for (int i = 0; i < per; i++) {
        int idx = g.list_top + i;
        if (idx >= g.list.n) break;
        ApiItem *it = &g.list.v[idx];
        int sel = (idx == g.list_sel);
        if (sel) {
            fb_fill(&g.fb, 0, y, g.W, rh, 0);
        }
        int fg = sel ? 255 : 0;
        int fg2 = sel ? 200 : 110;
        int tpx = g.base;
        int baseline = y + rh / 3;
        char num[16];
        snprintf(num, sizeof(num), "%d", idx + 1);
        int numw = font_width(tpx * 3 / 4, num, (int)strlen(num));
        text_at(g.margin, baseline, tpx * 3 / 4, num, fg2, 0);
        text_clip(g.margin + numw + g.base / 2, baseline, tpx, it->title,
                  g.W - 2 * g.margin - numw - g.base / 2, fg, sel);
        if (it->sub[0]) {
            int spx = g.base * 3 / 5;
            text_clip(g.margin + numw + g.base / 2, y + rh - spx, spx, it->sub,
                      g.W - 2 * g.margin - numw, fg2, 0);
        }
        if (!sel) fb_hline(&g.fb, g.margin, y + rh - 1, g.W - 2 * g.margin, 220);
        y += rh;
    }

    if (g.list.n == 0) {
        /* 这里以前只画一个灰色小字「（空）」，真机上的观感就是"打不开"：
         * 用户分不清是没网、被知乎风控，还是没登录。现在把原因和下一步
         * 都摆出来，至少让人知道该修哪一环。 */
        int x = g.margin;
        int y = g.header_h + g.base * 2;
        int w = g.W - 2 * g.margin;
        int maxy = g.H - g.footer_h - g.base * 4;

        text_at(x, y, g.base * 6 / 5, "没有取到内容", 0, 1);
        y += g.base * 2;

        const char *reason = g.list_msg[0] ? g.list_msg : "列表为空，但没有拿到具体原因。";
        y = draw_wrapped(x, y, w, g.base, reason, 40, maxy);
        y += g.base;

        y = draw_wrapped(x, y, w, g.base * 4 / 5, "可以试试：", 0, maxy);
        y = draw_wrapped(x, y, w, g.base * 4 / 5,
                         "· 到「账号 / 登录」扫码登录，或导入 Cookie\n"
                         "· 确认 WiFi 已连上（设置里可重新联网）\n"
                         "· 换个网络再试（例如手机热点）",
                         70, maxy);
    }

    char left[160];
    if (g.list.n > 0) snprintf(left, sizeof(left), "共 %d 条 · 点按打开", g.list.n);
    else              snprintf(left, sizeof(left), "空列表 · 点页眉或按键返回");
    footer_begin();
    footer_btn(BTN_BACK, "返回");
    if (g.list.n > 0 && total_pages > 1) {
        if (page + 1 < total_pages) footer_btn(BTN_NEXT, "下一页");
        if (page > 0) footer_btn(BTN_PREV, "上一页");
    }
    if (strcmp(g.list_kind, "rec") == 0) footer_btn(BTN_REFRESH, "刷新");
    footer_end(left);
    present_screen();
}

/* ---------------- 屏幕：阅读 ---------------- */

/* 正文区上下各留一点白，避免行贴着页眉线/压到页脚 */
static int reader_pad(void) { return g.base * 2 / 3; }

/* 1.2.7：阅读页标题比列表页页眉明显大一档（粗体，1.2 倍正文）。
 * 页眉跟着加高，否则放大后的标题会压到正文第一行上。 */
static int reader_header_h(void) { return g.base * 5 / 2; }

static void reader_layout(void) {
    layout_free(&g.lay);
    int w = g.W - 2 * g.margin;
    /* 底部多留 base/3：排版高度刚好卡到页脚时，最后一行的字身会压到
     * 页脚横线上（看上去像"被裁掉半行"）。留出下伸部的高度就没有这问题。 */
    int h0 = g.H - reader_header_h() - g.footer_h - 2 * reader_pad() - g.base / 3;
    /* 1.2.9：标题只在第一页显示，后面每页整页都是正文——页高也按两种算 */
    int h1 = g.H - g.footer_h - reader_top_n() - reader_pad() - g.base / 3;
    if (h1 < h0 * 3 / 4) h1 = h0;          /* 极端小屏别把后续页排爆 */
    layout_build2(&g.lay, &g.doc, w, h1, g.base, h0);
}

/* 阅读页后续页（无标题）的顶部留白，比第一页小得多 */
static int reader_top_n(void) { return g.base * 2 / 3; }

/* 重排后把阅读位置钉回原处：记下当前页首行属于哪个正文块，重排完再找
 * 这个块落在第几页。图片异步到达会让页高变化、页数变化，没有这一步
 * 用户会被莫名其妙地弹到别的页去。 */
static void reader_relayout_keep(void) {
    int anchor = 0, nth = 0, oldpage = g.page;
    if (g.lay.pages && g.lay.npages > 0 && g.page >= 0 && g.page < g.lay.npages &&
        g.lay.pages[g.page] < g.lay.n) {
        int li = g.lay.pages[g.page];
        anchor = g.lay.lines[li].blk;
        /* 一个块可能跨页：光记块下标会把"块从上一页延续过来"的位置算回块首。
         * 再记一个"这是块内第几行"，重排后按同样的块内偏移找回同一行。 */
        int first = li;
        while (first > 0 && g.lay.lines[first - 1].blk == anchor) first--;
        nth = li - first;
    }
    reader_layout();
    if (!g.lay.pages || g.lay.npages <= 0) { g.page = 0; return; }

    int first2 = -1, count = 0;
    for (int i = 0; i < g.lay.n; i++) {
        if (g.lay.lines[i].blk != anchor) continue;
        if (first2 < 0) first2 = i;
        count++;
    }
    int target = -1;
    if (first2 >= 0) {
        target = first2 + (nth < count ? nth : count - 1);
    }
    int page = 0;
    if (target >= 0)
        for (int p = 0; p < g.lay.npages; p++)
            if (g.lay.pages[p] <= target) page = p;
    if (page >= g.lay.npages) page = g.lay.npages - 1;
    if (page < 0) page = 0;
    logmsg("重排锚定: 块%d 块内第%d行 原页%d -> 新页%d（共 %d 页）",
           anchor, nth, oldpage, page, g.lay.npages);
    g.page = page;
}

/* 阅读页页眉：标题大号加粗，右上角仍显示页码。
 * 1.2.9：只在第一页画标题；后续页顶部只留一条细分隔线，正文更长。 */
static void draw_reader_header(const char *title, const char *right, int page) {
    int h = reader_header_h();
    if (page > 0) {
        /* 后续页：没有标题。顶部只画一条浅线，右上角小字页码。 */
        fb_fill(&g.fb, 0, 0, g.W, reader_top_n(), 255);
        fb_hline(&g.fb, 0, reader_top_n() - 1, g.W, 230);
        if (right && *right) {
            int rpx = g.base * 3 / 5;
            int w = font_width(rpx, right, (int)strlen(right));
            text_at(g.W - g.margin - w, reader_top_n() - 1 - rpx - 2, rpx, right, 150, 0);
        }
        return;
    }
    fb_fill(&g.fb, 0, 0, g.W, h + 1, 255);
    int px = g.base * 6 / 5;
    int rpx = g.base * 3 / 4;
    int rw = 0;
    if (right && *right) rw = font_width(rpx, right, (int)strlen(right)) + g.margin;
    int baseline = h - px / 2 - 2;
    if (baseline < px) baseline = px;
    text_clip(g.margin, baseline, px, title, g.W - 2 * g.margin - rw, 0, 1);
    if (right && *right) {
        int w = font_width(rpx, right, (int)strlen(right));
        text_at(g.W - g.margin - w, h - rpx / 2 - 3, rpx, right, 110, 0);
    }
    fb_hline(&g.fb, 0, h, g.W, 60);
}

/* 当前这一页上图片的缓存（只留一张：一页里通常也就一两张图，
 * 每次重绘都重新读盘会明显拖慢翻页）。 */
static ImgGray g_img_cur;
static char    g_img_cur_path[512];

/* 图片还没到时的占位框：画清楚"这里有一张图"，而不是让正文里凭空出现
 * 一片空白。1.2.8 起区分两种：
 *   自动模式（\x01）：写着「图片加载中… n/N」，后台正在从前往后补；
 *   点按模式（\x02）：写着「点按加载此图」，点一下才去下（省流量、也最快）。 */
/* 占位框上的主文字。抽出来是为了能在自检里断言三档文案——
 * 这三句话是用户唯一能看出"当前是哪一档"的地方。 */
static const char *img_placeholder_label(int tap) {
    if (tap) return "点按加载此图";
    return g.img_total > 0 ? "图片加载中…" : "图片（未能加载）";
}

static void draw_img_placeholder(int ytop, int w, int h, int tap) {
    if (h <= 0 || w <= 0) return;
    fb_fill(&g.fb, g.margin, ytop, w, h, 242);
    fb_hline(&g.fb, g.margin, ytop, w, 170);
    fb_hline(&g.fb, g.margin, ytop + h - 1, w, 170);
    fb_vline(&g.fb, g.margin, ytop, h, 170);
    fb_vline(&g.fb, g.margin + w - 1, ytop, h, 170);

    const char *t = img_placeholder_label(tap);
    int px = g.base * 4 / 5;
    int tw = font_width(px, t, (int)strlen(t));
    /* 文字基线放在框心稍下，给上面的箭头留位置（1.2.8 之前箭头压在字上） */
    int ty = ytop + h / 2 + px * 2 / 3;
    text_at(g.margin + (w - tw) / 2, ty, px, t, tap ? 60 : 120, tap);
    if (tap) {
        /* 文字上方画一个向下小箭头，暗示"这里可以点"：
         * 杆 + 三角，整体在文字上沿之上，不压字。 */
        int ax = g.margin + w / 2;
        int ah = px;                    /* 箭头总高 */
        int hw = px / 3;                /* 三角半宽 */
        int a_bottom = ty - px;         /* 文字上沿再往上让一格，别贴字 */
        int ay = a_bottom - ah;
        if (ay < ytop + 4) ay = ytop + 4;
        fb_fill(&g.fb, ax - 1, ay, 3, ah - hw, 90);
        for (int k = 0; k < hw; k++) {
            int yy = a_bottom - hw + k;
            fb_hline(&g.fb, ax - (hw - k), yy, 2 * (hw - k) + 1, 90);
        }
        return;
    }
    if (g.img_total > 0) {
        char s[48];
        snprintf(s, sizeof(s), "%d/%d", g.img_done, g.img_total);
        int spx = g.base * 3 / 5;
        int sw = font_width(spx, s, (int)strlen(s));
        text_at(g.margin + (w - sw) / 2, ty + spx + spx / 2, spx, s, 150, 0);
    }
}

static void draw_reader_image(const Line *ln, int ytop, int w) {
    if (!ln->s || !*ln->s) return;
    /* text = "路径\t宽\t高"；"\x01" 开头表示还没下到的占位 */
    char path[512];
    const char *t1 = strchr(ln->s, '\t');
    if (!t1) return;
    size_t plen = (size_t)(t1 - ln->s);
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, ln->s, plen);
    path[plen] = 0;

    /* 排版层已经保证一张图整页放得下，这里只是兜底：真放不下就不画，
     * 免得把半张图压扁贴在页尾（比留白更难看）。 */
    int ih = ln->px;
    int bottom = g.H - g.footer_h;
    if (ytop < 0 || ytop + ih > bottom || ih <= 0) return;
    if (path[0] == '\x01' || path[0] == '\x02') {
        draw_img_placeholder(ytop, w, ih, path[0] == '\x02');
        return;
    }

    if (strcmp(path, g_img_cur_path) != 0) {
        img_free(&g_img_cur);
        snprintf(g_img_cur_path, sizeof(g_img_cur_path), "%s", path);
        if (img_cache_load(path, &g_img_cur) != 0) {
            g_img_cur_path[0] = 0;
            return;
        }
    }
    if (!g_img_cur.px) return;
    fb_blit_gray_scaled(&g.fb, g.margin, ytop, w, ih, g_img_cur.px, g_img_cur.w, g_img_cur.h);
}

static void draw_reader(void) {
    /* 后台图片到货后只置脏标记，真正重排放在画之前：一次重排 + 一次刷屏，
     * 比"每来一张就重排一次"省一半时间，也少闪一半。 */
    if (g.img_dirty) {
        g.img_dirty = 0;
        reader_relayout_keep();
    }

    fb_clear(&g.fb, 255);

    char right[64];
    if (g.lay.npages > 0)
        snprintf(right, sizeof(right), "%d/%d", g.page + 1, g.lay.npages);
    else
        snprintf(right, sizeof(right), "-");
    draw_reader_header(g.doc_title, right, g.page);

    /* 1.2.9：第一页顶在标题页眉下，后续页没有页眉、顶端留白很小。
     * 页的全局 y 基准由 layout_page_top 给出（第一页 0，后面逐页累加）。 */
    int top = (g.page == 0) ? reader_header_h() + reader_pad() : reader_top_n();
    int page_top = (g.lay.pages && g.page < g.lay.npages)
                       ? layout_page_top(&g.lay, g.page) : 0;
    int start = g.lay.pages ? g.lay.pages[g.page] : 0;
    int end = (g.lay.pages && g.page + 1 <= g.lay.npages) ? g.lay.pages[g.page + 1] : 0;

    for (int i = start; i < end; i++) {
        Line *ln = &g.lay.lines[i];
        int y = top + (ln->y - page_top) + ln->baseline;
        if (ln->kind == BLK_IMAGE) {
            /* 图片块：ln->y 是块顶，ln->px 是高度，文本里带缓存路径与尺寸 */
            draw_reader_image(ln, top + (ln->y - page_top), g.W - 2 * g.margin);
            continue;
        }
        /* 下伸部也要留在页脚线以上：1.2.7 只按基线判，最后一行会被页脚裁掉 */
        if (y + ln->px / 4 > g.H - g.footer_h) break;
        if (ln->kind == BLK_META) {
            font_draw(&g.fb, g.margin + ln->x, y, ln->px, ln->s, ln->len, 90, 0);
            continue;
        }
        if (ln->kind == BLK_QUOTE) {
            /* 引用：左侧竖线 + 稍浅的字色 */
            int barh = ln->px * 3 / 2;   /* 略高于字身，多行引用连成一条 */
            fb_fill(&g.fb, g.margin + ln->px / 5, y - ln->px, 2, barh, 90);
            font_draw(&g.fb, g.margin + ln->x, y, ln->px, ln->s, ln->len, 45, ln->bold);
        } else if (ln->kind == BLK_LI) {
            font_draw(&g.fb, g.margin + ln->x, y, ln->px, ln->s, ln->len, 0, ln->bold);
            if (ln->x > 0) {
                /* 项目符号只画在列表项首行 */
                int dot = ln->px / 6;
                if (dot < 3) dot = 3;
                fb_fill(&g.fb, g.margin + ln->px / 3, y - ln->px / 2, dot, dot, 0);
            }
        } else {
            font_draw(&g.fb, g.margin + ln->x, y, ln->px, ln->s, ln->len, 0, ln->bold);
        }
    }

    /* 进度条 */
    if (g.lay.npages > 1) {
        int w = g.W - 2 * g.margin;
        int pw = w * (g.page + 1) / g.lay.npages;
        fb_fill(&g.fb, g.margin, g.H - g.footer_h + 2, w, 2, 220);
        fb_fill(&g.fb, g.margin, g.H - g.footer_h + 2, pw, 2, 60);
    }

    char left[160];
    if (g.lay.npages > 0) {
        if (g.img_total > 0 && g.img_done < g.img_total)
            snprintf(left, sizeof(left), "第 %d 页 / 共 %d 页 · 图 %d/%d",
                     g.page + 1, g.lay.npages, g.img_done, g.img_total);
        else
            snprintf(left, sizeof(left), "第 %d 页 / 共 %d 页", g.page + 1, g.lay.npages);
    } else {
        snprintf(left, sizeof(left), "%.120s", g.reader_err);
    }
    footer_begin();
    footer_btn(BTN_BACK, "返回");
    footer_end(left);
    /* 阅读页翻页同样尊重「快刷/清晰」设置：设成清晰就别用 DU，
     * 否则用户会觉得"设置了没用" */
    present_screen();
}

static void reader_goto(int page) {
    /* npages==0 时 pages 可能已分配但没有任何页，page 会被夹成 -1，
     * 接着 pages[-1] 就是越界读——这是能直接把人踹回主页的那种崩溃 */
    if (!g.lay.pages || g.lay.npages <= 0) return;
    if (page < 0) page = 0;
    if (page > g.lay.npages - 1) page = g.lay.npages - 1;
    if (page == g.page) return;
    g.page = page;
    g.fast_next = 1;      /* 翻页：允许局部刷新（快，且整屏都变，残影看不清） */
    draw_reader();
}

/* ---------------- 屏幕：设置 ---------------- */

static int set_row_h(void) { return g.base * 12 / 5; }

static void draw_settings(void) {
    fb_clear(&g.fb, 255);
    draw_header("设置", NULL);

    static const char *names[] = {
        "正文字号", "翻页刷屏", "界面旋转", "显示图片", "页边距",
        "推荐条数", "系统状态栏", "重新排版", "清空缓存", "返回首页",
    };
    char vals[10][64];
    snprintf(vals[0], sizeof(vals[0]), "%s", g_cfg.size == 0 ? "小" : (g_cfg.size == 1 ? "中" : "大"));
    /* 1.2.8：fast 不再等于"用 DU 波形"（那是残影和字发虚的根），
     * 而是"翻页要不要整屏闪一下"。默认不闪（局部 + GC16），画面本身是干净的。
     * 1.2.9 新增第三档 Regal（REAGL 残影补偿波形，实验）。 */
    snprintf(vals[1], sizeof(vals[1]), "%s",
             g_cfg.fast == 2 ? "Regal（实验）" :
             (g_cfg.fast ? "不闪（局部）" : "整屏闪（最干净）"));
    snprintf(vals[2], sizeof(vals[2]), "%d°", g_cfg.rotate * 90);
    static const char *imgmodes[] = { "关闭", "点按加载", "自动加载" };
    if (g_cfg.images < 0 || g_cfg.images > 2) g_cfg.images = 0;
    snprintf(vals[3], sizeof(vals[3]), "%s", imgmodes[g_cfg.images]);
    snprintf(vals[4], sizeof(vals[4]), "%d%%", g_cfg.margin_pct);
    snprintf(vals[5], sizeof(vals[5]), "%d 条", g_cfg.rec);
    snprintf(vals[6], sizeof(vals[6]), "%s", g_cfg.showbar ? "显示（推荐）" : "隐藏（压屏）");
    snprintf(vals[7], sizeof(vals[7]), "重新分页");
    snprintf(vals[8], sizeof(vals[8]), "含图片缓存");
    vals[9][0] = 0;

    int rh = set_row_h();
    int y = g.header_h;
    for (int i = 0; i < 10; i++) {
        if (i == g.list_sel) {
            fb_fill(&g.fb, 0, y, g.W, rh, 0);
        }
        int fg = (i == g.list_sel) ? 255 : 0;
        int fg2 = (i == g.list_sel) ? 200 : 120;
        int bpx = g.base;
        text_at(g.margin, y + rh / 2 + bpx / 3, bpx, names[i], fg, i == g.list_sel);
        if (vals[i][0]) {
            int w = font_width(g.base * 3 / 4, vals[i], (int)strlen(vals[i]));
            text_at(g.W - g.margin - w, y + rh / 2 + bpx / 3, g.base * 3 / 4, vals[i], fg2, 0);
        }
        if (i != g.list_sel) fb_hline(&g.fb, g.margin, y + rh - 1, g.W - 2 * g.margin, 220);
        y += rh;
    }

    footer_begin();
    footer_btn(BTN_BACK, "返回");
    footer_end("点按切换");
    present_screen();
}

/* ---------------- 屏幕：关于 ---------------- */

static void draw_about(void) {
    fb_clear(&g.fb, 255);
    draw_header("关于", NULL);

    int px = g.base * 3 / 5;
    int y = g.header_h + g.base + g.base / 2;
    int lh = px * 3 / 2;

    char line[256];
    int minx, maxx, miny, maxy;
    input_get_raw_range(&minx, &maxx, &miny, &maxy);

    const char *lines[] = {
        "",
        "运行于越狱 Kindle 的原生程序（非 KOReader 插件）",
        "",
        "",
        "",
        "",
        "",
        "",
        "数据来源：知乎网页端接口（设备端签名）",
        "本项目以 AGPL-3.0 发布",
        "",
        "点任意位置返回",
    };

    for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        switch (i) {
            case 0: snprintf(line, sizeof(line), "知乎阅读 %s", ZH_VERSION); break;
            case 3: {
                const char *ek = g.fb.eink_kind == 1 ? "EPDC" :
                                 (g.fb.eink_kind == 2 ? "普通fb" :
                                 (g.fb.eink_kind == 9 ? "模拟器" : "未知"));
                snprintf(line, sizeof(line), "屏幕：%dx%d  %dbpp  刷新：%s(%d)",
                         g.fb.width, g.fb.height, g.fb.bpp, ek, g.fb.eink_kind);
                break;
            }
            case 4: snprintf(line, sizeof(line), "字体：%s（%d 个）", font_info(), font_nfaces()); break;
            case 5: snprintf(line, sizeof(line), "触摸范围：%d..%d / %d..%d", minx, maxx, miny, maxy); break;
            case 6: snprintf(line, sizeof(line), "数据目录：%s", zh_home()); break;
            case 7: snprintf(line, sizeof(line), "登录状态：%s",
                         account_has_login() ? (g.acc_user[0] ? g.acc_user : "已导入 Cookie") : "游客"); break;
            default: snprintf(line, sizeof(line), "%s", lines[i]); break;
        }
        text_at(g.margin, y + i * lh, i == 0 ? g.base : px, line, i == 0 ? 0 : 60, i == 0);
    }
    present_screen();
}

/* ---------------- 屏幕：消息/输入/加载 ---------------- */

/* 按屏幕宽度折行绘制，返回最后一行的下一行 y。
 * 断行规则和消息屏（原 draw_message 内联实现）完全一致：
 * 支持手动换行、优先在空格/中文/ASCII 标点处断，行首行尾空格不画。 */
static int draw_wrapped(int x, int y, int maxw, int px, const char *s, int gray, int maxy) {
    if (!s || !*s) return y;
    int lh = px * 3 / 2;
    if (lh < 1) lh = 1;
    const char *p = s;
    while (*p) {
        if (y > maxy) break;
        int len = (int)strlen(p);
        int i = 0, last = 0, width = 0, hard_nl = 0;
        while (i < len) {
            if (p[i] == '\n') { hard_nl = 1; break; }
            uint32_t cp;
            int n = u8_decode(p + i, (size_t)(len - i), &cp);
            if (n <= 0) { n = 1; cp = (unsigned char)p[i]; }
            int cw = font_width(px, p + i, n);
            if (width + cw > maxw) break;
            width += cw;
            i += n;
            /* 可断点：空格、中日韩字符之后，以及 ASCII 标点之后。
             * 少了标点这一条，/mnt/us/zhihu/cookie.txt 这种纯西文长串
             * 找不到断点就会被硬切在单词中间。 */
            if (cp == ' ' || is_cjk_simple(cp) ||
                cp == '/' || cp == '.' || cp == ',' || cp == ';' || cp == ':' ||
                cp == '-' || cp == '_' || cp == ')' || cp == ']' || cp == '}' ||
                cp == '!' || cp == '?') last = i;
        }
        int cut;
        if (i >= len) cut = len;
        else if (hard_nl) cut = i;                 /* 手动换行优先 */
        else if (last > 0) cut = last;             /* 优先在词/标点边界断 */
        else cut = i > 0 ? i : 1;                 /* 实在没有断点才硬断 */
        if (cut <= 0) cut = 1;
        while (cut > 0 && p[cut - 1] == ' ') cut--;   /* 行尾空格不画 */
        if (cut > 0) font_draw(&g.fb, x, y, px, p, cut, gray, 0);
        p += cut;
        if (hard_nl && *p == '\n') p++;
        while (*p == ' ') p++;                     /* 行首空格不画 */
        y += lh;
    }
    return y;
}

static void draw_message(void) {
    fb_clear(&g.fb, 255);
    draw_wrapped(2 * g.margin, g.H / 3, g.W - 4 * g.margin, g.base, g.msg, 0,
                 g.H - g.footer_h - g.base * 3 / 2);
    int hpx = g.base * 3 / 5;
    text_at(2 * g.margin, g.H - g.footer_h - hpx, hpx, "点任意位置关闭", 130, 0);
    present_full();
}

/* 加载页只在「取消」按钮区域内响应点击，避免误触打断请求 */
static int g_cancel_x, g_cancel_y, g_cancel_w, g_cancel_h;

static void draw_loading(void) {
    fb_clear(&g.fb, 255);
    int px = g.base;
    text_at(g.margin, g.H / 2, px, g.load_text, 0, 0);
    int spx = g.base * 3 / 5;
    long el = (now_ms_ui() - g.load_start) / 1000;
    char buf[128];
    /* 写清「还在等」和「最多等多久」：真机上出现过把加载页当成死机的情况 */
    if (g.load_cancellable) snprintf(buf, sizeof(buf), "已等待 %ld 秒 · 最多 20 秒", el);
    else                   snprintf(buf, sizeof(buf), "已等待 %ld 秒", el);
    text_at(g.margin, g.H / 2 + px * 3 / 2, spx, buf, 130, 0);

    const char *hint = el >= 8 ? "网络较慢，可点下方「取消」返回上一页"
                               : "正在联网请求，请稍候";
    text_at(g.margin, g.H / 2 + px * 3, spx, hint, 150, 0);

    g_cancel_w = g.W / 4;
    g_cancel_h = g.base * 2;
    g_cancel_x = (g.W - g_cancel_w) / 2;
    g_cancel_y = g.H - g.footer_h - g_cancel_h * 2;
    fb_rect(&g.fb, g_cancel_x, g_cancel_y, g_cancel_w, g_cancel_h, 60);
    int cpx = g.base * 3 / 4;
    int tw = font_width(cpx, "取消", 6);
    text_at(g_cancel_x + (g_cancel_w - tw) / 2, g_cancel_y + g_cancel_h / 2 + cpx / 3, cpx,
            "取消", 0, 0);
    present_full();
}

static const char *KB_ROWS[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};

static void draw_input(void) {
    fb_clear(&g.fb, 255);
    draw_header(g.input_title, NULL);

    int px = g.base;
    int y = g.header_h + g.base;
    text_at(g.margin, y + px, g.base * 3 / 4, g.input, 0, 0);
    /* 光标 */
    int w = font_width(g.base * 3 / 4, g.input, g.input_len);
    fb_vline(&g.fb, g.margin + w + 2, y + px - g.base * 3 / 4, g.base, 0);
    fb_hline(&g.fb, g.margin, y + px + g.base / 3, g.W - 2 * g.margin, 210);

    int kb_top = g.H - g.footer_h - (g.base * 11 / 5) * 5;
    if (kb_top < y + px * 2) kb_top = y + px * 2;
    int key_h = (g.H - g.footer_h - kb_top) / 5;
    int cols = 10;
    int key_w = (g.W - 2 * g.margin) / cols;

    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[r];
        int len = (int)strlen(row);
        for (int c = 0; c < len; c++) {
            int x = g.margin + c * key_w + (g.W - 2 * g.margin - key_w * len) / 2;
            int ky = kb_top + r * key_h;
            fb_rect(&g.fb, x + 2, ky + 2, key_w - 4, key_h - 4, 120);
            char s[2] = { row[c], 0 };
            if (row[c] >= 'a' && row[c] <= 'z' && g.kb_shift) s[0] = (char)(s[0] - 32);
            int cpx = g.base * 4 / 5;
            int tw = font_width(cpx, s, 1);
            text_at(x + (key_w - tw) / 2, ky + key_h / 2 + cpx / 3, cpx, s, 0, 0);
        }
    }

    /* 第 5 行：空格 / 退格 / 确定 / 取消 */
    int ry = kb_top + 4 * key_h;
    const char *labels[4] = { "空格", "退格", "确定", "取消" };
    int widths[4] = { 4, 2, 2, 2 };
    int cx = g.margin;
    int total_units = 10;
    for (int i = 0; i < 4; i++) {
        int w2 = (g.W - 2 * g.margin) * widths[i] / total_units;
        fb_rect(&g.fb, cx + 2, ry + 2, w2 - 4, key_h - 4, i == 2 ? 0 : 120);
        int cpx = g.base * 3 / 4;
        int tw = font_width(cpx, labels[i], (int)strlen(labels[i]));
        text_at(cx + (w2 - tw) / 2, ry + key_h / 2 + cpx / 3, cpx, labels[i],
                i == 2 ? 255 : 0, 0);
        cx += w2;
    }

    present_screen();
}

/* ---------------- 屏幕：账号 / 登录 ---------------- */

static const char *ACC_ITEMS[] = {
    "二维码扫码登录",
    "导入 Cookie（cookie.txt）",
    "校验登录态",
    "私信端点诊断",
    "退出登录",
    "返回首页",
};
#define ACC_COUNT ((int)(sizeof(ACC_ITEMS) / sizeof(ACC_ITEMS[0])))

/* 状态区之后的菜单起点，draw_account 与 handle_tap 共用 */
static int acc_top(void) { return g.header_h + g.base * 5 / 2; }
static int acc_row_h(void) {
    int avail = g.H - g.footer_h - acc_top();
    int rh = avail / ACC_COUNT;
    int natural = g.base * 5 / 2;
    if (rh <= 0) rh = natural;
    if (rh > natural) rh = natural;
    return rh;
}

static void draw_account(void) {
    fb_clear(&g.fb, 255);
    draw_header("账号 / 登录", NULL);

    int px = g.base * 3 / 4;
    int y = g.header_h + g.base;
    char st[320];
    if (account_has_login())
        snprintf(st, sizeof(st), "当前：%s", g.acc_user[0] ? g.acc_user : "已登录（Cookie）");
    else
        snprintf(st, sizeof(st), "当前：游客（未登录）");
    text_clip(g.margin, y, px, st, g.W - 2 * g.margin, 0, 1);
    text_clip(g.margin, y + px * 3 / 2, g.base * 3 / 5,
              "扫码登录在 Kindle 上独立完成；Cookie 需先写入文件",
              g.W - 2 * g.margin, 120, 0);

    int rh = acc_row_h();
    int yy = acc_top();
    for (int i = 0; i < ACC_COUNT; i++) {
        if (i == g.qr_sel) fb_fill(&g.fb, 0, yy, g.W, rh, 0);
        int fg = (i == g.qr_sel) ? 255 : 0;
        text_at(g.margin + g.base / 2, yy + rh / 2 + g.base / 3, g.base,
                ACC_ITEMS[i], fg, i == g.qr_sel);
        if (i != g.qr_sel) fb_hline(&g.fb, g.margin, yy + rh - 1, g.W - 2 * g.margin, 220);
        yy += rh;
    }
    footer_begin();
    footer_btn(BTN_BACK, "返回");
    footer_end("点按执行");
    footer_right_text("v" ZH_VERSION);
    present_screen();
}

/* ---------------- 屏幕：二维码扫码登录 ---------------- */

static void draw_qrcode(void) {
    fb_clear(&g.fb, 255);
    draw_header("扫码登录", "返回");

    int spx = g.base * 3 / 5;
    text_at(g.margin, g.header_h + g.base / 2 + spx, spx,
            "用手机知乎 App 扫描屏幕上的二维码", 60, 0);

    int box_x = 2 * g.margin;
    int box_w = g.W - 4 * g.margin;
    int box_top = g.header_h + g.base * 2;
    int box_h = g.H - g.footer_h - box_top - g.base * 5 / 2;
    if (box_h < g.base * 6) box_h = g.base * 6;

    int drawn = 0;
    if (g.qr_link[0] && (g.qr_state == 1 || g.qr_state == 2 || g.qr_state == 3))
        drawn = qr_draw(&g.fb, g.qr_link, box_x, box_top, box_w, box_h);
    if (drawn <= 0) {
        /* 还没取到码 / 屏幕太小画不出可扫的码 */
        fb_rect(&g.fb, box_x, box_top, box_w, box_h, 190);
        const char *hint = drawn == -2
            ? "屏幕太小，二维码无法清晰显示，请改用 Cookie 导入"
            : "（正在准备二维码 / 或取码失败，请返回重试）";
        text_clip(box_x + g.base, box_top + box_h / 2, g.base * 3 / 4, hint,
                  box_w - 2 * g.base, 0, 0);
    }

    int stpx = g.base * 3 / 4;
    int sty = g.H - g.footer_h - g.base * 2;
    text_clip(g.margin, sty, stpx,
              g.qr_state_text[0] ? g.qr_state_text : "等待扫码…",
              g.W - 2 * g.margin, 0, 1);
    footer_begin();
    footer_btn(BTN_BACK, "返回");
    footer_end("扫码后在手机上点「确认登录」");
    present_screen();
}

/* ---------------- 屏幕：私信会话列表 ---------------- */

static int dm_row_h(void) { return g.base * 14 / 5; }
static int dm_rows_per_page(void) {
    int n = (g.H - g.header_h - g.footer_h) / dm_row_h();
    return n > 0 ? n : 1;
}

static void draw_dm_list(void) {
    fb_clear(&g.fb, 255);
    int per = dm_rows_per_page();
    int total = (g.threads.n + per - 1) / per;
    if (total < 1) total = 1;
    int page = g.dm_sel / per;
    if (page >= total) page = total - 1;
    if (g.dm_sel < g.dm_top) g.dm_top = g.dm_sel;
    if (g.dm_sel >= g.dm_top + per) g.dm_top = g.dm_sel - per + 1;
    if (g.dm_top < 0) g.dm_top = 0;

    char right[64];
    snprintf(right, sizeof(right), "%d/%d", page + 1, total);
    draw_header("私信", right);

    int y = g.header_h + 2;
    int rh = dm_row_h();
    for (int i = 0; i < per; i++) {
        int idx = g.dm_top + i;
        if (idx >= g.threads.n) break;
        MsgThread *t = &g.threads.v[idx];
        int sel = (idx == g.dm_sel);
        if (sel) fb_fill(&g.fb, 0, y, g.W, rh, 0);
        int fg = sel ? 255 : 0, fg2 = sel ? 200 : 110;
        int baseline = y + rh / 3;
        text_clip(g.margin, baseline, g.base, t->peer,
                  g.W - 2 * g.margin - g.base * 2, fg, sel);
        if (t->unread > 0) {
            char ub[32];
            snprintf(ub, sizeof(ub), "%d", t->unread);
            int uw = font_width(g.base * 3 / 4, ub, (int)strlen(ub));
            text_at(g.W - g.margin - uw, baseline, g.base * 3 / 4, ub, fg2, 1);
        }
        text_clip(g.margin, y + rh - g.base * 3 / 5, g.base * 3 / 5, t->snippet,
                  g.W - 2 * g.margin, fg2, 0);
        if (!sel) fb_hline(&g.fb, g.margin, y + rh - 1, g.W - 2 * g.margin, 220);
        y += rh;
    }
    if (g.threads.n == 0)
        text_at(g.margin, g.H / 2, g.base, "（暂无会话）", 120, 0);

    char left[128];
    snprintf(left, sizeof(left), "共 %d 个会话 · 点按查看消息", g.threads.n);
    footer_begin();
    footer_btn(BTN_BACK, "返回");
    footer_end(left);
    present_screen();
}

/* 私信消息复用阅读器的整页排版：同一套分页、翻页、进度条，
 * 只是返回目标改成会话列表（在事件路由里靠 SCR_DMREAD 区分）。 */
static void draw_dm_read(void) { draw_reader(); }

/* ---------------- 联网任务（fork 子进程） ---------------- */

static void layout_metrics(void);
static void redraw_current(void);

static long now_ms_ui(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* 子进程干活、父进程等结果：这样联网期间界面依然能响应点击 */
/* 通用 fork 任务：子进程执行 fn（只碰文件/网络，不碰屏幕），
 * 父进程保持界面可响应、可取消、有超时。HTTP / 私信 / 二维码登录都走这里。
 * fn 失败时把错误文本写进 err_path。 */
typedef int (*JobFn)(const char *out_path, const char *err_path, void *arg);
static void job_write_err(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s", text ? text : ""); fclose(f); }
}

static int job_fetch_raw(const char *out_path, const char *err_path, void *arg) {
    const char *path = (const char *)arg;
    Buf body;
    buf_init(&body);
    int rc = api_fetch_raw(path, &body);
    FILE *f = fopen(out_path, "wb");
    if (f) { if (rc == 0 && body.len) fwrite(body.p, 1, body.len, f); fclose(f); }
    job_write_err(err_path, rc == 0 ? "" : api_error());
    logmsg("子进程: rc=%d err=%s len=%zu", rc, rc == 0 ? "-" : api_error(), body.len);
    buf_free(&body);
    return rc;
}
/* 详情抓取放在子进程里做：父进程只负责显示。
 * 之所以不让父进程直接调 api_*：一次详情要发 1~2 个请求（失败会退到
 * 回答列表），期间界面必须还能响应取消。
 *
 * 输出格式（NUL 分隔 5 段）：
 *   title \0 author \0 voteup \0 created \0 html
 * 1.2.5 只有 title/author/html，赞同数和发布时间虽然接口里带回来了，
 * 却在写盘时被丢掉，正文顶部于是什么都没有。 */
typedef struct {
    char id[64];
    char qid[64];
    int  type;      /* 1=回答 2=文章 */
    /* 1.2.7 起详情只取正文：图片改成打开之后由后台进程逐张补，
     * 这几个字段保留只为二进制结构稳定（fork 传参），不再参与下载。 */
    int  want_img;
    int  max_w;
    int  max_h;
} DetailArg;

static int job_detail(const char *out_path, const char *err_path, void *arg) {
    DetailArg *d = (DetailArg *)arg;
    ApiContent c;
    memset(&c, 0, sizeof(c));
    /* api_*_detail2 内部自带 Referer 与 404 回退 */
    int rc = (d->type == 2) ? api_article_detail2(d->id, &c)
                            : api_answer_detail2(d->id, d->qid, &c);
    if (rc != 0) {
        job_write_err(err_path, api_error());
        logmsg("子进程: 详情 id=%s qid=%s type=%d 失败: %s（url=%s status=%d body=%s）",
               d->id, d->qid, d->type, api_error(), api_last_url(),
               api_last_status(), api_last_head());
        api_content_free(&c);
        return -1;
    }
    const char *t = c.title ? c.title : "";
    const char *a = c.author ? c.author : "";
    const char *h = c.html ? c.html : "";

    /* 1.2.7：这里只拿正文，绝不等图片。图片由 open_doc_from_html 之后
     * 的 img_start() 在另一个子进程里按顺序补（先出字，后补图）。 */

    FILE *f = fopen(out_path, "wb");
    if (!f) { api_content_free(&c); job_write_err(err_path, "写临时文件失败"); return -1; }
    char num[32];
    fwrite(t, 1, strlen(t), f); fputc(0, f);
    fwrite(a, 1, strlen(a), f); fputc(0, f);
    snprintf(num, sizeof(num), "%ld", c.voteup);
    fwrite(num, 1, strlen(num), f); fputc(0, f);
    snprintf(num, sizeof(num), "%ld", c.created);
    fwrite(num, 1, strlen(num), f); fputc(0, f);
    fwrite(h, 1, strlen(h), f);
    fclose(f);
    logmsg("子进程: 详情 ok id=%s type=%d 标题=%s 赞同=%ld 时间=%ld 正文=%zu 字节",
           d->id, d->type, t, c.voteup, c.created, strlen(h));
    api_content_free(&c);
    return 0;
}

static int job_threads(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    int rc = social_threads_dump(out_path, 0);
    job_write_err(err_path, rc == 0 ? "" : api_error());
    return rc;
}
typedef struct {
    char url[256];
    char id[64];
} MsgArg;

static int job_messages(const char *out_path, const char *err_path, void *arg) {
    MsgArg *a = (MsgArg *)arg;
    int rc = social_messages_dump2(out_path, a->url, a->id, 0);
    job_write_err(err_path, rc == 0 ? "" : api_error());
    return rc;
}

/* ---- 二维码登录：取码 + 轮询 ----
 * 取码走 run_job（显示加载页，几秒后可取消）；轮询不能切屏，否则二维码
 * 会被加载页盖掉，所以由主循环里的 qr_tick() 单独 fork 一个短子进程完成。 */
static int job_qr_begin(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    QrStateFile st;
    memset(&st, 0, sizeof(st));
    /* 失败原因写在 st.q.err / st.q.state 里，不算 job 失败 */
    account_qr_begin(&st.q);
    snprintf(st.session, sizeof(st.session), "%s", account_session_get());
    FILE *f = fopen(out_path, "wb");
    if (f) { fwrite(&st, sizeof(st), 1, f); fclose(f); }
    job_write_err(err_path, "");
    logmsg("qr: begin state=%d err=%s", st.q.state, st.q.err);
    return 0;
}

/* 轮询子进程：恢复会话 cookie → 轮询 → 把新状态与会话写回 */
static void qr_poll_child(void) {
    QrStateFile st;
    memset(&st, 0, sizeof(st));
    FILE *f = fopen(g.qr_path, "rb");
    if (f) {
        if (fread(&st, sizeof(st), 1, f) != 1) memset(&st, 0, sizeof(st));
        fclose(f);
    }
    account_session_set(st.session);
    if (st.q.token[0]) account_qr_poll(&st.q);
    snprintf(st.session, sizeof(st.session), "%s", account_session_get());
    FILE *o = fopen(g.qr_path, "wb");
    if (o) { fwrite(&st, sizeof(st), 1, o); fclose(o); }
}

static int qr_read_file(QrStateFile *st) {
    memset(st, 0, sizeof(*st));
    FILE *f = fopen(g.qr_path, "rb");
    if (!f) return -1;
    int ok = (fread(st, sizeof(*st), 1, f) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

/* ---- 登录类任务：Cookie 导入 / 登录态校验 / 端点诊断 ---- */
static int job_cookie(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    char path[320];
    snprintf(path, sizeof(path), "%s/cookie.txt", zh_home());
    size_t n = 0;
    char *raw = read_file(path, &n);
    LoginResult r;
    memset(&r, 0, sizeof(r));
    if (raw && *raw)
        r.rc = account_import_cookie(raw, r.user, sizeof(r.user), r.err, sizeof(r.err));
    else {
        r.rc = -1;
        snprintf(r.err, sizeof(r.err), "cookie.txt 为空或不存在");
    }
    free(raw);
    FILE *f = fopen(out_path, "wb");
    if (f) { fwrite(&r, sizeof(r), 1, f); fclose(f); }
    job_write_err(err_path, "");
    return 0;
}

static int job_me(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    LoginResult r;
    memset(&r, 0, sizeof(r));
    r.rc = account_me(r.user, sizeof(r.user), r.err, sizeof(r.err));
    FILE *f = fopen(out_path, "wb");
    if (f) { fwrite(&r, sizeof(r), 1, f); fclose(f); }
    job_write_err(err_path, "");
    return 0;
}

static int job_probe(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    social_probe_to(out_path);
    job_write_err(err_path, "");
    return 0;
}

/* ---- 二维码轮询调度（主循环每 500ms 调一次，全程不阻塞 UI） ---- */
static void qr_stop(void) {
    if (g.qr_pid > 0) {
        int st = 0;
        kill(g.qr_pid, SIGKILL);
        waitpid(g.qr_pid, &st, 0);
        g.qr_pid = 0;
    }
}

static char g_qr_drawn_text[256];

static void qr_apply_state(void) {
    QrStateFile st;
    if (qr_read_file(&st) != 0) {
        g.qr_state = -1;
        snprintf(g.qr_state_text, sizeof(g.qr_state_text), "状态读取失败，请返回重试");
        draw_qrcode();
        return;
    }
    g.qr_state = st.q.state;
    snprintf(g.qr_state_text, sizeof(g.qr_state_text), "%s",
             st.q.err[0] ? st.q.err : (g.qr_state == 2 ? "已扫码，请在手机上确认" : "等待扫码…"));
    if (g.qr_state == 3) {
        snprintf(g.acc_user, sizeof(g.acc_user), "%s", st.q.user[0] ? st.q.user : "已登录");
        api_load_cookie();   /* 子进程已把 z_c0 写进 cookie.txt */
        snprintf(g.qr_state_text, sizeof(g.qr_state_text),
                 "登录成功：%s。点任意位置返回账号页", g.acc_user);
        logmsg("qr: 扫码登录成功 %s", g.acc_user);
    }
    /* 状态文字没变就不重绘：电子墨水每刷一次都闪一下，等扫码期间别狂闪 */
    if (strcmp(g_qr_drawn_text, g.qr_state_text) != 0 || g.qr_state == 3 || g.qr_state < 0) {
        snprintf(g_qr_drawn_text, sizeof(g_qr_drawn_text), "%s", g.qr_state_text);
        draw_qrcode();
    }
}

static void qr_tick(void) {
    if (g.qr_demo) return;   /* 模拟器离线二维码，别去轮询真接口 */
    if (g.scr != SCR_QRCODE) return;
    if (g.qr_state != 1 && g.qr_state != 2) return;
    long now = now_ms_ui();
    if (g.qr_pid > 0) {
        int st = 0;
        pid_t w = waitpid(g.qr_pid, &st, WNOHANG);
        if (w == g.qr_pid) {
            g.qr_pid = 0;
            qr_apply_state();
        } else if (now - g.qr_last_ms > 20000) {
            qr_stop();
            g.qr_state = -1;
            snprintf(g.qr_state_text, sizeof(g.qr_state_text),
                     "轮询超时。请返回后重试，或改用「导入 Cookie」");
            draw_qrcode();
        }
        return;
    }
    if (now - g.qr_last_ms < 2500) return;
    g.qr_last_ms = now;
    pid_t pid = fork();
    if (pid == 0) {
        qr_poll_child();
        _exit(0);
    }
    g.qr_pid = pid > 0 ? pid : 0;
}

static int run_job(JobFn fn, void *arg, const char *title, const char *tag,
                   char *out_path, size_t outsz, char *err, size_t errsz,
                   int timeout_s) {
    snprintf(out_path, outsz, "%s/%s.out", zh_home(), tag);
    char ep[320];
    snprintf(ep, sizeof(ep), "%s/%s.err", zh_home(), tag);
    unlink(out_path);
    unlink(ep);

    show_loading(title, 1);
    pid_t pid = fork();
    if (pid == 0) {
        int rc = fn(out_path, ep, arg);
        _exit(rc == 0 ? 0 : 1);
    }
    if (pid < 0) { snprintf(err, errsz, "无法创建网络子进程"); return -1; }

    int status = 0, last_tick = 0;
    long limit = (long)timeout_s * 1000;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        /* 收到退出信号（SIGTERM 之类）：别让用户再等 20 秒网络超时，
         * 立刻收摊走正常退出路径，否则系统界面还原会被拖住 */
        if (app_quit_signal()) {
            kill(pid, SIGKILL); waitpid(pid, &status, 0);
            g.quit = 1;
            snprintf(err, errsz, "已退出");
            return -1;
        }
        Ev ev;
        if (input_poll(&ev, 150)) {
            int in_cancel = ev.type == ZH_TAP && ev.x >= g_cancel_x && ev.x < g_cancel_x + g_cancel_w &&
                            ev.y >= g_cancel_y && ev.y < g_cancel_y + g_cancel_h;
            if (in_cancel) {
                kill(pid, SIGKILL); waitpid(pid, &status, 0);
                snprintf(err, errsz, "已取消");
                return -1;
            }
        }
        long el = now_ms_ui() - g.load_start;
        if (el > 6000 && last_tick != 1) { last_tick = 1; draw_loading(); }
        if (el > limit) {
            kill(pid, SIGKILL); waitpid(pid, &status, 0);
            snprintf(err, errsz, "网络超时");
            return -1;
        }
    }

    /* 子进程可能刚拿到 d_c0 / z_c0，父进程重新读一次 cookie */
    api_load_cookie();

    size_t n = 0;
    char *e3 = read_file(ep, &n);
    if (e3) {
        if (n > 0) {
            snprintf(err, errsz, "%s", str_trim(e3));
            free(e3);
            return -1;
        }
        free(e3);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!err[0]) snprintf(err, errsz, "请求失败");
        return -1;
    }
    return 0;
}

static int fetch_job(const char *path, const char *title, char *out_path, size_t outsz,
                     char *err, size_t errsz) {
    return run_job(job_fetch_raw, (void *)path, title, "resp",
                   out_path, outsz, err, errsz, 20);
}

/* ---------------- 列表填充 ---------------- */

static void list_reset(const char *title, const char *kind) {
    api_list_free(&g.list);
    snprintf(g.list_title, sizeof(g.list_title), "%s", title ? title : "");
    snprintf(g.list_kind, sizeof(g.list_kind), "%s", kind ? kind : "");
    g.list_msg[0] = 0;
    g.list_sel = 0;
    g.list_top = 0;
    g.list_page = 0;
}

/* 列表空态的统一说明：把「为什么空」和「下一步」讲清楚。
 * 有具体错误就优先用错误（它信息量最大），未登录时再补一句登录建议。 */
static void list_set_empty_msg(const char *fallback) {
    char base[300];
    if (api_error()[0]) snprintf(base, sizeof(base), "原因：%s", api_error());
    else if (fallback)  snprintf(base, sizeof(base), "%s", fallback);
    else                snprintf(base, sizeof(base), "接口返回空列表，稍后再试。");

    if (!account_has_login()) {
        /* 游客态下热榜/推荐本来就容易被风控成空，这句必须带上，
         * 否则用户只会反复点同一个条目。 */
        snprintf(g.list_msg, sizeof(g.list_msg),
                 "%s\n未登录会明显更容易被风控，建议到「账号 / 登录」扫码或导入 Cookie。", base);
    } else {
        snprintf(g.list_msg, sizeof(g.list_msg), "%s", base);
    }
}

static void show_loading(const char *text, int cancellable) {
    snprintf(g.load_text, sizeof(g.load_text), "%s", text);
    g.load_start = now_ms_ui();
    g.load_cancellable = cancellable;
    g.scr = SCR_LOADING;
    draw_loading();
}

static void show_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.msg, sizeof(g.msg), fmt, ap);
    va_end(ap);
    /* 加载页/消息页本身不作为返回目标：网络任务失败时 g.scr 仍是
     * SCR_LOADING，若把它记成 scr_prev，关掉提示后会停在一个「画着首页、
     * 却按 SCR_LOADING 处理」的点不动状态。 */
    if (g.scr != SCR_MESSAGE && g.scr != SCR_LOADING) g.scr_prev = g.scr;
    g.scr = SCR_MESSAGE;
    g.msg_until = 0;          /* 新的提示：取消上一次的自动关闭计时 */
    draw_message();
}

/* ---------------- 阅读器打开 ---------------- */

static void history_add(int type, const char *id, const char *qid, const char *title) {
    char path[320];
    snprintf(path, sizeof(path), "%s/history.txt", zh_home());
    /* 读旧记录，去重后写回，最多 30 条 */
    char *old = read_file(path, NULL);
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%d\t%s\t%s\t%s\n", type, id ? id : "", qid ? qid : "", title ? title : "");
    int kept = 0;
    if (old) {
        char *save = NULL;
        for (char *line = strtok_r(old, "\n", &save); line && kept < 29;
             line = strtok_r(NULL, "\n", &save)) {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *t1 = strchr(tmp, '\t');
            if (!t1) continue;
            char *t2 = strchr(t1 + 1, '\t');
            if (!t2) continue;
            char *t3 = strchr(t2 + 1, '\t');
            if (!t3) continue;
            *t1 = 0;
            *t2 = 0;
            *t3 = 0;
            const char *oid = t2 + 1;
            if (id && strcmp(oid, id) == 0) continue;
            buf_add(&b, line, strlen(line));
            buf_addc(&b, '\n');
            kept++;
        }
        free(old);
    }
    write_file_atomic(path, b.p, b.len);
    buf_free(&b);
}

/* 读历史成列表 */
static void history_load(ApiList *out) {
    char path[320];
    snprintf(path, sizeof(path), "%s/history.txt", zh_home());
    char *s = read_file(path, NULL);
    if (!s) return;
    char *save = NULL;
    for (char *line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p1 = strchr(line, '\t');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, '\t');
        if (!p2) continue;
        char *p3 = strchr(p2 + 1, '\t');
        if (!p3) continue;
        *p1 = 0; *p2 = 0; *p3 = 0;

        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 32;
            out->v = xrealloc(out->v, sizeof(ApiItem) * (size_t)out->cap);
        }
        ApiItem *it = &out->v[out->n++];
        memset(it, 0, sizeof(*it));
        it->type = atoi(line);
        snprintf(it->id, sizeof(it->id), "%s", p1 + 1);
        snprintf(it->qid, sizeof(it->qid), "%s", p2 + 1);
        snprintf(it->title, sizeof(it->title), "%s", p3 + 1);
        if (!it->title[0]) snprintf(it->title, sizeof(it->title), "（已删除）");
        snprintf(it->sub, sizeof(it->sub), "历史记录");
    }
    free(s);
}

static void open_history(void) {
    list_reset("阅读历史", "history");
    history_load(&g.list);
    if (g.list.n == 0) {
        snprintf(g.list_msg, sizeof(g.list_msg),
                 "还没有阅读记录。打开任意热榜条目或回答后，会自动记在这里。");
    }
    g.scr = SCR_LIST;
    draw_list();
}

/* 把一个块插到文档最前面（元信息栏要用：先解析正文，再补一行头） */
static void doc_insert_front(Doc *d, const char *text, int kind) {
    if (!text || !*text) return;
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->v = xrealloc(d->v, sizeof(Blk) * (size_t)d->cap);
    }
    memmove(d->v + 1, d->v, sizeof(Blk) * (size_t)d->n);
    d->v[0].text = xstrdup(text);
    d->v[0].kind = (uint8_t)kind;
    d->n++;
}

/* 把 BLK_IMAGE 里的"地址下标"换成三种形态之一：
 *   已缓存 -> "缓存路径\t宽\t高"（直接贴图）
 *   未缓存 -> "\x01下标\t宽\t占位高"（自动模式：后台从前往后补）
 *           -> "\x02下标\t宽\t占位高"（点按模式：等人点）
 * 打开文章时**绝不**同步下载图片：以前 8 张图串行下完才写结果文件，
 * 正文要等几十秒才出来，用户感受就是"点开文章变慢了"。 */
static int doc_images_prepare(Doc *d, ImgRefs *refs, int text_w, int ph_h, int tap) {
    int pending = 0;
    for (int i = 0; i < d->n; i++) {
        Blk *b = &d->v[i];
        if (b->kind != BLK_IMAGE) continue;
        int idx = atoi(b->text ? b->text : "");
        char path[512];
        int iw = 0, ih = 0;
        free(b->text);
        b->text = NULL;
        if (idx >= 0 && idx < refs->n &&
            img_cache_fill(refs->v[idx], IMG_CACHE_W, path, sizeof(path), &iw, &ih) == 0 &&
            ih > 0) {
            char buf[640];
            snprintf(buf, sizeof(buf), "%s\t%d\t%d", path, iw, ih);
            b->text = xstrdup(buf);
        } else {
            char buf[640];
            snprintf(buf, sizeof(buf), tap ? "\x02" "%d\t%d\t%d" : "\x01" "%d\t%d\t%d",
                     idx, text_w, ph_h);
            b->text = xstrdup(buf);
            if (!tap) pending++;
        }
    }
    return pending;
}

/* 后台下完一张就调用一次：把已经能读到缓存的占位改成真图。
 * 返回这次新变成"可显示"的张数；高度有变化时置 img_dirty 让阅读页重排。 */
static int doc_images_apply(void) {
    int got = 0, img_changed = 0;
    for (int i = 0; i < g.doc.n; i++) {
        Blk *b = &g.doc.v[i];
        if (b->kind != BLK_IMAGE || !b->text) continue;
        if (b->text[0] != '\x01' && b->text[0] != '\x02') continue;
        int idx = atoi(b->text + 1);
        if (idx < 0 || idx >= g.doc_refs.n) continue;
        char path[512];
        int iw = 0, ih = 0;
        if (img_cache_fill(g.doc_refs.v[idx], IMG_CACHE_W, path, sizeof(path), &iw, &ih) != 0 ||
            ih <= 0)
            continue;
        char buf[640];
        snprintf(buf, sizeof(buf), "%s\t%d\t%d", path, iw, ih);
        /* 占位高和真图高不同 -> 页数会变，必须重排 */
        const char *tab = strrchr(b->text, '\t');
        int oldh = tab ? atoi(tab + 1) : 0;
        if (oldh != ih) img_changed = 1;
        free(b->text);
        b->text = xstrdup(buf);
        got++;
    }
    if (img_changed) g.img_dirty = 1;
    return got;
}

/* 自动模式全部试完还剩下的（下载/解码失败）：退成一行文字占位，别让阅读页
 * 里永远挂着一个"加载中"的框。点按模式的占位**不动**——那个框本身就是要
 * 让人点的，退成文字之后用户连重试的机会都没有了。 */
static void doc_images_give_up(void) {
    int n = 0;
    for (int i = 0; i < g.doc.n; i++) {
        Blk *b = &g.doc.v[i];
        if (b->kind != BLK_IMAGE || !b->text || b->text[0] != '\x01') continue;
        free(b->text);
        b->text = xstrdup("[图片]（未能加载，可在设置里改成「点按加载」重试）");
        b->kind = BLK_PARA;
        n++;
    }
    if (n) {
        g.img_dirty = 1;
        logmsg("图片: %d 张最终没能加载，退回文字占位", n);
    }
}

/* 单张图：下 → 解 → 存。点按模式用它，也用来给"自动模式里失败的那张"兜底。 */
typedef struct {
    char url[1024];
    char referer[512];
    int  max_h;
} ImgOneArg;

static int job_img_one(const char *out_path, const char *err_path, void *arg) {
    ImgOneArg *a = (ImgOneArg *)arg;
    (void)out_path;
    char path[512];
    int iw = 0, ih = 0;
    if (img_cache_fill(a->url, IMG_CACHE_W, path, sizeof(path), &iw, &ih) == 0) return 0;

    Buf bin;
    buf_init(&bin);
    if (api_fetch_bin(a->url, a->referer, &bin) != 0) {
        job_write_err(err_path, api_error());
        buf_free(&bin);
        return -1;
    }
    ImgGray g;
    char e[128] = "";
    if (img_decode_gray((const uint8_t *)bin.p, bin.len, IMG_CACHE_W, a->max_h,
                        &g, e, sizeof(e)) != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "解码失败：%s（%zu 字节）", e, bin.len);
        job_write_err(err_path, msg);
        buf_free(&bin);
        return -1;
    }
    int rc = img_cache_save(a->url, &g);
    img_free(&g);
    buf_free(&bin);
    if (rc != 0) {
        job_write_err(err_path, "缓存写入失败（检查 /mnt/us 空间）");
        return -1;
    }
    /* 同上：存完按查询键回读一次，确认真能取到 */
    if (img_cache_fill(a->url, IMG_CACHE_W, path, sizeof(path), &iw, &ih) != 0) {
        job_write_err(err_path, "缓存回读失败（文件名与查询键不一致）");
        return -1;
    }
    logmsg("图片: 单张 ok %s", a->url);
    return 0;
}

/* ---------------- 图片三档模式 ----------------
 * 0 关闭    —— 正文里只留「[图片]」一行字，最快、最省流量；
 * 1 点按    —— 排版时留一个「点按加载此图」的框，点哪张下哪张；
 * 2 自动    —— 文字先出来，图片在后台从正文第一张开始逐张补，补上就刷新。
 * 真机上"图一直出不来"时，点按模式还有个好处：失败原因会直接弹在屏幕上
 * （HTTP 状态码 / 解码错），不用再翻 log.txt。 */
static void img_load_one(int idx) {
    if (idx < 0 || idx >= g.doc_refs.n) return;
    /* 模拟器：直接塞一张测试图，把"点按 → 就地显示"这条链路跑通（真跑网络
     * 在离线模拟器里只会拿到失败提示，演示不出效果）。 */
    if (getenv("ZH_SIM")) {
        sim_seed_image(g.doc_refs.v[idx]);
        if (doc_images_apply() > 0) reader_relayout_keep();
        g.img_dirty = 0;
        draw_reader();
        return;
    }
    ImgOneArg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.url, sizeof(a.url), "%s", g.doc_refs.v[idx]);
    snprintf(a.referer, sizeof(a.referer), "%s", g.img_referer);
    a.max_h = g.H - reader_header_h() - g.footer_h;

    char out[320], err[256] = "";
    if (run_job(job_img_one, &a, "正在加载图片…", "imgone",
                out, sizeof(out), err, sizeof(err), 30) != 0) {
        if (strcmp(err, "已取消") == 0) { draw_reader(); return; }
        logmsg("图片: 点按加载失败 %s（%s）", a.url, err);
        /* 1.2.8 的坑：show_loading 把 g.scr 换成了 SCR_LOADING，
         * show_message 记返回目标时发现是 LOADING 就不更新 scr_prev，
         * 于是"点掉错误提示"会回到上上次停留的界面（往往是首页）。
         * 这里显式把返回目标钉回阅读页。 */
        g.scr_prev = (g.scr_prev == SCR_DMREAD) ? SCR_DMREAD : SCR_READER;
        show_message("这张图片没有加载成功。\n\n原因：%.160s\n\n"
                     "点屏幕任意位置回到文章。\n"
                     "可以把这句话发给我；也可以在 KUAL 的\n"
                     "「接口诊断」里看同一个地址的状态码。", err[0] ? err : "未知");
        return;
    }
    if (doc_images_apply() > 0) {
        reader_relayout_keep();
    }
    g.img_dirty = 0;
    draw_reader();
}

/* 点到阅读页上的某个图片占位框了吗？返回 refs 下标，-1 表示没点到。
 * 1.2.9：页几何与 draw_reader 一致（第一页有标题页眉、后续页没有）。 */
static int reader_img_hit(int x, int y, int *is_tap) {
    if (!g.lay.pages || g.lay.npages <= 0 || g.page < 0 || g.page >= g.lay.npages) return -1;
    int top = (g.page == 0) ? reader_header_h() + reader_pad() : reader_top_n();
    int page_top = layout_page_top(&g.lay, g.page);
    int i0 = g.lay.pages[g.page];
    int i1 = (g.page + 1 <= g.lay.npages) ? g.lay.pages[g.page + 1] : g.lay.n;
    for (int i = i0; i < i1 && i < g.lay.n; i++) {
        Line *ln = &g.lay.lines[i];
        if (ln->kind != BLK_IMAGE || !ln->s) continue;
        if (ln->s[0] != '\x01' && ln->s[0] != '\x02') continue;
        int ytop = top + (ln->y - page_top);
        if (y < ytop || y >= ytop + ln->px) continue;
        if (is_tap) *is_tap = (ln->s[0] == '\x02');
        return atoi(ln->s + 1);
    }
    return -1;
}

/* ---- 后台图片下载进程 ----
 * 单独 fork 一个子进程按顺序下（从正文第一张开始），每下完一张写一行
 * 进度文件。父进程每 200ms 看一眼，有变化就替换占位并刷新当前页。
 * 不放在主进程里做，是因为 Kindle 上一次 TLS 握手可能就要好几秒，
 * 期间界面必须还能翻页、还能返回。 */
static void img_child(ImgRefs *refs, const char *referer, int max_h, const char *stat) {
    int n = refs->n;
    for (int i = 0; i < n; i++) {
        int ow = 0, oh = 0;
        char path[512];
        int ok = (img_cache_fill(refs->v[i], IMG_CACHE_W, path, sizeof(path), &ow, &oh) == 0);
        if (!ok) {
            Buf bin;
            buf_init(&bin);
            if (api_fetch_bin(refs->v[i], referer, &bin) == 0) {
                ImgGray g;
                char err[96] = "";
                if (img_decode_gray((const uint8_t *)bin.p, bin.len, IMG_CACHE_W, max_h,
                                    &g, err, sizeof(err)) == 0) {
                    /* 存完立刻按"查缓存"的路子回读一次：文件名里的宽度一旦
                     * 和查询用的宽度对不上（1.2.7 的坑），这里就能当场发现，
                     * 不至于"下载成功、界面永远占位"。 */
                    if (img_cache_save(refs->v[i], &g) == 0) {
                        char vp[512];
                        int vw = 0, vh = 0;
                        if (img_cache_fill(refs->v[i], IMG_CACHE_W, vp, sizeof(vp), &vw, &vh) == 0)
                            ok = 1;
                        else
                            logmsg("图片[%d/%d] 存盘后回读失败（文件名与查询键不一致）", i + 1, n);
                    }
                    img_free(&g);
                } else {
                    logmsg("图片[%d/%d] 解码失败 %s（%s，%zu 字节）",
                           i + 1, n, refs->v[i], err, bin.len);
                }
            } else {
                logmsg("图片[%d/%d] 下载失败 %s（%s）", i + 1, n, refs->v[i], api_error());
            }
            buf_free(&bin);
        }
        FILE *f = fopen(stat, "w");
        if (f) {
            fprintf(f, "%d %d\n", i + 1, n);
            fclose(f);
        }
    }
}

static void img_stop(void) {
    if (g.img_pid > 0) {
        int st = 0;
        kill(g.img_pid, SIGKILL);
        waitpid(g.img_pid, &st, 0);
        g.img_pid = 0;
    }
    g.img_total = g.img_done = 0;
    g.img_dirty = 0;
    img_cache_set_article(NULL);   /* 离开文章：缓存回到平铺目录约定 */
}

/* 启动后台下载；没有待下的图就什么都不做 */
static void img_start(const char *referer, int max_h) {
    img_stop();
    if (g.doc_refs.n <= 0) return;
    int pending = 0;
    for (int i = 0; i < g.doc.n; i++)
        if (g.doc.v[i].kind == BLK_IMAGE && g.doc.v[i].text && g.doc.v[i].text[0] == '\x01')
            pending++;
    if (pending == 0) return;

    snprintf(g.img_stat, sizeof(g.img_stat), "%s/img.stat", zh_home());
    unlink(g.img_stat);
    snprintf(g.img_referer, sizeof(g.img_referer), "%s", referer ? referer : "");
    g.img_total = pending;
    g.img_done = 0;

    /* 模拟器专用：延迟 N 毫秒后把测试图塞进缓存，用来验证"占位 → 真图"
     * 这条异步链路（真机上没法控制图片什么时候到）。 */
    const char *delay = getenv("ZH_SIM_IMG_DELAY");
    if (delay && atoi(delay) > 0) {
        pid_t sp = fork();
        if (sp == 0) {
            usleep((useconds_t)atoi(delay) * 1000);
            if (g.doc_refs.n > 0) sim_seed_image(g.doc_refs.v[0]);
            FILE *sf = fopen(g.img_stat, "w");
            if (sf) { fprintf(sf, "1 1\n"); fclose(sf); }
            _exit(0);
        }
        g.img_pid = sp > 0 ? sp : 0;
        logmsg("sim: 图片延迟 %dms 到货（验证异步占位）", atoi(delay));
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        img_child(&g.doc_refs, g.img_referer, max_h, g.img_stat);
        _exit(0);
    }
    g.img_pid = pid > 0 ? pid : 0;
    logmsg("图片: 后台开始下载 %d 张（按正文顺序）", pending);
}

/* 主循环每 200ms 调一次：读进度、替换占位、必要时刷新当前页 */
static void img_tick(void) {
    if (g.img_pid <= 0) return;
    int done = 0, total = 0;
    FILE *f = fopen(g.img_stat, "r");
    if (f) {
        if (fscanf(f, "%d %d", &done, &total) != 2) { done = 0; total = 0; }
        fclose(f);
    }
    if (done > g.img_done && total > 0) {
        int had_pending_here = 0;
        /* 当前页上有占位吗？有才值得刷屏（没图到"眼前"就先攒着） */
        if (g.lay.pages && g.lay.npages > 0 && g.page >= 0 && g.page < g.lay.npages) {
            for (int i = g.lay.pages[g.page]; i < g.lay.pages[g.page + 1]; i++) {
                Line *ln = &g.lay.lines[i];
                if (ln->kind == BLK_IMAGE && ln->s && ln->s[0] == '\x01') { had_pending_here = 1; break; }
            }
        }
        g.img_done = done;
        int got = doc_images_apply();
        logmsg("图片: 进度 %d/%d，本次新就绪 %d 张%s", done, total, got,
               had_pending_here ? "（当前页有图，刷新）" : "");
        if (had_pending_here && got > 0 &&
            (g.scr == SCR_READER || g.scr == SCR_DMREAD)) {
            draw_reader();
        }
    }
    int st = 0;
    if (waitpid(g.img_pid, &st, WNOHANG) == g.img_pid) {
        g.img_pid = 0;
        int got = doc_images_apply();
        int left = 0;
        for (int i = 0; i < g.doc.n; i++)
            if (g.doc.v[i].kind == BLK_IMAGE && g.doc.v[i].text && g.doc.v[i].text[0] == '\x01')
                left++;
        if (left > 0) doc_images_give_up();
        logmsg("图片: 后台结束，新就绪 %d 张，剩余未加载 %d 张", got, left);
        g.img_total = g.img_done = 0;
        if (g.scr == SCR_READER || g.scr == SCR_DMREAD) draw_reader();
    }
}

static void open_doc_from_html(const char *title, const char *author, const char *html,
                               int type, const char *id, const char *qid,
                               long voteup, long created) {
    img_stop();
    imgrefs_free(&g.doc_refs);
    imgrefs_init(&g.doc_refs);

    doc_free(&g.doc);
    doc_init(&g.doc);

    int text_w = g.W - 2 * g.margin;
    int page_h = g.H - reader_header_h() - g.footer_h - 2 * reader_pad() - g.base / 3;
    /* 占位框高度先按 3:2 估一张横图；真图到货后按实际高度重排并锚定原位置。
     * 估得太高会让"还没图"的文章空一大片，估得太矮又会让真图一到就整页跳。 */
    int ph = text_w * 2 / 3;
    int cap = page_h * 3 / 4;
    if (cap > 0 && ph > cap) ph = cap;
    if (ph < g.base * 3) ph = g.base * 3;

    /* 三档：0 无图 / 1 点按加载 / 2 自动异步 */
    if (g_cfg.images < 0 || g_cfg.images > 2) g_cfg.images = 0;
    g.img_mode = g_cfg.images;
    int tap = (g_cfg.images == 1);
    int mode = g_cfg.images ? IMG_REAL : IMG_NONE;

    /* 1.2.9：图片缓存按文章分目录——key 用问题 id / 专栏 id（没有就用标题），
     * 同一篇文章的图都在一个夹子里，第二次打开直接读盘，瞬时显示。 */
    {
        char akey[256];
        if (qid && *qid)          snprintf(akey, sizeof(akey), "q:%s", qid);
        else if (type == 2 && id) snprintf(akey, sizeof(akey), "p:%s", id);
        else                      snprintf(akey, sizeof(akey), "t:%s", title ? title : "");
        img_cache_set_article(akey);
        logmsg("图片: 文章缓存目录 key=%s", akey);
    }

    char ref[512];
    if (type == 2) snprintf(ref, sizeof(ref), "https://zhuanlan.zhihu.com/p/%s", id ? id : "");
    else if (qid && *qid) snprintf(ref, sizeof(ref), "https://www.zhihu.com/question/%s", qid);
    else snprintf(ref, sizeof(ref), "https://www.zhihu.com/");
    snprintf(g.img_referer, sizeof(g.img_referer), "%s", ref);

    html_to_doc_img(html, &g.doc, mode, &g.doc_refs);
    int pending = 0;
    if (mode == IMG_REAL) {
        pending = doc_images_prepare(&g.doc, &g.doc_refs, text_w, ph, tap);
    } else {
        imgrefs_free(&g.doc_refs);
        imgrefs_init(&g.doc_refs);
    }

    if (g.doc.n == 0) {
        doc_add(&g.doc, "（没有可显示的正文）", BLK_PARA);
    }

    /* 顶部元信息：作者 · 赞同 N · 发布于 YYYY-MM-DD
     * 这些字段接口本来就返回，1.2.5 之前解析出来又丢掉，读正文时完全
     * 不知道作者是谁、多少人赞同、什么时候写的。 */
    char meta[320];
    char date[32];
    api_format_date(created, date, sizeof(date));
    meta[0] = 0;
    if (author && *author) snprintf(meta, sizeof(meta), "%s", author);
    if (voteup > 0) {
        char t[64];
        snprintf(t, sizeof(t), "%s%d 赞同", meta[0] ? " · " : "", (int)voteup);
        strncat(meta, t, sizeof(meta) - strlen(meta) - 1);
    }
    if (date[0]) {
        char t[64];
        snprintf(t, sizeof(t), "%s发布于 %s", meta[0] ? " · " : "", date);
        strncat(meta, t, sizeof(meta) - strlen(meta) - 1);
    }
    if (meta[0]) doc_insert_front(&g.doc, meta, BLK_META);

    snprintf(g.doc_title, sizeof(g.doc_title), "%s", title ? title : "");
    snprintf(g.doc_author, sizeof(g.doc_author), "%s", author ? author : "");
    reader_layout();
    g.page = 0;
    g.scr = SCR_READER;
    g.reading = 1;

    history_add(type, id, qid, g.doc_title);
    draw_reader();

    /* 正文先上屏（这一步是毫秒级），图片交给后台从第一张开始补。
     * 1.2.6 是在子进程里"下完 8 张图再写结果"，一篇文章要等十几秒才出字。 */
    if (pending > 0) img_start(ref, page_h);
}

/* ---------------- 首页动作 ---------------- */

static void act_hot(void) {
    char json[320];
    char err[256];
    if (fetch_job("/api/v3/feed/topstory/hot-list-web?limit=50&desktop=true", "正在获取热榜…",
                  json, sizeof(json), err, sizeof(err)) != 0) {
        show_message("%s", err);
        return;
    }
    size_t n = 0;
    char *body = read_file(json, &n);
    if (!body) { show_message("读取响应失败"); return; }
    list_reset("热榜", "hot");
    int rc = api_parse_feed(body, n, &g.list);
    free(body);
    if (rc != 0) { show_message("%s", api_error()); return; }
    if (g.list.n == 0) list_set_empty_msg("热榜接口返回空列表。");
    g.scr = SCR_LIST;
    draw_list();
}

/* 推荐流：一页按服务器给的量回（真机实测 6 条），所以必须连续翻页。
 * 1.2.6 用 page_number=0/1/2 硬翻，三页给的是同一批内容，去重后还是 6 条；
 * 现在改成照响应里的 paging.next 走——那是服务端自己给的续页地址，
 * 带上游标，翻出来才是新内容。 */
#define REC_PAGES 5
typedef struct { int pages; } RecArg;

static int job_recommend(const char *out_path, const char *err_path, void *arg) {
    RecArg *r = (RecArg *)arg;
    ApiList l;
    memset(&l, 0, sizeof(l));
    for (int p = 0; p < r->pages; p++) {
        int before = l.n;
        int rc;
        if (p == 0) rc = api_recommend(&l, 0, g_cfg.rec);
        else {
            const char *nx = api_last_next();
            if (!nx[0]) { logmsg("推荐: 第 %d 页没有 next，停止", p); break; }
            rc = api_recommend_next(&l, nx);
        }
        logmsg("推荐: 第 %d 页 rc=%d -> 累计 %d 条（本页新增 %d）", p, rc, l.n, l.n - before);
        if (rc != 0) break;
        if (l.n == before) break;      /* 这页没有新内容，再翻也是重复 */
        if (l.n >= g_cfg.rec) break;   /* 够了设置里的条数就停（6/12/18） */
    }
    int rc = l.n > 0 ? 0 : -1;
    if (rc == 0) api_list_save(out_path, &l);
    else job_write_err(err_path, api_error());
    api_list_free(&l);
    return rc;
}

static void act_recommend(void) {
    if (!account_has_login()) {
        show_message("推荐流需要登录。\n\n请先到「账号 / 登录」扫码登录或导入 Cookie。\n游客访问会被知乎返回 403。");
        return;
    }
    RecArg arg;
    arg.pages = REC_PAGES;
    char path[320], err[256];
    if (run_job(job_recommend, &arg, "正在获取推荐…", "rec",
                path, sizeof(path), err, sizeof(err), 40) != 0) {
        show_message("%s\n\n（推荐流对登录态和网络出口较敏感，若持续失败可重新登录或换网络）", err);
        return;
    }
    list_reset("推荐", "rec");
    if (api_list_load(path, &g.list) != 0) { show_message("推荐数据读取失败"); return; }
    if (g.list.n == 0) list_set_empty_msg("推荐接口返回空列表，隔一会儿再刷新。");
    g.scr = SCR_LIST;
    draw_list();
}

static void act_search(const char *q) {
    if (!q || !*q) return;
    Buf enc;
    buf_init(&enc);
    static const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)q; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') buf_addc(&enc, (char)*p);
        else {
            buf_addc(&enc, '%');
            buf_addc(&enc, hex[*p >> 4]);
            buf_addc(&enc, hex[*p & 15]);
        }
    }
    char path[1200];
    snprintf(path, sizeof(path),
             "/api/v4/search_v3?t=general&q=%s&correction=1&offset=0&limit=20", buf_cstr(&enc));
    buf_free(&enc);

    char json[320], err[256];
    if (fetch_job(path, "正在搜索…", json, sizeof(json), err, sizeof(err)) != 0) {
        show_message("%s", err);
        return;
    }
    size_t n = 0;
    char *body = read_file(json, &n);
    if (!body) { show_message("读取响应失败"); return; }
    list_reset(q, "search");
    int rc = api_parse_feed(body, n, &g.list);
    free(body);
    if (rc != 0) { show_message("%s", api_error()); return; }
    if (g.list.n == 0) list_set_empty_msg("没有搜到结果。换个关键词试试（输入框支持英文/数字）。");
    g.scr = SCR_LIST;
    draw_list();
}

static void act_question(const char *qid, const char *title) {
    char path[640], err[256], json[320];
    snprintf(path, sizeof(path),
             "/api/v4/questions/%s/answers?include=data[*].content,voteup_count,comment_count,"
             "author.name,excerpt&limit=20&offset=0&order_by=default", qid);
    if (fetch_job(path, "正在获取回答…", json, sizeof(json), err, sizeof(err)) != 0) {
        show_message("%s\n\n（若提示登录，说明知乎限制了游客访问该问题）", err);
        return;
    }
    size_t n = 0;
    char *body = read_file(json, &n);
    if (!body) { show_message("读取响应失败"); return; }
    list_reset(title, "answers");
    snprintf(g.list_qid, sizeof(g.list_qid), "%s", qid);
    int rc = api_parse_feed(body, n, &g.list);
    free(body);
    if (rc != 0) { show_message("%s", api_error()); return; }
    if (g.list.n == 0) list_set_empty_msg("这个问题下没有可显示的回答。");
    g.scr = SCR_LIST;
    draw_list();
}

static void act_open_item(ApiItem *it) {
    if (!it) return;

    /* 没有 id 就当场说清楚，绝不发"空 id 请求"。
     * 1.2.4 点进去 404（而且响应体是空的）最可能就是这条路：
     * 链接形态没认出来 → id 空 → 请求 /api/v4/answers/ → 404。 */
    if (!it->id[0] && !(it->type == 3 && it->qid[0])) {
        logmsg("打开: 条目 id 缺失 type=%d id=[%s] qid=[%s] url=%s 标题=%s",
               it->type, it->id, it->qid, it->url[0] ? it->url : "(无)", it->title);
        show_message("这个条目没有可用的 id，打不开。\n"
                     "知乎给的链接：%s\n"
                     "（已写入日志，可反馈给作者）",
                     it->url[0] ? it->url : "（列表里没给链接）");
        return;
    }

    if (it->type == 3 && it->qid[0]) {
        act_question(it->qid, it->title);
        return;
    }

    DetailArg d;
    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", it->id);
    snprintf(d.qid, sizeof(d.qid), "%s", it->qid);
    d.type = (it->type == 2) ? 2 : 1;
    /* 图片只在设置里打开时才下。宽度取正文宽，高度上限取正文高的 3/4，
     * 免得一张竖图占满整页、翻三页才过得去。 */
    d.want_img = 0;
    d.max_w = IMG_CACHE_W;
    d.max_h = (g.H - reader_header_h() - g.footer_h) * 3 / 4;

    char path[320], err[256];
    /* 不再等图：25 秒足够拿正文，图片走后台 */
    int timeout = 25;
    if (run_job(job_detail, &d, "正在打开…", "detail",
                path, sizeof(path), err, sizeof(err), timeout) != 0) {
        show_message("%s", err);
        return;
    }

    size_t n = 0;
    char *buf = read_file(path, &n);
    if (!buf) { show_message("读取响应失败"); return; }

    /* 5 段 NUL 分隔：title / author / voteup / created / html。
     * 老格式只有 3 段，用"第 3 段能不能解析成纯数字"来区分（html 一定含 '<'）。 */
    char *title = buf;
    char *author = (title + strlen(title) + 1 < buf + n) ? title + strlen(title) + 1 : title;
    char *f3 = (author + strlen(author) + 1 < buf + n) ? author + strlen(author) + 1 : author;
    long voteup = 0, created = 0;
    char *html;
    if (f3 > author && f3[0] >= '0' && f3[0] <= '9' &&
        strspn(f3, "0123456789") == strlen(f3)) {
        voteup = atol(f3);
        char *f4 = (f3 + strlen(f3) + 1 < buf + n) ? f3 + strlen(f3) + 1 : f3;
        created = atol(f4);
        html = (f4 + strlen(f4) + 1 < buf + n) ? f4 + strlen(f4) + 1 : "";
    } else {
        html = f3;                                  /* 旧格式：第 3 段就是 html */
    }

    open_doc_from_html(title, author, html, it->type, it->id, it->qid, voteup, created);
    free(buf);
}

/* ---------------- 我的收藏 ---------------- */

static int job_collections(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    ApiList l;
    memset(&l, 0, sizeof(l));
    int rc = api_my_collections(&l);
    if (rc == 0 && l.n > 0) {
        api_list_save(out_path, &l);
    } else {
        job_write_err(err_path, api_error());
        rc = -1;
    }
    logmsg("收藏: 收藏夹 %d 个 rc=%d", l.n, rc);
    api_list_free(&l);
    return rc;
}

static int job_allfav(const char *out_path, const char *err_path, void *arg) {
    (void)arg;
    ApiList l;
    memset(&l, 0, sizeof(l));
    int rc = api_all_collections(&l, 8);
    if (rc == 0 && l.n > 0) api_list_save(out_path, &l);
    else { job_write_err(err_path, api_error()); rc = -1; }
    api_list_free(&l);
    return rc;
}

static void act_collection_items(const char *cid, const char *title) {
    char path[800];
    snprintf(path, sizeof(path),
             "/api/v4/collections/%s/items?offset=0&limit=20&"
             "include=data[*].content", cid);
    char json[320], err[256];
    if (fetch_job(path, "正在读取收藏内容…", json, sizeof(json), err, sizeof(err)) != 0) {
        show_message("%s\n\n（知乎对收藏夹接口也做了风控，登录态失效时会直接拒绝）", err);
        return;
    }
    size_t n = 0;
    char *body = read_file(json, &n);
    if (!body) { show_message("读取响应失败"); return; }
    list_reset(title && *title ? title : "收藏内容", "collection");
    int rc = api_parse_feed(body, n, &g.list);
    free(body);
    if (rc != 0) { show_message("%s", api_error()); return; }
    if (g.list.n == 0) list_set_empty_msg("这个收藏夹里还没有内容。");
    g.scr = SCR_LIST;
    draw_list();
}

static void act_allfav(void) {
    char path[320], err[256];
    if (run_job(job_allfav, NULL, "正在合并收藏夹…", "allfav",
                path, sizeof(path), err, sizeof(err), 45) != 0) {
        show_message("%s", err);
        return;
    }
    list_reset("全部收藏", "collection");
    if (api_list_load(path, &g.list) != 0) { show_message("收藏数据读取失败"); return; }
    if (g.list.n == 0) list_set_empty_msg("收藏夹里还没有内容。");
    g.scr = SCR_LIST;
    draw_list();
}

/* 首页「我的收藏」：1.2.5 只弹一句"需要登录"，根本没调接口，
 * 已登录也只能看到那句提示。现在真的去拉收藏夹列表。 */
static void act_collections(void) {
    if (!account_has_login()) {
        show_message("「我的收藏」需要登录。\n\n"
                     "请先到「账号 / 登录」扫码登录或导入 Cookie；\n"
                     "游客态下知乎不返回任何收藏数据。");
        return;
    }
    char path[320], err[256];
    if (run_job(job_collections, NULL, "正在读取收藏夹…", "fav",
                path, sizeof(path), err, sizeof(err), 30) != 0) {
        show_message("读取收藏夹失败：%s", err);
        return;
    }
    ApiList l;
    memset(&l, 0, sizeof(l));
    if (api_list_load(path, &l) != 0) { show_message("收藏夹数据读取失败"); return; }

    /* 列表第一行固定是「全部收藏」：把各夹内容合并，按收藏时间倒序。
     * 有些人只想要"我收藏过的所有东西"，不想先点进夹子再点内容。 */
    api_list_free(&g.list);      /* 换列表前先放掉上一个（热榜/推荐那份） */
    ApiList all;
    memset(&all, 0, sizeof(all));
    all.v = xcalloc((size_t)l.n + 1, sizeof(ApiItem));
    all.cap = all.n = l.n + 1;
    snprintf(all.v[0].title, sizeof(all.v[0].title), "全部收藏");
    snprintf(all.v[0].sub, sizeof(all.v[0].sub), "合并全部 %d 个收藏夹 · 点按查看", l.n);
    all.v[0].type = 0;
    for (int i = 0; i < l.n; i++) all.v[i + 1] = l.v[i];
    free(l.v);

    g.list = all;
    snprintf(g.list_title, sizeof(g.list_title), "我的收藏");
    snprintf(g.list_kind, sizeof(g.list_kind), "collections");
    g.list_msg[0] = 0;
    g.list_sel = 0;
    g.list_top = 0;
    g.list_page = 0;
    g.scr = SCR_LIST;
    draw_list();
}

/* ---------------- 账号 / 登录 / 私信 动作 ---------------- */

static void act_account(void) {
    g.qr_sel = 0;
    if (account_has_login() && !g.acc_user[0])
        snprintf(g.acc_user, sizeof(g.acc_user), "已登录（Cookie）");
    g.scr = SCR_ACCOUNT;
    draw_account();
}

static void act_qr_begin(void) {
    qr_stop();
    g.qr_demo = 0;
    g.qr_state = 0;
    g.qr_state_text[0] = 0;
    g.qr_link[0] = 0;
    g_qr_drawn_text[0] = 0;
    g.qr_pid = 0;
    snprintf(g.qr_path, sizeof(g.qr_path), "%s/qr.out", zh_home());

    char out[320], err[256] = "";
    if (run_job(job_qr_begin, NULL, "正在获取登录二维码…", "qr",
                out, sizeof(out), err, sizeof(err), 25) != 0) {
        show_message("%s", err);
        return;
    }
    QrStateFile st;
    if (qr_read_file(&st) != 0) { show_message("二维码状态读取失败，请重试"); return; }
    snprintf(g.qr_link, sizeof(g.qr_link), "%s", st.q.link);
    g.qr_state = st.q.state;
    if (g.qr_state == 1)
        snprintf(g.qr_state_text, sizeof(g.qr_state_text), "等待扫码…（每 2 秒自动刷新）");
    else
        snprintf(g.qr_state_text, sizeof(g.qr_state_text), "%s",
                 st.q.err[0] ? st.q.err : "取码失败，请返回改用「导入 Cookie」");
    g.qr_last_ms = now_ms_ui();
    g.scr = SCR_QRCODE;
    draw_qrcode();
}

static void act_cookie_import(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/cookie.txt", zh_home());
    size_t n = 0;
    char *raw = read_file(path, &n);
    if (!raw || !*raw) {
        show_message("没有读到 %s\n\n请把浏览器知乎网页版的完整 Cookie（含 z_c0=）写入该文件，再点「导入 Cookie」。", path);
        free(raw);
        return;
    }
    free(raw);
    char out[320], err[256] = "";
    if (run_job(job_cookie, NULL, "正在校验 Cookie…", "cookie",
                out, sizeof(out), err, sizeof(err), 25) != 0) {
        show_message("%s", err);
        return;
    }
    LoginResult r;
    memset(&r, 0, sizeof(r));
    FILE *f = fopen(out, "rb");
    int ok = f && fread(&r, sizeof(r), 1, f) == 1;
    if (f) fclose(f);
    if (!ok) { show_message("导入结果读取失败"); return; }
    if (r.rc == 0) {
        snprintf(g.acc_user, sizeof(g.acc_user), "%s", r.user[0] ? r.user : "已登录");
        api_load_cookie();
        show_message("登录成功：%s\n\n现在可以看推荐、私信和收藏了。", g.acc_user);
    } else {
        show_message("Cookie 导入失败：%s\n\n请确认复制的是知乎网页版完整 Cookie，且 z_c0 未过期。",
                     r.err[0] ? r.err : "校验未通过");
    }
}

static void act_me(void) {
    char out[320], err[256] = "";
    if (run_job(job_me, NULL, "正在校验登录态…", "me",
                out, sizeof(out), err, sizeof(err), 25) != 0) {
        show_message("%s", err);
        return;
    }
    LoginResult r;
    memset(&r, 0, sizeof(r));
    FILE *f = fopen(out, "rb");
    int ok = f && fread(&r, sizeof(r), 1, f) == 1;
    if (f) fclose(f);
    if (!ok) { show_message("校验结果读取失败"); return; }
    if (r.rc == 0) {
        snprintf(g.acc_user, sizeof(g.acc_user), "%s", r.user[0] ? r.user : "已登录");
        show_message("登录有效：%s", g.acc_user);
    } else {
        g.acc_user[0] = 0;
        show_message("当前未登录或 Cookie 已失效：%s", r.err[0] ? r.err : "校验未通过");
    }
}

static void act_probe(void) {
    char out[320], err[256] = "";
    if (run_job(job_probe, NULL, "正在探测私信端点…", "probe",
                out, sizeof(out), err, sizeof(err), 60) != 0) {
        show_message("%s", err);
        return;
    }
    size_t n = 0;
    char *s = read_file(out, &n);
    show_message("%s\n\n完整路径与返回码见 log.txt；若某个端点 status=200，把它的路径写进 social.conf 的 threads= 即可。",
                 s ? s : "（诊断结果为空）");
    free(s);
}

static void act_logout(void) {
    account_logout();
    g.acc_user[0] = 0;
    api_load_cookie();
    show_message("已退出登录。\n\n设备标识 d_c0 已保留，下次扫码更不容易触发风控。");
}

static void act_dm(void) {
    if (!account_has_login()) {
        show_message("私信需要登录。\n\n请先到「账号 / 登录」用二维码扫码，或导入 Cookie。");
        return;
    }
    char out[320], err[256] = "";
    if (run_job(job_threads, NULL, "正在读取私信会话…", "threads",
                out, sizeof(out), err, sizeof(err), 25) != 0) {
        show_message("%s", err);
        return;
    }
    thread_list_free(&g.threads);
    if (social_threads_load(out, &g.threads) != 0) {
        show_message("会话数据读取失败。\n\n可用「私信端点诊断」查看各端点返回状态。");
        return;
    }
    g.dm_sel = 0;
    g.dm_top = 0;
    g.scr = SCR_DMLIST;
    draw_dm_list();
}

/* 把消息列表拼成阅读器文档，直接复用整页排版 / 翻页 / 进度条 */
static void dm_build_doc(void) {
    doc_free(&g.doc);
    doc_init(&g.doc);
    char head[256];
    snprintf(head, sizeof(head), "与 %s 的私信", g.dm_peer);
    doc_add(&g.doc, head, BLK_H2);
    doc_add(&g.doc, "点屏幕左侧翻上一页、右侧翻下一页；点右下角「返回」或顶部标题回会话列表。", BLK_QUOTE);
    if (g.msgs.n == 0) {
        doc_add(&g.doc,
                "（这个会话暂时没读到消息。可能是知乎私信端点结构与当前版本不同，请用「私信端点诊断」把 log.txt 反馈给我。）",
                BLK_PARA);
    }
    for (int i = 0; i < g.msgs.n; i++) {
        MsgItem *m = &g.msgs.v[i];
        char when[64] = "";
        if (m->created > 0) {
            time_t t = (time_t)m->created;
            struct tm tmv;
            if (localtime_r(&t, &tmv))
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
        }
        char meta[320];
        snprintf(meta, sizeof(meta), "%s  %s  · %s",
                 m->is_self ? "我" : (m->sender[0] ? m->sender : "对方"),
                 when, m->is_self ? "发出" : "收到");
        doc_add(&g.doc, meta, BLK_H3);
        doc_add(&g.doc, m->content[0] ? m->content : "（空消息）", BLK_PARA);
    }
    snprintf(g.doc_title, sizeof(g.doc_title), "私信 · %s", g.dm_peer);
    snprintf(g.doc_author, sizeof(g.doc_author), "%s", g.dm_peer);
    g.reader_err[0] = 0;
    reader_layout();
    g.page = 0;
    g.reading = 1;
    g.scr = SCR_DMREAD;
    draw_dm_read();
}

static void act_dm_open(int idx) {
    if (idx < 0 || idx >= g.threads.n) return;
    snprintf(g.dm_id, sizeof(g.dm_id), "%s", g.threads.v[idx].id);
    snprintf(g.dm_peer, sizeof(g.dm_peer), "%s", g.threads.v[idx].peer);
    MsgArg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "%s", g.threads.v[idx].id);
    snprintf(a.url, sizeof(a.url), "%s", g.threads.v[idx].url);
    char out[320], err[256] = "";
    if (run_job(job_messages, &a, "正在读取消息…", "messages",
                out, sizeof(out), err, sizeof(err), 25) != 0) {
        show_message("%s", err);
        return;
    }
    msg_list_free(&g.msgs);
    if (social_messages_load(out, &g.msgs) != 0) {
        show_message("消息数据读取失败。\n\n可用「私信端点诊断」查看各端点返回状态。");
        return;
    }
    dm_build_doc();
}

/* ---------------- 输入 ---------------- */

static void input_start(const char *title, int kind, const char *initial) {
    snprintf(g.input_title, sizeof(g.input_title), "%s", title);
    g.input_kind = kind;
    snprintf(g.input, sizeof(g.input), "%s", initial ? initial : "");
    g.input_len = (int)strlen(g.input);
    g.scr = SCR_INPUT;
    draw_input();
}

/* ---------------- 事件处理 ---------------- */

static int list_hit_row(int y) {
    int per = list_rows_per_page();
    int rh = list_row_h();
    int yy = y - g.header_h - 2;
    if (yy < 0) return -1;
    int i = yy / rh;
    if (i < 0 || i >= per) return -1;
    int idx = g.list_top + i;
    if (idx < 0 || idx >= g.list.n) return -1;
    return idx;
}

/* 统一返回：1.2.7 起右下角「返回」按钮到处都在，行为也必须到处一致 */
static void nav_back(void) {
    switch (g.scr) {
        case SCR_READER:
            img_stop();
            g.scr = SCR_LIST;
            draw_list();
            break;
        case SCR_DMREAD:
            g.scr = SCR_DMLIST;
            draw_dm_list();
            break;
        case SCR_QRCODE:
            qr_stop();
            g.scr = SCR_ACCOUNT;
            draw_account();
            break;
        case SCR_ACCOUNT:
        case SCR_DMLIST:
        case SCR_SETTINGS:
        case SCR_ABOUT:
            g.scr = SCR_HOME;
            g.list_sel = 0;
            draw_home();
            break;
        case SCR_LIST:
        default:
            g.scr = SCR_HOME;
            g.list_sel = 0;
            draw_home();
            break;
    }
}

/* 列表翻页：整页滚动，选中项跟着走 */
static void list_page(int step) {
    int per = list_rows_per_page();
    int total = (g.list.n + per - 1) / per;
    if (total < 1) total = 1;
    int page = g.list_sel / per + step;
    if (page < 0) page = 0;
    if (page > total - 1) page = total - 1;
    g.list_sel = page * per;
    /* 页首要与"这一页的第一条"对齐：只改选中项的话，draw_list 里那条
     * "保持选中项可见"的规则会把页首拉回上一页末尾（翻页后首行是第 2 条）。 */
    g.list_top = page * per;
    if (g.list_sel >= g.list.n) g.list_sel = g.list.n > 0 ? g.list.n - 1 : 0;
    draw_list();
}

static void handle_btn(int id) {
    switch (id) {
        case BTN_BACK: nav_back(); break;
        case BTN_PREV: list_page(-1); break;
        case BTN_NEXT: list_page(1); break;
        case BTN_REFRESH:
            if (g.scr == SCR_LIST && strcmp(g.list_kind, "rec") == 0) act_recommend();
            break;
        default: break;
    }
}

static void handle_tap(int x, int y) {
    /* 页脚按钮优先：按钮画在哪就点到哪，不再靠"点页眉/点空白处"这类约定 */
    if (g.scr != SCR_HOME && g.scr != SCR_MESSAGE && g.scr != SCR_LOADING) {
        int id = footer_hit(x, y);
        if (id == BTN_BACK) { handle_btn(id); return; }
        if ((id == BTN_PREV || id == BTN_NEXT || id == BTN_REFRESH) && g.scr == SCR_LIST) {
            handle_btn(id);
            return;
        }
    }

    switch (g.scr) {
        case SCR_HOME: {
            int top = home_top();
            int rh = home_row_h();
            if (y >= top) {
                int i = (y - top) / rh;
                if (i >= 0 && i < HOME_COUNT) {
                    g.list_sel = i;
                    /* 直接执行，不再多刷一次首页：电子墨水每刷一次都要闪一下 */
                    switch (i) {
                        case 0: act_hot(); break;
                        case 1: act_recommend(); break;
                        case 2: act_dm(); break;
                        case 3: act_account(); break;
                        case 4: input_start("搜索（英文/数字）", 1, ""); break;
                        case 5: act_collections(); break;
                        case 6: open_history(); break;
                        case 7: g.scr = SCR_SETTINGS; g.list_sel = 0; draw_settings(); break;
                        case 8: g.scr = SCR_ABOUT; draw_about(); break;
                        case 9: g.quit = 1; break;
                    }
                }
            }
            break;
        }
        case SCR_LIST: {
            if (y < g.header_h) { g.scr = SCR_HOME; g.list_sel = 0; draw_home(); return; }
            if (y > g.H - g.footer_h) return;   /* 页脚只认按钮，避免误触返回 */
            int idx = list_hit_row(y);
            if (idx >= 0) {
                g.list_sel = idx;
                ApiItem *it = &g.list.v[idx];
                if (strcmp(g.list_kind, "collections") == 0) {
                    /* 收藏夹列表：第 0 行是「全部收藏」，其余是具体夹子 */
                    if (idx == 0) act_allfav();
                    else act_collection_items(it->id, it->title);
                    break;
                }
                act_open_item(it);
            }
            break;
        }
        case SCR_DMREAD:
        case SCR_READER: {
            if (y < reader_header_h()) {
                /* 点标题栏也当返回：老用户的手感，和右下角按钮不冲突 */
                if (g.scr == SCR_DMREAD) { g.scr = SCR_DMLIST; draw_dm_list(); }
                else { img_stop(); g.scr = SCR_LIST; draw_list(); }
                return;
            }
            /* 页脚以上才是翻页区：右下角是「返回」，不能再当翻下一页 */
            if (y >= g.H - g.footer_h) return;
            /* 图片占位框优先于翻页：点按模式下点框就去下那一张 */
            if (g.scr == SCR_READER && g.img_mode == 1) {
                int is_tap = 0;
                int idx = reader_img_hit(x, y, &is_tap);
                if (idx >= 0 && is_tap) { img_load_one(idx); return; }
            }
            if (x < g.W / 4) reader_goto(g.page - 1);
            else reader_goto(g.page + 1);
            break;
        }
        case SCR_SETTINGS: {
            int rh = set_row_h();
            int i = (y - g.header_h) / rh;
            if (i < 0) { break; }
            if (i == 0) {
                g_cfg.size = (g_cfg.size + 1) % 3;
                g.base = g.W / (g_cfg.size == 0 ? 32 : (g_cfg.size == 1 ? 28 : 24));
                config_save();
                if (g.doc.n) { reader_layout(); if (g.page >= g.lay.npages) g.page = 0; }
            } else if (i == 1) {
                /* 翻页刷屏三档：不闪（局部 GC16）→ 整屏闪 → Regal（实验） */
                g_cfg.fast = (g_cfg.fast + 1) % 3;
                g.fb.fast_mode = g_cfg.fast;
                config_save();
                if (g_cfg.fast == 2)
                    show_message("翻页刷屏：Regal（实验）。\n\nRegal 是 Kindle 的残影补偿波形"
                                 "（REAGL，编号 0x8）。\n翻页不闪屏，且驱动会主动补偿残影。\n\n"
                                 "部分机型的驱动不认这个波形，会自动退回局部刷新\n"
                                 "并在 log.txt 里写明原因。");
                return;
            } else if (i == 2) {
                g_cfg.rotate = (g_cfg.rotate + 1) % 4;
                fb_set_rotate(&g.fb, g_cfg.rotate);
                /* 关键：显示转了，触摸必须跟着转。
                 * 1.2.4 只调了 fb_set_rotate，触摸矩阵还是启动时那套
                 * (0,0,0)，于是除 0° 外三个方向的点击全部错位。 */
                input_set_rotate(g_cfg.rotate, g.fb.width, g.fb.height);
                layout_metrics();
                if (g.doc.n) { reader_layout(); if (g.page >= g.lay.npages) g.page = 0; }
                logmsg("设置: 旋转 %d°（触摸同步，物理 %dx%d）",
                       g_cfg.rotate * 90, g.fb.width, g.fb.height);
                config_save();
            } else if (i == 3) {
                g_cfg.images = (g_cfg.images + 1) % 3;
                g.img_mode = g_cfg.images;
                config_save();
                /* 图片是在「打开文章」时排版的，开关改完得说清楚下一步，
                 * 否则用户会以为开关坏了。 */
                if (g_cfg.images == 0)
                    show_message("图片：关闭。\n\n正文里只留「[图片]」一行字，"
                                 "打开最快、最省流量。");
                else if (g_cfg.images == 1)
                    show_message("图片：点按加载。\n\n正文先出来，图片位置是一个"
                                 "「点按加载此图」的框；点哪张下哪张，\n"
                                 "下好立刻就地显示（不用等全部下完）。\n\n"
                                 "加载失败会在屏幕上直接告诉你原因。");
                else
                    show_message("图片：自动加载。\n\n正文先出来，图片在后台从"
                                 "第一张开始逐张下，\n补上就自动替换占位（只刷当前页）。");
                return;
            } else if (i == 4) {
                g_cfg.margin_pct = g_cfg.margin_pct >= 10 ? 4 : g_cfg.margin_pct + 3;
                g.margin = g.W * g_cfg.margin_pct / 100;
                config_save();
                if (g.doc.n) reader_layout();
            } else if (i == 5) {
                /* 推荐条数：6 → 12 → 18 → 6 */
                g_cfg.rec = (g_cfg.rec == 6) ? 12 : (g_cfg.rec == 12 ? 18 : 6);
                config_save();
            } else if (i == 6) {
                /* 1.2.10：系统状态栏与 framework 解耦。隐藏=共存+屏幕守护
                 * 压顶栏（顶栏偶尔闪现，秒级被回灌压掉），不再杀 cvm——
                 * 1.2.9 的 framework=stop 被 framework 看门狗判死、
                 * 宽限期后整机重启。重启程序后生效（start.sh 读 config）。 */
                g_cfg.showbar = g_cfg.showbar ? 0 : 1;
                config_save();
                if (g_cfg.showbar == 0)
                    show_message("系统状态栏：隐藏（压屏）。\n\n顶栏的时间/网络/电量出现后\n"
                                 "会被屏幕守护自动压掉（几秒内）。\n不杀系统界面，不会触发整机重启。\n\n"
                                 "退出本应用并从 KUAL 重新打开后生效。");
                else
                    show_message("系统状态栏：显示（推荐）。\n\n与 Kindle 系统界面共存，"
                                 "顶栏偶尔会盖到画面上。\n退出并重新打开后生效。");
                return;
            } else if (i == 7) {
                if (g.doc.n) { reader_layout(); if (g.page >= g.lay.npages) g.page = 0; }
            } else if (i == 8) {
                long n = api_cache_clear();
                show_message("已删除 %ld 个缓存文件\n（含图片缓存，全部文章的都清掉了）", n);
                return;
            } else {
                g.scr = SCR_HOME;
                g.list_sel = 0;
                draw_home();
                return;
            }
            draw_settings();
            break;
        }
        case SCR_ACCOUNT: {
            int rh = acc_row_h();
            if (y < acc_top()) break;
            int i = (y - acc_top()) / rh;
            if (i < 0 || i >= ACC_COUNT) break;
            g.qr_sel = i;
            switch (i) {
                case 0: act_qr_begin(); break;
                case 1: act_cookie_import(); break;
                case 2: act_me(); break;
                case 3: act_probe(); break;
                case 4: act_logout(); break;
                default:
                    qr_stop();
                    g.scr = SCR_HOME; g.list_sel = 0; draw_home();
                    break;
            }
            break;
        }
        case SCR_QRCODE:
            qr_stop();
            g.scr = SCR_ACCOUNT;
            draw_account();
            break;
        case SCR_DMLIST: {
            if (y < g.header_h || y > g.H - g.footer_h) {
                g.scr = SCR_HOME; draw_home(); return;
            }
            int rh = dm_row_h();
            int i = (y - g.header_h - 2) / rh + g.dm_top;
            if (i >= 0 && i < g.threads.n) { g.dm_sel = i; act_dm_open(i); }
            break;
        }
        case SCR_ABOUT:
            g.scr = SCR_HOME;
            draw_home();
            break;
        case SCR_MESSAGE: {
            g.msg_until = 0;      /* 用户点了，就别再自动关了 */
            int dst = g.scr_prev;
            switch (dst) {
                case SCR_LIST: case SCR_READER: case SCR_DMREAD:
                case SCR_DMLIST: case SCR_ACCOUNT: case SCR_QRCODE:
                    break;
                default:
                    dst = SCR_HOME;   /* 加载页/未知页不作为返回目标 */
                    break;
            }
            g.scr = dst;
            switch (dst) {
                case SCR_LIST:    draw_list(); break;
                case SCR_READER:  draw_reader(); break;
                case SCR_DMREAD:  draw_dm_read(); break;
                case SCR_DMLIST:  draw_dm_list(); break;
                case SCR_ACCOUNT: draw_account(); break;
                case SCR_QRCODE:  draw_qrcode(); break;
                default: g.list_sel = 0; draw_home(); break;
            }
            break;
        }
        case SCR_INPUT: {
            /* 交给键盘处理 */
            int kb_top = g.H - g.footer_h - (g.base * 11 / 5) * 5;
            if (kb_top < g.header_h + g.base * 3) kb_top = g.header_h + g.base * 3;
            int key_h = (g.H - g.footer_h - kb_top) / 5;
            int cols = 10;
            int key_w = (g.W - 2 * g.margin) / cols;
            int r = (y - kb_top) / key_h;
            if (r >= 0 && r < 4) {
                const char *row = KB_ROWS[r];
                int len = (int)strlen(row);
                int base_x = g.margin + (g.W - 2 * g.margin - key_w * len) / 2;
                int c = (x - base_x) / key_w;
                if (c >= 0 && c < len) {
                    char ch = row[c];
                    if (ch >= 'a' && ch <= 'z' && g.kb_shift) ch = (char)(ch - 32);
                    if (g.input_len < (int)sizeof(g.input) - 2) {
                        g.input[g.input_len++] = ch;
                        g.input[g.input_len] = 0;
                    }
                    draw_input();
                }
                return;
            }
            if (r == 4) {
                int cx = g.margin;
                int widths[4] = { 4, 2, 2, 2 };
                for (int i = 0; i < 4; i++) {
                    int w2 = (g.W - 2 * g.margin) * widths[i] / 10;
                    if (x >= cx && x < cx + w2) {
                        if (i == 0) {
                            if (g.input_len < (int)sizeof(g.input) - 2) {
                                g.input[g.input_len++] = ' ';
                                g.input[g.input_len] = 0;
                            }
                            draw_input();
                        } else if (i == 1) {
                            if (g.input_len > 0) {
                                /* 删一个 UTF-8 字符 */
                                int k = g.input_len - 1;
                                while (k > 0 && ((unsigned char)g.input[k] & 0xC0) == 0x80) k--;
                                g.input_len = k;
                                g.input[k] = 0;
                            }
                            draw_input();
                        } else if (i == 2) {
                            g.scr = SCR_HOME;
                            act_search(g.input);
                        } else {
                            g.scr = SCR_HOME;
                            draw_home();
                        }
                        return;
                    }
                    cx += w2;
                }
            }
            break;
        }
        default:
            break;
    }
}

/* 仅交互模拟器可用（ZH_SIM_LIVE）：注入一篇本地示例文章。
 * 知乎「问题→回答列表」接口在部分出口会被风控 403，模拟器里靠它把
 * 阅读页、整页翻页、手势、返回这条链路完整跑通。真机上永不触发。 */
/* 模拟器专用：造一张灰度测试图写进图片缓存，让"显示图片"这条链路
 * 在完全离线的情况下也能被看到。真机上永远不会走到这里。 */
static void sim_seed_image(const char *url) {
    int w = IMG_CACHE_W;
    if (w < 200) w = 200;
    int h = w * 2 / 5;
    ImgGray im;
    im.w = w;
    im.h = h;
    im.px = xmalloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v;
            if (x < w / 2 && y < h / 2)      v = 40;        /* 左上深灰 */
            else if (x >= w / 2 && y < h / 2) v = 120;
            else if (x < w / 2)               v = 200;
            else                              v = 245;
            /* 细网格：能一眼看出缩放是否正确、有没有被拉伸 */
            if (x % 40 == 0 || y % 40 == 0) v = (v + 128) / 2;
            im.px[y * w + x] = (uint8_t)v;
        }
    }
    if (img_cache_save(url, &im) == 0)
        logmsg("sim: 已生成测试图缓存 %dx%d", w, h);
    img_free(&im);
}

static void sim_demo_open(void) {
    if (!getenv("ZH_SIM_LIVE")) return;
    /* 演示文章带上图片：g_cfg.images 只在内存里打开，不写回 config.txt。
     * 2 = 自动异步档（模拟器要截"占位 → 补上"这两帧）。 */
    g_cfg.images = 2;
    g.img_mode = 2;
    /* 想单独看某一档（例如点按档）的排版：ZH_SIM_IMGMODE=1 ./kindle_sim.py … */
    const char *im = getenv("ZH_SIM_IMGMODE");
    if (im) { int v = atoi(im); if (v >= 0 && v <= 2) { g_cfg.images = v; g.img_mode = v; } }
    /* ZH_SIM_IMG_DELAY：不预置缓存，改用"后台延迟到货"来复现真机节奏，
     * 让模拟器能截到「占位框 → 图片补上」这两帧。 */
    if (!getenv("ZH_SIM_IMG_DELAY"))
        sim_seed_image("https://pic1.zhimg.com/sim-demo.jpg");
    static const char *html =
        "<h2>为什么现在的手机越来越贵？</h2>"
        "<p>先说结论：不是手机变贵了，是便宜的那部分被砍掉了。过去一千五能买到的配置，"
        "现在要两千五，中间这一千块并不是凭空涨上去的，它变成了几样很具体的东西。</p>"
        "<p>第一是影像。多摄模组、大底传感器、潜望长焦、光学防抖，这些零件在 BOM 里的"
        "占比这几年翻了一倍还多。厂商把「能拍月亮」当成卖点，但绝大多数人一个月的相册里，"
        "也没有一张真的需要潜望长焦的照片。</p>"
        "<h3>一、成本的重新分配</h3>"
        "<p>第二是屏幕和电池。高刷、高频调光、峰值亮度，加上为了压住功耗而不断加大的电池"
        "和快充，这一套组合下来，中端机的成本结构已经接近三年前的旗舰。</p>"
        "<blockquote>换机周期从 18 个月拉长到 36 个月，厂商只能靠提高单价来维持营收，"
        "这是最直接、也最容易被忽略的原因。</blockquote>"
        "<p>第三是存储。闪存价格在 2023 年触底之后连续上涨，12+256 从「标配」变成了"
        "「加钱项」，而系统本身占用的空间还在逐年变大。</p>"
        "<ul>"
        "<li>影像模组：成本占比从 12% 涨到 25% 左右</li>"
        "<li>屏幕：高刷与高频调光带来 30% 以上的涨幅</li>"
        "<li>存储：闪存涨价直接传导到售价</li>"
        "<li>认证与结构：防水、金属中框、无线充电线圈</li>"
        "</ul>"
        "<img src=\"https://pic1.zhimg.com/sim-demo.jpg\" />"
        "<h3>二、谁在为不需要的功能付钱</h3>"
        "<p>把一台机器拆开看，真正影响日常体验的其实只有三样：屏幕、续航、系统流畅度。"
        "剩下的部分更像是为了发布会上的那一页 PPT 而存在。问题在于，厂商不能只做这三样——"
        "同价位里参数表更长的那台，会在电商页面的对比里赢。</p>"
        "<p>于是出现了一个有点荒诞的结果：所有人都在抱怨手机贵，但所有人买的时候，"
        "还是会选那台「多一个摄像头」的。需求是被供给塑造出来的，价格也是。</p>"
        "<blockquote>贵的不是手机，是你为「不需要的功能」付了钱。</blockquote>"
        "<p>如果厂商愿意做一台只有好屏幕、好续航、能扫码的机器，一千五依然做得出来——"
        "只是没人愿意卖，因为它的毛利率撑不起渠道和营销。</p>"
        "<h3>三、还有别的路吗</h3>"
        "<p>有，但不在新机上。二手旗舰、上一代降价机型、以及厂商刻意做薄的「轻旗舰」，"
        "本质上都是把研发成本摊薄之后的产物。对不追新的人来说，这才是当下性价比最高的"
        "选择：同样的芯片，晚了十二个月，价格少一半。</p>"
        "<p>另一个变化是维修。电池不可拆、配件加密、官方换屏报价接近整机三成，"
        "这些都在悄悄把「用三年」变成「用两年就换」。当维修不划算，换机就成了唯一选项，"
        "单价上涨也就更容易被接受。</p>"
        "<p>回到最初的问题：手机并没有变贵，是「够用」的那条线被一点点抬高了。"
        "十年前一千五买的是一台完整的手机，今天一千五买到的是一台被精心裁剪过的手机。"
        "差别不在通货膨胀，而在厂商决定让你从哪一档开始挑。</p>"
        "<p>以上数据来自公开的拆解报告与几家供应链公司的年报，具体数字各家口径不同，"
        "量级上是一致的。本文只是模拟器内置的排版示例，与知乎上的任何回答无关。</p>";
    open_doc_from_html("示例文章 · 阅读页与整页翻页", "KO3 模拟器", html, 2, "demo", "",
                       1234, 1700000000L);
}

/* 仅交互模拟器可用（ZH_SIM_LIVE）：离线演示新增的三个页面。
 * 目的是让回归测试不依赖知乎登录态与网络出口，也能验证二维码绘制、
 * 私信列表排版、私信消息整页翻页。真机上永不触发。 */
/* 仅交互模拟器可用（ZH_SIM_LIVE）：空列表 + 原因说明页。
 * 这条路径以前只画一个灰色「（空）」，是用户反馈"点进去打不开"的主因之一。
 * 现在固化成离线演示，回归时能直接截图核对排版。真机上永不触发。 */
static void sim_empty_demo(void) {
    if (!getenv("ZH_SIM_LIVE")) return;
    list_reset("热榜", "hot");
    api_set_error("知乎返回错误（40352）：系统监测到您的网络环境存在异常，请求被拒绝");
    list_set_empty_msg("热榜接口返回空列表。");
    g.scr = SCR_LIST;
    draw_list();
}

static void sim_qr_demo(void) {
    if (!getenv("ZH_SIM_LIVE")) return;
    qr_stop();
    g.qr_demo = 1;
    snprintf(g.qr_link, sizeof(g.qr_link),
             "https://www.zhihu.com/account/scan/login/NThiNDg5ZTctNjBh?/api/login/qrcode");
    g.qr_state = 1;
    snprintf(g.qr_state_text, sizeof(g.qr_state_text),
             "模拟器离线二维码 · 真机请用手机知乎 App 扫码");
    snprintf(g.qr_path, sizeof(g.qr_path), "%s/qr.out", zh_home());
    g.scr = SCR_QRCODE;
    draw_qrcode();
}

static void sim_dm_list_demo(void) {
    if (!getenv("ZH_SIM_LIVE")) return;
    thread_list_free(&g.threads);
    g.threads.v = xcalloc(3, sizeof(MsgThread));
    g.threads.cap = g.threads.n = 3;
    const char *peers[]  = { "知乎小管家", "张三", "李四" };
    const char *snips[]  = { "你的回答已通过审核，点击查看",
                             "那篇泛癌多组学的文章我看了，很有意思",
                             "周末的讨论会改到下午三点" };
    for (int i = 0; i < 3; i++) {
        snprintf(g.threads.v[i].id, sizeof(g.threads.v[i].id), "t%d", i + 1);
        snprintf(g.threads.v[i].peer, sizeof(g.threads.v[i].peer), "%s", peers[i]);
        snprintf(g.threads.v[i].snippet, sizeof(g.threads.v[i].snippet), "%s", snips[i]);
        g.threads.v[i].unread = (i == 0) ? 2 : 0;
        g.threads.v[i].updated = 1700000000L + i;
    }
    g.dm_sel = 0;
    g.dm_top = 0;
    g.scr = SCR_DMLIST;
    draw_dm_list();
}

static void sim_dm_read_demo(void) {
    if (!getenv("ZH_SIM_LIVE")) return;
    msg_list_free(&g.msgs);
    g.msgs.v = xcalloc(6, sizeof(MsgItem));
    g.msgs.cap = g.msgs.n = 6;
    const char *senders[] = { "张三", "我", "张三", "我", "张三", "我" };
    const char *texts[] = {
        "你好，看到你整理的资料了。想请教一下头颈鳞癌那部分的数据来源：是用的 TCGA 原始队列，还是已经做过批次校正的合并矩阵？另外样本量在表格里标注的是肿瘤组织数，我想确认有没有把配对癌旁一并算进去。",
        "用的是公开数据库加几篇已发表的泛癌分析。原始数据我先做了 log2 转换，再用 ComBat 校掉测序平台带来的批次效应；配对癌旁没有混进肿瘤组，单独放在补充表二里。链接我晚点发你。",
        "太好了。还有个问题：生存分析那段的分组，是按表达量中位数一刀切，还是用最优截断值？如果是后者，有没有做多重检验校正？我担心不同切法会把 P 值推到阈值两边。",
        "按中位数切的，主图就是这套。最优截断值我们只在敏感性分析里试过，结论方向一致但没有写进正文；多重检验用 BH 校正，调整后的 q 值在附表里。",
        "明白了。最后是机制那部分：敲降之后做的增殖和迁移实验，用的是两株细胞系吗？有没有加 rescued 组把表型救回来？评审大概率会问这个，提前准备一下比较稳妥。",
        "两株都做了，rescue 也补上了，上周刚拿到结果，趋势和预期一致。等这轮图排完我把整套原始数据整理给你，方便你写讨论部分的时候直接引用。",
    };
    for (int i = 0; i < 6; i++) {
        snprintf(g.msgs.v[i].sender, sizeof(g.msgs.v[i].sender), "%s", senders[i]);
        snprintf(g.msgs.v[i].content, sizeof(g.msgs.v[i].content), "%s", texts[i]);
        g.msgs.v[i].created = 1700000000L + i * 3600;
        g.msgs.v[i].is_self = (i % 2 == 1);
    }
    snprintf(g.dm_peer, sizeof(g.dm_peer), "张三");
    snprintf(g.dm_id, sizeof(g.dm_id), "t2");
    dm_build_doc();
}

static void handle_key(int code) {
    if (code == 997) { sim_demo_open(); return; }
    if (code == 993) { sim_empty_demo(); return; }
    if (code == 996) { sim_qr_demo(); return; }
    if (code == 995) { sim_dm_list_demo(); return; }
    if (code == 994) { sim_dm_read_demo(); return; }
    if (code == 999) { g.quit = 1; return; }
    switch (g.scr) {
        case SCR_DMREAD:
        case SCR_READER:
            if (code == 158 || code == 102) {
                if (g.scr == SCR_DMREAD) { g.scr = SCR_DMLIST; draw_dm_list(); }
                else { img_stop(); g.scr = SCR_LIST; draw_list(); }
            }
            else if (code == 105 || code == 103 || code == 104) reader_goto(g.page - 1);
            else if (code == 106 || code == 108 || code == 109) reader_goto(g.page + 1);
            break;
        case SCR_DMLIST:
            if (code == 158 || code == 102) { g.scr = SCR_HOME; draw_home(); }
            else if (code == 103 || code == 105 || code == 104) { if (g.dm_sel > 0) { g.dm_sel--; draw_dm_list(); } }
            else if (code == 108 || code == 106 || code == 109) { if (g.dm_sel + 1 < g.threads.n) { g.dm_sel++; draw_dm_list(); } }
            else if (code == 28 || code == 96) { if (g.threads.n > 0) act_dm_open(g.dm_sel); }
            break;
        case SCR_ACCOUNT:
            if (code == 158 || code == 102) { g.scr = SCR_HOME; draw_home(); }
            else if (code == 103 || code == 105 || code == 104) { g.qr_sel = (g.qr_sel + ACC_COUNT - 1) % ACC_COUNT; draw_account(); }
            else if (code == 108 || code == 106 || code == 109) { g.qr_sel = (g.qr_sel + 1) % ACC_COUNT; draw_account(); }
            else if (code == 28 || code == 96)
                handle_tap(g.W / 2, acc_top() + acc_row_h() * g.qr_sel + 1);
            break;
        case SCR_QRCODE:
            if (code == 158 || code == 102 || code == 28 || code == 96) {
                qr_stop();
                g.scr = SCR_ACCOUNT;
                draw_account();
            }
            break;
        case SCR_LIST:
            if (code == 158 || code == 102) { g.scr = SCR_HOME; draw_home(); }
            else if (code == 103 || code == 105 || code == 104) { if (g.list_sel > 0) { g.list_sel--; draw_list(); } }
            else if (code == 108 || code == 106 || code == 109) { if (g.list_sel + 1 < g.list.n) { g.list_sel++; draw_list(); } }
            else if (code == 28 || code == 96) {
                if (g.list_sel < g.list.n) {
                    if (strcmp(g.list_kind, "collections") == 0) {
                        if (g.list_sel == 0) act_allfav();
                        else act_collection_items(g.list.v[g.list_sel].id, g.list.v[g.list_sel].title);
                    } else {
                        act_open_item(&g.list.v[g.list_sel]);
                    }
                }
            }
            break;
        case SCR_HOME:
            if (code == 108 || code == 106) { g.list_sel = (g.list_sel + 1) % HOME_COUNT; draw_home(); }
            else if (code == 103 || code == 105) { g.list_sel = (g.list_sel + HOME_COUNT - 1) % HOME_COUNT; draw_home(); }
            else if (code == 28 || code == 96)
                handle_tap(g.W / 2, home_top() + home_row_h() * g.list_sel + 1);
            else if (code == 158) g.quit = 1;
            break;
        case SCR_SETTINGS:
            if (code == 158 || code == 102) { g.scr = SCR_HOME; draw_home(); }
            break;
        default:
            if (code == 158) { qr_stop(); g.scr = SCR_HOME; draw_home(); }
            break;
    }
}

/* ---------------- 主循环 ---------------- */

static void layout_metrics(void) {
    g.W = fb_w(&g.fb);
    g.H = fb_h(&g.fb);
    g.base = g.W / (g_cfg.size == 0 ? 32 : (g_cfg.size == 1 ? 28 : 24));
    if (g.base < 14) g.base = 14;
    g.margin = g.W * g_cfg.margin_pct / 100;
    g.header_h = g.base * 3 / 2;
    g.footer_h = g.base * 3 / 2;
}

/* 把"当前这一屏"重画一遍。
 * 两个地方要用：① framework 把屏抢走了（守护发现像素被改）；
 * ② 从挂起回来，EPDC 丢了状态。以前遇到这两种情况屏幕就一直是花的。 */
static void redraw_current(void) {
    switch (g.scr) {
        case SCR_LIST:     draw_list();      break;
        case SCR_READER:   draw_reader();    break;
        case SCR_SETTINGS: draw_settings();  break;
        case SCR_ABOUT:    draw_about();     break;
        case SCR_MESSAGE:  draw_message();   break;
        case SCR_LOADING:  draw_loading();   break;
        case SCR_ACCOUNT:  draw_account();   break;
        case SCR_QRCODE:   draw_qrcode();    break;
        case SCR_DMLIST:   draw_dm_list();   break;
        case SCR_DMREAD:   draw_dm_read();   break;
        default:           draw_home();      break;
    }
}

/* 开发用：把一份 HTML 渲染成多张 PPM（模拟器），用来检查排版 */
int app_preview(const char *html_path, int max_pages) {
    memset(&g, 0, sizeof(g));
    config_load();
    char err[256];
    if (fb_open(&g.fb, err, sizeof(err)) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    g.fb.fast_mode = 1;
    layout_metrics();
    if (font_init(err, sizeof(err)) != 0) { fprintf(stderr, "%s\n", err); return 1; }

    size_t n = 0;
    char *html = read_file(html_path, &n);
    if (!html) { fprintf(stderr, "读不到 %s\n", html_path); return 1; }
    doc_init(&g.doc);
    html_to_doc(html, &g.doc, g_cfg.images);
    free(html);

    const char *base = strrchr(html_path, '/');
    snprintf(g.doc_title, sizeof(g.doc_title), "%s", base ? base + 1 : html_path);
    reader_layout();

    printf("画布 %dx%d 字号 %d 段落 %d 行 %d 页 %d\n", g.W, g.H, g.base, g.doc.n, g.lay.n,
           g.lay.npages);
    for (int p = 0; p < g.lay.npages && p < max_pages; p++) {
        g.page = p;
        draw_reader();
    }
    font_done();
    fb_close(&g.fb);
    return 0;
}

int app_run(void) {
    memset(&g, 0, sizeof(g));
    config_load();

    char err[256];
    if (fb_open(&g.fb, err, sizeof(err)) != 0) {
        fprintf(stderr, "zhihu: %s\n", err);
        logmsg("启动失败: %s", err);
        return 1;
    }
    logmsg("app_run: 屏幕 %dx%d %dbpp eink=%d", fb_w(&g.fb), fb_h(&g.fb), g.fb.bpp, g.fb.eink_kind);
    g.fb.fast_mode = g_cfg.fast;
    fb_set_rotate(&g.fb, g_cfg.rotate);
    layout_metrics();
    /* 影子副本：与系统界面共存时，cvm 随时可能往同一块屏上画顶栏。
     * 有了它就能整页回灌（毫秒级），不用重新排版，桌面基本露不出来。 */
    fb_shadow_enable(&g.fb);

    if (font_init(err, sizeof(err)) != 0) {
        fb_clear(&g.fb, 255);
        text_at(g.margin, g.base * 3, g.base, "知乎阅读", 0, 1);
        text_at(g.margin, g.base * 5, g.base * 3 / 4, err, 0, 0);
        present_full();
        logmsg("启动失败: %s", err);
        sleep(8);
        return 1;
    }
    logmsg("app_run: 字体 %s (%d)", font_info(), font_nfaces());

    /* 触摸映射的坐标变换由 input.c 按"原始范围 vs 画布"自动判断换不换轴，
     * 这里把三个数一起记下来：画布、原始范围、旋转。旋转后点不动/点错位，
     * 看这一行就能判断是映射问题还是触摸没进来。 */
    int inp = input_open(g.fb.width, g.fb.height);
    input_set_rotate(g_cfg.rotate, g.fb.width, g.fb.height);
    {
        int minx = 0, maxx = 0, miny = 0, maxy = 0;
        input_get_raw_range(&minx, &maxx, &miny, &maxy);
        /* 这一行是"点不准"的第一现场：画布（逻辑）、物理屏、旋转、
         * 触摸原始范围。四个数对不上就能立刻判断是映射还是设备问题。 */
        logmsg("app_run: 画布 %dx%d 物理 %dx%d rot=%d 触摸范围 %d..%d/%d..%d",
               g.W, g.H, g.fb.width, g.fb.height, g_cfg.rotate,
               minx, maxx, miny, maxy);
        logmsg("app_run: 触摸映射自检 %s", input_rotate_selftest() ? "ok" : "FAIL");
    }
    logmsg("app_run: 输入 %s", input_probe_text());
    /* 触摸独占的结果必须写清楚：拿不到独占说明 framework 还攥着屏，
     * 这时点哪儿都不会有反应——日志里一行就能定位，不用再猜。 */
    int grab = input_grab_status();
    logmsg("app_run: 触摸独占 grip=%s", grab == 1 ? "ok" : (grab == -1 ? "busy(被系统占用)" : "none"));
    api_set_home(zh_home());
    api_load_cookie();
    if (account_has_login())
        snprintf(g.acc_user, sizeof(g.acc_user), "已登录（Cookie）");

    g.scr = SCR_HOME;
    g.list_sel = 0;
    /* 首帧标记：这一段之后如果日志就断了，说明整机是在下面这行
     * draw_home() 的刷新 ioctl 里被内核 panic 掉的，而不是普通崩溃。
     * 用 logmsg_sync 强制落盘——panic 时页缓存里的最后几行会丢。 */
    logmsg_sync("pre-first-present eink=%d ioc=%08x size=%d dir=%d base=%u yv=%d",
                g.fb.eink_kind, g.fb.send_update_ioctl, g.fb.update_size,
                g.fb.update_dir, (unsigned)g.fb.base_off, g.fb.yres_virtual);
    draw_home();
    logmsg_sync("post-first-present ok eink=%d", g.fb.eink_kind);
    if (inp != 0) {
        /* 没有输入设备时至少让人知道为什么点不动。
         * 这类提示必须能自己消失——触摸本来就不通，靠点击关闭等于死锁。 */
        show_message("没有找到触摸屏或按键设备，无法操作。\n请确认是越狱的 Kindle，并查看日志。");
        g.msg_until = now_ms_ui() + 10000;
    } else if (grab == -1) {
        /* 拿不到独占时也要说清楚：不是程序坏了，是系统界面还占着触摸屏。
         * 这条提示以前完全没有，用户只能看到"点了没反应"。
         * 同样要自动消失：8 秒后回首页，程序还活着，补抢也可能成功。 */
        show_message("触摸屏被系统界面占用，点击可能无响应。\n"
                     "正在后台每 2 秒重试独占；若一直不行，\n"
                     "请在 config.txt 里加一行 framework=stop 后重开。");
        g.msg_until = now_ms_ui() + 8000;
    }
    logmsg("app_run: 进入主循环");

    long hb = now_ms_ui();
    /* 1.2.9：硬件旋转轮询基线（KO3 翻转设备时 framework 会转显示方向） */
    long rot_ms = hb;
    /* 挂起检测基线：/proc/uptime 走的是"含挂起"的时钟，now_ms_ui 是单调时钟。
     * 两者的差一旦变大，就说明设备在这段时间里睡过一次——
     * "停掉 framework 后自动挂起 → 唤醒失败 → 整机重启"这条链就靠它来证伪/证实。 */
    long up0 = sys_uptime_sec(), mono0 = now_ms_ui() / 1000;
    long sleep_seen = 0;
    g.idle_since = now_ms_ui();
    g.guard_ms = g.idle_since;
    g.guard_first = g.idle_since;
    long guard_hold = 0;        /* 连续抢屏时的退避截止时间 */
    int  guard_streak = 0;

    while (!g.quit) {
        /* SIGTERM/SIGINT：不在这里被砍死，走正常退出路径。
         * 否则 cvm 会一直冻着，framework 起来后弹「应用程序错误」。 */
        if (app_quit_signal()) {
            /* 把"谁发的、跑了多久"一起记下来。
             * 1.2.3 的 rc=143 就是这么被杀的，但日志里没有时间和来源，
             * 只能靠推理。这里是实锤入口：如果是固定 3~4 分钟、父进程是
             * 某个系统进程，那就能直接定位到看门狗。 */
            char pc[64] = "?";
            char pp[64];
            pid_t ppid = getppid();
            snprintf(pp, sizeof(pp), "/proc/%d/comm", (int)ppid);
            if (read_file_into(pp, pc, sizeof(pc)) > 0) {
                size_t k = strlen(pc);
                while (k && (pc[k - 1] == '\n' || pc[k - 1] == ' ')) pc[--k] = 0;
            }
            logmsg("收到信号 %d，开始正常退出（已运行 %lds，ppid=%d comm=%s）",
                   app_quit_signal(), sys_uptime_sec(), (int)ppid, pc);
            g.quit = 1;
            break;
        }

        long t0 = now_ms_ui();
        qr_tick();          /* 二维码登录轮询：不切屏、不阻塞主循环 */
        img_tick();         /* 正文图片后台下载：到一张补一张 */
        long t_qr = now_ms_ui() - t0;

        Ev ev;
        /* 120ms 而不是 500ms：1.2.4 的"偶尔看到桌面"就是因为轮询粗，
         * cvm 画完顶栏后最长要等 ~1.5 秒我们才发现。eink 上 120ms 一次
         * select 几乎不耗电，换来的却是"桌面基本看不见"。 */
        int got = input_poll(&ev, 120);
        long t_in = now_ms_ui() - t0;

        if (got == 0) {
            long now = now_ms_ui();

            /* ---- 硬件旋转跟随（1.2.9）----
             * KO3 翻转设备时 framework 会把显示方向转过去，我们的像素跟着
             * 被转（"界面旋转了"）但触摸没跟（"触摸没旋转"）。每秒查一次
             * var.rotate，变了就把绘制方向与触摸映射一起补偿。 */
            if (now - rot_ms > 1000) {
                rot_ms = now;
                if (fb_poll_rotate(&g.fb) == 1) {
                    int ui = ((g_cfg.rotate - g.fb.hw_rotate) % 4 + 4) % 4;
                    fb_set_rotate(&g.fb, ui);
                    input_set_rotate(ui, g.fb.width, g.fb.height);
                    layout_metrics();
                    if (g.doc.n) { reader_layout(); if (g.page >= g.lay.npages) g.page = 0; }
                    redraw_current();
                    present_full();
                    logmsg("旋转跟随: hw=%d -> 界面=%d°，触摸映射已同步（config.txt 可写 autorot=0 关闭）",
                           g.fb.hw_rotate, ui * 90);
                }
            }

            /* ---- 屏幕归属守护 ----
             * 默认与 framework 共存，cvm 可能把屏抢回去。每秒抽查一次
             * 我们画的那页有没有被人改过；改了就把当前这屏重画一遍。
             * 连续被抢会退避，避免"每秒钟闪一次"比"被盖掉"还难受。 */
            if (now - g.guard_ms > 120) {
                g.guard_ms = now;
                if (now >= guard_hold) {
                    uint32_t sig = g.screen_sig;
                    int r = fb_guard(&g.fb, &sig);
                    g.screen_sig = sig;
                    if (r) {
                        g.guard_hits++;
                        guard_streak++;
                        /* 优先用影子副本整页回灌：一次 memcpy + 一次全刷，
                         * 比重排版快一个数量级，桌面残留的窗口从"一秒"缩到
                         * "一两百毫秒"，eink 上基本看不见。
                         * 影子副本不可用（内存不足）才退回重新画。 */
                        int restored = (r == 1) ? fb_restore(&g.fb) : -1;
                        if (restored != 0) {
                            fb_clear(&g.fb, 255);   /* 先整屏清白，别把桌面像素留在缝里 */
                            redraw_current();
                        }
                        if (g.guard_hits <= 3 || g.guard_hits % 20 == 0)
                            logmsg("屏幕守护: %s，%s scr=%d（累计 %d 次）",
                                   r == 2 ? "可见页被翻走" : "像素被别的进程改过",
                                   restored == 0 ? "影子回灌" : "整页重画",
                                   g.scr, g.guard_hits);

                        /* 1.2.10：被抢只回灌，不再"请求启动器停系统界面"——
                         * 1.2.9 时代的 .want_stop 自动升级会让 cvm 死掉，
                         * framework 看门狗宽限期后整机重启（真机两次）。
                         * 抢屏由上面的影子回灌/整页重画自己解决。 */

                        if (guard_streak >= 3) {
                            guard_hold = now_ms_ui() + 3000;
                            guard_streak = 0;
                            logmsg("屏幕守护: 连续被抢，退避 3 秒");
                        }
                    } else {
                        guard_streak = 0;
                    }
                }
            }

            /* ---- 自动关闭的提示 ---- */
            if (g.msg_until && now > g.msg_until) {
                g.msg_until = 0;
                logmsg("提示已自动关闭，回到首页（触摸可能不通，不能指望点击）");
                g.scr = SCR_HOME;
                draw_home();
            }

            /* ---- 补抢触摸独占 ----
             * 启动时可能正赶上系统重新拉起 cvm，那一下抢不到就永远收不到点击。
             * 每两秒补一次，抢到就继续用（日志里能看到几秒后恢复可用）。 */
            if (now - g.grab_ms > 2000 && input_grab_status() <= 0) {
                g.grab_ms = now;
                input_retry_grab();
            }

            /* ---- 触摸活性检查 ----
             * 屏幕上点十几次都没收到一个事件，基本只能是设备没给到我们。
             * 30 秒报一次，避免刷屏。 */
            if (now - g.idle_since > 30000 && input_event_count() == 0) {
                logmsg("输入: 30 秒内一个触摸事件都没收到（grip=%d）——触摸可能没接进来",
                       input_grab_status());
                g.idle_since = now;
            }

            /* 每分钟记一次心跳：真机上出问题时，从日志就能看出是"启动即崩"
             * 还是"跑了一阵才崩"，以及内存涨没涨 */
            if (now_ms_ui() - hb > 60000) {
                hb = now_ms_ui();
                long rss = 0;
                /* /proc 文件的 stat 大小是 0，原来用 read_file 读出来恒为空，
                 * 于是"内存=0KB"是个假指标（不是 OOM）。改定长直读。 */
                char st[256];
                if (read_file_into("/proc/self/statm", st, sizeof(st)) > 0) {
                    long total = 0, resident = 0;
                    if (sscanf(st, "%ld %ld", &total, &resident) == 2 && resident > 0)
                        rss = resident * (sysconf(_SC_PAGESIZE) / 1024);
                }
                long up = sys_uptime_sec(), mono = now_ms_ui() / 1000;
                long drift = (up - up0) - (mono - mono0);
                long slept = drift - sleep_seen;
                if (slept > 3) {
                    sleep_seen = drift;
                    logmsg("心跳: 刚挂起过 %ld 秒（累计 %ld 秒）", slept, sleep_seen);
                    /* 挂起回来 EPDC 可能黑屏，补一次 unblank + 重画 */
                    fb_after_resume(&g.fb);
                    redraw_current();
                }
                /* 列表条数和登录态一起记：真机上"页=1/0"到底是没进阅读页，
                 * 还是进了但排版结果为空，看这两个值就能区分。
                 * 触摸计数和抢屏次数也带上：下次"点不动"能一眼分成
                 * "没收到事件"和"收到了但没处理"。 */
                /* 1.2.10：framework 状态进心跳——真机日志"戛然而止"（无
                 * *** CRASH 也无正常退出）就是被 framework 看门狗 SIGKILL，
                 * 看这行就知道当时是不是 stop 模式、停了多久。 */
                char fwbuf[48];
                {
                    char pp[320];
                    struct stat fst;
                    snprintf(pp, sizeof(pp), "%s/.framework_stopped", zh_home());
                    if (stat(pp, &fst) == 0) {
                        long stopped = (long)time(NULL) - (long)fst.st_mtime;
                        if (stopped < 0) stopped = 0;
                        snprintf(fwbuf, sizeof(fwbuf), "stop %lds", stopped);
                    } else {
                        snprintf(fwbuf, sizeof(fwbuf), "coexist");
                    }
                }
                logmsg("心跳 scr=%d 页=%d/%d 列表=%d 登录=%d 内存=%ldKB 挂起=%lds "
                       "刷新=%d eips=%d/%d 触摸=%ld 点击=%ld 抢屏=%d 开机=%lds fw=%s",
                       g.scr, g.page + 1, g.lay.npages, g.list.n, account_has_login(),
                       rss, sleep_seen, g.fb.eink_kind, g.fb.eips_ok, g.fb.eips_fail,
                       input_event_count(), input_action_count(), g.guard_hits,
                       sys_uptime_sec(), fwbuf);
            }
            continue;
        }

        g.idle_since = now_ms_ui();
        if (getenv("ZH_DEBUG")) fprintf(stderr, "[ev] type=%d x=%d y=%d code=%d scr=%d\n", ev.type, ev.x, ev.y, ev.code, g.scr);
        if (ev.type == ZH_TAP) handle_tap(ev.x, ev.y);
        else if (ev.type == ZH_SWIPE_L) {
            if (g.scr == SCR_READER || g.scr == SCR_DMREAD) reader_goto(g.page + 1);
            else if (g.scr == SCR_LIST || g.scr == SCR_DMLIST) { g.scr = SCR_HOME; draw_home(); }
        } else if (ev.type == ZH_SWIPE_R) {
            if (g.scr == SCR_READER || g.scr == SCR_DMREAD) reader_goto(g.page - 1);
        } else if (ev.type == ZH_KEY) handle_key(ev.code);

        /* ---- 主循环看门狗 ----
         * 真机"卡死"以前只能猜。这里把一次循环的耗时拆开记：
         * 是 qr_tick、input、还是这次事件的处理（多半是刷屏 ioctl 卡住）。
         * 超过 1.5 秒就写一行，平时一行都不写。 */
        long dt = now_ms_ui() - t0;
        if (dt > 1500)
            logmsg("主循环卡顿 %ldms（qr=%ldms 输入=%ldms）事件=%d scr=%d 刷新=%d",
                   dt, t_qr, t_in, ev.type, g.scr, g.fb.eink_kind);
        else if (t_qr > 800)
            logmsg("qr_tick 慢了 %ldms", t_qr);
    }

    qr_stop();
    img_stop();
    logmsg("app_run: 用户退出");
    fb_clear(&g.fb, 255);
    text_at(g.margin, g.base * 2, g.base, "知乎阅读已退出", 0, 1);
    /* 这一行必须折行：1264 宽的屏上全句一行放不下，直接 text_at 会被
     * 屏幕右边缘切掉半句（用户看到的是"……长按电源键 20 秒重"） */
    draw_wrapped(g.margin, g.base * 4, g.W - 2 * g.margin, g.base * 3 / 4,
                 "Kindle 系统界面即将恢复；若没恢复，长按电源键 20 秒重启。",
                 70, g.H - g.footer_h);
    present_full();

    input_close();
    font_done();
    fb_close(&g.fb);
    return 0;
}
