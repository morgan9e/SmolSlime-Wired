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
#include "hid.h"
/* The old allocator in tdma.h is not a running scheduler. */
#include "packet_log.h"
#include "ota.h"
#include "ota_proto.h"

#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/crc.h>

#include "esb.h"

static struct esb_payload rx_payload;
//static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0,
//														  0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_pair = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0, 0, 0, 0, 0);
//static struct esb_payload tx_payload_timer = ESB_CREATE_PAYLOAD(0,
//														  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_sync = ESB_CREATE_PAYLOAD(0,
														  0, 0, 0, 0);

uint8_t pairing_buf[8] = {0};
static uint8_t discovered_trackers[MAX_TRACKERS] = {0};
uint8_t sequences[256] = {0};
int64_t last_seq_time[256] = {[0 ... 255] = -1000};
uint16_t packets_count[256] = {0};
uint8_t packets_lost[256] = {0};

LOG_MODULE_REGISTER(esb_event, LOG_LEVEL_INF);

static void esb_packet_filter_thread(void);
K_THREAD_DEFINE(esb_packet_filter_thread_id, 256, esb_packet_filter_thread, NULL, NULL, NULL, 6, 0, 0);

#if CONFIG_RADIO_TDMA
#include "radio_tdma.h"
#include "radio_control.h"
#include "radio_test.h"
#if CONFIG_RADIO_TDMA_HW
#include "tdma_hw.h"
#endif
#include <zephyr/random/random.h>
BUILD_ASSERT(sizeof(((struct esb_payload *)0)->data) >= TDMA_BEACON_LEN + RC_LEN,
	"Radio control needs a 24-byte beacon payload");
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC >= 32768, "TDMA requires a fine system clock");
static bool tdma_running, tdma_tx_mode, tdma_inflight;
static atomic_t tdma_done;
static int64_t tdma_next, tdma_submitted;
static uint32_t tdma_serial, tdma_beacons, tdma_errors, tdma_late, tdma_max_late;
static int64_t tdma_epoch;
static bool tdma_epoch_valid;
static struct k_spinlock control_lock;
static struct rc_presence radio_presence;
static uint8_t control_paired_mask;
static bool ota_radio_paused;
static uint8_t active_radio_mode = RC_TDMA;
static struct rc_receiver radio_control = {
	.message = {.mode=RC_TDMA, .target=RC_TDMA},
};
static void radio_queue_ack(void);
static struct tdma_rx_stats {
	uint32_t count, outside;
	int32_t last_us, min_us, max_us;
} tdma_rx[TDMA_SLOTS];

static struct radio_test_stats {
	uint32_t generated, skipped, late, submitted, replaced, acked, failed, errors, hz;
	int64_t heard;
	uint8_t flags, kinds;
} radio_test_rx[TDMA_SLOTS];

static void radio_test_receive(const uint8_t *p)
{
	struct radio_test_stats *s = &radio_test_rx[p[1]];
	uint32_t generated=rft_get(p+4,4), a=rft_get(p+8,4), b=rft_get(p+12,3);
	if (generated < s->generated) memset(s,0,sizeof(*s)); /* reboot */
	s->generated=generated; s->flags=p[3]; s->heard=k_uptime_get(); s->kinds|=1u<<(p[3]&7);
	switch(p[3]&7) {
	case 1: s->skipped=a; s->late=b; break;
	case 2: s->submitted=a; s->replaced=b; break;
	case 3: s->acked=a; s->failed=b; break;
	case 4: s->errors=a; s->hz=b; break;
	default: break;
	}
}

static int64_t tdma_now(void)
{
#if CONFIG_RADIO_TDMA_HW
	return tdma_hw_now();
#else
	return k_ticks_to_us_floor64(k_uptime_ticks());
#endif
}
#endif

void esb_tdma_status(void)
{
#if CONFIG_RADIO_TDMA
	printk("tdma: %s mode=%s slots=8 frame_us=10000 beacons=%u errors=%u late=%u max_late_us=%u\n",
		tdma_running ? "running" : "stopped", active_radio_mode == RC_TDMA ? "tdma" : "legacy",
		tdma_beacons, tdma_errors, tdma_late, tdma_max_late);
#if CONFIG_RADIO_TDMA_HW
	tdma_hw_status();
#endif
	for (unsigned i = 0; i < TDMA_SLOTS; i++) {
		unsigned key = irq_lock();
		struct tdma_rx_stats r = tdma_rx[i];
		struct radio_test_stats t = radio_test_rx[i];
		irq_unlock(key);
		if (r.count)
			printk("tdma id %u: rx=%u outside=%u phase_us last=%d min=%d max=%d\n",
				i, r.count, r.outside, r.last_us, r.min_us, r.max_us);
		if (t.kinds)
			printk("rftest id %u: generated=%u skipped=%u late=%u submitted=%u replaced=%u acked=%u failed=%u errors=%u hz=%u confirmed=%u imu=%u kinds=%02x age_ms=%lld\n",
				i,t.generated,t.skipped,t.late,t.submitted,t.replaced,t.acked,t.failed,t.errors,t.hz,
				!!(t.flags&RFT_CONFIRMED),!!(t.flags&RFT_IMU_WORKLOAD),t.kinds,k_uptime_get()-t.heard);
	}
#else
	printk("tdma: disabled (legacy ESB)\n");
#endif
}

bool esb_radio_update_allowed(void)
{
#if CONFIG_RADIO_TDMA
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	bool allowed = radio_control.message.phase == RC_IDLE;
	k_spin_unlock(&control_lock, key);
	return allowed;
#else
	return true;
#endif
}

void esb_radio_console(size_t argc, char **argv)
{
#if CONFIG_RADIO_TDMA
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	bool accepted = false, valid = true;
	uint8_t paired = (uint8_t)((1u << MIN(stored_trackers, TDMA_SLOTS))-1);
	uint8_t active = rc_active(&radio_presence, paired, k_uptime_get());
	if (argc == 3 && strcmp(argv[1], "mode") == 0 &&
	    (!strcmp(argv[2], "tdma") || !strcmp(argv[2], "legacy"))) {
		if (!ota_busy() && tdma_running && stored_trackers > 0 && stored_trackers <= TDMA_SLOTS)
			accepted = rc_begin(&radio_control, !strcmp(argv[2], "tdma") ? RC_TDMA : RC_LEGACY,
				active, k_uptime_get());
		if (accepted) control_paired_mask = paired;
	} else if (argc == 2 && !strcmp(argv[1], "cancel")) {
		rc_cancel(&radio_control);
	} else if (!(argc == 1 || (argc == 2 && !strcmp(argv[1], "status")))) valid = false;
	struct rc_receiver r = radio_control;
	k_spin_unlock(&control_lock, key);
	if (!valid) { printk("Usage: radio [status|cancel|mode tdma|mode legacy]\n"); return; }
	if (argc == 3) printk(accepted ? "Radio change queued; waiting for active tracker snapshot\n" :
		"Radio change not queued: already in mode, busy (including OTA), no active trackers, or unsupported paired count\n");
	printk("radio presence: paired=%02x active=%02x offline=%02x window_ms=%u\n",
		paired, active, paired & ~active, RC_ACTIVE_MS);
	uint8_t missing = r.message.required & ~(r.message.phase == RC_PREPARE || r.result == RC_MISSING ? r.ready : r.arrived);
	static const char *phases[] = {"idle", "preparing", "checking"};
	static const char *results[] = {"none", "success", "missing-trackers", "rolled-back", "cancelled"};
	printk("radio: mode=%s target=%s phase=%s token=%u required=%02x ready=%02x arrived=%02x missing=%02x result=%s\n",
		r.message.mode == RC_TDMA ? "tdma" : "legacy", r.message.target == RC_TDMA ? "tdma" : "legacy",
		phases[r.message.phase], r.message.token, r.message.required, r.ready, r.arrived, missing, results[r.result]);
	if (missing) {
		printk("Missing required tracker IDs:");
		for (unsigned id=0; id<TDMA_SLOTS; id++) if (missing & BIT(id)) printk(" %u", id);
		printk("\n");
	}
#else
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	printk("Radio mode control requires a TDMA/control build\n");
#endif
}

static void esb_thread(void);
K_THREAD_DEFINE(esb_thread_id, 1024, esb_thread, NULL, NULL, NULL,
	IS_ENABLED(CONFIG_RADIO_TDMA) ? 4 : 6, 0, 0);

static void esb_parse_pair(void);

//|type    |description
//|RX  CRC8|pairing
//|TX  CRC8|pairing

//|b0      |b1      |b2      |b3      |b4      |b5      |b6      |b7      |b8      |b9      |b10     |b11     |b12     |b13     |b14     |b15     |
//|type    |data                                                                                                                                  |
//|RX  CRC8|ack     |device_addr                                          |-
//|TX  CRC8|ack     |recv_addr                                            |-

// TDMA to implement

//|packet  |description
//|RX     1|request from tracker
//|TX     2|pairing accepted from dongle
//|TX     3|Dongle State
//|TX     4|No Windows
//|TX     5|Window Info

//|packet  |b0      |b1      |b2      |b3      |b4      |b5      |b6      |b7      |b8      |b9      |b10     |b11     |b12     |b13     |b14     |b15     |
//|RX     1|    0xCD|    0x01|    0x00|Tracker Hardware ID                                  |Tracker Hardware ID                                  |-
//|TX     2|    0xCD|    0x02|Trckr ID|Dongle Hardware ID                                   |Tracker Hardware ID                                  |-
//|TX     3|    0xCD|    0x03|Dongle Hardware ID                                   |state   |channel |-
//|TX     4|    0xCD|    0x04|-
//|TX     5|    0xCD|    0x05|Window  |Timer                              |Packet  |-

//packet 3:
//state field bits: 9[0:0]: Accepts new trackers?; 9[1:1]: Force pair
//channel bundle field bits: 10[0:3]: Channels bundle; 10[4:7]: Next channel offset
//packet 5:
//Packet: Packet Number

void event_handler(struct esb_evt const *event)
{
	switch (event->evt_id)
	{
	case ESB_EVENT_TX_SUCCESS:
#if CONFIG_RADIO_TDMA
		if (tdma_inflight) {
			tdma_beacons++;
			tdma_epoch =
#if CONFIG_RADIO_TDMA_HW
				tdma_hw_tx_time();
#else
				tdma_now();
#endif
			tdma_epoch_valid = true;
			atomic_set(&tdma_done, 1);
		}
#endif
		LOG_DBG("TX SUCCESS");
		break;
	case ESB_EVENT_TX_FAILED:
#if CONFIG_RADIO_TDMA
		if (tdma_inflight) {
			tdma_errors++;
			atomic_set(&tdma_done, 1);
		}
#endif
		LOG_DBG("TX FAILED");
		break;
	case ESB_EVENT_RX_RECEIVED:
		LOG_DBG("RX");
	// TODO: make tx payload for ack here
		int err = 0;
		while (!err) // zero, rx success
		{
			err = esb_read_rx_payload(&rx_payload);
			if (err == -ENODATA)
			{
				return;
			}
			else if (err)
			{
				LOG_ERR("Error while reading rx packet: %d", err);
				return;
			}
			// TODO: split into separate handlers
			switch (rx_payload.pipe)
			{
			case 0: // base address 0 (pairing address)
				if (rx_payload.length != 8)
				{
					LOG_ERR("Wrong packet length: %d", rx_payload.length);
					continue;
				}
				LOG_DBG("rx: %16llX", *(uint64_t *)rx_payload.data);
				memcpy(pairing_buf, rx_payload.data, 8);
				switch (pairing_buf[1])
				{
				case 1: // receives ack generated from last packet
					LOG_DBG("RX Pairing Sent ACK");
					break;
				case 2: // should "acknowledge" pairing data sent from receiver
					LOG_DBG("RX Pairing ACK Receiver");
					break;
				default: // first packet in pairing burst
					LOG_INF("RX Pairing Request");
					break;
				}
				continue;
			default: // base address 1
			}
			if (rx_payload.pipe == OTA_PIPE) // a tracker receiving a firmware update
			{
				ota_esb_rx(&rx_payload);
				continue;
			}
#if CONFIG_RADIO_TDMA
			if (rx_payload.pipe == 1) {
				bool control = rx_payload.length == RC_LEN;
				if (control) {
					k_spinlock_key_t key = k_spin_lock(&control_lock);
					rc_ack(&radio_control, rx_payload.data, rx_payload.length);
					k_spin_unlock(&control_lock, key);
				}
				radio_queue_ack(); /* payload is for the next ACK on pipe 1 */
				if (control) continue; /* never forward control replies to SlimeVR */
			}
#endif
#if !CONFIG_RADIO_TDMA
			ota_esb_tracker_frame(); // control builds queue offers in radio_queue_ack
#endif
			switch (rx_payload.length)
			{
			case 21: // has sequence number
#if CONFIG_RADIO_TDMA
				if (radio_control.message.mode == RC_TDMA && tdma_epoch_valid && rx_payload.data[1] < TDMA_SLOTS) {
					unsigned id = rx_payload.data[1];
					int64_t received =
#if CONFIG_RADIO_TDMA_HW
						tdma_hw_rx_time();
#else
						tdma_now();
#endif
					int64_t phase = received - tdma_epoch - TDMA_FIRST_SLOT_US - id * TDMA_SLOT_US;
					struct tdma_rx_stats *r = &tdma_rx[id];
					/* Callback timing after the ACK, not a hardware RX-END capture.
					 * An out-of-slot count may indicate legacy TX or scheduling jitter. */
					int32_t p = (int32_t)CLAMP(phase, INT32_MIN, INT32_MAX);
					if (!r->count || p < r->min_us) r->min_us = p;
					if (!r->count || p > r->max_us) r->max_us = p;
					r->last_us = p;
					r->count++;
					if (phase < 0 || phase >= TDMA_SLOT_US) r->outside++;
				}
#endif
				packet_log_frame(rx_payload.data, rx_payload.rssi); // link diagnostics, before any filter
				// TODO : It's a very crude implementation
				// But brain hurty, will make a better one later
				uint8_t seq = rx_payload.data[20];
				uint8_t tracker_id = rx_payload.data[1];
				uint8_t next = sequences[tracker_id] + 1; // wrap
				if(seq != 0 && sequences[tracker_id] != 0 && next != seq) {
					if (k_uptime_get() - last_seq_time[tracker_id] < 100 && ((next < 128) // reset sequence if last packet was over 100ms old
						? ((seq < next) || (seq >= next + 128)) // next 0-127: seq is below next or above or equal to next +128
						: ((seq < next) && (seq >= next - 128)))) // next 128-255: seq is below next and above or equal to next -128
					{
						LOG_WRN("Sequence missmatch for tracker %d, expected %d, got %d. Discarding.", tracker_id, next, seq);
						// a repeat of the last frame: the tracker missed the ACK and sent it again
						packet_log_drop(tracker_id, seq == sequences[tracker_id] ? PACKET_DROP_DUPLICATE : PACKET_DROP_SEQUENCE);
						break;
					}
				}
				sequences[tracker_id] = seq;
				last_seq_time[tracker_id] = k_uptime_get();
				// Fall-throught
			case 20: // has crc32
				uint32_t crc_check = crc32_k_4_2_update(0x93a409eb, rx_payload.data, 16);
				uint32_t *crc_ptr = (uint32_t *)&rx_payload.data[16];
				if (*crc_ptr != crc_check)
				{
					LOG_ERR("Incorrect checksum, computed %08X, received %08X", crc_check, *crc_ptr);
					packet_log_drop(rx_payload.data[1], PACKET_DROP_CHECKSUM);
					printk("%08llx%016llX%016llX\n", *(uint64_t *)&rx_payload.data[16] & 0XFFFFFFFF, *(uint64_t *)&rx_payload.data[8], *(uint64_t *)rx_payload.data);
					break;
				}
				// Fall-throught
			case 16:
				uint8_t imu_id = rx_payload.data[1];
				if (imu_id >= stored_trackers) // not a stored tracker
				{
					packet_log_drop(imu_id, PACKET_DROP_UNKNOWN_ID);
					continue;
				}
				if (discovered_trackers[imu_id] < DETECTION_THRESHOLD) // garbage filtering of nonexistent tracker
				{
					discovered_trackers[imu_id]++;
					continue;
				}
#if CONFIG_RADIO_TDMA
				if (rx_payload.length == 21 && rft_valid(rx_payload.data)) {
					k_spinlock_key_t key = k_spin_lock(&control_lock);
					rc_seen(&radio_presence, imu_id, k_uptime_get());
					radio_test_receive(rx_payload.data);
					k_spin_unlock(&control_lock, key);
					break; /* diagnostics only: never put test data in SlimeVR HID */
				}
#endif
				if (rx_payload.data[0] > 223) // reserved for receiver only
					break;
#if CONFIG_RADIO_TDMA
				if (rx_payload.pipe == 1) {
					k_spinlock_key_t key = k_spin_lock(&control_lock);
					rc_seen(&radio_presence, imu_id, k_uptime_get());
					k_spin_unlock(&control_lock, key);
				}
#endif
				hid_write_packet_n(rx_payload.data, rx_payload.rssi); // write to hid endpoint
				break;
			default:
				LOG_ERR("Wrong packet length: %d", rx_payload.length);
				break;
			}
		}
		break;
	}
}

int clocks_start(void)
{
	int err;
	int res;
	struct onoff_manager *clk_mgr;
	struct onoff_client clk_cli;
	int fetch_attempts = 0;

	clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (!clk_mgr)
	{
		LOG_ERR("Unable to get the Clock manager");
		return -ENXIO;
	}

	sys_notify_init_spinwait(&clk_cli.notify);

	err = onoff_request(clk_mgr, &clk_cli);
	if (err < 0)
	{
		LOG_ERR("Clock request failed: %d", err);
		return err;
	}

	do
	{
		err = sys_notify_fetch_result(&clk_cli.notify, &res);
		if (!err && res)
		{
			LOG_ERR("Clock could not be started: %d", res);
			return res;
		}
		if (err && ++fetch_attempts > 10000) {
			LOG_WRN("Unable to fetch Clock request result: %d", err);
			return err;
		}
	} while (err);

	LOG_DBG("HF clock started");
	return 0;
}

// this was randomly generated
// TODO: I have no idea?
static const uint8_t discovery_base_addr_0[4] = {0x62, 0x39, 0x8A, 0xF2};
static const uint8_t discovery_base_addr_1[4] = {0x28, 0xFF, 0x50, 0xB8}; // TODO: not used
static const uint8_t discovery_addr_prefix[8] = {0xFE, 0xFF, 0x29, 0x27, 0x09, 0x02, 0xB2, 0xD6};

static uint8_t base_addr_0[4], base_addr_1[4], addr_prefix[8] = {0};

static bool esb_initialized = false;

int esb_initialize(bool tx)
{
	if (esb_initialized)
		LOG_WRN("ESB already initialized");
	int err;

	struct esb_config config = ESB_DEFAULT_CONFIG;

	if (tx)
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		// config.mode = ESB_MODE_PTX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = 30;
		// config.retransmit_delay = 600;
		config.retransmit_count = 0;
		config.tx_mode = ESB_TXMODE_MANUAL;
		// config.payload_length = 32;
		config.selective_auto_ack = true;
//		config.use_fast_ramp_up = true;
	}
	else
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		config.mode = ESB_MODE_PRX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = 30;
		// config.retransmit_delay = 600;
		// config.retransmit_count = 3;
		// config.tx_mode = ESB_TXMODE_AUTO;
		// config.payload_length = 32;
		config.selective_auto_ack = true;
//		config.use_fast_ramp_up = true;
	}

#if CONFIG_RADIO_TDMA
	config.bitrate = ESB_BITRATE_2MBPS;
	config.tx_output_power = 8; /* nRF52840 supported maximum */
#else
	LOG_INF("Initializing ESB, %sX mode", tx ? "T" : "R");
#endif
	err = esb_init(&config);

	if (!err)
		err = esb_set_base_address_0(base_addr_0);

	if (!err)
		err = esb_set_base_address_1(base_addr_1);

	if (!err)
		err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));

#if CONFIG_RADIO_TDMA
	if (!err && !tx)
		err = esb_enable_pipes(BIT(0) | BIT(1) | BIT(OTA_PIPE));
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

static void esb_deinitialize(void)
{
#if CONFIG_RADIO_TDMA_HW
	tdma_hw_cancel();
#endif
#if CONFIG_RADIO_TDMA
	tdma_running = false;
#endif
	LOG_INF("ESB deinitialize requested");
	if (esb_initialized)
	{
		esb_initialized = false;
		LOG_INF("Deinitializing ESB");
		k_msleep(10); // wait for pending transmissions
		if (esb_initialized)
		{
			LOG_INF("ESB denitialize cancelled");
			return;
		}
		esb_disable();
	}
	esb_initialized = false;
}

// TODO: not used
inline void esb_set_addr_discovery(void)
{
	memcpy(base_addr_0, discovery_base_addr_0, sizeof(base_addr_0));
	memcpy(base_addr_1, discovery_base_addr_1, sizeof(base_addr_1));
	memcpy(addr_prefix, discovery_addr_prefix, sizeof(addr_prefix));
}

inline void esb_set_addr_paired(void)
{
	// Generate addresses from device address
	uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet)
	uint8_t buf[6] = {0};
	memcpy(buf, addr, 6);
	uint8_t addr_buffer[16] = {0};
	for (int i = 0; i < 4; i++)
	{
		addr_buffer[i] = buf[i];
		addr_buffer[i + 4] = buf[i] + buf[4];
	}
	for (int i = 0; i < 8; i++)
		addr_buffer[i + 8] = buf[5] + i;
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

static bool esb_pairing = false;
static bool esb_paired = false;

void esb_add_pair(uint64_t addr, bool checksum)
{
	int id = stored_trackers;
	if (checksum)
	{
		for (int i = 0; i < stored_trackers; i++) // Check if the device is already stored
		{
			if (addr != 0 && stored_tracker_addr[i] == addr)
			{
				id = i;
			}
		}
	}
	if (IS_ENABLED(CONFIG_RADIO_TDMA) && id >= 8) {
		LOG_ERR("TDMA supports only tracker IDs 0-7");
		return;
	}
	if (id == stored_trackers)
	{
		LOG_INF("Added device on id %d with address %012llX", id, addr);
		stored_tracker_addr[id] = addr;
		sys_write(STORED_ADDR_0 + id, NULL, &stored_tracker_addr[id], sizeof(stored_tracker_addr[0]));
		stored_trackers++;
		sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
	}
	else
	{
		LOG_INF("Device already stored with id %d", id);
	}
	if (checksum)
	{
		uint8_t buf[6] = {0};
		memcpy(buf, &addr, 6);
		uint8_t checksum = crc8_ccitt(0x07, buf, 6);
		if (checksum == 0)
			checksum = 8;
		uint64_t *receiver_addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet
		addr = (*receiver_addr & 0xFFFFFFFFFFFF) << 16;
		addr |= checksum; // Add checksum to the address
		addr |= (uint64_t)id << 8; // Add tracker id to the address
		LOG_INF("Pair the device with %016llX", addr);
	}
}

void esb_pop_pair(void)
{
	if (stored_trackers > 0)
	{
		stored_trackers--;
		sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
		LOG_INF("Removed device on id %d with address %012llX", stored_trackers, stored_tracker_addr[stored_trackers]);
	}
	else
	{
		LOG_WRN("No devices to remove");
	}
}

void esb_parse_pair()
{
	uint64_t found_addr = (*(uint64_t *)pairing_buf >> 16) & 0xFFFFFFFFFFFF;
	uint16_t send_tracker_id = stored_trackers; // Use new tracker id
	for (int i = 0; i < stored_trackers; i++) // Check if the device is already stored
	{
		if (found_addr != 0 && stored_tracker_addr[i] == found_addr)
		{
			//LOG_INF("Found device linked to id %d with address %012llX", i, found_addr);
			send_tracker_id = i;
		}
	}
	if (IS_ENABLED(CONFIG_RADIO_TDMA) && send_tracker_id >= 8) {
		tx_payload_pair.data[0] = 0;
		return;
	}
	uint8_t checksum = crc8_ccitt(0x07, &pairing_buf[2], 6); // make sure the packet is valid
	if (checksum == 0)
		checksum = 8;
	if (checksum == pairing_buf[0] && found_addr != 0 && send_tracker_id == stored_trackers && stored_trackers < MAX_TRACKERS) // New device, add to NVS
	{
		esb_add_pair(found_addr, false);
		set_led(SYS_LED_PATTERN_ONESHOT_PROGRESS, SYS_LED_PRIORITY_HIGHEST);
	}
	if (checksum == pairing_buf[0] && send_tracker_id < MAX_TRACKERS) // Make sure the dongle is not full
		tx_payload_pair.data[0] = pairing_buf[0]; // Use checksum sent from device to make sure packet is for that device
	else
		tx_payload_pair.data[0] = 0; // Invalidate packet
	tx_payload_pair.data[1] = send_tracker_id; // Add tracker id to packet
}

void esb_pair(void)
{
	LOG_INF("Pairing");
	esb_set_addr_paired();
	esb_initialize(false);
	esb_start_rx();
	tx_payload_pair.pipe = 0;
	tx_payload_pair.noack = false;
	uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet)
	memcpy(&tx_payload_pair.data[2], addr, 6);
	LOG_INF("Device address: %012llX", *addr & 0xFFFFFFFFFFFF);
	set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_CONNECTION);
	esb_pairing = true;
	pairing_buf[1] = 255; // initialize packet flag
	while (esb_pairing)
	{
		if (!esb_initialized)
		{
			esb_initialize(false);
			esb_start_rx();
		}
		switch (pairing_buf[1])
		{
		case 0: // first packet in pairing burst
			esb_parse_pair();
			LOG_DBG("tx: %16llX", *(uint64_t *)tx_payload_pair.data);
//			esb_flush_tx();
			esb_write_payload(&tx_payload_pair); // Add to TX buffer
			pairing_buf[1] = 255; // flag packet processed
			k_msleep(10);
			esb_flush_tx(); // Flush TX buffer for next pairing burst
			continue;
		case 2:
			esb_flush_tx(); // Flush TX buffer for next pairing burst
		case 255:
		default:
			break;
		}
		pairing_buf[1] = 255; // flag packet processed
		//esb_flush_rx();
		//esb_flush_tx();
		//esb_write_payload(&tx_payload_pair); // Add to TX buffer
		k_usleep(1);
	}
	set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_CONNECTION);
	esb_deinitialize();
}

void esb_reset_pair(void)
{
	esb_deinitialize(); // make sure esb is off
	esb_paired = false;
}

void esb_finish_pair(void)
{
	esb_pairing = false;
}

void esb_clear(void)
{
	stored_trackers = 0;
	sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
	LOG_INF("NVS Reset");
	esb_reset_pair();
}

// TODO:
void esb_write_sync(uint16_t led_clock)
{
	if (!esb_initialized || !esb_paired)
		return;
	tx_payload_sync.noack = false;
	tx_payload_sync.data[0] = (led_clock >> 8) & 255;
	tx_payload_sync.data[1] = led_clock & 255;
	esb_write_payload(&tx_payload_sync);
}

// TODO:
void esb_receive(void)
{
	esb_set_addr_paired();
	esb_paired = true;
}

static void esb_packet_filter_thread(void)
{
	memset(discovered_trackers, 0, sizeof(discovered_trackers));
	while (1) // reset count if its not above threshold
	{
		k_msleep(1000);
		for (int i = 0; i < MAX_TRACKERS; i++)
			if (discovered_trackers[i] < DETECTION_THRESHOLD)
				discovered_trackers[i] = 0;
	}
}

#if CONFIG_RADIO_TDMA
static void radio_queue_ack(void)
{
	if (!esb_initialized || tdma_tx_mode) return;
	if (ota_sending()) return; /* never flush pending chunk ACKs on the OTA pipe */
	if (ota_busy()) {
		esb_flush_tx();
		ota_esb_tracker_frame(); /* offer replaces the normal mode-control ACK */
		return;
	}
	struct esb_payload p = {.pipe=1, .length=RC_LEN, .noack=false};
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	rc_encode(p.data, &radio_control.message);
	k_spin_unlock(&control_lock, key);
	esb_flush_tx();
	esb_write_payload(&p);
}

static int tdma_mode(bool tx)
{
#if CONFIG_RADIO_TDMA_HW
	tdma_hw_cancel();
#endif
	if (esb_initialized) esb_disable();
	esb_initialized = false;
	tdma_tx_mode = tx;
	int err = esb_initialize(tx);
	if (!err && !tx) radio_queue_ack();
	if (!err && !tx) err = esb_start_rx();
	if (err) {
		tdma_errors++;
		esb_initialized = false;
	}
	return err;
}

static void tdma_start(void)
{
#if CONFIG_RADIO_TDMA_HW
	if (tdma_hw_init()) {
		tdma_running = false;
		LOG_ERR("TDMA hardware timer initialization failed");
		return;
	}
#endif
	/* Reboot starts in TDMA; a fresh token prevents stale transaction replies. */
	uint32_t token = sys_rand32_get();
	k_spinlock_key_t key = k_spin_lock(&control_lock);
	radio_control = (struct rc_receiver){.message={.mode=RC_TDMA, .target=RC_TDMA,
		.token=token}};
	radio_presence = (struct rc_presence){0};
	k_spin_unlock(&control_lock, key);
	active_radio_mode = RC_TDMA;
	tdma_running = true;
	tdma_tx_mode = false;
	tdma_inflight = false;
	tdma_epoch_valid = false;
	tdma_next = tdma_now() + TDMA_FRAME_US;
	LOG_INF("TDMA v1: 8 slots / 10 ms, beacon pipe 2, OTA transfer pauses beacons");
	if (stored_trackers > TDMA_SLOTS)
		LOG_WRN("TDMA supports only tracker IDs 0-7");
}

static void tdma_service(void)
{
	if (!tdma_running || !esb_paired) return;
	int64_t now = tdma_now();
	if (ota_sending()) {
		tdma_epoch_valid = false;
		/* esb_flush_tx refuses an active PRX radio. Reset once in the worker
		 * to retire the offer occupying the single ACK slot. The target retries
		 * its request; thereafter preserve all queued chunks until completion. */
		if (!ota_radio_paused || tdma_tx_mode || !esb_initialized) {
			tdma_inflight = false;
			if (!tdma_mode(false)) ota_radio_paused = true;
		}
		return;
	}
	if (ota_radio_paused) {
		ota_radio_paused = false;
		tdma_inflight = false;
		tdma_mode(false);
		tdma_next = now + TDMA_FRAME_US;
		return;
	}
	k_spinlock_key_t control_key = k_spin_lock(&control_lock);
	if (radio_control.message.phase != RC_IDLE &&
	    control_paired_mask != (uint8_t)((1u << MIN(stored_trackers, TDMA_SLOTS))-1))
		rc_cancel(&radio_control); /* pairing table changed during the transaction */
	rc_tick(&radio_control, k_uptime_get());
	bool changed = active_radio_mode != radio_control.message.mode;
	struct rc_message control = radio_control.message;
	k_spin_unlock(&control_lock, control_key);
	if (changed) {
		active_radio_mode = control.mode;
		tdma_inflight = false;
		tdma_epoch_valid = false;
		tdma_mode(false);
		tdma_next = now + TDMA_FRAME_US;
		LOG_INF("Radio mode: %s", control.mode == RC_TDMA ? "tdma" : "legacy");
		return;
	}
	if (tdma_inflight) {
		if (!atomic_get(&tdma_done) && now - tdma_submitted < TDMA_TRANSACTION_US)
			return;
		if (!atomic_get(&tdma_done)) tdma_errors++;
		tdma_inflight = false;
		tdma_mode(false);
		return;
	}
	if (!esb_initialized) {
		tdma_mode(false);
		return;
	}
	if (now < tdma_next - TDMA_PREPARE_US) return;
	if (!tdma_tx_mode && tdma_mode(true)) return;
	now = tdma_now();
	if (!IS_ENABLED(CONFIG_RADIO_TDMA_HW) && now < tdma_next) return;
	struct esb_payload p = {.pipe = TDMA_BEACON_PIPE,
		.length = TDMA_BEACON_LEN + RC_LEN, .noack = true};
	tdma_make_beacon(p.data, tdma_serial++);
	rc_encode(p.data + TDMA_BEACON_LEN, &control);
	atomic_clear(&tdma_done);
	/* Manual PTX: queue first, then timestamp and start without preemption. */
	if (esb_write_payload(&p)) {
		tdma_errors++;
		tdma_next = now + TDMA_FRAME_US;
		tdma_mode(false);
		return;
	}
	unsigned key = irq_lock();
	tdma_submitted = IS_ENABLED(CONFIG_RADIO_TDMA_HW) ? tdma_next : tdma_now();
	tdma_inflight = true;
#if CONFIG_RADIO_TDMA_HW
	int err = tdma_hw_start_tx_at(tdma_submitted);
#else
	int err = esb_start_tx();
#endif
	irq_unlock(key);
	uint32_t late = (uint32_t)(tdma_submitted - tdma_next);
	if (late > TDMA_START_WINDOW_US) tdma_late++;
	if (late > tdma_max_late) tdma_max_late = late;
	tdma_next = control.mode == RC_TDMA ? tdma_next_beacon(tdma_next, tdma_submitted) :
		tdma_submitted + RC_LEGACY_BEACON_US;
	if (err) {
		if (err == -ETIME) tdma_late++;
		else tdma_errors++;
		tdma_next = tdma_now() + TDMA_FRAME_US;
		tdma_inflight = false;
		tdma_mode(false);
	}
}
#endif

static void esb_thread(void)
{
	clocks_start();

	sys_read(STORED_TRACKERS, &stored_trackers, sizeof(stored_trackers));
	if (stored_trackers)
		esb_paired = true;
	for (int i = 0; i < stored_trackers; i++)
		sys_read(STORED_ADDR_0 + i, &stored_tracker_addr[i], sizeof(stored_tracker_addr[0]));
	LOG_INF("%d/%d devices stored", stored_trackers, MAX_TRACKERS);

	if (esb_paired)
	{
		esb_receive();
		esb_initialize(false);
		esb_start_rx();
#if CONFIG_RADIO_TDMA
		tdma_start();
#endif
	}

	while (1)
	{
		if (!esb_paired)
		{
			esb_pair();
			esb_receive();
			esb_initialize(false);
			esb_start_rx();
#if CONFIG_RADIO_TDMA
			tdma_start();
#endif
		}
#if CONFIG_RADIO_TDMA
		tdma_service();
		k_usleep(100);
#else
		k_msleep(100);
#endif
	}
}
