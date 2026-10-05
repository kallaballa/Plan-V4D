#!/usr/bin/env bash
# Print the Android package id of a V4D demo APK.
#
#   tools/android/package-id.sh video-demo   # com.opencv.v4d.video_demo
#
# package-apk.sh derives the manifest's package attribute from this and run-demo.sh
# needs the same string to address the installed app; aapt2 will not accept a
# hyphen in a package name and `am start` needs the exact id, so the rule lives
# here rather than being written down twice.
#
# A demo whose name has a hyphen (video-demo, cube-demo, ...) is not a valid Java
# package segment as-is. --package overrides the whole id.
set -euo pipefail

DEMO="${1:-font_rendering}"
if [ "${2:-}" != "" ]; then
  printf '%s\n' "$2"
  exit 0
fi
printf 'com.opencv.v4d.%s\n' "$(printf '%s' "$DEMO" | tr -c 'A-Za-z0-9_' '_')"