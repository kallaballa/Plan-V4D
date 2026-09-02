cmake_policy(SET CMP0079 NEW)

OCV_OPTION(OPENCV_V4D_ENABLE_ES3 "Enable OpenGL ES 3.0 backend for V4D" OFF
  VERIFY HAVE_OPENGL)

OCV_OPTION(OPENCV_V4D_ENABLE_BGFX "Enable bgfx for V4D" OFF
  VERIFY HAVE_OPENGL)

OCV_OPTION(OPENCV_V4D_ENABLE_MALI "Enable mali gpu support for V4D" OFF
  VERIFY HAVE_OPENGL)

if(OPENCV_V4D_ENABLE_BGFX)
    add_definitions(-DOPENCV_V4D_ENABLE_BGFX=1)
endif()

include(FetchContent)
include(FindOpenCL)

if(OPENCV_V4D_ENABLE_MALI)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(mali REQUIRED IMPORTED_TARGET mali)
endif()

find_package(glfw3 3 REQUIRED)
include(FindOpenGL)
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

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/")
    message(FATAL_ERROR "The submodules were not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

set(V4D_MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../")

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

# Macro to add a native sample
macro(add_binary_sample sample source)
    ocv_add_executable(${sample} ${source})
    ocv_target_link_libraries(${sample} opencv_v4d opencv_plan opencv_core opencv_imgproc opencv_videoio opencv_video opencv_imgcodecs opencv_face opencv_tracking opencv_plot opencv_objdetect opencv_stitching opencv_optflow opencv_features2d opencv_dnn opencv_flann glfw nanovg)
    if(WITH_OPENCL)
      ocv_target_link_libraries(${sample} "${OpenCL_LIBRARIES}")
    endif()
    if(OPENCV_V4D_ENABLE_ES3)
      ocv_target_link_libraries(${sample} GLESv2)
    else()
      ocv_target_link_libraries(${sample} OpenGL GLU)
    endif()
    if(OPENCV_V4D_ENABLE_BGFX)
      ocv_target_link_libraries(${sample} bimg bx bgfx)
    endif()
    if(OPENCV_V4D_ENABLE_MALI)
      ocv_target_link_libraries(${sample} mali)
    endif()
    target_compile_features(${sample} PRIVATE cxx_std_20)
    target_link_directories(${sample} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../../lib")
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
      "${V4D_MODULES_DIR}/tracking/include"
      "${CMAKE_HOME_DIRECTORY}/modules/objdetect/include"
      "${CMAKE_HOME_DIRECTORY}/modules/stitching/include"
      "${CMAKE_HOME_DIRECTORY}/modules/imgcodecs/include"
      "${CMAKE_HOME_DIRECTORY}/modules/highgui/include"
      "${V4D_MODULES_DIR}/plan/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/glad/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/"
      "${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/src/"
      "${V4D_MODULES_DIR}/face/include"
      "${CMAKE_HOME_DIRECTORY}/modules/features2d/include"
      "${CMAKE_HOME_DIRECTORY}/modules/flann/include"
      "${V4D_MODULES_DIR}/optflow/include"
      ${BGFX_INC_DIRS}
      "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/")
endmacro()

if (NOT (TARGET nanovg))
   if(OPENCV_V4D_ENABLE_ES3)
      add_definitions(-DNANOVG_GLES3=1)
    else()
      add_definitions(-DNANOVG_GL3=1)
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/")
    target_compile_options(nanovg PUBLIC -Wno-error)
    target_compile_options(nanovg PUBLIC -pthread)
    target_compile_options(nanovg PUBLIC -fvisibility=default)

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
     ocv_add_module(v4d opencv_plan opencv_core opencv_imgproc opencv_videoio opencv_video opencv_imgcodecs glfw nanovg)
  else()
     ocv_add_module(v4d opencv_plan opencv_core opencv_imgproc opencv_videoio opencv_video)
  endif()

  file(GLOB imgui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/imguicontext.cpp")
  file(GLOB imgui_backend_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_opengl3*.cpp")
  file(GLOB imgui_glfw_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/imgui_impl_glfw.cpp")
  ocv_glob_module_sources("${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/" ${imgui_sources} ${imgui_backend_sources} ${imgui_glfw_sources})
  ocv_module_include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${V4D_MODULES_DIR}/plan/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/glad/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/imgui/backends/"
    "${CMAKE_CURRENT_SOURCE_DIR}/third/nanovg/src/")
   if(OPENCV_V4D_ENABLE_BGFX)
      ocv_module_include_directories(${sample} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bgfx/include/" "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bx/include/" "${CMAKE_CURRENT_SOURCE_DIR}/third/bgfx.cmake/bimg/include/")
  endif()
  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  # Populate assets
  set(LOCAL_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Local assets directory")
  if(EXISTS "${LOCAL_ASSETS_DIR}/lbfmodel.yaml")
    set(LBFMODEL_PATH "${LOCAL_ASSETS_DIR}/lbfmodel.yaml")
  else()
    fetch_file("LBFMODEL" "https://github.com/kurnianggoro/GSOC2017/raw/master/data/lbfmodel.yaml" "70dd8b1657c42d1595d6bd13d97d932877b3bed54a95d3c4733a0f740d1fd66b")
    set(LBFMODEL_PATH "${lbfmodel_SOURCE_DIR}/lbfmodel.yaml")
  endif()

  if(EXISTS "${LOCAL_ASSETS_DIR}/face_detection_yunet_2023mar.onnx")
    set(YUNET_PATH "${LOCAL_ASSETS_DIR}/face_detection_yunet_2023mar.onnx")
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
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/fonts/*.ttf"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/fonts/")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${LBFMODEL_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models/lbfmodel.yaml")

  add_custom_command(TARGET ${the_module} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${YUNET_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/assets/models/face_detection_yunet_2023mar.onnx")

  #Add sample targets
  if(BUILD_EXAMPLES)
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
      add_binary_sample(example_v4d_video-demo samples/video-demo.cpp)
      add_binary_sample(example_v4d_nanovg-demo samples/nanovg-demo.cpp)
      add_binary_sample(example_v4d_font-demo samples/font-demo.cpp)
      add_binary_sample(example_v4d_shader-demo samples/shader-demo.cpp)
      add_binary_sample(example_v4d_pedestrian-demo samples/pedestrian-demo.cpp)
      add_binary_sample(example_v4d_optflow-demo samples/optflow-demo.cpp)
      add_binary_sample(example_v4d_beauty-demo samples/beauty-demo.cpp)
      if(OPENCV_V4D_ENABLE_BGFX)
        add_binary_sample(example_v4d_bgfx-demo samples/bgfx-demo.cpp)
        add_binary_sample(example_v4d_bgfx-demo2 samples/bgfx-demo2.cpp)
      endif()
      add_binary_sample(example_v4d_montage-demo samples/montage-demo.cpp)
  endif()

  if(OPENCV_V4D_ENABLE_ES3)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DOPENCV_V4D_USE_ES3=1")
  endif()

  target_compile_features(${the_module} PRIVATE cxx_std_20)

  target_link_directories(${the_module} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../../lib")
  ocv_target_link_libraries(${the_module} opencv_plan glfw nanovg)
  if(OPENCV_V4D_ENABLE_ES3)
    ocv_target_link_libraries(${the_module} GLESv2)
  else()
    ocv_target_link_libraries(${the_module} OpenGL GLU)
  endif()

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
