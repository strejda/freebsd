/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2021 Michal Meloun <mmel@FreeBSD.org>
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sx.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/phy/phy_internal.h>
#include <dev/extres/regulator/regulator.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>
#include <dev/psci/smccc.h>

#include "phydev_if.h"
#include "syscon_if.h"

#if 0
#define DPRINTF(fmt...) printf(format)
#else
#define DPRINTF(fmt...)
#endif

#define	MV_COMPHY_MAX_LANES 8
#define	MV_COMPHY_MAX_PORTS 3

/*
 * Firmware/SMC call related constants
 */ 

/* Opcodes */
#define	MV_SIP_COMPHY_POWER_ON		0x82000001
#define	MV_SIP_COMPHY_POWER_OFF		0x82000002
#define	MV_SIP_COMPHY_PLL_LOCK		0x82000003
#define	MV_SIP_COMPHY_XFI_TRAIN		0x82000004
#define	MV_SIP_COMPHY_DIG_RESET		0x82000005

/* Phy mode */
#define	MV_SIP_COMPHY_SATA_MODE		0x1
#define	MV_SIP_COMPHY_SGMII_MODE	0x2	/* SGMII 1G */
#define	MV_SIP_COMPHY_HS_SGMII_MODE	0x3	/* SGMII 2.5G */
#define	MV_SIP_COMPHY_USB3H_MODE	0x4
#define	MV_SIP_COMPHY_USB3D_MODE	0x5
#define	MV_SIP_COMPHY_PCIE_MODE		0x6
#define	MV_SIP_COMPHY_RXAUI_MODE	0x7
#define	MV_SIP_COMPHY_XFI_MODE		0x8
#define	MV_SIP_COMPHY_SFI_MODE		0x9
#define	MV_SIP_COMPHY_USB3_MODE		0xa
#define	MV_SIP_COMPHY_AP_MODE		0xb
#define	MV_SIP_COMPHY_UNUSED		0xFFFFFFFF

/* Port speeds */
#define	MV_SIP_COMPHY_SPEED_1_25G	0 /* SGMII 1G */
#define	MV_SIP_COMPHY_SPEED_2_5G	1
#define	MV_SIP_COMPHY_SPEED_3_125G	2 /* SGMII 2.5G */
#define	MV_SIP_COMPHY_SPEED_5G		3
#define	MV_SIP_COMPHY_SPEED_5_15625G	4 /* XFI 5G */
#define	MV_SIP_COMPHY_SPEED_6G		5
#define	MV_SIP_COMPHY_SPEED_10_3125G	6 /* XFI 10G */
#define	MV_SIP_COMPHY_SPEED_MAX		0x3F

/*
 * SMC related lane config:
 *  - bit 1~0 represent comphy polarity invert
 *  - bit 7~2 represent comphy speed
 *  - bit 11~8 represent unit index
 *  - bit 16~12 represent mode
 *  - bit 17 represent comphy indication of clock source
 *  - bit 20~18 represents pcie width (in case of pcie comphy config.)
 *  - bit 21 represents the source of the request (os=0/Bootloader=1),
 *           (reguired only for PCIe!)
 *  - bit 31~22 reserved
 */

#define	MV_SIP_COMPHY_CALLER(x)		(((x) & 0x01) << 21)
#define	MV_SIP_COMPHY_PCIE_WIDTH(x)	(((x) & 0x07) << 18)
#define	MV_SIP_COMPHY_CLK_SRC(x)	(((x) & 0x01) << 17)
#define	MV_SIP_COMPHY_MODE(x)		(((x) & 0x1F) << 12)
#define	MV_SIP_COMPHY_ID(x)		(((x) & 0x0F) <<  8)
#define	MV_SIP_COMPHY_SPEED(x)		(((x) & 0x3F) <<  2)
#define	MV_SIP_COMPHY_POL_INV(x)	(((x) & 0x03) <<  0)

#define	PHY_LOCK(_sc)		mtx_lock(&(_sc)->mtx)
#define	PHY_UNLOCK(_sc)		mtx_unlock(&(_sc)->mtx)
#define	PHY_LOCK_INIT(_sc)	mtx_init(&(_sc)->mtx, 			\
	    device_get_nameunit(_sc->dev), "mv_comphy", MTX_DEF)
#define	PHY_LOCK_DESTROY(_sc)	mtx_destroy(&(_sc)->mtx);
#define	PHY_ASSERT_LOCKED(_sc)	mtx_assert(&(_sc)->mtx, MA_OWNED);
#define	PHY_ASSERT_UNLOCKED(_sc) mtx_assert(&(_sc)->mtx, MA_NOTOWNED);

#define	RD4(sc, reg)		bus_read_4((sc)->mem_res, (reg))
#define	WR4(sc, reg, val)	bus_write_4((sc)->mem_res, (reg), (val))
#define	SC_RD4(sc, reg)		SYSCON_READ_4((sc)->syscon, (reg))
#define	SC_WR4(sc, reg, val)	SYSCON_WRITE_4((sc)->syscon, (reg), (val))

static struct ofw_compat_data compat_data[] = {
	{"marvell,comphy-cp110",	1},
	{NULL,				0}
};

struct phy_softc {
	u_int			lane;
	u_int			port;
};

struct comphy_lane {
	regulator_t		supply_vbus;
	rman_res_t		paddr;
	phy_mode_t		mode;
	phy_submode_t		submode;
};

struct comphy_softc {
	device_t		dev;
	struct mtx		mtx;
	struct resource		*mem_res;
	struct syscon		*syscon;
	clk_t			mg_clk;
	clk_t			mg_core_clk;
	clk_t			axi_clk;
	struct comphy_lane	lane[MV_COMPHY_MAX_LANES];
};

struct lane_mode {
	u_int		lane;
	u_int		port;
	phy_mode_t	phy_mode;
	phy_submode_t	phy_submode;
};

#define	MODE(l, p, pmode, psmode) {					\
	.lane = l,							\
	.port = p,							\
	.phy_mode = pmode,						\
	.phy_submode = psmode,						\
}

/* Allowed modes - per lane and port */
static struct lane_mode lane_mode_tbl[] = {
	MODE(0, 0, PHY_MODE_PCIE,	PHY_SUBMODE_NA),
	MODE(0, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(0, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(0, 1, PHY_MODE_SATA,	PHY_SUBMODE_NA),

	MODE(1, 0, PHY_MODE_USB_HOST,	PHY_SUBMODE_USB_SS),
	MODE(1, 0, PHY_MODE_USB_DEVICE,	PHY_SUBMODE_USB_SS),
	MODE(1, 0, PHY_MODE_SATA,	PHY_SUBMODE_NA),
	MODE(1, 0, PHY_MODE_PCIE,	PHY_SUBMODE_NA),
	MODE(1, 2, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(1, 2, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),

	MODE(2, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(2, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(2, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_RXAUI),
	MODE(2, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_10GBASER),
	MODE(2, 0, PHY_MODE_USB_HOST,	PHY_SUBMODE_USB_SS),
	MODE(2, 0, PHY_MODE_SATA,	PHY_SUBMODE_NA),
	MODE(2, 0, PHY_MODE_PCIE,	PHY_SUBMODE_NA),

	MODE(3, 0, PHY_MODE_PCIE,	PHY_SUBMODE_NA),
	MODE(3, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(3, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(3, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_RXAUI),
	MODE(3, 1, PHY_MODE_USB_HOST,	PHY_SUBMODE_USB_SS),
	MODE(3, 1, PHY_MODE_SATA,	PHY_SUBMODE_NA),

	MODE(4, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(4, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(4, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_10GBASER),
	MODE(4, 0, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_RXAUI),
	MODE(4, 0, PHY_MODE_USB_DEVICE,	PHY_SUBMODE_USB_SS),
	MODE(4, 1, PHY_MODE_USB_HOST,	PHY_SUBMODE_USB_SS),
	MODE(4, 1, PHY_MODE_PCIE,	PHY_SUBMODE_NA),
	MODE(4, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(4, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(4, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_10GBASER),

	MODE(5, 1, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_RXAUI),
	MODE(5, 1, PHY_MODE_SATA,	PHY_SUBMODE_NA),
	MODE(5, 2, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_SGMII),
	MODE(5, 2, PHY_MODE_ETHERNET,	PHY_SUBMODE_ETH_2500BASEX),
	MODE(5, 2, PHY_MODE_PCIE,	PHY_SUBMODE_NA),
};

static int
mv_comphy_smccc(struct comphy_softc *sc, register_t fnc,
    register_t phys, register_t id, register_t mode, register_t cmd)
{
	struct arm_smccc_res res;
	int32_t ret;

	arm_smccc_smc(fnc, phys, id, mode, cmd, 0, 0, 0, &res);
	ret = res.a0 & 0xFFFFFFFF;
	if (ret== SMCCC_RET_NOT_SUPPORTED)
		return (EOPNOTSUPP);
	if (ret != 0) {
		device_printf(sc->dev,
		    "Unexpected return value from SMCC call: %ld\n", res.a0);
		return(EINVAL);
	}
	return (0);
}

static bool
mv_comphy_validate_mode(u_int lane, u_int port, phy_mode_t mode,
     phy_submode_t submode)
{
	const struct lane_mode *cfg;
	int i;

	if (mode == PHY_MODE_INVALID)
		return (false);

	for (i = 0; i < nitems(lane_mode_tbl); i++) {
		cfg = &lane_mode_tbl[i];
		if (cfg->lane == lane &&  cfg->port == port &&
		    cfg->phy_mode == mode &&
		    (cfg->phy_submode == submode ||
		     cfg->phy_submode == PHY_SUBMODE_NA))
			return (true);
	}

	return (false);
}

static inline register_t
mv_comphy_sip_param( u_int port_no, u_int sip_mode, u_int sip_speed,
    u_int sip_size)
{
	register_t val;

	val  = MV_SIP_COMPHY_ID(port_no);
	val |= MV_SIP_COMPHY_MODE(sip_mode);
	val |= MV_SIP_COMPHY_MODE(sip_speed);
	val |= MV_SIP_COMPHY_PCIE_WIDTH(sip_size);
	val |= MV_SIP_COMPHY_CLK_SRC(0);
	val |= MV_SIP_COMPHY_POL_INV(0);
	val |= MV_SIP_COMPHY_CALLER(0);
	return (val);
}

static int
mv_comphy_up(struct comphy_softc *sc, u_int lane_no, u_int port_no)
{
	struct comphy_lane *lane;
	u_int  sip_mode, sip_speed;
	register_t sip_param;
	int rv;

	PHY_LOCK(sc);

	lane = sc->lane + lane_no;
	if (!mv_comphy_validate_mode(lane_no, port_no, lane->mode,
	    lane->submode)) {
		device_printf(sc->dev,
		    "Unsupported  mode: %d, submode: %d, lane: %d, port: %d\n",
		    lane->mode, lane->submode, lane_no, port_no);
		PHY_UNLOCK(sc);
		return(ENXIO);
	}

	switch(lane->mode) {
	case PHY_MODE_ETHERNET:
		switch (lane->submode) {
		case PHY_SUBMODE_ETH_RXAUI:
			sip_mode = MV_SIP_COMPHY_RXAUI_MODE;
			sip_speed = MV_SIP_COMPHY_SPEED_1_25G;
			break;
		case PHY_SUBMODE_ETH_SGMII:
			sip_mode = MV_SIP_COMPHY_SGMII_MODE;
			sip_speed = MV_SIP_COMPHY_SPEED_1_25G;
			break;
		case PHY_SUBMODE_ETH_2500BASEX:
			sip_mode = MV_SIP_COMPHY_HS_SGMII_MODE;
			sip_speed = MV_SIP_COMPHY_SPEED_3_125G;
			break;
		case PHY_SUBMODE_ETH_10GBASER:
			sip_mode = MV_SIP_COMPHY_XFI_MODE;
			sip_speed = MV_SIP_COMPHY_SPEED_10_3125G;
			break;
		default:
			panic("Uunimplemented but validated  ethernet mode");
		}
		sip_param = mv_comphy_sip_param(port_no, sip_mode, sip_speed,
		     0);
		break;
	case PHY_MODE_USB_HOST:
		sip_param = mv_comphy_sip_param(port_no,
		     MV_SIP_COMPHY_USB3H_MODE, MV_SIP_COMPHY_SPEED_MAX, 0);
		break;
	case PHY_MODE_USB_DEVICE:
		sip_param = mv_comphy_sip_param(port_no,
		    MV_SIP_COMPHY_USB3D_MODE, MV_SIP_COMPHY_SPEED_MAX, 0);
		break;

	case PHY_MODE_SATA:
		sip_param = mv_comphy_sip_param(port_no,
		    MV_SIP_COMPHY_SATA_MODE, MV_SIP_COMPHY_SPEED_MAX, 0);
		break;
	case PHY_MODE_PCIE:
		sip_param = mv_comphy_sip_param(port_no,
		    MV_SIP_COMPHY_PCIE_MODE, MV_SIP_COMPHY_SPEED_5G,
		    lane->submode);
		break;
	default:
		panic("Unimplemented but validated  mode");
	}
	rv = mv_comphy_smccc(sc, MV_SIP_COMPHY_POWER_ON, lane->paddr, lane_no,
	    sip_param, 0);
	if (rv != EOPNOTSUPP) {
		PHY_UNLOCK(sc);
		return (rv);
	}

#ifdef not_yet
	/* TODO: We have the old firmware, do it manually. */
	rv = mv_comphy_power_on(sc, lane_no, port_no);
#endif

	PHY_UNLOCK(sc);
	return (0);
}


static int
mv_comphy_down(struct comphy_softc *sc, u_int lane_no, u_int port_no)
{
	struct comphy_lane *lane;
	int rv;

	PHY_LOCK(sc);

	lane = sc->lane + lane_no;

	/* Try secure firmware mode first. */
	rv = mv_comphy_smccc(sc, MV_SIP_COMPHY_POWER_OFF,
	    lane->paddr, lane_no, 0, 0);
	if (rv != EOPNOTSUPP) {
		PHY_UNLOCK(sc);
		return (rv);
	}
#ifdef not_yet
	/* TODO: We have the old firmware, do it manually. */
	rv = mv_comphy_power_off(sc, lane_no, port_no);
#endif
	PHY_UNLOCK(sc);
	return (rv);
}

static int
mv_comphy_set_mode(struct phynode *phynode, phy_mode_t mode,
    phy_submode_t submode)
{
	struct comphy_softc *sc;
	struct phy_softc *physc;
	struct comphy_lane *lane;
	device_t dev;
	u_int lane_no, port_no;

	physc = phynode_get_softc(phynode);
	lane_no = physc->lane;
	port_no = physc->port;
	dev = phynode_get_device(phynode);
	sc = device_get_softc(dev);
	lane = sc->lane + lane_no;
	
	PHY_LOCK(sc);
	DPRINTF("%s: enter: lane: %u, port: %u, mode : %d, submode: %d\n",
	    __func__, lane_no, port_no, mode, submode);
	if (!mv_comphy_validate_mode(lane_no, port_no, mode, submode)) {
		device_printf(sc->dev,
		    "Unsupported  mode: %d, submode: %d, lane: %d, port: %d\n",
		    lane->mode, lane->submode, lane_no, port_no);
		PHY_UNLOCK(sc);
		return(ENXIO);
	}
	if (mode == PHY_MODE_PCIE && 
	    submode != 1 && submode != 2 && submode != 4 && submode != 8 &&
	    submode != 12 && submode != 16 && submode != 32) {
		device_printf(sc->dev,
		    "Unsupported  PCIe link width: %d for lane: %d, port: %d\n",
		    submode, lane_no, port_no);
		PHY_UNLOCK(sc);
		return(ENXIO);
	}
	    
	lane->mode = mode;
	lane->submode = submode;
	DPRINTF("%s: done OK: lane: %u, port: %u, mode : %d, submode: %d\n",
	    __func__, lane_no, port_no, mode, submode);
	PHY_UNLOCK(sc);
	return (0);
}

static int
mv_comphy_enable(struct phynode *phynode, bool enable)
{
	struct comphy_softc *sc;
	struct phy_softc *physc;
	device_t dev;
	u_int lane_no, port_no;
	int rv;

	physc = phynode_get_softc(phynode);
	lane_no = physc->lane;
	port_no = physc->port;
	dev = phynode_get_device(phynode);
	sc = device_get_softc(dev);

	if (enable)
		rv = mv_comphy_up(sc, lane_no, port_no);
	 else
		rv = mv_comphy_down(sc, lane_no, port_no);

	return (rv);
}

/* Phy class and methods. */
static phynode_method_t mv_comphy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,		mv_comphy_enable),
	PHYNODEMETHOD(phynode_set_mode,		mv_comphy_set_mode),

	PHYNODEMETHOD_END
};

DEFINE_CLASS_1(mv_comphy_phynode, mv_comphy_phynode_class,
    mv_comphy_phynode_methods, sizeof( struct phy_softc), phynode_class);

static int
mv_comphy_lane_init(struct comphy_softc *sc, u_int lane_no, phandle_t node)
{
	struct comphy_lane *lane;
	phandle_t connector;
	int rv;

	lane = sc->lane + lane_no;
	lane->paddr =  rman_get_start(sc->mem_res);
	lane->mode = PHY_MODE_INVALID;
	lane->submode = PHY_SUBMODE_NA;

	connector = ofw_bus_find_child(node, "connector");
	if (connector > 0) {
		rv = regulator_get_by_ofw_property(sc->dev, connector,
		    "phy-supply",&lane->supply_vbus);
		if (rv != 0 && rv != ENOENT) {
			device_printf(sc->dev, "Cannot get vbus supply\n");
			return(rv);
		}
		if (lane->supply_vbus != NULL) {
			rv = regulator_enable(lane->supply_vbus);
			if (rv != 0) {
				device_printf(sc->dev,
				    "Cannot enable vbus supply\n");
				return (rv);
			}
		}
	}

	return (0);
}

static int
 mv_comphy_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Marvell High-Speed Serdes");
	return (BUS_PROBE_DEFAULT);
}

static int
mv_comphy_attach(device_t dev)
{
	struct comphy_softc *sc;
	struct phy_softc *physc;
	struct phynode_init_def phy_init;
	struct phynode *phynode;
	phandle_t node, child;
	pcell_t tmp;
	int rid, rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	PHY_LOCK_INIT(sc);

	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resources\n");
		rv = ENXIO;
		goto fail;
	}
	rv = syscon_get_by_ofw_property(dev, node, "marvell,system-controller",
	    &sc->syscon);
	if (rv != 0 ) {
		device_printf(sc->dev,
		    "Cannot get 'marvell,system-controller' syscon\n");
		rv = ENXIO;
		goto fail;
	}
	rv = clk_get_by_ofw_name(sc->dev, 0, "mg_clk", &sc->mg_clk);
	if (rv != 0 ) {
		device_printf(sc->dev, "Cannot get 'mg_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}
	rv = clk_get_by_ofw_name(sc->dev, 0, "mg_core_clk", &sc->mg_core_clk);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot get 'mg_core_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}
	rv = clk_get_by_ofw_name(sc->dev, 0, "axi_clk", &sc->axi_clk);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot get 'axi_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}

	rv = clk_enable(sc->mg_clk);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot enable 'mg_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}

	rv = clk_enable(sc->mg_core_clk);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot enable 'mg_core_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}

	rv = clk_enable(sc->axi_clk);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot enable 'axi_clk' clock\n");
		rv = ENXIO;
		goto fail;
	}

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		if (OF_getencprop(child, "reg", &tmp, sizeof(tmp)) <= 0) {
			device_printf(dev, "Cannot get phy controller id\n");
			continue;
		}
		if (tmp >= MV_COMPHY_MAX_LANES) {
			device_printf(sc->dev, "invalid 'reg' property\n");
			continue;
		}
		rv = mv_comphy_lane_init(sc, tmp, child);
		if (rv != 0) {
			device_printf(dev, "Cannot initialize phy[%d]\n", tmp);
			rv = ENXIO;
			goto fail;
		}

		for (int j = 0; j < MV_COMPHY_MAX_PORTS; j++) {
			/* 
			 * We use a custom ID mapping because a single phy
			 * driver instance handles multiple phy nodes in DT.
			 */ 
			phy_init.id = (child & 0xFFFFFF) + (j << 24);
			phy_init.ofw_node = child;
			phynode = phynode_create(dev, &mv_comphy_phynode_class,
			    &phy_init);
			if (phynode == NULL) {
				device_printf(dev, "Cannot create phy[%d]\n",
				    tmp);
				rv = ENXIO;
				goto fail;
			}
			physc = phynode_get_softc(phynode);
			physc->lane = tmp;
			physc->port = j;
			if (phynode_register(phynode) == NULL) {
				device_printf(dev, "Cannot register phy[%d]\n",
				    tmp);
				rv = ENXIO;
				goto fail;
			}
		}
	}

	return (0);

fail:
	PHY_LOCK_DESTROY(sc);
	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);
	return (rv);
}

static int
mv_comphy_map(device_t provider, phandle_t xref, int ncells,  pcell_t *cells,
    intptr_t *id)
{

	if (ncells != 1)
		return (ERANGE);
	*id = (OF_node_from_xref(xref) & 0xFFFFFF) + (cells[0] << 24);

	return (0);
}

static int
mv_comphy_detach(device_t dev)
{
	struct comphy_softc *sc;

	sc = device_get_softc(dev);

	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);
	return(bus_generic_detach(dev));
}

static device_method_t mv_comphy_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		mv_comphy_probe),
	DEVMETHOD(device_attach,	mv_comphy_attach),
	DEVMETHOD(device_detach,	mv_comphy_detach),

	DEVMETHOD(phydev_map,		mv_comphy_map),

	DEVMETHOD_END
};

DEFINE_CLASS_0(mv_comphy, mv_comphy_driver, mv_comphy_methods,
    sizeof(struct comphy_softc));

EARLY_DRIVER_MODULE(mv_comphy, simplebus, mv_comphy_driver,
    NULL, NULL, BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_MIDDLE);
