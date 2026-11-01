/* main.cpp - 程序入口与主循环 */

#include "mini.hpp"

Editor ed;

/* ---------------- 窗口尺寸变化处理(Unix / mintty-pty) ---------------- */

#if !defined(_WIN32) || defined(HAVE_POSIX_TTY)
static void sig_winch(int sig) { (void)sig; ed.resize_flag = 1; }
#endif

/* ---------------- 主循环 ---------------- */

int main(int argc, char** argv) {
    if (argc >= 2) {
        ed.filename = argv[1];
        ed.buffer.load(ed.filename);
    } else {
        ed.filename.clear();
        ed.buffer.load("");      /* 打开失败 -> 单行空白文档 */
    }
    ed.history.clear();
    ed.history.push_back(Snapshot{ ed.buffer.snapshot() });
    ed.hist_len = 1; ed.hist_pos = 0; ed.last_coalesce = 0;

    ed.enableRaw();
#ifndef _WIN32
    signal(SIGWINCH, sig_winch);
    ed.getWinsize();
#else
# ifdef HAVE_POSIX_TTY
    if (!ed.real_console) signal(SIGWINCH, sig_winch);   /* mintty/pty 下靠 SIGWINCH 感知窗口变化 */
# endif
    ed.getWinsize();
#endif
    std::atexit([]() { ed.disableRaw(); });

    std::string seq;
    while (ed.running) {
        if (ed.resize_flag) { ed.resize_flag = 0; ed.getWinsize(); }
        ed.render();

        int key = ed.getKey(seq);
        if (key == KEY_INT) { ed.running = 0; break; }   /* Ctrl-C: 退出并恢复屏幕(atexit 清屏) */
        ed.handleKey(key, seq);
    }
    return 0;
}
