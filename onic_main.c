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
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/moduleparam.h>
#include <linux/bpf.h>
#include <linux/dmi.h>

#include <linux/delay.h>

#include "onic.h"
#include "onic_hardware.h"
#include "onic_lib.h"
#include "onic_register.h"
#include "onic_common.h"
#include "onic_netdev.h"
#include "onic_ptp.h"
#include "qdma_access/qdma_device.h"
#include "qdma_access/qdma_context.h"

#undef CMS_SUPPORT    /* Need CMS IP in the design @320000 offset */

#ifndef ONIC_VF
#define DRV_STR "OpenNIC Linux Kernel Driver"
char onic_drv_name[] = "onic";
#else
#define DRV_STR "OpenNIC Linux Kernel Driver (VF)"
char onic_drv_name[] = "open-nic-vf";
#endif

#define DRV_VER "0.21"
const char onic_drv_str[] = DRV_STR;
const char onic_drv_ver[] = DRV_VER;

MODULE_AUTHOR("Xilinx Research Labs");
MODULE_DESCRIPTION(DRV_STR);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VER);

static int RS_FEC_ENABLED=1;
module_param(RS_FEC_ENABLED, int, 0644);

int onic_debug_level = 0;
module_param_named(debug_level, onic_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug verbosity (0=off, 1=info, 2=init, 3=data-path)");

static int host_id = -1;
module_param(host_id, int, 0444);
MODULE_PARM_DESC(host_id,
	"Host identifier (0-255) for MAC uniqueness across identical hosts. "
	"Default -1: auto-derive from DMI system UUID.");

#ifdef CMS_SUPPORT
extern int xocl_init_xmc(void);
extern void xocl_fini_xmc(void);
extern struct onic_private *onic_priv;
#endif

static const struct pci_device_id onic_pci_tbl[] = {
	/* Gen 3 PF */
	/* PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9031), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9131), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9231), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9331), },	/* PF 3 */
	/* PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9032), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9132), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9232), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9332), },	/* PF 3 */
	/* PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9034), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9134), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9234), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9334), },	/* PF 3 */
	/* PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9038), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9138), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9238), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9338), },	/* PF 3 */
	/* PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0x903f), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x913f), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x923f), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x933f), },	/* PF 3 */
	/* { PCI_DEVICE(0x10ee, 0x6a9f), }, */	     /* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x6aa0), },	/* PF 1 */

	/* Gen 4 PF */
	/* PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9041), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9141), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9241), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9341), },	/* PF 3 */
	/* PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9042), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9142), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9242), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9342), },	/* PF 3 */
	/* PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9044), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9144), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9244), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9344), },	/* PF 3 */
	/* PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9048), },	/* PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9148), },	/* PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9248), },	/* PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9348), },	/* PF 3 */

	{0,}
};

MODULE_DEVICE_TABLE(pci, onic_pci_tbl);

/**
 * Default MAC address 00:0A:35:00:00:00
 * First three octets indicate OUI (00:0A:35 for Xilinx)
 * Note that LSB of the first octet must be 0 (unicast)
 **/
static const unsigned char onic_default_dev_addr[] = {
	0x00, 0x0A, 0x35, 0x00, 0x00, 0x00
};

/**
 * onic_resolve_host_id - Determine a per-host byte for MAC address uniqueness
 *
 * When host_id is set explicitly (0-255), use that value directly.
 * Otherwise, hash the DMI system UUID (unique per motherboard) into a single
 * byte.  This ensures identical FPGAs on different hosts get different MACs
 * without any manual configuration.
 *
 * MAC layout: 00:0A:35:<host_id>:<bus>:<func>
 */
static u8 onic_resolve_host_id(struct pci_dev *pdev)
{
	const char *uuid;
	u8 hash = 0;
	int i;

	if (host_id >= 0 && host_id <= 255)
		return (u8)host_id;

	uuid = dmi_get_system_info(DMI_PRODUCT_UUID);
	if (uuid && uuid[0]) {
		for (i = 0; uuid[i]; i++)
			hash = (hash * 31) + uuid[i];
		/* Avoid 0 so explicit host_id=0 remains distinguishable */
		if (hash == 0)
			hash = 1;
		dev_info(&pdev->dev,
			 "auto host_id=%u from DMI UUID (override with modparam host_id=N)\n",
			 hash);
		return hash;
	}

	dev_warn(&pdev->dev,
		 "host_id not set and no DMI UUID — MAC may collide on identical hosts\n");
	return 0;
}

static const struct net_device_ops onic_netdev_ops = {
	.ndo_open = onic_open_netdev,
	.ndo_stop = onic_stop_netdev,
	.ndo_start_xmit = onic_xmit_frame,
	.ndo_set_mac_address = onic_set_mac_address,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	.ndo_eth_ioctl = onic_do_ioctl,
#else
	.ndo_do_ioctl = onic_do_ioctl,
#endif
	.ndo_change_mtu = onic_change_mtu,
	.ndo_get_stats64 = onic_get_stats64,
	.ndo_bpf = onic_xdp,
// For why we do this, see onic_netdev.c:onix_xdp_run
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	.ndo_xdp_xmit = onic_xdp_xmit,
#elif defined(RHEL_RELEASE_CODE)
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8, 1))
	.ndo_xdp_xmit = onic_xdp_xmit,
#endif
#endif
};

extern void onic_set_ethtool_ops(struct net_device *netdev);

/**
 * onic_probe - Probe and initialize PCI device
 * @pdev: pointer to PCI device
 * @ent: pointer to PCI device ID entries
 *
 * Return 0 on success, negative on failure
 **/
static int onic_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *netdev;
	struct onic_private *priv;
	struct sockaddr saddr;
	char dev_name[IFNAMSIZ];
	int rv;
#ifdef CMS_SUPPORT
        static int xmc_init=0;
#endif
	/* int pci_using_dac; */

	rv = pci_enable_device_mem(pdev);
	if (rv < 0) {
		dev_err(&pdev->dev, "pci_enable_device_mem, err = %d", rv);
		return rv;
	}

	/* QDMA only supports 32-bit consistent DMA for descriptor ring */
	rv = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to set DMA masks");
		goto disable_device;
	} else {
		dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	}

	rv = pci_request_mem_regions(pdev, onic_drv_name);
	if (rv < 0) {
		dev_err(&pdev->dev, "pci_request_mem_regions, err = %d", rv);
		goto disable_device;
	}

	/* enable relaxed ordering */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
	/* enable extended tag */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_EXT_TAG);
	pci_set_master(pdev);
	pci_save_state(pdev);
	pcie_set_readrq(pdev, 512);

	netdev = alloc_etherdev_mq(sizeof(struct onic_private),
				   ONIC_MAX_QUEUES);
	if (!netdev) {
		dev_err(&pdev->dev, "alloc_etherdev_mq failed");
		rv = -ENOMEM;
		goto release_pci_mem;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);
	netdev->netdev_ops = &onic_netdev_ops;
	onic_set_ethtool_ops(netdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
	xdp_set_features_flag(netdev, NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT);
#endif
	snprintf(dev_name, IFNAMSIZ, "onic%ds%df%d",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
	strscpy(netdev->name, dev_name, sizeof(netdev->name));

	memset(&saddr, 0, sizeof(struct sockaddr));
	memcpy(saddr.sa_data, onic_default_dev_addr, 6);
	saddr.sa_data[3] = onic_resolve_host_id(pdev);
	saddr.sa_data[4] = pdev->bus->number;
	saddr.sa_data[5] = PCI_FUNC(pdev->devfn);
	onic_set_mac_address(netdev, (void *)&saddr);

	priv = netdev_priv(netdev);

	memset(priv, 0, sizeof(struct onic_private));
	priv->msg_enable = NETIF_MSG_DRV | NETIF_MSG_LINK;
	priv->RS_FEC = RS_FEC_ENABLED;

	if (PCI_FUNC(pdev->devfn) == 0) {
		dev_info(&pdev->dev, "device is a master PF");
		set_bit(ONIC_FLAG_MASTER_PF, priv->flags);
	}
	priv->pdev = pdev;
	priv->netdev = netdev;
	priv->cmac_id = 0;  /* primary always owns CMAC0 */
	priv->vec_base = 0; /* primary starts at MSI-X vector 0 */
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);
	onic_init_link_recovery(priv);
	onic_init_error_rearm(priv);

	priv->netdev_stats = alloc_percpu(struct rtnl_link_stats64);
	if (!priv->netdev_stats) {
		dev_err(&pdev->dev, "error in allocating netdev_stats");
		goto free_netdev;
	}

	rv = onic_init_capacity(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_capacity, err = %d", rv);
		goto free_netdev;
	}

	rv = onic_init_hardware(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_hardware, err = %d", rv);
		goto clear_capacity;
	}

	rv = onic_init_interrupt(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_interrupt, err = %d", rv);
		goto clear_hardware;
	}

	netif_set_real_num_tx_queues(netdev, priv->num_tx_queues);
	netif_set_real_num_rx_queues(netdev, priv->num_rx_queues);

	/* Enable jumbo frame support - max_pkt_len from FPGA design is 9600 */
	netdev->min_mtu = ETH_MIN_MTU;        /* 68 bytes */
	netdev->max_mtu = 9600 - ETH_HLEN;    /* 9600 - 14 = 9586 */

	netdev->features |= NETIF_F_HIGHDMA;
	netdev->hw_features |= NETIF_F_HIGHDMA;

	rv = register_netdev(netdev);
	if (rv < 0) {
		dev_err(&pdev->dev, "register_netdev, err = %d", rv);
		goto clear_interrupt;
	}

	pci_set_drvdata(pdev, priv);
	netif_carrier_off(netdev);

	rv = onic_ptp_init(priv);
	if (rv < 0)
		dev_warn(&pdev->dev, "PTP init failed (err=%d), continuing without PTP\n", rv);

	/* For single-PF dual-CMAC builds, create a second net_device for CMAC1.
	 * The secondary shares hardware (BAR2, QDMA IP) with the primary but gets
	 * its own QDMA queue range [ONIC_MAX_QUEUES, 2*ONIC_MAX_QUEUES) and its own
	 * MSI-X vectors (vec_base = num_q_vectors + 2, after primary user+error). */
	if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags) && priv->hw.num_cmacs >= 2) {
		struct net_device *netdev2;
		struct onic_private *priv2;
		struct sockaddr saddr2;
		char dev_name2[IFNAMSIZ];
		struct qdma_dev *sq_dev;

		netdev2 = alloc_etherdev_mq(sizeof(struct onic_private), ONIC_MAX_QUEUES);
		if (!netdev2) {
			dev_warn(&pdev->dev, "failed to alloc secondary netdev, CMAC1 unavailable\n");
			goto probe_done;
		}

		SET_NETDEV_DEV(netdev2, &pdev->dev);
		netdev2->netdev_ops = &onic_netdev_ops;
		onic_set_ethtool_ops(netdev2);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
		xdp_set_features_flag(netdev2, NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT);
#endif
		snprintf(dev_name2, IFNAMSIZ, "onic%ds%df%dc1",
			 pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
		strscpy(netdev2->name, dev_name2, sizeof(netdev2->name));

		memset(&saddr2, 0, sizeof(struct sockaddr));
		memcpy(saddr2.sa_data, onic_default_dev_addr, 6);
		saddr2.sa_data[3] = onic_resolve_host_id(pdev);
		saddr2.sa_data[4] = pdev->bus->number;
		saddr2.sa_data[5] = PCI_FUNC(pdev->devfn) + 0x10; /* distinct CMAC1 MAC */
		onic_set_mac_address(netdev2, (void *)&saddr2);

		priv2 = netdev_priv(netdev2);
		memset(priv2, 0, sizeof(struct onic_private));
		priv2->pdev = pdev;
		priv2->netdev = netdev2;
		priv2->cmac_id = 1;
		/* vec_base: skip primary's num_q_vectors queue vectors + user + error */
		priv2->vec_base = priv->num_q_vectors + 2;
		priv2->num_q_vectors = priv->num_q_vectors;
		priv2->num_tx_queues = priv->num_tx_queues;
		priv2->num_rx_queues = priv->num_rx_queues;
		priv2->RS_FEC = RS_FEC_ENABLED;
		priv2->msg_enable = NETIF_MSG_DRV | NETIF_MSG_LINK;
		spin_lock_init(&priv2->tx_lock);
		spin_lock_init(&priv2->rx_lock);
		spin_lock_init(&priv2->ptp_lock);
		spin_lock_init(&priv2->ptp_tx_lock);
		onic_init_link_recovery(priv2);
		onic_init_error_rearm(priv2);

		priv2->netdev_stats = alloc_percpu(struct rtnl_link_stats64);
		if (!priv2->netdev_stats) {
			dev_warn(&pdev->dev, "failed to alloc secondary netdev_stats\n");
			free_netdev(netdev2);
			goto probe_done;
		}

		/* Secondary shares BAR2 addr and inherits num_cmacs from primary */
		priv2->hw = priv->hw;

		/* Create a separate qdma_dev for secondary with q_base=ONIC_MAX_QUEUES
		 * so that QDMA queue operations automatically target the correct range. */
		sq_dev = qdma_create_dev(pdev, 0);
		if (!sq_dev) {
			dev_warn(&pdev->dev, "failed to create secondary qdma_dev\n");
			free_percpu(priv2->netdev_stats);
			free_netdev(netdev2);
			goto probe_done;
		}
		sq_dev->q_base = ONIC_MAX_QUEUES;
		sq_dev->num_queues = priv2->num_q_vectors;
		priv2->hw.qdma = (unsigned long)sq_dev;

		/* Write shell QCONF for function 1 (CMAC1) */
		{
			u32 qconf = (FIELD_SET(QDMA_FUNC_QCONF_QBASE_MASK, ONIC_MAX_QUEUES) |
				     FIELD_SET(QDMA_FUNC_QCONF_NUMQ_MASK, priv2->num_q_vectors));
			onic_write_reg(&priv2->hw, QDMA_FUNC_OFFSET_QCONF(1), qconf);
		}
		/* Write RSS indirection table for function 1 */
		{
			int j;
			for (j = 0; j < 128; ++j) {
				u32 v = (ONIC_MAX_QUEUES + (j % priv2->num_q_vectors)) & 0xFFFF;
				onic_write_reg(&priv2->hw, QDMA_FUNC_OFFSET_INDIR_TABLE(1, j), v);
			}
		}

		rv = onic_init_interrupt(priv2);
		if (rv < 0) {
			dev_warn(&pdev->dev, "secondary onic_init_interrupt err=%d, CMAC1 unavailable\n", rv);
			qdma_destroy_dev(sq_dev);
			free_percpu(priv2->netdev_stats);
			free_netdev(netdev2);
			goto probe_done;
		}

		netif_set_real_num_tx_queues(netdev2, priv2->num_tx_queues);
		netif_set_real_num_rx_queues(netdev2, priv2->num_rx_queues);
		netdev2->min_mtu = ETH_MIN_MTU;
		netdev2->max_mtu = 9600 - ETH_HLEN;
		netdev2->features |= NETIF_F_HIGHDMA;
		netdev2->hw_features |= NETIF_F_HIGHDMA;

		rv = register_netdev(netdev2);
		if (rv < 0) {
			dev_warn(&pdev->dev, "register secondary netdev err=%d, CMAC1 unavailable\n", rv);
			onic_clear_interrupt(priv2);
			qdma_destroy_dev(sq_dev);
			free_percpu(priv2->netdev_stats);
			free_netdev(netdev2);
			goto probe_done;
		}

		priv->peer = priv2;
		netif_carrier_off(netdev2);
		dev_info(&pdev->dev, "secondary net_device %s registered for CMAC1\n", netdev2->name);
	}

probe_done:
#ifdef CMS_SUPPORT
        /* Support CMS sensors (lm-sensors), refer: pg348 */
        if(xmc_init == 0)
        {
            onic_priv = priv;
            xocl_init_xmc();
            xmc_init=1;
        }
#endif

	return 0;

clear_interrupt:
	onic_clear_interrupt(priv);
clear_hardware:
	onic_clear_hardware(priv);
clear_capacity:
	onic_clear_capacity(priv);
free_netdev:
	free_netdev(priv->netdev);
release_pci_mem:
	pci_release_mem_regions(pdev);
disable_device:
	pci_disable_device(pdev);

	return rv;
}

/**
 * onic_remove - remove PCI device
 * @pdev: pointer to PCI device
 **/
static void onic_remove(struct pci_dev *pdev)
{
	struct onic_private *priv = pci_get_drvdata(pdev);
#ifdef CMS_SUPPORT
        static int xmc_remove=0;
#endif

	dev_info(&pdev->dev, "removing device");

	/* 0. Tear down secondary net_device (CMAC1) before touching primary.
	 * Must happen before CMAC RX disable so the secondary's NAPI drains
	 * while IRQs are still live (same reasoning as the primary below). */
	if (priv->peer) {
		struct onic_private *priv2 = priv->peer;
		struct onic_hardware *hw2 = &priv2->hw;

		onic_write_reg(hw2, CMAC_OFFSET_CONF_RX_1(1), 0x0);
		udelay(10);
		set_bit(ONIC_FLAG_CMAC_RX_DISABLED, priv2->flags);

		cancel_work_sync(&priv2->link_recovery_work);
		onic_clear_interrupt(priv2);
		cancel_work_sync(&priv2->link_recovery_work);

		onic_ptp_cleanup(priv2);
		unregister_netdev(priv2->netdev);

		onic_clear_hardware(priv2);
		free_percpu(priv2->netdev_stats);
		priv->peer = NULL;
		free_netdev(priv2->netdev);
	}

	/* 1. STOP THE PACKET FLOW FIRST (primary / CMAC0).
	 *
	 * Disable CMAC RX before touching IRQs or NAPI.  With a 93 Gbps
	 * stream active, QDMA continuously writes completions and the
	 * IRQ handler schedules NAPI.  If we free IRQs while the CMAC is
	 * still active, the last-scheduled NAPI poll enters a busy-poll
	 * loop: poll → process → refill descriptors → QDMA DMAs more →
	 * more completions → poll returns budget → softirq re-polls.
	 * Nothing can break this loop because no IRQ handler remains to
	 * detect that the queue is being torn down, and napi_disable()
	 * later spins forever waiting for NAPI_STATE_SCHED to clear.
	 *
	 * By disabling CMAC RX first, the pipeline drains while IRQs are
	 * still live: the last completions generate interrupts, NAPI
	 * processes them, the poll returns < budget, napi_complete_done
	 * clears NAPI_STATE_SCHED, and the queue goes idle.  After that,
	 * free_irq and napi_disable complete instantly. */
	{
		struct onic_hardware *hw = &priv->hw;
		onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(0), 0x0);
		udelay(10);
		set_bit(ONIC_FLAG_CMAC_RX_DISABLED, priv->flags);
	}

	/* 2. Drain any work running before free_irq. */
	cancel_work_sync(&priv->link_recovery_work);

	/* 3. Free IRQs.  With CMAC RX already off, the NAPI polls have
	 * drained and free_irq just removes idle handlers. */
	onic_clear_interrupt(priv);

	/* 4. Drain any work re-scheduled in the window between the first
	 * cancel and free_irq; IRQ threads cannot enqueue new work now. */
	cancel_work_sync(&priv->link_recovery_work);

	/* 5. Clean up PTP before unregistering netdev */
	onic_ptp_cleanup(priv);

	/* 6. Unregister the netdev (calls onic_stop_netdev via ndo_stop).
	 * onic_stop_netdev skips the CMAC disable since we already did it. */
	unregister_netdev(priv->netdev);

	onic_clear_hardware(priv);
	onic_clear_capacity(priv);

	free_netdev(priv->netdev);

	pci_set_drvdata(pdev, NULL);
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);

#ifdef CMS_SUPPORT
        /* Support XMC sensors (lm-sensors) */
        if(xmc_remove == 0)
        {
            xocl_fini_xmc();
            xmc_remove=1;
        }
#endif
}

static pci_ers_result_t onic_error_detected(struct pci_dev *pdev,
					     pci_channel_state_t state)
{
	struct onic_private *priv = pci_get_drvdata(pdev);

	netif_device_detach(priv->netdev);
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t onic_slot_reset(struct pci_dev *pdev)
{
	struct onic_private *priv = pci_get_drvdata(pdev);

	if (pci_enable_device(pdev))
		return PCI_ERS_RESULT_DISCONNECT;
	pci_set_master(pdev);
	/* shell auto-fired system reset when pcie_rstn deasserted;
	 * wait for it to complete, then re-init hardware */
	if (onic_init_hardware(priv))
		return PCI_ERS_RESULT_DISCONNECT;
	return PCI_ERS_RESULT_RECOVERED;
}

static void onic_error_resume(struct pci_dev *pdev)
{
	struct onic_private *priv = pci_get_drvdata(pdev);

	netif_device_attach(priv->netdev);
}

static const struct pci_error_handlers onic_err_handler = {
	.error_detected = onic_error_detected,
	.slot_reset     = onic_slot_reset,
	.resume         = onic_error_resume,
};

static struct pci_driver pci_driver = {
	.name        = onic_drv_name,
	.id_table    = onic_pci_tbl,
	.probe       = onic_probe,
	.remove      = onic_remove,
	.err_handler = &onic_err_handler,
};

static int __init onic_init_module(void)
{
	pr_info("%s %s", onic_drv_str, onic_drv_ver);
	return pci_register_driver(&pci_driver);
}

static void __exit onic_exit_module(void)
{
	pr_info("%s %s unloaded", onic_drv_str, onic_drv_ver);
	pci_unregister_driver(&pci_driver);
}

module_init(onic_init_module);
module_exit(onic_exit_module);
