/* probe.c —— 开发期联网探针（不进最终设备包，只用于验证签名/TLS/接口） */
#include "api.h"
#include "common.h"
#include "content.h"
#include "json.h"
#include "net.h"
#include "zse96.h"

#include <stdio.h>
#include <string.h>

static void dump_list(ApiList *l) {
    for (int i = 0; i < l->n; i++) {
        ApiItem *it = &l->v[i];
        printf("%2d. [%d] id=%s qid=%s\n    %s\n    %s\n", i + 1, it->type, it->id, it->qid,
               it->title, it->sub);
    }
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "hot";
    setvbuf(stdout, NULL, _IONBF, 0);
    api_set_home("/tmp/zhprobe");
    zh_set_home("/tmp/zhprobe");
    mkdir_p("/tmp/zhprobe");

    if (strcmp(mode, "dns") == 0) {
        char ip[64];
        int rc = net_resolve("www.zhihu.com", ip, sizeof(ip));
        printf("[DNS] rc=%d ip=%s\n", rc, ip);
        return rc;
    }
    if (strcmp(mode, "warm") == 0) {
        int rc = api_warmup();
        printf("[warmup] rc=%d err=%s\n[cookie] %s\n", rc, api_error(), api_cookie());
        return rc;
    }
    /* raw <path> [cookie-file]：带 zse96 签名拉任意端点，打印状态码与正文，
     * 用于真机上确认私信/通知等端点的真实返回结构。 */
    if (strcmp(mode, "raw") == 0) {
        const char *path = argc > 2 ? argv[2] : "/api/v4/me";
        if (argc > 3) {
            size_t n = 0;
            char *c = read_file(argv[3], &n);
            if (c) { api_set_cookie(c); free(c); }
        }
        Buf b;
        buf_init(&b);
        int r = api_fetch_raw(path, &b);
        printf("[raw] %s rc=%d status=%d err=%s len=%zu\n",
               path, r, api_last_status(), api_error(), b.len);
        fwrite(b.p, 1, b.len > 4000 ? 4000 : b.len, stdout);
        printf("\n");
        buf_free(&b);
        return r;
    }

    int rc = api_warmup();
    if (rc != 0) {
        printf("[warmup 失败] %s\n", api_error());
        return 1;
    }
    printf("[cookie] %.80s...\n", api_cookie());

    ApiList list = {0};
    if (strcmp(mode, "search") == 0) {
        rc = api_search(argc > 2 ? argv[2] : "Kindle", &list, "general");
    } else if (strcmp(mode, "answer") == 0) {
        ApiContent c = {0};
        rc = api_answer_detail(argc > 2 ? argv[2] : "1", &c);
        if (rc == 0) {
            Doc d;
            doc_init(&d);
            html_to_doc(c.html, &d, 1);
            printf("[回答] %s / %s  段落=%d 赞=%ld\n", c.title, c.author, d.n, c.voteup);
            for (int i = 0; i < d.n && i < 8; i++) printf("  <%d> %.120s\n", d.v[i].kind, d.v[i].text);
            doc_free(&d);
            api_content_free(&c);
        }
    } else if (strcmp(mode, "answers") == 0) {
        rc = api_question_answers(argc > 2 ? argv[2] : "1", &list, 0, 5);
    } else if (strcmp(mode, "rec") == 0) {
        rc = api_recommend(&list, 0);
    } else {
        rc = api_hot(&list, 50);
    }

    if (rc != 0) {
        printf("[失败] %s\n", api_error());
        return 1;
    }
    printf("[成功] 条目=%d\n", list.n);
    dump_list(&list);
    api_list_free(&list);
    return 0;
}
