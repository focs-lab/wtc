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
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$WTC" -C "$WTC" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$WTC" -C "$WTC" rev-parse --short=12 HEAD 2>/dev/null)"
  echo "parsec=$(git -c safe.directory="$P" -C "$P" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "others=$CC $CFLAGS"
} > bin/build-info.txt
