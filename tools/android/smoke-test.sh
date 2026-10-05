#!/usr/bin/env bash
# Install, launch and check every packaged V4D demo on the attached device.
#
#   tools/android/smoke-test.sh                 # every sample in OPENCV_V4D_SAMPLES
#   tools/android/smoke-test.sh video-demo beauty-demo
#   tools/android/smoke-test.sh --abi x86_64
#
# --settle N is how long to wait (seconds) for a demo to come up, not how long
# to wait after it has.
#
# For each demo: install, start it, wait, and then judge it on three things that
# a build log cannot tell you --
#
#   * the process is still alive (a cv::Exception thrown out of a demo takes the
#     process down, and the exit code is the only place it shows up)
#   * the window has real content in it (check-frame.py: a window that never
#     rendered is a flat fill)
#   * logcat has no cv::error, no assertion failure and no native crash
#
# This cannot judge *what* is on the screen -- a red/blue channel swap renders
# perfectly good pixels -- so it is a smoke test, not an image comparison.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

ABI="${ANDROID_ABI:-arm64-v8a}"
SETTLE="${V4D_SMOKE_SETTLE:-12}"
KEEP_LOG=""
DEMOS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --abi)   ABI="$2"; shift 2 ;;
    --settle) SETTLE="$2"; shift 2 ;;
    --keep-log) KEEP_LOG=1; shift ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    -*) echo "unknown argument: $1" >&2; exit 1 ;;
    *) DEMOS+=("$1"); shift ;;
  esac
done

STAGE_ROOT="${ANDROID_STAGE:-$REPO_DIR/build/android}"
APK_DIR="$STAGE_ROOT/apk"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
export ANDROID_ABI="$ABI"
PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"
export PATH

if [ ${#DEMOS[@]} -eq 0 ]; then
  mapfile -t DEMOS < <("$SCRIPT_DIR/sample-list.sh" --lines)
fi

DEVICES=$(adb devices | awk 'NR > 1 && $2 == "device" { print $1 }')
[ -n "$DEVICES" ] || { echo "smoke-test: no device attached" >&2; exit 1; }
[ "$(printf '%s\n' "$DEVICES" | wc -l)" -eq 1 ] || {
  echo "smoke-test: several devices attached; set ANDROID_SERIAL" >&2; exit 1; }
export ANDROID_SERIAL="$DEVICES"
echo "==> device $ANDROID_SERIAL ($ABI), ${SETTLE}s settle per demo"

PASS=0; FAIL=0; FAILED_DEMOS=()
LAUNCHED=()
stop_previous() {
  # Every app launched so far, not just the current one. Two demos at once is
  # worse than slow: the camera is a single exclusive resource, and a demo whose
  # camera was already open fails to open it again and dies in makeCamera() --
  # which looks like that demo being broken when the previous one is at fault.
  for p in ${LAUNCHED[@]+"${LAUNCHED[@]}"}; do
    adb shell am force-stop "$p" >/dev/null 2>&1
  done
  LAUNCHED=()
}

install_apk() {
  # Xiaomi refuses an adb install outright now and then with
  # INSTALL_FAILED_USER_RESTRICTED, which is a device-side "install via USB"
  # policy rather than anything to do with the APK. It is intermittent and the
  # same APK installs moments later, so retry rather than report a broken demo.
  local apk="$1" attempt
  for attempt in 1 2 3; do
    if adb install -r -d "$apk" 2>&1; then
      return 0
    fi
    sleep 3
  done
  return 1
}

for demo in "${DEMOS[@]}"; do
  apk="$APK_DIR/$demo-$ABI.apk"
  printf '\n=== %-26s ' "$demo"
  if [ ! -f "$apk" ]; then
    echo "SKIP (no APK)"
    FAIL=$((FAIL+1)); FAILED_DEMOS+=("$demo: not packaged"); continue
  fi

  pkg="$("$SCRIPT_DIR/package-id.sh" "$demo")"
  log="$(mktemp -t "v4d-smoke.XXXXXX")"

  if ! install_apk "$apk" >"$log" 2>&1; then
    echo "FAIL (install)"
    grep -m2 -E 'Failure|error' "$log" | sed 's/^/      /'
    FAIL=$((FAIL+1)); FAILED_DEMOS+=("$demo: install failed"); rm -f "$log"; continue
  fi
  # The launcher asks for CAMERA at runtime and a permission dialog cannot be
  # answered over adb; grant it so the run is not a wait for a human.
  adb shell pm grant "$pkg" android.permission.CAMERA >/dev/null 2>&1 || true
  stop_previous
  adb logcat -c 2>/dev/null
  adb shell am start -n "$pkg/org.opencv.v4d.V4DCameraActivity" >/dev/null 2>&1
  LAUNCHED=("$pkg")

  # Poll for the process rather than sleeping a fixed time: a fixed sleep is
  # either too short on a loaded device (reported as "process exited" when the
  # demo had not started yet) or needlessly long on an idle one.
  pid=""
  for _ in $(seq 1 "$SETTLE"); do
    sleep 1
    pid="$(adb shell pidof "$pkg" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
    [ -n "$pid" ] && break
  done
  # A demo that is alive only just after it started still has to survive a
  # moment: the interesting failures (End of stream, a bad model path) land on
  # the first frame, a second or two in.
  if [ -n "$pid" ]; then
    sleep 3
    pid="$(adb shell pidof "$pkg" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
  fi

  problems=()
  [ -n "$pid" ] || problems+=("process exited")

  shot="$(mktemp -t "v4d-shot.XXXXXX.png")"
  adb exec-out screencap -p > "$shot" 2>/dev/null
  if ! "$SCRIPT_DIR/check-frame.py" "$shot" >/dev/null 2>&1; then
    problems+=("blank/static window")
  fi
  rm -f "$shot"

  if [ -n "$pid" ]; then
    # The app's own log only, so an unrelated system error cannot fail a demo.
    adb logcat -d --pid="$pid" > "$log.applog" 2>/dev/null
    if grep -qE 'cv::error\(\)|Assertion failed|Fatal signal|terminating due to' "$log.applog"; then
      problems+=("error in logcat")
      grep -m3 -E 'cv::error\(\)|Assertion failed|Fatal signal|terminating due to' "$log.applog" \
        | sed 's/^/      /'
    fi
  else
    # No pid: the log is still there, just not attributable to one.
    adb logcat -d > "$log.applog" 2>/dev/null
    grep -m3 -E "cv::error\(\)|Assertion failed.*frame|Fatal signal|terminating due to" "$log.applog" \
      | grep -iE "v4d|frame|cv::" | sed 's/^/      /' || true
  fi

  if [ ${#problems[@]} -eq 0 ]; then
    echo "PASS"
    PASS=$((PASS+1))
  else
    echo "FAIL (${problems[*]})"
    FAIL=$((FAIL+1)); FAILED_DEMOS+=("$demo: ${problems[*]}")
  fi

  if [ -n "$KEEP_LOG" ]; then
    mkdir -p "$REPO_DIR/build/android/smoke-logs"
    cp -f "$log.applog" "$REPO_DIR/build/android/smoke-logs/$demo.log"
  fi
  rm -f "$log" "$log.applog"
done

printf '\n==> %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
  printf '  %s\n' "${FAILED_DEMOS[@]}"
  exit 1
fi