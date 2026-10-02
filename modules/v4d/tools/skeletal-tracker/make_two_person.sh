#!/usr/bin/env bash
# Make a synthetic two-person clip at native resolution.
#
# The bundled clips have one dancer each, so nothing in the repo exercises the
# multi-person association path, the person limit or the duplicate merge. Two
# crops of the dancer are stacked side by side. The crop is taken at native
# resolution and the two halves are NOT rescaled: the MediaPipe detector sees
# the whole frame as a 224x224 input, so halving the pixels per person is
# exactly what makes the two-person case hard.
#
#   usage: make_two_person.sh <out.mp4> [frames]
set -euo pipefail

SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)/modules/v4d/assets/videos/dance.mp4}
OUT=${1:?output}
FRAMES=${2:-300}

# Dancer sits around x=750..1250, y=150..850 in the 1920x1080 source.
CROP=520:700:740:150

ffmpeg -v error -y -i "$SRC" -filter_complex \
  "crop=${CROP},split[left][right];[left][right]hstack=inputs=2[v]" \
  -map '[v]' -frames:v "$FRAMES" -an "$OUT"

ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=width,height,nb_read_frames,r_frame_rate -of default=nw=1 "$OUT"