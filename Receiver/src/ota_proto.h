#ifndef SLIMENRF_OTA_PROTO
#define SLIMENRF_OTA_PROTO

// Firmware updates over the radio, the messages between receiver and tracker (docs/OTA.md).
// The same file is in Tracker/src and Receiver/src. Multi-byte fields are little-endian.
//
// The receiver (ESB PRX) only answers: everything it sends rides on the ACK of a tracker frame.
//  1. OFFER, ACK payload on pipe 1 (all trackers send there): the addressed tracker takes it, the
//     others ignore it. The receiver keeps one queued while offering. A tracker that cannot take
//     it answers DONE with the reason and keeps tracking.
//  2. The tracker pauses tracking and sends REQ(position) on pipe 7, only it uses that pipe.
//  3. The receiver queues exactly the chunk asked for; it rides on the ACK of the tracker's next
//     REQ, and the tracker asks again as soon as a frame is done without it. The tracker owns the
//     offset: a lost chunk is simply asked for again.
//  4. After the last chunk the tracker checks the CRC32, sends DONE and reboots into the new image
//     (MCUboot test swap; the image confirms itself once the receiver hears it).

#define OTA_PIPE 7

#define OTA_MSG_OFFER 0xF1
#define OTA_MSG_CHUNK 0xF2
#define OTA_MSG_REQ 0xF3
#define OTA_MSG_DONE 0xF4

// Bumped on any change to the messages below. The OFFER carries it in byte 2, and the tracker
// answers an offer it cannot take with DONE, which carries its own version: the start of OFFER
// ([type][tracker id][protocol]) and all of DONE stay as they are in every version.
#define OTA_PROTOCOL 1

// OFFER   receiver -> tracker, ACK payload on pipe 1:
//         [type][tracker id][protocol][session][size u32][crc32 u32][board crc32 u32]
// REQ     tracker -> receiver, pipe 7: [type][tracker id][position u24]
// CHUNK   receiver -> tracker, ACK payload on pipe 7: [type][position u24][data]
// DONE    tracker -> receiver, pipe 7: [type][tracker id][status][protocol]
// position = session << 20 | offset: the session (1 - 15, new with every offer) keeps a chunk or
// request left over from an earlier, aborted update from being taken for one of this update.
#define OTA_OFFER_LEN 16
#define OTA_OFFER_MIN_LEN 3 // enough to read the protocol of an offer of any version
#define OTA_REQ_LEN 5
#define OTA_DONE_LEN 4
#define OTA_CHUNK_HEADER 4
#define OTA_CHUNK_MAX 28 // ESB payloads are at most 32 bytes (CONFIG_ESB_MAX_PAYLOAD_LENGTH)
#define OTA_OFFSET_BITS 20
#define OTA_OFFSET_MASK ((1u << OTA_OFFSET_BITS) - 1) // images up to 1 MB
#define OTA_SESSIONS 15

enum ota_status {
	OTA_STATUS_INSTALLED = 0,
	OTA_STATUS_FLASH_ERROR = 1,
	OTA_STATUS_CRC_ERROR = 2,
	OTA_STATUS_TIMEOUT = 3,
	// refused offers: the tracker keeps tracking with its image
	OTA_STATUS_PROTOCOL = 4,     // another protocol version (DONE says which)
	OTA_STATUS_WRONG_BOARD = 5,  // image for another board
	OTA_STATUS_TOO_LARGE = 6,    // does not fit the secondary slot
	OTA_STATUS_PENDING = 7,      // the running image is not confirmed yet: slot 1 holds its fallback
	OTA_STATUS_SAME_IMAGE = 8,   // already running this image
};

static inline const char *ota_status_name(uint8_t status)
{
	static const char *const names[] = {
		"installed", "flash error", "transfer CRC mismatch", "transfer timed out",
		"other OTA protocol version", "image for another board", "image too large",
		"running image not confirmed yet", "already running this image",
	};
	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

static inline uint32_t ota_position(uint8_t session, uint32_t offset)
{
	return (uint32_t)session << OTA_OFFSET_BITS | (offset & OTA_OFFSET_MASK);
}

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
