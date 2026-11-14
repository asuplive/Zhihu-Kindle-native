/* account.c —— 设备端登录：二维码扫码 + Cookie 导入
 *
 * 独立维护一份 cookie jar，因为登录链路要连续累积 _xsrf / _zap / d_c0 / z_c0，
 * 而主 api.c 的 cookie 只关心 d_c0 与已导入的登录态。登录成功后把整串回写。
 */
#include "account.h"
#include "api.h"
#include "common.h"
#include "json.h"
#include "net.h"
#include "zse96.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char a_cookie[4096];
static char a_err[256];

const char *account_cookie(void) { return a_cookie; }
const char *account_session_get(void) { return a_cookie; }
void account_session_set(const char *cookie) {
    snprintf(a_cookie, sizeof(a_cookie), "%s", cookie ? cookie : "");
}

static void a_errf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a_err, sizeof(a_err), fmt, ap);
    va_end(ap);
}

static void a_put(const char *name, const char *val) {
    if (!name || !*name || !val) return;
    char pat[96];
    snprintf(pat, sizeof(pat), "%s=", name);
    char *p = strstr(a_cookie, pat);
    while (p && p != a_cookie && p[-1] != ' ' && p[-1] != ';')
        p = strstr(p + 1, pat);
    if (p) {
        char *end = strchr(p, ';');
        Buf b;
        buf_init(&b);
        buf_add(&b, a_cookie, (size_t)(p - a_cookie));
        buf_printf(&b, "%s%s", pat, val);
        if (end) buf_adds(&b, end);
        snprintf(a_cookie, sizeof(a_cookie), "%s", buf_cstr(&b));
        buf_free(&b);
    } else {
        if (a_cookie[0]) strncat(a_cookie, "; ", sizeof(a_cookie) - strlen(a_cookie) - 1);
        strncat(a_cookie, pat, sizeof(a_cookie) - strlen(a_cookie) - 1);
        strncat(a_cookie, val, sizeof(a_cookie) - strlen(a_cookie) - 1);
    }
}

static int a_val(const char *name, char *out, size_t n) {
    out[0] = 0;
    char pat[96];
    snprintf(pat, sizeof(pat), "%s=", name);
    const char *p = strstr(a_cookie, pat);
    while (p && p != a_cookie && p[-1] != ' ' && p[-1] != ';')
        p = strstr(p + 1, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *end = strchr(p, ';');
    size_t l = end ? (size_t)(end - p) : strlen(p);
    if (l >= n) l = n - 1;
    memcpy(out, p, l);
    out[l] = 0;
    return 0;
}

static void a_absorb(HttpResp *r) {
    if (!r->set_cookie.len) return;
    buf_cstr(&r->set_cookie);
    const char *p = r->set_cookie.p;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 2) {
            const char *eq = memchr(p, '=', len);
            if (eq) {
                char name[64], val[512];
                size_t nl2 = (size_t)(eq - p);
                size_t vl = len - nl2 - 1;
                if (nl2 < sizeof(name) && vl < sizeof(val)) {
                    memcpy(name, p, nl2); name[nl2] = 0;
                    memcpy(val, eq + 1, vl); val[vl] = 0;
                    a_put(str_trim(name), str_trim(val));
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
}

/* 发一次请求。返回 0 表示拿到 HTTP 响应（status 有效），-1 表示网络层失败。
 * 想要的头由调用方给；这里统一补签名、Referer、Cookie，并吸收 Set-Cookie。 */
static int a_req(const char *method, const char *path, const char *extra,
                 const char *body, HttpResp *r) {
    char url[1400];
    snprintf(url, sizeof(url), "https://www.zhihu.com%s", path);

    char dc0[256];
    if (a_val("d_c0", dc0, sizeof(dc0)) != 0) dc0[0] = 0;
    char sig[128];
    zse96_sign(url, dc0, body, sig, sizeof(sig));

    char hdr[4096];
    int n = snprintf(hdr, sizeof(hdr),
                     "x-zse-93: %s\r\nx-zse-96: %s\r\nx-requested-with: fetch\r\n"
                     "Referer: https://www.zhihu.com/signin\r\n",
                     ZSE93_VERSION, sig);
    if (a_cookie[0]) n += snprintf(hdr + n, sizeof(hdr) - (size_t)n, "Cookie: %s\r\n", a_cookie);
    if (extra) snprintf(hdr + n, sizeof(hdr) - (size_t)n, "%s", extra);
    if (body && !strstr(hdr, "Content-Type:"))
        snprintf(hdr + strlen(hdr), sizeof(hdr) - strlen(hdr),
                 "Content-Type: application/x-www-form-urlencoded\r\n");

    http_resp_init(r);
    int rc = http_request(method, url, hdr, body, r, NULL);
    if (rc == 0) {
        a_absorb(r);
        buf_cstr(&r->body);
    }
    return rc;
}

static int a_json_str(const char *body, const char *key, char *out, size_t n) {
    JVal *v = json_parse(body, strlen(body));
    if (!v) return -1;
    const char *s = json_get_str(v, key, NULL);
    if (s) snprintf(out, n, "%s", s);
    json_free(v);
    return s ? 0 : -1;
}

int account_has_login(void) {
    /* 既看主 api.c 的 cookie，也看本模块缓存 */
    char z[64];
    return (a_val("z_c0", z, sizeof(z)) == 0 && z[0]) ||
           strstr(api_cookie(), "z_c0=") != NULL;
}

int account_me(char *user, size_t usern, char *err, size_t errn) {
    if (user) user[0] = 0;
    Buf b;
    buf_init(&b);
    int rc = api_fetch_raw("/api/v4/me?include=name,headline,url_token", &b);
    if (rc != 0) {
        if (err) snprintf(err, errn, "%s", api_error());
        buf_free(&b);
        return -1;
    }
    char tmp[128];
    int r = a_json_str(buf_cstr(&b), "name", tmp, sizeof(tmp));
    if (r == 0 && user && usern) snprintf(user, usern, "%s", tmp);
    buf_free(&b);
    if (r != 0 && err) snprintf(err, errn, "接口未返回用户信息，可能 Cookie 已失效");
    return r;
}

int account_qr_begin(AuthQr *q) {
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
    q->state = 0;
    a_cookie[0] = 0;   /* 用全新会话取码，避免旧登录态干扰 */

    HttpResp r;

    /* 1) /signin 拿 _xsrf / _zap / BEC */
    http_resp_init(&r);
    if (a_req("GET", "/signin", "Accept: text/html,application/xhtml+xml\r\n", NULL, &r) != 0 || r.status != 200) {
        a_errf("打开登录页失败（%s）", r.err[0] ? r.err : "网络错误");
        snprintf(q->err, sizeof(q->err), "%s", a_err);
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    http_resp_free(&r);

    /* 2) /udid 拿 d_c0（返回纯文本 udid，同时下发 d_c0） */
    http_resp_init(&r);
    if (a_req("POST", "/udid", NULL, "", &r) != 0 || (r.status != 200 && r.status != 201)) {
        a_errf("获取设备标识失败（%d）", r.status);
        snprintf(q->err, sizeof(q->err), "%s", a_err);
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    http_resp_free(&r);

    /* 3) 询问是否需要人机验证（不阻断，仅记录；二维码链路通常不需要） */
    http_resp_init(&r);
    a_req("GET", "/api/v3/oauth/captcha/v2?type=captcha_sign_in", NULL, NULL, &r);
    http_resp_free(&r);

    /* 4) 取二维码 */
    char xs[256] = "";
    a_val("_xsrf", xs, sizeof(xs));
    char xh[400];
    snprintf(xh, sizeof(xh), "x-xsrftoken: %s\r\nContent-Type: application/json\r\n", xs);
    http_resp_init(&r);
    int rc = a_req("POST", "/api/v3/account/api/login/qrcode", xh, "{}", &r);
    if (rc != 0 || r.status != 200) {
        a_errf("获取二维码失败（status=%d %s）", r.status, r.err[0] ? r.err : "");
        snprintf(q->err, sizeof(q->err), "%s", a_err);
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    buf_cstr(&r.body);
    JVal *v = json_parse(r.body.p, r.body.len);
    if (!v) {
        snprintf(q->err, sizeof(q->err), "二维码响应解析失败");
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    const char *tok = json_get_str(v, "token", NULL);
    const char *lnk = json_get_str(v, "link", NULL);
    if (!tok || !lnk) {
        snprintf(q->err, sizeof(q->err), "二维码响应缺少 token/link");
        json_free(v);
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    snprintf(q->token, sizeof(q->token), "%s", tok);
    snprintf(q->link, sizeof(q->link), "%s", lnk);
    json_free(v);
    http_resp_free(&r);

    /* 把当前 cookie 同步给主模块，后续签名才能用上 d_c0 */
    api_set_cookie(a_cookie);
    q->state = 1;
    return 0;
}

int account_qr_poll(AuthQr *q) {
    if (!q || !q->token[0]) return -1;
    char path[320];
    snprintf(path, sizeof(path), "/api/v3/account/api/login/qrcode/%s/scan_info", q->token);

    HttpResp r;
    http_resp_init(&r);
    int rc = a_req("GET", path, NULL, NULL, &r);
    if (rc != 0) {
        snprintf(q->err, sizeof(q->err), "轮询失败：%s", r.err[0] ? r.err : "网络错误");
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }
    buf_cstr(&r.body);
    char z[64];
    int have_z = (a_val("z_c0", z, sizeof(z)) == 0 && z[0]);

    if (have_z) {
        /* 扫码确认成功：整串 cookie 落盘，主模块立即可用 */
        api_set_cookie(a_cookie);
        api_save_cookie();
        char err[256];
        if (account_me(q->user, sizeof(q->user), err, sizeof(err)) == 0) {
            snprintf(q->err, sizeof(q->err), "登录成功：%s", q->user);
        } else {
            snprintf(q->user, sizeof(q->user), "已登录");
        }
        q->state = 3;
        logmsg("account: 扫码登录成功 z_c0 已保存");
        http_resp_free(&r);
        return 3;
    }

    if (r.status == 403 &&
        (strstr(r.body.p, "40352") || strstr(r.body.p, "unhuman") || strstr(r.body.p, "网络环境"))) {
        snprintf(q->err, sizeof(q->err),
                 "当前网络被知乎风控拦截。请改用「导入 Cookie」，或换一个网络后重试");
        logmsg("account: scan_info 被风控 40352");
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }

    if (r.status != 200) {
        snprintf(q->err, sizeof(q->err), "扫码状态查询失败（HTTP %d）", r.status);
        http_resp_free(&r);
        q->state = -1;
        return -1;
    }

    /* 正常等待：解析 status 字段（0 未扫 / 1 已扫待确认） */
    JVal *v = json_parse(r.body.p, r.body.len);
    long st = v ? json_get_int(v, "status", 0) : 0;
    if (v) json_free(v);
    q->state = (st >= 1) ? 2 : 1;
    if (q->state == 2)
        snprintf(q->err, sizeof(q->err), "已扫码，请在手机上点「确认登录」");
    else
        snprintf(q->err, sizeof(q->err), "等待扫码…");
    http_resp_free(&r);
    return q->state;
}

int account_import_cookie(const char *raw, char *user, size_t usern,
                          char *err, size_t errn) {
    if (!raw || !*raw) {
        if (err) snprintf(err, errn, "Cookie 为空");
        return -1;
    }
    if (!strstr(raw, "z_c0=")) {
        if (err) snprintf(err, errn, "Cookie 缺少 z_c0（登录凭证），请复制完整 Cookie");
        return -1;
    }
    /* 统一交给主模块清洗并保存 */
    api_set_cookie(raw);
    api_save_cookie();
    snprintf(a_cookie, sizeof(a_cookie), "%s", api_cookie());
    char e[256];
    if (account_me(user, usern, e, sizeof(e)) != 0) {
        if (err) snprintf(err, errn, "Cookie 校验失败：%s", e);
        return -1;
    }
    if (err) snprintf(err, errn, "导入成功：%s", user ? user : "");
    logmsg("account: Cookie 导入成功 %s", user ? user : "");
    return 0;
}

void account_logout(void) {
    /* 清掉登录凭证，保留 d_c0（设备标识变了会更容易触发风控） */
    api_set_cookie("");
    char d[256];
    if (a_val("d_c0", d, sizeof(d)) == 0 && d[0]) {
        char keep[320];
        snprintf(keep, sizeof(keep), "d_c0=%s", d);
        api_set_cookie(keep);
    }
    api_save_cookie();
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", api_cookie());
    snprintf(a_cookie, sizeof(a_cookie), "%s", buf);
    logmsg("account: 已退出登录");
}
