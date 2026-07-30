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

/*
 * onic_sysfs.c -- per-netdev sysfs attributes.
 *
 * Currently this file publishes exactly one group: the runtime flow-control
 * tuning registers described in shell docs Ch. 13 §13.4 / §13.12 item 5.
 *
 * WHY THIS EXISTS
 * ---------------
 * Pause generation is implemented in the gateware and provably reaches the peer
 * (measured: 140 pause frames emitted, all 140 counted by the peer CX-7), but
 * the total asserted pause time was 34.5 us in 12 s -- a 0.0003 % duty cycle,
 * far too little to change the drop rate.  The XOFF/XON watermarks are too
 * conservative and were compile-time constants, so every candidate value cost a
 * 78-minute FPGA rebuild.  The gateware is gaining CSRs (register map in
 * onic_register.h) to make them runtime-tunable; this is the host-side access so
 * a watermark sweep takes seconds instead of a working day.
 *
 * ATTRIBUTES  (/sys/class/net/<ifname>/)
 *   fc_enable            RW  bitmask: 1 = pause generation, 2 = pause reaction
 *   fc_xoff_watermark    RW  beats; assert XOFF at or above this occupancy
 *   fc_xon_watermark     RW  beats; release XOFF at or below this occupancy
 *   fc_min_xoff_cycles   RW  minimum XOFF hold, cmac_clk cycles
 *   fc_status            RO  decoded: enables, xoff_active, gate, occupancy
 *   fc_xoff_events       RO  XOFF assertions since bitstream load (wraps at 2^32)
 *   fc_xoff_cycles       RO  cycles spent in XOFF (wraps at 2^32)
 *
 * PER-PORT SCOPING -- THE THING MOST LIKELY TO BE GOT WRONG
 * --------------------------------------------------------
 * The two CMACs share one PF, so an attribute on the wrong port would pause the
 * wrong sender.  Every access here derives its register offset from
 * priv->cmac_id of the netdev that owns the sysfs file and from nothing else:
 * there is no module-wide "current port", no iteration over CMACs, and no
 * default index.  onic_fc_regs() is the single place that computes an offset;
 * every show/store goes through it.  See §13.4 "Per-CMAC scoping matters".
 *
 * THIS CODE IS AHEAD OF THE GATEWARE
 * ---------------------------------
 * The bitstream loaded while this was written (build stamp 0x07291754) does not
 * decode these addresses, so none of it has been exercised against real
 * registers.  It is written to fail loudly rather than invent a plausible
 * reading: see onic_fc_probe() and onic_fc_write_verify().
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "onic.h"
#include "onic_register.h"
#include "onic_sysfs.h"

/* This kernel has no netdev_warn_once()/netdev_info_once(), so here they are --
 * rate-limited per (call site, CMAC) rather than per call site, so a message
 * about cmac0 does not suppress the same message about cmac1.  A `cat` loop in a
 * sweep would otherwise either spam the log or hide one port entirely. */
#define onic_fc_log_once(fn, priv, fmt, ...)				\
	do {								\
		static unsigned long __seen;				\
		if (!test_and_set_bit((priv)->cmac_id, &__seen))		\
			fn((priv)->netdev, fmt, ##__VA_ARGS__);		\
	} while (0)

/* Serialises the write+read-back pairs below.  Sysfs writes here are rare
 * (operator/script driven), the register accesses are per-port and independent,
 * and nothing in the datapath takes this lock -- it exists only so two
 * concurrent writers cannot make each other's read-back verification fail and
 * report a spurious -EIO.  Kept file-static rather than per-priv so this feature
 * needs no change to struct onic_private. */
static DEFINE_MUTEX(onic_fc_lock);

/* Validation limits.
 *
 * Watermarks: FC_STATUS reports occupancy in a 16-bit field, so a watermark
 * wider than 16 bits could never be reached and is a typo, not a setting.  The
 * real buffers are far smaller than this (§13.12: 4096 beats for the 9600-byte
 * packet buffer, 256/64 for the plugin FIFO) but the driver does not know the
 * synthesised depths, so it enforces only the bound it can justify from the
 * register map.  A watermark above the true depth is rejected by nothing here
 * and simply never fires -- fc_status' occupancy field is how you find the real
 * depth empirically.
 *
 * Minimum XOFF hold: 322.265625 MHz cmac_clk.  The longest legal 802.3x pause is
 * 65535 quanta = 335 us; the gateware's reaction watchdog bounds a stall at
 * 2^18 cycles (~813 us).  The register is 24 bits wide (FC_MIN_XOFF_W in
 * packet_adapter_register.v) and the gateware SATURATES anything wider rather
 * than truncating it, so the bound here is the exact register width: a value the
 * gateware would saturate must be refused, or the read-back verification below
 * would report the saturated value as a rejected write.  2^24-1 cycles is ~52 ms;
 * anything past 2^20 (~3.3 ms) is allowed but warned about. */
#define ONIC_FC_WM_MAX			0xFFFFu		/* 16-bit registers */
#define ONIC_FC_MIN_XOFF_MAX		0xFFFFFFu	/* 24-bit register */
#define ONIC_FC_MIN_XOFF_WARN		(1u << 20)
#define ONIC_FC_WM_GAP_WARN		4u
#define ONIC_FC_CMAC_CLK_HZ		322265625u

/* Which register a given attribute maps to, resolved per port. */
enum onic_fc_reg {
	ONIC_FC_REG_CTRL,
	ONIC_FC_REG_XOFF_WM,
	ONIC_FC_REG_XON_WM,
	ONIC_FC_REG_MIN_XOFF,
	ONIC_FC_REG_STATUS,
	ONIC_FC_REG_XOFF_EVENTS,
	ONIC_FC_REG_XOFF_CYCLES,
};

enum onic_fc_presence {
	ONIC_FC_PRESENT,	/* registers answer with plausible values */
	ONIC_FC_ABSENT,		/* this bitstream does not implement them */
	ONIC_FC_INDETERMINATE,	/* everything reads 0 -- cannot tell */
};

static struct onic_private *onic_fc_priv(struct device *dev)
{
	return netdev_priv(to_net_dev(dev));
}

/**
 * onic_fc_regs - map (this netdev's CMAC, register) to a BAR2 offset
 *
 * The ONLY place a flow-control register offset is computed.  @priv->cmac_id is
 * the port that owns the sysfs file, so enp194s0 (cmac_id 0) can only ever
 * reach CMAC0's adapter window and enp194s0d1 (cmac_id 1) only CMAC1's.
 *
 * Returns a negative errno if the CMAC index is not one this device has, so a
 * corrupt priv can never be turned into an access to an arbitrary BAR offset.
 **/
static int onic_fc_regs(struct onic_private *priv, enum onic_fc_reg which,
			u32 *offset)
{
	u8 c = priv->cmac_id;

	if (!priv->hw.addr)
		return -ENODEV;
	if (c >= priv->hw.num_cmacs || c >= ONIC_MAX_CMACS)
		return -ENODEV;

	switch (which) {
	case ONIC_FC_REG_CTRL:
		*offset = CMAC_ADPT_OFFSET_FC_CTRL(c);		break;
	case ONIC_FC_REG_XOFF_WM:
		*offset = CMAC_ADPT_OFFSET_FC_XOFF_WM(c);	break;
	case ONIC_FC_REG_XON_WM:
		*offset = CMAC_ADPT_OFFSET_FC_XON_WM(c);	break;
	case ONIC_FC_REG_MIN_XOFF:
		*offset = CMAC_ADPT_OFFSET_FC_MIN_XOFF(c);	break;
	case ONIC_FC_REG_STATUS:
		*offset = CMAC_ADPT_OFFSET_FC_STATUS(c);	break;
	case ONIC_FC_REG_XOFF_EVENTS:
		*offset = CMAC_ADPT_OFFSET_FC_XOFF_EVENTS(c);	break;
	case ONIC_FC_REG_XOFF_CYCLES:
		*offset = CMAC_ADPT_OFFSET_FC_XOFF_CYCLES(c);	break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int onic_fc_read(struct onic_private *priv, enum onic_fc_reg which,
			u32 *val)
{
	u32 offset;
	int rv;

	rv = onic_fc_regs(priv, which, &offset);
	if (rv < 0)
		return rv;
	*val = onic_read_reg(&priv->hw, offset);
	return 0;
}

/**
 * onic_fc_probe - decide whether this bitstream implements the FC registers
 *
 * There is a definitive signal available.  The adapter register block answers
 * every undecoded offset in its 4 KB window with its case default,
 * 0xDEADBEEF (packet_adapter_register.v:238), so a bitstream without this block
 * is recognisable from one read.  All-ones on every register is the other
 * classic no-decode reply and is treated the same way.
 *
 * The one case that cannot be resolved is every register reading 0: that is
 * indistinguishable from an implemented block sitting at its reset defaults with
 * flow control disabled.  It is reported as INDETERMINATE rather than guessed,
 * and a write's read-back (onic_fc_write_verify) settles it -- a discarded write
 * reads back 0, a real register reads back what was written.
 **/
static enum onic_fc_presence onic_fc_probe(struct onic_private *priv)
{
	u32 ctrl = 0, status = 0, xoff = 0;

	if (onic_fc_read(priv, ONIC_FC_REG_CTRL, &ctrl) < 0 ||
	    onic_fc_read(priv, ONIC_FC_REG_STATUS, &status) < 0 ||
	    onic_fc_read(priv, ONIC_FC_REG_XOFF_WM, &xoff) < 0)
		return ONIC_FC_ABSENT;

	if (ctrl == ONIC_FC_ABSENT_MAGIC || status == ONIC_FC_ABSENT_MAGIC ||
	    xoff == ONIC_FC_ABSENT_MAGIC)
		return ONIC_FC_ABSENT;

	if (ctrl == ONIC_FC_ABSENT_ONES && status == ONIC_FC_ABSENT_ONES &&
	    xoff == ONIC_FC_ABSENT_ONES)
		return ONIC_FC_ABSENT;

	if (!ctrl && !status && !xoff)
		return ONIC_FC_INDETERMINATE;

	return ONIC_FC_PRESENT;
}

/* Common gate for every accessor: refuse to present or accept a value when the
 * gateware demonstrably has no such register.  -EOPNOTSUPP shows up as
 * "Operation not supported" from cat/echo, which is unambiguous; returning a
 * number would let a sweep record 0xDEADBEEF as a measurement. */
static int onic_fc_check(struct onic_private *priv)
{
	switch (onic_fc_probe(priv)) {
	case ONIC_FC_ABSENT:
		onic_fc_log_once(netdev_warn, priv,
				 "fc: cmac%u flow-control CSRs are not implemented by the loaded bitstream (adapter window answers 0x%08X for undecoded offsets); sysfs flow-control attributes are unavailable\n",
				 priv->cmac_id, ONIC_FC_ABSENT_MAGIC);
		return -EOPNOTSUPP;
	case ONIC_FC_INDETERMINATE:
		onic_fc_log_once(netdev_info, priv,
				 "fc: cmac%u flow-control CSRs read all-zero -- either implemented and at reset defaults, or not implemented in a way that reads 0; a write's read-back will settle it\n",
				 priv->cmac_id);
		return 0;
	default:
		return 0;
	}
}

/* ------------------------------------------------------------ show helpers -- */

static ssize_t onic_fc_show_reg(struct device *dev, char *buf,
				enum onic_fc_reg which)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val;
	int rv;

	rv = onic_fc_check(priv);
	if (rv < 0)
		return rv;
	rv = onic_fc_read(priv, which, &val);
	if (rv < 0)
		return rv;

	return sysfs_emit(buf, "%u\n", val);
}

/* ----------------------------------------------------------- store helpers -- */

/**
 * onic_fc_write_verify - write a register and prove the gateware took the value
 *
 * Everything written here is a plain RW register per the contract, so the value
 * read back must equal the value written (within @mask for registers with
 * reserved bits).  Two failures are distinguished because they mean different
 * things to the operator:
 *
 *   read-back is 0xDEADBEEF, or 0 where a non-zero value was written
 *       -> the address is not decoded; the write went nowhere.  -EOPNOTSUPP.
 *   read-back is some other unexpected value
 *       -> the register exists but did not accept the value as given (a clamp
 *          in the gateware would look like this).  -EIO, and the log names both
 *          values so the real setting is visible.
 *
 * Either way the caller's write is reported as failed, which matters: a tuning
 * sweep must not record a setting it did not actually apply.
 **/
static int onic_fc_write_verify(struct onic_private *priv,
				enum onic_fc_reg which, u32 val, u32 mask,
				const char *name)
{
	struct net_device *netdev = priv->netdev;
	u32 offset, back;
	int rv;

	rv = onic_fc_regs(priv, which, &offset);
	if (rv < 0)
		return rv;

	onic_write_reg(&priv->hw, offset, val);
	back = onic_read_reg(&priv->hw, offset);

	if (back == ONIC_FC_ABSENT_MAGIC || (val && !(back & mask))) {
		netdev_err(netdev,
			   "fc: cmac%u %s write of %u was discarded (read back 0x%08X) -- this bitstream does not implement the flow-control CSRs\n",
			   priv->cmac_id, name, val, back);
		return -EOPNOTSUPP;
	}
	if ((back & mask) != (val & mask)) {
		netdev_err(netdev,
			   "fc: cmac%u %s read back %u after writing %u (raw 0x%08X) -- the gateware rejected or clamped it; the register now holds the read-back value\n",
			   priv->cmac_id, name, back & mask, val & mask, back);
		return -EIO;
	}

	netdev_info(netdev, "fc: cmac%u %s = %u\n", priv->cmac_id, name,
		    val & mask);
	return 0;
}

/* ---------------------------------------------------------- fc_enable (RW) -- */

static ssize_t fc_enable_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val;
	int rv;

	rv = onic_fc_check(priv);
	if (rv < 0)
		return rv;
	rv = onic_fc_read(priv, ONIC_FC_REG_CTRL, &val);
	if (rv < 0)
		return rv;

	if (val & ~CMAC_ADPT_FC_CTRL_MASK)
		onic_fc_log_once(netdev_warn, priv,
				 "fc: cmac%u FC_CTRL reads 0x%08X -- reserved bits are set, which the agreed register map does not define; reporting bits 1:0 only\n",
				 priv->cmac_id, val);

	return sysfs_emit(buf, "%u\n", (u32)(val & CMAC_ADPT_FC_CTRL_MASK));
}

static ssize_t fc_enable_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val;
	int rv;

	rv = kstrtou32(buf, 0, &val);
	if (rv < 0)
		return rv;

	if (val & ~CMAC_ADPT_FC_CTRL_MASK) {
		netdev_err(priv->netdev,
			   "fc_enable: %u is out of range; valid values are 0..%u (bit0 = pause generation, bit1 = pause reaction)\n",
			   val, (unsigned int)CMAC_ADPT_FC_CTRL_MASK);
		return -EINVAL;
	}

	mutex_lock(&onic_fc_lock);
	rv = onic_fc_check(priv);
	if (!rv) {
		u32 xoff = 0, xon = 0;

		/* Refuse to arm generation on top of a watermark pair that
		 * cannot work -- enabling with xoff == 0 would hold XOFF
		 * permanently and stall the peer, which is worse than the drops
		 * this feature exists to remove. */
		if ((val & CMAC_ADPT_FC_CTRL_GEN_EN) &&
		    !onic_fc_read(priv, ONIC_FC_REG_XOFF_WM, &xoff) &&
		    !onic_fc_read(priv, ONIC_FC_REG_XON_WM, &xon) &&
		    (xoff == 0 || xon >= xoff)) {
			netdev_err(priv->netdev,
				   "fc_enable: refusing to enable pause generation on cmac%u with xoff=%u xon=%u; set fc_xon_watermark then fc_xoff_watermark to a valid pair (0 < xon < xoff) first\n",
				   priv->cmac_id, xoff, xon);
			rv = -EINVAL;
		} else {
			rv = onic_fc_write_verify(priv, ONIC_FC_REG_CTRL, val,
						  CMAC_ADPT_FC_CTRL_MASK,
						  "fc_enable");
			if (!rv && (val & CMAC_ADPT_FC_CTRL_REACT_EN))
				netdev_warn(priv->netdev,
					    "fc: cmac%u pause REACTION enabled -- this path has never been exercised on hardware (stat_rx_pause has been 0 for the life of this design); it is bounded by the gateware watchdog but test it alone (shell docs Ch. 13 §13.12 risk 2)\n",
					    priv->cmac_id);
		}
	}
	mutex_unlock(&onic_fc_lock);

	return rv ? rv : len;
}
static DEVICE_ATTR_RW(fc_enable);

/* -------------------------------------------------- fc_xoff_watermark (RW) -- */

static ssize_t fc_xoff_watermark_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return onic_fc_show_reg(dev, buf, ONIC_FC_REG_XOFF_WM);
}

static ssize_t fc_xoff_watermark_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val, xon = 0;
	int rv;

	rv = kstrtou32(buf, 0, &val);
	if (rv < 0)
		return rv;

	/* 0 means "assert XOFF at zero occupancy", i.e. never release: the link
	 * would be paused for ever. */
	if (val == 0) {
		netdev_err(priv->netdev,
			   "fc_xoff_watermark: 0 would hold XOFF permanently and stall the peer; use a positive occupancy in beats\n");
		return -EINVAL;
	}
	if (val > ONIC_FC_WM_MAX) {
		netdev_err(priv->netdev,
			   "fc_xoff_watermark: %u exceeds %u; FC_STATUS reports occupancy in 16 bits, so a wider watermark can never be reached\n",
			   val, ONIC_FC_WM_MAX);
		return -EINVAL;
	}

	mutex_lock(&onic_fc_lock);
	rv = onic_fc_check(priv);
	if (!rv)
		rv = onic_fc_read(priv, ONIC_FC_REG_XON_WM, &xon);
	if (!rv && val <= xon) {
		/* Hysteresis, not a single threshold: XOFF must be strictly
		 * above XON or the trigger latches and pause is toggled every
		 * beat.  Order matters when lowering a pair -- say so instead of
		 * making the caller guess. */
		netdev_err(priv->netdev,
			   "fc_xoff_watermark: %u is not above the current fc_xon_watermark (%u); write the lower fc_xon_watermark first (0 is accepted), then this\n",
			   val, xon);
		rv = -EINVAL;
	}
	if (!rv) {
		if (val - xon < ONIC_FC_WM_GAP_WARN)
			netdev_warn(priv->netdev,
				    "fc: cmac%u xoff-xon hysteresis is only %u beat(s); expect pause chatter\n",
				    priv->cmac_id, val - xon);
		rv = onic_fc_write_verify(priv, ONIC_FC_REG_XOFF_WM, val,
					  U32_MAX, "fc_xoff_watermark");
	}
	mutex_unlock(&onic_fc_lock);

	return rv ? rv : len;
}
static DEVICE_ATTR_RW(fc_xoff_watermark);

/* --------------------------------------------------- fc_xon_watermark (RW) -- */

static ssize_t fc_xon_watermark_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return onic_fc_show_reg(dev, buf, ONIC_FC_REG_XON_WM);
}

static ssize_t fc_xon_watermark_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val, xoff = 0;
	int rv;

	rv = kstrtou32(buf, 0, &val);
	if (rv < 0)
		return rv;

	/* 0 is legal here: drain the buffer completely before releasing.  It is
	 * also the value that makes a watermark pair writable in any order --
	 * see the sweep helper in tools/. */
	if (val > ONIC_FC_WM_MAX) {
		netdev_err(priv->netdev,
			   "fc_xon_watermark: %u exceeds %u; FC_STATUS reports occupancy in 16 bits, so a wider watermark can never be reached\n",
			   val, ONIC_FC_WM_MAX);
		return -EINVAL;
	}

	mutex_lock(&onic_fc_lock);
	rv = onic_fc_check(priv);
	if (!rv)
		rv = onic_fc_read(priv, ONIC_FC_REG_XOFF_WM, &xoff);
	if (!rv && val >= xoff) {
		netdev_err(priv->netdev,
			   "fc_xon_watermark: %u is not below the current fc_xoff_watermark (%u); XON must be strictly under XOFF or the hysteresis latches. Raise fc_xoff_watermark first\n",
			   val, xoff);
		rv = -EINVAL;
	}
	if (!rv) {
		if (xoff - val < ONIC_FC_WM_GAP_WARN)
			netdev_warn(priv->netdev,
				    "fc: cmac%u xoff-xon hysteresis is only %u beat(s); expect pause chatter\n",
				    priv->cmac_id, xoff - val);
		rv = onic_fc_write_verify(priv, ONIC_FC_REG_XON_WM, val,
					  U32_MAX, "fc_xon_watermark");
	}
	mutex_unlock(&onic_fc_lock);

	return rv ? rv : len;
}
static DEVICE_ATTR_RW(fc_xon_watermark);

/* ------------------------------------------------- fc_min_xoff_cycles (RW) -- */

static ssize_t fc_min_xoff_cycles_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return onic_fc_show_reg(dev, buf, ONIC_FC_REG_MIN_XOFF);
}

static ssize_t fc_min_xoff_cycles_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 val;
	int rv;

	rv = kstrtou32(buf, 0, &val);
	if (rv < 0)
		return rv;

	if (val > ONIC_FC_MIN_XOFF_MAX) {
		netdev_err(priv->netdev,
			   "fc_min_xoff_cycles: %u exceeds the register's %u (~%u ms at %u Hz), which the gateware would silently saturate; a hold that long turns a drop problem into a link stall anyway\n",
			   val, ONIC_FC_MIN_XOFF_MAX,
			   ONIC_FC_MIN_XOFF_MAX / (ONIC_FC_CMAC_CLK_HZ / 1000),
			   ONIC_FC_CMAC_CLK_HZ);
		return -EINVAL;
	}

	mutex_lock(&onic_fc_lock);
	rv = onic_fc_check(priv);
	if (!rv)
		rv = onic_fc_write_verify(priv, ONIC_FC_REG_MIN_XOFF, val,
					  U32_MAX, "fc_min_xoff_cycles");
	if (!rv) {
		/* Report the time equivalent: cycles are what the register takes
		 * but microseconds are what the operator is reasoning about. */
		netdev_info(priv->netdev,
			    "fc: cmac%u minimum XOFF hold = %u cycles (~%u ns at %u Hz)\n",
			    priv->cmac_id, val,
			    (u32)div_u64((u64)val * 1000000000ULL,
					 ONIC_FC_CMAC_CLK_HZ),
			    ONIC_FC_CMAC_CLK_HZ);
		if (val == 0)
			netdev_warn(priv->netdev,
				    "fc: cmac%u minimum XOFF hold is 0 -- XOFF then depends purely on the watermark hysteresis and may chatter one pause frame per crossing\n",
				    priv->cmac_id);
		else if (val > ONIC_FC_MIN_XOFF_WARN)
			netdev_warn(priv->netdev,
				    "fc: cmac%u minimum XOFF hold of %u cycles is longer than the longest legal 802.3x pause (65535 quanta ~ 335 us); the peer will resume before we release unless the CMAC refresh timer covers it (shell docs Ch. 13 §13.12 risk 1)\n",
				    priv->cmac_id, val);
	}
	mutex_unlock(&onic_fc_lock);

	return rv ? rv : len;
}
static DEVICE_ATTR_RW(fc_min_xoff_cycles);

/* ---------------------------------------------------------- fc_status (RO) -- */

/*
 * Decoded, not a raw hex word.  The raw value is printed last so it is still
 * available for a bug report, but nobody has to decode bit fields by hand while
 * running a sweep.  Occupancy is instantaneous -- a sample taken while idle says
 * nothing; read it during traffic.
 */
static ssize_t fc_status_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct onic_private *priv = onic_fc_priv(dev);
	u32 status, ctrl = 0, xoff_wm = 0, xon_wm = 0, min_xoff = 0;
	u32 events = 0, cycles = 0, occupancy;
	int rv, len = 0;

	rv = onic_fc_check(priv);
	if (rv < 0)
		return rv;
	rv = onic_fc_read(priv, ONIC_FC_REG_STATUS, &status);
	if (rv < 0)
		return rv;

	onic_fc_read(priv, ONIC_FC_REG_CTRL, &ctrl);
	onic_fc_read(priv, ONIC_FC_REG_XOFF_WM, &xoff_wm);
	onic_fc_read(priv, ONIC_FC_REG_XON_WM, &xon_wm);
	onic_fc_read(priv, ONIC_FC_REG_MIN_XOFF, &min_xoff);
	onic_fc_read(priv, ONIC_FC_REG_XOFF_EVENTS, &events);
	onic_fc_read(priv, ONIC_FC_REG_XOFF_CYCLES, &cycles);

	occupancy = (status & CMAC_ADPT_FC_STATUS_OCCUPANCY) >>
		    CMAC_ADPT_FC_STATUS_OCCUPANCY_SHIFT;

	len += sysfs_emit_at(buf, len, "port:            %s (cmac%u)\n",
			     netdev_name(priv->netdev), priv->cmac_id);
	len += sysfs_emit_at(buf, len, "pause_gen:       %s\n",
			     (ctrl & CMAC_ADPT_FC_CTRL_GEN_EN) ? "enabled"
							      : "disabled");
	len += sysfs_emit_at(buf, len, "pause_react:     %s\n",
			     (ctrl & CMAC_ADPT_FC_CTRL_REACT_EN) ? "enabled"
							        : "disabled");
	len += sysfs_emit_at(buf, len, "xoff_active:     %s\n",
			     (status & CMAC_ADPT_FC_STATUS_XOFF_ACTIVE)
				     ? "yes (asserting pause now)" : "no");
	len += sysfs_emit_at(buf, len, "tx_pause_gate:   %s\n",
			     (status & CMAC_ADPT_FC_STATUS_TX_PAUSE_GATE)
				     ? "closed (our TX is held off by a peer pause)"
				     : "open");
	len += sysfs_emit_at(buf, len, "occupancy:       %u beats\n", occupancy);
	len += sysfs_emit_at(buf, len,
			     "watermarks:      xoff %u  xon %u  min_hold %u cycles (~%u ns)\n",
			     xoff_wm, xon_wm, min_xoff,
			     (u32)div_u64((u64)min_xoff * 1000000000ULL,
					  ONIC_FC_CMAC_CLK_HZ));
	len += sysfs_emit_at(buf, len, "xoff_events:     %u\n", events);
	len += sysfs_emit_at(buf, len,
			     "xoff_cycles:     %u (~%u us in XOFF)\n", cycles,
			     (u32)div_u64((u64)cycles * 1000000ULL,
					  ONIC_FC_CMAC_CLK_HZ));

	/* Both free-running counters wrap at 2^32; at 322.27 MHz that is 13.3 s
	 * of continuous XOFF for the cycle counter.  Say so rather than let a
	 * long capture be misread. */
	len += sysfs_emit_at(buf, len,
			     "note:            counters are free-running 32-bit and wrap (xoff_cycles wraps after ~13.3 s of XOFF); difference two reads\n");

	if (onic_fc_probe(priv) == ONIC_FC_INDETERMINATE)
		len += sysfs_emit_at(buf, len,
				     "WARNING:         every flow-control register reads 0. This is either an implemented block at reset defaults, or a bitstream that does not implement them at all. Write a watermark and read it back to tell which.\n");

	len += sysfs_emit_at(buf, len, "raw_status:      0x%08X\n", status);

	return len;
}
static DEVICE_ATTR_RO(fc_status);

/* ---------------------------------------------- fc_xoff_events/cycles (RO) -- */

static ssize_t fc_xoff_events_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return onic_fc_show_reg(dev, buf, ONIC_FC_REG_XOFF_EVENTS);
}
static DEVICE_ATTR_RO(fc_xoff_events);

static ssize_t fc_xoff_cycles_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return onic_fc_show_reg(dev, buf, ONIC_FC_REG_XOFF_CYCLES);
}
static DEVICE_ATTR_RO(fc_xoff_cycles);

/* ------------------------------------------------------------------ group -- */

static struct attribute *onic_fc_attrs[] = {
	&dev_attr_fc_enable.attr,
	&dev_attr_fc_xoff_watermark.attr,
	&dev_attr_fc_xon_watermark.attr,
	&dev_attr_fc_min_xoff_cycles.attr,
	&dev_attr_fc_status.attr,
	&dev_attr_fc_xoff_events.attr,
	&dev_attr_fc_xoff_cycles.attr,
	NULL,
};

/* Unnamed group: the attributes land directly in /sys/class/net/<ifname>/.
 * The fc_ prefix keeps them out of the kernel's own namespace there. */
ATTRIBUTE_GROUPS(onic_fc);

void onic_sysfs_attach_groups(struct net_device *netdev)
{
	unsigned int i;

	/* net_device::sysfs_groups is a fixed 4-slot array that
	 * netdev_register_kobject() hands to the driver core as ->groups.  Take
	 * the first free slot rather than assuming slot 0, so this stays correct
	 * if another group is added later. */
	for (i = 0; i < ARRAY_SIZE(netdev->sysfs_groups); i++) {
		if (!netdev->sysfs_groups[i]) {
			netdev->sysfs_groups[i] = onic_fc_groups[0];
			return;
		}
	}

	netdev_warn(netdev,
		    "sysfs: no free sysfs_groups slot; flow-control attributes not published\n");
}
