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
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <machine/bus.h>
#include <machine/intr.h>
#include <machine/resource.h>

#include <dev/fdt/simplebus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "pic_if.h"

#define	BRCM_L2INTC_MAX_NIRQS	32


struct brcm_l2intc_soc {
	int			status_reg;
	int			status_clr_reg;
	int			mask_reg;
	int			mask_set_reg;
	int			mask_clr_reg;
};


struct brcm_l2intc_irqsrc {
	struct intr_irqsrc	isrc;
	u_int			irq;
};

struct brcm_l2intc_softc {
	device_t		dev;
	struct resource		*mem_res;
	struct resource		*irq_res;
	void			*irq_ih;
	struct brcm_l2intc_soc  *soc;

	struct brcm_l2intc_irqsrc *isrcs;
};

struct brcm_l2intc_soc brcm_l2intc_edge_soc = {
	.status_reg = 0x00,
	.status_clr_reg = 0x08,
	.mask_reg = 0x0C,
	.mask_set_reg = 0x10,
	.mask_clr_reg = 0x14,
};

struct brcm_l2intc_soc brcm_l2intc_level_soc = {
	.status_reg = 0x00,
	.status_clr_reg = -1,
	.mask_reg = 0x04,
	.mask_set_reg = 0x08,
	.mask_clr_reg = 0x0C,
};

static struct ofw_compat_data compat_data[] = {

	{"brcm,l2-intc", 		(uintptr_t)&brcm_l2intc_edge_soc},
	{"brcm,hif-spi-l2-intc", 	(uintptr_t)&brcm_l2intc_edge_soc},
	{"brcm,upg-aux-aon-l2-intc", 	(uintptr_t)&brcm_l2intc_edge_soc},
	{"brcm,bcm7271-l2-intc",	(uintptr_t)&brcm_l2intc_level_soc},

	{NULL,				0}
};

#define	RD4(sc, reg)		bus_read_4((sc)->mem_res, (reg))
#define	WR4(sc, reg, val)	bus_write_4((sc)->mem_res, (reg), (val))

static void
brcm_l2intc_isrc_eoi(struct brcm_l2intc_softc *sc,
    struct brcm_l2intc_irqsrc *l2isrc)
{

	if (sc->soc->status_clr_reg != -1)
		WR4(sc, sc->soc->status_clr_reg, 1U << l2isrc->irq);
}

static void
brcm_l2intc_enable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;
	WR4(sc, sc->soc->mask_clr_reg, 1U << l2isrc->irq);
}

static void
brcm_l2intc_disable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;
	WR4(sc, sc->soc->mask_set_reg, 1U << l2isrc->irq);
}

static int
brcm_l2intc_map(device_t dev, struct intr_map_data *data, u_int *irqp)
{
	struct intr_map_data_fdt *daf;
	u_int irq;

	if (data->type != INTR_MAP_DATA_FDT)
		return (ENOTSUP);

	daf = (struct intr_map_data_fdt *)data;
	if (daf->ncells != 1)
		return (EINVAL);

	if (daf->cells[0] >= BRCM_L2INTC_MAX_NIRQS)
		return (EINVAL);

	irq = daf->cells[0];


	if (irqp != NULL)
		*irqp = irq;

	return(0);
}

static int
brcm_l2intc_map_intr(device_t dev, struct intr_map_data *data,
    struct intr_irqsrc **isrcp)
{
	struct brcm_l2intc_softc *sc;
	u_int irq;
	int rv;

	sc = device_get_softc(dev);
	rv = brcm_l2intc_map(dev, data, &irq);
	if (rv == 0)
		*isrcp = &sc->isrcs[irq].isrc;

	return (rv);
}

static int
brcm_l2intc_setup_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;
	u_int irq;
	int rv;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;
	if (data == NULL)
		return (ENOTSUP);

	rv = brcm_l2intc_map(dev, data, &irq);
	if (rv != 0)
		return (rv);
	if (irq != l2isrc->irq)
		return (EINVAL);
	WR4(sc, sc->soc->mask_clr_reg, 1U << l2isrc->irq);
	return (0);
}

static int
brcm_l2intc_teardown_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;

	WR4(sc, sc->soc->mask_set_reg, 1U << l2isrc->irq);
	return (0);
}

static void
brcm_l2intc_pre_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;

	WR4(sc, sc->soc->mask_set_reg, 1U << l2isrc->irq);
	brcm_l2intc_isrc_eoi(sc, l2isrc);
}

static void
brcm_l2intc_post_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;

	WR4(sc, sc->soc->mask_set_reg, 1U << l2isrc->irq);
}

static void
brcm_l2intc_post_filter(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;

	sc = device_get_softc(dev);
	l2isrc = (struct brcm_l2intc_irqsrc *)isrc;

	WR4(sc, sc->soc->mask_set_reg, 1U << l2isrc->irq);
	brcm_l2intc_isrc_eoi(sc, l2isrc);
}

/* ----------------------------------------------------------------------------
 *
 *		B u s    i n t e r f a c e
 */
static int
brcm_l2intc_intr(void *arg)
{
	struct brcm_l2intc_softc *sc;
	struct brcm_l2intc_irqsrc *l2isrc;
	struct trapframe *tf;
	uint32_t status, mask;
	u_int irq;

	sc = (struct brcm_l2intc_softc *)arg;
	tf = curthread->td_intr_frame;
	while (true) {
		status = RD4(sc, sc->soc->status_reg);
		mask = RD4(sc, sc->soc->mask_reg);
		status &= mask;

		irq = ffsll(status);
		if (irq == 0) break;
		irq--;
		l2isrc = &sc->isrcs[irq];
		if (intr_isrc_dispatch(&l2isrc->isrc, tf) != 0) {
			WR4(sc, sc->soc->mask_clr_reg, 1U << l2isrc->irq);
			brcm_l2intc_isrc_eoi(sc, l2isrc);
			device_printf(sc->dev,
			    "Stray irq %u disabled\n", irq);
		}
	}

	return (FILTER_HANDLED);
}

static int
brcm_l2intc_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Broadcom Level2 interrupt controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_l2intc_attach(device_t dev)
{
	struct brcm_l2intc_softc *sc;
	phandle_t xref, node;
	uint32_t irq;
	const char *name;
	int rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	sc->soc = (struct brcm_l2intc_soc  *)
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data;

	/* Allocate resources. */
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0, RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		goto fail;
	}

	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, 0, RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "Cannot allocate IRQ resource\n");
		goto fail;
	}

	/* Mask all interrupts) */
	WR4(sc, sc->soc->mask_set_reg,  0xFFFFFFFF);
	if (sc->soc->status_clr_reg != -1)
		WR4(sc, sc->soc->status_clr_reg,  0xFFFFFFFF);

	/* Create all interrupt sources */
	sc->isrcs = malloc(sizeof(*sc->isrcs) * BRCM_L2INTC_MAX_NIRQS,
	    M_DEVBUF, M_WAITOK | M_ZERO);
	name = device_get_nameunit(sc->dev);
	for (irq = 0; irq < BRCM_L2INTC_MAX_NIRQS; irq++) {
		sc->isrcs[irq].irq = irq;
		rv = intr_isrc_register(&sc->isrcs[irq].isrc,
		    sc->dev, 0, "%s,%u", name, irq);
		if (rv != 0) {
			device_printf(dev, "Cannot register irqs: %d\n", rv);
			goto fail; /* XXX deregister ISRCs */
		}
	}
	xref = OF_xref_from_node(node);
	if (intr_pic_register(dev, xref) == NULL) {
		device_printf(dev, "Cannot register interrup controller\n");
		goto fail;
	}
	rv = bus_setup_intr(dev, sc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    brcm_l2intc_intr, NULL, sc, &sc->irq_ih);
	if (rv != 0) {
		device_printf(dev, "Cannot register interrupt handler: %d\n",
		    rv);
		goto fail;
	}

	OF_device_register_xref(xref, dev);
	return (0);

fail:
	if (sc->irq_ih != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_ih);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);
	return (ENXIO);
}

static int
brcm_l2intc_detach(device_t dev)
{

	return (EBUSY);
}



static device_method_t brcm_l2intc_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		brcm_l2intc_probe),
	DEVMETHOD(device_attach,	brcm_l2intc_attach),
	DEVMETHOD(device_detach,	brcm_l2intc_detach),

	/* Interrupt controller interface */
	DEVMETHOD(pic_disable_intr,	brcm_l2intc_disable_intr),
	DEVMETHOD(pic_enable_intr,	brcm_l2intc_enable_intr),
	DEVMETHOD(pic_map_intr,		brcm_l2intc_map_intr),
	DEVMETHOD(pic_setup_intr,	brcm_l2intc_setup_intr),
	DEVMETHOD(pic_teardown_intr,	brcm_l2intc_teardown_intr),
	DEVMETHOD(pic_post_filter,	brcm_l2intc_post_filter),
	DEVMETHOD(pic_post_ithread,	brcm_l2intc_post_ithread),
	DEVMETHOD(pic_pre_ithread,	brcm_l2intc_pre_ithread),



	DEVMETHOD_END
};

DEFINE_CLASS_0(brcm_l2intc, brcmstb_l2intc_driver, brcm_l2intc_methods,
    sizeof(struct brcm_l2intc_softc));


EARLY_DRIVER_MODULE(brcm_l2intc, simplebus, brcmstb_l2intc_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE + 1);
