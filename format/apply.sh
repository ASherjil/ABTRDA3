#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Areeb Sherjil
# apply.sh — clang-tidy (with fixes) then clang-format over the whole codebase.
#
# Usage:  format/apply.sh [<build-dir>]
#   <build-dir>  a configured build holding compile_commands.json — clang-tidy
#                needs the real compile flags. Use the FULL preset so every
#                transport's header is analysed (build/x86_64-release-full).
#                Omitted or missing => the tidy pass is skipped, format still runs.
#
# Tools are found as unversioned or -NN suffixed (clang-tidy / clang-tidy-21 ...).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-}"

find_tool() {
    local base="$1"
    local c
    for c in "$base" "$base-21" "$base-20" "$base-19" "$base-18"; do
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

mapfile -t FILES < <(find "$ROOT/src" "$ROOT/test" -type f \
    \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' \) | sort)
echo "[format] ${#FILES[@]} source files under src/ and test/"

# ── 1. clang-tidy ─────────────────────────────────────────────────────────────
if [ -n "$BUILD" ] && [ -f "$BUILD/compile_commands.json" ]; then
    TIDY="$(find_tool clang-tidy || true)"
    RUN_TIDY="$(find_tool run-clang-tidy || true)"
    APPLY="$(find_tool clang-apply-replacements || true)"
    if [ -z "$TIDY" ] || [ -z "$RUN_TIDY" ] || [ -z "$APPLY" ]; then
        echo "[format] clang-tidy / run-clang-tidy / clang-apply-replacements not all found — skipping tidy" >&2
    else
        echo "[format] clang-tidy: $TIDY  (compile db: $BUILD)"
        "$RUN_TIDY" -p "$BUILD" \
            -clang-tidy-binary "$TIDY" \
            -clang-apply-replacements-binary "$APPLY" \
            -config-file="$ROOT/format/.clang-tidy" \
            -header-filter='.*/[Aa][Bb][Tt][Rr][Dd][Aa]3/(src|test)/.*' \
            -fix -quiet -j "$(nproc)" \
            "$ROOT/src" "$ROOT/test" || echo "[format] clang-tidy reported diagnostics (fixes applied where possible)" >&2
    fi
else
    echo "[format] no build dir / compile_commands.json given — skipping clang-tidy"
fi

# ── 2. clang-format ───────────────────────────────────────────────────────────
FMT="$(find_tool clang-format)" || { echo "[format] clang-format not found" >&2; exit 1; }
echo "[format] clang-format: $FMT"
"$FMT" --style="file:$ROOT/format/.clang-format" -i "${FILES[@]}"
echo "[format] done"
