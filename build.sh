#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENCV_DIR="$(dirname "$SCRIPT_DIR")/Plan-V4D/opencv"
BUILD_DIR="$OPENCV_DIR/build"
BUILD_MARKER="$BUILD_DIR/.build-type"
JOBS=4
TARGET=plan
BUILD_TYPE=debug
REBUILD=
TEST_ARGS=
DNN_BACKEND=openvino
ANDROID_ABI=arm64-v8a
ANDROID_API_LEVEL=32
ANDROID_PACKAGE=0
ANDROID_CONFIGURE_ONLY=0
ANDROID_DEMO=all

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <test args>]

Build the OpenCV plan / plan+v4d stack, or the V4D demos for Android.

Options:
  -t, --target TARGET    What to build: 'plan', 'plan+v4d' or 'android'
                         (default: plan)
  -b, --build-type TYPE  Build configuration: release, debug, asan, ubsan, tsan
                         (default: debug)
  -j, --jobs N           Parallel build jobs (default: 4)
  -d, --dnn-backend NAME DNN inference backend: 'openvino' or 'opencv'
                         (default: openvino). 'openvino' compiles the OpenVINO
                         backend into libopencv_dnn and makes cv::dnn register
                         the OpenVINO 'GPU' (OpenCL) device. Note that on
                         OpenCV 5.x this only applies to models loaded as
                         OpenVINO IR -- see the note below.
  -r, --rebuild          Force a fresh cmake configure, discarding the current
                         build directory contents
  -h, --help             Show this help

Android options (only meaningful with --target android; everything below is
handed to tools/android-build.sh, which owns the NDK settings):

      --abi ABI          Android ABI: arm64-v8a, armeabi-v7a or x86_64
                         (default: arm64-v8a)
      --api-level N      Android API level to compile against (default: 32)
      --demo NAME        Which demos to build and package (default: all);
                         a comma separated list of sample names, or 'all' for
                         every sample that has an Android path (see
                         tools/android/sample-list.sh)
      --configure-only   Configure, but do not compile
      --apk              Also assemble a signed APK with
                         tools/android/package-apk.sh

Any arguments after '--' are passed through to the test binaries (only relevant
when target is 'plan', which builds and runs the plan tests).

Environment:
  OpenVINO_DIR   OpenVINO devel tree to build against, e.g.
                 /usr/lib64/cmake/OpenVINO (the default when unset). The runtime
                 plugins have to match: <libdir>/openvino-<version>/ must hold
                 libopenvino_intel_gpu_plugin.so for DNN_TARGET_OPENCL.
  ANDROID_SDK_ROOT / ANDROID_HOME  Android SDK location (default: ~/Android/Sdk)
  ANDROID_API_LEVEL / ANDROID_ABI  Defaults for the Android options above

Examples:
  $(basename "$0")
  $(basename "$0") -t plan+v4d
  $(basename "$0") -t plan -b asan -j 8 -- --gtest_filter=Plan.*
  $(basename "$0") -r -d openvino
  $(basename "$0") -t android --apk --demo font_rendering
  $(basename "$0") -t android --abi x86_64 --configure-only
EOF
  exit 0
}

while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in
    -t|--target)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      TARGET="$2"; shift 2 ;;
    -b|--build-type)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      BUILD_TYPE="$2"; shift 2 ;;
    -j|--jobs)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      JOBS="$2"; shift 2 ;;
    -d|--dnn-backend)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      DNN_BACKEND="$2"; shift 2 ;;
    --abi)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_ABI="$2"; shift 2 ;;
    --api-level)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_API_LEVEL="$2"; shift 2 ;;
    --demo)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_DEMO="$2"; shift 2 ;;
    --apk)
      ANDROID_PACKAGE=1; shift ;;
    --configure-only)
      ANDROID_CONFIGURE_ONLY=1; shift ;;
    -r|--rebuild)
      REBUILD=1; shift ;;
    -h|--help)
      usage ;;
    *)
      echo "Unknown argument: $1" >&2
      usage ;;
  esac
done
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
TEST_ARGS="$*"

case "$TARGET" in
  plan|plan+v4d|android) ;;
  *) echo "Invalid target '$TARGET' (expected 'plan', 'plan+v4d' or 'android')" >&2; exit 1 ;;
esac

# --- Android -----------------------------------------------------------------
# Delegated wholesale to tools/android-build.sh rather than reimplemented here.
# The host build below assumes an X11/Wayland desktop, OpenVINO, FFmpeg with
# VAAPI and `sudo make install` into /usr/local -- none of which exists on a
# device, and every one of which would fight the NDK toolchain if it were tried.
# The two share nothing but this argument parsing.
if [ "$TARGET" = android ]; then
  # 'all' is expanded here rather than in the sub-scripts so that the same list
  # drives the build (one cmake option) and the packaging loop (one APK each).
  if [ "$ANDROID_DEMO" = all ]; then
    ANDROID_DEMO="$("$SCRIPT_DIR/tools/android/sample-list.sh")"
    echo "==> Demos: all ($(tr ',' '\n' <<< "$ANDROID_DEMO" | wc -l) samples)"
  fi
  ANDROID_ARGS=(--abi "$ANDROID_ABI" --api-level "$ANDROID_API_LEVEL" -j "$JOBS")
  [ "$ANDROID_CONFIGURE_ONLY" = 1 ] && ANDROID_ARGS+=(--configure-only)
  [ "$REBUILD" = 1 ] && ANDROID_ARGS+=(--clean)
  # The demo list is a configure-time option, so it has to be set before the
  # configure rather than passed as an argument.
  export OPENCV_V4D_SAMPLES="$ANDROID_DEMO"
  "$SCRIPT_DIR/tools/android-build.sh" "${ANDROID_ARGS[@]}"
  # An --apk run of a configure-only build would package whatever was left in
  # the staging directory, which is at best stale.
  if [ "$ANDROID_PACKAGE" = 1 ] && [ "$ANDROID_CONFIGURE_ONLY" != 1 ]; then
    # --demo is a comma list (that is the form OPENCV_V4D_SAMPLES takes, and the
    # build above just consumed it as one), but packaging is a per-demo step, so
    # split it back out. `read -ra` with a one-shot IFS is used rather than
    # saving/restoring IFS around a `for` loop: IFS is global state, and the
    # build above runs arbitrary commands (which may reset or export it), so a
    # loop body that assumed the split had happened hands package-apk.sh one
    # "demo name" that is the whole list separated by spaces -- and the error it
    # produces, "libv4ddemo_a b c.so not found", points at the wrong place.
    IFS=',' read -r -a ANDROID_DEMOS <<< "$ANDROID_DEMO"
    for demo in "${ANDROID_DEMOS[@]}"; do
      demo="$(printf '%s' "$demo" | tr -d '[:space:]')"
      [ -n "$demo" ] || continue
      "$SCRIPT_DIR/tools/android/package-apk.sh" --demo "$demo" --abi "$ANDROID_ABI" \
        --api-level "$ANDROID_API_LEVEL"
    done
  fi
  exit 0
fi

case "$DNN_BACKEND" in
  openvino|opencv) ;;
  *) echo "Invalid dnn backend '$DNN_BACKEND' (expected 'openvino' or 'opencv')" >&2; exit 1 ;;
esac

CMAKE_BUILD_TYPE=Debug
C_FLAGS=
CXX_FLAGS="-DCL_TARGET_OPENCL_VERSION=120"
EXE_LINKER_FLAGS=
SHARED_LINKER_FLAGS=

# Optional: point cmake at a specific OpenVINO devel tree, e.g.
#   OpenVINO_DIR=/opt/intel/openvino_2026/runtime/lib/cmake/ov ./build.sh -r
# Left empty, OpenCV's find_package(OpenVINO) picks up whatever is installed.
OPENVINO_DIR_OVERRIDE="${OpenVINO_DIR:-}"

case "$BUILD_TYPE" in
  release)
    CMAKE_BUILD_TYPE=Release ;;
  debug)
    CMAKE_BUILD_TYPE=Debug ;;
  relwithdeb)
    CMAKE_BUILD_TYPE=ReleaseWithDebInfo ;;
  asan)  SAN="-fsanitize=address" ;;
  ubsan) SAN="-fsanitize=undefined" ;;
  tsan)  SAN="-fsanitize=thread" ;;
  *)
    echo "Invalid build type '$BUILD_TYPE' (expected release, debug, asan, ubsan or tsan)" >&2
    exit 1 ;;
esac

if [ -n "${SAN:-}" ]; then
  C_FLAGS="$SAN -fno-omit-frame-pointer"
  CXX_FLAGS="$CXX_FLAGS $SAN -fno-omit-frame-pointer"
  EXE_LINKER_FLAGS="$SAN"
  SHARED_LINKER_FLAGS="$SAN"
fi

if [ ! -d "$OPENCV_DIR" ]; then
  $(cd $SCRIPT_DIR; git clone git@github.com:kallaballa/opencv.git)
fi

if [ -f "$BUILD_MARKER" ] && [ "$(cat "$BUILD_MARKER")" != "$BUILD_TYPE" ]; then
  echo "Build dir was configured as '$(cat "$BUILD_MARKER")', reconfiguring for '$BUILD_TYPE'"
  REBUILD=1
fi

if [ "$REBUILD" = 1 ] || [ ! -d "$BUILD_DIR" ]; then
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
fi

CMAKE_ARGS=(
  -DCMAKE_POLICY_VERSION_MINIMUM=3.24
  -DWITH_WAYLAND=ON
  -DOPENCV_V4D_ENABLE_ES3=OFF
  -DOPENCV_V4D_ENABLE_BGFX=OFF
  -DOPENCV_ALGO_HINT_DEFAULT=ALGO_HINT_APPROX
  -DCMAKE_MODULE_LINKER_FLAGS="/usr/local/lib64/"
  -DINSTALL_BIN_EXAMPLES=OM
  -DENABLE_LTO=ON
  -DOPENCV_GENERATE_PKGCONFIG=ON
  -DINSTALL_CREATE_DISTRIB=ON
  -DCV_TRACE=OFF
  -DBUILD_SHARED_LIBS=ON
  -DWITH_OPENGL=ON
  -DOPENCV_ENABLE_EGL_INTEROP=ON
  -DOPENCV_ENABLE_GLX_INTEROP=ON
  -DOPENCV_FFMPEG_ENABLE_LIBAVDEVICE=ON
  -DBUILD_HARFBUZZ=ON
  -DWITH_FFMPEG=ON
  -DOPENCV_FFMPEG_SKIP_BUILD_CHECK=ON
  -DWITH_VA=ON
  -DWITH_VA_INTEL=ON
  -DWITH_1394=OFF
  -DWITH_ADE=OFF
  -DWITH_VTK=OFF
  -DWITH_EIGEN=OFF
  -DWITH_GTK=OFF
  -DWITH_GTK_2_X=OFF
  -DWITH_IPP=OFF
  -DWITH_JASPER=OFF
  -DWITH_WEBP=OFF
  -DWITH_OPENEXR=OFF
  -DWITH_OPENVX=OFF
  -DWITH_OPENNI=OFF
  -DWITH_OPENNI2=OFF
  -DWITH_TBB=OFF
  -DWITH_TIFF=OFF
  -DWITH_VULKAN=OFF
  -DWITH_OPENCL=ON
  -DWITH_OPENCL_SVM=ON
  -DWITH_OPENCLAMDFFT=OFF
  -DWITH_OPENCLAMDBLAS=OFF
  -DWITH_GPHOTO2=OFF
  -DWITH_LAPACK=OFF
  -DWITH_ITT=OFF
  -DWITH_QUIRC=ON
  -DBUILD_ZLIB=OFF
  -DBUILD_opencv_apps=OFF
  -DBUILD_opencv_calib3d=ON
  -DBUILD_opencv_ccalib=ON
  -DBUILD_opencv_dnn=ON
  -DBUILD_opencv_features2d=ON
  -DBUILD_opencv_flann=ON
  -DBUILD_opencv_gapi=OFF
  -DBUILD_opencv_ml=OFF
  -DBUILD_opencv_photo=ON
  -DBUILD_opencv_shape=OFF
  -DBUILD_opencv_imgcodecs=ON
  -DBUILD_opencv_superres=OFF
  -DBUILD_opencv_videoio=ON
  -DBUILD_opencv_videostab=OFF
  -DBUILD_opencv_stitching=ON
  -DBUILD_opencv_java=OFF
  -DBUILD_opencv_js=OFF
  -DBUILD_opencv_python2=OFF
  -DBUILD_opencv_python3=OFF
  -DBUILD_opencv_alphamat=OFF
  -DBUILD_opencv_aruco=OFF
  -DBUILD_opencv_barcode=OFF
  -DBUILD_opencv_bgsegm=OFF
  -DBUILD_opencv_bioinspired=OFF
  -DBUILD_opencv_cnn_3dobj=OFF
  -DBUILD_opencv_cudaarithm=OFF
  -DBUILD_opencv_cudabgsegm=OFF
  -DBUILD_opencv_cudacodec=OFF
  -DBUILD_opencv_cudafeatures2d=OFF
  -DBUILD_opencv_cudafilters=OFF
  -DBUILD_opencv_cudaimgproc=OFF
  -DBUILD_opencv_cudalegacy=OFF
  -DBUILD_opencv_cudaobjdetect=OFF
  -DBUILD_opencv_cudaoptflow=OFF
  -DBUILD_opencv_cudastereo=OFF
  -DBUILD_opencv_cudawarping=OFF
  -DBUILD_opencv_cudev=OFF
  -DBUILD_opencv_cvv=OFF
  -DBUILD_opencv_datasets=OFF
  -DBUILD_opencv_dnn_objdetect=OFF
  -DBUILD_opencv_dnns_easily_fooled=OFF
  -DBUILD_opencv_dnn_superres=OFF
  -DBUILD_opencv_dpm=OFF
  -DBUILD_opencv_face=ON
  -DBUILD_opencv_freetype=OFF
  -DBUILD_opencv_fuzzy=OFF
  -DBUILD_opencv_hdf=OFF
  -DBUILD_opencv_hfs=OFF
  -DBUILD_opencv_img_hash=OFF
  -DBUILD_opencv_intensity_transform=OFF
  -DBUILD_opencv_julia=OFF
  -DBUILD_opencv_line_descriptor=OFF
  -DBUILD_opencv_matlab=OFF
  -DBUILD_opencv_mcc=OFF
  -DBUILD_opencv_optflow=ON
  -DBUILD_opencv_ovis=OFF
  -DBUILD_opencv_phase_unwrapping=OFF
  -DBUILD_opencv_plot=ON
  -DBUILD_opencv_quality=OFF
  -DBUILD_opencv_rapid=OFF
  -DBUILD_opencv_reg=OFF
  -DBUILD_opencv_rgbd=OFF
  -DBUILD_opencv_saliency=OFF
  -DBUILD_opencv_sfm=OFF
  -DBUILD_opencv_structured_light=OFF
  -DBUILD_opencv_surface_matching=OFF
  -DBUILD_opencv_text=OFF
  -DBUILD_opencv_tracking=ON
  -DBUILD_opencv_viz=OFF
  -DBUILD_opencv_wechat_qrcode=OFF
  -DBUILD_opencv_xfeatures2d=OFF
  -DBUILD_opencv_ximgproc=ON
  -DBUILD_opencv_xphoto=OFF
  -DBUILD_opencv_world=OFF
  -DBUILD_EXAMPLES=ON
  -DBUILD_PACKAGE=ON
  -DBUILD_DOCS=OFF
  -DWITH_PTHREADS_PF=ON
  -DCV_ENABLE_INTRINSICS=ON
  -DBUILD_opencv_video=ON
  -DBUILD_opencv_plan=ON
  -DBGFX_CONFIG_MULTITHREADED=ON
  -DBGFX_CONFIG_PASSIVE=ON
  -DOPENCV_DNN_OPENVINO=ON
  -DOPENCV_DNN_TFLITE=OFF
  -DOPENCV_DNN_OPENCL=ON
  -DWITH_ONNXRUNTIME=ON
  -DDOWNLOAD_ONNXRUNTIME=ON
  -DDOWNLOAD_ONNXRUNTIME_GPU=ON
  -DOPENCV_EXTRA_MODULES_PATH="$SCRIPT_DIR/modules"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
)

if [ "$TARGET" = plan+v4d ]; then
  CMAKE_ARGS+=(
    -DWITH_QT=OFF
    -DBUILD_TESTS=OFF
    -DBUILD_PERF_TESTS=OFF
    -DBUILD_opencv_highgui=OFF
    -DBUILD_opencv_geometry=ON
    -DBUILD_opencv_stereo=ON
    -DBUILD_opencv_xobjdetect=ON
    -DBUILD_opencv_v4d=ON
  )
else
  CMAKE_ARGS+=(
    -DWITH_QT=ON
    -DOPENCV_BUILD_TEST_MODULES_LIST=plan
    -DOPENCV_BUILD_PERF_TEST_MODULES_LIST=plan
    -DBUILD_TESTS=ON
    -DBUILD_PERF_TESTS=ON
    -DBUILD_opencv_highgui=ON
    -DBUILD_opencv_stereo=OFF
    -DBUILD_opencv_xobjdetect=OFF
    -DBUILD_opencv_v4d=OFF
  )
fi

if [ -n "$C_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_C_FLAGS="$C_FLAGS")
fi
if [ -n "$EXE_LINKER_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="$EXE_LINKER_FLAGS")
fi
if [ -n "$SHARED_LINKER_FLAGS" ]; then
  CMAKE_ARGS+=(-DCMAKE_SHARED_LINKER_FLAGS="$SHARED_LINKER_FLAGS")
fi
CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="$CXX_FLAGS")

echo "==> Building target '${TARGET}' (build type: ${BUILD_TYPE} in ${BUILD_DIR}"
cd "$BUILD_DIR"

if [ "$REBUILD" = 1 ]; then
  cmake --fresh "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
else
  cmake "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
fi

echo "$BUILD_TYPE" > "$BUILD_MARKER"

if [ "$TARGET" = plan+v4d ]; then
  make -j"$JOBS" && sudo make install
else
  make -j"$JOBS" opencv_test_plan opencv_perf_plan
  if [ -x ./bin/opencv_test_plan ]; then
    ./bin/opencv_test_plan $TEST_ARGS
  else
    ./opencv_test_plan $TEST_ARGS
  fi
  if [ -x ./bin/opencv_perf_plan ]; then
    ./bin/opencv_perf_plan $TEST_ARGS
  else
    ./opencv_perf_plan $TEST_ARGS
  fi
fi
