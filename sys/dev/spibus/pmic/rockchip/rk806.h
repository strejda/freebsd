/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _RK806_H_

/* RK806 */
#define RK806_POWER_EN0			0x0
#define RK806_POWER_EN1			0x1
#define RK806_POWER_EN2			0x2
#define RK806_POWER_EN3			0x3
#define RK806_POWER_EN4			0x4
#define RK806_POWER_EN5			0x5
#define RK806_POWER_SLP_EN0		0x6
#define RK806_POWER_SLP_EN1		0x7
#define RK806_POWER_SLP_EN2		0x8
#define RK806_POWER_DISCHRG_EN0		0x9
#define RK806_POWER_DISCHRG_EN1		0xA
#define RK806_POWER_DISCHRG_EN2		0xB
#define RK806_BUCK_FB_CONFIG		0xC
#define RK806_SLP_LP_CONFIG		0xD
#define RK806_POWER_FPWM_EN0		0xE
#define RK806_POWER_FPWM_EN1		0xF
#define RK806_BUCK1_CONFIG		0x10
#define RK806_BUCK2_CONFIG		0x11
#define RK806_BUCK3_CONFIG		0x12
#define RK806_BUCK4_CONFIG		0x13
#define RK806_BUCK5_CONFIG		0x14
#define RK806_BUCK6_CONFIG		0x15
#define RK806_BUCK7_CONFIG		0x16
#define RK806_BUCK8_CONFIG		0x17
#define RK806_BUCK9_CONFIG		0x18
#define RK806_BUCK10_CONFIG		0x19
#define RK806_BUCK1_ON_VSEL		0x1A
#define RK806_BUCK2_ON_VSEL		0x1B
#define RK806_BUCK3_ON_VSEL		0x1C
#define RK806_BUCK4_ON_VSEL		0x1D
#define RK806_BUCK5_ON_VSEL		0x1E
#define RK806_BUCK6_ON_VSEL		0x1F
#define RK806_BUCK7_ON_VSEL		0x20
#define RK806_BUCK8_ON_VSEL		0x21
#define RK806_BUCK9_ON_VSEL		0x22
#define RK806_BUCK10_ON_VSEL		0x23
#define RK806_BUCK1_SLP_VSEL		0x24
#define RK806_BUCK2_SLP_VSEL		0x25
#define RK806_BUCK3_SLP_VSEL		0x26
#define RK806_BUCK4_SLP_VSEL		0x27
#define RK806_BUCK5_SLP_VSEL		0x28
#define RK806_BUCK6_SLP_VSEL		0x29
#define RK806_BUCK7_SLP_VSEL		0x2A
#define RK806_BUCK8_SLP_VSEL		0x2B
#define RK806_BUCK9_SLP_VSEL		0x2D
#define RK806_BUCK10_SLP_VSEL		0x2E
#define RK806_NLDO_IMAX			0x42
#define RK806_NLDO1_ON_VSEL		0x43
#define RK806_NLDO2_ON_VSEL		0x44
#define RK806_NLDO3_ON_VSEL		0x45
#define RK806_NLDO4_ON_VSEL		0x46
#define RK806_NLDO5_ON_VSEL		0x47
#define RK806_NLDO1_SLP_VSEL		0x48
#define RK806_NLDO2_SLP_VSEL		0x49
#define RK806_NLDO3_SLP_VSEL		0x4A
#define RK806_NLDO4_SLP_VSEL		0x4B
#define RK806_NLDO5_SLP_VSEL		0x4C
#define RK806_PLDO_IMAX			0x4D
#define RK806_PLDO1_ON_VSEL		0x4E
#define RK806_PLDO2_ON_VSEL		0x4F
#define RK806_PLDO3_ON_VSEL		0x50
#define RK806_PLDO4_ON_VSEL		0x51
#define RK806_PLDO5_ON_VSEL		0x52
#define RK806_PLDO6_ON_VSEL		0x53
#define RK806_PLDO1_SLP_VSEL		0x54
#define RK806_PLDO2_SLP_VSEL		0x55
#define RK806_PLDO3_SLP_VSEL		0x56
#define RK806_PLDO4_SLP_VSEL		0x57
#define RK806_PLDO5_SLP_VSEL		0x58
#define RK806_PLDO6_SLP_VSEL		0x59
#define RK806_CHIP_NAME			0x5A
#define RK806_CHIP_VER			0x5B
#define RK806_OTP_VER			0x5C
#define RK806_SYS_STS			0x5D
#define RK806_SYS_CFG0			0x5E
#define RK806_SYS_CFG1			0x5F
#define RK806_SYS_OPTION		0x61
#define RK806_PWRCTRL_CONFIG0		0x62
#define RK806_PWRCTRL_CONFIG1		0x63
#define RK806_VSEL_CTR_SEL0		0x64
#define RK806_VSEL_CTR_SEL1		0x65
#define RK806_VSEL_CTR_SEL2		0x66
#define RK806_VSEL_CTR_SEL3		0x67
#define RK806_VSEL_CTR_SEL4		0x68
#define RK806_VSEL_CTR_SEL5		0x69
#define RK806_DVS_CTRL_SEL0		0x6A
#define RK806_DVS_CTRL_SEL1		0x6B
#define RK806_DVS_CTRL_SEL2		0x6C
#define RK806_DVS_CTRL_SEL3		0x6D
#define RK806_DVS_CTRL_SEL4		0x6E
#define RK806_DVS_CTRL_SEL5		0x6F
#define RK806_DVS_START_CTRL		0x70
#define RK806_PWRCTRL_GPIO		0x71
#define RK806_SYS_CFG3			0x72
#define RK806_WDT_REG			0x73
#define RK806_ON_SOURCE			0x74
#define RK806_OFF_SOURCE		0x75
#define RK806_PWRON_KEY			0x76
#define RK806_INT_STS0			0x77
#define RK806_INT_MSK0			0x78
#define RK806_INT_STS1			0x79
#define RK806_INT_MSK1			0x7A
#define RK806_GPIO_INT_CONFIG		0x7B
#define RK806_DATA_REG0			0x7C
#define RK806_DATA_REG1			0x7D
#define RK806_DATA_REG2			0x7E
#define RK806_DATA_REG3			0x7F
#define RK806_DATA_REG4			0x80
#define RK806_DATA_REG5			0x81
#define RK806_DATA_REG6			0x82
#define RK806_DATA_REG7			0x83
#define RK806_DATA_REG8			0x84
#define RK806_DATA_REG9			0x85
#define RK806_DATA_REG10		0x86
#define RK806_DATA_REG11		0x87
#define RK806_DATA_REG12		0x88
#define RK806_DATA_REG13		0x89
#define RK806_DATA_REG14		0x8A
#define RK806_DATA_REG15		0x8B
#define RK806_BUCK_SEQ_REG0		0xB2
#define RK806_BUCK_SEQ_REG1		0xB3
#define RK806_BUCK_SEQ_REG2		0xB4
#define RK806_BUCK_SEQ_REG3		0xB5
#define RK806_BUCK_SEQ_REG4		0xB6
#define RK806_BUCK_SEQ_REG5		0xB7
#define RK806_BUCK_SEQ_REG6		0xB8
#define RK806_BUCK_SEQ_REG7		0xB9
#define RK806_BUCK_SEQ_REG8		0xBA
#define RK806_BUCK_SEQ_REG9		0xBB
#define RK806_BUCK_SEQ_REG10		0xBC
#define RK806_BUCK_SEQ_REG11		0xBD
#define RK806_BUCK_SEQ_REG12		0xBE
#define RK806_BUCK_SEQ_REG13		0xBF
#define RK806_BUCK_SEQ_REG14		0xC0
#define RK806_BUCK_SEQ_REG15		0xC1
#define RK806_BUCK_SEQ_REG16		0xC2
#define RK806_BUCK_SEQ_REG17		0xC3
#define RK806_BACKUP_REG7		0xDC
#define RK806_BACKUP_REG6		0xE6
#define RK806_BACKUP_REG5		0xE7
#define RK806_BACKUP_REG1		0xE8
#define RK806_BACKUP_REG2		0xE9
#define RK806_BACKUP_REG3		0xEA
#define RK806_BACKUP_REG4		0xEB




struct rk806_reg_sc;
struct rk806_gpio_pin;

struct rk806_softc {
	device_t		dev;
	struct sx		lock;
	struct resource		*irq_res;
	void			*irq_h;

	uint16_t		chip_name;
	uint8_t			chip_ver;
	uint8_t			otp_ver;

	/* Regulators. */
	struct rk806_reg_sc	**regs;
	int			nregs;

	/* GPIO */
	device_t		gpio_busdev;
	struct rk806_gpio_pin	**gpio_pins;
	int			gpio_npins;
	struct sx		gpio_lock;

};

#define	RD1(sc, reg, val)	rk806_read(sc, reg, val)
#define	WR1(sc, reg, val)	rk806_write(sc, reg, val)
#define	RM1(sc, reg, clr, set)	rk806_modify(sc, reg, clr, set)

int rk806_read(struct rk806_softc *sc, uint8_t reg, uint8_t *val);
int rk806_write(struct rk806_softc *sc, uint8_t reg, uint8_t val);
int rk806_modify(struct rk806_softc *sc, uint8_t reg, uint8_t clear,
    uint8_t set);
int rk806_read_buf(struct rk806_softc *sc, uint8_t reg, uint8_t *buf,
    size_t size);
int rk806_write_buf(struct rk806_softc *sc, uint8_t reg, uint8_t *buf,
    size_t size);

/* Regulators */
int rk806_regulator_attach(struct rk806_softc *sc, phandle_t node);
int rk806_regulator_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *num);


/* GPIO */
device_t rk806_gpio_get_bus(device_t dev);
int rk806_gpio_pin_max(device_t dev, int *maxpin);
int rk806_gpio_pin_getname(device_t dev, uint32_t pin, char *name);
int rk806_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags);
int rk806_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps);
int rk806_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags);
int rk806_gpio_pin_set(device_t dev, uint32_t pin, unsigned int value);
int rk806_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *val);
int rk806_gpio_pin_toggle(device_t dev, uint32_t pin);
int rk806_gpio_map_gpios(device_t dev, phandle_t pdev, phandle_t gparent,
    int gcells, pcell_t *gpios, uint32_t *pin, uint32_t *flags);
int rk806_gpio_attach(struct rk806_softc *sc, phandle_t node);
int rk806_pinmux_configure(device_t dev, phandle_t cfgxref);

#endif /* _RK806_H_ */
