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
#ifndef SLIMENRF_LOG_MODE
#define SLIMENRF_LOG_MODE

#include <stdbool.h>
#include <stdint.h>

// Console log modes, switched at runtime and reset on reboot:
// DEBUG shows every LOG_DBG message of the firmware; TRACE decodes every frame the tracker
// hands to a transport (radio, cable, relay, USB HID). The two are independent.
#if CONFIG_LOG_MODES
void log_mode_set_debug(bool on);
bool log_mode_debug(void);
// Logs every nth frame; 0 turns TRACE off.
void log_mode_set_trace(uint32_t every);
uint32_t log_mode_trace_every(void);
// Re-applies the modes; call after (re)enabling a log backend, which resets its filters.
void log_mode_apply(void);
// frame: 16-byte packet, plus CRC32 and sequence when seq >= 0. route: string literal.
void log_mode_trace(const char *route, const uint8_t *frame, int seq);
#else
static inline void log_mode_apply(void) {}
static inline void log_mode_trace(const char *route, const uint8_t *frame, int seq)
{
	(void)route;
	(void)frame;
	(void)seq;
}
#endif

#endif
