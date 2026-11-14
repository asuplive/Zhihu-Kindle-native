/* test_input.c —— 触摸状态机的离线测试（不需要真机、不需要 uinput）
 *
 *   gcc -Isrc -o /tmp/test_input tools/test_input.c src/common.c && /tmp/test_input
 *
 * 覆盖两代 Kindle 的报点方式：
 *   A 类：ABS_X/Y + BTN_TOUCH
 *   B 类：MT 协议 B（TRACKING_ID + POSITION），很多固件不发 BTN_TOUCH
 * 以及"摸一下屏幕会冒出伪按键"这个曾经导致点不动的 bug。
 */
#include "../src/input.c"

#include <stdio.h>

static int fails;

static void feed(Dev *d, int type, int code, int value, Ev *out, int *got) {
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (handle_event(d, &e, out)) {
        *got = 1;
    }
}

static void use_dev(Dev *d) {
    memset(g_devs, 0, sizeof(g_devs));
    g_devs[0] = *d;
    g_ndevs = 1;
}

static void reset_state(void) {
    g_raw_x = g_raw_y = 0;
    g_down = 0;
    g_moved = 0;
    g_tid = -1;
    g_pending = 0;
    g_have_xy = 0;
}

static void check(const char *name, int ok) {
    printf("%-46s %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    g_w = 1264;
    g_h = 1680;
    g_ndevs = 0;
    g_swap = g_ix = g_iy = 0;

    Dev mt;
    memset(&mt, 0, sizeof(mt));
    mt.has_abs = 1;
    mt.has_mt = 1;
    mt.minx = 0; mt.maxx = 1263;
    mt.miny = 0; mt.maxy = 1679;

    Dev old;
    memset(&old, 0, sizeof(old));
    old.has_abs = 1;
    old.has_mt = 0;
    old.minx = 0; old.maxx = 1263;
    old.miny = 0; old.maxy = 1679;

    Ev ev;
    int got;

    /* ---------- B 类：MT 协议 B，完全没有 BTN_TOUCH ---------- */
    {
        reset_state();
        use_dev(&mt);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 7, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_X, 300, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_Y, 400, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("MT-B 按下不产生事件", got == 0);
        feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, -1, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("MT-B 抬起产生点击", got == 1 && ev.type == ZH_TAP);
        check("MT-B 点击坐标正确", ev.x == 300 && ev.y == 400);
    }

    /* ---------- B 类：横向滑动 ---------- */
    {
        reset_state();
        use_dev(&mt);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 8, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_X, 900, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_Y, 800, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_X, 700, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_POSITION_X, 400, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, -1, &ev, &got);
        feed(&mt, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("MT-B 向左滑动识别", got == 1 && ev.type == ZH_SWIPE_L);
    }

    /* ---------- A 类：ABS + BTN_TOUCH ---------- */
    {
        reset_state();
        use_dev(&old);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&old, EV_ABS, ABS_X, 200, &ev, &got);
        feed(&old, EV_ABS, ABS_Y, 900, &ev, &got);
        feed(&old, EV_KEY, BTN_TOUCH, 1, &ev, &got);
        feed(&old, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("BTN_TOUCH 按下不产生事件", got == 0);
        feed(&old, EV_KEY, BTN_TOUCH, 0, &ev, &got);
        feed(&old, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("BTN_TOUCH 抬起产生点击", got == 1 && ev.type == ZH_TAP);
        check("BTN_TOUCH 点击坐标正确", ev.x == 200 && ev.y == 900);
    }

    /* ---------- 伪按键：摸屏幕时会先发 BTN_TOOL_FINGER ---------- */
    {
        reset_state();
        use_dev(&mt);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&mt, EV_KEY, BTN_TOOL_FINGER, 1, &ev, &got);
        check("BTN_TOOL_FINGER 不当成按键", got == 0);
        got = 0;
        feed(&mt, EV_KEY, BTN_TOOL_DOUBLETAP, 1, &ev, &got);
        check("BTN_TOOL_DOUBLETAP 不当成按键", got == 0);
        got = 0;
        feed(&mt, EV_KEY, KEY_POWER, 1, &ev, &got);
        check("电源键不当成按键", got == 0);
    }

    /* ---------- 实体按键 ---------- */
    {
        reset_state();
        use_dev(&mt);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&mt, EV_KEY, KEY_PAGEUP, 1, &ev, &got);
        check("翻页键识别为按键", got == 1 && ev.type == ZH_KEY && ev.code == KEY_PAGEUP);
        got = 0;
        feed(&mt, EV_KEY, KEY_PAGEUP, 2, &ev, &got);
        check("长按重复(值2)不重复触发", got == 0);
    }

    /* ---------- 坐标范围与画布不一致时缩放 ---------- */
    {
        reset_state();
        Dev small;
        memset(&small, 0, sizeof(small));
        small.has_abs = 1;
        small.has_mt = 1;
        small.minx = 0; small.maxx = 631;    /* 输入是屏幕的一半 */
        small.miny = 0; small.maxy = 839;
        use_dev(&small);
        memset(&ev, 0, sizeof(ev));
        got = 0;
        feed(&small, EV_ABS, ABS_MT_TRACKING_ID, 1, &ev, &got);
        feed(&small, EV_ABS, ABS_MT_POSITION_X, 631, &ev, &got);
        feed(&small, EV_ABS, ABS_MT_POSITION_Y, 839, &ev, &got);
        feed(&small, EV_SYN, SYN_REPORT, 0, &ev, &got);
        feed(&small, EV_ABS, ABS_MT_TRACKING_ID, -1, &ev, &got);
        feed(&small, EV_SYN, SYN_REPORT, 0, &ev, &got);
        check("坐标缩放到底右角", ev.x > 1200 && ev.y > 1650);
    }

    printf("\n结果：%s\n", fails ? "有失败" : "全部通过");
    return fails ? 1 : 0;
}
