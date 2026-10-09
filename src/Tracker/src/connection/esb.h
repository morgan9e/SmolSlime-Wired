/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2025 SlimeVR Contributors

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
#ifndef SLIMENRF_ESB
#define SLIMENRF_ESB

#include <esb.h>
#include <nrfx_timer.h>

// TODO: timer?
#define LAST_RESET_LIMIT 10
extern uint8_t last_reset;
// TODO: move to esb/timer
//extern const nrfx_timer_t m_timer;
extern bool esb_state;
extern bool timer_state;
extern bool send_data;
// TODO: esb/sensor?
extern uint16_t led_clock;
extern uint32_t led_clock_offset;

void event_handler(struct esb_evt const *event);
int clocks_start(void);
void clocks_stop(void);
void clocks_request_start(uint32_t delay_us);
void clocks_request_stop(uint32_t delay_us);
// Releases the HF clock unless a frame is still on the air or frames come back to back
// (relaying). The TX event releases it once the frame is done.
void clocks_release_when_idle(void);
int esb_initialize(bool);
void esb_deinitialize(void);

void esb_set_addr_discovery(void);
void esb_set_addr_paired(void);

void esb_set_pair(uint64_t addr);

void esb_pair(void);
void esb_reset_pair(void);
void esb_clear_pair(void);

void esb_write(uint8_t *data); // TODO: give packets some names

bool esb_ready(void);

// Prints radio ACK statistics per frame id (the "link" console command).
void esb_print_link(void);

#if CONFIG_WIRED_LINK
// Pairs another device (a wired ankle) with the receiver through this tracker's radio.
// Blocks for up to about 10 s. Returns 0 and the receiver's id for that address.
int esb_pair_other(const uint8_t addr[6], uint8_t *id_out);
#endif

#endif