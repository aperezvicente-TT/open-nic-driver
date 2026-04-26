/* SPDX-License-Identifier: GPL-2.0 */
/*
 * onic_sysdma.h - System DMA primitive for host <-> card-DDR4 transfers
 *
 * Reserves one QDMA queue (configured for AXI-MM mode, not Stream) and
 * exposes a synchronous host-to-DDR4 write primitive.  Used by the RDMA
 * verb path (B7+) to stage WQEs and MR-backing payloads into ERNIC's
 * DDR4-resident SQ/RQ/CQ rings.
 *
 * Design decisions:
 *  - One MM-mode queue, not pooled.  Driver-internal use only — RDMA
 *    verbs serialise their DDR4 stores via a mutex.  Throughput is not
 *    a goal; correctness and simplicity are.
 *  - Fixed-depth ring (default 256 descriptors) allocated once at probe.
 *  - Each onic_ddr4_write() call: kmemdup payload into a coherent
 *    staging buffer, build one MM descriptor, push, doorbell, poll
 *    completion, free staging.  ~5-10 us latency for small payloads,
 *    no batching.
 *  - MM mode runs alongside the existing ST-mode netdev queues.  The
 *    QDMA IP (en_axi_mm_qdma=true + dma_intf_sel=AXI_MM_and_AXI_Stream)
 *    lets each queue choose mode independently via its software
 *    context register.
 */

#ifndef __ONIC_SYSDMA_H__
#define __ONIC_SYSDMA_H__

#include <linux/types.h>

struct onic_private;

/* Reserved queue id used for system DMA.  Picked from the top end of the
 * QDMA queue space so it never collides with netdev queues (which start
 * at 0).  Adjust if NUM_QUEUE in the FPGA shrinks below 64. */
#define ONIC_SYSDMA_QID_OFFSET     2047  /* last queue */
#define ONIC_SYSDMA_RING_DEPTH     256
#define ONIC_SYSDMA_DESC_SIZE      32    /* MM descriptor: 32 bytes per PG302 */
#define ONIC_SYSDMA_TIMEOUT_MS     500   /* completion poll cap */

/**
 * onic_sysdma_init - Allocate ring + CMPT + program QDMA queue for MM mode.
 * Called once at driver probe (master PF only).
 *
 * Return: 0 on success, negative errno on failure.
 */
int onic_sysdma_init(struct onic_private *priv);

/**
 * onic_sysdma_fini - Tear down the system DMA queue.
 * Called at driver remove (master PF only).
 */
void onic_sysdma_fini(struct onic_private *priv);

/**
 * onic_ddr4_write - Synchronous host-to-DDR4 copy.
 * @priv:    private data of the master PF.
 * @dst_axi: destination AXI address on the card-side dev_mem crossbar.
 *           For DDR4-backed buffers, this is the offset into DDR4 (the
 *           crossbar maps DDR4 to AXI offset 0; ERNIC sees DDR4 at
 *           0xa3500000_00000000 but the crossbar's slave-0 address
 *           map starts at 0).
 * @src:     kernel-mode source buffer (will be staged).
 * @len:     byte count.  Capped at ONIC_SYSDMA_MAX_XFER (1 MiB).
 *
 * Synchronous: blocks until QDMA reports completion or timeout.
 * Caller must serialise via priv->sysdma->mutex (acquired internally).
 *
 * Return: 0 on success, -ETIMEDOUT on poll exhaustion, -EIO on QDMA
 *         error completion, -EINVAL on bad arguments.
 */
int onic_ddr4_write(struct onic_private *priv, u64 dst_axi,
                    const void *src, size_t len);

/**
 * onic_ddr4_read - Synchronous DDR4-to-host copy.  Mirror of write.
 * Used for round-trip verification at probe and for B8/B11 paths
 * that need to read CQEs out of DDR4-resident CQ rings.
 *
 * Return: 0 on success, negative errno on failure.
 */
int onic_ddr4_read(struct onic_private *priv, void *dst,
                   u64 src_axi, size_t len);

#define ONIC_SYSDMA_MAX_XFER       (1u << 20)   /* 1 MiB per call */

#endif /* __ONIC_SYSDMA_H__ */
