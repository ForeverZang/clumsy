#!/usr/bin/env bash
# Local debug build using MSYS2 mingw64 gcc + gdb (no zig required).
# Output: build/mingw64-debug/clumsy.exe (console subsystem, _DEBUG, -g -O0)
# Usage (from MSYS2 mingw64 shell): SIGN=A scripts/build-debug.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SIGN=${SIGN:-A} # WinDivert driver signing variant: A | B | C
WD="external/WinDivert-2.2.0-$SIGN"
IUP="external/iup-3.30_Win64_mingw6_lib"
CD="external/cd-5.14_Win64_mingw6_lib"
OCR="external/RapidOCRLite"
OUT="build/mingw64-debug"

mkdir -p "$OUT"

echo "== compiling resources (x64 manifest)"
windres -D X64 etc/clumsy.rc -O coff -o "$OUT/clumsy_res.o"

echo "== compiling + linking clumsy (gcc $(gcc -dumpversion))"
# iupcontrols (IupMatrix for the OCR tables) renders through CD; the
# static archives cross-reference each other, so one linker group.
gcc -std=gnu11 -g -O0 -D_DEBUG -DCLUMSY_OCR=1 -mconsole \
    -Wall -Wno-unused-parameter -Wno-format \
    -I"$WD/include" \
    -I"$IUP/include" \
    -I"$OCR/include" \
    src/*.c \
    "$OUT/clumsy_res.o" \
    -Wl,--start-group \
    "$IUP/libiupcontrols.a" "$IUP/libiupcd.a" "$CD/libcd.a" \
    "$IUP/libftgl.a" "$IUP/libfreetype6.a" "$IUP/libz.a" \
    "$IUP/libiup.a" \
    -Wl,--end-group \
    "$WD/x64/WinDivert.dll" \
    "$OCR/bin/x64/rapidocrlite.dll" \
    -lcomctl32 -lwinmm -lws2_32 -lgdi32 -luser32 -lmsimg32 -lgdiplus \
    -lusp10 -lcomdlg32 -lshell32 -luuid -lole32 -liphlpapi \
    -o "$OUT/clumsy.exe"

echo "== copying runtime files"
cp "$WD/x64/WinDivert.dll"    "$OUT/"
cp "$WD/x64/WinDivert64.sys"  "$OUT/"
cp etc/config.txt             "$OUT/"
cp "$OCR/bin/x64/rapidocrlite.dll" "$OUT/"
mkdir -p "$OUT/models"
cp "$OCR/models/PP-OCRv6_det_small.onnx" "$OUT/models/"
cp "$OCR/models/PP-OCRv6_rec_small.onnx" "$OUT/models/"
cp "$OCR/models/ppocrv6_dict.txt"        "$OUT/models/"

echo "== done: $OUT/clumsy.exe"
