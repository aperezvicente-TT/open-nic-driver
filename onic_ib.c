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
#include <rdma/ib_verbs.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_cache.h>

#include "onic.h"
#include "onic_ib.h"

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
	attr->max_qp_wr           = 1024;
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
	attr->page_size_cap       = PAGE_SIZE;
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
	return 0;
}

static int onic_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct onic_ib_dev *dev = to_onic_ib_dev(ibpd->device);
	struct onic_pd     *pd  = to_onic_pd(ibpd);

	spin_lock(&dev->pd_lock);
	clear_bit(pd->pdn, dev->pd_bitmap);
	spin_unlock(&dev->pd_lock);
	return 0;
}

/* ----- stubs — land real implementations in B5/B6/B7/B8 ------------------ */

#define STUB_BODY(name) \
	pr_info_ratelimited("onic_ib: b3_stub: " name " -> -EOPNOTSUPP\n"); \
	return -EOPNOTSUPP

/* OFED 26.01 signature: create_cq takes uverbs_attr_bundle instead of ib_udata */
static int onic_stub_create_cq(struct ib_cq *cq, const struct ib_cq_init_attr *a,
			       struct uverbs_attr_bundle *attrs) { STUB_BODY("create_cq"); }
static int onic_stub_destroy_cq(struct ib_cq *cq, struct ib_udata *u)
								  { STUB_BODY("destroy_cq"); }
static int onic_stub_create_qp(struct ib_qp *qp, struct ib_qp_init_attr *a,
			       struct ib_udata *u)               { STUB_BODY("create_qp"); }
static int onic_stub_modify_qp(struct ib_qp *qp, struct ib_qp_attr *a, int mask,
			       struct ib_udata *u)               { STUB_BODY("modify_qp"); }
static int onic_stub_query_qp(struct ib_qp *qp, struct ib_qp_attr *a, int mask,
			      struct ib_qp_init_attr *ia)        { STUB_BODY("query_qp"); }
static int onic_stub_destroy_qp(struct ib_qp *qp, struct ib_udata *u)
								  { STUB_BODY("destroy_qp"); }
static int onic_stub_post_send(struct ib_qp *qp, const struct ib_send_wr *w,
			       const struct ib_send_wr **bad)    { STUB_BODY("post_send"); }
static int onic_stub_post_recv(struct ib_qp *qp, const struct ib_recv_wr *w,
			       const struct ib_recv_wr **bad)    { STUB_BODY("post_recv"); }
static int onic_stub_poll_cq(struct ib_cq *cq, int n, struct ib_wc *wc)
								  { STUB_BODY("poll_cq"); }
static int onic_stub_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags f)
								  { STUB_BODY("req_notify_cq"); }
static int onic_stub_create_ah(struct ib_ah *ah, struct rdma_ah_init_attr *a,
			       struct ib_udata *u)               { STUB_BODY("create_ah"); }
static int onic_stub_destroy_ah(struct ib_ah *ah, u32 flags)     { STUB_BODY("destroy_ah"); }
/* OFED 26.01 signature: reg_user_mr has an extra struct ib_dmah * param */
static struct ib_mr *onic_stub_reg_user_mr(struct ib_pd *pd, u64 s, u64 l,
					   u64 va, int acc,
					   struct ib_dmah *dmah,
					   struct ib_udata *u)
{ pr_info_ratelimited("onic_ib: b3_stub: reg_user_mr -> -EOPNOTSUPP\n");
  return ERR_PTR(-EOPNOTSUPP); }
static int onic_stub_dereg_mr(struct ib_mr *mr, struct ib_udata *u)
								  { STUB_BODY("dereg_mr"); }
static struct ib_mr *onic_stub_alloc_mr(struct ib_pd *pd, enum ib_mr_type t, u32 n)
{ pr_info_ratelimited("onic_ib: b3_stub: alloc_mr -> -EOPNOTSUPP\n");
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

	.create_cq                   = onic_stub_create_cq,
	.destroy_cq                  = onic_stub_destroy_cq,
	.create_qp                   = onic_stub_create_qp,
	.modify_qp                   = onic_stub_modify_qp,
	.query_qp                    = onic_stub_query_qp,
	.destroy_qp                  = onic_stub_destroy_qp,
	.post_send                   = onic_stub_post_send,
	.post_recv                   = onic_stub_post_recv,
	.poll_cq                     = onic_stub_poll_cq,
	.req_notify_cq               = onic_stub_req_notify_cq,
	.create_ah                   = onic_stub_create_ah,
	.destroy_ah                  = onic_stub_destroy_ah,
	.reg_user_mr                 = onic_stub_reg_user_mr,
	.dereg_mr                    = onic_stub_dereg_mr,
	.alloc_mr                    = onic_stub_alloc_mr,
	.map_mr_sg                   = onic_stub_map_mr_sg,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, onic_ucontext, ibucontext),
	INIT_RDMA_OBJ_SIZE(ib_pd,       onic_pd,       ibpd),
};

/* ----- register / unregister -------------------------------------------- */

int onic_ib_register(struct onic_private *priv)
{
	struct onic_ib_dev *dev;
	char  name[IB_DEVICE_NAME_MAX];
	int   rv;

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return 0;

	dev = (struct onic_ib_dev *)ib_alloc_device(onic_ib_dev, ibdev);
	if (!dev)
		return -ENOMEM;

	dev->priv = priv;
	spin_lock_init(&dev->pd_lock);
	bitmap_zero(dev->pd_bitmap, ONIC_IB_MAX_PD);

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

	priv->ib_dev = dev;
	dev_info(&priv->pdev->dev, "ib_device '%s' registered (2 ports, RoCEv2)\n",
		 name);
	return 0;
}

int onic_ib_set_port2_netdev(struct onic_private *primary,
			     struct onic_private *secondary)
{
	struct onic_ib_dev *dev = primary ? primary->ib_dev : NULL;

	if (!dev || !secondary || !secondary->netdev)
		return 0;
	rcu_assign_pointer(dev->port[1].netdev, secondary->netdev);
	return ib_device_set_netdev(&dev->ibdev, secondary->netdev, 2);
}

void onic_ib_unregister(struct onic_private *priv)
{
	struct onic_ib_dev *dev = priv ? priv->ib_dev : NULL;

	if (!dev)
		return;
	ib_unregister_device(&dev->ibdev);
	ib_dealloc_device(&dev->ibdev);
	priv->ib_dev = NULL;
}
