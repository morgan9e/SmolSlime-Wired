#include "config.h"

#include "system.h"

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(retainedmemdevice))
#define MEMORY_REGION DT_PARENT(DT_ALIAS(retainedmemdevice))
#else
#error "retained_mem region not defined"
#endif

#define RETAINED_SETTINGS_OFFSET offsetof(struct retained_data, settings)

struct config_settings_data *config_settings = (struct config_settings_data *)(DT_REG_ADDR(MEMORY_REGION) + RETAINED_SETTINGS_OFFSET);

const uint16_t config_settings_count[] = {
    CONFIG_0_SETTINGS_END,
    CONFIG_1_SETTINGS_END,
    CONFIG_2_SETTINGS_END,
    CONFIG_3_SETTINGS_END
};

const char *config_settings_names[] = {
    "SENSOR_USE_LOW_POWER_2", // 0
    "USE_IMU_TIMEOUT",
    "USE_ACTIVE_TIMEOUT",
    "SENSOR_USE_MAG",
    "USE_SENSOR_CLOCK",
    "SENSOR_USE_6_SIDE_CALIBRATION",
    "USER_EXTRA_ACTIONS", // 1
    "IGNORE_RESET",
    "USER_SHUTDOWN",
    "USE_IMU_WAKEUP",
    "DELAY_SLEEP_ON_STATUS",
    "CONNECTION_OVER_HID",
    "LED_DEFAULT_COLOR_R", // 2
    "LED_DEFAULT_COLOR_G",
    "LED_DEFAULT_COLOR_B",
    "ACTIVE_TIMEOUT_MODE",
    "SENSOR_ACCEL_ODR",
    "SENSOR_GYRO_ODR",
    "SENSOR_ACCEL_FS",
    "SENSOR_GYRO_FS",
    "SENSOR_FUSION",
    "RADIO_TX_POWER",
    "CONNECTION_TIMEOUT_DELAY", // 3
    "SENSOR_LP_TIMEOUT",
    "IMU_TIMEOUT_RAMP_MIN",
    "IMU_TIMEOUT_RAMP_MAX",
    "ACTIVE_TIMEOUT_THRESHOLD",
    "ACTIVE_TIMEOUT_DELAY",
};

const bool config_0_settings_defaults[16] = {
    IS_ENABLED(CONFIG_SENSOR_USE_LOW_POWER_2),
    IS_ENABLED(CONFIG_USE_IMU_TIMEOUT),
    IS_ENABLED(CONFIG_USE_ACTIVE_TIMEOUT),
    IS_ENABLED(CONFIG_SENSOR_USE_MAG),
    IS_ENABLED(CONFIG_USE_SENSOR_CLOCK),
    IS_ENABLED(CONFIG_SENSOR_USE_6_SIDE_CALIBRATION),
};

const bool config_1_settings_defaults[16] = {
    IS_ENABLED(CONFIG_USER_EXTRA_ACTIONS),
    IS_ENABLED(CONFIG_IGNORE_RESET),
    IS_ENABLED(CONFIG_USER_SHUTDOWN),
    IS_ENABLED(CONFIG_USE_IMU_WAKEUP),
    IS_ENABLED(CONFIG_DELAY_SLEEP_ON_STATUS),
    IS_ENABLED(CONFIG_CONNECTION_OVER_HID),
};

const int16_t config_2_settings_defaults[16] = {
#ifdef CONFIG_LED_DEFAULT_COLOR_R
    CONFIG_LED_DEFAULT_COLOR_R, // 0-10000
#else
	4000,
#endif
#ifdef CONFIG_LED_DEFAULT_COLOR_G
    CONFIG_LED_DEFAULT_COLOR_G, // 0-10000
#else
	6000,
#endif
#ifdef CONFIG_LED_DEFAULT_COLOR_B
    CONFIG_LED_DEFAULT_COLOR_B, // 0-10000
#else
	0,
#endif
#if CONFIG_SHUTDOWN_ON_ACTIVE_TIMEOUT
    1,
#else // CONFIG_SLEEP_ON_ACTIVE_TIMEOUT
	0,
#endif
    CONFIG_SENSOR_ACCEL_ODR, // 0-3200 (ex.)
    CONFIG_SENSOR_GYRO_ODR, // 0-3200 (ex.)
    CONFIG_SENSOR_ACCEL_FS, // 0-32000 (ex.)
    CONFIG_SENSOR_GYRO_FS, // 0-32000 (ex.)
#if CONFIG_SENSOR_USE_VQF
    3,
#elif CONFIG_SENSOR_USE_NXPSENSORFUSION
	2,
#elif CONFIG_SENSOR_USE_XIOFUSION
	1,
#else
	0,
#endif
    CONFIG_RADIO_TX_POWER, // -128-127
};

const int32_t config_3_settings_defaults[16] = {
#ifdef CONFIG_CONNECTION_TIMEOUT_DELAY
    CONFIG_CONNECTION_TIMEOUT_DELAY,
#else
	300000,
#endif
    CONFIG_SENSOR_LP_TIMEOUT,
#ifdef CONFIG_IMU_TIMEOUT_RAMP_MIN
    CONFIG_IMU_TIMEOUT_RAMP_MIN,
#else
	5000,
#endif
#ifdef CONFIG_IMU_TIMEOUT_RAMP_MAX
    CONFIG_IMU_TIMEOUT_RAMP_MAX,
#else
	15000,
#endif
#ifdef CONFIG_ACTIVE_TIMEOUT_THRESHOLD
    CONFIG_ACTIVE_TIMEOUT_THRESHOLD,
#else
	15000,
#endif
#ifdef CONFIG_ACTIVE_TIMEOUT_DELAY
    CONFIG_ACTIVE_TIMEOUT_DELAY,
#else
	900000,
#endif
};

/*
bool config_0_settings_read(uint16_t id)
{
    uint16_t read_mask = 1 << id;

    if (config_settings->config_0_ovrd & read_mask)
        return config_settings->config_0_settings & read_mask;

    // Otherwise use default config value
    return config_0_settings_defaults[id];
}

bool config_1_settings_read(uint16_t id)
{
    uint16_t read_mask = 1 << id;

    if (config_settings->config_1_ovrd & read_mask)
        return config_settings->config_1_settings & read_mask;

    // Use default config value
    return config_1_settings_defaults[id];
}

int16_t config_2_settings_read(uint16_t id)
{
    uint16_t read_mask = 1 << id;

    if (config_settings->config_2_ovrd & read_mask)
        return config_settings->config_2_settings[id];

    // Use default config value
    return config_2_settings_defaults[id];
}

int32_t config_3_settings_read(uint16_t id)
{
    uint16_t read_mask = 1 << id;

    if (config_settings->config_3_ovrd & read_mask)
        return config_settings->config_3_settings[id];

    // Use default config value
    return config_3_settings_defaults[id];
}
*/

void config_0_settings_write(uint16_t id, bool value)
{
    uint16_t write_mask = 1 << id;

    config_settings->config_0_ovrd |= write_mask;

    if (value)
        config_settings->config_0_settings ^= write_mask;
    else
        config_settings->config_0_settings &= ~write_mask;

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
}

void config_1_settings_write(uint16_t id, bool value)
{
    uint16_t write_mask = 1 << id;

    config_settings->config_1_ovrd |= write_mask;

    if (value)
        config_settings->config_1_settings |= write_mask;
    else
        config_settings->config_1_settings &= ~write_mask;

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
}

void config_2_settings_write(uint16_t id, int16_t value)
{
    uint16_t write_mask = 1 << id;

    config_settings->config_2_ovrd |= write_mask;

    config_settings->config_2_settings[id] = value;

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
}

void config_3_settings_write(uint16_t id, int32_t value)
{
    uint16_t write_mask = 1 << id;

    config_settings->config_3_ovrd |= write_mask;

    config_settings->config_3_settings[id] = value;

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
}

void config_settings_reset(uint16_t config, uint16_t id)
{
    uint16_t write_mask = 1 << id;

    switch (config)
    {
    case 0:
        config_settings->config_0_ovrd &= ~write_mask;
        break;
    case 1:
        config_settings->config_1_ovrd &= ~write_mask;
        break;
    case 2:
        config_settings->config_2_ovrd &= ~write_mask;
        break;
    case 3:
        config_settings->config_3_ovrd &= ~write_mask;
        break;
    default:
    }

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
}

void config_settings_reset_all(void)
{
	static bool reset_confirm = false;
	if (!reset_confirm)
	{
		printk("Resetting device settings will clear all user-defined settings to the firmware defaults. Are you sure?\n");
		reset_confirm = true;
		return;
	}
	printk("Resetting device settings\n");

    config_settings->config_0_ovrd = 0;
    config_settings->config_1_ovrd = 0;
    config_settings->config_2_ovrd = 0;
    config_settings->config_3_ovrd = 0;

    sys_write(SETTINGS_ID, NULL, retained->settings, sizeof(retained->settings));
	reset_confirm = false;
    LOG_INF("Device settings reset");
}
