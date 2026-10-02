#!/usr/bin/env bash
# End-to-end measurement of the skeletal-tracker-demo on a fixed clip: how many
# source frames it consumed, how many it recorded, and the achieved frame rate.
#
#   usage: probe_e2e.sh <clip> [label] [extra demo args...]
#          NOSINK=1 probe_e2e.sh <clip>      # without the sink, to isolate encode
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
BIN=${BIN:-$REPO/opencv/build/bin/example_v4d_skeletal-tracker-demo}
CLIP=${1:?usage: probe_e2e.sh <clip> [label] [demo args...]}
LABEL=${2:-run}
shift 2 2>/dev/null || shift $#
WORK=${WORK:-$(mktemp -d)}
OUT=$WORK/r_${LABEL}.mkv
LOG=$WORK/r_${LABEL}.log
rm -f "$OUT"

if [ ! -x "$BIN" ]; then
  echo "FAIL  no demo binary at $BIN (build example_v4d_skeletal-tracker-demo first)"
  exit 1
fi

SRC=$(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames \
        -of csv=p=0 "$CLIP")
export OPENCV_LOG_LEVEL=${OPENCV_LOG_LEVEL:-ERROR}
# NOSINK=1 runs the demo without an output file, which isolates the cost of
# frame capture + inference + overlay from the cost of encoding.
if [ "${NOSINK:-0}" = 1 ]; then
  RUN=("$CLIP")
else
  RUN=("$CLIP" "$OUT" "$@")
fi
START=$(date +%s.%N)
timeout 300 xvfb-run -a "$BIN" "${RUN[@]}" >"$LOG" 2>&1
CODE=$?
END=$(date +%s.%N)
WALL=$(python3 -c "print(f'{$END-$START:.1f}')")

GOT=0
[ -s "$OUT" ] && GOT=$(ffprobe -v error -count_frames -select_streams v:0 \
             -show_entries stream=nb_read_frames -of csv=p=0 "$OUT" 2>/dev/null || echo 0)
SIZE=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$OUT" 2>/dev/null || echo '-')
SRCSZ=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$CLIP")
FPS=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
       -of csv=p=0 "$CLIP" 2>/dev/null || echo '?')

echo "label=$LABEL  clip=$(basename "$CLIP")  src=${SRCSZ}  $FPS fps  src_frames=$SRC"
echo "exit=$CODE$( [ "$CODE" = 124 ] && echo ' (timeout)')  wall=${WALL}s  recorded=$GOT ($SIZE)"
python3 - "$WALL" "$GOT" <<'EOF'
import sys
wall, got = float(sys.argv[1]), int(sys.argv[2])
if wall > 0 and got:
    print(f"recorded rate  : {got/wall:.1f} fps")
EOF
grep -m1 'End of stream' "$LOG" || true
echo "--- non-INFO log lines ---"
grep -v '^\[ INFO\|^\[ WARN\|^$' "$LOG" | tail -8
echo "--- artefacts: $OUT $LOG ---"