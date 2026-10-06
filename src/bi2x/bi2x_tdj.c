#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bi2x_link.h"
#include "bi2x_log.h"
#include "bi2x_module.h"
#include "bi2x_tdj.h"

/* ------------------------------------------------------------------------ */
/* Constants recovered from libaio-iob.dll / libaio-iob2_video.dll           */
/* ------------------------------------------------------------------------ */

#define BI2X_NODE_TYPE 0x0D060001u /* AIO_IOB2_BI2X */
#define BI2X_FW_PROTOCOL 0x0102u   /* FIRMINFO[5..6], AIO_IOB2_BI2X::CheckFirmware */

/* system module (id 00) commands, AIO_NMGR_IOB2 / AIO_NCTL_IOB2 */
#define SYS_ENUM 0x01
#define SYS_FIRMINFO 0x02
#define SYS_MOD_ALLOC 0x10
#define SYS_MOD_WRITE 0x13
#define SYS_MOD_EXEC 0x78
#define MOD_CHUNK 0x40

/* TDJ module commands, AIO_IOB2_BI2X_TDJ */
#define TDJ_STATUS 0x10
#define TDJ_OUTPUT 0x11
#define TDJ_IORESET 0x12
#define TDJ_TAPE_GAMMA 0x20
#define TDJ_TAPE_DATA 0x21
#define TDJ_TAPE_COMMIT 0x22

#define TDJ_OUT_SIZE 17
#define TDJ_RESET_SIZE 4

/* work flags, same bits as AIO_IOB2_BI2X_TDJ+0x170 */
#define F_OUTPUT 0x04
#define F_IORESET 0x08
#define F_GAMMA 0x10
#define F_TAPE 0x20
#define F_COMMIT_SENT 0x40
#define F_COMMIT 0x80

/* output block layout (AIO_IOB2_BI2X_TDJ+0xE4A), sent with TDJ_OUTPUT */
enum {
    OUT_WATCHDOG = 0,
    OUT_PANEL = 1, /* b0 coin blocker, b1 1P start, b2 2P start, b3 VEFX, b4 EFFECT */
    OUT_COUNTER = 2,
    OUT_WOOFER = 3,       /* RGB555 big endian, 2 bytes */
    OUT_P1_KEYS = 5,      /* b0-b6 */
    OUT_P1_ICCR = 6,      /* RGB555 BE */
    OUT_P1_TT = 8,        /* RGB555 BE */
    OUT_P1_TT_RESIST = 10,
    OUT_P2_KEYS = 11,
    OUT_P2_ICCR = 12,
    OUT_P2_TT = 14,
    OUT_P2_TT_RESIST = 16,
};

/* Tape LED sections: first LED index (global), strip id. Entry 17 is the
   end marker. (libaio-iob2_video.dll .data, "rev_tapeled_offset") */
static const struct {
    uint16_t start;
    uint8_t strip;
} tape_sections[BI2X_TDJ_TAPE_SECTIONS + 1] = {
    {0, 0},     {19, 0},    {38, 1},    {83, 2},    {128, 3},   {149, 4},
    {203, 4},   {214, 4},   {225, 4},   {279, 5},   {296, 5},   {313, 6},
    {381, 6},   {449, 6},   {510, 7},   {578, 7},   {646, 7},   {707, 8},
};

/* first global LED index of each strip */
static const uint16_t tape_strip_base[9] = {0, 38, 83, 128, 149, 279, 313, 510, 707};

/* sections whose LED order is reversed relative to the data passed in */
static const uint8_t tape_reversed[BI2X_TDJ_TAPE_SECTIONS] = {
    0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1};

/* max payload of one tape data packet (AIO_IOB2_BI2X_TDJ tape state 6) */
#define TAPE_PACKET_MAX 0x107

/* ------------------------------------------------------------------------ */

struct bi2x_tdj {
    struct bi2x_tdj_config cfg;
    char port[512];
    char **module_dirs;
    size_t module_dir_count;

    struct bi2x_mutex *lock;
    struct bi2x_thread *thread;
    volatile int running;
    volatile int online;

    struct bi2x_link link;
    struct bi2x_module mod_tdj;
    struct bi2x_module mod_sci;
    uint8_t gamma[256];

    /* ---- guarded by lock ---- */

    /* staging, written by the setters (TDJ+0xE4A..) */
    uint8_t stage_out[TDJ_OUT_SIZE + TDJ_RESET_SIZE];
    int stage_reset;
    uint16_t stage_tape[BI2X_TDJ_TAPE_LEDS];
    uint8_t stage_tape_dirty[3];
    uint8_t tape_limit_max;
    uint8_t tape_limit_avg;
    uint8_t tape_limit_last_max;
    uint8_t tape_limit_last_avg;

    /* committed by bi2x_tdj_update(), consumed by the I/O thread */
    uint8_t commit_out[TDJ_OUT_SIZE];
    uint8_t commit_reset[TDJ_RESET_SIZE];
    int commit_reset_pending;
    uint16_t commit_tape[BI2X_TDJ_TAPE_LEDS];
    uint8_t commit_tape_dirty[3];

    /* produced by the I/O thread */
    uint8_t raw_status[BI2X_TDJ_RAW_STATUS_SIZE];
    uint8_t sent_out[TDJ_OUT_SIZE];
    uint8_t sent_reset[TDJ_RESET_SIZE];
    uint8_t count_status;
    uint8_t count_output;
    uint8_t count_commit;
    struct bi2x_tdj_node_info node_info;

    /* latched by bi2x_tdj_update() */
    uint8_t devstatus[BI2X_TDJ_DEVSTATUS_SIZE];
    uint8_t debounce_6;

    /* ---- I/O thread private ---- */
    uint8_t node;
    uint8_t mod_id;
    uint32_t flags;
    uint16_t tape_work[BI2X_TDJ_TAPE_LEDS];
    uint8_t tape_work_dirty[3];
    uint16_t tape_pos;
    uint8_t pending_reset[TDJ_RESET_SIZE];
    int failures;
    int waiting_logged;
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static uint16_t rgb555(uint32_t rgb)
{
    /* AIO_IOB2_BI2X_TDJ::Set*Led */
    return (uint16_t) (((rgb >> 9) & 0x7C00) | ((rgb >> 6) & 0x03E0) |
                       ((rgb >> 3) & 0x001F));
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
        ((uint32_t) p[2] << 8) | p[3];
}

static void build_gamma(uint8_t *table)
{
    /* AIO_IOB2_BI2X::GenerateTapeLedGammaTable(table, 0xFF, 0.5f) */
    unsigned int i;

    for (i = 0; i < 256; i++) {
        float v = powf(1.0f - (float) i / 255.0f, 0.5f);
        table[i] = (uint8_t) (long) ((1.0f - v) * 255.0f);
    }
}

/* AIO_IOB2_BI2X::SetTapeLedDataLimited: scale all LEDs down so the average
   brightness stays below `avg` (a power budget), `max` is the full scale. */
static void tape_limit(
    uint16_t *dst, const uint16_t *src, unsigned int n, uint8_t avg, uint8_t max)
{
    uint64_t sum = 0;
    uint64_t level;
    unsigned int scale = (unsigned int) max + 1;
    unsigned int i;

    for (i = 0; i < n; i++) {
        uint16_t c = src[i];
        sum += (c >> 10) + ((c >> 5) & 0x1F) + (c & 0x1F);
    }

    level = (sum * max) / ((uint64_t) n * 0x5D);

    if (avg < level) {
        scale = (unsigned int) (((uint64_t) avg * (max + 1)) / level);
    }

    for (i = 0; i < n; i++) {
        uint16_t c = src[i];
        uint16_t r = (uint16_t) (((((c >> 8) & 0xFC) * scale)) & 0x7C00);
        uint16_t g = (uint16_t) (((((c >> 5) & 0x1F) * scale) >> 3) & 0x03E0);
        uint16_t b = (uint16_t) (((c & 0x1F) * scale) >> 8);
        dst[i] = r | g | b;
    }
}

static void mark_all_tape_dirty(uint8_t *dirty)
{
    dirty[0] = dirty[1] = dirty[2] = 0xFF;
}

static int tape_dirty_any(const uint8_t *dirty)
{
    return (dirty[0] | dirty[1] | dirty[2]) != 0;
}

/* ------------------------------------------------------------------------ */
/* Board bring-up                                                            */
/* ------------------------------------------------------------------------ */

static int enumerate_nodes(struct bi2x_tdj *t, uint8_t *count)
{
    static const uint8_t req[2] = {0x00, SYS_ENUM};
    uint8_t resp[64];
    uint32_t len = 0;
    int rc;

    rc = bi2x_link_transact_probe(
        &t->link, 0, req, sizeof(req), resp, sizeof(resp), &len, 0x11, 2, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    if (len <= 2) {
        bi2x_warn("node enumeration returned no nodes");
        return BI2X_ERR_PROTO;
    }

    *count = (uint8_t) (len - 2);

    return BI2X_OK;
}

static int query_firminfo(struct bi2x_tdj *t, uint8_t node, uint8_t *fi)
{
    static const uint8_t req[3] = {0x00, SYS_FIRMINFO, 0x81};
    uint8_t resp[64];
    uint32_t len = 0;
    int rc;

    rc = bi2x_link_transact(
        &t->link, node, req, sizeof(req), resp, sizeof(resp), &len, 0x23, 2, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    if (len < 0x23 || resp[0] != 0x00 || resp[1] != SYS_FIRMINFO ||
        resp[2] != 0x00) {
        return BI2X_ERR_PROTO;
    }

    memcpy(fi, resp + 3, 32);

    return BI2X_OK;
}

static void log_firminfo(uint8_t node, const uint8_t *fi)
{
    char name[5];
    int i;

    for (i = 0; i < 4; i++) {
        name[i] = (fi[8 + i] >= 0x20 && fi[8 + i] < 0x7F) ? (char) fi[8 + i] : '.';
    }
    name[4] = '\0';

    bi2x_info(
        "node %u: type %08X \"%s\" protocol %u.%u firmware %02X%02X",
        node, (unsigned int) get_be32(fi), name, fi[5], fi[6], fi[0x1E],
        fi[0x1F]);
}

/* AIO_NCTL_IOB2::LoadModule: allocate, upload in 64 byte chunks, execute */
static int load_module(
    struct bi2x_tdj *t, const struct bi2x_module *mod, uint8_t *handle_out,
    uint8_t *id_out)
{
    uint8_t req[7 + MOD_CHUNK];
    uint8_t resp[16];
    uint32_t len;
    uint32_t off;
    uint8_t handle;
    int rc;

    req[0] = 0x00;
    req[1] = SYS_MOD_ALLOC;
    put_be32(req + 2, mod->size);
    len = 0;

    rc = bi2x_link_transact(
        &t->link, t->node, req, 6, resp, sizeof(resp), &len, 4, 2, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    if (len != 4 || resp[0] != 0x00 || resp[1] != SYS_MOD_ALLOC ||
        resp[2] != 0x00) {
        bi2x_warn("module allocation refused (len %u)", (unsigned int) len);
        return BI2X_ERR_PROTO;
    }

    handle = resp[3];

    for (off = 0; off < mod->size; off += MOD_CHUNK) {
        uint32_t n = mod->size - off;

        if (n > MOD_CHUNK) {
            n = MOD_CHUNK;
        }

        req[0] = 0x00;
        req[1] = SYS_MOD_WRITE;
        req[2] = handle;
        put_be32(req + 3, off);
        memcpy(req + 7, mod->data + off, n);
        len = 0;

        rc = bi2x_link_transact(
            &t->link, t->node, req, 7 + n, resp, sizeof(resp), &len, 3, 2, 3);

        if (rc != BI2X_OK) {
            return rc;
        }

        if (len != 3 || resp[0] != 0x00 || resp[1] != SYS_MOD_WRITE ||
            resp[2] != 0x00) {
            bi2x_warn("module write refused at offset %u", (unsigned int) off);
            return BI2X_ERR_PROTO;
        }
    }

    req[0] = 0x00;
    req[1] = SYS_MOD_EXEC;
    req[2] = handle;
    put_be32(req + 3, 0);
    len = 0;

    rc = bi2x_link_transact(
        &t->link, t->node, req, 7, resp, sizeof(resp), &len, 4, 1000, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    if (len != 4 || resp[0] != 0x00 || resp[1] != SYS_MOD_EXEC ||
        resp[2] != 0x00) {
        bi2x_warn("module start refused");
        return BI2X_ERR_PROTO;
    }

    *handle_out = handle;
    *id_out = resp[3];

    return BI2X_OK;
}

static int connect_board(struct bi2x_tdj *t)
{
    static const uint8_t expect_cfg[14] = {
        0x00, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x12};
    uint8_t fi[32];
    uint8_t count = 0;
    uint8_t n;
    uint8_t handle;
    uint8_t sci_handle;
    uint8_t sci_id;
    int found = 0;
    int rc;

    if (t->link.serial == NULL) {
        if (bi2x_link_open(&t->link, t->port, 1000) != BI2X_OK) {
            if (!t->waiting_logged) {
                bi2x_info(
                    "waiting for the BI2X (USB %04X:%04X%s%s) to show up...",
                    BI2X_USB_VID, BI2X_USB_PID, t->port[0] ? ", port " : "",
                    t->port);
                t->waiting_logged = 1;
            }
            return BI2X_ERR_IO;
        }
    }

    t->waiting_logged = 0;
    bi2x_info("BI2X port open");

    if (!t->cfg.skip_reset) {
        rc = bi2x_link_reset_board(&t->link);

        if (rc != BI2X_OK) {
            return rc;
        }
    }

    rc = bi2x_link_sync(&t->link, 9000);

    if (rc != BI2X_OK) {
        bi2x_warn("BI2X did not answer the sync handshake");
        return rc;
    }

    rc = enumerate_nodes(t, &count);

    if (rc != BI2X_OK) {
        bi2x_warn("node enumeration failed (%d)", rc);
        return rc;
    }

    bi2x_info("%u node(s) on the bus", count);

    for (n = 1; n <= count; n++) {
        rc = query_firminfo(t, n, fi);

        if (rc != BI2X_OK) {
            bi2x_warn("node %u: firmware info query failed (%d)", n, rc);
            return rc;
        }

        log_firminfo(n, fi);

        if (!found && get_be32(fi) == BI2X_NODE_TYPE) {
            found = 1;
            t->node = n;

            bi2x_mutex_lock(t->lock);
            t->node_info.node = n;
            t->node_info.type = get_be32(fi);
            memcpy(t->node_info.firminfo, fi, sizeof(fi));
            bi2x_mutex_unlock(t->lock);

            if (((fi[5] << 8) | fi[6]) != BI2X_FW_PROTOCOL ||
                memcmp(fi + 0x10, expect_cfg, sizeof(expect_cfg)) != 0) {
                bi2x_warn(
                    "node %u firmware configuration differs from what libaio "
                    "expects for IIDX; the stock game would reflash it. "
                    "Continuing anyway.", n);
            }
        }
    }

    if (!found) {
        bi2x_warn("no BI2X node (type %08X) found", BI2X_NODE_TYPE);
        return BI2X_ERR_PROTO;
    }

    bi2x_info("uploading TDJ module (%u bytes)...", t->mod_tdj.size);
    rc = load_module(t, &t->mod_tdj, &handle, &t->mod_id);

    if (rc != BI2X_OK) {
        bi2x_warn("TDJ module upload failed (%d)", rc);
        return rc;
    }

    bi2x_info("uploading SCI module (%u bytes)...", t->mod_sci.size);
    rc = load_module(t, &t->mod_sci, &sci_handle, &sci_id);

    if (rc != BI2X_OK) {
        bi2x_warn("SCI module upload failed (%d)", rc);
        return rc;
    }

    bi2x_info(
        "modules running: TDJ id %02X (slot %02X), SCI id %02X (slot %02X)",
        t->mod_id, handle, sci_id, sci_handle);

    /* like a freshly constructed AIO_IOB2_BI2X_TDJ: push outputs, gamma
       table and every tape LED */
    bi2x_mutex_lock(t->lock);
    memset(t->sent_out, 0, sizeof(t->sent_out));
    mark_all_tape_dirty(t->commit_tape_dirty);
    bi2x_mutex_unlock(t->lock);

    t->flags = F_OUTPUT | F_GAMMA;
    t->tape_pos = 0;
    memset(t->tape_work_dirty, 0, sizeof(t->tape_work_dirty));
    t->failures = 0;

    return BI2X_OK;
}

/* ------------------------------------------------------------------------ */
/* Steady state                                                              */
/* ------------------------------------------------------------------------ */

/* One status poll, carrying pending output/reset/commit commands. Mirrors
   AIO_IOB2_BI2X_TDJ I/O state 1 (0x18000BE10). */
static int io_cycle(struct bi2x_tdj *t)
{
    uint8_t req[64];
    uint8_t resp[160];
    uint8_t out[TDJ_OUT_SIZE];
    uint8_t reset[TDJ_RESET_SIZE];
    uint32_t req_len = 0;
    uint32_t expect = 0;
    uint32_t len = 0;
    uint32_t off = 0;
    uint32_t sent;
    int rc;

    bi2x_mutex_lock(t->lock);

    memcpy(out, t->commit_out, sizeof(out));

    if (memcmp(out, t->sent_out, sizeof(out)) != 0) {
        t->flags |= F_OUTPUT;
    }

    if (t->commit_reset_pending) {
        memcpy(t->pending_reset, t->commit_reset, sizeof(t->pending_reset));
        memset(t->commit_reset, 0, sizeof(t->commit_reset));
        t->commit_reset_pending = 0;
        t->flags |= F_IORESET;
    }

    memcpy(reset, t->pending_reset, sizeof(reset));

    bi2x_mutex_unlock(t->lock);

    sent = t->flags;

    if (sent & F_IORESET) {
        req[req_len++] = t->mod_id;
        req[req_len++] = TDJ_IORESET;
        memcpy(req + req_len, reset, sizeof(reset));
        req_len += sizeof(reset);
        expect += 3;
    }

    if (sent & F_OUTPUT) {
        req[req_len++] = t->mod_id;
        req[req_len++] = TDJ_OUTPUT;
        memcpy(req + req_len, out, sizeof(out));
        req_len += sizeof(out);
        expect += 3;
    }

    if (sent & (F_COMMIT | F_COMMIT_SENT)) {
        t->flags = (t->flags & ~F_COMMIT) | F_COMMIT_SENT;
        req[req_len++] = t->mod_id;
        req[req_len++] = TDJ_TAPE_COMMIT;
        expect += 3;
    }

    req[req_len++] = t->mod_id;
    req[req_len++] = TDJ_STATUS;
    expect += 3 + BI2X_TDJ_RAW_STATUS_SIZE;

    rc = bi2x_link_transact(
        &t->link, t->node, req, req_len, resp, sizeof(resp), &len, expect, 2, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    bi2x_mutex_lock(t->lock);

    if (sent & F_IORESET) {
        if (len - off < 3) {
            goto short_resp;
        }
        if (resp[off] == t->mod_id && resp[off + 1] == TDJ_IORESET &&
            resp[off + 2] == 0x00) {
            memcpy(t->sent_reset, reset, sizeof(reset));
            t->flags &= ~F_IORESET;
        }
        off += 3;
    }

    if (sent & F_OUTPUT) {
        if (len - off < 3) {
            goto short_resp;
        }
        if (resp[off] == t->mod_id && resp[off + 1] == TDJ_OUTPUT &&
            resp[off + 2] <= 1) {
            memcpy(t->sent_out, out, sizeof(out));
            t->count_output++;

            /* 1 = busy, send again next time */
            if (resp[off + 2] == 0) {
                t->flags &= ~F_OUTPUT;
            }
        }
        off += 3;
    }

    if (sent & (F_COMMIT | F_COMMIT_SENT)) {
        if (len - off < 3) {
            goto short_resp;
        }
        if (resp[off] == t->mod_id && resp[off + 1] == TDJ_TAPE_COMMIT &&
            resp[off + 2] == 0x00) {
            t->count_commit++;
            t->flags &= ~F_COMMIT_SENT;
        }
        off += 3;
    }

    if (len - off < 3 + BI2X_TDJ_RAW_STATUS_SIZE || resp[off] != t->mod_id ||
        resp[off + 1] != TDJ_STATUS || resp[off + 2] != 0x00) {
        goto short_resp;
    }

    memcpy(t->raw_status, resp + off + 3, BI2X_TDJ_RAW_STATUS_SIZE);
    t->count_status++;

    bi2x_mutex_unlock(t->lock);

    return BI2X_OK;

short_resp:
    bi2x_mutex_unlock(t->lock);
    bi2x_misc("unexpected I/O response (%u bytes)", (unsigned int) len);

    return BI2X_ERR_PROTO;
}

static int tape_cycle(struct bi2x_tdj *t)
{
    uint8_t req[TAPE_PACKET_MAX + 8];
    uint8_t resp[128];
    uint32_t len = 0;
    uint32_t req_len = 0;
    uint32_t expect = 0;
    uint16_t pos;
    int rc;

    /* waiting for the commit of the previous upload to be acknowledged */
    if (t->flags & (F_COMMIT | F_COMMIT_SENT)) {
        return BI2X_OK;
    }

    if (t->flags & F_GAMMA) {
        req[0] = t->mod_id;
        req[1] = TDJ_TAPE_GAMMA;
        memcpy(req + 2, t->gamma, sizeof(t->gamma));

        rc = bi2x_link_transact(
            &t->link, t->node, req, 2 + sizeof(t->gamma), resp, sizeof(resp),
            &len, 3, 2, 3);

        if (rc != BI2X_OK) {
            return rc;
        }

        if (len == 3 && resp[0] == t->mod_id && resp[1] == TDJ_TAPE_GAMMA &&
            resp[2] == 0x00) {
            t->flags &= ~F_GAMMA;
        }

        return BI2X_OK;
    }

    if (!(t->flags & F_TAPE)) {
        bi2x_mutex_lock(t->lock);

        if (tape_dirty_any(t->commit_tape_dirty)) {
            memcpy(t->tape_work, t->commit_tape, sizeof(t->tape_work));
            memcpy(t->tape_work_dirty, t->commit_tape_dirty, 3);
            memset(t->commit_tape_dirty, 0, 3);
            t->flags |= F_TAPE;
            t->tape_pos = 0;
        }

        bi2x_mutex_unlock(t->lock);

        if (!(t->flags & F_TAPE)) {
            return BI2X_OK;
        }
    }

    /* pack as many dirty sections as fit, starting at tape_pos
       (AIO_IOB2_BI2X_TDJ tape state 6) */
    pos = t->tape_pos;

    for (;;) {
        unsigned int s = 0;
        unsigned int remain;
        unsigned int n;

        while (s < BI2X_TDJ_TAPE_SECTIONS && pos >= tape_sections[s + 1].start) {
            s++;
        }

        if (s >= BI2X_TDJ_TAPE_SECTIONS) {
            break;
        }

        remain = tape_sections[s + 1].start - pos;
        n = remain;

        if (t->tape_work_dirty[s >> 3] & (1u << (s & 7))) {
            unsigned int space = TAPE_PACKET_MAX - req_len;
            unsigned int i;
            uint8_t strip = tape_sections[s].strip;

            if (space < 9) {
                break;
            }

            if (n > (space - 7) / 2) {
                n = (space - 7) / 2;
            }

            req[req_len++] = t->mod_id;
            req[req_len++] = TDJ_TAPE_DATA;
            req[req_len++] = strip;
            put_be16(req + req_len, (uint16_t) (pos - tape_strip_base[strip]));
            req_len += 2;
            put_be16(req + req_len, (uint16_t) n);
            req_len += 2;

            for (i = 0; i < n; i++) {
                uint16_t c = t->tape_work[pos + i];
                req[req_len++] = (uint8_t) c;        /* little endian */
                req[req_len++] = (uint8_t) (c >> 8);
            }

            expect += 3;
        }

        pos = (uint16_t) (pos + n);
    }

    if (req_len == 0) {
        /* everything sent: latch it with a commit on the next poll */
        t->flags = (t->flags & ~F_TAPE) | F_COMMIT;
        return BI2X_OK;
    }

    rc = bi2x_link_transact(
        &t->link, t->node, req, req_len, resp, sizeof(resp), &len, expect, 2, 3);

    if (rc != BI2X_OK) {
        return rc;
    }

    t->tape_pos = pos;

    return BI2X_OK;
}

static int io_thread(void *ctx)
{
    struct bi2x_tdj *t = ctx;

    while (t->running) {
        int rc;

        if (!t->online) {
            rc = connect_board(t);

            if (rc != BI2X_OK) {
                bi2x_link_close(&t->link);

                if (t->running) {
                    bi2x_sleep_ms(1000);
                }

                continue;
            }

            bi2x_info("BI2X online");
            t->online = 1;
        }

        rc = io_cycle(t);

        if (rc == BI2X_OK) {
            rc = tape_cycle(t);
        }

        if (rc == BI2X_OK) {
            t->failures = 0;
        } else if (rc == BI2X_ERR_IO || ++t->failures >= 5) {
            bi2x_warn("lost BI2X (%d), reconnecting", rc);
            t->online = 0;
            bi2x_link_close(&t->link);
            continue;
        }

        if (t->cfg.poll_interval_ms > 0) {
            bi2x_sleep_ms(t->cfg.poll_interval_ms);
        }
    }

    bi2x_link_close(&t->link);

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

static int load_modules(struct bi2x_tdj *t)
{
    const char *dirs[16];
    char self[512];
    size_t n = 0;
    size_t i;

    if (t->module_dir_count > 0) {
        for (i = 0; i < t->module_dir_count && n < 16; i++) {
            dirs[n++] = t->module_dirs[i];
        }
    } else {
        bi2x_self_dir(self, sizeof(self));

        if (self[0] != '\0') {
            dirs[n++] = self;
        }

        dirs[n++] = "";
    }

    if (bi2x_module_find(BI2X_MODULE_TDJ, dirs, n, &t->mod_tdj) != 0) {
        bi2x_fatal(
            "TDJ module not found: put %s (or %s) next to this DLL",
            bi2x_module_bin_name(BI2X_MODULE_TDJ),
            bi2x_module_dll_name(BI2X_MODULE_TDJ));
        return -1;
    }

    if (bi2x_module_find(BI2X_MODULE_SCI, dirs, n, &t->mod_sci) != 0) {
        bi2x_fatal(
            "SCI module not found: put %s (or %s) next to this DLL",
            bi2x_module_bin_name(BI2X_MODULE_SCI),
            bi2x_module_dll_name(BI2X_MODULE_SCI));
        return -1;
    }

    return 0;
}

static void tdj_free(struct bi2x_tdj *t)
{
    size_t i;

    if (t == NULL) {
        return;
    }

    for (i = 0; i < t->module_dir_count; i++) {
        free(t->module_dirs[i]);
    }

    free(t->module_dirs);
    bi2x_module_free(&t->mod_tdj);
    bi2x_module_free(&t->mod_sci);
    bi2x_mutex_destroy(t->lock);
    free(t);
}

int bi2x_tdj_open(const struct bi2x_tdj_config *cfg, struct bi2x_tdj **out)
{
    struct bi2x_tdj *t;
    uint64_t deadline;
    size_t i;

    *out = NULL;
    t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return -1;
    }

    t->cfg = *cfg;
    t->cfg.module_dirs = NULL;
    t->cfg.port = NULL;

    if (cfg->port != NULL) {
        strncpy(t->port, cfg->port, sizeof(t->port) - 1);
    }

    if (cfg->module_dirs != NULL && cfg->module_dir_count > 0) {
        t->module_dirs = calloc(cfg->module_dir_count, sizeof(char *));

        for (i = 0; t->module_dirs != NULL && i < cfg->module_dir_count; i++) {
            size_t len = strlen(cfg->module_dirs[i]);
            t->module_dirs[i] = malloc(len + 1);

            if (t->module_dirs[i] != NULL) {
                memcpy(t->module_dirs[i], cfg->module_dirs[i], len + 1);
                t->module_dir_count++;
            }
        }
    }

    t->lock = bi2x_mutex_create();

    if (t->lock == NULL || load_modules(t) != 0) {
        tdj_free(t);
        return -1;
    }

    build_gamma(t->gamma);
    t->tape_limit_max = 0xFF;
    t->tape_limit_avg = 0x55;
    t->tape_limit_last_max = 0xFF;
    t->tape_limit_last_avg = 0x55;
    tape_limit(
        t->commit_tape, t->stage_tape, BI2X_TDJ_TAPE_LEDS, t->tape_limit_avg,
        t->tape_limit_max);

    t->running = 1;
    t->thread = bi2x_thread_start(cfg->threads, io_thread, t);

    if (t->thread == NULL) {
        tdj_free(t);
        return -1;
    }

    deadline = bi2x_time_ms() + cfg->connect_timeout_ms;

    while (!t->online && bi2x_time_ms() < deadline) {
        bi2x_sleep_ms(10);
    }

    *out = t;

    return t->online || cfg->connect_timeout_ms == 0 ? 0 : 1;
}

void bi2x_tdj_close(struct bi2x_tdj *t)
{
    if (t == NULL) {
        return;
    }

    t->running = 0;
    bi2x_thread_join(t->thread);
    tdj_free(t);
}

int bi2x_tdj_is_online(struct bi2x_tdj *t)
{
    return t != NULL && t->online;
}

int bi2x_tdj_get_node_info(struct bi2x_tdj *t, struct bi2x_tdj_node_info *info)
{
    if (!t->online) {
        return -1;
    }

    bi2x_mutex_lock(t->lock);
    *info = t->node_info;
    bi2x_mutex_unlock(t->lock);

    return 0;
}

/* AIO_IOB2_BI2X_TDJ::UpdateDevicesStatus equivalent (0x18000A5C0) */
void bi2x_tdj_update(struct bi2x_tdj *t)
{
    uint8_t *ds = t->devstatus;
    const uint8_t *raw = t->raw_status;
    int i;

    bi2x_mutex_lock(t->lock);

    if (ds[0] != t->count_status) {
        ds[0] = t->count_status;
        ds[1] = t->count_output;
        ds[2] = t->count_commit;
        memcpy(ds + 0x29, raw, BI2X_TDJ_RAW_STATUS_SIZE);
        memcpy(ds + 0x73, t->sent_out, TDJ_OUT_SIZE);
        memcpy(ds + 0x84, t->sent_reset, TDJ_RESET_SIZE);

        ds[0x03] = raw[0];

        for (i = 0; i < 4; i++) {
            ds[0x04 + i] = (raw[1] >> i) & 1; /* test, service, coin, ? */
        }
        for (i = 0; i < 8; i++) {
            ds[0x08 + i] = (raw[2] >> i) & 1; /* 1P/2P start, VEFX, EFFECT */
        }
        for (i = 0; i < 4; i++) {
            ds[0x10 + i] = (raw[3] >> i) & 1;
        }

        ds[0x14] = raw[4]; /* 1P turntable */
        ds[0x15] = raw[5]; /* 2P turntable */

        /* only taken once it reads the same twice in a row */
        if (raw[6] != t->debounce_6) {
            t->debounce_6 = raw[6];
        } else {
            ds[0x16] = raw[6];
        }

        ds[0x17] = raw[7] & 1;
        ds[0x18] = (raw[7] >> 1) & 1;
        ds[0x19] = (raw[7] >> 6) & 1;
        ds[0x1A] = (raw[7] >> 7) & 1;

        for (i = 0; i < 7; i++) {
            ds[0x1B + i] = (raw[8] >> i) & 1; /* 1P keys */
            ds[0x22 + i] = (raw[9] >> i) & 1; /* 2P keys */
        }

        memcpy(ds + 0x88, raw + 8, 0x42);
    }

    /* commit outputs */
    memcpy(t->commit_out, t->stage_out, TDJ_OUT_SIZE);

    if (t->stage_reset) {
        for (i = 0; i < TDJ_RESET_SIZE; i++) {
            t->commit_reset[i] |= t->stage_out[TDJ_OUT_SIZE + i];
        }
        memset(t->stage_out + TDJ_OUT_SIZE, 0, TDJ_RESET_SIZE);
        t->stage_reset = 0;
        t->commit_reset_pending = 1;
    }

    /* commit tape LEDs through the brightness limiter */
    tape_limit(
        t->commit_tape, t->stage_tape, BI2X_TDJ_TAPE_LEDS, t->tape_limit_avg,
        t->tape_limit_max);

    if (t->tape_limit_avg != t->tape_limit_last_avg ||
        t->tape_limit_max != t->tape_limit_last_max) {
        t->tape_limit_last_avg = t->tape_limit_avg;
        t->tape_limit_last_max = t->tape_limit_max;
        mark_all_tape_dirty(t->stage_tape_dirty);
    }

    for (i = 0; i < 3; i++) {
        t->commit_tape_dirty[i] |= t->stage_tape_dirty[i];
        t->stage_tape_dirty[i] = 0;
    }

    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_get_device_status(
    struct bi2x_tdj *t, uint8_t out[BI2X_TDJ_DEVSTATUS_SIZE])
{
    bi2x_mutex_lock(t->lock);
    memcpy(out, t->devstatus, BI2X_TDJ_DEVSTATUS_SIZE);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_get_input(struct bi2x_tdj *t, struct bi2x_tdj_input *in)
{
    const uint8_t *ds = t->devstatus;

    bi2x_mutex_lock(t->lock);

    in->seq = ds[0];
    in->test = ds[0x04];
    in->service = ds[0x05];
    in->coin = ds[0x06];
    in->start[0] = ds[0x08];
    in->start[1] = ds[0x09];
    in->vefx = ds[0x0A];
    in->effect = ds[0x0B];
    in->turntable[0] = ds[0x14];
    in->turntable[1] = ds[0x15];
    in->keys[0] = ds[0x29 + 8] & 0x7F;
    in->keys[1] = ds[0x29 + 9] & 0x7F;
    memcpy(in->raw, ds + 0x29, BI2X_TDJ_RAW_STATUS_SIZE);

    bi2x_mutex_unlock(t->lock);
}

/* ---- setters (AIO_IOB2_BI2X_TDJ::Set*) ---- */

static void set_bit(uint8_t *byte, unsigned int bit, int on)
{
    *byte = (uint8_t) ((*byte & ~(1u << bit)) | ((on ? 1u : 0u) << bit));
}

void bi2x_tdj_io_reset(struct bi2x_tdj *t, uint32_t flags)
{
    uint8_t *p;
    uint32_t cur;

    bi2x_mutex_lock(t->lock);
    p = t->stage_out + TDJ_OUT_SIZE;
    cur = get_be32(p) | flags;
    put_be32(p, cur);
    t->stage_reset = 1;
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_watchdog_timer(struct bi2x_tdj *t, uint8_t value)
{
    bi2x_mutex_lock(t->lock);
    t->stage_out[OUT_WATCHDOG] = value;
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_control_coin_blocker(struct bi2x_tdj *t, unsigned int slot, int block)
{
    if (slot != 0) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    set_bit(&t->stage_out[OUT_PANEL], 0, block);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_add_counter(struct bi2x_tdj *t, unsigned int slot, unsigned int count)
{
    if (slot != 0) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    t->stage_out[OUT_COUNTER] = (uint8_t) (t->stage_out[OUT_COUNTER] + count);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_start_lamp(struct bi2x_tdj *t, unsigned int player, int on)
{
    if (player > 1) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    set_bit(&t->stage_out[OUT_PANEL], 1 + player, on);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_vefx_button_lamp(struct bi2x_tdj *t, int on)
{
    bi2x_mutex_lock(t->lock);
    set_bit(&t->stage_out[OUT_PANEL], 3, on);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_effect_button_lamp(struct bi2x_tdj *t, int on)
{
    bi2x_mutex_lock(t->lock);
    set_bit(&t->stage_out[OUT_PANEL], 4, on);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_player_button_lamp(
    struct bi2x_tdj *t, unsigned int player, unsigned int button, int on)
{
    if (player > 1 || button > 6) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    set_bit(&t->stage_out[player ? OUT_P2_KEYS : OUT_P1_KEYS], button, on);
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_woofer_led(struct bi2x_tdj *t, uint32_t rgb)
{
    bi2x_mutex_lock(t->lock);
    put_be16(t->stage_out + OUT_WOOFER, rgb555(rgb));
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_iccr_led(struct bi2x_tdj *t, unsigned int player, uint32_t rgb)
{
    if (player > 1) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    put_be16(t->stage_out + (player ? OUT_P2_ICCR : OUT_P1_ICCR), rgb555(rgb));
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_turntable_led(
    struct bi2x_tdj *t, unsigned int player, uint32_t rgb)
{
    if (player > 1) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    put_be16(t->stage_out + (player ? OUT_P2_TT : OUT_P1_TT), rgb555(rgb));
    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_turntable_resist(
    struct bi2x_tdj *t, unsigned int player, uint8_t value)
{
    if (player > 1) {
        return;
    }

    bi2x_mutex_lock(t->lock);
    t->stage_out[player ? OUT_P2_TT_RESIST : OUT_P1_TT_RESIST] = value;
    bi2x_mutex_unlock(t->lock);
}

unsigned int bi2x_tdj_tape_led_count(unsigned int section)
{
    if (section >= BI2X_TDJ_TAPE_SECTIONS) {
        return 0;
    }

    return (unsigned int) (tape_sections[section + 1].start -
                           tape_sections[section].start);
}

void bi2x_tdj_set_tape_led_data(
    struct bi2x_tdj *t, unsigned int section, const uint8_t *rgb)
{
    unsigned int n = bi2x_tdj_tape_led_count(section);
    uint16_t *dst;
    unsigned int i;

    if (n == 0 || rgb == NULL) {
        return;
    }

    bi2x_mutex_lock(t->lock);

    dst = t->stage_tape + tape_sections[section].start;

    for (i = 0; i < n; i++) {
        const uint8_t *c = rgb + i * 3;
        uint16_t v = (uint16_t) (((c[0] & 0xF8) << 7) | ((c[1] & 0xF8) << 2) |
                                 (c[2] >> 3));

        dst[tape_reversed[section] ? n - 1 - i : i] = v;
    }

    t->stage_tape_dirty[section >> 3] |= (uint8_t) (1u << (section & 7));

    bi2x_mutex_unlock(t->lock);
}

void bi2x_tdj_set_tape_led_data_limit(
    struct bi2x_tdj *t, uint8_t max, uint8_t average)
{
    bi2x_mutex_lock(t->lock);
    t->tape_limit_max = max;
    t->tape_limit_avg = average;
    bi2x_mutex_unlock(t->lock);
}
