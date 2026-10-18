#!/usr/bin/env bash
# The benchmark suite. Not run in CI: shared runners are too noisy for
# timings to mean anything. Run it by hand on a quiet machine.
#
# Every workload in workloads/ is built in up to three variants:
#   baseline   wtc::queue<T>, lifted and lowered back unchanged
#   optimized  the same, plus --select-queue-impl
#   direct     wtc::runtime::spsc_queue<T> by hand, the ceiling
# and each workload states whether the optimized build must be
# substituted. A build that disagrees fails the suite, so a number in the
# results always means what its column says.
#
# Variants are run interleaved, RUNS times each, so drift over the session
# affects them all alike. The medians, the spread and the environment are
# written to RESULTS.
#
#   LLVM_BUILD  the pinned toolchain, built or installed; default /opt/llvm,
#               where the toolchain image puts it
#   WTC_BUILD   this project's build tree; default <repo>/build
#   RUNS        runs per variant; default 5
#   RESULTS     where to write the report; default benchmarks/RESULTS.md
#   BENCH_CPUS  Linux only: CPUs to pin each run to with taskset, e.g. 2,3
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
LLVM_BUILD="${LLVM_BUILD:-/opt/llvm}"
WTC_BUILD="${WTC_BUILD:-$repo/build}"
RUNS="${RUNS:-5}"
RESULTS="${RESULTS:-$here/RESULTS.md}"
BENCH_CPUS="${BENCH_CPUS:-}"
out="$WTC_BUILD/benchmarks"
mkdir -p "$out"

# name               the optimized build must be   direct variant
workloads=(
  "spsc_throughput   substituted                    yes"
  "spsc_latency      substituted                    yes"
  "spsc_payload      substituted                    yes"
  "spsc_same_thread  substituted                    yes"
  "mpsc_control      kept                           no"
)

die() { echo "run.sh: $*" >&2; exit 1; }

for tool in clang cir-opt mlir-translate llc; do
  [ -x "$LLVM_BUILD/bin/$tool" ] || die "no $tool in $LLVM_BUILD/bin; set LLVM_BUILD to the pinned toolchain"
done
wtc_opt="$WTC_BUILD/bin/wtc-opt"
[ -x "$wtc_opt" ] || die "no wtc-opt in $WTC_BUILD/bin; set WTC_BUILD"
command -v python3 > /dev/null || die "python3 is needed to write the report"

# A clang built from source does not know where macOS keeps the SDK, and the
# final link needs the system's C++ runtime either way.
sysroot=()
link_cxx=(c++)
if [ "$(uname)" = Darwin ]; then
  sysroot=(-isysroot "$(xcrun --show-sdk-path)")
  link_cxx=(xcrun clang++)
fi

pin=()
pinning="not pinned"
if [ -n "$BENCH_CPUS" ]; then
  if command -v taskset > /dev/null; then
    pin=(taskset -c "$BENCH_CPUS")
    pinning="pinned to CPUs $BENCH_CPUS with taskset"
  else
    echo "run.sh: BENCH_CPUS is set but taskset is not available here; runs are not pinned" >&2
  fi
fi

emit_cir() { # <source> <out.cir> [extra clang flags...]
  local src="$1" dst="$2"
  shift 2
  "$LLVM_BUILD/bin/clang" -x c++ -std=c++17 -O1 -I "$repo/shim" ${sysroot[@]+"${sysroot[@]}"} "$@" \
    -fclangir -emit-cir "$src" -o "$dst"
}

# cir-to-llvm leaves CIR types behind in attributes, even for plain clang
# output, so mlir-translate has to be told not to choke on them.
link() { # <in.cir> <binary>
  "$LLVM_BUILD/bin/cir-opt" "$1" --cir-to-llvm -o "$2.llvm.mlir"
  "$LLVM_BUILD/bin/mlir-translate" --allow-unregistered-dialect --mlir-to-llvmir \
    "$2.llvm.mlir" -o "$2.ll"
  "$LLVM_BUILD/bin/llc" -O2 -filetype=obj "$2.ll" -o "$2.o"
  "${link_cxx[@]}" "$2.o" -o "$2" -lpthread
}

wtc() { # <in.cir> <out.cir> <remarks> [extra passes...]
  local src="$1" dst="$2" remarks="$3"
  shift 3
  "$wtc_opt" "$src" --mlir-print-op-on-diagnostic=false \
    --lift-cir-to-wtc --queue-ownership-analysis "$@" --lower-wtc-to-cir \
    --reconcile-unrealized-casts -o "$dst" 2> "$remarks" > /dev/null
}

variants=()          # "workload variant binary"
: > "$out/substitution.txt"

for entry in "${workloads[@]}"; do
  read -r name expect direct <<< "$entry"
  src="$here/workloads/$name.cpp"
  dir="$out/$name"
  mkdir -p "$dir"
  echo "== building $name" >&2

  emit_cir "$src" "$dir/$name.cir"

  wtc "$dir/$name.cir" "$dir/baseline.cir" "$dir/baseline.remarks"
  link "$dir/baseline.cir" "$dir/baseline"
  variants+=("$name baseline $dir/baseline")

  wtc "$dir/$name.cir" "$dir/optimized.cir" "$dir/optimized.remarks" --select-queue-impl
  if grep -q 'remark: substituted' "$dir/optimized.remarks"; then got=substituted; else got=kept; fi
  [ "$got" = "$expect" ] || die "$name: the optimized build was $got, but it must be $expect (see $dir/optimized.remarks)"
  link "$dir/optimized.cir" "$dir/optimized"
  variants+=("$name optimized $dir/optimized")
  echo "$name $got" >> "$out/substitution.txt"

  if [ "$direct" = yes ]; then
    emit_cir "$src" "$dir/direct.cir" -DWTC_BENCH_DIRECT
    link "$dir/direct.cir" "$dir/direct"
    variants+=("$name direct $dir/direct")
  fi
done

raw="$out/raw.txt"
: > "$raw"
for run in $(seq "$RUNS"); do
  echo "== run $run of $RUNS" >&2
  for entry in "${variants[@]}"; do
    read -r name variant binary <<< "$entry"
    line="$(${pin[@]+"${pin[@]}"} "$binary")" || die "$name/$variant failed on run $run: its results were wrong"
    echo "run=$run variant=$variant ${line#RESULT }" >> "$raw"
  done
done

# What the numbers were measured on.
env_file="$out/environment.txt"
{
  echo "date=$(date -u '+%Y-%m-%d %H:%M UTC')"
  commit="$(git -C "$repo" rev-parse --short=12 HEAD)"
  git -C "$repo" diff --quiet HEAD -- . ':!benchmarks/RESULTS.md' || commit="$commit (with uncommitted changes)"
  echo "commit=$commit"
  echo "branch=$(git -C "$repo" rev-parse --abbrev-ref HEAD)"
  echo "llvm=$(git -C "$repo" rev-parse --short=12 HEAD:llvm)"
  if [ "$(uname)" = Darwin ]; then
    echo "cpu=$(sysctl -n machdep.cpu.brand_string)"
    echo "cores=$(sysctl -n hw.ncpu)"
    echo "memory=$(( $(sysctl -n hw.memsize) / 1073741824 )) GiB"
    echo "os=macOS $(sw_vers -productVersion) ($(uname -m))"
  else
    echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "cores=$(nproc)"
    echo "memory=$(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576 )) GiB"
    echo "os=$(uname -sr) ($(uname -m))"
  fi
  echo "runs=$RUNS"
  echo "pinning=$pinning"
} > "$env_file"

python3 "$here/report.py" "$raw" "$env_file" "$out/substitution.txt" "$here/workloads" "$RESULTS"
echo "== wrote $RESULTS" >&2
