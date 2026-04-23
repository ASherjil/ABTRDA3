#!/usr/bin/env bash
# test_rebind.sh — standalone unbind/rebind cycle for GEM0 on mkdev50
#
# Run from /dev/shm as root:
#   sudo cp test_rebind.sh /dev/shm/ && cd /dev/shm && sudo ./test_rebind.sh
set -uo pipefail

DEVICE="ff0b0000.ethernet"
DRIVER_PATH="/sys/bus/platform/drivers/macb"
IFACE="end0"
PAUSE="${1:-3}"

NFS_SERVER="cs-ccr-felab"
NFS_USR_LOCAL="${NFS_SERVER}:/data/dsc/lab/debian/12/aarch64"
KVER="$(uname -r)"

export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/lib:/usr/lib/aarch64-linux-gnu:/usr/lib

ts() { date '+%H:%M:%S.%N' | cut -c1-12; }

echo "=== GEM0 unbind/rebind test ($(ts)) ==="
echo "Device: $DEVICE   Interface: $IFACE   Pause: ${PAUSE}s"
echo

echo "Pre-warming binaries..."
ip link show "$IFACE"       > /dev/null 2>&1 || true
ip -4 -o addr show "$IFACE" > /dev/null 2>&1 || true
ip route show                > /dev/null 2>&1 || true
networkctl status "$IFACE"  > /dev/null 2>&1 || true
mount --version             > /dev/null 2>&1 || true
umount --version            > /dev/null 2>&1 || true
systemctl --version         > /dev/null 2>&1 || true
dmesg --version             > /dev/null 2>&1 || true
grep --version              > /dev/null 2>&1 || true
timeout --version           > /dev/null 2>&1 || true
echo "  done"
echo

# ── Capture network config BEFORE unbind (while everything works) ────
echo "--- Capturing network config before unbind ---"
# e.g. "10.11.33.71/24"
SAVED_ADDR=$(ip -4 -o addr show "$IFACE" 2>/dev/null \
    | grep -oP 'inet \K[\d./]+' | head -1)
# e.g. "10.11.33.1"
SAVED_GW=$(ip -4 route show default dev "$IFACE" 2>/dev/null \
    | grep -oP 'via \K[\d.]+' | head -1)
SAVED_MAC=$(cat /sys/class/net/"$IFACE"/address 2>/dev/null)

echo "  addr=$SAVED_ADDR  gw=$SAVED_GW  mac=$SAVED_MAC"

if [ -z "$SAVED_ADDR" ] || [ -z "$SAVED_GW" ]; then
    echo "  ERROR: could not capture IP/gateway — aborting"
    exit 1
fi
echo

# ── Pre-unbind snapshot ──────────────────────────────────────────────
echo "--- Pre-unbind state ($(ts)) ---"
echo "NFS mounts:"
mount -t nfs,nfs4 2>/dev/null | while read line; do echo "  $line"; done
echo "Shell test:"
timeout 3 ls /usr/local/bin/ > /dev/null 2>&1 && echo "  /usr/local: OK" || echo "  /usr/local: FAILED"
timeout 3 ls /nfs/cs-ccr-nfshome/user/ > /dev/null 2>&1 && echo "  NFS home:   OK" || echo "  NFS home:   FAILED"
echo

# ── Stop monitoring daemons (while NFS still works) ──────────────────
# fec-check-ethernet-speed does ethtool on every interface every 5 min.
# If it races with macb_probe during rebind → kernel NULL deref in
# phylink_ethtool_ksettings_get (phydev not yet attached).
echo ">>> $(ts) Stopping monitoring daemons..."
for svc in collectd fec-check-ethernet-speed; do
    timeout 5 systemctl stop "$svc" 2>&1 && echo "    $svc stopped" || echo "    $svc skip/timeout"
done

# ── Stop networkd cleanly BEFORE unbind (while it can still respond) ─
echo ">>> $(ts) Stopping systemd-networkd (while healthy)..."
timeout 5 systemctl stop systemd-networkd 2>&1 \
    && echo "    stopped" || echo "    skip/timeout"
echo

# ── Unbind ───────────────────────────────────────────────────────────
echo ">>> $(ts) Unbinding macb from $DEVICE..."
echo "$DEVICE" > "$DRIVER_PATH/unbind"
echo "    rc=$?"

# ════════════════════════════════════════════════════════════════════════
# DANGER ZONE: NFS is dead. No netlink between unbind and rebind.
# ════════════════════════════════════════════════════════════════════════

sleep 1
echo "--- Post-unbind ---"
[ -d /sys/class/net/"$IFACE" ] \
    && echo "    interface still exists (unexpected)" \
    || echo "    interface gone (expected)"
echo

echo ">>> $(ts) Waiting ${PAUSE}s before rebind..."
sleep "$PAUSE"

# ── Rebind ───────────────────────────────────────────────────────────
echo ">>> $(ts) Rebinding macb to $DEVICE..."
echo "$DEVICE" > "$DRIVER_PATH/bind"
echo "    rc=$?"

echo ">>> $(ts) Waiting for $IFACE to appear..."
for i in $(seq 1 15); do
    if [ -d /sys/class/net/"$IFACE" ]; then
        echo "    $IFACE appeared after ${i}s"
        break
    fi
    sleep 1
done
[ -d /sys/class/net/"$IFACE" ] || { echo "    FATAL: $IFACE never appeared"; exit 1; }

echo "--- Post-rebind dmesg ---"
timeout 3 dmesg 2>/dev/null | grep -i macb | tail -5 | while read line; do echo "  $line"; done
echo

# ── Network recovery: MANUAL (bypass networkd entirely) ──────────────
# systemd-networkd is hung after losing the interface. Don't bother
# trying to restart it — just configure the interface ourselves.
echo ">>> $(ts) Manual network recovery (bypassing networkd)..."

echo "  [1/5] Bringing $IFACE up..."
timeout 3 ip link set "$IFACE" up 2>&1 && echo "    OK" || echo "    FAILED"

echo "  [2/5] Assigning saved address $SAVED_ADDR..."
timeout 3 ip addr add "$SAVED_ADDR" dev "$IFACE" 2>&1 && echo "    OK" || echo "    FAILED (maybe already set)"

echo "  [3/5] Adding default route via $SAVED_GW..."
timeout 3 ip route add default via "$SAVED_GW" dev "$IFACE" 2>&1 && echo "    OK" || echo "    FAILED (maybe already set)"

# Wait for PHY link-up (carrier detect)
echo "  [4/5] Waiting for PHY link-up..."
for i in $(seq 1 20); do
    CARRIER=$(cat /sys/class/net/"$IFACE"/carrier 2>/dev/null)
    if [ "$CARRIER" = "1" ]; then
        echo "    Link up after ${i}s"
        break
    fi
    sleep 1
done
[ "$CARRIER" = "1" ] || echo "    WARNING: no carrier after 20s"

# Verify connectivity with a ping to the gateway
echo "  [5/5] Ping gateway $SAVED_GW..."
timeout 5 ping -c 2 -W 2 "$SAVED_GW" > /dev/null 2>&1 \
    && echo "    Gateway reachable!" \
    || echo "    Gateway unreachable (may need more time for STP)"

echo
echo "IPv4 after manual config:"
timeout 3 ip -4 -o addr show "$IFACE" 2>/dev/null | sed 's/^/  /' || true
echo

# ── NFS recovery ─────────────────────────────────────────────────────
echo ">>> $(ts) NFS recovery..."

echo "  [1/3] Waiting for /usr/local NFS auto-recover (up to 90s)..."
echo "         (hard mount retries on its own once TCP path works)"
NFS_OK=0
for i in $(seq 1 90); do
    if timeout 2 stat /usr/local/bin > /dev/null 2>&1; then
        echo "    /usr/local recovered after ${i}s"
        NFS_OK=1
        break
    fi
    if [ $((i % 15)) -eq 0 ]; then
        echo "    ... still waiting (${i}s)"
    fi
    sleep 1
done

if [ "$NFS_OK" -eq 0 ]; then
    echo "    /usr/local did NOT auto-recover — forcing remount..."
    timeout 10 systemctl stop "usr-local-lib-modules-*.mount" 2>&1 || true
    timeout 5 umount -l /usr/local 2>&1 || true
    sleep 1
    timeout 10 mount -overs=3,noatime,nodev,ro "$NFS_USR_LOCAL" /usr/local 2>&1 \
        && echo "    Remounted /usr/local" || echo "    FAILED to remount"
    DRIVERS_DEPMOD="/usr/local/lib/modules/${KVER}"
    if timeout 3 test -d "$DRIVERS_DEPMOD" 2>/dev/null; then
        USERDIR="/run/fec/overlayfs/userdir/${DRIVERS_DEPMOD}"
        WORKDIR="/run/fec/overlayfs/workdir/${DRIVERS_DEPMOD}"
        mkdir -p "$USERDIR" "$WORKDIR" 2>/dev/null || true
        timeout 5 mount -t overlay \
            -o"lowerdir=${DRIVERS_DEPMOD},upperdir=${USERDIR},workdir=${WORKDIR}" \
            none "$DRIVERS_DEPMOD" 2>&1 \
            && echo "    Overlay restored" || echo "    Overlay failed (non-critical)"
    fi
fi

echo "  [2/3] Restarting remote-fs.target..."
timeout 20 systemctl restart remote-fs.target 2>&1 \
    && echo "    OK" || echo "    FAILED"

echo "  [3/3] Restarting systemd-networkd (for long-term DHCP)..."
timeout 10 systemctl restart systemd-networkd 2>&1 \
    && echo "    OK" || echo "    timeout (IP is static, DHCP renewal will fail eventually)"

echo "  Restarting monitoring daemons..."
for svc in collectd fec-check-ethernet-speed; do
    timeout 5 systemctl start "$svc" 2>&1 && echo "    $svc OK" || echo "    $svc skip"
done

# ── Final verification ───────────────────────────────────────────────
echo
echo "=== Final state ($(ts)) ==="
echo
echo "IPv4:"
timeout 3 ip -4 -o addr show "$IFACE" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
echo
echo "Route:"
timeout 3 ip route show 2>/dev/null | sed 's/^/  /' || true
echo
echo "NFS mounts:"
timeout 3 mount -t nfs,nfs4 2>/dev/null | while read line; do echo "  $line"; done
echo
echo "Shell tests:"
timeout 5 ls /usr/local/bin/ > /dev/null 2>&1 \
    && echo "  /usr/local:  OK" || echo "  /usr/local:  HUNG or missing"
timeout 5 ls /nfs/cs-ccr-nfshome/user/ > /dev/null 2>&1 \
    && echo "  NFS home:    OK" || echo "  NFS home:    HUNG or missing"
timeout 5 ls /nfs/cs-ccr-felab/dsc/ > /dev/null 2>&1 \
    && echo "  NFS felab:   OK" || echo "  NFS felab:   HUNG or missing"
echo
echo "=== Done ($(ts)) ==="
