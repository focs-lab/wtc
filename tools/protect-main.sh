#!/usr/bin/env bash
#
# The branch policy for main, applied from branch-protection.json beside this
# script. That file is the record: it is reviewable in a diff and it travels
# with the repository, which a setting clicked in a web interface does not.
#
# Prints what would change and stops. Pass --apply to actually change it.
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

command -v jq >/dev/null || { echo "нужен jq"; exit 1; }
[ -f "$POLICY" ] || { echo "нет файла $POLICY"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "репозиторий: $SLUG, ветка: $BRANCH"
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
  WHY=$(jq -r '.message // "нет ответа"' <<<"${body:-{\}}" 2>/dev/null || echo "нет ответа")
  case "$WHY" in
    *"not protected"*|*"Branch not protected"*) CURRENT='{}'; return 0 ;;
    *) return 1 ;;
  esac
}

if ! fetch; then
  echo "унаследованный токен не подошёл: $WHY"
  read -rsp "  токен с правами администратора (не сохраняется, не отображается): " TOK; echo
  [ -n "$TOK" ] || { echo "пустой токен"; exit 1; }
  export GH_TOKEN="$TOK"
  fetch || { echo "и этот токен не подошёл: $WHY"; exit 1; }
  echo
fi

if [ "$CURRENT" = "{}" ] || [ "$(jq -r 'has("required_pull_request_reviews")' <<<"$CURRENT")" = "false" ]; then
  echo "сейчас защиты нет"
else
  # Только те поля, которыми управляет файл политики. Остальное GitHub
  # добавляет от себя, и в разнице оно лишь мешает.
  NORM='{required_status_checks: {strict: .required_status_checks.strict,
                                  contexts: .required_status_checks.contexts},
         enforce_admins: .enforce_admins.enabled,
         required_pull_request_reviews: {
           required_approving_review_count: .required_pull_request_reviews.required_approving_review_count,
           require_code_owner_reviews: .required_pull_request_reviews.require_code_owner_reviews}}'
  jq -S "$NORM" <<<"$CURRENT" > "$TMP/current.json"
  jq -S 'del(.restrictions)' "$POLICY"  > "$TMP/wanted.json"
  if diff -u "$TMP/current.json" "$TMP/wanted.json" > "$TMP/diff"; then
    echo "политика уже совпадает с файлом, менять нечего"
    exit 0
  fi
  echo "расхождение (минус это то, что стоит сейчас):"
  sed 's/^/  /' "$TMP/diff"
fi

echo
if [ "$APPLY" != "--apply" ]; then
  echo "ничего не изменено. Чтобы применить: $0 --apply"
  exit 0
fi

gh api -X PUT "repos/$SLUG/branches/$BRANCH/protection" --input "$POLICY" >/dev/null
echo "применено"
gh api "repos/$SLUG/branches/$BRANCH" --jq '"protected: " + (.protected|tostring)'
gh api "repos/$SLUG/branches/$BRANCH/protection" \
  --jq '"обязательные проверки: " + (.required_status_checks.contexts | join(", "))'
