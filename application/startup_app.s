/*
 * startup_app.s - Application vector table and Reset_Handler_App.
 *
 * The application is loaded into Slot A or Slot B at run time. Its vector
 * table sits at slot+0x200 (after the 512-byte FirmwareHeader_t), which is
 * 512-byte aligned per the linker script. The bootloader sets SCB->VTOR
 * to that address and then branches into Reset_Handler_App via the second
 * vector slot.
 *
 * Reset_Handler_App MUST NOT zero the SharedBootBlock at 0x2001FFF0; the
 * NOLOAD section in linker_app.ld guarantees this by keeping the block
 * outside the .bss range.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .global  g_pfnVectors_App
    .global  Reset_Handler_App
    .global  Default_Handler_App

    .word    _sidata
    .word    _sdata
    .word    _edata
    .word    _sbss
    .word    _ebss

    .section  .text.Reset_Handler_App
    .weak     Reset_Handler_App
    .type     Reset_Handler_App, %function
Reset_Handler_App:
    ldr   sp, =_estack

    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     copy_data_check_app
copy_data_loop_app:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4
copy_data_check_app:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   copy_data_loop_app

    ldr   r2, =_sbss
    ldr   r4, =_ebss
    movs  r3, #0
    b     zero_bss_check_app
zero_bss_loop_app:
    str   r3, [r2]
    adds  r2, r2, #4
zero_bss_check_app:
    cmp   r2, r4
    bcc   zero_bss_loop_app

#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1U)
    ldr   r0, =0xE000ED88
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb
#endif

    bl    SystemInit_App
    bl    app_main
1:  b     1b
    .size Reset_Handler_App, .-Reset_Handler_App

    .section .text.Default_Handler_App,"ax",%progbits
Default_Handler_App:
    b     Default_Handler_App
    .size Default_Handler_App, .-Default_Handler_App

/*
 * Compact vector table. We reserve room for all STM32F4 IRQs but only
 * declare the handlers the demo app actually uses; everything else falls
 * through to Default_Handler_App.
 */
    .section  .isr_vector,"a",%progbits
    .type     g_pfnVectors_App, %object
g_pfnVectors_App:
    .word     _estack
    .word     Reset_Handler_App
    .word     NMI_Handler_App
    .word     HardFault_Handler_App
    .word     MemManage_Handler_App
    .word     BusFault_Handler_App
    .word     UsageFault_Handler_App
    .word     0
    .word     0
    .word     0
    .word     0
    .word     SVC_Handler_App
    .word     DebugMon_Handler_App
    .word     0
    .word     PendSV_Handler_App
    .word     SysTick_Handler_App

    /* Pad the rest of the IRQ table to 82 entries with Default_Handler_App. */
    .rept     82
    .word     Default_Handler_App
    .endr

    .size  g_pfnVectors_App, .-g_pfnVectors_App

    .macro DEFAULT_APP name
        .weak \name
        .thumb_set \name, Default_Handler_App
    .endm

    DEFAULT_APP NMI_Handler_App
    DEFAULT_APP HardFault_Handler_App
    DEFAULT_APP MemManage_Handler_App
    DEFAULT_APP BusFault_Handler_App
    DEFAULT_APP UsageFault_Handler_App
    DEFAULT_APP SVC_Handler_App
    DEFAULT_APP DebugMon_Handler_App
    DEFAULT_APP PendSV_Handler_App
    DEFAULT_APP SysTick_Handler_App

    .end
