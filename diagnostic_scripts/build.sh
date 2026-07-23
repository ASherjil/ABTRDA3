#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Areeb Sherjil
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
echo "3) Release + Profiling  (rdtsc/HW-ts instrumentation, x86_64 only)"
read -rp "Build type [1/2/3]: " type_choice

# ── 4. Transports ──
echo ""
echo "1) Default  (packet_mmap + af_xdp)"
echo "2) All      (+ DPDK, Verbs, ef_vi, intel_i210 — the rtserver benchmark build)"
read -rp "Transports [1/2]: " transports_choice

# Resolve preset name
case "${arch_choice}" in
    1) arch="x86_64" ;;
    2) arch="arm64"  ;;
    *) echo "Invalid toolchain"; exit 1 ;;
esac

case "${type_choice}" in
    1) type="debug"   ;;
    2) type="release"  ;;
    3) type="relprof" ;;
    *) echo "Invalid build type"; exit 1 ;;
esac

# Profiling build is x86_64-only (rdtscp / x86 timestamp intrinsics in Profiling.hpp).
if [[ "${type}" == "relprof" && "${arch}" != "x86_64" ]]; then
    echo "Profiling (relprof) is x86_64-only — re-run and pick the x86_64 toolchain."; exit 1
fi

preset="${arch}-${type}"

# All-transports maps to the -full presets (ABTRDA3_WITH_ALL=ON); only the
# x86_64 release/relprof pair has them — the vendor stacks are x86-only here.
if [[ "${transports_choice}" == "2" ]]; then
    if [[ "${preset}" != "x86_64-release" && "${preset}" != "x86_64-relprof" ]]; then
        echo "All-transports needs x86_64 Release or Release+Profiling."; exit 1
    fi
    preset="${preset}-full"
fi

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
