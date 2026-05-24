#!/bin/bash
# AF_XDP-ZC: WHERE DOES THE NAPI POLL RUN?  (read-only profiling over SSH)
#
# XXV710 ONLY (i40e). Loopback pair on the server, both from abtrda3_test.toml:
#   client enp1s0f0np0 / core 3   <->   server enp1s0f1np1 / core 2
# Nothing else is touched. The I225 / enp6s0 / abtrda3_i225.toml is NOT used.
#
# Question this answers: when the client calls recvfrom() to wait for the reply,
# does the kernel run the driver RX poll (napi_busy_loop -> i40e_clean_rx_irq_zc)
# INLINE inside the syscall (busy-poll working), or only schedule the NAPI and
# wait for the gro_flush_timeout softirq/timer (busy-poll NOT working -> the
# ~17us floor that tracks gro_flush)?
#
#   INLINE  : napi_busy_loop / i40e_clean_rx_irq_zc UNDER __sys_recvfrom->xsk_recvmsg
#   DEFERRED: those symbols UNDER __softirqentry / run_ksoftirqd / net_rx_action
#             while xsk_recvmsg returns empty  => missing socket napi_id is root cause
#
# How it runs: copies the remote body to the server, then `ssh -t ... sudo bash`
# so a TTY exists (this server's sudo needs a password) and the whole capture
# runs as root in one shot. NAPI sysfs knobs are saved/restored; procs killed on
# exit; stale root-owned /tmp logs are cleared first. NOTHING is left changed.
#
# Usage:
#   ./perf_xsk_napi.sh                    # default host asherjil@100.72.135.7
#   ./perf_xsk_napi.sh asherjil@rtserver  # custom host
#
# You will be prompted ONCE for your sudo password on the server.

set -u
HOST="${1:-asherjil@100.72.135.7}"
REMOTE_TMP="/tmp/perf_xsk_remote.$$.sh"
LOCAL_TMP="$(mktemp /tmp/perf_xsk_remote.XXXXXX.sh)"
trap 'rm -f "$LOCAL_TMP"' EXIT

# ── remote body (runs as ROOT on the server; $1 = run directory) ─────────────
cat > "$LOCAL_TMP" <<'REMOTE'
#!/bin/bash
set -u
RUNDIR="${1:?need RUNDIR}"
CLI_IF=enp1s0f0np0     # client port (core 3)   XXV710
SRV_IF=enp1s0f1np1     # server port (core 2)   XXV710
CLI_CORE=3
DATA=/tmp/xsk.data
CAP_SECS=8

cd "$RUNDIR" || { echo "FATAL: $RUNDIR not found on server"; exit 1; }
[ -x ./abtrda3_test ]      || { echo "FATAL: ./abtrda3_test missing in $RUNDIR"; exit 1; }
[ -f ./abtrda3_test.toml ] || { echo "FATAL: abtrda3_test.toml missing in $RUNDIR"; exit 1; }
echo "Run dir: $RUNDIR   config: abtrda3_test.toml (XXV710 only)"

# ── perf availability + symbol permissions (we are root) ────────────────────
if ! command -v perf >/dev/null 2>&1; then
  echo "Installing linux-perf ..."; apt-get install -y linux-perf >/dev/null 2>&1
fi
command -v perf >/dev/null 2>&1 || { echo "FATAL: perf not available on server"; exit 1; }
sysctl -qw kernel.perf_event_paranoid=-1 2>/dev/null
sysctl -qw kernel.kptr_restrict=0        2>/dev/null

# ── save current NAPI knobs to restore EXACTLY what was there ───────────────
declare -A OLD_DEFER OLD_GRO
for IF in "$CLI_IF" "$SRV_IF"; do
  OLD_DEFER[$IF]=$(cat /sys/class/net/$IF/napi_defer_hard_irqs 2>/dev/null || echo 0)
  OLD_GRO[$IF]=$(cat   /sys/class/net/$IF/gro_flush_timeout    2>/dev/null || echo 0)
done

cleanup() {
  pkill -9 -f abtrda3_test 2>/dev/null
  for IF in "$CLI_IF" "$SRV_IF"; do
    echo "${OLD_DEFER[$IF]}" > /sys/class/net/$IF/napi_defer_hard_irqs 2>/dev/null
    echo "${OLD_GRO[$IF]}"   > /sys/class/net/$IF/gro_flush_timeout    2>/dev/null
  done
  echo "(restored NAPI knobs, killed test processes)"
}
trap cleanup EXIT

# ── clean slate: kill stale procs, remove stale root-owned logs ─────────────
pkill -9 -f abtrda3_test 2>/dev/null; sleep 1
rm -f /tmp/srv.log /tmp/cli.log "$DATA"

# 1) loopback pair (XXV710), server first so client packet 0 lands
echo "Starting server + client (XXV710 loopback) ..."
./abtrda3_test --server                 --config abtrda3_test.toml >/tmp/srv.log 2>&1 &
sleep 2
./abtrda3_test --client --count 5000000 --config abtrda3_test.toml >/tmp/cli.log 2>&1 &
sleep 2

# 2) hand the NAPI to busy-poll (NicTuner does NOT set these)
for IF in "$CLI_IF" "$SRV_IF"; do
  echo 2    > /sys/class/net/$IF/napi_defer_hard_irqs
  echo 2000 > /sys/class/net/$IF/gro_flush_timeout
done
sleep 1

# 3) profile the client core during steady-state ping-pong
echo "Profiling CPU $CLI_CORE for ${CAP_SECS}s ..."
perf record -C "$CLI_CORE" -g -o "$DATA" -- sleep "$CAP_SECS"

# 4) graceful stop so the app prints its RTT summary
pkill -INT -f abtrda3_test 2>/dev/null; sleep 1

# 5) results ─────────────────────────────────────────────────────────────────
echo; echo "=================== CLIENT RTT SUMMARY ==================="
tail -20 /tmp/cli.log
echo; echo "=================== SERVER LOG (tail) ==================="
tail -6 /tmp/srv.log

echo; echo "=================== KEY SYMBOL HITS (sample counts) ====="
perf script -i "$DATA" 2>/dev/null \
  | grep -oiE 'xsk_recvmsg|xsk_sendmsg|sock_recvmsg|__sys_recvfrom|__x64_sys_recvfrom|napi_busy_loop|busy_poll_stop|i40e_clean_rx_irq_zc|i40e_napi_poll|net_rx_action|__softirqentry_text_start|run_ksoftirqd|do_softirq|__hrtimer' \
  | sort | uniq -c | sort -rn

echo; echo "=================== FLAT (self time) top 40 ============="
perf report -i "$DATA" --stdio --no-children 2>/dev/null | head -40

echo; echo "=================== CALL GRAPHS (children) top 200 ======"
perf report -i "$DATA" --stdio 2>/dev/null | head -200

echo; echo "(done)"
REMOTE

# ── ship it + run as root with a real TTY (one sudo password prompt) ─────────
echo "=== AF_XDP-ZC NAPI placement profile -> $HOST (XXV710 only) ==="
echo "Copying remote capture script ..."
scp -q "$LOCAL_TMP" "$HOST:$REMOTE_TMP" || { echo "scp failed"; exit 1; }

echo "Running on server (you'll be asked for your sudo password) ..."
echo ""
ssh -t "$HOST" "cd ~/ABTTiming/ABTRDA3 && sudo bash '$REMOTE_TMP' \"\$(pwd)\"; rm -f '$REMOTE_TMP'"

echo ""
echo "=== capture complete ==="
