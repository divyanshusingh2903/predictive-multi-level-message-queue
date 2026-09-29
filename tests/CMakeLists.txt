# ── Unit tests: internal components (no gRPC) ─────────────────────────────────
# These tests exercise MultiLevelQueue, DeadLetterQueue, and Proxy directly
# without starting a server. Fast and self-contained.
add_executable(harbinger_unit_tests
    unit/test_multi_level_queue.cpp
    unit/test_dead_letter_queue.cpp
    unit/test_proxy.cpp
)
target_link_libraries(harbinger_unit_tests
    PRIVATE harbinger_server_lib GTest::gtest_main
)

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

include(GoogleTest)
gtest_discover_tests(harbinger_unit_tests PROPERTIES TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(harbinger_integration_tests PROPERTIES TIMEOUT 60 DISCOVERY_MODE PRE_TEST)

if(UNIX)
    add_executable(harbinger_server_shutdown_tests integration/test_server_shutdown.cpp)
    target_link_libraries(harbinger_server_shutdown_tests PRIVATE harbinger_proto GTest::gtest_main)
    target_compile_definitions(harbinger_server_shutdown_tests PRIVATE
        SERVER_EXECUTABLE="$<TARGET_FILE:harbinger_server>")
    add_dependencies(harbinger_server_shutdown_tests harbinger_server)
    gtest_discover_tests(harbinger_server_shutdown_tests PROPERTIES TIMEOUT 30 DISCOVERY_MODE PRE_TEST)
endif()
