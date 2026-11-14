@echo off
del mini.exe 2>nul
g++ -std=c++17 -s -static mini.cpp -o mini.exe
upx mini.exe
