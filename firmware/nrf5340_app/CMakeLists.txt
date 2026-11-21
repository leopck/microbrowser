cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microbrowser_nrf5340_app LANGUAGES C CXX)

if(NOT BOARD STREQUAL "nrf5340bsim" AND
   NOT BOARD STREQUAL "nrf7002dk")
    message(FATAL_ERROR
        "Select an explicit supported board: nrf5340bsim/nrf5340/cpuapp or nrf7002dk/nrf5340/cpuapp")
endif()

set(MICROBROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(MICROBROWSER_PACKAGE "${MICROBROWSER_ROOT}/build/gpio_conformance.mbp"
    CACHE FILEPATH "Exact compiled v3 .mbp artifact embedded in this application")
if(NOT EXISTS "${MICROBROWSER_PACKAGE}")
    message(FATAL_ERROR
        "Missing ${MICROBROWSER_PACKAGE}. Build it with:\n"
        "python3 ${MICROBROWSER_ROOT}/tools/mbc build "
        "${MICROBROWSER_ROOT}/examples/gpio_conformance.mb -o ${MICROBROWSER_PACKAGE}")
endif()
file(SIZE "${MICROBROWSER_PACKAGE}" MICROBROWSER_PACKAGE_SIZE)
if(MICROBROWSER_PACKAGE_SIZE LESS 20 OR MICROBROWSER_PACKAGE_SIZE GREATER 16777216)
    message(FATAL_ERROR "The Microbrowser package size is outside the v3 format limits")
endif()
if(DEFINED CONFIG_MB_PACKAGE_MAX_BYTES AND
   MICROBROWSER_PACKAGE_SIZE GREATER CONFIG_MB_PACKAGE_MAX_BYTES)
    message(FATAL_ERROR "Embedded package exceeds CONFIG_MB_PACKAGE_MAX_BYTES: ${MICROBROWSER_PACKAGE_SIZE} bytes")
endif()
file(SHA256 "${MICROBROWSER_PACKAGE}" MICROBROWSER_PACKAGE_DIGEST)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MICROBROWSER_PACKAGE}")

set(MB_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/generated")
file(GENERATE OUTPUT "${MB_GENERATED_DIR}/package_identity.h"
    CONTENT "#pragma once\n#define MB_EMBEDDED_PACKAGE_SHA256 \"${MICROBROWSER_PACKAGE_DIGEST}\"\n")
generate_inc_file_for_target(app "${MICROBROWSER_PACKAGE}"
    "${MB_GENERATED_DIR}/microbrowser_package.inc")


set(MICROBROWSER_TLS_CA_CERTIFICATE "" CACHE FILEPATH
    "Optional owner-provided DER CA certificate installed into MB_TLS_CA_SEC_TAG at startup")
if(NOT MICROBROWSER_TLS_CA_CERTIFICATE STREQUAL "")
    if(NOT EXISTS "${MICROBROWSER_TLS_CA_CERTIFICATE}")
        message(FATAL_ERROR "Owner CA certificate does not exist: ${MICROBROWSER_TLS_CA_CERTIFICATE}")
    endif()
    if(NOT CONFIG_MB_HTTP_ENABLED OR CONFIG_MB_TLS_CA_SEC_TAG LESS 0)
        message(FATAL_ERROR "Embedding a CA requires MB_HTTP_ENABLED and an explicit non-negative MB_TLS_CA_SEC_TAG")
    endif()
    generate_inc_file_for_target(app "${MICROBROWSER_TLS_CA_CERTIFICATE}"
        "${MB_GENERATED_DIR}/microbrowser_tls_ca.der.inc")
    target_compile_definitions(app PRIVATE MB_EMBED_TLS_CA_CERTIFICATE=1)
endif()

target_sources(app PRIVATE
    src/main.cpp
    "${MICROBROWSER_ROOT}/platform/zephyr_platform.cpp"
    "${MICROBROWSER_ROOT}/platform/zephyr_http.cpp"
    "${MICROBROWSER_ROOT}/platform/zephyr_peripherals.cpp"
    "${MICROBROWSER_ROOT}/platform/zephyr_event_link.cpp"
    "${MICROBROWSER_ROOT}/core/peripherals.cpp"
    "${MICROBROWSER_ROOT}/core/device_builtins.cpp"
    "${MICROBROWSER_ROOT}/core/browser.cpp"
    "${MICROBROWSER_ROOT}/core/events.cpp"
    "${MICROBROWSER_ROOT}/core/builtins.cpp"
    "${MICROBROWSER_ROOT}/core/package.cpp"
    "${MICROBROWSER_ROOT}/core/overlay.cpp"
    "${MICROBROWSER_ROOT}/core/paint.cpp"
    "${MICROBROWSER_ROOT}/core/vm.cpp"
    "${MICROBROWSER_ROOT}/core/layout.cpp"
    "${MICROBROWSER_ROOT}/core/canvas.cpp"
    "${MICROBROWSER_ROOT}/core/font_data.cpp"
    "${MICROBROWSER_ROOT}/core/fonts.cpp"
    "${MICROBROWSER_ROOT}/core/input.cpp"
    "${MICROBROWSER_ROOT}/core/text_layout.cpp"
    "${MICROBROWSER_ROOT}/core/json.cpp"
    "${MICROBROWSER_ROOT}/core/types.cpp"
    "${MICROBROWSER_ROOT}/core/reader_page.cpp"
    "${MICROBROWSER_ROOT}/core/url_navigation.cpp"
)

if(CONFIG_MB_BLE_CENTRAL)
    target_sources(app PRIVATE "${MICROBROWSER_ROOT}/platform/zephyr_ble_central.cpp")
endif()
target_include_directories(app PRIVATE
    "${MB_GENERATED_DIR}"
    "${MICROBROWSER_ROOT}/include"
    "${MICROBROWSER_ROOT}"
    "${MICROBROWSER_ROOT}/core"
)

if(BOARD STREQUAL "nrf5340bsim")
    target_compile_definitions(app PRIVATE MB_BSIM_COMPLETION_TEST=1)
endif()
target_compile_features(app PRIVATE cxx_std_20)
