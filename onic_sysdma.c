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
#include "qdma_access/qdma_register.h"

/* ------------------------------------------------------------------------- *
 *  Per-queue state — embedded in onic_private as priv->sysdma (TODO: add
 *  the field to onic.h).  Kept opaque to other files for now.
 * ------------------------------------------------------------------------- */
struct onic_sysdma_state {
	struct mutex            lock;       /* serialises ddr4_write/read */
	bool                    initialised;

	/* QDMA queue identity */
	u16                     qid;        /* absolute qid (qid_base + offset) */
	u16                     pidx;       /* next descriptor index to push */
	u16                     cidx;       /* completed index, polled from CMPT */

	/* Descriptor ring (MM format, 32B/desc) */
	void                   *desc_ring;
	dma_addr_t              desc_ring_dma;
	size_t                  desc_ring_size;

	/* Completion ring */
	void                   *cmpt_ring;
	dma_addr_t              cmpt_ring_dma;
	size_t                  cmpt_ring_size;

	/* Staging buffer for write payloads.  Reused across calls. */
	void                   *staging;
	dma_addr_t              staging_dma;
	size_t                  staging_size;
};

/* ------------------------------------------------------------------------- *
 *  MM descriptor packing.  QDMA's docs use little-endian 32-byte structs.
 *  The first 16 bytes carry src/dst addresses; len + flags pack into the
 *  next 4 bytes; the rest is reserved-zero.
 * ------------------------------------------------------------------------- */
struct qdma_mm_desc {
	__le64 src;
	__le64 dst;
	__le32 len_flags;       /* [27:0] length, [28] sop, [29] eop, rest rsvd */
	__le32 reserved[3];
} __packed;

#define QDMA_MM_FLAG_SOP   (1u << 28)
#define QDMA_MM_FLAG_EOP   (1u << 29)

static void onic_sysdma_pack_desc(struct qdma_mm_desc *d,
				  dma_addr_t src, u64 dst, u32 len)
{
	memset(d, 0, sizeof(*d));
	d->src       = cpu_to_le64((u64)src);
	d->dst       = cpu_to_le64(dst);
	d->len_flags = cpu_to_le32((len & 0x0FFFFFFFu) |
				   QDMA_MM_FLAG_SOP | QDMA_MM_FLAG_EOP);
}

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
 *  Three integration options (rough effort estimates):
 *
 *    A) Full libqdma integration (2-3 days).  Vendor the entire library
 *       into open-nic-driver/.  Best long-term — AMD-tested and gets
 *       MM, descriptor bypass, indirect interrupts, mailbox, debugfs
 *       all working.  Cost: rename our cut-down qdma_access/ to avoid
 *       symbol conflicts; rework onic_hardware.c queue init to use
 *       libqdma APIs; risks regressions on netdev TX/RX.
 *
 *    B) Cherry-pick the MM submit path (1-1.5 days).  Copy
 *       qdma_descq.{c,h}, the relevant qdma_context.c bits, and their
 *       qdma_access dependencies into a new open-nic-driver/qdma_mm/
 *       subdir.  Strip dependencies on libqdma's broader infra
 *       (mailbox, indirect intr, debugfs).  Self-contained.  Cost:
 *       creates two QDMA libs in the driver (technical debt).
 *
 *    C) Hand-write the MM submit path using libqdma as documentation
 *       (1-2 days).  Stay in our existing qdma_access framework, add
 *       MM helpers modelled after libqdma's qdma_descq_mm.c logic.
 *       Cost: more code to debug ourselves; less proven than (A)/(B).
 *
 *  Recommendation: (B) for v1.  Bounded, doesn't disturb netdev path,
 *  can migrate to (A) once B7 is functionally proven.
 * ------------------------------------------------------------------------- */
static int onic_sysdma_program_qctx(struct onic_private *priv,
				    struct onic_sysdma_state *s)
{
	/* TODO: implement QDMA H2C MM context write.  Roughly:
	 *
	 *   struct qdma_h2c_sw_ctxt ctxt = {0};
	 *   ctxt.qen      = 1;
	 *   ctxt.fcrd_en  = 0;       // we use direct doorbell PIDX, not credit
	 *   ctxt.wbi_chk  = 1;
	 *   ctxt.wbi_intvl_en = 0;   // poll completions, no MSI-X for now
	 *   ctxt.fnc_id   = priv->cmac_id;
	 *   ctxt.rngsz_idx = ONIC_SYSDMA_RING_DEPTH log2 index;
	 *   ctxt.dsc_base = s->desc_ring_dma;
	 *   ctxt.is_mm    = 1;       // <-- the MM-mode bit
	 *   ctxt.mrkr_dis = 0;
	 *   ctxt.irq_en   = 0;
	 *
	 *   qdma_program_ctxt(qdev, QDMA_CTXT_H2C_SW, s->qid, &ctxt);
	 *
	 * Plus the corresponding HW context (QDMA_CTXT_H2C_HW) and CMPT
	 * context if we want CMPT-based completion notification.
	 *
	 * The existing onic_qdma_init_tx_queue at onic_hardware.c:520 does
	 * the equivalent for ST mode — model after that.  Or factor common
	 * code out and parameterise on is_mm.
	 */
	dev_warn(&priv->pdev->dev,
		 "onic_sysdma: program_qctx is a stub; MM mode not yet active. qid=%u\n",
		 s->qid);
	return -EOPNOTSUPP;
}

static void onic_sysdma_clear_qctx(struct onic_private *priv,
				   struct onic_sysdma_state *s)
{
	/* TODO: invalidate the H2C SW + HW contexts before freeing the
	 * descriptor ring.  Use qdma_program_ctxt with all-zero ctxt and
	 * qen=0, then issue an invalidate via QDMA_OFFSET_CTXT_DATA_*
	 * registers.  Failing to do this leaves the QDMA engine with a
	 * stale DMA address pointing into freed kernel memory — usual
	 * IOMMU fault if SR-IOV is enabled, silent corruption otherwise.
	 */
	(void)priv;
	(void)s;
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

	/* TODO(onic.h): add `struct onic_sysdma_state *sysdma;` to
	 * struct onic_private and replace this kmalloc with kzalloc into
	 * priv->sysdma.  For now, allocate locally and return via priv
	 * by some other means (caller stashes it). */
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		return -ENOMEM;
	}

	mutex_init(&s->lock);
	/* TODO: derive absolute qid from priv->qid_base + offset, once the
	 * qdma_dev exposes the queue-base accessor.  For now, hard-code the
	 * relative offset. */
	s->qid  = ONIC_SYSDMA_QID_OFFSET;
	s->pidx = 0;
	s->cidx = 0;

	/* Descriptor ring — coherent so QDMA can read without explicit
	 * cache flushes.  256 entries * 32 B = 8 KiB. */
	s->desc_ring_size = ONIC_SYSDMA_RING_DEPTH * ONIC_SYSDMA_DESC_SIZE;
	s->desc_ring = dma_alloc_coherent(dev, s->desc_ring_size,
					  &s->desc_ring_dma, GFP_KERNEL);
	if (!s->desc_ring) {
		ret = -ENOMEM;
		goto err_free_state;
	}
	memset(s->desc_ring, 0, s->desc_ring_size);

	/* Completion ring — 8 B per entry (status + 16-bit cidx).
	 * Same depth as desc ring. */
	s->cmpt_ring_size = ONIC_SYSDMA_RING_DEPTH * 8;
	s->cmpt_ring = dma_alloc_coherent(dev, s->cmpt_ring_size,
					  &s->cmpt_ring_dma, GFP_KERNEL);
	if (!s->cmpt_ring) {
		ret = -ENOMEM;
		goto err_free_desc;
	}
	memset(s->cmpt_ring, 0, s->cmpt_ring_size);

	/* Staging buffer — reused across writes, sized to ONIC_SYSDMA_MAX_XFER. */
	s->staging_size = ONIC_SYSDMA_MAX_XFER;
	s->staging = dma_alloc_coherent(dev, s->staging_size,
					&s->staging_dma, GFP_KERNEL);
	if (!s->staging) {
		ret = -ENOMEM;
		goto err_free_cmpt;
	}

	/* Program QDMA queue context — this is the load-bearing TODO. */
	ret = onic_sysdma_program_qctx(priv, s);
	if (ret) {
		goto err_free_staging;
	}

	s->initialised = true;
	dev_info(dev, "onic_sysdma: ready (qid=%u, ring_depth=%u, max_xfer=%u)\n",
		 s->qid, ONIC_SYSDMA_RING_DEPTH, ONIC_SYSDMA_MAX_XFER);

	/* TODO: stash s in priv->sysdma.  For now, leak the pointer to
	 * avoid driver build issues until onic.h is amended. */
	(void)s; /* silence unused warning */
	return 0;

err_free_staging:
	dma_free_coherent(dev, s->staging_size, s->staging, s->staging_dma);
err_free_cmpt:
	dma_free_coherent(dev, s->cmpt_ring_size, s->cmpt_ring, s->cmpt_ring_dma);
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
	dev = &priv->pdev->dev;
	/* TODO: retrieve s from priv->sysdma once the field is added. */
	s = NULL;
	if (!s || !s->initialised) {
		return;
	}

	onic_sysdma_clear_qctx(priv, s);

	dma_free_coherent(dev, s->staging_size, s->staging, s->staging_dma);
	dma_free_coherent(dev, s->cmpt_ring_size, s->cmpt_ring, s->cmpt_ring_dma);
	dma_free_coherent(dev, s->desc_ring_size, s->desc_ring, s->desc_ring_dma);

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
	struct qdma_mm_desc *ring = s->desc_ring;
	struct qdma_mm_desc *d;
	u16 slot;
	unsigned long deadline;

	if (len == 0 || len > ONIC_SYSDMA_MAX_XFER) {
		return -EINVAL;
	}

	slot = s->pidx & (ONIC_SYSDMA_RING_DEPTH - 1);
	d    = &ring[slot];

	onic_sysdma_pack_desc(d, src, dst, len);
	wmb(); /* descriptor visible to QDMA before doorbell */

	s->pidx = (s->pidx + 1) & (ONIC_SYSDMA_RING_DEPTH - 1);

	/* TODO: ring the H2C PIDX doorbell.  Per PG302 §3.7, write
	 * priv->hw.addr + qdma_doorbell_offset(s->qid) with the new PIDX.
	 * The exact offset depends on the QDMA register map — see
	 * QDMA_OFFSET_DMAP_SEL_H2C_DBELL_BASE in qdma_register.h.  Worth
	 * factoring into a small helper. */

	/* TODO: poll the CMPT ring for slot completion or timeout. */
	deadline = jiffies + msecs_to_jiffies(ONIC_SYSDMA_TIMEOUT_MS);
	while (time_before(jiffies, deadline)) {
		/* Read CMPT entry at s->cidx, check if engine consumed our slot.
		 * For now, just busy-loop a stub. */
		cpu_relax();
		break; /* TODO: real completion check; for now succeed once for
			* compile-test only.  Remove this break. */
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
	/* TODO: s = priv->sysdma; */
	s = NULL;
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
