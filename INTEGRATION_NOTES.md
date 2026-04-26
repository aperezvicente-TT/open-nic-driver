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
