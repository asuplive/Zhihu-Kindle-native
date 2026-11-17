/* ui.h —— 应用入口 */
#ifndef ZH_UI_H
#define ZH_UI_H

/* 全局版本号：main.c 与界面共用，改一处即可 */
#define ZH_VERSION "1.3.3"

int app_run(void);
int app_preview(const char *html_path, int max_pages);

/* 自检用：验证 config.txt 里手写的 refresh= 不会被「保存设置」整份重写冲掉。
 * 用户为了绕开 KO3 重启会在 config.txt 手加 refresh=none，
 * 如果改一次字号就把它冲掉，下次启动又会掉回会重启的路径。 */
int ui_config_roundtrip_test(void);
int ui_nav_stack_test(void);
int ui_refresh_route_test(void);   /* 1.3.3 定稿第1条：刷屏两分法路由自检 */

/* 自检用：三档图片模式（关 / 点按 / 自动）的占位前缀与排队规则。 */
int ui_img_mode_test(void);

#endif
