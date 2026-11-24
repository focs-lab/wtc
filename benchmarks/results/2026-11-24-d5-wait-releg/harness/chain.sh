#!/usr/bin/env bash
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC (this tree at the measured
# commit), $WTC_D4 / $WTC_D5 (worktrees of the earlier legs), $LEG_DIR (the host's leg
# directory).
# The two timed legs on focs, one after the other, each announced through the leg
# driver of runbook 07 (leg.sh, extracted verbatim) and run inside its own bench
# reservation of 24 cores (processors 4-27, SMT siblings 60-83 offline, governor
# performance, turbo off).  chain.sh [d4] [d5]
set -u
L=$LEGS
IN=$WTC_D4/benchmarks/scripts/in_bench.py
export LEG_DIR=$LEG_DIR LEG_HOST=focs LEG_LANE=wtc
D=$(date -u +%F)
P=~/bin:/usr/local/bin:/usr/bin:/bin
[ $# -gt 0 ] || set -- d4 d5
for leg in "$@"; do
  case $leg in
    d4) name=d4-delegate; min=75; what="D4 timed leg (LevelDB readrandom: original, ClangIR, compiler-delegated, hand-delegated, LiTL; 1-23 threads, 10 reps) in a bench reservation of 24 cores 4-27, siblings 60-83 offline; nothing compiles on focs until END"; h=$L/d4/leg_d4.py;;
    d5) name=d5-wait; min=30; what="D5 timed leg (CTPL: original, hand-lifted, compiler-lifted wait, yield-budget sweep; 10 reps) in a bench reservation of 24 cores 4-27, siblings 60-83 offline; nothing compiles on focs until END"; h=$L/d5/leg_d5.py;;
    d5b) name=d5-wait-releg; min=25; what="D5 timed re-leg at the series' new head (CTPL: original, hand spin_then_park and park, compiler-lifted wait whose oversubscribed branch now parks after the pause; 10 reps) in a bench reservation of 24 cores 4-27, siblings 60-83 offline; nothing compiles on focs until END"; h=$L/d5b/leg_d5b.py;;
  esac
  O=$L/timed/$D-$name
  [ -e "$O" ] && mv "$O" "$O.bak-$(date -u +%H%M%S)"
  mkdir -p "$L/timed"
  echo "[$(date -u +%FT%TZ)] chain: $name -> $O"
  "$L/leg.sh" "$name" "$min" "$what" -- python3 "$IN" -c 24 -m 32G -e -- env PATH=$P python3 -I "$h" "$O" 10 timed \
    > "$L/timed/$name.bench.out" 2>&1
  rc=$?
  echo "[$(date -u +%FT%TZ)] chain: $name rc=$rc; $(grep " END $name " $LEG_DIR/FOCS_LEG.log | tail -1)"
  [ $rc = 0 ] || exit $rc
  sleep 60
done
