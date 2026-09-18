#!/usr/bin/env bash
#
# Publishes the toolchain image for the currently pinned llvm revision, then
# points CI at it. Run from anywhere inside the repository.
#
# Everything is derived: the repository from origin, the branch from HEAD, the
# image tag from the submodule pin. Nothing is hardcoded to one machine.
#
# Needs a classic token with the repo and write:packages scopes. Fine-grained
# tokens cannot push packages.
#
# Expects the image to be built already; see docs/toolchain.md.
#
# Do the one-time package grant BEFORE the first run of this script, not
# after. The last step pushes the branch, which starts a CI run at once, and
# that run cannot pull the image until the grant exists. The package settings
# page is reachable as soon as the package exists, so the grant does not have
# to wait for anything here. The note this script prints at the end repeats
# the link, for the case where you are reading it too late.
#
set -euo pipefail

ok()   { printf '  \033[32m+\033[0m %s\n' "$*"; }
skip() { printf '  \033[90m=\033[0m %s\n' "$*"; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die()  { printf '\n\033[31mСТОП:\033[0m %s\n' "$*"; exit 1; }

cd "$(git rev-parse --show-toplevel)"
SLUG=$(git remote get-url origin | sed -E 's#(git@github\.com:|https://github\.com/)([^/]+/[^/.]+)(\.git)?#\2#')
ORG=${SLUG%%/*}
BRANCH=$(git rev-parse --abbrev-ref HEAD)
REV=$(git rev-parse HEAD:llvm)
IMG="ghcr.io/$ORG/wtc-toolchain:llvm-${REV:0:12}"

step "Токен"
read -rsp "  classic-токен (repo + write:packages, не сохраняется): " TOK; echo
[ -n "$TOK" ] || die "пустой токен"
export GH_TOKEN="$TOK"
unset GITHUB_TOKEN 2>/dev/null || true
# credential.helper многозначен, и конфиг из окружения ДОПОЛНЯЕТ список.
# Пустое значение первым сбрасывает всё унаследованное, иначе чужой помощник
# ответит раньше нашего и запишет токен на диск.
export GIT_CONFIG_COUNT=2
export GIT_CONFIG_KEY_0=credential.helper
export GIT_CONFIG_VALUE_0=
export GIT_CONFIG_KEY_1=credential.helper
export GIT_CONFIG_VALUE_1='!f() { echo username=x-access-token; echo "password=$GH_TOKEN"; }; f'
WHO=$(gh api user --jq .login) || die "токен не принят GitHub"
SCOPES=$(gh api -i user 2>/dev/null | sed -n 's/^[Xx]-[Oo][Aa]uth-[Ss]copes: //p' | tr -d '\r')
[ -n "$SCOPES" ] || die "это fine-grained токен, нужен classic: https://github.com/settings/tokens"
SC=",${SCOPES// /},"
case "$SC" in *,repo,*) ;; *) die "нет области repo" ;; esac
case "$SC" in *,write:packages,*) ;; *) die "нет области write:packages, образ не отправить" ;; esac
ok "$WHO, области: $SCOPES"

step "0. Проверка"
[ -z "$(git status --porcelain)" ] || die "рабочее дерево не чистое"
docker image inspect "$IMG" >/dev/null 2>&1 || die "образ $IMG не собран, см. docs/toolchain.md"
# Без метки source пакет не привязан к репозиторию, и токен задачи его не
# прочитает. Образ, собранный старым Dockerfile, выглядит целым и молча даёт
# неработающий CI, поэтому проверяем здесь, а не после отправки.
SRC=$(docker image inspect "$IMG" --format '{{index .Config.Labels "org.opencontainers.image.source"}}')
[ "$SRC" = "https://github.com/$SLUG" ] \
  || die "образ без метки source=https://github.com/$SLUG (получено: '${SRC:-нет}'), пересоберите: docs/toolchain.md"
ok "$SLUG, ветка $BRANCH, пин ${REV:0:12}, метка связи на месте"

step "1. Образ"
# Отправляем всегда. Проверка "уже есть" по имени тега не работала бы до входа
# в реестр, а после входа была бы вредна: тег выводится из пина, и образ,
# пересобранный на том же пине с другими флагами, не отправился бы вовсе.
trap 'docker logout ghcr.io >/dev/null 2>&1 || true' EXIT
echo "$TOK" | docker login ghcr.io -u "$WHO" --password-stdin >/dev/null
docker push "$IMG"
docker logout ghcr.io >/dev/null 2>&1 || true
trap - EXIT
ok "$IMG отправлен, учётные данные docker убраны"

step "2. Переменная репозитория"
gh variable set WTC_TOOLCHAIN_IMAGE -R "$SLUG" -b "ghcr.io/$ORG/wtc-toolchain"
ok "выставлена, без тега: тег CI выводит из пина сам"

# Образ и переменная идут ПЕРВЫМИ, ветка после. Обратный порядок один раз уже
# обошёлся зря потраченным прогоном: отправка ветки запускает CI немедленно, и
# он падал на пустой переменной, которую скрипт выставлял секундой позже.
step "3. Ветка"
git push --set-upstream origin "$BRANCH"
ok "опубликована"

step "4. Pull request"
if gh pr view "$BRANCH" --json url --jq .url >/dev/null 2>&1; then
  skip "уже открыт: $(gh pr view "$BRANCH" --json url --jq .url)"
else
  gh pr create --base main --head "$BRANCH" --fill
  ok "открыт: $(gh pr view "$BRANCH" --json url --jq .url)"
fi

step "Осталось руками"
cat <<NOTE
  Один раз на пакет: выдать этому репозиторию доступ на чтение. Пакет,
  отправленный личным токеном, не привязан ни к одному репозиторию, и токен
  задачи получает отказ независимо от прав в рабочем процессе.

    https://github.com/orgs/$ORG/packages/container/wtc-toolchain/settings
    Manage Actions access -> Add repository -> ${SLUG#*/} -> Role: Read

  Публичным пакет сделать нельзя: в организации эта видимость отключена
  администраторами. Это и не нужно, CI входит в реестр своим токеном.

  После выдачи доступа перезапустить последний прогон:

    gh run rerun "\$(gh run list -R $SLUG -L 1 --json databaseId --jq '.[0].databaseId')" -R $SLUG

  Когда "Build and test" впервые пройдёт зелёной, добавьте её в
  tools/branch-protection.json и примените: tools/protect-main.sh --apply

  Когда пин llvm сдвинется, удалите старую версию образа на той же странице
  настроек: приватные версии занимают квоту организации, ~377 МиБ каждая.
NOTE
