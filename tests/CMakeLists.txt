# ── Unit tests: internal components (no gRPC) ─────────────────────────────────
# These tests exercise MultiLevelQueue, DeadLetterQueue, and Proxy directly
# without starting a server. Fast and self-contained.
add_executable(harbinger_unit_tests
    unit/test_multi_level_queue.cpp
    unit/test_dead_letter_queue.cpp
    unit/test_proxy.cpp
    unit/test_ml_features.cpp
    unit/test_ml_feedback.cpp
    unit/test_feedback_writer.cpp
    unit/test_duration_predictor.cpp
)
target_compile_definitions(harbinger_unit_tests PRIVATE
    ML_FEATURE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ml/features_v1.json"
    ML_FEEDBACK_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ml/feedback_v1.json")
target_link_libraries(harbinger_unit_tests
    PRIVATE harbinger_server_lib GTest::gtest_main
)
harbinger_target_warnings(harbinger_unit_tests)

# ── Integration tests: gRPC broker + client ────────────────────────────────────
# These tests start a real gRPC server on port 0 and exercise the full
# message flow: producer → broker → consumer → ack/nack → DLQ.
add_executable(harbinger_integration_tests
    unit/test_broker.cpp
    unit/test_client.cpp
)
target_link_libraries(harbinger_integration_tests
    PRIVATE harbinger_server_lib harbinger_client_lib GTest::gtest_main
)
harbinger_target_warnings(harbinger_integration_tests)

include(GoogleTest)
gtest_discover_tests(harbinger_unit_tests PROPERTIES TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(harbinger_integration_tests PROPERTIES TIMEOUT 60 DISCOVERY_MODE PRE_TEST)

option(HARBINGER_REQUIRE_PYTHON_TESTS "Require Python for cross-language ML feature tests" OFF)
if(HARBINGER_REQUIRE_PYTHON_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
else()
    find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME ml_feature_python_tests COMMAND "${Python3_EXECUTABLE}"
        -m unittest discover -s ml_engine/tests -v)
    set_tests_properties(ml_feature_python_tests PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 60
        ENVIRONMENT "HARBINGER_PREDICTOR_REPLAY=$<TARGET_FILE:harbinger_predictor_replay>")
endif()

if(UNIX)
    add_executable(harbinger_server_shutdown_tests integration/test_server_shutdown.cpp)
    target_link_libraries(harbinger_server_shutdown_tests PRIVATE harbinger_proto GTest::gtest_main)
    target_compile_definitions(harbinger_server_shutdown_tests PRIVATE
        SERVER_EXECUTABLE="$<TARGET_FILE:harbinger_server>")
    add_dependencies(harbinger_server_shutdown_tests harbinger_server)
    harbinger_target_warnings(harbinger_server_shutdown_tests)
    gtest_discover_tests(harbinger_server_shutdown_tests PROPERTIES TIMEOUT 30 DISCOVERY_MODE PRE_TEST)
endif()
