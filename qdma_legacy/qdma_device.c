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
#include "qdma_device.h"

struct qdma_dev *qdma_create_dev(struct pci_dev *pdev, void __iomem *bar0_regs)
{
	struct qdma_dev *qdev;

	if (!bar0_regs) {
		dev_err(&pdev->dev, "qdma_create_dev: NULL bar0_regs");
		return NULL;
	}

	qdev = kzalloc(sizeof(struct qdma_dev), GFP_KERNEL);
	if (!qdev)
		return NULL;

	qdev->pdev = pdev;
	qdev->func_id = PCI_FUNC(pdev->devfn);

	/* Borrow libqdma's BAR 0 ioremap.  We do NOT pci_iomap here, since
	 * libqdma already holds the PCI region claim (pci_request_regions in
	 * qdma_device_open).  Marking borrowed_addr makes qdma_destroy_dev
	 * skip the matching iounmap. */
	qdev->addr = bar0_regs;
	qdev->borrowed_addr = true;

	return qdev;
}

struct qdma_dev *qdma_create_child_dev(struct qdma_dev *parent, u16 q_base)
{
	struct qdma_dev *qdev;

	if (!parent)
		return NULL;

	qdev = kzalloc(sizeof(struct qdma_dev), GFP_KERNEL);
	if (!qdev)
		return NULL;

	qdev->pdev = parent->pdev;
	qdev->func_id = parent->func_id;
	qdev->addr = parent->addr;
	qdev->q_base = q_base;
	qdev->is_child = true;

	return qdev;
}

void qdma_destroy_dev(struct qdma_dev *qdev)
{
	if (!qdev)
		return;

	/* Children share the parent's addr; borrowed devices share libqdma's
	 * mapping.  In both cases we have no iomap to release here. */
	if (!qdev->is_child && !qdev->borrowed_addr)
		pci_iounmap(qdev->pdev, qdev->addr);
	kfree(qdev);
}
