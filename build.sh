#!/usr/bin/env bash
# 交叉编译 texel.exe (Windows 原生, 静态链接, 无运行时依赖)
set -e
cd "$(dirname "$0")"

CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}

cd src
$WINDRES texel.rc -O coff --codepage=65001 -o texel.res
$CXX -std=c++17 -O2 -municode -mwindows \
    -DUNICODE -D_UNICODE \
    -ffunction-sections -fdata-sections \
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE \
    texel.cpp texel.res \
    -o ../texel.exe \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections -s \
    -lgdiplus -lgdi32 -luser32 -lkernel32 -lole32 -lcomdlg32 -limm32 -lshell32

cd ..
ls -lh texel.exe
