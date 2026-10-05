#!/usr/bin/env bash
# Map an Android ABI name to its NDK target triple, and print the paths of the
# NDK libraries that live in that triple's library directory.
#
#   . tools/android/env.sh
#   TRIPLE=$(tools/android/abi-triple.sh arm64-v8a)          # aarch64-linux-android
#   LIBS=$(tools/android/abi-triple.sh --libs arm64-v8a)     # <sysroot>/usr/lib/aarch64-linux-android/32
#
# Needed for hand-written link lines: CMake's FindOpenGL does not find the NDK's
# EGL/GLESv3 (its Android branch still assumes the pre-r19 sysroot layout, and
# LEGACY mode insists on a GL/gl.h that the NDK does not ship).
set -euo pipefail

ABI="${1:-}"
if [ "$ABI" = "--libs" ]; then
  ABI="${2:-arm64-v8a}"
  : "${ANDROID_SYSROOT:?source tools/android/env.sh first}"
  case "$ABI" in
    arm64-v8a)   echo "$ANDROID_SYSROOT/usr/lib/aarch64-linux-android/$ANDROID_API_LEVEL" ;;
    armeabi-v7a) echo "$ANDROID_SYSROOT/usr/lib/armv7a-linux-androideabi/$ANDROID_API_LEVEL" ;;
    x86_64)      echo "$ANDROID_SYSROOT/usr/lib/x86_64-linux-android/$ANDROID_API_LEVEL" ;;
    x86)         echo "$ANDROID_SYSROOT/usr/lib/i686-linux-android/$ANDROID_API_LEVEL" ;;
    *) echo "abi-triple.sh: unknown ABI '$ABI'" >&2; exit 1 ;;
  esac
  exit 0
fi

case "${ABI:-arm64-v8a}" in
  arm64-v8a)   echo aarch64-linux-android ;;
  armeabi-v7a) echo armv7a-linux-androideabi ;;
  x86_64)      echo x86_64-linux-android ;;
  x86)         echo i686-linux-android ;;
  *) echo "abi-triple.sh: unknown ABI '${ABI:-}'" >&2; exit 1 ;;
esac