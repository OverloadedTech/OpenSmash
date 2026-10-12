#!/bin/sh
# Creates zig cc / c++ / ar / ranlib wrapper scripts for cmake/zig-*.cmake (CMake needs single-executable tools).
# Usage: tools/make-zig-wrappers.sh [outdir] [python-with-ziglang]   (pip install ziglang)
OUT=${1:-build/zig-wrappers}
PY=${2:-python3}
mkdir -p "$OUT"
for t in x86_64-windows-gnu aarch64-linux-gnu; do
  for k in cc c++; do
    f="$OUT/zig-$k-$t"
    printf '#!/bin/sh\nexec %s -m ziglang %s -target %s "$@"\n' "$PY" "$k" "$t" > "$f"
    chmod +x "$f"
  done
done
for k in ar ranlib; do
  printf '#!/bin/sh\nexec %s -m ziglang %s "$@"\n' "$PY" "$k" > "$OUT/zig-$k"
  chmod +x "$OUT/zig-$k"
done
echo "wrappers in $OUT"
