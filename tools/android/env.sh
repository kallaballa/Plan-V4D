#!/usr/bin/env bash
# Locate the Android SDK/NDK and export everything the V4D Android build needs.
# Source this, do not execute it:
#
#   . tools/android/env.sh
#
# Resolution order for each variable is: an explicit environment override, then
# the newest matching directory under the SDK. Nothing is guessed: if the SDK or
# NDK cannot be found the script fails loudly with the export that would fix it.
#
# Exports: ANDROID_SDK_ROOT ANDROID_NDK ANDROID_TOOLCHAIN_FILE ANDROID_HOST_TAG
#          ANDROID_SYSROOT ANDROID_SDK_BUILD_TOOLS ANDROID_API_LEVEL ANDROID_ABI

set -euo pipefail

# --- API level / ABI defaults (overridable, used by the other scripts) -----
export ANDROID_API_LEVEL="${ANDROID_API_LEVEL:-32}"
export ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"

_fail() {
  echo "env.sh: $*" >&2
  return 1 2>/dev/null || exit 1
}

# --- SDK root --------------------------------------------------------------
if [ -z "${ANDROID_SDK_ROOT:-}" ]; then
  ANDROID_SDK_ROOT="${ANDROID_SDK:-$HOME/Android/Sdk}"
fi
[ -d "$ANDROID_SDK_ROOT" ] || _fail "ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT does not exist.
  Install it with: tools/android/install-sdk.sh
  or point at an existing one:
    export ANDROID_SDK_ROOT=/path/to/Android/Sdk"
export ANDROID_SDK_ROOT
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_SDK="$ANDROID_SDK_ROOT"

# --- NDK -------------------------------------------------------------------
# Newest by version sort, so 27.2.12479018 wins over 26.1.10909125. The
# pre-r19 'ndk-bundle' layout is still accepted.
if [ -z "${ANDROID_NDK:-}" ]; then
  ANDROID_NDK_NAME=$(ls -1 "$ANDROID_SDK_ROOT/ndk" 2>/dev/null | sort -V | tail -1 || true)
  if [ -n "$ANDROID_NDK_NAME" ]; then
    ANDROID_NDK="$ANDROID_SDK_ROOT/ndk/$ANDROID_NDK_NAME"
  elif [ -d "$ANDROID_SDK_ROOT/ndk-bundle" ]; then
    ANDROID_NDK="$ANDROID_SDK_ROOT/ndk-bundle"
  else
    _fail "no NDK under $ANDROID_SDK_ROOT/ndk
  Install one with: tools/android/install-sdk.sh"
  fi
fi
[ -d "$ANDROID_NDK" ] || _fail "ANDROID_NDK=$ANDROID_NDK is not a directory"
export ANDROID_NDK
# Legacy spellings still read by Gradle/AGP and by some NDK headers.
export ANDROID_NDK_HOME="$ANDROID_NDK"

export ANDROID_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
[ -f "$ANDROID_TOOLCHAIN_FILE" ] || _fail "missing $ANDROID_TOOLCHAIN_FILE
  The NDK looks incomplete; re-install it with tools/android/install-sdk.sh"

# --- NDK host tag / sysroot ----------------------------------------------
# The unified toolchain ships exactly one prebuilt/ directory per host. Listing
# it is more reliable than mapping uname to a tag: the tags differ between NDK
# releases and Apple-silicon hosts use the x86_64 slice.
if [ -z "${ANDROID_HOST_TAG:-}" ]; then
  ANDROID_HOST_TAG=$(ls -1 "$ANDROID_NDK/toolchains/llvm/prebuilt" 2>/dev/null | head -1 || true)
  [ -n "$ANDROID_HOST_TAG" ] || _fail "no toolchains/llvm/prebuilt/* under $ANDROID_NDK"
fi
export ANDROID_HOST_TAG
export ANDROID_SYSROOT="$ANDROID_NDK/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/sysroot"
[ -d "$ANDROID_SYSROOT" ] || _fail "missing NDK sysroot $ANDROID_SYSROOT"

# --- build tools / platform (only needed for packaging an APK) -------------
if [ -z "${ANDROID_SDK_BUILD_TOOLS:-}" ]; then
  ANDROID_SDK_BUILD_TOOLS=$(ls -1 "$ANDROID_SDK_ROOT/build-tools" 2>/dev/null | sort -V | tail -1 || true)
fi
export ANDROID_SDK_BUILD_TOOLS="${ANDROID_SDK_BUILD_TOOLS:-}"
export ANDROID_JAVA_HOME="${ANDROID_JAVA_HOME:-${JAVA_HOME:-}}"

# --- summary ---------------------------------------------------------------
# Silence: sourcing this script from another script must not print.
if [ "${V4D_ANDROID_ENV_QUIET:-0}" != 1 ]; then
  cat <<EOF
ANDROID_SDK_ROOT        = $ANDROID_SDK_ROOT
ANDROID_NDK             = $ANDROID_NDK ($(sed -n 's/^Pkg.Revision *= *//p' "$ANDROID_NDK/source.properties" 2>/dev/null | tr -d '\r'))
ANDROID_TOOLCHAIN_FILE  = $ANDROID_TOOLCHAIN_FILE
ANDROID_SYSROOT         = $ANDROID_SYSROOT
ANDROID_HOST_TAG        = $ANDROID_HOST_TAG
ANDROID_API_LEVEL       = $ANDROID_API_LEVEL
ANDROID_ABI             = $ANDROID_ABI
ANDROID_SDK_BUILD_TOOLS = ${ANDROID_SDK_BUILD_TOOLS:-<none, packaging unavailable>}
EOF
fi