cmake_minimum_required(VERSION 3.20)
project(vajra_cpp_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(vajra_server_test
  test_support.cpp
  ipc_contract_test.cpp
  ipc_frame_contract_test.cpp
  ipc_frame_header_test.cpp
  ipc_protocol_compatibility_test.cpp
  server_lifecycle_test.cpp
  request_head_test.cpp
  response_test.cpp
  server_test.cpp
  ../../ext/vajra/ipc/protocol_contract.cpp
  ../../ext/vajra/ipc/frame_header.cpp
  ../../ext/vajra/listener/listener_socket.cpp
  ../../ext/vajra/request/request_head_reader.cpp
  ../../ext/vajra/request/request_processor.cpp
  ../../ext/vajra/response/response_serializer.cpp
  ../../ext/vajra/response/response_writer.cpp
  ../../ext/vajra/server.cpp
)

target_include_directories(vajra_server_test PRIVATE ../../ext/vajra)
target_link_libraries(vajra_server_test PRIVATE Threads::Threads)
target_compile_options(vajra_server_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_test(NAME vajra_server_test COMMAND vajra_server_test)
