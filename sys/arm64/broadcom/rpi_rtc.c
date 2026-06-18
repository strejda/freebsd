/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Michal Meloun <mmel@FreeBSD.org>
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/clock.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm/broadcom/bcm2835/bcm2835_firmware.h>

#include "clock_if.h"

struct rpi_fw_rtc_sysctl;

struct rpi_fw_rtc_softc {
	device_t		dev;
	device_t		firm_dev;
	struct sx		sx;
	struct rpi_fw_rtc_sysctl *sysctl_args;
	uint32_t		bbat_chhg;
};

struct rpi_fw_rtc_sysctl_desc {
	uint32_t		reg;
	bool			rw;
	char			*name;
	char			*unit;
	char			*help;
};

struct rpi_fw_rtc_sysctl {
	struct rpi_fw_rtc_softc	*sc;
	struct rpi_fw_rtc_sysctl_desc *desc;
};

#define	LOCK(_sc)	sx_xlock(&(_sc)->sx)
#define	UNLOCK(_sc)	sx_xunlock(&(_sc)->sx)
#define	LOCK_INIT(_sc)	sx_init(&(_sc)->sx, device_get_nameunit(_sc->dev))
#define	LOCK_DESTROY(_sc) sx_destroy(&(_sc)->sx)

static struct ofw_compat_data compat_data[] = {
	{"raspberrypi,rpi-rtc",		1},
	{NULL,				0}
};


static struct rpi_fw_rtc_sysctl_desc rpi_fw_rtc_sysctl_tbl[] = {
 {
	.reg = BCM2835_FIRMWARE_RTC_BBAT_CHG_VOLTS, .rw = true,
	.name = "bbat_chg", .unit = "UI",
	.help = "Trickle charge voltage for battery (uV)"
 },
{
	.reg = BCM2835_FIRMWARE_RTC_BBAT_CHG_VOLTS_MIN, .rw = false,
	.name = "bbat_chg_min", .unit = "UI",
	.help = "Minimum supported trickle charge voltage for battery (uV)"
 },
 {
	.reg = BCM2835_FIRMWARE_RTC_BBAT_CHG_VOLTS_MAX, .rw = false,
	.name = "bbat_chg_max", .unit = "UI",
	.help = "Maximum supported trickle charge voltage for battery (uV)"
 },
 {
	.reg = BCM2835_FIRMWARE_RTC_BBAT_VOLTS, .rw = false,
	.name = "bbat_voltage", .unit = "UI",
	.help = "Current battery voltage (uV)"
 },
};

static int
rpi_fw_rtc_get_reg(struct rpi_fw_rtc_softc *sc, uint32_t reg, uint32_t *val)
{
	union msg_rtcbuf msg;
	int rv;

	msg.req.reg = reg;
	msg.req.val = 0;

	LOCK(sc);
	rv = bcm2835_firmware_property(sc->firm_dev,
	    BCM2835_FIRMWARE_GET_RTC_REG, &msg, sizeof(msg));
	UNLOCK(sc);

	if (rv != 0)
		return (rv);

	*val = msg.resp.val;
	return (0);
}

static int
rpi_fw_rtc_set_reg(struct rpi_fw_rtc_softc *sc, uint32_t reg, uint32_t val)
{
	union msg_rtcbuf msg;
	int rv;

	msg.req.reg = reg;
	msg.req.val = val;

	LOCK(sc);
	rv = bcm2835_firmware_property(sc->firm_dev,
	    BCM2835_FIRMWARE_SET_RTC_REG, &msg, sizeof(msg));
	UNLOCK(sc);

	if (rv == 0 && msg.resp.reg != reg)
		return (EIO);
	return (rv);
}


static int
rpi_fw_rtc_sysctl_proc(SYSCTL_HANDLER_ARGS)
{
	struct rpi_fw_rtc_softc *sc;
	struct rpi_fw_rtc_sysctl *arg;
	struct rpi_fw_rtc_sysctl_desc *desc;
	uint32_t val;
	int rv;

	arg = (struct rpi_fw_rtc_sysctl *)arg1;
	sc = arg->sc;
	desc = arg->desc;

	rv = rpi_fw_rtc_get_reg(sc, desc->reg, &val);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot get RTC register %d: %d\n",
		     desc->reg, rv);
		return (rv);
	}

	rv = sysctl_handle_int(oidp, &val, sizeof(val), req);
	if (rv != 0) {
		device_printf(sc->dev,
		    "sysctl_handle_int failed for register %d: %d\n",
		    desc->reg, rv);
		return (rv);
	}

	if (req->newptr != NULL) {
		rv = rpi_fw_rtc_set_reg(sc, desc->reg, val);
		if (rv != 0) {
			device_printf(sc->dev,
			    "Cannot get RTC register %d: `%d\n", desc->reg, rv);
			return (rv);
		}
	}
	return (rv);
}

static int
rpi_fw_rtc_init_sysctl(struct rpi_fw_rtc_softc *sc)
{
	int i;

	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree_node;
	struct sysctl_oid_list *tree;
	struct rpi_fw_rtc_sysctl_desc *desc;
	struct rpi_fw_rtc_sysctl *arg;

	sc->sysctl_args =  malloc(
	    nitems(rpi_fw_rtc_sysctl_tbl) * sizeof(*sc->sysctl_args),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	ctx = device_get_sysctl_ctx(sc->dev);
	tree_node = device_get_sysctl_tree(sc->dev);
	tree = SYSCTL_CHILDREN(tree_node);
	for (i = 0; i < nitems(rpi_fw_rtc_sysctl_tbl); i++) {
		desc = rpi_fw_rtc_sysctl_tbl + i;
		arg = sc->sysctl_args + i;
		arg->sc = sc;
		arg->desc = desc;
		if (desc->rw) {
			SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, desc->name,
			    CTLFLAG_RW | CTLTYPE_UINT | CTLFLAG_MPSAFE, arg, 0,
			    rpi_fw_rtc_sysctl_proc, desc->unit, desc->help);
		} else {
			SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, desc->name,
			    CTLFLAG_RD | CTLTYPE_UINT | CTLFLAG_MPSAFE, arg, 0,
			    rpi_fw_rtc_sysctl_proc, desc->unit, desc->help);
		}
	}
	return (0);
}

static int
rpi_fw_rtc_gettime(device_t dev, struct timespec *ts)
{
	struct rpi_fw_rtc_softc *sc;
	uint32_t sec;
	int rv;

	sc = device_get_softc(dev);

	rv = rpi_fw_rtc_get_reg(sc, BCM2835_FIRMWARE_RTC_TIME, &sec);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot get RTC time: %d\n", rv);
		return (rv);
	}

	ts->tv_sec = sec;
	ts->tv_nsec = 0;
	return (0);
}

static int
rpi_fw_rtc_settime(device_t dev, struct timespec *ts)
{
	struct rpi_fw_rtc_softc *sc;
	int rv;

	sc = device_get_softc(dev);


	rv = rpi_fw_rtc_set_reg(sc, BCM2835_FIRMWARE_RTC_TIME,
	    (uint32_t)ts->tv_sec);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot set RTC time: %d\n", rv);
		return (rv);
	}

	return (0);
}

static int
rpi_fw_rtc_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Raspberry Pi Firmware RTC");
	return (BUS_PROBE_DEFAULT);
}

static int
rpi_fw_rtc_attach(device_t dev)
{
	struct rpi_fw_rtc_softc *sc;
	phandle_t node;
	int rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->firm_dev = device_get_parent(dev);
	node = ofw_bus_get_node(dev);

	LOCK_INIT(sc);

	/* trickle charging - optional, disabled by default */
	if (!OF_hasprop(node, "trickle-charge-microvolt")) {
		rv = OF_getencprop(node, "trickle-charge-microvolt",
		    &sc->bbat_chhg, sizeof(sc->bbat_chhg));
		if (rv <= 0) {
			device_printf(sc->dev,
			    "Malformed 'trickle-charge-microvolt' "
			    "propery: %d\n", rv);
			goto fail;
		}
	} else {
		sc->bbat_chhg = 0;
	}
	rv = rpi_fw_rtc_set_reg(sc, BCM2835_FIRMWARE_RTC_BBAT_CHG_VOLTS,
	     sc->bbat_chhg);
	if (rv !=0) {
		device_printf(sc->dev,
		    "Cannot set 'trickle-charge-microvolt': %d\n", rv);
		goto fail;
	}
	rv =  rpi_fw_rtc_init_sysctl(sc);
	if (rv !=0)
		device_printf(sc->dev,
		    "Unable to initialize sysctls : %d\n", rv);

	clock_register(dev, 1000000);

	bus_attach_children(dev);
	return (0);

fail:
	LOCK_DESTROY(sc);

	return (ENXIO);
}

static int
rpi_fw_rtc_detach(device_t dev)
{
	struct rpi_fw_rtc_softc *sc;

	sc = device_get_softc(dev);

	clock_unregister(dev);
	LOCK_DESTROY(sc);
	return (0);
}

static device_method_t rpi_fw_rtc_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe,		rpi_fw_rtc_probe),
	DEVMETHOD(device_attach,	rpi_fw_rtc_attach),
	DEVMETHOD(device_detach,	rpi_fw_rtc_detach),

	/* clock interface */
	DEVMETHOD(clock_gettime,	rpi_fw_rtc_gettime),
	DEVMETHOD(clock_settime,	rpi_fw_rtc_settime),

	DEVMETHOD_END
};



static DEFINE_CLASS_0(rtc, rpi_fw_rtc_driver, rpi_fw_rtc_methods,
    sizeof(struct rpi_fw_rtc_softc));

DRIVER_MODULE(rpi_fw_rtc, bcm2835_firmware, rpi_fw_rtc_driver, 0, 0);
