set(the_description "Plan: A dataflow execution framework")

# Plan is a BASE module: it must not depend on any other OpenCV module.
# It sits below opencv_core in the dependency graph; opencv_core (and all
# other modules) may depend on opencv_plan instead.
ocv_add_module(plan)

ocv_glob_module_sources(
  src/plan.cpp
  src/util.cpp
  src/transaction.cpp
  src/timetracker.cpp
  src/resequence.cpp
)

ocv_module_include_directories(
  ${CMAKE_CURRENT_LIST_DIR}/include
)

ocv_create_module()

# Plan headers require C++20 (barriers, semaphores, latches, ...), so every
# module that depends on Plan must be compiled with C++20 as well.
target_compile_features(${the_module} PUBLIC cxx_std_20)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(${the_module} PRIVATE PLAN_BUILDING_DLL)
  target_compile_definitions(${the_module} PUBLIC PLAN_DLL)
endif()
