#!/usr/bin/env bash
# Build OpenCV + plan + v4d for Android, one ABI at a time.
#
#   tools/android-build.sh                        # arm64-v8a: configure, build, stage
#   tools/android-build.sh --abi armeabi-v7a
#   tools/android-build.sh --abi x86_64            # what the emulator wants
#   tools/android-build.sh --configure-only
#   tools/android-build.sh --clean
#
# This is the Android counterpart of ./build.sh and shares no code with it: that
# one drives an X11/Wayland host build (FFmpeg+VAAPI, OpenVINO, /usr/local/lib64,
# `sudo make install`), none of which means anything on a device.
#
# Artifacts:
#   build/android/<abi>/          the CMake/Ninja build tree
#   build/android/<abi>/freetype/ the cross-built FreeType (see build-freetype.sh)
#   build/android/<abi>/jniLibs/  the staged .so files for an APK
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ANDROID_HOST_DIR="$REPO_DIR/tools/android"

OPENCV_DIR="$REPO_DIR/opencv"
[ -d "$OPENCV_DIR" ] || { echo "android-build: $OPENCV_DIR missing; run 'git submodule update --init --recursive'" >&2; exit 1; }

STAGE="${ANDROID_STAGE:-$REPO_DIR/build/android}"
ABI="${ANDROID_ABI:-arm64-v8a}"
API_LEVEL="${ANDROID_API_LEVEL:-32}"
CONFIGURE_ONLY=0
CLEAN=0
STRIP=1
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() { sed -n '2,17p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --abi)            ABI="$2"; shift 2 ;;
    --api-level)      API_LEVEL="$2"; shift 2 ;;
    -j|--jobs)        JOBS="$2"; shift 2 ;;
    --configure-only) CONFIGURE_ONLY=1; shift ;;
    --no-strip)       STRIP=0; shift ;;
    --clean)          CLEAN=1; shift ;;
    -h|--help)        usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

export ANDROID_ABI="$ABI"
export ANDROID_API_LEVEL="$API_LEVEL"
# shellcheck source=tools/android/env.sh
. "$ANDROID_HOST_DIR/env.sh"

export ANDROID_EXTRA_MODULES_PATH="$REPO_DIR/modules"
BUILD_DIR="$STAGE/$ABI"
FREETYPE_PREFIX="$BUILD_DIR"

if [ "$CLEAN" = 1 ]; then
  echo "==> Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# --- FreeType (the NDK ships none; modules/v4d requires it) ----------------
# Exported, not just passed to build-freetype.sh: opencv-cmake-args.sh needs it
# too, for -DFREETYPE_DIR. Leaving it unset there makes that script exit on an
# unbound variable, and its empty output turns into a *host* configure, which
# then succeeds and quietly builds the wrong thing.
export V4D_FREETYPE_DIR="$FREETYPE_PREFIX"
"$ANDROID_HOST_DIR/build-freetype.sh" "$FREETYPE_PREFIX" "$ABI"

# --- configure -------------------------------------------------------------
echo "==> Configuring OpenCV + plan + v4d for $ABI (API $API_LEVEL) in $BUILD_DIR"
# opencv-cmake-args.sh keeps blank lines and # comments in its output for
# readability; cmake rejects anything that is not an option or a generator
# expression, so they are dropped here.
mapfile -t CMAKE_ARGS < <("$ANDROID_HOST_DIR/opencv-cmake-args.sh" "$ABI" | grep -e '^-D')
# An empty argument list would configure OpenCV for the *host* -- successfully,
# and for the wrong architecture -- so it is an error rather than something to
# discover three hours into a build.
[ "${#CMAKE_ARGS[@]}" -gt 0 ] || {
  echo "android-build: opencv-cmake-args.sh produced no -D arguments" >&2
  echo "  (run it directly to see why: $ANDROID_HOST_DIR/opencv-cmake-args.sh $ABI)" >&2
  exit 1
}
cmake -S "$OPENCV_DIR" -B "$BUILD_DIR" -GNinja "${CMAKE_ARGS[@]}"
if [ "$CONFIGURE_ONLY" = 1 ]; then
  echo "==> --configure-only: stopping after configure"
  exit 0
fi

# --- build -----------------------------------------------------------------
# The demo shared objects are the deliverable, so build them first: a failure
# there is the interesting one and should not be buried under the module build.
echo "==> Building (ninja -j$JOBS)"
cmake --build "$BUILD_DIR" -- -j"$JOBS"

# --- stage -----------------------------------------------------------------
# An APK is just a zip, so everything the app loads has to sit next to the demo
# .so under lib/<abi>/. OpenCV and plan are static, so this is normally just the
# demo, plus libc++_shared.so when ANDROID_STL=c++_shared.
#
# The staging directory is the build tree's own output copied, so the debug
# info OpenCV compiles with (-g) has to come off here: an APK stores its .so
# entries uncompressed, because NativeActivity maps them straight out of the
# zip, so every byte of .debug_* is a byte installed on the device. font_rendering
# is 79 MB as built and 12 MB stripped.
JNI_LIBS="$STAGE/jniLibs/$ABI"
rm -rf "$JNI_LIBS"
mkdir -p "$JNI_LIBS"
# OpenCV writes ABI-suffixed output to lib/<abi>/ (it uses the two- component
# layout so one tree can hold several), so this cannot look at lib/ directly.
# -type f and the name filter keep the import libraries out.
find "$BUILD_DIR/lib" -type f -name '*.so' ! -name '*.so.*' \
  -exec cp -f {} "$JNI_LIBS/" \; 2>/dev/null || true
if [ "${ANDROID_STL:-c++_static}" = "c++_shared" ]; then
  cp -f "$ANDROID_NDK/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/sysroot/usr/lib/$("$ANDROID_HOST_DIR/abi-triple.sh" "$ABI")/libc++_shared.so" "$JNI_LIBS/" 2>/dev/null || true
fi
if [ "$STRIP" = 1 ]; then
  STRIP_TOOL="$ANDROID_NDK/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/bin/llvm-strip"
  for so in "$JNI_LIBS"/libv4ddemo_*.so; do
    [ -e "$so" ] || continue
    # --strip-debug keeps .dynsym, which is what dlopen() and the
    # ANativeActivity callback lookup (dlsym) read; only the .debug_* sections
    # go. A full --strip-all would still work for the callbacks but leaves no
    # readable names in a logcat backtrace.
    "$STRIP_TOOL" --strip-debug "$so"
  done
fi

echo
echo "==> Staged $(ls -1 "$JNI_LIBS" | wc -l) shared objects:"
ls -1sh "$JNI_LIBS"
echo
DEMO_FIRST="$(cut -d, -f1 <<< "${OPENCV_V4D_SAMPLES:-font_rendering}")"
echo "Next: tools/android/package-apk.sh --demo $DEMO_FIRST --abi $ABI"
echo "      tools/android/run-demo.sh $DEMO_FIRST $ABI"