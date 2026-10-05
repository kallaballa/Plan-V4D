cmake_policy(SET CMP0079 NEW)

OCV_OPTION(OPENCV_V4D_ENABLE_ES3 "Enable OpenGL ES 3.0 backend for V4D" OFF
  VERIFY HAVE_OPENGL)

OCV_OPTION(OPENCV_V4D_ENABLE_BGFX "Enable bgfx for V4D" OFF
  VERIFY HAVE_OPENGL)

OCV_OPTION(OPENCV_V4D_ENABLE_MALI "Enable mali gpu support for V4D" OFF
  VERIFY HAVE_OPENGL)

OCV_OPTION(OPENCV_V4D_USE_SYSTEM_GLFW "Link the GLFW found by find_package(glfw3) instead of the vendored third/glfw copy" OFF)

if(OPENCV_V4D_ENABLE_BGFX)
    add_definitions(-DOPENCV_V4D_ENABLE_BGFX=1)
endif()

include(FetchContent)
include(FindOpenCL)

if(OPENCV_V4D_ENABLE_MALI)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(mali REQUIRED IMPORTED_TARGET mali)
endif()

include(FindOpenGL)
find_package(Freetype REQUIRED)
set(the_description "V4D Visualization Module")
set(OPENCV_MODULE_IS_PART_OF_WORLD OFF)

# Check CXX Features
get_property(known_features GLOBAL PROPERTY CMAKE_CXX_KNOWN_FEATURES)
list (FIND known_features "cxx_std_20" idx)
if (${idx} LESS 0)
  message(STATUS "Module opencv_v4d disabled because it requires C++20")
  ocv_module_disable(v4d)
endif()

# Update submodules (skip in OBS build environment)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.git" AND NOT DEFINED ENV{OBS_BUILD})
  message(STATUS "Submodule update")
  execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../"
                  RESULT_VARIABLE GIT_SUBMOD_RESULT)
  if(NOT GIT_SUBMOD_RESULT EQUAL "0")
    message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
  endif()
endif()

set(V4D_THIRD_PARTY_DEPS imgui nanovg)
if(NOT OPENCV_V4D_USE_SYSTEM_GLFW)
  list(APPEND V4D_THIRD_PARTY_DEPS glfw)
endif()
foreach(dep ${V4D_THIRD_PARTY_DEPS})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third/${dep}/")
    message(FATAL_ERROR "The submodule 'third/${dep}' was not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
  endif()
endforeach()

# GLFW. The vendored copy in third/glfw is a submodule and is built together with
# this module, so a checkout needs no GLFW development package. Configure with
# -DOPENCV_V4D_USE_SYSTEM_GLFW=ON to link whatever find_package(glfw3) locates
# instead. Both paths provide the same `glfw` target, so the module and the
# samples link it the same way either way.
if(OPENCV_V4D_USE_SYSTEM_GLFW)
  find_package(glfw3 3 REQUIRED)
elseif(NOT TARGET glfw)
  # GLFW's own options, set as plain variables rather than cache entries:
  # option()/cmake_dependent_option() inside the subdirectory leave an
  # already-defined normal variable alone (CMP0077 NEW), so these are defaults
  # that any explicit -DGLFW_*=... on the command line still wins over.
  set(GLFW_BUILD_EXAMPLES OFF)
  set(GLFW_BUILD_TESTS OFF)
  set(GLFW_BUILD_DOCS OFF)
  # The library and its headers are installed below as part of OpenCV's own
  # install rules; GLFW_INSTALL would add a second, independent set of them.
  set(GLFW_INSTALL OFF)
  # GLFW builds its Wayland backend only if asked to, but it then hard-requires
  # the Wayland development files and wayland-scanner. OpenCV's WITH_WAYLAND
  # (which V4D follows: the runtime asks for GLFW_PLATFORM_WAYLAND when the
  # platform is available) doubles as the say for whether those are installed.
  if(NOT WITH_WAYLAND)
    set(GLFW_BUILD_WAYLAND OFF)
  endif()

  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third/glfw")

  # Keep the library next to libnanovg.so instead of glfw's own binary dir.
  set_target_properties(glfw PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
    ARCHIVE_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
    RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}")
  # Vendored third-party code: its warnings are not ours to fix, and silencing
  # the whole target would hide warnings from the V4D module itself.
  target_compile_options(glfw PRIVATE -w)

  # opencv2/v4d/events.hpp includes <GLFW/glfw3.h>, so the headers have to
  # travel with the library: the INSTALL_INTERFACE include directory glfw
  # exports is ${CMAKE_INSTALL_INCLUDEDIR} and #include <GLFW/glfw3.h> resolves
  # against it.
  install(TARGETS glfw EXPORT OpenCVModules
          LIBRARY DESTINATION "${OPENCV_LIB_INSTALL_PATH}"
          ARCHIVE DESTINATION "${OPENCV_LIB_INSTALL_PATH}"
          RUNTIME DESTINATION "${OPENCV_BIN_INSTALL_PATH}")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third/glfw/include/GLFW/glfw3.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/third/glfw/include/GLFW/glfw3native.h"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/GLFW")
endif()

set(V4D_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../")

# Asset search path. An ordered list of directories that are searched for asset
# files (videos, models, fonts, ...). The first existing directory has the
# highest priority. The list is baked into the binaries as V4D_ASSET_PATH and can
# be extended/overridden at runtime with the V4D_ASSET_PATH environment
# variable (same separator), which takes precedence over the compiled-in list.
set(V4D_ASSET_PATH_DIRS
  "${CMAKE_CURRENT_BINARY_DIR}/assets"
  "${CMAKE_CURRENT_BINARY_DIR}/samples/data"
  "${CMAKE_CURRENT_BINARY_DIR}/samples/fonts"
  "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  "${CMAKE_CURRENT_SOURCE_DIR}/samples/data"
  "${CMAKE_CURRENT_SOURCE_DIR}/samples/fonts"
  "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/opencv4"
  "/usr/local/share/opencv4")
list(REMOVE_DUPLICATES V4D_ASSET_PATH_DIRS)
if(WIN32)
  set(V4D_ASSET_PATH_SEPARATOR ";") # ':' would split windows drive letters
else()
  set(V4D_ASSET_PATH_SEPARATOR ":")
endif()
string(REPLACE ";" "${V4D_ASSET_PATH_SEPARATOR}" V4D_ASSET_PATH "${V4D_ASSET_PATH_DIRS}")
message(STATUS "V4D asset search path: ${V4D_ASSET_PATH}")

# Macro to download a file (or use local cache)
macro(fetch_file download_name url hash)
  set(LOCAL_CACHE_DIR "$ENV{HOME}/.cache/opencv_v4d" CACHE PATH "Local cache for downloaded files")
  set(LOCAL_CACHE_FILE "${LOCAL_CACHE_DIR}/${download_name}")
  if(EXISTS "${LOCAL_CACHE_FILE}")
    message(STATUS "Using cached file for ${download_name}: ${LOCAL_CACHE_FILE}")
    set(${download_name}_SOURCE_DIR "${LOCAL_CACHE_DIR}" CACHE PATH "Source dir for ${download_name}" FORCE)
    set(${download_name}_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/${download_name}-build" CACHE PATH "Binary dir for ${download_name}" FORCE)
    set(${download_name}_POPULATED TRUE CACHE BOOL "Populated flag for ${download_name}" FORCE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/${download_name}-build")
  else()
    FetchContent_Declare(${download_name}
    URL ${url}
    URL_HASH SHA256=${hash}
    DOWNLOAD_NO_EXTRACT true
    TLS_VERIFY true
    )
    FetchContent_MakeAvailable(${download_name})
  endif()
endmacro(fetch_file)

# Populate assets using local files
macro(populate_assets_local lbfmodel_path yunet_path)
  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${lbfmodel_path}"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models/lbfmodel.yaml")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${yunet_path}"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models/face_detection_yunet_2023mar.onnx")
endmacro(populate_assets_local)

# Copy one model asset into the build tree, keeping the two in sync.
#   v4d_copy_model_asset(<source> <path relative to assets/models/>)
# Declared as a file-level rule (OUTPUT/DEPENDS) rather than a PRE_BUILD command
# so that editing the asset in place refreshes the copy on the next `make`.
function(v4d_copy_model_asset source relative)
  if(NOT EXISTS "${source}")
    message(WARNING "V4D: asset '${source}' does not exist, skipping copy")
    return()
  endif()
  set(dst "${CMAKE_CURRENT_BINARY_DIR}/assets/models/${relative}")
  get_filename_component(dst_name "${dst}" NAME)
  add_custom_command(
    OUTPUT "${dst}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/models/pose"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${dst}"
    DEPENDS "${source}"
    COMMENT "V4D asset: ${relative}"
    VERBATIM)
  add_custom_target(v4d_asset_${dst_name} ALL DEPENDS "${dst}")
endfunction()

# Fetch a font the editor samples need (the code pane wants a monospace face).
# The files are checked in, so this only ever runs on a checkout that predates
# them; when they are already there the build costs nothing.
#   v4d_fetch_font(<name> <url> <sha256>)
function(v4d_fetch_font name url sha)
  set(dst "${CMAKE_CURRENT_SOURCE_DIR}/samples/fonts/${name}")
  if(EXISTS "${dst}")
    return()
  endif()
  file(DOWNLOAD "${url}" "${dst}.tmp" STATUS _st EXPECTED_HASH "SHA256=${sha}")
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    list(GET _st 1 _msg)
    file(REMOVE "${dst}.tmp")
    message(WARNING "V4D: could not download ${name}: ${_msg}. The sample falls "
                    "back to a system font.")
    return()
  endif()
  file(RENAME "${dst}.tmp" "${dst}")
  message(STATUS "V4D: fetched ${name}")
endfunction()

# Macro to add a native sample
#   add_binary_sample(<target> <source> [extra opencv modules...])
# Everything nearly every sample needs is linked here. Modules only some of them
# use -- geometry, for instance, which OpenCV 5 requires for
# getRotationMatrix2D -- are passed per sample, so that a build without one of
# them still gets BUILD_EXAMPLES for the samples that do not need it.
macro(add_binary_sample sample source)
    ocv_add_executable(${sample} ${source})
    _v4d_configure_sample(${sample} ${ARGN})
endmacro()

# Macro to add a native sample built from several translation units:
#   add_multisource_sample(<target> <source>... )
# It exists because add_binary_sample() cannot express this: it binds its second
# argument to a single source and reads every argument after it as an extra
# OpenCV module, so a second .cpp would become a bogus link flag. Extra modules
# are linked by the caller instead.
macro(add_multisource_sample sample)
    ocv_add_executable(${sample} ${ARGN})
    _v4d_configure_sample(${sample})
endmacro()

# Shared tail of the two macros above: what a V4D sample links against, the
# include directories it needs, and the asset path it is compiled with. ARGN is
# any extra OpenCV modules named by the caller.
macro(_v4d_configure_sample sample)
    ocv_target_link_libraries(${sample} opencv_v4d opencv_plan opencv_core opencv_imgproc opencv_ximgproc opencv_videoio opencv_video opencv_imgcodecs opencv_objdetect opencv_xobjdetect opencv_optflow opencv_tracking opencv_stitching opencv_face opencv_features opencv_dnn glfw nanovg)
    if(ARGN)
      ocv_target_link_libraries(${sample} ${ARGN})
    endif()
    if(WITH_OPENCL)
      ocv_target_link_libraries(${sample} "${OpenCL_LIBRARIES}")
    endif()
    if(OPENCV_V4D_ENABLE_ES3)
      ocv_target_link_libraries(${sample} GLESv2)
    elseif(APPLE)
      ocv_target_link_libraries(${sample} OpenGL)
    else()
      ocv_target_link_libraries(${sample} OpenGL GLU)
    endif()
    ocv_target_link_libraries(${sample} Freetype::Freetype)
    if(OPENCV_V4D_ENABLE_BGFX)
      ocv_target_link_libraries(${sample} bimg bx bgfx)
    endif()
    if(OPENCV_V4D_ENABLE_MALI)
      ocv_target_link_libraries(${sample} mali)
    endif()
    target_compile_features(${sample} PRIVATE cxx_std_20)
    target_link_directories(${sample} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../../lib")
    target_compile_definitions(${sample} PRIVATE
      "V4D_ASSET_PATH=\"${V4D_ASSET_PATH}\""
      "V4D_ASSETS_PATH=\"${V4D_ASSET_PATH}\"") # V4D_ASSETS_PATH is the deprecated spelling of V4D_ASSET_PATH
    if(OPENCV_V4D_ENABLE_BGFX)
      set(BGFX_INC_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bgfx/include/" "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bx/include/" "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bimg/include/")
    endif()
    ocv_target_include_directories(${sample} PRIVATE
      "${CMAKE_HOME_DIRECTORY}/modules/core/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include"
      "${CMAKE_HOME_DIRECTORY}/modules/video/include"
      "${CMAKE_HOME_DIRECTORY}/modules/videoio/include"
      "${CMAKE_HOME_DIRECTORY}/modules/dnn/include"
      "${V4D_MODULES_DIR}/plot/include"
      "${CMAKE_HOME_DIRECTORY}/modules/objdetect/include"
      "${CMAKE_HOME_DIRECTORY}/modules/stitching/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgcodecs/include"
      "${CMAKE_HOME_DIRECTORY}/modules/geometry/include"
      "${V4D_MODULES_DIR}/plan/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/glad/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/src/"
"${CMAKE_CURRENT_SOURCE_DIR}/third/AnyProperty"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui-color-text-edit"
      "${CMAKE_HOME_DIRECTORY}/modules/features/include"
      "${CMAKE_HOME_DIRECTORY}/modules/flann/include"
      "${V4D_MODULES_DIR}/xobjdetect/include"
      "${V4D_MODULES_DIR}/tracking/include"
      "${V4D_MODULES_DIR}/optflow/include"
      "${V4D_MODULES_DIR}/face/include"
      ${BGFX_INC_DIRS}
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/")
    if(OPENCV_V4D_ENABLE_BGFX)
      add_dependencies(${sample} v4d_copy_shaders)
    endif()
endmacro()

if (NOT (TARGET nanovg))
   if(OPENCV_V4D_ENABLE_ES3)
      add_definitions(-DNANOVG_GLES3=1)
    else()
      add_definitions(-DNANOVG_GL3=1)
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/")
    target_compile_options(nanovg PUBLIC -w)
    target_compile_options(nanovg PUBLIC -pthread)
    target_compile_options(nanovg PUBLIC -fvisibility=default)
    target_compile_definitions(nanovg PUBLIC NVG_NO_STB)

    install(TARGETS nanovg EXPORT OpenCVModules)
  endif()

  if (OPENCV_V4D_ENABLE_BGFX AND NOT (TARGET bgfx))
    set(BGFX_BUILD_EXAMPLES OFF)
    set(BGFX_LIBRARY_TYPE "SHARED")
    set(BGFX_INSTALL OFF)

  if(OPENCV_V4D_ENABLE_ES3)
        set(BGFX_OPENGLES_VERSION "30")
    else()
        set(BGFX_OPENGL_VERSION "32")
    endif()
    add_definitions(-DBGFX_CONFIG_PROFILER=0 -DBGFX_CONFIG_PASSIVE=1)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake")

    target_compile_features(bgfx PRIVATE cxx_std_20)
    target_compile_options(bgfx PUBLIC -Wno-error)
    target_compile_options(bgfx PUBLIC -fvisibility=default)
    ocv_target_include_directories(bgfx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third/glad/include")
    install(TARGETS bgfx EXPORT OpenCVModules)
    install(TARGETS bimg EXPORT OpenCVModules)
    install(TARGETS bx EXPORT OpenCVModules)
  endif()

# Add the opencv module
if(NOT (TARGET ${the_module}))
  if(BUILD_EXAMPLES)
     ocv_add_module(v4d opencv_plan opencv_core opencv_imgproc opencv_ximgproc opencv_videoio opencv_video opencv_imgcodecs glfw nanovg)
  else()
     ocv_add_module(v4d opencv_plan opencv_core opencv_imgproc opencv_ximgproc opencv_videoio opencv_video opencv_imgcodecs)
  endif()

  file(GLOB imgui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/misc/freetype/imgui_freetype.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/imguicontext.cpp")
  file(GLOB imgui_backend_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_opengl3*.cpp")
  file(GLOB imgui_glfw_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_glfw.cpp")
  file(GLOB imgui_thirdparty_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_opengl3*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_glfw.cpp")
  set_source_files_properties(${imgui_thirdparty_sources} COMPILE_OPTIONS "-w")
  ocv_glob_module_sources("${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/" ${imgui_sources} ${imgui_backend_sources} ${imgui_glfw_sources})
  set(_v4d_module_includes
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${V4D_MODULES_DIR}/plan/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/glad/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/src/"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/AnyProperty")
  if(OPENCV_V4D_ENABLE_BGFX)
    list(APPEND _v4d_module_includes
      "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bgfx/include/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bx/include/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bimg/include/")
  endif()
  ocv_module_include_directories(${_v4d_module_includes})
  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  # Populate assets
  set(LOCAL_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Local assets directory")

  if(EXISTS "${LOCAL_ASSETS_DIR}/models/face_detection_yunet_2023mar.onnx")
    set(YUNET_PATH "${LOCAL_ASSETS_DIR}/models/face_detection_yunet_2023mar.onnx")
  else()
    fetch_file("YUNET" "https://github.com/opencv/opencv_zoo/raw/refs/heads/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx" "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4")
    set(YUNET_PATH "${yunet_SOURCE_DIR}/face_detection_yunet_2023mar.onnx")
  endif()

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets/doxygen")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets/fonts")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/videos"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/videos")

  # The marker demo registers a photograph of its marker at start-up, so the
  # images directory has to reach the build tree like the videos one does.
  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/assets/images")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/images"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/images")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/data"
    "${CMAKE_CURRENT_BINARY_DIR}/samples/data")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/fonts/*.ttf"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/fonts/")

  # Single model files are copied through OUTPUT/DEPENDS rules rather than
  # PRE_BUILD commands. A PRE_BUILD command only runs when the module is
  # actually recompiled, so editing an asset in place (regenerating a model,
  # say) left a stale copy sitting in the build tree -- and that copy is the
  # first directory cv::samples::findFile() searches. As a proper file-level
  # rule, `make` compares timestamps and refreshes it.
  v4d_copy_model_asset("${YUNET_PATH}" "face_detection_yunet_2023mar.onnx")
  v4d_copy_model_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/pose/person_detection_mediapipe_2023mar.onnx"
    "pose/person_detection_mediapipe_2023mar.onnx")
  v4d_copy_model_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/pose/pose_estimation_mediapipe_2023mar.onnx"
    "pose/pose_estimation_mediapipe_2023mar.onnx")

  # DINOv3 ViT-B/16 for the marker demo. The graph and its external weights are
  # two files that must stay side by side: onnxruntime resolves the ".onnx.data"
  # name relative to the graph, so copying only the graph yields a session that
  # fails to create at run time rather than a load-time error.
  v4d_copy_model_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/dinov3_vitb16.onnx"
    "dinov3_vitb16.onnx")
  v4d_copy_model_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/models/dinov3_vitb16.onnx.data"
    "dinov3_vitb16.onnx.data")

  #Add sample targets
  if(BUILD_EXAMPLES)
    if(OPENCV_V4D_ENABLE_BGFX)
      add_custom_target(v4d_copy_shaders ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bgfx/examples/runtime/shaders"
        "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bgfx/examples/runtime/shaders"
        "${CMAKE_BINARY_DIR}/bin/shaders"
        )
    endif()
    add_binary_sample(example_v4d_display_image_fb samples/display_image_fb.cpp)
    add_binary_sample(example_v4d_imshow_reimplementation samples/imshow_reimplementation.cpp)
    add_binary_sample(example_v4d_display_image_nvg samples/display_image_nvg.cpp)
    add_binary_sample(example_v4d_vector_graphics samples/vector_graphics.cpp)
    add_binary_sample(example_v4d_vector_graphics_and_fb samples/vector_graphics_and_fb.cpp)
    add_binary_sample(example_v4d_render_opengl samples/render_opengl.cpp)
    add_binary_sample(example_v4d_custom_source_and_sink samples/custom_source_and_sink.cpp)
    add_binary_sample(example_v4d_font_rendering samples/font_rendering.cpp)
    add_binary_sample(example_v4d_font_with_gui samples/font_with_gui.cpp)
    add_binary_sample(example_v4d_video_editing samples/video_editing.cpp)
    add_binary_sample(example_v4d_cube-demo samples/cube-demo.cpp)
    add_binary_sample(example_v4d_many_cubes-demo samples/many_cubes-demo.cpp)
    add_binary_sample(example_v4d_two-windows-demo samples/two-windows-demo.cpp)
    add_binary_sample(example_v4d_video-demo samples/video-demo.cpp)
    add_binary_sample(example_v4d_nanovg-demo samples/nanovg-demo.cpp)
    add_binary_sample(example_v4d_font-demo samples/font-demo.cpp)
    add_binary_sample(example_v4d_shader-demo samples/shader-demo.cpp)
    add_binary_sample(example_v4d_pedestrian-demo samples/pedestrian-demo.cpp)
    add_binary_sample(example_v4d_skeletal-tracker-demo samples/skeletal-tracker-demo.cpp)
    add_binary_sample(example_v4d_optflow-demo samples/optflow-demo.cpp)
    add_binary_sample(example_v4d_beauty-demo samples/beauty-demo.cpp)

    # DINOv3 Marker Demo. Split across five V4D-free translation units so that a
    # head-less self-test can link the same code the window runs; see
    # samples/dinov3-marker-demo/README.md. findHomography lives in geometry in
    # OpenCV 5, so that is the one extra module; ORB and SIFT come from
    # opencv_features, which the shared list in _v4d_configure_sample() already
    # links (OpenCV 5 has no opencv_features2d -- it was folded into features).
    # These are linked at the call site because add_multisource_sample() passes
    # every argument to the compiler.
    add_multisource_sample(example_v4d_dinov3-marker-demo
      samples/dinov3-marker-demo.cpp
      samples/dinov3-marker-demo/dinov3-embedder.cpp
      samples/dinov3-marker-demo/dinov3-marker-database.cpp
      samples/dinov3-marker-demo/dinov3-geometric-verifier.cpp
      samples/dinov3-marker-demo/dinov3-patch-matcher.cpp
      samples/dinov3-marker-demo/dinov3-marker-pipeline.cpp)
    ocv_target_link_libraries(example_v4d_dinov3-marker-demo)
    ocv_target_include_directories(example_v4d_dinov3-marker-demo PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/samples/dinov3-marker-demo")

    # The demo's head-less self-test. It links the four helper translation units
    # but neither the window nor opencv_v4d, so it runs on a machine with no
    # display and no GL context -- which is the whole point of keeping the
    # pipeline in V4D-free files.
    ocv_add_executable(example_v4d_dinov3-marker-selftest
      samples/dinov3-marker-demo/dinov3-marker-selftest.cpp
      samples/dinov3-marker-demo/dinov3-embedder.cpp
      samples/dinov3-marker-demo/dinov3-marker-database.cpp
      samples/dinov3-marker-demo/dinov3-geometric-verifier.cpp
      samples/dinov3-marker-demo/dinov3-patch-matcher.cpp
      samples/dinov3-marker-demo/dinov3-marker-pipeline.cpp)
    ocv_target_include_directories(example_v4d_dinov3-marker-selftest PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/samples/dinov3-marker-demo"
      # The same list _v4d_configure_sample() adds by hand, trimmed to what this
      # target actually includes: OpenCV does not propagate a module's headers
      # to an executable through the link, so they have to be named here.
      "${CMAKE_HOME_DIRECTORY}/modules/core/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgcodecs/include"
      "${CMAKE_HOME_DIRECTORY}/modules/stereo/include"
      "${CMAKE_HOME_DIRECTORY}/modules/videoio/include"
      "${CMAKE_HOME_DIRECTORY}/modules/dnn/include"
      "${CMAKE_HOME_DIRECTORY}/modules/features/include"
      "${CMAKE_HOME_DIRECTORY}/modules/flann/include"
      "${CMAKE_HOME_DIRECTORY}/modules/geometry/include")
    ocv_target_link_libraries(example_v4d_dinov3-marker-selftest
      opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio opencv_dnn
      opencv_features opencv_geometry)

    # Add underscore version from playbook (the one we fixed)
    add_multisource_sample(example_v4d_dinov3_marker_demo
      samples/dinov3_marker_demo/dinov3_marker_demo.cpp
      samples/dinov3_marker_demo/dinov3_embedder.cpp
      samples/dinov3_marker_demo/marker_database.cpp
      samples/dinov3_marker_demo/geometric_verifier.cpp)
    ocv_target_link_libraries(example_v4d_dinov3_marker_demo opencv_geometry opencv_calib)
    ocv_target_include_directories(example_v4d_dinov3_marker_demo PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/samples/dinov3_marker_demo"
      "${CMAKE_HOME_DIRECTORY}/modules/core/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgcodecs/include"
      "${CMAKE_HOME_DIRECTORY}/modules/stereo/include"
      "${CMAKE_HOME_DIRECTORY}/modules/videoio/include"
      "${CMAKE_HOME_DIRECTORY}/modules/dnn/include"
      "${CMAKE_HOME_DIRECTORY}/modules/features/include"
      "${CMAKE_HOME_DIRECTORY}/modules/flann/include"
      "${CMAKE_HOME_DIRECTORY}/modules/calib/include"
      "${CMAKE_HOME_DIRECTORY}/modules/geometry/include")

    # Its head-less self-test: the three V4D-free units above, but neither the
    # window nor opencv_v4d, so it runs without a display or a GL context. This is
    # the instrument the marker outline is measured with -- see
    # samples/dinov3_marker_demo/README.md.
    ocv_add_executable(example_v4d_dinov3_marker_selftest
      samples/dinov3_marker_demo/dinov3_marker_selftest.cpp
      samples/dinov3_marker_demo/dinov3_embedder.cpp
      samples/dinov3_marker_demo/marker_database.cpp
      samples/dinov3_marker_demo/geometric_verifier.cpp)
    ocv_target_include_directories(example_v4d_dinov3_marker_selftest PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/samples/dinov3_marker_demo"
      "${CMAKE_HOME_DIRECTORY}/modules/core/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgcodecs/include"
      "${CMAKE_HOME_DIRECTORY}/modules/videoio/include"
      "${CMAKE_HOME_DIRECTORY}/modules/dnn/include"
      "${CMAKE_HOME_DIRECTORY}/modules/features/include"
      "${CMAKE_HOME_DIRECTORY}/modules/flann/include"
      "${CMAKE_HOME_DIRECTORY}/modules/geometry/include")
    ocv_target_link_libraries(example_v4d_dinov3_marker_selftest
      opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio opencv_dnn
      opencv_features opencv_geometry)

    if(OPENCV_V4D_ENABLE_BGFX)
      add_binary_sample(example_v4d_bgfx-demo samples/bgfx-demo.cpp)
      add_binary_sample(example_v4d_bgfx-demo2 samples/bgfx-demo2.cpp)
    endif()
    add_binary_sample(example_v4d_montage-demo samples/montage-demo.cpp)
    add_binary_sample(example_v4d_pipeline-demo samples/pipeline-demo.cpp)
    add_binary_sample(example_v4d_image_carousel-demo samples/image_carousel-demo.cpp)
    # The offline Shadertoy editor.
    #   v4d_fetch_font(<file> <url> <sha256>)
    # JetBrains Mono 2.304 (SIL OFL 1.1, see samples/fonts/OFL.txt), fetched
    # from the tag's raw tree so the hash below is the hash of the file itself.
    v4d_fetch_font(JetBrainsMono-Regular.ttf
      "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/ttf/JetBrainsMono-Regular.ttf"
      "a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f")
    v4d_fetch_font(JetBrainsMono-Bold.ttf
      "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/ttf/JetBrainsMono-Bold.ttf"
      "5590990c82e097397517f275f430af4546e1c45cff408bde4255dad142479dcb")
    v4d_fetch_font(JetBrainsMono-Italic.ttf
      "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/v2.304/fonts/ttf/JetBrainsMono-Italic.ttf"
      "9d0a1f7a708e6af183f1193b7e81d40da294f5c67682c085d8401c60aac8ded4")
    add_binary_sample(example_v4d_shadertoy-editor samples/shadertoy-editor.cpp)
    # The code pane is a vendored ImGui widget (ImGuiColorTextEdit, MIT) rather
    # than part of libv4d: only this sample edits code, and TextEditor.cpp is a
    # third-party translation unit that no other target should have to compile.
    # It reaches into imgui_internal.h, which the include list above already
    # covers. samples/shadertoy-editor/shadertoy-widget-vendor.sh re-derives the two files from
    # the pinned upstream tag and verifies their hashes.
    target_sources(example_v4d_shadertoy-editor PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui-color-text-edit/TextEditor.cpp")
    # Vendored third-party code: its warnings are not ours to fix, and silencing
    # the whole target would hide warnings from the sample itself.
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui-color-text-edit/TextEditor.cpp"
      PROPERTIES COMPILE_OPTIONS "-w")
  endif()

  if(OPENCV_V4D_ENABLE_ES3)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DOPENCV_V4D_USE_ES3=1")
  endif()

  target_compile_features(${the_module} PRIVATE cxx_std_20)
  target_compile_definitions(${the_module} PRIVATE "V4D_ASSET_PATH=\"${V4D_ASSET_PATH}\"")

  target_link_directories(${the_module} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../../lib")
  ocv_target_link_libraries(${the_module} opencv_plan glfw nanovg)
  if(OPENCV_V4D_ENABLE_ES3)
    ocv_target_link_libraries(${the_module} GLESv2)
  elseif(APPLE)
    ocv_target_link_libraries(${the_module} OpenGL)
  else()
    ocv_target_link_libraries(${the_module} OpenGL GLU)
  endif()
  ocv_target_link_libraries(${the_module} Freetype::Freetype)

  if(OPENCV_V4D_ENABLE_BGFX)
    ocv_target_link_libraries(${the_module} bimg bx bgfx)
  endif()
  if(WITH_OPENCL)
    ocv_target_link_libraries(${the_module} "${OpenCL_LIBRARIES}")
  endif()

  if(OPENCV_V4D_ENABLE_MALI)
    ocv_target_link_libraries(${the_module} mali)
  endif()

  # Install asset files
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets/"
          DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/opencv4"
          COMPONENT data)

  # Install OpenCV data files (haarcascades, lbpcascades) from main OpenCV source
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../data/haarcascades")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../data/haarcascades"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/opencv4"
            COMPONENT data)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../data/lbpcascades")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../data/lbpcascades"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/opencv4"
            COMPONENT data)
  endif()
endif()
