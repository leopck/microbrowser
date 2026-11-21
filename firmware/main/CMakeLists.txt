set(MICROBROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS
        "main.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf_peripherals.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf_ble_central.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf_spi.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf_i2c.cpp"
        "${MICROBROWSER_ROOT}/platform/esp_idf_i2s.cpp"
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
    INCLUDE_DIRS
        "${MICROBROWSER_ROOT}/include"
        "${MICROBROWSER_ROOT}/platform"
        "${MICROBROWSER_ROOT}/core"
    PRIV_REQUIRES
        cxx
        esp_driver_gpio
        esp_driver_i2c
        bt
        esp_driver_i2s
        esp_driver_spi
        esp_driver_uart
        esp_eth
        esp_event
        esp_http_client
        esp_lcd
        esp_netif
        esp_psram
        esp_timer
        heap
        mbedtls
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/application.mbp" "BINARY")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MB_EMBEDDED_PACKAGE_SHA256="${MICROBROWSER_PACKAGE_SHA256}")
