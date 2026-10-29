# Sourced, not run: names the source a leg ran or a build was made from.
#
#   . benchmarks/scripts/source_rev.sh
#   source_rev <root> || exit 2
#
# sets SOURCE_SHA (the commit), SOURCE_DIRTY (true, false, or unknown when
# no one can say) and SOURCE_LLVM_SHA (the pinned ClangIR commit, or
# unknown). Dirty means the source differs from the commit; the results
# of earlier legs in a chain are output, not source, and do not count.
#
# A leg run from a copy of the tree -- synced to another host without its
# .git, as every leg of 26 to 28 Oct was -- cannot ask git, and recorded
# an empty commit and a clean tree, the second of which was not known.
# The commit of such a copy comes from whoever made it:
#
#   WTC_GIT_SHA=<commit>        the commit the copy was made from
#   WTC_GIT_DIRTY=true|false    whether the copy had changes on top of it
#   WTC_CLANGIR_REV=<commit>    the llvm submodule's commit
#
# Without WTC_GIT_SHA outside a checkout, source_rev fails and says so: a
# result whose source cannot be named cannot be reproduced. To run anyway,
# WTC_GIT_SHA=unknown records "unknown" and warns. Inside a checkout git
# is asked, and a WTC_GIT_SHA that names another commit is an error, since
# one of the two is wrong.
source_rev() {
  local root=$1 top="" git_in=(git -c safe.directory="$1" -C "$1")
  SOURCE_SHA=""; SOURCE_DIRTY=unknown; SOURCE_LLVM_SHA=${WTC_CLANGIR_REV:-}
  case "${WTC_GIT_DIRTY:-}" in
    ""|true|false) ;;
    *) echo "source_rev: WTC_GIT_DIRTY must be true or false, not '$WTC_GIT_DIRTY'" >&2; return 1 ;;
  esac
  case "${WTC_GIT_SHA:-}" in
    ""|unknown) ;;
    *) if ! [[ $WTC_GIT_SHA =~ ^[0-9a-f]{7,40}$ ]]; then
         echo "source_rev: WTC_GIT_SHA must be a commit in hex, not '$WTC_GIT_SHA'" >&2; return 1
       fi ;;
  esac
  # A checkout of this tree itself, not a copy that happens to sit inside
  # some other repository.
  top=$("${git_in[@]}" rev-parse --show-toplevel 2>/dev/null || true)
  if [ -n "$top" ] && [ "$(cd "$top" && pwd -P)" = "$(cd "$root" && pwd -P)" ]; then
    # The callers run this under `|| exit 2`, which turns set -e off here:
    # every git call is checked by hand, and a failure is never read as
    # an empty commit or a clean tree.
    SOURCE_SHA=$("${git_in[@]}" rev-parse HEAD) && [ -n "$SOURCE_SHA" ] ||
      { echo "source_rev: git rev-parse HEAD failed in $root" >&2; return 1; }
    if [ -n "${WTC_GIT_SHA:-}" ] && [ "$WTC_GIT_SHA" != unknown ] && [[ $SOURCE_SHA != "$WTC_GIT_SHA"* ]]; then
      echo "source_rev: WTC_GIT_SHA=$WTC_GIT_SHA, but $root is a checkout of $SOURCE_SHA" >&2; return 1
    fi
    local changes
    if ! changes=$("${git_in[@]}" status --porcelain -- . ':(exclude)benchmarks/results'); then
      echo "source_rev: warning: git status failed in $root; dirty recorded as unknown" >&2
      SOURCE_DIRTY=unknown
    elif [ -z "$changes" ]; then
      SOURCE_DIRTY=false
    else
      SOURCE_DIRTY=true
    fi
    [ -n "$SOURCE_LLVM_SHA" ] ||
      SOURCE_LLVM_SHA=$("${git_in[@]}" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-')
  elif [ -n "${WTC_GIT_SHA:-}" ]; then
    SOURCE_SHA=$WTC_GIT_SHA
    SOURCE_DIRTY=${WTC_GIT_DIRTY:-unknown}
    if [ "$SOURCE_SHA" = unknown ]; then
      echo "source_rev: warning: $root is not a git checkout and WTC_GIT_SHA=unknown;" \
           "the results will not name their source" >&2
    elif [ "$SOURCE_DIRTY" = unknown ]; then
      echo "source_rev: warning: WTC_GIT_DIRTY not given; whether the copy of $SOURCE_SHA had" \
           "changes is recorded as unknown" >&2
    fi
  else
    echo "source_rev: $root is not a git checkout, so its commit is not known." >&2
    echo "source_rev: pass the commit the copy was made from: WTC_GIT_SHA=<commit>" \
         "WTC_GIT_DIRTY=true|false (and WTC_CLANGIR_REV=<commit>), or WTC_GIT_SHA=unknown" >&2
    return 1
  fi
  SOURCE_LLVM_SHA=${SOURCE_LLVM_SHA:-unknown}
}
