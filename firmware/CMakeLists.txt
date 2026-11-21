cmake_minimum_required(VERSION 3.16)

set(MICROBROWSER_PACKAGE "${CMAKE_CURRENT_LIST_DIR}/../build/dashboard.mbp"
    CACHE FILEPATH "Compiled Microbrowser package")
if(NOT EXISTS "${MICROBROWSER_PACKAGE}")
    message(FATAL_ERROR
        "Missing compiled application: ${MICROBROWSER_PACKAGE}\n"
        "Compile a v3 package with: python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/mbc build "
        "${CMAKE_CURRENT_LIST_DIR}/../examples/dashboard.mb -o ${MICROBROWSER_PACKAGE}")
endif()
file(SHA256 "${MICROBROWSER_PACKAGE}" MICROBROWSER_PACKAGE_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MICROBROWSER_PACKAGE}")
message(STATUS "Microbrowser package: ${MICROBROWSER_PACKAGE}; sha256=${MICROBROWSER_PACKAGE_SHA256}")
# Co-profile builds share neither this generated file nor their selected package.
configure_file("${MICROBROWSER_PACKAGE}" "${CMAKE_BINARY_DIR}/application.mbp" COPYONLY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microbrowser_firmware)
