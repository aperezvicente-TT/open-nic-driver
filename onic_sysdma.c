// SPDX-License-Identifier: GPL-2.0
/*
 * onic_sysdma.c - QDMA AXI-MM-mode system DMA for host <-> card-DDR4
 *
 * libqdma-backed implementation (Phase E of the libqdma vendoring plan).
 *
 * What this used to be: a hand-rolled MM submit path that built 32-byte
 * descriptors directly, programmed the SW context via the cut-down
 * qdma_legacy/ helpers, and polled wb_status to detect completion.  It
 * faulted at probe-time self-test with a DMAR error at 0xff00000000 even
 * though descriptor bytes and SW-context readbacks were bit-perfect, and
 * we ran out of differential-debug ammunition against a hand-rolled
 * implementation we couldn't fully audit.
 *
 * What it is now: a thin wrapper over AMD libqdma's public API.  We open
 * one Q_H2C and one Q_C2H queue at probe (st=0, irq_en=0, wb_status_en=1,
 * mm_channel=0) and submit blocking sg requests on each.  libqdma owns
 * the descriptor format, doorbell sequencing, completion polling, and
 * SW-context programming — paths AMD has shipped, customers test, and
 * we don't have to reverse-engineer.
 *
 * Lifecycle:
 *   onic_sysdma_init   — qdma_queue_add(H2C) + qdma_queue_start(H2C)
 *                        + qdma_queue_add(C2H) + qdma_queue_start(C2H)
 *   onic_ddr4_write    — copy host buf into staging, build sgl,
 *                        qdma_request_submit(write=1) blocking
 *   onic_ddr4_read     — symmetric, write=0
 *   onic_sysdma_fini   — qdma_queue_stop + qdma_queue_remove for both
 *
 * Note: priv->qdma_dev_handle must be nonzero (set by
 * qdma_device_open in onic_setup_primary) before calling sysdma_init.
 */

#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/pci.h>

#include "onic.h"
#include "onic_sysdma.h"
#include "libqdma/libqdma_export.h"

#define ONIC_SYSDMA_REL_QID    31         /* relative queue index */
#define ONIC_SYSDMA_RNGSZ_IDX  4          /* libqdma global_csr_conf.ring_sz[4] */
#define ONIC_SYSDMA_ERR_BUFLEN 256

/* Per-instance state stored on priv->sysdma. */
struct onic_sysdma_state {
	struct mutex            lock;       /* serialises ddr4_write/read */
	bool                    initialised;

	unsigned long           qhndl_h2c;  /* opaque from qdma_queue_add */
	unsigned long           qhndl_c2h;
	bool                    h2c_started;
	bool                    c2h_started;

	/* Staging buffer reused across calls — DMA-mapped once at init.
	 * Caller payload is memcpy'd in (write) or memcpy'd out (read). */
	void                   *staging;
	dma_addr_t              staging_dma;
	size_t                  staging_size;
};

/* ----------------------------------------------------------------- *
 *  Internal helpers
 * ----------------------------------------------------------------- */

static int sysdma_add_and_start(struct onic_private *priv,
				enum queue_type_t q_type,
				unsigned long *out_qhndl)
{
	struct qdma_queue_conf qconf;
	char errbuf[ONIC_SYSDMA_ERR_BUFLEN] = {0};
	int rv;

	memset(&qconf, 0, sizeof(qconf));
	qconf.qidx          = ONIC_SYSDMA_REL_QID;
	qconf.st            = 0;            /* AXI-MM, not stream */
	qconf.q_type        = q_type;
	qconf.irq_en        = 0;            /* poll-mode submit */
	qconf.wb_status_en  = 1;
	qconf.cmpl_status_acc_en = 0;
	qconf.cmpl_status_pend_chk = 0;
	qconf.desc_bypass   = 0;
	qconf.pfetch_en     = 0;
	qconf.fetch_credit  = 0;
	qconf.desc_rng_sz_idx = ONIC_SYSDMA_RNGSZ_IDX;
	qconf.mm_channel    = 0;

	rv = qdma_queue_add(priv->qdma_dev_handle, &qconf, out_qhndl,
			    errbuf, sizeof(errbuf));
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: qdma_queue_add(%s) failed (%d): %s\n",
			(q_type == Q_H2C) ? "H2C" : "C2H", rv, errbuf);
		return rv;
	}

	errbuf[0] = '\0';
	rv = qdma_queue_start(priv->qdma_dev_handle, *out_qhndl,
			      errbuf, sizeof(errbuf));
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: qdma_queue_start(%s) failed (%d): %s\n",
			(q_type == Q_H2C) ? "H2C" : "C2H", rv, errbuf);
		/* Caller will run sysdma_fini which removes the queue. */
		return rv;
	}

	dev_info(&priv->pdev->dev,
		 "onic_sysdma: %s queue ready (qid=%u, qhndl=%lu)\n",
		 (q_type == Q_H2C) ? "H2C" : "C2H",
		 ONIC_SYSDMA_REL_QID, *out_qhndl);
	return 0;
}

static void sysdma_stop_and_remove(struct onic_private *priv,
				   unsigned long *qhndl, bool *started,
				   const char *label)
{
	char errbuf[ONIC_SYSDMA_ERR_BUFLEN] = {0};

	if (*started) {
		(void)qdma_queue_stop(priv->qdma_dev_handle, *qhndl,
				      errbuf, sizeof(errbuf));
		*started = false;
	}
	if (*qhndl) {
		(void)qdma_queue_remove(priv->qdma_dev_handle, *qhndl,
					errbuf, sizeof(errbuf));
		*qhndl = 0;
	}
	(void)label;
}

/* Build a single-entry sgl pointing at @dma/@len, then submit a
 * blocking request. */
static int sysdma_submit_blocking(struct onic_private *priv,
				  unsigned long qhndl, bool write,
				  dma_addr_t dma, u64 ep_addr, u32 len)
{
	struct qdma_sw_sg sg;
	struct qdma_request req;
	ssize_t rv;

	memset(&sg, 0, sizeof(sg));
	sg.next     = NULL;
	sg.pg       = NULL;            /* virt_to_page not needed when dma_mapped=1 */
	sg.offset   = 0;
	sg.len      = len;
	sg.dma_addr = dma;

	memset(&req, 0, sizeof(req));
	req.sgl         = &sg;
	req.sgcnt       = 1;
	req.count       = len;
	req.ep_addr     = ep_addr;
	req.write       = write ? 1 : 0;
	req.dma_mapped  = 1;           /* staging is already coherent */
	req.no_memcpy   = 1;           /* we already memcpy'd in/out */
	req.timeout_ms  = ONIC_SYSDMA_TIMEOUT_MS;
	req.fp_done     = NULL;        /* NULL => blocking submit */

	rv = qdma_request_submit(priv->qdma_dev_handle, qhndl, &req);
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: %s submit failed (%zd) ep=0x%llx len=%u dma=0x%llx\n",
			write ? "H2C" : "C2H",
			rv, ep_addr, len, (u64)dma);
		return (int)rv;
	}
	if ((u32)rv != len) {
		dev_warn(&priv->pdev->dev,
			 "onic_sysdma: short xfer (%zd of %u) ep=0x%llx\n",
			 rv, len, ep_addr);
		return -EIO;
	}
	return 0;
}

/* ----------------------------------------------------------------- *
 *  Public API
 * ----------------------------------------------------------------- */

int onic_sysdma_init(struct onic_private *priv)
{
	struct device *dev;
	struct onic_sysdma_state *s;
	int rv;

	if (!priv) {
		return -EINVAL;
	}
	dev = &priv->pdev->dev;
	if (priv->sysdma) {
		dev_warn(dev, "onic_sysdma: already initialised\n");
		return -EBUSY;
	}
	if (!priv->qdma_dev_handle) {
		dev_warn(dev,
			 "onic_sysdma: libqdma device handle is zero — skipping\n");
		return -ENODEV;
	}

	/* Widen coherent mask: libqdma's queue_add allocates internal
	 * descriptor rings via dma_alloc_coherent.  Our driver-wide
	 * default is 32-bit, which gives addresses in the 0xff?? range
	 * on this platform that the QDMA MM engine has historically
	 * mishandled.  64-bit lets the IOMMU give libqdma cleaner IOVAs. */
	rv = dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
	if (rv) {
		dev_warn(dev,
			 "onic_sysdma: failed to widen coherent mask (%d) — using 32-bit\n",
			 rv);
	}

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		rv = -ENOMEM;
		goto err_restore_mask;
	}
	mutex_init(&s->lock);

	/* Bring up the H2C MM queue first — this is the load-bearing
	 * path for the original sysdma host->DDR4 self-test.  C2H is
	 * needed for ddr4_read and the round-trip self-test. */
	rv = sysdma_add_and_start(priv, Q_H2C, &s->qhndl_h2c);
	if (rv < 0) {
		goto err_free_state;
	}
	s->h2c_started = true;

	rv = sysdma_add_and_start(priv, Q_C2H, &s->qhndl_c2h);
	if (rv < 0) {
		dev_warn(dev,
			 "onic_sysdma: C2H setup failed (%d) — write-only mode\n",
			 rv);
		/* Not fatal: legacy callers only used H2C anyway. */
		s->c2h_started = false;
		s->qhndl_c2h   = 0;
	} else {
		s->c2h_started = true;
	}

	/* Staging buffer for payload memcpy.  One ONIC_SYSDMA_MAX_XFER
	 * slab reused across calls.  Coherent so we don't have to manage
	 * dma_sync_*. */
	s->staging_size = ONIC_SYSDMA_MAX_XFER;
	s->staging = dma_alloc_coherent(dev, s->staging_size,
					&s->staging_dma, GFP_KERNEL);
	if (!s->staging) {
		rv = -ENOMEM;
		goto err_remove_queues;
	}

	s->initialised = true;
	priv->sysdma = s;

	dev_info(dev,
		 "onic_sysdma: ready via libqdma (h2c=%lu c2h=%lu staging_dma=0x%llx max_xfer=%u)\n",
		 s->qhndl_h2c, s->qhndl_c2h, (u64)s->staging_dma,
		 ONIC_SYSDMA_MAX_XFER);

	/* Restore 32-bit coherent mask so other driver paths see the
	 * environment they were probed under.  Already-allocated
	 * coherent regions remain valid. */
	(void)dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	return 0;

err_remove_queues:
	sysdma_stop_and_remove(priv, &s->qhndl_c2h, &s->c2h_started, "C2H");
	sysdma_stop_and_remove(priv, &s->qhndl_h2c, &s->h2c_started, "H2C");
err_free_state:
	mutex_destroy(&s->lock);
	kfree(s);
err_restore_mask:
	(void)dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	return rv;
}

void onic_sysdma_fini(struct onic_private *priv)
{
	struct device *dev;
	struct onic_sysdma_state *s;

	if (!priv) {
		return;
	}
	s = priv->sysdma;
	if (!s) {
		return;
	}
	dev = &priv->pdev->dev;
	priv->sysdma = NULL;  /* prevent re-entry from racing callers */

	if (s->staging) {
		dma_free_coherent(dev, s->staging_size, s->staging,
				  s->staging_dma);
	}

	if (priv->qdma_dev_handle) {
		sysdma_stop_and_remove(priv, &s->qhndl_c2h,
				       &s->c2h_started, "C2H");
		sysdma_stop_and_remove(priv, &s->qhndl_h2c,
				       &s->h2c_started, "H2C");
	}

	mutex_destroy(&s->lock);
	kfree(s);
}

int onic_ddr4_write(struct onic_private *priv, u64 dst_axi,
		    const void *src, size_t len)
{
	struct onic_sysdma_state *s;
	int ret;

	if (!priv || !src || len == 0 || len > ONIC_SYSDMA_MAX_XFER) {
		return -EINVAL;
	}
	s = priv->sysdma;
	if (!s || !s->initialised || !s->h2c_started) {
		return -ENODEV;
	}

	mutex_lock(&s->lock);
	memcpy(s->staging, src, len);
	wmb();   /* host write visible to QDMA before submit */

	ret = sysdma_submit_blocking(priv, s->qhndl_h2c, /*write=*/true,
				     s->staging_dma, dst_axi, (u32)len);
	mutex_unlock(&s->lock);
	return ret;
}

int onic_ddr4_read(struct onic_private *priv, void *dst,
		   u64 src_axi, size_t len)
{
	struct onic_sysdma_state *s;
	int ret;

	if (!priv || !dst || len == 0 || len > ONIC_SYSDMA_MAX_XFER) {
		return -EINVAL;
	}
	s = priv->sysdma;
	if (!s || !s->initialised || !s->c2h_started) {
		return -ENODEV;
	}

	mutex_lock(&s->lock);

	ret = sysdma_submit_blocking(priv, s->qhndl_c2h, /*write=*/false,
				     s->staging_dma, src_axi, (u32)len);
	if (ret == 0) {
		rmb();   /* QDMA write visible before host read */
		memcpy(dst, s->staging, len);
	}

	mutex_unlock(&s->lock);
	return ret;
}

/* ----------------------------------------------------------------- *
 *  Probe-time self-test — write 64 B, read it back, memcmp.
 * ----------------------------------------------------------------- */

int onic_sysdma_self_test(struct onic_private *priv)
{
	static const u8 pattern[64] =
		"ONIC_SYSDMA_LIBQDMA_SELF_TEST_64B___v5_with_round_trip_2026";
	const u64 dst_axi_offset = 0x0;
	u8 readback[64] = {0};
	int rv;

	if (!priv || !priv->sysdma) {
		return -ENODEV;
	}

	dev_info(&priv->pdev->dev,
		 "onic_sysdma: self-test — writing 64 B to DDR4 @ 0x%llx (libqdma)\n",
		 dst_axi_offset);

	rv = onic_ddr4_write(priv, dst_axi_offset, pattern, sizeof(pattern));
	if (rv) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: self-test WRITE FAILED: %d\n", rv);
		return rv;
	}

	if (!priv->sysdma->c2h_started) {
		dev_warn(&priv->pdev->dev,
			 "onic_sysdma: self-test write OK; C2H not online so skipping read-back\n");
		return 0;
	}

	rv = onic_ddr4_read(priv, readback, dst_axi_offset, sizeof(readback));
	if (rv) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: self-test READ FAILED: %d\n", rv);
		return rv;
	}

	if (memcmp(pattern, readback, sizeof(pattern)) != 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: self-test DATA MISMATCH — H2C wrote, C2H read, bytes differ\n");
		print_hex_dump(KERN_ERR, "expect: ", DUMP_PREFIX_OFFSET,
			       16, 1, pattern, sizeof(pattern), true);
		print_hex_dump(KERN_ERR, "got:    ", DUMP_PREFIX_OFFSET,
			       16, 1, readback, sizeof(readback), true);
		return -EIO;
	}

	dev_info(&priv->pdev->dev,
		 "onic_sysdma: self-test OK — H2C+C2H round-trip verified (libqdma)\n");
	return 0;
}
