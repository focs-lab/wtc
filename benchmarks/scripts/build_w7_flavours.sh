#!/usr/bin/env bash
#
# Builds W7 (benchmarks/workloads/w7_batch_wtc.cpp) for every pairing of
# implementation and batching, for 8- and 64-byte elements:
#
#   w7_<impl>_<plain|batch>_e<8|64>   impl: generic, spsc, inline
#
# and checks that each build carries the implementation it is named for
# and that the batched builds were batched. Never run this inside a `bench`
# reservation; it compiles.
#
#   build_w7_flavours.sh <out-dir>
set -euo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/benchmarks/workloads/w7_batch_wtc.cpp"
CC="$ROOT/tools/wtc-cc"
mkdir -p "$OUT"
: > "$OUT/flavours.txt"
export WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
INC="-I$ROOT/benchmarks/queues -I$ROOT/benchmarks/harness"
LOCK=()
command -v machine-lock >/dev/null && LOCK=(machine-lock --lane wtc --cores 4 --mem 8G --minutes 15 --why "wtc w7 build" --)

build() { # name, extra wtc-cc args...
  local name=$1; shift
  echo "flavour $name=-O2 $*" >> "$OUT/flavours.txt"
  "${LOCK[@]}" "$CC" --toolchain "$ROOT/build-llvm" $INC -O2 "$@" "$SRC" -o "$OUT/$name" \
    2>"$OUT/$name.log" || { echo "$name: FAILED"; tail -5 "$OUT/$name.log"; return 1; }
  local batched=no
  grep -q "pushes in this loop batched" "$OUT/$name.log" && batched=yes
  printf '%-24s batched %-3s  ' "$name" "$batched"
  "$OUT/$name" --n 100000 --json /dev/null 2>/dev/null
}

for e in E8 E64; do
  s=$(echo "$e" | tr 'E' 'e')
  build "w7_generic_plain_$s" --no-specialize -DW7_ELEM=$e
  build "w7_generic_batch_$s" --no-specialize -DW7_ELEM=$e -DW7_BATCH
  build "w7_spsc_plain_$s"    --no-specialize -DW7_ELEM=$e -DW7_TAG=1
  build "w7_spsc_batch_$s"    --no-specialize -DW7_ELEM=$e -DW7_TAG=1 -DW7_BATCH
  build "w7_inline_plain_$s"  -DW7_ELEM=$e
  build "w7_inline_batch_$s"  -DW7_ELEM=$e -DW7_BATCH
done
echo
echo "every *_batch_* must say batched yes and every *_plain_* no; the tag must"
echo "match the name; the checksum must agree within each element size."

# What was built, for meta.json: the toolchain and each flavour's flags.
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$ROOT" -C "$ROOT" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null)$([ -n "$(git -c safe.directory="$ROOT" -C "$ROOT" status --porcelain 2>/dev/null)" ] && echo ' (dirty)')"
  cat "$OUT/flavours.txt"
} > "$OUT/build-info.txt"
