#!/bin/sh
# Host test for src/connection/wired.c (ankle encoder -> wire -> relay decoder/stamper).
# Usage: run.sh <path to zephyr>   (for lib/crc/crc32k_4_2_sw.c); CC defaults to cc.
set -e
cd "$(dirname "$0")"
ZEPHYR=${1:?give the zephyr directory of your west workspace}
OUT=$(mktemp -d)
cp -r shim "$OUT/" && cp ../../src/connection/wired.c ../../src/connection/wired.h ../../src/connection/hub.h "$OUT/shim/"
${CC:-cc} -std=gnu11 -O1 -Wall -Wno-format -Wno-unused-function -Wno-unused-variable -I"$OUT/shim" \
	wired_test.c "$ZEPHYR/lib/crc/crc32k_4_2_sw.c" -o "$OUT/wired_test"
"$OUT/wired_test"
