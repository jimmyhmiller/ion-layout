#!/usr/bin/env bash
# Generate real compiler dumps, verify every phase and render a comparison gallery.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/target/compiler-validation"
mkdir -p "$OUT/v8" "$OUT/rust" "$OUT/llvm" "$OUT/graphs"
node --allow-natives-syntax --trace-turbo --trace-turbo-filter='compiler*' \
  --trace-turbo-path="$OUT/v8" --trace-turbo-cfg-file="$OUT/v8/turbo.cfg" \
  examples/compiler/v8.js > "$OUT/v8/generation.log" 2>&1
for f in "$OUT"/v8/turbo-*.json; do
  node scripts/v8-json-to-dot.mjs "$f" "$OUT/graphs"
done
rustc +nightly -Z dump-mir=classify -Z dump-mir-graphviz \
  -Z "dump-mir-dir=$OUT/rust" examples/rustc-mir.rs -o "$OUT/rust/sample"
clang -O1 -emit-llvm -S examples/compiler/cfg.c -o "$OUT/llvm/cfg.ll"
(cd "$OUT/llvm" && opt -passes=dot-cfg -disable-output cfg.ll > generation.log 2>&1)
node scripts/verify.mjs "$OUT"/graphs/*.dot "$OUT"/rust/*.dot "$OUT"/llvm/.*.dot
python3 scripts/compiler-gallery.py
