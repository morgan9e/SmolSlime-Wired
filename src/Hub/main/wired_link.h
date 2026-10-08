// Wired chain link, tracker -> hub (the tracker side is src/connection/wired.c in src/Tracker).
// Each tracker in wired chain mode sends its own frames and forwards the ones from further
// down the chain, so one UART carries several trackers, told apart by address:
//     COBS( [type][body] ) + 0x00, 460800 baud 8N1
//     0x02 HELLO: address[6], fw major, minor, patch                     (once per second)
//     0x03 ADATA: address[6], packet[16], CRC32 LE, sequence
//                 CRC-32K/4.2: poly 0x93a409eb, MSB first, seed 0x93a409eb, over address + packet
// Portable: used by the UART task and by the host tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decoder.h"

#define WL_TYPE_HELLO 0x02
#define WL_TYPE_ADATA 0x03
#define WL_HELLO_LEN (1 + 6 + 3)
#define WL_ADATA_LEN (1 + 6 + HUB_PKT_LEN + 4 + 1)
#define WL_MAX_ENCODED 32 // an ADATA frame encodes to 29 bytes

#ifndef WL_MAX_IDS
#define WL_MAX_IDS 24
#endif

typedef enum {
	WL_NONE = 0, // byte consumed, no frame finished
	WL_HELLO,    // out->addr, out->fw valid
	WL_DATA,     // out->addr, out->pkt, out->seq valid
	WL_BAD,      // a frame finished but was malformed, of an unknown type, or failed its CRC
} wl_result_t;

typedef struct {
	uint8_t addr[6]; // tracker FICR device address, little-endian
	uint8_t fw[3];
	uint8_t pkt[HUB_PKT_LEN];
	uint8_t seq;
} wl_frame_t;

typedef struct {
	uint8_t buf[WL_MAX_ENCODED];
	size_t len;
	bool overlong;
	bool synced; // false until the first delimiter, so a partial first frame is not counted as bad
} wl_deframer_t;

void wl_reset(wl_deframer_t *d);
wl_result_t wl_push(wl_deframer_t *d, uint8_t c, wl_frame_t *out);

// CRC-32K/4.2 with the tracker's seed, as crc32_k_4_2_update(0x93a409eb, data, len).
uint32_t wl_crc32k(const uint8_t *data, size_t len);

// Wire frames with trailing 0x00, for tests and the simulator. out must hold 32 bytes.
size_t wl_encode_adata(const uint8_t addr[6], const uint8_t pkt[HUB_PKT_LEN], uint8_t seq, uint8_t *out);
size_t wl_encode_hello(const uint8_t addr[6], const uint8_t fw[3], uint8_t *out);

// Tracker ids for the hub, in order of first sight; stable until the hub restarts.
typedef struct {
	uint8_t addr[WL_MAX_IDS][6];
	uint8_t count;
} wl_table_t;

// Returns the id for addr, adding it if new (*added = true). -1 if the table is full.
int wl_table_id(wl_table_t *t, const uint8_t addr[6], bool *added);
