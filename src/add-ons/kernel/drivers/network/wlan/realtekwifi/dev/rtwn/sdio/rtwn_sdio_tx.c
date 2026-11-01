/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Transmit path of rtwn over SDIO.

	A frame is written with one CMD53 to the transmit FIFO of its queue; the
	low bits of that address carry its length in dwords. The chip charges
	pages of the FIFO (or of the public pool) for it, so a frame waits until
	REG_SDIO_FREE_TXPG shows enough of them. The write is synchronous: the
	frame is done when the command completes, and is handed back to net80211
	from a task so that no callback runs under the driver lock.

	The queue to FIFO mapping follows the RQPN table of the rtw88 driver for
	SDIO: VO, management and high to the high FIFO, VI to the normal one, BE
	and BK to the low one.
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
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>


/* How long a frame waits for free pages before it is dropped. */
#define RTWN_SDIO_TX_WAIT_US	20000


static int
rtwn_sdio_tx_queue(uint8_t type, struct mbuf *m)
{
	if (type != IEEE80211_FC0_TYPE_DATA)
		return (RTWN_SDIO_TXQ_HIGH);

	switch (M_WME_GETAC(m)) {
	case WME_AC_VO:
		return (RTWN_SDIO_TXQ_HIGH);
	case WME_AC_VI:
		return (RTWN_SDIO_TXQ_NORMAL);
	default:
		return (RTWN_SDIO_TXQ_LOW);
	}
}


static int
rtwn_sdio_free_pages(uint32_t freePages, int queue)
{
	int pages = RTWN_SDIO_FREE_TXPG_PUB(freePages);

	switch (queue) {
	case RTWN_SDIO_TXQ_HIGH:
		return (pages + RTWN_SDIO_FREE_TXPG_HIGH(freePages));
	case RTWN_SDIO_TXQ_NORMAL:
		return (pages + RTWN_SDIO_FREE_TXPG_NORMAL(freePages));
	default:
		return (pages + RTWN_SDIO_FREE_TXPG_LOW(freePages));
	}
}


static int
rtwn_sdio_wait_pages(struct rtwn_sdio_softc *uc, int queue, int pages)
{
	int waited;

	for (waited = 0; waited < RTWN_SDIO_TX_WAIT_US; waited += 100) {
		uint32_t freePages = rtwn_sdio_local_read_4(uc,
		    RTWN_SDIO_FREE_TXPG);
		if (freePages != 0xffffffff
		    && rtwn_sdio_free_pages(freePages, queue) >= pages
		    && rtwn_sdio_local_read_1(uc, RTWN_SDIO_OQT_FREE_PG) != 0)
			return (0);
		DELAY(100);
	}

	return (ENOBUFS);
}


/*!	Checksum of the first 32 bytes of the descriptor, inverted, as the
	RTL8723B family expects (rtw88, rtw8723x.c).
*/
static void
rtwn_sdio_tx_checksum(struct rtwn_tx_desc_common *txd)
{
	uint16_t sum = 0;
	int i;

	txd->txdw7.usb_checksum = 0;
	for (i = 0; i < 32 / 2; i++)
		sum ^= ((uint16_t *)txd)[i];
	txd->txdw7.usb_checksum = ~sum;
}


int
rtwn_sdio_tx_start(struct rtwn_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, uint8_t *tx_desc, uint8_t type, int id)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	struct rtwn_tx_desc_common *txd;
	size_t length, size;
	uint32_t addr;
	int queue, pages, error;

	RTWN_ASSERT_LOCKED(sc);

	length = sc->txdesc_len + m->m_pkthdr.len;
	size = rtwn_sdio_align_size(length);
	if (size > RTWN_SDIO_TXBUFSZ)
		return (EINVAL);

	queue = rtwn_sdio_tx_queue(type, m);
	pages = howmany(roundup2(length, 4), sc->page_size);
	error = rtwn_sdio_wait_pages(uc, queue, pages);
	if (error != 0) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_XMIT,
		    "%s: no free pages in queue %d\n", __func__, queue);
		return (error);
	}

	txd = (struct rtwn_tx_desc_common *)tx_desc;
	txd->pktlen = htole16(m->m_pkthdr.len);
	txd->offset = sc->txdesc_len;
	txd->flags0 |= RTWN_FLAGS0_OWN;
	rtwn_sdio_tx_checksum(txd);

	rtwn_dump_tx_desc(sc, tx_desc);

	memcpy(uc->sc_tx_buf, tx_desc, sc->txdesc_len);
	m_copydata(m, 0, m->m_pkthdr.len,
	    (caddr_t)(uc->sc_tx_buf + sc->txdesc_len));
	memset(uc->sc_tx_buf + length, 0, size - length);

	addr = RTWN_SDIO_ADDR(RTWN_SDIO_ADDR_TXFF_HIGH + queue,
	    howmany(length, 4));
	error = sdio_write_bytes(uc->sc_func, addr, uc->sc_tx_buf, size, true);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: writing %zu bytes to queue %d "
		    "failed: %d\n", __func__, size, queue, error);
		return (EIO);
	}

	/* Beacons (no node) stay owned by the caller. */
	if (ni != NULL) {
		m->m_pkthdr.PH_loc.ptr = ni;
		if (mbufq_enqueue(&uc->sc_tx_done, m) != 0)
			ieee80211_tx_complete(ni, m, 0);
		else
			taskqueue_enqueue(taskqueue_thread, &uc->sc_tx_task);
	}

	return (0);
}


/*!	Hands written frames back to net80211 with \a status. */
void
rtwn_sdio_tx_drain(struct rtwn_sdio_softc *uc, int status)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	struct mbufq done;
	struct mbuf *m;

	RTWN_LOCK(sc);
	mbufq_init(&done, IFQ_MAXLEN);
	while ((m = mbufq_dequeue(&uc->sc_tx_done)) != NULL)
		mbufq_enqueue(&done, m);
	RTWN_UNLOCK(sc);

	while ((m = mbufq_dequeue(&done)) != NULL) {
		struct ieee80211_node *ni = m->m_pkthdr.PH_loc.ptr;

		m->m_pkthdr.PH_loc.ptr = NULL;
		ieee80211_tx_complete(ni, m, status);
	}
}


void
rtwn_sdio_tx_done_task(void *arg, int pending)
{
	struct rtwn_sdio_softc *uc = arg;
	struct rtwn_softc *sc = &uc->sc_sc;

	rtwn_sdio_tx_drain(uc, 0);

	RTWN_LOCK(sc);
	rtwn_start(sc);
	RTWN_UNLOCK(sc);
}
