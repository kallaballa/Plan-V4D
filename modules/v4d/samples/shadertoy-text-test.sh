#!/usr/bin/env bash
# Builds and runs the unit tests for the editor's plain-C++ halves: the GLSL
# tokenizer, the text buffer, the palette table.
#
# The point of having this separately from shadertoy-editor-test.sh is the build
# time. shadertoy-editor-test.sh needs libopencv_v4d and the whole editor
# linked, which is minutes. These headers have no dependency on either, so this
# is a second, and the fiddly parts - offsets, line caches, state carried over
# line breaks - get checked a hundred times an hour instead of once a day.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
build="${1:-$here/.text-test}"

include=(
  -I"$here"
  -I"$repo/modules/v4d/third/imgui"
  -I"$repo/modules/v4d/include"
  -I"$repo/modules/v4d/third/AnyProperty"
  -I"$repo/modules/plan/include"
  -I"$repo/opencv/modules/core/include"
  -I"$repo/opencv/modules/imgproc/include"
  -I"$repo/opencv/build"
)

# The theme header reaches cv::samples::findFile, which lives in opencv_core, so
# the test links against the built library when there is one. Without it the
# test still compiles - it only needs the symbol if the inline that uses it is
# emitted - and a missing library here is not worth failing over.
libs=()
if [[ -f "$repo/opencv/build/lib/libopencv_core.so" ]]; then
  libs=(-L"$repo/opencv/build/lib" -lopencv_core -Wl,-rpath,"$repo/opencv/build/lib")
fi

mkdir -p "$build"
bin="$build/shadertoy-text-test"

echo "compiling $here/shadertoy-text-test.cpp"
# C++20 because imgui.h reaches the Plan headers through imconfig.h, and those
# use std::barrier. The editor itself is built with the same standard.
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter \
  "${include[@]}" "${libs[@]}" \
  -o "$bin" "$here/shadertoy-text-test.cpp"

echo "running $bin"
"$bin" "$@"