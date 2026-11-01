/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	SDIO bus glue of rtwn for the RTL8723BS.

	First stage: find the chip, switch it on with the power sequence of the
	RTL8723B and log its identity. The chip is not yet attached as a network
	device.

	The SDIO register access of Realtek chips is described by the rtw88 driver
	(sdio.c and sdio.h, GPL-2.0 OR BSD-3-Clause): CMD52 and CMD53 on function
	1 reach the SDIO local registers at their offset, and the MAC registers at
	0x10000 plus their offset (bits 16:13 of the address select the block).
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <dev/sdio/sdio_haiku.h>

#include <dev/rtwn/rtl8723b/r8723b_pwrseq.h>


#define RTWN_SDIO_VENDOR_REALTEK	0x024c
#define RTWN_SDIO_DEVICE_RTL8723BS	0xb723

#define RTWN_SDIO_LOCAL_REG(x)		((x) & 0x0fff)
#define RTWN_SDIO_MAC_REG(x)		(((x) & 0xffff) | (8 << 13))

#define RTWN_SDIO_HSUS_CTRL			0x0086
#define RTWN_SDIO_HSUS_SUS_REQ		0x01
#define RTWN_SDIO_HSUS_RESUME_RDY	0x02
#define RTWN_SDIO_HIMR				0x0014

#define RTWN_REG_SYS_CFG			0x00f0
#define RTWN_REG_SYS_STATUS1		0x00f4
#define RTWN_REG_SYS_CLKR			0x0008
#define RTWN_REG_CR					0x0100

#define RTWN_SDIO_BLOCK_SIZE		512


struct rtwn_sdio_softc {
	device_t			sc_dev;
	struct sdio_func*	sc_func;
};


static uint8_t
rtwn_sdio_read_1(struct rtwn_sdio_softc* sc, uint32_t addr, int* err)
{
	return sdio_read_1(sc->sc_func, addr, err);
}


static void
rtwn_sdio_write_1(struct rtwn_sdio_softc* sc, uint32_t addr, uint8_t value,
	int* err)
{
	sdio_write_1(sc->sc_func, addr, value, err);
}


/*!	Reads a 32 bit register byte by byte: before the chip is on, only CMD52
	works (rtw88 uses CMD53 only once the chip is powered).
*/
static uint32_t
rtwn_sdio_read_4_bytes(struct rtwn_sdio_softc* sc, uint32_t addr, int* err)
{
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) {
		uint8_t byte = rtwn_sdio_read_1(sc, addr + i, err);
		if (*err != 0)
			return 0;
		value |= (uint32_t)byte << (i * 8);
	}
	return value;
}


/*!	Runs one power sequence table for the SDIO interface (the format of
	rtw88, see r8723b_pwrseq.h).
*/
static int
rtwn_sdio_run_pwr_seq(struct rtwn_sdio_softc* sc,
	const struct rtw_pwr_seq_cmd* cmd)
{
	int err = 0;

	for (; cmd->cmd != RTW_PWR_CMD_END; cmd++) {
		if ((cmd->intf_mask & RTW_PWR_INTF_SDIO_MSK) == 0
			|| (cmd->cut_mask & RTW_PWR_CUT_ALL_MSK) == 0) {
			continue;
		}
		// Test chips (cut mask TEST only) are skipped
		if (cmd->cut_mask == RTW_PWR_CUT_TEST_MSK)
			continue;

		uint32_t addr = cmd->base == RTW_PWR_ADDR_SDIO
			? RTWN_SDIO_LOCAL_REG(cmd->offset)
			: RTWN_SDIO_MAC_REG(cmd->offset);

		switch (cmd->cmd) {
			case RTW_PWR_CMD_WRITE:
			{
				uint8_t value = rtwn_sdio_read_1(sc, addr, &err);
				if (err != 0)
					return err;
				value = (value & ~cmd->mask) | (cmd->value & cmd->mask);
				rtwn_sdio_write_1(sc, addr, value, &err);
				if (err != 0)
					return err;
				break;
			}

			case RTW_PWR_CMD_POLLING:
			{
				int tries;
				for (tries = 0; tries < RTW_PWR_POLLING_CNT; tries++) {
					uint8_t value = rtwn_sdio_read_1(sc, addr, &err);
					if (err != 0)
						return err;
					if ((value & cmd->mask) == (cmd->value & cmd->mask))
						break;
					DELAY(50);
				}
				if (tries == RTW_PWR_POLLING_CNT) {
					device_printf(sc->sc_dev, "power sequence: polling "
						"%#x mask %#x for %#x timed out\n", cmd->offset,
						cmd->mask, cmd->value);
					return ETIMEDOUT;
				}
				break;
			}

			case RTW_PWR_CMD_DELAY:
				if (cmd->value == RTW_PWR_DELAY_US)
					DELAY(cmd->offset);
				else
					DELAY(cmd->offset * 1000);
				break;

			default:
				break;
		}
	}

	return 0;
}


static int
rtwn_sdio_power_on(struct rtwn_sdio_softc* sc)
{
	int err = 0;

	// Leave the SDIO suspend state (rtw88 mac.c, pre system config)
	uint8_t value = rtwn_sdio_read_1(sc,
		RTWN_SDIO_LOCAL_REG(RTWN_SDIO_HSUS_CTRL), &err);
	if (err != 0)
		return err;
	rtwn_sdio_write_1(sc, RTWN_SDIO_LOCAL_REG(RTWN_SDIO_HSUS_CTRL),
		value & ~RTWN_SDIO_HSUS_SUS_REQ, &err);
	int tries;
	for (tries = 0; tries < RTW_PWR_POLLING_CNT; tries++) {
		value = rtwn_sdio_read_1(sc,
			RTWN_SDIO_LOCAL_REG(RTWN_SDIO_HSUS_CTRL), &err);
		if (err == 0 && (value & RTWN_SDIO_HSUS_RESUME_RDY) != 0)
			break;
		DELAY(50);
	}
	if (tries == RTW_PWR_POLLING_CNT) {
		device_printf(sc->sc_dev, "SDIO does not leave suspend (HSUS %#x)\n",
			value);
		return ETIMEDOUT;
	}

	// No interrupts while the power state changes
	for (int i = 0; i < 4; i++) {
		rtwn_sdio_write_1(sc, RTWN_SDIO_LOCAL_REG(RTWN_SDIO_HIMR + i), 0,
			&err);
	}

	for (int i = 0; card_enable_flow_8723b[i] != NULL; i++) {
		err = rtwn_sdio_run_pwr_seq(sc, card_enable_flow_8723b[i]);
		if (err != 0) {
			device_printf(sc->sc_dev, "power sequence %d failed: %d\n", i,
				err);
			return err;
		}
	}

	return 0;
}


static int
rtwn_sdio_probe(device_t dev)
{
	struct sdio_attach_args* args
		= (struct sdio_attach_args*)device_get_ivars(dev);
	if (args == NULL || args->vendor != RTWN_SDIO_VENDOR_REALTEK
		|| args->device != RTWN_SDIO_DEVICE_RTL8723BS
		|| args->function != 1) {
		return ENXIO;
	}

	device_set_desc(dev, "Realtek RTL8723BS");
	return BUS_PROBE_DEFAULT;
}


static int
rtwn_sdio_attach(device_t dev)
{
	struct rtwn_sdio_softc* sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_func = sdio_get_function(dev);
	int err = 0;

	err = sdio_enable_func(sc->sc_func);
	if (err != 0) {
		device_printf(dev, "cannot enable SDIO function 1: %d\n", err);
		return ENXIO;
	}
	err = sdio_set_block_size(sc->sc_func, RTWN_SDIO_BLOCK_SIZE);
	if (err != 0)
		device_printf(dev, "cannot set the block size: %d\n", err);

	uint32_t sysCfg = rtwn_sdio_read_4_bytes(sc,
		RTWN_SDIO_MAC_REG(RTWN_REG_SYS_CFG), &err);
	device_printf(dev, "before power on: SYS_CFG %08x (err %d)\n", sysCfg, err);

	err = rtwn_sdio_power_on(sc);
	device_printf(dev, "power on: %s\n", err == 0 ? "done" : "failed");

	sysCfg = rtwn_sdio_read_4_bytes(sc, RTWN_SDIO_MAC_REG(RTWN_REG_SYS_CFG),
		&err);
	uint32_t status = rtwn_sdio_read_4_bytes(sc,
		RTWN_SDIO_MAC_REG(RTWN_REG_SYS_STATUS1), &err);
	uint32_t cr = rtwn_sdio_read_4_bytes(sc, RTWN_SDIO_MAC_REG(RTWN_REG_CR),
		&err);
	device_printf(dev, "after power on: SYS_CFG %08x (chip version field "
		"%u), SYS_STATUS1 %08x, CR %08x (err %d)\n", sysCfg,
		(sysCfg >> 12) & 0xf, status, cr, err);

	// A CMD53 read of the same register shows that block transfers work
	uint32_t sysCfg53 = 0;
	uint8_t bytes[4];
	err = sdio_read_bytes(sc->sc_func, RTWN_SDIO_MAC_REG(RTWN_REG_SYS_CFG),
		bytes, 4, true);
	if (err == 0) {
		sysCfg53 = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16)
			| ((uint32_t)bytes[3] << 24);
	}
	device_printf(dev, "SYS_CFG through CMD53: %08x (err %d)\n", sysCfg53,
		err);

	// Not a network device yet
	return ENXIO;
}


static int
rtwn_sdio_detach(device_t dev)
{
	return 0;
}


static device_method_t rtwn_sdio_methods[] = {
	DEVMETHOD(device_probe,		rtwn_sdio_probe),
	DEVMETHOD(device_attach,	rtwn_sdio_attach),
	DEVMETHOD(device_detach,	rtwn_sdio_detach),

	DEVMETHOD_END
};

static driver_t rtwn_sdio_driver = {
	"rtwn",
	rtwn_sdio_methods,
	sizeof(struct rtwn_sdio_softc)
};

DRIVER_MODULE(rtwn_sdio, sdio, rtwn_sdio_driver, NULL, NULL);
