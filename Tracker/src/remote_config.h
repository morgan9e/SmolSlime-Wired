#ifndef SLIMENRF_REMOTE_CONFIG
#define SLIMENRF_REMOTE_CONFIG

#include <stdint.h>

// Tracker settings read and written over the radio, through the receiver (CONFIG_TRACKER_REMOTE_CONFIG,
// ota_proto.h CFG_MSG_*, docs/OTA.md).

#if CONFIG_TRACKER_REMOTE_CONFIG
// An ACK payload from the receiver while paired (ESB event handler, interrupt context): takes a
// settings request addressed to this tracker; the work is done on its own thread.
void remote_config_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id);
#else
static inline void remote_config_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id)
{
	(void)data;
	(void)len;
	(void)tracker_id;
}
#endif

#endif
