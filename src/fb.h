/* fb.h —— Kindle 帧缓冲直接绘制
 *
 * 不依赖 FBInk / KOReader / SDL，直接 mmap /dev/fb0 并调用 i.MX EPDC 的
 * MXCFB_SEND_UPDATE 触发电子墨水刷新。这是"原生"的关键：像素往哪儿写、
 * 什么时候刷屏，全部由本程序决定。
 */
#ifndef ZH_FB_H
#define ZH_FB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      fd;
    uint8_t *mem;
    size_t   memlen;

    int width, height;       /* 逻辑画布尺寸（= fbset 的 xres/yres） */
    int bpp;
    int line_len;
    int rotate;              /* var.rotate，仅作诊断 */
    int ui_rotate;           /* 用户选择的界面旋转 0..3 */

    int eink_kind;           /* 0=未知 1=mxcfb 2=普通 fb 3=只写内存不刷
                              * 4=交给系统 eips 刷（KO3 上的实际后路） 9=模拟器 */
    uint32_t marker;
    int fast_mode;           /* 0=整屏闪（GC16/FULL） 1=不闪（局部 GC16）
                              * 2=Regal（REAGL 波形，1.2.9 实验）
                              * 3=局部 N 次 + 定期一次全刷（1.2.15，N=full_every） */
    int flip_count;
    /* 1.2.15：第 4 档「局部+定期全刷」的周期（翻页次数）。config.txt
     * 写 fullevery=N 可调，默认 6。只在 fast_mode==3 时生效。 */
    int full_every;

    /* 1.2.9：硬件旋转跟踪。framework（cvm）在设备翻转时会改 var.rotate，
     * 我们的像素会被显示层转过去（"界面旋转了"），但触摸坐标没跟
     * （"触摸没旋转"）。ui.c 定期读一次 var.rotate，发现变化就把
     * ui_rotate 和触摸映射一起补偿。autorot=0（config.txt）可关闭。 */
    int hw_rotate;
    int autorot;

    /* 机型兼容：不同内核的 mxcfb_update_data 大小不同，ioctl 号也不同。
     * 开机时逐个试，记下真正能用的那个（0 表示都不支持）。 */
    unsigned int send_update_ioctl;
    int          update_size;
    int          update_dir;      /* 1=_IOW  3=_IOWR，KO3 的 EPDC 只认 _IOWR */
    int          update_temp;     /* TEMP_USE_AMBIENT(0x1000) 或 0，探测决定 */
    int          unknown_errno;   /* "命令不存在"的基线 errno，用来解释探测结果 */

    /* eips 后路：KO3 上 SEND_UPDATE 全部 EINVAL，但系统自带的 eips 能刷屏，
     * 所以探测失败时把整页像素交给它（eips -g 图片 / eips -f 全刷）。 */
    char eips_path[80];
    int  eips_mode;          /* 0=auto(先图片后 -f) 1=图片 2=-f 3=关闭 */
    int  eips_ok;
    int  eips_fail;

    /* 双缓冲虚拟屏（KO3 的 yres_virtual = 2*yres）。绘制基址要跟当前
     * 可见页对齐，否则画进了看不见的那一屏，屏幕始终不动。 */
    size_t base_off;
    int    yres_virtual;

    /* 刷新路径开关，可由 config.txt 的 refresh= 覆盖：
     * 0=auto（探测成功用 mxcfb，失败时交给 eips，再不行才只写内存）
     * 1=mxcfb  2=pan（仅老机型）  3=none（只写内存，绝不碰 ioctl）
     * 4=eips   5=blank（只写内存 + FBIOBLANK 脉冲，靠 unblank 逼它重画）
     * 之所以要这个开关：KO3 上 FBIOPAN_DISPLAY 会触发内核 panic 整机重启，
     * 一旦探测出了偏差，必须有个不改代码就能退出来的后路。 */
    int refresh_mode;
    int blankpulse;          /* kind=3 时是否用 FBIOBLANK 脉冲硬刷一屏 */

    /* 1.2.8：残影/发虚的两个来源，各给一个开关。
     *   use_du     —— 翻页用 DU（A2）波形。DU 只翻像素不擦底，连续几屏之后
     *                 上一屏的字会留在纸上（真机照片里的"文字重叠"），
     *                 灰阶抗锯齿也被量化掉，看起来就是"字发虚"。
     *                 默认关闭：改用 GC16 + PARTIAL，不闪屏但画面是干净的。
     *   eips_clean —— eips 后路用 `eips -g` 推图之后，再补一次 `eips -f`
     *                 强制 GC16 全刷。KO3 上 `-g` 是局部更新，换屏时不清底，
     *                 所以换一次界面就叠一层。默认开启，只在"整屏换内容"
     *                 时补，翻页不补（翻页本来就整屏变，残影看不出来）。 */
    int use_du;
    int eips_clean;
    int eips_clean_fail;

    /* 1.2.9：Regal（REAGL）模式开关，config.txt 写 regal=1 可强制。
     * Kindle 的 REAGL = WAVEFORM_MODE_REAGL(0x8)："残影补偿波形"，
     * 翻页不闪屏、且驱动会主动补偿残影。设置里选了 Regal 或配置了 regal=1
     * 时翻页走它；驱动不认就自动退回 GC16 并写日志。 */
    int regal;

    int trace_n;             /* 前 N 次 present 把标记刷进磁盘，用来定位重启点 */

    /* 可见页对齐检查的剩余次数：驱动如果每次 SEND_UPDATE 都自己翻页，
     * 这个检查会一直判"被翻走了"并每秒重画——3 次之后自动停掉。 */
    int guard_align;

    /* 我们自己那页的**影子副本**（2.1MB @8bpp）。
     * 与系统界面共存时，cvm 会往同一块 framebuffer 上画顶栏时钟之类，
     * 把我们的画面盖掉一部分。1.2.4 的做法是"发现被改就重新排版重画"，
     * 一次要几十毫秒、而且中间还会再空一帧；有了影子副本就可以
     * 直接把整页 memcpy 回去（约 2ms）+ 一次全刷，桌面残留几乎看不见。 */
    uint8_t *shadow;
    size_t   shadow_len;
} Fb;

/* 返回 0 成功；失败时 err 里是原因 */
int  fb_open(Fb *fb, char *err, int errsz);
void fb_close(Fb *fb);
void fb_set_rotate(Fb *fb, int r);

/* 画布尺寸（已考虑 ui_rotate） */
int fb_w(Fb *fb);
int fb_h(Fb *fb);

/* 基础绘制（坐标是"屏上看到的"坐标） */
void fb_clear(Fb *fb, int gray);
void fb_fill(Fb *fb, int x, int y, int w, int h, int gray);
void fb_hline(Fb *fb, int x, int y, int w, int gray);
void fb_vline(Fb *fb, int x, int y, int h, int gray);
void fb_rect(Fb *fb, int x, int y, int w, int h, int gray);
void fb_invert(Fb *fb, int x, int y, int w, int h);
/* 8bit alpha 位图混合（FreeType 输出）。alpha=255 处变 gray */
void fb_blit_gray(Fb *fb, int x, int y, int w, int h, const uint8_t *alpha, int gray);
/* 8bit 灰度位图按最近邻缩放贴到 (x,y,w,h)。用于正文图片。 */
void fb_blit_gray_scaled(Fb *fb, int x, int y, int w, int h,
                         const uint8_t *src, int sw, int sh);

/* 提交到屏幕。full=1 用 GC16 全刷（清残影），0 用快速波形 */
void fb_present(Fb *fb, int full);

/* ---- 屏幕归属守护 ----
 * 1.2.4 起默认不再停掉 framework，cvm 一直活着。它可能在自己有事的时候
 * 往同一块 framebuffer 上重画（顶栏时钟、状态变化），把我们的页面整个盖掉；
 * 双缓冲机型还可能把可见页翻到另一半去，看起来就是"界面莫名其妙跳回主页"。
 * 这两个接口让主循环能低成本发现这种情况并自己重画。
 * fb_sample_sig: 对我们绘制的那一页做稀疏采样哈希（约 2 万次读，微秒级）。
 * fb_guard: 0=没变化 1=像素被别的进程改了 2=可见页被翻走了（需重新对齐） */
uint32_t fb_sample_sig(Fb *fb);
int      fb_guard(Fb *fb, uint32_t *last_sig);

/* 影子副本：分配一块和可见页同样大的内存，每次 fb_present 后同步。
 * fb_guard 一旦发现被改，用 fb_restore 整页回灌，比重排版快一个数量级。
 * 返回 0 成功；分配失败返回 -1（此时退回"重画"路径，功能不受影响）。 */
int  fb_shadow_enable(Fb *fb);
/* 把影子副本整页写回可见页并全刷一次。返回 0 成功。-1 表示没有影子副本。 */
int  fb_restore(Fb *fb);

/* 设备从挂起回来后调一次：EPDC 会丢状态，不 unblank 的话屏幕是黑的。
 * ui.c 的心跳检测到 /proc/uptime 比单调时钟多跑了一截就调它。 */
void fb_after_resume(Fb *fb);

/* 1.2.9：读一次 var.rotate，检测系统是否旋转了屏幕（KO3 设备翻转时
 * framework 会转显示方向）。返回 1=旋转变了（fb->hw_rotate 已更新，
 * 调用方要补偿 ui_rotate 与触摸映射并重画）；0=没变；-1=读不到。
 * config.txt 写 autorot=0 可整体关闭（fb->autorot=0 时恒返回 0）。 */
int fb_poll_rotate(Fb *fb);

/* 自检用：算出指定结构体大小下的 MXCFB_SEND_UPDATE ioctl 号。
 * 1.2.1 曾把 _IOWR 写成 _IOW，方向位差一位导致 KO3 上必然 ENOTTY，
 * 于是退到 FBIOPAN_DISPLAY 把整机刷重启。用它把编码钉死。 */
unsigned int fb_send_update_cmd(int size);

#endif
