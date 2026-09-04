/**
 * @file    delta_patch.h
 * @brief   Apply HPatchLite-format patch to reconstruct a signed image in the
 *          inactive slot (patch staged at the end of the slot, output at base).
 */
#ifndef DELTA_PATCH_H
#define DELTA_PATCH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief SHA-256 of the on-device old image: bytes from active slot address
 *        for (FIRMWARE_HEADER_SIZE + FirmwareHeader_t.image_size).
 *
 * @param active_slot_base   Flash base of the currently active slot.
 * @param expected_digest    Expected SHA-256 digest (32 bytes).
 * @return true if digest matches.
 */
bool delta_verify_active_base_sha256(uint32_t active_slot_base,
                                     const uint8_t expected_digest[32]);

/**
 * @brief Apply a staged patch at the tail of the inactive slot.
 *
 * Patch bytes occupy [inactive_slot_base + SLOT_SIZE - patch_total, ...).
 * Output is written sequentially from inactive_slot_base for @p expected_new_total bytes.
 *
 * Preconditions: slots verified non-overlapping layout;
 * expected_new_total + patch_total <= SLOT_SIZE.
 *
 * @param active_slot_base    Old firmware (read-only).
 * @param inactive_slot_base  Target slot base; receives reconstructed image at offset 0.
 * @param patch_total         Patch size in bytes (tail placement).
 * @param expected_new_total  Total signed image size after patch (must match hpatch header).
 * @return true on success (caller runs crypto_verify_firmware next).
 */
bool delta_apply_patch(uint32_t active_slot_base,
                       uint32_t inactive_slot_base,
                       uint32_t patch_total,
                       uint32_t expected_new_total);

#endif /* DELTA_PATCH_H */
