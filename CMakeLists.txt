cmake_minimum_required(VERSION 3.20)
project(harbinger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compiler commands for editor tooling")

set(HARBINGER_SANITIZER "" CACHE STRING "Sanitizer to enable: address, undefined, or thread")
option(HARBINGER_BUILD_BENCHMARKS "Build cleanup and synthetic scheduling benchmarks" OFF)
option(HARBINGER_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(harbinger_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(HARBINGER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(HARBINGER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(HARBINGER_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(HARBINGER_SANITIZER STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
elseif(HARBINGER_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT HARBINGER_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unsupported HARBINGER_SANITIZER: ${HARBINGER_SANITIZER}")
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
# Prefer upstream configs; Ubuntu's matching protobuf/gRPC packages use
# FindProtobuf instead. Do not mix dependencies from different installations.
find_package(Threads  REQUIRED)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf MODULE REQUIRED)
endif()
find_package(gRPC     CONFIG REQUIRED)

# ── Proto code generation ─────────────────────────────────────────────────────
get_filename_component(PROTO_FILE "proto/harbinger.proto" ABSOLUTE)
get_filename_component(PROTO_DIR  "proto"              ABSOLUTE)

set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/harbinger.pb.cc")
set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/harbinger.pb.h")
set(GRPC_SRCS  "${CMAKE_CURRENT_BINARY_DIR}/harbinger.grpc.pb.cc")
set(GRPC_HDRS  "${CMAKE_CURRENT_BINARY_DIR}/harbinger.grpc.pb.h")

get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

add_custom_command(
    OUTPUT  ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND protobuf::protoc
    ARGS    --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
            --cpp_out  "${CMAKE_CURRENT_BINARY_DIR}"
            -I "${PROTO_DIR}"
            --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC/protobuf sources from harbinger.proto"
)

add_library(harbinger_proto STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_link_libraries(harbinger_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
target_include_directories(harbinger_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# ── Server-side library (queue + proxy + gRPC service impl) ───────────────────
set(HARBINGER_SERVER_SOURCES
    src/queue/multi_level_queue.cpp
    src/queue/dead_letter_queue.cpp
    src/proxy/proxy.cpp
    src/ml/features.cpp
    src/ml/feedback_event.cpp
    src/ml/feedback_writer.cpp
    src/harbinger_service.cpp
)
add_library(harbinger_server_lib STATIC ${HARBINGER_SERVER_SOURCES})
target_include_directories(harbinger_server_lib PUBLIC include)
target_link_libraries(harbinger_server_lib PUBLIC harbinger_proto Threads::Threads)
harbinger_target_warnings(harbinger_server_lib)
set_property(TARGET harbinger_server_lib PROPERTY INTERFACE_HARBINGER_SERVER_VARIANT ordinary)
set_property(TARGET harbinger_server_lib APPEND PROPERTY COMPATIBLE_INTERFACE_STRING HARBINGER_SERVER_VARIANT)

# ── Client library (Producer + Consumer gRPC clients) ─────────────────────────
add_library(harbinger_client_lib STATIC
    src/producer/producer.cpp
    src/consumer/consumer.cpp
)
target_include_directories(harbinger_client_lib PUBLIC include)
target_link_libraries(harbinger_client_lib PUBLIC harbinger_proto Threads::Threads)
harbinger_target_warnings(harbinger_client_lib)

# ── Server binary ─────────────────────────────────────────────────────────────
add_executable(harbinger_server server/main.cpp)
target_link_libraries(harbinger_server PRIVATE harbinger_server_lib gRPC::grpc++)
harbinger_target_warnings(harbinger_server)

# ── Tests ────────────────────────────────────────────────────────────────────
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(tests)

# ── Demo ─────────────────────────────────────────────────────────────────────
add_subdirectory(demo)
if(HARBINGER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
