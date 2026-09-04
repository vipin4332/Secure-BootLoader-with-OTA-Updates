/**
 * @file    sha256.h
 * @brief   Standalone SHA-256 implementation (no external deps).
 *
 * Streaming API: init -> update (any number of times) -> final.
 * Based on the public-domain Brad Conte / Saju Pillai port; matches the
 * NIST FIPS 180-4 test vectors.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE  32
#define SHA256_BLOCK_SIZE   64

typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/** Convenience one-shot hasher. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* SHA256_H */
