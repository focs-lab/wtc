#!/usr/bin/env bash
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC (this tree at the measured
# commit), $WTC_D4 / $WTC_D5 (worktrees of the earlier legs), $LEG_DIR (the host's leg
# directory).
# D5 re-leg: the CTPL variants at the series' new head (wait-lifting in $T), built on
# focs before the leg. orig: clang++-18 -O2. hand-<policy>: probe Q5's ctpl_wait.py
# through wtc-cc --lift with the policy fixed (spin_then_park: pause 4 us, then park;
# park: park at once). wtc: the released source through wtc-cc --lift.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
T=$WTC
SP=$PROBES
SRC=$SP/q5/src/ctpl
CW=$H/ctpl-wait
CTB=$T/benchmarks/apps/ctpl/bench.cpp
CTM=$SP/q5/scripts/ctpl_bench_multi.cpp
TC=$WTC/build-llvm
export WTC_OPT=$T/build/bin/wtc-opt
mkdir -p $H/bin $H/log
git -C $T log -1 --format='built at %H %s' > $H/log/rev.txt
python3 -I $SP/q5/scripts/ctpl_wait.py $SRC $CW >/dev/null
cc() { "$T/tools/wtc-cc" --toolchain $TC -O2 "$@"; }
for b in bench multi; do
  case $b in bench) src=$CTB;; multi) src=$CTM;; esac
  clang++-18 -O2 -std=c++17 -I$SRC $src -lpthread -o $H/bin/$b-orig
  for p in spin_then_park park; do
    cc --lift -I$CW -I$T/runtime -DCTPL_Q5_POLICY=$p -DWTC_SPIN_NS=4000 $src -lpthread -o $H/bin/$b-hand-$p > $H/log/$b-hand-$p.log 2>&1
  done
  cc --lift -I$SRC $src -lpthread -o $H/bin/$b-wtc > $H/log/$b-wtc.log 2>&1
  grep -h "locked wait cv" $H/log/$b-wtc.log | sed 's/.*remark: //' | sort -u
done
cat $H/log/rev.txt
ls -la $H/bin
