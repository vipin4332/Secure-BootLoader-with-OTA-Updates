/*
 * linker_app.ld
 *
 * Application linker script. The application image is preceded in flash by
 * a 512-byte FirmwareHeader_t, so the entry point (vector table) sits at
 *   SLOT_A_ADDR + 0x200 = 0x08011200
 * which is 512-byte aligned and therefore valid for SCB->VTOR on Cortex-M4.
 *
 * The application runs from a single slot (we link it as if it lives in
 * Slot A; the bootloader rewrites SCB->VTOR before jumping, so the same
 * binary works in either slot as long as the slots are size-equal and PIC
 * isn't required - which is fine here because we only use absolute branches
 * to addresses inside the same slot, and the linker emits position-
 * dependent code that the bootloader's VTOR fix-up handles via the vector
 * table indirection).
 */

ENTRY(Reset_Handler_App)

MEMORY
{
    /* App lives in Slot A starting after the 512-B header. */
    APP  (rx)  : ORIGIN = 0x08011200, LENGTH = 460K - 512
    /* 128 KB contiguous SRAM minus the top 16 B for SharedBootBlock. */
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K - 16
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

_Min_Stack_Size = 0x2000;
_Min_Heap_Size  = 0x0400;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(512);   /* enforce 512-B alignment of vector table */
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > APP

    .text :
    {
        . = ALIGN(4);
        *(.text) *(.text*)
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
        KEEP (*(.init))
        KEEP (*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > APP

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata) *(.rodata*)
        . = ALIGN(4);
    } > APP

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > APP
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > APP

    .preinit_array :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } > APP

    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } > APP

    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT(.fini_array.*)))
        KEEP (*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } > APP

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data) *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > APP

    . = ALIGN(4);
    .bss :
    {
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss) *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    .shared_boot_block 0x2001FFF0 (NOLOAD) :
    {
        KEEP(*(.shared_boot_block))
    }

    /DISCARD/ : { libc.a ( * ) libm.a ( * ) libgcc.a ( * ) }
    .ARM.attributes 0 : { *(.ARM.attributes) }
}
