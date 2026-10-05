#!/usr/bin/env bash
# Rebuild a subset of the Android demo targets, stage them, and print only the
# diagnostics.
#
#   tools/android/rebuild-samples.sh font_rendering nanovg-demo font-demo
#   tools/android/rebuild-samples.sh            # every sample in OPENCV_V4D_SAMPLES
#
# Iterating on a sample through './build.sh android' is slow: the driver
# reconfigures and relinks the whole OpenCV build, and a failing ninja line is
# 2 KB of compiler flags. This goes straight at ninja and prints the
# diagnostics alone (see log-errors.sh).
#
# The freshly linked .so's are staged into jniLibs/ exactly as android-build.sh
# stages them, because package-apk.sh packages what is *staged*, not what ninja
# produced. Without this step a rebuild that succeeded and a rebuild that was a
# silent no-op produce the same APK, and a fix that was never on the device looks
# like a fix that did not work.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
V4D_ANDROID_ENV_QUIET=1 . "$SCRIPT_DIR/env.sh"

ABI="${ANDROID_ABI:-arm64-v8a}"
STAGE_ROOT="${ANDROID_STAGE:-$REPO_DIR/build/android}"
BUILD_DIR="$STAGE_ROOT/$ABI"
JNI_LIBS="$STAGE_ROOT/jniLibs/$ABI"
# Same layout OpenCV uses for ABI-suffixed output; a target can also land
# straight in lib/ when there is only one ABI configured.
LIB_DIRS=("$BUILD_DIR/lib/$ABI" "$BUILD_DIR/lib")

if [ $# -gt 0 ]; then
  NAMES=("$@")
  TARGETS=()
  for name in "${NAMES[@]}"; do TARGETS+=("example_v4d_$name"); done
else
  mapfile -t NAMES < <("$SCRIPT_DIR/sample-list.sh" --lines)
  TARGETS=()
  for name in "${NAMES[@]}"; do TARGETS+=("example_v4d_$name"); done
fi

echo "==> ninja ${TARGETS[*]}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
set +e
ninja -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" "${TARGETS[@]}" > "$LOG" 2>&1
STATUS=$?
set -e

if [ "$STATUS" != 0 ]; then
  "$SCRIPT_DIR/log-errors.sh" "$LOG"
  echo "==> FAILED (full log: $LOG)" >&2
  exit "$STATUS"
fi

echo "==> OK"
grep -E "^\[[0-9]+/[0-9]+\] Linking" "$LOG" | sed 's|.*/||' || true

# --- stage -------------------------------------------------------------------
# Mirrors android-build.sh: the demo .so files, stripped of debug info, are what
# package-apk.sh copies into an APK. --strip-debug keeps .dynsym, which is what
# dlopen() and the NativeActivity callback lookup (dlsym) read.
mkdir -p "$JNI_LIBS"
STRIP_TOOL="$ANDROID_NDK/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/bin/llvm-strip"
staged=0
missing=()
for name in "${NAMES[@]}"; do
  so="libv4ddemo_$name.so"
  src=""
  for dir in "${LIB_DIRS[@]}"; do
    [ -f "$dir/$so" ] && { src="$dir/$so"; break; }
  done
  if [ -z "$src" ]; then
    missing+=("$name")
    continue
  fi
  cp -f "$src" "$JNI_LIBS/$so"
  [ -x "$STRIP_TOOL" ] && "$STRIP_TOOL" --strip-debug "$JNI_LIBS/$so"
  staged=$((staged + 1))
done

echo "==> Staged $staged/${#NAMES[@]} into $JNI_LIBS"
for name in "${missing[@]+"${missing[@]}"}"; do
  echo "    !! $name: no libv4ddemo_$name.so in the build tree" >&2
done
[ "${#missing[@]}" -eq 0 ] || exit 1