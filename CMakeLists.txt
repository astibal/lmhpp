cmake_minimum_required(VERSION 3.19)
project(lmhttpdcpp CXX)

set(CMAKE_CXX_STANDARD 17)

include_directories(include)

add_executable(sample1 examples/sample1.cpp)
target_link_libraries(sample1 PRIVATE microhttpd)