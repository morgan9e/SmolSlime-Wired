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
#include "globals.h"
#include "system/system.h"
#include "connection.h"

#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#if defined(NRF54L15_XXAA)
#include <hal/nrf_clock.h>
#endif /* defined(NRF54L15_XXAA) */
#include <zephyr/sys/crc.h>

#include "esb.h"
#include "wired.h"

uint8_t last_reset = 0;
//const nrfx_timer_t m_timer = NRFX_TIMER_INSTANCE(1);
bool esb_state = false;
bool timer_state = false;
bool send_data = false;
uint16_t led_clock = 0;
uint32_t led_clock_offset = 0;

uint32_t tx_errors = 0;
int64_t last_tx_success = 0;
int64_t last_tx_fail = 0;

// Radio ACKs per frame id (this tracker's own id, and relayed wired trackers), for "link".
// An ACK only proves that a receiver listening on the paired address heard the frame; the
// receiver can still drop it, e.g. an id it has not stored, which only its own log shows.
#define LINK_SLOTS 8
static struct link_stat {
	bool used;
	uint8_t id;
	uint32_t sent, acked, failed;
	int64_t last_ack;
} link_stats[LINK_SLOTS], link_shown[LINK_SLOTS]; // link_shown: values at the previous "link"
static volatile int link_inflight = -1; // slot of the frame waiting for its TX event

// Radio stall watchdog. esb_write_payload() starts a transmission only while the ESB driver is
// idle; if its state machine ever stops in another state, every frame is queued and none is
// sent, with neither TX_SUCCESS nor TX_FAILED (seen on a relaying thigh: no ACK for minutes,
// "Consecutive TX errors: 0"). A frame takes at most a few ms, so restart ESB when frames have
// been waiting for a TX event for RADIO_STALL_MS.
#define RADIO_STALL_MS 100
static volatile int64_t tx_waiting_since; // first frame written since the last TX event, 0: none
static uint32_t radio_restarts;
static uint16_t retransmit_delay_us; // per tracker, see esb_initialize()
static int64_t last_radio_restart;

static int link_slot(uint8_t id)
{
	for (int i = 0; i < LINK_SLOTS; i++)
		if (link_stats[i].used && link_stats[i].id == id)
			return i;
	for (int i = 0; i < LINK_SLOTS; i++)
		if (!link_stats[i].used)
		{
			link_stats[i] = (struct link_stat){.used = true, .id = id};
			return i;
		}
	return -1;
}

static void link_tx_event(bool success)
{
	int slot = link_inflight;
	link_inflight = -1;
	if (slot < 0)
		return;
	if (success)
	{
		link_stats[slot].acked++;
		link_stats[slot].last_ack = k_uptime_get();
	}
	else
	{
		link_stats[slot].failed++;
	}
}

static struct esb_payload rx_payload;
static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_pair = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0, 0, 0, 0, 0);

static uint8_t paired_addr[8] = {0};

static bool esb_initialized = false;
static bool esb_paired = false;

#define TX_ERROR_THRESHOLD 30
#define TX_ERROR_MAX 100
#define TX_ERROR_CLEAR_RATE 10

LOG_MODULE_REGISTER(esb_event, LOG_LEVEL_DBG);

#if CONFIG_WIRED_LINK
// A relay sends its own and the ankle's frames back to back. Instead of flushing the
// previous frame (which would cut an ankle frame off mid-retransmit), wait for it to finish.
static K_SEM_DEFINE(esb_tx_done, 1, 1);
static uint32_t esb_tx_timeout_us = 4500; // set from the ESB retransmit settings in esb_initialize()
// Pairing requests sent while staying paired: for a wired ankle (esb_pair_other) and the
// background re-pair (esb_try_repair). One at a time.
static volatile bool other_pairing;
static volatile bool other_ack_ready;
static uint8_t other_ack[8];
static K_MUTEX_DEFINE(pair_request_lock);
// While the receiver does not answer, ask every REPAIR_RETRY_MS whether a receiver in pairing mode
// has a new address or id for this tracker. A receiver changes its address on "clear", so every
// tracker paired before ends up here and gets a fresh id instead of colliding with the new ones.
#define REPAIR_RETRY_MS 3000
static int64_t last_repair_try;
#endif

// Keep the HF clock running between transmissions while frames come back to back.
static inline bool esb_hold_clock(void)
{
	if (IS_ENABLED(CONFIG_DEBUG_NO_POWER_SAVING))
		return true;
#if CONFIG_WIRED_LINK
	return wired_relaying() || other_pairing;
#else
	return false;
#endif
}

static void esb_thread(void);
K_THREAD_DEFINE(esb_thread_id, 512, esb_thread, NULL, NULL, NULL, ESB_THREAD_PRIORITY, 0, 0);

uint64_t pairing_packets = 0;

//|type    |description
//|TX  CRC8|pairing
//|RX  CRC8|pairing

//|b0      |b1      |b2      |b3      |b4      |b5      |b6      |b7      |b8      |b9      |b10     |b11     |b12     |b13     |b14     |b15     |
//|type    |id      |packet data                                                                                                                  |
//|TX  CRC8|ack     |device_addr                                          |
//|RX  CRC8|ack     |recv_addr                                            |

void event_handler(struct esb_evt const *event)
{
	switch (event->evt_id)
	{
	case ESB_EVENT_TX_SUCCESS:
		if (!paired_addr[0]) // zero, not paired
			pairing_packets++; // keep track of pairing state
		if (tx_errors >= TX_ERROR_THRESHOLD && tx_errors < TX_ERROR_THRESHOLD + TX_ERROR_CLEAR_RATE && last_tx_fail == 0)
		{
			last_tx_success = 0; // reset last_tx_success on threshold reached
			last_tx_fail = k_uptime_get();
		}
		if (tx_errors > TX_ERROR_CLEAR_RATE)
			tx_errors -= TX_ERROR_CLEAR_RATE;
		else
			tx_errors = 0;
		link_tx_event(true);
		tx_waiting_since = 0;
		LOG_DBG("TX SUCCESS");
#if CONFIG_WIRED_LINK
		k_sem_give(&esb_tx_done);
#endif
		if (esb_paired && !esb_hold_clock())
			clocks_stop();
		break;
	case ESB_EVENT_TX_FAILED:
		if (tx_errors < TX_ERROR_MAX)
			tx_errors++;
		if (tx_errors == TX_ERROR_THRESHOLD && last_tx_success == 0) // consecutive failure to transmit
		{
			last_tx_fail = 0; // reset last_tx_fail on threshold reached
			last_tx_success = k_uptime_get();
		}
		link_tx_event(false);
		tx_waiting_since = 0;
		LOG_DBG("TX FAILED");
#if CONFIG_WIRED_LINK
		k_sem_give(&esb_tx_done);
#endif
		if (esb_paired && !esb_hold_clock())
			clocks_stop();
		break;
	case ESB_EVENT_RX_RECEIVED:
		// TODO: have to read rx until -ENODATA (or -EACCES/-EINVAL)
		if (!esb_read_rx_payload(&rx_payload)) // zero, rx success
		{
			LOG_DBG("rx len: %d, pipe: %d", rx_payload.length, rx_payload.pipe);
#if CONFIG_WIRED_LINK
			if (other_pairing) // reply to a pairing request sent on behalf of a wired ankle
			{
				if (rx_payload.length == 8)
				{
					memcpy(other_ack, rx_payload.data, sizeof(other_ack));
					other_ack_ready = true;
				}
				break;
			}
#endif
			if (!paired_addr[0]) // zero, not paired
			{
				LOG_DBG("tx: %16llX rx: %16llX", *(uint64_t *)tx_payload_pair.data, *(uint64_t *)rx_payload.data);
				if (rx_payload.length == 8 && tx_payload_pair.data[1] == 1) // ack to second packet in pairing burst
					memcpy(paired_addr, rx_payload.data, sizeof(paired_addr));
			}
			else
			{
				if (rx_payload.length == 4)
				{
					// TODO: Device should never receive packets if it is already paired, why is this packet received?
					// This may be part of acknowledge
//					if (!nrfx_timer_init_check(&m_timer))
					{
						LOG_WRN("Timer not initialized");
						break;
					}
					if (timer_state == false)
					{
//						nrfx_timer_resume(&m_timer);
						timer_state = true;
					}
//					nrfx_timer_clear(&m_timer);
					last_reset = 0;
					led_clock = (rx_payload.data[0] << 8) + rx_payload.data[1]; // sync led flashes :)
					led_clock_offset = 0;
					LOG_DBG("RX, timer reset");
				}
			}
		}
		break;
	}
}

bool clock_status = false;

#if defined(CONFIG_CLOCK_CONTROL_NRF)
static struct onoff_manager *clk_mgr;

static int clocks_init(void)
{
	clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (!clk_mgr)
	{
		LOG_ERR("Unable to get the Clock manager");
		return -ENOTSUP;
	}

	return 0;
}

SYS_INIT(clocks_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int clocks_start(void)
{
	if (clock_status)
		return 0;
	int err;
	int res;
	struct onoff_client clk_cli;
	int fetch_attempts = 0;

	sys_notify_init_spinwait(&clk_cli.notify);

	err = onoff_request(clk_mgr, &clk_cli);
	if (err < 0)
	{
		LOG_ERR("Clock request failed: %d", err);
		return err;
	}

	do
	{
		k_usleep(100);
		err = sys_notify_fetch_result(&clk_cli.notify, &res);
		if (!err && res)
		{
			LOG_ERR("Clock could not be started: %d", res);
			return res;
		}
		if (err && ++fetch_attempts > 10) {
			LOG_WRN_ONCE("Unable to fetch Clock request result: %d", err);
			return err;
		}
	} while (err);

#if defined(NRF54L15_XXAA)
	/* MLTPAN-20 */
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART);
#endif /* defined(NRF54L15_XXAA) */

	LOG_DBG("HF clock started");
	clock_status = true;
	return 0;
}

void clocks_stop(void)
{
	if (!clock_status)
		return;
	clock_status = false;

	onoff_release(clk_mgr);

	LOG_DBG("HF clock stop request");
}

#else
BUILD_ASSERT(false, "No Clock Control driver");
#endif

void clocks_release_when_idle(void)
{
	// Stopping the HFXO under a frame that is still being sent or retransmitted moves the radio
	// onto the RC oscillator, off frequency: the receiver never hears it. On USB the USB stack keeps
	// the HFXO running anyway, so this only shows on battery (a relaying thigh lost most ankle frames).
	if (esb_hold_clock())
		return;
	if (esb_initialized && !esb_is_idle())
		return; // the TX event stops the clock when the frame is done
	clocks_stop();
}

static struct k_thread clocks_thread_id;
static K_THREAD_STACK_DEFINE(clocks_thread_id_stack, 128);

void clocks_request_start(uint32_t delay_us)
{
	k_thread_create(&clocks_thread_id, clocks_thread_id_stack, K_THREAD_STACK_SIZEOF(clocks_thread_id_stack), (k_thread_entry_t)clocks_start, NULL, NULL, NULL, CLOCKS_START_THREAD_PRIORITY, 0, K_USEC(delay_us));
}

static struct k_thread clocks_stop_thread_id;
static K_THREAD_STACK_DEFINE(clocks_stop_thread_id_stack, 128);

void clocks_request_stop(uint32_t delay_us)
{
	k_thread_create(&clocks_stop_thread_id, clocks_stop_thread_id_stack, K_THREAD_STACK_SIZEOF(clocks_stop_thread_id), (k_thread_entry_t)clocks_stop, NULL, NULL, NULL, CLOCKS_STOP_THREAD_PRIORITY, 0, K_USEC(delay_us));
}

// this was randomly generated
// TODO: I have no idea?
// TODO: see esb information, check CONFIG_ESB_PIPE_COUNT
/*
base_addr_p0: Base address for pipe 0, in big endian.
base_addr_p1: Base address for pipe 1-7, in big endian.
pipe_prefixes: Address prefix for pipe 0 to 7.
*/
static const uint8_t discovery_base_addr_0[4] = {0x62, 0x39, 0x8A, 0xF2};
static const uint8_t discovery_base_addr_1[4] = {0x28, 0xFF, 0x50, 0xB8}; // TODO: not used
static const uint8_t discovery_addr_prefix[8] = {0xFE, 0xFF, 0x29, 0x27, 0x09, 0x02, 0xB2, 0xD6};

static uint8_t base_addr_0[4], base_addr_1[4], addr_prefix[8] = {0};

int esb_initialize(bool tx)
{
	int err;

	struct esb_config config = ESB_DEFAULT_CONFIG;
	// The receiver must use the same rate ("radio 1m" on its console)
	if (CONFIG_0_SETTINGS_READ(CONFIG_0_RADIO_1MBPS))
		config.bitrate = ESB_BITRATE_1MBPS;

	if (tx)
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		// config.mode = ESB_MODE_PTX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = CONFIG_2_SETTINGS_READ(CONFIG_2_RADIO_TX_POWER);
		// All trackers share one channel. With the same retransmit delay, two trackers whose
		// frames collide retry at the same moment and collide again on every attempt, so both
		// lose the frame (a relaying thigh saw 1 in 4 frames fail all 4 attempts). Give each
		// tracker its own delay from its device address: 600 (stock) to 1475 us, 125 us steps.
		uint32_t a = NRF_FICR->DEVICEADDR[0];
		config.retransmit_delay = 600 + ((a ^ (a >> 8) ^ (a >> 16) ^ (a >> 24)) & 7) * 125;
		retransmit_delay_us = config.retransmit_delay;
		//config.retransmit_count = 0;
		//config.tx_mode = ESB_TXMODE_MANUAL;
		// config.payload_length = 32;
		config.selective_auto_ack = true; // TODO: while pairing, should be set to false
//		config.use_fast_ramp_up = true;
	}
	else
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		config.mode = ESB_MODE_PRX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = CONFIG_2_SETTINGS_READ(CONFIG_2_RADIO_TX_POWER);
		// config.retransmit_delay = 600;
		// config.retransmit_count = 3;
		// config.tx_mode = ESB_TXMODE_AUTO;
		// config.payload_length = 32;
		config.selective_auto_ack = true;
//		config.use_fast_ramp_up = true;
	}

	err = esb_init(&config);

	if (!err)
		esb_set_base_address_0(base_addr_0);

	if (!err)
		esb_set_base_address_1(base_addr_1);

	if (!err)
		esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));

	if (err)
	{
		LOG_ERR("ESB initialization failed: %d", err);
		set_status(SYS_STATUS_CONNECTION_ERROR, true);
		return err;
	}

#if CONFIG_WIRED_LINK
	// Worst case for one frame: every retransmit, about 400 us of airtime each (650 at 1 Mbps), plus margin.
	uint32_t airtime_us = config.bitrate == ESB_BITRATE_1MBPS ? 650 : 400;
	esb_tx_timeout_us = (config.retransmit_delay + airtime_us) * (config.retransmit_count + 1) + 500;
	k_sem_reset(&esb_tx_done);
	k_sem_give(&esb_tx_done);
#endif
	esb_initialized = true;
	return 0;
}

void esb_deinitialize(void)
{
	if (esb_initialized)
	{
		esb_initialized = false;
		k_msleep(10); // wait for pending transmissions
		esb_disable();
	}
	esb_initialized = false;
}

inline void esb_set_addr_discovery(void)
{
	memcpy(base_addr_0, discovery_base_addr_0, sizeof(base_addr_0));
	memcpy(base_addr_1, discovery_base_addr_1, sizeof(base_addr_1));
	memcpy(addr_prefix, discovery_addr_prefix, sizeof(addr_prefix));
}

inline void esb_set_addr_paired(void)
{
	// Recreate receiver address
	uint8_t addr_buffer[16] = {0};
	for (int i = 0; i < 4; i++)
	{
		addr_buffer[i] = paired_addr[i + 2];
		addr_buffer[i + 4] = paired_addr[i + 2] + paired_addr[6];
	}
	for (int i = 0; i < 8; i++)
		addr_buffer[i + 8] = paired_addr[7] + i;
	for (int i = 0; i < 16; i++)
	{
		if (addr_buffer[i] == 0x00 || addr_buffer[i] == 0x55 || addr_buffer[i] == 0xAA) // Avoid invalid addresses (see nrf datasheet)
			addr_buffer[i] += 8;
	}
//	memcpy(base_addr_0, addr_buffer, sizeof(base_addr_0));
	memcpy(base_addr_1, addr_buffer + 4, sizeof(base_addr_1));
//	memcpy(addr_prefix, addr_buffer + 8, sizeof(addr_prefix));
	memcpy(base_addr_0, discovery_base_addr_0, sizeof(base_addr_0));
	memcpy(addr_prefix, discovery_addr_prefix, sizeof(addr_prefix));
}

void esb_set_pair(uint64_t addr)
{
	uint64_t *device_addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet)
	uint8_t buf[6] = {0};
	memcpy(buf, device_addr, 6);
	uint8_t checksum = crc8_ccitt(0x07, buf, 6);
	if (checksum == 0)
		checksum = 8;
	if ((addr & 0xFF) != checksum)
	{
		LOG_INF("Incorrect checksum");
		return;
	}
	esb_reset_pair();
	memcpy(paired_addr, &addr, sizeof(paired_addr));
	LOG_INF("Paired");
	sys_write(PAIRED_ID, retained->paired_addr, paired_addr, sizeof(paired_addr)); // Write new address and tracker id
}

void esb_pair(void)
{
	if (get_status(SYS_STATUS_CONNECTION_ERROR))
		set_status(SYS_STATUS_CONNECTION_ERROR, false);
	tx_errors = 0;
	if (!paired_addr[0]) // zero, no receiver paired
	{
		LOG_INF("Pairing");
		esb_set_addr_discovery();
		esb_initialize(true);
//		timer_init(); // TODO: shouldn't be here!!!
		tx_payload_pair.pipe = 0;
		tx_payload_pair.noack = false;
		uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet)
		memcpy(&tx_payload_pair.data[2], addr, 6);
		LOG_INF("Device address: %012llX", *addr & 0xFFFFFFFFFFFF);
		uint8_t checksum = crc8_ccitt(0x07, &tx_payload_pair.data[2], 6);
		if (checksum == 0)
			checksum = 8;
		LOG_INF("Checksum: %02X", checksum);
		tx_payload_pair.data[0] = checksum; // Use checksum to make sure packet is for this device
		set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);
		while (paired_addr[0] != checksum)
		{
			int64_t time_begin = k_uptime_get();

			if (!esb_initialized)
			{
				esb_set_addr_discovery();
				esb_initialize(true);
			}

			if (!clock_status)
				clocks_start();

			if (paired_addr[0])
			{
				LOG_INF("Incorrect checksum: %02X", paired_addr[0]);
				paired_addr[0] = 0; // Packet not for this device
			}

//			esb_flush_rx();
//			esb_flush_tx();

			pairing_packets = 0;

			// send pairing request
			tx_payload_pair.data[1] = 0;
			esb_write_payload(&tx_payload_pair);
			k_usleep(100);
			while (!esb_is_idle() && (k_uptime_get() < (time_begin + 10)))
				k_usleep(1);

			// receive ack data
			tx_payload_pair.data[1] = 1;
			if ((pairing_packets == 1) && (k_uptime_get() < (time_begin + 10)))
				esb_write_payload(&tx_payload_pair);
			k_usleep(100);
			while (!esb_is_idle() && (k_uptime_get() < (time_begin + 10)))
				k_usleep(1);

			if (pairing_packets == 2)
				LOG_INF("Pairing request received");

			if (clock_status)
				clocks_stop();

			int64_t time_delta = k_uptime_get() - time_begin;
			if (time_delta > 1000)
				k_yield();
			else
				k_msleep(1000 - time_delta);
		}
		set_led(SYS_LED_PATTERN_ONESHOT_COMPLETE, SYS_LED_PRIORITY_PAIR);
		LOG_INF("Paired");
		sys_write(PAIRED_ID, retained->paired_addr, paired_addr, sizeof(paired_addr)); // Write new address and tracker id
		esb_deinitialize();
		k_msleep(1600); // wait for led pattern
	}
	LOG_INF("Tracker ID: %u", paired_addr[1]);
	LOG_INF("Receiver address: %012llX", (*(uint64_t *)&retained->paired_addr[0] >> 16) & 0xFFFFFFFFFFFF);

	connection_set_id(paired_addr[1]);

	esb_set_addr_paired();
	esb_paired = true;
	clocks_stop();
}

void esb_reset_pair(void)
{
	if (paired_addr[0] || esb_paired)
	{
		esb_deinitialize(); // make sure esb is off
		esb_paired = false;
		memset(paired_addr, 0, sizeof(paired_addr));
		LOG_INF("Pairing requested");
	}
}

void esb_clear_pair(void)
{
	esb_reset_pair();
	sys_write(PAIRED_ID, &retained->paired_addr, paired_addr, sizeof(paired_addr)); // write zeroes
	LOG_INF("Pairing data reset");
}

void esb_write(uint8_t *data)
{
	if (!esb_initialized || !esb_paired)
		return;
#if CONFIG_WIRED_LINK
	if (other_pairing)
		return;
#endif
	int64_t now = k_uptime_get();
	int64_t waiting = tx_waiting_since;
	if (waiting && now - waiting > RADIO_STALL_MS && !esb_is_idle())
	{
		radio_restarts++;
		last_radio_restart = now;
		LOG_WRN("Radio stalled: no TX event for %lld ms, restarting ESB (restart %u)", now - waiting, radio_restarts);
		esb_disable();
		esb_initialize(true); // same addresses; also resets esb_tx_done
		tx_waiting_since = 0;
	}
	if (!clock_status)
		clocks_start();
	tx_payload.pipe = 1; // using base address 1
#if defined(NRF54L15_XXAA) // TODO: esb halts with ack and tx fail
	tx_payload.noack = true;
#else
	tx_payload.noack = false;
#endif
#if CONFIG_WIRED_LINK
	if (wired_relaying())
	{
		k_sem_take(&esb_tx_done, K_USEC(esb_tx_timeout_us));
		// A frame that ran out of retransmits stays at the head of the ESB TX FIFO and would go
		// out again before this one. It usually arrived and only the ACK was lost, and the
		// receiver's duplicate filter does not catch the copy once another tracker sent on the
		// pipe in between ("Sequence missmatch ... expected N, got N-1"). Drop it, as stock does;
		// after a success the FIFO is already empty. Also drops a frame that never finished.
		esb_flush_tx();
	}
	else
	{
		esb_flush_tx(); // stock behaviour while not relaying
		k_sem_reset(&esb_tx_done); // this frame's TX event gives it back
	}
	memcpy(tx_payload.data, data, tx_payload.length);
	int slot = link_slot(data[1]);
	link_inflight = -1; // a flushed frame gets no TX event; it shows up as "no result"
	if (esb_write_payload(&tx_payload) != 0) // Add transmission to queue
		k_sem_give(&esb_tx_done); // nothing in flight
	else
	{
		if (!tx_waiting_since)
			tx_waiting_since = now;
		if (slot >= 0)
		{
			link_stats[slot].sent++;
			link_inflight = slot;
		}
	}
#else
	memcpy(tx_payload.data, data, tx_payload.length);
	esb_flush_tx(); // this will clear all transmissions even if they did not complete
	int slot = link_slot(data[1]);
	link_inflight = -1; // a flushed frame gets no TX event; it shows up as "no result"
	if (esb_write_payload(&tx_payload) == 0) // Add transmission to queue
	{
		if (!tx_waiting_since)
			tx_waiting_since = now;
		if (slot >= 0)
		{
			link_stats[slot].sent++;
			link_inflight = slot;
		}
	}
#endif
	send_data = true;
}

void esb_print_link(void)
{
	if (wired_is_ankle())
	{
		printk("Wired mode: radio off, frames go over the cable (see \"wired\")\n");
		return;
	}
	if (!retained->paired_addr[0])
	{
		printk("Not paired with a receiver\n");
		return;
	}
	printk("Paired as id %u with receiver %012llX\n", retained->paired_addr[1],
		(*(uint64_t *)&retained->paired_addr[0] >> 16) & 0xFFFFFFFFFFFF);
	if (!esb_paired || !esb_initialized)
		printk("Radio: %s\n", esb_paired ? "not started" : "pairing");
	printk("Consecutive TX errors: %u (connection error at %u)\n", tx_errors, TX_ERROR_THRESHOLD);
	printk("Radio: %s, %s, retransmit delay %u us; restarted after a stall %u times", esb_is_idle() ? "idle" : "busy",
		CONFIG_0_SETTINGS_READ(CONFIG_0_RADIO_1MBPS) ? "1 Mbps" : "2 Mbps", retransmit_delay_us, radio_restarts);
	if (radio_restarts)
		printk(", last %lld s ago", (k_uptime_get() - last_radio_restart) / 1000);
	printk("\n");
	printk("Per frame id, total (+ since the last \"link\"):\n");
	int64_t now = k_uptime_get();
	bool any = false;
	for (int i = 0; i < LINK_SLOTS; i++)
	{
		struct link_stat s = link_stats[i];
		if (!s.used)
			continue;
		any = true;
		struct link_stat p = link_shown[i].used ? link_shown[i] : (struct link_stat){0};
		uint32_t ds = s.sent - p.sent, da = s.acked - p.acked, df = s.failed - p.failed;
		printk("  id %u%s: sent %u (+%u), acked %u (+%u), failed %u (+%u), no result %u",
			s.id, s.id == retained->paired_addr[1] ? " (this tracker)" : " (relayed)",
			s.sent, ds, s.acked, da, s.failed, df, s.sent - s.acked - s.failed);
		if (s.last_ack)
			printk(", last ACK %lld ms ago\n", now - s.last_ack);
		else
			printk(", never ACKed\n");
		link_shown[i] = s;
	}
	if (!any)
		printk("  nothing sent yet\n");
	printk("No ACKs: the receiver is off, out of range, or not the one this tracker paired with.\n");
	printk("ACKs but no data on the receiver: its log names ids it has not stored.\n");
}

bool esb_ready(void)
{
	if (wired_is_ankle())
		return true; // the radio is never used; do not hold off sleep waiting for pairing
	return esb_initialized && esb_paired;
}

#if CONFIG_WIRED_LINK
static void esb_wait_idle(int64_t deadline)
{
	while (!esb_is_idle() && k_uptime_get() < deadline)
		k_usleep(1);
}

// Sends pairing requests for addr on the discovery address, as esb_pair() does, for up to
// `rounds` rounds of one second, and returns the receiver's 8-byte reply (checksum, id, receiver
// address). Only a receiver in pairing mode answers. This tracker's own pairing is kept.
static int esb_pair_request(const uint8_t addr[6], int rounds, uint8_t reply[8], bool led)
{
	uint8_t checksum = crc8_ccitt(0x07, addr, 6);
	if (checksum == 0)
		checksum = 8;

	k_mutex_lock(&pair_request_lock, K_FOREVER);
	other_pairing = true; // esb_write() returns early from here on
	esb_deinitialize();   // waits for a pending transmission
	esb_set_addr_discovery();
	esb_initialize(true);
	clocks_start();
	if (led)
		set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);

	struct esb_payload p = ESB_CREATE_PAYLOAD(0, 0, 0, 0, 0, 0, 0, 0, 0);
	p.pipe = 0;
	p.noack = false;
	p.data[0] = checksum; // the receiver echoes this, so the reply is known to be for this address
	memcpy(&p.data[2], addr, 6);

	int ret = -ETIMEDOUT;
	for (int round = 0; round < rounds && ret; round++)
	{
		int64_t begin = k_uptime_get();
		other_ack_ready = false;
		// Request, as esb_pair() does for the tracker itself.
		p.data[1] = 0;
		esb_write_payload(&p);
		esb_wait_idle(begin + 10);
		// Fetch the reply. A receiver that answers slowly (stock firmware writes flash
		// before replying to a new address) gets up to 20 more chances.
		for (int i = 0; i < 20 && !other_ack_ready; i++)
		{
			p.data[1] = 1;
			esb_write_payload(&p);
			esb_wait_idle(k_uptime_get() + 10);
			if (!other_ack_ready)
				k_msleep(1);
		}
		if (other_ack_ready && other_ack[0] == checksum)
		{
			memcpy(reply, other_ack, sizeof(other_ack));
			ret = 0;
			break;
		}
		if (other_ack_ready)
			LOG_INF("Reply for another device (checksum %02X)", other_ack[0]);
		int64_t delta = k_uptime_get() - begin;
		if (round + 1 < rounds && delta < 1000)
			k_msleep(1000 - delta);
	}

	esb_deinitialize();
	esb_set_addr_paired();
	esb_initialize(true);
	other_pairing = false;
	k_mutex_unlock(&pair_request_lock);
	if (led)
		set_led(ret ? SYS_LED_PATTERN_OFF : SYS_LED_PATTERN_ONESHOT_COMPLETE, SYS_LED_PRIORITY_PAIR);
	return ret;
}

int esb_pair_other(const uint8_t addr[6], uint8_t *id_out)
{
	if (wired_is_ankle())
		return -EPERM;
	if (!esb_paired)
	{
		LOG_WRN("Pair this tracker before pairing a wired tracker through it");
		return -EPERM;
	}
	uint8_t reply[8];
	int ret = esb_pair_request(addr, 10, reply, true);
	if (ret == 0)
		*id_out = reply[1];
	return ret;
}

// Called while the receiver does not answer. Takes over the address and id a receiver in pairing
// mode hands out, and re-pairs the wired ankle on a new receiver (its old id belongs to the old one).
static void esb_try_repair(void)
{
	uint8_t addr[6], reply[8];
	memcpy(addr, (const void *)NRF_FICR->DEVICEADDR, sizeof(addr));
	if (esb_pair_request(addr, 1, reply, false) != 0)
		return; // no receiver in pairing mode
	if (memcmp(reply, paired_addr, sizeof(paired_addr)) == 0)
		return; // same receiver, same id
	bool new_receiver = memcmp(&reply[2], &paired_addr[2], 6) != 0;
	LOG_INF("Re-paired: id %u -> %u, receiver %012llX -> %012llX", paired_addr[1], reply[1],
		(*(uint64_t *)&paired_addr[0] >> 16) & 0xFFFFFFFFFFFF, (*(uint64_t *)&reply[0] >> 16) & 0xFFFFFFFFFFFF);
	esb_deinitialize();
	memcpy(paired_addr, reply, sizeof(paired_addr));
	sys_write(PAIRED_ID, retained->paired_addr, paired_addr, sizeof(paired_addr)); // Write new address and tracker id
	connection_set_id(paired_addr[1]);
	esb_set_addr_paired();
	esb_initialize(true);
	tx_errors = 0;
	set_led(SYS_LED_PATTERN_ONESHOT_COMPLETE, SYS_LED_PRIORITY_PAIR);
	if (new_receiver && !wired_is_ankle())
	{
		wired_forget_ankles();
		wired_request_pair_ankle(); // the receiver is still in pairing mode; no-op without an ankle
	}
}
#endif

static void esb_thread(void)
{
	if (IS_ENABLED(CONFIG_DEBUG_NO_POWER_SAVING))
		LOG_WRN("Debug build: power saving off (no low rate, sleep or shutdown when still)");
	if (wired_is_ankle())
	{
		LOG_INF("Wired mode: radio not used");
		return; // never pair or initialize; tx_errors stays 0, so no connection error or shutdown
	}
	bool use_hid = CONFIG_0_SETTINGS_READ(CONFIG_0_CONNECTION_OVER_HID);
	bool use_shutdown = CONFIG_0_SETTINGS_READ(CONFIG_0_USER_SHUTDOWN) && !IS_ENABLED(CONFIG_DEBUG_NO_POWER_SAVING);
	int64_t start_time = k_uptime_get();

	// Read paired address from retained
	memcpy(paired_addr, retained->paired_addr, sizeof(paired_addr));

	while (1)
	{
		if (!esb_paired && (!use_hid || paired_addr[0] || (!get_status(SYS_STATUS_USB_CONNECTED) && k_uptime_get() - 750 > start_time))) // only automatically enter pairing while not potentially communicating by usb, however allow esb if already paired
		{
			esb_pair();
			esb_initialize(true);
		}
		if (tx_errors >= TX_ERROR_THRESHOLD)
		{
			if (!get_status(SYS_STATUS_CONNECTION_ERROR) && (!use_hid || !get_status(SYS_STATUS_USB_CONNECTED))) // only raise error while not potentially communicating by usb
				set_status(SYS_STATUS_CONNECTION_ERROR, true);
			if (use_shutdown && k_uptime_get() - last_tx_success > CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_TIMEOUT_DELAY)) // shutdown if receiver is not detected // TODO: is shutdown necessary if usb is connected at the time?
			{
				LOG_WRN("No response from receiver in %dm", CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_TIMEOUT_DELAY) / 60000);
				sys_request_system_off(false);
			}
		}
#if CONFIG_WIRED_LINK
		if (esb_paired && tx_errors >= TX_ERROR_THRESHOLD && k_uptime_get() - last_repair_try > REPAIR_RETRY_MS)
		{
			last_repair_try = k_uptime_get();
			esb_try_repair();
		}
#endif
		if (tx_errors < TX_ERROR_THRESHOLD && get_status(SYS_STATUS_CONNECTION_ERROR) && k_uptime_get() - last_tx_fail > 3000) // TODO: there is possibly some race condition causing tx_error to potentially be above zero more often than not, so the check is more lenient; tx_error under threshold and last errors above threshold was not recent
		{
			set_status(SYS_STATUS_CONNECTION_ERROR, false);
		}
		k_msleep(100);
	}
}
