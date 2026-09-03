cmake_minimum_required(VERSION 3.15)
project(toons_flag_chase LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizer configuration, opt-in only: the default (none) build is untouched.
#   cmake -S . -B build-tsan -DSANITIZER=thread  -DCMAKE_BUILD_TYPE=Debug
#   cmake -S . -B build-asan -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
set(SANITIZER "none" CACHE STRING "Sanitizer build: none|thread|address")
set_property(CACHE SANITIZER PROPERTY STRINGS none thread address)

# Lock-contention instrumentation, opt-in and separate from the sanitizer
# builds above: times every board.mtx acquisition/release (see BoardLock in
# src/main.cpp). This overhead would skew the real throughput/latency
# numbers, so it lives in its own build directory and is never combined
# with -DSANITIZER or with the numbers used for the actual benchmark:
#   cmake -S . -B build-instrumented -DLOCK_INSTRUMENTATION=ON -DCMAKE_BUILD_TYPE=Release
option(LOCK_INSTRUMENTATION "Instrument board.mtx wait/hold time (skews throughput; separate build only)" OFF)

add_executable(toons src/main.cpp)

if(LOCK_INSTRUMENTATION)
  target_compile_definitions(toons PRIVATE LOCK_INSTRUMENTATION)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(toons pthread)
endif()

if(SANITIZER STREQUAL "thread")
  target_compile_options(toons PRIVATE -fsanitize=thread -g -O1 -pthread)
  target_link_options(toons PRIVATE -fsanitize=thread -pthread)
elseif(SANITIZER STREQUAL "address")
  target_compile_options(toons PRIVATE -fsanitize=address,undefined -g -pthread)
  target_link_options(toons PRIVATE -fsanitize=address,undefined -pthread)
elseif(NOT SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown SANITIZER value '${SANITIZER}': expected none|thread|address")
endif()
