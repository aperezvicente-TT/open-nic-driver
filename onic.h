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
#ifndef __ONIC_H__
#define __ONIC_H__

#include <linux/netdevice.h>
#include <linux/cpumask.h>
#include <linux/bpf.h>
#include <net/xdp.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/net_tstamp.h>

#include "onic_hardware.h"
#include "onic_ptp.h"

/* Debug levels controlled by module param debug_level at insmod time.
 *   0 = silent  (no debug output)
 *   1 = info    (link state, FEC changes, ring full, errors)
 *   2 = init    (queue setup, re-init, configuration details)
 *   3 = data    (per-packet: rx_poll, completions, ring pointers, QDMA regs)
 */
#define ONIC_DBG_INFO	1
#define ONIC_DBG_INIT	2
#define ONIC_DBG_DATA	3

extern int onic_debug_level;

#define onic_netdev_dbg(lvl, netdev, fmt, ...)				\
	do {								\
		if (onic_debug_level >= (lvl)) {				\
			if ((lvl) >= ONIC_DBG_DATA) {			\
				if (net_ratelimit())			\
					netdev_info(netdev,		\
						"[DBG%d] " fmt,		\
						(lvl), ##__VA_ARGS__);	\
			} else {					\
				netdev_info(netdev, "[DBG%d] " fmt,	\
					    (lvl), ##__VA_ARGS__);	\
			}						\
		}							\
	} while (0)

#define onic_dev_dbg(lvl, dev, fmt, ...)				\
	do {								\
		if (onic_debug_level >= (lvl)) {				\
			if ((lvl) >= ONIC_DBG_DATA) {			\
				if (net_ratelimit())			\
					dev_info(dev,			\
						"[DBG%d] " fmt,		\
						(lvl), ##__VA_ARGS__);	\
			} else {					\
				dev_info(dev, "[DBG%d] " fmt,		\
					 (lvl), ##__VA_ARGS__);		\
			}						\
		}							\
	} while (0)

#define ONIC_MAX_QUEUES			64

/* state bits */
#define ONIC_ERROR_INTR			0
#define ONIC_USER_INTR			1

/* flag bits */
#define ONIC_FLAG_MASTER_PF		0
#define ONIC_FLAG_CMAC_RX_DISABLED	1

/* XDP */
#define ONIC_XDP_PASS    	BIT(0)	
#define ONIC_XDP_CONSUMED	BIT(1)
#define ONIC_XDP_TX       	BIT(2)
#define ONIC_XDP_REDIR    	BIT(3)

enum onic_tx_buf_type {
	ONIC_TX_SKB = BIT(0),
	ONIC_TX_XDPF = BIT(1),
	ONIC_TX_XDPF_XMIT = BIT(2),
};

struct onic_tx_buffer {
	enum onic_tx_buf_type type;
	union {
		struct sk_buff *skb;
		struct xdp_frame *xdpf;
	};
	dma_addr_t dma_addr;
	u32 len;
	u64 time_stamp;
};

struct onic_rx_buffer {
	struct page *pg;
	unsigned int offset;
	u64 time_stamp;
};

/**
 * struct onic_ring - generic ring structure
 **/
struct onic_ring {
	u16 count;		/* number of descriptors */
	u8 *desc;		/* base address for descriptors */
	u8 *wb;			/* descriptor writeback */
	dma_addr_t dma_addr;	/* DMA address for descriptors */

	u16 next_to_use;
	u16 next_to_clean;
	u8 color;
};

struct onic_tx_queue {
	struct net_device *netdev;
	u16 qid;
	DECLARE_BITMAP(state, 32);

	struct onic_tx_buffer *buffer;
	struct onic_ring ring;
	struct onic_q_vector *vector;

	struct {
		u64	xdp_xmit;
		u64	xdp_xmit_err;
	} xdp_tx_stats;
};

struct onic_rx_queue {
	struct net_device *netdev;
	u16 qid;

	struct onic_rx_buffer *buffer;
	struct onic_ring desc_ring;
	struct onic_ring cmpl_ring;
	struct onic_q_vector *vector;

	struct napi_struct napi;
	struct bpf_prog *xdp_prog;
	struct xdp_rxq_info xdp_rxq;
	struct page_pool *page_pool;

	struct {
		u64 xdp_redirect;
		u64 xdp_pass;
		u64 xdp_drop;
		u64	xdp_tx;
		u64	xdp_tx_err;
	} xdp_rx_stats;
	
};

struct onic_q_vector {
	u16 vid;
	struct onic_private *priv;
	struct cpumask affinity_mask;
	int numa_node;
};


/**
 * struct onic_private - OpenNIC driver private data
 **/
struct onic_private {
	struct list_head dev_list;

	struct pci_dev *pdev;
	DECLARE_BITMAP(state, 32);
	DECLARE_BITMAP(flags, 32);

        int RS_FEC;

	u32 msg_enable;

	u16 num_q_vectors;
	u16 num_tx_queues;
	u16 num_rx_queues;

	struct net_device *netdev;
	struct bpf_prog *xdp_prog;
	struct rtnl_link_stats64 *netdev_stats;
	spinlock_t tx_lock;
	spinlock_t rx_lock;

	struct onic_q_vector *q_vector[ONIC_MAX_QUEUES];
	struct onic_tx_queue *tx_queue[ONIC_MAX_QUEUES];
	struct onic_rx_queue *rx_queue[ONIC_MAX_QUEUES];

	struct onic_hardware hw;

	unsigned long cmac_last_enable_jiffies[ONIC_MAX_CMACS]; /* IRQ debounce */

	/* Workqueue item for link-recovery (cable replug).  Scheduled from
	 * onic_user_thread_fn so the IRQ thread returns immediately and
	 * free_irq() doesn't block.  The work item does a full dev_close +
	 * dev_open to reinitialise QDMA C2H contexts that may have stalled
	 * on an in-flight AXI-S transfer when the CMAC was reset. */
	struct work_struct link_recovery_work;
	u32 link_recovery_cmac_mask; /* bitmask of CMAC indices to re-enable */

	/* Periodic link watchdog — polls CMAC STAT_RX_STATUS every second to
	 * detect carrier transitions.  The link-recovery IRQ path handles
	 * cable-replug events, but the initial CMAC alignment after open does
	 * not reliably generate an IRQ, so we need polling as well. */
	struct delayed_work link_watchdog_work;

	/* Deferred re-arm for the QDMA error interrupt.  Rather than
	 * re-arming immediately after a fatal LEN_MISMATCH (which causes an
	 * instant double-fire if another glitch packet is in-flight), we wait
	 * ERROR_REARM_DELAY_MS before writing ARM=1 so QDMA's pipeline drains. */
	struct delayed_work error_rearm_work;

	/* PTP hardware timestamping support */
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_info;
	struct hwtstamp_config tstamp_config;
	spinlock_t ptp_lock;

	/* TX PTP timestamp tag management */
	struct onic_ptp_tx_pending ptp_tx_pending[ONIC_PTP_TX_PENDING_MAX];
	u16 ptp_next_tag;
	spinlock_t ptp_tx_lock;
	struct delayed_work ptp_tx_work;
};

#endif
