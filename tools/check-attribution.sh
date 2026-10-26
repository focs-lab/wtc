#!/usr/bin/env bash
#
# Refuses commits that credit the tool they were written with: a
# Co-authored-by or session trailer naming an AI assistant, a "Generated
# with ..." footer, or an author or committer address at an AI vendor.
# Authorship is git's author field and nothing is added to it (CLAUDE.md,
# "Authorship"). Such a trailer on any branch pushed here lists the vendor's
# account among the repository's contributors, and only deleting every
# branch and pull request that carries it takes it off again.
#
#   tools/check-attribution.sh <revision range>   # e.g. origin/main..HEAD
#   tools/check-attribution.sh --message <file>   # one message, as commit-msg
#
# The same script runs in CI on every push and pull request
# (.github/workflows/attribution.yml) and, once a clone runs
#
#   git config core.hooksPath tools/hooks
#
# as the commit-msg hook, before a commit exists at all.
#
set -euo pipefail

TOOLS='claude|anthropic|copilot|chatgpt|openai|gpt-[0-9]|gemini|cursor|codeium|tabnine|aider|devin'
TRAILER="^[[:space:]]*(co-authored-by|claude-session|session|generated-by|assisted-by):.*($TOOLS)"
FOOTER="generated (with|by) .*($TOOLS)"
VENDOR='@(anthropic\.com|openai\.com)>?$'

# message on stdin; prints the offending lines, returns 1 if there are any
scan_message() {
  grep -v '^#' | grep -n -i -E "$TRAILER|$FOOTER" && return 1 || return 0
}

if [ "${1:-}" = --message ]; then
  [ -f "${2:-}" ] || { echo "usage: $0 --message <file>" >&2; exit 2; }
  if ! scan_message < "$2"; then
    echo "commit message credits an AI tool; remove the line(s) above" >&2
    exit 1
  fi
  exit 0
fi

RANGE=${1:?usage: $0 <revision range> | --message <file>}
bad=0
for c in $(git rev-list "$RANGE"); do
  if ! out=$(git log -1 --format=%B "$c" | scan_message); then
    echo "$(git log -1 --format='%h %s' "$c"): message credits an AI tool:" >&2
    echo "$out" | sed 's/^/    /' >&2
    bad=1
  fi
  for who in an:ae cn:ce; do
    email=$(git log -1 --format="%${who#*:}" "$c")
    if printf '%s\n' "$email" | grep -q -i -E "$VENDOR"; then
      echo "$(git log -1 --format='%h %s' "$c"): ${who%%:*} address $email is an AI vendor's" >&2
      bad=1
    fi
  done
done
[ "$bad" = 0 ] || { echo "rewrite these commits without the lines named above" >&2; exit 1; }
