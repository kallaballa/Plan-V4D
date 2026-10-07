#!/usr/bin/env bash
# Print the CMake arguments that configure OpenCV + plan + v4d for one Android
# ABI, one argument per line (read with `mapfile -t`).
#
#   mapfile -t CMAKE_ARGS < tools/android/opencv-cmake-args.sh arm64-v8a
#
# Only -D lines are printed: blank lines and the # comments between them are
# kept here for readability but are dropped before anything is handed to cmake,
# which would reject them as unknown arguments.
#
# Deliberately NOT a mode of ./build.sh: that is the host driver, and its flags
# (X11/Wayland, FFmpeg+VAAPI, OpenVINO, /usr/local/lib64, `sudo make install`)
# are all wrong on a device. This script is the Android counterpart and shares
# no code with it.
#
# Requires (from tools/android/env.sh): ANDROID_TOOLCHAIN_FILE, ANDROID_API_LEVEL.
# Requires: ANDROID_EXTRA_MODULES_PATH, V4D_FREETYPE_DIR.
set -euo pipefail

ABI="${1:-arm64-v8a}"
: "${ANDROID_TOOLCHAIN_FILE:?source tools/android/env.sh first}"
: "${ANDROID_EXTRA_MODULES_PATH:?set ANDROID_EXTRA_MODULES_PATH to the repo modules/ dir}"
: "${V4D_FREETYPE_DIR:?set V4D_FREETYPE_DIR to the prefix from tools/android/build-freetype.sh}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TRIPLE="$("$SCRIPT_DIR/abi-triple.sh" "$ABI")"
# A hand-written link line needs the NDK's EGL/GLESv3: CMake's FindOpenGL looks
# for a GL/gl.h, which the NDK does not ship, so it reports OpenGL as not found
# even on Android where GLESv3 is right there in the sysroot.
NDK_LIBS="$("$SCRIPT_DIR/abi-triple.sh" --libs "$ABI")"

cat <<EOF
-DANDROID_ABI=$ABI
-DANDROID_NATIVE_API_LEVEL=$ANDROID_API_LEVEL
-DANDROID_PLATFORM=android-$ANDROID_API_LEVEL
-DANDROID_STL=${ANDROID_STL:-c++_static}
-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
-DANDROID_ARM_NEON=TRUE
-DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE
-DCMAKE_SYSTEM_VERSION=$ANDROID_API_LEVEL
-DCMAKE_POLICY_VERSION_MINIMUM=3.24
-DCMAKE_BUILD_TYPE=Release

# --- what to build ---------------------------------------------------------
-DOPENCV_EXTRA_MODULES_PATH=$ANDROID_EXTRA_MODULES_PATH
# The module list is derived from what the samples actually include (see the
# audit in android.md): geometry for getRotationMatrix2D, features2d/flann for
# optflow, objdetect/xobjdetect/tracking for the HOG and KCF demos, dnn for the
# pose and face nets, face and stitching for beauty-demo. Leaving any of these
# out does not fail the configure -- it fails later, as a sample that will not
# link -- so the list is kept complete rather than minimal: every sample that
# has an Android path can then be built without touching this file.
-DBUILD_LIST=plan,v4d,core,imgproc,imgcodecs,videoio,video,ximgproc,geometry,features2d,flann,imgcodecs,objdetect,xobjdetect,tracking,optflow,dnn,face,stitching,calib3d,plot
-DOPENCV_BUILD_TEST_MODULES_LIST=plan
-DBUILD_opencv_plan=ON
-DBUILD_opencv_v4d=ON
-DBUILD_EXAMPLES=ON
-DBUILD_TESTS=OFF
-DBUILD_PERF_TESTS=OFF
-DBUILD_DOCS=OFF
-DBUILD_PACKAGE=OFF
-DBUILD_SHARED_LIBS=OFF
-DBUILD_ANDROID_PROJECTS=OFF
-DBUILD_ANDROID_EXAMPLES=OFF
-DBUILD_KOTLIN_EXTENSIONS=OFF
-DBUILD_JAVA=OFF
-DBUILD_opencv_java=OFF
-DBUILD_opencv_apps=OFF
-DBUILD_opencv_highgui=OFF
-DBUILD_opencv_ts=OFF
-DBUILD_JAVA=OFF
-DOPENCV_GENERATE_PKGCONFIG=OFF
-DOPENCV_INSTALL_C_EXPORTS=OFF
-DCV_TRACE=OFF
-DENABLE_LTO=OFF
-DENABLE_PRECOMPILED_HEADERS=OFF
-DWITH_PTHREADS_PF=OFF

# --- 3rd party: let OpenCV build its own; there are no system packages here ---
-DBUILD_ZLIB=ON
-DBUILD_JPEG=ON
-DBUILD_PNG=ON
-DBUILD_TIFF=ON
-DBUILD_WEBP=ON
-DBUILD_OPENJPEG=ON
-DBUILD_PROTOBUF=ON
-DBUILD_QUIRC=ON
-DBUILD_ITT=ON
-DBUILD_TBB=OFF
-DWITH_ZLIB=ON
-DWITH_JPEG=ON
-DWITH_PNG=ON
-DWITH_WEBP=ON
-DWITH_TIFF=ON
-DWITH_OPENJPEG=ON
-DWITH_ITT=ON
-DWITH_QUIRC=ON
-DWITH_PROTOBUF=ON
-DWITH_1394=OFF
-DWITH_IPP=OFF
-DWITH_ITT=OFF
# OpenCL is dlopen()ed at runtime from the device driver (libOpenCL.so, or the
# Mali blob, PowerVR's libPVROCL.so, ...); nothing is linked or bundled. It
# enables UMat kernels and V4D's CL-GL framebuffer sharing (zero-copy), with
# an up-/download fallback when the device or driver does not cooperate.
-DWITH_OPENCL=ON
-DWITH_OPENCL_SVM=OFF
-DWITH_OPENCLAMDFFT=OFF
-DWITH_OPENCLAMDBLAS=OFF
-DWITH_VULKAN=OFF
-DWITH_LAPACK=OFF
-DWITH_EIGEN=OFF
-DWITH_OPENVINO=OFF
-DWITH_ONNXRUNTIME=OFF
-DOPENCV_DNN_OPENVINO=OFF
-DOPENCV_DNN_TFLITE=OFF
-DOPENCV_DNN_OPENCL=OFF
-DWITH_OPENVX=OFF
-DWITH_FFMPEG=OFF
-DWITH_GSTREAMER=OFF
-DWITH_1394=OFF
-DWITH_V4L=OFF
-DWITH_VA=OFF
-DWITH_GTK=OFF
-DWITH_GTK_2_X=OFF
-DWITH_QT=OFF
-DWITH_WAYLAND=OFF
-DWITH_OPENGL=OFF
-DWITH_VTK=OFF
-DWITH_GPHOTO2=OFF
-DWITH_JASPER=OFF
-DWITH_OPENEXR=OFF
-DWITH_TIFF=ON
-DWITH_CPUFEATURES=ON
-DWITH_KLEIDICV=ON
-DWITH_ANDROID_MEDIANDK=ON
-DWITH_ANDROID_NATIVE_CAMERA=ON

# --- V4D -------------------------------------------------------------------
# ES3 is the whole point: the desktop GL path cannot exist on a device. The
# GLFW replacement (modules/v4d/third/glfw-android) is selected automatically
# when ANDROID is set, so no GLFW_* flags are needed here.
-DOPENCV_V4D_ENABLE_ES3=ON
-DOPENCV_V4D_ENABLE_BGFX=OFF
-DOPENCV_V4D_ENABLE_MALI=OFF
-DOPENCV_V4D_USE_SYSTEM_GLFW=OFF
# Which samples become SharedObjects on Android (comma separated). An Android
# "executable" is a .so loaded by NativeActivity, and a desktop sample is not
# necessarily portable (xclip, /tmp, TextEditor, several windows), so the set is
# explicit rather than all-or-nothing. The default is every sample that
# registers an Android target, which sample-list.sh reads out of the module's
# CMakeLists so that adding a sample there is all it takes.
-DOPENCV_V4D_SAMPLES=${V4D_ANDROID_SAMPLES:-${OPENCV_V4D_SAMPLES:-$("$SCRIPT_DIR/sample-list.sh")}}

# --- FreeType --------------------------------------------------------------
# The NDK has none and OpenCV's WITH_* options do not build one for Android;
# tools/android/build-freetype.sh builds it per ABI into this prefix.
#
# The three variables below are named explicitly rather than left to
# find_package(Freetype)'s search: under the NDK toolchain CMAKE_FIND_ROOT_PATH
# points at the sysroot and CMAKE_FIND_ROOT_PATH_MODE_INCLUDE is ONLY, so the
# prefix -- which is deliberately outside the sysroot -- is never searched.
# find_path()/find_library() skip the search when the variable is already set,
# which is what makes this work.
-DFREETYPE_INCLUDE_DIR_ft2build=$V4D_FREETYPE_DIR/include/freetype2
-DFREETYPE_INCLUDE_DIR_freetype2=$V4D_FREETYPE_DIR/include/freetype2
-DFREETYPE_LIBRARY=$V4D_FREETYPE_DIR/lib/libfreetype.a

# --- compile-time GL/EGL discovery ----------------------------------------
# FindOpenGL's Android branch points at the pre-r19 NDK sysroot layout, so the
# paths are seeded explicitly instead.
-DOPENGL_INCLUDE_DIR=$ANDROID_SYSROOT/usr/include
-DOPENGL_EGL_INCLUDE_DIR=$ANDROID_SYSROOT/usr/include
-DOPENGL_GLES3_INCLUDE_DIR=$ANDROID_SYSROOT/usr/include
-DOPENGL_egl_LIBRARY=$NDK_LIBS/libEGL.so
-DOPENGL_gles3_LIBRARY=$NDK_LIBS/libGLESv3.so
-DOPENGL_gl_LIBRARY=$NDK_LIBS/libGLESv3.so
EOF