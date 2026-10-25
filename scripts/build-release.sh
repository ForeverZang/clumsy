#!/usr/bin/env bash
# Local release build using MSYS2 mingw64 gcc (no zig required).
# Mirrors build.zig Release: NDEBUG resource, -O2, Windows GUI subsystem
# (no console window; LOG() compiles out under NDEBUG).
# Output: build/mingw64-release/clumsy.exe + WinDivert runtime files.
# Usage (from MSYS2 mingw64 shell): SIGN=A scripts/build-release.sh
set -euo pipefail
cd "$(dirname "$0")/.."

SIGN=${SIGN:-A} # WinDivert driver signing variant: A | B | C
WD="external/WinDivert-2.2.0-$SIGN"
IUP="external/iup-3.30_Win64_mingw6_lib"
OCR="external/RapidOCRLite"
OUT="build/mingw64-release"

mkdir -p "$OUT"

echo "== compiling resources (x64 manifest, NDEBUG)"
windres -D NDEBUG -D X64 etc/clumsy.rc -O coff -o "$OUT/clumsy_res.o"

echo "== compiling + linking clumsy (gcc $(gcc -dumpversion), -O2 -mwindows)"
gcc -std=gnu11 -O2 -DNDEBUG -DCLUMSY_OCR=1 -mwindows -s \
    -Wall -Wno-unused-parameter -Wno-format \
    -I"$WD/include" \
    -I"$IUP/include" \
    -I"$OCR/include" \
    src/*.c \
    "$OUT/clumsy_res.o" \
    "$IUP/libiup.a" \
    "$WD/x64/WinDivert.dll" \
    "$OCR/bin/x64/rapidocrlite.dll" \
    -lcomctl32 -lwinmm -lws2_32 -lgdi32 -lcomdlg32 -lshell32 -luuid -lole32 \
    -liphlpapi \
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
# OCR runtime/model licenses (Apache-2.0) travel next to the dll/models.
cp "$OCR/LICENSE"            "$OUT/LICENSE.RapidOCRLite.txt"
cp "$OCR/LICENSE-indra-ocr"  "$OUT/LICENSE.IndraOCR.txt"
cp "$OCR/VENDOR.md"          "$OUT/OCR-VENDOR.md"

echo "== done: $OUT/clumsy.exe"
