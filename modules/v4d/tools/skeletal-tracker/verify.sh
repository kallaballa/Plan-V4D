#!/usr/bin/env bash
# Builds and runs every check the skeletal-tracker demo has, in the order that
# fails fastest. This is the whole automated coverage of the sample: run it
# after touching the pipeline, the sample, or the READMEs.
#
#   usage: verify.sh [--quick]      --quick leaves out the two demo runs
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=$HERE/build
QUICK=${1:-}
export OPENCV_LOG_LEVEL=${OPENCV_LOG_LEVEL:-ERROR}

fail=0
step() {
  echo
  echo "=== $1 ==="
  shift
  if "$@"; then echo "ok"; else echo "FAILED"; fail=1; fi
}

# The end-to-end probe needs a clip; check_cli.sh uses the bundled one itself.
CLIP=${CLIP:-$HERE/build/verify_clip.mp4}
if [ "$QUICK" != "--quick" ] && [ ! -s "$CLIP" ]; then
  step "clip for the demo runs" env SRC= ffmpeg -v error -y \
    -i "$(cd "$HERE/../../../.." && pwd)/modules/v4d/assets/videos/dance.mp4" \
    -frames:v 120 -an "$CLIP"
fi

step "build" "$HERE/build.sh" || exit 1

# 1. The anchor table, which is now computed rather than generated. No OpenCV,
#    no models, no clip -- so this always runs.
step "anchor table" "$BUILD/test_anchors"

# 2. The crop path: that the sample's fused warp and its landmark mapping agree
#    with an independent rebuild of the same geometry, and what the change of
#    resampling costs in landmark pixels.
step "pose crop" "$BUILD/probe_crop"

# 3. The pipeline on a clip: track ids, duplicate skeletons, coasting frames,
#    trail saturation and the detect/pose split.
step "pipeline, one person" "$BUILD/bench_pipeline"

# 4. The same on a two-person composite, which is the only coverage of the
#    multi-person association, the duplicate merge and the person limit.
step "two-person clip" "$HERE/make_two_person.sh" "$BUILD/two_person.mp4" 200
step "pipeline, two people" "$BUILD/bench_pipeline" "$BUILD/two_person.mp4" 200 1040 700

if [ "$QUICK" != "--quick" ]; then
  # 5. The demo's command line contract.
  step "command line" "$HERE/check_cli.sh"
  # 6. The demo itself, headless: wall time and how much of the clip is recorded.
  step "end to end" "$HERE/probe_e2e.sh" "$CLIP" verify
fi

echo
[ "$fail" = 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail