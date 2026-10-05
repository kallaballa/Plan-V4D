#!/usr/bin/env bash
# Print just the compiler diagnostics from a ninja build log.
#
#   tools/android/log-errors.sh /tmp/build.log          # all of them
#   tools/android/log-errors.sh /tmp/build.log 40       # at most 40
#
# Why this exists: a failing NDK compile line is ~2 KB of flags, so `grep error:`
# on the log buries the diagnostic under the command that produced it and loses
# which target failed. This keeps the FAILED header and the diagnostic that
# follows it, and drops the command line and the caret/line-number art.
set -uo pipefail

LOG="${1:?usage: log-errors.sh <build log> [max errors]}"
MAX="${2:-0}"

awk -v max="$MAX" '
  BEGIN { printing = 0; n = 0 }
  # Everything before the first failure is configure output and progress lines.
  /^FAILED: / {
    printing = 1
    target = $0
    sub(/^FAILED: \[[^]]*\] /, "", target)
    sub(/^.*CMakeFiles\//, "", target)
    if (target != last) { printf "\n### %s\n", target; last = target }
    next
  }
  !printing { next }
  max > 0 && n >= max { printing = 0; next }
  # The command line that failed, and clang/gcc chrome around the diagnostic.
  /^(\/usr\/bin\/ccache |.*\/-o \/)/ { next }
  /^In file included from / { next }
  /^ *[0-9]+ \|/ { next }
  /^ *\| *(\^|$)/ { next }
  /^[0-9]+ (errors?|warnings?) generated/ { next }
  /^ninja: / { next }
  /^$/ { next }
  { print; if ($0 ~ /error:/) n++ }
' "$LOG"
