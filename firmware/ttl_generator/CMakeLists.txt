cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ttl_generator)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(app PRIVATE
  src/main.cpp
  src/debug_log.cpp
  src/record_stream.cpp
  src/hardware.cpp
  src/application.cpp
  src/ble_service.cpp
  ../shared/src/sdc_anchor_report.c
  ../shared/src/transport.cpp
  ../shared/src/record_protocol.cpp
  ../shared/src/command_protocol.cpp
  ../shared/src/discovery_protocol.cpp
)

target_compile_options(app PRIVATE
  -Wall
  -Wextra
  -Wshadow
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

if(DEFINED NODE_ENABLE_DEBUG_STREAM)
  target_compile_definitions(app PRIVATE
    NODE_ENABLE_DEBUG_STREAM=${NODE_ENABLE_DEBUG_STREAM}
  )
endif()
