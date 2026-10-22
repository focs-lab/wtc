#!/usr/bin/env bash
# scrub_paths.sh DIR: rewrite the paths in a leg's result files so that they
# name no one. The runs record their command lines and meta.json the
# directories a leg was given, and on a shared host those paths carry a
# user's name. Paths inside the tree become relative to it; the directories
# given in the environment (FLAVOURS, INPUTS, BUILD, CONTROL_BUILD) are
# written as those variables; whatever is left under a home or a per-user
# directory becomes ~. campaign.sh runs it at the end of every leg.
set -euo pipefail
DIR=${1:?result directory}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

e=()
# The given directories go first, since one may lie inside the tree; when
# CONTROL_BUILD is BUILD, the runs say BUILD.
for v in FLAVOURS INPUTS BUILD CONTROL_BUILD; do
  p=${!v:-}
  [ -n "$p" ] && [ "$p" != "$ROOT" ] || continue
  e+=(-e "s#${p%/}#\$$v#g")
done
e+=(-e "s#$ROOT/##g" -e "s#$ROOT#.#g" -e "s#$HOME#~#g"
    -e "s#/[^/\" ]*/$(id -un)/#~/#g")

for f in "$DIR"/*; do
  if [ -f "$f" ]; then sed -i "${e[@]}" "$f"; fi
done
