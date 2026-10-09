/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2026 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#include "crash.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(crash, LOG_LEVEL_INF);

#define CRASH_MAGIC 0x57A11ED5

// Right after struct retained_data (1 KB) in the 4 KB retained region at its start, clear of the
// bootloader's stack at the top of RAM and of the application's RAM. Survives a soft reset only.
#define CRASH_RECORD_ADDR (DT_REG_ADDR(DT_PARENT(DT_ALIAS(retainedmemdevice))) + 1024)

struct crash_record {
	uint32_t magic;
	uint32_t count; // crashes in a row within CRASH_EARLY_MS of boot
	uint32_t total;
	uint32_t reason;
	uint32_t pc, lr, sp;
	uint32_t cfsr, hfsr, mmfar, bfar;
	uint32_t uptime_ms;
	char thread[16];
};

BUILD_ASSERT(DT_REG_SIZE(DT_PARENT(DT_ALIAS(retainedmemdevice))) >= 1024 + 2 * sizeof(struct crash_record),
	     "retained region too small for the crash record");

#define rec ((volatile struct crash_record *)CRASH_RECORD_ADDR)

static bool safe_mode;
static struct crash_record last; // the record as found at boot

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	if (rec->magic != CRASH_MAGIC) {
		memset((void *)rec, 0, sizeof(*rec));
		rec->magic = CRASH_MAGIC;
	}
	uint32_t up = k_uptime_get_32();
	rec->count = up < CRASH_EARLY_MS ? rec->count + 1 : 1;
	rec->total++;
	rec->reason = reason;
	rec->pc = esf ? esf->basic.pc : 0;
	rec->lr = esf ? esf->basic.lr : 0;
	rec->sp = (uint32_t)esf;
	rec->cfsr = SCB->CFSR;
	rec->hfsr = SCB->HFSR;
	rec->mmfar = SCB->MMFAR;
	rec->bfar = SCB->BFAR;
	rec->uptime_ms = up;
	// k_panic() and asserts land here through an exception, so this is the thread that failed.
	const char *name = k_thread_name_get(k_current_get());
	bool end = name == NULL;
	for (int i = 0; i < (int)sizeof(rec->thread); i++) {
		char c = end || i == (int)sizeof(rec->thread) - 1 ? 0 : name[i];
		end = end || c == 0;
		rec->thread[i] = c;
	}
	LOG_PANIC();
	NVIC_SystemReset();
	CODE_UNREACHABLE;
}

static int crash_init(void)
{
	if (rec->magic != CRASH_MAGIC) { // power-on: RAM is random
		memset((void *)rec, 0, sizeof(*rec));
		rec->magic = CRASH_MAGIC;
	}
	memcpy(&last, (const void *)rec, sizeof(last));
	last.thread[sizeof(last.thread) - 1] = 0;
	safe_mode = last.count >= CRASH_SAFE_COUNT;
	return 0;
}

SYS_INIT(crash_init, PRE_KERNEL_1, 0);

// Not a crash loop once the board has run this long; safe mode itself stays until "mode".
// Runs on the system workqueue (256-byte stack): no printing here, "wired" shows the record.
static void early_over_fn(struct k_work *work)
{
	if (!safe_mode)
		rec->count = 0;
}
static K_WORK_DELAYABLE_DEFINE(early_over_work, early_over_fn);

static int crash_late_init(void)
{
	k_work_schedule(&early_over_work, K_MSEC(CRASH_EARLY_MS));
	return 0;
}

SYS_INIT(crash_late_init, APPLICATION, 0);

bool crash_safe_mode(void)
{
	return safe_mode;
}

void crash_clear(void)
{
	rec->count = 0;
}

void crash_print(void)
{
	if (!last.total) {
		printk("No crash recorded since power-on\n");
		return;
	}
	printk("Last crash: reason %u in thread '%s' at %u ms, PC 0x%08x LR 0x%08x SP 0x%08x\n", last.reason,
	       last.thread, last.uptime_ms, last.pc, last.lr, last.sp);
	printk("  CFSR 0x%08x HFSR 0x%08x MMFAR 0x%08x BFAR 0x%08x; %u in a row, %u since power-on%s\n", last.cfsr,
	       last.hfsr, last.mmfar, last.bfar, last.count, last.total,
	       safe_mode ? "; SAFE MODE: wired link and hub not started (set a mode to retry)" : "");
}
