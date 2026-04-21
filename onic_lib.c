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
#include <linux/pci.h>
#include <linux/netdevice.h>

#include "onic_lib.h"
#include "onic_register.h"
#include "onic_hardware.h"
#include "onic.h"

#define ONIC_MAX_IRQ_NAME 32

extern int onic_poll(struct napi_struct *napi, int budget);

static irqreturn_t onic_q_handler(int irq, void *dev_id)
{
	struct onic_q_vector *vec = dev_id;
	struct onic_private *priv = vec->priv;
	u16 qid = vec->vid;
	struct onic_rx_queue *rxq = priv->rx_queue[qid];
	bool debug = 0;
	if (debug) dev_info(&priv->pdev->dev, "queue irq");

	/* rxq is NULL if the interrupt fires before the interface is brought
	 * up (ndo_open), or from a hardware-pending event on re-insmod after
	 * high-rate traffic.  Nothing to poll; acknowledge and return. */
	if (unlikely(!rxq))
		return IRQ_HANDLED;

	//napi_schedule(&rxq->napi);
	napi_schedule_irqoff(&rxq->napi);
	return IRQ_HANDLED;
}

static irqreturn_t onic_user_handler(int irq, void *dev_id)
{
	struct onic_private *priv = dev_id;
	dev_info(&priv->pdev->dev, "user irq");
	return IRQ_WAKE_THREAD;
}

/**
 * onic_link_recovery_work - workqueue handler for cable-replug recovery
 *
 * The threaded IRQ handler records which CMACs need recovery and returns
 * immediately.  This work item runs in a kernel thread and re-programs the
 * CMAC configuration registers (RX/TX enable, RS-FEC, flow control) without
 * asserting cmac_rstn.  Skipping the shell reset prevents the AXI-S bridge
 * from being disrupted mid-packet, which would otherwise stall the QDMA C2H
 * engine.  A NAPI kick is sufficient to re-arm the completion interrupt.
 */
static void onic_link_recovery_work(struct work_struct *work)
{
	struct onic_private *priv =
		container_of(work, struct onic_private, link_recovery_work);
	struct onic_hardware *hw = &priv->hw;
	u32 cmac_mask;
	int i, q;

	/* Snapshot and clear the pending mask atomically so a concurrent IRQ
	 * can re-arm while we run.  xchg is overkill for a single word but
	 * makes the intent clear and avoids a missing event. */
	cmac_mask = xchg(&priv->link_recovery_cmac_mask, 0);
	if (!cmac_mask)
		return;

	rtnl_lock();

	for (i = 0; i < hw->num_cmacs; i++) {
		struct pci_dev *pf_pdev;
		struct onic_private *pf_priv;
		struct net_device *netdev;

		if (!(cmac_mask & BIT(i)))
			continue;

		if (i == 0) {
			/* CMAC0 is always on the master PF (func 0) */
			pf_pdev = pci_get_domain_bus_and_slot(
				pci_domain_nr(priv->pdev->bus),
				priv->pdev->bus->number,
				PCI_DEVFN(PCI_SLOT(priv->pdev->devfn), 0));
			if (!pf_pdev)
				continue;
			pf_priv = pci_get_drvdata(pf_pdev);
			if (!pf_priv) {
				pci_dev_put(pf_pdev);
				continue;
			}
		} else {
			/* Single-PF dual-CMAC: CMAC1 served by secondary net_device.
			 * Use the peer pointer instead of a PCIe slot lookup. */
			if (!priv->peer)
				continue;
			pf_priv = priv->peer;
			pf_pdev = pf_priv->pdev;
			pci_dev_get(pf_pdev);
		}
		netdev = pf_priv->netdev;

		if (!netif_running(netdev)) {
			dev_info(&pf_pdev->dev,
				 "CMAC%d link change: interface not running, skipping\n", i);
			pci_dev_put(pf_pdev);
			continue;
		}

		/* Read actual CMAC link state to distinguish UP from DOWN.
		 * STAT_RX_STATUS bit 0 = stat_rx_aligned. */
		if (onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(i)) & 0x1) {
			/* Link is UP -- re-enable CMAC and assert carrier */
			if (time_after_eq(jiffies,
					  priv->cmac_last_enable_jiffies[i] + 5 * HZ)) {
				dev_info(&pf_pdev->dev,
					 "CMAC%d link up — reconfiguring MAC (no reset)\n", i);
				onic_enable_cmac(hw, i, false);
				priv->cmac_last_enable_jiffies[i] = jiffies;
			}
			if (!netif_carrier_ok(netdev)) {
				netif_info(pf_priv, link, netdev,
					   "Link up\n");
				netif_carrier_on(netdev);
			}
			for (q = 0; q < pf_priv->num_rx_queues; q++) {
				if (pf_priv->rx_queue[q])
					napi_schedule(&pf_priv->rx_queue[q]->napi);
			}
		} else {
			/* Link is DOWN -- deassert carrier */
			if (netif_carrier_ok(netdev)) {
				netif_info(pf_priv, link, netdev,
					   "Link down\n");
				netif_carrier_off(netdev);
			}
		}

		pci_dev_put(pf_pdev);
	}

	rtnl_unlock();
}

void onic_init_link_recovery(struct onic_private *priv)
{
	INIT_WORK(&priv->link_recovery_work, onic_link_recovery_work);
}

static irqreturn_t onic_user_thread_fn(int irq, void *dev_id)
{
	struct onic_private *priv = dev_id;
	struct onic_hardware *hw = &priv->hw;
	u32 status;
	int i;

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return IRQ_HANDLED;

	/* Abort immediately if the device is being torn down.  Any BAR2
	 * access (including the read below) is safe only while the device is
	 * open; checking netif_running() here ensures we don't touch hardware
	 * after dev_close() has been called. */
	if (!netif_running(priv->netdev))
		return IRQ_HANDLED;

	/* Read and W1C-clear the link-up sticky bits */
	status = onic_read_reg(hw, SYSCFG_OFFSET_LINK_IRQ_STATUS);
	if (!status)
		return IRQ_HANDLED;
	onic_write_reg(hw, SYSCFG_OFFSET_LINK_IRQ_STATUS, status);

	/* Record which CMACs need recovery and hand off to the work queue.
	 * The threaded handler must return quickly so that free_irq() during
	 * rmmod does not block.  The actual onic_enable_cmac() polling loop
	 * and dev_close/dev_open run in the work item instead. */
	for (i = 0; i < hw->num_cmacs; i++) {
		if (status & BIT(i))
			set_bit(i, (unsigned long *)&priv->link_recovery_cmac_mask);
	}
	schedule_work(&priv->link_recovery_work);

	return IRQ_HANDLED;
}

/* Delay between a fatal QDMA error and re-arming the error interrupt.
 * 2 ms gives ~2000 packets at 1 Mpps to flush through QDMA's C2H pipeline,
 * ensuring the glitch frame that caused LEN_MISMATCH is gone before ARM=1
 * is written.  Without this delay, immediate re-arm causes a double-fire
 * (and potential storm) if another glitch frame arrives in the ~1–5 µs
 * window between W1C and ARM. */
#define ERROR_REARM_DELAY_MS	2

static void onic_error_rearm_work_fn(struct work_struct *work)
{
	struct onic_private *priv =
		container_of(to_delayed_work(work), struct onic_private,
			     error_rearm_work);
	u16 vid = priv->num_q_vectors + 1;
	int q;

	/* Re-arm the QDMA error interrupt.  The W1C-clear inside
	 * onic_qdma_init_error_interrupt clears any latched error bits
	 * (LEN_MISMATCH, CMPL_INV_Q_ERR, …) before ARM=1 is written,
	 * preventing an immediate re-fire from a stale status bit.
	 *
	 * NOTE: we intentionally do NOT call dev_close/dev_open here.
	 * onic_stop_netdev writes CONF_RX_1=0 (CMAC rx_enable deassert).
	 * If a frame is in flight when rx_enable goes low, the Xilinx CMAC
	 * IP cuts the AXI-S output without inserting TLAST.  QDMA C2H then
	 * waits forever for TLAST, page_pool_destroy never completes, and
	 * the kernel hangs.  The correct fix is an RTL-level AXI-S drain
	 * (TLAST insertion on rx_enable deassert) in the OpenNIC shell. */
	onic_qdma_init_error_interrupt(priv->hw.qdma, vid);

	/* Kick all RX NAPI instances to re-sync with hardware state */
	for (q = 0; q < priv->num_rx_queues; q++) {
		if (priv->rx_queue[q])
			napi_schedule(&priv->rx_queue[q]->napi);
	}
}

void onic_init_error_rearm(struct onic_private *priv)
{
	INIT_DELAYED_WORK(&priv->error_rearm_work, onic_error_rearm_work_fn);
}

/* ---- Link watchdog --------------------------------------------------- */

#define LINK_WATCHDOG_INTERVAL_MS	1000

static void onic_link_watchdog_work_fn(struct work_struct *work)
{
	struct onic_private *priv =
		container_of(to_delayed_work(work), struct onic_private,
			     link_watchdog_work);
	struct onic_hardware *hw = &priv->hw;
	struct net_device *netdev = priv->netdev;
	u8 cmac_id = priv->cmac_id;
	u32 rx_status;
	bool link_up;

	if (!netif_running(netdev))
		return;

	/* Double-read to flush any previously latched value */
	onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_id));
	rx_status = onic_read_reg(hw, CMAC_OFFSET_STAT_RX_STATUS(cmac_id));
	link_up = (rx_status & 0x1) != 0;

	if (link_up && !netif_carrier_ok(netdev)) {
		netif_info(priv, link, netdev, "Link up\n");
		netif_carrier_on(netdev);
	} else if (!link_up && netif_carrier_ok(netdev)) {
		netif_info(priv, link, netdev, "Link down\n");
		netif_carrier_off(netdev);
	}

	/* Reschedule while the interface is up */
	if (netif_running(netdev))
		schedule_delayed_work(&priv->link_watchdog_work,
				      msecs_to_jiffies(LINK_WATCHDOG_INTERVAL_MS));
}

void onic_start_link_watchdog(struct onic_private *priv)
{
	INIT_DELAYED_WORK(&priv->link_watchdog_work,
			  onic_link_watchdog_work_fn);
	schedule_delayed_work(&priv->link_watchdog_work,
			      msecs_to_jiffies(LINK_WATCHDOG_INTERVAL_MS));
}

void onic_stop_link_watchdog(struct onic_private *priv)
{
	cancel_delayed_work_sync(&priv->link_watchdog_work);
}

static irqreturn_t onic_error_handler(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t onic_error_thread_fn(int irq, void *dev_id)
{
	struct onic_private *priv = dev_id;

	dev_err(&priv->pdev->dev,
		"Error IRQ (BH) fired on Funtion#%05x: vector=%d\n",
		PCI_FUNC(priv->pdev->devfn), irq);

	/* Dump error registers BEFORE the W1C clear so we capture what fired */
	onic_qdma_dump_error_regs(priv->hw.qdma);

	/* Do NOT re-arm synchronously here.  At line-rate a second glitch
	 * packet can arrive in the ~1–5 µs between W1C and ARM=1, causing an
	 * immediate double-fire.  Schedule a delayed re-arm instead so the
	 * pipeline drains first.  If work is already pending (rapid-fire
	 * scenario), schedule_delayed_work is a no-op. */
	schedule_delayed_work(&priv->error_rearm_work,
			      msecs_to_jiffies(ERROR_REARM_DELAY_MS));

	return IRQ_HANDLED;
}

/**
 * onic_init_q_vector - clear a queue vector
 * @priv: pointer to driver private data
 * @vid: vector ID
 **/
static void onic_clear_q_vector(struct onic_private *priv, u16 vid)
{
	struct onic_q_vector *vec = priv->q_vector[vid];

	if (!vec)
		return;
	free_irq(pci_irq_vector(priv->pdev, priv->vec_base + vid), vec);
	kfree(vec);
}

/**
 * onic_init_q_vector - initialize a queue vector
 * @priv: pointer to driver private data
 * @vid: vector ID
 *
 * This function does the following: allocate coherent DMA region for interrupt
 * aggregation ring, register NAPI instances, and initialize relevant QDMA
 * interrupt registers.  Return 0 on success, negative on failure
 **/
static int onic_init_q_vector(struct onic_private *priv, u16 vid)
{
	struct pci_dev *pdev = priv->pdev;
	struct onic_q_vector *vec;
	char* name = (char*)vmalloc(sizeof(char)*ONIC_MAX_IRQ_NAME);
	int rv;

	vec = kzalloc(sizeof(struct onic_q_vector), GFP_KERNEL);
	if (!vec)
		return -ENOMEM;
	vec->priv = priv;
	vec->vid = vid;

	snprintf(name, ONIC_MAX_IRQ_NAME, "%s-%d", priv->netdev->name, vid);
	rv = request_irq(pci_irq_vector(pdev, priv->vec_base + vid), onic_q_handler,
			 0, name, vec);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup queue vector %s", name);
		return rv;
	}

	/* setup affinity mask and node */
	/* cpu = vid % num_online_cpus(); */
	/* cpumask_set_cpu(cpu, &vec->affinity_mask); */
	/* vec->numa_node = node; */

	dev_info(&pdev->dev, "Setup IRQ vector %d with name %s",
		 pci_irq_vector(pdev, vid), name);
	priv->q_vector[vid] = vec;

	return 0;
}

/**
 * onic_acquire_msix_vectors - acquire MSI-X vectors
 * @priv: pointer to driver private data
 *
 * Attempt to acquire a suitable range of MSI-X vector interrupts.  Return 0 on
 * success, and negative on error.
 *
 * For every PF, a minimum of 2 vectors are required for proper operation, one
 * for queue interrupt and one for user interreupt.  The master PF requires one
 * additional vector for global error interrupt.
 **/
static int onic_acquire_msix_vectors(struct onic_private *priv)
{
	int vectors, non_q_vectors, q_per_cmac;

	non_q_vectors = 1; /* user interrupt */
	if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		non_q_vectors++; /* + error interrupt */

	/* For master PF with dual-CMAC hardware, request 2x queue vectors so
	 * the secondary net_device can use the upper half (vec_base offset). */
	q_per_cmac = ONIC_MAX_QUEUES;
	if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		vectors = 2 * q_per_cmac + non_q_vectors;
	else
		vectors = q_per_cmac + non_q_vectors;

	vectors = pci_alloc_irq_vectors(priv->pdev, non_q_vectors + 1, vectors,
					PCI_IRQ_MSIX);
	if (vectors < 0) {
		dev_err(&priv->pdev->dev,
			"Failed to allocate vectors in the range [%d, %d]",
			non_q_vectors + 1, vectors);
		return vectors;
	}

	/* For dual-CMAC master PF, reserve half for secondary (vec_base split).
	 * If hardware advertises enough (2*q_per_cmac + non_q) each CMAC gets
	 * q_per_cmac; if not (e.g. MSIX_CAP=32), they share equally. */
	/* Master PF always splits half for secondary (hw.num_cmacs not yet set
	 * here — onic_init_hardware runs after onic_init_capacity). */
	if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags)) {
		int avail = vectors - non_q_vectors;
		priv->num_q_vectors = min_t(u16, avail / 2, (u16)q_per_cmac);
	} else {
		priv->num_q_vectors = min_t(u16, vectors - non_q_vectors, (u16)q_per_cmac);
	}

	dev_info(&priv->pdev->dev, "Allocated %d MSI-X vectors, %d queue vectors\n",
		 vectors, priv->num_q_vectors);
	return 0;
}

/**
 * onic_set_num_queues - calculate the number of active queues
 * @priv: pointer to driver private data
 *
 * The number of active queues equals to either the number of queue vectors, or
 * the real number of queues in the associated net device, whichever is smaller.
 **/
static void onic_set_num_queues(struct onic_private *priv)
{
	struct net_device *dev = priv->netdev;

	priv->num_tx_queues =
		min_t(u16, priv->num_q_vectors, dev->real_num_tx_queues);
	priv->num_rx_queues =
		min_t(u16, priv->num_q_vectors, dev->real_num_rx_queues);
}

int onic_init_capacity(struct onic_private *priv)
{
	int rv;

	rv = onic_acquire_msix_vectors(priv);
	if (rv < 0)
		return rv;
	onic_set_num_queues(priv);
	return 0;
}

void onic_clear_capacity(struct onic_private *priv)
{
	priv->num_tx_queues = 0;
	priv->num_rx_queues = 0;
	priv->num_q_vectors = 0;
	if (priv->cmac_id == 0) /* secondary shares MSI-X with primary */
		pci_free_irq_vectors(priv->pdev);
}

int onic_init_interrupt(struct onic_private *priv)
{
	struct pci_dev *pdev = priv->pdev;
	int vid, rv;

	for (vid = 0; vid < priv->num_q_vectors; ++vid) {
		rv = onic_init_q_vector(priv, vid);
		if (rv < 0)
			goto clear_interrupt;
	}

	/* User and error interrupts belong to the primary (CMAC0) net_device only.
	 * The secondary (CMAC1) shares the primary's link recovery IRQ path. */
	if (priv->cmac_id != 0)
		return 0;

	rv = request_threaded_irq(pci_irq_vector(pdev, priv->vec_base + vid),
				  onic_user_handler, onic_user_thread_fn,
				  0, "onic-user", priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup user interrupt");
		goto clear_interrupt;
	}
	set_bit(ONIC_USER_INTR, priv->state);

	if (!test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return 0;

	vid++;
	rv = request_threaded_irq(pci_irq_vector(pdev, priv->vec_base + vid),
				  onic_error_handler, onic_error_thread_fn,
				  0, "onic-error", priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup error interrupt");
		goto clear_interrupt;
	}
	onic_qdma_init_error_interrupt(priv->hw.qdma, priv->vec_base + vid);
	set_bit(ONIC_ERROR_INTR, priv->state);

	return 0;

clear_interrupt:
	onic_clear_interrupt(priv);
	return rv;
}

void onic_clear_interrupt(struct onic_private *priv)
{
	u8 master_pf = test_bit(ONIC_FLAG_MASTER_PF, priv->flags);
	int vid;

	/* User and error interrupts belong to the primary (CMAC0) net_device. */
	if (priv->cmac_id == 0) {
		if (master_pf && test_bit(ONIC_ERROR_INTR, priv->state)) {
			vid = priv->vec_base + priv->num_q_vectors + 1;
			/* free_irq first: prevents new delayed work after cancel */
			free_irq(pci_irq_vector(priv->pdev, vid), priv);
			cancel_delayed_work_sync(&priv->error_rearm_work);
			onic_qdma_clear_error_interrupt(priv->hw.qdma);
		}
		if (test_bit(ONIC_USER_INTR, priv->state)) {
			vid = priv->vec_base + priv->num_q_vectors;
			free_irq(pci_irq_vector(priv->pdev, vid), priv);
		}
	}

	for (vid = priv->num_q_vectors - 1; vid >= 0; vid--)
		onic_clear_q_vector(priv, vid);
}
