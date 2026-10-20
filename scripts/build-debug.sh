#!/usr/bin/env bash
# Local debug build using MSYS2 mingw64 gcc + gdb (no zig required).
# Output: build/mingw64-debug/clumsy.exe (console subsystem, _DEBUG, -g -O0)
# Usage (from MSYS2 mingw64 shell): SIGN=A scripts/build-debug.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SIGN=${SIGN:-A} # WinDivert driver signing variant: A | B | C
WD="external/WinDivert-2.2.0-$SIGN"
IUP="external/iup-3.30_Win64_mingw6_lib"
OUT="build/mingw64-debug"

mkdir -p "$OUT"

echo "== compiling resources (x64 manifest)"
windres -D X64 etc/clumsy.rc -O coff -o "$OUT/clumsy_res.o"

echo "== compiling + linking clumsy (gcc $(gcc -dumpversion))"
gcc -std=gnu11 -g -O0 -D_DEBUG -mconsole \
    -Wall -Wno-unused-parameter -Wno-format \
    -I"$WD/include" \
    -I"$IUP/include" \
    src/*.c \
    "$OUT/clumsy_res.o" \
    "$IUP/libiup.a" \
    "$WD/x64/WinDivert.dll" \
    -lcomctl32 -lwinmm -lws2_32 -lgdi32 -lcomdlg32 -luuid -lole32 \
    -liphlpapi \
    -o "$OUT/clumsy.exe"

echo "== copying runtime files"
cp "$WD/x64/WinDivert.dll"    "$OUT/"
cp "$WD/x64/WinDivert64.sys"  "$OUT/"
cp etc/config.txt             "$OUT/"

echo "== done: $OUT/clumsy.exe"
