#!/usr/bin/env bash
#
# The gate's correctness criterion, which is a precondition for every number
# beside it: a specialised queue must be race-free where the compiler proved
# it may be substituted, and must be caught where it may not.
#
#   correctness.sh <out-dir>
#
# Four things are checked, and the last two matter most because they are the
# ones that can fail:
#
#   1. the ring's own tests, plain and under ThreadSanitizer
#   2. the workloads with the spsc tag under ThreadSanitizer, no reports
#   3. the contract check silent on programs where the proof holds
#   4. the contract check firing on a program where it does not
#
# Without the fourth, a check that never fires looks exactly like a correct
# program, which is the failure mode this project has already paid for once.
#
# ThreadSanitizer needs the system compiler: the pinned ClangIR clang was
# built without compiler-rt. g++-13's runtime additionally needs address
# randomisation reduced, so runs go through setarch.
set -uo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"
# The negative control is meant to abort; ten core dumps are not evidence.
ulimit -c 0

# Find a compiler whose ThreadSanitizer actually works, by trying it. Not by
# reading CXX: on this host CXX names a Clang built without compiler-rt, so a
# script that trusts it configures cleanly, fails to link, and reports the
# sanitizer as broken when it was only absent. Not by naming one either,
# since which compilers carry the runtime differs per machine.
RUN=(setarch x86_64 -R)
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
echo "=== 2. the workloads under ThreadSanitizer, spsc tag ==="
BT="$OUT/bench-tsan"
if cmake -S "$ROOT/benchmarks" -B "$BT" -DCMAKE_CXX_COMPILER="$CXX" -DWTC_BENCH_SANITIZE=thread >/dev/null 2>&1 \
   && cmake --build "$BT" -j 8 >/dev/null 2>&1; then
  for v in shim_spsc ring_sp ring_inline; do
    for e in 8 64 256; do
      if "${RUN[@]}" timeout 600 "$BT/w1_throughput" --variant "$v" --elem "$e" --n 300000 --json /dev/null \
           >"$OUT/tsan-$v-$e.log" 2>&1 && ! grep -q 'ThreadSanitizer' "$OUT/tsan-$v-$e.log"; then
        ok "W1 $v E$e: no report"
      else
        bad "W1 $v E$e: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/tsan-$v-$e.log" || echo 'run failed')"
      fi
    done
  done
  if "${RUN[@]}" timeout 900 "$BT/w3_pipeline" --variant shim_spsc --n 300000 --cpus 2,3,6 \
       >"$OUT/tsan-w3.log" 2>&1 && ! grep -q 'ThreadSanitizer' "$OUT/tsan-w3.log"; then
    ok "W3 shim_spsc: no report"
  else
    bad "W3 shim_spsc: $(grep -m1 -oE 'ThreadSanitizer: [a-z ]*' "$OUT/tsan-w3.log" || echo 'run failed')"
  fi
else
  bad "could not build the sanitized benchmarks"
fi

echo
echo "=== 3. the contract check stays silent where the proof holds ==="
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

echo
echo "=== 4. the contract check fires where it does not ==="
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
