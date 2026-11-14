/* main.c —— 独立原生 Kindle 应用的入口
 *
 * 启动方式（三种都行）：
 *   1) KUAL 里点「知乎阅读」
 *   2) SSH: /mnt/us/zhihu/zhihu
 *   3) 通过 launchpad / 开机自启脚本
 * 程序运行时不依赖电脑、不依赖 KOReader、不依赖任何解释器。
 */
#include "account.h"
#include "api.h"
#include "common.h"
#include "content.h"
#include "fb.h"
#include "font.h"
#include "img.h"
#include "input.h"
#include "md5.h"
#include "png.h"
#include "qr.h"
#include "social.h"
#include "ui.h"
#include "json.h"
#include "layout.h"
#include "zse96.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "ui.h"   /* 只为 ZH_VERSION */

/* ---------------- 崩溃现场记录 ----------------
 * Kindle 上没有 gdb，程序一崩就只剩"回到主页"，什么线索都没有。
 * 这里把信号、出错地址、PC/LR 直接追加到 log.txt，并由 start.sh 显示到屏幕上。
 */
static volatile int g_in_crash = 0;

static void crash_log(const char *text, int len) {
    char path[320];
    snprintf(path, sizeof(path), "%s/log.txt", zh_home());
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, text, (size_t)len);
    (void)w;
    fsync(fd);
    close(fd);
}

/* ---------------- 退出时还原系统界面 ----------------
 * 恢复路径以前只有 start.sh 的 trap 一条。脚本被强杀（framework 重启时
 * 常按进程组 kill -TERM，会把 KUAL 拉起来的脚本一起带走）就没人还原，
 * cvm 一直冻着 → framework 弹出「应用程序错误」。
 * 这里让程序自己也会还原，两条路互为备份。
 * safe=1 时只用信号安全的调用（open/read/write/kill/unlink），
 * 因为这部分可能在 SIGALRM 里执行。 */
static void run_quiet(const char *prog, const char *a1, const char *a2,
                      const char *a3, const char *a4) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
        char *argv[6];
        int i = 0;
        argv[i++] = (char *)prog;
        if (a1) argv[i++] = (char *)a1;
        if (a2) argv[i++] = (char *)a2;
        if (a3) argv[i++] = (char *)a3;
        if (a4) argv[i++] = (char *)a4;
        argv[i] = NULL;
        execvp(prog, argv);
        _exit(127);
    }
    /* 最多等 2 秒：powerd 死掉时 lipc 会一直挂着，不能把我们退出拖住 */
    for (int k = 0; k < 40; k++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
}

static void restore_system_ui(int safe) {
    char p[320];

    /* 1) 被冻结的 cvm：放它回来。
     * 即使不是我们冻的（文件不存在）也只是白读一次，幂等。 */
    snprintf(p, sizeof(p), "%s/.cvm_frozen", zh_home());
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char b[32];
        ssize_t n = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            int pid = atoi(b);
            if (pid > 0 && kill(pid, SIGCONT) == 0)
                logmsg_sync("退出: 恢复 cvm pid=%d（SIGCONT）", pid);
        }
        unlink(p);
    }

    /* 2) 内核 wakelock：只释放我们自己那把锁的名字 */
    int w = open("/sys/power/wake_unlock", O_WRONLY);
    if (w >= 0) {
        ssize_t k = write(w, "zhihu_reading\n", 14);
        (void)k;
        close(w);
    }

    if (safe) return;

    /* 3) powerd 的防休眠开关关掉（start.sh 正常情况下也会做） */
    run_quiet("lipc-set-prop", "-i", "com.lab126.powerd", "preventScreenSaver", NULL);

    /* 4) 我们把 framework 停过、而 start.sh 已经不在（标记文件没人清）：
     * 这里补一次启动。先删标记再启动，保证 start.sh 的 trap 看到标记消失
     * 就不会重复 start（重复 start 会起两个 cvm，比不启动更糟）。 */
    snprintf(p, sizeof(p), "%s/.framework_stopped", zh_home());
    if (file_exists(p)) {
        unlink(p);
        logmsg_sync("退出: 检测到 framework 是我们停的，补一次启动");
        run_quiet("/etc/init.d/framework", "start", NULL, NULL, NULL);
    }
}

static void restore_atexit(void) { restore_system_ui(0); }

static void on_fatal(int sig, siginfo_t *si, void *uc_) {
    if (g_in_crash++) _exit(128 + sig);

    char buf[512];
    unsigned long pc = 0, lr = 0, sp = 0;
#if defined(__arm__)
    {
        ucontext_t *uc = (ucontext_t *)uc_;
        pc = (unsigned long)uc->uc_mcontext.arm_pc;
        lr = (unsigned long)uc->uc_mcontext.arm_lr;
        sp = (unsigned long)uc->uc_mcontext.arm_sp;
    }
#endif
    /* 整行保持纯 ASCII：日志要能在 eips 上原样显示（Kindle 系统字体没有中文） */
    const char *nm = "?";
    switch (sig) {
        case SIGSEGV: nm = "SIGSEGV"; break;
        case SIGBUS:  nm = "SIGBUS";  break;
        case SIGILL:  nm = "SIGILL";  break;
        case SIGFPE:  nm = "SIGFPE";  break;
        case SIGABRT: nm = "SIGABRT"; break;
        default: break;
    }
    int n = snprintf(buf, sizeof(buf),
                     "*** CRASH sig=%d (%s) addr=%p pc=%08lx lr=%08lx sp=%08lx ver=%s\n",
                     sig, nm, si ? si->si_addr : (void *)0, pc, lr, sp, ZH_VERSION);
    if (n > 0) crash_log(buf, n);
    _exit(128 + sig);
}

static void on_term(int sig) {
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "*** 收到信号 %d，准备正常退出并还原系统界面\n", sig);
    if (n > 0) crash_log(buf, n);
    /* 不再 _exit()：1.2.3 就是这样把进程砍在绘图中间，
     * start.sh 的 trap 还没跑到，cvm 一直是冻着的，framework 起来后
     * 认为自家 UI 异常，于是弹「应用程序错误」。
     * 现在只置位，主循环走正常退出路径；万一主循环卡在 ioctl 里出不来，
     * 6 秒后 SIGALRM 在信号上下文里兜底还原。 */
    app_quit_request(sig);
    alarm(6);
}

/* 兜底：主循环没在 6 秒内退出（大概率卡在某个 ioctl 上），
 * 这里只做信号安全的事——读写文件、kill，绝不 malloc / printf / fork。 */
static void on_alarm(int sig) {
    (void)sig;
    restore_system_ui(1);
    _exit(143);
}

static void install_crash_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    struct sigaction st;
    memset(&st, 0, sizeof(st));
    st.sa_handler = on_term;
    sigemptyset(&st.sa_mask);
    sigaction(SIGTERM, &st, NULL);
    sigaction(SIGINT, &st, NULL);
    sigaction(SIGHUP, &st, NULL);

    struct sigaction al;
    memset(&al, 0, sizeof(al));
    al.sa_handler = on_alarm;
    sigemptyset(&al.sa_mask);
    sigaction(SIGALRM, &al, NULL);
}

static int run_selftest(void) {
    int pass = 0, fail = 0;
    char hex[33];

    struct { const char *in; const char *want; } vec[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6" },
        { "中文测试", "089b4943ea034acfa445d050c7913e55" },
    };
    for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        md5_hex(vec[i].in, strlen(vec[i].in), hex);
        int ok = strcmp(hex, vec[i].want) == 0;
        printf("[md5 ] %-30s %s\n", vec[i].in[0] ? vec[i].in : "(empty)", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    char sig1[128], sig2[128];
    zse96_sign("https://www.zhihu.com/api/v3/feed/topstory/hot-list-web?limit=50&desktop=true",
               "test_dc0", NULL, sig1, sizeof(sig1));
    zse96_sign("https://www.zhihu.com/api/v3/feed/topstory/hot-list-web?limit=50&desktop=true",
               "test_dc0", NULL, sig2, sizeof(sig2));
    int ok = strcmp(sig1, sig2) == 0 && strncmp(sig1, "2.0_", 4) == 0;
    printf("[zse96] %s %s\n", ok ? "OK" : "FAIL", sig1);
    ok ? pass++ : fail++;

    const char *js = "{\"a\":[1,2,{\"b\":\"\\u77e5\\u4e4e\"}],\"n\":null,\"t\":true}";
    JVal *j = json_parse(js, strlen(js));
    ok = j && json_count(json_get(j, "a")) == 3;
    printf("[json ] %s\n", ok ? "OK" : "FAIL");
    ok ? pass++ : fail++;
    json_free(j);

    /* 正文清洗：script/style 丢弃、实体解码、段落切分 */
    {
        Doc d;
        doc_init(&d);
        html_to_doc("<p>第一段</p><p>第二 &amp; 段</p><script>x=1</script><h2>标题</h2>", &d, 0);
        ok = (d.n == 3) && strcmp(d.v[0].text, "第一段") == 0 &&
             strstr(d.v[1].text, "&") != NULL && d.v[2].kind == BLK_H2;
        printf("[html ] %s 段落=%d\n", ok ? "OK" : "FAIL", d.n);
        ok ? pass++ : fail++;
        doc_free(&d);
    }

    /* 二维码：设备端本地生成登录码，验证矩阵尺寸 / 静区 / 黑白分布 */
    {
        const char *link =
            "https://www.zhihu.com/account/scan/login/NThiNDg5ZTctNjBh?/api/login/qrcode";
        uint8_t *bits = NULL;
        int n = 0, black = 0;
        int rc = qr_matrix(link, &bits, &n);
        if (rc == 0 && bits)
            for (int i = 0; i < n * n; i++) black += bits[i];
        /* 四个角必须落在静区（白），且黑白都要有，才算可扫 */
        int quiet_ok = rc == 0 && n > 20 && bits &&
                       bits[0] == 0 && bits[n - 1] == 0 &&
                       bits[(n - 1) * n] == 0 && bits[n * n - 1] == 0;
        ok = rc == 0 && n >= 21 && quiet_ok &&
             black > n * n / 20 && black < n * n * 3 / 4;
        printf("[qr   ] %s 边长=%d 黑块=%d\n", ok ? "OK" : "FAIL", n, black);
        ok ? pass++ : fail++;
        free(bits);
    }

    /* 登录会话：cookie 在 fork 父子进程间往返 + z_c0 登录判定 */
    {
        api_set_cookie("");     /* 只动内存，selftest 是独立进程，不碰设备上的 cookie.txt */
        const char *ck = "_xsrf=abc; _zap=def; d_c0=GHJK; z_c0=TOKEN123";
        account_session_set(ck);
        /* get() 返回的是静态缓冲区指针，set("") 会把内容覆盖掉，必须先做快照 */
        char snap[256];
        snprintf(snap, sizeof(snap), "%s", account_session_get());
        int has = account_has_login();
        account_session_set("");
        int no = account_has_login();
        ok = strcmp(snap, ck) == 0 && has == 1 && no == 0;
        printf("[acct ] %s 往返=%s 判定=%d/%d\n", ok ? "OK" : "FAIL", snap, has, no);
        ok ? pass++ : fail++;
    }

    /* 私信：fork 父子进程之间「结构体数组落盘 → 读回」这条链路 */
    {
        char path[320];
        snprintf(path, sizeof(path), "%s/selftest_threads.bin", zh_home());
        MsgThread arr[2];
        memset(arr, 0, sizeof(arr));
        snprintf(arr[0].peer, sizeof(arr[0].peer), "张三");
        snprintf(arr[0].snippet, sizeof(arr[0].snippet), "你好");
        arr[0].unread = 2;
        snprintf(arr[1].peer, sizeof(arr[1].peer), "李四");
        snprintf(arr[1].snippet, sizeof(arr[1].snippet), "在吗");
        FILE *f = fopen(path, "wb");
        int cnt = 2;
        if (f) {
            fwrite(&cnt, sizeof(cnt), 1, f);
            fwrite(arr, sizeof(MsgThread), 2, f);
            fclose(f);
        }
        ThreadList tl;
        int rc = social_threads_load(path, &tl);
        ok = f && rc == 0 && tl.n == 2 &&
             strcmp(tl.v[0].peer, "张三") == 0 &&
             strcmp(tl.v[1].snippet, "在吗") == 0 && tl.v[0].unread == 2;
        printf("[dm   ] %s 会话=%d\n", ok ? "OK" : "FAIL", tl.n);
        ok ? pass++ : fail++;
        thread_list_free(&tl);
        unlink(path);
    }

    /* 字体缓存压力：长时间阅读后不能出现随机缺字 */
    {
        char ferr[256] = "";
        if (font_init(ferr, sizeof(ferr)) == 0) {
            ok = font_cache_stress() == 1;
            printf("[font ] %s 高负载下 7424 个字形无丢失\n", ok ? "OK" : "FAIL");
            ok ? pass++ : fail++;
            font_done();
        } else {
            printf("[font ] SKIP 本机没有可用字体（%s）\n", ferr);
        }
    }

    /* 列表解析的「错误可见性」：这三种响应都不允许静默变成空列表。
     * 真机上"点进去一片空白"就是这里没兜住，所以固化成自检。 */
    {
        ApiList l;

        /* 1) HTTP 200 + {"error":{code,message}}：知乎风控的常见形态，必须报错 */
        memset(&l, 0, sizeof(l));
        const char *e1 = "{\"error\":{\"code\":40352,\"message\":\"网络环境异常\"}}";
        int r1 = api_parse_feed(e1, strlen(e1), &l);
        int m1 = (r1 != 0) && strstr(api_error(), "40352") != NULL;
        api_list_free(&l);

        /* 2) HTTP 200 + {"data":[]}：解析成功但 0 条，必须留下可读诊断 */
        memset(&l, 0, sizeof(l));
        const char *e2 = "{\"data\":[]}";
        int r2 = api_parse_feed(e2, strlen(e2), &l);
        int m2 = (r2 == 0) && l.n == 0 && api_error()[0] != 0;
        api_list_free(&l);

        /* 3) 正常数据仍要照常解析出来（别把修复做成误伤） */
        memset(&l, 0, sizeof(l));
        const char *e3 = "{\"data\":[{\"target\":{\"title\":\"测试标题\","
                         "\"url\":\"https://www.zhihu.com/question/1\",\"id\":1}}]}";
        int r3 = api_parse_feed(e3, strlen(e3), &l);
        int m3 = (r3 == 0) && l.n == 1 && strcmp(l.v[0].title, "测试标题") == 0;
        api_list_free(&l);

        ok = m1 && m2 && m3;
        printf("[feed ] %s error=%d 空列表有原因=%d 正常解析=%d\n",
               ok ? "OK" : "FAIL", m1, m2, m3);
        ok ? pass++ : fail++;
    }

    /* EPDC ioctl 编码：方向位必须和内核一致。
     * 1.2.1 把 _IOWR 写成 _IOW，KO3 上全尺寸探测都 ENOTTY，
     * 退到 FBIOPAN_DISPLAY 后整机重启。用系统宏算标准值逐尺寸比对。 */
    {
        int bad = 0;
        static const int sizes[] = { 72, 64, 56, 48, 40, 32 };
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            unsigned int want, got;
            switch (sizes[i]) {
                case 72: want = (unsigned int)_IOWR('F', 0x2E, char[72]); break;
                case 64: want = (unsigned int)_IOWR('F', 0x2E, char[64]); break;
                case 56: want = (unsigned int)_IOWR('F', 0x2E, char[56]); break;
                case 48: want = (unsigned int)_IOWR('F', 0x2E, char[48]); break;
                case 40: want = (unsigned int)_IOWR('F', 0x2E, char[40]); break;
                default: want = (unsigned int)_IOWR('F', 0x2E, char[32]); break;
            }
            got = fb_send_update_cmd(sizes[i]);
            if (want != got) {
                bad++;
                printf("[ioc  ] size=%d 期望 %08x 实际 %08x\n", sizes[i], want, got);
            }
        }
        ok = (bad == 0);
        printf("[ioc  ] %s MXCFB_SEND_UPDATE 用 _IOWR，6 个尺寸编码全对\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* URL -> id 抽取：1.2.4 的「点进去 404」有一半出在这里
     * （链接形态没认出来 → id 空 → 请求 /api/v4/answers/ → 404）。 */
    {
        ok = api_ids_selftest() == 1;
        printf("[ids  ] %s 8 种链接形态都能抽出正确的 question/answer id\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 19 位 id：这是"点进去 404"的根因之一。
     * 知乎的 id 已经超过 2^53，用 double 解析会变成别的数
     *（2103784826771862921 -> 2103784826771863040），32 位 long 更存不下。
     * 必须按原始字面量取。 */
    {
        const char *j = "{\"id\":2103784826771862921,\"qid\":\"2103584365720744278\","
                        "\"flag\":1.5,\"small\":42}";
        JVal *r = json_parse(j, strlen(j));
        char a[64] = {0}, b[64] = {0}, c[64] = {0};
        int r1 = r ? json_get_idstr(r, "id", a, sizeof(a)) : -1;
        int r2 = r ? json_get_idstr(r, "qid", b, sizeof(b)) : -1;
        int r3 = r ? json_get_idstr(r, "flag", c, sizeof(c)) : -1;   /* 小数不是 id */
        int m1 = (r1 == 0) && strcmp(a, "2103784826771862921") == 0;
        int m2 = (r2 == 0) && strcmp(b, "2103584365720744278") == 0;
        int m3 = (r3 != 0);
        long s1 = r ? json_get_int(r, "small", 0) : 0;
        int m4 = (s1 == 42);
        if (r) json_free(r);
        ok = m1 && m2 && m3 && m4;
        printf("[id64 ] %s 19 位 id 按字面量取（数字=%d 字符串=%d 小数不算 id=%d）\n",
               ok ? "OK" : "FAIL", m1, m2, m3);
        ok ? pass++ : fail++;
    }

    /* 触摸随旋转：显示按 pix_off 转，触摸必须按它的逆变换转。
     * 1.2.4 只改显示不改触摸，于是除 0° 外三个方向全错位。 */
    {
        ok = input_rotate_selftest() == 1;
        printf("[rot  ] %s 四个方向的触摸映射与显示旋转互为逆变换\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 影子副本 + 屏幕守护：这是 1.2.5 "看不到桌面残留" 的整套机制，
     * 用一块假 framebuffer 把三条行为钉死：
     *   没人动 -> 不能误报（否则每 120ms 重刷一次，比被盖掉还难受）
     *   被改过 -> 必须发现
     *   回灌后 -> 必须和影子完全一致（不然就是"越修越花"） */
    {
        Fb t;
        memset(&t, 0, sizeof(t));
        t.fd = -1;
        t.width = 320; t.height = 200; t.bpp = 8; t.line_len = 320;
        t.memlen = (size_t)t.line_len * t.height;
        t.mem = (uint8_t *)malloc(t.memlen);
        t.eink_kind = 3;        /* 只写内存：不需要真设备，也不会碰 ioctl */
        int ok1 = 0, ok2 = 0, ok3 = 0, ok4 = 0;
        if (t.mem) {
            memset(t.mem, 255, t.memlen);
            ok1 = (fb_shadow_enable(&t) == 0);
            fb_present(&t, 1);                      /* 同步影子副本 */
            uint32_t sig = 0;
            ok2 = (fb_guard(&t, &sig) == 0);        /* 没人动：不该报 */
            t.mem[1000] = 0;                        /* 模拟 cvm 画了顶栏 */
            ok3 = (fb_guard(&t, &sig) == 1);        /* 被改：必须报 */
            ok4 = (fb_restore(&t) == 0) && t.mem[1000] == 255;
            free(t.mem);
            t.mem = NULL;
            free(t.shadow);
        }
        ok = ok1 && ok2 && ok3 && ok4;
        printf("[shad ] %s 影子副本：启用=%d 不误报=%d 能发现=%d 回灌=%d\n",
               ok ? "OK" : "FAIL", ok1, ok2, ok3, ok4);
        ok ? pass++ : fail++;
    }

    /* config.txt 的手写项必须活过「保存设置」 */
    {
        ok = ui_config_roundtrip_test() == 1;
        printf("[cfg  ] %s 手写 refresh=/eips=/blankpulse= 读完存完仍在\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* PNG 编码：KO3 上刷屏要靠 eips -g 吃一张整页 PNG，
     * 格式错一个字节就是整屏不出图，而且肉眼很难和"驱动不刷"区分。 */
    {
        int w = 64, h = 40;
        uint8_t *px = xmalloc((size_t)w * (size_t)h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                px[(size_t)y * w + x] = (uint8_t)((x * 4 + y * 6) & 0xFF);
        char path[320];
        snprintf(path, sizeof(path), "%s/selftest.png", zh_home());
        char perr[128] = "";
        int gw = 0, gh = 0;
        int rcw = png_write_gray8(path, w, h, px, w);
        int okv = (rcw == 0) && png_verify(path, &gw, &gh, perr, sizeof(perr));
        ok = okv && gw == w && gh == h;
        printf("[png ] %s %dx%d %s\n", ok ? "OK" : "FAIL", gw, gh,
               ok ? "签名/CRC/IEND 全对" : perr);
        ok ? pass++ : fail++;
        unlink(path);
        free(px);
    }

    /* 屏幕归属守护：与 framework 共存时，cvm 随时可能把我们的像素改掉。
     * 这里用一块假 framebuffer 验证"改了能发现、没改不误报"。 */
    {
        Fb fb;
        memset(&fb, 0, sizeof(fb));
        int w = 64, h = 32;
        fb.mem = xmalloc((size_t)w * (size_t)h);
        memset(fb.mem, 255, (size_t)w * (size_t)h);
        fb.memlen = (size_t)w * (size_t)h;
        fb.line_len = w;
        fb.height = h;
        fb.base_off = 0;
        fb.eink_kind = 1;
        fb.fd = -1;
        uint32_t sig = fb_sample_sig(&fb);
        int same = (fb_guard(&fb, &sig) == 0);
        fb.mem[485] = 0;                 /* 97×5：一定落在采样点上 */
        int chg = (fb_guard(&fb, &sig) == 1);
        free(fb.mem);
        ok = same && chg;
        printf("[guard] %s 没人动=不误报 / 被改过=能发现\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 退出还原：程序侧必须能自己把 .cvm_frozen 里的 pid 放回来。
     * 这条链路以前只有 start.sh 的 trap 走，脚本被强杀就彻底没人管，
     * framework 起来后弹「应用程序错误」。 */
    {
        char p[320];
        snprintf(p, sizeof(p), "%s/.cvm_frozen", zh_home());
        char b[32];
        int n = snprintf(b, sizeof(b), "%d\n", (int)getpid());   /* 给自己发 SIGCONT，无害 */
        write_file_atomic(p, b, (size_t)n);
        restore_system_ui(1);
        ok = !file_exists(p);
        printf("[rvr  ] %s 退出时按 .cvm_frozen 恢复 cvm 并清理标记\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        unlink(p);
    }

    /* 翻页键独占规则：翻页键闪出原生主页就是这里没做对。
     * KO3 上翻页键和电源键在不同 gpio 设备，抢前者没问题；
     * 万一同设备，必须放弃抢（否则长按也关不了机）。 */
    {
        int r1 = input_key_grab_rule(1, 0);   /* 只有翻页键 -> 抢 */
        int r2 = input_key_grab_rule(1, 1);   /* 同设备有电源键 -> 不抢 */
        int r3 = input_key_grab_rule(0, 0);   /* 没有翻页键 -> 不抢 */
        int r4 = input_key_grab_rule(0, 1);
        ok = r1 == 1 && r2 == 0 && r3 == 0 && r4 == 0;
        printf("[grab] %s 翻页键独占规则：只抢「含翻页键且不含电源键」的设备\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 回答列表标题：一个问题下多个回答时，每行必须是"这一条回答的摘要"，
     * 1.2.5 全都退化成问题标题，整页看起来像复制粘贴。 */
    {
        const char *j = "{\"data\":[{\"type\":\"answer\",\"id\":2103784826771862921,"
                        "\"excerpt\":\"先说结论：这不是手机变贵了\","
                        "\"question\":{\"title\":\"为什么手机越来越贵\"},"
                        "\"author\":{\"name\":\"张三\"},\"voteup_count\":88}]}";
        ApiList l;
        memset(&l, 0, sizeof(l));
        int r = api_parse_feed(j, strlen(j), &l);
        ok = r == 0 && l.n == 1 &&
             strcmp(l.v[0].id, "2103784826771862921") == 0 &&
             strstr(l.v[0].title, "先说结论") != NULL &&
             strcmp(l.v[0].title, "为什么手机越来越贵") != 0;
        printf("[ans  ] %s 回答列表用摘要当标题（而不是重复问题标题）\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        api_list_free(&l);
    }

    /* 收藏夹内容：外层是 {"content":{...}}，解析器必须能钻进去。
     * 1.2.5 的「我的收藏」根本没接线，这条路径从没被验证过。 */
    {
        const char *j = "{\"data\":[{\"content\":{\"type\":\"answer\","
                        "\"id\":2103784826771862921,\"excerpt\":\"收藏的回答正文开头\","
                        "\"question\":{\"title\":\"问题\"},\"author\":{\"name\":\"李四\"},"
                        "\"voteup_count\":7}}]}";
        ApiList l;
        memset(&l, 0, sizeof(l));
        int r = api_parse_feed(j, strlen(j), &l);
        ok = r == 0 && l.n == 1 && strcmp(l.v[0].id, "2103784826771862921") == 0 &&
             strstr(l.v[0].title, "收藏的回答") != NULL && l.v[0].voteup == 7;
        printf("[fav  ] %s 收藏夹内容的 content 包装能钻进去解析\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        api_list_free(&l);
    }

    /* 推荐多页合并：同一页重复拉回来不能出现重复条目 */
    {
        const char *j = "{\"data\":[{\"target\":{\"type\":\"answer\",\"id\":111,"
                        "\"excerpt\":\"a\"}}]}";
        ApiList l;
        memset(&l, 0, sizeof(l));
        api_parse_feed(j, strlen(j), &l);
        api_parse_feed(j, strlen(j), &l);
        ok = (l.n == 1);
        printf("[rec  ] %s 多页累加时按 id 去重（重复页只留一条）\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        api_list_free(&l);
    }

    /* 私信：新版会话对象（participant / latest_message）要能解析出昵称和预览 */
    {
        const char *j = "{\"data\":[{\"id\":\"21037848\",\"participant\":{\"name\":\"李四\"},"
                        "\"latest_message\":{\"content\":\"在吗\"},"
                        "\"unread_count\":3,\"updated_time\":1700000000}]}";
        ThreadList tl;
        memset(&tl, 0, sizeof(tl));
        int r = social_parse_threads(j, strlen(j), &tl);
        ok = r == 0 && tl.n == 1 && strcmp(tl.v[0].peer, "李四") == 0 &&
             strcmp(tl.v[0].snippet, "在吗") == 0 && tl.v[0].unread == 3;
        printf("[dm2  ] %s 会话列表：participant/latest_message 解析\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        thread_list_free(&tl);
    }

    /* 图片：JPEG 解码 + 缓存往返（真机上没法开调试器，只能靠这条） */
    {
        ok = img_selftest() == 1;
        printf("[img  ] %s JPEG 解码四象限灰度正确、PNG 被拒、缓存往返一致\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 图片排版：一张图不允许被分页劈成两半。
     * 排版要用字体量宽，所以这里必须先 font_init（前面的字体自检已经
     * font_done 过了，不重开的话 FT_Load_Char 会直接段错误）。 */
    {
        char ferr[256] = "";
        if (font_init(ferr, sizeof(ferr)) != 0) {
            printf("[imgl ] SKIP 字体不可用（%s）\n", ferr);
        } else {
        Doc d;
        doc_init(&d);
        doc_add(&d, "前文", BLK_PARA);
        doc_add(&d, "/tmp/x.zhg\t400\t300", BLK_IMAGE);
        doc_add(&d, "中间一段文字", BLK_PARA);
        doc_add(&d, "/tmp/x.zhg\t400\t300", BLK_IMAGE);
        Layout L;
        layout_build(&L, &d, 400, 500, 20);
        int split = 0, imgs = 0;
        for (int i = 0; i < L.n; i++) {
            if (L.lines[i].kind != BLK_IMAGE) continue;
            imgs++;
            int a = L.lines[i].y / 500, b = (L.lines[i].y + L.lines[i].px - 1) / 500;
            if (a != b) split = 1;
        }
        ok = (imgs == 2) && !split && L.npages >= 2;
        printf("[imgl ] %s 图片整张占位：2 张图都不跨页（页数=%d）\n",
               ok ? "OK" : "FAIL", L.npages);
        ok ? pass++ : fail++;
        layout_free(&L);
        doc_free(&d);
        font_done();
        }
    }

    /* 发布时间格式化：Unix 秒 -> YYYY-MM-DD；0 必须给空串（接口没给时不能显示 1970） */
    {
        char a[32], b[32], want[32];
        api_format_date(0, a, sizeof(a));
        api_format_date(1700000000L, b, sizeof(b));
        time_t t = (time_t)1700000000L;
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(want, sizeof(want), "%Y-%m-%d", &tmv);
        ok = (a[0] == 0) && strcmp(b, want) == 0;
        printf("[date ] %s 0 -> 空串，1700000000 -> %s\n", ok ? "OK" : "FAIL", b);
        ok ? pass++ : fail++;
    }

    /* 1.2.8：三档图片模式（关 / 点按 / 自动）在排版阶段就分得清——
     * 自动档写 "\x01" 并排队，点按档写 "\x02" 且不排队。写反了界面表现就是
     * "点了没反应"或者"没点就偷偷跑流量"。 */
    {
        ok = ui_img_mode_test() == 1;
        printf("[imod ] %s 三档图片模式：自动档排队(\\x01)、点按档只标记(\\x02)\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 1.2.7：图片先行占位——占位块的格式必须是 "\x01下标\t宽\t高"，
     * 排版要按最后一段的高度给它留位置，否则"图还没到"的文章会全是空洞。 */
    {
        char ferr[256] = "";
        if (font_init(ferr, sizeof(ferr)) != 0) {
            printf("[imgp ] SKIP 字体不可用（%s）\n", ferr);
        } else {
            Doc d;
            doc_init(&d);
            doc_add(&d, "前面一段文字", BLK_PARA);
            doc_add(&d, "\x01" "3\t800\t600", BLK_IMAGE);
            doc_add(&d, "后面一段文字", BLK_PARA);
            Layout L;
            layout_build(&L, &d, 400, 500, 20);
            int found = 0, ok2 = 1;
            for (int i = 0; i < L.n; i++) {
                if (L.lines[i].kind != BLK_IMAGE) continue;
                /* 高 600 超过一页(500)，会被夹到整页高——说明高度确实是从
                 * 第三段读出来的，而不是恒为 0（恒 0 的话整块会被跳过） */
                found = (L.lines[i].px == 500);
                ok2 = (L.lines[i].blk == 1);         /* 块下标正确，重排才能锚定 */
            }
            ok = found && ok2;
            printf("[imgp ] %s 占位块按记录的宽高排版，块下标可锚定\n", ok ? "OK" : "FAIL");
            ok ? pass++ : fail++;
            layout_free(&L);
            doc_free(&d);
            font_done();
        }
    }

    /* 1.2.7：私信会话对象自带 url 时必须抓住——知乎的 /inbox/{id}/messages
     * 路由在真机上 404，顺着列表给的地址取才是唯一可靠的路。 */
    {
        const char *j = "{\"data\":[{\"id\":\"99\",\"url\":\"https://www.zhihu.com/api/v4/inbox/99/messages?limit=20&offset=0\","
                        "\"participant\":{\"name\":\"王五\"},\"snippet\":\"在吗\"}]}";
        ThreadList tl;
        memset(&tl, 0, sizeof(tl));
        int r = social_parse_threads(j, strlen(j), &tl);
        ok = r == 0 && tl.n == 1 && strcmp(tl.v[0].peer, "王五") == 0 &&
             strcmp(tl.v[0].url, "/api/v4/inbox/99/messages?limit=20&offset=0") == 0;
        printf("[dm3  ] %s 会话自带 url 被保留（去掉域名，可直接请求）\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        thread_list_free(&tl);
    }

    /* 1.2.7：收藏夹列表的真实结构（members/self/favlists）：
     * 每一项是 {"type":"collection","id":..,"title":..,"item_count":..} */
    {
        const char *j = "{\"paging\":{\"is_end\":true},\"data\":["
                        "{\"type\":\"collection\",\"id\":74499010,\"title\":\"我的收藏\","
                        "\"item_count\":12,\"is_public\":true}]}";
        ApiList l;
        memset(&l, 0, sizeof(l));
        int r = api_parse_feed(j, strlen(j), &l);
        ok = r == 0 && l.n == 1 && strcmp(l.v[0].id, "74499010") == 0 &&
             strcmp(l.v[0].title, "我的收藏") == 0 && l.v[0].answers == 12 &&
             strstr(l.v[0].sub, "条内容") != NULL;
        printf("[fav2 ] %s 收藏夹列表：id/title/item_count -> 「12 条内容」\n", ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        api_list_free(&l);
    }

    /* 1.2.7：paging.next 必须被记住——推荐流按 page_number 翻页会重复，
     * 只能照服务端给的续页地址走。 */
    {
        const char *j = "{\"paging\":{\"is_end\":false,\"next\":"
                        "\"https://www.zhihu.com/api/v3/feed/topstory/recommend?page_number=2&limit=6\"},"
                        "\"data\":[{\"target\":{\"type\":\"answer\",\"id\":123,\"excerpt\":\"x\"}}]}";
        ApiList l;
        memset(&l, 0, sizeof(l));
        api_parse_feed(j, strlen(j), &l);
        const char *nx = api_last_next();
        ok = nx && strstr(nx, "page_number=2") != NULL;
        printf("[next ] %s 推荐流的 paging.next 被保留（续页不再靠猜 page_number）\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
        api_list_free(&l);
    }

    /* 1.2.9：私信端点候选必须以"实测存在"的为准。
     * 2026-11 游客态实测：/api/v4/inbox/{id}/messages 是 nginx 级 404（路由
     * 不存在），真机上"4 个端点状态 404"就是拿它当首选造成的；
     * /api/v4/messages?thread_id= 与 /api/v4/me/message-threads 都返回 200。 */
    {
        extern const char *social_msg_candidate(int i);
        extern int social_msg_candidate_count(void);
        extern const char *social_thread_candidate(int i);
        int n = social_msg_candidate_count();
        int okc = n > 0 && strstr(social_msg_candidate(0), "thread_id=") != NULL;
        int found_me = 0;
        for (int i = 0; i < n; i++)
            if (strstr(social_msg_candidate(i), "/api/v4/messages?") != NULL) found_me = 1;
        int nthreads = 0;
        while (social_thread_candidate(nthreads)) nthreads++;
        int found_threads = 0;
        for (int i = 0; i < nthreads; i++)
            if (strstr(social_thread_candidate(i), "me/message-threads") != NULL)
                found_threads = 1;
        ok = okc && found_me && found_threads;
        printf("[dme  ] %s 私信端点首选 messages?thread_id=（实测 200），404 路由已降级\n",
               ok ? "OK" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 1.2.9：排版首页高度——标题只占第一页，后续页整页都是正文。 */
    {
        char ferr[256] = "";
        if (font_init(ferr, sizeof(ferr)) != 0) {
            printf("[lay2 ] SKIP 字体不可用（%s）\n", ferr);
        } else {
            Doc d;
            doc_init(&d);
            char para[512];
            for (int i = 0; i < 12; i++) {
                snprintf(para, sizeof(para), "第%d段，这一段有比较多的汉字内容，"
                                             "用来把排版撑过好几页。", i);
                doc_add(&d, para, BLK_PARA);
            }
            Layout L;
            int h0 = 500, h1 = 800;    /* 第一页矮 300（标题页眉的高度差） */
            layout_build2(&L, &d, 400, h1, 20, h0);
            int okp = L.npages >= 3;
            /* 第一页的所有行 y < h0；第二页的行 y ∈ [h0, h0+h1) */
            int p0 = 1, p1 = 1;
            for (int i = 0; i < L.n; i++) {
                int pg = -1;
                for (int q = 0; q < L.npages; q++)
                    if (i >= L.pages[q] && i < L.pages[q + 1]) { pg = q; break; }
                if (pg == 0 && L.lines[i].y >= h0) p0 = 0;
                if (pg == 1 && (L.lines[i].y < h0 || L.lines[i].y >= h0 + h1)) p1 = 0;
            }
            ok = okp && p0 && p1;
            printf("[lay2 ] %s 首页高 %d、后续页高 %d，页数=%d，各页行不越界\n",
                   ok ? "OK" : "FAIL", h0, h1, L.npages);
            ok ? pass++ : fail++;
            layout_free(&L);
            doc_free(&d);
            font_done();
        }
    }

    printf("结果：%d 通过，%d 失败\n", pass, fail);
    return fail ? 1 : 0;
}

/* --apitest：接口体检。
 *
 * 为什么要单独做一个：1.2.4 报"热榜/推荐能看标题、点进去全 404"，
 * 而 404 既可能是"资源不存在"，也可能是"我们的请求不对"。人在设备上
 * 没法开 DevTools，所以让程序自己把「真实 URL + 状态码 + 响应体前 200 字节」
 * 打出来。
 *
 * 输出分两路：
 *   · stdout —— 纯 ASCII，KUAL 会用 eips 打到屏幕上（eips 没有中文字体）；
 *   · log.txt —— 中文详情，包括每条的原始链接，方便回传。 */
static int run_apitest(void) {
    printf("zhihu %s api test\n", ZH_VERSION);
    api_set_home(zh_home());
    if (api_warmup() != 0) {
        printf("warmup: FAIL\n");
        logmsg("apitest: warmup 失败 %s", api_error());
    } else {
        printf("warmup: ok\n");
    }

    ApiList l;
    memset(&l, 0, sizeof(l));
    int rc = api_hot(&l, 10);
    printf("hot: rc=%d n=%d status=%d\n", rc, l.n, api_last_status());
    logmsg("apitest: 热榜 rc=%d n=%d status=%d url=%s err=%s body=%s",
           rc, l.n, api_last_status(), api_last_url(), api_error(), api_last_head());
    for (int i = 0; i < l.n && i < 3; i++)
        logmsg("apitest: 热榜[%d] type=%d id=[%s] qid=[%s] url=%s 标题=%s",
               i, l.v[i].type, l.v[i].id, l.v[i].qid,
               l.v[i].url[0] ? l.v[i].url : "(无)", l.v[i].title);

    if (l.n > 0) {
        ApiItem *it = &l.v[0];
        printf("item0: type=%d id=%s qid=%s\n", it->type,
               it->id[0] ? it->id : "-", it->qid[0] ? it->qid : "-");
        ApiContent c;
        memset(&c, 0, sizeof(c));
        int rc2;
        if (it->type == 3 && it->qid[0]) {
            ApiList q;
            memset(&q, 0, sizeof(q));
            rc2 = api_question_answers(it->qid, &q, 0, 10);
            printf("detail(question): rc=%d n=%d status=%d\n",
                   rc2, q.n, api_last_status());
            logmsg("apitest: 问题 %s rc=%d n=%d status=%d url=%s err=%s body=%s",
                   it->qid, rc2, q.n, api_last_status(), api_last_url(),
                   api_error(), api_last_head());
            api_list_free(&q);
        } else {
            rc2 = (it->type == 2) ? api_article_detail2(it->id, &c)
                                  : api_answer_detail2(it->id, it->qid, &c);
            printf("detail: rc=%d status=%d len=%d\n", rc2, api_last_status(),
                   c.html ? (int)strlen(c.html) : 0);
            printf("head: %.60s\n", api_last_head());
            logmsg("apitest: 详情 id=%s type=%d rc=%d status=%d url=%s err=%s body=%s",
                   it->id, it->type, rc2, api_last_status(), api_last_url(),
                   api_error(), api_last_head());
            api_content_free(&c);
        }
    }
    api_list_free(&l);

    /* 推荐流也测一次：它要求的登录态比热榜严格 */
    memset(&l, 0, sizeof(l));
    rc = api_recommend(&l, 0, 6);
    printf("recommend: rc=%d n=%d status=%d\n", rc, l.n, api_last_status());
    logmsg("apitest: 推荐 rc=%d n=%d status=%d url=%s err=%s body=%s",
           rc, l.n, api_last_status(), api_last_url(), api_error(), api_last_head());
    if (l.n > 0) {
        ApiItem *it = &l.v[0];
        logmsg("apitest: 推荐[0] type=%d id=[%s] qid=[%s] url=%s 标题=%s",
               it->type, it->id, it->qid, it->url[0] ? it->url : "(无)", it->title);
        ApiContent c;
        memset(&c, 0, sizeof(c));
        int rc3 = (it->type == 2) ? api_article_detail2(it->id, &c)
                                  : api_answer_detail2(it->id, it->qid, &c);
        printf("recommend-detail: rc=%d status=%d\n", rc3, api_last_status());
        logmsg("apitest: 推荐详情 id=%s rc=%d status=%d url=%s err=%s body=%s",
               it->id, rc3, api_last_status(), api_last_url(), api_error(), api_last_head());
        api_content_free(&c);
    }
    api_list_free(&l);

    printf("done - see log.txt\n");
    return 0;
}

/* --dmprobe：私信端点诊断。
 * 真机上没法开 DevTools，"私信打不开"到底是端点没了、没登录还是被风控，
 * 只能靠把每个候选端点的 HTTP 状态打出来。输出保持纯 ASCII，
 * 屏幕上的 eips 只认西文字体。 */
static int run_dmprobe(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/dmprobe.txt", zh_home());
    social_probe_to(path);
    printf("zhihu-reader dm probe:\n");
    printf("results (status, bytes):\n");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        int n = 0;
        while (fgets(line, sizeof(line), f) && n < 14) {
            fputc(' ', stdout);
            fputs(line, stdout);
            n++;
        }
        fclose(f);
    } else {
        printf(" probe file not written\n");
    }
    printf("full details in log.txt\n");
    return 0;
}

/* --diag：把环境信息打出来（屏幕、字体、输入设备），出问题时看这个最直接 */
static int run_diag(void) {
    char err[256];
    Fb fb;
    printf("知乎阅读 %s 诊断\n", ZH_VERSION);
    printf("home=%s\n", zh_home());

    if (fb_open(&fb, err, sizeof(err)) != 0) {
        printf("屏幕: 失败 %s\n", err);
    } else {
        printf("屏幕: %dx%d %dbpp line=%d smem=%zu rotate=%d\n",
               fb.width, fb.height, fb.bpp, fb.line_len, fb.memlen, fb.rotate);
        /* 下面这几行会被 eips 打到屏幕上，eips 只有西文字体，
         * 所以结论用 ASCII 写，中文只留在文件里 */
        printf("refresh: eink=%d ioc=%08x size=%d dir=%d unk=%d\n",
               fb.eink_kind, fb.send_update_ioctl, fb.update_size, fb.update_dir,
               fb.unknown_errno);
        printf("virtual: yres_virtual=%d base_off=%u aligned=%s\n",
               fb.yres_virtual, (unsigned)fb.base_off, fb.base_off ? "yes" : "no");
        printf("eips: path=%s mode=%d blankpulse=%d\n",
               fb.eips_path[0] ? fb.eips_path : "(not found)", fb.eips_mode, fb.blankpulse);
        printf("path: %s\n",
               fb.eink_kind == 1 ? "MXCFB_SEND_UPDATE (OK)" :
               fb.eink_kind == 2 ? "FBIOPAN_DISPLAY (legacy; may reboot on EPDC)" :
               fb.eink_kind == 3 ? "memory-only (screen may not update)" :
               fb.eink_kind == 4 ? "eips helper (fallback, works on KO3)" : "unknown");
        fb_close(&fb);
    }

    if (font_init(err, sizeof(err)) != 0) {
        printf("字体: 失败 %s\n", err);
    } else {
        printf("字体: %s（%d 个）\n", font_info(), font_nfaces());
        font_done();
    }

    input_set_transform(0, 0, 0);
    printf("input-rotate: %s (4 directions)\n",
           input_rotate_selftest() ? "ok" : "FAIL");
    printf("输入: %s\n", input_probe_text());
    /* 触摸独占状态是"点不动"的第一嫌疑：被 cvm 占着就一定收不到点击 */
    printf("input-grab: %s\n",
           input_grab_status() == 1 ? "ok (exclusive)" :
           input_grab_status() == -1 ? "busy (held by framework/cvm)" : "no touch device");
    input_close();
    printf("结束\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *home = getenv("ZHIHU_HOME");
    if (!home || !*home) {
        if (access("/mnt/us", W_OK) == 0) home = "/mnt/us/zhihu";
        else home = "/tmp/zhihu";
    }
    mkdir_p(home);
    char sub[320];
    snprintf(sub, sizeof(sub), "%s/cache", home);
    mkdir_p(sub);
    zh_set_home(home);
    api_set_home(home);
    install_crash_handlers();

    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("知乎阅读 %s\n", ZH_VERSION);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--crash") == 0) {
        /* 只用来验证崩溃记录链路，正常用户不会用到 */
        printf("触发一次空指针写入…\n");
        logmsg("crash test");
        *(volatile int *)0 = 1;
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--preview") == 0) {
        int pages = argc > 3 ? atoi(argv[3]) : 3;
        return app_preview(argv[2], pages);
    }
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        logmsg("selftest start");
        return run_selftest();
    }
    if (argc > 1 && strcmp(argv[1], "--diag") == 0) {
        return run_diag();
    }
    if (argc > 1 && strcmp(argv[1], "--apitest") == 0) {
        return run_apitest();
    }
    if (argc > 1 && strcmp(argv[1], "--dmprobe") == 0) {
        return run_dmprobe();
    }

    if (argc > 1 && strcmp(argv[1], "--grabtest") == 0) {
        /* 启动器用它在"动 framework 之前"做决定：
         *   grab=ok   —— 触摸屏没人独占，可以和 framework 共存（最安全，不重启）
         *   grab=busy —— 被 cvm 独占着。独占是排他的，不接管系统界面就永远
         *                收不到点击（1.2.3 的"卡在主页没法操作"就是这么来的）
         *   grab=none —— 没有触摸设备（K4/K5/模拟器），不用接管
         * 只做探测：拿到独占后立刻放开，不影响随后启动的真程序。 */
        int r = input_open(0, 0);
        int st = input_grab_status();
        printf("grab=%s\n", st == 1 ? "ok" : (st == -1 ? "busy" : "none"));
        if (r != 0) printf("input=missing\n");
        input_close();
        return st == 1 ? 0 : (st == -1 ? 2 : 3);
    }

    /* 真正跑界面时才接管会话与退出还原。
     * --grabtest / --diag / --selftest 是启动器在"决定要不要动 framework"
     * 之前调的探测命令，绝不能顺带去碰 cvm 或 framework。 */
    /* 自成会话/进程组。
     * Kindle 的 framework 出问题时会被 /etc/init.d 按进程组发 SIGTERM，
     * 我们如果还挂在 cvm（或 KUAL）那个组里就会一起被砍——1.2.3 日志里
     * `收到信号 15` + `rc=143` 就是这么来的，而内核日志里一条都没有。
     * setsid 之后这类"连带伤害"打不到我们。 */
    if (setsid() < 0) logmsg("启动: setsid 失败（可能已是进程组长），继续");
    atexit(restore_atexit);   /* 任何退出路径都还原系统界面 */

    logmsg("=== 知乎阅读 %s 启动 home=%s uid=%d ===", ZH_VERSION, home, (int)getuid());
    int rc = app_run();
    logmsg("=== 正常退出 rc=%d ===", rc);
    return rc;
}
