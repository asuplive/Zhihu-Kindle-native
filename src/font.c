#include "font.h"
#include "common.h"

#include <dirent.h>
#include <stdlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define MAX_FACES 3
#define CACHE_SLOTS 4096

typedef struct {
    uint32_t key;
    int      px;
    int      bold;
    int      w, h;
    int      bx, by;      /* bitmap_left / bitmap_top */
    int      advance;
    uint8_t *bmp;
} Glyph;

static FT_Library g_lib;
static FT_Face    g_faces[MAX_FACES];
static int        g_nfaces;
static char       g_info[160];
static Glyph      g_cache[CACHE_SLOTS];
static int        g_cached;

static uint32_t hash32(uint32_t a, uint32_t b) {
    uint32_t h = a * 2654435761u ^ b * 2246822519u;
    return h ^ (h >> 15);
}

static void cache_clear(void) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        free(g_cache[i].bmp);
        g_cache[i].bmp = NULL;
        g_cache[i].key = 0;
    }
    g_cached = 0;
}

/* ---------------- 字体发现 ---------------- */

static int face_has(FT_Face f, uint32_t cp) {
    return FT_Get_Char_Index(f, cp) != 0;
}

/* .ttc 里通常同时打包日/繁/简，优先挑简体（SC/GB/Hans）那一张，
 * 否则"直""骨"这类字会按日文字形渲染 */
static int pick_collection_index(const char *path) {
    FT_Face probe = NULL;
    if (FT_New_Face(g_lib, path, 0, &probe) != 0) return 0;
    long total = probe->num_faces;
    int best = 0;
    if (total > 1) {
        for (long i = 0; i < total && i < 16; i++) {
            const char *fam = NULL;
            FT_Face f2 = NULL;
            if (i == 0) fam = probe->family_name;
            else if (FT_New_Face(g_lib, path, i, &f2) == 0) fam = f2->family_name;
            int hit = 0;
            if (fam) {
                if (strstr(fam, " SC") || strstr(fam, "SC ") || strstr(fam, "Hans") ||
                    strstr(fam, "GB") || strstr(fam, "CN") || strstr(fam, "Chinese"))
                    hit = 1;
            }
            if (f2) FT_Done_Face(f2);
            if (hit) { best = (int)i; break; }
        }
    }
    FT_Done_Face(probe);
    return best;
}

static int try_add_face(const char *path) {
    if (g_nfaces >= MAX_FACES) return 0;
    FT_Face f = NULL;
    if (FT_New_Face(g_lib, path, pick_collection_index(path), &f) != 0) return 0;
    if (f->num_glyphs < 32) { FT_Done_Face(f); return 0; }
    /* 至少要能显示拉丁或中文之一 */
    if (!face_has(f, 'A') && !face_has(f, 0x77E5)) { FT_Done_Face(f); return 0; }
    g_faces[g_nfaces++] = f;
    return 1;
}

static int scan_dir(const char *dir, int depth, int want_cjk_only) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_nfaces < MAX_FACES) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t n = strlen(e->d_name);
        int is_font = 0;
        if (n > 4 && (str_ieq(e->d_name + n - 4, ".ttf") || str_ieq(e->d_name + n - 4, ".otf") ||
                      str_ieq(e->d_name + n - 4, ".ttc")))
            is_font = 1;
        if (is_font) {
            if (want_cjk_only) {
                FT_Face f = NULL;
                if (FT_New_Face(g_lib, path, 0, &f) == 0) {
                    int ok = face_has(f, 0x77E5);
                    FT_Done_Face(f);
                    if (!ok) continue;
                } else continue;
            }
            added += try_add_face(path);
        } else if (depth > 0 && e->d_type == DT_DIR) {
            added += scan_dir(path, depth - 1, want_cjk_only);
        }
    }
    closedir(d);
    return added;
}

int font_init(char *err, int errsz) {
    (void)err;
    (void)errsz;
    if (FT_Init_FreeType(&g_lib) != 0) return -1;

    /* 0) 显式指定（调试/极端机型） */
    const char *forced = getenv("ZH_FONT");
    if (forced && file_exists(forced)) try_add_face(forced);

    /* 1) 用户自备字体优先 */
    static const char *user_fonts[] = {
        "/mnt/us/zhihu/font.ttf",
        "/mnt/us/zhihu/font.otf",
        "/mnt/us/fonts/font.ttf",
    };
    for (size_t i = 0; i < sizeof(user_fonts) / sizeof(user_fonts[0]); i++)
        if (file_exists(user_fonts[i])) try_add_face(user_fonts[i]);

    /* 2) 系统字体目录（Kindle 固件自带中文 TrueType）
     *    先只收含"知"字形的字体，避免西文字体先把槽位占满导致中文变方框 */
    static const char *sys_dirs[] = {
        "/usr/java/lib/fonts",
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        "/mnt/us/fonts",
        "/mnt/us/koreader/fonts",
        "/mnt/us/koreader/fonts/noto",
    };
    for (size_t i = 0; i < sizeof(sys_dirs) / sizeof(sys_dirs[0]) && g_nfaces < 3; i++)
        scan_dir(sys_dirs[i], 3, 1);
    /* 再补西文/界面字体 */
    for (size_t i = 0; i < sizeof(sys_dirs) / sizeof(sys_dirs[0]) && g_nfaces < MAX_FACES; i++)
        scan_dir(sys_dirs[i], 3, 0);

    if (g_nfaces == 0) {
        snprintf(err, errsz,
                 "找不到可用字体。请把一个中文 TTF 复制为 /mnt/us/zhihu/font.ttf 后重试。");
        return -1;
    }

    /* 记录主字体名，便于排查 */
    const char *fn = g_faces[0]->family_name ? g_faces[0]->family_name : "font";
    snprintf(g_info, sizeof(g_info), "%s", fn);

    /* 确认中文可显示，否则把信息写进日志 */
    int has_cjk = 0;
    for (int i = 0; i < g_nfaces; i++) if (face_has(g_faces[i], 0x77E5)) has_cjk = 1;
    if (!has_cjk) logmsg("font: 未找到含中文字形的字体，中文会显示为方框");
    return 0;
}

const char *font_info(void) { return g_info; }
int font_nfaces(void) { return g_nfaces; }

void font_done(void) {
    cache_clear();
    for (int i = 0; i < g_nfaces; i++) if (g_faces[i]) FT_Done_Face(g_faces[i]);
    g_nfaces = 0;
    if (g_lib) FT_Done_FreeType(g_lib);
    g_lib = NULL;
}

/* ---------------- 字形缓存 ---------------- */

static FT_Face pick_face(uint32_t cp) {
    for (int i = 0; i < g_nfaces; i++)
        if (FT_Get_Char_Index(g_faces[i], cp) != 0) return g_faces[i];
    return g_faces[0];
}

static void set_px(FT_Face f, int px) {
    /* 每个 face 记住当前字号，避免重复 Set */
    if (!f || !f->size) return;
    if ((int)f->size->metrics.x_ppem != px) FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px);
}

static Glyph *get_glyph(uint32_t cp, int px, int bold) {
    uint32_t key = hash32(cp, (uint32_t)(px * 131 + bold));
    if (key == 0) key = 1;
    int idx = (int)(key & (CACHE_SLOTS - 1));
    /* 探测整张表，不能只探 8 个槽：缓存用到七八成时，hash 冲突很容易
     * 连成 8 个连续占用槽，旧实现会直接返回 NULL，那个字就被静默跳过
     * （屏幕上是"知乎"变成" 乎"这种随机缺字），且随着阅读页数增加越容易复现。 */
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        Glyph *g = &g_cache[(idx + probe) & (CACHE_SLOTS - 1)];
        if (g->key == key && g->px == px && g->bold == bold) return g;
        if (g->key == 0) {
            /* 空位：生成 */
            FT_Face f = pick_face(cp);
            set_px(f, px);
            if (FT_Load_Char(f, cp, FT_LOAD_RENDER) != 0) return NULL;

            FT_GlyphSlot s = f->glyph;
            g->key = key;
            g->px = px;
            g->bold = bold;
            g->w = (int)s->bitmap.width;
            g->h = (int)s->bitmap.rows;
            g->bx = s->bitmap_left;
            g->by = s->bitmap_top;
            g->advance = (int)(s->advance.x >> 6);
            g->bmp = NULL;
            if (g->w > 0 && g->h > 0 && s->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                g->bmp = xmalloc((size_t)g->w * (size_t)g->h);
                for (int r = 0; r < g->h; r++)
                    memcpy(g->bmp + (size_t)r * (size_t)g->w,
                           s->bitmap.buffer + (size_t)r * (size_t)s->bitmap.pitch, (size_t)g->w);
                if (bold) {
                    /* 加粗：横向膨胀一像素（简单有效，不额外加载 Bold 字重） */
                    for (int r = 0; r < g->h; r++) {
                        uint8_t *row = g->bmp + (size_t)r * (size_t)g->w;
                        for (int c = g->w - 1; c > 0; c--) {
                            int v = row[c] + row[c - 1];
                            row[c] = (uint8_t)(v > 255 ? 255 : v);
                        }
                    }
                }
            }
            if (g->w > 0 && g->h > 0) g_cached++;
            if (g_cached > CACHE_SLOTS * 3 / 4) {
                /* 缓存满：整体清空重来。
                 * 注意 cache_clear() 会把刚生成的这个 glyph 也一起 free 掉，
                 * 所以清完必须重新生成一次再返回，否则调用方拿到的是空壳，
                 * 那一行字会莫名消失。 */
                cache_clear();
                return get_glyph(cp, px, bold);
            }
            return g;
        }
    }
    /* 整张表都占满才走到这里：清空后重建一次 */
    cache_clear();
    return get_glyph(cp, px, bold);
}

/* ---------------- 对外 ---------------- */

int font_line_height(int px) { return px + px / 4; }   /* 1.25 倍行距基准 */
int font_ascent(int px) { return px; }
int font_descent(int px) { return px / 4; }

int font_width(int px, const char *s, int len) {
    int x = 0;
    int i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = u8_decode(s + i, (size_t)(len - i), &cp);
        if (n <= 0) break;
        i += n;
        Glyph *g = get_glyph(cp, px, 0);
        if (g) x += g->advance ? g->advance : px / 2;
        else x += px / 2;
    }
    return x;
}

void font_draw(Fb *fb, int x, int baseline, int px, const char *s, int len, int gray, int bold) {
    int pen = x;
    int i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = u8_decode(s + i, (size_t)(len - i), &cp);
        if (n <= 0) break;
        i += n;
        if (cp == '\n') continue;
        Glyph *g = get_glyph(cp, px, bold);
        if (!g) { pen += px / 2; continue; }
        if (g->bmp && g->w > 0 && g->h > 0) {
            /* 基线对齐：bitmap_top 是基线以上的高度 */
            int gx = pen + g->bx;
            int gy = baseline - g->by;
            fb_blit_gray(fb, gx, gy, g->w, g->h, g->bmp, gray);
        }
        pen += g->advance ? g->advance : px / 2;
    }
}

/* 自检：模拟长时间阅读（大量 字号×字符 组合把哈希表撑到高负载），
 * 期间任何一个字形取不到都算失败。修复前只探测 8 个槽，跑几百页后
 * 就会随机丢字（"知乎"变" 乎"），这条用例能把该回归挡住。 */
int font_cache_stress(void) {
    if (g_nfaces <= 0) return 0;
    int bad = 0;
    for (int px = 12; px <= 40; px++)
        for (uint32_t cp = 0x4E00; cp < 0x4F00; cp++)
            if (!get_glyph(cp, px, 0)) bad++;
    static const uint32_t keys[] = { 0x77E5, 0x4E4E, 0x9605, 0x8BFB, 0x79C1, 0x4FE1 };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!get_glyph(keys[i], 27, 0)) bad++;
    return bad == 0;
}
