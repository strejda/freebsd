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

#define BRCM_RESCAL_START	0x0
#define  BRCM_RESCAL_START_BIT		(1 << 0)
#define BRCM_RESCAL_CTRL	0x4
#define BRCM_RESCAL_STATUS	0x8
#define  BRCM_RESCAL_STATUS_BIT		(1 << 0)

struct brcm_rescal_softc {
	device_t		dev;
	struct resource		*mem_res;
	struct mtx		mtx;
};

#define	WR4(_sc, _off, _val)	bus_write_4((_sc)->mem_res, _off, _val)
#define	RD4(_sc, _off)		bus_read_4((_sc)->mem_res, _off)

static struct ofw_compat_data compat_data[] = {
	{"brcm,bcm7216-pcie-sata-rescal",	1},
	{NULL,			0}
};

static int
brcm_rescal_assert(device_t dev, intptr_t id, bool value)
{
	struct brcm_rescal_softc *sc;

	uint32_t reg;
	int i, rv;

	sc  = device_get_softc(dev);

	if (id != 1) {
		device_printf(sc->dev, "Invalid reset ID: %jd\n", (uintmax_t)id);
		return (EINVAL);
	}

	rv = 0;

	mtx_lock(&sc->mtx);
	if (value) {
		reg = RD4(sc, BRCM_RESCAL_START);
		reg |= BRCM_RESCAL_START_BIT;
		WR4(sc, BRCM_RESCAL_START, reg);

		for (i = 1000; i > 0; i--) {
			reg = RD4(sc, BRCM_RESCAL_STATUS);
			if (reg & BRCM_RESCAL_STATUS_BIT)
				break;
			DELAY(100);
		}

		if (i <= 0) {
			device_printf(sc->dev, "%s: timeout\n", __func__);
			rv = ETIMEDOUT;
			goto out;
		}

		reg = RD4(sc, BRCM_RESCAL_START);
		reg &= ~BRCM_RESCAL_START_BIT;
		WR4(sc, BRCM_RESCAL_START, reg);
	}

out:
	mtx_unlock(&sc->mtx);
	return (rv);
}

static int
brcm_rescal_is_asserted(device_t dev, intptr_t id, bool *value)
{
	struct brcm_rescal_softc *sc;

	sc  = device_get_softc(dev);

	if (id != 1) {
		device_printf(sc->dev, "Invalid reset ID: %jd\n", (uintmax_t)id);
		return (EINVAL);
	}

	*value= false;
	return (0);
}

static int
brcm_rescal_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "BRCMSTB Reset/Callibration controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_rescal_attach(device_t dev)
{
	struct brcm_rescal_softc *sc;

	sc = device_get_softc(dev);
	sc->dev = dev;
	mtx_init(&sc->mtx, "brcm rescan", NULL, MTX_DEF);

	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0, RF_ACTIVE);
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
brcm_rescal_detach(device_t dev)
{
	struct brcm_rescal_softc *sc;

	sc = device_get_softc(dev);

	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);

	 mtx_destroy(&sc->mtx);
	return (0);
}


static device_method_t brcm_rescal_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		brcm_rescal_probe),
	DEVMETHOD(device_attach,	brcm_rescal_attach),
	DEVMETHOD(device_detach,	brcm_rescal_detach),


	/* Reset interface */
	DEVMETHOD(hwreset_assert,	brcm_rescal_assert),
	DEVMETHOD(hwreset_is_asserted,	brcm_rescal_is_asserted),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(brcmstb_rescal, brcmstb_rescal_driver, brcm_rescal_methods,
    sizeof(struct brcm_rescal_softc));

EARLY_DRIVER_MODULE(brcmstb_rescal, simplebus, brcmstb_rescal_driver, NULL, NULL,
 BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);