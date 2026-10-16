#!/usr/bin/env bash
#
# The branch policy for main, applied from branch-protection.json beside this
# script. That file is the record: it is reviewable in a diff and it travels
# with the repository, which a setting clicked in a web interface does not.
#
# Prints what would change and stops. Pass --apply to show the same difference
# and then apply it after a confirmation, so one invocation and one token are
# enough; running it twice only asks for the token twice.
#
# Prompts read from the terminal rather than standard input where there is
# one. Do not paste this and the next command as one block regardless: a
# prompt consumes the line queued behind it, and the command you meant to run
# becomes the answer to a question.
#
# Needs a token with admin rights on the repository. The inherited one is
# tried first and is usually not it: on a host where several tools export
# their own environment, the token that answers is whichever was exported
# last, and it carries whatever scopes that tool needed. A prompt follows when
# it is refused, and what is typed there is never stored.
#
# A context is added to branch-protection.json only after that check has
# reported green at least once. A required check that has never run leaves
# every pull request waiting for a status that will not arrive, which looks
# like a broken repository and is tedious to undo.
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
POLICY="tools/branch-protection.json"
SLUG=$(git remote get-url origin | sed -E 's#(git@github\.com:|https://github\.com/)([^/]+/[^/.]+)(\.git)?#\2#')
BRANCH=main
APPLY=${1:-}

command -v jq >/dev/null || { echo "jq is required"; exit 1; }
[ -f "$POLICY" ] || { echo "no such file: $POLICY"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# A prompt belongs to the person, not to whatever is piped in. Falls back to
# standard input where there is no terminal, so the script stays testable.
# Opening it is the test. `[ -r /dev/tty ]` only reads the permission bits of
# the device node, which are the same whether or not this process has a
# controlling terminal, so it answers yes in exactly the case that breaks.
if { : < /dev/tty; } 2>/dev/null; then ASK=/dev/tty; else ASK=/dev/stdin; fi

echo "repository: $SLUG, branch: $BRANCH"
echo

CURRENT=""; WHY=""
# gh prints the response body to stdout on failure too, so the substitution
# captures it either way and the exit status is what decides. An unprotected
# branch and a refused request mean opposite things and must not be conflated:
# one is "nothing to compare against", the other is "you cannot see it".
fetch() {
  local body
  body=$(gh api "repos/$SLUG/branches/$BRANCH/protection" 2>/dev/null) \
    && { CURRENT=$body; return 0; }
  WHY=$(jq -r '.message // "no answer"' <<<"${body:-{\}}" 2>/dev/null || echo "no answer")
  case "$WHY" in
    *"not protected"*|*"Branch not protected"*) CURRENT='{}'; return 0 ;;
    *) return 1 ;;
  esac
}

if ! fetch; then
  echo "the inherited token was refused: $WHY"
  read -rsp "  token with admin rights (not stored, not echoed): " TOK < "$ASK"; echo
  [ -n "$TOK" ] || { echo "empty token"; exit 1; }
  export GH_TOKEN="$TOK"
  fetch || { echo "this token was refused too: $WHY"; exit 1; }
  echo
fi

if [ "$CURRENT" = "{}" ] || [ "$(jq -r 'has("required_pull_request_reviews")' <<<"$CURRENT")" = "false" ]; then
  echo "the branch is not protected now"
else
  # Only the fields the policy file governs. GitHub adds the rest on its
  # own, and in a diff they are noise.
  NORM='{required_status_checks: {strict: .required_status_checks.strict,
                                  contexts: .required_status_checks.contexts},
         enforce_admins: .enforce_admins.enabled,
         required_pull_request_reviews: {
           required_approving_review_count: .required_pull_request_reviews.required_approving_review_count,
           require_code_owner_reviews: .required_pull_request_reviews.require_code_owner_reviews}}'
  jq -S "$NORM" <<<"$CURRENT" > "$TMP/current.json"
  jq -S 'del(.restrictions)' "$POLICY"  > "$TMP/wanted.json"
  if diff -u "$TMP/current.json" "$TMP/wanted.json" > "$TMP/diff"; then
    echo "the policy already matches the file; nothing to change"
    exit 0
  fi
  echo "difference (minus is what is set now):"
  sed 's/^/  /' "$TMP/diff"
fi

echo
if [ "$APPLY" != "--apply" ]; then
  echo "nothing changed. To apply: $0 --apply"
  exit 0
fi

read -rp "apply this to $SLUG:$BRANCH? [y/N] " YES < "$ASK"
# Anything but a yes, an empty answer included, declines: the safe answer
# is the one you get by pressing return.
case "$YES" in
  y|Y|yes|Yes) ;;
  *) echo "nothing changed"; exit 0 ;;
esac

gh api -X PUT "repos/$SLUG/branches/$BRANCH/protection" --input "$POLICY" >/dev/null
echo "applied"
gh api "repos/$SLUG/branches/$BRANCH" --jq '"protected: " + (.protected|tostring)'
gh api "repos/$SLUG/branches/$BRANCH/protection" \
  --jq '"required checks: " + (.required_status_checks.contexts | join(", "))'
