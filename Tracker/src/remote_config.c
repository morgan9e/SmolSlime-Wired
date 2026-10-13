#include "globals.h"
#include "remote_config.h"

#if CONFIG_TRACKER_REMOTE_CONFIG

#include "ota.h"
#include "ota_proto.h"
#include "connection/esb.h"

#include <string.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(remote_config, LOG_LEVEL_INF);

#define RESP_REPEATS 3 // the receiver needs one; the request keeps coming until it has it

static uint8_t request[CFG_MSG_LEN]; // taken in the ESB event handler, carried out by the thread
static uint8_t response[CFG_MSG_LEN];
static bool answered; // response holds the answer to the request with seq response[2]
static atomic_t busy; // a request is waiting for the thread or being carried out
static K_SEM_DEFINE(request_sem, 0, 1);

void remote_config_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id)
{
	if (len < CFG_MSG_LEN || data[0] != CFG_MSG_REQ || data[1] != tracker_id)
		return;
	if (!atomic_cas(&busy, 0, 1))
		return; // the receiver sends it again with the next ACK
	memcpy(request, data, CFG_MSG_LEN);
	k_sem_give(&request_sem);
}

static int32_t get_i32(const uint8_t *p)
{
	return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}

// The console's setting id (list_config order) as its group and index in the group
static bool find_setting(uint16_t id, int *group, int *index)
{
	for (int i = 0; i < CONFIG_SETTINGS_COUNT; i++) {
		if (id < config_settings_count[i]) {
			*group = i;
			*index = id;
			return true;
		}
		id -= config_settings_count[i];
	}
	return false;
}

static int32_t read_setting(int group, int index)
{
	switch (group) {
	case 0:
		return CONFIG_0_SETTINGS_READ(index) ? 1 : 0;
	case 1:
		return CONFIG_1_SETTINGS_READ(index) ? 1 : 0;
	case 2:
		return CONFIG_2_SETTINGS_READ(index);
	default:
		return CONFIG_3_SETTINGS_READ(index);
	}
}

// The same checks as the console's write_config
static uint8_t write_setting(int group, int index, int32_t value)
{
	switch (group) {
	case 0:
	case 1:
		if (value < 0 || value > 1)
			return CFG_STATUS_BAD_VALUE;
		if (group == 0)
			config_0_settings_write(index, value);
		else
			config_1_settings_write(index, value);
		return CFG_STATUS_OK;
	case 2:
		if (value < INT16_MIN || value > INT16_MAX)
			return CFG_STATUS_BAD_VALUE;
		config_2_settings_write(index, value);
		return CFG_STATUS_OK;
	default:
		config_3_settings_write(index, value);
		return CFG_STATUS_OK;
	}
}

static uint8_t carry_out(uint8_t op, uint16_t id, int32_t *value)
{
	int group, index;
	if (ota_active())
		return CFG_STATUS_BUSY;
	if (!find_setting(id, &group, &index))
		return CFG_STATUS_BAD_ID;
	uint8_t status = CFG_STATUS_OK;
	switch (op) {
	case CFG_OP_READ:
		break;
	case CFG_OP_WRITE:
		status = write_setting(group, index, *value);
		break;
	case CFG_OP_RESET:
		config_settings_reset(group, index);
		break;
	default:
		return CFG_STATUS_BAD_OP;
	}
	if (status == CFG_STATUS_OK)
		*value = read_setting(group, index);
	return status;
}

static void remote_config_thread(void)
{
	while (1) {
		k_sem_take(&request_sem, K_FOREVER);
		uint8_t seq = request[2];
		if (!answered || response[2] != seq) {
			// A new request: carry it out once. A repeat of the last one (the receiver did not
			// get the answer yet) is only answered again.
			uint8_t op = request[3];
			uint16_t id = request[4] | request[5] << 8;
			int32_t value = get_i32(&request[6]);
			uint8_t status = carry_out(op, id, &value);
			if (status != CFG_STATUS_OK)
				value = 0;
			response[0] = CFG_MSG_RESP;
			response[1] = request[1];
			response[2] = seq;
			response[3] = status;
			response[4] = id;
			response[5] = id >> 8;
			ota_put_u32(&response[6], (uint32_t)value);
			answered = status != CFG_STATUS_BUSY; // busy is not an answer to keep
			if (status == CFG_STATUS_OK)
				LOG_INF("Setting %u %s over the radio: %d", id,
					op == CFG_OP_WRITE ? "written" : op == CFG_OP_RESET ? "reset" : "read", value);
			else
				LOG_WRN("Setting %u over the radio: %s", id, cfg_status_name(status));
		}
		for (int i = 0; i < RESP_REPEATS; i++) {
			esb_write_ota(response, sizeof(response));
			k_msleep(5);
		}
		atomic_set(&busy, 0);
	}
}

// Its own stack: writing a setting goes to flash
K_THREAD_DEFINE(remote_config_thread_id, 1536, remote_config_thread, NULL, NULL, NULL, 7, 0, 0);

#endif
