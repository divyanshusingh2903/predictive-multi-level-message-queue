add_executable(harbinger_demo main.cpp)
target_link_libraries(harbinger_demo PRIVATE harbinger_server_lib harbinger_client_lib gRPC::grpc++)
