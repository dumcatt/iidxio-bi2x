#ifndef BI2X_CRC_H
#define BI2X_CRC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Reflected ("LSB first") CRCs used by Konami's libacc (AC_CRC::*_MakeLGP_*).
 * The suffix is the reversed generator polynomial:
 *   Crc4_MakeLGP_C   -> CRC-4, reflected poly 0x0C (x^4 + x + 1)
 *   Crc7_MakeLGP_48  -> CRC-7, reflected poly 0x48 (x^7 + x^3 + 1)
 *   Crc16_MakeLGP_8408 -> CRC-16/CCITT reflected (only used for firmware images)
 */

/* Verified against libacc.dll (nibble-table implementation, tables built from
   the reflected polys). The MSB-first variant is kept only as a fallback the
   link layer can probe; it should never be selected. */
enum bi2x_crc_order {
    BI2X_CRC_LSB_FIRST = 0,
    BI2X_CRC_MSB_FIRST = 1,
};

void bi2x_crc_set_order(int crc4_order, int crc7_order);
void bi2x_crc_get_order(int *crc4_order, int *crc7_order);

uint8_t bi2x_crc4(uint8_t crc, const void *data, size_t len);
uint8_t bi2x_crc7(uint8_t crc, const void *data, size_t len);
uint16_t bi2x_crc16(uint16_t crc, const void *data, size_t len);

#endif
