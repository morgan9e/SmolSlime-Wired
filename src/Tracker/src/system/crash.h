#ifndef SLIMENRF_SYSTEM_CRASH
#define SLIMENRF_SYSTEM_CRASH

#include <stdbool.h>

// Crash record kept in retained RAM across the reset that follows a fatal error.
// After CRASH_SAFE_COUNT crashes in a row, each within CRASH_EARLY_MS of boot, the board starts in
// safe mode: the wired link and hub are not started, so a mode that crashes at boot does not lock
// out USB (set a mode again with "mode" to leave it).
#define CRASH_SAFE_COUNT 3
#define CRASH_EARLY_MS 15000

bool crash_safe_mode(void);
void crash_clear(void);
void crash_print(void);

#endif
