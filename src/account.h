/* account.h —— 设备端登录：二维码扫码 + Cookie 导入
 *
 * 为什么没有手机号登录：知乎 sign_in 表单由前端 encrypt.js 加密，且会触发
 * 网易易盾滑块（无 WebView/JS 引擎的帧缓冲程序无法完成）。二维码登录是
 * 网页端唯一不依赖 JS 加密的登录链路，实测可用：
 *   GET /signin → POST /udid → POST qrcode → 轮询 scan_info → 拿到 z_c0
 * Cookie 导入作为 100% 可靠兜底（扫码被风控时仍可登录）。
 */
#ifndef ZH_ACCOUNT_H
#define ZH_ACCOUNT_H

#include <stddef.h>

typedef struct {
    char link[512];     /* 二维码内容（手机知乎 App 扫码） */
    char token[128];    /* 轮询凭证 */
    char user[128];     /* 登录成功后的用户名 */
    char err[256];      /* 失败原因（含风控提示） */
    int  state;         /* 0 初始 / 1 等待扫码 / 2 已扫待确认 / 3 成功 / -1 失败 */
} AuthQr;

/* cookie 中是否已有登录凭证 z_c0 */
int  account_has_login(void);
/* 用当前 cookie 请求 /api/v4/me 校验登录态并取用户名 */
int  account_me(char *user, size_t usern, char *err, size_t errn);
/* 取二维码：填 q->link / q->token，state=1 */
int  account_qr_begin(AuthQr *q);
/* 轮询一次扫码状态。成功时自动把完整 cookie 写入 cookie.txt 并取用户名。
 * 返回 q->state（3=成功，-1=失败）。 */
int  account_qr_poll(AuthQr *q);
/* 导入浏览器 Cookie 整串，校验后返回 0 成功 */
int  account_import_cookie(const char *raw, char *user, size_t usern,
                           char *err, size_t errn);
/* 退出登录：清掉登录凭证但保留 d_c0 设备标识 */
void account_logout(void);
/* 当前完整 cookie（子进程登录成功后回写给父进程用） */
const char *account_cookie(void);

/* ---- fork 子进程之间传递登录会话 ----
 * 二维码登录是「取码 → 多次轮询」的长链路，每次轮询都要带上前一步累积的
 * _xsrf/_zap/d_c0。UI 把联网放进 fork 子进程保持界面可响应，而 fork 后
 * 父子内存独立，因此必须把这份会话 cookie 随结果文件一起落盘再读回。 */
const char *account_session_get(void);
void        account_session_set(const char *cookie);

#endif
