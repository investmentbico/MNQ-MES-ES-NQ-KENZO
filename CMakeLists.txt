cmake_minimum_required(VERSION 3.14)
project(ema21_flip_backtester CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build with optimizations by default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

add_executable(backtester ema21_flip_backtest.cpp)

# Optional: enable warnings
target_compile_options(backtester PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output binary: backtester")
