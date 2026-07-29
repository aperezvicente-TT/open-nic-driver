#!/usr/bin/env bash
#
# pktgen-sender.sh -- exact constant-rate packet generator, kernel pktgen backend.
#
# RUNS ON THE SENDER (desktop-0). MUST RUN AS ROOT: /proc/net/pktgen/* is mode 0600
# root:root, so even reading it as a normal user fails. See --help for the exact
# privilege gap and the sudoers line that closes it.
#
# ============================================================================
# WHY THIS PACING MECHANISM IS REPRODUCIBLE
# ============================================================================
# Two knobs, and the choice matters:
#
#   count  = pps * duration      -- an EXACT PACKET BUDGET, not a time limit.
#            Every repetition offers bit-for-bit the same number of packets, so
#            the drop-% denominator is identical across runs. This alone removes
#            the largest source of the iperf3 variance documented in
#            docs/13-flow-control-plan.md 13.10, where the delivered count
#            swung 4.4-6.7 M for the same nominal offer.
#
#   delay  = round(1e9 * burst * nthreads / pps)   nanoseconds
#            We set `delay` directly rather than pktgen's `ratep`, because:
#              * `ratep` is implemented internally as delay = NSEC_PER_SEC/ratep,
#                i.e. the same mechanism with an integer truncation we cannot see
#                or correct; and
#              * `ratep` knows nothing about `burst` or the thread count, so with
#                burst > 1 or nthreads > 1 it silently overshoots by that factor.
#                Computing `delay` ourselves lets us compensate exactly and lets
#                the script report the delay it used.
#
#            pktgen's pacer (spin() in net/core/pktgen.c) advances its next
#            transmit deadline as next_tx = previous_deadline + delay -- an
#            ABSOLUTE schedule. Errors do not accumulate, so the long-run average
#            rate is set by `delay` alone and does not depend on how long any
#            individual transmit took. It hrtimer-sleeps while far from the
#            deadline and busy-waits for the last ~100 us, so inter-packet spacing
#            is uniform to sub-microsecond.
#
# Additionally pktgen runs in a per-CPU kernel thread (kpktgend_N) pinned by
# construction, calls the driver's ndo_start_xmit directly, and allocates no skb
# per packet when clone_skb > 0. There is no socket, no qdisc, no GSO and no
# userspace scheduler in the path -- which is precisely the sender-side variance
# that made iperf3 unusable as an instrument.
#
# NOTE ON `burst` vs `clone_skb` -- they are different knobs:
#   clone_skb N : re-use one skb N times. Cost reduction only. Keeps the pacing
#                 uniform. Use it to reach high pps.
#   burst N     : hand N packets to the driver back-to-back with xmit_more set,
#                 i.e. a deliberate MICRO-BURST. One spin()/deadline per burst, so
#                 the average rate needs the burst compensation above. This is the
#                 knob for the 13.11 step-3 "sweep burst depth at fixed average
#                 rate" experiment.
#
# THE RATE IS ALWAYS VERIFIED, NEVER ASSUMED: pktgen reports the achieved pps and
# the elapsed time, and this script fails (exit 4) if the achieved rate deviates
# from the target by more than --rate-tol. A run whose generator did not hold its
# rate is rejected, not averaged in.
# ============================================================================
#
set -euo pipefail

PROG=${0##*/}
PG=/proc/net/pktgen

DEV=""
DST_IP=""
DST_MAC=""
SRC_IP=""
PPS=""
DELAY_NS=""
FRAME=9014          # Ethernet frame bytes, excl. 4-byte CRC (9014 = 9000 B MTU)
DURATION=""
COUNT=""
BURST=1
CLONE=1000
THREADS=1
CPU_BASE=""
UDP_SRC=9
UDP_DST=9
QUEUE=""
FMT="human"
DRY=0
RATE_TOL=2.0        # percent
KEEP=0

die()  { printf '%s: FATAL: %s\n' "$PROG" "$*" >&2; exit 2; }
warn() { printf '%s: WARN: %s\n'  "$PROG" "$*" >&2; }

usage() {
    cat <<'EOF'
Usage (as root, on the SENDER host):
  pktgen-sender.sh --dev DEV --dst-ip IP --dst-mac MAC
                   {--pps N | --delay-ns N} {--duration SEC | --count N}
                   [--frame-size B] [--burst N] [--clone N] [--threads N]
                   [--cpu-base N] [--queue N] [--udp-src P] [--udp-dst P]
                   [--src-ip IP] [--rate-tol PCT] [--kv] [--dry-run] [--keep]

Required:
  --dev DEV        egress netdev on the sender (e.g. enp130s0f0np0)
  --dst-ip IP      destination IP (e.g. 10.98.0.1)
  --dst-mac MAC    destination MAC. pktgen bypasses ARP, so this is mandatory and
                   must be the RECEIVER PORT's MAC (e.g. 00:0a:35:0d:c2:00).
  --pps N          target AVERAGE packet rate (packets/second), or
  --delay-ns N     set pktgen's inter-transmit delay directly (expert)
  --duration SEC   run length; count is derived as pps*SEC (needs --pps), or
  --count N        exact total packet budget (preferred: fully deterministic)

Shaping:
  --frame-size B   Ethernet frame size excluding CRC, 64..9014 (default 9014).
                   9014 = 14 eth + 20 ip + 8 udp + 8972 payload = 9000 B MTU.
  --burst N        pktgen `burst`: N packets back-to-back with xmit_more (default 1).
                   `delay` is scaled by N so the AVERAGE rate stays at --pps.
  --clone N        pktgen `clone_skb` (default 1000). 0 = fresh skb per packet.
  --threads N      number of kpktgend threads / tx queues (default 1)
  --cpu-base N     first CPU to use for the kthreads (default: leave to pktgen's
                   own thread for CPU 0.. ; explicit is better for repeatability)
  --queue N        pin to tx queue N (default: queue = thread index)

Other:
  --rate-tol PCT   fail if achieved pps deviates from target by more than this
                   (default 2.0). Set 0 to disable the check (not recommended).
  --kv             machine-readable KEY=VALUE result line
  --dry-run        print every /proc/net/pktgen write; touch nothing
  --keep           leave the pktgen config loaded after the run (for inspection)
  -h, --help       this text

Exit codes: 0 ok, 2 usage/fatal, 3 insufficient privilege, 4 achieved rate outside
--rate-tol (the run is NOT a valid measurement), 5 pktgen reported errors.

--------------------------------------------------------------------------------
PRIVILEGE REQUIREMENT
--------------------------------------------------------------------------------
/proc/net/pktgen/* is created by the kernel as -rw------- root:root. A normal user
cannot read or write it. Loading the module is already allowed passwordlessly on
both hosts (`sudo -n modprobe pktgen`), but WRITING the proc files is not, and
none of `sudo sh -c`, `sudo tee`, `sudo dd` or `sudo bash` is in the NOPASSWD set.

A human with the sudo password can just run:

    sudo modprobe pktgen
    sudo /home/alex/mpi-shfs/fpga/open-nic-driver/tools/pktgen-sender.sh ...

To make it passwordless (so the harness can drive it over ssh), install the
root-owned wrapper shipped next to this script and whitelist exactly that one
path -- see tools/pktgen-sudoers.example and tools/pktgen-proc-write.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)        DEV=${2:?};      shift 2 ;;
        --dst-ip)     DST_IP=${2:?};   shift 2 ;;
        --dst-mac)    DST_MAC=${2:?};  shift 2 ;;
        --src-ip)     SRC_IP=${2:?};   shift 2 ;;
        --pps)        PPS=${2:?};      shift 2 ;;
        --delay-ns)   DELAY_NS=${2:?}; shift 2 ;;
        --frame-size) FRAME=${2:?};    shift 2 ;;
        --duration)   DURATION=${2:?}; shift 2 ;;
        --count)      COUNT=${2:?};    shift 2 ;;
        --burst)      BURST=${2:?};    shift 2 ;;
        --clone)      CLONE=${2:?};    shift 2 ;;
        --threads)    THREADS=${2:?};  shift 2 ;;
        --cpu-base)   CPU_BASE=${2:?}; shift 2 ;;
        --queue)      QUEUE=${2:?};    shift 2 ;;
        --udp-src)    UDP_SRC=${2:?};  shift 2 ;;
        --udp-dst)    UDP_DST=${2:?};  shift 2 ;;
        --rate-tol)   RATE_TOL=${2:?}; shift 2 ;;
        --kv)         FMT=kv;          shift ;;
        --dry-run)    DRY=1;           shift ;;
        --keep)       KEEP=1;          shift ;;
        -h|--help)    usage; exit 0 ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

# ------------------------------------------------------------ validate args ----
num()  { case "${2:-}" in ''|*[!0-9]*) die "$1 must be a non-negative integer, got '${2:-}'";; esac; }
fnum() { case "${2:-}" in ''|*[!0-9.]*) die "$1 must be numeric, got '${2:-}'";; esac; }

[ -n "$DEV" ]     || die "--dev is required (try --help)"
[ -n "$DST_IP" ]  || die "--dst-ip is required"
[ -n "$DST_MAC" ] || die "--dst-mac is required -- pktgen does not do ARP, so an
       omitted or wrong MAC produces traffic the receiver silently ignores, which
       would look exactly like 100% loss"
[[ $DST_MAC =~ ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ ]] || die "--dst-mac '$DST_MAC' is not a MAC address"
[[ $DST_IP  =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]     || die "--dst-ip '$DST_IP' is not an IPv4 address"
num --frame-size "$FRAME"; num --burst "$BURST"; num --clone "$CLONE"; num --threads "$THREADS"
num --udp-src "$UDP_SRC"; num --udp-dst "$UDP_DST"; fnum --rate-tol "$RATE_TOL"
[ "$FRAME" -ge 64 ] && [ "$FRAME" -le 9014 ] || die "--frame-size must be 64..9014 (got $FRAME)"
[ "$BURST" -ge 1 ]   || die "--burst must be >= 1"
[ "$THREADS" -ge 1 ] || die "--threads must be >= 1"
[ -n "$PPS" ] || [ -n "$DELAY_NS" ] || die "one of --pps / --delay-ns is required"
[ -z "$PPS" ] || num --pps "$PPS"
[ -z "$DELAY_NS" ] || num --delay-ns "$DELAY_NS"
[ -n "$COUNT" ] || [ -n "$DURATION" ] || die "one of --count / --duration is required"
if [ -n "$DURATION" ]; then
    fnum --duration "$DURATION"
    [ -n "$PPS" ] || die "--duration needs --pps to derive the packet count; use --count with --delay-ns"
fi

if [ -n "$CPU_BASE" ]; then num --cpu-base "$CPU_BASE"; fi
if [ -n "$QUEUE" ];    then num --queue    "$QUEUE";    fi

# pktgen's `pkt_size` excludes the 4-byte CRC and includes the 14-byte Ethernet
# header, i.e. it is exactly our --frame-size.
PKT_SIZE=$FRAME

# ---- delay: absolute-schedule inter-transmit gap, compensated for burst+threads.
if [ -z "$DELAY_NS" ]; then
    [ "$PPS" -gt 0 ] || die "--pps must be > 0"
    DELAY_NS=$(awk -v p="$PPS" -v b="$BURST" -v t="$THREADS" \
        'BEGIN{ d = 1e9 * b * t / p; printf "%d", (d < 1 ? 1 : int(d + 0.5)) }')
    EFF_PPS=$(awk -v d="$DELAY_NS" -v b="$BURST" -v t="$THREADS" 'BEGIN{printf "%.1f", 1e9*b*t/d}')
else
    EFF_PPS=$(awk -v d="$DELAY_NS" -v b="$BURST" -v t="$THREADS" 'BEGIN{printf "%.1f", 1e9*b*t/d}')
    PPS=${PPS:-$(printf '%.0f' "$EFF_PPS")}
fi
# ---- count: exact packet budget, split evenly across threads.
if [ -z "$COUNT" ]; then
    COUNT=$(awk -v p="$PPS" -v s="$DURATION" 'BEGIN{printf "%d", int(p*s + 0.5)}')
fi
[ "$COUNT" -gt 0 ] || die "computed packet count is 0"
PER_THREAD=$(( COUNT / THREADS ))
[ "$PER_THREAD" -gt 0 ] || die "--count $COUNT split over $THREADS threads gives 0 packets per thread"
TOTAL=$(( PER_THREAD * THREADS ))
EXP_SEC=$(awk -v n="$TOTAL" -v p="$EFF_PPS" 'BEGIN{printf "%.3f", n/p}')

[[ $DEV =~ ^[A-Za-z0-9._-]{1,15}$ ]] || die "--dev '$DEV' is not a plausible interface name"

# ------------------------------------------------------------- privilege ----
# Two supported access paths to the root-only /proc/net/pktgen:
#   VIA=direct  we are uid 0
#   VIA=helper  /usr/local/sbin/pktgen-proc-write is installed root-owned and
#               whitelisted NOPASSWD (see pktgen-sudoers.example)
HELPER=/usr/local/sbin/pktgen-proc-write
VIA=none
if [ "$DRY" = 0 ]; then
    if [ "$(id -u)" = 0 ]; then
        VIA=direct
        if [ ! -d "$PG" ]; then
            modprobe pktgen 2>/dev/null || die "pktgen module could not be loaded (modprobe pktgen failed)"
            [ -d "$PG" ] || die "pktgen loaded but $PG does not exist"
        fi
        [ -w "$PG/pgctrl" ] || die "$PG/pgctrl is not writable even as uid 0"
    elif [ -x "$HELPER" ] && sudo -n "$HELPER" -l >/dev/null 2>&1; then
        VIA=helper
    else
        cat >&2 <<EOF
$PROG: FATAL: cannot reach /proc/net/pktgen.

/proc/net/pktgen/* is created by the kernel as -rw------- root:root, so an
unprivileged process can neither read nor write it. 'sudo -n modprobe pktgen' IS
already permitted on this host, but loading the module without being able to
configure it is useless, and none of sudo sh/tee/dd/bash is in the NOPASSWD set.

Choose one:

  (a) run this script as root, interactively:
        sudo $0 $*

  (b) install the narrow root helper once, then this script works unprivileged
      and the harness can drive it over ssh:
        TOOLS=$(cd -- "$(dirname -- "$0")" && pwd)
        sudo install -o root -g root -m 0755 \$TOOLS/pktgen-proc-write /usr/local/sbin/
        sudo install -o root -g root -m 0440 \$TOOLS/pktgen-sudoers.example \\
             /etc/sudoers.d/onic-pktgen
        sudo visudo -cf /etc/sudoers.d/onic-pktgen

  (c) use the no-privilege generator instead:
        $(cd -- "$(dirname -- "$0")" && pwd)/udp-pacer-sender.sh --help
EOF
        exit 3
    fi
    [ -d "/sys/class/net/$DEV" ] || die "no such netdev on this host: $DEV"
    if [ "$(cat "/sys/class/net/$DEV/operstate" 2>/dev/null)" != up ]; then
        die "$DEV is not up"
    fi
    NQ=$(ls -d "/sys/class/net/$DEV/queues/tx-"* 2>/dev/null | wc -l)
    if [ "$NQ" -gt 0 ] && [ "$THREADS" -gt "$NQ" ]; then
        die "--threads $THREADS exceeds $DEV's $NQ tx queues"
    fi
fi
LOADED_HERE=${LOADED_HERE:-0}

# ------------------------------------------------------- proc read/write ----
pgw() { # pgw <name-under-/proc/net/pktgen> <value...>
    local n=$1; shift
    local v="$*"
    if [ "$DRY" = 1 ]; then printf 'echo %-42s > %s\n' "\"$v\"" "$PG/$n"; return 0; fi
    case "$VIA" in
        direct) printf '%s\n' "$v" > "$PG/$n" 2>/dev/null ||
                    die "write failed: echo '$v' > $PG/$n (pktgen rejected the value)" ;;
        helper) sudo -n "$HELPER" "$n" $v ||
                    die "write failed via $HELPER: '$n' <- '$v'" ;;
        *)      die "internal: no access path to $PG" ;;
    esac
}

pgr() { # pgr <name>  -- print the contents of a pktgen proc file
    local n=$1
    case "$VIA" in
        direct) cat -- "$PG/$n" ;;
        helper) sudo -n "$HELPER" -r "$n" ;;
        *)      die "internal: no access path to $PG" ;;
    esac
}

pgexists() { # pgexists <name>
    if [ "$DRY" = 1 ]; then return 0; fi
    case "$VIA" in
        direct) [ -e "$PG/$1" ] ;;
        helper) sudo -n "$HELPER" -r "$1" >/dev/null 2>&1 ;;
        *) return 1 ;;
    esac
}

THREAD_CPUS=()
for i in $(seq 0 $((THREADS-1))); do
    if [ -n "$CPU_BASE" ]; then THREAD_CPUS+=( $(( CPU_BASE + i )) ); else THREAD_CPUS+=( "$i" ); fi
done

cleanup() {
    if [ "$DRY" = 1 ] || [ "$KEEP" = 1 ]; then return 0; fi
    for c in "${THREAD_CPUS[@]}"; do
        pgw "kpktgend_$c" rem_device_all 2>/dev/null || true
    done
}

configure() {
    pgw pgctrl reset
    local i c dkey
    for i in $(seq 0 $((THREADS-1))); do
        c=${THREAD_CPUS[$i]}
        pgexists "kpktgend_$c" || die "no pktgen thread for CPU $c (kpktgend_$c missing; is --cpu-base too high for this machine?)"
        pgw "kpktgend_$c" rem_device_all
        pgw "kpktgend_$c" "add_device ${DEV}@${i}"
        dkey="${DEV}@${i}"
        pgexists "$dkey" || die "add_device ${DEV}@${i} did not create $PG/$dkey"

        pgw "$dkey" "count $PER_THREAD"        # EXACT packet budget per thread
        pgw "$dkey" "clone_skb $CLONE"
        pgw "$dkey" "pkt_size $PKT_SIZE"
        pgw "$dkey" "delay $DELAY_NS"          # absolute-schedule pacing
        pgw "$dkey" "burst $BURST"
        pgw "$dkey" "dst $DST_IP"
        pgw "$dkey" "dst_mac $DST_MAC"
        if [ -n "$SRC_IP" ]; then
            pgw "$dkey" "src_min $SRC_IP"
            pgw "$dkey" "src_max $SRC_IP"
        fi
        pgw "$dkey" "udp_src_min $((UDP_SRC + i))"
        pgw "$dkey" "udp_src_max $((UDP_SRC + i))"
        pgw "$dkey" "udp_dst_min $UDP_DST"
        pgw "$dkey" "udp_dst_max $UDP_DST"
        local q=${QUEUE:-$i}
        pgw "$dkey" "queue_map_min $q"
        pgw "$dkey" "queue_map_max $q"
    done
}

parse_results() {
    # pktgen result line looks like:
    #  Result: OK: 10000123(c9999000+d1123) usec, 1000000 (9014byte,0frags)
    #    99998pps 7211Mb/sec (7211856064bps) errors: 0
    local total_pkts=0 total_pps=0 max_us=0 errs=0 i c dkey line
    for i in $(seq 0 $((THREADS-1))); do
        dkey="${DEV}@${i}"
        line=$(pgr "$dkey" 2>/dev/null | grep -m1 '^Result:' || true)
        [ -n "$line" ] || die "no Result line in $PG/$dkey -- the run did not complete"
        case "$line" in
            *"Result: OK:"*) : ;;
            *) die "pktgen reported a failure for ${DEV}@${i}: $line" ;;
        esac
        # Result: OK: <elapsed_us>(c<us>+d<us>) usec, <pkts> (<size>byte,Nfrags) <pps>pps <Mb/sec> ... errors: <n>
        eval "$(printf '%s\n' "$line" | awk '
            {
              us=0; pkts=0; pps=0; err=0
              if (match($0, /OK: [0-9]+/))            { us   = substr($0, RSTART+4, RLENGTH-4) + 0 }
              if (match($0, /[0-9]+pps/))             { pps  = substr($0, RSTART,   RLENGTH-3) + 0 }
              if (match($0, /errors: [0-9]+/))        { err  = substr($0, RSTART+8, RLENGTH-8) + 0 }
              if (match($0, /[0-9]+ \([0-9]+byte/))   { s = substr($0, RSTART, RLENGTH); sub(/ \(.*/, "", s); pkts = s + 0 }
              printf("R_US=%d R_PKTS=%d R_PPS=%d R_ERR=%d\n", us, pkts, pps, err)
            }')"
        [ "${R_PKTS:-0}" -gt 0 ] || die "could not parse a packet count out of ${DEV}@${i}: $line"
        total_pkts=$(( total_pkts + R_PKTS ))
        total_pps=$(( total_pps + R_PPS ))
        errs=$(( errs + R_ERR ))
        if [ "$R_US" -gt "$max_us" ]; then max_us=$R_US; fi
    done
    ACH_PKTS=$total_pkts
    ACH_PPS=$total_pps
    ACH_ERR=$errs
    ACH_SEC=$(awk -v u="$max_us" 'BEGIN{printf "%.6f", u/1e6}')
}

# --------------------------------------------------------------------- run ----
if [ "$DRY" = 1 ]; then
    printf '# --dry-run: the following would be written to %s\n' "$PG"
    configure
    pgw pgctrl start
    printf '# derived: delay=%s ns  per-thread count=%s  total=%s  effective target=%s pps  expected run=%s s\n' \
        "$DELAY_NS" "$PER_THREAD" "$TOTAL" "$EFF_PPS" "$EXP_SEC"
    exit 0
fi

trap cleanup EXIT
configure

# `echo start > pgctrl` blocks until every device has finished its count. Guard it
# with a timeout so a wedged tx queue cannot hang the harness forever.
GUARD=$(awk -v s="$EXP_SEC" 'BEGIN{printf "%d", (s*2 + 15)}')
START_RC=0
timeout "$GUARD" bash -c 'PG=$1; VIA=$2; HELPER=$3
    if [ "$VIA" = direct ]; then printf "start\n" > "$PG/pgctrl"
    else sudo -n "$HELPER" pgctrl start; fi' _ "$PG" "$VIA" "$HELPER" || START_RC=$?
if [ "$START_RC" != 0 ]; then
    pgw pgctrl stop 2>/dev/null || true
    die "pktgen run did not finish within ${GUARD}s (expected ${EXP_SEC}s), or the start write failed (rc=$START_RC) -- tx queue stalled?"
fi

parse_results

DEV_TOL_OK=1
if awk -v t="$RATE_TOL" 'BEGIN{exit !(t+0 > 0)}'; then
    DEV_PCT=$(awk -v a="$ACH_PPS" -v e="$EFF_PPS" 'BEGIN{printf "%.4f", (e>0 ? 100*(a-e)/e : 0)}')
    if awk -v d="$DEV_PCT" -v t="$RATE_TOL" 'BEGIN{d=(d<0?-d:d); exit !(d > t+0)}'; then
        DEV_TOL_OK=0
    fi
else
    DEV_PCT=$(awk -v a="$ACH_PPS" -v e="$EFF_PPS" 'BEGIN{printf "%.4f", (e>0 ? 100*(a-e)/e : 0)}')
fi

if [ "$FMT" = kv ]; then
    printf 'gen=pktgen dev=%s target_pps=%s effective_target_pps=%s delay_ns=%s burst=%s clone=%s threads=%s frame=%s count=%s sent_pkts=%s achieved_pps=%s run_s=%s rate_dev_pct=%s errors=%s rate_ok=%s\n' \
        "$DEV" "$PPS" "$EFF_PPS" "$DELAY_NS" "$BURST" "$CLONE" "$THREADS" "$FRAME" \
        "$TOTAL" "$ACH_PKTS" "$ACH_PPS" "$ACH_SEC" "$DEV_PCT" "$ACH_ERR" "$DEV_TOL_OK"
else
    cat <<EOF
pktgen run on $DEV -> $DST_IP ($DST_MAC)
  frame size            : $FRAME B (excl CRC)
  burst / clone_skb     : $BURST / $CLONE
  threads               : $THREADS (cpus: ${THREAD_CPUS[*]})
  delay (per transmit)  : $DELAY_NS ns
  target average rate   : $PPS pps  (exactly representable: $EFF_PPS pps)
  packet budget         : $TOTAL ($PER_THREAD per thread)
  ---- measured by pktgen ----
  packets sent          : $ACH_PKTS
  elapsed               : $ACH_SEC s
  achieved rate         : $ACH_PPS pps  (deviation ${DEV_PCT} %)
  pktgen errors         : $ACH_ERR
EOF
fi

if [ "$ACH_PKTS" != "$TOTAL" ]; then
    die "pktgen sent $ACH_PKTS packets but the budget was $TOTAL -- run is not a valid measurement"
fi
if [ "$ACH_ERR" != 0 ]; then
    printf '%s: FATAL: pktgen reported %s tx errors; the offered load is not what was requested\n' "$PROG" "$ACH_ERR" >&2
    exit 5
fi
if [ "$DEV_TOL_OK" = 0 ]; then
    printf '%s: FATAL: achieved %s pps vs target %s pps (%s %%), outside --rate-tol %s %%.\n' \
        "$PROG" "$ACH_PPS" "$EFF_PPS" "$DEV_PCT" "$RATE_TOL" >&2
    printf '%s:        The sender could not hold the requested rate. DISCARD this run; do not\n' "$PROG" >&2
    printf '%s:        average it in. Lower --pps, raise --clone, or add --threads.\n' "$PROG" >&2
    exit 4
fi
exit 0
