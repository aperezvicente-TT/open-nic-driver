#!/usr/bin/env bash
# Load the onic driver and verify the 1-PF / 2-CMAC dual-netdev bring-up.
# Safe to run before flashing (no device -> module loads, probe doesn't fire);
# run again after flashing + PCIe rescan to see the two netdevs appear.
#
# Usage:  [BUILD=1] [DEBUG=1] sudo -E ./load_and_verify.sh
#   BUILD=1  rebuild onic.ko first;  DEBUG=1  load with debug_level=2
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

KO=./onic.ko

echo "== 1. dependency: ib_core (onic links OFED ib_* symbols) =="
if ! lsmod | grep -q '^ib_core'; then
  echo "   ib_core not loaded — attempting modprobe ib_core"
  sudo modprobe ib_core || { echo "   ERROR: ib_core unavailable; onic insmod will fail"; exit 1; }
fi
echo "   ib_core: $(lsmod | awk '/^ib_core/{print "loaded ("$2" bytes)"}')"

echo "== 2. remove any stale onic =="
if lsmod | grep -q '^onic'; then sudo rmmod onic && echo "   removed stale onic"; else echo "   none loaded"; fi

if [[ "${BUILD:-0}" == "1" ]]; then
  echo "== build onic.ko =="; make -j"$(nproc)" || exit 1
fi
[[ -f "$KO" ]] || { echo "ERROR: $KO not found (run with BUILD=1)"; exit 1; }

echo "== 3. insmod onic.ko =="
ARGS=""; [[ "${DEBUG:-0}" == "1" ]] && ARGS="debug_level=2"
if sudo insmod "$KO" $ARGS; then echo "   INSMOD OK"; else
  echo "   INSMOD FAILED — check 'dmesg | tail' (symbol CRC mismatch => OFED/ib_core skew)"; exit 1; fi

echo "== 4. probe log =="
sudo dmesg | tail -30 | grep -iE 'onic|cmac|master PF|register_netdev|ptp|num_cmac' || true

echo "== 5. netdevs (EXPECT TWO: ...c0 and ...c1, distinct MACs) =="
mapfile -t ND < <(ls -1 /sys/class/net | grep -E '^onic')
if [[ ${#ND[@]} -eq 0 ]]; then
  echo "   no onic netdevs — expected if no FPGA flashed yet (probe didn't fire)."
else
  for n in "${ND[@]}"; do
    echo "   $n  mac=$(cat /sys/class/net/$n/address)  port=$(cat /sys/class/net/$n/dev_port 2>/dev/null)"
  done
  [[ ${#ND[@]} -eq 2 ]] && echo "   => 2 netdevs on one PF: PASS" \
                        || echo "   => got ${#ND[@]} (want 2): check dmesg / num_cmacs / bitstream"
fi
echo "== done =="
