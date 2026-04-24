/*
 * Copyright (c) 2026 Tenstorrent Inc.
 *
 * onic_ernic_irq.c — F5 MSI-X dispatch skeleton for dual-ERNIC.
 *
 * Register offsets are relative to each ERNIC's 2 MB slice in BAR2.
 * Master PF only.  Non-master / secondary netdev / VFs see no-op setup
 * and teardown.  F5 is log-only — the real completion-processing
 * handlers land in B8/B9 once the ib_device skeleton exists.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>

#include "onic.h"
#include "onic_hardware.h"
#include "onic_ernic_irq.h"

/* Offsets within a single ERNIC's 2 MB slice (ERNIC0 and ERNIC1 use the
 * same internal layout; apply ctx->ernic_base).  Values mirror F1-audited
 * reconic_reg.h but are kept local here so the driver doesn't pull in the
 * userspace header. */
#define ERNIC_OFFSET_INTEN                0x00100180
#define ERNIC_OFFSET_INTSTS               0x00100184
#define ERNIC_OFFSET_RQINTSTS_BASE        0x00100190  /* 64 banks × 4 bytes */
#define ERNIC_OFFSET_CQINTSTS_BASE        0x00100290
#define ERNIC_OFFSET_CNPSCHDSTS_BASE      0x00100390

/* INTSTS / INTEN bit positions — per F1 audit §5.3, PG332 v4.2 Table 8. */
#define ERNIC_INTSTS_PKTVALERR     BIT(0)
#define ERNIC_INTSTS_MADRX         BIT(1)
/* bit 2 RSVD */
#define ERNIC_INTSTS_RNRNACKGEN    BIT(3)
#define ERNIC_INTSTS_WQECOMPL      BIT(4)
#define ERNIC_INTSTS_ILLOPCODE     BIT(5)
#define ERNIC_INTSTS_RQPKT         BIT(6)
#define ERNIC_INTSTS_FATALERR      BIT(7)
#define ERNIC_INTSTS_CNPSCHD       BIT(8)
#define ERNIC_INTSTS_ALL_OWNED     (ERNIC_INTSTS_PKTVALERR   | \
				    ERNIC_INTSTS_MADRX       | \
				    ERNIC_INTSTS_RNRNACKGEN  | \
				    ERNIC_INTSTS_WQECOMPL    | \
				    ERNIC_INTSTS_ILLOPCODE   | \
				    ERNIC_INTSTS_RQPKT       | \
				    ERNIC_INTSTS_FATALERR    | \
				    ERNIC_INTSTS_CNPSCHD)

static inline u32 ernic_rd(struct onic_private *priv,
			   struct onic_ernic_irq_ctx *ctx, u32 off)
{
	return ioread32(priv->hw.addr + ctx->ernic_base + off);
}

static inline void ernic_wr(struct onic_private *priv,
			    struct onic_ernic_irq_ctx *ctx, u32 off, u32 val)
{
	iowrite32(val, priv->hw.addr + ctx->ernic_base + off);
}

static irqreturn_t onic_ernic_isr(int irq, void *data)
{
	struct onic_ernic_irq_ctx *ctx = data;
	struct onic_private       *priv = ctx->priv;
	u32 intsts;

	/* Probe / torn-down guard. */
	if (unlikely(!priv || !priv->hw.addr))
		return IRQ_NONE;

	intsts = ernic_rd(priv, ctx, ERNIC_OFFSET_INTSTS);
	if (!intsts) {
		atomic_inc(&ctx->spurious_count);
		return IRQ_NONE;
	}

	/* W1C-ack BEFORE scheduling work so edge re-triggers don't loop while
	 * the bottom half is slowly walking RQINTSTS banks.  RQ/CQ/CNP banks
	 * are W1C independently — we still see bits set when we read them. */
	ernic_wr(priv, ctx, ERNIC_OFFSET_INTSTS, intsts);

	atomic_or(intsts, &ctx->pending_intsts);

	atomic_inc(&ctx->total_count);
	if (intsts & ERNIC_INTSTS_PKTVALERR)  atomic_inc(&ctx->pktvalerr_count);
	if (intsts & ERNIC_INTSTS_MADRX)      atomic_inc(&ctx->madrx_count);
	if (intsts & ERNIC_INTSTS_RNRNACKGEN) atomic_inc(&ctx->rnrnackgen_count);
	if (intsts & ERNIC_INTSTS_WQECOMPL)   atomic_inc(&ctx->wqecompl_count);
	if (intsts & ERNIC_INTSTS_ILLOPCODE)  atomic_inc(&ctx->illopcode_count);
	if (intsts & ERNIC_INTSTS_RQPKT)      atomic_inc(&ctx->rqpkt_count);
	if (intsts & ERNIC_INTSTS_FATALERR)   atomic_inc(&ctx->fatal_count);
	if (intsts & ERNIC_INTSTS_CNPSCHD)    atomic_inc(&ctx->cnpschd_count);

	schedule_work(&ctx->event_work);
	return IRQ_HANDLED;
}

static void onic_ernic_event_worker(struct work_struct *w)
{
	struct onic_ernic_irq_ctx *ctx =
		container_of(w, struct onic_ernic_irq_ctx, event_work);
	struct onic_private       *priv = ctx->priv;
	u32 pending, rq, cq, cnp;
	int i;

	pending = atomic_xchg(&ctx->pending_intsts, 0);
	if (!pending)
		return;

	dev_info_ratelimited(&priv->pdev->dev,
		"onic-ernic%u: intsts=0x%03x (fatal=%d wqe_compl=%d rqpkt=%d cnpschd=%d)\n",
		 ctx->index, pending,
		 atomic_read(&ctx->fatal_count),
		 atomic_read(&ctx->wqecompl_count),
		 atomic_read(&ctx->rqpkt_count),
		 atomic_read(&ctx->cnpschd_count));

	/* Walk 64 banks for the sources the pending bits suggest.  We do NOT
	 * W1C the banks — that's the real completion handler's job (B8/B9),
	 * because clearing without processing would drop work items. */
	if (pending & (ERNIC_INTSTS_RQPKT | ERNIC_INTSTS_MADRX)) {
		for (i = 1; i <= 64; i++) {
			rq = ernic_rd(priv, ctx,
				      ERNIC_OFFSET_RQINTSTS_BASE + 4 * (i - 1));
			if (rq)
				dev_info_ratelimited(&priv->pdev->dev,
					"  ERNIC%u RQINTSTS%d = 0x%08x\n",
					ctx->index, i, rq);
		}
	}
	if (pending & ERNIC_INTSTS_WQECOMPL) {
		for (i = 1; i <= 64; i++) {
			cq = ernic_rd(priv, ctx,
				      ERNIC_OFFSET_CQINTSTS_BASE + 4 * (i - 1));
			if (cq)
				dev_info_ratelimited(&priv->pdev->dev,
					"  ERNIC%u CQINTSTS%d = 0x%08x\n",
					ctx->index, i, cq);
		}
	}
	if (pending & ERNIC_INTSTS_CNPSCHD) {
		for (i = 1; i <= 64; i++) {
			cnp = ernic_rd(priv, ctx,
				       ERNIC_OFFSET_CNPSCHDSTS_BASE + 4 * (i - 1));
			if (cnp)
				dev_info_ratelimited(&priv->pdev->dev,
					"  ERNIC%u CNPSCHDSTS%d = 0x%08x\n",
					ctx->index, i, cnp);
		}
	}

	if (pending & ERNIC_INTSTS_FATALERR)
		dev_err(&priv->pdev->dev,
			"ERNIC%u: FATALERR — check STATQPi / STATRQPIDBi (not yet dumped in F5)\n",
			ctx->index);
}

/* ------------------------------------------------------------------ debugfs */

#ifdef CONFIG_DEBUG_FS

#define DEF_COUNTER(fld) \
	debugfs_create_atomic_t(#fld, 0444, dir, &ctx->fld)

static void onic_ernic_debugfs_init_one(struct onic_private *priv, int idx)
{
	struct onic_ernic_irq_ctx *ctx = &priv->ernic_irq[idx];
	struct dentry             *dir;
	char name[8];

	if (!priv->dfs_root)
		return;

	snprintf(name, sizeof(name), "ernic%d", idx);
	dir = debugfs_create_dir(name, priv->dfs_root);
	if (IS_ERR_OR_NULL(dir))
		return;
	ctx->dfs_dir = dir;

	debugfs_create_x32("ernic_base", 0444, dir, &ctx->ernic_base);
	debugfs_create_u32("msix_vid",   0444, dir, (u32 *)&ctx->msix_vid);
	DEF_COUNTER(pktvalerr_count);
	DEF_COUNTER(madrx_count);
	DEF_COUNTER(rnrnackgen_count);
	DEF_COUNTER(wqecompl_count);
	DEF_COUNTER(illopcode_count);
	DEF_COUNTER(rqpkt_count);
	DEF_COUNTER(fatal_count);
	DEF_COUNTER(cnpschd_count);
	DEF_COUNTER(spurious_count);
	DEF_COUNTER(total_count);
}

static void onic_ernic_debugfs_init(struct onic_private *priv)
{
	priv->dfs_root = debugfs_create_dir(priv->netdev->name, NULL);
	if (IS_ERR_OR_NULL(priv->dfs_root)) {
		priv->dfs_root = NULL;
		return;
	}
	onic_ernic_debugfs_init_one(priv, 0);
	onic_ernic_debugfs_init_one(priv, 1);
}

static void onic_ernic_debugfs_exit(struct onic_private *priv)
{
	debugfs_remove_recursive(priv->dfs_root);
	priv->dfs_root             = NULL;
	priv->ernic_irq[0].dfs_dir = NULL;
	priv->ernic_irq[1].dfs_dir = NULL;
}

#else  /* !CONFIG_DEBUG_FS */
static inline void onic_ernic_debugfs_init(struct onic_private *priv) {}
static inline void onic_ernic_debugfs_exit(struct onic_private *priv) {}
#endif

/* ------------------------------------------------------------------ setup */

static const char *ernic_irq_name(int idx)
{
	return idx == 0 ? "onic-ernic0" : "onic-ernic1";
}

static int onic_ernic_irq_setup_one(struct onic_private *priv, int idx,
				    u32 base, int rel_vid)
{
	struct onic_ernic_irq_ctx *ctx = &priv->ernic_irq[idx];
	struct pci_dev            *pdev = priv->pdev;
	int rv;

	ctx->priv       = priv;
	ctx->index      = (u8)idx;
	ctx->ernic_base = base;
	ctx->msix_vid   = rel_vid;
	ctx->irq        = pci_irq_vector(pdev, priv->vec_base + rel_vid);
	atomic_set(&ctx->pending_intsts,   0);
	atomic_set(&ctx->pktvalerr_count,  0);
	atomic_set(&ctx->madrx_count,      0);
	atomic_set(&ctx->rnrnackgen_count, 0);
	atomic_set(&ctx->wqecompl_count,   0);
	atomic_set(&ctx->illopcode_count,  0);
	atomic_set(&ctx->rqpkt_count,      0);
	atomic_set(&ctx->fatal_count,      0);
	atomic_set(&ctx->cnpschd_count,    0);
	atomic_set(&ctx->spurious_count,   0);
	atomic_set(&ctx->total_count,      0);
	INIT_WORK(&ctx->event_work, onic_ernic_event_worker);

	/* Make sure INTEN is 0 before we arm the ISR so a stale latched bit
	 * from bitstream init doesn't fire on us.  Then clear any stale
	 * INTSTS via W1C of all-ones. */
	ernic_wr(priv, ctx, ERNIC_OFFSET_INTEN, 0);
	(void)ernic_rd(priv, ctx, ERNIC_OFFSET_INTEN);   /* posted-write flush */
	ernic_wr(priv, ctx, ERNIC_OFFSET_INTSTS, ~0u);   /* W1C any stale bits */

	rv = request_irq(ctx->irq, onic_ernic_isr, 0,
			 ernic_irq_name(idx), ctx);
	if (rv) {
		dev_err(&pdev->dev,
			"ERNIC%d request_irq failed (vec=%d irq=%d): %d\n",
			idx, priv->vec_base + rel_vid, ctx->irq, rv);
		return rv;
	}
	ctx->registered = true;

	/* Now enable ERNIC's bits. */
	ernic_wr(priv, ctx, ERNIC_OFFSET_INTEN, ERNIC_INTSTS_ALL_OWNED);

	dev_info(&pdev->dev,
		 "ERNIC%d IRQ setup: vec=%d irq=%d base=0x%08x inten=0x%03x\n",
		 idx, priv->vec_base + rel_vid, ctx->irq, base,
		 (u32)ERNIC_INTSTS_ALL_OWNED);
	return 0;
}

int onic_ernic_irq_setup(struct onic_private *priv)
{
	int rv;

	/* Master PF only.  Non-master / secondary / VFs get no-op setup. */
	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return 0;

	/* Relative vector IDs (within this PF's vector pool):
	 *   user  = num_q_vectors
	 *   error = num_q_vectors + 1
	 *   ernic0 = num_q_vectors + 2
	 *   ernic1 = num_q_vectors + 3
	 * Must stay in sync with the +2 non_q bump in onic_acquire_msix_vectors. */
	rv = onic_ernic_irq_setup_one(priv, 0, 0x00800000,
				      priv->num_q_vectors + 2);
	if (rv)
		return rv;

	rv = onic_ernic_irq_setup_one(priv, 1, 0x00A00000,
				      priv->num_q_vectors + 3);
	if (rv) {
		/* Unwind ERNIC0. */
		ernic_wr(priv, &priv->ernic_irq[0], ERNIC_OFFSET_INTEN, 0);
		free_irq(priv->ernic_irq[0].irq, &priv->ernic_irq[0]);
		cancel_work_sync(&priv->ernic_irq[0].event_work);
		priv->ernic_irq[0].registered = false;
		return rv;
	}

	onic_ernic_debugfs_init(priv);
	return 0;
}

void onic_ernic_irq_teardown(struct onic_private *priv)
{
	int i;

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return;

	onic_ernic_debugfs_exit(priv);

	for (i = 0; i < 2; i++) {
		struct onic_ernic_irq_ctx *ctx = &priv->ernic_irq[i];

		if (!ctx->registered)
			continue;

		/* Disable ERNIC side first, then free the vector, then drain
		 * any in-flight bottom half.  free_irq synchronises with the
		 * current ISR; cancel_work_sync drains pending worker. */
		ernic_wr(priv, ctx, ERNIC_OFFSET_INTEN, 0);
		(void)ernic_rd(priv, ctx, ERNIC_OFFSET_INTEN);
		free_irq(ctx->irq, ctx);
		cancel_work_sync(&ctx->event_work);
		ctx->registered = false;
	}
}
