/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Interrupt and receive path of rtwn over SDIO.

	The card interrupt runs rtwn_sdio_intr() in a thread. REG_SDIO_HISR says
	why; for received data REG_SDIO_RX0_REQ_LEN gives the number of bytes
	waiting in the receive FIFO, which are read with one CMD53 per page from
	the RX0 FIFO address. The buffer holds one or more frames, each with a
	receive descriptor and aligned to 8 bytes, like an aggregated USB
	transfer. The order of the steps follows the rtw88 driver (sdio.c).
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/taskqueue.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/sdio_haiku.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>
#include <dev/rtwn/if_rtwn_ridx.h>
#include <dev/rtwn/if_rtwn_rx.h>
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>


/* Stop reading after this many bytes to let other work in. */
#define RTWN_SDIO_RX_BUDGET	(64 * 1024)
/* Bytes per CMD53, whole blocks. */
#define RTWN_SDIO_RX_CHUNK	4096


void
rtwn_sdio_enable_intr(struct rtwn_sdio_softc *uc, bool enable)
{
	uc->sc_intr_enabled = enable;
	rtwn_sdio_local_write_4(uc, RTWN_SDIO_HIMR, enable ? uc->sc_himr : 0);
}


static int
rtwn_sdio_read_port(struct rtwn_sdio_softc *uc, uint8_t *buf, size_t size)
{
	uint32_t addr = RTWN_SDIO_RX0FF(uc->sc_rx_id++);
	int error = 0;

	/* Drain all of it even after an error, or the next read is garbage. */
	while (size > 0) {
		size_t chunk = MIN(size, RTWN_SDIO_RX_CHUNK);
		int status = sdio_read_bytes(uc->sc_func, addr, buf, chunk, true);
		if (status != 0)
			error = status;
		buf += chunk;
		size -= chunk;
	}

	return (error);
}


static struct mbuf *
rtwn_sdio_rx_copy(struct rtwn_softc *sc, struct rtwn_rx_stat_common *stat,
    int totlen)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t rxdw0 = le32toh(stat->rxdw0);
	struct mbuf *m;

	if (!(sc->sc_flags & RTWN_RUNNING))
		return (NULL);

	if (rxdw0 & (RTWN_RXDW0_CRCERR | RTWN_RXDW0_ICVERR)
	    || MS(rxdw0, RTWN_RXDW0_PKTLEN)
		< sizeof(struct ieee80211_frame_ack)) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	m = m_get3(totlen, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	memcpy(mtod(m, uint8_t *), stat, totlen);
	m->m_pkthdr.len = m->m_len = totlen;

	if (rtwn_check_frame(sc, m) != 0) {
		m_freem(m);
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	return (m);
}


/*
 * Test only: counts the frames per channel and logs the first 300 data
 * frames: channel, frame control, rate, RSSI, transmitter, BSSID and, for
 * beacons and probe responses, the SSID.
 */
static void
rtwn_sdio_diag_frame(struct rtwn_softc *sc, uint8_t *buf, int totlen)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct rtwn_rx_stat_common *stat = (struct rtwn_rx_stat_common *)buf;
	uint32_t rxdw0 = le32toh(stat->rxdw0);
	uint32_t rxdw3 = le32toh(stat->rxdw3);
	int infosz = MS(rxdw0, RTWN_RXDW0_INFOSZ) * 8;
	int shift = MS(rxdw0, RTWN_RXDW0_SHIFT);
	int pktlen = MS(rxdw0, RTWN_RXDW0_PKTLEN);
	uint8_t *wh = buf + sizeof(*stat) + infosz + shift;
	int rate = rxdw3 & 0x7f;
	int chan = ic->ic_curchan != NULL ? ic->ic_curchan->ic_ieee : 0;
	int rssi = 0;
	char ssid[33];

	if (chan > 0 && chan < 15)
		uc->sc_diag_chan_frames[chan]++;
	if (uc->sc_diag_logged >= 300 || pktlen < 24)
		return;
	uc->sc_diag_logged++;

	if (infosz != 0 && (rxdw0 & RTWN_RXDW0_PHYST) != 0) {
		void *physt = buf + sizeof(*stat);
		rssi = rate < 4 ? rtwn_get_rssi_cck(sc, physt)
		    : rtwn_get_rssi_ofdm(sc, physt);
	}

	ssid[0] = '\0';
	/* Beacon (0x80) or probe response (0x50): SSID element at 36. */
	if ((wh[0] == 0x80 || wh[0] == 0x50) && pktlen >= 38 && wh[36] == 0) {
		int n = MIN(wh[37], 32);
		int i;

		if (38 + n > pktlen)
			n = pktlen - 38;
		for (i = 0; i < n; i++)
			ssid[i] = (wh[38 + i] >= 0x20 && wh[38 + i] < 0x7f)
			    ? wh[38 + i] : '.';
		ssid[n] = '\0';
	}

	device_printf(sc->sc_dev, "rx: t %lld ms ch %d fc %02x%02x len %d "
	    "rate %d rssi %d ta %02x:%02x:%02x:%02x:%02x:%02x bssid "
	    "%02x:%02x:%02x:%02x:%02x:%02x \"%s\"\n",
	    (long long)(system_time() / 1000), chan, wh[0], wh[1], pktlen,
	    rate, rssi, wh[10], wh[11], wh[12], wh[13], wh[14], wh[15],
	    wh[16], wh[17], wh[18], wh[19], wh[20], wh[21], ssid);
	(void)totlen;
}


/*!	Splits the buffer into frames. Reports of the firmware are handled
	here, received frames are returned as a list.
*/
static struct mbuf *
rtwn_sdio_rx_parse(struct rtwn_softc *sc, uint8_t *buf, int len)
{
	struct mbuf *m0 = NULL, *last = NULL;

	while (len >= (int)sizeof(struct rtwn_rx_stat_common)) {
		struct rtwn_rx_stat_common *stat
		    = (struct rtwn_rx_stat_common *)buf;
		uint32_t rxdw0 = le32toh(stat->rxdw0);
		int pktlen = MS(rxdw0, RTWN_RXDW0_PKTLEN);
		int infosz = MS(rxdw0, RTWN_RXDW0_INFOSZ) * 8;
		int shift = MS(rxdw0, RTWN_RXDW0_SHIFT);
		int totlen = sizeof(*stat) + infosz + shift + pktlen;
		struct mbuf *m;

		if (pktlen == 0 || totlen > len)
			break;

		switch (rtwn_classify_intr(sc, buf, totlen)) {
		case RTWN_RX_DATA:
			rtwn_sdio_diag_frame(sc, buf, totlen);
			m = rtwn_sdio_rx_copy(sc, stat, totlen);
			if (m != NULL)
				RTWN_SDIO_SOFTC(sc)->sc_diag_rx_frames++;
			else
				RTWN_SDIO_SOFTC(sc)->sc_diag_rx_dropped++;
			if (m != NULL) {
				if (last == NULL)
					m0 = m;
				else
					last->m_nextpkt = m;
				last = m;
			}
			break;
		case RTWN_RX_TX_REPORT:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		case RTWN_RX_TX_REPORT2:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report2(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		default:
			RTWN_SDIO_SOFTC(sc)->sc_diag_rx_other++;
			rtwn_handle_c2h_report(sc, buf, totlen);
			break;
		}

		totlen = roundup2(totlen, RTWN_SDIO_DATA_ALIGN);
		buf += totlen;
		len -= totlen;
	}

	return (m0);
}


static void
rtwn_sdio_rx_input(struct rtwn_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m0 = m, *next;

	/* rtwn_rx_common() wants the PCIe layout: descriptor apart. */
	for (; m != NULL; m = m->m_nextpkt) {
		struct rtwn_rx_stat_common stat;

		m_copydata(m, 0, sizeof(stat), (caddr_t)&stat);
		m_adj(m, sizeof(stat));
		m->m_pkthdr.PH_loc.ptr = rtwn_rx_common(sc, m, &stat);
	}

	RTWN_UNLOCK(sc);
	for (m = m0; m != NULL; m = next) {
		struct ieee80211_node *ni = m->m_pkthdr.PH_loc.ptr;

		next = m->m_nextpkt;
		m->m_nextpkt = NULL;
		m->m_pkthdr.PH_loc.ptr = NULL;
		if (ni != NULL) {
			(void)ieee80211_input_mimo(ni, m);
			ieee80211_free_node(ni);
		} else
			(void)ieee80211_input_mimo_all(ic, m);
	}
	RTWN_LOCK(sc);
}


static void
rtwn_sdio_rx(struct rtwn_sdio_softc *uc)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	uint32_t total = 0;

	do {
		uint32_t len = rtwn_sdio_local_read_2(uc, RTWN_SDIO_RX0_REQ_LEN);
		size_t size;
		struct mbuf *m;

		if (len == 0 || len == 0xffff)
			break;

		size = rtwn_sdio_align_size(len);
		if (size > RTWN_SDIO_RXBUFSZ) {
			device_printf(sc->sc_dev, "%s: %u bytes do not fit\n",
			    __func__, len);
			size = RTWN_SDIO_RXBUFSZ;
		}
		if (rtwn_sdio_read_port(uc, uc->sc_rx_buf, size) != 0) {
			counter_u64_add(sc->sc_ic.ic_ierrors, 1);
			break;
		}
		total += len;
		uc->sc_diag_rx_reads++;
		uc->sc_diag_rx_bytes += len;

		m = rtwn_sdio_rx_parse(sc, uc->sc_rx_buf, MIN(len, size));
		if (m != NULL)
			rtwn_sdio_rx_input(sc, m);

		/*
		 * The 8051 chips can show a length while the FIFO is still
		 * being filled; only go on while a request is pending.
		 */
	} while (total < RTWN_SDIO_RX_BUDGET
	    && (rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR)
		& RTWN_SDIO_INT_RX_REQUEST) != 0);
}


void
rtwn_sdio_intr(void *arg)
{
	struct rtwn_sdio_softc *uc = arg;
	struct rtwn_softc *sc = &uc->sc_sc;
	uint32_t hisr;

	RTWN_LOCK(sc);
	uc->sc_diag_intr++;
	if (!uc->sc_intr_enabled) {
		RTWN_UNLOCK(sc);
		return;
	}

	hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
	if (hisr == 0xffffffff) {
		RTWN_UNLOCK(sc);
		return;
	}

	if (hisr & RTWN_SDIO_INT_TXERR)
		device_printf(sc->sc_dev, "transmit error (HISR %#x)\n", hisr);
	if (hisr & RTWN_SDIO_INT_RX_REQUEST)
		rtwn_sdio_rx(uc);

	/* Only defined bits: others keep the interrupt asserted. */
	hisr &= RTWN_SDIO_HISR_CLEAR_MASK;
	if (hisr != 0)
		rtwn_sdio_local_write_4(uc, RTWN_SDIO_HISR, hisr);

	rtwn_start(sc);
	RTWN_UNLOCK(sc);
}


/*
 * Test only: the PHY counters of the RTL8723B over the last 5 s (rtw88
 * rtw8723x_false_alarm_statistics() registers): packets the baseband saw
 * (CCA), false alarms and CRC good and bad, then the channel, the RF
 * channel register, IGI, RCR, the coexistence table and the frames per
 * channel.
 */
static void
rtwn_sdio_diag_phy(struct rtwn_sdio_softc *uc)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t cck_fa, cck_cca, ofdm_fa, t1, t2, t3, t4, cck_err, cck_ok;
	uint32_t ofdm_crc, ht_crc;
	char perchan[15 * 8];
	int i, n = 0;

	/* Hold the counters. */
	rtwn_bb_setbits(sc, 0xc00, 0, 1U << 31);
	rtwn_bb_setbits(sc, 0xd00, 0, 1U << 31);
	rtwn_bb_setbits(sc, 0xa2c, 0, (1U << 12) | (1U << 14));

	cck_fa = (rtwn_bb_read(sc, 0xa5c) & 0xff)
	    | ((rtwn_bb_read(sc, 0xa58) >> 24) << 8);
	cck_cca = rtwn_bb_read(sc, 0xa60);
	cck_cca = ((cck_cca & 0xff) << 8) | ((cck_cca >> 8) & 0xff);
	t1 = rtwn_bb_read(sc, 0xcf0);
	t2 = rtwn_bb_read(sc, 0xda0);
	t3 = rtwn_bb_read(sc, 0xda4);
	t4 = rtwn_bb_read(sc, 0xda8);
	ofdm_fa = (t1 & 0xffff) + (t1 >> 16) + (t2 >> 16) + (t3 & 0xffff)
	    + (t3 >> 16) + (t4 & 0xffff);
	cck_err = rtwn_bb_read(sc, 0xf84);
	cck_ok = rtwn_bb_read(sc, 0xf88);
	ht_crc = rtwn_bb_read(sc, 0xf90);
	ofdm_crc = rtwn_bb_read(sc, 0xf94);

	/* Reset them. */
	rtwn_bb_setbits(sc, 0xc0c, 0, 1U << 31);
	rtwn_bb_setbits(sc, 0xc0c, 1U << 31, 0);
	rtwn_bb_setbits(sc, 0xd00, 0, 1U << 27);
	rtwn_bb_setbits(sc, 0xd00, 1U << 27, 0);
	rtwn_bb_setbits(sc, 0xc00, 1U << 31, 0);
	rtwn_bb_setbits(sc, 0xd00, 1U << 31, 0);
	rtwn_bb_setbits(sc, 0xa2c, (3U << 12) | (3U << 14), 0);
	rtwn_bb_setbits(sc, 0xa2c, 0, (1U << 13) | (1U << 15));
	rtwn_bb_setbits(sc, 0xf14, 0, 1U << 16);
	rtwn_bb_setbits(sc, 0xf14, 1U << 16, 0);

	device_printf(sc->sc_dev, "phy: CCA cck %u ofdm %u, false alarm cck %u "
	    "ofdm %u, CRC ok/err cck %u/%u ofdm %u/%u ht %u/%u\n",
	    cck_cca, t2 & 0xffff, cck_fa, ofdm_fa, cck_ok, cck_err,
	    ofdm_crc & 0xffff, ofdm_crc >> 16, ht_crc & 0xffff, ht_crc >> 16);

	perchan[0] = '\0';
	for (i = 1; i < 15; i++) {
		if (uc->sc_diag_chan_frames[i] != 0) {
			n += snprintf(perchan + n, sizeof(perchan) - n, " %d:%u",
			    i, uc->sc_diag_chan_frames[i]);
		}
	}
	device_printf(sc->sc_dev, "phy: ch %d RF18 %#x IGI %#x RCR %#x "
	    "coex %#x %#x %#x GPIO_MUX %#x; frames per channel:%s\n",
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_ieee : 0,
	    rtwn_rf_read(sc, 0, 0x18), rtwn_bb_read(sc, 0xc50) & 0xff,
	    rtwn_read_4(sc, 0x608), rtwn_read_4(sc, 0x6c0),
	    rtwn_read_4(sc, 0x6c4), rtwn_read_4(sc, 0x6c8),
	    rtwn_read_4(sc, 0x40), perchan);
}


/*
 * Test only: every 5 s, log the interrupt and receive counters and the
 * SDIO interrupt registers. When a receive request is pending, service it
 * from here too, so that frames arrive even if the card interrupt does not.
 */
static status_t
rtwn_sdio_diag_thread(void *arg)
{
	struct rtwn_sdio_softc *uc = arg;
	struct rtwn_softc *sc = &uc->sc_sc;
	int rounds = 0;

	while (!uc->sc_diag_stop) {
		snooze(50000);

		RTWN_LOCK(sc);
		if (uc->sc_intr_enabled) {
			uint32_t hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
			if (hisr != 0xffffffff
			    && (hisr & RTWN_SDIO_INT_RX_REQUEST) != 0) {
				uc->sc_diag_polled++;
				rtwn_sdio_rx(uc);
			}
		}

		if (++rounds >= 100) {
			rounds = 0;
			device_printf(sc->sc_dev, "diag: intr %u, polled %u, "
			    "rx reads %u (%u bytes), frames %u, dropped %u, "
			    "other %u, tx %u, tx errors %u; HIMR %#x HISR %#x "
			    "RX0_REQ_LEN %u, running %d\n",
			    uc->sc_diag_intr, uc->sc_diag_polled,
			    uc->sc_diag_rx_reads, uc->sc_diag_rx_bytes,
			    uc->sc_diag_rx_frames, uc->sc_diag_rx_dropped,
			    uc->sc_diag_rx_other, uc->sc_diag_tx,
			    uc->sc_diag_tx_errors,
			    rtwn_sdio_local_read_4(uc, RTWN_SDIO_HIMR),
			    rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR),
			    rtwn_sdio_local_read_2(uc, RTWN_SDIO_RX0_REQ_LEN),
			    (sc->sc_flags & RTWN_RUNNING) != 0);
			if (uc->sc_powered)
				rtwn_sdio_diag_phy(uc);
		}
		RTWN_UNLOCK(sc);
	}

	return (B_OK);
}


void
rtwn_sdio_diag_start(struct rtwn_sdio_softc *uc)
{
	uc->sc_diag_stop = false;
	uc->sc_diag_thread = spawn_kernel_thread(rtwn_sdio_diag_thread,
	    "rtwn sdio diag", B_LOW_PRIORITY, uc);
	if (uc->sc_diag_thread >= 0)
		resume_thread(uc->sc_diag_thread);
}


void
rtwn_sdio_diag_stop(struct rtwn_sdio_softc *uc)
{
	status_t result;

	if (uc->sc_diag_thread < 0)
		return;
	uc->sc_diag_stop = true;
	wait_for_thread(uc->sc_diag_thread, &result);
	uc->sc_diag_thread = -1;
}
