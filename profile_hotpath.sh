#!/bin/bash
# Profile the Intel I210 PMD hot path
#
# Usage:
#   ./profile_hotpath.sh                          # with perf stat (default)
#   ./profile_hotpath.sh --no-perf                # without perf (pure latency test)
#   ./profile_hotpath.sh mkdev17 mkdev18          # custom hosts
#   ./profile_hotpath.sh --no-perf mkdev17 mkdev18

NO_PERF=false
HOSTS=()

for arg in "$@"; do
    if [ "$arg" = "--no-perf" ]; then
        NO_PERF=true
    else
        HOSTS+=("$arg")
    fi
done

SERVER=${HOSTS[0]:-cfc-865-mkdev16}
CLIENT=${HOSTS[1]:-cfc-865-mkdev30}
BUILD_DIR=/user/asherjil/ABTTiming/ABTRDA3/build/x86_64-release/test
CONFIG=../../../test/abtrda3_test.toml

echo "=== Intel I210 PMD — latency test ==="
echo "Server: $SERVER | Client: $CLIENT | perf: $($NO_PERF && echo off || echo on)"
echo ""

if [ "$NO_PERF" = true ]; then
    WRAP=""
else
    PERF_EVENTS="cycles,instructions,cache-misses,cache-references,\
L1-dcache-load-misses,L1-dcache-loads,\
LLC-load-misses,LLC-loads,\
branch-misses,dTLB-load-misses"
    WRAP="perf stat -e $PERF_EVENTS --"

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
fi

# Kill any stale abtrda3_test processes (SCHED_FIFO ignores SIGINT/SIGTERM)
for HOST in $SERVER $CLIENT; do
    ssh $HOST "sudo pkill -9 abtrda3_test" 2>/dev/null
done

# Start server
echo "Starting server on $SERVER..."
ssh $SERVER "cd $BUILD_DIR && sudo $WRAP ./abtrda3_test --server --config $CONFIG" &
SERVER_PID=$!

sleep 10

# Start client (blocks until done)
echo "Starting client on $CLIENT..."
ssh $CLIENT "cd $BUILD_DIR && sudo $WRAP ./abtrda3_test --client --config $CONFIG"

echo ""
echo "Client done. Killing server..."
ssh $SERVER "sudo pkill -SIGINT abtrda3_test" 2>/dev/null
wait $SERVER_PID 2>/dev/null
