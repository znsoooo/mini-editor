# mini-editor (Python)

一个仅用 ASCII 控制键、双手不用离开键盘即可完成编辑操作的极简终端文本编辑器。

## 设计思路

### 设计参考（仅借鉴设计，非二次开发）
- **kotte**：单文件 `curses` 编辑器（约 500 行），正确处理多字节 / 多宽度字符。本项目重点借鉴其 **文本加载、光标定位、翻页、插入、删除、复制** 的方式，尤其是多宽度字符（CJK）的显示处理。
- **tedit**：纯 Python 模态编辑器，将 `buffer / cursor / renderer / input` 职责分离。本项目据此把 **缓冲区逻辑** 与 **终端渲染** 拆成 `Buffer` 与 `Editor` 两个类。
- **kilo**（C） / **txted**：极简行数组 + 滚动偏移 + 渲染模型、双模式 + 撤销重做，佐证整体架构选择。

### 模块结构
- `Buffer`：纯逻辑，不依赖 `curses`。负责行模型、光标/选区、插入/删除/合并、查找、大小写转换、文件读写。
- `Editor`：持有 `Buffer` 与全局状态（模式、滚动偏移、剪贴板、撤销栈、提示输入），负责 `curses` 原始模式、按键解析、渲染（含 ASCII 减号分隔符与状态行）、命令分发。

## 使用说明

### 安装依赖

本程序基于 Python 标准库的 `curses` 模块做终端渲染与键盘输入。

其中 Windows 系统需要安装 `windows-curses` 包，而 Linux 系统需要安装 `ncurses` 环境后即可使用：

```bash
# Windows
pip install windows-curses

# Linux
apt install libncursesw5
```

### 运行程序

```bash
python mini.py [FILENAME]
```

