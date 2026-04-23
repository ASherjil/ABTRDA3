#!/bin/bash
# TapBridge standalone test on cfd-865-mkdev50 (Zynq UltraScale+ ARM64).
#
# Validates TAP fd plumbing WITHOUT unbinding the macb driver or touching
# real NIC hardware. The ARM64 kernel remains fully operational throughout
# (NFS, SSH keep working).
#
# Prerequisite: ./build.sh with arm64-release target before running.
#
# Test flow:
#   1. Binary creates TAP device "tap_pmd" via /dev/net/tun
#   2. Script assigns IP 10.99.0.1/24 to tap_pmd
#   3. Script pings 10.99.0.99 — kernel writes ARP to tap_pmd → bridge reads
#      via read(tap_fd) → bridge commits to StubTx → bridge.txCount++
#   4. Binary injects synthetic ARP into StubRx every 5s → bridge writes to
#      tap_fd → kernel receives it → bridge.rxCount++
#      (arrives in `ip neigh show` as 02:de:ad:be:ef:01 from 10.99.0.99)

set -e

HOST=cfd-865-mkdev50
BUILD_DIR=/user/asherjil/ABTTiming/ABTRDA3/build/arm64-release/test
BINARY=tap_bridge_test
TAP_NAME=tap_pmd
TAP_IP=10.99.0.1/24
PING_TARGET=10.99.0.99
DURATION=20

echo "=== TapBridge Test → $HOST ==="
echo "Binary:   $BUILD_DIR/$BINARY"
echo "Duration: ${DURATION}s"
echo ""

ssh "$HOST" "bash -s" << EOF
set +e

# Clean up stale state from any previous run
sudo pkill -9 $BINARY 2>/dev/null
sudo ip link del $TAP_NAME 2>/dev/null

cd $BUILD_DIR

# ── Launch bridge in background ──
echo '--- Launching $BINARY ---'
sudo ./$BINARY $TAP_NAME $DURATION &
TEST_PID=\$!

# ── Wait for TAP interface to appear ──
for i in \$(seq 1 20); do
    if ip link show $TAP_NAME &>/dev/null; then break; fi
    sleep 0.5
done

if ! ip link show $TAP_NAME &>/dev/null; then
    echo 'ERROR: $TAP_NAME did not come up'
    sudo kill -9 \$TEST_PID 2>/dev/null
    exit 1
fi

echo ''
echo '--- TAP interface state ---'
ip -d link show $TAP_NAME
echo ''

# ── Assign IP and exercise kernel→TX path ──
echo '--- Configuring $TAP_IP on $TAP_NAME ---'
sudo ip addr add $TAP_IP dev $TAP_NAME 2>/dev/null

echo ''
echo '--- Ping test (kernel writes ARP+ICMP to tap_fd) ---'
echo '(timeouts are EXPECTED — StubTx swallows packets, no reply returns)'
sudo ping -c 3 -W 1 $PING_TARGET 2>&1 || true

# ── Wait for synthetic ARP injection (binary injects every 5s) ──
echo ''
echo '--- Waiting for synthetic ARP from StubRx ---'
sleep 7
echo 'Kernel ARP table on $TAP_NAME:'
ip neigh show dev $TAP_NAME
echo ''
echo 'Looking for synthetic MAC 02:de:ad:be:ef:01 from 10.99.0.99...'
if ip neigh show dev $TAP_NAME | grep -qi '02:de:ad:be:ef:01'; then
    echo '  ✓ Synthetic ARP reached kernel (RX→kernel path works)'
else
    echo '  (not present — kernel may have aged it out or rejected it)'
fi

# ── Wait for binary to complete its full duration ──
echo ''
echo '--- Waiting for binary to finish ---'
wait \$TEST_PID
TEST_EXIT=\$?

sudo ip link del $TAP_NAME 2>/dev/null || true

echo ''
echo '--- Test binary exited with code: '\$TEST_EXIT' ---'
exit \$TEST_EXIT
EOF

echo ""
echo "=== Done ==="
