/*
 * linker_bootloader.ld
 *
 * Bootloader linker script for STM32F407 (and the QEMU netduino2 build,
 * which is Cortex-M3 / STM32F2 but with a compatible flash & SRAM layout).
 *
 * Memory regions match common/memory_map.h:
 *   - BOOT  flash partition: 0x08000000, 64 KB
 *   - SRAM:                  0x20000000, 128 KB contiguous (SRAM1+SRAM2)
 *
 * High-SRAM reservations (NOLOAD - excluded from .bss zeroing so they
 * survive a soft reset):
 *   - SharedBootBlock at 0x2001FFF0 (16 B) - app/bootloader handoff.
 *   - .flash_shadow at 0x2000E000 (~72 KB) - QEMU-only SRAM mirror of
 *     the writable flash regions. The QEMU netduino2 implements flash
 *     as a true ROM region (memory_region_init_rom), so writes are
 *     dropped at the bus. We route flash writes through a SRAM shadow
 *     to make the bootloader's write paths (config, OTA, rollback)
 *     testable on QEMU. The HW build ignores this section.
 *
 * SRAM is shortened so .bss/.data/heap/stack stop below the NOLOAD
 * reservations and never collide with them.
 */

ENTRY(Reset_Handler)

MEMORY
{
    BOOT (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    /* SRAM stops at 0x2000E000 to leave 0x2000E000..0x2001FFEF for the
     * .flash_shadow NOLOAD region and 0x2001FFF0..0x2001FFFF for
     * .shared_boot_block. */
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 56K
}

/* End-of-RAM stack pointer (top of usable SRAM, below the shadow region). */
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

/* Reserve 8 KB minimum stack and 1 KB minimum heap (heap not actually used,
 * but kept symbolic for ARM newlib-nano hooks). */
_Min_Stack_Size = 0x2000;
_Min_Heap_Size  = 0x0400;

SECTIONS
{
    /* Vector table at the very start of flash. */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > BOOT

    /* Code + read-only data. */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP (*(.init))
        KEEP (*(.fini))

        . = ALIGN(4);
        _etext = .;
    } > BOOT

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > BOOT

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > BOOT
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > BOOT

    .preinit_array :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } > BOOT

    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } > BOOT

    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT(.fini_array.*)))
        KEEP (*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } > BOOT

    /* Initialised data: stored in flash, copied to SRAM at startup. */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > BOOT

    /* Zero-initialised data. */
    . = ALIGN(4);
    .bss :
    {
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    /* Stack/heap sanity check. */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    /*
     * QEMU-only SRAM shadow of writable flash regions. NOLOAD keeps it
     * out of the binary and out of .bss zeroing, so its contents
     * survive a CPU soft reset (NVIC AIRCR). On real STM32F4 hardware
     * this section is unreferenced and the linker drops it.
     */
    .flash_shadow 0x2000E000 (NOLOAD) :
    {
        KEEP(*(.flash_shadow))
    }

    /*
     * SharedBootBlock at the top 16 B of SRAM. NOLOAD keeps it out of the
     * binary and out of the .bss zeroing range. This is the persistent-
     * across-soft-reset region used to hand off boot status to/from the app.
     */
    .shared_boot_block 0x2001FFF0 (NOLOAD) :
    {
        KEEP(*(.shared_boot_block))
    }

    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
