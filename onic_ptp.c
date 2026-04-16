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

/* Forward declarations — adjtime's large-delta fallback needs these */
static int onic_ptp_gettime64(struct ptp_clock_info *ptp,
			      struct timespec64 *ts);
static int onic_ptp_settime64(struct ptp_clock_info *ptp,
			      const struct timespec64 *ts);

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
	u32 period_ns, period_fns;

	/*
	 * Adjust the clock by modifying the PERIOD registers directly,
	 * following the same approach as Corundum (mqnic_phc_adjfine).
	 *
	 * The DRIFT mechanism in ptp_clock.v has a Verilog signedness bug
	 * (ternary with unsigned 0 literal), so we use PERIOD instead.
	 *
	 * scaled_ppm is ppb * 2^16 / 1000 (i.e., ppm with 16-bit fraction).
	 * delta = nominal * scaled_ppm / (10^6 * 2^16)
	 *
	 * Positive scaled_ppm → increase period → clock advances faster.
	 */
	if (scaled_ppm < 0) {
		negative = true;
		scaled_ppm = -scaled_ppm;
	}

	{
		u32 nominal = (ONIC_PTP_NOMINAL_PERIOD_NS << 16) |
			      ONIC_PTP_NOMINAL_PERIOD_FNS;
		u32 delta;
		u32 new_period;

		adj = div_u64((u64)nominal * (u64)scaled_ppm, 1000000);
		adj >>= 16;
		delta = (u32)adj;

		if (negative)
			new_period = nominal - delta;
		else
			new_period = nominal + delta;

		period_ns = (new_period >> 16) & 0xF;
		period_fns = new_period & 0xFFFF;
	}

	spin_lock_irqsave(&priv->ptp_lock, flags);

	onic_write_reg(hw, ONIC_PTP_PERIOD_NS, period_ns);
	onic_write_reg(hw, ONIC_PTP_PERIOD_FNS, period_fns);
	onic_write_reg(hw, ONIC_PTP_PERIOD_VALID, 1);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/**
 * onic_ptp_adjtime - Adjust PTP clock time by a delta
 * @ptp: pointer to ptp_clock_info
 * @delta: time adjustment in nanoseconds
 *
 * Always uses read-modify-write via gettime/settime.  The FPGA ADJ_NS
 * register is only 4 bits wide (signed range -8..+7 ns), far too narrow
 * for the offsets ptp4l needs to correct.  The gettime/settime path has
 * no such limitation and works for any delta.
 *
 * Return 0 on success.
 */
static int onic_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct onic_private *priv =
		container_of(ptp, struct onic_private, ptp_info);
	struct timespec64 ts;

	dev_info(&priv->pdev->dev,
		 "PTP adjtime: delta=%lld ns (%lld.%09lld s)\n",
		 delta, delta / 1000000000LL,
		 delta < 0 ? -(delta % 1000000000LL) : delta % 1000000000LL);

	onic_ptp_gettime64(ptp, &ts);
	ts = timespec64_add(ts, ns_to_timespec64(delta));
	return onic_ptp_settime64(ptp, &ts);
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
	u32 ctrl_before, ctrl_after;

	/* Read CDC locked status before the step */
	ctrl_before = onic_read_reg(hw, ONIC_PTP_CTRL);

	spin_lock_irqsave(&priv->ptp_lock, flags);

	onic_write_reg(hw, ONIC_PTP_SET_S_LO, (u32)(ts->tv_sec & 0xFFFFFFFF));
	onic_write_reg(hw, ONIC_PTP_SET_S_HI, (u32)(ts->tv_sec >> 32));
	onic_write_reg(hw, ONIC_PTP_SET_NS, (u32)ts->tv_nsec);
	/* Commit: write 1 to SET_VALID */
	onic_write_reg(hw, ONIC_PTP_SET_VALID, 1);

	spin_unlock_irqrestore(&priv->ptp_lock, flags);

	/* Read CDC locked status after the step */
	ctrl_after = onic_read_reg(hw, ONIC_PTP_CTRL);

	dev_info(&priv->pdev->dev,
		 "PTP settime64: sec=%lld ns=%ld cdc_locked=[before=0x%x after=0x%x] (p0_tx=%d p0_rx=%d p1_tx=%d p1_rx=%d)\n",
		 ts->tv_sec, ts->tv_nsec,
		 (ctrl_before >> 8) & 0xF, (ctrl_after >> 8) & 0xF,
		 (ctrl_after >> 8) & 1, (ctrl_after >> 9) & 1,
		 (ctrl_after >> 10) & 1, (ctrl_after >> 11) & 1);

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

/**
 * onic_ptp_alloc_tx_tag - Allocate a PTP tag for TX timestamping
 * @priv: pointer to driver private data
 * @skb: the original skb being transmitted
 * @tag_out: pointer to store the allocated 16-bit PTP tag
 *
 * Clones the skb, assigns a unique tag, and stores the pending entry.
 * The SKBTX_IN_PROGRESS flag is set on the original skb so the stack
 * knows a hardware timestamp is forthcoming.
 *
 * Return 0 on success, -EBUSY if no free slot, -ENOMEM if clone fails.
 */
int onic_ptp_alloc_tx_tag(struct onic_private *priv, struct sk_buff *skb,
			   u16 *tag_out)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&priv->ptp_tx_lock, flags);

	/* Find a free slot */
	for (i = 0; i < ONIC_PTP_TX_PENDING_MAX; i++) {
		if (!priv->ptp_tx_pending[i].active)
			break;
	}
	if (i == ONIC_PTP_TX_PENDING_MAX) {
		spin_unlock_irqrestore(&priv->ptp_tx_lock, flags);
		return -EBUSY;
	}

	/* Assign tag: increment, skip 0, wrap at 65535 */
	priv->ptp_next_tag++;
	if (priv->ptp_next_tag == 0)
		priv->ptp_next_tag = 1;

	/* Hold a reference to the original skb rather than cloning.
	 * skb_clone_sk() fails for PF_PACKET (L2) sockets because the
	 * socket refcount cannot be atomically incremented from xmit
	 * context.  skb_get() on the original preserves skb->sk, which
	 * skb_tstamp_tx() needs to deliver the timestamp via the
	 * socket error queue.  This matches the igb/ice driver approach.
	 *
	 * The extra reference prevents the skb from being freed in
	 * onic_tx_clean(); we release it in the poll/timeout path. */
	skb_get(skb);
	priv->ptp_tx_pending[i].skb = skb;
	priv->ptp_tx_pending[i].tag = priv->ptp_next_tag;
	priv->ptp_tx_pending[i].start = ktime_get();
	priv->ptp_tx_pending[i].active = true;

	/* Tell the stack that a HW timestamp is in progress */
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;

	*tag_out = priv->ptp_next_tag;

	spin_unlock_irqrestore(&priv->ptp_tx_lock, flags);

	/* Kick the poll workqueue to collect the timestamp when it arrives */
	schedule_delayed_work(&priv->ptp_tx_work, msecs_to_jiffies(1));

	return 0;
}

/**
 * onic_ptp_tx_ts_poll - Poll the TX timestamp FIFO and deliver timestamps
 * @priv: pointer to driver private data
 *
 * Reads completed TX timestamps from the FPGA FIFO, matches them to
 * pending skbs by tag, and delivers the hardware timestamp to userspace
 * via skb_tstamp_tx().
 */
void onic_ptp_tx_ts_poll(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	u16 func_id = PCI_FUNC(priv->pdev->devfn);
	int port = (func_id < hw->num_cmacs) ? func_id : 0;
	u32 valid, ts_lo, ts_hi, ts_tag_reg;
	u16 tag;
	u32 sec_lo, nsec;
	unsigned long flags;
	int i, drain = 0;

	valid = onic_read_reg(hw, ONIC_PTP_TX_TS_VALID(port));

	/* Limit drain iterations to prevent soft lockup if FPGA FIFO
	 * valid bit is stuck high.  64 is far more than the pending max. */
	while ((valid & 1) && drain++ < ONIC_PTP_TX_PENDING_MAX * 2) {
		ts_lo = onic_read_reg(hw, ONIC_PTP_TX_TS_LO(port));
		ts_hi = onic_read_reg(hw, ONIC_PTP_TX_TS_HI(port));
		ts_tag_reg = onic_read_reg(hw, ONIC_PTP_TX_TS_TAG(port));

		tag = (u16)(ts_tag_reg >> 16);
		sec_lo = ts_hi;               /* seconds[31:0] */
		nsec = ts_lo & 0x3FFFFFFF;    /* {2'b00, ns[29:0]} -> ns[29:0] */

		/* Diagnostic: compare TX TS to current PTP clock */
		{
			u32 now_s = onic_read_reg(hw, 0x18010);
			u32 now_ns = onic_read_reg(hw, 0x18018) & 0x3FFFFFFF;
			s64 tx_ns = (s64)sec_lo * 1000000000LL + nsec;
			s64 now_total = (s64)now_s * 1000000000LL + now_ns;
			dev_info(&priv->pdev->dev,
				"PTP TX DIAG: tag=%u tx_sec=%u tx_ns=%u now_sec=%u now_ns=%u delta_ms=%lld\n",
				tag, sec_lo, nsec, now_s, now_ns,
				(now_total - tx_ns) / 1000000);
		}

		{
			struct sk_buff *deliver_skb = NULL;
			struct skb_shared_hwtstamps hwts;

			memset(&hwts, 0, sizeof(hwts));
			hwts.hwtstamp = ktime_set((s64)sec_lo, nsec);

			spin_lock_irqsave(&priv->ptp_tx_lock, flags);
			for (i = 0; i < ONIC_PTP_TX_PENDING_MAX; i++) {
				if (priv->ptp_tx_pending[i].active &&
				    priv->ptp_tx_pending[i].tag == tag) {
					deliver_skb = priv->ptp_tx_pending[i].skb;
					priv->ptp_tx_pending[i].skb = NULL;
					priv->ptp_tx_pending[i].active = false;
					break;
				}
			}
			if (i == ONIC_PTP_TX_PENDING_MAX)
				dev_dbg(&priv->pdev->dev,
					"PTP TX TS NO MATCH: tag=%u (no active pending entry)\n",
					tag);
			spin_unlock_irqrestore(&priv->ptp_tx_lock, flags);

			/* Deliver outside the spinlock to avoid calling
			 * skb_tstamp_tx / kfree_skb with IRQs disabled. */
			if (deliver_skb) {
				dev_dbg(&priv->pdev->dev,
					"PTP TX TS DELIVER: tag=%u hwtstamp=%lld sk=%px\n",
					tag, ktime_to_ns(hwts.hwtstamp),
					deliver_skb->sk);
				skb_tstamp_tx(deliver_skb, &hwts);
				kfree_skb(deliver_skb);
			}
		}

		/* Pop the FIFO entry — FPGA requires a write to TX_TS_VALID */
		onic_write_reg(hw, ONIC_PTP_TX_TS_VALID(port), 1);

		/* Check for more entries */
		valid = onic_read_reg(hw, ONIC_PTP_TX_TS_VALID(port));
	}

	if (drain >= ONIC_PTP_TX_PENDING_MAX * 2)
		dev_warn_ratelimited(&priv->pdev->dev,
			"PTP TX TS FIFO drain limit hit (valid still %u) — FPGA FIFO may be stuck\n",
			valid);
}

/**
 * onic_ptp_tx_ts_work - Delayed work handler for TX timestamp polling
 * @work: pointer to the work_struct embedded in onic_private
 *
 * Polls the TX timestamp FIFO, times out stale pending entries, and
 * reschedules itself while TX timestamping is enabled.
 */
static void onic_ptp_tx_ts_work(struct work_struct *work)
{
	struct onic_private *priv =
		container_of(work, struct onic_private, ptp_tx_work.work);
	unsigned long flags;
	s64 elapsed_ns;
	int i;
	bool have_pending = false;

	onic_ptp_tx_ts_poll(priv);

	/* Timeout check: free stale pending entries */
	spin_lock_irqsave(&priv->ptp_tx_lock, flags);
	for (i = 0; i < ONIC_PTP_TX_PENDING_MAX; i++) {
		if (!priv->ptp_tx_pending[i].active)
			continue;
		have_pending = true;
		elapsed_ns = ktime_to_ns(ktime_sub(ktime_get(),
					 priv->ptp_tx_pending[i].start));
		if (elapsed_ns > (s64)ONIC_PTP_TX_TS_POLL_TIMEOUT_US * 1000) {
			kfree_skb(priv->ptp_tx_pending[i].skb);
			priv->ptp_tx_pending[i].skb = NULL;
			priv->ptp_tx_pending[i].active = false;
		}
	}
	spin_unlock_irqrestore(&priv->ptp_tx_lock, flags);

	/* Only reschedule if there are still pending timestamps to collect.
	 * New TX timestamps kick the workqueue via onic_ptp_alloc_tx_tag(). */
	if (have_pending)
		schedule_delayed_work(&priv->ptp_tx_work, msecs_to_jiffies(1));
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

	/* Set the nominal clock period */
	onic_write_reg(hw, ONIC_PTP_PERIOD_NS, ONIC_PTP_NOMINAL_PERIOD_NS);
	onic_write_reg(hw, ONIC_PTP_PERIOD_FNS, ONIC_PTP_NOMINAL_PERIOD_FNS);
	onic_write_reg(hw, ONIC_PTP_PERIOD_VALID, 1);

	/* Clear drift registers — stale values from a previous driver load
	 * persist across rmmod/insmod since the FPGA isn't reset. */
	onic_write_reg(hw, ONIC_PTP_DRIFT_NS, 0);
	onic_write_reg(hw, ONIC_PTP_DRIFT_FNS, 0);
	onic_write_reg(hw, ONIC_PTP_DRIFT_RATE, 0);
	onic_write_reg(hw, ONIC_PTP_DRIFT_VALID, 1);

	/* Enable the PTP block */
	onic_write_reg(hw, ONIC_PTP_CTRL, ctrl | ONIC_PTP_CTRL_ENABLE);

	/* Diagnostic: dump PTP and TX TS FIFO registers */
	{
		u32 ctrl_rb = onic_read_reg(hw, ONIC_PTP_CTRL);
		u32 ts_ns   = onic_read_reg(hw, ONIC_PTP_TS_NS);
		u32 ts_s_lo = onic_read_reg(hw, ONIC_PTP_TS_S_LO);
		u32 ts_lo   = onic_read_reg(hw, ONIC_PTP_TX_TS_LO(0));
		u32 ts_hi   = onic_read_reg(hw, ONIC_PTP_TX_TS_HI(0));
		u32 ts_tag  = onic_read_reg(hw, ONIC_PTP_TX_TS_TAG(0));
		u32 ts_val  = onic_read_reg(hw, ONIC_PTP_TX_TS_VALID(0));
		dev_info(&pdev->dev,
			 "PTP diag: CTRL=0x%08x time=%u.%u TX_TS[0]: LO=0x%08x HI=0x%08x TAG=0x%08x VALID=0x%08x\n",
			 ctrl_rb, ts_s_lo, ts_ns, ts_lo, ts_hi, ts_tag, ts_val);
		dev_info(&pdev->dev,
			 "PTP CDC locked: p0_tx=%d p0_rx=%d p1_tx=%d p1_rx=%d (raw bits [11:8]=0x%x)\n",
			 (ctrl_rb >> 8) & 1, (ctrl_rb >> 9) & 1,
			 (ctrl_rb >> 10) & 1, (ctrl_rb >> 11) & 1,
			 (ctrl_rb >> 8) & 0xF);
	}

	/* Fill in the PTP clock info structure */
	memset(&priv->ptp_info, 0, sizeof(priv->ptp_info));
	snprintf(priv->ptp_info.name, sizeof(priv->ptp_info.name),
		 "onic%ds%df%d",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
	priv->ptp_info.owner = THIS_MODULE;
	priv->ptp_info.max_adj = 5000000; /* 5000 ppm — headroom for PLL rate error without servo oscillation */
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

	/* Initialize TX PTP timestamp tag management */
	spin_lock_init(&priv->ptp_tx_lock);
	memset(priv->ptp_tx_pending, 0, sizeof(priv->ptp_tx_pending));
	priv->ptp_next_tag = 1;
	INIT_DELAYED_WORK(&priv->ptp_tx_work, onic_ptp_tx_ts_work);

	dev_info(&pdev->dev, "PTP clock registered as /dev/ptp%d\n",
		 ptp_clock_index(priv->ptp_clock));

	return 0;
}

void onic_ptp_cleanup(struct onic_private *priv)
{
	unsigned long flags;
	int i;

	cancel_delayed_work_sync(&priv->ptp_tx_work);

	/* Free any remaining pending TX timestamp skb clones */
	spin_lock_irqsave(&priv->ptp_tx_lock, flags);
	for (i = 0; i < ONIC_PTP_TX_PENDING_MAX; i++) {
		if (priv->ptp_tx_pending[i].active) {
			kfree_skb(priv->ptp_tx_pending[i].skb);
			priv->ptp_tx_pending[i].skb = NULL;
			priv->ptp_tx_pending[i].active = false;
		}
	}
	spin_unlock_irqrestore(&priv->ptp_tx_lock, flags);

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

	if (config.tx_type == HWTSTAMP_TX_ON) {
		struct onic_hardware *hw = &priv->hw;

		/* Reset clock period to nominal so stale adjfine values
		 * from a previous ptp4l session don't corrupt the rate. */
		onic_write_reg(hw, ONIC_PTP_PERIOD_NS,
			       ONIC_PTP_NOMINAL_PERIOD_NS);
		onic_write_reg(hw, ONIC_PTP_PERIOD_FNS,
			       ONIC_PTP_NOMINAL_PERIOD_FNS);
		onic_write_reg(hw, ONIC_PTP_PERIOD_VALID, 1);
		/* Workqueue starts on demand from onic_ptp_alloc_tx_tag() */
	} else {
		cancel_delayed_work(&priv->ptp_tx_work);
	}

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
