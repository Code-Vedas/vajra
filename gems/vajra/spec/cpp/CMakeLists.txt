cmake_minimum_required(VERSION 3.20)
project(vajra_cpp_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB VAJRA_VENDOR_NGHTTP2_SOURCES CONFIGURE_DEPENDS
  ../../ext/vajra/vendor/nghttp2/lib/*.c
)

add_executable(vajra_server_test
  test_support.cpp
  lifecycle_controller_test.cpp
  rack_env_test.cpp
  server_lifecycle_test.cpp
  request_head_test.cpp
  response_test.cpp
  http2_stream_test.cpp
  runtime_logging_test.cpp
  server_test.cpp
  ../../ext/vajra/lifecycle/lifecycle_controller.cpp
  ../../ext/vajra/listener/listener_socket.cpp
  ../../ext/vajra/rack/http2_stream.cpp
  ../../ext/vajra/rack/rack_execution_profiler.cpp
  ruby_execution_bridge_stub.cpp
  ruby_rack_transport_stub.cpp
  ../../ext/vajra/request/request_body_reader.cpp
  ../../ext/vajra/request/request_executor.cpp
  ../../ext/vajra/request/http2_session.cpp
  ../../ext/vajra/request/request_head_reader.cpp
  ../../ext/vajra/request/request_processor.cpp
  ../../ext/vajra/request/rack_env.cpp
  ../../ext/vajra/response/response_serializer.cpp
  ../../ext/vajra/response/response_writer.cpp
  ../../ext/vajra/runtime/runtime_logging.cpp
  ../../ext/vajra/runtime/runtime_state.cpp
  ../../ext/vajra/runtime/time_utils.cpp
  ../../ext/vajra/server.cpp
  ../../ext/vajra/transport/connection.cpp
  ../../ext/vajra/transport/tls_connection.cpp
  ${VAJRA_VENDOR_NGHTTP2_SOURCES}
)

target_include_directories(vajra_server_test PRIVATE
  ../../ext/vajra
  ../../ext/vajra/vendor/nghttp2/lib/includes
  ../../ext/vajra/vendor/nghttp2/lib
)
target_link_libraries(vajra_server_test PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(vajra_server_test PRIVATE
  VAJRA_RUNTIME_TESTING
  NGHTTP2_STATICLIB
  BUILDING_NGHTTP2
  HAVE_ARPA_INET_H
  HAVE_NETINET_IN_H
)
target_compile_options(vajra_server_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_test(NAME vajra_server_test COMMAND vajra_server_test)
