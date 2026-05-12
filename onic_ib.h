/*
 * Copyright (c) 2026 Tenstorrent Inc.
 *
 * onic_ib.h — B3 skeleton for the ib_device carried by master PF.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef __ONIC_IB_H__
#define __ONIC_IB_H__

#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/kconfig.h>
#include <rdma/ib_verbs.h>
#include "onic_ddr_alloc.h"

#if IS_ENABLED(CONFIG_DEBUG_FS)
struct dentry;
#endif

struct onic_private;

/* Kernel-side copy of the QP state machine from
 * libreconic/ernic_lifecycle.h — that header is userspace-facing
 * (includes <stdint.h>) so we just redeclare the enum here. */
enum ernic_qp_state {
	ERNIC_QP_RESET = 0,
	ERNIC_QP_INIT,
	ERNIC_QP_RTR,
	ERNIC_QP_RTS,
	ERNIC_QP_ERR,
	ERNIC_QP_UNDER_RECOVERY,
	ERNIC_QP_CLOSED,
};

#define ONIC_IB_MAX_PD 256

/* QPCONFi[31:16] = RQ Buffer size in MULTIPLES OF 256B, per Xilinx
 * PG332 v4.2 Table 8 (line ~2715): "RQ Buffer size (in multiple of
 * 256B)... programmed value should be the power of 2 for expected
 * behavior".  Value 16 makes each RQE slot 4096 B, matching the
 * single-packet PMTU=4096 RoCE flow.
 *
 * History:
 *   1  -> 256 B stride (2026-04: caused driver/ERNIC slot-offset
 *         disagreement).
 *   2  -> 512 B (2026-05-03 fix: -s ≤512 worked, larger SENDs failed
 *         with FATAL_CODE 0x02 due to per-slot overflow).
 *   16 -> 4096 B (2026-05-10 fix: covers full PMTU 4096 single-packet
 *         SENDs; combined with the 64-KiB RQ region from
 *         ONIC_DDR_QUEUE_SLOT_SIZE=128 KiB this lets rq_depth go to
 *         16 — the spec-minimum-tested queue depth, PG332 v4.3 p.71). */
#define ERNIC_RQE_SIZE_FIELD 16u

/* Derived sizing — computed from the slot layout in onic_ddr_alloc.h
 * so a future slot-size change re-derives the depth caps automatically.
 *
 * ERNIC_MAX_RQ_DEPTH is the upper bound on the per-QP RQ depth that
 * still fits inside the slot's RQ region without overlapping the CQ
 * region.  onic_create_qp clamps userspace's requested max_recv_wr
 * against this *and* against the PG332 minimum tested depth of 16. */
#define ERNIC_RQE_BUFFER_BYTES   (ERNIC_RQE_SIZE_FIELD * 256u)
#define ERNIC_MAX_RQ_DEPTH       (_ONIC_DDR_RQ_SIZE / ERNIC_RQE_BUFFER_BYTES)

/* SQ depth ceiling: SQ region holds N × 64-B WQEs (struct ernic_sq_wqe
 * below).  Using a literal because the struct isn't visible yet here. */
#define ERNIC_MAX_SQ_DEPTH       (_ONIC_DDR_SQ_SIZE / 64u)

/* PG332 v4.3 p.71 ("RC QP Creation"): "The minimum tested depth of the
 * queues is 16."  We hard-floor user-requested depths to this so the
 * IP is never operated below its characterized range. */
#define ERNIC_MIN_QUEUE_DEPTH    16u

/* Max bytes per single ERNIC WQE.  Empirically pinned 2026-05-11:
 * a WRITE with length > 16 × PMTU silently wedges the TX engine
 * (statcursqptr stays at 0; PCIe Bus Reset required to recover).
 * PG332 v4.3 §"Unsupported Features" says the architectural ceiling is
 * 8 MB, so this is a fragment-queue parameter baked into our bitstream
 * (PMTU 4096 × 16 entries = 64 KiB).  See
 * project_b7_64kib_write_cap_2026_05_11.md.
 *
 * Driver fragments any WRITE > this into N consecutive WQEs and
 * presents one ULP-visible CQE for the whole chain. */
#define ERNIC_MAX_WRITE_FRAG     (16u * 4096u)

/* B7 — packed ERNIC SQ WQE, 64 bytes.  Layout per
 * reference_ernic_wqe_spec.md §2 / libreconic/rdma_api.h:138-158. */
struct ernic_sq_wqe {
	__le16  wrid;                  /* 0x00 — echoed in CQE.wqe_idx */
	__le16  reserved;              /* 0x02 */
	__le32  laddr_low;             /* 0x04 */
	__le32  laddr_high;            /* 0x08 */
	__le32  length;                /* 0x0C */
	__le32  opcode;                /* 0x10 — only [7:0] honoured */
	__le32  remote_offset_low;     /* 0x14 — WRITE/READ only */
	__le32  remote_offset_high;    /* 0x18 — WRITE/READ only */
	__le32  r_key;                 /* 0x1C — only [7:0] honoured */
	u8      send_small_payload[16];/* 0x20 — inline SEND data ≤16 B */
	__le32  immdt_data;            /* 0x30 — *_IMMDT only */
	__le32  reserved0;             /* 0x34 */
	__le32  reserved1;             /* 0x38 */
	__le32  reserved2;             /* 0x3C */
} __packed;

/* B7 — packed ERNIC CQE, 4 bytes.  Layout per spec §4 / PG332 Table 10. */
struct ernic_cqe {
	__le16  wqe_idx;               /* 0x00 — echo of SQ slot completed */
	u8      opcode_echo;           /* 0x02 */
	u8      status;                /* 0x03 — 0=ok */
} __packed;

/* B7 — driver-side shadow tables. */
struct ernic_sq_shadow {
	u64                  wr_id;
	enum ib_wc_opcode    ib_opcode;
	u32                  length;
	/* is_filler: this slot belongs to a fragmented WRITE chain but is
	 * not the chain's last fragment.  poll_cq must absorb the engine's
	 * CQE for it (advance cq_consumer_idx) without delivering an
	 * ib_wc.  Only the chain's last slot carries the user-visible
	 * wr_id / ib_opcode / total length. */
	bool                 is_filler;
};

struct ernic_rq_shadow {
	u64                  wr_id;
};

struct onic_ib_dev {
	struct ib_device      ibdev;    /* MUST be first — to_onic_ib_dev casts */
	struct onic_private  *priv;
	__be64                node_guid;
	struct {
		struct net_device __rcu *netdev;
	} port[2];
	DECLARE_BITMAP(pd_bitmap, ONIC_IB_MAX_PD);
	spinlock_t            pd_lock;

	/* B5: DDR4 slab allocator — QP queue slots + MR staging. */
	struct onic_ddr_pool  ddr;

	/* Perf #1 — coherent host page for ERNIC doorbell DMA writes.
	 * Replaces the DDR4-tagged RQWPTRDBADDi/CQDBADDi targets when
	 * the host_doorbell module param is true.  4 KiB layout:
	 *   0x000..0x3FF — cq_pidb[256] (4 B per QP)
	 *   0x400..0x7FF — rq_pidb[256] (4 B per QP)
	 * onic_poll_cq reads cq_pidb[qp->qp_num] instead of MMIO CQHEADi,
	 * saving one PCIe read per poll. */
	struct {
		void           *vaddr;
		dma_addr_t      dma;
		size_t          size;
		bool            enabled;
	} hdb;

#if IS_ENABLED(CONFIG_DEBUG_FS)
	/* Per-ib_device debugfs root: /sys/kernel/debug/onic/<ibdev_name>/.
	 * Created in onic_ib_register after ib_register_device, removed in
	 * onic_ib_unregister.  Per-QP qp<N>/ subdirs hang off this. */
	struct dentry         *debugfs_root;
#endif
};

/* B3 PD struct, extended in B5 to track its single bound MR. */
struct onic_mr;
struct onic_pd {
	struct ib_pd     ibpd;
	u32              pdn;           /* PDT row number */
	struct onic_mr  *mr;            /* single bound MR, NULL if none */
	spinlock_t       mr_lock;       /* guards ->mr */
};

struct onic_mr {
	struct ib_mr     ibmr;
	struct onic_pd  *pd;            /* back-pointer */
	u64              va;            /* user VA (base of the MR's host range) */
	u64              ddr_off;       /* DDR4 byte offset (from allocator) */
	u64              length;
	u8               access;        /* ERNIC ACCESSDESC[1:0] */
	u32              lkey;          /* == rkey == pdn (Track B simplification) */
	u32              rkey;
	/* Perf #3 — DDR4 mirror is populated at reg_user_mr time via
	 * copy_from_user → onic_ddr4_write, in 1 MiB chunks.  post_send /
	 * post_write reference the mirror via wqe.laddr = ddr_off +
	 * (sg_addr - va).  Snapshot semantics: post-registration mutations
	 * to the host buffer don't propagate to DDR4 — matches IB undefined-
	 * behaviour for post-reg writes. */
};

struct onic_qp;

/* B7 originally enforced a 1:1 QP↔CQ binding (single back-pointer).
 * That broke any multi-QP workload — perftest -q 2..N uses ONE CQ
 * shared across all -q QPs, and ConnectX-style throughput tests need
 * the same.  2026-05-11: lifted to N:1 (multiple QPs per CQ).
 *
 * The cap matches ERNIC's `XRNIC_CONF_QP_EN` bitmap width (6 in this
 * IP build, per `project_b7_qp_en_bitmap_2026_05_05`).  Sized to
 * ONIC_QP_MAX so any future bitmap widening is automatic. */
#define ONIC_CQ_MAX_QPS  ONIC_QP_MAX

struct onic_cq {
	struct ib_cq     ibcq;
	u32              cq_id;         /* informational; first-bound QP idx */
	u64              ddr_off;       /* informational; first-bound QP's CQ DDR4 off */
	u32              depth;
	u32              head;          /* driver-side tail-tracker (for B8) */
	u32              tail;
	spinlock_t       lock;          /* guards bound_qps + num_bound_qps */

	/* Back-pointers to every QP whose send_cq (== recv_cq, ERNIC's 1:1
	 * SQ↔RQ↔CQ per-QP rule) is this CQ.  poll_cq iterates this list and
	 * reaps each QP's CQHEADi / STATMSN independently.  Order is
	 * insertion-order (FIFO); removal compacts.
	 *
	 * Guarded by lock above. */
	struct onic_qp  *bound_qps[ONIC_CQ_MAX_QPS];
	u32              num_bound_qps;
};

struct onic_qp {
	struct ib_qp            ibqp;
	enum ernic_qp_state     state;  /* from ernic_lifecycle.h */
	u32                     qp_num; /* ERNIC QPi / DDR4 queue-slot index */
	struct onic_pd         *pd;
	struct onic_cq         *send_cq, *recv_cq;

	u64                     slot_off;  /* DDR4 offset of the 16 KiB slot */
	u32                     sq_depth, rq_depth, cq_depth;
	u8                      path_mtu;  /* QPCONFi[10:8] code */

	/* B6: driver-stored attrs from modify_qp, used by query_qp and
	 * future B9 recovery. */
	u32                     dest_qp_num;
	u32                     rq_psn;
	u32                     sq_psn;
	u8                      timeout;
	u8                      retry_cnt;
	u8                      rnr_retry;
	u8                      min_rnr_timer;
	u8                      path_mtu_ib;  /* IB_MTU_* enum */
	u8                      port_num;
	u8                      dmac[6];
	union ib_gid            dgid;

	struct mutex            state_lock;	/* held across sysdma DDR4 r/w (sleeping); was spinlock — caused "scheduling while atomic" in post_send/poll_cq */

	/* B7 — DDR4 byte offsets of the per-QP rings.  Final values are
	 * resolved at RESET->INIT once port_num (and therefore the ERNIC
	 * binding) is known: ddr_off = (port-1)*0x2_0000_0000 + slot_off.
	 * post_send / poll_cq use these to stage WQEs and read CQEs via
	 * onic_ddr4_{write,read}. */
	u64                     sq_ddr_off;
	u64                     rq_ddr_off;
	u64                     cq_ddr_off;

	/* Dual-ERNIC plumbing — set at RESET->INIT once port_num is known.
	 *   ernic_base = RN_RDMA_BASE_ADDRESS   (0x800000) for port 1
	 *   ernic_base = RN_RDMA_1_BASE_ADDRESS (0xA00000) for port 2
	 * Used as the QCSR window base for every per-QP register access on
	 * the verb path (post_send doorbell, post_recv doorbell, poll_cq
	 * CQHEAD read, modify_qp QCSR writes, destroy_qp QCSR teardown).
	 * Zero before RESET->INIT — guard accordingly. */
	u32                     ernic_base;

	/* B7 — driver-side shadow rings, allocated in create_qp,
	 * freed in destroy_qp.  Indexed by (pidx % depth) for SQ/RQ. */
	struct ernic_sq_shadow *sq_shadow;
	struct ernic_rq_shadow *rq_shadow;

	/* B7 — monotonic 32-bit producer/consumer counters.  Engine sees
	 * them via QCSR doorbells; host computes slot = pidx % depth. */
	u32                     sq_pidb;
	u32                     rq_pidb;
	u32                     cq_consumer_idx;
	/* RQ consumer index — tracks how many RQEs we've reaped via
	 * poll_cq.  Engine-side producer index lives in STATRQPIDBi
	 * (QCSR 0x9C); when STATRQPIDBi > rq_consumer_idx, there are
	 * IB_WC_RECV completions to deliver. */
	u32                     rq_consumer_idx;

	/* Perf #1 — slots inside the ib_dev's coherent doorbell page.
	 * hdb_cq points at cq_pidb[qp_num], hdb_rq at rq_pidb[qp_num].
	 * hdb_*_dma are the bus addresses that go into CQDBADDi /
	 * RQWPTRDBADDi when hdb_active is true.  When hdb_active is
	 * false the QP falls back to MMIO/DDR4-tagged doorbells. */
	volatile __le32        *hdb_cq;
	volatile __le32        *hdb_rq;
	dma_addr_t              hdb_cq_dma;
	dma_addr_t              hdb_rq_dma;
	bool                    hdb_active;

#if IS_ENABLED(CONFIG_DEBUG_FS)
	/* Per-QP debugfs subdir: <ibdev>/qp<qp_num>/.  Created at the end of
	 * onic_create_qp once qp_num is assigned; removed at the start of
	 * onic_destroy_qp before any teardown so the read handler can never
	 * race a free.  debugfs_remove_recursive is synchronous w.r.t. open
	 * file handles so destroy_qp blocks until any in-flight `cat dump`
	 * returns. */
	struct dentry          *debugfs_dentry;
#endif
};

struct onic_ucontext {
	struct ib_ucontext ibucontext;
};

static inline struct onic_ib_dev *to_onic_ib_dev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct onic_ib_dev, ibdev);
}

static inline struct onic_pd *to_onic_pd(struct ib_pd *pd)
{
	return container_of(pd, struct onic_pd, ibpd);
}

static inline struct onic_mr *to_onic_mr(struct ib_mr *mr)
{
	return container_of(mr, struct onic_mr, ibmr);
}

static inline struct onic_cq *to_onic_cq(struct ib_cq *cq)
{
	return container_of(cq, struct onic_cq, ibcq);
}

static inline struct onic_qp *to_onic_qp(struct ib_qp *qp)
{
	return container_of(qp, struct onic_qp, ibqp);
}

int  onic_ib_register(struct onic_private *priv);
void onic_ib_unregister(struct onic_private *priv);
int  onic_ib_set_port2_netdev(struct onic_private *primary,
			      struct onic_private *secondary);

/* Exposed for onic_debugfs.c — the on-demand QP-state dump handler.
 * Same signature/implementation as the existing in-driver call sites
 * around modify_qp transitions and post_send. */
void onic_dump_qp_state(const struct onic_qp *qp, const char *tag);

#endif /* __ONIC_IB_H__ */
