#!/usr/bin/env bash
# Install the Android SDK pieces V4D needs into $ANDROID_SDK_ROOT
# (default: ~/Android/Sdk). Nothing is installed system-wide; the tree is
# owned by the user and picked up by tools/android/env.sh.
#
#   tools/android/install-sdk.sh                 # cmdline-tools + ndk + build-tools
#   tools/android/install-sdk.sh --ndk 27.2.12479018
#   tools/android/install-sdk.sh --minimal        # NDK only, no aapt2/apksigner
#
# Idempotent: an existing, complete component is left alone. Re-run it after a
# partial failure rather than starting over.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CMDLINE_TOOLS_BUILD=13114758   # command-line tools 16.0, last release that runs on JDK 17-21
DEFAULT_NDK=27.2.12479018     # r27c
DEFAULT_BUILD_TOOLS=34.0.0
DEFAULT_PLATFORM=android-34
MINIMAL=0
NDK_VERSION="$DEFAULT_NDK"

usage() { sed -n '2,11p' "$0"; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --ndk)          NDK_VERSION="$2"; shift 2 ;;
    --build-tools)  DEFAULT_BUILD_TOOLS="$2"; shift 2 ;;
    --platform)     DEFAULT_PLATFORM="$2"; shift 2 ;;
    --minimal)      MINIMAL=1; shift ;;
    -h|--help)      usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage 1 ;;
  esac
done

SDK="${ANDROID_SDK_ROOT:-${ANDROID_SDK:-$HOME/Android/Sdk}}"
DL_CACHE="${ANDROID_DL_CACHE:-$HOME/.cache/android-sdk}"
mkdir -p "$SDK" "$DL_CACHE"

# ---- 1. command-line tools (sdkmanager) -----------------------------------
SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$SDKMANAGER" ]; then
  ZIP="$DL_CACHE/commandlinetools-$CMDLINE_TOOLS_BUILD.zip"
  if [ ! -s "$ZIP" ]; then
    echo "==> Downloading command-line tools ($CMDLINE_TOOLS_BUILD)"
    curl -fSL --retry 3 -o "$ZIP.part" \
      "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS_BUILD}_latest.zip"
    mv "$ZIP.part" "$ZIP"
  fi
  # The zip holds a top-level `cmdline-tools/` directory; sdkmanager insists on
  # living at <sdk>/cmdline-tools/<channel>/bin, so unpack into a staging dir
  # and move it into place under the `latest` channel name.
  STAGE="$DL_CACHE/cmdline-tools-stage"
  rm -rf "$STAGE"
  mkdir -p "$STAGE"
  unzip -q "$ZIP" -d "$STAGE"
  rm -rf "$SDK/cmdline-tools"
  mkdir -p "$SDK/cmdline-tools"
  mv "$STAGE/cmdline-tools" "$SDK/cmdline-tools/latest"
  rm -rf "$STAGE"
fi
[ -x "$SDKMANAGER" ] || { echo "install-sdk: $SDKMANAGER missing" >&2; exit 1; }
echo "==> sdkmanager: $("$SDKMANAGER" --version 2>/dev/null | tail -1)"

# ---- 2. packages ----------------------------------------------------------
PACKAGES=("ndk;$NDK_VERSION")
[ "$MINIMAL" = 0 ] && PACKAGES+=(
  "build-tools;$DEFAULT_BUILD_TOOLS"
  "platform-tools"
  "platforms;$DEFAULT_PLATFORM"
)

echo "==> Accepting licenses and installing: ${PACKAGES[*]}"
yes 2>/dev/null | "$SDKMANAGER" --sdk_root="$SDK" --licenses >/dev/null 2>&1 || true
"$SDKMANAGER" --sdk_root="$SDK" "${PACKAGES[@]}"

# ---- 3. report ------------------------------------------------------------
NDK_DIR="$SDK/ndk/$NDK_VERSION"
echo
echo "ANDROID_SDK_ROOT     = $SDK"
echo "ANDROID_NDK          = $NDK_DIR"
echo "  revision           = $(sed -n 's/^Pkg.Revision *= *//p' "$NDK_DIR/source.properties" 2>/dev/null)"
echo "  toolchain file     = $NDK_DIR/build/cmake/android.toolchain.cmake"
echo "  cmake in NDK       = $(ls -d "$NDK_DIR"/build/cmake/*/ 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')"
[ "$MINIMAL" = 0 ] && {
  echo "build-tools          = $SDK/build-tools/$DEFAULT_BUILD_TOOLS"
  echo "platform             = $SDK/platforms/$DEFAULT_PLATFORM"
}
echo
echo "Next: tools/android-build.sh"