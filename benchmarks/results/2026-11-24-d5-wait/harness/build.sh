#!/usr/bin/env bash
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4 / $WTC_D5 (worktrees at the
# measured commits), $WTC (this tree), $LEG_DIR (the host's leg directory).
# D5 timed legs: every CTPL variant, built on focs before any leg (never inside one).
#   build.sh        (output in ./bin, logs in ./log)
# orig: CTPL as released, clang++-18 -O2.  hand-<policy>: probe Q5's ctpl_wait.py
# (the worker's cv wait replaced by the eventcount by hand) through wtc-cc --lift, as
# q5_build.sh builds it.  wtc: the released source through wtc-cc --lift at the
# wait-lifting commit (the compiler's lifted wait).  wtcy<ns>: the same, with the
# oversubscribed yield budget kLiftedOversubscribedYieldNs (shim/lifted.hpp) set to
# <ns>; built from a copy of tools/ shim/ runtime/ whose one constant is replaced, so
# the worktree is untouched.  wtcy16000000 is built from an unmodified copy and must
# equal wtc byte for byte.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
T=$WTC_D5
SP=$PROBES
SRC=$SP/q5/src/ctpl
CW=$H/ctpl-wait
CTB=$T/benchmarks/apps/ctpl/bench.cpp
CTM=$SP/q5/scripts/ctpl_bench_multi.cpp
TC=$WTC/build-llvm
export WTC_OPT=$T/build/bin/wtc-opt
mkdir -p $H/bin $H/log
python3 -I $SP/q5/scripts/ctpl_wait.py $SRC $CW >/dev/null
cc() { "$1/tools/wtc-cc" --toolchain $TC -O2 "${@:2}"; }
for b in bench multi; do
  case $b in bench) src=$CTB;; multi) src=$CTM;; esac
  clang++-18 -O2 -std=c++17 -I$SRC $src -lpthread -o $H/bin/$b-orig
  for p in spin_then_park spin; do
    cc $T --lift -I$CW -I$T/runtime -DCTPL_Q5_POLICY=$p -DWTC_SPIN_NS=4000 $src -lpthread -o $H/bin/$b-hand-$p > $H/log/$b-hand-$p.log 2>&1
  done
  cc $T --lift -I$SRC $src -lpthread -o $H/bin/$b-wtc > $H/log/$b-wtc.log 2>&1
done
for ns in 64000 1000000 4000000 16000000 64000000; do
  R=$H/yroot-$ns; rm -rf $R; mkdir -p $R
  cp -r $T/tools $T/shim $T/runtime $R/
  if [ $ns != 16000000 ]; then
    sed -i "s/^inline constexpr std::uint64_t kLiftedOversubscribedYieldNs = 16000000;$/inline constexpr std::uint64_t kLiftedOversubscribedYieldNs = $ns;/" $R/shim/lifted.hpp
  fi
  grep -q "kLiftedOversubscribedYieldNs = $ns;" $R/shim/lifted.hpp
  for b in bench multi; do
    case $b in bench) src=$CTB;; multi) src=$CTM;; esac
    cc $R --lift -I$SRC $src -lpthread -o $H/bin/$b-wtcy$ns > $H/log/$b-wtcy$ns.log 2>&1
  done
  rm -rf $R
done
for b in bench multi; do
  cmp $H/bin/$b-wtc $H/bin/$b-wtcy16000000 && echo "$b: wtcy16000000 == wtc"
  grep -h "locked wait cv" $H/log/$b-wtc.log | sed 's/.*remark: //' | sort -u
done
ls -la $H/bin
