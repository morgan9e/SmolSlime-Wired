#ifndef SLIMENRF_OTA_PROTO
#define SLIMENRF_OTA_PROTO

// Firmware updates over the radio, the messages between receiver and tracker (docs/OTA.md).
// The same file is in Tracker/src and Receiver/src. Multi-byte fields are little-endian.
//
// The receiver (ESB PRX) only answers: everything it sends rides on the ACK of a tracker frame.
//  1. OFFER, ACK payload on pipe 1 (all trackers send there): the addressed tracker accepts, the
//     others ignore it. The receiver keeps one queued while offering.
//  2. The tracker pauses tracking and sends REQ(offset) on pipe 7, only it uses that pipe.
//  3. The ACK to each REQ carries a CHUNK; the receiver queues the next one from the offsets it
//     sees, so a clean link moves one chunk per frame. The tracker owns the offset: a lost chunk
//     is simply asked for again.
//  4. After the last chunk the tracker checks the CRC32, sends DONE and reboots into the new image
//     (MCUboot test swap; the image confirms itself once the receiver hears it).

#define OTA_PIPE 7

#define OTA_MSG_OFFER 0xF1 // receiver: [type][tracker id][size u32][crc32 u32][board crc32 u32]
#define OTA_MSG_CHUNK 0xF2 // receiver: [type][offset u24][data, 1-28 bytes]
#define OTA_MSG_REQ 0xF3   // tracker: [type][tracker id][offset u24]
#define OTA_MSG_DONE 0xF4  // tracker: [type][tracker id][status: 0 installed, else an error]

#define OTA_OFFER_LEN 14
#define OTA_REQ_LEN 5
#define OTA_DONE_LEN 3
#define OTA_CHUNK_HEADER 4
#define OTA_CHUNK_MAX 28 // ESB payloads are at most 32 bytes (CONFIG_ESB_MAX_PAYLOAD_LENGTH)

enum ota_status {
	OTA_STATUS_INSTALLED = 0,
	OTA_STATUS_FLASH_ERROR = 1,
	OTA_STATUS_CRC_ERROR = 2,
	OTA_STATUS_TIMEOUT = 3,
};

// The board an image is for: crc32_ieee of its CONFIG_BOARD_TARGET, e.g.
// "promicro_uf2/nrf52840/stackedspi". The tracker refuses an offer for another board.

static inline uint32_t ota_get_u32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void ota_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static inline uint32_t ota_get_u24(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16;
}

static inline void ota_put_u24(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
}

#endif
