/* json.h —— 极简 JSON 解析（只读 DOM）
 *
 * 为什么不用第三方库：Kindle 上要静态链接、体积敏感，而本 App 只需
 * 「解析响应 + 按键取值」这一条路径，200 行足够，且不会引入额外依赖。
 */
#ifndef ZH_JSON_H
#define ZH_JSON_H

#include <stddef.h>

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } JType;

typedef struct JVal JVal;

struct JVal {
    JType t;
    int    b;          /* JBOOL */
    double num;        /* JNUM */
    char  *raw;        /* JNUM 的原始字面量（见 json_get_idstr 的注释） */
    char  *str;        /* JSTR */
    /* 数组 */
    JVal **items;
    int    n;
    /* 对象 */
    char **keys;
    JVal **vals;
};

JVal *json_parse(const char *s, size_t len);
void  json_free(JVal *v);

JVal       *json_get(JVal *v, const char *key);
const char *json_get_str(JVal *v, const char *key, const char *def);
long        json_get_int(JVal *v, const char *key, long def);

/* 取"可能是数字也可能是字符串"的 id，**按原始字面量**返回。
 *
 * 为什么不能用 json_get_int：知乎的 id 已经是 19 位（如
 * 2103784826771862921），这个数超出了 double 的精确整数范围（2^53），
 * 也远超 32 位 long。用 atof 解析再转回来会变成 2103784826771863040——
 * 差 121，请求出去的 id 根本不存在，服务端就回 404。
 * 这就是"标题能看到、点进去 404"的真正原因。
 * 返回 0 成功。 */
int         json_get_idstr(JVal *v, const char *key, char *buf, size_t bufsz);
int         json_get_bool(JVal *v, const char *key, int def);

static inline JType json_type(JVal *v) { return v ? v->t : JNULL; }
static inline int   json_count(JVal *v) { return (v && (v->t == JARR || v->t == JOBJ)) ? v->n : 0; }
static inline JVal *json_at(JVal *v, int i) {
    if (!v || v->t != JARR || i < 0 || i >= v->n) return NULL;
    return v->items[i];
}
const char *json_key_at(JVal *v, int i);

#endif
