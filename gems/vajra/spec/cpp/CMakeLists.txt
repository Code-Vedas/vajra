cmake_minimum_required(VERSION 3.20)
project(vajra_cpp_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(vajra_server_test
  server_test.cpp
  ../../ext/vajra/server.cpp
)

target_include_directories(vajra_server_test PRIVATE ../../ext/vajra)
target_link_libraries(vajra_server_test PRIVATE Threads::Threads)
target_compile_options(vajra_server_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_test(NAME vajra_server_test COMMAND vajra_server_test)
