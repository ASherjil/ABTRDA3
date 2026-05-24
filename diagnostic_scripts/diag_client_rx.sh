#!/bin/bash
# AF_XDP-ZC: WHY DOES THE CLIENT RECEIVE ZERO ECHOES?  (XXV710 only)
#
# Symptom: server reflects every packet, client RX sees none (100% loss on the
# server->client direction). This isolates WHERE the reflected frames go:
#
#   - client port.rx_unicast rises ~by #packets  => frames REACH the NIC but
#       never reach the XSK  => RSS not on queue 0, or BPF/redirect/filter issue
#   - client port.rx_unicast does NOT rise        => frames never arrive on the
#       wire (link / server-TX direction) despite "Reflected N"
#   - PROMISC flag missing on the client port     => VSI re-arm didn't take
#   - RSS indirection table not all-0             => reflections hash to a queue
#       with no XSK bound -> XDP_PASS to the stack, XSK sees nothing
#
# NAPI knobs (napi_defer_hard_irqs=2 / gro_flush_timeout=2000) are held on for
# the whole run by a background re-assert loop, so launch timing is not a factor.
# All NIC state is saved and restored; processes killed on exit.
#
# Usage:  ./diag_client_rx.sh [user@host]    (default asherjil@100.72.135.7)
# You will be prompted ONCE for your sudo password.

set -u
HOST="${1:-asherjil@100.72.135.7}"
REMOTE_TMP="/tmp/diag_client_rx.$$.sh"
LOCAL_TMP="$(mktemp /tmp/diag_client_rx.XXXXXX.sh)"
trap 'rm -f "$LOCAL_TMP"' EXIT

cat > "$LOCAL_TMP" <<'REMOTE'
#!/bin/bash
set -u
RUNDIR="${1:?need RUNDIR}"
CLI_IF=enp1s0f0np0     # client port (core 3)  XXV710  -- the one with 0 RX
SRV_IF=enp1s0f1np1     # server port (core 2)  XXV710
COUNT=1000

cd "$RUNDIR" || { echo "FATAL: $RUNDIR not found"; exit 1; }
[ -x ./abtrda3_test ] || { echo "FATAL: ./abtrda3_test missing"; exit 1; }
echo "Run dir: $RUNDIR   config: abtrda3_test.toml (XXV710 only)"

# ── save current NAPI knobs so we restore EXACTLY what was there ─────────────
declare -A OLD_DEFER OLD_GRO
for IF in "$CLI_IF" "$SRV_IF"; do
  OLD_DEFER[$IF]=$(cat /sys/class/net/$IF/napi_defer_hard_irqs 2>/dev/null || echo 0)
  OLD_GRO[$IF]=$(cat   /sys/class/net/$IF/gro_flush_timeout    2>/dev/null || echo 0)
done

# ── hold the NAPI knobs ON for the whole run (beat any NicTuner clobber) ─────
( while :; do
    for IF in "$CLI_IF" "$SRV_IF"; do
      echo 2    > /sys/class/net/$IF/napi_defer_hard_irqs 2>/dev/null
      echo 2000 > /sys/class/net/$IF/gro_flush_timeout    2>/dev/null
    done
    sleep 0.2
  done ) &
KNOB_PID=$!

cleanup() {
  kill "$KNOB_PID" 2>/dev/null
  pkill -9 -f abtrda3_test 2>/dev/null
  for IF in "$CLI_IF" "$SRV_IF"; do
    echo "${OLD_DEFER[$IF]}" > /sys/class/net/$IF/napi_defer_hard_irqs 2>/dev/null
    echo "${OLD_GRO[$IF]}"   > /sys/class/net/$IF/gro_flush_timeout    2>/dev/null
  done
  echo "(restored NAPI knobs, killed test processes)"
}
trap cleanup EXIT

pkill -9 -f abtrda3_test 2>/dev/null; sleep 1
rm -f /tmp/srv.log /tmp/cli.log /tmp/cnt_before /tmp/cnt_after

# 1) server up first
echo "Starting server ..."
./abtrda3_test --server --config abtrda3_test.toml >/tmp/srv.log 2>&1 &
sleep 3   # let server bind + NicTuner run

# 2) snapshot client-port HW counters BEFORE the client sends anything
ethtool -S "$CLI_IF" > /tmp/cnt_before 2>/dev/null

# 3) client (foreground): sends COUNT, stops early on the timeout cap
echo "Starting client (--count $COUNT) ..."
./abtrda3_test --client --count "$COUNT" --config abtrda3_test.toml >/tmp/cli.log 2>&1

# 4) snapshot AFTER
ethtool -S "$CLI_IF" > /tmp/cnt_after 2>/dev/null

# 5) stop server for its summary
pkill -INT -f abtrda3_test 2>/dev/null; sleep 1

# ── results ─────────────────────────────────────────────────────────────────
echo; echo "=================== CLIENT LOG (tail) ==================="
tail -8 /tmp/cli.log
echo; echo "=================== SERVER LOG (tail) ==================="
tail -6 /tmp/srv.log

echo; echo "=================== CLIENT PORT ($CLI_IF) PROMISC + LINK ="
ip link show "$CLI_IF" | head -2
echo "promisc users:"; cat /sys/class/net/$CLI_IF/flags 2>/dev/null

echo; echo "=================== CLIENT PORT RSS indirection table ===="
echo "(NicTuner should have steered ALL entries to queue 0)"
ethtool -x "$CLI_IF" 2>/dev/null | head -16

echo; echo "=================== CLIENT PORT counter CHANGES ========="
echo "(lines that moved while the server reflected ~$COUNT frames at us)"
diff /tmp/cnt_before /tmp/cnt_after | grep -E '^[<>]' || echo "(NO counters changed — frames are not reaching the NIC)"

echo; echo "=================== CLIENT PORT key RX counters (after) ="
grep -iE 'port\.rx_unicast|port\.rx_bytes|port\.rx_size|port\.rx_dropped|port\.rx_discards|rx_dropped|rx_missed|^ *rx-0\.' /tmp/cnt_after

echo; echo "(done)"
REMOTE

echo "=== client-RX diagnostic -> $HOST (XXV710 only) ==="
echo "Copying remote script ..."
scp -q "$LOCAL_TMP" "$HOST:$REMOTE_TMP" || { echo "scp failed"; exit 1; }
echo "Running on server (you'll be asked for your sudo password) ..."
echo ""
ssh -t "$HOST" "cd ~/ABTTiming/ABTRDA3 && sudo bash '$REMOTE_TMP' \"\$(pwd)\"; rm -f '$REMOTE_TMP'"
echo ""
echo "=== diagnostic complete ==="
