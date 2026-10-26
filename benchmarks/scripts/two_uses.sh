#!/usr/bin/env bash
#
# Does the progress check read the facts the specialisation established?
# Each lock-free function is checked twice, on the same CIR, by the same
# pipeline wtc-cc runs: once with the tags rewritten (the specialised
# build) and once with them left alone (wtc-cc --no-specialize, the
# general build). A function whose verdict differs between the two owes
# that difference to the topology and ownership facts, read through the
# progress answer of the implementation the specialisation chose.
#
#   two_uses.sh <out.tsv> < jobs
#
# Each job line is "label|lang|file|flags": lang is c or c++, file a
# source or a .cir already emitted, relative to the repository root or
# absolute, flags extra clang flags (the shim's include directory is added
# for c++ sources). Blank lines and lines starting with # are skipped.
# The tools are found as tools/wtc-cc finds them: clang and clang++ in
# $TOOLCHAIN/bin when TOOLCHAIN is set, else the LLVM build tree next to
# this repository, /opt/llvm, then PATH; wtc-opt is WTC_OPT, else this
# repository's build tree, then PATH. CLANG and CLANGXX override the
# former.
#
# A job fails the script when wtc-opt reports an error that is not the
# check's own, exits without a verdict to show for it, or finds no
# function declared lock-free: a crash must not read as "no verdicts".
#
# The rows are "label, function, general, specialised", each verdict
# "pass" or "fail"; a function that appears in one build only is "-" in
# the other.
set -euo pipefail

OUT=${1:?out.tsv}
[[ $OUT == /* ]] || OUT=$PWD/$OUT
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLCHAIN=${TOOLCHAIN:-}

find_tool() { # name, candidates...
  local name=$1; shift
  for c in "$@"; do [ -x "$c" ] && { echo "$c"; return; }; done
  command -v "$name" || { echo "two_uses: cannot find $name" >&2; exit 2; }
}
if [ -n "$TOOLCHAIN" ]; then
  CLANG=${CLANG:-$TOOLCHAIN/bin/clang}; CLANGXX=${CLANGXX:-$TOOLCHAIN/bin/clang++}
else
  CLANG=${CLANG:-$(find_tool clang "$ROOT/build-llvm/bin/clang" /opt/llvm/bin/clang)}
  CLANGXX=${CLANGXX:-$(find_tool clang++ "$ROOT/build-llvm/bin/clang++" /opt/llvm/bin/clang++)}
fi
WTC_OPT=${WTC_OPT:-$(find_tool wtc-opt "$ROOT/build/bin/wtc-opt")}
[ -x "$WTC_OPT" ] || { echo "two_uses: $WTC_OPT is not an executable" >&2; exit 2; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
TAB=$'\t'
export LC_ALL=C
cd "$ROOT"

PIPE=(--lift-cir-to-wtc --wtc-batch-pushes --wtc-batch-locks --queue-ownership-analysis --wtc-borrow-analysis)

verdicts() { # label cir specialise-option -> "function verdict" lines
  # A function that can block is an error, so wtc-opt fails whenever the
  # check finds one; any other failure is the script's.
  local log=$WORK/log status=0
  "$WTC_OPT" --mlir-print-op-on-diagnostic=false "$2" "${PIPE[@]}" "$3" --wtc-check-progress -o /dev/null > "$log" 2>&1 || status=$?
  sed -n -E "s/.*wtc: (.*) is declared lock-free: nothing it reaches blocks.*/\1\tpass/p; s/.*wtc: (.*) is declared lock-free but can block.*/\1\tfail/p" "$log" |
    sort -t "$TAB" -k1,1 -u > "$WORK/v"
  if grep -E '(^| )error: ' "$log" | grep -q -v -E ' error: wtc: '; then
    echo "two_uses: $1 ($3): wtc-opt reported an error of its own:" >&2; grep -E '(^| )error: ' "$log" | grep -v -E ' error: wtc: ' | head -5 >&2; exit 1
  fi
  if [ "$status" -ne 0 ] && ! grep -q "${TAB}fail\$" "$WORK/v"; then
    echo "two_uses: $1 ($3): wtc-opt exited with $status and no failing verdict:" >&2; tail -5 "$log" >&2; exit 1
  fi
  if [ ! -s "$WORK/v" ]; then
    echo "two_uses: $1 ($3): no function declared lock-free" >&2; exit 1
  fi
  cat "$WORK/v"
}

printf 'label\tfunction\tgeneral\tspecialised\n' > "$OUT"
n=0
while IFS='|' read -r label lang file flags; do
  [[ -z ${label// } || $label == \#* ]] && continue
  n=$((n + 1)); cir=$WORK/$n.cir
  if [[ $file == *.cir ]]; then cp "$file" "$cir"
  elif [[ $lang == c ]]; then
    # shellcheck disable=SC2086
    "$CLANG" -fclangir -emit-cir -w $flags "$file" -o "$cir"
  else
    # shellcheck disable=SC2086
    "$CLANGXX" -std=c++17 -I"$ROOT/shim" -fclangir -emit-cir -w $flags "$file" -o "$cir"
  fi
  verdicts "$label" "$cir" --wtc-specialize=rewrite=false > "$WORK/g"
  verdicts "$label" "$cir" --wtc-specialize > "$WORK/s"
  join -t "$TAB" -a1 -a2 -e - -o 0,1.2,2.2 "$WORK/g" "$WORK/s" |
    awk -F'\t' -v l="$label" '{ printf "%s\t%s\t%s\t%s\n", l, $1, $2, $3 }' >> "$OUT"
done

awk -F'\t' 'NR > 1 { n++; if ($3 != $4) d++ } END { printf "%d lock-free functions, %d with a different verdict in the general build\n", n, d }' "$OUT"
