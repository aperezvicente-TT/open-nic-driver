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
#include "onic_sysdma.h"
#include "qdma_legacy/qdma_device.h"
#include "qdma_legacy/qdma_context.h"

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
 * onic_alloc_netdev - allocate and initialize a net_device + onic_private
 * @pdev: owning PCI device (shared between primary/secondary on single-PF)
 * @cmac_id: CMAC index this netdev represents (0 or 1)
 *
 * Performs the pure net_device creation and priv zero-init common to both
 * primary and secondary.  Does NOT touch MSI-X, hardware, or interrupts —
 * the caller wires those up based on primary vs slave role.  Returns the
 * priv pointer on success (stats percpu allocated), NULL on failure.
 */
static struct onic_private *onic_alloc_netdev(struct pci_dev *pdev, u8 cmac_id)
{
	struct net_device *netdev;
	struct onic_private *priv;
	struct sockaddr saddr;
	char dev_name[IFNAMSIZ];

	netdev = alloc_etherdev_mq(sizeof(struct onic_private), ONIC_MAX_QUEUES);
	if (!netdev) {
		dev_err(&pdev->dev, "alloc_etherdev_mq failed for cmac%d", cmac_id);
		return NULL;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);
	netdev->netdev_ops = &onic_netdev_ops;
	onic_set_ethtool_ops(netdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0)
	xdp_set_features_flag(netdev, NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT);
#endif
	snprintf(dev_name, IFNAMSIZ, "onic%ds%df%dc%d",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn),
		 cmac_id);
	strscpy(netdev->name, dev_name, sizeof(netdev->name));
	netdev->dev_port = cmac_id;

	memset(&saddr, 0, sizeof(struct sockaddr));
	memcpy(saddr.sa_data, onic_default_dev_addr, 6);
	saddr.sa_data[3] = onic_resolve_host_id(pdev);
	saddr.sa_data[4] = pdev->bus->number;
	/* MAC last byte: PCI_FUNC in upper nibble, cmac_id in lower — keeps
	 * primary's MAC identical to pre-dual-netdev behavior on single-PF
	 * shells (PCI_FUNC=0, cmac_id=0) and gives CMAC1 a distinct address. */
	saddr.sa_data[5] = (PCI_FUNC(pdev->devfn) << 4) | cmac_id;
	onic_set_mac_address(netdev, (void *)&saddr);

	priv = netdev_priv(netdev);
	memset(priv, 0, sizeof(struct onic_private));
	priv->msg_enable = NETIF_MSG_DRV | NETIF_MSG_LINK;
	priv->RS_FEC = RS_FEC_ENABLED;
	priv->pdev = pdev;
	priv->netdev = netdev;
	priv->cmac_id = cmac_id;
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);
	onic_init_link_recovery(priv);
	onic_init_error_rearm(priv);

	priv->netdev_stats = alloc_percpu(struct rtnl_link_stats64);
	if (!priv->netdev_stats) {
		dev_err(&pdev->dev, "alloc_percpu netdev_stats failed");
		free_netdev(netdev);
		return NULL;
	}

	return priv;
}

static void onic_apply_netdev_features(struct net_device *netdev)
{
	netdev->min_mtu = ETH_MIN_MTU;        /* 68 bytes */
	netdev->max_mtu = 9600 - ETH_HLEN;    /* jumbo, max_pkt_len=9600 from shell */
	netdev->features |= NETIF_F_HIGHDMA;
	netdev->hw_features |= NETIF_F_HIGHDMA;
}

/**
 * onic_setup_primary - bring up the CMAC0 netdev (master PF)
 *
 * Owns BAR2 iomap, QDMA device, MSI-X allocation, and user/error IRQs.
 */
static int onic_setup_primary(struct pci_dev *pdev, struct onic_private **out)
{
	struct onic_private *priv;
	int rv;

	priv = onic_alloc_netdev(pdev, 0);
	if (!priv)
		return -ENOMEM;

	dev_info(&pdev->dev, "device is a master PF");
	set_bit(ONIC_FLAG_MASTER_PF, priv->flags);
	priv->vec_base = 0;
	priv->qid_base = 0;

	rv = onic_init_capacity(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_capacity (primary), err = %d", rv);
		goto free_netdev;
	}

	rv = onic_init_hardware(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_hardware (primary), err = %d", rv);
		goto clear_capacity;
	}

	rv = onic_init_interrupt(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_interrupt (primary), err = %d", rv);
		goto clear_hardware;
	}

	rv = onic_ernic_irq_setup(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_ernic_irq_setup, err = %d", rv);
		goto clear_interrupt;
	}

	netif_set_real_num_tx_queues(priv->netdev, priv->num_tx_queues);
	netif_set_real_num_rx_queues(priv->netdev, priv->num_rx_queues);
	onic_apply_netdev_features(priv->netdev);

	rv = register_netdev(priv->netdev);
	if (rv < 0) {
		dev_err(&pdev->dev, "register_netdev (primary), err = %d", rv);
		goto clear_interrupt;
	}

	rv = onic_ib_register(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_ib_register, err = %d", rv);
		unregister_netdev(priv->netdev);
		goto clear_interrupt;
	}

	/* B7: bring up the QDMA AXI-MM system DMA queue (master PF only).
	 * Used by the RDMA verb path to write WQEs into ERNIC's DDR4
	 * rings.  Failure is non-fatal — netdev + ib_device still work,
	 * RDMA verbs requiring DDR4 access will fail at post_send/recv. */
	rv = onic_sysdma_init(priv);
	if (rv < 0) {
		dev_warn(&pdev->dev,
			 "onic_sysdma_init failed (%d) — RDMA WQE path unavailable\n",
			 rv);
		/* deliberately not goto-out: probe continues. */
		rv = 0;
	} else {
		/* Smoke-test the H2C MM path with a 64-byte write.  Failure
		 * is logged at error level but probe still succeeds; the
		 * RDMA verbs themselves will return more specific errors. */
		(void)onic_sysdma_self_test(priv);
	}

	netif_carrier_off(priv->netdev);
	*out = priv;
	return 0;

clear_interrupt:
	onic_clear_interrupt(priv);
clear_hardware:
	onic_clear_hardware(priv);
clear_capacity:
	onic_clear_capacity(priv);
free_netdev:
	free_netdev(priv->netdev);
	return rv;
}

/**
 * onic_setup_secondary - bring up the CMAC1 netdev on the same PF
 *
 * Shares the primary's BAR2 iomap, QDMA device (via a child with q_base),
 * and upper-half of the primary's MSI-X pool (via vec_base).  Does not take
 * user/error IRQs — those belong to the primary only.
 */
static int onic_setup_secondary(struct onic_private *primary,
				struct onic_private **out)
{
	struct pci_dev *pdev = primary->pdev;
	struct onic_private *priv;
	int rv;

	priv = onic_alloc_netdev(pdev, 1);
	if (!priv)
		return -ENOMEM;

	/* Primary reserves the MSI-X slots just past its own queue vectors
	 * for user IRQ (always) and error IRQ (MASTER_PF only).  Secondary's
	 * queues must start after those — onic_init_q_vector builds its IRQ
	 * number as pci_irq_vector(pdev, vec_base + relative_vid). */
	{
		u16 non_q = 1; /* user IRQ */
		if (test_bit(ONIC_FLAG_MASTER_PF, primary->flags)) {
			non_q++;     /* + error IRQ */
			non_q += 2;  /* + ERNIC0 + ERNIC1 IRQs (F5) */
		}
		priv->vec_base = primary->num_q_vectors + non_q;
	}
	/* qid_base is dictated by the shell plugin's PER_CMAC_QUEUES constant —
	 * NOT by primary->num_tx_queues.  Primary uses queues [0, N) within the
	 * CMAC0 range; secondary must sit at the CMAC1 range start. */
	priv->qid_base = ONIC_PER_CMAC_QUEUES;
	priv->peer = primary;
	primary->peer = priv;

	onic_init_capacity_slave(priv, primary);

	rv = onic_init_hardware(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_hardware (secondary), err = %d", rv);
		goto free_netdev;
	}

	rv = onic_init_interrupt(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "onic_init_interrupt (secondary), err = %d", rv);
		goto clear_hardware;
	}

	netif_set_real_num_tx_queues(priv->netdev, priv->num_tx_queues);
	netif_set_real_num_rx_queues(priv->netdev, priv->num_rx_queues);
	onic_apply_netdev_features(priv->netdev);

	rv = register_netdev(priv->netdev);
	if (rv < 0) {
		dev_err(&pdev->dev, "register_netdev (secondary), err = %d", rv);
		goto clear_interrupt;
	}

	netif_carrier_off(priv->netdev);
	*out = priv;
	return 0;

clear_interrupt:
	onic_clear_interrupt(priv);
clear_hardware:
	onic_clear_hardware(priv);
	onic_clear_capacity(priv);
free_netdev:
	primary->peer = NULL;
	free_netdev(priv->netdev);
	return rv;
}

/**
 * onic_teardown_netdev - reverse the setup of one netdev
 *
 * Matches the teardown sequence previously inline in onic_remove: disable
 * CMAC RX first to drain NAPI, drain the recovery workqueue, free IRQs,
 * then unregister + clear_hardware + clear_capacity + free.  Safe to call
 * on either primary or secondary; onic_clear_hardware branches on
 * MASTER_PF to decide whether to do the full QDMA teardown (primary) or
 * just destroy the child qdma_dev wrapper (secondary).
 */
static void onic_teardown_netdev(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	u8 cmac_id = priv->cmac_id;

	/* STOP PACKET FLOW FIRST — see long comment in onic_remove below.
	 * Disabling CMAC RX drains the NAPI loop before we touch IRQs. */
	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(cmac_id), 0x0);
	udelay(10);
	set_bit(ONIC_FLAG_CMAC_RX_DISABLED, priv->flags);

	cancel_work_sync(&priv->link_recovery_work);
	onic_sysdma_fini(priv);  /* B7: tear down MM queue before ib_dev unregister */
	onic_ib_unregister(priv);
	onic_ernic_irq_teardown(priv);
	onic_clear_interrupt(priv);
	cancel_work_sync(&priv->link_recovery_work);

	onic_ptp_cleanup(priv);
	unregister_netdev(priv->netdev);

	onic_clear_hardware(priv);
	onic_clear_capacity(priv);

	free_netdev(priv->netdev);
}

/**
 * onic_probe - Probe and initialize PCI device
 * @pdev: pointer to PCI device
 * @ent: pointer to PCI device ID entries
 *
 * On single-PF dual-CMAC shells, allocates two netdevs from a single probe:
 * the primary (CMAC0) owns MSI-X/QDMA, the secondary (CMAC1) shares them.
 *
 * Return 0 on success, negative on failure
 **/
static int onic_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct onic_private *primary = NULL;
	struct onic_private *secondary = NULL;
	int rv;
#ifdef CMS_SUPPORT
        static int xmc_init=0;
#endif

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

	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_EXT_TAG);
	pci_set_master(pdev);
	pci_save_state(pdev);
	pcie_set_readrq(pdev, 512);

	rv = onic_setup_primary(pdev, &primary);
	if (rv < 0)
		goto release_pci_mem;

	pci_set_drvdata(pdev, primary);

	/* Spawn secondary netdev for CMAC1 if the shell exposes two CMACs.
	 * num_cmacs is discovered during primary's hardware init. */
	if (primary->hw.num_cmacs >= 2) {
		rv = onic_setup_secondary(primary, &secondary);
		if (rv < 0) {
			dev_err(&pdev->dev,
				"secondary (CMAC1) setup failed (err=%d); primary still usable\n",
				rv);
			/* Non-fatal: leave primary up.  CMAC1 will be unused. */
			rv = 0;
		} else {
			/* Bind port 2 of the ib_device to the secondary's netdev
			 * now that register_netdev() has assigned its name. */
			(void)onic_ib_set_port2_netdev(primary, secondary);
		}
	}

	rv = onic_ptp_init(primary);
	if (rv < 0)
		dev_warn(&pdev->dev, "PTP init (primary) failed (err=%d), continuing\n", rv);
	if (secondary) {
		rv = onic_ptp_init(secondary);
		if (rv < 0)
			dev_warn(&pdev->dev, "PTP init (secondary) failed (err=%d), continuing\n", rv);
	}
	rv = 0;

#ifdef CMS_SUPPORT
        if(xmc_init == 0)
        {
            onic_priv = primary;
            xocl_init_xmc();
            xmc_init=1;
        }
#endif

	return 0;

release_pci_mem:
	pci_release_mem_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	return rv;
}

/**
 * onic_remove - remove PCI device
 * @pdev: pointer to PCI device
 *
 * Teardown order: secondary first (drops CMAC1 and its child qdma_dev), then
 * primary (runs the full QDMA shell reset, fmap invalidation, and BAR2
 * iounmap that children shared).
 *
 * STOP THE PACKET FLOW FIRST: disabling CMAC RX before touching IRQs or NAPI
 * is what keeps free_irq/napi_disable from spinning.  With a live stream,
 * QDMA continuously writes completions and schedules NAPI; freeing IRQs
 * while the CMAC is still active creates a busy-poll loop with no IRQ
 * handler left to notice the queue is being torn down, and napi_disable
 * spins forever waiting for NAPI_STATE_SCHED to clear.  Disabling CMAC RX
 * first drains the pipeline while IRQs are still live, so the final NAPI
 * poll returns < budget and the queue goes idle.  onic_teardown_netdev
 * does this per-netdev.
 **/
static void onic_remove(struct pci_dev *pdev)
{
	struct onic_private *primary = pci_get_drvdata(pdev);
	struct onic_private *secondary;
#ifdef CMS_SUPPORT
        static int xmc_remove=0;
#endif

	dev_info(&pdev->dev, "removing device");

	if (!primary)
		return;

	secondary = primary->peer;

	/* B3/B3.5: Unregister the ib_device FIRST, before any netdev teardown.
	 * ib_device_set_netdev() took refcounts on both primary->netdev (port 1)
	 * and secondary->netdev (port 2); those refcounts must be released
	 * before unregister_netdev() will complete.  Otherwise the kernel loops
	 * on "unregister_netdevice: waiting for enp1s0d1 to become free".
	 * onic_ib_unregister() is a no-op on non-master-PF / VFs. */
	onic_ib_unregister(primary);

	if (secondary) {
		/* Break the bidirectional peer link before teardown so the
		 * link-recovery path (which dereferences priv->peer) doesn't
		 * touch the secondary while it's being freed. */
		primary->peer = NULL;
		secondary->peer = NULL;
		onic_teardown_netdev(secondary);
	}

	onic_teardown_netdev(primary);

	pci_set_drvdata(pdev, NULL);
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);

#ifdef CMS_SUPPORT
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
	/* Build-time contract: per-CMAC queue stride must fit within the
	 * per-netdev queue array.  If these diverge the plugin's tagged qid
	 * won't have a matching sw_ctxt in the driver and packets drop. */
	BUILD_BUG_ON(ONIC_PER_CMAC_QUEUES > ONIC_MAX_QUEUES);

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
