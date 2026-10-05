#!/usr/bin/env bash
# Fetch and cross-build FreeType for one Android ABI.
#
#   tools/android/build-freetype.sh <install-prefix> [abi]
#
# modules/v4d/CMakeLists.txt does find_package(Freetype REQUIRED) and compiles
# third/imgui/misc/freetype/imgui_freetype.cpp into the module, but the NDK
# ships no FreeType and OpenCV's WITH_* 3rd-party options do not build one for
# Android. FreeType >= 2.13 has a first-class CMake build that honours the NDK
# toolchain file, so it is built here, once per ABI, into its own prefix that is
# then handed to OpenCV as FREETYPE_DIR.
#
# Idempotent: an existing libfreetype.a in the prefix is kept.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FREETYPE_VERSION="${V4D_FREETYPE_VERSION:-2.13.3}"
# sha256 of freetype-${FREETYPE_VERSION}.tar.xz from download.savannah.gnu.org.
# Upstream only publishes a detached .sig, so the hash is pinned here; override
# with V4D_FREETYPE_SHA256 to move to another version.
FREETYPE_SHA256="${V4D_FREETYPE_SHA256:-0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289}"
FREETYPE_URL="${V4D_FREETYPE_URL:-https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.xz}"

PREFIX="${1:?usage: build-freetype.sh <install-prefix> [abi]}"
ABI="${2:-arm64-v8a}"
JOBS="$(nproc 2>/dev/null || echo 4)"

# shellcheck source=tools/android/env.sh
. "$SCRIPT_DIR/env.sh"

if [ -f "$PREFIX/lib/libfreetype.a" ]; then
  echo "==> FreeType already installed in $PREFIX"
  exit 0
fi

# Keep the sources next to the build tree so a repeated run reuses the download.
SRC_ROOT="${V4D_DEPS_ROOT:-$(dirname "$PREFIX")/deps}"
TARBALL="$SRC_ROOT/freetype-${FREETYPE_VERSION}.tar.xz"
SRC_DIR="$SRC_ROOT/freetype-${FREETYPE_VERSION}"
BUILD_DIR="$PREFIX/freetype-build"

mkdir -p "$SRC_ROOT"
if [ ! -f "$TARBALL" ]; then
  echo "==> Downloading FreeType $FREETYPE_VERSION"
  curl -fSL --retry 3 -o "$TARBALL.part" "$FREETYPE_URL"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "${FREETYPE_SHA256}  $TARBALL" | sha256sum -c - >/dev/null || {
  echo "build-freetype: sha256 mismatch for $TARBALL" >&2; exit 1; }

if [ ! -d "$SRC_DIR" ]; then
  tar -xJf "$TARBALL" -C "$SRC_ROOT"
fi

echo "==> Building FreeType for $ABI into $PREFIX"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$ANDROID_API_LEVEL" \
  -DANDROID_STL="${ANDROID_STL:-c++_static}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DFT_DISABLE_HARFBUZZ=ON \
  -DFT_DISABLE_BROTLI=ON \
  -DFT_DISABLE_BZIP2=ON \
  -DFT_DISABLE_PNG=ON \
  -DFT_DISABLE_ZLIB=ON \
  -DFT_DISABLE_BDF=ON
cmake --build "$BUILD_DIR" -- -j"$JOBS"
cmake --build "$BUILD_DIR" --target install >/dev/null

[ -f "$PREFIX/lib/libfreetype.a" ] || { echo "build-freetype: $PREFIX/lib/libfreetype.a missing" >&2; exit 1; }
echo "==> FreeType $FREETYPE_VERSION ($ABI) in $PREFIX"