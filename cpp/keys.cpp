/* keys.cpp - 编辑器状态、快捷键逻辑、移动/命令分发 */

#include "mini.hpp"

/* ---------------- 撤销/重做 ---------------- */

void Editor::pushSnapshot(bool coalesce) {
    Snapshot s; s.buf = buffer.snapshot();
    if (coalesce && hist_len > 0 && last_coalesce) {
        history[hist_pos] = s;                       /* 合并为同一步(旧快照自动释放) */
    } else {
        while (hist_len - 1 > hist_pos) { history.pop_back(); hist_len--; }
        if (hist_len >= HIST_MAX) { history.erase(history.begin()); hist_len--; }
        history.push_back(s);
        hist_len++; hist_pos = hist_len - 1;
    }
    last_coalesce = coalesce;
}

void Editor::undo() { if (hist_pos > 0) { hist_pos--; buffer.apply(history[hist_pos].buf); last_coalesce = 0; } }
void Editor::redo() { if (hist_pos < hist_len - 1) { hist_pos++; buffer.apply(history[hist_pos].buf); last_coalesce = 0; } }

/* ---------------- 提示输入子模式(F/G/S) ---------------- */

void Editor::handlePrompt(int key, const std::string& seq) {
    if (key == KEY_ESC) { prompt_mode = P_NONE; return; }
    if (key == KEY_ENTER) {
        if (prompt_mode == P_FIND) doFind(false);
        else if (prompt_mode == P_FINDREV) doFind(true);
        else if (prompt_mode == P_GOTO) {
            int ln = atoi(prompt_buf.c_str());
            if (ln >= 1 && ln <= buffer.nlines()) {
                buffer.cur.row = ln - 1; buffer.cur.col = 0; buffer.has_sel = false; buffer.clampCursor();
            }
            prompt_mode = P_NONE;
        } else if (prompt_mode == P_SAVEAS) cmdSaveAs();
        return;
    }
    if (key == KEY_BACK || key == KEY_DEL) {
        if (!prompt_buf.empty()) {
            int last = 0;
            for (int i = 0; i < (int)prompt_buf.size(); ) { last = i; i += utf8_len((unsigned char)prompt_buf[i]); }
            prompt_buf.resize(last);
        }
        return;
    }
    if (key > 0 && (int)prompt_buf.size() + key < 255) {
        prompt_buf.append(seq);    /* seq 已是本次按键的完整 UTF-8 字节 */
    }
}

/* ---------------- 移动/翻页(i/j/k/l/[ ]{ } 字母键) ---------------- */

bool Editor::handleMovement(char ch) {
    int sel = 0, dr = 0, dc = 0;
    switch (ch) {
        case 'i': case 'I': sel = (ch == 'I'); dr = -1; break;
        case 'k': case 'K': sel = (ch == 'K'); dr = 1; break;
        case 'j': case 'J': sel = (ch == 'J'); dc = -1; break;
        case 'l': case 'L': sel = (ch == 'L'); dc = 1; break;
        case '[': case '{': sel = (ch == '{'); dr = -(rows - 2); break;
        case ']': case '}': sel = (ch == '}'); dr = (rows - 2); break;
        default: return false;
    }
    buffer.moveCursor(dr, dc, sel != 0);
    last_coalesce = 0;
    return true;
}

/* 物理方向键/Home/End/PageUp/Down -> 移动(带 Shift 即选中)。返回 true 表示已消费 */
bool Editor::tryMovementKey(int key) {
    int dr = 0, dc = 0, sel = kbd_shift;
    switch (key) {
        case KEY_UP:    dr = -1; break;
        case KEY_DOWN:  dr = 1; break;
        case KEY_LEFT:  dc = -1; break;
        case KEY_RIGHT: dc = 1; break;
        case KEY_PGUP:  dr = -(rows - 2); break;
        case KEY_PGDN:  dr = (rows - 2); break;
        case KEY_HOME:
            if (sel && !buffer.has_sel) { buffer.sel.row = buffer.cur.row; buffer.sel.col = buffer.cur.col; buffer.has_sel = true; }
            buffer.cur.col = 0; if (!sel) buffer.has_sel = false;
            buffer.clampCursor(); last_coalesce = 0; return true;
        case KEY_END:
            if (sel && !buffer.has_sel) { buffer.sel.row = buffer.cur.row; buffer.sel.col = buffer.cur.col; buffer.has_sel = true; }
            buffer.cur.col = buffer.charCount(buffer.cur.row); if (!sel) buffer.has_sel = false;
            buffer.clampCursor(); last_coalesce = 0; return true;
        default: return false;
    }
    buffer.moveCursor(dr, dc, sel != 0);
    last_coalesce = 0;
    return true;
}

/* ---------------- 命令分发(命令模式) ---------------- */

void Editor::handleCommand(char ch) {
    int up = isupper((unsigned char)ch);
    char lc = (char)tolower((unsigned char)ch);
    switch (lc) {
        case 'a': cmdSelectAll(); break;
        case 'x': cmdCut(); break;
        case 'c': cmdCopy(); break;
        case 'v': cmdPaste(); break;
        case 'z': if (up) redo(); else undo(); break;
        case 'f': prompt_mode = up ? P_FINDREV : P_FIND; prompt_buf.clear(); break;
        case 'g': prompt_mode = P_GOTO; prompt_buf.clear(); break;
        case 'u': cmdCase(up ? 1 : 0); break;
        case 'd': if (up) cmdDelLine(); else cmdDupLine(); break;
        case 's': if (up) { prompt_mode = P_SAVEAS; prompt_buf.clear(); } else cmdSave(); break;
        case 'w': cmdClose(); break;
        case 'q': if (up) { running = 0; } else { cmdClose(); } break;  /* Q=强制退出; q=退出(未保存会提示) */
        default: break;
    }
}

/* ---------------- 查找 ---------------- */

void Editor::doFind(bool reverse) {
    int qlen = (int)prompt_buf.size();
    if (qlen == 0) { prompt_mode = P_NONE; return; }
    const std::string& q = prompt_buf;
    if (reverse) {
        for (int L = buffer.cur.row; L >= 0; L--) {
            const std::string& line = buffer.lineStr(L);
            int cc = buffer.charCount(L);
            for (int ci = (L == buffer.cur.row ? buffer.cur.col - 1 : cc - 1); ci >= 0; ci--) {
                int bo = buffer.byteOff(L, ci);
                if (strncmp(line.c_str() + bo, q.c_str(), qlen) == 0) {
                    buffer.cur.row = L; buffer.cur.col = ci;
                    buffer.sel.row = L; buffer.sel.col = ci;
                    int endc = ci; for (int k = 0; k < qlen; ) { k += utf8_len((unsigned char)q[k]); endc++; }
                    buffer.cur.col = endc; buffer.has_sel = true;
                    status_msg = "Find: " + q;
                    prompt_mode = P_NONE; return;
                }
            }
        }
    } else {
        for (int L = buffer.cur.row; L < buffer.nlines(); L++) {
            const std::string& line = buffer.lineStr(L);
            int start = (L == buffer.cur.row) ? buffer.cur.col : 0;
            const char* found = strstr(line.c_str() + buffer.byteOff(L, start), q.c_str());
            if (found) {
                int ci = 0; const char* p = line.c_str();
                while (p < found) { p += utf8_len((unsigned char)*p); ci++; }
                buffer.cur.row = L; buffer.cur.col = ci;
                buffer.sel.row = L; buffer.sel.col = ci;
                int endc = ci; for (int k = 0; k < qlen; ) { k += utf8_len((unsigned char)q[k]); endc++; }
                buffer.cur.col = endc; buffer.has_sel = true;
                status_msg = "Find: " + q;
                prompt_mode = P_NONE; return;
            }
        }
    }
    status_msg = "No match: " + q;
    prompt_mode = P_NONE;
}

/* ---------------- 命令实现 ---------------- */

void Editor::cmdSelectAll() {
    buffer.sel.row = 0; buffer.sel.col = 0;
    buffer.cur.row = buffer.nlines() - 1; buffer.cur.col = buffer.charCount(buffer.cur.row);
    buffer.has_sel = true;
}
void Editor::cmdCopy() {
    if (!buffer.has_sel) { status_msg = "No selection"; return; }
    clip = buffer.getSelectionText();
    status_msg = "Copied";
}
void Editor::cmdCut() {
    if (!buffer.has_sel) { status_msg = "No selection"; return; }
    clip = buffer.getSelectionText();
    buffer.deleteSelection(); pushSnapshot(false);
    status_msg = "Cut";
}
void Editor::cmdPaste() {
    if (clip.empty()) { status_msg = "Clipboard empty"; return; }
    buffer.insertText(clip); pushSnapshot(false);
    status_msg = "Pasted";
}
void Editor::cmdCase(bool upper) {
    int sr, sc, er, ec;
    if (buffer.has_sel) {
        sr = buffer.sel.row; sc = buffer.sel.col; er = buffer.cur.row; ec = buffer.cur.col;
        if (cmp_rc(sr, sc, er, ec) > 0) { int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t; }
    } else { sr = er = buffer.cur.row; sc = 0; ec = buffer.charCount(buffer.cur.row); }
    for (int L = sr; L <= er; L++) {
        int cc = buffer.charCount(L);
        int a = (L == sr) ? sc : 0;
        int b = (L == er) ? ec : cc;
        std::string s = buffer.lineStr(L);
        int i = buffer.byteOff(L, a);
        int end = buffer.byteOff(L, b);
        while (i < end) {
            int cp; int cl = utf8_decode(s.c_str() + i, &cp);
            if (cl == 1) { s[i] = (char)(upper ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i])); }
            i += cl;
        }
        buffer.setLine(L, s);
    }
    pushSnapshot(false);
    status_msg = upper ? "Uppercased" : "Lowercased";
    buffer.dirty = true;
    buffer.clampCursor();
}
void Editor::cmdDupLine() {
    std::string dup = buffer.lineStr(buffer.cur.row);
    buffer.insertLine(buffer.cur.row + 1, dup);
    buffer.cur.row++; buffer.cur.col = 0;
    pushSnapshot(false);
    buffer.dirty = true; buffer.clampCursor();
    status_msg = "Line duplicated";
}
void Editor::cmdDelLine() {
    if (buffer.nlines() == 1) { buffer.setLine(0, ""); }
    else buffer.removeLine(buffer.cur.row);
    if (buffer.cur.row >= buffer.nlines()) buffer.cur.row = buffer.nlines() - 1;
    buffer.cur.col = 0; buffer.dirty = true; buffer.clampCursor();
    pushSnapshot(false);
    status_msg = "Line deleted";
}
void Editor::cmdSave() {
    if (filename.empty()) { prompt_mode = P_SAVEAS; prompt_buf.clear(); return; }
    if (buffer.save(filename)) status_msg = "Saved " + filename;
    else status_msg = "Save failed: " + filename;
}
void Editor::cmdSaveAs() {
    if (prompt_buf.empty()) { prompt_mode = P_NONE; return; }
    filename = prompt_buf;
    if (buffer.save(filename)) status_msg = "Saved " + filename;
    else status_msg = "Save failed: " + filename;
    prompt_mode = P_NONE;
}
void Editor::cmdClose() {
    if (buffer.dirty) {
        if (!filename.empty()) buffer.save(filename);
        else { prompt_mode = P_SAVEAS; prompt_buf.clear(); return; }
    }
    running = 0;
}

/* ---------------- 主按键分发 ---------------- */

void Editor::handleKey(int key, const std::string& seq) {
    if (prompt_mode != P_NONE) { handlePrompt(key, seq); return; }
    if (key == KEY_ESC) { mode = (mode == 0) ? 1 : 0; status_msg.clear(); return; }

    if (mode == 0) {
        /* 编辑模式 */
        if (key == KEY_ENTER) { buffer.insertText("\n"); pushSnapshot(false); last_coalesce = 0; }
        else if (key == KEY_BACK) {
            if (buffer.has_sel) { buffer.deleteSelection(); pushSnapshot(false); }
            else { buffer.deleteBefore(); pushSnapshot(false); }
            last_coalesce = 0;
        }
        else if (key == KEY_DEL) {
            if (buffer.has_sel) { buffer.deleteSelection(); pushSnapshot(false); }
            else { buffer.deleteAfter(); pushSnapshot(false); }
            last_coalesce = 0;
        }
        else if (tryMovementKey(key)) { /* 方向键已处理 */ }
        else if (key > 0) {
            if (!handleMovement(seq[0])) { buffer.insertText(seq); pushSnapshot(true); }
        }
    } else {
        /* 命令模式 */
        if (tryMovementKey(key)) { /* 方向键已处理 */ }
        else if (key > 0) {
            if (!handleMovement(seq[0])) handleCommand(seq[0]);
        }
    }
}
