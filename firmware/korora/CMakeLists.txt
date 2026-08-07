cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(korora)

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
  src/fairy_manager.cpp
  src/ble_gateway.cpp
  src/galapagos_manager.cpp
  src/experiment.cpp
  src/control.cpp
  ../shared/src/sdc_anchor_report.c
  ../shared/src/transport.cpp
  ../shared/src/fairy_protocol.cpp
  ../shared/src/adelie_protocol.cpp
  ../shared/src/magellan_protocol.cpp
)

target_compile_options(app PRIVATE
  -Wall
  -Wextra
  -Wshadow
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

if(DEFINED FAIRY_MAX_BOARDS)
  target_compile_definitions(app PRIVATE
    FAIRY_MAX_BOARDS=${FAIRY_MAX_BOARDS}
  )
endif()

if(DEFINED FAIRY_ENABLE_DEBUG_STREAM)
  target_compile_definitions(app PRIVATE
    FAIRY_ENABLE_DEBUG_STREAM=${FAIRY_ENABLE_DEBUG_STREAM}
  )
endif()
