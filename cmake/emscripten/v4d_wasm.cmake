# Emscripten CMake helpers for the V4D module.
#
# Native v4d links against GLFW, OpenGL and GLU as separate system libraries.
# Under Emscripten these are provided by the emscripten runtime itself and are
# selected at link time via -sUSE_GLFW=3. This module, included from the v4d
# CMakeLists when EMSCRIPTEN, defines interface targets with the same names so
# the existing link lines keep working unchanged.

# --- GLFW --------------------------------------------------------------------
if(NOT TARGET glfw3)
  find_path(_v4d_glfw_include GLFW/glfw3.h
    PATHS "${EMSDK}/upstream/emscripten/system/include"
    NO_DEFAULT_PATH)
  if(NOT _v4d_glfw_include)
    find_path(_v4d_glfw_include GLFW/glfw3.h)
  endif()
  add_library(glfw3 INTERFACE IMPORTED)
  set_target_properties(glfw3 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_v4d_glfw_include}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_GLFW=3"
    INTERFACE_LINK_OPTIONS "-sUSE_GLFW=3")
endif()
if(NOT TARGET glfw)
  add_library(glfw INTERFACE IMPORTED)
  set_target_properties(glfw PROPERTIES INTERFACE_LINK_LIBRARIES glfw3)
endif()

# --- OpenGL / GLU -------------------------------------------------------------
if(NOT TARGET OpenGL::GL)
  add_library(OpenGL::GL INTERFACE IMPORTED)
endif()
if(NOT TARGET GL)
  add_library(GL INTERFACE IMPORTED)
  set_target_properties(GL PROPERTIES INTERFACE_LINK_LIBRARIES OpenGL::GL)
endif()
add_library(OpenGL ALIAS GL)
if(NOT TARGET OpenGL::GLU)
  add_library(OpenGL::GLU INTERFACE IMPORTED)
  set_target_properties(OpenGL::GLU PROPERTIES INTERFACE_LINK_LIBRARIES OpenGL::GL)
endif()
if(NOT TARGET GLU)
  add_library(GLU INTERFACE IMPORTED)
  set_target_properties(GLU PROPERTIES INTERFACE_LINK_LIBRARIES OpenGL::GLU)
endif()

# --- GLESv2 (WebGL 2 via emscripten runtime) ----------------------------------
if(NOT TARGET GLESv2)
  add_library(GLESv2 INTERFACE IMPORTED)
  set_target_properties(GLESv2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EMSCRIPTEN_SYSROOT}/include"
    INTERFACE_COMPILE_OPTIONS "-sMAX_WEBGL_VERSION=2"
    INTERFACE_LINK_OPTIONS "-sMAX_WEBGL_VERSION=2")
endif()

# --- v4d defines for wasm -----------------------------------------------------
add_definitions(-DOPENCV_V4D_USE_ES3=1 -DV4D_WASM=1)
