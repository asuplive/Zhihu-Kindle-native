/* font.h —— FreeType 文字渲染（含中文字体自动发现与回退） */
#ifndef ZH_FONT_H
#define ZH_FONT_H

#include "fb.h"

#include <stdint.h>

int  font_init(char *err, int errsz);
void font_done(void);
const char *font_info(void);      /* 例："CJK.ttf" */
int  font_nfaces(void);           /* 实际加载了几个字体 */

int  font_width(int px, const char *s, int len);   /* 字节长度版 */
int  font_line_height(int px);
int  font_ascent(int px);
int  font_descent(int px);

void font_draw(Fb *fb, int x, int baseline, int px, const char *s, int len, int gray, int bold);

/* 自检用：把字形缓存压到高负载，确认不再出现"随机缺字"。返回 1 通过 */
int  font_cache_stress(void);

#endif
