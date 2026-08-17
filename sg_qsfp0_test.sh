#!/bin/bash
# SG TX datapath test over QSFP0 -> remote ConnectX-6 (10.42.0.180).
# Runs ON .223 as user alex; privileged ops are individually sudo-prefixed
# (whitelisted in /etc/sudoers.d/onic-test). FPGA enp1s0 is the iperf3 client,
# so FPGA TX -- and thus the scatter-gather path -- is exercised.

FPGA=enp1s0
FPGA_IP=10.42.0.1
PEER=10.42.0.180
DUR=15
SSHKEYDIR=/home/alex/.ssh
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5"

s0() { cat /sys/class/net/$1/statistics/$2 2>/dev/null || echo 0; }

# ---- configure FPGA side (idempotent) ----
sudo ip addr flush dev $FPGA
sudo ip addr add $FPGA_IP/24 dev $FPGA
sudo ip link set $FPGA up
sleep 2
echo "=== enp1s0 ==="; ip -br addr show $FPGA; echo "carrier=$(cat /sys/class/net/$FPGA/carrier)"

echo "=== ping peer $PEER ==="
ping -c3 -W2 $PEER 2>&1 | tail -3 || true

# ---- find a working ssh key/user for the peer ----
PEER_LOGIN=""
for key in id_rsa id_ed25519_tenstorrent; do
  for user in tenstorrent alex; do
    if $SSH -i $SSHKEYDIR/$key ${user}@${PEER} true 2>/dev/null; then
      PEER_LOGIN="-i $SSHKEYDIR/$key ${user}@${PEER}"
      echo "=== peer ssh OK: ${user}@${PEER} key=$key ==="
      break 2
    fi
  done
done
[ -z "$PEER_LOGIN" ] && echo "!! could not ssh to peer $PEER with available keys"

# peer's iface that holds $PEER (for rx_crc stats)
PEER_IF=""
[ -n "$PEER_LOGIN" ] && PEER_IF=$($SSH $PEER_LOGIN "ip -o -4 addr show | awk '/$PEER/{print \$2; exit}'" 2>/dev/null)

run_iperf() {
  local tag="$1" extra="$2"
  echo "############################################################"
  echo "## $tag"
  echo "############################################################"
  ethtool -k $FPGA | grep -E "^scatter-gather:|^generic-receive"
  [ -z "$PEER_LOGIN" ] && { echo "(no peer ssh; skipping iperf)"; return; }
  $SSH $PEER_LOGIN 'pkill -f "iperf3 -s" 2>/dev/null; (iperf3 -s -1 >/dev/null 2>&1 &)' 2>/dev/null
  sleep 1
  local txe0=$(s0 $FPGA tx_errors) txd0=$(s0 $FPGA tx_dropped)
  local rxc0=$([ -n "$PEER_IF" ] && $SSH $PEER_LOGIN "ethtool -S $PEER_IF 2>/dev/null | grep -w rx_crc_errors_phy | grep -oE '[0-9]+\$'" 2>/dev/null)
  iperf3 -c $PEER -t $DUR -O 2 $extra 2>&1 | grep -E "sender|receiver|Retr|SUM|connect|error"
  sleep 1
  local txe1=$(s0 $FPGA tx_errors) txd1=$(s0 $FPGA tx_dropped)
  local rxc1=$([ -n "$PEER_IF" ] && $SSH $PEER_LOGIN "ethtool -S $PEER_IF 2>/dev/null | grep -w rx_crc_errors_phy | grep -oE '[0-9]+\$'" 2>/dev/null)
  echo "--- deltas ---"
  echo "FPGA tx_errors: $((txe1-txe0))  tx_dropped: $((txd1-txd0))   PEER($PEER_IF) rx_crc_phy: $((${rxc1:-0}-${rxc0:-0}))"
}

sudo ethtool -K $FPGA sg on  >/dev/null 2>&1
run_iperf "RUN 1: SG ON  -- FPGA TX (client -> peer)  [SG path under test]" ""
run_iperf "RUN 2: SG ON  -- reverse (-R: peer -> FPGA RX, for contrast)" "-R"

sudo ethtool -K $FPGA sg off >/dev/null 2>&1
run_iperf "RUN 3: SG OFF -- FPGA TX (linearize fallback)" ""

sudo ethtool -K $FPGA sg on >/dev/null 2>&1
echo "=== DONE (sg restored to on) ==="
