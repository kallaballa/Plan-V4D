#!/usr/bin/env bash
# Launches the editor fullscreen on a private virtual display and writes one PNG
# of it.
#
# Xvfb is used rather than the running compositor on purpose: on a desktop the
# editor's window is one window among many, so a capture of the screen is a
# picture of whatever else happens to be open. A private display holds nothing but
# the editor, which is what makes the result a picture of the editor.
#
#   shadertoy-editor-fullscreen-shot.sh [out.png] [--new <sample>]
#                                      [--size WxH] [--display :N]
#
# --fullscreen is passed to the editor so the render fills the window rather than
# being inset by the panel, and the virtual screen is sized to match.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"

out="${TMPDIR:-/tmp}/shadertoy-editor-fullscreen.png"
# The samples are built as part of the OpenCV tree, so the binary lands in that
# tree's bin, not the repository's.
editor=("$root/opencv/build/bin/example_v4d_shadertoy-editor")
screen="1920x1080"
display=""

while [ $# -gt 0 ]; do
  case "$1" in
    --new)
      editor+=(--new "${2:?--new needs a sample name}")
      shift 2
      ;;
    --size)
      screen="$2"
      shift 2
      ;;
    --display)
      display="$2"
      shift 2
      ;;
    --out)
      out="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '3,15p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    -*)
      echo "unknown option $1" >&2
      exit 1
      ;;
    *)
      out="$1"
      shift
      ;;
  esac
done

for tool in Xvfb import xdotool; do
  command -v "$tool" >/dev/null || {
    echo "need $tool on PATH to capture the editor" >&2
    exit 1
  }
done
[ -x "${editor[0]}" ] || {
  echo "no editor at ${editor[0]} - build it first" >&2
  exit 1
}

mkdir -p "$(dirname "$out")"
rm -f "$out"

# Pick a display number nothing is already using, so two runs cannot collide.
if [ -z "$display" ]; then
  for n in $(seq 90 120); do
    if [ ! -e "/tmp/.X11-unix/X$n" ] && [ ! -e "/tmp/.X$n-lock" ]; then
      display=":$n"
      break
    fi
  done
fi
[ -n "$display" ] || {
  echo "no free X display number between :90 and :120" >&2
  exit 1
}

log="${out%.png}.log"
xvfb_pid=""
editor_pid=""
cleanup() {
  [ -n "$editor_pid" ] && kill "$editor_pid" 2>/dev/null || true
  [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

Xvfb "$display" -screen 0 "${screen}x24" -nolisten tcp >"${log}.xvfb" 2>&1 &
xvfb_pid=$!

# Xvfb takes a moment before the socket accepts connections.
for _ in $(seq 1 50); do
  xdpyinfo -display "$display" >/dev/null 2>&1 && break
  sleep 0.2
done
xdpyinfo -display "$display" >/dev/null 2>&1 || {
  echo "Xvfb never came up on $display" >&2
  cat "${log}.xvfb" >&2 || true
  exit 1
}

# Wayland is unset so the editor uses the X11 display we just made. Mesa then
# picks a software GL there, which is slower but has no dependency on the
# desktop's GPU or compositor.
env -u WAYLAND_DISPLAY DISPLAY="$display" "${editor[@]}" --fullscreen \
  >"$log" 2>&1 &
editor_pid=$!

# Wait for the window to be mapped. Its size is checked as well as its existence,
# because a window that is up but not yet laid out would capture as a blank or
# wrongly-sized picture.
win=""
for _ in $(seq 1 60); do
  sleep 0.5
  win="$(xdotool search --onlyvisible --pid "$editor_pid" 2>/dev/null | tail -1)"
  [ -n "$win" ] || continue
  geom="$(xdotool getwindowgeometry --shell "$win" 2>/dev/null || true)"
  w="$(printf '%s' "$geom" | sed -n 's/^WIDTH=//p')"
  h="$(printf '%s' "$geom" | sed -n 's/^HEIGHT=//p')"
  case "${w:-0}x${h:-0}" in
    0x0 | "") continue ;;
  esac
  break
done
if [ -z "$win" ]; then
  echo "the editor never mapped a window on $display" >&2
  cat "$log" >&2 || true
  exit 1
fi

# A few more frames so the panel has settled and the shader has compiled.
sleep 3

# Capture the editor's own window rather than the root, so the result is the
# editor even if something else ever shares the display.
import -window "$win" -display "$display" "$out" 2>"${log}.import" || {
  import -window root -display "$display" "$out" 2>>"${log}.import"
}
import -window "$win" -display "$display" -crop "$(xdotool getwindowgeometry --shell "$win" | sed -n 's/^\(WIDTH\|HEIGHT\)=//p' | paste -sd x)" +repage "$out" 2>/dev/null || true

if [ ! -s "$out" ]; then
  echo "import wrote nothing to $out" >&2
  cat "${log}.import" >&2 || true
  exit 1
fi

echo "wrote $out  (window $win on $display, $(xdotool getwindowgeometry --shell "$win" | sed -n 's/^\(WIDTH\|HEIGHT\)=//p' | paste -sd'x'))"
echo "log: $log"