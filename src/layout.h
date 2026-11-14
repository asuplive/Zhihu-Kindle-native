/* layout.h —— 中文排版：把段落块排成整页
 *
 * 自己做排版而不是塞进 HTML/EPUB 引擎，原因：
 *   1) 翻页必须是"整页"的，行位置必须稳定，不能被外部引擎的流式布局左右；
 *   2) 排版一次后缓存在内存里，翻页只是重画缓存行，老机型也不卡；
 *   3) 不依赖任何渲染引擎，二进制最小。
 */
#ifndef ZH_LAYOUT_H
#define ZH_LAYOUT_H

#include "content.h"

typedef struct {
    const char *s;   /* 指向 Doc 内文本，不拥有 */
    int len;
    int x;
    int y;           /* 相对正文顶端 */
    int px;
    int bold;
    int baseline;    /* 相对行顶的基线偏移 */
    int kind;        /* BlkKind，用于引用竖线/列表圆点 */
    int blk;         /* 所属块下标（1.2.7：图片异步到达后按块重排要锚定位置） */
} Line;

typedef struct {
    Line *lines;
    int   n;
    int   cap;

    int  *pages;     /* pages[i] = 第 i 页首行下标；pages[npages] = n */
    int   npages;

    int   width;
    int   height;    /* 正文区高度（一页的可视高度） */
    /* 1.2.9：第一页正文高度（标题页眉只出现在第一页，后续页正文更长）。
     * first_h <= 0 时与 height 相同（向后兼容）。 */
    int   first_h;
} Layout;

void layout_free(Layout *L);

/* width: 正文可用宽度；height: 一页正文高度；base_px: 正文字号 */
void layout_build(Layout *L, const Doc *doc, int width, int height, int base_px);
/* 首页高度单独指定：第一页有标题页眉（矮一截），后续页整页都是正文 */
void layout_build2(Layout *L, const Doc *doc, int width, int height, int base_px,
                   int first_h);

/* 第 p 页的起始 y（页内 y 偏移的基准，画页时用） */
int layout_page_top(const Layout *L, int page);
/* 第 p 页的可用高度 */
int layout_page_h(const Layout *L, int page);

#endif
