#!/bin/sh
# Builds and runs the host tests. Uses $CC, or cc, or "zig cc" if available.
set -e
cd "$(dirname "$0")"
if [ -n "$CC" ]; then COMPILER="$CC"
elif command -v cc >/dev/null 2>&1; then COMPILER=cc
elif command -v zig >/dev/null 2>&1; then COMPILER="zig cc"
else echo "no C compiler found (set CC)"; exit 1; fi
$COMPILER -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I../main \
	host_test.c ../main/cobs.c ../main/crc16.c ../main/decoder.c ../main/deframer.c ../main/wired_link.c -lm -o /tmp/hub_host_test
/tmp/hub_host_test
