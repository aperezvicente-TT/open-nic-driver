/* SPDX-License-Identifier: GPL-2.0 */
/*
 * onic_ddr_alloc.c — B5 minimum DDR4 slab allocator for QP queue slots
 * and MR staging regions.
 */
#include <linux/slab.h>
#include <linux/string.h>
#include "onic_ddr_alloc.h"

int onic_ddr_pool_init(struct onic_ddr_pool *p, unsigned int ernic_id)
{
	memset(p, 0, sizeof(*p));
	/* F4 §2.2: disjoint halves.  ERNIC0 -> 0, ERNIC1 -> 0x2_0000_0000. */
	p->base_off = (u64)ernic_id * 0x200000000ULL;

	spin_lock_init(&p->qp_lock);
	spin_lock_init(&p->mr_lock);

	/* Reserve QP0 + QP1 (PG332 §7, MAD). */
	bitmap_zero(p->qp_bits, ONIC_QP_MAX);
	set_bit(0, p->qp_bits);
	set_bit(1, p->qp_bits);

	bitmap_zero(p->mr_small_bits, ONIC_DDR_MR_SMALL_COUNT);
	return 0;
}

void onic_ddr_pool_fini(struct onic_ddr_pool *p)
{
	/* No kmalloc'd state in the B5 allocator; nothing to free. */
	(void)p;
}

int onic_ddr_qp_slot_alloc(struct onic_ddr_pool *p,
			   u32 *out_qp_idx, u64 *out_slot_off)
{
	unsigned long flags;
	int bit;

	spin_lock_irqsave(&p->qp_lock, flags);
	bit = find_first_zero_bit(p->qp_bits, ONIC_QP_MAX);
	if (bit >= ONIC_QP_MAX) {
		spin_unlock_irqrestore(&p->qp_lock, flags);
		return -ENOMEM;
	}
	set_bit(bit, p->qp_bits);
	spin_unlock_irqrestore(&p->qp_lock, flags);

	*out_qp_idx   = (u32)bit;
	*out_slot_off = ONIC_DDR_QUEUE_TIER_OFF +
			(u64)bit * ONIC_DDR_QUEUE_SLOT_SIZE;
	return 0;
}

void onic_ddr_qp_slot_free(struct onic_ddr_pool *p, u32 qp_idx)
{
	unsigned long flags;

	if (qp_idx < ONIC_QP_RESERVED_LO || qp_idx >= ONIC_QP_MAX)
		return;
	spin_lock_irqsave(&p->qp_lock, flags);
	clear_bit(qp_idx, p->qp_bits);
	spin_unlock_irqrestore(&p->qp_lock, flags);
}

int onic_ddr_mr_alloc(struct onic_ddr_pool *p, u64 size,
		      u64 *out_off, u64 *out_len)
{
	unsigned long flags;
	int bit;

	/* B5 limitation: single class.  Reject >64 KiB. */
	if (size == 0 || size > ONIC_DDR_MR_SMALL_SIZE)
		return -ENOMEM;

	spin_lock_irqsave(&p->mr_lock, flags);
	bit = find_first_zero_bit(p->mr_small_bits, ONIC_DDR_MR_SMALL_COUNT);
	if (bit >= ONIC_DDR_MR_SMALL_COUNT) {
		spin_unlock_irqrestore(&p->mr_lock, flags);
		return -ENOMEM;
	}
	set_bit(bit, p->mr_small_bits);
	spin_unlock_irqrestore(&p->mr_lock, flags);

	*out_off = ONIC_DDR_MR_TIER_OFF +
		   (u64)bit * ONIC_DDR_MR_SMALL_SIZE;
	*out_len = ONIC_DDR_MR_SMALL_SIZE;
	return 0;
}

void onic_ddr_mr_free(struct onic_ddr_pool *p, u64 off)
{
	unsigned long flags;
	u64 idx;

	if (off < ONIC_DDR_MR_TIER_OFF)
		return;
	idx = (off - ONIC_DDR_MR_TIER_OFF) / ONIC_DDR_MR_SMALL_SIZE;
	if (idx >= ONIC_DDR_MR_SMALL_COUNT)
		return;

	spin_lock_irqsave(&p->mr_lock, flags);
	clear_bit(idx, p->mr_small_bits);
	spin_unlock_irqrestore(&p->mr_lock, flags);
}
