#!/usr/bin/env bash
# Builds the queue benchmark two ways from the same source -- baseline
# (wtc::queue<int>, lifted and lowered back unchanged) and optimized (the
# same pipeline plus --select-queue-impl) -- and spsc_direct, the
# hand-written ceiling. Runs each RUNS times and reports the median.
#
#   LLVM_BUILD  the pinned toolchain, built or installed; default /opt/llvm,
#               where the toolchain image puts it
#   WTC_BUILD   this project's build tree; default <repo>/build
#   RUNS        runs per variant; default 5
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
LLVM_BUILD="${LLVM_BUILD:-/opt/llvm}"
WTC_BUILD="${WTC_BUILD:-$repo/build}"
RUNS="${RUNS:-5}"
out="$WTC_BUILD/benchmarks"
mkdir -p "$out"

for tool in clang cir-opt mlir-translate llc; do
  if [ ! -x "$LLVM_BUILD/bin/$tool" ]; then
    echo "no $tool in $LLVM_BUILD/bin; set LLVM_BUILD to the pinned toolchain" >&2
    exit 1
  fi
done
wtc_opt="$WTC_BUILD/bin/wtc-opt"
[ -x "$wtc_opt" ] || { echo "no wtc-opt in $WTC_BUILD/bin; set WTC_BUILD" >&2; exit 1; }

# A clang built from source does not know where macOS keeps the SDK, and the
# final link needs the system's C++ runtime either way.
sysroot=()
link_cxx=(c++)
if [ "$(uname)" = Darwin ]; then
  sysroot=(-isysroot "$(xcrun --show-sdk-path)")
  link_cxx=(xcrun clang++)
fi

emit_cir() { # <source> <out.cir>
  "$LLVM_BUILD/bin/clang" -x c++ -std=c++17 -O1 -I "$repo/shim" "${sysroot[@]}" \
    -fclangir -emit-cir "$1" -o "$2"
}

# cir-to-llvm leaves CIR types behind in attributes, even for plain clang
# output, so mlir-translate has to be told not to choke on them.
link() { # <in.cir> <name>
  "$LLVM_BUILD/bin/cir-opt" "$1" --cir-to-llvm -o "$out/$2.llvm.mlir"
  "$LLVM_BUILD/bin/mlir-translate" --allow-unregistered-dialect --mlir-to-llvmir \
    "$out/$2.llvm.mlir" -o "$out/$2.ll"
  "$LLVM_BUILD/bin/llc" -O2 -filetype=obj "$out/$2.ll" -o "$out/$2.o"
  "${link_cxx[@]}" "$out/$2.o" -o "$out/$2" -lpthread
}

emit_cir "$here/queue_bench.cpp" "$out/queue_bench.cir"

"$wtc_opt" "$out/queue_bench.cir" --mlir-print-op-on-diagnostic=false \
  --lift-cir-to-wtc --queue-ownership-analysis --lower-wtc-to-cir \
  --reconcile-unrealized-casts -o "$out/queue_bench_baseline.cir" > /dev/null
link "$out/queue_bench_baseline.cir" queue_bench_baseline

"$wtc_opt" "$out/queue_bench.cir" --mlir-print-op-on-diagnostic=false \
  --lift-cir-to-wtc --queue-ownership-analysis --select-queue-impl --lower-wtc-to-cir \
  --reconcile-unrealized-casts -o "$out/queue_bench_optimized.cir" > /dev/null
# Linkage keywords differ by target (ELF globals are dso_local), so match
# only the name and the type.
grep -qE 'cir\.global .*@g_queue = #cir\.zero : !rec_wtc3A3Aruntime' "$out/queue_bench_optimized.cir" \
  || { echo "--select-queue-impl did not substitute g_queue" >&2; exit 1; }
link "$out/queue_bench_optimized.cir" queue_bench_optimized

emit_cir "$here/spsc_direct.cpp" "$out/spsc_direct.cir"
link "$out/spsc_direct.cir" spsc_direct

for bin in queue_bench_baseline queue_bench_optimized spsc_direct; do
  rates=()
  for _ in $(seq "$RUNS"); do
    line="$("$out/$bin")"
    rates+=("$(sed -n 's/.*items_per_sec=\([0-9]*\).*/\1/p' <<<"$line")")
  done
  median="$(printf '%s\n' "${rates[@]}" | sort -n | sed -n "$(( (RUNS + 1) / 2 ))p")"
  echo "$bin: median $median items/s over $RUNS runs (${rates[*]})"
done
