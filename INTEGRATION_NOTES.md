# libqdma vendoring — integration notes

Worktree: `/home/alex/mpi-shfs/fpga/open-nic-driver/.claude/worktrees/agent-a94cc7ba`
Branch:   `worktree-agent-a94cc7ba` (forked off `feature/ernic-v4.2-rebuild`)
Started:  2026-04-26 02:17 UTC
Finished: 2026-04-26 02:50 UTC

## Plan executed

All four phases per the plan, all four build gates green:

- Phase A: rename `qdma_access/` -> `qdma_legacy/` (commit `25b50e6`)
- Phase B+C: vendor `libqdma/` from dma_ip_drivers + Makefile integration (commit `7795428`)
- Phase D: wire `libqdma_init/exit` + `qdma_device_open/close` (commit `a4e1721`)
- Phase E: rewrite `onic_sysdma.c` over libqdma's public API (commit `2e2c920`)

Final `onic.ko` size: 14 MB (was ~1 MB).  Symbols `qdma_device_open`,
`qdma_queue_add`, `qdma_request_submit`, `libqdma_init`,
`libqdma_exit` all defined.

## Deviations from the plan

### 1. Worktree state at start

The worktree branch was a stale fork-only history (last commit `3de2269`
"resolve other compiler warnings") missing the entire B7 sysdma series
that the plan operates on.  The user's main tree is at
`feature/ernic-v4.2-rebuild` (`2794dfb`) with seven uncommitted
modifications staged on top.

Recovery: `git reset --hard feature/ernic-v4.2-rebuild` in the
worktree, then copied the seven uncommitted files
(onic_ethtool.c, onic_hardware.{c,h}, onic_lib.h, onic_sysdma.c,
qdma_access/qdma_device.{c,h}) from the main tree and committed them
as a "wip: import uncommitted main tree changes" baseline commit
(`35690fc`) so the integration could start from a green tree.

This means the user's uncommitted main-tree edits are preserved in the
worktree's history but not in the main tree — when they review/merge,
they should compare against `feature/ernic-v4.2-rebuild` *plus* their
working-directory changes, not just the branch tip.

### 2. libreconic symlink

`onic_ib.c` includes `../libreconic/reconic_reg.h`.  In the main tree
this resolves to `/home/alex/mpi-shfs/fpga/libreconic/`.  In the
worktree the relative path goes to `.claude/worktrees/libreconic` which
doesn't exist.  Fix: created a symlink
`/home/alex/mpi-shfs/fpga/open-nic-driver/.claude/worktrees/libreconic
-> /home/alex/mpi-shfs/fpga/libreconic`.  This is local to the worktree
filesystem layout and won't affect anything in the main tree.

### 3. qsets_max derivation in qdma_dev_conf

The plan said "qsets_max = priv->hw.num_q" but `struct onic_hardware`
has no `num_q` member.  Derived it instead from
`((struct qdma_dev *)priv->hw.qdma)->num_queues` (the legacy struct's
field) with a fallback of 64 if the legacy handle isn't set.

### 4. Comment-level qdma_access references

Two comment blocks (in `onic_qdma_mm.h` and the original `onic_sysdma.c`
TODO header) still mentioned `qdma_access/` after Phase A.  Both files
were deleted/rewritten in Phase E, so the final tree is clean.

### 5. -Werror / kernel module build

The plan said to drop -Werror because libqdma has unused-variable
warnings.  In practice libqdma compiled cleanly under gcc-12 and
kernel 6.8 — zero warnings, zero errors.  Dropped -Werror anyway per
plan (defensive against future kernel/gcc bumps).  -Wall is still on.

### 6. `clear_hardware` label rename

In `onic_setup_primary`, inserted `qdma_device_open` between
`onic_init_hardware` and `onic_init_interrupt`, which required a new
`clear_qdma:` goto target.  Renamed the existing `clear_hardware:`
label to `clear_qdma:` and made it fall through to
`onic_clear_hardware`; the failure-path label graph is now
`clear_interrupt -> clear_qdma -> clear_capacity -> free_netdev`.
Secondary's `clear_hardware:` label is unaffected (separate function).

## Things the user should look at

1. **Master PF only — secondary CMAC1 has no libqdma handle.**
   `qdma_device_open` is called once per PCI function in our driver,
   in `onic_setup_primary` only.  The secondary netdev for CMAC1 (when
   `num_cmacs >= 2`) reaches into the primary's QDMA via a child
   `qdma_dev` that was already created in the legacy hardware setup —
   that path is untouched.  If we ever want sysdma on CMAC1 we'd need
   either (a) a second `qdma_device_open` call (likely conflicts with
   our shared-MSI-X model) or (b) routing CMAC1's sysdma through the
   primary's `qdma_dev_handle` with an offset queue.  Out of scope.

2. **Coherent mask widening still in place.**
   The hand-rolled path widened to 64-bit at sysdma_init, allocated,
   then restored to 32-bit.  Kept this in the new path because libqdma
   does its own coherent allocations inside `qdma_queue_add`.  Without
   the widen, those allocations would land in the same 0xff?? IOVA
   range that the original DMAR fault came from.  If runtime testing
   reveals libqdma still gets a low-address ring, we may need to keep
   the mask at 64-bit longer.

3. **fp_done = NULL means blocking submit.**
   libqdma's `qdma_request_submit` waits internally on a wait queue
   when fp_done is NULL.  Confirmed by reading
   `libqdma_export.c:2413` (`wait = req->fp_done ? 0 : 1;`).  This
   matches the plan's requirement and the old hand-rolled
   "submit + poll" semantics.

4. **Netdev path still on legacy.**
   Netdev queue setup, doorbells, completion polling, tx/rx fast
   paths — all still go through `qdma_legacy/` (renamed from
   `qdma_access/`).  No data-plane change.  Only sysdma's *one* queue
   per PF moved to libqdma.

5. **POLL_MODE was load-bearing.**
   With INTR_MODE libqdma allocates MSI-X vectors that conflict with
   our `onic_init_interrupt`.  POLL_MODE + zero msix counts (as set
   in `priv->qdma_dev_conf`) make `qdma_device_open` a register-only
   operation.  Don't change this without also rewriting the netdev
   IRQ allocator.

## Risk register status

- R1 (struct qdma_dev collision): Mitigated.  `grep -l 'libqdma/qdma_device.h' onic_*.c` returns nothing.
- R6 (POLL_MODE): Set in `priv->qdma_dev_conf.qdma_drv_mode`.
- R8 (Threading): `libqdma_init(0, NULL)`.
- R10 (Coherent mask): Widened to 64-bit at sysdma_init, restored to 32-bit afterward.  See note 2 above if probe trouble appears.
- R11 (API drift): Did not need to fall back — vendored tree from `dma_ip_drivers/` built first try.
- R12 (Probe ordering): `onic_sysdma_init` runs after `qdma_device_open` succeeds.  Sysdma init also explicitly checks `priv->qdma_dev_handle != 0` before doing anything.

## Files changed

- `Makefile` — SRC_FOLDERS list + per-folder -I, dropped -Werror, added -DMBOX_INTERRUPT_DISABLE.
- `onic.h` — include `libqdma/libqdma_export.h`, add `qdma_dev_handle` + `qdma_dev_conf` fields.
- `onic_main.c` — libqdma_init/exit in module init/exit, qdma_device_open in setup_primary, qdma_device_close in teardown_netdev, clear_hardware -> clear_qdma label rename.
- `onic_sysdma.c` — total rewrite over libqdma queue API.
- `onic_sysdma.h` — drop now-unused ring/desc constants.
- `onic_qdma_mm.h` — deleted (libqdma owns descriptor format).
- `onic_netdev.c`, `onic_main.c`, `onic_sysdma.c` — `qdma_access/` -> `qdma_legacy/` include path updates.
- `qdma_access/` -> `qdma_legacy/` rename (15 files moved).
- `libqdma/` — new tree, ~50 files copied verbatim from dma_ip_drivers.

## Final build artifacts

- `onic.ko` — 14 MB, BTF skipped (no vmlinux), zero compile warnings.
- `/tmp/onic_libqdma_final_build.log` — clean compile of all phases.

## Stopping condition

Plan's stopping condition was "module builds clean and is ready for the
user to scp to the FPGA host."  Met.  No hardware run was attempted.
See `TESTING.md` for the user's runbook.

---

# Option 3: BAR ownership refactor — 2026-04-27

Branch:   `worktree-agent-a94cc7ba` (continued)
Started:  2026-04-27 13:42 UTC
Finished: 2026-04-27 13:50 UTC

## Phases

1. Revert Option 2 — `git revert 4f2dc17` (commit `ecbf2a9`).
2. Add libqdma accessors `qdma_device_get_{config,user}_regs()` plus a
   user-BAR ioremap inside `xdev_identify_bars` (commit `f78e150`).
3. Refactor `qdma_legacy/qdma_device.{c,h}` to take a borrowed BAR 0
   pointer; add `borrowed_addr` flag mirroring `is_child` to skip
   iounmap on destroy (commit `40851eb`).
4. Update `onic_hardware.c` to source `hw->addr` (BAR2 + SHELL_START)
   and the BAR0 pointer from libqdma; drop the iounmap_bar2 cleanup
   path and the BAR2 pci_iounmap in `onic_clear_hardware`
   (commit `42186f9`).
5. Update `onic_main.c`: drop pci_request_mem_regions /
   pci_release_mem_regions / pci_disable_device redundancy, reorder
   probe so qdma_device_open runs before onic_init_hardware, add
   defensive checks (R1 + R8), reorder failure-label cascade, reorder
   teardown so clear_hardware runs before qdma_device_close
   (commit `77d6f92`).

## Key architectural detail discovered

The plan said "after the user-BAR ioremap" in xdev_map_bars, but
libqdma's stock `xdev_map_bars` only ioremaps the *config* BAR.  The
user (AXI Master Lite, BAR 2) BAR is referenced by number only and
never mapped by libqdma itself.  I added the user-BAR ioremap inside
`xdev_identify_bars` (after `bar_num_user` is identified) and the
matching iounmap in `xdev_unmap_bars`.  This is a sensible extension
of libqdma rather than a hack — the field, accessor, and ioremap all
live behind libqdma's abstractions.

## Teardown ordering subtlety

Original plan put `qdma_device_close` BEFORE `onic_clear_hardware` in
`onic_teardown_netdev`.  That doesn't work under Option 3 because
`onic_clear_hardware` writes through `hw->addr` (the QDMA shell reset
and `QDMA_FUNC_OFFSET_QCONF(0) = 0`), and `hw->addr` is borrowed from
libqdma — once `qdma_device_close` iounmaps BAR 2, that pointer is
dangling.  Reordered: `onic_clear_hardware` first (still has a valid
mapping), then `qdma_device_close` (the iounmap point).  The same
ordering inversion is reflected in the failure-label cascade in
`onic_setup_primary`.

## Probe failure path: pci_disable_device balance

`qdma_device_close` itself calls `pci_disable_device`.  On the probe
failure path, after `onic_setup_primary` partially unwinds (which may
or may not have called `qdma_device_close` depending on how far it
got), the outer `onic_probe` must avoid double-disabling.  Fix:
`if (pci_is_enabled(pdev)) pci_disable_device(pdev)` after a failed
setup_primary.  The earlier `goto disable_device` from the dma_set_mask
failure remains an unconditional disable because pci_enable_device_mem
just succeeded one line above.

## Risk register status (Option 3 update)

- R1 (bar_num_user): defensive check at probe step 4 fails fast if
  libqdma reports anything other than 2.
- R2 (qdma_device_close double-disable): handled — `onic_remove` no
  longer calls `pci_disable_device`, and the probe failure path uses
  `pci_is_enabled()` to gate.
- R3 (double-mapping eliminated): legacy borrows BAR 0 via
  `qdma_create_dev(pdev, bar0_regs)`.  Onic does not iomap BAR 2; it
  borrows libqdma's mapping with a SHELL_START offset.
- R4 (sysdma_fini before qdma_device_close): verified, unchanged.
- R5 (POLL_MODE): unchanged.
- R6 (mem regions only on AU200): unchanged — pci_request_regions in
  libqdma is now the only claim.
- R7 (rmmod warning eliminated): redundant calls removed.
- R8 (qsets_max underflow): defensive check at probe step 5.
- R9 (FMAP programmed twice): unchanged (harmless).
- R10 (bar identification log): kept the existing libqdma `pr_info`
  plus an added "AXI Master Lite BAR N mapped at PTR" line for
  Option 3 verification.

## Files changed (Option 3 only)

- `libqdma/xdev.h` — add `user_regs` field.
- `libqdma/xdev.c` — ioremap user BAR in `xdev_identify_bars`,
  iounmap in `xdev_unmap_bars`, define accessors.
- `libqdma/libqdma_export.h` — declare accessors.
- `qdma_legacy/qdma_device.{c,h}` — borrow BAR 0 instead of mapping.
- `onic_hardware.c` — source BAR pointers from libqdma; drop
  iounmap_bar2 path.
- `onic_main.c` — drop pci_request/release/disable redundancy;
  reorder probe + teardown; add defensive checks.

## Build artifact

`onic.ko` — 14 MB, zero compile warnings (one stock kernel notice
"compiler differs" is host-environment unrelated).  Build log:
`/tmp/option3_build.log`.

## Stopping condition

Plan's stopping condition was "Final clean build: clean, similar size
.ko (~14 MB)."  Met.  No hardware run attempted; that is the user's
job per the plan.

## ST datapath regression after Option 3 — diagnosis and fix (2026-04-26)

### Symptom (verified on hardware after `2a16bb1`)

For both netdev interfaces (enp1s0, enp1s0d1):

| Path | Driver counter | CMAC ethtool counter            |
|------|----------------|----------------------------------|
| TX   | 37 packets     | 0  (`stat_tx_total_pkts: 0`)    |
| RX   | 0  packets     | 70 (`stat_rx_total_pkts: 70`)   |

Sysdma (MM-mode) self-test passes — QDMA engine itself is alive.  Only
the streaming (ST-mode) netdev datapath was broken.

### Root cause

Option 3 made libqdma the BAR owner and required `qdma_device_open` to
run BEFORE `onic_init_hardware`.  `qdma_device_open` invokes
`eqdma_set_default_global_csr()` which programs the QDMA core's global
CSR pool tables (rng_sz / c2h_buf_sz / c2h_timer_cnt / c2h_cnt_th) and,
for EQDMA5, calls `eqdma_set_perf_opt()` which carefully tunes 14
performance/throttling registers (visible as `eqdma_set_perf_opt: reg
= 0x...` lines in dmesg).

Then `onic_init_hardware_master()` ran legacy
`onic_qdma_init_csr(qdev)` which re-programmed the SAME global CSR
pools with QDMA4-style values AND directly clobbered three of the
EQDMA5 perf_opt registers:

- `0x250` GLBL_DSC_CFG       — overwritten with legacy value
- `0xB08` C2H_PFCH_CFG       — overwritten (also computed from
  `0xBE0` cache-depth using QDMA4-style field layout)
- `0xE24` H2C_REQ_THROT      — overwritten with legacy value
- `0xB50` C2H_WB_COAL_CFG    — overwritten
- `0x204…0x240` GLBL_RNG_SZ pool — overwritten with off-by-one values
- `0xA00…` C2H timer pool    — overwritten
- `0xA40…` C2H counter-th pool — overwritten
- `0xAB0…` C2H buffer-size pool — overwritten

The result: ST-mode queue contexts written by the driver indexed pool
entries with the wrong sizes, and EQDMA5's perf_opt landed in a hybrid
state that caused TX descriptors to land in QDMA but never advance to
CMAC, and CMAC RX completions to never propagate up.  MM-mode sysdma
is permissive enough to tolerate the mis-tuned perf_opt and uses a
single fixed-size descriptor ring, so it kept working.

The previous probe order (libqdma after onic) made the legacy CSR
init "win" the race and ST happened to work — by accident, on values
that happened to match QDMA4-era expectations for that shell.

### Fix chosen — Fix A (delete the redundant CSR init)

`onic_qdma_init_csr()` and its call site are removed.  All registers
it touched are programmed by libqdma's `eqdma_set_default_global_csr`
(BEFORE `onic_init_hardware_master` runs, per Option 3 ordering).

The driver-side ring/buffer/timer/counter pool tables in `onic_hardware.c`
are re-aligned to match libqdma's defaults so that per-queue
`rngsz_idx` / `bufsz_idx` etc. address the same hardware sizes the
driver allocates DMA rings for:

```
rngcnt_pool[0] = 2049  (was 4096)  — match libqdma rng_sz[0]
c2h_timer_pool = {1,2,4,...}       — match libqdma tmr_cnt
c2h_thres_pool = {2,4,8,16,...}    — match libqdma cnt_th
c2h_bufsz_pool                     — already matched libqdma buf_sz
```

Fix B (reorder shell reset before `qdma_device_open`) was not needed:
the only "shell reset" in `onic_init_hardware_master` is
`onic_reset_cmac_shell` which targets shell-only registers
(SYSCFG_OFFSET_SHELL_RESET) that libqdma never touches.  The
QDMA-shell reset that DOES run before libqdma's perf_opt is internal
to libqdma's own bring-up and does not clobber perf_opt.

Sysdma's MM round-trip is preserved logically: `qdma_queue_add` /
`qdma_request_submit` are unaffected by deleting `onic_qdma_init_csr`
because libqdma was already programming the same registers (and
correctly).  If anything, sysdma is now using the canonical libqdma
values rather than the legacy override.

### Files changed

- `onic_hardware.c` — delete `onic_qdma_init_csr()` and its call
  site; re-align pool tables to libqdma defaults; long comment in
  the pool-table block explains the contract.
- `INTEGRATION_NOTES.md` — this section.
- `TESTING.md` — add ethtool counter check (TX/RX counters must
  increment under traffic, not just driver counters).

### What we did NOT do

- Did NOT revert Option 3's BAR ownership.  `qdma_device_get_user_regs`,
  `qdma_device_get_config_regs`, the new probe ordering, the
  `pci_request_mem_regions` removal — all preserved.
- Did NOT add `pci_request_mem_regions` back.  libqdma is the BAR owner.
- Did NOT touch the sysdma path.  `qdma_queue_add`, `qdma_request_submit`,
  the self-test — unchanged.

## 8. Legacy ST H2C descriptor SOP/EOP fix (2026-04-26)

After the Option 3 + perf_opt fixes the QDMA-core CSRs were correct but
ST TX still produced `stat_tx_total_pkts = 0` at the CMAC under ping
traffic, even though the driver-side ifconfig TX counter incremented
normally.  Diagnostic chain:

1. CMAC counters frozen at 0 ⇒ frames never reach the CMAC TX FIFO.
2. QDMA H2C-engine debug counters showed descriptors fetched and
   completions written ⇒ QDMA itself was running.
3. Comparison of the legacy `qdma_pack_h2c_st_desc()` byte layout
   against libqdma's reference `struct qdma_h2c_desc` (in
   `libqdma/qdma_regs.h`) showed bytes 6-7 — where libqdma writes the
   `flags` field, including `S_H2C_DESC_F_SOP` (=1) and
   `S_H2C_DESC_F_EOP` (=2) — were left as compiler padding (zero) by
   the legacy struct/packer.
4. EQDMA5 Soft IP requires SOP|EOP on every single-descriptor frame.
   With both bits clear the IP silently drops the frame between QDMA
   and CMAC.

### Fix

In `qdma_legacy/qdma_export.h`:
- Replace the 16-bit compiler padding in `struct qdma_h2c_st_desc`
  with a named `u16 flags` field at the same offset (bytes 6-7).
- Add `QDMA_H2C_ST_DESC_F_SOP`/`_EOP` (BIT(0)/BIT(1)) and
  `QDMA_H2C_ST_DESC_DW0_FLAGS_MASK` (bits 63:48 of DW0).  Bit
  positions match libqdma's `S_H2C_DESC_F_SOP`/`_EOP` macros.

In `qdma_legacy/qdma_export.c::qdma_pack_h2c_st_desc()`:
- OR `SOP|EOP` into the caller-supplied `flags` and pack the result
  into bits 63:48 of DW0.  Set unconditionally because every netdev
  TX frame on this driver is a single descriptor (caller in
  `onic_xmit_frame` / `onic_xmit_xdp_ring` always writes one
  descriptor per skb/xdp_frame).

DW0 layout (LE on x86 / PCIe):

```
bits  [31: 0]  metadata
bits  [47:32]  len
bits  [63:48]  flags    <-- bit 0 = SOP, bit 1 = EOP  (was zero padding)
DW1   [63: 0]  src_addr
```

### Files changed

- `qdma_legacy/qdma_export.h` — add named `flags` field; add
  `QDMA_H2C_ST_DESC_F_SOP/_EOP` and `_FLAGS_MASK` macros.
- `qdma_legacy/qdma_export.c` — pack `flags | SOP | EOP` into DW0
  bits 63:48.
- `INTEGRATION_NOTES.md` — this section.
- `TESTING.md` — concrete post-fix expectation: ping succeeds and
  `stat_tx_total_pkts` increments.

### What we did NOT do

- Did NOT modify libqdma sources — fix is in `qdma_legacy/` only,
  matching the constraint that libqdma stays vendored verbatim.
- Did NOT touch sysdma — sysdma uses libqdma's `qdma_request_submit`
  which goes through `struct qdma_h2c_desc` and already sets SOP/EOP
  correctly when needed (`libqdma/qdma_descq.c`, ~line 632).
- Did NOT change the callers in `onic_netdev.c`.  They leave
  `desc.flags = 0` (default-init); the packer ORs SOP|EOP in.

## Secondary-PF queue diagnostics ([SEC_DIAG])

Diagnostic-only `pr_info` traces added to find the bug in the secondary
netdev (CMAC1, qid_base=64) datapath: TX descriptors are written and
the doorbell is rung, but neither CMAC0 nor CMAC1 sees frames at
`stat_tx_total_pkts`.

All prints are tagged `[SEC_DIAG]` for easy filtering:

```
sudo dmesg -T | grep '\[SEC_DIAG\]'
```

### Diagnostics, what each line means

- **`child_qdev: parent->addr=… child->addr=… q_base=…`**
  (`qdma_legacy/qdma_device.c::qdma_create_child_dev`)
  Fires once when the secondary's child `qdma_dev` wrapper is created.
  `parent->addr` and `child->addr` MUST be identical — both should
  point to libqdma's borrowed BAR0 ioremap.  If they differ, the
  child re-mapped BAR0 (wasteful, but only matters if the addresses
  look wildly wrong, e.g. one is NULL).  `q_base` should be 64 for
  CMAC1 (one print) and the primary path does not hit this print at
  all (primary uses `qdma_create_dev`, not `qdma_create_child_dev`).

- **`write_sw_ctxt: qdev=… qdev->q_base=… relative_qid=… abs_qid=…
  dir=… desc_base=0x… qen=… func_id=…`**
  (`qdma_legacy/qdma_context.c::qdma_write_sw_ctxt`)
  Fires once per queue per direction during `onic_open_netdev`.
  Cross-check that secondary writes to `abs_qid = 64..77`
  (q_base=64 + relative 0..13) for both H2C (dir=0) and C2H (dir=1).
  `qen` must be 1 and `desc_base` must be a non-zero DMA address.
  If primary's contexts at abs_qid 0..13 program `qen=1` but
  secondary's at 64..77 silently get a different func_id or qen=0,
  that's hypothesis #2 confirmed.

- **`set_q_pidx: q_base=… rel_qid=… abs_qid=… dir=… offset=0x… val=0x…`**
  (`onic_hardware.c::onic_qdma_set_q_pidx`)
  Fires for every TX (dir=0) and RX (dir=1) doorbell ring.  The DMAP
  PIDX register block is indexed by ABSOLUTE qid, so for the secondary
  to deliver packets the offset MUST step through the
  `0x18000 + 64*16` … `0x18000 + 77*16` range (or the C2H equivalent
  at 0x18004 + abs_qid*16).  Cross-reference with the SW context
  write trace: doorbell `abs_qid` for each packet must equal the
  `abs_qid` used at SW-context program time.  If the doorbell goes to
  abs_qid 64 but no SW context was ever written for abs_qid 64, the
  IP discards the doorbell — no error, no frame.

- **`FMAP readback (func_id=…): W0=0x… W1=0x… (qbase=… qmax=…)
  [wrote qbase=… qmax=…]`**
  (`onic_hardware.c::onic_init_hardware_master`)
  Fires once after the master writes the FMAP context.  The decoded
  `qbase`/`qmax` must equal what we wrote (typically qbase=0,
  qmax=128 for a 2-CMAC build).  If `qmax` reads back as 64 even
  though we wrote 128, libqdma or another agent later clobbered FMAP
  back to its `qsets_max` value, and qids 64-127 are NOT claimed by
  this function — every context write/read at those qids targets a
  function-less slot.  That would be hypothesis #3 confirmed.

- **`SW_CTXT abs_qid=… H2C (cmac_id=… q_base=… rel_qid=0 rv=…):
  W0=0x… W1=0x… W2=0x… W3=0x… W4=0x…`** plus a decoded follow-up
  line (`qen`, `desc_base`, `pidx`, `func_id`).
  (`onic_netdev.c::onic_open_netdev`)
  Fires once per netdev open, after `onic_init_tx_resource` and
  `onic_init_rx_resource` complete.  This is the load-bearing
  diagnostic.  Run from primary first (`ip link set enp1s0 up`) to
  capture the known-working `abs_qid=0` baseline, then bring up
  secondary (`ip link set enp1s0d1 up`) for `abs_qid=64`.
  - `qen` MUST be 1.  If 0, the queue is not enabled in the IP and
    every doorbell at that abs_qid is silently dropped.
  - `desc_base` MUST equal a non-zero DMA address that matches the
    `desc_base` printed by the corresponding `write_sw_ctxt` line
    above.  If it reads back zero, the FMAP didn't claim this qid
    for the function and the indirect-context write went to a
    nonexistent slot.
  - All zeros across W0..W4 → hypothesis #2 OR #3 confirmed.
  - `func_id` must equal the primary's func_id (single-PF design).

### Files changed

- `qdma_legacy/qdma_device.c` — add child-qdev creation print.
- `qdma_legacy/qdma_context.c` — add write_sw_ctxt entry print and
  new `qdma_read_sw_ctxt_raw` helper.
- `qdma_legacy/qdma_context.h` — declare `qdma_read_sw_ctxt_raw`.
- `onic_hardware.c` — add doorbell abs_qid print and FMAP readback
  inside `onic_init_hardware_master`.
- `onic_netdev.c` — include `qdma_context.h`; in `onic_open_netdev`,
  read back the H2C SW context for relative qid 0 and dump it.

### How to read the trace from a single ping

1. `sudo modprobe onic` (or `insmod ./onic.ko`).
2. Bring up primary: `sudo ip link set enp1s0 up && sudo ip addr add
   10.0.0.2/24 dev enp1s0`.
3. Bring up secondary: `sudo ip link set enp1s0d1 up && sudo ip addr
   add 10.0.0.3/24 dev enp1s0d1`.
4. Move cable to CMAC1 and ping from secondary:
   `sudo ping -I enp1s0d1 -c 1 10.0.0.1`.
5. `sudo dmesg -T | grep '\[SEC_DIAG\]' > /tmp/sec_diag.log` and
   inspect.

Compare the secondary lines (`abs_qid` 64-77) with the primary
baseline (`abs_qid` 0-13).  Any structural divergence (different
func_id, qen=0, desc_base mismatch, FMAP qmax narrowed to 64,
doorbell hitting wrong abs_qid) localises the bug to one of the
three hypotheses.

## `[DBG_XPATH]` per-packet trace — dual-CMAC drop localisation

When CMAC0 (`enp1s0`) and CMAC1 (`enp1s0d1`) are both link-up and
receiving traffic, `enp1s0d1` shows ~46% bursty packet loss on a 1
pps ping (drops are NOT throughput-related — 7 of 15 lost in 15 s).
The `[DBG_XPATH]` trace fires on every TX submission, every TX
completion advance, every NAPI poll entry/exit, and every delivered
RX packet, so the drop site can be pinpointed by correlating the
packet sequence on each netdev.

### Enabling

`onic_debug_level >= 4` activates the trace.  Load with
`sudo insmod onic.ko debug_level=4` (or
`echo 4 > /sys/module/onic/parameters/debug_level` at runtime).
At 1 pps the trace produces a few hundred lines per minute — fine
for normal use.  Drop back to `debug_level=0` after debugging.

### Trace tags (all prefixed `[DBG_XPATH]`)

- `TX_SUB <netdev> qid=R abs_qid=A skb_len=L proto=... dst_ip=...
  ntu=N ntc=M cpu=C ts=T` — emitted in `onic_xmit_frame` after
  `qdma_pack_h2c_st_desc` but before the doorbell.  Confirms the
  driver received the skb and built a descriptor.
- `TX_BUSY ... reason=ring_full` — TX ring full, NETDEV_TX_BUSY
  returned.
- `TX_DROP ... reason=dma_map_err` — DMA map failure path.
- `TX_DONE <netdev> qid=R abs_qid=A cidx_advance=O->N work=W
  wb_cidx=C cpu=C ts=T` — fires inside `onic_tx_clean` whenever
  `wb.cidx` advances past `next_to_clean`.  Pairing each TX_SUB
  with a TX_DONE proves the QDMA H2C engine actually consumed the
  descriptor.
- `RX_POLL_START <netdev> qid=R abs_qid=A budget=B cpu=C ts=T` — at
  entry to `onic_rx_poll`.
- `RX_PKT <netdev> qid=R abs_qid=A len=L proto=... src_ip=... cpu=C
  ts=T` — per delivered packet (XDP_PASS branch only).
- `RX_DROP ... reason=cmpl_err` — completion error bit set in the
  C2H descriptor.
- `RX_POLL_END <netdev> qid=R abs_qid=A processed=W done=D cpu=C
  ts=T` — `done=1` is the normal `napi_complete_done` exit; `done=0`
  is the budget-exhausted reschedule path.
- `RX_RING <netdev> qid=R abs_qid=A ntc=N nte=U rng=C fill=F
  cmpl_ntc=N2 cmpl_pidx=P` — desc-ring fill state and completion
  ring pointers, paired with each RX_POLL_END.

### Files touched

- `onic.h` — define `ONIC_DBG_XPATH` (=4) and the `onic_xpath()`
  macro.
- `onic_main.c` — extend `debug_level` parm description.
- `onic_netdev.c` — include `<linux/ip.h>`/`<linux/ktime.h>`; emit
  TX_SUB/TX_BUSY/TX_DROP in `onic_xmit_frame`, TX_DONE in
  `onic_tx_clean`, RX_POLL_START / RX_PKT / RX_DROP / RX_POLL_END /
  RX_RING in `onic_rx_poll`.

### Reading the trace

After running e.g. `ping -c 20 -W 1 -I enp1s0d1 10.0.0.1`,
`sudo dmesg -T | grep DBG_XPATH > /tmp/xpath.log`.

Patterns to look for:

- TX_SUB with no matching TX_DONE within ~10 ms → QDMA H2C engine
  stuck on that queue (HW/shell side, not SW).
- TX_SUB present on enp1s0d1 but RX_PKT shows the echo-reply on
  enp1s0 (or vice versa) → RSS hash collision or queue-ID
  cross-routing in the shell.
- Repeated `RX_POLL_END processed=0 done=1` on one netdev while the
  peer keeps sending → NAPI being woken without work, likely shared
  IRQ vector or spurious schedule.
- `RX_POLL_END processed=64 done=0` repeating (budget exhausted) on
  the active CMAC while the other shows zero progress → NAPI
  starvation between CMACs on the same CPU.
- `RX_RING fill=0` while peer is actively transmitting → packets
  never reach the C2H engine (CMAC RX FIFO drop, plugin RTL drop,
  or QDMA prefetch starvation).
- TX_SUB / TX_DONE / RX_POLL_START emitted from the *same* CPU for
  both CMACs → NAPI/IRQ affinity collision; try
  `irqbalance off` + manual SMP affinity to spread vectors.
