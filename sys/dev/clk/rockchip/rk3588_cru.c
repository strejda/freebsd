/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021, 2022 Soren Schmidt <sos@deepcore.dk>
 * Copyright (c) 2023, Emmanuel	Vadot <manu@freebsd.org>
 * Copyright (c) 2025, Michal Meloun <mmel@freebsd.org>
 *
 * Redistribution and use in source and	binary forms, with or without
 * modification, are permitted provided	that the following conditions
 * are met:
 * 1. Redistributions of source	code must retain the above copyright
 *    notice, this list	of conditions and the following	disclaimer.
 * 2. Redistributions in binary	form must reproduce the	above copyright
 *    notice, this list	of conditions and the following	disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS	SOFTWARE IS PROVIDED BY	THE AUTHOR ``AS	IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A	PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR	BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL,	EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS	OF USE,	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF	LIABILITY, WHETHER IN CONTRACT,	STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH	DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <machine/bus.h>

#include <dev/fdt/simplebus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/clk/clk_div.h>
#include <dev/clk/clk_fixed.h>
#include <dev/clk/clk_mux.h>

#include <dev/clk/rockchip/rk_cru.h>
#include <contrib/device-tree/include/dt-bindings/clock/rockchip,rk3588-cru.h>
#include <contrib/device-tree/include/dt-bindings/reset/rockchip,rk3588-cru.h>

/*
	TODO:
	 - Implement multiple dividers for armclk
	 - accept rounding flags for all clocks
*/

#define	CRU_BASE			0x00000000
#define	PHP_CRU_BASE			0x00008000
#define	PMU_CRU_BASE			0x00030000
#define	BC0_CRU_BASE			0x00050000
#define	BC1_CRU_BASE			0x00052000
#define	DSU_CRU_BASE			0x00058000

#define	CRU_CLKSEL_CON(x)		(CRU_BASE + 0x300 + 4 * (x)) /* 178 regs */
#define	CRU_CLKGATE_CON(x)		(CRU_BASE + 0x800 + 4 * (x)) /* 78 regs */

/* Some  registers */
#define	CRU_MODE_CON0			0x280
#define	BC0_CRU_MODE_CON0		(BC0_CRU_BASE +	0x280)
#define	BC1_CRU_MODE_CON0		(BC1_CRU_BASE +	0x280)
#define	DSU_CRU_MODE_CON0		(DSU_CRU_BASE +	0x280)
#define	CRU_V0PLL_CON0			0x150

/* Relative registers within blocks */
#define	PMU(x)				((PMU_CRU_BASE / 4) + (x))
#define	PHP(x)				((PHP_CRU_BASE / 4) + (x))
#define	BC0(x)				((BC0_CRU_BASE / 4) + (x))
#define	BC1(x)				((BC1_CRU_BASE / 4) + (x))
#define	DSU(x)				((DSU_CRU_BASE / 4) + (x))
#define	PHYREF_ALT_GATE			(0x0C38	/ 4)

/* Full	complex	clock with gate	*/
#define	COMG(_id, _name, _pnames, _f, _o, _ds, _dw, _ms, _mw, _go, _gs) \
{									\
	.type =	RK_CLK_COMPOSITE,					\
	.clk.composite = &(struct rk_clk_composite_def)	{		\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = _pnames,				\
		.clkdef.parent_cnt = nitems(_pnames),			\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS | _f,		\
		.muxdiv_offset = CRU_CLKSEL_CON(_o),			\
		.mux_shift = _ms,					\
		.mux_width = _mw,					\
		.div_shift = _ds,					\
		.div_width = _dw,					\
		.gate_offset = CRU_CLKGATE_CON(_go),			\
		.gate_shift = _gs,					\
		.flags = RK_CLK_COMPOSITE_HAVE_MUX |			\
			 RK_CLK_COMPOSITE_HAVE_GATE			\
	},								\
}

/* Full	complex	clock with gate	and half divider */
#define	COMH(_id, _name, _pnames, _f, _o, _ds, _dw, _ms, _mw, _go, _gs) \
{									\
	.type =	RK_CLK_COMPOSITE,					\
	.clk.composite = &(struct rk_clk_composite_def)	{		\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = _pnames,				\
		.clkdef.parent_cnt = nitems(_pnames),			\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS | _f,		\
		.muxdiv_offset = CRU_CLKSEL_CON(_o),			\
		.mux_shift = _ms,					\
		.mux_width = _mw,					\
		.div_shift = _ds,					\
		.div_width = _dw,					\
		.gate_offset = CRU_CLKGATE_CON(_go),			\
		.gate_shift = _gs,					\
		.flags = RK_CLK_COMPOSITE_HAVE_MUX  |			\
			 RK_CLK_COMPOSITE_HAVE_GATE |			\
			 RK_CLK_COMPOSITE_DIV_HALF,			\
	},								\
}

/* Complex clock with without mux */
#define	DIVG(_id, _name, _pname, _f, _o, _ds, _dw, _go, _gs)		\
{									\
	.type =	RK_CLK_COMPOSITE,					\
	.clk.composite = &(struct rk_clk_composite_def)	{		\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = &(const char*){_pname},		\
		.clkdef.parent_cnt = 1,					\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS | _f,		\
		.muxdiv_offset = CRU_CLKSEL_CON(_o),			\
		.mux_shift = 0,						\
		.mux_width = 0,						\
		.div_shift = _ds,					\
		.div_width = _dw,					\
		.gate_offset = CRU_CLKGATE_CON(_go),			\
		.gate_shift = _gs,					\
		.flags = RK_CLK_COMPOSITE_HAVE_GATE			\
	},								\
}

/* Raw mux with reparent option */
#undef MUX
#define MUX(_id, _name, _pn,  _mo, _ms, _mw)				\
	MUXRAW(_id, _name, _pn, RK_CLK_MUX_REPARENT,			\
	    CRU_CLKSEL_CON(_mo), _ms, _mw)

/* Complex clock with gate but without divider.	*/
#define	MUXG(_id, _name, _pn, _f, _o, _ms, _mw, _go, _gs)		\
{									\
	.type =	RK_CLK_COMPOSITE,					\
	.clk.composite = &(struct rk_clk_composite_def) {		\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = _pn,				\
		.clkdef.parent_cnt = nitems(_pn),			\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS | _f,		\
		.muxdiv_offset = CRU_CLKSEL_CON(_o),			\
		.mux_shift = _ms,					\
		.mux_width = _mw,					\
		.div_shift = 0,						\
		.div_width = 0,						\
		.gate_offset = CRU_CLKGATE_CON(_go),			\
		.gate_shift = _gs,					\
		.flags = RK_CLK_COMPOSITE_HAVE_GATE |			\
			 RK_CLK_COMPOSITE_HAVE_MUX,			\
	},								\
}

/* Fractional rate multipier/divider. */
#define	FRAG(_id, _name, _pname, _f, _o, _go, _gs)			\
{									\
	.type =	RK_CLK_FRACT,						\
	.clk.fract = &(struct rk_clk_fract_def)	{			\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = (const char *[]){_pname},	\
		.clkdef.parent_cnt = 1,					\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS,		\
		.offset	= CRU_CLKSEL_CON(_o),				\
		.gate_offset = CRU_CLKGATE_CON(_go),			\
		.gate_shift = _gs,					\
		.flags = RK_CLK_FRACT_HAVE_GATE,			\
	},								\
}
#undef GATE
#define	GATE(_idx, _clkname, _pname, _o, _s)				\
{									\
	.id = _idx,							\
	.name =	_clkname,						\
	.parent_name = _pname,						\
	.offset	= CRU_CLKGATE_CON(_o),					\
	.shift = _s,							\
	.flags = CLK_NODE_STATIC_STRINGS,				\
}
#define	GATN(_idx, _clkname, _pname, _o, _s)				\
{									\
	.id = _idx,							\
	.name =	_clkname,						\
	.parent_name = _pname,						\
	.offset	= CRU_CLKGATE_CON(_o),					\
	.shift = _s,							\
	.flags = CLK_NODE_STATIC_STRINGS | CLK_NODE_CANNOT_STOP,	\
}

/* Linked gate */
#define	GATL(_idx, _clkname, _pname, _linkname, _o, _s)		\
{									\
	.id = _idx,							\
	.name =	_clkname,						\
	.parent_name = _pname,						\
	.offset	= CRU_CLKGATE_CON(_o),					\
	.shift = _s,							\
	.flags = CLK_NODE_STATIC_STRINGS,				\
}

#define	RK_PLLRATE(_hz,	_m, _p, _s, _k)					\
{									\
	.freq =	_hz,							\
	.m = _m,							\
	.p = _p,							\
	.s = _s,							\
	.k = _k,							\
}

/* PLL clock */
#define	PLLI(_id, _name, _plists, _boff, _moff,	_mshift)		\
{									\
	.type =	 RK3588_CLK_PLL,					\
	.clk.pll = &(struct rk_clk_pll_def) {				\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = _plists,				\
		.clkdef.parent_cnt = nitems(_plists),			\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS,		\
		.base_offset = _boff,					\
		.mode_reg = _moff,					\
		.mode_shift = _mshift,					\
		.rates = rk_pllrates,					\
	},								\
}

#define	PLLF(_id, _name, _plists, _boff, _moff,	_mshift)		\
{									\
	.type =	 RK3588_CLK_PLL,					\
	.clk.pll = &(struct rk_clk_pll_def) {				\
		.clkdef.id = _id,					\
		.clkdef.name = _name,					\
		.clkdef.parent_names = _plists,				\
		.clkdef.parent_cnt = nitems(_plists),			\
		.clkdef.flags =	CLK_NODE_STATIC_STRINGS,		\
		.base_offset = _boff,					\
		.mode_reg = _moff,					\
		.mode_shift = _mshift,					\
		.rates = rk_pllrates,					\
		.flags = RK_CLK_PLL_FRACTIONAL,				\
	},								\
}

static struct rk_clk_pll_rate rk_pllrates[] = {
	/*               _mhz,  _m, _p, _s, _k */
	RK_PLLRATE(2520000000, 210,  2,  0, 0),
	RK_PLLRATE(2496000000, 208,  2,  0, 0),
	RK_PLLRATE(2472000000, 206,  2,  0, 0),
	RK_PLLRATE(2448000000, 204,  2,  0, 0),
	RK_PLLRATE(2424000000, 202,  2,  0, 0),
	RK_PLLRATE(2400000000, 200,  2,  0, 0),
	RK_PLLRATE(2376000000, 198,  2,  0, 0),
	RK_PLLRATE(2352000000, 196,  2,  0, 0),
	RK_PLLRATE(2328000000, 194,  2,  0, 0),
	RK_PLLRATE(2304000000, 192,  2,  0, 0),
	RK_PLLRATE(2280000000, 190,  2,  0, 0),
	RK_PLLRATE(2256000000, 376,  2,  1, 0),
	RK_PLLRATE(2232000000, 372,  2,  1, 0),
	RK_PLLRATE(2208000000, 368,  2,  1, 0),
	RK_PLLRATE(2184000000, 364,  2,  1, 0),
	RK_PLLRATE(2160000000, 360,  2,  1, 0),
	RK_PLLRATE(2136000000, 356,  2,  1, 0),
	RK_PLLRATE(2112000000, 352,  2,  1, 0),
	RK_PLLRATE(2088000000, 348,  2,  1, 0),
	RK_PLLRATE(2064000000, 344,  2,  1, 0),
	RK_PLLRATE(2040000000, 340,  2,  1, 0),
	RK_PLLRATE(2016000000, 336,  2,  1, 0),
	RK_PLLRATE(1992000000, 332,  2,  1, 0),
	RK_PLLRATE(1968000000, 328,  2,  1, 0),
	RK_PLLRATE(1944000000, 324,  2,  1, 0),
	RK_PLLRATE(1920000000, 320,  2,  1, 0),
	RK_PLLRATE(1896000000, 316,  2,  1, 0),
	RK_PLLRATE(1872000000, 312,  2,  1, 0),
	RK_PLLRATE(1848000000, 308,  2,  1, 0),
	RK_PLLRATE(1824000000, 304,  2,  1, 0),
	RK_PLLRATE(1800000000, 300,  2,  1, 0),
	RK_PLLRATE(1776000000, 296,  2,  1, 0),
	RK_PLLRATE(1752000000, 292,  2,  1, 0),
	RK_PLLRATE(1728000000, 288,  2,  1, 0),
	RK_PLLRATE(1704000000, 284,  2,  1, 0),
	RK_PLLRATE(1680000000, 280,  2,  1, 0),
	RK_PLLRATE(1656000000, 276,  2,  1, 0),
	RK_PLLRATE(1632000000, 272,  2,  1, 0),
	RK_PLLRATE(1608000000, 268,  2,  1, 0),
	RK_PLLRATE(1584000000, 264,  2,  1, 0),
	RK_PLLRATE(1560000000, 260,  2,  1, 0),
	RK_PLLRATE(1536000000, 256,  2,  1, 0),
	RK_PLLRATE(1512000000, 252,  2,  1, 0),
	RK_PLLRATE(1488000000, 248,  2,  1, 0),
	RK_PLLRATE(1464000000, 244,  2,  1, 0),
	RK_PLLRATE(1440000000, 240,  2,  1, 0),
	RK_PLLRATE(1416000000, 236,  2,  1, 0),
	RK_PLLRATE(1392000000, 232,  2,  1, 0),
	RK_PLLRATE(1320000000, 220,  2,  1, 0),
	RK_PLLRATE(1200000000, 200,  2,  1, 0),
	RK_PLLRATE(1188000000, 198,  2,  1, 0),
	RK_PLLRATE(1100000000, 550,  3,  2, 0),
	RK_PLLRATE(1008000000, 336,  2,  2, 0),
	RK_PLLRATE(1000000000, 500,  3,  2, 0),
	RK_PLLRATE(983040000,  655,  4,  2, 23592),
	RK_PLLRATE(955520000,  477,  3,  2, 49806),
	RK_PLLRATE(903168000,  903,  6,  2, 11009),
	RK_PLLRATE(900000000,  300,  2,  2, 0),
	RK_PLLRATE(850000000,  425,  3,  2, 0),
	RK_PLLRATE(816000000,  272,  2,  2, 0),
	RK_PLLRATE(786432000,  262,  2,  2, 9437),
	RK_PLLRATE(786000000,  131,  1,  2, 0),
	RK_PLLRATE(785560000,  392,  3,  2, 51117),
	RK_PLLRATE(722534400,  963,  8,  2, 24850),
	RK_PLLRATE(600000000,  200,  2,  2, 0),
	RK_PLLRATE(594000000,  198,  2,  2, 0),
	RK_PLLRATE(408000000,  272,  2,  3, 0),
	RK_PLLRATE(312000000,  208,  2,  3, 0),
	RK_PLLRATE(216000000,  288,  2,  4, 0),
	RK_PLLRATE(100000000,  400,  3,  5, 0),
	RK_PLLRATE(96000000,   256,  2,  5, 0),
	{},
};

static struct rk_clk_armclk_rates cpul_rates[] = {
	{2208000000, 1},
	{2184000000, 1},
	{2088000000, 1},
	{2040000000, 1},
	{2016000000, 1},
	{1992000000, 1},
	{1896000000, 1},
	{1800000000, 1},
	{1704000000, 0},
	{1608000000, 0},
	{1584000000, 0},
	{1560000000, 0},
	{1536000000, 0},
	{1512000000, 0},
	{1488000000, 0},
	{1464000000, 0},
	{1440000000, 0},
	{1416000000, 0},
	{1392000000, 0},
	{1368000000, 0},
	{1344000000, 0},
	{1320000000, 0},
	{1296000000, 0},
	{1272000000, 0},
	{1248000000, 0},
	{1224000000, 0},
	{1200000000, 0},
	{1104000000, 0},
	{1008000000, 0},
	{912000000, 0},
	{816000000, 0},
	{696000000, 0},
	{600000000, 0},
	{408000000, 0},
	{312000000, 0},
	{216000000, 0},
	{96000000, 0},
};


static struct rk_clk_armclk_rates cpubc0_rates[]  = {
	{2496000000, 1},
	{2400000000, 1},
	{2304000000, 1},
	{2208000000, 1},
	{2184000000, 1},
	{2088000000, 1},
	{2040000000, 1},
	{2016000000, 1},
	{1992000000, 1},
	{1896000000, 1},
	{1800000000, 1},
	{1704000000, 0},
	{1608000000, 0},
	{1584000000, 0},
	{1560000000, 0},
	{1536000000, 0},
	{1512000000, 0},
	{1488000000, 0},
	{1464000000, 0},
	{1440000000, 0},
	{1416000000, 0},
	{1392000000, 0},
	{1368000000, 0},
	{1344000000, 0},
	{1320000000, 0},
	{1296000000, 0},
	{1272000000, 0},
	{1248000000, 0},
	{1224000000, 0},
	{1200000000, 0},
	{1104000000, 0},
	{1008000000, 0},
	{912000000, 0},
	{816000000, 0},
	{696000000, 0},
	{600000000, 0},
	{408000000, 0},
	{312000000, 0},
	{216000000, 0},
	{96000000, 0},
};

static struct rk_clk_armclk_rates cpubc1_rates[] ={
	{2496000000, 1},
	{2400000000, 1},
	{2304000000, 1},
	{2208000000, 1},
	{2184000000, 1},
	{2088000000, 1},
	{2040000000, 1},
	{2016000000, 1},
	{1992000000, 1},
	{1896000000, 1},
	{1800000000, 1},
	{1704000000, 0},
	{1608000000, 0},
	{1584000000, 0},
	{1560000000, 0},
	{1536000000, 0},
	{1512000000, 0},
	{1488000000, 0},
	{1464000000, 0},
	{1440000000, 0},
	{1416000000, 0},
	{1392000000, 0},
	{1368000000, 0},
	{1344000000, 0},
	{1320000000, 0},
	{1296000000, 0},
	{1272000000, 0},
	{1248000000, 0},
	{1224000000, 0},
	{1200000000, 0},
	{1104000000, 0},
	{1008000000, 0},
	{912000000, 0},
	{816000000, 0},
	{696000000, 0},
	{600000000, 0},
	{408000000, 0},
	{312000000, 0},
	{216000000, 0},
	{96000000, 0},
};


/* Parent clock	defines	*/
PLIST(m_pll_p)				= { "xin24m", "xin32k" };
PLIST(m_armclkl_p)			= { "xin24m", "gpll", "lpll" };
PLIST(m_armclkb01_p)			= { "xin24m", "gpll", "b0pll",};
PLIST(m_armclkb23_p)			= { "xin24m", "gpll", "b1pll",};
PLIST(b0pll_b1pll_lpll_gpll_p)		= { "b0pll", "b1pll", "lpll", "gpll" };
PLIST(gpll_24m_p)			= { "gpll", "xin24m" };
PLIST(gpll_aupll_p)			= { "gpll", "aupll" };
PLIST(gpll_lpll_p)			= { "gpll", "lpll" };
PLIST(gpll_cpll_p)			= { "gpll", "cpll" };
PLIST(gpll_spll_p)			= { "gpll", "spll" };
PLIST(gpll_cpll_24m_p)			= { "gpll", "cpll", "xin24m"};
PLIST(gpll_cpll_aupll_p)		= { "gpll", "cpll", "aupll"};
PLIST(gpll_cpll_npll_p)			= { "gpll", "cpll", "npll"};
PLIST(gpll_cpll_npll_v0pll_p)		= { "gpll", "cpll", "npll", "v0pll"};
PLIST(gpll_cpll_24m_spll_p)		= { "gpll", "cpll", "xin24m", "spll" };
PLIST(gpll_cpll_aupll_spll_p)		= { "gpll", "cpll", "aupll", "spll" };
PLIST(gpll_cpll_aupll_npll_p)		= { "gpll", "cpll", "aupll", "npll" };
PLIST(gpll_cpll_v0pll_aupll_p)		= { "gpll", "cpll", "v0pll", "aupll" };
PLIST(gpll_cpll_v0pll_spll_p)		= { "gpll", "cpll", "v0pll", "spll" };
PLIST(gpll_cpll_aupll_npll_spll_p)	= { "gpll", "cpll", "aupll", "npll", "spll" };
PLIST(gpll_cpll_dmyaupll_npll_spll_p)	= { "gpll", "cpll", "dummy_aupll", "npll", "spll" };
PLIST(gpll_cpll_npll_aupll_spll_p)	= { "gpll", "cpll", "npll", "aupll", "spll" };
PLIST(gpll_cpll_npll_1000m_p)		= { "gpll", "cpll", "npll", "clk_1000m_src" };
PLIST(m_24m_spll_gpll_cpll_p)		= { "xin24m", "spll", "gpll", "cpll" };
PLIST(m_24m_32k_p)			= { "xin24m", "xin32k" };
PLIST(m_24m_100m_p)			= { "xin24m", "clk_100m_src" };
PLIST(m_200m_100m_p)			= { "clk_200m_src", "clk_100m_src" };
PLIST(m_100m_50m_24m_p)			= { "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(m_150m_50m_24m_p)			= { "clk_150m_src", "clk_50m_src", "xin24m" };
PLIST(m_150m_100m_24m_p)		= { "clk_150m_src", "clk_100m_src", "xin24m" };
PLIST(m_200m_150m_24m_p)		= { "clk_200m_src", "clk_150m_src", "xin24m" };
PLIST(m_150m_100m_50m_24m_p)		= { "clk_150m_src", "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(m_200m_100m_50m_24m_p)		= { "clk_200m_src", "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(m_300m_200m_100m_24m_p)		= { "clk_300m_src", "clk_200m_src", "clk_100m_src", "xin24m" };
PLIST(m_700m_400m_200m_24m_p)		= { "clk_700m_src", "clk_400m_src", "clk_200m_src", "xin24m" };
PLIST(m_500m_250m_100m_24m_p)		= { "clk_500m_src", "clk_250m_src", "clk_100m_src", "xin24m" };
PLIST(m_500m_300m_100m_24m_p)		= { "clk_500m_src", "clk_300m_src", "clk_100m_src", "xin24m" };
PLIST(m_400m_200m_100m_24m_p)		= { "clk_400m_src", "clk_200m_src", "clk_100m_src", "xin24m" };
PLIST(clk_i2s2_2ch_p)			= { "clk_i2s2_2ch_src",	"clk_i2s2_2ch_frac", "i2s2_mclkin", "xin12m" };
PLIST(i2s2_2ch_mclkout_p)		= { "mclk_i2s2_2ch", "xin12m" };
PLIST(clk_i2s3_2ch_p)			= { "clk_i2s3_2ch_src",	"clk_i2s3_2ch_frac", "i2s3_mclkin", "xin12m" };
PLIST(i2s3_2ch_mclkout_p)		= { "mclk_i2s3_2ch", "xin12m" };
PLIST(clk_i2s0_8ch_tx_p)		= { "clk_i2s0_8ch_tx_src", "clk_i2s0_8ch_tx_frac", "i2s0_mclkin", "xin12m" };
PLIST(clk_i2s0_8ch_rx_p)		= { "clk_i2s0_8ch_rx_src", "clk_i2s0_8ch_rx_frac", "i2s0_mclkin", "xin12m" };
PLIST(i2s0_8ch_mclkout_p)		= { "mclk_i2s0_8ch_tx",	"mclk_i2s0_8ch_rx", "xin12m" };
PLIST(clk_i2s1_8ch_tx_p)		= { "clk_i2s1_8ch_tx_src", "clk_i2s1_8ch_tx_frac", "i2s1_mclkin", "xin12m" };
PLIST(clk_i2s1_8ch_rx_p)		= { "clk_i2s1_8ch_rx_src", "clk_i2s1_8ch_rx_frac", "i2s1_mclkin", "xin12m" };
PLIST(i2s1_8ch_mclkout_p)		= { "mclk_i2s1_8ch_tx",	"mclk_i2s1_8ch_rx", "xin12m" };
PLIST(clk_i2s4_8ch_tx_p)		= { "clk_i2s4_8ch_tx_src", "clk_i2s4_8ch_tx_frac", "i2s4_mclkin", "xin12m" };
PLIST(clk_i2s5_8ch_tx_p)		= { "clk_i2s5_8ch_tx_src", "clk_i2s5_8ch_tx_frac", "i2s5_mclkin", "xin12m" };
PLIST(clk_i2s6_8ch_tx_p)		= { "clk_i2s6_8ch_tx_src", "clk_i2s6_8ch_tx_frac", "i2s6_mclkin", "xin12m" };
PLIST(clk_i2s6_8ch_rx_p)		= { "clk_i2s6_8ch_rx_src", "clk_i2s6_8ch_rx_frac", "i2s6_mclkin", "xin12m" };
PLIST(i2s6_8ch_mclkout_p)		= { "mclk_i2s6_8ch_tx",	"mclk_i2s6_8ch_rx", "xin12m" };
PLIST(clk_i2s7_8ch_rx_p)		= { "clk_i2s7_8ch_rx_src", "clk_i2s7_8ch_rx_frac", "i2s7_mclkin", "xin12m" };
PLIST(clk_i2s8_8ch_tx_p)		= { "clk_i2s8_8ch_tx_src", "clk_i2s8_8ch_tx_frac", "i2s8_mclkin", "xin12m" };
PLIST(clk_i2s9_8ch_rx_p)		= { "clk_i2s9_8ch_rx_src", "clk_i2s9_8ch_rx_frac", "i2s9_mclkin", "xin12m" };
PLIST(clk_i2s10_8ch_rx_p)		= { "clk_i2s10_8ch_rx_src", "clk_i2s10_8ch_rx_frac", "i2s10_mclkin", "xin12m" };
PLIST(clk_spdif0_p)			= { "clk_spdif0_src", "clk_spdif0_frac", "xin12m" };
PLIST(clk_spdif1_p)			= { "clk_spdif1_src", "clk_spdif1_frac", "xin12m" };
PLIST(clk_spdif2_dp0_p)			= { "clk_spdif2_dp0_src", "clk_spdif2_dp0_frac", "xin12m" };
PLIST(clk_spdif3_p)			= { "clk_spdif3_src", "clk_spdif3_frac", "xin12m" };
PLIST(clk_spdif4_p)			= { "clk_spdif4_src", "clk_spdif4_frac", "xin12m" };
PLIST(clk_spdif5_dp1_p)			= { "clk_spdif5_dp1_src", "clk_spdif5_dp1_frac", "xin12m" };
PLIST(clk_uart0_p)			= { "clk_uart0_src", "clk_uart0_frac", "xin24m"	};
PLIST(clk_uart1_p)			= { "clk_uart1_src", "clk_uart1_frac", "xin24m"	};
PLIST(clk_uart2_p)			= { "clk_uart2_src", "clk_uart2_frac", "xin24m"	};
PLIST(clk_uart3_p)			= { "clk_uart3_src", "clk_uart3_frac", "xin24m"	};
PLIST(clk_uart4_p)			= { "clk_uart4_src", "clk_uart4_frac", "xin24m"	};
PLIST(clk_uart5_p)			= { "clk_uart5_src", "clk_uart5_frac", "xin24m"	};
PLIST(clk_uart6_p)			= { "clk_uart6_src", "clk_uart6_frac", "xin24m"	};
PLIST(clk_uart7_p)			= { "clk_uart7_src", "clk_uart7_frac", "xin24m"	};
PLIST(clk_uart8_p)			= { "clk_uart8_src", "clk_uart8_frac", "xin24m"	};
PLIST(clk_uart9_p)			= { "clk_uart9_src", "clk_uart9_frac", "xin24m"	};
PLIST(clk_gmac0_ptp_ref_p)		= { "cpll", "clk_gmac0_ptpref_io" };
PLIST(clk_gmac1_ptp_ref_p)		= { "cpll", "clk_gmac1_ptpref_io" };
PLIST(clk_hdmirx_aud_p)			= { "clk_hdmirx_aud_src", "clk_hdmirx_aud_frac"	};
PLIST(aclk_hdcp1_root_p)		= { "gpll", "cpll", "clk_hdmitrx_refsrc" };
PLIST(aclk_vop_sub_src_p)		= { "aclk_vop_root", "aclk_vop_div2_src" };
PLIST(dclk_vop0_p)			= { "dclk_vop0_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1"	};
PLIST(dclk_vop1_p)			= { "dclk_vop1_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1"	};
PLIST(dclk_vop2_p)			= { "dclk_vop2_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1"	};
PLIST(pmu_200m_100m_p)			= { "clk_pmu1_200m_src", "clk_pmu1_100m_src" };
PLIST(pmu_300m_24m_p)			= { "clk_300m_src", "xin24m" };
PLIST(pmu_400m_24m_p)			= { "clk_400m_src", "xin24m" };
PLIST(pmu_100m_50m_24m_src_p)		= { "clk_pmu1_100m_src", "clk_pmu1_50m_src", "xin24m" };
PLIST(pmu_24m_32k_100m_src_p)		= { "xin24m", "xin32k",	"clk_pmu1_100m_src" };
PLIST(hclk_pmu1_root_p)			= { "clk_pmu1_200m_src", "clk_pmu1_100m_src", "clk_pmu1_50m_src", "xin24m" };
PLIST(hclk_pmu_cm0_root_p)		= { "clk_pmu1_400m_src", "clk_pmu1_200m_src", "clk_pmu1_100m_src", "xin24m" };
PLIST(mclk_pdm0_p)			= { "clk_pmu1_300m_src", "clk_pmu1_200m_src" };
PLIST(m_24m_ppll_spll_p)		= { "xin24m", "ppll", "spll" };
PLIST(m_24m_ppll_p)			= { "xin24m", "ppll" };
PLIST(clk_ref_pipe_phy0_p)		= { "clk_ref_pipe_phy0_osc_src", "clk_ref_pipe_phy0_pll_src" };
PLIST(clk_ref_pipe_phy1_p)		= { "clk_ref_pipe_phy1_osc_src", "clk_ref_pipe_phy1_pll_src" };
PLIST(clk_ref_pipe_phy2_p)		= { "clk_ref_pipe_phy2_osc_src", "clk_ref_pipe_phy2_pll_src" };


/* CLOCKS */
static struct rk_clk rk3588_clks[] = {
	/* External clocks */
	LINK("xin24m"),
	LINK("scmi_cclk_sd"),

	/* External clock inputs */
	FRATE(0, "i2s0_mclkin",  0),
	FRATE(0, "i2s1_mclkin",  0),
	FRATE(0, "i2s2_mclkin",  0),
	FRATE(0, "i2s3_mclkin",  0),
	FRATE(0, "i2s4_mclkin",  0),
	FRATE(0, "i2s5_mclkin",  0),
	FRATE(0, "i2s6_mclkin",  0),
	FRATE(0, "i2s7_mclkin",  0),
	FRATE(0, "i2s8_mclkin",  0),
	FRATE(0, "i2s9_mclkin",  0),
	FRATE(0, "i2s10_mclkin", 0),

	FRATE(0, "clk_gmac0_ptpref_io", 0),
	FRATE(0, "clk_gmac1_ptpref_io", 0),

	FRATE(0, "dummy_aupll", 0),



	FRATE(0, "clk_pipephy0_pipe_i", 0),
	FRATE(0, "clk_pipephy1_pipe_i", 0),
	FRATE(0, "clk_pipephy2_pipe_i", 0),
	FRATE(0, "clk_pipe30phy_pipe0_i", 0),
	FRATE(0, "clk_pipe30phy_pipe1_i", 0),
	FRATE(0, "clk_pipe30phy_pipe2_i", 0),

	LINK("clk_hdmiphy_pixel0"),
	LINK("clk_hdmiphy_pixel1"),

	/* TODO - mmc clocks are fixed divider by 2 with phase controll */
	FFACT(SCLK_SDIO_DRV, "sdio_drv", "cclk_src_sdio",	 1, 2),
	FFACT(SCLK_SDIO_SAMPLE, "sdio_sample", "cclk_src_sdio",	 1, 2),
	FFACT(SCLK_SDMMC_DRV, "sdmmc_drv", "scmi_cclk_sd",	 1, 2),
	FFACT(SCLK_SDMMC_SAMPLE, "sdmmc_sample", "scmi_cclk_sd", 1, 2),

	/* PLL's */
	PLLI(PLL_B0PLL,	"b0pll", m_pll_p, BC0_CRU_BASE + 0x000,	BC0_CRU_MODE_CON0, 0),
	PLLI(PLL_B1PLL,	"b1pll", m_pll_p, BC1_CRU_BASE + 0x000,	BC1_CRU_MODE_CON0, 0),
	PLLI(PLL_LPLL,	 "lpll", m_pll_p, DSU_CRU_BASE + 0x040,	DSU_CRU_MODE_CON0, 0),
	PLLF(PLL_V0PLL,	"v0pll", m_pll_p, 0x160,		CRU_MODE_CON0,	   4),
	PLLF(PLL_AUPLL,	"aupll", m_pll_p, 0x180,		CRU_MODE_CON0,	   6),
	PLLF(PLL_CPLL,	 "cpll", m_pll_p, 0x1A0,		CRU_MODE_CON0,	   8),
	PLLF(PLL_GPLL,	 "gpll", m_pll_p, 0x1C0,		CRU_MODE_CON0,	   2),
	PLLF(PLL_NPLL,	 "npll", m_pll_p, 0x1E0,		CRU_MODE_CON0,	   0),
	PLLI(PLL_PPLL,	 "ppll", m_pll_p, PHP_CRU_BASE + 0x200,	CRU_MODE_CON0,	   10),

	FFACT(0, "xin12m", "xin24m", 1,	2),
	FFACT(0, "aclk_vop_div2_src", "aclk_vop_root", 1, 2), // ??


	COMG(CLK_50M_SRC, "clk_50m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			0,  0, 5,  5, 1,  0, 0),
	COMG(CLK_100M_SRC,"clk_100m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			0,  6, 5, 11, 1,  0, 1),

	COMG(CLK_150M_SRC, "clk_150m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			1,  0, 5,  5, 1,  0, 2),
	COMG(CLK_200M_SRC, "clk_200m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			1,  6, 5, 11, 1,  0, 3),

	COMG(CLK_250M_SRC, "clk_250m_src", gpll_cpll_p,
		 CLK_NODE_CANNOT_STOP,			2,  0, 5,  5, 1,  0, 4),
	COMG(CLK_300M_SRC, "clk_300m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			2,  6, 5, 11, 1,  0, 5),

	COMG(CLK_350M_SRC, "clk_350m_src", gpll_spll_p,
		CLK_NODE_CANNOT_STOP,			3,  0, 5,  5, 1,  0, 6),
	COMG(CLK_400M_SRC, "clk_400m_src", gpll_cpll_p,
		 CLK_NODE_CANNOT_STOP,			3,  6, 5, 11, 1,  0, 7),

	COMH(CLK_450M_SRC, "clk_450m_src", gpll_cpll_p,
		0,					4,  0, 5,  5, 1,  0, 8),
	COMG(CLK_500M_SRC, "clk_500m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			4,  6, 5, 11, 1,  0, 9),

	COMG(CLK_600M_SRC, "clk_600m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			5,  0, 5,  5, 1,  0, 10),
	COMG(CLK_650M_SRC, "clk_650m_src", gpll_lpll_p,
		0,					5,  6, 5, 11, 1,  0, 11),

	COMG(CLK_700M_SRC, "clk_700m_src", gpll_spll_p,
		CLK_NODE_CANNOT_STOP,			6,  0, 5,  5, 1,  0, 12),
	COMG(CLK_800M_SRC, "clk_800m_src", gpll_aupll_p,
		CLK_NODE_CANNOT_STOP,			6,  6, 5, 11, 1,  0, 13),

	COMH(CLK_1000M_SRC, "clk_1000m_src", gpll_cpll_npll_v0pll_p,
		CLK_NODE_CANNOT_STOP,			7,  0, 5,  5, 1,  0, 14),
	COMG(CLK_1200M_SRC,"clk_1200m_src", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,			7,  6, 5, 11, 1,  0, 15),

	COMG(ACLK_TOP_ROOT, "aclk_top_root", gpll_cpll_aupll_p,
		CLK_NODE_CANNOT_STOP,			8,  0, 5,  5, 2,  1, 0),
	MUXG(PCLK_TOP_ROOT,"pclk_top_root", m_100m_50m_24m_p,
		 CLK_NODE_CANNOT_STOP,			8,	   7, 2,  1, 2),
	COMG(ACLK_LOW_TOP_ROOT,	"aclk_low_top_root", gpll_cpll_aupll_p,
		 CLK_NODE_CANNOT_STOP,			8,  0, 5, 14, 1,  1, 3),

	MUXG(ACLK_TOP_M300_ROOT, "aclk_top_m300_root", m_300m_200m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,			9,	   0, 2,  1, 4),
	MUXG(ACLK_TOP_M500_ROOT, "aclk_top_m500_root", m_500m_300m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,			9,	   2, 2,  1, 5),
	MUXG(ACLK_TOP_M400_ROOT, "aclk_top_m400_root", m_400m_200m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,			9,	   4, 2,  1, 6),
	MUXG(ACLK_TOP_S200_ROOT, "aclk_top_s200_root", m_200m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,			9,	   6, 2,  1, 7),
	MUXG(ACLK_TOP_S400_ROOT, "aclk_top_s400_root", m_400m_200m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,			9,	   8, 2,  1, 8),

	COMG(MCLK_GMAC0_OUT, "mclk_gmac0_out", gpll_cpll_p,
		0,				       15,  0, 7,  7, 1,  5, 3),
	COMG(REFCLKO25M_ETH0_OUT, "refclko25m_eth0_out", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		       15,  8, 7, 15, 1,  5, 4),
	COMG(REFCLKO25M_ETH1_OUT, "refclko25m_eth1_out", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		       16,  0, 7,  7, 1,  5, 5),
	COMG(CLK_CIFOUT_OUT, "clk_cifout_out_v",	 gpll_cpll_24m_spll_p,
		0,				       17,  0, 8,  8, 2,  5, 6),
	COMG(CLK_MIPI_CAMARAOUT_M0, "clk_mipi_camaraout_m0", m_24m_spll_gpll_cpll_p,
		0,				       18,  0, 8,  8, 2,  5, 9),
	COMG(CLK_MIPI_CAMARAOUT_M1, "clk_mipi_camaraout_m1", m_24m_spll_gpll_cpll_p,
		0,				       19,  0, 8,  8, 2,  5, 10),
	COMG(CLK_MIPI_CAMARAOUT_M2, "clk_mipi_camaraout_m2", m_24m_spll_gpll_cpll_p,
		0,				       20,  0, 8,  8, 2,  5, 11),
	COMG(CLK_MIPI_CAMARAOUT_M3, "clk_mipi_camaraout_m3", m_24m_spll_gpll_cpll_p,
		0,				       21,  0, 8,  8, 2,  5, 12),
	COMG(CLK_MIPI_CAMARAOUT_M4, "clk_mipi_camaraout_m4", m_24m_spll_gpll_cpll_p,
		0,				       22,  0, 8,  8, 2,  5, 13),

	MUXG(HCLK_AUDIO_ROOT, "hclk_audio_root", m_200m_100m_50m_24m_p,
		0,				       24,	   0, 2,  7, 0),
	MUXG(PCLK_AUDIO_ROOT, "pclk_audio_root", m_100m_50m_24m_p,
		0,				       24,	   2, 2,  7, 1),
	COMG(CLK_I2S0_8CH_TX_SRC, "clk_i2s0_8ch_tx_src", gpll_aupll_p,
		0,				       24,  4, 5,  9, 1,  7, 5),

	FRAG(CLK_I2S0_8CH_TX_FRAC, "clk_i2s0_8ch_tx_frac", "clk_i2s0_8ch_tx_src",
		0,				       25,		  7, 6),

	MUX(CLK_I2S0_8CH_TX, "clk_i2s0_8ch_tx",	clk_i2s0_8ch_tx_p,
						       26,	   0, 2),
	COMG(CLK_I2S0_8CH_RX_SRC, "clk_i2s0_8ch_rx_src", m_24m_spll_gpll_cpll_p,
		0,				       26,  2, 5,  7, 2,  7, 8),

	FRAG(CLK_I2S0_8CH_RX_FRAC, "clk_i2s0_8ch_rx_frac", "clk_i2s0_8ch_rx_src",
		0,				       27,		  7, 9),

	MUX(CLK_I2S0_8CH_RX, "clk_i2s0_8ch_rx",	clk_i2s0_8ch_rx_p,
						       28,	   0, 2),
	MUX(I2S0_8CH_MCLKOUT, "i2s0_8ch_mclkout", i2s0_8ch_mclkout_p,
						       28,	   2, 2),
	COMG(CLK_I2S2_2CH_SRC, "clk_i2s2_2ch_src", gpll_aupll_p,
		0,				       28,  4, 5,  9, 1,  7, 14),

	FRAG(CLK_I2S2_2CH_FRAC,	"clk_i2s2_2ch_frac", "clk_i2s2_2ch_src",
		0,				       29,	  7, 15),

	MUX(CLK_I2S2_2CH, "clk_i2s2_2ch", clk_i2s2_2ch_p,
		 				       30,	   0, 2),
	MUX(I2S2_2CH_MCLKOUT, "i2s2_2ch_mclkout", i2s2_2ch_mclkout_p,
						       30,	   2, 1),
	COMG(CLK_I2S3_2CH_SRC, "clk_i2s3_2ch_src", gpll_aupll_p,
		0,				       30,  3, 5,  8, 1,  8, 1),

	FRAG(CLK_I2S3_2CH_FRAC,	"clk_i2s3_2ch_frac", "clk_i2s3_2ch_src",
		0,				       31,		  8, 2),
	MUX(CLK_I2S3_2CH, "clk_i2s3_2ch", clk_i2s3_2ch_p,
						       32,	   0, 2),
	MUX(I2S3_2CH_MCLKOUT, "i2s3_2ch_mclkout", i2s3_2ch_mclkout_p,
						       32,	   2, 1),
	COMG(MCLK_PDM1,	"clk_spdif0_src", gpll_aupll_p,
		0,				       32,  3, 5,  8, 1,  8, 15),

	FRAG(CLK_SPDIF0_FRAC, "clk_spdif0_frac", "clk_spdif0_src",
		0,				       33,		  9, 0),

	COMG(CLK_SPDIF1_SRC, "clk_spdif1_src", gpll_aupll_p,
		0,				       34,  2, 5,  7, 1,  9, 3),

	FRAG(CLK_SPDIF1_FRAC, "clk_spdif1_frac", "clk_spdif1_src",
		0,				       35,		  9, 4),

	MUX(CLK_SPDIF0,	"clk_spdif0", clk_spdif0_p,
						       34,	   0, 2),

	MUX(CLK_SPDIF1,	"clk_spdif1", clk_spdif1_p,
						       36,	   0, 2),
	COMG(MCLK_PDM1,	"mclk_pdm1", gpll_cpll_aupll_p,
		0,				       36,  2, 5,  7, 2,  9 ,7),

	COMG(ACLK_BUS_ROOT, "aclk_bus_root", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		       38,  0, 5,  5, 1, 10, 0),
	MUXG(CLK_I2C1, "clk_i2c1", m_200m_100m_p,
		0,				       38,	   6, 1, 11, 0),
	MUXG(CLK_I2C2, "clk_i2c2", m_200m_100m_p,
		0,				       38,	   7, 1, 11, 1),
	MUXG(CLK_I2C3, "clk_i2c3", m_200m_100m_p,
		0,				       38,	   8, 1, 11, 2),
	MUXG(CLK_I2C4, "clk_i2c4", m_200m_100m_p,
		0,				       38,	   9, 1, 11, 3),
	MUXG(CLK_I2C5, "clk_i2c5", m_200m_100m_p,
		0,				       38,	  10, 1, 11, 4),
	MUXG(CLK_I2C6, "clk_i2c6", m_200m_100m_p,
		0,				       38,	  11, 1, 11, 5),
	MUXG(CLK_I2C7, "clk_i2c7", m_200m_100m_p,
		0,				       38,	  12, 1, 11, 6),
	MUXG(CLK_I2C8, "clk_i2c8", m_200m_100m_p,
		0,				       38,	  13, 1, 11, 7),


	COMG(CLK_CAN0, "clk_can0", gpll_cpll_p,
		0,				       39,  0, 5,  5, 1, 11, 9),
	COMG(CLK_CAN1, "clk_can1", gpll_cpll_p,
		0,				       39,  6, 5, 11, 1, 11, 11),

	COMG(CLK_CAN2, "clk_can2", gpll_cpll_p,
		0,				       40,  0, 5,  5, 1, 11, 13),
	COMG(CLK_SARADC, "clk_saradc", gpll_24m_p,
		0,				       40,  6, 8, 14, 1, 11, 15),

	COMG(CLK_TSADC,	"clk_tsadc", gpll_24m_p,
		0,				       41,  0, 8,  8, 1, 12, 1),
	COMG(CLK_UART1_SRC, "clk_uart1_src", gpll_cpll_p,
		0,				       41,  9, 5, 14, 1, 12, 11),

	FRAG(CLK_UART1_FRAC, "clk_uart1_frac", "clk_uart1_src",
		0,				       42,		 12, 12),

	MUX(CLK_UART1, "clk_uart1", clk_uart1_p,
						       43,	   0, 2),
	COMG(CLK_UART2_SRC, "clk_uart2_src", gpll_cpll_p,
		0,				       43,  2, 5,  7, 1, 12, 14),

	FRAG(CLK_UART2_FRAC, "clk_uart2_frac", "clk_uart2_src",
		0,				       44,		 12, 15),

	MUX(CLK_UART2, "clk_uart2", clk_uart2_p,
						       45,	   0, 2),
	COMG(CLK_UART3_SRC, "clk_uart3_src", gpll_cpll_p,
		0,				       45,  2, 5,  7, 1, 13, 1),

	FRAG(CLK_UART3_FRAC, "clk_uart3_frac", "clk_uart3_src",
		0,				       46,		 13, 2),

	MUX(CLK_UART3, "clk_uart3", clk_uart3_p,
						       47,	   0, 2),
	COMG(CLK_UART4_SRC, "clk_uart4_src", gpll_cpll_p,
		0,				       47,  2, 5,  7, 1, 13, 4),

	FRAG(CLK_UART4_FRAC, "clk_uart4_frac", "clk_uart4_src",
		0,				       48,		 13, 5),

	MUX(CLK_UART4, "clk_uart4", clk_uart4_p,
						       49,	   0, 2),
	COMG(CLK_UART5_SRC, "clk_uart5_src", gpll_cpll_p,
		0,				       49,  2, 5,  7, 1, 13, 7),

	FRAG(CLK_UART5_FRAC, "clk_uart5_frac", "clk_uart5_src",
		0,				       50,		 13, 8),

	MUX(CLK_UART5, "clk_uart5", clk_uart5_p,
						       51,	   0, 2),
	COMG(CLK_UART6_SRC, "clk_uart6_src", gpll_cpll_p,
		0,				       51,  2, 5,  7, 1, 13, 10),


	FRAG(CLK_UART6_FRAC, "clk_uart6_frac", "clk_uart6_src",
		0,				       52,		 13, 11),

	MUX(CLK_UART6, "clk_uart6", clk_uart6_p,
						       53,	   0, 2),
	COMG(CLK_UART7_SRC, "clk_uart7_src", gpll_cpll_p,
		0,				       53,  2, 5,  7, 1, 13, 13),

	FRAG(CLK_UART7_FRAC, "clk_uart7_frac", "clk_uart7_src",
		0,				       54,		 13, 14),

	MUX(CLK_UART7, "clk_uart7", clk_uart7_p,
						       55,	   0, 2),
	COMG(CLK_UART8_SRC, "clk_uart8_src", gpll_cpll_p,
		0,				       55,  2, 5,  7, 1, 14, 0),

	FRAG(CLK_UART8_FRAC, "clk_uart8_frac", "clk_uart8_src",
		0,				       56,		 14, 1),

	MUX(CLK_UART8, "clk_uart8", clk_uart8_p,
						       57,	   0, 2),
	COMG(CLK_UART9_SRC, "clk_uart9_src", gpll_cpll_p,
		0,				       57,  2, 5,  7, 1, 14, 3),

	FRAG(CLK_UART9_FRAC, "clk_uart9_frac", "clk_uart9_src",
		0,				       58,		 14, 4),

	MUX(CLK_UART9, "clk_uart9", clk_uart9_p,
						       59,	   0, 2),
	MUXG(CLK_SPI0, "clk_spi0", m_200m_150m_24m_p,
		0,				       59,	   2, 2, 14, 11),
	MUXG(CLK_SPI1, "clk_spi1", m_200m_150m_24m_p,
		0,				       59,	   4, 2, 14, 12),
	MUXG(CLK_SPI2, "clk_spi2", m_200m_150m_24m_p,
		0,				       59,	   6, 2, 14, 13),
	MUXG(CLK_SPI3, "clk_spi3", m_200m_150m_24m_p,
		0,				       59,	   8, 2, 14, 14),
	MUXG(CLK_SPI4, "clk_spi4", m_200m_150m_24m_p,
		0,				       59,	  10, 2, 14, 15),
	MUXG(CLK_PWM1, "clk_pwm1", m_100m_50m_24m_p,
		0,				       59,	  12, 2, 15, 4),
	MUXG(CLK_PWM2, "clk_pwm2", m_100m_50m_24m_p,
		0,				       59,	  14, 2, 15, 7),

	MUXG(CLK_PWM3, "clk_pwm3", m_100m_50m_24m_p,
		0,				       60,	   0, 2, 15, 10),
	MUXG(CLK_BUS_TIMER_ROOT, "clk_bus_timer_root", m_24m_100m_p,
		0,				       60,	   2, 1,  5, 14),

	COMG(DBCLK_GPIO1, "dbclk_gpio1", m_24m_32k_p,
		0,				       60,  3, 5,  8, 1, 16, 15),
	COMG(DBCLK_GPIO2, "dbclk_gpio2", m_24m_32k_p,
		0,				       60,  9, 5, 14, 1, 17, 1),

	COMG(DBCLK_GPIO3, "dbclk_gpio3", m_24m_32k_p,
		0,				       61,  0, 5, 14, 1, 17, 3),
	COMG(DBCLK_GPIO4, "dbclk_gpio4", m_24m_32k_p,
		0,				       61,  6, 5, 11, 1, 17, 5),

	COMG(DCLK_DECOM, "dclk_decom", gpll_spll_p,
		0,				       62,  0, 5,  5, 1, 17, 8),

	COMG(ACLK_ISP1_ROOT, "aclk_isp1_root", gpll_cpll_aupll_spll_p,
		0,				       67,  0, 5,  5, 2, 26, 0),
	MUXG(HCLK_ISP1_ROOT, "hclk_isp1_root", m_200m_100m_50m_24m_p,
		0,				       67,	   7, 2, 26, 1),
	COMG(CLK_ISP1_CORE, "clk_isp1_core", gpll_cpll_aupll_spll_p,
		0,				       67,  9, 5, 14, 2, 26, 2),

	MUXG(HCLK_NPU_ROOT, "hclk_npu_root", m_200m_100m_50m_24m_p,
		0,				       73,	   0, 2, 29, 0),
	COMG(CLK_NPU_DSU0, "clk_npu_dsu0", gpll_cpll_aupll_npll_spll_p,
		0,				       73,  2, 5,  7, 3, 29, 1),

	MUXG(PCLK_NPU_ROOT, "pclk_npu_root", m_100m_50m_24m_p,
		0,				       74,	   1, 2, 29, 4),
	MUXG(HCLK_NPU_CM0_ROOT,	"hclk_npu_cm0_root", m_400m_200m_100m_24m_p,
		0,				       74,	   5, 2, 30, 1),
	COMG(CLK_NPU_CM0_RTC, "clk_npu_cm0_rtc", m_24m_32k_p,
		0,				       74,  7, 5, 12, 1, 30, 5),
	MUXG(CLK_NPUTIMER_ROOT,	"clk_nputimer_root", m_24m_100m_p,
		0,				       74,	   3, 1, 29, 7),

	MUXG(HCLK_NVM_ROOT,  "hclk_nvm_root", m_200m_100m_50m_24m_p,
		0,				       77,	   0, 2, 31, 0),
	COMG(ACLK_NVM_ROOT, "aclk_nvm_root", gpll_cpll_p,
		0,				       77,  2, 5,  7, 1, 31, 1),
	COMG(CCLK_EMMC,	"cclk_emmc", gpll_cpll_24m_p,
		0,				       77,  8, 6, 14, 2, 31, 6),

	COMG(BCLK_EMMC,	"bclk_emmc", gpll_cpll_p,
		0,				       78,  0, 5,  5, 1, 31, 7),
	COMG(SCLK_SFC, "sclk_sfc", gpll_cpll_24m_p,
		0,				       78,  6, 6, 12, 2, 31, 9),

	COMG(ACLK_PCIE_ROOT, "aclk_pcie_root", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		       80,  2, 5, 7,  1, 32, 6),
	COMG(ACLK_PHP_ROOT, "aclk_php_root", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		       80,  8, 5, 13, 1, 32, 7),
	MUXG(PCLK_PHP_ROOT, "pclk_php_root", m_150m_50m_24m_p,
		0,				       80,	   0, 2, 32, 0),

	COMG(CLK_GMAC0_PTP_REF,	"clk_gmac0_ptp_ref", clk_gmac0_ptp_ref_p,
		0,				       81,  0, 6,  6, 1, 34, 10),
	COMG(CLK_GMAC1_PTP_REF,	"clk_gmac1_ptp_ref", clk_gmac1_ptp_ref_p,
		0,				       81,  7, 6, 13, 1, 34, 11),

	COMG(CLK_RXOOB0, "clk_rxoob0", gpll_cpll_p,
		0,				       82,  0, 7,  7, 1, 37, 10),
	COMG(CLK_RXOOB1, "clk_rxoob1", gpll_cpll_p,
		0,				       82,  8, 7, 15, 1, 37, 11),

	COMG(CLK_RXOOB2, "clk_rxoob2", gpll_cpll_p,
		0,				       83,  0, 7,  7, 1, 37, 12),
	COMG(CLK_GMAC_125M, "clk_gmac_125m", gpll_cpll_p,
		0,				       83,  8, 7, 15, 1, 35, 5),

	COMG(CLK_GMAC_50M, "clk_gmac_50m", gpll_cpll_p,
		0,				       84,  0, 7,  7, 1, 35, 6),
	COMG(CLK_UTMI_OTG2, "clk_utmi_otg2", m_150m_50m_24m_p,
		0,				       84,  8, 4, 12, 2, 35, 10),

	MUXG(0,	"hclk_rkvdec0_root", m_200m_100m_50m_24m_p,
		0,				       89,	   0, 2, 40, 0),
	COMG(0,	"aclk_rkvdec0_root", gpll_cpll_aupll_spll_p,
		0,				       89,  2, 5,  7, 2, 40, 1),
	COMG(ACLK_RKVDEC_CCU, "aclk_rkvdec_ccu", gpll_cpll_aupll_spll_p,
		0,				       89,  9, 5, 14, 2, 40, 2),

	COMG(CLK_RKVDEC0_CA, "clk_rkvdec0_ca", gpll_cpll_p,
		0,				       90,  0, 5,  5, 1, 40, 7),
	COMG(CLK_RKVDEC0_HEVC_CA, "clk_rkvdec0_hevc_ca", gpll_cpll_npll_1000m_p,
		0,				       90,  6, 5, 11, 2, 40, 8),

	COMG(CLK_RKVDEC0_CORE, "clk_rkvdec0_core", gpll_cpll_p,
		0,				       91,  0, 5,  5, 1, 40, 9),

	MUXG(0,	"hclk_rkvdec1_root", m_200m_100m_50m_24m_p,
		0,				       93,	   0, 2, 41, 0),
	COMG(0,	"aclk_rkvdec1_root", gpll_cpll_aupll_npll_p,
		0,				       93,  2, 5,  7, 2, 41, 1),
	COMG(CLK_RKVDEC1_CA, "clk_rkvdec1_ca", gpll_cpll_p,
		0,				       93,  9, 5, 14, 1, 41, 6),

	COMG(CLK_RKVDEC1_HEVC_CA, "clk_rkvdec1_hevc_ca", gpll_cpll_npll_1000m_p,
		0,				       94,  0, 5,  5, 2, 41, 7),
	COMG(CLK_RKVDEC1_CORE, "clk_rkvdec1_core", gpll_cpll_p,
		0,				       94,  7, 5, 12, 1, 41, 8),

	COMG(ACLK_USB_ROOT, "aclk_usb_root", gpll_cpll_p,
		0,				       96,  0, 5,  5, 1, 42, 0),
	MUXG(HCLK_USB_ROOT, "hclk_usb_root", m_150m_100m_50m_24m_p,
		0,				       96,	   6, 2, 42, 1),

	COMG(ACLK_VDPU_ROOT, "aclk_vdpu_root", gpll_cpll_aupll_p,
		0,				       98,  0, 5,  5, 2, 44, 0),
	MUXG(ACLK_VDPU_LOW_ROOT, "aclk_vdpu_low_root", m_400m_200m_100m_24m_p,
		0,				       98,	   7, 2, 44, 1),
	MUXG(HCLK_VDPU_ROOT, "hclk_vdpu_root", m_200m_100m_50m_24m_p,
		0,				       98,	   9, 2, 44, 2),

	COMG(ACLK_JPEG_DECODER_ROOT, "aclk_jpeg_decoder_root", gpll_cpll_aupll_spll_p,
		0,				       99,  0, 5,  5, 2, 44, 3),
	COMG(CLK_IEP2P0_CORE, "clk_iep2p0_core", gpll_cpll_p,
		0,				       99,  7, 5, 12, 1, 45, 6),

	MUXG(HCLK_RKVENC0_ROOT,	"hclk_rkvenc0_root", m_200m_100m_50m_24m_p,
		0,				      102,	   0, 2, 47, 0),
	COMG(ACLK_RKVENC0_ROOT,	"aclk_rkvenc0_root", gpll_cpll_npll_p,
		0,				      102,  2, 5,  7, 2, 47, 1),
	COMG(CLK_RKVENC0_CORE, "clk_rkvenc0_core", gpll_cpll_aupll_npll_p,
		0,				      102,  9, 5, 14, 2, 47, 6),

	MUXG(HCLK_RKVENC1_ROOT,	"hclk_rkvenc1_root", m_200m_100m_50m_24m_p,
		0,				      104,	   0, 2, 48, 0),
	COMG(ACLK_RKVENC1_ROOT,	"aclk_rkvenc1_root", gpll_cpll_npll_p,
		0,				      104,  2, 5,  7, 2, 48, 1),
	COMG(CLK_RKVENC1_CORE, "clk_rkvenc1_core", gpll_cpll_aupll_npll_p,
		0,				      104,  9, 5, 14, 2, 48, 6),

	COMG(ACLK_VI_ROOT, "aclk_vi_root", gpll_cpll_npll_aupll_spll_p,
		0,				      106,  0, 5,  5, 3, 49, 0),
	MUXG(HCLK_VI_ROOT, "hclk_vi_root", m_200m_100m_50m_24m_p,
		0,				      106,	   8, 2, 49, 1),
	MUXG(PCLK_VI_ROOT, "pclk_vi_root", m_100m_50m_24m_p,
		0,				      106,	  10, 2, 49, 2),

	COMG(DCLK_VICAP, "dclk_vicap", gpll_cpll_p,
		0,				      107,  0, 5,  5, 1, 49, 6),
	COMG(CLK_ISP0_CORE, "clk_isp0_core", gpll_cpll_aupll_spll_p,
		0,				      107,  6, 5, 11, 2, 49, 9),

	COMG(CLK_FISHEYE0_CORE,	"clk_fisheye0_core", gpll_cpll_aupll_spll_p,
		0,				      108,  0, 5,  5, 2,  50, 0),
	COMG(CLK_FISHEYE1_CORE,	"clk_fisheye1_core", gpll_cpll_aupll_spll_p,
		0,				      108,  7, 5, 12, 2, 50, 3),
	MUXG(ICLK_CSIHOST01, "iclk_csihost01", m_400m_200m_100m_24m_p,
		0,				      108,	  14, 2, 51, 10),

	COMG(ACLK_VOP_ROOT, "aclk_vop_root", gpll_cpll_dmyaupll_npll_spll_p,
		0,				      110,  0, 5,  5, 3,  5, 0),
	MUXG(ACLK_VOP_LOW_ROOT,	"aclk_vop_low_root", m_400m_200m_100m_24m_p,
		0,				      110,	   8, 2, 52, 1),
	MUXG(HCLK_VOP_ROOT, "hclk_vop_root", m_200m_100m_50m_24m_p,
		0,				      110,	  10, 2, 52, 2),
	MUXG(PCLK_VOP_ROOT, "pclk_vop_root", m_100m_50m_24m_p,
		0,				      110,	  12, 2, 52, 3),

	COMG(DCLK_VOP0_SRC, "dclk_vop0_src", gpll_cpll_v0pll_aupll_p,
		0,				      111,  0, 7,  7, 2, 52, 10),
	COMG(DCLK_VOP1_SRC, "dclk_vop1_src", gpll_cpll_v0pll_aupll_p,
		0,				      111,  9, 5, 14, 2, 52, 11),

	COMG(DCLK_VOP2_SRC, "dclk_vop2_src", gpll_cpll_v0pll_aupll_p,
		0,				      112,  0, 5,  5, 2, 52, 12),
	MUXG(DCLK_VOP0,	"dclk_vop0", dclk_vop0_p,
		0,				      112,	   7, 2, 52, 13),
	MUXG(DCLK_VOP1,	"dclk_vop1", dclk_vop1_p,
		0,				      112,	   9, 2, 53, 0),
	MUXG(DCLK_VOP2,	"dclk_vop2", dclk_vop2_p,
		0,				      112,	  11, 2, 53, 1),

	COMG(DCLK_VOP3,	"dclk_vop3", gpll_cpll_v0pll_aupll_p,
		0,				      113,  0, 7,  7, 2, 53, 2),

	COMG(CLK_DSIHOST0, "clk_dsihost0", gpll_cpll_v0pll_spll_p,
		0,				      114,  0, 7,  7, 2, 53, 6),

	COMG(CLK_DSIHOST1, "clk_dsihost1", gpll_cpll_v0pll_spll_p,
		0,				      115,  0, 7,  7, 2, 53, 7),
	MUX(ACLK_VOP_SUB_SRC, "aclk_vop_sub_src", aclk_vop_sub_src_p,
						      115,	   9, 1),

	COMG(ACLK_VO0_ROOT, "aclk_vo0_root", gpll_cpll_p,
		0,				      116,  0, 5,  5, 1,  55, 0),
	MUXG(HCLK_VO0_ROOT, "hclk_vo0_root", m_200m_100m_50m_24m_p,
		0,				      116,	   6, 2, 55, 1),
	MUXG(HCLK_VO0_S_ROOT, "hclk_vo0_s_root", m_200m_100m_50m_24m_p,
		0,				      116,	   8, 2, 55, 2),
	MUXG(PCLK_VO0_ROOT, "pclk_vo0_root", m_100m_50m_24m_p,
		0,				      116,	  10, 2, 55, 3),
	MUXG(PCLK_VO0_S_ROOT, "pclk_vo0_s_root", m_100m_50m_24m_p,
		0,				      116,	  12, 2, 55, 4),

	DIVG(CLK_AUX16M_0, "clk_aux16m_0", "gpll",
		0,				      117,  0, 8,	 56, 2),
	DIVG(CLK_AUX16M_1, "clk_aux16m_1", "gpll",
		0,				      117,  8, 8,	 56, 3),

	COMG(CLK_I2S4_8CH_TX_SRC, "clk_i2s4_8ch_tx_src", gpll_aupll_p,
		0,				      118,  0, 5,  5, 1, 56, 11),

	FRAG(CLK_I2S4_8CH_TX_FRAC, "clk_i2s4_8ch_tx_frac", "clk_i2s4_8ch_tx_src",
		0,				      119,		 56, 12),

	MUX(CLK_I2S4_8CH_TX, "clk_i2s4_8ch_tx",	clk_i2s4_8ch_tx_p,
						      120,	   0, 2),
	COMG(CLK_I2S8_8CH_TX_SRC, "clk_i2s8_8ch_tx_src", gpll_aupll_p,
		0,				      120,  3, 5,  8, 1, 56, 15),

	FRAG(CLK_I2S8_8CH_TX_FRAC, "clk_i2s8_8ch_tx_frac", "clk_i2s8_8ch_tx_src",
		0,				      121,		 57, 0),

	MUX(CLK_I2S8_8CH_TX, "clk_i2s8_8ch_tx",	clk_i2s8_8ch_tx_p,
						      122,	   0, 2),
	COMG(CLK_SPDIF2_DP0_SRC, "clk_spdif2_dp0_src", gpll_aupll_p,
		0,				      122,  3, 5,  8, 1, 57, 3),

	FRAG(CLK_SPDIF2_DP0_FRAC, "clk_spdif2_dp0_frac", "clk_spdif2_dp0_src",
		0,				      123,		 57, 4),

	MUX(CLK_SPDIF2_DP0, "clk_spdif2_dp0", clk_spdif2_dp0_p,
						      124,	   0, 2),

	COMG(CLK_SPDIF5_DP1_SRC, "clk_spdif5_dp1_src", gpll_aupll_p,
		0,				      124,  2, 5,  7, 1, 57, 8),

	FRAG(CLK_SPDIF5_DP1_FRAC, "clk_spdif5_dp1_frac", "clk_spdif5_dp1_src",
		0,				      125,		 57, 9),

	MUX(CLK_SPDIF5_DP1, "clk_spdif5_dp1", clk_spdif5_dp1_p,
						      126,	   0, 2),

	COMG(ACLK_HDCP1_ROOT, "aclk_hdcp1_root", aclk_hdcp1_root_p,
		0,				      128,  0, 5,  5, 2, 59, 0),
	COMG(ACLK_HDMIRX_ROOT, "aclk_hdmirx_root", gpll_cpll_p,
		0,				      128,  7, 5, 12, 1, 59, 1),
	MUXG(HCLK_VO1_ROOT, "hclk_vo1_root", m_200m_100m_50m_24m_p,
		0,				      128,	  13, 2, 59, 2),

	MUXG(HCLK_VO1_S_ROOT, "hclk_vo1_s_root", m_200m_100m_50m_24m_p,
		0,				      129,	   0, 2, 59, 3),
	MUXG(PCLK_VO1_ROOT, "pclk_vo1_root", m_150m_100m_24m_p,
		0,				      129,	   2, 2, 59, 4),
	MUXG(PCLK_VO1_S_ROOT, "pclk_vo1_s_root", m_100m_50m_24m_p,
		0,				      129,	   4, 2, 59, 5),
	COMG(CLK_I2S7_8CH_RX_SRC, "clk_i2s7_8ch_rx_src", gpll_aupll_p,
		0,				      129,  6, 5,  11, 1, 60, 1),

	FRAG(CLK_I2S7_8CH_RX_FRAC, "clk_i2s7_8ch_rx_frac", "clk_i2s7_8ch_rx_src",
		0,				      130,		 60, 2),

	MUX(CLK_I2S7_8CH_RX, "clk_i2s7_8ch_rx",	clk_i2s7_8ch_rx_p,
						      131,	   0, 2),

	COMG(CLK_HDMITX0_EARC, "clk_hdmitx0_earc", gpll_cpll_p,
		0,				      133,  1, 5,  6, 1, 60, 15),

	COMG(CLK_HDMITX1_EARC, "clk_hdmitx1_earc", gpll_cpll_p,
		0,				      136,  1, 5,  6, 1, 61, 6),

	COMG(CLK_HDMIRX_AUD_SRC, "clk_hdmirx_aud_src", gpll_aupll_p,
		0,				      138,  0, 8,  8, 1, 61, 12),

	FRAG(CLK_HDMIRX_AUD_FRAC, "clk_hdmirx_aud_frac", "clk_hdmirx_aud_src",
		0,				      139,		 61, 13),

	MUX(CLK_HDMIRX_AUD_P_MUX, "clk_hdmirx_aud_mux",	clk_hdmirx_aud_p,
						      140,	   0, 1),
	MUXG(CLK_EDP0_200M, "clk_edp0_200m", m_200m_100m_50m_24m_p,
		0,				      140,	   1, 2, 62, 2),
	MUXG(CLK_EDP1_200M, "clk_edp1_200m", m_200m_100m_50m_24m_p,
		0,				      140,	   3, 2, 62, 5),
	COMG(CLK_I2S5_8CH_TX_SRC, "clk_i2s5_8ch_tx_src", gpll_aupll_p,
		0,				      140,  5, 5, 10, 1, 62, 6),

	FRAG(CLK_I2S5_8CH_TX_FRAC, "clk_i2s5_8ch_tx_frac", "clk_i2s5_8ch_tx_src",
		0,				      141,		 62, 7),

	MUX(CLK_I2S5_8CH_TX, "clk_i2s5_8ch_tx",	clk_i2s5_8ch_tx_p,
						      142,	   0, 2),

	COMG(CLK_I2S6_8CH_TX_SRC, "clk_i2s6_8ch_tx_src", gpll_aupll_p,
		0,				      144, 3, 5,  8, 1,	62, 13),

	FRAG(CLK_I2S6_8CH_TX_FRAC, "clk_i2s6_8ch_tx_frac", "clk_i2s6_8ch_tx_src",
		0,				      145,		 62, 14),

	MUX(CLK_I2S6_8CH_TX, "clk_i2s6_8ch_tx",	clk_i2s6_8ch_tx_p,
						      146,	   0, 2),
	COMG(CLK_I2S6_8CH_RX_SRC, "clk_i2s6_8ch_rx_src", gpll_aupll_p,
		0,				      146,  2, 5,  7, 1, 63, 0),

	FRAG(CLK_I2S6_8CH_RX_FRAC, "clk_i2s6_8ch_rx_frac", "clk_i2s6_8ch_rx_src",
		0,				      147,		 63, 1),

	MUX(CLK_I2S6_8CH_RX, "clk_i2s6_8ch_rx",	clk_i2s6_8ch_rx_p,
						      148,	   0, 2),
	MUX(I2S6_8CH_MCLKOUT, "i2s6_8ch_mclkout", i2s6_8ch_mclkout_p,
						      148,	  2, 2),
	COMG(CLK_SPDIF3_SRC, "clk_spdif3_src", gpll_aupll_p,
		0,				      148,  4, 5,  9, 1, 63, 5),

	FRAG(CLK_SPDIF3_FRAC, "clk_spdif3_frac", "clk_spdif3_src",
		0,				      149,		 63, 6),

	MUX(CLK_SPDIF3,	"clk_spdif3", clk_spdif3_p,
						      150,	   0, 2),
	COMG(CLK_SPDIF4_SRC, "clk_spdif4_src", gpll_aupll_p,
		0,				      150,  2, 5,  7, 1, 63, 9),

	FRAG(CLK_SPDIF4_FRAC, "clk_spdif4_frac", "clk_spdif4_src",
		0,				      151,		 63, 10),

	MUX(CLK_SPDIF4,	"clk_spdif4", clk_spdif4_p,
						      152,	   0, 2),
	COMG(MCLK_SPDIFRX0, "mclk_spdifrx0", gpll_cpll_aupll_p,
		0,				      152,  2, 5,  7, 2, 63, 13),
	COMG(MCLK_SPDIFRX1, "mclk_spdifrx1", gpll_cpll_aupll_p,
		0,				      152,  9, 5, 14, 2, 63, 15),

	COMG(MCLK_SPDIFRX2, "mclk_spdifrx2", gpll_cpll_aupll_p,
		0,				      153,  0, 5,  5, 2, 64, 1),
	COMG(CLK_I2S9_8CH_RX_SRC, "clk_i2s9_8ch_rx_src", gpll_aupll_p,
		0,				      153,  7, 5, 12, 1, 65, 1),

	FRAG(CLK_I2S9_8CH_RX_FRAC, "clk_i2s9_8ch_rx_frac", "clk_i2s9_8ch_rx_src",
		0,				      154,		 65, 2),

	MUX(CLK_I2S9_8CH_RX, "clk_i2s9_8ch_rx",	clk_i2s9_8ch_rx_p,
						      155,	   0, 2),
	COMG(CLK_I2S10_8CH_RX_SRC, "clk_i2s10_8ch_rx_src", gpll_aupll_p,
		0,				      155,  3, 5,  8, 1, 65, 5),

	FRAG(CLK_I2S10_8CH_RX_FRAC, "clk_i2s10_8ch_rx_frac", "clk_i2s10_8ch_rx_src",
		0,				      156,		 65, 6),

	MUX(CLK_I2S10_8CH_RX, "clk_i2s10_8ch_rx", clk_i2s10_8ch_rx_p,
						      157,	   0, 2),
	COMH(CLK_HDMITRX_REFSRC, "clk_hdmitrx_refsrc", gpll_cpll_p,
		0,				      157,  2, 5,  7, 1, 65, 9),

	COMG(CLK_GPU_SRC, "clk_gpu_src", gpll_cpll_aupll_npll_spll_p,
		0,				      158,  0, 5,  5, 3, 66, 1),

	DIVG(CLK_GPU_STACKS, "clk_gpu_stacks", "clk_gpu_src",
		0,				      159,  0, 5,	66, 7),

	COMG(ACLK_AV1_ROOT, "aclk_av1_root", gpll_cpll_aupll_p,
		0,				      163,  0, 5,  5, 2, 68, 0),
	MUXG(PCLK_AV1_ROOT, "pclk_av1_root", m_200m_100m_50m_24m_p,
		0,				      163,	   7, 2, 68, 3),

	MUXG(ACLK_CENTER_ROOT, "aclk_center_root", m_700m_400m_200m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	   0, 2, 69, 0),
	MUXG(ACLK_CENTER_LOW_ROOT, "aclk_center_low_root", m_500m_250m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	   2, 2, 69, 1),
	MUXG(HCLK_CENTER_ROOT, "hclk_center_root", m_400m_200m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	   4, 2, 69, 2),
	MUXG(PCLK_CENTER_ROOT, "pclk_center_root", m_200m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	   6, 2, 69, 3),
	MUXG(ACLK_CENTER_S200_ROOT, "aclk_center_s200_root", m_200m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	   8, 2, 69, 8),
	MUXG(ACLK_CENTER_S400_ROOT, "aclk_center_s400_root", m_400m_200m_100m_24m_p,
		CLK_NODE_CANNOT_STOP,		      165,	  10, 2, 69, 9),
	MUXG(CLK_DDR_TIMER_ROOT, "clk_ddr_timer_root", m_24m_100m_p,
		CLK_NODE_CANNOT_STOP,		      165,	  12, 1, 69, 15),

	COMG(CLK_DDR_CM0_RTC, "clk_ddr_cm0_rtc", m_24m_32k_p,
		0,				      166,  0, 5,  5, 1, 70, 4),

	COMG(ACLK_VO1USB_TOP_ROOT, "aclk_vo1usb_top_root", gpll_cpll_p,
		CLK_NODE_CANNOT_STOP,		      170,  0, 5,  5, 1, 74, 0),
	MUXG(HCLK_VO1USB_TOP_ROOT, "hclk_vo1usb_top_root", m_200m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		      170,	   6, 2, 74, 2),

	MUXG(0,	"hclk_sdio_root", m_200m_100m_50m_24m_p,
		0,				      172,	   0, 2, 75, 0),
	COMG(CCLK_SRC_SDIO, "cclk_src_sdio", gpll_cpll_24m_p,
		0,				      172,  2, 6,  8, 2, 75, 3),

	COMG(ACLK_RGA3_ROOT, "aclk_rga3_root", gpll_cpll_aupll_p,
		0,				      174,  0, 5,  5, 2, 76, 0),
	MUXG(HCLK_RGA3_ROOT, "hclk_rga3_root", m_200m_100m_50m_24m_p,
		0,				      174,	   7, 2, 76, 1),
	COMG(CLK_RGA3_1_CORE, "clk_rga3_1_core", gpll_cpll_aupll_spll_p,
		0,				      174,  9, 5, 14, 2, 76, 6),

	DIVG(CLK_REF_PIPE_PHY0_PLL_SRC,	"clk_ref_pipe_phy0_pll_src", "ppll",
		0,				      176,  0, 6,	 77, 3),
	DIVG(CLK_REF_PIPE_PHY1_PLL_SRC,	"clk_ref_pipe_phy1_pll_src", "ppll",
		0,				      176,  6, 6,	 77, 4),

	DIVG(CLK_REF_PIPE_PHY2_PLL_SRC,	"clk_ref_pipe_phy2_pll_src", "ppll",
		0,				      177,  0, 6,	 77, 5),
	MUX(CLK_REF_PIPE_PHY0, "clk_ref_pipe_phy0", clk_ref_pipe_phy0_p,
						      177,	   6, 1),
	MUX(CLK_REF_PIPE_PHY1, "clk_ref_pipe_phy1", clk_ref_pipe_phy1_p,
						      177,	   7, 1),
	MUX(CLK_REF_PIPE_PHY2, "clk_ref_pipe_phy2", clk_ref_pipe_phy2_p,
						      177,	   8, 1),


	/* PMU */
	DIVG(CLK_PMU1_50M_SRC, "clk_pmu1_50m_src", "clk_pmu1_400m_src",
		CLK_NODE_CANNOT_STOP,		   PMU(0),  0, 4,	 PMU(0), 0),
	DIVG(CLK_PMU1_100M_SRC,	"clk_pmu1_100m_src", "clk_pmu1_400m_src",
		0,				   PMU(0),  4, 3,	 PMU(0), 1),
	DIVG(CLK_PMU1_200M_SRC,	"clk_pmu1_200m_src", "clk_pmu1_400m_src",
		CLK_NODE_CANNOT_STOP,		   PMU(0),	   7, 3, PMU(0), 2),
	COMG(CLK_PMU1_300M_SRC,	"clk_pmu1_300m_src", pmu_300m_24m_p,
		CLK_NODE_CANNOT_STOP,		   PMU(0), 10, 5, 15, 1, PMU(0), 3),

	COMG(CLK_PMU1_400M_SRC,	"clk_pmu1_400m_src", pmu_400m_24m_p,
		CLK_NODE_CANNOT_STOP,		   PMU(1),  0, 5, 15, 1, PMU(0), 4),
	MUXG(HCLK_PMU1_ROOT, "hclk_pmu1_root", hclk_pmu1_root_p,
		CLK_NODE_CANNOT_STOP,		   PMU(1),	   6, 2, PMU(0), 5),
	MUXG(PCLK_PMU1_ROOT, "pclk_pmu1_root", pmu_100m_50m_24m_src_p,
		CLK_NODE_CANNOT_STOP,		   PMU(1),	   8, 2, PMU(0), 7),
	MUXG(HCLK_PMU_CM0_ROOT,	"hclk_pmu_cm0_root", hclk_pmu_cm0_root_p,
		CLK_NODE_CANNOT_STOP,		   PMU(1),	  10, 2, PMU(0), 8),

	MUXG(TCLK_PMU1WDT, "tclk_pmu1wdt", m_24m_32k_p,
		0,				   PMU(2),	   6, 1, PMU(1), 7),
	MUXG(CLK_PMU1TIMER_ROOT, "clk_pmu1timer_root", pmu_24m_32k_100m_src_p,
		0,				   PMU(2),	   7, 2, PMU(1), 9),
	MUXG(CLK_PMU1PWM, "clk_pmu1pwm", pmu_100m_50m_24m_src_p,
		0,				   PMU(2),	   9, 2, PMU(1), 13),
	COMG(CLK_PMU_CM0_RTC, "clk_pmu_cm0_rtc", m_24m_32k_p,
		CLK_NODE_CANNOT_STOP,		   PMU(2),  0, 5,  5, 1, PMU(0), 15),

	MUXG(CLK_I2C0, "clk_i2c0", pmu_200m_100m_p,
		0,				   PMU(3),	   6, 1, PMU(2), 2),
	DIVG(CLK_UART0_SRC, "clk_uart0_src", "cpll",
		0,				   PMU(3),  7, 5,	 PMU(2), 3),

	FRAG(CLK_UART0_FRAC, "clk_uart0_frac", "clk_uart0_src",
		0,				   PMU(4),		 PMU(2), 4),

	MUX(CLK_UART0, "clk_uart0", clk_uart0_p,
						   PMU(5),	   0, 2),
	DIVG(CLK_I2S1_8CH_TX_SRC, "clk_i2s1_8ch_tx_src", "cpll",
		0,				   PMU(5),  2, 5,	 PMU(2), 8),


	FRAG(CLK_I2S1_8CH_TX_FRAC, "clk_i2s1_8ch_tx_frac", "clk_i2s1_8ch_tx_src",
		0,				   PMU(6),		 PMU(2), 9),


	MUX(CLK_I2S1_8CH_TX, "clk_i2s1_8ch_tx",	clk_i2s1_8ch_tx_p,
						   PMU(7),	   0, 2),
	DIVG(CLK_I2S1_8CH_RX_SRC, "clk_i2s1_8ch_rx_src", "cpll",
		0,				   PMU(7),  2, 5,	 PMU(2), 11),

	FRAG(CLK_I2S1_8CH_RX_FRAC, "clk_i2s1_8ch_rx_frac", "clk_i2s1_8ch_rx_src",
		0,				   PMU(8),		 PMU(2), 12),
	MUX(CLK_I2S1_8CH_RX, "clk_i2s1_8ch_rx",	clk_i2s1_8ch_rx_p,
						   PMU(9),	   0, 2),
	MUX(I2S1_8CH_MCLKOUT, "i2s1_8ch_mclkout", i2s1_8ch_mclkout_p,
						   PMU(9),	   2, 2),
	MUXG(MCLK_PDM0,	"mclk_pdm0", mclk_pdm0_p,
		0,				   PMU(9),	   4, 1, PMU(2), 15),


	COMG(CLK_USB2PHY_HDPTXRXPHY_REF, "clk_usb2phy_hdptxrxphy_ref", m_24m_ppll_p,
		CLK_NODE_CANNOT_STOP,		  PMU(14),  9, 5, 14, 1, PMU(4), 7),
	COMG(CLK_USBDPPHY_MIPIDCPPHY_REF, "clk_usbdpphy_mipidcpphy_ref", m_24m_ppll_spll_p,
		CLK_NODE_CANNOT_STOP,		  PMU(14),  0, 7,  7, 2, PMU(4), 3),

	COMG(CLK_CR_PARA, "clk_cr_para", m_24m_ppll_spll_p,
		0,				  PMU(15),  0, 5,  5, 2, PMU(4), 11),

	MUXG(DBCLK_GPIO0, "dbclk_gpio0", m_24m_32k_p,
		0,				  PMU(17),	   0, 1, PMU(5), 6),


	/* DSU */
	COMG(0, "sclk_dsu", b0pll_b1pll_lpll_gpll_p,
		CLK_NODE_CANNOT_STOP,		   DSU(0),  0, 5, 12, 2, DSU(0), 4),

	DIVG(0, "aclkm_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(1),  1, 5,	 DSU(0), 8),
	DIVG(0, "aclks_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(1),  6, 5,	 DSU(0), 9),
	DIVG(0, "aclkmp_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(1), 11, 5,	 DSU(0), 12),

	DIVG(0, "periph_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,			   DSU(2),  0, 5,	 DSU(0), 13),
	DIVG(0, "cntclk_dsu", "periph_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(2),  5, 5,	 DSU(0), 14),
	DIVG(0, "tsclk_dsu", "periph_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(2), 10, 5,	 DSU(0), 15),

	DIVG(0, "atclk_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(3),  0, 5,	 DSU(1), 0),
	DIVG(0, "gicclk_dsu", "sclk_dsu",
		CLK_NODE_CANNOT_STOP,		   DSU(3),  5, 5,	 DSU(1), 1),

	COMG(PCLK_DSU_ROOT, "pclk_dsu_root", b0pll_b1pll_lpll_gpll_p,
		CLK_NODE_CANNOT_STOP,		   DSU(4),  0, 5,  5, 2, DSU(1), 3),
	MUXG(PCLK_DSU_NS_ROOT, "pclk_dsu_ns_root", m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		   DSU(4), 	   7, 2, DSU(1), 4),
	MUXG(PCLK_DSU_S_ROOT, "pclk_dsu_s_root", m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		   DSU(4), 	  11, 2, DSU(2), 2),

	ARMDIV(ARMCLK_L, "armclk_l", m_armclkl_p, cpul_rates,
	    DSU(6),  0, 5,  14, 2,  2, 1),

	/* BC0 */
	MUXG(PCLK_BIGCORE0_ROOT, "pclk_bigcore0_root", m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		   BC0(2),  0, 2,	 BC0(0), 14),
	ARMDIV(ARMCLK_B01, "armclk_b01", m_armclkb01_p, cpubc0_rates,
	    BC0(0),  8, 5,  6, 2,  2, 1),


	/* BC1 */
	MUXG(PCLK_BIGCORE1_ROOT, "pclk_bigcore1_root", m_100m_50m_24m_p,
		CLK_NODE_CANNOT_STOP,		   BC1(2),  0, 2,	 BC1(0), 14),
	ARMDIV(ARMCLK_B23, "armclk_b23", m_armclkb23_p, cpubc1_rates,
	    BC1(0),  8, 5,  6, 2,  2, 1),

};

/* GATES */
static struct rk_cru_gate rk3588_gates[] = {
	GATE(PCLK_CSIPHY0, "pclk_csiphy0", "pclk_top_root",		 1, 6),
	GATE(PCLK_CSIPHY1, "pclk_csiphy1", "pclk_top_root",		 1, 8),

	GATE(CLK_USBDP_PHY0_IMMORTAL, "clk_usbdp_phy0_immortal", "xin24m", 2, 8),
	GATE(CLK_USBDP_PHY1_IMMORTAL, "clk_usbdp_phy1_immortal", "xin24m", 2, 15),


	GATE(PCLK_MIPI_DCPHY0, "pclk_mipi_dcphy0", "pclk_top_root",	 3, 14)
	,
	GATE(PCLK_MIPI_DCPHY1, "pclk_mipi_dcphy1", "pclk_top_root",	 4, 3),

	GATN(PCLK_CRU, "pclk_cru", "pclk_top_root",			 5, 0),

	GATE(HCLK_I2S0_8CH, "hclk_i2s0_8ch", "hclk_audio_root",		 7, 4),
	GATE(MCLK_I2S0_8CH_TX, "mclk_i2s0_8ch_tx", "clk_i2s0_8ch_tx",	 7, 7),


	GATE(MCLK_I2S0_8CH_RX, "mclk_i2s0_8ch_rx", "clk_i2s0_8ch_rx",	 7, 10),
	GATE(PCLK_ACDCDIG, "pclk_acdcdig", "pclk_audio_root",		 7, 11),

	GATE(MCLK_I2S2_2CH, "mclk_i2s2_2ch", "clk_i2s2_2ch",		 8, 0),
	GATE(MCLK_I2S3_2CH, "mclk_i2s3_2ch", "clk_i2s3_2ch",		 8, 3),
	GATE(CLK_DAC_ACDCDIG, "clk_dac_acdcdig", "mclk_i2s3_2ch",	 8, 4),
	GATE(HCLK_SPDIF0, "hclk_spdif0", "hclk_audio_root",		 8, 14),

	GATE(MCLK_SPDIF0, "mclk_spdif0", "clk_spdif0",			 9, 1),
	GATE(HCLK_SPDIF1, "hclk_spdif1", "hclk_audio_root",		 9, 2),
	GATE(HCLK_PDM1,	"hclk_pdm1", "hclk_audio_root",			 9, 6),
	GATE(MCLK_SPDIF1, "mclk_spdif1", "clk_spdif1",			 9, 5),

	GATN(ACLK_GIC, "aclk_gic", "aclk_bus_root",			10, 3),
	GATE(ACLK_DMAC0, "aclk_dmac0", "aclk_bus_root",			10, 5),
	GATE(ACLK_DMAC1, "aclk_dmac1", "aclk_bus_root",			10, 6),
	GATE(ACLK_DMAC2, "aclk_dmac2", "aclk_bus_root",			10, 7),
	GATE(PCLK_I2C1,	"pclk_i2c1", "pclk_top_root",			10, 8),
	GATE(PCLK_I2C2,	"pclk_i2c2", "pclk_top_root",			10, 9),
	GATE(PCLK_I2C3,	"pclk_i2c3", "pclk_top_root",			10, 10),
	GATE(PCLK_I2C4,	"pclk_i2c4", "pclk_top_root",			10, 11),
	GATE(PCLK_I2C5,	"pclk_i2c5", "pclk_top_root",			10, 12),
	GATE(PCLK_I2C6,	"pclk_i2c6", "pclk_top_root",			10, 13),
	GATE(PCLK_I2C7,	"pclk_i2c7", "pclk_top_root",			10, 14),
	GATE(PCLK_I2C8,	"pclk_i2c8", "pclk_top_root",			10, 15),

	GATE(PCLK_CAN0,	"pclk_can0", "pclk_top_root",			11, 8),
	GATE(PCLK_CAN1,	"pclk_can1", "pclk_top_root",			11, 10),
	GATE(PCLK_CAN2,	"pclk_can2", "pclk_top_root",			11, 12),
	GATE(PCLK_SARADC, "pclk_saradc", "pclk_top_root",		11, 14),

	GATE(PCLK_TSADC, "pclk_tsadc", "pclk_top_root",			12, 0),
	GATE(PCLK_UART1, "pclk_uart1", "pclk_top_root",			12, 2),
	GATE(PCLK_UART2, "pclk_uart2", "pclk_top_root",			12, 3),
	GATE(PCLK_UART3, "pclk_uart3", "pclk_top_root",			12, 4),
	GATE(PCLK_UART4, "pclk_uart4", "pclk_top_root",			12, 5),
	GATE(PCLK_UART5, "pclk_uart5", "pclk_top_root",			12, 6),
	GATE(PCLK_UART6, "pclk_uart6", "pclk_top_root",			12, 7),
	GATE(PCLK_UART7, "pclk_uart7", "pclk_top_root",			12, 8),
	GATE(PCLK_UART8, "pclk_uart8", "pclk_top_root",			12, 9),
	GATE(PCLK_UART9, "pclk_uart9", "pclk_top_root",			12, 10),
	GATE(SCLK_UART1, "sclk_uart1", "clk_uart1",			12, 13),

	GATE(SCLK_UART2, "sclk_uart2", "clk_uart2",			13, 0),
	GATE(SCLK_UART3, "sclk_uart3", "clk_uart3",			13, 3),
	GATE(SCLK_UART4, "sclk_uart4", "clk_uart4",			13, 6),
	GATE(SCLK_UART5, "sclk_uart5", "clk_uart5",			13, 9),
	GATE(SCLK_UART6, "sclk_uart6", "clk_uart6",			13, 12),
	GATE(SCLK_UART7, "sclk_uart7", "clk_uart7",			13, 15),

	GATE(SCLK_UART8, "sclk_uart8", "clk_uart8",			14, 2),
	GATE(SCLK_UART9, "sclk_uart9", "clk_uart9",			14, 5),
	GATE(PCLK_SPI0,	"pclk_spi0", "pclk_top_root",			14, 6),
	GATE(PCLK_SPI1,	"pclk_spi1", "pclk_top_root",			14, 7),
	GATE(PCLK_SPI2,	"pclk_spi2", "pclk_top_root",			14, 8),
	GATE(PCLK_SPI3,	"pclk_spi3", "pclk_top_root",			14, 9),
	GATE(PCLK_SPI4,	"pclk_spi4", "pclk_top_root",			14, 10),

	GATE(PCLK_WDT0,	"pclk_wdt0", "pclk_top_root",			15, 0),
	GATE(TCLK_WDT0,	"tclk_wdt0", "xin24m",				15, 1),
	GATE(PCLK_PWM1,	"pclk_pwm1", "pclk_top_root",			15, 3),
	GATE(CLK_PWM1_CAPTURE, "clk_pwm1_capture", "xin24m",		15, 5),
	GATE(PCLK_PWM2,	"pclk_pwm2", "pclk_top_root",			15, 6),
	GATE(CLK_PWM2_CAPTURE, "clk_pwm2_capture", "xin24m",		15, 8),
	GATE(PCLK_PWM3,	"pclk_pwm3", "pclk_top_root",			15, 9),
	GATE(CLK_PWM3_CAPTURE, "clk_pwm3_capture", "xin24m",		15, 11),
	GATE(PCLK_BUSTIMER0, "pclk_bustimer0", "pclk_top_root",		15, 12),
	GATE(PCLK_BUSTIMER1, "pclk_bustimer1", "pclk_top_root",		15, 13),
	GATE(CLK_BUSTIMER0, "clk_bustimer0", "clk_bus_timer_root",	15, 15),

	GATE(CLK_BUSTIMER1, "clk_bustimer1", "clk_bus_timer_root",	16, 0),
	GATE(CLK_BUSTIMER2, "clk_bustimer2", "clk_bus_timer_root",	16, 1),
	GATE(CLK_BUSTIMER3, "clk_bustimer3", "clk_bus_timer_root",	16, 2),
	GATE(CLK_BUSTIMER4, "clk_bustimer4", "clk_bus_timer_root",	16, 3),
	GATE(CLK_BUSTIMER5, "clk_bustimer5", "clk_bus_timer_root",	16, 4),
	GATE(CLK_BUSTIMER6, "clk_bustimer6", "clk_bus_timer_root",	16, 5),
	GATE(CLK_BUSTIMER7, "clk_bustimer7", "clk_bus_timer_root",	16, 6),
	GATE(CLK_BUSTIMER8, "clk_bustimer8", "clk_bus_timer_root",	16, 7),
	GATE(CLK_BUSTIMER9, "clk_bustimer9", "clk_bus_timer_root",	16, 8),
	GATE(CLK_BUSTIMER10, "clk_bustimer10", "clk_bus_timer_root",	16, 9),
	GATE(CLK_BUSTIMER11, "clk_bustimer11", "clk_bus_timer_root",	16, 10),
	GATE(PCLK_MAILBOX0, "pclk_mailbox0", "pclk_top_root",		16, 11),
	GATE(PCLK_MAILBOX1, "pclk_mailbox1", "pclk_top_root",		16, 12),
	GATE(PCLK_MAILBOX2, "pclk_mailbox2", "pclk_top_root",		16, 13),
	GATE(PCLK_GPIO1, "pclk_gpio1", "pclk_top_root",			16, 14),

	GATE(PCLK_GPIO2, "pclk_gpio2", "pclk_top_root",			17, 0),
	GATE(PCLK_GPIO3, "pclk_gpio3", "pclk_top_root",			17, 2),
	GATE(PCLK_GPIO4, "pclk_gpio4", "pclk_top_root",			17, 4),
	GATE(ACLK_DECOM, "aclk_decom", "aclk_bus_root",			17, 6),
	GATE(PCLK_DECOM, "pclk_decom", "pclk_top_root",			17, 7),

	GATE(ACLK_SPINLOCK, "aclk_spinlock", "aclk_bus_root",		18, 6),
	GATE(PCLK_OTPC_NS, "pclk_otpc_ns", "pclk_top_root",		18, 9),
	GATE(CLK_OTPC_NS, "clk_otpc_ns", "xin24m",			18, 10),
	GATE(CLK_OTPC_ARB, "clk_otpc_arb", "xin24m",			18, 11),
	GATE(CLK_OTP_PHY_G, "clk_otp_phy_g", "xin24m",			18, 13),
	GATE(CLK_OTPC_AUTO_RD_G, "clk_otpc_auto_rd_g", "xin24m",	18, 12),

	GATN(PCLK_PMU2,	"pclk_pmu2", "pclk_top_root",			19, 3),
	GATN(PCLK_PMUCM0_INTMUX, "pclk_pmucm0_intmux", "pclk_top_root",	19, 4),
	GATN(PCLK_DDRCM0_INTMUX, "pclk_ddrcm0_intmux", "pclk_top_root",	19, 5),

	GATE(CLK_ISP1_CORE_MARVIN, "clk_isp1_core_marvin", "clk_isp1_core", 26,	3),
	GATE(CLK_ISP1_CORE_VICAP, "clk_isp1_core_vicap", "clk_isp1_core", 26, 4),
	GATE(ACLK_ISP1,	"aclk_isp1", "aclk_isp1_pre",			26, 5),
	GATE(HCLK_ISP1,	"hclk_isp1", "hclk_isp1_pre",			26, 7),

	GATE(FCLK_NPU_CM0_CORE,	"fclk_npu_cm0_core", "hclk_npu_cm0_root",30, 3),

	GATE(HCLK_EMMC,	"hclk_emmc", "hclk_nvm",			31, 4),
	GATE(TMCLK_EMMC, "tmclk_emmc", "xin24m",			31, 8),
	GATE(HCLK_SFC, "hclk_sfc", "hclk_nvm",				31, 10),
	GATE(HCLK_SFC_XIP, "hclk_sfc_xip", "hclk_nvm",			31, 11),

	GATE(PCLK_GMAC0, "pclk_gmac0", "pclk_php_root",			32, 3),
	GATE(PCLK_GMAC1, "pclk_gmac1", "pclk_php_root",			32, 4),
	GATE(ACLK_PCIE_BRIDGE, "aclk_pcie_bridge", "aclk_pcie_root",	32, 8),
	GATE(ACLK_GMAC0, "aclk_gmac0", "aclk_mmu_php",			32, 10),
	GATE(ACLK_GMAC1, "aclk_gmac1", "aclk_mmu_php",			32, 11),
	GATE(ACLK_PCIE_4L_DBI, "aclk_pcie_4l_dbi", "aclk_php_root",	32, 13),
	GATE(ACLK_PCIE_2L_DBI, "aclk_pcie_2l_dbi", "aclk_php_root",	32, 14),
	GATE(ACLK_PCIE_1L0_DBI,	"aclk_pcie_1l0_dbi", "aclk_php_root",	32, 15),

	GATE(ACLK_PCIE_1L1_DBI,	"aclk_pcie_1l1_dbi", "aclk_php_root",	33, 0),
	GATE(ACLK_PCIE_1L2_DBI,	"aclk_pcie_1l2_dbi", "aclk_php_root",	33, 1),
	GATE(ACLK_PCIE_4L_MSTR,	"aclk_pcie_4l_mstr", "aclk_mmu_pcie",	33, 2),
	GATE(ACLK_PCIE_2L_MSTR,	"aclk_pcie_2l_mstr", "aclk_mmu_pcie",	33, 3),
	GATE(ACLK_PCIE_1L0_MSTR, "aclk_pcie_1l0_mstr", "aclk_mmu_pcie",	33, 4),
	GATE(ACLK_PCIE_1L1_MSTR, "aclk_pcie_1l1_mstr", "aclk_mmu_pcie",	33, 5),
	GATE(ACLK_PCIE_1L2_MSTR, "aclk_pcie_1l2_mstr", "aclk_mmu_pcie",	33, 6),
	GATE(ACLK_PCIE_4L_SLV, "aclk_pcie_4l_slv", "aclk_php_root",	33, 7),
	GATE(ACLK_PCIE_2L_SLV, "aclk_pcie_2l_slv", "aclk_php_root",	33, 8),
	GATE(ACLK_PCIE_1L0_SLV,	"aclk_pcie_1l0_slv", "aclk_php_root",	33, 9),
	GATE(ACLK_PCIE_1L1_SLV,	"aclk_pcie_1l1_slv", "aclk_php_root",	33, 10),
	GATE(ACLK_PCIE_1L2_SLV,	"aclk_pcie_1l2_slv", "aclk_php_root",	33, 11),
	GATE(PCLK_PCIE_4L, "pclk_pcie_4l", "pclk_php_root",		33, 12),
	GATE(PCLK_PCIE_2L, "pclk_pcie_2l", "pclk_php_root",		33, 13),
	GATE(PCLK_PCIE_1L0, "pclk_pcie_1l0", "pclk_php_root",		33, 14),
	GATE(PCLK_PCIE_1L1, "pclk_pcie_1l1", "pclk_php_root",		33, 15),

	GATE(PCLK_PCIE_1L2, "pclk_pcie_1l2", "pclk_php_root",		34, 0),
	GATE(CLK_PCIE_AUX0, "clk_pcie_aux0", "xin24m",			34, 1),
	GATE(CLK_PCIE_AUX1, "clk_pcie_aux1", "xin24m",			34, 2),
	GATE(CLK_PCIE_AUX2, "clk_pcie_aux2", "xin24m",			34, 3),
	GATE(CLK_PCIE_AUX3, "clk_pcie_aux3", "xin24m",			34, 4),
	GATE(CLK_PCIE_AUX4, "clk_pcie_aux4", "xin24m",			34, 5),
	GATN(ACLK_PHP_GIC_ITS, "aclk_php_gic_its", "aclk_pcie_root",	34, 6),
	GATE(ACLK_MMU_PCIE, "aclk_mmu_pcie", "aclk_pcie_bridge",	34, 7),
	GATE(ACLK_MMU_PHP, "aclk_mmu_php", "aclk_php_root",		34, 8),

	GATE(ACLK_USB3OTG2, "aclk_usb3otg2", "aclk_mmu_php",		35, 7),
	GATE(SUSPEND_CLK_USB3OTG2, "suspend_clk_usb3otg2", "xin24m",	35, 8),
	GATE(REF_CLK_USB3OTG2, "ref_clk_usb3otg2", "xin24m",		35, 9),

	GATE(CLK_PIPEPHY0_REF, "clk_pipephy0_ref", "xin24m",		37, 0),
	GATE(CLK_PIPEPHY1_REF, "clk_pipephy1_ref", "xin24m",		37, 1),
	GATE(CLK_PIPEPHY2_REF, "clk_pipephy2_ref", "xin24m",		37, 2),
	GATE(CLK_PMALIVE0, "clk_pmalive0", "xin24m",			37, 4),
	GATE(CLK_PMALIVE1, "clk_pmalive1", "xin24m",			37, 5),
	GATE(CLK_PMALIVE2, "clk_pmalive2", "xin24m",			37, 6),
	GATE(ACLK_SATA0, "aclk_sata0", "aclk_mmu_php",			37, 7),
	GATE(ACLK_SATA1, "aclk_sata1", "aclk_mmu_php",			37, 8),
	GATE(ACLK_SATA2, "aclk_sata2", "aclk_mmu_php",			37, 9),

	GATE(CLK_PIPEPHY0_PIPE_G, "clk_pipephy0_pipe_g", "clk_pipephy0_pipe_i",	38, 3),
	GATE(CLK_PIPEPHY1_PIPE_G, "clk_pipephy1_pipe_g", "clk_pipephy1_pipe_i",	38, 4),
	GATE(CLK_PIPEPHY2_PIPE_G, "clk_pipephy2_pipe_g", "clk_pipephy2_pipe_i",	38, 5),
	GATE(CLK_PIPEPHY0_PIPE_ASIC_G, "clk_pipephy0_pipe_asic_g", "clk_pipephy0_pipe_i", 38, 6),
	GATE(CLK_PIPEPHY1_PIPE_ASIC_G, "clk_pipephy1_pipe_asic_g", "clk_pipephy1_pipe_i", 38, 7),
	GATE(CLK_PIPEPHY2_PIPE_ASIC_G, "clk_pipephy2_pipe_asic_g", "clk_pipephy2_pipe_i", 38, 8),
	GATE(CLK_PIPEPHY2_PIPE_U3_G, "clk_pipephy2_pipe_u3_g", "clk_pipephy2_pipe_i", 38, 9),
#if 1
	GATE(CLK_PCIE1L2_PIPE, "clk_pcie1l2_pipe", "clk_pipephy0_pipe_g", 38, 13),
	GATE(CLK_PCIE1L0_PIPE, "clk_pcie1l0_pipe", "clk_pipephy1_pipe_g", 38, 14),
	GATE(CLK_PCIE1L1_PIPE, "clk_pcie1l1_pipe", "clk_pipephy2_pipe_g", 38, 15),
#else
	GATE(CLK_PCIE1L2_PIPE, "clk_pcie1l2_pipe", "clk_pipephy2_pipe_g", 38, 13),
	GATE(CLK_PCIE1L0_PIPE, "clk_pcie1l0_pipe", "clk_pipephy0_pipe_g", 38, 14),
	GATE(CLK_PCIE1L1_PIPE, "clk_pcie1l1_pipe", "clk_pipephy1_pipe_g", 38, 15),
#endif
	GATE(CLK_PCIE4L_PIPE, "clk_pcie4l_pipe", "clk_pipe30phy_pipe0_i", 39, 0),
	GATE(CLK_PCIE2L_PIPE, "clk_pcie2l_pipe", "clk_pipe30phy_pipe2_i", 39, 1),

	GATE(HCLK_RKVDEC0, "hclk_rkvdec0", "hclk_rkvdec0_pre",		40, 3),
	GATE(ACLK_RKVDEC0, "aclk_rkvdec0", "aclk_rkvdec0_pre",		40, 4),

	GATE(HCLK_RKVDEC1, "hclk_rkvdec1", "hclk_rkvdec1_pre",		41, 2),
	GATE(ACLK_RKVDEC1, "aclk_rkvdec1", "aclk_rkvdec1_pre",		41, 3),

	GATE(ACLK_USB3OTG0, "aclk_usb3otg0", "aclk_usb",		42, 4),
	GATE(SUSPEND_CLK_USB3OTG0, "suspend_clk_usb3otg0", "xin24m",	42, 5),
	GATE(REF_CLK_USB3OTG0, "ref_clk_usb3otg0", "xin24m",		42, 6),
	GATE(ACLK_USB3OTG1, "aclk_usb3otg1", "aclk_usb",		42, 7),
	GATE(SUSPEND_CLK_USB3OTG1, "suspend_clk_usb3otg1", "xin24m",	42, 8),
	GATE(REF_CLK_USB3OTG1, "ref_clk_usb3otg1", "xin24m",		42, 9),
	GATE(HCLK_HOST0, "hclk_host0", "hclk_usb",			42, 10),
	GATE(HCLK_HOST_ARB0, "hclk_host_arb0", "hclk_usb",		42, 11),
	GATE(HCLK_HOST1, "hclk_host1", "hclk_usb",			42, 12),
	GATE(HCLK_HOST_ARB1, "hclk_host_arb1", "hclk_usb",		42, 13),

	GATE(ACLK_VPU, "aclk_vpu", "aclk_vdpu_low_pre",			44, 8),
	GATE(HCLK_VPU, "hclk_vpu", "hclk_vdpu_root",			44, 9),
	GATE(ACLK_JPEG_ENCODER0, "aclk_jpeg_encoder0", "aclk_vdpu_low_pre", 44,	10),
	GATE(HCLK_JPEG_ENCODER0, "hclk_jpeg_encoder0", "hclk_vdpu_root",    44,	11),
	GATE(ACLK_JPEG_ENCODER1, "aclk_jpeg_encoder1", "aclk_vdpu_low_pre", 44,	12),
	GATE(HCLK_JPEG_ENCODER1, "hclk_jpeg_encoder1", "hclk_vdpu_root",    44,	13),
	GATE(ACLK_JPEG_ENCODER2, "aclk_jpeg_encoder2", "aclk_vdpu_low_pre", 44,	14),
	GATE(HCLK_JPEG_ENCODER2, "hclk_jpeg_encoder2", "hclk_vdpu_root",    44,	15),

	GATE(ACLK_JPEG_ENCODER3, "aclk_jpeg_encoder3", "aclk_vdpu_low_pre", 45,	0),
	GATE(HCLK_JPEG_ENCODER3, "hclk_jpeg_encoder3", "hclk_vdpu_root", 45, 1),
	GATE(ACLK_JPEG_DECODER,	"aclk_jpeg_decoder", "aclk_jpeg_decoder_pre", 45, 2),
	GATE(HCLK_JPEG_DECODER,	"hclk_jpeg_decoder", "hclk_vdpu_root",	45, 3),
	GATE(HCLK_IEP2P0, "hclk_iep2p0", "hclk_vdpu_root",		45, 4),
	GATE(ACLK_IEP2P0, "aclk_iep2p0", "aclk_vdpu_low_pre",		45, 5),
	GATE(HCLK_RGA2,	"hclk_rga2", "hclk_vdpu_root",			45, 7),
	GATE(ACLK_RGA2,	"aclk_rga2", "aclk_vdpu_root",			45, 8),
	GATE(HCLK_RGA3_0, "hclk_rga3_0", "hclk_vdpu_root",		45, 10),
	GATE(ACLK_RGA3_0, "aclk_rga3_0", "aclk_vdpu_root",		45, 11),

	GATE(HCLK_RKVENC0, "hclk_rkvenc0", "hclk_rkvenc0_root",		47, 4),
	GATE(ACLK_RKVENC0, "aclk_rkvenc0", "aclk_rkvenc0_root",		47, 5),

	GATE(HCLK_RKVENC1, "hclk_rkvenc1", "hclk_rkvenc1_pre",		48, 4),
	GATE(ACLK_RKVENC1, "aclk_rkvenc1", "aclk_rkvenc1_pre",		48, 5),

	GATE(ACLK_VICAP, "aclk_vicap", "aclk_vi_root",			49, 7),
	GATE(HCLK_VICAP, "hclk_vicap", "hclk_vi_root",			49, 8),
	GATE(CLK_ISP0_CORE_MARVIN, "clk_isp0_core_marvin", "clk_isp0_core", 49,	10),
	GATE(CLK_ISP0_CORE_VICAP, "clk_isp0_core_vicap", "clk_isp0_core", 49, 11),
	GATE(ACLK_ISP0,	"aclk_isp0", "aclk_vi_root",			49, 12),
	GATE(HCLK_ISP0,	"hclk_isp0", "hclk_vi_root",			49, 13),
	GATE(ACLK_FISHEYE0, "aclk_fisheye0", "aclk_vi_root",		49, 14),
	GATE(HCLK_FISHEYE0, "hclk_fisheye0", "hclk_vi_root",		49, 15),

	GATE(ACLK_FISHEYE1, "aclk_fisheye1", "aclk_vi_root",		50, 1),
	GATE(HCLK_FISHEYE1, "hclk_fisheye1", "hclk_vi_root",		50, 2),
	GATE(PCLK_CSI_HOST_0, "pclk_csi_host_0", "pclk_vi_root",	50, 4),
	GATE(PCLK_CSI_HOST_1, "pclk_csi_host_1", "pclk_vi_root",	50, 5),
	GATE(PCLK_CSI_HOST_2, "pclk_csi_host_2", "pclk_vi_root",	50, 6),
	GATE(PCLK_CSI_HOST_3, "pclk_csi_host_3", "pclk_vi_root",	50, 7),
	GATE(PCLK_CSI_HOST_4, "pclk_csi_host_4", "pclk_vi_root",	50, 8),
	GATE(PCLK_CSI_HOST_5, "pclk_csi_host_5", "pclk_vi_root",	50, 9),


	GATE(ICLK_CSIHOST0, "iclk_csihost0", "iclk_csihost01",		51, 11),
	GATE(ICLK_CSIHOST1, "iclk_csihost1", "iclk_csihost01",		51, 12),

	GATE(HCLK_VOP, "hclk_vop", "hclk_vop_root",			52, 8),
	GATE(ACLK_VOP, "aclk_vop", "aclk_vop_sub_src",			52, 9),

	GATE(PCLK_DSIHOST0, "pclk_dsihost0", "pclk_vop_root",		53, 4),
	GATE(PCLK_DSIHOST1, "pclk_dsihost1", "pclk_vop_root",		53, 5),
	GATE(CLK_VOP_PMU, "clk_vop_pmu", "xin24m",			53, 8),
	GATE(ACLK_VOP_DOBY, "aclk_vop_doby", "aclk_vop_root",		53, 10),

	GATE(HCLK_HDCP_KEY0, "hclk_hdcp_key0", "hclk_vo0_s_root",	55, 11),
	GATE(ACLK_HDCP0, "aclk_hdcp0", "aclk_hdcp0_pre",		55, 12),
	GATE(HCLK_HDCP0, "hclk_hdcp0", "hclk_vo0",			55, 13),
	GATE(PCLK_HDCP0, "pclk_hdcp0", "pclk_vo0_root",			55, 14),

	GATE(ACLK_TRNG0, "aclk_trng0", "aclk_vo0_root",			56, 0),
	GATE(PCLK_TRNG0, "pclk_trng0", "pclk_vo0_root",			56, 1),
	GATE(PCLK_DP0, "pclk_dp0", "pclk_vo0_root",			56, 4),
	GATE(PCLK_DP1, "pclk_dp1", "pclk_vo0_root",			56, 5),
	GATE(PCLK_S_DP0, "pclk_s_dp0", "pclk_vo0_s_root",		56, 6),
	GATE(PCLK_S_DP1, "pclk_s_dp1", "pclk_vo0_s_root",		56, 7),
	GATE(CLK_DP0, "clk_dp0", "aclk_vo0_root",			56, 8),
	GATE(CLK_DP1, "clk_dp1", "aclk_vo0_root",			56, 9),
	GATE(HCLK_I2S4_8CH, "hclk_i2s4_8ch", "hclk_vo0",		56, 10),
	GATE(MCLK_I2S4_8CH_TX, "mclk_i2s4_8ch_tx", "clk_i2s4_8ch_tx",	56, 13),
	GATE(HCLK_I2S8_8CH, "hclk_i2s8_8ch", "hclk_vo0",		56, 14),

	GATE(MCLK_I2S8_8CH_TX, "mclk_i2s8_8ch_tx", "clk_i2s8_8ch_tx",	57, 1),

	GATE(HCLK_SPDIF2_DP0, "hclk_spdif2_dp0", "hclk_vo0",		57, 2),
	GATE(MCLK_SPDIF2_DP0, "mclk_spdif2_dp0", "clk_spdif2_dp0",	57, 5),
	GATE(MCLK_SPDIF2, "mclk_spdif2", "clk_spdif2_dp0",		57, 6),
	GATE(HCLK_SPDIF5_DP1, "hclk_spdif5_dp1", "hclk_vo0",		57, 7),
	GATE(MCLK_SPDIF5_DP1, "mclk_spdif5_dp1", "clk_spdif5_dp1",	57, 10),
	GATE(MCLK_SPDIF5, "mclk_spdif5", "clk_spdif5_dp1",		57, 11),

	GATE(PCLK_S_EDP0, "pclk_s_edp0", "pclk_vo1_s_root",		59, 14),
	GATE(PCLK_S_EDP1, "pclk_s_edp1", "pclk_vo1_s_root",		59, 15),

	GATE(HCLK_I2S7_8CH, "hclk_i2s7_8ch", "hclk_vo1",		60, 0),
	GATE(MCLK_I2S7_8CH_RX, "mclk_i2s7_8ch_rx", "clk_i2s7_8ch_rx",	60, 3),
	GATE(HCLK_HDCP_KEY1, "hclk_hdcp_key1", "hclk_vo1_s_root",	60, 4),
	GATE(ACLK_HDCP1, "aclk_hdcp1", "aclk_hdcp1_pre",		60, 5),
	GATE(HCLK_HDCP1, "hclk_hdcp1", "hclk_vo1",			60, 6),
	GATE(PCLK_HDCP1, "pclk_hdcp1", "pclk_vo1_root",			60, 7),
	GATE(ACLK_TRNG1, "aclk_trng1", "aclk_hdcp1_root",		60, 9),
	GATE(PCLK_TRNG1, "pclk_trng1", "pclk_vo1_root",			60, 10),
	GATE(PCLK_HDMITX0, "pclk_hdmitx0", "pclk_vo1_root",		60, 11),

	GATE(CLK_HDMITX0_REF, "clk_hdmitx0_ref", "aclk_hdcp1_root",	61, 0),
	GATE(PCLK_HDMITX1, "pclk_hdmitx1", "pclk_vo1_root",		61, 2),
	GATE(CLK_HDMITX1_REF, "clk_hdmitx1_ref", "aclk_hdcp1_root",	61, 7),
	GATE(ACLK_HDMIRX, "aclk_hdmirx", "aclk_hdmirx_root",		61, 9),
	GATE(PCLK_HDMIRX, "pclk_hdmirx", "pclk_vo1_root",		61, 10),
	GATE(CLK_HDMIRX_REF, "clk_hdmirx_ref", "aclk_hdcp1_root",	61, 11),
	GATE(CLK_HDMIRX_AUD, "clk_hdmirx_aud", "clk_hdmirx_aud_mux",	61, 14),

	GATE(PCLK_EDP0,	"pclk_edp0", "pclk_vo1_root",			62, 0),
	GATE(CLK_EDP0_24M, "clk_edp0_24m", "xin24m",			62, 1),
	GATE(PCLK_EDP1,	"pclk_edp1", "pclk_vo1_root",			62, 3),
	GATE(CLK_EDP1_24M, "clk_edp1_24m", "xin24m",			62, 4),
	GATE(MCLK_I2S5_8CH_TX, "mclk_i2s5_8ch_tx", "clk_i2s5_8ch_tx",	62, 8),
	GATE(HCLK_I2S5_8CH, "hclk_i2s5_8ch", "hclk_vo1",		62, 12),
	GATE(MCLK_I2S6_8CH_TX, "mclk_i2s6_8ch_tx", "clk_i2s6_8ch_tx",	62, 15),

	GATE(MCLK_I2S6_8CH_RX, "mclk_i2s6_8ch_rx", "clk_i2s6_8ch_rx",	63, 2),
	GATE(HCLK_I2S6_8CH, "hclk_i2s6_8ch", "hclk_vo1",		63, 3),
	GATE(HCLK_SPDIF3, "hclk_spdif3", "hclk_vo1",			63, 4),
	GATE(MCLK_SPDIF3, "mclk_spdif3", "clk_spdif3",			63, 7),
	GATE(HCLK_SPDIF4, "hclk_spdif4", "hclk_vo1",			63, 8),

	GATE(MCLK_SPDIF4, "mclk_spdif4", "clk_spdif4",			63, 11),
	GATE(HCLK_SPDIFRX0, "hclk_spdifrx0", "hclk_vo1",		63, 12),
	GATE(HCLK_SPDIFRX1, "hclk_spdifrx1", "hclk_vo1",		63, 14),

	GATE(HCLK_SPDIFRX2, "hclk_spdifrx2", "hclk_vo1",		64, 0),

	GATE(HCLK_I2S9_8CH, "hclk_i2s9_8ch", "hclk_vo1",		65, 0),
	GATE(MCLK_I2S9_8CH_RX, "mclk_i2s9_8ch_rx", "clk_i2s9_8ch_rx",	65, 3),
	GATE(HCLK_I2S10_8CH, "hclk_i2s10_8ch", "hclk_vo1",		65, 4),
	GATE(MCLK_I2S10_8CH_RX,	"mclk_i2s10_8ch_rx", "clk_i2s10_8ch_rx",65, 7),
	GATE(PCLK_S_HDMIRX, "pclk_s_hdmirx", "pclk_vo1_s_root",		65, 8),

	GATE(CLK_GPU, "clk_gpu", "clk_gpu_src",				66, 4),
	GATE(CLK_GPU_COREGROUP,	"clk_gpu_coregroup", "clk_gpu_src",	66, 6),

	GATE(CLK_GPU_PVTM, "clk_gpu_pvtm", "xin24m",			67, 0),
	GATE(CLK_CORE_GPU_PVTM,	"clk_core_gpu_pvtm", "clk_gpu_src",	67, 1),

	GATE(PCLK_AV1, "pclk_av1", "pclk_av1_pre",			68, 5),
	GATE(ACLK_AV1, "aclk_av1", "aclk_av1_pre",			68, 2),

	GATN(ACLK_DMA2DDR, "aclk_dma2ddr", "aclk_center_root",		69, 5),
	GATN(ACLK_DDR_SHAREMEM, "aclk_ddr_sharemem", "aclk_center_low_root", 69, 6),
	GATN(FCLK_DDR_CM0_CORE,	"fclk_ddr_cm0_core", "hclk_center_root",69, 14),

	GATE(CLK_DDR_TIMER0, "clk_ddr_timer0", "clk_ddr_timer_root",	70, 0),
	GATE(CLK_DDR_TIMER1, "clk_ddr_timer1", "clk_ddr_timer_root",	70, 1),
	GATE(TCLK_WDT_DDR, "tclk_wdt_ddr", "xin24m",			70, 2),
	GATE(PCLK_WDT, "pclk_wdt", "pclk_center_root",			70, 7),
	GATE(PCLK_TIMER, "pclk_timer", "pclk_center_root",		70, 8),
	GATN(PCLK_DMA2DDR, "pclk_dma2ddr", "pclk_center_root",		70, 9),
	GATN(PCLK_SHAREMEM, "pclk_sharemem", "pclk_center_root",	70, 10),

	GATE(PCLK_USBDPPHY0, "pclk_usbdpphy0", "pclk_top_root",		72, 2),
	GATE(PCLK_USBDPPHY1, "pclk_usbdpphy1", "pclk_top_root",		72, 4),
	GATE(PCLK_HDPTX0, "pclk_hdptx0", "pclk_top_root",		72, 5),
	GATE(PCLK_HDPTX1, "pclk_hdptx1", "pclk_top_root",		72, 6),

	GATE(CLK_HDMIHDP0, "clk_hdmihdp0", "xin24m",			73, 12),
	GATE(CLK_HDMIHDP1, "clk_hdmihdp1", "xin24m",			73, 13),

	GATE(HCLK_SDIO,	"hclk_sdio", "hclk_sdio_pre",			75, 2),

	GATE(HCLK_RGA3_1, "hclk_rga3_1", "hclk_rga3_root",		76, 4),
	GATE(ACLK_RGA3_1, "aclk_rga3_1", "aclk_rga3_root",		76, 5),

	GATE(CLK_REF_PIPE_PHY0_OSC_SRC,	"clk_ref_pipe_phy0_osc_src", "xin24m", 77, 0),
	GATE(CLK_REF_PIPE_PHY1_OSC_SRC,	"clk_ref_pipe_phy1_osc_src", "xin24m", 77, 1),
	GATE(CLK_REF_PIPE_PHY2_OSC_SRC,	"clk_ref_pipe_phy2_osc_src", "xin24m",	77, 2),

	/* PHP */
	GATE(PCLK_PCIE_COMBO_PIPE_PHY0,	"pclk_pcie_combo_pipe_phy0", "pclk_top_root",	PHP(0),	5),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY1,	"pclk_pcie_combo_pipe_phy1", "pclk_top_root",	PHP(0),	6),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY2,	"pclk_pcie_combo_pipe_phy2", "pclk_top_root",	PHP(0),	7),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY, "pclk_pcie_combo_pipe_phy", "pclk_top_root",	PHP(0),	8),

	/* PMU */
	GATN(FCLK_PMU_CM0_CORE,	"fclk_pmu_cm0_core", "hclk_pmu_cm0_root", PMU(0), 13),

	GATN(PCLK_PMU1,	"pclk_pmu1", "pclk_pmu0_root",			PMU(1),	0),
	GATE(CLK_DDR_FAIL_SAFE,	"clk_ddr_fail_safe", "clk_pmu0",	PMU(1),	1),
	GATN(CLK_PMU1, "clk_pmu1", "clk_pmu0",				PMU(1),	3),
	GATE(PCLK_PMU1_IOC, "pclk_pmu1_ioc", "pclk_pmu0_root",		PMU(1),	5),
	GATE(PCLK_PMU1WDT, "pclk_pmu1wdt", "pclk_pmu0_root",		PMU(1),	6),
	GATE(PCLK_PMU1TIMER, "pclk_pmu1timer", "pclk_pmu0_root",	PMU(1),	8),
	GATE(CLK_PMU1TIMER0, "clk_pmu1timer0", "clk_pmu1timer_root",	PMU(1),	10),
	GATE(CLK_PMU1TIMER1, "clk_pmu1timer1", "clk_pmu1timer_root",	PMU(1),	11),
	GATE(PCLK_PMU1PWM, "pclk_pmu1pwm", "pclk_pmu0_root",		PMU(1),	12),
	GATE(CLK_PMU1PWM_CAPTURE, "clk_pmu1pwm_capture", "xin24m",	PMU(1),	14),

	GATE(PCLK_I2C0,	"pclk_i2c0", "pclk_pmu0_root",			PMU(2),	1),
	GATE(SCLK_UART0, "sclk_uart0", "clk_uart0",			PMU(2),	5),
	GATE(PCLK_UART0, "pclk_uart0", "pclk_pmu0_root",		PMU(2),	6),
	GATE(HCLK_I2S1_8CH, "hclk_i2s1_8ch", "hclk_pmu1_root",		PMU(2),	7),
	GATE(MCLK_I2S1_8CH_TX, "mclk_i2s1_8ch_tx", "clk_i2s1_8ch_tx",	PMU(2),	10),
	GATE(HCLK_PDM0,	"hclk_pdm0", "hclk_pmu1_root",			PMU(2),	14),
	GATE(MCLK_I2S1_8CH_RX, "mclk_i2s1_8ch_rx", "clk_i2s1_8ch_rx",	PMU(2),	13),

	GATE(HCLK_VAD, "hclk_vad", "hclk_pmu1_root",			PMU(3),	0),

	GATE(PCLK_PMU0_ROOT, "pclk_pmu0_root", "pclk_pmu1_root",	PMU(5),	0),
	GATN(CLK_PMU0, "clk_pmu0", "xin24m",				PMU(5),	1),
	GATN(PCLK_PMU0,	"pclk_pmu0", "pclk_pmu0_root",			PMU(5),	2),
	GATN(PCLK_PMU0IOC, "pclk_pmu0ioc", "pclk_pmu0_root",		PMU(5),	4),
	GATE(PCLK_GPIO0, "pclk_gpio0", "pclk_pmu0_root",		PMU(5),	5),

	GATE(CLK_PHY0_REF_ALT_P, "clk_phy0_ref_alt_p", "ppll",		PHYREF_ALT_GATE, 0),
	GATE(CLK_PHY0_REF_ALT_M, "clk_phy0_ref_alt_m", "ppll",		PHYREF_ALT_GATE, 1),
	GATE(CLK_PHY1_REF_ALT_P, "clk_phy1_ref_alt_p", "ppll",		PHYREF_ALT_GATE, 2),
	GATE(CLK_PHY1_REF_ALT_M, "clk_phy1_ref_alt_m", "ppll",		PHYREF_ALT_GATE, 3),

	GATN(PCLK_DSU, "pclk_dsu", "pclk_dsu_root", 			DSU(1), 6),
	GATN(PCLK_DBG, "pclk_dbg", "pclk_dsu_root", 			DSU(1), 7),
	GATE(PCLK_S_DAPLITE, "pclk_s_daplite", "pclk_dsu_ns_root",	DSU(1), 8),
	GATE(PCLK_M_DAPLITE, "pclk_m_daplite", "pclk_dsu_root",		DSU(1), 9),
	GATE(CLK_LITCORE_PVTM, "clk_litcore_pvtm", "xin24m",		DSU(2), 0),
	GATE(CLK_CORE_LITCORE_PVTM, "clk_core_litcore_pvtm", "armclk_l",DSU(2), 1),
	GATN(PCLK_LITCORE_PVTM, "pclk_litcore_pvtm", "pclk_dsu_ns_root", DSU(2), 6),

	/* Big core 0 */
	GATE(CLK_BIGCORE0_PVTM, "clk_bigcore0_pvtm", "xin24m",		BC0(0), 12),
	GATE(CLK_CORE_BIGCORE0_PVTM, "clk_core_bigcore0_pvtm", "armclk_b01", BC0(0), 13),
	GATE(PCLK_BIGCORE0_PVTM, "pclk_bigcore0_pvtm", "pclk_bigcore0_root", BC0(1), 0),

	/* Big core 1 */
	GATE(CLK_BIGCORE1_PVTM, "clk_bigcore1_pvtm", "xin24m",		BC1(0), 12),
	GATE(CLK_CORE_BIGCORE1_PVTM, "clk_core_bigcore1_pvtm", "armclk_b23", BC1(0), 13),
	GATE(PCLK_BIGCORE1_PVTM, "pclk_bigcore1_pvtm", "pclk_bigcore1_root", BC1(1), 0),

	/* Linked gates	*/
	GATL(ACLK_ISP1_PRE, "aclk_isp1_pre", "aclk_isp1_root",	ACLK_VI_ROOT,
							26, 6),
	GATL(HCLK_ISP1_PRE, "hclk_isp1_pre", "hclk_isp1_root",	HCLK_VI_ROOT,
							26, 8),
	GATL(HCLK_NVM,	"hclk_nvm", "hclk_nvm_root", ACLK_NVM_ROOT,
							31, 2),
	GATL(ACLK_USB,	"aclk_usb", "aclk_usb_root", ACLK_VO1USB_TOP_ROOT,
							42, 2),
	GATL(HCLK_USB,	"hclk_usb", "hclk_usb_root", HCLK_VO1USB_TOP_ROOT,
							42, 3),
	GATL(ACLK_JPEG_DECODER_PRE, "aclk_jpeg_decoder_pre", "aclk_jpeg_decoder_root",	ACLK_VDPU_ROOT,
							44, 7),
	GATL(ACLK_VDPU_LOW_PRE, "aclk_vdpu_low_pre", "aclk_vdpu_low_root", ACLK_VDPU_ROOT,
							44, 5),
	GATL(ACLK_RKVENC1_PRE,	"aclk_rkvenc1_pre", "aclk_rkvenc1_root", ACLK_RKVENC0,
							48, 3),
	GATL(HCLK_RKVENC1_PRE,	"hclk_rkvenc1_pre", "hclk_rkvenc1_root", HCLK_RKVENC0,
							48, 2),
	GATL(HCLK_RKVDEC0_PRE,	"hclk_rkvdec0_pre", "hclk_rkvdec0_root", HCLK_VDPU_ROOT,
							40, 5),
	GATL(ACLK_RKVDEC0_PRE,	"aclk_rkvdec0_pre", "aclk_rkvdec0_root", ACLK_VDPU_ROOT,
							40, 6),
	GATL(HCLK_RKVDEC1_PRE,	"hclk_rkvdec1_pre", "hclk_rkvdec1_root", HCLK_VDPU_ROOT,
							41, 4),
	GATL(ACLK_RKVDEC1_PRE,	"aclk_rkvdec1_pre", "aclk_rkvdec1_root", ACLK_VDPU_ROOT,
							41, 5),
	GATL(ACLK_HDCP0_PRE, "aclk_hdcp0_pre",	"aclk_vo0_root", ACLK_VOP_LOW_ROOT,
							55, 9),
	GATL(HCLK_VO0,	"hclk_vo0", "hclk_vo0_root", HCLK_VOP_ROOT,
							55, 5),
	GATL(ACLK_HDCP1_PRE, "aclk_hdcp1_pre",	"aclk_hdcp1_root", ACLK_VO1USB_TOP_ROOT,
							59, 6),
	GATL(HCLK_VO1,	"hclk_vo1", "hclk_vo1_root", HCLK_VO1USB_TOP_ROOT,
							59, 9),
	GATL(ACLK_AV1_PRE, "aclk_av1_pre", "aclk_av1_root", ACLK_VDPU_ROOT,
							68, 1),
	GATL(PCLK_AV1_PRE, "pclk_av1_pre", "pclk_av1_root", HCLK_VDPU_ROOT,
							68, 4),
	GATL(HCLK_SDIO_PRE, "hclk_sdio_pre", "hclk_sdio_root",	HCLK_NVM,
							75, 1),
	GATL(PCLK_VO0GRF, "pclk_vo0grf", "pclk_vo0_root", HCLK_VO0,
							55, 10),
	GATL(PCLK_VO1GRF, "pclk_vo1grf", "pclk_vo1_root", HCLK_VO1,
							59, 12),
};

#define RST_BASE(id, reg, bit) [id] = {id, 0x000A00 + (reg) * 4, (bit)}
#define RST__PHP(id, reg, bit) [id] = {id, 0x008A00 + (reg) * 4, (bit)}
#define RST_SCRU(id, reg, bit) [id] = {id, 0x100A00 + (reg) * 4, (bit)}
#define RST_PMU1(id, reg, bit) [id] = {id, 0x300A00 + (reg) * 4, (bit)}

/* mapping table for reset ID to register offset */
static struct rk_reset_table rk3588_resets[]  = {
	/* SOFTRST_CON01 */
	RST_BASE(SRST_A_TOP_BIU, 1, 3),
	RST_BASE(SRST_P_TOP_BIU, 1, 4),
	RST_BASE(SRST_P_CSIPHY0, 1, 6),
	RST_BASE(SRST_CSIPHY0, 1, 7),
	RST_BASE(SRST_P_CSIPHY1, 1, 8),
	RST_BASE(SRST_CSIPHY1, 1, 9),
	RST_BASE(SRST_A_TOP_M500_BIU, 1, 15),

	/* SOFTRST_CON02 */
	RST_BASE(SRST_A_TOP_M400_BIU, 2, 0),
	RST_BASE(SRST_A_TOP_S200_BIU, 2, 1),
	RST_BASE(SRST_A_TOP_S400_BIU, 2, 2),
	RST_BASE(SRST_A_TOP_M300_BIU, 2, 3),
	RST_BASE(SRST_USBDP_COMBO_PHY0_INIT, 2, 8),
	RST_BASE(SRST_USBDP_COMBO_PHY0_CMN, 2, 9),
	RST_BASE(SRST_USBDP_COMBO_PHY0_LANE, 2, 10),
	RST_BASE(SRST_USBDP_COMBO_PHY0_PCS, 2, 11),
	RST_BASE(SRST_USBDP_COMBO_PHY1_INIT, 2, 15),

	/* SOFTRST_CON03 */
	RST_BASE(SRST_USBDP_COMBO_PHY1_CMN, 3, 0),
	RST_BASE(SRST_USBDP_COMBO_PHY1_LANE, 3, 1),
	RST_BASE(SRST_USBDP_COMBO_PHY1_PCS, 3, 2),
	RST_BASE(SRST_DCPHY0, 3, 11),
	RST_BASE(SRST_P_MIPI_DCPHY0, 3, 14),
	RST_BASE(SRST_P_MIPI_DCPHY0_GRF, 3, 15),

	/* SOFTRST_CON04 */
	RST_BASE(SRST_DCPHY1, 4, 0),
	RST_BASE(SRST_P_MIPI_DCPHY1, 4, 3),
	RST_BASE(SRST_P_MIPI_DCPHY1_GRF, 4, 4),
	RST_BASE(SRST_P_APB2ASB_SLV_CDPHY, 4, 5),
	RST_BASE(SRST_P_APB2ASB_SLV_CSIPHY, 4, 6),
	RST_BASE(SRST_P_APB2ASB_SLV_VCCIO3_5, 4, 7),
	RST_BASE(SRST_P_APB2ASB_SLV_VCCIO6, 4, 8),
	RST_BASE(SRST_P_APB2ASB_SLV_EMMCIO, 4, 9),
	RST_BASE(SRST_P_APB2ASB_SLV_IOC_TOP, 4, 10),
	RST_BASE(SRST_P_APB2ASB_SLV_IOC_RIGHT, 4, 11),

	/* SOFTRST_CON05 */
	RST_BASE(SRST_P_CRU, 5, 0),
	RST_BASE(SRST_A_CHANNEL_SECURE2VO1USB, 5, 7),
	RST_BASE(SRST_A_CHANNEL_SECURE2CENTER, 5, 8),
	RST_BASE(SRST_H_CHANNEL_SECURE2VO1USB, 5, 14),
	RST_BASE(SRST_H_CHANNEL_SECURE2CENTER, 5, 15),

	/* SOFTRST_CON06 */
	RST_BASE(SRST_P_CHANNEL_SECURE2VO1USB, 6, 0),
	RST_BASE(SRST_P_CHANNEL_SECURE2CENTER, 6, 1),

	/* SOFTRST_CON07 */
	RST_BASE(SRST_H_AUDIO_BIU, 7, 2),
	RST_BASE(SRST_P_AUDIO_BIU, 7, 3),
	RST_BASE(SRST_H_I2S0_8CH, 7, 4),
	RST_BASE(SRST_M_I2S0_8CH_TX, 7, 7),
	RST_BASE(SRST_M_I2S0_8CH_RX, 7, 10),
	RST_BASE(SRST_P_ACDCDIG, 7, 11),
	RST_BASE(SRST_H_I2S2_2CH, 7, 12),
	RST_BASE(SRST_H_I2S3_2CH, 7, 13),

	/* SOFTRST_CON08 */
	RST_BASE(SRST_M_I2S2_2CH, 8, 0),
	RST_BASE(SRST_M_I2S3_2CH, 8, 3),
	RST_BASE(SRST_DAC_ACDCDIG, 8, 4),
	RST_BASE(SRST_H_SPDIF0, 8, 14),

	/* SOFTRST_CON09 */
	RST_BASE(SRST_M_SPDIF0, 9, 1),
	RST_BASE(SRST_H_SPDIF1, 9, 2),
	RST_BASE(SRST_M_SPDIF1, 9, 5),
	RST_BASE(SRST_H_PDM1, 9, 6),
	RST_BASE(SRST_PDM1, 9, 7),

	/* SOFTRST_CON10 */
	RST_BASE(SRST_A_BUS_BIU, 10, 1),
	RST_BASE(SRST_P_BUS_BIU, 10, 2),
	RST_BASE(SRST_A_GIC, 10, 3),
	RST_BASE(SRST_A_GIC_DBG, 10, 4),
	RST_BASE(SRST_A_DMAC0, 10, 5),
	RST_BASE(SRST_A_DMAC1, 10, 6),
	RST_BASE(SRST_A_DMAC2, 10, 7),
	RST_BASE(SRST_P_I2C1, 10, 8),
	RST_BASE(SRST_P_I2C2, 10, 9),
	RST_BASE(SRST_P_I2C3, 10, 10),
	RST_BASE(SRST_P_I2C4, 10, 11),
	RST_BASE(SRST_P_I2C5, 10, 12),
	RST_BASE(SRST_P_I2C6, 10, 13),
	RST_BASE(SRST_P_I2C7, 10, 14),
	RST_BASE(SRST_P_I2C8, 10, 15),

	/* SOFTRST_CON11 */
	RST_BASE(SRST_I2C1, 11, 0),
	RST_BASE(SRST_I2C2, 11, 1),
	RST_BASE(SRST_I2C3, 11, 2),
	RST_BASE(SRST_I2C4, 11, 3),
	RST_BASE(SRST_I2C5, 11, 4),
	RST_BASE(SRST_I2C6, 11, 5),
	RST_BASE(SRST_I2C7, 11, 6),
	RST_BASE(SRST_I2C8, 11, 7),
	RST_BASE(SRST_P_CAN0, 11, 8),
	RST_BASE(SRST_CAN0, 11, 9),
	RST_BASE(SRST_P_CAN1, 11, 10),
	RST_BASE(SRST_CAN1, 11, 11),
	RST_BASE(SRST_P_CAN2, 11, 12),
	RST_BASE(SRST_CAN2, 11, 13),
	RST_BASE(SRST_P_SARADC, 11, 14),

	/* SOFTRST_CON12 */
	RST_BASE(SRST_P_TSADC, 12, 0),
	RST_BASE(SRST_TSADC, 12, 1),
	RST_BASE(SRST_P_UART1, 12, 2),
	RST_BASE(SRST_P_UART2, 12, 3),
	RST_BASE(SRST_P_UART3, 12, 4),
	RST_BASE(SRST_P_UART4, 12, 5),
	RST_BASE(SRST_P_UART5, 12, 6),
	RST_BASE(SRST_P_UART6, 12, 7),
	RST_BASE(SRST_P_UART7, 12, 8),
	RST_BASE(SRST_P_UART8, 12, 9),
	RST_BASE(SRST_P_UART9, 12, 10),
	RST_BASE(SRST_S_UART1, 12, 13),

	/* SOFTRST_CON13 */
	RST_BASE(SRST_S_UART2, 13, 0),
	RST_BASE(SRST_S_UART3, 13, 3),
	RST_BASE(SRST_S_UART4, 13, 6),
	RST_BASE(SRST_S_UART5, 13, 9),
	RST_BASE(SRST_S_UART6, 13, 12),
	RST_BASE(SRST_S_UART7, 13, 15),

	/* SOFTRST_CON14 */
	RST_BASE(SRST_S_UART8, 14, 2),
	RST_BASE(SRST_S_UART9, 14, 5),
	RST_BASE(SRST_P_SPI0, 14, 6),
	RST_BASE(SRST_P_SPI1, 14, 7),
	RST_BASE(SRST_P_SPI2, 14, 8),
	RST_BASE(SRST_P_SPI3, 14, 9),
	RST_BASE(SRST_P_SPI4, 14, 10),
	RST_BASE(SRST_SPI0, 14, 11),
	RST_BASE(SRST_SPI1, 14, 12),
	RST_BASE(SRST_SPI2, 14, 13),
	RST_BASE(SRST_SPI3, 14, 14),
	RST_BASE(SRST_SPI4, 14, 15),

	/* SOFTRST_CON15 */
	RST_BASE(SRST_P_WDT0, 15, 0),
	RST_BASE(SRST_T_WDT0, 15, 1),
	RST_BASE(SRST_P_SYS_GRF, 15, 2),
	RST_BASE(SRST_P_PWM1, 15, 3),
	RST_BASE(SRST_PWM1, 15, 4),
	RST_BASE(SRST_P_PWM2, 15, 6),
	RST_BASE(SRST_PWM2, 15, 7),
	RST_BASE(SRST_P_PWM3, 15, 9),
	RST_BASE(SRST_PWM3, 15, 10),
	RST_BASE(SRST_P_BUSTIMER0, 15, 12),
	RST_BASE(SRST_P_BUSTIMER1, 15, 13),
	RST_BASE(SRST_BUSTIMER0, 15, 15),

	/* SOFTRST_CON16 */
	RST_BASE(SRST_BUSTIMER1, 16, 0),
	RST_BASE(SRST_BUSTIMER2, 16, 1),
	RST_BASE(SRST_BUSTIMER3, 16, 2),
	RST_BASE(SRST_BUSTIMER4, 16, 3),
	RST_BASE(SRST_BUSTIMER5, 16, 4),
	RST_BASE(SRST_BUSTIMER6, 16, 5),
	RST_BASE(SRST_BUSTIMER7, 16, 6),
	RST_BASE(SRST_BUSTIMER8, 16, 7),
	RST_BASE(SRST_BUSTIMER9, 16, 8),
	RST_BASE(SRST_BUSTIMER10, 16, 9),
	RST_BASE(SRST_BUSTIMER11, 16, 10),
	RST_BASE(SRST_P_MAILBOX0, 16, 11),
	RST_BASE(SRST_P_MAILBOX1, 16, 12),
	RST_BASE(SRST_P_MAILBOX2, 16, 13),
	RST_BASE(SRST_P_GPIO1, 16, 14),
	RST_BASE(SRST_GPIO1, 16, 15),

	/* SOFTRST_CON17 */
	RST_BASE(SRST_P_GPIO2, 17, 0),
	RST_BASE(SRST_GPIO2, 17, 1),
	RST_BASE(SRST_P_GPIO3, 17, 2),
	RST_BASE(SRST_GPIO3, 17, 3),
	RST_BASE(SRST_P_GPIO4, 17, 4),
	RST_BASE(SRST_GPIO4, 17, 5),
	RST_BASE(SRST_A_DECOM, 17, 6),
	RST_BASE(SRST_P_DECOM, 17, 7),
	RST_BASE(SRST_D_DECOM, 17, 8),
	RST_BASE(SRST_P_TOP, 17, 9),
	RST_BASE(SRST_A_GICADB_GIC2CORE_BUS, 17, 11),
	RST_BASE(SRST_P_DFT2APB, 17, 12),
	RST_BASE(SRST_P_APB2ASB_MST_TOP, 17, 13),
	RST_BASE(SRST_P_APB2ASB_MST_CDPHY, 17, 14),
	RST_BASE(SRST_P_APB2ASB_MST_BOT_RIGHT, 17, 15),

	/* SOFTRST_CON18 */
	RST_BASE(SRST_P_APB2ASB_MST_IOC_TOP, 18, 0),
	RST_BASE(SRST_P_APB2ASB_MST_IOC_RIGHT, 18, 1),
	RST_BASE(SRST_P_APB2ASB_MST_CSIPHY, 18, 2),
	RST_BASE(SRST_P_APB2ASB_MST_VCCIO3_5, 18, 3),
	RST_BASE(SRST_P_APB2ASB_MST_VCCIO6, 18, 4),
	RST_BASE(SRST_P_APB2ASB_MST_EMMCIO, 18, 5),
	RST_BASE(SRST_A_SPINLOCK, 18, 6),
	RST_BASE(SRST_P_OTPC_NS, 18, 9),
	RST_BASE(SRST_OTPC_NS, 18, 10),
	RST_BASE(SRST_OTPC_ARB, 18, 11),

	/* SOFTRST_CON19 */
	RST_BASE(SRST_P_BUSIOC, 19, 0),
	RST_BASE(SRST_P_PMUCM0_INTMUX, 19, 4),
	RST_BASE(SRST_P_DDRCM0_INTMUX, 19, 5),

	/* SOFTRST_CON20 */
	RST_BASE(SRST_P_DDR_DFICTL_CH0, 20, 0),
	RST_BASE(SRST_P_DDR_MON_CH0, 20, 1),
	RST_BASE(SRST_P_DDR_STANDBY_CH0, 20, 2),
	RST_BASE(SRST_P_DDR_UPCTL_CH0, 20, 3),
	RST_BASE(SRST_TM_DDR_MON_CH0, 20, 4),
	RST_BASE(SRST_P_DDR_GRF_CH01, 20, 5),
	RST_BASE(SRST_DFI_CH0, 20, 6),
	RST_BASE(SRST_SBR_CH0, 20, 7),
	RST_BASE(SRST_DDR_UPCTL_CH0, 20, 8),
	RST_BASE(SRST_DDR_DFICTL_CH0, 20, 9),
	RST_BASE(SRST_DDR_MON_CH0, 20, 10),
	RST_BASE(SRST_DDR_STANDBY_CH0, 20, 11),
	RST_BASE(SRST_A_DDR_UPCTL_CH0, 20, 12),
	RST_BASE(SRST_P_DDR_DFICTL_CH1, 20, 13),
	RST_BASE(SRST_P_DDR_MON_CH1, 20, 14),
	RST_BASE(SRST_P_DDR_STANDBY_CH1, 20, 15),

	/* SOFTRST_CON21 */
	RST_BASE(SRST_P_DDR_UPCTL_CH1, 21, 0),
	RST_BASE(SRST_TM_DDR_MON_CH1, 21, 1),
	RST_BASE(SRST_DFI_CH1, 21, 2),
	RST_BASE(SRST_SBR_CH1, 21, 3),
	RST_BASE(SRST_DDR_UPCTL_CH1, 21, 4),
	RST_BASE(SRST_DDR_DFICTL_CH1, 21, 5),
	RST_BASE(SRST_DDR_MON_CH1, 21, 6),
	RST_BASE(SRST_DDR_STANDBY_CH1, 21, 7),
	RST_BASE(SRST_A_DDR_UPCTL_CH1, 21, 8),
	RST_BASE(SRST_A_DDR01_MSCH0, 21, 13),
	RST_BASE(SRST_A_DDR01_RS_MSCH0, 21, 14),
	RST_BASE(SRST_A_DDR01_FRS_MSCH0, 21, 15),

	/* SOFTRST_CON22 */
	RST_BASE(SRST_A_DDR01_SCRAMBLE0, 22, 0),
	RST_BASE(SRST_A_DDR01_FRS_SCRAMBLE0, 22, 1),
	RST_BASE(SRST_A_DDR01_MSCH1, 22, 2),
	RST_BASE(SRST_A_DDR01_RS_MSCH1, 22, 3),
	RST_BASE(SRST_A_DDR01_FRS_MSCH1, 22, 4),
	RST_BASE(SRST_A_DDR01_SCRAMBLE1, 22, 5),
	RST_BASE(SRST_A_DDR01_FRS_SCRAMBLE1, 22, 6),
	RST_BASE(SRST_P_DDR01_MSCH0, 22, 7),
	RST_BASE(SRST_P_DDR01_MSCH1, 22, 8),

	/* SOFTRST_CON23 */
	RST_BASE(SRST_P_DDR_DFICTL_CH2, 23, 0),
	RST_BASE(SRST_P_DDR_MON_CH2, 23, 1),
	RST_BASE(SRST_P_DDR_STANDBY_CH2, 23, 2),
	RST_BASE(SRST_P_DDR_UPCTL_CH2, 23, 3),
	RST_BASE(SRST_TM_DDR_MON_CH2, 23, 4),
	RST_BASE(SRST_P_DDR_GRF_CH23, 23, 5),
	RST_BASE(SRST_DFI_CH2, 23, 6),
	RST_BASE(SRST_SBR_CH2, 23, 7),
	RST_BASE(SRST_DDR_UPCTL_CH2, 23, 8),
	RST_BASE(SRST_DDR_DFICTL_CH2, 23, 9),
	RST_BASE(SRST_DDR_MON_CH2, 23, 10),
	RST_BASE(SRST_DDR_STANDBY_CH2, 23, 11),
	RST_BASE(SRST_A_DDR_UPCTL_CH2, 23, 12),
	RST_BASE(SRST_P_DDR_DFICTL_CH3, 23, 13),
	RST_BASE(SRST_P_DDR_MON_CH3, 23, 14),
	RST_BASE(SRST_P_DDR_STANDBY_CH3, 23, 15),

	/* SOFTRST_CON24 */
	RST_BASE(SRST_P_DDR_UPCTL_CH3, 24, 0),
	RST_BASE(SRST_TM_DDR_MON_CH3, 24, 1),
	RST_BASE(SRST_DFI_CH3, 24, 2),
	RST_BASE(SRST_SBR_CH3, 24, 3),
	RST_BASE(SRST_DDR_UPCTL_CH3, 24, 4),
	RST_BASE(SRST_DDR_DFICTL_CH3, 24, 5),
	RST_BASE(SRST_DDR_MON_CH3, 24, 6),
	RST_BASE(SRST_DDR_STANDBY_CH3, 24, 7),
	RST_BASE(SRST_A_DDR_UPCTL_CH3, 24, 8),
	RST_BASE(SRST_A_DDR23_MSCH2, 24, 13),
	RST_BASE(SRST_A_DDR23_RS_MSCH2, 24, 14),
	RST_BASE(SRST_A_DDR23_FRS_MSCH2, 24, 15),

	/* SOFTRST_CON25 */
	RST_BASE(SRST_A_DDR23_SCRAMBLE2, 25, 0),
	RST_BASE(SRST_A_DDR23_FRS_SCRAMBLE2, 25, 1),
	RST_BASE(SRST_A_DDR23_MSCH3, 25, 2),
	RST_BASE(SRST_A_DDR23_RS_MSCH3, 25, 3),
	RST_BASE(SRST_A_DDR23_FRS_MSCH3, 25, 4),
	RST_BASE(SRST_A_DDR23_SCRAMBLE3, 25, 5),
	RST_BASE(SRST_A_DDR23_FRS_SCRAMBLE3, 25, 6),
	RST_BASE(SRST_P_DDR23_MSCH2, 25, 7),
	RST_BASE(SRST_P_DDR23_MSCH3, 25, 8),

	/* SOFTRST_CON26 */
	RST_BASE(SRST_ISP1, 26, 3),
	RST_BASE(SRST_ISP1_VICAP, 26, 4),
	RST_BASE(SRST_A_ISP1_BIU, 26, 6),
	RST_BASE(SRST_H_ISP1_BIU, 26, 8),

	/* SOFTRST_CON27 */
	RST_BASE(SRST_A_RKNN1, 27, 0),
	RST_BASE(SRST_A_RKNN1_BIU, 27, 1),
	RST_BASE(SRST_H_RKNN1, 27, 2),
	RST_BASE(SRST_H_RKNN1_BIU, 27, 3),

	/* SOFTRST_CON28 */
	RST_BASE(SRST_A_RKNN2, 28, 0),
	RST_BASE(SRST_A_RKNN2_BIU, 28, 1),
	RST_BASE(SRST_H_RKNN2, 28, 2),
	RST_BASE(SRST_H_RKNN2_BIU, 28, 3),

	/* SOFTRST_CON29 */
	RST_BASE(SRST_A_RKNN_DSU0, 29, 3),
	RST_BASE(SRST_P_NPUTOP_BIU, 29, 5),
	RST_BASE(SRST_P_NPU_TIMER, 29, 6),
	RST_BASE(SRST_NPUTIMER0, 29, 8),
	RST_BASE(SRST_NPUTIMER1, 29, 9),
	RST_BASE(SRST_P_NPU_WDT, 29, 10),
	RST_BASE(SRST_T_NPU_WDT, 29, 11),
	RST_BASE(SRST_P_NPU_PVTM, 29, 12),
	RST_BASE(SRST_P_NPU_GRF, 29, 13),
	RST_BASE(SRST_NPU_PVTM, 29, 14),

	/* SOFTRST_CON30 */
	RST_BASE(SRST_NPU_PVTPLL, 30, 0),
	RST_BASE(SRST_H_NPU_CM0_BIU, 30, 2),
	RST_BASE(SRST_F_NPU_CM0_CORE, 30, 3),
	RST_BASE(SRST_T_NPU_CM0_JTAG, 30, 4),
	RST_BASE(SRST_A_RKNN0, 30, 6),
	RST_BASE(SRST_A_RKNN0_BIU, 30, 7),
	RST_BASE(SRST_H_RKNN0, 30, 8),
	RST_BASE(SRST_H_RKNN0_BIU, 30, 9),

	/* SOFTRST_CON31 */
	RST_BASE(SRST_H_NVM_BIU, 31, 2),
	RST_BASE(SRST_A_NVM_BIU, 31, 3),
	RST_BASE(SRST_H_EMMC, 31, 4),
	RST_BASE(SRST_A_EMMC, 31, 5),
	RST_BASE(SRST_C_EMMC, 31, 6),
	RST_BASE(SRST_B_EMMC, 31, 7),
	RST_BASE(SRST_T_EMMC, 31, 8),
	RST_BASE(SRST_S_SFC, 31, 9),
	RST_BASE(SRST_H_SFC, 31, 10),
	RST_BASE(SRST_H_SFC_XIP, 31, 11),

	/* SOFTRST_CON32 */
	RST_BASE(SRST_P_GRF, 32, 1),
	RST_BASE(SRST_P_DEC_BIU, 32, 2),
	RST_BASE(SRST_P_PHP_BIU, 32, 5),
	RST_BASE(SRST_A_PCIE_GRIDGE, 32, 8),
	RST_BASE(SRST_A_PHP_BIU, 32, 9),
	RST_BASE(SRST_A_GMAC0, 32, 10),
	RST_BASE(SRST_A_GMAC1, 32, 11),
	RST_BASE(SRST_A_PCIE_BIU, 32, 12),
	RST_BASE(SRST_PCIE0_POWER_UP, 32, 13),
	RST_BASE(SRST_PCIE1_POWER_UP, 32, 14),
	RST_BASE(SRST_PCIE2_POWER_UP, 32, 15),

	/* SOFTRST_CON33 */
	RST_BASE(SRST_PCIE3_POWER_UP, 33, 0),
	RST_BASE(SRST_PCIE4_POWER_UP, 33, 1),
	RST_BASE(SRST_P_PCIE0, 33, 12),
	RST_BASE(SRST_P_PCIE1, 33, 13),
	RST_BASE(SRST_P_PCIE2, 33, 14),
	RST_BASE(SRST_P_PCIE3, 33, 15),

	/* SOFTRST_CON34 */
	RST_BASE(SRST_P_PCIE4, 34, 0),
	RST_BASE(SRST_A_PHP_GIC_ITS, 34, 6),
	RST_BASE(SRST_A_MMU_PCIE, 34, 7),
	RST_BASE(SRST_A_MMU_PHP, 34, 8),
	RST_BASE(SRST_A_MMU_BIU, 34, 9),

	/* SOFTRST_CON35 */
	RST_BASE(SRST_A_USB3OTG2, 35, 7),

	/* SOFTRST_CON37 */
	RST_BASE(SRST_PMALIVE0, 37, 4),
	RST_BASE(SRST_PMALIVE1, 37, 5),
	RST_BASE(SRST_PMALIVE2, 37, 6),
	RST_BASE(SRST_A_SATA0, 37, 7),
	RST_BASE(SRST_A_SATA1, 37, 8),
	RST_BASE(SRST_A_SATA2, 37, 9),
	RST_BASE(SRST_RXOOB0, 37, 10),
	RST_BASE(SRST_RXOOB1, 37, 11),
	RST_BASE(SRST_RXOOB2, 37, 12),
	RST_BASE(SRST_ASIC0, 37, 13),
	RST_BASE(SRST_ASIC1, 37, 14),
	RST_BASE(SRST_ASIC2, 37, 15),

	/* SOFTRST_CON40 */
	RST_BASE(SRST_A_RKVDEC_CCU, 40, 2),
	RST_BASE(SRST_H_RKVDEC0, 40, 3),
	RST_BASE(SRST_A_RKVDEC0, 40, 4),
	RST_BASE(SRST_H_RKVDEC0_BIU, 40, 5),
	RST_BASE(SRST_A_RKVDEC0_BIU, 40, 6),
	RST_BASE(SRST_RKVDEC0_CA, 40, 7),
	RST_BASE(SRST_RKVDEC0_HEVC_CA, 40, 8),
	RST_BASE(SRST_RKVDEC0_CORE, 40, 9),

	/* SOFTRST_CON41 */
	RST_BASE(SRST_H_RKVDEC1, 41, 2),
	RST_BASE(SRST_A_RKVDEC1, 41, 3),
	RST_BASE(SRST_H_RKVDEC1_BIU, 41, 4),
	RST_BASE(SRST_A_RKVDEC1_BIU, 41, 5),
	RST_BASE(SRST_RKVDEC1_CA, 41, 6),
	RST_BASE(SRST_RKVDEC1_HEVC_CA, 41, 7),
	RST_BASE(SRST_RKVDEC1_CORE, 41, 8),

	/* SOFTRST_CON42 */
	RST_BASE(SRST_A_USB_BIU, 42, 2),
	RST_BASE(SRST_H_USB_BIU, 42, 3),
	RST_BASE(SRST_A_USB3OTG0, 42, 4),
	RST_BASE(SRST_A_USB3OTG1, 42, 7),
	RST_BASE(SRST_H_HOST0, 42, 10),
	RST_BASE(SRST_H_HOST_ARB0, 42, 11),
	RST_BASE(SRST_H_HOST1, 42, 12),
	RST_BASE(SRST_H_HOST_ARB1, 42, 13),
	RST_BASE(SRST_A_USB_GRF, 42, 14),
	RST_BASE(SRST_C_USB2P0_HOST0, 42, 15),

	/* SOFTRST_CON43 */
	RST_BASE(SRST_C_USB2P0_HOST1, 43, 0),
	RST_BASE(SRST_HOST_UTMI0, 43, 1),
	RST_BASE(SRST_HOST_UTMI1, 43, 2),

	/* SOFTRST_CON44 */
	RST_BASE(SRST_A_VDPU_BIU, 44, 4),
	RST_BASE(SRST_A_VDPU_LOW_BIU, 44, 5),
	RST_BASE(SRST_H_VDPU_BIU, 44, 6),
	RST_BASE(SRST_A_JPEG_DECODER_BIU, 44, 7),
	RST_BASE(SRST_A_VPU, 44, 8),
	RST_BASE(SRST_H_VPU, 44, 9),
	RST_BASE(SRST_A_JPEG_ENCODER0, 44, 10),
	RST_BASE(SRST_H_JPEG_ENCODER0, 44, 11),
	RST_BASE(SRST_A_JPEG_ENCODER1, 44, 12),
	RST_BASE(SRST_H_JPEG_ENCODER1, 44, 13),
	RST_BASE(SRST_A_JPEG_ENCODER2, 44, 14),
	RST_BASE(SRST_H_JPEG_ENCODER2, 44, 15),

	/* SOFTRST_CON45 */
	RST_BASE(SRST_A_JPEG_ENCODER3, 45, 0),
	RST_BASE(SRST_H_JPEG_ENCODER3, 45, 1),
	RST_BASE(SRST_A_JPEG_DECODER, 45, 2),
	RST_BASE(SRST_H_JPEG_DECODER, 45, 3),
	RST_BASE(SRST_H_IEP2P0, 45, 4),
	RST_BASE(SRST_A_IEP2P0, 45, 5),
	RST_BASE(SRST_IEP2P0_CORE, 45, 6),
	RST_BASE(SRST_H_RGA2, 45, 7),
	RST_BASE(SRST_A_RGA2, 45, 8),
	RST_BASE(SRST_RGA2_CORE, 45, 9),
	RST_BASE(SRST_H_RGA3_0, 45, 10),
	RST_BASE(SRST_A_RGA3_0, 45, 11),
	RST_BASE(SRST_RGA3_0_CORE, 45, 12),

	/* SOFTRST_CON47 */
	RST_BASE(SRST_H_RKVENC0_BIU, 47, 2),
	RST_BASE(SRST_A_RKVENC0_BIU, 47, 3),
	RST_BASE(SRST_H_RKVENC0, 47, 4),
	RST_BASE(SRST_A_RKVENC0, 47, 5),
	RST_BASE(SRST_RKVENC0_CORE, 47, 6),

	/* SOFTRST_CON48 */
	RST_BASE(SRST_H_RKVENC1_BIU, 48, 2),
	RST_BASE(SRST_A_RKVENC1_BIU, 48, 3),
	RST_BASE(SRST_H_RKVENC1, 48, 4),
	RST_BASE(SRST_A_RKVENC1, 48, 5),
	RST_BASE(SRST_RKVENC1_CORE, 48, 6),

	/* SOFTRST_CON49 */
	RST_BASE(SRST_A_VI_BIU, 49, 3),
	RST_BASE(SRST_H_VI_BIU, 49, 4),
	RST_BASE(SRST_P_VI_BIU, 49, 5),
	RST_BASE(SRST_D_VICAP, 49, 6),
	RST_BASE(SRST_A_VICAP, 49, 7),
	RST_BASE(SRST_H_VICAP, 49, 8),
	RST_BASE(SRST_ISP0, 49, 10),
	RST_BASE(SRST_ISP0_VICAP, 49, 11),

	/* SOFTRST_CON50 */
	RST_BASE(SRST_FISHEYE0, 50, 0),
	RST_BASE(SRST_FISHEYE1, 50, 3),
	RST_BASE(SRST_P_CSI_HOST_0, 50, 4),
	RST_BASE(SRST_P_CSI_HOST_1, 50, 5),
	RST_BASE(SRST_P_CSI_HOST_2, 50, 6),
	RST_BASE(SRST_P_CSI_HOST_3, 50, 7),
	RST_BASE(SRST_P_CSI_HOST_4, 50, 8),
	RST_BASE(SRST_P_CSI_HOST_5, 50, 9),

	/* SOFTRST_CON51 */
	RST_BASE(SRST_CSIHOST0_VICAP, 51, 4),
	RST_BASE(SRST_CSIHOST1_VICAP, 51, 5),
	RST_BASE(SRST_CSIHOST2_VICAP, 51, 6),
	RST_BASE(SRST_CSIHOST3_VICAP, 51, 7),
	RST_BASE(SRST_CSIHOST4_VICAP, 51, 8),
	RST_BASE(SRST_CSIHOST5_VICAP, 51, 9),
	RST_BASE(SRST_CIFIN, 51, 13),

	/* SOFTRST_CON52 */
	RST_BASE(SRST_A_VOP_BIU, 52, 4),
	RST_BASE(SRST_A_VOP_LOW_BIU, 52, 5),
	RST_BASE(SRST_H_VOP_BIU, 52, 6),
	RST_BASE(SRST_P_VOP_BIU, 52, 7),
	RST_BASE(SRST_H_VOP, 52, 8),
	RST_BASE(SRST_A_VOP, 52, 9),
	RST_BASE(SRST_D_VOP0, 52, 13),
	RST_BASE(SRST_D_VOP2HDMI_BRIDGE0, 52, 14),
	RST_BASE(SRST_D_VOP2HDMI_BRIDGE1, 52, 15),

	/* SOFTRST_CON53 */
	RST_BASE(SRST_D_VOP1, 53, 0),
	RST_BASE(SRST_D_VOP2, 53, 1),
	RST_BASE(SRST_D_VOP3, 53, 2),
	RST_BASE(SRST_P_VOPGRF, 53, 3),
	RST_BASE(SRST_P_DSIHOST0, 53, 4),
	RST_BASE(SRST_P_DSIHOST1, 53, 5),
	RST_BASE(SRST_DSIHOST0, 53, 6),
	RST_BASE(SRST_DSIHOST1, 53, 7),
	RST_BASE(SRST_VOP_PMU, 53, 8),
	RST_BASE(SRST_P_VOP_CHANNEL_BIU, 53, 9),

	/* SOFTRST_CON55 */
	RST_BASE(SRST_H_VO0_BIU, 55, 5),
	RST_BASE(SRST_H_VO0_S_BIU, 55, 6),
	RST_BASE(SRST_P_VO0_BIU, 55, 7),
	RST_BASE(SRST_P_VO0_S_BIU, 55, 8),
	RST_BASE(SRST_A_HDCP0_BIU, 55, 9),
	RST_BASE(SRST_P_VO0GRF, 55, 10),
	RST_BASE(SRST_H_HDCP_KEY0, 55, 11),
	RST_BASE(SRST_A_HDCP0, 55, 12),
	RST_BASE(SRST_H_HDCP0, 55, 13),
	RST_BASE(SRST_HDCP0, 55, 15),

	/* SOFTRST_CON56 */
	RST_BASE(SRST_P_TRNG0, 56, 1),
	RST_BASE(SRST_DP0, 56, 8),
	RST_BASE(SRST_DP1, 56, 9),
	RST_BASE(SRST_H_I2S4_8CH, 56, 10),
	RST_BASE(SRST_M_I2S4_8CH_TX, 56, 13),
	RST_BASE(SRST_H_I2S8_8CH, 56, 14),

	/* SOFTRST_CON57 */
	RST_BASE(SRST_M_I2S8_8CH_TX, 57, 1),
	RST_BASE(SRST_H_SPDIF2_DP0, 57, 2),
	RST_BASE(SRST_M_SPDIF2_DP0, 57, 6),
	RST_BASE(SRST_H_SPDIF5_DP1, 57, 7),
	RST_BASE(SRST_M_SPDIF5_DP1, 57, 11),

	/* SOFTRST_CON59 */
	RST_BASE(SRST_A_HDCP1_BIU, 59, 6),
	RST_BASE(SRST_A_HDMIRX_BIU, 59, 7),
	RST_BASE(SRST_A_VO1_BIU, 59, 8),
	RST_BASE(SRST_H_VOP1_BIU, 59, 9),
	RST_BASE(SRST_H_VOP1_S_BIU, 59, 10),
	RST_BASE(SRST_P_VOP1_BIU, 59, 11),
	RST_BASE(SRST_P_VO1GRF, 59, 12),
	RST_BASE(SRST_P_VO1_S_BIU, 59, 13),

	/* SOFTRST_CON60 */
	RST_BASE(SRST_H_I2S7_8CH, 60, 0),
	RST_BASE(SRST_M_I2S7_8CH_RX, 60, 3),
	RST_BASE(SRST_H_HDCP_KEY1, 60, 4),
	RST_BASE(SRST_A_HDCP1, 60, 5),
	RST_BASE(SRST_H_HDCP1, 60, 6),
	RST_BASE(SRST_HDCP1, 60, 8),
	RST_BASE(SRST_P_TRNG1, 60, 10),
	RST_BASE(SRST_P_HDMITX0, 60, 11),

	/* SOFTRST_CON61 */
	RST_BASE(SRST_HDMITX0_REF, 61, 0),
	RST_BASE(SRST_P_HDMITX1, 61, 2),
	RST_BASE(SRST_HDMITX1_REF, 61, 7),
	RST_BASE(SRST_A_HDMIRX, 61, 9),
	RST_BASE(SRST_P_HDMIRX, 61, 10),
	RST_BASE(SRST_HDMIRX_REF, 61, 11),

	/* SOFTRST_CON62 */
	RST_BASE(SRST_P_EDP0, 62, 0),
	RST_BASE(SRST_EDP0_24M, 62, 1),
	RST_BASE(SRST_P_EDP1, 62, 3),
	RST_BASE(SRST_EDP1_24M, 62, 4),
	RST_BASE(SRST_M_I2S5_8CH_TX, 62, 8),
	RST_BASE(SRST_H_I2S5_8CH, 62, 12),
	RST_BASE(SRST_M_I2S6_8CH_TX, 62, 15),

	/* SOFTRST_CON63 */
	RST_BASE(SRST_M_I2S6_8CH_RX, 63, 2),
	RST_BASE(SRST_H_I2S6_8CH, 63, 3),
	RST_BASE(SRST_H_SPDIF3, 63, 4),
	RST_BASE(SRST_M_SPDIF3, 63, 7),
	RST_BASE(SRST_H_SPDIF4, 63, 8),
	RST_BASE(SRST_M_SPDIF4, 63, 11),
	RST_BASE(SRST_H_SPDIFRX0, 63, 12),
	RST_BASE(SRST_M_SPDIFRX0, 63, 13),
	RST_BASE(SRST_H_SPDIFRX1, 63, 14),
	RST_BASE(SRST_M_SPDIFRX1, 63, 15),

	/* SOFTRST_CON64 */
	RST_BASE(SRST_H_SPDIFRX2, 64, 0),
	RST_BASE(SRST_M_SPDIFRX2, 64, 1),
	RST_BASE(SRST_LINKSYM_HDMITXPHY0, 64, 12),
	RST_BASE(SRST_LINKSYM_HDMITXPHY1, 64, 13),
	RST_BASE(SRST_VO1_BRIDGE0, 64, 14),
	RST_BASE(SRST_VO1_BRIDGE1, 64, 15),

	/* SOFTRST_CON65 */
	RST_BASE(SRST_H_I2S9_8CH, 65, 0),
	RST_BASE(SRST_M_I2S9_8CH_RX, 65, 3),
	RST_BASE(SRST_H_I2S10_8CH, 65, 4),
	RST_BASE(SRST_M_I2S10_8CH_RX, 65, 7),
	RST_BASE(SRST_P_S_HDMIRX, 65, 8),

	/* SOFTRST_CON66 */
	RST_BASE(SRST_GPU, 66, 4),
	RST_BASE(SRST_SYS_GPU, 66, 5),
	RST_BASE(SRST_A_S_GPU_BIU, 66, 8),
	RST_BASE(SRST_A_M0_GPU_BIU, 66, 9),
	RST_BASE(SRST_A_M1_GPU_BIU, 66, 10),
	RST_BASE(SRST_A_M2_GPU_BIU, 66, 11),
	RST_BASE(SRST_A_M3_GPU_BIU, 66, 12),
	RST_BASE(SRST_P_GPU_BIU, 66, 14),
	RST_BASE(SRST_P_GPU_PVTM, 66, 15),

	/* SOFTRST_CON67 */
	RST_BASE(SRST_GPU_PVTM, 67, 0),
	RST_BASE(SRST_P_GPU_GRF, 67, 2),
	RST_BASE(SRST_GPU_PVTPLL, 67, 3),
	RST_BASE(SRST_GPU_JTAG, 67, 4),

	/* SOFTRST_CON68 */
	RST_BASE(SRST_A_AV1_BIU, 68, 1),
	RST_BASE(SRST_A_AV1, 68, 2),
	RST_BASE(SRST_P_AV1_BIU, 68, 4),
	RST_BASE(SRST_P_AV1, 68, 5),

	/* SOFTRST_CON69 */
	RST_BASE(SRST_A_DDR_BIU, 69, 4),
	RST_BASE(SRST_A_DMA2DDR, 69, 5),
	RST_BASE(SRST_A_DDR_SHAREMEM, 69, 6),
	RST_BASE(SRST_A_DDR_SHAREMEM_BIU, 69, 7),
	RST_BASE(SRST_A_CENTER_S200_BIU, 69, 10),
	RST_BASE(SRST_A_CENTER_S400_BIU, 69, 11),
	RST_BASE(SRST_H_AHB2APB, 69, 12),
	RST_BASE(SRST_H_CENTER_BIU, 69, 13),
	RST_BASE(SRST_F_DDR_CM0_CORE, 69, 14),

	/* SOFTRST_CON70 */
	RST_BASE(SRST_DDR_TIMER0, 70, 0),
	RST_BASE(SRST_DDR_TIMER1, 70, 1),
	RST_BASE(SRST_T_WDT_DDR, 70, 2),
	RST_BASE(SRST_T_DDR_CM0_JTAG, 70, 3),
	RST_BASE(SRST_P_CENTER_GRF, 70, 5),
	RST_BASE(SRST_P_AHB2APB, 70, 6),
	RST_BASE(SRST_P_WDT, 70, 7),
	RST_BASE(SRST_P_TIMER, 70, 8),
	RST_BASE(SRST_P_DMA2DDR, 70, 9),
	RST_BASE(SRST_P_SHAREMEM, 70, 10),
	RST_BASE(SRST_P_CENTER_BIU, 70, 11),
	RST_BASE(SRST_P_CENTER_CHANNEL_BIU, 70, 12),

	/* SOFTRST_CON72 */
	RST_BASE(SRST_P_USBDPGRF0, 72, 1),
	RST_BASE(SRST_P_USBDPPHY0, 72, 2),
	RST_BASE(SRST_P_USBDPGRF1, 72, 3),
	RST_BASE(SRST_P_USBDPPHY1, 72, 4),
	RST_BASE(SRST_P_HDPTX0, 72, 5),
	RST_BASE(SRST_P_HDPTX1, 72, 6),
	RST_BASE(SRST_P_APB2ASB_SLV_BOT_RIGHT, 72, 7),
	RST_BASE(SRST_P_USB2PHY_U3_0_GRF0, 72, 8),
	RST_BASE(SRST_P_USB2PHY_U3_1_GRF0, 72, 9),
	RST_BASE(SRST_P_USB2PHY_U2_0_GRF0, 72, 10),
	RST_BASE(SRST_P_USB2PHY_U2_1_GRF0, 72, 11),
	RST_BASE(SRST_HDPTX0_ROPLL, 72, 12),
	RST_BASE(SRST_HDPTX0_LCPLL, 72, 13),
	RST_BASE(SRST_HDPTX0, 72, 14),
	RST_BASE(SRST_HDPTX1_ROPLL, 72, 15),

	/* SOFTRST_CON73 */
	RST_BASE(SRST_HDPTX1_LCPLL, 73, 0),
	RST_BASE(SRST_HDPTX1, 73, 1),
	RST_BASE(SRST_HDPTX0_HDMIRXPHY_SET, 73, 2),
	RST_BASE(SRST_USBDP_COMBO_PHY0, 73, 3),
	RST_BASE(SRST_USBDP_COMBO_PHY0_LCPLL, 73, 4),
	RST_BASE(SRST_USBDP_COMBO_PHY0_ROPLL, 73, 5),
	RST_BASE(SRST_USBDP_COMBO_PHY0_PCS_HS, 73, 6),
	RST_BASE(SRST_USBDP_COMBO_PHY1, 73, 7),
	RST_BASE(SRST_USBDP_COMBO_PHY1_LCPLL, 73, 8),
	RST_BASE(SRST_USBDP_COMBO_PHY1_ROPLL, 73, 9),
	RST_BASE(SRST_USBDP_COMBO_PHY1_PCS_HS, 73, 10),
	RST_BASE(SRST_HDMIHDP0, 73, 12),
	RST_BASE(SRST_HDMIHDP1, 73, 13),

	/* SOFTRST_CON74 */
	RST_BASE(SRST_A_VO1USB_TOP_BIU, 74, 1),
	RST_BASE(SRST_H_VO1USB_TOP_BIU, 74, 3),

	/* SOFTRST_CON75 */
	RST_BASE(SRST_H_SDIO_BIU, 75, 1),
	RST_BASE(SRST_H_SDIO, 75, 2),
	RST_BASE(SRST_SDIO, 75, 3),

	/* SOFTRST_CON76 */
	RST_BASE(SRST_H_RGA3_BIU, 76, 2),
	RST_BASE(SRST_A_RGA3_BIU, 76, 3),
	RST_BASE(SRST_H_RGA3_1, 76, 4),
	RST_BASE(SRST_A_RGA3_1, 76, 5),
	RST_BASE(SRST_RGA3_1_CORE, 76, 6),

	/* SOFTRST_CON77 */
	RST_BASE(SRST_REF_PIPE_PHY0, 77, 6),
	RST_BASE(SRST_REF_PIPE_PHY1, 77, 7),
	RST_BASE(SRST_REF_PIPE_PHY2, 77, 8),

	/* PHPTOPCRU_SOFTRST_CON00 */
	RST__PHP(SRST_P_PHPTOP_CRU, 0, 1),
	RST__PHP(SRST_P_PCIE2_GRF0, 0, 2),
	RST__PHP(SRST_P_PCIE2_GRF1, 0, 3),
	RST__PHP(SRST_P_PCIE2_GRF2, 0, 4),
	RST__PHP(SRST_P_PCIE2_PHY0, 0, 5),
	RST__PHP(SRST_P_PCIE2_PHY1, 0, 6),
	RST__PHP(SRST_P_PCIE2_PHY2, 0, 7),
	RST__PHP(SRST_P_PCIE3_PHY, 0, 8),
	RST__PHP(SRST_P_APB2ASB_SLV_CHIP_TOP, 0, 9),
	RST__PHP(SRST_PCIE30_PHY, 0, 10),

	/* PMU1CRU_SOFTRST_CON00 */
	RST_PMU1(SRST_H_PMU1_BIU, 0, 10),
	RST_PMU1(SRST_P_PMU1_BIU, 0, 11),
	RST_PMU1(SRST_H_PMU_CM0_BIU, 0, 12),
	RST_PMU1(SRST_F_PMU_CM0_CORE, 0, 13),
	RST_PMU1(SRST_T_PMU1_CM0_JTAG, 0, 14),

	/* PMU1CRU_SOFTRST_CON01 */
	RST_PMU1(SRST_DDR_FAIL_SAFE, 1, 1),
	RST_PMU1(SRST_P_CRU_PMU1, 1, 2),
	RST_PMU1(SRST_P_PMU1_GRF, 1, 4),
	RST_PMU1(SRST_P_PMU1_IOC, 1, 5),
	RST_PMU1(SRST_P_PMU1WDT, 1, 6),
	RST_PMU1(SRST_T_PMU1WDT, 1, 7),
	RST_PMU1(SRST_P_PMU1TIMER, 1, 8),
	RST_PMU1(SRST_PMU1TIMER0, 1, 10),
	RST_PMU1(SRST_PMU1TIMER1, 1, 11),
	RST_PMU1(SRST_P_PMU1PWM, 1, 12),
	RST_PMU1(SRST_PMU1PWM, 1, 13),

	/* PMU1CRU_SOFTRST_CON02 */
	RST_PMU1(SRST_P_I2C0, 2, 1),
	RST_PMU1(SRST_I2C0, 2, 2),
	RST_PMU1(SRST_S_UART0, 2, 5),
	RST_PMU1(SRST_P_UART0, 2, 6),
	RST_PMU1(SRST_H_I2S1_8CH, 2, 7),
	RST_PMU1(SRST_M_I2S1_8CH_TX, 2, 10),
	RST_PMU1(SRST_M_I2S1_8CH_RX, 2, 13),
	RST_PMU1(SRST_H_PDM0, 2, 14),
	RST_PMU1(SRST_PDM0, 2, 15),

	/* PMU1CRU_SOFTRST_CON03 */
	RST_PMU1(SRST_H_VAD, 3, 0),
	RST_PMU1(SRST_HDPTX0_INIT, 3, 11),
	RST_PMU1(SRST_HDPTX0_CMN, 3, 12),
	RST_PMU1(SRST_HDPTX0_LANE, 3, 13),
	RST_PMU1(SRST_HDPTX1_INIT, 3, 15),

	/* PMU1CRU_SOFTRST_CON04 */
	RST_PMU1(SRST_HDPTX1_CMN, 4, 0),
	RST_PMU1(SRST_HDPTX1_LANE, 4, 1),
	RST_PMU1(SRST_M_MIPI_DCPHY0, 4, 3),
	RST_PMU1(SRST_S_MIPI_DCPHY0, 4, 4),
	RST_PMU1(SRST_M_MIPI_DCPHY1, 4, 5),
	RST_PMU1(SRST_S_MIPI_DCPHY1, 4, 6),
	RST_PMU1(SRST_OTGPHY_U3_0, 4, 7),
	RST_PMU1(SRST_OTGPHY_U3_1, 4, 8),
	RST_PMU1(SRST_OTGPHY_U2_0, 4, 9),
	RST_PMU1(SRST_OTGPHY_U2_1, 4, 10),

	/* PMU1CRU_SOFTRST_CON05 */
	RST_PMU1(SRST_P_PMU0GRF, 5, 3),
	RST_PMU1(SRST_P_PMU0IOC, 5, 4),
	RST_PMU1(SRST_P_GPIO0, 5, 5),
	RST_PMU1(SRST_GPIO0, 5, 6),

	/* SECURECRU_SOFTRST_CON00 */
	RST_SCRU(SRST_A_SECURE_NS_BIU, 0, 10),
	RST_SCRU(SRST_H_SECURE_NS_BIU, 0, 11),
	RST_SCRU(SRST_A_SECURE_S_BIU, 0, 12),
	RST_SCRU(SRST_H_SECURE_S_BIU, 0, 13),
	RST_SCRU(SRST_P_SECURE_S_BIU, 0, 14),
	RST_SCRU(SRST_CRYPTO_CORE, 0, 15),

	/* SECURECRU_SOFTRST_CON01 */
	RST_SCRU(SRST_CRYPTO_PKA, 1, 0),
	RST_SCRU(SRST_CRYPTO_RNG, 1, 1),
	RST_SCRU(SRST_A_CRYPTO, 1, 2),
	RST_SCRU(SRST_H_CRYPTO, 1, 3),
	RST_SCRU(SRST_KEYLADDER_CORE, 1, 9),
	RST_SCRU(SRST_KEYLADDER_RNG, 1, 10),
	RST_SCRU(SRST_A_KEYLADDER, 1, 11),
	RST_SCRU(SRST_H_KEYLADDER, 1, 12),
	RST_SCRU(SRST_P_OTPC_S, 1, 13),
	RST_SCRU(SRST_OTPC_S, 1, 14),
	RST_SCRU(SRST_WDT_S, 1, 15),

	/* SECURECRU_SOFTRST_CON02 */
	RST_SCRU(SRST_T_WDT_S, 2, 0),
	RST_SCRU(SRST_H_BOOTROM, 2, 1),
	RST_SCRU(SRST_A_DCF, 2, 2),
	RST_SCRU(SRST_P_DCF, 2, 3),
	RST_SCRU(SRST_H_BOOTROM_NS, 2, 5),
	RST_SCRU(SRST_P_KEYLADDER, 2, 14),
	RST_SCRU(SRST_H_TRNG_S, 2, 15),

	/* SECURECRU_SOFTRST_CON03 */
	RST_SCRU(SRST_H_TRNG_NS, 3, 0),
	RST_SCRU(SRST_D_SDMMC_BUFFER, 3, 1),
	RST_SCRU(SRST_H_SDMMC, 3, 2),
	RST_SCRU(SRST_H_SDMMC_BUFFER, 3, 3),
	RST_SCRU(SRST_SDMMC, 3, 4),
	RST_SCRU(SRST_P_TRNG_CHK, 3, 5),
	RST_SCRU(SRST_TRNG_S, 3, 6),
};



static int
rk3588_cru_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-cru")) {
		device_set_desc(dev, "Rockchip RK3588 Clock & Reset Unit");
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

static int
rk3588_cru_attach(device_t dev)
{
	struct rk_cru_softc *sc;

	sc = device_get_softc(dev);
	sc->dev	= dev;
	sc->clks = rk3588_clks;
	sc->nclks = nitems(rk3588_clks);
	sc->gates = rk3588_gates;
	sc->ngates = nitems(rk3588_gates);
	sc->reset_table = rk3588_resets;
	sc->reset_num =	nitems(rk3588_resets);
	return (rk_cru_attach(dev));
}

static device_method_t methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk3588_cru_probe),
	DEVMETHOD(device_attach,	rk3588_cru_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(rk3588_cru, rk3588_cru_driver, methods,
    sizeof(struct rk_cru_softc), rk_cru_driver);

EARLY_DRIVER_MODULE(rk3588_cru,	simplebus, rk3588_cru_driver,
    0, 0, BUS_PASS_INTERRUPT + BUS_PASS_ORDER_EARLY);
