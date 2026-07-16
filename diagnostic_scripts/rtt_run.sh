#!/usr/bin/env bash
# rtt_run.sh — one-command RTT (Server + Client) on a single host.
#
# Starts --server in the background, waits a few seconds for it to come up, runs
# --client in the foreground (you see its latency report), then SIGINTs the server
# so its handler runs a cooperative stop -> C++ destructors fire (NIC/XDP restored
# cleanly) instead of being SIGKILLed.
#
# Usage (from ANYWHERE):  ./rtt_run.sh <config.toml> [count]
#   <config.toml>  YOUR config, used AS-IS for both roles (server=[server], client=[client]).
#                  >>> For RTT runs longer than [general].watchdog_sec, set watchdog_sec = 0
#                      in your config — this script owns the server's lifetime and stops it
#                      with SIGINT when the client finishes, so the built-in watchdog only
#                      gets in the way.
#   [count]        optional; overrides [client].count
#   WAIT=<sec>     env, default 5; seconds to let the server come up before the client
#                  starts. Use ~10 for AF_XDP (it bounces the link on init).
#
# Binary auto-located under <repo>/build/*/src/app/abtrda3_test (override with APP=...).
set -u

CFG="${1:?usage: rtt_run.sh <config.toml> [count]}"
COUNT="${2:-}"
WAIT="${WAIT:-5}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Locate the binary: $APP, else ./abtrda3_test, else <repo>/build/*/src/app/.
APP="${APP:-}"
if [ -z "$APP" ]; then
    if [ -x ./abtrda3_test ]; then
        APP="$(pwd)/abtrda3_test"
    else
        for c in "$SCRIPT_DIR"/../build/*/src/app/abtrda3_test; do
            [ -x "$c" ] && APP="$c" && break
        done
    fi
fi
[ -n "$APP" ] && [ -x "$APP" ] || { echo "[rtt_run] ERROR: abtrda3_test not found; set APP=" >&2; exit 1; }

# Absolutize binary + config BEFORE cd (relative paths would otherwise break).
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
APP_DIR="$(dirname "$APP")"
[ -f "$CFG" ] || { echo "[rtt_run] ERROR: config '$CFG' not found" >&2; exit 1; }
CFG="$(cd "$(dirname "$CFG")" && pwd)/$(basename "$CFG")"
# Run from the binary's dir so AF_XDP's af_xdp_kern.o (CWD-relative) resolves.
cd "$APP_DIR" || { echo "[rtt_run] ERROR: cannot cd to $APP_DIR" >&2; exit 1; }

# ── ef_vi preflight ───────────────────────────────────────────────────────────
# The certified ef_vi configuration needs host state that does not survive a
# reboot, plus an environment nobody should have to retype:
#   * a 2MiB hugepage pool >= 2 pages (buffer determinism — kills the 4K page
#     lottery; one page per process),
#   * ASLR off (setarch -R) + ABTRDA3_TX_PAD=8 — the frozen-layout fragile-cell
#     dodge (certified config of record),
#   * the X2522 ports admin-UP (they boot DOWN; netplan does not manage them),
#   * /dev/sfc_char (onload/sfc modules — only present on kernel 6.9.12-hz100).
# All checks are idempotent: existing state is verified, missing state created.
EFVI=0
if grep -Eq '^[[:space:]]*transport[[:space:]]*=[[:space:]]*"ef_vi"' "$CFG"; then
    EFVI=1
fi
LAUNCH=()
if [ "$EFVI" = "1" ]; then
    echo "[rtt_run] ef_vi config detected — preflight:"
    if [ ! -e /dev/sfc_char ]; then
        echo "[rtt_run] ERROR: /dev/sfc_char missing — onload/sfc modules not loaded (wrong kernel?)" >&2
        exit 1
    fi
    HP=/sys/kernel/mm/hugepages/hugepages-2048kB
    if [ "$(cat "$HP/free_hugepages")" -lt 2 ]; then
        NEED=$(( $(cat "$HP/nr_hugepages") + 2 - $(cat "$HP/free_hugepages") ))
        echo "[rtt_run]   2M hugepages: reserving (pool -> $NEED)"
        echo "$NEED" | sudo tee "$HP/nr_hugepages" >/dev/null
        if [ "$(cat "$HP/free_hugepages")" -lt 2 ]; then
            echo "[rtt_run] ERROR: could not reserve 2x 2MiB hugepages" >&2
            exit 1
        fi
    fi
    echo "[rtt_run]   2M hugepages: $(cat "$HP/free_hugepages") free — OK"
    # Bring the config's interfaces up and wait for carrier (admin-DOWN at boot).
    for ifc in $(grep -E '^[[:space:]]*interface[[:space:]]*=' "$CFG" | sed -E 's/.*"([^"]+)".*/\1/' | sort -u); do
        if [ ! -d "/sys/class/net/$ifc" ]; then
            echo "[rtt_run] ERROR: interface '$ifc' from the config does not exist" >&2
            exit 1
        fi
        sudo ip link set "$ifc" up
        for _ in $(seq 1 100); do
            [ "$(cat "/sys/class/net/$ifc/carrier" 2>/dev/null)" = "1" ] && break
            sleep 0.1
        done
        if [ "$(cat "/sys/class/net/$ifc/carrier" 2>/dev/null)" != "1" ]; then
            echo "[rtt_run] ERROR: $ifc has no carrier after 10s (DAC seated?)" >&2
            exit 1
        fi
        echo "[rtt_run]   link $ifc: up, carrier OK"
    done
    LAUNCH=(setarch x86_64 -R)
    echo "[rtt_run]   layout freeze: setarch x86_64 -R (ASLR off)"
fi

# Stop the server gently: SIGINT -> its handler -> cooperative stop -> destructors
# (this is what restores NIC/XDP state). Escalate only if it refuses. Idempotent.
stop_server() {
    pgrep -f "abtrda3_test --server" >/dev/null 2>&1 || return 0
    sudo pkill -INT -f "abtrda3_test --server" 2>/dev/null
    for _ in $(seq 1 20); do
        pgrep -f "abtrda3_test --server" >/dev/null 2>&1 || return 0
        sleep 0.5
    done
    echo "[rtt_run] server didn't exit on SIGINT — escalating to SIGTERM" >&2
    sudo pkill -TERM -f "abtrda3_test --server" 2>/dev/null
    sleep 2
}
trap 'echo; echo "[rtt_run] interrupted"; stop_server; exit 130' INT TERM

# The inner sudo below applies env_reset and STRIPS EF_VI_*/ABTRDA3_* — the
# variables that tune libciul's CTPIO writer and the ef_vi backend. Forward
# them explicitly as sudo command-line assignments (those survive env_reset).
mapfile -t ENVPASS < <(env | grep -E '^(EF_VI_|ABTRDA3_)[A-Za-z0-9_]*=')
# ef_vi default: the certified TX pad, unless the caller set their own.
if [ "$EFVI" = "1" ] && ! printf '%s\n' "${ENVPASS[@]}" | grep -q '^ABTRDA3_TX_PAD='; then
    ENVPASS+=("ABTRDA3_TX_PAD=8")
fi

echo "[rtt_run] app=$APP"
echo "[rtt_run] cfg=$CFG"
if [ "${#ENVPASS[@]}" -gt 0 ]; then
    echo "[rtt_run] env passthrough: ${ENVPASS[*]}"
fi
echo "[rtt_run] ===== SERVER (background) ====="
sudo "${ENVPASS[@]}" "${LAUNCH[@]}" "$APP" --server --config "$CFG" &

echo "[rtt_run] waiting ${WAIT}s for the server to come up (override: WAIT=<sec>)…"
sleep "$WAIT"
if ! pgrep -f "abtrda3_test --server" >/dev/null 2>&1; then
    echo "[rtt_run] ERROR: server not running after ${WAIT}s — init failed, or watchdog_sec" >&2
    echo "          is shorter than startup. Set watchdog_sec=0 in the config for RTT." >&2
    exit 1
fi

echo "[rtt_run] ===== CLIENT ====="
if [ -n "$COUNT" ]; then
    sudo "${ENVPASS[@]}" "${LAUNCH[@]}" "$APP" --client --config "$CFG" --count "$COUNT"
else
    sudo "${ENVPASS[@]}" "${LAUNCH[@]}" "$APP" --client --config "$CFG"
fi
rc=$?

echo "[rtt_run] client exited (rc=$rc) — stopping server (SIGINT, clean shutdown)…"
stop_server
trap - INT TERM
echo "[rtt_run] done (client rc=$rc)"
exit "$rc"
