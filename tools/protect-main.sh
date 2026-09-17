#!/usr/bin/env bash
#
# The branch policy for main, applied from branch-protection.json beside this
# script. That file is the record: it is reviewable in a diff and it travels
# with the repository, which a setting clicked in a web interface does not.
#
# Prints what would change and stops. Pass --apply to actually change it.
#
# Needs a token with the repo scope and admin rights on the repository.
#
# On the contexts list: "Build and test" belongs there and is deliberately
# absent until it has reported green on main at least once. A required check
# that has never run leaves every pull request waiting for a status that will
# not arrive, which looks like a broken repository and is tedious to undo.
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
POLICY="tools/branch-protection.json"
SLUG=$(git remote get-url origin | sed -E 's#(git@github\.com:|https://github\.com/)([^/]+/[^/.]+)(\.git)?#\2#')
BRANCH=main
APPLY=${1:-}

command -v jq >/dev/null || { echo "нужен jq"; exit 1; }
[ -f "$POLICY" ] || { echo "нет файла $POLICY"; exit 1; }

echo "репозиторий: $SLUG, ветка: $BRANCH"
echo

# gh печатает тело ответа в стандартный вывод и при ошибке тоже, поэтому
# подстановка забирает его в любом случае, а код возврата разбирается отдельно.
# Ветка без защиты и нехватка прав дают разные сообщения и разный смысл.
if CURRENT=$(gh api "repos/$SLUG/branches/$BRANCH/protection" 2>/dev/null); then
  :
else
  MSG=$(jq -r '.message // "нет ответа"' <<<"${CURRENT:-{\}}" 2>/dev/null || echo "нет ответа")
  case "$MSG" in
    *"not protected"*|*"Branch not protected"*) CURRENT='{}' ;;
    *) echo "не удалось прочитать текущую политику: $MSG"
       echo "нужен токен с правами администратора репозитория"
       exit 1 ;;
  esac
fi

if [ "$CURRENT" = "{}" ] || [ "$(jq -r 'has("required_pull_request_reviews")' <<<"$CURRENT")" = "false" ]; then
  echo "сейчас защиты нет"
else
  # Сравниваем только те поля, которыми управляет файл политики; всё
  # остальное GitHub добавляет от себя и в diff только мешает.
  NORM='{required_status_checks: {strict: .required_status_checks.strict,
                                  contexts: .required_status_checks.contexts},
         enforce_admins: .enforce_admins.enabled,
         required_pull_request_reviews: {
           required_approving_review_count: .required_pull_request_reviews.required_approving_review_count,
           require_code_owner_reviews: .required_pull_request_reviews.require_code_owner_reviews}}'
  echo "$CURRENT" | jq -S "$NORM" > /tmp/wtc-prot-current.json
  jq -S 'del(.restrictions)' "$POLICY" > /tmp/wtc-prot-wanted.json
  if diff -u /tmp/wtc-prot-current.json /tmp/wtc-prot-wanted.json > /tmp/wtc-prot.diff; then
    echo "политика уже совпадает с файлом, менять нечего"
    [ "$APPLY" = "--apply" ] && exit 0
    exit 0
  fi
  echo "расхождение (минус это то, что стоит сейчас):"
  sed 's/^/  /' /tmp/wtc-prot.diff
fi

echo
if [ "$APPLY" != "--apply" ]; then
  echo "ничего не изменено. Чтобы применить: $0 --apply"
  exit 0
fi

gh api -X PUT "repos/$SLUG/branches/$BRANCH/protection" --input "$POLICY" >/dev/null
echo "применено"
gh api "repos/$SLUG/branches/$BRANCH" --jq '"protected: " + (.protected|tostring)'
