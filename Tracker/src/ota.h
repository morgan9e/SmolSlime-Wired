#ifndef SLIMENRF_OTA
#define SLIMENRF_OTA

// Firmware updates over the radio (CONFIG_TRACKER_OTA, MCUboot build), see docs/OTA.md.

#if CONFIG_TRACKER_OTA
// A frame was acknowledged by the receiver (ESB event handler, interrupt context). A new image
// confirms itself after CONFIG_TRACKER_OTA_CONFIRM_ACKS of them.
void ota_tx_acked(void);
#else
static inline void ota_tx_acked(void) {}
#endif

#endif
