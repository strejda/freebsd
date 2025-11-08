/*-
 * Copyright 2016 Michal Meloun <mmel@FreeBSD.org>
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
#include <dev/gpio/gpiobusvar.h>


#include "rk806.h"

MALLOC_DEFINE(M_RK806_REG, "RK806 regulator", "RK806 power regulator");

#define	DIV_ROUND_UP(n,d) howmany(n, d)

enum rk806_reg_id {
	RK806_ID_DCDC1,
	RK806_ID_DCDC2,
	RK806_ID_DCDC3,
	RK806_ID_DCDC4,
	RK806_ID_DCDC5,
	RK806_ID_DCDC6,
	RK806_ID_DCDC7,
	RK806_ID_DCDC8,
	RK806_ID_DCDC9,
	RK806_ID_DCDC10,

	RK806_ID_NLDO1,
	RK806_ID_NLDO2,
	RK806_ID_NLDO3,
	RK806_ID_NLDO4,
	RK806_ID_NLDO5,

	RK806_ID_PLDO1,
	RK806_ID_PLDO2,
	RK806_ID_PLDO3,
	RK806_ID_PLDO4,
	RK806_ID_PLDO5,
	RK806_ID_PLDO6,
};

/* Regulator HW definition. */
struct reg_def {
	intptr_t		id;		/* ID */
	char			*name;		/* Regulator name */
	char			*supply_name;	/* Source property name */
	uint8_t			cfg_reg;
	uint8_t			volt_reg;
	uint8_t			enable_reg;
	int			enable_bit;
	struct regulator_range	*ranges;
	int			nranges;
};


struct rk806_reg_sc {
	struct regnode		*regnode;
	struct rk806_softc	*base_sc;
	struct reg_def		*def;
	phandle_t		xref;

	struct regnode_std_param *param;
	int 			ext_control;
	int	 		enable_tracking;

	int			enable_usec;
};

static  struct regulator_range rk806_buck_ranges[] = {
	REG_RANGE_INIT(  0, 159,  500000,  6250), /* 500mV ~ 1500mV */
	REG_RANGE_INIT(160, 235, 1500000, 25000), /* 1500mV ~ 3400mV */
	REG_RANGE_INIT(236, 255, 3400000,     0),
};

static  struct regulator_range rk806_ldo_ranges[] = {
	REG_RANGE_INIT(0,   231,  500000, 12500), /* 500mV ~ 3400mV */
	REG_RANGE_INIT(232, 255, 3400000,     0),
};

static struct reg_def rk806_def[] = {
	{
		.id = RK806_ID_DCDC1,
		.name = "dcdc-reg1",
		.supply_name = "vcc1",
		.cfg_reg = RK806_BUCK1_CONFIG,
		.volt_reg = RK806_BUCK1_ON_VSEL,
		.enable_reg = RK806_POWER_EN0,
		.enable_bit = 0,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC2,
		.name = "dcdc-reg2",
		.supply_name = "vcc2",
		.cfg_reg = RK806_BUCK2_CONFIG,
		.volt_reg = RK806_BUCK2_ON_VSEL,
		.enable_reg = RK806_POWER_EN0,
		.enable_bit = 1,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC3,
		.name = "dcdc-reg3",
		.supply_name = "vcc3",
		.cfg_reg = RK806_BUCK3_CONFIG,
		.volt_reg = RK806_BUCK3_ON_VSEL,
		.enable_reg = RK806_POWER_EN0,
		.enable_bit = 2,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC4,
		.name = "dcdc-reg4",
		.supply_name = "vcc4",
		.cfg_reg = RK806_BUCK4_CONFIG,
		.volt_reg = RK806_BUCK4_ON_VSEL,
		.enable_reg = RK806_POWER_EN0,
		.enable_bit = 3,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC5,
		.name = "dcdc-reg5",
		.supply_name = "vcc5",
		.cfg_reg = RK806_BUCK5_CONFIG,
		.volt_reg = RK806_BUCK5_ON_VSEL,
		.enable_reg = RK806_POWER_EN1,
		.enable_bit = 0,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC6,
		.name = "dcdc-reg6",
		.supply_name = "vcc6",
		.cfg_reg = RK806_BUCK6_CONFIG,
		.volt_reg = RK806_BUCK6_ON_VSEL,
		.enable_reg = RK806_POWER_EN1,
		.enable_bit = 1,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC7,
		.name = "dcdc-reg7",
		.supply_name = "vcc7",
		.cfg_reg = RK806_BUCK7_CONFIG,
		.volt_reg = RK806_BUCK7_ON_VSEL,
		.enable_reg = RK806_POWER_EN1,
		.enable_bit = 2,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC8,
		.name = "dcdc-reg8",
		.supply_name = "vcc8",
		.cfg_reg = RK806_BUCK8_CONFIG,
		.volt_reg = RK806_BUCK8_ON_VSEL,
		.enable_reg = RK806_POWER_EN1,
		.enable_bit = 3,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC9,
		.name = "dcdc-reg9",
		.supply_name = "vcc9",
		.cfg_reg = RK806_BUCK9_CONFIG,
		.volt_reg = RK806_BUCK9_ON_VSEL,
		.enable_reg = RK806_POWER_EN2,
		.enable_bit = 0,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_DCDC10,
		.name = "dcdc-reg10",
		.supply_name = "vcc10",
		.cfg_reg = RK806_BUCK10_CONFIG,
		.volt_reg = RK806_BUCK10_ON_VSEL,
		.enable_reg = RK806_POWER_EN2,
		.enable_bit = 2,
		.ranges = rk806_buck_ranges,
		.nranges = nitems(rk806_buck_ranges),
	},
	{
		.id = RK806_ID_NLDO1,
		.name = "nldo-reg1",
		.supply_name = "vcc13",
		.volt_reg = RK806_NLDO1_ON_VSEL,
		.enable_reg = RK806_POWER_EN3,
		.enable_bit = 0,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_NLDO2,
		.name = "nldo-reg2",
		.supply_name = "vcc13",
		.volt_reg = RK806_NLDO2_ON_VSEL,
		.enable_reg = RK806_POWER_EN3,
		.enable_bit = 1,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_NLDO3,
		.name = "nldo-reg3",
		.supply_name = "vcc13",
		.volt_reg = RK806_NLDO3_ON_VSEL,
		.enable_reg = RK806_POWER_EN3,
		.enable_bit = 2,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_NLDO4,
		.name = "nldo-reg4",
		.supply_name = "vcc14",
		.volt_reg = RK806_NLDO4_ON_VSEL,
		.enable_reg = RK806_POWER_EN3,
		.enable_bit = 3,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_NLDO5,
		.name = "nldo-reg5",
		.supply_name = "vcc14",
		.volt_reg = RK806_NLDO5_ON_VSEL,
		.enable_reg = RK806_POWER_EN5,
		.enable_bit = 2,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO1,
		.name = "pldo-reg1",
		.supply_name = "vcc11",
		.volt_reg = RK806_PLDO1_ON_VSEL,
		.enable_reg = RK806_POWER_EN4,
		.enable_bit = 1,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO2,
		.name = "pldo-reg2",
		.supply_name = "vcc11",
		.volt_reg = RK806_PLDO2_ON_VSEL,
		.enable_reg = RK806_POWER_EN4,
		.enable_bit = 2,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO3,
		.name = "pldo-reg3",
		.supply_name = "vcc11",
		.volt_reg = RK806_PLDO3_ON_VSEL,
		.enable_reg = RK806_POWER_EN4,
		.enable_bit = 3,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO4,
		.name = "pldo-reg4",
		.supply_name = "vcc12",
		.volt_reg = RK806_PLDO4_ON_VSEL,
		.enable_reg = RK806_POWER_EN5,
		.enable_bit = 0,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO5,
		.name = "pldo-reg5",
		.supply_name = "vcc12",
		.volt_reg = RK806_PLDO5_ON_VSEL,
		.enable_reg = RK806_POWER_EN5,
		.enable_bit = 1,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
	{
		.id = RK806_ID_PLDO6,
		.name = "pldo-reg6",
		.supply_name = "vcca",
		.volt_reg = RK806_PLDO6_ON_VSEL,
		.enable_reg = RK806_POWER_EN4,
		.enable_bit = 0,
		.ranges = rk806_ldo_ranges,
		.nranges = nitems(rk806_ldo_ranges),
	},
};

struct rk806_regnode_init_def {
	struct regnode_init_def	reg_init_def;
	int 			ext_control;
	int	 		enable_tracking;
};

static int rk806_regnode_init(struct regnode *regnode);
static int rk806_regnode_enable(struct regnode *regnode, bool enable,
    int *udelay);
static int rk806_regnode_set_volt(struct regnode *regnode, int min_uvolt,
    int max_uvolt, int *udelay);
static int rk806_regnode_get_volt(struct regnode *regnode, int *uvolt);
static regnode_method_t rk806_regnode_methods[] = {
	/* Regulator interface */
	REGNODEMETHOD(regnode_init,		rk806_regnode_init),
	REGNODEMETHOD(regnode_enable,		rk806_regnode_enable),
	REGNODEMETHOD(regnode_set_voltage,	rk806_regnode_set_volt),
	REGNODEMETHOD(regnode_get_voltage,	rk806_regnode_get_volt),
	REGNODEMETHOD_END
};
DEFINE_CLASS_1(rk806_regnode, rk806_regnode_class, rk806_regnode_methods,
   sizeof(struct rk806_reg_sc), regnode_class);

static int
rk806_read_sel(struct rk806_reg_sc *sc, uint8_t *sel)
{
	int rv;

	rv = RD1(sc->base_sc, sc->def->volt_reg, sel);
	return (rv);
}

static int
rk806_write_sel(struct rk806_reg_sc *sc, uint8_t sel)
{
	int rv;

	rv = WR1(sc->base_sc, sc->def->volt_reg, sel);
	return (rv);
}

static int
rk806_reg_enable(struct rk806_reg_sc *sc)
{
	int rv;

	rv = WR1(sc->base_sc, sc->def->enable_reg,
	    1 << (sc->def->enable_bit + 4) | (1 << sc->def->enable_bit));
	return (rv);
}

static int
rk806_reg_disable(struct rk806_reg_sc *sc)
{
	int rv;

	rv = WR1(sc->base_sc, sc->def->enable_reg,
	    1 << (sc->def->enable_bit + 4) | (0 << sc->def->enable_bit));
	return (rv);
}

static int
rk806_regnode_init(struct regnode *regnode)
{
	struct rk806_reg_sc *sc;

	sc = regnode_get_softc(regnode);
	sc->enable_usec = 500;

	return (0);
}

static void
rk806_fdt_parse(struct rk806_softc *sc, phandle_t node, struct reg_def *def,
struct rk806_regnode_init_def *init_def)
{
	int rv;
	phandle_t parent, supply_node;
	char prop_name[64]; /* Maximum OFW property name length. */

	rv = regulator_parse_ofw_stdparam(sc->dev, node,
	    &init_def->reg_init_def);

	/* Get parent supply. */
	if (def->supply_name == NULL)
		 return;

	parent = OF_parent(node);
	snprintf(prop_name, sizeof(prop_name), "%s-supply",
	    def->supply_name);
	rv = OF_getencprop(parent, prop_name, &supply_node,
	    sizeof(supply_node));
	if (rv <= 0)
		return;
	supply_node = OF_node_from_xref(supply_node);
	rv = OF_getprop_alloc(supply_node, "regulator-name",
	    (void **)&init_def->reg_init_def.parent_name);
	if (rv <= 0)
		init_def->reg_init_def.parent_name = NULL;
}

static struct rk806_reg_sc *
rk806_attach(struct rk806_softc *sc, phandle_t node, struct reg_def *def)
{
	struct rk806_reg_sc *reg_sc;
	struct rk806_regnode_init_def init_def;
	struct regnode *regnode;

	bzero(&init_def, sizeof(init_def));

	rk806_fdt_parse(sc, node, def, &init_def);
	init_def.reg_init_def.id = def->id;
	init_def.reg_init_def.ofw_node = node;
	regnode = regnode_create(sc->dev, &rk806_regnode_class,
	    &init_def.reg_init_def);
	if (regnode == NULL) {
		device_printf(sc->dev, "Cannot create regulator.\n");
		return (NULL);
	}
	reg_sc = regnode_get_softc(regnode);

	/* Init regulator softc. */
	reg_sc->regnode = regnode;
	reg_sc->base_sc = sc;
	reg_sc->def = def;
	reg_sc->xref = OF_xref_from_node(node);

	reg_sc->param = regnode_get_stdparam(regnode);

	regnode_register(regnode);
	if (bootverbose) {
		int volt, rv;
		regnode_topo_slock();
		rv = regnode_get_voltage(regnode, &volt);
		if (rv == ENODEV) {
			device_printf(sc->dev,
			   " Regulator %s: parent doesn't exist yet.\n",
			   regnode_get_name(regnode));
		} else if (rv != 0) {
			device_printf(sc->dev,
			   " Regulator %s: voltage: INVALID!!!\n",
			   regnode_get_name(regnode));
		} else {
			device_printf(sc->dev,
			    " Regulator %s: voltage: %d uV\n",
			    regnode_get_name(regnode), volt);
		}
		regnode_topo_unlock();
	}

	return (reg_sc);
}

int
rk806_regulator_attach(struct rk806_softc *sc, phandle_t node)
{
	struct rk806_reg_sc *reg;
	phandle_t child, rnode;
	int i;

	rnode = ofw_bus_find_child(node, "regulators");
	if (rnode <= 0) {
		device_printf(sc->dev, " Cannot find regulators subnode\n");
		return (ENXIO);
	}

	sc->nregs = nitems(rk806_def);
	sc->regs = malloc(sizeof(struct rk806_reg_sc *) * sc->nregs,
	    M_RK806_REG, M_WAITOK | M_ZERO);

	/* Attach all known regulators if exist in DT. */
	for (i = 0; i < sc->nregs; i++) {
		child = ofw_bus_find_child(rnode, rk806_def[i].name);
		if (child == 0) {
			if (bootverbose)
				device_printf(sc->dev,
				    "Regulator %s missing in DT\n",
				    rk806_def[i].name);
			continue;
		}
		reg = rk806_attach(sc, child, rk806_def + i);
		if (reg == NULL) {
			device_printf(sc->dev, "Cannot attach regulator: %s\n",
			    rk806_def[i].name);
			return (ENXIO);
		}
		sc->regs[i] = reg;
	}

	return (0);
}


int
rk806_regulator_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *num)
{
	struct rk806_softc *sc;
	int i;

	sc = device_get_softc(dev);
	for (i = 0; i < sc->nregs; i++) {
		if (sc->regs[i] == NULL)
			continue;
		if (sc->regs[i]->xref == xref) {
			*num = sc->regs[i]->def->id;
			return (0);
		}
	}
	return (ENXIO);
}

static int
rk806_regnode_enable(struct regnode *regnode, bool val, int *udelay)
{
	struct rk806_reg_sc *sc;
	int rv;

	sc = regnode_get_softc(regnode);

	if (val)
		rv = rk806_reg_enable(sc);
	else
		rv = rk806_reg_disable(sc);
	*udelay = sc->param->enable_delay;
	return (rv);
}

static int
rk806_regnode_set_volt(struct regnode *regnode, int min_uvolt, int max_uvolt,
    int *udelay)
{
	struct rk806_reg_sc *sc;
	uint8_t sel;
	int rv;

	sc = regnode_get_softc(regnode);

	*udelay = 3 * (abs(max_uvolt) / 1000);  /* 3 uS/mV */
	rv = regulator_range_volt_to_sel8(sc->def->ranges, sc->def->nranges,
	    min_uvolt, max_uvolt, &sel);
	if (rv != 0)
		return (rv);

	rv = rk806_write_sel(sc, sel);
	return (rv);

}

static int
rk806_regnode_get_volt(struct regnode *regnode, int *uvolt)
{
	struct rk806_reg_sc *sc;
	uint8_t sel;
	int rv;

	sc = regnode_get_softc(regnode);
	rv = rk806_read_sel(sc, &sel);
	if (rv != 0)
		return (rv);

	rv = regulator_range_sel8_to_volt(sc->def->ranges, sc->def->nranges,
	    sel, uvolt);
	return (rv);
}
