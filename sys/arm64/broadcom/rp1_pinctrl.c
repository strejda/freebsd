/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/intr.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/fdt/fdt_pinctrl.h>

#include "gpio_if.h"
#include "pic_if.h"
#include "fdt_pinctrl_if.h"

#define RP1_NUM_BANKS	3
#define RP1_NFNCS	9


#define	RP1_GPIO_CAPS						  \
    (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT |					  \
    GPIO_INTR_LEVEL_LOW | GPIO_INTR_LEVEL_HIGH |			  \
    GPIO_INTR_EDGE_RISING | GPIO_INTR_EDGE_FALLING | GPIO_INTR_EDGE_BOTH) \

#define	RP1_PINCTRL_LOCK(_sc)		mtx_lock_spin(&(_sc)->mtx)
#define	RP1_PINCTRL_UNLOCK(_sc)	mtx_unlock_spin(&(_sc)->mtx)
#define	RP1_PINCTRL_LOCK_ASSERT(_sc)	mtx_assert(&(_sc)->mtx, MA_OWNED)

#define RP1_REG(bank, sp, reg, span) 					      \
	(rp1_banks[bank].offs + (sp) * (span) + (reg))
#define GPIO_RD4(sc, bank, sp, reg)					      \
	bus_read_4((sc)->mem_res[0], RP1_REG(bank, sp, reg , 8))
#define GPIO_WR4(sc, bank, sp, reg, val)				      \
	bus_write_4((sc)->mem_res[0],  RP1_REG(bank, sp, reg , 8), (val))

#define PADS_RD4(sc, bank, sp, reg)					      \
	bus_read_4((sc)->mem_res[2],  RP1_REG(bank, sp, reg , 4))
#define PADS_WR4(sc, bank, sp, reg, val)				      \
	bus_write_4((sc)->mem_res[2],  RP1_REG(bank, sp, reg , 4), (val))

#define RIO_RD4(sc, bank, reg)						      \
	bus_read_4((sc)->mem_res[1], rp1_banks[bank].offs + (reg))
#define RIO_WR4(sc, bank, reg, val)					      \
	bus_write_4((sc)->mem_res[1], rp1_banks[bank].offs + (reg), (val))

#define RP1_GPIO_STATUS		0x00
#define RP1_GPIO_CTRL		0x04
#define  RP1_CTRL_INOVER_MASK		0x03
#define  RP1_CTRL_INOVER_SHIFT		16
#define  RP1_CTRL_OEOVER_MASK		0x03
#define  RP1_CTRL_OEOVER_SHIFT		14
#define  RP1_CTRL_OUTOVER_MASK		0x03
#define  RP1_CTRL_OUTOVER_SHIFT		12
#define  RP1_CTRL_FNC_MASK		0x1F
#define  RP1_CTRL_FNC_SHIFT		0



#define RP1_PADS_VOLTAGE	0x00
#define RP1_PADS_CTRL		0x04
#define  RP1_CTRL_OD			7
#define  RP1_CTRL_IE			5
#define  RP1_CTRL_DRIVE_MASK		0x03
#define  RP1_CTRL_DRIVE_SHIFT		4
#define  RP1_CTRL_PUE			3
#define  RP1_CTRL_PDE			2
#define  RP1_CTRL_SCHMITT		1
#define  RP1_CTRL_SLEWFAST		0

#define RP1_RIO_OUT		0x0000
#define RP1_RIO_OE 		0x0004
#define RP1_RIO_NOSYNC_IN 	0x0008
#define RP1_RIO_SYNC_IN 	0x0008
#define RP1_RIO_OUT_XOR		0x1000
#define RP1_RIO_OE_XOR 		0x1004
#define RP1_RIO_OUT_SET		0x2000
#define RP1_RIO_OE_SET		0x2004
#define RP1_RIO_OUT_CLR		0x3000
#define RP1_RIO_OE_CLR		0x3004

struct rp1_cfg_param {
	bool bias_disable;
	bool bias_pull_down;
	bool bias_pull_up;
	bool input_enable;
	bool input_schmitt_enable;
	bool output_enable;
	bool output_high;
	bool output_low;
	int  slew_rate;
	int  drive_strength;
};

struct rp1_cookie {
	struct rp1_pinctrl_softc *sc;
	int	bank;
};

struct rp1_pinctrl_softc {
	device_t		dev;
	phandle_t		node;
	device_t		gpiobus_dev;
	struct resource		*mem_res[3];
	struct resource		*irq_res[3];
	struct mtx		mtx;
	struct gpio_pin		*pins;
	int			npins;
	void			*irq_ih[3];
	struct rp1_cookie	cookies[3];
};

struct rp1_grpdef {
	const char	*name;
	int 		npins;
	const int	*pins;
};

struct rp1_bankdef {
	int		bank;
	int 		npins;
	uint32_t	offs;
};

struct rp1_fncdef {
	int		pin;
	char 		*fncs[RP1_NFNCS];
};



/* Compatible devices. */
static struct ofw_compat_data compat_data[] = {
	{ "raspberrypi,rp1-gpio",		1},
	{NULL,					0},
};

static struct resource_spec rp1_pinctrl_mspec[] = {
	{ SYS_RES_MEMORY, 0, RF_ACTIVE },
	{ SYS_RES_MEMORY, 1, RF_ACTIVE },
	{ SYS_RES_MEMORY, 2, RF_ACTIVE },
	{ -1, 0 }
};

static struct resource_spec rp1_pinctrl_ispec[] = {
	{ SYS_RES_IRQ, 0, RF_ACTIVE },
	{ SYS_RES_IRQ, 1, RF_ACTIVE },
	{ SYS_RES_IRQ, 2, RF_ACTIVE },
	{ -1, 0 }
};

struct rp1_bankdef rp1_banks[] = {
	{.bank = 0, .npins = 28, .offs = 0x0000},
	{.bank = 1, .npins =  6, .offs = 0x4000},
	{.bank = 2, .npins = 20, .offs = 0x8000},
};

/*
 * Group name -> formal pin number translation table.
 */
#define RP1_GRP(_name, ...) {						\
	.name = _name,							\
	.npins = nitems(((unsigned int[]) {__VA_ARGS__})),		\
	.pins = (const unsigned int []) {__VA_ARGS__}			\
}
static const struct rp1_grpdef  rp1_grpdef[] = {
	RP1_GRP("uart0", 14, 15),
	RP1_GRP("uart0_ctrl", 4, 5, 6, 7, 16, 17),
	RP1_GRP("uart1", 0, 1),
	RP1_GRP("uart1_ctrl", 2, 3),
	RP1_GRP("uart2", 4, 5),
	RP1_GRP("uart2_ctrl", 6, 7),
	RP1_GRP("uart3", 8, 9),
	RP1_GRP("uart3_ctrl", 10, 11),
	RP1_GRP("uart4", 12, 13),
	RP1_GRP("uart4_ctrl", 14, 15),
	RP1_GRP("uart5_0", 30, 31),
	RP1_GRP("uart5_0_ctrl", 32, 33),
	RP1_GRP("uart5_1", 36, 37),
	RP1_GRP("uart5_1_ctrl", 38, 39),
	RP1_GRP("uart5_2", 40, 41),
	RP1_GRP("uart5_2_ctrl", 42, 43),
	RP1_GRP("uart5_3", 48, 49),
	RP1_GRP("sd0", 22, 23, 24, 25, 26, 27),
	RP1_GRP("sd1", 28, 29, 30, 31, 32, 33),
	RP1_GRP("i2s0", 18, 19, 20, 21),
	RP1_GRP("i2s0_dual", 18, 19, 20, 21, 22, 23),
	RP1_GRP("i2s0_quad", 18, 19, 20, 21, 22, 23, 24, 25, 26, 27),
	RP1_GRP("i2s1", 18, 19, 20, 21),
	RP1_GRP("i2s1_dual", 18, 19, 20, 21, 22, 23),
	RP1_GRP("i2s1_quad", 18, 19, 20, 21, 22, 23, 24, 25, 26, 27),
	RP1_GRP("i2s2_0", 28, 29, 30, 31),
	RP1_GRP("i2s2_0_dual", 28, 29, 30, 31, 32, 33),
	RP1_GRP("i2s2_1", 42, 43, 44, 45),
	RP1_GRP("i2s2_1_dual", 42, 43, 44, 45, 46, 47),
	RP1_GRP("i2c4_0", 28, 29),
	RP1_GRP("i2c4_1", 34, 35),
	RP1_GRP("i2c4_2", 40, 41),
	RP1_GRP("i2c4_3", 46, 47),
	RP1_GRP("i2c6_0", 38, 39),
	RP1_GRP("i2c6_1", 51, 52),
	RP1_GRP("i2c5_0", 30, 31),
	RP1_GRP("i2c5_1", 36, 37),
	RP1_GRP("i2c5_2", 44, 45),
	RP1_GRP("i2c5_3", 49, 50),
	RP1_GRP("i2c0_0", 0, 1),
	RP1_GRP("i2c0_1", 8, 9),
	RP1_GRP("i2c1_0", 2, 3),
	RP1_GRP("i2c1_1", 10, 11),
	RP1_GRP("i2c2_0", 4, 5),
	RP1_GRP("i2c2_1", 12, 13),
	RP1_GRP("i2c3_0", 6, 7),
	RP1_GRP("i2c3_1", 14, 15),
	RP1_GRP("i2c3_2", 22, 23),
	RP1_GRP("dpi_16bit", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
		  11, 12, 13, 14, 15, 16, 17, 18, 19),
	RP1_GRP("dpi_16bit_cpadhi", 0, 1, 2, 3, 4, 5, 6, 7, 8,
		  12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24),
	RP1_GRP("dpi_16bit_pad666", 0, 1, 2, 3, 5, 6, 7, 8, 9,
		  12, 13, 14, 15, 16, 17, 21, 22, 23, 24, 25),
	RP1_GRP("dpi_18bit", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
		  11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21),
	RP1_GRP("dpi_18bit_cpadhi", 0, 1, 2, 3, 4, 5, 6, 7, 8,
		  9, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24,
		  25),
	RP1_GRP("dpi_24bit", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
		  11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
		  22, 23, 24, 25, 26, 27),
	RP1_GRP("spi0", 9, 10, 11),
	RP1_GRP("spi0_quad", 0, 1, 9, 10, 11),
	RP1_GRP("spi1", 19, 20, 21),
	RP1_GRP("spi2", 1, 2, 3),
	RP1_GRP("spi3", 5, 6, 7),
	RP1_GRP("spi4", 9, 10, 11),
	RP1_GRP("spi5", 13, 14, 15),
	RP1_GRP("spi6_0", 28, 29, 30),
	RP1_GRP("spi6_1", 40, 41, 42),
	RP1_GRP("spi7_0", 46, 47, 48),
	RP1_GRP("spi7_1", 49, 50, 51),
	RP1_GRP("spi8_0", 37, 38, 39),
	RP1_GRP("spi8_1", 49, 50, 51),
	RP1_GRP("aaud_0", 12, 13),
	RP1_GRP("aaud_1", 38, 39),
	RP1_GRP("aaud_2", 40, 41),
	RP1_GRP("aaud_3", 49, 50),
	RP1_GRP("aaud_4", 51, 52),
	RP1_GRP("vbus0_0", 28, 29),
	RP1_GRP("vbus0_1", 34, 35),
	RP1_GRP("vbus1", 42, 43),
	RP1_GRP("vbus2", 50, 51),
	RP1_GRP("vbus3", 52, 53),
	RP1_GRP("mic_0", 25, 26, 27),
	RP1_GRP("mic_1", 34, 35, 36),
	RP1_GRP("mic_2", 37, 38, 39),
	RP1_GRP("mic_3", 46, 47, 48),
	RP1_GRP("ir", 2, 3),
};

#define RP1_FNC(_pin, ...) {						\
	.pin= _pin,							\
	.fncs = {__VA_ARGS__}						\
}

#define RP1_FNC_GPIO	5
static const struct rp1_fncdef  rp1_fncdef[] = {
/*                  0               1                 2                3                 4       5          6               7         8 */
 RP1_FNC( 0,   "spi0",           "dpi",          "uart1",          "i2c0",            NULL, "gpio", "proc_rio",          "pio",   "spi2"),
 RP1_FNC( 1,   "spi0",           "dpi",          "uart1",          "i2c0",            NULL, "gpio", "proc_rio",          "pio",   "spi2"),
 RP1_FNC( 2,   "spi0",           "dpi",          "uart1",          "i2c1",            "ir", "gpio", "proc_rio",          "pio",   "spi2"),
 RP1_FNC( 3,   "spi0",           "dpi",          "uart1",          "i2c1",            "ir", "gpio", "proc_rio",          "pio",   "spi2"),
 RP1_FNC( 4, "gpclk0",           "dpi",          "uart2",          "i2c2",         "uart0", "gpio", "proc_rio",          "pio",   "spi3"),
 RP1_FNC( 5, "gpclk1",           "dpi",          "uart2",          "i2c2",         "uart0", "gpio", "proc_rio",          "pio",   "spi3"),
 RP1_FNC( 6, "gpclk2",           "dpi",          "uart2",          "i2c3",         "uart0", "gpio", "proc_rio",          "pio",   "spi3"),
 RP1_FNC( 7,   "spi0",           "dpi",          "uart2",          "i2c3",         "uart0", "gpio", "proc_rio",          "pio",   "spi3"),
 RP1_FNC( 8,   "spi0",           "dpi",          "uart3",          "i2c0",            NULL, "gpio", "proc_rio",          "pio",   "spi4"),
 RP1_FNC( 9,   "spi0",           "dpi",          "uart3",          "i2c0",            NULL, "gpio", "proc_rio",          "pio",   "spi4"),
 RP1_FNC(10,   "spi0",           "dpi",          "uart3",          "i2c1",            NULL, "gpio", "proc_rio",          "pio",   "spi4"),
 RP1_FNC(11,   "spi0",           "dpi",          "uart3",          "i2c1",            NULL, "gpio", "proc_rio",          "pio",   "spi4"),
 RP1_FNC(12,   "pwm0",           "dpi",          "uart4",          "i2c2",          "aaud", "gpio", "proc_rio",          "pio",   "spi5"),
 RP1_FNC(13,   "pwm0",           "dpi",          "uart4",          "i2c2",          "aaud", "gpio", "proc_rio",          "pio",   "spi5"),
 RP1_FNC(14,   "pwm0",           "dpi",          "uart4",          "i2c3",         "uart0", "gpio", "proc_rio",          "pio",   "spi5"),
 RP1_FNC(15,   "pwm0",           "dpi",          "uart4",          "i2c3",         "uart0", "gpio", "proc_rio",          "pio",   "spi5"),
 RP1_FNC(16,   "spi1",           "dpi",    "dsi0_te_ext",            NULL,         "uart0", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(17,   "spi1",           "dpi",    "dsi1_te_ext",            NULL,         "uart0", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(18,   "spi1",           "dpi",           "i2s0",          "pwm0",          "i2s1", "gpio", "proc_rio",          "pio", "gpclk1"),
 RP1_FNC(19,   "spi1",           "dpi",           "i2s0",          "pwm0",          "i2s1", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(20,   "spi1",           "dpi",           "i2s0",        "gpclk0",          "i2s1", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(21,   "spi1",           "dpi",           "i2s0",        "gpclk1",          "i2s1", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(22,    "sd0",           "dpi",           "i2s0",          "i2c3",          "i2s1", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(23,    "sd0",           "dpi",           "i2s0",          "i2c3",          "i2s1", "gpio", "proc_rio",          "pio",     NULL),
 RP1_FNC(24,    "sd0",           "dpi",           "i2s0",            NULL,          "i2s1", "gpio", "proc_rio",          "pio",   "spi2"),
 RP1_FNC(25,    "sd0",           "dpi",           "i2s0",           "mic",          "i2s1", "gpio", "proc_rio",          "pio",   "spi3"),
 RP1_FNC(26,    "sd0",           "dpi",           "i2s0",           "mic",          "i2s1", "gpio", "proc_rio",          "pio",   "spi5"),
 RP1_FNC(27,    "sd0",           "dpi",           "i2s0",           "mic",          "i2s1", "gpio", "proc_rio",          "pio",   "spi1"),
 RP1_FNC(28,    "sd1",          "i2c4",           "i2s2",          "spi6",         "vbus0", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(29,    "sd1",          "i2c4",           "i2s2",          "spi6",         "vbus0", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(30,    "sd1",          "i2c5",           "i2s2",          "spi6",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(31,    "sd1",          "i2c5",           "i2s2",          "spi6",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(32,    "sd1",        "gpclk3",           "i2s2",          "spi6",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(33,    "sd1",        "gpclk4",           "i2s2",          "spi6",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(34,   "pwm1",        "gpclk3",          "vbus0",          "i2c4",           "mic", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(35,   "spi8",          "pwm1",          "vbus0",          "i2c4",           "mic", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(36,   "spi8",         "uart5",  "pcie_clkreq_n",          "i2c5",           "mic", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(37,   "spi8",         "uart5",            "mic",          "i2c5", "pcie_clkreq_n", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(38,   "spi8",         "uart5",            "mic",          "i2c6",          "aaud", "gpio", "proc_rio",  "dsi0_te_ext",     NULL),
 RP1_FNC(39,   "spi8",         "uart5",            "mic",          "i2c6",          "aaud", "gpio", "proc_rio",  "dsi1_te_ext",     NULL),
 RP1_FNC(40,   "pwm1",         "uart5",           "i2c4",          "spi6",          "aaud", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(41,   "pwm1",         "uart5",           "i2c4",          "spi6",          "aaud", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(42, "gpclk5",         "uart5",          "vbus1",          "spi6",          "i2s2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(43, "gpclk4",         "uart5",          "vbus1",          "spi6",          "i2s2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(44, "gpclk5",          "i2c5",           "pwm1",          "spi6",          "i2s2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(45,   "pwm1",          "i2c5",           "spi7",          "spi6",          "i2s2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(46, "gpclk3",          "i2c4",           "spi7",           "mic",          "i2s2", "gpio", "proc_rio",  "dsi0_te_ext",     NULL),
 RP1_FNC(47, "gpclk5",          "i2c4",           "spi7",           "mic",          "i2s2", "gpio", "proc_rio",  "dsi1_te_ext",     NULL),
 RP1_FNC(48,   "pwm1", "pcie_clkreq_n",           "spi7",           "mic",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(49,   "spi8",         "spi7",            "i2c5",          "aaud",         "uart5", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(50,   "spi8",         "spi7",            "i2c5",          "aaud",         "vbus2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(51,   "spi8",         "spi7",            "i2c6",          "aaud",         "vbus2", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(52,   "spi8",           NULL,            "i2c6",          "aaud",         "vbus3", "gpio", "proc_rio",           NULL,     NULL),
 RP1_FNC(53,   "spi8",         "spi7",              NULL, "pcie_clkreq_n",         "vbus3", "gpio", "proc_rio",           NULL,     NULL),
};

static int
rp1_get_pin_bank(struct rp1_pinctrl_softc *sc, int pin, int *bank, int*subpin)
{
	int tmp, i;

	if (pin >= sc->npins)
		return (EINVAL);

	tmp = pin;
	for (i = 0; i < nitems(rp1_banks); i++) {
		if (tmp < rp1_banks[i].npins) {
			*bank = rp1_banks[i].bank;
			*subpin = tmp;
			return (0);
		}
		tmp -= rp1_banks[i].npins;
	}

	return(EINVAL);
}

static int
rp1_gpio_is_pin_gpio(struct rp1_pinctrl_softc *sc, int pin, int *bank,
    int *subpin)
{
	uint32_t reg;

	if (rp1_get_pin_bank(sc, pin, bank, subpin) != 0)
		return (false);

	reg = GPIO_RD4(sc, *bank, *subpin, RP1_GPIO_CTRL);
	reg >>= RP1_CTRL_FNC_SHIFT;
	reg &= RP1_CTRL_FNC_MASK;
	return (reg == RP1_FNC_GPIO);
}

static void
rp1_gpio_read_flags(struct rp1_pinctrl_softc *sc, int bank, int subpin,
    uint32_t *flags)
{
	uint32_t pads, rio;

	*flags = 0;
	pads = PADS_RD4(sc, bank, subpin, RP1_PADS_CTRL);
	rio = RIO_RD4(sc, bank, RP1_RIO_OE);

	rio >>= subpin;
	rio &= 1;
	if (pads & RP1_CTRL_PUE) *flags |= GPIO_PIN_PULLUP;
	if (pads & RP1_CTRL_PDE) *flags |= GPIO_PIN_PULLDOWN;

	if (rio == 0) {
		*flags |= GPIO_PIN_INPUT;
	} else {
		*flags |= GPIO_PIN_OUTPUT;
		if (pads & RP1_CTRL_OD) *flags |= GPIO_PIN_TRISTATE;
	}
}

/*
 * PINCTRL Interface
 */
static int
rp1_pinctrl_parse_fnc(struct rp1_pinctrl_softc *sc, int pin,
    char *fnc, int *fncnum)
{
	int i;
	const struct rp1_fncdef *def;

	/* Find pin # */
	def = NULL;
	for (i = 0; i < nitems(rp1_fncdef); i++) {
		if (rp1_fncdef[i].pin == pin) {
			def = rp1_fncdef + i;
			break;
		}
	}
	if (def == NULL)
		return (EINVAL);

	/* Find function name */
	for (i = 0; i < RP1_NFNCS; i++) {
		if (strcmp (def->fncs[i], fnc) == 0) {
			*fncnum = i;
			return (0);
		}
	}
	return (EINVAL);
}

static int
rp1_pinctrl_cfg_pin(struct rp1_pinctrl_softc *sc, int pin, char *fnc,
struct rp1_cfg_param *param)
{
	uint32_t reg;
	int bank, subpin, fncnum;
	int rv;

	rv = rp1_get_pin_bank(sc, pin, &bank, &subpin);
	if (rv != 0) {
		device_printf(sc->dev, "%s: invalid pin %d\n",
		    __func__, pin);
		return (rv);
	}
	rv = rp1_pinctrl_parse_fnc(sc, pin, fnc, &fncnum);
	if (rv != 0) {
		device_printf(sc->dev, "%s: invalid function '%s' for pin %d\n",
		    __func__, fnc, pin);
		return (rv);
	}

	/* Select function  and enable input and outpu from it */
	reg = GPIO_RD4(sc, bank, subpin, RP1_GPIO_CTRL);

	reg &= ~(RP1_CTRL_FNC_MASK << RP1_CTRL_FNC_SHIFT);
	reg |= fncnum << RP1_CTRL_FNC_SHIFT;

	reg &= ~(RP1_CTRL_INOVER_MASK << RP1_CTRL_INOVER_SHIFT);
	reg &= ~(RP1_CTRL_OEOVER_MASK << RP1_CTRL_OEOVER_SHIFT);
	reg &= ~(RP1_CTRL_OUTOVER_MASK << RP1_CTRL_OUTOVER_SHIFT);

	GPIO_WR4(sc, bank, subpin, RP1_GPIO_CTRL, reg);

	/* Output level */
	if (param->output_high || param->output_low) {
		if (param->output_high)
			RIO_WR4(sc, bank, RP1_RIO_OUT_SET, 1 << subpin);
		else
			RIO_WR4(sc, bank, RP1_RIO_OUT_SET, 1 << subpin);
		RIO_WR4(sc, bank, RP1_RIO_OE_SET, 1 << subpin);
	}

	/* Pad parameters*/
	reg = PADS_RD4(sc, bank, subpin, RP1_PADS_CTRL);
	if (param->slew_rate != -1) {
		reg &=  ~RP1_CTRL_SLEWFAST;
		reg |= param->slew_rate != 0 ? RP1_CTRL_SLEWFAST: 0;
	}
	if (param->input_schmitt_enable)
		reg |= RP1_CTRL_SCHMITT;
	if (param->bias_pull_down)
		reg |= RP1_CTRL_PDE;
	if (param->bias_pull_up)
		reg |= RP1_CTRL_PUE;
	if (param->bias_disable)
		reg &= ~(RP1_CTRL_PDE | RP1_CTRL_PUE);
	if (param->input_enable)
		reg |= RP1_CTRL_IE;
	if (param->output_enable)
		reg &= ~RP1_CTRL_OD;
	PADS_WR4(sc, bank, subpin, RP1_PADS_CTRL, reg);

	return(0);
}

static int
rp1_pinctrl_pins(struct rp1_pinctrl_softc *sc, phandle_t node,
const int  *pins, int npins, char *fnc, struct rp1_cfg_param *param)
{
	int i, rv;

	for (i = 0; i < npins; i++) {
		rv = rp1_pinctrl_cfg_pin(sc, pins[i], fnc, param);
		if (rv != 0) {
			device_printf(sc->dev, "%s: cannot configure pin %d\n",
			    __func__, pins[i]);
			return (rv);
		}
	}

	return(0);
}

static const struct rp1_grpdef *
rp1_pinctrl_find_group(struct rp1_pinctrl_softc *sc, const char *group)
{
	int i;

	for (i = 0; i < nitems(rp1_grpdef); i++) {
		if (strcmp(rp1_grpdef[i].name, group) == 0)
			return(rp1_grpdef + i);
	}

	device_printf(sc->dev, "%s: cannot find '%s' group\n", __func__, group);
	return (NULL);
}

static int
rp1_pinctrl_groups(struct rp1_pinctrl_softc *sc, phandle_t node,
const char  **groups, int ngroups, char *fnc, struct rp1_cfg_param *param)
{
	int i;
	const struct rp1_grpdef *def;

	for (i = 0; i < ngroups; i++) {
		def = rp1_pinctrl_find_group(sc, groups[i]);
		if (def != NULL)
			break;
	}
	if (def == NULL)
		return(EINVAL);

	return (rp1_pinctrl_pins(sc, node, def->pins, def->npins, fnc, param));
}

static int
rp1_pinctrl_get_param(struct rp1_pinctrl_softc *sc, phandle_t node,
    struct rp1_cfg_param *param)
{
	int rv;
	int drive;

	param->bias_disable = OF_hasprop(node, "bias-disable");
	param->bias_pull_down = OF_hasprop(node, "bias-pull-down");
	param->bias_pull_up = OF_hasprop(node, "bias-pull-up");
	param->input_enable = OF_hasprop(node, "input-enable");
	param->input_schmitt_enable = OF_hasprop(node, "input-schmitt-enable");
	param->output_enable = OF_hasprop(node, "output_enable");
	param->output_high = OF_hasprop(node, "output_high");
	param->output_low = OF_hasprop(node, "output_low");

	if (OF_hasprop(node, "slew-rate")) {
		rv = OF_getencprop(node, "slew-rate", &param->slew_rate,
		    sizeof(param->slew_rate));
		if (rv < 0) {
			device_printf(sc->dev,
			    "%s: malfgormed property 'slew-rate'\n",
			    __func__);
		}
	} else {
		param->slew_rate = -1;
	}

	if (OF_hasprop(node, "drive-strength")) {
		rv = OF_getencprop(node, "drive-strength", &drive,
		    sizeof(drive));
		if (rv < 0) {
			device_printf(sc->dev,
			    "%s: malfgormed property 'drive-strength'\n",
			    __func__);
			return (EINVAL);
		}

		switch (drive) {
		case  2:
			param->drive_strength = 0;
			break;
		case  4:
			param->drive_strength = 1;
			break;
		case  8:
			param->drive_strength = 2;
			break;
		case 12:
			param->drive_strength = 3;
			break;
		default:
			device_printf(sc->dev,
			    "%s: malfgormed property 'drive-strength' %d\n",
			    __func__, drive);
			return (EINVAL);

		}
	} else {
		param->drive_strength = -1;
	}
	return (0);
}

static int
rp1_pinctrl_configure_pins(device_t dev, phandle_t cfgxref)
{
	struct rp1_pinctrl_softc *sc;
	struct rp1_cfg_param param;
	phandle_t node;
	char  *nm, *fnc;
	uint32_t *pins;
	const char  **groups;
	int rv, npins, ngroups;


	sc = device_get_softc(dev);
	node = OF_node_from_xref(cfgxref);

	nm = NULL;
	fnc = NULL;
	pins = NULL;
	groups = NULL;
	npins = 0;
	ngroups = 0;
	memset(&param, 0, sizeof(param));

	OF_getprop_alloc(node, "name", (void **)&nm);
	OF_getprop_alloc(node, "function", (void **)&fnc);
	if (fnc == NULL) {
		device_printf(sc->dev, "%s, node: %s 'function' is missing\n",
		    __func__, nm);
		rv = EINVAL;
		goto done;
	}

	npins = OF_getencprop_alloc_multi(node, "pins",  sizeof(*pins),
	    (void **)&pins);
	ngroups = ofw_bus_string_list_to_array(node, "groups", &groups);

	if (npins <= 0 && ngroups <= 0) {
		device_printf(sc->dev,
		     "%s, node %s 'pins' or 'groups' is missing\n",
		      __func__, nm);
		rv = EINVAL;
		goto done;
	}

	rv = rp1_pinctrl_get_param(sc, node, &param);
	if  (rv != 0)
		goto done;

	if (ngroups > 0) {
		rv = rp1_pinctrl_groups(sc, node, groups, ngroups, fnc, &param);
		if (rv != 0) {
			rv = EINVAL;
			goto done;
		}
	}

	if (npins > 0) {
		rv = rp1_pinctrl_pins(sc, node, pins, npins, fnc, &param);
		if (rv != 0) {
			rv = EINVAL;
			goto done;
		}
	}

done:
	if (nm != NULL)
		OF_prop_free(nm);
	if (fnc != NULL)
		OF_prop_free(fnc);
	if (pins != NULL)
		OF_prop_free(pins);
	if (groups != NULL)
		OF_prop_free(groups);
	return (rv);
}

/*
 * GPIO Interface
 */
static device_t
rp1_gpio_get_bus(device_t dev)
{
	struct rp1_pinctrl_softc *sc;

	sc = device_get_softc(dev);

	return (sc->gpiobus_dev);
}

static int
rp1_gpio_pin_max(device_t dev, int *maxpin)
{
	struct rp1_pinctrl_softc *sc;

	sc = device_get_softc(dev);

	*maxpin = sc->npins - 1;
	return (0);
}

static int
rp1_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);

	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}

	*caps = sc->pins[pin].gp_caps;
	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}

	*flags = sc->pins[pin].gp_flags;
	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	};

	snprintf(name, GPIOMAXNAME - 1, "%s",
	    sc->pins[pin].gp_name);
	name[GPIOMAXNAME - 1] = '\0';
	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}

	if (value != 0)
		RIO_WR4(sc, bank, RP1_RIO_OUT_SET, 1 << subpin);
	else
		RIO_WR4(sc, bank, RP1_RIO_OUT_CLR, 1 << subpin);
	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;
	uint32_t pads, rio;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}
	pads = PADS_RD4(sc, bank, subpin, RP1_PADS_CTRL);
	rio = RIO_RD4(sc, bank, RP1_RIO_OE);

	if (flags & GPIO_PIN_PULLUP)
		pads |= RP1_CTRL_PUE;
	else
		pads &= ~RP1_CTRL_PUE;

	if (flags & GPIO_PIN_PULLDOWN)
		pads |= RP1_CTRL_PDE;
	else
		pads &= ~RP1_CTRL_PDE;

	if (flags & GPIO_PIN_TRISTATE)
		pads |= RP1_CTRL_OD;
	else
		pads &= ~RP1_CTRL_OD;

	if (flags & GPIO_PIN_INPUT)
		rio &= ~(1 << subpin);
	if (flags & GPIO_PIN_OUTPUT)
		rio |= 1 << subpin;

	PADS_WR4(sc, bank, subpin, RP1_PADS_CTRL, pads);
	RIO_WR4(sc, bank, RP1_RIO_OE, rio);

	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *val)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;
	uint32_t reg;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (!rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}

	reg = 	RIO_RD4(sc, bank, RP1_RIO_SYNC_IN);
	reg >>= subpin;
	*val = reg & 1;
	RP1_PINCTRL_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct rp1_pinctrl_softc *sc;
	int bank, subpin;

	sc = device_get_softc(dev);

	RP1_PINCTRL_LOCK(sc);
	if (rp1_gpio_is_pin_gpio(sc, pin, &bank, &subpin)) {
		RP1_PINCTRL_UNLOCK(sc);
		return (EINVAL);
	}

	RIO_WR4(sc, bank, RP1_RIO_OUT_XOR, 1 << subpin);

	return (0);
}

static int
rp1_gpio_map_gpios(device_t bus, phandle_t dev, phandle_t gparent, int gcells,
    pcell_t *gpios, uint32_t *pin, uint32_t *flags)
{

	/* The gpios are mapped as <pin flags> */
	if (gcells != 2)
		return (EINVAL);

	*pin = gpios[0];
	*flags = gpios[1];
	return (0);
}

/*
 * OFWBUS Interface
 */
static phandle_t
rp1_gpio_get_node(device_t dev, device_t bus)
{

	/* We only have one child, the GPIO bus, which needs our own node. */
	return (ofw_bus_get_node(dev));
}

/*
 * BUS Interface
 */
 static int
rp1_gpio_intr(void *arg)
{
	struct rp1_pinctrl_softc *sc __unused;
	struct rp1_cookie  *cookie;
	int bank __unused;

	cookie = arg;
	sc = cookie->sc;
	bank =  cookie->bank;
	panic("%s: got interrupt\n", __func__);

	return (FILTER_HANDLED);
}


static int
rp1_pinctrl_detach(device_t dev)
{
	struct rp1_pinctrl_softc *sc;
	int i;

	sc = device_get_softc(dev);

#ifdef not_implemented_yet
	rp1_gpio_pic_detach(sc);
#endif

	for (i = 0; i < nitems(sc->irq_res); i++) {
		if (sc->irq_ih[i] != NULL)
			   bus_teardown_intr(dev, sc->irq_res[i],
			       sc->irq_ih[i]);
	}
	if (sc->gpiobus_dev)
		gpiobus_detach_bus(dev);
	bus_release_resources(sc->dev,  rp1_pinctrl_mspec, sc->mem_res);
	bus_release_resources(sc->dev,  rp1_pinctrl_ispec, sc->irq_res);
	mtx_destroy(&sc->mtx);
	return (0);

}

static int
rp1_pinctrl_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "RP1 pinctrl");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_pinctrl_attach(device_t dev)
{
	struct rp1_pinctrl_softc *sc;
	int nnames;
	const char **names;
	int i, rv, bank, subpin, tmp;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);

	mtx_init(&sc->mtx, "rp1 pinctrl", "pinctrl", MTX_SPIN);

	rv = bus_alloc_resources(dev, rp1_pinctrl_mspec, sc->mem_res);
	if (rv != 0) {
		device_printf(dev, "Cannot allocate memory resources: %d\n",
		    rv);
		goto fail;
	}

	rv = bus_alloc_resources(dev, rp1_pinctrl_ispec, sc->irq_res);
	if (rv != 0) {
		device_printf(dev, "Cannot allocate interrupt resources: %d\n",
		    rv);
		goto fail;
	}

	sc->npins = nitems(rp1_fncdef);
	for (i = 0, tmp = 0; i < nitems(rp1_banks); i++)
		tmp += rp1_banks[i].npins;
	if (sc->npins != tmp) {
		device_printf(sc->dev, "Number of pins in pintable(%d) differs "
		    "from bank table (%d)\n", sc->npins, tmp);
		panic("Consistency error");
	}

	/* Configure pins */
	fdt_pinctrl_register(dev, NULL);
	fdt_pinctrl_configure_tree(dev);

	/* Init GPIOs */
	sc->pins = malloc(sc->npins * sizeof (*sc->pins), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	nnames = 0;
	names = NULL;
	if (OF_hasprop(sc->node, "gpio-line-names")) {
		nnames = ofw_bus_string_list_to_array(sc->node,
		    "gpio-line-names", &names);
	}
	for (i = 0; i < sc->npins; i++) {
		rp1_get_pin_bank(sc, i, &bank, &subpin);

		sc->pins[i].gp_pin = i;
		sc->pins[i].gp_caps = RP1_GPIO_CAPS;
		if (i < nnames && names[i] != NULL && names[i][0] != '\0') {
			strncpy(sc->pins[i].gp_name, names[i], GPIOMAXNAME);
			sc->pins[i].gp_name[GPIOMAXNAME - 1] = '\0';
		} else {
			snprintf(sc->pins[i].gp_name, GPIOMAXNAME,
			    "gpio_%d.%d", bank, subpin);
		}
		rp1_gpio_read_flags(sc, bank, subpin, &sc->pins[i].gp_flags);
	}
	if (names != NULL)
		OF_prop_free(names);


	for (i = 0; i < nitems(sc->irq_res); i++) {
		sc->cookies[i].sc = sc;
		sc->cookies[i].bank = i;
		rv = bus_setup_intr(dev, sc->irq_res[i],
		    INTR_TYPE_MISC | INTR_MPSAFE, rp1_gpio_intr, NULL,
		    sc->cookies + i, &sc->irq_ih[i]);
		if (rv != 0) {
			device_printf(sc->dev, "Cannot setup irq\n");
			goto fail;
		}
	}

#ifdef not_implemented_yet
		/* Setup the GPIO interrupt handler. */
		if (rp1_gpio_pic_attach(sc)) {
			device_printf(dev,
			    "unable to setup the gpio irq handler\n");
			goto fail;
		}
#endif

	sc->gpiobus_dev = gpiobus_add_bus(dev);
	if (sc->gpiobus_dev == NULL)
		goto fail;


	bus_attach_children(dev);
	return (0);

fail:
	rp1_pinctrl_detach(dev);
	return (ENXIO);
}

static device_method_t rp1_pinctrl_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			rp1_pinctrl_probe),
	DEVMETHOD(device_attach,		rp1_pinctrl_attach),
	DEVMETHOD(device_detach,		rp1_pinctrl_detach),

	/* Bus interface */
	DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,		bus_generic_teardown_intr),

	/* GPIO protocol */
	DEVMETHOD(gpio_get_bus,			rp1_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,			rp1_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,		rp1_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags,		rp1_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps,		rp1_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags,		rp1_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,			rp1_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,			rp1_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,		rp1_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios,		rp1_gpio_map_gpios),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_node,		rp1_gpio_get_node),
	/* fdt_pinctrl interface */
	DEVMETHOD(fdt_pinctrl_configure,	rp1_pinctrl_configure_pins),

	DEVMETHOD_END
};

DEFINE_CLASS_0(rp1_pinctrl, rp1_pinctrl_driver, rp1_pinctrl_methods,
    sizeof(struct rp1_pinctrl_softc));

EARLY_DRIVER_MODULE(rp1_pinctrl, simplebus, rp1_pinctrl_driver, 0, 0,
    BUS_PASS_INTERRUPT);

extern driver_t ofw_gpiobus_driver;
DRIVER_MODULE(ofw_gpiobus, rp1_pinctrl, ofw_gpiobus_driver, NULL, NULL);
