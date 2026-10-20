#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Unpacks dedup's inputs into <dir>/{test,simdev,native}.
#
#   fetch-inputs.sh <parsec-dir> <dir>
#
# test and simdev come with the PARSEC tree; simdev is one 1.1 MB text,
# too small to time. PARSEC's simlarge (media.dat) is in its input archive,
# whose site did not answer when this was written (20 October 2026), and
# the published PARSEC image the ferret port uses carries no dedup inputs.
# The native input is a public file, the Fedora Core 6 x86_64 first install
# disc, which the Fedora archive still serves with a signed SHA-1 list; it
# is checked against that SHA-1 and against the SHA-256 recorded here.
set -euo pipefail
P=$(cd "${1:?parsec dir}" && pwd)
mkdir -p "${2:?dir}"; D=$(cd "$2" && pwd)
ISO=FC-6-x86_64-disc1.iso
URL=https://archives.fedoraproject.org/pub/archive/fedora/linux/core/6/x86_64/iso/$ISO

unpack() { # name, archive, sha256
  local name=$1 tar=$2 sum=$3
  echo "$sum  $tar" | sha256sum -c --quiet || { echo "checksum mismatch for $tar" >&2; exit 1; }
  rm -rf "$D/$name" && mkdir -p "$D/$name" && tar xf "$tar" -C "$D/$name"
  echo "$name: $(du -sh "$D/$name" | cut -f1)"
}
unpack test "$P/pkgs/kernels/dedup/inputs/input_test.tar" ada9fb08b78e630669e433d975d3fcd9ef88f6e9b31f8969c3a6566e94f72f0a
unpack simdev "$P/pkgs/kernels/dedup/inputs/input_simdev.tar" ee2a1b090f3c1073511e5d17210969c5359ce301874507182b77dcaca243fda3

mkdir -p "$D/native"
if [ ! -f "$D/native/$ISO" ]; then
  curl -fsS -o "$D/native/$ISO.part" "$URL" && mv "$D/native/$ISO.part" "$D/native/$ISO"
fi
echo "5c976214a16b206761e37bbe5c98e53494b115ac  $D/native/$ISO" | sha1sum -c --quiet \
  || { echo "SHA-1 mismatch for $ISO" >&2; exit 1; }
echo "fcdd05b12aca8170605ad58afc1c5b938d3052203d0d9ef495ce5e73a2ce23b4  $D/native/$ISO" | sha256sum -c --quiet \
  || { echo "SHA-256 mismatch for $ISO" >&2; exit 1; }
echo "native: $(du -sh "$D/native" | cut -f1)"
