#!/usr/bin/env bash
#
# fc-tune-sweep.sh -- walk a set of (xoff, xon, min_xoff) flow-control settings
# and report, for each one, how much pause the port actually generated and
# whether the drops moved.
#
# RUNS ON THE HOST THAT OWNS THE FPGA NIC. One command, one table.
#
# WHY THIS EXISTS
# ---------------
# Pause generation works: 140 pause frames emitted, all 140 counted by the peer
# ConnectX-7. But the total asserted pause time was 34.5 us in 12 s -- a
# 0.0003 % duty cycle -- so the drop rate did not budge. The watermarks are far
# too conservative, and until the gateware gained CSRs each candidate value cost
# a 78-minute rebuild. Now they are runtime-writable
# (/sys/class/net/DEV/fc_*, see ../onic_sysfs.c), so the sweep that was a
# week of rebuilds is a few minutes of measurement.
#
# THE COLUMN THAT MATTERS IS xoff_duty, NOT drop %
# ------------------------------------------------
# drop % on this bench is only trustworthy under the 13.11 acceptance gate
# (flowctl-bench.sh --gate); 13.10 records the same configuration measuring
# 0.0000 % and 6.16 %. xoff_events and xoff_cycles are hardware counters of what
# the flow-control logic actually did, and they are not subject to that variance.
# Read them first:
#
#   xoff_events == 0            the watermark was never reached. Either the
#                               occupancy never gets that high (lower xoff) or
#                               generation is not enabled. Nothing else in the
#                               row means anything.
#   xoff_duty ~ 0.000x %        what tonight measured. Pause is firing but for
#                               far too little time to matter.
#   xoff_duty rising, drops falling   this is the result the sweep is looking for.
#   xoff_duty large, throughput down  overshoot: pause is now the bottleneck.
#                               Cross-check with the TCP line-rate test in 13.8.
#
# WHAT IT REUSES
# --------------
#   flowctl-bench.sh    drives the constant-rate generator on the sender over ssh
#                       and does the drop math (which in turn uses
#                       rx-drop-measure.sh). Load/topology options are passed
#                       straight through; --gate is available per triple.
#   rx-drop-measure.sh  used directly in --external-load mode, where something
#                       else generates the traffic.
# Nothing about drop measurement is reimplemented here.
#
# ==========================================================================
# UNTESTED AGAINST REAL REGISTERS
# ==========================================================================
# Written against the agreed register map before the gateware implemented it.
# The bitstream in the FPGA at the time (build stamp 0x07291754) does not decode
# these addresses. The driver detects that and returns EOPNOTSUPP; this script
# refuses to run rather than sweep settings that go nowhere. Nothing below has
# been executed against a bitstream that has the CSRs.
#
# This script writes ONLY the four fc_* attributes of the port named by --dev. It
# does not reload the driver, does not change MTU or addressing, and restores the
# settings it found on exit.
#
set -euo pipefail

PROG=${0##*/}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BENCH=$HERE/flowctl-bench.sh
MEASURE=$HERE/rx-drop-measure.sh
FC_HELPER=/usr/local/sbin/onic-fc-write

# cmac_clk, for turning cycle counts into time. Must match the gateware.
CMAC_CLK_HZ=322265625

DEV=enp194s0
ENABLE=1                # fc_enable value: 1 = generation, 3 = + reaction
TRIPLES=""
TRIPLES_FILE=""
EXTERNAL=0              # --external-load: someone else generates the traffic
EXT_SECS=10
DURATION=10
REPS=1
GATE=0
PPS=""
SETTLE=1
FMT=human
DRY=0
FORCE=0
KEEP=0
BENCH_ARGS=()

# A default ladder that walks DOWN from the gateware's own reset values, which
# are the settings that produced the 0.0003 % duty cycle:
#   FC_XOFF_WM_RST = 3072, FC_XON_WM_RST = 2048, FC_MIN_XOFF_RST = 1024
# (packet_adapter_register.v). fc_buf_fill is the adapter packet buffer's
# occupancy, ~4096 beats for a 9600-byte MTU build, so 3072 is 3/4 full: XOFF is
# requested only when the buffer is nearly gone, which is why so little pause got
# asserted. A lower XOFF fires earlier and more often. The hold time is raised in
# the last steps because a short hold releases before the peer's pause takes
# effect.
DEFAULT_TRIPLES="3072:2048:1024 2048:1024:1024 1024:512:1024 512:256:2048 256:128:4096 128:64:8192"

die()  { printf '%s: FATAL: %s\n' "$PROG" "$*" >&2; exit 2; }
warn() { printf '%s: WARN: %s\n'  "$PROG" "$*" >&2; }
note() { printf '%s: %s\n'        "$PROG" "$*" >&2; }

usage() {
    cat <<'EOF'
Usage:
  fc-tune-sweep.sh --pps N [--triples "XOFF:XON:MIN ..."] [options] [-- bench-args]
  fc-tune-sweep.sh --external-load SEC --triples "..." [options]
  fc-tune-sweep.sh --status [--dev DEV]

Modes:
  (default)          For each triple: apply it, run flowctl-bench.sh against the
                     sender, then read back the hardware flow-control counters.
                     --pps is required (it is flowctl-bench's required argument).
  --external-load S  Apply the triple, bracket S seconds with rx-drop-measure.sh
                     and let something else generate the load. Use this when the
                     traffic source is not flowctl-bench's ssh-driven generator.
  --status           Print the port's decoded fc_status and counters, then exit.
                     Reads only; writes nothing.

Sweep:
  --triples LIST     space- or comma-separated XOFF:XON:MIN triples, where
                     XOFF/XON are adapter packet-buffer occupancy in beats
                     (16-bit registers) and MIN is the minimum XOFF hold in
                     cmac_clk cycles (24-bit register). Default, walking down
                     from the gateware's reset values:
                       3072:2048:1024 2048:1024:1024 1024:512:1024
                       512:256:2048 256:128:4096 128:64:8192
  --triples-file F   read triples from F (one per line, # comments allowed)
  --enable N         fc_enable value applied before each run (default 1 =
                     generation only). 3 also enables pause REACTION, which has
                     never been exercised on hardware -- enable it on its own,
                     deliberately (shell docs Ch. 13 §13.12 risk 2).
  --keep             leave the last triple applied on exit instead of restoring
                     the settings found at start

Load (passed to flowctl-bench.sh):
  --pps N            offered packet rate. Required unless --external-load/--status.
  --duration SEC     per-rep run length (default 10)
  --reps N           repetitions per triple (default 1)
  --gate             use flowctl-bench's 13.11 acceptance gate per triple. Slow
                     (5 reps each) but the only way the drop % column is
                     believable. Strongly recommended before acting on drop %.
  --dev DEV          local port under test (default enp194s0)
  -- ...             everything after a bare -- is appended to the flowctl-bench
                     command line verbatim (e.g. -- --frame-size 1518 --gen pktgen
                     --burst 32 --peer 192.168.12.135)

Other:
  --settle SEC       idle wait after the load before the closing counter read
                     (default 1)
  --kv               machine-readable output
  --dry-run          print the plan and the exact commands; touch nothing at all
                     (no sysfs reads, no writes, no traffic)
  --force            run even if the driver says the gateware has no flow-control
                     registers. You will be measuring nothing; the only reason to
                     do this is to test the script itself.
  -h, --help         this text

Exit codes: 0 ok, 1 a triple could not be applied, 2 usage/fatal,
            3 the load or measurement failed, 4 the flow-control registers are
            absent (driver returned EOPNOTSUPP) or the driver has no fc_*
            attributes at all.

WRITE PERMISSION
  The fc_* attributes are root-writable. Either run this as root (not
  recommended: flowctl-bench sshes to the sender as the invoking user) or install
  the narrow helper:
      sudo install -o root -g root -m 0755 tools/onic-fc-write /usr/local/sbin/
      sudo install -o root -g root -m 0440 tools/fc-sudoers.example \
           /etc/sudoers.d/onic-fc
  See tools/fc-sudoers.example for why this is a helper and not `sudo tee`.
EOF
}

MODE=sweep
while [ $# -gt 0 ]; do
    case "$1" in
        --dev)            DEV=${2:?};           shift 2 ;;
        --triples)        TRIPLES=${2:?};       shift 2 ;;
        --triples-file)   TRIPLES_FILE=${2:?};  shift 2 ;;
        --enable)         ENABLE=${2:?};        shift 2 ;;
        --pps)            PPS=${2:?};           shift 2 ;;
        --duration)       DURATION=${2:?};      shift 2 ;;
        --reps)           REPS=${2:?};          shift 2 ;;
        --gate)           GATE=1;               shift ;;
        --external-load)  EXTERNAL=1; EXT_SECS=${2:?}; shift 2 ;;
        --status)         MODE=status;          shift ;;
        --settle)         SETTLE=${2:?};        shift 2 ;;
        --keep)           KEEP=1;               shift ;;
        --kv)             FMT=kv;               shift ;;
        --dry-run)        DRY=1;                shift ;;
        --force)          FORCE=1;              shift ;;
        -h|--help)        usage; exit 0 ;;
        --)               shift; BENCH_ARGS=( "$@" ); break ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

SYS=/sys/class/net/$DEV
FC_RW_ATTRS="fc_enable fc_xoff_watermark fc_xon_watermark fc_min_xoff_cycles"

# ---------------------------------------------------------------- reading -----
# Read-only fc_* attributes are world-readable, so no helper is needed. A read
# that fails is meaningful, not noise: EOPNOTSUPP is the driver saying this
# bitstream does not implement the register.
fc_read() {
    local attr=$1 val
    if ! val=$(cat "$SYS/$attr" 2>/dev/null); then
        printf 'ERR'
        return 1
    fi
    printf '%s' "$val"
}

sysfs_stat() { cat "$SYS/statistics/$1"; }

# ---------------------------------------------------------------- writing -----
WRITER=""
resolve_writer() {
    if [ "$(id -u)" = 0 ]; then
        WRITER=direct
    elif [ -x "$FC_HELPER" ] && sudo -n "$FC_HELPER" --help >/dev/null 2>&1; then
        WRITER=helper
    else
        cat >&2 <<EOF
$PROG: FATAL: cannot write $SYS/fc_* as $(id -un).

  These attributes are root-writable, and this bench's passwordless sudo set
  deliberately excludes sh/tee/dd (see tools/install-bringup-sudoers.sh), so
  there is no generic way to poke sysfs.

  Install the narrow helper once, by a human with the sudo password:
      sudo install -o root -g root -m 0755 $HERE/onic-fc-write $FC_HELPER
      sudo install -o root -g root -m 0440 $HERE/fc-sudoers.example \\
           /etc/sudoers.d/onic-fc
      sudo visudo -cf /etc/sudoers.d/onic-fc

  Or run this script under sudo -- but note that flowctl-bench.sh then sshes to
  the sender as root, which is normally not what you want.
EOF
        exit 2
    fi
}

fc_write() {
    local attr=$1 val=$2
    case "$WRITER" in
        direct) printf '%s\n' "$val" > "$SYS/$attr" 2>/dev/null ;;
        helper) sudo -n "$FC_HELPER" "$DEV" "$attr" "$val" >/dev/null ;;
        *)      return 1 ;;
    esac
}

# fc_set ATTR VALUE -- write and prove it took. The driver already verifies its
# own register read-back and fails the write on a mismatch, so this is the second
# line of defence, not the first.
fc_set() {
    local attr=$1 val=$2 back
    if ! fc_write "$attr" "$val"; then
        warn "$attr = $val was rejected (see 'dmesg | tail' for the driver's reason)"
        return 1
    fi
    back=$(fc_read "$attr") || { warn "$attr became unreadable after writing it"; return 1; }
    if [ "$back" != "$val" ]; then
        warn "$attr reads back $back after writing $val"
        return 1
    fi
    return 0
}

# apply_triple XOFF XON MIN
#
# Order is not arbitrary. The driver enforces xon < xoff on every write, so a
# naive "write xoff then xon" fails whenever the new pair is entirely below the
# old one. Dropping xon to 0 first makes any subsequent pair writable:
#   xon=0        always valid (0 is explicitly allowed: drain fully before XON)
#   xoff=XOFF    valid because XOFF > 0 = xon
#   xon=XON      valid because XON < XOFF
apply_triple() {
    local xoff=$1 xon=$2 min=$3
    fc_set fc_xon_watermark 0        || return 1
    fc_set fc_xoff_watermark "$xoff" || return 1
    fc_set fc_xon_watermark "$xon"   || return 1
    fc_set fc_min_xoff_cycles "$min" || return 1
    # Enable last: the driver refuses to arm generation on an invalid pair, so
    # arming after the watermarks are in place is the order that always works.
    fc_set fc_enable "$ENABLE"       || return 1
    return 0
}

# 32-bit wrap-aware delta. FC_XOFF_EVENTS and FC_XOFF_CYCLES are free-running
# 32-bit counters that never reset (the same trap as the plugin adap_in counter
# handled in the driver for Ch. 13 §13.2).
d32() { printf '%s' "$(( ($2 - $1 + 4294967296) % 4294967296 ))"; }

# ------------------------------------------------------------- preflight -------
preflight() {
    [ -d "$SYS" ] || die "no such netdev: $DEV"
    [ -x "$BENCH" ] || die "$BENCH is missing or not executable"
    [ -x "$MEASURE" ] || die "$MEASURE is missing or not executable"

    local missing="" a
    for a in $FC_RW_ATTRS fc_status fc_xoff_events fc_xoff_cycles; do
        [ -e "$SYS/$a" ] || missing="$missing $a"
    done
    if [ -n "$missing" ]; then
        cat >&2 <<EOF
$PROG: FATAL: $DEV has no flow-control sysfs attributes (missing:$missing).

  The loaded onic driver predates them. Rebuild and reload:
      make -j32 && sudo rmmod onic && sudo insmod ./onic.ko
  (Do not do that while a bitstream is under test by someone else.)
EOF
        exit 4
    fi

    # The decisive check: the driver returns EOPNOTSUPP when the adapter register
    # window answers 0xDEADBEEF, which is what a bitstream without these CSRs
    # does. Sweeping in that state would produce a table of zeros that looks like
    # a result.
    if ! cat "$SYS/fc_status" >/dev/null 2>&1; then
        cat >&2 <<EOF
$PROG: the driver cannot read $DEV's flow-control registers
  (reading fc_status failed -- the driver returns EOPNOTSUPP when the gateware's
  adapter register window answers 0xDEADBEEF for these offsets, i.e. this
  bitstream does not implement them).

  Nothing to sweep: writes would be discarded and every counter would read as
  garbage. Load a bitstream with the Ch. 13 §13.4 flow-control CSRs first.
EOF
        [ "$FORCE" = 1 ] || exit 4
        warn "--force given: continuing against a bitstream with no flow-control registers. The numbers below are meaningless."
    fi
}

# --------------------------------------------------------------- status --------
if [ "$MODE" = status ]; then
    [ -d "$SYS" ] || die "no such netdev: $DEV"
    [ -e "$SYS/fc_status" ] || die "$DEV has no fc_status attribute (driver too old)"
    if ! cat "$SYS/fc_status"; then
        die "the driver returned an error reading fc_status -- this bitstream most
       likely does not implement the flow-control registers"
    fi
    printf 'rx_missed_errors: %s\n' "$(sysfs_stat rx_missed_errors)"
    printf 'rx_packets:       %s\n' "$(sysfs_stat rx_packets)"
    exit 0
fi

# ----------------------------------------------------------- triple list ------
if [ -n "$TRIPLES_FILE" ]; then
    [ -r "$TRIPLES_FILE" ] || die "cannot read --triples-file $TRIPLES_FILE"
    TRIPLES="$TRIPLES $(sed -e 's/#.*//' "$TRIPLES_FILE")"
fi
[ -n "${TRIPLES// /}" ] || TRIPLES=$DEFAULT_TRIPLES
TRIPLES=${TRIPLES//,/ }

LIST=()
for t in $TRIPLES; do
    case "$t" in
        *:*:*) : ;;
        *) die "triple '$t' is not XOFF:XON:MIN" ;;
    esac
    x=${t%%:*}; rest=${t#*:}; n=${rest%%:*}; m=${rest#*:}
    for v in "$x" "$n" "$m"; do
        case "$v" in ''|*[!0-9]*) die "triple '$t' has a non-numeric field";; esac
    done
    [ "$x" -gt 0 ]  || die "triple '$t': xoff must be > 0 (0 would pause for ever)"
    [ "$n" -lt "$x" ] || die "triple '$t': xon ($n) must be strictly below xoff ($x)"
    # Same bounds the driver enforces, checked here so a typo fails before any
    # register is touched rather than half-way through the sweep.
    [ "$x" -le 65535 ] || die "triple '$t': xoff $x exceeds 65535 (FC_STATUS reports occupancy in 16 bits)"
    [ "$m" -le 16777215 ] || die "triple '$t': min_xoff $m exceeds the register's 24 bits (16777215 cycles, ~52 ms); the gateware would saturate it and the driver refuses it"
    LIST+=( "$x:$n:$m" )
done

case "$ENABLE" in 0|1|2|3) : ;; *) die "--enable must be 0..3 (bit0 generation, bit1 reaction)";; esac
[ "$ENABLE" != 0 ] || warn "--enable 0 disables pause generation; every triple will show xoff_events=0"
if [ "$ENABLE" = 2 ] || [ "$ENABLE" = 3 ]; then
    warn "--enable $ENABLE turns on pause REACTION, a path never exercised on hardware (§13.12 risk 2). Test it on its own."
fi

if [ "$EXTERNAL" = 0 ]; then
    [ -n "$PPS" ] || die "--pps is required (flowctl-bench.sh needs it); or use --external-load SEC"
fi

# --------------------------------------------------------------- dry run -------
bench_cmd() {
    printf '%s' "$BENCH --dev $DEV --pps $PPS --duration $DURATION --reps $REPS --kv"
    [ "$GATE" = 1 ] && printf ' --gate'
    [ ${#BENCH_ARGS[@]} -gt 0 ] && printf ' %s' "${BENCH_ARGS[*]}"
    printf '\n'
}

if [ "$DRY" = 1 ]; then
    cat <<EOF
# --dry-run: nothing is read or written
device            : $DEV   (attributes $SYS/fc_*)
triples           : ${LIST[*]}
fc_enable         : $ENABLE
load              : $( [ "$EXTERNAL" = 1 ] && printf 'EXTERNAL, %s s per triple' "$EXT_SECS" || printf '%s pps, %s s x %s rep(s)%s' "$PPS" "$DURATION" "$REPS" "$( [ "$GATE" = 1 ] && printf ' (acceptance gate)' )" )
restore on exit   : $( [ "$KEEP" = 1 ] && printf 'no (--keep)' || printf 'yes' )

per triple, in order:
  onic-fc-write $DEV fc_xon_watermark 0        # makes any new pair writable
  onic-fc-write $DEV fc_xoff_watermark XOFF
  onic-fc-write $DEV fc_xon_watermark XON
  onic-fc-write $DEV fc_min_xoff_cycles MIN
  onic-fc-write $DEV fc_enable $ENABLE
  read  $SYS/fc_xoff_events $SYS/fc_xoff_cycles $SYS/statistics/rx_missed_errors
EOF
    if [ "$EXTERNAL" = 1 ]; then
        printf '  %s --dev %s --snapshot   (bracket %s s of externally driven load)\n' \
            "$MEASURE" "$DEV" "$EXT_SECS"
    else
        printf '  %s\n' "$(bench_cmd)"
    fi
    printf '  read the same three counters again; report wrap-aware deltas\n'
    exit 0
fi

preflight
resolve_writer

# --------------------------------------------------------------- restore -------
ORIG=""
save_original() {
    local a v out=""
    for a in $FC_RW_ATTRS; do
        v=$(fc_read "$a") || v=ERR
        out="$out $a=$v"
    done
    ORIG=${out# }
}
restore_original() {
    [ -n "$ORIG" ] || return 0
    [ "$KEEP" = 0 ] || { note "--keep: leaving the last triple applied"; return 0; }
    local kv a v
    # Reverse order of apply_triple: disable first, drop xon, then the rest, so
    # no intermediate state trips the driver's xon < xoff rule.
    fc_write fc_enable 0 || true
    fc_write fc_xon_watermark 0 || true
    for kv in $ORIG; do
        a=${kv%%=*}; v=${kv#*=}
        [ "$v" = ERR ] && continue
        [ "$a" = fc_enable ] && continue
        fc_write "$a" "$v" || warn "could not restore $a=$v"
    done
    for kv in $ORIG; do
        a=${kv%%=*}; v=${kv#*=}
        [ "$a" = fc_enable ] || continue
        [ "$v" = ERR ] && continue
        fc_write "$a" "$v" || warn "could not restore $a=$v"
    done
    note "restored: $ORIG"
}
trap restore_original EXIT

save_original
[ "$FMT" = kv ] || printf '=== fc-tune-sweep: %s ===\noriginal settings: %s\n\n' "$DEV" "$ORIG"

# kv_get <kv-line> <key> [default] -- same helper flowctl-bench.sh uses, so the
# two scripts parse each other's output the same way.
kv_get() {
    local line=$1 key=$2 def=${3:-na} tok
    for tok in $line; do
        if [ "${tok%%=*}" = "$key" ]; then printf '%s' "${tok#*=}"; return 0; fi
    done
    printf '%s' "$def"
}

# ------------------------------------------------------------- one triple ------
# Sets: T_DROPPCT T_DROPPKT T_OFFERED T_EVENTS T_CYCLES T_MISSED T_SECS T_RC
run_triple() {
    local xoff=$1 xon=$2 min=$3
    local ev_a ev_b cy_a cy_b mi_a mi_b out snap_a snap_b

    T_RC=0; T_DROPPCT=na; T_DROPPKT=na; T_OFFERED=na

    # Read the flow-control counters and rx_missed_errors from sysfs only. Do NOT
    # call ethtool here: the CMAC stat_* counters are clear-on-read, and an extra
    # `ethtool -S` inside flowctl-bench's own bracket would steal part of its
    # interval (rx-drop-measure.sh documents this at length).
    ev_a=$(fc_read fc_xoff_events) || { T_RC=4; return 0; }
    cy_a=$(fc_read fc_xoff_cycles) || { T_RC=4; return 0; }
    mi_a=$(sysfs_stat rx_missed_errors)

    if [ "$EXTERNAL" = 1 ]; then
        snap_a=$("$MEASURE" --dev "$DEV" --snapshot)
        note "generate load on $DEV now -- waiting $EXT_SECS s (triple $xoff:$xon:$min)"
        sleep "$EXT_SECS"
        snap_b=$("$MEASURE" --dev "$DEV" --snapshot)
        out=$("$MEASURE" --diff "$snap_a" "$snap_b" --kv 2>&1) || true
        if ! printf '%s' "$out" | grep -q 'offered_pkts='; then
            printf '%s: triple %s: measurement failed:\n%s\n' "$PROG" "$xoff:$xon:$min" "$out" >&2
            T_RC=3
        else
            T_DROPPCT=$(kv_get "$out" drop_pct)
            T_DROPPKT=$(kv_get "$out" dropped_pkts)
            T_OFFERED=$(kv_get "$out" offered_pkts)
        fi
    else
        set +e
        out=$(eval "$(bench_cmd)" 2>&1)
        local rc=$?
        set -e
        # The summary line carries the median, which is the statistic 13.11
        # asks for; per-rep lines are echoed on failure only.
        local sum
        sum=$(printf '%s\n' "$out" | grep -E '^summary ' | tail -1 || true)
        if [ -z "$sum" ]; then
            printf '%s: triple %s: flowctl-bench produced no summary (rc=%s):\n%s\n' \
                "$PROG" "$xoff:$xon:$min" "$rc" "$out" >&2
            T_RC=3
        else
            T_DROPPCT=$(kv_get "$sum" median)
            T_OFFERED=$(kv_get "$sum" omin)
            T_DROPPKT=$(kv_get "$sum" pmax)
            if [ "$rc" != 0 ]; then
                warn "triple $xoff:$xon:$min: flowctl-bench exited $rc (invalid rep, bad cross-check, or gate FAIL) -- the drop % column for this row is not trustworthy"
                T_RC=1
            fi
        fi
    fi

    sleep "$SETTLE"
    ev_b=$(fc_read fc_xoff_events) || { T_RC=4; return 0; }
    cy_b=$(fc_read fc_xoff_cycles) || { T_RC=4; return 0; }
    mi_b=$(sysfs_stat rx_missed_errors)

    T_EVENTS=$(d32 "$ev_a" "$ev_b")
    T_CYCLES=$(d32 "$cy_a" "$cy_b")
    T_MISSED=$(( mi_b - mi_a ))
    if [ "$T_MISSED" -lt 0 ]; then
        # rx_missed_errors is a 64-bit netdev counter that only resets on driver
        # load. Going backwards means the driver was reloaded mid-sweep, which
        # also resets the baseline this triple was measured against.
        warn "triple $xoff:$xon:$min: rx_missed_errors went BACKWARDS ($mi_a -> $mi_b); the driver was reloaded mid-sweep and this row is void"
        T_RC=3
    fi
    # Denominator for the duty cycle: the time the generator was actually
    # sending, not this wall-clock bracket. With --reps/--gap the bracket
    # includes idle gaps and would understate the duty cycle.
    if [ "$EXTERNAL" = 1 ]; then
        T_SECS=$EXT_SECS
    else
        T_SECS=$(awk -v d="$DURATION" -v r="$REPS" -v g="$GATE" \
                     'BEGIN{ reps = (g == 1 && r < 5 ? 5 : r); printf "%.3f", d * reps }')
    fi
    return 0
}

# ------------------------------------------------------------------ sweep ------
if [ "$FMT" = human ]; then
    # rx_missed is this script's own sysfs delta over the whole triple (all reps);
    # drop_% is flowctl-bench's median over the reps. They answer different
    # questions, so both are shown rather than one being derived from the other.
    printf '%9s %6s %9s %12s %12s %11s %10s %12s %10s\n' \
        xoff xon min_xoff xoff_events xoff_cycles xoff_us 'xoff_duty%' rx_missed 'drop_%'
fi

ROWS=()
FAILED=0
for t in "${LIST[@]}"; do
    xoff=${t%%:*}; rest=${t#*:}; xon=${rest%%:*}; min=${rest#*:}

    if ! apply_triple "$xoff" "$xon" "$min"; then
        warn "could not apply $xoff:$xon:$min -- skipping it (nothing was measured with it)"
        FAILED=1
        continue
    fi

    T_EVENTS=0; T_CYCLES=0; T_MISSED=0; T_SECS=1
    run_triple "$xoff" "$xon" "$min"
    if [ "$T_RC" = 4 ]; then
        die "the flow-control counters became unreadable mid-sweep (driver returned an error). Aborting rather than reporting partial numbers."
    fi
    [ "$T_RC" = 0 ] || FAILED=1

    XUS=$(awk -v c="$T_CYCLES" -v f="$CMAC_CLK_HZ" 'BEGIN{ printf "%.1f", c * 1e6 / f }')
    XDUTY=$(awk -v c="$T_CYCLES" -v s="$T_SECS" -v f="$CMAC_CLK_HZ" \
                'BEGIN{ printf "%.6f", (s > 0 ? 100.0 * c / (s * f) : 0) }')

    if [ "$FMT" = human ]; then
        printf '%9s %6s %9s %12s %12s %11s %10s %12s %10s\n' \
            "$xoff" "$xon" "$min" "$T_EVENTS" "$T_CYCLES" "$XUS" "$XDUTY" \
            "$T_MISSED" "$T_DROPPCT"
    else
        printf 'xoff=%s xon=%s min_xoff=%s xoff_events=%s xoff_cycles=%s xoff_us=%s xoff_duty_pct=%s rx_missed_delta=%s bench_dropped_pkts=%s bench_drop_pct=%s offered_pkts=%s traffic_s=%s rc=%s\n' \
            "$xoff" "$xon" "$min" "$T_EVENTS" "$T_CYCLES" "$XUS" "$XDUTY" \
            "$T_MISSED" "$T_DROPPKT" "$T_DROPPCT" "$T_OFFERED" "$T_SECS" "$T_RC"
    fi
    ROWS+=( "$xoff:$xon:$min|$T_EVENTS|$XDUTY|$T_MISSED|$T_DROPPCT" )
done

[ ${#ROWS[@]} -gt 0 ] || die "no triple was successfully measured"

# ---------------------------------------------------------------- verdict ------
if [ "$FMT" = human ]; then
    printf '\n--- interpretation ---\n'

    allzero=1
    for r in "${ROWS[@]}"; do
        IFS='|' read -r _t ev _d _m _p <<< "$r"
        [ "$ev" = 0 ] || allzero=0
    done
    if [ "$allzero" = 1 ]; then
        cat <<EOF
  xoff_events is 0 for EVERY triple: the XOFF watermark was never reached, so no
  pause was generated and no row below it means anything.

  In order of likelihood:
    1. every xoff is still above the occupancy this buffer ever reaches. Read
       'occupancy' from --status DURING traffic to find the real high-water mark,
       then set xoff below it.
    2. fc_enable did not stick (--enable was $ENABLE; check 'dmesg | tail').
    3. the offered load is not enough to congest the receive path at all.
EOF
    else
        best=$(printf '%s\n' "${ROWS[@]}" | awk -F'|' '$5 != "na" { print $5, $0 }' \
                | sort -g | head -1 | cut -d' ' -f2- || true)
        if [ -n "$best" ]; then
            IFS='|' read -r bt bev bduty bmiss bpct <<< "$best"
            printf '  lowest drop %%: %s  ->  drop %s %%, rx_missed delta %s, xoff duty %s %% (%s assertions)\n' \
                "$bt" "$bpct" "$bmiss" "$bduty" "$bev"
        fi
        printf '  compare against the baseline that motivated this sweep: 140 XOFF\n'
        printf '  assertions and 34.5 us of pause in 12 s = 0.000287 %% duty, drops unchanged.\n'
    fi

    if [ "$GATE" != 1 ] && [ "$EXTERNAL" = 0 ]; then
        cat <<'EOF'

  NOTE: the drop % column was measured WITHOUT the 13.11 acceptance gate. Ch. 13
  §13.10 records this instrument reading 0.0000 % and 6.16 % for identical
  settings, so do not rank triples by drop % from a single rep. Re-run the
  shortlist with --gate (5 reps each) before believing it. xoff_events,
  xoff_cycles and the duty cycle are hardware counters and are not affected.
EOF
    fi
fi

[ "$FAILED" = 0 ] || exit 1
exit 0
