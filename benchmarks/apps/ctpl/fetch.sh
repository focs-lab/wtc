#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Clones CTPL at the pinned commit into <dir> (read only afterwards).
#
#   fetch.sh <dir>
#
# The library has no release tags; the pin is the head of its master
# branch, 437e135dbd94eb65b45533d9ce8ee28b5bd37b6d, checked after the
# clone, so a rewritten branch is noticed.
set -euo pipefail
D=${1:?dir}
PIN=437e135dbd94eb65b45533d9ce8ee28b5bd37b6d
if [ ! -d "$D/.git" ]; then
  git clone -q https://github.com/vit-vit/CTPL.git "$D"
  git -C "$D" -c advice.detachedHead=false checkout -q "$PIN"
fi
got=$(git -C "$D" rev-parse HEAD)
[ "$got" = "$PIN" ] || { echo "CTPL at $got, expected $PIN" >&2; exit 1; }
echo "CTPL $PIN at $D"
