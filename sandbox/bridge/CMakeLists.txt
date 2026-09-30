cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bridge)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/shared/include)

target_sources(app PRIVATE
  src/main.cpp
  ../../firmware/shared/src/transport.cpp)

target_compile_options(app PRIVATE
  -Wall -Wextra
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
