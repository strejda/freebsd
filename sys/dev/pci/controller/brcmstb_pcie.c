/*
 * Copyright (c) 2020, 2025 Mark Kettenis <kettenis@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

 /*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/intr.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>
#include <dev/phy/phy.h>
#include <dev/regulator/regulator.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/ofw/ofwpci.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcib_private.h>

#include <machine/resource.h>
#include <machine/bus.h>

#include "msi_if.h"
#include "pcib_if.h"
#include "pic_if.h"

#if 0
#define	dprintf(fmt, args...) do { printf(fmt,##args); } while (0)
#else
#define	dprintf(fmt, args...)
#endif

#define	PCI_BUS_SHIFT		20
#define	PCI_SLOT_SHIFT		15
#define	PCI_FUNC_SHIFT		12
#define	PCI_REG_SHIFT		0
#define	PCI_BUS_MASK		0xFF
#define	PCI_SLOT_MASK		0x1F
#define	PCI_FUNC_MASK		0x07
#define	PCI_REG_MASK		0xFFF

#define	ECAM_CFG_BUS(bus)	((uint32_t)((bus)  & PCI_BUS_MASK) << PCI_BUS_SHIFT)
#define	ECAM_CFG_SLOT(slot)	((uint32_t)((slot) & PCI_SLOT_MASK) << PCI_SLOT_SHIFT)
#define	ECAM_CFG_FUNC(func)	((uint32_t)((func) & PCI_FUNC_MASK) << PCI_FUNC_SHIFT)
#define	ECAM_CFG_REG(reg)	((uint32_t)((reg)  & PCI_REG_MASK) <<  PCI_REG_SHIFT)

#define BRCM_INBOUND_WINS		16
#define BRCM_OUTBOUND_WINS		4
#define BRCM_MSI_INTS			32
#define BRCM_MSI_LEGACY_INTS		4

#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1		0x0188

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043C
#define  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_MASK			(0xffffff << 0)

#define PCIE_RC_CFG_PRIV1_LINK_CAP			0x04DC
#define  PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_MASK		(0x1F << 4)
#define  PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_SHIFT	 4
#define  PCIE_RC_CFG_PRIV1_LINK_CAP_ASPM_SUPPORT_MASK		(0x3 << 10)

#define PCIE_RC_CFG_PRIV1_ROOT_CAP			0x04F8
#define  PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK		(0x1F << 3)
#define  PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_SHIFT		3

#define PCIE_RC_TL_VDM_CTL1				0x0A0C

#define PCIE_RC_TL_VDM_CTL0				0x0A20
#define  PCIE_RC_TL_VDM_CTL0_VDM_ENABLED			(1 << 16)
#define  PCIE_RC_TL_VDM_CTL0_VDM_IGNORETAG			(1 << 17)
#define  PCIE_RC_TL_VDM_CTL0_VDM_IGNOREVNDRID			(1 << 18)

#define PCIE_RC_DL_MDIO_ADDR				0x1100
#define  PCIE_RC_DL_MDIO_PORT_MASK				(0xF << 16)
#define  PCIE_RC_DL_MDIO_PORT_SHIFT				16
#define  PCIE_RC_DL_MDIO_REGAD_MASK				(0xFFFF << 0)
#define  PCIE_RC_DL_MDIO_REGAD_SHIFT				0
#define  PCIE_RC_DL_MDIO_CMD_READ				(1 << 20)
#define  PCIE_RC_DL_MDIO_CMD_WRITE				(0 << 20)

#define PCIE_RC_DL_MDIO_WR_DATA				0x1104
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108
#define  PCIE_RC_DL_MDIO_DATA_DONE				(1U << 31)
#define  PCIE_RC_DL_MDIO_DATA_MASK				(0x7FFFFFFF << 0)

#define PCIE_RC_PL_REG_PHY_CTL_1			0x1804
#define  PCIE_RC_PL_REG_P2_POWERDOWN_ENA_NOSYNC_MASK		0x8

#define PCIE_RC_PL_PHY_CTL_15				0x184C
#define  PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK		(0xFF << 0)
#define  PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_SHIFT		0

#define PCIE_MISC_MISC_CTRL				0x4008
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE			(1 << 7)
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE			(1 << 10)
#define  PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN			(1 << 12)
#define  PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE			(1 << 13)
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK		(0x3 << 20)
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_128			(0x0 << 20)
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_256			(0x1 << 20)
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_512			(0x2 << 20)
#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK			(0x1FU << 27)
#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_SHIFT			27

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400C
#define PCIE_MEM_WIN0_LO(i)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + ((i) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MEM_WIN0_HI(i)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + ((i) * 8)

#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402C
#define  PCIE_MISC_RC_BAR1_CONFIG_SIZE_MASK			(0x1F << 0)
#define PCIE_MISC_RC_BAR1_CONFIG_HI			0x4030

#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044
#define  PCIE_MISC_MSI_BAR_CONFIG_LO_EN				(1 << 0)
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048

#define PCIE_MISC_MSI_DATA_CONFIG			0x404C
#define  PCIE_MISC_MSI_DATA_CONFIG_8				0xFFE06540
#define  PCIE_MISC_MSI_DATA_CONFIG_32				0xFFF86540

#define PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT		0x405C
#define PCIE_MISC_PCIE_CTRL				0x4064
#define  PCIE_MISC_PCIE_CTRL_PCIE_PERSTB			(1 << 2)

#define PCIE_MISC_PCIE_STATUS				0x4068
#define  PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP			(1 << 4)
#define  PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE			(1 << 5)

#define PCIE_MISC_REVISION				0x406c
#define  BRCM_PCIE_HW_REV_33				0x0303
#define  BRCM_PCIE_HW_REV_3_20				0x0320

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT	0x4070
#define  PCIE_MEM_WIN0_BASE_LIMIT(i)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((i) * 4)
#define  PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK			0xfff00000
#define  PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_SHIFT			20
#define  PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK			0xfff0
#define  PCIE_MEM_WIN0_BASE_LIMIT_BASE_SHIFT			4
#define  PCIE_MEM_WIN0_BASE_LIMIT(i)					\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((i) * 4)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI		0x4080
#define  PCIE_MEM_WIN0_BASE_HI(i)					\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + ((i) * 8)
#define PCIE_MEM_WIN0_BASE_HI_BASE_MASK				0xFF
#define PCIE_MEM_WIN0_BASE_HI_BASE_SHIFT			0

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI		0x4084
#define  PCIE_MEM_WIN0_LIMIT_HI(i)					\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + ((i) * 8)
#define  PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK			0xFF
#define  PCIE_MEM_WIN0_LIMIT_HI_LIMIT_SHIFT			0

#define PCIE_MISC_CTRL_1				0x40A0
#define  PCIE_MISC_CTRL_1_EN_VDM_QOS_CONTROL			(1 << 5)

#define PCIE_MISC_UBUS_CTRL				0x40A4
#define  PCIE_MISC_UBUS_CTRL_UBUS_PCIE_REPLY_ERR_DIS    	(1 << 13)
#define  PCIE_MISC_UBUS_CTRL_UBUS_PCIE_REPLY_DECERR_DIS 	(1 << 19)

#define PCIE_MISC_UBUS_TIMEOUT				0x40A8

#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_LO		0x40AC
#define  PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_EN			(1 << 0)
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI		0x40B0
#define PCIE_MISC_RC_BAR4_CONFIG_LO			0x40D4
#define PCIE_MISC_RC_BAR4_CONFIG_HI			0x40D8
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP_LO		0x410C
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP_HI		0x4110

#define PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_HI		0x4164
#define PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_LO		0x4168
#define PCIE_MISC_AXI_INTF_CTRL				0x416C
#define  PCIE_MISC_AXI_REQFIFO_EN_QOS_PROPAGATION		(1 << 17)
#define  PCIE_MISC_AXI_EN_RCLK_QOS_ARRAY_FIX			(1 << 13)
#define  PCIE_MISC_AXI_EN_QOS_UPDATE_TIMING_FIX			(1 << 12)
#define  PCIE_MISC_AXI_DIS_QOS_GATING_IN_MASTER			(1 << 11)
#define  PCIE_MISC_AXI_MASTER_MAX_OUTSTANDING_REQUESTS_MASK 	(0x3F << 0)
#define  PCIE_MISC_AXI_MASTER_MAX_OUTSTANDING_REQUESTS_SHIFT 	0

#define PCIE_MISC_AXI_READ_ERROR_DATA			0x4170
#define PCIE_HARD_DEBUG_2711				0x4204
#define PCIE_HARD_DEBUG_2712				0x4304
#define  PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE			(1 << 1)
#define  PCIE_HARD_DEBUG_REFCLK_OVRD_ENABLE			(1 << 16)
#define  PCIE_HARD_DEBUG_REFCLK_OVRD_OUT			(1 << 20)
#define  PCIE_HARD_DEBUG_L1SS_ENABLE				(1 << 21)
#define  PCIE_HARD_DEBUG_SERDES_IDDQ				(1 << 27)
#define  PCIE_HARD_DEBUG_CLKREQ_MASK 					\
	    (PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE |			\
	     PCIE_HARD_DEBUG_REFCLK_OVRD_ENABLE	|			\
	     PCIE_HARD_DEBUG_REFCLK_OVRD_OUT |				\
	     PCIE_HARD_DEBUG_L1SS_ENABLE)

#define PCIE_MSI_INTR2_INT_STATUS			0x4500
#define PCIE_MSI_INTR2_INT_CLR				0x4508
#define PCIE_MSI_INTR2_INT_MASK_SET			0x4510
#define PCIE_MSI_INTR2_INT_MASK_CLR			0x4514
#define PCIE_EXT_CFG_DATA				0x8000
#define PCIE_EXT_CFG_INDEX				0x9000
#define PCIE_RGR1_SW_INIT_1				0x9210
#define  PCIE_RGR1_SW_INIT_1_PERST				(1 << 0)
#define  PCIE_RGR1_SW_INIT_1_INIT				(1 << 1)

#define MDIO_SET_ADDR			0x1F
#define  MDIO_SSC_REGS_ADDR		0x1100

#define MDIO_SSC_STATUS			0x01
#define  MDIO_SSC_STATUS_SSC		(1 << 10)
#define  MDIO_SSC_STATUS_PLL_LOCK	(1 << 11)
#define MDIO_SSC_CNTL			0x02
#define  MDIO_SSC_CNTL_OVRD_EN		(1 << 15)
#define  MDIO_SSC_CNTL_OVRD_VAL		(1 << 14)

#define RD4(sc, reg)		bus_read_4((sc)->mem_res, (reg))
#define RD2(sc, reg)		bus_read_2((sc)->mem_res, (reg))
#define RD1(sc, reg)		bus_read_1((sc)->mem_res, (reg))
#define WR4(sc, reg, val)	bus_write_4((sc)->mem_res, (reg), (val))
#define WR2(sc, reg, val)	bus_write_2((sc)->mem_res, (reg), (val))
#define WR1(sc, reg, val)	bus_write_1((sc)->mem_res, (reg), (val))

enum soc_type {
	BCM2711,
	BCM2712,
};

struct brcm_pcie_soc {
	enum soc_type	type;
	uint32_t	hard_debug_reg;
	int		inbound_wins;
};

static struct brcm_pcie_soc bcm2711_soc = {
	.type = BCM2711,
	.hard_debug_reg = PCIE_HARD_DEBUG_2711,
	.inbound_wins = 3,
};

static struct brcm_pcie_soc bcm2712_soc = {
	.type = BCM2712,
	.hard_debug_reg = PCIE_HARD_DEBUG_2712,
	.inbound_wins = 10,
};

/* Compatible devices. */
static struct ofw_compat_data compat_data[] = {
	{"brcm,bcm2711-pcie", (uintptr_t)&bcm2711_soc},
	{"brcm,bcm2712-pcie", (uintptr_t)&bcm2712_soc},
	{NULL,		 	 0},
};

struct brcm_pcie_irqsrc {
	struct intr_irqsrc	isrc;
	u_int			irq;
	bool			used;
	bool			legacy;
};

struct brcm_pcie_softc {
	struct ofw_pci_softc	ofw_pci;
	device_t		dev;
	phandle_t		node;
	struct brcm_pcie_soc	*soc;
	struct mtx		mtx;
	bus_dma_tag_t		dmat;

	struct resource		*mem_res;
	struct resource 	*pcie_irq_res;
	struct resource 	*msi_irq_res;
	void			*pcie_intr_cookie;
	void			*msi_intr_cookie;

	clk_t			clk_sw_pcie;

	hwreset_t		hwreset_perst;
	hwreset_t		hwreset_rescal;
	hwreset_t		hwreset_bridge;
	hwreset_t		hwreset_swinit;

	bool			coherent;
	bool			no_l0s;
	bool			enable_ssc;
	char			clkreq_mode[32];

	int 			num_lanes;

	u_int 			bus_start;
	u_int 			bus_end;
	u_int 			root_bus;
	u_int 			sec_bus;

	struct ofw_pci_range	*mem_range;
	int			mem_ranges;

	uint64_t		*memc_size;
	int			memc_sizes;

	bus_addr_t		pcie_hard_debug;
	int			vl805_fwload;
	uint32_t		hw_rev;
};


static void
brcm_pcie_perst(struct brcm_pcie_softc *sc, bool assert)
{
	uint32_t reg;

	if (sc->soc->type == BCM2711) {
		reg = RD4(sc, PCIE_RGR1_SW_INIT_1);
		if (assert)
			reg |= PCIE_RGR1_SW_INIT_1_PERST;
		else
			reg &= ~PCIE_RGR1_SW_INIT_1_PERST;
		WR4(sc, PCIE_RGR1_SW_INIT_1, reg);
	} else {
		reg = RD4(sc, PCIE_MISC_PCIE_CTRL);
		if (assert)
			reg &= ~PCIE_MISC_PCIE_CTRL_PCIE_PERSTB;
		else
			reg |= PCIE_MISC_PCIE_CTRL_PCIE_PERSTB;
		WR4(sc, PCIE_MISC_PCIE_CTRL, reg);
	}
}

static int
brcm_pcie_reset_bridge(struct brcm_pcie_softc *sc, bool assert)
{
	int rv;
	uint32_t reg;

	if (sc->hwreset_bridge != NULL) {
		if (assert)
			rv = hwreset_assert(sc->hwreset_bridge);
		else
			rv = hwreset_deassert(sc->hwreset_bridge);
		if (rv != 0)
			device_printf(sc->dev, "Cannot %s 'bridge' reset: %d\n",
			    assert ? "assert": "deassert", rv);

		return(rv);
	}

	reg = RD4(sc, PCIE_RGR1_SW_INIT_1);
	if (assert)
		reg |=  PCIE_RGR1_SW_INIT_1_INIT;
	else
		reg &= PCIE_RGR1_SW_INIT_1_INIT;
	WR4(sc, PCIE_RGR1_SW_INIT_1, reg);

	sc->vl805_fwload = 1;
	return (0);
}

static int
brcm_pcie_mdio_write(struct brcm_pcie_softc *sc, uint8_t port, uint16_t addr,
     uint32_t data)
{
	uint32_t reg;
	int i;

	MPASS(port < 16);

	reg = PCIE_RC_DL_MDIO_CMD_WRITE;
	reg |= ((uint32_t)port << PCIE_RC_DL_MDIO_PORT_SHIFT);
	reg |= ((uint32_t)addr << PCIE_RC_DL_MDIO_REGAD_SHIFT);

	WR4(sc, PCIE_RC_DL_MDIO_ADDR, reg);
	RD4(sc, PCIE_RC_DL_MDIO_ADDR);

	WR4(sc, PCIE_RC_DL_MDIO_WR_DATA, data | PCIE_RC_DL_MDIO_DATA_DONE);
	for (i = 100; i > 0; i--) {
		reg = RD4(sc, PCIE_RC_DL_MDIO_WR_DATA);
		if ((reg & PCIE_RC_DL_MDIO_DATA_DONE) == 0)
			break;
		DELAY(10);
	}
	if (i == 0) {
		device_printf(sc->dev, "%s: timeouted\n", __func__);
		return (ETIMEDOUT);
	}

	return (0);
}

static int
brcm_pcie_mdio_read(struct brcm_pcie_softc *sc, uint8_t port, uint16_t addr,
     uint32_t *data)
{
	uint32_t reg;
	int i;

	MPASS(port < 16);

	reg = PCIE_RC_DL_MDIO_CMD_READ;
	reg |= ((uint32_t)port << PCIE_RC_DL_MDIO_PORT_SHIFT);
	reg |= ((uint32_t)addr << PCIE_RC_DL_MDIO_REGAD_SHIFT);
	WR4(sc, PCIE_RC_DL_MDIO_ADDR, reg);
	RD4(sc, PCIE_RC_DL_MDIO_ADDR);

	for (i = 100; i > 0; i--) {
		reg = RD4(sc, PCIE_RC_DL_MDIO_RD_DATA);
		if (reg & PCIE_RC_DL_MDIO_DATA_DONE)
			break;
		DELAY(10);
	}
	if (i == 0) {
		device_printf(sc->dev, "%s: timeouted\n", __func__);
		return (ETIMEDOUT);
	}

	*data = reg & PCIE_RC_DL_MDIO_DATA_MASK;
	return 0;
}

static int
brcm_pcie_setup_ssc(struct brcm_pcie_softc *sc)
{
	uint32_t reg;
	int rv;

	rv = brcm_pcie_mdio_write(sc, 0, MDIO_SET_ADDR, MDIO_SSC_REGS_ADDR);
	if (rv) return (rv);

	rv = brcm_pcie_mdio_read(sc, 0, MDIO_SSC_CNTL, &reg);
	if (rv) return (rv);
	reg |= MDIO_SSC_CNTL_OVRD_VAL | MDIO_SSC_CNTL_OVRD_EN;
	rv = brcm_pcie_mdio_write(sc, 0, MDIO_SSC_CNTL, reg);
	if (rv) return (rv);

	DELAY(1000);

	rv = brcm_pcie_mdio_read(sc, 0, MDIO_SSC_STATUS, &reg);
	if (rv)
		return (rv);

	if ((reg & MDIO_SSC_STATUS_SSC) == 0 ||
	    (reg & MDIO_SSC_STATUS_PLL_LOCK) == 0)
		return (EIO);

	return (0);
}

static bool
brcm_pcie_get_link(struct brcm_pcie_softc *sc)
{
	uint32_t reg;
	bool  dl, pl;

	reg = RD4(sc, PCIE_MISC_PCIE_STATUS);
	dl = !!(reg & PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE);
	pl = !!(reg & PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP);
	return (dl  && pl);
}

/*-----------------------------------------------------------------------------
 *
 *  PCIB  INTERFACE
 */

static int
brcm_pcie_maxslots(device_t dev)
{
	return (32);
}

static bool
brcm_pcie_check_dev(struct brcm_pcie_softc *sc, u_int bus, u_int slot,
    u_int func, u_int reg)
{

	if (bus < sc->bus_start || bus > sc->bus_end || slot > PCI_SLOTMAX ||
	    func > PCI_FUNCMAX || reg > PCIE_REGMAX)
		return (false);

	/* link is needed for access to all non-root busses */
	if (bus != sc->root_bus) {
		if (brcm_pcie_get_link(sc))
			return (true);
		return (false);
	}

	/* we have only 1 device with 1 function on root port */
	if (slot > 0 || func > 0)
		return (false);
	return (true);
}

static uint32_t
brcm_pcie_read_config(device_t dev, u_int bus, u_int slot,
    u_int func, u_int reg, int bytes)
{
	struct brcm_pcie_softc *sc;

	sc = device_get_softc(dev);

	if (!brcm_pcie_check_dev(sc, bus, slot, func, reg))
		return (0xFFFFFFFFU);

	if (bus == sc->root_bus) {
		switch (bytes) {
		case 1:
//			dprintf("%s: read1(%d, %d, %d, %d): 0x%02X\n", __func__,
//			    bus, slot, func, reg, RD1(sc, reg));
			return (RD1(sc, reg));
		case 2:
//			dprintf("%s: read2(%d, %d, %d, %d): 0x%04X\n", __func__,
//			    bus, slot, func, reg, RD2(sc, reg));
			return (RD2(sc, reg));
		case 4:
//			dprintf("%s: read4(%d, %d, %d, %d): 0x%08X\n", __func__,
//			    bus, slot, func, reg, RD4(sc, reg));
			return (RD4(sc, reg));
		default:
			device_printf(sc->dev, "Unsupported width: %d\n",
			     bytes);
			return (0xFFFFFFFF);
		}
	}

	WR4(sc, PCIE_EXT_CFG_INDEX,
	    ECAM_CFG_BUS(bus) | ECAM_CFG_SLOT(slot) | ECAM_CFG_FUNC(func));
	switch (bytes) {
	case 1:
//		dprintf("%s: read1(%d, %d, %d, %d): 0x%02X\n", __func__,
//		    bus, slot, func, reg, RD1(sc, PCIE_EXT_CFG_DATA + reg));
		return (RD1(sc, PCIE_EXT_CFG_DATA + reg));
	case 2:
//		dprintf("%s: read2(%d, %d, %d, %d): 0x%04X\n", __func__,
//		    bus, slot, func, reg, RD2(sc, PCIE_EXT_CFG_DATA + reg));
		return (RD2(sc, PCIE_EXT_CFG_DATA + reg));
	case 4:
//		dprintf("%s: read4(%d, %d, %d, %d): 0x%08X\n", __func__,
//		    bus, slot, func, reg, RD4(sc, PCIE_EXT_CFG_DATA + reg));
		return (RD4(sc, PCIE_EXT_CFG_DATA + reg));
	default:
		device_printf(sc->dev, "Unsupported width: %d\n", bytes);
		return (0xFFFFFFFF);
	}
}

static void
brcm_pcie_write_config(device_t dev, u_int bus, u_int slot,
    u_int func, u_int reg, uint32_t val, int bytes)
{
	struct brcm_pcie_softc *sc;

	sc = device_get_softc(dev);
	if (!brcm_pcie_check_dev(sc, bus, slot, func, reg))
		return;

	if (bus == sc->root_bus) {
		switch (bytes) {
		case 1:
			WR1(sc, reg, val);
			break;
		case 2:
			WR2(sc, reg, val);
			break;
		case 4:
			WR4(sc, reg, val);
			break;
		default:
			device_printf(sc->dev, "Unsupported width: %d\n", bytes);
		}
		return;
	}
	WR4(sc, PCIE_EXT_CFG_INDEX,
	    ECAM_CFG_BUS(bus) | ECAM_CFG_SLOT(slot) | ECAM_CFG_FUNC(func));
	switch (bytes) {
	case 1:
		WR1(sc, PCIE_EXT_CFG_DATA + reg, val);
		break;
	case 2:
		WR2(sc, PCIE_EXT_CFG_DATA + reg, val);
		break;
	case 4:
		WR4(sc, PCIE_EXT_CFG_DATA + reg, val);
		break;
	default:
		device_printf(sc->dev, "Unsupported width: %d\n", bytes);
		break;
	}
}


static void
brcm_pcie_setup_outbound(struct brcm_pcie_softc *sc)
{
	int i;
	uint32_t reg;
	uint64_t host_start, host_end;

	for (i = 0; i < sc->mem_ranges; i++) {
		host_start = sc->mem_range[i].host;
		host_end = host_start  +  sc->mem_range[i].size - 1;
//printf("%s: win: %d,  host: 0x%016lX, size: 0x%016lX,\n", __func__, i, sc->mem_range[i].host, sc->mem_range[i].size);
//printf("%s: win: %d, start: 0x%016lX,  end: 0x%016lX,\n", __func__, i, host_start, host_end);

		host_start /= 1024 * 1024;
		host_end /= 1024 * 1024;
//printf("%s: win: %d, start: 0x%016lX,  end: 0x%016lX,\n", __func__, i, host_start, host_end);

		WR4(sc, PCIE_MEM_WIN0_LO(i),
		    (uint32_t)(sc->mem_range[i].pci >>  0));
		WR4(sc, PCIE_MEM_WIN0_HI(i),
		    (uint32_t)(sc->mem_range[i].pci >> 32));

		reg = RD4(sc, PCIE_MEM_WIN0_BASE_LIMIT(i));
		reg &= ~PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK;
		reg |= (host_start <<  PCIE_MEM_WIN0_BASE_LIMIT_BASE_SHIFT) &
		    PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK;
		reg |= (host_end << PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_SHIFT) &
		     PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK;
		WR4(sc, PCIE_MEM_WIN0_BASE_LIMIT(i), reg);

		host_start >>= 12;
		host_end >>= 12;

		reg = RD4(sc, PCIE_MEM_WIN0_BASE_HI(i));
		reg &= ~PCIE_MEM_WIN0_BASE_HI_BASE_MASK;
		reg |= (host_start << PCIE_MEM_WIN0_BASE_HI_BASE_SHIFT) &
		    PCIE_MEM_WIN0_BASE_HI_BASE_MASK;
		WR4(sc, PCIE_MEM_WIN0_BASE_HI(i), reg);

		reg = RD4(sc, PCIE_MEM_WIN0_LIMIT_HI(i));
		reg &= ~PCIE_MEM_WIN0_BASE_HI_BASE_MASK;
		reg |= (host_end << PCIE_MEM_WIN0_LIMIT_HI_LIMIT_SHIFT) &
		    PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK;
		WR4(sc, PCIE_MEM_WIN0_LIMIT_HI(i), reg);
//printf("%s: regs %d:\n", __func__, i);
//printf("%s:  PCIE_MEM_WIN0_LO: 0x%X, 0x%08X\n", __func__, PCIE_MEM_WIN0_LO(i), RD4(sc, PCIE_MEM_WIN0_LO(i)));
//printf("%s:  PCIE_MEM_WIN0_HI: 0x%X, 0x%08X\n", __func__, PCIE_MEM_WIN0_HI(i), RD4(sc, PCIE_MEM_WIN0_HI(i)));
//printf("%s:  PCIE_MEM_WIN0_BASE_LIMIT: 0x%X, 0x%08X\n", __func__, PCIE_MEM_WIN0_BASE_LIMIT(i), RD4(sc, PCIE_MEM_WIN0_BASE_LIMIT(i)));
//printf("%s:  PCIE_MEM_WIN0_BASE_HI: 0x%X, 0x%08X\n", __func__, PCIE_MEM_WIN0_BASE_HI(i), RD4(sc, PCIE_MEM_WIN0_BASE_HI(i)));
//printf("%s:  PCIE_MEM_WIN0_LIMIT_HI: 0x%X, 0x%08X\n", __func__, PCIE_MEM_WIN0_LIMIT_HI(i), RD4(sc, PCIE_MEM_WIN0_LIMIT_HI(i)));

	}
}

inline static uint32_t
brcm_pcie_ubus_reg(int n, bool hi)
{
	uint32_t reg;

	if (n < 3)
		reg = PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_LO + 8 * n;
	else
		reg = PCIE_MISC_UBUS_BAR4_CONFIG_REMAP_LO + 8 * (n - 3);
	if (hi)
		reg += 4;

	return (reg);
}

inline static uint32_t
brcm_pcie_bar_reg(int n, bool hi)
{
	uint32_t reg;

	if (n < 3)
		reg =  PCIE_MISC_RC_BAR1_CONFIG_LO + 8 * n;
	else
		reg =  PCIE_MISC_RC_BAR4_CONFIG_LO + 8 * (n - 3);
	if (hi)
		reg += 4;

	return (reg);
}

/* Encode bar size for PCIE_X_MISC_RC_BAR */
inline static uint32_t
brcm_pcie_encode_size(uint64_t  size)
{
	int n;

	if (size == 0)
		return (0);

	n  = ilog2(size);
	if (n >= 12 && n <= 15)
		return (n - 12) + 0x1c;

	if (n >= 16 && n <= 36)
		return n - 15;

	panic("Invalid bar size");
}


static int
brcm_pcie_setup_inbound(struct brcm_pcie_softc *sc)
{
	struct ofw_pci_range ranges[BRCM_INBOUND_WINS];
	struct ofw_pci_range *rangep;
	uint64_t min_pcie, tot_size, pci, size;
	uint32_t reg;
	int wins, i;

	bzero(ranges, sizeof(ranges));
	rangep = ranges;
	min_pcie = ~(uint64_t)0;
	tot_size = 0;



	for (i = 0; i < sc->ofw_pci.sc_dma_nrange; i++) {
		if (min_pcie > sc->ofw_pci.sc_dma_range[i].pci)
			min_pcie = sc->ofw_pci.sc_dma_range[i].pci;
		tot_size += sc->ofw_pci.sc_dma_range[i].size;

		if (sc->soc->type == BCM2712) {
			*rangep = sc->ofw_pci.sc_dma_range[i];
//printf("%s: dmarange: %d, pci: 0x%lX, host: 0x%lX, size: 0x%lX\n", __func__, i,  sc->ofw_pci.sc_dmarange[i].pci, sc->ofw_pci.sc_dmarange[i].host, sc->ofw_pci.sc_dmarange[i].size);
//printf("%s: range: %d, pci: 0x%lX, host: 0x%lX, size: 0x%lX\n", __func__, i,  rangep->pci, rangep->host, rangep->size);

			rangep++;
		}

		if (rangep >= ranges + sc->soc->inbound_wins)
			break;
	}
	if (min_pcie == ~(uint64_t)0) {
		device_printf(sc->dev, "Missing 'dma-ranges' property\n");
		return (ENXIO);
	}

	if (sc->soc->type != BCM2712) {

		if (sc->memc_size > 0) {
			/* BCM2711 -> BAR2 host is harcoded to 0 */
			for (i = 0, size = 0; i < sc->memc_sizes; i++)
				size += sc->memc_size[i];
		} else {
			size = 1ULL << flsll(tot_size - 1);
		}

		size = 1ULL << flsll(size - 1);
		pci = min_pcie;

		if ((size == 0) || ((pci & (size - 1)) != 0) ||
		    (pci < (4ULL * 1024 * 1024 *1024) &&
		     pci > (2ULL * 1024 *1024 *1024))) {
			device_printf(sc->dev,
			    "Invalid inbound win2 parameters: size 0x%jX, "
			    "pcie 0x%jX\n",
				(uintmax_t)size, (uintmax_t)pci);
			return (ENXIO);
		}

		/* Clear window 1 */
		rangep++;
		rangep = ranges;

		/* Setup window 2*/
		rangep->pci = pci;
		rangep->host = 0;
		rangep->size = size;
		rangep++;

		/* Clear window 3 */
		rangep++;
	}

	wins = rangep - ranges;
	for (i = 0; i < wins; i++) {
//printf("%s: range: %d, pci: 0x%lX, host: 0x%lX, size: 0x%lX\n", __func__, i,  ranges[i].pci, ranges[i].host, ranges[i].size);

		reg = (uint32_t)ranges[i].pci & ~0x1f;
		reg |= brcm_pcie_encode_size(ranges[i].size);
		WR4(sc, brcm_pcie_bar_reg(i, false), reg);
		reg = (uint32_t)(ranges[i].pci >> 32);
		WR4(sc, brcm_pcie_bar_reg(i, true), reg);
//printf("%s: win: %d, RC_BAR1_CONFIG_LO(0x%X): 0x%08X\n", __func__, i,  brcm_pcie_bar_reg(i, false), RD4(sc, brcm_pcie_bar_reg(i, false)));
//printf("%s: win: %d, RC_BAR1_CONFIG_HI(0x%X): 0x%08X\n", __func__, i,  brcm_pcie_bar_reg(i, true), RD4(sc, brcm_pcie_bar_reg(i, true)));
		if (sc->soc->type == BCM2712) {
			reg = (uint32_t)ranges[i].host & ~0xfff;
			reg |= PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_EN;
			WR4(sc, brcm_pcie_ubus_reg(i, false), reg);
			reg = (uint32_t)(ranges[i].host >> 32);
			WR4(sc, brcm_pcie_ubus_reg(i, true), reg);
//printf("%s: win: %d, UBUS_CONFIG_REMAP_LO(0x%X): 0x%08X\n", __func__, i,  brcm_pcie_ubus_reg(i, false), RD4(sc, brcm_pcie_ubus_reg(i, false)));
//printf("%s: win: %d, UBUS_CONFIG_REMAP_HI(0x%X): 0x%08X\n", __func__, i,  brcm_pcie_ubus_reg(i, true), RD4(sc, brcm_pcie_ubus_reg(i, true)));
		}
	}

	return (0);
}


static int
brcm_pcie_alloc_msi(device_t pci, device_t child, int count,
    int maxcount, int *irqs)
{
	phandle_t msi_parent;
	int rv;

	dprintf("%s: count: %d, maxcount: %d\n", __func__, count,  maxcount);
	rv = ofw_bus_msimap(ofw_bus_get_node(pci), pci_get_rid(child),
	    &msi_parent, NULL);
	if (rv != 0)
		return (rv);

	rv = intr_alloc_msi(pci, child, msi_parent, count, maxcount,
	    irqs);
	return (rv);
}

static int
brcm_pcie_release_msi(device_t pci, device_t child, int count, int *irqs)
{
	phandle_t msi_parent;
	int rv;

	rv = ofw_bus_msimap(ofw_bus_get_node(pci), pci_get_rid(child),
	    &msi_parent, NULL);
	if (rv != 0)
		return (rv);
	return (intr_release_msi(pci, child, msi_parent, count, irqs));
}


static int
brcm_pcie_alloc_msix(device_t pci, device_t child, int *irq)
{
	phandle_t msi_parent;
	int rv;

	rv = ofw_bus_msimap(ofw_bus_get_node(pci), pci_get_rid(child),
	    &msi_parent, NULL);
	if (rv != 0)
		return (rv);

	return (intr_alloc_msix(pci, child, msi_parent, irq));
}

static int
brcm_pcie_release_msix(device_t pci, device_t child, int irq)
{
	phandle_t msi_parent;
	int rv;

	rv = ofw_bus_msimap(ofw_bus_get_node(pci), pci_get_rid(child),
	    &msi_parent, NULL);
	if (rv != 0)
		return (rv);
	return (intr_release_msix(pci, child, msi_parent, irq));
}

static int
brcm_pcie_map_msi(device_t pci, device_t child, int irq, uint64_t *addr,
    uint32_t *data)
{
	phandle_t msi_parent;
	int rv;

	rv = ofw_bus_msimap(ofw_bus_get_node(pci), pci_get_rid(child),
	    &msi_parent, NULL);

	if (rv != 0)
		return (rv);

	return (intr_map_msi(pci, child, msi_parent, irq, addr, data));

}


static int
brcm_pcie_get_id(device_t pci, device_t child, enum pci_id_type type,
    uintptr_t *id)
{
	phandle_t node;
	int rv;
	uint32_t rid;
	uint16_t pci_rid;

	if (type != PCI_ID_MSI)
		return (pcib_get_id(pci, child, type, id));

	node = ofw_bus_get_node(pci);
	pci_rid = pci_get_rid(child);

	rv = ofw_bus_msimap(node, pci_rid, NULL, &rid);
	if (rv != 0)
		return (rv);
	*id = rid;

	return (0);
}

/*-----------------------------------------------------------------------------
 *
 *  DEVICE  INTERFACE
 */
static int
brcm_pcie_intr_pcie(void *arg)
{
	struct brcm_pcie_softc *sc;

	sc = arg;

	device_printf(sc->dev, "%s: enter\n", __func__);
panic("%s", __func__);
	return (FILTER_HANDLED);
}

static int
brcm_pcie_intr_msi(void *arg)
{
	struct brcm_pcie_softc *sc;

	sc = arg;

	device_printf(sc->dev, "%s: enter\n", __func__);
panic("%s", __func__);
	return (FILTER_HANDLED);
}

static int
brcm_pcie_decode_ranges(struct brcm_pcie_softc *sc,
    struct ofw_pci_range *range, int ranges)
{
	int i, mems;

	mems = 0;
	for (i = 0; i < ranges; i++) {
		switch (range[i].pci_hi & OFW_PCI_PHYS_HI_SPACEMASK) {
		case OFW_PCI_PHYS_HI_SPACE_MEM32:
		case OFW_PCI_PHYS_HI_SPACE_MEM64:
			++mems;
			break;
		default:
			break;
		}
	}

	sc->mem_range = malloc(mems * sizeof(*sc->mem_range), M_DEVBUF,
	    M_WAITOK);
	sc->mem_ranges = mems;

	mems = 0;
	for (i = 0; i < ranges; i++) {
		switch (range[i].pci_hi & OFW_PCI_PHYS_HI_SPACEMASK) {
		case OFW_PCI_PHYS_HI_SPACE_MEM32:
		case OFW_PCI_PHYS_HI_SPACE_MEM64:
			MPASS(mems < sc->mem_ranges);
			sc->mem_range[mems] = range[i];
			++mems;
			break;

		default:
			device_printf(sc->dev,
				    "%s: Unsupported range type (0x%X)\n",
				    __func__, range[i].pci_hi &
				    OFW_PCI_PHYS_HI_SPACEMASK);
		}
	}

	MPASS(mems == sc->mem_ranges);

	if (mems== 0) {
		device_printf(sc->dev,
		    "Missing required memory range(s) in DT\n");
		return (ENXIO);
	}

	return (0);
}

static int
brcm_pcie_setup_hw(struct brcm_pcie_softc *sc)
{
	int i, rv;
	uint32_t reg, cap_lanes;

	rv = 0;
	sc->hw_rev = RD4(sc, PCIE_MISC_REVISION);

	/* Enable clockls */
	if (sc->clk_sw_pcie != NULL)
		rv = clk_enable(sc->clk_sw_pcie);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot enable 'aclk' clock: %d\n", rv);
		return (rv);
	}

	rv = hwreset_assert(sc->hwreset_rescal);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot assert  'rescal' reset: %d\n",
		     rv);
		return (rv);
	}
	rv = hwreset_deassert(sc->hwreset_rescal);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot deassert  'rescal' reset: %d\n",
		     rv);
		return (rv);
	}

	brcm_pcie_perst(sc, true);
	rv = brcm_pcie_reset_bridge(sc, true);
	if (rv != 0)
		return (rv);
	DELAY(200);
	rv = brcm_pcie_reset_bridge(sc, false);
	if (rv != 0)
		return (rv);

	reg = RD4(sc, sc->soc->hard_debug_reg);
	reg &= ~PCIE_HARD_DEBUG_SERDES_IDDQ;
	WR4(sc, sc->soc->hard_debug_reg, reg);
	DELAY(200);

	reg = RD4(sc, PCIE_MISC_MISC_CTRL);
	reg &= ~PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK;
	if (sc->soc->type == BCM2711)
		reg |= PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_128;
	else
		reg |= PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_512;
	reg |= PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN;
	reg |= PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE;
	reg |= PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE;
	reg |= PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE;
	WR4(sc, PCIE_MISC_MISC_CTRL, reg);


	/* Self-identify as a PCI bridge. */
	reg = RD4(sc, PCIE_RC_CFG_PRIV1_ID_VAL3);
	reg &= ~PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_MASK;
	reg |= (PCIC_BRIDGE << 16) | ( PCIS_BRIDGE_HOST << 8);
	reg |= (PCIC_BRIDGE << 16) | ( PCIS_BRIDGE_PCI << 8);
	WR4(sc, PCIE_RC_CFG_PRIV1_ID_VAL3, reg);

	if (sc->soc->type == BCM2712) {
		struct {
			uint16_t addr;
			uint16_t data;
		} regs[] = {
			{ 0x16, 0x50B9 },
			{ 0x17, 0xBDA1 },
			{ 0x18, 0x0094 },
			{ 0x19, 0x97B4 },
			{ 0x1B, 0x5030 },
			{ 0x1C, 0x5030 },
			{ 0x1E, 0x0007 },
		};

		/* Make sure read errors return 0xffffffff  */
		reg = RD4(sc, PCIE_MISC_UBUS_CTRL);
		reg &= ~PCIE_MISC_UBUS_CTRL_UBUS_PCIE_REPLY_ERR_DIS;
		reg &= ~PCIE_MISC_UBUS_CTRL_UBUS_PCIE_REPLY_DECERR_DIS;
		WR4(sc, PCIE_MISC_UBUS_CTRL, reg);
		WR4(sc, PCIE_MISC_AXI_READ_ERROR_DATA, 0xEEEEEEEE); //0xFFFFFFFF);

		/* Magic to select a 54MHz refclk soure? */
		rv = brcm_pcie_mdio_write(sc, 0, MDIO_SET_ADDR, 0x1600);
		if (rv)
			return (rv);
		for (i = 0; i < nitems(regs); i++) {
			rv = brcm_pcie_mdio_write(sc, 0,
			     regs[i].addr, regs[i].data);
			if (rv)
				return (rv);
		}

		DELAY(100);

		/* Adjust L1SS sub-state timers. */
		reg = RD4(sc, PCIE_RC_PL_PHY_CTL_15);
		reg &= ~PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK;
		reg |= 18 << PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_SHIFT;
		WR4(sc, PCIE_RC_PL_PHY_CTL_15, reg);
	}



	reg = RD4(sc, sc->pcie_hard_debug);
	if (strcmp(sc->clkreq_mode, "no-l1ss") == 0) {
		reg |= PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE;
	} else if (strcmp(sc->clkreq_mode, "default") == 0) {
		reg |= PCIE_HARD_DEBUG_L1SS_ENABLE;
	}
	WR4(sc, sc->pcie_hard_debug, reg);

	/* Unadvertise L1SS if appropriate. */
	if (strcmp(sc->clkreq_mode, "no-l1ss") == 0) {
		reg = RD4(sc, PCIE_RC_CFG_PRIV1_ROOT_CAP);
		reg &= ~PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK;
		reg |= (1 << PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_SHIFT);
		WR4(sc, PCIE_RC_CFG_PRIV1_ROOT_CAP, reg);
	}

	if (sc->enable_ssc) {
		rv = brcm_pcie_setup_ssc(sc);
		device_printf(sc->dev, "Cannot enable SSC: %d\n", rv);
	}

	reg = RD4(sc, PCIE_RC_CFG_PRIV1_LINK_CAP);
	cap_lanes = (reg &
	    PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_MASK) >>
	    PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_SHIFT;
	dprintf("cap_lanes: %d, num_lanes: %d\n", cap_lanes, sc->num_lanes);

	if (sc->num_lanes > 0 && sc->num_lanes <= 4 &&
	    sc->num_lanes != cap_lanes) {
		reg = RD4(sc, PCIE_RC_CFG_PRIV1_LINK_CAP);
		reg &= ~PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_MASK;
		reg  |= sc->num_lanes <<
		    PCIE_RC_CFG_PRIV1_LINK_CAP_MAX_LINK_WIDTH_SHIFT;
		WR4(sc, PCIE_RC_CFG_PRIV1_LINK_CAP, reg);

		reg = RD4(sc, PCIE_RC_PL_REG_PHY_CTL_1);
		reg |= PCIE_RC_PL_REG_P2_POWERDOWN_ENA_NOSYNC_MASK;
		WR4(sc, PCIE_RC_PL_REG_PHY_CTL_1, reg);
	}

	brcm_pcie_perst(sc, false);

	/* Wait for the link to come up. */
	for (i = 10000; i > 0; i--) {
		if (brcm_pcie_get_link(sc))
			break;
		DELAY(10);
	}
	if (i == 0) {
		device_printf(sc->dev, "Link-up timeouted\n");
		return (ETIMEDOUT);
	}


	brcm_pcie_setup_outbound(sc);
	rv = brcm_pcie_setup_inbound(sc);
	if (rv != 0)
		return (rv);

	return (0);
}

static int
brcm_pcie_parse_fdt_resources(struct brcm_pcie_softc *sc)
{
	int rv;
	u_int32_t br[2];

	/* Resets. */
	rv = hwreset_get_by_ofw_name(sc->dev, 0, "perst", &sc->hwreset_perst);
	if (rv != 0 && rv != ENOENT) {
		device_printf(sc->dev, "Cannot get 'perst' reset: %d\n", rv);
		return (ENXIO);
	}
	rv = hwreset_get_by_ofw_name(sc->dev, 0, "rescal", &sc->hwreset_rescal);
	if (rv != 0 && rv != ENOENT) {
		device_printf(sc->dev, "Cannot get 'rescal' reset: %d\n", rv);
		return (ENXIO);
	}
	rv = hwreset_get_by_ofw_name(sc->dev, 0, "bridge", &sc->hwreset_bridge);
	if (rv != 0 && rv != ENOENT) {
		device_printf(sc->dev, "Cannot get 'bridge' reset: %d\n", rv);
		return (ENXIO);
	}
	rv = hwreset_get_by_ofw_name(sc->dev, 0, "swinit", &sc->hwreset_swinit);
	if (rv != 0 && rv != ENOENT) {
		device_printf(sc->dev, "Cannot get 'swinit' reset: %d\n", rv);
		return (ENXIO);
	}

	/* Clocks. */
	rv = clk_get_by_ofw_name(sc->dev, 0, "pex", &sc->clk_sw_pcie);
	if (rv != 0 && rv != ENOENT) {
		device_printf(sc->dev, "Cannot get 'sw_pcie' clock: %d\n", rv);
		return (ENXIO);
	}
	sc->coherent = OF_hasprop(sc->node, "dma-coherent");
	sc->no_l0s = OF_hasprop(sc->node, "aspm-no-l0s");
	sc->enable_ssc = OF_hasprop(sc->node, "brcm,enable-ssc");

	rv = OF_getprop(sc->node, "brcm,clkreq-mode", sc->clkreq_mode,
	    sizeof(sc->clkreq_mode));
	if (rv <= 0)
		strcpy(sc->clkreq_mode, "default");

	rv = OF_getencprop(sc->node, "num-lanes", &sc->num_lanes,
	    sizeof(sc->num_lanes));
	if (rv != sizeof(sc->num_lanes))
		sc->num_lanes = 1;
	dprintf("%s: num lanes: %d\n", __func__, sc->num_lanes);

	if (OF_hasprop(sc->node, "bus-range")) {
		rv = OF_getencprop(sc->node, "bus-range", br, sizeof(br));
		if (rv < 0 || rv != sizeof(br)) {
			device_printf(sc->dev,
			    "Malformed 'bus-range' property: %d\n", rv);
			return (ENXIO);
		}
		sc->bus_start = br[0];
		sc->bus_end = br[1];
	} else {
		sc->bus_start = 0;
		sc->bus_end = 255;
	}
	sc->root_bus = sc->bus_start;
	sc->sec_bus = sc->bus_start + 1;
	dprintf("%s: bus range[%d..%d], root bus %d, sub bus: %d\n", __func__,
	    sc->bus_end, sc->bus_start, sc->root_bus, sc->sec_bus);

	rv = OF_getencprop_alloc_multi(sc->node, "brcm,scb-sizes",
	    sizeof(sc->memc_size[0]), (void **)&sc->memc_size);
	if (rv < 0)
		sc->memc_sizes = 0;

	return (0);
}


static bus_dma_tag_t
brcm_pcie_get_dma_tag(device_t dev, device_t child)
{

	struct brcm_pcie_softc *sc;

	sc = device_get_softc(dev);
	return (sc->dmat);
}

static int
brcm_pcie_busdma_mapseg(void *arg, bus_dma_tag_t dmat, bus_addr_t *addr,
    bus_size_t size)
{
	struct brcm_pcie_softc *sc;
	int i;
	uint64_t pci_start, host_start, host_end;

	sc = arg;

	for (i = 0; i < sc->ofw_pci.sc_dma_nrange; i++) {
		pci_start  = sc->ofw_pci.sc_dma_range[i].pci;
		host_start = sc->ofw_pci.sc_dma_range[i].host;
		host_end = host_start + sc->ofw_pci.sc_dma_range[i].size;

		if (*addr >= host_start && *addr + size <= host_end) {
			*addr -= host_start;
			*addr += pci_start;
			return (0);
		}
	}
	panic("%s: Cannot map DMA segment:  PA: 0x%lX, size: %lu\n", __func__,
	    *addr, size);
	return (EINVAL);
}


static int
brcm_pcie_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data != 0) {
		device_set_desc(dev, "Broadcom STB PCIe Controller");
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

static int
brcm_pcie_attach(device_t dev)
{
	struct brcm_pcie_softc *sc;

	int rv, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->soc = (struct brcm_pcie_soc *)ofw_bus_search_compatible(dev,
	    compat_data)->ocd_data;
	mtx_init(&sc->mtx, "msi_mtx", NULL, MTX_DEF);

	rv = brcm_pcie_parse_fdt_resources(sc);
	if (rv != 0) {
		device_printf(dev, "Cannot parse FDT resources\n");
		return (rv);
	}

	/* Allocate bus_space resources. */
	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		rv = ENXIO;
		goto out;
	}

	/*
	 * Get PCI interrupt
	 */
	rv = ofw_bus_find_string_index(sc->node, "interrupt-names", "pcie",
	    &rid);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'pcie' interrupt: %d\n", rv);
		rv = ENXIO;
		goto out;
	}
	sc->pcie_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (sc->pcie_irq_res == NULL) {
		device_printf(dev, "Cannot allocate 'pcie' interrupt: %d\n", rv);
		rv = ENXIO;
		goto out;
	}

	rv = ofw_bus_find_string_index(sc->node, "interrupt-names", "msi",
	    &rid);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'msi' interrupt: %d\n", rv);
		rv = ENXIO;
		goto out;
	}
	sc->msi_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	if (sc->msi_irq_res == NULL) {
		device_printf(dev, "Cannot allocate 'msi' interupt\n");
		rv = ENXIO;
		goto out;
	}

	rv = ofw_pcib_init(dev);
	if (rv != 0)
		goto out;

	rv = brcm_pcie_decode_ranges(sc, sc->ofw_pci.sc_range,
	    sc->ofw_pci.sc_nrange);
	if (rv != 0)
		goto out;

	if (bootverbose)
		device_printf(dev, "Bus is %s cache-coherent\n",
		    sc->coherent ? "" : " not");

	rv = bus_dma_tag_create(bus_get_dma_tag(dev), /* parent */
	    1, 0,				/* alignment, bounds */
	    BUS_SPACE_MAXADDR,			/* lowaddr */
	    BUS_SPACE_MAXADDR,			/* highaddr */
	    NULL, NULL,				/* filter, filterarg */
	    BUS_SPACE_MAXSIZE,			/* maxsize */
	    BUS_SPACE_UNRESTRICTED,		/* nsegments */
	    BUS_SPACE_MAXSIZE,			/* maxsegsize */
	    sc->coherent ? BUS_DMA_COHERENT : 0, /* flags */
	    NULL, NULL,				/* lockfunc, lockarg */
	    &sc->dmat);
	if (rv != 0) {
		device_printf(dev, "Cannot crete dma tag: %d\n", rv);
		rv = ENXIO;
		goto out;
	}
	bus_dma_tag_set_mapseg(sc->dmat, brcm_pcie_busdma_mapseg, sc);


	rv = brcm_pcie_setup_hw(sc);
	if (rv != 0)
		goto out;

	if (bus_setup_intr(dev, sc->pcie_irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
		    brcm_pcie_intr_pcie, NULL, sc, &sc->pcie_intr_cookie)) {
		device_printf(dev, "Cannot setup 'pcie' interrupt handler\n");
		rv = ENXIO;
		goto out;
	}

	if (bus_setup_intr(dev, sc->msi_irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
		    brcm_pcie_intr_msi, NULL, sc, &sc->msi_intr_cookie)) {
		device_printf(dev, "Cannot setup 'pcie' interrupt handler\n");
		rv = ENXIO;
		goto out;
	}

	device_add_child(dev, "pci", DEVICE_UNIT_ANY);
	bus_attach_children(dev);

	return (0);

out:

	return (rv);
}

static device_method_t brcm_pcie_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			brcm_pcie_probe),
	DEVMETHOD(device_attach,		brcm_pcie_attach),

	/* Bus interface */
	DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,		bus_generic_teardown_intr),
	DEVMETHOD(bus_get_dma_tag,		brcm_pcie_get_dma_tag),


	/* pcib interface */
	DEVMETHOD(pcib_maxslots,		brcm_pcie_maxslots),
	DEVMETHOD(pcib_read_config,		brcm_pcie_read_config),
	DEVMETHOD(pcib_write_config,		brcm_pcie_write_config),
	DEVMETHOD(pcib_get_id,			brcm_pcie_get_id),
	DEVMETHOD(pcib_request_feature,		pcib_request_feature_allow),

	/* MSI/MSI-X */
	DEVMETHOD(pcib_alloc_msi,		brcm_pcie_alloc_msi),
	DEVMETHOD(pcib_release_msi,		brcm_pcie_release_msi),
	DEVMETHOD(pcib_alloc_msix,		brcm_pcie_alloc_msix),
	DEVMETHOD(pcib_release_msix,		brcm_pcie_release_msix),
	DEVMETHOD(pcib_map_msi,			brcm_pcie_map_msi),

	DEVMETHOD_END
};

DEFINE_CLASS_1(pcib, brcmstb_pcie_driver, brcm_pcie_methods,
    sizeof(struct brcm_pcie_softc), ofw_pcib_driver);
DRIVER_MODULE(brcmstb_pcie, simplebus, brcmstb_pcie_driver, NULL, NULL);
