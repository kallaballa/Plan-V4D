#!/usr/bin/env bash
# Builds the skeletal-tracker harnesses against the in-tree OpenCV.
#
# They are not part of the OpenCV build: they poke at private sections of the
# pipeline header and rebuild that geometry independently, which is exactly what
# an extra target in modules/v4d/CMakeLists.txt should not be asked to do. Build
# them by hand instead -- see README.md.
#
#   usage: build.sh [output-dir]
#          OPENCV_BUILD=... build.sh        # to point at a different OpenCV build
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
OCV="$REPO/opencv"
BUILD=${1:-$HERE/build}
OPENCV_BUILD=${OPENCV_BUILD:-$OCV/build}

if [ ! -d "$OPENCV_BUILD/lib" ]; then
  echo "no OpenCV build in $OPENCV_BUILD; set OPENCV_BUILD" >&2
  exit 1
fi

mkdir -p "$BUILD"

INCLUDES=(
  -I"$OPENCV_BUILD"
  -I"$OCV/modules/core/include"
  -I"$OCV/modules/imgproc/include"
  -I"$OCV/modules/imgcodecs/include"
  -I"$OCV/modules/videoio/include"
  -I"$OCV/modules/dnn/include"
  -I"$OCV/modules/geometry/include"
  -I"$OCV/modules/video/include"
  -I"$REPO/modules/v4d/include"
  -I"$REPO/modules/plan/include"
  -I"$REPO/modules/v4d/third/AnyProperty"
  -I"$REPO/modules/v4d/third/imgui"
  -I"$REPO/modules/v4d/third/nanovg/src"
  -I"$HERE"
  -I"$REPO/modules/v4d/samples"
)
LIBS=(
  -L"$OPENCV_BUILD/lib"
  -Wl,-rpath,"$OPENCV_BUILD/lib"
  -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio
  -lopencv_dnn -lopencv_geometry -lopencv_v4d -lopencv_plan
)
FLAGS=(-std=c++20 -O2 -Wall)

# The anchor table is deliberately dependency-free, and this proves it: the
# check compiles with no OpenCV at all.
echo "building test_anchors"
g++ "${FLAGS[@]}" -I"$REPO/modules/v4d/samples" \
  "$HERE/test_anchors.cpp" -o "$BUILD/test_anchors"

for src in probe_crop bench_pipeline; do
  echo "building $src"
  # shellcheck disable=SC2086
  g++ "${FLAGS[@]}" "$HERE/$src.cpp" -o "$BUILD/$src" "${INCLUDES[@]}" "${LIBS[@]}"
done

echo "harnesses in $BUILD"