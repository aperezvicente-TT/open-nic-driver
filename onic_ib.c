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
#include <rdma/ib_verbs.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_cache.h>

#include "onic.h"
#include "onic_ib.h"
#include "../libreconic/reconic_reg.h"

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

	(void)start;
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
	mr->lkey    = pd->pdn;
	mr->rkey    = pd->pdn;

	/* 64-bit DDR4 addr the ERNIC DMA targets on remote WRITE/READ. */
	buf_addr = ((u64)ONIC_DDR4_MSB << 32) |
		   (dev->ddr.base_off + ddr_off);

	/* Program the 8 PDT registers (F1 §5.1.2, stride 0x100). */
	row_base = RN_RDMA_BASE_ADDRESS + pd->pdn * 0x100;
	iowrite32(pd->pdn,                         mmio + row_base + 0x00);
	iowrite32((u32)(virt_addr & 0xffffffffu),  mmio + row_base + 0x04);
	iowrite32((u32)(virt_addr >> 32),          mmio + row_base + 0x08);
	iowrite32((u32)(buf_addr & 0xffffffffu),   mmio + row_base + 0x0C);
	iowrite32((u32)(buf_addr >> 32),           mmio + row_base + 0x10);
	iowrite32(mr->rkey & 0xffu,                mmio + row_base + 0x14);
	iowrite32((u32)(length & 0xffffffffu),     mmio + row_base + 0x18);
	iowrite32(((u32)(length >> 16) & 0xffff0000u) |
		  ((u32)mr->access & 0x3),         mmio + row_base + 0x1C);
	(void)ioread32(mmio + row_base + 0x00);   /* posted-write flush */

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
	u32                 row_base = RN_RDMA_BASE_ADDRESS + pd->pdn * 0x100;
	int                 i;
	(void)udata;

	for (i = 0; i < 8; i++)
		iowrite32(0, mmio + row_base + i * 4);
	(void)ioread32(mmio + row_base + 0);

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
	cq->bound = false;
	/* cq_id and ddr_off get filled in when create_qp binds this CQ. */
	return 0;
}

static int onic_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct onic_cq *cq = to_onic_cq(ibcq);
	(void)udata;

	if (cq->bound)
		return -EBUSY;
	return 0;
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
	void __iomem       *mmio = dev->priv->hw.addr;
	u32                 qp_idx;
	u64                 slot_off, sq_off, rq_off, cq_off;
	u32                 qpconfi, qpadvconfi, qdepthi, q;
	int                 rv;
	(void)udata;

	if (init_attr->qp_type != IB_QPT_RC)
		return -EOPNOTSUPP;
	if (init_attr->cap.max_send_wr > 16 ||
	    init_attr->cap.max_recv_wr > 16 ||
	    init_attr->cap.max_send_sge > 1 ||
	    init_attr->cap.max_recv_sge > 1)
		return -EINVAL;
	if (!scq || !rcq)
		return -EINVAL;
	if (scq->bound || rcq->bound)
		return -EINVAL;     /* B5: no CQ sharing across QPs */
	if (rcq != scq)
		return -EINVAL;     /* B5: single CQ for send + recv (ERNIC
				     * has one CQ per QP) */

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
	qp->sq_depth   = init_attr->cap.max_send_wr;
	qp->rq_depth   = init_attr->cap.max_recv_wr;
	qp->cq_depth   = scq->depth;
	qp->path_mtu   = 4;                       /* PATHMTU code 4 = 4096 */
	qp->state      = ERNIC_QP_RESET;
	spin_lock_init(&qp->state_lock);

	/* QPCONFi: QPEN=0 (B6 turns it on), RQINTEN+CQINTEN, HWHSHKDIS,
	 * PATHMTU=4096, RQBUFSZ = rq_depth in 256-B units (PG332). */
	qpconfi  = 0;
	qpconfi |= (1u << 2);                      /* RQINTEN */
	qpconfi |= (1u << 3);                      /* CQINTEN */
	qpconfi |= (1u << 5);                      /* HWHSHKDIS */
	qpconfi |= ((u32)qp->path_mtu & 0x7) << 8;
	qpconfi |= ((u32)qp->rq_depth & 0xffffu) << 16;

	qpadvconfi  = 0;
	qpadvconfi |= (0u    << 0);                /* TC */
	qpadvconfi |= (64u   << 8);                /* TTL */
	qpadvconfi |= (0xFFFFu << 16);             /* PKEY */

	qdepthi  = ((u32)qp->rq_depth << 16) | (u32)qp->sq_depth;

	q = RN_RDMA_QCSR_REG(qp_idx, 0x00);
	iowrite32(qpconfi,     mmio + q);                         /* QPCONFi   */
	iowrite32(qpadvconfi,  mmio + q + 0x04);                  /* QPADVCONFi*/
	iowrite32(onic_ddr_addr_lsb(&dev->ddr, rq_off),
		  mmio + q + 0x08);                               /* RQBAi     */
	iowrite32(onic_ddr_addr_msb(&dev->ddr, rq_off),
		  mmio + q + 0xC0);                               /* RQBAMSBi  */
	iowrite32(onic_ddr_addr_lsb(&dev->ddr, sq_off),
		  mmio + q + 0x10);                               /* SQBAi     */
	iowrite32(onic_ddr_addr_msb(&dev->ddr, sq_off),
		  mmio + q + 0xC8);                               /* SQBAMSBi  */
	iowrite32(onic_ddr_addr_lsb(&dev->ddr, cq_off),
		  mmio + q + 0x18);                               /* CQBAi     */
	iowrite32(onic_ddr_addr_msb(&dev->ddr, cq_off),
		  mmio + q + 0xD0);                               /* CQBAMSBi  */
	iowrite32(qdepthi,     mmio + q + 0x3C);                  /* QDEPTHi   */
	iowrite32(pd->pdn,     mmio + q + 0xB0);                  /* PDi       */
	/* We don't use host-DMA doorbells in B5. */
	iowrite32(0,           mmio + q + 0x20);                  /* RQWPTRDBADDi  */
	iowrite32(0,           mmio + q + 0x24);                  /* RQWPTRDBADDMSBi*/
	iowrite32(0,           mmio + q + 0x28);                  /* CQDBADDi  */
	iowrite32(0,           mmio + q + 0x2C);                  /* CQDBADDMSBi*/
	(void)ioread32(mmio + q + 0x00);                          /* flush     */

	/* Bind the CQ to this slot. */
	scq->cq_id   = qp_idx;
	scq->ddr_off = cq_off;
	scq->bound   = true;

	qp->state        = ERNIC_QP_INIT;
	qp->ibqp.qp_num  = qp_idx;
	return 0;
}

static int onic_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct onic_qp     *qp   = to_onic_qp(ibqp);
	struct onic_ib_dev *dev  = to_onic_ib_dev(ibqp->device);
	void __iomem       *mmio = dev->priv->hw.addr;
	u32                 q    = RN_RDMA_QCSR_REG(qp->qp_num, 0x00);
	(void)udata;

	iowrite32(0, mmio + q);
	iowrite32(0, mmio + q + 0x04);
	iowrite32(0, mmio + q + 0x08);
	iowrite32(0, mmio + q + 0xC0);
	iowrite32(0, mmio + q + 0x10);
	iowrite32(0, mmio + q + 0xC8);
	iowrite32(0, mmio + q + 0x18);
	iowrite32(0, mmio + q + 0xD0);
	iowrite32(0, mmio + q + 0x3C);
	iowrite32(0, mmio + q + 0xB0);
	(void)ioread32(mmio + q);

	if (qp->send_cq)
		qp->send_cq->bound = false;
	if (qp->recv_cq && qp->recv_cq != qp->send_cq)
		qp->recv_cq->bound = false;

	onic_ddr_qp_slot_free(&dev->ddr, qp->qp_num);
	qp->state = ERNIC_QP_CLOSED;
	return 0;
}

/* ---- remaining stubs (B6/B7/B8 land these) ----------------------------- */

static int onic_stub_modify_qp(struct ib_qp *qp, struct ib_qp_attr *a, int mask,
			       struct ib_udata *u)               { STUB_BODY("modify_qp"); }
static int onic_stub_query_qp(struct ib_qp *qp, struct ib_qp_attr *a, int mask,
			      struct ib_qp_init_attr *ia)        { STUB_BODY("query_qp"); }
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

	.modify_qp                   = onic_stub_modify_qp,
	.query_qp                    = onic_stub_query_qp,
	.post_send                   = onic_stub_post_send,
	.post_recv                   = onic_stub_post_recv,
	.poll_cq                     = onic_stub_poll_cq,
	.req_notify_cq               = onic_stub_req_notify_cq,
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

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return 0;

	dev = (struct onic_ib_dev *)ib_alloc_device(onic_ib_dev, ibdev);
	if (!dev)
		return -ENOMEM;

	dev->priv = priv;
	spin_lock_init(&dev->pd_lock);
	bitmap_zero(dev->pd_bitmap, ONIC_IB_MAX_PD);
	onic_ddr_pool_init(&dev->ddr, 0 /* ERNIC0 */);

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
	onic_ddr_pool_fini(&dev->ddr);
	ib_dealloc_device(&dev->ibdev);
	priv->ib_dev = NULL;
}
