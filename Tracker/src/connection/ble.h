#ifndef SLIMENRF_BLE
#define SLIMENRF_BLE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

// BLE connection mode (CONFIG_TRACKER_BLE): the connection_over_ble setting, read once at boot,
// picks ESB to the receiver or BLE to a PC (tools/ble_bridge.py). The radio runs one of them.

#if CONFIG_TRACKER_BLE
bool ble_mode(void); // this boot runs BLE (ESB stays off)
void ble_write(const uint8_t *data); // a packet from connection_thread: 16 bytes, sequence at [20]
int ble_write_config(const uint8_t *data, uint8_t len); // a settings answer (remote_config.c)
bool ble_connected(void);
void ble_print_status(void);
void ble_set_mode(bool on); // stores the setting and reboots when the mode changes
#else
static inline bool ble_mode(void) { return false; }
static inline void ble_write(const uint8_t *data) {}
static inline bool ble_connected(void) { return false; }
static inline int ble_write_config(const uint8_t *data, uint8_t len) { return -ENOTSUP; }
#endif

#endif
