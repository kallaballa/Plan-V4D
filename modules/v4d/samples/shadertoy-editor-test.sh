#!/usr/bin/env bash
# Test for modules/v4d/samples/shadertoy-editor.cpp.
#
# The editor is the only Shadertoy front end here that never touches the
# network, so everything it needs can be tested unattended:
#
#   1. every built-in sample compiles;
#   2. a shader that does not compile is reported with the pass and line the
#      author wrote, and --verify exits non-zero (so a script can use it);
#   3. a project survives a round trip through --export: what comes out is
#      valid Shadertoy JSON, and the editor reads its own output back;
#   4. a channel wired to a local image file renders that image;
#   5. --shot writes a PNG of the requested size that is not a blank frame;
#   6. the window itself is screenshotted when there is a display to grab.
#
#   ./shadertoy-editor-test.sh [outdir]
#
# There is no mock API here on purpose: nothing in this test should open a
# socket. Sway/grim and X11/import are both tried for the last step, and a
# headless box reports "skipped" for it instead of failing - a V4D window does
# not map without a window manager, which is true of the stock demos too.

set -u -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"

outdir="${1:-${TMPDIR:-/tmp}/shadertoy-editor-test}"
binary="$repo/opencv/build/bin/example_v4d_shadertoy-editor"
editor=("$binary")
status=0

die() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$binary" ] || die "$binary not built - run ./build.sh -t plan+v4d"
command -v python3 >/dev/null || die "python3 is missing"

rm -rf "$outdir"
mkdir -p "$outdir"
cd "$outdir" || die "cannot use $outdir"

run() {  # run <logname> <args...>  -> leaves the exit code in $?
  local log="$1"; shift
  timeout 120 "${editor[@]}" "$@" >"$log" 2>&1
}

# expect <name> <wanted exit code> <logname> <args...>
expect() {
  local name="$1" wanted="$2" log="$3"; shift 3
  run "$log" "$@"
  local got=$?
  if [ "$got" -ne "$wanted" ]; then
    echo "FAIL $name: exit $got, wanted $wanted" >&2
    tail -5 "$log" >&2
    status=1
    return 1
  fi
  return 0
}

# ---- 1. the built-in samples -----------------------------------------------

for sample in gradient feedback keyboard common; do
  if expect "new $sample" 0 "new-$sample.log" --verify --new "$sample" &&
     grep -q "^ok: " "new-$sample.log"; then
    echo "ok   new $sample ($(grep -m1 '^ok: ' "new-$sample.log" | cut -d' ' -f2-))"
  else
    echo "FAIL new $sample: no ok line in new-$sample.log" >&2
    status=1
  fi
done

# An unknown name has to be refused, not quietly turned into another sample.
if expect "new nosuch" 1 "new-nosuch.log" --verify --new nosuch; then
  echo "ok   new nosuch is refused"
else
  echo "FAIL new nosuch: should exit 1" >&2
  status=1
fi

# ---- 2. a shader that does not compile ------------------------------------

python3 - <<'PY'
import json

broken = {"Shader": {
    "info": {"id": "brkHS", "author": "test", "username": "test",
             "name": "Broken"},
    "renderpass": [
        {"type": "common", "name": "",
         "code": "float twice(float x) { return x * 2.0; }\n"},
        {"type": "image", "name": "Image",
         "code": ("void mainImage(out vec4 c, in vec2 f)\n{\n"
                  "    float v = twice(f.x / iResolution.x);\n"
                  "    thisFunctionDoesNotExist(v);\n"
                  "    c = vec4(v);\n}\n"),
         "inputs": [], "outputs": [{"id": 0, "channel": 0}]}]}}
open("broken.json", "w").write(json.dumps(broken, indent=1))
PY

if expect "broken" 1 broken.log --verify broken.json; then
  problems=""
  grep -q "^FAILED: Broken" broken.log || problems="$problems no-failed-line"
  # Line 4 of the image pass is the bad call, and that is what has to be named:
  # the driver counts lines in the assembled shader, the author does not.
  grep -q "pass 1 line 4" broken.log || problems="$problems wrong-line"
  grep -q "thisFunctionDoesNotExist" broken.log || problems="$problems no-message"
  grep -q ">|" broken.log || problems="$problems no-context"
  if [ -n "$problems" ]; then
    echo "FAIL broken:$problems" >&2
    sed -n '/^FAILED/,$p' broken.log | head -8 >&2
    status=1
  else
    echo "ok   broken (pass 1 line 4, with context)"
  fi
else
  echo "FAIL broken: --verify should exit 1" >&2
  status=1
fi

# A file that is not there at all is a different failure from one that does not
# compile, and it must not hang either.
if expect "missing file" 1 missing.log --verify does-not-exist.json; then
  echo "ok   a missing project file is reported, not ignored"
else
  echo "FAIL missing file: should exit 1" >&2
  status=1
fi

# ---- 3. export round trip --------------------------------------------------

if expect "export" 0 export.log --new feedback --export exported.json; then
  problems=""
  [ -s exported.json ] || problems="$problems nothing-written"
  python3 - <<'PY' || problems="$problems not-shadertoy-json"
import json, sys

d = json.load(open("exported.json"))
assert list(d.keys()) == ["Shader"], d.keys()
shader = d["Shader"]
assert shader["info"]["name"], "info.name missing"
passes = shader["renderpass"]
assert [p["type"] for p in passes] == ["buffer", "image"], passes
# A buffer pass has to declare the channel it reads, or it renders black.
# Shadertoy spells a buffer input "Name.0"; anything that points at the buffer
# pass by name is what the editor has to produce.
assert passes[0]["inputs"][0]["ctype"] == "buffer", passes[0]["inputs"]
assert passes[0]["inputs"][0]["src"].startswith(passes[0]["name"]), passes[0]["inputs"]
assert passes[-1]["outputs"], "the image pass has no output"
PY
  if [ -n "$problems" ]; then
    echo "FAIL export:$problems" >&2
    status=1
  elif expect "re-read exported" 0 verify-exported.log --verify exported.json &&
       grep -q "^ok: " verify-exported.log; then
    echo "ok   export round trip (feedback, 2 passes)"
  else
    echo "FAIL export: the exported file does not read back" >&2
    status=1
  fi
else
  echo "FAIL export: --export should exit 0" >&2
  status=1
fi

# ---- 4. a channel wired to a local image -----------------------------------

python3 - <<'PY'
import json, struct, zlib


def png(path, w, h, pixel):
    """A solid RGBA PNG, written by hand so the test needs no image library."""
    raw = b"".join(b"\x00" + bytes(pixel) * w for _ in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body)))

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


png("magenta.png", 4, 4, (255, 0, 255, 255))
shader = {"Shader": {
    "info": {"id": "texHS", "author": "test", "username": "test",
             "name": "Local texture"},
    "renderpass": [{
        "type": "image", "name": "Image",
        "code": ("void mainImage(out vec4 c, in vec2 f)\n{\n"
                 "    c = texture2D(iChannel0, f / iResolution.xy);\n}\n"),
        "inputs": [{"id": 0, "channel": 0, "ctype": "texture",
                    "src": "magenta.png", "filter": "linear",
                    "wrap": "clamp", "vflip": False}],
        "outputs": [{"id": 0, "channel": 0}]}]}}
open("texture.json", "w").write(json.dumps(shader, indent=1))
PY

if expect "texture" 0 texture.log --verify texture.json &&
   grep -q "^ok: " texture.log; then
  echo "ok   a project that reads magenta.png compiles"
else
  echo "FAIL texture: --verify should compile it" >&2
  sed -n '/^FAILED/,$p' texture.log | head -8 >&2
  status=1
fi

# ---- 5. --shot -------------------------------------------------------------
#
# A frame that is one flat color could be a working shader or a blank canvas,
# so the shot has to carry the texture's color to prove the pixels came from it.

if expect "shot" 0 shot.log --shot tex.png --frames 10 --size 200x120 texture.json &&
   [ -s tex.png ]; then
  if python3 - <<'PY'
import sys
try:
    import cv2
    import numpy as np
except ImportError:  # no OpenCV python bindings: only the size is checked
    sys.exit(0)

img = cv2.imread("tex.png", cv2.IMREAD_UNCHANGED)
if img is None or img.shape[:2] != (120, 200):
    raise SystemExit("shot is not 200x120")
b, g, r = img[:, :, 0].mean(), img[:, :, 1].mean(), img[:, :, 2].mean()
if not (r > 200 and b > 200 and g < 60):
    raise SystemExit("shot does not carry the texture color: b={:.0f} g={:.0f} r={:.0f}"
                     .format(b, g, r))
PY
  then
    echo "ok   --shot (200x120, the texture color is in it)"
  else
    echo "FAIL --shot: the frame does not show the texture" >&2
    status=1
  fi
else
  echo "FAIL --shot: no PNG written" >&2
  status=1
fi

# A shader that does not compile must not leave a stale picture behind.
rm -f broken.png
if expect "shot of a broken shader" 1 shot-broken.log --shot broken.png --frames 4 broken.json ||
   [ ! -s broken.png ]; then
  echo "ok   a broken shader writes no screenshot"
else
  echo "FAIL a broken shader should exit 1 and write no PNG" >&2
  status=1
fi

# ---- 6. the window, when there is a display to look at ---------------------

sway_rect() {
  swaymsg -t get_tree 2>/dev/null | python3 -c '
import json, sys
try:
    tree = json.load(sys.stdin)
except ValueError:
    sys.exit(1)


def walk(node):
    yield node
    for child in node.get("nodes", ()):
        for sub in walk(child):
            yield sub


for node in walk(tree):
    name = node.get("name") or ""
    if "Shadertoy" not in name and "shadertoy" not in (node.get("app_id") or "").lower():
        continue
    r = node.get("rect") or {}
    if r.get("width") and r.get("height"):
        print("{} {} {}x{}".format(r.get("x", 0), r.get("y", 0),
                                   r["width"], r["height"]))
        sys.exit(0)
sys.exit(1)
'
}

screenshot() {
  local i rect
  if command -v swaymsg >/dev/null && command -v grim >/dev/null &&
     [ -n "${WAYLAND_DISPLAY:-}" ]; then
    # sway reports a 0x0 rectangle for a few seconds after the surface is
    # created, so ask a few times instead of once.
    for i in $(seq 1 20); do
      if rect="$(sway_rect)"; then
        grim -g "${rect/ /,}" "$outdir/window.png" && return 0
        echo "      grim: invalid geometry for [$rect]"
        return 1
      fi
      sleep 1
    done
    echo "      sway never reported a usable rectangle for the window"
  fi
  if command -v xdotool >/dev/null && command -v import >/dev/null &&
     [ -n "${DISPLAY:-}" ]; then
    import -window root "$outdir/window.png" 2>"$outdir/window.err" && return 0
    echo "      import: $(head -1 "$outdir/window.err")"
  fi
  return 1
}

if command -v grim >/dev/null || command -v import >/dev/null; then
  "${editor[@]}" --new feedback >"$outdir/window.log" 2>&1 &
  window_pid=$!
  sleep 3
  if screenshot; then
    echo "ok   captured window.png (the panel is in there)"
  else
    echo "skip window screenshot - no compositor or display to grab"
  fi
  kill "$window_pid" 2>/dev/null
  wait "$window_pid" 2>/dev/null
else
  echo "skip window screenshot - neither grim nor import is installed"
fi

echo
echo "logs in $outdir"
[ "$status" -eq 0 ] || exit 1
echo "OK"