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
#ifndef __QDMA_DEVICE_H__
#define __QDMA_DEVICE_H__

#include <linux/pci.h>

#define QDMA_FLAG_FMAP		 BIT(1)

struct qdma_dev {
	struct pci_dev *pdev;
	u16 func_id;
	u16 q_base;
	u16 num_queues;
	void __iomem *addr;	/* mappaed address of device registers */
	bool is_child;		/* true: shares addr with parent, no iounmap on destroy */
	bool borrowed_addr;	/* true: addr was borrowed from libqdma, no iounmap on destroy */
};

/**
 * qdma_create_dev - Create a QDMA device using a borrowed BAR 0 mapping
 * @pdev: pointer to PCI device
 * @bar0_regs: ioremap pointer for BAR 0 (config), borrowed from libqdma via
 *             qdma_device_get_config_regs().  Must be non-NULL.  The legacy
 *             qdma_dev keeps a copy of this pointer for its register pokes
 *             but does NOT own the mapping — qdma_destroy_dev will not
 *             iounmap it.
 *
 * Return a pointer to the created QDMA device, or NULL on failure.
 **/
struct qdma_dev *qdma_create_dev(struct pci_dev *pdev, void __iomem *bar0_regs);

/**
 * qdma_create_child_dev - Create a child QDMA device sharing the parent's iomap
 * @parent: parent QDMA device whose BAR iomap and func_id are shared
 * @q_base: queue ID offset for this child (absolute qid = q_base + relative qid)
 *
 * A child shares @parent->pdev, @parent->addr, and @parent->func_id.  It is
 * intended for single-PF multi-CMAC designs where two net_devices must own
 * distinct queue ranges within one QDMA function.  The parent writes the fmap
 * context once for the combined range; children only use queue-context ops,
 * which route via q_base.  Children must be destroyed before the parent.
 **/
struct qdma_dev *qdma_create_child_dev(struct qdma_dev *parent, u16 q_base);

/**
 * qdma_destroy_dev - Destroy a QDMA device
 * @qdev: pointer to QDMA device
 *
 * The input pointer is checked.  So it is safe to pass in NULL pointers.
 * For child devices, the shared BAR iomap is left intact (parent owns it).
 **/
void qdma_destroy_dev(struct qdma_dev *qdev);

#endif
