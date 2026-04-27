# Testing the libqdma-vendored onic.ko (Option 3 BAR ownership)

Built on the dev host; needs to be scp'd to the FPGA host and insmod'd.

This build implements the canonical RecoNIC BAR-ownership pattern:
**libqdma is the sole owner of the PCIe BAR claims and ioremaps**;
onic borrows BAR pointers via `qdma_device_get_{config,user}_regs()`.
This replaces the temporary Option 2 release-the-claim hack.

## 1. Copy artifacts

From the dev host (this worktree):

```bash
cd /home/alex/mpi-shfs/fpga/open-nic-driver/.claude/worktrees/agent-a94cc7ba
scp onic.ko <fpga-host>:/tmp/onic.ko
```

The .ko is large (~14 MB) because libqdma is now linked in.  This is
expected — RecoNIC's onic.ko is similar.  At runtime libqdma is just
loaded in memory; nothing is allocated until `qdma_device_open` runs in
probe.

## 2. Reload the driver

On the FPGA host:

```bash
sudo rmmod onic           # if currently loaded
sudo dmesg -C             # clear so the next dmesg is just our boot
sudo insmod /tmp/onic.ko
```

## 3. Confirm probe and self-test

```bash
sudo dmesg | grep -E 'onic|qdma'
```

What you should see (in order):

```
onic: OpenNIC Linux Kernel Driver ...
onic: device is a master PF
onic: <name>: AXI Master Lite BAR 2 mapped at <ptr> (len=...)   <- new in Option 3
onic: ...   (other init lines: hardware, interrupt, ernic, register_netdev)
onic: onic_sysdma: ready via libqdma (h2c=<h> c2h=<c> staging_dma=0x... max_xfer=1048576)
onic: onic_sysdma: self-test - writing 64 B to DDR4 @ 0x0 (libqdma)
onic: onic_sysdma: self-test OK - H2C+C2H round-trip verified (libqdma)
```

What you should NOT see anymore (was an Option 2 artifact):

```
onic 0000:01:00.0: pci_release_mem_regions: ...   <- absent under Option 3
```

On rmmod, the Option 2 build emitted a benign warning about a redundant
`pci_disable_device`; under Option 3 that warning is gone because
`qdma_device_close` is the sole caller and `onic_remove` no longer makes
the redundant call.

If the read-back passes, the host->DDR4 path is correct end-to-end.
This is the regression we were chasing with the hand-rolled MM submit:
descriptor bytes were correct on readback, but transfers faulted at
0xff00000000.  Vendoring libqdma replaces the entire MM path with
AMD's shipping code.

## 4. If it still fails

Capture what dmesg actually shows for the sysdma lines, plus:

```bash
sudo cat /sys/kernel/debug/qdma-* 2>/dev/null     # libqdma debugfs (if mounted)
sudo dmesg | grep -E 'DMAR|IOMMU|qdma_descq|qdma_queue' | tail -50
sudo lspci -vvv -s <bdf> | head -80
```

Likely failure modes and what they mean:

- `qdma_device_open() failed: -EBUSY/-ENODEV` — libqdma can't claim the
  PCI function.  Under Option 3 onic should NEVER hold the BAR claim
  itself.  Verify that no `pci_request_mem_regions` call survived in
  the source (`grep pci_request onic_main.c` should return nothing).
  Also: if the kernel module load order ever puts another driver that
  binds the same BDF first, libqdma's claim will fail here.

- `libqdma reports user BAR N, expected 2 — refusing to bind` — the
  defensive check at probe step 4 fired.  R1 in the design plan.
  Inspect the shell's `qdma_get_user_bar` CSR; if the design genuinely
  uses a different user BAR, update the constant in `onic_setup_primary`
  (and the `SHELL_START` offset will likely move too).

- `libqdma reports qsets_max=N, need >= 32 for sysdma — refusing to bind`
  — the defensive check at probe step 5 fired (R8).  Either the shell
  exposes too few queues, or libqdma's resource manager reduced the
  available range.  sysdma's self-test uses qid 31; widening to 32 or
  more is the only way forward.

- `qdma_queue_add(H2C) failed (-EINVAL): ...` — qsets_max in
  qdma_dev_conf is smaller than ONIC_SYSDMA_REL_QID (31).
  We set qsets_max from the legacy `qdma_dev->num_queues` — verify the
  shell exposes >= 32 queues per function.

- `onic_sysdma: self-test WRITE FAILED: -ETIMEDOUT` — libqdma's
  internal wait_for_cmpl timed out.  This is what the hand-rolled
  path was failing on too.  Check IOMMU / DMAR messages in dmesg —
  on the failing run the hint was "DMAR: DMA Read NO_PASID for device
  ... addr 0xff00000000".  If you see that, try widening the per-PF
  DMA mask further, disabling IOMMU passthrough, or running with
  `intel_iommu=off`.

- `onic_sysdma: self-test DATA MISMATCH` — bytes round-trip but
  contents differ.  Means the descriptor *did* land at DDR4 but at
  the wrong offset, or our staging memcpy is racing the DMA engine.
  print_hex_dump is included in the failure path for diagnosis.

## 5. Removing the driver

```bash
sudo rmmod onic
```

This now also calls `libqdma_exit()`.  The teardown path closes the
sysdma queues (qdma_queue_stop + qdma_queue_remove), then closes the
libqdma device handle, then proceeds with the existing legacy QDMA
shell teardown.

## 6. Notes for the user

- Netdev path (CMAC0/CMAC1, RX/TX queues) is unchanged.  It still uses
  the renamed `qdma_legacy/` (formerly `qdma_access/`) hand-rolled
  helpers.  Vendoring libqdma did **not** migrate the netdev path —
  only sysdma.

- ERNIC, IB device, PTP, hwmon, ethtool — all unchanged.

- If insmod fails with "Unknown symbol" against the kernel:
  drop -DMBOX_INTERRUPT_DISABLE from the Makefile and rebuild.  We set
  that to match RecoNIC because we run libqdma in poll mode; in
  interrupt mode libqdma also pulls in mailbox handlers.
