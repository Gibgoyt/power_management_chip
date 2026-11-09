/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * startup.c - reset handler and vector table for the STM32L051.
 *
 * One file serves the bootloader and both application builds.  The linker
 * script decides where the vector table lands; the reset handler is otherwise
 * identical.
 *
 * The .ramfunc copy is the part that matters here.  u41_flash.h explains why
 * the flash routines cannot execute from flash; this is where they get moved
 * into SRAM, before any C code that might call them runs.
 */

#include <stdint.h>

#include "stm32l0xx.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern uint32_t _siramfunc, _sramfunc, _eramfunc;

extern int main(void);
void Reset_Handler(void);
void Default_Handler(void);

/* Handlers the application overrides.  Everything else traps. */
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SPI1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));

/*
 * STM32L051 vector table: 16 system entries plus 32 external interrupts.
 * 48 words = 192 bytes, which is why the slot bases are 256-byte aligned
 * (ARMv6-M VTOR only implements bits [31:8]).
 */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
	(void (*)(void))(&_estack),   /*  0 initial stack pointer */
	Reset_Handler,                /*  1 reset                 */
	NMI_Handler,                  /*  2 NMI                   */
	HardFault_Handler,            /*  3 hard fault            */
	0, 0, 0, 0, 0, 0, 0,          /*  4..10 reserved          */
	SVC_Handler,                  /* 11 SVCall                */
	0, 0,                         /* 12..13 reserved          */
	PendSV_Handler,               /* 14 PendSV                */
	SysTick_Handler,              /* 15 SysTick               */

	Default_Handler,              /*  0 WWDG                  */
	Default_Handler,              /*  1 PVD                   */
	Default_Handler,              /*  2 RTC                   */
	Default_Handler,              /*  3 FLASH                 */
	Default_Handler,              /*  4 RCC                   */
	Default_Handler,              /*  5 EXTI0_1               */
	Default_Handler,              /*  6 EXTI2_3               */
	Default_Handler,              /*  7 EXTI4_15              */
	Default_Handler,              /*  8 reserved              */
	Default_Handler,              /*  9 DMA1_Channel1         */
	Default_Handler,              /* 10 DMA1_Channel2_3       */
	Default_Handler,              /* 11 DMA1_Channel4_5_6_7   */
	Default_Handler,              /* 12 ADC1_COMP             */
	Default_Handler,              /* 13 LPTIM1                */
	Default_Handler,              /* 14 reserved              */
	Default_Handler,              /* 15 TIM2                  */
	Default_Handler,              /* 16 reserved              */
	Default_Handler,              /* 17 reserved              */
	Default_Handler,              /* 18 reserved              */
	Default_Handler,              /* 19 TIM21                 */
	Default_Handler,              /* 20 reserved              */
	Default_Handler,              /* 21 TIM22                 */
	Default_Handler,              /* 22 I2C1                  */
	Default_Handler,              /* 23 reserved              */
	SPI1_IRQHandler,              /* 24 SPI1                  */
	Default_Handler,              /* 25 reserved              */
	Default_Handler,              /* 26 USART1                */
	Default_Handler,              /* 27 USART2                */
	Default_Handler,              /* 28 LPUART1               */
	Default_Handler,              /* 29 reserved              */
	Default_Handler,              /* 30 reserved              */
	Default_Handler,              /* 31 reserved              */
};

void Default_Handler(void)
{
	/*
	 * Nothing sensible to do on a stray interrupt in a power manager: we
	 * must not silently continue with an unknown state, and we must not
	 * drop the rails.  Sit here and let the independent watchdog reset us,
	 * which restarts the bootloader and re-runs slot validation.
	 */
	for (;;)
		__WFI();
}

void Reset_Handler(void)
{
	uint32_t *src, *dst;

	/*
	 * The application is linked at a slot base, not at 0x08000000, and the
	 * bootloader has already pointed VTOR at us.  Setting it again here is
	 * harmless and means the image is also correct if it is ever started
	 * directly by a debugger over J12.
	 */
	SCB->VTOR = (uint32_t)g_vectors;

	/* .data: initialised variables, flash image -> SRAM. */
	src = &_sidata;
	for (dst = &_sdata; dst < &_edata; )
		*dst++ = *src++;

	/*
	 * .ramfunc: the flash driver.  This has to happen before main(), and
	 * certainly before anything calls into u41_flash.c, because those
	 * routines stall on their own instruction fetch if they run from NVM.
	 */
	src = &_siramfunc;
	for (dst = &_sramfunc; dst < &_eramfunc; )
		*dst++ = *src++;

	/* .bss */
	for (dst = &_sbss; dst < &_ebss; )
		*dst++ = 0;

	(void)main();

	/* main() does not return.  If it somehow does, wait for the watchdog. */
	for (;;)
		__WFI();
}
