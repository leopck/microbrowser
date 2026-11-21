#include "microbrowser/canvas.h"
#include "microbrowser/browser.h"
#include "esp_idf.h"
#include "esp_idf_peripherals.h"
#include "esp_idf_spi.h"
#include "esp_idf_i2c.h"
#include "esp_idf_i2s.h"
#include "esp_idf_ble_central.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <span>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern const uint8_t application_mbp_start[] asm("_binary_application_mbp_start");
extern const uint8_t application_mbp_end[] asm("_binary_application_mbp_end");

namespace {

constexpr char kLogTag[] = "microbrowser_app";
constexpr mb::Size kScreenSize{480, 320};

struct TouchPoint {
    int x;
    int y;
};
constexpr TouchPoint kScrollFrom{400, 240};
constexpr TouchPoint kScrollTo{400, 40};
constexpr int kUpwardDragsToBottom = 4;
constexpr TouchPoint kSelftestButtonCenter{240, 120};
constexpr TouchPoint kSelftestButtonPixel{40, 112};
constexpr std::size_t kMaximumCommandBytes = 2051;
constexpr std::size_t kMaximumUartBytesPerPoll = 64;
constexpr uint32_t kApplicationPeriodMs = 10;
constexpr std::size_t kUartReceiveBufferBytes = 4096;

void check_esp(esp_err_t error, std::string_view operation) {
    if (error == ESP_OK) return;
    std::string message(operation);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

int print_length(std::string_view text) {
    if (text.size() > std::size_t(std::numeric_limits<int>::max())) throw std::runtime_error("text exceeds UART formatting limits");
    return int(text.size());
}

class QemuDisplay final {
public:
    QemuDisplay() {
        esp_lcd_rgb_qemu_config_t config = {
            .width = uint32_t(kScreenSize.width),
            .height = uint32_t(kScreenSize.height),
            .bpp = RGB_QEMU_BPP_16,
        };
        check_esp(esp_lcd_new_rgb_qemu(&config, &m_panel), "creating QEMU RGB panel");
        check_esp(esp_lcd_panel_reset(m_panel), "resetting QEMU RGB panel");
        check_esp(esp_lcd_panel_init(m_panel), "initializing QEMU RGB panel");
        void* framebuffer = nullptr;
        check_esp(esp_lcd_rgb_qemu_get_frame_buffer(m_panel, &framebuffer), "acquiring the QEMU framebuffer");
        m_frame_buffer = static_cast<uint16_t*>(framebuffer);
    }

    ~QemuDisplay() {
        if (m_panel != nullptr) {
            const esp_err_t error = esp_lcd_panel_del(m_panel);
            if (error != ESP_OK) ESP_LOGE(kLogTag, "deleting QEMU RGB panel: %s", esp_err_to_name(error));
        }
    }

    QemuDisplay(const QemuDisplay&) = delete;
    QemuDisplay& operator=(const QemuDisplay&) = delete;
    QemuDisplay(QemuDisplay&&) = delete;
    QemuDisplay& operator=(QemuDisplay&&) = delete;

    void present(const mb::Browser& browser) {
        const mb::Damage& damage = browser.damage();
        for (std::size_t index = 0; index < damage.count; ++index) present_region(browser, damage.regions[index]);
    }

private:
    void present_region(const mb::Browser& browser, mb::Rect region) {
        if (region.empty()) return;
        for (int row = 0; row < region.height; ++row) {
            const int y = region.y + row;
            const std::size_t pixel_offset = std::size_t(y) * std::size_t(kScreenSize.width) + std::size_t(region.x);
            const uint16_t* source = browser.pixels() + pixel_offset;
            uint16_t* destination = m_frame_buffer + pixel_offset;
            const int height = region.width == kScreenSize.width ? region.height : 1;
            std::memcpy(destination, source, std::size_t(region.width) * std::size_t(height) * sizeof(uint16_t));
            check_esp(esp_lcd_panel_draw_bitmap(m_panel, region.x, y, region.x + region.width, y + height, destination),
                "presenting QEMU framebuffer damage");
            if (region.width == kScreenSize.width) return;
        }
    }

    esp_lcd_panel_handle_t m_panel = nullptr;
    uint16_t* m_frame_buffer = nullptr; // Borrowed from the QEMU panel; not owned or freed by QemuDisplay.
};

std::size_t internal_heap_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

std::size_t internal_heap_minimum_free() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

std::size_t main_stack_minimum_free_bytes() {
    return uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
}

void print_page(const mb::Browser& browser) {
    const std::string_view page = browser.page_name();
    std::printf("PAGE name=%.*s error_present=%u\n", print_length(page), page.data(), browser.last_error().empty() ? 0U : 1U);
}

void print_stats(const mb::Browser& browser, const mb::EspIdfPlatform& platform) {
    const mb::Metrics& metrics = browser.metrics();
    const std::string_view page = browser.page_name();
    std::printf(
        "MICROBROWSER_STATS page=%.*s nodes=%" PRIu32 " frames=%" PRIu64
        " scripts=%" PRIu64 " layouts=%" PRIu64 " layout_us=%" PRIu64
        " render_us=%" PRIu64 " package_bytes=%" PRIu64 " active_requests=%" PRIu32
        " heap_free=%zu heap_min_free=%zu psram_total=%zu psram_free=%zu network=%s memory_source=emulated stack_min_free_bytes=%zu\n",
        print_length(page), page.data(), metrics.node_count, metrics.frames, metrics.scripts, metrics.layout_count,
        metrics.last_layout_us, metrics.last_render_us, metrics.package_bytes, metrics.active_requests,
        internal_heap_free(), internal_heap_minimum_free(), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), platform.network_online() ? "online" : "offline",
        main_stack_minimum_free_bytes());
}

void print_ready(const mb::Browser& browser, const mb::EspIdfPlatform& platform) {
    const mb::Metrics& metrics = browser.metrics();
    const std::string_view page = browser.page_name();
    std::printf(
        "MICROBROWSER_QEMU_READY page=%.*s nodes=%" PRIu32 " frames=%" PRIu64
        " heap_free=%zu heap_min_free=%zu psram_total=%zu psram_free=%zu network=%s memory_source=emulated stack_min_free_bytes=%zu\n",
        print_length(page), page.data(), metrics.node_count, metrics.frames, internal_heap_free(),
        internal_heap_minimum_free(), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), platform.network_online() ? "online" : "offline", main_stack_minimum_free_bytes());
    std::printf("UART commands: call NAME | url URL | page | stats | selftest (maximum %zu bytes per line)\n", kMaximumCommandBytes);
    std::printf("MODE value=%.*s location=%.*s\n", print_length(browser.content_mode()), browser.content_mode().data(),
        print_length(browser.location()), browser.location().data());
    std::fflush(stdout);
}

bool render_once(mb::Browser& browser, QemuDisplay& display) {
    if (!browser.tick()) return false;
    display.present(browser);
    return true;
}

void selftest_invoke(mb::Browser& browser, QemuDisplay& display, std::string_view function,
    std::string_view expected_page = {}) {
    browser.dispatch(function);
    if (!browser.last_error().empty()) {
        throw std::runtime_error(std::string(function) + ": " + std::string(browser.last_error()));
    }
    if (!expected_page.empty() && browser.page_name() != expected_page) {
        throw std::runtime_error(std::string(function) + ": expected page " + std::string(expected_page));
    }
    render_once(browser, display);
}

void selftest_drag_to_bottom(mb::Browser& browser, QemuDisplay& display) {
    for (int drag = 0; drag < kUpwardDragsToBottom; ++drag) {
        browser.pointer_down(kScrollFrom.x, kScrollFrom.y);
        browser.pointer_move(kScrollTo.x, kScrollTo.y);
        browser.pointer_up(kScrollTo.x, kScrollTo.y);
        if (!render_once(browser, display)) throw std::runtime_error("drag scrolling did not paint a frame");
    }

    const std::size_t pixel_index = std::size_t(kSelftestButtonPixel.y) * std::size_t(kScreenSize.width) +
        std::size_t(kSelftestButtonPixel.x);
    const uint16_t actual = browser.pixels()[pixel_index];
    if (actual != mb::to_rgb565(mb::Color::rgb(0xe9, 0xed, 0xf5))) {
        throw std::runtime_error("drag scrolling did not expose the self-test button");
    }

    const uint64_t scripts = browser.metrics().scripts;
    browser.pointer_down(kSelftestButtonCenter.x, kSelftestButtonCenter.y);
    browser.pointer_up(kSelftestButtonCenter.x, kSelftestButtonCenter.y);
    if (browser.metrics().scripts != scripts + 1) {
        throw std::runtime_error("the visible self-test button did not accept a click");
    }
    if (!render_once(browser, display)) throw std::runtime_error("the self-test button did not repaint");
}

void verify_typed_nvs() {
    constexpr int64_t expected_integer = 9'007'199'254'740'993LL;
    const mb::ByteBuffer expected_bytes{0U, 255U, 0U, 128U};
    {
        mb::EspIdfPlatform writer;
        writer.save_value("selftest.i64", mb::Value::from_integer(expected_integer));
        writer.save_value("selftest.bytes", mb::Value::from_bytes(expected_bytes));
    }
    mb::EspIdfPlatform reader;
    const std::optional<mb::Value> integer = reader.load_value("selftest.i64");
    if (!integer || integer->type != mb::Value::Type::integer || integer->integer != expected_integer) {
        throw std::runtime_error("NVS did not reload the exact integer");
    }
    const std::optional<mb::Value> bytes = reader.load_value("selftest.bytes");
    if (!bytes || bytes->type != mb::Value::Type::bytes || !bytes->bytes || *bytes->bytes != expected_bytes) {
        throw std::runtime_error("NVS did not reload exact binary data");
    }
}

void run_selftest(mb::Browser& browser, QemuDisplay& display) {
    try {
        if (browser.page_name() != "dashboard") throw std::runtime_error("selftest must start on the dashboard page");
        if (!browser.last_error().empty()) throw std::runtime_error(std::string("existing browser error: ") + std::string(browser.last_error()));
        verify_typed_nvs();
        const uint64_t initial_frames = browser.metrics().frames;

        // Uses only existing dashboard functions with local effects: stop_timers,
        // open_details/go_back, refresh_sensors, open_settings, and NVS-only save_all.
        selftest_invoke(browser, display, "stop_timers");
        selftest_invoke(browser, display, "open_details", "details");
        selftest_invoke(browser, display, "refresh_sensors");
        selftest_invoke(browser, display, "go_back", "dashboard");
        selftest_invoke(browser, display, "open_settings", "settings");
        selftest_drag_to_bottom(browser, display);
        selftest_invoke(browser, display, "save_all");
        selftest_invoke(browser, display, "go_back", "dashboard");
        selftest_invoke(browser, display, "stop_timers");

        if (browser.metrics().frames <= initial_frames) throw std::runtime_error("selftest did not paint a new frame");
        const std::string_view page = browser.page_name();
        std::printf("SELFTEST PASS page=%.*s frames=%" PRIu64 " heap=%zu\n", print_length(page), page.data(),
            browser.metrics().frames, internal_heap_free());
    } catch (const std::exception& error) {
        std::printf("SELFTEST FAIL reason=%s\n", error.what());
    }
}

class UartCommands final {
public:
    UartCommands(mb::Browser& browser, QemuDisplay& display, mb::EspIdfPlatform& platform)
        : m_browser(browser), m_display(display), m_platform(platform) {
        if (!uart_is_driver_installed(UART_NUM_0)) {
            check_esp(uart_driver_install(UART_NUM_0, kUartReceiveBufferBytes, 0, 0, nullptr, 0), "installing UART0 command input");
        }
    }

    void poll() {
        std::size_t available = 0;
        check_esp(uart_get_buffered_data_len(UART_NUM_0, &available), "polling UART0 command input");
        const std::size_t length = std::min(available, kMaximumUartBytesPerPoll);
        if (length == 0) return;
        std::array<uint8_t, kMaximumUartBytesPerPoll> bytes{};
        const int received = uart_read_bytes(UART_NUM_0, bytes.data(), uint32_t(length), 0);
        if (received < 0) throw std::runtime_error("reading UART0 command input failed");
        for (int index = 0; index < received; ++index) consume(bytes[std::size_t(index)]);
    }

private:
    void consume(uint8_t byte) {
        if (m_skip_next_lf) {
            m_skip_next_lf = false;
            if (byte == '\n') return;
        }
        if (byte == '\r' || byte == '\n') {
            m_skip_next_lf = byte == '\r';
            if (m_discarding) {
                m_discarding = false;
                m_length = 0;
                return;
            }
            execute(std::string(m_line.data(), m_length));
            std::fflush(stdout);
            m_length = 0;
            return;
        }
        if (m_discarding) return;
        if (m_length == kMaximumCommandBytes) {
            m_discarding = true;
            m_length = 0;
            std::printf("COMMAND FAIL reason=line exceeds %zu bytes\n", kMaximumCommandBytes);
            std::fflush(stdout);
            return;
        }
        m_line[m_length++] = char(byte);
    }

    void execute(std::string line) {
        if (line == "page") {
            print_page(m_browser);
            return;
        }
        if (line == "stats") {
            print_stats(m_browser, m_platform);
            return;
        }
        if (line == "selftest") {
            run_selftest(m_browser, m_display);
            return;
        }
        constexpr std::string_view call_prefix = "call ";
        if (line.starts_with(call_prefix) && line.size() > call_prefix.size()) {
            const std::string_view function(line.data() + call_prefix.size(), line.size() - call_prefix.size());
            try {
                m_browser.dispatch(function);
                if (!m_browser.last_error().empty()) throw std::runtime_error(std::string(m_browser.last_error()));
                render_once(m_browser, m_display);
                std::printf("COMMAND OK call=%.*s\n", print_length(function), function.data());
            } catch (const std::exception& error) {
                std::printf("COMMAND FAIL call=%.*s reason=%s\n", print_length(function), function.data(), error.what());
            }
            return;
        }
        constexpr std::string_view url_prefix = "url ";
        if (line.starts_with(url_prefix) && line.size() > url_prefix.size()) {
            const std::string url(line.substr(url_prefix.size()));
            try {
                m_browser.open_url(url);
                if (!m_browser.last_error().empty()) throw std::runtime_error(std::string(m_browser.last_error()));
                render_once(m_browser, m_display);
                std::printf("URL QUEUED\n");
            } catch (const std::exception& error) {
                std::printf("COMMAND FAIL url reason=%s\n", error.what());
            }
            return;
        }
        if (line.empty()) {
            std::printf("COMMAND FAIL reason=empty command\n");
        } else if (line == "call " || line == "call") {
            std::printf("COMMAND FAIL reason=missing function name\n");
        } else {
            std::printf("COMMAND FAIL reason=unknown command\n");
        }
    }

    mb::Browser& m_browser;
    QemuDisplay& m_display;
    mb::EspIdfPlatform& m_platform;
    std::array<char, kMaximumCommandBytes> m_line{};
    std::size_t m_length = 0;
    bool m_discarding = false;
    bool m_skip_next_lf = false;
};

std::vector<uint8_t> embedded_package() {
    const auto begin = reinterpret_cast<std::uintptr_t>(application_mbp_start);
    const auto end = reinterpret_cast<std::uintptr_t>(application_mbp_end);
    if (end < begin) throw std::runtime_error("embedded application package has invalid bounds");
    if (end - begin > 16U * 1024U * 1024U) throw std::runtime_error("embedded application exceeds the package limit");
    return std::vector<uint8_t>(application_mbp_start, application_mbp_end);
}


// IDF's xTaskCreate takes a byte count (unlike vanilla FreeRTOS words). Keep
// these values in bytes to match the drivers' worker_stack_bytes contracts.
constexpr std::size_t kNativeWorkerStackBytes = 6144;
constexpr std::size_t kNativePendingOperations = 2;

std::span<const mb::EspGpioEndpointConfig> owner_gpio_endpoints() {
    [[maybe_unused]] static constexpr std::array<mb::EspGpioEndpointConfig, 2> endpoints{{
#ifdef CONFIG_MB_GPIO_STATUS_LED
        mb::EspGpioEndpointConfig{
            "status_led", static_cast<gpio_num_t>(CONFIG_MB_GPIO_STATUS_LED_PIN),
            mb::GpioDirection::output, mb::esp_gpio_output_operations},
#endif
#ifdef CONFIG_MB_GPIO_USER_BUTTON
        mb::EspGpioEndpointConfig{
            "user_button", static_cast<gpio_num_t>(CONFIG_MB_GPIO_USER_BUTTON_PIN),
            mb::GpioDirection::input, mb::esp_gpio_input_operations},
#endif
    }};
#if defined(CONFIG_MB_GPIO_STATUS_LED) && defined(CONFIG_MB_GPIO_USER_BUTTON)
    return endpoints;
#elif defined(CONFIG_MB_GPIO_STATUS_LED) || defined(CONFIG_MB_GPIO_USER_BUTTON)
    return std::span<const mb::EspGpioEndpointConfig>(endpoints).first(1);
#else
    return {};
#endif
}

#if defined(CONFIG_MB_GPIO_STATUS_LED) && defined(CONFIG_MB_GPIO_USER_BUTTON)
static_assert(CONFIG_MB_GPIO_STATUS_LED_PIN != CONFIG_MB_GPIO_USER_BUTTON_PIN,
    "status_led and user_button cannot share an ESP32-S3 GPIO");
#endif

std::span<const mb::EspSpiEndpointConfig> owner_spi_endpoints() {
#ifdef CONFIG_MB_SPI_SENSOR
    static const std::array<mb::EspSpiEndpointConfig, 1> endpoints{{
        mb::EspSpiEndpointConfig{
            .logical_name = "spi_sensor",
            .host = SPI2_HOST,
            .bus = {
                .mosi_io_num = CONFIG_MB_SPI_SENSOR_MOSI_PIN,
                .miso_io_num = CONFIG_MB_SPI_SENSOR_MISO_PIN,
                .sclk_io_num = CONFIG_MB_SPI_SENSOR_CLOCK_PIN,
                .quadwp_io_num = GPIO_NUM_NC,
                .quadhd_io_num = GPIO_NUM_NC,
                .data4_io_num = GPIO_NUM_NC,
                .data5_io_num = GPIO_NUM_NC,
                .data6_io_num = GPIO_NUM_NC,
                .data7_io_num = GPIO_NUM_NC,
                .data_io_default_level = false,
                .max_transfer_sz = 0,
                .dma_burst_size = 0,
                .flags = 0,
                .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
                .intr_flags = 0,
            },
            .chip_select = static_cast<gpio_num_t>(CONFIG_MB_SPI_SENSOR_CS_PIN),
            .dma_channel = SPI_DMA_CH_AUTO,
            .maximum_frequency_hz = 1'000'000,
            .allowed_modes = 0x01,
            .supported_lanes = mb::EspSpiLaneSupport::single,
            .maximum_transfer_bytes = 1024,
            .maximum_pending_operations = kNativePendingOperations,
            .io_timeout_ms = 1000,
            .worker_stack_bytes = kNativeWorkerStackBytes,
        },
    }};
    return endpoints;
#else
    return {};
#endif
}

std::span<const mb::EspI2cEndpointConfig> owner_i2c_endpoints() {
#ifdef CONFIG_MB_I2C_SENSOR_BUS
    static constexpr std::array<mb::EspI2cAddressRange, 1> allowed_addresses{{
        mb::EspI2cAddressRange{
            .first_address = CONFIG_MB_I2C_SENSOR_FIRST_ADDRESS,
            .last_address = CONFIG_MB_I2C_SENSOR_LAST_ADDRESS,
            .ten_bit = false,
        },
    }};
    static const std::array<mb::EspI2cEndpointConfig, 1> endpoints{{
        mb::EspI2cEndpointConfig{
            .logical_name = "sensor_bus",
            .port = I2C_NUM_0,
            .sda_pin = static_cast<gpio_num_t>(CONFIG_MB_I2C_SENSOR_SDA_PIN),
            .scl_pin = static_cast<gpio_num_t>(CONFIG_MB_I2C_SENSOR_SCL_PIN),
            .enable_internal_pullup = false,
            .minimum_frequency_hz = 100'000,
            .maximum_frequency_hz = 100'000,
            .scl_wait_us = 2000,
            .allowed_addresses = allowed_addresses,
            .maximum_transfer_bytes = 1024,
            .maximum_pending_operations = kNativePendingOperations,
            .io_timeout_ms = 1000,
            .worker_stack_bytes = kNativeWorkerStackBytes,
        },
    }};
    return endpoints;
#else
    return {};
#endif
}

std::span<const mb::EspI2sEndpointConfig> owner_i2s_endpoints() {
#ifdef CONFIG_MB_I2S_AUDIO_OUTPUT
    static constexpr std::array<mb::PcmSampleFormat, 3> allowed_formats{
        mb::PcmSampleFormat::signed_16_le,
        mb::PcmSampleFormat::signed_24_le,
        mb::PcmSampleFormat::signed_32_le,
    };
    static const std::array<mb::EspI2sEndpointConfig, 1> endpoints{{
        mb::EspI2sEndpointConfig{
            .logical_name = "audio_output",
            .role = mb::EspPcmRole::transmit,
            .full_duplex = false,
            .standard_gpio = {
                .mclk = GPIO_NUM_NC,
                .bclk = static_cast<gpio_num_t>(CONFIG_MB_I2S_AUDIO_BCLK_PIN),
                .ws = static_cast<gpio_num_t>(CONFIG_MB_I2S_AUDIO_WS_PIN),
                .dout = static_cast<gpio_num_t>(CONFIG_MB_I2S_AUDIO_DOUT_PIN),
                .din = GPIO_NUM_NC,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
            .allowed_formats = allowed_formats,
            .maximum_pending_operations = kNativePendingOperations,
            .io_timeout_ms = 1000,
            .worker_stack_bytes = kNativeWorkerStackBytes,
            .worker_priority = 5,
            .event_logical_name = {},
            .event = mb::EspI2sDmaEvent::sent,
        },
    }};
    return endpoints;
#else
    return {};
#endif

}
std::span<const mb::EspBleCentralEndpointConfig> owner_ble_central_endpoints() {
#if defined(CONFIG_MB_BLE_CENTRAL)
    static constexpr std::array<mb::EspBleCentralEndpointConfig, 1> endpoints{{
        mb::EspBleCentralEndpointConfig{
            "ble_central", CONFIG_MB_BLE_SCAN_TIMEOUT_MS, 30'000},
    }};
    return endpoints;
#else
    return {};
#endif
}

class OwnerPinReservations final {
public:
    void add(int pin, std::string_view signal) {
        if (pin == GPIO_NUM_NC) return;
        for (std::size_t index = 0; index < m_count; ++index) {
            if (m_pins[index].pin != pin) continue;
            throw std::invalid_argument(
                "owner GPIO conflict: " + std::string(signal) + " and " +
                std::string(m_pins[index].signal) + " both reserve GPIO" + std::to_string(pin));
        }
        if (m_count == m_pins.size()) throw std::runtime_error("owner GPIO reservation capacity exceeded");
        m_pins[m_count++] = Reservation{pin, signal};
    }

private:
    struct Reservation {
        int pin;
        std::string_view signal;
    };
    std::array<Reservation, 16> m_pins{};
    std::size_t m_count = 0;
};

void reserve_spi_pins(OwnerPinReservations& pins, const mb::EspSpiEndpointConfig& config) {
    pins.add(config.bus.mosi_io_num, "spi_sensor.mosi");
    pins.add(config.bus.miso_io_num, "spi_sensor.miso");
    pins.add(config.bus.sclk_io_num, "spi_sensor.clock");
    pins.add(config.bus.quadwp_io_num, "spi_sensor.quadwp");
    pins.add(config.bus.quadhd_io_num, "spi_sensor.quadhd");
    pins.add(config.chip_select, "spi_sensor.chip_select");
}

void reserve_i2s_pins(OwnerPinReservations& pins, const mb::EspI2sEndpointConfig& config) {
    pins.add(config.standard_gpio.mclk, "audio_output.mclk");
    pins.add(config.standard_gpio.bclk, "audio_output.bclk");
    pins.add(config.standard_gpio.ws, "audio_output.ws");
    pins.add(config.standard_gpio.dout, "audio_output.dout");
    pins.add(config.standard_gpio.din, "audio_output.din");
    pins.add(config.pdm_clock_pin, "audio_output.pdm_clock");
    pins.add(config.pdm_data_pin, "audio_output.pdm_data");
}

void validate_owner_profile(
    std::span<const mb::EspGpioEndpointConfig> gpio,
    std::span<const mb::EspSpiEndpointConfig> spi,
    std::span<const mb::EspI2cEndpointConfig> i2c,
    std::span<const mb::EspI2sEndpointConfig> i2s) {
    for (const auto& config : spi) mb::validate_esp_idf_spi_config(config);
    for (const auto& config : i2c) mb::validate_esp_idf_i2c_config(config);
    for (const auto& config : i2s) mb::validate_esp_idf_i2s_config(config);

    OwnerPinReservations pins;
    for (const auto& config : gpio) pins.add(config.pin, config.logical_name);
    for (const auto& config : spi) reserve_spi_pins(pins, config);
    for (const auto& config : i2c) {
        pins.add(config.sda_pin, "sensor_bus.sda");
        pins.add(config.scl_pin, "sensor_bus.scl");
    }
    for (const auto& config : i2s) reserve_i2s_pins(pins, config);
}

[[noreturn]] void run() {
    const auto gpio_configs = owner_gpio_endpoints();
    const auto spi_configs = owner_spi_endpoints();
    const auto i2c_configs = owner_i2c_endpoints();
    const auto i2s_configs = owner_i2s_endpoints();
    validate_owner_profile(gpio_configs, spi_configs, i2c_configs, i2s_configs);
#if defined(CONFIG_MB_BLE_CENTRAL)
    const auto ble_central_configs = owner_ble_central_endpoints();
#endif

    ESP_LOGI(kLogTag, "initializing platform");
    mb::EspIdfPlatform platform;
    QemuDisplay display;
    mb::PeripheralRegistry peripherals;
#if defined(CONFIG_MB_BLE_CENTRAL)
    mb::EspIdfBleCentralRegistry ble_central_registry(peripherals, ble_central_configs);
#endif
    mb::EspIdfGpioRegistry gpio_registry(peripherals, gpio_configs);
    mb::EspIdfSpiRegistry spi_registry(peripherals, spi_configs);
    mb::EspIdfI2cRegistry i2c_registry(peripherals, i2c_configs);
    mb::EspIdfI2sRegistry i2s_registry(peripherals, i2s_configs);
    ESP_LOGI(kLogTag, "initializing the RGB565 framebuffer");
    mb::Browser browser(platform, kScreenSize, CONFIG_MB_GATEWAY_URL, &peripherals);
    platform.start_network();
    browser.load(embedded_package());
    std::printf("MICROBROWSER_PACKAGE source=embedded payload_bytes=%" PRIu64
        " package_digest_sha256=%s format=v3.0\n",
        browser.metrics().package_bytes, MB_EMBEDDED_PACKAGE_SHA256);
    if (std::string_view(CONFIG_MB_PACKAGE_URL).size() != 0) {
        browser.open_url(CONFIG_MB_PACKAGE_URL);
        browser.confirm_native_package();
    }
    UartCommands commands(browser, display, platform);

    if (!render_once(browser, display)) throw std::runtime_error("initial dashboard did not produce a framebuffer");
    print_ready(browser, platform);

    TickType_t previous_wake = xTaskGetTickCount();
    for (;;) {
        commands.poll();
        render_once(browser, display);
        vTaskDelayUntil(&previous_wake, pdMS_TO_TICKS(kApplicationPeriodMs));
    }
}

} // namespace

extern "C" void app_main() {
    ESP_LOGI(kLogTag, "app_main entered");
    try {
        run();
    } catch (const std::exception& error) {
        ESP_LOGE(kLogTag, "fatal Microbrowser startup or runtime error: %s", error.what());
        std::abort();
    }
}
