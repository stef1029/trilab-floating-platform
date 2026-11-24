cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(hub)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(app PRIVATE
  src/main.cpp
  src/debug_log.cpp
  src/timebase.cpp
  src/controller_clock.cpp
  src/rs485_bus.cpp
  src/node_manager.cpp
  src/ble_gateway.cpp
  src/ttl_generator_manager.cpp
  src/experiment.cpp
  src/control.cpp
  src/local_sensors.cpp
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

if(DEFINED RIG_MAX_NODES)
  target_compile_definitions(app PRIVATE
    RIG_MAX_NODES=${RIG_MAX_NODES}
  )
endif()

if(DEFINED NODE_ENABLE_DEBUG_STREAM)
  target_compile_definitions(app PRIVATE
    NODE_ENABLE_DEBUG_STREAM=${NODE_ENABLE_DEBUG_STREAM}
  )
endif()
