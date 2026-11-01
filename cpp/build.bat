@echo off
del mini.exe 2>nul
g++ *.cpp -s -static -o mini.exe
upx mini.exe
mini.exe
