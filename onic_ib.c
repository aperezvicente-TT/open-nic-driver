/*
 * Copyright (c) 2026 Tenstorrent Inc.
 *
 * onic_ib.c — B3 ib_device skeleton: query_device/_port/_immutable,
 * empty ucontext, bitmap PD allocator, -EOPNOTSUPP stubs for the rest.
 * Real verbs (create_qp, post_send, poll_cq, etc.) land in B5/B6/B7/B8.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_cache.h>

#include "onic.h"
#include "onic_ib.h"
#include "onic_sysdma.h"
#include "onic_debugfs.h"
#include "../libreconic/reconic_reg.h"

/* Perf #1 — when true, allocate a coherent host page at ib_register
 * time and use it as the ERNIC RQWPTRDBADDi/CQDBADDi DMA target.
 * onic_poll_cq then reads the CQ producer index from the host page
 * instead of issuing an MMIO read of QCSR CQHEADi (saves one PCIe
 * round-trip per poll, ~1 µs on Gen3 x16). */
/* Default DISABLED 2026-05-07: cadence trace at onic_poll_cq:2086 showed
 * cqhead_page=1 vs cqhead_mmio=5 with all 5 CQEs actually produced (sqpi=
 * statcursqptr=5, inampkt=10).  ERNIC writes the host-coherent page once
 * on the first completion and never again — PG332 calls the sideband
 * notification a "completion count" (singular), not increment-per-WQE.
 * The kernel poll_cq reads page=1, returns 1 completion, then loops
 * forever returning 0 (~7000 spins per stalled WRITE batch).
 *
 * Until perf #1 is repaired (rearm CQDBADDi/CQDBADDMSBi after each poll,
 * or per-QP coherent page that survives modify_qp), default to MMIO
 * polling.  Set host_doorbell=1 explicitly to reproduce the bug. */
static bool host_doorbell = false;
module_param(host_doorbell, bool, 0444);
MODULE_PARM_DESC(host_doorbell,
	"Perf #1: use a host coherent page for ERNIC CQ/RQ doorbell DMA "
	"(default false; enable to reproduce the page-stuck-at-1 bug).");

/* ====================================================================
 * Dual-ERNIC helpers — verb-path register access.
 *
 * The bitstream carries TWO ERNIC instances:
 *   ERNIC0 at BAR2 offset RN_RDMA_BASE_ADDRESS    (0x800000) -> port 1
 *   ERNIC1 at BAR2 offset RN_RDMA_1_BASE_ADDRESS  (0xA00000) -> port 2
 *
 * libreconic/reconic_reg.h's RN_RDMA_QCSR_REG() macro hard-codes
 * RN_RDMA_BASE_ADDRESS, so it can only address ERNIC0.  For port-2 QPs
 * we compute the QCSR offset against qp->ernic_base, which is set at
 * RESET->INIT once port_num is known.
 *
 * libreconic uses one rdma_dev_t per port, with rdma_dev->axil_ctl
 * already offset by port_offset, and qpid is per-ERNIC inside that
 * device.  Our driver maintains a SINGLE qp_num namespace at the
 * ib_device level (required by IB — ibqp.qp_num must be unique inside
 * an ib_device), so we use the same qp_num as the ERNIC slot index in
 * each ERNIC's QCSR addressing.  qp_num collisions across ports are
 * impossible because the slot bitmap is shared (one onic_ddr_pool per
 * ib_device); two QPs with the same ibqp.qp_num cannot exist.
 *
 * DDR4 isolation: F4 §2.2 splits DDR4 into disjoint halves, ERNIC0 in
 * 0..2_0000_0000 and ERNIC1 in 0x2_0000_0000+.  We don't need separate
 * onic_ddr_pool instances for that — the slot byte offset is just
 * (port-1)*0x2_0000_0000 + slot_off.  Slot N on port 1 and slot M on
 * port 2 (with M==N never happening anyway) can never collide.
 * ==================================================================== */

/* Per-QP QCSR address in BAR2.  qp->ernic_base must have been set
 * (i.e. QP must be past RESET) before calling this. */
static inline u32 onic_qcsr(const struct onic_qp *qp, u32 off)
{
	return qp->ernic_base + 0x00180000u +
	       (qp->qp_num - 1u) * RN_RDMA_QCSR_STRIDE + off;
}

/* Dump every interesting QCSR + GCSR register for one QP, to dmesg.
 * Used to give visibility into ERNIC's view at every state transition
 * and around post_send.  Cheap: ~30 ioread32() per call.  `tag` shows
 * up in the log so we can match dumps to a transition.
 * Non-static so onic_debugfs.c can invoke it from the read handler. */
void onic_dump_qp_state(const struct onic_qp *qp, const char *tag)
{
	void __iomem *mmio = qp->ibqp.device->dev.parent ?
			     to_onic_ib_dev(qp->ibqp.device)->priv->hw.addr :
			     NULL;
	u32 q;
	u32 gcsr;

	if (!mmio || !qp->ernic_base)
		return;

	q = onic_qcsr(qp, 0x00);
	gcsr = qp->ernic_base + 0x00100000u;

	pr_info("onic_ib: [%s] qp=%u port=%u ernic=%u QCSR base=0x%08x\n",
		tag, qp->qp_num, qp->port_num,
		(qp->port_num == 1) ? 0u : 1u, q);
	pr_info("onic_ib:   QPCONF=%08x QPADVCONF=%08x QDEPTH=%08x PD=%08x\n",
		ioread32(mmio + q + 0x00), ioread32(mmio + q + 0x04),
		ioread32(mmio + q + 0x3C), ioread32(mmio + q + 0xB0));
	pr_info("onic_ib:   SQBA=%08x:%08x  RQBA=%08x:%08x  CQBA=%08x:%08x\n",
		ioread32(mmio + q + 0xC8), ioread32(mmio + q + 0x10),
		ioread32(mmio + q + 0xC0), ioread32(mmio + q + 0x08),
		ioread32(mmio + q + 0xD0), ioread32(mmio + q + 0x18));
	pr_info("onic_ib:   DESTQP=%08x TIMEOUT=%08x DMAC_LSB=%08x DMAC_MSB=%08x IPDST=%08x\n",
		ioread32(mmio + q + 0x48), ioread32(mmio + q + 0x4C),
		ioread32(mmio + q + 0x50), ioread32(mmio + q + 0x54),
		ioread32(mmio + q + 0x60));
	pr_info("onic_ib:   SQPSN=%08x SQPI=%08x RQCI=%08x CQHEAD=%08x\n",
		ioread32(mmio + q + 0x40), ioread32(mmio + q + 0x38),
		ioread32(mmio + q + 0x34), ioread32(mmio + q + 0x30));
	pr_info("onic_ib:   STATSSN=%08x STATMSN=%08x STATQP=%08x STATCURSQPTR=%08x\n",
		ioread32(mmio + q + 0x80), ioread32(mmio + q + 0x84),
		ioread32(mmio + q + 0x88), ioread32(mmio + q + 0x8C));
	pr_info("onic_ib:   STATRESPSN=%08x STATRQBUFCA=%08x STATWQE=%08x STATRQPIDB=%08x\n",
		ioread32(mmio + q + 0x90), ioread32(mmio + q + 0x94),
		ioread32(mmio + q + 0x98), ioread32(mmio + q + 0x9C));
	pr_info("onic_ib:   GCSR XRNICCONF=%08x XRNIC_CONF_QP_EN=%08x INSRRPKT=%08x INALLDRP=%08x\n",
		ioread32(mmio + gcsr + 0x00), ioread32(mmio + gcsr + 0x44),
		ioread32(mmio + gcsr + 0x100), ioread32(mmio + gcsr + 0x130));
	pr_info("onic_ib:   GCSR ERRBUFWPTR=%08x IPKTERRQWPTR=%08x WQEPROC=%08x QPMSTS=%08x\n",
		ioread32(mmio + gcsr + 0x6C), ioread32(mmio + gcsr + 0x94),
		ioread32(mmio + gcsr + 0x124), ioread32(mmio + gcsr + 0x12C));
	/* Hardware-probe-equivalent: outgoing-packet counters and last-packet
	 * header bytes.  Tell us whether ERNIC actually emitted a packet on
	 * its TX AXI-Stream port (no ILA needed).  If OUTIO/OUTAM stay 0 after
	 * post_send → engine never emitted; if they tick → engine emitted but
	 * something downstream dropped it.  LSTOUTPKT carries the actual RoCE
	 * BTH bytes (PSN, opcode, dst QPN) of the most recent outgoing packet. */
	pr_info("onic_ib:   GCSR OUTIOPKT=%08x OUTAMPKT=%08x LSTOUTPKT=%08x OUTRNR=%08x\n",
		ioread32(mmio + gcsr + 0x108), ioread32(mmio + gcsr + 0x10C),
		ioread32(mmio + gcsr + 0x114), ioread32(mmio + gcsr + 0x120));
	pr_info("onic_ib:   GCSR INAMPKT=%08x LSTINPKT=%08x INNCK=%08x INNAK=%08x INVDUP=%08x\n",
		ioread32(mmio + gcsr + 0x104), ioread32(mmio + gcsr + 0x110),
		ioread32(mmio + gcsr + 0x11C), ioread32(mmio + gcsr + 0x134),
		ioread32(mmio + gcsr + 0x118));
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/* seq_file mirror of onic_dump_qp_state — same register set, but rendered
 * into the seq_file buffer instead of pr_info().  Used by the debugfs
 * qp<N>/dump entry so `cat` returns the dump as text and `watch -n 0.2 cat
 * ...` works as a live monitor.  Kept in sync with onic_dump_qp_state. */
void onic_format_qp_state(struct seq_file *m, const struct onic_qp *qp)
{
	void __iomem *mmio;
	u32 q, gcsr;

	if (!m || !qp)
		return;

	seq_printf(m, "qp=%u port=%u state=%d ernic_base=0x%08x\n",
		qp->qp_num, qp->port_num, (int)qp->state, qp->ernic_base);

	if (!qp->ibqp.device) {
		seq_puts(m, "  (no ibqp.device — cannot read MMIO)\n");
		return;
	}
	mmio = to_onic_ib_dev(qp->ibqp.device)->priv->hw.addr;
	if (!mmio) {
		seq_puts(m, "  (priv->hw.addr is NULL — cannot read MMIO)\n");
		return;
	}
	if (!qp->ernic_base) {
		seq_puts(m, "  (ernic_base=0 — QP still in RESET, no QCSR window assigned)\n");
		return;
	}

	q    = onic_qcsr(qp, 0x00);
	gcsr = qp->ernic_base + 0x00100000u;

	seq_printf(m, "QCSR base=0x%08x ernic=%u\n",
		q, (qp->port_num == 1) ? 0u : 1u);
	seq_printf(m, "  QPCONF=%08x QPADVCONF=%08x QDEPTH=%08x PD=%08x\n",
		ioread32(mmio + q + 0x00), ioread32(mmio + q + 0x04),
		ioread32(mmio + q + 0x3C), ioread32(mmio + q + 0xB0));
	seq_printf(m, "  SQBA=%08x:%08x  RQBA=%08x:%08x  CQBA=%08x:%08x\n",
		ioread32(mmio + q + 0xC8), ioread32(mmio + q + 0x10),
		ioread32(mmio + q + 0xC0), ioread32(mmio + q + 0x08),
		ioread32(mmio + q + 0xD0), ioread32(mmio + q + 0x18));
	seq_printf(m, "  DESTQP=%08x TIMEOUT=%08x DMAC_LSB=%08x DMAC_MSB=%08x IPDST=%08x\n",
		ioread32(mmio + q + 0x48), ioread32(mmio + q + 0x4C),
		ioread32(mmio + q + 0x50), ioread32(mmio + q + 0x54),
		ioread32(mmio + q + 0x60));
	seq_printf(m, "  SQPSN=%08x SQPI=%08x RQCI=%08x CQHEAD=%08x\n",
		ioread32(mmio + q + 0x40), ioread32(mmio + q + 0x38),
		ioread32(mmio + q + 0x34), ioread32(mmio + q + 0x30));
	seq_printf(m, "  STATSSN=%08x STATMSN=%08x STATQP=%08x STATCURSQPTR=%08x\n",
		ioread32(mmio + q + 0x80), ioread32(mmio + q + 0x84),
		ioread32(mmio + q + 0x88), ioread32(mmio + q + 0x8C));
	seq_printf(m, "  STATRESPSN=%08x STATRQBUFCA=%08x STATWQE=%08x STATRQPIDB=%08x\n",
		ioread32(mmio + q + 0x90), ioread32(mmio + q + 0x94),
		ioread32(mmio + q + 0x98), ioread32(mmio + q + 0x9C));
	seq_printf(m, "  GCSR XRNICCONF=%08x XRNIC_CONF_QP_EN=%08x INSRRPKT=%08x INALLDRP=%08x\n",
		ioread32(mmio + gcsr + 0x00), ioread32(mmio + gcsr + 0x44),
		ioread32(mmio + gcsr + 0x100), ioread32(mmio + gcsr + 0x130));
	seq_printf(m, "  GCSR ERRBUFWPTR=%08x IPKTERRQWPTR=%08x WQEPROC=%08x QPMSTS=%08x\n",
		ioread32(mmio + gcsr + 0x6C), ioread32(mmio + gcsr + 0x94),
		ioread32(mmio + gcsr + 0x124), ioread32(mmio + gcsr + 0x12C));
	seq_printf(m, "  GCSR OUTIOPKT=%08x OUTAMPKT=%08x LSTOUTPKT=%08x OUTRNR=%08x\n",
		ioread32(mmio + gcsr + 0x108), ioread32(mmio + gcsr + 0x10C),
		ioread32(mmio + gcsr + 0x114), ioread32(mmio + gcsr + 0x120));
	seq_printf(m, "  GCSR INAMPKT=%08x LSTINPKT=%08x INNCK=%08x INNAK=%08x INVDUP=%08x\n",
		ioread32(mmio + gcsr + 0x104), ioread32(mmio + gcsr + 0x110),
		ioread32(mmio + gcsr + 0x11C), ioread32(mmio + gcsr + 0x134),
		ioread32(mmio + gcsr + 0x118));
}

/* Per-ibdev one-screen counter summary across BOTH ERNICs.  Used as the
 * top-level debugfs gcsr_summary entry — the file `watch -n 0.2 cat`
 * tracks during WRITE/SEND debug.  No per-QP info; for that, cat the
 * sibling qp<N>/dump entries.  Order chosen so the most diagnostically
 * useful counters (INVDUP, INALLDRP, INSRRPKT, OUTIOPKT, LSTINPKT,
 * LSTOUTPKT) appear first. */
void onic_format_gcsr_summary(struct seq_file *m, struct onic_ib_dev *dev)
{
	static const u32 ernic_bases[2] = {
		RN_RDMA_BASE_ADDRESS,    /* ERNIC0 -> port 1 */
		RN_RDMA_1_BASE_ADDRESS,  /* ERNIC1 -> port 2 */
	};
	void __iomem *mmio;
	int i, n_ernics;

	if (!m || !dev || !dev->priv)
		return;
	mmio     = dev->priv->hw.addr;
	n_ernics = (dev->priv->hw.num_cmacs >= 2) ? 2 : 1;
	if (!mmio)
		return;

	for (i = 0; i < n_ernics; i++) {
		u32 g = ernic_bases[i] + 0x00100000u;

		seq_printf(m, "ERNIC%d (port %d, base=0x%08x, GCSR=0x%08x)\n",
			i, i + 1, ernic_bases[i], g);
		seq_printf(m, "  INVDUP   =%08x  INALLDRP=%08x  INSRRPKT=%08x  INNAK   =%08x\n",
			ioread32(mmio + g + 0x118),
			ioread32(mmio + g + 0x130),
			ioread32(mmio + g + 0x100),
			ioread32(mmio + g + 0x134));
		seq_printf(m, "  INAMPKT  =%08x  INNCK   =%08x  LSTINPKT =%08x\n",
			ioread32(mmio + g + 0x104),
			ioread32(mmio + g + 0x11C),
			ioread32(mmio + g + 0x110));
		seq_printf(m, "  OUTIOPKT =%08x  OUTAMPKT=%08x  LSTOUTPKT=%08x  OUTRNR  =%08x\n",
			ioread32(mmio + g + 0x108),
			ioread32(mmio + g + 0x10C),
			ioread32(mmio + g + 0x114),
			ioread32(mmio + g + 0x120));
		seq_printf(m, "  XRNICCONF=%08x  QP_EN_CT=%08x  WQEPROC =%08x  QPMSTS  =%08x\n",
			ioread32(mmio + g + 0x000),
			ioread32(mmio + g + 0x044),
			ioread32(mmio + g + 0x124),
			ioread32(mmio + g + 0x12C));
		seq_printf(m, "  ERRBUFWP =%08x  IPKTERRQWP=%08x\n",
			ioread32(mmio + g + 0x06C),
			ioread32(mmio + g + 0x094));
	}
}

#endif /* CONFIG_DEBUG_FS */

/* Per-port DDR4 byte offset.  port_num is 1-based as in IB.
 *
 * The F4 §2.2 spec splits DDR4 into 8 GiB halves (port 1 at 0, port 2 at
 * 0x2_0000_0000) for ERNIC isolation.  Empirically (2026-04-29) the
 * production shell's QDMA M_AXI MM cannot reach 0x2_0000_0000 — it
 * raises GLBL_DSC_ERR_STS_DMA on the descriptor.  Until the shell-side
 * mapping is verified (or the controller capacity is widened), collapse
 * both ports onto port 1's region.  QP isolation is preserved by the
 * per-QP slot bitmap which assigns unique 16 KiB slots regardless of
 * port_num. */
static inline u64 onic_port_ddr_off(u8 port_num)
{
	(void)port_num;
	return 0ULL;
}

/* Map IB port_num -> ERNIC BAR2 base. */
static inline u32 onic_ernic_base_for_port(u8 port_num)
{
	return (port_num == 2) ? RN_RDMA_1_BASE_ADDRESS
			       : RN_RDMA_BASE_ADDRESS;
}


/* GCSR offsets relative to an ERNIC base (libreconic uses these via the
 * concatenation in reconic_reg.h, which hardcodes RN_RDMA_BASE_ADDRESS).
 * Per-ERNIC init writes go to ernic_base + GCSR_OFF + reg_off. */
#define ONIC_GCSR_OFF              0x00100000u
#define ONIC_GCSR_XRNICCONF        0x00000000u
#define ONIC_GCSR_XRNICADCONF      0x00000004u
#define ONIC_GCSR_MACXADDLSB       0x00000010u
#define ONIC_GCSR_MACXADDMSB       0x00000014u
/* INTEN lives at GCSR 0x180 per PG332 §Table 8 — but onic_ernic_irq.c
 * already programs it correctly (with the full 0x1FB mask including
 * CNP+MAD RX).  Letting onic_ernic_global_init also write here would
 * clobber that to 0xFF every modify_qp(RESET->INIT), losing bits 1+8.
 * So we deliberately do not duplicate that write — a no-op write to a
 * scratch offset suffices to keep the call site simple. */
#define ONIC_GCSR_INTEN            0x00000040u  /* reserved/scratch — see comment above */
#define ONIC_GCSR_XRNIC_CONF_QP_EN 0x00000044u
#define ONIC_GCSR_ERRBUFBA         0x00000060u  /* REQERRBUFBA per PG332 */
#define ONIC_GCSR_ERRBUFBAMSB      0x00000064u
#define ONIC_GCSR_ERRBUFSZ         0x00000068u
#define ONIC_GCSR_IPV4XADD         0x00000070u
#define ONIC_GCSR_IPKTERRQBA       0x00000088u  /* FATALERRBUFBA per PG332 */
#define ONIC_GCSR_IPKTERRQBAMSB    0x0000008Cu
#define ONIC_GCSR_IPKTERRQSZ       0x00000090u
#define ONIC_GCSR_DATBUFBA         0x000000A0u
#define ONIC_GCSR_DATBUFBAMSB      0x000000A4u
#define ONIC_GCSR_DATBUFSZ         0x000000A8u
#define ONIC_GCSR_RESPERRPKTBA     0x000000B0u
#define ONIC_GCSR_RESPERRPKTBAMSB  0x000000B4u
#define ONIC_GCSR_RESPERRSZ        0x000000B8u
#define ONIC_GCSR_RESPERRSZMSB     0x000000BCu

/* Per-ERNIC reservation of DDR4 for the four global error/data buffers.
 * libreconic programs all four; ERNIC v4.2 stalls silently if any are
 * left at zero (symptom: SQPI doorbell ignored, CQE ring stays 0xFF).
 * We carve a 4 MiB block per ERNIC at fixed high offsets that cannot
 * collide with the QP slot allocator (which starts at byte 0).
 *
 * ERNIC0 block: 0x01000000 (16 MiB).  ERNIC1 block: 0x01400000 (20 MiB).
 * Each block is split:
 *   +0x000000 .. +0x040000 (256 KiB) — DATBUF
 *   +0x100000 .. +0x140000 (256 KiB) — IPKTERRQ (incoming-packet error)
 *   +0x200000 .. +0x240000 (256 KiB) — ERRBUF   (request error)
 *   +0x300000 .. +0x340000 (256 KiB) — RESPERRPKT
 */
#define ONIC_ERNIC_GBUF_BASE(ernic)   (0x01000000u + (ernic) * 0x00400000u)
#define ONIC_ERNIC_DATBUF_OFF         0x00000000u
#define ONIC_ERNIC_IPKTERRQ_OFF       0x00100000u
#define ONIC_ERNIC_ERRBUF_OFF         0x00200000u
#define ONIC_ERNIC_RESPERRPKT_OFF     0x00300000u
#define ONIC_ERNIC_GBUF_SLOT_SZ       0x00040000u  /* 256 KiB each */

/* Bring an ERNIC instance out of reset and program local-side identity:
 *   src MAC, src IPv4, ENICEN=1, default advanced-config, all IRQs enabled.
 *
 * Without this, the engine ignores QPCONFi and SQ doorbells — observed end
 * of 2026-04-29: XRNICCONF=0 caused STATCURSQPTRi to never advance even
 * though SQPIi=1 had landed.  Writes follow libreconic's
 * config_rdma_global_csr() ordering (MAC -> IP -> XRNICCONF -> ADCONF).
 *
 * IPv4 is read from the netdev at call time; if the user hasn't yet
 * configured an address the field is left at 0 and re-programmed when a
 * QP first transitions RESET->INIT (by which point an address is required
 * for any meaningful traffic). */
static void onic_ernic_global_init(struct onic_ib_dev *dev, u8 port_num)
{
	void __iomem    *mmio  = dev->priv->hw.addr;
	struct net_device *ndev;
	u32              base  = onic_ernic_base_for_port(port_num) + ONIC_GCSR_OFF;
	u32              mac_lsb = 0, mac_msb = 0;
	__be32           src_ip  = 0;
	u32              xrnic_conf, xrnic_advanced_conf;
	const u32        udp_sport         = 0x12B7;   /* 4791 */
	const u32        num_qp            = 8;        /* matches sim default */
	const u32        en_ernic          = 1;
	const u32        err_buf_en        = 1;
	/* XRNICCONF[4:3]: TX-ACK generation policy.  PG332 v4.2 line 1511-1516.
	 *   00 = ACK on explicit-request OR coalesce-timeout (default).
	 *   10 = ACK only on explicit request — disables coalesce-timeout path.
	 * Multi-packet WRITEs ≥3 frames send unacked MIDDLE frames; if the
	 * coalesce-timeout fires before LAST arrives, the responder may NAK
	 * mid-burst and confuse the requester's PSN window (issue 1).  Set to
	 * 10 to kill the timeout path and isolate. */
	const u32        tx_ack_gen        = 2;
	const u32        sw_override_en    = 0;
	const u32        retry_cnt_fatal_d = 1;
	const u32        base_count_width  = 10;       /* 250 MHz axil_aclk */
	const u32        sw_override_qp    = 0;
	u32              config_8bit, config_16bit;

	rcu_read_lock();
	ndev = rcu_dereference(dev->port[port_num - 1].netdev);
	if (ndev) {
		const u8 *m = ndev->dev_addr;
		struct in_device *in_dev;

		mac_msb = ((u32)m[0] << 8) | (u32)m[1];
		mac_lsb = ((u32)m[2] << 24) | ((u32)m[3] << 16) |
			  ((u32)m[4] << 8)  | (u32)m[5];

		in_dev = __in_dev_get_rcu(ndev);
		if (in_dev) {
			struct in_ifaddr *ifa;

			in_dev_for_each_ifa_rcu(ifa, in_dev) {
				src_ip = ifa->ifa_local;
				break;
			}
		}
	}
	rcu_read_unlock();

	config_8bit = ((err_buf_en & 1u) << 5) |
		      ((tx_ack_gen & 3u) << 3) |
		      (en_ernic & 1u);
	/* XRNICCONF layout per libreconic (the working reference) — udp_sport
	 * lives in bits [23:8], NOT [31:16] as previously coded.  num_qp is
	 * NOT placed in XRNICCONF; it goes to XRNIC_CONF_QP_EN (0x44) elsewhere.
	 * Prior layout caused outgoing RoCEv2 frames to carry the wrong UDP
	 * source port, which receivers silently dropped (INALLDRPPKTCNT++,
	 * no NAK). 2026-05-03 fix.
	 */
	xrnic_conf = ((udp_sport << 8) & 0x00ffff00u) |
		     (config_8bit & 0x000000ffu);
	(void)num_qp;  /* programmed via XRNIC_CONF_QP_EN per-QP */

	config_16bit = (sw_override_en & 1u) |
		       ((retry_cnt_fatal_d & 1u) << 2);
	xrnic_advanced_conf = (config_16bit & 0xffffu) |
			      ((base_count_width & 0xfu) << 16) |
			      ((sw_override_qp   & 0xffu) << 24);

	/* Program the four error/data buffers ERNIC needs even if we never
	 * consume them.  Addresses are AXI form (with 0xA3500000 tag in
	 * MSB) so the engine's own master can reach DDR4 through
	 * sys_mem_5to2 M01.
	 *
	 * Size field encoding (Gap-A fix 2026-05-03, per PG332 §Table 8 and
	 * RecoNIC/examples/rdma_test/write.c:46-52):
	 *     [31:16] = per_entry_size in bytes
	 *     [15:0]  = number of entries
	 * Total size = num_entries * per_entry_size.  The previous values
	 * `(1<<16)|0x1000 = 0x00011000` (4096 entries × 1 B = 4 KiB) and
	 * `0x00040000` (0 entries × 4 B — degenerate) were both wrong;
	 * RESPERRSZ=0 in particular meant any retry-error logging by ERNIC
	 * could fall back to host bus address 0 and trip the IOMMU.
	 *
	 * 64 entries × 4 KiB each = 256 KiB matches our `ONIC_ERNIC_GBUF_SLOT_SZ`
	 * allocation per buffer.  Layout: BUF_SIZE_FIELD = (4096 << 16) | 64. */
	{
		u32       ernic_idx = (port_num == 1) ? 0u : 1u;
		u64       gbuf_base = (u64)ONIC_ERNIC_GBUF_BASE(ernic_idx);
		const u32 buf_size_field = (4096u << 16) | 64u;  /* 64 × 4 KiB = 256 KiB */
		u64       datbuf  = ((u64)ONIC_DDR4_MSB << 32) |
				    (gbuf_base + ONIC_ERNIC_DATBUF_OFF);
		u64       errbuf  = ((u64)ONIC_DDR4_MSB << 32) |
				    (gbuf_base + ONIC_ERNIC_ERRBUF_OFF);
		u64       ipkterr = ((u64)ONIC_DDR4_MSB << 32) |
				    (gbuf_base + ONIC_ERNIC_IPKTERRQ_OFF);
		u64       resperr = ((u64)ONIC_DDR4_MSB << 32) |
				    (gbuf_base + ONIC_ERNIC_RESPERRPKT_OFF);

		iowrite32((u32)(datbuf  & 0xffffffffu), mmio + base + ONIC_GCSR_DATBUFBA);
		iowrite32((u32)(datbuf  >> 32),         mmio + base + ONIC_GCSR_DATBUFBAMSB);
		iowrite32(buf_size_field,               mmio + base + ONIC_GCSR_DATBUFSZ);

		iowrite32((u32)(errbuf  & 0xffffffffu), mmio + base + ONIC_GCSR_ERRBUFBA);
		iowrite32((u32)(errbuf  >> 32),         mmio + base + ONIC_GCSR_ERRBUFBAMSB);
		iowrite32(buf_size_field,               mmio + base + ONIC_GCSR_ERRBUFSZ);

		iowrite32((u32)(ipkterr & 0xffffffffu), mmio + base + ONIC_GCSR_IPKTERRQBA);
		iowrite32((u32)(ipkterr >> 32),         mmio + base + ONIC_GCSR_IPKTERRQBAMSB);
		iowrite32(buf_size_field,               mmio + base + ONIC_GCSR_IPKTERRQSZ);

		iowrite32((u32)(resperr & 0xffffffffu), mmio + base + ONIC_GCSR_RESPERRPKTBA);
		iowrite32((u32)(resperr >> 32),         mmio + base + ONIC_GCSR_RESPERRPKTBAMSB);
		iowrite32(buf_size_field,               mmio + base + ONIC_GCSR_RESPERRSZ);
		iowrite32(0,                            mmio + base + ONIC_GCSR_RESPERRSZMSB);
	}

	/* Order matches libreconic: identity first, then engine enable. */
	iowrite32(0x000000FFu,         mmio + base + ONIC_GCSR_INTEN);
	iowrite32(mac_lsb,             mmio + base + ONIC_GCSR_MACXADDLSB);
	iowrite32(mac_msb,             mmio + base + ONIC_GCSR_MACXADDMSB);
	iowrite32((u32)be32_to_cpu(src_ip), mmio + base + ONIC_GCSR_IPV4XADD);
	iowrite32(xrnic_conf,          mmio + base + ONIC_GCSR_XRNICCONF);
	iowrite32(xrnic_advanced_conf, mmio + base + ONIC_GCSR_XRNICADCONF);
	(void)ioread32(mmio + base + ONIC_GCSR_XRNICCONF);  /* flush */

	pr_info("onic_ib: ERNIC%u global init: base=0x%08x mac=%04x:%08x ip=%pI4 xrnic_conf=0x%08x adconf=0x%08x\n",
		(port_num == 1) ? 0u : 1u,
		onic_ernic_base_for_port(port_num),
		mac_msb, mac_lsb, &src_ip, xrnic_conf, xrnic_advanced_conf);
}


/* ----- query_* ----------------------------------------------------------- */

static int onic_query_device(struct ib_device *ibdev,
			     struct ib_device_attr *attr,
			     struct ib_udata *udata)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibdev);

	if (udata->inlen || udata->outlen)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));
	attr->fw_ver              = 0x0402;
	attr->hw_ver              = 0x0402;
	attr->vendor_id           = 0x10ee;
	attr->vendor_part_id      = dev->priv->pdev->device;
	attr->sys_image_guid      = dev->node_guid;
	attr->max_qp              = 255;
	/* onic_create_qp rejects max_send_wr/max_recv_wr > 64 (line 982).
	 * Report 64 here so test harnesses that query before creating (e.g.
	 * perftest) auto-clamp instead of hitting EINVAL. Real fix would
	 * deepen the per-QP shadow rings; cap at 64 for now matches HW. */
	attr->max_qp_wr           = 64;
	attr->device_cap_flags    = 0;
	attr->kernel_cap_flags    = 0;
	attr->max_send_sge        = 1;
	attr->max_recv_sge        = 1;
	attr->max_sge_rd          = 1;
	attr->max_cq              = 255;
	attr->max_cqe             = 1024;
	attr->max_mr              = 255;
	attr->max_pd              = ONIC_IB_MAX_PD;
	attr->atomic_cap          = IB_ATOMIC_NONE;
	attr->masked_atomic_cap   = IB_ATOMIC_NONE;
	attr->max_mcast_grp       = 0;
	attr->max_ah              = 0;
	attr->max_srq             = 0;
	attr->max_pkeys           = 1;
	attr->local_ca_ack_delay  = 14;
	attr->max_mr_size         = ~0ULL;
	/* ERNIC uses physical addresses — any page size >= 4 KiB works.
	 * Bitmap: bit N set means page size 2^N supported.  Matches mlx5's
	 * "everything from 4K up" convention so hugepage-backed buffers
	 * (2 MiB, 1 GiB) don't require special handling on the driver side. */
	attr->page_size_cap       = ~0xfffULL;
	return 0;
}

static int onic_query_port(struct ib_device *ibdev, u32 port,
			   struct ib_port_attr *attr)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibdev);
	struct net_device  *ndev;

	if (port < 1 || port > 2)
		return -EINVAL;

	rcu_read_lock();
	ndev = rcu_dereference(dev->port[port - 1].netdev);
	rcu_read_unlock();

	memset(attr, 0, sizeof(*attr));

	if (ndev && netif_running(ndev) && netif_carrier_ok(ndev)) {
		attr->state      = IB_PORT_ACTIVE;
		attr->phys_state = 5; /* LINK_UP */
	} else {
		attr->state      = IB_PORT_DOWN;
		attr->phys_state = 3; /* DISABLED */
	}
	attr->max_mtu        = IB_MTU_4096;
	attr->active_mtu     = IB_MTU_4096;
	attr->phys_mtu       = 4096;
	attr->gid_tbl_len    = 4;
	attr->ip_gids        = 1;
	attr->port_cap_flags = 0;
	attr->max_msg_sz     = 1u << 30;
	attr->pkey_tbl_len   = 1;
	attr->active_width   = IB_WIDTH_4X;
	attr->active_speed   = IB_SPEED_EDR;
	return 0;
}

static int onic_get_port_immutable(struct ib_device *ibdev, u32 port,
				   struct ib_port_immutable *imm)
{
	struct ib_port_attr a;
	int rv = ib_query_port(ibdev, port, &a);
	if (rv) return rv;
	imm->pkey_tbl_len   = a.pkey_tbl_len;
	imm->gid_tbl_len    = a.gid_tbl_len;
	imm->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	imm->max_mad_size   = 0;
	return 0;
}

static enum rdma_link_layer
onic_get_link_layer(struct ib_device *ibdev, u32 port)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int onic_query_pkey(struct ib_device *ibdev, u32 port, u16 idx, u16 *pkey)
{
	if (idx > 0) return -EINVAL;
	*pkey = 0xFFFF;
	return 0;
}

static int onic_query_gid(struct ib_device *ibdev, u32 port, int idx,
			  union ib_gid *gid)
{
	return -EINVAL; /* RoCE: GID cache owns these */
}

static struct net_device *onic_get_netdev(struct ib_device *ibdev, u32 port)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibdev);
	struct net_device  *ndev = NULL;

	if (port < 1 || port > 2)
		return NULL;
	rcu_read_lock();
	ndev = rcu_dereference(dev->port[port - 1].netdev);
	if (ndev)
		dev_hold(ndev);
	rcu_read_unlock();
	return ndev;
}

static void onic_get_dev_fw_str(struct ib_device *ibdev, char *str)
{
	snprintf(str, IB_FW_VERSION_NAME_MAX, "ERNIC v4.2 (F5+B3 2026-04-23)");
}

/* ----- ucontext ---------------------------------------------------------- */

static int onic_alloc_ucontext(struct ib_ucontext *uc, struct ib_udata *udata)
{
	return 0;
}
static void onic_dealloc_ucontext(struct ib_ucontext *uc) { }

/* ----- PD allocator ------------------------------------------------------ */

static int onic_alloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibpd->device);
	struct onic_pd     *pd  = to_onic_pd(ibpd);
	int                 bit;

	spin_lock(&dev->pd_lock);
	bit = find_first_zero_bit(dev->pd_bitmap, ONIC_IB_MAX_PD);
	if (bit >= ONIC_IB_MAX_PD) {
		spin_unlock(&dev->pd_lock);
		return -ENOMEM;
	}
	set_bit(bit, dev->pd_bitmap);
	spin_unlock(&dev->pd_lock);

	pd->pdn = bit;
	pd->mr  = NULL;
	spin_lock_init(&pd->mr_lock);
	return 0;
}

static int onic_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibpd->device);
	struct onic_pd     *pd  = to_onic_pd(ibpd);

	/* ib_core normally tears MRs down before their PD; refuse defensively. */
	spin_lock(&pd->mr_lock);
	if (pd->mr) {
		spin_unlock(&pd->mr_lock);
		return -EBUSY;
	}
	spin_unlock(&pd->mr_lock);

	spin_lock(&dev->pd_lock);
	clear_bit(pd->pdn, dev->pd_bitmap);
	spin_unlock(&dev->pd_lock);
	return 0;
}

/* ----- stubs — land real implementations in B5/B6/B7/B8 ------------------ */

#define STUB_BODY(name) \
	pr_info_ratelimited("onic_ib: b3_stub: " name " -> -EOPNOTSUPP\n"); \
	return -EOPNOTSUPP

/* ---- B5 real verbs: MR / CQ / QP create + destroy ---------------------- */

#ifdef OFED_HAVE_IB_DMAH
static struct ib_mr *
onic_reg_user_mr(struct ib_pd *ibpd, u64 start, u64 length, u64 virt_addr,
		 int access_flags, struct ib_dmah *dmah, struct ib_udata *udata)
#else
static struct ib_mr *
onic_reg_user_mr(struct ib_pd *ibpd, u64 start, u64 length, u64 virt_addr,
		 int access_flags, struct ib_udata *udata)
#endif
{
#ifdef OFED_HAVE_IB_DMAH
	(void)dmah;
#endif
	struct onic_ib_dev *dev = to_onic_ib_dev(ibpd->device);
	struct onic_pd     *pd  = to_onic_pd(ibpd);
	struct onic_mr     *mr;
	void __iomem       *mmio = dev->priv->hw.addr;
	u64   ddr_off, ddr_len;
	u64   buf_addr;
	u32   access_desc;
	u32   row_base;
	int   ret;

	(void)udata;

	pr_info_ratelimited("onic_ib: reg_user_mr called len=%llu va=0x%llx acc=0x%x\n",
			    length, virt_addr, access_flags);

	if (length == 0 || length > ONIC_DDR_MR_SMALL_SIZE) {
		pr_info_ratelimited("onic_ib: reg_user_mr REJECT length=%llu (cap=%u)\n",
				    length, ONIC_DDR_MR_SMALL_SIZE);
		return ERR_PTR(-EINVAL);
	}

	/* F7: one MR per PD. */
	spin_lock(&pd->mr_lock);
	if (pd->mr) {
		spin_unlock(&pd->mr_lock);
		return ERR_PTR(-EBUSY);
	}
	spin_unlock(&pd->mr_lock);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	ret = onic_ddr_mr_alloc(&dev->ddr, length, &ddr_off, &ddr_len);
	if (ret) {
		kfree(mr);
		return ERR_PTR(ret);
	}

	/* ERNIC ACCESSDESC[1:0]: 0=R, 1=W, 2=R/W. */
	access_desc = 0;
	if (access_flags & IB_ACCESS_REMOTE_READ)  access_desc |= 0x1;
	if (access_flags & IB_ACCESS_REMOTE_WRITE) access_desc |= 0x2;

	mr->pd      = pd;
	mr->va      = virt_addr;
	mr->ddr_off = ddr_off;
	mr->length  = length;
	mr->access  = access_desc & 0x3;
	/* ERNIC IP uses wire RETH.RKey upper byte as MR_INDEX into the PDT
	 * (PG332 v4.2 Figure 9 — confirmed empirically 2026-05-06 via
	 * tools/ernic-baremetal: wire RKey = (idx<<8)|key_byte makes ERNIC
	 * look up PDT[idx]; raw key_byte makes it look up PDT[0] which is
	 * unused, triggering FATAL_CODE 0x17 on every WRITE).  Encode pdn
	 * in bits [15:8] AND in [7:0] so PDT[pdn] is selected and BUFRKEY
	 * (low 8 bits) matches.  See project_b7_baremetal_isolation_2026_05_06. */
	mr->lkey    = (pd->pdn << 8) | (pd->pdn & 0xff);
	mr->rkey    = (pd->pdn << 8) | (pd->pdn & 0xff);
	pr_info("onic_ib: mr->rkey set to 0x%08x (pd->pdn=%u)\n",
		mr->rkey, pd->pdn);

	/* Perf #3 — populate the DDR4 mirror with the user buffer's contents.
	 * Eager copy: snapshot at registration time, no ongoing pinning.
	 * 1 MiB chunks because onic_ddr4_write caps at ONIC_SYSDMA_MAX_XFER.
	 * Failure here unwinds the slot allocation. */
	if (length > 0) {
		const size_t chunk = ONIC_SYSDMA_MAX_XFER;   /* 1 MiB */
		void *bounce = kmalloc(chunk, GFP_KERNEL);
		u64   off    = 0;
		int   crv    = 0;

		if (!bounce) {
			onic_ddr_mr_free(&dev->ddr, ddr_off);
			kfree(mr);
			return ERR_PTR(-ENOMEM);
		}
		while (off < length) {
			size_t n = min_t(size_t, chunk, length - off);

			if (copy_from_user(bounce,
					   (void __user *)(uintptr_t)
					   (start + off), n)) {
				crv = -EFAULT;
				break;
			}
			crv = onic_ddr4_write(dev->priv,
					      ddr_off + off, bounce, n);
			if (crv)
				break;
			off += n;
		}
		kfree(bounce);
		if (crv) {
			pr_info_ratelimited("onic_ib: reg_user_mr DDR4 mirror copy failed at off=%llu rv=%d\n",
					    (unsigned long long)off, crv);
			onic_ddr_mr_free(&dev->ddr, ddr_off);
			kfree(mr);
			return ERR_PTR(crv);
		}
	}

	/* 64-bit DDR4 addr the ERNIC DMA targets on remote WRITE/READ.
	 * MSB carries the libreconic 0xA3500000 tag — same routing reason
	 * as SQBAi/RQBAi/CQBAi (see onic_modify_qp_reset_to_init): ERNIC's
	 * M_AXI master uses sys_mem_5to2 M01 only for tagged addresses. */
	buf_addr = ((u64)ONIC_DDR4_MSB << 32) |
		   (dev->ddr.base_off + ddr_off);

	/* Program the 8 PDT registers (F1 §5.1.2, stride 0x100).
	 * Mirrored into BOTH ERNICs so the same PD/MR is usable from a QP
	 * bound to either port — port_num is not known at MR-registration
	 * time.  Each ERNIC has its own 2048-entry PDT bank; the mirrored
	 * writes go to identical slots in each. */
	{
		const u32 ernic_bases[2] = {
			RN_RDMA_BASE_ADDRESS,    /* ERNIC0 (port 1) */
			RN_RDMA_1_BASE_ADDRESS,  /* ERNIC1 (port 2) */
		};
		int e;
		for (e = 0; e < 2; e++) {
			row_base = ernic_bases[e] + pd->pdn * 0x100;
			iowrite32(pd->pdn,                         mmio + row_base + 0x00);
			/* PDT row layout per PG332 v4.2 Table 8 (line 1454-1505):
			 *   0x00 PDPDNUM       (24-bit PD number)
			 *   0x04 VIRTADDRLSB   (virtual address LSB — the buffer's VA
			 *                       as the application sees it; the
			 *                       incoming WRITE RETH carries this VA;
			 *                       ERNIC range-checks remote_addr in
			 *                       [VIRTADDR, VIRTADDR+length])
			 *   0x08 VIRTADDRMSB   (VA MSB)
			 *   0x0C BUFBASEADDRLSB(physical/DDR4 buffer addr LSB —
			 *                       where ERNIC actually writes via M_AXI;
			 *                       must be the 0xA3500000-tagged DDR4 slot
			 *                       for routing through sys_mem_5to2 M01)
			 *   0x10 BUFBASEADDRMSB(buf addr MSB)
			 *   0x14 BUFRKEY       (8-bit R_KEY)
			 *   0x18 WRRDBUFLEN    (length LSB)
			 *   0x1C ACCESSDESC    ([3:0]: 0000=R, 0001=W, 0010=RW,
			 *                       others NOT SUPPORTED;
			 *                       [31:16]: length MSB)
			 *
			 * 2026-05-06 fixes (after PG332 read):
			 * (1) VIRTADDR was previously buf_addr; standard verbs (perftest,
			 *     rdma_cm apps) carry the user VA in WRITE RETH so the
			 *     range check failed with bit-20 syndrome on every WRITE.
			 *     Now VIRTADDR = mr->va (user VA from reg_user_mr).
			 * (2) ACCESSDESC was `mr->access & 0x3` which for verbs flags
			 *     LOCAL_WRITE|REMOTE_WRITE = 0x3 produced encoding "0011"
			 *     which PG332 lists as "Not supported" → bit-20 fail.
			 *     Map verbs flags to ERNIC encoding explicitly:
			 *       no remote access     → 0b0000 (engine unusable)
			 *       REMOTE_WRITE only    → 0b0001
			 *       REMOTE_READ only     → 0b0000
			 *       REMOTE_WRITE + READ  → 0b0010
			 */
			{
				/* ACCESSDESC mapping (PG332 §Table 8 0x1C):
				 *   0x0=R, 0x1=W, 0x2=RW, others reserved.
				 *
				 * 2026-05-11 fix (B7 SEND-path):
				 *   Earlier mapping wrote 0x0 when only LOCAL_WRITE was
				 *   requested.  pingpong does reg_mr(IB_ACCESS_LOCAL_WRITE)
				 *   for the SEND/RECV buffer, never REMOTE_*.  ERNIC then
				 *   rejected every incoming SEND with syndrome bit 20
				 *   ("access perm fail"): the responder must be able to
				 *   *write* the payload into the local buffer, so the
				 *   PDT entry must permit W.  Working bare-metal
				 *   (tools/ernic-baremetal/bm_qp.c:99-102) uses RW
				 *   unconditionally for the same reason — RDMA-WRITE
				 *   targets need W, SEND/RECV need W, RDMA-READ targets
				 *   need R, and PG332's 0x2=RW is the only encoding
				 *   that covers all of them. */
				u32 access_enc = 0x2;   /* Read and Write */
				(void)mr->access;

				iowrite32((u32)(mr->va & 0xffffffffu),     mmio + row_base + 0x04);
				iowrite32((u32)(mr->va >> 32),             mmio + row_base + 0x08);
				iowrite32((u32)(buf_addr & 0xffffffffu),   mmio + row_base + 0x0C);
				iowrite32((u32)(buf_addr >> 32),           mmio + row_base + 0x10);
				iowrite32(mr->rkey & 0xffu,                mmio + row_base + 0x14);
				iowrite32((u32)(length & 0xffffffffu),     mmio + row_base + 0x18);
				iowrite32(((u32)(length >> 16) & 0xffff0000u) |
					  (access_enc & 0xfu),             mmio + row_base + 0x1C);
				(void)ioread32(mmio + row_base + 0x00);
				pr_info("onic_ib: PDT[%u]@ernic%d va=0x%llx buf=0x%llx rkey=%u len=%llu accdesc=0x%x readback: 04=%08x 08=%08x 0C=%08x 10=%08x 14=%08x 18=%08x 1C=%08x\n",
					pd->pdn, e,
					(unsigned long long)mr->va,
					(unsigned long long)buf_addr,
					(unsigned)(mr->rkey & 0xff),
					(unsigned long long)length,
					(unsigned)(((u32)(length >> 16) & 0xffff0000u) | (access_enc & 0xfu)),
					ioread32(mmio + row_base + 0x04),
					ioread32(mmio + row_base + 0x08),
					ioread32(mmio + row_base + 0x0C),
					ioread32(mmio + row_base + 0x10),
					ioread32(mmio + row_base + 0x14),
					ioread32(mmio + row_base + 0x18),
					ioread32(mmio + row_base + 0x1C));
			}
		}
	}

	mr->ibmr.lkey = mr->lkey;
	mr->ibmr.rkey = mr->rkey;

	spin_lock(&pd->mr_lock);
	pd->mr = mr;
	spin_unlock(&pd->mr_lock);

	return &mr->ibmr;
}

static int onic_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct onic_mr     *mr   = to_onic_mr(ibmr);
	struct onic_pd     *pd   = mr->pd;
	struct onic_ib_dev *dev  = to_onic_ib_dev(ibmr->device);
	void __iomem       *mmio = dev->priv->hw.addr;
	const u32           ernic_bases[2] = {
		RN_RDMA_BASE_ADDRESS, RN_RDMA_1_BASE_ADDRESS,
	};
	u32                 row_base;
	int                 e, i;
	(void)udata;

	/* Clear the mirrored PDT row in both ERNICs. */
	for (e = 0; e < 2; e++) {
		row_base = ernic_bases[e] + pd->pdn * 0x100;
		for (i = 0; i < 8; i++)
			iowrite32(0, mmio + row_base + i * 4);
		(void)ioread32(mmio + row_base + 0);
	}

	onic_ddr_mr_free(&dev->ddr, mr->ddr_off);

	spin_lock(&pd->mr_lock);
	pd->mr = NULL;
	spin_unlock(&pd->mr_lock);

	kfree(mr);
	return 0;
}

static int onic_create_cq(struct ib_cq *ibcq,
			  const struct ib_cq_init_attr *attr,
			  struct uverbs_attr_bundle *attrs)
{
	struct onic_cq *cq = to_onic_cq(ibcq);
	(void)attrs;

	if (attr->cqe == 0 || attr->cqe > 1024)
		return -EINVAL;

	spin_lock_init(&cq->lock);
	cq->depth = attr->cqe;
	cq->head  = 0;
	cq->tail  = 0;
	cq->num_bound_qps = 0;
	memset(cq->bound_qps, 0, sizeof(cq->bound_qps));
	/* cq_id and ddr_off get filled in by the first QP that binds. */
	return 0;
}

static int onic_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct onic_cq *cq = to_onic_cq(ibcq);
	unsigned long   flags;
	int             rv = 0;
	(void)udata;

	spin_lock_irqsave(&cq->lock, flags);
	if (cq->num_bound_qps != 0)
		rv = -EBUSY;
	spin_unlock_irqrestore(&cq->lock, flags);
	return rv;
}

static int onic_create_qp(struct ib_qp *ibqp,
			  struct ib_qp_init_attr *init_attr,
			  struct ib_udata *udata)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(ibqp->device);
	struct onic_pd     *pd   = to_onic_pd(ibqp->pd);
	struct onic_qp     *qp   = to_onic_qp(ibqp);
	struct onic_cq     *scq  = init_attr->send_cq ?
				   to_onic_cq(init_attr->send_cq) : NULL;
	struct onic_cq     *rcq  = init_attr->recv_cq ?
				   to_onic_cq(init_attr->recv_cq) : NULL;
	u32                 qp_idx;
	u64                 slot_off, sq_off, rq_off, cq_off;
	int                 rv;
	(void)udata;

	if (init_attr->qp_type != IB_QPT_RC)
		return -EOPNOTSUPP;
	/* DDR4 SQ buffer is 4 KB at SQBA + slot*64; max 64 entries.
	 * Same for RQ buffer at RQBA. Bumped from 16 → 64 (2026-05-06) to
	 * exercise wrap-around bug at higher iters; if wrap bug bites, use
	 * -t N where N >= iters to avoid it. */
	/* Cap-check the user-requested depths against what our DDR4 slot
	 * layout can hold.  ERNIC_MAX_{SQ,RQ}_DEPTH is derived from the
	 * per-QP slot regions in onic_ddr_alloc.h; the actual depth used
	 * downstream is clamped via clamp_t in this function too, but
	 * outright rejecting requests beyond capacity gives userspace a
	 * clear error instead of silently shrinking. */
	if (init_attr->cap.max_send_wr > ERNIC_MAX_SQ_DEPTH ||
	    init_attr->cap.max_recv_wr > ERNIC_MAX_RQ_DEPTH ||
	    init_attr->cap.max_send_sge > 1 ||
	    init_attr->cap.max_recv_sge > 1)
		return -EINVAL;
	if (!scq || !rcq)
		return -EINVAL;
	if (rcq != scq)
		return -EINVAL;     /* ERNIC's per-QP SQ/RQ→CQ is 1:1; the QP's
				     * send_cq and recv_cq objects must be the
				     * same CQ.  Multiple QPs may still share
				     * that CQ — see below. */
	/* CQ-sharing capacity check: bind slot reservation is done after
	 * onic_ddr_qp_slot_alloc succeeds (so we don't reserve a CQ slot
	 * for a QP that can't get a DDR4 slot).  See "Bind into CQ" below. */
	{
		unsigned long flags;
		bool full;
		spin_lock_irqsave(&scq->lock, flags);
		full = (scq->num_bound_qps >= ONIC_CQ_MAX_QPS);
		spin_unlock_irqrestore(&scq->lock, flags);
		if (full)
			return -ENOMEM;
	}

	/* Require a PD with an MR registered — ERNIC needs the PDT row filled
	 * before the QP can reference it. */
	spin_lock(&pd->mr_lock);
	if (!pd->mr) {
		spin_unlock(&pd->mr_lock);
		return -EINVAL;
	}
	spin_unlock(&pd->mr_lock);

	rv = onic_ddr_qp_slot_alloc(&dev->ddr, &qp_idx, &slot_off);
	if (rv)
		return rv;

	sq_off = slot_off + ONIC_DDR_QUEUE_SQ_OFF;
	rq_off = slot_off + ONIC_DDR_QUEUE_RQ_OFF;
	cq_off = slot_off + ONIC_DDR_QUEUE_CQ_OFF;

	qp->qp_num     = qp_idx;
	qp->pd         = pd;
	qp->send_cq    = scq;
	qp->recv_cq    = rcq;
	qp->slot_off   = slot_off;
	/* Clamp queue depths to the spec-tested range.
	 *
	 * PG332 v4.3 §"RC QP Creation" (p.71): "The minimum tested depth of
	 * the queues is 16."  Operating below this floor lets the engine's
	 * internal credit logic hit untested edges — we observed multi-iter
	 * ibv_rc_pingpong (-r 1) deadlocking in a HW retransmit storm
	 * 2026-05-11, with the first iter completing normally and the
	 * second iter stalled at SQPI=2/CQHEAD=1 while STATSSN grew into
	 * the millions.  Floor at 16 to stay inside the characterized
	 * envelope.  Ceiling is the per-slot RQ region capacity (computed
	 * in onic_ib.h:ERNIC_MAX_RQ_DEPTH); same for SQ. */
	qp->sq_depth = clamp_t(u32, init_attr->cap.max_send_wr,
			       ERNIC_MIN_QUEUE_DEPTH, ERNIC_MAX_SQ_DEPTH);
	qp->rq_depth = clamp_t(u32, init_attr->cap.max_recv_wr,
			       ERNIC_MIN_QUEUE_DEPTH, ERNIC_MAX_RQ_DEPTH);
	if (qp->sq_depth != init_attr->cap.max_send_wr ||
	    qp->rq_depth != init_attr->cap.max_recv_wr)
		pr_info("onic_ib: qp[%u] depth clamp: SQ %u->%u, RQ %u->%u (spec floor=%u, RQ ceil=%u)\n",
			qp_idx,
			init_attr->cap.max_send_wr, qp->sq_depth,
			init_attr->cap.max_recv_wr, qp->rq_depth,
			ERNIC_MIN_QUEUE_DEPTH, ERNIC_MAX_RQ_DEPTH);
	qp->cq_depth   = scq->depth;
	qp->path_mtu   = 4;                       /* PATHMTU code 4 = 4096 */
	qp->state      = ERNIC_QP_RESET;
	mutex_init(&qp->state_lock);

	/* DDR4 byte offsets and ernic_base are deferred to RESET->INIT —
	 * we don't know which ERNIC (port 1 = ERNIC0, port 2 = ERNIC1) the
	 * QP is bound to until that transition.  Both QCSR programming
	 * (SQBA/RQBA/CQBA/QPCONF/etc.) and post_send/poll_cq DDR4 byte
	 * offsets are computed there. */
	qp->ernic_base      = 0;
	qp->sq_ddr_off      = 0;
	qp->rq_ddr_off      = 0;
	qp->cq_ddr_off      = 0;
	qp->sq_pidb         = 0;
	qp->rq_pidb         = 0;
	qp->cq_consumer_idx = 0;
	qp->rq_consumer_idx = 0;

	/* Perf #1 — bind this QP's slot in the dev's coherent doorbell page.
	 * 4 B per QP, indexed by qp_idx.  cq_pidb at offset 0, rq_pidb at
	 * offset 0x400.  Programming of CQDBADDi/RQWPTRDBADDi to these bus
	 * addresses happens in modify_qp R->I (where the registers are
	 * written for the first time). */
	if (dev->hdb.enabled) {
		u8 *base = (u8 *)dev->hdb.vaddr;

		qp->hdb_cq     = (volatile __le32 *)(base + qp_idx * 4u);
		qp->hdb_rq     = (volatile __le32 *)(base + 0x400u + qp_idx * 4u);
		qp->hdb_cq_dma = dev->hdb.dma + qp_idx * 4u;
		qp->hdb_rq_dma = dev->hdb.dma + 0x400u + qp_idx * 4u;
		WRITE_ONCE(*qp->hdb_cq, 0);
		WRITE_ONCE(*qp->hdb_rq, 0);
		qp->hdb_active = true;
	} else {
		qp->hdb_cq     = NULL;
		qp->hdb_rq     = NULL;
		qp->hdb_cq_dma = 0;
		qp->hdb_rq_dma = 0;
		qp->hdb_active = false;
	}

	qp->sq_shadow = kcalloc(qp->sq_depth, sizeof(*qp->sq_shadow),
				GFP_KERNEL);
	qp->rq_shadow = kcalloc(qp->rq_depth, sizeof(*qp->rq_shadow),
				GFP_KERNEL);
	if (!qp->sq_shadow || !qp->rq_shadow) {
		kfree(qp->sq_shadow);
		kfree(qp->rq_shadow);
		qp->sq_shadow = NULL;
		qp->rq_shadow = NULL;
		onic_ddr_qp_slot_free(&dev->ddr, qp_idx);
		return -ENOMEM;
	}

	/* Bind this QP into the CQ's QP list (multi-QP-per-CQ).
	 *
	 * cq_id and ddr_off keep their B7 meaning for the first-bound QP
	 * only (informational; not relied on by poll_cq).  Subsequent QPs
	 * leave them at the first-bind values — poll_cq looks up each QP's
	 * own DDR4 CQ offset via qp->cq_ddr_off, not cq->ddr_off. */
	{
		unsigned long flags;
		spin_lock_irqsave(&scq->lock, flags);
		if (scq->num_bound_qps >= ONIC_CQ_MAX_QPS) {
			/* Race lost to another concurrent create_qp; unwind.
			 * The early capacity check above would have failed,
			 * but two creates can race past it. */
			spin_unlock_irqrestore(&scq->lock, flags);
			kfree(qp->sq_shadow);
			kfree(qp->rq_shadow);
			qp->sq_shadow = NULL;
			qp->rq_shadow = NULL;
			onic_ddr_qp_slot_free(&dev->ddr, qp_idx);
			return -ENOMEM;
		}
		if (scq->num_bound_qps == 0) {
			/* First QP into this CQ — fill the informational fields. */
			scq->cq_id   = qp_idx;
			scq->ddr_off = cq_off;
		}
		scq->bound_qps[scq->num_bound_qps++] = qp;
		spin_unlock_irqrestore(&scq->lock, flags);
	}

	/* B6: leave state at RESET — ibverbs sends an explicit RESET→INIT
	 * modify_qp right after create_qp and that call is what advances
	 * us into INIT.  Matches IB spec expectations. */
	qp->state        = ERNIC_QP_RESET;
	qp->ibqp.qp_num  = qp_idx;

	/* Expose this QP to userspace via debugfs.  Reading
	 * /sys/kernel/debug/onic/<ibdev>/qp<qpn>/dump prints GCSR+QCSR
	 * state to dmesg without modifying any QP state. */
	onic_debugfs_qp_add(dev, qp);
	return 0;
}

static int onic_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct onic_qp     *qp   = to_onic_qp(ibqp);
	struct onic_ib_dev *dev  = to_onic_ib_dev(ibqp->device);
	void __iomem       *mmio = dev->priv->hw.addr;
	(void)udata;

	/* Tear debugfs down FIRST so no further cat-dump can land while we
	 * are clearing QCSR / freeing shadow rings.  debugfs_remove_recursive
	 * is synchronous w.r.t. open file handles. */
	onic_debugfs_qp_remove(qp);

	/* QCSR teardown only if the QP made it past RESET — before
	 * RESET->INIT the QCSR window was never programmed.
	 *
	 * Gap-D fix (2026-05-03): do NOT zero SQBAi/RQBAi/CQBAi here.
	 * After a WRITE retry-exhausted teardown, ERNIC may still have an
	 * in-flight WQE refetch DMA queued internally; with SQBA cleared to
	 * 0/0 the DMA targets host bus address 0 and trips AMD-Vi
	 * IO_PAGE_FAULT (domain=0x22 addr=0x0).  Mirrors the earlier fix for
	 * RQWPTRDBADDi/CQDBADDi: those are configuration-target addresses,
	 * not state.  The next R->I that takes this slot reprograms them.
	 *
	 * We DO clear QPCONFi (which turns QPEN=0 → engine stops processing
	 * this QP), QPADVCONFi, QDEPTHi, PDi.  Those are control state, not
	 * DMA targets. */
	if (qp->ernic_base) {
		u32 q          = onic_qcsr(qp, 0x00);
		u32 qpen_addr  = qp->ernic_base + 0x00100044u;
		u32 qpen_mask;

		iowrite32(0, mmio + q);             /* QPCONFi → QPEN off */
		iowrite32(0, mmio + q + 0x04);      /* QPADVCONFi */
		iowrite32(0, mmio + q + 0x3C);      /* QDEPTHi */
		iowrite32(0, mmio + q + 0xB0);      /* PDi */

		/* Reset PSN tracking state. Without this, LSTRQREQi (responder's
		 * last-received PSN) and SQPSNi (sender's outgoing PSN) carry the
		 * prior QP's values into the next QP that lands at this slot. The
		 * next R->I rewrites these via onic_modify_qp_init_to_rtr (LSTRQREQi
		 * @ 0x44) and rtr_to_rts (SQPSNi @ 0x40), but in the gap between
		 * destroy_qp clearing QPEN and the new RTR programming, an
		 * incoming packet validates against stale state -> syndrome 21
		 * (PSN sequence error) -> silent INALLDRP increment. Explains the
		 * "first run hangs / second run works" pattern observed since
		 * project_b7_first_pingpong_2026_05_03.
		 * 2026-05-06 fix: zero them at destroy. */
		iowrite32(0, mmio + q + 0x40);      /* SQPSNi */
		iowrite32(0, mmio + q + 0x44);      /* LSTRQREQi */

		/* XRNIC_CONF_QP_EN is a per-QP bitmap (PG332 §Table 8 line 996
		 * documents it as a count but actual hardware uses bits — see
		 * onic_modify_qp_rtr_to_rts comment).  Clear our bit so the
		 * engine drops the next-arriving packet for this slot with
		 * PG332 §Table 4 syndrome bit 15, instead of silently
		 * processing it against a half-torn-down QPCONFi/RQBA. */
		qpen_mask = ioread32(mmio + qpen_addr);
		iowrite32(qpen_mask & ~(1u << qp->qp_num), mmio + qpen_addr);
		(void)ioread32(mmio + q);
	}

	/* Unbind this QP from its CQ's list.  send_cq == recv_cq for us
	 * (enforced at create_qp), so this single removal covers both
	 * sides.  Order preserved by compacting any tail entries down. */
	if (qp->send_cq) {
		struct onic_cq *cq = qp->send_cq;
		unsigned long   flags;
		u32             i;

		spin_lock_irqsave(&cq->lock, flags);
		for (i = 0; i < cq->num_bound_qps; i++) {
			if (cq->bound_qps[i] != qp)
				continue;
			memmove(&cq->bound_qps[i],
				&cq->bound_qps[i + 1],
				(cq->num_bound_qps - i - 1) *
				sizeof(cq->bound_qps[0]));
			cq->num_bound_qps--;
			cq->bound_qps[cq->num_bound_qps] = NULL;
			break;
		}
		spin_unlock_irqrestore(&cq->lock, flags);
	}

	/* Perf #1 — zero our host doorbell slot so a fresh QP later assigned
	 * to the same qp_idx sees a clean producer index.  Doing this after
	 * the QCSR teardown (which cleared QPCONFi[0] QPEN above) guarantees
	 * the engine has stopped issuing doorbell DMA writes for this QP
	 * before we reset the slot.  Note: we leave CQDBADDi/RQWPTRDBADDi
	 * non-zero — the next R->I on this slot will reprogram them. */
	if (qp->hdb_cq)
		WRITE_ONCE(*qp->hdb_cq, 0);
	if (qp->hdb_rq)
		WRITE_ONCE(*qp->hdb_rq, 0);

	onic_ddr_qp_slot_free(&dev->ddr, qp->qp_num);

	/* B7 — release shadow rings allocated in create_qp. */
	kfree(qp->sq_shadow);
	kfree(qp->rq_shadow);
	qp->sq_shadow = NULL;
	qp->rq_shadow = NULL;

	qp->state = ERNIC_QP_CLOSED;
	return 0;
}

/* ---- remaining stubs (B6/B7/B8 land these) ----------------------------- */

/* ---- B6: modify_qp real implementation --------------------------------- */

static bool gid_is_ipv4(const union ib_gid *g)
{
	static const u8 pfx[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
	return memcmp(g->raw, pfx, 12) == 0;
}

static int onic_modify_qp_reset_to_init(struct onic_qp *qp,
					struct ib_qp_attr *attr, int mask)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	struct onic_pd     *pd   = qp->pd;
	void __iomem       *mmio = dev->priv->hw.addr;
	const int required = IB_QP_STATE | IB_QP_PKEY_INDEX |
			     IB_QP_PORT  | IB_QP_ACCESS_FLAGS;
	u64 port_off, sq_off, rq_off, cq_off;
	u32 qpconfi, qpadvconfi, qdepthi, q;
	u32 sq_lsb, sq_msb, rq_lsb, rq_msb, cq_lsb, cq_msb;

	if ((mask & required) != required) {
		pr_info_ratelimited("onic_ib: R->I missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}
	if (attr->port_num != 1 && attr->port_num != 2) {
		pr_info_ratelimited("onic_ib: R->I port_num=%u, only 1 or 2 supported\n",
				    attr->port_num);
		return -EOPNOTSUPP;
	}
	if (attr->pkey_index != 0)
		return -EINVAL;

	/* Bind QP to the chosen ERNIC.  All subsequent QCSR accesses (this
	 * function plus INIT->RTR, RTR->RTS, post_send doorbell, post_recv
	 * doorbell, poll_cq CQHEAD read, destroy_qp teardown) go through
	 * onic_qcsr(qp, off) and resolve to qp->ernic_base. */
	qp->ernic_base = onic_ernic_base_for_port(attr->port_num);
	qp->port_num   = attr->port_num;

	/* Refresh ERNIC global identity (MAC/IP) — by RESET->INIT the user
	 * has typically assigned an IP that wasn't present at ib_register
	 * time.  Idempotent: the engine-enable bit is already latched. */
	onic_ernic_global_init(dev, attr->port_num);

	/* DDR4 byte offsets — F4 §2.2 disjoint halves, port 1 in 0..2G,
	 * port 2 in 0x2_0000_0000.. .  Slot itself lives at slot_off
	 * inside the half. */
	port_off = onic_port_ddr_off(attr->port_num);
	sq_off = qp->slot_off + ONIC_DDR_QUEUE_SQ_OFF;
	rq_off = qp->slot_off + ONIC_DDR_QUEUE_RQ_OFF;
	cq_off = qp->slot_off + ONIC_DDR_QUEUE_CQ_OFF;

	/* Two masters reach the SAME DDR4 byte through different fabric
	 * paths in the open-nic-shell (per agent investigation 2026-04-29):
	 *   - ERNIC's M_AXI master goes through `sys_mem_5to2` and routes
	 *     to M01 (DDR4 path) only when the address has the libreconic
	 *     device-mem tag 0xA350_0000_0000_0000 in its high bits.  So
	 *     SQBAi/RQBAi/CQBAi register pairs must carry that tag in the
	 *     MSB half (the 5-to-2 crossbar matches on it).
	 *   - QDMA's H2C MM master is wired directly to dev_mem_4to1 S00
	 *     and reaches DDR4 at the plain byte offset (untagged).
	 * So: keep the tag on register writes, but pass the untagged byte
	 * offset to onic_ddr4_write/read in post_send/poll_cq. */
	qp->sq_ddr_off = port_off + sq_off;
	qp->rq_ddr_off = port_off + rq_off;
	qp->cq_ddr_off = port_off + cq_off;
	/* cq->ddr_off was a per-CQ snapshot of the QP's CQ DDR4 offset,
	 * used only by debug paths.  With multi-QP-per-CQ that field
	 * only meaningfully describes the first-bound QP; poll_cq reaches
	 * per-QP cq_ddr_off through the bound_qps[] iterator instead. */

	{
		u64 sq_axi = ((u64)ONIC_DDR4_MSB << 32) | qp->sq_ddr_off;
		u64 rq_axi = ((u64)ONIC_DDR4_MSB << 32) | qp->rq_ddr_off;
		u64 cq_axi = ((u64)ONIC_DDR4_MSB << 32) | qp->cq_ddr_off;
		sq_lsb = (u32)(sq_axi & 0xffffffffu);
		sq_msb = (u32)(sq_axi >> 32);
		rq_lsb = (u32)(rq_axi & 0xffffffffu);
		rq_msb = (u32)(rq_axi >> 32);
		cq_lsb = (u32)(cq_axi & 0xffffffffu);
		cq_msb = (u32)(cq_axi >> 32);
	}

	/* QPCONFi per PG332 (definitions from libreconic/rdma_api.c comments
	 * 410-422, working values masked at 0x30 for bits [5:4]):
	 *   bit 0    QPEN              — RTR->RTS turns it on
	 *   bit 2    RQINTEN
	 *   bit 3    CQINTEN
	 *   bit 4    HWHSHKDIS         — REQUIRED.  When 0, ERNIC expects
	 *                                HW handshake ports (not wired in
	 *                                this shell), so the engine errors
	 *                                processing the WQE.
	 *   bit 5    CQE write enable  — REQUIRED.  Without this set, ERNIC
	 *                                does not write CQEs to DDR4.
	 *   bit 7    IPv4 (set in INIT->RTR after dgid known)
	 *   [10:8]   PATHMTU
	 *   [31:16]  RQBUFSZ in 256-B units (=2 for 512-B libreconic RQEs)
	 */
	qpconfi  = 0;
	qpconfi |= (1u << 2);                      /* RQINTEN */
	qpconfi |= (1u << 3);                      /* CQINTEN */
	qpconfi |= (1u << 4);                      /* HWHSHKDIS */
	qpconfi |= (1u << 5);                      /* CQE write enable */
	qpconfi |= ((u32)qp->path_mtu & 0x7) << 8;
	qpconfi |= ((u32)ERNIC_RQE_SIZE_FIELD & 0xffffu) << 16;

	qpadvconfi  = 0;
	qpadvconfi |= (0u    << 0);                /* TC */
	qpadvconfi |= (64u   << 8);                /* TTL */
	qpadvconfi |= (0xFFFFu << 16);             /* PKEY */

	qdepthi  = ((u32)qp->rq_depth << 16) | (u32)qp->sq_depth;

	q = onic_qcsr(qp, 0x00);
	iowrite32(qpconfi,     mmio + q);                         /* QPCONFi   */
	iowrite32(qpadvconfi,  mmio + q + 0x04);                  /* QPADVCONFi*/
	iowrite32(rq_lsb,      mmio + q + 0x08);                  /* RQBAi     */
	iowrite32(rq_msb,      mmio + q + 0xC0);                  /* RQBAMSBi  */
	iowrite32(sq_lsb,      mmio + q + 0x10);                  /* SQBAi     */
	iowrite32(sq_msb,      mmio + q + 0xC8);                  /* SQBAMSBi  */
	iowrite32(cq_lsb,      mmio + q + 0x18);                  /* CQBAi     */
	iowrite32(cq_msb,      mmio + q + 0xD0);                  /* CQBAMSBi  */
	iowrite32(qdepthi,     mmio + q + 0x3C);                  /* QDEPTHi   */
	iowrite32(pd->pdn,     mmio + q + 0xB0);                  /* PDi       */
	/* Doorbell DMA targets — ERNIC issues these writes unconditionally
	 * regardless of QPCONFi[4] HWHSHKDIS, so the registers must point
	 * at a valid landing zone.  Two paths:
	 *   - hdb_active: host coherent page allocated at ib_register.
	 *     Bus address has bits [63:52] = 0, so it routes through the
	 *     5-to-2 crossbar's M00 (host PCIe) without the 0xA350 tag.
	 *     Saves one MMIO read per poll_cq (Perf #1).
	 *   - !hdb_active: DDR4 slot tail at +0x3F00 / +0x3F08, tagged with
	 *     0xA350 so the crossbar's M01 path takes over.  This is the
	 *     pre-Perf-#1 fallback (host_doorbell=0 or alloc failed).
	 * Either way ERNIC's writes never traverse PCIe to host addr 0 and
	 * therefore can't trip AMD-Vi/DMAR IO_PAGE_FAULT. */
	{
		u32 rqdb_lsb, rqdb_msb, cqdb_lsb, cqdb_msb;

		if (qp->hdb_active) {
			rqdb_lsb = lower_32_bits(qp->hdb_rq_dma);
			rqdb_msb = upper_32_bits(qp->hdb_rq_dma);
			cqdb_lsb = lower_32_bits(qp->hdb_cq_dma);
			cqdb_msb = upper_32_bits(qp->hdb_cq_dma);
		} else {
			u64 rqdb_off = qp->slot_off + ONIC_DDR_QUEUE_RQDB_OFF;
			u64 cqdb_off = qp->slot_off + ONIC_DDR_QUEUE_CQDB_OFF;
			u64 rqdb_axi = ((u64)ONIC_DDR4_MSB << 32) | rqdb_off;
			u64 cqdb_axi = ((u64)ONIC_DDR4_MSB << 32) | cqdb_off;

			rqdb_lsb = lower_32_bits(rqdb_axi);
			rqdb_msb = upper_32_bits(rqdb_axi);
			cqdb_lsb = lower_32_bits(cqdb_axi);
			cqdb_msb = upper_32_bits(cqdb_axi);
		}
		iowrite32(rqdb_lsb, mmio + q + 0x20);    /* RQWPTRDBADDi    */
		iowrite32(rqdb_msb, mmio + q + 0x24);    /* RQWPTRDBADDMSBi */
		iowrite32(cqdb_lsb, mmio + q + 0x28);    /* CQDBADDi        */
		iowrite32(cqdb_msb, mmio + q + 0x2C);    /* CQDBADDMSBi     */
	}

	/* libreconic-style fatal recovery to clear stale engine state.
	 * Per rdma_api.c:1104-1128.  Required because:
	 *   - ERNIC IP retains per-QP register state across driver rmmod
	 *     / insmod (only a power-cycle truly resets it).
	 *   - SQPIi/CQHEADi/STATCURSQPTRi/STATRQPIDBi/STATMSNi are
	 *     read-only unless XRNICADCONF[0] (SW override) is on.  Without
	 *     SW override, writes silently fail (SQPSNi=0 reads back as
	 *     0xa40a2 from a previous test) or trigger AXI SLVERR which
	 *     hangs the PCIe bus.
	 *   - Without this clearing, the engine starts the next QP cycle
	 *     in retry/error from the stale STATSSN/STATMSN counts and
	 *     issues background DMA reads (host addr 0x0/0x1000/...) that
	 *     IOMMU rejects.
	 *
	 * Sequence:
	 *   1. XRNICADCONF[0] = 1   — SW override ON
	 *   2. QPCONFi[0] = 0       — QPEN OFF (already 0 here, but defensive)
	 *   3. Zero host-side and (now writable) status registers
	 *   4. QPCONFi[6] = 1       — QP under recovery (engine accepts the
	 *                              clean state and won't run retries)
	 *   5. XRNICADCONF[0] = 0   — SW override OFF for normal operation
	 * The subsequent QPCONFi write below sets QPEN=0 and clears the
	 * recovery bit; QPEN gets set by RTR->RTS as usual. */
	{
		u32 adconf_addr = qp->ernic_base + 0x00100004u;  /* GCSR XRNICADCONF */
		u32 adconf_orig = ioread32(mmio + adconf_addr);
		u32 qpconfi_pre;

		iowrite32(adconf_orig | 0x1u, mmio + adconf_addr);  /* SW override ON */
		(void)ioread32(mmio + adconf_addr);                  /* flush */

		qpconfi_pre = ioread32(mmio + q);
		iowrite32(qpconfi_pre & ~0x1u, mmio + q);            /* QPEN OFF */

		/* DO NOT zero RQWPTRDBADDi/CQDBADDi here — those are
		 * configuration (DMA target addresses), not state pointers.
		 * They were programmed above to safe DDR4 addresses (with the
		 * 0xA350.. tag) so doorbell DMA writes never traverse PCIe.
		 * Zeroing them caused ERNIC to DMA-write to host addr 0
		 * during QP transitions, triggering IOMMU IO_PAGE_FAULT /
		 * DMAR DMA Write faults that stalled the engine.
		 * 2026-05-03 fix: skip address-reg zeroing; only the
		 * count/sequence pointers below need clearing. */
		iowrite32(0, mmio + q + 0x30);                /* CQHEADi */
		iowrite32(0, mmio + q + 0x34);                /* RQCIi */
		iowrite32(0, mmio + q + 0x38);                /* SQPIi */
		iowrite32(0, mmio + q + 0x40);                /* SQPSNi */
		iowrite32(0, mmio + q + 0x44);                /* LSTRQREQi */
		iowrite32(0, mmio + q + 0x80);                /* STATSSNi */
		iowrite32(0, mmio + q + 0x84);                /* STATMSNi */
		iowrite32(0, mmio + q + 0x88);                /* STATQPi  — clear sticky syndrome (variance fix 2026-05-03) */
		iowrite32(0, mmio + q + 0x8C);                /* STATCURSQPTRi */
		iowrite32(0, mmio + q + 0x90);                /* STATRESPSNi — RO per PG332 v4.2 (sender-side expected-ACK PSN); write is a no-op, kept for symmetry with the rest of this STAT* clear block */
		iowrite32(0, mmio + q + 0x9C);                /* STATRQPIDBi */
		/* Perf #1 — zero the host coherent doorbell slots in lockstep
		 * with CQHEAD/STATRQPIDB.  poll_cq trusts this slot as the
		 * authoritative cq producer index; failing to zero it would
		 * leave a stale value from a previous QP cycle and make the
		 * first poll_cq deliver phantom completions. */
		if (qp->hdb_active) {
			WRITE_ONCE(*qp->hdb_cq, 0);
			WRITE_ONCE(*qp->hdb_rq, 0);
			smp_wmb();
		}

		qpconfi_pre = ioread32(mmio + q);
		iowrite32(qpconfi_pre | (1u << 6), mmio + q);        /* QP under recovery */
		(void)ioread32(mmio + q);

		iowrite32(adconf_orig, mmio + adconf_addr);          /* SW override OFF */
		(void)ioread32(mmio + adconf_addr);
	}

	qp->state = ERNIC_QP_INIT;
	pr_info("onic_ib: qp[%u] RESET -> INIT (port=%u pkey_idx=%u, ERNIC%u base=0x%08x ddr_off=0x%llx)\n",
		qp->qp_num, attr->port_num, attr->pkey_index,
		(attr->port_num == 1) ? 0u : 1u,
		qp->ernic_base, (unsigned long long)qp->sq_ddr_off);
	onic_dump_qp_state(qp, "after R->I");
	return 0;
}

static int onic_modify_qp_init_to_rtr(struct onic_qp *qp,
				      struct ib_qp_attr *attr, int mask)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;
	const int required = IB_QP_STATE | IB_QP_AV | IB_QP_PATH_MTU |
			     IB_QP_DEST_QPN | IB_QP_RQ_PSN;
	u32 q       = onic_qcsr(qp, 0x00);
	u32 destqp, mac_lsb, mac_msb;
	u32 ip1 = 0, ip2 = 0, ip3 = 0, ip4 = 0;
	u32 timeoutconf, qpconfi;
	const u8 *dmac;
	const union ib_gid *dgid;
	bool is_v4;
	u8  to_val, rt_val, rnr_rt, rnr_to;

	if ((mask & required) != required) {
		pr_info("onic_ib: I->R missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}
	if (!(rdma_ah_get_ah_flags(&attr->ah_attr) & IB_AH_GRH)) {
		pr_info("onic_ib: I->R no GRH in ah_attr\n");
		return -EINVAL;
	}
	dgid = &rdma_ah_read_grh(&attr->ah_attr)->dgid;
	dmac = rdma_ah_retrieve_dmac(&attr->ah_attr);
	if (!dmac) {
		pr_info("onic_ib: I->R dmac missing\n");
		return -EINVAL;
	}
	/* Accept any standard IB MTU (256..4096).  ERNIC PATHMTU field encoding
	 * is IB enum minus 1: IB_MTU_256(1)->0, IB_MTU_512(2)->1, ...,
	 * IB_MTU_4096(5)->4.  Reject only if outside the IB-enum range. */
	if (attr->path_mtu < IB_MTU_256 || attr->path_mtu > IB_MTU_4096) {
		pr_info("onic_ib: I->R path_mtu=%d out of range\n",
				    attr->path_mtu);
		return -EINVAL;
	}
	qp->path_mtu = (u8)((u32)attr->path_mtu - 1u);

	destqp  = attr->dest_qp_num & 0x00FFFFFFu;
	/* DMAC encoding per libreconic reconic.c convert_mac_addr_to_uint:
	 *   MSB = (mac[0]<<8) | mac[1]
	 *   LSB = (mac[2]<<24) | (mac[3]<<16) | (mac[4]<<8) | mac[5]
	 * The earlier encoding (LSB packed dmac[0..3] little-endian, MSB
	 * packed dmac[4..5]) was the bug behind 2026-04-29's silent SEND
	 * failure: ERNIC emitted 100+ packets per test (CMAC counted them
	 * as unicast TX with good FCS) but partner Mellanox saw nothing
	 * because the destination-MAC bytes on the wire were garbled. */
	mac_msb = ((u32)dmac[0] << 8) | (u32)dmac[1];
	mac_lsb = ((u32)dmac[2] << 24) | ((u32)dmac[3] << 16) |
		  ((u32)dmac[4] << 8)  | (u32)dmac[5];

	is_v4 = gid_is_ipv4(dgid);
	if (is_v4) {
		/* Pack bytes 12..15 of the IPv4-mapped GID big-endian: bit 31
		 * is byte[12] (the most-significant IPv4 octet).  libreconic
		 * convert_ip_addr_to_uint (reconic.c) does the same.  Earlier
		 * little-endian packing wrote 10.0.0.1 as 0x0100000a — Mellanox
		 * silently dropped it. */
		ip1 = ((u32)dgid->raw[12] << 24) |
		      ((u32)dgid->raw[13] << 16) |
		      ((u32)dgid->raw[14] <<  8) |
		      ((u32)dgid->raw[15]);
		ip2 = ip3 = ip4 = 0;
	} else {
		memcpy(&ip1, &dgid->raw[0],  4);
		memcpy(&ip2, &dgid->raw[4],  4);
		memcpy(&ip3, &dgid->raw[8],  4);
		memcpy(&ip4, &dgid->raw[12], 4);
	}

	to_val = (mask & IB_QP_TIMEOUT)       ? (attr->timeout       & 0x1F) : 14;
	rt_val = (mask & IB_QP_RETRY_CNT)     ? (attr->retry_cnt     & 0x07) : 7;
	rnr_rt = (mask & IB_QP_RNR_RETRY)     ? (attr->rnr_retry     & 0x07) : 7;
	rnr_to = (mask & IB_QP_MIN_RNR_TIMER) ? (attr->min_rnr_timer & 0x1F) : 12;
	timeoutconf = ((u32)to_val  <<  0) | ((u32)rt_val  <<  8) |
		      ((u32)rnr_rt  << 11) | ((u32)rnr_to  << 16);

	iowrite32(destqp,      mmio + q + 0x48);
	iowrite32(timeoutconf, mmio + q + 0x4C);
	iowrite32(mac_lsb,     mmio + q + 0x50);
	iowrite32(mac_msb,     mmio + q + 0x54);
	iowrite32(ip1,         mmio + q + 0x60);
	iowrite32(ip2,         mmio + q + 0x64);
	iowrite32(ip3,         mmio + q + 0x68);
	iowrite32(ip4,         mmio + q + 0x6C);

	/* LSTRQREQi (0x44) = (last-RQ-opcode << 24) | (LAST_received PSN & 0xFFFFFF).
	 * libreconic config_last_rq_psn (rdma_api.c:379-392) uses arbitrary
	 * opcode 0x0a "to avoid opcode sequence error".  The PSN field is the
	 * LAST-received PSN, NOT the next-expected.  libreconic's
	 * send_recv.c:281-284 sets rq_psn=N and sq_psn=N+1 — confirming the
	 * register holds last-received, with next-expected = LSTRQREQi+1.
	 * IB verbs's attr->rq_psn is the NEXT-expected PSN, so we must
	 * subtract 1 to get the last-received.  2026-05-03 fix: prior code
	 * wrote attr->rq_psn directly, which made ERNIC expect rq_psn+1.
	 * Every incoming frame triggered packet-validation syndrome 21
	 * (PSN sequence error) — observed as INVDUPCNT growing while
	 * INSRRPKT stayed at 0. */
	iowrite32(((u32)0x0a << 24) |
		  (((attr->rq_psn - 1u) & 0x00FFFFFFu)),
		  mmio + q + 0x44);

	/* QPCONFi[7]: IP version.  Per Xilinx PG332 v4.2 Table 8 (line ~2705):
	 *   "QP configured for IPv4 or IPv6: 0 = IPv4, 1 = IPv6"
	 * Prior code set bit 7 when is_v4 was true -- inverted!  That made
	 * ERNIC validate incoming frames against IPv6 headers while the wire
	 * carried IPv4 frames -> every packet failed RX validation and was
	 * dropped silently in INALLDRPPKTCNT.
	 * 2026-05-03 fix: correct sense -- bit 7 set ONLY for IPv6.
	 */
	/* Enable this QP at RTR, not at RTS. perftest's responder-side flow
	 * goes INIT->RTR only and never RTR->RTS — if QP_EN and QPCONFi[0]
	 * are deferred to RTS the responder can never receive. RTR is the
	 * IB state where the QP becomes able to receive, so it's the right
	 * place. RTS transition becomes idempotent on these bits.
	 * Verified 2026-05-06: server-side ib_write_bw with this fix sees
	 * INSRRPKT increment instead of INALLDRP saturating. */
	qpconfi = ioread32(mmio + q);
	qpconfi &= ~(1u << 7);
	if (!is_v4)
		qpconfi |= (1u << 7);
	qpconfi |= 0x1u;          /* QPCONFi[0]: per-QP enable */
	qpconfi &= ~(1u << 6);    /* clear "QP under recovery" sticky from R->I */
	iowrite32(qpconfi, mmio + q);
	(void)ioread32(mmio + q);

	/* OR-in this QP's bit in the global QP_EN bitmap. Per
	 * project_b7_qp_en_bitmap_2026_05_05: register is a per-QP enable
	 * bitmap, not a count. */
	{
		u32 qpen_addr = qp->ernic_base + 0x00100044u;
		u32 qpen      = ioread32(mmio + qpen_addr);
		u32 new_en    = qpen | (1u << qp->qp_num);
		if (new_en != qpen) {
			iowrite32(new_en, mmio + qpen_addr);
			(void)ioread32(mmio + qpen_addr);
		}
	}

	qp->dest_qp_num   = attr->dest_qp_num;
	qp->rq_psn        = attr->rq_psn;
	qp->timeout       = to_val;
	qp->retry_cnt     = rt_val;
	qp->rnr_retry     = rnr_rt;
	qp->min_rnr_timer = rnr_to;
	qp->path_mtu_ib   = attr->path_mtu;
	memcpy(qp->dmac,  dmac, 6);
	memcpy(&qp->dgid, dgid, sizeof(qp->dgid));

	qp->state = ERNIC_QP_RTR;
	pr_info("onic_ib: qp[%u] INIT -> RTR dest_qpn=%u dmac=%pM dgid=%pI6c rq_psn=%u\n",
		qp->qp_num, qp->dest_qp_num, qp->dmac, qp->dgid.raw, qp->rq_psn);
	onic_dump_qp_state(qp, "after I->R");
	return 0;
}

static int onic_modify_qp_rtr_to_rts(struct onic_qp *qp,
				     struct ib_qp_attr *attr, int mask)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;
	u32 q       = onic_qcsr(qp, 0x00);
	u32 qpen_ct, new_ct, qpconfi;
	/* XRNIC_CONF_QP_EN lives in this QP's ERNIC GCSR window (offset
	 * 0x100044 inside the ERNIC slice). */
	u32 qpen_addr = qp->ernic_base + 0x00100044u;
	const int required = IB_QP_STATE | IB_QP_SQ_PSN;

	if ((mask & required) != required) {
		pr_info_ratelimited("onic_ib: R->S missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}

	iowrite32(attr->sq_psn & 0x00FFFFFFu, mmio + q + 0x40);
	qp->sq_psn = attr->sq_psn;

	/* XRNIC_CONF_QP_EN is a per-QP BITMAP, not a count.
	 *
	 * PG332 v4.3 §Table 8 line 996 calls field [11:0] "Number of QPs
	 * enabled" but actual hardware honors bit i as "QP i enabled".
	 * Discovered 2026-05-05 via fpga/tools/ernic-baremetal/bar-poke:
	 * writing 0xFFFFFFFF reads back as 0x3F (6 bits) — masking semantics,
	 * not saturated count.  Confirmed by REQERRBUF entries: every packet
	 * to QP=2 hit PG332 §Table 4 bit 15 ("QP not configured/enabled")
	 * even though the prior count-style write set the register to 3,
	 * because bitmap 0b011 enables QPs 0 and 1 only.
	 *
	 * Now: OR in this QP's bit; never clear other QPs' bits here.
	 * destroy_qp clears the bit (see onic_destroy_qp). */
	qpen_ct = ioread32(mmio + qpen_addr);
	new_ct  = qpen_ct | (1u << qp->qp_num);
	if (new_ct != qpen_ct) {
		iowrite32(new_ct, mmio + qpen_addr);
		(void)ioread32(mmio + qpen_addr);
	}

	/* Note: STATRESPSNi (QCSR 0x90) is READ-ONLY per PG332 v4.2 Table 8
	 * lines 3000-3003.  It is the sender-side "expected ACK PSN" tracker,
	 * NOT the responder-side duplicate-detector seed.  An earlier patch
	 * wrote (rq_psn-1) here as a workaround — it was a silent no-op and
	 * misled debugging.  Responder-side next-expected RQ PSN is set via
	 * LSTRQREQi (QCSR 0x44, RW) at INIT->RTR (line ~1420). */

	/* Set QPEN=1 and clear "QP under recovery" (bit 6).  Our RESET->INIT
	 * recovery sequence sets bit 6 to push the engine into a clean
	 * state, but bit 6 is sticky through the subsequent QPCONFi write
	 * — observed 2026-04-29: STATSSN stayed 0 (no transmission) until
	 * bit 6 was explicitly cleared.  Clear it here, just before going
	 * live so the engine actually transmits. */
	qpconfi  = ioread32(mmio + q);
	qpconfi |= 0x1u;
	qpconfi &= ~(1u << 6);
	iowrite32(qpconfi, mmio + q);
	(void)ioread32(mmio + q);

	qp->state = ERNIC_QP_RTS;
	pr_info("onic_ib: qp[%u] RTR -> RTS sq_psn=%u qp_en_ct %u -> %u\n",
		qp->qp_num, qp->sq_psn, qpen_ct, max(qpen_ct, new_ct));
	onic_dump_qp_state(qp, "after R->S");
	return 0;
}

static int onic_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			  int attr_mask, struct ib_udata *udata)
{
	struct onic_qp *qp = to_onic_qp(ibqp);
	enum ib_qp_state new_ib_state;
	enum ernic_qp_state cur;
	int rv = 0;
	(void)udata;

	if (!(attr_mask & IB_QP_STATE)) {
		pr_info_ratelimited("onic_ib: modify_qp w/o IB_QP_STATE, mask=0x%x\n",
				    attr_mask);
		return -EINVAL;
	}
	new_ib_state = attr->qp_state;

	mutex_lock(&qp->state_lock);
	cur = qp->state;

	if (cur == ERNIC_QP_RESET && new_ib_state == IB_QPS_INIT)
		rv = onic_modify_qp_reset_to_init(qp, attr, attr_mask);
	else if (cur == ERNIC_QP_INIT && new_ib_state == IB_QPS_RTR)
		rv = onic_modify_qp_init_to_rtr(qp, attr, attr_mask);
	else if (cur == ERNIC_QP_RTR  && new_ib_state == IB_QPS_RTS)
		rv = onic_modify_qp_rtr_to_rts(qp, attr, attr_mask);
	else {
		pr_info_ratelimited("onic_ib: modify_qp unsupported %d -> %d\n",
				    (int)cur, (int)new_ib_state);
		rv = -EOPNOTSUPP;
	}

	mutex_unlock(&qp->state_lock);
	return rv;
}

static int onic_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct onic_qp *qp = to_onic_qp(ibqp);
	(void)attr_mask;

	memset(attr, 0, sizeof(*attr));
	mutex_lock(&qp->state_lock);
	switch (qp->state) {
	case ERNIC_QP_RESET:           attr->qp_state = IB_QPS_RESET; break;
	case ERNIC_QP_INIT:            attr->qp_state = IB_QPS_INIT;  break;
	case ERNIC_QP_RTR:             attr->qp_state = IB_QPS_RTR;   break;
	case ERNIC_QP_RTS:             attr->qp_state = IB_QPS_RTS;   break;
	case ERNIC_QP_ERR:
	case ERNIC_QP_UNDER_RECOVERY:  attr->qp_state = IB_QPS_ERR;   break;
	case ERNIC_QP_CLOSED: default: attr->qp_state = IB_QPS_RESET; break;
	}
	attr->cur_qp_state   = attr->qp_state;
	attr->path_mtu       = qp->path_mtu_ib ? qp->path_mtu_ib : IB_MTU_4096;
	attr->dest_qp_num    = qp->dest_qp_num;
	attr->rq_psn         = qp->rq_psn;
	attr->sq_psn         = qp->sq_psn;
	attr->timeout        = qp->timeout;
	attr->retry_cnt      = qp->retry_cnt;
	attr->rnr_retry      = qp->rnr_retry;
	attr->min_rnr_timer  = qp->min_rnr_timer;
	attr->port_num       = qp->port_num ? qp->port_num : 1;
	attr->pkey_index     = 0;
	mutex_unlock(&qp->state_lock);

	if (init_attr) {
		memset(init_attr, 0, sizeof(*init_attr));
		init_attr->qp_type           = IB_QPT_RC;
		init_attr->send_cq           = qp->send_cq ? &qp->send_cq->ibcq : NULL;
		init_attr->recv_cq           = qp->recv_cq ? &qp->recv_cq->ibcq : NULL;
		init_attr->cap.max_send_wr   = qp->sq_depth;
		init_attr->cap.max_recv_wr   = qp->rq_depth;
		init_attr->cap.max_send_sge  = 1;
		init_attr->cap.max_recv_sge  = 1;
	}
	return 0;
}
/* ====================================================================
 * B7 — real verb path: post_send / post_recv / poll_cq
 *
 * Layout of per-QP rings in DDR4 (already programmed via SQBAi/RQBAi/
 * CQBAi during create_qp; offsets captured in qp->{sq,rq,cq}_ddr_off):
 *   SQ slot s : qp->sq_ddr_off + s * sizeof(struct ernic_sq_wqe)   (64 B)
 *   RQ slot s : qp->rq_ddr_off + s * 512                           (256-B field == 2)
 *   CQ slot s : qp->cq_ddr_off + s * sizeof(struct ernic_cqe)      ( 4 B)
 *
 * Counters in qp->{sq_pidb, rq_pidb, cq_consumer_idx} are MONOTONIC
 * 32-bit (the engine never wraps them either) — slot index is
 * (counter % depth).
 *
 * Doorbells (per-QP QCSR offsets):
 *   SQPIi    0x38   host writes after wmb()
 *   RQCIi    0x34   host writes after consuming an RQ slot
 *   CQHEADi  0x30   engine writes; host reads to find ready CQEs
 *
 * Restrictions of this first cut:
 *   - num_sge == 1 only (B5 already capped at 1 in create_qp)
 *   - SEND payload must be ≤16 bytes inline (no DDR4 staging path yet)
 *   - RDMA_WRITE source must already live in a registered MR
 *   - IBV_WR_RDMA_READ is not implemented (separate work item)
 * ==================================================================== */

/* Translate a libibverbs send opcode into an ERNIC WQE opcode and the
 * `enum ib_wc_opcode` we'll echo back via poll_cq. */
static int ernic_xlate_opcode(enum ib_wr_opcode wr_op,
			      u32 *out_ernic_op,
			      enum ib_wc_opcode *out_wc_op)
{
	switch (wr_op) {
	case IB_WR_SEND:
		*out_ernic_op = RNIC_OP_SEND;
		*out_wc_op    = IB_WC_SEND;
		return 0;
	case IB_WR_RDMA_WRITE:
		*out_ernic_op = RNIC_OP_WRITE;
		*out_wc_op    = IB_WC_RDMA_WRITE;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* Look up the MR registered against this QP's PD and translate a user
 * VA + length into a DDR4 mirror byte offset.  Returns 0 on success and
 * fills *out_ddr_off; -EINVAL if the range falls outside the MR.  Caller
 * holds no locks; pd->mr_lock is taken internally. */
static int ernic_mr_xlate(struct onic_qp *qp, u32 lkey,
			  u64 user_va, u32 length,
			  u64 *out_ddr_off)
{
	struct onic_pd *pd = qp->pd;
	struct onic_mr *mr;
	u64             off;

	spin_lock(&pd->mr_lock);
	mr = pd->mr;
	if (!mr) {
		spin_unlock(&pd->mr_lock);
		return -EINVAL;
	}
	if (lkey != mr->lkey) {
		spin_unlock(&pd->mr_lock);
		return -EINVAL;
	}
	if (user_va < mr->va || user_va + length > mr->va + mr->length) {
		spin_unlock(&pd->mr_lock);
		return -EINVAL;
	}
	off = mr->ddr_off + (user_va - mr->va);
	spin_unlock(&pd->mr_lock);
	*out_ddr_off = off;
	return 0;
}

/* Compose one SQ WQE in DDR4 and update the SQ shadow. Does NOT ring
 * the doorbell — caller does that once at the end of a chain.
 *
 * Perf #3 (2026-05-03): both SEND and WRITE branches now reference the
 * MR's DDR4 mirror.  The user buffer was copied to DDR4 at reg_user_mr
 * time; here we just compute the byte offset and put it in wqe.laddr.
 * No more per-WQE host→DDR4 staging copy on SEND, no more 64 B cap, and
 * the WRITE branch is no longer broken (it was treating sg_list.addr as
 * a DDR4 offset directly — silently corrupted any real ULP). */
static int ernic_sq_post_one(struct onic_qp *qp, const struct ib_send_wr *wr)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	struct ernic_sq_wqe wqe;
	u32                 ernic_op;
	enum ib_wc_opcode   wc_op;
	u32                 length;
	u32                 lkey;
	u64                 user_va;
	u64                 ddr_off = 0;
	u32                 n_frags;
	u32                 in_flight;
	u32                 frag;
	int                 rv;

	if (wr->num_sge > 1)
		return -EOPNOTSUPP;

	rv = ernic_xlate_opcode(wr->opcode, &ernic_op, &wc_op);
	if (rv) {
		dev_info(&dev->priv->pdev->dev,
			 "onic_ib: post_send unsupported opcode=%d\n",
			 (int)wr->opcode);
		return rv;
	}

	length  = (wr->num_sge == 1) ? wr->sg_list[0].length : 0;
	user_va = (wr->num_sge == 1) ? wr->sg_list[0].addr   : 0;
	lkey    = (wr->num_sge == 1) ? wr->sg_list[0].lkey   : 0;

	/* Resolve user VA → DDR4 mirror offset (Perf #3) once for the whole
	 * chain; per-fragment laddr/remote_offset are derived by adding the
	 * intra-chain byte offset. */
	if (length > 0) {
		rv = ernic_mr_xlate(qp, lkey, user_va, length, &ddr_off);
		if (rv) {
			dev_info(&dev->priv->pdev->dev,
				 "onic_ib: post_send mr xlate failed lkey=%u va=0x%llx len=%u (no MR or out of range)\n",
				 lkey, (unsigned long long)user_va, length);
			return rv;
		}
	}

	/* Fragment-queue cap (project_b7_64kib_write_cap_2026_05_11): WRITE
	 * WQEs larger than ERNIC_MAX_WRITE_FRAG silently wedge the engine.
	 * Only WRITE is fragmented; SEND and READ are out of scope for now
	 * (SEND > 64 KiB has never been tested on this stack; READ has its
	 * own outstanding-request queue per PG332). */
	if (wr->opcode == IB_WR_RDMA_WRITE &&
	    length > ERNIC_MAX_WRITE_FRAG) {
		n_frags = DIV_ROUND_UP(length, ERNIC_MAX_WRITE_FRAG);
	} else {
		n_frags = 1;
	}

	/* Reject if posting n_frags WQEs would overrun the SQ slot ring.
	 * poll_cq advances cq_consumer_idx to track engine completions;
	 * (sq_pidb - cq_consumer_idx) is the number of in-flight WQEs.
	 * We need at least n_frags free slots beyond that. */
	in_flight = qp->sq_pidb - qp->cq_consumer_idx;
	if (in_flight + n_frags > qp->sq_depth) {
		dev_info_ratelimited(&dev->priv->pdev->dev,
				     "onic_ib: post_send qp[%u] SQ full (in_flight=%u n_frags=%u depth=%u)\n",
				     qp->qp_num, in_flight, n_frags,
				     qp->sq_depth);
		return -ENOMEM;
	}

	for (frag = 0; frag < n_frags; frag++) {
		u32      slot       = qp->sq_pidb % qp->sq_depth;
		u32      frag_off   = frag * ERNIC_MAX_WRITE_FRAG;
		u32      frag_len   = (length - frag_off > ERNIC_MAX_WRITE_FRAG)
				    ? ERNIC_MAX_WRITE_FRAG
				    : (length - frag_off);
		u64      laddr_axi  = ((u64)ONIC_DDR4_MSB << 32) |
				      (ddr_off + frag_off);
		bool     is_last    = (frag == n_frags - 1);

		memset(&wqe, 0, sizeof(wqe));
		wqe.wrid       = cpu_to_le16((u16)slot);
		wqe.length     = cpu_to_le32(frag_len);
		wqe.opcode     = cpu_to_le32(ernic_op & 0xff);
		wqe.laddr_low  = cpu_to_le32(lower_32_bits(laddr_axi));
		wqe.laddr_high = cpu_to_le32(upper_32_bits(laddr_axi));

		if (wr->opcode == IB_WR_SEND) {
			/* For SEND, r_key is unused; PD-derived rkey is a
			 * sentinel.  Encode pdn in upper byte too in case
			 * ERNIC inspects it. */
			wqe.r_key = cpu_to_le32((qp->pd->pdn << 8) |
						(qp->pd->pdn & 0xffu));
		} else {
			u64 raddr = rdma_wr(wr)->remote_addr + frag_off;

			/* 2026-05-06 fix: drop the `& 0xffu` mask.  ERNIC IP
			 * uses the upper byte of wire RETH.RKey as MR_INDEX
			 * into the receiver PDT (PG332 v4.2 Figure 9 —
			 * confirmed empirically via tools/ernic-baremetal).
			 * reg_user_mr now sets mr->rkey = (pdn<<8)|pdn so
			 * the full value must propagate. */
			wqe.remote_offset_low  =
				cpu_to_le32(lower_32_bits(raddr));
			wqe.remote_offset_high =
				cpu_to_le32(upper_32_bits(raddr));
			wqe.r_key = cpu_to_le32(rdma_wr(wr)->rkey);
		}

		/* Land the 64 B WQE in DDR4 at qp->sq_ddr_off + slot * 64. */
		rv = onic_ddr4_write(dev->priv,
				     qp->sq_ddr_off +
					(u64)slot * sizeof(wqe),
				     &wqe, sizeof(wqe));
		if (rv) {
			dev_info(&dev->priv->pdev->dev,
				 "onic_ib: post_send sysdma WRITE failed qp=%u slot=%u frag=%u/%u rv=%d\n",
				 qp->qp_num, slot, frag, n_frags, rv);
			return rv;
		}

		/* SQ shadow: only the chain's last slot carries the user's
		 * wr_id; earlier slots are marked is_filler so poll_cq absorbs
		 * their CQEs without delivering ib_wc. */
		if (is_last) {
			qp->sq_shadow[slot].is_filler = false;
			qp->sq_shadow[slot].wr_id     = wr->wr_id;
			qp->sq_shadow[slot].ib_opcode = wc_op;
			qp->sq_shadow[slot].length    = length;
		} else {
			qp->sq_shadow[slot].is_filler = true;
			qp->sq_shadow[slot].wr_id     = 0;
			qp->sq_shadow[slot].ib_opcode = 0;
			qp->sq_shadow[slot].length    = 0;
		}

		qp->sq_pidb++;
	}

	return 0;
}

/* Ring the SQ doorbell after one or more WQEs have landed in DDR4. */
static void ernic_sq_doorbell(struct onic_qp *qp)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;

	wmb();                                  /* WQE bytes visible first */
	iowrite32(qp->sq_pidb, mmio + onic_qcsr(qp, 0x38));
	(void)ioread32(mmio + onic_qcsr(qp, 0x38));
}

static int onic_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			  const struct ib_send_wr **bad_wr)
{
	struct onic_qp     *qp  = to_onic_qp(ibqp);
	struct onic_ib_dev *dev = to_onic_ib_dev(ibqp->device);
	const struct ib_send_wr *cur = wr;
	int posted = 0;
	int rv = 0;

	if (!wr)
		return -EINVAL;

	mutex_lock(&qp->state_lock);
	if (qp->state != ERNIC_QP_RTS) {
		mutex_unlock(&qp->state_lock);
		dev_info_ratelimited(&dev->priv->pdev->dev,
				     "onic_ib: post_send qp[%u] not RTS (state=%d)\n",
				     qp->qp_num, (int)qp->state);
		*bad_wr = wr;
		return -EINVAL;
	}

	dev_info_ratelimited(&dev->priv->pdev->dev,
			     "onic_ib: post_send qp[%u] start chain\n",
			     qp->qp_num);

	while (cur) {
		rv = ernic_sq_post_one(qp, cur);
		if (rv) {
			*bad_wr = cur;
			break;
		}
		posted++;
		cur = cur->next;
	}

	if (posted) {
		onic_dump_qp_state(qp, "post_send pre-doorbell");
		ernic_sq_doorbell(qp);
		/* Give the engine a few microseconds to react before sampling. */
		udelay(50);
		onic_dump_qp_state(qp, "post_send post-doorbell+50us");
	}

	mutex_unlock(&qp->state_lock);
	return rv;
}

static int onic_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			  const struct ib_recv_wr **bad_wr)
{
	struct onic_qp     *qp   = to_onic_qp(ibqp);
	struct onic_ib_dev *dev  = to_onic_ib_dev(ibqp->device);
	void __iomem       *mmio = dev->priv->hw.addr;
	const struct ib_recv_wr *cur = wr;
	int posted = 0;

	pr_info("onic_ib: onic_post_recv ENTER qpn=%u state=%d wr=%p num_sge=%d\n",
		qp->qp_num, (int)qp->state, wr, wr ? wr->num_sge : -1);

	if (!wr)
		return -EINVAL;

	mutex_lock(&qp->state_lock);
	if (qp->state == ERNIC_QP_RESET) {
		mutex_unlock(&qp->state_lock);
		*bad_wr = wr;
		return -EINVAL;
	}

	dev_info_ratelimited(&dev->priv->pdev->dev,
			     "onic_ib: post_recv qp[%u] start chain\n",
			     qp->qp_num);

	while (cur) {
		u32 slot;

		if (cur->num_sge > 1) {
			*bad_wr = cur;
			mutex_unlock(&qp->state_lock);
			return -EOPNOTSUPP;
		}

		slot = qp->rq_pidb % qp->rq_depth;
		qp->rq_shadow[slot].wr_id = cur->wr_id;
		qp->rq_pidb++;
		posted++;
		cur = cur->next;
	}

	if (posted) {
		/* RQCIi (0x34) — write the buffer-credit count visible to
		 * the engine.  Two facts shape this:
		 *
		 * 1. PG332 v4.3 p.13 ("RDMA Queues"): "The Receive Queue work
		 *    requests need not be posted by the application as the
		 *    ERNIC Hardware automatically re-posts consumed receive
		 *    buffers as per the configured receive queue depth."  So
		 *    the engine maintains an internal RQ-slot ring sized by
		 *    QDEPTHi[31:16] and recycles slots automatically; the host
		 *    just needs to tell the engine when slots are "released".
		 *
		 * 2. Empirically (2026-05-11): an incoming SEND with RQCIi=1
		 *    against QDEPTHi=16 fires REQERRBUF syndrome bit 20
		 *    ("access perm fail").  Same packet against RQCIi=64,
		 *    depth=64 (the old unclamped pingpong -r 64 path) is
		 *    accepted.  The engine appears to gate SEND acceptance on
		 *    RQCIi >= QDEPTHi at startup — interpretable as "the RQ
		 *    must be fully credited before any traffic flows".
		 *
		 * Therefore: write `max(rq_pidb, rq_depth)` so the engine
		 * always sees a fully credited RQ even when userspace posted
		 * fewer recv WRs than the spec-floored depth (16).  This is
		 * safe because the ERNIC RX path deposits payload at the
		 * fixed DDR4 address `RQBA + slot * RQE_SIZE` — it never
		 * dereferences a host-side SGE — and our poll_cq still
		 * clamps RECV completion delivery to `rq_pidb` so we don't
		 * surface phantom completions for unposted WRs. */
		u32 rqci = max_t(u32, qp->rq_pidb, qp->rq_depth);
		wmb();
		iowrite32(rqci, mmio + onic_qcsr(qp, 0x34));
		(void)ioread32(mmio + onic_qcsr(qp, 0x34));
	}

	mutex_unlock(&qp->state_lock);
	return 0;
}

/* Reap as many completions as possible from ONE QP into wc[] starting
 * at wc_off.  Returns the number of completions delivered (0..budget).
 *
 * Caller is responsible for the upper-level num_entries budget; this
 * helper takes a budget (= num_entries - already_polled) and bounds
 * its own loops against it.
 *
 * All per-QP state mutations (`cq_consumer_idx`, `rq_consumer_idx`)
 * occur under qp->state_lock — exactly as the pre-multi-QP code did.
 * The CQ-list lock is NOT held here so different CQs / different QPs
 * on the same CQ can poll concurrently from different ULP threads. */
static int onic_poll_one_qp(struct onic_qp *qp, struct onic_ib_dev *dev,
			    int budget, struct ib_wc *wc)
{
	void __iomem *mmio;
	u32           cqhead;
	int           polled = 0;

	if (budget <= 0)
		return 0;
	if (!qp || !qp->ernic_base)
		return 0;       /* QP never advanced past RESET */

	mmio = dev->priv->hw.addr;

	/* DEBUG-perf5: cadence trace — gap between consecutive poll calls
	 * per QP, with the four counters that disambiguate perf #5 (CQ-
	 * doorbell coherency) vs wire-side starvation.  Remove once
	 * perf #5 is closed. */
	{
		static u64 _last_poll_ns;       /* SHARED across all QPs —
						 * good enough for cadence
						 * spotting; per-QP timestamps
						 * would require extra state. */
		u64 _now    = ktime_get_ns();
		u64 _delta  = _last_poll_ns ? (_now - _last_poll_ns) : 0;
		u32 _q      = onic_qcsr(qp, 0x00);
		u32 _gcsr   = qp->ernic_base + 0x00100000u;
		u32 _cqh_mmio = ioread32(mmio + _q + 0x30);
		u32 _cqh_page = qp->hdb_cq ? le32_to_cpu(READ_ONCE(*qp->hdb_cq))
					   : 0xffffffffu;
		_last_poll_ns = _now;
		pr_info("onic_poll_cq qp=%u hdb=%d delta_ns=%llu cqhead_page=%u cqhead_mmio=%u sqpi=%u statcursqptr=%u statssn=%08x outampkt=%u inampkt=%u sw_cq_ci=%u sw_sq_pi=%u budget=%d\n",
			qp->qp_num, qp->hdb_active ? 1 : 0, _delta,
			_cqh_page, _cqh_mmio,
			ioread32(mmio + _q + 0x38),
			ioread32(mmio + _q + 0x8C),
			ioread32(mmio + _q + 0x80),
			ioread32(mmio + _gcsr + 0x10C),
			ioread32(mmio + _gcsr + 0x104),
			qp->cq_consumer_idx, qp->sq_pidb, budget);
	}

	/* Perf #1 (host-coherent CQ doorbell page) is currently disabled
	 * by default (see hdb_active init in onic_create_qp).  Until the
	 * bitstream-side fix lands we always read CQHEADi via MMIO. */
	if (qp->hdb_active) {
		smp_rmb();
		cqhead = le32_to_cpu(READ_ONCE(*qp->hdb_cq));
	} else {
		cqhead = ioread32(mmio + onic_qcsr(qp, 0x30));
	}

	mutex_lock(&qp->state_lock);

	/* SQ-side completions: phantom-completion clamp (variance fix
	 * 2026-05-03) — never deliver more SEND completions than
	 * qp->sq_pidb (count of post_send calls on this QP since last
	 * R->I).
	 *
	 * Fragmented WRITE (project_b7_64kib_write_cap_2026_05_11): a single
	 * user-visible WRITE > 64 KiB is split into N internal WQEs in
	 * ernic_sq_post_one.  The engine produces a CQE per WQE; for the
	 * leading N-1 fragments the SQ shadow has is_filler=true and we
	 * advance cq_consumer_idx without delivering an ib_wc.  Only the
	 * chain's last slot surfaces to the ULP. */
	while (qp->cq_consumer_idx != cqhead &&
	       qp->cq_consumer_idx != qp->sq_pidb) {
		u32 wqe_slot = qp->cq_consumer_idx % qp->sq_depth;

		if (qp->sq_shadow[wqe_slot].is_filler) {
			qp->cq_consumer_idx++;
			continue;
		}
		if (polled >= budget)
			break;

		memset(&wc[polled], 0, sizeof(wc[polled]));
		wc[polled].qp        = &qp->ibqp;
		wc[polled].wr_id     = qp->sq_shadow[wqe_slot].wr_id;
		wc[polled].opcode    = qp->sq_shadow[wqe_slot].ib_opcode;
		wc[polled].byte_len  = qp->sq_shadow[wqe_slot].length;
		wc[polled].status    = IB_WC_SUCCESS;
		qp->cq_consumer_idx++;
		polled++;
	}

	/* RQ-side completions: STATMSNi counts received messages
	 * (advances 0 -> N after N message-receives).  Two clamps:
	 *   - budget cap from caller
	 *   - rq_consumer_idx != rq_pidb (never deliver more RECVs than
	 *     ULP has posted recv buffers for) */
	{
		u32 statmsn = ioread32(mmio + onic_qcsr(qp, 0x84)) &
			      0x00ffffffu;
		u32 rcv_count = statmsn;

		while (polled < budget &&
		       qp->rq_consumer_idx != rcv_count &&
		       qp->rq_consumer_idx != qp->rq_pidb) {
			u32 slot = qp->rq_consumer_idx % qp->rq_depth;

			memset(&wc[polled], 0, sizeof(wc[polled]));
			wc[polled].qp        = &qp->ibqp;
			wc[polled].wr_id     = qp->rq_shadow[slot].wr_id;
			wc[polled].opcode    = IB_WC_RECV;
			wc[polled].status    = IB_WC_SUCCESS;
			wc[polled].byte_len  = 0;
			qp->rq_consumer_idx++;
			polled++;
		}
	}

	mutex_unlock(&qp->state_lock);
	return polled;
}

static int onic_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct onic_cq      *cq   = to_onic_cq(ibcq);
	struct onic_ib_dev  *dev  = to_onic_ib_dev(ibcq->device);
	struct onic_qp      *qps_snap[ONIC_CQ_MAX_QPS];
	unsigned long        flags;
	u32                  n_qps;
	u32                  i;
	int                  polled = 0;

	if (num_entries <= 0)
		return 0;

	/* Snapshot the bound-QP list under cq->lock, then drop the lock
	 * before reaping.  Keeps the lock-held region tiny — concurrent
	 * destroy_qp / create_qp can re-acquire the lock while we walk
	 * QPs that were valid at the snapshot instant.
	 *
	 * Per-QP state mutations during reap happen under qp->state_lock
	 * inside onic_poll_one_qp.  A QP being torn down concurrently is
	 * safe to read: destroy_qp clears QPCONFi[0] and removes the QP
	 * from this list *before* freeing the QP storage (verbs core
	 * holds a refcount through the destroy callback). */
	spin_lock_irqsave(&cq->lock, flags);
	n_qps = cq->num_bound_qps;
	for (i = 0; i < n_qps; i++)
		qps_snap[i] = cq->bound_qps[i];
	spin_unlock_irqrestore(&cq->lock, flags);

	if (n_qps == 0)
		return 0;

	/* Round-robin reap.  Each pass takes (num_entries - polled) as the
	 * per-QP budget, so a busy QP cannot monopolize a small num_entries
	 * caller (e.g. perftest --cq-mod=N polls in groups of N). */
	for (i = 0; i < n_qps && polled < num_entries; i++) {
		int got = onic_poll_one_qp(qps_snap[i], dev,
					   num_entries - polled,
					   &wc[polled]);
		if (got > 0)
			polled += got;
	}

	return polled;
}

/* Polling-mode req_notify_cq: arming an IRQ-driven completion is B8
 * work.  Returning 0 means "no missed events" — ULPs that explicitly
 * want IRQ-mode wakeups will fall back to polling, which is what we
 * want for now. */
static int onic_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags f)
{
	(void)cq;
	(void)f;
	return 0;
}
static int onic_stub_create_ah(struct ib_ah *ah, struct rdma_ah_init_attr *a,
			       struct ib_udata *u)               { STUB_BODY("create_ah"); }
static int onic_stub_destroy_ah(struct ib_ah *ah, u32 flags)     { STUB_BODY("destroy_ah"); }
static struct ib_mr *onic_stub_alloc_mr(struct ib_pd *pd, enum ib_mr_type t, u32 n)
{ pr_info_ratelimited("onic_ib: b5_stub: alloc_mr -> -EOPNOTSUPP\n");
  return ERR_PTR(-EOPNOTSUPP); }
static int onic_stub_map_mr_sg(struct ib_mr *mr, struct scatterlist *sg, int n,
			       unsigned int *off)                { STUB_BODY("map_mr_sg"); }

/* ----- ops table --------------------------------------------------------- */

static const struct ib_device_ops onic_ib_ops = {
	.owner                       = THIS_MODULE,
	.driver_id                   = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver              = 1,
	.uverbs_no_driver_id_binding = 1,

	.query_device                = onic_query_device,
	.query_port                  = onic_query_port,
	.get_port_immutable          = onic_get_port_immutable,
	.get_link_layer              = onic_get_link_layer,
	.query_pkey                  = onic_query_pkey,
	.query_gid                   = onic_query_gid,
	.get_netdev                  = onic_get_netdev,
	.get_dev_fw_str              = onic_get_dev_fw_str,

	.alloc_ucontext              = onic_alloc_ucontext,
	.dealloc_ucontext            = onic_dealloc_ucontext,
	.alloc_pd                    = onic_alloc_pd,
	.dealloc_pd                  = onic_dealloc_pd,

	.create_cq                   = onic_create_cq,
	.destroy_cq                  = onic_destroy_cq,
	.create_qp                   = onic_create_qp,
	.destroy_qp                  = onic_destroy_qp,
	.reg_user_mr                 = onic_reg_user_mr,
	.dereg_mr                    = onic_dereg_mr,

	.modify_qp                   = onic_modify_qp,
	.query_qp                    = onic_query_qp,
	.post_send                   = onic_post_send,
	.post_recv                   = onic_post_recv,
	.poll_cq                     = onic_poll_cq,
	.req_notify_cq               = onic_req_notify_cq,
	.create_ah                   = onic_stub_create_ah,
	.destroy_ah                  = onic_stub_destroy_ah,
	.alloc_mr                    = onic_stub_alloc_mr,
	.map_mr_sg                   = onic_stub_map_mr_sg,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, onic_ucontext, ibucontext),
	INIT_RDMA_OBJ_SIZE(ib_pd,       onic_pd,       ibpd),
	INIT_RDMA_OBJ_SIZE(ib_cq,       onic_cq,       ibcq),
	INIT_RDMA_OBJ_SIZE(ib_qp,       onic_qp,       ibqp),
};

/* ----- register / unregister -------------------------------------------- */

int onic_ib_register(struct onic_private *priv)
{
	struct onic_ib_dev *dev;
	char  name[IB_DEVICE_NAME_MAX];
	int   rv;

	/* B7 — compile-time guards on packed ERNIC wire structs. */
	BUILD_BUG_ON(sizeof(struct ernic_sq_wqe) != 64);
	BUILD_BUG_ON(sizeof(struct ernic_cqe)    != 4);

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return 0;

	dev = (struct onic_ib_dev *)ib_alloc_device(onic_ib_dev, ibdev);
	if (!dev)
		return -ENOMEM;

	dev->priv = priv;
	spin_lock_init(&dev->pd_lock);
	bitmap_zero(dev->pd_bitmap, ONIC_IB_MAX_PD);
	/* Reserve PD 0 so first user PD gets pdn=1.  The MR keys are derived
	 * from pdn (mr->lkey = mr->rkey = pdn) and many libibverbs paths treat
	 * lkey=0 as "invalid local key" — post_recv would silently fail in
	 * userspace before reaching the kernel.  The PDT row 0 is therefore
	 * unused; rows 1..ONIC_IB_MAX_PD-1 carry real registrations. */
	set_bit(0, dev->pd_bitmap);
	onic_ddr_pool_init(&dev->ddr, 0 /* ERNIC0 */);

	/* Perf #1 — coherent doorbell page for ERNIC CQ/RQ producer-index
	 * DMA writes.  4 KiB total: cq_pidb[256]@0..0x3FF, rq_pidb[256]@
	 * 0x400..0x7FF (4 B per QP, qp_num indexed).  Failure here is non-
	 * fatal: every QP falls back to DDR4-tagged doorbells. */
	dev->hdb.size    = PAGE_SIZE;
	dev->hdb.enabled = false;
	if (host_doorbell) {
		dev->hdb.vaddr = dma_alloc_coherent(&priv->pdev->dev,
						    dev->hdb.size,
						    &dev->hdb.dma,
						    GFP_KERNEL);
		if (dev->hdb.vaddr) {
			memset(dev->hdb.vaddr, 0, dev->hdb.size);
			dev->hdb.enabled = true;
			dev_info(&priv->pdev->dev,
				 "host_doorbell: coherent page va=%px dma=%pad size=%zu\n",
				 dev->hdb.vaddr, &dev->hdb.dma, dev->hdb.size);
		} else {
			dev_warn(&priv->pdev->dev,
				 "host_doorbell alloc failed; using DDR4-tagged doorbells\n");
		}
	}

	{
		const u8 *mac = priv->netdev->dev_addr;
		u8 guid[8] = { mac[0] | 0x02, mac[1], mac[2],
			       0xFF, 0xFE, mac[3], mac[4], mac[5] };
		memcpy(&dev->node_guid, guid, 8);
	}
	memcpy(&dev->ibdev.node_guid, &dev->node_guid, sizeof(__be64));
	dev->ibdev.node_type        = RDMA_NODE_IB_CA;
	dev->ibdev.phys_port_cnt    = 2;
	dev->ibdev.num_comp_vectors = 1;
	dev->ibdev.dev.parent       = &priv->pdev->dev;

	/* MLNX-OFED's ib_uverbs legacy write() path (used by ibv_cmd_post_recv
	 * etc.) gates verb dispatch on this mask before checking the ops table.
	 * Without these bits, post_recv/post_send/poll_cq return -EOPNOTSUPP at
	 * the ib_uverbs layer without ever reaching our handler. */
	dev->ibdev.uverbs_cmd_mask =
		BIT_ULL(IB_USER_VERBS_CMD_GET_CONTEXT)        |
		BIT_ULL(IB_USER_VERBS_CMD_QUERY_DEVICE)       |
		BIT_ULL(IB_USER_VERBS_CMD_QUERY_PORT)         |
		BIT_ULL(IB_USER_VERBS_CMD_ALLOC_PD)           |
		BIT_ULL(IB_USER_VERBS_CMD_DEALLOC_PD)         |
		BIT_ULL(IB_USER_VERBS_CMD_REG_MR)             |
		BIT_ULL(IB_USER_VERBS_CMD_DEREG_MR)           |
		BIT_ULL(IB_USER_VERBS_CMD_CREATE_CQ)          |
		BIT_ULL(IB_USER_VERBS_CMD_DESTROY_CQ)         |
		BIT_ULL(IB_USER_VERBS_CMD_CREATE_QP)          |
		BIT_ULL(IB_USER_VERBS_CMD_MODIFY_QP)          |
		BIT_ULL(IB_USER_VERBS_CMD_QUERY_QP)           |
		BIT_ULL(IB_USER_VERBS_CMD_DESTROY_QP)         |
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND)          |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV)          |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ)            |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	ib_set_device_ops(&dev->ibdev, &onic_ib_ops);
	rcu_assign_pointer(dev->port[0].netdev, priv->netdev);

	snprintf(name, sizeof(name), "onic_%02x%02x",
		 priv->pdev->bus->number, priv->pdev->devfn);

	rv = ib_register_device(&dev->ibdev, name, &priv->pdev->dev);
	if (rv) {
		dev_err(&priv->pdev->dev, "ib_register_device(%s) err=%d\n", name, rv);
		ib_dealloc_device(&dev->ibdev);
		return rv;
	}

	rv = ib_device_set_netdev(&dev->ibdev, priv->netdev, 1);
	if (rv) {
		dev_err(&priv->pdev->dev, "ib_device_set_netdev p1 err=%d\n", rv);
		ib_unregister_device(&dev->ibdev);
		ib_dealloc_device(&dev->ibdev);
		return rv;
	}

	/* Bring ERNIC0 (port 1) out of reset.  ERNIC1 (port 2) is brought up
	 * later in onic_ib_set_port2_netdev once the secondary netdev is
	 * available. */
	onic_ernic_global_init(dev, 1);

	priv->ib_dev = dev;

	/* Per-ib_device debugfs root: /sys/kernel/debug/onic/<ibdev_name>/.
	 * Per-QP qp<N>/dump entries hang off this and are populated/removed
	 * by onic_create_qp / onic_destroy_qp. */
	onic_debugfs_register(dev);

	dev_info(&priv->pdev->dev, "ib_device '%s' registered (2 ports, RoCEv2)\n",
		 name);
	return 0;
}

int onic_ib_set_port2_netdev(struct onic_private *primary,
			     struct onic_private *secondary)
{
	struct onic_ib_dev *dev = primary ? primary->ib_dev : NULL;
	int rv;

	if (!dev || !secondary || !secondary->netdev)
		return 0;
	rcu_assign_pointer(dev->port[1].netdev, secondary->netdev);
	rv = ib_device_set_netdev(&dev->ibdev, secondary->netdev, 2);
	if (rv)
		return rv;

	/* Bring ERNIC1 (port 2) out of reset now that we have its netdev. */
	onic_ernic_global_init(dev, 2);
	return 0;
}

void onic_ib_unregister(struct onic_private *priv)
{
	struct onic_ib_dev *dev = priv ? priv->ib_dev : NULL;

	if (!dev)
		return;

	/* Tear debugfs down before ib_unregister_device so no `cat dump` can
	 * fire after the underlying ib_device is gone.  Per-QP entries get
	 * pruned recursively as part of debugfs_root removal. */
	onic_debugfs_unregister(dev);

	ib_unregister_device(&dev->ibdev);
	onic_ddr_pool_fini(&dev->ddr);
	if (dev->hdb.vaddr) {
		dma_free_coherent(&priv->pdev->dev, dev->hdb.size,
				  dev->hdb.vaddr, dev->hdb.dma);
		dev->hdb.vaddr   = NULL;
		dev->hdb.enabled = false;
	}
	ib_dealloc_device(&dev->ibdev);
	priv->ib_dev = NULL;
}
