#!/bin/bash
set -euo pipefail

# local-test-ubuntu.sh -- Build Plan-V4D .deb packages locally inside an Ubuntu 24.04 LTS container
#
# This simulates what OBS would do, useful for testing the debian packaging before uploading.
#
# Usage:
#   ./local-test-ubuntu.sh           # full build
#   ./local-test-ubuntu.sh --shell   # drop into build shell for debugging

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONTAINER="ubuntu:24.04"

echo "=== Local Test Build: Ubuntu 24.04 LTS ==="
echo "Project root: $PROJECT_DIR"
echo ""

# ---- Ensure sources are available ----
OPENCV_SRC="/tmp/obs-test-opencv-$$"
PLANV4D_SRC="/tmp/obs-test-plan-v4d-$$"
cleanup() { rm -rf "$OPENCV_SRC" "$PLANV4D_SRC" "$TARBALL_DIR"; }
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
(cd "$OPENCV_SRC" && tar czf "$TARBALL_DIR/opencv_4.13.0.orig.tar.gz" --transform "s,^.,opencv-${VERSION}," .)
(cd "$PLANV4D_SRC" && tar czf "$TARBALL_DIR/plan-v4d_4.13.0.orig.tar.gz" --transform "s,^.,Plan-V4D-${VERSION}," .)

# ---- Build in container ----
if [[ "${1:-}" == "--shell" ]]; then
    echo "--- Dropping into Ubuntu shell ---"
    podman run --rm -it \
        -v "$TARBALL_DIR:/sources:ro" \
        -v "$SCRIPT_DIR:/pkg:ro" \
        -v "$PROJECT_DIR:/project:ro" \
        -w /work \
        "$CONTAINER" \
        bash -c '
            apt-get update && \
            DEBIAN_FRONTEND=noninteractive apt-get -y install \
                debhelper cmake g++ make pkg-config git \
                opencl-headers zlib1g-dev libpng-dev libjpeg-dev libva-dev \
                libxinerama-dev libxcursor-dev libxi-dev \
                libgl1-mesa-dev libglu1-mesa-dev libglew-dev libglfw3-dev \
                qtbase5-dev ocl-icd-opencl-dev && \
            mkdir -p /work && cd /work && \
            cp -r /pkg/plan-v4d* /work/ && \
            echo "=== Ready. Run dpkg-buildpackage in the plan-v4d dir. ===" && \
            exec bash
        '
else
    echo "--- Building .debs in Ubuntu container ---"
    podman run --rm \
        -v "$TARBALL_DIR:/sources:ro" \
        -v "$SCRIPT_DIR:/pkg:ro" \
        "$CONTAINER" \
        bash -c '
            set -ex
            echo "=== Installing build dependencies ==="
            apt-get update
            DEBIAN_FRONTEND=noninteractive apt-get -y install \
                debhelper cmake g++ make pkg-config git \
                opencl-headers zlib1g-dev libpng-dev libjpeg-dev libva-dev \
                libxinerama-dev libxcursor-dev libxi-dev \
                libgl1-mesa-dev libglu1-mesa-dev libglew-dev libglfw3-dev \
                qtbase5-dev ocl-icd-opencl-dev

            mkdir -p /work && cd /work

            echo "=== Extracting sources ==="
            tar xzf /sources/opencv_4.13.0.orig.tar.gz
            tar xzf /sources/plan-v4d_4.13.0.orig.tar.gz

            echo "=== Preparing debian packaging ==="
            cp -r /pkg/plan-v4d /work/plan-v4d-source

            # Build the combined source tree: OpenCV is the upstream package,
            # with Plan-V4D modules as extra modules.
            cd /work/plan-v4d-source
            mv ../opencv-${VERSION} opencv-${VERSION} 2>/dev/null || true
            mkdir -p extra_modules
            cp -a ../Plan-V4D-${VERSION}/modules/* extra_modules/ 2>/dev/null || true
            rm -rf ../Plan-V4D-${VERSION}

            echo "=== Building deb ==="
            dh_auto_configure \
            && dh_auto_build \
            && dh_auto_install \
            && dpkg-buildpackage -us -uc -b

            echo ""
            echo "=== Build Complete ==="
            ls -lh /work/*.deb
        '
fi

rm -rf "$TARBALL_DIR"
