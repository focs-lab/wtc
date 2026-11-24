#!/usr/bin/env bash
# Model-checks the index rings with GenMC under RC11, every configuration of
# scq_model.cpp, and then the deliberate bugs each must catch (the
# configurations that can fail). Run where GenMC is installed:
#
#   GENMC=/path/to/genmc runtime/test/model/run.sh [config...]
#
# GenMC 0.20 or later (it needs LLVM 19 or later). Every loop is unrolled
# to a bound; an execution that would go past it is counted as blocked and
# not explored. The bound must exceed the rings' entries, which the
# constructors' loops walk: 2n + 1 for a ring of n indices.
set -u
here=$(cd "$(dirname "$0")" && pwd)
genmc=${GENMC:-genmc}
rc=0

# config unroll-bound
configs="ring_pair:5 ring_fifo:5 ring_empty:5 ring_wrap:5 queue_plain:5 queue_exact:5 queue_mpsc:9"
# mutant config: the configuration that must report the bug
mutants="1:ring_pair 1:ring_fifo 1:queue_plain 2:ring_pair"

run() {  # file config unroll extra...
  local file=$1 config=$2 unroll=$3; shift 3
  local start=$(date +%s)
  local out
  out=$("$genmc" --rc11 --disable-estimation --unroll="$unroll" -- -std=c++17 \
        -DCONFIG="$config" -I"$here/../.." "$@" "$here/$file" 2>&1)
  local status=$?
  local runs=$(echo "$out" | sed -n 's/^Number of complete executions explored: //p')
  local blocked=$(echo "$out" | sed -n 's/^Number of blocked executions seen: //p')
  echo "$status|${runs:-0}|${blocked:-0}|$(( $(date +%s) - start ))|$(echo "$out" | grep -m1 -E '^Error|INTERNAL')"
}

want=${*:-}
for c in $configs; do
  name=${c%%:*}; unroll=${c##*:}
  [ -n "$want" ] && ! echo " $want " | grep -q " $name " && continue
  IFS='|' read -r status runs blocked secs err <<< "$(run scq_model.cpp "$name" "$unroll")"
  if [ "$status" = 0 ]; then
    printf '%-12s ok      %8s executions, %6s blocked, unroll %s, %4s s\n' "$name" "$runs" "$blocked" "$unroll" "$secs"
  else
    printf '%-12s FAILED  %s\n' "$name" "$err"; rc=1
  fi
done
[ -n "$want" ] && exit $rc
for m in $mutants; do
  mut=${m%%:*}; name=${m##*:}
  IFS='|' read -r status runs blocked secs err <<< "$(run scq_model.cpp "$name" 5 -DWTC_TEST_RING_MUTANT="$mut")"
  if [ "$status" = 42 ]; then
    printf 'mutant %s %-12s caught  (%s)\n' "$mut" "$name" "$err"
  else
    printf 'mutant %s %-12s MISSED  (status %s)\n' "$mut" "$name" "$status"; rc=1
  fi
done
exit $rc
