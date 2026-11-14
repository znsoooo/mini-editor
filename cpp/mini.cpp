/* mini.cpp - mini-editor 的 C++17 版本(单文件, 无第三方依赖)

对照 python/mini.py 逐段移植: 类与函数名、变量名、大小写风格保持一致,
    DisplayWidth / IsWordChar
    Buffer    : 文本区域基础功能 - 光标移动/设置选区/基础增删, 不碰终端
    Commands  : 各个命令的实现逻辑 - 撤销重做/剪贴板/行操作/大小写/缩进/跳转/查找
    Editor    : 页面渲染 + 文件读写交互 + 命令分发(具体实现由 Commands 完成)
    Screen    : 顶替 Python 的 curses 窗口 - 自绘单元格 + ANSI 输出 + 原始按键
内部文本一律存为 UTF-32(std::u32string), 与 Python 的 str 一样按"字符"索引;
终端输出时才编码成 UTF-8, 因此列宽(CJK 占 2 格)由 DisplayWidth 统一决定。

屏幕后端两套, 同一份 Editor 代码通用:
    Win32 控制台(cmd / PowerShell / Windows Terminal)
        输出 WriteConsoleOutputW(逐格写 CHAR_INFO, 颜色用控制台属性),
        输入 ReadConsoleInputW(直接取按键记录)。不依赖 ANSI/VT, 老 conhost 也能跑,
        中文输入法不会被打断。
    其它终端(Linux / macOS / mintty)
        ANSI 转义 + termios 原始字节流。

两种模式:
    命令模式(默认) / 编辑模式
    按 Esc 切换

命令模式:
    I J K L [ ]  移动光标 (上/左/下/右/上翻页/下翻页)
    Shift+I/J/K/L/[/]  移动并选中

    A 全选  X 剪切  C 复制  V 粘贴
    Z 撤销  Shift+Z 重做
    F 查找  Shift+F 反向查找
    G 跳转  Shift+G 选中到跳转位置

    U 转小写  Shift+U 转大写
    D 重复行  Shift+D 删除行
    T 增加缩进  Shift+T 减少缩进
    B 向前删除  Shift+B 向后删除

    S 保存  Shift+S 另存
    W 关闭(未保存提示)
    Q 退出(未保存提示)  Shift+Q 不保存退出

编辑模式:
    打字插入
    Tab 增加缩进 Shift+Tab 减少缩进
    Enter 换行 Backspace/Delete 删除
    方向键/Home/End/PageUp/Down 移动

构建(见 build.bat):
    g++ -std=c++17 -O2 mini.cpp -o mini.exe
*/

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <conio.h>          /* _kbhit: 判断控制台输入队列里是否已有按键 */
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#  ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#    define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#  endif
#  ifndef ENABLE_QUICK_EDIT_MODE
#    define ENABLE_QUICK_EDIT_MODE 0x0040
#  endif
#  ifndef ENABLE_EXTENDED_FLAGS
#    define ENABLE_EXTENDED_FLAGS 0x0080
#  endif
#  if defined(__MINGW32__) && __has_include(<termios.h>)
#    include <termios.h>
#    include <unistd.h>
#    include <poll.h>
#    include <sys/ioctl.h>
#    define HAVE_POSIX_TTY 1
#  endif
#else
#  include <termios.h>
#  include <unistd.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#endif

/* ---------------------------------------------------------------------------
 * 常量: 属性 / 特殊键
 *
 * Python 版里 "普通字符" 是 str, "特殊键" 是 int, 用 isinstance(ch, int) 区分。
 * 这里统一用 char32_t 承载, 特殊键放在 Unicode 之外的高位段,
 * 用 IsKeyCode() 判定, 对应原来的 isinstance(ch, int)。
 * ------------------------------------------------------------------------- */
typedef char32_t Char;
typedef std::u32string Text;
typedef std::pair<int, int> Pos;

const int A_NORMAL = 0;
const int A_REVERSE = 1;

const Char KEY_ESC = 27;  /* get_wch 对单独的 Esc 返回 27 */
const Char KEY_CODE_BASE = 0x110000;
const Char KEY_UP = KEY_CODE_BASE + 1;
const Char KEY_DOWN = KEY_CODE_BASE + 2;
const Char KEY_LEFT = KEY_CODE_BASE + 3;
const Char KEY_RIGHT = KEY_CODE_BASE + 4;
const Char KEY_HOME = KEY_CODE_BASE + 5;
const Char KEY_END = KEY_CODE_BASE + 6;
const Char KEY_PPAGE = KEY_CODE_BASE + 7;
const Char KEY_NPAGE = KEY_CODE_BASE + 8;
const Char KEY_DC = KEY_CODE_BASE + 9;
const Char KEY_IC = KEY_CODE_BASE + 10;
const Char KEY_BACKSPACE = KEY_CODE_BASE + 11;
const Char KEY_BTAB = KEY_CODE_BASE + 12;
const Char KEY_RESIZE = KEY_CODE_BASE + 13;
const Char KEY_EOF = KEY_CODE_BASE + 14;

static bool IsKeyCode(Char ch) { return ch >= KEY_CODE_BASE; }

/* ---------------------------------------------------------------------------
 * 显示宽度(仅用于光标定位对齐; 渲染本身由 Screen 按列写入)
 * ------------------------------------------------------------------------- */
struct WidthRange { uint32_t first, last; };

/* 东亚宽(W)与全宽(F)字符区间, 等价于 unicodedata.east_asian_width(ch) in ('W','F') */
static const WidthRange kWideRanges[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6B},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FB}, {0x1F300, 0x1FAFF}, {0x20000, 0x3FFFD},
};

static int DisplayWidth(Char ch) {
    /* 单个字符在终端占用的列数: 制表符按 4, 中日韩全角字符按 2 */
    if (ch == U'\t') return 4;
    if (ch < 0x1100) return 1;
    for (const WidthRange& range : kWideRanges) {
        if (ch >= range.first && ch <= range.last) return 2;
        if (ch < range.first) break;
    }
    return 1;
}

static bool IsWordChar(Char ch) {
    /* 连续英文 + 数字组合判定(仅 ASCII, 对应 [A-Za-z0-9]) */
    return (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z') || (ch >= U'0' && ch <= U'9');
}

/* ---- 大小写(对应 str.upper()/str.lower(), 覆盖 ASCII / Latin-1 / 全角字母) ---- */
static Char ToUpper(Char ch) {
    if (ch >= U'a' && ch <= U'z') return ch - 32;
    if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32;   /* Latin-1 小写 */
    if (ch >= 0xFF41 && ch <= 0xFF5A) return ch - 32;             /* 全角小写字母 */
    if (ch >= 0x3B1 && ch <= 0x3C9 && ch != 0x3C2) return ch - 32; /* 希腊字母 */
    if (ch >= 0x430 && ch <= 0x44F) return ch - 32;               /* 西里尔字母 */
    if (ch >= 0x450 && ch <= 0x45F) return ch - 80;
    return ch;
}

static Char ToLower(Char ch) {
    if (ch >= U'A' && ch <= U'Z') return ch + 32;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32;   /* Latin-1 大写 */
    if (ch >= 0xFF21 && ch <= 0xFF3A) return ch + 32;             /* 全角大写字母 */
    if (ch >= 0x391 && ch <= 0x3A9 && ch != 0x3A2) return ch + 32; /* 希腊字母 */
    if (ch >= 0x410 && ch <= 0x42F) return ch + 32;               /* 西里尔字母 */
    if (ch >= 0x400 && ch <= 0x40F) return ch + 80;
    return ch;
}

static bool IsUpper(Char ch) { return ToLower(ch) != ch; }

static Text ToUpperText(const Text& text) {
    Text out;
    out.reserve(text.size());
    for (Char ch : text) out.push_back(ToUpper(ch));
    return out;
}

static Text ToLowerText(const Text& text) {
    Text out;
    out.reserve(text.size());
    for (Char ch : text) out.push_back(ToLower(ch));
    return out;
}

/* ---- UTF-8 编解码 ---- */
static int Utf8Length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

/* 解码一个 UTF-8 字符, 返回字节数, 码点写入 cp; 非法字节按原值返回 */
static int Utf8Decode(const char* s, Char& cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { cp = c; return 1; }
    if ((c >> 5) == 0x6) { cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c >> 4) == 0xE) { cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c >> 3) == 0x1E) {
        cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    cp = c;
    return 1;
}

static std::string Utf8Encode(Char ch) {
    std::string out;
    if (ch < 0x80) {
        out.push_back((char)ch);
    } else if (ch < 0x800) {
        out.push_back((char)(0xC0 | (ch >> 6)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    } else if (ch < 0x10000) {
        out.push_back((char)(0xE0 | (ch >> 12)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (ch >> 18)));
        out.push_back((char)(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (ch & 0x3F)));
    }
    return out;
}

static Text Utf8ToText(const std::string& text) {
    Text out;
    size_t i = 0;
    while (i < text.size()) {
        Char ch;
        int n = Utf8Decode(text.c_str() + i, ch);
        out.push_back(ch);
        i += (size_t)n;
    }
    return out;
}

static std::string TextToUtf8(const Text& text) {
    std::string out;
    for (Char ch : text) out += Utf8Encode(ch);
    return out;
}

/* 对应 Python 的切片 line[start:end](越界自动钳制) */
static Text Slice(const Text& text, int start, int end) {
    int length = (int)text.size();
    if (start < 0) start = 0;
    if (end > length) end = length;
    if (start >= end) return Text();
    return text.substr((size_t)start, (size_t)(end - start));
}

static Text TextOf(const char* ascii) {
    Text out;
    for (const char* p = ascii; *p; ++p) out.push_back((unsigned char)*p);
    return out;
}

/* 对应 str.split('\n')(至少返回一段) */
static std::vector<Text> SplitLines(const Text& text) {
    std::vector<Text> parts;
    Text current;
    for (Char ch : text) {
        if (ch == U'\n') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

/* 对应 str.strip()(去掉首尾空白) */
static Text Strip(const Text& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == U' ' || text[begin] == U'\t'
                           || text[begin] == U'\n' || text[begin] == U'\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == U' ' || text[end - 1] == U'\t'
                           || text[end - 1] == U'\n' || text[end - 1] == U'\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/* 对应 int(text): 允许前后空白与正负号, 其余字符非法 */
static bool ParseInt(const Text& text, long& value) {
    Text s = Strip(text);
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == U'+' || s[i] == U'-')) {
        negative = (s[i] == U'-');
        ++i;
    }
    if (i >= s.size()) return false;
    long result = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < U'0' || s[i] > U'9') return false;
        result = result * 10 + (long)(s[i] - U'0');
        if (result > 100000000L) result = 100000000L;  /* 防溢出, 越界值本就会被钳制 */
    }
    value = negative ? -result : result;
    return true;
}

/* 对应 str.find(query, start) */
static int FindFrom(const Text& line, const Text& query, int start) {
    if (start < 0) start = 0;
    if (start > (int)line.size()) return -1;
    size_t index = line.find(query, (size_t)start);
    return index == Text::npos ? -1 : (int)index;
}

/* 对应 str.rfind(query, 0, end): 命中必须完整落在 [0, end) 内 */
static int RFindBefore(const Text& line, const Text& query, int end) {
    if (end > (int)line.size()) end = (int)line.size();
    int last = end - (int)query.size();
    for (int start = last; start >= 0; --start) {
        if (line.compare((size_t)start, query.size(), query) == 0) return start;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Screen: 顶替 curses 的窗口
 *
 * 只实现 mini.py 用到的那几个原语: erase / move / clrtoeol / hline /
 * addstr(可带坐标) / refresh / get_wch, 语义与 curses 一致 -
 * 坐标是"单元格", 宽字符占两格(第二格标记为延续格), 越界静默裁剪。
 * ------------------------------------------------------------------------- */
struct Cell {
    Char ch;
    unsigned char attr;
    bool cont;      /* 宽字符的第二格 */

    Cell() : ch(U' '), attr(A_NORMAL), cont(false) {}
};

class Screen {
public:
    Screen() : cursor_y(0), cursor_x(0) { cells.resize(Width() * Height()); }

    /* ---- 终端进出 ---- */
    void Init();
    void Shutdown();

    /* ---- 尺寸 curses 对应物 ---- */
    int Height() const { return height; }
    int Width() const { return width; }
    void CheckResize();

    /* ---- 绘制原语 ---- */
    void erase();
    void move(int y, int x);
    void clrtoeol();
    void hline(Char ch, int n);
    void addstr(const Text& text, int attribute = A_NORMAL);
    void addstr(int y, int x, const Text& text, int attribute = A_NORMAL);
    void refresh();

    /* ---- 按键 ---- */
    Char get_wch();

private:
    void PutText(int y, int x, const Text& text, int attribute);
    void PutCell(int y, int x, Char ch, int attribute);
    void SplitCell(int y, int x);
#ifdef _WIN32
    void RefreshConsole();      /* 控制台: 把 cells 整屏写进屏幕缓冲区 */
#endif
    Cell& At(int y, int x) { return cells[(size_t)y * width + x]; }

    std::vector<Cell> cells;
    int height = 24;
    int width = 80;
    int cursor_y;
    int cursor_x;
};

/* ---- 平台状态 ---- */
#ifdef _WIN32
/* 两条完全不同的 I/O 路线:
     g_real_console = true  -> 真正的 Win32 控制台(cmd / PowerShell / Windows
         Terminal)。输出走 WriteConsoleOutputW(CHAR_INFO 逐格写, 颜色是控制台
         属性), 输入走 ReadConsoleInputW(直接拿按键记录)。全程不依赖 VT 序列,
         老版本 conhost(Win7/8 及关掉 VT 的 Win10)也能正常工作。
     g_real_console = false -> mintty/MSYS 的 pty 是管道, 用 ANSI + 字节流,
         与 POSIX 共用同一套代码。 */
static bool g_real_console = false;
static HANDLE g_hIn = INVALID_HANDLE_VALUE;
static HANDLE g_hOut = INVALID_HANDLE_VALUE;
static DWORD g_saved_in_mode = 0;
static DWORD g_saved_out_mode = 0;
static int g_origin_x = 0;          /* 控制台窗口左上角在屏幕缓冲区中的坐标 */
static int g_origin_y = 0;

/* 控制台在 VT 输入模式下把"一次按键"整体翻译成一段字节(方向键是一条 3 字节的
   CSI 序列)。若只向 _read 要 1 字节, 剩下的字节会留在控制台内部, 而
   _kbhit() 只统计输入记录条数, 看不到这些残留字节, 于是 get_wch 解析转义
   序列时会误判为"后续没有输入", 方向键就失效了。
   因此这里一次读多字节, 自己留一个读前缓冲, 按字节发放。 */
static unsigned char g_input_buffer[256];
static size_t g_input_pos = 0;
static size_t g_input_len = 0;

static bool BufferHasByte() { return g_input_pos < g_input_len; }

/* 阻塞式填充读前缓冲; 返回 false 表示输入结束 */
static bool FillInputBuffer() {
    int chunk;
    do {
        chunk = _read(_fileno(stdin), g_input_buffer, (unsigned)sizeof(g_input_buffer));
    } while (chunk < 0 && errno == EINTR);
    if (chunk <= 0) return false;
    g_input_pos = 0;
    g_input_len = (size_t)chunk;
    return true;
}
#endif
/* 是否走 Win32 控制台 API(非 Windows 恒为 false) */
static bool UseConsoleApi() {
#ifdef _WIN32
    return g_real_console;
#else
    return false;
#endif
}

#if !defined(_WIN32) || defined(HAVE_POSIX_TTY)
static struct termios g_saved_termios;      /* 进入前的终端属性, 退出时原样还原 */
static bool g_saved_termios_valid = false;
#endif

static bool ReadByte(unsigned char& out) {
#ifdef _WIN32
#  ifdef HAVE_POSIX_TTY
    if (!g_real_console) {
        if (read(STDIN_FILENO, &out, 1) != 1) return false;
        return true;
    }
#  endif
    if (!BufferHasByte() && !FillInputBuffer()) return false;
    out = g_input_buffer[g_input_pos++];
    return true;
#else
    if (read(STDIN_FILENO, &out, 1) != 1) return false;
    return true;
#endif
}

static bool ReadByteTimeout(unsigned char& out, int milliseconds) {
#ifdef _WIN32
#  ifdef HAVE_POSIX_TTY
    if (!g_real_console) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, milliseconds) <= 0) return false;
        if (read(STDIN_FILENO, &out, 1) != 1) return false;
        return true;
    }
#  endif
    if (!BufferHasByte()) {
        if (!_isatty(_fileno(stdin))) {
            /* 标准输入被重定向成文件/管道: 没有"是否已有按键"的概念, 直接读 */
            if (!FillInputBuffer()) return false;
        } else {
            int waited = 0;
            while (!_kbhit()) {
                if (waited >= milliseconds) return false;
                Sleep(1);
                waited += 1;
            }
            if (!FillInputBuffer()) return false;
        }
    }
    out = g_input_buffer[g_input_pos++];
    return true;
#else
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, milliseconds) <= 0) return false;
    if (read(STDIN_FILENO, &out, 1) != 1) return false;
    return true;
#endif
}

/* 把 CSI 序列(含起始的 '[' 或 'O')解析为方向键等; 无法识别返回 0 */
static Char ParseCsi(const std::string& seq) {
    if (seq.size() < 2) return 0;
    char fin = seq[seq.size() - 1];
    /* 带修饰键参数(如 Shift/Ctrl/Alt)的序列: Python 版里 ncurses 只对方向键给出
       KEY_SR/SF/SLEFT/SRIGHT 且一律按未按 Shift 处理(HandleMoveKey 传 shift=False),
       其余(Shift+Home/End/Delete 等)在 HandleMoveKey 里没有分支, 表现为忽略。
       这里保持同样的行为。 */
    bool has_modifier = seq.find(';') != std::string::npos;
    if (has_modifier && fin != 'A' && fin != 'B' && fin != 'C' && fin != 'D') return 0;
    if (fin == '~') {
        int number = 0;
        bool has_digit = false;
        for (size_t i = 1; i + 1 < seq.size(); ++i) {
            if (seq[i] < '0' || seq[i] > '9') break;
            number = number * 10 + (seq[i] - '0');
            has_digit = true;
        }
        if (!has_digit) return 0;
        if (number == 1 || number == 7) return KEY_HOME;
        if (number == 4 || number == 8) return KEY_END;
        if (number == 5) return KEY_PPAGE;
        if (number == 6) return KEY_NPAGE;
        if (number == 3) return KEY_DC;
        if (number == 2) return KEY_IC;
        return 0;
    }
    if (fin == 'A') return KEY_UP;
    if (fin == 'B') return KEY_DOWN;
    if (fin == 'C') return KEY_RIGHT;
    if (fin == 'D') return KEY_LEFT;
    if (fin == 'H') return KEY_HOME;
    if (fin == 'F') return KEY_END;
    if (fin == 'Z') return KEY_BTAB;   /* Shift+Tab */
    return 0;
}

void Screen::Init() {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0;
    DWORD out_mode = 0;
    /* 输入输出都能取到控制台模式, 才说明真的是控制台; mintty 的 pty 是管道 */
    g_real_console = GetConsoleMode(hIn, &in_mode) && GetConsoleMode(hOut, &out_mode);
    if (g_real_console) {
        g_hIn = hIn;
        g_hOut = hOut;
        g_saved_in_mode = in_mode;
        g_saved_out_mode = out_mode;
        /* 关掉行缓冲/回显/Ctrl-C 处理: 让按键一条一条立刻送达;
           顺带关掉快速编辑(否则鼠标点一下窗口, 输入就会卡住等回车) */
        DWORD mode = in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                 ENABLE_PROCESSED_INPUT);
        mode = (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    } else {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    }
#  ifdef HAVE_POSIX_TTY
    if (!g_real_console) {
        tcgetattr(STDIN_FILENO, &g_saved_termios);
        g_saved_termios_valid = true;
        struct termios raw = g_saved_termios;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
#  endif
#else
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    g_saved_termios_valid = true;
    struct termios raw = g_saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    if (!UseConsoleApi()) {
        /* 备用屏幕 + 清屏 + 显示光标(方块光标由终端自身绘制) */
        fputs("\x1b[?1049h\x1b[0m\x1b[2J\x1b[?25h", stdout);
        fflush(stdout);
    }
    CheckResize();
    cells.assign((size_t)width * height, Cell());
}

void Screen::Shutdown() {
#ifdef _WIN32
    if (g_real_console) {
        CONSOLE_CURSOR_INFO info;
        if (GetConsoleCursorInfo(g_hOut, &info) && !info.bVisible) {
            info.bVisible = TRUE;
            SetConsoleCursorInfo(g_hOut, &info);
        }
        SetConsoleMode(g_hIn, g_saved_in_mode);
        SetConsoleMode(g_hOut, g_saved_out_mode);
        /* 光标挪到窗口最后一行再换行, 让命令提示符从下面接着走 */
        COORD pos = {(SHORT)g_origin_x, (SHORT)(g_origin_y + height - 1)};
        SetConsoleCursorPosition(g_hOut, pos);
        fputs("\r\n", stdout);
        fflush(stdout);
        return;
    }
#endif
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
#ifdef _WIN32
#  ifdef HAVE_POSIX_TTY
    if (!g_real_console) {
        if (g_saved_termios_valid) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    }
#  endif
#else
    if (g_saved_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    }
#endif
}

void Screen::CheckResize() {
    int rows = 0;
    int cols = 0;
#ifdef _WIN32
    if (g_real_console) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(g_hOut, &info)) {
            cols = info.srWindow.Right - info.srWindow.Left + 1;
            rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            /* WriteConsoleOutputW / SetConsoleCursorPosition 用的是缓冲区坐标,
               而屏幕上的 (0,0) 是窗口左上角, 所以每次重排都要记住窗口原点 */
            g_origin_x = info.srWindow.Left;
            g_origin_y = info.srWindow.Top;
        }
    }
#endif
#if !defined(_WIN32) || defined(HAVE_POSIX_TTY)
    if ((rows <= 0 || cols <= 0)
#  ifdef _WIN32
        && !g_real_console
#  endif
    ) {
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
            cols = size.ws_col;
            rows = size.ws_row;
        }
    }
#endif
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;
    if (rows < 3) rows = 3;
    if (cols < 8) cols = 8;
    if (cols == width && rows == height) return;
    width = cols;
    height = rows;
    if (cursor_x >= width) cursor_x = width - 1;
    if (cursor_y >= height) cursor_y = height - 1;
    cells.assign((size_t)width * height, Cell());
}

void Screen::erase() {
    cells.assign((size_t)width * height, Cell());
    cursor_y = 0;
    cursor_x = 0;
}

void Screen::move(int y, int x) {
    if (y < 0) y = 0;
    if (y > height - 1) y = height - 1;
    if (x < 0) x = 0;
    if (x > width - 1) x = width - 1;
    cursor_y = y;
    cursor_x = x;
}

void Screen::SplitCell(int y, int x) {
    Cell& cell = At(y, x);
    if (cell.cont) {
        if (x > 0) {
            Cell& previous = At(y, x - 1);
            previous.ch = U' ';
            previous.attr = A_NORMAL;
        }
        cell.cont = false;
        cell.ch = U' ';
        return;
    }
    if (cell.ch >= 0x1100 && x + 1 < width && At(y, x + 1).cont) {
        At(y, x + 1).cont = false;
        At(y, x + 1).ch = U' ';
    }
}

void Screen::PutCell(int y, int x, Char ch, int attribute) {
    SplitCell(y, x);
    Cell& cell = At(y, x);
    cell.ch = ch;
    cell.attr = (unsigned char)attribute;
    if (DisplayWidth(ch) == 2 && x + 1 < width) {
        SplitCell(y, x + 1);
        Cell& second = At(y, x + 1);
        second.ch = 0;
        second.attr = (unsigned char)attribute;
        second.cont = true;
    }
    cursor_y = y;
    cursor_x = x;
}

void Screen::PutText(int y, int x, const Text& text, int attribute) {
    if (y < 0 || y >= height) return;
    int column = x;
    for (Char ch : text) {
        if (ch == U'\n') {          /* curses 里 addstr 遇 \n 换行 */
            y += 1;
            column = 0;
            if (y >= height) return;
            continue;
        }
        if (ch == U'\r') { column = 0; continue; }
        if (ch == U'\t') {          /* 与 DisplayWidth 一致: 制表符占 4 格 */
            for (int i = 0; i < 4; ++i) {
                if (column >= width) break;
                PutCell(y, column, U' ', attribute);
                column += 1;
            }
            continue;
        }
        int char_width = DisplayWidth(ch);
        if (column + char_width > width) break;
        PutCell(y, column, ch, attribute);
        column += char_width;
    }
    cursor_y = y;
    cursor_x = column < width ? column : width - 1;
}

void Screen::addstr(const Text& text, int attribute) {
    PutText(cursor_y, cursor_x, text, attribute);
}

void Screen::addstr(int y, int x, const Text& text, int attribute) {
    PutText(y, x, text, attribute);
}

void Screen::clrtoeol() {
    for (int x = cursor_x; x < width; ++x) {
        SplitCell(cursor_y, x);
        Cell& cell = At(cursor_y, x);
        cell.ch = U' ';
        cell.attr = A_NORMAL;
    }
}

void Screen::hline(Char ch, int n) {
    int y = cursor_y;
    int column = cursor_x;
    for (int i = 0; i < n && column < width; ++i, ++column) {
        SplitCell(y, column);
        Cell& cell = At(y, column);
        cell.ch = ch;
        cell.attr = A_NORMAL;
    }
}

#ifdef _WIN32
/* 把整屏 cells 转成 CHAR_INFO 直接写进控制台缓冲区:
   反色 = 前景/背景互换(默认灰字黑底), 宽字符占两格由控制台自己处理。 */
void Screen::RefreshConsole() {
    std::vector<CHAR_INFO> image((size_t)width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Cell& cell = At(y, x);
            CHAR_INFO& item = image[(size_t)y * width + x];
            Char ch = cell.cont ? U' ' : cell.ch;
            item.Char.UnicodeChar = (WCHAR)(ch <= 0xFFFF ? ch : U'?');
            item.Attributes = (cell.attr & A_REVERSE)
                ? (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)
                : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }
    }
    COORD image_size = {(SHORT)width, (SHORT)height};
    COORD image_origin = {0, 0};
    SMALL_RECT target = {(SHORT)g_origin_x, (SHORT)g_origin_y,
                         (SHORT)(g_origin_x + width - 1),
                         (SHORT)(g_origin_y + height - 1)};
    WriteConsoleOutputW(g_hOut, image.data(), image_size, image_origin, &target);
    COORD cursor = {(SHORT)(g_origin_x + cursor_x), (SHORT)(g_origin_y + cursor_y)};
    SetConsoleCursorPosition(g_hOut, cursor);
    CONSOLE_CURSOR_INFO info;
    if (GetConsoleCursorInfo(g_hOut, &info) && !info.bVisible) {
        info.bVisible = TRUE;
        SetConsoleCursorInfo(g_hOut, &info);
    }
}
#endif

void Screen::refresh() {
#ifdef _WIN32
    if (g_real_console) {
        RefreshConsole();
        return;
    }
#endif
    std::string out;
    out.reserve((size_t)width * height * 3);
    out += "\x1b[?25l";                     /* 绘制期间隐藏光标, 避免闪烁 */
    for (int y = 0; y < height; ++y) {
        int last = -1;                      /* 行尾最后一个非空白单元格 */
        for (int x = 0; x < width; ++x) {
            const Cell& cell = At(y, x);
            if (cell.cont) { last = x; continue; }
            if (cell.ch != U' ' || cell.attr != A_NORMAL) last = x;
        }
        char head[32];
        snprintf(head, sizeof head, "\x1b[%d;1H", y + 1);
        out += head;
        int attribute = A_NORMAL;
        for (int x = 0; x <= last; ++x) {
            const Cell& cell = At(y, x);
            if (cell.cont) continue;
            if ((cell.attr & A_REVERSE) != attribute) {
                attribute = cell.attr & A_REVERSE;
                out += attribute ? "\x1b[7m" : "\x1b[0m";
            }
            out += Utf8Encode(cell.ch);
        }
        if (attribute != A_NORMAL) out += "\x1b[0m";
        out += "\x1b[K";
    }
    char tail[64];
    snprintf(tail, sizeof tail, "\x1b[%d;%dH\x1b[?25h", cursor_y + 1, cursor_x + 1);
    out += tail;
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

#ifdef _WIN32
/* 长按按键时控制台会把重复次数合并进一条记录(wRepeatCount), 这里拆成多次返回 */
static Char g_pending_key = 0;
static int g_pending_count = 0;

/* 控制台按键记录 -> 内部字符码; 返回 0 表示这条记录不用理会 */
static Char ConsoleKeyChar(const KEY_EVENT_RECORD& key) {
    bool shift = (key.dwControlKeyState & SHIFT_PRESSED) != 0;
    switch (key.wVirtualKeyCode) {
        case VK_UP:     return KEY_UP;
        case VK_DOWN:   return KEY_DOWN;
        case VK_LEFT:   return KEY_LEFT;
        case VK_RIGHT:  return KEY_RIGHT;
        case VK_HOME:   return KEY_HOME;
        case VK_END:    return KEY_END;
        case VK_PRIOR:  return KEY_PPAGE;
        case VK_NEXT:   return KEY_NPAGE;
        case VK_DELETE: return KEY_DC;
        case VK_INSERT: return KEY_IC;
        case VK_BACK:   return KEY_BACKSPACE;
        case VK_TAB:    return shift ? KEY_BTAB : (Char)U'\t';
        case VK_RETURN: return (Char)U'\n';
        case VK_ESCAPE: return KEY_ESC;
        default: break;
    }
    WCHAR wc = key.uChar.UnicodeChar;
    if (wc == 0) return 0;      /* 只有修饰键按下, 或输入法还在组合中 */
    return (Char)wc;
}
#endif

Char Screen::get_wch() {
#ifdef _WIN32
    if (g_real_console) {
        /* 直接读按键记录: 不经过 VT 翻译, 方向键/中文输入都不会走样 */
        for (;;) {
            if (g_pending_count > 0) {
                Char ch = g_pending_key;
                g_pending_count -= 1;
                if (g_pending_count == 0) g_pending_key = 0;
                return ch;
            }
            INPUT_RECORD record;
            DWORD count = 0;
            if (!ReadConsoleInputW(g_hIn, &record, 1, &count)) return KEY_EOF;
            if (count == 0) continue;
            if (record.EventType != KEY_EVENT) continue;
            const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
            if (!key.bKeyDown) continue;
            Char ch = ConsoleKeyChar(key);
            if (!ch) continue;
            if (key.wRepeatCount > 1) {
                g_pending_key = ch;
                g_pending_count = key.wRepeatCount - 1;
            }
            return ch;
        }
    }
#endif
    for (;;) {
        unsigned char c = 0;
        if (!ReadByte(c)) return KEY_EOF;
        if (c == 27) {
            /* Esc 与转义序列的分界: 短等一会儿看后面是否跟着 '[' 或 'O' */
            unsigned char next = 0;
            if (ReadByteTimeout(next, 25) && (next == '[' || next == 'O')) {
                std::string sequence;
                sequence.push_back((char)next);
                while (sequence.size() < 15) {
                    unsigned char b = 0;
                    if (!ReadByteTimeout(b, 25)) break;
                    sequence.push_back((char)b);
                    if (b >= 0x40 && b <= 0x7E) break;
                }
                Char key = ParseCsi(sequence);
                if (key) return key;
                continue;               /* 未识别的序列: 忽略, 继续读 */
            }
            return KEY_ESC;
        }
        if (c == 3) return 3;           /* 裸 Ctrl-C: 上层忽略 */
        if (c == 127 || c == 8) return KEY_BACKSPACE;
        if (c == '\r' || c == '\n') return U'\n';
        if (c < 0x80) return (Char)c;
        /* 多字节 UTF-8 字符 */
        char buffer[8];
        int length = Utf8Length(c);
        buffer[0] = (char)c;
        int got = 1;
        for (int i = 1; i < length; ++i) {
            unsigned char d = 0;
            if (!ReadByteTimeout(d, 100)) break;
            buffer[got++] = (char)d;
        }
        buffer[got] = 0;
        Char ch = 0;
        Utf8Decode(buffer, ch);
        if (ch == 0) ch = c;
        return ch;
    }
}

/* ---------------------------------------------------------------------------
 * 文本缓冲区(整文件: 每行一个字符串)
 * ------------------------------------------------------------------------- */
class Buffer {
public:
    std::vector<Text> lines;
    int row;
    int col;
    int sel_row;
    int sel_col;
    bool has_sel;
    bool dirty;

    Buffer() : lines(1, Text()), row(0), col(0), sel_row(0), sel_col(0),
               has_sel(false), dirty(false) {}

    /* ---- 行列换算 / 位置偏移 ---- */
    int LineCount() const { return (int)lines.size(); }

    int LineLength(int row) const { return (int)lines[row].size(); }

    Pos ClampPosition(Pos pos) const {
        /* 把任意 (row, col) 钳制为文档内的有效位置 */
        int row = pos.first;
        int col = pos.second;
        if (row < 0) {
            row = 0;
        } else if (row >= (int)lines.size()) {
            row = (int)lines.size() - 1;
        }
        if (col < 0) {
            col = 0;
        } else if (col > (int)lines[row].size()) {
            col = (int)lines[row].size();
        }
        return Pos(row, col);
    }

    void ClampCursor() { Pos pos = ClampPosition(Pos(row, col)); row = pos.first; col = pos.second; }

    Pos Offset(Pos pos, int offset) const {
        /* 把文档视作一条扁平文本(每行行尾隐含一个换行符),
           返回从 pos 前进(正)或后退(负) offset 个字符后的位置; 越界钳制到文档首/尾。 */
        pos = ClampPosition(pos);
        int index = pos.second;
        for (int line_index = 0; line_index < pos.first; ++line_index) {
            index += (int)lines[line_index].size() + 1;
        }
        index += offset;
        if (index <= 0) return Pos(0, 0);
        for (int line_index = 0; line_index < (int)lines.size(); ++line_index) {
            if (index <= (int)lines[line_index].size()) return Pos(line_index, index);
            index -= (int)lines[line_index].size() + 1;
        }
        int last_row = (int)lines.size() - 1;
        return Pos(last_row, (int)lines[last_row].size());
    }

    /* ---- 光标移动 ---- */
    void Move(int delta_row, int delta_col, bool extend) {
        Pos origin(row, col);
        if (delta_col > 0) {
            /* 右移: 到行尾继续按则移动到下一行行首 */
            int steps = delta_col;
            while (steps > 0 && row < (int)lines.size()) {
                if (col < (int)lines[row].size()) {
                    col += 1;
                } else if (row < (int)lines.size() - 1) {
                    row += 1;
                    col = 0;
                } else {
                    break;
                }
                steps -= 1;
            }
        } else if (delta_col < 0) {
            /* 左移: 到行首继续按则移动到上一行行尾 */
            int steps = -delta_col;
            while (steps > 0 && row >= 0) {
                if (col > 0) {
                    col -= 1;
                } else if (row > 0) {
                    row -= 1;
                    col = (int)lines[row].size();
                } else {
                    break;
                }
                steps -= 1;
            }
        } else {
            /* 垂直移动(上/下/翻页): 已在首/末行继续按键 -> 行首/行尾(文件头/尾) */
            int new_row = row + delta_row;
            if (new_row < 0) {
                row = 0;
                col = 0;
            } else if (new_row >= (int)lines.size()) {
                row = (int)lines.size() - 1;
                col = (int)lines[row].size();
            } else {
                row = new_row;
            }
        }
        if (extend) {
            Pos anchor = has_sel ? Pos(sel_row, sel_col) : origin;
            SetSelection(anchor, Pos(row, col));
        } else {
            SetCursor(Pos(row, col));
        }
    }

    /* ---- 文本替换 ---- */
    Text ExpandTabs(const Text& text) const {
        Text out;
        out.reserve(text.size());
        for (Char ch : text) {
            if (ch == U'\t') {
                out += U"    ";      /* 制表符统一为 4 空格, 避免宽度错位 */
            } else {
                out.push_back(ch);
            }
        }
        return out;
    }

    void Replace(Pos start, Pos end, const Text& text) {
        /* 把 [start, end) 区间替换为 text; 光标停在替换内容末尾并清除选区。
           start > end 时自动交换, 区间为空且 text 为空时不做任何改动。 */
        start = ClampPosition(start);
        end = ClampPosition(end);
        if (start > end) std::swap(start, end);
        if (start == end && text.empty()) return;
        Text expanded = ExpandTabs(text);
        int start_row = start.first;
        int start_col = start.second;
        int end_row = end.first;
        int end_col = end.second;
        Text merged = lines[start_row].substr(0, (size_t)start_col) + expanded
                    + lines[end_row].substr((size_t)end_col);
        std::vector<Text> pieces = SplitLines(merged);
        lines.erase(lines.begin() + start_row, lines.begin() + end_row + 1);
        lines.insert(lines.begin() + start_row, pieces.begin(), pieces.end());
        std::vector<Text> inserted = SplitLines(expanded);
        if (inserted.size() == 1) {
            row = start_row;
            col = start_col + (int)expanded.size();
        } else {
            row = start_row + (int)inserted.size() - 1;
            col = (int)inserted.back().size();
        }
        has_sel = false;
        dirty = true;
    }

    void Insert(Pos pos, const Text& text) {
        /* 在 pos 处插入文本, 等价于 Replace(pos, pos, text) */
        Replace(pos, pos, text);
    }

    void Delete(Pos start, Pos end) {
        /* 删除 [start, end) 区间, 等价于 Replace(start, end, '') */
        Replace(start, end, Text());
    }

    /* ---- 选区 ---- */
    std::pair<Pos, Pos> GetSelection() const {
        /* 返回有序的 (起点, 终点); 无选区时退化为 (光标, 光标) */
        Pos anchor(sel_row, sel_col);
        Pos cursor(row, col);
        if (anchor > cursor) return std::make_pair(cursor, anchor);
        return std::make_pair(anchor, cursor);
    }

    Text GetSelectionText() const {
        if (!has_sel) return Text();
        std::pair<Pos, Pos> range = GetSelection();
        Pos start = range.first;
        Pos end = range.second;
        if (start.first == end.first) {
            return Slice(lines[start.first], start.second, end.second);
        }
        std::vector<Text> parts;
        parts.push_back(lines[start.first].substr((size_t)start.second));
        for (int row = start.first + 1; row < end.first; ++row) {
            parts.push_back(lines[row]);
        }
        parts.push_back(lines[end.first].substr(0, (size_t)end.second));
        Text out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out.push_back(U'\n');
            out += parts[i];
        }
        return out;
    }

    void SetSelection(Pos start, Pos end) {
        /* start 为锚点, end 为光标位置; end 可以位于 start 之前(反向选区)。
           首尾重合视为没有选区, 因此 SetCursor(pos) 等价于 SetSelection(pos, pos)。 */
        Pos anchor = ClampPosition(start);
        Pos cursor = ClampPosition(end);
        sel_row = anchor.first;
        sel_col = anchor.second;
        row = cursor.first;
        col = cursor.second;
        has_sel = (Pos(sel_row, sel_col) != Pos(row, col));
    }

    void SetCursor(Pos pos) { SetSelection(pos, pos); }

    bool IsInSelection(int row, int col) const {
        /* 选区是半开区间 [start, end): 终点所在位置是光标(插入点)本身,
           不属于选中的字符, 因此终点位置的字符不会被高亮。 */
        if (!has_sel) return false;
        std::pair<Pos, Pos> range = GetSelection();
        Pos position(row, col);
        return range.first <= position && position < range.second;
    }

    bool IsNewlineSelected(int row) const {
        /* 判断第 row 行行尾的换行符是否落在选区内。
           换行符位于该行最后一个字符之后, 即位置 (row, 行长度);
           与该位置本身重合的终点不算选中, 只有终点越过它才算。 */
        if (!has_sel) return false;
        std::pair<Pos, Pos> range = GetSelection();
        Pos position(row, (int)lines[row].size());
        return range.first <= position && position < range.second;
    }

    void SelectTo(Pos pos, bool extend = true) {
        /* 把光标移到 pos; extend=true 时从原锚点(无选区时取移动前光标)扩展选区 */
        if (!extend) {
            SetCursor(pos);
            return;
        }
        Pos anchor = has_sel ? Pos(sel_row, sel_col) : Pos(row, col);
        SetSelection(anchor, pos);
    }

    void MoveToLineStart(bool extend = true) { SelectTo(Pos(row, 0), extend); }

    void MoveToLineEnd(bool extend = true) { SelectTo(Pos(row, (int)lines[row].size()), extend); }

    void SelectLine(int row) { SetSelection(Pos(row, 0), Pos(row, (int)lines[row].size())); }

    void SelectAll() {
        SetSelection(Pos(0, 0), Pos((int)lines.size() - 1, (int)lines.back().size()));
    }
};

/* 撤销快照(对应 Python 的 dict) */
struct Snapshot {
    std::vector<Text> lines;
    int row;
    int col;
    int sel_row;
    int sel_col;
    bool has_sel;
};

/* 连续编辑合并组: {'kind', 'next'} */
struct UndoGroup {
    std::string kind;
    Pos next;
};

class Editor;

/* ---------------------------------------------------------------------------
 * 命令实现(撤销重做/剪贴板/行操作/大小写/缩进/跳转/查找; 由 Editor 分派)
 * ------------------------------------------------------------------------- */
class Commands {
public:
    explicit Commands(Editor& editor) : editor(editor) {}

    /* ---- 撤销/重做(快照) ---- */
    Snapshot TakeSnapshot();
    void PushUndo();
    void RestoreSnapshot(const Snapshot& snapshot);
    void Undo();
    void Redo();
    void RefreshDirty();
    void BeginUndoGroup(const std::string& kind, int row, int col);
    bool Edit(const std::function<void()>& action);

    /* ---- 编辑命令(调用 Buffer 原语 + 撤销合并) ---- */
    void TypeCharacter(Char ch);
    void InsertString(const Text& text);
    void SplitLine();
    void DeleteCharacter(bool backspace);

    /* ---- 行级操作 ---- */
    void DuplicateCurrentLine();
    void DeleteCurrentLine();

    /* ---- 选区 / 剪贴板命令 ---- */
    void SelectAll();
    void Copy();
    void Cut();
    void Paste();
    void Duplicate();
    void DeleteLine();

    /* ---- 大小写 / 缩进 ---- */
    void ChangeCase(bool upper);
    void ApplyCase(bool upper);
    std::optional<Pos> CurrentWordRange();
    bool ApplyWordCase(bool upper);
    void ChangeIndent(bool dedent);
    void ApplyIndent(bool dedent);

    /* ---- 跳转 / 查找 ---- */
    std::optional<Pos> ParseGoto(const Text& text);
    std::optional<Pos> Search(const Text& query, bool reverse = false,
                              std::optional<int> start_row = std::nullopt,
                              std::optional<int> start_col = std::nullopt);
    void FindNext(const Text& query, bool reverse);

    /* ---- 文件命令 ---- */
    void Save();
    void Close();

private:
    Editor& editor;
};

/* ---------------------------------------------------------------------------
 * 编辑器(终端/光标/渲染/输入)
 * ------------------------------------------------------------------------- */
class Editor {
public:
    Editor(Screen& stdscr, const std::string& filename);

    Buffer buffer;
    Screen& stdscr;
    std::string filename;
    int mode;                       /* 0=编辑 1=命令(默认进入命令模式) */
    int top;                        /* 顶部可见行 */
    int left;                       /* 水平滚动 */
    Text clipboard;
    Text status;
    std::optional<std::string> prompt_mode;   /* "find" "findrev" "goto" "saveas" */
    Text prompt_text;
    bool goto_select;               /* Shift+G: 跳转同时产生/改变选区 */
    std::vector<Snapshot> undo_stack;
    std::vector<Snapshot> redo_stack;
    std::optional<UndoGroup> undo_group;      /* 连续输入/删除合并组 */
    bool running;
    std::optional<std::vector<Text>> saved;   /* 干净基准: 无值表示新文档(始终脏) */
    Commands commands;

    /* ---- 文件读写交互 ---- */
    void LoadFile(const std::string& path);
    void SaveFile(const std::string& path);

    /* ---- 尺寸 ---- */
    int Height() const { return stdscr.Height(); }
    int Width() const { return stdscr.Width(); }
    int TextRows() const { return std::max(1, Height() - 2); }

    /* ---- 渲染 ---- */
    void AddText(int screen_row, int column, const Text& text, int attribute = A_NORMAL);
    void AddTextRun(const Text& text, bool selected);
    void DrawLine(int screen_row, int line_index);
    void DrawStatus(int height, int width);
    void Render();

    /* ---- 主循环 ---- */
    void Run();

    /* ---- 按键分发 ---- */
    void HandleKey(Char ch);
    void HandleEdit(Char ch);
    void HandleCommand(Char ch);
    void HandleMoveKey(Char key, bool shift);

    /* ---- 提示子模式(F / Shift+F / G / Shift+S) ---- */
    void HandlePrompt(Char ch);
    void ConfirmPrompt();
};

/* ===========================================================================
 * Commands 实现
 * ========================================================================= */
Snapshot Commands::TakeSnapshot() {
    Buffer& buffer = editor.buffer;
    Snapshot snapshot;
    snapshot.lines = buffer.lines;
    snapshot.row = buffer.row;
    snapshot.col = buffer.col;
    snapshot.sel_row = buffer.sel_row;
    snapshot.sel_col = buffer.sel_col;
    snapshot.has_sel = buffer.has_sel;
    return snapshot;
}

void Commands::PushUndo() {
    editor.undo_stack.push_back(TakeSnapshot());
    editor.redo_stack.clear();
}

void Commands::RestoreSnapshot(const Snapshot& snapshot) {
    Buffer& buffer = editor.buffer;
    buffer.lines = snapshot.lines;
    buffer.row = snapshot.row;
    buffer.col = snapshot.col;
    buffer.sel_row = snapshot.sel_row;
    buffer.sel_col = snapshot.sel_col;
    buffer.has_sel = snapshot.has_sel;
    buffer.ClampCursor();
}

void Commands::Undo() {
    if (editor.undo_stack.empty()) return;
    editor.redo_stack.push_back(TakeSnapshot());
    Snapshot snapshot = editor.undo_stack.back();
    editor.undo_stack.pop_back();
    RestoreSnapshot(snapshot);
    editor.undo_group.reset();
    RefreshDirty();
}

void Commands::Redo() {
    if (editor.redo_stack.empty()) return;
    editor.undo_stack.push_back(TakeSnapshot());
    Snapshot snapshot = editor.redo_stack.back();
    editor.redo_stack.pop_back();
    RestoreSnapshot(snapshot);
    editor.undo_group.reset();
    RefreshDirty();
}

void Commands::RefreshDirty() {
    /* 脏标记 = 当前内容是否偏离"最近一次保存的状态"(以保存快照为准)。
       撤销/重做恰好回到保存点(或打开时的磁盘内容)即视为干净。 */
    if (!editor.saved.has_value()) {
        editor.buffer.dirty = true;     /* 尚未保存过的新文档始终脏 */
    } else {
        editor.buffer.dirty = (editor.buffer.lines != editor.saved.value());
    }
}

void Commands::BeginUndoGroup(const std::string& kind, int row, int col) {
    /* 连续输入(insert)/连续退格(back)/连续删除(del) 合并为一次撤销。
       判断当前操作的"起始光标位置"是否紧接上一次同类操作之后。 */
    Pos next_position;
    if (kind == "insert") {
        next_position = Pos(row, col + 1);      /* 输入后光标右移一格 */
    } else if (kind == "back") {
        next_position = Pos(row, col - 1);      /* 退格后光标左移一格 */
    } else {                                    /* del */
        next_position = Pos(row, col);          /* Delete 键光标不动 */
    }
    if (editor.undo_group.has_value() && editor.undo_group->kind == kind
            && editor.undo_group->next == Pos(row, col)) {
        editor.undo_group->next = next_position;   /* 续接: 不压新快照 */
        return;
    }
    PushUndo();
    UndoGroup group;
    group.kind = kind;
    group.next = next_position;
    editor.undo_group = group;
}

bool Commands::Edit(const std::function<void()>& action) {
    /* 执行一次编辑动作, 并在"这次编辑什么都没留下"时回滚撤销记录。
       判据是文本与光标/选区都未变化: 例如空文档 Shift+D、文档开头退格、
       已是目标大小写的文本再转一次、无缩进可减, 都回滚本次压入的快照,
       并还原重做栈/连续编辑组/脏标记。
       反过来, 文本没变但光标或选区变了(例如原地粘贴导致选区被清除)仍算一次
       有效编辑: 撤销需要把选区还原回去。 */
    Buffer& buffer = editor.buffer;
    std::vector<Text> before_lines = buffer.lines;
    int before_row = buffer.row;
    int before_col = buffer.col;
    int before_sel_row = buffer.sel_row;
    int before_sel_col = buffer.sel_col;
    bool before_has_sel = buffer.has_sel;
    bool before_dirty = buffer.dirty;
    size_t undo_depth = editor.undo_stack.size();
    std::vector<Snapshot> redo_before = editor.redo_stack;
    std::optional<UndoGroup> group_before = editor.undo_group;
    action();
    if (buffer.lines != before_lines) return true;
    if (buffer.row != before_row || buffer.col != before_col
            || buffer.sel_row != before_sel_row || buffer.sel_col != before_sel_col
            || buffer.has_sel != before_has_sel) {
        return true;
    }
    if (editor.undo_stack.size() > undo_depth) editor.undo_stack.resize(undo_depth);
    editor.redo_stack = redo_before;
    editor.undo_group = group_before;
    buffer.row = before_row;
    buffer.col = before_col;
    buffer.sel_row = before_sel_row;
    buffer.sel_col = before_sel_col;
    buffer.has_sel = before_has_sel;
    buffer.dirty = before_dirty;
    buffer.ClampCursor();
    return false;
}

void Commands::TypeCharacter(Char ch) {
    /* 单字符输入: 连续输入合并为一次撤销 */
    Buffer& buffer = editor.buffer;
    Text text(1, ch);
    Edit([this, &buffer, text]() {
        BeginUndoGroup("insert", buffer.row, buffer.col);
        buffer.Insert(Pos(buffer.row, buffer.col), text);
    });
}

void Commands::InsertString(const Text& text) {
    /* 粘贴等多字符插入: 不并入连续输入组, 单独压一次快照 */
    Buffer& buffer = editor.buffer;
    Edit([this, &buffer, text]() {
        PushUndo();
        editor.undo_group.reset();
        buffer.Insert(Pos(buffer.row, buffer.col), text);
    });
}

void Commands::SplitLine() {
    /* Enter: 在光标处把一行拆成两行 */
    Buffer& buffer = editor.buffer;
    Edit([this, &buffer]() {
        PushUndo();
        editor.undo_group.reset();
        buffer.Insert(Pos(buffer.row, buffer.col), Text(1, U'\n'));
    });
}

void Commands::DeleteCharacter(bool backspace) {
    /* 两种模式共用: backspace 删光标前, 否则删光标后; 有选区则整段删除 */
    Edit([this, backspace]() {
        Buffer& buffer = editor.buffer;
        Pos cursor(buffer.row, buffer.col);
        if (buffer.has_sel) {
            PushUndo();
            editor.undo_group.reset();
            std::pair<Pos, Pos> range = buffer.GetSelection();
            buffer.Delete(range.first, range.second);
        } else if (backspace) {
            BeginUndoGroup("back", buffer.row, buffer.col);
            buffer.Delete(buffer.Offset(cursor, -1), cursor);
        } else {
            BeginUndoGroup("del", buffer.row, buffer.col);
            buffer.Delete(cursor, buffer.Offset(cursor, 1));
        }
    });
}

void Commands::DuplicateCurrentLine() {
    /* 复制当前行并插到其后, 光标落在新行行首 */
    Buffer& buffer = editor.buffer;
    int row = buffer.row;
    buffer.Insert(Pos(row, buffer.LineLength(row)), Text(1, U'\n') + buffer.lines[row]);
    buffer.SetCursor(Pos(row + 1, 0));
}

void Commands::DeleteCurrentLine() {
    /* 删除当前行; 删末行时连同其前面的换行一起删除, 保证行数正确减少 */
    Buffer& buffer = editor.buffer;
    int row = buffer.row;
    if (row != buffer.LineCount() - 1) {
        buffer.Delete(Pos(row, 0), Pos(row + 1, 0));
    } else if (row == 0) {
        buffer.Delete(Pos(0, 0), Pos(0, buffer.LineLength(0)));
    } else {
        buffer.Delete(Pos(row - 1, buffer.LineLength(row - 1)),
                      Pos(row, buffer.LineLength(row)));
    }
    buffer.SetCursor(Pos(std::min(row, buffer.LineCount() - 1), 0));
}

void Commands::SelectAll() { editor.buffer.SelectAll(); }

void Commands::Copy() {
    Buffer& buffer = editor.buffer;
    if (!buffer.has_sel) {
        /* 无选区: 复制当前行(含行尾换行符), 光标回到行首, 不选中整行 */
        int row = buffer.row;
        editor.clipboard = buffer.lines[row] + Text(1, U'\n');
        buffer.SetCursor(Pos(row, 0));
        editor.status = TextOf("Copied line");
        return;
    }
    editor.clipboard = buffer.GetSelectionText();   /* 选区保留 */
    editor.status = TextOf("Copied");
}

void Commands::Cut() {
    Buffer& buffer = editor.buffer;
    if (!buffer.has_sel) {
        /* 无选区: 剪切当前行(含行尾换行符) */
        Edit([this, &buffer]() {
            editor.clipboard = buffer.lines[buffer.row] + Text(1, U'\n');
            PushUndo();
            editor.undo_group.reset();
            DeleteCurrentLine();
            editor.status = TextOf("Cut");
        });
        return;
    }
    Edit([this, &buffer]() {
        editor.clipboard = buffer.GetSelectionText();
        PushUndo();
        editor.undo_group.reset();
        std::pair<Pos, Pos> range = buffer.GetSelection();
        buffer.Delete(range.first, range.second);
        editor.status = TextOf("Cut");
    });
}

void Commands::Paste() {
    if (editor.clipboard.empty()) {
        editor.status = TextOf("Clipboard empty");
        return;
    }
    Buffer& buffer = editor.buffer;
    Text text = editor.clipboard;
    Edit([this, &buffer, text]() {
        PushUndo();
        editor.undo_group.reset();
        if (buffer.has_sel) {
            std::pair<Pos, Pos> range = buffer.GetSelection();
            buffer.Delete(range.first, range.second);   /* 有选区: 用剪贴板内容替换选区 */
        }
        buffer.Insert(Pos(buffer.row, buffer.col), text);
        editor.status = TextOf("Pasted");
    });
}

void Commands::Duplicate() {
    /* 重复当前行或选中内容; 重复后光标与选区范围保持不变 */
    Edit([this]() {
        Buffer& buffer = editor.buffer;
        PushUndo();
        editor.undo_group.reset();
        int cursor_row = buffer.row;
        int cursor_col = buffer.col;
        int anchor_row = buffer.sel_row;
        int anchor_col = buffer.sel_col;
        bool has_sel = buffer.has_sel;
        if (buffer.has_sel) {
            Text selection_text = buffer.GetSelectionText();
            Pos selection_end = buffer.GetSelection().second;
            buffer.Insert(selection_end, selection_text);   /* 副本插在选区之后 */
        } else {
            DuplicateCurrentLine();
        }
        buffer.row = cursor_row;
        buffer.col = cursor_col;
        buffer.sel_row = anchor_row;
        buffer.sel_col = anchor_col;
        buffer.has_sel = has_sel;
        buffer.ClampCursor();
        editor.status = TextOf("Duplicated");
    });
}

void Commands::DeleteLine() {
    /* Shift+D: 有选区删选中区域, 否则删当前行 */
    Edit([this]() {
        Buffer& buffer = editor.buffer;
        PushUndo();
        editor.undo_group.reset();
        if (buffer.has_sel) {
            std::pair<Pos, Pos> range = buffer.GetSelection();
            buffer.Delete(range.first, range.second);
        } else {
            DeleteCurrentLine();
        }
        editor.status = TextOf("Deleted");
    });
}

void Commands::ChangeCase(bool upper) {
    /* U / Shift+U: 有选区转换选中部分, 无选区转换光标所在单词。
       是否留下撤销记录由 Edit 统一判断(文本无变化则自动回滚)。 */
    Buffer& buffer = editor.buffer;
    if (!buffer.has_sel && !CurrentWordRange().has_value()) return;
    Edit([this, upper]() {
        PushUndo();
        editor.undo_group.reset();
        if (editor.buffer.has_sel) {
            ApplyCase(upper);
        } else {
            ApplyWordCase(upper);
        }
    });
}

void Commands::ApplyCase(bool upper) {
    /* 转换选中区域; 无选区时转换整行 */
    Buffer& buffer = editor.buffer;
    if (buffer.has_sel) {
        std::pair<Pos, Pos> range = buffer.GetSelection();
        Pos start = range.first;
        Pos end = range.second;
        if (start.first == end.first) {
            Text& line = buffer.lines[start.first];
            Text segment = Slice(line, start.second, end.second);
            Text converted = upper ? ToUpperText(segment) : ToLowerText(segment);
            line = Slice(line, 0, start.second) + converted + line.substr((size_t)end.second);
        } else {
            Text& line = buffer.lines[start.first];
            Text tail = line.substr((size_t)start.second);
            line = Slice(line, 0, start.second) + (upper ? ToUpperText(tail) : ToLowerText(tail));
            for (int row = start.first + 1; row < end.first; ++row) {
                buffer.lines[row] = upper ? ToUpperText(buffer.lines[row])
                                          : ToLowerText(buffer.lines[row]);
            }
            Text& last_line = buffer.lines[end.first];
            Text head = last_line.substr(0, (size_t)end.second);
            last_line = (upper ? ToUpperText(head) : ToLowerText(head))
                      + last_line.substr((size_t)end.second);
        }
    } else {
        int row = buffer.row;
        buffer.lines[row] = upper ? ToUpperText(buffer.lines[row])
                                  : ToLowerText(buffer.lines[row]);
    }
    buffer.dirty = true;
}

std::optional<Pos> Commands::CurrentWordRange() {
    /* 返回光标所在单词的 [start, end) 区间; 光标不在任何单词上则返回 nullopt */
    Buffer& buffer = editor.buffer;
    const Text& line = buffer.lines[buffer.row];
    int length = (int)line.size();
    int col = buffer.col;
    int start = 0;
    int end = 0;
    if (col < length && IsWordChar(line[col])) {
        start = col;
        while (start > 0 && IsWordChar(line[start - 1])) start -= 1;
        end = col;
        while (end < length && IsWordChar(line[end])) end += 1;
    } else if (col > 0 && IsWordChar(line[col - 1])) {
        /* 光标落在非单词字符上: 取紧邻左侧的单词 */
        end = col;
        start = col;
        while (start > 0 && IsWordChar(line[start - 1])) start -= 1;
    } else {
        return std::nullopt;
    }
    return Pos(start, end);
}

bool Commands::ApplyWordCase(bool upper) {
    /* 仅转换光标所在单词(连续英文+数字); 返回是否发生修改 */
    std::optional<Pos> word_range = CurrentWordRange();
    if (!word_range.has_value()) return false;
    int start = word_range->first;
    int end = word_range->second;
    Buffer& buffer = editor.buffer;
    Text& line = buffer.lines[buffer.row];
    Text word = Slice(line, start, end);
    line = Slice(line, 0, start)
         + (upper ? ToUpperText(word) : ToLowerText(word))
         + line.substr((size_t)end);
    buffer.dirty = true;
    return true;
}

void Commands::ChangeIndent(bool dedent) {
    /* T / Shift+T: 缩进或减少缩进; 文本无变化的减少缩进不记撤销(由 Edit 判定) */
    Edit([this, dedent]() {
        PushUndo();
        editor.undo_group.reset();
        ApplyIndent(dedent);
    });
}

void Commands::ApplyIndent(bool dedent) {
    /* 处理当前行(无选区)或选区涵盖的所有行; 每档缩进为 4 空格。
       保留调用前的选区不变: 不对行重新整体选中, 只把光标/锚点按各行的
       实际缩进变化量整体平移(缩进 +4, 减少缩进 -4 或 -1)。 */
    Buffer& buffer = editor.buffer;
    int first_row = 0;
    int last_row = 0;
    if (buffer.has_sel) {
        first_row = std::min(buffer.sel_row, buffer.row);
        last_row = std::max(buffer.sel_row, buffer.row);
    } else {
        first_row = last_row = buffer.row;
    }
    int cursor_row = buffer.row;
    int cursor_col = buffer.col;
    int anchor_row = buffer.sel_row;
    int anchor_col = buffer.sel_col;
    bool has_sel = buffer.has_sel;
    std::map<int, int> deltas;
    for (int row = first_row; row <= last_row; ++row) {
        if (dedent) {
            Text& line = buffer.lines[row];
            if (line.compare(0, 4, U"    ") == 0) {
                buffer.Delete(Pos(row, 0), Pos(row, 4));
                deltas[row] = -4;
            } else if (!line.empty() && line[0] == U'\t') {
                buffer.Delete(Pos(row, 0), Pos(row, 1));
                deltas[row] = -1;
            } else {
                deltas[row] = 0;
                /* 行首无可移除的缩进档时保持不变 */
            }
        } else {
            buffer.Insert(Pos(row, 0), TextOf("    "));
            deltas[row] = 4;
        }
    }
    /* 按各行缩进变化量平移光标与选区锚点(行号不变, 只移动列) */
    buffer.row = cursor_row;
    buffer.col = std::max(0, std::min(cursor_col + deltas[cursor_row],
                                      buffer.LineLength(cursor_row)));
    if (has_sel) {
        buffer.sel_row = anchor_row;
        buffer.sel_col = std::max(0, std::min(anchor_col + deltas[anchor_row],
                                             buffer.LineLength(anchor_row)));
        buffer.has_sel = true;
    } else {
        buffer.has_sel = false;
    }
}

std::optional<Pos> Commands::ParseGoto(const Text& text) {
    /* 解析 G/Goto 输入的行列。语法:
           R          跳到第 R 行行首(列 1)
           R:C / R.C  跳到第 R 行第 C 列
           :C / .C    R 空缺, 在当前行跳到第 C 列
       R、C 均可省略 / 为负; R 的 0 视为 1; 越界均被安全钳制。
       未指定列时: 仅 R 为正数越界(超过总行数)停在末行行尾, 其余(含负数 R)停在行首。
       列的正数越界(如第 len+1 位及以后)停在行尾; 负数按精确位置从行尾数:
       -1 = 行尾(最后一个字符之后), -2 = 行尾前一格, -3 再前一格。
       返回 (row, col) 或 nullopt(无法解析)。
       注: 行号与列号均为 1-based(列 1 = 第 1 个字符); 内部统一换算为缓冲区 0-based 列索引。 */
    Buffer& buffer = editor.buffer;
    Text trimmed = Strip(text);
    if (trimmed.empty()) return std::nullopt;
    Text row_part;
    std::optional<Text> col_part;
    size_t colon = trimmed.find(U':');
    size_t dot = trimmed.find(U'.');
    if (colon != Text::npos) {
        row_part = trimmed.substr(0, colon);
        col_part = trimmed.substr(colon + 1);
    } else if (dot != Text::npos) {
        row_part = trimmed.substr(0, dot);
        col_part = trimmed.substr(dot + 1);
    } else {
        row_part = trimmed;
    }
    int line_count = buffer.LineCount();
    /* 行 */
    int row = 0;
    bool to_end = false;
    if (row_part.empty()) {
        row = buffer.row;
        to_end = false;
    } else {
        long value = 0;
        if (!ParseInt(row_part, value)) return std::nullopt;
        if (value == 0) {
            value = 1;                          /* 0 视为第 1 行 */
        } else if (value < 0) {
            value = line_count + value + 1;     /* 负数从末尾: -1=末行 */
        }
        /* 仅正数越界(如 9999)时, 未指定列才定位到末行行尾;
           负数只是按倒数行号取行, 未指定列时停在行首。 */
        to_end = value > line_count;
        if (value < 1) value = 1;
        if (value > line_count) value = line_count;
        row = (int)value - 1;
    }
    /* 列: 1-based; 负数按"从行尾边界数"的精确位置处理 */
    int col = 0;
    int length = (int)buffer.lines[row].size();
    if (!col_part.has_value()) {
        col = to_end ? length : 0;
    } else {
        long value = 0;
        if (!ParseInt(*col_part, value)) return std::nullopt;
        if (value > 0) {
            /* 1-based -> 0-based; 超过行长度(第 len+1 位及以后)一律落在行尾 */
            col = (int)std::min<long>(value - 1, length);
        } else if (value == 0) {
            col = 0;                            /* 0 视为首列, 与行号一致 */
        } else {
            /* 负数从行尾边界数: -1=行尾(len), -2=行尾前一格(len-1)... */
            col = (int)std::max<long>(0, length + value + 1);
        }
    }
    return Pos(row, col);
}

std::optional<Pos> Commands::Search(const Text& query, bool reverse,
                                    std::optional<int> start_row,
                                    std::optional<int> start_col) {
    /* 查找实现: 返回 (row, col) 或 nullopt */
    Buffer& buffer = editor.buffer;
    if (query.empty()) return std::nullopt;
    int line_count = buffer.LineCount();
    if (!start_row.has_value()) {
        start_row = buffer.row;
        start_col = buffer.col;
    }
    int begin_row = *start_row;
    int begin_col = start_col.has_value() ? *start_col : 0;
    if (!reverse) {
        for (int row = begin_row; row < line_count; ++row) {
            int col = FindFrom(buffer.lines[row], query, row == begin_row ? begin_col : 0);
            if (col >= 0) return Pos(row, col);
        }
        return std::nullopt;
    }
    for (int row = begin_row; row >= 0; --row) {
        int end = (row == begin_row) ? begin_col : (int)buffer.lines[row].size();
        int col = RFindBefore(buffer.lines[row], query, end);
        if (col >= 0) return Pos(row, col);
    }
    return std::nullopt;
}

void Commands::FindNext(const Text& query, bool reverse) {
    /* F / Shift+F: 查找并把光标移到命中位置 */
    Buffer& buffer = editor.buffer;
    if (query.empty()) return;
    std::optional<Pos> target;
    if (reverse) {
        target = Search(query, true, buffer.row, buffer.col);
    } else {
        target = Search(query, false, buffer.row, buffer.col + 1);
    }
    if (!target.has_value()) {
        editor.status = TextOf("No match: ") + query;
        return;
    }
    buffer.SetCursor(*target);
    editor.status = TextOf("Find: ") + query;
}

void Commands::Save() {
    if (editor.filename.empty()) {
        editor.prompt_mode = "saveas";
        editor.prompt_text.clear();
        return;
    }
    editor.SaveFile(editor.filename);
    editor.saved = editor.buffer.lines;      /* 更新干净基准 */
    RefreshDirty();
    editor.status = TextOf("Saved ") + Utf8ToText(editor.filename);
}

void Commands::Close() {
    if (editor.buffer.dirty) {
        if (!editor.filename.empty()) {
            editor.SaveFile(editor.filename);
        } else {
            editor.prompt_mode = "saveas";
            editor.prompt_text.clear();
            return;
        }
    }
    editor.running = false;
}

/* ===========================================================================
 * Editor 实现
 * ========================================================================= */
static bool FileExists(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

Editor::Editor(Screen& stdscr, const std::string& filename)
    : stdscr(stdscr), filename(""), mode(1), top(0), left(0),
      goto_select(false), running(true), commands(*this) {
    LoadFile(filename);
    this->filename = filename;
    /* 干净基准: 已存在文件 -> 以磁盘内容为准(初始 clean);
              新建/无文件名 -> 尚未保存(始终 dirty, 直到首次保存) */
    if (!filename.empty() && FileExists(filename)) {
        saved = buffer.lines;
    } else {
        saved.reset();
    }
    commands.RefreshDirty();
}

/* ---- 文件读写交互 ---- */
void Editor::LoadFile(const std::string& path) {
    /* 读取文件填充 Buffer; 文件不存在/不可读时保持空文档 */
    buffer.lines.assign(1, Text());
    buffer.row = 0;
    buffer.col = 0;
    buffer.has_sel = false;
    buffer.dirty = false;
    if (path.empty()) return;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return;
    std::string raw;
    char chunk[4096];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) raw.append(chunk, got);
    fclose(file);
    /* \r\n / \r 归一为 \n */
    std::string normalized;
    normalized.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            normalized.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
        } else {
            normalized.push_back(raw[i]);
        }
    }
    buffer.lines = SplitLines(Utf8ToText(normalized));
    if (buffer.lines.empty()) buffer.lines.assign(1, Text());
    buffer.ClampCursor();
}

void Editor::SaveFile(const std::string& path) {
    /* 把 Buffer 内容写入文件 */
    Text joined;
    for (size_t i = 0; i < buffer.lines.size(); ++i) {
        if (i) joined.push_back(U'\n');
        joined += buffer.lines[i];
    }
    std::string text = TextToUtf8(joined);
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return;
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    buffer.dirty = false;
}

/* ---- 渲染 ---- */
void Editor::AddText(int screen_row, int column, const Text& text, int attribute) {
    stdscr.addstr(screen_row, column, text, attribute);
}

void Editor::AddTextRun(const Text& text, bool selected) {
    stdscr.addstr(text, selected ? A_REVERSE : A_NORMAL);
}

void Editor::DrawLine(int screen_row, int line_index) {
    int width = Width();
    const Text& line = buffer.lines[line_index];
    Text segment = Slice(line, left, left + width);
    if (!buffer.has_sel) {
        AddText(screen_row, 0, segment);
        return;
    }
    /* 有选区: 按运行(选中/未选中)分段绘制 */
    stdscr.move(screen_row, 0);
    int column = left;
    Text run_text;
    bool run_selected = buffer.IsInSelection(line_index, column);
    for (Char ch : segment) {
        bool selected = buffer.IsInSelection(line_index, column);
        if (selected != run_selected) {
            AddTextRun(run_text, run_selected);
            run_text.clear();
            run_selected = selected;
        }
        run_text.push_back(ch);
        column += 1;
    }
    if (!run_text.empty()) AddTextRun(run_text, run_selected);
    /* 选区包含行尾换行符时, 用一个反色的 ASCII 宽度空白表示该换行符 */
    if (buffer.IsNewlineSelected(line_index)) {
        int marker_x = 0;
        for (Char ch : segment) marker_x += DisplayWidth(ch);
        if (marker_x < width) AddText(screen_row, marker_x, TextOf(" "), A_REVERSE);
    }
}

void Editor::DrawStatus(int height, int width) {
    int screen_row = height - 1;
    stdscr.move(screen_row, 0);
    stdscr.clrtoeol();
    Text mode_text = (mode == 0) ? TextOf("EDIT") : TextOf("COMMAND");
    Text filename_text = filename.empty() ? TextOf("[untitled]") : Utf8ToText(filename);
    Text message;
    if (prompt_mode == "find") {
        message = TextOf("Find: ") + prompt_text;
    } else if (prompt_mode == "findrev") {
        message = TextOf("Find(rev): ") + prompt_text;
    } else if (prompt_mode == "goto") {
        message = TextOf("Goto line: ") + prompt_text;
    } else if (prompt_mode == "saveas") {
        message = TextOf("Save as: ") + prompt_text;
    } else if (!status.empty()) {
        message = status;
    } else {
        char position[64];
        snprintf(position, sizeof position, "L%d C%d", buffer.row + 1, buffer.col + 1);
        message = TextOf(position);
    }
    Text dirty_mark = buffer.dirty ? TextOf(" (*)") : Text();
    Text base = mode_text + TextOf(" | ") + filename_text + dirty_mark;
    AddText(screen_row, 0, Slice(base + TextOf("  ") + message, 0, width - 1));
}

void Editor::Render() {
    commands.RefreshDirty();    /* 每帧以保存快照重算脏标记, 保证撤销/重做后即时准确 */
    stdscr.CheckResize();
    int height = Height();
    int width = Width();
    int text_rows = TextRows();
    /* 滚动: 保证光标可见 */
    if (buffer.row < top) top = buffer.row;
    while (buffer.row >= top + text_rows) top += 1;
    if (buffer.col < left) left = buffer.col;
    while (buffer.col >= left + width) left += 1;

    stdscr.erase();
    for (int screen_row = 0; screen_row < text_rows; ++screen_row) {
        int line_index = top + screen_row;
        if (line_index >= buffer.LineCount()) break;
        DrawLine(screen_row, line_index);
    }
    /* 分隔符 */
    stdscr.move(height - 2, 0);
    stdscr.hline(U'-', width);
    DrawStatus(height, width);
    /* 硬件光标定位(可见方块光标, 跨平台稳定; 多宽度用显示宽度对齐) */
    int cursor_y = buffer.row - top;
    int cursor_x = 0;
    for (Char ch : Slice(buffer.lines[buffer.row], left, buffer.col)) {
        cursor_x += DisplayWidth(ch);
    }
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y > height - 3) cursor_y = height - 3;
    if (cursor_x > width - 1) cursor_x = width - 1;
    stdscr.move(cursor_y, cursor_x);
    stdscr.refresh();
}

/* ---- 主循环 ---- */
void Editor::Run() {
    while (running) {
        Render();
        Char ch = stdscr.get_wch();
        if (ch == KEY_EOF) {
            running = false;
            break;
        }
        if (ch == 3) continue;          /* 裸 Ctrl-C 字符情形 */
        HandleKey(ch);
    }
}

/* ---- 按键分发 ---- */
void Editor::HandleKey(Char ch) {
    if (ch == 3) return;                /* 屏蔽 Ctrl-C */
    if (prompt_mode.has_value()) {
        HandlePrompt(ch);
        return;
    }
    if (ch == KEY_ESC) {                /* Esc: 切换模式 */
        mode = 1 - mode;
        undo_group.reset();
        status.clear();
        return;
    }
    if (ch == KEY_BTAB) {               /* Shift+Tab: 减少缩进(两种模式通用) */
        commands.ChangeIndent(true);
        return;
    }
    if (mode == 0) {
        HandleEdit(ch);
    } else {
        HandleCommand(ch);
    }
}

void Editor::HandleEdit(Char ch) {
    /* 编辑模式: 把原始按键路由到 Commands 的编辑命令(内含撤销合并) */
    if (ch == U'\n' || ch == U'\r') {
        commands.SplitLine();
        return;
    }
    if (ch == U'\t') {                  /* Tab 等同 T: 缩进当前行/选中行 */
        commands.ChangeIndent(false);
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        commands.DeleteCharacter(true);
        return;
    }
    if (ch == KEY_DC) {
        commands.DeleteCharacter(false);
        return;
    }
    if (IsKeyCode(ch)) {                /* 方向键/Home/End/PageUp/Down */
        HandleMoveKey(ch, false);
        return;
    }
    /* 普通字符(含中文等多字节) */
    commands.TypeCharacter(ch);
}

void Editor::HandleCommand(Char ch) {
    /* 命令模式同样响应 Backspace / Delete / Enter / Tab(与编辑模式一致) */
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        commands.DeleteCharacter(true);
        return;
    }
    if (ch == KEY_DC) {
        commands.DeleteCharacter(false);
        return;
    }
    if (IsKeyCode(ch)) {
        HandleMoveKey(ch, false);
        return;
    }
    if (ch == U'\n' || ch == U'\r') {
        commands.SplitLine();
        return;
    }
    if (ch == U'\t') {
        commands.ChangeIndent(false);   /* Tab 等同 T */
        return;
    }
    bool up = IsUpper(ch);
    Char key = ToLower(ch);
    if (key == U' ') {
        commands.TypeCharacter(U' ');   /* 空格: 两模式通用, 插入空格 */
    } else if (key == U'i') {
        buffer.Move(-1, 0, up);
    } else if (key == U'k') {
        buffer.Move(1, 0, up);
    } else if (key == U'j') {
        buffer.Move(0, -1, up);
    } else if (key == U'l') {
        buffer.Move(0, 1, up);
    } else if (key == U'[') {           /* 上翻页 */
        buffer.Move(-TextRows(), 0, false);
    } else if (key == U']') {           /* 下翻页 */
        buffer.Move(TextRows(), 0, false);
    } else if (key == U'{') {           /* 上翻页并选中 */
        buffer.Move(-TextRows(), 0, true);
    } else if (key == U'}') {           /* 下翻页并选中 */
        buffer.Move(TextRows(), 0, true);
    } else if (key == U'a') {
        commands.SelectAll();
    } else if (key == U'x') {
        commands.Cut();
    } else if (key == U'c') {
        commands.Copy();
    } else if (key == U'v') {
        commands.Paste();
    } else if (key == U'z') {
        if (up) {
            commands.Redo();
        } else {
            commands.Undo();
        }
    } else if (key == U'f') {
        prompt_mode = up ? "findrev" : "find";
        prompt_text.clear();
        undo_group.reset();
    } else if (key == U'g') {
        prompt_mode = "goto";
        goto_select = up;               /* Shift+G: 从(选区)起点选择到跳转位置 */
        prompt_text.clear();
        undo_group.reset();
    } else if (key == U'u') {
        commands.ChangeCase(up);
    } else if (key == U't') {
        commands.ChangeIndent(up);      /* T 缩进 / Shift+T 减少缩进 */
    } else if (key == U'b') {
        commands.DeleteCharacter(!up);  /* B=Backspace, Shift+B=Delete */
    } else if (key == U'd') {
        if (up) {
            commands.DeleteLine();
        } else {
            commands.Duplicate();
        }
    } else if (key == U's') {
        if (up) {
            prompt_mode = "saveas";
            prompt_text.clear();
            undo_group.reset();
        } else {
            commands.Save();
        }
    } else if (key == U'q') {
        if (up) {                       /* Shift+Q: 不保存退出 */
            running = false;
        } else {                        /* Q: 提示保存后退出 */
            commands.Close();
        }
    } else if (key == U'w') {
        commands.Close();
    }
}

void Editor::HandleMoveKey(Char key, bool shift) {
    if (key == KEY_UP) {
        buffer.Move(-1, 0, shift);
    } else if (key == KEY_DOWN) {
        buffer.Move(1, 0, shift);
    } else if (key == KEY_LEFT) {
        buffer.Move(0, -1, shift);
    } else if (key == KEY_RIGHT) {
        buffer.Move(0, 1, shift);
    } else if (key == KEY_HOME) {
        buffer.MoveToLineStart(shift);
    } else if (key == KEY_END) {
        buffer.MoveToLineEnd(shift);
    } else if (key == KEY_PPAGE) {
        buffer.Move(-TextRows(), 0, shift);
    } else if (key == KEY_NPAGE) {
        buffer.Move(TextRows(), 0, shift);
    }
}

/* ---- 提示子模式(F / Shift+F / G / Shift+S) ---- */
void Editor::HandlePrompt(Char ch) {
    if (ch == KEY_ESC) {                /* Esc 取消 */
        prompt_mode.reset();
        prompt_text.clear();
        goto_select = false;
        return;
    }
    if (ch == U'\n' || ch == U'\r') {
        ConfirmPrompt();
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (!prompt_text.empty()) {
            prompt_text.pop_back();     /* 按字符删除(支持中文) */
        }
        return;
    }
    if (IsKeyCode(ch)) return;          /* 提示态忽略方向键 */
    prompt_text.push_back(ch);
}

void Editor::ConfirmPrompt() {
    std::optional<std::string> mode = prompt_mode;
    Text text = prompt_text;
    prompt_mode.reset();
    prompt_text.clear();
    if (mode == "find") {
        commands.FindNext(text, false);
    } else if (mode == "findrev") {
        commands.FindNext(text, true);
    } else if (mode == "goto") {
        bool select = goto_select;
        goto_select = false;
        std::optional<Pos> target = commands.ParseGoto(text);
        if (!target.has_value()) {
            status = TextOf("Invalid line");
            return;
        }
        if (select) {
            /* 从(选区)起点选择到跳转目标位置 */
            buffer.SelectTo(*target, true);
        } else {
            buffer.SetCursor(*target);
        }
    } else if (mode == "saveas") {
        if (!text.empty()) {
            filename = TextToUtf8(text);
            commands.Save();
        }
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */
static void Main(Screen& stdscr, int argc, char** argv) {
    std::string filename = argc > 1 ? argv[1] : "";
    Editor(stdscr, filename).Run();
}

int main(int argc, char** argv) {
    Screen stdscr;
    stdscr.Init();
    Main(stdscr, argc, argv);
    stdscr.Shutdown();
    return 0;
}
