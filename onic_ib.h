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

struct onic_cq {
	struct ib_cq     ibcq;
	u32              cq_id;         /* == bound QP index once bound */
	u64              ddr_off;       /* DDR4 byte offset of CQ ring */
	u32              depth;
	u32              head;          /* driver-side tail-tracker (for B8) */
	u32              tail;
	bool             bound;         /* false until create_qp binds it */
	spinlock_t       lock;
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
