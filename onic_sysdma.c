// SPDX-License-Identifier: GPL-2.0
/*
 * onic_sysdma.c - QDMA AXI-MM-mode system DMA for host <-> card-DDR4
 *
 * See onic_sysdma.h for design rationale.
 *
 * Hardware path: host buffer -> coherent staging -> QDMA H2C MM descriptor
 *   -> dev_mem 4:1 crossbar (s_axi_qdma_mm_* slave) -> DDR4 controller.
 *
 * The QDMA IP must have been generated with en_axi_mm_qdma=true and
 * dma_intf_sel=AXI_MM_and_AXI_Stream_with_Completion (v4 bitstream config).
 * The wrapper's qdma_no_sriov instantiation must drive m_axi_* into the
 * dev_mem crossbar (commit b18430a).  Without those, the MM engine is
 * either inactive (older bitstream) or the m_axi_* outputs dangle.
 *
 * MM descriptor format (PG302 v5.1 Ch 5, 32 bytes):
 *   [255:192] reserved
 *   [191:160] sop/eop control + reserved
 *   [159]     valid (driven by QDMA itself based on PIDX, not user-set)
 *   [158]     eop
 *   [157]     sop
 *   [156:128] length (28 bits, byte count, max 256 MiB per descriptor)
 *   [127:64]  dst_addr (card-side AXI address)
 *   [63:0]    src_addr (host-side PCIe address; for H2C this is dma_addr_t)
 */

#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/pci.h>

#include "onic.h"
#include "onic_sysdma.h"
#include "onic_qdma_mm.h"
#include "qdma_access/qdma_register.h"
#include "qdma_access/qdma_context.h"
#include "qdma_access/qdma_device.h"
#include "qdma_access/qdma_export.h"

/* QDMA ring-size pool index for 256 entries (matches rngcnt_pool[4]
 * in onic_hardware.c). */
#define ONIC_SYSDMA_RNGSZ_IDX  4

/* Reserved qid (relative to function's qid_base).  Netdev uses 0..N-1
 * for its TX/RX queues; we use a value past the netdev range.  Queue
 * 31 is well past 14-queue netdev allocations and within typical
 * num_q=2048 caps. */
#define ONIC_SYSDMA_REL_QID    31

/* ------------------------------------------------------------------------- *
 *  Per-queue state — embedded in onic_private as priv->sysdma (TODO: add
 *  the field to onic.h).  Kept opaque to other files for now.
 *
 *  Memory layout of desc_ring buffer:
 *
 *      offset 0:                 onic_qdma_mm_desc[0]
 *      offset 32:                onic_qdma_mm_desc[1]
 *      ...
 *      offset (N-1)*32:          onic_qdma_mm_desc[N-1]
 *      offset N*32:              onic_qdma_wb_status (8 bytes)
 *
 *  QDMA writes wb_status here when wbi_chk=1; we poll cidx for completion.
 * ------------------------------------------------------------------------- */
struct onic_sysdma_state {
	struct mutex            lock;       /* serialises ddr4_write/read */
	bool                    initialised;

	/* QDMA queue identity */
	u16                     qid;        /* relative qid within this qdev */
	u16                     pidx;       /* next descriptor index to push */

	/* Descriptor ring (MM format, 32 B/desc) + wb_status sentinel.
	 * Allocated size = (ring_depth + 1) * 32. */
	void                   *desc_ring;
	dma_addr_t              desc_ring_dma;
	size_t                  desc_ring_size;
	struct onic_qdma_wb_status *wb_status; /* points into desc_ring */

	/* Staging buffer for write payloads.  Reused across calls. */
	void                   *staging;
	dma_addr_t              staging_dma;
	size_t                  staging_size;
};

/* MM descriptor + WB-status structs: see onic_qdma_mm.h. */

/* ------------------------------------------------------------------------- *
 *  QDMA queue context programming for MM mode.
 *
 *  Reference: AMD's open-source dma_ip_drivers / libqdma implements this
 *  fully.  Path on this machine:
 *      /home/alex/fpga-wksp/dma_ip_drivers/QDMA/linux-kernel/driver/libqdma/
 *
 *  Key entry points in libqdma:
 *      qdma_queue_add()       libqdma_export.h:1248  — config + register
 *      qdma_queue_start()     libqdma_export.h:1277  — bring online
 *      qdma_request_submit()  libqdma_export.h:1462  — submit DMA work
 *
 *  Mode selection is qdma_queue_conf.st = 0 (MM) or 1 (ST).
 *  The MM-specific submit logic lives in qdma_descq.c:
 *      descq_mm_proc_request()
 *      descq_mm_n_h2c_cmpl_status()
 *      descq_poll_mm_n_h2c_cmpl_status()
 *
 *  Implementation plan (DECIDED — see commit log for the pivot):
 *
 *  Hand-roll the minimal MM submit path using libqdma as DOCUMENTATION,
 *  not as a vendored library.  Reasoning:
 *
 *    - libqdma is a monolithic driver, not a cherry-pickable component.
 *      Transitive header closure of qdma_request_submit is 22 headers
 *      and ~11K LoC across the core .c files (qdma_descq, qdma_context,
 *      libqdma_export, qdma_device, qdma_regs, xdev, qdma_intr,
 *      qdma_st_c2h).  Every "core" file pulls in 5-8 others.
 *
 *    - Our requirements are tiny compared to libqdma's surface area:
 *      one MM queue, sync H2C, sync C2H, poll completions, no
 *      PF/VF, no mailbox, no descriptor bypass, no indirect intr,
 *      no debugfs.  Hand-rolling gives ~300-500 LoC; cherry-picking
 *      would import most of libqdma anyway, with weeks of dependency-
 *      untangling.
 *
 *    - We already have qdma_access/qdma_register.h with the register
 *      offsets we need.  Hand-rolling extends our existing minimal
 *      framework rather than introducing a parallel one.
 *
 *  Concrete reference points in libqdma (read these when implementing):
 *
 *    H2C SW context format & programming:
 *      qdma_context.c:make_qdma_descq_sw_ctxt + qdma_indirect_reg_write
 *      qdma_descq.c:descq_h2c_pidx_update for doorbell
 *
 *    MM submit logic:
 *      qdma_descq.c:descq_mm_proc_request (the request-to-descriptor
 *      conversion) and qdma_request_submit() in libqdma_export.c
 *
 *    Completion polling:
 *      qdma_descq.c:descq_mm_n_h2c_cmpl_status
 *      qdma_descq.c:descq_poll_mm_n_h2c_cmpl_status
 *
 *    Indirect register access (used to program contexts):
 *      qdma_access/qdma_access_common.c:qdma_indirect_reg_write
 *      Registers: QDMA_OFFSET_IND_CTXT_DATA (0x804..0x814),
 *                 QDMA_OFFSET_IND_CTXT_MASK (0x824..0x834),
 *                 QDMA_OFFSET_IND_CTXT_CMD  (0x844)
 * ------------------------------------------------------------------------- */
static int onic_sysdma_program_qctx(struct onic_private *priv,
				    struct onic_sysdma_state *s)
{
	struct qdma_dev *qdev = (struct qdma_dev *)priv->hw.qdma;
	struct qdma_sw_ctxt sw_ctxt;
	int rv;

	if (!qdev) {
		dev_err(&priv->pdev->dev, "onic_sysdma: priv->hw.qdma is NULL\n");
		return -ENODEV;
	}

	/* Build H2C SW context for MM mode.  Modeled after
	 * onic_qdma_init_tx_queue() in onic_hardware.c, but with:
	 *   is_mm    = 1     (vs 0 for ST netdev)
	 *   desc_sz  = 2     (32-byte MM desc, vs 1=16B for ST H2C)
	 *   wbi_chk  = 1     (writeback status enabled)
	 *   wbi_intvl_en = 0 (write status after every descriptor, no batching)
	 *   irq_en   = 0     (poll mode — no MSI-X for sysdma)
	 *   fcrd_en  = 0     (direct PIDX doorbell, not credit-based)
	 */
	memset(&sw_ctxt, 0, sizeof(sw_ctxt));
	sw_ctxt.func_id      = qdev->func_id;
	sw_ctxt.qen          = 1;
	sw_ctxt.is_mm        = 1;
	sw_ctxt.wbk_en       = 1;
	sw_ctxt.wbi_chk      = 1;
	sw_ctxt.wbi_intvl_en = 0;
	sw_ctxt.irq_arm      = 0;
	sw_ctxt.irq_en       = 0;
	sw_ctxt.desc_sz      = 2;       /* 32B MM descriptor */
	sw_ctxt.fcrd_en      = 0;
	sw_ctxt.at           = 0;
	sw_ctxt.rngsz_idx    = ONIC_SYSDMA_RNGSZ_IDX;
	sw_ctxt.desc_base    = s->desc_ring_dma;
	sw_ctxt.vec          = 0;
	sw_ctxt.intr_aggr    = 0;

	/* Clear any stale state, then write our context. */
	rv = qdma_clear_sw_ctxt(qdev, s->qid, QDMA_H2C);
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: clear_sw_ctxt qid=%u failed: %d\n",
			s->qid, rv);
		return rv;
	}
	rv = qdma_clear_hw_ctxt(qdev, s->qid, QDMA_H2C);
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: clear_hw_ctxt qid=%u failed: %d\n",
			s->qid, rv);
		return rv;
	}
	rv = qdma_clear_cr_ctxt(qdev, s->qid, QDMA_H2C);
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: clear_cr_ctxt qid=%u failed: %d\n",
			s->qid, rv);
		return rv;
	}

	rv = qdma_write_sw_ctxt(qdev, s->qid, QDMA_H2C, &sw_ctxt);
	if (rv < 0) {
		dev_err(&priv->pdev->dev,
			"onic_sysdma: write_sw_ctxt qid=%u failed: %d\n",
			s->qid, rv);
		return rv;
	}

	dev_info(&priv->pdev->dev,
		 "onic_sysdma: queue programmed qid=%u (MM mode, ring=256, desc_sz=32B)\n",
		 s->qid);
	return 0;
}

static void onic_sysdma_clear_qctx(struct onic_private *priv,
				   struct onic_sysdma_state *s)
{
	struct qdma_dev *qdev = (struct qdma_dev *)priv->hw.qdma;

	if (!qdev || !s) {
		return;
	}

	/* Invalidate first (engine stops fetching), then clear (slot
	 * marked free for reuse).  Order matters: clearing without
	 * invalidating can race with in-flight descriptor fetches. */
	qdma_invalidate_sw_ctxt(qdev, s->qid, QDMA_H2C);
	qdma_invalidate_hw_ctxt(qdev, s->qid, QDMA_H2C);
	qdma_clear_sw_ctxt(qdev, s->qid, QDMA_H2C);
	qdma_clear_hw_ctxt(qdev, s->qid, QDMA_H2C);
	qdma_clear_cr_ctxt(qdev, s->qid, QDMA_H2C);
}

/* ------------------------------------------------------------------------- *
 *  Public API
 * ------------------------------------------------------------------------- */

int onic_sysdma_init(struct onic_private *priv)
{
	struct device *dev = &priv->pdev->dev;
	struct onic_sysdma_state *s;
	int ret;

	if (!priv) {
		return -EINVAL;
	}
	if (priv->sysdma) {
		dev_warn(&priv->pdev->dev,
			 "onic_sysdma: already initialised\n");
		return -EBUSY;
	}

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		return -ENOMEM;
	}

	mutex_init(&s->lock);
	/* TODO: derive absolute qid from priv->qid_base + offset, once the
	 * qdma_dev exposes the queue-base accessor.  For now, hard-code the
	 * relative offset. */
	s->qid  = ONIC_SYSDMA_REL_QID;
	s->pidx = 0;

	/* Descriptor ring + wb_status sentinel in one coherent buffer.
	 * Size = (ring_depth + 1) * 32 B = 8 KiB + 32 B (sentinel takes
	 * a full descriptor slot to keep alignment).  QDMA writes the
	 * wb_status word at offset ring_depth*32 when wbi_chk=1. */
	s->desc_ring_size = (ONIC_SYSDMA_RING_DEPTH + 1) * ONIC_SYSDMA_DESC_SIZE;
	s->desc_ring = dma_alloc_coherent(dev, s->desc_ring_size,
					  &s->desc_ring_dma, GFP_KERNEL);
	if (!s->desc_ring) {
		ret = -ENOMEM;
		goto err_free_state;
	}
	memset(s->desc_ring, 0, s->desc_ring_size);
	s->wb_status = (struct onic_qdma_wb_status *)
		((u8 *)s->desc_ring + (ONIC_SYSDMA_RING_DEPTH * ONIC_SYSDMA_DESC_SIZE));

	/* Staging buffer — reused across writes, sized to ONIC_SYSDMA_MAX_XFER. */
	s->staging_size = ONIC_SYSDMA_MAX_XFER;
	s->staging = dma_alloc_coherent(dev, s->staging_size,
					&s->staging_dma, GFP_KERNEL);
	if (!s->staging) {
		ret = -ENOMEM;
		goto err_free_desc;
	}

	/* Program QDMA queue context — this is the load-bearing TODO. */
	ret = onic_sysdma_program_qctx(priv, s);
	if (ret) {
		goto err_free_staging;
	}

	s->initialised = true;
	priv->sysdma = s;
	dev_info(dev, "onic_sysdma: ready (qid=%u, ring_depth=%u, max_xfer=%u)\n",
		 s->qid, ONIC_SYSDMA_RING_DEPTH, ONIC_SYSDMA_MAX_XFER);
	return 0;

err_free_staging:
	dma_free_coherent(dev, s->staging_size, s->staging, s->staging_dma);
err_free_desc:
	dma_free_coherent(dev, s->desc_ring_size, s->desc_ring, s->desc_ring_dma);
err_free_state:
	kfree(s);
	return ret;
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

	if (s->initialised) {
		onic_sysdma_clear_qctx(priv, s);
	}

	if (s->staging) {
		dma_free_coherent(dev, s->staging_size, s->staging, s->staging_dma);
	}
	if (s->desc_ring) {
		dma_free_coherent(dev, s->desc_ring_size, s->desc_ring, s->desc_ring_dma);
	}

	mutex_destroy(&s->lock);
	kfree(s);
}

/* ------------------------------------------------------------------------- *
 *  ddr4_write / ddr4_read core path
 * ------------------------------------------------------------------------- */

static int onic_sysdma_submit_one(struct onic_private *priv,
				  struct onic_sysdma_state *s,
				  dma_addr_t src, u64 dst, u32 len)
{
	struct onic_qdma_mm_desc *ring = s->desc_ring;
	struct onic_qdma_mm_desc *d;
	u16 slot, expected_cidx;
	unsigned long deadline;
	u16 cidx;

	if (len == 0 || len > ONIC_SYSDMA_MAX_XFER) {
		return -EINVAL;
	}

	slot = s->pidx & (ONIC_SYSDMA_RING_DEPTH - 1);
	d    = &ring[slot];

	onic_qdma_pack_mm_desc(d, (u64)src, dst, len);
	wmb(); /* descriptor visible to QDMA before doorbell */

	s->pidx = (s->pidx + 1) & (ONIC_SYSDMA_RING_DEPTH - 1);
	expected_cidx = s->pidx;  /* QDMA bumps cidx to match pidx on completion */

	/* Ring the H2C PIDX doorbell via the existing helper.  This writes
	 * QDMA_OFFSET_DMAP_SEL_H2C_DESC_PIDX + abs_qid*16 with the new
	 * pidx.  irq_arm=0 since we poll. */
	onic_set_tx_head(priv->hw.qdma, s->qid, s->pidx);

	/* Poll the wb_status sentinel.  QDMA writes pidx/cidx into this
	 * 8-byte word at the end of the descriptor ring after each
	 * completed batch (wbi_chk=1, wbi_intvl_en=0 → write per descriptor).
	 *
	 * Reference: libqdma qdma_descq.c:descq_mm_n_h2c_cmpl_status. */
	deadline = jiffies + msecs_to_jiffies(ONIC_SYSDMA_TIMEOUT_MS);
	for (;;) {
		rmb(); /* re-read wb_status fresh from coherent memory */
		cidx = le16_to_cpu(s->wb_status->cidx);
		if (cidx == expected_cidx) {
			break;
		}
		if (time_after(jiffies, deadline)) {
			dev_err(&priv->pdev->dev,
				"onic_sysdma: timeout waiting for cidx=%u (got %u, pidx=%u)\n",
				expected_cidx, cidx, s->pidx);
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	return 0;
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
	if (!s || !s->initialised) {
		return -ENODEV;
	}

	mutex_lock(&s->lock);

	memcpy(s->staging, src, len);
	wmb(); /* host-side write visible before QDMA reads */

	ret = onic_sysdma_submit_one(priv, s, s->staging_dma, dst_axi, len);

	mutex_unlock(&s->lock);
	return ret;
}

int onic_ddr4_read(struct onic_private *priv, void *dst,
		   u64 src_axi, size_t len)
{
	/* TODO: implement.  Same shape as write but uses the C2H MM path
	 * (descriptor src=DDR4_AXI, dst=staging_dma).  Then memcpy from
	 * staging into caller's dst.  Requires a separate C2H MM queue +
	 * context programming, similar to the H2C side. */
	(void)priv;
	(void)dst;
	(void)src_axi;
	(void)len;
	return -EOPNOTSUPP;
}
