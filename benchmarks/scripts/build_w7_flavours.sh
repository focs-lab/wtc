#!/usr/bin/env bash
#
# Builds W7 (benchmarks/workloads/w7_batch_wtc.cpp) for every pairing of
# implementation and batching, for 8- and 64-byte elements:
#
#   w7_<impl>_<plain|batch|hand>_e<8|64>   impl: generic, spsc, inline
#
# batch is the compiler's batching of a WTC_BATCH loop, hand the same batch
# written out through the shim's calls; both at the default batch size of
# 64. For 64-byte elements also the batch-size sweep:
#
#   w7_<impl>_<batch|hand>_k<K>_e64        K: 1 4 16 256 1024
#
# W7_SWEEP=0 leaves the sweep out. Checks that each build carries the
# implementation it is named for and that the compiler's batched builds
# were batched. Never run this inside a `bench` reservation; it compiles.
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

# How each implementation is reached: the generic and spsc builds keep the
# tag the source gives, the inline build is the compiler's choice.
impl_flags() {
  case "$1" in
    generic) echo "--no-specialize" ;;
    spsc)    echo "--no-specialize -DW7_TAG=1" ;;
    inline)  echo "" ;;
  esac
}

for e in E8 E64; do
  s=$(echo "$e" | tr 'E' 'e')
  for impl in generic spsc inline; do
    # shellcheck disable=SC2046
    build "w7_${impl}_plain_$s" $(impl_flags $impl) -DW7_ELEM=$e
    # shellcheck disable=SC2046
    build "w7_${impl}_batch_$s" $(impl_flags $impl) -DW7_ELEM=$e -DW7_BATCH
    # shellcheck disable=SC2046
    build "w7_${impl}_hand_$s"  $(impl_flags $impl) -DW7_ELEM=$e -DW7_HAND_BATCH
  done
done
if [ "${W7_SWEEP:-1}" != 0 ]; then
  for k in 1 4 16 256 1024; do
    for impl in generic spsc inline; do
      # shellcheck disable=SC2046
      build "w7_${impl}_batch_k${k}_e64" $(impl_flags $impl) -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=$k
      # shellcheck disable=SC2046
      build "w7_${impl}_hand_k${k}_e64"  $(impl_flags $impl) -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=$k
    done
  done
fi
echo
echo "every *_batch_* must say batched yes, every *_plain_* and *_hand_* no (the"
echo "hand builds batch in the source); the tag must match the name; the"
echo "checksum must agree within each element size."

# What was built, for meta.json: the toolchain and each flavour's flags.
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$ROOT" -C "$ROOT" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null)$([ -n "$(git -c safe.directory="$ROOT" -C "$ROOT" status --porcelain 2>/dev/null)" ] && echo ' (dirty)')"
  cat "$OUT/flavours.txt"
} > "$OUT/build-info.txt"
