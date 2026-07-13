#!/bin/bash
# efvi_eflatency_bench.sh — vendor ground-truth RTT benchmark for the X2522 (ef_vi).
#
# Runs Onload's eflatency (CTPIO, all ULL tricks) across the two X2522 ports.
# eflatency reports mean/min/50%/95%/99%/max itself — that table is the target
# the abtrda3 ef_vi transport has to match.
#
#   sudo ./efvi_eflatency_bench.sh              # ~60s run, 64B frames, CTPIO thresh 64
#   sudo ./efvi_eflatency_bench.sh -n 1000000   # quick smoke (~8s)
#   sudo ./efvi_eflatency_bench.sh -s 86 -c 128 # 128B frames, threshold A/B
#
# Frame size: eflatency -s sets the UDP PAYLOAD; frame = payload + 42 (eth+ip+udp).
#   -s 22 -> 64B frame (matches the campaign's frame_size = 64)
#   -s 86 -> 128B frame (the order-book appendix size)
#
# Ping is pinned to core 6, pong to core 5 (isolcpus=5-7).
# Do NOT run watch/sensors during the run.

set -euo pipefail

PING_IF="enp1s0f0"          # 00:0f:53:ad:3c:90  PCI 01:00.0
PONG_IF="enp1s0f1"          # 00:0f:53:ad:3c:91  PCI 01:00.1
PING_CORE=6
PONG_CORE=5
EFLATENCY="${EFLATENCY:-$HOME/onload/build/gnu_x86_64/tests/ef_vi/eflatency}"

ITERS=30000000              # ~57s at the expected ~1.9us RTT
WARMUPS=1000000             # ~2s, mirrors the app's warmup discard
PAYLOAD=22                  # 64B frame
CT_THRESH=64
MODES="c"                   # CTPIO only — fail loudly rather than silently fall back
NO_POISON=""

while getopts "n:w:s:c:m:p" opt; do
    case "${opt}" in
        n) ITERS="${OPTARG}";;
        w) WARMUPS="${OPTARG}";;
        s) PAYLOAD="${OPTARG}";;
        c) CT_THRESH="${OPTARG}";;
        m) MODES="${OPTARG}";;
        p) NO_POISON="-p";;
        *) echo "usage: $0 [-n iters] [-w warmups] [-s payload] [-c ct_thresh] [-m modes] [-p]"; exit 1;;
    esac
done

if [[ "$(id -u)" != "0" ]]; then
    echo "ERROR: run with sudo (VI allocation + link set need root)"
    exit 1
fi
# sudo resets $HOME to /root; the onload build lives under the login user
if [[ ! -x "${EFLATENCY}" && -n "${SUDO_USER:-}" ]]; then
    EFLATENCY="/home/${SUDO_USER}/onload/build/gnu_x86_64/tests/ef_vi/eflatency"
fi
if [[ ! -x "${EFLATENCY}" ]]; then
    echo "ERROR: eflatency not found at ${EFLATENCY} (set EFLATENCY=...)"
    exit 1
fi
if [[ ! -e /dev/sfc_char ]]; then
    echo "ERROR: /dev/sfc_char missing — onload/sfc_char modules not loaded (kernel 6.9.12-hz100 only)"
    exit 1
fi
if pgrep -f "eflatency (ping|pong)" >/dev/null; then
    echo "ERROR: a stale eflatency is already running — kill it first (pgrep -af eflatency)"
    exit 1
fi

# ── the two hot processes MUST sit on isolated cores ─────────────────────────
# Verify against the kernel's actual isolated set (isolcpus=5-7 on this box),
# not just trust the constants at the top of this file.
ISOLATED="$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
in_isolated() {
    local core="$1" part lo hi
    IFS=',' read -ra parts <<< "${ISOLATED}"
    for part in "${parts[@]}"; do
        if [[ "${part}" == *-* ]]; then
            lo="${part%-*}"
            hi="${part#*-}"
            (( core >= lo && core <= hi )) && return 0
        else
            [[ "${part}" == "${core}" ]] && return 0
        fi
    done
    return 1
}
for c in "${PING_CORE}" "${PONG_CORE}"; do
    if ! in_isolated "${c}"; then
        echo "ERROR: core ${c} is not in the kernel's isolated set ('${ISOLATED:-none}')"
        echo "       adjust PING_CORE/PONG_CORE or the isolcpus= cmdline"
        exit 1
    fi
done
echo "isolated cores: ${ISOLATED} — ping->cpu${PING_CORE} pong->cpu${PONG_CORE}"

# ── links up (they come up admin-DOWN at boot; netplan does not manage them) ──
for ifc in "${PING_IF}" "${PONG_IF}"; do
    ip link set "${ifc}" up
done
for ifc in "${PING_IF}" "${PONG_IF}"; do
    for _ in $(seq 1 100); do
        [[ "$(cat /sys/class/net/${ifc}/carrier 2>/dev/null)" == "1" ]] && break
        sleep 0.1
    done
    if [[ "$(cat /sys/class/net/${ifc}/carrier 2>/dev/null)" != "1" ]]; then
        echo "ERROR: ${ifc} has no carrier after 10s (DAC seated?)"
        exit 1
    fi
done

TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="./eflatency_${TS}"
mkdir -p "${OUT_DIR}"
FRAME=$(( PAYLOAD + 42 ))

{
    echo "── eflatency bench ${TS} ──"
    echo "kernel:    $(uname -r)"
    echo "eflatency: ${EFLATENCY}"
    echo "ping ${PING_IF} core ${PING_CORE}  |  pong ${PONG_IF} core ${PONG_CORE}"
    echo "iters ${ITERS}  warmups ${WARMUPS}  payload ${PAYLOAD} (frame ${FRAME}B)  ct_thresh ${CT_THRESH}  modes ${MODES} ${NO_POISON}"
    for ifc in "${PING_IF}" "${PONG_IF}"; do
        ethtool "${ifc}" 2>/dev/null | grep -E "Speed|Link detected" | tr -s ' \n' ' ' | sed "s/^/${ifc}: /"
        echo ""
        ethtool --show-fec "${ifc}" 2>/dev/null | grep -i "Active FEC" | sed "s/^/${ifc}: /" || true
    done
} | tee "${OUT_DIR}/summary.txt"

# ── run ──────────────────────────────────────────────────────────────────────
# pong MUST get the same -n/-w as ping: it loops exactly warmups+iterations
# round-trips and exits (its defaults are 100k+10k — a mismatched pong dies
# mid-run and strands the ping).
taskset -c "${PONG_CORE}" "${EFLATENCY}" -n "${ITERS}" -w "${WARMUPS}" -s "${PAYLOAD}" \
    -c "${CT_THRESH}" -m "${MODES}" ${NO_POISON} \
    pong "${PONG_IF}" > "${OUT_DIR}/pong.log" 2>&1 &
PONG_PID=$!
trap 'kill ${PONG_PID} 2>/dev/null || true' EXIT
sleep 2
if ! kill -0 "${PONG_PID}" 2>/dev/null; then
    echo "ERROR: pong died at startup:"
    cat "${OUT_DIR}/pong.log"
    exit 1
fi

echo "── running (~$(( ITERS / 500000 ))s estimated) ──"
taskset -c "${PING_CORE}" "${EFLATENCY}" -n "${ITERS}" -w "${WARMUPS}" -s "${PAYLOAD}" \
    -c "${CT_THRESH}" -m "${MODES}" ${NO_POISON} \
    ping "${PING_IF}" > "${OUT_DIR}/ping.log" 2>&1 &
PING_PID=$!
trap 'kill ${PING_PID} ${PONG_PID} 2>/dev/null || true' EXIT

# confirm actual placement (PSR = the cpu each process is executing on)
sleep 3
for spec in "ping:${PING_PID}:${PING_CORE}" "pong:${PONG_PID}:${PONG_CORE}"; do
    name="${spec%%:*}"
    rest="${spec#*:}"
    pid="${rest%%:*}"
    want="${rest#*:}"
    psr="$(ps -o psr= -p "${pid}" 2>/dev/null | tr -d ' ' || true)"
    if [[ -z "${psr}" ]]; then
        # already exited: fine for a very short run; the wait below judges success
        echo "  ${name} pid ${pid} finished before the placement check (short run)" | tee -a "${OUT_DIR}/summary.txt"
    elif [[ "${psr}" != "${want}" ]]; then
        echo "ERROR: ${name} (pid ${pid}) is on cpu ${psr}, expected cpu ${want} — aborting"
        exit 1
    else
        echo "  ${name} pid ${pid} confirmed on isolated cpu ${psr}" | tee -a "${OUT_DIR}/summary.txt"
    fi
done

# bounded wait: ~3x the expected duration, and detect a stranded ping (pong
# gone but ping still spinning — the failure mode of a died/mismatched pong)
DEADLINE=$(( SECONDS + ITERS / 250000 + 60 ))
while kill -0 "${PING_PID}" 2>/dev/null; do
    if (( SECONDS > DEADLINE )); then
        echo "ERROR: run exceeded time budget — killing both (see ${OUT_DIR}/*.log)"
        kill "${PING_PID}" "${PONG_PID}" 2>/dev/null || true
        exit 1
    fi
    if ! kill -0 "${PONG_PID}" 2>/dev/null; then
        sleep 5
        if kill -0 "${PING_PID}" 2>/dev/null; then
            echo "ERROR: pong exited but ping is still running (stranded) — pong.log:"
            cat "${OUT_DIR}/pong.log"
            kill "${PING_PID}" 2>/dev/null || true
            exit 1
        fi
        break
    fi
    sleep 1
done
PING_RC=0
wait "${PING_PID}" || PING_RC=$?
kill "${PONG_PID}" 2>/dev/null || true
trap - EXIT
cat "${OUT_DIR}/ping.log"
if [[ "${PING_RC}" != "0" ]]; then
    echo "ERROR: ping exited with ${PING_RC}"
    exit 1
fi

# ── validate the datapath actually used (v9 prints "# mode: CTPIO") ──────────
if grep -qE "^# (TX )?mode:" "${OUT_DIR}/ping.log" && \
   ! grep -qE "^# (TX )?mode: CTPIO" "${OUT_DIR}/ping.log"; then
    echo "WARNING: TX mode is NOT CTPIO — this is not the ULL configuration:"
    grep -E "^# (TX )?mode:" "${OUT_DIR}/ping.log"
fi

# eflatency's own table (mean/min/50%/95%/99%/max) is the result — keep it all
cat "${OUT_DIR}/ping.log" >> "${OUT_DIR}/summary.txt"
echo "── done: ${OUT_DIR}/summary.txt ──"
