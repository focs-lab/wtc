#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Builds RocksDB's db_bench for the census: gflags first (db_bench needs it),
# then RocksDB with frame pointers and symbols, and with the census's
# reference-count hooks (tools/census/rc_hooks.h) included in every file,
# so that one binary gives both the mutex and the shared_ptr census.
#
#   docs/census/build_rocksdb.sh <src-dir> <out-dir>
#
# <src-dir> holds rocksdb (the corpus clone) and gflags-2.2.2 (the GitHub
# release tarball, sha256 34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf).
# J sets the job count (default 8).
set -euo pipefail
SRC=$(cd "${1:?src dir}" && pwd); mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd); WTC=$(cd "$HERE/../.." && pwd)
J=${J:-8}
CXX=${CXX:-clang++-18}; CC=${CC:-clang-18}
FP="-O2 -g -fno-omit-frame-pointer"

if [ ! -f "$W/gflags/lib/libgflags.a" ]; then
  cmake -G Ninja -S "$SRC/gflags-2.2.2" -B "$W/gflags-build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF -DREGISTER_INSTALL_PREFIX=OFF -DREGISTER_BUILD_DIR=OFF \
        -DCMAKE_INSTALL_PREFIX="$W/gflags" >/dev/null
  ninja -C "$W/gflags-build" -j "$J" install >/dev/null
fi

cmake -G Ninja -S "$SRC/rocksdb" -B "$W/rocksdb-build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_CXX_FLAGS="$FP -include $WTC/tools/census/rc_hooks.h" -DCMAKE_C_FLAGS="$FP" \
      -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g" -DFAIL_ON_WARNINGS=OFF \
      -DWITH_GFLAGS=ON -Dgflags_DIR="$W/gflags/lib/cmake/gflags" -DCMAKE_PREFIX_PATH="$W/gflags" \
      -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON -DWITH_ZLIB=ON \
      -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=ON -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF \
      -DWITH_JNI=OFF -DROCKSDB_BUILD_SHARED=OFF -DPORTABLE=1 >/dev/null
ninja -C "$W/rocksdb-build" -j "$J" db_bench >/dev/null
echo "built: $W/rocksdb-build/db_bench"
