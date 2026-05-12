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

/* F4 §2.5 — queue tier
 *
 * Per-QP slot layout sized for spec-minimum-tested queue depth.  PG332
 * v4.3 §"RC QP Creation" (p.71) is explicit:
 *
 *   "The minimum tested depth of the queues is 16."
 *
 * PG332 §Memory Requirement Table 10 (p.69) uses 128 entries per queue
 * as the reference dimensioning.  We target 16 here as a balance:
 *   - the slot fits cleanly in the existing queue tier (0x40000..
 *     0x400000 = 3.75 MiB) without disturbing the MR tier or per-ERNIC
 *     GBUF region;
 *   - it satisfies the spec's documented validity floor, unlike the
 *     prior depth=1 which the IP was never characterized at and which
 *     caused multi-iter pingpong to stall (2026-05-11);
 *   - bumping later to 128 (full spec reference) requires shifting
 *     MR_TIER_OFF + GBUF base — see project_b7_send_path_fixed_2026_05_11
 *     for the layout sketch.
 *
 * Per-QP 128-KiB slot breakdown:
 *   0x00000 .. 0x01FFF   8 KiB  SQ        — 128 × 64-B WQE (room for full
 *                                            spec-reference depth even
 *                                            though our RQ caps at 16)
 *   0x02000 .. 0x11FFF  64 KiB  RQ        — 16 × 4-KiB RQE matches
 *                                            ERNIC_RQE_SIZE_FIELD=16
 *                                            (PMTU 4096 single-packet)
 *   0x12000 .. 0x12FFF   4 KiB  CQ        — 128 × 4-B CQE + slack
 *   0x13000 .. 0x13FFF   4 KiB  SEND-payload staging
 *   0x14000 .. 0x1FFEF  48 KiB  reserved for future use
 *   0x1FFF0 .. 0x1FFF7   8 B    RQDB scratch (RQWPTRDBADDi target)
 *   0x1FFF8 .. 0x1FFFF   8 B    CQDB scratch (CQDBADDi target)
 */
#define ONIC_DDR_QUEUE_TIER_OFF         0x00040000u
#define ONIC_DDR_QUEUE_SLOT_SIZE        0x00020000u   /* 128 KiB */
#define ONIC_DDR_QUEUE_SQ_OFF           0x00000000u
#define ONIC_DDR_QUEUE_RQ_OFF           0x00002000u
#define ONIC_DDR_QUEUE_CQ_OFF           0x00012000u

/* Per-WR SEND-payload staging area.  ERNIC v4.2 fetches packet payload
 * via M_AXI from laddr — libreconic's reference test ALWAYS sets laddr
 * to a real DDR4 address and leaves send_small_payload zero, so the
 * BTH-inline path appears not to work on this IP variant. */
#define ONIC_DDR_QUEUE_SEND_PAYLOAD_OFF 0x00013000u
#define ONIC_DDR_QUEUE_SEND_PAYLOAD_STR 64u
#define ONIC_DDR_QUEUE_SEND_PAYLOAD_MAX 64u

/* Doorbell DMA scratch — ERNIC v4.2 unconditionally DMA-writes the RQ
 * write-pointer and CQ doorbell to the addresses in RQWPTRDBADDi /
 * CQDBADDi when QP enters RTS, regardless of QPCONFi[4] HWHSHKDIS.
 * If those registers are 0 the writes hit host PCIe address 0 and the
 * IOMMU rejects them (AMD-Vi IO_PAGE_FAULT / DMAR DMA Write fault),
 * stalling the engine.  Park them in the unused tail of each slot so
 * writes land in DDR4 via the 0xA350.. tag and never traverse PCIe. */
#define ONIC_DDR_QUEUE_RQDB_OFF         0x0001FFF0u
#define ONIC_DDR_QUEUE_CQDB_OFF         0x0001FFF8u

/* Sanity: per-slot inner regions must not overlap and must fit. */
#define _ONIC_DDR_RQ_SIZE       (ONIC_DDR_QUEUE_CQ_OFF - ONIC_DDR_QUEUE_RQ_OFF)
#define _ONIC_DDR_SQ_SIZE       (ONIC_DDR_QUEUE_RQ_OFF - ONIC_DDR_QUEUE_SQ_OFF)
#define _ONIC_DDR_CQ_SIZE       (ONIC_DDR_QUEUE_SEND_PAYLOAD_OFF - ONIC_DDR_QUEUE_CQ_OFF)

/* B5 MR pool (single size class).  Sized for Perf #3 — 16 MiB per slot
 * lets `ib_write_bw -s 4096..16777216` register a payload buffer in one
 * MR.  Total pool = 16 MiB × 64 = 1 GiB at offset 0x00400000, well within
 * the 8 GiB per-ERNIC region cap. */
#define ONIC_DDR_MR_TIER_OFF            0x00400000u
#define ONIC_DDR_MR_SMALL_SIZE          0x01000000u   /* 16 MiB */
#define ONIC_DDR_MR_SMALL_COUNT         64u

/* Per-ERNIC region size — F4 §2.2 8 GiB each until lifted. */
#define ONIC_DDR_ERNIC_REGION_SIZE      0x200000000ULL

/* QP 0 + QP 1 are reserved (PG332 §7; F1 §5.4).
 *
 * XRNIC_CONF_QP_EN was discovered to be a 6-bit-wide bitmap on this IP
 * (see `project_b7_qp_en_bitmap_2026_05_05`), so the engine can only
 * have 6 enabled QPs at once.  More importantly, per PG332 the actual
 * QP count is configured at IP-generation time via the C_NUM_QP
 * parameter; reads of XRNIC_CONF_QP_EN show our build sets it to 6.
 *
 * Queue-tier capacity check (sanity):
 *   ONIC_DDR_QUEUE_TIER_OFF + ONIC_QP_MAX * SLOT_SIZE  must be
 *   <= ONIC_DDR_MR_TIER_OFF.
 *
 * Current: 0x40000 + 16 * 0x20000 = 0x240000 < 0x400000 ✓
 *
 * For future growth (RQ depth 128 → SLOT_SIZE 1 MiB), MR_TIER_OFF
 * must move beyond 0x40000 + 16 * 0x100000 = 0x1040000 AND the
 * per-ERNIC GBUF region (0x01000000) must also shift to avoid
 * overlap. */
#define ONIC_QP_RESERVED_LO             2u
#define ONIC_QP_MAX                     16u

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
