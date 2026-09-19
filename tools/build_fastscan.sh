#!/usr/bin/env bash
# Compile the C signature scanner (tools/fastscan) from source.
# It is a drop-in accelerator for verify_signatures.py / fast_matrix.py:
# scanning a 300MB libminecraftpe.so takes ~3s instead of minutes.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CC_BIN="${CC:-}"
if [ -z "$CC_BIN" ]; then
    for c in clang-17 clang gcc cc; do command -v "$c" >/dev/null 2>&1 && { CC_BIN="$c"; break; }; done
fi
[ -n "$CC_BIN" ] || { echo "error: no C compiler found (clang-17 / gcc)" >&2; exit 1; }
echo "compiling with $CC_BIN"
"$CC_BIN" -O2 -o "$HERE/fastscan" "$HERE/fastscan.c"
echo "ok: $HERE/fastscan"
