# mini-editor (C++)

一个仅用 ASCII 控制键、双手不用离开键盘即可完成编辑操作的极简终端文本编辑器。

## 编译方法

### 依赖

mini-editor 的 C++17 单文件实现，只依赖 MinGW-w64 的 `g++`，需要支持 C++17。不使用 ncurses / PDCurses，不使用第三方库。

### 编译

```bash
g++ -std=c++17 -s -static mini.cpp -o mini.exe
```

## 运行程序

```bash
mini.exe [FILENAME]
```
