/*
 * mini.hpp - 各模块共享的声明
 *
 * 模块划分:
 *   mini.hpp    本文件(常量 / 结构体 / 类声明)
 *   utf8.cpp    UTF-8 编解码 / 字符宽度 / 行列换算
 *   buffer.cpp  文本缓冲区模型(整文件存为单个 std::string, 以 \n 分行) + 撤销重做 + 编辑操作
 *   display.cpp 终端原始模式 / 窗口尺寸 / 按键读取 / 渲染
 *   keys.cpp    快捷键逻辑 / 移动 / 命令分发
 *   main.cpp    程序入口 / 主循环
 *
 * 构建:
 *   g++ *.cpp -o mini.exe
 */

#ifndef MINI_HPP
#define MINI_HPP

#define _XOPEN_SOURCE 700
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>

#ifdef _WIN32
# include <windows.h>
# include <io.h>
# include <fcntl.h>
# include <conio.h>
# ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#  define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
# endif
# ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#  define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
# endif
# if defined(__MINGW32__) && __has_include(<termios.h>)
#  define HAVE_POSIX_TTY 1
#  include <unistd.h>
#  include <termios.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <signal.h>
# endif
#else
# include <unistd.h>
# include <termios.h>
# include <poll.h>
# include <sys/ioctl.h>
# include <signal.h>
#endif

/* 按键返回值: 负数=控制码; 正数=UTF-8 字符长度(字符存入 seq) */
enum {
    KEY_NONE = -1, KEY_ESC = -2, KEY_ENTER = -3, KEY_BACK = -4, KEY_INT = -5,
    KEY_UP = -10, KEY_DOWN = -11, KEY_LEFT = -12, KEY_RIGHT = -13,
    KEY_PGUP = -14, KEY_PGDN = -15, KEY_HOME = -16, KEY_END = -17, KEY_DEL = -18
};

enum PromptMode { P_NONE, P_FIND, P_FINDREV, P_GOTO, P_SAVEAS };

struct Cursor { int row = 0, col = 0; };

/* (r1,c1) < (r2,c2) ? */
int cmp_rc(int r1, int c1, int r2, int c2);

/* ---------------- UTF-8 工具 ---------------- */
int  utf8_len(unsigned char c);
int  utf8_decode(const char* s, int* cp);
int  cp_width(int cp);
int  utf8_nchars(const char* s, int nbytes);
int  char_count(const std::string& s);
int  byte_off(const std::string& s, int ci);
int  line_wrap_rows(const std::string& s, int cols);

/* ---------------- 文本缓冲区(整文件单字符串模型) ----------------
 * 不追求大文件/懒加载: 整个文档常驻内存于一个 std::string, 行以 '\n' 分隔。
 * 不变式: 非空文档 text 必以 '\n' 结尾(空文档 text == "")。
 * 这样插入/删除/跨行合并都退化为 std::string 的 insert/erase, 不再有拆行/合行样板。 */
class Buffer {
public:
    struct State {
        std::string text;
        Cursor cur, sel;
        bool has_sel;
    };
private:
    std::string text;
    void normalize();                              /* 维持"非空文本以 \n 结尾"不变式 */
    void lineBounds(int row, int* pstart, int* pend) const;  /* 第 row 行在 text 中的字节区间 */
    int  rcToOffset(int row, int col) const;       /* (行,列)-> 绝对字节偏移 */
    void offsetToRC(int abs, int* prow, int* pcol) const;    /* 绝对字节偏移 -> (行,列) */
public:
    Cursor cur, sel;          /* 光标与选区锚点(小项目直接暴露，等价于原 C 的全局 cur_row/sel_row) */
    bool has_sel = false;
    bool dirty = false;

    State snapshot() const { return {text, cur, sel, has_sel}; }
    void apply(const State& s) {
        text   = s.text;
        cur    = s.cur;
        sel    = s.sel;
        has_sel = s.has_sel;
        clampCursor();
    }

    int nlines() const;
    std::string lineStr(int r) const;
    void setLine(int r, const std::string& s);      /* 替换第 r 行内容(不含换行) */
    void insertLine(int at, const std::string& s);  /* 在第 at 行之后插入一行 */
    void removeLine(int at);                        /* 删除第 at 行 */

    int  charCount(int row) const;
    int  byteOff(int row, int ci) const;
    int  wrapRows(int row, int cols) const;
    bool inSel(int L, int ci) const;

    void clampCursor();
    void moveCursor(int dr, int dc, bool extend);

    void insertText(const std::string& text);
    void deleteBefore();
    void deleteAfter();
    std::string getSelectionText() const;
    void deleteSelection();

    void load(const std::string& path);
    bool save(const std::string& path);
};

/* ---------------- 编辑器(全局状态) ---------------- */
struct Snapshot { Buffer::State buf; };
static const int HIST_MAX = 300;

class Editor {
public:
    Buffer buffer;
    int mode = 0;                       /* 0=编辑 1=命令 */
    std::string filename;
    std::string clip;
    std::string status_msg;

    int cols = 80, rows = 24;
    int top_line = 0;
    int resize_flag = 0, running = 1;
    int kbd_shift = 0;
    int real_console = 0;               /* 1=真实 Windows 控制台; 0=mintty/pty/管道 */

    PromptMode prompt_mode = P_NONE;
    std::string prompt_buf;

    std::vector<Snapshot> history;
    int hist_len = 0, hist_pos = -1, last_coalesce = 0;

    /* 终端 */
    void enableRaw();
    void disableRaw();
    void getWinsize();
    int  getKey(std::string& seq);
    void render();
    void ensureVisible();

    /* 编辑/命令 */
    void pushSnapshot(bool coalesce);
    void undo();
    void redo();

    void handlePrompt(int key, const std::string& seq);
    bool handleMovement(char ch);
    bool tryMovementKey(int key);
    void handleCommand(char ch);
    void handleKey(int key, const std::string& seq);

    void cmdSelectAll();
    void cmdCopy();
    void cmdCut();
    void cmdPaste();
    void cmdCase(bool upper);
    void cmdDupLine();
    void cmdDelLine();
    void cmdSave();
    void cmdSaveAs();
    void cmdClose();
    void doFind(bool reverse);

    void open(const std::string& path);   /* 载入文件或新建空白文档 */
};

extern Editor ed;

#endif /* MINI_HPP */
