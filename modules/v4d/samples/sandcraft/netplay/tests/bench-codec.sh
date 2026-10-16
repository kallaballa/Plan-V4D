#!/bin/bash
# This file is part of OpenCV project.
# It is subject to the license terms in the LICENSE file found in the top-level
# directory of this distribution and at http://opencv.org/license.html.
# Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
#
# Optimised netplay-codec benchmark (roadmap M1 verification).
#
# The tree's build is Debug (build.sh's default), and zstd in a Debug build is
# an order of magnitude slower than at -O2, so timing the codec from
# opencv/build/bin/sandcraft_netplay_tests measures the compiler, not the codec.
# This script compiles the same test source against a freshly -O2-built zstd,
# into a throwaway directory, and runs it. It needs no OpenCV and touches no
# build tree.
#
# Usage: bench-codec.sh [output-dir]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../../../../.." && pwd)"
zstd_lib="$repo/modules/v4d/third/zstd/lib"
src="$here/test_netplay_codec.cpp"
out="${1:-/tmp/opencode/tmp-scripts/netplay-bench}"

if [ ! -f "$zstd_lib/zstd.h" ]; then
  echo "zstd submodule missing at $zstd_lib" >&2
  exit 1
fi

cc="${CC:-cc}"
cxx="${CXX:-c++}"
mkdir -p "$out/zstd" "$out/bin"

# Build libzstd at -O2, once. Cached by the archive's presence.
if [ ! -f "$out/zstd/libzstd_o2.a" ]; then
  rm -f "$out/zstd"/*.o
  for f in "$zstd_lib"/common/*.c "$zstd_lib"/compress/*.c \
           "$zstd_lib"/decompress/*.c "$zstd_lib"/dictBuilder/*.c; do
    "$cc" -O2 -fPIC -DZSTD_MULTITHREAD=0 -DZSTD_DISABLE_ASM \
      -DXXH_NAMESPACE=ZSTD_ -I"$zstd_lib" -c "$f" -o "$out/zstd/$(basename "${f%.c}").o"
  done
  ar rcs "$out/zstd/libzstd_o2.a" "$out/zstd"/*.o
fi

"$cxx" -O2 -std=c++20 -I"$here/.." -I"$zstd_lib" "$src" \
  "$out/zstd/libzstd_o2.a" -o "$out/bin/sandcraft_netplay_tests_o2"

"$out/bin/sandcraft_netplay_tests_o2"
