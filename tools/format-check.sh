#!/usr/bin/env bash
#
# format-check.sh - warn about C/C++ sources that do not follow CODE_CONVENTION.md.
#
# What this enforces is exactly what §2 of CODE_CONVENTION.md declares to be
# machine-decided: the style in modules/.clang-format (rules F1-F13, F15), plus the
# few mechanical rules clang-format cannot express (F1 leftovers, F2 tabs, F16 stray
# semicolons, H5 quoted includes). Everything else in the document -- §3 naming, §6
# class layout, §8 state, §12 the schedule, §13 the UI -- is a review rule and is
# deliberately NOT checked here.
#
# Scope is modules/plan/** and modules/v4d/**, minus modules/v4d/third/, which is
# vendored code plus three git submodules: reformatting it would rewrite ~45k lines
# of upstream sources for nothing (§2.2 of CODE_CONVENTION.md).
#
# Usage:
#   tools/format-check.sh FILE...        check the given files
#   tools/format-check.sh --staged       check what is about to be committed
#   tools/format-check.sh --all          check every source under the two modules
#   tools/format-check.sh --fix ...      rewrite the offenders in place
#
# Exit codes: 0 conforming (or nothing to check), 1 violations found, 2 bad usage.
#
# If clang-format is not installed the script says so and exits 0: a missing
# formatter must never block a commit.

set -uo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# --- the scope of the convention -------------------------------------------------
readonly SCOPE_PREFIXES=("modules/plan/" "modules/v4d/")
readonly EXCLUDED_PREFIXES=("modules/v4d/third/")
readonly SOURCE_RE='\.(c|cc|cpp|cxx|c\+\+|h|hh|hpp|hxx|h\+\+|inc|ipp|inl)$'
readonly MAX_LINES_PER_FILE=12
readonly MAX_FILES_IN_HINT=8
readonly TAB_WIDTH=8
readonly COLUMN_LIMIT=80

MODE=files
FIX=0
DIFF=0
CLANG_FORMAT_ONLY=0
QUIET=0
PRINT_OFFENDERS=0
COLOR=0
declare -a ARGS=()
declare -a DIAGNOSTICS=()
declare -a OFFENDING=()

# --- output helpers --------------------------------------------------------------

setup_colors() {
  # Colour only when writing to a terminal, so redirected output stays clean.
  [[ -t 1 ]] || return 0
  [[ -z "${NO_COLOR:-}" ]] || return 0
  COLOR=1
}

if ((COLOR)); then
  readonly C_RESET=$'\033[0m' C_BOLD=$'\033[1m' C_RED=$'\033[31m'
  readonly C_YELLOW=$'\033[33m' C_GREEN=$'\033[32m' C_DIM=$'\033[2m'
else
  readonly C_RESET='' C_BOLD='' C_RED='' C_YELLOW='' C_GREEN='' C_DIM=''
fi

say() { ((QUIET)) || printf '%s\n' "$*"; }
die() {
  printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2
  exit 2
}

usage() {
  cat <<EOF
$SCRIPT_NAME - warn about sources that do not follow CODE_CONVENTION.md

Usage:
  $SCRIPT_NAME [options] [FILE...]
  $SCRIPT_NAME --all
  $SCRIPT_NAME --staged

Options:
  -a, --all              Check every C/C++ source in modules/plan and modules/v4d
                         (modules/v4d/third/ is excluded: vendored code).
  -s, --staged           Check the files staged for commit. This is what the
                         pre-commit hook uses.
  -f, --fix              Rewrite the offending files in place: clang-format, plus
                         the repairs that are safe to make mechanically.
                         Nothing is staged; the hook re-stages for you.
  -D, --diff             Print the patch --fix would produce, changing nothing.
  -c, --clang-format-only
                         Only run clang-format; skip the checks it cannot express
                         (F1 leftovers, F2 tabs, F16, H5).
  -p, --print-offenders
                         Print nothing but the offending paths, one per line, on
                         stdout, for another script to consume. The diagnostics
                         are suppressed.
  -q, --quiet            Print only the summary.
  -h, --help             Show this help.

Without --all or --staged, FILE... is checked. Paths outside the scope of the
convention are silently ignored, so this is safe to point at any file list.

Environment:
  NO_COLOR=1             Never colourise the output.
  CLANG_FORMAT=<path>    Use this clang-format instead of the one on PATH.

Exit codes: 0 conforming, 1 violations found, 2 bad usage.
EOF
}

# --- argument parsing ------------------------------------------------------------

while (($#)); do
  case "$1" in
    -a | --all)
      MODE=all
      shift
      ;;
    -s | --staged)
      MODE=staged
      shift
      ;;
    -f | --fix)
      FIX=1
      shift
      ;;
    -D | --diff)
      DIFF=1
      shift
      ;;
    -c | --clang-format-only)
      CLANG_FORMAT_ONLY=1
      shift
      ;;
    -p | --print-offenders)
      PRINT_OFFENDERS=1
      shift
      ;;
    -q | --quiet)
      QUIET=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      ARGS+=("$@")
      break
      ;;
    -*)
      die "unknown option '$1' (try --help)"
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

case "$MODE" in
  all | staged) ((${#ARGS[@]} == 0)) || die "--$MODE takes no FILE arguments" ;;
esac

setup_colors

# --- locate ourselves in the work tree ------------------------------------------

REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)" || true
[[ -n "$REPO_ROOT" ]] || die "cannot locate the git work tree from $SCRIPT_DIR"
readonly REPO_ROOT

cd "$REPO_ROOT" || die "cannot enter $REPO_ROOT"

# --- the formatter ---------------------------------------------------------------

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  cat >&2 <<EOF
$SCRIPT_NAME: '$CLANG_FORMAT' not found - skipping the CODE_CONVENTION.md check.
              Install clang-format (>= 14) to enforce §2 automatically, or run it
              by hand:  clang-format --dry-run -Werror <file>
EOF
  exit 0
fi
readonly CLANG_FORMAT

# --- scope predicates ------------------------------------------------------------

# Reduce a path to its repo-relative form and drop "./".
relativise() {
  local path=$1
  [[ $path == /* ]] && path="${path#"$REPO_ROOT"/}"
  while [[ $path == ./* ]]; do path="${path#./}"; done
  printf '%s\n' "$path"
}

# True when CODE_CONVENTION.md applies to $1 (a path, absolute or repo-relative).
in_scope() {
  local path prefix
  path="$(relativise "$1")"
  for prefix in "${EXCLUDED_PREFIXES[@]}"; do
    [[ $path == "$prefix"* ]] && return 1
  done
  for prefix in "${SCOPE_PREFIXES[@]}"; do
    [[ $path == "$prefix"* ]] && return 0
  done
  return 1
}

# True when $1 looks like a C or C++ translation unit or header.
is_source() { [[ $1 =~ $SOURCE_RE ]]; }

# --- candidate collection --------------------------------------------------------

declare -a FILES=()

collect_files() {
  local line arg
  case "$MODE" in
    all)
      # Tracked sources plus untracked ones that are not ignored, so a file that
      # has not been added yet is still audited.
      while IFS= read -r -d '' line; do
        FILES+=("$(relativise "$line")")
      done < <(git ls-files -z --exclude-standard -co -- "${SCOPE_PREFIXES[@]}")
      ;;
    staged)
      # ACMR: added, copied, modified, renamed. A deletion has nothing to format.
      while IFS= read -r -d '' line; do
        FILES+=("$(relativise "$line")")
      done < <(git diff --cached --name-only --diff-filter=ACMR -z)
      ;;
    files)
      for arg in "${ARGS[@]}"; do FILES+=("$(relativise "$arg")"); done
      ;;
  esac

  # Keep what is in scope, is a source, and still exists on disk.
  local -a kept=()
  local path
  for path in "${FILES[@]}"; do
    in_scope "$path" || continue
    is_source "$path" || continue
    [[ -f $path ]] || continue
    kept+=("$path")
  done
  FILES=("${kept[@]}")
}

# --- diagnostics -----------------------------------------------------------------

add_diagnostic() { DIAGNOSTICS+=("$1:$2:$3: [$4] $5"); }

mark_offending() {
  local f=$1 i
  for i in "${OFFENDING[@]:-}"; do [[ -n $i && $i == "$f" ]] && return 0; done
  OFFENDING+=("$f")
}

# --- the check: clang-format -----------------------------------------------------
#
# Reports one diagnostic per offending line. clang-format emits one per edited
# token, which would otherwise bury the report in noise.
#
# Returns 0 when the file conforms, 1 when it does not.

check_clang_format() {
  local file=$1 raw hits lines last
  raw="$("$CLANG_FORMAT" --dry-run -Werror -- "$file" 2>&1 >/dev/null)"
  [[ -n $raw ]] || return 0

  hits="$(printf '%s\n' "$raw" |
    sed -nE 's/^[^:]+:([0-9]+):([0-9]+): (error|warning): code should be.*/\1/p' |
    awk '!seen[$0]++')"
  [[ -n $hits ]] || return 0

  mark_offending "$file"
  lines="$(printf '%s\n' "$hits" | wc -l | tr -d '[:space:]')"
  local shown=$lines
  ((shown > MAX_LINES_PER_FILE)) && shown=$MAX_LINES_PER_FILE
  # A here-string, not a pipe: a pipeline would put the loop in a subshell and the
  # diagnostics it appends would be lost.
  while read -r line; do
    [[ -n $line ]] || continue
    add_diagnostic "$file" "$line" 1 'F' \
      "not clang-formatted (CODE_CONVENTION.md §2, modules/.clang-format)"
  done <<<"$(printf '%s\n' "$hits" | head -n "$shown")"
  if ((lines > shown)); then
    last="$(printf '%s\n' "$hits" | tail -1)"
    add_diagnostic "$file" "$last" 1 'F' \
      "... $lines offending lines in total, $shown shown; '$0 --fix $file' rewrites them all"
  fi
  return 1
}

# --- the checks clang-format cannot express --------------------------------------
#
# awk rather than grep, so that the reported column honours tab stops (TabWidth: 8)
# and every rule can point at a column. Text is assumed to be ASCII/UTF-8 without
# wide characters; clang-format owns the authoritative F1 check anyway.
#
# Returns 0 when the file conforms, 1 when it does not.

readonly EXTRA_CHECKS_AWK='
  BEGIN { limit = COLUMN_LIMIT; tab = TAB_WIDTH }
  {
    line = $0

    tabcol = index(line, "\t")
    if (tabcol > 0)
      printf("%s|%d|%d|F2|tab character; F2 is spaces only\n", FILENAME, FNR, tabcol)

    width = 0
    overcol = 0
    for (i = 1; i <= length(line); i++) {
      c = substr(line, i, 1)
      if (c == "\t")
        width = int(width / tab) * tab + tab
      else
        width++
      if (width > limit && overcol == 0) overcol = i
    }
    if (overcol > 0)
      printf("%s|%d|%d|F1|%d columns wide, limit is %d; clang-format cannot shorten this (long literal, URL or macro)\n",
             FILENAME, FNR, overcol, width, limit)

    if (line ~ /^[ \t]*#[ \t]*include[ \t]*"/)
      printf("%s|%d|1|H5|quoted include; H5 wants angle brackets, absolute from the include root\n",
             FILENAME, FNR)

    if (line ~ /^[ \t]*;[ \t]*$/)
      printf("%s|%d|1|F16|stray semicolon on a line of its own\n", FILENAME, FNR)
    else if (line ~ /^[ \t]*(public|private|protected)[ \t]*:[ \t]*;/)
      printf("%s|%d|1|F16|semicolon after an access specifier\n", FILENAME, FNR)
  }
'

check_extra() {
  local file=$1 out
  out="$(awk -v COLUMN_LIMIT="$COLUMN_LIMIT" -v TAB_WIDTH="$TAB_WIDTH" \
    "$EXTRA_CHECKS_AWK" "$file")"
  [[ -n $out ]] || return 0

  mark_offending "$file"
  local line col rule msg
  while IFS='|' read -r _f line col rule msg; do
    [[ -n ${rule:-} ]] || continue
    add_diagnostic "$file" "$line" "$col" "$rule" "$msg"
  done <<<"$out"
  return 1
}

# --- one file, both checks --------------------------------------------------------

# Returns 0 when the file conforms.
#
# The extra checks only run on a file that clang-format already accepts: otherwise
# every line it is about to rewrap would be listed twice, once as "not formatted"
# and once as "over 80 columns".

check_file() {
  local file=$1
  if ! check_clang_format "$file"; then return 1; fi
  ((CLANG_FORMAT_ONLY)) && return 0
  # check_extra returns 1 when it found something, which is what we want here.
  check_extra "$file"
}

# --- the fixes -------------------------------------------------------------------
#
# Only repairs that cannot change the meaning of a program. Everything else is
# reported and left to a human.
#
# The sed passes run first and clang-format last, on purpose. Dropping a stray ';'
# can leave a body that now fits on one line, which clang-format then wants to join
# (AllowShortFunctionsOnASingleLine: All), so formatting last is what makes --fix
# converge: afterwards 'clang-format --dry-run -Werror' is clean.

fix_file() {
  local file=$1 tmp
  ((CLANG_FORMAT_ONLY)) || {
    tmp="$file.format-check.$$.tmp"
    # F16: drop a semicolon that sits on a line of its own ...
    sed -e '/^[[:space:]]*;[[:space:]]*$/d' "$file" >"$tmp" && mv "$tmp" "$file"
    # ... and one that trails an access specifier.
    sed -E -e 's/^([[:space:]]*(public|private|protected)[[:space:]]*:[[:space:]]*);.*/\1/' \
      "$file" >"$tmp" && mv "$tmp" "$file"
    # H5: our own headers are included with angle brackets, not quotes.
    sed -E -e 's@^([[:space:]]*#[[:space:]]*include[[:space:]]*)"(opencv2/[^"]+)"@\1<\2>@' \
      "$file" >"$tmp" && mv "$tmp" "$file"
    rm -f "$tmp"
  }
  "$CLANG_FORMAT" -i -- "$file"
}

# --- report ----------------------------------------------------------------------

print_diagnostics() {
  local i
  for ((i = 0; i < $#; i++)); do
    printf '  %s%s%s\n' "$C_RED" "${DIAGNOSTICS[i]}" "$C_RESET"
  done
}

# How many replacements clang-format would make in one file. Used by the hook's
# "show me what it wants to change" option.
format_replacement_count() {
  local file=$1
  "$CLANG_FORMAT" --output-replacements-xml -- "$file" 2>/dev/null |
    grep -c '<replacement ' || true
}

# The patch --fix would produce for one file, and nothing is written to the file.
# The scratch copy lives next to the original so that clang-format resolves the
# same modules/.clang-format it would use on the real thing.
format_diff() {
  local file=$1 scratch
  scratch="$(dirname "$file")/.$(basename "$file").format-check-diff.$$.tmp"
  if ! cp -- "$file" "$scratch"; then
    printf 'cannot create a scratch copy of %s\n' "$file" >&2
    return 1
  fi
  fix_file "$scratch" >/dev/null
  diff -u --label "a/$file" --label "b/$file" "$file" "$scratch"
  local status=$?
  rm -f "$scratch"
  return $status
}

# --- main ------------------------------------------------------------------------

collect_files

if ((${#FILES[@]} == 0)); then
  say "${C_DIM}$SCRIPT_NAME: no source files in scope - nothing to check.${C_RESET}"
  exit 0
fi

if ((DIFF)); then
  patches=0
  for file in "${FILES[@]}"; do
    DIAGNOSTICS=()
    OFFENDING=()
    # check_file returns 0 when the file conforms, i.e. when there is no patch.
    check_file "$file" && continue
    format_diff "$file"
    patches=$((patches + 1))
  done
  ((patches)) || say "${C_DIM}$SCRIPT_NAME: nothing to change.${C_RESET}"
  # Same signal as a plain check: something is off convention if a patch exists.
  ((patches)) && exit 1
  exit 0
fi

if ((FIX)); then
  declare -a fixed=()
  for file in "${FILES[@]}"; do
    DIAGNOSTICS=()
    OFFENDING=()
    if ! check_file "$file"; then
      say "${C_BOLD}fixing${C_RESET} $file"
      if fix_file "$file"; then fixed+=("$file"); fi
    fi
  done
  if ((${#fixed[@]})); then
    say "${C_BOLD}repaired${C_RESET} ${#fixed[@]} file(s):"
    printf '  %s\n' "${fixed[@]}"
  fi
  say ""
fi

DIAGNOSTICS=()
OFFENDING=()
for file in "${FILES[@]}"; do check_file "$file"; done

checked=${#FILES[@]}
offending=${#OFFENDING[@]}

if ((PRINT_OFFENDERS)); then
  # Machine-readable: the offending paths and nothing else, on stdout.
  printf '%s\n' "${OFFENDING[@]:-}" | grep -v '^$'
  ((offending == 0)) && exit 0
  exit 1
fi

if ((offending == 0)); then
  ((QUIET)) ||
    printf '%s%s%s: %d file(s) checked, all conforming.\n' \
      "$C_GREEN" "$SCRIPT_NAME" "$C_RESET" "$checked"
  exit 0
fi

{
  printf '%s%s%s: %d of %d file(s) do not follow CODE_CONVENTION.md:\n' \
    "$C_BOLD" "$SCRIPT_NAME" "$C_RESET" "$offending" "$checked"
  for file in "${OFFENDING[@]}"; do
    count="$(format_replacement_count "$file")"
    if [[ $count =~ ^[1-9][0-9]*$ ]]; then
      printf '  %s%s%s  (%s clang-format replacement(s))\n' \
        "$C_YELLOW" "$file" "$C_RESET" "$count"
    else
      printf '  %s%s%s\n' "$C_YELLOW" "$file" "$C_RESET"
    fi
  done
  print_diagnostics "${DIAGNOSTICS[@]}"
  cat <<EOF

Repair what can be repaired mechanically with:
EOF
  if ((offending <= MAX_FILES_IN_HINT)); then
    printf '  %s --fix %s\n' "$0" "${OFFENDING[*]}"
  else
    printf '  %s --fix --all\n' "$0"
  fi
  cat <<EOF

[F1] over-long lines that clang-format cannot wrap, [F2] tabs inside comments and
literals, [F16] stray semicolons and [H5] quoted includes are the rules it cannot
express; --fix repairs the last two, the first two are yours to finish by hand.
Naming, class layout, state and schedule rules (§3, §6, §8, §12) are review rules -
this script cannot see them. See CODE_CONVENTION.md §17.
EOF
} >&2

exit 1
