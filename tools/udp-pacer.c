/*
 * udp-pacer.c -- constant-rate UDP packet source that needs NO PRIVILEGES.
 *
 * Purpose: be a usable replacement for iperf3 as the offered-load generator for
 * the RX-drop measurement in docs/13-flow-control-plan.md 13.10/13.11, on a
 * bench where kernel pktgen cannot be driven because /proc/net/pktgen is
 * root-only (see tools/pktgen-sender.sh --help).
 *
 * Why this is a better instrument than iperf3 -u, point by point:
 *
 *   1. EXACT PACKET BUDGET. The run ends after exactly -c packets. iperf3 ends
 *      on a wall-clock deadline, which is why its delivered count wandered over
 *      4.4-6.7 M for the same nominal offer and made the drop-% denominator
 *      unstable.
 *
 *   2. ABSOLUTE DEADLINE SCHEDULE, NOT SLEEP-AND-HOPE. Packet i is due at
 *      t0 + i*1e9/pps ns, computed from t0 in integer nanoseconds. Scheduling
 *      error therefore never accumulates: a late packet does not push the whole
 *      remaining schedule out, so the long-run average rate is exact. iperf3's
 *      pacer works from relative sleeps, which both drift and quantise to the
 *      timer tick, producing the bimodal ~380k/~545k pps behaviour reported in
 *      13.10.
 *
 *   3. BUSY-WAIT, NOT nanosleep(), FOR THE LAST STRETCH. Below --spin-ns of the
 *      deadline we spin on CLOCK_MONOTONIC. Above it we clock_nanosleep(ABSTIME)
 *      to (deadline - spin_ns). Inter-packet spacing is then set by the CPU, not
 *      by timer-slack, so the shape of the offered load is repeatable rather
 *      than a function of how loaded the sender happened to be.
 *
 *   4. IT REPORTS ITS OWN FAILURE. The program counts how many packets were sent
 *      late (deadline + --late-tol-ns already past at send time), the worst
 *      lateness, and the achieved rate. If the late fraction exceeds
 *      --max-late-frac it exits non-zero. A run in which the SENDER was the
 *      bottleneck is therefore rejected, not silently averaged into a drop
 *      statistic -- which is the specific way the iperf3 measurements went wrong.
 *
 * What it deliberately does NOT do: it is an ordinary AF_INET/SOCK_DGRAM sender,
 * so packets traverse the sender's qdisc and driver like real traffic. It is not
 * a wire-exact generator the way pktgen is; it cannot produce sub-microsecond
 * bursts on demand (use --batch for coarse micro-bursts). It needs no
 * CAP_NET_RAW, no CAP_NET_ADMIN and no root.
 *
 * Build:  gcc -O2 -Wall -Wextra -o udp-pacer udp-pacer.c
 * Output: one KEY=VALUE line per run on stdout, prefixed 'RESULT '.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETH_HDR 14
#define IP_HDR  20
#define UDP_HDR 8
#define HDR_TOTAL (ETH_HDR + IP_HDR + UDP_HDR)   /* 42 */

static volatile sig_atomic_t stop_flag = 0;
static void on_sig(int s) { (void)s; stop_flag = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Block until CLOCK_MONOTONIC reaches `deadline`: sleep while far away, then
 * busy-wait the last `spin_ns`. Returns the time actually observed at release. */
static uint64_t wait_until(uint64_t deadline, long spin_ns)
{
    uint64_t t = now_ns();
    if (deadline > t + (uint64_t)spin_ns) {
        struct timespec ts;
        uint64_t wake = deadline - (uint64_t)spin_ns;
        ts.tv_sec  = (time_t)(wake / 1000000000ull);
        ts.tv_nsec = (long)(wake % 1000000000ull);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        t = now_ns();
    }
    while (t < deadline) t = now_ns();
    return t;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "udp-pacer: FATAL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(2);
}

static void usage(void)
{
    fputs(
"Usage: udp-pacer -d DST_IP [-p PORT] -r PPS {-n COUNT | -t SECONDS} [options]\n"
"\n"
"Constant-rate UDP source. No privileges required.\n"
"\n"
"Required:\n"
"  -d, --dst IP           destination IPv4 address\n"
"  -r, --pps N            target average packet rate (packets/second)\n"
"  -n, --count N          exact number of packets to send (preferred), or\n"
"  -t, --duration SEC     run length; count is derived as pps*SEC\n"
"\n"
"Options:\n"
"  -p, --port N           destination UDP port (default 5201)\n"
"  -S, --src-ip IP        bind to this source address (pins the egress port)\n"
"  -P, --src-port N       bind to this source port (0 = ephemeral, default)\n"
"  -s, --frame-size B     Ethernet frame bytes excl. CRC, 64..9014 (default 9014;\n"
"                         payload = frame - 42). Use --payload for raw UDP bytes.\n"
"      --payload B        UDP payload bytes directly (overrides --frame-size)\n"
"  -b, --batch N          send N packets per deadline (default 1). N>1 creates a\n"
"                         deliberate micro-burst of N and divides the number of\n"
"                         deadlines by N; the AVERAGE rate stays at --pps. This is\n"
"                         the burst-depth knob analogous to pktgen's `burst`.\n"
"      --spin-ns N        busy-wait below this many ns to deadline (default 20000)\n"
"      --late-tol-ns N    a batch is 'late' past deadline+N (default: auto, one\n"
"                         whole packet slot = batch*1e9/pps ns, floor 5000)\n"
"      --max-late-frac F  burstiness guard: exit non-zero if the late fraction\n"
"                         exceeds F (default 0.10). This is NOT the rate check --\n"
"                         see --max-rate-dev. Lateness only says the load arrived\n"
"                         in a lumpier shape than requested; the achieved AVERAGE\n"
"                         rate can still be exact.\n"
"      --max-rate-dev PCT THE rate check: exit non-zero if the achieved average pps\n"
"                         deviates from --pps by more than PCT (default 1.0). This\n"
"                         is what determines the offered load, so it is tight.\n"
"      --sndbuf B         SO_SNDBUF (default 8388608)\n"
"      --warmup N         send N paced but unmeasured packets first (default 0).\n"
"                         Gets ARP, the route cache, page faults and CPU frequency\n"
"                         out of the measured window.\n"
"  -q, --quiet            only print the RESULT line\n"
"  -h, --help             this text\n"
"\n"
"Exit codes: 0 ok, 2 usage/fatal, 3 send errors occurred, 4 the sender could not\n"
"hold the requested rate (late fraction over --max-late-frac). 3 and 4 mean THE RUN\n"
"IS NOT A VALID MEASUREMENT -- discard it, do not average it in.\n",
        stderr);
}

int main(int argc, char **argv)
{
    const char *dst = NULL, *src_ip = NULL;
    int port = 5201, src_port = 0;
    long frame = 9014, payload = -1;
    double pps = 0, duration = 0;
    long long count = -1;
    long batch = 1;
    long spin_ns = 20000, late_tol = -1;   /* -1 = auto: one packet slot */
    double max_late_frac = 0.10, max_rate_dev = 1.0;
    int sndbuf = 8 * 1024 * 1024;
    long warmup = 0;
    int quiet = 0;

    static struct option lo[] = {
        {"dst",1,0,'d'}, {"port",1,0,'p'}, {"pps",1,0,'r'}, {"count",1,0,'n'},
        {"duration",1,0,'t'}, {"frame-size",1,0,'s'}, {"payload",1,0,1001},
        {"batch",1,0,'b'}, {"src-ip",1,0,'S'}, {"src-port",1,0,'P'},
        {"spin-ns",1,0,1002}, {"late-tol-ns",1,0,1003}, {"max-late-frac",1,0,1004},
        {"sndbuf",1,0,1005}, {"warmup",1,0,1006}, {"max-rate-dev",1,0,1007},
        {"quiet",0,0,'q'},
        {"help",0,0,'h'}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:p:r:n:t:s:b:S:P:qh", lo, NULL)) != -1) {
        switch (c) {
        case 'd': dst = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'r': pps = atof(optarg); break;
        case 'n': count = atoll(optarg); break;
        case 't': duration = atof(optarg); break;
        case 's': frame = atol(optarg); break;
        case 1001: payload = atol(optarg); break;
        case 'b': batch = atol(optarg); break;
        case 'S': src_ip = optarg; break;
        case 'P': src_port = atoi(optarg); break;
        case 1002: spin_ns = atol(optarg); break;
        case 1003: late_tol = atol(optarg); break;
        case 1004: max_late_frac = atof(optarg); break;
        case 1005: sndbuf = atoi(optarg); break;
        case 1006: warmup = atol(optarg); break;
        case 1007: max_rate_dev = atof(optarg); break;
        case 'q': quiet = 1; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }

    if (!dst) die("--dst is required (try --help)");
    if (pps <= 0) die("--pps must be > 0");
    if (count < 0 && duration <= 0) die("one of --count / --duration is required");
    if (count < 0) count = (long long)(pps * duration + 0.5);
    if (count <= 0) die("computed packet count is 0");
    if (batch < 1) die("--batch must be >= 1");
    if (payload < 0) {
        if (frame < 64 || frame > 9014) die("--frame-size must be 64..9014 (got %ld)", frame);
        payload = frame - HDR_TOTAL;
        if (payload < 0) payload = 0;
    } else {
        frame = payload + HDR_TOTAL;
    }
    if (payload > 65507) die("--payload %ld exceeds the UDP maximum", payload);
    if (spin_ns < 0) die("--spin-ns must be >= 0");
    if (max_rate_dev < 0) die("--max-rate-dev must be >= 0");

    /* Round count down to a whole number of batches so every deadline carries an
     * identical number of packets -- otherwise the last batch is a different
     * shape and the run is not exactly reproducible. */
    long long nbatch = count / batch;
    if (nbatch < 1) die("--count %lld is smaller than --batch %ld", count, batch);
    count = nbatch * batch;

    /* Auto late tolerance: one whole packet slot. A fixed few-microsecond tolerance
     * is meaningless at high rates, where the slot itself is only microseconds and
     * any ordinary scheduling hiccup would be counted as "late". */
    if (late_tol < 0) {
        double slot = (double)batch * 1e9 / pps;
        late_tol = (long)(slot > 5000.0 ? slot : 5000.0);
    }

    /* Socket ---------------------------------------------------------------- */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket: %s", strerror(errno));
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf) < 0)
        die("SO_SNDBUF: %s", strerror(errno));
    /* Never fragment: a silently fragmented 9000 B datagram would change the
     * packet count on the wire and corrupt the drop-% denominator. */
    int pmtu = IP_PMTUDISC_DO;
    if (setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu) < 0)
        die("IP_MTU_DISCOVER: %s", strerror(errno));

    if (src_ip || src_port) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)src_port);
        sa.sin_addr.s_addr = src_ip ? inet_addr(src_ip) : htonl(INADDR_ANY);
        if (src_ip && sa.sin_addr.s_addr == INADDR_NONE) die("bad --src-ip '%s'", src_ip);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
            die("bind %s:%d: %s", src_ip ? src_ip : "*", src_port, strerror(errno));
    }
    struct sockaddr_in da;
    memset(&da, 0, sizeof da);
    da.sin_family = AF_INET;
    da.sin_port = htons((uint16_t)port);
    da.sin_addr.s_addr = inet_addr(dst);
    if (da.sin_addr.s_addr == INADDR_NONE) die("bad --dst '%s'", dst);
    if (connect(fd, (struct sockaddr *)&da, sizeof da) < 0)
        die("connect %s:%d: %s", dst, port, strerror(errno));

    /* Buffers: one per batch slot, each stamped with a 64-bit sequence number so a
     * receiver can later be taught to detect reordering/duplication. */
    unsigned char **buf = calloc((size_t)batch, sizeof *buf);
    struct mmsghdr *msgs = calloc((size_t)batch, sizeof *msgs);
    struct iovec *iov = calloc((size_t)batch, sizeof *iov);
    if (!buf || !msgs || !iov) die("out of memory");
    for (long i = 0; i < batch; i++) {
        buf[i] = malloc((size_t)payload ? (size_t)payload : 1);
        if (!buf[i]) die("out of memory");
        memset(buf[i], 0x5a, (size_t)payload);
        iov[i].iov_base = buf[i];
        iov[i].iov_len = (size_t)payload;
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* Warmup: get the route cache, ARP entry, page faults and CPU frequency out
     * of the measured window. NOT counted in the result -- but it IS paced at the
     * target rate. An unpaced warmup would be a line-rate blast of `warmup`
     * packets arriving just before the measured window, i.e. exactly the kind of
     * uncontrolled burst this generator exists to eliminate; at 9 kB frames a
     * 2000-packet unpaced warmup is an 18 MB slug that can itself cause drops.
     * The receiving harness brackets its counters around warmup + run, so these
     * packets do appear in the offered count -- as a fixed constant, which keeps
     * the count deterministic across repetitions. */
    {
        uint64_t w0 = now_ns();
        for (long i = 0; i < warmup; i++) {
            wait_until(w0 + (uint64_t)((double)i * 1e9 / pps + 0.5), spin_ns);
            if (send(fd, buf[0], (size_t)payload, 0) < 0) {
                if (errno == ECONNREFUSED || errno == ECONNRESET || errno == EAGAIN)
                    continue;
                die("warmup send: %s", strerror(errno));
            }
        }
    }

    const uint64_t period_num = 1000000000ull;     /* ns per second */
    long long sent = 0, late = 0, errs = 0, eagain = 0, refused = 0;
    uint64_t worst_late = 0, sum_late = 0;

    uint64_t t0 = now_ns();
    for (long long b = 0; b < nbatch && !stop_flag; b++) {
        /* Absolute deadline of the first packet of batch b. Integer math from t0,
         * so no drift: (b*batch) * 1e9 / pps. */
        uint64_t off = (uint64_t)(((double)(b * batch) * (double)period_num) / pps + 0.5);
        uint64_t deadline = t0 + off;

        uint64_t t = wait_until(deadline, spin_ns);

        if (t > deadline + (uint64_t)late_tol) {
            uint64_t l = t - deadline;
            late++;
            sum_late += l;
            if (l > worst_late) worst_late = l;
        }

        /* Stamp sequence numbers, then hand the whole batch over in one syscall. */
        for (long i = 0; i < batch; i++) {
            if (payload >= 8) {
                uint64_t seq = (uint64_t)(b * batch + i);
                memcpy(buf[i], &seq, sizeof seq);
            }
        }
        long done = 0;
        while (done < batch) {
            int n = sendmmsg(fd, msgs + done, (unsigned)(batch - done), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == ENOBUFS || errno == EINTR) { eagain++; continue; }
                if (errno == ECONNREFUSED || errno == ECONNRESET) {
                    /* Asynchronous ICMP port-unreachable. Retry: the packet budget
                     * must stay exact or the drop-%% denominator moves. */
                    refused++; continue;
                }
                errs++;
                fprintf(stderr, "udp-pacer: sendmmsg: %s\n", strerror(errno));
                break;
            }
            done += n;
            sent += n;
        }
    }
    uint64_t t1 = now_ns();

    double wall = (double)(t1 - t0) / 1e9;
    double ach = wall > 0 ? (double)sent / wall : 0;
    double late_frac = nbatch > 0 ? (double)late / (double)nbatch : 0;
    double gbps = wall > 0 ? (double)sent * (double)(frame + 4 + 20) * 8.0 / wall / 1e9 : 0;
    /* frame + 4 CRC + 20 preamble/IFG = wire bytes per packet */

    if (!quiet) {
        fprintf(stderr,
            "udp-pacer: %s:%d  frame=%ldB payload=%ldB batch=%ld\n"
            "  target      : %.1f pps   budget %lld packets (%lld deadlines)\n"
            "  sent        : %lld packets in %.6f s -> %.1f pps  (%.3f Gb/s on the wire)\n"
            "  rate error  : %+.4f %%\n"
            "  late batches: %lld / %lld (%.4f %%) past deadline+%ld ns; worst %.1f us, mean-late %.1f us\n"
            "  send errors : %lld   retries(EAGAIN/ENOBUFS): %lld   icmp-refused: %lld\n",
            dst, port, frame, payload, batch,
            pps, count, nbatch,
            sent, wall, ach, gbps,
            pps > 0 ? 100.0 * (ach - pps) / pps : 0.0,
            late, nbatch, 100.0 * late_frac, late_tol,
            (double)worst_late / 1000.0,
            late ? (double)sum_late / (double)late / 1000.0 : 0.0,
            errs, eagain, refused);
    }

    printf("RESULT gen=udp-pacer dst=%s port=%d frame=%ld payload=%ld batch=%ld"
           " target_pps=%.1f budget_pkts=%lld sent_pkts=%lld run_s=%.6f achieved_pps=%.1f"
           " rate_dev_pct=%+.4f late_batches=%lld late_frac=%.6f late_tol_ns=%ld worst_late_ns=%llu"
           " send_errors=%lld retries=%lld icmp_refused=%lld gbps=%.4f\n",
           dst, port, frame, payload, batch, pps, count, sent, wall, ach,
           pps > 0 ? 100.0 * (ach - pps) / pps : 0.0,
           late, late_frac, late_tol, (unsigned long long)worst_late, errs, eagain, refused, gbps);
    fflush(stdout);

    if (errs > 0) {
        fprintf(stderr, "udp-pacer: FATAL: %lld send errors -- the offered load is not"
                        " what was requested; DISCARD this run\n", errs);
        return 3;
    }
    if (sent != count) {
        fprintf(stderr, "udp-pacer: FATAL: sent %lld of %lld packets (interrupted?);"
                        " DISCARD this run\n", sent, count);
        return 3;
    }
    double rate_dev = pps > 0 ? 100.0 * (ach - pps) / pps : 0.0;
    if (rate_dev < 0) rate_dev = -rate_dev;
    if (rate_dev > max_rate_dev) {
        fprintf(stderr, "udp-pacer: FATAL: achieved %.1f pps vs target %.1f pps"
                        " (%.4f %% off, limit %.4f %%). The SENDER could not hold the\n"
                        "           requested rate. DISCARD this run -- do not average it into a\n"
                        "           drop statistic. Use more parallel senders, a larger --batch,\n"
                        "           or a lower --pps.\n",
                ach, pps, rate_dev, max_rate_dev);
        return 4;
    }
    if (late_frac > max_late_frac) {
        fprintf(stderr, "udp-pacer: FATAL: %.4f %% of deadlines missed by more than"
                        " %ld ns (limit %.4f %%).\n"
                        "           The average rate was held, but the load arrived lumpier than\n"
                        "           requested, so the burst shape is not the one configured.\n"
                        "           DISCARD this run, or raise --max-late-frac if a lumpier shape\n"
                        "           is acceptable for what you are measuring.\n",
                100.0 * late_frac, late_tol, 100.0 * max_late_frac);
        return 4;
    }
    return 0;
}
