/**
 * @file    crc32.c
 * @brief   Compact, table-free, slice-by-1 CRC32 (IEEE 802.3 polynomial).
 *
 * No lookup table -> ~600 B of code with -O2 instead of 1 KB+ for the
 * tabular variant. CRC32 is on the slow path here (only used for the
 * fast-reject fast pre-check before SHA-256/ECDSA verify, and for boot
 * config integrity), so the throughput hit is acceptable.
 */

#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i) {
            uint32_t mask = -(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return crc;
}

uint32_t crc32(const void *data, size_t len)
{
    return crc32_finalize(crc32_update(CRC32_INIT, data, len));
}
