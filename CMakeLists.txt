cmake_minimum_required(VERSION 3.15)
project(kill-all-moa CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kill-all-moa src/kill-all-moa.cpp)

if(MSVC)
  target_compile_options(kill-all-moa PRIVATE /W3 /O2 /EHsc)
else()
  target_compile_options(kill-all-moa PRIVATE -O2 -Wall)
endif()
