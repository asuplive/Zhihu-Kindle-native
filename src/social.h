/* social.h —— 私信（只读）：会话列表 + 会话内消息
 *
 * 知乎私信端点随版本变动较大，且必须登录态才返回数据。这里采用
 * 「多候选端点 + 通用 JSON 字段提取」，并把端点写进配置文件可覆盖：
 *   /mnt/us/zhihu/social.conf
 *     threads=/api/v4/inbox?limit=20&offset=0
 *     messages=/api/v4/inbox/{id}/messages?limit=20&offset=0
 * 真机上若端点变了，改这个文件即可，无需重编译。
 */
#ifndef ZH_SOCIAL_H
#define ZH_SOCIAL_H

#include "common.h"

typedef struct {
    char id[64];
    char url[256];        /* 服务端给的会话地址（有就照它取消息，别再猜端点） */
    char peer[128];       /* 对方昵称 */
    char snippet[400];    /* 最后一条消息摘要 */
    int  unread;
    long updated;         /* unix 秒 */
} MsgThread;

typedef struct {
    MsgThread *v;
    int n, cap;
} ThreadList;

typedef struct {
    char sender[128];
    char content[2048];
    long created;
    int  is_self;
} MsgItem;

typedef struct {
    MsgItem *v;
    int n, cap;
} MsgList;

void thread_list_free(ThreadList *l);
void msg_list_free(MsgList *l);

/* 返回 0 成功；失败时 api_error() 有原因。未登录时返回 -1 并给出提示。 */
int social_threads(ThreadList *out, int offset);
int social_messages(const char *thread_id, MsgList *out, int offset);
/* 优先按会话自带 url 取（知乎列表项里就有），取不到再按 id 猜端点 */
int social_messages2(const char *url, const char *thread_id, MsgList *out, int offset);

/* 解析（纯函数，供离线自检：真机上没有登录态可用来跑测试） */
int social_parse_threads(const char *json, size_t len, ThreadList *out);
int social_parse_messages(const char *json, size_t len, MsgList *out);

/* 端点诊断：依次记录每个候选端点的 HTTP 状态，写入日志，便于真机排查 */
void social_probe(void);
/* 同上，但把精简结果同时写到 path，供 UI 直接显示在屏幕上 */
int  social_probe_to(const char *path);

/* ---- 供 UI 的 fork 子进程使用：结果直接以结构体数组落盘 ----
 * 父子进程同架构、同一份二进制，直接 fwrite/fread，无需序列化。
 * 失败时返回 -1，api_error() 里有原因。 */
int social_threads_dump(const char *path, int offset);
int social_threads_load(const char *path, ThreadList *out);
int social_messages_dump(const char *path, const char *thread_id, int offset);
int social_messages_dump2(const char *path, const char *url, const char *id, int offset);
int social_messages_load(const char *path, MsgList *out);

#endif
