#!/usr/bin/env bash
#
# udp-pacer-sender.sh -- build-and-run wrapper for tools/udp-pacer.c.
#
# RUNS ON THE SENDER (desktop-0). Needs NO privileges at all: no root, no
# modprobe, no /proc writes, no capabilities. This is the generator to use while
# the pktgen privilege gap (see pktgen-sender.sh --help) is open.
#
# It compiles udp-pacer.c into a host-local cache directory (the tools/ tree is
# NFS-shared between the two hosts, so a binary must not be shared), then runs
# either one pinned instance or --senders N pinned instances that together offer
# --pps, and aggregates their self-reported results.
#
# Aggregation is strict: if ANY instance reports send errors or misses more than
# its deadline budget, the whole run is declared invalid (exit 4) so that a
# sender-limited run can never be mistaken for a receiver drop measurement.
#
set -euo pipefail

PROG=${0##*/}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SRC=$HERE/udp-pacer.c
CACHE=/var/tmp/onic-tools-${USER:-$(id -un)}
BIN=$CACHE/udp-pacer

DST=""; PORT=5201; PPS=""; DURATION=""; COUNT=""
FRAME=9014; BATCH=1; SENDERS=1; CPUS=""; SRC_IP=""; WARMUP=200
MAX_LATE=0.10; MAX_RATE_DEV=1.0; FMT=human; REBUILD=0; EXTRA=()

die()  { printf '%s: FATAL: %s\n' "$PROG" "$*" >&2; exit 2; }

usage() {
    cat <<'EOF'
Usage (on the SENDER, unprivileged):
  udp-pacer-sender.sh --dst IP --pps N {--duration SEC | --count N}
                      [--port N] [--frame-size B] [--batch N] [--senders N]
                      [--cpus LIST] [--src-ip IP] [--warmup N]
                      [--max-late-frac F] [--max-rate-dev PCT]
                      [--kv] [--rebuild] [-- EXTRA ARGS]

Required:
  --dst IP           destination IPv4 (the FPGA port, e.g. 10.98.0.1)
  --pps N            TOTAL target average packet rate across all senders
  --duration SEC     run length (count derived as pps*SEC), or
  --count N          exact TOTAL packet budget (preferred: fully deterministic)

Options:
  --port N           destination UDP port (default 5201). With --senders N the
                     instances use ports PORT..PORT+N-1 so RSS spreads them.
  --frame-size B     Ethernet frame bytes excl. CRC, 64..9014 (default 9014)
  --batch N          packets per deadline; N>1 is a deliberate micro-burst of N
                     at the same average rate (default 1)
  --senders N        number of parallel pinned sender processes (default 1). Each
                     gets pps/N and count/N. Use this when one process cannot
                     hold the rate (udp-pacer will tell you: exit 4).
  --cpus LIST        comma-separated CPU list to pin senders to, one per sender
                     (default: no pinning -- pinning is strongly recommended for
                     repeatability)
  --src-ip IP        bind source address (pins which port egresses)
  --warmup N         paced, unmeasured warmup packets per sender (default 200)
  --max-late-frac F  per-sender burstiness guard: deadline-miss budget (default
                     0.10). NOT the rate check.
  --max-rate-dev PCT per-sender achieved-rate tolerance (default 1.0 %). The
                     AGGREGATE rate across senders is checked separately below.
  --kv               machine-readable aggregate KEY=VALUE line
  --rebuild          force recompile
  -h, --help         this text

Everything after a bare `--` is passed through to udp-pacer unchanged.

Exit codes: 0 ok, 2 usage/fatal, 3 a sender reported send errors, 4 a sender
could not hold its rate. 3 and 4 mean THE RUN IS NOT A VALID MEASUREMENT.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dst)            DST=${2:?};      shift 2 ;;
        --port)           PORT=${2:?};     shift 2 ;;
        --pps)            PPS=${2:?};      shift 2 ;;
        --duration)       DURATION=${2:?}; shift 2 ;;
        --count)          COUNT=${2:?};    shift 2 ;;
        --frame-size)     FRAME=${2:?};    shift 2 ;;
        --batch)          BATCH=${2:?};    shift 2 ;;
        --senders)        SENDERS=${2:?};  shift 2 ;;
        --cpus)           CPUS=${2:?};     shift 2 ;;
        --src-ip)         SRC_IP=${2:?};   shift 2 ;;
        --warmup)         WARMUP=${2:?};   shift 2 ;;
        --max-late-frac)  MAX_LATE=${2:?}; shift 2 ;;
        --max-rate-dev)   MAX_RATE_DEV=${2:?}; shift 2 ;;
        --kv)             FMT=kv;          shift ;;
        --rebuild)        REBUILD=1;       shift ;;
        -h|--help)        usage; exit 0 ;;
        --)               shift; EXTRA=( "$@" ); break ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

[ -n "$DST" ] || die "--dst is required (try --help)"
[ -n "$PPS" ] || die "--pps is required"
[ -n "$COUNT" ] || [ -n "$DURATION" ] || die "one of --count / --duration is required"
case "$SENDERS" in ''|*[!0-9]*) die "--senders must be an integer";; esac
[ "$SENDERS" -ge 1 ] || die "--senders must be >= 1"

[ -f "$SRC" ] || die "generator source not found at $SRC"

# ---------------------------------------------------------------- build --------
mkdir -p "$CACHE"
if [ "$REBUILD" = 1 ] || [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    command -v gcc >/dev/null || die "gcc not found on $(hostname); cannot build udp-pacer"
    gcc -O2 -Wall -Wextra -o "$BIN.tmp.$$" "$SRC" || die "compile of $SRC failed"
    mv -f "$BIN.tmp.$$" "$BIN"
    if [ "$FMT" = human ]; then printf '%s: built %s on %s\n' "$PROG" "$BIN" "$(hostname)" >&2; fi
fi

# ------------------------------------------------------------- split load ------
if [ -z "$COUNT" ]; then
    COUNT=$(awk -v p="$PPS" -v s="$DURATION" 'BEGIN{printf "%d", int(p*s+0.5)}')
fi
PER_PPS=$(awk -v p="$PPS" -v n="$SENDERS" 'BEGIN{printf "%.6f", p/n}')
PER_CNT=$(( COUNT / SENDERS ))
[ "$PER_CNT" -gt 0 ] || die "--count $COUNT over $SENDERS senders gives 0 packets each"
TOTAL=$(( PER_CNT * SENDERS ))

IFS=',' read -r -a CPUARR <<< "${CPUS:-}"
if [ -n "$CPUS" ] && [ "${#CPUARR[@]}" -lt "$SENDERS" ]; then
    die "--cpus has ${#CPUARR[@]} entries but --senders is $SENDERS"
fi

# ------------------------------------------------------------------- run -------
TMPD=$(mktemp -d /var/tmp/udp-pacer-run.XXXXXX)
cleanup() { rm -rf "$TMPD"; }
trap cleanup EXIT

pids=()
for i in $(seq 0 $((SENDERS-1))); do
    cmd=( "$BIN" --dst "$DST" --port "$((PORT + i))" --pps "$PER_PPS"
          --count "$PER_CNT" --frame-size "$FRAME" --batch "$BATCH"
          --warmup "$WARMUP" --max-late-frac "$MAX_LATE"
          --max-rate-dev "$MAX_RATE_DEV" --quiet )
    if [ -n "$SRC_IP" ]; then cmd+=( --src-ip "$SRC_IP" ); fi
    if [ "${#EXTRA[@]}" -gt 0 ]; then cmd+=( "${EXTRA[@]}" ); fi
    if [ -n "$CPUS" ]; then
        taskset -c "${CPUARR[$i]}" "${cmd[@]}" >"$TMPD/out.$i" 2>"$TMPD/err.$i" &
    else
        "${cmd[@]}" >"$TMPD/out.$i" 2>"$TMPD/err.$i" &
    fi
    pids+=( $! )
done

worst_rc=0
for i in $(seq 0 $((SENDERS-1))); do
    rc=0
    wait "${pids[$i]}" || rc=$?
    if [ "$rc" -gt "$worst_rc" ]; then worst_rc=$rc; fi
done

# -------------------------------------------------------------- aggregate ------
AGG=$(cat "$TMPD"/out.* 2>/dev/null | awk '
    /^RESULT / {
        for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        sent += v["sent_pkts"] + 0
        budget += v["budget_pkts"] + 0
        late += v["late_batches"] + 0
        errs += v["send_errors"] + 0
        retr += v["retries"] + 0
        gbps += v["gbps"] + 0
        if (v["run_s"] + 0 > wall) wall = v["run_s"] + 0
        if (v["worst_late_ns"] + 0 > wl) wl = v["worst_late_ns"] + 0
        n++
    }
    END {
        if (n == 0) { print "PARSE_FAIL"; exit 0 }
        printf("instances=%d sent_pkts=%d budget_pkts=%d run_s=%.6f achieved_pps=%.1f gbps=%.4f late_batches=%d worst_late_ns=%d send_errors=%d retries=%d\n",
               n, sent, budget, wall, (wall > 0 ? sent / wall : 0), gbps, late, wl, errs, retr)
    }')

if [ "$AGG" = PARSE_FAIL ] || [ -z "$AGG" ]; then
    printf '%s: FATAL: no RESULT line from any sender. stderr follows:\n' "$PROG" >&2
    cat "$TMPD"/err.* >&2 || true
    exit 2
fi

eval "$(printf '%s\n' "$AGG" | tr ' ' '\n' | sed 's/^/A_/')" 2>/dev/null || true
DEVPCT=$(awk -v a="${A_achieved_pps}" -v t="$PPS" 'BEGIN{printf "%+.4f", (t>0 ? 100*(a-t)/t : 0)}')

if [ "$FMT" = kv ]; then
    printf 'gen=udp-pacer dst=%s target_pps=%s frame=%s batch=%s senders=%s count=%s %s rate_dev_pct=%s rc=%s\n' \
        "$DST" "$PPS" "$FRAME" "$BATCH" "$SENDERS" "$TOTAL" "$AGG" "$DEVPCT" "$worst_rc"
else
    cat <<EOF
udp-pacer aggregate: $SENDERS sender(s) -> $DST:$PORT
  frame / batch        : $FRAME B / $BATCH
  target total rate    : $PPS pps  ($PER_PPS pps each)
  packet budget        : $TOTAL ($PER_CNT each)
  ---- measured by the senders themselves ----
  $AGG
  rate deviation       : $DEVPCT %
EOF
    if [ "$worst_rc" != 0 ]; then cat "$TMPD"/err.* >&2 || true; fi
fi

# The aggregate achieved rate is what actually defines the offered load, so check
# it independently of the per-sender verdicts.
if awk -v d="$DEVPCT" -v t="$MAX_RATE_DEV" 'BEGIN{d=(d<0?-d:d); exit !(d > t+0)}'; then
    printf '%s: FATAL: aggregate achieved %s pps vs target %s pps (%s %%, limit %s %%).\n' \
        "$PROG" "${A_achieved_pps}" "$PPS" "$DEVPCT" "$MAX_RATE_DEV" >&2
    printf '%s:        THIS RUN IS NOT A VALID MEASUREMENT. Add --senders, raise --batch,\n' "$PROG" >&2
    printf '%s:        or lower --pps. One socket on this bench tops out near 165 kpps at\n' "$PROG" >&2
    printf '%s:        9014 B frames, so ~40 G needs at least 4 parallel senders.\n' "$PROG" >&2
    exit 4
fi
# Compare against the budget the senders actually adopted: with --batch N, each
# instance rounds its count down to a whole number of batches so that every
# deadline carries an identical burst. That rounding is deterministic, so the
# offered total stays identical across repetitions -- which is what the gate needs.
if [ "${A_sent_pkts:-0}" != "${A_budget_pkts:-x}" ]; then
    printf '%s: FATAL: senders delivered %s of their own %s packet budget -- THIS RUN IS NOT A VALID MEASUREMENT.\n' \
        "$PROG" "${A_sent_pkts:-0}" "${A_budget_pkts:-?}" >&2
    exit 3
fi
if [ "${A_budget_pkts:-0}" != "$TOTAL" ] && [ "$FMT" = human ]; then
    printf '%s: note: --batch %s rounded the budget from %s to %s packets (deterministic)\n' \
        "$PROG" "$BATCH" "$TOTAL" "${A_budget_pkts}" >&2
fi
if [ "$worst_rc" != 0 ]; then
    printf '%s: FATAL: at least one sender exited %s -- THIS RUN IS NOT A VALID MEASUREMENT.\n' "$PROG" "$worst_rc" >&2
    exit "$worst_rc"
fi
exit 0
