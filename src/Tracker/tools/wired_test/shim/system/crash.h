/* Host test shim for src/system/crash.h: never in safe mode. */
#pragma once
#include <stdbool.h>
#define CRASH_SAFE_COUNT 3
static inline bool crash_safe_mode(void) { return false; }
static inline void crash_clear(void) {}
static inline void crash_print(void) {}
