#!/bin/bash
# Profile the Intel I210 PMD hot path using perf stat
#
# Usage: ./profile_hotpath.sh [server_host] [client_host]
#
# Profiles BOTH server and client under perf stat.
# Set send_interval_us = 0 in the TOML config for back-to-back
# profiling, otherwise most client time is spent sleeping.

SERVER=${1:-cfc-865-mkdev16}
CLIENT=${2:-cfc-865-mkdev30}
BUILD_DIR=/user/asherjil/ABTTiming/ABTRDA3/build/x86_64-release/test
CONFIG=../../../test/abtrda3_test.toml

echo "=== Intel I210 PMD — perf stat (server + client) ==="
echo "Server: $SERVER | Client: $CLIENT"
echo ""

# Ensure perf is installed and configured on both machines
for HOST in $SERVER $CLIENT; do
    ssh $HOST "
        if ! command -v perf &>/dev/null; then
            echo \"Installing linux-perf on $HOST...\"
            sudo apt-get install -y linux-perf > /dev/null 2>&1
        fi
        sudo sysctl -qw kernel.perf_event_paranoid=-1
    "
    echo "perf ready on $HOST"
done
echo ""

PERF_EVENTS="cycles,instructions,cache-misses,cache-references,\
L1-dcache-load-misses,L1-dcache-loads,\
LLC-load-misses,LLC-loads,\
branch-misses,dTLB-load-misses"

# Start server under perf stat
echo "Starting server under perf stat on $SERVER..."
ssh $SERVER "cd $BUILD_DIR && sudo perf stat -e $PERF_EVENTS \
    -- ./abtrda3_test --server --config $CONFIG" &
SERVER_PID=$!

sleep 10

# Start client under perf stat (blocks until client finishes)
echo "Starting client under perf stat on $CLIENT..."
ssh $CLIENT "cd $BUILD_DIR && sudo perf stat -e $PERF_EVENTS \
    -- ./abtrda3_test --client --config $CONFIG"

echo ""
echo "Client done. Killing server..."
ssh $SERVER "sudo pkill -SIGINT abtrda3_test" 2>/dev/null
wait $SERVER_PID 2>/dev/null
