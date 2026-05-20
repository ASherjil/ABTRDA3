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

# ── 4. SCP to remote ──
echo ""
echo "── SCP to remote server ──"
echo "1) Skip"
echo "2) scp → asherjil@100.72.135.7:~/ABTTiming/ABTRDA3/"
read -rp "SCP [1/2]: " scp_choice

if [[ "${scp_choice}" == "2" ]]; then
    remote="asherjil@100.72.135.7:~/ABTTiming/ABTRDA3/"
    binary="${build_dir}/test/abtrda3_test"
    config="test/abtrda3_test.toml"
    bpf="${build_dir}/test/af_xdp_kern.o"

    if [[ ! -f "${binary}" ]]; then
        echo "ERROR: binary not found: ${binary}"
        exit 1
    fi
    if [[ ! -f "${config}" ]]; then
        echo "ERROR: config not found: ${config}"
        exit 1
    fi
    if [[ ! -f "${bpf}" ]]; then
        echo "ERROR: BPF object not found: ${bpf}"
        exit 1
    fi

    echo "Transferring abtrda3_test + abtrda3_test.toml + af_xdp_kern.o → ${remote} ..."
    scp "${binary}" "${config}" "${bpf}" "${remote}"
    echo "── SCP complete ──"
fi
