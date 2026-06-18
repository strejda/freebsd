/*
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/rman.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <machine/bus.h>
#include <machine/resource.h>


#include <dev/fdt/simplebus.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dt-bindings/interrupt-controller/irq.h>

#include "pic_if.h"
#include "pcib_if.h"

#if 1
#define dprintf(fmt, args...)	 printf("%s(): "fmt, __func__, ##args)
#else
#define dprintf(fmt, args...)
#endif


#ifndef PCI_VENDOR_ID_RPI
#define PCI_VENDOR_ID_RPI		0x1de4
#endif

#ifndef PCI_DEVICE_ID_RPI_RP1_C0
#define PCI_DEVICE_ID_RPI_RP1_C0	0x0001
#endif

#define RP1_SB_MSI_NUM		61

/* Registers */
#define	RP1_SB_PCIE_APBS	0x108000
#define	RP1_SB_MSIX_CFG(x)	(0x108008 + (4 * (x)))
#define	 MSIX_CFG_IACK_EN		(1 << 3)
#define	 MSIX_CFG_IACK			(1 << 2)
#define	 MSIX_CFG_ENABLE		(1 << 0)

#define	RD4(sc, reg)		bus_read_4((sc)->mem_res, (reg))
#define	WR4(sc, reg, val)	bus_write_4((sc)->mem_res, (reg), (val))
#define	WR4_SET(sc, reg, val)	bus_write_4((sc)->mem_res, (reg) + 0x800, (val))
#define	WR4_CLR(sc, reg, val)	bus_write_4((sc)->mem_res, (reg) + 0xC00, (val))
struct rp1_sb_soft;

struct rp1_sb_irqsrc {
	struct intr_irqsrc	isrc;
	u_int			irq;
	bool			is_level;
	struct intr_irqsrc	*parent_isrc;
};

struct rp1_sb_irqarg {
	struct rp1_sb_softc	*sc;
	struct rp1_sb_irqsrc	*risrc;
};

struct rp1_sb_softc {
	struct simplebus_softc  simplebus_sc;
	device_t		dev;
	struct mtx		mtx;
	struct resource		*mem_res;
	struct resource		*msix_res;
	struct resource		*shmem_res;
	struct resource		*irq_res[RP1_SB_MSI_NUM];
	struct rman		mem_rman;
	struct rman		irq_rman;
	void			*msi_intr_cookie[RP1_SB_MSI_NUM];

	bool			simplebus_inited;
	phandle_t		node;
	device_t		msi_parent;
	device_t		base_dev;	/* PCIe RC */
	phandle_t		base_node;	/* PCIe RC */
	phandle_t		bus_node;	/* attached PCIe bus */

	struct rp1_sb_irqsrc	risrcs[RP1_SB_MSI_NUM];
	struct rp1_sb_irqarg	irq_args[RP1_SB_MSI_NUM];
};

/* ---------------------- RESOURCES interface ------------------------------ */

 /*
  * BAR0 maps the MSIX table
  * BAR1 maps the peripheral region at 0x40000000
  * BAR2 maps the complete SRAM at 0x20000000
  */
static struct rman *
rp1_sb_get_rman(device_t dev, int type, u_int flags)
{
	struct rp1_sb_softc *sc;

	sc = device_get_softc(dev);
	if (type == SYS_RES_MEMORY)
		return(&sc->mem_rman);

	if (type == SYS_RES_IRQ)
		return(&sc->irq_rman);

	panic("Unexpected type or rman: %d\n", type);
}

static struct resource *
rp1_sb_alloc_resource(device_t dev, device_t child, int type, int rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	struct simplebus_softc *sc;
	struct simplebus_devinfo *di;
	struct resource_list_entry *rle;
	int j;

	sc = device_get_softc(dev);
	/*
	 * Request for the default allocation with a given rid: use resource
	 * list stored in the local device info.
	 */
	if (RMAN_IS_DEFAULT_RANGE(start, end) && count == 1) {
		if ((di = device_get_ivars(child)) == NULL)
			return (NULL);

		rle = resource_list_find(&di->rl, type, rid);
		if (rle == NULL) {
			if (bootverbose)
				device_printf(dev, "no default resources for "
				    "rid = %d, type = %d\n", rid, type);
			return (NULL);
		}
		start = rle->start;
		end = rle->end;
		count = rle->count;
	}

	if (type == SYS_RES_MEMORY) {
		/* Remap through ranges property */
		for (j = 0; j < sc->nranges; j++) {
			if (start >= sc->ranges[j].bus && end <
			    sc->ranges[j].bus + sc->ranges[j].size) {
				start -= sc->ranges[j].bus;
				start += sc->ranges[j].host;
				end -= sc->ranges[j].bus;
				end += sc->ranges[j].host;
				break;
			}
		}
		if (j == sc->nranges && sc->nranges != 0) {
			if (bootverbose)
				device_printf(dev, "Could not map resource "
				    "%#jx-%#jx\n", start, end);

			return (NULL);
		}

		/* Allocate memory from local rman for BAR(1) */
		return (bus_generic_rman_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));
	} else 	if (type == SYS_RES_IRQ) {
		/* Allocate interrupt from local rman for MSI interrupts */
		return (bus_generic_rman_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));
	}

	return (bus_generic_alloc_resource(dev, child, type, rid, start, end,
	    count, flags));
}

static int
rp1_sb_release_resource(device_t dev, device_t child, struct resource *r)
{

	if (rman_get_type(r) == SYS_RES_MEMORY ||
	    rman_get_type(r) == SYS_RES_IRQ)
		return (bus_generic_rman_release_resource(dev, child, r));

	return (bus_generic_release_resource(dev, child, r));

}

static int
rp1_sb_adjust_resource(device_t dev, device_t child, struct resource *r,
    rman_res_t start, rman_res_t end)
{

	if (rman_get_type(r) == SYS_RES_MEMORY ||
	    rman_get_type(r) == SYS_RES_IRQ)
		return (bus_generic_rman_adjust_resource(dev, child, r, start,
		    end));

	return (bus_generic_adjust_resource(dev, child, r, start, end));
}

static int
rp1_sb_activate_resource(device_t dev, device_t child, struct resource *r)
{
	if (rman_get_type(r) == SYS_RES_MEMORY ||
	    rman_get_type(r) == SYS_RES_IRQ)
		return (bus_generic_rman_activate_resource(dev, child, r));
	return (bus_generic_activate_resource(dev, child, r));
}

static int
rp1_sb_deactivate_resource(device_t dev, device_t child, struct resource *r)
{

	if (rman_get_type(r) == SYS_RES_MEMORY ||
	    rman_get_type(r) == SYS_RES_IRQ)
		return (bus_generic_rman_deactivate_resource(dev, child, r));

	return (bus_generic_deactivate_resource(dev, child, r));
}

static int
rp1_sb_map_resource(device_t dev, device_t child, struct resource *r,
    struct resource_map_request *argsp, struct resource_map *map)
{
	struct rp1_sb_softc *sc;
	uintptr_t tmp;
	struct resource_map_request args;
	rman_res_t length, start;
	int rv;

	sc = device_get_softc(dev);
	if (rman_get_type(r) == SYS_RES_MEMORY) {
		resource_init_map_request(&args);
		rv = resource_validate_map_request(r, argsp, &args, &start,
		     &length);
		if (rv != 0)
			return (rv);

		tmp = (uintptr_t)rman_get_virtual(sc->mem_res);
		tmp += rman_get_start(r);

		map->r_vaddr = (void *)tmp;
		map->r_bustag = BUS_GET_BUS_TAG(child, child);
		if (map->r_bustag == NULL)
			return (ENOMEM);
		map->r_size = length;
		map->r_bushandle = (bus_space_handle_t)map->r_vaddr;

		return (0);
	}
	return (bus_generic_map_resource(dev, child, r, argsp, map));

}

static int
rp1_sb_unmap_resource(device_t dev, device_t child, struct resource *r,
    struct resource_map *map)
{

	if (rman_get_type(r) == SYS_RES_MEMORY) {
		rman_set_virtual(r, NULL);
		return (0);
	}

	return (bus_generic_unmap_resource(dev, child, r, map));

}

static phandle_t
rp1_sb_get_node(device_t dev, device_t child)
{
	struct rp1_sb_softc *sc;

	sc = device_get_softc(dev);
	return (sc->bus_node);
}

static const struct ofw_bus_devinfo *
rp1_sb_get_devinfo(device_t dev __unused, device_t child)
{
	struct simplebus_devinfo *ndi;

	ndi = device_get_ivars(child);
	if (ndi == NULL)
		return (NULL);
	return (&ndi->obdinfo);
}

/* ------------------------- PIC interface ---------------------------------- */

static void
rp1_sb_pic_enable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqsrc *risrc;

	sc = device_get_softc(dev);
	risrc = (struct rp1_sb_irqsrc *)isrc;

	WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_ENABLE);
	WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK);
}

static void
rp1_sb_pic_disable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqsrc *risrc;

	sc = device_get_softc(dev);
//device_printf(sc->dev, "%s: Enter\n", __func__);
	risrc = (struct rp1_sb_irqsrc *)isrc;

	WR4_CLR(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_ENABLE);
}

static int
rp1_sb_pic_map_intr(device_t dev, struct intr_map_data *data,
   struct intr_irqsrc **isrcp)
{
	struct rp1_sb_softc *sc;
	struct intr_map_data_fdt *daf;
	u_int irq;

	sc = device_get_softc(dev);

	if (data->type != INTR_MAP_DATA_FDT)
		return (ENOTSUP);
	daf = (struct intr_map_data_fdt *)data;
	if (daf->ncells != 2)
		return (EINVAL);
	if (daf->cells[0] >= RP1_SB_MSI_NUM)
		return (EINVAL);
	irq = daf->cells[0];

	if (isrcp != NULL)
		*isrcp =  (struct intr_irqsrc *)(sc->risrcs + irq);
	return (0);
}

static int
rp1_sb_pic_setup_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqsrc *risrc;
	struct intr_map_data_fdt *daf;

	sc = device_get_softc(dev);

	if (data->type != INTR_MAP_DATA_FDT)
		return (ENOTSUP);

	daf = (struct intr_map_data_fdt *)data;
	if (daf->ncells != 2)
		return (EINVAL);

	risrc = (struct rp1_sb_irqsrc *)isrc;

	switch (daf->cells[1] ) {
	case IRQ_TYPE_LEVEL_HIGH:
		risrc->is_level = true;
		WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK_EN);
	break;
	case IRQ_TYPE_EDGE_RISING:
		risrc->is_level = false;
		break;
	default:
		return (EINVAL);
	}

	if (risrc->is_level)
		WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK_EN);
	else
		WR4_CLR(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK_EN);
	rp1_sb_pic_enable_intr(sc->dev, isrc);

	return (0);
}


static int
rp1_sb_pic_teardown_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct rp1_sb_softc *sc;

	sc = device_get_softc(dev);
device_printf(sc->dev, "%s: Enter\n", __func__);
	rp1_sb_pic_disable_intr(sc->dev, isrc);
	return (0);
}

static void
rp1_sb_pic_pre_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqsrc *risrc;

	sc = device_get_softc(dev);
//device_printf(sc->dev, "%s: Enter\n", __func__);
	risrc = (struct rp1_sb_irqsrc *)isrc;

	rp1_sb_pic_disable_intr(sc->dev, isrc);
	if (risrc->is_level)
		WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK_EN);
}

static void
rp1_sb_pic_post_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_sb_softc *sc;

	sc = device_get_softc(dev);
//device_printf(sc->dev, "%s: Enter\n", __func__);
	rp1_sb_pic_enable_intr(sc->dev, isrc);
}

static void
rp1_sb_pic_post_filter(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqsrc *risrc;

	sc = device_get_softc(dev);
//device_printf(sc->dev, "%s: Enter\n", __func__);
	risrc = (struct rp1_sb_irqsrc *)isrc;
	if (risrc->is_level)
		WR4_SET(sc, RP1_SB_MSIX_CFG(risrc->irq), MSIX_CFG_IACK_EN);
}

/* ------------------------- BUS interface ---------------------------------- */

static int
rp1_sb_probe(device_t dev)
{
	uint16_t devid, vendor;

	vendor = pci_get_vendor(dev);
	devid = pci_get_device(dev);

	if (vendor == PCI_VENDOR_ID_RPI && devid == PCI_DEVICE_ID_RPI_RP1_C0) {
		device_set_desc(dev, "RaspberryPi RP1 southbridge");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}


static phandle_t
rp1_sb_find_msi_parent(phandle_t node)
{
	phandle_t msi_parent;

	do {
		if (OF_getencprop(node, "msi-parent", &msi_parent,
		    sizeof(msi_parent)) > 0) {
			node = OF_node_from_xref(msi_parent);
			break;
		} else {
			node = OF_parent(node);
		}
		if (node == 0)
			return (0);
	} while (true);

	return (OF_xref_from_node(node));
}

static int
rp1_sb_intr(void *arg)
{
	struct rp1_sb_softc *sc;
	struct rp1_sb_irqarg *irq_arg;
	struct rp1_sb_irqsrc *risrc;
	struct trapframe *tf;

	irq_arg = arg;
	sc =  irq_arg->sc;
	risrc = irq_arg->risrc;

	tf = curthread->td_intr_frame;

	if (intr_isrc_dispatch(&risrc->isrc, tf) != 0) {
		 device_printf(sc->dev, "Stray irq %u disabled\n", risrc->irq);
	}

	return (FILTER_HANDLED);
}

static int
rp1_sb_attach(device_t dev)
{
	struct rp1_sb_softc *sc;
	phandle_t c1, c2, msi_xref;
	char cname[64];
	const char *name;
	u_int irq, mem_size;
	int rv, count, i;

	sc = device_get_softc(dev);
	sc->dev = dev;


	/* Find OFW based parent */
	sc->base_dev = device_get_parent(sc->dev);
	sc->base_node = ofw_bus_get_node(sc->base_dev);
	while (sc->base_node == -1) {
		sc->base_dev = device_get_parent(sc->base_dev);
		if (sc->base_dev == NULL)
			break;
		sc->base_node = ofw_bus_get_node(sc->base_dev);
	}
	if (sc->base_node == -1 || sc->base_dev == NULL) {
		device_printf(sc->dev, "Cannot find base node.\n");
		return (ENXIO);
	}


	/* Find node for this device */
	snprintf(cname, sizeof(cname), "pci%x,%x", pci_get_vendor(sc->dev),
	    pci_get_device(sc->dev));
	sc->node = -1;
	for (c1 = OF_child(sc->base_node); c1 > 0; c1 = OF_peer(c1)) {
		for (c2 = OF_child(c1); c2 > 0; c2 = OF_peer(c2)) {
			if (ofw_bus_node_is_compatible(c2, cname))
				sc->node = c2;
		}
	}

	if (sc->node == -1) {
		device_printf(sc->dev, "Cannot find compatible node.\n");
		return (ENXIO);
	}

	/* Find node for child simplebus */
	sc->bus_node = -1;
	for (c1 = OF_child(sc->node); c1 > 0; c1 = OF_peer(c1)) {
		if (ofw_bus_node_is_compatible(c1, "simple-bus"))
			sc->bus_node = c1;
	}
	if (sc->bus_node == -1) {
		device_printf(sc->dev, "Cannot find simplebus node.\n");
		return (ENXIO);
	}
	msi_xref = rp1_sb_find_msi_parent(sc->node);
	if (msi_xref <= 0) {
		device_printf(sc->dev, "Cannot find interrupt parent.\n");
		return (ENXIO);
	}

	sc->msi_parent = OF_device_from_xref(msi_xref);
	if (sc->msi_parent  == NULL) {
		device_printf(sc->dev, "Cannot get interrupt parent.\n");
		goto fail;
	}

	/* Map MSIX table BAR(0) */
	sc->msix_res = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY,
	    pci_msix_table_bar(dev), RF_ACTIVE);
	if (sc->msix_res == NULL) {
		device_printf(sc->dev, "Cannot allocate MSIX table resource\n");
		return (ENXIO);
	}


	/* Map periperals MMIO BAR(1) */
	sc->mem_res = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY,
	    PCIR_BAR(1), RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(sc->dev, "Cannot allocate memory resource\n");
		return (ENXIO);
	}

	/* Map shared memory  BAR(2) */
	sc->shmem_res = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY,
	    PCIR_BAR(2), RF_ACTIVE);
	if (sc->shmem_res == NULL) {
		device_printf(sc->dev, "Cannot allocate memory resource\n");
		return (ENXIO);
	}

	mem_size = rman_get_size(sc->mem_res);

	sc->mem_rman.rm_type = RMAN_ARRAY;
	sc->mem_rman.rm_descr = "BAR(1) pheripheral MMIO";
	sc->mem_rman.rm_start = 0;
	sc->mem_rman.rm_end = mem_size - 1;
	if (rman_init(&sc->mem_rman) != 0) {
		device_printf(sc->dev, "Cannot init MMIO rman\n");
		goto fail;
	}
	if (rman_manage_region(&sc->mem_rman, 0, mem_size - 1) != 0) {
		device_printf(sc->dev, "Cannot manage MMIO region\n");
		goto fail;
	}

	sc->irq_rman.rm_type = RMAN_ARRAY;
	sc->irq_rman.rm_descr = "MSI interrupts";
	if (rman_init(&sc->irq_rman) != 0) {
		device_printf(sc->dev, "Cannot init MSI rman\n");
		goto fail;
	}
	/* In INTRNG, interrupt resources are only arbitrary numbers */
	if (rman_manage_region(&sc->irq_rman, 0, ~0) != 0) {
		device_printf(sc->dev, "Cannot manage MSI region\n");
		goto fail;
	}

	/* Create all interrupt sources */
	name = device_get_nameunit(sc->dev);
	for (irq = 0; irq < RP1_SB_MSI_NUM; irq++) {
		sc->risrcs[irq].irq = irq;
		rv = intr_isrc_register(&sc->risrcs[irq].isrc,
		    sc->dev, 0, "%s,%u", name, irq);
		if (rv != 0) {
			device_printf(dev, "Cannot register irqs: %d\n", rv);
			goto fail; /* XXX deregister ISRCs */
		}
	}
	if (intr_pic_register(dev, OF_xref_from_node(sc->node)) == NULL) {
		device_printf(dev, "Cannot register PIC\n");
		goto fail;
	}

	count = RP1_SB_MSI_NUM;
	rv = pci_alloc_msix(sc->dev, &count);
	if (rv != 0) {
		device_printf(dev, "Cannot allocate MSI-X interrupts.\n");
		goto fail;
	}
	if (count != RP1_SB_MSI_NUM) {
		device_printf(dev, "Unexpected number of MSI-X interrupts\n");
		goto fail;
	}

	for (i = 0; i < RP1_SB_MSI_NUM; i++) {
		sc->irq_res[i] = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ,
		    i + 1, RF_ACTIVE);
		if (sc->irq_res[i] == NULL) {
			device_printf(dev,
			    "Cannot allocate MSI-X interrupt resource\n");
			goto fail;
		}

		sc->irq_args[i].sc = sc;
		sc->irq_args[i].risrc = sc->risrcs + i;
		rv = bus_setup_intr(dev, sc->irq_res[i],
		    INTR_TYPE_BIO | INTR_MPSAFE, rp1_sb_intr, NULL,
		    sc->irq_args + i, sc->msi_intr_cookie + i);
		if (rv != 0 ) {
			device_printf(dev, "Cannot setup interrupt handler\n");
			goto fail;
		}
	}

	pci_enable_busmaster(sc->dev);

	/* Init simple bus and attach it */
	sc->simplebus_sc.flags  = SB_FLAG_NO_RANGES;
	sc->simplebus_inited = true;
	rv = simplebus_attach(sc->dev);
	if (rv != 0 ) {
		device_printf(dev, "Cannot setup initialize simplebus\n");
		goto fail;
	}

	return (0);

fail:
	if (sc->simplebus_inited)
		simplebus_detach(dev);
	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, 0, sc->mem_res);
panic("end");
	return (ENXIO);
}


static int
rp1_sb_detach(device_t dev)
{
	struct rp1_sb_softc *sc;
	int rv;

	sc = device_get_softc(dev);

	if (sc->simplebus_inited)
		simplebus_detach(dev);
	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);

	rv = bus_generic_detach(dev);
	return (rv);
}

static device_method_t rp1_sb_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			rp1_sb_probe),
	DEVMETHOD(device_attach,		rp1_sb_attach),
	DEVMETHOD(device_detach,		rp1_sb_detach),

	/* Bus interface */
	DEVMETHOD(bus_get_rman,			rp1_sb_get_rman),
	DEVMETHOD(bus_alloc_resource,		rp1_sb_alloc_resource),
	DEVMETHOD(bus_release_resource,		rp1_sb_release_resource),
	DEVMETHOD(bus_adjust_resource,		rp1_sb_adjust_resource),
	DEVMETHOD(bus_activate_resource, 	rp1_sb_activate_resource),
	DEVMETHOD(bus_deactivate_resource,	rp1_sb_deactivate_resource),
	DEVMETHOD(bus_map_resource,		rp1_sb_map_resource),
	DEVMETHOD(bus_unmap_resource,		rp1_sb_unmap_resource),

	/* Interrupt controller interface */
	DEVMETHOD(pic_disable_intr,		rp1_sb_pic_disable_intr),
	DEVMETHOD(pic_enable_intr,		rp1_sb_pic_enable_intr),
	DEVMETHOD(pic_map_intr,			rp1_sb_pic_map_intr),
	DEVMETHOD(pic_setup_intr,		rp1_sb_pic_setup_intr),
	DEVMETHOD(pic_teardown_intr,		rp1_sb_pic_teardown_intr),
	DEVMETHOD(pic_post_filter,		rp1_sb_pic_post_filter),
	DEVMETHOD(pic_post_ithread,		rp1_sb_pic_post_ithread),
	DEVMETHOD(pic_pre_ithread,		rp1_sb_pic_pre_ithread),

	/* OFW bus interface */
	DEVMETHOD(ofw_bus_get_node,		rp1_sb_get_node),
	DEVMETHOD(ofw_bus_get_devinfo,		rp1_sb_get_devinfo),
	DEVMETHOD_END
};

DEFINE_CLASS_1(rp1_sb, rp1_sb_driver, rp1_sb_methods,
    sizeof(struct rp1_sb_softc), simplebus_driver);
DRIVER_MODULE(rp1_sb, pci, rp1_sb_driver, NULL, NULL);
EARLY_DRIVER_MODULE(simplebus, rp1_sb, simplebus_driver, 0, 0, BUS_PASS_BUS);
