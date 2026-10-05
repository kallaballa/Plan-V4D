#!/usr/bin/env bash
# This file is part of OpenCV project.
# It is subject to the license terms in the LICENSE file found in the top-level
# directory of this distribution and at http://opencv.org/license.html.
# Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
#
# Build and run the Tetralis check suite. The rules header has no dependency at
# all -- no window, no GPU, no clock -- so this needs nothing but a C++17
# compiler, and it is the fastest way to know that a change to
# `tetralis-rules.hpp` still plays by tetralis_rules.md.
#
#   ./tetralis-selftest.sh              # the whole suite
#   ./tetralis-selftest.sh 4242         # same checks, random plays use this seed
#   CXX=clang++ ./tetralis-selftest.sh

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
out="${TMPDIR:-/tmp}/tetralis-selftest.$$"

trap 'rm -f "$out"' EXIT

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -o "$out" "$here/tetralis-selftest.cpp"

"$out" "$@"