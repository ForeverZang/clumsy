#!/usr/bin/env bash
# Build and run every pure-logic test in tests/ (see tests/README.md).
# Each test TU owns its main(); GUI and real-capture behavior is verified
# manually per spec §7. Exits non-zero if any build or test fails.
set -u
cd "$(dirname "$0")/.."

WD=external/WinDivert-2.2.0-A
IUP=external/iup-3.30_Win64_mingw6_lib
FLAGS="-std=gnu11 -g -O0 -D_DEBUG -Wall -Wno-unused-parameter \
       -I$WD/include -I$IUP/include -Isrc"
LIBS="$IUP/libiup.a $WD/x64/WinDivert.dll -lws2_32 -lwinmm -lgdi32 \
      -lcomctl32 -lcomdlg32 -luuid -lole32 -liphlpapi"

mkdir -p build/tests

srcs_for() {
    case "$1" in
        sanity)         SRC="" ;;
        flowmap)        SRC="src/flowmap.c src/procmap.c" ;;
        procmap)        SRC="src/procmap.c" ;;
        flowwatch_seed) SRC="src/flowwatch.c src/flowmap.c src/procmap.c" ;;
        pktparse)       SRC="src/pktparse.c src/packet.c" ;;
        targeting)      SRC="src/targeting.c src/procmap.c src/swfilter.c src/pktparse.c" ;;
        simple)         SRC="src/simple.c src/targeting.c src/procmap.c src/swfilter.c \
                         src/pktparse.c src/packet.c src/utils.c" ;;
        traffic)        SRC="src/traffic.c" ;;
        i18n)           SRC="src/i18n.c" ;;
        swfilter)       SRC="src/swfilter.c" ;;
        ocr_rules)      SRC="src/ocr_rules.c" ;;
        ocr_timeline)   SRC="src/ocr_timeline.c" ;;
        ui_simple)      SRC="src/ui_simple.c src/traffic.c src/i18n.c src/simple.c \
                         src/targeting.c src/procmap.c src/swfilter.c \
                         src/pktparse.c src/packet.c src/utils.c" ;;
        *) echo "unknown test: $1" >&2; exit 2 ;;
    esac
}

TESTS="sanity flowmap procmap flowwatch_seed pktparse targeting simple \
       traffic i18n swfilter ocr_rules ocr_timeline ui_simple"

fail=0
ran=0
for name in $TESTS; do
    srcs_for "$name"
    out="build/tests/test_$name.exe"
    # shellcheck disable=SC2086
    if ! gcc $FLAGS "tests/test_$name.c" $SRC $LIBS -o "$out" 2>build/tests/$name.build.log; then
        echo "BUILD FAIL: $name (see build/tests/$name.build.log)"
        fail=1
        continue
    fi
    line=$("$out" 2>&1 | tail -1)
    if [ "$line" = "OK: $name" ]; then
        echo "PASS: $name"
    else
        echo "FAIL: $name -> $line"
        fail=1
    fi
    ran=$((ran + 1))
done

echo "ran $ran test programs"
exit $fail
