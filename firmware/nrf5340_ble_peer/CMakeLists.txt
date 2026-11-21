cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf5340_ble_peer LANGUAGES C)

if(NOT BOARD STREQUAL "nrf5340bsim" AND NOT BOARD STREQUAL "nrf7002dk")
    message(FATAL_ERROR
        "Select nrf5340bsim/nrf5340/cpuapp or nrf7002dk/nrf5340/cpuapp")
endif()

target_sources(app PRIVATE src/main.c)
