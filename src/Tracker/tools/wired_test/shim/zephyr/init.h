#pragma once
#define SYS_INIT(fn, lvl, prio) int (*const fn##_ptr)(void) = fn
