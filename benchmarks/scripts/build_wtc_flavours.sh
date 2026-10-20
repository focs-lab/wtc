#!/usr/bin/env bash
#
# Builds the four flavours of the pipeline workload that answer "does the
# compiler pick the implementation, and does its pick cost what the human's
# pick costs":
#
#   generic      source unmodified, specialisation off      -> the fallback
#   hand         source sets the tag to spsc itself         -> the human's pick
#   hand-inline  source sets spsc_inline itself              -> the human's
#                pick with elements held in the ring
#   compiler     source unmodified, the verdicts set the tag -> ours; both
#                queues are spsc and their consumers only borrow, so it
#                must choose spsc_inline, exactly as hand-inline does
#
# All four go through the same pipeline, so a difference between them is
# the substitution and not a difference between compilers.
#
#   build_wtc_flavours.sh <out-dir>
#
# Never run this inside a `bench` reservation; it compiles. It takes the
# host's shared build lock itself.
set -euo pipefail

OUT=${1:?output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/benchmarks/workloads/w3_pipeline_wtc.cpp"
CC="$ROOT/tools/wtc-cc"

mkdir -p "$OUT"
: > "$OUT/flavours.txt"
export WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
INC="-I$ROOT/benchmarks/queues -I$ROOT/benchmarks/harness"

# Through the host's build lock where there is one, as the W7 and W8
# scripts do; a machine without it builds directly.
LOCK=()
command -v machine-lock >/dev/null && LOCK=(machine-lock --lane wtc --cores 4 --mem 8G --minutes 15 --why "wtc w3 build" --)

build() { # name, extra wtc-cc args...
  local name=$1; shift
  echo "=== $name"
  echo "flavour $name=-O2 $*" >> "$OUT/flavours.txt"
  "${LOCK[@]}" "$CC" --toolchain "$ROOT/build-llvm" $INC -O2 "$@" "$SRC" -o "$OUT/w3_$name" \
    2>"$OUT/$name.log" || { echo "  FAILED; last lines:"; tail -5 "$OUT/$name.log"; return 1; }
  grep -oE 'wtc: (queue @[A-Za-z0-9_]+: [a-z_ ]+|specialised[^;]*|not specialised[^;]*)' "$OUT/$name.log" \
    | sort | uniq -c | sed 's/^/  /' || true
}

build generic  --no-specialize
build hand     --no-specialize -DWTC_HAND
build hand-inline --no-specialize -DWTC_HAND_INLINE
build compiler

echo
echo "=== which implementation each build actually uses ==="
# Asked of the objects at run time, not read off a disassembly. The tag
# branch lives inside the shim's member functions, which are the same
# symbols in every build, so comparing the pipeline's own functions shows
# nothing: the first version of this check called three identical builds
# "identical" and counted it as success.
for f in generic hand hand-inline compiler; do
  printf '  %-11s ' "$f"
  "$OUT/w3_$f" --n 200000 --warm 20000 --json /dev/null 2>/dev/null
done
echo
echo "  generic must say generic,generic; hand spsc,spsc; hand-inline and compiler"
echo "  must both say spsc_inline,spsc_inline."
echo
echo "=== and the binaries themselves ==="
# The strongest form of the claim, and the cheapest to check: if the
# compiler's build and the human's are the same bytes, no measurement can
# separate them and none needs to. The generic build must differ, or
# nothing was substituted at all.
if cmp -s "$OUT/w3_hand-inline" "$OUT/w3_compiler"; then
  echo "  hand-inline and compiler: byte for byte identical"
else
  echo "  hand-inline and compiler: DIFFER — the compiler did not reproduce the human's choice"
fi
if cmp -s "$OUT/w3_generic" "$OUT/w3_compiler"; then
  echo "  generic and compiler: IDENTICAL — nothing was substituted"
else
  echo "  generic and compiler: differ in $(cmp -l "$OUT/w3_generic" "$OUT/w3_compiler" | wc -l) bytes, as they must"
fi

# What was built, for meta.json: the toolchain and each flavour's flags.
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$ROOT" -C "$ROOT" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null)$([ -n "$(git -c safe.directory="$ROOT" -C "$ROOT" status --porcelain 2>/dev/null)" ] && echo ' (dirty)')"
  cat "$OUT/flavours.txt"
} > "$OUT/build-info.txt"
