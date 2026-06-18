/*
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/gpio/gpiobus_internal.h>


#include "gpio_if.h"

#include "pic_if.h"

#if 1
#define dprintf(sc, ...)	device_printf((sc)->dev, ##__VA_ARGS__)
#else
#define dprintf(sc, ...)
#endif

#define	BCM_BANK_SIZE		0x20

#define	BCM_MAX_PINS_PER_BANK	32
#define	BCM_BANK(a)	((a) / BCM_MAX_PINS_PER_BANK)
#define	BCM_MASK(a)	(1U << ((a) % BCM_MAX_PINS_PER_BANK))
#define	BCM_OFFS(a)	(BCM_BANK(a) * BCM_BANK_SIZE)

#define	BRM_GPIO_OEN(_pin)	(0x00 + BCM_OFFS(_pin))	/* output enable */
#define	BRCM_GPIO_DATA(_pin)	(0x04 + BCM_OFFS(_pin))	/* data register */
#define	BRCM_GPIO_DIR(_pin)	(0x08 + BCM_OFFS(_pin))	/* direction */
#define	BCM_INT_EDGE_CFG(_pin)	(0x0C + BCM_OFFS(_pin))	/* edge cfg */
#define	BCM_INT_EDGE_BOTH(_pin)	(0x10 + BCM_OFFS(_pin))	/* both Edges */
#define	BCM_INT_MASK(_pin)	(0x14 + BCM_OFFS(_pin))	/* int mask */
#define	BCM_INT_LEVEL(_pin)	(0x18 + BCM_OFFS(_pin))	/* level */
#define	BCM_INT_STAT(_pin)	(0x1C + BCM_OFFS(_pin))	/* int status */

#define	BRCM_GPIO_DEFAULT_CAPS	(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | 	  \
    GPIO_PIN_TRISTATE  | GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)

struct brcm_gpio_irqsrc {
	struct intr_irqsrc	bgi_isrc;
	uint32_t		bgi_irq;
	uint32_t		bgi_mode;
	uint32_t		bgi_mask;
};

struct brcm_gpio_sc {
	device_t		dev;
	device_t		busdev;
	struct mtx		mtx;
	struct resource		*mem_res;
	struct resource		*irq_res;
	void			*irq_ih;
	uint32_t		caps;
	int			max_pins;
	int			npins;
	int			nbanks;
	pcell_t 		*bank_pins;
	struct gpio_pin		**gpio_pins;
	struct gpio_pin		*pins_alloc;
	struct fdt_gpio_map 	gpio_map;
};

#define	BRCM_GPIO_LOCK(_sc)	mtx_lock(&(_sc)->mtx)
#define	BRCM_GPIO_UNLOCK(_sc)	mtx_unlock(&(_sc)->mtx)
#define	BRCM_GPIO_LOCK_ASSERT(_sc)  mtx_assert(&(_sc)->mtx, MA_OWNED)

#define	WR4(_sc, _off, _val)	bus_write_4((_sc)->mem_res, _off, _val)
#define	RD4(_sc, _off)		bus_read_4((_sc)->mem_res, _off)

static struct ofw_compat_data compat_data[] = {
	{"brcm,brcmstb-gpio",	1},
	{NULL,			0}
};


static device_t
brcm_gpio_get_bus(device_t dev)
{
	struct brcm_gpio_sc *sc;

	sc = device_get_softc(dev);

	return (sc->busdev);
}

static int
brcm_gpio_pin_max(device_t dev, int *maxpin)
{
	struct brcm_gpio_sc *sc;

	sc = device_get_softc(dev);
	*maxpin = sc->npins  - 1;
	return (0);
}

static int
brcm_gpio_check_pin(struct brcm_gpio_sc *sc, uint32_t pin)
{
	bool is_gpio;
	int rv;

	if (pin >= sc->npins || sc->gpio_pins[pin] == NULL)
		return (EINVAL);

	rv = fdt_pinctrl_is_gpio(sc->dev, &sc->gpio_map, pin, &is_gpio);
	if (rv != 0 )
		return (rv);

	if (!is_gpio)
		return (ENXIO);

	return (0);
}

static int
brcm_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct brcm_gpio_sc *sc;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}

	*caps = sc->gpio_pins[pin]->gp_caps;
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct brcm_gpio_sc *sc;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}

	memcpy(name, sc->gpio_pins[pin]->gp_name, GPIOMAXNAME);
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct brcm_gpio_sc *sc;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}

	*flags &= GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN;
	*flags |= sc->gpio_pins[pin]->gp_flags;
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct brcm_gpio_sc *sc;
	uint32_t reg;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}

	rv = fdt_pinctrl_set_flags(dev, &sc->gpio_map, pin,
	    flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN));
	if (rv != 0)
		return (rv);
	flags &= ~(GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN);

	if (flags & (GPIO_PIN_INPUT|GPIO_PIN_OUTPUT)) {
		reg = RD4(sc, BRCM_GPIO_DIR(pin));
		sc->gpio_pins[pin]->gp_flags &=
		    ~(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT);
		if (flags & GPIO_PIN_OUTPUT) {
			sc->gpio_pins[pin]->gp_flags |= GPIO_PIN_OUTPUT;
			reg |= BCM_MASK(pin);
		} else {
			sc->gpio_pins[pin]->gp_flags |= GPIO_PIN_INPUT;
			reg &= ~BCM_MASK(pin);
		}
		WR4(sc, BRCM_GPIO_DIR(pin), reg);
	}

	sc->gpio_pins[pin]->gp_flags &= ~GPIO_PIN_TRISTATE;
	reg = RD4(sc, BRM_GPIO_OEN(pin));
	if (flags & GPIO_PIN_PULLUP) {
		sc->gpio_pins[pin]->gp_flags |= GPIO_PIN_TRISTATE;
		reg |= BCM_MASK(pin);
	} else {
		reg &= ~BCM_MASK(pin);
	}
	WR4(sc, BRM_GPIO_OEN(pin), reg);

	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct brcm_gpio_sc *sc;
	uint32_t reg;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}

	reg = RD4(sc, BRCM_GPIO_DATA(pin));
	if (value)
		reg |= BCM_MASK(pin);
	else
		reg &= ~BCM_MASK(pin);
	WR4(sc, BRCM_GPIO_DATA(pin), reg);
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *val)
{
	struct brcm_gpio_sc *sc;
	uint32_t reg;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}
	reg = RD4(sc, BRCM_GPIO_DATA(pin));
	*val = (reg & BCM_MASK(pin)) ? 1 : 0;
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct brcm_gpio_sc *sc;
	uint32_t reg;
	int rv;

	sc = device_get_softc(dev);

	BRCM_GPIO_LOCK(sc);
	rv = brcm_gpio_check_pin(sc, pin);
	if (rv != 0) {
		BRCM_GPIO_UNLOCK(sc);
		return (rv);
	}
	reg = RD4(sc, BRCM_GPIO_DATA(pin));
	reg ^= BCM_MASK(pin);
	WR4(sc, BRCM_GPIO_DATA(pin), reg);
	BRCM_GPIO_UNLOCK(sc);

	return (0);
}

static int
brcm_gpio_intr(void *arg)
{
	struct brcm_gpio_sc *sc __unused;

	panic("%s: got interrupt\n", __func__);

	return (FILTER_HANDLED);
}

static int
brcm_gpio_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "BRCMSTB GPIO controller");
	return (BUS_PROBE_DEFAULT);
}

static int
brcm_gpio_attach(device_t dev)
{
	struct brcm_gpio_sc *sc;
	phandle_t node;
	const char **names;
	uint32_t reg;
	int rv, i, j, idx, nnames;

	node = ofw_bus_get_node(dev);
	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->caps = BRCM_GPIO_DEFAULT_CAPS;

	mtx_init(&sc->mtx, "brcmstb gpio", "gpio", MTX_DEF);

	if (!OF_hasprop(node, "gpio-controller")) {
		device_printf(dev, "'gpio-controller' property is missing");
		goto fail;
	}

	sc->nbanks = OF_getencprop_alloc_multi(node, "brcm,gpio-bank-widths",
	  sizeof(*(sc->bank_pins)), (void **)&sc->bank_pins);
	if (sc->nbanks <= 0) {
		device_printf(dev,
		    "Cannot parse 'brcm,gpio-bank-widths' property\n");
		goto fail;
	}
	sc->max_pins = sc->nbanks * BCM_MAX_PINS_PER_BANK;

	dprintf(sc, "Number of banks: %d\n", sc->nbanks);
	for (i = 0; i < sc->nbanks; i++) {
		dprintf(sc, "Bank[%d]: width %d\n", i, sc->bank_pins[i]);
		if (sc->bank_pins[i] == 0) {
			device_printf(dev, "WARNING: Zero sized bank %d\n", i);
		} else if (sc->bank_pins[i] > BCM_MAX_PINS_PER_BANK) {
			device_printf(dev, "Bank[%d] width is over limit: %d\n",
			    i, sc->bank_pins[i]);
			goto fail;
		}
		sc->npins += sc->bank_pins[i];
	}

	/* allocate array of pointers to pin descriptors, up to maximum index */
	sc->gpio_pins = malloc(sizeof(*sc->gpio_pins) * sc->max_pins , M_DEVBUF,
	 M_WAITOK | M_ZERO);
	/* allocate pin descriptors, only existing ones */
	sc->pins_alloc = malloc(sizeof(**sc->gpio_pins) * sc->npins, M_DEVBUF,
	    M_WAITOK | M_ZERO);
	idx = 0;
	for (i = 0; i < sc->nbanks; i++) {
		for (j = 0; j < sc->bank_pins[i]; j++) {
			sc->gpio_pins[i * BCM_MAX_PINS_PER_BANK + j] =
			    sc->pins_alloc + idx;
			idx++;
		}
	}

	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, 0,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		goto fail;
	}

	if (OF_hasprop(node, "interrupt-controller")) {
		sc->caps |= GPIO_INTR_LEVEL_LOW | GPIO_INTR_LEVEL_HIGH |
		    GPIO_INTR_EDGE_RISING | GPIO_INTR_EDGE_FALLING |
		    GPIO_INTR_EDGE_BOTH;

		sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, 0,
		    RF_ACTIVE);
		if (sc->irq_res == NULL) {
			device_printf(dev, "Cannot allocate irq resource\n");
			goto fail;
		}
#ifdef not_implemented_yet
		/* Setup the GPIO interrupt handler. */
		if (brcm_gpio_pic_attach(sc)) {
			device_printf(dev,
			    "unable to setup the gpio irq handler\n");
			goto fail;
		}
#endif
		rv = bus_setup_intr(dev, sc->irq_res,
		    INTR_TYPE_MISC | INTR_MPSAFE, brcm_gpio_intr, NULL, sc,
		    &sc->irq_ih);
		if (rv != 0) {
			device_printf(sc->dev, "Cannot setup irq\n");
			goto fail;
		}
	}

	rv = fdt_pinctrl_get_gpio_map(sc->dev, "",0, sc->npins,
	    &sc->gpio_map);
	if (rv != 0) {
		device_printf(dev, "Cannot get pinctrl  map: %d\n", rv);
		goto fail;
	}

	nnames = 0;
	names = NULL;
	if (OF_hasprop(node, "gpio-line-names")) {
		nnames = ofw_bus_string_list_to_array(node,
		    "gpio-line-names", &names);
	}
	for (i = 0; i < sc->max_pins ; i++) {
		if (sc->gpio_pins[i] == NULL)
			continue;
		sc->gpio_pins[i]->gp_pin = i;
		sc->gpio_pins[i]->gp_caps = sc->caps;

		if (i < nnames && names[i] != NULL && names[i][0] != '\0') {
			strncpy(sc->gpio_pins[i]->gp_name, names[i],
			    GPIOMAXNAME);
			sc->gpio_pins[i]->gp_name[GPIOMAXNAME - 1] = '\0';
		} else {
			snprintf(sc->gpio_pins[i]->gp_name, GPIOMAXNAME,
			    "gpio_%d.%d", BCM_BANK(i),
			    i % BCM_MAX_PINS_PER_BANK);
		}

		reg = RD4(sc, BRCM_GPIO_DIR(i));
		sc->gpio_pins[i]->gp_flags =
		   (reg & BCM_MASK(i)) != 0 ? GPIO_PIN_OUTPUT : GPIO_PIN_INPUT;

		reg = RD4(sc, BRM_GPIO_OEN(i));
		sc->gpio_pins[i]->gp_flags |=
		   (reg & BCM_MASK(i)) != 0 ? GPIO_PIN_TRISTATE : 0;
	}
	if (names != NULL)
		OF_prop_free(names);

	sc->busdev = gpiobus_add_bus(dev);
	if (sc->busdev == NULL)
		goto fail;

	bus_attach_children(dev);
	return (0);

fail:
#ifdef not_implemented_yet
	brcm_gpio_pic_detach(sc);
#endif

	if (sc->irq_ih != NULL)
	    bus_teardown_intr(dev, sc->irq_res, sc->irq_ih);
	if (sc->mem_res != NULL)
		bus_release_resource(sc->dev,  sc->mem_res);
	if (sc->irq_res != NULL)
		bus_release_resource(sc->dev,  sc->irq_res);
	mtx_destroy(&sc->mtx);

	if (sc->bank_pins != NULL)
		OF_prop_free(sc->bank_pins);

	if (sc->gpio_pins != NULL)
		free(sc->gpio_pins, M_DEVBUF);
	if (sc->pins_alloc != NULL)
		free(sc->pins_alloc, M_DEVBUF);

	return (ENXIO);
}

static int
brcm_gpio_detach(device_t dev)
{

	return (EBUSY);
}

static phandle_t
brcm_gpio_get_node(device_t bus, device_t dev)
{

	/* We only have one child, the GPIO bus, which needs our own node. */
	return (ofw_bus_get_node(bus));
}


static device_method_t brcm_gpio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		brcm_gpio_probe),
	DEVMETHOD(device_attach,	brcm_gpio_attach),
	DEVMETHOD(device_detach,	brcm_gpio_detach),

	/* Bus interface */
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),

	/* GPIO protocol */
	DEVMETHOD(gpio_get_bus,		brcm_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,		brcm_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	brcm_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags,	brcm_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps,	brcm_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags,	brcm_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		brcm_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,		brcm_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,	brcm_gpio_pin_toggle),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_node,	brcm_gpio_get_node),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(gpio, brcmstb_gpio_driver, brcm_gpio_methods,
    sizeof(struct brcm_gpio_sc));

EARLY_DRIVER_MODULE(brcmstb_gpio, simplebus, brcmstb_gpio_driver, NULL, NULL,
 BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);