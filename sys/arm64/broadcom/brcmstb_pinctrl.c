/*
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
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

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/fdt/fdt_pinctrl.h>

#include "fdt_pinctrl_if.h"
#include "gpiobus_if.h"

#if 0
#define dprintf(sc, ...)	device_printf((sc)->dev, ##__VA_ARGS__)
#else
#define dprintf(sc, ...)
#endif

#define	BRCM_PINCTRL_LOCK(_sc)		mtx_lock_spin(&(_sc)->mtx)
#define	BRCM_PINCTRL_UNLOCK(_sc)	mtx_unlock_spin(&(_sc)->mtx)
#define	BRCM_PINCTRL_LOCK_ASSERT(_sc)	mtx_assert(&(_sc)->mtx, MA_OWNED)

#define	RD4(sc, reg)		bus_read_4((sc)->mem_res, reg)
#define	WR4(sc, reg, val)	bus_write_4((sc)->mem_res, reg, val)

#define BPC_NFUNCS		9
#define BPC_FUNC_MASK		0xF
#define BPC_PAD_MASK		0x3
#define BPC_PAD_PULL_DOWN	0x1
#define BPC_PAD_PULL_UP		0x2
#define BPC_FUNC_GPIO		0

struct bpc_func_def {
	const char 	*funcs[BPC_NFUNCS];
};
struct bpc_reg_def {
	char 		*name;
	int		mux;
	u_int		mux_shift;
	int		pad;
	u_int		pad_shift;
};

enum soc_type {
	BCM2712C0,
	BCM2712C0_AON,
	BCM2712D0,
	BCM2712D0_AON,
};

struct bpc_soc {
	const int			type;
	const struct bpc_reg_def	*regs;
	int				nregs;
	const struct bpc_func_def	*funcs;
	int				nfuncs;
	int				range_pins;
};

struct bpc_pin {
	 u_int				num;
	const struct bpc_reg_def	*reg;
	const struct bpc_func_def	*func;
};

struct bpc_cfg_param {
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

struct bpc_softc {
	device_t		dev;
	phandle_t		node;
	struct bpc_soc		*soc;
	struct resource		*mem_res;
	struct mtx		mtx;
	int			npins;
	struct bpc_pin		*pins;

};

/*
 * BC2712 have 2 bans of pins named AON (always on domain) and GPIO
 */

/* name, mux reg, mux shift, pad reg, pad shift */
static const struct bpc_reg_def bcm2712_c0_aon_regs[] = {
 [ 0] = { "aon_gpio0",  12,  0, 24, 20},
 [ 1] = { "aon_gpio1",  12,  4, 24, 22},
 [ 2] = { "aon_gpio2",  12,  8, 24, 24},
 [ 3] = { "aon_gpio3",  12, 12, 24, 26},
 [ 4] = { "aon_gpio4",  12, 16, 24, 28},
 [ 5] = { "aon_gpio5",  12, 20, 28,  0},
 [ 6] = { "aon_gpio6",  12, 24, 28,  2},
 [ 7] = { "aon_gpio7",  12, 28, 28,  4},
 [ 8] = { "aon_gpio8",  16,  0, 28,  6},
 [ 9] = { "aon_gpio9",  16,  4, 28,  8},
 [10] = {"aon_gpio10",  16,  8, 28, 10},
 [11] = {"aon_gpio11",  16, 12, 28, 12},
 [12] = {"aon_gpio12",  16, 16, 28, 14},
 [13] = {"aon_gpio13",  16, 20, 28, 16},
 [14] = {"aon_gpio14",  16, 24, 28, 18},
 [15] = {"aon_gpio15",  16, 28, 28, 20},
 [16] = {"aon_gpio16",  20,  0, 28, 22},
 [32] = {"aon_sgpio0",   0,  0, -1,  0},
 [33] = {"aon_sgpio1",   0,  4, -1,  0},
 [34] = {"aon_sgpio2",   0,  8, -1,  0},
 [35] = {"aon_sgpio3",   0, 12, -1,  0},
 [36] = {"aon_sgpio4",   4,  0, -1,  0},
 [37] = {"aon_sgpio5",   8,  0, -1,  0},
};

static const struct bpc_func_def bcm2712_c0_aon_funcs[] = {
/*           0                   1                     2             3                     4              5            6             7             8*/
 [ 0] = {"gpio",            "ir_in",            "vc_spi0",   "vc_uart3",            "vc_i2c3",         "te0",   "vc_i2c0",        NULL,         NULL},
 [ 1] = {"gpio",          "vc_pwm0",            "vc_spi0",   "vc_uart3",            "vc_i2c3",         "te1",   "aon_pwm",   "vc_i2c0",    "vc_pwm1"},
 [ 2] = {"gpio",          "vc_pwm0",            "vc_spi0",   "vc_uart3",        "ctl_hdmi_5v",         "fl0",   "aon_pwm",     "ir_in",    "vc_pwm1"},
 [ 3] = {"gpio",            "ir_in",            "vc_spi0",   "vc_uart3", "aon_fp_4sec_resetb",         "fl1", "sd_card_g", "aon_gpclk",         NULL},
 [ 4] = {"gpio",           "gpclk0",            "vc_spi0",   "vc_i2csl",          "aon_gpclk",  "pm_led_out",   "aon_pwm", "sd_card_g",    "vc_pwm0"},
 [ 5] = {"gpio",           "gpclk1",              "ir_in",   "vc_i2csl",        "clk_observe",     "aon_pwm", "sd_card_g",   "vc_pwm0",         NULL},
 [ 6] = {"gpio",            "uart1",           "vc_uart4",     "gpclk2",        "ctl_hdmi_5v",    "vc_uart0",   "vc_spi3",        NULL,         NULL},
 [ 7] = {"gpio",            "uart1",           "vc_uart4",     "gpclk0",            "aon_pwm",    "vc_uart0",   "vc_spi3",        NULL,         NULL},
 [ 8] = {"gpio",            "uart1",           "vc_uart4",   "vc_i2csl",        "ctl_hdmi_5v",    "vc_uart0",   "vc_spi3",        NULL,         NULL},
 [ 9] = {"gpio",            "uart1",           "vc_uart4",   "vc_i2csl",            "aon_pwm",    "vc_uart0",   "vc_spi3",        NULL,         NULL},
 [10] = {"gpio",             "tsio",        "ctl_hdmi_5v",        "sc0",          "spdif_out",     "vc_spi5",   "usb_pwr",  "aon_gpclk", "sd_card_f"},
 [11] = {"gpio",             "tsio",              "uart0",        "sc0",        "aud_fs_clk0",     "vc_spi5",  "usb_vbus",   "vc_uart2", "sd_card_f"},
 [12] = {"gpio",             "tsio",              "uart0",   "vc_uart0",               "tsio",     "vc_spi5",   "usb_pwr",   "vc_uart2", "sd_card_f"},
 [13] = {"gpio",           "bsc_m1",              "uart0",   "vc_uart0",                "uui",     "vc_spi5",  "arm_jtag",   "vc_uart2",   "vc_i2c3"},
 [14] = {"gpio",           "bsc_m1",              "uart0",   "vc_uart0",                "uui",     "vc_spi5",  "arm_jtag",   "vc_uart2",   "vc_i2c3"},
 [15] = {"gpio",            "ir_in", "aon_fp_4sec_resetb",   "vc_uart0",         "pm_led_out", "ctl_hdmi_5v",   "aon_pwm",  "aon_gpclk",        NULL},
 [16] = {"gpio", "aon_cpu_standbyb",             "gpclk0", "pm_led_out",        "ctl_hdmi_5v",     "vc_pwm0",   "usb_pwr", "aud_fs_clk0",       NULL},
};

/* name, mux reg, mux shift, pad reg, pad shift */
static const struct bpc_reg_def bcm2712_c0_gpio_regs[] = {
[ 0] = {    "gpio0",  0,  0, 28, 14},
[ 1] = {    "gpio1",  0,  4, 28, 16},
[ 2] = {    "gpio2",  0,  8, 28, 18},
[ 3] = {    "gpio3",  0, 12, 28, 20},
[ 4] = {    "gpio4",  0, 16, 28, 22},
[ 5] = {    "gpio5",  0, 20, 28, 24},
[ 6] = {    "gpio6",  0, 24, 28, 26},
[ 7] = {    "gpio7",  0, 28, 28, 28},
[ 8] = {    "gpio8",  4,  0, 32,  0},
[ 9] = {    "gpio9",  4,  4, 32,  2},
[10] = {   "gpio10",  4,  8, 32,  4},
[11] = {   "gpio11",  4, 12, 32,  6},
[12] = {   "gpio12",  4, 16, 32,  8},
[13] = {   "gpio13",  4, 20, 32, 10},
[14] = {   "gpio14",  4, 24, 32, 12},
[15] = {   "gpio15",  4, 28, 32, 14},
[16] = {   "gpio16",  8,  0, 32, 16},
[17] = {   "gpio17",  8,  4, 32, 18},
[18] = {   "gpio18",  8,  8, 32, 20},
[19] = {   "gpio19",  8, 12, 32, 22},
[20] = {   "gpio20",  8, 16, 32, 24},
[21] = {   "gpio21",  8, 20, 32, 26},
[22] = {   "gpio22",  8, 24, 32, 28},
[23] = {   "gpio23",  8, 28, 36,  0},
[24] = {   "gpio24", 12,  0, 36,  2},
[25] = {   "gpio25", 12,  4, 36,  4},
[26] = {   "gpio26", 12,  8, 36,  6},
[27] = {   "gpio27", 12, 12, 36,  8},
[28] = {   "gpio28", 12, 16, 36, 10},
[29] = {   "gpio29", 12, 20, 36, 12},
[30] = {   "gpio30", 12, 24, 36, 14},
[31] = {   "gpio31", 12, 28, 36, 16},
[32] = {   "gpio32", 16,  0, 36, 18},
[33] = {   "gpio33", 16,  4, 36, 20},
[34] = {   "gpio34", 16,  8, 36, 22},
[35] = {   "gpio35", 16, 12, 36, 24},
[36] = {   "gpio36", 16, 16, 36, 26},
[37] = {   "gpio37", 16, 20, 36, 28},
[38] = {   "gpio38", 16, 24, 40,  0},
[39] = {   "gpio39", 16, 28, 40,  2},
[40] = {   "gpio40", 20,  0, 40,  4},
[41] = {   "gpio41", 20,  4, 40,  6},
[42] = {   "gpio42", 20,  8, 40,  8},
[43] = {   "gpio43", 20, 12, 40, 10},
[44] = {   "gpio44", 20, 16, 40, 12},
[45] = {   "gpio45", 20, 20, 40, 14},
[46] = {   "gpio46", 20, 24, 40, 16},
[47] = {   "gpio47", 20, 28, 40, 18},
[48] = {   "gpio48", 24,  0, 40, 20},
[49] = {   "gpio49", 24,  4, 40, 22},
[50] = {   "gpio50", 24,  8, 40, 24},
[51] = {   "gpio51", 24, 12, 40, 26},
[52] = {   "gpio52", 24, 16, 40, 28},
[53] = {   "gpio53", 24, 20, 44,  0},
[54] = { "emmc_cmd", -1,  0, 44,  2},
[55] = {  "emmc_ds", -1,  0, 44,  4},
[56] = { "emmc_clk", -1,  0, 44,  6},
[57] = {"emmc_dat0", -1,  0, 44,  8},
[58] = {"emmc_dat1", -1,  0, 44, 10},
[59] = {"emmc_dat2", -1,  0, 44, 12},
[60] = {"emmc_dat3", -1,  0, 44, 14},
[61] = {"emmc_dat4", -1,  0, 44, 16},
[62] = {"emmc_dat5", -1,  0, 44, 18},
[63] = {"emmc_dat6", -1,  0, 44, 20},
[64] = {"emmc_dat7", -1,  0, 44, 22},
};

static const struct bpc_func_def bcm2712_c0_gpio_funcs[] = {
/*           0            1               2             3               4              5               6              7             8 */
 [ 0] = {"gpio",    "bsc_m3",      "vc_i2c0",     "gpclk0",        "enet0",     "vc_pwm1",      "vc_spi0",       "ir_in",        NULL},
 [ 1] = {"gpio",    "bsc_m3",     "vc_i2c0",      "gpclk1",        "enet0",     "vc_pwm1", "sr_edm_sense",     "vc_spi0",  "vc_uart3"},
 [ 2] = {"gpio",       "pdm",      "i2s_in",      "gpclk2",      "vc_spi4",         "pkt",      "vc_spi0",    "vc_uart3",        NULL},
 [ 3] = {"gpio",       "pdm",      "i2s_in",     "vc_spi4",          "pkt",     "vc_spi0",     "vc_uart3",          NULL,        NULL},
 [ 4] = {"gpio",       "pdm",      "i2s_in",    "arm_jtag",      "vc_spi4",         "pkt",      "vc_spi0",    "vc_uart3",        NULL},
 [ 5] = {"gpio",       "pdm",     "vc_i2c3",    "arm_jtag",    "sd_card_e",     "vc_spi4",          "pkt",      "vc_pcm",   "vc_i2c5"},
 [ 6] = {"gpio",       "pdm",     "vc_i2c3",    "arm_jtag",    "sd_card_e",     "vc_spi4",          "pkt",      "vc_pcm",   "vc_i2c5"},
 [ 7] = {"gpio",   "i2s_out",   "spdif_out",    "arm_jtag",    "sd_card_e",     "vc_i2c3",  "enet0_rgmii",      "vc_pcm",   "vc_spi4"},
 [ 8] = {"gpio",   "i2s_out", "aud_fs_clk0",    "arm_jtag",    "sd_card_e",     "vc_i2c3",    "enet0_mii",      "vc_pcm",   "vc_spi4"},
 [ 9] = {"gpio",   "i2s_out", "aud_fs_clk0",    "arm_jtag",    "sd_card_e",   "enet0_mii",    "sd_card_c",     "vc_spi4",        NULL},
 [10] = {"gpio",    "bsc_m3",  "mtsif_alt1",      "i2s_in",      "i2s_out",     "vc_spi5",    "enet0_mii",   "sd_card_c",   "vc_spi4"},
 [11] = {"gpio",    "bsc_m3",  "mtsif_alt1",      "i2s_in",      "i2s_out",     "vc_spi5",    "enet0_mii",   "sd_card_c",   "vc_spi4"},
 [12] = {"gpio",     "spi_s",  "mtsif_alt1",      "i2s_in",      "i2s_out",     "vc_spi5",     "vc_i2csl",         "sd0", "sd_card_d"},
 [13] = {"gpio",     "spi_s",  "mtsif_alt1",     "i2s_out",     "usb_vbus",     "vc_spi5",     "vc_i2csl",         "sd0", "sd_card_d"},
 [14] = {"gpio",     "spi_s",    "vc_i2csl", "enet0_rgmii",     "arm_jtag",     "vc_spi5",      "vc_pwm0",     "vc_i2c4", "sd_card_d"},
 [15] = {"gpio",     "spi_s",    "vc_i2csl",     "vc_spi3",     "arm_jtag",     "vc_pwm0",      "vc_i2c4",      "gpclk0",        NULL},
 [16] = {"gpio", "sd_card_b",     "i2s_out",     "vc_spi3",       "i2s_in",         "sd0",  "enet0_rgmii",      "gpclk1",        NULL},
 [17] = {"gpio", "sd_card_b",     "i2s_out",     "vc_spi3",       "i2s_in",  "ext_sc_clk",          "sd0", "enet0_rgmii",    "gpclk2"},
 [18] = {"gpio", "sd_card_b",     "i2s_out",     "vc_spi3",       "i2s_in",         "sd0",  "enet0_rgmii",     "vc_pwm1",        NULL},
 [19] = {"gpio", "sd_card_b",     "usb_pwr",     "vc_spi3",          "pkt",   "spdif_out",          "sd0",       "ir_in",   "vc_pwm1"},
 [20] = {"gpio", "sd_card_b",         "uui",    "vc_uart0",     "arm_jtag",       "uart2",      "usb_pwr",      "vc_pcm",  "vc_uart4"},
 [21] = {"gpio",   "usb_pwr",         "uui",    "vc_uart0",     "arm_jtag",       "uart2",    "sd_card_b",      "vc_pcm",  "vc_uart4"},
 [22] = {"gpio",   "usb_pwr",       "enet0",    "vc_uart0",        "mtsif",       "uart2",     "usb_vbus",      "vc_pcm",   "vc_i2c5"},
 [23] = {"gpio",  "usb_vbus",       "enet0",    "vc_uart0",        "mtsif",       "uart2",      "i2s_out",      "vc_pcm",   "vc_i2c5"},
 [24] = {"gpio",     "mtsif",         "pkt",       "uart0",  "enet0_rgmii", "enet0_rgmii",      "vc_i2c4",    "vc_uart3",        NULL},
 [25] = {"gpio",     "mtsif",         "pkt",         "sc0",        "uart0", "enet0_rgmii",  "enet0_rgmii",     "vc_i2c4",  "vc_uart3"},
 [26] = {"gpio",     "mtsif",         "pkt",         "sc0",        "uart0", "enet0_rgmii",     "vc_uart4",     "vc_spi5",        NULL},
 [27] = {"gpio",     "mtsif",         "pkt",         "sc0",        "uart0", "enet0_rgmii",     "vc_uart4",     "vc_spi5",        NULL},
 [28] = {"gpio",     "mtsif",         "pkt",         "sc0",  "enet0_rgmii",    "vc_uart4",      "vc_spi5",          NULL,        NULL},
 [29] = {"gpio",     "mtsif",         "pkt",         "sc0",  "enet0_rgmii",    "vc_uart4",      "vc_spi5",          NULL,        NULL},
 [30] = {"gpio",     "mtsif",         "pkt",         "sc0",          "sd2", "enet0_rgmii",       "gpclk0",     "vc_pwm0",        NULL},
 [31] = {"gpio",     "mtsif",         "pkt",         "sc0",          "sd2", "enet0_rgmii",      "vc_spi3",     "vc_pwm0",        NULL},
 [32] = {"gpio",     "mtsif",         "pkt",         "sc0",          "sd2", "enet0_rgmii",      "vc_spi3",    "vc_uart3",        NULL},
 [33] = {"gpio",     "mtsif",         "pkt",         "sd2",  "enet0_rgmii",     "vc_spi3",     "vc_uart3",          NULL,        NULL},
 [34] = {"gpio",     "mtsif",         "pkt",  "ext_sc_clk",          "sd2", "enet0_rgmii",      "vc_spi3",     "vc_i2c5",        NULL},
 [35] = {"gpio",     "mtsif",         "pkt",         "sd2",  "enet0_rgmii",     "vc_spi3",      "vc_i2c5",          NULL,        NULL},
 [36] = {"gpio",       "sd0",       "mtsif",         "sc0",       "i2s_in",    "vc_uart3",     "vc_uart2",          NULL,        NULL},
 [37] = {"gpio",       "sd0",       "mtsif",         "sc0",      "vc_spi0",      "i2s_in",     "vc_uart3",    "vc_uart2",        NULL},
 [38] = {"gpio",       "sd0",   "mtsif_alt",         "sc0",      "vc_spi0",      "i2s_in",     "vc_uart3",    "vc_uart2",        NULL},
 [39] = {"gpio",       "sd0",   "mtsif_alt",         "sc0",      "vc_spi0",    "vc_uart3",     "vc_uart2",          NULL,        NULL},
 [40] = {"gpio",       "sd0",   "mtsif_alt",         "sc0",      "vc_spi0",      "bsc_m3",           NULL,          NULL,        NULL},
 [41] = {"gpio",       "sd0",   "mtsif_alt",         "sc0",      "vc_spi0",      "bsc_m3",           NULL,          NULL,        NULL},
 [42] = {"gpio",   "vc_spi0",   "mtsif_alt",     "vc_i2c0",    "sd_card_a",  "mtsif_alt1",     "arm_jtag",         "pdm",     "spi_m"},
 [43] = {"gpio",   "vc_spi0",   "mtsif_alt",     "vc_i2c0",    "sd_card_a",  "mtsif_alt1",     "arm_jtag",         "pdm",     "spi_m"},
 [44] = {"gpio",   "vc_spi0",   "mtsif_alt",       "enet0",    "sd_card_a",  "mtsif_alt1",     "arm_jtag",         "pdm",     "spi_m"},
 [45] = {"gpio",   "vc_spi0",   "mtsif_alt",       "enet0",    "sd_card_a",  "mtsif_alt1",     "arm_jtag",         "pdm",     "spi_m"},
 [46] = {"gpio",   "vc_spi0",   "mtsif_alt",   "sd_card_a",   "mtsif_alt1",    "arm_jtag",          "pdm",       "spi_m",        NULL},
 [47] = {"gpio",     "enet0",   "mtsif_alt",     "i2s_out",   "mtsif_alt1",    "arm_jtag",           NULL,          NULL,        NULL},
 [48] = {"gpio",      "sc0",      "usb_pwr",   "spdif_out",        "mtsif",          NULL,           NULL,          NULL,        NULL},
 [49] = {"gpio",      "sc0",      "usb_pwr", "aud_fs_clk0",        "mtsif",          NULL,           NULL,          NULL,        NULL},
 [50] = {"gpio",      "sc0",     "usb_vbus",         "sc0",           NULL,          NULL,           NULL,          NULL,        NULL},
 [51] = {"gpio",      "sc0",        "enet0",         "sc0", "sr_edm_sense",          NULL,           NULL,          NULL,        NULL},
 [52] = {"gpio",      "sc0",        "enet0",     "vc_pwm1",           NULL,          NULL,           NULL,          NULL,        NULL},
 [53] = {"gpio",      "sc0",  "enet0_rgmii",  "ext_sc_clk",           NULL,          NULL,           NULL,          NULL,        NULL},
};


/* name, mux reg, mux shift, pad reg, pad shift */
static struct bpc_reg_def bcm2712_d0_aon_regs[] = {
 [ 0] = { "aon_gpio0",  12,  0, 20, 18},
 [ 1] = { "aon_gpio1",  12,  4, 20, 20},
 [ 2] = { "aon_gpio2",  12,  8, 20, 22},
 [ 3] = { "aon_gpio3",  12, 12, 20, 24},
 [ 4] = { "aon_gpio4",  12, 16, 20, 26},
 [ 5] = { "aon_gpio5",  12, 20, 20, 28},
 [ 6] = { "aon_gpio6",  12, 24, 24,  0},
 [ 8] = { "aon_gpio8",  12, 28, 24,  2},
 [ 9] = { "aon_gpio9",  16,  0, 24,  4},
 [12] = {"aon_gpio12",  16,  4, 24,  6},
 [13] = {"aon_gpio13",  16,  8, 24,  8},
 [14] = {"aon_gpio14",  16, 12, 24, 10},
 [32] = {"aon_sgpio0",   0,  0, -4,  0},
 [33] = {"aon_sgpio1",   0,  4, -4,  0},
 [34] = {"aon_sgpio2",   0,  8, -4,  0},
 [35] = {"aon_sgpio3",   0, 12, -4,  0},
 [36] = {"aon_sgpio4",   4,  0, -4,  0},
 [37] = {"aon_sgpio5",   8,  0, -4,  0},
};

static const struct bpc_func_def bcm2712_d0_aon_funcs[] = {
/*           0          1            2              3              4            5            6            7          8 */
 [ 0] = {"gpio",   "ir_in",   "vc_spi0",    "vc_uart0",     "vc_i2c3",     "uart0",   "vc_i2c0",       NULL,      NULL},
 [ 1] = {"gpio", "vc_pwm0",   "vc_spi0",    "vc_uart0",     "vc_i2c3",     "uart0",   "aon_pwm",  "vc_i2c0", "vc_pwm1"},
 [ 2] = {"gpio", "vc_pwm0",   "vc_spi0",    "vc_uart0", "ctl_hdmi_5v",     "uart0",   "aon_pwm",    "ir_in", "vc_pwm1"},
 [ 3] = {"gpio",   "ir_in",   "vc_spi0",    "vc_uart0",       "uart0", "sd_card_g", "aon_gpclk",       NULL,      NULL},
 [ 4] = {"gpio",  "gpclk0",   "vc_spi0",  "pm_led_out",     "aon_pwm", "sd_card_g",   "vc_pwm0",       NULL,      NULL},
 [ 5] = {"gpio",  "gpclk1",     "ir_in",     "aon_pwm",   "sd_card_g",   "vc_pwm0",        NULL,       NULL,      NULL},
 [ 6] = {"gpio",   "uart1",  "vc_uart2", "ctl_hdmi_5v",      "gpclk2",   "vc_spi3",        NULL,       NULL,      NULL},
 [ 8] = {"gpio",   "uart1",  "vc_uart2", "ctl_hdmi_5v",     "vc_spi0",   "vc_spi3",        NULL,       NULL,      NULL},
 [ 9] = {"gpio",   "uart1",  "vc_uart2",    "vc_uart0",     "aon_pwm",   "vc_spi0",  "vc_uart2",  "vc_spi3",      NULL},
 [12] = {"gpio",   "uart1",  "vc_uart2",    "vc_uart0",     "vc_spi0",   "usb_pwr",  "vc_uart2",  "vc_spi3",      NULL},
 [13] = {"gpio",  "bsc_m1",  "vc_uart0",         "uui",     "vc_spi0",  "arm_jtag",  "vc_uart2",  "vc_i2c3",      NULL},
 [14] = {"gpio",  "bsc_m1", "aon_gpclk",    "vc_uart0",         "uui",   "vc_spi0",  "arm_jtag", "vc_uart2", "vc_i2c3"},
};

/* name, mux reg, mux shift, pad reg, pad shift */
static const struct bpc_reg_def bcm2712_d0_gpio_regs[] = {
 [ 1] = {   "gpio1",   0,  0, 16, 10},
 [ 2] = {   "gpio2",   0,  4, 16, 12},
 [ 3] = {   "gpio3",   0,  8, 16, 14},
 [ 4] = {   "gpio4",   0, 12, 16, 16},
 [10] = {   "gpio10",  0, 16, 16, 18},
 [11] = {   "gpio11",  0, 20, 16, 20},
 [12] = {   "gpio12",  0, 24, 16, 22},
 [13] = {   "gpio13",  0, 28, 16, 24},
 [14] = {   "gpio14",  4,  0, 16, 26},
 [15] = {   "gpio15",  4,  4, 16, 28},
 [18] = {   "gpio18",  4,  8, 20,  0},
 [19] = {   "gpio19",  4, 12, 20,  2},
 [20] = {   "gpio20",  4, 16, 20,  4},
 [21] = {   "gpio21",  4, 20, 20,  6},
 [22] = {   "gpio22",  4, 24, 20,  8},
 [23] = {   "gpio23",  4, 28, 20, 10},
 [24] = {   "gpio24",  8,  0, 20, 12},
 [25] = {   "gpio25",  8,  4, 20, 14},
 [26] = {   "gpio26",  8,  8, 20, 16},
 [27] = {   "gpio27",  8, 12, 20, 18},
 [28] = {   "gpio28",  8, 16, 20, 20},
 [29] = {   "gpio29",  8, 20, 20, 22},
 [30] = {   "gpio30",  8, 24, 20, 24},
 [31] = {   "gpio31",  8, 28, 20, 26},
 [32] = {   "gpio32", 12,  0, 20, 28},
 [33] = {   "gpio33", 12,  4, 24,  0},
 [34] = {   "gpio34", 12,  8, 24,  2},
 [35] = {   "gpio35", 12, 12, 24,  4},
 [36] = { "emmc_cmd", -1,  0, 24,  6},
 [37] = {  "emmc_ds", -1,  0, 24,  8},
 [38] = { "emmc_clk", -1,  0, 24, 10},
 [39] = {"emmc_dat0", -1,  0, 24, 12},
 [40] = {"emmc_dat1", -1,  0, 24, 14},
 [41] = {"emmc_dat2", -1,  0, 24, 16},
 [42] = {"emmc_dat3", -1,  0, 24, 18},
 [43] = {"emmc_dat4", -1,  0, 24, 20},
 [44] = {"emmc_dat5", -1,  0, 24, 22},
 [45] = {"emmc_dat6", -1,  0, 24, 24},
 [46] = {"emmc_dat7", -1,  0, 24, 26},
};


static const struct bpc_func_def bcm2712_d0_gpio_funcs[] = {
/*           0            1           2              3            4           5               6           7           8  */
 [ 1] = {"gpio",   "vc_i2c0",  "usb_pwr",      "gpclk0", "sd_card_e",  "vc_spi3", "sr_edm_sense",  "vc_spi0", "vc_uart0"},
 [ 2] = {"gpio",   "vc_i2c0",  "usb_pwr",      "gpclk1", "sd_card_e",  "vc_spi3",  "clk_observe",  "vc_spi0", "vc_uart0"},
 [ 3] = {"gpio",   "vc_i2c3", "usb_vbus",      "gpclk2", "sd_card_e",  "vc_spi3",      "vc_spi0", "vc_uart0",       NULL},
 [ 4] = {"gpio",   "vc_i2c3",  "vc_pwm1", "    vc_spi3", "sd_card_e",  "vc_spi3",      "vc_spi0", "vc_uart0",       NULL},
 [10] = {"gpio",    "bsc_m3",  "vc_pwm1", "    vc_spi3", "sd_card_e",  "vc_spi3",       "gpclk0",       NULL,       NULL},
 [11] = {"gpio",    "bsc_m3",  "vc_spi3", "clk_observe", "sd_card_c",   "gpclk1",           NULL,       NULL,       NULL},
 [12] = {"gpio",     "spi_s",  "vc_spi3",   "sd_card_c", "sd_card_d",       NULL,           NULL,       NULL,       NULL},
 [13] = {"gpio",     "spi_s",  "vc_spi3",   "sd_card_c", "sd_card_d",       NULL,           NULL,       NULL,       NULL},
 [14] = {"gpio",     "spi_s",      "uui",    "arm_jtag",   "vc_pwm0",  "vc_i2c0",    "sd_card_d",       NULL,       NULL},
 [15] = {"gpio",     "spi_s",      "uui",    "arm_jtag",   "vc_pwm0",  "vc_i2c0",       "gpclk0",       NULL,       NULL},
 [18] = {"gpio", "sd_card_f",  "vc_pwm1",          NULL,        NULL,       NULL,           NULL,       NULL,       NULL},
 [19] = {"gpio", "sd_card_f",  "usb_pwr",     "vc_pwm1",        NULL,       NULL,           NULL,       NULL,       NULL},
 [20] = {"gpio",   "vc_i2c3",      "uui",    "vc_uart0",  "arm_jtag", "vc_uart2",           NULL,       NULL,       NULL},
 [21] = {"gpio",   "vc_i2c3",      "uui",    "vc_uart0",  "arm_jtag", "vc_uart2",           NULL,       NULL,       NULL},
 [22] = {"gpio", "sd_card_f", "vc_uart0",     "vc_i2c3",        NULL,       NULL,           NULL,       NULL,       NULL},
 [23] = {"gpio",  "vc_uart0",  "vc_i2c3",          NULL,        NULL,       NULL,           NULL,       NULL,       NULL},
 [24] = {"gpio", "sd_card_b",  "vc_spi0",    "arm_jtag",     "uart0",  "usb_pwr",     "vc_uart2", "vc_uart0",       NULL},
 [25] = {"gpio", "sd_card_b",  "vc_spi0",    "arm_jtag",     "uart0",  "usb_pwr",     "vc_uart2", "vc_uart0",       NULL},
 [26] = {"gpio", "sd_card_b",  "vc_spi0",    "arm_jtag",     "uart0", "usb_vbus",     "vc_uart2",  "vc_spi0",       NULL},
 [27] = {"gpio", "sd_card_b",  "vc_spi0",    "arm_jtag",     "uart0", "vc_uart2",      "vc_spi0",       NULL,       NULL},
 [28] = {"gpio", "sd_card_b",  "vc_spi0",    "arm_jtag",   "vc_i2c0",  "vc_spi0",           NULL,       NULL,       NULL},
 [29] = {"gpio",  "arm_jtag",  "vc_i2c0",     "vc_spi0",        NULL,       NULL,           NULL,       NULL,       NULL},
 [30] = {"gpio",       "sd2",   "gpclk0",     "vc_pwm0",        NULL,       NULL,           NULL,       NULL,       NULL},
 [31] = {"gpio",       "sd2",  "vc_spi3",     "vc_pwm0",        NULL,       NULL,           NULL,       NULL,       NULL},
 [32] = {"gpio",       "sd2",  "vc_spi3",    "vc_uart3",        NULL,       NULL,           NULL,       NULL,       NULL},
 [33] = {"gpio",       "sd2",  "vc_spi3",    "vc_uart3",        NULL,       NULL,           NULL,       NULL,       NULL},
 [34] = {"gpio",       "sd2",  "vc_spi3",     "vc_i2c5",        NULL,       NULL,           NULL,       NULL,       NULL},
 [35] = {"gpio",       "sd2",  "vc_spi3",     "vc_i2c5",        NULL,       NULL,           NULL,       NULL,       NULL},
};


static struct bpc_soc soc_bcm2712c0 = {
	.type = BCM2712C0,
	.regs = bcm2712_c0_gpio_regs,
	.nregs = nitems(bcm2712_c0_gpio_regs),
	.funcs = bcm2712_c0_gpio_funcs,
	.nfuncs = nitems(bcm2712_c0_gpio_funcs),
	.range_pins = 54,
};

static struct bpc_soc soc_bcm2712c0_aon = {
	.type = BCM2712C0_AON,
	.regs = bcm2712_c0_aon_regs,
	.nregs = nitems(bcm2712_c0_aon_regs),
	.funcs = bcm2712_c0_aon_funcs,
	.nfuncs = nitems(bcm2712_c0_aon_funcs),
	.range_pins = 23,
};

static struct bpc_soc soc_bcm2712d0 = {
	.type = BCM2712D0,
	.regs = bcm2712_d0_gpio_regs,
	.nregs = nitems(bcm2712_d0_gpio_regs),
	.funcs = bcm2712_d0_gpio_funcs,
	.nfuncs = nitems(bcm2712_d0_gpio_funcs),
	.range_pins = 54,
};

static struct bpc_soc soc_bcm2712d0_aon = {
	.type = BCM2712D0_AON,
	.regs = bcm2712_d0_aon_regs,
	.nregs = nitems(bcm2712_d0_aon_regs),
	.funcs = bcm2712_d0_aon_funcs,
	.nfuncs = nitems(bcm2712_d0_aon_funcs),
	.range_pins = 21,
};


/* Compatible devices. */
static struct ofw_compat_data compat_data[] = {
	{ "brcm,bcm2712c0-pinctrl",		(uintptr_t)&soc_bcm2712c0},
	{ "brcm,bcm2712c0-aon-pinctrl",		(uintptr_t)&soc_bcm2712c0_aon},
	{ "brcm,bcm2712d0-pinctrl",		(uintptr_t)&soc_bcm2712d0},
	{ "brcm,bcm2712d0-aon-pinctrl",		(uintptr_t)&soc_bcm2712d0_aon},
	{ NULL,					0},
};


/*
 * PINCTRL Interface
 */
 static int
brcm_find_gpio_pin(struct bpc_softc *sc, u_int pinnum, struct bpc_pin **pindef)
{
	/* Find pindef */
	*pindef = NULL;
	if (pinnum >=sc->npins)
		return (EINVAL);

	if (sc->pins[pinnum].reg == NULL)
		return (EINVAL);

	if (sc->pins[pinnum].num != pinnum) {
		panic("%s: pin mnumber misch-masch %d <-> %d", __func__,
		    sc->pins[pinnum].num, pinnum);
	}

	*pindef = sc->pins + pinnum;
	return (0);
}

static int
brcm_pinctrl_is_gpio_locked(struct bpc_softc *sc, struct bpc_pin *pindef,
    bool *is_gpio)
{
	uint32_t reg;

	BRCM_PINCTRL_LOCK_ASSERT(sc);

	if (pindef->reg->mux == -1) {
		*is_gpio = false;
		return (0);
	}

	reg = RD4(sc, pindef->reg->mux);
	reg >>= pindef->reg->mux_shift;
	reg &= BPC_FUNC_MASK;
	*is_gpio = reg == BPC_FUNC_GPIO;

	return (0);
}

static int
brcm_pinctrl_is_gpio(device_t pinctrl, device_t gpio, uint32_t pin,
    bool *is_gpio)
{
	struct bpc_softc *sc;
	struct bpc_pin *pindef;
	int rv;

	sc = device_get_softc(pinctrl);
	rv = brcm_find_gpio_pin(sc, pin, &pindef);
	if (rv != 0)
		return (rv);

	BRCM_PINCTRL_LOCK(sc);

	rv = brcm_pinctrl_is_gpio_locked(sc, pindef, is_gpio);

	BRCM_PINCTRL_UNLOCK(sc);

	return (rv);
}

static int
brcm_pinctrl_get_flags(device_t pinctrl, device_t gpio, uint32_t pin,
    uint32_t *flags)
{
	struct bpc_softc *sc;
	struct bpc_pin *pindef;
	bool is_gpio;
	uint32_t reg;
	int rv;

	sc = device_get_softc(pinctrl);

	rv = brcm_find_gpio_pin(sc, pin, &pindef);
	if (rv != 0)
		return (rv);

	BRCM_PINCTRL_LOCK(sc);

	rv = brcm_pinctrl_is_gpio_locked(sc, pindef, &is_gpio);
	if (rv != 0 || !is_gpio)
		goto done;

	if (!is_gpio) {
		rv = EINVAL;
		goto done;
	}

	*flags = 0;

	/* GPIO without PAD register */
	if (pindef->reg->pad == -1) {
		*flags = 0;
		return (0);
	}

	reg = RD4(sc, pindef->reg->pad);
	reg >>= pindef->reg->pad_shift;
	reg &= BPC_PAD_MASK;
	if (reg & BPC_PAD_PULL_DOWN)
		*flags |= GPIO_PIN_PULLDOWN;
	if (reg & BPC_PAD_PULL_UP)
		*flags |= GPIO_PIN_PULLUP;

done:
	BRCM_PINCTRL_UNLOCK(sc);
	return (rv);
}

static int
brcm_pinctrl_set_flags(device_t pinctrl, device_t gpio, uint32_t pin,
    uint32_t flags)
{
	struct bpc_softc *sc;
	struct bpc_pin *pindef;
	bool is_gpio;
	uint32_t reg;
	int rv;

	sc = device_get_softc(pinctrl);

	rv = brcm_find_gpio_pin(sc, pin, &pindef);
	if (rv != 0)
		return (rv);

	BRCM_PINCTRL_LOCK(sc);

	rv = brcm_pinctrl_is_gpio_locked(sc, pindef, &is_gpio);
	if (rv != 0 || !is_gpio)
		goto done;

	/* GPIO without PAD register */
	if (pindef->reg->pad == -1 && flags == 0)
		return (0);
	if (pindef->reg->pad == -1)
		return (EINVAL);

	reg = RD4(sc, pindef->reg->pad);
	reg &= ~(BPC_PAD_MASK << pindef->reg->pad_shift);
	if (flags & GPIO_PIN_PULLDOWN)
		reg |=  BPC_PAD_PULL_DOWN << pindef->reg->pad_shift;
	if (flags & GPIO_PIN_PULLUP)
		reg |=  BPC_PAD_PULL_UP << pindef->reg->pad_shift;
	WR4(sc, pindef->reg->pad, reg);

done:
	BRCM_PINCTRL_UNLOCK(sc);
	return (rv);
}

static int
brcm_pinctrl_get_param(struct bpc_softc *sc, phandle_t node,
    struct bpc_cfg_param *param)
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
brsm_find_pindef(struct bpc_softc *sc, const char *pin, char *func,
    struct bpc_pin **pindef, int *funcnum)
{
	int i;
	const char * const *functbl;

	/* Find pindef */
	*pindef = NULL;
	for (i = 0; i < sc->npins; i++) {
		if (sc->pins[i].reg == NULL)
			continue;
		if (strcmp(sc->pins[i].reg->name, pin) == 0) {
			*pindef = sc->pins + i;
			break;
		}
	}
	if (*pindef == NULL)
		return (EINVAL);
	functbl = (*pindef)->func->funcs;

	if (functbl == NULL && func ==NULL)
		return (0);
	if (functbl == NULL)
		return (EINVAL);

	/* Find function name */
	for (i = 0; i < BPC_NFUNCS; i++) {
		if (strcmp (functbl[i], func) == 0) {
			*funcnum = i;
			return (0);
		}
	}
	return (EINVAL);
}

static int
brcm_pinctrl_cfg_pin(struct bpc_softc *sc, char *name, const char* pin,
    char *func, struct bpc_cfg_param *param)
{
	uint32_t reg;
	struct bpc_pin *pindef;
	int funcnum;
	int rv;

	rv = brsm_find_pindef(sc, pin, func, &pindef, &funcnum);
	if (rv != 0) {
		device_printf(sc->dev, "%s: invalid pin '%s' or function '%s'\n",
		    __func__, pin, func);
		return (rv);
	}
	dprintf(sc,
	    "%s: parsed pin: '%s', pinnum: %d, func: '%s', funcnum:%d\n",
	     __func__, pin, pindef->num, func, funcnum);

	if (func != NULL && pindef->reg->mux == -1){
		device_printf(sc->dev,
		    "%s: pin '%s' have function '%s' but mux register "
		    "doesn't exist\n",
		    __func__, pin, func);
		return (EINVAL);
	}

	/* Set function(mux) if exist */
	if (func != NULL) {
		reg = RD4(sc, pindef->reg->mux);
		reg &= ~(BPC_FUNC_MASK << pindef->reg->mux_shift);
		reg |= funcnum << pindef->reg->mux_shift;
		WR4(sc, pindef->reg->mux, reg);
	}
	/* Set pad if exist */
	if (pindef->reg->pad != -1) {
		reg = RD4(sc, pindef->reg->pad);
		reg &= ~(BPC_PAD_MASK << pindef->reg->pad_shift);
		if (param->bias_pull_down)
			reg |=  BPC_PAD_PULL_DOWN << pindef->reg->pad_shift;
		if (param->bias_pull_up)
			reg |=  BPC_PAD_PULL_UP << pindef->reg->pad_shift;
		WR4(sc, pindef->reg->pad, reg);
	}

	return(0);
}

static int
brcm_pinctrl_pins(struct bpc_softc *sc, phandle_t node, char *name,
    const char *pins[], int npins, char *func, struct bpc_cfg_param *param)
{
	int i, rv;

	for (i = 0; i < npins; i++) {
		rv = brcm_pinctrl_cfg_pin(sc, name, pins[i], func, param);
		if (rv != 0) {
			device_printf(sc->dev,
			     "%s: %s cannot configure pin %s\n", __func__,
			     name, pins[i]);
			return (rv);
		}
	}

	return(0);
}

static int
brcm_pinctrl_configure_pins(struct bpc_softc *sc, phandle_t node)
{
	struct bpc_cfg_param param;
	char  *name, *func;
	const char **pins;
	int rv, npins;

	name = NULL;
	func = NULL;
	pins = NULL;
	npins = 0;
	memset(&param, 0, sizeof(param));

	OF_getprop_alloc(node, "name", (void **)&name);
	dprintf(sc, "%s: Processing node: %s\n", __func__, name);
	OF_getprop_alloc(node, "function", (void **)&func);

	npins = ofw_bus_string_list_to_array(node, "pins", &pins);
	if (npins <= 0 ) {
		device_printf(sc->dev,
		     "%s, node %s 'pins' property is missing\n",
		      __func__, name);
		rv = EINVAL;
		goto done;
	}

	rv = brcm_pinctrl_get_param(sc, node, &param);
	if  (rv != 0)
		goto done;

	rv = brcm_pinctrl_pins(sc, node, name, pins, npins, func, &param);
	if (rv != 0) {
		rv = EINVAL;
		goto done;
	}

done:
	if (name != NULL)
		OF_prop_free(name);
	if (func != NULL)
		OF_prop_free(func);
	if (pins != NULL)
		OF_prop_free(pins);
	return (rv);
}

static int
brcm_pinctrl_configure_node(device_t dev, phandle_t cfgxref)
{
	struct bpc_softc *sc;
	phandle_t node, child;
	int	rv;

	sc = device_get_softc(dev);
	node = OF_node_from_xref(cfgxref);

	for (child = OF_child(node); child > 0; child = OF_peer(child)) {
		rv = brcm_pinctrl_configure_pins(sc, child);
		if (rv != 0)
			return (rv);
	}

	if (OF_hasprop(node, "pins")) {
		rv = brcm_pinctrl_configure_pins(sc, node);
		if (rv != 0)
			return (rv);
	}

	return (0);
}

/*
 * OFW Interface
 */
static phandle_t
brcm_pinctr_get_node(device_t dev, device_t child)
{
	struct bpc_softc *sc;

	sc = device_get_softc(dev);
	return (sc->node);
}


/*
 * DEVICE Interface
 */
static int
brcm_pinctrl_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Broadcom STB Pinctrl controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_pinctrl_attach(device_t dev)
{
	struct bpc_softc *sc;
	int i, rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);

	mtx_init(&sc->mtx, "brcmstb pinctrl", "pinctrl", MTX_SPIN);

	sc->soc = (struct bpc_soc *)ofw_bus_search_compatible(dev,
	    compat_data)->ocd_data;

	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0, RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		goto fail;
	}

	sc->npins = sc->soc->nregs;
	sc->pins= malloc(sc->npins * sizeof(*sc->pins), M_DEVBUF,
	    M_WAITOK | M_ZERO);

	for (i = 0; i < sc->npins;i++) {
		if (sc->soc->regs[i].name == NULL)
			continue;
		sc->pins[i].num = i;
		sc->pins[i].reg = sc->soc->regs + i;
		if (i < sc->soc->nfuncs)
			sc->pins[i].func = sc->soc->funcs + i;
	}

	fdt_pinctrl_register(dev, NULL);
	rv =  fdt_pinctrl_register_gpio_range(sc->dev,
	    ofw_bus_get_compat(dev), 0, 0, sc->soc->range_pins);
	if (rv != 0) {
		device_printf(dev, "Cannot register gpio range\n");
		panic("aaa");
	}

	fdt_pinctrl_configure_tree(dev);

	return (0);

fail:
	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);

	return (ENXIO);
}

static int
brcm_pinctrl_detach(device_t dev)
{
	struct bpc_softc *sc;

	sc = device_get_softc(dev);

	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);

	if (sc->pins != NULL)
		free(sc->pins, M_DEVBUF);
	mtx_destroy(&sc->mtx);
	return (0);

}

static device_method_t brcm_pinctrl_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			brcm_pinctrl_probe),
	DEVMETHOD(device_attach,		brcm_pinctrl_attach),
	DEVMETHOD(device_detach,		brcm_pinctrl_detach),

	/* fdt_pinctrl interface */
	DEVMETHOD(fdt_pinctrl_configure,	brcm_pinctrl_configure_node),
	DEVMETHOD(fdt_pinctrl_is_gpio,		brcm_pinctrl_is_gpio),
	DEVMETHOD(fdt_pinctrl_get_flags,	brcm_pinctrl_get_flags),
	DEVMETHOD(fdt_pinctrl_set_flags,	brcm_pinctrl_set_flags),

	/* OFW bus interface */
	DEVMETHOD(ofw_bus_get_node,		brcm_pinctr_get_node),

	DEVMETHOD_END
};

DEFINE_CLASS_0(brcmstb_pinctrl, brcmstb_pinctrl_driver, brcm_pinctrl_methods,
    sizeof(struct bpc_softc));

EARLY_DRIVER_MODULE(brcmstm_pinctrl, simplebus, brcmstb_pinctrl_driver, 0, 0,
    BUS_PASS_INTERRUPT);

