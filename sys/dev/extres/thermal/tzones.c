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
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/reboot.h>
#include <sys/taskqueue.h>

#include <dev/extres/thermal/tsensor.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#if 0
#define DPRINTF(format, arg...)						\
	device_printf(sc->dev, "%s: " format, __func__, arg)
#else
#define DPRINTF(format, arg...)
#endif



MALLOC_DECLARE(M_THERMAL);

#define TZONE_LOCK(_sc)			mtx_lock(&(_sc)->mtx)
#define TZONE_UNLOCK(_sc)		mtx_unlock(&(_sc)->mtx)
#define TZONE_LOCK_INIT(_sc)		mtx_init(&(_sc)->mtx,		   \
					    device_get_nameunit(_sc->dev), \
					    "tzone", MTX_DEF)
#define TZONE_LOCK_DESTROY(_sc)		mtx_destroy(&(_sc)->mtx);
#define TZONE_ASSERT_LOCKED(_sc)	mtx_assert(&(_sc)->mtx, MA_OWNED);
#define TZONE_ASSERT_UNLOCKED(_sc)	mtx_assert(&(_sc)->mtx, MA_NOTOWNED); 

#define TZONE_TEMP_NONE			(~0)

enum tz_trip_type {
	TRIP_NONE,
	TRIP_ACTIVE,
	TRIP_PASSIVE,
	TRIP_HOT,
	TRIP_CRITICAL
};

struct tz_cooler {
	char			*name;
	phandle_t		node;
	int			min;
	int			max;
};

struct tz_trip {
	char			*name;
	phandle_t		node;
	int			temp;
	int			hyst;
	enum tz_trip_type	type;

	bool			tripped;
};

struct tz_map {
	char			*name;
	struct tz_trip		*trip;
	int			num_coolers;
	struct tz_cooler	**cooler;
};

struct tzone {
	char			*name;
	
	/*
	 * Polling delay (in ms) for normal mode
	 * (all passive trips are cold) or passive mode
	 * (at least one passive trip is in hot state.
	 * Value of 0 disables given polling (so zone is driven by interrupts)
	 */
	int			normal_poll;
	int			passive_poll;

	/* Transform sensor reading to real zone temperature. */
	int			slope;
	int			offset;
	
	/* Config - trips data and mapping between zone and cooling device. */
	int			num_trips;
	struct tz_trip 		**trip;
	int			num_maps;
	struct tz_map		**map;

	/* Operational data */
	tsensor_t		tsensor;	
	device_t		dev;
	struct mtx		mtx;
	struct timeout_task	task;
	int			hot_trips;
	int			temp;
	int			prev_temp;
};

struct tzones_softc {
	device_t		dev;
	phandle_t 		node;
	int			num_zones;
	struct tzone		**zone;
};

static int tzones_detach(device_t dev);

/* XXX this should be moved to ofw_subr.c */
static int
ofw_count_childerns(phandle_t node)
{
	phandle_t child;
	int count;

	count = 0;
	for (child = OF_child(node); child > 0; child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		count++;
	}

	return (count);
}

static struct tz_trip *
find_trip(struct tzone *tz, phandle_t node)
{
	int i;

	for (i =0 ; i < tz->num_trips; i++) {
		if (tz->trip[i]->node == node)
			return (tz->trip[i]);
	}
	return (NULL);
}

static void
tz_normal_trip(struct tzones_softc *sc, struct tzone *tz,
    struct tz_trip *trip)
{
}

static void
tz_hot_trip(struct tzones_softc *sc, struct tzone *tz,
    struct tz_trip *trip)
{
	char stemp[16];

	if (tz->temp >= trip->temp && !trip->tripped) {
		trip->tripped = true;
		snprintf(stemp, sizeof(stemp), "%d", tz->temp / 1000);
		devctl_notify("coretemp", "Thermal", stemp,
			    "notify=0xcc");
		return;
	}
	
	if (tz->temp < (trip->temp - trip->hyst) && trip->tripped) {
		trip->tripped = false;
		return;
	}
	return;
}

static void
tz_critical_trip(struct tzones_softc *sc, struct tzone *tz,
    struct tz_trip *trip)
{
	if (tz->temp >= trip->temp && !trip->tripped) {
		trip->tripped = true;
		device_printf(sc->dev, "FATAL - system overheat, "
		    "shutdown was initiated\n");
		shutdown_nice(RB_POWEROFF);

		/* wait for real shutdown */
		pause_sbt("thermal", mstosbt(5000), 0, C_HARDCLOCK | C_CATCH);
 		return;
	}
	
	if (tz->temp < (trip->temp - trip->hyst) && trip->tripped) {
		/* this should never happen */
		trip->tripped = false;
		return;
	}
	return;
}

static void
tz_poll_task(void * arg, int pending)
{
	struct tzones_softc *sc;
	struct tzone *tz;
	struct tz_trip *trip;
	int temp, rv, i;

	tz = arg;
	sc = device_get_softc(tz->dev);

	rv = tsensor_temperature(tz->tsensor, &temp);
	if (rv != 0) {
		tz->temp  = TZONE_TEMP_NONE;
		goto next;
	}	
	/* compute temperature of hotspot */
	temp = temp * tz->slope + tz->offset;
	
	tz->prev_temp = tz->temp;
	tz->temp = temp;
	DPRINTF("polling zone: %s, temp: %d\n", tz->name, temp);
	
	/* handle all trips */
	for (i = 0; i <  tz->num_trips; i++) {
		trip = tz->trip[i];
		switch (trip->type) {
		case TRIP_CRITICAL:
			tz_critical_trip(sc, tz, trip);
			break;

		case TRIP_HOT:
			tz_hot_trip(sc, tz, trip);
			break;

		case TRIP_ACTIVE:
		case TRIP_PASSIVE:
			tz_normal_trip(sc, tz, trip);
			break;

		default:
			break;
		}
	}
next:
	/* finally, reschedule me */
	taskqueue_enqueue_timeout_sbt(taskqueue_thread, &tz->task,
	    mstosbt(tz->hot_trips > 0 ? tz->passive_poll: tz->normal_poll),
	    0, C_PREL(2));
}

static int
tz_initialize_zone(struct tzones_softc *sc, struct tzone *tz)
{

	DPRINTF("starting tzone: %s\n", tz->name);
	TZONE_LOCK_INIT(tz);
	TIMEOUT_TASK_INIT(taskqueue_thread, &tz->task, 0,
	     tz_poll_task, tz);
	     
	tz->hot_trips = 0;
	tz->prev_temp  = TZONE_TEMP_NONE;
	tz->temp  = TZONE_TEMP_NONE;
	
	/* Schedule first run after 10 ticks */
	taskqueue_enqueue_timeout(taskqueue_thread, &tz->task, 10);

	return (0);
}

/*
 * OFW related parts
 */
static int
tz_parse_cooler(struct tzones_softc *sc, struct tz_map *map, phandle_t node,
    int idx)
{
	struct tz_cooler *cooler;
	phandle_t xref;
	pcell_t	*cells;
	int rv, ncells;

	cooler =  malloc(sizeof(struct tz_cooler), M_THERMAL, M_WAITOK | M_ZERO);
	map->cooler[idx] = cooler;

	rv = ofw_bus_parse_xref_list_alloc(node , "cooling-device",
	    "#cooling-cells", idx, &xref, &ncells, &cells);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot parse 'cooling-device' "
		    "property1.\n");
		return (ENXIO);
	}
	if (ncells < 2) {
		device_printf(sc->dev, "Cannot parse 'cooling-device' "
		    "property limits2.\n");
		OF_prop_free(cells);	 
		return (ENXIO);
	}
	
	cooler->node = OF_node_from_xref(xref);
	cooler->min = cells[0];
	cooler->max = cells[1];
	OF_prop_free(cells);
	return (0);
}

static int
tz_parse_map(struct tzones_softc *sc, struct tzone *tz, phandle_t node,
    int idx)
{
	struct tz_map *map;
	phandle_t xref;
	int rv, i;

	map =  malloc(sizeof(struct tz_map), M_THERMAL, M_WAITOK | M_ZERO);
	tz->map[idx] = map;
	rv = OF_getprop_alloc(node, "name", (void **)&(map->name));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read map name.\n");
		return (ENXIO);
	}

	DPRINTF("parsing tzone: %s, map[%d]: %s\n", tz->name, idx, map->name);
	rv = OF_getencprop(node, "trip", &xref, sizeof(xref));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read 'trip' property.\n");
		return (ENXIO);
	}
	map->trip = find_trip(tz, OF_node_from_xref(xref));
	if (map->trip == NULL) {
		device_printf(sc->dev, "Cannot find 'trip' reference\n");
		return (ENXIO);
	}

	rv = ofw_bus_parse_xref_list_get_length(node , "cooling-device",
	    "#cooling-cells", &map->num_coolers);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot parse 'cooling-device' "
		    "property: %d.\n", rv);
		return (ENXIO);
	}
	map->num_coolers = ofw_count_childerns(node);
	map->cooler = malloc(sizeof(struct tz_trip *) * map->num_coolers,
	    M_THERMAL, M_WAITOK | M_ZERO);
	for(i = 0; i < map->num_coolers; i++) {
		rv = tz_parse_cooler(sc, map, node, i);
		if (rv !=  0)
			return (ENXIO);
	}	
	return (0);
}

static int
tz_parse_trip(struct tzones_softc *sc, struct tzone *tz, phandle_t node,
    int idx)
{
	struct tz_trip *trip;
	char *ttype;
	int rv;

	trip =  malloc(sizeof(struct tz_trip), M_THERMAL, M_WAITOK | M_ZERO);
	tz->trip[idx] = trip;
	rv = OF_getprop_alloc(node, "name", (void **)&(trip->name));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read trip name.\n");
		return (ENXIO);
	}
	DPRINTF("parsing zone: %s, trip[%d]: %s\n", tz->name, idx,
	    trip->name);
	trip->node = node;

	rv = OF_getencprop(node, "temperature", &trip->temp,
	    sizeof(trip->temp));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read 'temperature' "
		"property.\n");
		return (ENXIO);
	}

	rv = OF_getencprop(node, "hysteresis", &trip->hyst,
	    sizeof(trip->hyst));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read 'hysteresis' "
		"property.\n");
		return (ENXIO);
	}

	rv = OF_getprop_alloc(node, "type", (void **)&(ttype));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read trip type.\n");
		return (ENXIO);
	}

	trip->type = TRIP_NONE;
	if (strcmp(ttype, "active") == 0)
		trip->type = TRIP_ACTIVE;
	else if (strcmp(ttype, "passive") == 0)
		trip->type = TRIP_PASSIVE;
	else if (strcmp(ttype, "hot") == 0)
		trip->type = TRIP_HOT;
	else if (strcmp(ttype, "critical") == 0)
		trip->type = TRIP_CRITICAL;
	if (trip->type == TRIP_NONE) {
		device_printf(sc->dev, "Invalid trip type: %s.\n",ttype);
		OF_prop_free(ttype);
		return (ENXIO);
	}
	OF_prop_free(ttype);

	return (0);
}

static int
tz_process_zone(struct tzones_softc *sc, phandle_t node, int idx)
{
	struct tzone *tz;
	phandle_t child, trips;
	int *tmp, count, rv, i;

	tz = malloc(sizeof(struct tzone), M_THERMAL, M_WAITOK | M_ZERO);
	sc->zone[idx] = tz;

	tz->dev = sc->dev;
	rv = OF_getprop_alloc(node, "name", (void **)&(tz->name));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read zone name.\n");
		return (ENXIO);
	}

	DPRINTF("parsing tzone[%d]: %s\n",idx, tz->name);
	rv = OF_getencprop(node, "polling-normal_poll-passive",
	    &tz->passive_poll, sizeof(tz->passive_poll));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read "
		"'polling-normal_poll-passive' property.\n");
		return (ENXIO);
	}
	rv = OF_getencprop(node, "polling-normal_poll", &tz->normal_poll,
	    sizeof(tz->normal_poll));
	if (rv <= 0) {
		device_printf(sc->dev, "Cannot read 'polling-normal_poll' "
		"property.\n");
		return (ENXIO);
	}

	tz->slope = 1;
	tz->offset = 0;
	count = OF_getencprop_alloc_multi(node, "coefficients",  sizeof(int),
	    (void **)&tmp);
	if (count >= 1)
		tz->slope = tmp[0];
	if (count >= 2)
		tz->offset = tmp[1];
	OF_prop_free(tmp);


	/* Process trips  */
	trips = ofw_bus_find_child(node, "trips");
	if (trips == 0) {
		device_printf(sc->dev, "Cannot find 'trips' subnode\n");
		return (ENXIO);
	}
	tz->num_trips = ofw_count_childerns(trips);
	tz->trip = malloc(sizeof(struct tz_trip *) * tz->num_trips,
	     M_THERMAL, M_WAITOK | M_ZERO);

	i = 0;
	for (child = OF_child(trips); child > 0; child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		rv = tz_parse_trip(sc, tz, child, i++);
		if (rv !=  0)
			return (ENXIO);
	}

	/* Process cooling maps */
	child = ofw_bus_find_child(node, "cooling-maps");
	if (child == 0) {
		device_printf(sc->dev, "Cannot find 'cooling-maps' subnode\n");
		return (ENXIO);
	}

	tz->num_maps = ofw_count_childerns(node);
 	tz->map = malloc(sizeof(struct tz_map *) * tz->num_maps,
 	    M_THERMAL, M_WAITOK | M_ZERO);

	i = 0;
	for (child = OF_child(child); child > 0; child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		rv = tz_parse_map(sc, tz, child, i++);
		if (rv !=  0)
			return (ENXIO);
	}
	
	rv = tsensor_get_by_ofw_idx(sc->dev, node, 0, &tz->tsensor);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot get termal sensor for zone\n");
		return (ENXIO);
	}

	return (0);
}

static void
tzones_startup(void *arg)
{
	struct tzones_softc *sc;
	int i, rv;

	sc = arg;
	for (i = 0; i < sc->num_zones; i++) {
		rv = tz_initialize_zone(sc, sc->zone[i]);
		if (rv != 0) {
			device_printf(sc->dev, "Cannot startup zone'%s': %d\n",
			    sc->zone[i]->name, rv);
		}
	 }
}

static int
tzones_probe(device_t dev)
{
	const char *name;

	name = ofw_bus_get_name(dev);

	if (name == NULL || strcmp(name, "thermal-zones") != 0)
		return (ENXIO);

	device_set_desc(dev, "OFW thermal zones");

	return (BUS_PROBE_GENERIC);
}

static int
tzones_attach(device_t dev)
{
	struct tzones_softc *sc;
	phandle_t child;
	int rv, i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(sc->dev);

	sc->num_zones = ofw_count_childerns(sc->node);
 	sc->zone = malloc(sizeof(struct tzone *) * sc->num_zones, M_THERMAL,
	    M_WAITOK | M_ZERO);

	i = 0;
	for (child = OF_child(sc->node); child > 0; child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		rv = tz_process_zone(sc, child, i++);
		if (rv !=  0) {
			tzones_detach(dev);
			return (rv);
		}
	}

	config_intrhook_oneshot(tzones_startup, sc); 
	return (bus_generic_attach(dev));
}

static int
tzones_detach(device_t dev)
{
	return (0);
}

static device_method_t tzones_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		tzones_probe),
	DEVMETHOD(device_attach,	tzones_attach),
	DEVMETHOD(device_detach,	tzones_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(tzones, tzones_driver, tzones_methods,
    sizeof(struct tzones_softc));
DRIVER_MODULE(tzones, simplebus, tzones_driver, 0, 0);
MODULE_VERSION(tzones, 1);
