/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2018-2019  Realtek Corporation
 *
 * Power switching of the RTL8723BS, from the rtw88 driver (mac.c), used
 * here under the BSD-3-Clause license.
 * Changes for RenkuOS by Akakor, 2026: rewritten for the rtwn driver, with
 * the power sequences run by its SDIO bus glue.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8723b/r8723b_pwrseq.h>
#include <dev/rtwn/rtl8723b/sdio/r8723bs.h>


/* The value of R92C_CR while the MAC is off. */
#define R8723BS_CR_POWER_OFF	0xea

#define R8723BS_SYS_CFG1_LDO	0x01000000
#define R8723BS_LDO_SEL		0xc3
#define R8723BS_SPS_SEL		0x83


static void
r8723bs_pre_system_cfg(struct rtwn_softc *sc)
{
	rtwn_write_1(sc, R92C_RSV_CTRL, 0);

	if (rtwn_read_4(sc, R92C_SYS_CFG) & R8723BS_SYS_CFG1_LDO)
		rtwn_write_1(sc, R92C_LDO_SWR_CTRL, R8723BS_LDO_SEL);
	else
		rtwn_write_1(sc, R92C_LDO_SWR_CTRL, R8723BS_SPS_SEL);
}


static int
r8723bs_power_switch(struct rtwn_sdio_softc *uc, bool on)
{
	int error;

	/* No interrupts while the power state changes. */
	rtwn_sdio_local_write_4(uc, RTWN_SDIO_HIMR, 0);

	if (!on)
		uc->sc_powered = false;

	error = rtwn_sdio_run_pwr_seq(uc,
	    on ? card_enable_flow_8723b : card_disable_flow_8723b);
	if (error == 0 && on)
		uc->sc_powered = true;

	if (uc->sc_intr_enabled)
		rtwn_sdio_local_write_4(uc, RTWN_SDIO_HIMR, uc->sc_himr);

	return (error);
}


int
r8723bs_power_on(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	int error;

	error = rtwn_sdio_resume_hci(uc);
	if (error != 0)
		return (error);

	r8723bs_pre_system_cfg(sc);

	/* Already on (from attach or an earlier run): switch off first. */
	if (rtwn_read_1(sc, R92C_CR) != R8723BS_CR_POWER_OFF) {
		r8723bs_power_switch(uc, false);
		r8723bs_pre_system_cfg(sc);
	}

	error = r8723bs_power_switch(uc, true);
	if (error != 0) {
		device_printf(sc->sc_dev, "power on sequence failed: %d\n",
		    error);
		return (error);
	}

	/* System configuration of the chips with an 8051. */
	rtwn_write_1(sc, R92C_CR, 0xff);
	rtwn_delay(sc, 2000);
	rtwn_write_1(sc, R92C_HWSEQ_CTRL, 0x7f);
	rtwn_delay(sc, 2000);
	rtwn_setbits_1(sc, R92C_SYS_CLKR, 0, 0x08);	/* WAKEPAD_EN */
	rtwn_setbits_2(sc, R92C_GPIO_MUXCFG, 0x1000, 0);	/* EN_SIC */
	rtwn_write_2(sc, R92C_CR, 0x2ff);

	return (0);
}


void
r8723bs_power_off(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);

	if (rtwn_read_1(sc, R92C_CR) == R8723BS_CR_POWER_OFF)
		return;

	r8723bs_power_switch(uc, false);
}


/*!	Receive DMA timer masking: clear bits 2:0 of TX_CTRL and the timeout
	in its upper half, as rtw88 does after the PHY setup.
*/
void
r8723bs_post_init(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	uint32_t value;

	r8723b_post_init(sc);

	value = rtwn_sdio_local_read_4(uc, RTWN_SDIO_TX_CTRL);
	rtwn_sdio_local_write_4(uc, RTWN_SDIO_TX_CTRL, value & 0x0000fff8);

	/* MAC Tx/Rx, security and the 32K calibration timer. */
	rtwn_setbits_2(sc, R92C_CR, 0, 0x06ff);

	device_printf(sc->sc_dev, "initialized, firmware %s (version %u), "
	    "free pages %#x\n",
	    (sc->sc_flags & RTWN_FW_LOADED) ? "running" : "not loaded",
	    sc->fwver, rtwn_sdio_local_read_4(uc, RTWN_SDIO_FREE_TXPG));
}
