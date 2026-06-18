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
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/hwreset/hwreset.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "hwreset_if.h"


#define BRCM_RESET_SET		0x00
#define BRCM_RESET_CLEAR	0x04
#define BRCM_RESET_STATUS	0x08

#define BRCM_RESET_BANK_BITS	0x20
#define BRCM_RESET_BANK_SIZE	0x18

#define BRCM_RESET_MASK(id)	(1 << ((id) % BRCM_RESET_BANK_BITS))
#define BRCM_RESET_BANK(id)	((id) / BRCM_RESET_BANK_BITS)
#define BRCM_RESET_REG(id)	(BRCM_RESET_BANK(id) * BRCM_RESET_BANK_SIZE)

struct brcm_reset_softc {
	device_t		dev;
	struct resource		*mem_res;
	struct mtx		mtx;
};

#define	WR4(_sc, _off, _val)	bus_write_4((_sc)->mem_res, _off, _val)
#define	RD4(_sc, _off)		bus_read_4((_sc)->mem_res, _off)

static struct ofw_compat_data compat_data[] = {
	{"brcm,brcmstb-reset",	1},
	{NULL,			0}
};

static int
brcm_reset_assert(device_t dev, intptr_t id, bool value)
{
	struct brcm_reset_softc *sc;

	sc  = device_get_softc(dev);

	if (value)
		WR4(sc, BRCM_RESET_REG(id) + BRCM_RESET_SET,
		    BRCM_RESET_MASK(id));
	else
		WR4(sc, BRCM_RESET_REG(id) + BRCM_RESET_CLEAR,
		    BRCM_RESET_MASK(id));
	DELAY(100);
	return (0);
}

static int
brcm_reset_is_asserted(device_t dev, intptr_t id, bool *value)
{
	struct brcm_reset_softc *sc;
	uint32_t reg;

	sc  = device_get_softc(dev);

	reg = RD4(sc, BRCM_RESET_REG(id) + BRCM_RESET_STATUS);
	*value = reg & BRCM_RESET_MASK(id) ? true: false;

	return (0);
}

static int
brcm_reset_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "BRCMSTB Reset controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_reset_attach(device_t dev)
{
	struct brcm_reset_softc *sc;
	int rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	mtx_init(&sc->mtx, "brcm rescan", NULL, MTX_DEF);

	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		goto fail;
	}

	hwreset_register_ofw_provider(dev);

	return (0);

fail:

	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);

	return (ENXIO);
}

static int
brcm_reset_detach(device_t dev)
{

	struct brcm_reset_softc *sc;

	sc = device_get_softc(dev);

	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);

	 mtx_destroy(&sc->mtx);
	return (EBUSY);
}


static device_method_t brcm_reset_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		brcm_reset_probe),
	DEVMETHOD(device_attach,	brcm_reset_attach),
	DEVMETHOD(device_detach,	brcm_reset_detach),


	/* Reset interface */
	DEVMETHOD(hwreset_assert,	brcm_reset_assert),
	DEVMETHOD(hwreset_is_asserted,	brcm_reset_is_asserted),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(brcmstb_reset, brcmstb_reset_driver, brcm_reset_methods,
    sizeof(struct brcm_reset_softc));

EARLY_DRIVER_MODULE(brcmstb_reset, simplebus, brcmstb_reset_driver, NULL, NULL,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);