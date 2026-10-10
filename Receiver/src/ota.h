#ifndef SLIMENRF_OTA
#define SLIMENRF_OTA

#include <esb.h>

// Tracker firmware updates over the radio, the receiver's part (docs/OTA.md, ota_proto.h):
// the PC stores an image in this receiver's flash over the console ("ota" commands, see
// tools/ota.py), then the receiver hands it to one tracker at a time.

// The "ota" console command (argv[0] is "ota")
void ota_console(int argc, char **argv);

// ESB event handler, interrupt context: a frame on the OTA pipe (requests and results from the
// tracker being updated), and every tracker frame on pipe 1 (keeps an update offer queued).
void ota_esb_rx(const struct esb_payload *payload);
void ota_esb_tracker_frame(void);

#endif
