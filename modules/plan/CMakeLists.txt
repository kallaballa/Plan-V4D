cmake_policy(SET CMP0079 NEW)

set(the_description "Plan-DSL Module")

# Check CXX Features
get_property(known_features GLOBAL PROPERTY CMAKE_CXX_KNOWN_FEATURES)
list (FIND known_features "cxx_std_20" idx)
if (${idx} LESS 0)
  message(STATUS "Module opencv_plan disabled because it requires C++20")
  ocv_module_disable(plan)
endif()

if(NOT (TARGET ${the_module}))
  ocv_add_module(plan opencv_core opencv_imgproc)

  ocv_glob_module_sources()
  ocv_module_include_directories("${CMAKE_CURRENT_LIST_DIR}/include")

  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  target_compile_features(${the_module} PRIVATE cxx_std_20)
endif()
