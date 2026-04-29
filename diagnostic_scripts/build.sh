#!/bin/bash
set -e

cd "$(dirname "$0")/.."

# ── 1. Clean or build only ──
echo "1) Clean + Build"
echo "2) Build only"
read -rp "Choice [1/2]: " clean_choice

# ── 2. Toolchain ──
echo ""
echo "1) x86_64 (PCIe)"
echo "2) arm64  (AXI)"
read -rp "Toolchain [1/2]: " arch_choice

# ── 3. Build type ──
echo ""
echo "1) Debug"
echo "2) Release"
read -rp "Build type [1/2]: " type_choice

# Resolve preset name
case "${arch_choice}" in
    1) arch="x86_64" ;;
    2) arch="arm64"  ;;
    *) echo "Invalid toolchain"; exit 1 ;;
esac

case "${type_choice}" in
    1) type="debug"   ;;
    2) type="release"  ;;
    *) echo "Invalid build type"; exit 1 ;;
esac

preset="${arch}-${type}"
build_dir="build/${preset}"

echo ""
echo "── Preset: ${preset} ──"

# Clean if requested
if [[ "${clean_choice}" == "1" ]]; then
    echo "Cleaning ${build_dir}..."
    rm -rf "${build_dir}"
fi

# Configure if needed
if [[ ! -f "${build_dir}/build.ninja" ]]; then
    echo "Configuring..."
    cmake --preset="${preset}" -G Ninja
fi

# Build
echo "Building..."
cmake --build "${build_dir}" -j"$(nproc)"

echo ""
echo "── Done: ${build_dir} ──"
