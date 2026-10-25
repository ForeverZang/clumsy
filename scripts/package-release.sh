#!/usr/bin/env bash
# Assemble a distributable folder and zip from a gcc release build.
# Prerequisite: SIGN=A scripts/build-release.sh
# Output: build/dist/clumsy-<version>-win64/ and the matching .zip
#
# Contents: clumsy.exe, WinDivert runtime, RapidOCRLite runtime + models,
# config, all licenses, and the bilingual Quick Start texts.
set -euo pipefail
cd "$(dirname "$0")/.."

SIGN=${SIGN:-A}
WD="external/WinDivert-2.2.0-$SIGN"
OCR="external/RapidOCRLite"
SRC="build/mingw64-release"

VER=$(grep -E '#define[[:space:]]+CLUMSY_VERSION[[:space:]]+"[^"]+"' \
      src/common.h | sed -E 's/.*"([^"]+)".*/\1/')
NAME="clumsy-$VER-win64"
OUT="build/dist/$NAME"

if [ ! -x "$SRC/clumsy.exe" ]; then
    echo "missing $SRC/clumsy.exe — run: SIGN=$SIGN scripts/build-release.sh" >&2
    exit 2
fi
for f in "$SRC/rapidocrlite.dll" "$SRC/models/PP-OCRv6_det_small.onnx" \
         "$SRC/models/PP-OCRv6_rec_small.onnx" "$SRC/models/ppocrv6_dict.txt"; do
    if [ ! -f "$f" ]; then
        echo "missing OCR runtime file: $f" >&2
        exit 2
    fi
done

rm -rf "$OUT"
mkdir -p "$OUT/models"
cp "$SRC/clumsy.exe" "$SRC/config.txt" "$OUT/"
cp "$SRC/WinDivert.dll" "$SRC/WinDivert64.sys" "$OUT/"
cp "$SRC/rapidocrlite.dll" "$OUT/"
cp "$SRC/models/"* "$OUT/models/"

cp LICENSE "$OUT/LICENSE.clumsy.txt"
cp "$WD/LICENSE" "$OUT/LICENSE.WinDivert.txt"
cp "$OCR/LICENSE" "$OUT/LICENSE.RapidOCRLite.txt"
cp "$OCR/LICENSE-indra-ocr" "$OUT/LICENSE.IndraOCR.txt"
cp "$OCR/VENDOR.md" "$OUT/OCR-VENDOR.md"
cp packaging/README-QuickStart.txt "$OUT/"
cp "packaging/快速上手.txt" "$OUT/"

rm -f "build/dist/$NAME.zip"
# Windows bsdtar opens -f relative to the ORIGINAL cwd even with -C before
# it, so archive from inside build/dist in a subshell. bsdtar also sets the
# ZIP UTF-8 flag for non-ASCII entry names, so the Chinese Quick Start
# filename survives extraction in Explorer.
( cd build/dist && "$WINDIR/System32/tar.exe" -a -cf "$NAME.zip" "$NAME" )

echo "== $OUT"
echo "== build/dist/$NAME.zip"
du -sh "$OUT" "build/dist/$NAME.zip"
