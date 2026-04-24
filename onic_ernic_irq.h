/*
 * Copyright (c) 2026 Tenstorrent Inc.
 *
 * onic_ernic_irq.h — MSI-X dispatch skeleton for dual-ERNIC.
 *
 * Reserves two MSI-X vectors (one per ERNIC) on the master PF, wires an
 * ISR that reads INTSTS, W1C-acks it, accumulates bits into a pending
 * snapshot, and schedules a bottom-half work item that dumps
 * RQINTSTS/CQINTSTS/CNPSCHDSTS banks.  F5 is log-only — completion
 * processing lands in B8/B9.
 *
 * Per-source counters are exposed under
 * /sys/kernel/debug/onic/<netdev>/ernic{0,1}/.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef __ONIC_ERNIC_IRQ_H__
#define __ONIC_ERNIC_IRQ_H__

#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>

struct onic_private;

/**
 * struct onic_ernic_irq_ctx - per-ERNIC MSI-X dispatch context
 *
 * Two instances per master PF: one for ERNIC0 (base 0x800000) and one for
 * ERNIC1 (base 0xA00000).  The ISR reads INTSTS at ernic_base + 0x100184,
 * W1C-acks it, accumulates bits via atomic_or into pending_intsts, and
 * schedules event_work for register-bank dumping.
 */
struct onic_ernic_irq_ctx {
	struct onic_private *priv;       /* back-pointer */
	u8                   index;      /* 0 or 1 — which ERNIC */
	u32                  ernic_base; /* 0x00800000 (ERNIC0) or 0x00A00000 (ERNIC1) */
	int                  msix_vid;   /* relative vector ID within master's pool */
	int                  irq;        /* resolved cookie from pci_irq_vector */
	bool                 registered; /* true once request_irq succeeded */
	struct work_struct   event_work;

	/* Latched INTSTS value from the last ISR fire, consumed by event_work.
	 * A single u32 is sufficient because the ISR W1C-clears before scheduling
	 * work and atomic_or accumulates any racing bits. */
	atomic_t             pending_intsts;

	/* Per-source counters — exposed via debugfs. */
	atomic_t             pktvalerr_count;   /* INTSTS[0] */
	atomic_t             madrx_count;       /* INTSTS[1] */
	atomic_t             rnrnackgen_count;  /* INTSTS[3] */
	atomic_t             wqecompl_count;    /* INTSTS[4] */
	atomic_t             illopcode_count;   /* INTSTS[5] */
	atomic_t             rqpkt_count;       /* INTSTS[6] */
	atomic_t             fatal_count;       /* INTSTS[7] */
	atomic_t             cnpschd_count;     /* INTSTS[8] */
	atomic_t             spurious_count;    /* INTSTS==0 on entry */
	atomic_t             total_count;       /* any ISR fire */

	/* debugfs entries; NULL on !CONFIG_DEBUG_FS kernels. */
	struct dentry       *dfs_dir;
};

int  onic_ernic_irq_setup(struct onic_private *priv);
void onic_ernic_irq_teardown(struct onic_private *priv);

#endif /* __ONIC_ERNIC_IRQ_H__ */
