#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds PARSEC ferret from the pinned PARSEC tree, six ways, and with
# TSAN=1 four more:
#
#   ferret-orig          PARSEC's own pthreads build (condition-variable queues)
#   ferret-generic-one   the port, one thread per stage, wtc-cc --no-specialize (B0)
#   ferret-compiler-one  the port, one thread per stage, wtc-cc (the compiler's choice)
#   ferret-hand-one      the port, one thread per stage, every queue tagged
#                        spsc_inline by hand (B3)
#   ferret-generic, ferret-compiler   the port, n threads per middle stage
#   ferret-compiler-one-tsan, ferret-compiler-tsan
#                        the compiler's builds under ThreadSanitizer
#   ferret-generic-tsan  the port, n threads per middle stage,
#                        --no-specialize, under ThreadSanitizer
#   ferret-orig-tsan     PARSEC's own pthreads build under ThreadSanitizer
#
#   build.sh <parsec-dir> <out-dir>
#
# <parsec-dir> is bamos/parsec-benchmark at 2e492f771c6749577fe4e7610d516a9e7f8ad580
# (tools/corpus-fetch.sh); it is only read. Needs clang-18, gcc and
# libjpeg's headers, and the WTC toolchain built in the tree this script is
# in. Never run it inside a `bench` reservation; it compiles.
set -euo pipefail
P=$(cd "${1:?parsec dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
# The commit built, asked before the build: a copy of the tree takes
# it from WTC_GIT_SHA (source_rev.sh).
. "$WTC/benchmarks/scripts/source_rev.sh"
source_rev "$WTC" || exit 2
CC=clang-18
JOBS=${JOBS:-16}
cd "$W"

# gsl from the PARSEC tree, static, once. Built with gcc: its test
# directory uses <varargs.h>, which clang 18 refuses; gsl is a dependency
# here, like libjpeg, not part of what is compared.
if [ ! -f gsl/lib/libgsl.a ]; then
  rm -rf gsl-src && cp -r "$P/pkgs/libs/gsl/src" gsl-src && chmod -R u+w gsl-src
  (cd gsl-src && CC=gcc CFLAGS="-O2" bash ./configure --prefix="$W/gsl" --disable-shared >../gsl.log 2>&1 \
     && make -j"$JOBS" >>../gsl.log 2>&1 && make install >>../gsl.log 2>&1) || { echo "gsl build failed:"; tail -25 gsl.log; exit 1; }
fi

# ferret's libraries and PARSEC's own pthreads binary. PARSEC's C predates
# C99's rules, which clang 18 enforces by default, and uses the SVID macro
# HUGE, which glibc no longer defines.
OLDC="-O2 -std=gnu89 -DHUGE=HUGE_VAL -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types"
rm -rf ferret-src && cp -r "$P/pkgs/apps/ferret/src" ferret-src && chmod -R u+w ferret-src
make -C ferret-src CC=$CC CXX=clang++-18 CFLAGS="$OLDC -I$W/ferret-src/include -I$W/gsl/include" \
     LDFLAGS="-L$W/ferret-src/parsec/lib -L$W/gsl/lib" \
     version=pthreads dirs lib "$W/ferret-src/parsec/lib/libcassimage.a" benchmark >ferret-make.log 2>&1 \
  || { echo "ferret make failed"; tail -20 ferret-make.log; exit 1; }
mkdir -p bin && cp ferret-src/parsec/bin/ferret-pthreads bin/ferret-orig

# The port: the one translation unit with the queues and the threads goes
# through wtc-cc, everything else is the libraries above.
python3 "$HERE/port.py" "$P/pkgs/apps/ferret/src/benchmark/ferret-pthreads.c" ferret-wtc.cpp
LIBS="$W/ferret-src/parsec/lib/libcass.a $W/ferret-src/parsec/lib/libcassimage.a $W/gsl/lib/libgsl.a $W/gsl/lib/libgslcblas.a -ljpeg -lm -lrt"
INC="-I$W/ferret-src/include -I$W/ferret-src/image -DHUGE=HUGE_VAL"
build() { # name, wtc-cc flags...
  local name=$1; shift
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 $INC "$@" ferret-wtc.cpp $LIBS -o "bin/ferret-$name" \
    2>"bin/ferret-$name.log" || { echo "$name: FAILED"; tail -5 "bin/ferret-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (queue @[a-z_]+: [a-z_ ]+\([^)]*\)|specialised[^;]*|queue access is unresolvable \([^)]*\))' "bin/ferret-$name.log" \
    | sort | uniq -c | sed 's/^/  /' || true
}
build generic-one  --no-specialize -DFERRET_ONE
build compiler-one -DFERRET_ONE
build hand-one     --no-specialize -DFERRET_ONE -DFERRET_HAND_TAG
build generic      --no-specialize
build compiler
# TSAN=1 adds the compiler's builds under ThreadSanitizer, for the safety
# question, and the port on the general-purpose queue and PARSEC's own
# build the same way, so that a report can be told apart as the program's
# own or the substitution's. The translation unit with the queues and the
# threads is instrumented; ferret's libraries, which start no threads, are
# not (PARSEC's queue is among them: ThreadSanitizer sees its mutex
# through its interceptors).
if [ "${TSAN:-0}" = 1 ]; then
  build compiler-one-tsan -DFERRET_ONE --tsan -O1 -g
  build compiler-tsan --tsan -O1 -g
  build generic-tsan --no-specialize --tsan -O1 -g
  $CC $OLDC -O1 -g -fsanitize=thread -I"$W/ferret-src/include" -I"$W/ferret-src/image" -I"$W/gsl/include" \
      -c ferret-src/benchmark/ferret-pthreads.c -o ferret-orig-tsan.o
  clang++-18 -fsanitize=thread ferret-orig-tsan.o $LIBS -lpthread -o bin/ferret-orig-tsan
fi
{
  echo "compiler=tools/wtc-cc, ClangIR $(echo "$SOURCE_LLVM_SHA" | cut -c1-12)"
  echo "wtc=$(echo "$SOURCE_SHA" | cut -c1-12)$(case $SOURCE_DIRTY in true) echo ' (dirty)';; unknown) echo ' (dirty: unknown)';; esac)"
  echo "parsec=$(git -c safe.directory="$P" -C "$P" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "libraries=clang-18 $OLDC; gsl by gcc"
} > bin/build-info.txt
