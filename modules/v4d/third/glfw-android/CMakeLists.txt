# This file is part of OpenCV project.
# It is subject to the license terms in the LICENSE file found in the top-level
# directory of this distribution and at http://opencv.org/license.html.
#
# The Android GLFW replacement. See glfw_android.cpp for why one is needed.
#
# It publishes a target named `glfw`, which is the name third/glfw and every
# desktop GLFW package also publish, so modules/v4d/CMakeLists.txt links the
# module and the samples exactly as it does on the host. The generated
# GLFW/glfw3.h here shadows the vendored one on the include path, so no V4D
# source needs an #ifdef to choose between the two.
#
# Two things about the layout are load-bearing:
#
#   * android_main.cpp (ANativeActivity_onCreate and friends) is NOT part of
#     the `glfw` target. It is compiled once per demo, into that demo's shared
#     object, because the manifest names exactly one lib_name and the activity
#     entry point has to live in it. modules/v4d/CMakeLists.txt does that.
#
#   * the shim keeps process-wide state (the activity, the surface, the input
#     queue) in one Platform object, so it must end up in the final image
#     exactly once. Linking this static library into opencv_v4d and then into
#     each demo (rather than into both) is what guarantees that: the demo
#     reaches the shim through opencv_v4d, so the archive member is pulled in a
#     single time. Linking it into both would give two EGL surfaces and two
#     input queues, and the symptom would be a window that never appears.

if(NOT ANDROID)
  message(FATAL_ERROR "third/glfw-android is an Android-only replacement for third/glfw")
endif()

add_library(glfw STATIC
  glfw_android.cpp)

set_target_properties(glfw PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  # Vendored third-party code: its warnings are not ours to fix.
  COMPILE_OPTIONS "-w")

# <GLFW/glfw3.h> next to this file. This has to be an include directory of the
# target rather than of opencv_v4d, so that the header wins over
# third/glfw/include/GLFW/glfw3.h without anything having to know which GLFW is
# in play. modules/v4d/CMakeLists.txt adds the same directory for the samples.
target_include_directories(glfw PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(glfw PRIVATE OPENCV_V4D_ANDROID=1)

# EGL and GLESv3 for the context, android for ANativeActivity/AInputQueue, log
# for __android_log_print. gl is already part of the NDK's libc startup for an
# executable, but a library loaded by NativeActivity links no crt at all, so the
# GL entry points are named explicitly: they have to resolve at load time from
# libGLESv3.so.
target_link_libraries(glfw PUBLIC EGL GLESv3 android log)

# The shim is C++ but its public surface is the C GLFW API, and it is built once
# per ABI; keep it in OpenCV's own library directory so it lands next to
# libopencv_v4d.
set_target_properties(glfw PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
  LIBRARY_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}")

install(TARGETS glfw EXPORT OpenCVModules
        ARCHIVE DESTINATION "${OPENCV_LIB_INSTALL_PATH}"
        LIBRARY DESTINATION "${OPENCV_LIB_INSTALL_PATH}")
# opencv2/v4d/events.hpp includes <GLFW/glfw3.h>, so the header has to travel
# with the library: the INSTALL_INTERFACE include directory is
# ${CMAKE_INSTALL_INCLUDEDIR} and #include <GLFW/glfw3.h> resolves against it.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/GLFW/glfw3.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/GLFW")