#!/usr/bin/env bash
#
# How many wrong topology declarations the declared-topology check catches.
#
#   benchmarks/scripts/declared_recall.sh <work dir> <dpdk checkout> \
#       <supercollider checkout> <boost include dir> [<mutants.tsv>]
#
# Two sets, each against the unmodified code as a control:
#
#  - every mutant of <mutants.tsv> (default: the one in
#    benchmarks/results/2026-10-26-declared-recall), a copy of DPDK's
#    examples or of supercollider's tree with one substitution that makes
#    one declaration wrong, run through tools/declared-check/dpdk.sh or
#    supercollider.sh as the audit ran them;
#  - tools/declared-check/recall_spsc.cpp, three libraries' single-producer
#    single-consumer queues used by a second thread in six ways, checked and
#    also run five times under ThreadSanitizer, built once with the
#    libraries' assertions and once with -DNDEBUG.
#
# The checkouts are only read: DPDK at the audit's commit (dpdk.sh refuses
# another), supercollider at b2f6e4422088 with its submodules (refused at
# another), and a Boost
# whose include directory holds boost/lockfree (supercollider's
# external_libraries/boost does). Every mutant gets its own directory under
# <work dir>, hard-linked to the checkout but for the file it changes.
#
# Output: <work dir>/recall.tsv, one row per run: id, kind, target, queue,
# outcome, the check's line about the queue; and for the library programs
# the two ThreadSanitizer answers. Outcomes: caught (an error: the build
# stops), warned (a warning naming the declaration: it cannot be verified),
# verified (the check says the declaration holds), unresolved (no line about
# the queue, only remarks that accesses to queues of its type are
# unresolvable, which makes every queue of the type unknown), silent
# (nothing at all), vacuous (the declaration holds only because a side it
# declares single has no access in the translation unit, which the
# analysis reports as vacuously satisfied). VACUOUS=0 (passed on to
# dpdk.sh and supercollider.sh) runs the analysis with
# vacuous-outcome=false, which reports those as verified, as the runs
# before 29 Oct did.
# <work dir>/tools.txt records the compilers and commits.
#
# Environment: CLANG, WTC_OPT as in tools/declared-check/dpdk.sh; the DPDK
# build directory is configured once in <work dir>/dpdk-build (MESON).
# TSAN_CXX: the compiler for the ThreadSanitizer builds (default: the
# first of clang++-18, clang++ and g++ that links -fsanitize=thread; the
# pinned clang has no sanitizer runtime). PARTS: "mutants", "libs" or both
# (the default), to rerun one set into a fresh recall.tsv.
set -euo pipefail

W=$(mkdir -p "${1:?usage: $0 <work dir> <dpdk> <supercollider> <boost include> [<mutants.tsv>]}" && cd "$1" && pwd)
D=$(cd "${2:?}" && pwd)
SC=$(cd "${3:?}" && pwd)
BOOST=$(cd "${4:?}" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MUT=${5:-$ROOT/benchmarks/results/2026-10-26-declared-recall/mutants.tsv}
CLANG=${CLANG:-$ROOT/build-llvm/bin/clang}
WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
export CLANG WTC_OPT
CHECK=$ROOT/tools/declared-check
export VACUOUS=${VACUOUS:-1}
ANALYSIS=--queue-ownership-analysis
[ "$VACUOUS" = 0 ] && ANALYSIS=--queue-ownership-analysis=vacuous-outcome=false
PARTS=${PARTS:-mutants libs}
if [ -z "${TSAN_CXX:-}" ]; then
  t=$(mktemp -d)
  echo 'int main() {}' >"$t/t.cpp"
  for c in clang++-18 clang++ g++; do
    if "$c" -fsanitize=thread "$t/t.cpp" -o "$t/t" 2>/dev/null && "$t/t"; then TSAN_CXX=$c; break; fi
  done
  rm -rf "$t"
fi
SC_COMMIT=b2f6e4422088c511ef75951069ad854761b81fe0
[ "$(git -C "$SC" rev-parse HEAD)" = "$SC_COMMIT" ] ||
  { echo "$SC is not at $SC_COMMIT, the commit the audit read" >&2; exit 1; }
{
  echo "clang (CIR): $("$CLANG" --version | head -1)"
  echo "ThreadSanitizer builds: ${TSAN_CXX:-none} ($("${TSAN_CXX:-false}" --version 2>/dev/null | head -1))"
  echo "dpdk: $(git -C "$D" rev-parse HEAD)"
  echo "supercollider: $(git -C "$SC" rev-parse HEAD)"
} >"$W/tools.txt"
OUT=$W/recall.tsv
printf 'id\tkind\ttarget\tqueue\toutcome\tline\ttsan\ttsan_ndebug\n' >"$OUT"

# classify <log> <queue>: the outcome and the first line that decides it.
# A DPDK queue is named by its creation site (file:line:), a member by its
# name, a global by @name.
classify() {
  local log=$1 q=$2 pat lines l
  case $q in
    *.c:*) pat="made at $q" ;;
    @*) pat="queue @([_A-Za-z0-9]*[0-9])?${q#@}[ :]" ;;   # a static's name is mangled (@_ZL1q)
    *) pat="queue $q " ;;
  esac
  lines=$(grep -E "wtc: (queue|the address)" "$log" | grep -E "$pat" | sed -E 's#^[^ ]*/##' || true)
  if l=$(grep -m1 -E 'error: wtc:' <<<"$lines"); then echo "caught	$l"
  elif l=$(grep -m1 -E 'warning: wtc: .*cannot verify' <<<"$lines"); then echo "warned	$l"
  elif l=$(grep -m1 -E 'vacuously satisfied' <<<"$lines"); then echo "vacuous	$l"
  elif l=$(grep -m1 -E 'verified' <<<"$lines"); then echo "verified	$l"
  elif l=$(grep -m1 -E 'queue access is unresolvable' "$log" | sed -E 's#^[^ ]*/##'); then
    echo "unresolved	$l"
  else echo "silent	-"
  fi
}

# mutate <file> <substitution>: applies it once, or fails.
mutate() {
  # perl -i writes a new file, which breaks the hard link to the checkout.
  SUB=$2 perl -0777 -i -pe 'BEGIN { $n = 0 } $n += eval "\$_ =~ $ENV{SUB}" || 0; END { exit($n == 1 ? 0 : 3) }' "$1"
}

# The DPDK build directory, configured once.
B=$W/dpdk-build
[ -f "$B/rte_build_config.h" ] || "$CHECK/dpdk.sh" "$W/base" "$D" >/dev/null
[ -f "$B/rte_build_config.h" ] || cp -a "$W/base/dpdk-build" "$B"

dpdk_run() {  # out-dir examples app
  mkdir -p "$1"
  EXAMPLES=$2 DPDK_BUILD=$B ONLY=$3 "$CHECK/dpdk.sh" "$1" "$D" >"$1/check.out" 2>&1 || true
}

sc_run() {  # work dir with supercollider/ in it
  "$CHECK/supercollider.sh" "$1" >"$1/check.out" 2>&1 || true
}

declare -A done_orig=()
[[ " $PARTS " == *" mutants "* ]] && while IFS=$'\t' read -r id kind target file queue wrong sub reach; do
  case $id in ''|'#'*) continue ;; esac
  M=$W/mutants/$id
  rm -rf "$M"; mkdir -p "$M"
  if [ "$target" = supercollider ]; then
    if [ -z "${done_orig[$target]:-}" ]; then
      mkdir -p "$W/orig/$target"; cp -al "$SC" "$W/orig/$target/supercollider"
      sc_run "$W/orig/$target"; done_orig[$target]=1
    fi
    cp -al "$SC" "$M/supercollider"
    mutate "$M/supercollider/$file" "$sub" || { echo "$id: the substitution does not apply once" >&2; exit 1; }
    sc_run "$M"
    log=$M/supercollider.log orig=$W/orig/$target/supercollider.log
  else
    if [ -z "${done_orig[$target]:-}" ]; then
      dpdk_run "$W/orig/$target" "$D/examples" "$target"; done_orig[$target]=1
    fi
    cp -al "$D/examples" "$M/examples"
    mutate "$M/examples/$file" "$sub" || { echo "$id: the substitution does not apply once" >&2; exit 1; }
    dpdk_run "$M/out" "$M/examples" "$target"
    log=$M/out/$target.log orig=$W/orig/$target/$target.log
    [ -f "$log" ] || { echo "$id: no CIR: $(grep -m1 error "$M/out/$target.cir.err" || true)" >&2; exit 1; }
  fi
  printf '%s\t%s\t%s\t%s\t%s\t-\t-\n' "orig:$id" "$kind" "$target" "$queue" "$(classify "$orig" "$queue")" >>"$OUT"
  printf '%s\t%s\t%s\t%s\t%s\t-\t-\n' "$id" "$kind" "$target" "$queue" "$(classify "$log" "$queue")" >>"$OUT"
done <"$MUT"
[[ " $PARTS " == *" libs "* ]] || { echo "wrote $OUT"; exit 0; }
[ -n "${TSAN_CXX:-}" ] || { echo "no compiler links -fsanitize=thread; set TSAN_CXX" >&2; exit 1; }

# The library programs: the check, then ThreadSanitizer five times, with
# the libraries' assertions and without.
L=$W/libs; mkdir -p "$L"
INC="-I$ROOT/benchmarks/third_party -I$BOOST"

tsan_runs() {  # name defines...: builds, runs five times, prints the counts
  local n=$1 rc races=0 asserts=0 wrong=0
  shift
  # shellcheck disable=SC2086
  "$TSAN_CXX" -std=c++17 -w -O1 -g -fsanitize=thread $INC "$@" \
    "$CHECK/recall_spsc.cpp" -o "$L/$n.tsan" -pthread
  for _ in 1 2 3 4 5; do
    rc=0
    TSAN_OPTIONS="halt_on_error=0 exitcode=66 report_signal_unsafe=0" \
      timeout 120 "$L/$n.tsan" >"$L/$n.run" 2>&1 || rc=$?
    grep -q 'WARNING: ThreadSanitizer' "$L/$n.run" && races=$((races + 1))
    # A library's own assertion (rigtorp's pop, moodycamel's re-entrance
    # guard), or else lost or doubled elements: the counts differ, or the
    # run hangs (124) or dies on another signal.
    if grep -q 'Assertion .* failed' "$L/$n.run"; then
      asserts=$((asserts + 1))
    elif { grep -q 'pushed' "$L/$n.run" && ! grep -qE 'pushed ([0-9]+) taken \1$' "$L/$n.run"; } ||
         [ "$rc" = 124 ] || [ "$rc" -gt 128 ]; then
      wrong=$((wrong + 1))
    fi
  done
  echo "of 5 runs: $races with a race reported, $asserts stopped by a library assertion, $wrong with elements lost or doubled"
}
for lib in RIGTORP MOODY BOOST; do
  for shape in OK P2_FUNCTION P2_LAMBDA P2_PTHREAD P2_LOOP C2_FUNCTION P2_MAIN; do
    n=${lib,,}-${shape,,}
    # shellcheck disable=SC2086
    "${CLANG}++" -std=c++17 -w $INC -DLIB_$lib -DSHAPE_$shape -fclangir -emit-cir \
      "$CHECK/recall_spsc.cpp" -o "$L/$n.cir"
    "$WTC_OPT" "$L/$n.cir" --lift-cir-to-wtc "$ANALYSIS" -o /dev/null >"$L/$n.log" 2>&1 || true
    tsan=$(tsan_runs "$n" -DLIB_$lib -DSHAPE_$shape)
    tsan_ndebug=$(tsan_runs "$n.ndebug" -DNDEBUG -DLIB_$lib -DSHAPE_$shape)
    k=c; [ "$shape" = OK ] && k=control
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$k" "recall_spsc.cpp" "@q" "$(classify "$L/$n.log" "@q")" \
      "$tsan" "$tsan_ndebug" >>"$OUT"
  done
done

echo "wrote $OUT"
