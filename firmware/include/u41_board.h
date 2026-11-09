/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u41_board.h - U41 pin map for the Sedna RIO v4.04 carrier board.
 *
 * Source of truth:
 *   toolchain/rio_board_schematics/rio_4.04/schematics/pwr_mgmt.xml
 *   toolchain/rio_board_schematics/rio_4.04/rio.pdf  (sheet 8, "pwr_mgmt")
 *
 * U41 = STM32L051K6U6TR in UFQFPN32.  The package pin numbers below were
 * cross-checked against the extracted netlist, which independently pins down
 * the mapping:
 *
 *   U41.1, U41.17 -> PM_3V3   (VDD)
 *   U41.5         -> VDDA via FB13
 *   U41.4         -> NRST, pulled up by R191 10K, driven by U42
 *                    (TPS3808G30QDBVRQ1 supervisor) and by J12.5
 *   U41.23        -> JTAG_M_TMS/SWDIO -> J12.4      => pin 23 is PA13
 *   U41.24        -> JTAG_M_TCK/SWCLK -> J12.2      => pin 24 is PA14
 *   U41.31        -> BOOT0, R188 10K pulldown + S4 (CHS-01TA DIP) to PM_3V3
 *   U41.33        -> EPAD -> I_GND
 *
 * The SWDIO/SWCLK pin numbers confirm the standard UFQFPN32 pinout, from
 * which the rest of the map follows.
 *
 * IMPORTANT WIRING FACTS that shape this firmware:
 *
 *   - SWD (PA13/PA14) and NRST reach J12 ONLY.  The nets JTAG_M_TCK/SWCLK,
 *     JTAG_M_TMS/SWDIO, JTAG_M_RESET, MCU_RESET and PM_RESET appear only on
 *     the pwr_mgmt sheet.  The i.MX8 SoM connector J5 has no SWD and no reset
 *     line to U41.  The SoM therefore cannot halt, reset or SWD-flash U41;
 *     its only channel is the PM_SPI link below.
 *
 *   - BOOT0 is set by DIP switch S4 with a 10K pulldown.  It is not routed to
 *     the SoM, so Linux cannot drop U41 into ST's built-in system bootloader.
 *     (RM0377 3.2: the category 3 system bootloader would otherwise speak
 *     SPI1/SPI2/USART1/USART2.)  Our own bootloader has to be the recovery
 *     path, hence the A/B scheme.
 *
 *   - PM_SPI_CS/SCK/MISO/MOSI are ports present on BOTH pwr_mgmt.xml and
 *     IMX8_Module.xml, i.e. this is the one link between the SoM and U41.
 *     PM_SPI_CS is J5 pin 176 (SoM GPIO2_IO03).  The bus is shared with
 *     GNSS_SPI_CS (J5.174, GPIO2_IO02) and I_SPI_CS (J5.52).
 */

#ifndef U41_BOARD_H
#define U41_BOARD_H

#include "stm32l0xx.h"

/* ------------------------------------------------------ analogue monitors */

/* PA0 / pin 6  - INPUT_V : scaled main input rail sense. ADC channel 0. */
#define U41_INPUT_V_PORT        GPIOA
#define U41_INPUT_V_PIN         0u
#define U41_INPUT_V_ADC_CH      0u

/* PA1 / pin 7  - CAP_V   : supercapacitor stack voltage. ADC channel 1. */
#define U41_CAP_V_PORT          GPIOA
#define U41_CAP_V_PIN           1u
#define U41_CAP_V_ADC_CH        1u

/* ------------------------------------------------------------- outputs */

/* PA2 / pin 8  - SCAP_EN     : supercap charger enable, active high. */
#define U41_SCAP_EN_PORT        GPIOA
#define U41_SCAP_EN_PIN         2u

/* PA3 / pin 9  - PSE_PWR_EN  : PoE PSE power enable, active high. */
#define U41_PSE_PWR_EN_PORT     GPIOA
#define U41_PSE_PWR_EN_PIN      3u

/* PB3 / pin 26 - SOM_PSU_EN  : i.MX8 SoM rail enable, active high. */
#define U41_SOM_PSU_EN_PORT     GPIOB
#define U41_SOM_PSU_EN_PIN      3u

/* PB4 / pin 27 - WIFI_PSU_EN : WiFi/BT rail enable, active high. */
#define U41_WIFI_PSU_EN_PORT    GPIOB
#define U41_WIFI_PSU_EN_PIN     4u

/* PB5 / pin 28 - LTE_PSU_EN  : LTE modem rail enable, active high. */
#define U41_LTE_PSU_EN_PORT     GPIOB
#define U41_LTE_PSU_EN_PIN      5u

/* -------------------------------------------------------------- inputs */

/* PB0 / pin 14 - SOM_PSU_PG  : SoM rail power-good, active high. */
#define U41_SOM_PSU_PG_PORT     GPIOB
#define U41_SOM_PSU_PG_PIN      0u

/* PB1 / pin 15 - WIFI_PSU_PG : WiFi rail power-good, active high. */
#define U41_WIFI_PSU_PG_PORT    GPIOB
#define U41_WIFI_PSU_PG_PIN     1u

/* PB2 / pin 16 - LTE_PSU_PG  : LTE rail power-good, active high. */
#define U41_LTE_PSU_PG_PORT     GPIOB
#define U41_LTE_PSU_PG_PIN      2u

/* PA8 / pin 18 - OC_PG       : over-current / power-good from the input
 *                              stage.  Active high = healthy. */
#define U41_OC_PG_PORT          GPIOA
#define U41_OC_PG_PIN           8u

/*
 * PB8 / pin 32 - IGN_INP : ignition sense.
 *
 * ACTIVE LOW.  The ignition line is conditioned on the Overview sheet by an
 * optocoupler (U1 ACPL-M43T) followed by an N-MOSFET (Q1 DMN2004) whose drain
 * is pulled up to SOM_3V3 by R2 (10K).  That MOSFET stage INVERTS: ignition
 * active turns the opto/Q1 on and pulls the logic node - and hence PB8 - LOW;
 * ignition off releases it HIGH.  So a LOW read means ignition is ON.
 *
 * The signal arrives at PB8 as a clean 3.3V logic level with no RC or Schmitt
 * on the pwr_mgmt side, so it needs only debounce in firmware, not analog
 * conditioning.  See docs/rio_4.04/IGNITION-PATH.md.
 */
#define U41_IGN_INP_PORT        GPIOB
#define U41_IGN_INP_PIN         8u
#define U41_IGN_ACTIVE_LEVEL    0   /* ignition-on drives PB8 low */

/*
 * PA15 / pin 25 - SOM_FB : keep-alive from the SoM.
 *
 * On the Linux side this is PCA9534 U19 pin P7 on lpi2c1, driven by a
 * gpio-hog in meta-rio/recipes-kernel/linux/files/imx8qm-rio.dts:
 *
 *     som-fb-hog {
 *             gpio-hog;
 *             gpios     = <7 GPIO_ACTIVE_HIGH>;
 *             output-high;              // high = SOM alive signal to MCU
 *             line-name = "som-fb";
 *     };
 *
 * so HIGH means "the SoM is alive".  Note the som-fb-keepalive_1.0.bb
 * DESCRIPTION in that same layer says the service drives SOM_FB "low", which
 * contradicts the hog and its comment.  We follow the hog.  If that turns out
 * to be backwards on real hardware, flip U41_SOM_FB_ALIVE_LEVEL and nothing
 * else needs to change.
 */
#define U41_SOM_FB_PORT         GPIOA
#define U41_SOM_FB_PIN          15u
#define U41_SOM_FB_ALIVE_LEVEL  1

/* ---------------------------------------------------------- PM_SPI (SPI1) */

/*
 * PA4  / pin 10 - PM_SPI_CS   -> SPI1_NSS   (AF0)
 * PA5  / pin 11 - PM_SPI_SCK  -> SPI1_SCK   (AF0)
 * PA6  / pin 12 - PM_SPI_MISO -> SPI1_MISO  (AF0)
 * PA7  / pin 13 - PM_SPI_MOSI -> SPI1_MOSI  (AF0)
 *
 * U41 is the SPI SLAVE; the i.MX8 is the master.  Naming is from the master's
 * point of view, so U41 drives MISO and samples MOSI.
 */
#define U41_SPI_PORT            GPIOA
#define U41_SPI_NSS_PIN         4u
#define U41_SPI_SCK_PIN         5u
#define U41_SPI_MISO_PIN        6u
#define U41_SPI_MOSI_PIN        7u
#define U41_SPI_AF              0u

/* ------------------------------------------------------ PWR I2C (I2C1) */

/*
 * PB6 / pin 29 - PWR_SCL -> I2C1_SCL (AF1), 2.2K pullup R184 to PM_3V3
 * PB7 / pin 30 - PWR_SDA -> I2C1_SDA (AF1), 2.2K pullup R185 to PM_3V3
 *
 * These go to the psu sheet, not to the SoM.  Not used by this firmware yet;
 * the pins are left as inputs so they do not fight the bus.
 */
#define U41_PWR_I2C_PORT        GPIOB
#define U41_PWR_SCL_PIN         6u
#define U41_PWR_SDA_PIN         7u
#define U41_PWR_I2C_AF          1u

/* ------------------------------------------------------------- helpers */

#define U41_GPIO_SET(port, pin)    ((port)->BSRR = (1UL << (pin)))
#define U41_GPIO_CLR(port, pin)    ((port)->BRR  = (1UL << (pin)))
#define U41_GPIO_GET(port, pin)    (((port)->IDR >> (pin)) & 1UL)

#define U41_GPIO_WRITE(port, pin, v) \
	do { \
		if (v) U41_GPIO_SET(port, pin); \
		else   U41_GPIO_CLR(port, pin); \
	} while (0)

/* MODER field values. */
#define U41_MODE_INPUT   0UL
#define U41_MODE_OUTPUT  1UL
#define U41_MODE_AF      2UL
#define U41_MODE_ANALOG  3UL

static inline void u41_gpio_mode(GPIO_TypeDef *port, unsigned pin, uint32_t mode)
{
	port->MODER = (port->MODER & ~(3UL << (pin * 2u))) | (mode << (pin * 2u));
}

static inline void u41_gpio_af(GPIO_TypeDef *port, unsigned pin, uint32_t af)
{
	volatile uint32_t *r = &port->AFR[pin >> 3u];
	unsigned sh = (pin & 7u) * 4u;

	*r = (*r & ~(0xFUL << sh)) | (af << sh);
	u41_gpio_mode(port, pin, U41_MODE_AF);
}

/* 0 = none, 1 = pull-up, 2 = pull-down. */
static inline void u41_gpio_pull(GPIO_TypeDef *port, unsigned pin, uint32_t pull)
{
	port->PUPDR = (port->PUPDR & ~(3UL << (pin * 2u))) | (pull << (pin * 2u));
}

static inline void u41_gpio_ospeed(GPIO_TypeDef *port, unsigned pin, uint32_t sp)
{
	port->OSPEEDR = (port->OSPEEDR & ~(3UL << (pin * 2u))) | (sp << (pin * 2u));
}

#endif /* U41_BOARD_H */
