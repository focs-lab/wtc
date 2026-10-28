#!/usr/bin/env bash
#
# The thread-pool libraries' leg: ThreadPool and BS::thread_pool, each as
# released, in a control, and ported to the shim (benchmarks/apps/threadpool,
# benchmarks/apps/bs-thread-pool), in paired, interleaved rounds. Untimed:
# it reserves nothing and fixes no frequency, so it reports ratios taken
# within a round, not speeds to compare across hosts.
#
#   pool_apps_leg.sh <out-dir> <lib>:<bin-dir> [<lib>:<bin-dir> ...]
#
# <lib> is the programs' prefix (threadpool, bs-thread-pool) and <bin-dir>
# where that library's build.sh put them. Each library has two programs:
# <lib>-<build>, whose one submitter is the main thread, and
# <lib>-multi-<build>, with SUBMITTERS submitting threads (4). The builds
# are orig, orig-clang, orig-spin, generic, compiler and, for the first
# program only, hand (build.sh says what each is).
#
# REPS rounds (10) of every configuration: WORKS ns of work a task (0 100
# 1000 10000) times WORKERS (1 2 4 8), RUN_SECONDS a run (0.4). The work is
# calibrated once, at the start, by the first library's orig build, and
# every run gets the same number of rounds for the same WORKS. Each run is
# a fresh process. Within a round a configuration runs all its builds back
# to back, in an order rotated by one from round to round, so that the
# ratios taken within a round pair runs made a few seconds apart and no
# build always runs first. A run that fails its own checks stops the leg.
# The processors are those the leg is given (taskset or a container's
# cpuset); each program pins its threads to distinct cores among them.
#
# Writes runs.jsonl (one line a run), log.txt (each run's line and the
# load average once per configuration) and meta.txt.
set -euo pipefail
OUT=${1:?out dir}; shift
[ $# -ge 1 ] || { echo "usage: pool_apps_leg.sh <out-dir> <lib>:<bin-dir>..." >&2; exit 2; }
mkdir -p "$OUT"
REPS=${REPS:-10}
RUN_SECONDS=${RUN_SECONDS:-0.4}
WORKS=${WORKS:-"0 100 1000 10000"}
WORKERS=${WORKERS:-"1 2 4 8"}
SUBMITTERS=${SUBMITTERS:-4}
JSON=$OUT/runs.jsonl
LOG=$OUT/log.txt
: > "$JSON"

first=${1%%:*}; firstbin=${1#*:}
RPN=$("$firstbin/$first-orig" --calibrate)
{
  echo "start $(date -u +%FT%TZ)"
  echo "host $(uname -n) kernel $(uname -r)"
  echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  echo "allowed $(taskset -pc $$ | awk -F': ' '{print $2}')"
  echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "rounds_per_ns $RPN"
  echo "reps $REPS run_seconds $RUN_SECONDS works $WORKS workers $WORKERS submitters $SUBMITTERS"
  for spec in "$@"; do
    [ -f "${spec#*:}/build-info.txt" ] && sed "s/^/${spec%%:*} /" "${spec#*:}/build-info.txt"
  done
} > "$OUT/meta.txt"
cat "$OUT/meta.txt" >> "$LOG"

for rep in $(seq 0 $((REPS - 1))); do
  for spec in "$@"; do
    lib=${spec%%:*}; bin=${spec#*:}
    for prog in single multi; do
      if [ "$prog" = single ]; then
        builds=(orig orig-clang orig-spin generic compiler hand); pre=$lib; extra=()
      else
        builds=(orig orig-clang orig-spin generic compiler); pre=$lib-multi; extra=(--submitters "$SUBMITTERS")
      fi
      n=${#builds[@]}
      for work in $WORKS; do
        rounds=$(awk -v w="$work" -v r="$RPN" 'BEGIN { printf "%d", w * r + 0.5 }')
        for w in $WORKERS; do
          echo "rep $rep $pre workers $w work $work load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
          for k in $(seq 0 $((n - 1))); do
            b=${builds[$(((k + rep) % n))]}
            "$bin/$pre-$b" --workers "$w" "${extra[@]}" --work "$work" --rounds "$rounds" \
              --seconds "$RUN_SECONDS" --rep "$rep" --variant "$b" --json "$JSON" 2>> "$LOG" \
              || { echo "FAILED: $pre-$b workers $w work $work rep $rep" | tee -a "$LOG" >&2; exit 1; }
          done
        done
      done
    done
  done
done
echo "end $(date -u +%FT%TZ)" | tee -a "$OUT/meta.txt" >> "$LOG"
