#include "content.h"
#include "common.h"

#include <ctype.h>
#include <strings.h>

void doc_init(Doc *d) { d->v = NULL; d->n = 0; d->cap = 0; }

void doc_free(Doc *d) {
    for (int i = 0; i < d->n; i++) free(d->v[i].text);
    free(d->v);
    doc_init(d);
}

void doc_add(Doc *d, const char *text, int kind) {
    if (!text) return;
    /* 丢掉空白块 */
    const char *p = text;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (*p == 0) return;
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->v = xrealloc(d->v, sizeof(Blk) * (size_t)d->cap);
    }
    d->v[d->n].text = xstrdup(text);
    d->v[d->n].kind = (uint8_t)kind;
    d->n++;
}

/* ---------------- 实体解码 ---------------- */

static const struct { const char *name; uint32_t cp; } ENT[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0x00A0}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"middot", 0x00B7}, {"bull", 0x2022}, {"times", 0x00D7}, {"divide", 0x00F7},
    {"laquo", 0x00AB}, {"raquo", 0x00BB}, {"deg", 0x00B0}, {"copy", 0x00A9},
    {"reg", 0x00AE}, {"trade", 0x2122}, {"euro", 0x20AC}, {"pound", 0x00A3},
    {"yen", 0x00A5}, {"sect", 0x00A7}, {"para", 0x00B6}, {"permil", 0x2030},
    {"dagger", 0x2020}, {"prime", 0x2032}, {"frac12", 0x00BD}, {"frac14", 0x00BC},
    {"sup2", 0x00B2}, {"sup3", 0x00B3}, {"minus", 0x2212}, {"plusmn", 0x00B1},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"infin", 0x221E},
    {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
    {"lambda", 0x03BB}, {"mu", 0x03BC}, {"pi", 0x03C0}, {"sigma", 0x03C3},
    {"omega", 0x03C9}, {"Delta", 0x0394}, {"Omega", 0x03A9}, {"Sigma", 0x03A3},
    {"hearts", 0x2665}, {"star", 0x2606}, {"check", 0x2713}, {"cross", 0x2717},
};

static void decode_entities(const char *s, Buf *out) {
    const char *p = s;
    while (*p) {
        if (*p != '&') { buf_addc(out, *p++); continue; }
        const char *semi = strchr(p, ';');
        if (!semi || semi - p > 12) { buf_addc(out, *p++); continue; }
        char ent[16];
        size_t n = (size_t)(semi - p - 1);
        memcpy(ent, p + 1, n);
        ent[n] = 0;

        if (ent[0] == '#') {
            uint32_t cp = 0;
            if (ent[1] == 'x' || ent[1] == 'X') cp = (uint32_t)strtoul(ent + 2, NULL, 16);
            else cp = (uint32_t)strtoul(ent + 1, NULL, 10);
            if (cp == 0) cp = ' ';
            char tmp[4];
            int k = u8_encode(cp, tmp);
            buf_add(out, tmp, (size_t)k);
            p = semi + 1;
            continue;
        }
        int hit = 0;
        for (size_t i = 0; i < sizeof(ENT) / sizeof(ENT[0]); i++) {
            if (strcmp(ENT[i].name, ent) == 0) {
                char tmp[4];
                int k = u8_encode(ENT[i].cp, tmp);
                buf_add(out, tmp, (size_t)k);
                hit = 1;
                break;
            }
        }
        if (hit) p = semi + 1;
        else buf_addc(out, *p++);
    }
}

/* 把 &nbsp; 等转成普通空格、去零宽字符、压缩连续空白 */
static void normalize_ws(const char *in, Buf *out) {
    int space_pending = 0;
    const char *p = in;
    while (*p) {
        uint32_t cp = 0;
        int n = u8_decode(p, strlen(p), &cp);
        if (cp == 0x00A0 || cp == 0x3000 || cp == ' ' || cp == '\t' || cp == '\r') {
            space_pending = 1;
            p += n;
            continue;
        }
        if (cp == '\n') {
            /* 段内换行当空格 */
            space_pending = 1;
            p += n;
            continue;
        }
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) { p += n; continue; }
        if (space_pending && out->len > 0) buf_addc(out, ' ');
        space_pending = 0;
        buf_add(out, p, (size_t)n);
        p += n;
    }
}

/* ---------------- 噪声过滤 ---------------- */

static int is_junk(const char *t) {
    static const char *CTA[] = {
        "下载知乎客户端", "打开知乎", "扫码", "扫码下载", "点击查看", "展开阅读全文",
        "查看全部", "查看更多", "收起", "申请转载", "著作权归作者所有", "本文来自",
        "开通盐选", "盐选会员", "本回答为盐选", "付费内容", "购买后可阅读",
        "App 内打开", "在 App 中打开", "继续浏览", "登录后", "还没有人", "关注问题",
        "写回答", "分享", "收藏", "举报",
    };
    if (!t || !*t) return 1;
    size_t len = strlen(t);
    if (len > 60) return 0; /* 长段落不动，避免误删正文 */
    for (size_t i = 0; i < sizeof(CTA) / sizeof(CTA[0]); i++)
        if (strstr(t, CTA[i])) return 1;
    /* 「编辑于 2023-01-01」「发布于 ...」这类时间尾巴 */
    if (str_startswith(t, "编辑于 ") || str_startswith(t, "发布于 ") ||
        str_startswith(t, "修改于 ") || str_startswith(t, "更新于 "))
        return 1;
    /* 纯标点/单字块 */
    if (u8_strlen(t) <= 1 && !isalnum((unsigned char)t[0])) return 1;
    return 0;
}

/* ---------------- 图片地址 ---------------- */

void imgrefs_init(ImgRefs *r) {
    if (!r) return;
    r->v = NULL;
    r->n = r->cap = 0;
}

void imgrefs_free(ImgRefs *r) {
    if (!r) return;
    for (int i = 0; i < r->n; i++) free(r->v[i]);
    free(r->v);
    r->v = NULL;
    r->n = r->cap = 0;
}

/* 取标签里某个属性的值。src 可能是 data: 占位图，知乎的真实地址在
 * data-original / data-actualsrc 上，所以调用方按优先级挨个取。 */
static int attr_value(const char *tag, const char *end, const char *name,
                      char *out, size_t outsz) {
    size_t nl = strlen(name);
    for (const char *p = tag; p + nl + 1 < end; p++) {
        if ((p[0] != ' ' && p[0] != '\t' && p[0] != '\n' && p[0] != '\r')) continue;
        const char *q = p + 1;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q + nl >= end) continue;
        if (strncasecmp(q, name, nl) != 0) continue;
        q += nl;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != '=') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end) continue;
        char quote = 0;
        if (*q == '"' || *q == '\'') { quote = *q++; }
        const char *v = q;
        while (q < end && (quote ? *q != quote : (*q != ' ' && *q != '\t' && *q != '>'))) q++;
        size_t n = (size_t)(q - v);
        if (n == 0 || n + 1 > outsz) return -1;
        memcpy(out, v, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

/* 把 HTML 里的 &amp; 之类还原（图片地址里经常带 &amp;） */
static void unescape_attr(const char *in, char *out, size_t outsz) {
    out[0] = 0;
    if (!in || !*in) return;
    if (!strchr(in, '&')) { snprintf(out, outsz, "%s", in); return; }
    Buf b;
    buf_init(&b);
    decode_entities(in, &b);
    buf_cstr(&b);
    snprintf(out, outsz, "%s", b.p ? b.p : "");
    buf_free(&b);
}

/* 统一成绝对地址：
 *   //pic1.zhimg.com/x.jpg   -> https://pic1.zhimg.com/x.jpg
 *   /equation?tex=...        -> 站内绝对地址（这类是公式，解码会失败，交给上层兜底）
 *   http://...               -> https://...（Kindle 的 TLS 只走 443，明文 http 会被拦）
 * 注意输出要写进独立缓冲区：https 比 http 多一个字符，原地改会截掉尾巴。 */
static int normalize_img_url(const char *in, char *out, size_t outsz) {
    if (!in || !*in) return -1;
    if (strncasecmp(in, "data:", 5) == 0) return -1;          /* 内联占位图，没意义 */
    if (strncasecmp(in, "http://", 7) == 0) {
        snprintf(out, outsz, "https://%s", in + 7);
    } else if (str_startswith(in, "//")) {
        snprintf(out, outsz, "https://%s", in + 2);
    } else if (in[0] == '/') {
        snprintf(out, outsz, "https://www.zhihu.com%s", in);
    } else if (str_startswith(in, "https://")) {
        snprintf(out, outsz, "%s", in);
    } else {
        return -1;                                            /* 相对路径：补不全，放弃 */
    }
    return 0;
}

static int imgrefs_push(ImgRefs *r, const char *url) {
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->v[i], url) == 0) return i;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->v = xrealloc(r->v, sizeof(char *) * (size_t)r->cap);
    }
    r->v[r->n] = xstrdup(url);
    return r->n++;
}

/* ---------------- 主解析 ---------------- */

static void flush_block(Buf *text, Doc *out, int kind) {
    if (text->len == 0) return;
    Buf norm;
    buf_init(&norm);
    decode_entities(text->p ? text->p : "", &norm);
    Buf fin;
    buf_init(&fin);
    normalize_ws(buf_cstr(&norm), &fin);
    buf_cstr(&fin);
    char *t = str_trim(fin.p ? fin.p : (char *)"");
    if (!is_junk(t) && *t) doc_add(out, t, kind);
    buf_free(&norm);
    buf_free(&fin);
    buf_reset(text);
}

void html_to_doc_img(const char *html, Doc *out, int mode, ImgRefs *refs) {
    if (!html) return;
    Buf text;
    buf_init(&text);
    const char *p = html;
    int kind = BLK_PARA;
    int skip_depth = 0;
    int quote_depth = 0;   /* blockquote 里再套 <p> 时不能丢掉引用样式 */

    while (*p) {
        if (*p != '<') {
            if (!skip_depth) buf_addc(&text, *p);
            p++;
            continue;
        }
        const char *end = strchr(p, '>');
        if (!end) break;
        char tag[32] = {0};
        size_t ti = 0;
        const char *q = p + 1;
        int closing = 0;
        if (*q == '/') { closing = 1; q++; }
        while (q < end && ti < sizeof(tag) - 1 &&
               (isalnum((unsigned char)*q) || *q == '-' || *q == ':')) {
            tag[ti++] = (char)tolower((unsigned char)*q);
            q++;
        }
        tag[ti] = 0;

        if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
            strcmp(tag, "noscript") == 0 || strcmp(tag, "svg") == 0) {
            if (!closing) skip_depth++;
            else if (skip_depth) skip_depth--;
            p = end + 1;
            continue;
        }
        if (skip_depth) { p = end + 1; continue; }

        if (strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 || strcmp(tag, "section") == 0 ||
            strcmp(tag, "article") == 0 || strcmp(tag, "table") == 0 || strcmp(tag, "tr") == 0 ||
            strcmp(tag, "figure") == 0 || strcmp(tag, "figcaption") == 0) {
            flush_block(&text, out, kind);
            kind = quote_depth > 0 ? BLK_QUOTE : BLK_PARA;
        } else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) {
            flush_block(&text, out, kind);
            kind = (tag[1] == '1') ? BLK_H1 : (tag[1] == '2' ? BLK_H2 : BLK_H3);
        } else if (strcmp(tag, "blockquote") == 0) {
            flush_block(&text, out, kind);
            if (closing) {
                if (quote_depth > 0) quote_depth--;
                kind = quote_depth > 0 ? BLK_QUOTE : BLK_PARA;
            } else {
                quote_depth++;
                kind = BLK_QUOTE;
            }
        } else if (strcmp(tag, "pre") == 0) {
            flush_block(&text, out, kind);
            kind = closing ? BLK_PARA : BLK_CODE;
        } else if (strcmp(tag, "li") == 0) {
            flush_block(&text, out, kind);
            kind = closing ? BLK_PARA : BLK_LI;
        } else if (strcmp(tag, "br") == 0) {
            flush_block(&text, out, kind);
        } else if (strcmp(tag, "img") == 0) {
            flush_block(&text, out, kind);
            if (closing || mode == IMG_NONE) { p = end + 1; continue; }

            /* 知乎正文里的图片：src 常常是内联占位图，真地址在
             * data-actualsrc / data-original / data-src 上。
             * 以前整个 <img> 直接跳过，所以"文章里一张图都看不到"。 */
            char url[1024] = {0};
            char abs[1200] = {0};
            static const char *keys[] = { "data-actualsrc", "data-original", "data-src", "src" };
            for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && !url[0]; k++)
                attr_value(p, end, keys[k], url, sizeof(url));
            if (!url[0]) { p = end + 1; continue; }
            char plain[1200] = {0};
            unescape_attr(url, plain, sizeof(plain));
            if (normalize_img_url(plain, abs, sizeof(abs)) != 0) { p = end + 1; continue; }

            if (mode == IMG_REAL && refs) {
                int idx = imgrefs_push(refs, abs);
                char num[16];
                snprintf(num, sizeof(num), "%d", idx);
                doc_add(out, num, BLK_IMAGE);
            } else {
                doc_add(out, "[图片]", BLK_PARA);
            }
        } else if (strcmp(tag, "hr") == 0) {
            flush_block(&text, out, kind);
        }
        p = end + 1;
    }
    flush_block(&text, out, kind);
    buf_free(&text);

    /* 合并连续的列表项：保持 LI 标记，不做额外处理 */
}

void html_to_doc(const char *html, Doc *out, int keep_image_marker) {
    /* 老接口：mode 只区分「丢图」和「[图片] 占位」。
     * 真图路径必须带 ImgRefs，所以单独走 html_to_doc_img。 */
    html_to_doc_img(html, out, keep_image_marker ? IMG_PLACEHOLDER : IMG_NONE, NULL);
}

void text_to_doc(const char *text, Doc *out) {
    if (!text) return;
    Buf cur;
    buf_init(&cur);
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            flush_block(&cur, out, BLK_PARA);
            p++;
            continue;
        }
        buf_addc(&cur, *p++);
    }
    flush_block(&cur, out, BLK_PARA);
    buf_free(&cur);
}
