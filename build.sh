#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENCV_DIR="$(dirname "$SCRIPT_DIR")/Plan-V4D/opencv"
BUILD_DIR="$OPENCV_DIR/build"
BUILD_MARKER="$BUILD_DIR/.build-config"
JOBS=4
TARGET=plan
TARGET_SET=0
BUILD_TYPE=debug
REBUILD=
TEST_ARGS=
DNN_BACKEND=openvino
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_API_LEVEL="${ANDROID_API_LEVEL:-32}"
ANDROID_PACKAGE=0
ANDROID_CONFIGURE_ONLY=0
ANDROID_DEMO=all
ANDROID_DEMO_SET=0
ANDROID_SERIAL="${ANDROID_SERIAL:-}"
ANDROID_LOG=0
ANDROID_WAIT=12
ANDROID_SETTLE=12
ANDROID_KEEP_LOG=0
SDK_NDK_VERSION=
SDK_BUILD_TOOLS=
SDK_PLATFORM=
SDK_MINIMAL=0

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [options] [-- <test args>]

Build the OpenCV plan / plan+v4d stack, the V4D demos for Android, and deploy
them to a device.

Commands:
  plan            Configure, build and run the plan tests (the default when no
                  command is given)
  plan+v4d        Configure, build and install the plan+v4d stack
                  ('v4d' is accepted as a shorthand)
  android         Cross-compile the V4D demos for Android
  deploy          Install a packaged demo APK on a device and start it
                  (tools/android/run-demo.sh)
  debug           Install and start a demo, then capture its logcat, a
                  screencap and the device's GL properties
                  (tools/android/debug-device.sh)
  smoke           Install, launch and judge every packaged demo on the device
                  (tools/android/smoke-test.sh)
  sdk             Install the Android SDK/NDK pieces the build needs
                  (tools/android/install-sdk.sh)
  devices         List the attached devices and emulators plus the SDK/NDK
                  the build would use (tools/android/devices.sh)
  configure       Configure the build directory, nothing else
  build           Compile an already-configured build directory
  test            Run the plan test and perf binaries
  install         Build (if needed) then install a built tree
  clean           Remove the build directory

Options:
  -t, --target TARGET  Module set for configure/build/test/install/clean:
                       'plan' or 'plan+v4d' (default: plan). The 'plan' and
                       'plan+v4d' commands name the module set themselves and
                       reject this option.
  -b, --build-type TYPE  Build configuration: release, debug, relwithdeb,
                         asan, ubsan, tsan (default: debug)
  -j, --jobs N           Parallel build jobs (default: 4)
  -d, --dnn-backend NAME DNN inference backend: 'openvino' or 'opencv'
                         (default: openvino). Parsed and validated for
                         forward-compatible scripting only: on OpenCV 5.x the
                         OpenVINO backend is compiled in unconditionally, so this
                         option currently selects nothing. See the "DNN on the
                         GPU" section of the top-level README.
  -r, --rebuild          Force a fresh cmake configure, discarding the current
                         build directory contents
  -h, --help             Show this help

Android options:

      --abi ABI          Android ABI: arm64-v8a, armeabi-v7a or x86_64
                         (default: arm64-v8a)
      --api-level N      Android API level to compile against (default: 32)
      --demo NAME        Which demos to build and package (default: all);
                         a comma separated list of sample names, or 'all' for
                         every sample that has an Android path (see
                          tools/android/sample-list.sh). 'deploy' and 'debug'
                          take a single sample only (default: font_rendering);
                          'smoke' takes a list.
      --configure-only   Configure, but do not compile
      --apk              Also assemble a signed APK with
                         tools/android/package-apk.sh
      --serial SERIAL    Talk to this device only (exported as ANDROID_SERIAL);
                         when it is unset exactly one device has to be attached
      --log              'deploy': keep following the demo's logcat
                         after it starts
      --wait N           'debug': seconds to wait for the demo to start
                         before capturing (default: 12)
      --settle N         'smoke': seconds to give each demo to come up
                         (default: 12)
      --keep-log         'smoke': keep each demo's app log under
                         build/android/smoke-logs/

Android SDK options (only meaningful with the 'sdk' command):

      --ndk VERSION      NDK package to install (default: r27c)
      --build-tools V    build-tools package (default: 34.0.0)
      --platform NAME    SDK platform (default: android-34)
      --minimal          NDK only, no aapt2/apksigner/platform

Any arguments after '--' are passed through to the test binaries (only relevant
for the 'plan' and 'test' commands, which build and run the plan tests).


Environment:
  OpenVINO_DIR   OpenVINO devel tree to build against, e.g.
                 /usr/lib64/cmake/OpenVINO (the default when unset). The runtime
                 plugins have to match: <libdir>/openvino-<version>/ must hold
                 libopenvino_intel_gpu_plugin.so for DNN_TARGET_OPENCL.
  ANDROID_SDK_ROOT / ANDROID_HOME  Android SDK location (default: ~/Android/Sdk)
  ANDROID_API_LEVEL / ANDROID_ABI  Defaults for the Android options above
  ANDROID_SERIAL    Device serial for deploy/debug/smoke (equivalent to
                    --serial)
  ANDROID_STAGE     Android: staging directory for the cross-build's .so
                    files and APKs (default: <repo>/build/android)
  V4D_SKIP_CLVK     Android: set to 1 to skip building the vendored clvk
                    (OpenCL-on-Vulkan) layer and get a CPU-only build

Examples:
  $(basename "$0") plan
  $(basename "$0") plan+v4d -b release
  $(basename "$0") plan -b asan -j 8 -- --gtest_filter=Plan.*
  $(basename "$0") configure -t plan+v4d -b release -d openvino
  $(basename "$0") build && $(basename "$0") test
  $(basename "$0") android --apk --demo font_rendering
  $(basename "$0") android --abi x86_64 --configure-only
  $(basename "$0") sdk --ndk 27.2.12479018
  $(basename "$0") deploy --demo font_rendering --log
  $(basename "$0") debug --demo font_rendering --wait 15
  $(basename "$0") smoke --settle 15
  $(basename "$0") devices
  $(basename "$0") clean
EOF
  exit 0
}

# --- Android -----------------------------------------------------------------
# Delegated wholesale to tools/android-build.sh rather than reimplemented here.
# The host build below assumes an X11/Wayland desktop, OpenVINO, FFmpeg with
# VAAPI and `sudo make install` into /usr/local -- none of which exists on a
# device, and every one of which would fight the NDK toolchain if it were tried.
# The two share nothing but this argument parsing.
android_build() {
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
}

# --- Android device + SDK ----------------------------------------------------
# deploy/debug/smoke/sdk/devices each delegate to one script under
# tools/android/, which owns the adb and SDK details. This file contributes only
# the option parsing and the default that a bare command needs.
#
# adb honours ANDROID_SERIAL itself; run-demo.sh and smoke-test.sh additionally
# skip their "exactly one device" check when it is set, so a --serial on a host
# with several devices addresses only that one.
export_android_serial() {
  if [ -n "$ANDROID_SERIAL" ]; then
    export ANDROID_SERIAL
  fi
}

# deploy and debug act on one demo. A bare command defaults to the first sample
# rather than 'all'; an explicit --demo has to be a single name.
require_single_demo() {
  if [ "$ANDROID_DEMO_SET" = 0 ]; then
    ANDROID_DEMO=font_rendering
  fi
  case "$ANDROID_DEMO" in
    ""|all|*,*)
      echo "'$COMMAND' acts on one demo at a time; --demo takes a single sample." >&2
      exit 1 ;;
  esac
}

android_deploy() {
  require_single_demo
  export_android_serial
  local args=("$ANDROID_DEMO" "$ANDROID_ABI")
  [ "$ANDROID_LOG" = 1 ] && args+=(--log)
  "$SCRIPT_DIR/tools/android/run-demo.sh" "${args[@]}"
}

android_debug() {
  require_single_demo
  export_android_serial
  "$SCRIPT_DIR/tools/android/debug-device.sh" "$ANDROID_DEMO" "$ANDROID_ABI" "$ANDROID_WAIT"
}

android_smoke() {
  export_android_serial
  local args=(--abi "$ANDROID_ABI" --settle "$ANDROID_SETTLE")
  [ "$ANDROID_KEEP_LOG" = 1 ] && args+=(--keep-log)
  if [ "$ANDROID_DEMO" != all ]; then
    IFS=',' read -r -a DEMOS <<< "$ANDROID_DEMO"
    args+=("${DEMOS[@]}")
  fi
  "$SCRIPT_DIR/tools/android/smoke-test.sh" "${args[@]}"
}

android_devices() {
  "$SCRIPT_DIR/tools/android/devices.sh"
}

android_sdk() {
  local args=()
  [ -n "$SDK_NDK_VERSION" ] && args+=(--ndk "$SDK_NDK_VERSION")
  [ -n "$SDK_BUILD_TOOLS" ] && args+=(--build-tools "$SDK_BUILD_TOOLS")
  [ -n "$SDK_PLATFORM" ] && args+=(--platform "$SDK_PLATFORM")
  [ "$SDK_MINIMAL" = 1 ] && args+=(--minimal)
  "$SCRIPT_DIR/tools/android/install-sdk.sh" "${args[@]}"
}

# The command is the first positional argument; a leading '-' is not a command,
# so it falls through to the option loop and gets its error message there.
COMMAND=plan
if [ $# -gt 0 ] && [ "$1" != "--" ] && [ "${1#-}" = "$1" ]; then
  COMMAND="$1"
  shift
fi

while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in
    -t|--target)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      TARGET="$2"; TARGET_SET=1; shift 2 ;;
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
      ANDROID_DEMO="$2"; ANDROID_DEMO_SET=1; shift 2 ;;
    --apk)
      ANDROID_PACKAGE=1; shift ;;
    --configure-only)
      ANDROID_CONFIGURE_ONLY=1; shift ;;
    --serial)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_SERIAL="$2"; shift 2 ;;
    --log)
      ANDROID_LOG=1; shift ;;
    --wait)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_WAIT="$2"; shift 2 ;;
    --settle)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      ANDROID_SETTLE="$2"; shift 2 ;;
    --keep-log)
      ANDROID_KEEP_LOG=1; shift ;;
    --ndk)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      SDK_NDK_VERSION="$2"; shift 2 ;;
    --build-tools)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      SDK_BUILD_TOOLS="$2"; shift 2 ;;
    --platform)
      [ $# -ge 2 ] || { echo "Missing value for $1" >&2; exit 1; }
      SDK_PLATFORM="$2"; shift 2 ;;
    --minimal)
      SDK_MINIMAL=1; shift ;;
    -r|--rebuild)
      REBUILD=1; shift ;;
    -h|--help)
      usage ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Try '$(basename "$0") --help' for the commands and options." >&2
      exit 1 ;;
  esac
done
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
TEST_ARGS="$*"

case "$COMMAND" in
  v4d) COMMAND=plan+v4d ;;
esac

# 'plan' and 'plan+v4d' are the module sets themselves, so they set the target
# rather than defaulting it: without this './build.sh plan+v4d' would configure
# the plan-only cmake arguments and build neither v4d nor its tests.
if [ "$COMMAND" = plan ] || [ "$COMMAND" = plan+v4d ]; then
  if [ "$TARGET_SET" = 1 ]; then
    echo "'$COMMAND' already names the module set; drop -t/--target." >&2
    exit 1
  fi
  TARGET="$COMMAND"
fi

case "$COMMAND" in
  plan|plan+v4d|android|deploy|debug|smoke|sdk|devices|configure|build|test|install|clean) ;;
  *)
    echo "Invalid command '$COMMAND'" >&2
    echo "Try '$(basename "$0") --help' for the list of commands." >&2
    exit 1 ;;
esac

if [ "$COMMAND" = android ]; then
  android_build
  exit 0
fi

case "$COMMAND" in
  deploy)  android_deploy;  exit 0 ;;
  debug)   android_debug;   exit 0 ;;
  smoke)   android_smoke;   exit 0 ;;
  sdk)     android_sdk;     exit 0 ;;
  devices) android_devices; exit 0 ;;
esac

case "$TARGET" in
  plan|plan+v4d) ;;
  *)
    echo "Invalid target '$TARGET' (expected 'plan' or 'plan+v4d';" >&2
    echo "'android' is a command, not a target)" >&2
    exit 1 ;;
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
#   OpenVINO_DIR=/opt/intel/openvino_2026/runtime/lib/cmake/ov ./build.sh plan -r
# Left empty, OpenCV's find_package(OpenVINO) picks up whatever is installed.
OPENVINO_DIR_OVERRIDE="${OpenVINO_DIR:-}"

case "$BUILD_TYPE" in
  release)
    CMAKE_BUILD_TYPE=Release ;;
  debug)
    CMAKE_BUILD_TYPE=Debug ;;
  relwithdeb)
    CMAKE_BUILD_TYPE=RelWithDebInfo ;;
  asan)  SAN="-fsanitize=address" ;;
  ubsan) SAN="-fsanitize=undefined" ;;
  tsan)  SAN="-fsanitize=thread" ;;
  *)
    echo "Invalid build type '$BUILD_TYPE' (expected release, debug, relwithdeb, asan, ubsan or tsan)" >&2
    exit 1 ;;
esac

if [ -n "${SAN:-}" ]; then
  C_FLAGS="$SAN -fno-omit-frame-pointer"
  CXX_FLAGS="$CXX_FLAGS $SAN -fno-omit-frame-pointer"
  EXE_LINKER_FLAGS="$SAN"
  SHARED_LINKER_FLAGS="$SAN"
fi

CMAKE_ARGS=(
  -DCMAKE_POLICY_VERSION_MINIMUM=3.24
  -DWITH_WAYLAND=ON
  -DOPENCV_V4D_ENABLE_ES3=OFF
  -DOPENCV_V4D_ENABLE_BGFX=OFF
  -DOPENCV_ALGO_HINT_DEFAULT=ALGO_HINT_APPROX
  -DCMAKE_MODULE_LINKER_FLAGS="/usr/local/lib64/"
  -DINSTALL_BIN_EXAMPLES=ON
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

if [ -n "$OPENVINO_DIR_OVERRIDE" ]; then
  CMAKE_ARGS+=(-DOpenVINO_DIR="$OPENVINO_DIR_OVERRIDE")
fi

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

# --- host build steps --------------------------------------------------------

# Make sure there is a build directory to configure into, discarding a stale one
# when the module set or build type changed, or --rebuild was asked for.
prepare_build_dir() {
  if [ ! -d "$OPENCV_DIR" ]; then
    (cd "$SCRIPT_DIR"; git clone git@github.com:kallaballa/opencv.git)
  fi

  # The marker records both the module set and the build type. Both have to
  # match, because either one changes the CMake arguments: switching module sets
  # (e.g. plan+v4d -> plan) turns modules such as opencv_ts on or off, and CMake
  # caches the resulting OPENCV_MODULES_DISABLED_FORCE list -- so an in-place
  # reconfigure would leave the tests (or the v4d module) disabled and only a
  # fresh tree re-enables them. The stale marker from before the split read just
  # the build type, so the first run after this change reconfigures once.
  local want="$TARGET $BUILD_TYPE"
  if [ -f "$BUILD_MARKER" ] && [ "$(cat "$BUILD_MARKER")" != "$want" ]; then
    echo "Build dir was configured as '$(cat "$BUILD_MARKER")', reconfiguring for '$want'"
    REBUILD=1
  fi

  if [ "$REBUILD" = 1 ] || [ ! -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
  fi
}

# For the commands that take an already-configured tree as given. Without this a
# stale tree left over from a different module set would be compiled against and
# the mismatch would only show up as a missing target halfway through the build.
require_configured_build_dir() {
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "$BUILD_DIR is not a configured build tree." >&2
    echo "  Run: $(basename "$0") configure -t $TARGET -b $BUILD_TYPE" >&2
    exit 1
  fi
}

do_configure() {
  prepare_build_dir
  echo "==> Configuring target '${TARGET}' (build type: ${BUILD_TYPE}) in ${BUILD_DIR}"
  cd "$BUILD_DIR"
  if [ "$REBUILD" = 1 ]; then
    cmake --fresh "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
  else
    cmake "${CMAKE_ARGS[@]}" "$OPENCV_DIR"
  fi
  echo "$TARGET $BUILD_TYPE" > "$BUILD_MARKER"
}

# The plan module set builds only its test binaries; plan+v4d builds everything.
do_build() {
  cd "$BUILD_DIR"
  echo "==> Building target '${TARGET}' with ${JOBS} job(s)"
  if [ "$TARGET" = plan+v4d ]; then
    make -j"$JOBS"
  else
    make -j"$JOBS" opencv_test_plan opencv_perf_plan
  fi
}

do_test() {
  cd "$BUILD_DIR"
  local binary
  for binary in opencv_test_plan opencv_perf_plan; do
    if [ -x "./bin/$binary" ]; then
      "./bin/$binary" $TEST_ARGS
    elif [ -x "./$binary" ]; then
      "./$binary" $TEST_ARGS
    else
      echo "$binary not found in $BUILD_DIR; build it first:" >&2
      echo "  Run: $(basename "$0") build -t $TARGET" >&2
      exit 1
    fi
  done
}

do_make_install() {
  sudo /usr/bin/make -C "$BUILD_DIR" install
}

# 'install' stands on its own: the tree may be configured but not yet built, and
# a bare `make install` would then fail on a missing target rather than build it.
do_install() {
  do_build
  do_make_install
}

case "$COMMAND" in
  plan)
    do_configure
    do_build
    do_test ;;
  plan+v4d)
    do_configure
    do_build
    do_make_install ;;
  configure)
    do_configure ;;
  build)
    require_configured_build_dir
    do_build ;;
  test)
    require_configured_build_dir
    do_test ;;
  install)
    require_configured_build_dir
    do_install ;;
  clean)
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR" ;;
esac
