/**
 * @file    crypto.c
 * @brief   ECDSA-P256 / SHA-256 firmware verification, micro-ecc backend.
 */

#include "crypto.h"
#include "crc32.h"
#include "firmware_format.h"
#include "flash_driver.h"
#include "iwdg.h"
#include "memory_map.h"
#include "public_key.h"
#include "sha256.h"

#include "uECC.h"

#include <stddef.h>
#include <string.h>

/* Compile-time sanity: max payload fits in a slot. */
_Static_assert(FIRMWARE_MAX_PAYLOAD_SIZE + FIRMWARE_HEADER_SIZE <= SLOT_SIZE,
               "Header + payload must fit in a slot");

#define VERIFY_CHUNK_BYTES   1024U

/* Compute SHA-256 over [header with .signature zeroed || payload] */
static bool hash_signed_region(uint32_t slot_addr,
                               const FirmwareHeader_t *hdr,
                               uint8_t out[32])
{
    static const uint8_t zero_sig[FIRMWARE_SIGNATURE_SIZE] = { 0 };

    const size_t sig_offset = 20U;
    _Static_assert(offsetof(FirmwareHeader_t, signature) == 20U,
                   "Signature offset mismatch");

    sha256_ctx_t ctx;
    sha256_init(&ctx);

    uint8_t buf[VERIFY_CHUNK_BYTES];

    /* Header with zeroed signature field */
    const uint8_t *hdr_bytes = (const uint8_t *)hdr;
    sha256_update(&ctx, hdr_bytes, sig_offset);
    sha256_update(&ctx, zero_sig, sizeof(zero_sig));
    sha256_update(&ctx,
                  hdr_bytes + sig_offset + FIRMWARE_SIGNATURE_SIZE,
                  FIRMWARE_HEADER_SIZE - sig_offset - FIRMWARE_SIGNATURE_SIZE);

    /* Payload, streamed in chunks. */
    uint32_t payload_addr = slot_addr + FIRMWARE_HEADER_SIZE;
    uint32_t remaining    = hdr->image_size;
    while (remaining > 0U) {
        uint32_t chunk = remaining > VERIFY_CHUNK_BYTES
                         ? VERIFY_CHUNK_BYTES : remaining;
        if (flash_read(payload_addr, buf, chunk) != FLASH_OK) return false;
        sha256_update(&ctx, buf, chunk);
        iwdg_kick();
        payload_addr += chunk;
        remaining    -= chunk;
    }
    sha256_final(&ctx, out);
    return true;
}

/* SECP256R1 group order n divided by 2:
 * n   = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
 * n/2 = 0x7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8
 */
static const uint8_t k_secp256r1_half_order[32] = {
    0x7FU, 0xFFU, 0xFFU, 0xFFU, 0x80U, 0x00U, 0x00U, 0x00U,
    0x7FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xDEU, 0x73U, 0x7DU, 0x56U, 0xD3U, 0x8BU, 0xCFU, 0x42U,
    0x79U, 0xDCU, 0xE5U, 0x61U, 0x7EU, 0x31U, 0x92U, 0xA8U
};

/* Canonical low-S check (s <= n/2) to prevent malleability */
static bool is_canonical_low_s(const uint8_t sig[FIRMWARE_SIGNATURE_SIZE])
{
    const uint8_t *r = sig;
    const uint8_t *s = sig + 32;

    uint8_t r_or = 0U;
    uint8_t s_or = 0U;
    for (int i = 0; i < 32; ++i) {
        r_or |= r[i];
        s_or |= s[i];
    }
    if (r_or == 0U || s_or == 0U) {
        return false;
    }

    /* Constant-time borrow of (n/2 - s) */
    int32_t borrow = 0;
    for (int i = 31; i >= 0; --i) {
        int32_t diff = (int32_t)k_secp256r1_half_order[i] - (int32_t)s[i] - borrow;
        borrow = (diff < 0) ? 1 : 0;
    }
    return (borrow == 0);
}

bool crypto_constant_time_equal(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0U;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return (diff == 0U);
}

bool crypto_validate_public_key(void)
{
    if (uECC_valid_public_key(public_key_xy, uECC_secp256r1()) != 1) {
        return false;
    }

    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, public_key_xy, sizeof(public_key_xy));
    sha256_final(&ctx, hash);

    return crypto_constant_time_equal(hash, public_key_sha256, sizeof(hash));
}

bool crypto_verify_firmware(uint32_t slot_addr)
{
    if (!crypto_validate_public_key()) {
        return false;
    }

    /* Read header from flash */
    FirmwareHeader_t hdr;
    if (flash_read(slot_addr, (uint8_t *)&hdr, sizeof(hdr)) != FLASH_OK) {
        return false;
    }

    if (hdr.magic != FIRMWARE_MAGIC) return false;
    if (hdr.image_size == 0U) return false;
    if (hdr.image_size > FIRMWARE_MAX_PAYLOAD_SIZE) return false;

    /* CRC32 pre-check over payload */
    uint32_t crc_state    = CRC32_INIT;
    uint32_t payload_addr = slot_addr + FIRMWARE_HEADER_SIZE;
    uint32_t remaining    = hdr.image_size;
    uint8_t  buf[VERIFY_CHUNK_BYTES];
    while (remaining > 0U) {
        uint32_t chunk = remaining > VERIFY_CHUNK_BYTES
                         ? VERIFY_CHUNK_BYTES : remaining;
        if (flash_read(payload_addr, buf, chunk) != FLASH_OK) return false;
        crc_state = crc32_update(crc_state, buf, chunk);
        iwdg_kick();
        payload_addr += chunk;
        remaining    -= chunk;
    }
    if (crc32_finalize(crc_state) != hdr.crc32) return false;

    /* SHA-256 over [header(zero-sig) || payload] */
    uint8_t hash[32];
    if (!hash_signed_region(slot_addr, &hdr, hash)) return false;

    /* Verify signature */
    if (!is_canonical_low_s(hdr.signature)) return false;

    iwdg_kick();
    int ok = uECC_verify(public_key_xy,
                         hash, sizeof(hash),
                         hdr.signature,
                         uECC_secp256r1());
    iwdg_kick();
    return (ok == 1);
}
