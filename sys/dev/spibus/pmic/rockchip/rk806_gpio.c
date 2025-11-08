/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Michal Meloun <mmel@FreeBSD.org>
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
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/sx.h>

#include <machine/bus.h>

#include <dev/fdt/fdt_common.h>
#include <dev/gpio/gpiobusvar.h>

#include "rk806.h"

MALLOC_DEFINE(M_RK806_GPIO, "RK806 gpio", "RK806 GPIO");

#define	NGPIO		3

#define	GPIO_LOCK(_sc)	sx_slock(&(_sc)->gpio_lock)
#define	GPIO_UNLOCK(_sc)	sx_unlock(&(_sc)->gpio_lock)
#define	GPIO_ASSERT(_sc)	sx_assert(&(_sc)->gpio_lock, SA_LOCKED)

#define	RK806_GPIO_VAL_MASK(pin)	(1 << ((pin) + 4))
#define	RK806_GPIO_DIR_MASK(pin)	(1 << (pin))

#define	RK806_FNC_SHIFT(pin)		(4 *(pin))
#define	RK806_FNC_MASK(pin)		(7 << RK806_FNC_SHIFT(pin))

static struct {
	const char *name;
	int fnc_val;
} rk806_fnc_table[] = {
	{"pin_fun0",			0},
	{"pin_fun1",			1},
	{"pin_fun2",			2},
	{"pin_fun3",			3},
	{"pin_fun4",			4},
	{"pin_fun5",			5},
};

struct rk806_pincfg {
	char	*function;
	int	flags;
};

struct rk806_gpio_pin {
	int	pin_caps;
	char	pin_name[GPIOMAXNAME];
};


/* --------------------------------------------------------------------------
 *
 *  Pinmux functions.
 */
static int
rk806_pinmux_get_function(struct rk806_softc *sc, char *name)
{
	int i;

	for (i = 0; i < nitems(rk806_fnc_table); i++) {
		if (strcmp(rk806_fnc_table[i].name, name) == 0)
			 return (rk806_fnc_table[i].fnc_val);
	}
	return (-1);
}

static int
rk806_pinmux_config_node(struct rk806_softc *sc, char *pin_name,
    struct rk806_pincfg *cfg)
{
	uint8_t tmp;
	int rv, fnc, pin;

	for (pin = 0; pin < sc->gpio_npins; pin++) {
		if (strcmp(sc->gpio_pins[pin]->pin_name, pin_name) == 0)
			 break;
	}
	if (pin >= sc->gpio_npins) {
		device_printf(sc->dev, "Unknown pin: %s\n", pin_name);
		return (ENXIO);
	}

	if (cfg->function == NULL) {
		device_printf(sc->dev, "Function is missing for pin %s\n",
		    sc->gpio_pins[pin]->pin_name);
	}

	fnc = rk806_pinmux_get_function(sc, cfg->function);
	if (fnc == -1) {
		device_printf(sc->dev, "Unknown function %s for pin %s\n",
		    cfg->function, sc->gpio_pins[pin]->pin_name);
		return (ENXIO);
	}

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_CONFIG1, &tmp);
	if (rv != 0) {
		GPIO_UNLOCK(sc);
		return (rv);
	}

	tmp &= ~RK806_FNC_MASK(pin);
	tmp |= fnc << RK806_FNC_SHIFT(pin);

	rv = WR1(sc, RK806_PWRCTRL_CONFIG1, tmp);
	GPIO_UNLOCK(sc);

	return (rv);
}

static int
rk806_pinmux_read_node(struct rk806_softc *sc, phandle_t node,
     struct rk806_pincfg *cfg, char **pins, int *lpins)
{
	int rv;


	*lpins = OF_getprop_alloc(node, "pins", (void **)pins);
	if (*lpins <= 0)
		return (ENOENT);


	/* Read function (mux) settings. */
	rv = OF_getprop_alloc(node, "function", (void **)&cfg->function);
	if (rv <= 0)
		cfg->function = NULL;

	return (0);
}

static int
rk806_pinmux_process_node(struct rk806_softc *sc, phandle_t node)
{
	struct rk806_pincfg cfg;
	char *pins, *pname;
	int i, len, lpins, rv;

	rv = rk806_pinmux_read_node(sc, node, &cfg, &pins, &lpins);
	if (rv != 0)
		return (rv);

	len = 0;
	pname = pins;
	do {
		i = strlen(pname) + 1;
		rv = rk806_pinmux_config_node(sc, pname, &cfg);
		if (rv != 0) {
			device_printf(sc->dev,
			    "Cannot configure pin: %s: %d\n", pname, rv);
			return (rv);
		}
		len += i;
		pname += i;
	} while (len < lpins);

	if (pins != NULL)
		OF_prop_free(pins);
	if (cfg.function != NULL)
		OF_prop_free(cfg.function);

	return (0);
}

int rk806_pinmux_configure(device_t dev, phandle_t cfgxref)
{
	struct rk806_softc *sc;
	phandle_t node, cfgnode;
	int rv;

	sc = device_get_softc(dev);
	cfgnode = OF_node_from_xref(cfgxref);

	for (node = OF_child(cfgnode); node != 0; node = OF_peer(node)) {
		if (!ofw_bus_node_status_okay(node))
			continue;
		rv = rk806_pinmux_process_node(sc, node);
		if (rv != 0)
			device_printf(dev, "Failed to process pinmux");
	}
	return (0);
}


/* --------------------------------------------------------------------------
 *
 *  GPIO
 */
device_t
rk806_gpio_get_bus(device_t dev)
{
	struct rk806_softc *sc;

	sc = device_get_softc(dev);
	return (sc->gpio_busdev);
}

int
rk806_gpio_pin_max(device_t dev, int *maxpin)
{

	*maxpin = NGPIO - 1;
	return (0);
}

int
rk806_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct rk806_softc *sc;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);
	GPIO_LOCK(sc);
	*caps = sc->gpio_pins[pin]->pin_caps;
	GPIO_UNLOCK(sc);
	return (0);
}

int
rk806_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct rk806_softc *sc;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);
	GPIO_LOCK(sc);
	memcpy(name, sc->gpio_pins[pin]->pin_name, GPIOMAXNAME);
	GPIO_UNLOCK(sc);
	return (0);
}


int
rk806_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *out_flags)
{
	struct rk806_softc *sc;
	uint8_t tmp;
	int rv;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_GPIO, &tmp);
	GPIO_UNLOCK(sc);
	if (rv != 0)
		return (rv);
	if ((tmp & RK806_GPIO_DIR_MASK(pin)) == 0)
		*out_flags = GPIO_PIN_INPUT;
	else
		*out_flags = GPIO_PIN_OUTPUT;
	return (0);
}

int
rk806_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct rk806_softc *sc;
	uint8_t tmp;
	int rv;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);

	if (flags != GPIO_PIN_INPUT && flags != GPIO_PIN_OUTPUT)
		return (EINVAL);

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_GPIO, &tmp);
	if (rv != 0) {
		GPIO_UNLOCK(sc);
		return (rv);
	}
	if (flags == GPIO_PIN_INPUT)
		tmp &= ~RK806_GPIO_DIR_MASK(pin);
	else
		tmp |= RK806_GPIO_DIR_MASK(pin);
	rv = WR1(sc, RK806_PWRCTRL_GPIO, tmp);
	GPIO_UNLOCK(sc);

	return (rv);
}

int
rk806_gpio_pin_set(device_t dev, uint32_t pin, uint32_t val)
{
	struct rk806_softc *sc;
	uint8_t tmp;
	int rv;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_GPIO, &tmp);
	if (rv != 0) {
		GPIO_UNLOCK(sc);
		return (rv);
	}
	if (val == 0)
		tmp &= ~RK806_GPIO_VAL_MASK(pin);
	else
		tmp |= RK806_GPIO_VAL_MASK(pin);
	rv = WR1(sc, RK806_PWRCTRL_GPIO, tmp);
	GPIO_UNLOCK(sc);

	return (rv);
}

int
rk806_gpio_pin_get(device_t dev, uint32_t pin, uint32_t *val)
{
	struct rk806_softc *sc;
	uint8_t tmp;
	int rv;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_GPIO, &tmp);
	GPIO_UNLOCK(sc);
	if (rv != 0)
		return (rv);
	*val = tmp & RK806_GPIO_VAL_MASK(pin) ? 1: 0;
	return (0);
}

int
rk806_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct rk806_softc *sc;
	uint8_t tmp;
	int rv;

	sc = device_get_softc(dev);
	if (pin >= sc->gpio_npins)
		return (EINVAL);

	GPIO_LOCK(sc);
	rv = RD1(sc, RK806_PWRCTRL_GPIO, &tmp);
	if (rv != 0) {
		GPIO_UNLOCK(sc);
		return (rv);
	}
	tmp ^= RK806_GPIO_VAL_MASK(pin);
	rv = WR1(sc, RK806_PWRCTRL_GPIO, tmp);
	GPIO_UNLOCK(sc);
	return (rv);
}


int
rk806_gpio_map_gpios(device_t dev, phandle_t pdev, phandle_t gparent,
    int gcells, pcell_t *gpios, uint32_t *pin, uint32_t *flags)
{

	if (gcells != 2)
		return (ERANGE);
	*pin = gpios[0];
	*flags= gpios[1];
	return (0);
}


int
rk806_gpio_attach(struct rk806_softc *sc, phandle_t node)
{
	struct rk806_gpio_pin *pin;
	int i;

	sx_init(&sc->gpio_lock, "RK806 GPIO lock");
	sc->gpio_npins = NGPIO;
	sc->gpio_pins = malloc(sizeof(struct rk806_gpio_pin *) *
	    sc->gpio_npins, M_RK806_GPIO, M_WAITOK | M_ZERO);

	sc->gpio_busdev = gpiobus_add_bus(sc->dev);
	if (sc->gpio_busdev == NULL)
		return (ENXIO);
	for (i = 0; i < sc->gpio_npins; i++) {
		sc->gpio_pins[i] = malloc(sizeof(struct rk806_gpio_pin),
		    M_RK806_GPIO, M_WAITOK | M_ZERO);
		pin = sc->gpio_pins[i];
		sprintf(pin->pin_name, "gpio_pwrctrl%d", i + 1);
		pin->pin_caps = GPIO_PIN_INPUT | GPIO_PIN_OUTPUT ;
	}
	return (0);
}
