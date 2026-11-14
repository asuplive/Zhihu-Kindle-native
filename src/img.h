/* img.h —— 正文图片：JPEG 解码（灰度）+ 本地缓存
 *
 * 为什么要自己写解码器：Kindle 上没有 libjpeg，静态链接的程序里也带不了
 * 系统库；而 1.2.5 的「显示图片」只往正文里插一行「[图片]」文本，
 * 用户开了开关也看不到一张真图。
 *
 * 这里只实现 e-ink 真正需要的部分：
 *   - 只要亮度 Y（电子墨水本来就是 8bit 灰度），色度块解码后直接丢弃；
 *   - 基线 / 扩展顺序 JPEG（SOF0/SOF1），渐进式（SOF2）明确拒绝；
 *   - 支持 DCT 缩放（1/2、1/4、1/8），大图不必按原尺寸展开，省内存也快；
 *   - 解码后按目标宽度做盒式缩小，结果落到 K:\zhihu\cache\img\。
 */
#ifndef ZH_IMG_H
#define ZH_IMG_H

#include <stddef.h>
#include <stdint.h>

/* 图片统一按这个宽度缓存（而不是按当前正文宽）：
 * 用户改字号/页边距后正文宽会变，按正文宽做 key 会让缓存全部失效、
 * 图片变成"[图片]（未能加载）"。固定 1024 再缩放贴图，视觉上看不出差别。 */
#define IMG_CACHE_W 1024

typedef struct {
    int      w, h;
    uint8_t *px;      /* w*h 个 8bit 灰度样本 */
} ImgGray;

/* 解码成灰度图并按 target_w 缩小。
 * max_h > 0 时，高度也会压到不超过 max_h（宽度等比缩）。
 * 返回 0 成功；-1 失败，err 里写原因（"不是 JPEG"/"渐进式 JPEG 暂不支持"等）。
 * 1.2.9 起支持 JPEG / PNG / GIF / BMP（stb_image）/ WebP（libwebp），
 * 由文件头自动识别；超大图（>2000 万像素）明确拒绝，防止低内存设备卡死。 */
int  img_decode_gray(const uint8_t *data, size_t len, int target_w, int max_h,
                     ImgGray *out, char *err, int errsz);
void img_free(ImgGray *g);

/* ---- 缓存（<home>/cache/img/<文章子目录>/<md5>_<宽>.zhg） ----
 * 1.2.9：图片按"当前文章"分目录存放（子目录名 = 文章 key 的 md5 前 12 位），
 * 一篇文章一个夹子，清理/检查都直观；再次点击同一张图直接读盘，瞬时显示。
 * 兼容旧版平铺的 <md5>_<宽>.zhg：查缓存时先查子目录再查旧位置。 */
/* 设置当前文章（清空 key 或 NULL 即回到平铺目录）。 */
void img_cache_set_article(const char *key);
/* 统计 cache/img 下的缓存文件个数（含子目录，用于"清空缓存"提示）。 */
long img_cache_count(void);
/* 命中返回 0 并给出尺寸；未命中返回 -1。 */
int img_cache_lookup(const char *url, int w, int *ow, int *oh);
/* 命中则给出缓存文件路径与尺寸（绘制/排版要用）。返回 0 命中。 */
int img_cache_fill(const char *url, int w, char *path, size_t n, int *ow, int *oh);
/* 保存，返回 0 成功 */
int img_cache_save(const char *url, const ImgGray *g);
/* 读出整张图（绘制时用） */
int img_cache_load(const char *path, ImgGray *g);
/* 由缓存文件路径取尺寸（只读 12 字节头） */
int img_cache_dims(const char *path, int *w, int *h);

/* 自检：把内置的 8x8 测试 JPEG 解出来，核对宽高与几个采样点的灰度。
 * 返回 1 通过。用于把"解码器能不能跑"钉进 selftest（真机上没法调试）。 */
int img_selftest(void);

#endif /* ZH_IMG_H */
