#include <stdlib.h>
#include <string.h>

#include "bi2x/bi2x_log.h"
#include "relay/relay_board.h"

#define SOF 0xAA
#define ESCAPE 0xFF
#define DEFAULT_INTERVAL_MS 8
#define REOPEN_INTERVAL_MS 2000
#define ANSWER_CHECK_MS 2000
#define REPLY_DATA_SIZE 13
#define REPLY_FADER_OFFSET 6
#define FADER_STEPS 16

/* Register bit of each spotlight, left to right (same order as MAME's twinkle
   driver; not yet checked on a cabinet one spotlight at a time) */
static const uint8_t spotlight_bits[8] = {3, 2, 1, 0, 4, 5, 6, 7};

struct reply_parser {
    uint8_t body[64];
    size_t len;
    int in_frame;
    int escaped;
};

struct relay_board {
    struct relay_board_config cfg;
    char port[256];
    struct bi2x_thread *thread;
    struct bi2x_mutex *lock;
    volatile int stop;

    /* guarded by lock */
    char ticker[RELAY_BOARD_TICKER_SIZE];
    uint8_t spotlights;
    uint8_t neon;
    uint8_t panel_lamps;
    int fader_steps[RELAY_BOARD_FADERS]; /* -1 until the first reply */

    /* thread only */
    struct reply_parser parser;
    uint8_t last_reply[REPLY_DATA_SIZE];
    unsigned long replies;
};

static size_t build_request(struct relay_board *rb, uint8_t *out)
{
    uint8_t body[5 + 12] = {0x00, 0x01, 0x12, 0x00, 0x0C};
    uint8_t *data = body + 5;
    uint8_t raw = 0;
    uint8_t checksum = 0;
    size_t n = 0;
    size_t i;

    bi2x_mutex_lock(rb->lock);

    /* the BIO2 sends 1F with every lamp off: 5 lamp bits, the top 3 held
       low */
    data[0] = (uint8_t) (0x1F & ~(rb->cfg.panel_lamps ? rb->panel_lamps : 0));

    for (i = 0; i < RELAY_BOARD_TICKER_SIZE; i++) {
        data[1 + i] = (uint8_t) ~(uint8_t) rb->ticker[i];
    }

    for (i = 0; i < 8; i++) {
        if (rb->spotlights & (1 << i)) {
            raw |= (uint8_t) (1 << spotlight_bits[i]);
        }
    }

    data[10] = (uint8_t) ~raw;
    data[11] = (uint8_t) ~(rb->neon & 0x07);

    bi2x_mutex_unlock(rb->lock);

    out[n++] = SOF;

    for (i = 0; i <= sizeof(body); i++) {
        uint8_t value;

        if (i < sizeof(body)) {
            value = body[i];
            checksum = (uint8_t) (checksum + value);
        } else {
            value = checksum;
        }

        if (value == SOF || value == ESCAPE) {
            out[n++] = ESCAPE;
            out[n++] = (uint8_t) ~value;
        } else {
            out[n++] = value;
        }
    }

    return n;
}

/* Returns 1 when a complete, checksum-valid reply is in p->body */
static int parser_feed(struct reply_parser *p, uint8_t value)
{
    uint8_t checksum = 0;
    size_t i;

    if (value == SOF) {
        /* start (or restart) of a frame; AAs never appear inside one */
        p->len = 0;
        p->in_frame = 1;
        p->escaped = 0;
        return 0;
    }

    if (!p->in_frame) {
        return 0;
    }

    if (p->escaped) {
        value = (uint8_t) ~value;
        p->escaped = 0;
    } else if (value == ESCAPE) {
        p->escaped = 1;
        return 0;
    }

    p->body[p->len++] = value;

    if (p->len >= 5 && p->len == (size_t) (6 + p->body[4])) {
        p->in_frame = 0;

        for (i = 0; i + 1 < p->len; i++) {
            checksum = (uint8_t) (checksum + p->body[i]);
        }

        return checksum == p->body[p->len - 1] && p->body[0] == 0x80 &&
               p->body[1] == 0x01 && p->body[2] == 0x12 &&
               p->body[4] >= REPLY_FADER_OFFSET + RELAY_BOARD_FADERS;
    }

    if (p->len >= sizeof(p->body)) {
        p->in_frame = 0;
    }

    return 0;
}

static void handle_reply(struct relay_board *rb)
{
    const uint8_t *data = rb->parser.body + 5;
    size_t data_len = rb->parser.len - 6;
    unsigned int i;

    bi2x_mutex_lock(rb->lock);

    /* raw fader byte 0x01..0xFF to a step 0..15 */
    for (i = 0; i < RELAY_BOARD_FADERS; i++) {
        rb->fader_steps[i] = data[REPLY_FADER_OFFSET + i] / FADER_STEPS;
    }

    bi2x_mutex_unlock(rb->lock);

    if (data_len > REPLY_DATA_SIZE) {
        data_len = REPLY_DATA_SIZE;
    }

    /* the bytes around the faders are not identified yet (probably the
       effector buttons); log them when they change to help with that */
    for (i = 0; i < data_len; i++) {
        if (i >= REPLY_FADER_OFFSET &&
            i < REPLY_FADER_OFFSET + RELAY_BOARD_FADERS) {
            continue;
        }

        if (rb->replies > 0 && data[i] != rb->last_reply[i]) {
            bi2x_misc(
                "relay: reply byte %u %02X -> %02X", i, rb->last_reply[i],
                data[i]);
        }
    }

    memcpy(rb->last_reply, data, data_len);
    rb->replies++;
}

static int relay_thread(void *ctx)
{
    struct relay_board *rb = ctx;
    struct bi2x_serial *serial = NULL;
    unsigned int interval =
        rb->cfg.interval_ms ? rb->cfg.interval_ms : DEFAULT_INTERVAL_MS;
    uint64_t next_open = 0;
    uint64_t last_check = 0;
    unsigned long replies_at_check = 0;
    int answering = 1;
    int open_failed_logged = 0;

    while (!rb->stop) {
        uint8_t frame[64];
        uint8_t buf[256];
        uint64_t now = bi2x_time_ms();
        uint64_t next_send;
        size_t len;

        if (serial == NULL) {
            if (now < next_open) {
                bi2x_sleep_ms(50);
                continue;
            }

            if (bi2x_serial_open(&serial, rb->port) != 0) {
                serial = NULL;
                next_open = now + REOPEN_INTERVAL_MS;

                if (!open_failed_logged) {
                    bi2x_warn(
                        "relay: could not open %s, retrying in the "
                        "background", rb->port);
                    open_failed_logged = 1;
                }

                continue;
            }

            bi2x_info("relay: %s opened", rb->port);
            open_failed_logged = 0;
            memset(&rb->parser, 0, sizeof(rb->parser));
            last_check = now;
            replies_at_check = rb->replies;
            answering = 1;
        }

        len = build_request(rb, frame);

        if (bi2x_serial_write(serial, frame, len) != 0) {
            bi2x_warn("relay: write to %s failed, reopening", rb->port);
            bi2x_serial_close(serial);
            serial = NULL;
            next_open = now + REOPEN_INTERVAL_MS;
            continue;
        }

        /* collect the reply until the next request is due */
        next_send = now + interval;

        for (;;) {
            uint64_t t = bi2x_time_ms();
            int got;
            int i;

            if (t >= next_send) {
                break;
            }

            got = bi2x_serial_read(
                serial, buf, sizeof(buf), (unsigned int) (next_send - t));

            if (got < 0) {
                break;
            }

            for (i = 0; i < got; i++) {
                if (parser_feed(&rb->parser, buf[i])) {
                    handle_reply(rb);
                }
            }
        }

        now = bi2x_time_ms();

        if (now - last_check >= ANSWER_CHECK_MS) {
            int answered = rb->replies != replies_at_check;

            if (answered != answering) {
                answering = answered;

                if (answering) {
                    bi2x_info("relay: board is answering on %s", rb->port);
                } else {
                    bi2x_warn(
                        "relay: board is not answering on %s (check TX/RX, "
                        "ground, and that the BIO2 is unplugged from it)",
                        rb->port);
                }
            }

            replies_at_check = rb->replies;
            last_check = now;
        }
    }

    if (serial != NULL) {
        /* leave the top lights dark */
        bi2x_mutex_lock(rb->lock);
        memset(rb->ticker, ' ', sizeof(rb->ticker));
        rb->spotlights = 0;
        rb->neon = 0;
        rb->panel_lamps = 0;
        bi2x_mutex_unlock(rb->lock);

        uint8_t frame[64];
        size_t len = build_request(rb, frame);

        bi2x_serial_write(serial, frame, len);
        bi2x_sleep_ms(interval);
        bi2x_serial_close(serial);
    }

    return 0;
}

struct relay_board *relay_board_open(const struct relay_board_config *cfg)
{
    struct relay_board *rb = calloc(1, sizeof(*rb));
    unsigned int i;

    if (rb == NULL) {
        return NULL;
    }

    rb->cfg = *cfg;
    strncpy(rb->port, cfg->port, sizeof(rb->port) - 1);
    rb->cfg.port = rb->port;

    memset(rb->ticker, ' ', sizeof(rb->ticker));

    for (i = 0; i < RELAY_BOARD_FADERS; i++) {
        rb->fader_steps[i] = -1;
    }

    rb->lock = bi2x_mutex_create();

    if (rb->lock == NULL) {
        free(rb);
        return NULL;
    }

    rb->thread = bi2x_thread_start(cfg->threads, relay_thread, rb);

    if (rb->thread == NULL) {
        bi2x_mutex_destroy(rb->lock);
        free(rb);
        return NULL;
    }

    bi2x_info(
        "relay: driving the relay board on %s (panel lamps %s)", rb->port,
        rb->cfg.panel_lamps ? "on" : "off");

    return rb;
}

void relay_board_close(struct relay_board *rb)
{
    if (rb == NULL) {
        return;
    }

    rb->stop = 1;
    bi2x_thread_join(rb->thread);
    bi2x_mutex_destroy(rb->lock);
    free(rb);
}

void relay_board_set_ticker(struct relay_board *rb, const char *text)
{
    size_t i;

    if (rb == NULL || text == NULL) {
        return;
    }

    bi2x_mutex_lock(rb->lock);

    for (i = 0; i < RELAY_BOARD_TICKER_SIZE && text[i] != '\0'; i++) {
        rb->ticker[i] = text[i];
    }

    for (; i < RELAY_BOARD_TICKER_SIZE; i++) {
        rb->ticker[i] = ' ';
    }

    bi2x_mutex_unlock(rb->lock);
}

void relay_board_set_spotlights(struct relay_board *rb, uint8_t spotlights)
{
    if (rb == NULL) {
        return;
    }

    bi2x_mutex_lock(rb->lock);
    rb->spotlights = spotlights;
    bi2x_mutex_unlock(rb->lock);
}

void relay_board_set_neon(struct relay_board *rb, bool on)
{
    if (rb == NULL) {
        return;
    }

    bi2x_mutex_lock(rb->lock);
    rb->neon = on ? 0x01 : 0x00;
    bi2x_mutex_unlock(rb->lock);
}

void relay_board_set_panel_lamps(struct relay_board *rb, uint8_t lamps)
{
    if (rb == NULL) {
        return;
    }

    bi2x_mutex_lock(rb->lock);
    rb->panel_lamps = lamps & 0x0F;
    bi2x_mutex_unlock(rb->lock);
}

uint8_t relay_board_get_fader(
    struct relay_board *rb, unsigned int fader, uint8_t fallback)
{
    int step;

    if (rb == NULL || fader >= RELAY_BOARD_FADERS) {
        return fallback;
    }

    bi2x_mutex_lock(rb->lock);
    step = rb->fader_steps[fader];
    bi2x_mutex_unlock(rb->lock);

    if (step < 0) {
        return fallback;
    }

    return (uint8_t) step;
}
