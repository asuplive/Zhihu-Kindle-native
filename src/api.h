/* api.h —— 知乎网页端接口封装（全部设备端直连）
 *
 * 关键点：网页端接口需要 x-zse-96 签名 + d_c0 cookie。游客也能用，
 * 只是内容受限；导入登录 cookie 后可看关注流与收藏夹。
 */
#ifndef ZH_API_H
#define ZH_API_H

#include "common.h"

typedef struct {
    char  id[64];       /* 回答 id / 文章 id / 问题 id */
    char  qid[64];      /* 所属问题 id（回答时有） */
    char  title[600];   /* 标题或问题标题 */
    char  sub[400];     /* 作者、摘要、热度等副标题 */
    char  url[256];     /* 列表里给的原始链接（排障用：404 时打日志） */
    int   type;         /* 1=回答 2=文章 3=问题 */
    long  voteup;
    long  comments;
    long  answers;
} ApiItem;

typedef struct {
    ApiItem *v;
    int      n;
    int      cap;
} ApiList;

typedef struct {
    char *title;
    char *author;
    char *html;     /* 原始 HTML，交给 content.c 清洗 */
    long  voteup;
    long  comments;
    long  created;  /* 发布时间（Unix 秒），0 = 接口没给 */
} ApiContent;

void        api_set_home(const char *home);
const char *api_error(void);
/* 供其它模块（私信/登录）统一设置错误文案 */
void        api_set_error(const char *fmt, ...);
int         api_has_login(void);
/* 取游客 d_c0（首次联网时自动完成）；返回 0 成功 */
int         api_warmup(void);

int api_hot(ApiList *out, int limit);
int api_recommend(ApiList *out, int page, int limit);
int api_search(const char *q, ApiList *out, const char *tab);
int api_question_answers(const char *qid, ApiList *out, int offset, int limit);
int api_answer_detail(const char *aid, ApiContent *out);
int api_article_detail(const char *aid, ApiContent *out);
/* 带上下文 Referer 的详情请求；失败时（404）会自动退到"问题回答列表里
 * 按 id 捞"。qid 可为空。这是 1.2.5 修"点进去 404"的主要手段。 */
int api_answer_detail2(const char *aid, const char *qid, ApiContent *out);
int api_article_detail2(const char *aid, ApiContent *out);
int api_my_collections(ApiList *out);
int api_collection_items(const char *cid, ApiList *out, int offset);
/* 「全部收藏」：把各收藏夹的内容合并成一个列表（子进程里跑，避免卡界面）。
 * max_cols 限制遍历的收藏夹个数，防止请求数失控。 */
int api_all_collections(ApiList *out, int max_cols);

/* 时间戳 -> "YYYY-MM-DD"；ts<=0 时返回空串 */
void api_format_date(long ts, char *out, size_t outsz);

/* 取二进制内容（图片 CDN）。不签名，但带上 Cookie 与 Referer，
 * 否则 pic1.zhimg.com 会返回 403。 */
int api_fetch_bin(const char *url, const char *referer, Buf *out);

/* ApiList 落盘/读取（fork 子进程 -> 父进程）。返回 0 成功。 */
int api_list_save(const char *path, const ApiList *l);
int api_list_load(const char *path, ApiList *l);

void api_list_free(ApiList *l);
void api_content_free(ApiContent *c);

/* 底层：取原始 JSON（供 fork 子进程写入文件后再解析） */
int  api_fetch_raw(const char *path, Buf *out);
int  api_parse_feed(const char *json, size_t len, ApiList *out);
int  api_parse_detail(const char *json, size_t len, ApiContent *out);
long api_cache_clear(void);
/* 自检：URL -> id 抽取（1.2.4 的 404 有一半出在这里） */
int  api_ids_selftest(void);
void api_load_cookie(void);

/* 导入/导出 cookie（整段 Cookie 头字符串） */
const char *api_cookie(void);
void        api_set_cookie(const char *cookie);
int         api_save_cookie(void);
/* 最近一次请求的 HTTP 状态码（0 = 网络层失败）、完整 URL、响应体前 200 字节，
 * 供端点诊断（--apitest 会把它们打到屏幕上） */
int         api_last_status(void);
const char *api_last_next(void);
/* 按服务端 paging.next 继续拉一页（推荐流专用：page_number 翻页会重复） */
int         api_recommend_next(ApiList *out, const char *abs_url);
const char *api_last_url(void);
const char *api_last_head(void);

#endif
