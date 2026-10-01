cmake_policy(SET CMP0079 NEW)

set(the_description "Plan-DSL Module")

# Check CXX Features
get_property(known_features GLOBAL PROPERTY CMAKE_CXX_KNOWN_FEATURES)
list (FIND known_features "cxx_std_20" idx)
if (${idx} LESS 0)
  message(STATUS "Module opencv_plan disabled because it requires C++20")
  ocv_module_disable(plan)
endif()

# AnyProperty (kallaballa/AnyProperty) is the type erased, thread safe property
# map this module keeps its shared and its per thread state in. It is header
# only and lives as a git submodule in the v4d module's third/ directory. We
# deliberately do not add_subdirectory() it: its own CMakeLists.txt calls
# include(CTest), which would clash with OpenCV's test setup.
set(ANYPROPERTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../v4d/third/AnyProperty")

if(NOT EXISTS "${ANYPROPERTY_DIR}/anyproperty.hpp")
  # v4d also runs 'git submodule update --init --recursive', but that only
  # happens while the v4d module is configured, and a plan-only build never
  # configures it - so do it here too. Skipped in the OBS build environment,
  # which ships the sources unpacked instead.
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../.git" AND NOT DEFINED ENV{OBS_BUILD})
    message(STATUS "Submodule update")
    execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init -- modules/v4d/third/AnyProperty
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
                    RESULT_VARIABLE GIT_SUBMOD_RESULT)
    if(NOT GIT_SUBMOD_RESULT EQUAL "0")
      message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
    endif()
  endif()
endif()

if(NOT EXISTS "${ANYPROPERTY_DIR}/anyproperty.hpp")
  message(FATAL_ERROR "AnyProperty was not found in ${ANYPROPERTY_DIR}. It is a git submodule - run 'git submodule update --init' and try again.")
endif()

if(NOT (TARGET ${the_module}))
  ocv_add_module(plan opencv_core opencv_imgproc)

  ocv_glob_module_sources()
  ocv_module_include_directories("${CMAKE_CURRENT_LIST_DIR}/include" "${ANYPROPERTY_DIR}")

  ocv_create_module()
  set_target_properties(${the_module} PROPERTIES LINKER_LANGUAGE CXX)

  target_compile_features(${the_module} PRIVATE cxx_std_20)

  target_compile_options(${the_module} PRIVATE
    $<$<CONFIG:Release>:-march=native -ftree-vectorize -funroll-loops>
    $<$<CONFIG:RelWithDebInfo>:-march=native -ftree-vectorize -funroll-loops>
  )
endif()

# Accuracy test suite (Plan features only; no V4D).
# Requires C++20 for the test target because plan.hpp uses <barrier>/<semaphore>.
if(BUILD_TESTS AND NOT BUILD_opencv_world)
  ocv_add_accuracy_tests()
  if(TARGET opencv_test_plan)
    target_compile_features(opencv_test_plan PRIVATE cxx_std_20)
    target_include_directories(opencv_test_plan PRIVATE "${ANYPROPERTY_DIR}")
    target_compile_options(opencv_test_plan PRIVATE
      $<$<CONFIG:Release>:-march=native -ftree-vectorize -funroll-loops>
      $<$<CONFIG:RelWithDebInfo>:-march=native -ftree-vectorize -funroll-loops>
    )
  endif()
endif()

# Performance test suite (Plan features only; no V4D).
# Requires C++20 for the perf target too; reuses MockPlanRuntime/TestedPlan
# from the accuracy test dir.
if(BUILD_PERF_TESTS AND NOT BUILD_opencv_world)
  ocv_add_perf_tests()
  if(TARGET opencv_perf_plan)
    target_compile_features(opencv_perf_plan PRIVATE cxx_std_20)
    target_include_directories(opencv_perf_plan PRIVATE "${CMAKE_CURRENT_LIST_DIR}/test" "${ANYPROPERTY_DIR}")
    target_compile_options(opencv_perf_plan PRIVATE
      $<$<CONFIG:Release>:-march=native -ftree-vectorize -funroll-loops>
      $<$<CONFIG:RelWithDebInfo>:-march=native -ftree-vectorize -funroll-loops>
    )
  endif()
endif()
