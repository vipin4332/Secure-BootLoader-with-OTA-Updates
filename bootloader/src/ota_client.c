/**
 * @file    ota_client.c
 * @brief   UART-bridged OTA receiver. See ota_client.h for the wire format.
 */

#include "ota_client.h"

#include "config.h"
#include "crc32.h"
#include "crypto.h"
#include "delta_patch.h"
#include "firmware_format.h"
#include "flash_driver.h"
#include "iwdg.h"
#include "memory_map.h"
#include "shared_memory.h"
#include "uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Inter-byte timeout while reading a single frame (host should be back-to-
 * back within ~50 ms even on slow links). */
#define OTA_BYTE_TIMEOUT_MS   500U

/* Forward decl from main.c. */
__attribute__((noreturn)) void bootloader_system_reset(void);

/* ===================================================================== */
/* Per-transfer state                                                       */
/* ===================================================================== */
typedef enum {
    OTA_KIND_FULL = 0,
    OTA_KIND_DELTA = 1,
} ota_kind_t;

typedef struct {
    bool       active; /* between START / START_DELTA and END */
    ota_kind_t kind;
    uint32_t   slot_addr; /* inactive slot base */
    uint32_t   xfer_total; /* full image size, or patch blob size for delta */
    uint32_t   written;
    uint16_t   expected_seq;
    uint32_t   patch_tail_base; /* delta: flash addr of first patch byte */
    uint32_t   expected_new_total; /* delta: reconstructed signed image size */
    uint32_t   active_slot_base; /* delta: running firmware slot base */
} ota_state_t;

static ota_state_t g_state;

static void send_response(uint8_t code, uint16_t seq)
{
    uint8_t buf[3];
    buf[0] = code;
    buf[1] = (uint8_t)(seq & 0xFFU);
    buf[2] = (uint8_t)((seq >> 8) & 0xFFU);
    uart_ota_write(buf, sizeof(buf));
}

static inline uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] <<  8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

/* ===================================================================== */
/* Frame reader                                                             */
/* ===================================================================== */
typedef struct {
    uint8_t  op;
    uint16_t seq;
    uint16_t len;
    uint8_t  data[OTA_MAX_PAYLOAD];
} ota_frame_t;

typedef enum {
    FRAME_OK = 0,
    FRAME_TIMEOUT,
    FRAME_CRC_BAD,
    FRAME_TOO_BIG,
    FRAME_BAD_SOH,
} frame_status_t;

static frame_status_t read_frame(ota_frame_t *frame)
{
    /* Resync to SOH. */
    uint8_t b;
    while (true) {
        if (!uart_ota_getc(&b, OTA_BYTE_TIMEOUT_MS)) return FRAME_TIMEOUT;
        if (b == OTA_FRAME_SOH) break;
        /* Non-SOH bytes are silently dropped (host may resync). */
    }

    uint8_t hdr[5]; /* OP + SEQ(2) + LEN(2) */
    if (uart_ota_read(hdr, sizeof(hdr), OTA_BYTE_TIMEOUT_MS) != sizeof(hdr)) {
        return FRAME_TIMEOUT;
    }
    frame->op  = hdr[0];
    frame->seq = rd_u16_le(&hdr[1]);
    frame->len = rd_u16_le(&hdr[3]);

    if (frame->len > OTA_MAX_PAYLOAD) return FRAME_TOO_BIG;

    if (frame->len > 0U) {
        if (uart_ota_read(frame->data, frame->len, OTA_BYTE_TIMEOUT_MS)
            != frame->len) {
            return FRAME_TIMEOUT;
        }
    }

    uint8_t crc_buf[4];
    if (uart_ota_read(crc_buf, sizeof(crc_buf), OTA_BYTE_TIMEOUT_MS)
        != sizeof(crc_buf)) {
        return FRAME_TIMEOUT;
    }
    uint32_t crc_rx = rd_u32_le(crc_buf);

    /* CRC covers OP || SEQ || LEN || DATA. */
    uint32_t crc = CRC32_INIT;
    crc = crc32_update(crc, hdr, sizeof(hdr));
    if (frame->len) crc = crc32_update(crc, frame->data, frame->len);
    crc = crc32_finalize(crc);
    if (crc != crc_rx) return FRAME_CRC_BAD;

    return FRAME_OK;
}

/* ===================================================================== */
/* Per-opcode handlers                                                      */
/* ===================================================================== */
static bool handle_start(const ota_frame_t *f)
{
    if (f->len < 4U) return false;

    /* Pick the inactive slot from the live boot config. */
    BootConfig_t cfg;
    boot_config_load(&cfg);
    uint8_t target = memory_map_other_slot(cfg.active_slot);
    uint32_t slot_addr = memory_map_slot_addr(target);
    uint32_t total = rd_u32_le(&f->data[0]);

    if (total < FIRMWARE_HEADER_SIZE ||
        total > (FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_PAYLOAD_SIZE)) {
        uart_log_printf("OTA : reject START total_size=%u out of range\r\n",
                        (unsigned)total);
        return false;
    }

    uart_log_printf("OTA : START slot=%c addr=0x%08x total=%u\r\n",
                    (char)target, (unsigned)slot_addr, (unsigned)total);

    if (flash_erase_range(slot_addr, total) != FLASH_OK) {
        uart_log_puts("OTA : erase failed\r\n");
        return false;
    }

    g_state.active       = true;
    g_state.kind         = OTA_KIND_FULL;
    g_state.slot_addr    = slot_addr;
    g_state.xfer_total   = total;
    g_state.written      = 0U;
    g_state.expected_seq = (uint16_t)(f->seq + 1U);
    return true;
}

/** START_DELTA payload: patch_total (u32), expected_new_total (u32), base_sha256 (32). */
#define OTA_START_DELTA_PAYLOAD  (4U + 4U + 32U)

static bool handle_start_delta(const ota_frame_t *f)
{
    if (f->len < OTA_START_DELTA_PAYLOAD) {
        return false;
    }

    BootConfig_t cfg;
    boot_config_load(&cfg);
    uint8_t  target    = memory_map_other_slot(cfg.active_slot);
    uint32_t slot_addr = memory_map_slot_addr(target);
    uint32_t patch_total = rd_u32_le(&f->data[0]);
    uint32_t expected_new = rd_u32_le(&f->data[4]);
    const uint8_t *digest = &f->data[8];

    const uint32_t slot_span = flash_ota_slot_span();
    if (patch_total == 0U || patch_total > slot_span) {
        uart_log_puts("OTA : START_DELTA bad patch_total\r\n");
        return false;
    }
    if (expected_new < FIRMWARE_HEADER_SIZE ||
        expected_new > (FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_PAYLOAD_SIZE)) {
        uart_log_puts("OTA : START_DELTA bad expected_new_total\r\n");
        return false;
    }
    if (expected_new + patch_total > slot_span) {
        uart_log_puts("OTA : START_DELTA patch+new overlap slot\r\n");
        return false;
    }

    uint32_t active_base = memory_map_slot_addr(cfg.active_slot);
    if (!delta_verify_active_base_sha256(active_base, digest)) {
        uart_log_puts("OTA : START_DELTA base SHA-256 mismatch\r\n");
        return false;
    }

    uart_log_printf(
        "OTA : START_DELTA slot=%c inact=0x%08x patch=%u new=%u\r\n",
        (char)target, (unsigned)slot_addr, (unsigned)patch_total,
        (unsigned)expected_new);

    if (flash_erase_range(slot_addr, slot_span) != FLASH_OK) {
        uart_log_puts("OTA : erase inactive slot failed\r\n");
        return false;
    }

    g_state.active             = true;
    g_state.kind               = OTA_KIND_DELTA;
    g_state.slot_addr          = slot_addr;
    g_state.xfer_total         = patch_total;
    g_state.expected_new_total = expected_new;
    g_state.active_slot_base   = active_base;
    g_state.patch_tail_base    = slot_addr + slot_span - patch_total;
    g_state.written            = 0U;
    g_state.expected_seq       = (uint16_t)(f->seq + 1U);
    return true;
}

static bool handle_data(const ota_frame_t *f)
{
    uint32_t dest;

    if (!g_state.active) return false;
    if (f->seq != g_state.expected_seq) return false;
    if (f->len == 0U) return false;
    if (g_state.written + f->len > g_state.xfer_total) return false;

    if (g_state.kind == OTA_KIND_DELTA) {
        dest = g_state.patch_tail_base + g_state.written;
    } else {
        dest = g_state.slot_addr + g_state.written;
    }

    if (flash_program_bytes(dest, f->data, f->len) != FLASH_OK) {
        uart_log_puts("OTA : flash write failed\r\n");
        return false;
    }
    g_state.written += f->len;
    g_state.expected_seq = (uint16_t)(g_state.expected_seq + 1U);
    return true;
}

static bool handle_end(void)
{
    if (!g_state.active) return false;
    if (g_state.written != g_state.xfer_total) {
        uart_log_printf("OTA : END short, got %u of %u\r\n",
                        (unsigned)g_state.written,
                        (unsigned)g_state.xfer_total);
        return false;
    }

    if (g_state.kind == OTA_KIND_DELTA) {
        if (!delta_apply_patch(g_state.active_slot_base, g_state.slot_addr,
                               g_state.xfer_total,
                               g_state.expected_new_total)) {
            uart_log_puts("OTA : delta apply failed\r\n");
            return false;
        }
    }

    if (!crypto_verify_firmware(g_state.slot_addr)) {
        uart_log_puts("OTA : verify failed; staying on active slot\r\n");
        return false;
    }

    FirmwareHeader_t hdr;
    if (flash_read(g_state.slot_addr, (uint8_t *)&hdr, sizeof(hdr)) != FLASH_OK
        || hdr.magic != FIRMWARE_MAGIC) {
        uart_log_puts("OTA : header read failed after verify\r\n");
        return false;
    }
    BootConfig_t cfg;
    boot_config_load(&cfg);
    if (!boot_config_firmware_allowed(&cfg, hdr.version)) {
        uart_log_printf(
            "OTA : reject version 0x%08x (floor 0x%08x)\r\n",
            (unsigned)hdr.version,
            (unsigned)boot_config_antidowngrade_floor(&cfg));
        return false;
    }
    /* Mark OTA pending so the bootloader swaps on the next boot. */
    SHARED_BOOT_BLOCK.magic       = SHARED_BLOCK_MAGIC;
    SHARED_BOOT_BLOCK.ota_pending = 1U;
    SHARED_BOOT_BLOCK.boot_confirmed = 0U;
    uart_log_puts("OTA : END verified; rebooting\r\n");
    return true;
}

/* ===================================================================== */
/* Public entrypoint                                                        */
/* ===================================================================== */
bool ota_client_receive(void)
{
    memset(&g_state, 0, sizeof(g_state));
    uart_log_puts("OTA : ready, awaiting frames on USART2 (TCP 4444 in QEMU)\r\n");

    for (uint32_t consec_errs = 0; consec_errs < 16U; ) {
        iwdg_kick();
        ota_frame_t frame;
        frame_status_t s = read_frame(&frame);
        if (s != FRAME_OK) {
            ++consec_errs;
            /* Best-effort NAK with seq=0 since we don't know the seq. */
            if (s == FRAME_CRC_BAD || s == FRAME_TOO_BIG) {
                send_response(OTA_RESP_NAK, 0U);
            }
            /* Timeout / bad SOH: stay quiet and let host retry. */
            continue;
        }

        bool ok = false;
        switch (frame.op) {
        case OTA_OP_START:
            ok = handle_start(&frame);
            break;
        case OTA_OP_START_DELTA:
            ok = handle_start_delta(&frame);
            break;
        case OTA_OP_DATA:  ok = handle_data(&frame);           break;
        case OTA_OP_END:   ok = handle_end();                  break;
        case OTA_OP_ABORT:
            uart_log_puts("OTA : ABORT received\r\n");
            send_response(OTA_RESP_ACK, frame.seq);
            return false;
        default:           ok = false;                          break;
        }

        send_response(ok ? OTA_RESP_ACK : OTA_RESP_NAK, frame.seq);
        if (ok) {
            consec_errs = 0;
            if (frame.op == OTA_OP_END) {
                /* Give the ACK time to drain on the wire, then reset. */
                for (volatile uint32_t i = 0; i < 200000U; ++i) { }
                bootloader_system_reset();
                return true;  /* unreachable */
            }
        } else {
            ++consec_errs;
        }
    }

    uart_log_puts("OTA : too many consecutive errors; aborting\r\n");
    return false;
}
