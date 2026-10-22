#!/usr/bin/env bash
#
# The gate's correctness criterion, which is a precondition for every number
# beside it: a specialised queue must be race-free where the compiler proved
# it may be substituted, and must be caught where it may not.
#
#   correctness.sh <out-dir>
#
# Five things are checked, and the last two matter most because they are the
# ones that can fail:
#
#   1. the ring's own tests, plain and under ThreadSanitizer
#   2. the workloads with the spsc and inline tags under ThreadSanitizer, no
#      reports
#   3. the compiler's own builds under ThreadSanitizer, no reports
#   4. the contract checks silent on programs where the proof holds
#   5. the contract check firing on a program where it does not
#
# Without the fifth, a check that never fires looks exactly like a correct
# program, which is the failure mode this project has already paid for once.
# The third needs the toolchain built (build-llvm, build/bin/wtc-opt) and is
# skipped, and says so, where it is not; its own negative control is
# test/Integration/tsan_own_build_caught.cpp.
#
# ThreadSanitizer needs the system compiler: the pinned ClangIR clang was
# built without compiler-rt. g++-13's runtime additionally needs address
# randomisation reduced, so runs go through setarch where it is permitted
# (a container may refuse it; there the runs go without).
set -uo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)   # the runtime tests write their logs from elsewhere
# The negative control is meant to abort; ten core dumps are not evidence.
ulimit -c 0

# Find a compiler whose ThreadSanitizer actually works, by trying it. Not by
# reading CXX: on this host CXX names a Clang built without compiler-rt, so a
# script that trusts it configures cleanly, fails to link, and reports the
# sanitizer as broken when it was only absent. Not by naming one either,
# since which compilers carry the runtime differs per machine.
RUN=()
setarch x86_64 -R true 2>/dev/null && RUN=(setarch x86_64 -R)
pick_tsan_cxx() {
  local probe out
  probe=$(mktemp -d)
  printf '#include <thread>\nint x;int main(){std::thread a([]{x=1;});a.join();}\n' > "$probe/t.cpp"
  for cand in ${WTC_TSAN_CXX:-} clang++-18 g++-13 clang++ g++; do
    command -v "$cand" >/dev/null 2>&1 || continue
    if "$cand" -fsanitize=thread -O1 "$probe/t.cpp" -o "$probe/t" >/dev/null 2>&1 \
       && "${RUN[@]}" "$probe/t" >/dev/null 2>&1; then
      rm -rf "$probe"; echo "$cand"; return 0
    fi
  done
  rm -rf "$probe"; return 1
}

CXX=$(pick_tsan_cxx) || { echo "no compiler here has a working ThreadSanitizer" >&2; exit 2; }
echo "ThreadSanitizer builds use $CXX ($("$CXX" --version | head -1))"
echo

pass=0; fail=0
ok()   { printf '  \033[32mok\033[0m   %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; fail=$((fail+1)); }

echo "=== 1. the ring's own tests ==="
for cfg in "plain:" "tsan:-DWTC_RT_SANITIZE=thread" "check:-DWTC_CHECK=ON"; do
  name=${cfg%%:*}; extra=${cfg#*:}
  B="$OUT/rt-$name"
  if cmake -S "$ROOT/runtime/test" -B "$B" -DCMAKE_CXX_COMPILER="$CXX" $extra >/dev/null 2>&1 \
     && cmake --build "$B" -j 4 >/dev/null 2>&1 \
     && ( cd "$B" && "${RUN[@]}" timeout 600 ctest --output-on-failure >"$OUT/rt-$name.log" 2>&1 ); then
    ok "runtime tests, $name"
  else
    bad "runtime tests, $name (see $OUT/rt-$name.log)"
  fi
done

echo
echo "=== 2. the workloads under ThreadSanitizer, spsc and inline tags ==="
BT="$OUT/bench-tsan"
if cmake -S "$ROOT/benchmarks" -B "$BT" -DCMAKE_CXX_COMPILER="$CXX" -DWTC_BENCH_SANITIZE=thread >/dev/null 2>&1 \
   && cmake --build "$BT" -j 8 >/dev/null 2>&1; then
  for v in shim_spsc shim_spsc_inline ring_sp ring_inline ring_borrowed; do
    for e in 8 64 256; do
      if "${RUN[@]}" timeout 600 "$BT/w1_throughput" --variant "$v" --elem "$e" --n 300000 --json /dev/null \
           >"$OUT/tsan-$v-$e.log" 2>&1 && ! grep -q 'ThreadSanitizer' "$OUT/tsan-$v-$e.log"; then
        ok "W1 $v E$e: no report"
      else
        bad "W1 $v E$e: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/tsan-$v-$e.log" || echo 'run failed')"
      fi
    done
  done
  # The ablation's six rows, the runtime's queues used directly: the
  # segmented queue holding the element itself is what the shim builds,
  # and a shared_ptr crossing threads in a cell is what it no longer does.
  for v in mpmc_xsp mpmc_share mpmc_cell spsc_xsp spsc_share spsc_cell; do
    for e in 8 256; do
      if "${RUN[@]}" timeout 600 "$BT/w10_ablation" --variant "$v" --elem "$e" --n 300000 --json /dev/null \
           >"$OUT/tsan-w10-$v-$e.log" 2>&1 && ! grep -q 'ThreadSanitizer' "$OUT/tsan-w10-$v-$e.log"; then
        ok "W10 $v E$e: no report"
      else
        bad "W10 $v E$e: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/tsan-w10-$v-$e.log" || echo 'run failed')"
      fi
    done
  done
  if "${RUN[@]}" timeout 900 "$BT/w3_pipeline" --variant shim_spsc --n 300000 \
       >"$OUT/tsan-w3.log" 2>&1 && ! grep -q 'ThreadSanitizer' "$OUT/tsan-w3.log"; then
    ok "W3 shim_spsc: no report"
  else
    bad "W3 shim_spsc: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/tsan-w3.log" || echo 'run failed')"
  fi
else
  bad "could not build the sanitized benchmarks"
fi

echo
echo "=== 3. the compiler's own builds under ThreadSanitizer ==="
# What the compiler generated, instrumented after the whole pipeline
# (tools/wtc-cc --tsan): the pipeline with the queues it made inline, and
# the producer loop it batched, over each implementation.
W3=$ROOT/benchmarks/workloads/w3_pipeline_wtc.cpp
W7=$ROOT/benchmarks/workloads/w7_batch_wtc.cpp
HAVE_WTC=0
[ -x "$ROOT/build/bin/wtc-opt" ] && [ -x "$ROOT/build-llvm/bin/clang++" ] && HAVE_WTC=1
wtc_cc() {
  WTC_TSAN_CXX="$CXX" WTC_OPT="$ROOT/build/bin/wtc-opt" "$ROOT/tools/wtc-cc" \
    --toolchain "$ROOT/build-llvm" -I"$ROOT/benchmarks/queues" -I"$ROOT/benchmarks/harness" "$@"
}
# name, the tags it must report, wtc-cc arguments. A name with "batch" in it
# must also have been batched.
own() {
  local name=$1 want=$2; shift 2
  if ! wtc_cc --tsan -O1 -g "$@" -o "$OUT/own-$name" >"$OUT/own-$name.build.log" 2>&1; then
    bad "$name: could not build (see $OUT/own-$name.build.log)"; return
  fi
  if [[ $name == *batch* ]] && ! grep -q 'pushes in this loop batched' "$OUT/own-$name.build.log"; then
    bad "$name: not batched"; return
  fi
  if "${RUN[@]}" timeout 900 "$OUT/own-$name" --n 300000 --json /dev/null >"$OUT/own-$name.log" 2>&1 \
     && ! grep -q 'ThreadSanitizer' "$OUT/own-$name.log" && grep -qE "tags? $want\$" "$OUT/own-$name.log"; then
    ok "$name, $want: no report"
  else
    bad "$name: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/own-$name.log" || echo 'run failed or wrong tag')"
  fi
}
if [ $HAVE_WTC = 1 ]; then
  own w3-compiler spsc_inline,spsc_inline "$W3"
  own w7-generic-batch generic --no-specialize -DW7_BATCH "$W7"
  own w7-spsc-batch spsc --no-specialize -DW7_TAG=1 -DW7_BATCH "$W7"
  own w7-inline-batch spsc_inline -DW7_BATCH "$W7"
else
  bad "the compiler's builds: skipped, the toolchain is not built here (build/bin/wtc-opt, build-llvm/bin/clang++)"
fi

echo
echo "=== 4. the contract checks stay silent where the proof holds ==="
BC="$OUT/bench-check"
if cmake -S "$ROOT/benchmarks" -B "$BC" -DCMAKE_CXX_COMPILER="$CXX" -DWTC_CHECK=ON >/dev/null 2>&1 \
   && cmake --build "$BC" -j 8 >/dev/null 2>&1; then
  silent=1
  for k in $(seq 1 10); do
    "$BC/w1_throughput" --variant shim_spsc --elem 64 --n 2000000 --json /dev/null >/dev/null 2>"$OUT/check-$k.log" || silent=0
    grep -q 'contract violated' "$OUT/check-$k.log" && silent=0
  done
  [ $silent = 1 ] && ok "W1 shim_spsc, 10 runs, check silent" || bad "W1 shim_spsc: the check fired on a correct program"
else
  bad "could not build the checked benchmarks"
fi
# The guard that catches a kept element, on the compiler's inline builds.
if [ $HAVE_WTC = 1 ]; then
  for spec in "w3-compiler:$W3" "w7-inline-batch:$W7:-DW7_BATCH"; do
    IFS=: read -r name src extra <<<"$spec"
    # shellcheck disable=SC2086
    if wtc_cc -O2 -DWTC_CHECK $extra "$src" -o "$OUT/check-$name" >"$OUT/check-$name.build.log" 2>&1; then
      silent=1
      for k in $(seq 1 10); do
        "$OUT/check-$name" --n 1000000 --json /dev/null >"$OUT/check-$name-$k.log" 2>&1 || silent=0
        grep -q 'spsc_inline' "$OUT/check-$name-$k.log" || silent=0
      done
      [ $silent = 1 ] && ok "$name, inline, 10 runs, check silent" || bad "$name: the check fired or the tag is not inline"
    else
      bad "$name: could not build with the checks (see $OUT/check-$name.build.log)"
    fi
  done
fi

echo
echo "=== 5. the contract check fires where it does not ==="
if [ -x "$BC/w5_mpmc_control" ]; then
  fired=0
  for k in $(seq 1 10); do
    "$BC/w5_mpmc_control" --variant shim_spsc --n 200000 --force-unsafe >/dev/null 2>"$OUT/neg-$k.log"
    grep -q 'SPSC contract violated' "$OUT/neg-$k.log" && fired=$((fired+1))
  done
  [ $fired = 10 ] && ok "2P2C forced spsc, caught 10 of 10" || bad "2P2C forced spsc, caught only $fired of 10"
fi

echo
echo "  passed $pass, failed $fail"
[ $fail = 0 ]
