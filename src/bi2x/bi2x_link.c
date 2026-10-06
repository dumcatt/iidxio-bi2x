#include <string.h>

#include "bi2x_crc.h"
#include "bi2x_link.h"
#include "bi2x_log.h"

#define RESET_BREAK_MS 2750
#define RESET_SETTLE_MS 5500
#define SYNC_INTERVAL_MS 20
#define SYNC_QUIET_MS 500

static int crc4_known;
static int crc7_known;

int bi2x_link_crc_known(int which)
{
    return which == 4 ? crc4_known : crc7_known;
}

static void log_crc_order(void)
{
    int o4;
    int o7;

    bi2x_crc_get_order(&o4, &o7);

    if (o4 != BI2X_CRC_LSB_FIRST || o7 != BI2X_CRC_LSB_FIRST) {
        bi2x_warn(
            "board uses %s-first CRC-4 / %s-first CRC-7 framing checksums; "
            "please report this so the default can be fixed",
            o4 == BI2X_CRC_MSB_FIRST ? "MSB" : "LSB",
            o7 == BI2X_CRC_MSB_FIRST ? "MSB" : "LSB");
    }
}

static uint8_t next_tag(struct bi2x_link *link)
{
    /* AIO_NMGR_IOB::GetTag skips 00, AA and FF */
    do {
        link->tag++;
    } while (link->tag == 0x00 || link->tag == 0xAA || link->tag == 0xFF);

    return link->tag;
}

static void reset_rx(struct bi2x_link *link)
{
    link->rx_len = 0;
    link->rx_pos = 0;
    bi2x_frame_decoder_init(&link->dec, &link->cipher);
}

int bi2x_link_open(struct bi2x_link *link, const char *port, unsigned int wait_ms)
{
    uint64_t deadline = bi2x_time_ms() + wait_ms;

    memset(link, 0, sizeof(*link));

    if (port != NULL) {
        strncpy(link->port, port, sizeof(link->port) - 1);
    }

    reset_rx(link);

    for (;;) {
        if (bi2x_serial_open(&link->serial, link->port) == 0) {
            return BI2X_OK;
        }

        if (bi2x_time_ms() >= deadline) {
            return BI2X_ERR_IO;
        }

        bi2x_sleep_ms(250);
    }
}

void bi2x_link_close(struct bi2x_link *link)
{
    if (link->serial != NULL) {
        bi2x_serial_close(link->serial);
        link->serial = NULL;
    }
}

int bi2x_link_reopen(struct bi2x_link *link, unsigned int wait_ms)
{
    uint64_t deadline = bi2x_time_ms() + wait_ms;

    bi2x_link_close(link);
    reset_rx(link);
    memset(&link->cipher, 0, sizeof(link->cipher));

    for (;;) {
        if (bi2x_serial_open(&link->serial, link->port) == 0) {
            return BI2X_OK;
        }

        if (bi2x_time_ms() >= deadline) {
            return BI2X_ERR_IO;
        }

        bi2x_sleep_ms(250);
    }
}

int bi2x_link_reset_board(struct bi2x_link *link)
{
    bi2x_info("resetting BI2X (line break)...");

    if (bi2x_serial_set_break(link->serial, 1) != 0) {
        if (bi2x_link_reopen(link, 10000) != BI2X_OK ||
            bi2x_serial_set_break(link->serial, 1) != 0) {
            bi2x_warn("could not assert break, skipping board reset");
            return BI2X_OK;
        }
    }

    bi2x_sleep_ms(RESET_BREAK_MS);

    if (bi2x_serial_set_break(link->serial, 0) != 0) {
        /* the board re-enumerated underneath us */
        bi2x_misc("port went away during reset, reopening");

        if (bi2x_link_reopen(link, 15000) != BI2X_OK) {
            bi2x_warn("BI2X did not come back after reset");
            return BI2X_ERR_IO;
        }
    }

    bi2x_sleep_ms(RESET_SETTLE_MS);
    bi2x_serial_purge(link->serial);
    reset_rx(link);

    return BI2X_OK;
}

static int send_frame(
    struct bi2x_link *link,
    uint8_t node,
    uint8_t tag,
    const uint8_t *data,
    uint32_t len)
{
    uint8_t buf[BI2X_FRAME_MAX];
    size_t n;

    n = bi2x_frame_encode(&link->cipher, node, 0, tag, data, len, buf, sizeof(buf));

    if (n == 0) {
        return BI2X_ERR_PROTO;
    }

    if (bi2x_serial_write(link->serial, buf, n) != 0) {
        return BI2X_ERR_IO;
    }

    link->tx_frames++;

    return BI2X_OK;
}

int bi2x_link_recv(
    struct bi2x_link *link, struct bi2x_packet **pkt, unsigned int timeout_ms)
{
    uint64_t deadline = bi2x_time_ms() + timeout_ms;

    for (;;) {
        uint64_t now;
        int n;

        while (link->rx_pos < link->rx_len) {
            if (bi2x_frame_decoder_feed(&link->dec, link->rx[link->rx_pos++])) {
                *pkt = &link->dec.pkt;
                return BI2X_OK;
            }
        }

        now = bi2x_time_ms();

        if (now >= deadline) {
            return BI2X_ERR_TIMEOUT;
        }

        n = bi2x_serial_read(
            link->serial, link->rx, sizeof(link->rx),
            (unsigned int) (deadline - now));

        if (n < 0) {
            return BI2X_ERR_IO;
        }

        link->rx_len = n;
        link->rx_pos = 0;
    }
}

int bi2x_link_sync(struct bi2x_link *link, unsigned int timeout_ms)
{
    uint64_t deadline = bi2x_time_ms() + timeout_ms;
    uint64_t quiet_until;
    int synced = 0;
    unsigned int attempt = 0;
    int o4;
    int o7;

    reset_rx(link);
    bi2x_crc_get_order(&o4, &o7);

    while (!synced) {
        uint8_t tag = next_tag(link);
        uint64_t wait_until;
        int rc;

        if (bi2x_time_ms() >= deadline) {
            return BI2X_ERR_TIMEOUT;
        }

        /* until the board has answered once, alternate the header CRC bit
           order between attempts */
        if (!crc4_known) {
            bi2x_crc_set_order((int) (attempt & 1), o7);
        }

        attempt++;

        rc = send_frame(link, 0, tag, NULL, 0);

        if (rc != BI2X_OK) {
            return rc;
        }

        wait_until = bi2x_time_ms() + SYNC_INTERVAL_MS;

        for (;;) {
            struct bi2x_packet *pkt;
            uint64_t now = bi2x_time_ms();

            if (now >= wait_until) {
                break;
            }

            rc = bi2x_link_recv(link, &pkt, (unsigned int) (wait_until - now));

            if (rc == BI2X_ERR_IO) {
                return rc;
            }

            if (rc == BI2X_OK && pkt->tag == tag && pkt->len == 0) {
                synced = 1;

                if (!crc4_known) {
                    crc4_known = 1;
                    log_crc_order();
                }
                break;
            }
        }
    }

    /* drain until the line has been quiet for 500 ms */
    quiet_until = bi2x_time_ms() + SYNC_QUIET_MS;

    while (bi2x_time_ms() < quiet_until) {
        uint8_t junk[64];
        int n = bi2x_serial_read(link->serial, junk, sizeof(junk), 20);

        if (n < 0) {
            return BI2X_ERR_IO;
        }

        if (n > 0) {
            quiet_until = bi2x_time_ms() + SYNC_QUIET_MS;
        }
    }

    reset_rx(link);

    return BI2X_OK;
}

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
    int attempts)
{
    /* wire time at 115200 8N1 is ~87 us/byte (plus escaping); USB CDC adds
       a millisecond or two. Be generous: a timeout only costs a retry. */
    unsigned int timeout_ms =
        40 + proc_ms + (unsigned int) ((req_len * 2 + expect_len + 32) / 10);
    int attempt;
    int rc = BI2X_ERR_TIMEOUT;

    for (attempt = 0; attempt < attempts; attempt++) {
        uint8_t tag = next_tag(link);
        uint64_t deadline;

        rc = send_frame(link, node, tag, req, req_len);

        if (rc != BI2X_OK) {
            return rc;
        }

        deadline = bi2x_time_ms() + timeout_ms;

        for (;;) {
            struct bi2x_packet *pkt;
            uint64_t now = bi2x_time_ms();

            if (now >= deadline) {
                rc = BI2X_ERR_TIMEOUT;
                break;
            }

            rc = bi2x_link_recv(link, &pkt, (unsigned int) (deadline - now));

            if (rc == BI2X_ERR_IO) {
                return rc;
            }

            if (rc != BI2X_OK) {
                continue;
            }

            if (pkt->tag != tag || pkt->node != node) {
                bi2x_misc(
                    "dropping unexpected frame node %u tag %02X len %u",
                    pkt->node, pkt->tag, (unsigned int) pkt->len);
                continue;
            }

            if (pkt->len > resp_cap) {
                return BI2X_ERR_PROTO;
            }

            memcpy(resp, pkt->data, pkt->len);
            *resp_len = pkt->len;

            return BI2X_OK;
        }

        link->timeouts++;
        bi2x_misc(
            "node %u: no response to %02X %02X (attempt %d)", node,
            req_len > 0 ? req[0] : 0, req_len > 1 ? req[1] : 0, attempt + 1);
    }

    return rc;
}

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
    int attempts)
{
    int o4;
    int o7;
    int rc;

    rc = bi2x_link_transact(
        link, node, req, req_len, resp, resp_cap, resp_len, expect_len,
        proc_ms, attempts);

    if (rc == BI2X_ERR_TIMEOUT && !crc7_known && req_len > 0) {
        bi2x_crc_get_order(&o4, &o7);
        bi2x_crc_set_order(o4, !o7);

        rc = bi2x_link_transact(
            link, node, req, req_len, resp, resp_cap, resp_len, expect_len,
            proc_ms, attempts);

        if (rc != BI2X_OK) {
            bi2x_crc_set_order(o4, o7);
        }
    }

    if (rc == BI2X_OK && !crc7_known && req_len > 0) {
        crc7_known = 1;
        log_crc_order();
    }

    return rc;
}
