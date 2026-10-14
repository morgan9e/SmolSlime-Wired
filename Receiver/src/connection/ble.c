/*
	BLE receiver mode (CONFIG_RECEIVER_BLE, console: ble on|off)

	The receiver as a BLE central for trackers in BLE mode (Tracker/src/connection/ble.c): it
	finds them by their service UUID, connects up to CONFIG_BT_MAX_CONN, discovers the service,
	reads the tracker's device address and subscribes to its data and settings notifications.
	Their packets go to the USB HID path as ESB packets do (hid_write_packet_n), so SlimeVR Server
	sees no difference.

	A tracker's id is its place in the paired list, as over ESB: the address characteristic holds
	the same device address the tracker sends when pairing over ESB, so paired trackers keep their
	ids. As over ESB, a tracker not in the list is added only while pairing (console pair / exit);
	otherwise it is disconnected and left alone until pairing starts.

	ESB and BLE do not share the radio: the receiver_ble setting picks one at boot. MPSL keeps its
	default clock control in both modes (the tracker's Kconfig, TRACKER_BLE, has why); in ESB mode
	it stays idle and ESB takes the radio interrupt over.
*/
#include "globals.h"
#include "system/system.h"
#include "hid.h"
#include "remote_config.h"
#include "esb.h"
#include "ble.h"

#if CONFIG_RECEIVER_BLE

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

#define RECEIVER_BLE_ID 1000 // NVS id of the setting, clear of STORED_ADDR_0 + MAX_TRACKERS

// 6e5c000N-7a3b-4c8e-9f1d-2b4a5c6d7e8f, as Tracker/src/connection/ble.c
#define SLIME_UUID_VAL(n) BT_UUID_128_ENCODE(0x6e5c0000 + (n), 0x7a3b, 0x4c8e, 0x9f1d, 0x2b4a5c6d7e8f)
static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(1));
static const struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(2));
static const struct bt_uuid_128 address_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(3));
static const struct bt_uuid_128 config_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(4));

#define PACKET_LEN 17 // the 16-byte packet and its sequence
#define CFG_LEN 10

// No latency, 1 s supervision timeout (after a receiver reset a tracker advertises again 1 s
// later); the tracker asks 7.5-15 ms and 4 s, the receiver keeps its own (le_param_req).
// The interval leaves each tracker 1.5 ms (Kconfig BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT), two
// notification exchanges: 10 ms for up to six trackers (200 packets/s each), 12.5 ms for seven
// or eight (160/s). A moving tracker sends about 167/s. Measured with six moving: one exchange per
// event (7.5 ms / 0.9 ms, 10 ms / 1.2 ms: the controller budgets each exchange for 27-byte
// packets both ways) lost ~30% of the packets at the trackers.
#define CONN_TIMEOUT 100
#define CONN_INTERVAL_FEW 8 // 10 ms
#define CONN_INTERVAL_MANY 10 // 12.5 ms
#define CONN_FEW 6
static struct bt_le_conn_param conn_param = BT_LE_CONN_PARAM_INIT(CONN_INTERVAL_FEW, CONN_INTERVAL_FEW, 0, CONN_TIMEOUT);

enum link_state { LINK_FREE, LINK_SETUP, LINK_READY };

struct link {
	enum link_state state;
	struct bt_conn *conn;
	int id; // tracker id (paired list), -1 until the address is read
	uint64_t addr;
	uint16_t address_handle, config_handle;
	struct bt_gatt_subscribe_params data_sub, config_sub;
	struct bt_gatt_read_params read;
	uint8_t rssi; // -dBm, as ESB gives it
	uint32_t packets, reported;
};

static struct link links[CONFIG_BT_MAX_CONN];
static struct k_mutex links_lock;
static bool connecting; // a connection or its setup runs: scanning waits
static int64_t connect_started;

// Pairing (console pair / exit): only then are trackers not in the paired list added. A tracker
// turned away is not connected again until pairing starts, as it would only be turned away again.
static bool pairing;
#define REJECTED_MAX 16
static bt_addr_le_t rejected[REJECTED_MAX];
static int rejected_count, rejected_next;

static int paired_id(uint64_t addr)
{
	for (int i = 0; i < stored_trackers; i++)
		if (addr && stored_tracker_addr[i] == addr)
			return i;
	return -1;
}

static void reject(const bt_addr_le_t *addr)
{
	rejected[rejected_next] = *addr;
	rejected_next = (rejected_next + 1) % REJECTED_MAX;
	if (rejected_count < REJECTED_MAX)
		rejected_count++;
}

static bool is_rejected(const bt_addr_le_t *addr)
{
	for (int i = 0; i < rejected_count; i++)
		if (bt_addr_le_eq(&rejected[i], addr))
			return true;
	return false;
}

void ble_set_pairing(bool on)
{
	pairing = on;
	if (on)
		rejected_count = rejected_next = 0; // try them all again
	set_led(on ? SYS_LED_PATTERN_SHORT : SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_CONNECTION);
}
static int8_t mode = -1; // latched at boot

static void scan_start(void);

bool ble_mode(void)
{
	if (mode < 0)
	{
		uint8_t setting = 0;
		sys_read(RECEIVER_BLE_ID, &setting, sizeof(setting));
		mode = setting ? 1 : 0;
	}
	return mode;
}

void ble_set_mode(bool on)
{
	uint8_t setting = 0;
	sys_read(RECEIVER_BLE_ID, &setting, sizeof(setting));
	if (!!setting == on && ble_mode() == on)
	{
		printk("Already using %s\n", on ? "BLE" : "ESB");
		return;
	}
	setting = on;
	sys_write(RECEIVER_BLE_ID, NULL, &setting, sizeof(setting));
	printk("Switching to %s, rebooting\n", on ? "BLE" : "ESB");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
}

static struct link *link_by_conn(struct bt_conn *conn)
{
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		if (links[i].state != LINK_FREE && links[i].conn == conn)
			return &links[i];
	return NULL;
}

static struct link *link_by_id(int id)
{
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		if (links[i].state == LINK_READY && links[i].id == id)
			return &links[i];
	return NULL;
}

// The interval for this many trackers; the connected ones move to it. links_lock held.
static void interval_for(int trackers)
{
	uint16_t interval = trackers > CONN_FEW ? CONN_INTERVAL_MANY : CONN_INTERVAL_FEW;
	if (interval == conn_param.interval_min)
		return;
	conn_param.interval_min = conn_param.interval_max = interval;
	LOG_INF("%d trackers: interval %u.%02u ms", trackers, interval * 125 / 100, interval * 125 % 100);
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		if (links[i].state == LINK_READY)
			bt_conn_le_param_update(links[i].conn, &conn_param);
}

static int used_count(void)
{
	int n = 0;
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		n += links[i].state != LINK_FREE;
	return n;
}

static struct link *link_free(void)
{
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		if (links[i].state == LINK_FREE)
			return &links[i];
	return NULL;
}

static void setup_failed(struct link *l, const char *what, int err)
{
	LOG_WRN("Tracker setup failed at %s: %d", what, err);
	bt_conn_disconnect(l->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void setup_end(void)
{
	connecting = false;
	scan_start();
}

/* ---- notifications ---- */

static uint8_t on_data(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data, uint16_t length)
{
	struct link *l = CONTAINER_OF(params, struct link, data_sub);
	if (!data)
		return BT_GATT_ITER_STOP; // unsubscribed
	if (length != PACKET_LEN || l->id < 0)
		return BT_GATT_ITER_CONTINUE;
	uint8_t packet[16];
	memcpy(packet, data, sizeof(packet));
	if (packet[0] > 223) // reserved for the receiver
		return BT_GATT_ITER_CONTINUE;
	packet[1] = l->id;
	l->packets++;
	hid_write_packet_n(packet, l->rssi);
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_config(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data, uint16_t length)
{
	if (!data)
		return BT_GATT_ITER_STOP;
	remote_config_ble_rx(data, length);
	return BT_GATT_ITER_CONTINUE;
}

// One settings request at a time (remote_config.c waits for its answer): its buffer and params
static uint8_t config_request[CFG_LEN];
static struct bt_gatt_write_params config_write;

static void config_written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	if (err)
		LOG_WRN("Settings request not taken: 0x%02x", err); // remote_config.c times out
}

int ble_write_config(uint8_t tracker_id, const uint8_t *data, uint8_t len)
{
	if (len != CFG_LEN)
		return -EINVAL;
	k_mutex_lock(&links_lock, K_FOREVER);
	struct link *l = link_by_id(tracker_id);
	int err = -ENOTCONN;
	if (l && l->config_handle)
	{
		memcpy(config_request, data, len);
		config_write.func = config_written;
		config_write.handle = l->config_handle;
		config_write.offset = 0;
		config_write.data = config_request;
		config_write.length = len;
		err = bt_gatt_write(l->conn, &config_write); // the tracker's characteristic takes writes with response
	}
	k_mutex_unlock(&links_lock);
	return err;
}

/* ---- setup: discovery, address, subscriptions ---- */

static void subscribe(struct link *l, struct bt_gatt_subscribe_params *sub, uint16_t value_handle, uint16_t ccc_handle,
		      bt_gatt_notify_func_t func)
{
	sub->notify = func;
	sub->value = BT_GATT_CCC_NOTIFY;
	sub->value_handle = value_handle;
	sub->ccc_handle = ccc_handle;
	int err = bt_gatt_subscribe(l->conn, sub);
	if (err && err != -EALREADY)
		LOG_WRN("Subscribe failed: %d", err);
}

static uint8_t on_address(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params, const void *data, uint16_t length)
{
	struct link *l = CONTAINER_OF(params, struct link, read);
	if (err || !data || length != 6)
	{
		setup_failed(l, "address", err ? err : length);
		return BT_GATT_ITER_STOP;
	}
	uint64_t addr = 0;
	memcpy(&addr, data, 6); // little endian, as the paired list holds it
	int id = paired_id(addr);
	if (id < 0 && !pairing)
	{
		// Not paired: left alone until pairing starts (console pair), as over ESB
		reject(bt_conn_get_dst(conn));
		LOG_INF("Tracker %012llX is not paired: ignored until pairing", addr);
		bt_conn_disconnect(l->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}
	if (id < 0)
	{
		esb_add_pair(addr, true); // a new tracker: the first free id
		id = paired_id(addr);
	}
	if (id < 0)
	{
		setup_failed(l, "paired list full", 0);
		return BT_GATT_ITER_STOP;
	}
	l->addr = addr;
	l->id = id;
	l->state = LINK_READY;
	LOG_INF("Tracker %d (%012llX) connected", id, addr);
	set_led(SYS_LED_PATTERN_ONESHOT_PROGRESS, SYS_LED_PRIORITY_HIGHEST);
	setup_end();
	return BT_GATT_ITER_STOP;
}

static void discovery_completed(struct bt_gatt_dm *dm, void *context)
{
	struct link *l = context;
	const struct bt_gatt_dm_attr *chrc, *value, *ccc;
	uint16_t data_handle = 0, data_ccc = 0, config_ccc = 0;

	chrc = bt_gatt_dm_char_by_uuid(dm, &data_uuid.uuid);
	if (chrc && (value = bt_gatt_dm_desc_by_uuid(dm, chrc, &data_uuid.uuid)) && (ccc = bt_gatt_dm_desc_by_uuid(dm, chrc, BT_UUID_GATT_CCC)))
	{
		data_handle = value->handle;
		data_ccc = ccc->handle;
	}
	chrc = bt_gatt_dm_char_by_uuid(dm, &address_uuid.uuid);
	if (chrc && (value = bt_gatt_dm_desc_by_uuid(dm, chrc, &address_uuid.uuid)))
		l->address_handle = value->handle;
	chrc = bt_gatt_dm_char_by_uuid(dm, &config_uuid.uuid);
	if (chrc && (value = bt_gatt_dm_desc_by_uuid(dm, chrc, &config_uuid.uuid)) && (ccc = bt_gatt_dm_desc_by_uuid(dm, chrc, BT_UUID_GATT_CCC)))
	{
		l->config_handle = value->handle;
		config_ccc = ccc->handle;
	}
	bt_gatt_dm_data_release(dm);

	if (!data_handle || !l->address_handle)
	{
		setup_failed(l, "characteristics", 0);
		return;
	}
	subscribe(l, &l->data_sub, data_handle, data_ccc, on_data);
	if (l->config_handle)
		subscribe(l, &l->config_sub, l->config_handle, config_ccc, on_config);

	l->read.func = on_address;
	l->read.handle_count = 1;
	l->read.single.handle = l->address_handle;
	l->read.single.offset = 0;
	int err = bt_gatt_read(l->conn, &l->read);
	if (err)
		setup_failed(l, "read address", err);
}

static void discovery_not_found(struct bt_conn *conn, void *context)
{
	setup_failed(context, "service", -ENOENT);
}

static void discovery_error(struct bt_conn *conn, int err, void *context)
{
	setup_failed(context, "discovery", err);
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed,
	.service_not_found = discovery_not_found,
	.error_found = discovery_error,
};

/* ---- connections ---- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	k_mutex_lock(&links_lock, K_FOREVER);
	struct link *l = link_by_conn(conn);
	if (!l)
	{
		k_mutex_unlock(&links_lock);
		return;
	}
	if (err)
	{
		LOG_WRN("Connection failed: 0x%02x", err);
		bt_conn_unref(l->conn);
		l->state = LINK_FREE;
		l->conn = NULL;
		k_mutex_unlock(&links_lock);
		setup_end();
		return;
	}
	k_mutex_unlock(&links_lock);
	int rc = bt_gatt_dm_start(conn, &service_uuid.uuid, &discovery_cb, l);
	if (rc)
		setup_failed(l, "discovery start", rc);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_mutex_lock(&links_lock, K_FOREVER);
	struct link *l = link_by_conn(conn);
	if (!l)
	{
		k_mutex_unlock(&links_lock);
		return;
	}
	bool was_setup = l->state == LINK_SETUP;
	if (l->id >= 0)
		LOG_INF("Tracker %d disconnected: 0x%02x", l->id, reason);
	bt_conn_unref(l->conn);
	memset(l, 0, sizeof(*l));
	l->id = -1;
	interval_for(used_count());
	k_mutex_unlock(&links_lock);
	if (was_setup)
		connecting = false;
	scan_start();
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	// The tracker's own wish (7.5-15 ms, 4 s): this receiver's instead
	*param = conn_param;
	return true;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	struct link *l = link_by_conn(conn);
	LOG_INF("Tracker %d: interval %u.%02u ms, timeout %u ms", l ? l->id : -1, interval * 125 / 100, interval * 125 % 100, timeout * 10);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
};

/* ---- scanning ---- */

static bool ad_has_service(struct bt_data *data, void *user_data)
{
	bool *found = user_data;
	if (data->type == BT_DATA_UUID128_ALL || data->type == BT_DATA_UUID128_SOME)
		for (int i = 0; i + 16 <= data->data_len; i += 16)
			if (!memcmp(&data->data[i], service_uuid.val, 16))
				*found = true;
	return !*found;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
	if (connecting || (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND))
		return;
	bool found = false;
	bt_data_parse(ad, ad_has_service, &found);
	if (!found)
		return;
	if (is_rejected(addr))
		return;
	struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	if (existing)
	{
		bt_conn_unref(existing);
		return;
	}
	k_mutex_lock(&links_lock, K_FOREVER);
	struct link *l = link_free();
	if (!l)
	{
		k_mutex_unlock(&links_lock);
		return;
	}
	connecting = true;
	connect_started = k_uptime_get();
	interval_for(used_count() + 1); // with the new one
	bt_le_scan_stop();
	struct bt_conn *conn = NULL;
	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, &conn_param, &conn);
	if (err)
	{
		k_mutex_unlock(&links_lock);
		LOG_WRN("Connect failed: %d", err);
		setup_end();
		return;
	}
	memset(l, 0, sizeof(*l));
	l->id = -1;
	l->conn = conn; // the reference from bt_conn_le_create
	l->state = LINK_SETUP;
	l->rssi = -rssi;
	k_mutex_unlock(&links_lock);
}

static int ready_count(void)
{
	int n = 0;
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		n += links[i].state == LINK_READY;
	return n;
}

// Passive. Without trackers 30 of 60 ms; with any, 30 ms a second: a tracker advertises every
// 20-30 ms, so one window finds it, and the connections keep the radio. Scanning 30 of 60 ms
// beside six moving trackers cost four of them 27-35% of their packets (the controller raises a
// starved scanner over connection events).
static bool scan_busy; // the duty the scan runs with
static void scan_start(void)
{
	if (connecting || !link_free())
		return;
	bool busy = ready_count() > 0;
	if (busy != scan_busy)
		bt_le_scan_stop(); // restart with the other duty
	scan_busy = busy;
	struct bt_le_scan_param param = busy ? (struct bt_le_scan_param)BT_LE_SCAN_PARAM_INIT(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE, 0x0640, 0x0030)
					     : (struct bt_le_scan_param)BT_LE_SCAN_PARAM_INIT(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE, 0x0060, 0x0030);
	int err = bt_le_scan_start(&param, device_found);
	if (err && err != -EALREADY)
		LOG_WRN("Scan failed: %d", err);
}

/* ---- status ---- */

static int read_rssi(struct bt_conn *conn, int8_t *rssi)
{
	uint16_t handle;
	if (bt_hci_get_conn_handle(conn, &handle))
		return -ENOTCONN;
	struct net_buf *buf = bt_hci_cmd_alloc(K_MSEC(100));
	if (!buf)
		return -ENOBUFS;
	struct bt_hci_cp_read_rssi *cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	struct net_buf *rsp = NULL;
	int err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (!err)
		*rssi = ((struct bt_hci_rp_read_rssi *)rsp->data)->rssi;
	if (rsp)
		net_buf_unref(rsp);
	return err;
}

void ble_print_status(void)
{
	uint8_t setting = 0;
	sys_read(RECEIVER_BLE_ID, &setting, sizeof(setting));
	printk("Connection: %s\n", ble_mode() ? "BLE" : "ESB");
	if (!!setting != ble_mode())
		printk("After reboot: %s\n", setting ? "BLE" : "ESB");
	if (!ble_mode())
		return;
	int count = 0;
	k_mutex_lock(&links_lock, K_FOREVER);
	for (int i = 0; i < ARRAY_SIZE(links); i++)
	{
		struct link *l = &links[i];
		if (l->state == LINK_FREE)
			continue;
		count++;
		struct bt_conn_info info;
		uint32_t interval_us = bt_conn_get_info(l->conn, &info) ? 0 : info.le.interval_us;
		if (l->state == LINK_SETUP)
			printk("  setting up a connection\n");
		else
			printk("  tracker %d %012llX: -%u dBm, %u.%02u ms, PHY %s, %u packets\n", l->id, l->addr, l->rssi,
			       interval_us / 1000, interval_us % 1000 / 10,
			       interval_us && info.le.phy ? (info.le.phy->tx_phy == BT_GAP_LE_PHY_2M ? "2M" : "1M") : "?", l->packets);
	}
	k_mutex_unlock(&links_lock);
	printk("BLE: %d of %d connections%s%s\n", count, CONFIG_BT_MAX_CONN, connecting ? ", connecting" : "",
	       pairing ? ", pairing (new trackers are added)" : "");
}

void ble_disconnect_id(int id)
{
	if (!ble_mode())
		return;
	k_mutex_lock(&links_lock, K_FOREVER);
	struct link *l = link_by_id(id);
	if (l)
		bt_conn_disconnect(l->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	k_mutex_unlock(&links_lock);
}

void ble_disconnect_all(void)
{
	if (!ble_mode())
		return;
	k_mutex_lock(&links_lock, K_FOREVER);
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		if (links[i].state != LINK_FREE)
			bt_conn_disconnect(links[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	k_mutex_unlock(&links_lock);
}

/* ---- thread ---- */

static K_SEM_DEFINE(start_sem, 0, 1);

void ble_start(void)
{
	k_sem_give(&start_sem);
}

static void ble_thread(void)
{
	k_mutex_init(&links_lock);
	for (int i = 0; i < ARRAY_SIZE(links); i++)
		links[i].id = -1;
	k_sem_take(&start_sem, K_FOREVER); // esb_thread, after loading the paired list, in BLE mode only
	int err = bt_enable(NULL);
	if (err)
	{
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}
	LOG_INF("BLE mode: %d trackers paired, scanning", stored_trackers);
	scan_start();
	while (1)
	{
		k_msleep(1000);
		// RSSI for the HID packets, as ESB gives it per frame
		for (int i = 0; i < ARRAY_SIZE(links); i++)
		{
			int8_t rssi;
			if (links[i].state == LINK_READY && !read_rssi(links[i].conn, &rssi) && rssi < 0)
				links[i].rssi = -rssi;
		}
		// A connection or setup that never ended would stop scanning for good: drop it
		if (connecting && k_uptime_get() - connect_started > 10000)
		{
			LOG_WRN("Connection setup stuck, dropping it");
			k_mutex_lock(&links_lock, K_FOREVER);
			for (int i = 0; i < ARRAY_SIZE(links); i++)
				if (links[i].state == LINK_SETUP)
					bt_conn_disconnect(links[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			k_mutex_unlock(&links_lock);
			connecting = false;
		}
		scan_start(); // whenever it should run but does not
	}
}

K_THREAD_DEFINE(ble_thread_id, 2048, ble_thread, NULL, NULL, NULL, 7, 0, 0);

#endif
