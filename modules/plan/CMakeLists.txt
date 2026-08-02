set(the_description "Plan Graph Execution Module")
set(OPENCV_MODULE_IS_PART_OF_WORLD ON)

ocv_add_module(plan opencv_core)

ocv_glob_module_sources()
ocv_module_include_directories()
ocv_create_module()

target_compile_features(${the_module} PRIVATE cxx_std_20)

# Tests
if(BUILD_TESTS)
    ocv_add_testdata(${CMAKE_CURRENT_SOURCE_DIR}/testdata
                     ${CMAKE_CURRENT_BINARY_DIR}/testdata)

    set(OPENCV_PLAN_TEST_DEPS opencv_plan opencv_core opencv_imgproc)

    ocv_add_executable(plan_edges test/test_plan_edges.cpp)
    ocv_target_link_libraries(plan_edges ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_edges PRIVATE cxx_std_20)
    add_test(NAME plan.edges COMMAND plan_edges)
    ocv_target_include_directories(plan_edges "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(plan_transactions test/test_plan_transactions.cpp)
    ocv_target_link_libraries(plan_transactions ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_transactions PRIVATE cxx_std_20)
    add_test(NAME plan.transactions COMMAND plan_transactions)
    ocv_target_include_directories(plan_transactions "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(plan_branching test/test_plan_branching.cpp)
    ocv_target_link_libraries(plan_branching ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_branching PRIVATE cxx_std_20)
    add_test(NAME plan.branching COMMAND plan_branching)
    ocv_target_include_directories(plan_branching "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(plan_state test/test_plan_state.cpp)
    ocv_target_link_libraries(plan_state ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_state PRIVATE cxx_std_20)
    add_test(NAME plan.state COMMAND plan_state)
    ocv_target_include_directories(plan_state "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(plan_shared test/test_plan_shared.cpp)
    ocv_target_link_libraries(plan_shared ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_shared PRIVATE cxx_std_20)
    add_test(NAME plan.shared COMMAND plan_shared)
    ocv_target_include_directories(plan_shared "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(plan_integration test/test_plan_integration.cpp)
    ocv_target_link_libraries(plan_integration ${OPENCV_PLAN_TEST_DEPS} opencv_plan opencv_core opencv_imgproc)
    target_compile_features(plan_integration PRIVATE cxx_std_20)
    add_test(NAME plan.integration COMMAND plan_integration)
    ocv_target_include_directories(plan_integration "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")
endif()

# Samples
if(BUILD_EXAMPLES)
    ocv_add_executable(example_plan_simple_counter samples/simple_counter.cpp)
    ocv_target_link_libraries(example_plan_simple_counter opencv_plan opencv_core)
    target_compile_features(example_plan_simple_counter PRIVATE cxx_std_20)
    ocv_target_include_directories(example_plan_simple_counter "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(example_plan_branching samples/branching_demo.cpp)
    ocv_target_link_libraries(example_plan_branching opencv_plan opencv_core)
    target_compile_features(example_plan_branching PRIVATE cxx_std_20)
    ocv_target_include_directories(example_plan_branching "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(example_plan_subplan samples/subplan_demo.cpp)
    ocv_target_link_libraries(example_plan_subplan opencv_plan opencv_core)
    target_compile_features(example_plan_subplan PRIVATE cxx_std_20)
    ocv_target_include_directories(example_plan_subplan "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(example_plan_shared_state samples/shared_state_demo.cpp)
    ocv_target_link_libraries(example_plan_shared_state opencv_plan opencv_core)
    target_compile_features(example_plan_shared_state PRIVATE cxx_std_20)
    ocv_target_include_directories(example_plan_shared_state "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

    ocv_add_executable(example_plan_image_pipeline samples/image_pipeline_demo.cpp)
    ocv_target_link_libraries(example_plan_image_pipeline opencv_plan opencv_core opencv_imgproc)
    target_compile_features(example_plan_image_pipeline PRIVATE cxx_std_20)
    ocv_target_include_directories(example_plan_image_pipeline "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${CMAKE_HOME_DIRECTORY}/modules/core/include" "${CMAKE_HOME_DIRECTORY}/modules/imgproc/include")

endif()
