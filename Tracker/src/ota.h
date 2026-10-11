#ifndef SLIMENRF_OTA
#define SLIMENRF_OTA

#include <stdbool.h>
#include <stdint.h>

// Firmware updates over the radio (CONFIG_TRACKER_OTA, MCUboot build), see docs/OTA.md and
// ota_proto.h.

#if CONFIG_TRACKER_OTA
// A frame was acknowledged by the receiver (ESB event handler, interrupt context). A new image
// confirms itself after CONFIG_TRACKER_OTA_CONFIRM_ACKS of them.
void ota_tx_acked(void);
// An ACK payload from the receiver while paired (ESB event handler): update offers and chunks.
void ota_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id);
// A frame sent during an update is done, acknowledged or not (ESB event handler)
void ota_tx_done(void);
// An update is being received: tracking is paused (esb_write sends nothing) and the radio clock
// stays on.
bool ota_active(void);
bool ota_image_confirmed(void);
// "Image: confirmed" or "Image: pending (...)", MCUboot's swap state and a running update, for
// the console's info.
void ota_print_status(void);
#else
static inline void ota_tx_acked(void) {}
static inline void ota_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id)
{
	(void)data;
	(void)len;
	(void)tracker_id;
}
static inline void ota_tx_done(void) {}
static inline bool ota_active(void) { return false; }
static inline bool ota_image_confirmed(void) { return false; }
static inline void ota_print_status(void) {}
#endif

#endif
