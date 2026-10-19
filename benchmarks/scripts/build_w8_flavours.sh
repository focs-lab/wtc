#!/usr/bin/env bash
#
# Builds W8 (benchmarks/workloads/w8_lock_batch_wtc.cpp) as written and with
# its loop's critical sections batched:
#
#   w8_plain    -DW8_PLAIN
#   w8_batched  marked WTC_BATCH
#
# and checks that the batched build was batched and the plain one not.
# Never run this inside a `bench` reservation; it compiles.
#
#   build_w8_flavours.sh <out-dir>
set -euo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/benchmarks/workloads/w8_lock_batch_wtc.cpp"
CC="$ROOT/tools/wtc-cc"
mkdir -p "$OUT"
export WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
INC="-I$ROOT/benchmarks/harness"
LOCK=()
command -v machine-lock >/dev/null && LOCK=(machine-lock --lane wtc --cores 4 --mem 8G --minutes 15 --why "wtc w8 build" --)

build() { # name, extra wtc-cc args...
  local name=$1; shift
  "${LOCK[@]}" "$CC" --toolchain "$ROOT/build-llvm" $INC -O2 "$@" "$SRC" -o "$OUT/$name" \
    2>"$OUT/$name.log" || { echo "$name: FAILED"; tail -5 "$OUT/$name.log"; return 1; }
  local batched=no
  grep -q "critical sections in this loop batched" "$OUT/$name.log" && batched=yes
  printf '%-12s batched %-3s  ' "$name" "$batched"
  "$OUT/$name" --n 200000 --json /dev/null
}

build w8_plain -DW8_PLAIN
build w8_batched
echo
echo "w8_batched must say batched yes and w8_plain no; the checksums must agree."
