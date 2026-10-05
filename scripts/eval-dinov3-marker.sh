#!/bin/bash
# Measure the DINOv3 Marker Demo on the shipped marker assets.
#
#   harddisk_as_marker.jpeg   the marker, registered as "harddisk"
#   marker_test.mp4           handheld footage of that same harddisk
#   {dance,dance2,bunny,kristen}.mp4   the false-positive corpus
#
# The demo's real behaviour lives in V4D-free translation units, so the
# head-less example_v4d_dinov3-marker-selftest drives exactly the same
# embedder, database, patch matcher and geometric verifier the window uses --
# without needing a display or a GL context.
#
# usage: ./scripts/eval-dinov3-marker.sh [--every N] [--quick] [--sweep]
#                                      [--dump FILE] [--assets DIR] [--tag NAME]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

EVERY=10
SWEEP=
DUMP=
TAG=
ASSETS="$REPO_DIR/modules/v4d/assets"

usage() {
  cat <<EOF
usage: $(basename "$0") [options]

  --every N     process every Nth frame (default 10)
  --quick       every 40th frame, a fast smoke run
  --sweep       also measure input size, crop and feature type
  --dump FILE   write per-frame CSV rows for offline analysis
  --assets DIR  root holding videos/, images/ and models/
                (default: $REPO_DIR/modules/v4d/assets)
  --tag NAME    label for the saved report (default: run)
EOF
  exit 0
}

while [ $# -gt 0 ]; do
  case "$1" in
    --every)  EVERY="$2"; shift 2 ;;
    --quick)  EVERY=40; shift ;;
    --sweep)  SWEEP=--sweep; shift ;;
    --dump)   DUMP="$2"; shift 2 ;;
    --assets) ASSETS="$2"; shift 2 ;;
    --tag)    TAG="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done
[ -n "$TAG" ] || TAG="every${EVERY}"

# Prefer a freshly built binary in the build tree; fall back to an installed one.
SELFTEST=""
for candidate in \
    "$REPO_DIR/opencv/build/bin/example_v4d_dinov3-marker-selftest" \
    "$(command -v example_v4d_dinov3-marker-selftest 2>/dev/null || true)"; do
  [ -n "$candidate" ] && [ -x "$candidate" ] && { SELFTEST="$candidate"; break; }
done
if [ -z "$SELFTEST" ]; then
  echo "cannot find example_v4d_dinov3-marker-selftest; run ./build.sh -t plan+v4d" >&2
  exit 1
fi

RESULTS_DIR="${RESULTS_DIR:-$REPO_DIR/eval-results}"
mkdir -p "$RESULTS_DIR"
REPORT="$RESULTS_DIR/dinov3-marker-$TAG.txt"
CSV="$RESULTS_DIR/dinov3-marker-$TAG.csv"
[ -n "$DUMP" ] || DUMP="$CSV"

ARGS=(--assets "$ASSETS" --every "$EVERY" --dump "$DUMP")
[ -n "$SWEEP" ] && ARGS+=("$SWEEP")

echo "==> $SELFTEST (every $EVERY frame)"
set +e
"$SELFTEST" "${ARGS[@]}" | tee "$REPORT"
status=${PIPESTATUS[0]}
set -e

echo
echo "report: $REPORT"
echo "csv:    $DUMP"
exit "$status"
