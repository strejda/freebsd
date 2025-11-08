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

/*
 * RK806 PMIC driver
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/sx.h>

#include <machine/bus.h>

#include <dev/regulator/regulator.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/spibus/spi.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>


#include "regdev_if.h"
#include "spibus_if.h"

#include "rk806.h"

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk806",	1},
	{NULL,			0},
};

#define	LOCK(_sc)		sx_xlock(&(_sc)->lock)
#define	UNLOCK(_sc)		sx_xunlock(&(_sc)->lock)
#define	LOCK_INIT(_sc)		sx_init(&(_sc)->lock, "rk806")
#define	LOCK_DESTROY(_sc)	sx_destroy(&(_sc)->lock);
#define	ASSERT_LOCKED(_sc)	sx_assert(&(_sc)->lock, SA_XLOCKED);
#define	ASSERT_UNLOCKED(_sc)	sx_assert(&(_sc)->lock, SA_UNLOCKED);

/* SPI command */
#define RK806_CMD_READ		(0 << 7) 
#define RK806_CMD_WRITE		(1 << 7)

#define RK806_CMD_SIZE		0x0F

#define RK806_CMD(CMD, SIZE)	(RK806_CMD_##CMD | (SIZE - 1))

#define RK806_CHIP_NAME_ID	0x806

/*
 * Raw register access function.
 */
int
rk806_read(struct rk806_softc *sc, uint8_t reg, uint8_t *val)
{
	uint8_t tx_buf[3];
	struct spi_command cmd;
	int rv;

	memset(&cmd, 0, sizeof(cmd));
	memset(&tx_buf, 0, sizeof(tx_buf));
	cmd.tx_cmd = &tx_buf;
	cmd.tx_cmd_sz = sizeof(tx_buf);
	cmd.rx_data = val;
	cmd.rx_data_sz = sizeof(*val);

	tx_buf[0] = RK806_CMD(READ, sizeof(*val));
	tx_buf[1] = reg;
	tx_buf[2] = 0;

	rv = SPIBUS_TRANSFER(device_get_parent(sc->dev), sc->dev, &cmd);
	if (rv != 0) {
		device_printf(sc->dev,
		    "Error when reading reg 0x%02X, rv: %d\n", reg,  rv);
		return (EIO);
	}
	return (0);
}

int
rk806_write(struct rk806_softc *sc, uint8_t reg, uint8_t val)
{
	uint8_t tx_buf[4];
	struct spi_command cmd;
	int rv;

	memset(&cmd, 0, sizeof(cmd));
	memset(&tx_buf, 0, sizeof(tx_buf));
	cmd.tx_cmd = &tx_buf;
	cmd.tx_cmd_sz = sizeof(tx_buf);

	tx_buf[0] = RK806_CMD(WRITE, sizeof(val));
	tx_buf[1] = reg;
	tx_buf[2] = 0;
	tx_buf[3] = val;

	rv = SPIBUS_TRANSFER(device_get_parent(sc->dev), sc->dev, &cmd);
	if (rv != 0) {
		device_printf(sc->dev,
		    "Error when writing reg 0x%02X, rv: %d\n", reg, rv);
		return (EIO);
	}
	return (0);
}

int
rk806_modify(struct rk806_softc *sc, uint8_t reg, uint8_t clear, uint8_t set)
{
	uint8_t val;
	int rv;

	rv = rk806_read(sc, reg, &val);
	if (rv != 0)
		return (rv);

	val &= ~clear;
	val |= set;

	rv = rk806_write(sc, reg, val);
	if (rv != 0)
		return (rv);

	return (0);
}

static int
rk806_get_version(struct rk806_softc *sc)
{
	uint8_t reg1, reg2, otp;
	int rv;

	/* Verify RK806 ID and version. */
	rv = RD1(sc, RK806_CHIP_NAME, &reg1);
	if (rv != 0)
		return (ENXIO);
	rv = RD1(sc, RK806_CHIP_VER, &reg2);
	if (rv != 0)
		return (ENXIO);
	rv = RD1(sc, RK806_OTP_VER, &otp);
	if (rv != 0)
		return (ENXIO);
		
	sc->chip_name = (uint16_t)reg1 << 4;
	sc->chip_name |= (uint16_t)reg2 >> 4;
	sc->chip_ver = reg2 & 0x0F;
	sc->chip_ver = otp & 0x0F;
	
	if (sc->chip_name != RK806_CHIP_NAME_ID) {
		device_printf(sc->dev, "Invalid chip, ID is 0x%X\n",
		    sc->chip_name);
		return (ENXIO);
	}

	if (bootverbose)
		device_printf(sc->dev, "RK806 ver: 0x%X, opt: 0x%X\n",
		    sc->chip_ver, sc->otp_ver);
	return (0);
}

static int
rk806_init(struct rk806_softc *sc)
{
#if 0
	uint32_t reg;
	int rv;

	reg = 0;
	if (sc->int_pullup)
		reg |= RK806_INT_PULL_UP;
	if (sc->i2c_pullup)
		reg |= RK806_I2C_PULL_UP;

	rv = RM1(sc, RK806_IO_VOLTAGE,
	    RK806_INT_PULL_UP | RK806_I2C_PULL_UP, reg);
	if (rv != 0)
		return (ENXIO);

	/* mask interrupts */
	rv = WR1(sc, RK806_INTERRUPT_MASK1, 0);
	if (rv != 0)
		return (ENXIO);
	rv = WR1(sc, RK806_INTERRUPT_MASK2, 0);
	if (rv != 0)
		return (ENXIO);
	rv = WR1(sc, RK806_INTERRUPT_MASK3, 0);
	if (rv != 0)
		return (ENXIO);
	rv = WR1(sc, RK806_INTERRUPT_MASK4, 0);
	if (rv != 0)
		return (ENXIO);
#endif
	return (0);
}


static void
rk806_intr(void *arg)
{
	/* XXX Finish temperature alarms. */
}

static int
rk806_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data)
		return (ENXIO);

	device_set_desc(dev, "RK806 PMIC");
	return (BUS_PROBE_DEFAULT);
}

static int
rk806_attach(device_t dev)
{
	struct rk806_softc *sc;
	int rv, rid;
	phandle_t node;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(sc->dev);
	rv = 0;

	LOCK_INIT(sc);

	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "Cannot allocate interrupt.\n");
		rv = ENXIO;
		goto fail;
	}

	rv = rk806_get_version(sc);
	if (rv != 0)
		goto fail;
	rv = rk806_init(sc);
	if (rv != 0)
		goto fail;
	rv = rk806_regulator_attach(sc, node);
	if (rv != 0)
		goto fail;
	rv = rk806_gpio_attach(sc, node);
	if (rv != 0)
		goto fail;

	fdt_pinctrl_register(dev, NULL);
	fdt_pinctrl_configure_by_name(dev, "default");

	/* Setup  interrupt. */
	rv = bus_setup_intr(dev, sc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, rk806_intr, sc, &sc->irq_h);
	if (rv) {
		device_printf(dev, "Cannot setup interrupt.\n");
		goto fail;
	}
	bus_attach_children(dev);
	return (0);

fail:
	if (sc->irq_h != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_h);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	LOCK_DESTROY(sc);
	return (rv);
}

static int
rk806_detach(device_t dev)
{
	struct rk806_softc *sc;
	int error;

	error = bus_generic_detach(dev);
	if (error != 0)
		return (error);

	sc = device_get_softc(dev);
	if (sc->irq_h != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_h);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	LOCK_DESTROY(sc);

	return (0);
}

static phandle_t
rk806_gpio_get_node(device_t bus, device_t dev)
{

	/* We only have one child, the GPIO bus, which needs our own node. */
	return (ofw_bus_get_node(bus));
}

static device_method_t rk806_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk806_probe),
	DEVMETHOD(device_attach,	rk806_attach),
	DEVMETHOD(device_detach,	rk806_detach),

	/* Regdev interface */
	DEVMETHOD(regdev_map,		rk806_regulator_map),

	/* GPIO protocol interface */
	DEVMETHOD(gpio_get_bus,		rk806_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,		rk806_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	rk806_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags,	rk806_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps,	rk806_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags,	rk806_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		rk806_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,		rk806_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,	rk806_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios,	rk806_gpio_map_gpios),

	/* fdt_pinctrl interface */
	DEVMETHOD(fdt_pinctrl_configure, rk806_pinmux_configure),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_node,	rk806_gpio_get_node),

	DEVMETHOD_END
};
extern driver_t ofw_gpiobus_driver, gpioc_driver;
static DEFINE_CLASS_0(rk806_pmic, rk806_driver, rk806_methods,
    sizeof(struct rk806_softc));
EARLY_DRIVER_MODULE(rk806_pmic, spibus, rk806_driver, NULL, NULL, 74);
EARLY_DRIVER_MODULE(ofw_gpiobus, rk806_pmic, ofw_gpiobus_driver,
    0, 0, BUS_PASS_BUS + 1);
DRIVER_MODULE(gpioc, rk806_pmic, gpioc_driver, 0, 0);

