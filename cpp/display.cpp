/* display.cpp - 终端原始模式、窗口尺寸、按键读取、渲染(显示构架) */

#include "mini.hpp"

/* ---------------- 终端状态备份 ---------------- */
#ifdef _WIN32
static DWORD g_orig_in_mode = 0;        /* 真实控制台的原始输入模式(退出时还原) */
#endif
#if !defined(_WIN32) || defined(HAVE_POSIX_TTY)
static struct termios g_orig_term;
#endif

/* ---------------- 终端原始模式 ---------------- */

#ifdef _WIN32
void Editor::enableRaw() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    /* 用文件类型区分: mintty/MSYS 的 pty 是管道(FILE_TYPE_PIPE), 真实控制台是字符设备(FILE_TYPE_CHAR).
       GetConsoleMode 在 ConPTY 下也会"成功", 故不能用来判断, 否则又走回不工作的 Win32 路径. */
    real_console = (GetFileType(hIn) == FILE_TYPE_CHAR);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);               /* 关闭 \r\n 翻译, 按字节解析转义序列 */

    if (real_console) {
        /* 真实 Windows 控制台: 用 Win32 控制台 API 关闭回显/行编辑/信号, 启用虚拟终端 */
        GetConsoleMode(hIn, &g_orig_in_mode);
        DWORD inm = g_orig_in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        inm |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(hIn, inm);
        DWORD outm; GetConsoleMode(hOut, &outm);
        SetConsoleMode(hOut, outm | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        /* 闪烁竖线光标(插入模式光标, DECSCUSR=5) */
        printf("\x1b[?25h\x1b[5 q");
        fflush(stdout);
    } else {
        /* mintty / MSYS pty / 管道: 当作 Unix 终端, 用 termios 设原始模式 + 备用屏 */
#ifdef HAVE_POSIX_TTY
        tcgetattr(STDIN_FILENO, &g_orig_term);
        struct termios raw = g_orig_term;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);   /* 关闭回显/行编辑/Ctrl-C 信号 */
        raw.c_iflag &= ~(IXON | ICRNL);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
        /* 进入备用屏(退出后自动恢复启用前内容) + 闪烁竖线光标 */
        printf("\x1b[?1049h\x1b[?25h\x1b[5 q");
        fflush(stdout);
    }
}
void Editor::disableRaw() {
    if (real_console) {
        /* 真实控制台: 恢复原始模式, 清屏复位回到启用 mini 之前的状态 */
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (g_orig_in_mode) SetConsoleMode(hIn, g_orig_in_mode);
        printf("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
        fflush(stdout);
    } else {
        /* pty: 退出备用屏, 恢复启用前内容 */
#ifdef HAVE_POSIX_TTY
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_term);
#endif
        printf("\x1b[?25h\x1b[0 q\x1b[?1049l");
        fflush(stdout);
    }
}
void Editor::getWinsize() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(hOut, &ci)) {
        cols = ci.srWindow.Right - ci.srWindow.Left + 1;
        rows = ci.srWindow.Bottom - ci.srWindow.Top + 1;
        if (cols < 1) cols = ci.dwSize.X;
        if (rows < 1) rows = ci.dwSize.Y;
    }
#ifdef HAVE_POSIX_TTY
    /* pty 下若上面失败, 退回 ioctl(MSYS 提供) */
    if (cols < 1 || rows < 1) {
#ifdef TIOCGWINSZ
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            cols = ws.ws_col; rows = ws.ws_row;
        }
#endif
    }
#endif
    if (cols < 1) cols = 80;
    if (rows < 4) rows = 24;   /* 至少 1 文本行 + 分隔符 + 状态行, 防止光标溢出到控制区 */
}
#else
/* 非 Windows: 标准 Unix 终端(POSIX) */
void Editor::disableRaw() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_term);
    printf("\x1b[?25h\x1b[0 q\x1b[?1049l");
    fflush(stdout);
}
void Editor::enableRaw() {
    tcgetattr(STDIN_FILENO, &g_orig_term);
    struct termios raw = g_orig_term;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);   /* 关闭回显/行编辑/信号, Ctrl-C 作为字节传入 */
    raw.c_iflag &= ~(IXON | ICRNL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    /* 进入备用屏幕；闪烁竖线光标(DECSCUSR=5) */
    printf("\x1b[?1049h\x1b[?25h\x1b[5 q");
    fflush(stdout);
}
void Editor::getWinsize() {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols = ws.ws_col; rows = ws.ws_row; }
    if (cols < 1) cols = 80;
    if (rows < 4) rows = 24;
}
#endif

/* ---------------- 按键读取(统一字节流 + CSI 解析) ---------------- */

#ifdef _WIN32
static int read_byte_blocking(int* out) {
#ifdef HAVE_POSIX_TTY
    if (!ed.real_console) {            /* pty: 直接用 POSIX read */
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;
        *out = c; return 0;
    }
#endif
    int c;
    if (_read(_fileno(stdin), &c, 1) != 1) return -1;
    *out = (unsigned char)c; return 0;
}
static int read_byte_try(int* out) {
#ifdef HAVE_POSIX_TTY
    if (!ed.real_console) {            /* pty: 用 poll 非阻塞探测 */
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&pfd, 1, 30) <= 0) return -1;
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;
        *out = c; return 0;
    }
#endif
    if (!_kbhit()) return -1;
    int c;
    if (_read(_fileno(stdin), &c, 1) != 1) return -1;
    *out = (unsigned char)c; return 0;
}
#else
static int read_byte_blocking(int* out) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    *out = c; return 0;
}
static int read_byte_try(int* out) {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pfd, 1, 30) <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    *out = c; return 0;
}
#endif

/* 把 CSI 序列(不含起始的 '[' 或 'O')解析为方向键等；设置 kbd_shift */
static int parse_csi(const char* b, int n) {
    int shift = 0;
    for (int i = 0; i + 1 < n; i++) if (b[i] == ';' && b[i + 1] == '2') shift = 1;
    ed.kbd_shift = shift;
    char fin = b[n - 1];
    if (fin == 'A') return KEY_UP;
    if (fin == 'B') return KEY_DOWN;
    if (fin == 'C') return KEY_RIGHT;
    if (fin == 'D') return KEY_LEFT;
    if (fin == 'H') return KEY_HOME;
    if (fin == 'F') return KEY_END;
    if (fin == '~' && n >= 2) {
        char p = b[n - 2];
        if (p == '1') return KEY_HOME;
        if (p == '4') return KEY_END;
        if (p == '5') return KEY_PGUP;
        if (p == '6') return KEY_PGDN;
        if (p == '3') return KEY_DEL;
    }
    return KEY_NONE;
}

/* 读取一个"键": 返回控制码(KEY_*)或 UTF-8 字符长度(字符存入 seq) */
int Editor::getKey(std::string& seq) {
    int c;
    if (read_byte_blocking(&c) != 0) return KEY_NONE;
    if (c == 27) {
        int n;
        if (read_byte_try(&n) == 0 && (n == '[' || n == 'O')) {
            int b;
            if (read_byte_blocking(&b) != 0) return KEY_ESC;
            char buf[16]; int bi = 0;
            buf[bi++] = (char)b;
            while (bi < 15) {
                if (b >= 0x40 && b <= 0x7E) break;
                if (read_byte_blocking(&b) != 0) break;
                buf[bi++] = (char)b;
            }
            int k = parse_csi(buf, bi);
            return (k == KEY_NONE) ? KEY_ESC : k;
        }
        return KEY_ESC;
    }
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == '\b' || c == 127) return KEY_BACK;
    if (c == 3) return KEY_INT;
    int len = utf8_len((unsigned char)c);
    seq.clear();
    seq.push_back((char)c);
    for (int i = 1; i < len; i++) { int d; if (read_byte_blocking(&d) != 0) break; seq.push_back((char)d); }
    return len;
}

/* ---------------- 渲染 ---------------- */

void Editor::ensureVisible() {
    int used = 0;
    for (int L = top_line; L < buffer.cur.row; L++) used += buffer.wrapRows(L, cols);
    while (used + buffer.wrapRows(buffer.cur.row, cols) > rows - 2) {
        if (top_line >= buffer.cur.row) break;
        used -= buffer.wrapRows(top_line, cols);
        top_line++;
    }
    if (buffer.cur.row < top_line) top_line = buffer.cur.row;
}

/* 计算逻辑光标(cur.row/cur.col)在屏幕上的(行,列)，与 render 的软换行逻辑一致。
   用于把硬件光标(闪烁竖线)定位到正确格位(插入点的左侧) */
static void cursor_screen_pos(const Editor& e, int* out_sr, int* out_sc) {
    int sr = 1;
    for (int L = e.top_line; L < e.buffer.nlines(); L++) {
        const std::string& s = e.buffer.lineStr(L);
        int w = 0, ci = 0, i = 0, col = 0, row_has = 0;
        while (i < (int)s.size()) {
            int cl = utf8_len((unsigned char)s[i]);
            int cp; utf8_decode(s.c_str() + i, &cp);
            int cw = cp_width(cp);
            if (row_has && w + cw > e.cols) {
                sr++; col = 0; w = cw; row_has = 1;
                if (sr > e.rows - 2) { *out_sr = e.rows - 2; *out_sc = 0; return; }
            } else { w += cw; row_has = 1; }
            if (L == e.buffer.cur.row && ci == e.buffer.cur.col) { *out_sr = sr; *out_sc = col; return; }
            col += cw; ci++; i += cl;
        }
        if (L == e.buffer.cur.row && ci == e.buffer.cur.col) { *out_sr = sr; *out_sc = col; return; }
        sr++;
    }
    *out_sr = (sr > e.rows - 2) ? e.rows - 2 : sr;
    *out_sc = 0;
}

void Editor::render() {
    ensureVisible();
    printf("\x1b[H");
    int sr = 1;
    for (int L = top_line; L < buffer.nlines() && sr <= rows - 2; L++) {
        const std::string& s = buffer.lineStr(L);
        int w = 0, ci = 0, seg_start = 0;
        int i = 0;
        while (i < (int)s.size()) {
            int cl = utf8_len((unsigned char)s[i]);
            int cp; utf8_decode(s.c_str() + i, &cp);
            int cw = cp_width(cp);
            if (w + cw > cols && w > 0) {
                /* 绘制本屏幕段 [seg_start, ci) */
                printf("\x1b[%d;1H\x1b[K", sr);
                int k = seg_start;
                while (k < ci) {
                    int bo = buffer.byteOff(L, k);
                    int kcl = utf8_len((unsigned char)s[bo]);
                    bool sel = buffer.inSel(L, k);
                    if (sel) printf("\x1b[7m");
                    fwrite(s.c_str() + bo, 1, kcl, stdout);
                    if (sel) printf("\x1b[0m");
                    k++;
                }
                sr++;
                if (sr > rows - 2) break;
                seg_start = ci; w = cw;
            } else { w += cw; }
            i += cl; ci++;
        }
        if (sr > rows - 2) break;
        /* 最后一段 */
        printf("\x1b[%d;1H\x1b[K", sr);
        int k = seg_start;
        while (k < ci) {
            int bo = buffer.byteOff(L, k);
            int kcl = utf8_len((unsigned char)s[bo]);
            bool sel = buffer.inSel(L, k);
            if (sel) printf("\x1b[7m");
            fwrite(s.c_str() + bo, 1, kcl, stdout);
            if (sel) printf("\x1b[0m");
            k++;
        }
        sr++;
    }
    /* 文本结束后的空行留白，紧贴分隔符(控制提示行) */
    while (sr <= rows - 2) {
        printf("\x1b[%d;1H\x1b[K", sr);
        sr++;
    }
    /* 分隔符行(倒数第二行): 纯 ASCII 减号 */
    printf("\x1b[%d;1H\x1b[K", rows - 1);
    for (int c = 0; c < cols; c++) putchar('-');
    /* 状态行(最后一行) */
    printf("\x1b[%d;1H\x1b[K", rows);
    const char* modestr = (mode == 0) ? "EDIT " : "COMMAND";
    if (prompt_mode == P_FIND) status_msg = "Find: " + prompt_buf;
    else if (prompt_mode == P_FINDREV) status_msg = "Find(rev): " + prompt_buf;
    else if (prompt_mode == P_GOTO) status_msg = "Goto line: " + prompt_buf;
    else if (prompt_mode == P_SAVEAS) status_msg = "Save as: " + prompt_buf;
    char line_info[64];
    snprintf(line_info, sizeof line_info, "L%d C%d", buffer.cur.row + 1, buffer.cur.col + 1);
    char fn[80]; snprintf(fn, sizeof fn, "%s", filename.empty() ? "[untitled]" : filename.c_str());
    printf("%s | %s%s | %s", modestr, buffer.dirty ? "*" : " ", fn, line_info);
    if (prompt_mode != P_NONE) printf("  > %s", prompt_buf.c_str());
    else if (!status_msg.empty()) printf("  | %s", status_msg.c_str());
    /* 硬件光标定位到逻辑光标所在的屏幕格(闪烁竖线) */
    int csr = 1, csc = 0;
    cursor_screen_pos(*this, &csr, &csc);
    if (csr < 1) csr = 1;
    if (csr > rows - 2) csr = rows - 2;
    printf("\x1b[%d;%dH", csr, csc + 1);
    fflush(stdout);
}
