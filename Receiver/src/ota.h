#ifndef SLIMENRF_OTA
#define SLIMENRF_OTA

#include <esb.h>

// Tracker firmware updates over the radio, the receiver's part (docs/OTA.md, ota_proto.h):
// the PC streams the image through this receiver's console ("ota" commands, tools/ota.py) into a
// RAM window, while the tracker fetches it over the radio.

// The "ota" console command (argv[0] is "ota")
void ota_console(int argc, char **argv);

// ESB event handler, interrupt context: a frame on the OTA pipe (requests and results from the
// tracker being updated), and every tracker frame on pipe 1 (keeps an update offer queued).
void ota_esb_rx(const struct esb_payload *payload);
void ota_esb_tracker_frame(void);

#endif
