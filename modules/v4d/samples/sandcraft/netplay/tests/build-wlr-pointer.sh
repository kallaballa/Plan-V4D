#!/usr/bin/env bash
# This file is part of OpenCV project.
# It is subject to the license terms in the LICENSE file found in the top-level
# directory of this distribution and at http://opencv.org/license.html.
# Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
#
# Build the wlr-pointer helper (roadmap.md §2): a privilege-free Wayland virtual
# pointer, used by the headless toolkit to inject drags (press+move+release) into
# the sample. wlrctl cannot hold a button; ydotool/evemu need /dev/uinput, which
# is root-only on the test host. This helper speaks
# zwlr_virtual_pointer_manager_v1 directly through the compositor instead.
#
# Everything it needs is committed next to it: the protocol XML is
# wlr-virtual-pointer-unstable-v1.xml and the source is wlr-pointer.c. The
# protocol bindings are generated with wayland-scanner into a build directory.
#
# Usage: build-wlr-pointer.sh [output-path]
#   output-path defaults to $HOME/.local/bin/wlr-pointer
#
# Requires: wayland-client dev headers + pkg-config + wayland-scanner, gcc.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest="${1:-$HOME/.local/bin/wlr-pointer}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if ! command -v wayland-scanner >/dev/null 2>&1; then
  echo "build-wlr-pointer.sh: wayland-scanner not found (install wayland-scanner)" >&2
  exit 1
fi
if ! pkg-config --exists wayland-client; then
  echo "build-wlr-pointer.sh: wayland-client pkg-config not found" >&2
  exit 1
fi

wayland-scanner client-header "$here/wlr-virtual-pointer-unstable-v1.xml" \
  "$work/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code "$here/wlr-virtual-pointer-unstable-v1.xml" \
  "$work/wlr-virtual-pointer-unstable-v1-protocol.c"

mkdir -p "$(dirname "$dest")"
gcc -O2 -Wall -Wextra -o "$dest" "$here/wlr-pointer.c" \
  "$work/wlr-virtual-pointer-unstable-v1-protocol.c" \
  -I"$work" $(pkg-config --cflags --libs wayland-client)

echo "built: $dest"
"$dest" --help 2>&1 | head -3 || true
ls -l "$dest"
