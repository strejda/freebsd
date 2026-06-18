/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/kobj.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <machine/bus.h>
#include <machine/cpu.h>

#include <dev/clk/clk_div.h>
#include <dev/clk/clk_fixed.h>
#include <dev/clk/clk_gate.h>
#include <dev/clk/clk_mux.h>
#include <dev/hwreset/hwreset.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dt-bindings/clock/raspberrypi,rp1-clocks.h>

/*Kill this after next DT update */
#ifndef RP1_CLK_MIPI0_DSI_BYTECLOCK
#define RP1_CLK_MIPI0_DSI_BYTECLOCK      45
#endif
#ifndef RP1_CLK_MIPI1_DSI_BYTECLOCK
#define RP1_CLK_MIPI1_DSI_BYTECLOCK      46
#endif

#include "clkdev_if.h"
#include "hwreset_if.h"
#include "rp1_clk_reg.h"

#if 0
#define	dprintf(format, arg...)						\
	printf("%s:(%s) " format, __func__, clknode_get_name(clk), arg)
#else
#define	dprintf(format, arg...)
#endif


#define	WRITE4(_clk, off, val)						      \
	CLKDEV_WRITE_4(clknode_get_device(_clk), off, val)
#define	READ4(_clk, off, val)						      \
	CLKDEV_READ_4(clknode_get_device(_clk), off, val)
#define	DEVICE_LOCK(_clk)						      \
	CLKDEV_DEVICE_LOCK(clknode_get_device(_clk))
#define	DEVICE_UNLOCK(_clk)						      \
	CLKDEV_DEVICE_UNLOCK(clknode_get_device(_clk))


/* Common registers bits */
#define RP1_CLK_CTRL			0x00
#define RP1_CLK_INT_DIV			0x04
#define RP1_CLK_FRACT_DIV		0x08
#define RP1_CLK_SELECT			0x0c

#define RP1_CLK_CTRL_GATE		(1 << 11)
#define RP1_CLK_CTRL_AUX_MASK		0x1F
#define RP1_CLK_CTRL_AUX_SHIFT		5
#define RP1_CLK_CTRL_STD_MASK		0x1F
#define RP1_CLK_CTRL_STD_SHIFT		0
#define RP1_CLK_FRAC_DIV_BITS		16

#define RP1_CLK_PLL_CS			0x00
#define RP1_CLK_PLL_PWR			0x04
#define RP1_CLK_PLL_FBDIV_INT		0x08
#define RP1_CLK_PLL_FBDIV_FRAC		0x0c
#define RP1_CLK_PLL_PRIM		0x10
#define RP1_CLK_PLL_SEC			0x14
#define RP1_CLK_PLL_TERN		0x18

#define RP1_CLK_PLL_PRIM_DIV1_SHIFT	16
#define RP1_CLK_PLL_PRIM_DIV1_MASK	0x7
#define RP1_CLK_PLL_PRIM_DIV2_SHIFT	12
#define RP1_CLK_PLL_PRIM_DIV2_MASK	0x7
#define RP1_CLK_PLL_SEC_DIV_SHIFT	8
#define RP1_CLK_PLL_SEC_DIV_WIDTH	5
#define RP1_CLK_PLL_SEC_DIV_MASK	((1 << RP1_CLK_PLL_SEC_DIV_WIDTH) - 1)
#define RP1_CLK_PLL_SEC_RST		(1U << 16)
#define RP1_CLK_PLL_SEC_IMPL		(1U << 31)

#define RP1_CLK_PLL_CS_LOCK		(1U << 31)
#define RP1_CLK_PLL_CS_REFDIV_SHIFT	0

#define RP1_CLK_PLL_PWR_PD		(1 << 0)
#define RP1_CLK_PLL_PWR_DACPD		(1 << 1)
#define RP1_CLK_PLL_PWR_DSMPD		(1 << 2)
#define RP1_CLK_PLL_PWR_POSTDIVPD	(1 << 3)
#define RP1_CLK_PLL_PWR_4PHASEPD	(1 << 4)
#define RP1_CLK_PLL_PWR_VCOPD		(1 << 5)
#define RP1_CLK_PLL_PWR_MASK		0x3f


/* Flags */
#define RP1_CLK_HAVE_FRACT		0x01


#define	PLIST(x) static const char *x[]

/* Integer divider in PLL block. */
#define	DIV_PLL(_id, cname, plist, o)					\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = cname,						\
	.clkdef.parent_names = (const char *[]){plist},			\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags =  CLK_NODE_STATIC_STRINGS,			\
	.offset = o,							\
	.i_shift = RP1_CLK_PLL_SEC_DIV_SHIFT,				\
	.i_width = RP1_CLK_PLL_SEC_DIV_WIDTH,				\
	.div_flags = 0,							\
}

/* Gate in PLL block. */
#define	GATE_PLL(_id, cname, plist, o, s)				\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = cname,						\
	.clkdef.parent_names = (const char *[]){plist},			\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags = CLK_NODE_STATIC_STRINGS,			\
	.offset = o,							\
	.shift = s,							\
	.mask = 3,							\
	.on_value = 3,							\
	.off_value = 0,							\
}

/* Fixed rate clock. */
#define	FRATE(_id, cname, _freq)					\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = cname,						\
	.clkdef.parent_names = NULL,					\
	.clkdef.parent_cnt = 0,						\
	.clkdef.flags = CLK_NODE_STATIC_STRINGS,			\
	.freq = _freq,							\
}


/* Fixed rate multipier/divider. */
#define	FACT(_id, cname, pname, _mult, _div)				\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = cname,						\
	.clkdef.parent_names = (const char *[]){pname},			\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags = CLK_NODE_STATIC_STRINGS,			\
	.mult = _mult,							\
	.div = _div,							\
}

/* Double primary divider in PLL block. */
#define	PRIM(_id, _cname, _plist, _base)				\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = _cname,						\
	.clkdef.parent_names = (const char *[]){_plist},		\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags =  CLK_NODE_STATIC_STRINGS,			\
	.base = _base,							\
}
/* Secondary divider in PLL block. */
#define	SEC(_id, _cname, _plist, _ctrl)				\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = _cname,						\
	.clkdef.parent_names = (const char *[]){_plist},		\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags =  CLK_NODE_STATIC_STRINGS,			\
	.ctrl = _ctrl,							\
}

/* PLL core. */
#define	PLL(_id, _cname, _plist, _base)				\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = _cname,						\
	.clkdef.parent_names = (const char *[]){_plist},		\
	.clkdef.parent_cnt = 1,						\
	.clkdef.flags =  CLK_NODE_STATIC_STRINGS,			\
	.base = _base,							\
}

/* Standard complex clock. */
#define STD_CLK(_id, _cname, _plist, _base, _nstd, _om, _idm, _flags)	\
{									\
	.clkdef.id = _id,						\
	.clkdef.name = _cname,						\
	.clkdef.parent_names = _plist,					\
	.clkdef.parent_cnt = nitems(_plist),				\
	.clkdef.flags =  CLK_NODE_STATIC_STRINGS,			\
	.base = _base,							\
	.n_stdsrc = _nstd,						\
	.n_auxsrc = nitems(_plist) - (_nstd),				\
	.oe_mask = _om,							\
	.int_div_max = _idm,						\
	.flags = _flags,						\
}

static struct ofw_compat_data compat_data[] = {
	{"raspberrypi,rp1-clocks",	1},
	{NULL,		 		0},
};

struct rp1_clk_softc {
	device_t		dev;
	phandle_t		node;
	struct resource 	*mem_res;
	struct mtx		mtx;
	struct clkdom 		*clkdom;
};

struct rp1_clk_composite_def {
	struct clknode_init_def	clkdef;
	uint32_t		base;
	int			n_stdsrc;
	int			n_auxsrc;
	uint32_t		oe_mask;
	uint32_t		int_div_max;
	uint32_t		flags;
};

struct rp1_clk_prim_def {
	struct clknode_init_def	clkdef;
	uint32_t		base;
};

struct rp1_clk_sec_def {
	struct clknode_init_def	clkdef;
	uint32_t		ctrl;
};

struct rp1_clk_pll_def {
	struct clknode_init_def	clkdef;
	uint32_t		base;
};


/* Placeholder for xoc -> clk_rp1_xosc link */
static struct clk_fixed_def rp1_clk_fixed_xosc =
	FACT(0, "xosc", "edit_me", 1, 1);


static struct clk_fixed_def rp1_clk_fixed_clks[] = {
	FRATE(RP1_CLK_MIPI0_DSI_BYTECLOCK, "clksrc_mipi0_dsi_byteclk", 0),
	FRATE(RP1_CLK_MIPI1_DSI_BYTECLOCK, "clksrc_mipi1_dsi_byteclk", 0),
	FRATE(0, "clksrc_gp0", 0),
	FRATE(0, "clksrc_gp1", 0),
	FRATE(0, "clksrc_gp2", 0),
	FRATE(0, "clksrc_gp3", 0),
	FRATE(0, "clksrc_gp4", 0),
	FRATE(0, "clksrc_gp5", 0),

	/* PLL PH dividers, after gate. */
	FACT(RP1_PLL_SYS_PRI_PH,   "pll_sys_pri_ph",   "pll_sys_pri_ph_g",   1, 2),
	FACT(RP1_PLL_AUDIO_PRI_PH, "pll_audio_pri_ph", "pll_audio_pri_ph_g", 1, 2),
	FACT(RP1_PLL_VIDEO_PRI_PH, "pll_video_pri_ph", "pll_video_pri_ph_g", 1, 2),
};

static struct clk_gate_def rp1_clk_gate_clks[] = {
	/* PLL PH dividers gate */
	GATE_PLL(0, "pll_sys_pri_ph_g", "pll_sys",     PLL_SYS + RP1_CLK_PLL_PRIM,   4),
	GATE_PLL(0, "pll_audio_pri_ph_g", "pll_audio", PLL_AUDIO + RP1_CLK_PLL_PRIM, 4),
	GATE_PLL(0, "pll_video_pri_ph_g", "pll_video", PLL_AUDIO + RP1_CLK_PLL_PRIM, 4),

};

static struct rp1_clk_pll_def rp1_clk_pll_clks[] = {
	PLL(RP1_PLL_SYS_CORE, 	"pll_sys_core", "xosc",   PLL_SYS),
	PLL(RP1_PLL_AUDIO_CORE, "pll_audio_core", "xosc", PLL_AUDIO),
	PLL(RP1_PLL_VIDEO_CORE, "pll_video_core", "xosc", PLL_VIDEO),
};

static struct rp1_clk_prim_def rp1_clk_prim_clks[] = {
	PRIM(RP1_PLL_SYS,   "pll_sys",   "pll_sys_core",   PLL_SYS),
	PRIM(RP1_PLL_AUDIO, "pll_audio", "pll_audio_core", PLL_AUDIO),
	PRIM(RP1_PLL_VIDEO, "pll_video", "pll_video_core", PLL_VIDEO),
};

static struct rp1_clk_sec_def rp1_clk_sec_clks[] = {
	SEC(RP1_PLL_SYS_SEC,    "pll_sys_sec",    "pll_sys_core" ,  PLL_SYS   + RP1_CLK_PLL_SEC),
	SEC(RP1_PLL_AUDIO_SEC,  "pll_audio_sec",  "pll_audio_core", PLL_AUDIO + RP1_CLK_PLL_SEC),
	SEC(RP1_PLL_AUDIO_TERN, "pll_audio_tern", "pll_audio_core", PLL_VIDEO + RP1_CLK_PLL_TERN),
	SEC(RP1_PLL_VIDEO_SEC,  "pll_video_sec",  "pll_video_core", PLL_VIDEO + RP1_CLK_PLL_SEC),
};

/* Parent lists. */
PLIST(sys_p) = {"xosc", NULL, "pll_sys"};
PLIST(slow_sys_p) = {"xosc"};
PLIST(dma_p) = {"pll_sys_pri_ph", "pll_video", "xosc", "clksrc_gp0",
		"clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		"clksrc_gp5"};
PLIST(uart_p) = {"pll_sys_pri_ph", "pll_video", "xosc", "clksrc_gp0",
		"clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		"clksrc_gp5"};
PLIST(eth_p) = {"pll_sys_sec", "pll_sys", "pll_video_sec", "clksrc_gp0",
		"clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		"clksrc_gp5"};
PLIST(pwm0_p) = {"pll_sys_pri_ph", "pll_video_sec", "xosc", "clksrc_gp0",
		 "clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		 "clksrc_gp5"};
PLIST(pwm1_p) = {"pll_sys_pri_ph", "pll_video_sec", "xosc", "clksrc_gp0",
		 "clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		 "clksrc_gp5"};
PLIST(audio_in_p) = {"pll_audio", "pll_audio_pri_ph", "pll_audio_sec" ,"pll_video_sec", "xosc",
		    "clksrc_gp0", "clksrc_gp1", "clksrc_gp2", "clksrc_gp3",
		    "clksrc_gp4", "clksrc_gp5"};
PLIST(audio_out_p) = {"pll_audio", "pll_audio_sec", "pll_video_sec", "xosc",
		  "clksrc_gp0", "clksrc_gp1", "clksrc_gp2", "clksrc_gp3",
		  "clksrc_gp4", "clksrc_gp5"};
PLIST(i2s_p) = {"xosc", "pll_audio", "pll_audio_sec", "clksrc_gp0",
		"clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4",
		"clksrc_gp5"};
PLIST(mipi0_cfg_p) = {"rp1-xosc"};
PLIST(mipi1_cfg_p) = {"rp1-xosc"};
PLIST(eth_tsu_p) = {"xosc","pll_video_sec", "clksrc_gp0","clksrc_gp1",
		    "clksrc_gp2", "clksrc_gp3", "clksrc_gp4", "clksrc_gp5"};
PLIST(adc_p) = {"xosc", "pll_audio_tern", "clksrc_gp0","clksrc_gp1",
		"clksrc_gp2", "clksrc_gp3", "clksrc_gp4", "clksrc_gp5"};
PLIST(sdio_timer_p) = {"xosc"};
PLIST(sdio_alt_src_p) = {"pll_sys"};
PLIST(gp0_p) = { "xosc",  "clksrc_gp1", "clksrc_gp2", "clksrc_gp3",
		 "clksrc_gp4", "clksrc_gp5", "pll_sys", "pll_audio",
		  NULL, NULL, "clk_i2s", "clk_adc",
		  NULL, NULL, NULL, "clk_sys"};
PLIST(gp1_p) = {"clk_sdio_timer", "clksrc_gp0", "clksrc_gp2", "clksrc_gp3",
		"clksrc_gp4", "clksrc_gp5", "pll_sys_pri_ph", "pll_audio_pri_ph",
		NULL, NULL, "clk_adc", "clk_dpi",
		"clk_pwm0", NULL, NULL, NULL,};
PLIST(gp2_p) = {"clk_sdio_alt_src", "clksrc_gp0", "clksrc_gp1", "clksrc_gp3",
		"clksrc_gp4", "clksrc_gp5", "pll_sys_sec", "pll_audio_sec",
		"pll_video", "clk_audio_in", "clk_dpi", "clk_pwm0",
		"clk_pwm1", "clk_mipi0_dpi", "clk_mipi1_cfg", "clk_sys"};
PLIST(gp3_p) = {"xosc", "clksrc_gp0", "clksrc_gp1", "clksrc_gp2",
		"clksrc_gp4", "clksrc_gp5", NULL, NULL,
		"pll_video_pri_ph", "clk_audio_out", NULL, NULL,
		"clk_mipi1_dpi", NULL, NULL, NULL};
PLIST(gp4_p) = {"xosc",  "clksrc_gp0", "clksrc_gp1", "clksrc_gp2",
		"clksrc_gp3", "clksrc_gp5", "pll_audio_tern", "pll_video_sec",
		NULL, NULL, NULL, "clk_mipi0_cfg",
	"clk_uart", NULL, NULL, "clk_sys"};
PLIST(gp5_p) = {"xosc", "clksrc_gp0", "clksrc_gp1", "clksrc_gp2",
		"clksrc_gp3", "clksrc_gp4", "pll_audio_tern", "pll_video_sec",
		"clk_eth_tsu", NULL, "clk_vec", NULL,
		 NULL, NULL, NULL, NULL};
PLIST(vec_p) =  {"pll_sys_pri_ph", "pll_video_sec",  "pll_video", "clksrc_gp0",
		 "clksrc_gp1",  "clksrc_gp2", "clksrc_gp3", "clksrc_gp4"};
PLIST(dpi_p) = {"pll_sys", "pll_video_sec", "pll_video", "clksrc_gp0",
		"clksrc_gp1", "clksrc_gp2", "clksrc_gp3", "clksrc_gp4"};
PLIST(mipi0_dpi_p) = {"pll_sys",  "pll_video_sec", "pll_video", "clksrc_mipi0_dsi_byteclk",
		      "clksrc_gp0", "clksrc_gp1", "clksrc_gp2", "clksrc_gp3"};
PLIST(mipi1_dpi_p) = {"pll_sys", "pll_video_sec", "pll_video", "clksrc_mipi1_dsi_byteclk",
		      "clksrc_gp0", "clksrc_gp1", "clksrc_gp2", "clksrc_gp3"};

static struct rp1_clk_composite_def rp1_std_clks[] = {
/*								_base,		     nstd, om, idm, flags	*/
	STD_CLK(RP1_CLK_SYS, 	   "clk_sys",	    sys_p,	 CLK_SYS,		3,	  0, 24, 0),
	STD_CLK(RP1_CLK_SLOW_SYS,  "clk_slow_sys",  slow_sys_p,  CLK_SLOW_SYS,		1,	  0,  8, 0),
	STD_CLK(RP1_CLK_DMA, 	   "clk_dma",	    dma_p,	 CLK_DMA,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_UART,	   "clk_uart",	    uart_p,	 CLK_UART,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_ETH,	   "clk_eth",	    eth_p,	 CLK_ETH,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_PWM0,	   "clk_pwm0",	    pwm0_p,	 CLK_PWM0,		0,	  0, 16, 0),
	STD_CLK(RP1_CLK_PWM1,	   "clk_pwm1",	    pwm1_p,	 CLK_PWM1,		0,	  0, 16, 0),
	STD_CLK(RP1_CLK_AUDIO_IN,  "clk_audio_in",  audio_in_p,  CLK_AUDIO_IN,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_AUDIO_OUT, "clk_audio_out", audio_out_p, CLK_AUDIO_OUT,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_I2S,	   "clk_i2s",	    i2s_p,	 CLK_I2S,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_MIPI0_CFG, "clk_mipi0_cfg", mipi0_cfg_p, CLK_MIPI0_CFG,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_MIPI1_CFG, "clk_mipi1_cfg", mipi1_cfg_p, CLK_MIPI1_CFG,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_ADC,	   "clk_adc",       adc_p,	 CLK_ADC,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_SDIO_TIMER, "clk_sdio_timer", sdio_timer_p, CLK_SDIO_TIMER,     0,	  0,  8, 0),
	STD_CLK(RP1_CLK_SDIO_ALT_SRC, "clk_sdio_alt_src", sdio_alt_src_p, CLK_SDIO_ALT_SRC, 0,	  0,  8, 0),
	STD_CLK(RP1_CLK_GP0,	   "clk_gp0",       gp0_p, 	 CLK_GP0,		0,  1 <<  0, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_GP1,	   "clk_gp1",       gp1_p, 	 CLK_GP1,		0,  1 <<  1, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_GP2,	   "clk_gp2",       gp2_p, 	 CLK_GP2,		0,  1 <<  2, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_GP3,	   "clk_gp3",       gp3_p, 	 CLK_GP3,		0,  1 <<  3, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_GP4,	   "clk_gp4",       gp4_p, 	 CLK_GP4,		0,  1 <<  4, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_GP5,	   "clk_gp5",       gp5_p, 	 CLK_GP5,		0,  1 <<  5, 16, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_VEC,	   "clk_vec",       vec_p, 	 VIDEO_CLK_VEC,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_DPI,	   "clk_dpi",       dpi_p, 	 VIDEO_CLK_DPI,		0,	  0,  8, 0),
	STD_CLK(RP1_CLK_MIPI0_DPI, "clk_mipi0_dpi", mipi0_dpi_p, VIDEO_CLK_MIPI0_DPI,	0,	  0,  8, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_MIPI1_DPI, "clk_mipi1_dpi", mipi1_dpi_p, VIDEO_CLK_MIPI1_DPI,	0,	  0,  8, RP1_CLK_HAVE_FRACT),
	STD_CLK(RP1_CLK_ETH_TSU,   "clk_eth_tsu",   eth_tsu_p,   CLK_ETH_TSU,		0,	  0,  8, 0),

};

static inline
uint64_t abs_diff64(uint64_t  a, uint64_t  b) {
	return (a > b) ? (a - b) : (b - a);
}

/***************** PLL  ****************************************/
struct rp1_clk_pll_sc {
	/* Register set */
	uint32_t	base;		/* base PLL register */
};

static int
rp1_clk_pll_init(struct clknode *clk, device_t dev)
{
	struct rp1_clk_pll_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);

	READ4(clk, sc->base + RP1_CLK_PLL_CS, &reg);
	if ((reg & RP1_CLK_PLL_CS_LOCK) == 0) {
		/* Init unlocked PLL to safe state */
		WRITE4(clk, sc->base + RP1_CLK_PLL_PWR, RP1_CLK_PLL_PWR_MASK);
		WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_INT, 20);
		WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_FRAC, 0);
		WRITE4(clk, sc->base + RP1_CLK_PLL_CS,
		    1 << RP1_CLK_PLL_CS_REFDIV_SHIFT);
	}

	clknode_init_parent_idx(clk, 0);
	return (0);
}

static int
rp1_clk_pll_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rp1_clk_pll_sc *sc;
	uint32_t fbint, fbfrac;
	uint64_t tmp;
	sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_PLL_FBDIV_INT, &fbint);
	READ4(clk, sc->base + RP1_CLK_PLL_FBDIV_FRAC, &fbfrac);
	DEVICE_UNLOCK(clk);

	dprintf("Read: base=%x,  fbint: %d, fbfrac: %d\n",
	    sc->base,  fbint, fbfrac);

	tmp = ((uint64_t)fbint << 24) +  fbfrac;
	tmp = *freq * tmp;
	*freq = tmp >> 24;

	dprintf("Final freq=%ju\n", *freq);
	return (0);
}

static int
rp1_clk_pll_compute(struct rp1_clk_pll_sc *sc, uint64_t fout, uint64_t fin,
    uint32_t *fbint, uint32_t *fbfrac, int flags)

{
	uint64_t mult, freq;

	flags = CLK_SET_ROUND(flags);

	mult = ((fout << 24) + fin / 2) / fin;	/* 24 bits of fractional div */

	freq = (fin * mult) >> 24;
	if (flags == CLK_SET_ROUND_ANY || flags == CLK_SET_ROUND_EXACT) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		mult++;
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		mult--;
	}

	freq = (fin * mult) >> 24;
	if (flags == CLK_SET_ROUND_ANY) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_EXACT && freq != fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		return (ERANGE);
	}

	*fbint = (uint32_t)(mult >> 24);
	*fbfrac = (uint32_t)((mult & 0xffffff) << (32 - 24));

	return(0);
}
static int
rp1_clk_pll_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rp1_clk_pll_sc *sc;
	uint32_t fbint, fbfrac;
	uint64_t tmp;
	int rv;

	sc = clknode_get_softc(clk);
	if ((*fout /fin) > 16) {
		panic("Unsupported PLL multipliers > 16, fin: %jd, fout: %jd\n",
		    (uintmax_t)fin, (uintmax_t)*fout);
	}
	rv = rp1_clk_pll_compute(sc, *fout, fin, &fbint, &fbfrac, flags);

	if (rv != 0)
		return (rv);

	if (flags & CLK_SET_DRYRUN)
		return (0);

	*stop = 1;
	tmp = ((uint64_t)fbint << 24) +  fbfrac;
	tmp = fin * tmp;
	*fout = tmp >> 24;

	DEVICE_LOCK(clk);
	/* Disable fb divider first. */
	WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_INT, 0);
	WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_FRAC, 0);

	/* Then setup it. */
	WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_INT, fbint);
	WRITE4(clk, sc->base + RP1_CLK_PLL_FBDIV_FRAC, fbfrac);
	DEVICE_UNLOCK(clk);

	dprintf("Write: base: %x,  int_div: %d, frac_div: 0x%08X\n", sc->base,
	    fbint, fbfrac);

	return (0);
}

static int
rp1_clk_pll_set_gate(struct clknode *clk, bool enable)
{
	struct rp1_clk_pll_sc *sc;
	uint32_t reg;
	int i;

	sc = clknode_get_softc(clk);

	if (!enable) {
		DEVICE_LOCK(clk);
		WRITE4(clk, sc->base + RP1_CLK_PLL_PWR, RP1_CLK_PLL_PWR_MASK);
		DEVICE_UNLOCK(clk);

		return(0);
	}

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_PLL_CS, &reg);
	DEVICE_UNLOCK(clk);
	if ((reg & RP1_CLK_PLL_CS_LOCK) == 0) {
		printf("%s:(%s) Unlocked PLL should not be enabled.\n ", __func__,
		    clknode_get_name(clk));
		return (EINVAL);
	}

	/* Unreset PLL */
	DEVICE_LOCK(clk);
	WRITE4(clk, sc->base + RP1_CLK_PLL_PWR, 0);
	DEVICE_UNLOCK(clk);

	/* Wait for lock */
	for (i = 100; i > 0; i--) {

		DEVICE_LOCK(clk);
		READ4(clk, sc->base + RP1_CLK_PLL_CS, &reg);
		DEVICE_UNLOCK(clk);
		if (reg & RP1_CLK_PLL_CS_LOCK)
			break;
	}
	if ( i <= 0) {
		printf("%s:(%s) Lock timeouted.\n ", __func__,
		    clknode_get_name(clk));
		return (ETIMEDOUT);
	}
	return (0);
}

static int
rp1_clk_pll_get_gate(struct clknode *clk, bool *enabled)
{
	struct rp1_clk_pll_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);


	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_PLL_PWR, &reg);
	DEVICE_UNLOCK(clk);

	*enabled = (reg & RP1_CLK_PLL_PWR_PD) ||
	    (reg & RP1_CLK_PLL_PWR_POSTDIVPD);

	return (0);
}

static clknode_method_t rp1_clk_pll_clknode_methods[] = {
	/* Device interface */
	CLKNODEMETHOD(clknode_init,	   rp1_clk_pll_init),
	CLKNODEMETHOD(clknode_recalc_freq, rp1_clk_pll_recalc),
	CLKNODEMETHOD(clknode_set_freq,	   rp1_clk_pll_set_freq),
	CLKNODEMETHOD(clknode_set_gate,	   rp1_clk_pll_set_gate),
	CLKNODEMETHOD(clknode_get_gate,	   rp1_clk_pll_get_gate),

	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rp1_clk_pll_clknode, rp1_clk_pll_clknode_class,
    rp1_clk_pll_clknode_methods, sizeof(struct rp1_clk_pll_sc),
    clknode_class);

static int
rp1_clk_pll_register(struct clkdom *clkdom,
    struct rp1_clk_pll_def *clkdef)
{
	struct clknode *clk;
	struct rp1_clk_pll_sc *sc;

	clk = clknode_create(clkdom, &rp1_clk_pll_clknode_class,
	    &clkdef->clkdef);
	if (clk == NULL)
		return (1);

	sc = clknode_get_softc(clk);
	sc->base = clkdef->base;
	clknode_register(clkdom, clk);
	return (0);
}

static void
rp1_clk_init_pll(struct rp1_clk_softc *sc,
     struct rp1_clk_pll_def *clks,  int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = rp1_clk_pll_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("rp1_clk_pll_register failed: %d", rv);
	}
}

/***************** PLL primary divider ****************************************/

struct rp1_clk_prim_sc {
	/* Register set */
	uint32_t	base;		/* control  register */
};

static int
rp1_clk_prim_init(struct clknode *clk, device_t dev)
{

	clknode_init_parent_idx(clk, 0);
	return (0);
}

static int
rp1_clk_prim_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rp1_clk_prim_sc *sc;
	uint32_t reg, div1, div2;

	sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_PLL_PRIM, &reg);
	DEVICE_UNLOCK(clk);

	div1 = (reg >> RP1_CLK_PLL_PRIM_DIV1_SHIFT) & RP1_CLK_PLL_PRIM_DIV1_MASK;
	div2 = (reg >> RP1_CLK_PLL_PRIM_DIV2_SHIFT) & RP1_CLK_PLL_PRIM_DIV2_MASK;

	dprintf("Read: base: %x,  div1: %d, div2: %d\n", sc->base, div1, div2);

	*freq = *freq / (div1 * div2);

	dprintf("Final freq=%ju\n", *freq);
	return (0);
}

static int
rp1_clk_prim_compute(struct rp1_clk_prim_sc *sc, uint64_t fout,
    uint64_t fin, uint32_t *out_div1, uint32_t *out_div2, int flags)
{
	uint32_t div1, div2;
	uint32_t best_div1, best_div2;
	uint64_t best_diff, diff, freq;

	best_diff = ~0;

	for (div1 = 1; div1 <= 7; div1++) {
		for (div2 = 1; div2 <= div1; div2++) {
			freq = fin / (div1 * div2);
			diff = abs_diff64(fout,  freq);

			if (freq == fout) {
				best_div1 = div1;
				best_div2 = div2;
				best_diff = 0;
				goto done;
			} else if (diff < best_diff) {
				if (freq > fout  &&
				    (CLK_SET_ROUND(flags) & CLK_SET_ROUND_UP)
				     == 0)
					continue;
				if (freq < fout  &&
				    (CLK_SET_ROUND(flags) & CLK_SET_ROUND_DOWN)
				     == 0)
					continue;
				best_div1 = div1;
				best_div2 = div2;
				best_diff = diff;
			}
		}
	}

done:
	if (best_diff == ~0)
		return (ERANGE);
	*out_div1 = best_div1;
	*out_div2 = best_div2;

	return(0);
}

static int
rp1_clk_prim_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rp1_clk_prim_sc *sc;
	uint32_t div1, div2, reg;
	int rv;

	sc = clknode_get_softc(clk);

	rv = rp1_clk_prim_compute(sc, *fout, fin, &div1, &div2, flags);
	if (rv != 0)
		return (rv);
	*fout =  fin / (div1 * div2);

	if (flags & CLK_SET_DRYRUN)
		return (0);

	*stop = 1;
	*fout = fin / (div1 * div2);

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_PLL_PRIM, &reg);
	reg &= ~(RP1_CLK_PLL_PRIM_DIV1_MASK << RP1_CLK_PLL_PRIM_DIV1_SHIFT);
	reg |= div1 << RP1_CLK_PLL_PRIM_DIV1_SHIFT;
	reg &= ~(RP1_CLK_PLL_PRIM_DIV2_MASK << RP1_CLK_PLL_PRIM_DIV2_SHIFT);
	reg |= div2 << RP1_CLK_PLL_PRIM_DIV2_SHIFT;
	WRITE4(clk, sc->base + RP1_CLK_PLL_PRIM, reg);
	DEVICE_UNLOCK(clk);

	dprintf("Read: base=%x,  div1: %d, div2: %d, reg: 0x%08X\n",
	    sc->base, div1, div2, reg);

	return (0);
}

static clknode_method_t rp1_clk_prim_clknode_methods[] = {
	/* Device interface */
	CLKNODEMETHOD(clknode_init,	   rp1_clk_prim_init),
	CLKNODEMETHOD(clknode_recalc_freq, rp1_clk_prim_recalc),
	CLKNODEMETHOD(clknode_set_freq,	   rp1_clk_prim_set_freq),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rp1_clk_prim_clknode, rp1_clk_prim_clknode_class,
    rp1_clk_prim_clknode_methods, sizeof(struct rp1_clk_prim_sc),
    clknode_class);

static int
rp1_clk_prim_register(struct clkdom *clkdom,
    struct rp1_clk_prim_def *clkdef)
{
	struct clknode *clk;
	struct rp1_clk_prim_sc *sc;

	clk = clknode_create(clkdom, &rp1_clk_prim_clknode_class,
	    &clkdef->clkdef);
	if (clk == NULL)
		return (1);

	sc = clknode_get_softc(clk);

	sc->base = clkdef->base;

	clknode_register(clkdom, clk);

	return (0);
}

static void
rp1_clk_init_prim(struct rp1_clk_softc *sc,
     struct rp1_clk_prim_def *clks,  int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = rp1_clk_prim_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("rp1_clk_prim_register failed: %d", rv);
	}
}


/***************** PLL secondary divider **************************************/

struct rp1_clk_sec_sc {
	/* Register set */
	uint32_t	ctrl;		/* control  register */
};

static int
rp1_clk_sec_init(struct clknode *clk, device_t dev)
{

	clknode_init_parent_idx(clk, 0);
	return (0);
}

static int
rp1_clk_sec_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rp1_clk_sec_sc *sc;
	uint32_t reg, div;

	sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	READ4(clk, sc->ctrl, &reg);
	DEVICE_UNLOCK(clk);

	div = (reg >> RP1_CLK_PLL_SEC_DIV_SHIFT) & RP1_CLK_PLL_SEC_DIV_MASK;

	dprintf("Read: ctrl: %x,  div: %d\n", sc->ctrl, div);

	*freq = *freq / div;

	dprintf("Final freq=%ju\n", *freq);
	return (0);
}

static int
rp1_clk_sec_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rp1_clk_sec_sc *sc;
	uint32_t div, reg;
	uint64_t freq;

	sc = clknode_get_softc(clk);

	flags = CLK_SET_ROUND(flags);
	div  = (fin  + *fout / 2) / *fout;
	freq = fin / div;

	if (flags == CLK_SET_ROUND_ANY || flags == CLK_SET_ROUND_EXACT) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_UP && freq < *fout) {
		div--;
	} else if (flags == CLK_SET_ROUND_DOWN && freq > *fout) {
		div++;
	}

	/* Clip divider range */
	if (div  < 8) div = 8;
	if (div  > 19) div = 19;

	/* Check if requested  rounding is  still valid */
	freq = fin / div;
	if (flags == CLK_SET_ROUND_ANY) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_EXACT && freq != *fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_UP && freq < *fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_DOWN && freq > *fout) {
		return (ERANGE);
	}

	if (flags & CLK_SET_DRYRUN)
		return (0);

	*stop = 1;
	*fout = fin / div;

	DEVICE_LOCK(clk);
	/* Put divider to reset */
	READ4(clk, sc->ctrl, &reg);
	reg |= RP1_CLK_PLL_SEC_RST;
	WRITE4(clk, sc->ctrl, reg);

	/* Setup divider */
	reg &= ~(RP1_CLK_PLL_SEC_DIV_MASK << RP1_CLK_PLL_SEC_DIV_SHIFT);
	reg |= div << RP1_CLK_PLL_SEC_DIV_SHIFT;
	WRITE4(clk, sc->ctrl, reg);

	/* Put divider out of reset */
	DELAY(10);
	reg &= ~RP1_CLK_PLL_SEC_RST;
	WRITE4(clk, sc->ctrl, reg);

	DEVICE_UNLOCK(clk);

	dprintf("Write: ctrl: %x,  div: %d, reg: 0x%08X\n", sc->ctrl, div, reg);

	return (0);
}

static int
rp1_clk_sec_set_gate(struct clknode *clk, bool enable)
{

	struct rp1_clk_sec_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	READ4(clk, sc->ctrl, &reg);
	if (enable)
		reg &= ~RP1_CLK_PLL_SEC_RST;
	else
		reg |= RP1_CLK_PLL_SEC_RST;
	WRITE4(clk, sc->ctrl, reg);
	DEVICE_UNLOCK(clk);

	dprintf("Set gate to 0x%08X, reg: 0x%08X\n", reg, sc->ctrl);

	return (0);
}

static int
rp1_clk_sec_get_gate(struct clknode *clk, bool *enabled)
{
	struct rp1_clk_sec_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);


	DEVICE_LOCK(clk);
	READ4(clk, sc->ctrl, &reg);
	DEVICE_UNLOCK(clk);

	*enabled = (reg & RP1_CLK_PLL_SEC_RST) ? false: true;

	return (0);
}

static clknode_method_t rp1_clk_sec_clknode_methods[] = {
	/* Device interface */
	CLKNODEMETHOD(clknode_init,	   rp1_clk_sec_init),
	CLKNODEMETHOD(clknode_recalc_freq, rp1_clk_sec_recalc),
	CLKNODEMETHOD(clknode_set_freq,	   rp1_clk_sec_set_freq),
	CLKNODEMETHOD(clknode_set_gate,	   rp1_clk_sec_set_gate),
	CLKNODEMETHOD(clknode_get_gate,	   rp1_clk_sec_get_gate),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rp1_clk_sec_clknode, rp1_clk_sec_clknode_class,
    rp1_clk_sec_clknode_methods, sizeof(struct rp1_clk_sec_sc),
    clknode_class);

static int
rp1_clk_sec_register(struct clkdom *clkdom,
    struct rp1_clk_sec_def *clkdef)
{
	struct clknode *clk;
	struct rp1_clk_sec_sc *sc;

	clk = clknode_create(clkdom, &rp1_clk_sec_clknode_class,
	    &clkdef->clkdef);
	if (clk == NULL)
		return (1);

	sc = clknode_get_softc(clk);

	sc->ctrl = clkdef->ctrl;

	clknode_register(clkdom, clk);

	return (0);
}

static void
rp1_clk_init_sec(struct rp1_clk_softc *sc,
     struct rp1_clk_sec_def *clks,  int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = rp1_clk_sec_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("rp1_clk_prim_register failed: %d", rv);
	}
}


/***************** Composite  ***********************************************/

struct rp1_clk_composite_sc {
	/* Register set */
	uint32_t	base;		/* control  register */
	int		n_stdsrc;	/* number of glitch free sources */
	int		n_auxsrc;	/* number of not-glitch free sources */
	uint32_t	oe_mask;	/* some clocks have separater OE bit
					 * in OE_CTRL register */
	uint32_t	int_div_max;	/* maximal integer divider */
	uint32_t	flags;
};

static int
rp1_clk_composite_init(struct clknode *clk, device_t dev)
{
	struct rp1_clk_composite_sc *sc;
	uint32_t reg;
	int idx;

	sc = clknode_get_softc(clk);
	idx = 0;

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_CTRL, &reg);
	DEVICE_UNLOCK(clk);
	dprintf("readed reg: 0x%04X, 0x%X\n", sc->base + RP1_CLK_CTRL, reg);

	idx = (reg >> RP1_CLK_CTRL_STD_SHIFT) & RP1_CLK_CTRL_STD_MASK;
	if (idx >=  sc->n_stdsrc) {
		idx = (reg >> RP1_CLK_CTRL_AUX_SHIFT) & RP1_CLK_CTRL_AUX_MASK;
		idx += sc->n_stdsrc;
	}

	clknode_init_parent_idx(clk, idx);
	return (0);
}

static int
rp1_clk_composite_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rp1_clk_composite_sc *sc;
	uint32_t reg, frac;
	uint64_t div, tmp;

	sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_INT_DIV, &reg);
	if (sc->flags & RP1_CLK_HAVE_FRACT)
		READ4(clk, sc->base + RP1_CLK_FRACT_DIV, &frac);
	else
		frac = 0;
	DEVICE_UNLOCK(clk);

	div = reg;
	dprintf("Read: base:%x, div: %jd, fract: %d\n",
	    sc->base,  div, frac);

	if (div == 0)
		div = 1 << 16;

	/* do fractional divide */
	div = (div << RP1_CLK_FRAC_DIV_BITS) |
	    (frac >> (32 - RP1_CLK_FRAC_DIV_BITS));
	tmp = *freq << RP1_CLK_FRAC_DIV_BITS;
	*freq = tmp / div;

	dprintf("Final freq=%ju\n", *freq);
	return (0);
}

static int
rp1_clk_composite_compute_fract(struct rp1_clk_composite_sc *sc, uint64_t fout,
    uint64_t fin, uint32_t *int_div, uint32_t *frac_div, int flags)
{
	uint64_t div, freq;

	flags = CLK_SET_ROUND(flags);
	div = ((fin  + fout/ 2) << RP1_CLK_FRAC_DIV_BITS) / fout;
	freq = (fin << RP1_CLK_FRAC_DIV_BITS) / div;


	if (flags == CLK_SET_ROUND_ANY || flags == CLK_SET_ROUND_EXACT) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		div -= 1 << RP1_CLK_FRAC_DIV_BITS;
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		div += 1 << RP1_CLK_FRAC_DIV_BITS;
	}

	/*
	 * Clip divider range
	 * Note that maximim divider cannot have fractional part
	 */
	if (div  < (1 << RP1_CLK_FRAC_DIV_BITS))
		 div = 1 << RP1_CLK_FRAC_DIV_BITS;
	if (div  > (sc->int_div_max << RP1_CLK_FRAC_DIV_BITS))
		 div = sc->int_div_max << RP1_CLK_FRAC_DIV_BITS;

	/* Check if requested rounding is still valid */
	freq = (fin << RP1_CLK_FRAC_DIV_BITS) / div;
	if (flags == CLK_SET_ROUND_ANY) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_EXACT && freq != fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		return (ERANGE);
	}

	*int_div = (uint32_t)(div >> RP1_CLK_FRAC_DIV_BITS);
	*frac_div =  (uint32_t)(div & (1 << RP1_CLK_FRAC_DIV_BITS) - 1);
	*frac_div  <<= 32 - RP1_CLK_FRAC_DIV_BITS;
	return (0);
}


static int
rp1_clk_composite_compute_int(struct rp1_clk_composite_sc *sc, uint64_t fout,
    uint64_t fin, uint32_t *int_div,  int flags)
{
	uint32_t div;
	uint64_t freq;

	flags = CLK_SET_ROUND(flags);
	div = (fin  + fout/ 2) / fout;
	freq = fin / div;


	if (flags == CLK_SET_ROUND_ANY || flags == CLK_SET_ROUND_EXACT) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		div--;
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		div++;
	}

	/* Clip divider range */
	if (div  < 1)
		div = 1;
	if (div  > sc->int_div_max)
		div = sc->int_div_max;

	/* Check if requested rounding is still valid */
	freq = fin / div;
	if (flags == CLK_SET_ROUND_ANY) {
		/* Do nothing */
	} else if (flags == CLK_SET_ROUND_EXACT && freq != fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_UP && freq < fout) {
		return (ERANGE);
	} else if (flags == CLK_SET_ROUND_DOWN && freq > fout) {
		return (ERANGE);
	}

	*int_div  = (uint32_t)div;
	return (0);
}

static int
rp1_clk_composite_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rp1_clk_composite_sc *sc;
	uint32_t int_div, frac_div;
	uint64_t div;
	int rv;

	sc = clknode_get_softc(clk);
	frac_div = 0;

	if (sc->flags & RP1_CLK_HAVE_FRACT) {
		rv = rp1_clk_composite_compute_fract(sc, *fout, fin, &int_div,
		    &frac_div, flags);
	} else {
		rv = rp1_clk_composite_compute_int(sc, *fout, fin, &int_div,
		    flags);
	}
dprintf("computed rv: %d  int_div: %d, frac_div: 0x%08X\n", rv,
	    int_div, frac_div);
	if (rv != 0)
		return (rv);

	if (flags & CLK_SET_DRYRUN)
		return (0);

	*stop = 1;
	div = (int_div << RP1_CLK_FRAC_DIV_BITS) |
	    (frac_div >> (32 - RP1_CLK_FRAC_DIV_BITS));
	*fout = (fin << RP1_CLK_FRAC_DIV_BITS) / div;

	DEVICE_LOCK(clk);
	WRITE4(clk, sc->base + RP1_CLK_INT_DIV, int_div);
	if (sc->flags & RP1_CLK_HAVE_FRACT)
		WRITE4(clk, sc->base + RP1_CLK_FRACT_DIV, frac_div);
	DEVICE_UNLOCK(clk);

	dprintf("Write: base: %x, int_div: %d, frac_div: 0x%08X\n", sc->base,
	    int_div, frac_div);

	return (0);
}

static int
rp1_clk_composite_get_gate(struct clknode *clk, bool *enabled)
{
	struct rp1_clk_composite_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);


	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_CTRL, &reg);
	DEVICE_UNLOCK(clk);

	*enabled = (reg & RP1_CLK_CTRL_GATE) ? true : false;

	return (0);
}

static int
rp1_clk_composite_set_mux(struct clknode *clk, int idx)
{

	struct rp1_clk_composite_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);

	dprintf("Set mux to %d\n", idx);

	DEVICE_LOCK(clk);
		READ4(clk, sc->base + RP1_CLK_CTRL, &reg);
	if (idx >=  sc->n_stdsrc) {
		idx -= sc->n_stdsrc;
		/* set AUX index */
		reg &= ~(RP1_CLK_CTRL_AUX_MASK << RP1_CLK_CTRL_AUX_SHIFT);
		reg |= idx << RP1_CLK_CTRL_AUX_SHIFT;

		/* set standard mux to aux */
		reg &=  ~(RP1_CLK_CTRL_STD_MASK << RP1_CLK_CTRL_STD_SHIFT);
		reg |=  sc->n_stdsrc << RP1_CLK_CTRL_STD_SHIFT;
	} else {
		/* set standard mux  */
		reg &=  ~(RP1_CLK_CTRL_STD_MASK << RP1_CLK_CTRL_STD_SHIFT);
		reg |= idx << RP1_CLK_CTRL_STD_SHIFT;
	}

	dprintf("Write: reg: %x, val: %x\n", sc->base + RP1_CLK_CTRL, reg);
	WRITE4(clk, sc->base + RP1_CLK_CTRL, reg);
	DEVICE_UNLOCK(clk);

	return (0);
}


static int
rp1_clk_composite_set_gate(struct clknode *clk, bool enable)
{
	struct rp1_clk_composite_sc *sc;
	uint32_t reg;

	sc = clknode_get_softc(clk);


	DEVICE_LOCK(clk);
	READ4(clk, sc->base + RP1_CLK_CTRL, &reg);
	if (enable)
		reg |= RP1_CLK_CTRL_GATE;
	else
		reg &= ~RP1_CLK_CTRL_GATE;
	WRITE4(clk, sc->base + RP1_CLK_CTRL, reg);

	if (sc ->oe_mask != 0) {
		READ4(clk, GPCLK_OE, &reg);
		if (enable)
			reg |= sc->oe_mask;
		else
			reg &= ~sc->oe_mask;
		WRITE4(clk, GPCLK_OE, reg);
	}

	DEVICE_UNLOCK(clk);

	dprintf("Set gate to 0x%08X, reg: 0x%08X\n",
	    reg, sc->base + RP1_CLK_CTRL);

	return (0);
}

static clknode_method_t rp1_clk_composite_clknode_methods[] = {
	/* Device interface */
	CLKNODEMETHOD(clknode_init,	   rp1_clk_composite_init),
	CLKNODEMETHOD(clknode_set_mux,	   rp1_clk_composite_set_mux),
	CLKNODEMETHOD(clknode_recalc_freq, rp1_clk_composite_recalc),
	CLKNODEMETHOD(clknode_set_freq,	   rp1_clk_composite_set_freq),
	CLKNODEMETHOD(clknode_set_gate,	   rp1_clk_composite_set_gate),
	CLKNODEMETHOD(clknode_get_gate,	   rp1_clk_composite_get_gate),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rp1_clk_composite_clknode, rp1_clk_composite_clknode_class,
    rp1_clk_composite_clknode_methods, sizeof(struct rp1_clk_composite_sc),
    clknode_class);

static int
rp1_clk_composite_register(struct clkdom *clkdom,
    struct rp1_clk_composite_def *clkdef)
{
	struct clknode *clk;
	struct rp1_clk_composite_sc *sc;

	clk = clknode_create(clkdom, &rp1_clk_composite_clknode_class,
	    &clkdef->clkdef);
	if (clk == NULL)
		return (1);

	sc = clknode_get_softc(clk);

	sc->base = clkdef->base;
	sc->n_stdsrc = clkdef->n_stdsrc;
	sc->n_auxsrc = clkdef->n_auxsrc;
	sc->oe_mask = clkdef->oe_mask;
	sc->int_div_max = clkdef->int_div_max;
	sc->flags = clkdef->flags;

	clknode_register(clkdom, clk);

	return (0);
}


static void
rp1_clk_init_composite(struct rp1_clk_softc *sc,
     struct rp1_clk_composite_def *clks,  int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = rp1_clk_composite_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("rp1_clk_composite_register failed: %d", rv);
	}
}

static void
rp1_clk_init_gates(struct rp1_clk_softc *sc, struct clk_gate_def *clks, int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = clknode_gate_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("clknode_gate_register failed");
	}
}


static void
rp1_clk_init_fixed(struct rp1_clk_softc *sc, struct clk_fixed_def *clks,
    int nclks)
{
	int i, rv;

	for (i = 0; i < nclks; i++) {
		rv = clknode_fixed_register(sc->clkdom, clks + i);
		if (rv != 0)
			panic("clknode_fixed_register failed: %d", rv);
	}
}



static void
rp1_clk_register_clocks(device_t dev)
{
	struct rp1_clk_softc *sc;

	sc = device_get_softc(dev);

	sc->clkdom = clkdom_create(dev);
	if (sc->clkdom == NULL)
		panic("clkdom == NULL");

	clknode_fixed_register(sc->clkdom,  &rp1_clk_fixed_xosc);
	rp1_clk_init_fixed(sc, rp1_clk_fixed_clks, nitems(rp1_clk_fixed_clks));
	rp1_clk_init_gates(sc, rp1_clk_gate_clks, nitems(rp1_clk_gate_clks));
	rp1_clk_init_composite(sc, rp1_std_clks, nitems(rp1_std_clks));
	rp1_clk_init_prim(sc, rp1_clk_prim_clks, nitems(rp1_clk_prim_clks));
	rp1_clk_init_sec(sc, rp1_clk_sec_clks, nitems(rp1_clk_sec_clks));
	rp1_clk_init_pll(sc, rp1_clk_pll_clks, nitems(rp1_clk_pll_clks));
	clkdom_finit(sc->clkdom);
	if (bootverbose)
		clkdom_dump(sc->clkdom);
}


static int
rp1_clk_clkdev_read_4(device_t dev, bus_addr_t addr, uint32_t *val)
{
	struct rp1_clk_softc *sc;

	sc = device_get_softc(dev);
	*val = bus_read_4(sc->mem_res, addr);
	return (0);
}

static int
rp1_clk_clkdev_write_4(device_t dev, bus_addr_t addr, uint32_t val)
{
	struct rp1_clk_softc *sc;

	sc = device_get_softc(dev);
	bus_write_4(sc->mem_res, addr, val);
	return (0);
}

static int
rp1_clk_clkdev_modify_4(device_t dev, bus_addr_t addr, uint32_t clear_mask,
    uint32_t set_mask)
{
	struct rp1_clk_softc *sc;
	uint32_t reg;

	sc = device_get_softc(dev);
	reg = bus_read_4(sc->mem_res, addr);
	reg &= ~clear_mask;
	reg |= set_mask;
	bus_write_4(sc->mem_res, addr, reg);
	return (0);
}

static void
rp1_clk_clkdev_device_lock(device_t dev)
{
	struct rp1_clk_softc *sc;

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);
}

static void
rp1_clk_clkdev_device_unlock(device_t dev)
{
	struct rp1_clk_softc *sc;

	sc = device_get_softc(dev);
	mtx_unlock(&sc->mtx);
}

static int
rp1_clk_detach(device_t dev)
{

	device_printf(dev, "Error: Clock driver cannot be detached\n");
	return (EBUSY);
}

static int
rp1_clk_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data != 0) {
		device_set_desc(dev, "RP1 Clock Driver");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
rp1_clk_attach(device_t dev)
{
	struct rp1_clk_softc *sc;
	clk_t clk;
	int  rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);

	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0,
	    RF_ACTIVE);
	if (!sc->mem_res) {
		device_printf(dev, "cannot allocate memory resource\n");
		rv = ENXIO;
		goto fail;
	}

	rv = clk_get_by_ofw_index(sc->dev, sc->node, 0, &clk);
	if (rv != 0) {
		device_printf(dev, "Cannot get parent clock: %d\n", rv);
		goto fail;
	}

	/* Get real name of xosc */
	rp1_clk_fixed_xosc.clkdef.parent_names[0] = clk_get_name(clk);
	clk_release(clk);

	rp1_clk_register_clocks(dev);

	clk_set_assigned(dev, sc->node);

	return (0);

fail:
	if (sc->mem_res)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);

	return (rv);
}


static device_method_t rp1_clk_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rp1_clk_probe),
	DEVMETHOD(device_attach,	rp1_clk_attach),
	DEVMETHOD(device_detach,	rp1_clk_detach),

	/* Clkdev interface*/
	DEVMETHOD(clkdev_read_4,	rp1_clk_clkdev_read_4),
	DEVMETHOD(clkdev_write_4,	rp1_clk_clkdev_write_4),
	DEVMETHOD(clkdev_modify_4,	rp1_clk_clkdev_modify_4),
	DEVMETHOD(clkdev_device_lock,	rp1_clk_clkdev_device_lock),
	DEVMETHOD(clkdev_device_unlock,	rp1_clk_clkdev_device_unlock),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(rp1_clk, rp1_clk_driver, rp1_clk_methods,
    sizeof(struct rp1_clk_softc));
EARLY_DRIVER_MODULE(rp1_clk, simplebus, rp1_clk_driver, NULL, NULL,
    BUS_PASS_TIMER);
