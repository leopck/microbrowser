cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microbrowser_nrf5340_gpio LANGUAGES C CXX)

set(MICROBROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
target_sources(app PRIVATE
    src/main.cpp
    "${MICROBROWSER_ROOT}/core/peripherals.cpp"
    "${MICROBROWSER_ROOT}/platform/zephyr_peripherals.cpp"
    "${MICROBROWSER_ROOT}/platform/zephyr_event_link.cpp"
)
target_include_directories(app PRIVATE
    "${MICROBROWSER_ROOT}/include"
    "${MICROBROWSER_ROOT}"
)
target_compile_features(app PRIVATE cxx_std_20)

if(BOARD STREQUAL "nrf5340bsim")
    target_compile_definitions(app PRIVATE MB_GPIO_EXPECT_SIMULATED_EDGES=1 MB_BSIM_EVENT_LINK_TEST=1)
endif()
