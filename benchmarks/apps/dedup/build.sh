#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Builds PARSEC dedup from the pinned PARSEC tree:
#
#   dedup-orig               PARSEC's own pthreads build (condition-variable queues)
#   dedup-generic            the port, wtc-cc --no-specialize (B0)
#   dedup-compiler           the port, wtc-cc (the compiler's choice)
#   dedup-generic-one        the port, one spawn per stage, --no-specialize
#   dedup-compiler-one       the port, one spawn per stage, wtc-cc
#   dedup-hand-one           the port, one spawn per stage, tagged by hand (B3):
#                            spsc_inline where single-producer
#   dedup-hand-spsc-one      the same with plain spsc rings
#   dedup-compiler-flat-one  the port with its queues as globals, one spawn
#                            per stage, wtc-cc: what the analysis would see
#                            if arrays of queues were no obstacle
#   dedup-compiler-flat-one-unity  the same with dedup's main in the
#                            encoder's translation unit: what it would see
#                            if, besides, it saw the whole program
#   dedup-compiler-unity     the port as written, arrays of queues and
#                            threads per stage from argv, with main in the
#                            encoder's translation unit: the arrays alone
#   dedup-compiler-one-unity the same with one spawn per stage
#   dedup-lifted             PARSEC's own sources through wtc-cc, unedited:
#                            the encoder, the queue and dedup's main as one
#                            unit (--unity), the queue's functions lifted
#                            where the proof allows (docs/lifting.md, "C
#                            queues"), its batches included
#   dedup-unlifted           the same with --no-lift
#   dedup-lifted-tsan        (TSAN=1) the lifted build under ThreadSanitizer
#
#   build.sh <parsec-dir> <out-dir>
#
# <parsec-dir> is bamos/parsec-benchmark at 2e492f771c6749577fe4e7610d516a9e7f8ad580
# (tools/corpus-fetch.sh); it is only read. Needs clang-18 and zlib's
# headers, and the WTC toolchain built in the tree this script is in.
# Never run it inside a `bench` reservation; it compiles.
set -euo pipefail
P=$(cd "${1:?parsec dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
# The commit built, asked before the build: a copy of the tree takes
# it from WTC_GIT_SHA (source_rev.sh).
. "$WTC/benchmarks/scripts/source_rev.sh"
source_rev "$WTC" || exit 2
SRC="$P/pkgs/kernels/dedup/src"
CC=clang-18
cd "$W"
mkdir -p obj bin

# PARSEC's pthreads build, with the flags of its Makefile (gzip
# compression, statistics from config.h), and the same objects for the
# port's other files: everything but the encoder and the queue. The SHA-1
# dedup carries is an old OpenSSL one whose x86 rotate is inline assembly
# clang rejects (an operand's width); OPENSSL_NO_INLINE_ASM selects its C
# rotate, the same digest. Three of its files define the same globals
# without an initialiser, which links only as common symbols, the default
# of the compilers PARSEC was written for (-fcommon). Every build is timed
# by PARSEC's own hooks, from the first thread started to the last joined
# ("Total time spent in ROI"); they pin nothing unless PARSEC_CPU_NUM is
# set.
HOOKS="$P/pkgs/libs/hooks/src"
DEFS="-D_XOPEN_SOURCE=600 -DENABLE_GZIP_COMPRESSION -DENABLE_PTHREADS -DENABLE_PARSEC_HOOKS -I$HOOKS/include"
CFLAGS="-O2 -fno-strict-aliasing -fcommon -pthread $DEFS -DOPENSSL_NO_INLINE_ASM -I$SRC"
for f in hashtable util dedup rabin encoder decoder mbuffer sha queue binheap tree; do
  $CC $CFLAGS -w -c "$SRC/$f.c" -o "obj/$f.o"
done
$CC -O2 -w -D_GNU_SOURCE -I"$HOOKS" -I"$HOOKS/include" -c "$HOOKS/hooks.c" -o obj/hooks.o
OTHERS="obj/hashtable.o obj/util.o obj/dedup.o obj/rabin.o obj/decoder.o obj/mbuffer.o obj/sha.o obj/binheap.o obj/tree.o obj/hooks.o"
$CC -pthread $OTHERS obj/encoder.o obj/queue.o -o bin/dedup-orig -lz -lm

python3 "$HERE/port.py" "$SRC/encoder.c" encoder-wtc.cpp "$SRC/dedup.c"
build() { # name, wtc-cc flags...
  local name=$1; shift
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 -fno-strict-aliasing $DEFS -I"$SRC" "$@" \
    encoder-wtc.cpp ${OBJS:-$OTHERS} -lz -lm -o "bin/dedup-$name" \
    2>"bin/dedup-$name.log" || { echo "$name: FAILED"; tail -5 "bin/dedup-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (queue [^:]+: [a-z_ ]+(\([^)]*\))?|specialised[^;]*|queue access is unresolvable \([^)]*\))' \
    "bin/dedup-$name.log" | sort | uniq -c | sed 's/^/  /' || true
}
build generic            --no-specialize
build compiler
build generic-one        --no-specialize -DDEDUP_ONE
build compiler-one       -DDEDUP_ONE
build hand-one           --no-specialize -DDEDUP_ONE -DDEDUP_HAND_TAG=spsc_inline
build hand-spsc-one      --no-specialize -DDEDUP_ONE -DDEDUP_HAND_TAG=spsc
build compiler-flat-one  -DDEDUP_FLAT -DDEDUP_ONE
OBJS=${OTHERS/obj\/dedup.o /} build compiler-flat-one-unity -DDEDUP_FLAT -DDEDUP_ONE -DDEDUP_UNITY
OBJS=${OTHERS/obj\/dedup.o /} build compiler-unity -DDEDUP_UNITY
OBJS=${OTHERS/obj\/dedup.o /} build compiler-one-unity -DDEDUP_ONE -DDEDUP_UNITY

# The original sources, unedited: the files with the queues, the threads
# and main as one unit; the others as PARSEC builds them.
lifted() { # name, objects, wtc-cc flags...
  local name=$1 objs=$2; shift 2
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 -fno-strict-aliasing -fcommon $DEFS -DOPENSSL_NO_INLINE_ASM \
    -I"$SRC" -w "$@" --unity "$SRC/encoder.c" "$SRC/queue.c" "$SRC/dedup.c" $objs -lz -lm -o "bin/dedup-$name" \
    2>"bin/dedup-$name.log" || { echo "$name: FAILED"; tail -5 "bin/dedup-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (C queue [^:(]+ (lifted|not lifted: \([a-e]\) [^,;]*)|queue [^:]+: [a-z_ ]+\([^)]*\)|specialised[^;]*)' \
    "bin/dedup-$name.log" | sed -E 's/lifted \(.*/lifted/' | sort | uniq -c | sed 's/^/  /' || true
}
lifted lifted "${OTHERS/obj\/dedup.o /}"
lifted unlifted "${OTHERS/obj\/dedup.o /}" --no-lift
# TSAN=1 adds the compiler's builds under ThreadSanitizer, every file
# instrumented, for the safety question: the build as written, whose
# queues stay general; the one translation unit with one thread per stage,
# whose proved queues are inline rings and the list for many producers;
# and the arrays kept in one translation unit with threads per stage from
# argv, whose refine queue is the queue for one producer and many
# consumers and whose reorder queue is the list for many producers.
if [ "${TSAN:-0}" = 1 ]; then
  mkdir -p obj-tsan
  for f in hashtable util dedup rabin decoder mbuffer sha binheap tree; do
    $CC $CFLAGS -O1 -g -fsanitize=thread -w -c "$SRC/$f.c" -o "obj-tsan/$f.o"
  done
  $CC -O1 -g -fsanitize=thread -w -D_GNU_SOURCE -I"$HOOKS" -I"$HOOKS/include" -c "$HOOKS/hooks.c" -o obj-tsan/hooks.o
  TOTHERS=${OTHERS//obj\//obj-tsan/}
  OBJS=$TOTHERS build compiler-tsan --tsan -O1 -g
  OBJS=${TOTHERS/obj-tsan\/dedup.o /} build compiler-flat-one-unity-tsan --tsan -O1 -g -DDEDUP_FLAT -DDEDUP_ONE -DDEDUP_UNITY
  OBJS=${TOTHERS/obj-tsan\/dedup.o /} build compiler-unity-tsan --tsan -O1 -g -DDEDUP_UNITY
  lifted lifted-tsan "${TOTHERS/obj-tsan\/dedup.o /}" --tsan -O1 -g
fi
{
  echo "compiler=tools/wtc-cc, ClangIR $(echo "$SOURCE_LLVM_SHA" | cut -c1-12)"
  echo "wtc=$(echo "$SOURCE_SHA" | cut -c1-12)$(case $SOURCE_DIRTY in true) echo ' (dirty)';; unknown) echo ' (dirty: unknown)';; esac)"
  echo "parsec=$(git -c safe.directory="$P" -C "$P" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "others=$CC $CFLAGS"
} > bin/build-info.txt
