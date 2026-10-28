#!/usr/bin/env bash
#
# Builds W8 (benchmarks/workloads/w8_lock_batch_wtc.cpp) as written, with
# its loop's critical sections batched by the compiler, and with the same
# batch written out by hand:
#
#   w8_plain            -DW8_PLAIN
#   w8_batched          marked WTC_BATCH, batches of 64
#   w8_hand             -DW8_HAND_BATCH, batches of 64
#   w8_<batched|hand>_k<K>   K: 8 512, the batch size swept
#
# and checks that the compiler's batched builds were batched and the others
# not. W8 reads its work at run time, which the cost model cannot bound
# (decisions/0006), so these builds turn the model off: they measure the
# transformation, not the decision to make it.
#
# With W8_COST_LEVELS set (e.g. "0 20 50 100 200 500", nanoseconds) and
# W8_ROUNDS_PER_NS (the work rounds a nanosecond on the host the builds will
# run on: the `work_rounds` over `work_ns` a W8 line records there), it
# builds instead, for the cost model's validation, at each level L with the
# work fixed at round(L x rounds per ns) when the program is built:
#
#   w8c_plain_L   -DW8_PLAIN
#   w8c_off_L     marked WTC_BATCH, the model off: batched wherever legal
#   w8c_on_L      marked WTC_BATCH, the model on (W8_HOST, if set, is the
#                 class of that host; W8_NS_PER_UNIT its constant)
#
# and writes each build's remark to decisions.txt. Never run this inside a
# `bench` reservation; it compiles.
#
#   build_w8_flavours.sh <out-dir>
set -euo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/benchmarks/workloads/w8_lock_batch_wtc.cpp"
CC="$ROOT/tools/wtc-cc"
mkdir -p "$OUT"
: > "$OUT/flavours.txt"
export WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
INC="-I$ROOT/benchmarks/harness"
LOCK=()
command -v machine-lock >/dev/null && LOCK=(machine-lock --lane wtc --cores 4 --mem 8G --minutes 15 --why "wtc w8 build" --)

build() { # name, extra wtc-cc args...
  local name=$1; shift
  echo "flavour $name=-O2 $*" >> "$OUT/flavours.txt"
  "${LOCK[@]}" "$CC" --toolchain "$ROOT/build-llvm" $INC -O2 "$@" "$SRC" -o "$OUT/$name" \
    2>"$OUT/$name.log" || { echo "$name: FAILED"; tail -5 "$OUT/$name.log"; return 1; }
  local batched=no
  grep -q "critical sections in this loop batched" "$OUT/$name.log" && batched=yes
  printf '%-14s batched %-3s  ' "$name" "$batched"
  "$OUT/$name" --n 200000 --json /dev/null
}

if [ -n "${W8_COST_LEVELS:-}" ]; then
  : "${W8_ROUNDS_PER_NS:?the work rounds a nanosecond on the host the builds will run on}"
  NSU=()
  [ -n "${W8_HOST:-}" ] && NSU+=(--batch-host "$W8_HOST")
  [ -n "${W8_NS_PER_UNIT:-}" ] && NSU+=(--batch-ns-per-unit "$W8_NS_PER_UNIT")
  : > "$OUT/decisions.txt"
  for l in $W8_COST_LEVELS; do
    r=$(awk -v l="$l" -v p="$W8_ROUNDS_PER_NS" 'BEGIN { printf "%d", l * p + 0.5 }')
    build "w8c_plain_$l" -DW8_PLAIN -DW8_WORK_ROUNDS="$r"
    build "w8c_off_$l" --batch-cost off -DW8_WORK_ROUNDS="$r"
    build "w8c_on_$l" "${NSU[@]}" -DW8_WORK_ROUNDS="$r"
    printf '%s ns, %s rounds: %s\n' "$l" "$r" \
      "$(grep -o 'critical sections in this loop.*' "$OUT/w8c_on_$l.log" | head -1)" >> "$OUT/decisions.txt"
  done
  echo
  echo "every w8c_off_* must say batched yes, every w8c_plain_* no; w8c_on_* as"
  echo "decisions.txt says; the checksums must agree."
else
  build w8_plain -DW8_PLAIN
  build w8_batched --batch-cost off
  build w8_hand -DW8_HAND_BATCH
  for k in 8 512; do
    build "w8_batched_k$k" --batch-cost off -DWTC_LOCK_BATCH_K=$k
    build "w8_hand_k$k" -DW8_HAND_BATCH -DWTC_LOCK_BATCH_K=$k
  done
  echo
  echo "every w8_batched* must say batched yes, w8_plain and w8_hand* no (the hand"
  echo "builds batch in the source); the checksums must agree."
fi

# What was built, for meta.json: the toolchain and each flavour's flags.
# Outside a git checkout (a tree copied to another host) WTC_REV and
# WTC_CLANGIR_REV name what was copied.
{
  cir=${WTC_CLANGIR_REV:-$(git -c safe.directory="$ROOT" -C "$ROOT" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)}
  rev=${WTC_REV:-$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null)$([ -n "$(git -c safe.directory="$ROOT" -C "$ROOT" status --porcelain -- . ":(exclude)benchmarks/results" 2>/dev/null)" ] && echo ' (dirty)')}
  echo "compiler=tools/wtc-cc, ClangIR ${cir:-unknown}"
  echo "wtc=${rev:-unknown}"
  cat "$OUT/flavours.txt"
} > "$OUT/build-info.txt"
