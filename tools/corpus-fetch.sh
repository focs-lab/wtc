#!/usr/bin/env bash
# Shallow-clone the codebases whose authors are known to have fought false
# sharing by hand.  Each hand-written cache-line annotation is a place where
# an expert already decided "different threads touch these", which is the
# oracle the WTC ownership analysis is measured against.  Depth 1: the census
# reads the current source, not the history.
#
#   tools/corpus-fetch.sh /path/to/corpus        (about 7 GB with llvm-project)
#
# The corpus is deliberately NOT inside the repository: it is other people's
# source, it is large, and the census is reproducible from this script.
set -u
DEST=${1:-${WTC_CORPUS:?set WTC_CORPUS or pass a destination directory}}
mkdir -p "$DEST" && cd "$DEST"
echo "cloning into $DEST"
repos=(
  linux=https://github.com/torvalds/linux.git
  dpdk=https://github.com/DPDK/dpdk.git
  folly=https://github.com/facebook/folly.git
  rocksdb=https://github.com/facebook/rocksdb.git
  onetbb=https://github.com/uxlfoundation/oneTBB.git
  jemalloc=https://github.com/jemalloc/jemalloc.git
  abseil=https://github.com/abseil/abseil-cpp.git
  postgres=https://github.com/postgres/postgres.git
  nginx=https://github.com/nginx/nginx.git
  libuv=https://github.com/libuv/libuv.git
  leveldb=https://github.com/google/leveldb.git
  memcached=https://github.com/memcached/memcached.git
  parsec=https://github.com/bamos/parsec-benchmark.git
  # The three the census read from local trees before 17 October 2026:
  # pinned to the releases those trees were, so a recount compares.
  # llvm-project gives both llvm/ and compiler-rt/.
  mysql=https://github.com/mysql/mysql-server.git@mysql-8.0.39
  sqlite=https://github.com/sqlite/sqlite.git@version-3.50.2
  llvm-project=https://github.com/llvm/llvm-project.git
)
for r in "${repos[@]}"; do
  name=${r%%=*}; url=${r#*=}; tag=${url##*@}; url=${url%@*}
  [ "$tag" = "$url" ] && tag=
  if [ -d "$name/.git" ]; then echo "SKIP $name (present)"; continue; fi
  printf 'FETCH %-12s ' "$name"
  if timeout 3600 git clone --quiet --depth 1 --single-branch ${tag:+--branch "$tag"} "$url" "$name" 2>/dev/null; then
    echo "ok  $(du -sh "$name" 2>/dev/null | cut -f1)"
  else
    echo "FAILED"
  fi
done
echo "=== total: $(du -sh . 2>/dev/null | cut -f1) ==="
