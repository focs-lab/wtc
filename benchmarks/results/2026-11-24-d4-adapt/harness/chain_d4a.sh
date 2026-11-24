#!/usr/bin/env bash
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4A (the worktree at the measured
# commit), $LEG_DIR (the host's leg directory), $DRIVER (runbook 07's leg driver, leg.sh,
# a verbatim copy).
# The timed leg of the adaptive delegation on focs, announced through the leg driver of
# runbook 07 (leg.sh, the verbatim copy) and run inside a bench reservation of 24 cores
# (processors 4-27, SMT siblings 60-83 offline, governor performance, turbo off).
set -u
L=$LEGS
IN=$WTC_D4A/benchmarks/scripts/in_bench.py
export LEG_DIR=$LEG_DIR LEG_HOST=focs LEG_LANE=wtc
D=$(date -u +%F)
P=~/bin:/usr/local/bin:/usr/bin:/bin
name=d4-adapt; min=55
what="D4 adaptive timed leg (LevelDB readrandom: original, ClangIR, always-delegate, adaptive delegation, LiTL MCS; 1-23 threads, 10 reps) in a bench reservation of 24 cores 4-27, siblings 60-83 offline; nothing compiles on focs until END"
O=$L/timed/$D-$name
[ -e "$O" ] && mv "$O" "$O.bak-$(date -u +%H%M%S)"
mkdir -p "$L/timed"
# No compile anywhere on the host before START: wait while one runs. Defunct processes
# and the two make processes left asleep since October (more than a day old, no CPU
# time) are not compiles.
compiles() { ps -eo pid=,stat=,etimes=,comm= | awk '$2 !~ /^Z/ && $3 < 86400 && $4 ~ /^(cc1plus|cc1|clang|clang\+\+|clang-[0-9]+|ninja|make|ld|lld)$/'; }
while [ -n "$(compiles)" ]; do echo "[$(date -u +%FT%TZ)] chain: a compile is running, waiting"; compiles; sleep 60; done
echo "[$(date -u +%FT%TZ)] chain: $name -> $O"
"$DRIVER" "$name" "$min" "$what" -- python3 "$IN" -c 24 -m 32G -e -- env PATH=$P python3 -I "$L/harness/leg_d4a.py" "$O" 10 timed   > "$L/timed/$name.bench.out" 2>&1
rc=$?
echo "[$(date -u +%FT%TZ)] chain: $name rc=$rc; $(grep " END $name " $LEG_DIR/FOCS_LEG.log | tail -1)"
exit $rc
