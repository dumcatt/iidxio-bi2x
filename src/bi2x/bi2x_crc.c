#include "bi2x_crc.h"

static int crc4_order = BI2X_CRC_LSB_FIRST;
static int crc7_order = BI2X_CRC_LSB_FIRST;

void bi2x_crc_set_order(int order4, int order7)
{
    crc4_order = order4;
    crc7_order = order7;
}

void bi2x_crc_get_order(int *order4, int *order7)
{
    *order4 = crc4_order;
    *order7 = crc7_order;
}

/* MSB-first CRC of `width` bits with the normal (non reflected) poly */
static uint32_t crc_normal(
    uint32_t crc, uint32_t poly, int width, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t top = 1u << (width - 1);
    uint32_t mask = (1u << width) - 1;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        for (bit = 7; bit >= 0; bit--) {
            uint32_t fb = ((crc & top) ? 1u : 0u) ^ ((p[i] >> bit) & 1u);

            crc = (crc << 1) & mask;

            if (fb) {
                crc ^= poly;
            }
        }
    }

    return crc;
}

static uint32_t crc_reflected(
    uint32_t crc, uint32_t poly, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= p[i];

        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ poly : (crc >> 1);
        }
    }

    return crc;
}

uint8_t bi2x_crc4(uint8_t crc, const void *data, size_t len)
{
    if (crc4_order == BI2X_CRC_MSB_FIRST) {
        return (uint8_t) crc_normal(crc & 0x0F, 0x03, 4, data, len);
    }

    return (uint8_t) (crc_reflected(crc & 0x0F, 0x0C, data, len) & 0x0F);
}

uint8_t bi2x_crc7(uint8_t crc, const void *data, size_t len)
{
    if (crc7_order == BI2X_CRC_MSB_FIRST) {
        return (uint8_t) crc_normal(crc & 0x7F, 0x09, 7, data, len);
    }

    return (uint8_t) (crc_reflected(crc & 0x7F, 0x48, data, len) & 0x7F);
}

uint16_t bi2x_crc16(uint16_t crc, const void *data, size_t len)
{
    return (uint16_t) crc_reflected(crc, 0x8408, data, len);
}
