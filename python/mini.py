"""mini-editor - 一个仅用 ASCII 控制键、键盘驱动的极简终端文本编辑器

两种模式:
    编辑模式(默认) / 命令模式
    按 Esc 切换

命令模式:
    I J K L [ ]  移动光标 (上/左/下/右/上翻页/下翻页)
    Shift+I/J/K/L/[/]  移动并选中
    A 全选  X 剪切  C 复制  V 粘贴
    Z 撤销  Shift+Z 重做
    F 查找  Shift+F 反向查找
    G 跳转到行
    U 转小写  Shift+U 转大写
    D 复制当前行  Shift+D 删除当前行
    S 保存  Shift+S 另存
    W 关闭(未保存提示)
    Q 退出(未保存提示)  Shift+Q 不保存退出

编辑模式:
    打字插入
    Enter 换行  Backspace/Delete 删除
    方向键/Home/End/PageUp/Down 移动

"""

import os
# 缩短 Esc 判定延迟(否则单独按 Esc 切换模式会有 ~1s 卡顿)
os.environ.setdefault('ESCDELAY', '25')

import sys
import copy
import curses
import signal
import unicodedata

# get_wch 对单独的 Esc 返回的是字符串 '\x1b'(而非整数 27), 这里统一定义
KEY_ESC = 27
ESC_STR = '\x1b'


# ---------------------------------------------------------------------------
# 显示宽度(仅用于光标定位对齐; 渲染本身交给 curses 处理多宽度字符)
# ---------------------------------------------------------------------------
def DispWidth(ch):
    if ch == '\t':
        return 4
    try:
        return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1
    except Exception:
        return 1


# ---------------------------------------------------------------------------
# 文本缓冲区(整文件: 每行一个字符串)
# ---------------------------------------------------------------------------
class Buffer:
    def __init__(self):
        self.lines = ['']
        self.row = 0
        self.col = 0
        self.sel_row = 0
        self.sel_col = 0
        self.has_sel = False
        self.dirty = False

    # ---- 行列换算 ----
    def LineCount(self):
        return len(self.lines)

    def LineLen(self, r):
        return len(self.lines[r])

    def Clamp(self):
        if self.row < 0:
            self.row = 0
        if self.row >= len(self.lines):
            self.row = len(self.lines) - 1
        if self.row < 0:
            self.row = 0
        if self.col < 0:
            self.col = 0
        if self.col > len(self.lines[self.row]):
            self.col = len(self.lines[self.row])

    def Move(self, dr, dc, extend):
        if extend and not self.has_sel:
            self.sel_row, self.sel_col = self.row, self.col
            self.has_sel = True
        if not extend:
            self.has_sel = False
        if dc > 0:
            # 右移: 到行尾继续按则移动到下一行行首
            rem = dc
            while rem > 0 and self.row < len(self.lines):
                if self.col < len(self.lines[self.row]):
                    self.col += 1
                elif self.row < len(self.lines) - 1:
                    self.row += 1
                    self.col = 0
                else:
                    break
                rem -= 1
        elif dc < 0:
            # 左移: 到行首继续按则移动到上一行行尾
            rem = -dc
            while rem > 0 and self.row >= 0:
                if self.col > 0:
                    self.col -= 1
                elif self.row > 0:
                    self.row -= 1
                    self.col = len(self.lines[self.row])
                else:
                    break
                rem -= 1
        else:
            self.row += dr
        self.Clamp()

    # ---- 编辑操作 ----
    def Normalize(self, text):
        return text.replace('\t', '    ')  # 制表符统一为 4 空格, 避免宽度错位

    def InsertChar(self, ch):
        ch = self.Normalize(ch)
        line = self.lines[self.row]
        self.lines[self.row] = line[:self.col] + ch + line[self.col:]
        self.col += len(ch)
        self.dirty = True

    def InsertText(self, text):
        # 支持多行插入(粘贴)
        text = self.Normalize(text)
        if '\n' not in text:
            self.InsertChar(text)
            return
        line = self.lines[self.row]
        before = line[:self.col]
        after = line[self.col:]
        segs = text.split('\n')
        self.lines[self.row] = before + segs[0]
        for i in range(1, len(segs) - 1):
            self.lines.insert(self.row + i, segs[i])
        last = self.row + len(segs) - 1
        self.lines.insert(last, segs[-1] + after)
        self.row = last
        self.col = len(segs[-1])
        self.dirty = True

    def InsertNewline(self):
        line = self.lines[self.row]
        self.lines[self.row] = line[:self.col]
        self.lines.insert(self.row + 1, line[self.col:])
        self.row += 1
        self.col = 0
        self.dirty = True

    def DeleteBefore(self):
        # 删除光标前的字符; 光标随之左移(用于 Backspace 键)
        if self.col > 0:
            line = self.lines[self.row]
            self.lines[self.row] = line[:self.col - 1] + line[self.col:]
            self.col -= 1
        elif self.row > 0:
            prev = self.row - 1
            self.lines[prev] = self.lines[prev] + self.lines[self.row]
            del self.lines[self.row]
            self.row = prev
            self.col = len(self.lines[prev])
        else:
            return
        self.dirty = True

    def DeleteAfter(self):
        # 删除光标后的字符; 光标位置不变(用于 Delete 键)
        if self.col < len(self.lines[self.row]):
            line = self.lines[self.row]
            self.lines[self.row] = line[:self.col] + line[self.col + 1:]
        elif self.row < len(self.lines) - 1:
            self.lines[self.row] = self.lines[self.row] + self.lines[self.row + 1]
            del self.lines[self.row + 1]
        else:
            return
        self.dirty = True

    # ---- 选区 ----
    def OrderedSel(self):
        a = (self.sel_row, self.sel_col)
        b = (self.row, self.col)
        if a > b:
            a, b = b, a
        return a, b

    def InSel(self, r, c):
        if not self.has_sel:
            return False
        a, b = self.OrderedSel()
        return a <= (r, c) <= b

    def SelectionText(self):
        if not self.has_sel:
            return ''
        a, b = self.OrderedSel()
        if a[0] == b[0]:
            return self.lines[a[0]][a[1]:b[1]]
        parts = [self.lines[a[0]][a[1]:]]
        for r in range(a[0] + 1, b[0]):
            parts.append(self.lines[r])
        parts.append(self.lines[b[0]][:b[1]])
        return '\n'.join(parts)

    def DeleteSelection(self):
        if not self.has_sel:
            return
        a, b = self.OrderedSel()
        if a[0] == b[0]:
            line = self.lines[a[0]]
            self.lines[a[0]] = line[:a[1]] + line[b[1]:]
        else:
            self.lines[a[0]] = self.lines[a[0]][:a[1]] + self.lines[b[0]][b[1]:]
            del self.lines[a[0] + 1:b[0] + 1]
        self.row, self.col = a
        self.has_sel = False
        self.dirty = True

    # ---- 文本操作 ----
    def SetLine(self, r, s):
        self.lines[r] = s
        self.dirty = True

    def DuplicateLine(self):
        self.lines.insert(self.row + 1, self.lines[self.row])
        self.row += 1
        self.col = 0
        self.dirty = True

    def DeleteLine(self):
        if len(self.lines) == 1:
            self.lines[0] = ''
        else:
            del self.lines[self.row]
            if self.row >= len(self.lines):
                self.row = len(self.lines) - 1
        self.col = 0
        self.dirty = True

    def ChangeCase(self, upper):
        if self.has_sel:
            a, b = self.OrderedSel()
            if a[0] == b[0]:
                s = self.lines[a[0]]
                self.lines[a[0]] = s[:a[1]] + (s[a[1]:b[1]].upper() if upper else s[a[1]:b[1]].lower()) + s[b[1]:]
            else:
                s = self.lines[a[0]]
                self.lines[a[0]] = s[:a[1]] + (s[a[1]:].upper() if upper else s[a[1]:].lower())
                for r in range(a[0] + 1, b[0]):
                    self.lines[r] = self.lines[r].upper() if upper else self.lines[r].lower()
                s = self.lines[b[0]]
                self.lines[b[0]] = (s[:b[1]].upper() if upper else s[:b[1]].lower()) + s[b[1]:]
        else:
            r = self.row
            self.lines[r] = self.lines[r].upper() if upper else self.lines[r].lower()
        self.dirty = True

    # ---- 查找 ----
    def Find(self, query, reverse=False, start_row=None, start_col=None):
        if not query:
            return None
        n = len(self.lines)
        if start_row is None:
            start_row, start_col = self.row, self.col
        if not reverse:
            for r in range(start_row, n):
                c = self.lines[r].find(query, start_col if r == start_row else 0)
                if c >= 0:
                    return (r, c)
            return None
        else:
            for r in range(start_row, -1, -1):
                c = self.lines[r].rfind(query, 0, start_col if r == start_row else len(self.lines[r]))
                if c >= 0:
                    return (r, c)
            return None

    # ---- 文件读写 ----
    def Load(self, path):
        self.lines = ['']
        self.row = self.col = 0
        self.has_sel = False
        self.dirty = False
        if not path:
            return
        try:
            with open(path, 'r', encoding='utf-8') as f:
                text = f.read()
        except (FileNotFoundError, IsADirectoryError, PermissionError):
            return
        text = text.replace('\r\n', '\n').replace('\r', '\n')
        self.lines = text.split('\n')
        if not self.lines:
            self.lines = ['']
        self.Clamp()

    def Save(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.lines))
        self.dirty = False


# ---------------------------------------------------------------------------
# 编辑器(终端/光标/渲染/输入)
# ---------------------------------------------------------------------------
class Editor:
    def __init__(self, stdscr, filename):
        self.stdscr = stdscr
        self.buf = Buffer()
        self.buf.Load(filename)
        self.filename = filename or ''
        self.mode = 0            # 0=编辑 1=命令
        self.top = 0             # 顶部可见行
        self.left = 0            # 水平滚动
        self.clip = ''
        self.status = ''
        self.prompt_mode = None  # 'find' 'findrev' 'goto' 'saveas'
        self.prompt_buf = ''
        self.undo_stack = []
        self.redo_stack = []
        self._group = None       # 连续输入/删除合并组: {'type','next'}
        self.running = True

    # ---- 尺寸 ----
    def Height(self):
        return self.stdscr.getmaxyx()[0]

    def Width(self):
        return self.stdscr.getmaxyx()[1]

    def TextRows(self):
        return max(1, self.Height() - 2)

    # ---- 撤销/重做(快照) ----
    def Snapshot(self):
        return {
            'lines': copy.deepcopy(self.buf.lines),
            'row': self.buf.row, 'col': self.buf.col,
            'sel_row': self.buf.sel_row, 'sel_col': self.buf.sel_col,
            'has_sel': self.buf.has_sel,
        }

    def PushUndo(self):
        self.undo_stack.append(self.Snapshot())
        self.redo_stack = []

    def Restore(self, s):
        self.buf.lines = copy.deepcopy(s['lines'])
        self.buf.row, self.buf.col = s['row'], s['col']
        self.buf.sel_row, self.buf.sel_col = s['sel_row'], s['sel_col']
        self.buf.has_sel = s['has_sel']
        self.buf.Clamp()

    def Undo(self):
        if not self.undo_stack:
            return
        self.redo_stack.append(self.Snapshot())
        self.Restore(self.undo_stack.pop())
        self._group = None

    def Redo(self):
        if not self.redo_stack:
            return
        self.undo_stack.append(self.Snapshot())
        self.Restore(self.redo_stack.pop())
        self._group = None

    def BeginEdit(self, op_type, row, col):
        # 连续输入(insert)/连续退格(back)/连续删除(del) 合并为一次撤销。
        # 判断当前操作的"起始光标位置"是否紧接上一次同类操作之后。
        if op_type == 'insert':
            nxt = (row, col + 1)      # 输入后光标右移一格
        elif op_type == 'back':
            nxt = (row, col - 1)      # 退格后光标左移一格
        else:  # del
            nxt = (row, col)          # Delete 键光标不动
        if (self._group and self._group['type'] == op_type
                and self._group['next'] == (row, col)):
            self._group['next'] = nxt  # 续接: 仅推进期望位置, 不压新快照
            return
        self.PushUndo()
        self._group = {'type': op_type, 'next': nxt}

    # ---- 渲染 ----
    def AddStr(self, y, x, s, attr=0):
        try:
            self.stdscr.addstr(y, x, s, attr)
        except curses.error:
            pass

    def AddRun(self, s, sel):
        try:
            self.stdscr.addstr(s, curses.A_REVERSE if sel else curses.A_NORMAL)
        except curses.error:
            pass

    def DrawLine(self, y, lr):
        w = self.Width()
        line = self.buf.lines[lr]
        seg = line[self.left:self.left + w]
        if not self.buf.has_sel:
            self.AddStr(y, 0, seg)
            return
        # 有选区: 按运行(选区/非选区)分段绘制, curses 负责多宽度对齐
        self.stdscr.move(y, 0)
        ci = self.left
        run = ''
        run_sel = self.buf.InSel(lr, ci)
        for ch in seg:
            s = self.buf.InSel(lr, ci)
            if s != run_sel:
                self.AddRun(run, run_sel)
                run = ''
                run_sel = s
            run += ch
            ci += 1
        if run:
            self.AddRun(run, run_sel)

    def DrawStatus(self, h, w):
        y = h - 1
        self.stdscr.move(y, 0)
        self.stdscr.clrtoeol()
        modestr = 'EDIT' if self.mode == 0 else 'COMMAND'
        fn = self.filename if self.filename else '[untitled]'
        if self.prompt_mode == 'find':
            msg = 'Find: ' + self.prompt_buf
        elif self.prompt_mode == 'findrev':
            msg = 'Find(rev): ' + self.prompt_buf
        elif self.prompt_mode == 'goto':
            msg = 'Goto line: ' + self.prompt_buf
        elif self.prompt_mode == 'saveas':
            msg = 'Save as: ' + self.prompt_buf
        elif self.status:
            msg = self.status
        else:
            msg = 'L%d C%d' % (self.buf.row + 1, self.buf.col + 1)
        base = '%s | %s | %s' % (modestr, '*' if self.buf.dirty else ' ', fn)
        self.AddStr(y, 0, (base + '  ' + msg)[:w - 1])

    def Render(self):
        h, w = self.Height(), self.Width()
        tr = self.TextRows()
        # 滚动: 保证光标可见
        if self.buf.row < self.top:
            self.top = self.buf.row
        while self.buf.row >= self.top + tr:
            self.top += 1
        if self.buf.col < self.left:
            self.left = self.buf.col
        while self.buf.col >= self.left + w:
            self.left += 1

        self.stdscr.erase()
        for i in range(tr):
            lr = self.top + i
            if lr >= self.buf.LineCount():
                break
            self.DrawLine(i, lr)
        # 分隔符
        try:
            self.stdscr.move(h - 2, 0)
            self.stdscr.hline('-', w)
        except curses.error:
            pass
        self.DrawStatus(h, w)
        # 硬件光标定位(可见方块光标 curs_set(1), 跨平台稳定; 多宽度用显示宽度对齐)
        cy = self.buf.row - self.top
        cx = 0
        for ch in self.buf.lines[self.buf.row][self.left:self.buf.col]:
            cx += DispWidth(ch)
        if cy < 0:
            cy = 0
        if cy > h - 3:
            cy = h - 3
        if cx > w - 1:
            cx = w - 1
        try:
            self.stdscr.move(cy, cx)
        except curses.error:
            pass
        self.stdscr.refresh()

    # ---- 主循环 ----
    def Run(self):
        self.stdscr.keypad(True)
        try:
            curses.set_escdelay(25)  # 缩短 Esc 延迟, 否则单独按 Esc 切换模式会有明显卡顿
        except AttributeError:
            pass
        # 可见方块光标: 跨平台稳定(不依赖 xterm 专属的竖线光标转义,
        # 后者在 Windows PDCurses 下可能把转义字节当成字面量污染光标状态)
        curses.curs_set(1)
        # 屏蔽 Ctrl-C: 拦截 SIGINT, 避免误触中断退出
        prev_sigint = signal.signal(signal.SIGINT, lambda *a: None)
        try:
            while self.running:
                self.Render()
                try:
                    ch = self.stdscr.get_wch()
                except KeyboardInterrupt:
                    continue
                if ch == 3 or ch == '\x03':  # 裸 Ctrl-C 字符情形
                    continue
                self.HandleKey(ch)
        finally:
            signal.signal(signal.SIGINT, prev_sigint)
            curses.curs_set(0)

    # ---- 按键分发 ----
    def HandleKey(self, ch):
        if ch == 3 or ch == '\x03':  # 屏蔽 Ctrl-C
            return
        if self.prompt_mode:
            self.HandlePrompt(ch)
            return
        if ch == KEY_ESC or ch == ESC_STR:  # Esc: 切换模式
            self.mode = 1 - self.mode
            self._group = None
            self.status = ''
            return
        if self.mode == 0:
            self.HandleEdit(ch)
        else:
            self.HandleCommand(ch)

    def HandleDelete(self, backspace):
        # 两种模式共用的删除: Backspace 删光标前, Delete 删光标后; 有选区则整段删除
        if self.buf.has_sel:
            self.PushUndo()
            self._group = None
            self.buf.DeleteSelection()
        elif backspace:
            # 连续退格合并为一次撤销
            self.BeginEdit('back', self.buf.row, self.buf.col)
            self.buf.DeleteBefore()
        else:
            # 连续 Delete 合并为一次撤销
            self.BeginEdit('del', self.buf.row, self.buf.col)
            self.buf.DeleteAfter()

    def HandleEdit(self, ch):
        if ch == '\n' or ch == '\r':
            self.PushUndo()
            self._group = None
            self.buf.InsertNewline()
            return
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            self.HandleDelete(backspace=True)
            return
        if ch == curses.KEY_DC:
            self.HandleDelete(backspace=False)
            return
        if isinstance(ch, int):  # 方向键/Home/End/PageUp/Down
            self.HandleMoveKey(ch, shift=False)
            return
        # 普通字符(含中文等多字节): 连续输入合并为一次撤销
        self.BeginEdit('insert', self.buf.row, self.buf.col)
        self.buf.InsertChar(ch)

    def HandleCommand(self, ch):
        # 控制模式同样响应 Backspace / Delete
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            self.HandleDelete(backspace=True)
            return
        if ch == curses.KEY_DC:
            self.HandleDelete(backspace=False)
            return
        if isinstance(ch, int):
            self.HandleMoveKey(ch, shift=False)
            return
        up = ch.isupper()
        lc = ch.lower()
        if lc == 'i':
            self.buf.Move(-1, 0, up)
        elif lc == 'k':
            self.buf.Move(1, 0, up)
        elif lc == 'j':
            self.buf.Move(0, -1, up)
        elif lc == 'l':
            self.buf.Move(0, 1, up)
        elif lc == '[':          # 上翻页
            self.buf.Move(-self.TextRows(), 0, False)
        elif lc == ']':          # 下翻页
            self.buf.Move(self.TextRows(), 0, False)
        elif lc == '{':          # 上翻页并选中
            self.buf.Move(-self.TextRows(), 0, True)
        elif lc == '}':          # 下翻页并选中
            self.buf.Move(self.TextRows(), 0, True)
        elif lc == 'a':
            self.CmdSelectAll()
        elif lc == 'x':
            self.CmdCut()
        elif lc == 'c':
            self.CmdCopy()
        elif lc == 'v':
            self.CmdPaste()
        elif lc == 'z':
            if up:
                self.Redo()
            else:
                self.Undo()
        elif lc == 'f':
            self.prompt_mode = 'findrev' if up else 'find'
            self.prompt_buf = ''
            self._group = None
        elif lc == 'g':
            self.prompt_mode = 'goto'
            self.prompt_buf = ''
            self._group = None
        elif lc == 'u':
            self.PushUndo()
            self._group = None
            self.buf.ChangeCase(up)
        elif lc == 'd':
            if up:
                self.CmdDeleteLine()
            else:
                self.CmdDuplicateLine()
        elif lc == 's':
            if up:
                self.prompt_mode = 'saveas'
                self.prompt_buf = ''
                self._group = None
            else:
                self.CmdSave()
        elif lc == 'q':
            if up:               # Shift+Q: 不保存退出
                self.running = False
            else:                # Q: 提示保存后退出
                self.CmdClose()
        elif lc == 'w':
            self.CmdClose()

    def HandleMoveKey(self, key, shift):
        if key in (curses.KEY_UP, curses.KEY_SR):
            self.buf.Move(-1, 0, shift)
        elif key in (curses.KEY_DOWN, curses.KEY_SF):
            self.buf.Move(1, 0, shift)
        elif key in (curses.KEY_LEFT, curses.KEY_SLEFT):
            self.buf.Move(0, -1, shift)
        elif key in (curses.KEY_RIGHT, curses.KEY_SRIGHT):
            self.buf.Move(0, 1, shift)
        elif key == curses.KEY_HOME:
            if shift and not self.buf.has_sel:
                self.buf.sel_row, self.buf.sel_col = self.buf.row, self.buf.col
                self.buf.has_sel = True
            self.buf.col = 0
            if not shift:
                self.buf.has_sel = False
        elif key == curses.KEY_END:
            if shift and not self.buf.has_sel:
                self.buf.sel_row, self.buf.sel_col = self.buf.row, self.buf.col
                self.buf.has_sel = True
            self.buf.col = len(self.buf.lines[self.buf.row])
            if not shift:
                self.buf.has_sel = False
        elif key == curses.KEY_PPAGE:
            self.buf.Move(-self.TextRows(), 0, shift)
        elif key == curses.KEY_NPAGE:
            self.buf.Move(self.TextRows(), 0, shift)

    # ---- 提示子模式(F / Shift+F / G / Shift+S) ----
    def HandlePrompt(self, ch):
        if ch == KEY_ESC or ch == ESC_STR:  # Esc 取消
            self.prompt_mode = None
            self.prompt_buf = ''
            return
        if ch == '\n' or ch == '\r':
            self.ConfirmPrompt()
            return
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            if self.prompt_buf:
                self.prompt_buf = self.prompt_buf[:-1]  # 按码点删除(支持中文)
            return
        if isinstance(ch, int):
            return  # 提示态忽略方向键
        self.prompt_buf += ch

    def ConfirmPrompt(self):
        mode = self.prompt_mode
        buf = self.prompt_buf
        self.prompt_mode = None
        self.prompt_buf = ''
        if mode == 'find':
            self.DoFind(buf, False)
        elif mode == 'findrev':
            self.DoFind(buf, True)
        elif mode == 'goto':
            try:
                n = int(buf)
            except ValueError:
                self.status = 'Invalid line'
                return
            if 1 <= n <= self.buf.LineCount():
                self.buf.row = n - 1
                self.buf.col = 0
                self.buf.has_sel = False
                self.buf.Clamp()
            else:
                self.status = 'No such line'
        elif mode == 'saveas':
            if buf:
                self.filename = buf
                self.CmdSave()

    def DoFind(self, query, reverse):
        if not query:
            return
        if reverse:
            res = self.buf.Find(query, reverse=True, start_row=self.buf.row, start_col=self.buf.col)
        else:
            res = self.buf.Find(query, reverse=False, start_row=self.buf.row, start_col=self.buf.col + 1)
        if res is None:
            self.status = 'No match: ' + query
            return
        r, c = res
        self.buf.row = r
        self.buf.col = c
        self.buf.sel_row, self.buf.sel_col = r, c
        self.buf.has_sel = True
        self.buf.Clamp()
        self.status = 'Find: ' + query

    # ---- 文件/选区命令 ----
    def CmdSelectAll(self):
        self.buf.sel_row, self.buf.sel_col = 0, 0
        self.buf.row = self.buf.LineCount() - 1
        self.buf.col = len(self.buf.lines[-1])
        self.buf.has_sel = True

    def CmdCopy(self):
        if not self.buf.has_sel:
            self.status = 'No selection'
            return
        self.clip = self.buf.SelectionText()
        self.status = 'Copied'

    def CmdCut(self):
        if not self.buf.has_sel:
            self.status = 'No selection'
            return
        self.clip = self.buf.SelectionText()
        self.PushUndo()
        self._group = None
        self.buf.DeleteSelection()
        self.status = 'Cut'

    def CmdPaste(self):
        if not self.clip:
            self.status = 'Clipboard empty'
            return
        self.PushUndo()
        self._group = None
        self.buf.InsertText(self.clip)
        self.status = 'Pasted'

    def CmdDuplicateLine(self):
        self.PushUndo()
        self._group = None
        self.buf.DuplicateLine()
        self.status = 'Line duplicated'

    def CmdDeleteLine(self):
        self.PushUndo()
        self._group = None
        self.buf.DeleteLine()
        self.status = 'Line deleted'

    def CmdSave(self):
        if not self.filename:
            self.prompt_mode = 'saveas'
            self.prompt_buf = ''
            return
        self.buf.Save(self.filename)
        self.status = 'Saved ' + self.filename

    def CmdClose(self):
        if self.buf.dirty:
            if self.filename:
                self.buf.Save(self.filename)
            else:
                self.prompt_mode = 'saveas'
                self.prompt_buf = ''
                return
        self.running = False


def Main(stdscr):
    filename = sys.argv[1] if len(sys.argv) > 1 else ''
    Editor(stdscr, filename).Run()


if __name__ == '__main__':
    curses.wrapper(Main)
