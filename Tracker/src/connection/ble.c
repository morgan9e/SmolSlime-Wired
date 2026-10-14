/*
	BLE connection mode (CONFIG_TRACKER_BLE, console: ble on|off)

	The tracker is a BLE peripheral with one GATT service. Its data characteristic notifies the
	same 16-byte packets connection_thread sends over ESB (types 0-7, see connection.c), plus the
	packet sequence: 17 bytes, within the default ATT MTU. The address characteristic reads the
	tracker's device address (FICR DEVICEADDR, 6 bytes little endian, as the receiver reports it),
	so the PC bridge (tools/ble_bridge.py) gives SlimeVR the same tracker identity as over ESB.
	The config characteristic carries settings over the radio (remote_config.c): the PC writes a
	10-byte CFG_REQ, the tracker notifies the CFG_RESP, the same messages as through the receiver.

	ESB and BLE do not share the radio: the connection_over_ble setting picks one at boot. In ESB
	mode MPSL is uninitialized before ESB starts (ble_mode_init); in BLE mode ESB is never
	initialized. OTA updates need ESB; USB works in both modes.
*/
#include "globals.h"
#include "system/system.h"
#include "ble.h"
#include "ota.h"
#include "ota_proto.h"
#include "remote_config.h"

#if CONFIG_TRACKER_BLE

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

// 6e5c000N-7a3b-4c8e-9f1d-2b4a5c6d7e8f, also in tools/ble_bridge.py
#define SLIME_UUID_VAL(n) BT_UUID_128_ENCODE(0x6e5c0000 + (n), 0x7a3b, 0x4c8e, 0x9f1d, 0x2b4a5c6d7e8f)
#define SLIME_SERVICE_UUID_VAL SLIME_UUID_VAL(1)

static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SLIME_SERVICE_UUID_VAL);
static const struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(2));
static const struct bt_uuid_128 address_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(3));
static const struct bt_uuid_128 config_uuid = BT_UUID_INIT_128(SLIME_UUID_VAL(4));

#define BLE_PACKET_LEN 17 // 16-byte packet and its sequence
#define BLE_MAX_IN_FLIGHT 2 // notifications queued in the stack; the next waits, and only the newest

static int8_t mode = -1; // latched at boot
static uint8_t device_address[6];
static char name[20];

static struct bt_conn *current_conn;
static struct k_spinlock conn_lock;
static bool notify_enabled;
static atomic_t in_flight;
static K_SEM_DEFINE(sent_sem, 0, 1); // a notification left the stack
static uint32_t sent_count, dropped_count;

static void ble_thread(void);
K_THREAD_DEFINE(ble_thread_id, 1024, ble_thread, NULL, NULL, NULL, ESB_THREAD_PRIORITY, 0, 0);

bool ble_mode(void)
{
	if (mode < 0) // settings are read during init (sys_retained_init), see ble_mode_init
		mode = CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_OVER_BLE) ? 1 : 0;
	return mode;
}

// ESB mode: MPSL stays initialized (it runs the clocks, also for USB) but idle, since the BLE
// stack is never enabled; esb_init connects ESB's own radio interrupt over MPSL's (dynamic
// interrupts). Uninitializing MPSL would stop the clocks it controls.

static ssize_t read_address(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, device_address, sizeof(device_address));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = value == BT_GATT_CCC_NOTIFY;
	LOG_INF("Notifications %s", notify_enabled ? "on" : "off");
}

// A settings request (CFG_REQ) from the PC. The link is point to point, so its tracker id byte is
// taken as this tracker's. A request while the last one is still carried out is dropped; the PC
// sends it again.
static ssize_t write_config(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;
	if (offset || len != CFG_MSG_LEN)
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	remote_config_rx_ack(data, len, data[1]);
	return len;
}

BT_GATT_SERVICE_DEFINE(slime_service,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&address_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_address, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&config_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_WRITE, NULL, write_config, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);
#define CONFIG_VALUE_ATTR (&slime_service.attrs[7]) // [6] its declaration, [8] its CCC

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SLIME_SERVICE_UUID_VAL),
};

static struct bt_data sd[] = {
	{.type = BT_DATA_NAME_COMPLETE, .data = (const uint8_t *)name},
};

// 20-30 ms, the fastest connectable advertising: a PC whose Bluetooth radio is busy (headphone
// audio) listens only in short windows, and connects sooner the more often the tracker advertises.
// Only while not connected.
#define BLE_ADV_PARAM BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, 0x0020, 0x0030, NULL) // units of 0.625 ms

static void advertise(void)
{
	int err = bt_le_adv_start(BLE_ADV_PARAM, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY)
		LOG_ERR("Advertising failed: %d", err);
}

static void advertise_work_handler(struct k_work *work)
{
	advertise();
}

static K_WORK_DEFINE(advertise_work, advertise_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err)
	{
		LOG_WRN("Connection from %s failed: 0x%02x", addr, err);
		return;
	}
	K_SPINLOCK(&conn_lock) {
		current_conn = bt_conn_ref(conn);
	}
	LOG_INF("Connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *old = NULL;
	K_SPINLOCK(&conn_lock) {
		if (current_conn == conn)
		{
			old = current_conn;
			current_conn = NULL;
		}
	}
	if (old)
		bt_conn_unref(old);
	notify_enabled = false;
	atomic_set(&in_flight, 0);
	k_sem_give(&sent_sem); // ble_tx_thread may be waiting for room
	LOG_INF("Disconnected: 0x%02x", reason);
}

static void recycled(void)
{
	k_work_submit(&advertise_work); // the connection object is free again: advertise for the next
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	LOG_INF("Connection interval %u.%02u ms, latency %u", interval * 125 / 100, interval * 125 % 100, latency);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.le_param_updated = le_param_updated,
};

bool ble_connected(void)
{
	return current_conn != NULL;
}

static void notify_sent(struct bt_conn *conn, void *user_data)
{
	if (atomic_dec(&in_flight) <= 0)
		atomic_set(&in_flight, 0);
	sent_count++;
	k_sem_give(&sent_sem);
	ota_tx_acked(); // a new MCUboot image confirms itself once its link works, in BLE mode too
}

// The newest packet from connection_thread, waiting for ble_tx_thread. connection_thread has a
// small stack (512 B) that the BLE host's send path does not fit in, so it only hands packets over.
static uint8_t pending[BLE_PACKET_LEN];
static bool pending_valid;
static struct k_spinlock pending_lock;
static K_SEM_DEFINE(pending_sem, 0, 1);

void ble_write(const uint8_t *data)
{
	if (!notify_enabled)
		return;
	K_SPINLOCK(&pending_lock) {
		if (pending_valid)
			dropped_count++; // the link is behind: the newer packet replaces it, as ESB flushes
		memcpy(pending, data, 16);
		pending[16] = data[20]; // sequence
		pending_valid = true;
	}
	k_sem_give(&pending_sem);
}

static void ble_tx_thread(void)
{
	if (!ble_mode())
		return;
	uint8_t payload[BLE_PACKET_LEN];
	while (1)
	{
		k_sem_take(&pending_sem, K_FOREVER);
		// Wait for room in the stack; packets arriving meanwhile replace the pending one
		while (notify_enabled && atomic_get(&in_flight) >= BLE_MAX_IN_FLIGHT)
			if (k_sem_take(&sent_sem, K_MSEC(100)))
				atomic_set(&in_flight, 0); // no completion in 100 ms: do not stall on a lost one
		bool have = false;
		K_SPINLOCK(&pending_lock) {
			if (pending_valid)
			{
				memcpy(payload, pending, sizeof(payload));
				pending_valid = false;
				have = true;
			}
		}
		if (!have || !notify_enabled)
			continue;
		struct bt_conn *conn = NULL;
		K_SPINLOCK(&conn_lock) {
			if (current_conn)
				conn = bt_conn_ref(current_conn);
		}
		if (!conn)
			continue;
		struct bt_gatt_notify_params params = {
			.attr = &slime_service.attrs[2], // data value
			.data = payload,
			.len = sizeof(payload),
			.func = notify_sent,
		};
		atomic_inc(&in_flight);
		if (bt_gatt_notify_cb(conn, &params))
		{
			atomic_dec(&in_flight);
			dropped_count++;
		}
		bt_conn_unref(conn);
	}
}

K_THREAD_DEFINE(ble_tx_thread_id, 1536, ble_tx_thread, NULL, NULL, NULL, CONNECTION_THREAD_PRIORITY, 0, 0);

int ble_write_config(const uint8_t *data, uint8_t len)
{
	struct bt_conn *conn = NULL;
	K_SPINLOCK(&conn_lock) {
		if (current_conn)
			conn = bt_conn_ref(current_conn);
	}
	if (!conn)
		return -ENOTCONN;
	int err = bt_gatt_notify(conn, CONFIG_VALUE_ATTR, data, len); // may wait for a buffer: own thread
	bt_conn_unref(conn);
	return err;
}

void ble_print_status(void)
{
	bool setting = CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_OVER_BLE);
	printk("Connection: %s\n", ble_mode() ? "BLE" : "ESB");
	if (setting != ble_mode())
		printk("After reboot: %s\n", setting ? "BLE" : "ESB");
	if (!ble_mode())
		return;
	printk("BLE name: %s\n", name);
	struct bt_conn *conn = NULL;
	K_SPINLOCK(&conn_lock) {
		if (current_conn)
			conn = bt_conn_ref(current_conn);
	}
	if (!conn)
	{
		printk("BLE: advertising\n");
		return;
	}
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	struct bt_conn_info info;
	if (!bt_conn_get_info(conn, &info))
		printk("BLE: connected to %s, interval %u.%02u ms\n", addr, info.le.interval * 125 / 100, info.le.interval * 125 % 100);
	bt_conn_unref(conn);
	printk("Notifications: %s, sent %u, dropped %u\n", notify_enabled ? "on" : "off", sent_count, dropped_count);
}

void ble_set_mode(bool on)
{
	bool setting = CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_OVER_BLE);
	if (setting == on && ble_mode() == on)
	{
		printk("Already using %s\n", on ? "BLE" : "ESB");
		return;
	}
	config_3_settings_write(CONFIG_3_CONNECTION_OVER_BLE, on);
	printk("Switching to %s, rebooting\n", on ? "BLE" : "ESB");
	k_msleep(100); // let the console print
	sys_request_system_reboot(false);
}

static void ble_thread(void)
{
	if (!ble_mode())
		return;

	memcpy(device_address, (const void *)NRF_FICR->DEVICEADDR, sizeof(device_address));
	snprintk(name, sizeof(name), "SlimeNRF-%02X%02X", device_address[1], device_address[0]);
	sd[0].data_len = strlen(name);

	int err = bt_enable(NULL);
	if (err)
	{
		LOG_ERR("Bluetooth init failed: %d", err);
		set_status(SYS_STATUS_CONNECTION_ERROR, true);
		return;
	}
	bt_set_name(name);
	LOG_INF("BLE mode, advertising as %s", name);
	advertise();

	bool use_shutdown = CONFIG_0_SETTINGS_READ(CONFIG_0_USER_SHUTDOWN);
	int64_t last_connected = k_uptime_get();
	bool was_connected = true; // shows the advertising pattern on the first pass
	while (1)
	{
		bool is_connected = ble_connected();
		if (is_connected != was_connected)
			set_led(is_connected ? SYS_LED_PATTERN_ONESHOT_COMPLETE : SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);
		was_connected = is_connected;
		// On USB power the tracker could not wake up again (USB power never "appears" while the
		// cable stays plugged in, and it may have no button): never power off then
		bool on_usb_power = get_status(SYS_STATUS_USB_CONNECTED) || get_status(SYS_STATUS_PLUGGED);
		if (is_connected || on_usb_power)
			last_connected = k_uptime_get();
		else if (use_shutdown && k_uptime_get() - last_connected > CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_TIMEOUT_DELAY))
		{
			LOG_WRN("No BLE connection in %dm", CONFIG_3_SETTINGS_READ(CONFIG_3_CONNECTION_TIMEOUT_DELAY) / 60000);
			sys_request_system_off(false);
		}
		k_msleep(100);
	}
}

#endif
