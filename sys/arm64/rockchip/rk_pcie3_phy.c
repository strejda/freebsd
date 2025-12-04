/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021, 2022 Soren Schmidt <sos@deepcore.dk>
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
 *
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/fdt/simple_mfd.h>

#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>
#include <dev/regulator/regulator.h>
#include <dev/syscon/syscon.h>
#include <dev/phy/phy.h>

#include <contrib/device-tree/include/dt-bindings/phy/phy.h>

#include "syscon_if.h"
#include "phydev_if.h"
#include "phynode_if.h"

#define	PCIE3_PHY_RK3568	1
#define	PCIE3_PHY_RK3588	2

/* RK3568 */
#define	GRF_PCIE30PHY_CON1		0x04
#define	GRF_PCIE30PHY_CON4		0x10
#define	GRF_PCIE30PHY_CON5		0x14
#define	GRF_PCIE30PHY_CON6		0x18
#define	 GRF_BIFURCATION_LANE_1		0
#define	 GRF_BIFURCATION_LANE_2		1
#define	 GRF_PCIE30PHY_WR_EN		(0xf << 16)
#define	GRF_PCIE30PHY_CON9		0x24
#define	 GRF_PCIE30PHY_DA_OCM_MASK	(1 << (15 + 16))
#define	 GRF_PCIE30PHY_DA_OCM		((1 << 15) | GRF_PCIE30PHY_DA_OCM_MASK)
#define	GRF_PCIE30PHY_STATUS0		0x80
#define	 GRF_PCIE30PHY_SRAM_INIT_DONE	 (1 << 14)


/* RK3588 */
#define PHP_PCIESEL_CON			0x100
#define PCIE3PHY_CMN_CON0		0x000
#define PCIE3PHY_CLAMP_DIS		 (1 << 8)
#define PCIE3PHY_CLAMP_MASK		 ((1 << 8) << 16)
#define RK3588_PCIE30_MODE_SHIFT	0
#define RK3588_PCIE30_MODE_MASK		((0x7 <<R K3588_PCIE30_MODE_SHIFT)  << 16)

#define PCIE3PHY_PHY0_STATUS1		0x904
#define PCIE3PHY_PHY1_STATUS1		0xa04
#define PCIE3PHY_PHY0_LN0_CON1		0x1004
#define PCIE3PHY_PHY0_LN1_CON1		0x1104
#define PCIE3PHY_PHY1_LN0_CON1		0x2004
#define PCIE3PHY_PHY1_LN1_CON1		0x2104
#define PCIE3PHY_SRAM_INIT_DONE		 (0x01 << 0)

#define PCIE3PHY_BIFURCATION_LANE_0_1		(1 << 0)
#define PCIE3PHY_BIFURCATION_LANE_2_3		(1 << 1)
#define PCIE3PHY_LANE_AGGREGATION		(1 << 2)
#define PCIE3PHY_RX_CMN_REFCLK_MODE_MASK	((1 << 7) << 16)
#define PCIE3PHY_RX_CMN_REFCLK_MODE_SHIT	7

#define PCIE3PHY_PCIE1LN_SEL_MASK		((0x03 << 0) << 16)
#define PCIE3PHY_PCIE1LN_SEL_SHIFT		0
#define PCIE3PHY_PCIE30_PHY_MODE_MASK		((0x07 << 0) << 16)
#define PCIE3PHY_PCIE30_PHY_MODE_SHIFT		0

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk3568-pcie3-phy",	PCIE3_PHY_RK3568},
	{"rockchip,rk3588-pcie3-phy",	PCIE3_PHY_RK3588},
	{NULL, 0}
};

struct rk_pcie3_phy_softc {
	device_t	dev;
	phandle_t	node;
	int		phy_type;
	struct resource	*mem;
	struct phynode	*phynode;
	struct syscon	*phy_grf;
	struct syscon	*pipe_grf;
	clk_t		refclk_m;
	clk_t		refclk_n;
	clk_t		pclk;
	hwreset_t	phy_reset;
	uint32_t 	data_lanes[4];
	int		num_data_lanes;
	uint32_t	rx_refclk_mode[4];
};




/* PHY class and methods */
static int
rk3568_pcie3_phy_enable(struct phynode *phynode, bool enable)
{
	device_t dev = phynode_get_device(phynode);
	struct rk_pcie3_phy_softc *sc = device_get_softc(dev);
	int count;

	if (enable) {
		/* Pull PHY out of reset */
		if (sc->phy_reset != NULL)
			hwreset_deassert(sc->phy_reset);

		/* Poll for SRAM loaded and ready */
		for (count = 100; count; count--) {
			if (SYSCON_READ_4(sc->phy_grf, GRF_PCIE30PHY_STATUS0) &
			    GRF_PCIE30PHY_SRAM_INIT_DONE)
				break;
			DELAY(10000);
			if (count == 0) {
				device_printf(dev, "SRAM init timeout!\n");
				return (ENXIO);
			}
		}
	} else {
		/* Pull PHY to  reset */
		if (sc->phy_reset != NULL)
			hwreset_assert(sc->phy_reset);
	}
	return (0);
}

static phynode_method_t rk3568_pcie3_phy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,	rk3568_pcie3_phy_enable),

	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(rk3568_pcie3_phy_phynode, rk3568_pcie3_phy_phynode_class,
    rk3568_pcie3_phy_phynode_methods, 0, phynode_class);

static void
rk3568_pciephy_bifurcate(struct rk_pcie3_phy_softc *sc, int control,
    uint32_t lane)
{

	switch (lane) {
	case 0:
		SYSCON_WRITE_4(sc->phy_grf, control, GRF_PCIE30PHY_WR_EN);
		return;
	case 1:
		SYSCON_WRITE_4(sc->phy_grf, control,
		    GRF_PCIE30PHY_WR_EN | GRF_BIFURCATION_LANE_1);
		break;
	case 2:
		SYSCON_WRITE_4(sc->phy_grf, control,
		    GRF_PCIE30PHY_WR_EN | GRF_BIFURCATION_LANE_2);
		break;
	default:
		device_printf(sc->dev, "Illegal lane %d\n", lane);
		return;
	}
	if (bootverbose)
		device_printf(sc->dev, "lane %d @ pcie3x%d\n", lane,
		    (control == GRF_PCIE30PHY_CON5) ? 1 : 2);
}
static int
rk3568_init(struct rk_pcie3_phy_softc *sc)
{
	/* Deassert PCIe PMA output clamp mode */
	SYSCON_WRITE_4(sc->phy_grf, GRF_PCIE30PHY_CON9, GRF_PCIE30PHY_DA_OCM);

	/* Configure PHY HW accordingly */
	rk3568_pciephy_bifurcate(sc, GRF_PCIE30PHY_CON5, sc->data_lanes[0]);
	rk3568_pciephy_bifurcate(sc, GRF_PCIE30PHY_CON6, sc->data_lanes[1]);

	if (sc->data_lanes[0] || sc->data_lanes[1])
		SYSCON_WRITE_4(sc->phy_grf, GRF_PCIE30PHY_CON1,
		    GRF_PCIE30PHY_DA_OCM);
	else
		SYSCON_WRITE_4(sc->phy_grf, GRF_PCIE30PHY_CON1,
		    GRF_PCIE30PHY_DA_OCM_MASK);

	return (0);
}

static int
rk3588_pcie3_phy_enable(struct phynode *phynode, bool enable)
{
	device_t dev;
	struct rk_pcie3_phy_softc *sc;
	int count;

	dev = phynode_get_device(phynode);
	sc = device_get_softc(dev);
	if (enable) {
		/* Pull PHY out of reset */
		if (sc->phy_reset != NULL)
			hwreset_deassert(sc->phy_reset);

		/* Poll for SRAM loaded and ready */
		for (count = 100; count; count--) {
			if (SYSCON_READ_4(sc->phy_grf,
			    PCIE3PHY_PHY0_STATUS1) &
			    GRF_PCIE30PHY_SRAM_INIT_DONE)
				break;
			DELAY(10000);
			if (count == 0) {
				device_printf(dev, "SRAM(0) init timeout!\n");
				return (ENXIO);
			}
		}
		for (count = 100; count; count--) {
			if (SYSCON_READ_4(sc->phy_grf,
			     PCIE3PHY_PHY1_STATUS1) &
			    GRF_PCIE30PHY_SRAM_INIT_DONE)
				break;
			DELAY(10000);
			if (count == 0) {
				device_printf(dev, "SRAM(1) init timeout!\n");
				return (ENXIO);
			}
		}
	} else {
		/* Pull PHY to  reset */
		if (sc->phy_reset != NULL)
			hwreset_assert(sc->phy_reset);
	}

	return (0);
}

static phynode_method_t rk3588_pcie3_phy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,	rk3588_pcie3_phy_enable),

	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(rk3588_pcie3_phy_phynode, rk3588_pcie3_phy_phynode_class,
    rk3588_pcie3_phy_phynode_methods, 0, phynode_class);

static uint32_t rx_refclk_reg[4] = {
	PCIE3PHY_PHY0_LN0_CON1,
	PCIE3PHY_PHY0_LN1_CON1,
	PCIE3PHY_PHY1_LN0_CON1,
	PCIE3PHY_PHY1_LN1_CON1,
};

static int
rk3588_init(struct rk_pcie3_phy_softc *sc)
{
	int i;
	uint32_t val, tmp;

	/* Apply rx_refclk_mode(s) */
	for (i = 0; i < 4; i++) {
		val = 0;
		if (sc->rx_refclk_mode[i] != 0)
			val |=	PCIE3PHY_RX_CMN_REFCLK_MODE_MASK;
		SYSCON_WRITE_4(sc->phy_grf, rx_refclk_reg[i],
		    PCIE3PHY_RX_CMN_REFCLK_MODE_MASK |
		    (val << PCIE3PHY_RX_CMN_REFCLK_MODE_SHIT));
	}

	/* Deassert PCIe PMA output clamp mode */
	SYSCON_WRITE_4(sc->phy_grf, PCIE3PHY_CMN_CON0,
	    PCIE3PHY_CLAMP_MASK | PCIE3PHY_CLAMP_DIS);

	/* Compute bifurcation */
	val =  0;
	for (i = 0; i < sc->num_data_lanes; i++) {
		if (sc->data_lanes[i] <= 1)
			val = PCIE3PHY_LANE_AGGREGATION;
		if (sc->data_lanes[i] == 3)
			val |= PCIE3PHY_BIFURCATION_LANE_0_1;
		if (sc->data_lanes[i] == 4)
			val |= PCIE3PHY_BIFURCATION_LANE_2_3;
	}
	SYSCON_WRITE_4(sc->phy_grf, PCIE3PHY_CMN_CON0,
	    PCIE3PHY_PCIE30_PHY_MODE_MASK |
	    (val << PCIE3PHY_PCIE30_PHY_MODE_SHIFT));

	tmp = val & (PCIE3PHY_BIFURCATION_LANE_0_1 |
	     PCIE3PHY_BIFURCATION_LANE_2_3);
	if (sc->pipe_grf != NULL && tmp != 0) {
		SYSCON_WRITE_4(sc->pipe_grf, PHP_PCIESEL_CON,
    			PCIE3PHY_PCIE1LN_SEL_MASK |
    		tmp << PCIE3PHY_PCIE1LN_SEL_SHIFT);
	}

	return (0);
}



/* Device class and methods */
static int
rk_pcie3_phy_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "Rockchip PCIe PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
rk_pcie3_phy_attach(device_t dev)
{
	struct rk_pcie3_phy_softc *sc;
	struct phynode_init_def phy_init;
	struct phynode *phynode;
	int rid, rv,i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->phy_type =  ofw_bus_search_compatible(dev, compat_data)->ocd_data;

	/* Get memory resource */
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE | RF_SHAREABLE);
	if (sc->mem == NULL) {
		device_printf(dev, "Cannot allocate memory resources\n");
		return (ENXIO);
	}

	/* Get syncons handle(s) */
	rv = syscon_get_by_ofw_property(dev, sc->node, "rockchip,phy-grf",
	    &sc->phy_grf);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'phy-grf' syscon: %d\n", rv);
		return (ENXIO);
	}

	if (sc->phy_type == PCIE3_PHY_RK3588) {
		rv = syscon_get_by_ofw_property(dev, sc->node,
		    "rockchip,pipe-grf", &sc->pipe_grf);
		if (rv != 0) {
			device_printf(dev, "Cannot get 'phy-grf' syscon: %d\n",
			    rv);
		return (ENXIO);
		}
	}

	/* Get & enable clocks */
	rv = clk_get_by_ofw_name(dev, 0, "pclk", &sc->pclk);
	if (rv != 0) {
		device_printf(dev, "Getting 'pclk' failed: %d\n", rv);
		return (ENXIO);
	}
	rv = clk_enable(sc->pclk);
	if (rv != 0) {
		device_printf(dev, "Enable 'pclk' failed: %d\n", rv);
		return (ENXIO);
	}

	if (sc->phy_type == PCIE3_PHY_RK3568) {
		rv = clk_get_by_ofw_name(dev, 0, "refclk_m", &sc->refclk_m);
		if (rv != 0) {
			device_printf(dev, "Getting 'refclk_m' failed: %d\n",
			    rv);
			return (ENXIO);
		}
		rv = clk_enable(sc->refclk_m);
		if (rv != 0) {
			device_printf(dev, "Enable 'refclk_m' failed: %d\n",
			    rv);
			return (ENXIO);
		}

		rv = clk_get_by_ofw_name(dev, 0, "refclk_n", &sc->refclk_n);
		if (rv != 0) {
			device_printf(dev, "Getting 'refclk_n' failed: %d\n",
			    rv);
			return (ENXIO);
		}
		rv = clk_enable(sc->refclk_n);
		if (rv != 0) {
			device_printf(dev, "Enable 'refclk_n' failed: %d\n",
			    rv);
			return (ENXIO);
		}
	}

	/* Read data-lanes */
	if (OF_hasprop(sc->node, "data-lanes")) {
		rv = OF_getencprop(sc->node, "data-lanes", sc->data_lanes,
		    sizeof(sc->data_lanes));
		if (rv <= 0) {
			device_printf(dev, "Cannot read 'data-lanes': %d\n",
			    rv);
			return (ENXIO);
		}
		sc->num_data_lanes = rv / sizeof(uint32_t);
		if (sc->num_data_lanes < 2) {
			device_printf(dev,
			    "Unexpected format of 'data-lanes'\n");
			return (ENXIO);
		}
	} else {
		sc->num_data_lanes = 1;
		sc->data_lanes[0] = 1;
		if (bootverbose)
			device_printf(dev, "Impicit data-lanes config used.\n");
	}

	/* Read rockchip,rx-common-refclk-mode */
	if (OF_hasprop(sc->node, "rockchip,rx-common-refclk-mode")) {
		rv = OF_getencprop(sc->node, "rockchip,rx-common-refclk-mode",
		    sc->rx_refclk_mode, sizeof(sc->rx_refclk_mode));
		if (rv <= 0) {
			device_printf(dev,
			    "Cannot read 'rx-common-refclk-mode': %d\n", rv);
			return (ENXIO);
		}
	} else {
		for (i = 0; i < nitems(sc->rx_refclk_mode); i++)
			sc->rx_refclk_mode[i] = 1;
	}

	/* Get & assert reset */
	rv  = hwreset_get_by_ofw_name(dev, sc->node, "phy", &sc->phy_reset);
	if (rv != 0 && rv != ENOENT) {
		device_printf(dev, "Cannot get reset: %d\n", rv);
		return (ENXIO);
	}
	if (sc->phy_reset != NULL)
		hwreset_assert(sc->phy_reset);


	switch (sc->phy_type) {
	case PCIE3_PHY_RK3568:
		rv = rk3568_init(sc);
		break;

	case PCIE3_PHY_RK3588:
		rv = rk3588_init(sc);
		break;

	default:
		device_printf(dev, "Invalid pcie phy type: %d\n", sc->phy_type);
		return (ENXIO);
	}

	if (rv != 0) {
		device_printf(dev, "Cannot initiliza phy: %d\n", rv);
		return (ENXIO);
	}

	bzero(&phy_init, sizeof(phy_init));
	phy_init.id = PHY_NONE;
	phy_init.ofw_node = sc->node;

	switch (sc->phy_type) {
	case PCIE3_PHY_RK3568:
		phynode = phynode_create(dev, &rk3568_pcie3_phy_phynode_class,
		    &phy_init);
		break;

	case PCIE3_PHY_RK3588:
		phynode = phynode_create(dev, &rk3588_pcie3_phy_phynode_class,
		    &phy_init);
		break;

	default:
		panic("Invalid pcie phy type: %d\n", sc->phy_type);
	}

	if (phynode == NULL) {
		device_printf(dev, "Failed to create phy\n");
		return (ENXIO);
	}
	if (phynode_register(phynode) == NULL) {
		device_printf(dev, "Failed to register phy\n");
		return (ENXIO);
	}
	sc->phynode = phynode;

	return (0);
}

static device_method_t rk_pcie3_phy_methods[] = {
	DEVMETHOD(device_probe,		rk_pcie3_phy_probe),
	DEVMETHOD(device_attach,	rk_pcie3_phy_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(rk_pcie3_phy, rk_pcie3_phy_driver, rk_pcie3_phy_methods,
    sizeof(struct rk_pcie3_phy_softc));
EARLY_DRIVER_MODULE(rk_pcie3_phy, simplebus, rk_pcie3_phy_driver,
    0, 0, BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_MIDDLE);
