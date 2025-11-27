/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Oleksandr Tymoshenko <gonzo@FreeBSD.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/resource.h>
#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/spibus/spi.h>
#include <dev/spibus/spibusvar.h>

#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>

#include "spibus_if.h"


#if 0
#define	dprintf(fmt, args...)	printf( "%s: "fmt, __func__, ##args)
#else
#define	dprintf(fmt, args...)
#endif

#define	RK_SPI_CTRLR0		0x0000
#define		CTRLR0_OPM_MASTER	(0 << 20)
#define		CTRLR0_XFM_MASK		(3 << 18)
#define		CTRLR0_XFM_TR		(0 << 18)
#define		CTRLR0_XFM_TO		(1 << 18)
#define		CTRLR0_XFM_RO		(2 << 18)
#define		CTRLR0_FRF_SPI		(0 << 16)
#define		CTRLR0_FRF_SSP		(1 << 16)
#define		CTRLR0_FRF_UWIRE	(2 << 16)
#define		CTRLR0_BHT_8BIT		(1 << 13)
#define		CTRLR0_EM_BIG		(1 << 11)
#define		CTRLR0_SSD_ONE		(1 << 10)
#define		CTRLR0_SCPOL		(1 <<  7)
#define		CTRLR0_SCPH		(1 <<  6)
#define		CTRLR0_DFS_8BIT		(1 <<  0)
#define	RK_SPI_CTRLR1		0x0004
#define	RK_SPI_ENR		0x0008
#define	RK_SPI_SER		0x000c
#define	RK_SPI_BAUDR		0x0010
#define	RK_SPI_TXFTLR		0x0014
#define	RK_SPI_RXFTLR		0x0018
#define	RK_SPI_TXFLR		0x001c
#define	RK_SPI_RXFLR		0x0020
#define	RK_SPI_SR		0x0024
#define		SR_BUSY			(1 <<  0)
#define	RK_SPI_IPR		0x0028
#define	RK_SPI_IMR		0x002c
#define		IMR_RFFIM		(1 <<  4)
#define		IMR_TFEIM		(1 <<  0)
#define	RK_SPI_ISR		0x0030
#define		ISR_RFFIS		(1 <<  4)
#define		ISR_TFEIS		(1 <<  0)
#define	RK_SPI_RISR		0x0034
#define	RK_SPI_ICR		0x0038
#define	RK_SPI_DMACR		0x003c
#define	RK_SPI_DMATDLR		0x0040
#define	RK_SPI_DMARDLR		0x0044
#define	RK_SPI_VERSION		0x0048
#define		RK_SPI_VERSION_2_1	0x05EC0002
#define		RK_SPI_VERSION_2_2	0x00110002

#define	RK_SPI_TXDR		0x0400
#define	RK_SPI_RXDR		0x0800

#define	CS_MAX			1

static struct ofw_compat_data compat_data[] = {
	{ "rockchip,rk3066-spi",		1 },
	{ "rockchip,rk3328-spi",		1 },
	{ "rockchip,rk3399-spi",		1 },
	{ "rockchip,rk3568-spi",		1 },
	{ NULL,					0 }
};

static struct resource_spec rk_spi_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE | RF_SHAREABLE },
	{ -1, 0 }
};

struct rk_spi_softc {
	device_t	dev;
	device_t	spibus;
	struct resource	*res[2];
	struct mtx	mtx;
	clk_t		clk_apb;
	clk_t		clk_spi;
	void *		intrhand;
	int		transfer;
	uint32_t	fifo_size;
	uint64_t	max_freq;

	uint32_t	intreg;
	uint8_t		*rxbuf;
	uint32_t	rxidx;
	uint8_t		*txbuf;
	uint32_t	txidx;
	uint32_t	txlen;
	uint32_t	rxlen;
};

#define	RK_SPI_LOCK(sc)			mtx_lock(&(sc)->mtx)
#define	RK_SPI_UNLOCK(sc)		mtx_unlock(&(sc)->mtx)
#define	RK_SPI_READ_4(sc, reg)		bus_read_4((sc)->res[0], (reg))
#define	RK_SPI_WRITE_4(sc, reg, val)	bus_write_4((sc)->res[0], (reg), (val))

static int rk_spi_probe(device_t dev);
static int rk_spi_attach(device_t dev);
static int rk_spi_detach(device_t dev);
static void rk_spi_intr(void *arg);

static void
rk_spi_enable_chip(struct rk_spi_softc *sc, int enable)
{

	RK_SPI_WRITE_4(sc, RK_SPI_ENR, enable ? 1 : 0);
}

static int
rk_spi_set_cs(struct rk_spi_softc *sc, uint32_t cs, bool active)
{
	uint32_t reg;

	if (cs & SPIBUS_CS_HIGH) {
		device_printf(sc->dev, "SPIBUS_CS_HIGH is not supported\n");
		return (EINVAL);
	}

	if (cs > CS_MAX)
		return (EINVAL);

	reg = RK_SPI_READ_4(sc, RK_SPI_SER);
	if (active)
		reg |= (1 << cs);
	else
		reg &= ~(1 << cs);
	RK_SPI_WRITE_4(sc, RK_SPI_SER, reg);

	return (0);
}

static void
rk_spi_hw_setup(struct rk_spi_softc *sc, uint32_t mode, uint32_t freq)
{
	uint32_t cr0;
	uint32_t div;

	cr0 =  CTRLR0_OPM_MASTER | CTRLR0_FRF_SPI |
	    CTRLR0_BHT_8BIT | CTRLR0_EM_BIG | CTRLR0_SSD_ONE |
	    CTRLR0_DFS_8BIT;

	if (mode & SPIBUS_MODE_CPHA)
		cr0 |= CTRLR0_SCPH;
	if (mode & SPIBUS_MODE_CPOL)
		cr0 |= CTRLR0_SCPOL;

	/* minimum divider is 2 */
	if (sc->max_freq < freq * 2) {
		clk_set_freq(sc->clk_spi, 2 * freq, CLK_SET_ROUND_DOWN);
		clk_get_freq(sc->clk_spi, &sc->max_freq);
	}

	div = ((sc->max_freq + freq - 1) / freq);
	div = (div + 1) & 0xfffe;
	RK_SPI_WRITE_4(sc, RK_SPI_BAUDR, div);

	RK_SPI_WRITE_4(sc, RK_SPI_CTRLR0, cr0);
}


static uint32_t
rk_spi_fifo_size(struct rk_spi_softc *sc)
{

	switch (RK_SPI_READ_4(sc, RK_SPI_VERSION)) {
	case RK_SPI_VERSION_2_1:
	case RK_SPI_VERSION_2_2:
		return (64);
	default:
		return (32);
	}
}

static void
rk_spi_empty_rxfifo(struct rk_spi_softc *sc)
{
	int32_t fifo, treshold, left;

	fifo = RK_SPI_READ_4(sc, RK_SPI_RXFLR);
	treshold = RK_SPI_READ_4(sc, RK_SPI_RXFTLR) + 1;
	left = sc->rxlen - sc->rxidx;

	dprintf("fifo: %d, treshold: %d, left: %d, rxidx: %d, rxlen: %d, "
	    "txidx: %d, txlen: %d\n", fifo, treshold, left, sc->rxidx,
	    sc->rxlen, sc->txidx, sc->txlen);

	if (left <=  fifo) {
		/* last transfer */
		fifo = left;
	} else if ((left - fifo) < treshold) {
		/*
		 * Penultimate transfer, keep exact (treshold) bytes
		 * for last transfer
		 */
		fifo = left - treshold;
	}

	while (fifo-- > 0) {
		if (sc->rxidx < sc->rxlen) {
			sc->rxbuf[sc->rxidx++] =
			    (uint8_t)RK_SPI_READ_4(sc, RK_SPI_RXDR);
		}
	}
}

static void
rk_spi_fill_txfifo(struct rk_spi_softc *sc)
{
	uint32_t txlevel;

	txlevel = RK_SPI_READ_4(sc, RK_SPI_TXFLR);

	while (sc->txidx < sc->txlen && txlevel < sc->fifo_size) {
		RK_SPI_WRITE_4(sc, RK_SPI_TXDR, sc->txbuf[sc->txidx++]);
		txlevel++;
	}

	if (sc->txidx < sc->txlen)
		sc->intreg |= IMR_TFEIM;
}

static int
rk_spi_xfer_buf(struct rk_spi_softc *sc, void *rxbuf, void *txbuf,
    uint32_t rxlen, uint32_t txlen)
{
	uint32_t cr0, cr1, sr;
	int err, i;

	dprintf("enter: rxlen: %d, txlen: %d\n", rxlen, txlen);

	if (rxlen == 0 && txlen == 0)
		return (0);

	cr0 = RK_SPI_READ_4(sc, RK_SPI_CTRLR0);
	cr0 &= ~CTRLR0_XFM_MASK;

	sc->rxbuf = rxbuf;
	sc->rxlen = rxlen;
	sc->rxidx = 0;
	sc->txbuf = txbuf;
	sc->txlen = txlen;
	sc->txidx = 0;

	cr1 = 0;
	if (rxlen == 0 && txlen == 0)
	{
		device_printf(sc->dev, "Unsupported zero transfer\n");
		return (EINVAL);
	}
	if (rxlen == 0) {
		/* TX only transfer */
		cr0 |= CTRLR0_XFM_TR;
		cr1 = 0;
	} else if (txlen == 0) {
		/* RX only transfer */
		cr0 |= CTRLR0_XFM_RO;
		cr1 = sc->rxlen;
	} else {
		if (rxlen != txlen) {
			device_printf(sc->dev,
			    "Unsupported buffer sizes rxlen != txlen \n");
			return (EINVAL);
		}
		cr0 |= CTRLR0_XFM_TR;
		cr1 = 0;
	}
	RK_SPI_WRITE_4(sc, RK_SPI_CTRLR0, cr0);
	RK_SPI_WRITE_4(sc, RK_SPI_CTRLR1, cr1);

	/* Adjust Rx FIFO threshold for short transfers */
	if (sc->rxlen != 0) {
		if (sc->rxlen < sc->fifo_size)
			RK_SPI_WRITE_4(sc, RK_SPI_RXFTLR, sc->rxlen  - 1);
		else
			RK_SPI_WRITE_4(sc, RK_SPI_RXFTLR,
			    sc->fifo_size / 2 - 1);
	};
	dprintf("SPI_CTRLR0: 0x%08X , SPI_CTRLR1: 0x%08X\n",
	    RK_SPI_READ_4(sc, RK_SPI_CTRLR0), RK_SPI_READ_4(sc, RK_SPI_CTRLR1));
	sc->intreg = 0;

	RK_SPI_WRITE_4(sc, RK_SPI_ICR, 0xFFFFFFFF);
	rk_spi_enable_chip(sc, true);
	if (sc->txlen != 0) {
		rk_spi_fill_txfifo(sc);
		sc->intreg |= IMR_TFEIM;
	}
	if (sc->rxlen != 0) {
		sc->intreg |= IMR_RFFIM;
	}
	RK_SPI_WRITE_4(sc, RK_SPI_IMR, sc->intreg);

#if 0
	{
		int i;
		printf(" tx[%d]: ",  sc->txidx);
		for (i = 0; i < sc->txlen; i++) {
			printf("0x%02X, ",  sc->txbuf[i]);
		}
		printf("\n");
	}
#endif

	err = 0;
	do {
		if (cold) {
			RK_SPI_UNLOCK(sc);
			rk_spi_intr(sc);
			RK_SPI_LOCK(sc);
		} else {
			err = msleep(sc, &sc->mtx, 0, "rk_spi", 10 * hz);
			if (err != 0) break;
		}
	} while (sc->rxidx != sc->rxlen || sc->txidx != sc->txlen);
	RK_SPI_WRITE_4(sc, RK_SPI_IMR, 0);
		if (sc->rxidx != sc->rxlen || sc->txidx != sc->txlen)
		err = EIO;
	if (err == 0) {
		/* Wait for controller to finish */
		for (i = 10000; i >0; i--) {
			sr =  RK_SPI_READ_4(sc, RK_SPI_SR);
			if ((sr & SR_BUSY) == 0)
				break;
			DELAY(10);
		}
		if (sr & SR_BUSY)
			device_printf(sc->dev, "Ending transfer but controller"
			    " is still busy:0x%08X\n", sr);
	}
	rk_spi_enable_chip(sc, false);
#if 0
	{
		int i;
		printf(" rx[%d]: ",  sc->rxidx);
		for (i = 0; i < sc->rxlen; i++) {
			printf("0x%02X, ",  sc->rxbuf[i]);
		}
		printf("\n");
	}
#endif
	dprintf("done: %d\n", err);
	return (err);
}

static int
rk_spi_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data)
		return (ENXIO);

	device_set_desc(dev, "Rockchip SPI");
	return (BUS_PROBE_DEFAULT);
}

static int
rk_spi_attach(device_t dev)
{
	struct rk_spi_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;

	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);

	if (bus_alloc_resources(dev, rk_spi_spec, sc->res) != 0) {
		device_printf(dev, "cannot allocate resources for device\n");
		error = ENXIO;
		goto fail;
	}

	if (bus_setup_intr(dev, sc->res[1], INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    rk_spi_intr, sc, &sc->intrhand)) {
		bus_release_resources(dev, rk_spi_spec, sc->res);
		device_printf(dev, "cannot setup interrupt handler\n");
		return (ENXIO);
	}

	/* Activate the module clock. */
	error = clk_get_by_ofw_name(dev, 0, "apb_pclk", &sc->clk_apb);
	if (error != 0) {
		device_printf(dev, "cannot get 'apb_pclk' clock: %d\n", error);
		goto fail;
	}
	error = clk_get_by_ofw_name(dev, 0, "spiclk", &sc->clk_spi);
	if (error != 0) {
		device_printf(dev, "cannot get 'spiclk' clock: %d\n", error);
		goto fail;
	}
	error = clk_enable(sc->clk_apb);
	if (error != 0) {
		device_printf(dev, "cannot enable 'ahb' clock: %d\n", error);
		goto fail;
	}
	error = clk_enable(sc->clk_spi);
	if (error != 0) {
		device_printf(dev, "cannot enable  'spiclk' clock: %d\n", error);
		goto fail;
	}
	error = clk_get_freq(sc->clk_spi, &sc->max_freq);
	if (error != 0) {
		device_printf(dev, "cannot get 'spiclk' frequency: %d\n", error);
		goto fail;
	}
	sc->fifo_size = rk_spi_fifo_size(sc);

	sc->spibus = device_add_child(dev, "spibus", DEVICE_UNIT_ANY);

	RK_SPI_WRITE_4(sc, RK_SPI_IMR, 0);
	RK_SPI_WRITE_4(sc, RK_SPI_DMACR, 0);
	RK_SPI_WRITE_4(sc, RK_SPI_TXFTLR, 0);

	bus_attach_children(dev);
	return (0);

fail:
	rk_spi_detach(dev);
	return (error);
}

static int
rk_spi_detach(device_t dev)
{
	struct rk_spi_softc *sc;

	sc = device_get_softc(dev);

	bus_generic_detach(sc->dev);

	if (sc->clk_spi != NULL)
		clk_release(sc->clk_spi);
	if (sc->clk_apb)
		clk_release(sc->clk_apb);

	if (sc->intrhand != NULL)
		bus_teardown_intr(sc->dev, sc->res[1], sc->intrhand);

	bus_release_resources(dev, rk_spi_spec, sc->res);
	mtx_destroy(&sc->mtx);

	return (0);
}

static void
rk_spi_intr(void *arg)
{
	struct rk_spi_softc *sc;
	uint32_t isr;

	sc = arg;

	RK_SPI_LOCK(sc);
	isr = RK_SPI_READ_4(sc, RK_SPI_ISR);
	RK_SPI_WRITE_4(sc, RK_SPI_ICR, isr);

	if (sc->txlen != 0)
		rk_spi_fill_txfifo(sc);

	rk_spi_empty_rxfifo(sc);

	/* no bytes left, disable interrupt */
	if (sc->txidx == sc->txlen && sc->rxidx == sc->rxlen){
		RK_SPI_WRITE_4(sc, RK_SPI_IMR, 0);
	}
	wakeup(sc);

	RK_SPI_UNLOCK(sc);
}

static phandle_t
rk_spi_get_node(device_t bus, device_t dev)
{

	return ofw_bus_get_node(bus);
}

static int
rk_spi_transfer(device_t dev, device_t child, struct spi_command *cmd)
{
	struct rk_spi_softc *sc;
	uint32_t cs, mode, clock;
	int err = 0;
	sc = device_get_softc(dev);

	spibus_get_cs(child, &cs);
	spibus_get_clock(child, &clock);
	spibus_get_mode(child, &mode);

	RK_SPI_LOCK(sc);
	rk_spi_hw_setup(sc, mode, clock);
	err = rk_spi_set_cs(sc, cs, true);
	if (err != 0) {
		RK_SPI_UNLOCK(sc);
		return (err);
	}

	/* Transfer command then data bytes. */
	err = rk_spi_xfer_buf(sc, cmd->rx_cmd, cmd->tx_cmd,
		    cmd->rx_cmd_sz, cmd->tx_cmd_sz);

	if (err != 0)
		goto fail;
	err = rk_spi_xfer_buf(sc, cmd->rx_data, cmd->tx_data,
		    cmd->rx_data_sz, cmd->tx_data_sz);

	if (err != 0)
		goto fail;
fail:
	rk_spi_set_cs(sc, cs, false);
	RK_SPI_UNLOCK(sc);

	return (err);
}

static device_method_t rk_spi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk_spi_probe),
	DEVMETHOD(device_attach,	rk_spi_attach),
	DEVMETHOD(device_detach,	rk_spi_detach),

	/* Bus interface */
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),
	DEVMETHOD(bus_alloc_resource,	bus_generic_alloc_resource),
	DEVMETHOD(bus_release_resource,	bus_generic_release_resource),
	DEVMETHOD(bus_activate_resource, bus_generic_activate_resource),
	DEVMETHOD(bus_deactivate_resource, bus_generic_deactivate_resource),
	DEVMETHOD(bus_adjust_resource,	bus_generic_adjust_resource),
	DEVMETHOD(bus_set_resource,	bus_generic_rl_set_resource),
	DEVMETHOD(bus_get_resource,	bus_generic_rl_get_resource),

	/* spibus_if  */
	DEVMETHOD(spibus_transfer,	rk_spi_transfer),

	/* ofw_bus_if */
	DEVMETHOD(ofw_bus_get_node,	rk_spi_get_node),

	DEVMETHOD_END
};

static driver_t rk_spi_driver = {
	"spi",
	rk_spi_methods,
	sizeof(struct rk_spi_softc),
};
EARLY_DRIVER_MODULE(rk806, simplebus, rk_spi_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
EARLY_DRIVER_MODULE(ofw_spibus, rk_spi, ofw_spibus_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
MODULE_DEPEND(rk_spi, ofw_spibus, 1, 1, 1);
OFWBUS_PNP_INFO(compat_data);
