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
#include <linux/delay.h>
#include <linux/pci.h>

#include "onic_hardware.h"
#include "onic_register.h"
#include "onic.h"
#include "qdma_register.h"
#include "qdma_context.h"
#include "qdma_error_info.h"
#include "libqdma/libqdma_export.h"

#define RX_ALIGN_TIMEOUT_MS			1000
#define CMAC_RESET_WAIT_MS			1
#define SHELL_RST_TIMEOUT_MS			200

/*
 * Per-index pool tables MUST agree with what libqdma programs into the QDMA
 * core's global CSR pool registers (GLBL_RNG_SZ/C2H_BUF_SZ/C2H_TIMER_CNT/
 * C2H_CNT_TH).  When ST netdev sets sw_ctxt.rngsz_idx / pfch_ctxt.bufsz_idx
 * etc., the hardware reads its pool table at that index and the driver
 * allocates a DMA ring of `onic_ring_count(idx)` entries — both sides MUST
 * see the same value.
 *
 * libqdma's eqdma_set_default_global_csr() (called from qdma_device_open
 * before onic_init_hardware runs) programs:
 *   rng_sz  = {2049, 65, 129, 193, 257, 385, 513, 769,
 *              1025, 1537, 3073, 4097, 6145, 8193, 12289, 16385}
 *   buf_sz  = {4096, 256, 512, 1024, 2048, 3968, 4096, 4096,
 *              4096, 4096, 4096, 4096, 4096, 8192, 9018, 16384}
 *   tmr_cnt = {1, 2, 4, 5, 8, 10, 15, 20, 25,
 *              30, 50, 75, 100, 125, 150, 200}
 *   cnt_th  = {2, 4, 8, 16, 24, 32, 48, 64,
 *              80, 96, 112, 128, 144, 160, 176, 192}
 *
 * Previously this driver had its own onic_qdma_init_csr() that re-programmed
 * the same pool registers with QDMA4-style values AND clobbered three EQDMA5
 * perf_opt registers (0x250, 0xB08, 0xE24).  That caused ST-mode TX/RX to
 * silently drop on the QDMA<->CMAC datapath.  The CSR init was deleted; the
 * tables here are the QDMA5 values libqdma programs, so the queue-context
 * indices the ST datapath uses now address the correct hardware sizes.
 */
static const u16 rngcnt_pool[QDMA_NUM_DESC_RNGCNT] = {
	2049, 65, 129, 193, 257, 385, 513, 769,
	1025, 1537, 3073, 4097, 6145, 8193, 12289, 16385
};

static const u16 c2h_bufsz_pool[QDMA_NUM_C2H_BUFSZ] = {
	4096, 256, 512, 1024, 2048, 3968, 4096, 4096,
	4096, 4096, 4096, 4096, 4096, 8192, 9018, 16384
};

static const u16 c2h_timer_pool[QDMA_NUM_C2H_TIMERS] = {
	1, 2, 4, 5, 8, 10, 15, 20, 25,
	30, 50, 75, 100, 125, 150, 200
};

static const u16 c2h_thres_pool[QDMA_NUM_C2H_COUNTERS] = {
	2, 4, 8, 16, 24, 32, 48, 64,
	80, 96, 112, 128, 144, 160, 176, 192
};

u16 onic_ring_count(u8 idx)
{
	return (idx < QDMA_NUM_DESC_RNGCNT) ? rngcnt_pool[idx] : 0;
}

/**
 * onic_reset_cmac_shell - shell-level CMAC reset WITHOUT enabling RX
 *
 * Used at probe time to put CMAC in a clean default state (RX disabled)
 * before the netdev is opened and queue contexts are programmed.  Without
 * this split, onic_enable_cmac(reset=true) at probe would leave RX enabled
 * with no queue consumer — packets arriving on the wire before the first
 * `ip link set ... up` would hit qids with no sw_ctxt, triggering QDMA
 * CMPT_INV_Q_ERR / pipeline stalls.  The open path calls onic_enable_cmac
 * with reset=false to turn RX on once queues are ready.
 */
int onic_reset_cmac_shell(struct onic_hardware *hw, u8 cmac_id)
{
	u32 mask;
	int i;

	if (cmac_id != 0 && cmac_id != 1)
		return -EINVAL;

	mask = (cmac_id == 0) ? 0x10 : 0x100;
	onic_write_reg(hw, SYSCFG_OFFSET_SHELL_RESET, mask);
	for (i = 0; i < SHELL_RST_TIMEOUT_MS; i++) {
		if ((onic_read_reg(hw, SYSCFG_OFFSET_SHELL_STATUS) & mask) == mask)
			break;
		mdelay(CMAC_RESET_WAIT_MS);
	}
	if (i == SHELL_RST_TIMEOUT_MS)
		pr_warn("onic: CMAC%d shell reset timed out, continuing\n", cmac_id);
	return 0;
}

int onic_enable_cmac(struct onic_hardware *hw, u8 cmac_id, bool reset)
{
	if (cmac_id != 0 && cmac_id != 1)
		return -EINVAL;

	if (reset)
		onic_reset_cmac_shell(hw, cmac_id);

    if (hw->RS_FEC) {
		/* Enable RS-FEC for CMACs with RS-FEC implemented */
		onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_id), 0x3);
		onic_write_reg(hw, CMAC_OFFSET_RSFEC_CONF_IND_CORRECTION(cmac_id), 0x7);
    }

	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_1(cmac_id), 0x1);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_1(cmac_id), 0x10);

	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_1(cmac_id), 0x1);

	/* RX flow control */
	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_FC_CTRL_1(cmac_id), 0x00003DFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_RX_FC_CTRL_2(cmac_id), 0x0001C631);

	/* TX flow control */
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_1(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_2(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_3(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_4(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_QNTA_5(cmac_id), 0x0000FFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_1(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_2(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_3(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_4(cmac_id), 0xFFFFFFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_RFRH_5(cmac_id), 0x0000FFFF);
	onic_write_reg(hw, CMAC_OFFSET_CONF_TX_FC_CTRL_1(cmac_id), 0x000001FF);

	return 0;
}

/**
 * onic_init_hardware_master - primary (MASTER_PF) hardware init
 *
 * Owns the BAR2 iomap and the one real qdma_dev.  Single-PF shell carves the
 * queue namespace across all CMACs via a single fmap[0] entry covering the
 * full range [0, num_cmacs * N).  Secondaries get child qdma_devs that share
 * this iomap and func_id with distinct q_base offsets.
 */
static int onic_init_hardware_master(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev;
	struct qdma_fmap_ctxt fmap_ctxt;
	void __iomem *bar0_regs;
	void __iomem *bar2_regs;
	u16 qmax, total_qmax;
	u32 val;
	int i, rv;

	/* Option 3: libqdma owns the BAR claims and ioremaps.  We must run
	 * after qdma_device_open so priv->qdma_dev_handle is valid, and we
	 * borrow both BAR pointers from libqdma instead of ioremaping them
	 * ourselves.  Anything else would re-introduce the BAR conflict. */
	if (!priv->qdma_dev_handle) {
		dev_err(&pdev->dev,
			"onic_init_hardware_master: qdma_dev_handle not set — qdma_device_open must run first");
		return -EINVAL;
	}

	bar2_regs = qdma_device_get_user_regs(priv->qdma_dev_handle);
	if (!bar2_regs) {
		dev_err(&pdev->dev,
			"onic_init_hardware_master: libqdma did not map user (BAR2) — design must expose it");
		return -EINVAL;
	}
	/* hw->addr is the shell-register window, which lives at SHELL_START
	 * inside BAR 2.  libqdma maps the whole BAR; we offset into it. */
	hw->addr = bar2_regs + SHELL_START;

	bar0_regs = qdma_device_get_config_regs(priv->qdma_dev_handle);
	if (!bar0_regs) {
		dev_err(&pdev->dev,
			"onic_init_hardware_master: libqdma did not map config (BAR0)");
		hw->addr = NULL;
		return -EINVAL;
	}

	/* QDMA IP registers use BAR-0 — borrow libqdma's mapping */
	qdev = qdma_create_dev(pdev, bar0_regs);
	if (!qdev) {
		rv = -ENOMEM;
		hw->addr = NULL;
		return rv;
	}
	hw->qdma = (unsigned long)qdev;

	/* Detect CMAC count before fmap write — total queue range depends on it */
	for (i = 0; i < ONIC_MAX_CMACS; ++i) {
		val = onic_read_reg(hw, CMAC_OFFSET_CORE_VERSION(i));
		if (val != ONIC_CMAC_CORE_VERSION)
			break;
	}
	hw->num_cmacs = i;
	dev_info(&pdev->dev, "Number of CMAC instances = %d", hw->num_cmacs);

	qmax = max(priv->num_tx_queues, priv->num_rx_queues);
	/* fmap must cover the ABSOLUTE qid namespace the shell plugin tags, not
	 * the per-CMAC dynamic queue count.  CMAC i places its queues at
	 * [i * ONIC_PER_CMAC_QUEUES, i * ONIC_PER_CMAC_QUEUES + N).  So fmap
	 * qmax = num_cmacs * ONIC_PER_CMAC_QUEUES covers all CMACs' ranges even
	 * when N is smaller than ONIC_PER_CMAC_QUEUES (MSI-X-constrained builds).
	 * Driver-shell contract: ONIC_PER_CMAC_QUEUES must equal the plugin's
	 * PER_CMAC_QUEUES constant (both 64). */
	total_qmax = hw->num_cmacs * ONIC_PER_CMAC_QUEUES;

	/* Single-PF: one fmap entry for the entire queue range shared across
	 * all CMACs.  Children use the same func_id with distinct q_base. */
	memset(&fmap_ctxt, 0, sizeof(struct qdma_fmap_ctxt));
	fmap_ctxt.qbase = 0;
	fmap_ctxt.qmax = total_qmax;
	rv = qdma_clear_fmap_ctxt(qdev);
	if (rv < 0)
		goto clear_hardware;
	rv = qdma_write_fmap_ctxt(qdev, &fmap_ctxt);
	if (rv < 0)
		goto clear_hardware;

	/* [SEC_DIAG] FMAP readback to confirm hardware accepted the values
	 * libqdma may program FMAP with qmax=qsets_max (e.g. 64) and we
	 * overwrite with total_qmax (e.g. 128).  If something else clobbers
	 * this back to 64, qids 64-127 will not be claimed by the function. */
	{
		union qdma_ctxt_cmd cmd_rd;
		u32 raw0 = 0, raw1 = 0, busy;
		int j;

		cmd_rd.word = 0;
		cmd_rd.bits.sel = QDMA_CTXT_CMD_SEL_FMAP;
		cmd_rd.bits.op = QDMA_CTXT_CMD_OP_RD;
		cmd_rd.bits.qid = qdev->func_id;

		qdma_write_reg(qdev, QDMA_OFFSET_IND_CTXT_CMD, cmd_rd.word);
		for (j = 0; j < 50000; ++j) {
			busy = qdma_read_reg(qdev, QDMA_OFFSET_IND_CTXT_CMD);
			if ((busy & QDMA_IND_CTXT_CMD_BUSY_MASK) == 0)
				break;
			udelay(10);
		}
		raw0 = qdma_read_reg(qdev, QDMA_OFFSET_IND_CTXT_DATA);
		raw1 = qdma_read_reg(qdev, QDMA_OFFSET_IND_CTXT_DATA + 4);
		pr_info("[SEC_DIAG] FMAP readback (func_id=%u): W0=0x%08x W1=0x%08x (qbase=%u qmax=%u) [wrote qbase=%u qmax=%u]\n",
			qdev->func_id, raw0, raw1,
			raw0 & 0x7FF, raw1 & 0xFFF,
			fmap_ctxt.qbase, fmap_ctxt.qmax);
	}

	/* Single shell function slot for all CMACs.  The RDMA plugin arbitrates
	 * CMAC0+CMAC1 non-RoCE streams into slot 0 and encodes CMAC identity
	 * into the queue ID (CMAC0 → [0, N), CMAC1 → [N, 2N)).  QCONF(0) must
	 * cover the full combined range so QDMA accepts writes across [0, 2N).
	 * Secondary does not touch shell-slot registers — see
	 * agent/reconic_integration/dual_netdev_plan.md. */
	val = FIELD_SET(QDMA_FUNC_QCONF_QBASE_MASK, 0) |
	      FIELD_SET(QDMA_FUNC_QCONF_NUMQ_MASK, total_qmax);
	onic_write_reg(hw, QDMA_FUNC_OFFSET_QCONF(0), val);

	/* RSS indirection table spans the full combined range.  Hash-selected
	 * queues within a CMAC's share stay in that CMAC's range because the
	 * plugin's direct qid tagging happens before QDMA indir lookup. */
	for (i = 0; i < 128; ++i) {
		u32 v = (i % total_qmax) & 0x0000FFFF;
		u32 offset = QDMA_FUNC_OFFSET_INDIR_TABLE(0, i);
		onic_write_reg(hw, offset, v);
	}

	/* QDMA core CSRs (ring/buffer/timer/counter pools, GLBL_DSC_CFG,
	 * C2H_PFCH_CFG, C2H_WB_COAL_CFG, H2C_REQ_THROT, plus EQDMA5
	 * perf_opt registers) are owned by libqdma — qdma_device_open
	 * already invoked eqdma_set_default_global_csr → eqdma_set_perf_opt
	 * before we got here.  Re-programming them from the legacy
	 * QDMA4-style table broke ST-mode TX/RX (TX dropped at QDMA→CMAC,
	 * RX dropped at CMAC→QDMA) while leaving MM-mode sysdma working;
	 * see INTEGRATION_NOTES.md "ST datapath regression". */

	/* Shell-reset this netdev's CMAC to known clean state WITHOUT enabling
	 * RX.  RX is enabled in onic_open_netdev once queue contexts exist. */
	onic_reset_cmac_shell(hw, priv->cmac_id);

	return 0;

clear_hardware:
	onic_clear_hardware(priv);
	return rv;
}

/**
 * onic_init_hardware_slave - secondary (non-master) hardware init
 *
 * Shares the primary's BAR2 iomap and wraps its qdma_dev in a child with an
 * offset q_base so this netdev's queues land in [q_base, q_base + N) of the
 * shared QDMA queue namespace.  Does not touch fmap/CSR/QCONF (primary owns).
 */
static int onic_init_hardware_slave(struct onic_private *priv)
{
	struct onic_private *primary = priv->peer;
	struct onic_hardware *hw = &priv->hw;
	struct qdma_dev *parent_qdev, *child_qdev;

	if (!primary || !primary->hw.addr || !primary->hw.qdma) {
		pr_err("onic: slave priv missing primary hw context\n");
		return -EINVAL;
	}

	/* Share BAR2 iomap and topology info with primary */
	hw->addr = primary->hw.addr;
	hw->num_cmacs = primary->hw.num_cmacs;

	/* Child qdev routes relative qids to absolute via q_base.  The plugin
	 * arbiter in the shell tags CMAC1 packets with absolute qid∈[qid_base,
	 * qid_base + N), so secondary's per-queue contexts land at the right
	 * addresses without any shell-slot register writes here.  Primary owns
	 * the single QCONF(0) / INDIR_TABLE(0) pair covering the full range. */
	parent_qdev = (struct qdma_dev *)primary->hw.qdma;
	child_qdev = qdma_create_child_dev(parent_qdev, priv->qid_base);
	if (!child_qdev)
		return -ENOMEM;
	hw->qdma = (unsigned long)child_qdev;

	/* Shell-reset this netdev's CMAC; open path enables RX when ready. */
	onic_reset_cmac_shell(hw, priv->cmac_id);

	return 0;
}

int onic_init_hardware(struct onic_private *priv)
{
	priv->hw.RS_FEC = priv->RS_FEC;

	if (test_bit(ONIC_FLAG_MASTER_PF, priv->flags))
		return onic_init_hardware_master(priv);
	else
		return onic_init_hardware_slave(priv);
}

static int onic_shell_qdma_reset(struct onic_hardware *hw)
{
	u32 status;
	int i;

	/* assert QDMA subsystem reset (bit 0 of shell reset register) */
	onic_write_reg(hw, SYSCFG_OFFSET_SHELL_RESET, BIT(0));

	/* poll shell status bit 0 for reset-done, ~1ms per iteration */
	for (i = 0; i < SHELL_RST_TIMEOUT_MS; i++) {
		status = onic_read_reg(hw, SYSCFG_OFFSET_SHELL_STATUS);
		if (status & BIT(0))
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

static int __maybe_unused onic_shell_system_reset(struct onic_hardware *hw)
{
	int i;

	onic_write_reg(hw, SYSCFG_OFFSET_SYSTEM_RESET, 1);

	for (i = 0; i < SHELL_RST_TIMEOUT_MS; i++) {
		if (onic_read_reg(hw, SYSCFG_OFFSET_SYSTEM_STATUS) & BIT(0))
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

void onic_clear_hardware(struct onic_private *priv)
{
	struct onic_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev = (struct qdma_dev *)hw->qdma;
	u8 master_pf = test_bit(ONIC_FLAG_MASTER_PF, priv->flags);
	int rv;

	if (!qdev) {
		memset(hw, 0, sizeof(struct onic_hardware));
		return;
	}

	if (master_pf) {
		/* Runs after all secondaries have torn down.  Reset the QDMA
		 * DMA engine and clear the single fmap entry.  BAR0 / BAR2
		 * mappings are owned by libqdma (Option 3) — qdma_destroy_dev
		 * frees only the legacy wrapper, qdma_device_close in the
		 * caller will iounmap. */
		rv = onic_shell_qdma_reset(hw);
		if (rv)
			dev_warn(&pdev->dev,
				 "QDMA shell reset timed out, continuing teardown\n");

		onic_write_reg(hw, QDMA_FUNC_OFFSET_QCONF(0), 0);
		qdma_invalidate_fmap_ctxt(qdev);
		qdma_destroy_dev(qdev); /* borrowed BAR0 — no iounmap here */
	} else {
		/* Slave: free the child qdma_dev wrapper only.  Parent iomap
		 * is still owned by primary, and the shell-slot registers
		 * were never touched by the slave (primary owns QCONF(0)). */
		qdma_destroy_dev(qdev);
	}

	memset(hw, 0, sizeof(struct onic_hardware));
}

void onic_qdma_init_error_interrupt(unsigned long qdma, u16 vid)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	int i;

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 0));
	qdma_write_reg(qdev, offset, val);

	for (i = 0; i < NUM_LEAF_ERROR_AGGREGATORS; i++) {
		u32 err_idx = leaf_error_aggregators[i];

		offset = qdma_error_info[err_idx].mask_reg_addr;
		val = qdma_error_info[err_idx].leaf_err_mask;
		qdma_write_reg(qdev, offset, val);

		offset = QDMA_OFFSET_GLBL_ERR_MASK;
		val = qdma_read_reg(qdev, offset);
		val |= FIELD_SET(qdma_error_info[err_idx].glbl_err_mask, 1);
		qdma_write_reg(qdev, offset, val);
	}

	/* W1C-clear all leaf error status registers before re-arming.
	 * Without this, any latched error bit (e.g. C2H MTY/QID mismatch
	 * caused by an in-flight packet when the cable was pulled) remains
	 * set and immediately re-fires the interrupt the moment ARM=1 is
	 * written, producing an unrecoverable interrupt storm. */
	for (i = 0; i < NUM_LEAF_ERROR_AGGREGATORS; i++) {
		u32 err_idx = leaf_error_aggregators[i];

		val = qdma_read_reg(qdev, qdma_error_info[err_idx].stat_reg_addr);
		if (val)
			qdma_write_reg(qdev, qdma_error_info[err_idx].stat_reg_addr, val);
	}
	val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT, val);

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 1));
	qdma_write_reg(qdev, offset, val);
}

void onic_qdma_clear_error_interrupt(unsigned long qdma)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT, 0);
}

int onic_qdma_init_tx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_h2c_param *param)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 1;
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.irq_arm = 0;
	sw_ctxt.irq_en = 0;
	sw_ctxt.desc_sz = 1; /* 1: 16B for H2C stream */
	sw_ctxt.fcrd_en = 0;
	sw_ctxt.wbi_chk = 1;
	sw_ctxt.wbi_intvl_en = 1;
	sw_ctxt.at = 0;
	sw_ctxt.rngsz_idx = param->rngcnt_idx;
	sw_ctxt.desc_base = param->dma_addr;
	sw_ctxt.vec = param->vid;
	sw_ctxt.intr_aggr = 0;
	/* Route H2C packets to the correct CMAC. The shell's qdma_subsystem
	 * arbiter uses the SW context port_id (W1 bits 24:22) to select
	 * CMAC0 vs CMAC1. q_base is 0 for primary (CMAC0) and 64 for the
	 * secondary child qdev (CMAC1) per ONIC_PER_CMAC_QUEUES. Without
	 * this, secondary's packets are routed to CMAC0 and silently dropped
	 * at the SerDes when CMAC0 has no link. */
	sw_ctxt.port_id = qdev->q_base / ONIC_PER_CMAC_QUEUES;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_tx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;

	return 0;

clear_tx_queue:
	onic_qdma_clear_tx_queue(qdma, qid);
	return rv;
}

int onic_qdma_init_rx_queue(unsigned long qdma, u16 qid,
			    const struct onic_qdma_c2h_param *param)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	struct qdma_pfch_ctxt pfch_ctxt;
	struct qdma_cmpl_ctxt cmpl_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context — written in two phases.
	 * Phase 1: configure ring addresses/sizes with qen=0 so the QDMA
	 * descriptor and credit engines are idle while we set up HW/CR/PFCH/CMPT
	 * contexts.  Writing qen=1 (phase 2, after CMPT is valid) prevents the
	 * QDMA from issuing a credit-update completion to an uninitialized CMPT
	 * ring, which would otherwise fire CMPT_INV_Q_ERR immediately. */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 0;  /* will be set to 1 after CMPT context is ready */
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.desc_sz = 0; /* 0: 8B for C2H stream */
	sw_ctxt.fcrd_en = 1;
	sw_ctxt.rngsz_idx = param->desc_rngcnt_idx;
	sw_ctxt.desc_base = param->desc_dma_addr;
	/* Route C2H descriptors per CMAC — same rationale as H2C above. */
	sw_ctxt.port_id = qdev->q_base / ONIC_PER_CMAC_QUEUES;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;

	/* Write completion context BEFORE prefetch context so that when
	 * pfch_en=1 activates the prefetch engine, CMPT_CTXT.valid is
	 * already set.  Writing PFCH first (even with qen=0) can still
	 * trigger CMPT_INV_Q_ERR if the prefetch engine wakes and finds
	 * a stale non-zero PIDX with no valid CMPT ring. */
	memset(&cmpl_ctxt, 0, sizeof(struct qdma_cmpl_ctxt));
	cmpl_ctxt.stat_en = 1;
	cmpl_ctxt.intr_en = 1;
	cmpl_ctxt.trig_mode = 0x5;
	cmpl_ctxt.func_id = qdev->func_id;
	cmpl_ctxt.counter_idx = 0;
	cmpl_ctxt.timer_idx = 0;
	cmpl_ctxt.color = 1;
	cmpl_ctxt.rngsz_idx = param->cmpl_rngcnt_idx;
	cmpl_ctxt.baddr = param->cmpl_dma_addr;
	cmpl_ctxt.desc_sz = param->cmpl_desc_sz;
	cmpl_ctxt.valid = 1;
	cmpl_ctxt.full_upd = 0;
	cmpl_ctxt.ovf_chk_dis = 0;
	cmpl_ctxt.vec = param->vid;
	cmpl_ctxt.intr_aggr = 0;

	rv = qdma_clear_cmpl_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_cmpl_ctxt(qdev, qid, &cmpl_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	/* Prefetch context — CMPT is now valid so pfch_en=1 is safe. */
	memset(&pfch_ctxt, 0, sizeof(struct qdma_pfch_ctxt));
	pfch_ctxt.bufsz_idx = param->bufsz_idx;
	pfch_ctxt.pfch_en = 1;
	pfch_ctxt.valid = 1;
	/* Same per-CMAC routing as the SW context port_id, in case the
	 * prefetch path also consults port_id for arbitration. */
	pfch_ctxt.port_id = qdev->q_base / ONIC_PER_CMAC_QUEUES;

	rv = qdma_clear_pfch_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_pfch_ctxt(qdev, qid, &pfch_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	/* Phase 2: all contexts valid — enable the queue. */
	sw_ctxt.qen = 1;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	return 0;

clear_rx_queue:
	onic_qdma_clear_rx_queue(qdma, qid);
	return rv;
}

void onic_qdma_clear_tx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
}

void onic_qdma_clear_rx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
	qdma_invalidate_pfch_ctxt(qdev, qid);
	qdma_invalidate_cmpl_ctxt(qdev, qid);
}

/**
 * onic_qdma_set_q_pidx - set QDMA queue producer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @dir: queue direction
 * @pidx: producer index
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void onic_qdma_set_q_pidx(unsigned long qdma, u16 qid,
				 enum qdma_dir dir, u16 pidx, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;

	onic_dev_dbg(ONIC_DBG_DATA, &qdev->pdev->dev,
		     "set_q_pidx qid:%u dir:%x pidx:%u irq_arm:%u",
		     qid, dir, pidx, irq_arm);

	if (qid < 0)
		return;

	/* DMAP registers are indexed by the ABSOLUTE queue ID (q_base + qid),
	 * not by the relative qid local to this qdma_dev. */
	if (dir == QDMA_C2H)
		offset = QDMA_OFFSET_DMAP_SEL_C2H_DESC_PIDX + ((qdev->q_base + qid) * 16);
	else
		offset = QDMA_OFFSET_DMAP_SEL_H2C_DESC_PIDX + ((qdev->q_base + qid) * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_DESC_PIDX_MASK, pidx) |
	       FIELD_SET(QDMA_DMAP_SEL_DESC_IRQ_ARM_MASK, irq_arm));

	pr_info("[SEC_DIAG] set_q_pidx: q_base=%u rel_qid=%u abs_qid=%u dir=%d offset=0x%x val=0x%x\n",
		qdev->q_base, qid, qdev->q_base + qid, dir, offset, val);

	qdma_write_reg(qdev, offset, val);
}

void onic_set_tx_head(unsigned long qdma, u16 qid, u16 head)
{
	onic_qdma_set_q_pidx(qdma, qid, QDMA_H2C, head, 0);
}

void onic_set_rx_head(unsigned long qdma, u16 qid, u16 head)
{
	onic_qdma_set_q_pidx(qdma, qid, QDMA_C2H, head, 0);
}

/**
 * onic_qdma_set_cmpl_cidx - set QDMA completion queue consumer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @cidx: consumer index
 * @counter_idx: index to C2H counter threshold registers
 * @timer_idx: index to C2H timer registers
 * @trig_mode: interrupt and status descriptor trigger mode
 * @stat_en: enable completion status writeback
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void onic_qdma_set_cmpl_cidx(unsigned long qdma, u16 qid, u16 cidx,
				       u8 counter_idx, u8 timer_idx,
				       u8 trig_mode, u8 stat_en, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;

	onic_dev_dbg(ONIC_DBG_DATA, &qdev->pdev->dev,
		     "set_cmpl_cidx qid:%u cidx:%u cnt_idx:%u tmr_idx:%u trig:%u irq_arm:%u",
		     qid, cidx, counter_idx, timer_idx, trig_mode, irq_arm);

	if (qid < 0)
		return;

	/* DMAP uses absolute queue ID. */
	offset = QDMA_OFFSET_DMAP_SEL_CMPL_CIDX + ((qdev->q_base + qid) * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_CMPL_CIDX_MASK, cidx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_COUNTER_IDX_MASK, counter_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TIMER_IDX_MASK, timer_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TRIG_MODE_MASK, trig_mode) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_STAT_EN_MASK, stat_en) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_IRQ_ARM_MASK, irq_arm));
	qdma_write_reg(qdev, offset, val);
}

void onic_qdma_dump_error_regs(unsigned long qdma)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 glbl, dsc, trq, c2h, c2h_fatal, h2c, sbe, dbe;

	glbl      = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT);
	dsc       = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_DSC_ERR_STAT);
	trq       = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_TRQ_ERR_STAT);
	c2h       = qdma_read_reg(qdev, QDMA_OFFSET_C2H_ERR_STAT);
	c2h_fatal = qdma_read_reg(qdev, QDMA_OFFSET_C2H_FATAL_ERR_STAT);
	h2c       = qdma_read_reg(qdev, QDMA_OFFSET_H2C_ERR_STAT);
	sbe       = qdma_read_reg(qdev, QDMA_OFFSET_RAM_SBE_STAT);
	dbe       = qdma_read_reg(qdev, QDMA_OFFSET_RAM_DBE_STAT);

	dev_err(&qdev->pdev->dev,
		"QDMA err: GLBL=0x%08x DSC=0x%08x TRQ=0x%08x "
		"C2H=0x%08x C2H_FATAL=0x%08x H2C=0x%08x "
		"SBE=0x%08x DBE=0x%08x C2H_FIRST_ERR_QID=0x%08x\n",
		glbl, dsc, trq, c2h, c2h_fatal, h2c, sbe, dbe,
		qdma_read_reg(qdev, QDMA_OFFSET_C2H_FIRST_ERR_QID));
}

void onic_set_completion_tail(unsigned long qdma, u16 qid, u16 tail, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u8 trig_mode = 5;
	u8 stat_en = 1;
	u32 offset;

	onic_dev_dbg(ONIC_DBG_DATA, &qdev->pdev->dev,
		     "set_completion_tail qid:%u tail:%u irq_arm:%u",
		     qid, tail, irq_arm);
	onic_qdma_set_cmpl_cidx(qdma, qid, tail, 0, 0, trig_mode, stat_en, irq_arm);

	if (irq_arm) {
		offset = QDMA_OFFSET_DMAP_SEL_CMPL_CIDX + ((qdev->q_base + qid) * 16);
		(void)qdma_read_reg(qdev, offset);
	}
}
