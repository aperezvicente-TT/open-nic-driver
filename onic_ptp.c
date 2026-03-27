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
#include <linux/ptp_clock_kernel.h>
#include <linux/net_tstamp.h>
#include <linux/delay.h>

#include "onic.h"
#include "onic_ptp.h"
#include "onic_register.h"

/**
 * onic_ptp_adjfine - Adjust PTP clock frequency using the DRIFT registers
 * @ptp: pointer to ptp_clock_info
 * @scaled_ppm: frequency adjustment in scaled parts per million
 *
 * The FPGA PTP clock runs at a nominal period of 4 ns (250 MHz).
 * scaled_ppm is in units of ppb * 2^16 / 1000.
 *
 * We convert the scaled_ppm to a per-cycle drift:
 *   drift_ns.fns = nominal_period * scaled_ppm / (10^6 * 2^16)
 *
 * The DRIFT registers apply an additive correction of DRIFT_NS.DRIFT_FNS
 * every DRIFT_RATE cycles.  For simplicity, we set DRIFT_RATE=1 so the
 * drift is applied every clock cycle.
 *
 * Return 0 on success.
 */
static int onic_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct onic_private *priv =
		container_of(ptp, struct onic_private, ptp_info);
	struct onic_hardware *hw = &priv->hw;
	unsigned long flags;
	bool negative = false;
	u64 adj;
	u32 drift_ns, drift_fns;

	if (scaled_ppm < 0) {
		negative = true;
		scaled_ppm = -scaled_ppm;
	}

	/*
	 * Compute the per-cycle frequency adjustment.
	 *
	 * nominal_period = 4 ns = 4 * 2^32 fractional-ns units
	 * drift = nominal_period * scaled_ppm / (10^6 * 2^16)
	 *
	 * To avoid overflow, rearrange:
	 *   drift = (4 * scaled_ppm) / (10^6 * 2^16) in ns
	 *         = (4 * scaled_ppm * 2^32) / (10^6 * 2^16) in fns
	 *         = (4 * scaled_ppm * 2^16) / 10^6  in fns
	 */
	adj = (u64)scaled_ppm * 4;
	/* Multiply by 2^16 = 65536 */
	adj <<= 16;
	/* Divide by 10^6 */
	adj = div_u64(adj, 1000000);

	/*
	 * Convert from 0.32 fixed-point (2^-32 ns units) to the FPGA's
	 * 4.16 fixed-point format: drift_ns[3:0] + drift_fns[15:0].
	 * Shift right by 16 to go from 2^-32 to 2^-16 resolution.
	 */
	drift_ns = (u32)(adj >> 32) & 0xF;
	drift_fns = (u32)((adj >> 16) & 0xFFFF);

	/* If negative, use two's complement in 20-bit {4,16} format.
	 * The FPGA interprets {drift_ns, drift_fns} as signed. */
	if (negative) {
		u32 combined = ((drift_ns & 0xF) << 16) | drift_fns;
		combined = (~combined + 1) & 0xFFFFF;
		drift_ns = (combined >> 16) & 0xF;
		drift_fns = combined & 0xFFFF;
	}

	spin_lock_irqsave(&priv->ptp_lock, flags);

	onic_write_reg(hw, ONIC_PTP_DRIFT_NS, drift_ns);
	onic_write_reg(hw, ONIC_PTP_DRIFT_FNS, drift_fns);
	/* Apply drift every cycle */
	onic_write_reg(hw, ONIC_PTP_DRIFT_RATE, 1);
	/* Commit: write 1 to DRIFT_VALID */
	onic_write_reg(hw, ONIC_PTP_DRIFT_VALID, 1);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/**
 * onic_ptp_adjtime - Adjust PTP clock time by a delta
 * @ptp: pointer to ptp_clock_info
 * @delta: time adjustment in nanoseconds
 *
 * Writes the delta to the ADJ registers and triggers a single-shot
 * adjustment (ADJ_COUNT = 1).
 *
 * Return 0 on success.
 */
static int onic_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct onic_private *priv =
		container_of(ptp, struct onic_private, ptp_info);
	struct onic_hardware *hw = &priv->hw;
	unsigned long flags;
	bool negative = false;
	u32 adj_ns, adj_fns;

	if (delta < 0) {
		negative = true;
		delta = -delta;
	}

	adj_ns = (u32)delta;
	adj_fns = 0;

	/* For negative adjustments, use two's complement */
	if (negative) {
		u64 combined = ((u64)adj_ns << 32) | adj_fns;
		combined = ~combined + 1;
		adj_ns = (u32)(combined >> 32);
		adj_fns = (u32)(combined & 0xFFFFFFFF);
	}

	spin_lock_irqsave(&priv->ptp_lock, flags);

	onic_write_reg(hw, ONIC_PTP_ADJ_NS, adj_ns);
	onic_write_reg(hw, ONIC_PTP_ADJ_FNS, adj_fns);
	/* Single-shot adjustment */
	onic_write_reg(hw, ONIC_PTP_ADJ_COUNT, 1);
	/* Commit: write 1 to ADJ_VALID */
	onic_write_reg(hw, ONIC_PTP_ADJ_VALID, 1);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/**
 * onic_ptp_gettime64 - Read the current PTP hardware clock time
 * @ptp: pointer to ptp_clock_info
 * @ts: pointer to timespec64 to fill
 *
 * Reading TS_S_LO triggers the hardware to latch all time registers
 * atomically.  We then read TS_S_HI and TS_NS to construct the time.
 *
 * Return 0 on success.
 */
static int onic_ptp_gettime64(struct ptp_clock_info *ptp,
			      struct timespec64 *ts)
{
	struct onic_private *priv =
		container_of(ptp, struct onic_private, ptp_info);
	struct onic_hardware *hw = &priv->hw;
	unsigned long flags;
	u32 s_lo, s_hi, ns;

	spin_lock_irqsave(&priv->ptp_lock, flags);

	/* Reading TS_S_LO triggers the snapshot */
	s_lo = onic_read_reg(hw, ONIC_PTP_TS_S_LO);
	s_hi = onic_read_reg(hw, ONIC_PTP_TS_S_HI);
	ns = onic_read_reg(hw, ONIC_PTP_TS_NS);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	ts->tv_sec = ((s64)s_hi << 32) | s_lo;
	ts->tv_nsec = ns;

	return 0;
}

/**
 * onic_ptp_settime64 - Set the PTP hardware clock time
 * @ptp: pointer to ptp_clock_info
 * @ts: pointer to timespec64 containing the new time
 *
 * Writes the seconds and nanoseconds to the SET registers, then
 * triggers the update by writing SET_VALID.
 *
 * Return 0 on success.
 */
static int onic_ptp_settime64(struct ptp_clock_info *ptp,
			      const struct timespec64 *ts)
{
	struct onic_private *priv =
		container_of(ptp, struct onic_private, ptp_info);
	struct onic_hardware *hw = &priv->hw;
	unsigned long flags;

	spin_lock_irqsave(&priv->ptp_lock, flags);

	onic_write_reg(hw, ONIC_PTP_SET_S_LO, (u32)(ts->tv_sec & 0xFFFFFFFF));
	onic_write_reg(hw, ONIC_PTP_SET_S_HI, (u32)(ts->tv_sec >> 32));
	onic_write_reg(hw, ONIC_PTP_SET_NS, (u32)ts->tv_nsec);
	/* Commit: write 1 to SET_VALID */
	onic_write_reg(hw, ONIC_PTP_SET_VALID, 1);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/**
 * onic_ptp_enable - Enable or disable PTP features
 * @ptp: pointer to ptp_clock_info
 * @rq: PTP clock request
 * @on: enable (1) or disable (0)
 *
 * Currently no optional features (PPS, external triggers) are supported.
 *
 * Return -EOPNOTSUPP for unsupported features.
 */
static int onic_ptp_enable(struct ptp_clock_info *ptp,
			   struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

int onic_ptp_init(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	u32 ctrl;
	u16 version;

	spin_lock_init(&priv->ptp_lock);

	/* Probe the PTP block: read the CTRL register.
	 * If the PTP IP is not present, the read returns 0xFFFFFFFF
	 * (unmapped address) or 0x00000000. */
	ctrl = onic_read_reg(hw, ONIC_PTP_CTRL);
	if (ctrl == 0xFFFFFFFF || ctrl == 0x00000000) {
		dev_info(&pdev->dev,
			 "PTP hardware not detected (CTRL=0x%08x), skipping PTP init\n",
			 ctrl);
		priv->ptp_clock = NULL;
		return 0;
	}

	version = (ctrl & ONIC_PTP_CTRL_VERSION_MASK) >>
		  ONIC_PTP_CTRL_VERSION_SHIFT;
	if (version < ONIC_PTP_MIN_VERSION) {
		dev_warn(&pdev->dev,
			 "PTP hardware version 0x%04x < minimum 0x%04x, skipping PTP init\n",
			 version, ONIC_PTP_MIN_VERSION);
		priv->ptp_clock = NULL;
		return 0;
	}

	dev_info(&pdev->dev, "PTP hardware detected, version 0x%04x\n",
		 version);

	/* Set the nominal clock period: 4 ns for 250 MHz */
	onic_write_reg(hw, ONIC_PTP_PERIOD_NS, ONIC_PTP_NOMINAL_PERIOD_NS);
	onic_write_reg(hw, ONIC_PTP_PERIOD_FNS, ONIC_PTP_NOMINAL_PERIOD_FNS);
	onic_write_reg(hw, ONIC_PTP_PERIOD_VALID, 1);

	/* Enable the PTP block */
	onic_write_reg(hw, ONIC_PTP_CTRL, ctrl | ONIC_PTP_CTRL_ENABLE);

	/* Fill in the PTP clock info structure */
	memset(&priv->ptp_info, 0, sizeof(priv->ptp_info));
	snprintf(priv->ptp_info.name, sizeof(priv->ptp_info.name),
		 "onic%ds%df%d",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
	priv->ptp_info.owner = THIS_MODULE;
	priv->ptp_info.max_adj = 500000000; /* 500 ppm */
	priv->ptp_info.adjfine = onic_ptp_adjfine;
	priv->ptp_info.adjtime = onic_ptp_adjtime;
	priv->ptp_info.gettime64 = onic_ptp_gettime64;
	priv->ptp_info.settime64 = onic_ptp_settime64;
	priv->ptp_info.enable = onic_ptp_enable;

	priv->ptp_clock = ptp_clock_register(&priv->ptp_info, &pdev->dev);
	if (IS_ERR(priv->ptp_clock)) {
		dev_err(&pdev->dev, "ptp_clock_register failed: %ld\n",
			PTR_ERR(priv->ptp_clock));
		priv->ptp_clock = NULL;
		return -EINVAL;
	}

	/* Initialize hardware timestamping config to disabled */
	memset(&priv->tstamp_config, 0, sizeof(priv->tstamp_config));

	dev_info(&pdev->dev, "PTP clock registered as /dev/ptp%d\n",
		 ptp_clock_index(priv->ptp_clock));

	return 0;
}

void onic_ptp_cleanup(struct onic_private *priv)
{
	if (priv->ptp_clock) {
		ptp_clock_unregister(priv->ptp_clock);
		priv->ptp_clock = NULL;
		dev_info(&priv->pdev->dev, "PTP clock unregistered\n");
	}
}

int onic_ptp_hwtstamp_set(struct net_device *dev, struct ifreq *ifr)
{
	struct onic_private *priv = netdev_priv(dev);
	struct hwtstamp_config config;

	if (!priv->ptp_clock)
		return -EOPNOTSUPP;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* Reject unsupported flags */
	if (config.flags)
		return -EINVAL;

	/* Validate and store TX timestamping mode */
	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
	case HWTSTAMP_TX_ON:
		break;
	default:
		return -ERANGE;
	}

	/* Validate and store RX filter mode.
	 * We only support all-or-nothing filtering. */
	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_ALL:
	default:
		/* Accept any non-NONE filter as FILTER_ALL */
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	}

	priv->tstamp_config = config;

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
	       -EFAULT : 0;
}

int onic_ptp_hwtstamp_get(struct net_device *dev, struct ifreq *ifr)
{
	struct onic_private *priv = netdev_priv(dev);

	if (!priv->ptp_clock)
		return -EOPNOTSUPP;

	return copy_to_user(ifr->ifr_data, &priv->tstamp_config,
			    sizeof(priv->tstamp_config)) ? -EFAULT : 0;
}
