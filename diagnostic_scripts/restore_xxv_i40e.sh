#!/usr/bin/env bash
# restore_xxv_i40e.sh — rebind NIC ports from vfio-pci back to a kernel driver
# after a DPDK run.
#
# WHY THIS EXISTS: our DPDK prepare() binds the port to vfio-pci by setting
# `driver_override = vfio-pci`. That override BLOCKS binding to any other driver,
# so `dpdk-devbind.py --bind=i40e` fails with ENODEV ("No such device") and the
# port is stuck on vfio with no netdev. The override MUST be cleared first — that
# is the one step the standard tools miss. This script does: unbind -> clear
# override -> bind to the kernel driver -> bring the netdev up -> wait for carrier.
#
# USAGE:
#   ./restore_xxv_i40e.sh                       # XXV710 pair (02:00.0/.1) -> i40e
#   ./restore_xxv_i40e.sh igc 0000:06:00.0      # any driver + explicit BDF(s)
#   ./restore_xxv_i40e.sh i40e 0000:02:00.0 0000:02:00.1
#
# Re-execs itself under sudo if not run as root.

set -u

DRIVER="${1:-i40e}"
shift 2>/dev/null || true
BDFS=("$@")
if [ "${#BDFS[@]}" -eq 0 ]; then
    # Default target: the XXV710 (i40e) pair. NOTE the ConnectX-4 Lx took 01:00.x,
    # which pushed the XXV710 to 02:00.x — update here if the PCI layout changes.
    BDFS=(0000:02:00.0 0000:02:00.1)
fi

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$DRIVER" "${BDFS[@]}"
fi

# The target kernel driver must be registered (module loaded).
if [ ! -d "/sys/bus/pci/drivers/$DRIVER" ]; then
    echo "[restore] $DRIVER not loaded — modprobe $DRIVER"
    modprobe "$DRIVER" || { echo "[restore] ERROR: cannot load $DRIVER"; exit 1; }
fi

for bdf in "${BDFS[@]}"; do
    dev="/sys/bus/pci/devices/$bdf"
    echo "=== $bdf ==="
    if [ ! -e "$dev" ]; then
        echo "  SKIP: device not present"
        continue
    fi

    # 1. Unbind from whatever driver currently holds it (typically vfio-pci).
    if [ -e "$dev/driver" ]; then
        cur="$(basename "$(readlink "$dev/driver")")"
        echo "  unbind from $cur"
        echo "$bdf" > "$dev/driver/unbind" 2>/dev/null || true
    fi

    # 2. Clear driver_override (THE key step) so the device can bind to a driver
    #    other than the forced vfio-pci. An empty write clears it.
    if [ -e "$dev/driver_override" ]; then
        echo "" > "$dev/driver_override"
        echo "  cleared driver_override"
    fi

    # 3. Bind to the target kernel driver.
    if echo "$bdf" > "/sys/bus/pci/drivers/$DRIVER/bind" 2>/dev/null; then
        echo "  bound to $DRIVER"
    else
        echo "  WARN: explicit bind failed — trying drivers_probe"
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    fi
done

# Verify: driver in use + bring netdev up + wait for carrier.
echo
echo "=== result ==="
for bdf in "${BDFS[@]}"; do
    dev="/sys/bus/pci/devices/$bdf"
    [ -e "$dev" ] || continue
    drv="$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)"
    nic=""
    [ -d "$dev/net" ] && nic="$(ls "$dev/net" 2>/dev/null | head -1)"

    if [ -n "$nic" ]; then
        ip link set "$nic" up 2>/dev/null
        carrier="?"
        for _ in $(seq 1 12); do          # up to ~6s for the link to settle
            c="$(cat "/sys/class/net/$nic/carrier" 2>/dev/null || echo 0)"
            if [ "$c" = "1" ]; then carrier="1"; break; fi
            carrier="$c"
            sleep 0.5
        done
        printf "%s  driver=%s  netdev=%s  carrier=%s\n" "$bdf" "${drv:-none}" "$nic" "$carrier"
    else
        printf "%s  driver=%s  netdev=none (not a kernel netdev driver?)\n" "$bdf" "${drv:-none}"
    fi
done
