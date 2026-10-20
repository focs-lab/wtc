#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unpacks ferret's inputs into <dir>/{test,simdev,simlarge}.
#
#   fetch-inputs.sh <parsec-dir> <dir>
#
# test and simdev come with the PARSEC tree. simlarge is in PARSEC's input
# archive, whose site (parsec.cs.princeton.edu) did not answer when this was
# written (20 October 2026); it is taken from a published image of the
# PARSEC tree, pinned by digest, and the archive is checked against the
# checksum recorded here. Needs docker for simlarge.
set -euo pipefail
P=$(cd "${1:?parsec dir}" && pwd)
mkdir -p "${2:?dir}"; D=$(cd "$2" && pwd)
IMAGE=anakli/parsec@sha256:6d1734426b3807bc415b4bfc1e25b80693d8ba670832994f0248590c3085848d

unpack() { # name, archive, sha256
  local name=$1 tar=$2 sum=$3
  echo "$sum  $tar" | sha256sum -c --quiet || { echo "checksum mismatch for $tar" >&2; exit 1; }
  rm -rf "$D/$name" && mkdir -p "$D/$name" && tar xf "$tar" -C "$D/$name"
  echo "$name: $(du -sh "$D/$name" | cut -f1)"
}
unpack test "$P/pkgs/apps/ferret/inputs/input_test.tar" 197f8146b1abd0eb2165a1c91764780f947a20c536724f384314e2f006af65f4
unpack simdev "$P/pkgs/apps/ferret/inputs/input_simdev.tar" f821396e6685cf454b8710e10fc4d3f7ddee74dbfa24bb68cf1c4c33c1d17883
docker pull -q "$IMAGE" >/dev/null
docker run --rm --entrypoint cat "$IMAGE" /parsec-benchmark/pkgs/apps/ferret/inputs/input_simlarge.tar > "$D/input_simlarge.tar"
unpack simlarge "$D/input_simlarge.tar" 3de09035c86baae6d6d8347a5fd47fd83b91327aaeec4c2222b90b6bd0e2e5d9
rm -f "$D/input_simlarge.tar"
