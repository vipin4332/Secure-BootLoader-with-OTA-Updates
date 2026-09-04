/*
 * startup.s - Bootloader vector table and Reset_Handler for STM32F407.
 *
 * The vector table lives at the beginning of the bootloader partition
 * (0x08000000) and contains all standard Cortex-M system vectors plus
 * the STM32F4 IRQ list (we keep room for 82 IRQs even on the QEMU M3 build,
 * because the binary may be run on real STM32F4 silicon as well; unused
 * entries point at Default_Handler, which spins).
 *
 * Reset_Handler:
 *   1. Initialise stack pointer (the CPU does this from vector 0 already).
 *   2. Copy initialised .data from flash to SRAM.
 *   3. Zero out .bss.
 *   4. (HW build only) Enable the FPU (CPACR).
 *   5. Call SystemInit() - clock setup and any pre-main init.
 *   6. Branch to main(). Never return.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .global  g_pfnVectors
    .global  Reset_Handler
    .global  Default_Handler

    /* Symbols from the linker script. */
    .word    _sidata
    .word    _sdata
    .word    _edata
    .word    _sbss
    .word    _ebss

/* ===================================================================== */
/* Reset_Handler                                                           */
/* ===================================================================== */
    .section  .text.Reset_Handler
    .weak     Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    ldr   sp, =_estack          /* set stack (CPU already did this, but be explicit) */

    /* --- copy .data from flash to SRAM --- */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     copy_data_check
copy_data_loop:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4
copy_data_check:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   copy_data_loop

    /* --- zero .bss --- */
    ldr   r2, =_sbss
    ldr   r4, =_ebss
    movs  r3, #0
    b     zero_bss_check
zero_bss_loop:
    str   r3, [r2]
    adds  r2, r2, #4
zero_bss_check:
    cmp   r2, r4
    bcc   zero_bss_loop

#if defined(__FPU_PRESENT) && (__FPU_PRESENT == 1U)
    /* Enable the FPU (CP10 + CP11 full access) on hard-float builds. */
    ldr   r0, =0xE000ED88        /* CPACR */
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb
#endif

    bl    SystemInit
    bl    main
1:  b     1b                       /* main should not return */
    .size Reset_Handler, .-Reset_Handler

/* ===================================================================== */
/* Default_Handler                                                         */
/* ===================================================================== */
    .section .text.Default_Handler,"ax",%progbits
Default_Handler:
    b     Default_Handler
    .size Default_Handler, .-Default_Handler

/* ===================================================================== */
/* Vector table                                                            */
/* ===================================================================== */
    .section  .isr_vector,"a",%progbits
    .type     g_pfnVectors, %object
g_pfnVectors:
    .word     _estack                  /* Initial stack pointer */
    .word     Reset_Handler            /* Reset */
    .word     NMI_Handler              /* NMI */
    .word     HardFault_Handler        /* HardFault */
    .word     MemManage_Handler        /* MPU fault */
    .word     BusFault_Handler         /* Bus fault */
    .word     UsageFault_Handler       /* Usage fault */
    .word     0
    .word     0
    .word     0
    .word     0
    .word     SVC_Handler              /* SVCall */
    .word     DebugMon_Handler         /* DebugMon */
    .word     0
    .word     PendSV_Handler           /* PendSV */
    .word     SysTick_Handler          /* SysTick */

    /* External Interrupts (STM32F4 / netduino2 share the first 60+ IRQs). */
    .word     WWDG_IRQHandler          /* 0  Window Watchdog */
    .word     PVD_IRQHandler           /* 1  PVD via EXTI Line */
    .word     TAMP_STAMP_IRQHandler    /* 2  Tamper and TimeStamps */
    .word     RTC_WKUP_IRQHandler      /* 3  RTC Wakeup */
    .word     FLASH_IRQHandler         /* 4  FLASH */
    .word     RCC_IRQHandler           /* 5  RCC */
    .word     EXTI0_IRQHandler         /* 6  EXTI Line 0 */
    .word     EXTI1_IRQHandler         /* 7  EXTI Line 1 */
    .word     EXTI2_IRQHandler         /* 8  EXTI Line 2 */
    .word     EXTI3_IRQHandler         /* 9  EXTI Line 3 */
    .word     EXTI4_IRQHandler         /* 10 EXTI Line 4 */
    .word     DMA1_Stream0_IRQHandler  /* 11 */
    .word     DMA1_Stream1_IRQHandler  /* 12 */
    .word     DMA1_Stream2_IRQHandler  /* 13 */
    .word     DMA1_Stream3_IRQHandler  /* 14 */
    .word     DMA1_Stream4_IRQHandler  /* 15 */
    .word     DMA1_Stream5_IRQHandler  /* 16 */
    .word     DMA1_Stream6_IRQHandler  /* 17 */
    .word     ADC_IRQHandler           /* 18 */
    .word     CAN1_TX_IRQHandler       /* 19 */
    .word     CAN1_RX0_IRQHandler      /* 20 */
    .word     CAN1_RX1_IRQHandler      /* 21 */
    .word     CAN1_SCE_IRQHandler      /* 22 */
    .word     EXTI9_5_IRQHandler       /* 23 */
    .word     TIM1_BRK_TIM9_IRQHandler /* 24 */
    .word     TIM1_UP_TIM10_IRQHandler /* 25 */
    .word     TIM1_TRG_COM_TIM11_IRQHandler /* 26 */
    .word     TIM1_CC_IRQHandler       /* 27 */
    .word     TIM2_IRQHandler          /* 28 */
    .word     TIM3_IRQHandler          /* 29 */
    .word     TIM4_IRQHandler          /* 30 */
    .word     I2C1_EV_IRQHandler       /* 31 */
    .word     I2C1_ER_IRQHandler       /* 32 */
    .word     I2C2_EV_IRQHandler       /* 33 */
    .word     I2C2_ER_IRQHandler       /* 34 */
    .word     SPI1_IRQHandler          /* 35 */
    .word     SPI2_IRQHandler          /* 36 */
    .word     USART1_IRQHandler        /* 37 */
    .word     USART2_IRQHandler        /* 38 */
    .word     USART3_IRQHandler        /* 39 */
    .word     EXTI15_10_IRQHandler     /* 40 */
    .word     RTC_Alarm_IRQHandler     /* 41 */
    .word     OTG_FS_WKUP_IRQHandler   /* 42 */
    .word     TIM8_BRK_TIM12_IRQHandler/* 43 */
    .word     TIM8_UP_TIM13_IRQHandler /* 44 */
    .word     TIM8_TRG_COM_TIM14_IRQHandler /* 45 */
    .word     TIM8_CC_IRQHandler       /* 46 */
    .word     DMA1_Stream7_IRQHandler  /* 47 */
    .word     FSMC_IRQHandler          /* 48 */
    .word     SDIO_IRQHandler          /* 49 */
    .word     TIM5_IRQHandler          /* 50 */
    .word     SPI3_IRQHandler          /* 51 */
    .word     UART4_IRQHandler         /* 52 */
    .word     UART5_IRQHandler         /* 53 */
    .word     TIM6_DAC_IRQHandler      /* 54 */
    .word     TIM7_IRQHandler          /* 55 */
    .word     DMA2_Stream0_IRQHandler  /* 56 */
    .word     DMA2_Stream1_IRQHandler  /* 57 */
    .word     DMA2_Stream2_IRQHandler  /* 58 */
    .word     DMA2_Stream3_IRQHandler  /* 59 */
    .word     DMA2_Stream4_IRQHandler  /* 60 */
    .word     ETH_IRQHandler           /* 61 */
    .word     ETH_WKUP_IRQHandler      /* 62 */
    .word     CAN2_TX_IRQHandler       /* 63 */
    .word     CAN2_RX0_IRQHandler      /* 64 */
    .word     CAN2_RX1_IRQHandler      /* 65 */
    .word     CAN2_SCE_IRQHandler      /* 66 */
    .word     OTG_FS_IRQHandler        /* 67 */
    .word     DMA2_Stream5_IRQHandler  /* 68 */
    .word     DMA2_Stream6_IRQHandler  /* 69 */
    .word     DMA2_Stream7_IRQHandler  /* 70 */
    .word     USART6_IRQHandler        /* 71 */
    .word     I2C3_EV_IRQHandler       /* 72 */
    .word     I2C3_ER_IRQHandler       /* 73 */
    .word     OTG_HS_EP1_OUT_IRQHandler/* 74 */
    .word     OTG_HS_EP1_IN_IRQHandler /* 75 */
    .word     OTG_HS_WKUP_IRQHandler   /* 76 */
    .word     OTG_HS_IRQHandler        /* 77 */
    .word     DCMI_IRQHandler          /* 78 */
    .word     0                        /* 79 reserved */
    .word     HASH_RNG_IRQHandler      /* 80 */
    .word     FPU_IRQHandler           /* 81 */

    .size  g_pfnVectors, .-g_pfnVectors

/* ===================================================================== */
/* Weak default-handlers - any of these can be overridden in C.            */
/* ===================================================================== */
    .macro DEFAULT_HANDLER name
        .weak \name
        .thumb_set \name, Default_Handler
    .endm

    DEFAULT_HANDLER NMI_Handler
    DEFAULT_HANDLER HardFault_Handler
    DEFAULT_HANDLER MemManage_Handler
    DEFAULT_HANDLER BusFault_Handler
    DEFAULT_HANDLER UsageFault_Handler
    DEFAULT_HANDLER SVC_Handler
    DEFAULT_HANDLER DebugMon_Handler
    DEFAULT_HANDLER PendSV_Handler
    DEFAULT_HANDLER SysTick_Handler

    DEFAULT_HANDLER WWDG_IRQHandler
    DEFAULT_HANDLER PVD_IRQHandler
    DEFAULT_HANDLER TAMP_STAMP_IRQHandler
    DEFAULT_HANDLER RTC_WKUP_IRQHandler
    DEFAULT_HANDLER FLASH_IRQHandler
    DEFAULT_HANDLER RCC_IRQHandler
    DEFAULT_HANDLER EXTI0_IRQHandler
    DEFAULT_HANDLER EXTI1_IRQHandler
    DEFAULT_HANDLER EXTI2_IRQHandler
    DEFAULT_HANDLER EXTI3_IRQHandler
    DEFAULT_HANDLER EXTI4_IRQHandler
    DEFAULT_HANDLER DMA1_Stream0_IRQHandler
    DEFAULT_HANDLER DMA1_Stream1_IRQHandler
    DEFAULT_HANDLER DMA1_Stream2_IRQHandler
    DEFAULT_HANDLER DMA1_Stream3_IRQHandler
    DEFAULT_HANDLER DMA1_Stream4_IRQHandler
    DEFAULT_HANDLER DMA1_Stream5_IRQHandler
    DEFAULT_HANDLER DMA1_Stream6_IRQHandler
    DEFAULT_HANDLER ADC_IRQHandler
    DEFAULT_HANDLER CAN1_TX_IRQHandler
    DEFAULT_HANDLER CAN1_RX0_IRQHandler
    DEFAULT_HANDLER CAN1_RX1_IRQHandler
    DEFAULT_HANDLER CAN1_SCE_IRQHandler
    DEFAULT_HANDLER EXTI9_5_IRQHandler
    DEFAULT_HANDLER TIM1_BRK_TIM9_IRQHandler
    DEFAULT_HANDLER TIM1_UP_TIM10_IRQHandler
    DEFAULT_HANDLER TIM1_TRG_COM_TIM11_IRQHandler
    DEFAULT_HANDLER TIM1_CC_IRQHandler
    DEFAULT_HANDLER TIM2_IRQHandler
    DEFAULT_HANDLER TIM3_IRQHandler
    DEFAULT_HANDLER TIM4_IRQHandler
    DEFAULT_HANDLER I2C1_EV_IRQHandler
    DEFAULT_HANDLER I2C1_ER_IRQHandler
    DEFAULT_HANDLER I2C2_EV_IRQHandler
    DEFAULT_HANDLER I2C2_ER_IRQHandler
    DEFAULT_HANDLER SPI1_IRQHandler
    DEFAULT_HANDLER SPI2_IRQHandler
    DEFAULT_HANDLER USART1_IRQHandler
    DEFAULT_HANDLER USART2_IRQHandler
    DEFAULT_HANDLER USART3_IRQHandler
    DEFAULT_HANDLER EXTI15_10_IRQHandler
    DEFAULT_HANDLER RTC_Alarm_IRQHandler
    DEFAULT_HANDLER OTG_FS_WKUP_IRQHandler
    DEFAULT_HANDLER TIM8_BRK_TIM12_IRQHandler
    DEFAULT_HANDLER TIM8_UP_TIM13_IRQHandler
    DEFAULT_HANDLER TIM8_TRG_COM_TIM14_IRQHandler
    DEFAULT_HANDLER TIM8_CC_IRQHandler
    DEFAULT_HANDLER DMA1_Stream7_IRQHandler
    DEFAULT_HANDLER FSMC_IRQHandler
    DEFAULT_HANDLER SDIO_IRQHandler
    DEFAULT_HANDLER TIM5_IRQHandler
    DEFAULT_HANDLER SPI3_IRQHandler
    DEFAULT_HANDLER UART4_IRQHandler
    DEFAULT_HANDLER UART5_IRQHandler
    DEFAULT_HANDLER TIM6_DAC_IRQHandler
    DEFAULT_HANDLER TIM7_IRQHandler
    DEFAULT_HANDLER DMA2_Stream0_IRQHandler
    DEFAULT_HANDLER DMA2_Stream1_IRQHandler
    DEFAULT_HANDLER DMA2_Stream2_IRQHandler
    DEFAULT_HANDLER DMA2_Stream3_IRQHandler
    DEFAULT_HANDLER DMA2_Stream4_IRQHandler
    DEFAULT_HANDLER ETH_IRQHandler
    DEFAULT_HANDLER ETH_WKUP_IRQHandler
    DEFAULT_HANDLER CAN2_TX_IRQHandler
    DEFAULT_HANDLER CAN2_RX0_IRQHandler
    DEFAULT_HANDLER CAN2_RX1_IRQHandler
    DEFAULT_HANDLER CAN2_SCE_IRQHandler
    DEFAULT_HANDLER OTG_FS_IRQHandler
    DEFAULT_HANDLER DMA2_Stream5_IRQHandler
    DEFAULT_HANDLER DMA2_Stream6_IRQHandler
    DEFAULT_HANDLER DMA2_Stream7_IRQHandler
    DEFAULT_HANDLER USART6_IRQHandler
    DEFAULT_HANDLER I2C3_EV_IRQHandler
    DEFAULT_HANDLER I2C3_ER_IRQHandler
    DEFAULT_HANDLER OTG_HS_EP1_OUT_IRQHandler
    DEFAULT_HANDLER OTG_HS_EP1_IN_IRQHandler
    DEFAULT_HANDLER OTG_HS_WKUP_IRQHandler
    DEFAULT_HANDLER OTG_HS_IRQHandler
    DEFAULT_HANDLER DCMI_IRQHandler
    DEFAULT_HANDLER HASH_RNG_IRQHandler
    DEFAULT_HANDLER FPU_IRQHandler

    .end
