#!/bin/bash
# Build the V4D/Plan OpenCV stack as WebAssembly.
#
# Usage: build-wasm.sh [configure|build|all]
set -e

BUILD_DIR="${BUILD_DIR:-/home/elchaschab/devel/opencv/build_wasm}"
OPENCV_SRC="${OPENCV_SRC:-/home/elchaschab/devel/opencv}"
EXTRA_MODULES="${EXTRA_MODULES:-/home/elchaschab/devel/Plan-V4D/modules}"
JOBS="${JOBS:-12}"

if [ -z "$EMSDK" ]; then
  for f in "$HOME/emsdk/emsdk_env.sh"; do
    [ -f "$f" ] && set +u && source "$f" >/dev/null && set -u && break
  done
fi
[ -n "$EMSDK" ] || { echo "emsdk not found" >&2; exit 1; }

CMD="${1:-all}"
mkdir -p "$BUILD_DIR"

COMMON_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
  -DOPENCV_EXTRA_MODULES_PATH="$EXTRA_MODULES"
  -DBUILD_opencv_v4d=ON
  -DBUILD_opencv_plan=ON
  -DOPENCV_V4D_ENABLE_ES3=ON
  -DBUILD_EXAMPLES=ON
  -DBUILD_TESTS=OFF
  -DBUILD_PERF_TESTS=OFF
  -DBUILD_DOCS=OFF
  -DBUILD_PACKAGE=OFF
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_C_FLAGS="-msimd128 -pthread -sUSE_GLFW=3"
  -DCMAKE_CXX_FLAGS="-msimd128 -pthread -sUSE_GLFW=3"
  -DCMAKE_EXE_LINKER_FLAGS="-sUSE_GLFW=3 -sNO_EXIT_RUNTIME=1 -sPTHREAD_POOL_SIZE=4 -sMAX_WEBGL_VERSION=2 -sINITIAL_MEMORY=268435456"
  -DBUILD_opencv_java=OFF
  -DBUILD_opencv_python3=OFF
  -DBUILD_opencv_python2=OFF
  -DBUILD_opencv_apps=OFF
  -DBUILD_opencv_js=OFF
  -DWITH_FFMPEG=OFF
  -DWITH_OPENCL=OFF
  -DWITH_OPENCLAMDFFT=OFF
  -DWITH_OPENCLAMDBLAS=OFF
  -DWITH_OPENCL_SVM=OFF
  -DWITH_QT=OFF
  -DWITH_GTK=OFF
  -DWITH_1394=OFF
  -DWITH_ADE=OFF
  -DWITH_VTK=OFF
  -DWITH_EIGEN=OFF
  -DWITH_TBB=OFF
  -DWITH_IPP=OFF
  -DWITH_JASPER=OFF
  -DWITH_WEBP=OFF
  -DWITH_OPENEXR=OFF
  -DWITH_OPENVX=OFF
  -DWITH_LAPACK=OFF
  -DWITH_ITT=OFF
  -DWITH_JPEG=ON
  -DWITH_PNG=ON
  -DBUILD_opencv_calib3d=ON
  -DBUILD_opencv_core=ON
  -DBUILD_opencv_dnn=ON
  -DBUILD_opencv_features2d=ON
  -DBUILD_opencv_flann=ON
  -DBUILD_opencv_imgcodecs=ON
  -DBUILD_opencv_imgproc=ON
  -DBUILD_opencv_objdetect=ON
  -DBUILD_opencv_optflow=ON
  -DBUILD_opencv_photo=ON
  -DBUILD_opencv_plot=ON
  -DBUILD_opencv_signal=ON
  -DBUILD_opencv_stitching=ON
  -DBUILD_opencv_tracking=ON
  -DBUILD_opencv_video=ON
  -DBUILD_opencv_videoio=ON
  -DBUILD_opencv_ximgproc=ON
  -DBUILD_opencv_face=ON
)

case "$CMD" in
  configure)
    ( cd "$BUILD_DIR" && emcmake cmake "${COMMON_ARGS[@]}" "$OPENCV_SRC" )
    ;;
  build)
    ( cd "$BUILD_DIR" && emmake make -j"$JOBS" )
    ;;
  all)
    ( cd "$BUILD_DIR" && emcmake cmake "${COMMON_ARGS[@]}" "$OPENCV_SRC" )
    ( cd "$BUILD_DIR" && emmake make -j"$JOBS" )
    ;;
  *)
    echo "usage: $0 [configure|build|all]" >&2
    exit 2
    ;;
esac
