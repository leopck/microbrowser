cmake_minimum_required(VERSION 3.16)

set(MICROBROWSER_DASHBOARD_PACKAGE "${CMAKE_CURRENT_LIST_DIR}/../build/dashboard.mbp"
    CACHE FILEPATH "Compiled Microbrowser dashboard package")
if(NOT EXISTS "${MICROBROWSER_DASHBOARD_PACKAGE}")
    message(FATAL_ERROR
        "Missing compiled dashboard: ${MICROBROWSER_DASHBOARD_PACKAGE}\n"
        "Prepare it with: python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/mbc build "
        "${CMAKE_CURRENT_LIST_DIR}/../examples/dashboard.mb -o ${MICROBROWSER_DASHBOARD_PACKAGE}")
endif()
# The binary-data rule needs a component-relative input. This is generated during
# configuration and intentionally excluded by the repository ignore rules.
configure_file("${MICROBROWSER_DASHBOARD_PACKAGE}" "${CMAKE_CURRENT_LIST_DIR}/main/dashboard.mbp" COPYONLY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microbrowser_firmware)
