#!/usr/bin/env bash
# Fetch + unpack the ONNX Runtime binary for one Android ABI into the OpenCV
# build tree's 3rdparty dir, where OpenCV's dnn FindONNX.cmake expects it, and
# print the resulting ONNXRT_ROOT_DIR for the caller (android-build.sh) to pass
# to cmake as -DONNXRT_ROOT_DIR.
#
#   tools/android/fetch-onnxruntime.sh <build_dir> <abi>
#
# Why this exists: OpenCV's dnn download logic (modules/dnn/CMakeLists.txt) has
# no Android package. CMake reports Android as UNIX + aarch64, so it fetches the
# glibc onnxruntime-linux-aarch64-*.tgz, which cannot dlopen on Bionic
# ("libdl.so.2 not found") and makes every demo crash on launch. onnxruntime's
# official Android build ships inside the Maven artifact
# com.microsoft.onnxruntime:onnxruntime-android:<ver>.aar (jni/<abi>/*.so +
# headers). This script vendors exactly that.
#
# Idempotent: the AAR is cached under <build_dir>/3rdparty/onnxruntime/.cache
# and the unpack is skipped when android-$VER already has lib/ and include/.
set -euo pipefail

BUILD_DIR="${1:?usage: fetch-onnxruntime.sh <build_dir> <abi>}"
ABI="${2:?usage: fetch-onnxruntime.sh <build_dir> <abi>}"
# Absolute: the unpack below cd's into a temp dir, where a relative $AAR would
# no longer resolve.
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
VERSION="${ONNXRUNTIME_VERSION:-1.25.1}"
GROUP_PATH="com/microsoft/onnxruntime/onnxruntime-android/$VERSION"
AAR_NAME="onnxruntime-android-$VERSION.aar"
AAR_URL="https://repo1.maven.org/maven2/$GROUP_PATH/$AAR_NAME"

ORT_ROOT="$BUILD_DIR/3rdparty/onnxruntime/onnxruntime-android-$VERSION"
CACHE_DIR="$BUILD_DIR/3rdparty/onnxruntime/.cache"
AAR="$CACHE_DIR/$AAR_NAME"

# The AAR's jni/ directories are named after the NDK ABIs verbatim.
if [ ! -d "$ORT_ROOT/lib" ] || [ ! -f "$ORT_ROOT/lib/libonnxruntime.so" ] || [ ! -f "$ORT_ROOT/include/onnxruntime_cxx_api.h" ]; then
  mkdir -p "$CACHE_DIR"
  if [ ! -f "$AAR" ]; then
    echo "==> fetch-onnxruntime: downloading $AAR_URL" >&2
    curl -fsSL --retry 3 -o "$AAR.tmp" "$AAR_URL" && mv -f "$AAR.tmp" "$AAR" || { echo "fetch-onnxruntime: AAR download failed" >&2; exit 1; }
  fi
  rm -rf "$ORT_ROOT"
  mkdir -p "$ORT_ROOT/lib" "$ORT_ROOT/include"
  echo "==> fetch-onnxruntime: unpacking headers + jni/$ABI/libonnxruntime.so -> $ORT_ROOT" >&2
  TMP="$(mktemp -d)"
  (cd "$TMP" && unzip -oq "$AAR" "headers/*" "jni/$ABI/libonnxruntime.so")
  cp -a "$TMP/headers/." "$ORT_ROOT/include/"
  cp -a "$TMP/jni/$ABI/libonnxruntime.so" "$ORT_ROOT/lib/libonnxruntime.so"
  rm -rf "$TMP"
  [ -f "$ORT_ROOT/lib/libonnxruntime.so" ] && [ -f "$ORT_ROOT/include/onnxruntime_cxx_api.h" ] || {
    echo "fetch-onnxruntime: AAR had no lib/headers for ABI $ABI" >&2; exit 1; }
else
  echo "==> fetch-onnxruntime: reusing $ORT_ROOT" >&2
fi

# A stale glibc linux-aarch64 tree downloaded by dnn's own logic would be picked
# up by FindONNX's build-tree glob ahead of this one; drop it.
rm -rf "$BUILD_DIR/3rdparty/onnxruntime/onnxruntime-linux-aarch64-"* 2>/dev/null || true

printf '%s\n' "$ORT_ROOT"
