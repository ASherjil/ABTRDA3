# =============================================================================
# BpfCompile.cmake — Compile BPF programs and embed as C headers
# =============================================================================
#
# bpf_compile(
#   TARGET     <cmake-target>      # Target that will #include the generated header
#   SOURCE     <path/prog.bpf.c>   # BPF source file
#   OUTPUT_DIR <dir>               # Where to generate .bpf.o and _embed.h
#   INCLUDES   <dir> ...           # Extra -I directories for clang
# )

find_program(CLANG_BPF clang REQUIRED)

function(bpf_compile)
    cmake_parse_arguments(BPF "" "TARGET;SOURCE;OUTPUT_DIR" "INCLUDES" ${ARGN})

    get_filename_component(stem "${BPF_SOURCE}" NAME_WE)   # "xdp_filter"
    set(bpf_obj    "${BPF_OUTPUT_DIR}/${stem}.bpf.o")
    set(bpf_header "${BPF_OUTPUT_DIR}/${stem}_embed.h")

    # Build -I flags for BPF includes (libbpf headers, etc.)
    set(inc_flags "")
    foreach(dir IN LISTS BPF_INCLUDES)
        list(APPEND inc_flags "-I${dir}")
    endforeach()

    # Step 1: .bpf.c  ->  .bpf.o  (BPF bytecode in ELF container)
    #
    # -target bpf : emit BPF bytecode, not x86 (only clang has this backend)
    # -O2         : required -- unoptimised BPF often fails the kernel verifier
    # -g          : emit BTF debug info (required by libbpf for global var rewrite)
    add_custom_command(
        OUTPUT  "${bpf_obj}"
        COMMAND ${CLANG_BPF}
                -target bpf
                -O2 -g
                -Wall
                ${inc_flags}
                -c "${BPF_SOURCE}"
                -o "${bpf_obj}"
        DEPENDS "${BPF_SOURCE}"
        COMMENT "[BPF] ${stem}.bpf.c -> ${stem}.bpf.o"
        VERBATIM
    )

    # Step 2: .bpf.o  ->  _embed.h  (C byte array via BinToHeader.cmake)
    add_custom_command(
        OUTPUT  "${bpf_header}"
        COMMAND ${CMAKE_COMMAND}
                "-DINPUT=${bpf_obj}"
                "-DOUTPUT=${bpf_header}"
                "-DVAR=${stem}_bpf"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BinToHeader.cmake"
        DEPENDS "${bpf_obj}"
        COMMENT "[BPF] ${stem}.bpf.o -> ${stem}_embed.h"
        VERBATIM
    )

    # Ensure the header is generated before the target compiles
    add_custom_target(${stem}_embed DEPENDS "${bpf_header}")
    add_dependencies(${BPF_TARGET} ${stem}_embed)

    # Let the target find the generated header via #include "xdp_filter_embed.h"
    target_include_directories(${BPF_TARGET} PRIVATE "${BPF_OUTPUT_DIR}")
endfunction()
