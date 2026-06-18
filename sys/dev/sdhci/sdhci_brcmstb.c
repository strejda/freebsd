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
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/intr.h>

#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcbrvar.h>
#include <dev/mmc/mmc_fdt_helpers.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/sdhci/sdhci.h>
#include <dev/sdhci/sdhci_fdt_gpio.h>

#include "sdhci_if.h"

#include "opt_mmccam.h"

/* Compatible devices. */
static struct ofw_compat_data compat_data[] = {
	{ "brcm,bcm2712-sdhci",		1},
	{NULL,				0},
};

struct brcm_sdhci_softc {
	device_t		dev;
	int			quirks;
	struct resource *	host_mem_res;
	struct resource *	cfg_mem_res;
	struct resource *	irq_res;
	void *			intr_cookie;
	uint32_t		max_clk; /* Max possible freq */
	clk_t			sdio_clk;
	clk_t			sdio_freq;

	bool			force_card_present;
	uint32_t		caps;

	struct sdhci_slot	slot;
	struct mmc_helper	fdt_helper;

};

static inline uint32_t
RD4(struct brcm_sdhci_softc *sc, bus_size_t off)
{

	return (bus_read_4(sc->host_mem_res, off));
}

static inline uint16_t
RD2(struct brcm_sdhci_softc *sc, bus_size_t off)
{

	return (bus_read_2(sc->host_mem_res, off));
}

static inline void
WR2(struct brcm_sdhci_softc *sc, bus_size_t off, uint16_t val)
{

	bus_write_2(sc->host_mem_res, off, val);
}

static uint8_t
brcm_sdhci_read_1(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	return (bus_read_1(sc->host_mem_res, off));
}

static uint16_t
brcm_sdhci_read_2(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	return (bus_read_2(sc->host_mem_res, off));
}

static uint32_t
brcm_sdhci_read_4(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct brcm_sdhci_softc *sc;
	uint32_t val32;

	sc = device_get_softc(dev);
	val32 = bus_read_4(sc->host_mem_res, off);
	/* Force the card-present state if necessary. */
	if (off == SDHCI_PRESENT_STATE && sc->force_card_present)
		val32 |= SDHCI_CARD_PRESENT;
	return (val32);
}

static void
brcm_sdhci_read_multi_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t *data, bus_size_t count)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	bus_read_multi_4(sc->host_mem_res, off, data, count);
}

static void
brcm_sdhci_write_1(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint8_t val)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	bus_write_1(sc->host_mem_res, off, val);
}

static void
brcm_sdhci_write_2(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint16_t val)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	bus_write_2(sc->host_mem_res, off, val);
}

static void
brcm_sdhci_write_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t val)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	bus_write_4(sc->host_mem_res, off, val);
}

static void
brcm_sdhci_write_multi_4(device_t dev, struct sdhci_slot *slot, bus_size_t off,
    uint32_t *data, bus_size_t count)
{
	struct brcm_sdhci_softc *sc;

	sc = device_get_softc(dev);
	bus_write_multi_4(sc->host_mem_res, off, data, count);
}

static void
brcm_sdhci_intr(void *arg)
{
	struct brcm_sdhci_softc *sc = arg;

	sdhci_generic_intr(&sc->slot);
	RD4(sc, SDHCI_INT_STATUS);
}

static int
brcm_sdhci_probe(device_t dev)
{
	const struct ofw_compat_data *cd;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	cd = ofw_bus_search_compatible(dev, compat_data);
	if (cd->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Broadcom SDHCI controller");

	return (BUS_PROBE_DEFAULT);
}

static int
brcm_sdhci_attach(device_t dev)
{
	struct brcm_sdhci_softc *sc;
	int rid, rv;
	phandle_t node, prop;
	pcell_t cid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	/* Allow dts to patch quirks, slots, and max-frequency. */
	if ((OF_getencprop(node, "quirks", &cid, sizeof(cid))) > 0)
		sc->quirks = cid;

	if ((OF_getencprop(node, "clock-frequency", &cid, sizeof(cid))) > 0)
		sc->max_clk = cid;

	rv = ofw_bus_find_string_index(node, "reg-names", "host", &rid);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'host' memory: %d\n", rv);
		rv = ENXIO;
		goto fail;
	}
	sc->host_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->host_mem_res == NULL) {
		device_printf(dev, "Cannot allocate 'host' memory\n");
		rv = ENXIO;
		goto fail;
	}

	rv = ofw_bus_find_string_index(node, "reg-names", "cfg", &rid);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'cfg' memory: %d\n", rv);
		rv = ENXIO;
		goto fail;
	}
	sc->cfg_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->cfg_mem_res== NULL) {
		device_printf(dev, "Cannot allocate 'cfg' memory %d\n", rid);
		rv = ENXIO;
		goto fail;
	}

	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "Cannot allocate interrupt\n");
		rv = ENXIO;
		goto fail;
	}

	if (OF_hasprop(node, "assigned-clocks")) {
		rv = clk_set_assigned(sc->dev, node);
		if (rv != 0) {
			device_printf(dev, "Cannot set assigned clocks: %d\n",
			    rv);
			goto fail;
		}
	}

	rv = clk_get_by_ofw_name(dev, 0, "sw_sdio", &sc->sdio_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'sw_sdio' clock: %d\n", rv);
		goto fail;
	}
	rv = clk_enable(sc->sdio_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot enable 'sw_sdio' clock: %d\n", rv);
		goto fail;
	}

	rv = clk_get_by_ofw_name(dev, 0, "sdio_freq", &sc->sdio_freq);
	if (rv != 0 && rv != ENOENT) {
		if (rv != ENOENT) {
			device_printf(dev,
			    "Cannot get 'sdio_freq' clock: %d\n", rv);
			goto fail;
		}

		rv = clk_enable(sc->sdio_freq);
		if (rv != 0) {
			device_printf(dev,
			    "Cannot enable 'sdio_freq' clock: %d\n", rv);
			goto fail;
		}
	}

	/* Fill slot information. */
	sc->quirks |= SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
	    SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
	    SDHCI_QUIRK_MISSING_CAPS;

	/* Limit real slot capabilities. */
	sc->caps = RD4(sc, SDHCI_CAPABILITIES);

	if (OF_getencprop(node, "bus-width", &prop, sizeof(prop)) > 0) {
		sc->caps &= ~(MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA);
		switch (prop) {
		case 8:
			sc->caps |= MMC_CAP_8_BIT_DATA;
			/* FALLTHROUGH */
		case 4:
			sc->caps |= MMC_CAP_4_BIT_DATA;
			break;
		case 1:
			break;
		default:
			device_printf(dev, "Bad bus-width value %u\n", prop);
			break;
		}
	}

	/*
	 * Clear clock field, so SDHCI driver uses supplied frequency.
	 * in sc->slot.max_clk
	 */

	sc->slot.quirks = sc->quirks;
//	sc->slot.max_clk = sc->max_clk;
	sc->slot.caps = sc->caps;

	if (bus_setup_intr(dev, sc->irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, brcm_sdhci_intr, sc, &sc->intr_cookie)) {
		device_printf(dev, "cannot setup interrupt handler\n");
		rv = ENXIO;
		goto fail;
	}
	rv = sdhci_init_slot(dev, &sc->slot, 0);
	if (rv != 0) {
		goto fail;
	}


	bus_identify_children(dev);
	bus_attach_children(dev);

	sdhci_start_slot(&sc->slot);

	return (0);

fail:
	if (sc->intr_cookie != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->intr_cookie);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->host_mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->host_mem_res);
	if (sc->cfg_mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->cfg_mem_res);

	return (rv);
}

static int
brcm_sdhci_detach(device_t dev)
{
	struct brcm_sdhci_softc *sc = device_get_softc(dev);
	struct sdhci_slot *slot = &sc->slot;
	int error;

	error = bus_detach_children(dev);
	if (error != 0)
		return (error);

	bus_teardown_intr(dev, sc->irq_res, sc->intr_cookie);
	bus_release_resource(dev, SYS_RES_IRQ, rman_get_rid(sc->irq_res),
			     sc->irq_res);

	sdhci_cleanup_slot(slot);
	bus_release_resource(dev, SYS_RES_MEMORY,
			     rman_get_rid(sc->host_mem_res),
			     sc->host_mem_res);
	return (0);
}

static device_method_t brcm_sdhci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		brcm_sdhci_probe),
	DEVMETHOD(device_attach,	brcm_sdhci_attach),
	DEVMETHOD(device_detach,	brcm_sdhci_detach),

	/* Bus interface */
	DEVMETHOD(bus_read_ivar,	sdhci_generic_read_ivar),
	DEVMETHOD(bus_write_ivar,	sdhci_generic_write_ivar),

	/* MMC bridge interface */
	DEVMETHOD(mmcbr_update_ios,	sdhci_generic_update_ios),
	DEVMETHOD(mmcbr_request,	sdhci_generic_request),
	DEVMETHOD(mmcbr_get_ro,		sdhci_generic_get_ro),
	DEVMETHOD(mmcbr_acquire_host,	sdhci_generic_acquire_host),
	DEVMETHOD(mmcbr_release_host,	sdhci_generic_release_host),

	/* SDHCI registers accessors */
	DEVMETHOD(sdhci_read_1,		brcm_sdhci_read_1),
	DEVMETHOD(sdhci_read_2,		brcm_sdhci_read_2),
	DEVMETHOD(sdhci_read_4,		brcm_sdhci_read_4),
	DEVMETHOD(sdhci_read_multi_4,	brcm_sdhci_read_multi_4),
	DEVMETHOD(sdhci_write_1,	brcm_sdhci_write_1),
	DEVMETHOD(sdhci_write_2,	brcm_sdhci_write_2),
	DEVMETHOD(sdhci_write_4,	brcm_sdhci_write_4),
	DEVMETHOD(sdhci_write_multi_4,	brcm_sdhci_write_multi_4),
	DEVMETHOD(sdhci_get_card_present,sdhci_generic_get_card_present),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(sdhci, brcm_sdhci_driver, brcm_sdhci_methods,
    sizeof(struct brcm_sdhci_softc));
DRIVER_MODULE(sdhci_brmstbf, simplebus, brcm_sdhci_driver, NULL, NULL);
SDHCI_DEPEND(sdhci_brmstb);
#ifndef MMCCAM
MMC_DECLARE_BRIDGE(sdhci);
#endif
