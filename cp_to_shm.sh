#!/usr/bin/env bash
# Stage the test binary + config + gem_counter into /dev/shm so they (and the
# shell's CWD after `cd /dev/shm`) survive the NFS mount dropping when macb is
# unbound. /dev/shm is tmpfs — RAM-backed, cleared on reboot.
#
# Run from the project root (ABTRDA3/):
#   ./cp_to_shm.sh
# Then:
#   cd /dev/shm
#   sudo ./abtrda3_test --txgen --config abtrda3_test.toml --count 100000
set -euo pipefail

BIN=build/arm64-release/test/abtrda3_test
CFG=test/abtrda3_test.toml
CNT=gem_counter.sh

for f in "$BIN" "$CFG" "$CNT"; do
    if [[ ! -f "$f" ]]; then
        echo "error: $f not found — run from project root after building" >&2
        exit 1
    fi
done

sudo cp "$BIN" /dev/shm/
sudo cp "$CFG" /dev/shm/
sudo cp "$CNT" /dev/shm/
sudo chmod +x /dev/shm/abtrda3_test /dev/shm/gem_counter.sh

echo "Staged to /dev/shm:"
ls -la /dev/shm/abtrda3_test /dev/shm/abtrda3_test.toml /dev/shm/gem_counter.sh
echo
echo "Next:  cd /dev/shm  &&  sudo ABTRDA3_REBIND=1 ./abtrda3_test --txgen --config abtrda3_test.toml"
