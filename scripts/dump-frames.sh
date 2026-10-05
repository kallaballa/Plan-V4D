#!/usr/bin/env bash
# Extract frames from a clip as a contact sheet, so a claim about what the marker
# looks like can be checked against the footage instead of asserted.
#
# usage: dump-frames.sh CLIP "40 120 240 400" OUT.png
set -euo pipefail

clip=${1:?usage: dump-frames.sh CLIP "FRAME..." OUT.png}
frames=${2:?usage: dump-frames.sh CLIP "FRAME..." OUT.png}
out=${3:?usage: dump-frames.sh CLIP "FRAME..." OUT.png}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

i=0
for f in $frames; do
  ffmpeg -v error -i "$clip" -vf "select=eq(n\,$f)" -fps_mode passthrough \
    -frames:v 1 "$tmp/f$i.png"
  i=$((i + 1))
done

# montage in the order the frames were asked for; -label keeps the index visible.
args=()
i=0
for f in $frames; do
  args+=(-label "frame $f" "$tmp/f$i.png")
  i=$((i + 1))
done
montage "${args[@]}" -tile 4x -geometry +4+4 -background black "$out"
echo "$out"