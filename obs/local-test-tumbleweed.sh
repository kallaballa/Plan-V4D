#!/bin/bash
set -euo pipefail

# local-test-tumbleweed.sh — Build Plan-V4D RPMs locally inside an openSUSE Tumbleweed container
#
# This simulates what OBS would do, useful for testing the spec file before uploading.
#
# Usage:
#   ./local-test-tumbleweed.sh           # full build
#   ./local-test-tumbleweed.sh --shell   # drop into build shell for debugging

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONTAINER="opensuse/tumbleweed"

echo "=== Local Test Build: openSUSE Tumbleweed ==="
echo "Project root: $PROJECT_DIR"
echo ""

# ---- Ensure sources are available ----
OPENCV_SRC="/tmp/obs-test-opencv-$$"
PLANV4D_SRC="/tmp/obs-test-plan-v4d-$$"
cleanup() { rm -rf "$OPENCV_SRC" "$PLANV4D_SRC"; }
trap cleanup EXIT

echo "--- Fetching sources ---"
if [[ ! -d "$OPENCV_SRC" ]]; then
    git clone --depth 1 --branch GCV https://github.com/kallaballa/opencv.git "$OPENCV_SRC"
fi
if [[ ! -d "$PLANV4D_SRC" ]]; then
    git clone --depth 1 --branch rollback https://github.com/kallaballa/Plan-V4D.git "$PLANV4D_SRC"
fi

# ---- Version ----
VERSION="4.13.0~beta~kallaballa"

# ---- Prepare source tarballs ----
TARBALL_DIR="/tmp/obs-test-tarballs-$$"
mkdir -p "$TARBALL_DIR"
(cd "$OPENCV_SRC" && tar czf "$TARBALL_DIR/opencv-${VERSION}.tar.gz" --transform "s,^.,opencv-${VERSION}," .)
(cd "$PLANV4D_SRC" && tar czf "$TARBALL_DIR/plan-v4d-${VERSION}.tar.gz" --transform "s,^.,plan-v4d-${VERSION}," .)

# ---- Build in container ----
if [[ "${1:-}" == "--shell" ]]; then
    echo "--- Dropping into Tumbleweed shell ---"
    podman run --rm -it \
        -v "$TARBALL_DIR:/sources:ro" \
        -v "$SCRIPT_DIR:/spec:ro" \
        -v "$PROJECT_DIR:/project:ro" \
        -w /work \
        "$CONTAINER" \
        bash -c '
            echo "=== Install build dependencies ==="
            zypper --non-interactive install \
                cmake gcc-c++ make git-core pkgconfig \
                opencl-headers libglfw-devel libGL-devel libGLU-devel libGLEW-devel \
                zlib-devel libpng-devel libjpeg-devel libva-devel \
                libXinerama-devel libXcursor-devel libXi-devel \
                libqt5-qtbase-devel ffmpeg-7-devel ocl-icd-devel \
                fdupes rpm-build && \
            mkdir -p /work && cd /work && \
            rpmbuild -bp /spec/plan-v4d.spec \
                --define "_sourcedir /sources" \
                --define "_topdir /work/rpmbuild" && \
            echo "=== Source extracted. Use: rpmbuild -bc /spec/plan-v4d.spec ===" && \
            exec bash
        '
else
    echo "--- Building RPMs in Tumbleweed container ---"
    podman run --rm \
        -v "$TARBALL_DIR:/sources:ro" \
        -v "$SCRIPT_DIR:/spec:ro" \
        "$CONTAINER" \
        bash -c '
            set -ex
            echo "=== Installing build dependencies ==="
            zypper --non-interactive install \
                cmake gcc-c++ make git-core pkgconfig \
                opencl-headers libglfw-devel libGL-devel libGLU-devel libGLEW-devel \
                zlib-devel libpng-devel libjpeg-devel libva-devel \
                libXinerama-devel libXcursor-devel libXi-devel \
                libqt5-qtbase-devel ffmpeg-7-devel ocl-icd-devel \
                fdupes rpm-build

            mkdir -p /work && cd /work

            echo "=== Building RPM ==="
            rpmbuild -ba /spec/plan-v4d.spec \
                --define "_sourcedir /sources" \
                --define "_topdir /work/rpmbuild"

            echo ""
            echo "=== Build Complete ==="
            find /work/rpmbuild -name "*.rpm" -exec ls -lh {} \;
        '
fi

rm -rf "$TARBALL_DIR"
