#!/bin/bash
# Installs the multi-window GLFW library (kallaballa's emscripten PR #19233,
# vendored as legacy_library_glfw.js) into the active emsdk, replacing
# emscripten's stock src/library_glfw.js. The stock file is backed up once.
set -e
if [ -z "$EMSDK" ]; then
  for f in "$HOME/emsdk/emsdk_env.sh"; do
    [ -f "$f" ] && set +u && source "$f" >/dev/null && set -u && break
  done
fi
[ -n "$EMSDK" ] || { echo "emsdk not found" >&2; exit 1; }
DEST="$EMSDK/upstream/emscripten/src/library_glfw.js"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/legacy_library_glfw.js"
[ -f "$SRC" ] || { echo "vendored library not found: $SRC" >&2; exit 1; }
if [ ! -f "$DEST.orig" ]; then
  cp "$DEST" "$DEST.orig"
fi
if cmp -s "$DEST" "$SRC"; then
  echo "legacy library_glfw.js already installed"
else
  cp -f "$SRC" "$DEST"
  echo "installed multi-window library_glfw.js into $DEST (original kept as $DEST.orig)"
fi