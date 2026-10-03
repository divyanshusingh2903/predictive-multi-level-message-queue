add_executable(harbinger_cleanup_benchmark phase1_cleanup.cpp)
target_link_libraries(harbinger_cleanup_benchmark PRIVATE harbinger_server_lib harbinger_client_lib)
harbinger_target_warnings(harbinger_cleanup_benchmark)

set(benchmark_server_sources ${HARBINGER_SERVER_SOURCES})
list(TRANSFORM benchmark_server_sources PREPEND "${PROJECT_SOURCE_DIR}/")
add_library(harbinger_benchmark_server_lib STATIC ${benchmark_server_sources})
target_sources(harbinger_benchmark_server_lib PRIVATE hooks.cpp)
target_include_directories(harbinger_benchmark_server_lib PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(harbinger_benchmark_server_lib PUBLIC HARBINGER_BENCHMARK_SUPPORT)
target_link_libraries(harbinger_benchmark_server_lib PUBLIC harbinger_proto Threads::Threads)
harbinger_target_warnings(harbinger_benchmark_server_lib)
set_property(TARGET harbinger_benchmark_server_lib PROPERTY INTERFACE_HARBINGER_SERVER_VARIANT benchmark)
set_property(TARGET harbinger_benchmark_server_lib APPEND PROPERTY COMPATIBLE_INTERFACE_STRING HARBINGER_SERVER_VARIANT)

add_executable(harbinger_synthetic_benchmark synthetic_replay.cpp)
target_link_libraries(harbinger_synthetic_benchmark PRIVATE harbinger_benchmark_server_lib)
harbinger_target_warnings(harbinger_synthetic_benchmark)

add_executable(harbinger_benchmark_tests tests/test_replay.cpp)
target_link_libraries(harbinger_benchmark_tests PRIVATE harbinger_benchmark_server_lib GTest::gtest_main)
harbinger_target_warnings(harbinger_benchmark_tests)
include(GoogleTest)
gtest_discover_tests(harbinger_benchmark_tests PROPERTIES TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME synthetic_benchmark_python_tests COMMAND "${Python3_EXECUTABLE}"
        -m unittest discover -s benchmarks/tests -v)
    set_tests_properties(synthetic_benchmark_python_tests PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 60
        ENVIRONMENT "HARBINGER_BENCHMARK_RUNNER=$<TARGET_FILE:harbinger_synthetic_benchmark>")
endif()
