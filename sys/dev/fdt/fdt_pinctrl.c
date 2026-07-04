/*-
 * Copyright (c) 2014 Ian Lepore <ian@freebsd.org>
 * All rights reserved.
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
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "fdt_pinctrl_if.h"

#include <dev/fdt/fdt_common.h>
#include <dev/fdt/fdt_pinctrl.h>

#if 1
#define dprintf(dev, ...)	device_printf(dev, ##__VA_ARGS__)
#else
#define dprintf(dev, ...)
#endif

MALLOC_DEFINE(M_PINCTRL, "pinctrl", "FDT pinctrl");

struct fdt_gpio_range {
	device_t	pinctrl_dev;
	uint32_t 	gpio_base;	/* gpio base */
	uint32_t 	pctrl_base;	/* pinctrls base */
	uint32_t  	npins;		/* number of pins in range */
};


struct fdt_pinctrl_range {
	device_t	pinctrl_dev;		/* pinctrl device */
	char		*name;			/* name of pinctrl */
	uint32_t 	gpio_base;		/* gpio base */
	uint32_t 	pctrl_base;		/* pinctrls base */
	uint32_t  	npins;
	SLIST_ENTRY(fdt_pinctrl_range) next;
};
static SLIST_HEAD(, fdt_pinctrl_range)
    fdt_pinctrl_range_list = SLIST_HEAD_INITIALIZER(fdt_pinctrl_range_list);

static struct mtx pinctrl_mtx;
MTX_SYSINIT(pinctrl_mtx, &pinctrl_mtx, "pinctrl mutex", MTX_DEF);

int
fdt_pinctrl_configure(device_t client, u_int index)
{
	device_t pinctrl;
	phandle_t *configs;
	int i, nconfigs;
	char name[16];

	snprintf(name, sizeof(name), "pinctrl-%u", index);
	nconfigs = OF_getencprop_alloc_multi(ofw_bus_get_node(client), name,
	    sizeof(*configs), (void **)&configs);
	if (nconfigs < 0)
		return (ENOENT);
	if (nconfigs == 0)
		return (0); /* Empty property is documented as valid. */
	for (i = 0; i < nconfigs; i++) {
		if ((pinctrl = OF_device_from_xref(configs[i])) != NULL)
			FDT_PINCTRL_CONFIGURE(pinctrl, configs[i]);
	}
	OF_prop_free(configs);
	return (0);
}

int
fdt_pinctrl_configure_by_name(device_t client, const char * name)
{
	char * names;
	int i, offset, nameslen;

	nameslen = OF_getprop_alloc(ofw_bus_get_node(client), "pinctrl-names",
	    (void **)&names);
	if (nameslen <= 0)
		return (ENOENT);
	for (i = 0, offset = 0; offset < nameslen; i++) {
		if (strcmp(name, &names[offset]) == 0)
			break;
		offset += strlen(&names[offset]) + 1;
	}
	OF_prop_free(names);
	if (offset < nameslen)
		return (fdt_pinctrl_configure(client, i));
	else
		return (ENOENT);
}

static int
pinctrl_register_children(device_t pinctrl, phandle_t parent,
    const char *pinprop)
{
	phandle_t node;

	/*
	 * Recursively descend from parent, looking for nodes that have the
	 * given property, and associate the pinctrl device_t with each one.
	 */
	for (node = OF_child(parent); node != 0; node = OF_peer(node)) {
		pinctrl_register_children(pinctrl, node, pinprop);
		if (pinprop == NULL || OF_hasprop(node, pinprop)) {
			OF_device_register_xref(OF_xref_from_node(node),
			    pinctrl);
		}
	}
	return (0);
}

int
fdt_pinctrl_register(device_t pinctrl, const char *pinprop)
{
	phandle_t node;
	int ret;

	TSENTER();
	node = ofw_bus_get_node(pinctrl);

	OF_device_register_xref(OF_xref_from_node(node), pinctrl);
	ret = pinctrl_register_children(pinctrl, node, pinprop);
	TSEXIT();

	return (ret);
}


static int
pinctrl_configure_children(device_t pinctrl, phandle_t parent)
{
	phandle_t node, *configs;
	int i, nconfigs;

	TSENTER();

	for (node = OF_child(parent); node != 0; node = OF_peer(node)) {
		if (!ofw_bus_node_status_okay(node))
			continue;
		pinctrl_configure_children(pinctrl, node);
		nconfigs = OF_getencprop_alloc_multi(node, "pinctrl-0",
		    sizeof(*configs), (void **)&configs);
		if (nconfigs <= 0)
			continue;
		if (bootverbose) {
			char name[32];
			OF_getprop(node, "name", &name, sizeof(name));
			printf("Processing %d pin-config node(s) in pinctrl-0 for %s\n",
			    nconfigs, name);
		}
		for (i = 0; i < nconfigs; i++) {
			if (OF_device_from_xref(configs[i]) == pinctrl)
				FDT_PINCTRL_CONFIGURE(pinctrl, configs[i]);
		}
		OF_prop_free(configs);
	}
	TSEXIT();
	return (0);
}

int
fdt_pinctrl_configure_tree(device_t pinctrl)
{

	return (pinctrl_configure_children(pinctrl, OF_peer(0)));
}

/*
 * Register gpio-range - old, hardwired version
 * - used  by pinctrl to register hardwired gpio range
 * - old unpreffered way, 'gpio-range' propety shoud be used in DT
 */
int
fdt_pinctrl_register_gpio_range(device_t pinctrl, const char *name,
    uint32_t gpio_base, uint32_t pctrl_base, uint32_t npins)
{
	struct fdt_pinctrl_range *range;
	char *tmp;
device_printf(pinctrl, "%s: Enter name: %s\n", __func__, name);
	range = malloc(sizeof(*range), M_PINCTRL, M_WAITOK);
	if (name != NULL) {
		tmp = malloc(strlen(name) + 1, M_PINCTRL, M_WAITOK);
		strncpy(tmp, name, strlen(name) + 1);
	}

	range->pinctrl_dev = pinctrl;
		range->name = (name != NULL) ? tmp: NULL;
	range->gpio_base = gpio_base;
	range->pctrl_base = pctrl_base;
	range->npins = npins;

	/* XXX - check duplicates ??, only in INVARIANTS ?? */
	mtx_lock(&pinctrl_mtx);
	SLIST_INSERT_HEAD(&fdt_pinctrl_range_list, range, next);
	mtx_unlock(&pinctrl_mtx);

	return (0);
}

static int
fdt_pinctrl_get_fdt_gpio_map(device_t gpio_dev, struct fdt_gpio_map *map)
{
	struct fdt_gpio_range *ranges;
	pcell_t *ranges_prop;
	phandle_t xref, node;
	int nranges, i;

	node = ofw_bus_get_node(gpio_dev);

	nranges = OF_getencprop_alloc(node, "gpio-ranges",
	    (void **)&ranges_prop);
	if (nranges == -1)
		return (ENOENT);
	if (nranges == 0 || (nranges  % (4 * sizeof(pcell_t))) != 0) {
		device_printf(gpio_dev, "Malformed 'gpio-ranges' property\n");
		OF_prop_free(ranges_prop);
		return(EINVAL);
	}
	ranges = malloc(nranges * sizeof(*ranges), M_PINCTRL, M_WAITOK);
	for (i = 0; i < nranges; i += 4) {
		xref = ranges_prop[i + 0];
		ranges[i].gpio_base = ranges_prop[i + 1];
		ranges[i].pctrl_base = ranges_prop[i + 2];
		ranges[i].npins = ranges_prop[i + 3];
		ranges[i].pinctrl_dev = OF_device_from_xref(xref);
		if (ranges[i].pinctrl_dev  == NULL) {
			OF_prop_free(ranges_prop);
			return (ENODEV);
		}
	}
	map->ranges = ranges;
	map->nranges = nranges;
	OF_prop_free(ranges_prop);
	return (0);

}

int
fdt_pinctrl_get_gpio_map(device_t gpio, const char *name, uint32_t gpio_base,
    uint32_t npins, struct fdt_gpio_map *map)
{
	int rv;
	struct fdt_pinctrl_range *range;
	struct fdt_gpio_range *gpio_range;

	dprintf(gpio, "%s: Start for name: %s, base: %d, npins: %d\n",
	    __func__, name, gpio_base, npins);

	/* Try "gpio-ranges" property on gpio node first */
	rv = fdt_pinctrl_get_fdt_gpio_map(gpio, map);
	if (rv != ENOENT)
		return (rv);

	gpio_range = malloc(sizeof(*gpio_range), M_PINCTRL, M_WAITOK);
	mtx_lock(&pinctrl_mtx);

	if (name == NULL)
		goto second_pass;

	/* first pass, try match by name and by range */
	SLIST_FOREACH(range, &fdt_pinctrl_range_list, next) {
		dprintf(gpio, "%s: testing 1 name: %s, base: %d, npins: %d\n",
		    __func__, range->name,  range->gpio_base, range->npins);
		if (range->name == NULL ||
		    strcmp(range->name, name) != 0)
			continue;

		if (range->gpio_base <= gpio_base &&
		    (range->gpio_base + range->npins) >= (gpio_base + npins)) {
			dprintf(gpio,
			    "%s: found1 name: %s, base: %d, npins: %d\n",
			    __func__, range->name, range->gpio_base,
			    range->npins);
			goto found;
		}
	}

second_pass:
	/* second pass, try match by exact range */
	SLIST_FOREACH(range, &fdt_pinctrl_range_list, next) {
		dprintf(gpio, "%s: testing2 base: %d, npins: %d\n", __func__,
		    range->gpio_base, range->npins);
		if (range->gpio_base == gpio_base &&
		    (range->gpio_base + range->npins) == (gpio_base + npins)) {
			dprintf(gpio, "%s: found2 base: %d, npins: %d\n",
			    __func__, range->gpio_base, range->npins);
			goto found;
		}
	}

	mtx_unlock(&pinctrl_mtx);
	free(gpio_range,  M_PINCTRL);

	return (ENXIO);

found:
	gpio_range->pinctrl_dev = range->pinctrl_dev;
	gpio_range->gpio_base = range->gpio_base;
	gpio_range->pctrl_base = range->pctrl_base;
	gpio_range->npins = range->npins;

	mtx_unlock(&pinctrl_mtx);

	map->ranges = gpio_range;
	map->nranges = 1;

	return (0);
}

static int
fdt_pinctrl_map_pin(struct fdt_gpio_map *map, uint32_t gpio_pin,
    device_t *pinctrl, uint32_t *pinctrl_pin)
{
	int i;
	struct fdt_gpio_range *range;

	for (i = 0; i < map->nranges; i++) {
		range = map->ranges + i;
		if (gpio_pin >= range->gpio_base &&
		    gpio_pin < (range->gpio_base + range->npins)) {
			*pinctrl = range->pinctrl_dev;
			*pinctrl_pin = range->pctrl_base +
			    gpio_pin - range->gpio_base;
			return (0);
		}

	}
	return (EINVAL);
}

int
fdt_pinctrl_is_gpio(device_t gpio, struct fdt_gpio_map *map, uint32_t pin,
    bool *is_gpio)
{
	uint32_t pinctrl_pin;
	device_t pinctrl;
	int rv;

	rv = fdt_pinctrl_map_pin(map, pin, &pinctrl, &pinctrl_pin);
	if (rv != 0)
		return (rv);

	rv = FDT_PINCTRL_IS_GPIO(pinctrl, gpio, pinctrl_pin, is_gpio);

	return (rv);
}

int
fdt_pinctrl_set_flags(device_t gpio, struct fdt_gpio_map *map, uint32_t pin,
    uint32_t flags)
{
	uint32_t pinctrl_pin;
	device_t pinctrl;
	int rv;

	rv = fdt_pinctrl_map_pin(map, pin, &pinctrl, &pinctrl_pin);
	if (rv != 0)
		return (rv);

	rv = FDT_PINCTRL_SET_FLAGS(pinctrl, gpio, pinctrl_pin, flags);

	return (rv);
}

int
fdt_pinctrl_get_flags(device_t gpio, struct fdt_gpio_map *map, uint32_t pin,
    uint32_t *flags)
{
	uint32_t pinctrl_pin;
	device_t pinctrl;
	int rv;

	rv = fdt_pinctrl_map_pin(map, pin, &pinctrl, &pinctrl_pin);
	if (rv != 0)
		return (rv);

	rv = FDT_PINCTRL_GET_FLAGS(pinctrl, gpio, pinctrl_pin, flags);

	return (rv);
}
