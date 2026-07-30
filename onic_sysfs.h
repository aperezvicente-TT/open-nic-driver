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
#ifndef __ONIC_SYSFS_H__
#define __ONIC_SYSFS_H__

struct net_device;

/**
 * onic_sysfs_attach_groups - publish onic's per-netdev sysfs attributes
 * @netdev: the netdev being brought up
 *
 * Installs the flow-control tuning attribute group into @netdev's
 * sysfs_groups[], so the attributes appear under /sys/class/net/<ifname>/ when
 * the netdev is registered and are torn down with it.
 *
 * MUST be called BEFORE register_netdev(): netdev_register_kobject() copies
 * sysfs_groups[] into the embedded device's ->groups, and later changes are
 * ignored.  No teardown counterpart is needed for the same reason.
 **/
void onic_sysfs_attach_groups(struct net_device *netdev);

#endif /* __ONIC_SYSFS_H__ */
