/* statusbar.h —— 系统状态栏（chrome pillow）的隐藏与恢复
 *
 * 顶栏的时间/网络/电量是 framework 的 awesome 窗口管理器画的 chrome pillow。
 * 1.2.9 曾用「杀 cvm」隐藏它——cvm 一死 framework 看门狗就在宽限期后整机
 * 重启（真机实测两次），1.2.10 起与 framework 彻底解耦，只靠屏幕守护压。
 *
 * 1.2.14：压屏只是"事后回灌"，时间还是能冒出来晃半秒。这里换成 KOReader
 * 在真机上验证多年的正面方案（koreader.sh 同款）：
 *   1) lipc-set-prop com.lab126.pillow disableEnablePillow disable
 *      —— 告诉 framework 别再画 pillow（FW>=5.6.5 支持）；
 *   2) kill -STOP awesome
 *      —— FW>=5.7.2 光禁 pillow 时钟仍会刷新，把 WM 整个 SIGSTOP 才彻底安静。
 * 全程不碰 cvm/framework 进程本身，无看门狗风险；两条都可逆：
 * 退出时 SIGCONT awesome + disableEnablePillow enable。
 *
 * 每次隐藏都会把 awesome 的 pid 写进 <home>/.awesome_stopped 标记，
 * 正常退出、崩溃处理器、start.sh 兜底三条恢复路都认它。
 */
#ifndef ZH_STATUSBAR_H
#define ZH_STATUSBAR_H

/* 隐藏状态栏：禁 pillow + 暂停 awesome。幂等，可在唤醒后重复调用
 * （awesome 若已被系统 SIGCONT 会重新压住）。 */
void statusbar_hide(void);

/* 完整恢复：SIGCONT awesome + pillow enable + 清标记（会 fork lipc）。 */
void statusbar_restore(void);

/* 只做信号安全的部分：读标记 -> SIGCONT -> unlink，不 fork。
 * 供崩溃处理器 / SIGALRM 兜底路径用。 */
void statusbar_restore_safe(void);

#endif
