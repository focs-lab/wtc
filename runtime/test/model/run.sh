#!/usr/bin/env bash
# Model-checks the index rings with GenMC under RC11, every configuration of
# scq_model.cpp, and then the deliberate bugs each must catch (the
# configurations that can fail). Run where GenMC is installed:
#
#   GENMC=/path/to/genmc runtime/test/model/run.sh [config...]
#
# GENMC_THREADS (default 1) explores with that many threads; GENMC_TIMEOUT
# (seconds, default 3600) gives up on a configuration after that long and
# says so, which the wait-free ring's larger configurations may need.
#
# GenMC 0.20 or later (it needs LLVM 19 or later). scq_model checks the
# lock-free ring and the queue over it, wcq_model the wait-free ring's slow
# path and helping on 32-bit halves (it says why). Every loop is unrolled
# to a bound; an execution that would go past it is counted as blocked and
# not explored. The bound must exceed the rings' entries, which the
# constructors' loops walk: 2n + 1 for a ring of n indices.
set -u
here=$(cd "$(dirname "$0")" && pwd)
genmc=${GENMC:-genmc}
rc=0

# file:config:unroll-bound
configs="scq_model:ring_pair:5 scq_model:ring_fifo:5 scq_model:ring_empty:5 scq_model:ring_wrap:5
         scq_model:queue_plain:5 scq_model:queue_exact:5 scq_model:queue_mpsc:9
         wcq_model:wcq_empty:6 wcq_model:wcq_enq2:6 wcq_model:wcq_enqdeq:6 wcq_model:wcq_fifo:6
         wcq_model:wcq_pair:6 wcq_model:wcq_wrap:6"
# mutant:file:config: the configuration that must report the bug
mutants="1:scq_model:ring_pair 1:scq_model:ring_fifo 1:scq_model:queue_plain 2:scq_model:ring_pair
         3:wcq_model:wcq_enqdeq"

run() {  # file config unroll extra...
  local file=$1 config=$2 unroll=$3; shift 3
  local start=$(date +%s)
  local out
  out=$(timeout "${GENMC_TIMEOUT:-3600}" "$genmc" --rc11 --disable-estimation --nthreads="${GENMC_THREADS:-1}" \
        --unroll="$unroll" -- -std=c++17 \
        -DCONFIG="$config" -I"$here/../.." "$@" "$here/$file" 2>&1)
  local status=$?
  local runs=$(echo "$out" | sed -n 's/^Number of complete executions explored: //p')
  local blocked=$(echo "$out" | sed -n 's/^Number of blocked executions seen: //p')
  echo "$status|${runs:-0}|${blocked:-0}|$(( $(date +%s) - start ))|$(echo "$out" | grep -m1 -E '^Error|INTERNAL')"
}

want=${*:-}
for c in $configs; do
  file=${c%%:*}; rest=${c#*:}; name=${rest%%:*}; unroll=${rest##*:}
  [ -n "$want" ] && ! echo " $want " | grep -q " $name " && continue
  IFS='|' read -r status runs blocked secs err <<< "$(run "$file.cpp" "$name" "$unroll")"
  if [ "$status" = 0 ]; then
    printf '%-12s ok      %8s executions, %6s blocked, unroll %s, %4s s\n' "$name" "$runs" "$blocked" "$unroll" "$secs"
  elif [ "$status" = 124 ]; then
    printf '%-12s timeout after %s s: no error in what was explored, not complete\n' "$name" "$secs"
  else
    printf '%-12s FAILED  %s\n' "$name" "$err"; rc=1
  fi
done
[ -n "$want" ] && exit $rc
for m in $mutants; do
  mut=${m%%:*}; rest=${m#*:}; file=${rest%%:*}; name=${rest##*:}
  IFS='|' read -r status runs blocked secs err <<< "$(run "$file.cpp" "$name" 6 -DWTC_TEST_RING_MUTANT="$mut")"
  if [ "$status" = 42 ]; then
    printf 'mutant %s %-12s caught  (%s)\n' "$mut" "$name" "$err"
  else
    printf 'mutant %s %-12s MISSED  (status %s)\n' "$mut" "$name" "$status"; rc=1
  fi
done
exit $rc
