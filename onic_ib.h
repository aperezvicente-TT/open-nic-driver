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
#include <rdma/ib_verbs.h>
#include "onic_ddr_alloc.h"

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

/* B7 — RQBUFSZ field value for 512-B RQE slots (libreconic RQE_SIZE = 512).
 * QPCONFi[31:16] is RQE size in 256-B units, so 512 / 256 = 2.
 * The earlier B5 code packed `rq_depth` here by mistake. */
#define ERNIC_RQE_SIZE_FIELD 2u

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
	u64              va;            /* user VA (advisory) */
	u64              ddr_off;       /* DDR4 byte offset (from allocator) */
	u64              length;
	u8               access;        /* ERNIC ACCESSDESC[1:0] */
	u32              lkey;          /* == rkey == pdn (Track B simplification) */
	u32              rkey;
};

struct onic_qp;

struct onic_cq {
	struct ib_cq     ibcq;
	u32              cq_id;         /* == bound QP index once bound */
	u64              ddr_off;       /* DDR4 byte offset of CQ ring */
	u32              depth;
	u32              head;          /* driver-side tail-tracker (for B8) */
	u32              tail;
	bool             bound;         /* false until create_qp binds it */
	spinlock_t       lock;

	/* B7 — back-pointer to the QP bound to this CQ. Populated by
	 * create_qp; cleared by destroy_qp. ERNIC v4.2 has a 1:1 mapping
	 * between QP and CQ, so this is unambiguous. Used by poll_cq to
	 * find the QCSR doorbell, sq_shadow and DDR4 CQ ring offset. */
	struct onic_qp  *qp_back;
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

	spinlock_t              state_lock;

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

#endif /* __ONIC_IB_H__ */
