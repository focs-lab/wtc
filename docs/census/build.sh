#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Builds the census library and the five programs it is run on, with frame
# pointers and symbols so that the reports name the first lock's stack.
#
#   docs/census/build.sh <src-dir> <out-dir>
#
# <src-dir> holds memcached (the corpus clone), redis-7.2.4 and
# sqlite-amalgamation-3450100; spdlog's and dedup's binaries are those of
# benchmarks/apps/spdlog and benchmarks/apps/dedup.
set -euo pipefail
SRC=$(cd "${1:?src dir}" && pwd); mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd); WTC=$(cd "$HERE/../.." && pwd)
FP="-O2 -g -fno-omit-frame-pointer"
J=${J:-8}
cmake -S "$WTC/tools/census" -B "$W/census" >/dev/null && cmake --build "$W/census" -j "$J" >/dev/null
(cd "$W/census" && ctest --output-on-failure | tail -1)
# memcached: the corpus clone needs autogen
if [ ! -x "$W/memcached/memcached" ]; then
  rm -rf "$W/memcached" && cp -r "$SRC/memcached" "$W/memcached" && cd "$W/memcached"
  ./autogen.sh >/dev/null 2>&1 && ./configure CFLAGS="$FP" >/dev/null && make -j "$J" memcached >/dev/null 2>&1
  cd "$W"
fi
# redis: its own allocator left out (MALLOC=libc) so that the census sees only redis
if [ ! -x "$W/redis/src/redis-server" ]; then
  rm -rf "$W/redis" && cp -r "$SRC/redis-7.2.4" "$W/redis" && cd "$W/redis"
  make -j "$J" MALLOC=libc OPTIMIZATION="$FP" >/dev/null 2>&1
  cd "$W"
fi
# sqlite: the amalgamation, serialized mode, and the driver
cc $FP -DSQLITE_THREADSAFE=1 -I"$SRC/sqlite-amalgamation-3450100" "$SRC/sqlite-amalgamation-3450100/sqlite3.c" \
   "$HERE/sqlite_driver.c" -lpthread -ldl -lm -o "$W/sqlite_driver"
echo "built: $W"
