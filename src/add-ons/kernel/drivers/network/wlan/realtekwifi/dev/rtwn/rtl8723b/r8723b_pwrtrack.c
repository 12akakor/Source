/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2018-2019  Realtek Corporation
 * Copyright 2024 Fiona Klute
 *
 * Thermal power tracking of the RTL8723B: the transmit power follows the
 * temperature of the chip, by the baseband swing first and the TX power
 * index for what the swing cannot take. From the rtw88 driver
 * (rtw8723b.c, phy.c and rtw8723x.c), used here under the BSD-3-Clause
 * license.
 * Changes for RenkuOS by Akakor, 2026: rewritten for the rtwn driver, for
 * one RF path; the transmit rate is not known here, so the OFDM swing is
 * limited to its default, as the vendor driver does without a rate.
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

#include <sys/cdefs.h>
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
#include <sys/linker.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r8723b.h>
#include <dev/rtwn/rtl8192e/r92e.h>


#define R8723B_OFDM0_XA_TX_IQ_IMB	0xc80
#define R8723B_OFDM0_ECCA_THRESHOLD	0xc4c
#define R8723B_TXIQK_MATRIXA_LSB2	0xc94
#define R8723B_CCK_SWING		0xa22	/* 8 registers */

#define R8723B_PT_TBL_SIZE		30
#define R8723B_PT_LIMIT_CCK		28	/* -2 dB */
#define R8723B_PT_DEFAULT_OFDM		30	/* +0 dB */
#define R8723B_PT_DEFAULT_CCK		20	/* -6 dB */


/* Full values of 0xc80 without IQ calibration, -15 to +6 dB in 0.5 dB. */
static const uint32_t r8723b_ofdm_swing[] = {
	0x0b40002d, 0x0c000030, 0x0cc00033, 0x0d800036, 0x0e400039,
	0x0f00003c, 0x10000040, 0x11000044, 0x12000048, 0x1300004c,
	0x14400051, 0x15800056, 0x16c0005b, 0x18000060, 0x19800066,
	0x1b00006c, 0x1c800072, 0x1e400079, 0x20000080, 0x22000088,
	0x24000090, 0x26000098, 0x288000a2, 0x2ac000ab, 0x2d4000b5,
	0x300000c0, 0x32c000cb, 0x35c000d7, 0x390000e4, 0x3c8000f2,
	0x40000100, 0x43c0010f, 0x47c0011f, 0x4c000130, 0x50800142,
	0x55400155, 0x5a400169, 0x5fc0017f, 0x65400195, 0x6b8001ae,
	0x71c001c7, 0x788001e2, 0x7f8001fe
};

/* CCK filter (0xa22 to 0xa29), -16 to +0 dB in 0.5 dB. */
static const uint8_t r8723b_cck_swing_ch1_ch13[][8] = {
	{0x09, 0x08, 0x07, 0x06, 0x04, 0x03, 0x01, 0x01},
	{0x09, 0x09, 0x08, 0x06, 0x05, 0x03, 0x01, 0x01},
	{0x0a, 0x09, 0x08, 0x07, 0x05, 0x03, 0x02, 0x01},
	{0x0a, 0x0a, 0x09, 0x07, 0x05, 0x03, 0x02, 0x01},
	{0x0b, 0x0a, 0x09, 0x08, 0x06, 0x04, 0x02, 0x01},
	{0x0b, 0x0b, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x01},
	{0x0c, 0x0c, 0x0a, 0x09, 0x06, 0x04, 0x02, 0x01},
	{0x0d, 0x0c, 0x0b, 0x09, 0x07, 0x04, 0x02, 0x01},
	{0x0d, 0x0d, 0x0c, 0x0a, 0x07, 0x05, 0x02, 0x01},
	{0x0e, 0x0e, 0x0c, 0x0a, 0x08, 0x05, 0x02, 0x01},
	{0x0f, 0x0f, 0x0d, 0x0b, 0x08, 0x05, 0x03, 0x01},
	{0x10, 0x10, 0x0e, 0x0b, 0x08, 0x05, 0x03, 0x01},
	{0x11, 0x11, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01},
	{0x12, 0x12, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01},
	{0x13, 0x13, 0x10, 0x0d, 0x0a, 0x06, 0x03, 0x01},
	{0x14, 0x14, 0x11, 0x0e, 0x0b, 0x07, 0x03, 0x02},
	{0x16, 0x15, 0x12, 0x0f, 0x0b, 0x07, 0x04, 0x01},
	{0x17, 0x16, 0x13, 0x10, 0x0c, 0x08, 0x04, 0x02},
	{0x18, 0x17, 0x15, 0x11, 0x0c, 0x08, 0x04, 0x02},
	{0x1a, 0x19, 0x16, 0x12, 0x0d, 0x09, 0x04, 0x02},
	{0x1b, 0x1a, 0x17, 0x13, 0x0e, 0x09, 0x04, 0x02},
	{0x1d, 0x1c, 0x18, 0x14, 0x0f, 0x0a, 0x05, 0x02},
	{0x1f, 0x1e, 0x1a, 0x15, 0x10, 0x0a, 0x05, 0x02},
	{0x20, 0x20, 0x1b, 0x16, 0x11, 0x08, 0x05, 0x02},
	{0x22, 0x21, 0x1d, 0x18, 0x11, 0x0b, 0x06, 0x02},
	{0x24, 0x23, 0x1f, 0x19, 0x13, 0x0c, 0x06, 0x03},
	{0x26, 0x25, 0x21, 0x1b, 0x14, 0x0d, 0x06, 0x03},
	{0x28, 0x28, 0x22, 0x1c, 0x15, 0x0d, 0x07, 0x03},
	{0x2b, 0x2a, 0x25, 0x1e, 0x16, 0x0e, 0x07, 0x03},
	{0x2d, 0x2d, 0x27, 0x1f, 0x18, 0x0f, 0x08, 0x03},
	{0x30, 0x2f, 0x29, 0x21, 0x19, 0x10, 0x08, 0x03},
	{0x33, 0x32, 0x2b, 0x23, 0x1a, 0x11, 0x08, 0x04},
	{0x36, 0x35, 0x2e, 0x25, 0x1c, 0x12, 0x09, 0x04},
};

static const uint8_t r8723b_cck_swing_ch14[][8] = {
	{0x09, 0x08, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00},
	{0x09, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00},
	{0x0a, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00},
	{0x0a, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00},
	{0x0b, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00},
	{0x0b, 0x0b, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00},
	{0x0c, 0x0c, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00},
	{0x0d, 0x0c, 0x0b, 0x06, 0x00, 0x00, 0x00, 0x00},
	{0x0d, 0x0d, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00},
	{0x0e, 0x0e, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00},
	{0x0f, 0x0f, 0x0d, 0x08, 0x00, 0x00, 0x00, 0x00},
	{0x10, 0x10, 0x0e, 0x08, 0x00, 0x00, 0x00, 0x00},
	{0x11, 0x11, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00},
	{0x12, 0x12, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00},
	{0x13, 0x13, 0x10, 0x0a, 0x00, 0x00, 0x00, 0x00},
	{0x14, 0x14, 0x11, 0x0a, 0x00, 0x00, 0x00, 0x00},
	{0x16, 0x15, 0x12, 0x0b, 0x00, 0x00, 0x00, 0x00},
	{0x17, 0x16, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00},
	{0x18, 0x17, 0x15, 0x0c, 0x00, 0x00, 0x00, 0x00},
	{0x1a, 0x19, 0x16, 0x0d, 0x00, 0x00, 0x00, 0x00},
	{0x1b, 0x1a, 0x17, 0x0e, 0x00, 0x00, 0x00, 0x00},
	{0x1d, 0x1c, 0x18, 0x0e, 0x00, 0x00, 0x00, 0x00},
	{0x1f, 0x1e, 0x1a, 0x0f, 0x00, 0x00, 0x00, 0x00},
	{0x20, 0x20, 0x1b, 0x10, 0x00, 0x00, 0x00, 0x00},
	{0x22, 0x21, 0x1d, 0x11, 0x00, 0x00, 0x00, 0x00},
	{0x24, 0x23, 0x1f, 0x12, 0x00, 0x00, 0x00, 0x00},
	{0x26, 0x25, 0x21, 0x13, 0x00, 0x00, 0x00, 0x00},
	{0x28, 0x28, 0x24, 0x14, 0x00, 0x00, 0x00, 0x00},
	{0x2b, 0x2a, 0x25, 0x15, 0x00, 0x00, 0x00, 0x00},
	{0x2d, 0x2d, 0x17, 0x17, 0x00, 0x00, 0x00, 0x00},
	{0x30, 0x2f, 0x29, 0x18, 0x00, 0x00, 0x00, 0x00},
	{0x33, 0x32, 0x2b, 0x19, 0x00, 0x00, 0x00, 0x00},
	{0x36, 0x35, 0x2e, 0x1b, 0x00, 0x00, 0x00, 0x00},
};

#define R8723B_OFDM_SWING_SIZE	nitems(r8723b_ofdm_swing)
#define R8723B_CCK_SWING_SIZE	nitems(r8723b_cck_swing_ch14)

/*
 * Swing steps per degree of the thermal meter away from the efuse value,
 * for OFDM; hotter ("up") and colder ("down"). The CCK tables of rtw88
 * differ by at most one step; the transmit rate that would choose between
 * them is not known here.
 */
static const uint8_t r8723b_pt_up[R8723B_PT_TBL_SIZE] = {
	0, 0, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7, 8, 8,
	9, 9, 10, 10, 10, 11, 11, 12, 12, 13, 13, 14, 15, 15
};

static const uint8_t r8723b_pt_down[R8723B_PT_TBL_SIZE] = {
	0, 0, 1, 2, 2, 2, 3, 3, 3, 4, 5, 5, 6, 6, 6, 6,
	7, 7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 13, 14, 15
};


static int32_t
r8723b_pt_xy_to_s32(int32_t val)
{
	/* 10 bit two's complement */
	return (val & 0x200) ? (val | ~0x3ff) : (val & 0x3ff);
}


static int32_t
r8723b_pt_mult(int32_t x, int32_t y, int32_t *ext)
{
	int32_t t = x * y;

	if (ext != NULL)
		*ext = (t >> 7) & 0x1;
	return (t >> 8);
}


/*
 * The OFDM swing goes into element D of the TX IQ matrix; with an IQ
 * calibration result, elements A and C are scaled with it. The extra bit
 * of element A goes to bit 24 of 0xc4c here, as in rtw88's and the vendor
 * driver's power tracking.
 */
static void
r8723b_pt_set_ofdm_swing(struct rtwn_softc *sc, int index)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint32_t swing, d;
	int32_t a, a_ext, c;

	index = MAX(0, MIN(index, (int)R8723B_OFDM_SWING_SIZE - 1));
	swing = r8723b_ofdm_swing[index];

	if (!rs->iqk_done) {
		rtwn_bb_write(sc, R8723B_OFDM0_XA_TX_IQ_IMB, swing);
		rtwn_bb_setbits(sc, R8723B_TXIQK_MATRIXA_LSB2, 0xf0000000, 0);
		rtwn_bb_setbits(sc, R8723B_OFDM0_ECCA_THRESHOLD, 0x01000000, 0);
		return;
	}

	d = swing >> 22;
	a = r8723b_pt_mult(r8723b_pt_xy_to_s32(rs->iqk_tx_x), d, &a_ext);
	c = r8723b_pt_mult(r8723b_pt_xy_to_s32(rs->iqk_tx_y), d, NULL);

	rtwn_bb_write(sc, R8723B_OFDM0_XA_TX_IQ_IMB,
	    (d << 22) | ((c & 0x3f) << 16) | (a & 0x3ff));
	rtwn_bb_setbits(sc, R8723B_TXIQK_MATRIXA_LSB2, 0xf0000000,
	    ((c & 0x3c0) >> 6) << 28);
	rtwn_bb_setbits(sc, R8723B_OFDM0_ECCA_THRESHOLD, 0x01000000,
	    a_ext ? 0x01000000 : 0);
}


static void
r8723b_pt_set_cck_swing(struct rtwn_softc *sc, int index)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const uint8_t *table;
	int i;

	index = MAX(0, MIN(index, (int)R8723B_CCK_SWING_SIZE - 1));
	if (ic->ic_curchan != IEEE80211_CHAN_ANYC
	    && rtwn_chan2centieee(ic->ic_curchan) == 14)
		table = r8723b_cck_swing_ch14[index];
	else
		table = r8723b_cck_swing_ch1_ch13[index];

	for (i = 0; i < 8; i++)
		rtwn_write_1(sc, R8723B_CCK_SWING + i, table[i]);
}


/* Swing what can be swung, the rest goes to the TX power index. */
static void
r8723b_pt_apply(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	struct ieee80211com *ic = &sc->sc_ic;
	int limit_ofdm = rs->pt_default_ofdm;
	int ofdm = rs->pt_default_ofdm + rs->pt_delta;
	int cck = rs->pt_default_cck + rs->pt_delta;
	int8_t remnant_ofdm = 0, remnant_cck = 0;

	if (ofdm > limit_ofdm) {
		remnant_ofdm = ofdm - limit_ofdm;
		ofdm = limit_ofdm;
	} else if (ofdm < 0) {
		remnant_ofdm = ofdm;
		ofdm = 0;
	}
	if (cck > R8723B_PT_LIMIT_CCK) {
		remnant_cck = cck - R8723B_PT_LIMIT_CCK;
		cck = R8723B_PT_LIMIT_CCK;
	} else if (cck < 0) {
		remnant_cck = cck;
		cck = 0;
	}

	r8723b_pt_set_ofdm_swing(sc, ofdm);
	r8723b_pt_set_cck_swing(sc, cck);

	if (remnant_ofdm != rs->rs.pwr_remnant_ofdm
	    || remnant_cck != rs->rs.pwr_remnant_cck) {
		rs->rs.pwr_remnant_ofdm = remnant_ofdm;
		rs->rs.pwr_remnant_cck = remnant_cck;
		if (ic->ic_curchan != IEEE80211_CHAN_ANYC)
			r92e_set_txpower(sc, ic->ic_curchan);
	}

	device_printf(sc->sc_dev, "power tracking: thermal %u (efuse %u), "
	    "%+d steps: OFDM swing %d%+d, CCK swing %d%+d\n",
	    rs->pt_avg >> 10, sc->thermal_meter, rs->pt_delta, ofdm,
	    remnant_ofdm, cck, remnant_cck);
}


/* After the baseband setup and the IQ calibration. */
void
r8723b_pwrtrack_init(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint32_t d;
	uint8_t cck;
	int i;

	d = rtwn_bb_read(sc, R8723B_OFDM0_XA_TX_IQ_IMB) >> 22;
	rs->pt_default_ofdm = R8723B_PT_DEFAULT_OFDM;
	for (i = 0; i < (int)R8723B_OFDM_SWING_SIZE; i++) {
		if ((r8723b_ofdm_swing[i] >> 22) == d) {
			rs->pt_default_ofdm = i;
			break;
		}
	}

	cck = rtwn_read_1(sc, R8723B_CCK_SWING);
	rs->pt_default_cck = R8723B_PT_DEFAULT_CCK;
	for (i = 0; i < (int)R8723B_CCK_SWING_SIZE; i++) {
		if (r8723b_cck_swing_ch1_ch13[i][0] == cck) {
			rs->pt_default_cck = i;
			break;
		}
	}

	rs->pt_delta = 0;
	rs->pt_init = 1;
	rs->pt_avg_valid = 0;
	rs->rs.pwr_remnant_ofdm = 0;
	rs->rs.pwr_remnant_cck = 0;
}


/*
 * Called every 2 seconds with a link, right after rtwn_temp_calib(); acts
 * when that has just read a new thermal meter value (it measures every
 * other round).
 */
void
r8723b_pwrtrack(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint8_t thermal, avg, delta;
	int8_t steps;

	RTWN_ASSERT_LOCKED(sc);

	if (sc->thermal_meter == 0xff || sc->thermal_meter == 0)
		return;
	if (sc->sc_flags & RTWN_TEMP_MEASURED)
		return;		/* a measurement is running */

	thermal = rtwn_temp_read(sc);
	if (thermal == 0)
		return;

	/* Exponential average, weight 1/4, as rtw88's ewma_thermal. */
	if (!rs->pt_avg_valid) {
		rs->pt_avg = (uint32_t)thermal << 10;
		rs->pt_avg_valid = 1;
	} else {
		rs->pt_avg = ((rs->pt_avg << 2) - rs->pt_avg
		    + ((uint32_t)thermal << 10)) >> 2;
	}
	avg = rs->pt_avg >> 10;

	if (rs->pt_init)
		rs->pt_init = 0;
	else if (avg == thermal)
		return;

	delta = avg > sc->thermal_meter ? avg - sc->thermal_meter
	    : sc->thermal_meter - avg;
	delta = MIN(delta, R8723B_PT_TBL_SIZE - 1);
	if (avg > sc->thermal_meter)
		steps = r8723b_pt_up[delta];
	else
		steps = -r8723b_pt_down[delta];

	if (steps == rs->pt_delta && rs->pt_avg_valid > 1)
		return;
	rs->pt_avg_valid = 2;
	rs->pt_delta = steps;
	r8723b_pt_apply(sc);
}
