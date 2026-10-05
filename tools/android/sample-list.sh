#!/usr/bin/env bash
# Print the V4D samples that have an Android build, as a comma separated list --
# the form OPENCV_V4D_SAMPLES and `build.sh --demo` both take.
#
#   tools/android/sample-list.sh              # one line: a,b,c
#   tools/android/sample-list.sh --lines      # one per line
#   tools/android/sample-list.sh --has x      # exit 0 if x is in the list
#
# The list is derived from modules/v4d/CMakeLists.txt rather than repeated here,
# because that file is where a sample is registered: add_binary_sample()
# there and this script follows. The exceptions are the two bgfx demos, which are
# registered inside `if(OPENCV_V4D_ENABLE_BGFX)` and so exist only where
# third/bgfx.cmake is populated -- it is an empty directory in this tree, on
# every platform, so there is no Android path for them to have.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CMAKE_LISTS="$REPO_DIR/modules/v4d/CMakeLists.txt"

[ -f "$CMAKE_LISTS" ] || { echo "sample-list.sh: $CMAKE_LISTS not found" >&2; exit 1; }

# add_binary_sample(example_v4d_<name> samples/<name>.cpp [extra modules])
EXCLUDED=" bgfx-demo bgfx-demo2 "

samples=()
while read -r name; do
  [ -n "$name" ] || continue
  case "$EXCLUDED" in
    *" $name "*) continue ;;
  esac
  samples+=("$name")
done < <(sed -n 's/^[[:space:]]*add_binary_sample(example_v4d_\([^ )]*\).*/\1/p' "$CMAKE_LISTS")

[ "${#samples[@]}" -gt 0 ] || { echo "sample-list.sh: no samples found in $CMAKE_LISTS" >&2; exit 1; }

MODE="${1:-csv}"
case "$MODE" in
  --lines) printf '%s\n' "${samples[@]}" ;;
  --has)   sample="${2:-}"; for s in "${samples[@]}"; do [ "$s" = "$sample" ] && exit 0; done; exit 1 ;;
  csv|"")  printf '%s\n' "$(IFS=,; echo "${samples[*]}")" ;;
  *) echo "sample-list.sh: unknown mode '$MODE'" >&2; exit 1 ;;
esac