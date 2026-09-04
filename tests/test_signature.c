/*
 * test_signature.c - Host-compiled signature verification tests.
 *
 * Links against the same crypto.c / sha256.c / crc32.c / uECC.c that the
 * bootloader uses. The test:
 *
 *   1. Loads keys/public_key.pem (via a small embedded copy emitted at
 *      build time as `bootloader/include/public_key.h`).
 *   2. Reads `build/app_signed.bin` produced by the normal build pipeline
 *      and checks that crypto_verify_firmware() returns true on it.
 *   3. Flips a single byte in the payload and confirms rejection.
 *   4. Flips a byte in the signature and confirms rejection.
 *   5. Flips the magic and confirms rejection.
 *
 * The image is loaded into a 460 KB heap buffer at any address; we treat
 * that buffer's start as the slot address from the verifier's perspective.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "crypto.h"
#include "firmware_format.h"
#include "memory_map.h"

static uint8_t *g_slot_buf = NULL;
static size_t   g_slot_len = 0;
static bool     g_slot_is_mmap = false;

static uint8_t *alloc_slot_buffer(size_t size)
{
#if defined(__linux__) && defined(__x86_64__)
    void *p;

    p = mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED) {
        g_slot_is_mmap = true;
        return (uint8_t *)p;
    }
#endif
    {
        uint8_t *p;

        p = aligned_alloc(64, size);
        if (p == NULL) {
            return NULL;
        }
        if ((uintptr_t)p > UINT32_MAX) {
            free(p);
            return NULL;
        }
        g_slot_is_mmap = false;
        return p;
    }
}

static void free_slot_buffer(uint8_t *p, size_t size)
{
#if defined(__linux__) && defined(__x86_64__)
    if (g_slot_is_mmap && (p != NULL)) {
        (void)munmap(p, size);
        return;
    }
#endif
    free(p);
}

static bool load_image(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < (long)FIRMWARE_HEADER_SIZE) {
        fprintf(stderr, "FAIL: %s too small (%ld B)\n", path, sz);
        fclose(fp);
        return false;
    }
    g_slot_buf = alloc_slot_buffer((size_t)sz);
    if (!g_slot_buf) { fclose(fp); return false; }
    if (fread(g_slot_buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); return false;
    }
    fclose(fp);
    g_slot_len = (size_t)sz;
    return true;
}

static bool verify_buf(void)
{
    return crypto_verify_firmware((uint32_t)(uintptr_t)g_slot_buf);
}

static int run_one(const char *name, bool expect_ok, int (*mutator)(void))
{
    /* Snapshot current contents so the test is reentrant. */
    uint8_t *snap = malloc(g_slot_len);
    memcpy(snap, g_slot_buf, g_slot_len);

    if (mutator) mutator();
    bool got = verify_buf();
    bool pass = (got == expect_ok);
    printf("  %-30s expect=%s got=%s -> %s\n",
           name,
           expect_ok ? "ACCEPT" : "REJECT",
           got       ? "ACCEPT" : "REJECT",
           pass ? "PASS" : "FAIL");

    /* Restore. */
    memcpy(g_slot_buf, snap, g_slot_len);
    free(snap);
    return pass ? 0 : 1;
}

static int flip_first_payload_byte(void) {
    g_slot_buf[FIRMWARE_HEADER_SIZE] ^= 0x01; return 0;
}
static int flip_signature_byte(void) {
    /* signature is at offset 20 in the header. */
    g_slot_buf[20] ^= 0x80; return 0;
}
static int corrupt_magic(void) {
    g_slot_buf[0] ^= 0xFF; return 0;
}
static int truncate_size(void) {
    /* image_size is at header offset 8 (LE). Set it to 0. */
    g_slot_buf[8] = 0; g_slot_buf[9] = 0; g_slot_buf[10] = 0; g_slot_buf[11] = 0;
    return 0;
}
static int flip_s_to_malleable(void) {
    /* Signature is at offset 20 in the header.
     * s is at offset 20 + 32 = 52.
     * Invert s modulo curve order n: s' = n - s.
     */
    static const uint8_t n[32] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xBCU, 0xE6U, 0xFAU, 0xADU, 0xA7U, 0x17U, 0x9EU, 0x84U,
        0xF3U, 0xB9U, 0xCAU, 0xC2U, 0xFCU, 0x63U, 0x25U, 0x51U
    };
    uint8_t *s = &g_slot_buf[52];
    int32_t borrow = 0;
    for (int i = 31; i >= 0; --i) {
        int32_t diff = (int32_t)n[i] - (int32_t)s[i] - borrow;
        if (diff < 0) {
            diff += 256;
            borrow = 1;
        } else {
            borrow = 0;
        }
        s[i] = (uint8_t)diff;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = (argc > 1) ? argv[1] : "build/app_signed.bin";
    if (!load_image(image)) return 2;

    printf("test_signature: image=%s (%zu B)\n", image, g_slot_len);

    int failed = 0;
    failed += run_one("baseline accept",          true,  NULL);
    failed += run_one("flip first payload byte",  false, flip_first_payload_byte);
    failed += run_one("flip signature byte",      false, flip_signature_byte);
    failed += run_one("corrupt magic",            false, corrupt_magic);
    failed += run_one("zero image_size field",    false, truncate_size);
    failed += run_one("reject malleable s > n/2", false, flip_s_to_malleable);

    free_slot_buffer(g_slot_buf, g_slot_len);
    if (failed) {
        printf("\n%d FAILED\n", failed);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
