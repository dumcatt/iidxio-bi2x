#include <string.h>

#include "bi2x_lz.h"

#define LZ_RING_SIZE 4096
#define LZ_RING_START 0xFEE
#define LZ_MAX_MODULE (4u * 1024u * 1024u)

size_t bi2x_module_blob_size(const uint8_t *blob, size_t blob_len)
{
    uint32_t size;

    if (blob == NULL || blob_len < 5) {
        return 0;
    }

    size = ((uint32_t) blob[0] << 24) | ((uint32_t) blob[1] << 16) |
        ((uint32_t) blob[2] << 8) | (uint32_t) blob[3];

    if (size == 0 || size > LZ_MAX_MODULE) {
        return 0;
    }

    return size;
}

int bi2x_module_blob_decode(
    const uint8_t *blob, size_t blob_len, uint8_t *out, size_t out_len)
{
    uint8_t ring[LZ_RING_SIZE];
    unsigned int r = LZ_RING_START;
    uint8_t key = 0;
    size_t in_pos = 4;
    size_t out_pos = 0;
    unsigned int flags = 0;
    int flag_bits = 0;

    if (bi2x_module_blob_size(blob, blob_len) != out_len) {
        return -1;
    }

    memset(ring, 0, sizeof(ring));

/* fetch the next descrambled input byte into v, or stop at end of input */
#define LZ_NEXT(v)                                                         \
    do {                                                                   \
        uint8_t plain_;                                                    \
        if (in_pos >= blob_len) {                                          \
            goto done;                                                     \
        }                                                                  \
        plain_ = blob[in_pos++] ^ key;                                     \
        key = (uint8_t) ~((uint8_t) ((plain_ >> 1) | (plain_ << 7)));      \
        (v) = plain_;                                                      \
    } while (0)

    while (out_pos < out_len) {
        uint8_t b0;
        uint8_t b1;

        if (flag_bits == 0) {
            uint8_t f;
            LZ_NEXT(f);
            flags = f;
            flag_bits = 8;
        }

        flag_bits--;

        if (flags & 1) {
            LZ_NEXT(b0);
            out[out_pos++] = b0;
            ring[r] = b0;
            r = (r + 1) & (LZ_RING_SIZE - 1);
        } else {
            unsigned int pos;
            unsigned int len;
            unsigned int i;

            LZ_NEXT(b0);
            LZ_NEXT(b1);

            pos = ((unsigned int) (b0 & 0x0F) << 8) | b1;
            len = (unsigned int) (b0 >> 4) + 3;

            for (i = 0; i < len && out_pos < out_len; i++) {
                uint8_t c = ring[(pos + i) & (LZ_RING_SIZE - 1)];
                out[out_pos++] = c;
                ring[r] = c;
                r = (r + 1) & (LZ_RING_SIZE - 1);
            }
        }

        flags >>= 1;
    }

#undef LZ_NEXT

done:
    return out_pos == out_len ? 0 : -1;
}
