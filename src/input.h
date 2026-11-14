/* input.h —— 读 Kindle 的触摸屏与实体按键（/dev/input/event*） */
#ifndef ZH_INPUT_H
#define ZH_INPUT_H

typedef enum {
    ZH_NONE = 0,
    ZH_TAP,      /* 单击：x,y 为画布坐标 */
    ZH_SWIPE_L,
    ZH_SWIPE_R,
    ZH_KEY,      /* code 为 KEY_* */
} EvType;

typedef struct {
    EvType type;
    int x, y;
    int code;
    long ms;
} Ev;

/* phys_w / phys_h 是 framebuffer 的物理宽高（不是旋转后的画布）。 */
int  input_open(int phys_w, int phys_h);
void input_close(void);
/* 等待事件，timeout_ms 内无事件返回 0（ZH_NONE） */
int  input_poll(Ev *out, int timeout_ms);
/* 兜底校准：换轴 / 翻转（正常情况下不需要，rotate 已经自动处理） */
void input_set_transform(int swap, int invert_x, int invert_y);
/* 把界面旋转同步到触摸映射。**改旋转时必须调用**，否则显示转了触摸不转，
 * 表现就是"有一面是正的，其余三个方向全反"（1.2.4 的 bug）。 */
void input_set_rotate(int rotate, int phys_w, int phys_h);
int  input_rotate(void);
/* 纯映射函数：原始触摸坐标 -> 逻辑坐标，自检与真机共用 */
void input_map_pure(int raw_x, int raw_y,
                    int raw_minx, int raw_maxx, int raw_miny, int raw_maxy,
                    int phys_w, int phys_h, int rot,
                    int swap_hint, int invert_x, int invert_y,
                    int *out_x, int *out_y);
/* 自检：四个旋转方向下"物理坐标 -> 逻辑坐标"都要对得上 */
int  input_rotate_selftest(void);
void input_get_raw_range(int *minx, int *maxx, int *miny, int *maxy);
/* 诊断用：枚举一遍设备并返回一句话描述 */
const char *input_probe_text(void);

/* EVIOCGRAB（触摸屏独占）的结果：1=拿到了 0=没有触摸设备 -1=被别人占着。
 * 这个值决定启动器要不要接管系统界面——拿不到独占就一定收不到点击。 */
int  input_grab_status(void);
/* 累计收到的事件条数 / 判定出的点击数，用来区分"没点"和"点了没反应" */
long input_event_count(void);
long input_action_count(void);
/* 补抢独占（触摸屏 + 翻页键设备）：返回这轮抢到几个 */
int  input_retry_grab(void);

/* 按键设备是否该独占：含翻页键且不含电源键才抢。抽成纯函数给自检用。
 * 翻页键 = KEY_PREVIOUS(105)/KEY_NEXT(106)/190/191/192。 */
int  input_key_grab_rule(int has_page_keys, int has_power_key);

#endif
