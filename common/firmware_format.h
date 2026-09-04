/**
 * @file    firmware_format.h
 * @brief   On-flash firmware image header for signed application slots.
 *
 * The header is exactly 512 bytes so that the application's Cortex-M4 vector
 * table (which follows immediately) is 512-byte aligned, which is the
 * minimum alignment required by SCB->VTOR for an STM32F4 (>= 98 vectors).
 *
 * Layout:
 *   offset  size   field
 *   ------  ----   --------------------------------
 *      0     4     magic        = 0xDEADBEEF
 *      4     4     version      = 0xMMmmpp (semver, big-endian-like)
 *      8     4     image_size   bytes of payload (excludes this header)
 *     12     4     timestamp    Unix epoch seconds
 *     16     4     crc32        CRC32 of payload (fast pre-check)
 *     20    64     signature    ECDSA-P256 r||s, 32 B each, big-endian
 *     84   428     reserved     padding to 512 B (zeros)
 *
 * SHA-256 / ECDSA signing convention: the hash is computed over
 *   [header_with_signature_field_zeroed || payload]
 * so the verifier can recompute the same hash without first stripping the
 * signature.
 */
#ifndef FIRMWARE_FORMAT_H
#define FIRMWARE_FORMAT_H

#include <stdint.h>

#define FIRMWARE_HEADER_SIZE       512U
#define FIRMWARE_SIGNATURE_SIZE     64U
#define FIRMWARE_RESERVED_SIZE    (FIRMWARE_HEADER_SIZE \
                                   - 4U /* magic */     \
                                   - 4U /* version */   \
                                   - 4U /* image_size */\
                                   - 4U /* timestamp */ \
                                   - 4U /* crc32 */     \
                                   - FIRMWARE_SIGNATURE_SIZE)

typedef struct __attribute__((packed)) {
    uint32_t magic;        /**< Must equal FIRMWARE_MAGIC (0xDEADBEEF). */
    uint32_t version;      /**< Semver: 0x010203 = v1.2.3. */
    uint32_t image_size;   /**< Payload bytes following the header. */
    uint32_t timestamp;    /**< Unix epoch seconds at signing time. */
    uint32_t crc32;        /**< CRC32 of the payload (fast reject). */
    uint8_t  signature[FIRMWARE_SIGNATURE_SIZE]; /**< ECDSA-P256 r||s. */
    uint8_t  reserved[FIRMWARE_RESERVED_SIZE];   /**< Pads to 512 B. */
} FirmwareHeader_t;

_Static_assert(sizeof(FirmwareHeader_t) == FIRMWARE_HEADER_SIZE,
               "FirmwareHeader_t must be exactly 512 B (VTOR alignment)");
_Static_assert(FIRMWARE_RESERVED_SIZE == 428,
               "Reserved field arithmetic mismatch");

/**
 * @brief Maximum allowed payload size for one slot.
 *
 * Defined in terms of SLOT_SIZE from memory_map.h to keep the two in sync,
 * but we don't include memory_map.h here to keep firmware_format.h
 * self-contained. The static assert in crypto.c verifies the relationship.
 */
#define FIRMWARE_MAX_PAYLOAD_SIZE  ((460U * 1024U) - FIRMWARE_HEADER_SIZE)

#endif /* FIRMWARE_FORMAT_H */
