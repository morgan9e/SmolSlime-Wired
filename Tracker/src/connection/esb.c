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
#include "ota.h"
#include "ota_proto.h"

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

static struct esb_payload rx_payload;
static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_pair = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0, 0, 0, 0, 0);

static uint8_t paired_addr[8] = {0};

static bool esb_initialized = false;
static bool esb_paired = false;

#if CONFIG_RADIO_TDMA
#include "radio_tdma.h"
#include "radio_control.h"
BUILD_ASSERT(sizeof(((struct esb_payload *)0)->data) >= TDMA_BEACON_LEN + RC_LEN,
	"Radio control needs a 24-byte beacon payload");
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC >= 32768, "TDMA requires a fine system clock");
BUILD_ASSERT(CONFIG_ESB_PIPE_COUNT > TDMA_BEACON_PIPE, "TDMA requires beacon pipe 2");
static struct tdma_sync tdma_sync;
static struct k_spinlock tdma_lock; /* IRQ beacon timestamp and pending frame */
static bool tdma_running, tdma_tx_mode, tdma_pending, tdma_inflight;
static uint8_t tdma_data[21], tdma_sequence;
static uint32_t tdma_beacons, tdma_sent, tdma_late, tdma_replaced, tdma_errors;
static uint32_t tdma_acked, tdma_failed, tdma_empty;
static int64_t tdma_started, tdma_submitted;
static bool tdma_outage;
static bool radio_tdma = true;
static struct rc_message radio_control = {.mode=RC_TDMA, .target=RC_TDMA};
static int64_t control_reply_due;
static uint32_t control_replies;
static atomic_t ota_radio_ready;

static int64_t tdma_now(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static void tdma_receive_beacon(const struct esb_payload *p)
{
	if (!tdma_running) return;
	k_spinlock_key_t key = k_spin_lock(&tdma_lock);
	struct rc_message m;
	if (p->pipe == 1 && rc_decode(&m, p->data, p->length)) {
		radio_control = m; /* receiver's authoritative mode, delivered in an ACK */
	} else if (!tdma_tx_mode && p->pipe == TDMA_BEACON_PIPE) {
		bool control = p->length == TDMA_BEACON_LEN + RC_LEN &&
			rc_decode(&m, p->data + TDMA_BEACON_LEN, RC_LEN);
		if ((control || p->length == TDMA_BEACON_LEN) &&
		    tdma_accept_beacon(&tdma_sync, p->data, TDMA_BEACON_LEN, tdma_now())) {
			tdma_beacons++;
			if (control) radio_control = m;
		}
	}
	k_spin_unlock(&tdma_lock, key);
}
#endif

void esb_tdma_status(void)
{
#if CONFIG_RADIO_TDMA
	k_spinlock_key_t key = k_spin_lock(&tdma_lock);
	bool locked = tdma_running && tdma_sync.valid &&
		tdma_now() - tdma_sync.epoch_us < 2 * TDMA_FRAME_US;
	uint32_t b = tdma_beacons, s = tdma_sent, l = tdma_late, r = tdma_replaced;
	uint32_t e = tdma_errors, a = tdma_acked, f = tdma_failed, n = tdma_empty;
	struct rc_message control = radio_control;
	k_spin_unlock(&tdma_lock, key);
	printk("radio: mode=%s control_phase=%u token=%u replies=%u\n",
		radio_tdma ? "tdma" : "legacy", control.phase, control.token, control_replies);
	printk("tdma: %s id=%u beacons=%u submitted=%u acked=%u failed=%u late=%u empty=%u replaced=%u errors=%u\n",
		paired_addr[1] >= TDMA_SLOTS ? "unsupported-id" : !radio_tdma ? "legacy" : locked ? "locked" : "listening",
		paired_addr[1], b, s, a, f, l, n, r, e);
#else
	printk("tdma: disabled (legacy ESB)\n");
#endif
}

#define TX_ERROR_THRESHOLD 30
#define TX_ERROR_MAX 100
#define TX_ERROR_CLEAR_RATE 10

LOG_MODULE_REGISTER(esb_event, LOG_LEVEL_INF);

static void esb_thread(void);
K_THREAD_DEFINE(esb_thread_id, 1024, esb_thread, NULL, NULL, NULL,
	IS_ENABLED(CONFIG_RADIO_TDMA) ? 4 : ESB_THREAD_PRIORITY, 0, 0);

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
#if CONFIG_RADIO_TDMA
		if (tdma_running && !ota_active()) tdma_acked++;
#endif
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
		LOG_DBG("TX SUCCESS");
		if (esb_paired)
			ota_tx_acked(); // a new image confirms itself once the receiver hears it
		ota_tx_done();
		if (esb_paired && !ota_active()) // an update keeps the radio clock on
			clocks_stop();
		break;
	case ESB_EVENT_TX_FAILED:
#if CONFIG_RADIO_TDMA
		if (tdma_running && !ota_active()) tdma_failed++;
#endif
		if (ota_active())
		{
			ota_tx_done();
			break; // an update asks again by itself; not a connection error
		}
		if (tx_errors < TX_ERROR_MAX)
			tx_errors++;
		if (tx_errors == TX_ERROR_THRESHOLD && last_tx_success == 0) // consecutive failure to transmit
		{
			last_tx_fail = 0; // reset last_tx_fail on threshold reached
			last_tx_success = k_uptime_get();
		}
		LOG_DBG("TX FAILED");
		if (esb_paired)
			clocks_stop();
		break;
	case ESB_EVENT_RX_RECEIVED:
		/* Drain ACKs/beacons so an old payload cannot acquire a new timestamp. */
		while (!esb_read_rx_payload(&rx_payload)) // zero, rx success
		{
			LOG_DBG("rx len: %d, pipe: %d", rx_payload.length, rx_payload.pipe);
			if (!paired_addr[0]) // zero, not paired
			{
				LOG_DBG("tx: %16llX rx: %16llX", *(uint64_t *)tx_payload_pair.data, *(uint64_t *)rx_payload.data);
				if (rx_payload.length == 8 && tx_payload_pair.data[1] == 1) // ack to second packet in pairing burst
					memcpy(paired_addr, rx_payload.data, sizeof(paired_addr));
			}
			else
			{
#if CONFIG_RADIO_TDMA
				tdma_receive_beacon(&rx_payload);
				if (rx_payload.pipe == 1 || rx_payload.pipe == OTA_PIPE)
					ota_rx_ack(rx_payload.data, rx_payload.length, paired_addr[1]);
				continue;
#endif
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
				else
				{
					ota_rx_ack(rx_payload.data, rx_payload.length, paired_addr[1]); // update offers and chunks
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
#if CONFIG_RADIO_TDMA
	if (tdma_running) return; /* listening for the next beacon requires HFXO */
#endif
	if (!clock_status)
		return;
	clock_status = false;

	onoff_release(clk_mgr);

	LOG_DBG("HF clock stop request");
}

void clocks_release_when_idle(void)
{
	// Stopping the HFXO under a frame that is still being sent or retransmitted moves the radio
	// onto the RC oscillator, off frequency: the receiver never hears it. The TX events stop the
	// clock when the queue is done.
	if (esb_initialized && !esb_is_idle())
		return;
	clocks_stop();
}

#else
BUILD_ASSERT(false, "No Clock Control driver");
#endif

static struct k_thread clocks_thread_id;
static K_THREAD_STACK_DEFINE(clocks_thread_id_stack, 128);

void clocks_request_start(uint32_t delay_us)
{
#if CONFIG_RADIO_TDMA
	if (tdma_running) return; /* the TDMA worker owns the continuous HF clock */
#endif
	k_thread_create(&clocks_thread_id, clocks_thread_id_stack, K_THREAD_STACK_SIZEOF(clocks_thread_id_stack), (k_thread_entry_t)clocks_start, NULL, NULL, NULL, CLOCKS_START_THREAD_PRIORITY, 0, K_USEC(delay_us));
}

static struct k_thread clocks_stop_thread_id;
static K_THREAD_STACK_DEFINE(clocks_stop_thread_id_stack, 128);

void clocks_request_stop(uint32_t delay_us)
{
#if CONFIG_RADIO_TDMA
	if (tdma_running) return; /* the TDMA worker owns the continuous HF clock */
#endif
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

	if (tx)
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		// config.mode = ESB_MODE_PTX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = CONFIG_2_SETTINGS_READ(CONFIG_2_RADIO_TX_POWER);
		// All trackers share one channel. With the same retransmit delay, two trackers whose frames
		// collide retry at the same moment and collide again on every attempt, so both lose the
		// frame. Give each tracker its own delay from its device address: 600 (stock) to 1475 us.
		uint32_t a = NRF_FICR->DEVICEADDR[0];
		config.retransmit_delay = 600 + ((a ^ (a >> 8) ^ (a >> 16) ^ (a >> 24)) & 7) * 125;
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

#if CONFIG_RADIO_TDMA
	if (esb_paired && radio_tdma && !ota_active()) {
		config.bitrate = ESB_BITRATE_2MBPS;
		config.retransmit_count = 0; /* never retry into a neighbour's slot */
	}
#endif
	err = esb_init(&config);

	if (!err)
		err = esb_set_base_address_0(base_addr_0);

	if (!err)
		err = esb_set_base_address_1(base_addr_1);

	if (!err)
		err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));

#if CONFIG_RADIO_TDMA
	if (!err && esb_paired && !tx)
		err = esb_enable_pipes(BIT(TDMA_BEACON_PIPE));
#endif
	if (err)
	{
		LOG_ERR("ESB initialization failed: %d", err);
		set_status(SYS_STATUS_CONNECTION_ERROR, true);
		return err;
	}

	esb_initialized = true;
	return 0;
}

void esb_deinitialize(void)
{
#if CONFIG_RADIO_TDMA
	tdma_running = false;
#endif
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

#if CONFIG_TRACKER_OTA
// A firmware update message on the OTA pipe (ota.c); tracking is paused meanwhile
int esb_write_ota(const uint8_t *data, uint8_t len)
{
#if CONFIG_RADIO_TDMA
	/* The radio worker installs unslotted PTX before the OTA thread may send.
	 * Never reconfigure from the ACK interrupt or race the tracking worker. */
	int64_t deadline = k_uptime_get() + 100;
	while (!atomic_get(&ota_radio_ready)) {
		if (!ota_active() || !tdma_running || k_uptime_get() >= deadline)
			return -EAGAIN;
		k_msleep(1);
	}
#endif
	static struct esb_payload ota_payload = ESB_CREATE_PAYLOAD(OTA_PIPE, 0);
	if (!esb_initialized || !esb_paired)
		return -ENOTCONN;
	if (!clock_status)
		clocks_start();
	ota_payload.pipe = OTA_PIPE;
	ota_payload.noack = false;
	ota_payload.length = MIN(len, sizeof(ota_payload.data));
	memcpy(ota_payload.data, data, ota_payload.length);
	esb_flush_tx();
	return esb_write_payload(&ota_payload);
}
#endif

#if CONFIG_RADIO_TDMA
/* Called only by the radio thread. Never reconfigure ESB in an IRQ or timer ISR. */
static int tdma_mode(bool tx)
{
	if (esb_initialized) esb_disable(); /* bounded stop, discards a failed frame */
	esb_initialized = false;
	tdma_tx_mode = tx;
	int err = esb_initialize(tx);
	if (!err && !tx)
		err = esb_start_rx();
	if (err) {
		tdma_errors++;
		esb_initialized = false;
	}
	return err;
}

static void tdma_service(void)
{
	if (!tdma_running || !esb_paired)
		return;
	if (ota_active()) {
		if (!atomic_get(&ota_radio_ready)) {
			tdma_inflight = false;
			k_spinlock_key_t key = k_spin_lock(&tdma_lock);
			tdma_sync.valid = false;
			tdma_pending = false;
			k_spin_unlock(&tdma_lock, key);
			if (!tdma_mode(true)) atomic_set(&ota_radio_ready, 1);
		}
		return; /* OTA thread exclusively owns PTX until it completes or aborts */
	}
	if (atomic_set(&ota_radio_ready, 0)) {
		tdma_started = tdma_now();
		tdma_outage = false;
		tdma_mode(!radio_tdma);
		return;
	}
	k_spinlock_key_t control_key = k_spin_lock(&tdma_lock);
	bool requested_tdma = radio_control.mode == RC_TDMA;
	k_spin_unlock(&tdma_lock, control_key);
	if (requested_tdma != radio_tdma) {
		/* All ESB reconfiguration stays in this worker, never the ACK IRQ. */
		radio_tdma = requested_tdma;
		tdma_inflight = false;
		control_key = k_spin_lock(&tdma_lock);
		tdma_sync.valid = false;
		k_spin_unlock(&tdma_lock, control_key);
		control_reply_due = 0;
		tx_errors = 0; last_tx_success = 0; last_tx_fail = 0;
		tdma_started = tdma_now(); tdma_outage = false;
		tdma_mode(!radio_tdma);
		LOG_INF("Radio mode: %s", radio_tdma ? "tdma" : "legacy");
		return;
	}
	if (!radio_tdma) {
		/* Unslotted PTX with the original per-device retries. New data still
		 * replaces pending data; one worker owns the radio and control replies. */
		if (!esb_initialized || !tdma_tx_mode) { tdma_mode(true); return; }
		if (!esb_is_idle()) return;
		control_key = k_spin_lock(&tdma_lock);
		bool reply = radio_control.phase != RC_IDLE && tdma_now() >= control_reply_due;
		if (paired_addr[1] < TDMA_SLOTS && (reply || tdma_pending)) {
			struct esb_payload p = {.pipe=1, .length=21, .noack=false};
			if (reply) {
				struct rc_message response = radio_control;
				response.mode = RC_LEGACY; /* acknowledge the mode actually installed */
				p.length = RC_LEN; rc_reply(p.data, &response, paired_addr[1]);
			} else {
				memcpy(p.data, tdma_data, sizeof(tdma_data)); p.data[20] = tdma_sequence;
			}
			esb_flush_tx();
			if (!esb_write_payload(&p)) {
				if (reply) { control_replies++; control_reply_due = tdma_now() + 50000; }
				else { tdma_pending = false; tdma_sequence++; tdma_sent++; }
			} else tdma_errors++;
		}
		k_spin_unlock(&tdma_lock, control_key);
		return;
	}
	k_spinlock_key_t sync_key = k_spin_lock(&tdma_lock);
	int64_t last_beacon = tdma_sync.valid ? tdma_sync.epoch_us : tdma_started;
	bool outage = tdma_now() - last_beacon > 1000000;
	k_spin_unlock(&tdma_lock, sync_key);
	if (outage && !tdma_outage) {
		LOG_WRN("TDMA: beacon absent for 1 s; listening, no unscheduled TX");
		tx_errors = TX_ERROR_THRESHOLD;
		last_tx_success = k_uptime_get();
	}
	tdma_outage = outage;
	if (!esb_initialized) {
		tdma_mode(false);
		return;
	}
	if (tdma_inflight) {
		if (!esb_is_idle()) {
			if (tdma_now() - tdma_submitted < TDMA_TRANSACTION_US)
				return;
			tdma_errors++; /* abort a stuck transaction and reacquire */
		}
		tdma_inflight = false;
		tdma_mode(false);
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&tdma_lock);
	enum tdma_action action = tdma_action_at(&tdma_sync, paired_addr[1], tdma_now());
	k_spin_unlock(&tdma_lock, key);
	if (action == TDMA_PREPARE || action == TDMA_SEND) {
		if (!tdma_tx_mode && tdma_mode(true))
			return;
		/* Recheck AFTER reconfiguration. Lock through submission so a thread
		 * preemption cannot turn an on-time decision into an out-of-slot TX. */
		key = k_spin_lock(&tdma_lock);
		action = tdma_action_at(&tdma_sync, paired_addr[1], tdma_now());
		if (action == TDMA_SEND) {
			tdma_sync.used = true;
			bool reply = radio_control.phase != RC_IDLE && tdma_now() >= control_reply_due;
			if (reply || tdma_pending) {
				struct esb_payload p = {.pipe = 1, .length = 21, .noack = false};
				memcpy(p.data, tdma_data, sizeof(tdma_data));
				/* CRC covers bytes 0..15, so changing byte 20 is safe. Count
				 * attempts, not sensor frames coalesced before their slot. */
				p.data[20] = tdma_sequence;
				if (reply) {
					struct rc_message response = radio_control;
					response.mode = RC_TDMA;
					p.length = RC_LEN; rc_reply(p.data, &response, paired_addr[1]);
				}
				tdma_submitted = tdma_now();
				if (esb_write_payload(&p) == 0) {
					if (reply) { control_replies++; control_reply_due = tdma_now() + 50000; }
					else { tdma_sequence++; tdma_sent++; tdma_pending = false; }
					tdma_inflight = true;
				} else {
					tdma_errors++;
				}
			} else {
				tdma_empty++;
			}
		}
		k_spin_unlock(&tdma_lock, key);
	}
	if (action == TDMA_EXPIRED) {
		key = k_spin_lock(&tdma_lock);
		/* A new beacon may have arrived since the first snapshot. */
		if (tdma_action_at(&tdma_sync, paired_addr[1], tdma_now()) == TDMA_EXPIRED) {
			tdma_sync.used = true;
			tdma_late++;
		}
		k_spin_unlock(&tdma_lock, key);
	}
	if (tdma_tx_mode && !tdma_inflight && action != TDMA_PREPARE)
		tdma_mode(false);
}

static void tdma_start(void)
{
	memset(&tdma_sync, 0, sizeof(tdma_sync));
	tdma_pending = false;
	tdma_inflight = false;
	tdma_started = tdma_now();
	tdma_outage = false;
	tdma_running = true;
	if (clocks_start()) {
		tdma_running = false;
		esb_paired = false; /* retry startup, never run the radio on HFINT */
		return;
	}
	tdma_mode(false);
	LOG_INF("TDMA v1: id %u, 8 slots / 10 ms, no tracking retries", paired_addr[1]);
	if (paired_addr[1] >= TDMA_SLOTS)
		LOG_ERR("TDMA supports IDs 0-7; this tracker will only listen");
}
#endif

void esb_write(uint8_t *data)
{
	if (!esb_initialized || !esb_paired)
		return;
	if (ota_active()) return;
#if CONFIG_RADIO_TDMA
	k_spinlock_key_t key = k_spin_lock(&tdma_lock);
	if (tdma_pending) tdma_replaced++;
	memcpy(tdma_data, data, sizeof(tdma_data));
	tdma_pending = true;
	k_spin_unlock(&tdma_lock, key);
	return;
#endif
	if (!clock_status)
		clocks_start();
	tx_payload.pipe = 1; // using base address 1
#if defined(NRF54L15_XXAA) // TODO: esb halts with ack and tx fail
	tx_payload.noack = true;
#else
	tx_payload.noack = false;
#endif
	memcpy(tx_payload.data, data, tx_payload.length);
	// The newest frame replaces one still being retransmitted: keeping the old ones (tried in 0.0.2)
	// lost more frames with six trackers on one channel, from repeats and blocked queues.
	esb_flush_tx(); // this will clear all transmissions even if they did not complete
	esb_write_payload(&tx_payload); // Add transmission to queue
	send_data = true;
}

bool esb_ready(void)
{
	return esb_initialized && esb_paired;
}

static void esb_thread(void)
{
	bool use_hid = CONFIG_0_SETTINGS_READ(CONFIG_0_CONNECTION_OVER_HID);
	bool use_shutdown = CONFIG_0_SETTINGS_READ(CONFIG_0_USER_SHUTDOWN);
	int64_t start_time = k_uptime_get();

	// Read paired address from retained
	memcpy(paired_addr, retained->paired_addr, sizeof(paired_addr));

	while (1)
	{
		if (!esb_paired && (!use_hid || paired_addr[0] || (!get_status(SYS_STATUS_USB_CONNECTED) && k_uptime_get() - 750 > start_time))) // only automatically enter pairing while not potentially communicating by usb, however allow esb if already paired
		{
			esb_pair();
#if CONFIG_RADIO_TDMA
			tdma_start();
#else
			esb_initialize(true);
#endif
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
		else if (tx_errors < TX_ERROR_THRESHOLD && get_status(SYS_STATUS_CONNECTION_ERROR) && k_uptime_get() - last_tx_fail > 3000) // TODO: there is possibly some race condition causing tx_error to potentially be above zero more often than not, so the check is more lenient; tx_error under threshold and last errors above threshold was not recent
		{
			set_status(SYS_STATUS_CONNECTION_ERROR, false);
		}
#if CONFIG_RADIO_TDMA
		tdma_service();
		k_usleep(100);
#else
		k_msleep(100);
#endif
	}
}
