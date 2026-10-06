#ifndef BI2X_LINK_H
#define BI2X_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "bi2x_frame.h"
#include "bi2x_port.h"

/* Request/response transport over the BI2X USB CDC port
   (replaces AIO_SCI_COMM + AIO_NMGR_IOB2_COMM). */

struct bi2x_link {
    struct bi2x_serial *serial;
    char port[512];
    struct bi2x_cipher cipher;
    struct bi2x_frame_decoder dec;
    uint8_t tag;
    uint8_t rx[512];
    int rx_len;
    int rx_pos;
    uint32_t timeouts;
    uint32_t tx_frames;
};

enum {
    BI2X_OK = 0,
    BI2X_ERR_TIMEOUT = -1,
    BI2X_ERR_IO = -2,
    BI2X_ERR_PROTO = -3,
};

/* Open the port (retrying for up to wait_ms). port NULL -> autodetect. */
int bi2x_link_open(struct bi2x_link *link, const char *port, unsigned int wait_ms);
void bi2x_link_close(struct bi2x_link *link);

/* Close and reopen the same port, retrying for up to wait_ms. */
int bi2x_link_reopen(struct bi2x_link *link, unsigned int wait_ms);

/* Hardware reset through a line break, as done by AIO_NMGR_IOB2_COMM at
   start-up: break for 2.75 s, release, let the board settle for 5.5 s. The
   board may drop off USB while this happens; the port is reopened. */
int bi2x_link_reset_board(struct bi2x_link *link);

/* Wait for the board to answer an empty frame, then for the line to go
   quiet (AIO_NMGR_IOB2_COMM start-up state 2). */
int bi2x_link_sync(struct bi2x_link *link, unsigned int timeout_ms);

/* CRC bit order discovery (see bi2x_crc.h). Sync probes the header CRC-4
   order until the board answers; bi2x_link_transact_probe() does the same
   for the data CRC-7 using a real request. Remembered across reconnects. */
int bi2x_link_crc_known(int which /* 4 or 7 */);

/* Like bi2x_link_transact, but if the data CRC order is not known yet and
   the request times out, retry once with the other CRC-7 bit order. */
int bi2x_link_transact_probe(
    struct bi2x_link *link,
    uint8_t node,
    const uint8_t *req,
    uint32_t req_len,
    uint8_t *resp,
    uint32_t resp_cap,
    uint32_t *resp_len,
    uint32_t expect_len,
    unsigned int proc_ms,
    int attempts);

/* Receive the next deliverable frame, up to timeout_ms. */
int bi2x_link_recv(
    struct bi2x_link *link, struct bi2x_packet **pkt, unsigned int timeout_ms);

/*
 * Send a request to a node and wait for the matching response (same node and
 * tag). expect_len is the maximum response size and proc_ms the node's
 * processing allowance, as passed to AIO_NMGR_IOB::PacketSendRequest; both
 * only feed into the timeout. The response payload is copied to resp.
 */
int bi2x_link_transact(
    struct bi2x_link *link,
    uint8_t node,
    const uint8_t *req,
    uint32_t req_len,
    uint8_t *resp,
    uint32_t resp_cap,
    uint32_t *resp_len,
    uint32_t expect_len,
    unsigned int proc_ms,
    int attempts);

#endif
