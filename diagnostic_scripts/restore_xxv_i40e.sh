#!/usr/bin/env bash
# restore_xxv_i40e.sh — rebind NIC ports from vfio-pci back to their kernel driver
# after a DPDK run.
#
# WHY THIS EXISTS: our DPDK prepare() binds the port to vfio-pci by setting
# `driver_override = vfio-pci`. That override BLOCKS binding to any other driver,
# so `dpdk-devbind.py --bind=<drv>` fails with ENODEV ("No such device") and the
# port is stuck on vfio with no netdev. The override MUST be cleared first — that
# is the one step the standard tools miss. Per device: unbind -> clear override ->
# bind the kernel driver -> bring the netdev up -> wait for carrier.
#
# By default it restores BOTH DPDK-capable Intel NIC pairs on this box:
#   Intel XXV710-DA2 (i40e)  0000:02:00.0 / 0000:02:00.1
#   Intel I225-V     (igc)   0000:05:00.0 / 0000:06:00.0
# (ConnectX-4 Lx / mlx5 is bifurcated — never unbound — so it is not listed.)
# A port already on its target kernel driver is left in place (just brought up) —
# i.e. re-running is a safe no-op.
#
# USAGE:
#   ./restore_xxv_i40e.sh                          # restore BOTH pairs above
#   ./restore_xxv_i40e.sh igc 0000:06:00.0         # explicit: one driver + BDF(s)
#   ./restore_xxv_i40e.sh i40e 0000:02:00.0 0000:02:00.1
#
# Re-execs itself under sudo if not run as root.

set -u

# Re-exec under sudo FIRST so the original argv is preserved verbatim (incl. no-args).
if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

# ---- build the list of "driver bdf..." groups to restore ---------------------
# No args -> both default pairs.  Args -> "<driver> <bdf...>" single group (legacy).
declare -a TARGETS
if [ "$#" -eq 0 ]; then
    TARGETS=(
        "i40e 0000:02:00.0 0000:02:00.1"   # Intel XXV710-DA2
        "igc 0000:05:00.0 0000:06:00.0"    # Intel I225-V
    )
else
    driver="$1"; shift
    if [ "$#" -eq 0 ]; then
        set -- 0000:02:00.0 0000:02:00.1   # legacy default BDFs for a bare driver arg
    fi
    TARGETS=("$driver $*")
fi

# ---- restore one device: driver_override-aware rebind ------------------------
restore_dev() {
    local driver="$1" bdf="$2"
    local dev="/sys/bus/pci/devices/$bdf"
    echo "=== $bdf -> $driver ==="
    if [ ! -e "$dev" ]; then
        echo "  SKIP: device not present"
        return
    fi

    local cur=""
    [ -e "$dev/driver" ] && cur="$(basename "$(readlink "$dev/driver")")"

    # Already on the target kernel driver -> no rebind (avoid a needless link bounce).
    # verify_dev() below still brings it up. This is the "already bound = no-op" path.
    if [ "$cur" = "$driver" ]; then
        echo "  already bound to $driver — no rebind"
        return
    fi

    # 1. Unbind from whatever holds it (typically vfio-pci).
    if [ -n "$cur" ]; then
        echo "  unbind from $cur"
        echo "$bdf" > "$dev/driver/unbind" 2>/dev/null || true
    fi
    # 2. Clear driver_override (THE key step) so it can bind to a non-vfio driver.
    if [ -e "$dev/driver_override" ]; then
        echo "" > "$dev/driver_override"
        echo "  cleared driver_override"
    fi
    # 3. Bind to the target kernel driver.
    if echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" 2>/dev/null; then
        echo "  bound to $driver"
    else
        echo "  WARN: explicit bind failed — trying drivers_probe"
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    fi
}

# ---- verify: driver in use + bring netdev up + wait for carrier --------------
verify_dev() {
    local bdf="$1"
    local dev="/sys/bus/pci/devices/$bdf"
    [ -e "$dev" ] || return
    local drv nic
    drv="$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)"
    nic=""
    [ -d "$dev/net" ] && nic="$(ls "$dev/net" 2>/dev/null | head -1)"

    if [ -n "$nic" ]; then
        ip link set "$nic" up 2>/dev/null
        local carrier="?" c
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
}

# ---- run ---------------------------------------------------------------------
declare -a ALL_BDFS
for group in "${TARGETS[@]}"; do
    # shellcheck disable=SC2086
    set -- $group
    driver="$1"; shift
    # The target kernel driver must be registered (module loaded) — once per group.
    if [ ! -d "/sys/bus/pci/drivers/$driver" ]; then
        echo "[restore] $driver not loaded — modprobe $driver"
        modprobe "$driver" || { echo "[restore] ERROR: cannot load $driver — skipping group"; continue; }
    fi
    for bdf in "$@"; do
        restore_dev "$driver" "$bdf"
        ALL_BDFS+=("$bdf")
    done
done

echo
echo "=== result ==="
for bdf in "${ALL_BDFS[@]}"; do
    verify_dev "$bdf"
done
