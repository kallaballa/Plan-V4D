#!/usr/bin/env bash
# Build (and run) the Shadertoy editor sample without the full OpenCV rebuild
# that ./build.sh does, and without the `sudo make install` at its end.
#
# ./build.sh is the supported entry point and this script does not replace it:
# it drives the same build tree with the same configuration, only narrowed to
# the one target, so an edit/rebuild cycle takes seconds instead of the minutes a
# `make` over the whole of OpenCV takes.
#
#   ./shadertoy-editor-build.sh              build example_v4d_shadertoy-editor
#   ./shadertoy-editor-build.sh run [args]   build, then run it
#   ./shadertoy-editor-build.sh test [dir]   build, then run the smoke test
#   ./shadertoy-editor-build.sh clean        drop the target's stale objects
#
# Environment:
#   JOBS     parallel compile jobs (default: nproc, capped at 16)
#   BUILD    the build tree (default: <repo>/opencv/build)
#   TARGET   the make target (default: example_v4d_shadertoy-editor)

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../../.." && pwd)"
build="${BUILD:-$repo/opencv/build}"
target="${TARGET:-example_v4d_shadertoy-editor}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
[ "$jobs" -gt 16 ] && jobs=16

binary="$build/bin/$target"
action="${1:-build}"

if [ ! -f "$build/CMakeCache.txt" ]; then
  cat >&2 <<EOF
$build is not a configured build tree. Configure one first:

  ./build.sh plan+v4d -b release
EOF
  exit 1
fi

# A sample that was renamed, deleted or had a file removed keeps its objects and
# its binary in the tree and `make` never notices, so a link can silently pick up
# a file that is no longer in the source list. Same reasoning as the repo's
# prune-stale-build-artifacts.sh, narrowed to the one target.
if [ "$action" = clean ]; then
  echo "==> dropping the build artifacts of $target"
  rm -f "$binary"
  find "$build" -type d -name "$target.dir" -prune -exec rm -rf {} + 2>/dev/null || true
  exit 0
fi

echo "==> $target (-j$jobs) in $build"
make -C "$build" -j"$jobs" "$target"

case "$action" in
  build) ;;
  run)
    shift
    # The build tree's own libraries come first: an installed libopencv_v4d of a
    # different vintage would be picked up otherwise.
    export LD_LIBRARY_PATH="$build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    exec "$binary" "$@"
    ;;
  test)
    exec "$here/shadertoy-editor-test.sh" "${2:-${TMPDIR:-/tmp}/shadertoy-editor-test}"
    ;;
  *)
    echo "unknown action '$action' (build, run, test, clean)" >&2
    exit 1
    ;;
esac
