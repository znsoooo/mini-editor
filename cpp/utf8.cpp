/* utf8.cpp - UTF-8 编解码、字符宽度、行列换算 */

#include "mini.hpp"

int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

/* 解码一个 UTF-8 字符，返回字节长度，码点写入 *cp */
int utf8_decode(const char* s, int* cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c >> 5) == 0x6) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c >> 4) == 0xE) { *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c >> 3) == 0x1E) { *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *cp = c; return 1;
}

/* 一个 Unicode 码点的显示宽度(CJK 等双宽=2)。自带实现，不依赖 wcwidth(Windows 没有)。 */
int cp_width(int cp) {
    if (cp < 0x20) return 0;
    /* 东亚宽/全宽字符：Hangul、CJK、全角符号、Emoji 等 */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

/* 前 nbytes 字节内包含的 UTF-8 字符数 */
int utf8_nchars(const char* s, int nbytes) {
    int i = 0, c = 0;
    while (i < nbytes) { i += utf8_len((unsigned char)s[i]); c++; }
    return c;
}

/* 行的字符数 */
int char_count(const std::string& s) {
    int i = 0, c = 0, n = (int)s.size();
    while (i < n) { i += utf8_len((unsigned char)s[i]); c++; }
    return c;
}

/* 行中第 ci 个字符(按字符计)的字节偏移 */
int byte_off(const std::string& s, int ci) {
    int i = 0, c = 0, n = (int)s.size();
    while (i < n && c < ci) { i += utf8_len((unsigned char)s[i]); c++; }
    return i;
}

/* 行在给定列宽下的"软换行"屏幕行数 */
int line_wrap_rows(const std::string& s, int cols) {
    int w = 0, rows = 1, i = 0, n = (int)s.size();
    while (i < n) {
        int cl = utf8_len((unsigned char)s[i]);
        int cp; utf8_decode(s.c_str() + i, &cp);
        int cw = cp_width(cp);
        if (w + cw > cols && w > 0) { rows++; w = cw; }
        else w += cw;
        i += cl;
    }
    return rows;
}
