/* content.h —— 知乎正文结构化：HTML -> 段落块
 *
 * 阅读器的排版引擎不认识 HTML，也不需要认识。我们只把正文抽成
 * 「段落 / 标题 / 引用 / 代码 / 列表项 / 元信息 / 图片」这些块，剩下的交给排版层。
 * 顺带把盐选推广、App 下载、版权声明等噪声块丢掉。
 */
#ifndef ZH_CONTENT_H
#define ZH_CONTENT_H

#include <stdint.h>

typedef enum {
    BLK_PARA = 0,
    BLK_H1,
    BLK_H2,
    BLK_H3,
    BLK_QUOTE,
    BLK_CODE,
    BLK_LI,
    /* 详情页顶部的「作者 · 赞同 · 发布时间」。单独一种块，是为了让它
     * 用小一号的灰字排在最上面，和正文段落区分开（1.2.5 之前这类信息
     * 根本没被解析，用户看不到作者和赞同数）。 */
    BLK_META,
    /* 图片。text 的内容是 "缓存文件路径\t宽\t高"，由 ui.c 在拿到
     * 解码结果后填入；排版层只按高度占位，绘制层负责把它贴上去。 */
    BLK_IMAGE,
} BlkKind;

typedef struct {
    char   *text;   /* UTF-8，已去标签、解码实体、合并空白 */
    uint8_t kind;
} Blk;

typedef struct {
    Blk  *v;
    int   n;
    int   cap;
} Doc;

/* 正文里出现的图片地址（已补全成绝对 https 地址、按出现顺序、已去重） */
typedef struct {
    char **v;
    int    n;
    int    cap;
} ImgRefs;

void doc_init(Doc *d);
void doc_free(Doc *d);
void doc_add(Doc *d, const char *text, int kind);

/* 图片处理模式 */
enum { IMG_NONE = 0, IMG_PLACEHOLDER = 1, IMG_REAL = 2 };

/* HTML -> Doc。
 *   mode = IMG_NONE        图片直接丢掉
 *   mode = IMG_PLACEHOLDER 图片位置插入「[图片]」占位（省流量模式）
 *   mode = IMG_REAL        图片位置插入 BLK_IMAGE，text 为 ImgRefs 的下标；
 *                          真正的地址收集到 refs 里（refs 可为 NULL，此时退回占位）。
 * refs 由调用方初始化/释放。 */
void html_to_doc_img(const char *html, Doc *out, int mode, ImgRefs *refs);

/* 兼容旧调用：keep_image_marker 为 1 时插入「[图片]」 */
void html_to_doc(const char *html, Doc *out, int keep_image_marker);

/* 纯文本兜底（接口里部分字段是纯文本） */
void text_to_doc(const char *text, Doc *out);

void imgrefs_init(ImgRefs *r);
void imgrefs_free(ImgRefs *r);

#endif /* ZH_CONTENT_H */
