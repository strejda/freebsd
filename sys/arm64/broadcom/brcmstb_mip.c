/*
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/intr.h>
#include <machine/resource.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "pic_if.h"
#include "msi_if.h"

#define	MIP_INT_RAISE		0x00
#define	MIP_INT_CLEAR		0x10
#define	MIP_INT_CFGL_HOST	0x20
#define	MIP_INT_CFGH_HOST	0x30
#define	MIP_INT_MASKL_HOST	0x40
#define	MIP_INT_MASKH_HOST	0x50
#define	MIP_INT_MASKL_VPU	0x60
#define	MIP_INT_MASKH_VPU	0x70
#define	MIP_INT_STATUSL_HOST	0x80
#define	MIP_INT_STATUSH_HOST	0x90
#define	MIP_INT_STATUSL_VPU	0xa0
#define	MIP_INT_STATUSH_VPU	0xb0


#define	BRCM_MPI_MAX_MAPS	16

#define	WR4(_sc, _r, _v)	bus_write_4((_sc)->mem_res, (_r), (_v))
#define	RD4(_sc, _r)		bus_read_4((_sc)->mem_res, (_r))

struct brcm_mip_msi_map {
	phandle_t		iparent;
	pcell_t			*cells;
	int			ncells;
	uint32_t		span;
};

struct brcm_mip_msidef {
	struct intr_irqsrc	*isrc;
	struct intr_map_data_fdt *daf;
	bool 			used;
};

struct brcm_mip_softc {
	device_t		dev;
	phandle_t		node;
	struct resource		*mem_res;
	struct resource		*msi_res;
	struct mtx		mtx;


	struct brcm_mip_msi_map *map[BRCM_MPI_MAX_MAPS];
	int			maps;

	device_t		msi_parent;
	uint32_t		msi_span;
	uint32_t		msi_offset;
	uint32_t		msi_base;
	uint64_t		msi_paddr;

	struct brcm_mip_msidef **msi_def;
};

static struct ofw_compat_data compat_data[] = {
	{"brcm,bcm2712-mip", 	1},
	{NULL,			0}
};

static struct intr_map_data *
brcm_mip_getdata(struct brcm_mip_softc *sc, struct intr_irqsrc *isrc)
{
	int msi;

	msi = isrc->isrc_irq - sc->msi_def[0]->isrc->isrc_irq;
	if (sc->msi_def[msi]->isrc != isrc) {
		printf ("irc map failed:  msi: %d, irq: %d != stored irq: %d\n",
		    msi, isrc->isrc_irq, sc->msi_def[0]->isrc->isrc_irq);
		panic("irc map failed");
	}
	return ((struct intr_map_data *)(sc->msi_def[msi]->daf));
}

static int
brcm_mip_get_msi_ranges(struct brcm_mip_softc *sc)
{
	phandle_t iparent, iparent_node;
	struct brcm_mip_msi_map *p;
	cell_t *msimap;
	uint32_t  icells;
	int i, rv, nmsimap, idx;


	nmsimap = OF_getencprop_alloc_multi(sc->node, "msi-ranges",
	    sizeof(*msimap),(void **)&msimap);
	if (nmsimap <= 0) {
		device_printf(sc->dev, "Missing 'msi-ranges' property.");
		return(ENXIO);
	}

	for (i = 0, idx = 0; i < nmsimap && idx < BRCM_MPI_MAX_MAPS; idx++) {
		iparent = msimap[i++];
		iparent_node = OF_node_from_xref(iparent);
		if (OF_searchencprop(iparent_node,
		    "#interrupt-cells", &icells, sizeof(icells)) == -1) {
			device_printf(sc->dev, "Missing '#interrupt-cells' "
			    "property\n");
			rv = ENOENT;
			goto fail;
		}
		if (icells < 1 || (i + icells + 1) > nmsimap) {
			device_printf(sc->dev, "Invalid #interrupt-cells "
			    "property value <%d>\n", icells);
			rv = ERANGE;
			goto fail;
		}
		sc->map[idx] = malloc(sizeof(struct brcm_mip_msi_map),
		    M_DEVBUF, M_WAITOK);
		p = sc->map[idx];

		p->iparent = iparent;
		p->cells = malloc(icells * sizeof(*p->cells),
		    M_DEVBUF, M_WAITOK);
		p->ncells = icells;
		memcpy(p->cells, msimap + i, icells * sizeof(*p->cells));
		i += icells;
		p->span =  msimap[i++];
	}

	sc->maps = idx;
	OF_prop_free(msimap);
	return (0);

fail:
	OF_prop_free(msimap);

	for (i = 0; i < BRCM_MPI_MAX_MAPS; i++) {
		if (sc->map[i] == NULL)
			break;
		if (sc->map[i]->cells != NULL)
			free(sc->map[i]->cells, M_DEVBUF);
		free(sc->map[i]->cells, M_DEVBUF);
	}
	return (rv);
}


static int
brcm_mip_msi_alloc_msi(device_t dev, device_t child, int count, int maxcount,
    device_t *pic, struct intr_irqsrc **srcs)
{
	struct brcm_mip_softc *sc;
	int i, irq, end_irq;
	bool found;

	KASSERT(powerof2(count), ("%s: bad count", __func__));
	KASSERT(powerof2(maxcount), ("%s: bad maxcount", __func__));

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);

	found = false;
	for (irq = 0; (irq + count - 1) < sc->msi_span; irq++) {
		/* Start on an aligned interrupt */
		if ((irq & (maxcount - 1)) != 0)
			continue;

		/* Assume we found a valid range until shown otherwise */
		found = true;

		/* Check if this range is valid */
		for (end_irq = irq; end_irq < irq + count; end_irq++) {
			if (sc->msi_def[end_irq]->used) {
				found = false;
				break;
			}
		}

		if (found)
			break;
	}

	/* Not enough interrupts were found */
	if (!found || irq == (sc->msi_span - 1)) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}


	for (i = 0; i < count; i++) {
		srcs[i] = (struct intr_irqsrc *)sc->msi_def[irq + i]->isrc;
		sc->msi_def[irq + i]->used = true;
	}

	mtx_unlock(&sc->mtx);
	*pic = dev;

	return (0);
}

static int
brcm_mip_msi_release_msi(device_t dev, device_t child, int count,
    struct intr_irqsrc **isrc)
{
	struct brcm_mip_softc *sc;
	int i, msi;

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);

	for (i = 0; i < count; i++) {
		msi = isrc[i]->isrc_irq - sc->msi_def[0]->isrc->isrc_irq;

		KASSERT(sc->msi_def[msi]->used,
		   ("%s: Trying to release an unused MSI-X interrupt",
		   __func__));
		sc->msi_def[msi]->used = false;
	}
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
brcm_mip_msi_alloc_msix(device_t dev, device_t child, device_t *pic,
    struct intr_irqsrc **isrcp)
{

	return (brcm_mip_msi_alloc_msi(dev, child, 1, 1, pic, isrcp));
}

static int
brcm_mip_msi_release_msix(device_t dev, device_t child, struct intr_irqsrc *isrc)
{

	return (brcm_mip_msi_release_msi(dev, child, 1, &isrc));
}

static int
brcm_mip_msi_map_msi(device_t dev, device_t child, struct intr_irqsrc *isrc,
    uint64_t *addr, uint32_t *data)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);

	*addr = sc->msi_paddr;
	*data = isrc->isrc_irq - sc->msi_def[0]->isrc->isrc_irq + sc->msi_offset;
	return (0);
}

static int
brcm_mip_activate_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	return (PIC_ACTIVATE_INTR(sc->msi_parent, isrc, res,
	   brcm_mip_getdata(sc, isrc)));
}

static void
brcm_mip_disable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	PIC_DISABLE_INTR(sc->msi_parent, isrc);
}

static void
brcm_mip_enable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	PIC_ENABLE_INTR(sc->msi_parent, isrc);
}

static int
brcm_mip_deactivate_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	return (PIC_DEACTIVATE_INTR(sc->msi_parent, isrc, res,
	   brcm_mip_getdata(sc, isrc)));
}

static int
brcm_mip_setup_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	return (PIC_SETUP_INTR(sc->msi_parent, isrc, res,
	   brcm_mip_getdata(sc, isrc)));
}

static int
brcm_mip_teardown_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	return (PIC_TEARDOWN_INTR(sc->msi_parent, isrc, res,
	   brcm_mip_getdata(sc, isrc)));
}

static int
brcm_mip_map_intr(device_t dev, struct intr_map_data *data,
   struct intr_irqsrc **isrcp)
{
	struct brcm_mip_softc *sc;
	struct intr_map_data_fdt *daf;
	u_int irq;
	int rv;

	sc = device_get_softc(dev);

	if (data->type != INTR_MAP_DATA_FDT)
		return (ENOTSUP);

	daf = (struct intr_map_data_fdt *)data;
	if (daf->ncells != 2)
		return (EINVAL);

	irq = daf->cells[0];
	if (irq >= sc->msi_span)
		return (EINVAL);
device_printf(sc->dev, "%s: Mapping IRQ: %d, GIC: %d\n", __func__, irq, sc->msi_def[irq]->daf->cells[1]);

	rv = PIC_MAP_INTR(sc->msi_parent,
	    (struct intr_map_data *)sc->msi_def[irq]->daf, isrcp);

	return(rv);
}

static void
brcm_mip_pre_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	PIC_PRE_ITHREAD(sc->msi_parent, isrc);
}

static void
brcm_mip_post_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	PIC_POST_ITHREAD(sc->msi_parent, isrc);
}

static void
brcm_mip_post_filter(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	PIC_POST_FILTER(sc->msi_parent, isrc);
}

#ifdef SMP
static int
brcm_mip_bind_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct brcm_mip_softc *sc;

	sc = device_get_softc(dev);
	return (PIC_BIND_INTR(sc->msi_parent, isrc));
}
#endif

static int
brcm_mip_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "Broadcom STB MSI-X interrupt controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_mip_detach(device_t dev)
{
	struct brcm_mip_softc *sc;
	int i;

	sc = device_get_softc(dev);

	for (i = 0; i < BRCM_MPI_MAX_MAPS; i++) {
		if (sc->map[i] == NULL)
			break;
		if (sc->map[i]->cells != NULL)
			free(sc->map[i]->cells, M_DEVBUF);
		free(sc->map[i]->cells, M_DEVBUF);
	}

	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev, sc->mem_res);

	mtx_destroy(&sc->mtx);

	return (0);
}
static int
brcm_mip_attach(device_t dev)
{
	struct brcm_mip_softc *sc;
	pcell_t cell0, cell1, cell2;
	int i, rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);

	mtx_init(&sc->mtx, "brcm_mip mtx", NULL, MTX_DEF);
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0, RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		goto fail;
	}

	rv = OF_getencprop(sc->node, "brcm,msi-offset", &sc->msi_offset,
	    sizeof(sc->msi_offset));
	if (rv <= 0)
		sc->msi_offset = 0;

	rv = brcm_mip_get_msi_ranges(sc);
	if (rv != 0)
		goto fail;
	if (sc->maps != 1) {
		device_printf(sc->dev,
		    "Invalid number of 'msi-ranges' ranges.\n");
		goto fail;
	}

	/* Second reg property determines PA of MSI page */
	sc->msi_paddr  = bus_get_resource_start(dev, SYS_RES_MEMORY, 1);
	if (sc->msi_paddr == 0) {
		device_printf(dev, "Cannot get 'msi' resource\n");
		goto fail;
	}

	sc->msi_span = sc->map[0]->span;
	sc->msi_base = sc->map[0]->cells[1];
	sc->msi_parent = OF_device_from_xref(sc->map[0]->iparent);
	if (sc->msi_parent  == NULL) {
		device_printf(sc->dev, "Cannot get interrupt parent.\n");
		goto fail;
	}

	/* For all Umask all for host cpu, mask all for VPU,set all edge-triggered */
	WR4(sc, MIP_INT_MASKL_HOST, 0);
	WR4(sc, MIP_INT_MASKH_HOST, 0);
	WR4(sc, MIP_INT_MASKL_VPU, 0xFFFFFFFF);
	WR4(sc, MIP_INT_MASKH_VPU, 0xFFFFFFFF);
	WR4(sc, MIP_INT_CFGL_HOST, 0xFFFFFFFF);
	WR4(sc, MIP_INT_CFGH_HOST, 0xFFFFFFFF);


	/* Prepare remap data for MSI num  -> GIC FDT */
	sc->msi_def = malloc(sizeof(struct brcm_mip_msidef *) * sc->msi_span,
	    M_DEVBUF,  M_WAITOK | M_ZERO);
	cell0 = sc->map[0]->cells[0];
	cell1 = sc->map[0]->cells[1];
	cell2 = sc->map[0]->cells[2];
	for (i = 0; i < sc->msi_span; i++) {
		sc->msi_def[i] = malloc(sizeof(struct brcm_mip_msidef),
		    M_DEVBUF, M_WAITOK | M_ZERO);

		sc->msi_def[i]->daf =
		    (struct intr_map_data_fdt *)intr_alloc_map_data(
		    INTR_MAP_DATA_FDT,  sizeof(struct intr_map_data_fdt) +
		    3 * sizeof(pcell_t),  M_WAITOK | M_ZERO);
		sc->msi_def[i]->daf->ncells = 3;
		sc->msi_def[i]->daf->cells[0] = cell0;	/* GIC type */
		sc->msi_def[i]->daf->cells[1] = cell1 + i + sc->msi_offset;
		sc->msi_def[i]->daf->cells[2] = cell2;	/* GIC triger */

		rv = PIC_MAP_INTR(sc->msi_parent,
		    (struct intr_map_data *)sc->msi_def[i]->daf,
		    &sc->msi_def[i]->isrc);
		if (rv != 0) {
			device_printf(sc->dev,
			    "%s: Cannot map interrupt %d: %d\n",
			    __func__, i, rv);
			goto fail;
		}
	}


	rv = intr_msi_register(sc->dev, OF_xref_from_node(sc->node));
	if (rv != 0) {
		device_printf(dev, "Cannot register MSI PIC: %d\n", rv);
		goto fail;
	}

	OF_device_register_xref(OF_xref_from_node(sc->node), sc->dev);
	if (bootverbose) {
		device_printf(sc->dev, "count: %u, msi_base: %u, "
		    "msi_offset: %u, msi_paddr: 0x%lX, parent: %s\n",
		    sc->msi_span, sc->msi_base, sc->msi_offset,  sc->msi_paddr,
		    device_get_nameunit(sc->msi_parent));
	}

	return (0);

fail:

	brcm_mip_detach(sc->dev);
	return (ENXIO);
}

static device_method_t brcm_mip_methods[] = {
	DEVMETHOD(device_probe,		brcm_mip_probe),
	DEVMETHOD(device_attach,	brcm_mip_attach),
	DEVMETHOD(device_detach,	brcm_mip_detach),

	/* MSI/MSI-X */
	DEVMETHOD(msi_alloc_msi,	brcm_mip_msi_alloc_msi),
	DEVMETHOD(msi_release_msi,	brcm_mip_msi_release_msi),
	DEVMETHOD(msi_alloc_msix,	brcm_mip_msi_alloc_msix),
	DEVMETHOD(msi_release_msix,	brcm_mip_msi_release_msix),
	DEVMETHOD(msi_map_msi,		brcm_mip_msi_map_msi),

	/* Interrupt controller interface */
	DEVMETHOD(pic_activate_intr,	brcm_mip_activate_intr),
	DEVMETHOD(pic_disable_intr,	brcm_mip_disable_intr),
	DEVMETHOD(pic_enable_intr,	brcm_mip_enable_intr),
	DEVMETHOD(pic_deactivate_intr,	brcm_mip_deactivate_intr),
	DEVMETHOD(pic_setup_intr,	brcm_mip_setup_intr),
	DEVMETHOD(pic_teardown_intr,	brcm_mip_teardown_intr),
	DEVMETHOD(pic_map_intr,		brcm_mip_map_intr),
	DEVMETHOD(pic_pre_ithread,	brcm_mip_pre_ithread),
	DEVMETHOD(pic_post_ithread,	brcm_mip_post_ithread),
	DEVMETHOD(pic_post_filter,	brcm_mip_post_filter),
#ifdef SMP
	DEVMETHOD(pic_bind_intr,	brcm_mip_bind_intr),
#endif
	DEVMETHOD_END
};

static DEFINE_CLASS_0(brcmstb_mip, brcmstb_mip_driver, brcm_mip_methods,
    sizeof(struct brcm_mip_softc));
EARLY_DRIVER_MODULE(brcmstb_mip, simplebus, brcmstb_mip_driver, NULL, NULL,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE + 1);
