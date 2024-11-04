#ifndef SLIMENRF_HID
#define SLIMENRF_HID

#include <stdint.h>

void hid_write_packet_n(uint8_t *data, uint8_t rssi);

#endif