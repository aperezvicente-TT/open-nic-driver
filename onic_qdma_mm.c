// SPDX-License-Identifier: GPL-2.0
/*
 * onic_qdma_mm.c - Minimal MM-mode QDMA primitives, hand-rolled from libqdma.
 *
 * Reference: AMD's dma_ip_drivers / libqdma at
 *   /home/alex/fpga-wksp/dma_ip_drivers/QDMA/linux-kernel/driver/libqdma/
 * Specifically eqdma_soft_access/eqdma_soft_access.c (functions
 * eqdma_indirect_reg_write, eqdma_h2c_context_write, etc.)
 *
 * Scope:
 *   - Indirect-register context write (for SW/HW/CMPT context programming)
 *   - H2C SW-context bit packing with is_mm=1
 *
 * Out of scope (handled in onic_sysdma.c or future commits):
 *   - Allocating descriptor/CMPT rings (onic_sysdma.c)
 *   - Doorbell PIDX writes
 *   - Completion polling
 *   - C2H side
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "onic_qdma_mm.h"
#include "qdma_access/qdma_register.h"

/* ------------------------------------------------------------------------ *
 *  Indirect register write — the QDMA mechanism for programming any of
 *  the per-queue context structures (SW / HW / CMPT / etc.).
 *
 *  Sequence:
 *    1. Write up to 8 data words to QDMA_OFFSET_IND_CTXT_DATA (0x804..0x820).
 *    2. Write up to 8 mask words to QDMA_OFFSET_IND_CTXT_MASK (0x824..0x840).
 *       Mask = 0xFFFFFFFF means "write all bits"; the libqdma reference
 *       always does this for full WR.  Partial writes use selective masks.
 *    3. Write the command word to QDMA_OFFSET_IND_CTXT_CMD (0x844):
 *         [bit0]    BUSY (set by HW; we read to poll)
 *         [bits4:1] SEL  (which context: SW_H2C, HW_H2C, CMPT, etc.)
 *         [bits6:5] OP   (WR / RD / CLR / INV)
 *         [bits18:7] QID
 *    4. Poll BUSY until clear (HW indicates command complete).
 *
 *  Mirrors eqdma_indirect_reg_write at libqdma eqdma_soft_access.c:2165.
 * ------------------------------------------------------------------------ */

int onic_qdma_indirect_reg_write(void __iomem *csr_base,
				 enum onic_qdma_ctxt_sel sel,
				 u16 qid, const u32 *data, u16 cnt)
{
	u32 cmd, busy;
	u16 i;
	int rv;

	if (!csr_base || !data) {
		return -EINVAL;
	}
	if (cnt == 0 || cnt > ONIC_QDMA_IND_CTXT_NUM_REGS) {
		return -EINVAL;
	}

	/* Step 1: data words (zero-pad unused). */
	for (i = 0; i < ONIC_QDMA_IND_CTXT_NUM_REGS; i++) {
		u32 v = (i < cnt) ? data[i] : 0;
		writel(v, csr_base + QDMA_OFFSET_IND_CTXT_DATA + (i * 4));
	}

	/* Step 2: mask words = all-ones (write all bits). */
	for (i = 0; i < ONIC_QDMA_IND_CTXT_NUM_REGS; i++) {
		writel(0xFFFFFFFFu, csr_base + QDMA_OFFSET_IND_CTXT_MASK + (i * 4));
	}

	/* Step 3: command. */
	cmd = ((u32)qid << ONIC_QDMA_IND_CTXT_CMD_QID_SHIFT) |
	      ((u32)ONIC_QDMA_CTXT_CMD_WR << ONIC_QDMA_IND_CTXT_CMD_OP_SHIFT) |
	      ((u32)sel << ONIC_QDMA_IND_CTXT_CMD_SEL_SHIFT);
	writel(cmd, csr_base + QDMA_OFFSET_IND_CTXT_CMD);

	/* Step 4: poll BUSY (defaults: 10us interval, 500ms cap, matching
	 * libqdma's QDMA_REG_POLL_DFLT_*).  readl_poll_timeout returns 0
	 * on success, -ETIMEDOUT on timeout. */
	rv = readl_poll_timeout(csr_base + QDMA_OFFSET_IND_CTXT_CMD,
				busy,
				(busy & ONIC_QDMA_IND_CTXT_CMD_BUSY_BIT) == 0,
				10, 500 * 1000);
	if (rv) {
		return -ETIMEDOUT;
	}

	return 0;
}

/* ------------------------------------------------------------------------ *
 *  Pack an H2C SW context into 8 u32 words.
 *
 *  Bit layout from libqdma eqdma_soft_reg.h SW_IND_CTXT_DATA_W*:
 *    W0  [28:17] FNC_ID    [15:0]  PIDX
 *    W1  [31]    IS_MM     [30]    MRKR_DIS    [19] MM_CHN
 *        [15:12] RNG_SZ    [8:5]   FETCH_MAX
 *        [3]     WBI_INTVL_EN [2] WBI_CHK [1] FCRD_EN [0] QEN
 *    W2  [31:0]  DSC_BASE_L
 *    W3  [31:0]  DSC_BASE_H
 *    W4  [13]    VIRTIO_EN [10:0] VEC
 *    W5  [10]    PASID_EN  [9:0] PASID_H  [31:11] VIRTIO_DSC_BASE_L
 *    W6  [31:0]  VIRTIO_DSC_BASE_M
 *    W7  [10:0]  VIRTIO_DSC_BASE_H
 *
 *  We zero W5-W7 (no PASID, no virtio).  W4 only fills VEC.
 * ------------------------------------------------------------------------ */

void onic_qdma_pack_h2c_sw_ctxt(const struct onic_qdma_h2c_sw_ctxt *ctxt,
				u32 data[ONIC_QDMA_IND_CTXT_NUM_REGS])
{
	memset(data, 0, ONIC_QDMA_IND_CTXT_NUM_REGS * sizeof(u32));

	data[0] = ((u32)(ctxt->fnc_id & 0xFFFu) << 17) |
		  ((u32)(ctxt->pidx   & 0xFFFFu));

	data[1] = (ctxt->is_mm        ? BIT(31) : 0) |
		  (ctxt->mrkr_dis     ? BIT(30) : 0) |
		  (ctxt->mm_chn       ? BIT(19) : 0) |
		  ((u32)(ctxt->rng_sz_idx & 0xFu)  << 12) |
		  ((u32)(ctxt->fetch_max  & 0xFu)  <<  5) |
		  (ctxt->wbi_intvl_en ? BIT(3)  : 0) |
		  (ctxt->wbi_chk      ? BIT(2)  : 0) |
		  (ctxt->fcrd_en      ? BIT(1)  : 0) |
		  (ctxt->qen          ? BIT(0)  : 0);

	data[2] = (u32)(ctxt->dsc_base & 0xFFFFFFFFu);
	data[3] = (u32)((ctxt->dsc_base >> 32) & 0xFFFFFFFFu);

	data[4] = ((u32)ctxt->vec & 0x7FFu);

	/* W5..W7 = 0 (no PASID, no virtio) */
}
