#ifndef BI2X_LZ_H
#define BI2X_LZ_H

#include <stddef.h>
#include <stdint.h>

/*
 * Firmware "module" blobs embedded in libaio-iob*.dll are stored as:
 *
 *   u32 BE  decompressed size
 *   u8[]    scrambled LZSS stream
 *
 * Scrambling: key = 0; for each byte: plain = enc ^ key;
 *             key = ~ror8(plain, 1)
 *
 * LZSS (AC_LZ_INFLATE): 4 KiB ring buffer, zero-filled, write position
 * starting at 0xFEE. Flag byte, bits consumed LSB first, 1 = literal byte,
 * 0 = back-reference of two bytes b0 b1:
 *   ring position = ((b0 & 0x0F) << 8) | b1
 *   length        = (b0 >> 4) + 3
 */

/* Returns decompressed size, or 0 if the blob header is implausible. */
size_t bi2x_module_blob_size(const uint8_t *blob, size_t blob_len);

/* Decode a module blob into out (out_len must be bi2x_module_blob_size()).
   Returns 0 on success, -1 if the stream does not decode to exactly
   out_len bytes. */
int bi2x_module_blob_decode(
    const uint8_t *blob, size_t blob_len, uint8_t *out, size_t out_len);

#endif
