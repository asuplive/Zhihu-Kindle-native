#include "json.h"
#include "common.h"

typedef struct {
    const char *p;
    const char *end;
} Cur;

static JVal *parse_value(Cur *c, int depth);

static void skip_ws(Cur *c) {
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') c->p++;
        else break;
    }
}

static JVal *jnew(JType t) {
    JVal *v = xcalloc(1, sizeof(JVal));
    v->t = t;
    return v;
}

static void push_item(JVal *arr, JVal *item) {
    arr->items = xrealloc(arr->items, sizeof(JVal *) * (size_t)(arr->n + 1));
    arr->items[arr->n++] = item;
}

static void push_pair(JVal *obj, char *k, JVal *v) {
    obj->keys = xrealloc(obj->keys, sizeof(char *) * (size_t)(obj->n + 1));
    obj->vals = xrealloc(obj->vals, sizeof(JVal *) * (size_t)(obj->n + 1));
    obj->keys[obj->n] = k;
    obj->vals[obj->n] = v;
    obj->n++;
}

/* 解析字符串到 Buf，处理 \uXXXX（含代理对） */
static int parse_string_raw(Cur *c, char **out) {
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++;
    Buf b;
    buf_init(&b);
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') { c->p++; *out = buf_steal(&b); return 0; }
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) break;
            char e = *c->p++;
            switch (e) {
                case '"':  buf_addc(&b, '"');  break;
                case '\\': buf_addc(&b, '\\'); break;
                case '/':  buf_addc(&b, '/');  break;
                case 'b':  buf_addc(&b, '\b'); break;
                case 'f':  buf_addc(&b, '\f'); break;
                case 'n':  buf_addc(&b, '\n'); break;
                case 'r':  buf_addc(&b, '\r'); break;
                case 't':  buf_addc(&b, '\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        if (c->p >= c->end) { cp = 0xFFFD; break; }
                        char h = *c->p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                        else cp |= 0;
                    }
                    /* 代理对 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && c->end - c->p >= 6 &&
                        c->p[0] == '\\' && c->p[1] == 'u') {
                        uint32_t lo = 0;
                        const char *save = c->p;
                        c->p += 2;
                        int ok = 1;
                        for (int i = 0; i < 4; i++) {
                            char h = *c->p++;
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (uint32_t)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (uint32_t)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (uint32_t)(h - 'A' + 10);
                            else ok = 0;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            c->p = save;
                    }
                    char tmp[4];
                    int n = u8_encode(cp, tmp);
                    buf_add(&b, tmp, (size_t)n);
                    break;
                }
                default: buf_addc(&b, e); break;
            }
            continue;
        }
        buf_addc(&b, (char)ch);
        c->p++;
    }
    buf_free(&b);
    return -1;
}

static JVal *parse_value(Cur *c, int depth) {
    if (depth > 64) return NULL;
    skip_ws(c);
    if (c->p >= c->end) return NULL;
    char ch = *c->p;

    if (ch == '{') {
        c->p++;
        JVal *o = jnew(JOBJ);
        skip_ws(c);
        if (c->p < c->end && *c->p == '}') { c->p++; return o; }
        for (;;) {
            skip_ws(c);
            char *k = NULL;
            if (parse_string_raw(c, &k) != 0) { json_free(o); return NULL; }
            skip_ws(c);
            if (c->p >= c->end || *c->p != ':') { free(k); json_free(o); return NULL; }
            c->p++;
            JVal *v = parse_value(c, depth + 1);
            if (!v) { free(k); json_free(o); return NULL; }
            push_pair(o, k, v);
            skip_ws(c);
            if (c->p < c->end && *c->p == ',') { c->p++; continue; }
            if (c->p < c->end && *c->p == '}') { c->p++; break; }
            json_free(o);
            return NULL;
        }
        return o;
    }
    if (ch == '[') {
        c->p++;
        JVal *a = jnew(JARR);
        skip_ws(c);
        if (c->p < c->end && *c->p == ']') { c->p++; return a; }
        for (;;) {
            JVal *v = parse_value(c, depth + 1);
            if (!v) { json_free(a); return NULL; }
            push_item(a, v);
            skip_ws(c);
            if (c->p < c->end && *c->p == ',') { c->p++; continue; }
            if (c->p < c->end && *c->p == ']') { c->p++; break; }
            json_free(a);
            return NULL;
        }
        return a;
    }
    if (ch == '"') {
        char *s = NULL;
        if (parse_string_raw(c, &s) != 0) return NULL;
        JVal *v = jnew(JSTR);
        v->str = s;
        return v;
    }
    if (ch == 't' && c->end - c->p >= 4 && strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        JVal *v = jnew(JBOOL);
        v->b = 1;
        return v;
    }
    if (ch == 'f' && c->end - c->p >= 5 && strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        JVal *v = jnew(JBOOL);
        v->b = 0;
        return v;
    }
    if (ch == 'n' && c->end - c->p >= 4 && strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return jnew(JNULL);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const char *start = c->p;
        if (*c->p == '-') c->p++;
        while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
                                 *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-'))
            c->p++;
        char tmp[64];
        size_t n = (size_t)(c->p - start);
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, start, n);
        tmp[n] = 0;
        JVal *v = jnew(JNUM);
        v->num = atof(tmp);
        /* 保留原始字面量：19 位 id 转 double 会丢精度（见 json_get_idstr） */
        v->raw = xstrdup(tmp);
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *s, size_t len) {
    Cur c;
    c.p = s;
    c.end = s + len;
    JVal *v = parse_value(&c, 0);
    return v;
}

void json_free(JVal *v) {
    if (!v) return;
    switch (v->t) {
        case JSTR:
            free(v->str);
            break;
        case JNUM:
            free(v->raw);
            break;
        case JARR:
            for (int i = 0; i < v->n; i++) json_free(v->items[i]);
            free(v->items);
            break;
        case JOBJ:
            for (int i = 0; i < v->n; i++) {
                free(v->keys[i]);
                json_free(v->vals[i]);
            }
            free(v->keys);
            free(v->vals);
            break;
        default:
            break;
    }
    free(v);
}

JVal *json_get(JVal *v, const char *key) {
    if (!v || v->t != JOBJ || !key) return NULL;
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->keys[i], key) == 0) return v->vals[i];
    return NULL;
}

const char *json_get_str(JVal *v, const char *key, const char *def) {
    JVal *x = json_get(v, key);
    if (!x) return def;
    if (x->t == JSTR) return x->str;
    if (x->t == JNUM) return def;
    return def;
}

long json_get_int(JVal *v, const char *key, long def) {
    JVal *x = json_get(v, key);
    if (!x) return def;
    if (x->t == JNUM) return (long)x->num;
    if (x->t == JSTR) return (long)atol(x->str);
    if (x->t == JBOOL) return x->b;
    return def;
}

int json_get_idstr(JVal *v, const char *key, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return -1;
    buf[0] = 0;
    JVal *x = key ? json_get(v, key) : v;
    if (!x) return -1;
    if (x->t == JSTR) {
        if (!x->str || !x->str[0]) return -1;
        snprintf(buf, bufsz, "%s", x->str);
        return 0;
    }
    if (x->t == JNUM) {
        const char *src = x->raw;
        if (!src || !*src) return -1;
        /* 整数形式才当 id；带小数点/指数的（1.2e3）不是 id */
        for (const char *t = src; *t; t++)
            if (*t == '.' || *t == 'e' || *t == 'E') return -1;
        snprintf(buf, bufsz, "%s", src);
        return 0;
    }
    return -1;
}

int json_get_bool(JVal *v, const char *key, int def) {
    JVal *x = json_get(v, key);
    if (!x) return def;
    if (x->t == JBOOL) return x->b;
    if (x->t == JNUM) return x->num != 0;
    return def;
}

const char *json_key_at(JVal *v, int i) {
    if (!v || v->t != JOBJ || i < 0 || i >= v->n) return NULL;
    return v->keys[i];
}
