set(the_description "Plan: A lock-free dataflow execution framework")
ocv_add_module(plan opencv_core)
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
target_compile_features(${the_module} PRIVATE cxx_std_20)

