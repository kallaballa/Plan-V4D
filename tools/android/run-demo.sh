#!/usr/bin/env bash
# Install a V4D demo APK on a connected device or emulator and start it.
#
#   tools/android/run-demo.sh font_rendering
#   tools/android/run-demo.sh vector_graphics arm64-v8a
#   tools/android/run-demo.sh font_rendering arm64-v8a --log
#
# One device at a time: `adb` picks the only one when exactly one is attached,
# and errors out when several are, which is the intended behaviour.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

DEMO="${1:-font_rendering}"
ABI="${2:-${ANDROID_ABI:-arm64-v8a}}"
shift || true
shift 2>/dev/null || true

STAGE_ROOT="${ANDROID_STAGE:-$REPO_DIR/build/android}"
APK="$STAGE_ROOT/apk/$DEMO-$ABI.apk"
FOLLOW_LOG=0
for arg in "$@"; do
  case "$arg" in
    --log) FOLLOW_LOG=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 1 ;;
  esac
done

command -v adb >/dev/null || {
  # platform-tools is not always on PATH; the SDK copy is where it was installed.
  if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -x "$ANDROID_SDK_ROOT/platform-tools/adb" ]; then
    PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"
    export PATH
  else
    echo "run-demo: adb not found; install platform-tools or set ANDROID_SDK_ROOT." >&2
    exit 1
  fi
}
[ -f "$APK" ] || {
  echo "run-demo: $APK not found." >&2
  echo "  Build it first: ./build.sh -t android --apk --demo $DEMO" >&2
  exit 1
}

# The package id comes from the same helper package-apk.sh uses, and the activity
# is V4DCameraActivity -- the subclass of android.app.NativeActivity that asks for
# the camera permission before letting NativeActivity load the library. Both were
# correct to hardcode once and are not any more.
PACKAGE_ID="$("$SCRIPT_DIR/package-id.sh" "$DEMO")"
ACTIVITY="$PACKAGE_ID/org.opencv.v4d.V4DCameraActivity"

DEVICES=$(adb devices | awk 'NR > 1 && $2 == "device" { print $1 }')
[ -n "$DEVICES" ] || {
  echo "run-demo: no device is attached (or none is authorised)." >&2
  echo "  Check 'adb devices'; an emulator counts once it has booted." >&2
  exit 1
}
DEVICE_COUNT=$(printf '%s\n' "$DEVICES" | wc -l)
[ "$DEVICE_COUNT" = 1 ] || {
  echo "run-demo: $DEVICE_COUNT devices attached; pass one with ANDROID_SERIAL=" >&2
  printf '  %s\n' $DEVICES >&2
  exit 1
}
SERIAL="$DEVICES"
export ANDROID_SERIAL="$SERIAL"

echo "==> $SERIAL: $APK"
# -r reinstalls, -d allows a version downgrade.
# No -g: granting runtime permissions is a shell permission the plain adb install
# path does not have, and asking for it fails the whole install with a
# SecurityException rather than skipping the flag.
adb install -r -d "$APK"

# V4DCameraActivity asks for CAMERA and shows a system dialog when the app has
# not been granted it. A dialog cannot be answered over adb, so a script that
# starts the demo would sit there looking like a hang. Grant it here instead,
# and say so: `pm grant` needs the permission to be declared, which it now is.
adb shell pm grant "$PACKAGE_ID" android.permission.CAMERA 2>/dev/null || true

# A leftover instance from a previous run would keep rendering into the old
# library; NativeActivity holds a surface, so stop it first.
adb shell am force-stop "$PACKAGE_ID" >/dev/null 2>&1 || true

echo "==> Starting $ACTIVITY"
adb shell am start -n "$ACTIVITY"

if [ "$FOLLOW_LOG" = 1 ]; then
  LOG_TAG=v4d-activity
  echo "==> logcat ($LOG_TAG, ^D to stop)"
  adb logcat -s "$LOG_TAG" '*:S'
else
  echo
  echo "  logs:  adb logcat -s v4d-activity '*:S'"
  echo "  video:  adb exec-out screencap -p > screen.png"
  echo "  stop:   adb shell am force-stop $PACKAGE_ID"
fi