//the only consideration right now is just everything in kconfig, and possibly some way to export/import a full configuration
//i do not want a json parser though

/*
menu "Status LED default status color"
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_R
    int "Red value"
    range 0 10000
    default 4000
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_G
    int "Green value"
    range 0 10000
    default 6000
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_B
    int "Blue value"
    range 0 10000
    default 0
    depends on LED_RGB_COLOR

endmenu

config USER_EXTRA_ACTIONS
    bool "Multiple press actions"

config IGNORE_RESET
    bool "Ignore reset"
    default y

config USER_SHUTDOWN
    bool "User shutdown support"
    default y

config USE_IMU_WAKE_UP
    bool "IMU wake up support"
    default y

config DELAY_SLEEP_ON_STATUS
    bool "Delay IMU wake up mode on status flags"
    default y
    depends on USE_IMU_WAKE_UP

config WOM_USE_DCDC
    bool "Use DCDC in IMU wake up mode"
    depends on USE_IMU_WAKE_UP

config CONNECTION_TIMEOUT_DELAY
    int "Connection timeout delay (ms)"
    default 300000
    depends on USER_SHUTDOWN

menu "Sensor power saving"

config SENSOR_LP_TIMEOUT
    int "Sensor low power delay (ms)"
    default 500

config SENSOR_USE_LOW_POWER_2
    bool "Use additional low power modes"

config USE_IMU_TIMEOUT
    bool "Use IMU wake up state"
    default y
    depends on USE_IMU_WAKE_UP

config IMU_TIMEOUT_RAMP_MIN
    int "Sensor timeout minimum delay (ms)"
    default 5000
    depends on USE_IMU_TIMEOUT || SENSOR_USE_LOW_POWER_2

config IMU_TIMEOUT_RAMP_MAX
    int "Sensor timeout maximum delay (ms)"
    default 15000
    depends on USE_IMU_TIMEOUT || SENSOR_USE_LOW_POWER_2

config USE_ACTIVE_TIMEOUT
    bool "Use activity timeout"
    default y
    depends on USE_IMU_WAKE_UP || USER_SHUTDOWN

choice
store as single int instead!
    prompt "Activity timeout mode"
    default SLEEP_ON_ACTIVE_TIMEOUT
    depends on USE_ACTIVE_TIMEOUT

config SLEEP_ON_ACTIVE_TIMEOUT
    bool "IMU wake up"
    depends on USE_IMU_WAKE_UP

config SHUTDOWN_ON_ACTIVE_TIMEOUT
    bool "User shutdown"
    depends on USER_SHUTDOWN

endchoice

config ACTIVE_TIMEOUT_THRESHOLD
    int "Activity timeout threshold (ms)"
    default 15000
    depends on USE_ACTIVE_TIMEOUT

config ACTIVE_TIMEOUT_DELAY
    int "Activity timeout delay (ms)"
    default 900000
    depends on USE_ACTIVE_TIMEOUT

endmenu

config SENSOR_ACCEL_ODR
    int "Accelerometer output data rate (Hz)"
    default 100

config SENSOR_GYRO_ODR
    int "Gyrometer output data rate (Hz)"
    default 200

config SENSOR_ACCEL_FS
    int "Accelerometer full scale (g)"
    default 4

config SENSOR_GYRO_FS
    int "Gyrometer full scale (dps)"
    default 1000

config SENSOR_USE_MAG
    bool "Magnetometer support"
    default y

config USE_SENSOR_CLOCK
    bool "Use external IMU clock"
    default y

choice
store as single int instead!
	prompt "Sensor fusion"
    default SENSOR_USE_VQF

config SENSOR_USE_XIOFUSION
    bool "Use x-io Technologies Fusion"

config SENSOR_USE_VQF
    bool "Use VQF"

endchoice

config SENSOR_USE_6_SIDE_CALIBRATION
    bool "Use 6-side calibration"
    default y
    depends on USE_SLIMENRF_CONSOLE

config RADIO_TX_POWER
    int "Radio output power (dBm)"
    default 8

config CONNECTION_OVER_HID
    bool "Use HID for data output"
    default n

*/

/*
schema:
storage is all in a 16bit array for ints
second binary array for bools

can address the config system by writing/reading words
for bools, similar but use 0/1? or true/false?

this has to be stored efficiently in retain, probably fixed array sizes
for nvs, the size can be variable depending on how many settings are known to the device
*/


/*
menu "Status LED default status color"
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_R
    int "Red value"
    range 0 10000
    default 4000
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_G
    int "Green value"
    range 0 10000
    default 6000
    depends on LED_RGB_COLOR

config LED_DEFAULT_COLOR_B
    int "Blue value"
    range 0 10000
    default 0
    depends on LED_RGB_COLOR

endmenu

config USER_EXTRA_ACTIONS
    bool "Multiple press actions"

config IGNORE_RESET
    bool "Ignore reset"
    default y

config USER_SHUTDOWN
    bool "User shutdown support"
    default y

config USE_IMU_WAKE_UP
    bool "IMU wake up support"
    default y

config DELAY_SLEEP_ON_STATUS
    bool "Delay IMU wake up mode on status flags"
    default y
    depends on USE_IMU_WAKE_UP

config WOM_USE_DCDC
    bool "Use DCDC in IMU wake up mode"
    depends on USE_IMU_WAKE_UP

config CONNECTION_TIMEOUT_DELAY
    int "Connection timeout delay (ms)"
    default 300000
    depends on USER_SHUTDOWN

menu "Sensor power saving"

config SENSOR_LP_TIMEOUT
    int "Sensor low power delay (ms)"
    default 500

config SENSOR_USE_LOW_POWER_2
    bool "Use additional low power modes"

config USE_IMU_TIMEOUT
    bool "Use IMU wake up state"
    default y
    depends on USE_IMU_WAKE_UP

config IMU_TIMEOUT_RAMP_MIN
    int "Sensor timeout minimum delay (ms)"
    default 5000
    depends on USE_IMU_TIMEOUT || SENSOR_USE_LOW_POWER_2

config IMU_TIMEOUT_RAMP_MAX
    int "Sensor timeout maximum delay (ms)"
    default 15000
    depends on USE_IMU_TIMEOUT || SENSOR_USE_LOW_POWER_2

config USE_ACTIVE_TIMEOUT
    bool "Use activity timeout"
    default y
    depends on USE_IMU_WAKE_UP || USER_SHUTDOWN

choice
store as single int instead!
    prompt "Activity timeout mode"
    default SLEEP_ON_ACTIVE_TIMEOUT
    depends on USE_ACTIVE_TIMEOUT

config SLEEP_ON_ACTIVE_TIMEOUT
    bool "IMU wake up"
    depends on USE_IMU_WAKE_UP

config SHUTDOWN_ON_ACTIVE_TIMEOUT
    bool "User shutdown"
    depends on USER_SHUTDOWN

endchoice

config ACTIVE_TIMEOUT_THRESHOLD
    int "Activity timeout threshold (ms)"
    default 15000
    depends on USE_ACTIVE_TIMEOUT

config ACTIVE_TIMEOUT_DELAY
    int "Activity timeout delay (ms)"
    default 900000
    depends on USE_ACTIVE_TIMEOUT

endmenu

config SENSOR_ACCEL_ODR
    int "Accelerometer output data rate (Hz)"
    default 100

config SENSOR_GYRO_ODR
    int "Gyrometer output data rate (Hz)"
    default 200

config SENSOR_ACCEL_FS
    int "Accelerometer full scale (g)"
    default 4

config SENSOR_GYRO_FS
    int "Gyrometer full scale (dps)"
    default 1000

config SENSOR_USE_MAG
    bool "Magnetometer support"
    default y

config USE_SENSOR_CLOCK
    bool "Use external IMU clock"
    default y

choice
store as single int instead!
	prompt "Sensor fusion"
    default SENSOR_USE_VQF

config SENSOR_USE_XIOFUSION
    bool "Use x-io Technologies Fusion"

config SENSOR_USE_VQF
    bool "Use VQF"

endchoice

config SENSOR_USE_6_SIDE_CALIBRATION
    bool "Use 6-side calibration"
    default y
    depends on USE_SLIMENRF_CONSOLE

config RADIO_TX_POWER
    int "Radio output power (dBm)"
    default 8

config CONNECTION_OVER_HID
    bool "Use HID for data output"
    default n

*/

/*
write/read to settings area performed as a write/read of arbitrary bytes (lol)
write byte <hex> <int>
write word <hex> <int>
write all <base64>
read byte <hex> -> <int>
read word <hex> -> <int>
read all -> <base64>
*/
