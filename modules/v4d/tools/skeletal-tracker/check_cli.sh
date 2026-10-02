#!/usr/bin/env bash
# Exercise the demo's command line handling:
#   1. no output argument  -> must not write a file (recording is opt-in)
#   2. recording           -> must honour the demo's fixed 1920x1080 viewport
#   3. missing input       -> must fail with usage
# There is no viewport argument: the sample's viewport is a constant, and
# recording is written at that size whatever the source's own resolution is.
#
#   usage: check_cli.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
BIN=${BIN:-$REPO/opencv/build/bin/example_v4d_skeletal-tracker-demo}
CLIP=${CLIP:-$REPO/modules/v4d/assets/videos/dance.mp4}
WORK=${WORK:-$(mktemp -d)}
export OPENCV_LOG_LEVEL=${OPENCV_LOG_LEVEL:-ERROR}

if [ ! -x "$BIN" ]; then
  echo "FAIL  no demo binary at $BIN (build example_v4d_skeletal-tracker-demo first)"
  exit 1
fi

fail=0
check() { if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

# --- 1. no output argument: nothing may be written ----------------------------
cd "$WORK"
rm -f skeletal_tracker_out.mkv
timeout 300 xvfb-run -a "$BIN" "$CLIP" >"$WORK/cli1.log" 2>&1 || echo "  (exited $?)"
if [ -e "$WORK/skeletal_tracker_out.mkv" ]; then
  echo "FAIL  no-output-arg: wrote skeletal_tracker_out.mkv anyway"; fail=1
else
  echo "PASS  no-output-arg: no file written"
fi

# --- 2. recording: the sink is built from the demo's viewport, not the source --
OUT=$WORK/cli_rec.mkv
rm -f "$OUT"
timeout 300 xvfb-run -a "$BIN" "$CLIP" "$OUT" >"$WORK/cli2.log" 2>&1 || echo "  (exited $?)"
size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$OUT" 2>/dev/null | tr -d '\n' || true)
check "recorded at the fixed viewport" "$size" "1920,1080"

# --- 3. bad input -----------------------------------------------------------
if timeout 60 xvfb-run -a "$BIN" "$WORK/does_not_exist.mp4" >"$WORK/cli3.log" 2>&1; then
  echo "FAIL  missing input: exited 0"; fail=1
else
  echo "PASS  missing input: exited non-zero"
fi

exit $fail