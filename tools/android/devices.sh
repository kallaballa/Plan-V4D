#!/usr/bin/env bash
# List the devices/emulators adb can see and the SDK/NDK the V4D build would use.
# Read-only: nothing is installed and nothing is started.
#
#   tools/android/devices.sh
#
# The exit status is 0 whenever the report was produced, even with no device
# attached -- "no device" is a result here, not a script failure.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "${1:-}" in
  -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
  "") ;;
  *) echo "unknown argument: $1" >&2; exit 1 ;;
esac

# Same resolution as tools/android/env.sh, but without failing when a piece is
# missing: this is an inventory, not a precondition check.
SDK="${ANDROID_SDK_ROOT:-${ANDROID_SDK:-${ANDROID_HOME:-$HOME/Android/Sdk}}}"
ADB="$(command -v adb || true)"
if [ -z "$ADB" ] && [ -x "$SDK/platform-tools/adb" ]; then
  ADB="$SDK/platform-tools/adb"
fi

echo "==> devices"
if [ -z "$ADB" ]; then
  echo "  adb not found (install platform-tools, or set ANDROID_SDK_ROOT)"
else
  [ -n "${ANDROID_SERIAL:-}" ] && echo "  ANDROID_SERIAL=$ANDROID_SERIAL (adb addresses this device only)"
  "$ADB" devices -l | sed 's/^/  /'
fi

echo
echo "==> emulators (AVDs)"
EMULATOR="$SDK/emulator/emulator"
if [ -x "$EMULATOR" ]; then
  AVDS="$("$EMULATOR" -list-avds 2>/dev/null || true)"
  if [ -n "$AVDS" ]; then printf '  %s\n' $AVDS; else echo "  (none configured)"; fi
else
  echo "  emulator not installed under $SDK/emulator"
fi

echo
echo "==> SDK / NDK the build would use"
echo "  ANDROID_SDK_ROOT        = $SDK"
if [ ! -d "$SDK" ]; then
  echo "  (not a directory; install it with tools/android/install-sdk.sh)"
  exit 0
fi

NDK_NAME="$(ls -1 "$SDK/ndk" 2>/dev/null | sort -V | tail -1 || true)"
if [ -n "$NDK_NAME" ]; then
  NDK="$SDK/ndk/$NDK_NAME"
elif [ -d "$SDK/ndk-bundle" ]; then
  NDK="$SDK/ndk-bundle"
else
  NDK=""
fi
echo "  ANDROID_NDK             = ${NDK:-<none>}${NDK:+ ($(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null | tr -d '\r'))}"

BT_NAME="$(ls -1 "$SDK/build-tools" 2>/dev/null | sort -V | tail -1 || true)"
echo "  ANDROID_SDK_BUILD_TOOLS = ${BT_NAME:-<none, packaging unavailable>}"

PLATFORMS="$(ls -1 "$SDK/platforms" 2>/dev/null | sort -V | tr '\n' ' ' || true)"
echo "  platforms               = ${PLATFORMS:-<none>}"

echo "  ANDROID_API_LEVEL       = ${ANDROID_API_LEVEL:-32}"
echo "  ANDROID_ABI             = ${ANDROID_ABI:-arm64-v8a}"
