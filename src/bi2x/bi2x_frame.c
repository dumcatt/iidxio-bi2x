#include <string.h>

#include "bi2x_crc.h"
#include "bi2x_frame.h"

enum {
    DEC_IDLE = 0,
    DEC_ADDR,
    DEC_TAG,
    DEC_LEN,
    DEC_FLAGS,
    DEC_SUBST,
    DEC_DATA,
    DEC_CRC,
};

uint8_t bi2x_cipher_byte(uint32_t *state, uint8_t b)
{
    uint8_t mask;

    /* bytes with all of bits 1,3,5,7 set (AA, FF, ...) pass through and do
       not advance the keystream */
    if (((uint8_t) ~b & 0xAA) == 0) {
        return b;
    }

    *state = *state * 0x41C64E6Du + 0x3039u;
    mask = (b & 0x80) ? 0x55 : 0x7F;

    return (uint8_t) (b ^ ((uint8_t) *state & mask));
}

uint8_t bi2x_frame_head_sum(
    uint8_t addr, uint8_t tag, uint8_t len_count, uint32_t len, uint8_t flags)
{
    uint8_t crc = 0x0F;
    uint8_t b;

    crc = bi2x_crc4(crc, &addr, 1);
    crc = bi2x_crc4(crc, &tag, 1);

    /* mirrors the fall-through switch in QueryHeadSum */
    if (len_count >= 2 && len_count <= 5) {
        if (len_count >= 5) {
            b = (uint8_t) ((len >> 25) | 0xC0);
            crc = bi2x_crc4(crc, &b, 1);
        }
        if (len_count >= 4) {
            b = (uint8_t) ((len >> 19) | 0xC0);
            crc = bi2x_crc4(crc, &b, 1);
        }
        if (len_count >= 3) {
            b = (uint8_t) ((len >> 13) | 0xC0);
            crc = bi2x_crc4(crc, &b, 1);
        }
        b = (uint8_t) ((len >> 7) | 0xC0);
        crc = bi2x_crc4(crc, &b, 1);
    }

    b = (uint8_t) (len & 0x7F);
    crc = bi2x_crc4(crc, &b, 1);
    b = flags & 0xF0;
    crc = bi2x_crc4(crc, &b, 1);

    return crc ^ 0x0F;
}

/* Pick the data encoding the same way the original sender does
   (minus mode 4 compression). Returns mode, *subst set for mode 3. */
static uint8_t choose_mode(const uint8_t *data, uint32_t len, uint8_t *subst)
{
    uint8_t present[32];
    uint32_t i;
    int has_aa = 0;
    int v;

    if (len == 0) {
        return 0;
    }

    memset(present, 0, sizeof(present));

    for (i = 0; i < len; i++) {
        present[data[i] >> 3] |= (uint8_t) (1 << (data[i] & 7));
        if (data[i] == 0xAA) {
            has_aa = 1;
        }
    }

    if (!has_aa) {
        return 2;
    }

    for (v = 0; v < 256; v++) {
        if (!(present[v >> 3] & (1 << (v & 7)))) {
            *subst = (uint8_t) v;
            return 3;
        }
    }

    return 0;
}

size_t bi2x_frame_encode(
    struct bi2x_cipher *cipher,
    uint8_t node,
    uint8_t response,
    uint8_t tag,
    const uint8_t *data,
    uint32_t len,
    uint8_t *out,
    size_t out_cap)
{
    size_t pos = 0;
    size_t data_start;
    uint8_t len_count;
    uint8_t mode;
    uint8_t subst = 0;
    uint8_t flags;
    int encrypt;
    int shift;
    uint32_t i;

    if (len > BI2X_PAYLOAD_MAX || out_cap < (size_t) len * 2 + 16) {
        return 0;
    }

    out[pos++] = 0xAA;
    out[pos++] = (uint8_t) (((node & 0x3F) << 1) | (response ? 1 : 0));
    out[pos++] = tag;

    for (shift = 25; shift >= 7; shift -= 6) {
        uint8_t group = (uint8_t) ((len >> shift) & 0x3F);

        /* the original sender skips every zero group, not just leading
           ones; replicate it (only matters for len >= 0x2000) */
        if (group != 0) {
            out[pos++] = (uint8_t) (0xC0 | group);
        }
    }

    out[pos++] = (uint8_t) (len & 0x7F);
    len_count = (uint8_t) (pos - 3);

    mode = choose_mode(data, len, &subst);
    encrypt = len != 0 && node != 0 && cipher != NULL &&
        cipher->enabled[node & 0x3F];

    flags = (uint8_t) (mode << 5);
    if (encrypt) {
        flags |= 0x10;
    }
    flags |= bi2x_frame_head_sum(out[1], tag, len_count, len, flags);

    out[pos++] = flags;
    data_start = pos;

    if (len != 0) {
        switch (mode) {
            case 0:
                for (i = 0; i < len; i++) {
                    if (data[i] == 0xAA || data[i] == 0xFF) {
                        out[pos++] = 0xFF;
                        out[pos++] = (uint8_t) ~data[i];
                    } else {
                        out[pos++] = data[i];
                    }
                }
                break;

            case 3:
                out[pos++] = subst;
                for (i = 0; i < len; i++) {
                    out[pos++] = data[i] == 0xAA ? subst : data[i];
                }
                break;

            default:
                memcpy(out + pos, data, len);
                pos += len;
                break;
        }

        out[pos++] = bi2x_crc7(0x7F, data, len) ^ 0x7F;

        if (encrypt) {
            uint32_t state = (uint32_t) (tag ^ 0x55);
            size_t j;

            for (j = data_start; j < pos; j++) {
                out[j] = bi2x_cipher_byte(&state, out[j]);
            }
        }
    }

    return pos;
}

void bi2x_frame_decoder_init(
    struct bi2x_frame_decoder *dec, struct bi2x_cipher *cipher)
{
    memset(dec, 0, sizeof(*dec));
    dec->cipher = cipher;
    dec->state = DEC_IDLE;
}

static void dec_start(struct bi2x_frame_decoder *dec)
{
    dec->state = DEC_ADDR;
    dec->len = 0;
    dec->len_count = 0;
    dec->escape = 0;
    dec->decrypting = 0;
    dec->pos = 0;
}

static int dec_finish(struct bi2x_frame_decoder *dec)
{
    struct bi2x_packet *pkt = &dec->pkt;

    dec->state = DEC_IDLE;

    /* AIO_NODE_IOB2_COMMPKT::CheckRecvPkt: packets from a non-zero node
       must carry the response bit */
    if ((dec->hdr_addr & 0x7E) != 0 && (dec->hdr_addr & 1) == 0) {
        dec->frames_bad++;
        return 0;
    }

    dec->frames_ok++;

    /* mode 1 frames and "00 7F" notifications are swallowed by the host */
    if (pkt->mode == 1) {
        return 0;
    }

    if (pkt->len >= 2 && pkt->data[0] == 0x00 && pkt->data[1] == 0x7F) {
        return 0;
    }

    /* once a node has sent us an encrypted frame, we encrypt too */
    if (dec->decrypting && dec->cipher != NULL) {
        dec->cipher->enabled[pkt->node] = 1;
    }

    return 1;
}

int bi2x_frame_decoder_feed(struct bi2x_frame_decoder *dec, uint8_t raw)
{
    struct bi2x_packet *pkt = &dec->pkt;
    uint8_t b = raw;

    if (raw == 0xAA) {
        if (dec->state != DEC_IDLE) {
            dec->frames_bad++;
        }
        dec_start(dec);
        return 0;
    }

    if (dec->decrypting) {
        b = bi2x_cipher_byte(&dec->key, raw);
    }

    switch (dec->state) {
        case DEC_IDLE:
            dec->bytes_discarded++;
            return 0;

        case DEC_ADDR:
            dec->hdr_addr = b;
            dec->state = DEC_TAG;
            return 0;

        case DEC_TAG:
            dec->hdr_tag = b;
            dec->state = DEC_LEN;
            return 0;

        case DEC_LEN:
            dec->len_count++;

            if (b & 0x80) {
                if (!(b & 0x40) || dec->len_count > 4) {
                    goto bad;
                }
                dec->len = (dec->len << 6) | (b & 0x3F);
            } else {
                dec->len = (dec->len << 7) | (b & 0x7F);
                dec->state = DEC_FLAGS;
            }
            return 0;

        case DEC_FLAGS:
            dec->flags = b;

            if ((b & 0x0F) !=
                    bi2x_frame_head_sum(
                        dec->hdr_addr, dec->hdr_tag, dec->len_count,
                        dec->len, b)) {
                goto bad;
            }

            if ((b >> 5) > 3 || dec->len > BI2X_PAYLOAD_MAX) {
                goto bad;
            }

            pkt->node = (dec->hdr_addr >> 1) & 0x3F;
            pkt->response = dec->hdr_addr & 1;
            pkt->tag = dec->hdr_tag;
            pkt->mode = b >> 5;
            pkt->encrypted = (b & 0x10) ? 1 : 0;
            pkt->len = dec->len;
            dec->pos = 0;
            dec->escape = 0;

            if (pkt->encrypted && pkt->node != 0 && dec->cipher != NULL) {
                dec->decrypting = 1;
                dec->key = (uint32_t) (dec->hdr_tag ^ 0xAA);
            }

            if (pkt->len == 0) {
                return dec_finish(dec);
            }

            dec->state = pkt->mode == 3 ? DEC_SUBST : DEC_DATA;
            return 0;

        case DEC_SUBST:
            dec->subst = b;
            dec->state = DEC_DATA;
            return 0;

        case DEC_DATA:
            switch (pkt->mode) {
                case 0:
                    if (dec->escape) {
                        b = (uint8_t) ~b;
                        dec->escape = 0;
                    } else if (b == 0xFF) {
                        dec->escape = 1;
                        return 0;
                    }
                    break;

                case 3:
                    if (b == dec->subst) {
                        b = 0xAA;
                    }
                    break;

                default:
                    break;
            }

            pkt->data[dec->pos++] = b;

            if (dec->pos >= pkt->len) {
                dec->state = DEC_CRC;
            }
            return 0;

        case DEC_CRC:
            if (b != (bi2x_crc7(0x7F, pkt->data, pkt->len) ^ 0x7F)) {
                goto bad;
            }
            return dec_finish(dec);

        default:
            goto bad;
    }

bad:
    dec->frames_bad++;
    dec->state = DEC_IDLE;
    dec->decrypting = 0;
    return 0;
}
