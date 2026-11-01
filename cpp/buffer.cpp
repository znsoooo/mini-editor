/* buffer.cpp - 文本缓冲区模型、撤销重做、编辑操作
 * 整文件存为单个 std::string, 行以 '\n' 分隔。无大文件/懒加载需求, 故所有
 * 编辑操作都退化为 std::string 的 insert/erase, 不再有拆行/合行样板代码。 */
 
#include "mini.hpp"
#include <algorithm>

/* (r1,c1) < (r2,c2) ? */
int cmp_rc(int r1, int c1, int r2, int c2) {
    if (r1 != r2) return r1 - r2;
    return c1 - c2;
}

/* ---------------- 内部辅助 ---------------- */

/* 维持不变式: 非空文档必须以 '\n' 结尾(空文档保持 text=="") */
void Buffer::normalize() {
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
}

/* 第 row 行在 text 中的字节区间 [start, end) (end 落在 '\n' 上或文本末尾) */
void Buffer::lineBounds(int row, int* pstart, int* pend) const {
    int start = 0;
    for (int r = 0; r < row; r++) {
        start = (int)text.find('\n', start);
        if (start == (int)std::string::npos) { start = (int)text.size(); break; }
        start++;
    }
    int end = (int)text.find('\n', start);
    if (end == (int)std::string::npos) end = (int)text.size();
    *pstart = start; *pend = end;
}

/* (行, 列) -> 绝对字节偏移 */
int Buffer::rcToOffset(int row, int col) const {
    int s, e; lineBounds(row, &s, &e);
    return s + byte_off(text.substr(s, e - s), col);
}

/* 绝对字节偏移 -> (行, 列) */
void Buffer::offsetToRC(int abs, int* prow, int* pcol) const {
    int row = 0, n = nlines();
    while (row < n - 1) {
        int s2, e2; lineBounds(row + 1, &s2, &e2);
        if (abs < s2) break;
        row++;
    }
    int s, e; lineBounds(row, &s, &e);
    if (abs < s) abs = s;
    if (abs > e) abs = e;
    *prow = row;
    *pcol = char_count(text.substr(s, abs - s));
}

/* ---------------- 行列换算 ---------------- */

int  Buffer::nlines() const {
    if (text.empty()) return 1;
    return (int)std::count(text.begin(), text.end(), '\n');
}
std::string Buffer::lineStr(int r) const {
    int s, e; lineBounds(r, &s, &e);
    return text.substr(s, e - s);
}
int  Buffer::charCount(int row) const { return char_count(lineStr(row)); }
int  Buffer::byteOff(int row, int ci) const { return ::byte_off(lineStr(row), ci); }
int  Buffer::wrapRows(int row, int cols) const { return line_wrap_rows(lineStr(row), cols); }

void Buffer::clampCursor() {
    if (cur.row < 0) cur.row = 0;
    if (cur.row >= nlines()) cur.row = nlines() - 1;
    if (cur.row < 0) cur.row = 0;
    int cc = charCount(cur.row);
    if (cur.col < 0) cur.col = 0;
    if (cur.col > cc) cur.col = cc;
}

bool Buffer::inSel(int L, int ci) const {
    if (!has_sel) return false;
    int sr = sel.row, sc = sel.col, er = cur.row, ec = cur.col;
    if (cmp_rc(sr, sc, er, ec) > 0) { int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t; }
    return cmp_rc(sr, sc, L, ci) <= 0 && cmp_rc(L, ci, er, ec) <= 0;
}

void Buffer::moveCursor(int dr, int dc, bool extend) {
    if (extend && !has_sel) { sel.row = cur.row; sel.col = cur.col; has_sel = true; }
    if (!extend) has_sel = false;
    cur.row += dr; cur.col += dc;
    clampCursor();
}

/* ---------------- 行级操作(供命令模式使用) ---------------- */

void Buffer::setLine(int r, const std::string& s) {
    int ls, le; lineBounds(r, &ls, &le);
    text.replace(ls, le - ls, s);
    normalize();
}
void Buffer::insertLine(int at, const std::string& s) {
    /* 语义同原版: 在第 at 行处插入(原第 at 行被下推) */
    if (at <= 0) {
        text.insert(0, s + "\n");
    } else {
        int ls, le; lineBounds(at - 1, &ls, &le);  /* le = 上一行末尾的 '\n' */
        text.insert(le + 1, s + "\n");             /* 插在该 '\n' 之后, 成为新的一行 */
    }
    normalize();
}
void Buffer::removeLine(int at) {
    int s, e; lineBounds(at, &s, &e);
    int e2; { int d1, d2; lineBounds(at + 1, &d1, &d2); e2 = d1; }  /* 下一行的起始(即本行 '\n' 之后) */
    text.erase(s, e2 - s);   /* 删除整行(含其结尾的 '\n') */
    normalize();
}

/* ---------------- 文本编辑 ---------------- */

/* 在光标处插入字符串(可含 \n); UTF-8 整体作为一个字符处理 */
void Buffer::insertText(const std::string& ins) {
    int ls, le; lineBounds(cur.row, &ls, &le);
    const std::string oline = text.substr(ls, le - ls);
    int old_line_chars = char_count(oline);
    int bo = ls + byte_off(oline, cur.col);      /* 光标处的绝对字节偏移 */

    text.insert(bo, ins);

    int nl = (int)std::count(ins.begin(), ins.end(), '\n');
    if (nl == 0) {
        cur.col += utf8_nchars(ins.c_str(), (int)ins.size());
    } else {
        int last_nl = (int)ins.rfind('\n');
        int last_seg_chars = utf8_nchars(ins.c_str() + last_nl + 1,
                                         (int)ins.size() - last_nl - 1);
        int suffix_chars = old_line_chars - cur.col;   /* 原光标右侧的字符数 */
        cur.row += nl;
        cur.col = last_seg_chars + suffix_chars;
    }
    dirty = true;
    normalize();
    clampCursor();
}

/* 删除光标前(左)一个字符/换行 */
void Buffer::deleteBefore() {
    if (cur.row == 0 && cur.col == 0) return;   /* 文档最开头, 无内容可删 */
    int ls, le; lineBounds(cur.row, &ls, &le);
    if (cur.col > 0) {
        int bo = ls + byte_off(text.substr(ls, le - ls), cur.col);
        /* 回退到前一个字符的首字节(不能用 utf8_len 末字节, 否则 CJK 续字节会算成 1) */
        int pb = bo - 1;
        while (pb > ls && ((unsigned char)text[pb] & 0xC0) == 0x80) pb--;
        text.erase(pb, bo - pb);
        cur.col--;
    } else {
        /* 行首: 删掉上行末尾的 '\n', 与上一行合并 */
        text.erase(ls - 1, 1);
        cur.row--;
        cur.col = charCount(cur.row);   /* 合并后置于本行末尾(与原 C 版行为一致) */
    }
    dirty = true;
    normalize();
    clampCursor();
}

/* 删除光标后(右)一个字符/换行 */
void Buffer::deleteAfter() {
    int cc = charCount(cur.row);
    if (cur.col < cc) {
        int ls, le; lineBounds(cur.row, &ls, &le);
        int bo = ls + byte_off(text.substr(ls, le - ls), cur.col);
        int cl = utf8_len((unsigned char)text[bo]);
        text.erase(bo, cl);
    } else if (cur.row < nlines() - 1) {
        /* 行尾且非末行: 删掉本行末尾的 '\n', 与下一行合并 */
        int ls, le; lineBounds(cur.row, &ls, &le);
        text.erase(le, 1);
    }
    dirty = true;
    normalize();
    clampCursor();
}

std::string Buffer::getSelectionText() const {
    int sr = sel.row, sc = sel.col, er = cur.row, ec = cur.col;
    if (cmp_rc(sr, sc, er, ec) > 0) { int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t; }
    int a = rcToOffset(sr, sc), b = rcToOffset(er, ec);
    if (a > b) { int t = a; a = b; b = t; }
    return text.substr(a, b - a);
}

void Buffer::deleteSelection() {
    int sr = sel.row, sc = sel.col, er = cur.row, ec = cur.col;
    if (cmp_rc(sr, sc, er, ec) > 0) { int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t; }
    int a = rcToOffset(sr, sc), b = rcToOffset(er, ec);
    if (a > b) { int t = a; a = b; b = t; }
    text.erase(a, b - a);
    offsetToRC(a, &cur.row, &cur.col);
    has_sel = false;
    dirty = true;
    normalize();
    clampCursor();
}

/* ---------------- 文件读写 ---------------- */

void Buffer::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    text.clear();
    if (!f) { dirty = false; return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::string data((size_t)(sz > 0 ? sz : 0), '\0');
    if (sz > 0) fread(&data[0], 1, (size_t)sz, f);
    fclose(f);
    /* 整文件读入: 把 CRLF('\r\n') 归一成 LF, 其余原样保留 */
    for (long i = 0; i < sz; i++) {
        if (data[i] == '\r' && i + 1 < sz && data[i + 1] == '\n') continue;  /* 跳过 \r */
        text.push_back(data[i]);
    }
    /* 文件末尾无换行时补一个, 维持不变式 */
    if (text.empty() || text.back() != '\n') text.push_back('\n');
    dirty = false;
    clampCursor();
}

bool Buffer::save(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);   /* text 已含全部 '\n' 分隔与末尾换行 */
    fclose(f);
    dirty = false;
    return true;
}
