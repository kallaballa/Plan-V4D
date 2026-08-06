set(the_description "Plan: a parallel, transactional dataflow-graph DSL")

ocv_add_module(plan opencv_core opencv_imgproc)

ocv_module_include_directories(
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/src"
)

file(GLOB_RECURSE plan_srcs
  "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp"
)
file(GLOB_RECURSE plan_hdrs
  "${CMAKE_CURRENT_LIST_DIR}/include/*.hpp"
)

ocv_set_module_sources(HEADERS ${plan_hdrs} SOURCES ${plan_srcs})
ocv_create_module()

ocv_add_accuracy_tests()
ocv_add_samples()

