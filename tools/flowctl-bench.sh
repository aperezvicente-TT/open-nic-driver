#!/usr/bin/env bash
#
# flowctl-bench.sh -- RX-drop measurement harness with the 13.11 acceptance gate.
#
# RUNS ON THE RECEIVER (the host that owns the FPGA NIC). Drives a constant-rate
# generator on the sender over ssh, brackets it with the local per-port counters,
# and reports delivered pps / drop pps / drop %.
#
# --gate runs the SAME configuration N times (default 5) and decides, explicitly,
# whether the instrument is good enough to believe. From docs/13-flow-control-plan.md
# 13.11 step 1:
#
#     "Acceptance test: the same configuration measured five times must agree
#      within a factor of two before any tuning result is believed."
#
# NOTHING downstream of this gate is meaningful until it prints GATE: PASS.
#
# The gate is deliberately stricter than the one sentence above, because a
# factor-of-two rule alone is not decidable on this data:
#
#   G1 GENERATOR STABILITY. max/min offered packet count across reps must be
#      within --offer-tol (default 1%). If the source is not constant, a stable
#      drop % would be luck and an unstable one would be uninterpretable. This is
#      the check iperf3 could never have passed: 13.10 records offered counts of
#      4.4-6.7 M for one nominal setting, i.e. a 1.52x spread.
#   G2 SENDER VALIDITY. Every rep's generator must self-report that it held its
#      rate (pktgen: achieved pps within tolerance and 0 errors; udp-pacer: late
#      fraction under budget). A rep that fails is a HARD FAIL, never a silent
#      drop from the sample -- discarding failed reps is how you manufacture a
#      reproducible-looking result.
#   G3 COUNTER CONSISTENCY. Every rep's wire count must equal delivered + drops.
#   G4 FACTOR OF TWO on the drop metric, with an explicit rule for zero:
#        - all reps at or below --zero-pct (default 0.001 %)  -> PASS, lossless
#        - some reps at zero and some not                     -> FAIL (bimodal;
#          this is exactly the 0.0000-6.16 % pathology of 13.10)
#        - otherwise max/min <= --factor (default 2.0)        -> PASS
#
# Reported summary statistic is the MEDIAN drop %, per 13.11 step 2 ("medians of
# >= 5"). The mean is also printed but should not be used: it is dominated by the
# tail.
#
set -euo pipefail

PROG=${0##*/}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MEASURE=$HERE/rx-drop-measure.sh

# --- bench defaults, matching the current cabling ---------------------------
DEV=enp194s0                 # local FPGA port under test
PEER=192.168.12.135          # management address of the sender
PEER_DEV=enp130s0f0np0       # sender's egress port, cabled to $DEV
TARGET_IP=10.98.0.1          # address on $DEV
TARGET_MAC=""                # auto-detected from $DEV
SRC_IP=10.98.0.2             # address on $PEER_DEV

GEN=udp-pacer                # udp-pacer | pktgen
PPS=""
FRAME=9014
DURATION=10
COUNT=""
BURST=1                      # pktgen `burst` / udp-pacer `--batch`
CLONE=1000                   # pktgen clone_skb
THREADS=1                    # pktgen threads
SENDERS=1                    # udp-pacer parallel processes
CPUS=""
PORT=5201
REPS=1
GATE=0
SETTLE=0.5
SINK=1                       # bind-only UDP sink to suppress ICMP port-unreachable
FACTOR=2.0
OFFER_TOL=1.0
ZERO_PCT=0.001
ZERO_PKTS=10                 # ...or this many packets, whichever is more permissive
GAP=2                        # idle seconds between reps
WARMUP_REPS=0                # discarded, clearly-labelled warm-up repetitions
FMT=human
DRY=0
MAX_LATE=""                  # udp-pacer burstiness guard (its default: 0.10)
MAX_RATE_DEV=""              # udp-pacer achieved-rate tolerance (its default: 1.0 %)
TOOLS_REMOTE=""              # path to tools/ on the sender (default: same path)

die()  { printf '%s: FATAL: %s\n' "$PROG" "$*" >&2; exit 2; }
warn() { printf '%s: WARN: %s\n'  "$PROG" "$*" >&2; }

usage() {
    cat <<'EOF'
Usage:
  flowctl-bench.sh --pps N [--duration SEC | --count N] [options]
  flowctl-bench.sh --gate --pps N [--reps 5] [options]

The single most important mode:
  --gate            run the SAME configuration --reps times and print an explicit
                    GATE: PASS / GATE: FAIL verdict on whether the measurement is
                    reproducible (13.11 step 1). Exit 0 only on PASS.

Load:
  --pps N           target offered packet rate (required)
  --duration SEC    per-rep run length (default 10)
  --count N         exact per-rep packet budget (overrides --duration; the most
                    reproducible choice)
  --frame-size B    Ethernet frame bytes excl. CRC, 64..9014 (default 9014)
  --burst N         micro-burst depth at the same average rate (default 1).
                    pktgen `burst` / udp-pacer `--batch`. This is the knob for the
                    13.11 step-3 burst-depth sweep.

Generator:
  --gen NAME        udp-pacer (default, needs no privileges) or pktgen (needs root
                    on the sender -- see pktgen-sender.sh --help)
  --senders N       udp-pacer: parallel sender processes (default 1)
  --threads N       pktgen: kpktgend threads / tx queues (default 1)
  --clone N         pktgen: clone_skb (default 1000)
  --cpus LIST       comma-separated sender CPUs to pin to, one per sender
  --max-rate-dev P  udp-pacer: reject a rep whose achieved average rate is more
                    than P % off target (default 1.0). This is the real check on
                    the offered load; keep it tight.
  --max-late-frac F udp-pacer: reject a rep in which more than a fraction F of
                    deadlines slipped by a whole packet slot (default 0.10). This
                    is a BURST-SHAPE guard, not a rate check. At ~40 G with 9 kB
                    frames a userspace socket sender cannot keep a uniform spacing
                    (measured: ~10 % of slots slip), so you may have to raise it --
                    but do so DELIBERATELY and record that you did, because it
                    means the offered load is lumpier than the nominal constant
                    rate. If uniform spacing matters, use --gen pktgen.

Topology (defaults match the current cabling; change only if it changes):
  --dev DEV         local port under test (default enp194s0)
  --peer HOST       sender's ssh address (default 192.168.12.135)
  --peer-dev DEV    sender's egress port (default enp130s0f0np0)
  --target-ip IP    destination address on --dev (default 10.98.0.1)
  --target-mac MAC  destination MAC (default: read from --dev)
  --src-ip IP       source address on --peer-dev (default 10.98.0.2)
  --port N          destination UDP port (default 5201)
  --tools-remote D  path to this tools/ dir on the sender (default: same path,
                    which is correct here because tools/ is NFS-shared)

Gate thresholds:
  --reps N          repetitions (default 1, or 5 with --gate)
  --factor F        allowed max/min ratio of drop % (default 2.0)
  --offer-tol PCT   allowed spread of the offered packet count (default 1.0 %)
  --zero-pct PCT    drop % at or below this counts as lossless (default 0.001)
  --zero-pkts N     ...or this many dropped packets, whichever is more permissive
                    (default 10). Both exist because a percentage floor alone is
                    unusable on short runs: 2 packets out of 100 k is 0.002 %, which
                    a 0.001 % floor would wrongly call a real loss event.
  --gap SEC         idle time between reps (default 2)
  --warmup-reps N   run N repetitions BEFORE the measured ones and exclude them
                    (default 0). They are printed, labelled `warm`, so the
                    exclusion is visible rather than silent. Use this ONLY for a
                    cold-start systematic -- the first run after an idle period
                    behaves differently (CPU frequency, C-states, IRQ/NAPI
                    placement, cold descriptor rings). It is NOT a licence to drop
                    inconvenient reps: N is fixed in advance and applies to the
                    FIRST N runs only, whatever they turn out to be.
  --settle SEC      wait after the generator stops before the closing counter
                    snapshot, so in-flight packets are counted (default 0.5)
  --no-sink         do not open the local bind-only UDP sink. Without the sink the
                    receiver answers every datagram with ICMP port-unreachable
                    (rate-limited, but still extra receive-path work), so leave it
                    on unless you are deliberately testing that path.

Other:
  --kv              machine-readable output
  --dry-run         print what would be run, touch nothing
  -h, --help        this text

Exit codes: 0 ok / gate PASS, 1 gate FAIL, 2 usage or fatal error, 3 a rep was
invalid (generator could not hold its rate, or counters were inconsistent).

This script only READS local counters. It never writes to the device, never
reloads or reconfigures the driver, and never changes MTU or IP addressing.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)          DEV=${2:?};        shift 2 ;;
        --peer)         PEER=${2:?};       shift 2 ;;
        --peer-dev)     PEER_DEV=${2:?};   shift 2 ;;
        --target-ip)    TARGET_IP=${2:?};  shift 2 ;;
        --target-mac)   TARGET_MAC=${2:?}; shift 2 ;;
        --src-ip)       SRC_IP=${2:?};     shift 2 ;;
        --gen)          GEN=${2:?};        shift 2 ;;
        --pps)          PPS=${2:?};        shift 2 ;;
        --frame-size)   FRAME=${2:?};      shift 2 ;;
        --duration)     DURATION=${2:?};   shift 2 ;;
        --count)        COUNT=${2:?};      shift 2 ;;
        --burst)        BURST=${2:?};      shift 2 ;;
        --clone)        CLONE=${2:?};      shift 2 ;;
        --threads)      THREADS=${2:?};    shift 2 ;;
        --senders)      SENDERS=${2:?};    shift 2 ;;
        --cpus)         CPUS=${2:?};       shift 2 ;;
        --port)         PORT=${2:?};       shift 2 ;;
        --reps)         REPS=${2:?};       shift 2 ;;
        --gate)         GATE=1;            shift ;;
        --factor)       FACTOR=${2:?};     shift 2 ;;
        --offer-tol)    OFFER_TOL=${2:?};  shift 2 ;;
        --zero-pct)     ZERO_PCT=${2:?};   shift 2 ;;
        --zero-pkts)    ZERO_PKTS=${2:?};  shift 2 ;;
        --gap)          GAP=${2:?};        shift 2 ;;
        --warmup-reps)  WARMUP_REPS=${2:?}; shift 2 ;;
        --settle)       SETTLE=${2:?};     shift 2 ;;
        --no-sink)      SINK=0;            shift ;;
        --max-late-frac) MAX_LATE=${2:?};   shift 2 ;;
        --max-rate-dev)  MAX_RATE_DEV=${2:?}; shift 2 ;;
        --tools-remote) TOOLS_REMOTE=${2:?}; shift 2 ;;
        --kv)           FMT=kv;            shift ;;
        --dry-run)      DRY=1;             shift ;;
        -h|--help)      usage; exit 0 ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

[ -n "$PPS" ] || die "--pps is required (try --help)"
case "$GEN" in udp-pacer|pktgen) : ;; *) die "--gen must be udp-pacer or pktgen" ;; esac
if [ "$GATE" = 1 ] && [ "$REPS" = 1 ]; then REPS=5; fi
case "$REPS" in ''|*[!0-9]*) die "--reps must be an integer";; esac
[ "$REPS" -ge 1 ] || die "--reps must be >= 1"
if [ "$GATE" = 1 ] && [ "$REPS" -lt 3 ]; then
    die "--gate with --reps $REPS is meaningless; 13.11 asks for five"
fi
[ -x "$MEASURE" ] || die "$MEASURE is missing or not executable"
[ -d "/sys/class/net/$DEV" ] || die "no such local netdev: $DEV"
TOOLS_REMOTE=${TOOLS_REMOTE:-$HERE}

if [ -z "$TARGET_MAC" ]; then
    TARGET_MAC=$(< "/sys/class/net/$DEV/address")
fi
[ -n "$TARGET_MAC" ] || die "could not determine the MAC of $DEV"

if [ -z "$COUNT" ]; then
    COUNT=$(awk -v p="$PPS" -v s="$DURATION" 'BEGIN{printf "%d", int(p*s+0.5)}')
fi
[ "$COUNT" -gt 0 ] || die "computed packet count is 0"

SSH=( ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" )

# ------------------------------------------------------------ preflight -------
preflight() {
    local out
    out=$("${SSH[@]}" "hostname" 2>&1) || die "cannot ssh to $PEER: $out"
    PEER_HOST=$out
    out=$("${SSH[@]}" "cat /sys/class/net/$PEER_DEV/operstate 2>&1") || \
        die "sender has no netdev $PEER_DEV"
    [ "$out" = up ] || die "sender port $PEER_DEV is '$out', not up"
    out=$("${SSH[@]}" "test -f $TOOLS_REMOTE/udp-pacer.c && echo ok" 2>&1) || true
    [ "$out" = ok ] || die "the tools directory is not visible on the sender at
       $TOOLS_REMOTE (pass --tools-remote). Note that on this bench
       /home/alex/mpi-shfs is NFS-exported BY the sender, so the same absolute
       path is normally correct."
    # An unresolved ARP entry would make the first rep differ from the rest.
    if ! ping -c1 -W2 -q "$SRC_IP" >/dev/null 2>&1; then
        warn "cannot ping $SRC_IP from here; ARP may resolve mid-run and skew rep 1"
    fi
    if [ "$GEN" = pktgen ]; then
        if ! "${SSH[@]}" "sudo -n test -w /proc/net/pktgen/pgctrl" 2>/dev/null; then
            cat >&2 <<EOF
$PROG: FATAL: --gen pktgen cannot be driven on $PEER_HOST without more privilege.

  /proc/net/pktgen/* is created -rw------- root:root, and this account's
  passwordless sudo does not include any way to write it (no sudo sh/tee/dd/bash).
  'sudo -n modprobe pktgen' IS allowed, so the module can be loaded -- but loading
  it is not enough.

  Fix (once, by a human with the sudo password, ON $PEER_HOST):
      sudo install -o root -g root -m 0755 \\
          $TOOLS_REMOTE/pktgen-proc-write /usr/local/sbin/pktgen-proc-write
      sudo install -o root -g root -m 0440 \\
          $TOOLS_REMOTE/pktgen-sudoers.example /etc/sudoers.d/onic-pktgen
      sudo visudo -cf /etc/sudoers.d/onic-pktgen

  Or run it interactively without any sudoers change:
      ssh -t $PEER "sudo $TOOLS_REMOTE/pktgen-sender.sh --dev $PEER_DEV \\
          --dst-ip $TARGET_IP --dst-mac $TARGET_MAC --pps $PPS --count $COUNT"

  Until then use the default --gen udp-pacer, which needs no privileges.
EOF
            exit 3
        fi
    fi
}

# --------------------------------------------------------- generator cmds -----
gen_cmd() {
    if [ "$GEN" = pktgen ]; then
        printf '%s' "sudo -n $TOOLS_REMOTE/pktgen-sender.sh --kv \
--dev $PEER_DEV --dst-ip $TARGET_IP --dst-mac $TARGET_MAC --src-ip $SRC_IP \
--pps $PPS --count $COUNT --frame-size $FRAME --burst $BURST --clone $CLONE \
--threads $THREADS --udp-dst $PORT${CPUS:+ --cpu-base ${CPUS%%,*}}"
    else
        printf '%s' "$TOOLS_REMOTE/udp-pacer-sender.sh --kv \
--dst $TARGET_IP --src-ip $SRC_IP --pps $PPS --count $COUNT \
--frame-size $FRAME --batch $BURST --senders $SENDERS --port $PORT${CPUS:+ --cpus $CPUS}\
${MAX_LATE:+ --max-late-frac $MAX_LATE}${MAX_RATE_DEV:+ --max-rate-dev $MAX_RATE_DEV}"
    fi
}

# ------------------------------------------------------------------ sink ------
# A bind-only UDP sink on the destination port(s). It NEVER reads: binding is
# enough to stop the kernel emitting ICMP port-unreachable for every arriving
# datagram, and never reading means it burns no CPU and cannot perturb NAPI or
# steal a core from the receive path. Datagrams pile up in the socket buffer and
# are dropped there -- a UDP-layer drop, entirely separate from the
# rx_missed_errors / DESC_RSP_DROP counter this harness measures.
SINK_PID=""
sink_start() {
    [ "$SINK" = 1 ] || return 0
    command -v python3 >/dev/null || { warn "python3 missing; running without a sink (expect ICMP port-unreachable)"; return 0; }
    python3 -c '
import socket, sys, time
base, n, secs = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
socks = []
for i in range(n):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", base + i))
    socks.append(s)
sys.stderr.write("ready\n"); sys.stderr.flush()
time.sleep(secs)
' "$PORT" "$SENDERS" "$1" 2>/dev/null &
    SINK_PID=$!
    sleep 0.3   # let the binds land before the generator starts
}
sink_stop() {
    if [ -n "$SINK_PID" ]; then kill "$SINK_PID" 2>/dev/null || true; wait "$SINK_PID" 2>/dev/null || true; SINK_PID=""; fi
}
trap sink_stop EXIT

# kv_get <kv-line> <key> [default]
kv_get() {
    local line=$1 key=$2 def=${3:-}
    local tok
    for tok in $line; do
        if [ "${tok%%=*}" = "$key" ]; then printf '%s' "${tok#*=}"; return 0; fi
    done
    printf '%s' "$def"
}

# ---------------------------------------------------------------- one rep -----
# Sets: R_OFFERED R_DELIVERED R_DROPPED R_DROPPCT R_GENPPS R_GENSEC R_RC R_XCHK
run_rep() {
    local idx=$1
    local snap_a snap_b gen_out gen_rc rep_out

    # Sink covers the generator run plus warmup, settle and ssh overhead.
    sink_start "$(awk -v s="$DURATION" 'BEGIN{printf "%.1f", s+20}')"

    snap_a=$("$MEASURE" --dev "$DEV" --snapshot)

    gen_rc=0
    gen_out=$("${SSH[@]}" "$(gen_cmd)" 2>&1) || gen_rc=$?

    sleep "$SETTLE"
    snap_b=$("$MEASURE" --dev "$DEV" --snapshot)
    sink_stop

    R_GEN_RAW=$(printf '%s\n' "$gen_out" | grep -E '^(gen=|RESULT gen=)' | tail -1 || true)
    if [ -z "$R_GEN_RAW" ]; then
        printf '%s: rep %s: the generator produced no result line. Output was:\n%s\n' \
            "$PROG" "$idx" "$gen_out" >&2
        R_RC=3
        return 0
    fi
    R_GENPPS=$(kv_get "$R_GEN_RAW" achieved_pps 0)
    R_GENSEC=$(kv_get "$R_GEN_RAW" run_s 0)
    R_GENSENT=$(kv_get "$R_GEN_RAW" sent_pkts 0)
    R_GENDEV=$(kv_get "$R_GEN_RAW" rate_dev_pct 0)

    # Use the generator's own elapsed time as the rate denominator: the local
    # bracket includes the ssh round-trip and would inflate the window.
    local win=$R_GENSEC
    if awk -v w="$win" 'BEGIN{exit !(w+0 <= 0)}'; then win=""; fi

    rep_out=$("$MEASURE" --diff "$snap_a" "$snap_b" --kv ${win:+--window "$win"} 2>&1) || true
    if ! printf '%s' "$rep_out" | grep -q 'offered_pkts='; then
        printf '%s: rep %s: receiver measurement failed:\n%s\n' "$PROG" "$idx" "$rep_out" >&2
        R_RC=3
        return 0
    fi
    R_OFFERED=$(kv_get   "$rep_out" offered_pkts)
    R_DELIVERED=$(kv_get "$rep_out" delivered_pkts)
    R_DROPPED=$(kv_get   "$rep_out" dropped_pkts)
    R_DROPPCT=$(kv_get   "$rep_out" drop_pct)
    R_DELPPS=$(kv_get    "$rep_out" delivered_pps)
    R_DROPPPS=$(kv_get   "$rep_out" drop_pps)
    R_OFFPPS=$(kv_get    "$rep_out" offered_pps)
    R_XCHK=$(kv_get      "$rep_out" crosscheck SKIPPED)
    R_RC=$gen_rc
    return 0
}

# -------------------------------------------------------------------- main ----
if [ "$DRY" = 1 ]; then
    cat <<EOF
# --dry-run
local device      : $DEV  (mac $TARGET_MAC, ip $TARGET_IP)
sender            : $PEER  dev $PEER_DEV  src $SRC_IP
generator         : $GEN
per-rep budget    : $COUNT packets at $PPS pps, frame $FRAME B, burst/batch $BURST
reps              : $REPS   gate=$GATE (factor $FACTOR, offer-tol $OFFER_TOL %, zero $ZERO_PCT %)
remote command    : $(gen_cmd)
receiver snapshots: $MEASURE --dev $DEV --snapshot
EOF
    exit 0
fi

preflight

if [ "$FMT" = human ]; then
    cat <<EOF
=== flowctl-bench: $DEV <- $PEER_HOST:$PEER_DEV ===
generator        : $GEN
offered load     : $PPS pps, $COUNT packets/rep, frame $FRAME B, burst/batch $BURST
$( [ "$GEN" = pktgen ] && printf 'pktgen           : threads %s clone_skb %s\n' "$THREADS" "$CLONE" || printf 'udp-pacer        : %s parallel sender(s)%s\n' "$SENDERS" "${CPUS:+ pinned to $CPUS}" )
reps             : $REPS measured$( [ "$WARMUP_REPS" != 0 ] && printf ' + %s excluded warm-up' "$WARMUP_REPS" || true )$( [ "$GATE" = 1 ] && printf '  (ACCEPTANCE GATE)' || true )

EOF
    printf '%3s %12s %12s %12s %10s %12s %10s %s\n' \
        rep offered delivered dropped 'drop_%' gen_pps 'rate_dev' xcheck
fi

DROPS=(); OFFERS=(); DROPPKTS=(); INVALID=0; BADXCHK=0
case "$WARMUP_REPS" in ''|*[!0-9]*) die "--warmup-reps must be an integer";; esac
TOTAL_REPS=$(( REPS + WARMUP_REPS ))
for r in $(seq 1 "$TOTAL_REPS"); do
    if [ "$r" -gt 1 ]; then sleep "$GAP"; fi
    IS_WARM=0
    if [ "$r" -le "$WARMUP_REPS" ]; then IS_WARM=1; fi
    R_RC=0; R_OFFERED=0; R_DELIVERED=0; R_DROPPED=0; R_DROPPCT=0
    R_GENPPS=0; R_GENDEV=0; R_XCHK=SKIPPED; R_DELPPS=0; R_DROPPPS=0; R_OFFPPS=0
    run_rep "$r"

    if [ "$IS_WARM" = 1 ]; then
        if [ "$FMT" = human ]; then
            printf '%3s %12s %12s %12s %10s %12s %10s %s\n' \
                "w$r" "${R_OFFERED:-?}" "${R_DELIVERED:-?}" "${R_DROPPED:-?}" \
                "${R_DROPPCT:-?}" "$(printf '%.0f' "${R_GENPPS:-0}")" "${R_GENDEV:-?}" \
                "warm-up, EXCLUDED (gen_rc=$R_RC)"
        else
            printf 'rep=w%s status=WARMUP_EXCLUDED offered_pkts=%s dropped_pkts=%s drop_pct=%s gen_rc=%s\n' \
                "$r" "${R_OFFERED:-0}" "${R_DROPPED:-0}" "${R_DROPPCT:-0}" "$R_RC"
        fi
        continue
    fi
    if [ "$R_RC" != 0 ]; then
        INVALID=$((INVALID+1))
        if [ "$FMT" = human ]; then
            printf '%3s %s\n' "$((r - WARMUP_REPS))" "INVALID -- generator exited $R_RC (rate not held / send errors). NOT counted, NOT discarded: the gate fails."
        else
            printf 'rep=%s status=INVALID gen_rc=%s\n' "$r" "$R_RC"
        fi
        continue
    fi
    case "$R_XCHK" in BAD) BADXCHK=$((BADXCHK+1)) ;; esac

    DROPS+=( "$R_DROPPCT" )
    OFFERS+=( "$R_OFFERED" )
    DROPPKTS+=( "$R_DROPPED" )
    if [ "$FMT" = human ]; then
        printf '%3s %12s %12s %12s %10s %12s %10s %s\n' \
            "$((r - WARMUP_REPS))" "$R_OFFERED" "$R_DELIVERED" "$R_DROPPED" "$R_DROPPCT" \
            "$(printf '%.0f' "$R_GENPPS")" "$R_GENDEV" "$R_XCHK"
    else
        printf 'rep=%s status=OK offered_pkts=%s delivered_pkts=%s dropped_pkts=%s drop_pct=%s offered_pps=%s delivered_pps=%s drop_pps=%s gen_pps=%s rate_dev_pct=%s crosscheck=%s\n' \
            "$((r - WARMUP_REPS))" "$R_OFFERED" "$R_DELIVERED" "$R_DROPPED" "$R_DROPPCT" \
            "$R_OFFPPS" "$R_DELPPS" "$R_DROPPPS" "$R_GENPPS" "$R_GENDEV" "$R_XCHK"
    fi
done

VALID=${#DROPS[@]}
if [ "$VALID" = 0 ]; then die "every repetition was invalid; nothing to report"; fi

STATS=$(printf '%s\n' "${DROPS[@]}" | sort -g | awk '
    { v[NR] = $1 + 0; s += $1 + 0 }
    END {
        n = NR
        med = (n % 2 ? v[(n+1)/2] : (v[n/2] + v[n/2+1]) / 2)
        printf("n=%d min=%.6f max=%.6f median=%.6f mean=%.6f\n", n, v[1], v[n], med, s/n)
    }')
PSTATS=$(printf '%s\n' "${DROPPKTS[@]}" | sort -g | awk '
    { v[NR] = $1 + 0 }
    END { printf("pmin=%d pmax=%d\n", v[1], v[NR]) }')
OSTATS=$(printf '%s\n' "${OFFERS[@]}" | sort -g | awk '
    { v[NR] = $1 + 0 }
    END { printf("omin=%d omax=%d ospread_pct=%.4f\n", v[1], v[NR], (v[1] > 0 ? 100*(v[NR]-v[1])/v[1] : 999)) }')

eval "$STATS"; eval "$OSTATS"; eval "$PSTATS"

if [ "$FMT" = human ]; then
    cat <<EOF

--- summary over $VALID valid rep(s) ---
  drop %          : min $min  median $median  max $max   (mean $mean -- do not use)
  dropped packets : min $pmin  max $pmax
  offered packets : min $omin  max $omax   spread $ospread_pct %
EOF
else
    printf 'summary %s %s %s invalid_reps=%s bad_crosschecks=%s\n' "$STATS" "$OSTATS" "$PSTATS" "$INVALID" "$BADXCHK"
fi

# ------------------------------------------------------------ the gate --------
if [ "$GATE" != 1 ]; then
    if [ "$INVALID" != 0 ] || [ "$BADXCHK" != 0 ]; then exit 3; fi
    exit 0
fi

VERDICT=PASS; REASONS=()

# G1 generator stability
if awk -v s="$ospread_pct" -v t="$OFFER_TOL" 'BEGIN{exit !(s+0 > t+0)}'; then
    VERDICT=FAIL
    REASONS+=( "G1 generator NOT constant: offered packet count spread $ospread_pct % > --offer-tol $OFFER_TOL % (min $omin, max $omax). The source, not the receiver, is moving." )
else
    REASONS+=( "G1 ok: offered packet count spread $ospread_pct % <= $OFFER_TOL %" )
fi

# G2 sender validity
if [ "$INVALID" != 0 ]; then
    VERDICT=FAIL
    REASONS+=( "G2 FAIL: $INVALID of $REPS reps were invalid (the generator could not hold its rate). They are counted as failures, not discarded." )
else
    REASONS+=( "G2 ok: all $REPS reps held the requested rate" )
fi
if [ "$VALID" != "$REPS" ]; then
    VERDICT=FAIL
    REASONS+=( "G2 FAIL: only $VALID of $REPS reps produced a measurement" )
fi

# G3 counter consistency
if [ "$BADXCHK" != 0 ]; then
    VERDICT=FAIL
    REASONS+=( "G3 FAIL: $BADXCHK rep(s) failed the wire-vs-delivered+dropped cross-check; the drop numbers are not trustworthy" )
else
    REASONS+=( "G3 ok: counters self-consistent in every rep" )
fi

# G4 factor of two, with an explicit zero rule
G4=$(awk -v mn="$min" -v mx="$max" -v pmn="$pmin" -v pmx="$pmax" \
         -v z="$ZERO_PCT" -v zp="$ZERO_PKTS" -v f="$FACTOR" 'BEGIN{
    # "lossless" = below EITHER floor, so that a couple of stray packets on a short
    # run is not mistaken for a loss event.
    lo_max = (mx <= z || pmx <= zp)
    lo_min = (mn <= z || pmn <= zp)
    if (lo_max) { printf "PASS|every rep at the lossless floor (worst %.6f %% / %d packets; floors %g %% / %d packets): consistently lossless, so there is no drop signal to reproduce yet -- raise --pps until there is\n", mx, pmx, z, zp; exit }
    if (lo_min) { printf "FAIL|bimodal: the best rep is at the lossless floor (%.6f %% / %d packets) while the worst is %.6f %% / %d packets. A factor-of-two rule cannot be applied across zero; this is the 13.10 pathology, not a measurement.\n", mn, pmn, mx, pmx; exit }
    r = mx / mn
    if (r <= f) printf "PASS|max/min = %.3f <= %.2f (min %.6f %%, max %.6f %%)\n", r, f, mn, mx
    else        printf "FAIL|max/min = %.3f > %.2f (min %.6f %%, max %.6f %%): the same configuration does not reproduce\n", r, f, mn, mx
}')
G4V=${G4%%|*}; G4M=${G4#*|}
if [ "$G4V" != PASS ]; then VERDICT=FAIL; fi
REASONS+=( "G4 $G4V: $G4M" )

printf '\n=== ACCEPTANCE GATE (docs/13-flow-control-plan.md 13.11 step 1) ===\n'
printf 'same configuration, %s repetitions, factor-of-%s agreement required\n\n' "$REPS" "$FACTOR"
for x in "${REASONS[@]}"; do printf '  - %s\n' "$x"; done
printf '\nmedian drop %% = %s  (use this, not the mean)\n' "$median"
printf 'GATE: %s\n' "$VERDICT"
if [ "$VERDICT" != PASS ]; then
    printf '\nDo NOT believe any tuning comparison made with this instrument until the\ngate passes. See 13.10 for four consecutive conclusions that failed to replicate.\n'
    exit 1
fi
exit 0
