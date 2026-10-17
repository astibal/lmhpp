cmake_minimum_required(VERSION 3.20)
project(lmhttpdcpp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

add_executable(sample1 examples/sample1.cpp)
target_link_libraries(sample1 PRIVATE microhttpd)
