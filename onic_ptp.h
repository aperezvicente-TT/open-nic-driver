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
#ifndef __ONIC_PTP_H__
#define __ONIC_PTP_H__

#include <linux/ptp_clock_kernel.h>
#include <linux/net_tstamp.h>

/* PTP register base within the BAR-2 shell address space */
#define ONIC_PTP_BASE			0x18000

/* Global PTP registers */
#define ONIC_PTP_CTRL			(ONIC_PTP_BASE + 0x000)
#define ONIC_PTP_TS_S_LO		(ONIC_PTP_BASE + 0x010)
#define ONIC_PTP_TS_S_HI		(ONIC_PTP_BASE + 0x014)
#define ONIC_PTP_TS_NS			(ONIC_PTP_BASE + 0x018)
#define ONIC_PTP_TS_FNS			(ONIC_PTP_BASE + 0x01C)
#define ONIC_PTP_SET_S_LO		(ONIC_PTP_BASE + 0x020)
#define ONIC_PTP_SET_S_HI		(ONIC_PTP_BASE + 0x024)
#define ONIC_PTP_SET_NS			(ONIC_PTP_BASE + 0x028)
#define ONIC_PTP_SET_VALID		(ONIC_PTP_BASE + 0x030)
#define ONIC_PTP_PERIOD_NS		(ONIC_PTP_BASE + 0x040)
#define ONIC_PTP_PERIOD_FNS		(ONIC_PTP_BASE + 0x044)
#define ONIC_PTP_PERIOD_VALID		(ONIC_PTP_BASE + 0x048)
#define ONIC_PTP_ADJ_NS			(ONIC_PTP_BASE + 0x050)
#define ONIC_PTP_ADJ_FNS		(ONIC_PTP_BASE + 0x054)
#define ONIC_PTP_ADJ_COUNT		(ONIC_PTP_BASE + 0x058)
#define ONIC_PTP_ADJ_VALID		(ONIC_PTP_BASE + 0x05C)
#define ONIC_PTP_DRIFT_NS		(ONIC_PTP_BASE + 0x060)
#define ONIC_PTP_DRIFT_FNS		(ONIC_PTP_BASE + 0x064)
#define ONIC_PTP_DRIFT_RATE		(ONIC_PTP_BASE + 0x068)
#define ONIC_PTP_DRIFT_VALID		(ONIC_PTP_BASE + 0x06C)

/* Per-port TX timestamp registers */
#define ONIC_PTP_PORT_BASE(n)		(ONIC_PTP_BASE + 0x1000 + (n) * 0x1000)
#define ONIC_PTP_TX_TS_LO(n)		(ONIC_PTP_PORT_BASE(n) + 0x000)
#define ONIC_PTP_TX_TS_HI(n)		(ONIC_PTP_PORT_BASE(n) + 0x004)
#define ONIC_PTP_TX_TS_TAG(n)		(ONIC_PTP_PORT_BASE(n) + 0x008)
#define ONIC_PTP_TX_TS_VALID(n)		(ONIC_PTP_PORT_BASE(n) + 0x00C)

/* PTP CTRL register bits */
#define ONIC_PTP_CTRL_ENABLE		BIT(0)

/* PTP version field in CTRL register (bits 31:16) */
#define ONIC_PTP_CTRL_VERSION_MASK	GENMASK(31, 16)
#define ONIC_PTP_CTRL_VERSION_SHIFT	16

/* Expected minimum PTP block version */
#define ONIC_PTP_MIN_VERSION		0x0001

/* Default clock period for 250 MHz: 4 ns + 0 fractional ns */
#define ONIC_PTP_NOMINAL_PERIOD_NS	4
#define ONIC_PTP_NOMINAL_PERIOD_FNS	0

/* TX timestamp FIFO polling timeout in microseconds */
#define ONIC_PTP_TX_TS_POLL_TIMEOUT_US	10000
#define ONIC_PTP_TX_TS_POLL_DELAY_US	10

struct onic_private;

/**
 * onic_ptp_init - Register a PTP clock device with the kernel
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure.  If PTP hardware is not
 * present in the FPGA build, returns 0 without registering (no error).
 */
int onic_ptp_init(struct onic_private *priv);

/**
 * onic_ptp_cleanup - Unregister PTP clock device
 * @priv: pointer to driver private data
 */
void onic_ptp_cleanup(struct onic_private *priv);

/**
 * onic_ptp_hwtstamp_set - Handle SIOCSHWTSTAMP ioctl
 * @dev: pointer to net device
 * @ifr: pointer to ifreq structure
 *
 * Return 0 on success, negative on failure.
 */
int onic_ptp_hwtstamp_set(struct net_device *dev, struct ifreq *ifr);

/**
 * onic_ptp_hwtstamp_get - Handle SIOCGHWTSTAMP ioctl
 * @dev: pointer to net device
 * @ifr: pointer to ifreq structure
 *
 * Return 0 on success, negative on failure.
 */
int onic_ptp_hwtstamp_get(struct net_device *dev, struct ifreq *ifr);

#endif
