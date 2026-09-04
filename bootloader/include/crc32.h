/**
 * @file    crc32.h
 * @brief   IEEE 802.3 CRC32 (polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR
 *          0xFFFFFFFF) - matches Python's `zlib.crc32` and `binascii.crc32`.
 *
 * Streaming form to avoid buffering large payloads. The same polynomial is
 * used for boot config integrity, firmware payload fast-reject, and OTA
 * frame integrity.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/** Initial value passed to crc32_update() for a new computation. */
#define CRC32_INIT  0xFFFFFFFFUL

/** One-shot CRC32 over @p data of @p len bytes. */
uint32_t crc32(const void *data, size_t len);

/** Streaming update: feed bytes into a running CRC. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

/** Finalise a running CRC (apply the final XOR with 0xFFFFFFFF). */
static inline uint32_t crc32_finalize(uint32_t crc) { return crc ^ 0xFFFFFFFFUL; }

#endif /* CRC32_H */
