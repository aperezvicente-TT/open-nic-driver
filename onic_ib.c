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
#include "onic_sysdma.h"
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

	/* B7 — capture DDR4 byte offsets for the verb path.
	 * Includes pool->base_off so onic_ddr4_write/read get the right
	 * crossbar AXI offset on both ERNIC0 (base_off=0) and ERNIC1. */
	qp->sq_ddr_off = dev->ddr.base_off + sq_off;
	qp->rq_ddr_off = dev->ddr.base_off + rq_off;
	qp->cq_ddr_off = dev->ddr.base_off + cq_off;
	qp->sq_pidb         = 0;
	qp->rq_pidb         = 0;
	qp->cq_consumer_idx = 0;

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

	/* QPCONFi: QPEN=0 (B6 turns it on), RQINTEN+CQINTEN, HWHSHKDIS,
	 * PATHMTU=4096, RQBUFSZ = rq_depth in 256-B units (PG332). */
	qpconfi  = 0;
	qpconfi |= (1u << 2);                      /* RQINTEN */
	qpconfi |= (1u << 3);                      /* CQINTEN */
	qpconfi |= (1u << 5);                      /* HWHSHKDIS */
	qpconfi |= ((u32)qp->path_mtu & 0x7) << 8;
	/* B7 fix: QPCONFi[31:16] is RQE size in 256-B units (= 2 for 512-B
	 * libreconic RQEs), NOT rq_depth.  The B5 code packed rq_depth here
	 * by mistake — without this fix post_recv simply will not work
	 * because the engine writes RQEs at the wrong stride. */
	qpconfi |= ((u32)ERNIC_RQE_SIZE_FIELD & 0xffffu) << 16;

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
	scq->qp_back = qp;        /* B7: poll_cq needs it */

	/* B6: leave state at RESET — ibverbs sends an explicit RESET→INIT
	 * modify_qp right after create_qp and that call is what advances
	 * us into INIT.  Matches IB spec expectations. */
	qp->state        = ERNIC_QP_RESET;
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

	if (qp->send_cq) {
		qp->send_cq->bound   = false;
		qp->send_cq->qp_back = NULL;
	}
	if (qp->recv_cq && qp->recv_cq != qp->send_cq) {
		qp->recv_cq->bound   = false;
		qp->recv_cq->qp_back = NULL;
	}

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
	const int required = IB_QP_STATE | IB_QP_PKEY_INDEX |
			     IB_QP_PORT  | IB_QP_ACCESS_FLAGS;

	if ((mask & required) != required) {
		pr_info_ratelimited("onic_ib: R->I missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}
	if (attr->port_num != 1) {
		pr_info_ratelimited("onic_ib: R->I port_num=%u, only 1 supported\n",
				    attr->port_num);
		return -EOPNOTSUPP;
	}
	if (attr->pkey_index != 0)
		return -EINVAL;

	qp->state = ERNIC_QP_INIT;
	qp->port_num = attr->port_num;
	pr_info("onic_ib: qp[%u] RESET -> INIT (port=%u pkey_idx=%u)\n",
		qp->qp_num, attr->port_num, attr->pkey_index);
	return 0;
}

static int onic_modify_qp_init_to_rtr(struct onic_qp *qp,
				      struct ib_qp_attr *attr, int mask)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;
	const int required = IB_QP_STATE | IB_QP_AV | IB_QP_PATH_MTU |
			     IB_QP_DEST_QPN | IB_QP_RQ_PSN;
	u32 q       = RN_RDMA_QCSR_REG(qp->qp_num, 0x00);
	u32 destqp, mac_lsb, mac_msb;
	u32 ip1 = 0, ip2 = 0, ip3 = 0, ip4 = 0;
	u32 timeoutconf, qpconfi;
	const u8 *dmac;
	const union ib_gid *dgid;
	bool is_v4;
	u8  to_val, rt_val, rnr_rt, rnr_to;

	if ((mask & required) != required) {
		pr_info_ratelimited("onic_ib: I->R missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}
	if (!(rdma_ah_get_ah_flags(&attr->ah_attr) & IB_AH_GRH)) {
		pr_info_ratelimited("onic_ib: I->R no GRH in ah_attr\n");
		return -EINVAL;
	}
	dgid = &rdma_ah_read_grh(&attr->ah_attr)->dgid;
	dmac = rdma_ah_retrieve_dmac(&attr->ah_attr);
	if (!dmac) {
		pr_info_ratelimited("onic_ib: I->R dmac missing\n");
		return -EINVAL;
	}
	if (attr->path_mtu != IB_MTU_4096) {
		pr_info_ratelimited("onic_ib: I->R path_mtu=%d unsupported\n",
				    attr->path_mtu);
		return -EOPNOTSUPP;
	}

	destqp  = attr->dest_qp_num & 0x00FFFFFFu;
	mac_lsb = ((u32)dmac[0]) | ((u32)dmac[1] << 8) |
		  ((u32)dmac[2] << 16) | ((u32)dmac[3] << 24);
	mac_msb = ((u32)dmac[4]) | ((u32)dmac[5] << 8);

	is_v4 = gid_is_ipv4(dgid);
	if (is_v4) {
		ip1 = ((u32)dgid->raw[12]) |
		      ((u32)dgid->raw[13] <<  8) |
		      ((u32)dgid->raw[14] << 16) |
		      ((u32)dgid->raw[15] << 24);
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

	/* QPCONFi[7]: IP version.  RMW, don't touch QPEN (still 0 here). */
	qpconfi = ioread32(mmio + q);
	qpconfi &= ~(1u << 7);
	if (is_v4)
		qpconfi |= (1u << 7);
	iowrite32(qpconfi, mmio + q);
	(void)ioread32(mmio + q);

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
	return 0;
}

static int onic_modify_qp_rtr_to_rts(struct onic_qp *qp,
				     struct ib_qp_attr *attr, int mask)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;
	u32 q       = RN_RDMA_QCSR_REG(qp->qp_num, 0x00);
	u32 qpen_ct, new_ct, qpconfi;
	const int required = IB_QP_STATE | IB_QP_SQ_PSN;

	if ((mask & required) != required) {
		pr_info_ratelimited("onic_ib: R->S missing mask have=0x%x need=0x%x\n",
				    mask, required);
		return -EINVAL;
	}

	iowrite32(attr->sq_psn & 0x00FFFFFFu, mmio + q + 0x40);
	qp->sq_psn = attr->sq_psn;

	/* XRNIC_CONF_QP_EN is global GCSR.  VERIFY: count vs mask — using
	 * COUNT semantics (F7 §5.4, F1 audit §6.2). */
	qpen_ct = ioread32(mmio + (RN_RDMA_GCSR_XRNIC_CONF_QP_EN -
				   RN_RDMA_BASE_ADDRESS)) & 0xFFFu;
	new_ct  = qp->qp_num + 1;
	if (new_ct > qpen_ct) {
		iowrite32(new_ct, mmio + (RN_RDMA_GCSR_XRNIC_CONF_QP_EN -
					  RN_RDMA_BASE_ADDRESS));
		(void)ioread32(mmio + (RN_RDMA_GCSR_XRNIC_CONF_QP_EN -
				       RN_RDMA_BASE_ADDRESS));
	}

	/* Set QPEN=1 without trashing other bits. */
	qpconfi  = ioread32(mmio + q);
	qpconfi |= 0x1u;
	iowrite32(qpconfi, mmio + q);
	(void)ioread32(mmio + q);

	qp->state = ERNIC_QP_RTS;
	pr_info("onic_ib: qp[%u] RTR -> RTS sq_psn=%u qp_en_ct %u -> %u\n",
		qp->qp_num, qp->sq_psn, qpen_ct, max(qpen_ct, new_ct));
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

	spin_lock(&qp->state_lock);
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

	spin_unlock(&qp->state_lock);
	return rv;
}

static int onic_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct onic_qp *qp = to_onic_qp(ibqp);
	(void)attr_mask;

	memset(attr, 0, sizeof(*attr));
	spin_lock(&qp->state_lock);
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
	spin_unlock(&qp->state_lock);

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

/* Compose one SQ WQE in DDR4 and update the SQ shadow. Does NOT ring
 * the doorbell — caller does that once at the end of a chain. */
static int ernic_sq_post_one(struct onic_qp *qp, const struct ib_send_wr *wr)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	struct onic_pd     *pd   = qp->pd;
	struct ernic_sq_wqe wqe;
	u32                 ernic_op;
	enum ib_wc_opcode   wc_op;
	u32                 slot;
	u32                 length;
	u64                 laddr;
	int                 rv;

	if (wr->num_sge > 1)
		return -EOPNOTSUPP;
	if (wr->next != NULL && wr->num_sge == 0) {
		/* Allowed at IB level (zero-length SEND) — fall through. */
	}

	rv = ernic_xlate_opcode(wr->opcode, &ernic_op, &wc_op);
	if (rv) {
		dev_info(&dev->priv->pdev->dev,
			 "onic_ib: post_send unsupported opcode=%d\n",
			 (int)wr->opcode);
		return rv;
	}

	length = (wr->num_sge == 1) ? wr->sg_list[0].length : 0;
	laddr  = (wr->num_sge == 1) ? wr->sg_list[0].addr   : 0;

	/* B7 first cut: payload comes from a registered MR. We don't have a
	 * generic "stage host buffer in DDR4" path yet, so for SEND we only
	 * accept inline payload ≤16 B (carried in the WQE itself). For
	 * RDMA_WRITE the engine reads `laddr` from DDR4 directly, so the
	 * caller must have registered the MR and `addr` is the DDR4 byte
	 * offset of the source — same convention as libreconic. */
	if (wr->opcode == IB_WR_SEND && length > 16) {
		dev_info(&dev->priv->pdev->dev,
			 "onic_ib: post_send SEND len=%u > 16 not yet supported\n",
			 length);
		return -EOPNOTSUPP;
	}

	memset(&wqe, 0, sizeof(wqe));
	slot = qp->sq_pidb % qp->sq_depth;

	wqe.wrid       = cpu_to_le16((u16)slot);
	wqe.length     = cpu_to_le32(length);
	wqe.opcode     = cpu_to_le32(ernic_op & 0xff);

	if (wr->opcode == IB_WR_SEND) {
		/* Inline payload path: the WQE itself carries the bytes; the
		 * laddr/laddr_high fields are unused for inline SEND. */
		if (length > 0 && wr->sg_list[0].addr) {
			/* `addr` here is a kernel virtual address from the
			 * post_send caller (in-kernel ULPs only — userspace
			 * verbs have not been wired through ucontext yet).
			 * Direct memcpy is the safe path for ≤16 B inline. */
			memcpy(wqe.send_small_payload,
			       (const void *)(uintptr_t)wr->sg_list[0].addr,
			       length);
		}
	} else {
		/* RDMA_WRITE: laddr = DDR4 byte offset of source (within the
		 * caller's registered MR). The engine resolves this through
		 * the PDT row indexed by qp->pd->mr->lkey. */
		wqe.laddr_low  = cpu_to_le32((u32)(laddr & 0xffffffffu));
		wqe.laddr_high = cpu_to_le32((u32)(laddr >> 32));
		wqe.remote_offset_low  =
			cpu_to_le32((u32)(rdma_wr(wr)->remote_addr & 0xffffffffu));
		wqe.remote_offset_high =
			cpu_to_le32((u32)(rdma_wr(wr)->remote_addr >> 32));
		/* r_key — only [7:0] honoured per spec. */
		wqe.r_key = cpu_to_le32(rdma_wr(wr)->rkey & 0xffu);
	}

	(void)pd;  /* future use: per-PD WQE bookkeeping */

	/* Land the 64 bytes in DDR4 at qp->sq_ddr_off + slot * 64. */
	rv = onic_ddr4_write(dev->priv,
			     qp->sq_ddr_off + (u64)slot * sizeof(wqe),
			     &wqe, sizeof(wqe));
	if (rv) {
		dev_info(&dev->priv->pdev->dev,
			 "onic_ib: post_send sysdma WRITE failed qp=%u slot=%u rv=%d\n",
			 qp->qp_num, slot, rv);
		return rv;
	}

	/* Update the SQ shadow so poll_cq can recover the full wr_id. */
	qp->sq_shadow[slot].wr_id     = wr->wr_id;
	qp->sq_shadow[slot].ib_opcode = wc_op;
	qp->sq_shadow[slot].length    = length;

	qp->sq_pidb++;
	return 0;
}

/* Ring the SQ doorbell after one or more WQEs have landed in DDR4. */
static void ernic_sq_doorbell(struct onic_qp *qp)
{
	struct onic_ib_dev *dev  = to_onic_ib_dev(qp->ibqp.device);
	void __iomem       *mmio = dev->priv->hw.addr;

	wmb();                                  /* WQE bytes visible first */
	iowrite32(qp->sq_pidb,
		  mmio + RN_RDMA_QCSR_REG(qp->qp_num, 0x38));
	(void)ioread32(mmio + RN_RDMA_QCSR_REG(qp->qp_num, 0x38));
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

	spin_lock(&qp->state_lock);
	if (qp->state != ERNIC_QP_RTS) {
		spin_unlock(&qp->state_lock);
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

	if (posted)
		ernic_sq_doorbell(qp);

	spin_unlock(&qp->state_lock);
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

	if (!wr)
		return -EINVAL;

	spin_lock(&qp->state_lock);
	if (qp->state == ERNIC_QP_RESET) {
		spin_unlock(&qp->state_lock);
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
			spin_unlock(&qp->state_lock);
			return -EOPNOTSUPP;
		}

		slot = qp->rq_pidb % qp->rq_depth;
		qp->rq_shadow[slot].wr_id = cur->wr_id;
		qp->rq_pidb++;
		posted++;
		cur = cur->next;
	}

	if (posted) {
		/* Host does NOT compose RQE bytes — engine fills the slot
		 * when an RQPKT lands.  We just bump the consumer-side
		 * counter so the engine knows how many slots are armed.
		 * RQCIi is the "RQ consumer index"; in v4.2 the host writes
		 * the *expected* tail (== monotonic count of armed slots). */
		wmb();
		iowrite32(qp->rq_pidb,
			  mmio + RN_RDMA_QCSR_REG(qp->qp_num, 0x34));
		(void)ioread32(mmio + RN_RDMA_QCSR_REG(qp->qp_num, 0x34));
	}

	spin_unlock(&qp->state_lock);
	return 0;
}

static int onic_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct onic_cq      *cq   = to_onic_cq(ibcq);
	struct onic_ib_dev  *dev  = to_onic_ib_dev(ibcq->device);
	void __iomem        *mmio = dev->priv->hw.addr;
	struct onic_private *priv = dev->priv;
	struct onic_qp      *qp;
	u32                  cqhead;
	int                  polled = 0;

	if (num_entries <= 0)
		return 0;
	if (!cq->bound)
		return 0;

	/* ERNIC v4.2 has a strict 1:1 QP↔CQ mapping; create_qp stashes the
	 * back-pointer on the CQ. Without it we can't find the doorbell. */
	qp = cq->qp_back;
	if (!qp)
		return 0;

	cqhead = ioread32(mmio + RN_RDMA_QCSR_REG(qp->qp_num, 0x30));

	spin_lock(&qp->state_lock);

	while (polled < num_entries && qp->cq_consumer_idx != cqhead) {
		u32                slot = qp->cq_consumer_idx % qp->cq_depth;
		struct ernic_cqe   cqe_raw = {0};
		u32                wqe_slot;
		int                rv;

		rv = onic_ddr4_read(priv, &cqe_raw,
				    qp->cq_ddr_off + (u64)slot * sizeof(cqe_raw),
				    sizeof(cqe_raw));
		if (rv) {
			dev_info_ratelimited(&priv->pdev->dev,
					     "onic_ib: poll_cq sysdma READ failed qp=%u slot=%u rv=%d\n",
					     qp->qp_num, slot, rv);
			break;
		}

		wqe_slot = le16_to_cpu(cqe_raw.wqe_idx) % qp->sq_depth;

		memset(&wc[polled], 0, sizeof(wc[polled]));
		wc[polled].qp        = &qp->ibqp;
		wc[polled].wr_id     = qp->sq_shadow[wqe_slot].wr_id;
		wc[polled].opcode    = qp->sq_shadow[wqe_slot].ib_opcode;
		wc[polled].byte_len  = qp->sq_shadow[wqe_slot].length;
		wc[polled].status    = (cqe_raw.status == 0) ?
					IB_WC_SUCCESS : IB_WC_GENERAL_ERR;
		wc[polled].vendor_err = cqe_raw.status;

		qp->cq_consumer_idx++;
		polled++;
	}

	spin_unlock(&qp->state_lock);
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
