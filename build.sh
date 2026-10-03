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

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <test args>]

Build the OpenCV plan / plan+v4d stack.

Options:
  -t, --target TARGET    What to build: 'plan' or 'plan+v4d' (default: plan)
  -b, --build-type TYPE  Build configuration: release, debug, asan, ubsan, tsan
                         (default: debug)
  -j, --jobs N           Parallel build jobs (default: 4)
  -d, --dnn-backend NAME DNN inference backend: 'openvino' or 'opencv'
                         (default: openvino). 'openvino' compiles the OpenVINO
                         backend in and makes DNN_BACKEND_DEFAULT resolve to
                         DNN_BACKEND_INFERENCE_ENGINE; the device is still
                         chosen per net at runtime via setPreferableTarget().
  -r, --rebuild          Force a fresh cmake configure, discarding the current
                         build directory contents
  -h, --help             Show this help

Any arguments after '--' are passed through to the test binaries (only relevant
when target is 'plan', which builds and runs the plan tests).

Environment:
  OpenVINO_DIR   OpenVINO devel tree to build against, e.g.
                 /usr/lib64/cmake/OpenVINO (the default when unset). The runtime
                 plugins have to match: <libdir>/openvino-<version>/ must hold
                 libopenvino_intel_gpu_plugin.so for DNN_TARGET_OPENCL.

Examples:
  $(basename "$0")
  $(basename "$0") -t plan+v4d
  $(basename "$0") -t plan -b asan -j 8 -- --gtest_filter=Plan.*
  $(basename "$0") -r -d openvino
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
  plan|plan+v4d) ;;
  *) echo "Invalid target '$TARGET' (expected 'plan' or 'plan+v4d')" >&2; exit 1 ;;
esac

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
  -DINSTALL_BIN_EXAMPLES=OFF
  -DOPENCV_CUSTOM_PACKAGE_INFO=ON
  -DCPACK_PACKAGE_VERSION_MAJOR=4
  -DCPACK_PACKAGE_VERSION_MINOR=13
  -DCPACK_PACKAGE_VERSION_PATCH=0
  -DCPACK_PACKAGE_VERSION=4:13.0-beta-kallaballa
  -DCPACK_PACKAGE_CONTACT="you@example.com"
  -DOPENCV_GENERATE_PKGCONFIG=ON
  -DCPACK_PACKAGE_VENDOR=yourname
  -DCPACK_DEBIAN_PACKAGE_DEPENDS="libqt5opengl5,freeglut3,ocl-icd-libopencl1,libavcodec58,libavdevice58,libavfilter7,libavformat58,libavutil56,libpostproc55,libswresample3,libswscale5,libglfw3,libstb0,libglew2.2,zlib1g,libxinerama1,libxcursor1,libxi6,libva2,intel-opencl-icd,ca-certificates"
  -DINSTALL_CREATE_DISTRIB=ON
  -DCPACK_BINARY_DEB=ON
  -DCV_TRACE=OFF
  -DBUILD_SHARED_LIBS=ON
  -DWITH_OPENGL=ON
  -DOPENCV_ENABLE_EGL=ON
  -DOPENCV_ENABLE_EGL_INTEROP=ON
  -DOPENCV_ENABLE_GLX=ON
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
  -DWITH_VULKAN=ON
  # OpenCL is what both halves of "openvino (opencl)" need: WITH_OPENCL gives
  # opencv_core the OpenCL runtime, OPENCV_DNN_OPENCL (added by dnn_cmake_args)
  # turns on CV_OCL4DNN in opencv_dnn, and OpenVINO's own GPU device - the one
  # DNN_TARGET_OPENCL maps to - is an OpenCL device provided by libopenvino.
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

# DNN -> OpenVINO. Three independent switches have to agree for the DNN module to
# offer the OpenVINO OpenCL target, and each one fails silently on its own:
#
#   WITH_OPENVINO           find_package(OpenVINO) -> ocv.3rdparty.openvino
#   OPENCV_DNN_OPENVINO     compiles it into libopencv_dnn
#                           (HAVE_INF_ENGINE / HAVE_DNN_NGRAPH)
#   OPENCV_DNN_PLUGIN_LIST  left empty on purpose: that builds the backend *into*
#                           libopencv_dnn instead of a separate
#                           opencv_dnn_openvino*.so plugin, which is what
#                           DNN_BACKEND_INFERENCE_ENGINE needs to resolve.
#
# OPENCV_DNN_BACKEND_DEFAULT only moves DNN_BACKEND_DEFAULT; the inference
# *device* is still chosen per net at runtime via setPreferableTarget().
#
# Both variants deliberately leave OPENCV_DNN_BACKEND_DEFAULT at its stock value
# (DNN_BACKEND_OPENCV, i.e. 'opencv'). Making DNN_BACKEND_DEFAULT itself resolve
# to DNN_BACKEND_INFERENCE_ENGINE does not merely change the default: in OpenCV
# 5.x it makes every Net::Impl swap itself for a NetImplOpenVINO at construction
# time, which is what used to turn any later setPreferableTarget() into a
# Net::Impl::clear() that dropped the freshly imported graph. With the default
# left alone, setPreferableBackend(DNN_BACKEND_INFERENCE_ENGINE) is what opts a
# net into the OpenVINO whole-graph offload.
DNN_CMAKE_ARGS=(
  -DOPENCV_DNN_OPENCL=ON
)
case "$DNN_BACKEND" in
  openvino)
    DNN_CMAKE_ARGS+=(
      -DWITH_OPENVINO=ON
      -DOPENCV_DNN_OPENVINO=ON
      -DOPENCV_DNN_PLUGIN_LIST=
    )
    if [ -n "$OPENVINO_DIR_OVERRIDE" ]; then
      DNN_CMAKE_ARGS+=(-DOpenVINO_DIR="$OPENVINO_DIR_OVERRIDE")
    fi
    ;;
  opencv)
    DNN_CMAKE_ARGS+=(
      -DWITH_OPENVINO=OFF
      -DOPENCV_DNN_OPENVINO=OFF
    )
    ;;
esac
CMAKE_ARGS+=("${DNN_CMAKE_ARGS[@]}")

# OpenCV reports a missing OpenVINO as a plain "OpenVINO: NO" line and keeps
# going, so an unbuildable or mismatched installation is only discovered much
# later as "DNN_BACKEND_INFERENCE_ENGINE is not available". Check it here.
verify_dnn_backend() {
  local cache="$BUILD_DIR/CMakeCache.txt"
  local flags="$BUILD_DIR/modules/dnn/CMakeFiles/opencv_dnn.dir/flags.make"
  local failed=0

  if [ "$DNN_BACKEND" != openvino ]; then
    return 0
  fi

  if ! grep -q "OPENCV_MODULE_opencv_dnn_LINK_DEPS.*openvino" "$cache"; then
    echo "ERROR: opencv_dnn does not link OpenVINO (OpenVINO runtime not found by cmake)." >&2
    echo "       Install the OpenVINO *devel* package and the matching runtime" >&2
    echo "       plugins, then re-run with -r." >&2
    failed=1
  fi

  # The GPU/OpenCL target only shows up in getAvailableTargets() when OpenCV's
  # own OpenCL is compiled in and the default OpenCL device is an Intel one.
  if ! grep -q "CV_OCL4DNN=1" "$flags" 2>/dev/null; then
    echo "WARNING: opencv_dnn built without OpenCL (CV_OCL4DNN), so the" >&2
    echo "         DNN_TARGET_OPENCL / DNN_TARGET_OPENCL_FP16 targets will not" >&2
    echo "         be listed by cv::dnn::getAvailableTargets()." >&2
  fi

  verify_openvino_gpu_plugin || failed=1

  grep -E "^(OpenVINO_DIR|WITH_OPENVINO|OPENCV_DNN_OPENVINO|OPENCV_DNN_BACKEND_DEFAULT|OPENCV_DNN_OPENCL):" "$cache" \
    | sed 's/^/       /'

  [ "$failed" -eq 0 ]
}

# DNN_TARGET_OPENCL is served by OpenVINO's *GPU* device, which ov::Core only
# finds through the plugin next to the libopenvino.so that opencv_dnn links:
# <dir(libopenvino.so)>/openvino-<core version>/libopenvino_intel_gpu_plugin.so.
# A devel package whose plugins are a different version than its core enumerates
# zero devices, and then every OpenVINO target silently disappears from
# cv::dnn::getAvailableBackends() -- with no error anywhere in the build.
verify_openvino_gpu_plugin() {
  local link="$BUILD_DIR/modules/dnn/CMakeFiles/opencv_dnn.dir/link.txt"
  local ov_so ov_libdir ov_ver plugindir

  ov_so="$(tr ' ' '\n' < "$link" 2>/dev/null | grep -oE '/[^ ]*/libopenvino\.so[^ ]*' | head -1)"
  if [ -z "$ov_so" ]; then
    echo "ERROR: cannot read the libopenvino.so that opencv_dnn links from $link." >&2
    return 1
  fi
  ov_so="$(readlink -f "$ov_so")"
  ov_libdir="$(dirname "$ov_so")"
  ov_ver="$(basename "$ov_so" | sed -n 's/^libopenvino\.so\.\([0-9][0-9.]*\)$/\1/p')"

  if [ -z "$ov_ver" ]; then
    echo "WARNING: $ov_so carries no version in its soname, cannot locate the" >&2
    echo "         OpenVINO plugin directory next to it." >&2
    return 0
  fi

  plugindir="$ov_libdir/openvino-$ov_ver"
  if [ ! -d "$plugindir" ]; then
    echo "ERROR: no $plugindir -- the OpenVINO $ov_ver core cannot load any device plugin." >&2
    for d in "$ov_libdir"/openvino-*; do
      [ -d "$d" ] && echo "       (found instead: $d)" >&2
    done
    echo "       Install the runtime plugins matching the devel package, e.g." >&2
    echo "         sudo zypper install openvino-intel-gpu-plugin=$ov_ver openvino-intel-cpu-plugin=$ov_ver" >&2
    return 1
  fi

  if [ ! -e "$plugindir/libopenvino_intel_gpu_plugin.so" ]; then
    echo "ERROR: $plugindir has no libopenvino_intel_gpu_plugin.so, so OpenVINO" >&2
    echo "       has no GPU device and DNN_TARGET_OPENCL cannot be used." >&2
    echo "         sudo zypper install openvino-intel-gpu-plugin=$ov_ver" >&2
    return 1
  fi

  echo "       OpenVINO GPU (OpenCL) plugin: $plugindir/libopenvino_intel_gpu_plugin.so"
}

echo "==> Building target '${TARGET}' (build type: ${BUILD_TYPE}, dnn backend: ${DNN_BACKEND}) in ${BUILD_DIR}"
cd "$BUILD_DIR"

if [ "$REBUILD" = 1 ]; then
  cmake --fresh "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
else
  cmake "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
fi

verify_dnn_backend

if [ "$DNN_BACKEND" = openvino ]; then
  echo "==> DNN note: the inference *device* is selected per net at runtime, not by cmake --"
  echo "    net.setPreferableBackend(cv::dnn::DNN_BACKEND_INFERENCE_ENGINE);"
  echo "    net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);"
  echo "    Verify what this build really offers: ./check-dnn-openvino.sh"
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
