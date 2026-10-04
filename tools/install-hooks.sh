#!/usr/bin/env bash
#
# install-hooks.sh - make git run the versioned pre-commit hook in tools/hooks.
#
# The hook is kept in the work tree rather than copied into .git/hooks, so that it
# is versioned, reviewable and installed identically for everyone.
#
# Usage:
#   tools/install-hooks.sh            install
#   tools/install-hooks.sh --uninstall
#   tools/install-hooks.sh --status

set -uo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)" || true
readonly HOOKS_DIR="$SCRIPT_DIR/hooks"

if [[ -z ${REPO_ROOT:-} ]]; then
  printf '%s: not inside a git work tree\n' "$SCRIPT_NAME" >&2
  exit 2
fi

[[ -f $HOOKS_DIR/pre-commit ]] || {
  printf '%s: %s/pre-commit is missing\n' "$SCRIPT_NAME" "$HOOKS_DIR" >&2
  exit 2
}

# The hook needs to be executable, and git refuses to run a hook that is not.
chmod +x "$HOOKS_DIR/pre-commit" || exit 2

current="$(git -C "$REPO_ROOT" config --get core.hooksPath || true)"
wanted="$HOOKS_DIR"

status() {
  if [[ $current == "$wanted" ]]; then
    printf '%s: installed, core.hooksPath = %s\n' "$SCRIPT_NAME" "$current"
  elif [[ -n $current ]]; then
    printf '%s: core.hooksPath is %s, so the convention hook is NOT installed\n' \
      "$SCRIPT_NAME" "$current"
  elif [[ -x "$REPO_ROOT/.git/hooks/pre-commit" ]]; then
    printf '%s: not installed; an untracked .git/hooks/pre-commit exists and shadows nothing\n' \
      "$SCRIPT_NAME"
  else
    printf '%s: not installed\n' "$SCRIPT_NAME"
  fi
}

case "${1:---install}" in
  --install)
    # An absolute path, so the hook is found no matter where git is invoked from.
    git -C "$REPO_ROOT" config core.hooksPath "$wanted" || exit 2
    printf '%s\n' "$SCRIPT_NAME: core.hooksPath = $wanted"
    cat <<EOF

Every 'git commit' in this work tree now checks the staged sources against
CODE_CONVENTION.md §2 and offers to fix them.

  check only   tools/format-check.sh --staged
  audit all    tools/format-check.sh --all
  bypass once  SKIP_FORMAT_CHECK=1 git commit ...
  uninstall    $SCRIPT_NAME --uninstall
EOF
    ;;
  --uninstall)
    git -C "$REPO_ROOT" config --unset core.hooksPath || exit 2
    printf '%s: core.hooksPath removed\n' "$SCRIPT_NAME"
    ;;
  --status)
    status
    ;;
  -h | --help)
    sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    ;;
  *)
    printf '%s: unknown option %s\n' "$SCRIPT_NAME" "$1" >&2
    printf 'Usage: %s [--install|--uninstall|--status]\n' "$SCRIPT_NAME" >&2
    exit 2
    ;;
esac
