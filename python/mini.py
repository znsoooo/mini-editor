"""mini-editor - 一个仅用 ASCII 控制键、键盘驱动的极简终端文本编辑器

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

"""

import os
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
def DisplayWidth(ch):
    # 单个字符在终端占用的列数: 制表符按 4, 中日韩全角字符按 2
    if ch == '\t':
        return 4
    try:
        return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1
    except Exception:
        return 1


def IsWordChar(ch):
    # 连续英文 + 数字组合判定(仅 ASCII, 对应 [A-Za-z0-9])
    return ch.isascii() and ch.isalnum()


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

    # ---- 行列换算 / 位置偏移 ----
    def LineCount(self):
        return len(self.lines)

    def LineLength(self, row):
        return len(self.lines[row])

    def ClampPosition(self, pos):
        # 把任意 (row, col) 钳制为文档内的有效位置
        row, col = pos
        if row < 0:
            row = 0
        elif row >= len(self.lines):
            row = len(self.lines) - 1
        if col < 0:
            col = 0
        elif col > len(self.lines[row]):
            col = len(self.lines[row])
        return (row, col)

    def ClampCursor(self):
        self.row, self.col = self.ClampPosition((self.row, self.col))

    def Offset(self, pos, offset):
        # 把文档视作一条扁平文本(每行行尾隐含一个换行符),
        # 返回从 pos 前进(正)或后退(负) offset 个字符后的位置; 越界钳制到文档首/尾。
        row, col = self.ClampPosition(pos)
        index = col
        for line_index in range(row):
            index += len(self.lines[line_index]) + 1
        index += offset
        if index <= 0:
            return (0, 0)
        for line_index, line in enumerate(self.lines):
            if index <= len(line):
                return (line_index, index)
            index -= len(line) + 1
        last_row = len(self.lines) - 1
        return (last_row, len(self.lines[last_row]))

    # ---- 光标移动 ----
    def Move(self, delta_row, delta_col, extend):
        origin = (self.row, self.col)
        if delta_col > 0:
            # 右移: 到行尾继续按则移动到下一行行首
            steps = delta_col
            while steps > 0 and self.row < len(self.lines):
                if self.col < len(self.lines[self.row]):
                    self.col += 1
                elif self.row < len(self.lines) - 1:
                    self.row += 1
                    self.col = 0
                else:
                    break
                steps -= 1
        elif delta_col < 0:
            # 左移: 到行首继续按则移动到上一行行尾
            steps = -delta_col
            while steps > 0 and self.row >= 0:
                if self.col > 0:
                    self.col -= 1
                elif self.row > 0:
                    self.row -= 1
                    self.col = len(self.lines[self.row])
                else:
                    break
                steps -= 1
        else:
            # 垂直移动(上/下/翻页): 已在首/末行继续按键 -> 行首/行尾(文件头/尾)
            new_row = self.row + delta_row
            if new_row < 0:
                self.row, self.col = 0, 0
            elif new_row >= len(self.lines):
                self.row = len(self.lines) - 1
                self.col = len(self.lines[self.row])
            else:
                self.row = new_row
        if extend:
            anchor = (self.sel_row, self.sel_col) if self.has_sel else origin
            self.SetSelection(anchor, (self.row, self.col))
        else:
            self.SetCursor((self.row, self.col))

    # ---- 文本替换 ----
    def ExpandTabs(self, text):
        return text.replace('\t', '    ')  # 制表符统一为 4 空格, 避免宽度错位

    def Replace(self, start, end, text):
        # 把 [start, end) 区间替换为 text; 光标停在替换内容末尾并清除选区。
        # start > end 时自动交换, 区间为空且 text 为空时不做任何改动。
        start = self.ClampPosition(start)
        end = self.ClampPosition(end)
        if start > end:
            start, end = end, start
        if start == end and not text:
            return
        text = self.ExpandTabs(text)
        start_row, start_col = start
        end_row, end_col = end
        merged = (self.lines[start_row][:start_col]
                  + text
                  + self.lines[end_row][end_col:])
        self.lines[start_row:end_row + 1] = merged.split('\n')
        inserted = text.split('\n')
        if len(inserted) == 1:
            self.row, self.col = start_row, start_col + len(text)
        else:
            self.row = start_row + len(inserted) - 1
            self.col = len(inserted[-1])
        self.has_sel = False
        self.dirty = True

    def Insert(self, pos, text):
        # 在 pos 处插入文本, 等价于 Replace(pos, pos, text)
        self.Replace(pos, pos, text)

    def Delete(self, start, end):
        # 删除 [start, end) 区间, 等价于 Replace(start, end, '')
        self.Replace(start, end, '')

    # ---- 选区 ----
    def GetSelection(self):
        # 返回有序的 (起点, 终点); 无选区时退化为 (光标, 光标)
        anchor = (self.sel_row, self.sel_col)
        cursor = (self.row, self.col)
        if anchor > cursor:
            return (cursor, anchor)
        return (anchor, cursor)

    def GetSelectionText(self):
        if not self.has_sel:
            return ''
        start, end = self.GetSelection()
        if start[0] == end[0]:
            return self.lines[start[0]][start[1]:end[1]]
        parts = [self.lines[start[0]][start[1]:]]
        for row in range(start[0] + 1, end[0]):
            parts.append(self.lines[row])
        parts.append(self.lines[end[0]][:end[1]])
        return '\n'.join(parts)

    def SetSelection(self, start, end):
        # start 为锚点, end 为光标位置; end 可以位于 start 之前(反向选区)。
        # 首尾重合视为没有选区, 因此 SetCursor(pos) 等价于 SetSelection(pos, pos)。
        self.sel_row, self.sel_col = self.ClampPosition(start)
        self.row, self.col = self.ClampPosition(end)
        self.has_sel = ((self.sel_row, self.sel_col) != (self.row, self.col))

    def SetCursor(self, pos):
        self.SetSelection(pos, pos)

    def IsInSelection(self, row, col):
        # 选区是半开区间 [start, end): 终点所在位置是光标(插入点)本身,
        # 不属于选中的字符, 因此终点位置的字符不会被高亮。
        if not self.has_sel:
            return False
        start, end = self.GetSelection()
        return start <= (row, col) < end

    def IsNewlineSelected(self, row):
        # 判断第 row 行行尾的换行符是否落在选区内。
        # 换行符位于该行最后一个字符之后, 即位置 (row, 行长度);
        # 与该位置本身重合的终点不算选中, 只有终点越过它才算。
        if not self.has_sel:
            return False
        start, end = self.GetSelection()
        return start <= (row, len(self.lines[row])) < end

    def SelectTo(self, pos, extend=True):
        # 把光标移到 pos; extend=True 时从原锚点(无选区时取移动前光标)扩展选区
        if not extend:
            self.SetCursor(pos)
            return
        anchor = (self.sel_row, self.sel_col) if self.has_sel else (self.row, self.col)
        self.SetSelection(anchor, pos)

    def MoveToLineStart(self, extend=True):
        self.SelectTo((self.row, 0), extend)

    def MoveToLineEnd(self, extend=True):
        self.SelectTo((self.row, len(self.lines[self.row])), extend)

    def SelectLine(self, row):
        self.SetSelection((row, 0), (row, len(self.lines[row])))

    def SelectAll(self):
        self.SetSelection((0, 0), (len(self.lines) - 1, len(self.lines[-1])))


# ---------------------------------------------------------------------------
# 命令实现(撤销重做/剪贴板/行操作/大小写/缩进/跳转/查找; 由 Editor 分派)
# ---------------------------------------------------------------------------
class Commands:
    def __init__(self, editor):
        self.editor = editor

    # ---- 撤销/重做(快照) ----
    def TakeSnapshot(self):
        buffer = self.editor.buffer
        return {
            'lines': copy.deepcopy(buffer.lines),
            'row': buffer.row, 'col': buffer.col,
            'sel_row': buffer.sel_row, 'sel_col': buffer.sel_col,
            'has_sel': buffer.has_sel,
        }

    def PushUndo(self):
        self.editor.undo_stack.append(self.TakeSnapshot())
        self.editor.redo_stack = []

    def RestoreSnapshot(self, snapshot):
        buffer = self.editor.buffer
        buffer.lines = copy.deepcopy(snapshot['lines'])
        buffer.row, buffer.col = snapshot['row'], snapshot['col']
        buffer.sel_row, buffer.sel_col = snapshot['sel_row'], snapshot['sel_col']
        buffer.has_sel = snapshot['has_sel']
        buffer.ClampCursor()

    def Undo(self):
        if not self.editor.undo_stack:
            return
        self.editor.redo_stack.append(self.TakeSnapshot())
        self.RestoreSnapshot(self.editor.undo_stack.pop())
        self.editor.undo_group = None
        self.RefreshDirty()

    def Redo(self):
        if not self.editor.redo_stack:
            return
        self.editor.undo_stack.append(self.TakeSnapshot())
        self.RestoreSnapshot(self.editor.redo_stack.pop())
        self.editor.undo_group = None
        self.RefreshDirty()

    def RefreshDirty(self):
        # 脏标记 = 当前内容是否偏离"最近一次保存的状态"(以保存快照为准)。
        # 撤销/重做恰好回到保存点(或打开时的磁盘内容)即视为干净。
        editor = self.editor
        if editor.saved is None:
            editor.buffer.dirty = True  # 尚未保存过的新文档始终脏
        else:
            editor.buffer.dirty = (editor.buffer.lines != editor.saved)

    def BeginUndoGroup(self, kind, row, col):
        # 连续输入(insert)/连续退格(back)/连续删除(del) 合并为一次撤销。
        # 判断当前操作的"起始光标位置"是否紧接上一次同类操作之后。
        editor = self.editor
        if kind == 'insert':
            next_position = (row, col + 1)  # 输入后光标右移一格
        elif kind == 'back':
            next_position = (row, col - 1)  # 退格后光标左移一格
        else:  # del
            next_position = (row, col)      # Delete 键光标不动
        if (editor.undo_group and editor.undo_group['kind'] == kind
                and editor.undo_group['next'] == (row, col)):
            editor.undo_group['next'] = next_position  # 续接: 不压新快照
            return
        self.PushUndo()
        editor.undo_group = {'kind': kind, 'next': next_position}

    def Edit(self, action):
        # 执行一次编辑动作, 并在"这次编辑什么都没留下"时回滚撤销记录。
        # 判据是文本与光标/选区都未变化: 例如空文档 Shift+D、文档开头退格、
        # 已是目标大小写的文本再转一次、无缩进可减, 都回滚本次压入的快照,
        # 并还原重做栈/连续编辑组/脏标记。
        # 反过来, 文本没变但光标或选区变了(例如原地粘贴导致选区被清除)仍算一次
        # 有效编辑: 撤销需要把选区还原回去。
        editor = self.editor
        buffer = editor.buffer
        before_lines = list(buffer.lines)  # 行字符串不可变, 浅拷贝即可比较内容
        before_state = (buffer.row, buffer.col, buffer.sel_row, buffer.sel_col,
                        buffer.has_sel, buffer.dirty)
        undo_depth = len(editor.undo_stack)
        redo_before = editor.redo_stack
        group_before = dict(editor.undo_group) if editor.undo_group else None
        action()
        if buffer.lines != before_lines:
            return True
        if (buffer.row, buffer.col, buffer.sel_row, buffer.sel_col,
                buffer.has_sel) != before_state[:5]:
            return True
        del editor.undo_stack[undo_depth:]
        editor.redo_stack = redo_before
        editor.undo_group = group_before
        (buffer.row, buffer.col, buffer.sel_row, buffer.sel_col,
         buffer.has_sel, buffer.dirty) = before_state
        buffer.ClampCursor()
        return False

    # ---- 编辑命令(调用 Buffer 原语 + 撤销合并) ----
    def TypeCharacter(self, ch):
        # 单字符输入: 连续输入合并为一次撤销
        buffer = self.editor.buffer

        def action():
            self.BeginUndoGroup('insert', buffer.row, buffer.col)
            buffer.Insert((buffer.row, buffer.col), ch)

        self.Edit(action)

    def InsertString(self, text):
        # 粘贴等多字符插入: 不并入连续输入组, 单独压一次快照
        buffer = self.editor.buffer

        def action():
            self.PushUndo()
            self.editor.undo_group = None
            buffer.Insert((buffer.row, buffer.col), text)

        self.Edit(action)

    def SplitLine(self):
        # Enter: 在光标处把一行拆成两行
        buffer = self.editor.buffer

        def action():
            self.PushUndo()
            self.editor.undo_group = None
            buffer.Insert((buffer.row, buffer.col), '\n')

        self.Edit(action)

    def DeleteCharacter(self, backspace):
        # 两种模式共用: backspace 删光标前, 否则删光标后; 有选区则整段删除
        def action():
            buffer = self.editor.buffer
            cursor = (buffer.row, buffer.col)
            if buffer.has_sel:
                self.PushUndo()
                self.editor.undo_group = None
                buffer.Delete(*buffer.GetSelection())
            elif backspace:
                self.BeginUndoGroup('back', buffer.row, buffer.col)
                buffer.Delete(buffer.Offset(cursor, -1), cursor)
            else:
                self.BeginUndoGroup('del', buffer.row, buffer.col)
                buffer.Delete(cursor, buffer.Offset(cursor, 1))

        self.Edit(action)

    # ---- 行级操作 ----
    def DuplicateCurrentLine(self):
        # 复制当前行并插到其后, 光标落在新行行首
        buffer = self.editor.buffer
        row = buffer.row
        buffer.Insert((row, buffer.LineLength(row)), '\n' + buffer.lines[row])
        buffer.SetCursor((row + 1, 0))

    def DeleteCurrentLine(self):
        # 删除当前行; 删末行时连同其前面的换行一起删除, 保证行数正确减少
        buffer = self.editor.buffer
        row = buffer.row
        if row != buffer.LineCount() - 1:
            buffer.Delete((row, 0), (row + 1, 0))
        elif row == 0:
            buffer.Delete((0, 0), (0, buffer.LineLength(0)))
        else:
            buffer.Delete((row - 1, buffer.LineLength(row - 1)),
                          (row, buffer.LineLength(row)))
        buffer.SetCursor((min(row, buffer.LineCount() - 1), 0))

    # ---- 选区 / 剪贴板命令 ----
    def SelectAll(self):
        self.editor.buffer.SelectAll()

    def Copy(self):
        buffer = self.editor.buffer
        if not buffer.has_sel:
            # 无选区: 复制当前行(含行尾换行符), 光标回到行首, 不选中整行
            row = buffer.row
            self.editor.clipboard = buffer.lines[row] + '\n'
            buffer.SetCursor((row, 0))
            self.editor.status = 'Copied line'
            return
        self.editor.clipboard = buffer.GetSelectionText()  # 选区保留
        self.editor.status = 'Copied'

    def Cut(self):
        buffer = self.editor.buffer
        if not buffer.has_sel:
            # 无选区: 剪切当前行(含行尾换行符)
            def action():
                self.editor.clipboard = buffer.lines[buffer.row] + '\n'
                self.PushUndo()
                self.editor.undo_group = None
                self.DeleteCurrentLine()
                self.editor.status = 'Cut'

            self.Edit(action)
            return
        def action():
            self.editor.clipboard = buffer.GetSelectionText()
            self.PushUndo()
            self.editor.undo_group = None
            buffer.Delete(*buffer.GetSelection())
            self.editor.status = 'Cut'

        self.Edit(action)

    def Paste(self):
        if not self.editor.clipboard:
            self.editor.status = 'Clipboard empty'
            return
        buffer = self.editor.buffer

        def action():
            self.PushUndo()
            self.editor.undo_group = None
            if buffer.has_sel:
                buffer.Delete(*buffer.GetSelection())  # 有选区: 用剪贴板内容替换选区
            buffer.Insert((buffer.row, buffer.col), self.editor.clipboard)
            self.editor.status = 'Pasted'

        self.Edit(action)

    def Duplicate(self):
        # 重复当前行或选中内容; 重复后光标与选区范围保持不变
        def action():
            buffer = self.editor.buffer
            self.PushUndo()
            self.editor.undo_group = None
            cursor_state = (buffer.row, buffer.col,
                            buffer.sel_row, buffer.sel_col, buffer.has_sel)
            if buffer.has_sel:
                selection_text = buffer.GetSelectionText()
                selection_end = buffer.GetSelection()[1]
                buffer.Insert(selection_end, selection_text)  # 副本插在选区之后
            else:
                self.DuplicateCurrentLine()
            (buffer.row, buffer.col,
             buffer.sel_row, buffer.sel_col, buffer.has_sel) = cursor_state
            buffer.ClampCursor()
            self.editor.status = 'Duplicated'

        self.Edit(action)

    def DeleteLine(self):
        # Shift+D: 有选区删选中区域, 否则删当前行
        def action():
            buffer = self.editor.buffer
            self.PushUndo()
            self.editor.undo_group = None
            if buffer.has_sel:
                buffer.Delete(*buffer.GetSelection())
            else:
                self.DeleteCurrentLine()
            self.editor.status = 'Deleted'

        self.Edit(action)

    # ---- 大小写 / 缩进 ----
    def ChangeCase(self, upper):
        # U / Shift+U: 有选区转换选中部分, 无选区转换光标所在单词。
        # 是否留下撤销记录由 Edit 统一判断(文本无变化则自动回滚)。
        buffer = self.editor.buffer
        if not buffer.has_sel and self.CurrentWordRange() is None:
            return

        def action():
            self.PushUndo()
            self.editor.undo_group = None
            if buffer.has_sel:
                self.ApplyCase(upper)
            else:
                self.ApplyWordCase(upper)

        self.Edit(action)

    def ApplyCase(self, upper):
        # 转换选中区域; 无选区时转换整行
        buffer = self.editor.buffer
        if buffer.has_sel:
            start, end = buffer.GetSelection()
            if start[0] == end[0]:
                line = buffer.lines[start[0]]
                segment = line[start[1]:end[1]]
                buffer.lines[start[0]] = (line[:start[1]]
                                          + (segment.upper() if upper else segment.lower())
                                          + line[end[1]:])
            else:
                line = buffer.lines[start[0]]
                tail = line[start[1]:]
                buffer.lines[start[0]] = line[:start[1]] + (tail.upper() if upper else tail.lower())
                for row in range(start[0] + 1, end[0]):
                    buffer.lines[row] = (buffer.lines[row].upper() if upper
                                         else buffer.lines[row].lower())
                line = buffer.lines[end[0]]
                head = line[:end[1]]
                buffer.lines[end[0]] = (head.upper() if upper else head.lower()) + line[end[1]:]
        else:
            row = buffer.row
            buffer.lines[row] = (buffer.lines[row].upper() if upper
                                 else buffer.lines[row].lower())
        buffer.dirty = True

    def CurrentWordRange(self):
        # 返回光标所在单词的 [start, end) 区间; 光标不在任何单词上则返回 None
        buffer = self.editor.buffer
        line = buffer.lines[buffer.row]
        length = len(line)
        col = buffer.col
        if col < length and IsWordChar(line[col]):
            start = col
            while start > 0 and IsWordChar(line[start - 1]):
                start -= 1
            end = col
            while end < length and IsWordChar(line[end]):
                end += 1
        elif col > 0 and IsWordChar(line[col - 1]):
            # 光标落在非单词字符上: 取紧邻左侧的单词
            end = col
            start = col
            while start > 0 and IsWordChar(line[start - 1]):
                start -= 1
        else:
            return None
        return (start, end)

    def ApplyWordCase(self, upper):
        # 仅转换光标所在单词(连续英文+数字); 返回是否发生修改
        word_range = self.CurrentWordRange()
        if word_range is None:
            return False
        start, end = word_range
        buffer = self.editor.buffer
        line = buffer.lines[buffer.row]
        word = line[start:end]
        buffer.lines[buffer.row] = (line[:start]
                                    + (word.upper() if upper else word.lower())
                                    + line[end:])
        buffer.dirty = True
        return True

    def ChangeIndent(self, dedent):
        # T / Shift+T: 缩进或减少缩进; 文本无变化的减少缩进不记撤销(由 Edit 判定)
        def action():
            self.PushUndo()
            self.editor.undo_group = None
            self.ApplyIndent(dedent)

        self.Edit(action)

    def ApplyIndent(self, dedent):
        # 处理当前行(无选区)或选区涵盖的所有行; 每档缩进为 4 空格。
        # 保留调用前的选区不变: 不对行重新整体选中, 只把光标/锚点按各行的
        # 实际缩进变化量整体平移(缩进 +4, 减少缩进 -4 或 -1)。
        buffer = self.editor.buffer
        if buffer.has_sel:
            first_row = min(buffer.sel_row, buffer.row)
            last_row = max(buffer.sel_row, buffer.row)
        else:
            first_row = last_row = buffer.row
        cursor_row, cursor_col = buffer.row, buffer.col
        anchor_row, anchor_col = buffer.sel_row, buffer.sel_col
        has_sel = buffer.has_sel
        deltas = {}
        for row in range(first_row, last_row + 1):
            if dedent:
                line = buffer.lines[row]
                if line.startswith('    '):
                    buffer.Delete((row, 0), (row, 4))
                    deltas[row] = -4
                elif line.startswith('\t'):
                    buffer.Delete((row, 0), (row, 1))
                    deltas[row] = -1
                else:
                    deltas[row] = 0
                    # 行首无可移除的缩进档时保持不变
            else:
                buffer.Insert((row, 0), '    ')
                deltas[row] = 4
        # 按各行缩进变化量平移光标与选区锚点(行号不变, 只移动列)
        buffer.row = cursor_row
        buffer.col = max(0, min(cursor_col + deltas.get(cursor_row, 0),
                                buffer.LineLength(cursor_row)))
        if has_sel:
            buffer.sel_row = anchor_row
            buffer.sel_col = max(0, min(anchor_col + deltas.get(anchor_row, 0),
                                        buffer.LineLength(anchor_row)))
            buffer.has_sel = True
        else:
            buffer.has_sel = False

    # ---- 跳转 / 查找 ----
    def ParseGoto(self, text):
        """解析 G/Goto 输入的行列。语法:
            R          跳到第 R 行行首(列 1)
            R:C / R.C  跳到第 R 行第 C 列
            :C / .C    R 空缺, 在当前行跳到第 C 列
        R、C 均可省略 / 为负; R 的 0 视为 1; 越界均被安全钳制。
        未指定列时: 仅 R 为正数越界(超过总行数)停在末行行尾, 其余(含负数 R)停在行首。
        列的正数越界(如第 len+1 位及以后)停在行尾; 负数按精确位置从行尾数:
        -1 = 行尾(最后一个字符之后), -2 = 行尾前一格, -3 再前一格。
        返回 (row, col) 或 None(无法解析)。
        注: 行号与列号均为 1-based(列 1 = 第 1 个字符); 内部统一换算为缓冲区 0-based 列索引。
        """
        buffer = self.editor.buffer
        text = text.strip()
        if not text:
            return None
        if ':' in text:
            row_part, col_part = text.split(':', 1)
        elif '.' in text:
            row_part, col_part = text.split('.', 1)
        else:
            row_part, col_part = text, None
        line_count = buffer.LineCount()
        # 行
        if row_part == '':
            row = buffer.row
            to_end = False
        else:
            try:
                value = int(row_part)
            except ValueError:
                return None
            if value == 0:
                value = 1  # 0 视为第 1 行
            elif value < 0:
                value = line_count + value + 1  # 负数从末尾: -1=末行
            # 仅正数越界(如 9999)时, 未指定列才定位到末行行尾;
            # 负数只是按倒数行号取行, 未指定列时停在行首。
            to_end = value > line_count
            if value < 1:
                value = 1
            if value > line_count:
                value = line_count
            row = value - 1
        # 列: 1-based; 负数按“从行尾边界数”的精确位置处理
        if col_part is None:
            col = len(buffer.lines[row]) if to_end else 0
        else:
            try:
                value = int(col_part)
            except ValueError:
                return None
            length = len(buffer.lines[row])
            if value > 0:
                # 1-based -> 0-based; 超过行长度(第 len+1 位及以后)一律落在行尾
                col = min(value - 1, length)
            elif value == 0:
                col = 0            # 0 视为首列, 与行号一致
            else:
                # 负数从行尾边界数: -1=行尾(len), -2=行尾前一格(len-1)...
                col = max(0, length + value + 1)
        return (row, col)

    def Search(self, query, reverse=False, start_row=None, start_col=None):
        # 查找实现: 返回 (row, col) 或 None
        buffer = self.editor.buffer
        if not query:
            return None
        line_count = len(buffer.lines)
        if start_row is None:
            start_row, start_col = buffer.row, buffer.col
        if not reverse:
            for row in range(start_row, line_count):
                col = buffer.lines[row].find(query, start_col if row == start_row else 0)
                if col >= 0:
                    return (row, col)
            return None
        for row in range(start_row, -1, -1):
            end = start_col if row == start_row else len(buffer.lines[row])
            col = buffer.lines[row].rfind(query, 0, end)
            if col >= 0:
                return (row, col)
        return None

    def FindNext(self, query, reverse):
        # F / Shift+F: 查找并把光标移到命中位置
        buffer = self.editor.buffer
        if not query:
            return
        if reverse:
            target = self.Search(query, reverse=True,
                                 start_row=buffer.row, start_col=buffer.col)
        else:
            target = self.Search(query, reverse=False,
                                 start_row=buffer.row, start_col=buffer.col + 1)
        if target is None:
            self.editor.status = 'No match: ' + query
            return
        buffer.SetCursor(target)
        self.editor.status = 'Find: ' + query

    # ---- 文件命令 ----
    def Save(self):
        editor = self.editor
        if not editor.filename:
            editor.prompt_mode = 'saveas'
            editor.prompt_text = ''
            return
        editor.SaveFile(editor.filename)
        editor.saved = copy.deepcopy(editor.buffer.lines)  # 更新干净基准
        self.RefreshDirty()
        editor.status = 'Saved ' + editor.filename

    def Close(self):
        editor = self.editor
        if editor.buffer.dirty:
            if editor.filename:
                editor.SaveFile(editor.filename)
            else:
                editor.prompt_mode = 'saveas'
                editor.prompt_text = ''
                return
        editor.running = False


# ---------------------------------------------------------------------------
# 编辑器(终端/光标/渲染/输入)
# ---------------------------------------------------------------------------
class Editor:
    def __init__(self, stdscr, filename):
        self.stdscr = stdscr
        self.buffer = Buffer()
        self.LoadFile(filename)
        self.filename = filename or ''
        self.mode = 1            # 0=编辑 1=命令(默认进入命令模式)
        self.top = 0             # 顶部可见行
        self.left = 0            # 水平滚动
        self.clipboard = ''
        self.status = ''
        self.prompt_mode = None  # 'find' 'findrev' 'goto' 'saveas'
        self.prompt_text = ''
        self.goto_select = False  # Shift+G: 跳转同时产生/改变选区
        self.undo_stack = []
        self.redo_stack = []
        self.undo_group = None   # 连续输入/删除合并组: {'kind','next'}
        self.running = True
        # 干净基准: 已存在文件 -> 以磁盘内容为准(初始 clean);
        #          新建/无文件名 -> 尚未保存(始终 dirty, 直到首次保存)
        if filename and os.path.exists(filename):
            self.saved = copy.deepcopy(self.buffer.lines)
        else:
            self.saved = None
        self.commands = Commands(self)
        self.commands.RefreshDirty()

    # ---- 文件读写交互 ----
    def LoadFile(self, path):
        # 读取文件填充 Buffer; 文件不存在/不可读时保持空文档
        buffer = self.buffer
        buffer.lines = ['']
        buffer.row = buffer.col = 0
        buffer.has_sel = False
        buffer.dirty = False
        if not path:
            return
        try:
            with open(path, 'r', encoding='utf-8') as file:
                text = file.read()
        except (FileNotFoundError, IsADirectoryError, PermissionError):
            return
        text = text.replace('\r\n', '\n').replace('\r', '\n')
        buffer.lines = text.split('\n')
        if not buffer.lines:
            buffer.lines = ['']
        buffer.ClampCursor()

    def SaveFile(self, path):
        # 把 Buffer 内容写入文件
        with open(path, 'w', encoding='utf-8') as file:
            file.write('\n'.join(self.buffer.lines))
        self.buffer.dirty = False

    # ---- 尺寸 ----
    def Height(self):
        return self.stdscr.getmaxyx()[0]

    def Width(self):
        return self.stdscr.getmaxyx()[1]

    def TextRows(self):
        return max(1, self.Height() - 2)

    # ---- 渲染 ----
    def AddText(self, screen_row, column, text, attribute=0):
        try:
            self.stdscr.addstr(screen_row, column, text, attribute)
        except curses.error:
            pass

    def AddTextRun(self, text, selected):
        try:
            self.stdscr.addstr(text, curses.A_REVERSE if selected else curses.A_NORMAL)
        except curses.error:
            pass

    def DrawLine(self, screen_row, line_index):
        width = self.Width()
        line = self.buffer.lines[line_index]
        segment = line[self.left:self.left + width]
        if not self.buffer.has_sel:
            self.AddText(screen_row, 0, segment)
            return
        # 有选区: 按运行(选中/未选中)分段绘制, curses 负责多宽度对齐
        self.stdscr.move(screen_row, 0)
        column = self.left
        run_text = ''
        run_selected = self.buffer.IsInSelection(line_index, column)
        for ch in segment:
            selected = self.buffer.IsInSelection(line_index, column)
            if selected != run_selected:
                self.AddTextRun(run_text, run_selected)
                run_text = ''
                run_selected = selected
            run_text += ch
            column += 1
        if run_text:
            self.AddTextRun(run_text, run_selected)
        # 选区包含行尾换行符时, 用一个反色的 ASCII 宽度空白表示该换行符
        if self.buffer.IsNewlineSelected(line_index):
            marker_x = 0
            for ch in segment:
                marker_x += DisplayWidth(ch)
            if marker_x < width:
                self.AddText(screen_row, marker_x, ' ', curses.A_REVERSE)

    def DrawStatus(self, height, width):
        screen_row = height - 1
        self.stdscr.move(screen_row, 0)
        self.stdscr.clrtoeol()
        mode_text = 'EDIT' if self.mode == 0 else 'COMMAND'
        filename = self.filename if self.filename else '[untitled]'
        if self.prompt_mode == 'find':
            message = 'Find: ' + self.prompt_text
        elif self.prompt_mode == 'findrev':
            message = 'Find(rev): ' + self.prompt_text
        elif self.prompt_mode == 'goto':
            message = 'Goto line: ' + self.prompt_text
        elif self.prompt_mode == 'saveas':
            message = 'Save as: ' + self.prompt_text
        elif self.status:
            message = self.status
        else:
            message = 'L%d C%d' % (self.buffer.row + 1, self.buffer.col + 1)
        dirty_mark = ' (*)' if self.buffer.dirty else ''
        base = '%s | %s%s' % (mode_text, filename, dirty_mark)
        self.AddText(screen_row, 0, (base + '  ' + message)[:width - 1])

    def Render(self):
        self.commands.RefreshDirty()  # 每帧以保存快照重算脏标记, 保证撤销/重做后即时准确
        height, width = self.Height(), self.Width()
        text_rows = self.TextRows()
        # 滚动: 保证光标可见
        if self.buffer.row < self.top:
            self.top = self.buffer.row
        while self.buffer.row >= self.top + text_rows:
            self.top += 1
        if self.buffer.col < self.left:
            self.left = self.buffer.col
        while self.buffer.col >= self.left + width:
            self.left += 1

        self.stdscr.erase()
        for screen_row in range(text_rows):
            line_index = self.top + screen_row
            if line_index >= self.buffer.LineCount():
                break
            self.DrawLine(screen_row, line_index)
        # 分隔符
        try:
            self.stdscr.move(height - 2, 0)
            self.stdscr.hline('-', width)
        except curses.error:
            pass
        self.DrawStatus(height, width)
        # 硬件光标定位(可见方块光标 curs_set(1), 跨平台稳定; 多宽度用显示宽度对齐)
        cursor_y = self.buffer.row - self.top
        cursor_x = 0
        for ch in self.buffer.lines[self.buffer.row][self.left:self.buffer.col]:
            cursor_x += DisplayWidth(ch)
        if cursor_y < 0:
            cursor_y = 0
        if cursor_y > height - 3:
            cursor_y = height - 3
        if cursor_x > width - 1:
            cursor_x = width - 1
        try:
            self.stdscr.move(cursor_y, cursor_x)
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
        previous_sigint = signal.signal(signal.SIGINT, lambda *args: None)
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
            signal.signal(signal.SIGINT, previous_sigint)
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
            self.undo_group = None
            self.status = ''
            return
        if ch == curses.KEY_BTAB:  # Shift+Tab: 减少缩进(两种模式通用)
            self.commands.ChangeIndent(dedent=True)
            return
        if self.mode == 0:
            self.HandleEdit(ch)
        else:
            self.HandleCommand(ch)

    def HandleEdit(self, ch):
        # 编辑模式: 把原始按键路由到 Commands 的编辑命令(内含撤销合并)
        if ch == '\n' or ch == '\r':
            self.commands.SplitLine()
            return
        if ch == '\t':  # Tab 等同 T: 缩进当前行/选中行
            self.commands.ChangeIndent(dedent=False)
            return
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            self.commands.DeleteCharacter(backspace=True)
            return
        if ch == curses.KEY_DC:
            self.commands.DeleteCharacter(backspace=False)
            return
        if isinstance(ch, int):  # 方向键/Home/End/PageUp/Down
            self.HandleMoveKey(ch, shift=False)
            return
        # 普通字符(含中文等多字节)
        self.commands.TypeCharacter(ch)

    def HandleCommand(self, ch):
        # 命令模式同样响应 Backspace / Delete / Enter / Tab(与编辑模式一致)
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            self.commands.DeleteCharacter(backspace=True)
            return
        if ch == curses.KEY_DC:
            self.commands.DeleteCharacter(backspace=False)
            return
        if isinstance(ch, int):
            self.HandleMoveKey(ch, shift=False)
            return
        if ch == '\n' or ch == '\r':
            self.commands.SplitLine()
            return
        if ch == '\t':
            self.commands.ChangeIndent(dedent=False)  # Tab 等同 T
            return
        up = ch.isupper()
        key = ch.lower()
        if key == ' ':
            self.commands.TypeCharacter(' ')  # 空格: 两模式通用, 插入空格
        elif key == 'i':
            self.buffer.Move(-1, 0, up)
        elif key == 'k':
            self.buffer.Move(1, 0, up)
        elif key == 'j':
            self.buffer.Move(0, -1, up)
        elif key == 'l':
            self.buffer.Move(0, 1, up)
        elif key == '[':          # 上翻页
            self.buffer.Move(-self.TextRows(), 0, False)
        elif key == ']':          # 下翻页
            self.buffer.Move(self.TextRows(), 0, False)
        elif key == '{':          # 上翻页并选中
            self.buffer.Move(-self.TextRows(), 0, True)
        elif key == '}':          # 下翻页并选中
            self.buffer.Move(self.TextRows(), 0, True)
        elif key == 'a':
            self.commands.SelectAll()
        elif key == 'x':
            self.commands.Cut()
        elif key == 'c':
            self.commands.Copy()
        elif key == 'v':
            self.commands.Paste()
        elif key == 'z':
            if up:
                self.commands.Redo()
            else:
                self.commands.Undo()
        elif key == 'f':
            self.prompt_mode = 'findrev' if up else 'find'
            self.prompt_text = ''
            self.undo_group = None
        elif key == 'g':
            self.prompt_mode = 'goto'
            self.goto_select = up  # Shift+G: 从(选区)起点选择到跳转位置
            self.prompt_text = ''
            self.undo_group = None
        elif key == 'u':
            self.commands.ChangeCase(up)
        elif key == 't':
            self.commands.ChangeIndent(dedent=up)  # T 缩进 / Shift+T 减少缩进
        elif key == 'b':
            self.commands.DeleteCharacter(backspace=not up)  # B=Backspace, Shift+B=Delete
        elif key == 'd':
            if up:
                self.commands.DeleteLine()
            else:
                self.commands.Duplicate()
        elif key == 's':
            if up:
                self.prompt_mode = 'saveas'
                self.prompt_text = ''
                self.undo_group = None
            else:
                self.commands.Save()
        elif key == 'q':
            if up:               # Shift+Q: 不保存退出
                self.running = False
            else:                # Q: 提示保存后退出
                self.commands.Close()
        elif key == 'w':
            self.commands.Close()

    def HandleMoveKey(self, key, shift):
        if key in (curses.KEY_UP, curses.KEY_SR):
            self.buffer.Move(-1, 0, shift)
        elif key in (curses.KEY_DOWN, curses.KEY_SF):
            self.buffer.Move(1, 0, shift)
        elif key in (curses.KEY_LEFT, curses.KEY_SLEFT):
            self.buffer.Move(0, -1, shift)
        elif key in (curses.KEY_RIGHT, curses.KEY_SRIGHT):
            self.buffer.Move(0, 1, shift)
        elif key == curses.KEY_HOME:
            self.buffer.MoveToLineStart(shift)
        elif key == curses.KEY_END:
            self.buffer.MoveToLineEnd(shift)
        elif key == curses.KEY_PPAGE:
            self.buffer.Move(-self.TextRows(), 0, shift)
        elif key == curses.KEY_NPAGE:
            self.buffer.Move(self.TextRows(), 0, shift)

    # ---- 提示子模式(F / Shift+F / G / Shift+S) ----
    def HandlePrompt(self, ch):
        if ch == KEY_ESC or ch == ESC_STR:  # Esc 取消
            self.prompt_mode = None
            self.prompt_text = ''
            self.goto_select = False
            return
        if ch == '\n' or ch == '\r':
            self.ConfirmPrompt()
            return
        if ch in (curses.KEY_BACKSPACE, 127, 8, '\x08'):
            if self.prompt_text:
                self.prompt_text = self.prompt_text[:-1]  # 按码点删除(支持中文)
            return
        if isinstance(ch, int):
            return  # 提示态忽略方向键
        self.prompt_text += ch

    def ConfirmPrompt(self):
        mode = self.prompt_mode
        text = self.prompt_text
        self.prompt_mode = None
        self.prompt_text = ''
        if mode == 'find':
            self.commands.FindNext(text, False)
        elif mode == 'findrev':
            self.commands.FindNext(text, True)
        elif mode == 'goto':
            select = self.goto_select
            self.goto_select = False
            target = self.commands.ParseGoto(text)
            if target is None:
                self.status = 'Invalid line'
                return
            if select:
                # 从(选区)起点选择到跳转目标位置
                self.buffer.SelectTo(target, extend=True)
            else:
                self.buffer.SetCursor(target)
        elif mode == 'saveas':
            if text:
                self.filename = text
                self.commands.Save()


def Main(stdscr):
    filename = sys.argv[1] if len(sys.argv) > 1 else ''
    Editor(stdscr, filename).Run()


if __name__ == '__main__':
    curses.wrapper(Main)
