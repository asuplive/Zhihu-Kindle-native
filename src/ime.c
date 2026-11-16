/* 1.3.0：拼音输入法核心——码表查找、音节切分。
 * 键盘与候选栏的绘制/交互在 ui.c（draw_input / handle_tap SCR_INPUT）。
 * 码表数据来自 KOReader 中文键盘的同一份码表（Rime 简体拼音，Apache-2.0），
 * Kindle 原生程序调不了系统输入法（framework 的 vkbd 不对全屏 fb 程序开放），
 * 这是 KOReader 在真机上验证多年的同源方案。 */
#include "ime.h"

#include <string.h>

/* 标准普通话音节表（无声调）。排序存放，可二分也可线性。 */
static const char *SYL[] = {
    "a", "ai", "an", "ang", "ao",
    "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian",
    "biao", "bie", "bin", "bing", "bo", "bu",
    "ca", "cai", "can", "cang", "cao", "ce", "cen", "ceng", "cha", "chai",
    "chan", "chang", "chao", "che", "chen", "cheng", "chi", "chong", "chou",
    "chu", "chua", "chuan", "chuang", "chui", "chun", "chuo", "ci", "cong",
    "cou", "cu", "cuan", "cui", "cun", "cuo",
    "da", "dai", "dan", "dang", "dao", "de", "dei", "den", "deng", "di",
    "dia", "dian", "diao", "die", "ding", "diu", "dong", "dou", "du", "duan",
    "dui", "dun", "duo",
    "e", "ei", "en", "eng", "er",
    "fa", "fan", "fang", "fei", "fen", "feng", "fo", "fou", "fu",
    "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong",
    "gou", "gu", "gua", "guai", "guan", "guang", "gui", "gun", "guo",
    "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng", "hong",
    "hou", "hu", "hua", "huai", "huan", "huang", "hui", "hun", "huo",
    "ji", "jia", "jian", "jiang", "jiao", "jie", "jin", "jing", "jiong",
    "jiu", "ju", "juan", "jue", "jun",
    "ka", "kai", "kan", "kang", "kao", "ke", "kei", "ken", "keng", "kong",
    "kou", "ku", "kua", "kuai", "kuan", "kuang", "kui", "kun", "kuo",
    "la", "lai", "lan", "lang", "lao", "le", "lei", "leng", "li", "lia",
    "lian", "liang", "liao", "lie", "lin", "ling", "liu", "lo", "long",
    "lou", "lu", "luan", "lun", "luo", "lv", "lue",
    "ma", "mai", "man", "mang", "mao", "me", "mei", "men", "meng", "mi",
    "mian", "miao", "mie", "min", "ming", "miu", "mo", "mou", "mu",
    "na", "nai", "nan", "nang", "nao", "ne", "nei", "nen", "neng", "ni",
    "nian", "niang", "niao", "nie", "nin", "ning", "niu", "nong", "nou",
    "nu", "nuan", "nuo", "nv", "nue",
    "o", "ou",
    "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng", "pi", "pian",
    "piao", "pie", "pin", "ping", "po", "pou", "pu",
    "qi", "qia", "qian", "qiang", "qiao", "qie", "qin", "qing", "qiong",
    "qiu", "qu", "quan", "que", "qun",
    "ran", "rang", "rao", "re", "ren", "reng", "ri", "rong", "rou", "ru",
    "rua", "ruan", "rui", "run", "ruo",
    "sa", "sai", "san", "sang", "sao", "se", "sen", "seng", "sha", "shai",
    "shan", "shang", "shao", "she", "shei", "shen", "sheng", "shi", "shou",
    "shu", "shua", "shuai", "shuan", "shuang", "shui", "shun", "shuo", "si",
    "song", "sou", "su", "suan", "sui", "sun", "suo",
    "ta", "tai", "tan", "tang", "tao", "te", "teng", "ti", "tian", "tiao",
    "tie", "ting", "tong", "tou", "tu", "tuan", "tui", "tun", "tuo",
    "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu",
    "xi", "xia", "xian", "xiang", "xiao", "xie", "xin", "xing", "xiong",
    "xiu", "xu", "xuan", "xue", "xun",
    "ya", "yan", "yang", "yao", "ye", "yi", "yin", "ying", "yo", "yong",
    "you", "yu", "yuan", "yue", "yun",
    "za", "zai", "zan", "zang", "zao", "ze", "zei", "zen", "zeng", "zha",
    "zhai", "zhan", "zhang", "zhao", "zhe", "zhen", "zheng", "zhi", "zhong",
    "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui", "zhun", "zhuo",
    "zi", "zong", "zou", "zu", "zuan", "zui", "zun", "zuo",
};
#define SYL_N ((int)(sizeof(SYL) / sizeof(SYL[0])))

const ImeEntry *ime_find(const char *py) {
    int lo = 0, hi = ime_table_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(ime_table[mid].py, py);
        if (c == 0) return &ime_table[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int ime_split_cands(const char *cands, char out[][16], int max) {
    int n = 0;
    if (!cands) return 0;
    const char *p = cands;
    while (*p && n < max) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < 16) {
            memcpy(out[n], p, len);
            out[n][len] = 0;
            n++;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

int ime_is_syllable(const char *s, int n) {
    if (n <= 0 || n >= 8) return 0;
    char buf[8];
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    for (int i = 0; i < SYL_N; i++)
        if (strcmp(SYL[i], buf) == 0) return 1;
    return 0;
}

/* 最长匹配切分音节：nihao → ni + hao。切不完全返回 -1。 */
int ime_segment(const char *py, char segs[][8], int max_segs) {
    int n = (int)strlen(py);
    int pos = 0, ns = 0;
    while (pos < n) {
        int matched = 0;
        for (int len = 6; len >= 1; len--) {
            if (pos + len > n) continue;
            if (ime_is_syllable(py + pos, len)) {
                if (ns >= max_segs) return -1;
                memcpy(segs[ns], py + pos, (size_t)len);
                segs[ns][len] = 0;
                ns++;
                pos += len;
                matched = 1;
                break;
            }
        }
        if (!matched) return -1;
    }
    return ns;
}
