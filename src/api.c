#include "api.h"
#include "json.h"
#include "net.h"
#include "zse96.h"

#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

static char g_home[256] = "/mnt/us/zhihu";
static char g_cookie[4096];
static char g_err[256];
static int  g_warmed = 0;
static int  g_last_status = 0;   /* 最近一次 api_call 的 HTTP 状态码，供端点诊断 */
static char g_last_url[512];     /* 最近一次真正发出去的 URL（含查询串） */
static char g_last_head[256];    /* 响应体前 200 字节（单行化），失败时最有价值 */
/* 1.2.7：feed 的 paging.next 原样留着。知乎推荐流按 page_number 翻页会
 * 反复给同一批（真机日志里 3 页只有 6 条），必须照着服务端给的 next 走。 */
static char g_next_url[1024];

int api_last_status(void) { return g_last_status; }
const char *api_last_url(void) { return g_last_url; }
const char *api_last_head(void) { return g_last_head; }
const char *api_last_next(void) { return g_next_url; }

void api_set_home(const char *home) {
    if (!home) return;
    snprintf(g_home, sizeof(g_home), "%s", home);
}

const char *api_error(void) { return g_err; }

void api_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}
const char *api_cookie(void) { return g_cookie; }

int api_has_login(void) { return strstr(g_cookie, "z_c0=") != NULL; }

/* 网页端接口用的 UA。
 * 为什么要单独切：知乎对 `desktop=true` 的接口是按 UA 分流的，
 * 拿安卓手机 UA 去请求桌面接口，会被当成"不支持的客户端"。
 * 1.2.4 之前的 404（而不是 403）很可能就是这个原因——签名错是 403，
 * 而"客户端不认识"走的不是同一个错误码。 */
static const char *UA_PC =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

/* config.txt 的 ua=pc|mobile 可以不改代码切换（默认 pc）。
 * 万一哪天桌面 UA 反而更容易被拦，用户在设备上改一行就能回退。 */
static int ua_is_pc(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 1;
    char path[320];
    snprintf(path, sizeof(path), "%s/config.txt", g_home);
    size_t n = 0;
    char *s = read_file(path, &n);
    if (!s) return cached;
    char *p = s;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *line = str_trim(p);
        if (str_startswith(line, "ua=")) {
            const char *v = line + 3;
            if (str_ieq(v, "mobile") || str_ieq(v, "app")) cached = 0;
            else cached = 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(s);
    return cached;
}

static void errf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

/* ---------------- cookie 管理 ---------------- */

static void cookie_put(const char *name, const char *value) {
    /* 覆盖同名项，否则追加 */
    char pat[128];
    snprintf(pat, sizeof(pat), "%s=", name);
    char *p = strstr(g_cookie, pat);
    if (p && (p == g_cookie || p[-1] == ' ' || p[-1] == ';')) {
        char *end = strchr(p, ';');
        Buf b;
        buf_init(&b);
        buf_add(&b, g_cookie, (size_t)(p - g_cookie));
        buf_printf(&b, "%s%s", pat, value);
        if (end) buf_adds(&b, end);
        snprintf(g_cookie, sizeof(g_cookie), "%s", buf_cstr(&b));
        buf_free(&b);
        return;
    }
    if (g_cookie[0]) strncat(g_cookie, "; ", sizeof(g_cookie) - strlen(g_cookie) - 1);
    strncat(g_cookie, pat, sizeof(g_cookie) - strlen(g_cookie) - 1);
    strncat(g_cookie, value, sizeof(g_cookie) - strlen(g_cookie) - 1);
}

void api_set_cookie(const char *cookie) {
    snprintf(g_cookie, sizeof(g_cookie), "%s", cookie ? cookie : "");
    /* 去掉可能的 "Cookie:" 前缀与换行 */
    char *p = strstr(g_cookie, "Cookie:");
    if (p == g_cookie) memmove(g_cookie, g_cookie + 7, strlen(g_cookie + 7) + 1);
    for (char *q = g_cookie; *q; q++) if (*q == '\r' || *q == '\n') *q = ' ';
}

int api_save_cookie(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/cookie.txt", g_home);
    return write_file_atomic(path, g_cookie, strlen(g_cookie));
}

void api_load_cookie(void) {
    char path[320];
    snprintf(path, sizeof(path), "%s/cookie.txt", g_home);
    size_t n = 0;
    char *s = read_file(path, &n);
    if (s) {
        api_set_cookie(s);
        free(s);
    }
}

static void absorb_set_cookie(HttpResp *r) {
    if (r->set_cookie.len == 0) return;
    const char *p = r->set_cookie.p;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 3) {
            const char *eq = memchr(p, '=', len);
            if (eq) {
                char name[64];
                size_t nl2 = (size_t)(eq - p);
                if (nl2 < sizeof(name)) {
                    memcpy(name, p, nl2);
                    name[nl2] = 0;
                    char val[512];
                    size_t vl = len - nl2 - 1;
                    if (vl < sizeof(val)) {
                        memcpy(val, eq + 1, vl);
                        val[vl] = 0;
                        char *trim = str_trim(val);
                        if (strcmp(name, "d_c0") == 0 || strcmp(name, "z_c0") == 0 ||
                            strcmp(name, "_xsrf") == 0 || strcmp(name, "q_c1") == 0)
                            cookie_put(name, trim);
                    }
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
}

/* ---------------- 基础请求 ---------------- */

/* 取 cookie 里某项的值（签名要用 d_c0 的裸值，不能带 "d_c0=" 前缀） */
static int cookie_value(const char *name, char *out, size_t outsz) {
    out[0] = 0;
    char pat[96];
    snprintf(pat, sizeof(pat), "%s=", name);
    const char *p = strstr(g_cookie, pat);
    while (p && p != g_cookie && p[-1] != ' ' && p[-1] != ';') p = strstr(p + 1, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *end = strchr(p, ';');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

/* 把响应体前 N 字节压成一行，写进日志。
 * 1.2.4 的日志里 `len=0` 其实是我们自己没填 buf——失败时 buf 一直是空的，
 * 于是"响应体为空"这个结论是假的。这里直接读 HttpResp.body。 */
static void log_body(const char *tag, const char *p, size_t n) {
    char b[241];
    size_t k = n < 200 ? n : 200;
    if (!p) k = 0;
    if (k) memcpy(b, p, k);
    b[k] = 0;
    for (size_t i = 0; i < k; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c < 0x20 || c == 0x7f) b[i] = ' ';
    }
    snprintf(g_last_head, sizeof(g_last_head), "%s", b);
    logmsg("api: %s status=%d len=%zu body=%s", tag, g_last_status, n, b);
}

/* 从知乎的错误响应里抠出人话。知乎 404 的 body 通常是
 * {"error":{"code":404,"message":"..."}}，直接显示比"内容不存在(404)"有用得多。 */
static void err_from_body(const char *json, size_t len, const char *fallback) {
    if (!json || len == 0) { errf("%s（响应体为空）", fallback); return; }
    JVal *root = json_parse(json, len);
    if (!root) {
        /* 不是 JSON：可能是 HTML 错误页，把前 80 字节带出来 */
        char b[120];
        size_t k = len < 80 ? len : 80;
        memcpy(b, json, k);
        b[k] = 0;
        for (size_t i = 0; i < k; i++) if ((unsigned char)b[i] < 0x20) b[i] = ' ';
        errf("%s；原文：%s", fallback, str_trim(b));
        return;
    }
    JVal *e = json_get(root, "error");
    const char *msg = e ? json_get_str(e, "message", NULL) : NULL;
    long code = e ? json_get_int(e, "code", 0) : 0;
    if (msg && *msg) errf("%s（%ld）：%s", fallback, code, msg);
    else errf("%s", fallback);
    json_free(root);
}

/* 真正干活的那个。referer 为空就用首页——但详情类接口最好带上下文，
 * 知乎会校验来源页面（裸 referer 过去在 v4 详情上更容易被判异常）。 */
static int api_call_ref(const char *method, const char *path, const char *body,
                        const char *referer, Buf *out) {
    char url[1400];
    snprintf(url, sizeof(url), "https://www.zhihu.com%s", path);
    snprintf(g_last_url, sizeof(g_last_url), "%s", url);
    g_last_head[0] = 0;

    char dc0[256];
    if (cookie_value("d_c0", dc0, sizeof(dc0)) != 0) dc0[0] = 0;

    char sig[128];
    zse96_sign(url, dc0, body, sig, sizeof(sig));

    /* x-zst-81：知乎某些接口的额外反爬头。浏览器里由 JS 生成，
     * 我们无法凭空算出来；但如果用户导入的 cookie 里带了，就原样透传
     * （zhihu-plus-plus 也是这么做的：非空才加）。 */
    char zst[256];
    zst[0] = 0;
    if (cookie_value("zst_81", zst, sizeof(zst)) != 0 &&
        cookie_value("x-zst-81", zst, sizeof(zst)) != 0)
        zst[0] = 0;

    const char *ref = (referer && *referer) ? referer : "https://www.zhihu.com/";
    /* UA 是全局的，改完必须还原：登录/二维码那些接口反过来要移动端 UA，
     * 调完这里如果不还原，扫码就会被当成桌面浏览器而失败。 */
    char ua_prev[256];
    snprintf(ua_prev, sizeof(ua_prev), "%s", http_user_agent());
    http_set_user_agent(ua_is_pc() ? UA_PC : NULL);   /* NULL = 用默认移动端 UA */

    Buf hdr;
    buf_init(&hdr);
    buf_printf(&hdr, "x-zse-93: %s\r\nx-zse-96: %s\r\n", ZSE93_VERSION, sig);
    if (zst[0]) buf_printf(&hdr, "x-zst-81: %s\r\n", zst);
    buf_printf(&hdr, "x-requested-with: fetch\r\n"
                     "Accept: application/json, text/plain, */*\r\n"
                     "Referer: %s\r\n"
                     "Origin: https://www.zhihu.com\r\n", ref);
    if (body) buf_adds(&hdr, "Content-Type: application/json\r\n");
    buf_printf(&hdr, "Cookie: %s\r\n", g_cookie);

    HttpResp r;
    http_resp_init(&r);
    int rc = http_request(method, url, buf_cstr(&hdr), body, &r, NULL);
    buf_free(&hdr);
    http_set_user_agent(ua_prev);
    if (rc != 0) {
        g_last_status = 0;
        errf("%s", r.err[0] ? r.err : "网络请求失败");
        logmsg("api: %s %s 网络失败: %s", method, url, r.err);
        http_resp_free(&r);
        return -1;
    }
    g_last_status = r.status;
    absorb_set_cookie(&r);

    if (r.status != 200) {
        const char *msg = "请求失败";
        if (r.status == 403) msg = "知乎拒绝访问（403），多为风控或需要登录";
        else if (r.status == 404) msg = "内容不存在（404）";
        else if (r.status == 401) msg = "需要登录（401）";
        else if (r.status == 429) msg = "请求过于频繁（429）";
        /* 把 url 和 body 一起记下来：404 到底是"资源没了"还是"我们的请求不对"，
         * 看这两样就够了，不用再猜。 */
        log_body(url, r.body.p, r.body.len);
        err_from_body(r.body.p, r.body.len, msg);
        http_resp_free(&r);
        return -1;
    }
    /* 200 就说明这次请求本身没出错：清掉上一次的错误文案。 */
    g_err[0] = 0;
    logmsg("api: %s %s -> 200 len=%zu", method, url, r.body.len);
    if (out) {
        buf_init(out);
        buf_add(out, r.body.p, r.body.len);
    }
    http_resp_free(&r);
    return 0;
}

static int api_call(const char *method, const char *path, const char *body, Buf *out) {
    return api_call_ref(method, path, body, NULL, out);
}

int api_warmup(void) {
    if (g_warmed) return 0;
    api_load_cookie();
    if (strstr(g_cookie, "d_c0=")) { g_warmed = 1; return 0; }

    HttpResp r;
    http_resp_init(&r);
    const char *hdr = "Accept: text/html,application/xhtml+xml\r\n";
    int rc = http_request("GET", "https://www.zhihu.com/", hdr, NULL, &r, NULL);
    if (rc != 0) {
        errf("%s", r.err[0] ? r.err : "无法连接知乎");
        http_resp_free(&r);
        return -1;
    }
    absorb_set_cookie(&r);
    http_resp_free(&r);

    if (!strstr(g_cookie, "d_c0=")) {
        errf("未能获取 d_c0 cookie（知乎可能调整了首页），可手动导入 Cookie");
        return -1;
    }
    api_save_cookie();
    g_warmed = 1;
    return 0;
}

/* ---------------- 列表解析 ---------------- */

void api_list_free(ApiList *l) {
    if (!l) return;
    free(l->v);
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

static ApiItem *list_push(ApiList *l) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = xrealloc(l->v, sizeof(ApiItem) * (size_t)l->cap);
    }
    ApiItem *it = &l->v[l->n++];
    memset(it, 0, sizeof(*it));
    it->type = 1;
    return it;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    /* 控制字符替换成空格，避免渲染异常 */
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[j++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
    }
    dst[j] = 0;
}

/* 从 URL 里抽 question id / answer id / article id。
 *
 * 1.2.4 的版本有三个坑，都会导致"标题能看到、点进去 404"：
 *   1. 不剥查询串/锚点：`/p/123?utm_source=..` 之后的内容会被当成 id 的一部分；
 *   2. 认不出 `/question/123/answer/456` 这种最完整的形态；
 *   3. 抽不到 id 时**仍然改 type**，于是"类型=问题、qid 为空"，
 *      调用方只好去请求 `/api/v4/answers/`（空 id）→ 404（响应体还常是空的）。
 * 现在：先剥干净，再按"越具体越优先"匹配，抽不到就什么都不改。 */
static void digits_after(const char *p, char *out, size_t outsz) {
    size_t i = 0;
    while (p && *p >= '0' && *p <= '9' && i + 1 < outsz) out[i++] = *p++;
    if (outsz) out[i] = 0;
}

static void ids_from_url(const char *url, char *qid, size_t qsz, char *aid, size_t asz, int *type) {
    if (qid && qsz) qid[0] = 0;
    if (aid && asz) aid[0] = 0;
    if (!url || !*url) return;

    char clean[512];
    snprintf(clean, sizeof(clean), "%s", url);
    char *cut = strpbrk(clean, "?#");
    if (cut) *cut = 0;

    const char *a = strstr(clean, "/answer/");
    const char *q = strstr(clean, "/question/");
    const char *p = strstr(clean, "/p/");

    if (a) {
        digits_after(a + 8, aid, asz);
        if (type) *type = 1;
    }
    if (q) {
        char tmp[32];
        digits_after(q + 10, tmp, sizeof(tmp));
        if (tmp[0]) { if (qid) snprintf(qid, qsz, "%s", tmp); if (type && !a) *type = 3; }
    }
    if (p) {
        char tmp[32];
        digits_after(p + 3, tmp, sizeof(tmp));
        if (tmp[0]) {
            if (aid) snprintf(aid, asz, "%s", tmp);
            if (qid) qid[0] = 0;
            if (type) *type = 2;
        }
    }
    /* 纯数字（有些字段直接给 id） */
    if (!(a && aid && aid[0]) && !(q && qid && qid[0]) && !(p && aid && aid[0])) {
        int all = clean[0] ? 1 : 0;
        for (const char *t = clean; *t; t++)
            if (*t < '0' || *t > '9') { all = 0; break; }
        if (all && aid && asz) { snprintf(aid, asz, "%s", clean); if (type && *type == 0) *type = 1; }
    }
}

static long jnum(JVal *v, const char *k) { return json_get_int(v, k, 0); }

/* 把一段 HTML 压成纯文本，最多保留 max_chars 个字符（按 UTF-8 字符计）。
 * 用途：回答没有独立标题时，用正文开头当列表标题——比重复问题标题有用得多。 */
static void html_plain(const char *html, char *out, size_t outsz, int max_chars) {
    out[0] = 0;
    if (!html) return;
    size_t j = 0;
    int n = 0;
    for (const char *p = html; *p && n < max_chars && j + 4 < outsz; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p <= 10) { p = semi + 1; continue; }
        }
        if ((unsigned char)*p < 0x20) { p++; continue; }
        int len = u8_seqlen((unsigned char)*p);
        if (len <= 0) len = 1;
        memcpy(out + j, p, (size_t)len);
        j += (size_t)len;
        p += len;
        n++;
    }
    out[j] = 0;
    /* 去掉首尾空白 */
    char *t = str_trim(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
}

/* 把一个 feed 条目解析成 ApiItem；返回 NULL 表示跳过 */
static int parse_one(JVal *item, ApiItem *it) {
    JVal *t = json_get(item, "target");
    if (!t) t = json_get(item, "object");
    /* 收藏夹内容：每一项是 {"content": {type,id,question,author,...}}，
     * 真正的内容对象包在 content 里。注意回答对象的 content 是 HTML 字符串，
     * 只有对象类型才这么取。 */
    if (!t) {
        JVal *c = json_get(item, "content");
        if (c && json_type(c) == JOBJ) t = c;
    }
    if (!t) t = item;

    const char *typestr = json_get_str(t, "type", NULL);
    if (!typestr) typestr = json_get_str(item, "type", NULL);

    memset(it, 0, sizeof(*it));
    it->type = 1;
    if (typestr) {
        if (strcmp(typestr, "article") == 0) it->type = 2;
        else if (strcmp(typestr, "question") == 0) it->type = 3;
        else if (strcmp(typestr, "answer") == 0) it->type = 1;
    }

    /* 1.2.12：摘要独立成 summary 字段（excerpt，最多 140 字）。
     * 详细模式的列表布局是"问题标题 → 回答摘要 → 作者赞同"，
     * 摘要不再挤在标题位上。 */
    char sumbuf[1024];
    sumbuf[0] = 0;
    {
        const char *ex = json_get_str(t, "excerpt", NULL);
        if (!ex) {
            JVal *ea = json_get(t, "excerpt_area");
            if (ea) ex = json_get_str(ea, "text", NULL);
        }
        if (!ex) ex = json_get_str(t, "content", NULL);   /* 回答的正文 HTML */
        if (ex && *ex) html_plain(ex, sumbuf, sizeof(sumbuf), 140);
    }
    copy_str(it->summary, sizeof(it->summary), sumbuf);

    /* 标题优先级（1.2.12 调整）：条目自己的 title > 问题标题 > 摘要。
     * 之前回答条目把 excerpt 当标题（1.2.5 为了区分同一问题下的多条回答），
     * 用户反馈"先看到回答内容再放标题，阅读逻辑不顺"——现在以问题优先，
     * 区分同问题的多条回答交给详细模式的摘要预览。 */
    const char *title = json_get_str(t, "title", NULL);
    if (!title) {
        JVal *ta = json_get(t, "title_area");
        if (ta) title = json_get_str(ta, "text", NULL);
    }
    if (!title || !*title) {
        JVal *q = json_get(t, "question");
        if (q) title = json_get_str(q, "title", NULL);
    }
    if (!title || !*title && sumbuf[0]) title = sumbuf;   /* 兜底：连问题标题都没有 */
    if (!title) {
        JVal *target2 = json_get(item, "highlight");
        if (target2) title = json_get_str(target2, "title", NULL);
    }
    if (!title) title = "";
    copy_str(it->title, sizeof(it->title), title);

    /* id：必须按原始字面量取。
     * 知乎的 id 是 19 位（> 2^53），用 double/long 转一次就会变样，
     * 请求出去就是一个不存在的 id → 404。见 json_get_idstr 的注释。 */
    json_get_idstr(t, "id", it->id, sizeof(it->id));
    JVal *q = json_get(t, "question");
    if (q) json_get_idstr(q, "id", it->qid, sizeof(it->qid));
    else if (it->type == 3) json_get_idstr(t, "id", it->qid, sizeof(it->qid));
    /* 热榜条目的 id 只存在于 link.url */
    JVal *link = json_get(t, "link");
    if (link) {
        const char *url = json_get_str(link, "url", NULL);
        int t2 = it->type;
        char qid2[64] = {0}, aid2[64] = {0};
        ids_from_url(url, qid2, sizeof(qid2), aid2, sizeof(aid2), &t2);
        if (aid2[0]) snprintf(it->id, sizeof(it->id), "%s", aid2);
        if (qid2[0]) snprintf(it->qid, sizeof(it->qid), "%s", qid2);
        /* 只有真的抽到 id 才改类型。否则会出现"type=问题、qid 空"，
         * 调用方误以为这是问题页，最后请求到 /answers/ 这种空 id → 404。 */
        if (aid2[0] || qid2[0]) it->type = t2;
        /* 记下原始链接：404 的时候把它打进日志，一眼能看出是哪一类 URL */
        const char *u = url ? url : "";
        snprintf(it->url, sizeof(it->url), "%s", u);
    }

    /* 作者 */
    const char *author = NULL;
    JVal *a = json_get(t, "author");
    if (a) author = json_get_str(a, "name", NULL);
    if (!author) {
        JVal *hi = json_get(item, "highlight");
        if (hi) {
            JVal *ha = json_get(hi, "author");
            if (ha) author = json_get_str(ha, "name", NULL);
        }
    }

    it->voteup = jnum(t, "voteup_count");
    it->comments = jnum(t, "comment_count");
    it->answers = jnum(t, "answer_count");
    /* 收藏夹列表里没有 answer_count，条数在 item_count 上。
     * 单位不一样（"个回答" / "条内容"），单独记一下，别把收藏夹写成"3 个回答"。 */
    int is_coll = 0;
    if (it->answers == 0) {
        long ic = jnum(t, "item_count");
        if (ic == 0) ic = jnum(t, "items_count");
        if (ic > 0) { it->answers = ic; is_coll = 1; }
    }

    /* 副标题 */
    char sub[400] = {0};
    if (author && *author) snprintf(sub, sizeof(sub), "%s", author);
    if (it->answers > 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s%d %s", sub[0] ? " · " : "", (int)it->answers,
                 is_coll ? "条内容" : "个回答");
        strncat(sub, tmp, sizeof(sub) - strlen(sub) - 1);
    }
    if (it->voteup > 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s%d 赞同", sub[0] ? " · " : "", (int)it->voteup);
        strncat(sub, tmp, sizeof(sub) - strlen(sub) - 1);
    }
    if (!sub[0]) {
        JVal *ma = json_get(t, "metrics_area");
        if (ma) {
            const char *mt = json_get_str(ma, "text", NULL);
            if (mt) snprintf(sub, sizeof(sub), "%s", mt);
        }
    }
    if (!sub[0]) {
        JVal *ea = json_get(t, "excerpt_area");
        if (ea) {
            const char *et = json_get_str(ea, "text", NULL);
            if (et) copy_str(sub, sizeof(sub), et);
        }
    }
    if (!sub[0]) {
        const char *ex = json_get_str(t, "excerpt", NULL);
        if (!ex) ex = json_get_str(item, "highlight_description", NULL);
        if (ex) copy_str(sub, sizeof(sub), ex);
    }
    copy_str(it->sub, sizeof(it->sub), sub);

    return it->title[0] ? 1 : 0;
}

static int parse_feed(const char *json, size_t len, ApiList *out) {
    JVal *root = json_parse(json, len);
    if (!root) { errf("响应不是合法 JSON"); return -1; }

    /* 知乎的风控不是每次都返回 403：有时是 HTTP 200 + {"error":{code,message}}，
     * 有时干脆是 200 + {"data":[]}。这两种情况旧代码都只让列表变空，
     * 界面上就只剩一个「（空）」，用户根本分不清是没网、被风控还是没登录。
     * 所以这里必须把原因写进 g_err，交给 UI 显示出来。 */
    JVal *error = json_get(root, "error");
    if (error && json_type(error) == JOBJ) {
        const char *msg = json_get_str(error, "message", NULL);
        long code = json_get_int(error, "code", 0);
        if (msg && *msg) errf("知乎返回错误（%ld）：%s", code, msg);
        else errf("知乎返回错误（%ld），多为风控或需要登录", code);
        json_free(root);
        return -1;
    }

    JVal *data = json_get(root, "data");
    if (!data) {
        errf("接口未返回 data 字段，知乎可能调整了接口");
        json_free(root);
        return -1;
    }
    if (json_type(data) != JARR && json_type(data) != JOBJ) {
        errf("接口返回结构异常（data 不是列表），知乎可能调整了接口");
        json_free(root);
        return -1;
    }

    g_next_url[0] = 0;
    JVal *paging = json_get(root, "paging");
    if (paging && json_type(paging) == JOBJ) {
        const char *nx = json_get_str(paging, "next", NULL);
        if (nx && *nx) snprintf(g_next_url, sizeof(g_next_url), "%s", nx);
    }

    int n = json_count(data);
    for (int i = 0; i < n; i++) {
        ApiItem tmp;
        if (!parse_one(json_at(data, i), &tmp)) continue;
        /* 去重 */
        int dup = 0;
        for (int k = 0; k < out->n; k++) {
            if (out->v[k].type == tmp.type && strcmp(out->v[k].id, tmp.id) == 0 && tmp.id[0]) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        ApiItem *slot = list_push(out);
        *slot = tmp;
    }
    json_free(root);

    /* 200 且 data 是数组，但一条都没解析出来 —— 同样是「静默失败」的常见形态：
     * 结构变了、或者返回了空数组。留一句可读的诊断，UI 空态直接用。 */
    if (out->n == 0) {
        if (n == 0) errf("接口返回 0 条（多为风控或登录态失效）");
        else errf("返回了 %d 条但都无法解析，知乎结构可能变了", n);
    }
    return 0;
}

/* ---------------- 具体接口 ---------------- */

static void url_encode(const char *s, Buf *out) {
    static const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') buf_addc(out, (char)*p);
        else {
            buf_addc(out, '%');
            buf_addc(out, hex[*p >> 4]);
            buf_addc(out, hex[*p & 15]);
        }
    }
}

int api_hot(ApiList *out, int limit) {
    char path[256];
    /* 注意：hot-lists/total 现在对游客返回 401，网页版实际走的是 hot-list-web */
    snprintf(path, sizeof(path),
             "/api/v3/feed/topstory/hot-list-web?limit=%d&desktop=true", limit);
    Buf b;
    if (api_call("GET", path, NULL, &b) != 0) return -1;
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

int api_recommend(ApiList *out, int page, int limit) {
    char path[320];
    if (limit <= 0) limit = 20;
    snprintf(path, sizeof(path),
             "/api/v3/feed/topstory/recommend?desktop=true&page_number=%d&limit=%d&action=down",
             page, limit);
    Buf b;
    if (api_call("GET", path, NULL, &b) != 0) return -1;
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

int api_search(const char *q, ApiList *out, const char *tab) {
    Buf enc;
    buf_init(&enc);
    url_encode(q, &enc);
    const char *t = (tab && *tab) ? tab : "general";
    char path[1200];
    snprintf(path, sizeof(path),
             "/api/v4/search_v3?t=%s&q=%s&correction=1&offset=0&limit=20", t, buf_cstr(&enc));
    buf_free(&enc);

    Buf b;
    if (api_call("GET", path, NULL, &b) != 0) return -1;
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

int api_question_answers(const char *qid, ApiList *out, int offset, int limit) {
    char path[640];
    snprintf(path, sizeof(path),
             "/api/v4/questions/%s/answers?include=data[*].content,voteup_count,comment_count,"
             "author.name,author.headline,excerpt&limit=%d&offset=%d&order_by=default",
             qid, limit, offset);
    Buf b;
    if (api_call("GET", path, NULL, &b) != 0) return -1;
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

static int parse_detail(const char *json, size_t len, ApiContent *out) {
    JVal *root = json_parse(json, len);
    if (!root) { errf("响应不是合法 JSON"); return -1; }
    JVal *e = json_get(root, "error");
    if (e && json_type(e) == JOBJ) {
        const char *m = json_get_str(e, "message", NULL);
        long c = json_get_int(e, "code", 0);
        errf("知乎返回错误（%ld）：%s", c, m ? m : "无说明");
        json_free(root);
        return -1;
    }
    const char *title = json_get_str(root, "title", NULL);
    if (!title) {
        JVal *q = json_get(root, "question");
        if (q) title = json_get_str(q, "title", NULL);
    }
    const char *html = json_get_str(root, "content", NULL);
    if (!html) html = json_get_str(root, "excerpt", NULL);
    const char *author = NULL;
    JVal *a = json_get(root, "author");
    if (a) author = json_get_str(a, "name", NULL);

    out->title = xstrdup(title ? title : "");
    out->author = xstrdup(author ? author : "");
    out->html = xstrdup(html ? html : "");
    out->voteup = json_get_int(root, "voteup_count", 0);
    out->comments = json_get_int(root, "comment_count", 0);
    /* 回答是 created_time（编辑过还有 updated_time）；专栏文章是 created/published_time。
     * 详情页头部要显示"发布于"，所以这几个键都要试。 */
    out->created = json_get_int(root, "created_time", 0);
    if (out->created <= 0) out->created = json_get_int(root, "created", 0);
    if (out->created <= 0) out->created = json_get_int(root, "published_time", 0);
    if (out->created <= 0) out->created = json_get_int(root, "updated_time", 0);
    json_free(root);
    if (!out->html[0]) { errf("正文为空（可能需要在知乎 App 内查看）"); return -1; }
    return 0;
}

/* 详情接口的备用路径：从"问题下的回答列表"里按 id 捞。
 * 直接 /api/v4/answers/{id} 404 时，走这条常常还能拿到正文——
 * 列表接口的审核比详情接口松。 */
static int pick_answer_from_list(const char *json, size_t len, const char *aid, ApiContent *out) {
    JVal *root = json_parse(json, len);
    if (!root) { errf("响应不是合法 JSON"); return -1; }
    JVal *data = json_get(root, "data");
    int found = -1;
    for (int i = 0; i < json_count(data); i++) {
        JVal *it = json_at(data, i);
        if (!it) continue;
        char ids[64];
        if (json_get_idstr(it, "id", ids, sizeof(ids)) != 0) continue;
        if (strcmp(ids, aid) != 0) continue;
        const char *html = json_get_str(it, "content", NULL);
        if (!html) html = json_get_str(it, "excerpt", NULL);
        const char *title = NULL;
        JVal *q = json_get(it, "question");
        if (q) title = json_get_str(q, "title", NULL);
        const char *author = NULL;
        JVal *a = json_get(it, "author");
        if (a) author = json_get_str(a, "name", NULL);
        out->title = xstrdup(title ? title : "");
        out->author = xstrdup(author ? author : "");
        out->html = xstrdup(html ? html : "");
        out->voteup = json_get_int(it, "voteup_count", 0);
        out->comments = json_get_int(it, "comment_count", 0);
        out->created = json_get_int(it, "created_time", 0);
        if (out->created <= 0) out->created = json_get_int(it, "created", 0);
        found = out->html[0] ? 0 : -1;
        break;
    }
    json_free(root);
    if (found != 0) {
        errf("回答列表里没有 id=%s（可能已被删除或折叠）", aid);
        return -1;
    }
    return 0;
}

int api_answer_detail2(const char *aid, const char *qid, ApiContent *out) {
    memset(out, 0, sizeof(*out));
    if (!aid || !*aid) { errf("条目 id 缺失（链接形态未识别）"); return -1; }

    char path[640], ref[512];
    snprintf(path, sizeof(path),
             "/api/v4/answers/%s?include=content,voteup_count,comment_count,created_time,"
             "question.title,author.name,excerpt", aid);
    if (qid && *qid) snprintf(ref, sizeof(ref), "https://www.zhihu.com/question/%s", qid);
    else snprintf(ref, sizeof(ref), "https://www.zhihu.com/");

    Buf b;
    buf_init(&b);
    int rc = api_call_ref("GET", path, NULL, ref, &b);
    if (rc == 0) {
        rc = parse_detail(b.p, b.len, out);
        buf_free(&b);
        if (rc == 0) return 0;
    } else {
        buf_free(&b);
    }
    int st = api_last_status();
    logmsg("详情: /answers/%s 失败 status=%d err=%s → 试问题回答列表", aid, st, api_error());

    if (!qid || !*qid) return -1;
    snprintf(path, sizeof(path),
             "/api/v4/questions/%s/answers?include=data[*].content,voteup_count,comment_count,"
             "author.name&limit=20&offset=0&order_by=default", qid);
    snprintf(ref, sizeof(ref), "https://www.zhihu.com/question/%s", qid);
    buf_init(&b);
    if (api_call_ref("GET", path, NULL, ref, &b) != 0) { buf_free(&b); return -1; }
    rc = pick_answer_from_list(b.p, b.len, aid, out);
    buf_free(&b);
    if (rc == 0) logmsg("详情: 从回答列表里取到 /answers/%s 的正文（备用路径成功）", aid);
    return rc;
}

int api_answer_detail(const char *aid, ApiContent *out) {
    return api_answer_detail2(aid, NULL, out);
}

int api_article_detail2(const char *aid, ApiContent *out) {
    memset(out, 0, sizeof(*out));
    if (!aid || !*aid) { errf("条目 id 缺失（链接形态未识别）"); return -1; }
    char path[640], ref[512];
    snprintf(path, sizeof(path),
             "/api/v4/articles/%s?include=content,voteup_count,comment_count,created_time,"
             "title,author.name", aid);
    snprintf(ref, sizeof(ref), "https://zhuanlan.zhihu.com/p/%s", aid);
    Buf b;
    buf_init(&b);
    if (api_call_ref("GET", path, NULL, ref, &b) != 0) { buf_free(&b); return -1; }
    int rc = parse_detail(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

int api_article_detail(const char *aid, ApiContent *out) {
    return api_article_detail2(aid, out);
}

/* 收藏夹列表。
 * 1.2.6 用的 /api/v4/collections/self 在真机上恒返回 404「内容不存在」——
 * 这个路由早就没了，跟登录态无关。实测仍然有效的两个是
 *   /api/v4/members/self/favlists       （网页端「我的收藏」在用）
 *   /api/v4/people/self/collections     （老版 web，登录后可用）
 * 两个都试，谁先返回条目就用谁。 */
int api_my_collections(ApiList *out) {
    static const char *cands[] = {
        "/api/v4/members/self/favlists?offset=0&limit=20&include=data[*].item_count,description,is_public",
        "/api/v4/people/self/collections?offset=0&limit=20&include=data[*].item_count,description,is_public",
    };
    char last[256] = "";
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        Buf b;
        buf_init(&b);
        if (api_call("GET", cands[i], NULL, &b) != 0) {
            logmsg("收藏: %s 失败 status=%d %s", cands[i], api_last_status(), api_error());
            snprintf(last, sizeof(last), "%s", api_error());
            buf_free(&b);
            continue;
        }
        int rc = parse_feed(b.p, b.len, out);
        int got = out->n;
        buf_free(&b);
        if (rc == 0 && got > 0) {
            logmsg("收藏: %s 成功，%d 个收藏夹", cands[i], got);
            return 0;
        }
        logmsg("收藏: %s 返回 %d 个（status=%d）", cands[i], got, api_last_status());
        if (got == 0) snprintf(last, sizeof(last), "%s", api_error());
        api_list_free(out);
        memset(out, 0, sizeof(*out));
    }
    api_set_error("%s", last[0] ? last : "没有读取到收藏夹，请确认已登录");
    return -1;
}

int api_collection_items(const char *cid, ApiList *out, int offset) {
    char path[400];
    snprintf(path, sizeof(path),
             "/api/v4/collections/%s/items?offset=%d&limit=20&include=data[*].content,title,"
             "voteup_count,author.name", cid, offset);
    Buf b;
    if (api_call("GET", path, NULL, &b) != 0) return -1;
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

/* 按 paging.next 继续拉推荐流。next 是绝对地址，取路径部分复用签名逻辑。 */
int api_recommend_next(ApiList *out, const char *abs) {
    if (!abs || !*abs) { errf("没有下一页地址"); return -1; }
    const char *p = abs;
    const char *scheme = strstr(abs, "://");
    if (scheme) {
        p = strchr(scheme + 3, '/');
        if (!p) { errf("下一页地址无法解析"); return -1; }
    }
    Buf b;
    buf_init(&b);
    if (api_fetch_raw(p, &b) != 0) { buf_free(&b); return -1; }
    int rc = parse_feed(b.p, b.len, out);
    buf_free(&b);
    return rc;
}

/* ---------------- 供 UI 使用的底层入口 ---------------- */

int api_fetch_raw(const char *path, Buf *out) {
    if (api_warmup() != 0) return -1;
    return api_call("GET", path, NULL, out);
}

int api_parse_feed(const char *json, size_t len, ApiList *out) {
    return parse_feed(json, len, out);
}

int api_parse_detail(const char *json, size_t len, ApiContent *out) {
    memset(out, 0, sizeof(*out));
    return parse_detail(json, len, out);
}


/* ---------------- 收藏 / 时间 / 图片 / 列表落盘 ---------------- */

void api_format_date(long ts, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = 0;
    if (ts <= 0) return;
    time_t t = (time_t)ts;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return;
    strftime(out, outsz, "%Y-%m-%d", &tmv);
}

/* 「全部收藏」：先列收藏夹，再逐个拉内容合并。
 * 之所以放在子进程里跑：收藏夹多的时候要发十几个请求，
 * 父进程必须还能响应点击、还能取消。 */
int api_all_collections(ApiList *out, int max_cols) {
    ApiList cols;
    memset(&cols, 0, sizeof(cols));
    if (api_my_collections(&cols) != 0) {
        api_list_free(&cols);
        return -1;
    }
    if (cols.n == 0) {
        api_list_free(&cols);
        errf("没有收藏夹（或接口返回空）");
        return -1;
    }
    int lim = cols.n < max_cols ? cols.n : max_cols;
    int got = 0;
    for (int i = 0; i < lim; i++) {
        if (!cols.v[i].id[0]) continue;
        int before = out->n;
        api_collection_items(cols.v[i].id, out, 0);
        got += out->n - before;
        logmsg("收藏: 夹[%d] %s 取到 %d 条（累计 %d）", i, cols.v[i].title, out->n - before, out->n);
    }
    api_list_free(&cols);
    if (out->n == 0) {
        errf(got == 0 ? "收藏夹里没有内容" : "收藏夹内容读取失败");
        return -1;
    }
    return 0;
}

/* 单次图片请求。use_cookie=0 时不带 Cookie：有些 CDN 节点对跨域 Cookie
 * 反而回 403，带不带要都试一遍。 */
static int fetch_bin_once(const char *url, const char *ref, int use_cookie, const char *ua,
                          Buf *out, int *status, char *err, size_t errsz) {
    char ua_prev[256];
    snprintf(ua_prev, sizeof(ua_prev), "%s", http_user_agent());
    http_set_user_agent(ua);

    Buf hdr;
    buf_init(&hdr);
    buf_printf(&hdr, "Accept: image/jpeg,image/png,image/*;q=0.8,*/*;q=0.5\r\n"
                     "Referer: %s\r\n", ref);
    if (use_cookie && g_cookie[0]) buf_printf(&hdr, "Cookie: %s\r\n", g_cookie);

    HttpResp r;
    http_resp_init(&r);
    int rc = http_request("GET", url, buf_cstr(&hdr), NULL, &r, NULL);
    buf_free(&hdr);
    http_set_user_agent(ua_prev);

    *status = (rc == 0) ? r.status : 0;
    if (rc != 0) {
        snprintf(err, errsz, "%s", r.err[0] ? r.err : "网络失败");
        http_resp_free(&r);
        return -1;
    }
    if (r.status != 200 || r.body.len < 64) {
        snprintf(err, errsz, "HTTP %d（%zu 字节）", r.status, r.body.len);
        http_resp_free(&r);
        return -1;
    }
    buf_init(out);
    buf_add(out, r.body.p, r.body.len);
    http_resp_free(&r);
    return 0;
}

/* 图片 CDN 取图。
 *
 * 1.2.6 只用一种写法（https + Cookie + 桌面 UA），真机上一张都下不下来，
 * 而日志里只有一行"图片 下载失败"，没法定位是哪一环。现在按"最可能成功 →
 * 最不可能"的顺序逐级降级，每一级都把 URL 和状态码写进日志：
 *   1) 原地址 + Cookie + 桌面 UA
 *   2) 原地址，不带 Cookie（CDN 对跨域 Cookie 可能直接 403）
 *   3) 原地址，移动端 UA（知乎图片对移动 UA 更宽松）
 *   4) 换成 .jpg / _1440w.jpg（CDN 支持按后缀转码，PNG/WebP 也能拿到 JPEG）
 * 全部失败时把最后一次的真实原因（HTTP 码或 TLS 错误）留在 api_error() 里。 */
int api_fetch_bin(const char *url, const char *referer, Buf *out) {
    if (!url || !*url) { errf("图片地址为空"); return -1; }
    const char *ref = (referer && *referer) ? referer : "https://www.zhihu.com/";

    char err[160] = "";
    int status = 0;

    if (fetch_bin_once(url, ref, 1, UA_PC, out, &status, err, sizeof(err)) == 0) {
        logmsg("api: 图片 ok %zu 字节 %s", out->len, url);
        return 0;
    }
    logmsg("api: 图片[1/4] 带 Cookie 失败 status=%d %s（%s）", status, err, url);

    if (fetch_bin_once(url, ref, 0, UA_PC, out, &status, err, sizeof(err)) == 0) {
        logmsg("api: 图片 ok（不带 Cookie）%zu 字节 %s", out->len, url);
        return 0;
    }
    logmsg("api: 图片[2/4] 不带 Cookie 失败 status=%d %s（%s）", status, err, url);

    if (fetch_bin_once(url, ref, 0, NULL, out, &status, err, sizeof(err)) == 0) {
        logmsg("api: 图片 ok（移动 UA）%zu 字节 %s", out->len, url);
        return 0;
    }
    logmsg("api: 图片[3/4] 移动 UA 失败 status=%d %s（%s）", status, err, url);

    /* 后缀换 jpg：v2-xxxx.png / .webp 这类源图，CDN 能按后缀转成 JPEG */
    const char *dot = strrchr(url, '.');
    const char *slash = strrchr(url, '/');
    const char *q = strchr(url, '?');
    if (dot && slash && dot > slash && (!q || dot < q)) {
        char base[1024];
        size_t n = (size_t)(dot - url);
        if (n < sizeof(base)) {
            memcpy(base, url, n);
            base[n] = 0;
            char alt[1100];
            const char *suffixes[] = { ".jpg", "_1440w.jpg" };
            for (int k = 0; k < 2; k++) {
                snprintf(alt, sizeof(alt), "%s%s", base, suffixes[k]);
                if (strcmp(alt, url) == 0) continue;
                if (fetch_bin_once(alt, ref, 0, UA_PC, out, &status, err, sizeof(err)) == 0) {
                    logmsg("api: 图片 ok（改后缀）%zu 字节 %s", out->len, alt);
                    return 0;
                }
                logmsg("api: 图片[4/4] 改后缀失败 status=%d %s（%s）", status, err, alt);
            }
        }
    }

    errf("%s", err[0] ? err : "图片下载失败");
    return -1;
}

/* ApiList 落盘：count + 定长数组。父子进程同架构，直接 fwrite。 */
int api_list_save(const char *path, const ApiList *l) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int cnt = (l && l->n > 0) ? l->n : 0;
    fwrite(&cnt, sizeof(cnt), 1, f);
    if (cnt > 0) fwrite(l->v, sizeof(ApiItem), (size_t)cnt, f);
    fclose(f);
    return 0;
}

int api_list_load(const char *path, ApiList *l) {
    memset(l, 0, sizeof(*l));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int cnt = 0;
    if (fread(&cnt, sizeof(cnt), 1, f) != 1 || cnt < 0 || cnt > 5000) { fclose(f); return -1; }
    if (cnt == 0) { fclose(f); return 0; }
    l->v = xcalloc((size_t)cnt, sizeof(ApiItem));
    l->cap = l->n = cnt;
    if (fread(l->v, sizeof(ApiItem), (size_t)cnt, f) != (size_t)cnt) {
        fclose(f);
        api_list_free(l);
        return -1;
    }
    fclose(f);
    return 0;
}

/* 自检：URL -> id 抽取。1.2.4 的 404 有一半是这里抽错/抽空造成的，
 * 所以把真机上见过的几种链接形态钉死在自检里。 */
int api_ids_selftest(void) {
    struct { const char *url; int type; const char *qid; const char *aid; } cases[] = {
        { "https://www.zhihu.com/question/123456",                       3, "123456", "" },
        { "https://www.zhihu.com/question/123456?utm_source=wechat",      3, "123456", "" },
        { "https://www.zhihu.com/question/123456/answer/789",             1, "123456", "789" },
        { "https://www.zhihu.com/answer/789",                             1, "",       "789" },
        { "https://zhuanlan.zhihu.com/p/456789",                          2, "",       "456789" },
        { "https://zhuanlan.zhihu.com/p/456789#comments",                 2, "",       "456789" },
        { "https://www.zhihu.com/question/",                              0, "",       "" },
        { "123456",                                                       1, "",       "123456" },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char qid[64] = {0}, aid[64] = {0};
        int type = 0;
        ids_from_url(cases[i].url, qid, sizeof(qid), aid, sizeof(aid), &type);
        int good = strcmp(qid, cases[i].qid) == 0 && strcmp(aid, cases[i].aid) == 0;
        /* type=0 表示"什么都没抽到"，这种情况不比对类型 */
        if (cases[i].qid[0] || cases[i].aid[0]) good = good && (type == cases[i].type);
        else good = good && (type == 0 || type == cases[i].type);
        if (!good) {
            ok = 0;
            logmsg("selfcheck: ids 失败 url=%s 得到 type=%d qid=[%s] aid=[%s]，期望 type=%d qid=[%s] aid=[%s]",
                   cases[i].url, type, qid, aid, cases[i].type, cases[i].qid, cases[i].aid);
        }
    }
    return ok;
}

/* 清空 <home>/cache（含图片缓存的子目录）。返回删除的文件数。
 * 1.2.9 之前 remove() 对目录无效，图片按文章分目录后"清空缓存"
 * 会漏掉整个 cache/img/<文章>/ 子树——必须递归删。 */
static long cache_rm_rec(const char *dir) {
    long n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            n += cache_rm_rec(p);
            rmdir(p);          /* 子目录清空后把夹子也删掉 */
        } else if (remove(p) == 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

long api_cache_clear(void) {
    char dir[320];
    snprintf(dir, sizeof(dir), "%s/cache", g_home);
    return cache_rm_rec(dir);
}

void api_content_free(ApiContent *c) {
    if (!c) return;
    free(c->title);
    free(c->author);
    free(c->html);
    memset(c, 0, sizeof(*c));
}
