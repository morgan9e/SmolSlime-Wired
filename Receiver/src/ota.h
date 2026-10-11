#ifndef SLIMENRF_OTA
#define SLIMENRF_OTA

#include <esb.h>

// Tracker firmware updates over the radio, the receiver's part (docs/OTA.md, ota_proto.h):
// the PC streams the image through this receiver's console ("ota" commands, tools/ota.py) into a
// RAM window, while the tracker fetches it over the radio.

// The "ota" console command (argv[0] is "ota"): begin, stop, status. The PC polls the status to
// keep within the window.
void ota_console(int argc, char **argv);

// Image data from the PC: a HID OUT report [OTA_HID_DATA][offset u24][length][data], 64 bytes.
// Only the next offset is taken, and only within the window; the PC sees from the status where
// to go on.
#define OTA_HID_DATA 0xF5
#define OTA_HID_HEADER 5
void ota_hid_data(const uint8_t *report, int len);

// ESB event handler, interrupt context: a frame on the OTA pipe (requests and results from the
// tracker being updated), and every tracker frame on pipe 1 (keeps an update offer queued).
void ota_esb_rx(const struct esb_payload *payload);
void ota_esb_tracker_frame(void);
bool ota_busy(void);
bool ota_sending(void);

#endif
