#!/usr/bin/env bash
#
# Install passwordless sudo for the onic bring-up / debug commands, plus two
# root-owned helpers for operations that cannot be granted safely on their own.
#
#   sudo ./tools/install-bringup-sudoers.sh            # install for $SUDO_USER
#   sudo ./tools/install-bringup-sudoers.sh --user bob
#   sudo ./tools/install-bringup-sudoers.sh --peer      # peer host (no FPGA)
#   sudo ./tools/install-bringup-sudoers.sh --uninstall
#
# --peer installs only the networking / diagnostics / tuning rules, omitting
# everything that touches the onic module or the FPGA's BARs.  Use it on the
# machines at the other end of the cables so both ends of a traffic test can be
# configured non-interactively:
#
#   scp tools/install-bringup-sudoers.sh peer:/tmp/
#   ssh -t peer 'sudo /tmp/install-bringup-sudoers.sh --peer --user alex'
#
# TRUST MODEL -- read this before installing:
#   Passwordless `insmod` of a kernel module that the target user can rewrite is
#   equivalent to passwordless root, because that user can build any module they
#   like at that path.  The same holds for the bar-read helper (raw MMIO access).
#   This is an appropriate trade-off on a single-user development bench and NOT
#   appropriate on a shared or production host.  Nothing here is a substitute for
#   restricting who can log into this machine.
#
# What is deliberately NOT granted:
#   - `sh -c` / any shell (would be a trivial root shell).  The sysfs pokes that
#     needed it live in /usr/local/sbin/onic-pci-rescan instead.
#   - `ip netns exec` (also a trivial root shell) -- only `ip link`/`ip addr`
#     patterns are allowed.
#   - `insmod` of an arbitrary path -- only this repo's onic.ko.
set -euo pipefail

SUDOERS_FILE=/etc/sudoers.d/onic-bringup
HELPER_RESCAN=/usr/local/sbin/onic-pci-rescan
HELPER_BARREAD=/usr/local/sbin/onic-bar-read
HELPER_LIBDIR=/usr/local/lib/onic

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_USER="${SUDO_USER:-}"
UNINSTALL=0
PEER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --user)      TARGET_USER="$2"; shift 2 ;;
    --peer)      PEER=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
    *)           echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

[[ $EUID -eq 0 ]] || { echo "error: run me under sudo" >&2; exit 1; }

if [[ $UNINSTALL -eq 1 ]]; then
  rm -f "$SUDOERS_FILE" "$HELPER_RESCAN" "$HELPER_BARREAD"
  rm -rf "$HELPER_LIBDIR"
  echo "removed $SUDOERS_FILE and helpers"
  exit 0
fi

[[ -n "$TARGET_USER" ]] || { echo "error: cannot determine user; pass --user" >&2; exit 1; }
id "$TARGET_USER" >/dev/null 2>&1 || { echo "error: no such user: $TARGET_USER" >&2; exit 1; }

KO="${REPO_DIR}/onic.ko"
if [[ $PEER -eq 0 ]]; then
  [[ -f "${REPO_DIR}/onic_main.c" ]] || {
    echo "error: ${REPO_DIR} is not the driver repo (use --peer on hosts without it)" >&2
    exit 1; }
fi

# Resolve tool paths.  Two subtleties:
#  - Do NOT canonicalize with `readlink -f`: sudo matches the path it resolved
#    via secure_path, so /usr/sbin/insmod must appear literally.  Resolving it
#    would yield /usr/bin/kmod (the multicall binary), which never matches
#    `sudo insmod` and would grant every kmod subcommand besides.
#  - Locations differ across distros, so emit every path that exists and let
#    sudo match whichever one it picks.
paths_for() {
  local tool="$1" d found=0
  for d in /usr/local/sbin /usr/local/bin /usr/sbin /usr/bin /sbin /bin; do
    if [[ -x "$d/$tool" ]]; then echo "$d/$tool"; found=1; fi
  done
  [[ $found -eq 1 ]] || { echo "error: $tool not found" >&2; exit 1; }
}

# Build a comma-separated Cmnd list: every existing path for $1, each with the
# argument pattern $2 appended (empty pattern = bare command).
cmnds() {
  local tool="$1" args="${2-}" p out=()
  while read -r p; do
    out+=("${p}${args:+ $args}")
  done < <(paths_for "$tool")
  local IFS=,; echo "${out[*]}"
}

if [[ $PEER -eq 1 ]]; then
  echo "== peer mode: skipping onic helpers (no FPGA on this host) =="
else
echo "== installing helpers =="
install -d -m 0755 -o root -g root "$HELPER_LIBDIR"
install -m 0755 -o root -g root "${REPO_DIR}/tools/bar_read.py" "${HELPER_LIBDIR}/bar_read.py"

cat > "$HELPER_RESCAN" <<'HELPER'
#!/usr/bin/env bash
# Detach every Xilinx (10ee) endpoint and re-enumerate the bus.  Needed after
# reconfiguring the FPGA: the kernel otherwise keeps a stale device object whose
# BARs show up as [virtual] in lspci.
#
# Unload the onic driver before calling this.
set -euo pipefail

mapfile -t BDFS < <(lspci -D -d 10ee: | awk '{print $1}')
if [[ ${#BDFS[@]} -eq 0 ]]; then
  echo "no 10ee devices present; rescanning anyway"
else
  if lsmod | grep -q '^onic'; then
    echo "warning: onic is still loaded -- removing the device under a live driver" >&2
    echo "         is unsafe; run 'sudo rmmod onic' first" >&2
    exit 1
  fi
  for b in "${BDFS[@]}"; do
    echo "removing $b"
    echo 1 > "/sys/bus/pci/devices/${b}/remove"
  done
fi

echo "rescanning PCI bus"
echo 1 > /sys/bus/pci/rescan
sleep 1
lspci -D -d 10ee: || echo "no 10ee device after rescan -- link may not have retrained"
HELPER
chmod 0755 "$HELPER_RESCAN"; chown root:root "$HELPER_RESCAN"

cat > "$HELPER_BARREAD" <<HELPER
#!/usr/bin/env bash
# Root-owned wrapper around bar_read.py so the sudo grant does not point at a
# user-writable file.
exec /usr/bin/python3 ${HELPER_LIBDIR}/bar_read.py "\$@"
HELPER
chmod 0755 "$HELPER_BARREAD"; chown root:root "$HELPER_BARREAD"
fi   # end of non-peer helper install

echo "== writing sudoers rules =="
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
cat > "$TMP" <<EOF
# onic bring-up / debug -- generated by tools/install-bringup-sudoers.sh
# Regenerate with that script; do not hand-edit.
#
# See the TRUST MODEL note in the installer: passwordless insmod of a
# user-writable module is root-equivalent.  Dev bench only.

Cmnd_Alias ONIC_DIAG   = $(cmnds dmesg), \\
                         $(cmnds dmesg "-C"), \\
                         $(cmnds dmesg "-w"), \\
                         $(cmnds dmesg "-T"), \\
                         $(cmnds ethtool "*"), \\
                         $(cmnds tcpdump "*")
Cmnd_Alias ONIC_NET    = $(cmnds ip "link set *"), \\
                         $(cmnds ip "addr *"), \\
                         $(cmnds ip "neigh *"), \\
                         $(cmnds ip "route add *"), \\
                         $(cmnds ip "route del *"), \\
                         $(cmnds ip "route show *"), \\
                         $(cmnds ip "-s link *"), \\
                         $(cmnds ip "-br link *"), \\
                         $(cmnds ip "-br addr *")
# NetworkManager hands out DHCP leases on and strips static IPs from the test
# interfaces; taking them out of NM's control is part of bring-up.
Cmnd_Alias ONIC_NM     = $(cmnds nmcli "device set * managed no"), \\
                         $(cmnds nmcli "device set * managed yes"), \\
                         $(cmnds nmcli "connection reload")
# Socket-buffer limits for high-rate iperf3 runs
Cmnd_Alias ONIC_TUNE   = $(cmnds sysctl "-w net.core.*"), \\
                         $(cmnds sysctl "-w net.ipv4.tcp_*")
Cmnd_Alias ONIC_PKG    = $(cmnds apt-get "update"), \\
                         $(cmnds apt-get "-y install iperf3")
EOF

if [[ $PEER -eq 0 ]]; then
cat >> "$TMP" <<EOF
Cmnd_Alias ONIC_MOD    = $(cmnds insmod "${KO}"), \\
                         $(cmnds insmod "${KO} debug_level=*"), \\
                         $(cmnds rmmod onic), \\
                         $(cmnds modprobe onic), \\
                         $(cmnds modprobe "-r onic")
Cmnd_Alias ONIC_BAR    = ${HELPER_BARREAD}, ${HELPER_BARREAD} *
Cmnd_Alias ONIC_PCI    = ${HELPER_RESCAN}

${TARGET_USER} ALL=(root) NOPASSWD: ONIC_MOD, ONIC_BAR, ONIC_PCI, \\
                                    ONIC_DIAG, ONIC_NET, ONIC_NM, ONIC_TUNE, ONIC_PKG
EOF
else
cat >> "$TMP" <<EOF

# Peer host: networking and diagnostics only -- no onic module, no FPGA BAR access
${TARGET_USER} ALL=(root) NOPASSWD: ONIC_DIAG, ONIC_NET, ONIC_NM, ONIC_TUNE, ONIC_PKG
EOF
fi

# Never install a file that would break sudo
if ! visudo -cqf "$TMP"; then
  echo "error: generated sudoers file failed validation; nothing installed" >&2
  visudo -cf "$TMP" || true
  exit 1
fi

install -m 0440 -o root -g root "$TMP" "$SUDOERS_FILE"
visudo -cqf /etc/sudoers || { echo "error: /etc/sudoers now invalid -- removing" >&2
                              rm -f "$SUDOERS_FILE"; exit 1; }

echo
echo "installed $SUDOERS_FILE for user '$TARGET_USER'$([[ $PEER -eq 1 ]] && echo ' (peer mode)')"
echo
echo "Passwordless now:"
if [[ $PEER -eq 0 ]]; then
  echo "  insmod/rmmod/modprobe onic   (module path pinned to ${KO})"
  echo "  $HELPER_RESCAN     # remove 10ee devices + rescan the bus"
  echo "  $HELPER_BARREAD <bdf> <offset> [-n N] [--bar N]"
fi
echo "  dmesg, ethtool, tcpdump"
echo "  ip link set / addr / neigh / route add|del|show"
echo "  nmcli device set <dev> managed no|yes, nmcli connection reload"
echo "  sysctl -w net.core.* / net.ipv4.tcp_*"
echo "  apt-get update, apt-get -y install iperf3"
echo
echo "Verify with:  sudo -l -U $TARGET_USER | tail -20"
echo "Undo with:    sudo $0 --uninstall"
