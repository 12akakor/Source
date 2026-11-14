/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Interrupt and receive path of rtwn over SDIO.

	rtwn_sdio_handle() does the work of a chip interrupt: the card
	interrupt reaches it through rtwn_sdio_service(), and while the card
	interrupt is off (see rtwn_sdio_attach()) the poll thread calls it
	directly. REG_SDIO_HISR says why; for received data
	REG_SDIO_RX0_REQ_LEN gives the number of bytes waiting in the receive
	FIFO, which are read with one CMD53 per page from the RX0 FIFO address.
	The buffer holds one or more frames, each with a receive descriptor and
	aligned to 8 bytes, like an aggregated USB transfer. The order of the
	steps follows the rtw88 driver (sdio.c).

	The receive path also counts what it does, for the test line that the
	poll thread prints every 10 s (see rtwn_sdio_rx_count()).
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

#include <dev/rtwn/rtl8192c/r92c_reg.h>
#include <dev/rtwn/rtl8192c/r92c_rx_desc.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>


/* Stop reading after this many bytes to let other work in. */
#define RTWN_SDIO_RX_BUDGET	(64 * 1024)
/* Bytes per CMD53, whole blocks. */
#define RTWN_SDIO_RX_CHUNK	4096
/* How often rtwn_sdio_poll_thread() looks for a receive request. */
#define RTWN_SDIO_POLL_INTERVAL	10000
/* How many polls make the 10 s of the test statistics. */
#define RTWN_SDIO_STAT_TICKS		(10 * 1000000 / RTWN_SDIO_POLL_INTERVAL)


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
		uc->sc_test_rx_reads++;	/* test */
		if (status != 0)
			error = status;
		buf += chunk;
		size -= chunk;
	}

	return (error);
}


/*!	Counts one received frame for the test statistics: its length, whether
	the chip reports an error, whether it came from an aggregate, and the
	rate and signal strength it came in with. \a physt is \c NULL when the
	descriptor has no PHY status of its own (the following MPDUs of an
	aggregate), in which case the last known signal strength stays.
*/
static void
rtwn_sdio_rx_count(struct rtwn_sdio_softc *uc,
    struct rtwn_rx_stat_common *stat, void *physt)
{
	uint32_t rxdw0 = le32toh(stat->rxdw0);
	uint32_t rxdw1 = le32toh(stat->rxdw1);
	uint32_t rxdw3 = le32toh(stat->rxdw3);
	uint8_t rate = MS(rxdw3, R92C_RXDW3_RATE);
	int pktlen = MS(rxdw0, RTWN_RXDW0_PKTLEN);

	uc->sc_test_rx++;
	uc->sc_test_rx_payload += pktlen;
	uc->sc_test_last_pktlen = pktlen;
	uc->sc_test_last_rxdw1 = rxdw1;
	uc->sc_test_last_rxdw3 = rxdw3;

	if (rxdw0 & RTWN_RXDW0_CRCERR)
		uc->sc_test_rx_crc++;
	if (rxdw0 & RTWN_RXDW0_ICVERR)
		uc->sc_test_rx_icv++;
	if (rxdw1 & R92C_RXDW1_AMPDU)
		uc->sc_test_rx_ampdu++;

	if (pktlen <= 128)
		uc->sc_test_rx_small++;
	else if (pktlen <= 512)
		uc->sc_test_rx_medium++;
	else if (pktlen <= 1500)
		uc->sc_test_rx_large++;
	else
		uc->sc_test_rx_jumbo++;

	if (physt != NULL) {
		if (RTWN_RATE_IS_CCK(rate))
			uc->sc_test_last_rssi
			    = rtwn_get_rssi_cck(&uc->sc_sc, physt);
		else
			uc->sc_test_last_rssi
			    = rtwn_get_rssi_ofdm(&uc->sc_sc, physt);
	}
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
		void *physt = NULL;
		struct mbuf *m;

		if (pktlen == 0 || totlen > len)
			break;

		switch (rtwn_classify_intr(sc, buf, totlen)) {
		case RTWN_RX_DATA:
			/* test: count it, with its own signal strength. */
			if (infosz != 0 && (rxdw0 & RTWN_RXDW0_PHYST))
				physt = buf + shift;
			rtwn_sdio_rx_count(RTWN_SDIO_SOFTC(sc), stat, physt);
			m = rtwn_sdio_rx_copy(sc, stat, totlen);
			sc->sc_rx_transfers++;
			if (m != NULL) {
				if (last == NULL)
					m0 = m;
				else
					last->m_nextpkt = m;
				last = m;
			}
			break;
		case RTWN_RX_TX_REPORT:
			RTWN_SDIO_SOFTC(sc)->sc_test_tx_reports++;	/* test */
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		case RTWN_RX_TX_REPORT2:
			RTWN_SDIO_SOFTC(sc)->sc_test_tx_reports2++;	/* test */
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report2(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		default:
			RTWN_SDIO_SOFTC(sc)->sc_test_c2h++;	/* test */
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

		uc->sc_test_rx_req++;	/* test */
		size = rtwn_sdio_align_size(len);
		if (size > RTWN_SDIO_RXBUFSZ) {
			device_printf(sc->sc_dev, "%s: %u bytes do not fit\n",
			    __func__, len);
			size = RTWN_SDIO_RXBUFSZ;
		}
		if (rtwn_sdio_read_port(uc, uc->sc_rx_buf, size) != 0) {
			uc->sc_test_rx_err++;	/* test */
			counter_u64_add(sc->sc_ic.ic_ierrors, 1);
			break;
		}
		uc->sc_test_rx_bytes += len;	/* test */
		total += len;

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


/*!	Does the work of a chip interrupt: reads REG_SDIO_HISR, handles a
	transmit error, reads the frames of a receive request and acknowledges
	the interrupt. \a kickTx restarts the transmit queue; the poll thread
	passes false, because rtwn_start() can wait 20 ms for free pages and
	that would keep the only thread that reads the receive FIFO away from
	it.
*/
static void
rtwn_sdio_handle(struct rtwn_sdio_softc *uc, bool kickTx)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	uint32_t hisr;

	RTWN_LOCK(sc);
	if (!uc->sc_intr_enabled) {
		RTWN_UNLOCK(sc);
		return;
	}

	uc->sc_test_polls++;	/* test */
	hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
	if (hisr == 0xffffffff) {
		RTWN_UNLOCK(sc);
		return;
	}

	if (hisr & RTWN_SDIO_INT_RXFOVW)
		uc->sc_test_ovfw++;	/* test */

	if (hisr & RTWN_SDIO_INT_TXERR) {
		/* The cause is in REG_TXDMA_STATUS; write 1 to clear. */
		uint32_t status = rtwn_read_4(sc, R92C_TXDMA_STATUS);
		rtwn_write_4(sc, R92C_TXDMA_STATUS, status);
		device_printf(sc->sc_dev, "transmit error (HISR %#x, "
		    "TXDMA_STATUS %#x)\n", hisr, status);
	}
	if (hisr & RTWN_SDIO_INT_RX_REQUEST) {
		uc->sc_test_polls_rx++;	/* test */
		rtwn_sdio_rx(uc);
	}

	/* Only defined bits: others keep the interrupt asserted. */
	hisr &= RTWN_SDIO_HISR_CLEAR_MASK;
	if (hisr != 0)
		rtwn_sdio_local_write_4(uc, RTWN_SDIO_HISR, hisr);

	if (kickTx)
		rtwn_start(sc);
	RTWN_UNLOCK(sc);
}


/*!	Entry point for the card interrupt; see rtwn_sdio_handle(). */
void
rtwn_sdio_service(void *arg)
{
	rtwn_sdio_handle(arg, true);
}


/*!	Polls the chip for work. The card interrupt is off (see
	rtwn_sdio_attach()), so this thread does what it would have done.
*/
static status_t
rtwn_sdio_poll_thread(void *arg)
{
	struct rtwn_sdio_softc *uc = arg;
	struct rtwn_softc *sc = &uc->sc_sc;

	int statTicks = 0;	/* test */
	while (!uc->sc_poll_stop) {
		snooze(RTWN_SDIO_POLL_INTERVAL);

		/* test: link statistics every 10 s */
		if (++statTicks % RTWN_SDIO_STAT_TICKS == 0) {
			RTWN_LOCK(sc);
			if (uc->sc_intr_enabled) {
				device_printf(sc->sc_dev, "test: tx %u reports %u/%u "
				    "c2h %u rx %u, last data txdw3 %08x txdw4 %08x, "
				    "RRSR %08x ARFR 0x444 %08x 0x448 %08x 0x44c %08x "
				    "TXPAUSE %02x IGI %02x rxerr %u HISR %08x RX0_REQ_LEN %u\n",
				    uc->sc_test_tx,
				    uc->sc_test_tx_reports, uc->sc_test_tx_reports2,
				    uc->sc_test_c2h, uc->sc_test_rx,
				    uc->sc_test_last_txdw3, uc->sc_test_last_txdw4,
				    rtwn_read_4(sc, 0x440), rtwn_read_4(sc, 0x444),
				    rtwn_read_4(sc, 0x448), rtwn_read_4(sc, 0x44c),
				    rtwn_read_1(sc, 0x522),
				    rtwn_bb_read(sc, 0xc50) & 0x7f,
				    uc->sc_test_rx_err,
				    rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR),
				    rtwn_sdio_local_read_2(uc, RTWN_SDIO_RX0_REQ_LEN));
				/* test: what the receive path did with it */
				device_printf(sc->sc_dev, "test: rxb %u crc %u icv %u "
				    "ampdu %u, len 128:%u 512:%u 1500:%u big:%u, "
				    "req %u reads %u %u B, rxreq %u of %u polls, "
				    "ovfw %u, last len %u rate %u%s%s%s rssi %d\n",
				    uc->sc_test_rx_payload, uc->sc_test_rx_crc,
				    uc->sc_test_rx_icv, uc->sc_test_rx_ampdu,
				    uc->sc_test_rx_small, uc->sc_test_rx_medium,
				    uc->sc_test_rx_large, uc->sc_test_rx_jumbo,
				    uc->sc_test_rx_req, uc->sc_test_rx_reads,
				    uc->sc_test_rx_bytes, uc->sc_test_polls_rx,
				    uc->sc_test_polls, uc->sc_test_ovfw,
				    uc->sc_test_last_pktlen,
				    MS(uc->sc_test_last_rxdw3, R92C_RXDW3_RATE),
				    (uc->sc_test_last_rxdw3 & R92C_RXDW3_HT) != 0
					? " ht" : "",
				    (uc->sc_test_last_rxdw3 & R92C_RXDW3_HT40) != 0
					? " 40" : "",
				    (uc->sc_test_last_rxdw1 & R92C_RXDW1_AMPDU) != 0
					? " ampdu" : "",
				    uc->sc_test_last_rssi);
			}
			RTWN_UNLOCK(sc);
		}

		/* test: the card interrupt is off, so do its work here. */
		rtwn_sdio_handle(uc, false);
	}

	return (B_OK);
}


void
rtwn_sdio_poll_start(struct rtwn_sdio_softc *uc)
{
	uc->sc_poll_stop = false;
	uc->sc_poll_thread = spawn_kernel_thread(rtwn_sdio_poll_thread,
	    "rtwn sdio poll", B_LOW_PRIORITY, uc);
	if (uc->sc_poll_thread >= 0)
		resume_thread(uc->sc_poll_thread);
}


void
rtwn_sdio_poll_stop(struct rtwn_sdio_softc *uc)
{
	status_t result;

	if (uc->sc_poll_thread < 0)
		return;
	uc->sc_poll_stop = true;
	wait_for_thread(uc->sc_poll_thread, &result);
	uc->sc_poll_thread = -1;
}
