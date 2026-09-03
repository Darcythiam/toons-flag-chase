cmake_minimum_required(VERSION 3.15)
project(toons_flag_chase LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizer configuration, opt-in only: the default (none) build is untouched.
#   cmake -S . -B build-tsan -DSANITIZER=thread  -DCMAKE_BUILD_TYPE=Debug
#   cmake -S . -B build-asan -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
set(SANITIZER "none" CACHE STRING "Sanitizer build: none|thread|address")
set_property(CACHE SANITIZER PROPERTY STRINGS none thread address)

add_executable(toons src/main.cpp)

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
