/* SPDX-License-Identifier: GPL-2.0 */
/*
 * onic_ddr_alloc.h — B5 minimum viable DDR4 allocator.
 *
 * Queue-slot bitmap (16 KiB per QP slot, 256 slots) + a single-class MR
 * pool (64 KiB × 256 MRs).  The full multi-class design is in
 * agent/reconic_integration/f4_ddr4_allocator_design.md; this is the
 * B5 subset that's just enough to land real create_qp / reg_user_mr.
 */
#ifndef __ONIC_DDR_ALLOC_H__
#define __ONIC_DDR_ALLOC_H__

#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* F4 §2.1 — DDR4 appears to ERNIC at this 64-bit base.  Byte offset
 * inside DDR4 is OR'd into the LSB. */
#define ONIC_DDR4_MSB                   0xa3500000u

/* F4 §2.5 — queue tier */
#define ONIC_DDR_QUEUE_TIER_OFF         0x00040000u
#define ONIC_DDR_QUEUE_SLOT_SIZE        0x00004000u   /* 16 KiB */
#define ONIC_DDR_QUEUE_SQ_OFF           0x00000000u
#define ONIC_DDR_QUEUE_RQ_OFF           0x00001000u
#define ONIC_DDR_QUEUE_CQ_OFF           0x00002000u

/* B5 MR pool (single size class). */
#define ONIC_DDR_MR_TIER_OFF            0x00400000u
#define ONIC_DDR_MR_SMALL_SIZE          0x00010000u   /* 64 KiB */
#define ONIC_DDR_MR_SMALL_COUNT         256u

/* Per-ERNIC region size — F4 §2.2 8 GiB each until lifted. */
#define ONIC_DDR_ERNIC_REGION_SIZE      0x200000000ULL

/* QP 0 + QP 1 are reserved (PG332 §7; F1 §5.4). */
#define ONIC_QP_RESERVED_LO             2u
#define ONIC_QP_MAX                     256u

struct onic_ddr_pool {
	/* base_off: 0 for ERNIC0, 0x2_0000_0000 for ERNIC1 (F4 §2.2). */
	u64                 base_off;

	/* Queue slots. Bit i == in-use; bits 0,1 preset to reserved. */
	DECLARE_BITMAP(qp_bits, ONIC_QP_MAX);
	spinlock_t          qp_lock;

	/* MR small-class free bitmap. */
	DECLARE_BITMAP(mr_small_bits, ONIC_DDR_MR_SMALL_COUNT);
	spinlock_t          mr_lock;
};

int  onic_ddr_pool_init(struct onic_ddr_pool *p, unsigned int ernic_id);
void onic_ddr_pool_fini(struct onic_ddr_pool *p);

int  onic_ddr_qp_slot_alloc(struct onic_ddr_pool *p,
			    u32 *out_qp_idx, u64 *out_slot_off);
void onic_ddr_qp_slot_free(struct onic_ddr_pool *p, u32 qp_idx);

int  onic_ddr_mr_alloc(struct onic_ddr_pool *p, u64 size,
		       u64 *out_off, u64 *out_len);
void onic_ddr_mr_free(struct onic_ddr_pool *p, u64 off);

/* Compose 64b DDR4 address from a byte offset. */
static inline u32 onic_ddr_addr_lsb(const struct onic_ddr_pool *p, u64 off)
{
	return (u32)((p->base_off + off) & 0xFFFFFFFFu);
}
static inline u32 onic_ddr_addr_msb(const struct onic_ddr_pool *p, u64 off)
{
	/* For ERNIC1 with base_off=0x2_0000_0000, the high 32b of the DDR4
	 * addr is 0xa350_0002, not 0xa350_0000. */
	u64 a = ((u64)ONIC_DDR4_MSB << 32) | (p->base_off + off);
	return (u32)(a >> 32);
}

#endif /* __ONIC_DDR_ALLOC_H__ */
