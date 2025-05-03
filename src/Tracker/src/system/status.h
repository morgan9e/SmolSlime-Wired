#ifndef SLIMENRF_SYSTEM_STATUS
#define SLIMENRF_SYSTEM_STATUS

enum sys_status {
	SYS_STATUS_SENSOR_ERROR = 1,
	SYS_STATUS_CONNECTION_ERROR = 2,
	SYS_STATUS_SYSTEM_ERROR = 4,
	SYS_STATUS_USB_CONNECTED = 8,
	SYS_STATUS_PLUGGED = 16,
	SYS_STATUS_CALIBRATION_RUNNING = 32
};

void set_status(enum sys_status status, bool set);

bool status_ready(void);

#endif