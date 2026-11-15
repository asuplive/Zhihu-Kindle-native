/* social.c —— 私信（只读）：会话列表 + 会话内消息
 *
 * 知乎私信端点在网页/移动端之间差异很大，且必须登录态才返回数据。
 * 因此这里不硬编码单一端点：按顺序尝试若干候选，取第一个返回 200 且
 * 能解析出条目的；端点可由 /mnt/us/zhihu/social.conf 覆盖，真机改配置即可。
 *
 *   1.2.6 以为网页版端点是 /api/v4/me/conversations，真机上恒 404——
 *   这个路由不存在（实测 www.zhihu.com 对它是 nginx 级 404，连 JSON 都不回）。
 *   目前确认存在的是 /api/v4/inbox（游客 401、登录后返回会话列表），
 *   会话内消息优先走列表项自带的 url 字段，取不到再按 {id} 猜。
 */
#include "social.h"
#include "api.h"
#include "json.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *THREAD_CANDIDATES[] = {
    /* 1.2.13：App 端（api.zhihu.com）路由。2026-11 游客探针实测：
     * /messages/inbox 返回 401（路由存在、要登录），登录态真机上
     * 很可能这才是真正有数据的通道。绝对 URL 由 api_call 原样使用。 */
    "https://api.zhihu.com/messages/inbox?limit=20&offset=%d",
    /* 首页私信入口用的就是它：未登录 401、登录后 200 */
    "/api/v4/inbox?limit=20&offset=%d",
    /* 1.2.9 实测存在（游客 200 + 空成功体），登录后应返回会话数组 */
    "/api/v4/me/message-threads?limit=20&offset=%d",
    /* 老接口（怀疑已下线，留着兜底，失败只多两条日志） */
    "/api/v4/message-threads?limit=20&offset=%d",
};
static const char *MSG_CANDIDATES[] = {
    /* 1.2.9 实测（2026-11，游客态）：/api/v4/inbox/{id}/messages 是 nginx 级
     * 404——这个路由根本不存在，这就是真机上"4 个端点状态 404"的根。
     * 真正存在的是下面两个（游客 200 + 空成功体，登录后带数据）。
     * 顺序按把握排：先 messages?thread_id，再 me/message-threads/{id}/messages
     * （404 但万一某些固件又把它加回来），最后老 inbox 路由兜底。 */
    "/api/v4/messages?thread_id={id}&limit=20&offset=%d",
    /* 1.2.13：App 端路由，游客探针 401（路由存在、要登录）——
     * 真机诊断显示 web 端 M1 拿到 200 空成功体（len=22，被当成游客），
     * 所以登录态下换 App 端通道是首要尝试。 */
    "https://api.zhihu.com/messages/inbox?thread_id={id}&limit=20&offset=%d",
    "https://api.zhihu.com/messages?thread_id={id}&limit=20&offset=%d",
    "https://api.zhihu.com/messages/conversation?thread_id={id}&limit=20&offset=%d",
    "/api/v4/me/message-threads/{id}/messages?limit=20&offset=%d",
    "/api/v4/inbox/{id}/messages?limit=20&offset=%d",
};

static char cfg_threads[512];
static char cfg_messages[512];
static int  cfg_loaded = 0;

static void load_conf(void) {
    if (cfg_loaded) return;
    cfg_loaded = 1;
    char path[320];
    snprintf(path, sizeof(path), "%s/social.conf", zh_home());
    size_t n = 0;
    char *s = read_file(path, &n);
    if (!s) return;
    char *save = NULL;
    for (char *ln = strtok_r(s, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(ln, '=');
        if (!eq || *ln == '#') continue;
        *eq = 0;
        char *k = str_trim(ln), *v = str_trim(eq + 1);
        if (strcmp(k, "threads") == 0) snprintf(cfg_threads, sizeof(cfg_threads), "%s", v);
        else if (strcmp(k, "messages") == 0) snprintf(cfg_messages, sizeof(cfg_messages), "%s", v);
    }
    free(s);
    logmsg("social: 配置 threads=%s messages=%s",
           cfg_threads[0] ? cfg_threads : "(默认)", cfg_messages[0] ? cfg_messages : "(默认)");
}

static JVal *find_array(JVal *root) {
    if (!root) return NULL;
    if (root->t == JARR) return root;
    if (root->t == JOBJ) {
        const char *keys[] = {"data", "threads", "conversations", "items", "messages", "list"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            JVal *a = json_get(root, keys[i]);
            if (a && a->t == JARR) return a;
        }
        JVal *d = json_get(root, "data");
        if (d && d->t == JOBJ) return find_array(d);
    }
    return NULL;
}

static JVal *unwrap(JVal *raw) {
    JVal *tgt = json_get(raw, "target");
    if (tgt && tgt->t == JOBJ) return tgt;
    JVal *simple = json_get(raw, "simple");
    if (simple && simple->t == JOBJ) return simple;
    return raw;
}

static void pick_peer(JVal *it, char *out, size_t n) {
    /* 会话列表里对方的昵称：新版是 participant，旧版/别的形态是 user 等 */
    const char *objs[] = {"participant", "peer", "user", "receiver", "sender", "target", "to"};
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        JVal *o = json_get(it, objs[i]);
        if (o && o->t == JOBJ) {
            const char *s = json_get_str(o, "name", NULL);
            if (!s) {
                JVal *m = json_get(o, "member");
                if (m) s = json_get_str(m, "name", NULL);
            }
            if (s && *s) { snprintf(out, n, "%s", s); return; }
        }
    }
    /* participants 是数组形态：取第一个不是自己的 */
    JVal *ps = json_get(it, "participants");
    if (ps && ps->t == JARR && ps->n > 0) {
        for (int i = 0; i < ps->n && i < 4; i++) {
            JVal *o = json_at(ps, i);
            const char *s = o ? json_get_str(o, "name", NULL) : NULL;
            if (s && *s) { snprintf(out, n, "%s", s); return; }
        }
    }
    const char *direct[] = {"name", "peer", "user_name", "nickname", "title"};
    for (size_t i = 0; i < sizeof(direct) / sizeof(direct[0]); i++) {
        const char *s = json_get_str(it, direct[i], NULL);
        if (s && *s) { snprintf(out, n, "%s", s); return; }
    }
    snprintf(out, n, "未知用户");
}

static void pick_snippet(JVal *it, char *out, size_t n) {
    const char *lmkeys[] = {"latest_message", "last_message", "lastMessage"};
    for (size_t i = 0; i < sizeof(lmkeys) / sizeof(lmkeys[0]); i++) {
        JVal *lm = json_get(it, lmkeys[i]);
        if (!lm || lm->t != JOBJ) continue;
        const char *s = json_get_str(lm, "content", NULL);
        if (!s || !*s) s = json_get_str(lm, "text", NULL);
        if (s && *s) {
            snprintf(out, n, "%s", s);
            /* 预览里夹带换行会把整行列表撑乱 */
            for (char *q = out; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
            return;
        }
    }
    const char *keys[] = {"snippet", "excerpt", "last_content", "content", "text", "summary"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *s = json_get_str(it, keys[i], NULL);
        if (s && *s) { snprintf(out, n, "%s", s); return; }
    }
    snprintf(out, n, "（无预览）");
}

/* 会话对象里往往带 url（就是"这个会话的消息列表"地址）。
 * 顺着它取，比猜端点可靠得多。 */
static void pick_url(JVal *it, char *out, size_t n) {
    out[0] = 0;
    const char *s = json_get_str(it, "url", NULL);
    if (!s) s = json_get_str(it, "messages_url", NULL);
    if (!s) return;
    if (!strstr(s, "://")) { snprintf(out, n, "%s", s); return; }
    const char *p = strstr(s, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    snprintf(out, n, "%s", p ? p : "");
}

static void pick_id(JVal *it, char *out, size_t n) {
    const char *keys[] = {"id", "thread_id", "conversation_id", "message_thread_id", "thread_token"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        JVal *v = json_get(it, keys[i]);
        if (!v) continue;
        /* 用字面量，别用 double：私信会话 id 同样是 19 位 */
        if (json_get_idstr(it, keys[i], out, n) == 0 && out[0]) return;
    }
    snprintf(out, n, "");
}

static void pick_long(JVal *it, long *out, const char **keys, int nk) {
    for (int i = 0; i < nk; i++) {
        long v = json_get_int(it, keys[i], -1);
        if (v > 0) { *out = v; return; }
    }
}

void thread_list_free(ThreadList *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
void msg_list_free(MsgList *l)      { free(l->v); l->v = NULL; l->n = l->cap = 0; }

static void thread_push(ThreadList *l, MsgThread t) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, sizeof(MsgThread) * (size_t)l->cap);
    }
    l->v[l->n++] = t;
}
static void msg_push(MsgList *l, MsgItem m) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, sizeof(MsgItem) * (size_t)l->cap);
    }
    l->v[l->n++] = m;
}

/* 把模板里的 %d 换成 offset、{id} 换成会话 id；用字符级拼装避免 snprintf 参数陷阱 */
static void format_path(char *out, size_t n, const char *tmpl,
                        const char *id, int offset) {
    char *p = out;
    char *end = out + n - 1;
    for (const char *s = tmpl; *s && p < end; ) {
        if (s[0] == '{' && strncmp(s, "{id}", 4) == 0) {
            size_t l = strlen(id ? id : "");
            if ((size_t)(end - p) < l) l = (size_t)(end - p);
            memcpy(p, id ? id : "", l); p += l; s += 4;
        } else if (s[0] == '%' && s[1] == 'd') {
            p += snprintf(p, (size_t)(end - p), "%d", offset);
            s += 2;
        } else {
            *p++ = *s++;
        }
    }
    *p = 0;
}

static int no_login(void) {
    api_set_error("私信需要登录。请先在「账号」里扫码登录或导入 Cookie");
    return -1;
}
static int no_data(void) {
    api_set_error("没有读到私信会话。请确认已登录，或用「排障 → 私信端点诊断」把 log.txt 反馈给我");
    return -1;
}

/* 记录每个候选端点的结果，全失败时拼成一句能直接回传的话 */
static char g_tried[240];

/* 解析会话列表 JSON（抽出来是为了能离线自检：真机上不可能拿登录态跑测试）。
 * 返回 0 成功（哪怕 0 条），-1 表示结构不对。 */
int social_parse_threads(const char *json, size_t len, ThreadList *out) {
    JVal *root = json_parse(json, len);
    if (!root) return -1;
    JVal *arr = find_array(root);
    if (!arr) { json_free(root); return -1; }
    for (int i = 0; i < arr->n; i++) {
        JVal *it = unwrap(json_at(arr, i));
        MsgThread t;
        memset(&t, 0, sizeof(t));
        pick_id(it, t.id, sizeof(t.id));
        pick_url(it, t.url, sizeof(t.url));
        pick_peer(it, t.peer, sizeof(t.peer));
        pick_snippet(it, t.snippet, sizeof(t.snippet));
        t.unread = (int)json_get_int(it, "unread_count", json_get_int(it, "unread", 0));
        const char *tk[] = {"updated_time", "updated_at", "latest_time", "created_time", "created"};
        pick_long(it, &t.updated, tk, 5);
        thread_push(out, t);
    }
    json_free(root);
    return 0;
}

/* 解析一个会话里的消息列表 */
int social_parse_messages(const char *json, size_t len, MsgList *out) {
    JVal *root = json_parse(json, len);
    if (!root) return -1;
    JVal *arr = find_array(root);
    if (!arr) { json_free(root); return -1; }
    for (int k = 0; k < arr->n; k++) {
        JVal *it = unwrap(json_at(arr, k));
        MsgItem m;
        memset(&m, 0, sizeof(m));
        const char *sk[] = {"content", "text", "body", "excerpt", "snippet"};
        for (size_t j = 0; j < sizeof(sk) / sizeof(sk[0]); j++) {
            const char *sv = json_get_str(it, sk[j], NULL);
            if (sv && *sv) { snprintf(m.content, sizeof(m.content), "%s", sv); break; }
        }
        JVal *snd = json_get(it, "sender");
        if (!snd) snd = json_get(it, "from");
        if (!snd) snd = json_get(it, "user");
        if (!snd) snd = json_get(it, "author");
        const char *nm = snd ? json_get_str(snd, "name", NULL) : NULL;
        if (!nm) nm = json_get_str(it, "sender_name", NULL);
        snprintf(m.sender, sizeof(m.sender), "%s", nm ? nm : "对方");
        const char *ck[] = {"created_time", "created", "created_at", "time"};
        pick_long(it, &m.created, ck, 4);
        m.is_self = json_get_bool(it, "is_me", json_get_bool(it, "is_self", 0));
        msg_push(out, m);
    }
    json_free(root);
    return 0;
}

static int try_threads(const char *path, ThreadList *out, int *status) {
    Buf b;
    buf_init(&b);
    int rc = api_fetch_raw(path, &b);
    *status = api_last_status();
    if (rc != 0) { buf_free(&b); return -1; }
    buf_cstr(&b);
    JVal *root = json_parse(b.p, b.len);
    if (!root) {
        /* 200 但不是 JSON：把前 200 字节记下来（多半是 HTML 或被拦了） */
        logmsg("social: %s 返回非 JSON，前 200 字节：%.200s", path, b.p ? b.p : "");
        buf_free(&b);
        return -1;
    }
    JVal *arr = find_array(root);
    if (!arr) {
        /* 200 + 能解析、但找不到数组：结构猜错了。把顶层键和原文头一起留下，
         * 下次照着改字段名就行，不用再猜。 */
        char keys[200] = {0};
        for (int i = 0; i < json_count(root) && i < 12; i++) {
            const char *k = json_key_at(root, i);
            if (k) { strncat(keys, k, sizeof(keys) - strlen(keys) - 2); strncat(keys, ",", 1); }
        }
        logmsg("social: %s 找不到列表，顶层键=[%s] 前 200 字节：%.200s",
               path, keys, b.p ? b.p : "");
        json_free(root);
        buf_free(&b);
        return -1;
    }
    json_free(root);
    int pr = social_parse_threads(b.p, b.len, out);
    buf_free(&b);
    return pr;
}

int social_threads(ThreadList *out, int offset) {
    load_conf();
    memset(out, 0, sizeof(*out));
    g_tried[0] = 0;
    if (!api_has_login()) return no_login();

    char path[768];
    if (cfg_threads[0]) {
        format_path(path, sizeof(path), cfg_threads, NULL, offset);
        int st = 0;
        int r = try_threads(path, out, &st);
        logmsg("social: threads(配置) status=%d n=%d", st, out->n);
        if (r != 0) return no_data();
        return 0;
    }
    for (size_t i = 0; i < sizeof(THREAD_CANDIDATES) / sizeof(THREAD_CANDIDATES[0]); i++) {
        format_path(path, sizeof(path), THREAD_CANDIDATES[i], NULL, offset);
        int st = 0;
        int r = try_threads(path, out, &st);
        logmsg("social: threads[%zu] %s status=%d n=%d", i, path, st, out->n);
        {
            char one[64];
            snprintf(one, sizeof(one), "%zu:%d ", i + 1, st);
            strncat(g_tried, one, sizeof(g_tried) - strlen(g_tried) - 1);
        }
        if (r == 0) {
            snprintf(cfg_threads, sizeof(cfg_threads), "%s", THREAD_CANDIDATES[i]);
            return 0;
        }
        thread_list_free(out);
    }
    api_set_error("没有读到私信会话（端点状态 %s）。请确认已登录，或把 log.txt 反馈给我",
                  g_tried[0] ? g_tried : "未知");
    return -1;
}

/* 真正取消息的实现：
 *   url —— 会话对象自带的地址（可能为空）
 *   id  —— 会话 id
 * 顺序：自带 url → 候选模板。全部失败时把每个端点的状态码拼进错误里，
 * 用户回传 log.txt 就能看出是"路由没了"还是"登录态不对"。 */
static int messages_at(const char *url, const char *id, MsgList *out, int offset) {
    load_conf();
    memset(out, 0, sizeof(*out));
    g_tried[0] = 0;
    if ((!id || !*id) && (!url || !*url)) return no_data();
    if (!api_has_login()) return no_login();

    char path[1024];
    int attempt = 0;

    if (url && *url) {
        /* 自带地址可能已经是 ?limit=..&offset=.. 的形式：只换 offset 数值 */
        const char *p = strstr(url, "offset=");
        if (p) {
            snprintf(path, sizeof(path), "%.*s%d%s", (int)(p - url + 7), url, offset,
                     strchr(p, '&') ? strchr(p, '&') : "");
        } else {
            snprintf(path, sizeof(path), "%s%slimit=20&offset=%d", url,
                     strchr(url, '?') ? "&" : "?", offset);
        }
        Buf b;
        buf_init(&b);
        int rc = api_fetch_raw(path, &b);
        int st = api_last_status();
        logmsg("social: messages(url) %s status=%d len=%zu", path, st, b.len);
        if (rc == 0) {
            buf_cstr(&b);
            int pr = social_parse_messages(b.p, b.len, out);
            if (pr == 0) { buf_free(&b); return 0; }
            logmsg("social: messages(url) %s status=%d 200 但解析失败，"
                   "前 160 字节：%.160s", path, st, b.p ? b.p : "");
            buf_free(&b);
        } else {
            buf_free(&b);
        }
        attempt++;
    }

    const char **list = MSG_CANDIDATES;
    size_t nlist = sizeof(MSG_CANDIDATES) / sizeof(MSG_CANDIDATES[0]);
    const char *one[1];
    if (cfg_messages[0]) { one[0] = cfg_messages; list = one; nlist = 1; }

    for (size_t i = 0; i < nlist; i++) {
        format_path(path, sizeof(path), list[i], id, offset);
        Buf b;
        buf_init(&b);
        int rc = api_fetch_raw(path, &b);
        int st = api_last_status();
        if (rc != 0) {
            buf_free(&b);
            logmsg("social: messages[%zu] %s status=%d 跳过", i, path, st);
            char one_s[48];
            snprintf(one_s, sizeof(one_s), "%d ", st);
            strncat(g_tried, one_s, sizeof(g_tried) - strlen(g_tried) - 1);
            attempt++;
            continue;
        }
        buf_cstr(&b);
        int pr = social_parse_messages(b.p, b.len, out);
        if (pr != 0) {
            /* 1.2.13：200 但结构猜错/空成功体，之前不留痕——用户只看到
             * "4 个端点状态 404"，真正该看到的是"200 但没有数据"。
             * 现在状态记为 200*，响应头 160 字节进日志，照着改就行。 */
            logmsg("social: messages[%zu] %s status=%d 200 但解析失败，"
                   "前 160 字节：%.160s", i, path, st, b.p ? b.p : "");
            char one_s[48];
            snprintf(one_s, sizeof(one_s), "%d* ", st);
            strncat(g_tried, one_s, sizeof(g_tried) - strlen(g_tried) - 1);
            buf_free(&b);
            attempt++;
            continue;
        }
        buf_free(&b);
        logmsg("social: messages[%zu] 成功 %s n=%d", i, path, out->n);
        if (!cfg_messages[0]) snprintf(cfg_messages, sizeof(cfg_messages), "%s", list[i]);
        return 0;
    }
    api_set_error("这个会话的消息没读到（%d 个端点，状态 %s）。"
                  "会话 id=%s，请把 log.txt 反馈给我",
                  attempt, g_tried[0] ? g_tried : "未知", id ? id : "-");
    return -1;
}

int social_messages(const char *thread_id, MsgList *out, int offset) {
    return messages_at(NULL, thread_id, out, offset);
}

int social_messages2(const char *url, const char *id, MsgList *out, int offset) {
    return messages_at(url, id, out, offset);
}

/* ---------------- 结构体数组落盘 / 读取 ----------------
 * 文件格式：int count + count 个定长结构体。父进程 fork 出的子进程写，
 * 父进程读；同架构同二进制，不用管字节序与对齐。 */
static int dump_blob(const char *path, const void *v, size_t item, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int cnt = n < 0 ? -1 : n;
    fwrite(&cnt, sizeof(cnt), 1, f);
    if (cnt > 0) fwrite(v, item, (size_t)cnt, f);
    fclose(f);
    return 0;
}
static int load_count(const char *path, FILE **fout) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int cnt = -1;
    if (fread(&cnt, sizeof(cnt), 1, f) != 1) { fclose(f); return -1; }
    *fout = f;
    return cnt;
}

int social_threads_dump(const char *path, int offset) {
    ThreadList l = {0};
    int rc = social_threads(&l, offset);
    dump_blob(path, l.v, sizeof(MsgThread), rc == 0 ? l.n : -1);
    thread_list_free(&l);
    return rc;
}
int social_threads_load(const char *path, ThreadList *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = NULL;
    int cnt = load_count(path, &f);
    if (!f) return -1;
    if (cnt <= 0) { fclose(f); return cnt == 0 ? 0 : -1; }
    out->v = xcalloc((size_t)cnt, sizeof(MsgThread));
    out->cap = out->n = cnt;
    if (fread(out->v, sizeof(MsgThread), (size_t)cnt, f) != (size_t)cnt) {
        fclose(f); thread_list_free(out); return -1;
    }
    fclose(f);
    return 0;
}
int social_messages_dump2(const char *path, const char *url, const char *thread_id, int offset) {
    MsgList l = {0};
    int rc = social_messages2(url, thread_id, &l, offset);
    dump_blob(path, l.v, sizeof(MsgItem), rc == 0 ? l.n : -1);
    msg_list_free(&l);
    return rc;
}
int social_messages_dump(const char *path, const char *thread_id, int offset) {
    return social_messages_dump2(path, NULL, thread_id, offset);
}
int social_messages_load(const char *path, MsgList *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = NULL;
    int cnt = load_count(path, &f);
    if (!f) return -1;
    if (cnt <= 0) { fclose(f); return cnt == 0 ? 0 : -1; }
    out->v = xcalloc((size_t)cnt, sizeof(MsgItem));
    out->cap = out->n = cnt;
    if (fread(out->v, sizeof(MsgItem), (size_t)cnt, f) != (size_t)cnt) {
        fclose(f); msg_list_free(out); return -1;
    }
    fclose(f);
    return 0;
}

static void probe_run(const char *out_path) {
    load_conf();
    FILE *f = out_path ? fopen(out_path, "w") : NULL;
    logmsg("social: === 端点诊断开始 ===");
    /* 屏幕上由 eips 显示，只认西文字体：这里一律用 ASCII */
    if (f) fputs("dm probe (status=0 = network fail)\n", f);
    char path[768];
    for (size_t i = 0; i < sizeof(THREAD_CANDIDATES) / sizeof(THREAD_CANDIDATES[0]); i++) {
        format_path(path, sizeof(path), THREAD_CANDIDATES[i], NULL, 0);
        Buf b;
        buf_init(&b);
        int rc = api_fetch_raw(path, &b);
        int stt = api_last_status();
        logmsg("social: threads[%zu] %-52s rc=%d status=%d len=%zu",
               i, path, rc, stt, b.len);
        /* 屏幕能显示的行数有限，只留编号 / 状态码 / 长度；完整路径写日志 */
        if (f) fprintf(f, "T%d: status=%d len=%zu\n", (int)i + 1, stt, b.len);
        buf_free(&b);
    }
    /* 1.2.9：会话内消息端点也一起探（4 个 404 的定性就在这里） */
    for (size_t i = 0; i < sizeof(MSG_CANDIDATES) / sizeof(MSG_CANDIDATES[0]); i++) {
        format_path(path, sizeof(path), MSG_CANDIDATES[i], "1234567890", 0);
        Buf b;
        buf_init(&b);
        int rc = api_fetch_raw(path, &b);
        int stt = api_last_status();
        logmsg("social: messages[%zu] %-64s rc=%d status=%d len=%zu",
               i, path, rc, stt, b.len);
        if (f) fprintf(f, "M%d: status=%d len=%zu\n", (int)i + 1, stt, b.len);
        buf_free(&b);
    }
    logmsg("social: === 端点诊断结束 ===");
    if (f) fclose(f);
}

void social_probe(void) { probe_run(NULL); }

/* 自检用：把候选端点暴露给 main.c，钉死"首选端点实测存在"这件事 */
const char *social_msg_candidate(int i) {
    if (i < 0 || i >= (int)(sizeof(MSG_CANDIDATES) / sizeof(MSG_CANDIDATES[0]))) return NULL;
    return MSG_CANDIDATES[i];
}
int social_msg_candidate_count(void) {
    return (int)(sizeof(MSG_CANDIDATES) / sizeof(MSG_CANDIDATES[0]));
}
const char *social_thread_candidate(int i) {
    if (i < 0 || i >= (int)(sizeof(THREAD_CANDIDATES) / sizeof(THREAD_CANDIDATES[0])))
        return NULL;
    return THREAD_CANDIDATES[i];
}

int social_probe_to(const char *path) {
    probe_run(path);
    return 0;
}
