/*
	BLE receiver: trackers in BLE mode (Tracker/src/connection/ble.c) to the PC over USB. It finds
	trackers by their service UUID, connects up to MAX_TRACKERS of them, subscribes to their data
	and settings notifications and passes both on to the PC (link.h):
	- ESP32-S3 (link_usb.c): as the nRF receiver does, over USB HID, so SlimeVR Server reads the
	  trackers directly. Each tracker gets a stable id (kept in NVS, like the receiver's pairing
	  list), in the order they first connected. The text lines below on its CDC console, without
	  the data lines unless asked (D 1).
	- ESP32-C6 (link_jtag.c): text lines on its USB Serial/JTAG port; tools/ble_bridge.py --serial
	  forwards them to SlimeVR.

	To the PC, one line each (slot: 0..MAX_TRACKERS-1, hex without separators):
		I SlimeBleReceiver <version> hid|serial at boot and on "?"
		C <slot> <mac>                         tracker ready: device address (12 hex, as `info`)
		D <slot> <34 hex>                      data notification: 16-byte packet and sequence
		F <slot> <20 hex>                      settings answer (CFG_RESP)
		X <slot> <reason hex>                  disconnected (BLE host error, 0x2xx = HCI code)
		E <slot|-> <what> <status>             a connection or setup step failed
		A <addr> <rssi> <name>                 a tracker seen while scanning (once per address)
		# ...                                  status, every 5 s, and other information
	From the PC:
		?                                      the I line, then C for each ready tracker
		W <slot> <20 hex>                      settings request (CFG_REQ) to that tracker
		K <slot>                               disconnect that tracker
		O <names>                              only these trackers (SlimeNRF- suffixes, comma
		                                       separated, e.g. O 0791,B8C8); "O" alone: all
		D 0|1                                  data lines off/on (default: on without HID)
		L                                      the tracker ids: "# id <id> <mac>" each
		Z                                      forget the tracker ids (as the receiver's clear)
		B                                      reboot into the ROM download mode (ESP32-S3: for
		                                       esptool, which cannot reset it there itself)
*/
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "link.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"

#define VERSION "2"
#define MAX_TRACKERS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define NAME_PREFIX "SlimeNRF-"
#define CFG_MSG_LEN 10
#define PACKET_LEN 17
// Connection interval (1.25 ms units, console P): 7.5 ms. The ESP32-S3 controller reserves 3.75 ms
// per connection within the interval (whatever the event length asked for): it took two trackers
// at 7.5 ms, four at 15 ms, then rejected the next (HCI 0x0D). There 30 ms, for eight trackers;
// two notifications per event then carry ~66 packets/s each, short of the 100 Hz moving rate
// (P 12: 15 ms for up to four at ~133/s).
#if CONFIG_IDF_TARGET_ESP32S3
#define CONN_INTERVAL_DEFAULT 24
#else
#define CONN_INTERVAL_DEFAULT 6
#endif
#define CONN_CE_LEN 0 // the controller decides
static uint16_t conn_interval = CONN_INTERVAL_DEFAULT;
#define CONN_TIMEOUT 100 // supervision timeout, 10 ms units: a lost tracker reconnects 1 s later

// 6e5c000N-7a3b-4c8e-9f1d-2b4a5c6d7e8f (Tracker/src/connection/ble.c), little endian
#define SLIME_UUID(n) BLE_UUID128_INIT(0x8f, 0x7e, 0x6d, 0x5c, 0x4a, 0x2b, 0x1d, 0x9f, 0x8e, 0x4c, 0x3b, 0x7a, n, 0x00, 0x5c, 0x6e)
static const ble_uuid128_t service_uuid = SLIME_UUID(1);
static const ble_uuid128_t data_uuid = SLIME_UUID(2);
static const ble_uuid128_t address_uuid = SLIME_UUID(3);
static const ble_uuid128_t config_uuid = SLIME_UUID(4);

enum { SLOT_FREE, SLOT_SETUP, SLOT_READY };

typedef struct {
	uint8_t state;
	uint16_t conn;
	ble_addr_t addr;
	uint16_t svc_start, svc_end;
	uint16_t data_handle, address_handle, config_handle; // value handles; each CCC follows its value
	uint8_t mac[6]; // device address as the tracker reads it (little endian)
	int id; // tracker id (ids[]), -1 when the table is full
	uint8_t rssi; // -dBm, as the receiver passes it on
	uint32_t packets, reported;
} slot_t;

// Tracker ids for the HID reports: device addresses in the order they first connected, kept in
// NVS (the receiver's pairing list does the same). 16 at most, as the receiver's address packets
// are sent round robin.
#define MAX_IDS 16
static uint8_t ids[MAX_IDS][6];
static int id_count;
static bool data_lines; // D lines; off with HID, SlimeVR reads the trackers there (app_main)

static slot_t slots[MAX_TRACKERS];
static SemaphoreHandle_t lock; // slots and the scan state, from the host task and the PC task
static uint8_t own_addr_type;
static bool connecting; // a connection attempt or a tracker setup runs; scanning waits for it
static TickType_t connect_started;
#define CONNECT_STUCK_MS 10000 // longer than a connection attempt (3 s) and a setup: give it up
static char only[64]; // O command: comma separated name suffixes, empty: all

// Addresses already reported in an A line
#define SEEN_MAX 32
static ble_addr_t seen[SEEN_MAX];
static int seen_count;
static uint32_t adv_reports; // every advertising report, for the status line

static void scan_resume(void);

// Output to the PC. Lines are dropped while nobody reads the port, so BLE never waits on USB.
static void out(const char *fmt, ...)
{
	char buf[400]; // the status line with 8 trackers
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	link_write(buf, n);
}

static void hex(char *dst, const uint8_t *src, int len)
{
	static const char digits[] = "0123456789ABCDEF";
	for (int i = 0; i < len; i++)
	{
		dst[2 * i] = digits[src[i] >> 4];
		dst[2 * i + 1] = digits[src[i] & 15];
	}
	dst[2 * len] = 0;
}

static int unhex(const char *src, uint8_t *dst, int len)
{
	for (int i = 0; i < len; i++)
	{
		unsigned v;
		if (!isxdigit((unsigned char)src[2 * i]) || !isxdigit((unsigned char)src[2 * i + 1]) || sscanf(src + 2 * i, "%2x", &v) != 1)
			return -1;
		dst[i] = v;
	}
	return 0;
}

static const char *addr_str(const ble_addr_t *a, char *buf)
{
	sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
	return buf;
}

static int slot_by_conn(uint16_t conn)
{
	for (int i = 0; i < MAX_TRACKERS; i++)
		if (slots[i].state != SLOT_FREE && slots[i].conn == conn)
			return i;
	return -1;
}

static bool addr_connected(const ble_addr_t *a)
{
	for (int i = 0; i < MAX_TRACKERS; i++)
		if (slots[i].state != SLOT_FREE && !ble_addr_cmp(&slots[i].addr, a))
			return true;
	return false;
}

static int free_slot(void)
{
	for (int i = 0; i < MAX_TRACKERS; i++)
		if (slots[i].state == SLOT_FREE)
			return i;
	return -1;
}

// The name suffix a tracker advertises: the last two bytes of its device address. The BLE address
// is the device address with its two top bits set (static random), so its low bytes match.
static bool wanted(const ble_addr_t *a)
{
	if (!only[0])
		return true;
	char suffix[5];
	sprintf(suffix, "%02X%02X", a->val[1], a->val[0]);
	for (const char *p = only; *p;)
	{
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len == 4 && !strncasecmp(p, suffix, 4))
			return true;
		p += len + (end ? 1 : 0);
	}
	return false;
}

// A step of the tracker setup failed: report it and drop the connection
static void setup_failed(int i, const char *what, int status)
{
	out("E %d %s %d", i, what, status);
	ble_gap_terminate(slots[i].conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void mac_str(const uint8_t *le, char *buf) // as `info` and the receiver's `list` print it
{
	uint8_t be[6];
	for (int k = 0; k < 6; k++)
		be[k] = le[5 - k];
	hex(buf, be, 6);
}

static void ids_save(void)
{
	nvs_handle_t nvs;
	if (nvs_open("slime", NVS_READWRITE, &nvs) == ESP_OK)
	{
		nvs_set_blob(nvs, "ids", ids, id_count * 6);
		nvs_commit(nvs);
		nvs_close(nvs);
	}
	link_hid_trackers(ids, id_count);
}

static void ids_load(void)
{
	nvs_handle_t nvs;
	size_t len = sizeof(ids);
	if (nvs_open("slime", NVS_READONLY, &nvs) == ESP_OK)
	{
		if (nvs_get_blob(nvs, "ids", ids, &len) == ESP_OK)
			id_count = len / 6;
		nvs_close(nvs);
	}
	link_hid_trackers(ids, id_count);
}

// The tracker's id, a new one for a new tracker; -1 when all are taken
static int tracker_id(const uint8_t *mac)
{
	for (int k = 0; k < id_count; k++)
		if (!memcmp(ids[k], mac, 6))
			return k;
	if (id_count == MAX_IDS)
		return -1;
	memcpy(ids[id_count++], mac, 6);
	ids_save();
	return id_count - 1;
}

static void setup_done(int i)
{
	char mac[13];
	mac_str(slots[i].mac, mac);
	slots[i].id = tracker_id(slots[i].mac);
	if (slots[i].id < 0)
		out("E %d no_tracker_id %d", i, MAX_IDS);
	slots[i].state = SLOT_READY;
	out("C %d %s", i, mac);
	xSemaphoreTake(lock, portMAX_DELAY);
	connecting = false;
	xSemaphoreGive(lock);
	scan_resume();
}

static int on_config_ccc(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
	int i = (int)(intptr_t)arg;
	if (error->status) // older firmware without settings: still a tracker
		out("# %d no settings notifications: %d", i, error->status);
	setup_done(i);
	return 0;
}

static int on_data_ccc(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
	int i = (int)(intptr_t)arg;
	if (error->status)
	{
		setup_failed(i, "subscribe", error->status);
		return 0;
	}
	if (!slots[i].config_handle)
	{
		setup_done(i);
		return 0;
	}
	static const uint8_t notify_on[2] = {1, 0};
	int rc = ble_gattc_write_flat(conn, slots[i].config_handle + 1, notify_on, sizeof(notify_on), on_config_ccc, arg);
	if (rc)
		setup_failed(i, "subscribe_config", rc);
	return 0;
}

static int on_address(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
	int i = (int)(intptr_t)arg;
	if (error->status)
	{
		setup_failed(i, "read_address", error->status);
		return 0;
	}
	if (OS_MBUF_PKTLEN(attr->om) != 6 || os_mbuf_copydata(attr->om, 0, 6, slots[i].mac))
	{
		setup_failed(i, "address_length", OS_MBUF_PKTLEN(attr->om));
		return 0;
	}
	static const uint8_t notify_on[2] = {1, 0};
	int rc = ble_gattc_write_flat(conn, slots[i].data_handle + 1, notify_on, sizeof(notify_on), on_data_ccc, arg);
	if (rc)
		setup_failed(i, "subscribe", rc);
	return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
	int i = (int)(intptr_t)arg;
	if (error->status == 0)
	{
		if (!ble_uuid_cmp(&chr->uuid.u, &data_uuid.u))
			slots[i].data_handle = chr->val_handle;
		else if (!ble_uuid_cmp(&chr->uuid.u, &address_uuid.u))
			slots[i].address_handle = chr->val_handle;
		else if (!ble_uuid_cmp(&chr->uuid.u, &config_uuid.u))
			slots[i].config_handle = chr->val_handle;
		return 0;
	}
	if (error->status != BLE_HS_EDONE)
	{
		setup_failed(i, "characteristics", error->status);
		return 0;
	}
	if (!slots[i].data_handle || !slots[i].address_handle)
	{
		setup_failed(i, "characteristics_missing", 0);
		return 0;
	}
	int rc = ble_gattc_read(conn, slots[i].address_handle, on_address, arg);
	if (rc)
		setup_failed(i, "read_address", rc);
	return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
	int i = (int)(intptr_t)arg;
	if (error->status == 0)
	{
		slots[i].svc_start = svc->start_handle;
		slots[i].svc_end = svc->end_handle;
		return 0;
	}
	if (error->status != BLE_HS_EDONE || !slots[i].svc_start)
	{
		setup_failed(i, "service", error->status);
		return 0;
	}
	int rc = ble_gattc_disc_all_chrs(conn, slots[i].svc_start, slots[i].svc_end, on_chr, arg);
	if (rc)
		setup_failed(i, "characteristics", rc);
	return 0;
}

static bool has_service(const struct ble_hs_adv_fields *f)
{
	for (int k = 0; k < f->num_uuids128; k++)
		if (!ble_uuid_cmp(&f->uuids128[k].u, &service_uuid.u))
			return true;
	return false;
}

static void report_seen(const ble_addr_t *a, int rssi, const struct ble_hs_adv_fields *f)
{
	for (int k = 0; k < seen_count; k++)
		if (!ble_addr_cmp(&seen[k], a))
			return;
	if (seen_count < SEEN_MAX)
		seen[seen_count++] = *a;
	char name[32] = "?", buf[18];
	if (f->name_len)
		snprintf(name, sizeof(name), "%.*s", f->name_len, f->name);
	out("A %s %d %s", addr_str(a, buf), rssi, name);
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void connect_to(const ble_addr_t *a, int rssi)
{
	// conn_interval exactly (the controller takes the longest interval offered), no latency; scan
	// 30 of 30 ms while connecting
	const struct ble_gap_conn_params params = {
		.scan_itvl = 0x30, .scan_window = 0x30,
		.itvl_min = conn_interval, .itvl_max = conn_interval, .latency = 0, .supervision_timeout = CONN_TIMEOUT,
		.min_ce_len = CONN_CE_LEN, .max_ce_len = CONN_CE_LEN,
	};
	ble_gap_disc_cancel();
	int rc = ble_gap_connect(own_addr_type, a, 3000, &params, gap_event, NULL);
	char buf[18];
	if (rc)
	{
		out("E - connect %s %d", addr_str(a, buf), rc);
		xSemaphoreTake(lock, portMAX_DELAY);
		connecting = false;
		xSemaphoreGive(lock);
		scan_resume(); // cancelled above
		return;
	}
	connect_started = xTaskGetTickCount();
	out("# connecting %s, %d dBm", addr_str(a, buf), rssi);
}

static void scan_resume(void)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	bool start = !connecting && free_slot() >= 0 && !ble_gap_disc_active();
	xSemaphoreGive(lock);
	if (!start)
		return;
	// Active (for the names in the A lines), 30 of 60 ms: leaves the radio to the connections
	struct ble_gap_disc_params params = {
		.itvl = 0x60, .window = 0x30, .filter_policy = 0, .limited = 0, .passive = 0, .filter_duplicates = 0,
	};
	int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
	if (rc && rc != BLE_HS_EALREADY)
		out("E - scan %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
	char buf[PACKET_LEN * 2 + 1];
	int i;
	switch (event->type)
	{
	case BLE_GAP_EVENT_DISC: {
		adv_reports++;
		struct ble_hs_adv_fields f;
		if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data))
			return 0;
		bool named = f.name_len >= strlen(NAME_PREFIX) && !memcmp(f.name, NAME_PREFIX, strlen(NAME_PREFIX));
		if (named)
			report_seen(&event->disc.addr, event->disc.rssi, &f);
		if (!has_service(&f))
			return 0;
		xSemaphoreTake(lock, portMAX_DELAY);
		bool go = !connecting && !addr_connected(&event->disc.addr) && free_slot() >= 0 && wanted(&event->disc.addr);
		if (go)
			connecting = true;
		xSemaphoreGive(lock);
		if (go)
			connect_to(&event->disc.addr, event->disc.rssi);
		return 0;
	}
	case BLE_GAP_EVENT_DISC_COMPLETE:
		scan_resume();
		return 0;
	case BLE_GAP_EVENT_CONNECT: {
		if (event->connect.status)
		{
			out("E - connect_failed %d", event->connect.status);
			xSemaphoreTake(lock, portMAX_DELAY);
			connecting = false;
			xSemaphoreGive(lock);
			scan_resume();
			return 0;
		}
		struct ble_gap_conn_desc desc;
		ble_gap_conn_find(event->connect.conn_handle, &desc);
		xSemaphoreTake(lock, portMAX_DELAY);
		i = free_slot();
		if (i >= 0)
		{
			memset(&slots[i], 0, sizeof(slots[i]));
			slots[i].state = SLOT_SETUP;
			slots[i].conn = event->connect.conn_handle;
			slots[i].addr = desc.peer_id_addr;
		}
		xSemaphoreGive(lock);
		if (i < 0)
		{
			ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
			return 0;
		}
		out("# %d connected %s, interval %u.%02u ms", i, addr_str(&desc.peer_id_addr, buf), desc.conn_itvl * 125 / 100, desc.conn_itvl * 125 % 100);
		int rc = ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &service_uuid.u, on_service, (void *)(intptr_t)i);
		if (rc)
			setup_failed(i, "service", rc);
		return 0;
	}
	case BLE_GAP_EVENT_DISCONNECT: {
		xSemaphoreTake(lock, portMAX_DELAY);
		i = slot_by_conn(event->disconnect.conn.conn_handle);
		// No slot: the connection was never established (HCI 0x3E), which NimBLE reports here
		// instead of as a failed CONNECT. Either way the attempt is over.
		if (i < 0 || slots[i].state == SLOT_SETUP)
			connecting = false;
		if (i >= 0)
			slots[i].state = SLOT_FREE;
		// Report it again when it advertises
		for (int k = 0; k < seen_count; k++)
			if (!ble_addr_cmp(&seen[k], &event->disconnect.conn.peer_id_addr))
				seen[k] = seen[--seen_count];
		xSemaphoreGive(lock);
		out("X %d %x", i, event->disconnect.reason);
		scan_resume();
		return 0;
	}
	case BLE_GAP_EVENT_NOTIFY_RX: {
		i = slot_by_conn(event->notify_rx.conn_handle);
		if (i < 0 || slots[i].state != SLOT_READY)
			return 0;
		uint8_t data[PACKET_LEN];
		int len = OS_MBUF_PKTLEN(event->notify_rx.om);
		if (event->notify_rx.attr_handle == slots[i].data_handle && len == PACKET_LEN)
		{
			os_mbuf_copydata(event->notify_rx.om, 0, len, data);
			slots[i].packets++;
			if (data_lines)
			{
				hex(buf, data, len);
				out("D %d %s", i, buf);
			}
			if (link_hid && slots[i].id >= 0 && data[0] < 224) // 224-255 are the receiver's own types
			{
				data[1] = slots[i].id;
				if (data[0] != 1 && data[0] != 4) // 1 and 4 use every byte (full precision)
					data[15] = slots[i].rssi;
				link_hid_packet(data);
			}
		}
		else if (event->notify_rx.attr_handle == slots[i].config_handle && len == CFG_MSG_LEN)
		{
			os_mbuf_copydata(event->notify_rx.om, 0, len, data);
			hex(buf, data, len);
			out("F %d %s", i, buf);
		}
		return 0;
	}
	case BLE_GAP_EVENT_CONN_UPDATE: {
		struct ble_gap_conn_desc desc;
		i = slot_by_conn(event->conn_update.conn_handle);
		if (event->conn_update.status || ble_gap_conn_find(event->conn_update.conn_handle, &desc))
			return 0;
		out("# %d interval %u.%02u ms, latency %u, timeout %u ms", i, desc.conn_itvl * 125 / 100, desc.conn_itvl * 125 % 100, desc.conn_latency, desc.supervision_timeout * 10);
		// The tracker asks 7.5-15 ms and a 4 s timeout (5 s after connecting). One notification
		// per event at 15 ms carries 66 packets/s, short of its 100 Hz moving rate; and after a
		// receiver reset the tracker would send into the void for the whole timeout before it
		// advertises again. The receiver, as central, sets its own once the tracker's are in.
		if (desc.conn_itvl != conn_interval || desc.conn_latency || desc.supervision_timeout != CONN_TIMEOUT)
		{
			const struct ble_gap_upd_params params = {
				.itvl_min = conn_interval, .itvl_max = conn_interval, .latency = 0,
				.supervision_timeout = CONN_TIMEOUT, .min_ce_len = CONN_CE_LEN, .max_ce_len = CONN_CE_LEN,
			};
			int rc = ble_gap_update_params(event->conn_update.conn_handle, &params);
			if (rc)
				out("E %d update_params %d", i, rc);
		}
		return 0;
	}
	case BLE_GAP_EVENT_CONN_UPDATE_REQ:
	case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
		return 0; // accepted as asked (self_params may be NULL here); corrected at CONN_UPDATE
	default:
		return 0;
	}
}

static void on_sync(void)
{
	int rc = ble_hs_util_ensure_addr(0);
	if (!rc)
		rc = ble_hs_id_infer_auto(0, &own_addr_type);
	if (rc)
	{
		out("E - address %d", rc);
		return;
	}
	out("# BLE ready");
	scan_resume();
}

static void on_reset(int reason)
{
	out("E - host_reset %d", reason);
}

static void host_task(void *param)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

static void hello(void)
{
	out("I SlimeBleReceiver " VERSION " %s", link_hid ? "hid" : "serial");
	for (int i = 0; i < MAX_TRACKERS; i++)
		if (slots[i].state == SLOT_READY)
		{
			char mac[13];
			mac_str(slots[i].mac, mac);
			out("C %d %s", i, mac);
		}
}

static void command(char *line)
{
	int i;
	char arg[48];
	if (line[0] == '?')
		hello();
	else if (line[0] == 'W' && sscanf(line + 1, "%d %40s", &i, arg) == 2 && i >= 0 && i < MAX_TRACKERS)
	{
		uint8_t msg[CFG_MSG_LEN];
		if (strlen(arg) != 2 * CFG_MSG_LEN || unhex(arg, msg, CFG_MSG_LEN))
		{
			out("E %d config_format 0", i);
			return;
		}
		if (slots[i].state != SLOT_READY || !slots[i].config_handle)
		{
			out("E %d config_not_connected 0", i);
			return;
		}
		int rc = ble_gattc_write_flat(slots[i].conn, slots[i].config_handle, msg, CFG_MSG_LEN, NULL, NULL);
		if (rc)
			out("E %d config_write %d", i, rc);
	}
	else if (line[0] == 'K' && sscanf(line + 1, "%d", &i) == 1 && i >= 0 && i < MAX_TRACKERS && slots[i].state != SLOT_FREE)
		ble_gap_terminate(slots[i].conn, BLE_ERR_REM_USER_CONN_TERM);
	else if (line[0] == 'O')
	{
		const char *p = line + 1;
		while (*p == ' ')
			p++;
		xSemaphoreTake(lock, portMAX_DELAY);
		snprintf(only, sizeof(only), "%s", p);
		xSemaphoreGive(lock);
		out("# only: %s", only[0] ? only : "all");
	}
	else if (line[0] == 'D' && sscanf(line + 1, "%d", &i) == 1)
	{
		data_lines = i;
		out("# data lines %s", data_lines ? "on" : "off");
	}
	else if (line[0] == 'P' && sscanf(line + 1, "%d", &i) == 1 && i >= 6 && i <= 80)
	{
		conn_interval = i; // the connected trackers too
		const struct ble_gap_upd_params params = {
			.itvl_min = i, .itvl_max = i, .latency = 0, .supervision_timeout = CONN_TIMEOUT, .min_ce_len = CONN_CE_LEN, .max_ce_len = CONN_CE_LEN,
		};
		for (int k = 0; k < MAX_TRACKERS; k++)
			if (slots[k].state == SLOT_READY)
				ble_gap_update_params(slots[k].conn, &params);
		out("# interval %d.%02d ms", i * 125 / 100, i * 125 % 100);
	}
	else if (line[0] == 'L')
	{
		for (int k = 0; k < id_count; k++)
		{
			char mac[13];
			mac_str(ids[k], mac);
			out("# id %d %s", k, mac);
		}
		out("# %d tracker ids", id_count);
	}
	else if (line[0] == 'Z')
	{
		xSemaphoreTake(lock, portMAX_DELAY);
		id_count = 0;
		ids_save();
		for (int k = 0; k < MAX_TRACKERS; k++)
			if (slots[k].state != SLOT_FREE)
				ble_gap_terminate(slots[k].conn, BLE_ERR_REM_USER_CONN_TERM); // new ids as they come back
		xSemaphoreGive(lock);
		out("# tracker ids cleared");
	}
	else if (line[0] == 'B')
	{
		out("# download mode");
		vTaskDelay(pdMS_TO_TICKS(100)); // let the line out
		link_download_mode();
	}
}

static void pc_task(void *param)
{
	char line[96];
	int len = 0;
	while (1)
	{
		uint8_t c;
		if (link_read(&c, 1, portMAX_DELAY) != 1)
			continue;
		if (c == '\r' || c == '\n')
		{
			line[len] = 0;
			if (len)
				command(line);
			len = 0;
		}
		else if (len < (int)sizeof(line) - 1)
			line[len++] = c;
	}
}

// Every second the trackers' RSSI (for the HID packets); every 5 s one line per tracker with its
// notification rate and link
static void status_task(void *param)
{
	uint32_t last_adv = 0;
	int ticks = 0;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
		for (int i = 0; i < MAX_TRACKERS; i++)
		{
			int8_t rssi;
			if (slots[i].state == SLOT_READY && !ble_gap_conn_rssi(slots[i].conn, &rssi) && rssi < 0)
				slots[i].rssi = -rssi;
		}
		scan_resume(); // whenever it should run but does not (a path that missed restarting it)
		if (++ticks < 5)
			continue;
		ticks = 0;
		// An attempt whose end never came back would stop scanning for good: drop it
		xSemaphoreTake(lock, portMAX_DELAY);
		bool stuck = connecting && xTaskGetTickCount() - connect_started > pdMS_TO_TICKS(CONNECT_STUCK_MS);
		int setup = -1;
		for (int i = 0; i < MAX_TRACKERS; i++)
			if (slots[i].state == SLOT_SETUP)
				setup = i;
		if (stuck && setup < 0)
			connecting = false;
		xSemaphoreGive(lock);
		if (stuck)
		{
			out("E %d connect_stuck 0", setup);
			if (setup >= 0)
				ble_gap_terminate(slots[setup].conn, BLE_ERR_REM_USER_CONN_TERM); // its DISCONNECT clears it
			else
			{
				ble_gap_conn_cancel();
				scan_resume();
			}
		}
		char line[360];
		int n = snprintf(line, sizeof(line), "# %s, %lu adverts/s", ble_gap_disc_active() ? "scanning" : "not scanning", (unsigned long)(adv_reports - last_adv) / 5);
		last_adv = adv_reports;
		for (int i = 0; i < MAX_TRACKERS && n < (int)sizeof(line) - 44; i++)
		{
			if (slots[i].state == SLOT_FREE)
				continue;
			struct ble_gap_conn_desc desc;
			uint32_t packets = slots[i].packets;
			int itvl = ble_gap_conn_find(slots[i].conn, &desc) ? 0 : desc.conn_itvl * 125 / 100;
			n += snprintf(line + n, sizeof(line) - n, "; %d %02X%02X %s %lu/s -%ddBm %dms", i, slots[i].mac[1], slots[i].mac[0],
				slots[i].state == SLOT_READY ? "ok" : "setup", (unsigned long)(packets - slots[i].reported) / 5, slots[i].rssi, itvl);
			slots[i].reported = packets;
		}
		out("%s", line);
	}
}

void app_main(void)
{
	lock = xSemaphoreCreateMutex();

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	link_init();
	data_lines = !link_hid;
	ids_load();
	hello();
	ESP_ERROR_CHECK(nimble_port_init());
	ble_hs_cfg.sync_cb = on_sync;
	ble_hs_cfg.reset_cb = on_reset;
	nimble_port_freertos_init(host_task);

	xTaskCreate(pc_task, "pc", 3072, NULL, 5, NULL);
	xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);
}
