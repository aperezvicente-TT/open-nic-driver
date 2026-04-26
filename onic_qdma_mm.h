/* SPDX-License-Identifier: GPL-2.0 */
/*
 * onic_qdma_mm.h - QDMA AXI-MM mode bits NOT in qdma_access/.
 *
 * The existing qdma_access library (qdma_context.{c,h} et al.) already
 * provides:
 *   - struct qdma_sw_ctxt with is_mm field
 *   - qdma_write_sw_ctxt() / qdma_clear_sw_ctxt() / qdma_invalidate_sw_ctxt()
 *   - qdma_clear_hw_ctxt() / qdma_invalidate_hw_ctxt()
 *
 * These work for MM mode by setting ctxt.is_mm = 1 (the bit at W1[31]).
 * The existing netdev TX/RX path uses these with is_mm=0 for ST mode;
 * setting is_mm=1 and otherwise filling the same struct gives MM mode.
 *
 * What's MM-specific and NOT in qdma_access:
 *   - 32-byte MM descriptor format (qdma_h2c_st_desc is the ST variant)
 *   - Writeback-status word format used to signal MM completion
 *
 * Both are in this header.
 */

#ifndef __ONIC_QDMA_MM_H__
#define __ONIC_QDMA_MM_H__

#include <linux/types.h>

/* ------------------------------------------------------------------------ *
 *  MM descriptor — 32 bytes per PG302 v5.1 §5.1.
 *  H2C: src = host PCIe dma_addr_t, dst = card AXI offset, len = bytes.
 *  C2H: src = card AXI offset,      dst = host PCIe dma_addr_t, len = bytes.
 * ------------------------------------------------------------------------ */

struct onic_qdma_mm_desc {
	__le64 src;
	__le64 dst;
	__le32 len_flags;       /* [27:0] length, [28] sop, [29] eop */
	__le32 reserved[3];
} __packed;

#define ONIC_QDMA_MM_FLAG_SOP   BIT(28)
#define ONIC_QDMA_MM_FLAG_EOP   BIT(29)
#define ONIC_QDMA_MM_LEN_MASK   GENMASK(27, 0)

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

/* ------------------------------------------------------------------------ *
 *  Writeback-status word.
 *
 *  When wbi_chk=1 in the SW context, QDMA writes a u64 status word at
 *  the end of the descriptor ring (slot index = ring_size, just past
 *  the last real descriptor) every time it completes a batch.  Layout:
 *      [15:0]  pidx  (advances as descriptors are submitted)
 *      [31:16] cidx  (advances as descriptors complete)
 *      [bits beyond] error / overrun flags
 *
 *  We poll cidx >= our submitted_pidx for completion detection.  No
 *  CMPT context programming is needed in MM mode — that's only for
 *  ST C2H per qdma_descq.c:descq_mm_n_h2c_cmpl_status in libqdma.
 * ------------------------------------------------------------------------ */

struct onic_qdma_wb_status {
	__le16 pidx;
	__le16 cidx;
	__le32 reserved;
} __packed;

#endif /* __ONIC_QDMA_MM_H__ */
