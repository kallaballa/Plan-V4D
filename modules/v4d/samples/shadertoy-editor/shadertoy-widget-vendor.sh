#!/usr/bin/env bash
# Vendor (or re-vendor) ImGuiColorTextEdit, the code widget the Shadertoy editor's
# code pane is built on.
#
# The widget is checked in under third/imgui-color-text-edit/ the same way glad/
# and PerlinNoise/ are - the sample needs it to build, so a checkout must not
# depend on the network reaching github. This script exists so that what is
# checked in can be re-derived and verified rather than trusted: it fetches the
# pinned tag, refuses to write anything unless every file matches its recorded
# SHA256, and is a no-op when the tree is already correct.
#
#   ./shadertoy-widget-vendor.sh            fetch and install (verify hashes)
#   ./shadertoy-widget-vendor.sh --check    only verify what is checked in
#
# Why this fork and not the original: BalazsJako's last commit was June 2019 and
# it calls ImGui APIs that were removed in 1.90, which is what V4D vendors (1.93
# WIP). The santaclose fork adds multi-cursor but pulls in boost::regex. Johan
# Goossens' fork is the one that still builds against current Dear ImGui, needs
# neither boost nor std::regex, and is released in step with ImGui itself, so
# this pins the v1.92.9 tag rather than a branch.
#
# Only the widget itself is vendored. TextDiff (which needs dtl.h, another 40k
# lines) and the LSP bridge are not used by the sample and are left behind.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../../.." && pwd)"
dst="$repo/modules/v4d/third/imgui-color-text-edit"

readonly repo_url="https://github.com/goossens/ImGuiColorTextEdit"
readonly tag="v1.92.9"
readonly base_url="$repo_url/raw/$tag"

# file:sha256. Recorded from the v1.92.9 tag; a mismatch means either the tag
# moved (it should not - it is a tag) or the download was corrupted.
readonly wanted=(
  "TextEditor.h:f89e3c76d9bba345c0ff626ebab238d8794eaf42e6d336fe513b6ea0a063d416"
  "TextEditor.cpp:a79f43877e297a1959df17c6c713e9f7111ea13a9f70e5c6003a8ce6292905b9"
)

mode="install"
case "${1:-}" in
  --check | check) mode="check" ;;
  "") ;;
  -h | --help)
    sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *)
    echo "usage: $(basename "$0") [--check]" >&2
    exit 2
    ;;
esac

hash_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

fail=0
for entry in "${wanted[@]}"; do
  file="${entry%%:*}"
  want="${entry##*:}"
  path="$dst/$file"

  if [ "$mode" = check ]; then
    if [ ! -f "$path" ]; then
      echo "missing: $file" >&2
      fail=1
      continue
    fi
    got="$(hash_of "$path")"
    if [ "$got" != "$want" ]; then
      echo "changed: $file ($got != $want)" >&2
      fail=1
    else
      echo "ok: $file"
    fi
    continue
  fi

  mkdir -p "$dst"
  tmp="$(mktemp)"
  trap 'rm -f "$tmp"' EXIT
  curl -fsSL "$base_url/$file" -o "$tmp"
  got="$(hash_of "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "$file: sha256 $got does not match the pinned $want" >&2
    echo "The tag $tag may have been moved. Check $repo_url/releases before" >&2
    echo "changing the hash here." >&2
    exit 1
  fi
  mv "$tmp" "$path"
  trap - EXIT
  echo "vendored $file ($got)"
done

if [ "$mode" = check ] && [ "$fail" != 0 ]; then
  echo >&2
  echo "The vendored widget does not match the pinned tag." >&2
  echo "Re-run without --check to restore it." >&2
  exit 1
fi

if [ "$mode" = install ]; then
  # The licence travels with the code. OpenCV's own LICENSE does not cover it.
  cat >"$dst/LICENSE" <<'EOF'
ImGuiColorTextEdit - a syntax highlighting text editor for Dear ImGui.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT>.

Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.

Derived from ImGuiColorTextEdit, originally by Balazs Jako (2017):
<https://github.com/BalazsJako/ImGuiColorTextEdit>, also MIT licensed.
EOF
  echo "wrote $dst/LICENSE"
fi