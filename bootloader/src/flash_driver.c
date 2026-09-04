/**
 * @file    flash_driver.c
 * @brief   STM32F4 flash driver, with a QEMU_FLASH_SIM compile-time backend.
 */

#include "flash_driver.h"
#include "iwdg.h"
#include "memory_map.h"

#include <string.h>

/* ===== STM32F4 flash sector table ===================================== */

/* Sector start addresses for STM32F407 (1 MB part, single bank). */
static const uint32_t k_sector_start[FLASH_NUM_SECTORS] = {
    0x08000000UL, /* sector  0: 16 KB */
    0x08004000UL, /* sector  1: 16 KB */
    0x08008000UL, /* sector  2: 16 KB */
    0x0800C000UL, /* sector  3: 16 KB */
    0x08010000UL, /* sector  4: 64 KB */
    0x08020000UL, /* sector  5: 128 KB */
    0x08040000UL, /* sector  6: 128 KB */
    0x08060000UL, /* sector  7: 128 KB */
    0x08080000UL, /* sector  8: 128 KB */
    0x080A0000UL, /* sector  9: 128 KB */
    0x080C0000UL, /* sector 10: 128 KB */
    0x080E0000UL, /* sector 11: 128 KB */
};

static const uint32_t k_sector_size[FLASH_NUM_SECTORS] = {
    16U * 1024U,  16U * 1024U,  16U * 1024U,  16U * 1024U,
    64U * 1024U,
    128U * 1024U, 128U * 1024U, 128U * 1024U, 128U * 1024U,
    128U * 1024U, 128U * 1024U, 128U * 1024U,
};

static int sector_index_for_addr(uint32_t addr)
{
    for (int i = FLASH_NUM_SECTORS - 1; i >= 0; --i) {
        if (addr >= k_sector_start[i]) {
            return i;
        }
    }
    return -1;
}

uint32_t flash_sector_start(uint32_t addr)
{
    int idx = sector_index_for_addr(addr);
    return (idx < 0) ? 0U : k_sector_start[idx];
}

uint32_t flash_sector_size(uint32_t addr)
{
    int idx = sector_index_for_addr(addr);
    return (idx < 0) ? 0U : k_sector_size[idx];
}

/* ===================================================================== */
/* QEMU simulation backend                                                 */
/* ===================================================================== */
#if defined(QEMU_FLASH_SIM) && (QEMU_FLASH_SIM != 0)

/*
 * QEMU netduino2's flash region (0x08000000+) is initialised as a ROM
 * memory region (memory_region_init_rom): the CPU can read from it but
 * writes are silently dropped at the bus. To exercise the bootloader's
 * write paths (config save, OTA, rollback) on QEMU, we maintain a
 * SRAM-backed shadow of the writable flash regions.
 *
 * Layout: a struct in SRAM whose fields mirror the on-flash regions we
 * care about. The struct lives in a NOLOAD linker section ("`.flash_shadow`")
 * placed just below the SharedBootBlock so its contents survive a CPU
 * soft reset (NVIC/AIRCR) - this is what makes post-OTA re-verify work
 * across a `bootloader_system_reset()`. A magic cookie distinguishes a
 * cold boot (random/zero RAM, init from ROM) from a soft reset
 * (shadow already populated, leave alone).
 *
 * The shadow does NOT cover the entire 460 KB slot - that wouldn't fit
 * in 128 KB of SRAM. It covers the first SHADOW_SLOT_SIZE bytes of each
 * slot, which is more than enough for the demo image (~2 KB) and any
 * reasonable test image. Real hardware is unaffected: this whole
 * compilation unit branch is excluded from the HW build.
 */
#define QEMU_SHADOW_MAGIC      0xCAFEFEEDUL
#define SHADOW_CONFIG_SIZE     ((uint32_t)BOOT_CONFIG_SIZE)        /* 4 KB */
#define SHADOW_SLOT_SIZE       (32U * 1024U)                       /* 32 KB */

typedef struct {
    uint32_t magic;
    uint8_t  config[SHADOW_CONFIG_SIZE];
    uint8_t  slot_a[SHADOW_SLOT_SIZE];
    uint8_t  slot_b[SHADOW_SLOT_SIZE];
} qemu_flash_shadow_t;

/* Linker-placed at 0x2000E000 in a NOLOAD section; see
 * bootloader/linker_bootloader.ld. The `used` attribute keeps GC-sections
 * from dropping it. */
__attribute__((section(".flash_shadow"), used))
static qemu_flash_shadow_t g_qemu_flash_shadow;
#define SHADOW (g_qemu_flash_shadow)

static uint8_t *shadow_byte_ptr(uint32_t addr)
{
    if (addr >= BOOT_CONFIG_ADDR &&
        addr <  BOOT_CONFIG_ADDR + SHADOW_CONFIG_SIZE) {
        return &SHADOW.config[addr - BOOT_CONFIG_ADDR];
    }
    if (addr >= SLOT_A_ADDR &&
        addr <  SLOT_A_ADDR + SHADOW_SLOT_SIZE) {
        return &SHADOW.slot_a[addr - SLOT_A_ADDR];
    }
    if (addr >= SLOT_B_ADDR &&
        addr <  SLOT_B_ADDR + SHADOW_SLOT_SIZE) {
        return &SHADOW.slot_b[addr - SLOT_B_ADDR];
    }
    return NULL;
}

/* Read up to `size` bytes from a flash address, transparently routing
 * through the shadow for shadowed regions. Bytes that fall outside any
 * shadowed window are read directly from the (read-only) ROM. */
static void shadow_aware_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t *sp = shadow_byte_ptr(addr + i);
        buf[i] = sp ? *sp : *((const volatile uint8_t *)(addr + i));
    }
}

flash_status_t flash_init(void)
{
    /* Lazy initialise on cold boot: the NOLOAD section is left
     * uninitialised by startup, so on the first boot of a fresh QEMU
     * process the magic field will (almost certainly) not match. We
     * then seed the shadow from the live ROM. After a soft reset
     * (NVIC AIRCR), the magic IS still set and we keep whatever the
     * pre-reset bootloader/OTA wrote. */
    if (SHADOW.magic != QEMU_SHADOW_MAGIC) {
        memcpy(SHADOW.config,
               (const void *)BOOT_CONFIG_ADDR, SHADOW_CONFIG_SIZE);
        memcpy(SHADOW.slot_a,
               (const void *)SLOT_A_ADDR,      SHADOW_SLOT_SIZE);
        memcpy(SHADOW.slot_b,
               (const void *)SLOT_B_ADDR,      SHADOW_SLOT_SIZE);
        SHADOW.magic = QEMU_SHADOW_MAGIC;
    }
    return FLASH_OK;
}

flash_status_t flash_unlock(void)      { return FLASH_OK; }
flash_status_t flash_lock(void)        { return FLASH_OK; }

uint32_t flash_ota_slot_span(void)
{
    return SHADOW_SLOT_SIZE;
}

flash_status_t flash_erase_sector(uint32_t sector_start_addr)
{
    int idx = sector_index_for_addr(sector_start_addr);
    if (idx < 0 || k_sector_start[idx] != sector_start_addr) {
        return FLASH_ERR_RANGE;
    }
    uint32_t sz = k_sector_size[idx];
    /* Kick the watchdog every 4 KB during the (simulated) erase. */
    for (uint32_t off = 0; off < sz; off += 4096U) {
        uint32_t chunk = (sz - off) < 4096U ? (sz - off) : 4096U;
        for (uint32_t i = 0; i < chunk; ++i) {
            uint8_t *sp = shadow_byte_ptr(sector_start_addr + off + i);
            if (sp) *sp = 0xFFU;
            /* Bytes outside the shadow are in true ROM and unreachable
             * for erase on QEMU; we silently skip them. They aren't
             * part of any writable region so this is fine. */
        }
        iwdg_kick();
    }
    return FLASH_OK;
}

flash_status_t flash_erase_range(uint32_t addr, uint32_t size)
{
    if (size == 0U) return FLASH_OK;
    uint32_t start = flash_sector_start(addr);
    uint32_t end   = addr + size;
    while (start < end) {
        flash_status_t s = flash_erase_sector(start);
        if (s != FLASH_OK) return s;
        start += flash_sector_size(start);
    }
    return FLASH_OK;
}

flash_status_t flash_program_word(uint32_t addr, uint32_t value)
{
    if ((addr & 0x3U) != 0U) return FLASH_ERR_RANGE;
    /* Word straddles 4 contiguous shadow bytes; if any one of them is
     * outside the shadow, the address isn't in a writable region and we
     * fail rather than silently dropping the write. */
    for (uint32_t i = 0; i < 4U; ++i) {
        if (shadow_byte_ptr(addr + i) == NULL) return FLASH_ERR_RANGE;
    }
    uint8_t bytes[4] = {
        (uint8_t)(value      ),
        (uint8_t)(value >>  8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    for (uint32_t i = 0; i < 4U; ++i) {
        *shadow_byte_ptr(addr + i) = bytes[i];
        if (*shadow_byte_ptr(addr + i) != bytes[i]) return FLASH_ERR_VERIFY;
    }
    return FLASH_OK;
}

flash_status_t flash_program_bytes(uint32_t addr,
                                   const uint8_t *data,
                                   uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t *sp = shadow_byte_ptr(addr + i);
        if (sp == NULL) return FLASH_ERR_RANGE;
        *sp = data[i];
        if (*sp != data[i]) return FLASH_ERR_VERIFY;
        if ((i & 0xFFU) == 0xFFU) iwdg_kick();
    }
    return FLASH_OK;
}

flash_status_t flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    shadow_aware_read(addr, buf, size);
    return FLASH_OK;
}

void *flash_get_ptr(uint32_t addr)
{
    uint8_t *sp = shadow_byte_ptr(addr);
    return sp ? (void *)sp : (void *)(uintptr_t)addr;
}

#else /* ===== Real STM32F4 hardware backend ============================ */

/* STM32F4 flash controller registers (RM0090) */
#define FLASH_R_BASE        (0x40023C00UL)
#define FLASH_KEYR          (*(volatile uint32_t *)(FLASH_R_BASE + 0x04U))
#define FLASH_SR            (*(volatile uint32_t *)(FLASH_R_BASE + 0x0CU))
#define FLASH_CR            (*(volatile uint32_t *)(FLASH_R_BASE + 0x10U))

#define FLASH_KEY1          (0x45670123UL)
#define FLASH_KEY2          (0xCDEF89ABUL)

#define FLASH_SR_BSY        (1UL << 16)
#define FLASH_SR_PGSERR     (1UL <<  7)
#define FLASH_SR_PGPERR     (1UL <<  6)
#define FLASH_SR_PGAERR     (1UL <<  5)
#define FLASH_SR_WRPERR     (1UL <<  4)
#define FLASH_SR_OPERR      (1UL <<  1)
#define FLASH_SR_EOP        (1UL <<  0)
#define FLASH_SR_ERR_MASK   (FLASH_SR_PGSERR | FLASH_SR_PGPERR | \
                             FLASH_SR_PGAERR | FLASH_SR_WRPERR | \
                             FLASH_SR_OPERR)

#define FLASH_CR_PG         (1UL <<  0)
#define FLASH_CR_SER        (1UL <<  1)
#define FLASH_CR_STRT       (1UL << 16)
#define FLASH_CR_LOCK       (1UL << 31)
#define FLASH_CR_PSIZE_X32  (2UL <<  8)
#define FLASH_CR_SNB_POS    (3U)
#define FLASH_CR_SNB_MSK    (0xFUL << FLASH_CR_SNB_POS)

static flash_status_t wait_busy(void)
{
    /* Bounded busy-wait. ~50 ms worst case for sector 11 erase at typical
     * conditions; the iwdg_kick inside keeps us alive longer. */
    for (uint32_t i = 0; i < 5000000U; ++i) {
        if ((FLASH_SR & FLASH_SR_BSY) == 0U) {
            if (FLASH_SR & FLASH_SR_ERR_MASK) {
                FLASH_SR |= FLASH_SR_ERR_MASK;
                return FLASH_ERR_PROG;
            }
            return FLASH_OK;
        }
        if ((i & 0xFFFFU) == 0U) iwdg_kick();
    }
    return FLASH_ERR_BUSY;
}

uint32_t flash_ota_slot_span(void)
{
    return SLOT_SIZE;
}

flash_status_t flash_init(void)
{
    /* Clear any leftover error flags; harmless if none are set. */
    FLASH_SR |= FLASH_SR_ERR_MASK | FLASH_SR_EOP;
    return FLASH_OK;
}

flash_status_t flash_unlock(void)
{
    if ((FLASH_CR & FLASH_CR_LOCK) == 0U) return FLASH_OK;
    FLASH_KEYR = FLASH_KEY1;
    FLASH_KEYR = FLASH_KEY2;
    if (FLASH_CR & FLASH_CR_LOCK) return FLASH_ERR_LOCKED;
    return FLASH_OK;
}

flash_status_t flash_lock(void)
{
    FLASH_CR |= FLASH_CR_LOCK;
    return FLASH_OK;
}

flash_status_t flash_erase_sector(uint32_t sector_start_addr)
{
    int idx = sector_index_for_addr(sector_start_addr);
    if (idx < 0 || k_sector_start[idx] != sector_start_addr) {
        return FLASH_ERR_RANGE;
    }
    flash_status_t s = wait_busy();
    if (s != FLASH_OK) return s;

    s = flash_unlock();
    if (s != FLASH_OK) return s;

    uint32_t cr = FLASH_CR;
    cr &= ~(FLASH_CR_SNB_MSK | FLASH_CR_PG);
    cr |= FLASH_CR_SER | ((uint32_t)idx << FLASH_CR_SNB_POS) |
          FLASH_CR_PSIZE_X32;
    FLASH_CR = cr;
    FLASH_CR = cr | FLASH_CR_STRT;

    s = wait_busy();

    FLASH_CR &= ~(FLASH_CR_SER | FLASH_CR_SNB_MSK);

    /* Verify all bytes are 0xFF. */
    if (s == FLASH_OK) {
        const uint8_t *p = (const uint8_t *)sector_start_addr;
        uint32_t sz = k_sector_size[idx];
        for (uint32_t i = 0; i < sz; ++i) {
            if (p[i] != 0xFFU) { s = FLASH_ERR_VERIFY; break; }
            if ((i & 0xFFFU) == 0U) iwdg_kick();
        }
    }
    return s;
}

flash_status_t flash_erase_range(uint32_t addr, uint32_t size)
{
    if (size == 0U) return FLASH_OK;
    uint32_t start = flash_sector_start(addr);
    uint32_t end   = addr + size;
    while (start < end) {
        flash_status_t s = flash_erase_sector(start);
        if (s != FLASH_OK) return s;
        start += flash_sector_size(start);
        iwdg_kick();
    }
    return FLASH_OK;
}

flash_status_t flash_program_word(uint32_t addr, uint32_t value)
{
    if ((addr & 0x3U) != 0U) return FLASH_ERR_RANGE;
    flash_status_t s = wait_busy();
    if (s != FLASH_OK) return s;
    s = flash_unlock();
    if (s != FLASH_OK) return s;
    FLASH_CR = (FLASH_CR & ~FLASH_CR_SER) | FLASH_CR_PG | FLASH_CR_PSIZE_X32;
    *(volatile uint32_t *)addr = value;
    s = wait_busy();
    FLASH_CR &= ~FLASH_CR_PG;
    if (s == FLASH_OK && *(volatile uint32_t *)addr != value) {
        s = FLASH_ERR_VERIFY;
    }
    return s;
}

flash_status_t flash_program_bytes(uint32_t addr,
                                   const uint8_t *data,
                                   uint32_t size)
{
    /* Word-aligned programming for the bulk; byte-by-byte for the trailing
     * 1..3 bytes. STM32F4 supports byte/halfword/word programming via
     * PSIZE; we use word for speed and switch to byte for the tail. */
    uint32_t i = 0;
    flash_status_t s;

    s = flash_unlock();
    if (s != FLASH_OK) {
        return s;
    }

    /* Word phase. */
    for (; i + 4U <= size && (((addr + i) & 0x3U) == 0U); i += 4U) {
        uint32_t w =
            ((uint32_t)data[i + 0]      ) |
            ((uint32_t)data[i + 1] <<  8) |
            ((uint32_t)data[i + 2] << 16) |
            ((uint32_t)data[i + 3] << 24);
        s = flash_program_word(addr + i, w);
        if (s != FLASH_OK) {
            flash_lock();
            return s;
        }
        if ((i & 0xFFU) == 0U) iwdg_kick();
    }

    /* Byte phase for the unaligned tail. */
    for (; i < size; ++i) {
        s = wait_busy();
        if (s != FLASH_OK) {
            flash_lock();
            return s;
        }
        FLASH_CR = (FLASH_CR & ~FLASH_CR_SER) | FLASH_CR_PG;
        *(volatile uint8_t *)(addr + i) = data[i];
        s = wait_busy();
        FLASH_CR &= ~FLASH_CR_PG;
        if (s != FLASH_OK) {
            flash_lock();
            return s;
        }
        if (*(volatile uint8_t *)(addr + i) != data[i]) {
            flash_lock();
            return FLASH_ERR_VERIFY;
        }
    }
    flash_lock();
    return FLASH_OK;
}

flash_status_t flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    memcpy(buf, (const void *)addr, size);
    return FLASH_OK;
}

void *flash_get_ptr(uint32_t addr)
{
    /* On real STM32F4 silicon the flash is memory-mapped; nothing to do. */
    return (void *)(uintptr_t)addr;
}

#endif /* QEMU_FLASH_SIM */
