#!/bin/sh
# Builds the host simulation. Usage: run_sim.sh [duration_s] [server_ip|auto]
set -e
cd "$(dirname "$0")"
if [ -n "$CC" ]; then COMPILER="$CC"; elif command -v cc >/dev/null 2>&1; then COMPILER=cc; else COMPILER="zig cc"; fi
M=../../main
$COMPILER -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Ishim -I$M ${SIM_CFLAGS} \
	sim_main.c shim/sim_rtos.c $M/slime.c $M/decoder.c $M/deframer.c $M/cobs.c $M/crc16.c \
	-lm -lpthread -o /tmp/hub_sim
exec /tmp/hub_sim "$@"
