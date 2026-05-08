add_executable(pmlmq_demo main.cpp)
target_link_libraries(pmlmq_demo PRIVATE pmlmq_server_lib pmlmq_client_lib gRPC::grpc++)
