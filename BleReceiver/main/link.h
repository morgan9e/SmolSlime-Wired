// The receiver's USB side: a text line link to the PC on every board, plus (ESP32-S3) a USB HID
// interface that looks like the nRF receiver's, so SlimeVR Server reads the trackers directly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

void link_init(void);

// One whole text line to the PC; dropped while the PC is not reading (BLE never waits on USB)
void link_write(const char *line, size_t len);

// Command bytes from the PC
int link_read(uint8_t *buf, size_t len, TickType_t wait);

// Reboot into the chip's ROM download mode (esptool), where nothing else gets there
void link_download_mode(void);

// USB HID like the nRF receiver (link_usb.c); without it (link_jtag.c) these do nothing
extern const bool link_hid;
// A tracker packet: 16 bytes with its tracker id at [1] and RSSI (-dBm) at [15], as the nRF
// receiver passes it on
void link_hid_packet(const uint8_t packet[16]);
// The known trackers by id: device addresses (little endian), sent as address packets (type 255)
void link_hid_trackers(const uint8_t (*address)[6], int count);
