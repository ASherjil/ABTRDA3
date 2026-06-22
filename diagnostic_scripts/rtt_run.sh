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

echo "[rtt_run] app=$APP"
echo "[rtt_run] cfg=$CFG"
echo "[rtt_run] ===== SERVER (background) ====="
sudo "$APP" --server --config "$CFG" &

echo "[rtt_run] waiting ${WAIT}s for the server to come up (override: WAIT=<sec>)…"
sleep "$WAIT"
if ! pgrep -f "abtrda3_test --server" >/dev/null 2>&1; then
    echo "[rtt_run] ERROR: server not running after ${WAIT}s — init failed, or watchdog_sec" >&2
    echo "          is shorter than startup. Set watchdog_sec=0 in the config for RTT." >&2
    exit 1
fi

echo "[rtt_run] ===== CLIENT ====="
if [ -n "$COUNT" ]; then
    sudo "$APP" --client --config "$CFG" --count "$COUNT"
else
    sudo "$APP" --client --config "$CFG"
fi
rc=$?

echo "[rtt_run] client exited (rc=$rc) — stopping server (SIGINT, clean shutdown)…"
stop_server
trap - INT TERM
echo "[rtt_run] done (client rc=$rc)"
exit "$rc"
