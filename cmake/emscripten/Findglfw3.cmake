# Emscripten provides GLFW 3.x as part of the emscripten runtime (enabled with
# -sUSE_GLFW=3). There is no separate library to link; the C++ headers are part
# of the emscripten system include path. This module creates an imported
# INTERFACE target named "glfw3" (and the alias "glfw") that v4d links against.

if(NOT EMSCRIPTEN)
  return()
endif()

if(TARGET glfw3)
  set(glfw3_FOUND TRUE)
  return()
endif()

find_path(glfw3_INCLUDE_DIR GLFW/glfw3.h
  PATHS "${EMSDK}/upstream/emscripten/system/include"
  NO_DEFAULT_PATH)

if(glfw3_INCLUDE_DIR)
  add_library(glfw3 INTERFACE IMPORTED)
  set_target_properties(glfw3 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${glfw3_INCLUDE_DIR}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_GLFW=3"
    INTERFACE_LINK_OPTIONS "-sUSE_GLFW=3;-sALLOW_MEMORY_GROWTH=1")
  add_library(glfw ALIAS glfw3)
  set(glfw3_FOUND TRUE)
  set(glfw3_VERSION 3)
endif()

mark_as_advanced(glfw3_INCLUDE_DIR)
