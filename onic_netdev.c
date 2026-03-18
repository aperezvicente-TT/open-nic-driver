/*
 * Copyright (c) 2020 Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#include <linux/if_link.h>
#include <linux/pci_regs.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/bpf_trace.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#include <net/page_pool/helpers.h>
#include <net/page_pool/types.h>
#else 
#include <net/page_pool.h>
#endif

#include "onic_netdev.h"
#include "onic_hardware.h"
#include "onic_register.h"
#include "qdma_access/qdma_register.h"
#include "onic.h"

#define ONIC_RX_DESC_STEP 256

inline static u16 onic_ring_get_real_count(struct onic_ring *ring)
{
	/* Valid writeback entry means one less count of descriptor entries */
	return (ring->wb) ? (ring->count - 1) : ring->count;
}

inline static bool onic_ring_full(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	return ((ring->next_to_use + 1) % real_count) == ring->next_to_clean;
}

inline static int onic_ring_free_count(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	int free = ring->next_to_clean - ring->next_to_use - 1;
	if (free < 0)
		free += real_count;
	return free;
}

inline static void onic_ring_increment_head(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	ring->next_to_use = (ring->next_to_use + 1) % real_count;
}

inline static void onic_ring_increment_tail(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	ring->next_to_clean = (ring->next_to_clean + 1) % real_count;
}

static void onic_tx_clean(struct onic_tx_queue *q)
{
	struct onic_private *priv = netdev_priv(q->netdev);
	struct onic_ring *ring = &q->ring;
	struct qdma_wb_stat wb;
	int work, i;

	// this is a locking mechanism to guarantee that only one thread is cleaning the ring
	// bitmask functions are atomic!
	if (test_and_set_bit(0, q->state))
		return;

	qdma_unpack_wb_stat(&wb, ring->wb);

	if (wb.cidx == ring->next_to_clean) {
		clear_bit(0, q->state);
		return;
	}

	work = wb.cidx - ring->next_to_clean;
	if (work < 0)
		work += onic_ring_get_real_count(ring);

	/* Clamp to what was actually submitted: never clean past next_to_use.
	 * A stale or spurious cidx from the QDMA writeback could otherwise
	 * advance into uninitialised buffer slots (type == 0). */
	{
		int max_clean = ring->next_to_use - ring->next_to_clean;
		if (max_clean < 0)
			max_clean += onic_ring_get_real_count(ring);
		if (work > max_clean)
			work = max_clean;
	}

	for (i = 0; i < work; ++i) {
		struct onic_tx_buffer *buf = &q->buffer[ring->next_to_clean];

		if (buf->type == ONIC_TX_SKB) {
			dma_unmap_single(&priv->pdev->dev, buf->dma_addr, buf->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(buf->skb);
			buf->skb = NULL;
		} else if (buf->type == ONIC_TX_XDPF) {
			xdp_return_frame(buf->xdpf);
			buf->xdpf = NULL;
		} else if (buf->type == ONIC_TX_XDPF_XMIT) {
			dma_unmap_single(&priv->pdev->dev, buf->dma_addr, buf->len, DMA_TO_DEVICE);
			xdp_return_frame(buf->xdpf);
			buf->xdpf = NULL;
		} else {
			netdev_err(priv->netdev, "unknown buffer type %d at index %d (ntc=%d ntu=%d cidx=%d)\n",
				   buf->type, ring->next_to_clean,
				   ring->next_to_clean, ring->next_to_use, wb.cidx);
		}
		/* Reset type so double-clean of this slot is detectable */
		buf->type = 0;

		onic_ring_increment_tail(ring);
	}

	clear_bit(0, q->state);
}

static bool onic_rx_high_watermark(struct onic_rx_queue *q)
{
	struct onic_ring *ring = &q->desc_ring;
	int unused;

	unused = ring->next_to_use - ring->next_to_clean;
	if (ring->next_to_use < ring->next_to_clean)
		unused += onic_ring_get_real_count(ring);

	return (unused < (ONIC_RX_DESC_STEP / 2));
}

static void onic_rx_refill(struct onic_rx_queue *q)
{
	struct onic_private *priv = netdev_priv(q->netdev);
	struct onic_ring *ring = &q->desc_ring;

	ring->next_to_use += ONIC_RX_DESC_STEP;
	ring->next_to_use %= onic_ring_get_real_count(ring);

	onic_set_rx_head(priv->hw.qdma, q->qid, ring->next_to_use);

}

static void onic_rx_page_refill(struct onic_rx_queue *q)
{
	struct onic_ring *desc_ring = &q->desc_ring;
	struct qdma_c2h_st_desc desc;
	struct page *pg;
	u8 *desc_ptr = desc_ring->desc + QDMA_C2H_ST_DESC_SIZE * desc_ring->next_to_clean;

	pg = page_pool_dev_alloc_pages(q->page_pool);

	q->buffer[desc_ring->next_to_clean].pg = pg;
	q->buffer[desc_ring->next_to_clean].offset = XDP_PACKET_HEADROOM;


	desc.dst_addr = page_pool_get_dma_addr(pg) + XDP_PACKET_HEADROOM;
	qdma_pack_c2h_st_desc(desc_ptr, &desc);
}

static struct onic_tx_queue *onic_xdp_tx_queue_mapping(struct onic_private *priv)
{
	unsigned int r_idx = smp_processor_id();

	if (r_idx >= priv->num_tx_queues)
		r_idx = r_idx % priv->num_tx_queues;

	return priv->tx_queue[r_idx];
}

static int onic_xmit_xdp_ring(struct onic_private *priv,struct  onic_tx_queue  *tx_queue, struct xdp_frame *xdpf, bool dma_map)
 {
	u8 *desc_ptr;
	dma_addr_t dma_addr;
	struct onic_ring *ring;
	struct qdma_h2c_st_desc desc;
	struct rtnl_link_stats64 *pcpu_stats_pointer;
	enum onic_tx_buf_type type;

	ring = &tx_queue->ring;

	onic_tx_clean(tx_queue);

	if (onic_ring_full(ring)) {
		onic_netdev_dbg(ONIC_DBG_INFO, priv->netdev, "xdp tx ring full");
		return NETDEV_TX_BUSY;
	}

	if (dma_map) {
		/* ndo_xdp_dmit */
		dma_addr = dma_map_single(&priv->pdev->dev, xdpf->data,xdpf->len, DMA_TO_DEVICE);
		type = ONIC_TX_XDPF_XMIT;
		if (unlikely(dma_mapping_error(&priv->pdev->dev, dma_addr)))
			return ONIC_XDP_CONSUMED;
	} else {
		/* ONIC_XDP_TX */
		struct page *page = virt_to_page(xdpf->data);
		//TODO  i don't get why adding the size of the xdp_frame struct to the dma_addr. mvneta does this 
		dma_addr = page_pool_get_dma_addr(page) + sizeof(*xdpf) + xdpf->headroom;
		dma_sync_single_for_device(&priv->pdev->dev, dma_addr,
					   xdpf->len, DMA_BIDIRECTIONAL);
		type = ONIC_TX_XDPF;
		
	}

	

	desc_ptr = ring->desc + QDMA_H2C_ST_DESC_SIZE * ring->next_to_use;
	desc.len = xdpf->len;
	desc.src_addr = dma_addr;
	desc.metadata = xdpf->len;
	qdma_pack_h2c_st_desc(desc_ptr, &desc);

	tx_queue->buffer[ring->next_to_use].xdpf = xdpf;
	tx_queue->buffer[ring->next_to_use].type = type;
	tx_queue->buffer[ring->next_to_use].dma_addr = dma_addr;
	tx_queue->buffer[ring->next_to_use].len = xdpf->len;
	

	pcpu_stats_pointer = this_cpu_ptr(priv->netdev_stats);
	pcpu_stats_pointer->tx_packets++;
	pcpu_stats_pointer->tx_bytes += xdpf->len;
	onic_ring_increment_head(ring);

	return ONIC_XDP_TX;
}

static int onic_xdp_xmit_back(struct onic_rx_queue *q, struct xdp_buff *xdp_buff) {
	struct onic_private *priv = netdev_priv(q->netdev);
	struct xdp_frame *xdpf = xdp_convert_buff_to_frame(xdp_buff);
	struct onic_tx_queue *tx_queue;
	struct netdev_queue *nq;
	u32 ret = 0, cpu = smp_processor_id();

	if (unlikely(!xdpf)){
		q->xdp_rx_stats.xdp_tx_err++;
		return ONIC_XDP_CONSUMED;
	}

	tx_queue = q->xdp_prog ? priv->tx_queue[q->qid] : NULL;
	if (unlikely(!tx_queue)){
		q->xdp_rx_stats.xdp_tx_err++;
		return -ENXIO;
	}

	nq = netdev_get_tx_queue(tx_queue->netdev, tx_queue->qid);

	__netif_tx_lock(nq, cpu);
	ret = onic_xmit_xdp_ring(priv, tx_queue, xdpf,false);
	q->xdp_rx_stats.xdp_tx++;

	wmb();
	onic_set_tx_head(priv->hw.qdma, tx_queue->qid, tx_queue->ring.next_to_use);

	__netif_tx_unlock(nq);

	return ret;
}

static void *onic_run_xdp(struct onic_rx_queue *rx_queue, struct xdp_buff *xdp_buff, struct onic_private *priv) {
	int err, result = ONIC_XDP_PASS;
	struct bpf_prog *xdp_prog;
	u32 act;
	struct page *page = virt_to_page(xdp_buff->data_hard_start);
	
	xdp_prog = rx_queue->xdp_prog;
	if (!xdp_prog){
		goto out;
	}

	act = bpf_prog_run_xdp(xdp_prog, xdp_buff);
	
	switch (act){
		case XDP_PASS:
			rx_queue->xdp_rx_stats.xdp_pass++;
			break;
// Since before 5.3.0 the xmit_more hint was tied to skbs, and in XDP we run
// before skb allocation, we cannot correctly implement onic_xdp_xmit_frame and
// thus XDP_TX and XDP_REDIRECT
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
    case XDP_TX:
			result = onic_xdp_xmit_back(rx_queue, xdp_buff);
			if (result == ONIC_XDP_CONSUMED) {
				goto out_failure;
				}
			break;
    case XDP_REDIRECT:
			err = xdp_do_redirect(rx_queue->netdev, xdp_buff, xdp_prog);
			if (err) {
				result = ONIC_XDP_CONSUMED;
				goto out_failure;
			}
			result = ONIC_XDP_REDIR;
			rx_queue->xdp_rx_stats.xdp_redirect++;
			break;
#elif defined(RHEL_RELEASE_CODE)
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8, 1))
    case XDP_TX:
			result = onic_xdp_xmit_back(rx_queue, xdp_buff);
			if (result == ONIC_XDP_CONSUMED)
				goto out_failure;
			break;
    case XDP_REDIRECT:
			err = xdp_do_redirect(rx_queue->netdev, xdp_buff, xdp_prog);
			if (err) {
				ret = ONIC_XDP_CONSUMED;
				goto out_failure;
			}
			result = ONIC_XDP_REDIR;
			rx_queue->xdp_rx_stats.xdp_redirect++;
			break;
#endif
#else
		case XDP_TX:
			fallthrough;
		case XDP_REDIRECT
			fallthrough;
#endif
    default:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0)
			bpf_warn_invalid_xdp_action(priv->netdev, xdp_prog, act);
#else
			bpf_warn_invalid_xdp_action(act);
#endif
			fallthrough;
    case XDP_ABORTED:
out_failure:
			trace_xdp_exception(rx_queue->netdev, xdp_prog, act);
			fallthrough;
    case XDP_DROP:
			result = ONIC_XDP_CONSUMED;
			rx_queue->xdp_rx_stats.xdp_drop++;
			page_pool_recycle_direct(rx_queue->page_pool, page);
			break;
  }

out:
	return ERR_PTR(-result);
}

static int onic_rx_poll(struct napi_struct *napi, int budget)
{
	struct onic_rx_queue *q =
		container_of(napi, struct onic_rx_queue, napi);
	struct onic_private *priv = netdev_priv(q->netdev);
	u16 qid = q->qid;
	struct onic_ring *desc_ring = &q->desc_ring;
	struct onic_ring *cmpl_ring = &q->cmpl_ring;
	struct qdma_c2h_cmpl cmpl;
	struct qdma_c2h_cmpl_stat cmpl_stat;
	u8 *cmpl_ptr;
	u8 *cmpl_stat_ptr;
	u32 color_stat;
	int work = 0;
	int rv;
	void *res;

	struct xdp_buff xdp;
	unsigned int xdp_xmit = 0;
	struct rtnl_link_stats64 *pcpu_stats_pointer;
	pcpu_stats_pointer = this_cpu_ptr(priv->netdev_stats);

	if (qid < priv->num_tx_queues)
		onic_tx_clean(priv->tx_queue[qid]);

	cmpl_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean;
	cmpl_stat_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * (cmpl_ring->count - 1);

	qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);
	qdma_unpack_c2h_cmpl_stat(&cmpl_stat, cmpl_stat_ptr);

	color_stat = cmpl_stat.color;
	onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
			"rx_poll: stat_pidx %u color_stat %u ntc %u stat_cidx %u intr %u count %u",
			cmpl_stat.pidx, color_stat, cmpl_ring->next_to_clean,
			cmpl_stat.cidx, cmpl_stat.intr_state, cmpl_ring->count);
	onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
			"c2h_cmpl pkt_id %u pkt_len %u err %u color %u ring_color %u",
			cmpl.pkt_id, cmpl.pkt_len, cmpl.err, cmpl.color,
			cmpl_ring->color);

	/* Color of completion entries and completion ring are initialized to 0
	 * and 1 respectively.  When an entry is filled, it has a color bit of
	 * 1, thus making it the same as the completion ring color.  A different
	 * color indicates that we are done with the current batch.  When the
	 * ring index wraps around, the color flips in both software and
	 * hardware.  Therefore, it becomes that completion entries are filled
	 * with a color 0, and completion ring has a color 0 as well.
	 */
	if (cmpl.color != cmpl_ring->color) {
		onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
				"color mismatch: cmpl %u ring %u stat %u",
				cmpl.color, cmpl_ring->color, color_stat);
	}

	if (cmpl.err == 1) {
		netdev_warn(q->netdev, "completion error at ntc=%u pidx=%u — rescheduling",
			    cmpl_ring->next_to_clean, cmpl_stat.pidx);
		/* Do not disarm the global error IRQ here; onic_error_thread_fn
		 * owns the error interrupt lifecycle and will re-arm it.
		 * Reschedule so the poll function retries after the error clears. */
		napi_schedule(napi);
	}

	// main processing loop for rx_poll
	while ((cmpl_ring->next_to_clean != cmpl_stat.pidx)) {
		struct onic_rx_buffer *buf =
			&q->buffer[desc_ring->next_to_clean];
		struct sk_buff *skb;
		
		int len = cmpl.pkt_len;

		xdp_init_buff(&xdp,
			      PAGE_SIZE << q->page_pool->p.order,
			      &q->xdp_rxq);

		dma_sync_single_for_cpu(&priv->pdev->dev,
					page_pool_get_dma_addr(buf->pg) +
						buf->offset,
						len, DMA_FROM_DEVICE);
   
		xdp_prepare_buff(&xdp, page_address(buf->pg), buf->offset, len, false);
		
		res = onic_run_xdp(q, &xdp,priv);
		if (IS_ERR(res)) {
			unsigned int xdp_res = -PTR_ERR(res);

			if (xdp_res & (ONIC_XDP_TX | ONIC_XDP_REDIR)) {
				xdp_xmit |= xdp_res;
			}

			// Allocate skb only if we are continuing to process the packet
			if (xdp_res & ONIC_XDP_PASS) {
				
				// allocate a new skb structure around the data 
				skb = napi_build_skb(xdp.data_hard_start,
						     PAGE_SIZE << q->page_pool->p.order);

				if (!skb) {
					rv = -ENOMEM;
					break;
				}
				
				// mark the skb for page_pool recycling
				skb_mark_for_recycle(skb);
				// reserve space in the skb for the data for the xdp headroom
				skb_reserve(skb, xdp.data - xdp.data_hard_start);
				// set the data pointer
				skb_put(skb, xdp.data_end - xdp.data);

				skb->protocol = eth_type_trans(skb, q->netdev);
				skb_record_rx_queue(skb, qid);
				rv = napi_gro_receive(napi, skb);
				if (rv < 0) {
					netdev_err(q->netdev, "napi_gro_receive, err = %d", rv);
					break;
				}
			}
		}


		// here the page where packet data was written has either been recycled or marked for recycling
		onic_rx_page_refill(q);

		pcpu_stats_pointer->rx_packets++;
		pcpu_stats_pointer->rx_bytes += len;

		onic_ring_increment_tail(desc_ring);

		if (onic_ring_full(desc_ring))
			onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
					"desc_ring full");

		if (onic_rx_high_watermark(q)) {
			onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
					"high watermark: ntu=%d ntc=%d",
					desc_ring->next_to_use,
					desc_ring->next_to_clean);
			onic_rx_refill(q);
		}

		onic_ring_increment_tail(cmpl_ring);

		if (onic_ring_full(cmpl_ring))
			onic_netdev_dbg(ONIC_DBG_DATA, q->netdev,
					"cmpl_ring full");

		if (cmpl.color != cmpl_ring->color) {
			cmpl_ring->color = (cmpl_ring->color == 0) ? 1 : 0;
		}
		cmpl_ptr = cmpl_ring->desc +
			   (QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean);

		if ((++work) >= budget) {
			if (xdp_xmit & ONIC_XDP_REDIR)
				xdp_do_flush();
			/* Returning budget without calling napi_complete signals
			 * the NAPI subsystem to reschedule automatically.
			 * Calling napi_complete + napi_schedule here is wrong:
			 * it re-enables interrupts then immediately re-queues
			 * via softirq, producing the "Budget exhausted after
			 * napi rescheduled" warning and a busy-loop.
			 *
			 * Update CIDX here (irq_arm=0) so the hardware sees the
			 * completions we just consumed and has room to write more.
			 * Without this, the CIDX stays stale at line rate and the
			 * completion ring fills up → CMPL_QFULL_ERR followed by
			 * CMPL_INV_Q_ERR as QDMA marks the queue invalid. */
			onic_set_completion_tail(priv->hw.qdma, qid,
						 cmpl_ring->next_to_clean, 0);
			goto out_of_budget;
		}

		qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);
	}

	if (xdp_xmit & ONIC_XDP_REDIR)
		xdp_do_flush();

	napi_complete_done(napi, work);
	onic_set_completion_tail(priv->hw.qdma, qid,
				 cmpl_ring->next_to_clean, 1);
	/*
	 * Re-read completion status after re-arming the interrupt.
	 * A completion may have arrived between the loop exit and
	 * the re-arm, in which case the interrupt was lost.
	 */
	qdma_unpack_c2h_cmpl_stat(&cmpl_stat, cmpl_stat_ptr);
	if (cmpl_ring->next_to_clean != cmpl_stat.pidx)
		napi_schedule(&q->napi);

out_of_budget:
	return work;
}

static void onic_clear_tx_queue(struct onic_private *priv, u16 qid)
{
	struct onic_tx_queue *q = priv->tx_queue[qid];
	struct onic_ring *ring;
	u32 size;
	int real_count;
	int i;

	if (!q)
		return;

	onic_tx_clean(q);

	onic_qdma_clear_tx_queue(priv->hw.qdma, qid);

	ring = &q->ring;
	real_count = ring->count - 1;
	size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	/* Drain any in-flight buffers not yet acknowledged by hardware.
	 * onic_tx_clean() only processes up to wb.cidx; packets submitted
	 * after that point are orphaned when the QDMA queue is torn down. */
	for (i = 0; i < real_count; ++i) {
		struct onic_tx_buffer *buf = &q->buffer[i];

		if (buf->type == ONIC_TX_SKB && buf->skb) {
			dma_unmap_single(&priv->pdev->dev, buf->dma_addr, buf->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(buf->skb);
			buf->skb = NULL;
		} else if (buf->type == ONIC_TX_XDPF && buf->xdpf) {
			xdp_return_frame(buf->xdpf);
			buf->xdpf = NULL;
		} else if (buf->type == ONIC_TX_XDPF_XMIT && buf->xdpf) {
			dma_unmap_single(&priv->pdev->dev, buf->dma_addr, buf->len, DMA_TO_DEVICE);
			xdp_return_frame(buf->xdpf);
			buf->xdpf = NULL;
		}
		buf->type = 0;
	}

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);
	if (q->buffer) kfree(q->buffer);
	kfree(q);
	priv->tx_queue[qid] = NULL;
}

static int onic_init_tx_queue(struct onic_private *priv, u16 qid)
{
	const u8 rngcnt_idx = 0;
	struct net_device *dev = priv->netdev;
	struct onic_tx_queue *q;
	struct onic_ring *ring;
	struct onic_qdma_h2c_param param;
	u16 vid;
	u32 size, real_count;
	int rv;

	if (priv->tx_queue[qid]) {
		onic_netdev_dbg(ONIC_DBG_INIT, dev,
				"Re-initializing TX queue %d", qid);
		onic_clear_tx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct onic_tx_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	/* evenly assign to TX queues available vectors */
	vid = qid % priv->num_q_vectors;

	q->netdev = dev;
	q->vector = priv->q_vector[vid];
	q->qid = qid;

	ring = &q->ring;
	ring->count = onic_ring_count(rngcnt_idx);
	real_count = ring->count - 1;

	/* allocate DMA memory for TX descriptor ring */
	size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_tx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_H2C_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	netdev_info(dev, "TX queue %d, ring count %d, ring size %d, real_count %d", 
		    qid, ring->count, size, real_count);

	/* initialize TX buffers */
	q->buffer =
		kcalloc(real_count, sizeof(struct onic_tx_buffer), GFP_KERNEL);
	if (!q->buffer) {
		rv = -ENOMEM;
		goto clear_tx_queue;
	}

	/* initialize QDMA H2C queue */
	param.rngcnt_idx = rngcnt_idx;
	param.dma_addr = ring->dma_addr;
	param.vid = vid;
	rv = onic_qdma_init_tx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_tx_queue;

	priv->tx_queue[qid] = q;
	return 0;

clear_tx_queue:
	onic_clear_tx_queue(priv, qid);
	return rv;
}

static void onic_clear_rx_queue(struct onic_private *priv, u16 qid)
{
	struct onic_rx_queue *q = priv->rx_queue[qid];
	struct onic_ring *ring;
	u32 size, real_count;
	int i;

	if (!q)
		return;

	/* Stop NAPI BEFORE tearing down the QDMA queue.  If a poll is
	 * running on another CPU when we invalidate the QDMA contexts,
	 * the poll reads undefined cmpl_stat.pidx and its while-loop
	 * spins on garbage — napi_disable() then waits forever.
	 *
	 * napi_disable() won't spin here because:
	 *   - onic_remove() already disabled CMAC RX (pipeline drained)
	 *   - onic_clear_interrupt() freed all queue IRQs
	 *   - any in-progress poll exhausts remaining completions and
	 *     calls napi_complete_done(), clearing NAPI_STATE_SCHED */
	napi_disable(&q->napi);
	netif_napi_del(&q->napi);

	onic_qdma_clear_rx_queue(priv->hw.qdma, qid);

	ring = &q->desc_ring;
	real_count = ring->count - 1;
	size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);

	ring = &q->cmpl_ring;
	real_count = ring->count - 1;
	size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);

	for (i = 0; i < real_count; ++i) {
		struct page *pg = q->buffer[i].pg;
		page_pool_put_full_page(q->page_pool, pg, false);
	}

	if (q->buffer) kfree(q->buffer);
	if (xdp_rxq_info_is_reg(&q->xdp_rxq))
		xdp_rxq_info_unreg(&q->xdp_rxq);
	page_pool_destroy(q->page_pool);
	q->page_pool = NULL;
	kfree(q);
	priv->rx_queue[qid] = NULL;
}


static int onic_create_page_pool(struct onic_private *priv,
				 struct onic_rx_queue *q, int size,
				 int page_order) {
	struct bpf_prog *xdp_prog = READ_ONCE(priv->xdp_prog);
	struct page_pool_params pp_params = {
		.order = page_order,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = size,
		.nid = dev_to_node(&priv->pdev->dev),
		.dev = &priv->pdev->dev,
		.dma_dir = xdp_prog ? DMA_BIDIRECTIONAL : DMA_FROM_DEVICE,
		.offset = XDP_PACKET_HEADROOM,
		.max_len = priv->netdev->mtu + ETH_HLEN,
	};
	int err;

	q->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(q->page_pool)) {
		err = PTR_ERR(q->page_pool);
		q->page_pool = NULL;
		return err;
	}

	err = xdp_rxq_info_reg(&q->xdp_rxq, priv->netdev, q->qid, 0);
	if (err < 0)
		goto err_free_pp;

	err = xdp_rxq_info_reg_mem_model(&q->xdp_rxq, MEM_TYPE_PAGE_POOL,
					 q->page_pool);
	if (err)
		goto err_unregister_rxq;

	return 0;

err_unregister_rxq:
	xdp_rxq_info_unreg(&q->xdp_rxq);
err_free_pp:
	page_pool_destroy(q->page_pool);
	q->page_pool = NULL;
	return err;
}

static int onic_init_rx_queue(struct onic_private *priv, u16 qid)
{
	const u8 desc_rngcnt_idx = 0;
	const u8 cmpl_rngcnt_idx = 0;
	/*
	 * Pick QDMA C2H buffer size and page order based on MTU.
	 * c2h_bufsz_pool: {4096,256,512,1024,2048,3968,4096,4096,
	 *                   4096,4096,4096,4096,4096,8192,9018,16384}
	 * The DMA target starts at XDP_PACKET_HEADROOM inside the page,
	 * so the page must be large enough for headroom + max frame.
	 */
	const u32 max_frame = priv->netdev->mtu + ETH_HLEN;
	const u8 bufsz_idx = (max_frame > 4096) ? 14 : 0; /* 9018 or 4096 */
	const int page_order = (max_frame + XDP_PACKET_HEADROOM > PAGE_SIZE) ? 2 : 0;
	struct net_device *dev = priv->netdev;
	struct onic_rx_queue *q;
	struct onic_ring *ring;
	struct onic_qdma_c2h_param param;
	u16 vid;
	u32 size, real_count;
	int i, rv;

	if (priv->rx_queue[qid]) {
		onic_netdev_dbg(ONIC_DBG_INIT, dev,
				"Re-initializing RX queue %d", qid);
		onic_clear_rx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct onic_rx_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	/* evenly assign to RX queues available vectors */
	vid = qid % priv->num_q_vectors;

	q->netdev = dev;
	q->vector = priv->q_vector[vid];
	q->qid = qid;

	q->xdp_prog = priv->xdp_prog;

	/* allocate DMA memory for RX descriptor ring */
	ring = &q->desc_ring;
	ring->count = onic_ring_count(desc_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	/* initialize RX buffers */
	q->buffer =
		kcalloc(real_count, sizeof(struct onic_rx_buffer), GFP_KERNEL);
	if (!q->buffer) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}

	rv = onic_create_page_pool(priv, q, real_count, page_order);
	if (rv < 0)
		goto clear_rx_queue;
	

	for (i = 0; i < real_count; ++i) {
		struct page *pg = page_pool_dev_alloc_pages(q->page_pool);

		if (!pg) {
			netdev_err(dev, "page_pool_dev_alloc_pages failed at %d", i);
			rv = -ENOMEM;
			goto clear_rx_queue;
		}

		q->buffer[i].pg = pg;
		q->buffer[i].offset = XDP_PACKET_HEADROOM;
	}

	/* map pages and initialize descriptors */
	for (i = 0; i < real_count; ++i) {
		u8 *desc_ptr = ring->desc + QDMA_C2H_ST_DESC_SIZE * i;
		struct qdma_c2h_st_desc desc;
		struct page *pg = q->buffer[i].pg;
		unsigned int offset = q->buffer[i].offset;

		desc.dst_addr = page_pool_get_dma_addr(pg) + offset;
		qdma_pack_c2h_st_desc(desc_ptr, &desc);
	}

	/* allocate DMA memory for completion ring */
	ring = &q->cmpl_ring;
	ring->count = onic_ring_count(cmpl_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_CMPL_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,1,0)
	netif_napi_add(dev, &q->napi, onic_rx_poll);
#else
	netif_napi_add(dev, &q->napi, onic_rx_poll, 64);
#endif
	napi_enable(&q->napi);

	/* initialize QDMA C2H queue */
	param.bufsz_idx = bufsz_idx;
	param.desc_rngcnt_idx = desc_rngcnt_idx;
	param.cmpl_rngcnt_idx = cmpl_rngcnt_idx;
	param.cmpl_desc_sz = 0;
	param.desc_dma_addr = q->desc_ring.dma_addr;
	param.cmpl_dma_addr = q->cmpl_ring.dma_addr;
	param.vid = vid;
	onic_netdev_dbg(ONIC_DBG_INIT, dev,
			"RX queue %u: bufsz_idx %u desc_rng %u cmpl_rng %u vid %d",
			qid, bufsz_idx, desc_rngcnt_idx, cmpl_rngcnt_idx, vid);

	rv = onic_qdma_init_rx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_rx_queue;

	/* fill RX descriptor ring with a few descriptors */
	q->desc_ring.next_to_use = ONIC_RX_DESC_STEP;
	onic_set_rx_head(priv->hw.qdma, qid, q->desc_ring.next_to_use);
	onic_set_completion_tail(priv->hw.qdma, qid, 0, 1);

	priv->rx_queue[qid] = q;
	return 0;

clear_rx_queue:
	onic_clear_rx_queue(priv, qid);
	return rv;
}

static int onic_init_tx_resource(struct onic_private *priv)
{
	struct net_device *dev = priv->netdev;
	int qid, rv;

	for (qid = 0; qid < priv->num_tx_queues; ++qid) {
		rv = onic_init_tx_queue(priv, qid);
		if (!rv)
			continue;

		netdev_err(dev, "onic_init_tx_queue %d, err = %d", qid, rv);
		goto clear_tx_resource;
	}

	return 0;

clear_tx_resource:
	while (qid--)
		onic_clear_tx_queue(priv, qid);
	return rv;
}

static int onic_init_rx_resource(struct onic_private *priv)
{
	struct net_device *dev = priv->netdev;
	int qid, rv;

	for (qid = 0; qid < priv->num_rx_queues; ++qid) {
		rv = onic_init_rx_queue(priv, qid);
		if (!rv)
			continue;

		netdev_err(dev, "onic_init_rx_queue %d, err = %d", qid, rv);
		goto clear_rx_resource;
	}

	return 0;

clear_rx_resource:
	while (qid--)
		onic_clear_rx_queue(priv, qid);
	return rv;
}

int onic_open_netdev(struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	int rv;

	rv = onic_init_tx_resource(priv);
	if (rv < 0)
		goto stop_netdev;

	rv = onic_init_rx_resource(priv);
	if (rv < 0)
		goto stop_netdev;

	netif_tx_start_all_queues(dev);

	/* Only assert carrier if the physical link is already up.
	 * The user IRQ (link-change) will call carrier_on later if the
	 * cable is plugged in after the interface is opened. */
	{
		struct onic_hardware *hw = &priv->hw;
		u16 func_id = PCI_FUNC(priv->pdev->devfn);
		u8 cmac_id = (func_id < hw->num_cmacs) ? func_id : 0;
		u32 rx_status = onic_read_reg(hw,
					      CMAC_OFFSET_STAT_RX_STATUS(cmac_id));
		if (rx_status & 0x1)
			netif_carrier_on(dev);
	}
	return 0;

stop_netdev:
	onic_stop_netdev(dev);
	return rv;
}

int onic_stop_netdev(struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	struct onic_hardware *hw = &priv->hw;
	int qid, i;

	/* Disable CMAC RX so new frames stop entering the QDMA C2H pipeline.
	 *
	 * During rmmod, onic_remove() already disabled CMAC RX early (before
	 * freeing IRQs) and set ONIC_FLAG_CMAC_RX_DISABLED.  Skip the
	 * duplicate disable in that case.  For a normal "ip link set down"
	 * (no rmmod), we still need to disable here. */
	if (!test_bit(ONIC_FLAG_CMAC_RX_DISABLED, priv->flags)) {
		if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags)) {
			for (i = 0; i < hw->num_cmacs; i++)
				onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(i), 0x0);
		} else {
			u16 func_id = PCI_FUNC(priv->pdev->devfn);

			if (func_id < hw->num_cmacs)
				onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(func_id), 0x0);
		}

		/* Let the RTL drain shim inject TLAST on any in-flight frame.
		 * The shim needs ~50 ns + pipeline propagation; 10 us generous. */
		udelay(10);
	}

	/* stop sending */
	netif_carrier_off(dev);
	netif_tx_stop_all_queues(dev);

	for (qid = 0; qid < priv->num_tx_queues; ++qid)
		onic_clear_tx_queue(priv, qid);
	for (qid = 0; qid < priv->num_rx_queues; ++qid)
		onic_clear_rx_queue(priv, qid);

	return 0;
}

netdev_tx_t onic_xmit_frame(struct sk_buff *skb, struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	struct onic_tx_queue *q;
	struct onic_ring *ring;
	struct qdma_h2c_st_desc desc;
	u16 qid = skb->queue_mapping;
	dma_addr_t dma_addr;
	u8 *desc_ptr;
	int rv;
	struct rtnl_link_stats64 *pcpu_stats_pointer;
	pcpu_stats_pointer = this_cpu_ptr(priv->netdev_stats);
	q = priv->tx_queue[qid];
	ring = &q->ring;

	onic_tx_clean(q);

	if (unlikely(onic_ring_full(ring)))
		return NETDEV_TX_BUSY;

	rv = skb_put_padto(skb, ETH_ZLEN);
	if (rv < 0) {
		netdev_err(dev, "skb_put_padto failed, err = %d", rv);
		return NETDEV_TX_OK;
	}

	dma_addr = dma_map_single(&priv->pdev->dev, skb->data, skb->len,
				  DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&priv->pdev->dev, dma_addr))) {
		dev_kfree_skb(skb);
		pcpu_stats_pointer->tx_dropped++;
		pcpu_stats_pointer->tx_errors++;
		return NETDEV_TX_OK;
	}

	desc_ptr = ring->desc + QDMA_H2C_ST_DESC_SIZE * ring->next_to_use;
	desc.len = skb->len;
	desc.src_addr = dma_addr;
	desc.metadata = skb->len;
	qdma_pack_h2c_st_desc(desc_ptr, &desc);

	q->buffer[ring->next_to_use].type = ONIC_TX_SKB;
	q->buffer[ring->next_to_use].skb = skb;
	q->buffer[ring->next_to_use].dma_addr = dma_addr;
	q->buffer[ring->next_to_use].len = skb->len;

	pcpu_stats_pointer->tx_packets++;
	pcpu_stats_pointer->tx_bytes += skb->len;

	onic_ring_increment_head(ring);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	if (onic_ring_full(ring) || !netdev_xmit_more()) {
#elif defined(RHEL_RELEASE_CODE)
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8, 1)
        if (onic_ring_full(ring) || !netdev_xmit_more()) {
#endif
#else
	if (onic_ring_full(ring) || !skb->xmit_more) {
#endif
		wmb();
		onic_set_tx_head(priv->hw.qdma, qid, ring->next_to_use);
	}

	return NETDEV_TX_OK;
}

int onic_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *saddr = addr;
	u8 *dev_addr = saddr->sa_data;
	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	netdev_info(dev, "Set MAC address to %02x:%02x:%02x:%02x:%02x:%02x",
			dev_addr[0], dev_addr[1], dev_addr[2],
			dev_addr[3], dev_addr[4], dev_addr[5]);
	eth_hw_addr_set(dev, dev_addr);
	return 0;
}

int onic_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return 0;
}

int onic_change_mtu(struct net_device *dev, int mtu)
{
	bool running = netif_running(dev);
	int rv = 0;

	netdev_info(dev, "Changing MTU from %d to %d", dev->mtu, mtu);

	if (running)
		onic_stop_netdev(dev);

	dev->mtu = mtu;

	if (running) {
		rv = onic_open_netdev(dev);
		if (rv)
			netdev_err(dev, "Failed to reopen after MTU change: %d", rv);
	}
	return rv;
}

inline void onic_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct onic_private *priv = netdev_priv(dev);
	struct rtnl_link_stats64 *pcpu_ptr;
	struct rtnl_link_stats64 total_stats = { };
	unsigned int cpu;
	for_each_possible_cpu(cpu) {
		pcpu_ptr = per_cpu_ptr(priv->netdev_stats, cpu);
		
		
		total_stats.rx_packets += pcpu_ptr->rx_packets;
		total_stats.rx_bytes += pcpu_ptr->rx_bytes;
		total_stats.tx_packets += pcpu_ptr->tx_packets;
		total_stats.tx_bytes += pcpu_ptr->tx_bytes;
		total_stats.tx_errors += pcpu_ptr->tx_errors;
		total_stats.tx_dropped += pcpu_ptr->tx_dropped;
	}
	
	stats->tx_packets = total_stats.tx_packets;
	stats->tx_bytes = total_stats.tx_bytes;
	stats->rx_packets = total_stats.rx_packets;
	stats->rx_bytes = total_stats.rx_bytes;
	stats->tx_dropped = total_stats.tx_dropped;
	stats->tx_errors = total_stats.tx_errors;
}


static int onic_setup_xdp_prog(struct net_device *dev, struct bpf_prog *prog) {

	struct onic_private *priv = netdev_priv(dev);
	bool running = netif_running(dev);

	bool need_reset;

	struct bpf_prog *old_prog = xchg(&priv->xdp_prog, prog);
	need_reset = (!!prog != !!old_prog);

	if (need_reset && running) {
		onic_stop_netdev(dev);
	} else {
		int i;
		for (i = 0; i < priv->num_rx_queues; i++) {
			xchg(&priv->rx_queue[i]->xdp_prog, priv->xdp_prog);
		}
	}
	if (old_prog){

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
		xdp_features_clear_redirect_target(dev);
#endif
		bpf_prog_put(old_prog);
	}

	if (!need_reset)
		return 0;

	if (running)
		onic_open_netdev(dev);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
	if (need_reset && prog)
		xdp_features_set_redirect_target(dev, false);
#endif
	return 0;
}

int onic_xdp(struct net_device *dev, struct netdev_bpf *xdp) {
	switch (xdp->command) {
		case XDP_SETUP_PROG:
			return onic_setup_xdp_prog(dev, xdp->prog);
		default:
			return -EINVAL;
	}
}

int onic_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames, u32 flags) {
	struct onic_private *priv = netdev_priv(dev);
	struct onic_ring *tx_ring;
	struct onic_tx_queue *tx_queue;
	struct netdev_queue *nq;
	int i, drops = 0, cpu;
	
	 
	cpu  = smp_processor_id();

	tx_queue =  onic_xdp_tx_queue_mapping(priv);

	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK)){
			netdev_err(dev, "Invalid flags");
		tx_queue->xdp_tx_stats.xdp_xmit_err++;
		return -EINVAL;
	}

	tx_ring = &tx_queue->ring;
	nq = netdev_get_tx_queue(tx_queue->netdev, tx_queue->qid);

	__netif_tx_lock(nq, cpu);
	for (i = 0; i < n; i++) {
		struct xdp_frame *frame = frames[i];
		int err;

		err = 0;
		err = onic_xmit_xdp_ring(priv, tx_queue, frame, true);
		if (err != ONIC_XDP_TX) {
			xdp_return_frame_rx_napi(frame);
			netdev_err(dev, "Failed to transmit frame");
			tx_queue->xdp_tx_stats.xdp_xmit_err++;
			drops++;
		} else {
			tx_queue->xdp_tx_stats.xdp_xmit++;
		}
	}

	if (flags & XDP_XMIT_FLUSH) {
		wmb();
		onic_set_tx_head(priv->hw.qdma, tx_queue->qid, tx_queue->ring.next_to_use);
	}
	__netif_tx_unlock(nq);

	return n - drops;
}

