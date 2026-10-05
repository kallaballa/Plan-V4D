#!/usr/bin/env bash
# Capture what a running V4D demo says on a connected device.
#
#   tools/android/debug-device.sh <demo> [abi] [seconds]
#
# Installs and starts the APK, waits, then writes three files under
# build/android/log/:
#
#   <demo>-logcat.txt   everything the process said (native + Java + crash)
#   <demo>-screen.png   a screencap, if the device allows one
#   <demo>-props.txt    the device's GL/display properties
#
# Exit status is 0 whenever the capture succeeded, even when the demo crashed --
# a crash is the interesting result here, not a script failure.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

DEMO="${1:-font_rendering}"
ABI="${2:-arm64-v8a}"
WAIT="${3:-12}"
LOG_DIR="$REPO_DIR/build/android/log"
PACKAGE_ID="com.opencv.v4d.$DEMO"
STAGE="${ANDROID_STAGE:-$REPO_DIR/build/android}"
APK="$STAGE/apk/$DEMO-$ABI.apk"

# adb is normally on PATH; when it is not, fall back to the same SDK the build
# uses so the two cannot disagree about which device is being talked to.
command -v adb >/dev/null || {
  SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
  [ -x "$SDK/platform-tools/adb" ] && PATH="$SDK/platform-tools:$PATH" && export PATH
}
command -v adb >/dev/null || {
  echo "debug-device: adb not found; install platform-tools or set ANDROID_SDK_ROOT" >&2
  exit 1
}
[ -f "$APK" ] || {
  echo "debug-device: $APK not found; build it with" >&2
  echo "  ./build.sh android --apk --demo $DEMO" >&2
  exit 1
}

mkdir -p "$LOG_DIR"
PROPS="$LOG_DIR/$DEMO-props.txt"
LOGCAT="$LOG_DIR/$DEMO-logcat.txt"
SHOT="$LOG_DIR/$DEMO-screen.png"

# ---- device properties ------------------------------------------------------
{
  echo "serial        : $(adb get-serialno)"
  echo "android       : $(adb shell getprop ro.build.version.release) (API $(adb shell getprop ro.build.version.sdk))"
  echo "model         : $(adb shell getprop ro.product.model)"
  echo "abi list      : $(adb shell getprop ro.product.cpu.abilist)"
  echo "egl           : $(adb shell getprop ro.hardware.egl)"
  echo "vulkan        : $(adb shell getprop ro.hardware.vulkan)"
  echo "wm size       : $(adb shell wm size | tr '\n' ' ')"
  echo "wm density    : $(adb shell wm density)"
  echo "refresh rate  : $(adb shell dumpsys display 2>/dev/null | grep -m1 'fps=' || true)"
} | tee "$PROPS"

# ---- install + launch -------------------------------------------------------
echo
echo "==> installing $APK"
# No -g: adb install without that flag works; with it, the shell needs
# INSTALL_GRANT_RUNTIME_PERMISSIONS and the install fails outright.
adb install -r -d "$APK" >/dev/null || { echo "debug-device: install failed" >&2; exit 1; }
adb shell am force-stop "$PACKAGE_ID" >/dev/null 2>&1 || true
adb logcat -c
adb shell am start -n "$PACKAGE_ID/android.app.NativeActivity" >/dev/null

# ---- collect ----------------------------------------------------------------
echo "==> waiting ${WAIT}s"
sleep "$WAIT"
adb logcat -d -v time > "$LOGCAT"
adb exec-out screencap -p > "$SHOT" 2>/dev/null || rm -f "$SHOT"

echo
echo "logs   : $LOGCAT"
echo "screen : $SHOT"
echo
echo "---- app-relevant logcat -------------------------------------------"
# Everything the demo and the graphics stack had to say, plus the lines the
# platform prints when a native app dies.
grep -E 'v4d-activity|v4d-glfw|AndroidRuntime|DEBUG *:|libEGL|libGLESv2|Mali|BufferQueue|SurfaceFlinger|Canvas|eglCreate|native_window|libc *:' "$LOGCAT" \
  | tail -n 120 || echo "(nothing matched)"