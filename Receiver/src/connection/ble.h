#ifndef SLIMENRF_RECEIVER_BLE
#define SLIMENRF_RECEIVER_BLE

#include <stdbool.h>
#include <stdint.h>

// BLE receiver mode (CONFIG_RECEIVER_BLE): the receiver_ble setting, read once at boot, picks ESB
// or BLE for the radio. In BLE mode the receiver is a BLE central for trackers in BLE mode
// (Tracker/src/connection/ble.c) and hands their packets to the same USB HID path as ESB.

#if CONFIG_RECEIVER_BLE
bool ble_mode(void); // this boot runs BLE (ESB stays off)
void ble_start(void); // BLE mode: the paired list is loaded, start scanning (esb_thread)
void ble_set_mode(bool on); // stores the setting and reboots when the mode changes
void ble_print_status(void);
void ble_disconnect_all(void); // the paired list changed: trackers get their ids anew
int ble_write_config(uint8_t tracker_id, const uint8_t *data, uint8_t len); // settings request (remote_config.c)
#else
static inline bool ble_mode(void) { return false; }
static inline void ble_start(void) {}
static inline void ble_disconnect_all(void) {}
static inline int ble_write_config(uint8_t tracker_id, const uint8_t *data, uint8_t len) { return -1; }
#endif

#endif
