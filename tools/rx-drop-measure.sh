#!/usr/bin/env bash
#
# rx-drop-measure.sh -- receiver-side RX loss measurement for the onic datapath.
#
# Runs on the HOST THAT OWNS THE FPGA NIC (homelab-1). Samples the per-port
# counters and reports delivered pps, drop pps and drop %.
#
# Counters used (all readable without root except the ethtool set, which needs
# only the passwordless `sudo -n ethtool -S` rule that already exists):
#
#   /sys/class/net/DEV/statistics/rx_packets        delivered to the stack
#   /sys/class/net/DEV/statistics/rx_missed_errors  C2H descriptor-response drops
#                                                   (driver maps QDMA
#                                                    C2H_STAT_DESC_RSP_DROP here)
#   ethtool -S: stat_rx_total_good_pkts   packets accepted at the CMAC (the WIRE
#                                         count -- an independent measure of what
#                                         was actually offered)
#               stat_adapt_rx_drop        drops in the packet adapter
#               qdma_c2h_desc_rsp_drop    the raw hardware drop counter
#               qdma_c2h_accepted
#
# Having the wire count lets this script CHECK ITSELF: offered-at-wire must equal
# delivered + all drop counters. If it does not, the numbers are not trustworthy
# and the script says so (and with --strict, exits non-zero) rather than printing
# a plausible-looking drop percentage.
#
# ==========================================================================
# COUNTER SEMANTICS -- THE CMAC stat_* COUNTERS ARE CLEAR-ON-READ
# ==========================================================================
# Measured on this driver (2024258), with no traffic flowing:
#
#     $ sudo ethtool -S enp194s0 | grep stat_rx_total_good_pkts
#          stat_rx_total_good_pkts: 1      <- packets since the PREVIOUS read
#     $ sudo ethtool -S enp194s0 | grep stat_rx_total_good_pkts
#          stat_rx_total_good_pkts: 0      <- and now it is zero
#
# Every ONIC_STATS entry backed by a CMAC statistics register -- all stat_tx_* and
# stat_rx_* names -- is an INTERVAL counter that the hardware latches and clears on
# each read. They are NOT cumulative totals. Two consequences:
#
#   1. The interval wire count is the value read by the CLOSING snapshot, NOT
#      (closing - opening). Subtracting them is wrong and can go negative, which is
#      how this was found.
#   2. The opening snapshot's read is what ZEROES the counter. Any other
#      `ethtool -S` on this device between the two snapshots steals part of the
#      interval. Do not run one, and do not run two measurements concurrently.
#      This script detects the resulting shortfall and reports it.
#
# By contrast stat_adapt_* and every qdma_* counter ARE cumulative, so those are
# differenced normally. Anyone treating stat_rx_total_good_pkts as a running total
# (for example to compute an offered rate over a long capture) is reading noise.
#
# This script never writes to the device, never changes MTU/IP, and never touches
# the driver's module parameters.
#
set -euo pipefail

PROG=${0##*/}

DEV=""
DURATION=""
INTERVAL=0
MODE="measure"          # measure | snapshot | diff
FMT="human"             # human | kv
STRICT=0
USE_ETHTOOL=1
WINDOW_OVERRIDE=""      # seconds; use an externally measured window for pps math
DIFF_A=""
DIFF_B=""

die() { printf '%s: FATAL: %s\n' "$PROG" "$*" >&2; exit 2; }
warn() { printf '%s: WARN: %s\n' "$PROG" "$*" >&2; }

usage() {
    cat <<'EOF'
Usage:
  rx-drop-measure.sh --dev DEV [--duration SEC] [--interval SEC] [--kv] [--strict]
  rx-drop-measure.sh --dev DEV --snapshot [--no-ethtool]
  rx-drop-measure.sh --diff SNAP_A SNAP_B [--window SEC] [--kv] [--strict]

Modes:
  (default)   Take a snapshot, wait --duration, take another, report the delta.
              With --interval N also print a row every N seconds.
  --snapshot  Print ONE machine-readable counter sample line and exit. Use this
              to bracket an externally driven traffic run.
  --diff      Compute and report the delta between two snapshot lines produced by
              --snapshot. This is how flowctl-bench.sh reuses the same math.

Options:
  --dev DEV         netdev to measure (e.g. enp194s0). Required except with --diff.
  --duration SEC    measurement window (default 10, accepts fractions)
  --interval SEC    also emit per-interval rows (0 = totals only, default)
  --window SEC      with --diff: use this as the rate denominator instead of the
                    timestamp delta between the two snapshots. Use the GENERATOR's
                    own reported run duration -- it is a far better denominator
                    than a wall clock that includes ssh round-trips.
  --kv              machine-readable KEY=VALUE output instead of a table
  --strict          exit non-zero if the counter cross-check fails
  --no-ethtool      skip ethtool counters (sysfs only; disables the cross-check)
  -h, --help        this text

Exit codes: 0 ok, 1 cross-check failed under --strict, 2 usage/fatal error.

Metric definitions:
  offered   = delivered + dropped              (what the datapath was asked to take)
  wire      = stat_rx_total_good_pkts read at the CLOSING snapshot -- that counter
              is CLEAR-ON-READ, so its value already is the interval count
  drop %    = dropped / offered * 100
  *_pps     = count / window
Drop % is computed from PACKET COUNTS, not from rates, so it is immune to any
error in the window measurement.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)        DEV=${2:?--dev needs a value}; shift 2 ;;
        --duration)   DURATION=${2:?}; shift 2 ;;
        --interval)   INTERVAL=${2:?}; shift 2 ;;
        --window)     WINDOW_OVERRIDE=${2:?}; shift 2 ;;
        --snapshot)   MODE="snapshot"; shift ;;
        --diff)       MODE="diff"; DIFF_A=${2:?--diff needs two snapshot lines}
                      DIFF_B=${3:?--diff needs two snapshot lines}; shift 3 ;;
        --kv)         FMT="kv"; shift ;;
        --strict)     STRICT=1; shift ;;
        --no-ethtool) USE_ETHTOOL=0; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "unknown argument '$1' (try --help)" ;;
    esac
done

# ---------------------------------------------------------------- sampling ----

now_ns() {
    # bash 5 EPOCHREALTIME is "sec.usec"; convert to integer ns without forking.
    local t=${EPOCHREALTIME:-}
    [ -n "$t" ] || die "bash 5 EPOCHREALTIME unavailable; cannot timestamp reliably"
    local s=${t%%[.,]*} us=${t#*[.,]}
    us=${us}000000; us=${us:0:6}
    printf '%s%s000\n' "$s" "$us"
}

sysfs_get() {
    local f=/sys/class/net/$1/statistics/$2
    [ -r "$f" ] || die "cannot read $f"
    local v; v=$(< "$f")
    case "$v" in ''|*[!0-9]*) die "counter $f has non-numeric value '$v'" ;; esac
    printf '%s\n' "$v"
}

# Populate globals ET_* from one ethtool -S invocation (cheap: one fork).
ET_WIRE=na; ET_ADAPT=na; ET_C2HDROP=na; ET_C2HACC=na
ethtool_sample() {
    local dev=$1 out=""
    ET_WIRE=na; ET_ADAPT=na; ET_C2HDROP=na; ET_C2HACC=na
    [ "$USE_ETHTOOL" = 1 ] || return 0
    if ! out=$(sudo -n ethtool -S "$dev" 2>/dev/null); then
        warn "sudo -n ethtool -S $dev failed; continuing without the wire cross-check"
        USE_ETHTOOL=0
        return 0
    fi
    local k v
    while read -r k v; do
        case "$k" in
            stat_rx_total_good_pkts:) ET_WIRE=$v ;;
            stat_adapt_rx_drop:)      ET_ADAPT=$v ;;
            qdma_c2h_desc_rsp_drop:)  ET_C2HDROP=$v ;;
            qdma_c2h_accepted:)       ET_C2HACC=$v ;;
        esac
    done <<< "$out"
    if [ "$ET_WIRE" = na ]; then
        warn "stat_rx_total_good_pkts not present in ethtool output; cross-check disabled"
        USE_ETHTOOL=0
    fi
}

snapshot() {
    local dev=$1 ts rxp rxm rxd rxe
    # Order matters: read the cheap sysfs counters tightly around the timestamp,
    # then ethtool (which forks and takes ~ms). The ethtool counters are only used
    # for the consistency cross-check, not for the primary rate math.
    ts=$(now_ns)
    rxp=$(sysfs_get "$dev" rx_packets)
    rxm=$(sysfs_get "$dev" rx_missed_errors)
    rxd=$(sysfs_get "$dev" rx_dropped)
    rxe=$(sysfs_get "$dev" rx_errors)
    ethtool_sample "$dev"
    printf 'ts_ns=%s dev=%s rx_packets=%s rx_missed=%s rx_dropped=%s rx_errors=%s wire_pkts=%s adapt_rx_drop=%s c2h_drop=%s c2h_accepted=%s\n' \
        "$ts" "$dev" "$rxp" "$rxm" "$rxd" "$rxe" "$ET_WIRE" "$ET_ADAPT" "$ET_C2HDROP" "$ET_C2HACC"
}

# ------------------------------------------------------------------ report ----

# report <snap_a> <snap_b>  -- prints the delta report; sets global RC
RC=0
report() {
    local a=$1 b=$2
    local A_ts A_rxp A_rxm A_rxd A_rxe A_wire A_adapt A_c2h A_acc A_dev
    local B_ts B_rxp B_rxm B_rxd B_rxe B_wire B_adapt B_c2h B_acc B_dev
    local kv k v
    for kv in $a; do k=${kv%%=*}; v=${kv#*=}
        case $k in ts_ns) A_ts=$v;; dev) A_dev=$v;; rx_packets) A_rxp=$v;;
            rx_missed) A_rxm=$v;; rx_dropped) A_rxd=$v;; rx_errors) A_rxe=$v;;
            wire_pkts) A_wire=$v;; adapt_rx_drop) A_adapt=$v;; c2h_drop) A_c2h=$v;;
            c2h_accepted) A_acc=$v;; esac
    done
    for kv in $b; do k=${kv%%=*}; v=${kv#*=}
        case $k in ts_ns) B_ts=$v;; dev) B_dev=$v;; rx_packets) B_rxp=$v;;
            rx_missed) B_rxm=$v;; rx_dropped) B_rxd=$v;; rx_errors) B_rxe=$v;;
            wire_pkts) B_wire=$v;; adapt_rx_drop) B_adapt=$v;; c2h_drop) B_c2h=$v;;
            c2h_accepted) B_acc=$v;; esac
    done
    [ -n "${A_rxp:-}" ] && [ -n "${B_rxp:-}" ] || die "malformed snapshot line(s)"
    [ "${A_dev:-x}" = "${B_dev:-y}" ] || die "snapshots are from different devices ($A_dev vs $B_dev)"

    set +e   # we want awk's exit status, not a set -e abort
    awk -v a_ts="$A_ts" -v b_ts="$B_ts" \
        -v a_rxp="$A_rxp" -v b_rxp="$B_rxp" \
        -v a_rxm="$A_rxm" -v b_rxm="$B_rxm" \
        -v a_rxd="$A_rxd" -v b_rxd="$B_rxd" \
        -v a_rxe="$A_rxe" -v b_rxe="$B_rxe" \
        -v a_w="$A_wire"  -v b_w="$B_wire" \
        -v a_ad="$A_adapt" -v b_ad="$B_adapt" \
        -v a_c="$A_c2h"   -v b_c="$B_c2h" \
        -v dev="$A_dev" -v fmt="$FMT" -v winov="$WINDOW_OVERRIDE" '
    function neg(n, what) { if (n < 0) {
        printf("rx-drop-measure.sh: FATAL: %s went BACKWARDS by %d -- counters were reset (driver reload?) mid-measurement; refusing to report\n", what, -n) > "/dev/stderr"; exit 2 } }
    BEGIN {
        win = (b_ts - a_ts) / 1e9
        d_rxp = b_rxp - a_rxp; neg(d_rxp, "rx_packets")
        d_rxm = b_rxm - a_rxm; neg(d_rxm, "rx_missed_errors")
        d_rxd = b_rxd - a_rxd; neg(d_rxd, "rx_dropped")
        d_rxe = b_rxe - a_rxe; neg(d_rxe, "rx_errors")

        have_et = (a_w != "na" && b_w != "na")
        if (have_et) {
            # CLEAR-ON-READ: the closing read already IS the interval count. The
            # opening read only served to zero the register.
            d_w  = b_w + 0
            d_ad = b_ad - a_ad; neg(d_ad, "stat_adapt_rx_drop")
            d_c  = b_c  - a_c;  neg(d_c,  "qdma_c2h_desc_rsp_drop")
        }

        offered = d_rxp + d_rxm
        if (offered == 0) {
            print "rx-drop-measure.sh: FATAL: no packets observed on " dev " during the window (rx_packets and rx_missed_errors both unchanged). The generator did not run, or it targeted a different port." > "/dev/stderr"
            exit 2
        }
        rate_win = (winov != "" ? winov+0 : win)
        if (rate_win <= 0) {
            print "rx-drop-measure.sh: FATAL: non-positive measurement window" > "/dev/stderr"; exit 2
        }

        drop_pct  = 100.0 * d_rxm / offered
        del_pps   = d_rxp / rate_win
        drop_pps  = d_rxm / rate_win
        off_pps   = offered / rate_win

        # ---- self-check: the wire count must account for delivered + all drops.
        check = "SKIPPED(no-ethtool)"
        bad = 0
        if (have_et) {
            accounted = d_rxp + d_rxm + d_ad
            resid = d_w - accounted
            tol = 8 + 0.0005 * accounted  # a few packets, or 0.05%, whichever larger
            if (resid < -tol) {
                check = sprintf("FAILED: wire count %d is SHORT of delivered+missed+adapt_drop=%d by %d (tol %.0f). The CMAC stat_rx_* counters are clear-on-read, so something else read them inside the measurement window -- a concurrent ethtool -S on %s, another instance of this script, or a monitoring daemon. The wire cross-check is void for this run.", d_w, accounted, -resid, tol, dev)
                bad = 1
            } else if (resid > tol) {
                check = sprintf("FAILED: wire count %d EXCEEDS delivered+missed+adapt_drop=%d by %d (tol %.0f) -- %d packets arrived at the MAC and were accounted for nowhere. Either traffic reached the port outside the intended stream, or there is a drop point with no counter.", d_w, accounted, resid, tol, resid)
                bad = 1
            } else {
                check = sprintf("OK (wire=%d, unexplained=%d)", d_w, resid)
            }
            # rx_missed_errors is supposed to mirror the hardware counter. Allow a
            # few packets of skew: the driver accumulates the hardware counter from
            # NAPI context, and this script reads sysfs and ethtool a couple of
            # milliseconds apart, so a small difference is sampling skew, not a bug.
            ctol = 4 + 0.00002 * d_w
            if (d_c - d_rxm > ctol || d_rxm - d_c > ctol) {
                check = check sprintf("; MISMATCH rx_missed_errors=%d vs qdma_c2h_desc_rsp_drop=%d (tol %.0f)", d_rxm, d_c, ctol)
                bad = 1
            }
        }

        if (fmt == "kv") {
            printf("dev=%s window_s=%.6f wall_s=%.6f offered_pkts=%d delivered_pkts=%d dropped_pkts=%d", dev, rate_win, win, offered, d_rxp, d_rxm)
            printf(" drop_pct=%.6f offered_pps=%.1f delivered_pps=%.1f drop_pps=%.1f", drop_pct, off_pps, del_pps, drop_pps)
            printf(" rx_dropped=%d rx_errors=%d", d_rxd, d_rxe)
            if (have_et) printf(" wire_pkts=%d adapt_rx_drop=%d c2h_drop=%d", d_w, d_ad, d_c)
            printf(" crosscheck=%s\n", (bad ? "BAD" : (have_et ? "OK" : "SKIPPED")))
        } else {
            printf("  device                 : %s\n", dev)
            printf("  window (rate denom)    : %.4f s%s\n", rate_win, (winov != "" ? "  [generator-reported]" : "  [counter wall clock]"))
            if (have_et)
            printf("  offered at wire (CMAC) : %12d pkts   [clear-on-read interval]\n", d_w)
            printf("  offered (deliv+drop)   : %12d pkts   %12.1f pps\n", offered, off_pps)
            printf("  delivered to stack     : %12d pkts   %12.1f pps\n", d_rxp, del_pps)
            printf("  dropped (rx_missed)    : %12d pkts   %12.1f pps\n", d_rxm, drop_pps)
            if (have_et)
            printf("  dropped (adapt_rx)     : %12d pkts\n", d_ad)
            printf("  rx_dropped / rx_errors : %12d / %d\n", d_rxd, d_rxe)
            printf("  DROP %%                 : %12.4f %%\n", drop_pct)
            printf("  counter cross-check    : %s\n", check)
        }
        exit (bad ? 1 : 0)
    }'
    RC=$?          # awk exits 0 ok / 1 cross-check bad / 2 fatal
    set -e
    if [ "$RC" = 2 ]; then exit 2; fi
    if [ "$RC" = 1 ] && [ "$STRICT" = 1 ]; then
        printf '%s: FATAL: counter cross-check failed and --strict was given; the drop number is not trustworthy\n' "$PROG" >&2
        exit 1
    fi
    return 0
}

# -------------------------------------------------------------------- main ----

case "$MODE" in
  snapshot)
    [ -n "$DEV" ] || die "--dev is required"
    [ -d "/sys/class/net/$DEV" ] || die "no such netdev: $DEV"
    snapshot "$DEV"
    ;;

  diff)
    report "$DIFF_A" "$DIFF_B"
    exit "$RC"
    ;;

  measure)
    [ -n "$DEV" ] || die "--dev is required"
    [ -d "/sys/class/net/$DEV" ] || die "no such netdev: $DEV"
    DURATION=${DURATION:-10}
    case "$DURATION" in ''|*[!0-9.]*) die "--duration must be numeric" ;; esac
    A=$(snapshot "$DEV")
    if awk -v i="$INTERVAL" 'BEGIN{exit !(i+0 > 0)}'; then
        # per-interval rows
        if [ "$FMT" = human ]; then
            printf '%8s  %s\n' "t(s)" "per-interval rates"
        fi
        prev=$A
        elapsed=0
        while :; do
            sleep "$INTERVAL"
            elapsed=$(awk -v e="$elapsed" -v i="$INTERVAL" 'BEGIN{printf "%.3f", e+i}')
            cur=$(snapshot "$DEV")
            row=$( FMT=kv STRICT=0; report "$prev" "$cur" 2>/dev/null || true )
            if [ "$FMT" = human ]; then
                printf '%8s  %s\n' "$elapsed" \
                  "$(printf '%s' "$row" | tr ' ' '\n' \
                     | grep -E '^(delivered_pps|drop_pps|drop_pct)=' | tr '\n' ' ')"
            else
                printf 't=%s %s\n' "$elapsed" "$row"
            fi
            prev=$cur
            if awk -v e="$elapsed" -v d="$DURATION" 'BEGIN{exit !(e+1e-9 >= d)}'; then break; fi
        done
        B=$prev
    else
        sleep "$DURATION"
        B=$(snapshot "$DEV")
    fi
    if [ "$FMT" = human ]; then printf '\n=== totals over the whole window ===\n'; fi
    report "$A" "$B"
    exit "$RC"
    ;;
esac
