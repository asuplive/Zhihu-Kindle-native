#include "layout.h"
#include "common.h"
#include "font.h"

#include <stdlib.h>

void layout_free(Layout *L) {
    free(L->lines);
    free(L->pages);
    memset(L, 0, sizeof(*L));
}

static int is_cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0x3000 && c <= 0x303F) ||
           (c >= 0x20000 && c <= 0x2FA1F);
}

/* 不能出现在行首的标点 */
static int forbidden_start(uint32_t c) {
    switch (c) {
        case 0x3002: case 0xFF0C: case 0x3001: case 0xFF1B: case 0xFF1A:
        case 0xFF1F: case 0xFF01: case 0xFF09: case 0x3011: case 0x300B:
        case 0x300D: case 0x300F: case 0x3015: case 0x3009: case 0x3017:
        case 0xFF3D: case 0xFF3B: case 0x3014: case 0x3016:
        case '.': case ',': case ';': case ':': case '?': case '!':
        case ')': case ']': case '}': case '%':
        case 0x201D: case 0x2019: case 0x2026: case 0x00B7: case 0xFF5E:
            return 1;
        default:
            return 0;
    }
}

/* 不能出现在行尾的标点 */
static int forbidden_end(uint32_t c) {
    switch (c) {
        case 0xFF08: case 0x3010: case 0x300A: case 0x300C: case 0x300E:
        case 0x3014: case 0x3008: case 0x3016: case 0xFF3B: case 0xFF5B:
        case '(': case '[': case '{':
        case 0x201C: case 0x2018:
            return 1;
        default:
            return 0;
    }
}

static int can_break(uint32_t prev, uint32_t cur) {
    if (cur == ' ' || prev == ' ') return 1;
    if (forbidden_start(cur)) return 0;
    if (forbidden_end(prev)) return 0;
    if (is_cjk(prev) || is_cjk(cur)) return 1;
    return 0;
}

static void push_line(Layout *L, Line ln) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 512;
        L->lines = xrealloc(L->lines, sizeof(Line) * (size_t)L->cap);
    }
    L->lines[L->n++] = ln;
}

/* 一行内的字符宽度（含 ASCII 空格） */
static int ch_width(int px, const char *s, int n) { return font_width(px, s, n); }

/* 图片块的高度：ui.c 会把 text 改写成 "路径\t宽\t高" */
static int block_image_h(const Blk *b) {
    const char *t = b->text ? strrchr(b->text, '\t') : NULL;
    int h = t ? atoi(t + 1) : 0;
    return h > 0 ? h : 0;
}

void layout_build(Layout *L, const Doc *doc, int width, int height, int base_px) {
    layout_build2(L, doc, width, height, base_px, height);
}

int layout_page_h(const Layout *L, int page) {
    if (!L || L->height <= 0) return 1;
    if (page <= 0) return L->first_h > 0 ? L->first_h : L->height;
    return L->height;
}

/* 第 p 页内容在"全局 y"里的起始位置：第一页从 0 起，后面各页依次排 */
int layout_page_top(const Layout *L, int page) {
    if (!L || page <= 0) return 0;
    int h0 = layout_page_h(L, 0);
    return h0 + (page - 1) * L->height;
}

void layout_build2(Layout *L, const Doc *doc, int width, int height, int base_px,
                   int first_h) {
    memset(L, 0, sizeof(*L));
    L->width = width;
    L->height = height > 0 ? height : 1;
    L->first_h = first_h > 0 ? first_h : L->height;

    /* 1.2.9：第一页矮（有标题页眉）、后续页高（没有页眉）。
     * 用"当前页还剩多少"而不是 y % ph 来判断换页，两种页高才都能支持。 */
    int cur_h = L->first_h;          /* 当前页的页高 */
    int page_top = 0;                /* 当前页在全局 y 里的起点 */

    /* 分页起点的逐页记录：pstarts[p] = 第 p 页首行下标 */
    int  *pstarts = xcalloc(64, sizeof(int));
    int   pcap = 64;
    int   pn = 1;
    pstarts[0] = 0;
#define ADV_PAGE()                                                          \
    do {                                                                    \
        while (y >= page_top + cur_h) {                                     \
            page_top += cur_h;                                              \
            cur_h = L->height;                                              \
            if (pn >= pcap) {                                               \
                pcap *= 2;                                                  \
                pstarts = xrealloc(pstarts, sizeof(int) * (size_t)pcap);    \
            }                                                               \
            pstarts[pn++] = L->n;                                           \
        }                                                                   \
    } while (0)

    int y = 0;
    for (int bi = 0; bi < doc->n; bi++) {
        const Blk *b = &doc->v[bi];

        /* 图片：整张占位，绝不允许被分页切成两半。
         * 排不下就整体挪到下一页（eink 上一张图被腰斩比留白难看得多）。 */
        if (b->kind == BLK_IMAGE) {
            int ih = block_image_h(b);
            if (ih <= 0) continue;              /* 没拿到尺寸（解码失败）就不占位 */
            int ph = cur_h;
            if (ih > ph) ih = ph;               /* 比一页还高：按一页截断 */
            int rem = ph - (y - page_top);
            if (rem < ih && y > page_top) y += rem;    /* 挪到下一页顶端 */
            ADV_PAGE();
            Line ln;
            memset(&ln, 0, sizeof(ln));
            ln.s = b->text;
            ln.len = 0;
            ln.x = 0;
            ln.y = y;
            ln.px = ih;
            ln.kind = BLK_IMAGE;
            ln.blk = bi;
            push_line(L, ln);
            y += ih + base_px / 2;
            ADV_PAGE();
            continue;
        }

        int px = base_px;
        int bold = 0;
        int indent = base_px;          /* 中文段首缩进一个字 */
        int extra_before = 0, extra_after = 0;
        int line_gap = base_px * 55 / 100; /* 行距 1.55 */

        switch (b->kind) {
            case BLK_H1: px = base_px * 2; bold = 1; indent = 0; extra_before = base_px; extra_after = base_px / 2; line_gap = px / 4; break;
            case BLK_H2: px = base_px * 3 / 2; bold = 1; indent = 0; extra_before = base_px * 3 / 4; extra_after = base_px / 3; line_gap = px / 4; break;
            case BLK_H3: px = base_px * 5 / 4; bold = 1; indent = 0; extra_before = base_px / 2; extra_after = base_px / 4; line_gap = px / 4; break;
            case BLK_QUOTE: indent = base_px; extra_before = base_px / 3; extra_after = base_px / 3; break;
            case BLK_CODE: px = base_px * 85 / 100; indent = base_px; line_gap = px / 3; extra_before = base_px / 3; extra_after = base_px / 3; break;
            case BLK_LI: indent = base_px * 3 / 2; extra_after = base_px / 4; break;
            /* 作者 · 赞同 · 时间：小一号灰字，排在正文最上面 */
            case BLK_META: px = base_px * 4 / 5; indent = 0; extra_after = base_px; line_gap = px / 2; break;
            default: indent = base_px * 2; extra_after = base_px / 3; break;
        }

        int line_h = px + line_gap;
        if (b->kind == BLK_H1 || b->kind == BLK_H2 || b->kind == BLK_H3) line_h = px + px / 4;

        y += extra_before;

        /* 段首别贴着页底：一行字实际要占 px*(1+1/4) 高（字身 + 下伸部），
         * 标题还得带上后面一行正文，否则会出现"标题孤零零卡在页脚上、
         * 字被裁掉半个"——真机上看着就像和页脚重叠。
         * 排不下就整段挪到下一页顶端。 */
        {
            int rem = cur_h - (y - page_top);
            int need = px + px / 4;
            if (b->kind == BLK_H1 || b->kind == BLK_H2 || b->kind == BLK_H3)
                need = line_h + base_px;
            if (rem < need && (y - page_top) > 0) y += rem;
            ADV_PAGE();
        }

        const char *text = b->text;
        int tlen = (int)strlen(text);

        if (b->kind == BLK_CODE) {
            /* 代码/公式：不折行地尽量塞，超宽则按字符硬断 */
        }

        int pos = 0;
        int first = 1;
        while (pos < tlen) {
            int avail = width - (first ? indent : 0);
            if (avail < px) avail = px;
            int x = 0;
            int i = pos;
            int last_break = -1;
            uint32_t prev_cp = 0;

            while (i < tlen) {
                uint32_t cp = 0;
                int n = u8_decode(text + i, (size_t)(tlen - i), &cp);
                if (n <= 0) { n = 1; cp = (unsigned char)text[i]; }
                int w = ch_width(px, text + i, n);
                if (x + w > avail) break;
                if (i > pos) {
                    if (can_break(prev_cp, cp)) last_break = i;
                }
                x += w;
                prev_cp = cp;
                i += n;
            }

            int cut;
            if (i >= tlen) {
                cut = tlen;
            } else if (last_break > pos) {
                cut = last_break;
                /* 行尾的空格不进入渲染 */
                while (cut > pos && text[cut - 1] == ' ') cut--;
            } else {
                cut = i > pos ? i : pos + u8_decode(text + pos, (size_t)(tlen - pos), NULL);
            }

            /* 去掉首部空格 */
            int start = pos;
            while (start < cut && text[start] == ' ') start++;

            Line ln;
            ln.s = text + start;
            ln.len = cut - start;
            ln.x = first ? indent : 0;
            ln.y = y;
            ln.px = px;
            ln.bold = bold;
            ln.baseline = px;
            ln.kind = b->kind;
            ln.blk = bi;
            push_line(L, ln);

            y += line_h;
            ADV_PAGE();
            pos = cut;
            first = 0;
            if (pos == tlen) break;
            if (L->n > 60000) break; /* 防御：超大文档截断 */
        }
        y += extra_after;
        ADV_PAGE();
    }

    /* 分页：页起点在排版的 ADV_PAGE() 里逐页记录。
     * 第一页页高是 first_h（有标题页眉），后面每页都是 height。 */
    L->npages = pn + 1;
    L->pages = xcalloc((size_t)L->npages + 1, sizeof(int));
    for (int p = 0; p < pn; p++) L->pages[p] = pstarts[p];
    L->pages[L->npages] = L->n;
    free(pstarts);
#undef ADV_PAGE
}
