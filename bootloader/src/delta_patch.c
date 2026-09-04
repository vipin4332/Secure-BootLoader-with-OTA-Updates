/**
 * @file    delta_patch.c
 * @brief   HPatchLite patch apply (uncompressed or tinyuz-compressed).
 */

#include "crypto.h"
#include "delta_patch.h"

#include "firmware_format.h"
#include "flash_driver.h"
#include "hpatch_decompress_tuz.h"
#include "iwdg.h"
#include "memory_map.h"
#include "sha256.h"
#include "uart.h"

#include "hpatch_lite.h"

#include <stddef.h>
#include <string.h>

#ifndef DELTA_PATCH_CACHE_SIZE
#define DELTA_PATCH_CACHE_SIZE (1024U * 16U)
#endif

#ifndef DELTA_TUZ_DICT_MAX
#define DELTA_TUZ_DICT_MAX (8U * 1024U)
#endif

#ifndef DELTA_TUZ_DEC_BUF
#define DELTA_TUZ_DEC_BUF 1024U
#endif

typedef struct {
    hpatchi_listener_t base;
    uint32_t           active_base;
    uint32_t           inactive_base;
    uint32_t           new_written;
    uint32_t           new_expected;
    uint32_t           active_logical_total;
} delta_listener_t;

typedef struct {
    uint32_t base;
    uint32_t pos;
    uint32_t len;
} flash_diff_stream_t;

static hpi_BOOL diff_stream_read(hpi_TInputStreamHandle stream,
                                 hpi_byte *out_data,
                                 hpi_size_t *data_size)
{
    flash_diff_stream_t *s = (flash_diff_stream_t *)stream;
    hpi_size_t           want;

    if (*data_size == 0U) {
        return hpi_TRUE;
    }
    want = *data_size;
    uint32_t rem = s->len - s->pos;

    if (want > rem) {
        want = (hpi_size_t)rem;
    }
    if (want == 0U) {
        *data_size = 0;
        return hpi_FALSE;
    }
    if (flash_read(s->base + s->pos, out_data, (uint32_t)want) != FLASH_OK) {
        return hpi_FALSE;
    }
    s->pos += (uint32_t)want;
    *data_size = want;
    return hpi_TRUE;
}

static delta_listener_t *listener_from(hpatchi_listener_t *p)
{
    return (delta_listener_t *)((uint8_t *)p - offsetof(delta_listener_t, base));
}

static hpi_BOOL read_old_cb(hpatchi_listener_t *listener,
                            hpi_pos_t read_from_pos,
                            hpi_byte *out_data,
                            hpi_size_t data_size)
{
    delta_listener_t *L = listener_from(listener);

    if (read_from_pos > L->active_logical_total) {
        return hpi_FALSE;
    }
    if (data_size > L->active_logical_total - read_from_pos) {
        return hpi_FALSE;
    }

    if (flash_read(L->active_base + read_from_pos, out_data, (uint32_t)data_size)
        != FLASH_OK) {
        return hpi_FALSE;
    }
    iwdg_kick();
    return hpi_TRUE;
}

static hpi_BOOL write_new_cb(hpatchi_listener_t *listener,
                             const hpi_byte *data,
                             hpi_size_t data_size)
{
    delta_listener_t *L = listener_from(listener);

    if ((uint32_t)data_size > (L->new_expected - L->new_written)) {
        return hpi_FALSE;
    }
    if (flash_program_bytes(L->inactive_base + L->new_written, data,
                            (uint32_t)data_size)
        != FLASH_OK) {
        return hpi_FALSE;
    }
    L->new_written += (uint32_t)data_size;
    iwdg_kick();
    return hpi_TRUE;
}

bool delta_verify_active_base_sha256(uint32_t active_slot_base,
                                     const uint8_t expected_digest[32])
{
    FirmwareHeader_t hdr;

    if (flash_read(active_slot_base, (uint8_t *)&hdr, sizeof(hdr)) != FLASH_OK) {
        return false;
    }
    if (hdr.magic != FIRMWARE_MAGIC) {
        return false;
    }
    uint32_t total = FIRMWARE_HEADER_SIZE + hdr.image_size;
    if (total > SLOT_SIZE) {
        return false;
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);

    uint32_t off = 0U;
    uint8_t  buf[512];

    while (off < total) {
        uint32_t chunk = total - off;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (flash_read(active_slot_base + off, buf, chunk) != FLASH_OK) {
            return false;
        }
        sha256_update(&ctx, buf, chunk);
        off += chunk;
        iwdg_kick();
    }

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, digest);
    /* Constant-time compare */
    return crypto_constant_time_equal(digest, expected_digest, SHA256_DIGEST_SIZE);
}

static bool delta_run_patch(delta_listener_t *L,
                          flash_diff_stream_t *stream,
                          hpi_compressType ct,
                          hpi_pos_t newSize)
{
    static uint8_t temp_cache[DELTA_PATCH_CACHE_SIZE];
    static uint8_t tuz_work[DELTA_TUZ_DICT_MAX + DELTA_TUZ_DEC_BUF];
    static tuz_TStream tuzStream;

    hpi_TInputStreamHandle diff_handle = stream;
    hpi_TInputStream_read  diff_read   = diff_stream_read;

    if (ct == hpi_compressType_tuz) {
        const hpi_size_t dictSize =
            hpatch_tuz_reserved_mem_size(stream, diff_stream_read);
        if (dictSize == 0U || dictSize > DELTA_TUZ_DICT_MAX) {
            uart_log_puts("DELTA: tuz dict size out of range\r\n");
            return false;
        }
        if (tuz_OK
            != tuz_TStream_open(&tuzStream, stream, diff_stream_read, tuz_work,
                                (tuz_size_t)dictSize, DELTA_TUZ_DEC_BUF)) {
            uart_log_puts("DELTA: tuz decompressor open failed\r\n");
            return false;
        }
        diff_handle = &tuzStream;
        diff_read   = hpatch_tuz_stream_decompress;
    } else if (ct != hpi_compressType_no) {
        uart_log_puts("DELTA: unsupported compress type\r\n");
        return false;
    }

    L->base.diff_data = diff_handle;
    L->base.read_diff = diff_read;

    if (!hpatch_lite_patch(&L->base, newSize, temp_cache, sizeof(temp_cache))) {
        return false;
    }
    return (L->new_written == L->new_expected) && (newSize == (hpi_pos_t)L->new_written);
}

bool delta_apply_patch(uint32_t active_slot_base,
                       uint32_t inactive_slot_base,
                       uint32_t patch_total,
                       uint32_t expected_new_total)
{
    FirmwareHeader_t active_hdr;

    if (patch_total == 0U || expected_new_total == 0U) {
        return false;
    }
    const uint32_t slot_span = flash_ota_slot_span();
    if (expected_new_total + patch_total > slot_span) {
        return false;
    }

    if (flash_read(active_slot_base, (uint8_t *)&active_hdr, sizeof(active_hdr))
        != FLASH_OK) {
        return false;
    }
    if (active_hdr.magic != FIRMWARE_MAGIC || active_hdr.image_size == 0U) {
        return false;
    }
    uint32_t active_logical_total =
        FIRMWARE_HEADER_SIZE + active_hdr.image_size;
    if (active_logical_total > SLOT_SIZE) {
        return false;
    }

    flash_diff_stream_t stream;
    stream.base = inactive_slot_base + slot_span - patch_total;
    stream.pos  = 0U;
    stream.len  = patch_total;

    delta_listener_t L;
    memset(&L, 0, sizeof(L));
    L.active_base          = active_slot_base;
    L.inactive_base        = inactive_slot_base;
    L.new_expected         = expected_new_total;
    L.active_logical_total = active_logical_total;
    L.base.read_old        = read_old_cb;
    L.base.write_new       = write_new_cb;

    hpi_compressType ct;
    hpi_pos_t        newSize;
    hpi_pos_t        uncSize;

    if (!hpatch_lite_open(&stream, diff_stream_read, &ct, &newSize, &uncSize)) {
        return false;
    }
    if ((uint32_t)newSize != expected_new_total) {
        return false;
    }

    return delta_run_patch(&L, &stream, ct, newSize);
}
