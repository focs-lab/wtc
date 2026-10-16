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
die()  { printf '\n\033[31mSTOP:\033[0m %s\n' "$*"; exit 1; }

cd "$(git rev-parse --show-toplevel)"
SLUG=$(git remote get-url origin | sed -E 's#(git@github\.com:|https://github\.com/)([^/]+/[^/.]+)(\.git)?#\2#')
ORG=${SLUG%%/*}
BRANCH=$(git rev-parse --abbrev-ref HEAD)
REV=$(git rev-parse HEAD:llvm)
IMG="ghcr.io/$ORG/wtc-toolchain:llvm-${REV:0:12}"

step "Token"
read -rsp "  classic token (repo + write:packages, not stored): " TOK; echo
[ -n "$TOK" ] || die "empty token"
export GH_TOKEN="$TOK"
unset GITHUB_TOKEN 2>/dev/null || true
# credential.helper is multi-valued, and configuration from the environment
# ADDS to the list. An empty value first clears everything inherited;
# otherwise some other helper answers before ours and writes the token to
# disk.
export GIT_CONFIG_COUNT=2
export GIT_CONFIG_KEY_0=credential.helper
export GIT_CONFIG_VALUE_0=
export GIT_CONFIG_KEY_1=credential.helper
export GIT_CONFIG_VALUE_1='!f() { echo username=x-access-token; echo "password=$GH_TOKEN"; }; f'
WHO=$(gh api user --jq .login) || die "GitHub rejected the token"
SCOPES=$(gh api -i user 2>/dev/null | sed -n 's/^[Xx]-[Oo][Aa]uth-[Ss]copes: //p' | tr -d '\r')
[ -n "$SCOPES" ] || die "this is a fine-grained token; a classic one is needed: https://github.com/settings/tokens"
SC=",${SCOPES// /},"
case "$SC" in *,repo,*) ;; *) die "the repo scope is missing" ;; esac
case "$SC" in *,write:packages,*) ;; *) die "the write:packages scope is missing; the image cannot be pushed" ;; esac
ok "$WHO, scopes: $SCOPES"

step "0. Checks"
[ -z "$(git status --porcelain)" ] || die "the working tree is not clean"
docker image inspect "$IMG" >/dev/null 2>&1 || die "image $IMG is not built; see docs/toolchain.md"
# Without the source label the package is tied to no repository, and the
# job token cannot read it. An image built by the old Dockerfile looks whole
# and silently yields a CI that does not work, so this is checked here, not
# after the push.
SRC=$(docker image inspect "$IMG" --format '{{index .Config.Labels "org.opencontainers.image.source"}}')
[ "$SRC" = "https://github.com/$SLUG" ] \
  || die "image lacks the label source=https://github.com/$SLUG (got: '${SRC:-none}'); rebuild it: docs/toolchain.md"
ok "$SLUG, branch $BRANCH, pin ${REV:0:12}, source label present"

step "1. Image"
# Always pushed. An "already there" check by tag name would not work before
# logging in to the registry, and after logging in it would be harmful: the
# tag is derived from the pin, so an image rebuilt on the same pin with other
# flags would never be pushed at all.
trap 'docker logout ghcr.io >/dev/null 2>&1 || true' EXIT
echo "$TOK" | docker login ghcr.io -u "$WHO" --password-stdin >/dev/null
docker push "$IMG"
docker logout ghcr.io >/dev/null 2>&1 || true
trap - EXIT
ok "$IMG pushed, docker credentials removed"

step "2. Repository variable"
gh variable set WTC_TOOLCHAIN_IMAGE -R "$SLUG" -b "ghcr.io/$ORG/wtc-toolchain"
ok "set, without a tag: CI derives the tag from the pin itself"

# The image and the variable go FIRST, the branch after. The other order
# once cost a wasted run: pushing the branch starts CI at once, and it failed
# on the empty variable the script set a second later.
step "3. Branch"
git push --set-upstream origin "$BRANCH"
ok "pushed"

step "4. Pull request"
if gh pr view "$BRANCH" --json url --jq .url >/dev/null 2>&1; then
  skip "already open: $(gh pr view "$BRANCH" --json url --jq .url)"
else
  gh pr create --base main --head "$BRANCH" --fill
  ok "opened: $(gh pr view "$BRANCH" --json url --jq .url)"
fi

step "Left to do by hand"
cat <<NOTE
  Once per package: grant this repository read access. A package pushed
  with a personal token is tied to no repository, and the job token is
  refused whatever the workflow's permissions say.

    https://github.com/orgs/$ORG/packages/container/wtc-toolchain/settings
    Manage Actions access -> Add repository -> ${SLUG#*/} -> Role: Read

  The package cannot be made public: the organization's administrators have
  disabled that visibility. Nor is it needed; CI logs in to the registry
  with its own token.

  After granting access, rerun the last run:

    gh run rerun "\$(gh run list -R $SLUG -L 1 --json databaseId --jq '.[0].databaseId')" -R $SLUG

  When "Build and test" first passes, add it to
  tools/branch-protection.json and apply: tools/protect-main.sh --apply

  When the llvm pin moves, delete the old image version on the same settings
  page: private versions count against the organization's quota, about
  377 MiB each.
NOTE
