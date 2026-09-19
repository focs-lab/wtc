#!/usr/bin/env bash
# Builds the queue benchmark two ways from the same source -- baseline
# (wtc::queue<int>, round-tripped unchanged) and optimized (the same queue
# substituted with wtc::spsc_queue by --select-queue-impl) -- plus
# spsc_direct, the hand-written ceiling, then runs each.
#
#   LLVM_BUILD  CIR-enabled LLVM build tree (default ~/Development/llvm-project/build-cir)
#   WTC_BUILD   this project's build tree   (default <repo>/build)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
LLVM_BUILD="${LLVM_BUILD:-$HOME/Development/llvm-project/build-cir}"
WTC_BUILD="${WTC_BUILD:-$repo/build}"
out="$WTC_BUILD/benchmarks"
mkdir -p "$out"

clangxx="$LLVM_BUILD/bin/clang++"
cir_opt="$LLVM_BUILD/bin/cir-opt"
translate="$LLVM_BUILD/bin/mlir-translate"
llc="$LLVM_BUILD/bin/llc"
wtc_opt="$WTC_BUILD/bin/wtc-opt"

emit_cir() { # <source> <out.cir>
  "$clangxx" -std=c++20 -I "$repo/shim" \
    -isysroot "$(xcrun --show-sdk-path)" \
    -isystem "$(xcrun --show-sdk-path)/../../usr/include/c++/v1" \
    -emit-cir "$1" -o "$2"
}

link() { # <in.cir> <name>
  "$cir_opt" "$1" --cir-to-llvm -o "$out/$2_llvm.mlir"
  "$translate" --allow-unregistered-dialect --mlir-to-llvmir "$out/$2_llvm.mlir" -o "$out/$2.ll"
  "$llc" -filetype=obj "$out/$2.ll" -o "$out/$2.o"
  xcrun clang++ "$out/$2.o" -o "$out/$2"
}

emit_cir "$here/queue_bench.cpp" "$out/queue_bench.cir"
"$wtc_opt" "$out/queue_bench.cir" --allow-unregistered-dialect \
  --lift-cir-to-wtc -o "$out/queue_bench_lifted.mlir"

# baseline: identical pipeline minus --select-queue-impl
"$wtc_opt" "$out/queue_bench_lifted.mlir" --allow-unregistered-dialect \
  --queue-ownership-analysis --lower-wtc-to-cir \
  --reconcile-unrealized-casts --canonicalize -o "$out/queue_bench_baseline.cir"
link "$out/queue_bench_baseline.cir" queue_bench_baseline

"$wtc_opt" "$out/queue_bench_lifted.mlir" --allow-unregistered-dialect \
  --queue-ownership-analysis --select-queue-impl --lower-wtc-to-cir \
  --reconcile-unrealized-casts --canonicalize -o "$out/queue_bench_optimized.cir"
link "$out/queue_bench_optimized.cir" queue_bench_optimized

emit_cir "$here/spsc_direct.cpp" "$out/spsc_direct.cir"
link "$out/spsc_direct.cir" spsc_direct

for bin in queue_bench_baseline queue_bench_optimized spsc_direct; do
  echo "== $bin"
  "$out/$bin"
done
