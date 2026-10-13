#ifndef SLIMENRF_REMOTE_CONFIG
#define SLIMENRF_REMOTE_CONFIG

#include <stdbool.h>
#include <esb.h>

// Tracker settings over the radio, the receiver's part (ota_proto.h CFG_MSG_*, docs/OTA.md): the
// PC asks on the console ("cfg", tools/remote_config.py), the request rides on the ACKs of tracker
// frames until the tracker answers on the OTA pipe.

// The "cfg" console command (argv[0] is "cfg"): <tracker id> get|set|reset <setting id> [value],
// status.
void remote_config_console(int argc, char **argv);

// ESB event handler, interrupt context: an answer on the OTA pipe, and every tracker frame on
// pipe 1 (keeps the request queued while waiting).
void remote_config_esb_rx(const struct esb_payload *payload);
void remote_config_esb_tracker_frame(void);

// Waiting for an answer: no firmware update offer meanwhile, they share the ACKs on pipe 1
bool remote_config_busy(void);
// A firmware update starts: give up the request
void remote_config_cancel(void);

#endif
