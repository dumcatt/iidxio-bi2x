#ifndef BI2X_FRAME_H
#define BI2X_FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * IOB2 wire framing (AIO_NODE_IOB2_COMMPKT / AIO_NMGR_IOB2_COMM).
 *
 *   AA               sync (only ever appears raw at the start of a frame)
 *   addr             node << 1 | response
 *   tag              request tag, echoed by the response (never 00/AA/FF)
 *   len...           0-4 bytes of (0xC0 | 6 bits), then 1 byte of 7 bits
 *   flags            mode << 5 | encrypted << 4 | crc4(header)
 *   [subst]          mode 3 only: byte that stands in for AA in the data
 *   data...          len bytes, encoded according to mode
 *   crc7             only when len > 0, crc7 over the decoded data
 *
 * modes: 0 = AA/FF escaped as FF, ~b
 *        1 = discarded by the host (device keep-alive)
 *        2 = raw (data contains no AA)
 *        3 = AA replaced by the subst byte
 *        4 = LZ compressed (host -> device only, not used here)
 */

#define BI2X_NODE_MAX 64
#define BI2X_PAYLOAD_MAX 1024
#define BI2X_FRAME_MAX (BI2X_PAYLOAD_MAX * 2 + 16)

struct bi2x_cipher {
    uint8_t enabled[BI2X_NODE_MAX];
};

struct bi2x_packet {
    uint8_t node;
    uint8_t response;
    uint8_t tag;
    uint8_t mode;
    uint8_t encrypted;
    uint32_t len;
    uint8_t data[BI2X_PAYLOAD_MAX];
};

struct bi2x_frame_decoder {
    struct bi2x_cipher *cipher;
    int state;
    uint8_t hdr_addr;
    uint8_t hdr_tag;
    uint8_t len_count;
    uint32_t len;
    uint8_t flags;
    uint8_t subst;
    uint8_t escape;
    uint8_t decrypting;
    uint32_t key;
    uint32_t pos;
    struct bi2x_packet pkt;
    /* statistics */
    uint32_t bytes_discarded;
    uint32_t frames_bad;
    uint32_t frames_ok;
};

/* Keystream cipher used when a frame has the "encrypted" flag set. */
uint8_t bi2x_cipher_byte(uint32_t *state, uint8_t b);

/* Header checksum as computed by AIO_NODE_IOB2_COMMPKT::QueryHeadSum. */
uint8_t bi2x_frame_head_sum(
    uint8_t addr, uint8_t tag, uint8_t len_count, uint32_t len, uint8_t flags);

/* Encode a host -> device frame. Returns the number of bytes written to
   out, or 0 if the payload is too large. out must hold BI2X_FRAME_MAX. */
size_t bi2x_frame_encode(
    struct bi2x_cipher *cipher,
    uint8_t node,
    uint8_t response,
    uint8_t tag,
    const uint8_t *data,
    uint32_t len,
    uint8_t *out,
    size_t out_cap);

void bi2x_frame_decoder_init(
    struct bi2x_frame_decoder *dec, struct bi2x_cipher *cipher);

/* Feed one received byte. Returns 1 when dec->pkt holds a complete,
   checksum-verified frame that should be delivered, 0 otherwise. Frames the
   host is supposed to drop (mode 1, 00 7F notifications) are not
   delivered. */
int bi2x_frame_decoder_feed(struct bi2x_frame_decoder *dec, uint8_t byte);

#endif
