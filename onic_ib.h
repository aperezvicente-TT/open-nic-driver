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

struct onic_private;

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
};

struct onic_pd {
	struct ib_pd ibpd;
	u32          pdn;
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

int  onic_ib_register(struct onic_private *priv);
void onic_ib_unregister(struct onic_private *priv);
int  onic_ib_set_port2_netdev(struct onic_private *primary,
			      struct onic_private *secondary);

#endif /* __ONIC_IB_H__ */
