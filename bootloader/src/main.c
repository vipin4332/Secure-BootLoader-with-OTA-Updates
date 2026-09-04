/**
 * @file    main.c
 * @brief   Bootloader entry point and boot state machine.
 *
 * The state machine implements the architecture defined in docs/architecture.md:
 *
 *   1. App-confirmation check immediately after loading config
 *      (boot_confirmed -> reset attempts + stamp last_good_boot, then
 *      always clear the flag so a stale value can't carry across cycles).
 *   2. OTA-pending swap with post-swap re-verify and revert-on-failure.
 *   3. Slot switch on attempts > 3 resets boot_attempts to 0 before
 *      re-entering verify-active to prevent slot ping-ponging.
 *   4. Recovery mode reachable only after both slots fail verification.
 *
 * In addition, every pass through the state machine is bounded by an
 * iteration cap as a defense-in-depth backstop.
 */

#include "bootloader.h"
#include "config.h"
#include "crypto.h"
#include "firmware_format.h"
#include "flash_driver.h"
#include "iwdg.h"
#include "memory_map.h"
#include "shared_memory.h"
#include "uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- ARM Cortex-M system register fragments we need without a CMSIS dep --- */
#define SCB_VTOR  (*(volatile uint32_t *)0xE000ED08UL)
#define SCB_AIRCR (*(volatile uint32_t *)0xE000ED0CUL)
#define AIRCR_KEY        (0x05FA0000UL)
#define AIRCR_SYSRESETREQ (1UL <<  2)

#define MAX_SWAP_RETRIES   4   /**< hard iteration cap; backstop only. */
#define MAX_BOOT_ATTEMPTS  3   /**< if attempts > this, switch slot. */

static void log_banner(const BootConfig_t *cfg)
{
    uart_log_puts("\r\n");
    uart_log_puts("==============================================\r\n");
    uart_log_puts(" Secure Dual-Bank Bootloader (P-256 + SHA-256)\r\n");
    uart_log_puts("==============================================\r\n");
    uart_log_printf(
        " active=%c  attempts=%u  ver_a=0x%08x  ver_b=0x%08x\r\n",
        (char)cfg->active_slot,
        (unsigned)cfg->boot_attempts,
        (unsigned)cfg->slot_a_version,
        (unsigned)cfg->slot_b_version);
}

/**
 * @brief Persist a config; on failure, log and continue. We deliberately do
 *        not panic here - the bootloader must keep running even if flash
 *        is flaky, so that we can fall through to recovery mode.
 */
static void save_or_warn(const BootConfig_t *cfg, const char *what)
{
    if (!boot_config_save(cfg)) {
        uart_log_printf("WARN: failed to persist config (%s)\r\n", what);
    }
}

/* ===================================================================== */
/* Application confirmation check                                          */
/* ===================================================================== */
/**
 * Reads SHARED_BOOT_BLOCK and updates @p cfg in place if the previous boot
 * confirmed itself. Always clears the SharedBootBlock flags before returning
 * so a stale value cannot survive into a future boot.
 */
static void check_app_confirmation(BootConfig_t *cfg, bool *cfg_dirty)
{
    if (SHARED_BOOT_BLOCK.magic == SHARED_BLOCK_MAGIC &&
        SHARED_BOOT_BLOCK.boot_confirmed != 0U) {
        cfg->boot_attempts = 0U;
        /* Stamp a synthetic timestamp; on real hardware this would come
         * from RTC or a build-time constant. */
        cfg->last_good_boot += 1U;
        *cfg_dirty = true;
        uart_log_puts("INFO: previous boot confirmed by application\r\n");
    } else {
        uart_log_puts(
            "INFO: no boot confirmation from previous app (cold or crash)\r\n"
        );
    }
    /* Always invalidate the shared block now we've consumed it. */
    SHARED_BOOT_BLOCK.boot_confirmed = 0U;
    SHARED_BOOT_BLOCK.magic          = SHARED_BLOCK_MAGIC;
}

/* ===================================================================== */
/* OTA-pending swap                                                        */
/* ===================================================================== */
/** Swap+re-verify+revert; bumps anti-downgrade state on success. */
static void process_ota_pending_swap(BootConfig_t *cfg, bool *cfg_dirty)
{
    if (SHARED_BOOT_BLOCK.ota_pending == 0U) {
        return;
    }

    /* Always clear the pending bit up-front, regardless of outcome - we
     * never want to retry an OTA swap automatically on the next boot. */
    SHARED_BOOT_BLOCK.ota_pending = 0U;

    const uint8_t inactive = memory_map_other_slot(cfg->active_slot);
    const uint32_t inactive_addr = memory_map_slot_addr(inactive);

    uart_log_printf("INFO: OTA pending; verifying inactive slot %c at 0x%08x\r\n",
                    (char)inactive, (unsigned)inactive_addr);
    if (!crypto_verify_firmware(inactive_addr)) {
        uart_log_puts("WARN: inactive slot failed verify; staying on active\r\n");
        return;
    }

    FirmwareHeader_t hdr;
    if (flash_read(inactive_addr, (uint8_t *)&hdr, sizeof(hdr)) != FLASH_OK
        || hdr.magic != FIRMWARE_MAGIC) {
        uart_log_puts("WARN: failed to read inactive slot header; staying on active\r\n");
        return;
    }

    if (!boot_config_firmware_allowed(cfg, hdr.version)) {
        uart_log_printf(
            "WARN: reject inactive slot version 0x%08x (floor 0x%08x)\r\n",
            (unsigned)hdr.version,
            (unsigned)boot_config_antidowngrade_floor(cfg));
        return;
    }

    const uint8_t old_active = cfg->active_slot;
    boot_config_record_ota_to_slot(cfg, inactive, hdr.version);
    cfg->active_slot   = inactive;
    cfg->boot_attempts = 0U;
    *cfg_dirty = true;
    save_or_warn(cfg, "ota-swap-commit");

    /* Post-swap re-verify: defense against a transient read failure in the
     * window between the verify above and our about-to-jump path. */
    if (crypto_verify_firmware(memory_map_slot_addr(cfg->active_slot))) {
        uart_log_printf("INFO: OTA swap committed; new active slot=%c\r\n",
                        (char)cfg->active_slot);
        return;
    }

    uart_log_puts("WARN: post-swap re-verify failed; reverting\r\n");
    cfg->active_slot = old_active;
    save_or_warn(cfg, "ota-swap-revert");
}

/* ===================================================================== */
/* Verify-active loop                                                       */
/* ===================================================================== */
/**
 * Returns the slot to jump into, or 0 if recovery mode should be entered.
 * Updates @p cfg in place; the caller persists once before jumping.
 */
static uint8_t pick_slot_to_boot(BootConfig_t *cfg, bool *cfg_dirty)
{
    for (unsigned iter = 0; iter < MAX_SWAP_RETRIES; ++iter) {
        const uint32_t active_addr = memory_map_slot_addr(cfg->active_slot);
        uart_log_printf("INFO: verifying active slot %c at 0x%08x\r\n",
                        (char)cfg->active_slot, (unsigned)active_addr);

        if (crypto_verify_firmware(active_addr)) {
            if (cfg->boot_attempts >= MAX_BOOT_ATTEMPTS) {
                uart_log_printf(
                    "WARN: %u consecutive boot attempts on slot %c; "
                    "switching slot\r\n",
                    (unsigned)cfg->boot_attempts,
                    (char)cfg->active_slot);
                cfg->active_slot   = memory_map_other_slot(cfg->active_slot);
                cfg->boot_attempts = 0U;
                *cfg_dirty = true;
                continue;  /* re-verify the new active slot */
            }
            cfg->boot_attempts++;
            *cfg_dirty = true;
            return cfg->active_slot;
        }

        /* Active failed verify: try the alternate. */
        const uint8_t  other_slot = memory_map_other_slot(cfg->active_slot);
        const uint32_t other_addr = memory_map_slot_addr(other_slot);
        uart_log_printf(
            "WARN: active slot %c invalid; trying %c at 0x%08x\r\n",
            (char)cfg->active_slot, (char)other_slot, (unsigned)other_addr);

        if (crypto_verify_firmware(other_addr)) {
            cfg->active_slot   = other_slot;
            cfg->boot_attempts = 0U;
            *cfg_dirty = true;
            return cfg->active_slot;
        }

        uart_log_puts("ERR : both slots failed verification\r\n");
        return 0U;
    }
    uart_log_puts("ERR : MAX_SWAP_RETRIES exceeded; entering recovery\r\n");
    return 0U;
}

/* ===================================================================== */
/* main                                                                    */
/* ===================================================================== */
int main(void)
{
    iwdg_init();
    uart_init();
    flash_init();

    BootConfig_t cfg;
    bool cfg_loaded_ok = boot_config_load(&cfg);
    bool cfg_dirty     = !cfg_loaded_ok;
    if (!cfg_loaded_ok) {
        uart_log_puts("WARN: boot config invalid; using defaults\r\n");
    }

    log_banner(&cfg);

    check_app_confirmation(&cfg, &cfg_dirty);

    if (cfg_dirty) {
        save_or_warn(&cfg, "post-confirm-check");
        cfg_dirty = false;
    }

    process_ota_pending_swap(&cfg, &cfg_dirty);
    if (cfg_dirty) {
        save_or_warn(&cfg, "post-ota-swap");
        cfg_dirty = false;
    }

    uint8_t boot_slot = pick_slot_to_boot(&cfg, &cfg_dirty);
    if (cfg_dirty) {
        save_or_warn(&cfg, "post-pick");
    }

    if (boot_slot == 0U) {
        recovery_mode_run();  /* never returns */
    }

    uint32_t slot_addr = memory_map_slot_addr(boot_slot);
    uart_log_printf("INFO: jumping to slot %c (0x%08x + 0x%x)\r\n",
                    (char)boot_slot,
                    (unsigned)slot_addr,
                    (unsigned)FIRMWARE_HEADER_SIZE);
    bootloader_jump_to_application(slot_addr);
    /* unreachable */
    return 0;
}

/* ===================================================================== */
/* jump_to_application                                                     */
/* ===================================================================== */
__attribute__((noreturn))
void bootloader_jump_to_application(uint32_t slot_addr)
{
    const uint32_t app_vt = slot_addr + FIRMWARE_HEADER_SIZE;
    /* Sanity: the linker math guarantees this is true at compile time, but
     * we re-check at runtime in case slot_addr came from a corrupt config. */
    if ((app_vt & 0x1FFU) != 0U) {
        uart_log_puts("FATAL: app vector table misaligned; halting\r\n");
        for (;;) iwdg_kick();
    }

    const uint32_t app_msp = *(volatile uint32_t *)(app_vt + 0U);
    const uint32_t app_rh  = *(volatile uint32_t *)(app_vt + 4U);

    /* Disable IRQs before teardown */
    __asm volatile ("cpsid i" ::: "memory");

    /* Point VTOR at the application's vector table */
    SCB_VTOR = app_vt;
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");

    /* Set Main Stack Pointer to the value the app expects. */
    __asm volatile ("msr msp, %0" :: "r" (app_msp) : "memory");

    /* Re-enable IRQs and tail-call the app's reset handler. The reset
     * handler is the second word in the vector table, which on Cortex-M
     * is always thumb (LSB set); we don't need to mask the bit because
     * the address itself encodes the ARM/Thumb state. */
    __asm volatile ("cpsie i" ::: "memory");

    void (*app_reset_handler)(void) = (void (*)(void))app_rh;
    app_reset_handler();

    /* Should never return - but spin if it somehow does. */
    for (;;) { iwdg_kick(); }
}

/* ===================================================================== */
/* SystemInit - lightweight, safe on QEMU netduino2 and real STM32F4       */
/* ===================================================================== */
void SystemInit(void)
{
    /* Set VTOR to bootloader base in case some reset path left it
     * elsewhere. */
    SCB_VTOR = BOOTLOADER_ADDR;
}

/* ===================================================================== */
/* Software-reset helper used by ota_client and recovery                   */
/* ===================================================================== */
__attribute__((noreturn))
void bootloader_system_reset(void)
{
    __asm volatile ("dsb 0xF" ::: "memory");
    SCB_AIRCR = AIRCR_KEY | AIRCR_SYSRESETREQ;
    __asm volatile ("dsb 0xF" ::: "memory");
    for (;;) { /* wait for reset */ }
}
