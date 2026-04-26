/* SPDX-License-Identifier: GPL-2.0 */
/*
 * onic_qdma_mm.h - Minimal MM-mode QDMA context + descriptor primitives
 *
 * Hand-rolled subset of AMD's libqdma, scoped to what onic_sysdma needs:
 * indirect-register context programming, MM SW context layout, and
 * descriptor packing.  Models after libqdma's eqdma_soft_access.c
 * (Vivado QDMA v5.0 = "Enhanced QDMA Soft" — our IP type).
 *
 * Reference points in libqdma:
 *   eqdma_soft_access.c:eqdma_indirect_reg_write   (lines ~2165-2208)
 *   eqdma_soft_reg.h:SW_IND_CTXT_DATA_W*           (lines 1357-1395)
 *   qdma_access_common.h:enum ind_ctxt_cmd_sel     (lines 113-127)
 */

#ifndef __ONIC_QDMA_MM_H__
#define __ONIC_QDMA_MM_H__

#include <linux/types.h>

/* ------------------------------------------------------------------------ *
 *  Indirect-register context-programming command layout
 *  (mirror of libqdma's enum ind_ctxt_cmd_op / ind_ctxt_cmd_sel)
 * ------------------------------------------------------------------------ */

enum onic_qdma_ctxt_op {
	ONIC_QDMA_CTXT_CMD_CLR = 0,   /* clear */
	ONIC_QDMA_CTXT_CMD_WR  = 1,   /* write data + mask */
	ONIC_QDMA_CTXT_CMD_RD  = 2,   /* read into data */
	ONIC_QDMA_CTXT_CMD_INV = 3,   /* invalidate */
};

enum onic_qdma_ctxt_sel {
	ONIC_QDMA_CTXT_SEL_SW_C2H   = 0,
	ONIC_QDMA_CTXT_SEL_SW_H2C   = 1,
	ONIC_QDMA_CTXT_SEL_HW_C2H   = 2,
	ONIC_QDMA_CTXT_SEL_HW_H2C   = 3,
	ONIC_QDMA_CTXT_SEL_CR_C2H   = 4,
	ONIC_QDMA_CTXT_SEL_CR_H2C   = 5,
	ONIC_QDMA_CTXT_SEL_CMPT     = 6,
	ONIC_QDMA_CTXT_SEL_FMAP     = 12,
};

/* QDMA indirect context layout: 8 data words + 8 mask words + 1 cmd word. */
#define ONIC_QDMA_IND_CTXT_NUM_REGS   8

/* Command-register fields (from libqdma's struct qdma_ind_ctxt_cmd_bits). */
#define ONIC_QDMA_IND_CTXT_CMD_BUSY_BIT     BIT(0)
#define ONIC_QDMA_IND_CTXT_CMD_QID_SHIFT    7
#define ONIC_QDMA_IND_CTXT_CMD_OP_SHIFT     5
#define ONIC_QDMA_IND_CTXT_CMD_SEL_SHIFT    1

/* ------------------------------------------------------------------------ *
 *  H2C SW-context bit layout (from eqdma_soft_reg.h SW_IND_CTXT_DATA_W*).
 *  We use W0..W4 for our minimal MM use case; W5..W7 are zero
 *  (no PASID, no virtio).
 *
 *  W0:  [28:17] FNC_ID (function id)
 *       [15:0]  PIDX (initial — usually 0)
 *
 *  W1:  [31]    IS_MM        ← THE bit that selects MM vs ST
 *       [30]    MRKR_DIS
 *       [19]    MM_CHN
 *       [15:12] RNG_SZ_IDX   (log2 ring size index, see qdma_dev_global_csr)
 *       [8:5]   FETCH_MAX
 *       [3]     WBI_INTVL_EN
 *       [2]     WBI_CHK
 *       [1]     FCRD_EN      (flow control / credit; we set 0)
 *       [0]     QEN          (queue enable)
 *
 *  W2:  [31:0]  DSC_BASE_L   (descriptor ring physical addr, low 32b)
 *  W3:  [31:0]  DSC_BASE_H   (descriptor ring physical addr, high 32b)
 *  W4:  [10:0]  VEC          (interrupt vector — unused in poll mode)
 *       [13]    VIRTIO_EN    (0)
 * ------------------------------------------------------------------------ */

struct onic_qdma_h2c_sw_ctxt {
	u16  fnc_id;
	u16  pidx;
	bool is_mm;        /* true for MM, false for ST */
	bool mrkr_dis;
	bool wbi_chk;
	bool wbi_intvl_en;
	bool fcrd_en;
	bool qen;
	u8   rng_sz_idx;   /* 4-bit index into ring-size table */
	u8   fetch_max;    /* 4-bit */
	u64  dsc_base;     /* descriptor ring physical addr */
	u16  vec;          /* MSI-X vector (poll mode = ignored) */
	bool mm_chn;
};

/* ------------------------------------------------------------------------ *
 *  H2C HW context layout (from eqdma_soft_reg.h HW_IND_CTXT_DATA_W*):
 *
 *  W0:  [31:16] CRD_USE   (credit usage — 0 for our use)
 *       [15:0]  CIDX      (consumer index — initial 0)
 *  W1:  [14:11] FETCH_PND (fetch pending — 0 initially)
 *       [10]    EVT_PND   (event pending — 0)
 *       [8]     DSC_PND   (descriptor pending — 0)
 *
 *  Nearly always programmed with all-zeros — HW updates these as DMA
 *  proceeds.  We just need to make sure the context exists so HW can
 *  update it.
 * ------------------------------------------------------------------------ */

struct onic_qdma_h2c_hw_ctxt {
	u16 cidx;
	u16 crd_use;     /* credit-use counter; 0 in fcrd_en=0 mode */
	u8  fetch_pnd;   /* 4-bit */
	bool evt_pnd;
	bool dsc_pnd;
};

/* ------------------------------------------------------------------------ *
 *  Completion mechanism for MM mode: writeback-status (WBI), not CMPT ring.
 *
 *  When wbi_chk=1 in the SW context, QDMA writes a 8-byte status word
 *  past the end of the descriptor ring (slot index = ring_size, just
 *  after the last real descriptor) every time it completes a batch.
 *  Layout:
 *      [15:0]  pidx (advancing as descriptors are submitted)
 *      [31:16] cidx (advancing as descriptors complete)
 *      [bits beyond] error / overrun flags
 *
 *  We poll cidx >= our_pidx to detect completion.  No CMPT context
 *  programming needed for MM mode — that's only used for ST C2H.
 *
 *  See libqdma qdma_descq.c:descq_mm_n_h2c_cmpl_status for the reference
 *  read pattern.
 * ------------------------------------------------------------------------ */

struct onic_qdma_wb_status {
	__le16 pidx;
	__le16 cidx;
	__le32 reserved;
} __packed;


/* ------------------------------------------------------------------------ *
 *  MM descriptor (32 bytes per PG302 v5.1 §5.1).
 * ------------------------------------------------------------------------ */

struct onic_qdma_mm_desc {
	__le64 src;            /* host PCIe addr (H2C) or card AXI addr (C2H) */
	__le64 dst;            /* card AXI addr (H2C) or host PCIe addr (C2H) */
	__le32 len_flags;      /* [27:0] length, [28] sop, [29] eop */
	__le32 reserved[3];
} __packed;

#define ONIC_QDMA_MM_FLAG_SOP   BIT(28)
#define ONIC_QDMA_MM_FLAG_EOP   BIT(29)
#define ONIC_QDMA_MM_LEN_MASK   GENMASK(27, 0)

/* ------------------------------------------------------------------------ *
 *  Public API — caller supplies the BAR2 base (priv->hw.addr) and qid.
 *  All functions assume the caller holds whatever lock guards concurrent
 *  context programming.  In onic_sysdma we use priv->sysdma->mutex.
 * ------------------------------------------------------------------------ */

/**
 * onic_qdma_indirect_reg_write - Program a context register via the
 *   indirect-register interface.  Mirrors libqdma's eqdma_indirect_reg_write.
 *
 * @csr_base:  ioremap'd BAR2 base (where QDMA CSRs live).  In our shell
 *             this is system_config crossbar slot M15 at BAR2+0x14000;
 *             pass the already-offset pointer (priv->hw.qdma_csr_base
 *             once we add that field).
 * @sel:       which context to write (ONIC_QDMA_CTXT_SEL_SW_H2C etc.)
 * @qid:      hardware queue id
 * @data:      array of up to 8 u32 words (zero-padded internally)
 * @cnt:       number of valid words in @data (1..8)
 *
 * Returns 0 on success, -ETIMEDOUT if BUSY bit doesn't clear, -EINVAL on
 * bad args.
 */
int onic_qdma_indirect_reg_write(void __iomem *csr_base,
				 enum onic_qdma_ctxt_sel sel,
				 u16 qid, const u32 *data, u16 cnt);

/**
 * onic_qdma_pack_h2c_sw_ctxt - Pack an H2C SW context struct into the
 *   8-word data array suitable for onic_qdma_indirect_reg_write.
 */
void onic_qdma_pack_h2c_sw_ctxt(const struct onic_qdma_h2c_sw_ctxt *ctxt,
				u32 data[ONIC_QDMA_IND_CTXT_NUM_REGS]);

/**
 * onic_qdma_pack_h2c_hw_ctxt - Pack an H2C HW context struct.
 *   Almost always called with all-zero ctxt (HW initialises itself);
 *   the call exists so the context slot exists post-clear.
 */
void onic_qdma_pack_h2c_hw_ctxt(const struct onic_qdma_h2c_hw_ctxt *ctxt,
				u32 data[ONIC_QDMA_IND_CTXT_NUM_REGS]);

/**
 * onic_qdma_clear_ctxt - Clear (invalidate) a context slot.
 *   Equivalent to indirect-write with op=CLR.
 */
int onic_qdma_clear_ctxt(void __iomem *csr_base,
			 enum onic_qdma_ctxt_sel sel, u16 qid);

/**
 * onic_qdma_pack_mm_desc - Pack src/dst/len into a 32-byte MM descriptor.
 */
static inline void onic_qdma_pack_mm_desc(struct onic_qdma_mm_desc *d,
					  u64 src, u64 dst, u32 len)
{
	memset(d, 0, sizeof(*d));
	d->src       = cpu_to_le64(src);
	d->dst       = cpu_to_le64(dst);
	d->len_flags = cpu_to_le32((len & ONIC_QDMA_MM_LEN_MASK) |
				   ONIC_QDMA_MM_FLAG_SOP |
				   ONIC_QDMA_MM_FLAG_EOP);
}

#endif /* __ONIC_QDMA_MM_H__ */
