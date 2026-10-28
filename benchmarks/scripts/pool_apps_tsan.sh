#!/usr/bin/env bash
#
# The thread-pool libraries' ports under ThreadSanitizer: does the
# compiler's build of each pool run without a report, and does every run
# pass its own checks (every future's value, every task run exactly once,
# the tasks queued at destruction run before the workers leave)? The
# compiler's builds that benchmarks/apps/{threadpool,bs-thread-pool}/
# build.sh makes with TSAN=1 run REPS times per configuration, each run to
# its end (halt_on_error=0), so that every report it makes is named.
#
#   pool_apps_tsan.sh <threadpool-out> <bs-thread-pool-out>
#
# The two are the out-dirs of the two build.sh scripts, run with TSAN=1.
# Writes one line per run to stdout: library, program and workers, run,
# exit status (66 is ThreadSanitizer's, 1 a failed check), reports,
# whether the run's checks passed, and what each report is about (the
# global it names, else the function at the top of each stack). The
# reports themselves go to ERR_DIR (default: a temporary directory, kept).
# SYMBOLIZER is llvm-symbolizer, by default the one in the LLVM build tree
# next to this repository, else from PATH. REPS defaults to 30, WORKERS to
# 1 2 4 8, SUBMITTERS (the second program's submitting threads) to 4.
#
# What each build runs (the analysis's verdicts, in each build's
# bin/<name>.log):
#
#   threadpool      compiler-tsan        one submitter: the queue chosen
#                                        when the pool is built, spsc with
#                                        one worker, else the lock-free
#                                        queue for one producer (spmc)
#                   multi-compiler-tsan  several submitters: the
#                                        single-consumer queue (mpsc) with
#                                        one worker, else the general queue
#   bs-thread-pool  compiler-tsan        one submitter: spmc, for every
#                                        number of workers
#                   multi-compiler-tsan  several submitters: the general
#                                        queue
set -uo pipefail
TP=$(cd "${1:?threadpool out}" && pwd); BS=$(cd "${2:?bs-thread-pool out}" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
REPS=${REPS:-30}
WORKERS=${WORKERS:-"1 2 4 8"}
SUBMITTERS=${SUBMITTERS:-4}
SYMBOLIZER=${SYMBOLIZER:-$( [ -x "$ROOT/build-llvm/bin/llvm-symbolizer" ] && echo "$ROOT/build-llvm/bin/llvm-symbolizer" || command -v llvm-symbolizer || true)}
export TSAN_OPTIONS="halt_on_error=0${SYMBOLIZER:+:external_symbolizer_path=$SYMBOLIZER}"
E=${ERR_DIR:-$(mktemp -d)}; mkdir -p "$E"
echo "# reports in $E" >&2

# What a report is about: the global it names, else the functions at the
# top of its two stacks; one entry per distinct report, with its count.
what() {
  awk '/WARNING: ThreadSanitizer/ { if (w != "") print w; w = $0; sub(/.*ThreadSanitizer: /, "", w); sub(/ \(pid=[0-9]+\)/, "", w); f = 0; next }
       /^  (Read|Write|Atomic|Previous)/ { f = 1; next }
       f == 1 && /#0 / { split($0, a, " "); w = w " " a[2]; f = 2; next }
       /Location is global/ { match($0, /global .[^ ]+/); w = w " " substr($0, RSTART + 7, RLENGTH - 7) }
       END { if (w != "") print w }' "$1" | sort | uniq -c | awk '{ $1 = $1 "x"; print }' | paste -sd';' -
}

for run in $(seq 1 "$REPS"); do
  for lib in threadpool bs-thread-pool; do
    [ "$lib" = threadpool ] && out=$TP || out=$BS
    for prog in single multi; do
      if [ "$prog" = single ]; then exe=$out/bin/$lib-compiler-tsan; extra=()
      else exe=$out/bin/$lib-multi-compiler-tsan; extra=(--submitters "$SUBMITTERS"); fi
      for w in $WORKERS; do
        err="$E/$lib-$prog-w$w-r$run.err"
        "$exe" --workers "$w" "${extra[@]}" --rounds 50 --seconds 0.3 --inflight 256 \
          --variant tsan --json /dev/null >/dev/null 2>"$err"; rc=$?
        grep -q ', checked)$' "$err" && checks=passed || checks=FAILED
        printf '%s\t%s\trun=%s\trc=%s\treports=%s\tchecks=%s\t%s\n' "$lib" \
          "$prog$([ "$prog" = multi ] && echo " submitters=$SUBMITTERS") workers=$w" "$run" "$rc" \
          "$(grep -c 'WARNING: ThreadSanitizer' "$err")" "$checks" "$(what "$err")"
      done
    done
  done
done
