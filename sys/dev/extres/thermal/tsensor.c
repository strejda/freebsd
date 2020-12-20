/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright 2020 Michal Meloun <mmel@FreeBSD.org>
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

#include <sys/cdefs.h>

#include "opt_platform.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kobj.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>

#ifdef FDT
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include  <dev/extres/thermal/tsensor.h>
#include  <dev/extres/thermal/tsensor_internal.h>

#include "tsdev_if.h"

SYSCTL_NODE(_hw, OID_AUTO, temperature, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
    "Temperature");

MALLOC_DEFINE(M_THERMAL, "thermal", "thermal framework");

static struct tsnode *tsnode_find_by_name(const char *name);
static int tsnode_method_init(struct tsnode *tsnode);

/*
 * tsensor controller methods.
 */
static tsnode_method_t tsnode_methods[] = {
	TSNODEMETHOD(tsnode_init,		tsnode_method_init),

	TSNODEMETHOD_END
};
DEFINE_CLASS_0(tsnode, tsnode_class, tsnode_methods, 0);

static tsnode_list_t tsnode_list = TAILQ_HEAD_INITIALIZER(tsnode_list);
struct sx tsnode_topo_lock;
SX_SYSINIT(ts_topology, &tsnode_topo_lock, "tsensor topology lock");

/*
 * sysctl handler
 */
static int
tsnode_temperature_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct tsnode *tsnode = arg1;
	int rv, val;


	TSENSOR_TOPO_SLOCK();
	if ((rv = tsnode_temperature(tsnode, &val)) != 0) {
		TSENSOR_TOPO_UNLOCK();
		return (rv);
	}
	TSENSOR_TOPO_UNLOCK();

	/* Convert from microcelsius to decikelvins */
	val = val / 100;
	val +=  2731;
	return sysctl_handle_int(oidp, &val, sizeof(val), req);
}

/*
 *
 * Default tsensor methods for base class.
 *
 */

static int
tsnode_method_init(struct tsnode *tsnode)
{

	return (0);
}

/* Create and initialize tsensor object, but do not register it. */
struct tsnode *
tsnode_create(device_t pdev, tsnode_class_t tsnode_class,
    struct tsnode_init_def *def)
{
	struct tsnode *tsnode;
	struct sysctl_oid *tsnode_oid;

	KASSERT(def->name != NULL, ("tsensor name is NULL"));
	KASSERT(def->name[0] != '\0', ("tsensor name is empty"));

	TSENSOR_TOPO_SLOCK();
	if (tsnode_find_by_name(def->name) != NULL)
		panic("Duplicated tsensor registration: %s\n", def->name);
	TSENSOR_TOPO_UNLOCK();

	/* Create object and initialize it. */
	tsnode = malloc(sizeof(struct tsnode), M_THERMAL,
	    M_WAITOK | M_ZERO);
	kobj_init((kobj_t)tsnode, (kobj_class_t)tsnode_class);
	sx_init(&tsnode->lock, "tsensor node lock");

	/* Allocate softc if required. */
	if (tsnode_class->size > 0) {
		tsnode->softc = malloc(tsnode_class->size, M_THERMAL,
		    M_WAITOK | M_ZERO);
	}

	/* Copy all strings unless they're flagged as static. */
	if (def->flags &TSENSOR_FLAGS_STATIC) {
		tsnode->name = def->name;
	} else {
		tsnode->name = strdup(def->name, M_THERMAL);
	}

	/* Rest of init. */
	TAILQ_INIT(&tsnode->consumers_list);
	tsnode->id = def->id;
	tsnode->pdev = pdev;
#ifdef FDT
	tsnode->ofw_node = def->ofw_node;
#endif

	sysctl_ctx_init(&tsnode->sysctl_ctx);
	tsnode_oid = SYSCTL_ADD_NODE(&tsnode->sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw_temperature),
	    OID_AUTO, "tsensor",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "Temperature sensors");
	SYSCTL_ADD_PROC(&tsnode->sysctl_ctx,
	    SYSCTL_CHILDREN(tsnode_oid),
	    OID_AUTO, tsnode->name,
	    CTLTYPE_INT | CTLFLAG_RD,
	    tsnode, 0, tsnode_temperature_sysctl,
	    "IK",
	    "Current temperature");

	return (tsnode);
}

/* Register tsensor object. */
struct tsnode *
tsnode_register(struct tsnode *tsnode)
{
	int rv;

#ifdef FDT
	if (tsnode->ofw_node <= 0)
		tsnode->ofw_node = ofw_bus_get_node(tsnode->pdev);
	if (tsnode->ofw_node <= 0)
		return (NULL);
#endif

	rv = TSNODE_INIT(tsnode);
	if (rv != 0) {
		printf("TSNODE_INIT failed: %d\n", rv);
		return (NULL);
	}

	TSENSOR_TOPO_XLOCK();
	TAILQ_INSERT_TAIL(&tsnode_list, tsnode, tslist_link);
	TSENSOR_TOPO_UNLOCK();
#ifdef FDT
	OF_device_register_xref(OF_xref_from_node(tsnode->ofw_node),
	    tsnode->pdev);
#endif
	return (tsnode);
}

static struct tsnode *
tsnode_find_by_name(const char *name)
{
	struct tsnode *entry;

	TSENSOR_TOPO_ASSERT();

	TAILQ_FOREACH(entry, &tsnode_list, tslist_link) {
		if (strcmp(entry->name, name) == 0)
			return (entry);
	}

	return (NULL);
}

static struct tsnode *
tsnode_find_by_id(device_t dev, intptr_t id)
{
	struct tsnode *entry;

	TSENSOR_TOPO_ASSERT();

	TAILQ_FOREACH(entry, &tsnode_list, tslist_link) {
		if ((entry->pdev == dev) && (entry->id ==  id))
			return (entry);
	}

	return (NULL);
}

void *
tsnode_get_softc(struct tsnode *tsnode)
{

	return (tsnode->softc);
}

device_t
tsnode_get_device(struct tsnode *tsnode)
{

	return (tsnode->pdev);
}

intptr_t tsnode_get_id(struct tsnode *tsnode)
{

	return (tsnode->id);
}

#ifdef FDT
phandle_t
tsnode_get_ofw_node(struct tsnode *tsnode)
{

	return (tsnode->ofw_node);
}
#endif

int
tsnode_temperature(struct tsnode *tsnode, int *value)
{
	int rv;

	TSENSOR_TOPO_ASSERT();

	TSNODE_XLOCK(tsnode);
	rv = TSNODE_TEMPERATURE(tsnode, value);
	TSNODE_UNLOCK(tsnode);
	return (rv);
}

 /* --------------------------------------------------------------------------
 *
 * tsensor consumers interface.
 *
 */

/* Helper function for tsensor_get*() */
static tsensor_t
tsensor_create(struct tsnode *tsnode, device_t cdev)
{
	struct tsensor *tsensor;

	TSENSOR_TOPO_ASSERT();

	tsensor =  malloc(sizeof(struct tsensor), M_THERMAL, M_WAITOK | M_ZERO);
	tsensor->cdev = cdev;
	tsensor->tsnode = tsnode;

	TSNODE_XLOCK(tsnode);
	tsnode->ref_cnt++;
	TAILQ_INSERT_TAIL(&tsnode->consumers_list, tsensor, link);
	TSNODE_UNLOCK(tsnode);

	return (tsensor);
}


int
tsensor_temperature(tsensor_t tsensor, int *value)
{
	int rv;
	struct tsnode *tsnode;

	tsnode = tsensor->tsnode;
	KASSERT(tsnode->ref_cnt > 0,
	   ("Attempt to access unreferenced tsensor.\n"));

	TSENSOR_TOPO_SLOCK();
	rv = tsnode_temperature(tsnode, value);
	TSENSOR_TOPO_UNLOCK();
	return (rv);
}

int
tsensor_get_by_id(device_t consumer_dev, device_t provider_dev, intptr_t id,
    tsensor_t *tsensor)
{
	struct tsnode *tsnode;

	TSENSOR_TOPO_SLOCK();

	tsnode = tsnode_find_by_id(provider_dev, id);
	if (tsnode == NULL) {
		TSENSOR_TOPO_UNLOCK();
		return (ENODEV);
	}
	*tsensor = tsensor_create(tsnode, consumer_dev);
	TSENSOR_TOPO_UNLOCK();

	return (0);
}

void
tsensor_release(tsensor_t tsensor)
{
	struct tsnode *tsnode;

	tsnode = tsensor->tsnode;
	KASSERT(tsnode->ref_cnt > 0,
	   ("Attempt to access unreferenced tsensor.\n"));

	TSENSOR_TOPO_SLOCK();
	TSNODE_XLOCK(tsnode);
	TAILQ_REMOVE(&tsnode->consumers_list, tsensor, link);
	tsnode->ref_cnt--;
	TSNODE_UNLOCK(tsnode);
	TSENSOR_TOPO_UNLOCK();

	free(tsensor, M_THERMAL);
}

#ifdef FDT
int tsdev_default_ofw_map(device_t provider, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *id)
{
	struct tsnode *entry;
	phandle_t node;

	/* Single device can register multiple subnodes. */
	if (ncells == 0) {

		node = OF_node_from_xref(xref);
		TSENSOR_TOPO_XLOCK();
		TAILQ_FOREACH(entry, &tsnode_list, tslist_link) {
			if ((entry->pdev == provider) &&
			    (entry->ofw_node == node)) {
				*id = entry->id;
				TSENSOR_TOPO_UNLOCK();
				return (0);
			}
		}
		TSENSOR_TOPO_UNLOCK();
		return (ERANGE);
	}

	/* First cell is ID. */
	if (ncells == 1) {
		*id = cells[0];
		return (0);
	}

	/* No default way how to get ID, custom mapper is required. */
	return  (ERANGE);
}

int
tsensor_get_by_ofw_idx(device_t consumer_dev, phandle_t cnode, int idx, tsensor_t *tsensor)
{
	phandle_t xnode;
	pcell_t *cells;
	device_t tsdev;
	int ncells, rv;
	intptr_t id;

	if (cnode <= 0)
		cnode = ofw_bus_get_node(consumer_dev);
	if (cnode <= 0) {
		device_printf(consumer_dev,
		    "%s called on not ofw based device\n", __func__);
		return (ENXIO);
	}
	rv = ofw_bus_parse_xref_list_alloc(cnode, "thermal-sensors",
	     "#thermal-sensor-cells", idx, &xnode, &ncells, &cells);
	if (rv != 0)
		return (rv);

	/* Tranlate provider to device. */
	tsdev = OF_device_from_xref(xnode);
	if (tsdev == NULL) {
		OF_prop_free(cells);
		return (ENODEV);
	}
	/* Map tsensor to number. */
	rv = TSDEV_MAP(tsdev, xnode, ncells, cells, &id);
	OF_prop_free(cells);
	if (rv != 0)
		return (rv);

	return (tsensor_get_by_id(consumer_dev, tsdev, id, tsensor));
}

int
tsensor_get_by_ofw_name(device_t consumer_dev, phandle_t cnode, char *name,
    tsensor_t *tsensor)
{
	int rv, idx;

	if (cnode <= 0)
		cnode = ofw_bus_get_node(consumer_dev);
	if (cnode <= 0) {
		device_printf(consumer_dev,
		    "%s called on not ofw based device\n",  __func__);
		return (ENXIO);
	}
	rv = ofw_bus_find_string_index(cnode, "thermal-sensor-names", name,
	    &idx);
	if (rv != 0)
		return (rv);
	return (tsensor_get_by_ofw_idx(consumer_dev, cnode, idx, tsensor));
}

int
tsensor_get_by_ofw_property(device_t consumer_dev, phandle_t cnode, char *name,
    tsensor_t *tsensor)
{
	pcell_t *cells;
	device_t tsdev;
	int ncells, rv;
	intptr_t id;

	if (cnode <= 0)
		cnode = ofw_bus_get_node(consumer_dev);
	if (cnode <= 0) {
		device_printf(consumer_dev,
		    "%s called on not ofw based device\n", __func__);
		return (ENXIO);
	}
	ncells = OF_getencprop_alloc_multi(cnode, name, sizeof(pcell_t),
	    (void **)&cells);
	if (ncells < 1)
		return (ENOENT);

	/* Tranlate provider to device. */
	tsdev = OF_device_from_xref(cells[0]);
	if (tsdev == NULL) {
		OF_prop_free(cells);
		return (ENODEV);
	}
	/* Map tsensor to number. */
	rv = TSDEV_MAP(tsdev, cells[0], ncells - 1 , cells + 1, &id);
	OF_prop_free(cells);
	if (rv != 0)
		return (rv);

	return (tsensor_get_by_id(consumer_dev, tsdev, id, tsensor));
}
#endif
