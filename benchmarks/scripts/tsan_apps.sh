#!/usr/bin/env bash
#
# The ported applications under ThreadSanitizer (RQ6): does the compiler's
# output of a real program run without a report, and does it compute what
# the original computes? Every build the three build.sh scripts make with
# TSAN=1 runs REPS times per configuration, each run to its end
# (halt_on_error=0) so that its output can be compared and every report it
# makes is named.
#
#   tsan_apps.sh <dedup-out> <ferret-out> <spdlog-out> <ferret-input-dir> <dedup-input>...
#
# The first three are the out-dirs of benchmarks/apps/{dedup,ferret,spdlog}/
# build.sh, run with TSAN=1; the ferret input is a directory with PARSEC's
# corel database and queries (simdev); each dedup input is a file to
# compress. Writes one line per run to stdout: program, configuration,
# run, exit status (66 is ThreadSanitizer's), reports, whether the output
# is the original's, and what each report is about (the global it names,
# else the two functions at the top of its stacks). The reports
# themselves go to ERR_DIR (default: a temporary directory, kept).
# SYMBOLIZER is llvm-symbolizer, by default the one in the LLVM build tree
# next to this repository, else from PATH. REPS defaults to 30.
#
# What each build runs, and what its output is compared with:
#
#   dedup  compiler-tsan                the port as written: every queue
#                                       general (each array of queues is
#                                       one queue; its threads start in a
#                                       function another file calls)
#          compiler-flat-one-unity-tsan one thread per stage, one
#                                       translation unit: three inline
#                                       rings and the list for many
#                                       producers
#          compiler-unity-tsan          the arrays kept, threads per stage
#                                       from argv, one translation unit:
#                                       the queue for one producer and many
#                                       consumers (refine), the list for
#                                       many producers (reorder), two
#                                       general
#          against PARSEC's own build at one thread per stage, byte for byte
#   ferret compiler-one-tsan            four inline rings, the loader's
#                                       queue general
#          compiler-tsan                threads per middle stage from argv:
#                                       inline rings at one, else the list
#                                       for many producers into the output
#                                       stage and the general queue
#          generic-tsan, orig-tsan      the port on the general queue, and
#                                       PARSEC's own build: the controls
#                                       that tell a program's own race from
#                                       the substitution's
#          against PARSEC's own build at one thread per stage: byte for
#          byte, or with more threads per stage line for line
#   spdlog compiler-tsan                a pool of one worker (the list for
#                                       many producers, chosen when the
#                                       pool is built) or two (the general
#                                       queue); every message must reach
#                                       the file once
set -uo pipefail
D=$(cd "${1:?dedup out}" && pwd); F=$(cd "${2:?ferret out}" && pwd); S=$(cd "${3:?spdlog out}" && pwd)
FI=$(cd "${4:?ferret input dir}" && pwd); shift 4
[ $# -gt 0 ] || { echo "no dedup input" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
REPS=${REPS:-30}
SYMBOLIZER=${SYMBOLIZER:-$( [ -x "$ROOT/build-llvm/bin/llvm-symbolizer" ] && echo "$ROOT/build-llvm/bin/llvm-symbolizer" || command -v llvm-symbolizer || true)}
export TSAN_OPTIONS="halt_on_error=0${SYMBOLIZER:+:external_symbolizer_path=$SYMBOLIZER}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
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
line() { # program configuration run rc report-file output
  printf '%s\t%s\trun=%s\trc=%s\treports=%s\toutput=%s\t%s\n' "$1" "$2" "$3" "$4" \
    "$(grep -c 'WARNING: ThreadSanitizer' "$5")" "$6" "$(what "$5")"
}

for inp in "$@"; do
  inp=$(cd "$(dirname "$inp")" && pwd)/$(basename "$inp")
  (cd "$D" && bin/dedup-orig -c -p -t 1 -i "$inp" -o "$T/ref.ddp" >/dev/null 2>&1)
  for run in $(seq 1 "$REPS"); do
    for c in "compiler-tsan 1" "compiler-tsan 2" "compiler-tsan 4" "compiler-flat-one-unity-tsan 1" \
             "compiler-unity-tsan 1" "compiler-unity-tsan 2" "compiler-unity-tsan 4"; do
      set -- $c; err="$E/dedup-$(basename "$inp")-$1-t$2-r$run.err"
      (cd "$D" && bin/dedup-$1 -c -p -t "$2" -i "$inp" -o "$T/o.ddp" >/dev/null 2>"$err"); rc=$?
      line dedup "$(basename "$inp") $1 t=$2" "$run" "$rc" "$err" "$(cmp -s "$T/ref.ddp" "$T/o.ddp" && echo same || echo DIFFERS)"
    done
  done
done

cd "$FI"
"$F/bin/ferret-orig" corel lsh queries 10 20 1 "$T/fref.txt" >/dev/null 2>&1
for run in $(seq 1 "$REPS"); do
  for c in "compiler-one-tsan 1" "compiler-tsan 1" "compiler-tsan 2" "compiler-tsan 4" \
           "generic-tsan 1" "generic-tsan 2" "generic-tsan 4" "orig-tsan 1" "orig-tsan 2" "orig-tsan 4"; do
    set -- $c; err="$E/ferret-$1-n$2-r$run.err"
    "$F/bin/ferret-$1" corel lsh queries 10 20 "$2" "$T/fo.txt" >/dev/null 2>"$err"; rc=$?
    if cmp -s "$T/fref.txt" "$T/fo.txt"; then same="same bytes"
    elif cmp -s <(sort "$T/fref.txt") <(sort "$T/fo.txt"); then same="same lines"
    else same=DIFFERS; fi
    line ferret "$(basename "$FI") $1 n=$2" "$run" "$rc" "$err" "$same"
  done
done

N=20000 MSG=$(printf 'x%.0s' $(seq 64))
for run in $(seq 1 "$REPS"); do
  for c in "1 1" "2 1" "4 1" "8 1" "8 2"; do
    set -- $c; err="$E/spdlog-t$1-w$2-r$run.err"; rm -f "$T/s.log"
    "$S/bin/spdlog-compiler-tsan" --threads "$1" --workers "$2" --n $N --payload 64 --sink file --file "$T/s.log" \
      >/dev/null 2>"$err"; rc=$?
    lines=$(wc -l < "$T/s.log" 2>/dev/null || echo 0); good=$(grep -c -x "$MSG" "$T/s.log" 2>/dev/null || true)
    if [ "$lines" = $(( $1 * N )) ] && [ "$good" = "$lines" ]; then same="all $lines messages"
    else same="DIFFERS ($lines lines, $good as sent, $(( $1 * N )) sent)"; fi
    line spdlog "compiler-tsan threads=$1 workers=$2" "$run" "$rc" "$err" "$same"
  done
done
