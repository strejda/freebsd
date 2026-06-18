/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 */

#ifndef _RP1_CLK_REG_H_
#define _RP1_CLK_REG_H_

#define GPCLK_OE			0x00000

#define CLK_SYS				0x00014
#define CLK_SLOW_SYS			0x00024
#define CLK_DMA				0x00044
#define CLK_UART			0x00054
#define CLK_ETH				0x00064
#define CLK_PWM0			0x00074
#define CLK_PWM1			0x00084
#define CLK_AUDIO_IN			0x00094
#define CLK_AUDIO_OUT			0x000a4
#define CLK_I2S				0x000b4
#define CLK_MIPI0_CFG			0x000c4
#define CLK_MIPI1_CFG			0x000d4
#define CLK_PCIE_AUX			0x000e4
#define CLK_USBH0_MICROFRAME		0x000f4
#define CLK_USBH1_MICROFRAME		0x00104
#define CLK_USBH0_SUSPEND		0x00114
#define CLK_USBH1_SUSPEND		0x00124
#define CLK_ETH_TSU			0x00134
#define CLK_ADC				0x00144
#define CLK_SDIO_TIMER			0x00154
#define CLK_SDIO_ALT_SRC		0x00164
#define CLK_GP0				0x00174
#define CLK_GP1				0x00184
#define CLK_GP2				0x00194
#define CLK_GP3				0x001a4
#define CLK_GP4				0x001b4
#define CLK_GP5				0x001c4
#define CLK_SYS_RESUS_CTRL		0x0020c

#define CLK_SLOW_SYS_RESUS_CTRL		0x00214


#define VIDEO_CLK_VEC			0x04000
#define VIDEO_CLK_DPI			0x04010
#define VIDEO_CLK_MIPI0_DPI		0x04020
#define VIDEO_CLK_MIPI1_DPI		0x04030

#define PLL_SYS				0x08000
#define PLL_AUDIO			0x0c000
#define PLL_AUDIO_TERN			0x0c018

#define PLL_VIDEO			0x10000

#endif