#!/bin/bash
# SG TX datapath test.
# Path: FPGA QSFP1 (enp1s0d1) --cable--> local Mellanox 100G (enp2s0np0), both on this host.
# The mlnx is moved into a netns so TCP traffic egresses the physical cable
# (two NICs in the same subnet in the root ns would shortcut via loopback).
# FPGA is the iperf3 *client* => FPGA TX is exercised => scatter-gather path is exercised.

FPGA=enp1s0d1
MLNX=enp2s0np0
NS=nsmlnx
FPGA_IP=192.168.50.1
MLNX_IP=192.168.50.2
MTU=9000
DUR=15

s0() { cat /sys/class/net/$1/statistics/$2 2>/dev/null || echo 0; }

cleanup() {
  echo "=== cleanup ==="
  ip netns pids $NS 2>/dev/null | xargs -r kill 2>/dev/null
  ip netns del $NS 2>/dev/null || true   # returns $MLNX to root ns
  ip addr flush dev $FPGA 2>/dev/null || true
  ip link set $MLNX up 2>/dev/null || true
  ethtool -K $FPGA sg on 2>/dev/null || true
}
trap cleanup EXIT

# ---- teardown any prior run ----
ip netns del $NS 2>/dev/null || true
sleep 1

# ---- namespace + mlnx (server side) ----
ip netns add $NS
ip link set $MLNX netns $NS
ip netns exec $NS ip link set lo up
ip netns exec $NS ip link set $MLNX mtu $MTU up
ip netns exec $NS ip addr add $MLNX_IP/24 dev $MLNX

# ---- fpga (client side, root ns) ----
ip addr flush dev $FPGA
ip link set $FPGA mtu $MTU up
ip addr add $FPGA_IP/24 dev $FPGA

sleep 3
echo "=== carrier (fpga / mlnx) ==="
echo "fpga=$(cat /sys/class/net/$FPGA/carrier)  mlnx=$(ip netns exec $NS cat /sys/class/net/$MLNX/carrier)"

echo "=== jumbo connectivity (8000B, DF) ==="
ping -c3 -M do -s 8000 -W2 $MLNX_IP 2>&1 | tail -4

run_iperf() {
  local tag="$1"
  echo "############################################################"
  echo "## $tag"
  echo "############################################################"
  ethtool -k $FPGA | grep -E "^scatter-gather:|^generic-receive"
  # start server in ns
  ip netns exec $NS iperf3 -s -1 -D
  sleep 1
  local txe0=$(s0 $FPGA tx_errors) txd0=$(s0 $FPGA tx_dropped)
  local rxe0=$(ip netns exec $NS cat /sys/class/net/$MLNX/statistics/rx_errors)
  local mcrc0=$(ip netns exec $NS ethtool -S $MLNX 2>/dev/null | grep -w rx_crc_errors_phy | grep -oE '[0-9]+$')
  # FPGA is client -> its TX is under test
  iperf3 -c $MLNX_IP -t $DUR -O 2 2>&1 | grep -E "sender|receiver|retr|SUM"
  sleep 1
  local txe1=$(s0 $FPGA tx_errors) txd1=$(s0 $FPGA tx_dropped)
  local rxe1=$(ip netns exec $NS cat /sys/class/net/$MLNX/statistics/rx_errors)
  local mcrc1=$(ip netns exec $NS ethtool -S $MLNX 2>/dev/null | grep -w rx_crc_errors_phy | grep -oE '[0-9]+$')
  echo "--- deltas over run ---"
  echo "FPGA tx_errors:  $((txe1-txe0))   tx_dropped: $((txd1-txd0))"
  echo "MLNX rx_errors:  $((rxe1-rxe0))   rx_crc_phy: $((${mcrc1:-0}-${mcrc0:-0}))"
}

ethtool -K $FPGA sg on  >/dev/null 2>&1
run_iperf "RUN 1: scatter-gather ON (new code path)"

ethtool -K $FPGA sg off >/dev/null 2>&1
run_iperf "RUN 2: scatter-gather OFF (linearize fallback)"

echo "=== DONE ==="
