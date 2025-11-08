/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021, 2022 Soren Schmidt <sos@deepcore.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/intr.h>
#include <sys/mutex.h>
#include <sys/gpio.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/ofw/ofwpci.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcib_private.h>
#include <dev/pci/pci_dw.h>

#include <dev/clk/clk.h>
#include <dev/phy/phy.h>
#include <dev/regulator/regulator.h>
#include <dev/hwreset/hwreset.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include "pcib_if.h"

/* APB Registers */
#define	PCIE_CLIENT_GENERAL_CON		0x0000
#define	 DEVICE_TYPE_MASK		0x00f0
#define	 DEVICE_TYPE_RC			(1<<6)
#define	 LINK_REQ_RST_GRT		(1<<3)
#define	 LTSSM_ENABLE			(1<<2)
#define PCIE_CLIENT_INTR_STATUS_MISC	0x10
#define  PCIE_LINK_REQ_RST_NOT_INT	(1<<2)
#define  PCIE_RDLH_LINK_UP_CHGED	(1<<1)

#define	PCIE_CLIENT_INTR_MASK_MSG_RX	0x0018
#define	PCIE_CLIENT_INTR_MASK_LEGACY	0x001c
#define	PCIE_CLIENT_INTR_MASK_ERR	0x0020
#define	PCIE_CLIENT_INTR_MASK_MISC	0x0024
#define	PCIE_CLIENT_INTR_MASK_PMC	0x0028
#define	PCIE_CLIENT_GENERAL_DEBUG_INFO	0x0104
#define	PCIE_CLIENT_HOT_RESET_CTRL	0x0180
#define	 APP_LSSTM_ENABLE_ENHANCE	(1<<4)
#define	PCIE_CLIENT_LTSSM_STATUS	0x0300
#define	 RDLH_LINK_UP			(1<<17)
#define	 SMLH_LINK_UP			(1<<16)
#define	 SMLH_LTSSM_STATE_MASK		0x003f
#define	 SMLH_LTSSM_STATE_LINK_UP	((1<<4) | (1<<0))

struct rk3568_pcie_softc {
	struct pci_dw_softc		dw_sc;  /* Must be first */
	device_t			dev;
	int				apb_rid;
	struct resource			*apb_res;
	int				dbi_rid;
	struct resource			*dbi_res;
	int				sys_irq_rid;
	struct resource			*sys_irq_res;
	void				*sys_irq_handle;
	phandle_t			node;
	clk_t				aclk_mst, aclk_slv, aclk_dbi, pclk, aux, pipe;
	regulator_t			regulator;
	hwreset_t			rst_pipe;
	hwreset_t			rst_power;
	struct gpiobus_pin		*rst_gpio;
	phy_t				phy;
};

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk3568-pcie",	1},
	{NULL,				0}
};



static int
rk3568_pcie_get_link(device_t dev, bool *status)
{
	struct rk3568_pcie_softc *sc = device_get_softc(dev);
	uint32_t val;

	val = bus_read_4(sc->apb_res, PCIE_CLIENT_LTSSM_STATUS);
	if (((val & (RDLH_LINK_UP | SMLH_LINK_UP)) ==
	    (RDLH_LINK_UP | SMLH_LINK_UP)) &&
	    ((val & SMLH_LTSSM_STATE_MASK) == SMLH_LTSSM_STATE_LINK_UP))
		*status = true;
	else
		*status = false;
	return (0);
}


static void
rk3568_sys_intr(void *data)
{
	struct rk3568_pcie_softc *sc = data;
	bool status;
	int val;

	val = bus_read_4(sc->apb_res, PCIE_CLIENT_INTR_STATUS_MISC),
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_STATUS_MISC, val);

	if (val & PCIE_RDLH_LINK_UP_CHGED) {
		rk3568_pcie_get_link(sc->dev, &status);
		if (status)
			BUS_RESCAN(sc->dev);
	}
}

static int
rk3568_pcie_init_soc(device_t dev)
{
	struct rk3568_pcie_softc *sc = device_get_softc(dev);
	int err, count;
	bool status;

	/* Assert PCIe reset */
	if (sc->rst_gpio != NULL) {
		err = gpio_pin_setflags(sc->rst_gpio, GPIO_PIN_OUTPUT);
		if (err != 0) {
			device_printf(dev, "Could not setup PCIe reset: %d\n",
			    err);
			return (ENXIO);
		}
		err = gpio_pin_set_active(sc->rst_gpio, true);
		if (err != 0) {
			device_printf(dev, "Could not set PCIe reset: %d\n",
			    err);
			return (ENXIO);
		}
	}

	/* Assert reset */
	err = hwreset_assert(sc->rst_pipe);
	if (err != 0) {
		device_printf(dev, "Could not assert 'pipe' reset: %d\n", err);
		return (ENXIO);
	}
	if (sc->rst_power != NULL) {
		err = hwreset_assert(sc->rst_power);
		if (err != 0) {
			device_printf(dev,
			    "Could not assert 'power' reset: %d\n", err);
			return (ENXIO);
		}
	}

	/* Powerup PCIe */
	if (sc->regulator != NULL) {
		err = regulator_enable(sc->regulator);
		if (err != 0) {
			device_printf(dev, "Cannot enable regulator: %d\n",
			    err);
			return (ENXIO);
		}
	}

	/* Enable PHY */
	if (phy_enable(sc->phy)) {
		device_printf(dev, "Cannot enable phy\n");
		return (ENXIO);
	}


	/* Enable clocks */
	err = clk_enable(sc->aclk_mst);
	if (err != 0) {
		device_printf(dev, "Could not enable 'aclk_mst' clk: %d\n",
		    err);
		return (ENXIO);
	}
	err = clk_enable(sc->aclk_slv);
	if (err != 0) {
		device_printf(dev, "Could not enable 'aclk_slv' clk: %d\n",
		    err);
		return (ENXIO);
	}
	err = clk_enable(sc->aclk_dbi);
	if (err != 0) {
		device_printf(dev, "Could not enable 'aclk_dbi' clk; %d\n",
		    err);
		return (ENXIO);
	}
	err = clk_enable(sc->pclk);
	if (err != 0) {
		device_printf(dev, "Could not enable 'pclk' clk: %d\n", err);
		return (ENXIO);
	}
	err = clk_enable(sc->aux);
	if (err != 0) {
		device_printf(dev, "Could not enable 'aux' clk: %d\n", err);
		return (ENXIO);
	}
	if (sc->pipe != NULL) {
		err = clk_enable(sc->pipe);
		if (err != 0) {
			device_printf(dev, "Could not enable 'pipe' clk: %d\n",
			    err);
			return (ENXIO);
		}
	}

	/* Deassert reset */
	err = hwreset_deassert(sc->rst_pipe);
	if (err != 0) {
		device_printf(dev, "Could not deassert 'pipe' reset: %d\n",
		    err);
		return (ENXIO);
	}

	if (sc->rst_power != NULL) {
		err = hwreset_deassert(sc->rst_power);
		if (err != 0) {
			device_printf(dev,
			    "Could not deassert 'power' reset: %d\n", err);
			return (ENXIO);
		}
	}

	/* Set Root Complex (RC) mode */
	bus_write_4(sc->apb_res, PCIE_CLIENT_HOT_RESET_CTRL,
	    (APP_LSSTM_ENABLE_ENHANCE << 16) | APP_LSSTM_ENABLE_ENHANCE);
	bus_write_4(sc->apb_res, PCIE_CLIENT_GENERAL_CON,
	    (DEVICE_TYPE_MASK << 16) | DEVICE_TYPE_RC);

	/* Deassert PCIe bus  reset */
	err = gpio_pin_set_active(sc->rst_gpio, false);
	if (err != 0) {
		device_printf(dev, "'reset_gpio' set failed: %d\n", err);
		return (ENXIO);
	}

	/* Start Link Training and Status State Machine (LTSSM) */
	bus_write_4(sc->apb_res, PCIE_CLIENT_GENERAL_CON,
	    (LINK_REQ_RST_GRT | LTSSM_ENABLE) << 16 |
	    (LINK_REQ_RST_GRT | LTSSM_ENABLE));
	DELAY(100000);

	/* Release PCIe reset */
	if (sc->rst_gpio != NULL) {
		err = gpio_pin_set_active(sc->rst_gpio, true);
		if (err != 0) {
			device_printf(dev, "Could not release PCIe reset: %d",
			    err);
			return (ENXIO);
		}
	}

	/* Wait for link up/stable */
	for (count = 20; count; count--) {
		rk3568_pcie_get_link(dev, &status);
		if (status)
			break;
		DELAY(100000);
	}

	if (count <= 0)
		device_printf(dev, "Link up timeout, continuing...\n");

	err = pci_dw_init(dev);
	if (err != 0)
		return (ENXIO);

	/* Delay to have things settle */
	DELAY(100000);

#if 0
	/* Enable all MSG interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_MSG_RX, 0x7fff0000);

	/* Enable all Legacy interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_LEGACY, 0x00ff0000);

	/* Enable all Error interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_ERR, 0x0fff0000);
#endif

	/* Enable link status interrupt */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_MISC,
	    PCIE_RDLH_LINK_UP_CHGED << 16 | PCIE_RDLH_LINK_UP_CHGED);
	return (0);
}

static int
rk3568_pcie_detach(device_t dev)
{
	struct rk3568_pcie_softc *sc = device_get_softc(dev);

	/* Release allocated resources */
	if (sc->sys_irq_handle)
		bus_teardown_intr(dev, sc->sys_irq_res, sc->sys_irq_handle);
	if (sc->phy)
		phy_release(sc->phy);
	if (sc->aux)
		clk_release(sc->aux);
	if (sc->pclk)
		clk_release(sc->pclk);
	if (sc->aclk_dbi)
		clk_release(sc->aclk_dbi);
	if (sc->aclk_slv)
		clk_release(sc->aclk_slv);
	if (sc->aclk_mst)
		clk_release(sc->aclk_mst);
	if (sc->rst_power)
		hwreset_release(sc->rst_power);
	if (sc->rst_pipe)
		hwreset_release(sc->rst_pipe);
	if (sc->regulator)
		regulator_release(sc->regulator);
	if (sc->sys_irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, sc->sys_irq_rid,
		    sc->sys_irq_res);
	if (sc->dbi_res)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->dbi_rid,
		    sc->dbi_res);
	if (sc->apb_res)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->apb_rid,
		    sc->apb_res);
	return (0);
}

static int
rk3568_pcie_attach(device_t dev)
{
	struct rk3568_pcie_softc *sc = device_get_softc(dev);
	int err;

	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);

	/* Setup resources */
	err = ofw_bus_find_string_index(sc->node, "reg-names", "apb",
	    &sc->apb_rid);
	if (err != 0) {
		device_printf(dev, "Cannot get APB memory: %d\n", err);
		goto fail;
	}

	sc->apb_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->apb_rid, RF_ACTIVE);
	if (sc->apb_res == NULL) {
		device_printf(dev, "Cannot allocate APB resource\n");
		goto fail;
	}

	err = ofw_bus_find_string_index(sc->node, "reg-names", "dbi",
	    &sc->dbi_rid);
	if (err != 0) {
		device_printf(dev, "Cannot get DBI memory: %d\n", err);
		goto fail;
	}

	err = ofw_bus_find_string_index(sc->node, "reg-names", "dbi",
	    &sc->dbi_rid);
	if (err != 0) {
		device_printf(dev, "Cannot get DBI memory: %d\n", err);
		goto fail;
	}

	sc->dw_sc.dbi_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->dbi_rid, RF_ACTIVE);
	if (sc->dw_sc.dbi_res == NULL) {
		device_printf(dev, "Cannot allocate DBI resource\n");
		goto fail;
	}

	err = ofw_bus_find_string_index(sc->node, "interrupt-names", "sys",
	    &sc->sys_irq_rid);
	if (err != 0) {
		device_printf(dev, "Cannot get SYS interrupt %d\n", err);
		goto fail;
	}
	sc->sys_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
	    &sc->sys_irq_rid, RF_ACTIVE | RF_SHAREABLE);
	 if (sc->sys_irq_res == NULL) {
		device_printf(dev, "Cannot allocate SYS IRQ resource\n");
		goto fail;
	}

	/* Get regulator if present */
	err = regulator_get_by_ofw_property(dev, 0, "vpcie3v3-supply",
	    &sc->regulator);
	if (err != 0 && err != ENOENT) {
		device_printf(dev, "Cannot get regulator: %d\n", err);
		goto fail;
	}

	/* Get reset */
	err = hwreset_get_by_ofw_name(dev, 0, "pipe", &sc->rst_pipe);
	if (err != 0) {
		device_printf(dev, "Cannot get reset 'pipe': %d\n", err);
		goto fail;
	}
	err = hwreset_get_by_ofw_name(dev, 0, "pwr", &sc->rst_power);
	if (err != 0 && err != ENOENT) {
		device_printf(dev, "Cannot get reset 'pwr': %d\n", err);
		goto fail;
	}

	/* Get GPIO reset */
	err = gpio_pin_get_by_ofw_property(dev, sc->node, "reset-gpios",
		    &sc->rst_gpio);
	if (err != 0 && err != ENOENT) {
		device_printf(dev, "Cannot get reset-gpios: %d\n", err);
		goto fail;
	}

	/* Get clocks */
	err = clk_get_by_ofw_name(dev, 0, "aclk_mst", &sc->aclk_mst);
	if (err != 0) {
		device_printf(dev, "Cannot get 'aclk_mst' clk: %d\n", err);
		goto fail;
	}
	err = clk_get_by_ofw_name(dev, 0, "aclk_slv", &sc->aclk_slv);
	if (err != 0) {
		device_printf(dev, "Cannot get 'aclk_slv' clk: %d\n", err);
		goto fail;
	}
	err = clk_get_by_ofw_name(dev, 0, "aclk_dbi", &sc->aclk_dbi);
	if (err != 0) {
		device_printf(dev, "Cannot get 'aclk_dbi' clk: %d\n", err);
		goto fail;
	}
	err = clk_get_by_ofw_name(dev, 0, "pclk", &sc->pclk);
	if (err != 0) {
		device_printf(dev, "Cannot get 'pclk' clk: %d\n", err);
		goto fail;
	}
	err = clk_get_by_ofw_name(dev, 0, "aux", &sc->aux);
	if (err != 0) {
		device_printf(dev, "Cannot get 'aux' clk: %d\n", err);
		goto fail;
	}
	err = clk_get_by_ofw_name(dev, 0, "pipe", &sc->pipe);
	if (err != 0 && err != ENOENT) {
		device_printf(dev, "Cannot get 'pipe' clk: %d\n", err);
		goto fail;
	}

	/* Get PHY */
	err = phy_get_by_ofw_name(dev, 0, "pcie-phy", &sc->phy);
	if (err != 0) {
		device_printf(dev, "Cannot get 'pcie-phy' phy: %d\n", err);
		goto fail;
	}

	err = rk3568_pcie_init_soc(dev);
	if (err != 0)
		goto fail;

	/* Enable SYS interrupt */
	err = bus_setup_intr(dev, sc->sys_irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, rk3568_sys_intr, sc, &sc->sys_irq_handle);
	if (err != 0) {
		device_printf(dev, "Unable to setup interrupt: %d\n", err);
		goto fail;
	}

	bus_attach_children(dev);
	return (0);
fail:
	rk3568_pcie_detach(dev);
	return (ENXIO);
}

static int
rk3568_pcie_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data)
		return (ENXIO);
	device_set_desc(dev, "RockChip RK3568 PCI-express controller");
	return (BUS_PROBE_DEFAULT);
}

static device_method_t rk3568_pcie_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk3568_pcie_probe),
	DEVMETHOD(device_attach,	rk3568_pcie_attach),
	DEVMETHOD(device_detach,	rk3568_pcie_detach),

	/* PCI DW interface */
	DEVMETHOD(pci_dw_get_link,	rk3568_pcie_get_link),

	DEVMETHOD_END
};

DEFINE_CLASS_1(pcib, rk3568_pcie_driver, rk3568_pcie_methods,
    sizeof(struct rk3568_pcie_softc), pci_dw_driver);
DRIVER_MODULE(rk3568_pcie, simplebus, rk3568_pcie_driver, NULL, NULL);
