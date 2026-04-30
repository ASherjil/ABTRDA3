#!/usr/bin/env bash
# Stage the test binary + config + gem_counter into /dev/shm on mkdev50 via SSH.
# /dev/shm is tmpfs — RAM-backed, survives the NFS mount dropping when macb is
# unbound.  Files are written as root so they persist and are ready for sudo.
#
# Run from anywhere — the script cd's to the project root itself:
#   ./diagnostic_scripts/cp_to_shm.sh
# Then on mkdev50:
#   cd /dev/shm
#   sudo ./abtrda3_test --txgen --config abtrda3_test.toml --count 100000
set -euo pipefail

cd "$(dirname "$0")/.."

TARGET=cfd-865-mkdev50
BIN=build/arm64-release/test/abtrda3_test
CFG=test/abtrda3_test.toml
CNT=diagnostic_scripts/gem_counter.sh
KO=src/Cadence_GEM/gem_uio/gem_uio.ko

# gem_uio.ko is optional from this script's POV — it must be built separately
# via `cd src/Cadence_GEM/gem_uio && make KDIR=... ARCH=arm64 CROSS_COMPILE=...`
# and the vermagic patched to match the running kernel (see gem_uio/Makefile).
files_to_send=("$BIN" "$CFG" "$CNT")
if [[ -f "$KO" ]]; then
    files_to_send+=("$KO")
    have_ko=1
else
    have_ko=0
fi

for f in "$BIN" "$CFG" "$CNT"; do
    if [[ ! -f "$f" ]]; then
        echo "error: $f not found — run from project root after building" >&2
        exit 1
    fi
done

echo "Deploying to $TARGET:/dev/shm/ ..."
ssh "$TARGET" "sudo rm -f /dev/shm/abtrda3_test /dev/shm/abtrda3_test.toml /dev/shm/gem_counter.sh /dev/shm/gem_uio.ko"

# Pipe tar over ssh — avoids scp ownership issues.
tar cf - "${files_to_send[@]}" \
  | ssh "$TARGET" "cd /dev/shm && sudo tar xf - --strip-components=0 --transform='s|.*/||' && sudo chmod +x abtrda3_test gem_counter.sh"

echo "Staged to $TARGET:/dev/shm:"
ssh "$TARGET" "ls -la /dev/shm/abtrda3_test /dev/shm/abtrda3_test.toml /dev/shm/gem_counter.sh $([[ $have_ko -eq 1 ]] && echo /dev/shm/gem_uio.ko)"
echo
if [[ $have_ko -eq 1 ]]; then
    echo "Next on $TARGET:"
    echo "  sudo insmod /dev/shm/gem_uio.ko        # once per boot"
    echo "  cd /dev/shm  &&  sudo ./abtrda3_test --txgen --config abtrda3_test.toml"
else
    echo "Next on $TARGET:  cd /dev/shm  &&  sudo ./abtrda3_test --txgen --config abtrda3_test.toml"
    echo "(gem_uio.ko not built; descriptor pool open will fail without it)"
fi
