/* 1.3.0：拼音输入法码表接口（数据本体在 ime_table.c）。
 * 码表转换自 KOReader 的 zh_pinyin_data.lua（Rime 简体拼音，
 * Apache License 2.0）——即 KOReader 中文键盘用的同一份码表。 */
#ifndef ZH_IME_H
#define ZH_IME_H

typedef struct {
    const char *py;     /* 全拼键（ASCII，如 "ba"、"baiming"） */
    const char *cands;  /* 候选，逗号分隔的 UTF-8 字/词 */
} ImeEntry;

extern const ImeEntry ime_table[];
extern const int ime_table_n;

/* 精确查找一个键；找不到返回 NULL（二分查找，表已按 ASCII 排序） */
const ImeEntry *ime_find(const char *py);

/* 把候选串按逗号切开，最多 max 条，返回条数。
 * out[i] 容量须 ≥ 16 字节（4 个汉字 12 字节 + NUL）。 */
int ime_split_cands(const char *cands, char out[][16], int max);

/* 标准拼音音节表（无声调，含 lv/nv/lue/nue 这类 v 拼 ü 的写法）。
 * is_syl 判断 [s, s+n) 是否为合法音节；segment 用最长匹配把整串
 * 拼音切成音节（如 nihao→ni,hao），切不完返回 -1。 */
int ime_is_syllable(const char *s, int n);
int ime_segment(const char *py, char segs[][8], int max_segs);

#endif
