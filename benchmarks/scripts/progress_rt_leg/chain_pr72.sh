#!/usr/bin/env bash
# Copy of the script the leg ran, with the host's leg directory written as
# $LEG_DIR and the cell list read from cells.txt beside the harness.
# The timed leg of the progress runtime's queues on focs, announced through the
# leg driver of runbook 07 (leg.sh, the verbatim copy) and run inside a bench
# reservation of 24 cores (processors 4-27, SMT siblings 60-83 offline,
# governor performance, turbo off).
#   chain_pr72.sh <reps> <expected-minutes>
# Cells: harness/cells.txt (PR72_KEYS, the untimed A/B's list).
# Paths: $PERF72 (this leg's working directory: harness/, focs-build/),
# $WTC_PR (the worktree at the measured commit), $DRIVER (leg.sh).
set -u
REPS=${1:?reps}; MIN=${2:?minutes}
L=$PERF72
IN=$WTC_PR/benchmarks/scripts/in_bench.py
export LEG_DIR=$LEG_DIR LEG_HOST=focs LEG_LANE=wtc
D=$(date -u +%F)
P=$HOME/bin:/usr/local/bin:/usr/bin:/bin
name=progress-rt
what="progress runtime queue leg (W1, W9, W10, W11 and the bounded queue: main 83270b5 against #72 e7ba584; the 22 cells of the untimed A/B; $REPS reps) in a bench reservation of 24 cores 4-27, siblings 60-83 offline; nothing compiles on focs until END"
O=$L/timed/$D-$name
[ -e "$O" ] && mv "$O" "$O.bak-$(date -u +%H%M%S)"
mkdir -p "$L/timed"
# No compile anywhere on the host before START: wait while one runs. Defunct
# processes and make processes more than a day old are not compiles.
compiles() { ps -eo pid=,stat=,etimes=,comm= | awk '$2 !~ /^Z/ && $3 < 86400 && $4 ~ /^(cc1plus|cc1|clang|clang\+\+|clang-[0-9]+|ninja|make|ld|lld)$/'; }
while [ -n "$(compiles)" ]; do echo "[$(date -u +%FT%TZ)] chain: a compile is running, waiting"; compiles; sleep 60; done
echo "[$(date -u +%FT%TZ)] chain: $name -> $O"
"$DRIVER" "$name" "$MIN" "$what" -- python3 "$IN" -c 24 -m 32G -e -- env PATH=$P \
  PR72_BIN=$L/focs-build-e7ba584 "PR72_KEYS=$(cat $L/harness/cells.txt)" PR72_SET=4-27 PR72_CHECK_ENV=$WTC_PR/benchmarks/scripts/check_env.sh \
  python3 -I "$L/harness/leg_pr72.py" "$O" "$REPS" timed > "$L/timed/$name.bench.out" 2>&1
rc=$?
echo "[$(date -u +%FT%TZ)] chain: $name rc=$rc; $(grep " END $name " $LEG_DIR/FOCS_LEG.log | tail -1)"
exit $rc
